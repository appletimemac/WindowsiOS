/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Full-duplex host audio backend for macOS and iOS:
 *   - Playback (Speaker Out): low-latency RemoteIO (iOS) / DefaultOutput (macOS) AudioUnit
 *     pulling S16LE frames straight from HvHda's lock-free TX ring.
 *   - Capture (Microphone In): CoreAudio AudioQueueInput with automatic hardware sample-rate
 *     and mono-to-stereo conversion into HvHda's lock-free RX ring. */
#include "audio_mac.h"

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#if TARGET_OS_IPHONE
#define HV_OUTPUT_UNIT kAudioUnitSubType_RemoteIO
#else
#define HV_OUTPUT_UNIT kAudioUnitSubType_DefaultOutput
#endif

#define IN_NUM_BUFFERS 4
#define IN_BUFFER_FRAMES 960 /* 20 ms @ 48 kHz */

static pthread_mutex_t audio_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---- HDA Playback (Output AudioUnit) ---- */
static AudioComponentInstance hda_out_unit;
static int hda_out_running;
static unsigned hda_out_ch, hda_out_rate;

static OSStatus hda_out_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                               UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)flags, (void)ts, (void)bus, (void)frames;
    HvHda *hda = (HvHda *)ref;
    for (UInt32 i = 0; i < io->mNumberBuffers; i++)
        hv_hda_pull_playback(hda, io->mBuffers[i].mData, io->mBuffers[i].mDataByteSize);
    return noErr;
}

static int hda_start_out(void *opaque, unsigned channels, unsigned rate_hz) {
    HvHda *hda = (HvHda *)opaque;
    if (channels < 1 || channels > 2) channels = 2;
    if (rate_hz < 8000 || rate_hz > 192000) rate_hz = 48000;

    pthread_mutex_lock(&audio_mu);
    if (hda_out_unit && (channels != hda_out_ch || rate_hz != hda_out_rate)) {
        AudioOutputUnitStop(hda_out_unit);
        AudioUnitUninitialize(hda_out_unit);
        AudioComponentInstanceDispose(hda_out_unit);
        hda_out_unit = NULL;
        hda_out_running = 0;
    }
    if (!hda_out_unit) {
        AudioComponentDescription desc = {
            .componentType = kAudioUnitType_Output,
            .componentSubType = HV_OUTPUT_UNIT,
            .componentManufacturer = kAudioUnitManufacturer_Apple,
        };
        AudioComponent comp = AudioComponentFindNext(NULL, &desc);
        if (!comp || AudioComponentInstanceNew(comp, &hda_out_unit) != noErr) {
            pthread_mutex_unlock(&audio_mu);
            return -1;
        }
        AudioStreamBasicDescription f = {0};
        f.mSampleRate = (Float64)rate_hz;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        f.mBytesPerPacket = f.mBytesPerFrame = 2u * channels;
        f.mFramesPerPacket = 1;
        f.mChannelsPerFrame = channels;
        f.mBitsPerChannel = 16;

        AURenderCallbackStruct cb = {hda_out_render, hda};
        if (AudioUnitSetProperty(hda_out_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr ||
            AudioUnitSetProperty(hda_out_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
            AudioUnitInitialize(hda_out_unit) != noErr) {
            fprintf(stderr, "winvm: audio: output unit refused %u ch @ %u Hz\n", channels, rate_hz);
            AudioComponentInstanceDispose(hda_out_unit);
            hda_out_unit = NULL;
            pthread_mutex_unlock(&audio_mu);
            return -1;
        }
        hda_out_ch = channels;
        hda_out_rate = rate_hz;
    }
    if (!hda_out_running && AudioOutputUnitStart(hda_out_unit) == noErr)
        hda_out_running = 1;
    int ok = hda_out_running ? 0 : -1;
    pthread_mutex_unlock(&audio_mu);
    return ok;
}

static void hda_stop_out(void *opaque) {
    (void)opaque;
    pthread_mutex_lock(&audio_mu);
    if (hda_out_unit && hda_out_running) {
        AudioOutputUnitStop(hda_out_unit);
        hda_out_running = 0;
    }
    pthread_mutex_unlock(&audio_mu);
}

/* ---- HDA Capture (Microphone AudioQueueInput) ---- */
static AudioQueueRef hda_in_queue;
static AudioQueueBufferRef hda_in_bufs[IN_NUM_BUFFERS];
static int hda_in_running;
static unsigned hda_in_ch, hda_in_rate;

static void hda_in_callback(void *opaque, AudioQueueRef aq, AudioQueueBufferRef buf,
                            const AudioTimeStamp *start_time, UInt32 num_packets,
                            const AudioStreamPacketDescription *pkt_desc) {
    (void)start_time, (void)num_packets, (void)pkt_desc;
    HvHda *hda = (HvHda *)opaque;
    if (hda && buf && buf->mAudioDataByteSize > 0)
        hv_hda_push_capture(hda, buf->mAudioData, buf->mAudioDataByteSize);

    pthread_mutex_lock(&audio_mu);
    if (hda_in_running && aq == hda_in_queue)
        AudioQueueEnqueueBuffer(aq, buf, 0, NULL);
    pthread_mutex_unlock(&audio_mu);
}

/* The microphone queue is stopped and disposed of without audio_mu: its callback takes that lock, and a stop
   that waits for a callback in flight would wait for ever with it held. in_life_mu, which the callback never takes,
   keeps the queue's starts, stops and disposal one at a time instead (a CPU thread stops it while another thread
   shuts audio down). It is taken before audio_mu. */
static pthread_mutex_t in_life_mu = PTHREAD_MUTEX_INITIALIZER;

static AudioQueueRef take_in_locked(void) {
    AudioQueueRef q = hda_in_queue;
    hda_in_queue = NULL;
    hda_in_running = 0;
    memset(hda_in_bufs, 0, sizeof hda_in_bufs);
    return q;
}

static void end_in(AudioQueueRef q) {
    if (!q) return;
    AudioQueueStop(q, true);
    AudioQueueDispose(q, true);
}

static int hda_start_in(void *opaque, unsigned channels, unsigned rate_hz) {
    HvHda *hda = (HvHda *)opaque;
    if (channels < 1 || channels > 2) channels = 2;
    if (rate_hz < 8000 || rate_hz > 192000) rate_hz = 48000;

    pthread_mutex_lock(&in_life_mu);
    pthread_mutex_lock(&audio_mu);
    AudioQueueRef other = hda_in_queue && (channels != hda_in_ch || rate_hz != hda_in_rate) ? take_in_locked() : NULL;
    pthread_mutex_unlock(&audio_mu);
    end_in(other);

    pthread_mutex_lock(&audio_mu);
    if (!hda_in_queue) {
        AudioStreamBasicDescription f = {0};
        f.mSampleRate = (Float64)rate_hz;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        f.mBytesPerPacket = f.mBytesPerFrame = 2u * channels;
        f.mFramesPerPacket = 1;
        f.mChannelsPerFrame = channels;
        f.mBitsPerChannel = 16;

        OSStatus st = AudioQueueNewInput(&f, hda_in_callback, hda, NULL, NULL, 0, &hda_in_queue);
        if (st != noErr || !hda_in_queue) {
            fprintf(stderr, "winvm: audio: microphone input unavailable (OSStatus=%d)\n", (int)st);
            hda_in_queue = NULL;
            pthread_mutex_unlock(&audio_mu);
            pthread_mutex_unlock(&in_life_mu);
            return -1;
        }
        UInt32 buf_bytes = IN_BUFFER_FRAMES * f.mBytesPerFrame;
        for (int i = 0; i < IN_NUM_BUFFERS; i++) {
            if (AudioQueueAllocateBuffer(hda_in_queue, buf_bytes, &hda_in_bufs[i]) == noErr)
                AudioQueueEnqueueBuffer(hda_in_queue, hda_in_bufs[i], 0, NULL);
        }
        hda_in_ch = channels;
        hda_in_rate = rate_hz;
    }

    AudioQueueRef refused = NULL;
    if (!hda_in_running) {
        OSStatus st = AudioQueueStart(hda_in_queue, NULL);
        if (st == noErr) {
            hda_in_running = 1;
        } else {
            fprintf(stderr, "winvm: audio: AudioQueueStart(input) returned %d (mic permission pending?)\n", (int)st);
            refused = take_in_locked();
        }
    }
    int ok = hda_in_running ? 0 : -1;
    pthread_mutex_unlock(&audio_mu);
    end_in(refused);
    pthread_mutex_unlock(&in_life_mu);
    return ok;
}

static void hda_stop_in(void *opaque) {
    (void)opaque;
    pthread_mutex_lock(&in_life_mu);
    pthread_mutex_lock(&audio_mu);
    AudioQueueRef q = hda_in_queue && hda_in_running ? hda_in_queue : NULL;
    hda_in_running = 0; /* its callback hands no buffer back from here on */
    pthread_mutex_unlock(&audio_mu);
    if (q) {
        AudioQueueStop(q, true); /* without audio_mu: see in_life_mu */
        pthread_mutex_lock(&audio_mu);
        /* Re-prime buffers so next start is instant */
        for (int i = 0; i < IN_NUM_BUFFERS; i++) {
            if (hda_in_bufs[i]) AudioQueueEnqueueBuffer(q, hda_in_bufs[i], 0, NULL);
        }
        pthread_mutex_unlock(&audio_mu);
    }
    pthread_mutex_unlock(&in_life_mu);
}

void hv_audio_attach_hda(HvHda *hda) {
    if (!hda) return;
    hda->backend = (HvHdaBackend){
        .start_out = hda_start_out,
        .stop_out = hda_stop_out,
        .start_in = hda_start_in,
        .stop_in = hda_stop_in,
        .opaque = hda,
    };
}

void hv_audio_shutdown(void) {
    pthread_mutex_lock(&audio_mu);
    if (hda_out_unit) {
        if (hda_out_running) AudioOutputUnitStop(hda_out_unit);
        AudioUnitUninitialize(hda_out_unit);
        AudioComponentInstanceDispose(hda_out_unit);
        hda_out_unit = NULL;
        hda_out_running = 0;
    }
    pthread_mutex_unlock(&audio_mu);
    pthread_mutex_lock(&in_life_mu);
    pthread_mutex_lock(&audio_mu);
    AudioQueueRef in = take_in_locked();
    pthread_mutex_unlock(&audio_mu);
    end_in(in);
    pthread_mutex_unlock(&in_life_mu);
}

/* ---- Legacy virtio-snd compatibility ---- */
static AudioComponentInstance vsnd_unit;
static int vsnd_running;
static unsigned vsnd_ch, vsnd_fmt, vsnd_rate;

static OSStatus vsnd_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                            UInt32 frames, AudioBufferList *io) {
    (void)flags, (void)ts, (void)bus, (void)frames;
    HvVirtioSnd *d = ref;
    for (UInt32 i = 0; i < io->mNumberBuffers; i++)
        hv_virtio_snd_pull(d, io->mBuffers[i].mData, io->mBuffers[i].mDataByteSize);
    return noErr;
}

static int vsnd_start(void *opaque, unsigned channels, unsigned format, unsigned rate_hz) {
    HvVirtioSnd *d = opaque;
    if (vsnd_unit && (channels != vsnd_ch || format != vsnd_fmt || rate_hz != vsnd_rate)) {
        AudioOutputUnitStop(vsnd_unit);
        AudioUnitUninitialize(vsnd_unit);
        AudioComponentInstanceDispose(vsnd_unit);
        vsnd_unit = NULL;
        vsnd_running = 0;
    }
    if (!vsnd_unit) {
        AudioComponentDescription desc = {kAudioUnitType_Output, HV_OUTPUT_UNIT,
                                          kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent comp = AudioComponentFindNext(NULL, &desc);
        if (!comp || AudioComponentInstanceNew(comp, &vsnd_unit) != noErr) return -1;
        unsigned bytes = format == HV_VSND_FMT_S16 ? 2 : 4;
        AudioStreamBasicDescription f = {0};
        f.mSampleRate = rate_hz;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = (format == HV_VSND_FMT_FLOAT ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) |
                         kAudioFormatFlagIsPacked;
        f.mBytesPerPacket = f.mBytesPerFrame = bytes * channels;
        f.mFramesPerPacket = 1;
        f.mChannelsPerFrame = channels;
        f.mBitsPerChannel = bytes * 8;
        AURenderCallbackStruct cb = {vsnd_render, d};
        if (AudioUnitSetProperty(vsnd_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr ||
            AudioUnitSetProperty(vsnd_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
            AudioUnitInitialize(vsnd_unit) != noErr) {
            AudioComponentInstanceDispose(vsnd_unit);
            vsnd_unit = NULL;
            return -1;
        }
        vsnd_ch = channels, vsnd_fmt = format, vsnd_rate = rate_hz;
    }
    if (!vsnd_running && AudioOutputUnitStart(vsnd_unit) == noErr) vsnd_running = 1;
    return vsnd_running ? 0 : -1;
}

static void vsnd_stop(void *opaque) {
    (void)opaque;
    if (vsnd_unit && vsnd_running) AudioOutputUnitStop(vsnd_unit);
    vsnd_running = 0;
}

void hv_audio_attach(HvVirtioSnd *d) {
    d->backend = (HvVirtioSndBackend){vsnd_start, vsnd_stop, d};
}
