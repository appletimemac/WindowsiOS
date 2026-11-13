/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "winvm.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
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
#include "mmu.h"
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
static double display_content[4]; /* the picture's place in the layer; no area: all of it */
static _Atomic(HvFbDisplay *) display_fb;

static void (*exit_cb)(int reason);

int hv_winvm_share_up(void) { return share_on; }
HvMachine *hv_winvm_machine(void) { return &m; }

/* What each device would raise an interrupt for, read without the devices' locks: a diagnostic of a machine
   that is stuck, where a torn value does not matter. */
static void dump_virtio(FILE *f, const char *name, const HvVirtio *v) {
    fprintf(f, "%s: status=%02x isr=%u driver_ok=%d", name, v->status, v->isr, (v->status & 4) != 0);
    for (int q = 0; q < v->nq; q++) {
        uint16_t flags = 0xffff;
        const HvVirtq *vq = &v->q[q];
        if (vq->enabled && vq->avail) hv_phys_read(v->m, vq->avail, &flags, 2);
        fprintf(f, " q%d: enabled=%u avail_flags=%x", q, vq->enabled, flags);
    }
    fprintf(f, "\n");
}

void hv_winvm_dump_devices(FILE *f) {
    if (board.pcie) hv_pcie_dump(board.pcie, f);
    if (board.net) dump_virtio(f, "virtio-net", &board.net->vio);
    if (board.gpu) dump_virtio(f, "virtio-gpu", &board.gpu->vio);
    if (board.hda) {
        const HvHda *d = board.hda;
        fprintf(f, "hda: gctl=%x intctl=%x wakeen=%x statests=%x rirbctl=%x rirbsts=%x", d->gctl, d->intctl, d->wakeen, d->statests, d->rirbctl, d->rirbsts);
        for (int s = 0; s < HV_HDA_NSTREAMS; s++)
            if (d->st[s].ctl || d->st[s].sts || d->st[s].running)
                fprintf(f, " stream%d: ctl=%x sts=%x running=%d lpib=%x", s, d->st[s].ctl, d->st[s].sts, d->st[s].running, d->st[s].lpib);
        fprintf(f, "\n");
    }
    if (board.usb) {
        const HvXhci *x = board.usb;
        fprintf(f, "xhci: usbcmd=%x usbsts=%x", x->usbcmd, x->usbsts);
        for (int i = 0; i < HV_XHCI_INTRS; i++) fprintf(f, " iman%d=%x", i, x->intr[i].iman);
        fprintf(f, "\n");
    }
    for (int n = 0; n < 2; n++) {
        const HvNvme *nv = n ? board.nvme1 : board.nvme0;
        if (!nv) continue;
        fprintf(f, "nvme%d: cc=%x csts=%x intms=%x", n, nv->cc, nv->csts, nv->intms);
        for (int q = 0; q < HV_NVME_MAX_QUEUES; q++)
            if (nv->cq[q].valid) fprintf(f, " cq%d: ien=%u pending=%u head=%u tail=%u", q, nv->cq[q].ien, nv->cq[q].irq_pending, nv->cq[q].head, nv->cq[q].tail);
        fprintf(f, "\n");
    }
}
HvWinVirt *hv_winvm_board(void) { return &board; }

uint64_t hv_winvm_display_flushes(void) {
    return board.gpu ? __atomic_load_n(&board.gpu->flushes, __ATOMIC_RELAXED) : 0;
}

void hv_winvm_cpu_insns(HvCpu *cpu, uint64_t pc, uint32_t words[17]) {
    for (int j = 0; j < 17; j++) {
        uint64_t va = (pc & ~3ull) + (int64_t)(j * 4 - 32);
        HvWalk w = {0};
        hv_mmu_walk(cpu, va, HV_ACC_READ, cpu->el, &w);
        words[j] = 0;
        if (w.fsc == 0) hv_phys_read(cpu->m, w.pa, &words[j], 4);
    }
}

void hv_winvm_set_oauth_mode(int enabled) { hv_share_set_url_handler_enabled(enabled); }
char *hv_winvm_take_open_url(void) { return hv_share_take_open_url(); }
int hv_winvm_take_oauth_done(void) { return hv_share_take_oauth_done(); }

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

static mach_port_t cpu_port[HV_MAX_CPUS]; /* each vCPU's host thread, for its on-core time in perf.csv */
static uint64_t created_at;                /* mach time hv_winvm_create ran */

static void prepare_cpu_thread(int index) {
    static __thread int done;
    if (done) return;
    done = 1;
    if (index >= 0 && index < HV_MAX_CPUS) cpu_port[index] = mach_thread_self();
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

void hv_winvm_set_display_content(double x, double y, double w, double h) {
    os_unfair_lock_lock(&display_lock);
    display_content[0] = x;
    display_content[1] = y;
    display_content[2] = w;
    display_content[3] = h;
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    if (fb) hv_fb_metal_set_content(fb, x, y, w, h);
    os_unfair_lock_unlock(&display_lock);
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

/* The last size asked for. A running VM gets it at once and the next one when it starts: the app asks only
   when its layout changes, so Windows started again in the same window must come up at this size, not at
   whatever was asked before the first start. */
static uint32_t wanted_width, wanted_height;

void hv_winvm_request_display_size(uint32_t width, uint32_t height) {
    os_unfair_lock_lock(&display_lock);
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    wanted_width = width;
    wanted_height = height;
    os_unfair_lock_unlock(&display_lock);
    if (fb) hv_fb_display_request_size(fb, width, height);
}

#if !TARGET_OS_IPHONE
/* The Mac host has no keyboard to raise: HV_FOCUS_LOG prints what the agent reports instead. */
static void log_text_focus(unsigned long seq, int editable, int inside, void *ctx) {
    (void)ctx;
    fprintf(stderr, "text_focus editable=%d inside=%d seq=%lu\n", editable, inside, seq);
}
#endif

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

    created_at = mach_absolute_time();
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
        hv_share_clear_hibernate();
#if !TARGET_OS_IPHONE
        if (getenv("HV_FOCUS_LOG")) hv_share_set_text_focus_handler(log_text_focus, NULL);
#endif
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
    if (display_layer_set && board.fb) {
        hv_fb_metal_set_layer(board.fb, (void *)display_layer);
        hv_fb_metal_set_content(board.fb, display_content[0], display_content[1], display_content[2], display_content[3]);
    }
    atomic_store_explicit(&display_fb, board.fb, memory_order_release);
    uint32_t want_w = wanted_width, want_h = wanted_height;
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
    prepare_cpu_thread((int)c->index);
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
    prepare_cpu_thread(0);
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
            /* Devices that write guest memory on their own (received frames, input reports, audio) would
               otherwise keep using the rings of the OS that just rebooted, in memory the firmware and the
               next kernel now own, until a driver resets them. */
            hv_dev_lock(&m);
            if (board.net) hv_virtio_reset(&board.net->vio);
            if (board.snd) hv_virtio_reset(&board.snd->vio);
            if (board.usb) hv_xhci_reset(board.usb);
            if (board.hda) hv_hda_reset(board.hda);
            hv_dev_unlock(&m);
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

void hv_winvm_request_hibernate(void) {
    hv_share_request_hibernate();
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
                           uint64_t *jit_flushes, uint64_t *wfi_ticks) {
    if (jit_used_bytes) *jit_used_bytes = (uint64_t)m.jit.used * 4ull;
    if (jit_cap_bytes) *jit_cap_bytes = (uint64_t)m.jit.capacity * 4ull;
    if (jit_flushes) *jit_flushes = m.stats.block_flushes;
    if (wfi_ticks) *wfi_ticks = m.stats.wfi_ticks;
}

static uint64_t ticks_ns(uint64_t t) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return t * tb.numer / tb.denom;
}

/* Largest-since-last-read values: take and zero (a racing update may be lost, which is fine here). */
static uint64_t take_max_us(uint64_t *v) { return ticks_ns(__atomic_exchange_n(v, 0, __ATOMIC_RELAXED)) / 1000; }

static uint64_t thread_run_ns(mach_port_t port) {
    if (!port) return 0;
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS) return 0;
    return ((uint64_t)info.user_time.seconds + (uint64_t)info.system_time.seconds) * 1000000000ull +
           ((uint64_t)info.user_time.microseconds + (uint64_t)info.system_time.microseconds) * 1000ull;
}

/* Reads every counter racily: a snapshot for rates, not an audit. Host-side fields (memory, thermal) are the caller's. */
void hv_winvm_perf_snapshot(HvPerfSnap *s) {
    memset(s, 0, sizeof *s);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    s->t_ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    if (created_at) s->uptime_ms = ticks_ns(mach_absolute_time() - created_at) / 1000000ull;

    HvStats *st = &m.stats;
    s->translations = st->translations;
    s->blocks = st->blocks;
    s->block_flushes = st->block_flushes;
    s->flush_full = st->flush_full;
    s->flush_smc = st->flush_smc;
    s->flush_ic = st->flush_ic;
    s->code_protects = st->code_protects;
    s->chained = st->chained;
    s->chain_far = st->chain_far;
    s->xchained_now = m.nxchained;
    s->nblocks_now = m.nblocks;
    s->jit_used_kb_now = (uint64_t)m.jit.used * 4ull / 1024ull;
    s->jit_cap_kb_now = (uint64_t)m.jit.capacity * 4ull / 1024ull;
    s->inval_dev = st->inval_dev;
    s->inval_scanned = st->inval_scanned;
    s->code_writes = st->code_writes, s->code_write_hits = st->code_write_hits;
    s->code_write_forced = st->code_write_forced, s->code_retranslates = st->code_retranslates;
    s->code_block_kills = st->code_block_kills;
    s->diag_word = st->diag_word, s->diag_same = st->diag_same, s->diag_zero = st->diag_zero;
    s->diag_el1 = st->diag_el1, s->diag_dev = st->diag_dev, s->diag_big = st->diag_big;
    s->diag_g8 = st->diag_g8, s->diag_g16 = st->diag_g16;
    s->inval_ns = ticks_ns(st->inval_ticks);
    s->stw_count = st->stw_count;
    s->stw_ns = ticks_ns(st->stw_ticks);
    s->stw_max_us = take_max_us(&st->stw_max_ticks);
    s->exceptions = st->exceptions;
    s->interrupts = st->interrupts;
    s->tlb_flushes = st->tlb_flushes;
    s->fetch_walks = st->fetch_walks;
    s->proc_switches = st->stash_saves;
    /* Indirect-branch cache: resets and light resolves always count; fills, TLB refills and dispatcher
       entries by cause only with engine stats on (stats_interval != 0). */
    s->ijc_resets = st->ijc_resets;
    s->ijc_light = st->ijc_light;
    s->ijc_fills = st->ijc_fills;
    s->tlb_refills = st->refills;
    uint64_t *disp[6] = {&s->disp_chainfill, &s->disp_kick, &s->disp_exc, &s->disp_irq, &s->disp_cached, &s->disp_miss};
    for (int i = 0; i < 6; i++) *disp[i] = st->why[i];
    s->wfi = st->wfi;
    s->wfi_ns = ticks_ns(st->wfi_ticks);
    uint64_t *wfi_cpu[4] = {&s->wfi0_ns, &s->wfi1_ns, &s->wfi2_ns, &s->wfi3_ns};
    uint64_t *run_cpu[4] = {&s->run0_ns, &s->run1_ns, &s->run2_ns, &s->run3_ns};
    for (int i = 0; i < ncpus && i < 4; i++) {
        *wfi_cpu[i] = ticks_ns(m.percpu[i].wfi_ticks);
        *run_cpu[i] = thread_run_ns(cpu_port[i]);
    }
    s->translate_ns = ticks_ns(st->translate_ticks);
    s->dev_wait_ns = ticks_ns(st->dev_wait_ticks);
    s->tr_wait_ns = ticks_ns(st->tr_wait_ticks);
    s->tlbi_wait_ns = ticks_ns(st->sync_ticks);

    HvVirtioGpu *g = board.gpu;
    if (g) {
        s->gpu_cmds = g->cmds;
        s->gpu_transfers = g->transfers;
        s->gpu_flushes = g->flushes;
        s->gpu_flush_px = g->flush_px;
        s->gpu_xfer_bytes = g->xfer_bytes;
        s->gpu_xfer_ns = ticks_ns(g->xfer_ticks);
        s->gpu_rect_small = g->rect_small;
        s->gpu_rect_mid = g->rect_mid;
        s->gpu_rect_big = g->rect_big;
        _Static_assert(HV_VGPU_GRID_W * HV_VGPU_GRID_H == HV_PERF_GRID, "perf.csv grid matches the GPU's");
        for (int i = 0; i < HV_PERF_GRID; i++) s->gpu_grid[i] = g->grid[i];
    }
    s->frames_presented = hv_winvm_display_frames();
    uint32_t fw = 0, fh = 0;
    if (hv_winvm_display_size(&fw, &fh) == 0) {
        s->fb_w_now = fw;
        s->fb_h_now = fh;
    }
    HvNvme *nv = board.nvme0;
    if (nv) {
        s->nvme_rd_n = nv->rd_n;
        s->nvme_rd_bytes = nv->rd_bytes;
        s->nvme_rd_ns = ticks_ns(nv->rd_ticks);
        s->nvme_rd_max_us = take_max_us(&nv->rd_max_ticks);
        s->nvme_wr_n = nv->wr_n;
        s->nvme_wr_bytes = nv->wr_bytes;
        s->nvme_wr_ns = ticks_ns(nv->wr_ticks);
        s->nvme_wr_max_us = take_max_us(&nv->wr_max_ticks);
        s->nvme_flush_n = nv->flush_n;
        s->nvme_lat_100us = nv->lat_hist[0];
        s->nvme_lat_1ms = nv->lat_hist[1];
        s->nvme_lat_10ms = nv->lat_hist[2];
        s->nvme_lat_slow = nv->lat_hist[3];
    }
    HvXhci *x = board.usb;
    if (x) {
        s->usb_delivered = x->delivered;
        s->in_n = x->in_n;
        s->in_q_ns = ticks_ns(x->in_q_ticks);
        s->in_q_max_us = take_max_us(&x->in_q_max_ticks);
    }
    s->tick0_gap_max_us = take_max_us(&board.tick0_gap_max_ticks);
}
