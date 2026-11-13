/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Dual-mapped RX/RW JIT allocator for iOS 17..27 (including iOS 26/27 TXM).
 * Adapted from Henge (Leviidev/Henge, GPL-2.0-or-later).
 */
#include "winios_jit.h"
#include "winvirt.h"

#include <TargetConditionals.h>
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/task_info.h>
#include <mach/vm_map.h>
#include <os/log.h>
#include <os/proc.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

/* Provide pipe2 if referenced by static glib/slirp on older iOS runtimes. */
__attribute__((weak, visibility("default")))
int pipe2(int fds[2], int flags) {
    if (!fds) {
        errno = EFAULT;
        return -1;
    }
    if (pipe(fds) < 0) return -1;
    if ((flags & O_CLOEXEC) &&
        (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0)) {
        int e = errno;
        close(fds[0]);
        close(fds[1]);
        errno = e;
        return -1;
    }
    if (flags & O_NONBLOCK) {
        int f0 = fcntl(fds[0], F_GETFL), f1 = fcntl(fds[1], F_GETFL);
        if (f0 < 0 || f1 < 0 ||
            fcntl(fds[0], F_SETFL, f0 | O_NONBLOCK) < 0 ||
            fcntl(fds[1], F_SETFL, f1 | O_NONBLOCK) < 0) {
            int e = errno;
            close(fds[0]);
            close(fds[1]);
            errno = e;
            return -1;
        }
    }
    return 0;
}

extern void    *winios_brk_get_jit_mapping(void *addr, size_t len);
extern void     winios_brk_jit_detach(void);
extern uint64_t winios_brk_probe(void);

#define WINIOS_LOG(fmt, ...)                                              \
    do {                                                                  \
        os_log(OS_LOG_DEFAULT, "[winios-jit] " fmt, ##__VA_ARGS__);       \
        fprintf(stderr, "[winios-jit] " fmt "\n", ##__VA_ARGS__);         \
        fflush(stderr);                                                   \
    } while (0)

/* Executable JIT pages above 0x7000000000 hang on iOS 26/27 TXM devices.
 * Reserve a zero-fill block (WINIOS_JIT_MAX_BYTES) in __DATA at launch so it sits inside the
 * low < 0x180000000 window, then deallocate it immediately before debugserver's
 * `_M<size>,rx` allocation takes the first-fit hole. */
#define WINIOS_JIT_LOW_LIMIT 0x7000000000ull
__attribute__((used, aligned(16384)))
static uint8_t g_jit_reserve[WINIOS_JIT_MAX_BYTES + (16u << 10)];
static bool g_jit_reserve_released;

static _Thread_local bool g_expecting_jit_trap;
static struct sigaction g_prev_sigtrap;
static struct sigaction g_prev_sigbus;

static WinIosDualMapping g_mapping;
static void *g_guest_ram;
static uint64_t g_guest_ram_bytes;
static atomic_bool g_jit_live;
static char g_status[256] = "Not initialized";

static void prealloc_ledger_exempt_ram(void) {
    if (g_guest_ram) return;
    uint64_t phys_mem = 0;
    size_t len = sizeof phys_mem;
    sysctlbyname("hw.memsize", &phys_mem, &len, NULL, 0);
    uint64_t phys_mb = phys_mem >> 20;
    /* The largest size that leaves iOS at least 4 GiB of physical RAM, since Windows eventually
     * touches all of its memory. Sizes above 4608 MiB are meant for the extended-virtual-addressing
     * and increased-memory-limit entitlements; if an allocation fails the next size is tried. */
    static const uint64_t ios_reserve_mb = 4096;
    static const uint64_t candidates_mb[] = {8192, 7168, 6144, 5120, 4608, 4096, 3584, 3072, 2560, 2048};
    for (size_t i = 0; i < sizeof(candidates_mb) / sizeof(candidates_mb[0]); i++) {
        uint64_t mb = candidates_mb[i];
        if (phys_mb > 0 && mb + ios_reserve_mb > phys_mb) continue;
        uint8_t *p = hv_alloc_no_footprint(mb << 20);
        if (p) {
            g_guest_ram = p;
            g_guest_ram_bytes = mb << 20;
            WINIOS_LOG("pre-allocated %llu MiB ledger-exempt Guest RAM at %p",
                       (unsigned long long)mb, (void *)p);
            return;
        }
    }
    WINIOS_LOG("ledger-exempt Guest RAM pre-allocation unavailable; will fall back at VM start");
}

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
}

static void winios_trap_handler(int sig, siginfo_t *info, void *ctx) {
    (void)info;
    if (g_expecting_jit_trap && ctx != NULL) {
        ucontext_t *uc = (ucontext_t *)ctx;
        uc->uc_mcontext->__ss.__pc += 4;
        uc->uc_mcontext->__ss.__x[0] = 0;
        return;
    }
    struct sigaction *prev = (sig == SIGTRAP) ? &g_prev_sigtrap : &g_prev_sigbus;
    sigaction(sig, prev, NULL);
    raise(sig);
}

void winios_jit_install_trap_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = winios_trap_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, &g_prev_sigtrap);
    sigaction(SIGBUS, &sa, &g_prev_sigbus);
    set_status("Trap guard ready");
    WINIOS_LOG("trap guard installed");
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
bool winios_jit_is_debugger_attached(void) {
    uint32_t flags = 0;
    return syscall(169 /* SYS_csops */, getpid(), 0 /* CS_OPS_STATUS */, &flags, sizeof flags) == 0 &&
           (flags & 0x10000000u) != 0; /* CS_DEBUGGED */
}
#pragma clang diagnostic pop

size_t winios_available_memory_bytes(void) {
#if TARGET_OS_IPHONE
    return os_proc_available_memory();
#else
    return 4ull << 30;
#endif
}

static void release_low_hold(size_t bytes) {
    if (g_jit_reserve_released || bytes > WINIOS_JIT_MAX_BYTES) return;
    vm_address_t start = ((vm_address_t)g_jit_reserve + 16383) & ~(vm_address_t)16383;
    kern_return_t kr = vm_deallocate(mach_task_self(), start, WINIOS_JIT_MAX_BYTES);
    WINIOS_LOG("released low __DATA hold at %#lx (%zu MiB): %s",
               (unsigned long)start, WINIOS_JIT_MAX_BYTES >> 20, mach_error_string(kr));
    g_jit_reserve_released = true;
}

static bool jit_selftest(const WinIosDualMapping *m) {
    /* movz w0, #42 ; ret */
    static const uint32_t kCode[2] = {0x52800540u, 0xD65F03C0u};
    if (!m->rw_addr || !m->rx_addr || m->size < 16384) return false;

    const size_t offsets[2] = {0, m->size - 16384};
    for (int i = 0; i < 2; i++) {
        size_t off = offsets[i];
        memcpy(m->rw_addr + off, kCode, sizeof kCode);
        sys_icache_invalidate(m->rx_addr + off, sizeof kCode);

        uint32_t rb[2] = {0, 0};
        memcpy(rb, m->rx_addr + off, sizeof rb);
        if (rb[0] != kCode[0] || rb[1] != kCode[1]) {
            WINIOS_LOG("selftest FAIL at +%#zx: alias mismatch (%08x %08x)", off, rb[0], rb[1]);
            return false;
        }
        int (*fn)(void) = (int (*)(void))(void *)(m->rx_addr + off);
        int rc = fn();
        if (rc != 42) {
            WINIOS_LOG("selftest FAIL at +%#zx: expected 42, got %d", off, rc);
            return false;
        }
    }
    WINIOS_LOG("selftest PASS: executed RX code at %p and +%#zx -> 42",
               (void *)m->rx_addr, m->size - 16384);
    return true;
}

bool winios_jit_acquire(size_t bytes, bool detach_after) {
    if (atomic_load(&g_jit_live)) return true;
    if (bytes == 0) bytes = WINIOS_JIT_DEFAULT_BYTES;
    if (bytes > WINIOS_JIT_MAX_BYTES) bytes = WINIOS_JIT_MAX_BYTES;
    bytes = (bytes + 0x3fffu) & ~(size_t)0x3fffu;

    if (!winios_jit_is_debugger_attached()) {
        set_status("Waiting for debugger (StikDebug or LLDB)");
        WINIOS_LOG("acquire: CS_DEBUGGED not set yet");
        return false;
    }

    g_expecting_jit_trap = true;
    uint64_t probe = winios_brk_probe();
    g_expecting_jit_trap = false;

    if (probe == 0) {
        /* Pre-iOS 26 fallback where CS_DEBUGGED permits plain vm_remap RX/RW without trap servicing */
        if (!__builtin_available(iOS 26.0, *)) {
            release_low_hold(bytes);
            uint8_t *rx = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (rx != MAP_FAILED) {
                vm_address_t rw = 0;
                vm_prot_t cur, max;
                if (vm_remap(mach_task_self(), &rw, bytes, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                             (vm_address_t)rx, FALSE, &cur, &max, VM_INHERIT_NONE) == KERN_SUCCESS &&
                    vm_protect(mach_task_self(), (vm_address_t)rx, bytes, FALSE,
                               VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS &&
                    vm_protect(mach_task_self(), rw, bytes, FALSE,
                               VM_PROT_READ | VM_PROT_WRITE) == KERN_SUCCESS) {
                    WinIosDualMapping m = {(uint8_t *)rw, rx, bytes};
                    if (jit_selftest(&m)) {
                        g_mapping = m;
                        atomic_store(&g_jit_live, true);
                        set_status("Live (%zu MiB plain RX/RW)", bytes >> 20);
                        return true;
                    }
                }
                munmap(rx, bytes);
            }
        }
        set_status("Debugger attached, waiting for brk trap handler");
        WINIOS_LOG("probe returned 0 (debugger did not answer brk #0x69)");
        return false;
    }

    WINIOS_LOG("probe answered: 0x%llx", (unsigned long long)probe);
    release_low_hold(bytes);

    void *rx = NULL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        g_expecting_jit_trap = true;
        rx = winios_brk_get_jit_mapping(NULL, bytes);
        g_expecting_jit_trap = false;
        if (rx != NULL) break;
        usleep(50 * 1000);
    }

    if (!rx) {
        set_status("Failed: debugger returned NULL RX region");
        return false;
    }
    if ((uint64_t)(uintptr_t)rx + bytes > WINIOS_JIT_LOW_LIMIT) {
        WINIOS_LOG("RX region %p above %#llx limit; releasing", rx, WINIOS_JIT_LOW_LIMIT);
        vm_deallocate(mach_task_self(), (vm_address_t)rx, bytes);
        set_status("Failed: RX allocated above low limit (relaunch app)");
        return false;
    }

    /* Map writable alias within +-2 GiB of RX if possible, otherwise anywhere */
    vm_address_t rw = 0;
    vm_prot_t cur_prot = VM_PROT_NONE, max_prot = VM_PROT_NONE;
    kern_return_t kr = KERN_NO_SPACE;
    const uintptr_t step = 16u << 20, reach = (uintptr_t)2 << 30;
    for (uintptr_t off = 0; off <= reach && kr != KERN_SUCCESS; off += step) {
        for (int side = 0; side < 2 && kr != KERN_SUCCESS; side++) {
            uintptr_t want = side == 0 ? (uintptr_t)rx + bytes + off : (uintptr_t)rx - bytes - off;
            if (side == 1 && (uintptr_t)rx < bytes + off + step) continue;
            rw = (vm_address_t)want;
            kr = vm_remap(mach_task_self(), &rw, (vm_size_t)bytes, 0, VM_FLAGS_FIXED,
                          mach_task_self(), (vm_address_t)rx, FALSE, &cur_prot, &max_prot, VM_INHERIT_NONE);
        }
    }
    if (kr != KERN_SUCCESS) {
        rw = 0;
        kr = vm_remap(mach_task_self(), &rw, (vm_size_t)bytes, 0, VM_FLAGS_ANYWHERE,
                      mach_task_self(), (vm_address_t)rx, FALSE, &cur_prot, &max_prot, VM_INHERIT_NONE);
    }
    if (kr != KERN_SUCCESS) {
        set_status("Failed: vm_remap (%s)", mach_error_string(kr));
        vm_deallocate(mach_task_self(), (vm_address_t)rx, bytes);
        return false;
    }

    kr = vm_protect(mach_task_self(), rw, (vm_size_t)bytes, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) {
        set_status("Failed: vm_protect RW (%s)", mach_error_string(kr));
        vm_deallocate(mach_task_self(), rw, bytes);
        vm_deallocate(mach_task_self(), (vm_address_t)rx, bytes);
        return false;
    }

    WinIosDualMapping m = {(uint8_t *)rw, (uint8_t *)rx, bytes};
    if (!jit_selftest(&m)) {
        set_status("Failed: TXM selftest rejected RX execution");
        vm_deallocate(mach_task_self(), rw, bytes);
        vm_deallocate(mach_task_self(), (vm_address_t)rx, bytes);
        return false;
    }

    g_mapping = m;
    prealloc_ledger_exempt_ram();
    atomic_store(&g_jit_live, true);

    if (detach_after) {
        WINIOS_LOG("detaching debugger; %zu MiB RX=%p RW=%p, GuestRAM=%llu MiB stays live",
                   bytes >> 20, (void *)m.rx_addr, (void *)m.rw_addr,
                   (unsigned long long)(g_guest_ram_bytes >> 20));
        g_expecting_jit_trap = true;
        winios_brk_jit_detach();
        g_expecting_jit_trap = false;
    }

    set_status("Live (%zu MiB TXM JIT, %llu MiB RAM)", bytes >> 20,
               (unsigned long long)(g_guest_ram_bytes >> 20));
    return true;
}

bool winios_jit_is_live(void) {
    return atomic_load(&g_jit_live);
}

WinIosDualMapping winios_jit_mapping(void) {
    return g_mapping;
}

void *winios_jit_guest_ram(uint64_t *out_bytes) {
    if (out_bytes) *out_bytes = g_guest_ram_bytes;
    return g_guest_ram;
}

const char *winios_jit_status_text(void) {
    return g_status;
}
