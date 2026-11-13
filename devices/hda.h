/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HV_HDA_H
#define HV_HDA_H

#include <stdatomic.h>
#include <stdint.h>

#include "machine.h"
#include "pcie.h"

/* Intel ICH6 High Definition Audio controller (8086:2668, PCI\CC_0403) +
 * Full-Duplex HDA Codec (NID 2/3 Speakers Out + NID 4/5 Microphone In).
 * Bound natively out-of-the-box by Windows 11 ARM64 inbox hdaudbus.sys + HdAudio.sys. */

#define HV_HDA_ISS 4
#define HV_HDA_OSS 4
#define HV_HDA_NSTREAMS (HV_HDA_ISS + HV_HDA_OSS)
#define HV_HDA_RING_BYTES (1u << 18) /* 256 KiB (~1.36s @ 48 kHz stereo S16LE) */

typedef struct HvHdaBackend {
    int (*start_out)(void *opaque, unsigned channels, unsigned rate_hz);
    void (*stop_out)(void *opaque);
    int (*start_in)(void *opaque, unsigned channels, unsigned rate_hz);
    void (*stop_in)(void *opaque);
    void *opaque;
} HvHdaBackend;

typedef struct HvHdaStream {
    uint32_t ctl;       /* 24-bit SDnCTL */
    uint8_t sts;        /* 8-bit SDnSTS */
    uint32_t lpib;      /* SDnLPIB */
    uint32_t cbl;       /* SDnCBL */
    uint16_t lvi;       /* SDnLVI */
    uint16_t fifos;     /* SDnFIFOS */
    uint16_t fmt;       /* SDnFMT */
    uint32_t bdpl;      /* SDnBDPL */
    uint32_t bdpu;      /* SDnBDPU */

    int running;
    uint64_t last_ns;
    uint64_t rem_num;   /* fractional byte accumulator (bytes * 1e9) */
} HvHdaStream;

typedef struct HvHda {
    HvMachine *m;
    HvPciDev pci;
    HvHdaBackend backend;

    uint64_t base_ns;

    /* Global HDA controller registers */
    uint32_t gctl;
    uint16_t wakeen;
    uint16_t statests;
    uint16_t gsts;
    uint32_t intctl;
    uint32_t ssync;

    /* CORB */
    uint32_t corblbase;
    uint32_t corbubase;
    uint16_t corbwp;
    uint16_t corbrp;
    uint8_t corbctl;
    uint8_t corbsts;
    uint8_t corbsize;

    /* RIRB */
    uint32_t rirblbase;
    uint32_t rirbubase;
    uint16_t rirbwp;
    uint16_t rintcnt;
    uint8_t rirbctl;
    uint8_t rirbsts;
    uint8_t rirbsize;
    uint16_t rirb_cnt;

    /* Immediate Command Interface */
    uint32_t icoi;
    uint32_t icii;
    uint16_t icis;

    /* DMA Position Buffer */
    uint32_t dplbase;
    uint32_t dpubase;

    HvHdaStream st[HV_HDA_NSTREAMS];

    /* Codec 0 (Duplex AFG) state */
    uint32_t subsys_id;
    uint8_t afg_power;
    /* NID 2: DAC */
    uint16_t dac_fmt;
    uint8_t dac_stream_chan;
    uint8_t dac_power;
    uint8_t dac_amp_l, dac_amp_r; /* bit 7 = mute, 6:0 = gain (0x4a = 0 dB) */
    /* NID 3: Speaker Pin */
    uint8_t spk_pin_ctl;
    uint8_t spk_unsol;
    uint8_t spk_eapd;
    uint32_t spk_cfg;
    /* NID 4: ADC */
    uint16_t adc_fmt;
    uint8_t adc_stream_chan;
    uint8_t adc_power;
    uint8_t adc_amp_l, adc_amp_r; /* bit 7 = mute, 6:0 = gain (0x32 = 0 dB) */
    uint8_t adc_conn_sel;
    /* NID 5: Mic Pin */
    uint8_t mic_pin_ctl;
    uint8_t mic_unsol;
    uint32_t mic_cfg;

    /* Lock-free SPSC PCM rings (always 48 kHz or active stream rate S16LE stereo) */
    uint8_t *tx_ring; /* Guest Speaker -> Host CoreAudio Output */
    _Atomic uint64_t tx_written;
    _Atomic uint64_t tx_consumed;
    _Atomic uint32_t out_rate_hz;
    _Atomic uint32_t out_channels;

    uint8_t *rx_ring; /* Host CoreAudio Mic -> Guest Microphone */
    _Atomic uint64_t rx_written;
    _Atomic uint64_t rx_consumed;
    _Atomic uint32_t in_rate_hz;
    _Atomic uint32_t in_channels;

    uint64_t tx_frames_total;
    uint64_t rx_frames_total;
} HvHda;

HvHda *hv_hda_create(HvMachine *m, HvPcie *bus);
void hv_hda_poll(HvHda *d);
/* Machine reset: as the guest clearing GCTL.CRST. Call with the device lock held. */
void hv_hda_reset(HvHda *d);

/* Host CoreAudio Output render callback: pulls S16LE interleaved stereo/mono at stream rate. */
uint32_t hv_hda_pull_playback(HvHda *d, void *dst, uint32_t bytes);

/* Host CoreAudio Input capture callback: pushes S16LE interleaved stereo at stream rate. */
void hv_hda_push_capture(HvHda *d, const void *src, uint32_t bytes);

#endif
