#include "fw_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* QEMU fw_cfg MMIO & DMA protocol (hw/nvram/fw_cfg.c):
   +0x00..0x07: Data register (raw byte stream)
   +0x08..0x09: Selector register (16-bit BIG-ENDIAN!)
   +0x10..0x17: DMA descriptor physical address (64-bit BIG-ENDIAN; writing +0x14 triggers DMA)
   FWCfgDmaAccess in guest RAM (all fields 32/64-bit BIG-ENDIAN):
     uint32_t control;  // bit 0 ERROR, bit 1 READ, bit 2 SKIP, bit 3 SELECT (sel in bits 31:16), bit 4 WRITE
     uint32_t length;
     uint64_t address;
*/

#define FW_CFG_DMA_CTL_ERROR  0x01u
#define FW_CFG_DMA_CTL_READ   0x02u
#define FW_CFG_DMA_CTL_SKIP   0x04u
#define FW_CFG_DMA_CTL_SELECT 0x08u
#define FW_CFG_DMA_CTL_WRITE  0x10u

static uint16_t bswap16(uint16_t x) { return __builtin_bswap16(x); }
static uint32_t bswap32(uint32_t x) { return __builtin_bswap32(x); }
static uint64_t bswap64(uint64_t x) { return __builtin_bswap64(x); }

static void rebuild_dir(HvFwCfg *fc) {
    free(fc->dir_blob);
    uint32_t total = 4u + (uint32_t)fc->nfiles * 64u;
    fc->dir_blob = calloc(1, total);
    fc->dir_len = total;
    uint32_t count_be = bswap32((uint32_t)fc->nfiles);
    memcpy(fc->dir_blob, &count_be, 4);
    for (int i = 0; i < fc->nfiles; i++) {
        uint8_t *e = fc->dir_blob + 4 + i * 64;
        uint32_t sz_be = bswap32(fc->files[i].len);
        uint16_t sel_be = bswap16((uint16_t)(0x0020 + i));
        memcpy(e + 0, &sz_be, 4);
        memcpy(e + 4, &sel_be, 2);
        memcpy(e + 8, fc->files[i].name, 56);
    }
}

static uint8_t *entry_lookup(HvFwCfg *fc, uint16_t sel, uint32_t *len_out, int *writable_out, int *file_idx_out) {
    *writable_out = 0;
    *file_idx_out = -1;
    if (sel == 0x0019) {
        *len_out = fc->dir_len;
        return fc->dir_blob;
    }
    if (sel < 0x0020) {
        if (!fc->fixed_len[sel]) return NULL;
        *len_out = fc->fixed_len[sel];
        return fc->fixed[sel];
    }
    int idx = (int)sel - 0x0020;
    if (idx >= 0 && idx < fc->nfiles) {
        *len_out = fc->files[idx].len;
        *writable_out = fc->files[idx].writable;
        *file_idx_out = idx;
        return fc->files[idx].data;
    }
    return NULL;
}

static void run_dma(HvFwCfg *fc, uint64_t dma_pa) {
    uint8_t desc[16];
    if (hv_phys_read(fc->m, dma_pa, desc, 16) != 0) return;

    uint32_t ctl_be, len_be;
    uint64_t addr_be;
    memcpy(&ctl_be, desc + 0, 4);
    memcpy(&len_be, desc + 4, 4);
    memcpy(&addr_be, desc + 8, 8);
    uint32_t ctl = bswap32(ctl_be);
    uint32_t len = bswap32(len_be);
    uint64_t addr = bswap64(addr_be);

    if (ctl & FW_CFG_DMA_CTL_SELECT) {
        fc->cur_sel = (uint16_t)(ctl >> 16);
        fc->cur_off = 0;
    }

    uint32_t elen = 0;
    int writable = 0, fidx = -1;
    uint8_t *ebuf = entry_lookup(fc, fc->cur_sel, &elen, &writable, &fidx);
    uint32_t out_ctl = 0;

    if (ctl & FW_CFG_DMA_CTL_READ) {
        uint32_t pos = 0;
        while (pos < len) {
            uint32_t chunk = len - pos;
            if (chunk > 4096) chunk = 4096;
            uint8_t tmp[4096];
            memset(tmp, 0, chunk);
            if (ebuf && fc->cur_off < elen) {
                uint32_t avail = elen - fc->cur_off;
                if (avail > chunk) avail = chunk;
                memcpy(tmp, ebuf + fc->cur_off, avail);
            }
            if (hv_phys_write(fc->m, addr + pos, tmp, chunk) != 0) {
                out_ctl = FW_CFG_DMA_CTL_ERROR;
                break;
            }
            fc->cur_off += chunk;
            pos += chunk;
        }
    } else if (ctl & FW_CFG_DMA_CTL_WRITE) {
        if (!ebuf || !writable) {
            out_ctl = FW_CFG_DMA_CTL_ERROR;
        } else {
            uint32_t avail = fc->cur_off < elen ? elen - fc->cur_off : 0;
            uint32_t n = len < avail ? len : avail;
            if (n > 0 && hv_phys_read(fc->m, addr, ebuf + fc->cur_off, n) != 0) {
                out_ctl = FW_CFG_DMA_CTL_ERROR;
            } else {
                fc->cur_off += n;
                if (fidx >= 0 && fc->files[fidx].on_write)
                    fc->files[fidx].on_write(fc->files[fidx].write_opaque, ebuf, elen);
            }
        }
    } else if (ctl & FW_CFG_DMA_CTL_SKIP) {
        fc->cur_off += len;
    }

    uint32_t done_be = bswap32(out_ctl);
    hv_phys_write(fc->m, dma_pa, &done_be, 4);
}

static uint64_t fw_cfg_read(void *opaque, uint64_t off, unsigned size) {
    HvFwCfg *fc = opaque;
    if (off < 8) {
        uint32_t elen = 0;
        int writable = 0, fidx = -1;
        uint8_t *ebuf = entry_lookup(fc, fc->cur_sel, &elen, &writable, &fidx);
        uint8_t tmp[8] = {0};
        unsigned n = size > 8 ? 8 : size;
        for (unsigned i = 0; i < n; i++) {
            if (ebuf && fc->cur_off < elen) tmp[i] = ebuf[fc->cur_off];
            fc->cur_off++;
        }
        uint64_t v = 0;
        memcpy(&v, tmp, n);
        return v;
    }
    if (off == 0x10 && size == 8) {
        /* Signature "QEMU CFG" in big-endian when reading DMA reg */
        return bswap64(0x51454d5520434647ull);
    }
    if (off == 0x10 && size == 4) return bswap32(0x51454d55u);
    if (off == 0x14 && size == 4) return bswap32(0x20434647u);
    return 0;
}

static void fw_cfg_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    HvFwCfg *fc = opaque;
    if (off == 0x08 && size == 2) {
        fc->cur_sel = bswap16((uint16_t)val);
        fc->cur_off = 0;
    } else if (off == 0x10 && size == 8) {
        uint64_t pa = bswap64(val);
        fc->dma_hi = 0;
        run_dma(fc, pa);
    } else if (off == 0x10 && size == 4) {
        fc->dma_hi = bswap32((uint32_t)val);
    } else if (off == 0x14 && size == 4) {
        uint64_t pa = ((uint64_t)fc->dma_hi << 32) | bswap32((uint32_t)val);
        fc->dma_hi = 0;
        run_dma(fc, pa);
    }
}

int hv_fw_cfg_add_file(HvFwCfg *fc, const char *name, const void *data, uint32_t len) {
    if (fc->nfiles >= HV_FW_CFG_MAX_FILES) return -1;
    int idx = fc->nfiles++;
    memset(&fc->files[idx], 0, sizeof fc->files[idx]);
    strncpy(fc->files[idx].name, name, sizeof(fc->files[idx].name) - 1);
    fc->files[idx].data = calloc(1, len ? len : 1);
    if (len && data) memcpy(fc->files[idx].data, data, len);
    fc->files[idx].len = len;
    rebuild_dir(fc);
    return idx;
}

int hv_fw_cfg_add_writable_file(HvFwCfg *fc, const char *name, uint32_t len, HvFwCfgWriteCb cb, void *opaque) {
    int idx = hv_fw_cfg_add_file(fc, name, NULL, len);
    if (idx < 0) return -1;
    fc->files[idx].writable = 1;
    fc->files[idx].on_write = cb;
    fc->files[idx].write_opaque = opaque;
    return idx;
}

HvFwCfg *hv_fw_cfg_create(HvMachine *m, uint64_t pa, uint16_t ncpus) {
    HvFwCfg *fc = calloc(1, sizeof *fc);
    fc->m = m;
    /* 0x0000: signature "QEMU" */
    memcpy(fc->fixed[0x00], "QEMU", 4);
    fc->fixed_len[0x00] = 4;
    /* 0x0001: ID = 3 (VERSION | DMA) */
    uint32_t id = 3;
    memcpy(fc->fixed[0x01], &id, 4);
    fc->fixed_len[0x01] = 4;
    /* 0x0004: nographic = 0 (we have graphical GOP!) */
    uint16_t nogfx = 0;
    memcpy(fc->fixed[0x04], &nogfx, 2);
    fc->fixed_len[0x04] = 2;
    /* 0x0005: nb_cpus, 0x000f: max_cpus */
    memcpy(fc->fixed[0x05], &ncpus, 2);
    fc->fixed_len[0x05] = 2;
    memcpy(fc->fixed[0x0f], &ncpus, 2);
    fc->fixed_len[0x0f] = 2;
    /* 0x000e and etc/boot-menu-wait: the boot menu "on" with a wait of 0 ms, as QEMU's -boot menu=on,splash-time=0.
       The firmware's boot manager then starts the first boot option at once; without them it waits its default
       of five seconds for a key, on every boot, and nobody presses one on a phone. HV_BOOT_MENU_WAIT_MS asks for
       another wait (the firmware counts whole seconds, rounded up), for one who wants its menu. A negative one
       leaves it to the firmware, which keeps the last wait it was given in its NVRAM (the Timeout variable). */
    const char *wait = getenv("HV_BOOT_MENU_WAIT_MS");
    long wait_ms = wait ? strtol(wait, NULL, 10) : 0;
    if (wait_ms >= 0) {
        uint16_t menu = 1, ms = wait_ms > 0xffff ? 0xffff : (uint16_t)wait_ms;
        memcpy(fc->fixed[0x0e], &menu, 2);
        fc->fixed_len[0x0e] = 2;
        hv_fw_cfg_add_file(fc, "etc/boot-menu-wait", &ms, 2);
    }

    rebuild_dir(fc);
    hv_machine_add_mmio(m, pa, 0x1000, fc, fw_cfg_read, fw_cfg_write, "fw_cfg");
    return fc;
}
