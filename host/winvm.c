/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "winvm.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <TargetConditionals.h>
#include <os/lock.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "audio_mac.h"
#include "dispatch.h"
#include "net_slirp.h"
#include "share.h"

static HvMachine m;
static HvCpu cpus[HV_MAX_CPUS];
static pthread_t sec_threads[HV_MAX_CPUS];
static int ncpus = 1;
static HvWinVirt board;
static HvDisk disk0 = {.fd = -1};
static HvDisk disk1 = {.fd = -1};
static int net_on;
static int share_on;
static _Atomic int stop_requested;

static os_unfair_lock display_lock = OS_UNFAIR_LOCK_INIT;
static CFTypeRef display_layer;
static int display_layer_set;
static _Atomic(HvFbDisplay *) display_fb;

static void (*exit_cb)(int reason);

int hv_winvm_share_up(void) { return share_on; }
HvMachine *hv_winvm_machine(void) { return &m; }
HvWinVirt *hv_winvm_board(void) { return &board; }

const char *hv_winvm_reason_name(int r) {
    switch (r) {
    case HV_EXIT_HALT: return "halt";
    case HV_EXIT_WFI: return "wfi with nothing to wake it";
    case HV_EXIT_UNSUPPORTED: return "unsupported instruction";
    case HV_EXIT_FAULT_LOOP: return "fault loop on exception vector";
    default: return "other";
    }
}

static void board_poll(void) {
    if (net_on) hv_net_poll();
}

static int fail(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
    return -1;
}

static struct sigaction prior_ill, prior_bus, prior_segv;
static void on_ill(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = ctx;
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    uint64_t rx = (uint64_t)(uintptr_t)m.jit.base;
    uint64_t size = (uint64_t)m.jit.capacity * 4;
    int in_rx = pc >= rx && pc < rx + size;
    if (sig == SIGILL || in_rx) {
        fprintf(stderr, "winvm: signal %d in JIT: host_pc=%llx (rx=%llx +%lld) addr=%p\n",
                sig, (unsigned long long)pc, (unsigned long long)rx, (long long)(pc - rx), info->si_addr);
        for (int i = 0; i < ncpus; i++) {
            fprintf(stderr, "  cpu %d: guest_pc=%llx el=%u sctlr=%llx\n",
                    i, (unsigned long long)cpus[i].pc, cpus[i].el, (unsigned long long)cpus[i].sctlr_el1);
        }
        fflush(stderr);
    }
    struct sigaction *prior = sig == SIGILL ? &prior_ill : sig == SIGBUS ? &prior_bus : &prior_segv;
    sigaction(sig, prior, NULL);
}

static void prepare_cpu_thread(void) {
    static __thread int done;
    if (done) return;
    done = 1;
    stack_t ss = {.ss_sp = malloc(1u << 16), .ss_size = 1u << 16, .ss_flags = 0};
    if (ss.ss_sp) sigaltstack(&ss, NULL);
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGSEGV);
    sigdelset(&set, SIGBUS);
    sigdelset(&set, SIGILL);
    sigdelset(&set, SIGTRAP);
    sigdelset(&set, SIGFPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    const char *q = getenv("HV_QOS");
#if TARGET_OS_IPHONE
    /* Schedule guest vCPUs on P-cores while staying strictly below UI (USER_INTERACTIVE) */
    qos_class_t c = QOS_CLASS_USER_INITIATED;
#else
    qos_class_t c = QOS_CLASS_USER_INTERACTIVE;
#endif
    if (q && !strcmp(q, "ui")) c = QOS_CLASS_USER_INTERACTIVE;
    else if (q && !strcmp(q, "init")) c = QOS_CLASS_USER_INITIATED;
    else if (q && !strcmp(q, "default")) c = QOS_CLASS_DEFAULT;
    if (c != QOS_CLASS_DEFAULT) pthread_set_qos_class_self_np(c, 0);
}

void hv_winvm_set_display_layer(void *layer) {
    CFTypeRef retained = layer ? CFRetain(layer) : NULL;
    os_unfair_lock_lock(&display_lock);
    CFTypeRef previous = display_layer;
    display_layer = retained;
    display_layer_set = 1;
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    if (fb) hv_fb_metal_set_layer(fb, layer);
    os_unfair_lock_unlock(&display_lock);
    if (previous) CFRelease(previous);
}

int hv_winvm_display_size(uint32_t *w, uint32_t *h) {
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    if (!fb) return -1;
    const uint8_t *px = NULL;
    uint32_t fw = 0, fh = 0, st = 0;
    if (hv_fb_display_get_frame(fb, &px, &fw, &fh, &st) != 0) return -1;
    if (w) *w = fw;
    if (h) *h = fh;
    return 0;
}

uint64_t hv_winvm_display_frames(void) {
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    return fb ? hv_fb_metal_frames(fb) : 0;
}

int hv_winvm_save_png(const char *path) {
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    return fb ? hv_fb_save_png(fb, path) : -1;
}

static void kicker_timer_hint(HvMachine *mm, uint64_t due);

static uint32_t pending_width, pending_height;

void hv_winvm_request_display_size(uint32_t width, uint32_t height) {
    os_unfair_lock_lock(&display_lock);
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    if (!fb) {
        pending_width = width;
        pending_height = height;
    }
    os_unfair_lock_unlock(&display_lock);
    if (fb) hv_fb_display_request_size(fb, width, height);
}

int hv_winvm_create(const HvWinVmConfig *c, char *err, size_t errlen) {
    if (!c || !c->code_fd || !c->vars_fd)
        return fail(err, errlen, "UEFI firmware (code.fd) and NVRAM (vars.fd) are required.");

    atomic_store(&stop_requested, 0);

    struct sigaction ill = {.sa_flags = SA_SIGINFO | SA_ONSTACK};
    ill.sa_sigaction = on_ill;
    sigaction(SIGILL, &ill, &prior_ill);
    sigaction(SIGBUS, &ill, &prior_bus);
    sigaction(SIGSEGV, &ill, &prior_segv);
    signal(SIGPIPE, SIG_IGN);

    HvDisk *d0_ptr = NULL, *d1_ptr = NULL;
    if (c->disk0 && *c->disk0) {
        if (hv_disk_open(&disk0, c->disk0, 0, 0) != 0)
            return fail(err, errlen, "Cannot open primary NVMe disk image.");
        d0_ptr = &disk0;
    }
    if (c->disk1 && *c->disk1) {
        if (hv_disk_open(&disk1, c->disk1, 0, 1) != 0)
            return fail(err, errlen, "Cannot open secondary/installer disk image.");
        d1_ptr = &disk1;
    }

    size_t jit = c->jit_bytes ? c->jit_bytes : ((size_t)768 << 20);
    int r = c->jit_rx ? hv_machine_init_jit(&m, c->jit_rx, c->jit_rw, jit) : hv_machine_init(&m, jit);
    if (r != 0) return fail(err, errlen, "Cannot initialize ARM64 JIT region (is JIT active?).");

#if TARGET_OS_IPHONE
    /* Defer synchronous guest NVMe FLUSH fsyncs on iOS APFS NAND; explicit host checkpoints call hv_disk_sync(). */
    if (!getenv("HV_FLUSH")) hv_knob[HV_K_FLUSH] = 1;
#endif

    m.log = c->log;
    m.stats_interval = c->stats_interval;
    m.timer_hint = kicker_timer_hint;
    if (getenv("HV_SLOW_MEM")) m.slow_mem = 1;
    ncpus = c->ncpus < 1 ? 1 : c->ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : c->ncpus;
    m.big_lock_on = ncpus > 1 && getenv("HV_BIG_LOCK") != NULL;

    uint64_t ram_bytes = (c->ram_mb ? c->ram_mb : 4096ULL) << 20;
    static const uint8_t mac[6] = {0x02, 0x57, 0x49, 0x4e, 0x00, 0x01};
    if (hv_winvirt_create(&board, &m, cpus, ncpus, ram_bytes, c->prealloc_ram, c->code_fd, c->vars_fd,
                          d0_ptr, d1_ptr, mac, c->enable_sound) != 0) {
        return fail(err, errlen, "Cannot create virtual ARM64 Windows board (insufficient RAM or missing firmware).");
    }
    hv_winvirt_reset_cpu(&board);

    share_on = 0;
    if (c->share_dir && *c->share_dir && c->share_sock && *c->share_sock) {
        if (hv_share_start(c->share_dir, c->share_sock, NULL) == 0) {
            hv_net_set_share_socket(c->share_sock);
            share_on = 1;
        }
    }
    if (board.net && !c->no_net) {
        if (hv_net_attach(&m, board.net) == 0) net_on = 1;
        else fprintf(stderr, "winvm: user-mode SLIRP networking unavailable\n");
    }
    if (board.hda && c->enable_sound) hv_audio_attach_hda(board.hda);
    board.poll = board_poll;

    os_unfair_lock_lock(&display_lock);
    if (display_layer_set && board.fb) hv_fb_metal_set_layer(board.fb, (void *)display_layer);
    atomic_store_explicit(&display_fb, board.fb, memory_order_release);
    uint32_t want_w = pending_width, want_h = pending_height;
    os_unfair_lock_unlock(&display_lock);
    if (want_w && board.fb) hv_fb_display_request_size(board.fb, want_w, want_h);

    return 0;
}

/* The kicker makes running CPUs leave translated code: every KICK_PERIOD so CPU 0
   keeps polling devices, and exactly when a CPU's guest timer falls due so timer
   interrupts are not quantized to that period. mach_absolute_time runs at 24 MHz. */
#if TARGET_OS_IPHONE
#define KICK_PERIOD_TICKS 60000ull /* 2.5 ms */
#else
#define KICK_PERIOD_TICKS 24000ull /* 1 ms */
#endif

static pthread_mutex_t kick_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kick_cv = PTHREAD_COND_INITIALIZER;
/* The host time the kicker sleeps until, or 0 while it plans (not waiting). Written under kick_mu. */
static _Atomic uint64_t kick_wake_at;

static void kicker_timer_hint(HvMachine *mm, uint64_t due) {
    (void)mm;
    uint64_t w = atomic_load_explicit(&kick_wake_at, memory_order_acquire);
    if (w && due >= w) return; /* the kicker wakes first and re-plans with this deadline */
    /* Holding kick_mu orders this after the kicker's plan: it either waits already
       (and this wakes it) or has not read timer_due yet. */
    pthread_mutex_lock(&kick_mu);
    pthread_cond_signal(&kick_cv);
    pthread_mutex_unlock(&kick_mu);
}

static void *kicker_thread(void *unused) {
    (void)unused;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    uint64_t next_all = mach_absolute_time() + KICK_PERIOD_TICKS;
    pthread_mutex_lock(&kick_mu);
    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        uint64_t now = mach_absolute_time();
        uint64_t wake = next_all;
        for (int i = 0; i < ncpus; i++) {
            uint64_t due = __atomic_load_n(&cpus[i].timer_due, __ATOMIC_RELAXED);
            if (due > now && due < wake) wake = due;
        }
        if (wake > now) {
            atomic_store_explicit(&kick_wake_at, wake, memory_order_release);
            uint64_t d = wake - now;
            struct timespec ts = {(time_t)(d / 24000000), (long)(d % 24000000) * 1000 / 24};
            pthread_cond_timedwait_relative_np(&kick_cv, &kick_mu, &ts);
            atomic_store_explicit(&kick_wake_at, 0, memory_order_release);
        }
        now = mach_absolute_time();
        if (now >= next_all) {
            next_all = now + KICK_PERIOD_TICKS;
            hv_machine_kick_all(&m);
            continue;
        }
        /* A CPU parked in WFI keeps a stale deadline; kicking it only sets a flag, and
           it times its own sleep. */
        int kicked = 0;
        for (int i = 0; i < ncpus; i++) {
            uint64_t due = __atomic_load_n(&cpus[i].timer_due, __ATOMIC_RELAXED);
            if (due && due <= now) {
                __atomic_store_n(&cpus[i].exit_req, 1, __ATOMIC_SEQ_CST);
                kicked = 1;
            }
        }
        if (kicked) __asm__ volatile("sev" ::: "memory");
    }
    pthread_mutex_unlock(&kick_mu);
    return NULL;
}

static void *secondary_thread(void *arg) {
    HvCpu *c = arg;
    prepare_cpu_thread();
    int r = hv_run(c);
    if (!c->halted) {
        fprintf(stderr, "winvm: cpu %u stopped (%s) at pc=%llx el=%u\n",
                c->index, hv_winvm_reason_name(r), (unsigned long long)c->pc, c->el);
    }
    return NULL;
}

static void spawn_secondaries(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    for (int i = 1; i < ncpus; i++) {
        pthread_create(&sec_threads[i], &attr, secondary_thread, &cpus[i]);
    }
    pthread_attr_destroy(&attr);
}

static void join_secondaries(void) {
    for (int i = 1; i < ncpus; i++) {
        cpus[i].halted = 1;
        hv_cpu_wake(&cpus[i]);
    }
    hv_machine_kick_all(&m);
    for (int i = 1; i < ncpus; i++) {
        if (sec_threads[i]) {
            pthread_join(sec_threads[i], NULL);
            sec_threads[i] = 0;
        }
    }
}

void hv_winvm_flush_disks(void) {
    if (disk0.fd >= 0) hv_disk_sync(&disk0);
}

void hv_winvm_checkpoint_disks(void) {
    if (disk0.fd >= 0) hv_disk_barrier(&disk0);
}

int hv_winvm_run_cpu0(void) {
    prepare_cpu_thread();
    for (;;) {
        int r = hv_run(&cpus[0]);
        hv_winvm_flush_disks();
        /* PSCI SYSTEM_RESET sets halted == 2: perform warm reboot without tearing down RAM/JIT! */
        if (cpus[0].halted == 2 && !atomic_load(&stop_requested)) {
            fprintf(stderr, "winvm: guest requested PSCI SYSTEM_RESET — rebooting...\n");
            join_secondaries();
            /* Reset GIC, ITS, PCIe, and pflash state for warm reboot */
            if (board.gic) {
                pthread_mutex_lock(&board.gic->mu);
                board.gic->ctlr = 0;
                memset(board.gic->enabled, 0, sizeof board.gic->enabled);
                memset(board.gic->latch, 0, sizeof board.gic->latch);
                memset(board.gic->level, 0, sizeof board.gic->level);
                memset(board.gic->active, 0, sizeof board.gic->active);
                memset(board.gic->spi_on, 0, sizeof board.gic->spi_on);
                for (int c = 0; c < HV_MAX_CPUS; c++) {
                    memset(&board.gic->cpu[c], 0, sizeof board.gic->cpu[c]);
                    board.gic->cpu[c].waker = 6;
                    for (int q = 0; q < 16; q++) board.gic->cpu[c].edge[q] = 1;
                    m.percpu[c].irq_line = 0;
                    m.percpu[c].fiq_line = 0;
                }
                pthread_mutex_unlock(&board.gic->mu);
            }
            hv_gicv3_its_reset(board.its);
            hv_pcie_reset(board.pcie);
            if (board.flash1) {
                board.flash1->cmd = 0xff;
                board.flash1->wcycle = 0;
                board.flash1->status = 0x80;
            }
            if (board.fb) board.fb->active = 0;
            if (board.gpu) hv_virtio_gpu_reset(board.gpu); /* the scanout must not outlive the guest that set it */
            hv_winvirt_reset_cpu(&board);
            m.flush_pending = 1;
            spawn_secondaries();
            continue;
        }
        return r;
    }
}

static void *cpu0_thread(void *unused) {
    (void)unused;
    int r = hv_winvm_run_cpu0();
    atomic_store(&stop_requested, 1);
    join_secondaries();
    if (exit_cb) exit_cb(r);
    return NULL;
}

void hv_winvm_start(int spawn_cpu0, void (*on_exit)(int reason)) {
    exit_cb = on_exit;
    spawn_secondaries();
    pthread_t kt;
    pthread_create(&kt, NULL, kicker_thread, NULL);
    pthread_detach(kt);
    if (spawn_cpu0) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        pthread_t ct;
        pthread_create(&ct, &attr, cpu0_thread, NULL);
        pthread_detach(ct);
        pthread_attr_destroy(&attr);
    }
}

void hv_winvm_stop(void) {
    atomic_store(&stop_requested, 1);
    hv_audio_shutdown();
    for (int i = 0; i < ncpus; i++) cpus[i].halted = 3;
    hv_machine_kick_all(&m);
    for (int i = 0; i < ncpus; i++) hv_cpu_wake(&cpus[i]);
    hv_winvm_flush_disks();
}

void hv_winvm_power_button(void) {
    hv_winvirt_power_button(&board);
}

void hv_winvm_set_posture(int laptop) {
    hv_share_set_posture(laptop);
}

void hv_winvm_key_hid(uint8_t usage, int down) {
    if (board.usb) hv_xhci_key(board.usb, usage, down);
}

void hv_winvm_key_linux(uint16_t code, int down) {
    if (board.usb) hv_xhci_key_linux(board.usb, code, down);
}

void hv_winvm_tablet(double x_norm, double y_norm, uint8_t buttons, int8_t wheel) {
    if (!board.usb) return;
    if (x_norm < 0.0) x_norm = 0.0;
    if (x_norm > 1.0) x_norm = 1.0;
    if (y_norm < 0.0) y_norm = 0.0;
    if (y_norm > 1.0) y_norm = 1.0;
    int ax = (int)(x_norm * 32767.0 + 0.5);
    int ay = (int)(y_norm * 32767.0 + 0.5);
    hv_xhci_tablet(board.usb, ax, ay, buttons, wheel);
}

void hv_winvm_touch(const HvTouchContact *contacts, int count) {
    if (board.usb) hv_xhci_touch(board.usb, contacts, count);
}

int hv_winvm_touch_ready(void) {
    return board.usb ? hv_xhci_touch_ready(board.usb) : 0;
}

void hv_winvm_send_ctrl_alt_del(void) {
    if (!board.usb) return;
    /* Left Ctrl (0xe0), Left Alt (0xe2), Delete (0x4c) */
    hv_xhci_key(board.usb, 0xe0, 1);
    hv_xhci_key(board.usb, 0xe2, 1);
    hv_xhci_key(board.usb, 0x4c, 1);
    hv_xhci_key(board.usb, 0x4c, 0);
    hv_xhci_key(board.usb, 0xe2, 0);
    hv_xhci_key(board.usb, 0xe0, 0);
}

void hv_winvm_usb_stats(int *kbd_ready, int *tab_ready, uint64_t *delivered) {
    HvXhci *x = board.usb;
    if (!x) {
        if (kbd_ready) *kbd_ready = 0;
        if (tab_ready) *tab_ready = 0;
        if (delivered) *delivered = 0;
        return;
    }
    pthread_mutex_lock(&x->mu);
    int ks = x->kbd_slot;
    int ts = x->tab_slot;
    if (kbd_ready) *kbd_ready = (ks >= 1 && ks <= HV_XHCI_SLOTS && x->slots[ks].ep_running[3]) ? 1 : 0;
    if (tab_ready) *tab_ready = (ts >= 1 && ts <= HV_XHCI_SLOTS && x->slots[ts].ep_running[3]) ? 1 : 0;
    if (delivered) *delivered = x->delivered;
    pthread_mutex_unlock(&x->mu);
}

void hv_winvm_engine_stats(uint64_t *jit_used_bytes, uint64_t *jit_cap_bytes,
                           uint64_t *jit_full_flushes, uint64_t *wfi_ticks) {
    if (jit_used_bytes) *jit_used_bytes = (uint64_t)m.jit.used * 4ull;
    if (jit_cap_bytes) *jit_cap_bytes = (uint64_t)m.jit.capacity * 4ull;
    if (jit_full_flushes) *jit_full_flushes = m.stats.flush_full;
    if (wfi_ticks) *wfi_ticks = m.stats.wfi_ticks;
}
