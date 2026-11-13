/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * main_mac.m — macOS Cocoa + Headless runner for ARM64 Windows 11 (`winvm`).
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>

#include <ctype.h>
#include <mach/mach.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "winvm.h"
#include "cmdfile.h"
#include "disk.h"

/* macOS virtual key codes (kVK_*) -> Linux EV_KEY codes (mapped to USB HID by hv_winvm_key_linux) */
static const uint16_t kMacToLinuxKey[128] = {
    [0x00] = 30,  [0x01] = 31,  [0x02] = 32,  [0x03] = 33,  [0x04] = 35,  [0x05] = 34,  [0x06] = 44,  [0x07] = 45,
    [0x08] = 46,  [0x09] = 47,  [0x0a] = 86,  [0x0b] = 48,  [0x0c] = 16,  [0x0d] = 17,  [0x0e] = 18,  [0x0f] = 19,
    [0x10] = 21,  [0x11] = 20,  [0x12] = 2,   [0x13] = 3,   [0x14] = 4,   [0x15] = 5,   [0x16] = 7,   [0x17] = 6,
    [0x18] = 13,  [0x19] = 10,  [0x1a] = 8,   [0x1b] = 12,  [0x1c] = 9,   [0x1d] = 11,  [0x1e] = 27,  [0x1f] = 24,
    [0x20] = 22,  [0x21] = 26,  [0x22] = 23,  [0x23] = 25,  [0x24] = 28,  [0x25] = 38,  [0x26] = 36,  [0x27] = 40,
    [0x28] = 37,  [0x29] = 39,  [0x2a] = 43,  [0x2b] = 51,  [0x2c] = 53,  [0x2d] = 49,  [0x2e] = 50,  [0x2f] = 52,
    [0x30] = 15,  [0x31] = 57,  [0x32] = 41,  [0x33] = 14,  [0x35] = 1,   [0x36] = 126, [0x37] = 125, [0x38] = 42,
    [0x39] = 58,  [0x3a] = 56,  [0x3b] = 29,  [0x3c] = 54,  [0x3d] = 100, [0x3e] = 97,  [0x40] = 187, [0x41] = 83,
    [0x43] = 55,  [0x45] = 78,  [0x47] = 69,  [0x48] = 115, [0x49] = 114, [0x4a] = 113, [0x4b] = 98,  [0x4c] = 96,
    [0x4e] = 74,  [0x4f] = 188, [0x50] = 189, [0x51] = 117, [0x52] = 82,  [0x53] = 79,  [0x54] = 80,  [0x55] = 81,
    [0x56] = 75,  [0x57] = 76,  [0x58] = 77,  [0x59] = 71,  [0x5b] = 72,  [0x5c] = 73,  [0x60] = 63,  [0x61] = 64,
    [0x62] = 65,  [0x63] = 61,  [0x64] = 66,  [0x65] = 67,  [0x67] = 87,  [0x69] = 183, [0x6a] = 186, [0x6b] = 184,
    [0x6d] = 68,  [0x6f] = 88,  [0x71] = 185, [0x72] = 110, [0x73] = 102, [0x74] = 104, [0x75] = 111, [0x76] = 62,
    [0x77] = 107, [0x78] = 60,  [0x79] = 109, [0x7a] = 59,  [0x7b] = 105, [0x7c] = 106, [0x7d] = 108, [0x7e] = 103,
};

static volatile sig_atomic_t g_sig_stop = 0;

static void on_sig(int sig) {
    (void)sig;
    g_sig_stop = 1;
    hv_winvm_stop();
}

/* ---- Timestamps and the perf CSV (--perf-csv) ---- */

static uint64_t g_mono_zero;

/* Monotonic ms since the runner started: the prefix of every logged command. */
static uint64_t mono_ms(void) {
    return (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - g_mono_zero) / 1000000ull;
}

/* Wall-clock ms, the same clock as the t_ms column of the CSV rows, so a mark lines up with them. */
static uint64_t wall_ms(void) {
    return clock_gettime_nsec_np(CLOCK_REALTIME) / 1000000ull;
}

static HvPerfCsv g_perf;
static pthread_mutex_t g_perf_mu = PTHREAD_MUTEX_INITIALIZER; /* guards g_perf and g_perf_prev */
static pthread_t g_perf_tid;
static int g_perf_up;
static int g_perf_ms = 1000;
static atomic_int g_perf_quit;
static HvPerfSnap g_perf_prev;

static void perf_fill_host(HvPerfSnap *s) {
    static mach_port_t host;
    if (!host) host = mach_host_self();
    vm_statistics64_data_t vm;
    mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &n) == KERN_SUCCESS)
        s->avail_mem_mb_now = ((uint64_t)vm.free_count + vm.inactive_count + vm.speculative_count) *
                              (uint64_t)getpagesize() >> 20;
    @autoreleasepool {
        s->thermal_now = (uint64_t)NSProcessInfo.processInfo.thermalState;
    }
    s->low_power_now = 0;
}

static void perf_row(void) {
    pthread_mutex_lock(&g_perf_mu);
    if (g_perf.f) {
        HvPerfSnap s;
        hv_winvm_perf_snapshot(&s);
        perf_fill_host(&s);
        hv_perf_csv_row(&g_perf, &s);
        g_perf_prev = s;
    }
    pthread_mutex_unlock(&g_perf_mu);
}

/* Prints MARK <t_ms> <label> and, when the CSV is open, writes the same moment into it. */
static void perf_mark(const char *label) {
    uint64_t t = wall_ms();
    pthread_mutex_lock(&g_perf_mu);
    if (g_perf.f) hv_perf_csv_mark(&g_perf, t, label);
    pthread_mutex_unlock(&g_perf_mu);
    printf("MARK %llu %s\n", (unsigned long long)t, label);
    fflush(stdout);
}

static void *perf_thread(void *arg) {
    (void)arg;
    uint64_t next = mono_ms() + (uint64_t)g_perf_ms; /* perf_start wrote the first row */
    while (!atomic_load(&g_perf_quit)) {
        uint64_t now = mono_ms();
        if (now >= next) {
            perf_row();
            next += (uint64_t)g_perf_ms;
            if (next <= now) next = now + (uint64_t)g_perf_ms;
        }
        uint64_t wait = next > now ? next - now : 0;
        usleep((useconds_t)(wait < 50 ? wait : 50) * 1000u);
    }
    return NULL;
}

/* At exit, headless or not: one last row, then close. */
static void perf_finish(void) {
    if (!g_perf_up) return;
    g_perf_up = 0;
    atomic_store(&g_perf_quit, 1);
    pthread_join(g_perf_tid, NULL);
    pthread_mutex_lock(&g_perf_mu);
    if (g_perf.f) {
        HvPerfSnap s;
        hv_winvm_perf_snapshot(&s);
        perf_fill_host(&s);
        /* A vCPU thread that has exited can no longer be asked for its on-core time: keep the last value read. */
        uint64_t *run[4] = {&s.run0_ns, &s.run1_ns, &s.run2_ns, &s.run3_ns};
        const uint64_t prev[4] = {g_perf_prev.run0_ns, g_perf_prev.run1_ns, g_perf_prev.run2_ns, g_perf_prev.run3_ns};
        for (int i = 0; i < 4; i++)
            if (*run[i] < prev[i]) *run[i] = prev[i];
        hv_perf_csv_row(&g_perf, &s);
        hv_perf_csv_close(&g_perf);
    }
    pthread_mutex_unlock(&g_perf_mu);
}

static int perf_start(const char *path, const HvWinVmConfig *cfg) {
    char comment[256];
    const char *q = getenv("HV_QOS");
    /* winvm.c gives a zero jit_bytes 768 MiB. */
    snprintf(comment, sizeof comment, "runner=mac cpus=%d ram_mb=%llu jit_mb=%llu qos=%s", cfg->ncpus,
             (unsigned long long)cfg->ram_mb,
             (unsigned long long)(cfg->jit_bytes ? cfg->jit_bytes >> 20 : 768), q && *q ? q : "default");
    if (hv_perf_csv_open(&g_perf, path, 0, comment) != 0) return -1;
    perf_row();
    g_perf_up = 1;
    pthread_create(&g_perf_tid, NULL, perf_thread, NULL);
    atexit(perf_finish);
    return 0;
}

static void cmd_mark(void *opaque, const char *label) {
    (void)opaque;
    perf_mark(label);
}

static void cmd_perf_row(void *opaque) {
    (void)opaque;
    perf_row();
}

static void cmd_quit(void *opaque) {
    (void)opaque;
    hv_winvm_stop();
}

static int cmd_stopped(void *opaque) {
    (void)opaque;
    return g_sig_stop;
}

static void *cmd_file_thread(void *arg) {
    const char *path = (const char *)arg;
    FILE *f = fopen(path, "a+");
    if (!f) return NULL;
    HvCmdFile cmd = {.out = stdout, .err = stderr, .mark = cmd_mark,
                     .perf_row = cmd_perf_row, .quit = cmd_quit, .stopped = cmd_stopped};
    long pos = 0;
    while (!g_sig_stop) {
        char line[512];
        fseek(f, pos, SEEK_SET);
        if (!fgets(line, sizeof line, f) || !strchr(line, '\n')) {
            clearerr(f);
            usleep(150000);
            continue;
        }
        pos = ftell(f);
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;

        uint64_t t_cmd = mono_ms();
        if (hv_cmdfile_exec(&cmd, line)) break;
        fprintf(stderr, "[%llu] winvm: cmd: %s\n", (unsigned long long)t_cmd, line);
    }
    fclose(f);
    return NULL;
}

typedef struct TimerArgs {
    const char *screenshot_path;
    int screenshot_after;
    int timeout_sec;
} TimerArgs;

static void *timer_thread(void *arg) {
    TimerArgs *ta = (TimerArgs *)arg;
    int elapsed = 0;
    int shot_taken = 0;
    while (!g_sig_stop) {
        sleep(1);
        elapsed++;
        if (ta->screenshot_path && !shot_taken && ta->screenshot_after > 0 && elapsed >= ta->screenshot_after) {
            int rc = hv_winvm_save_png(ta->screenshot_path);
            uint32_t fw = 0, fh = 0;
            hv_winvm_display_size(&fw, &fh);
            fprintf(stderr, "winvm: saved screenshot %s at %ds (%ux%u, rc=%d)\n",
                    ta->screenshot_path, elapsed, fw, fh, rc);
            shot_taken = 1;
        }
        if (ta->timeout_sec > 0 && elapsed >= ta->timeout_sec) {
            if (ta->screenshot_path && !shot_taken) {
                int rc = hv_winvm_save_png(ta->screenshot_path);
                uint32_t fw = 0, fh = 0;
                hv_winvm_display_size(&fw, &fh);
                fprintf(stderr, "winvm: saved final screenshot %s at %ds (%ux%u, rc=%d)\n",
                        ta->screenshot_path, elapsed, fw, fh, rc);
            }
            fprintf(stderr, "winvm: timeout (%ds) reached — stopping VM.\n", ta->timeout_sec);
            hv_winvm_stop();
            break;
        }
    }
    return NULL;
}

/* ---- Cocoa Metal Display View ---- */

@interface WinVmDisplayView : NSView {
    CAMetalLayer *_metalLayer;
    NSUInteger _heldMods;
    uint8_t _buttons;
    double _lastX, _lastY;
}
@property (nonatomic, readonly) CAMetalLayer *metalLayer;
@end

@implementation WinVmDisplayView

- (instancetype)initWithFrame:(NSRect)frameRect {
    if ((self = [super initWithFrame:frameRect])) {
        self.wantsLayer = YES;
        _lastX = 0.5;
        _lastY = 0.5;
    }
    return self;
}

- (CALayer *)makeBackingLayer {
    _metalLayer = [CAMetalLayer layer];
    _metalLayer.contentsScale = NSScreen.mainScreen ? NSScreen.mainScreen.backingScaleFactor : 2.0;
    return _metalLayer;
}

- (CAMetalLayer *)metalLayer {
    if (!_metalLayer) {
        self.wantsLayer = YES;
    }
    return _metalLayer;
}

- (void)layout {
    [super layout];
    if (_metalLayer) {
        CGFloat scale = self.window.backingScaleFactor > 0 ? self.window.backingScaleFactor : 2.0;
        _metalLayer.contentsScale = scale;
        _metalLayer.drawableSize = CGSizeMake(self.bounds.size.width * scale, self.bounds.size.height * scale);
    }
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) {
        self.window.acceptsMouseMovedEvents = YES;
        [self.window makeFirstResponder:self];
        [self setNeedsLayout:YES];
    }
}

- (BOOL)acceptsFirstResponder { return YES; }

- (void)updatePointerFromEvent:(NSEvent *)e wheel:(int8_t)wheel {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    CGSize vsz = self.bounds.size;
    if (vsz.width < 4 || vsz.height < 4) return;

    /* Account for aspect-fit letterboxing inside fb_metal.m */
    uint32_t fw = 1024, fh = 768;
    if (hv_winvm_display_size(&fw, &fh) != 0 || !fw || !fh) { fw = 1024; fh = 768; }
    double dstAspect = vsz.width / vsz.height;
    double srcAspect = (double)fw / (double)fh;
    double rx = 0, ry = 0, rw = vsz.width, rh = vsz.height;
    if (dstAspect > srcAspect) {
        rw = vsz.height * srcAspect;
        rx = (vsz.width - rw) * 0.5;
    } else {
        rh = vsz.width / srcAspect;
        ry = (vsz.height - rh) * 0.5;
    }

    /* NSView default origin is bottom-left; flip Y for top-left [0..1] */
    double xn = (p.x - rx) / rw;
    double yn = 1.0 - ((p.y - ry) / rh);
    if (xn < 0.0) xn = 0.0; else if (xn > 1.0) xn = 1.0;
    if (yn < 0.0) yn = 0.0; else if (yn > 1.0) yn = 1.0;
    _lastX = xn;
    _lastY = yn;
    hv_winvm_tablet(xn, yn, _buttons, wheel);
}

- (void)mouseMoved:(NSEvent *)e         { [self updatePointerFromEvent:e wheel:0]; }
- (void)mouseDragged:(NSEvent *)e       { [self updatePointerFromEvent:e wheel:0]; }
- (void)rightMouseDragged:(NSEvent *)e  { [self updatePointerFromEvent:e wheel:0]; }
- (void)otherMouseDragged:(NSEvent *)e  { [self updatePointerFromEvent:e wheel:0]; }

- (void)mouseDown:(NSEvent *)e          { _buttons |= 1;  [self updatePointerFromEvent:e wheel:0]; }
- (void)mouseUp:(NSEvent *)e            { _buttons &= ~1; [self updatePointerFromEvent:e wheel:0]; }
- (void)rightMouseDown:(NSEvent *)e     { _buttons |= 2;  [self updatePointerFromEvent:e wheel:0]; }
- (void)rightMouseUp:(NSEvent *)e       { _buttons &= ~2; [self updatePointerFromEvent:e wheel:0]; }
- (void)otherMouseDown:(NSEvent *)e     { _buttons |= 4;  [self updatePointerFromEvent:e wheel:0]; }
- (void)otherMouseUp:(NSEvent *)e       { _buttons &= ~4; [self updatePointerFromEvent:e wheel:0]; }

- (void)scrollWheel:(NSEvent *)e {
    double dy = e.hasPreciseScrollingDeltas ? (e.scrollingDeltaY / 12.0) : e.scrollingDeltaY;
    int w = (int)lround(dy);
    if (w == 0 && fabs(e.scrollingDeltaY) > 0.01) w = e.scrollingDeltaY > 0 ? 1 : -1;
    if (w < -127) w = -127;
    if (w > 127) w = 127;
    if (w != 0) hv_winvm_tablet(_lastX, _lastY, _buttons, (int8_t)w);
}

- (void)sendMacKey:(uint16_t)macCode down:(int)down {
    if (macCode < 128 && kMacToLinuxKey[macCode]) {
        hv_winvm_key_linux(kMacToLinuxKey[macCode], down);
    }
}

- (void)keyDown:(NSEvent *)e {
    if (!e.isARepeat) [self sendMacKey:e.keyCode down:1];
}

- (void)keyUp:(NSEvent *)e {
    [self sendMacKey:e.keyCode down:0];
}

- (void)flagsChanged:(NSEvent *)e {
    uint16_t k = e.keyCode;
    if (k == 0x39) {
        [self sendMacKey:k down:1];
        [self sendMacKey:k down:0];
    } else if (k < 64) {
        NSUInteger bit = (NSUInteger)1 << k;
        int down = !(_heldMods & bit);
        _heldMods ^= bit;
        [self sendMacKey:k down:down];
    }
}

@end

@interface WinVmAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    NSWindow *_window;
    WinVmDisplayView *_view;
    NSTimer *_statusTimer;
    uint64_t _prevFrames;
}
@end

@implementation WinVmAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    NSRect rect = NSMakeRect(0, 0, 1280, 800);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    _window = [[NSWindow alloc] initWithContentRect:rect
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"Windows 11 ARM64 (winvm)";
    _window.delegate = self;
    [_window center];

    _view = [[WinVmDisplayView alloc] initWithFrame:rect];
    _view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _window.contentView = _view;
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    [_view layoutSubtreeIfNeeded];
    hv_winvm_set_display_layer((__bridge void *)_view.metalLayer);
    [self requestGuestSize];

    /* Menu bar shortcuts for Windows actions */
    NSMenu *menubar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [menubar addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Windows"];
    [appMenu addItemWithTitle:@"Send Ctrl+Alt+Del" action:@selector(sendCAD:) keyEquivalent:@"d"];
    [appMenu addItemWithTitle:@"ACPI Power Button (Shut Down)" action:@selector(pressPower:) keyEquivalent:@"p"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit winvm" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [NSApp setMainMenu:menubar];

    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                   repeats:YES
                                                     block:^(NSTimer * _Nonnull timer) {
        (void)timer;
        uint32_t w = 0, h = 0;
        uint64_t frames = hv_winvm_display_frames();
        uint64_t fps = frames >= self->_prevFrames ? (frames - self->_prevFrames) : 0;
        self->_prevFrames = frames;
        if (hv_winvm_display_size(&w, &h) == 0 && w && h) {
            self->_window.title = [NSString stringWithFormat:@"Windows 11 ARM64 — %ux%u (%llu fps)",
                                   w, h, (unsigned long long)fps];
        }
    }];
}

/* The guest's virtio-gpu renders at the window's size in pixels. */
- (void)requestGuestSize {
    CGFloat scale = _window.backingScaleFactor > 0 ? _window.backingScaleFactor : 2.0;
    NSSize size = _view.bounds.size;
    long w = lround(size.width * scale), h = lround(size.height * scale);
    if (w > 0 && h > 0) hv_winvm_request_display_size((uint32_t)w, (uint32_t)h);
}

/* A live resize reports every frame of the drag; the guest changes mode once it settles. */
- (void)scheduleGuestSizeRequest {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(requestGuestSize) object:nil];
    [self performSelector:@selector(requestGuestSize) withObject:nil afterDelay:0.25];
}

- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    [self scheduleGuestSizeRequest];
}

- (void)windowDidChangeBackingProperties:(NSNotification *)notification {
    (void)notification;
    [self scheduleGuestSizeRequest];
}

- (void)sendCAD:(id)sender {
    (void)sender;
    hv_winvm_send_ctrl_alt_del();
}

- (void)pressPower:(id)sender {
    (void)sender;
    hv_winvm_power_button();
}

- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    [_statusTimer invalidate];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(requestGuestSize) object:nil];
    hv_winvm_set_display_layer(NULL);
    hv_winvm_stop();
    [NSApp terminate:nil];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    hv_winvm_stop();
    hv_winvm_flush_disks();
    return NSTerminateNow;
}

@end

static void on_vm_exit_gui(int reason) {
    fprintf(stderr, "winvm: guest exited (%s)\n", hv_winvm_reason_name(reason));
    hv_winvm_flush_disks();
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s --code <code.fd> --vars <vars.fd> [options]\n"
            "  --disk <img|hvsparse>     Primary NVMe disk image (read/write)\n"
            "  --disk-size <GB>          Grow the --disk image to at least this many GB first (sparse, never\n"
            "                            shrinks; off by default; Windows then extends C: with scripts/winios_extend_c.ps1)\n"
            "  --disk1 <img|dmg>         Secondary/installer NVMe image (read-only)\n"
            "  --cpus <N>                Number of vCPUs (default: 4)\n"
            "  --ram <MB>                Guest RAM in MB (default: 4096)\n"
            "  --jit <MB>                JIT region in MB (default: 768, as on the iPhone)\n"
            "  --share <dir>             Host folder shared at http://10.0.2.4/\n"
            "  --headless                Run without Cocoa GUI window\n"
            "  --screenshot <png>        Save guest framebuffer PNG\n"
            "  --screenshot-after <sec>  Save screenshot after N seconds\n"
            "  --timeout <sec>           Auto-stop VM after N seconds\n"
            "  --cmd-file <path>         Tail input commands from file (verbs below)\n"
            "  --perf-csv <path>         Append a row of performance counters to path (host/perf.h)\n"
            "  --perf-ms <n>             Milliseconds between perf rows (default: 1000); one more row when the VM stops\n"
            "  --no-net                  Disable SLIRP networking\n"
            "  --sound                   Enable VirtIO sound output\n"
            "  --stats <sec>             Print JIT statistics every N seconds\n"
            "  --log <level>             Log level (default: 0)\n"
            "\n"
            "cmd-file verbs, one per line ('#' starts a comment); each is echoed to stderr as\n"
            "'[<ms since start>] winvm: cmd: <line>', the time being when it began:\n"
            "  click|rclick|move <x> <y>       Pointer at x,y in 0..1 (click and right-click press once)\n"
            "  tap <x> <y>                     Touch tap\n"
            "  press|pressrepeat <x> <y> <ms>  Touch and hold (pressrepeat re-sends the held frame)\n"
            "  type <text>                     Type ASCII text\n"
            "  hidkey <usage>                  Press a USB HID key; combo <usage> <usage>: chord\n"
            "  win | winr | enter              Windows key, Win+R, Enter\n"
            "  size <w> <h>                    Ask the guest display for w x h pixels\n"
            "  posture laptop|tablet           Tablet mode follows this posture\n"
            "  cad | power | hibernate         Ctrl+Alt+Del, ACPI power button, hibernate\n"
            "  screenshot <png>                Save the guest framebuffer\n"
            "  state <path>                    Save every vCPU's registers and recent exceptions\n"
            "  cpus | touchready | oauthmode <0|1> | oauthstatus   Debug dumps\n"
            "  wait <ms>                       Pause the script\n"
            "  quit                            Stop the VM\n"
            "  mark <label>                    Write '#mark <t_ms> <label>' to the perf CSV (if open) and\n"
            "                                  print 'MARK <t_ms> <label>' to stdout (t_ms: wall-clock ms,\n"
            "                                  the t_ms column of the CSV rows)\n"
            "  settle <quiet_ms> <max_ms>      Wait until the virtio-gpu flush count has not changed for\n"
            "                                  quiet_ms, or max_ms have passed; print 'SETTLE <elapsed_ms>\n"
            "                                  <quiet|timeout>' to stdout and mark it\n"
            "  perfrow                         Write a perf row now\n",
            prog);
    exit(2);
}

int main(int argc, char **argv) {
    HvWinVmConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.ncpus = 4;
    cfg.ram_mb = 4096;

    int headless = 0;
    uint64_t disk_gb = 0;
    TimerArgs targs = {NULL, 0, 0};
    const char *cmd_file = getenv("HV_INPUT_FILE");
    const char *perf_path = NULL;
    g_mono_zero = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    char share_sock_buf[512] = {0};

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) { headless = 1; continue; }
        if (!strcmp(argv[i], "--no-net"))   { cfg.no_net = 1; continue; }
        if (!strcmp(argv[i], "--sound"))    { cfg.enable_sound = 1; continue; }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) usage(argv[0]);
        if (i + 1 >= argc) usage(argv[0]);

        if (!strcmp(argv[i], "--code"))              cfg.code_fd = argv[++i];
        else if (!strcmp(argv[i], "--vars"))         cfg.vars_fd = argv[++i];
        else if (!strcmp(argv[i], "--disk"))         cfg.disk0 = argv[++i];
        else if (!strcmp(argv[i], "--disk-size")) {
            disk_gb = strtoull(argv[++i], NULL, 0);
            if (disk_gb > 16384) usage(argv[0]);
        }
        else if (!strcmp(argv[i], "--disk1"))        cfg.disk1 = argv[++i];
        else if (!strcmp(argv[i], "--cpus"))         cfg.ncpus = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ram"))          cfg.ram_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--jit"))          cfg.jit_bytes = (size_t)strtoull(argv[++i], NULL, 0) << 20;
        else if (!strcmp(argv[i], "--share"))        cfg.share_dir = argv[++i];
        else if (!strcmp(argv[i], "--screenshot"))   targs.screenshot_path = argv[++i];
        else if (!strcmp(argv[i], "--screenshot-after")) targs.screenshot_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout"))      targs.timeout_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cmd-file"))     cmd_file = argv[++i];
        else if (!strcmp(argv[i], "--stats"))        cfg.stats_interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log"))          cfg.log = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--perf-csv"))     perf_path = argv[++i];
        else if (!strcmp(argv[i], "--perf-ms")) {
            int ms = atoi(argv[++i]);
            g_perf_ms = ms > 0 ? ms : 1000;
        }
        else usage(argv[0]);
    }

    if (!cfg.code_fd || !cfg.vars_fd) usage(argv[0]);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (cfg.share_dir && *cfg.share_dir) {
        snprintf(share_sock_buf, sizeof share_sock_buf, "%s/.winvm_webdav.sock", cfg.share_dir);
        cfg.share_sock = share_sock_buf;
    }

    if (disk_gb && cfg.disk0 && hv_disk_grow(cfg.disk0, disk_gb << 30) < 0) {
        fprintf(stderr, "winvm: cannot grow %s to %llu GB\n", cfg.disk0, (unsigned long long)disk_gb);
        return 1;
    }

    char err[256] = {0};
    if (hv_winvm_create(&cfg, err, sizeof err) != 0) {
        fprintf(stderr, "winvm: failed to create VM: %s\n", err);
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (perf_path && *perf_path && perf_start(perf_path, &cfg) != 0) {
        fprintf(stderr, "winvm: cannot open perf csv %s\n", perf_path);
        return 1;
    }

    if (targs.screenshot_path || targs.timeout_sec > 0) {
        pthread_t tt;
        pthread_create(&tt, NULL, timer_thread, &targs);
        pthread_detach(tt);
    }

    if (cmd_file && *cmd_file) {
        pthread_t ct;
        pthread_create(&ct, NULL, cmd_file_thread, (void *)cmd_file);
        pthread_detach(ct);
    }

    if (headless) {
        fprintf(stderr, "winvm: starting headless (%d vCPUs, %llu MB RAM)...\n",
                cfg.ncpus, (unsigned long long)cfg.ram_mb);
        hv_winvm_start(0, NULL);
        int r = hv_winvm_run_cpu0();
        fprintf(stderr, "\nwinvm: stopped (%s)\n", g_sig_stop ? "interrupted" : hv_winvm_reason_name(r));
        if (targs.screenshot_path && targs.screenshot_after == 0) {
            int rc = hv_winvm_save_png(targs.screenshot_path);
            fprintf(stderr, "winvm: saved screenshot %s (rc=%d)\n", targs.screenshot_path, rc);
        }
        hv_winvm_flush_disks();
        return 0;
    }

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        WinVmAppDelegate *del = [[WinVmAppDelegate alloc] init];
        app.delegate = del;
        hv_winvm_start(1, on_vm_exit_gui);
        [app run];
    }
    return 0;
}
