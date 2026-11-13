#include "nvme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NVME_BAR_SIZE 0x4000
#define NVME_MAX_XFER (2u << 20) /* 2 MB MDTS */

typedef struct NvmeCmd {
    uint8_t  opc;
    uint8_t  fuse_psdt;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} NvmeCmd;

typedef struct NvmeCqe {
    uint32_t dw0;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; /* bit 0: phase, bits 15:1: status code */
} NvmeCqe;

static uint32_t page_size_of(const HvNvme *nv) {
    uint32_t mps = (nv->cc >> 7) & 0xf;
    if (mps > 4) mps = 4;
    return 1u << (12 + mps);
}

/* Transfer `len` bytes between `buf` and guest PRP1/PRP2/PRP-list. */
static int nvme_dma_prp(HvNvme *nv, uint64_t prp1, uint64_t prp2, void *buf, uint32_t len, int to_guest) {
    if (!len) return 0;
    uint32_t ps = page_size_of(nv);
    uint64_t pmask = ~((uint64_t)ps - 1);
    uint8_t *p = buf;

    /* First segment: from prp1 (may have intra-page offset) */
    uint32_t off1 = (uint32_t)(prp1 & (ps - 1));
    uint32_t chunk = ps - off1;
    if (chunk > len) chunk = len;
    int rc = to_guest ? hv_phys_write(nv->m, prp1, p, chunk)
                      : hv_phys_read(nv->m, prp1, p, chunk);
    if (rc != 0) return -1;
    p += chunk;
    len -= chunk;
    if (!len) return 0;

    /* Second segment: if remaining fits in a single page, prp2 IS that page PA */
    if (len <= ps) {
        uint64_t pa = prp2 & pmask;
        return to_guest ? hv_phys_write(nv->m, pa, p, len)
                        : hv_phys_read(nv->m, pa, p, len);
    }

    /* Otherwise prp2 points to a PRP list of 64-bit page addresses */
    uint64_t list_pa = prp2;
    uint32_t list_off = (uint32_t)(list_pa & (ps - 1));
    list_pa &= pmask;
    while (len > 0) {
        /* If at last 8-byte slot of PRP list page and >1 page remains, chain to next PRP list page */
        if (list_off + 8 == ps && len > ps) {
            uint64_t next_list = 0;
            if (hv_phys_read(nv->m, list_pa + list_off, &next_list, 8) != 0) return -1;
            list_pa = next_list & pmask;
            list_off = 0;
        }
        uint64_t entry_pa = 0;
        if (hv_phys_read(nv->m, list_pa + list_off, &entry_pa, 8) != 0) return -1;
        entry_pa &= pmask;
        list_off += 8;

        uint32_t n = len < ps ? len : ps;
        rc = to_guest ? hv_phys_write(nv->m, entry_pa, p, n)
                      : hv_phys_read(nv->m, entry_pa, p, n);
        if (rc != 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static void nvme_update_intx(HvNvme *nv) {
    if (hv_pci_msix_enabled(&nv->pci)) {
        hv_pci_set_intx(&nv->pci, 0);
        return;
    }
    uint32_t active = 0;
    for (int i = 0; i < HV_NVME_MAX_QUEUES; i++) {
        if (nv->cq[i].valid && nv->cq[i].ien && nv->cq[i].irq_pending) {
            uint32_t bit = nv->cq[i].irq_vec < 32 ? (1u << nv->cq[i].irq_vec) : 1u;
            active |= bit;
        }
    }
    hv_pci_set_intx(&nv->pci, (active & ~nv->intms) != 0);
}

static void post_cqe(HvNvme *nv, HvNvmeSq *sq, uint16_t sqid, uint16_t cid, uint32_t dw0, uint16_t status_code) {
    HvNvmeCq *cq = &nv->cq[sq->cqid];
    if (!cq->valid || !cq->size) return;

    NvmeCqe cqe = {
        .dw0 = dw0,
        .rsvd = 0,
        .sq_head = sq->head,
        .sq_id = sqid,
        .cid = cid,
        .status = (uint16_t)((status_code << 1) | (cq->phase & 1)),
    };
    uint64_t cqe_pa = cq->pa + (uint64_t)cq->tail * 16u;
    hv_phys_write(nv->m, cqe_pa, &cqe, 16);

    if (++cq->tail >= cq->size) {
        cq->tail = 0;
        cq->phase ^= 1;
    }

    if (cq->ien) {
        cq->irq_pending = 1;
        if (hv_pci_msix_enabled(&nv->pci)) {
            hv_pci_msix_notify(&nv->pci, cq->irq_vec);
        } else {
            nvme_update_intx(nv);
        }
    }
}

static void pad_ascii(uint8_t *dst, size_t len, const char *s) {
    memset(dst, ' ', len);
    size_t n = strlen(s);
    if (n > len) n = len;
    memcpy(dst, s, n);
}

static uint16_t handle_identify(HvNvme *nv, const NvmeCmd *cmd) {
    uint8_t buf[4096];
    memset(buf, 0, sizeof buf);
    uint8_t cns = (uint8_t)(cmd->cdw10 & 0xff);

    if (cns == 0x00) { /* Identify Namespace */
        if (cmd->nsid != 1) return 0x000b; /* Invalid Namespace or Format */
        uint64_t blocks = nv->disk->size / 512ull;
        memcpy(buf + 0, &blocks, 8);  /* NSZE */
        memcpy(buf + 8, &blocks, 8);  /* NCAP */
        memcpy(buf + 16, &blocks, 8); /* NUSE */
        buf[24] = 0;                  /* NSFEAT */
        buf[25] = 0;                  /* NLBAF = 0 (1 format) */
        buf[26] = 0;                  /* FLBAS = format 0 */
        buf[33] = 0x09;               /* DLFEAT: Write Zeroes takes DEAC (bit 3); deallocated blocks read as 0 (001b, hv_disk_discard) */
        buf[99] = nv->disk->readonly ? 1 : 0; /* NSATTR: write protected */
        /* LBAF 0 at byte 128: MS=0, LBADS=9 (2^9 = 512 bytes), RP=0 */
        buf[130] = 9;
    } else if (cns == 0x01) { /* Identify Controller */
        uint16_t vid = 0x1b36;
        memcpy(buf + 0, &vid, 2);     /* VID */
        memcpy(buf + 2, &vid, 2);     /* SSVID */
        pad_ascii(buf + 4, 20, nv->serial[0] ? nv->serial : "WIN11IOS01");
        pad_ascii(buf + 24, 40, "QEMU NVMe Ctrl");
        pad_ascii(buf + 64, 8, "1.4");
        buf[72] = 6;                  /* RAB */
        buf[73] = 0x52; buf[74] = 0x54; buf[75] = 0x00; /* IEEE OUI */
        buf[77] = 9;                  /* MDTS: 2^9 * 4 KB = 2 MB */
        uint16_t cntlid = (uint16_t)(nv->pci.slot ? nv->pci.slot : 1);
        memcpy(buf + 78, &cntlid, 2);
        uint32_t ver = nv->vs;
        memcpy(buf + 80, &ver, 4);
        uint32_t rtd3r = 100000, rtd3e = 100000;
        memcpy(buf + 84, &rtd3r, 4);
        memcpy(buf + 88, &rtd3e, 4);
        uint32_t oaes = 0;
        memcpy(buf + 92, &oaes, 4);
        uint32_t ctratt = 0;
        memcpy(buf + 96, &ctratt, 4);
        uint16_t oacs = 0;
        memcpy(buf + 256, &oacs, 2);
        buf[258] = 3;                 /* ACL */
        buf[259] = 7;                 /* AERL */
        buf[260] = 0x12;              /* FRMW: 1 RO slot */
        buf[261] = 0x02;              /* LPA: Commands Supported & Effects log page */
        buf[262] = 63;                /* ELPE */
        buf[263] = 0;                 /* NPSS = 0 (1 power state) */
        buf[512] = 0x66;              /* SQES: min=64B (2^6), max=64B */
        buf[513] = 0x44;              /* CQES: min=16B (2^4), max=16B */
        uint16_t maxcmd = 1024;
        memcpy(buf + 514, &maxcmd, 2);
        uint32_t nn = 1;
        memcpy(buf + 516, &nn, 4);    /* NN = 1 namespace */
        uint16_t oncs = 0x000c;       /* Dataset Management (bit 2) + Write Zeroes (bit 3) */
        memcpy(buf + 520, &oncs, 2);
        buf[525] = 1;                 /* VWC: Volatile Write Cache present */
        {
            char nqn[96];
            snprintf(nqn, sizeof nqn, "nqn.2026-11.ios.windows:nvme:%s", nv->serial[0] ? nv->serial : "win11ios01");
            pad_ascii(buf + 768, 64, nqn);
        }
        /* Power State 0 descriptor at byte 2048: MP = 2500 (25W) */
        uint16_t mp = 2500;
        memcpy(buf + 2048, &mp, 2);
    } else if (cns == 0x02) { /* Active Namespace ID list */
        if (cmd->nsid < 1) {
            uint32_t ns1 = 1;
            memcpy(buf, &ns1, 4);
        }
    } else if (cns == 0x03) { /* Namespace Identification Descriptor list */
        if (cmd->nsid != 1) return 0x000b;
        /* NIDT = 3 (Namespace UUID, 16 bytes) */
        buf[0] = 3;
        buf[1] = 16;
        uint8_t uuid[16] = {
            0x57, 0x49, 0x4e, 0x31, 0x31, 0x49, 0x4f, 0x53,
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, (uint8_t)nv->pci.slot
        };
        memcpy(buf + 4, uuid, 16);
    } else if (cns == 0x05 || cns == 0x06) {
        /* I/O Command Set specific Identify: return zeroed structure */
    } else {
        return 0x0002; /* Invalid Field in Command */
    }

    return nvme_dma_prp(nv, cmd->prp1, cmd->prp2, buf, 4096, 1) == 0 ? 0 : 0x0004;
}

static uint16_t handle_get_log_page(HvNvme *nv, const NvmeCmd *cmd) {
    uint8_t lid = (uint8_t)(cmd->cdw10 & 0xff);
    uint32_t numdl = (cmd->cdw10 >> 16) & 0xffff;
    uint32_t numdu = cmd->cdw11 & 0xffff;
    uint32_t numd = ((numdu << 16) | numdl) + 1u;
    uint32_t bytes = numd * 4u;
    if (bytes > 4096) bytes = 4096;

    uint8_t buf[4096];
    memset(buf, 0, sizeof buf);

    if (lid == 0x01) { /* Error Information: empty */
    } else if (lid == 0x02) { /* SMART / Health Information */
        uint16_t temp = 305; /* 32 C in Kelvin */
        memcpy(buf + 1, &temp, 2);
        buf[3] = 100;        /* Available Spare 100% */
        buf[4] = 10;         /* Spare Threshold 10% */
        uint64_t du_rd = nv->rd_bytes / (512ull * 1000ull);
        uint64_t du_wr = nv->wr_bytes / (512ull * 1000ull);
        memcpy(buf + 32, &du_rd, 8);
        memcpy(buf + 48, &du_wr, 8);
    } else if (lid == 0x03) { /* Firmware Slot Info */
        buf[0] = 0x01;
        pad_ascii(buf + 8, 8, "1.4");
    } else if (lid == 0x05) { /* Commands Supported and Effects */
        uint32_t csupp = 1u; /* CSUPP */
        /* Admin: Delete/Create SQ/CQ, Get Log Page, Identify, Abort, Set/Get Features, AER */
        const uint8_t admin_ops[] = {0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x0c};
        for (size_t i = 0; i < sizeof admin_ops; i++)
            memcpy(buf + (size_t)admin_ops[i] * 4, &csupp, 4);
        /* I/O (at +1024): Flush, Write, Read, Write Zeroes, Dataset Management */
        const uint8_t io_ops[] = {0x00, 0x01, 0x02, 0x08, 0x09};
        for (size_t i = 0; i < sizeof io_ops; i++)
            memcpy(buf + 1024 + (size_t)io_ops[i] * 4, &csupp, 4);
    } else {
        /* Unknown log page: returnInvalid Field */
        return 0x0002;
    }

    return nvme_dma_prp(nv, cmd->prp1, cmd->prp2, buf, bytes, 1) == 0 ? 0 : 0x0004;
}

static void process_admin_cmd(HvNvme *nv, HvNvmeSq *sq, const NvmeCmd *cmd) {
    uint32_t dw0 = 0;
    uint16_t status = 0;
    if (getenv("HV_NVME_LOG"))
        fprintf(stderr, "nvme[%d] admin opc=%02x nsid=%u cdw10=%08x cdw11=%08x prp1=%llx prp2=%llx\n",
                nv->pci.slot, cmd->opc, cmd->nsid, cmd->cdw10, cmd->cdw11,
                (unsigned long long)cmd->prp1, (unsigned long long)cmd->prp2);

    switch (cmd->opc) {
    case 0x00: { /* Delete I/O Submission Queue */
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffff);
        if (qid == 0 || qid >= HV_NVME_MAX_QUEUES) status = 0x0101; /* Invalid Queue Identifier */
        else nv->sq[qid] = (HvNvmeSq){0};
        break;
    }
    case 0x01: { /* Create I/O Submission Queue */
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffff);
        uint16_t qsize = (uint16_t)((cmd->cdw10 >> 16) + 1u);
        uint16_t cqid = (uint16_t)(cmd->cdw11 >> 16);
        if (qid == 0 || qid >= HV_NVME_MAX_QUEUES || cqid == 0 || cqid >= HV_NVME_MAX_QUEUES || !nv->cq[cqid].valid) {
            status = 0x0101;
        } else {
            nv->sq[qid] = (HvNvmeSq){.pa = cmd->prp1, .size = qsize, .cqid = cqid, .valid = 1};
        }
        break;
    }
    case 0x02: /* Get Log Page */
        status = handle_get_log_page(nv, cmd);
        break;
    case 0x04: { /* Delete I/O Completion Queue */
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffff);
        if (qid == 0 || qid >= HV_NVME_MAX_QUEUES) status = 0x0101;
        else {
            nv->cq[qid] = (HvNvmeCq){0};
            nvme_update_intx(nv);
        }
        break;
    }
    case 0x05: { /* Create I/O Completion Queue */
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffff);
        uint16_t qsize = (uint16_t)((cmd->cdw10 >> 16) + 1u);
        uint16_t iv = (uint16_t)(cmd->cdw11 >> 16);
        uint8_t ien = (uint8_t)((cmd->cdw11 >> 1) & 1);
        if (qid == 0 || qid >= HV_NVME_MAX_QUEUES) {
            status = 0x0101;
        } else {
            nv->cq[qid] = (HvNvmeCq){.pa = cmd->prp1, .size = qsize, .irq_vec = iv, .phase = 1, .ien = ien, .valid = 1};
        }
        break;
    }
    case 0x06: /* Identify */
        status = handle_identify(nv, cmd);
        break;
    case 0x08: /* Abort: command not aborted (0) */
        dw0 = 1; /* bit 0 = command not aborted */
        break;
    case 0x09: { /* Set Features */
        uint8_t fid = (uint8_t)(cmd->cdw10 & 0xff);
        switch (fid) {
        case 0x01: nv->arb_feat = cmd->cdw11; break;
        case 0x02: nv->pm_feat = cmd->cdw11; break;
        case 0x04: nv->temp_thresh = cmd->cdw11; break;
        case 0x06: nv->vwc_feat = cmd->cdw11 & 1; break;
        case 0x07: dw0 = nv->num_queues_feat; break; /* Number of Queues (0-based): 15 SQs | 15 CQs */
        case 0x08: case 0x09: case 0x0b: case 0x0c: break;
        default: status = 0x0002; break;
        }
        break;
    }
    case 0x0a: { /* Get Features */
        uint8_t fid = (uint8_t)(cmd->cdw10 & 0xff);
        switch (fid) {
        case 0x01: dw0 = nv->arb_feat; break;
        case 0x02: dw0 = nv->pm_feat; break;
        case 0x04: dw0 = nv->temp_thresh; break;
        case 0x06: dw0 = nv->vwc_feat; break;
        case 0x07: dw0 = nv->num_queues_feat; break;
        case 0x08: case 0x09: case 0x0b: case 0x0c: dw0 = 0; break;
        default: status = 0x0002; break;
        }
        break;
    }
    case 0x0c: /* Async Event Request: keep parked without completing */
        return;
    default:
        status = 0x0001; /* Invalid Command Opcode */
        break;
    }

    post_cqe(nv, sq, 0, cmd->cid, dw0, status);
}

static uint8_t *ensure_io_buf(HvNvme *nv, uint32_t bytes) {
    if (bytes <= nv->io_buf_cap) return nv->io_buf;
    uint8_t *nb = realloc(nv->io_buf, bytes);
    if (!nb) return NULL;
    nv->io_buf = nb;
    nv->io_buf_cap = bytes;
    return nb;
}

static void process_io_cmd(HvNvme *nv, HvNvmeSq *sq, uint16_t sqid, const NvmeCmd *cmd) {
    uint16_t status = 0;

    if (cmd->nsid != 1 && cmd->nsid != 0xffffffffu) {
        post_cqe(nv, sq, sqid, cmd->cid, 0, 0x000b);
        return;
    }

    if (nv->disk->readonly && (cmd->opc == 0x01 || cmd->opc == 0x08 ||
                              (cmd->opc == 0x09 && (cmd->cdw11 & 4)))) {
        post_cqe(nv, sq, sqid, cmd->cid, 0, 0x0020); /* Namespace is Write Protected */
        return;
    }

    switch (cmd->opc) {
    case 0x00: /* Flush */
        if (hv_disk_flush(nv->disk) != 0) status = 0x0004;
        break;
    case 0x01:   /* Write */
    case 0x02: { /* Read */
        uint64_t slba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
        uint32_t nlb = (cmd->cdw12 & 0xffff) + 1u;
        uint64_t off = slba * 512ull;
        uint32_t bytes = nlb * 512u;
        uint64_t blocks = nv->disk->size / 512ull;
        if (bytes > NVME_MAX_XFER || slba > blocks || nlb > blocks - slba) {
            status = 0x0080; /* LBA Out of Range */
            break;
        }
        uint8_t *buf = ensure_io_buf(nv, bytes);
        if (!buf) {
            status = 0x0004;
            break;
        }
        if (cmd->opc == 0x02) {
            if (hv_disk_read(nv->disk, off, buf, bytes) != 0 ||
                nvme_dma_prp(nv, cmd->prp1, cmd->prp2, buf, bytes, 1) != 0) {
                status = 0x0004;
            } else {
                nv->rd_bytes += bytes;
            }
        } else {
            if (nvme_dma_prp(nv, cmd->prp1, cmd->prp2, buf, bytes, 0) != 0 ||
                hv_disk_write(nv->disk, off, buf, bytes) != 0) {
                status = 0x0004;
            } else {
                nv->wr_bytes += bytes;
            }
        }
        break;
    }
    case 0x08: { /* Write Zeroes */
        uint64_t slba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
        uint32_t nlb = (cmd->cdw12 & 0xffff) + 1u;
        uint64_t off = slba * 512ull;
        uint32_t bytes = nlb * 512u;
        uint64_t blocks = nv->disk->size / 512ull;
        if (slba > blocks || nlb > blocks - slba) {
            status = 0x0080;
            break;
        }
        if (cmd->cdw12 & (1u << 25)) {
            if (hv_disk_discard(nv->disk, off, bytes) != 0) status = 0x0004;
            else {
                nv->trim_n++;
                nv->trim_bytes += bytes;
            }
            break;
        }
        uint8_t zero[4096] = {0};
        while (bytes > 0) {
            uint32_t chunk = bytes < sizeof(zero) ? bytes : (uint32_t)sizeof(zero);
            if (hv_disk_write(nv->disk, off, zero, chunk) != 0) {
                status = 0x0004;
                break;
            }
            off += chunk;
            bytes -= chunk;
        }
        break;
    }
    case 0x09: { /* Dataset Management (TRIM / Deallocate) */
        static int trim_off = -1; /* HV_NO_TRIM: accept and ignore, as before deallocation was real */
        if (trim_off < 0) trim_off = getenv("HV_NO_TRIM") != NULL;
        if (!(cmd->cdw11 & 4) || trim_off) break;
        uint32_t nr = (cmd->cdw10 & 0xff) + 1u;
        uint8_t ranges[256 * 16];
        if (nvme_dma_prp(nv, cmd->prp1, cmd->prp2, ranges, nr * 16, 0) != 0) {
            status = 0x0004;
            break;
        }
        uint64_t blocks = nv->disk->size / 512ull;
        for (uint32_t i = 0; i < nr; i++) {
            uint32_t nlb;
            uint64_t slba;
            memcpy(&nlb, ranges + i * 16 + 4, 4);
            memcpy(&slba, ranges + i * 16 + 8, 8);
            if (nlb && (slba > blocks || nlb > blocks - slba)) {
                status = 0x0080;
                break;
            }
        }
        if (status) break;
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < nr; i++) {
            uint32_t nlb;
            uint64_t slba;
            memcpy(&nlb, ranges + i * 16 + 4, 4);
            memcpy(&slba, ranges + i * 16 + 8, 8);
            if (!nlb) continue;
            uint64_t len = (uint64_t)nlb * 512ull;
            if (hv_disk_discard(nv->disk, slba * 512ull, len) != 0) {
                status = 0x0004;
                break;
            }
            bytes += len;
            nv->trim_bytes += len;
        }
        if (!status && bytes) nv->trim_n++;
        break;
    }
    default:
        status = 0x0001; /* Invalid Command Opcode */
        break;
    }

    if (status) { /* Windows turns these into paging and file system errors: say which command failed */
        static int logged;
        if (logged < 100) {
            logged++;
            fprintf(stderr, "nvme[%d] io opc=%02x sq=%u cid=%u cdw10=%08x cdw11=%08x cdw12=%08x prp1=%llx prp2=%llx: status %04x\n",
                    nv->pci.slot, cmd->opc, sqid, cmd->cid, cmd->cdw10, cmd->cdw11, cmd->cdw12,
                    (unsigned long long)cmd->prp1, (unsigned long long)cmd->prp2, status);
        }
    }
    post_cqe(nv, sq, sqid, cmd->cid, 0, status);
}

static void drain_sq(HvNvme *nv, uint16_t qid) {
    HvNvmeSq *sq = &nv->sq[qid];
    if (!sq->valid || !sq->size) return;
    int guard = 0;
    while (sq->head != sq->tail && guard++ < 4096) {
        NvmeCmd cmd;
        uint64_t cmd_pa = sq->pa + (uint64_t)sq->head * 64u;
        if (hv_phys_read(nv->m, cmd_pa, &cmd, 64) != 0) break;
        if (++sq->head >= sq->size) sq->head = 0;
        if (qid == 0) process_admin_cmd(nv, sq, &cmd);
        else process_io_cmd(nv, sq, qid, &cmd);
    }
}

static void nvme_reset_ctrl(HvNvme *nv) {
    memset(nv->sq, 0, sizeof nv->sq);
    memset(nv->cq, 0, sizeof nv->cq);
    nv->csts = 0;
    nvme_update_intx(nv);
}

static void nvme_enable_ctrl(HvNvme *nv) {
    uint16_t asqs = (uint16_t)((nv->aqa & 0xfff) + 1u);
    uint16_t acqs = (uint16_t)(((nv->aqa >> 16) & 0xfff) + 1u);
    nv->sq[0] = (HvNvmeSq){.pa = nv->asq & ~0xfffull, .size = asqs, .cqid = 0, .valid = 1};
    nv->cq[0] = (HvNvmeCq){.pa = nv->acq & ~0xfffull, .size = acqs, .irq_vec = 0, .phase = 1, .ien = 1, .valid = 1};
    nv->csts = 1u; /* RDY */
}

static uint64_t nvme_bar_read(HvPciDev *d, int bar, uint64_t off, unsigned size) {
    HvNvme *nv = d->opaque;
    if (bar != 0) return ~0ull;
    pthread_mutex_lock(&nv->mu);
    uint64_t val = 0;
    if (size == 8) {
        switch (off) {
        case 0x00: val = nv->cap; break;
        case 0x28: val = nv->asq; break;
        case 0x30: val = nv->acq; break;
        default: break;
        }
    } else {
        uint64_t base = off & ~3ull;
        uint32_t w = 0;
        switch (base) {
        case 0x00: w = (uint32_t)nv->cap; break;
        case 0x04: w = (uint32_t)(nv->cap >> 32); break;
        case 0x08: w = nv->vs; break;
        case 0x0c: w = nv->intms; break;
        case 0x10: w = nv->intms; break;
        case 0x14: w = nv->cc; break;
        case 0x1c: w = nv->csts; break;
        case 0x24: w = nv->aqa; break;
        case 0x28: w = (uint32_t)nv->asq; break;
        case 0x2c: w = (uint32_t)(nv->asq >> 32); break;
        case 0x30: w = (uint32_t)nv->acq; break;
        case 0x34: w = (uint32_t)(nv->acq >> 32); break;
        default: break;
        }
        val = (w >> ((off & 3) * 8)) & (size >= 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
    }
    pthread_mutex_unlock(&nv->mu);
    return val;
}

static void nvme_bar_write(HvPciDev *d, int bar, uint64_t off, uint64_t val, unsigned size) {
    HvNvme *nv = d->opaque;
    if (bar != 0) return;
    if (getenv("HV_NVME_LOG") && off < 0x1000)
        fprintf(stderr, "nvme[%d] bar_write %03llx (%u) = %llx\n",
                d->slot, (unsigned long long)off, size, (unsigned long long)val);
    pthread_mutex_lock(&nv->mu);

    if (off >= 0x1000 && off < 0x1000 + HV_NVME_MAX_QUEUES * 8) {
        uint32_t idx = (uint32_t)((off - 0x1000) >> 2);
        uint16_t qid = (uint16_t)(idx >> 1);
        int is_cq = idx & 1;
        if (qid < HV_NVME_MAX_QUEUES && (nv->csts & 1)) {
            if (!is_cq) {
                if (nv->sq[qid].valid && nv->sq[qid].size) {
                    nv->sq[qid].tail = (uint16_t)(val % nv->sq[qid].size);
                    drain_sq(nv, qid);
                }
            } else {
                if (nv->cq[qid].valid && nv->cq[qid].size) {
                    nv->cq[qid].head = (uint16_t)(val % nv->cq[qid].size);
                    if (nv->cq[qid].head == nv->cq[qid].tail) {
                        nv->cq[qid].irq_pending = 0;
                        nvme_update_intx(nv);
                    }
                }
            }
        }
        pthread_mutex_unlock(&nv->mu);
        return;
    }

    if (size == 8) {
        if (off == 0x28) nv->asq = val;
        else if (off == 0x30) nv->acq = val;
        pthread_mutex_unlock(&nv->mu);
        return;
    }

    uint32_t w = (uint32_t)val;
    switch (off & ~3ull) {
    case 0x0c: /* INTMS */
        nv->intms |= w;
        nvme_update_intx(nv);
        break;
    case 0x10: /* INTMC */
        nv->intms &= ~w;
        nvme_update_intx(nv);
        break;
    case 0x14: { /* CC */
        uint32_t old_cc = nv->cc;
        nv->cc = w;
        if (!(old_cc & 1) && (w & 1)) {
            nvme_enable_ctrl(nv);
        } else if ((old_cc & 1) && !(w & 1)) {
            nvme_reset_ctrl(nv);
        }
        uint32_t shn = (w >> 14) & 3;
        if (shn != 0) {
            hv_disk_flush(nv->disk);
            nv->csts = (nv->csts & ~(3u << 2)) | (2u << 2); /* SHST = 2 (shutdown complete) */
        }
        break;
    }
    case 0x24: nv->aqa = w; break;
    case 0x28: nv->asq = (nv->asq & 0xffffffff00000000ull) | w; break;
    case 0x2c: nv->asq = (nv->asq & 0x00000000ffffffffull) | ((uint64_t)w << 32); break;
    case 0x30: nv->acq = (nv->acq & 0xffffffff00000000ull) | w; break;
    case 0x34: nv->acq = (nv->acq & 0x00000000ffffffffull) | ((uint64_t)w << 32); break;
    default: break;
    }

    pthread_mutex_unlock(&nv->mu);
}

HvNvme *hv_nvme_create(HvMachine *m, HvPcie *bus, HvDisk *disk, const char *serial) {
    HvNvme *nv = calloc(1, sizeof *nv);
    nv->m = m;
    nv->disk = disk;
    if (serial) snprintf(nv->serial, sizeof nv->serial, "%s", serial);
    pthread_mutex_init(&nv->mu, NULL);

    /* CAP: MQES=1023, CQR=1, TO=20, DSTRD=0, CSS=1 (NVM), MPSMIN=0 (4KB), MPSMAX=4 (64KB) */
    nv->cap = 1023ull | (1ull << 16) | (20ull << 24) | (1ull << 37) | (0ull << 48) | (4ull << 52);
    nv->vs = 0x00010400u; /* NVMe 1.4.0 */
    nv->temp_thresh = 358; /* 85 C */
    nv->vwc_feat = 1;
    nv->num_queues_feat = (15u << 16) | 15u; /* 16 I/O SQs & 16 I/O CQs (0-based) */

    hv_pci_init(&nv->pci, 0x1b36, 0x0010, 0x01080202u, 0x1af4, 0x1100);
    hv_pci_add_bar(&nv->pci, 0, NVME_BAR_SIZE, 1);
    hv_pci_add_pm_cap(&nv->pci, 0x60);
    hv_pci_add_pcie_cap(&nv->pci, 0x80);
    hv_pci_add_msix(&nv->pci, 0x40, 0, 0x2000, 0x3000, HV_NVME_MAX_QUEUES);
    nv->pci.opaque = nv;
    nv->pci.bar_read = nvme_bar_read;
    nv->pci.bar_write = nvme_bar_write;
    hv_pcie_attach(bus, &nv->pci);
    return nv;
}
