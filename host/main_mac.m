/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * main_mac.m — macOS Cocoa + Headless runner for ARM64 Windows 11 (`winvm`).
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "winvm.h"
#include "mmu.h"
#include "share.h"

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

/* Simple ASCII typer using USB HID Keyboard usages */
static void type_ascii_char(char ch) {
    uint8_t usage = 0;
    int shift = 0;
    if (ch >= 'a' && ch <= 'z') { usage = (uint8_t)(0x04 + (ch - 'a')); }
    else if (ch >= 'A' && ch <= 'Z') { usage = (uint8_t)(0x04 + (ch - 'A')); shift = 1; }
    else if (ch >= '1' && ch <= '9') { usage = (uint8_t)(0x1e + (ch - '1')); }
    else if (ch == '0') { usage = 0x27; }
    else {
        switch (ch) {
        case ' ': usage = 0x2c; break;
        case '\n': case '\r': usage = 0x28; break;
        case '\t': usage = 0x2b; break;
        case '-': usage = 0x2d; break;
        case '_': usage = 0x2d; shift = 1; break;
        case '=': usage = 0x2e; break;
        case '+': usage = 0x2e; shift = 1; break;
        case '[': usage = 0x2f; break;
        case '{': usage = 0x2f; shift = 1; break;
        case ']': usage = 0x30; break;
        case '}': usage = 0x30; shift = 1; break;
        case '\\': usage = 0x31; break;
        case '|': usage = 0x31; shift = 1; break;
        case ';': usage = 0x33; break;
        case ':': usage = 0x33; shift = 1; break;
        case '\'': usage = 0x34; break;
        case '"': usage = 0x34; shift = 1; break;
        case '`': usage = 0x35; break;
        case '~': usage = 0x35; shift = 1; break;
        case ',': usage = 0x36; break;
        case '<': usage = 0x36; shift = 1; break;
        case '.': usage = 0x37; break;
        case '>': usage = 0x37; shift = 1; break;
        case '/': usage = 0x38; break;
        case '?': usage = 0x38; shift = 1; break;
        case '!': usage = 0x1e; shift = 1; break;
        case '@': usage = 0x1f; shift = 1; break;
        case '#': usage = 0x20; shift = 1; break;
        case '$': usage = 0x21; shift = 1; break;
        case '%': usage = 0x22; shift = 1; break;
        case '^': usage = 0x23; shift = 1; break;
        case '&': usage = 0x24; shift = 1; break;
        case '*': usage = 0x25; shift = 1; break;
        case '(': usage = 0x26; shift = 1; break;
        case ')': usage = 0x27; shift = 1; break;
        default: return;
        }
    }
    if (shift) { hv_winvm_key_hid(0xe1, 1); usleep(20000); }
    hv_winvm_key_hid(usage, 1);
    usleep(35000);
    hv_winvm_key_hid(usage, 0);
    if (shift) { usleep(20000); hv_winvm_key_hid(0xe1, 0); }
    usleep(35000);
}

static void *cmd_file_thread(void *arg) {
    const char *path = (const char *)arg;
    FILE *f = fopen(path, "a+");
    if (!f) return NULL;
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

        double xn = 0, yn = 0;
        int code = 0, yi = 0;
        if (sscanf(line, "click %lf %lf", &xn, &yn) == 2) {
            hv_winvm_tablet(xn, yn, 0, 0);
            usleep(40000);
            hv_winvm_tablet(xn, yn, 1, 0);
            usleep(80000);
            hv_winvm_tablet(xn, yn, 0, 0);
        } else if (sscanf(line, "rclick %lf %lf", &xn, &yn) == 2) {
            hv_winvm_tablet(xn, yn, 0, 0);
            usleep(40000);
            hv_winvm_tablet(xn, yn, 2, 0);
            usleep(80000);
            hv_winvm_tablet(xn, yn, 0, 0);
        } else if (sscanf(line, "move %lf %lf", &xn, &yn) == 2) {
            hv_winvm_tablet(xn, yn, 0, 0);
        } else if (sscanf(line, "hidkey %i", &code) == 1) {
            hv_winvm_key_hid((uint8_t)code, 1);
            usleep(50000);
            hv_winvm_key_hid((uint8_t)code, 0);
        } else if (!strcmp(line, "win")) {
            hv_winvm_key_hid(0xe3, 1);
            usleep(60000);
            hv_winvm_key_hid(0xe3, 0);
        } else if (!strcmp(line, "winr")) {
            hv_winvm_key_hid(0xe3, 1);
            usleep(30000);
            hv_winvm_key_hid(0x15, 1); /* 'r' */
            usleep(50000);
            hv_winvm_key_hid(0x15, 0);
            usleep(30000);
            hv_winvm_key_hid(0xe3, 0);
        } else if (!strcmp(line, "enter")) {
            hv_winvm_key_hid(0x28, 1);
            usleep(50000);
            hv_winvm_key_hid(0x28, 0);
        } else if (sscanf(line, "size %d %d", &code, &yi) == 2 && code > 0 && yi > 0) {
            hv_winvm_request_display_size((uint32_t)code, (uint32_t)yi);
        } else if (!strcmp(line, "cad")) {
            hv_winvm_send_ctrl_alt_del();
        } else if (!strcmp(line, "power")) {
            hv_winvm_power_button();
        } else if (!strncmp(line, "type ", 5)) {
            for (const char *p = line + 5; *p; p++) type_ascii_char(*p);
        } else if (!strncmp(line, "screenshot ", 11)) {
            int rc = hv_winvm_save_png(line + 11);
            fprintf(stderr, "winvm: screenshot -> %s (rc=%d)\n", line + 11, rc);
        } else if (!strcmp(line, "cpus")) {
            HvMachine *m = hv_winvm_machine();
            for (int i = 0; i < m->ncpus; i++)
                if (m->cpus[i]) __atomic_store_n(&m->cpus[i]->exit_req, 1, __ATOMIC_SEQ_CST);
            usleep(4000);
            for (int i = 0; i < m->ncpus; i++) {
                HvCpu *c = m->cpus[i];
                if (!c) continue;
                uint64_t pc = c->pc;
                fprintf(stderr, "CPU%d: pc=%llx el=%u exit=%u x0=%llx x1=%llx x2=%llx x3=%llx x8=%llx x16=%llx lr=%llx\n",
                        i, (unsigned long long)pc, c->el, c->exit_reason,
                        (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                        (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                        (unsigned long long)c->x[8], (unsigned long long)c->x[16],
                        (unsigned long long)c->x[30]);
                if (pc >= 0x1000) {
                    fprintf(stderr, "  insns:");
                    for (int off = -32; off <= 32; off += 4) {
                        uint64_t va = (pc & ~3ull) + (int64_t)off;
                        HvWalk w = {0};
                        hv_mmu_walk(c, va, HV_ACC_READ, c->el, &w);
                        uint32_t word = 0;
                        if (w.fsc == 0) hv_phys_read(m, w.pa, &word, 4);
                        fprintf(stderr, " %s%08x", off == 0 ? "*" : "", word);
                    }
                    fprintf(stderr, "\n");
                }
            }
            fflush(stderr);
        } else if (sscanf(line, "oauthmode %d", &code) == 1) {
            hv_share_set_url_handler_enabled(code);
            fprintf(stderr, "winvm: oauthmode=%d\n", code);
        } else if (!strcmp(line, "oauthstatus")) {
            char *u = hv_share_take_open_url();
            int done = hv_share_take_oauth_done();
            fprintf(stderr, "OAUTH_STATUS url=%s done=%d\n", u ? u : "(none)", done);
            free(u);
        } else if (sscanf(line, "wait %d", &code) == 1 && code > 0) {
            usleep((useconds_t)code * 1000u);
        } else if (!strcmp(line, "quit")) {
            hv_winvm_stop();
            break;
        }
        fprintf(stderr, "winvm: cmd: %s\n", line);
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
            "  --disk1 <img|dmg>         Secondary/installer NVMe image (read-only)\n"
            "  --cpus <N>                Number of vCPUs (default: 4)\n"
            "  --ram <MB>                Guest RAM in MB (default: 4096)\n"
            "  --share <dir>             Host folder shared at http://10.0.2.4/\n"
            "  --headless                Run without Cocoa GUI window\n"
            "  --screenshot <png>        Save guest framebuffer PNG\n"
            "  --screenshot-after <sec>  Save screenshot after N seconds\n"
            "  --timeout <sec>           Auto-stop VM after N seconds\n"
            "  --cmd-file <path>         Tail input commands from file\n"
            "  --no-net                  Disable SLIRP networking\n"
            "  --sound                   Enable VirtIO sound output\n"
            "  --stats <sec>             Print JIT statistics every N seconds\n"
            "  --log <level>             Log level (default: 0)\n",
            prog);
    exit(2);
}

int main(int argc, char **argv) {
    HvWinVmConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.ncpus = 4;
    cfg.ram_mb = 4096;

    int headless = 0;
    TimerArgs targs = {NULL, 0, 0};
    const char *cmd_file = getenv("HV_INPUT_FILE");
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
        else if (!strcmp(argv[i], "--disk1"))        cfg.disk1 = argv[++i];
        else if (!strcmp(argv[i], "--cpus"))         cfg.ncpus = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ram"))          cfg.ram_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--share"))        cfg.share_dir = argv[++i];
        else if (!strcmp(argv[i], "--screenshot"))   targs.screenshot_path = argv[++i];
        else if (!strcmp(argv[i], "--screenshot-after")) targs.screenshot_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout"))      targs.timeout_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cmd-file"))     cmd_file = argv[++i];
        else if (!strcmp(argv[i], "--stats"))        cfg.stats_interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log"))          cfg.log = atoi(argv[++i]);
        else usage(argv[0]);
    }

    if (!cfg.code_fd || !cfg.vars_fd) usage(argv[0]);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (cfg.share_dir && *cfg.share_dir) {
        snprintf(share_sock_buf, sizeof share_sock_buf, "%s/.winvm_webdav.sock", cfg.share_dir);
        cfg.share_sock = share_sock_buf;
    }

    char err[256] = {0};
    if (hv_winvm_create(&cfg, err, sizeof err) != 0) {
        fprintf(stderr, "winvm: failed to create VM: %s\n", err);
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

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
