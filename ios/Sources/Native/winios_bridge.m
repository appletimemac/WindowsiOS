/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "winios_bridge.h"

#include "disk.h"
#include "hvsparse.h"
#include "share.h"
#include "winios_jit.h"
#include "winvm.h"
#include "cmdfile.h"

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
    CGRect _pictureRect; /* the picture's place in _pendingLayer, as fractions of it; null: all of it */
    uint64_t _diagTickCount;
    uint64_t _lastMachTicks;
    uint64_t _lastWfiTicks;
    double _lastWfiIdlePct;
    dispatch_source_t _perfTimer;
    HvPerfCsv _perfCsv;
    BOOL _engineStats;
    NSThread *_cmdThread;
    uint64_t _vmStartNS;
    uint64_t _perfLastExceptions, _perfLastUptime;
    unsigned _exceptionStormRows;
    BOOL _hangStateWritten;

    os_unfair_lock _metricsLock;
    CGSize _screenSizePts;
    CGFloat _screenScale;
    NSInteger _hingeStatus;
    double _hingeAngleDegrees;
    NSString *_geometryReport;
    uint32_t _requestedGuestW, _requestedGuestH;
}
- (void)handleVMExitWithReason:(NSString *)reason;
- (void)updateUnpackProgress:(double)frac;
- (void)startDiagTimer;
- (void)stopDiagTimer;
- (void)startCommandFile;
- (void)commandMark:(const char *)label;
- (void)commandPerfRow;
@end

static uint64_t command_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef struct IOSCmdCtx {
    __unsafe_unretained WinIOSBridge *bridge;
    const char *docs;
    FILE *log;
    int quit;
} IOSCmdCtx;

static int ios_cmd_path(void *opaque, const char *name, char *path, size_t size) {
    IOSCmdCtx *ctx = opaque;
    if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) return -1;
    int n = snprintf(path, size, "%s/%s", ctx->docs, name);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static void ios_cmd_mark(void *opaque, const char *label) {
    IOSCmdCtx *ctx = opaque;
    [ctx->bridge commandMark:label];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    fprintf(ctx->log, "MARK %llu %s\n", (unsigned long long)ms, label);
}

static void ios_cmd_perf_row(void *opaque) {
    IOSCmdCtx *ctx = opaque;
    [ctx->bridge commandPerfRow];
}

static void ios_cmd_quit(void *opaque) {
    ((IOSCmdCtx *)opaque)->quit = 1;
}

static int ios_cmd_stopped(void *opaque) {
    return ((IOSCmdCtx *)opaque)->quit || NSThread.currentThread.cancelled;
}

static uint64_t ios_cmd_seq(const char *text) {
    if (!text || *text < '0' || *text > '9') return 0;
    char *end;
    errno = 0;
    uint64_t seq = strtoull(text, &end, 10);
    if (errno) return 0;
    if (*end == '\r') end++;
    if (*end == '\n') end++;
    return *end ? 0 : seq;
}

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

/* Test settings for a launch that cannot carry an environment: after an install StikDebug restarts the
   app without the one devicectl gave it, so the remote command file and the engine statistics were off
   until somebody shut Windows down by hand. Documents/launch_env.json is a flat object of strings; only
   HV_ and WINIOS_ names are taken, and never over a variable the launch did set. */
static void apply_launch_env_file(NSString *docs) {
    NSData *data = [NSData dataWithContentsOfFile:[docs stringByAppendingPathComponent:@"launch_env.json"]];
    if (!data) return;
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![json isKindOfClass:[NSDictionary class]]) return;
    [(NSDictionary *)json enumerateKeysAndObjectsUsingBlock:^(id key, id value, __unused BOOL *stop) {
        if (![key isKindOfClass:[NSString class]] || ![value isKindOfClass:[NSString class]]) return;
        if (![key hasPrefix:@"HV_"] && ![key hasPrefix:@"WINIOS_"]) return;
        setenv([key UTF8String], [value UTF8String], 0);
    }];
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
        apply_launch_env_file([self documentsPath]); /* before anything reads the environment */
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

- (void)updateGeometryReport:(NSString *)report {
    os_unfair_lock_lock(&_metricsLock);
    _geometryReport = [report copy];
    os_unfair_lock_unlock(&_metricsLock);
    dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
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

/* The translated-code region: 768 MiB unless the launch asks for another size (WINIOS_JIT_MB, or the
   winios.jitMB default) between 128 and WINIOS_JIT_MAX_BYTES, for measuring the trade-off on the device. */
static size_t jit_region_bytes(void) {
    const char *env = getenv("WINIOS_JIT_MB");
    long mb = env ? strtol(env, NULL, 10) : (long)[[NSUserDefaults standardUserDefaults] integerForKey:@"winios.jitMB"];
    if (mb < 128 || (size_t)mb << 20 > WINIOS_JIT_MAX_BYTES) return WINIOS_JIT_DEFAULT_BYTES;
    return (size_t)mb << 20;
}

- (BOOL)tryAcquireJIT:(BOOL)detachAfter {
    BOOL ok = winios_jit_acquire(jit_region_bytes(), detachAfter);
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
    _pictureRect = CGRectNull; /* the canvas says where its picture goes right after binding */
    if (_vmRunning) {
        hv_winvm_set_display_layer((__bridge void *)layer);
        hv_winvm_set_display_content(0, 0, 0, 0);
    }
}

- (void)setPictureRect:(CGRect)rect forLayer:(CAMetalLayer *)layer {
    /* Two canvases exist for a moment when the layout changes; only the bound one places the picture. */
    if (layer != _pendingLayer) return;
    _pictureRect = rect;
    if (_vmRunning) [self applyPictureRect];
}

- (void)applyPictureRect {
    CGRect r = CGRectIsNull(_pictureRect) ? CGRectZero : _pictureRect;
    hv_winvm_set_display_content(r.origin.x, r.origin.y, r.size.width, r.size.height);
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
    /* Windows gets a 64 GB disk: the file grows sparse (no space is used up front) and the guest extends C:
       into the new space (scripts/winios_extend_c.ps1). Before the disk is opened; a failure leaves the
       image as it was, which still boots. */
    hv_disk_grow(diskPath.fileSystemRepresentation, HV_DISK_GROW_TARGET);
    /* The "WindowsiOS Tools" disk (the display driver, posture agent and their installer for Windows) rides as the
       second NVMe disk whenever it has been copied in. */
    NSString *toolsImage = [dir stringByAppendingPathComponent:@"tools.img"];
    const char *toolsPath = [[NSFileManager defaultManager] fileExistsAtPath:toolsImage] ? toolsImage.fileSystemRepresentation : NULL;

    _configuredRamMB = ramMB;
    /* 4 vCPUs; WINIOS_CPUS (1 to 4) at launch for measuring against the phone's 2 performance cores. Only on a
       fresh boot: a hibernated Windows must resume with the CPU count it hibernated with. */
    const char *cpusEnv = getenv("WINIOS_CPUS");
    int cpus = cpusEnv ? atoi(cpusEnv) : 4;
    _configuredCPUs = cpus >= 1 && cpus <= 4 ? cpus : 4;
    _engineStats = getenv("HV_ENGINE_STATS") != NULL || [[NSUserDefaults standardUserDefaults] boolForKey:@"winios.engineStats"];

    HvWinVmConfig cfg = {
        .code_fd = codePath.fileSystemRepresentation,
        .vars_fd = varsPath.fileSystemRepresentation,
        .disk0 = diskPath.fileSystemRepresentation,
        .disk1 = toolsPath,
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
        /* Engine stats (translation time, lock waits) cost a little on every translation: on request only
           (launch with HV_ENGINE_STATS=1, or the winios.engineStats default). The huge interval keeps the
           counters on without ever printing the stats summary, which would stall CPU 0. */
        .stats_interval = _engineStats ? 1000000 : 0,
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
        [self applyPictureRect];
    }

    _vmRunning = YES;
    _bootStartTime = CFAbsoluteTimeGetCurrent();
    _vmStartNS = command_mono_ns();
    _diagTickCount = 0;
    _lastMachTicks = mach_absolute_time();
    _lastWfiTicks = 0;
    _lastWfiIdlePct = 0.0;
    _vmStatus = [NSString stringWithFormat:@"Running Windows 11 ARM64 (%d vCPUs, %llu MB RAM)", _configuredCPUs, _configuredRamMB];
    [[UIApplication sharedApplication] setIdleTimerDisabled:YES];

    hv_winvm_start(1, vm_exit_trampoline);

    [self startDiagTimer];
    [self startPerfLog];
    [self startCommandFile];
    dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
    [self notifyChanged];
}

- (void)startCommandFile {
    const char *enabled = getenv("WINIOS_CMD");
    if (!enabled || strcmp(enabled, "1")) return;
    [_cmdThread cancel];
    NSString *docs = [self documentsPath];
    uint64_t startNS = _vmStartNS;
    _cmdThread = [[NSThread alloc] initWithBlock:^{
        @autoreleasepool {
            NSString *input = [docs stringByAppendingPathComponent:@"winvm_cmds.txt"];
            NSString *done = [docs stringByAppendingPathComponent:@"winvm_cmds.done"];
            NSString *logPath = [docs stringByAppendingPathComponent:@"winvm_cmds.log"];
            FILE *log = fopen(logPath.fileSystemRepresentation, "a");
            if (!log) return;
            IOSCmdCtx ctx = {.bridge = self, .docs = docs.fileSystemRepresentation, .log = log};
            HvCmdFile cmd = {.opaque = &ctx, .out = log, .err = log, .path = ios_cmd_path,
                             .mark = ios_cmd_mark, .perf_row = ios_cmd_perf_row,
                             .quit = ios_cmd_quit, .stopped = ios_cmd_stopped};
            NSString *savedSeq = [NSString stringWithContentsOfFile:done encoding:NSUTF8StringEncoding error:nil];
            uint64_t lastSeq = ios_cmd_seq(savedSeq.UTF8String);
            NSString *previous = nil;
            while (!ios_cmd_stopped(&ctx)) {
                @autoreleasepool {
                    /* Reopen each time: devicectl can replace the inode. Require a stable, complete copy. */
                    NSString *text = [NSString stringWithContentsOfFile:input encoding:NSUTF8StringEncoding error:nil];
                    if ([text hasSuffix:@"\n"] && [text isEqualToString:previous]) {
                        NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
                        const char *header = lines[0].UTF8String;
                        uint64_t seq = !strncmp(header, "#seq ", 5) ? ios_cmd_seq(header + 5) : 0;
                        if (seq > lastSeq) {
                            for (NSUInteger i = 1; i < lines.count && !ios_cmd_stopped(&ctx); i++) {
                                NSString *line = lines[i];
                                if ([line hasSuffix:@"\r"]) line = [line substringToIndex:line.length - 1];
                                if (!line.length) continue;
                                uint64_t ms = (command_mono_ns() - startNS) / 1000000;
                                fprintf(log, "[%llu] %s\n", (unsigned long long)ms, line.UTF8String);
                                hv_cmdfile_exec(&cmd, line.UTF8String);
                                fflush(log);
                            }
                            lastSeq = seq;
                            if (!NSThread.currentThread.cancelled) {
                                NSString *ack = [NSString stringWithFormat:@"%llu\n", (unsigned long long)seq];
                                [ack writeToFile:done atomically:YES encoding:NSUTF8StringEncoding error:nil];
                            }
                        }
                    }
                    previous = text;
                }
                if (!ios_cmd_stopped(&ctx)) usleep(200000);
            }
            fclose(log);
        }
    }];
    _cmdThread.name = @"winvm.cmdfile";
    [_cmdThread start];
}

- (void)commandMark:(const char *)label {
    uint64_t t = (uint64_t)(NSDate.date.timeIntervalSince1970 * 1000.0);
    dispatch_sync(_diagQueue, ^{ hv_perf_csv_mark(&self->_perfCsv, t, label); });
}

- (void)commandPerfRow {
    dispatch_sync(_diagQueue, ^{
        HvPerfSnap s;
        hv_winvm_perf_snapshot(&s);
        s.avail_mem_mb_now = [self availableMemoryMB];
        s.thermal_now = (uint64_t)NSProcessInfo.processInfo.thermalState;
        s.low_power_now = NSProcessInfo.processInfo.lowPowerModeEnabled ? 1 : 0;
        hv_perf_csv_row(&self->_perfCsv, &s);
    });
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

static int sysctl_int(const char *name) {
    int v = 0;
    size_t n = sizeof v;
    return sysctlbyname(name, &v, &n, NULL, 0) == 0 ? v : -1;
}

/* Documents/perf.csv: one row of every counter (host/perf.h) every 2 s, kept to about 8 MB plus perf.csv.1.
   Pulled with devicectl and read by scripts/perf_report.py. */
- (void)startPerfLog {
    [self stopPerfLog];
    NSString *path = [[self documentsPath] stringByAppendingPathComponent:@"perf.csv"];
    NSString *build = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"?";
    const char *qos = getenv("HV_QOS");
    NSString *comment = [NSString stringWithFormat:
        @"start=%.0f build=%@ os=%@ cpus=%d ram_mb=%llu jit_mb=%.0f pcores=%d ecores=%d engine_stats=%d qos=%s",
        [NSDate date].timeIntervalSince1970, build, UIDevice.currentDevice.systemVersion, _configuredCPUs,
        _configuredRamMB, (double)winios_jit_mapping().size / (1024.0 * 1024.0),
        sysctl_int("hw.perflevel0.logicalcpu"), sysctl_int("hw.perflevel1.logicalcpu"), _engineStats ? 1 : 0,
        qos ? qos : "default"];
    dispatch_sync(_diagQueue, ^{
        hv_perf_csv_open(&self->_perfCsv, path.fileSystemRepresentation, 8l << 20, comment.UTF8String);
        HvPerfSnap initial;
        hv_winvm_perf_snapshot(&initial);
        self->_perfLastExceptions = initial.exceptions;
        self->_perfLastUptime = initial.uptime_ms;
        self->_exceptionStormRows = 0;
    });
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _diagQueue);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2 * NSEC_PER_SEC)),
                              2ull * NSEC_PER_SEC, NSEC_PER_SEC / 10);
    __weak typeof(self) weakSelf = self;
    dispatch_source_set_event_handler(timer, ^{
        typeof(self) strongSelf = weakSelf;
        if (!strongSelf || !strongSelf->_vmRunning) return;
        HvPerfSnap s;
        hv_winvm_perf_snapshot(&s);
        s.avail_mem_mb_now = [strongSelf availableMemoryMB];
        s.thermal_now = (uint64_t)NSProcessInfo.processInfo.thermalState;
        s.low_power_now = NSProcessInfo.processInfo.lowPowerModeEnabled ? 1 : 0;
        hv_perf_csv_row(&strongSelf->_perfCsv, &s);
        uint64_t elapsed = s.uptime_ms - strongSelf->_perfLastUptime;
        uint64_t before = strongSelf->_perfLastExceptions;
        BOOL storm = s.uptime_ms > strongSelf->_perfLastUptime && s.exceptions >= before &&
                     (double)(s.exceptions - before) * 1000.0 / (double)elapsed > 500000.0;
        strongSelf->_perfLastExceptions = s.exceptions;
        strongSelf->_perfLastUptime = s.uptime_ms;
        strongSelf->_exceptionStormRows = storm ? strongSelf->_exceptionStormRows + 1 : 0;
        if (!strongSelf->_hangStateWritten && strongSelf->_exceptionStormRows >= 3) {
            strongSelf->_hangStateWritten = YES;
            NSString *state = [[strongSelf documentsPath] stringByAppendingPathComponent:@"hang_state.txt"];
            int rc = hv_cmdfile_state(state.fileSystemRepresentation);
            const char *label = rc == 0 ? "exception storm: wrote Documents/hang_state.txt" :
                                         "exception storm: failed to write Documents/hang_state.txt";
            hv_perf_csv_mark(&strongSelf->_perfCsv, s.t_ms, label);
            fprintf(stderr, "winvm: %s (rc=%d)\n", label, rc);
        }
    });
    _perfTimer = timer;
    dispatch_resume(timer);
}

- (void)stopPerfLog {
    if (_perfTimer) {
        dispatch_source_cancel(_perfTimer);
        _perfTimer = nil;
    }
    dispatch_sync(_diagQueue, ^{ hv_perf_csv_close(&self->_perfCsv); });
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
    NSString *geometry = _geometryReport ?: @"";
    uint32_t requestW = _requestedGuestW, requestH = _requestedGuestH;
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
        @"jit_flushes: %llu\n"
        @"wfi_idle_pct: %.1f\n"
        @"thermal_state: %ld\n"
        @"low_power: %d\n"
        @"cores_perf_eff: %d/%d\n"
        @"fb_size: %ux%u\n"
        @"frames_presented: %llu\n"
        @"usb_kbd_ready: %d\n"
        @"usb_tab_ready: %d\n"
        @"usb_delivered: %llu\n"
        @"share_up: %d\n"
        @"focus_agent_seen_s: %ld\n"
        @"oauth_safari: %d\n"
        @"hinge_status: %ld\n"
        @"hinge_angle_deg: %.1f\n"
        @"screen_pts: %.0fx%.0f @%.0fx\n"
        @"guest_request: %ux%u\n"
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
        (long)NSProcessInfo.processInfo.thermalState,
        NSProcessInfo.processInfo.lowPowerModeEnabled ? 1 : 0,
        sysctl_int("hw.perflevel0.logicalcpu"), sysctl_int("hw.perflevel1.logicalcpu"),
        w, h,
        frames,
        kbdReady,
        tabReady,
        usbDelivered,
        running ? hv_winvm_share_up() : 0,
        hv_share_focus_agent_seen(),
        hv_share_url_handler_enabled(),
        (long)hingeStatus,
        hingeDeg,
        pt.width, pt.height, scale,
        requestW, requestH,
        self.guestRAMMB,
        (unsigned long long)(NSProcessInfo.processInfo.physicalMemory >> 20)];
    txt = [txt stringByAppendingString:geometry];
    [txt writeToFile:statusFile atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

- (void)handleVMExitWithReason:(NSString *)reason {
    _vmRunning = NO;
    [_cmdThread cancel];
    [self stopDiagTimer];
    [self stopPerfLog];
    [[UIApplication sharedApplication] setIdleTimerDisabled:NO];
    _vmStatus = [NSString stringWithFormat:@"Stopped (%@)", reason];
    dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
    [self notifyChanged];
}

- (void)sendTouchContacts:(const WinIOSTouchContact *)contacts count:(NSInteger)count {
    if (!_vmRunning || !contacts || count <= 0) return;
    HvTouchContact frame[5];
    int n = (int)MIN(count, 5);
    for (int i = 0; i < n; i++) {
        frame[i] = (HvTouchContact){.id = contacts[i].id, .tip = contacts[i].tip, .x = contacts[i].x, .y = contacts[i].y};
    }
    hv_winvm_touch(frame, n);
}

- (BOOL)touchscreenReady {
    return _vmRunning && hv_winvm_touch_ready();
}

- (void)requestDisplayWidth:(uint32_t)width height:(uint32_t)height {
    os_unfair_lock_lock(&_metricsLock);
    _requestedGuestW = width;
    _requestedGuestH = height;
    os_unfair_lock_unlock(&_metricsLock);
    hv_winvm_request_display_size(width, height);
    dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
}

- (void)setLaptopPosture:(BOOL)laptop {
    hv_winvm_set_posture(laptop ? 1 : 0);
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

- (void)requestHibernate {
    if (!_vmRunning) return;
    hv_winvm_request_hibernate();
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

/* The event the app noted last, on the main queue. A report that answers an earlier one is dropped there: the
   share server has checked it against its own latest, but a newer tap can be noted before the report is delivered.
   Only earlier ones: the command file's notetap and notelook note events the app never sees, and their reports
   must still arrive. */
static unsigned long latest_focus_event;

static void text_focus_reported(unsigned long seq, int editable, int inside, void *ctx) {
    (void)ctx;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (seq < latest_focus_event) return;
        void (^handler)(BOOL, BOOL) = WinIOSBridge.shared.onGuestTextFocus;
        if (handler) handler(editable != 0, inside != 0);
    });
}

- (void)setOnGuestTextFocus:(void (^)(BOOL, BOOL))onGuestTextFocus {
    _onGuestTextFocus = [onGuestTextFocus copy];
    hv_share_set_text_focus_handler(_onGuestTextFocus ? text_focus_reported : NULL, NULL);
}

- (NSInteger)textFocusAgentSeenSeconds {
    return (NSInteger)hv_share_focus_agent_seen();
}

- (void)noteTapAtGuestX:(int32_t)x y:(int32_t)y {
    dispatch_assert_queue(dispatch_get_main_queue());
    latest_focus_event = hv_share_note_tap(x, y);
}

- (void)noteFocusLook {
    dispatch_assert_queue(dispatch_get_main_queue());
    latest_focus_event = hv_share_note_look();
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
