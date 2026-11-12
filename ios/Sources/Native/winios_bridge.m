/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "winios_bridge.h"

#include "hvsparse.h"
#include "share.h"
#include "winios_jit.h"
#include "winvm.h"

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <sys/stat.h>

static WinIOSBridge *s_shared_bridge = nil;

@interface WinIOSBridge () {
    BOOL _vmRunning;
    BOOL _unpacking;
    double _unpackProgress;
    NSString *_vmStatus;
    dispatch_queue_t _diagQueue;
    dispatch_source_t _diagTimer;
    CFAbsoluteTime _bootStartTime;
    uint64_t _configuredRamMB;
    int _configuredCPUs;
    CAMetalLayer *__weak _pendingLayer;
    uint64_t _diagTickCount;
    uint64_t _lastMachTicks;
    uint64_t _lastWfiTicks;
    double _lastWfiIdlePct;

    os_unfair_lock _metricsLock;
    CGSize _screenSizePts;
    CGFloat _screenScale;
    NSInteger _hingeStatus;
    double _hingeAngleDegrees;
}
- (void)handleVMExitWithReason:(NSString *)reason;
- (void)updateUnpackProgress:(double)frac;
- (void)startDiagTimer;
- (void)stopDiagTimer;
@end

static void vm_exit_trampoline(int reason) {
    NSString *r = [NSString stringWithUTF8String:hv_winvm_reason_name(reason)];
    dispatch_async(dispatch_get_main_queue(), ^{
        [s_shared_bridge handleVMExitWithReason:r];
    });
}

typedef struct UnpackCtx {
    __unsafe_unretained WinIOSBridge *bridge;
    CFAbsoluteTime lastUpdate;
} UnpackCtx;

static void unpack_progress_cb(uint64_t done, uint64_t total, void *opaque) {
    UnpackCtx *ctx = (UnpackCtx *)opaque;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (done < total && now - ctx->lastUpdate < 0.2) return;
    ctx->lastUpdate = now;
    double frac = total ? (double)done / (double)total : 0.0;
    WinIOSBridge *b = ctx->bridge;
    dispatch_async(dispatch_get_main_queue(), ^{
        [b updateUnpackProgress:frac];
    });
}

@implementation WinIOSBridge

+ (WinIOSBridge *)shared {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        s_shared_bridge = [[WinIOSBridge alloc] init];
    });
    return s_shared_bridge;
}

- (instancetype)init {
    if ((self = [super init])) {
        _vmStatus = @"Idle";
        _metricsLock = OS_UNFAIR_LOCK_INIT;
        _screenSizePts = CGSizeZero;
        _screenScale = 3.0;
        _hingeStatus = 0;
        _hingeAngleDegrees = -1.0;
        NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
        BOOL safariDefault = ([ud objectForKey:@"winios.safariOAuthV2"] == nil) ? YES : [ud boolForKey:@"winios.safariOAuthV2"];
        hv_share_set_url_handler_enabled(safariDefault ? 1 : 0);
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
        _diagQueue = dispatch_queue_create("me.ammaar.windowsios.diag", attr);
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [nc addObserver:self selector:@selector(onAppBackgroundOrTerminate:) name:UIApplicationDidEnterBackgroundNotification object:nil];
        [nc addObserver:self selector:@selector(onAppBackgroundOrTerminate:) name:UIApplicationWillTerminateNotification object:nil];
        [nc addObserver:self selector:@selector(onAudioInterruption:) name:AVAudioSessionInterruptionNotification object:nil];
    }
    return self;
}

- (void)configureAudioSessionAndMicPermission {
    AVAudioSession *session = [AVAudioSession sharedInstance];
    AVAudioSessionCategoryOptions opts = AVAudioSessionCategoryOptionDefaultToSpeaker |
                                         AVAudioSessionCategoryOptionAllowBluetoothHFP |
                                         AVAudioSessionCategoryOptionAllowBluetoothA2DP |
                                         AVAudioSessionCategoryOptionMixWithOthers;
    NSError *err = nil;
    if (![session setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeDefault options:opts error:&err]) {
        /* Fallback to Playback if PlayAndRecord temporarily rejected before permission */
        [session setCategory:AVAudioSessionCategoryPlayback withOptions:AVAudioSessionCategoryOptionMixWithOthers error:nil];
    }
    [session setPreferredSampleRate:48000.0 error:nil];
    [session setPreferredIOBufferDuration:0.010 error:nil];
    [session setActive:YES error:nil];

    if (@available(iOS 17.0, *)) {
        [AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL granted) {
            if (granted) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    AVAudioSession *s = [AVAudioSession sharedInstance];
                    [s setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeDefault options:opts error:nil];
                    [s setActive:YES error:nil];
                });
            }
        }];
    }
}

- (void)onAudioInterruption:(NSNotification *)note {
    NSNumber *typeNum = note.userInfo[AVAudioSessionInterruptionTypeKey];
    if (typeNum && typeNum.unsignedIntegerValue == AVAudioSessionInterruptionTypeEnded) {
        [[AVAudioSession sharedInstance] setActive:YES error:nil];
    }
}

- (void)onAppBackgroundOrTerminate:(NSNotification *)note {
    (void)note;
    if (_vmRunning) {
        dispatch_async(_diagQueue, ^{
            hv_winvm_flush_disks();
        });
    }
}

- (void)notifyChanged {
    if ([NSThread isMainThread]) {
        if (self.onStateChanged) self.onStateChanged();
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self.onStateChanged) self.onStateChanged();
        });
    }
}

- (void)updateScreenSize:(CGSize)size scale:(CGFloat)scale {
    os_unfair_lock_lock(&_metricsLock);
    _screenSizePts = size;
    if (scale > 0.0) _screenScale = scale;
    os_unfair_lock_unlock(&_metricsLock);
}

- (void)updateHingeStatus:(NSInteger)status angleDegrees:(double)angleDegrees {
    os_unfair_lock_lock(&_metricsLock);
    _hingeStatus = status;
    _hingeAngleDegrees = angleDegrees;
    os_unfair_lock_unlock(&_metricsLock);
}

- (void)installTrapGuard {
    winios_jit_install_trap_handler();
    [[UIApplication sharedApplication] setIdleTimerDisabled:YES];
    [self configureAudioSessionAndMicPermission];
    [self notifyChanged];
}

- (BOOL)tryAcquireJIT:(BOOL)detachAfter {
    BOOL ok = winios_jit_acquire(WINIOS_JIT_DEFAULT_BYTES, detachAfter);
    dispatch_async(_diagQueue, ^{
        [self writeDiagnosticsSnapshot];
    });
    [self notifyChanged];
    return ok;
}

- (BOOL)isJITLive {
    return winios_jit_is_live();
}

- (BOOL)isDebuggerAttached {
    return winios_jit_is_debugger_attached();
}

- (BOOL)isVMRunning {
    return _vmRunning;
}

- (BOOL)isUnpacking {
    return _unpacking;
}

- (double)unpackProgress {
    return _unpackProgress;
}

- (NSString *)jitStatusText {
    return [NSString stringWithUTF8String:winios_jit_status_text()];
}

- (NSString *)vmStatusText {
    return _vmStatus;
}

- (uint32_t)framebufferWidth {
    uint32_t w = 0, h = 0;
    if (_vmRunning && hv_winvm_display_size(&w, &h) == 0) return w;
    return 0;
}

- (uint32_t)framebufferHeight {
    uint32_t w = 0, h = 0;
    if (_vmRunning && hv_winvm_display_size(&w, &h) == 0) return h;
    return 0;
}

- (uint64_t)presentedFrames {
    return _vmRunning ? hv_winvm_display_frames() : 0;
}

- (uint64_t)availableMemoryMB {
    return (uint64_t)(winios_available_memory_bytes() >> 20);
}

- (uint64_t)guestRAMMB {
    if (_configuredRamMB) return _configuredRamMB;
    uint64_t bytes = 0;
    winios_jit_guest_ram(&bytes);
    return bytes >> 20;
}

- (BOOL)usbKeyboardReady {
    if (!_vmRunning) return NO;
    int kr = 0;
    hv_winvm_usb_stats(&kr, NULL, NULL);
    return kr != 0;
}

- (BOOL)usbTabletReady {
    if (!_vmRunning) return NO;
    int tr = 0;
    hv_winvm_usb_stats(NULL, &tr, NULL);
    return tr != 0;
}

- (uint64_t)usbEventsDelivered {
    if (!_vmRunning) return 0;
    uint64_t d = 0;
    hv_winvm_usb_stats(NULL, NULL, &d);
    return d;
}

- (NSString *)documentsPath {
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject ?: NSTemporaryDirectory();
}

- (NSString *)machineDirectory {
    NSString *docs = [self documentsPath];
    NSString *win11Dir = [docs stringByAppendingPathComponent:@"Machines/Win11"];
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:[win11Dir stringByAppendingPathComponent:@"code.fd"]]) {
        return win11Dir;
    }
    if ([fm fileExistsAtPath:[docs stringByAppendingPathComponent:@"code.fd"]]) {
        return docs;
    }
    [fm createDirectoryAtPath:win11Dir withIntermediateDirectories:YES attributes:nil error:nil];
    return win11Dir;
}

- (BOOL)hasRawDiskImageAt:(NSString *)dir {
    NSString *raw = [dir stringByAppendingPathComponent:@"windows11.img"];
    NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:raw error:nil];
    return attrs && [attrs fileSize] >= (64ull << 20);
}

- (BOOL)needsSparseUnpack {
    NSString *dir = [self machineDirectory];
    NSString *sparse = [dir stringByAppendingPathComponent:@"windows11.hvsparse"];
    return [[NSFileManager defaultManager] fileExistsAtPath:sparse];
}

- (BOOL)hasProvisionedMachine {
    NSString *dir = [self machineDirectory];
    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:[dir stringByAppendingPathComponent:@"code.fd"]]) return NO;
    if (![fm fileExistsAtPath:[dir stringByAppendingPathComponent:@"vars.fd"]]) return NO;
    return [self hasRawDiskImageAt:dir] || [self needsSparseUnpack];
}

- (void)updateUnpackProgress:(double)frac {
    _unpackProgress = frac;
    _vmStatus = [NSString stringWithFormat:@"Unpacking Windows 11 NVMe image (%.0f%%)...", frac * 100.0];
    dispatch_async(_diagQueue, ^{
        [self writeDiagnosticsSnapshot];
    });
    [self notifyChanged];
}

- (void)bindMetalLayer:(nullable CAMetalLayer *)layer {
    _pendingLayer = layer;
    if (_vmRunning) {
        hv_winvm_set_display_layer((__bridge void *)layer);
    }
}

- (void)prepareAndStartWithLayer:(nullable CAMetalLayer *)layer {
    if (layer) _pendingLayer = layer;
    if (_vmRunning || _unpacking) return;

    if (!self.isJITLive) {
        [self tryAcquireJIT:YES];
        if (!self.isJITLive) {
            _vmStatus = @"Waiting for TXM JIT (attach StikDebug or launch_on_duo.sh)";
            dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
            [self notifyChanged];
            return;
        }
    }

    if (![self hasProvisionedMachine]) {
        _vmStatus = @"Place code.fd, vars.fd, and windows11.hvsparse in Documents/Machines/Win11";
        dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
        [self notifyChanged];
        return;
    }

    NSString *dir = [self machineDirectory];
    if ([self needsSparseUnpack]) {
        _unpacking = YES;
        _unpackProgress = 0.0;
        _vmStatus = @"Unpacking Windows 11 NVMe image (0%)...";
        dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
        [self notifyChanged];

        NSString *src = [dir stringByAppendingPathComponent:@"windows11.hvsparse"];
        NSString *dst = [dir stringByAppendingPathComponent:@"windows11.img"];
        NSString *tmpDst = [dir stringByAppendingPathComponent:@"windows11.img.unpacking"];

        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            unlink(tmpDst.fileSystemRepresentation);
            unlink(dst.fileSystemRepresentation);
            UnpackCtx ctx = {.bridge = self, .lastUpdate = 0};
            int rc = hv_sparse_unpack(src.fileSystemRepresentation, tmpDst.fileSystemRepresentation, unpack_progress_cb, &ctx);
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_unpacking = NO;
                if (rc == 0) {
                    unlink(dst.fileSystemRepresentation);
                    rename(tmpDst.fileSystemRepresentation, dst.fileSystemRepresentation);
                    unlink(src.fileSystemRepresentation);
                    self->_unpackProgress = 1.0;
                    [self startConfiguredVMInDirectory:dir];
                } else {
                    unlink(tmpDst.fileSystemRepresentation);
                    self->_vmStatus = @"Error: failed to unpack windows11.hvsparse";
                    dispatch_async(self->_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
                    [self notifyChanged];
                }
            });
        });
        return;
    }

    [self startConfiguredVMInDirectory:dir];
}

- (void)startConfiguredVMInDirectory:(NSString *)dir {
    if (_vmRunning) return;
    [self configureAudioSessionAndMicPermission];

    WinIosDualMapping jit = winios_jit_mapping();
    if (!jit.rx_addr || !jit.rw_addr) {
        _vmStatus = @"Error: JIT region missing";
        [self notifyChanged];
        return;
    }

    NSString *ramBacking = [NSTemporaryDirectory() stringByAppendingPathComponent:@"win11_guest_ram.bin"];
    setenv("HV_RAM_FILE", ramBacking.fileSystemRepresentation, 1);

    uint64_t preallocBytes = 0;
    void *preallocRam = winios_jit_guest_ram(&preallocBytes);
    uint64_t ramMB = 3072;
    if (preallocRam && preallocBytes >= (1024ull << 20)) {
        ramMB = preallocBytes >> 20;
    } else {
        uint64_t availMB = [self availableMemoryMB];
        if (availMB >= 5000) ramMB = 4096;
        else if (availMB >= 4200) ramMB = 3584;
        else if (availMB < 3400 && availMB > 1800) ramMB = (availMB - 768) & ~127ull;
        else if (availMB <= 1800) ramMB = 2048;
    }

    NSString *sharedDir = [[self documentsPath] stringByAppendingPathComponent:@"Shared"];
    [[NSFileManager defaultManager] createDirectoryAtPath:sharedDir withIntermediateDirectories:YES attributes:nil error:nil];
    /* Keep AF_UNIX sun_path well under Darwin's 104-byte limit (~82 bytes vs 106 bytes) */
    NSString *rawTmp = [NSTemporaryDirectory() stringByReplacingOccurrencesOfString:@"/private/var/" withString:@"/var/"];
    NSString *shareSock = [rawTmp stringByAppendingPathComponent:@"s"];
    unlink(shareSock.fileSystemRepresentation);

    NSString *codePath = [dir stringByAppendingPathComponent:@"code.fd"];
    NSString *varsPath = [dir stringByAppendingPathComponent:@"vars.fd"];
    NSString *diskPath = [dir stringByAppendingPathComponent:@"windows11.img"];

    _configuredRamMB = ramMB;
    _configuredCPUs = 4;

    HvWinVmConfig cfg = {
        .code_fd = codePath.fileSystemRepresentation,
        .vars_fd = varsPath.fileSystemRepresentation,
        .disk0 = diskPath.fileSystemRepresentation,
        .disk1 = NULL,
        .share_dir = sharedDir.fileSystemRepresentation,
        .share_sock = shareSock.fileSystemRepresentation,
        .ncpus = _configuredCPUs,
        .ram_mb = _configuredRamMB,
        .jit_bytes = jit.size,
        .jit_rx = jit.rx_addr,
        .jit_rw = jit.rw_addr,
        .prealloc_ram = preallocRam,
        .no_net = 0,
        .enable_sound = 1,
        .log = 0,
        .stats_interval = 0,
    };

    char err[256] = {0};
    if (hv_winvm_create(&cfg, err, sizeof err) != 0) {
        _vmStatus = [NSString stringWithFormat:@"VM create failed: %s", err];
        dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
        [self notifyChanged];
        return;
    }

    CAMetalLayer *layer = _pendingLayer;
    if (layer) {
        hv_winvm_set_display_layer((__bridge void *)layer);
    }

    _vmRunning = YES;
    _bootStartTime = CFAbsoluteTimeGetCurrent();
    _diagTickCount = 0;
    _lastMachTicks = mach_absolute_time();
    _lastWfiTicks = 0;
    _lastWfiIdlePct = 0.0;
    _vmStatus = [NSString stringWithFormat:@"Running Windows 11 ARM64 (%d vCPUs, %llu MB RAM)", _configuredCPUs, _configuredRamMB];
    [[UIApplication sharedApplication] setIdleTimerDisabled:YES];

    hv_winvm_start(1, vm_exit_trampoline);

    [self startDiagTimer];
    dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
    [self notifyChanged];
}

- (void)startDiagTimer {
    [self stopDiagTimer];
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _diagQueue);
    dispatch_source_set_timer(timer,
                              dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5 * NSEC_PER_SEC)),
                              8ull * NSEC_PER_SEC,
                              1ull * NSEC_PER_SEC);
    __weak typeof(self) weakSelf = self;
    dispatch_source_set_event_handler(timer, ^{
        typeof(self) strongSelf = weakSelf;
        if (!strongSelf) return;
        [strongSelf onDiagTickBackground];
    });
    _diagTimer = timer;
    dispatch_resume(timer);
}

- (void)stopDiagTimer {
    if (_diagTimer) {
        dispatch_source_cancel(_diagTimer);
        _diagTimer = nil;
    }
}

- (void)onDiagTickBackground {
    _diagTickCount++;
    [self writeDiagnosticsSnapshot];
    if (_vmRunning) {
        // A full fsync of the multi-GB image stalls guest disk I/O while it drains; the kernel
        // keeps dirty pages across an app kill, so a barrier is enough here. Backgrounding and
        // stopping still do the full sync.
        if ((_diagTickCount % 4u) == 0u) {
            hv_winvm_checkpoint_disks();
        }
        double uptime = CFAbsoluteTimeGetCurrent() - _bootStartTime;
        if (uptime <= 240.0 || getenv("WINIOS_ALWAYS_SCREENSHOT") != NULL) {
            NSString *docs = [self documentsPath];
            NSString *tmpPng = [docs stringByAppendingPathComponent:@"latest_screen.png.tmp"];
            NSString *dstPng = [docs stringByAppendingPathComponent:@"latest_screen.png"];
            if (hv_winvm_save_png(tmpPng.fileSystemRepresentation) == 0) {
                rename(tmpPng.fileSystemRepresentation, dstPng.fileSystemRepresentation);
            }
        }
    }
    [self notifyChanged];
}

- (void)writeDiagnosticsSnapshot {
    NSString *docs = [self documentsPath];
    NSString *statusFile = [docs stringByAppendingPathComponent:@"boot_status.txt"];
    BOOL running = _vmRunning;
    double uptime = running ? (CFAbsoluteTimeGetCurrent() - _bootStartTime) : 0.0;
    uint32_t w = [self framebufferWidth], h = [self framebufferHeight];
    uint64_t frames = [self presentedFrames];

    int kbdReady = 0, tabReady = 0;
    uint64_t usbDelivered = 0;
    uint64_t jitUsedBytes = 0, jitCapBytes = 0, jitFullFlushes = 0, wfiTicks = 0;
    double wfiIdlePct = _lastWfiIdlePct;
    if (running) {
        hv_winvm_usb_stats(&kbdReady, &tabReady, &usbDelivered);
        hv_winvm_engine_stats(&jitUsedBytes, &jitCapBytes, &jitFullFlushes, &wfiTicks);
        uint64_t nowMach = mach_absolute_time();
        if (_lastMachTicks > 0 && nowMach > _lastMachTicks && wfiTicks >= _lastWfiTicks && _configuredCPUs > 0) {
            double pct = 100.0 * (double)(wfiTicks - _lastWfiTicks) /
                         ((double)(nowMach - _lastMachTicks) * (double)_configuredCPUs);
            if (pct < 0.0) pct = 0.0;
            if (pct > 100.0) pct = 100.0;
            wfiIdlePct = pct;
            _lastWfiIdlePct = pct;
        }
        _lastMachTicks = nowMach;
        _lastWfiTicks = wfiTicks;
    }

    os_unfair_lock_lock(&_metricsLock);
    CGSize pt = _screenSizePts;
    CGFloat scale = _screenScale;
    NSInteger hingeStatus = _hingeStatus;
    double hingeDeg = _hingeAngleDegrees;
    os_unfair_lock_unlock(&_metricsLock);

    NSString *txt = [NSString stringWithFormat:
        @"vm_running: %@\n"
        @"vm_status: %@\n"
        @"jit_live: %@\n"
        @"jit_status: %@\n"
        @"uptime_sec: %.1f\n"
        @"cpus: %d\n"
        @"ram_mb: %llu\n"
        @"avail_mem_mb: %llu\n"
        @"jit_used_mb: %.1f\n"
        @"jit_cap_mb: %.0f\n"
        @"jit_full_flushes: %llu\n"
        @"wfi_idle_pct: %.1f\n"
        @"fb_size: %ux%u\n"
        @"frames_presented: %llu\n"
        @"usb_kbd_ready: %d\n"
        @"usb_tab_ready: %d\n"
        @"usb_delivered: %llu\n"
        @"share_up: %d\n"
        @"oauth_safari: %d\n"
        @"hinge_status: %ld\n"
        @"hinge_angle_deg: %.1f\n"
        @"screen_pts: %.0fx%.0f @%.0fx\n"
        @"guest_ram_mb: %llu\n"
        @"phys_mb: %llu\n",
        running ? @"true" : @"false",
        _vmStatus ?: @"",
        self.isJITLive ? @"true" : @"false",
        self.jitStatusText ?: @"",
        uptime,
        _configuredCPUs,
        _configuredRamMB,
        [self availableMemoryMB],
        (double)jitUsedBytes / (1024.0 * 1024.0),
        (double)jitCapBytes / (1024.0 * 1024.0),
        jitFullFlushes,
        wfiIdlePct,
        w, h,
        frames,
        kbdReady,
        tabReady,
        usbDelivered,
        running ? hv_winvm_share_up() : 0,
        hv_share_url_handler_enabled(),
        (long)hingeStatus,
        hingeDeg,
        pt.width, pt.height, scale,
        self.guestRAMMB,
        (unsigned long long)(NSProcessInfo.processInfo.physicalMemory >> 20)];
    [txt writeToFile:statusFile atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

- (void)handleVMExitWithReason:(NSString *)reason {
    _vmRunning = NO;
    [self stopDiagTimer];
    [[UIApplication sharedApplication] setIdleTimerDisabled:NO];
    _vmStatus = [NSString stringWithFormat:@"Stopped (%@)", reason];
    dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
    [self notifyChanged];
}

- (void)requestDisplayWidth:(uint32_t)width height:(uint32_t)height {
    hv_winvm_request_display_size(width, height);
}

- (void)sendTabletNormalizedX:(double)x y:(double)y buttons:(uint8_t)buttons wheel:(int8_t)wheel {
    if (!_vmRunning) return;
    hv_winvm_tablet(x, y, buttons, wheel);
}

- (void)sendKeyHid:(uint8_t)usage down:(BOOL)down {
    if (!_vmRunning) return;
    hv_winvm_key_hid(usage, down ? 1 : 0);
}

- (void)tapKeyHid:(uint8_t)usage {
    if (!_vmRunning) return;
    hv_winvm_key_hid(usage, 1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(35 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
        hv_winvm_key_hid(usage, 0);
    });
}

- (void)sendKeyComboWithModifiers:(NSArray<NSNumber *> *)modifiers key:(uint8_t)usage {
    if (!_vmRunning) return;
    for (NSNumber *m in modifiers) {
        hv_winvm_key_hid((uint8_t)m.unsignedCharValue, 1);
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(18 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
        if (usage != 0) hv_winvm_key_hid(usage, 1);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(38 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
            if (usage != 0) hv_winvm_key_hid(usage, 0);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(15 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
                for (NSNumber *m in [modifiers reverseObjectEnumerator]) {
                    hv_winvm_key_hid((uint8_t)m.unsignedCharValue, 0);
                }
            });
        });
    });
}

- (void)sendWindowsKey {
    [self tapKeyHid:0xe3]; /* Left GUI / Windows key */
}

- (void)sendEscape {
    [self tapKeyHid:0x29]; /* Escape */
}

- (void)sendAltTab {
    [self sendKeyComboWithModifiers:@[@(0xe2)] key:0x2b];
}

- (void)sendCtrlAltDel {
    if (!_vmRunning) return;
    hv_winvm_send_ctrl_alt_del();
}

- (void)pressPowerButton {
    if (!_vmRunning) return;
    hv_winvm_power_button();
}

- (void)flushDisks {
    if (!_vmRunning) return;
    hv_winvm_flush_disks();
}

- (void)stopVM {
    if (!_vmRunning) return;
    hv_winvm_stop();
}

- (BOOL)useSafariForGuestLinks {
    return hv_share_url_handler_enabled() != 0;
}

- (void)setUseSafariForGuestLinks:(BOOL)useSafariForGuestLinks {
    hv_share_set_url_handler_enabled(useSafariForGuestLinks ? 1 : 0);
    [[NSUserDefaults standardUserDefaults] setBool:useSafariForGuestLinks forKey:@"winios.safariOAuthV2"];
}

- (nullable NSString *)takeGuestOpenURL {
    char *raw = hv_share_take_open_url();
    if (!raw) return nil;
    NSString *s = [NSString stringWithUTF8String:raw];
    free(raw);
    return s;
}

- (BOOL)takeGuestOAuthDone {
    return hv_share_take_oauth_done() != 0;
}

@end
