#ifndef WINVM_NVME_H
#define WINVM_NVME_H

#include <pthread.h>
#include <stdint.h>

#include "disk.h"
#include "pcie.h"

#define HV_NVME_MAX_QUEUES 17 /* 0: Admin, 1..16: I/O */

typedef struct HvNvmeSq {
    uint64_t pa;
    uint16_t size;  /* number of 64-byte entries */
    uint16_t head;
    uint16_t tail;
    uint16_t cqid;
    int valid;
} HvNvmeSq;

typedef struct HvNvmeCq {
    uint64_t pa;
    uint16_t size;  /* number of 16-byte entries */
    uint16_t head;
    uint16_t tail;
    uint16_t irq_vec;
    uint8_t phase;
    uint8_t ien;
    uint8_t irq_pending;
    int valid;
} HvNvmeCq;

typedef struct HvNvme {
    HvPciDev pci;
    HvMachine *m;
    HvDisk *disk;
    pthread_mutex_t mu;
    char serial[24];

    uint64_t cap;
    uint32_t vs;
    uint32_t intms;
    uint32_t cc;
    uint32_t csts;
    uint32_t aqa;
    uint64_t asq;
    uint64_t acq;

    uint32_t temp_thresh;
    uint32_t arb_feat;
    uint32_t pm_feat;
    uint32_t vwc_feat;
    uint32_t num_queues_feat;

    HvNvmeSq sq[HV_NVME_MAX_QUEUES];
    HvNvmeCq cq[HV_NVME_MAX_QUEUES];

    uint8_t *io_buf;
    uint32_t io_buf_cap;
    uint64_t rd_bytes, wr_bytes;
    uint64_t trim_n, trim_bytes;
    /* Counters read racily from another thread (host ticks are mach_absolute_time units). */
    uint64_t rd_n, wr_n, flush_n;            /* I/O commands handled */
    uint64_t rd_ticks, wr_ticks;             /* host time per command class, including PRP DMA */
    uint64_t rd_max_ticks, wr_max_ticks;     /* largest single command since the reader last zeroed it */
    uint64_t lat_hist[4];                    /* reads and writes by latency: < 100 us, < 1 ms, < 10 ms, longer */
} HvNvme;

HvNvme *hv_nvme_create(HvMachine *m, HvPcie *bus, HvDisk *disk, const char *serial);

#endif
