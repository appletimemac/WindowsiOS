/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One row of performance counters for the whole VM. The iOS app appends a row to Documents/perf.csv every
   2 s and the Mac runner to --perf-csv; scripts/perf_report.py turns consecutive rows into rates.
   Counters are cumulative, except names ending in _now (a level) and _max_us (the largest single event
   since the previous row: reading resets it). Times are nanoseconds unless the name says otherwise. */
#ifndef HV_PERF_H
#define HV_PERF_H

#include <stdint.h>
#include <stdio.h>

#define HV_PERF_GRID 48 /* virtio-gpu flushed pixels by rect center: 8 x 6 cells over the screen, row-major */

#define HV_PERF_FIELDS(X)                                                                                  \
    X(t_ms) X(uptime_ms)                                                                                   \
    X(translations) X(blocks) X(block_flushes) X(flush_full) X(flush_smc) X(flush_ic) X(code_protects)   \
    X(chained) X(chain_far) X(xchained_now) X(nblocks_now) X(jit_used_kb_now) X(jit_cap_kb_now)           \
    X(inval_dev) X(inval_scanned) X(inval_ns) X(stw_count) X(stw_ns) X(stw_max_us)                       \
    X(exceptions) X(interrupts) X(tlb_flushes) X(fetch_walks) X(proc_switches)                            \
    X(ijc_resets) X(ijc_light) X(ijc_fills) X(tlb_refills)                                                \
    X(disp_chainfill) X(disp_kick) X(disp_exc) X(disp_irq) X(disp_cached) X(disp_miss)                    \
    X(wfi) X(wfi_ns) X(wfi0_ns) X(wfi1_ns) X(wfi2_ns) X(wfi3_ns)                                         \
    X(run0_ns) X(run1_ns) X(run2_ns) X(run3_ns)                                                          \
    X(translate_ns) X(dev_wait_ns) X(tr_wait_ns) X(tlbi_wait_ns)                                         \
    X(gpu_cmds) X(gpu_transfers) X(gpu_flushes) X(gpu_flush_px) X(gpu_xfer_bytes) X(gpu_xfer_ns)          \
    X(gpu_rect_small) X(gpu_rect_mid) X(gpu_rect_big) X(frames_presented) X(fb_w_now) X(fb_h_now)          \
    X(nvme_rd_n) X(nvme_rd_bytes) X(nvme_rd_ns) X(nvme_rd_max_us)                                        \
    X(nvme_wr_n) X(nvme_wr_bytes) X(nvme_wr_ns) X(nvme_wr_max_us) X(nvme_flush_n)                        \
    X(nvme_lat_100us) X(nvme_lat_1ms) X(nvme_lat_10ms) X(nvme_lat_slow)                                  \
    X(in_n) X(in_q_ns) X(in_q_max_us) X(tick0_gap_max_us) X(usb_delivered)                               \
    X(avail_mem_mb_now) X(thermal_now) X(low_power_now)

typedef struct HvPerfSnap {
#define HV_PERF_DECL(name) uint64_t name;
    HV_PERF_FIELDS(HV_PERF_DECL)
#undef HV_PERF_DECL
    uint64_t gpu_grid[HV_PERF_GRID];
} HvPerfSnap;

/* Appends rows to path. A file that grows past max_bytes is renamed to path.1 (replacing the older one) and
   started again. Each open (and each new file) writes '# <comment>' (when given) and then the column-name line,
   which starts with "t_ms,": a reader starts a new segment there. */
typedef struct HvPerfCsv {
    FILE *f;
    long max_bytes;
    char path[1024];
    char comment[512];
} HvPerfCsv;

int hv_perf_csv_open(HvPerfCsv *c, const char *path, long max_bytes, const char *comment);
void hv_perf_csv_row(HvPerfCsv *c, const HvPerfSnap *s);
/* A '#mark <t_ms> <label>' line between rows: a step of a benchmark, or a moment someone noted. */
void hv_perf_csv_mark(HvPerfCsv *c, uint64_t t_ms, const char *label);
void hv_perf_csv_close(HvPerfCsv *c);

#endif
