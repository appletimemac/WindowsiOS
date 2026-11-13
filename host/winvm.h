/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HV_WINVM_H
#define HV_WINVM_H

#include <stddef.h>
#include <stdint.h>

#include "perf.h"
#include "winvirt.h"

typedef struct HvWinVmConfig {
    const char *code_fd;      /* 64 MB EDK2 UEFI firmware (read-only) */
    const char *vars_fd;      /* 64 MB UEFI NVRAM (read/write persistent) */
    const char *disk0;        /* Primary NVMe disk image (raw or .hvsparse) */
    const char *disk1;        /* Optional secondary/installer NVMe disk image (read-only) */
    const char *share_dir;    /* Optional host folder shared over WebDAV at http://10.0.2.4/ */
    const char *share_sock;   /* Path for internal WebDAV Unix domain socket */
    int ncpus;
    uint64_t ram_mb;
    size_t jit_bytes;
    void *jit_rx;             /* Optional split RX/RW JIT pointers for iOS TXM */
    void *jit_rw;
    void *prealloc_ram;       /* Optional pre-allocated Guest RAM (ledger-exempt on iOS) */
    int no_net;
    int enable_sound;
    int log;
    int stats_interval;
} HvWinVmConfig;

int hv_winvm_create(const HvWinVmConfig *cfg, char *err, size_t errlen);
void hv_winvm_start(int spawn_cpu0, void (*on_exit)(int reason));
int hv_winvm_run_cpu0(void);
void hv_winvm_stop(void);
void hv_winvm_power_button(void);
/* Windows saves its session to disk and powers off; the next boot resumes it (needs WindowsiOS Tools). */
void hv_winvm_request_hibernate(void);
void hv_winvm_flush_disks(void);
/* Periodic checkpoint while running: an ordering barrier instead of a full sync. */
void hv_winvm_checkpoint_disks(void);
/* Posture the guest's tablet mode follows: 1 laptop, 0 tablet (the default). Any thread. */
void hv_winvm_set_posture(int laptop);

/* Display & screenshot hooks (any thread) */
void hv_winvm_set_display_layer(void *ca_metal_layer);
int hv_winvm_display_size(uint32_t *w, uint32_t *h);
uint64_t hv_winvm_display_frames(void);
int hv_winvm_save_png(const char *path);
/* The host window's size in pixels: the guest's virtio-gpu is told to render at it (clamped to even values in
   640x480..3840x2160). Works before hv_winvm_create too (it is applied then). */
void hv_winvm_request_display_size(uint32_t width, uint32_t height);

/* Input hooks (any thread) */
void hv_winvm_key_hid(uint8_t usage, int down);
void hv_winvm_key_linux(uint16_t code, int down);
/* Normalized tablet pointer: x_norm, y_norm in [0.0, 1.0] mapped to 0..32767;
 * buttons: bit0=Left, bit1=Right, bit2=Middle; wheel: -127..127. */
void hv_winvm_tablet(double x_norm, double y_norm, uint8_t buttons, int8_t wheel);
/* Multi-touch frame for the USB touchscreen (see HvTouchContact in xhci.h): the full set of contacts
 * (1..5, x and y 0..32767 across the Windows desktop), contacts lifting (tip 0) reported once. */
void hv_winvm_touch(const HvTouchContact *contacts, int count);
/* 1 once Windows has configured the touchscreen, so touches stop being dropped. */
int hv_winvm_touch_ready(void);
void hv_winvm_send_ctrl_alt_del(void);
void hv_winvm_usb_stats(int *kbd_ready, int *tab_ready, uint64_t *delivered);
/* jit_flushes: translation-cache flushes that happened (stats.block_flushes). */
void hv_winvm_engine_stats(uint64_t *jit_used_bytes, uint64_t *jit_cap_bytes,
                           uint64_t *jit_flushes, uint64_t *wfi_ticks);
/* One row of perf.csv (host/perf.h). Any thread; the caller fills the host fields (memory, thermal, power). */
void hv_winvm_perf_snapshot(HvPerfSnap *out);
int hv_winvm_share_up(void);

HvMachine *hv_winvm_machine(void);
HvWinVirt *hv_winvm_board(void);
const char *hv_winvm_reason_name(int reason);

#endif
