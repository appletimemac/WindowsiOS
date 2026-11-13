/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "acpi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} Buf;

static void b_grow(Buf *b, size_t add) {
    if (b->len + add <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 1024;
    while (ncap < b->len + add) ncap *= 2;
    uint8_t *p = realloc(b->buf, ncap);
    if (!p) abort();
    b->buf = p;
    b->cap = ncap;
}

static void b_put(Buf *b, const void *src, size_t n) {
    b_grow(b, n);
    if (n) memcpy(b->buf + b->len, src, n);
    b->len += n;
}

static void b_zero(Buf *b, size_t n) {
    b_grow(b, n);
    memset(b->buf + b->len, 0, n);
    b->len += n;
}

static void b_u8(Buf *b, uint8_t v) { b_put(b, &v, 1); }
static void b_u16(Buf *b, uint16_t v) { b_put(b, &v, 2); }
static void b_u32(Buf *b, uint32_t v) { b_put(b, &v, 4); }
static void b_u64(Buf *b, uint64_t v) { b_put(b, &v, 8); }

static uint8_t acpi_cksum(const uint8_t *p, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return (uint8_t)(0u - sum);
}

static void acpi_hdr(Buf *b, const char sig[4], uint32_t len, uint8_t rev, const char oem_table[8]) {
    b_put(b, sig, 4);
    b_u32(b, len);
    b_u8(b, rev);
    b_u8(b, 0); /* checksum filled by finalize_table and BIOS linker */
    b_put(b, "WINIOS", 6);
    b_put(b, oem_table, 8);
    b_u32(b, 1); /* OEM revision */
    b_put(b, "WIOS", 4);
    b_u32(b, 1); /* creator revision */
}

static void finalize_table(Buf *tabs, size_t start) {
    uint32_t len = (uint32_t)(tabs->len - start);
    memcpy(tabs->buf + start + 4, &len, 4);
    tabs->buf[start + 9] = 0; /* BIOS linker ADD_CHECKSUM computes checksum */
}

/* ---- BIOS Linker/Loader (etc/table-loader) ---- */
typedef struct {
    Buf buf;
} Loader;

static void loader_alloc(Loader *l, const char *file, uint32_t align, uint8_t zone) {
    uint8_t ent[128] = {0};
    uint32_t cmd = 1;
    memcpy(ent + 0, &cmd, 4);
    strncpy((char *)(ent + 4), file, 55);
    memcpy(ent + 60, &align, 4);
    ent[64] = zone;
    b_put(&l->buf, ent, 128);
}

static void loader_add_ptr(Loader *l, const char *dst, const char *src, uint32_t dst_off, uint8_t ptr_size) {
    uint8_t ent[128] = {0};
    uint32_t cmd = 2;
    memcpy(ent + 0, &cmd, 4);
    strncpy((char *)(ent + 4), dst, 55);
    strncpy((char *)(ent + 60), src, 55);
    memcpy(ent + 116, &dst_off, 4);
    ent[120] = ptr_size;
    b_put(&l->buf, ent, 128);
}

static void loader_add_cksum(Loader *l, const char *file, uint32_t cksum_off, uint32_t start, uint32_t len) {
    uint8_t ent[128] = {0};
    uint32_t cmd = 3;
    memcpy(ent + 0, &cmd, 4);
    strncpy((char *)(ent + 4), file, 55);
    memcpy(ent + 60, &cksum_off, 4);
    memcpy(ent + 64, &start, 4);
    memcpy(ent + 68, &len, 4);
    b_put(&l->buf, ent, 128);
}

/* ---- AML Builder Helpers ---- */
static void aml_pkg_len(Buf *b, size_t body_len) {
    if (body_len + 1 < 64) {
        b_u8(b, (uint8_t)(body_len + 1));
    } else if (body_len + 2 < (1u << 12)) {
        uint32_t tot = (uint32_t)(body_len + 2);
        b_u8(b, (uint8_t)(0x40u | (tot & 0x0fu)));
        b_u8(b, (uint8_t)((tot >> 4) & 0xffu));
    } else if (body_len + 3 < (1u << 20)) {
        uint32_t tot = (uint32_t)(body_len + 3);
        b_u8(b, (uint8_t)(0x80u | (tot & 0x0fu)));
        b_u8(b, (uint8_t)((tot >> 4) & 0xffu));
        b_u8(b, (uint8_t)((tot >> 12) & 0xffu));
    } else {
        uint32_t tot = (uint32_t)(body_len + 4);
        b_u8(b, (uint8_t)(0xc0u | (tot & 0x0fu)));
        b_u8(b, (uint8_t)((tot >> 4) & 0xffu));
        b_u8(b, (uint8_t)((tot >> 12) & 0xffu));
        b_u8(b, (uint8_t)((tot >> 20) & 0xffu));
    }
}

static void aml_int(Buf *b, uint64_t v) {
    if (v == 0) {
        b_u8(b, 0x00); /* ZeroOp */
    } else if (v == 1) {
        b_u8(b, 0x01); /* OneOp */
    } else if (v <= 0xff) {
        b_u8(b, 0x0a); /* BytePrefix */
        b_u8(b, (uint8_t)v);
    } else if (v <= 0xffff) {
        b_u8(b, 0x0b); /* WordPrefix */
        b_u16(b, (uint16_t)v);
    } else if (v <= 0xffffffffull) {
        b_u8(b, 0x0c); /* DWordPrefix */
        b_u32(b, (uint32_t)v);
    } else {
        b_u8(b, 0x0e); /* QWordPrefix */
        b_u64(b, v);
    }
}

static void aml_str(Buf *b, const char *s) {
    b_u8(b, 0x0d); /* StringPrefix */
    b_put(b, s, strlen(s) + 1);
}

static void aml_name_int(Buf *b, const char name[4], uint64_t v) {
    b_u8(b, 0x08); /* NameOp */
    b_put(b, name, 4);
    aml_int(b, v);
}

static void aml_name_str(Buf *b, const char name[4], const char *s) {
    b_u8(b, 0x08); /* NameOp */
    b_put(b, name, 4);
    aml_str(b, s);
}

static void aml_buffer(Buf *b, const void *data, size_t n) {
    Buf inner = {0};
    aml_int(&inner, n);
    b_put(&inner, data, n);
    b_u8(b, 0x11); /* BufferOp */
    aml_pkg_len(b, inner.len);
    b_put(b, inner.buf, inner.len);
    free(inner.buf);
}

static void aml_name_buf(Buf *b, const char name[4], const void *data, size_t n) {
    b_u8(b, 0x08);
    b_put(b, name, 4);
    aml_buffer(b, data, n);
}

static void aml_device(Buf *b, const char name[4], const Buf *body) {
    b_u8(b, 0x5b);
    b_u8(b, 0x82); /* DeviceOp */
    aml_pkg_len(b, 4 + body->len);
    b_put(b, name, 4);
    b_put(b, body->buf, body->len);
}

static void aml_crs_mem32_irq(Buf *crs, uint32_t base, uint32_t len, uint32_t gsi, int has_irq, int edge, int shared) {
    if (len) {
        /* Memory32Fixed descriptor (0x86, length 9) */
        b_u8(crs, 0x86);
        b_u16(crs, 9);
        b_u8(crs, 1); /* ReadWrite */
        b_u32(crs, base);
        b_u32(crs, len);
    }
    if (has_irq) {
        /* Extended Interrupt descriptor (0x89, length 6) */
        b_u8(crs, 0x89);
        b_u16(crs, 6);
        uint8_t flags = 0x01; /* ResourceConsumer */
        if (edge) flags |= 0x02;
        if (shared) flags |= 0x08;
        b_u8(crs, flags);
        b_u8(crs, 1); /* count */
        b_u32(crs, gsi);
    }
    /* EndTag (0x79, 0x00) */
    b_u8(crs, 0x79);
    b_u8(crs, 0x00);
}

static void build_dsdt(Buf *tabs, int ncpus) {
    size_t start = tabs->len;
    acpi_hdr(tabs, "DSDT", 0, 2, "WINVIRT ");

    Buf sb = {0};

    /* Processor devices C000 .. C0xx */
    for (int i = 0; i < ncpus; i++) {
        char cname[5];
        snprintf(cname, sizeof cname, "C%03X", i);
        Buf dev = {0};
        aml_name_str(&dev, "_HID", "ACPI0007");
        aml_name_int(&dev, "_UID", (uint64_t)i);
        aml_device(&sb, cname, &dev);
        free(dev.buf);
    }

    /* PL011 UART COM0 at 0x09000000, GSI 33 (SPI 1) */
    {
        Buf dev = {0}, crs = {0};
        aml_name_str(&dev, "_HID", "ARMH0011");
        aml_name_int(&dev, "_UID", 0);
        aml_crs_mem32_irq(&crs, 0x09000000U, 0x1000U, 33U, 1, 0, 0);
        aml_name_buf(&dev, "_CRS", crs.buf, crs.len);
        aml_device(&sb, "COM0", &dev);
        free(crs.buf);
        free(dev.buf);
    }

    /* QEMU fw_cfg FWCF at 0x09020000 */
    {
        Buf dev = {0}, crs = {0};
        aml_name_str(&dev, "_HID", "QEMU0002");
        aml_name_int(&dev, "_STA", 0x0b);
        aml_name_int(&dev, "_CCA", 1);
        aml_crs_mem32_irq(&crs, 0x09020000U, 0x18U, 0, 0, 0, 0);
        aml_name_buf(&dev, "_CRS", crs.buf, crs.len);
        aml_device(&sb, "FWCF", &dev);
        free(crs.buf);
        free(dev.buf);
    }

    /* PCI Interrupt Link devices L000..L003 -> GSIs 35..38 (SPIs 3..6) */
    for (int i = 0; i < 4; i++) {
        char lname[5];
        snprintf(lname, sizeof lname, "L00%d", i);
        Buf dev = {0}, crs = {0};
        aml_name_str(&dev, "_HID", "PNP0C0F");
        aml_name_int(&dev, "_UID", (uint64_t)i);
        aml_crs_mem32_irq(&crs, 0, 0, 35U + (uint32_t)i, 1, 0, 0);
        aml_name_buf(&dev, "_PRS", crs.buf, crs.len);
        aml_name_buf(&dev, "_CRS", crs.buf, crs.len);
        /* Method (_SRS, 1, NotSerialized) {} -> 0x14, pkg_len=6, "_SRS", 0x01 */
        b_u8(&dev, 0x14);
        b_u8(&dev, 6);
        b_put(&dev, "_SRS", 4);
        b_u8(&dev, 0x01);
        aml_device(&sb, lname, &dev);
        free(crs.buf);
        free(dev.buf);
    }

    /* PCIe Root Complex PCI0 */
    {
        Buf dev = {0};
        aml_name_str(&dev, "_HID", "PNP0A08");
        aml_name_str(&dev, "_CID", "PNP0A03");
        aml_name_int(&dev, "_SEG", 0);
        aml_name_int(&dev, "_BBN", 0);
        aml_name_int(&dev, "_UID", 0);
        aml_name_int(&dev, "_CCA", 1);

        /* _PRT: VarPackageOp (0x13) of 128 4-element Packages (slots 0..31 x pins 0..3) */
        {
            Buf prt_items = {0};
            aml_int(&prt_items, 128);
            for (uint32_t slot = 0; slot < 32; slot++) {
                for (uint32_t pin = 0; pin < 4; pin++) {
                    Buf ent = {0};
                    b_u8(&ent, 4); /* 4 elements */
                    aml_int(&ent, (slot << 16) | 0xffffU);
                    aml_int(&ent, pin);
                    char lname[5];
                    snprintf(lname, sizeof lname, "L00%u", (slot + pin) & 3U);
                    b_put(&ent, lname, 4); /* NameString reference to L00x */
                    aml_int(&ent, 0);

                    b_u8(&prt_items, 0x12); /* PackageOp */
                    aml_pkg_len(&prt_items, ent.len);
                    b_put(&prt_items, ent.buf, ent.len);
                    free(ent.buf);
                }
            }
            b_u8(&dev, 0x08);
            b_put(&dev, "_PRT", 4);
            b_u8(&dev, 0x13); /* VarPackageOp */
            aml_pkg_len(&dev, prt_items.len);
            b_put(&dev, prt_items.buf, prt_items.len);
            free(prt_items.buf);
        }

        /* Method (_CBA, 0) { Return (0x4010000000) } */
        {
            static const uint8_t cba_aml[] = {
                0x14, 0x10, '_', 'C', 'B', 'A', 0x00,
                0xa4, 0x0e, 0x00, 0x00, 0x00, 0x10, 0x40, 0x00, 0x00, 0x00,
            };
            b_put(&dev, cba_aml, sizeof cba_aml);
        }

        /* _CRS: Bus 0x00..0xff, MMIO32 0x10000000..0x3efeffff, PIO 0x0000..0xffff (+0x3eff0000), MMIO64 0x80_00000000..0xff_ffffffff */
        {
            static const uint8_t pci_crs[] = {
                /* WordBusNumber (0x88, len 0x0d): 0x0000..0x00ff */
                0x88, 0x0d, 0x00, 0x02, 0x0c, 0x00, 0x00, 0x00,
                0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01,
                /* DWordMemory (0x87, len 0x17): 0x10000000..0x3efeffff, len 0x2eff0000 */
                0x87, 0x17, 0x00, 0x00, 0x0c, 0x01, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x10, 0xff, 0xff, 0xfe, 0x3e,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x2e,
                /* DWordIO (0x87, len 0x17): 0x0000..0xffff, xlat 0x3eff0000, len 0x10000 */
                0x87, 0x17, 0x00, 0x01, 0x0c, 0x03, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
                0x00, 0x00, 0xff, 0x3e, 0x00, 0x00, 0x01, 0x00,
                /* QWordMemory (0x8a, len 0x2b): 0x80_00000000..0xff_ffffffff, len 0x80_00000000 */
                0x8a, 0x2b, 0x00, 0x00, 0x0c, 0x01,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
                0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
                /* EndTag */
                0x79, 0x00,
            };
            aml_name_buf(&dev, "_CRS", pci_crs, sizeof pci_crs);
        }

        /* Standard PCIe _OSC, _DSM, and RES0 ECAM reservation */
        {
            static const uint8_t pci_osc_dsm_res0[] = {
                /* Method (_OSC, 4) */
                0x14, 0x4c, 0x07, '_', 'O', 'S', 'C', 0x04,
                0x8a, 0x6b, 0x00, 'C', 'D', 'W', '1',
                0xa0, 0x4e, 0x05, 0x93, 0x68,
                0x11, 0x13, 0x0a, 0x10,
                0x5b, 0x4d, 0xdb, 0x33, 0xf7, 0x1f, 0x1c, 0x40,
                0x96, 0x57, 0x74, 0x41, 0xc0, 0x3d, 0xd7, 0x66,
                0x8a, 0x6b, 0x0a, 0x04, 'C', 'D', 'W', '2',
                0x8a, 0x6b, 0x0a, 0x08, 'C', 'D', 'W', '3',
                0x70, 'C', 'D', 'W', '3', 0x60,
                0x7b, 0x60, 0x0a, 0x1f, 0x60,
                0xa0, 0x10, 0x92, 0x93, 0x69, 0x01,
                0x7d, 'C', 'D', 'W', '1', 0x0a, 0x08, 'C', 'D', 'W', '1',
                0xa0, 0x13, 0x92, 0x93, 'C', 'D', 'W', '3', 0x60,
                0x7d, 'C', 'D', 'W', '1', 0x0a, 0x10, 'C', 'D', 'W', '1',
                0x70, 0x60, 'C', 'D', 'W', '3',
                0xa1, 0x0c,
                0x7d, 'C', 'D', 'W', '1', 0x0a, 0x04, 'C', 'D', 'W', '1',
                0xa4, 0x6b,
                /* Method (_DSM, 4) */
                0x14, 0x2d, '_', 'D', 'S', 'M', 0x04,
                0xa0, 0x21, 0x93, 0x68,
                0x11, 0x13, 0x0a, 0x10,
                0xd0, 0x37, 0xc9, 0xe5, 0x53, 0x35, 0x7a, 0x4d,
                0x91, 0x17, 0xea, 0x4d, 0x19, 0xc3, 0x43, 0x4d,
                0xa0, 0x09, 0x93, 0x6a, 0x00,
                0xa4, 0x11, 0x03, 0x01, 0x00,
                0xa4, 0x11, 0x03, 0x01, 0x00,
                /* Device (RES0): PNP0C02 ECAM 0x4010000000..0x401fffffff */
                0x5b, 0x82, 0x4d, 0x04, 'R', 'E', 'S', '0',
                0x08, '_', 'H', 'I', 'D', 0x0d, 'P', 'N', 'P', '0', 'C', '0', '2', 0x00,
                0x08, '_', 'C', 'R', 'S', 0x11, 0x33, 0x0a, 0x30,
                0x8a, 0x2b, 0x00, 0x00, 0x0c, 0x01,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x10, 0x40, 0x00, 0x00, 0x00,
                0xff, 0xff, 0xff, 0x1f, 0x40, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
                0x79, 0x00,
            };
            b_put(&dev, pci_osc_dsm_res0, sizeof pci_osc_dsm_res0);
        }

        aml_device(&sb, "PCI0", &dev);
        free(dev.buf);
    }

    /* Power button (PWRB, PNP0C0C) + Generic Event Device (GED_, ACPI0013 at 0x09080000, GSI 41 / SPI 9) */
    {
        Buf pwrb = {0};
        aml_name_str(&pwrb, "_HID", "PNP0C0C");
        aml_name_int(&pwrb, "_UID", 0);
        aml_device(&sb, "PWRB", &pwrb);
        free(pwrb.buf);

        Buf ged = {0}, crs = {0};
        aml_name_str(&ged, "_HID", "ACPI0013");
        aml_name_str(&ged, "_UID", "GED");
        aml_crs_mem32_irq(&crs, 0, 0, 41U, 1, 1, 0);
        aml_name_buf(&ged, "_CRS", crs.buf, crs.len);
        static const uint8_t ged_evt[] = {
            /* OperationRegion (EREG, SystemMemory, 0x09080000, 0x04) */
            0x5b, 0x80, 'E', 'R', 'E', 'G', 0x00, 0x0c, 0x00, 0x00, 0x08, 0x09, 0x0a, 0x04,
            /* Field (EREG, DWordAcc, NoLock, WriteAsZeros) { ESEL, 32 } */
            0x5b, 0x81, 0x0b, 'E', 'R', 'E', 'G', 0x43, 'E', 'S', 'E', 'L', 0x20,
            /* Method (_EVT, 1, Serialized) { Local0 = ESEL; If ((Local0 & 2) == 2) { Notify (PWRB, 0x80) } } */
            0x14, 0x1d, '_', 'E', 'V', 'T', 0x09,
            0x70, 'E', 'S', 'E', 'L', 0x60,
            0xa0, 0x10, 0x93, 0x7b, 0x60, 0x0a, 0x02, 0x00, 0x0a, 0x02,
            0x86, 'P', 'W', 'R', 'B', 0x0a, 0x80,
        };
        b_put(&ged, ged_evt, sizeof ged_evt);
        aml_device(&sb, "GED_", &ged);
        free(crs.buf);
        free(ged.buf);
    }

    /* Laptop/slate mode indicator (_CID PNP0C60). No GPIO resource: Windows' inbox "GPIO Laptop or Slate
       Indicator" driver loads on the _CID and takes its state by injection from the guest agent. */
    {
        Buf conv = {0};
        aml_name_str(&conv, "_HID", "WIOS0001");
        aml_name_str(&conv, "_CID", "PNP0C60");
        aml_device(&sb, "CONV", &conv);
        free(conv.buf);
    }

    /* Wrap in Scope (\_SB_) */
    b_u8(tabs, 0x10); /* ScopeOp */
    aml_pkg_len(tabs, 5 + sb.len);
    b_put(tabs, "\\_SB_", 5);
    b_put(tabs, sb.buf, sb.len);
    free(sb.buf);

    finalize_table(tabs, start);
}

static size_t build_facp(Buf *tabs, uint32_t dsdt_off, uint32_t *out_xdsdt_ptr_off) {
    size_t start = tabs->len;
    acpi_hdr(tabs, "FACP", 276, 6, "WINVIRT ");
    b_zero(tabs, 276 - 36);

    /* Flags at +112: HW_REDUCED_ACPI (bit 20) | TMR_VAL_EXT (bit 8) */
    uint32_t flags = (1u << 20) | (1u << 8);
    memcpy(tabs->buf + start + 112, &flags, 4);

    /* ARM_BOOT_ARCH at +129 (2 bytes): PSCI_COMPLIANT (1) | PSCI_USE_HVC (2) = 3 */
    uint16_t arm_boot = 0x0003;
    memcpy(tabs->buf + start + 129, &arm_boot, 2);

    /* FADT Minor Version at +131: 3 (ACPI 6.3+) */
    tabs->buf[start + 131] = 3;

    /* X_DSDT at +140 (8 bytes): relative offset into etc/acpi/tables, patched by ADD_POINTER */
    uint64_t xdsdt = dsdt_off;
    memcpy(tabs->buf + start + 140, &xdsdt, 8);
    *out_xdsdt_ptr_off = (uint32_t)(start + 140);

    /* Hypervisor Vendor Identity at +268 (8 bytes) */
    memcpy(tabs->buf + start + 268, "WINIOS\0\0", 8);

    finalize_table(tabs, start);
    return start;
}

static size_t build_madt(Buf *tabs, int ncpus, int spi_msi_base, int spi_msi_count) {
    size_t start = tabs->len;
    acpi_hdr(tabs, "APIC", 0, 4, "WINVIRT ");
    b_u32(tabs, 0); /* Local Interrupt Controller Address */
    b_u32(tabs, 0); /* Flags */

    /* GICD (0x0C, 24 bytes): 0x08000000, GICv3 */
    b_u8(tabs, 0x0c);
    b_u8(tabs, 24);
    b_u16(tabs, 0);
    b_u32(tabs, 0); /* GIC ID */
    b_u64(tabs, 0x08000000ULL);
    b_u32(tabs, 0);
    b_u8(tabs, 3); /* GICv3 */
    b_u8(tabs, 0);
    b_u16(tabs, 0);

    /* GICC (0x0B, 80 bytes) per CPU */
    for (int i = 0; i < ncpus; i++) {
        size_t c0 = tabs->len;
        b_zero(tabs, 80);
        tabs->buf[c0 + 0] = 0x0b;
        tabs->buf[c0 + 1] = 80;
        uint32_t uid = (uint32_t)i, flags = 1U, pmu_gsiv = 23U;
        uint64_t mpidr = (uint64_t)i;
        memcpy(tabs->buf + c0 + 4, &uid, 4);      /* CPU Interface Number */
        memcpy(tabs->buf + c0 + 8, &uid, 4);      /* ACPI Processor UID */
        memcpy(tabs->buf + c0 + 12, &flags, 4);   /* Flags: Enabled */
        memcpy(tabs->buf + c0 + 20, &pmu_gsiv, 4);
        memcpy(tabs->buf + c0 + 68, &mpidr, 8);   /* MPIDR */
    }

    /* GICR (0x0E, 16 bytes): 0x080a0000, length 0x00f60000 */
    b_u8(tabs, 0x0e);
    b_u8(tabs, 16);
    b_u16(tabs, 0);
    b_u64(tabs, 0x080a0000ULL);
    b_u32(tabs, 0x00f60000U);

    /* GICv2m MSI Frame (0x0D) or GICv3 ITS (0x0F, 20 bytes) at 0x08080000 for PCIe MSI-X */
    if (spi_msi_count > 0) {
        b_u8(tabs, 0x0d);
        b_u8(tabs, 24);
        b_u16(tabs, 0);
        b_u32(tabs, 0); /* GIC MSI Frame ID */
        b_u64(tabs, 0x08080000ULL);
        b_u32(tabs, 1U); /* Flags: SPI_COUNT_BASE_SELECT */
        b_u16(tabs, (uint16_t)spi_msi_count);
        b_u16(tabs, (uint16_t)spi_msi_base);
    } else {
        b_u8(tabs, 0x0f); /* GIC ITS Structure */
        b_u8(tabs, 20);
        b_u16(tabs, 0);
        b_u32(tabs, 0);   /* GIC ITS ID = 0 */
        b_u64(tabs, 0x08080000ULL);
        b_u32(tabs, 0);   /* Reserved */
    }

    finalize_table(tabs, start);
    return start;
}

static size_t build_pptt(Buf *tabs, int ncpus) {
    size_t start = tabs->len;
    acpi_hdr(tabs, "PPTT", 0, 2, "WINVIRT ");

    uint32_t pkg_off = 36U;
    uint32_t clu_off = 56U;

    /* Socket / Physical Package node (20 bytes) */
    b_u8(tabs, 0);
    b_u8(tabs, 20);
    b_u16(tabs, 0);
    b_u32(tabs, 0x11U); /* Physical package | Identical implementation */
    b_u32(tabs, 0);     /* Parent */
    b_u32(tabs, 0);     /* ACPI Processor ID */
    b_u32(tabs, 0);     /* Number of private resources */

    /* Cluster node (20 bytes) */
    b_u8(tabs, 0);
    b_u8(tabs, 20);
    b_u16(tabs, 0);
    b_u32(tabs, 0x10U); /* Identical implementation */
    b_u32(tabs, pkg_off);
    b_u32(tabs, 0);
    b_u32(tabs, 0);

    /* Core nodes (20 bytes each) */
    for (int i = 0; i < ncpus; i++) {
        b_u8(tabs, 0);
        b_u8(tabs, 20);
        b_u16(tabs, 0);
        b_u32(tabs, 0x0aU); /* ACPI Processor ID valid | Node is a Leaf */
        b_u32(tabs, clu_off);
        b_u32(tabs, (uint32_t)i);
        b_u32(tabs, 0);
    }

    finalize_table(tabs, start);
    return start;
}

static size_t build_gtdt(Buf *tabs) {
    size_t start = tabs->len;
    acpi_hdr(tabs, "GTDT", 0, 3, "WINVIRT ");
    b_u64(tabs, ~0ULL); /* CntControlBase Physical Address (not memory-mapped) */
    b_u32(tabs, 0);     /* Reserved */
    b_u32(tabs, 29U);   /* Secure EL1 Timer GSIV (PPI 13) */
    b_u32(tabs, 0U);    /* Flags: Level, Active-High */
    b_u32(tabs, 30U);   /* Non-Secure EL1 Timer GSIV (PPI 14) */
    b_u32(tabs, 4U);    /* Flags: Level, Active-High, Always-On */
    b_u32(tabs, 27U);   /* Virtual EL1 Timer GSIV (PPI 11) */
    b_u32(tabs, 4U);    /* Flags: Level, Active-High, Always-On */
    b_u32(tabs, 26U);   /* EL2 Timer GSIV (PPI 10) */
    b_u32(tabs, 0U);
    b_u64(tabs, ~0ULL); /* CntReadBase Physical Address */
    b_u32(tabs, 0);     /* Platform Timer Count */
    b_u32(tabs, 0);     /* Platform Timer Offset */
    b_u32(tabs, 0);     /* Virtual EL2 Timer GSIV */
    b_u32(tabs, 0);     /* Virtual EL2 Timer Flags */
    finalize_table(tabs, start);
    return start;
}

static size_t build_mcfg(Buf *tabs) {
    size_t start = tabs->len;
    acpi_hdr(tabs, "MCFG", 0, 1, "WINVIRT ");
    b_u64(tabs, 0); /* Reserved */
    b_u64(tabs, 0x4010000000ULL); /* ECAM Base Address */
    b_u16(tabs, 0);    /* PCI Segment Group */
    b_u8(tabs, 0x00);  /* Start Bus */
    b_u8(tabs, 0xff);  /* End Bus */
    b_u32(tabs, 0);    /* Reserved */
    finalize_table(tabs, start);
    return start;
}

static size_t build_spcr(Buf *tabs) {
    size_t start = tabs->len;
    acpi_hdr(tabs, "SPCR", 0, 2, "WINVIRT ");
    b_u8(tabs, 3);  /* Interface Type: ARM PL011 UART */
    b_u8(tabs, 0);
    b_u16(tabs, 0);
    /* BaseAddress GAS: SystemMemory (0), BitWidth=32 (0x20), BitOffset=0, AccessSize=3 (DWord), Addr=0x09000000 */
    b_u8(tabs, 0);
    b_u8(tabs, 32);
    b_u8(tabs, 0);
    b_u8(tabs, 3);
    b_u64(tabs, 0x09000000ULL);
    b_u8(tabs, 8);    /* Interrupt Type: ARM GIC */
    b_u8(tabs, 0);    /* PC IRQ */
    b_u32(tabs, 33U); /* Global System Interrupt (SPI 1) */
    b_u8(tabs, 3);    /* Baud Rate: 9600/115200 */
    b_u8(tabs, 0);    /* Parity */
    b_u8(tabs, 1);    /* Stop Bits */
    b_u8(tabs, 2);    /* Flow Control */
    b_u8(tabs, 0);    /* Terminal Type */
    b_u8(tabs, 0);
    b_u16(tabs, 0xffffU); /* PCI Device ID */
    b_u16(tabs, 0xffffU); /* PCI Vendor ID */
    b_u8(tabs, 0);
    b_u8(tabs, 0);
    b_u8(tabs, 0);
    b_u32(tabs, 0);
    b_u8(tabs, 0);
    b_u32(tabs, 0);
    finalize_table(tabs, start);
    return start;
}

static size_t build_dbg2_fixed(Buf *tabs) {
    size_t start = tabs->len;
    acpi_hdr(tabs, "DBG2", 0, 0, "WINVIRT ");
    b_u32(tabs, 44U); /* OffsetDbgDeviceInfo */
    b_u32(tabs, 1U);  /* NumberDbgDeviceInfo */
    /* Debug Device Information Structure: 22B header + 12B GAS + 4B AddrSize + 5B "COM0\0" = 43B */
    b_u8(tabs, 0);         /* +0: Revision */
    b_u16(tabs, 43U);      /* +1: Length */
    b_u8(tabs, 1);         /* +3: NumberofGenericAddressRegisters */
    b_u16(tabs, 5U);       /* +4: NameSpaceStringLength ("COM0\0") */
    b_u16(tabs, 38U);      /* +6: NameSpaceStringOffset (22 + 12 + 4) */
    b_u16(tabs, 0);        /* +8: OemDataLength */
    b_u16(tabs, 0);        /* +10: OemDataOffset */
    b_u16(tabs, 0x8000U);  /* +12: Port Type: Serial */
    b_u16(tabs, 0x0003U);  /* +14: Port Subtype: ARM PL011 */
    b_u16(tabs, 0);        /* +16: Reserved */
    b_u16(tabs, 22U);      /* +18: BaseAddressRegisterOffset */
    b_u16(tabs, 34U);      /* +20: AddressSizeOffset */
    /* GAS at +22 (12 bytes) */
    b_u8(tabs, 0);
    b_u8(tabs, 32);
    b_u8(tabs, 0);
    b_u8(tabs, 3);
    b_u64(tabs, 0x09000000ULL);
    /* AddressSize at +34 (4 bytes) */
    b_u32(tabs, 0x1000U);
    /* NameSpaceString at +38 (5 bytes) */
    b_put(tabs, "COM0", 5);

    finalize_table(tabs, start);
    return start;
}

static size_t build_iort(Buf *tabs) {
    size_t start = tabs->len;
    acpi_hdr(tabs, "IORT", 0, 5, "WINVIRT ");
    b_u32(tabs, 2U);  /* Number of IORT nodes: Node 0 = ITS Group, Node 1 = PCIe Root Complex */
    b_u32(tabs, 48U); /* Offset to array of IORT nodes (0x30) */
    b_u32(tabs, 0U);  /* Reserved */

    /* Node 0 at +0x30: ITS Group Node (Type 0, 24 bytes, rev 1) */
    b_u8(tabs, 0);     /* Type: ITS Group */
    b_u16(tabs, 24U);  /* Length */
    b_u8(tabs, 1);     /* Revision */
    b_u32(tabs, 0);    /* Identifier */
    b_u32(tabs, 0);    /* Number of ID mappings */
    b_u32(tabs, 0);    /* Reference to ID Array */
    b_u32(tabs, 1U);   /* Number of ITSs = 1 */
    b_u32(tabs, 0U);   /* GIC ITS ID[0] = 0 */

    /* Node 1 at +0x48: Root Complex Node (Type 2, 56 bytes = 36B header + 20B ID mapping, rev 3) */
    b_u8(tabs, 2);     /* Type: Root complex */
    b_u16(tabs, 56U);  /* Length */
    b_u8(tabs, 3);     /* Revision */
    b_u32(tabs, 1U);   /* Identifier */
    b_u32(tabs, 1U);   /* Number of ID mappings = 1 */
    b_u32(tabs, 36U);  /* Reference to ID Array (+0x24) */
    b_u32(tabs, 1U);   /* Cache Coherent Attributes: Fully coherent */
    b_u8(tabs, 0);     /* Allocation Hints */
    b_u16(tabs, 0);    /* Reserved */
    b_u8(tabs, 3);     /* Memory Access Flags: CPM | DACS */
    b_u32(tabs, 0);    /* ATS Attribute */
    b_u32(tabs, 0);    /* PCI Segment number */
    b_u8(tabs, 64);    /* Memory address size limit: 64-bit */
    b_u16(tabs, 0);    /* PASID capabilities */
    b_u8(tabs, 0);     /* Reserved */

    /* ID Mapping [0]: PCI RequesterID 0x0000..0xffff -> ITS Group Node at +0x30 (48) */
    b_u32(tabs, 0U);      /* Input base */
    b_u32(tabs, 0xffffU); /* Number of IDs (minus 1 -> 65536 BDFs) */
    b_u32(tabs, 0U);      /* Output base */
    b_u32(tabs, 48U);     /* Output reference: offset 0x30 (ITS Group Node 0) */
    b_u32(tabs, 0U);      /* Flags */

    finalize_table(tabs, start);
    return start;
}

static void build_smbios(HvFwCfg *fw, int ncpus, uint64_t ram_size) {
    Buf t = {0};

    /* Type 1: System Information (len 27) */
    {
        b_u8(&t, 1);
        b_u8(&t, 27);
        b_u16(&t, 0x0100U);
        b_u8(&t, 1); /* Manufacturer */
        b_u8(&t, 2); /* Product Name */
        b_u8(&t, 3); /* Version */
        b_u8(&t, 4); /* Serial Number */
        static const uint8_t uuid[16] = {
            0x57, 0x49, 0x4e, 0x31, 0x31, 0x49, 0x4f, 0x53,
            0x44, 0x55, 0x4f, 0x30, 0x30, 0x30, 0x31, 0x00,
        };
        b_put(&t, uuid, 16);
        b_u8(&t, 6); /* Wake-up Type: Power Switch */
        b_u8(&t, 0); /* SKU */
        b_u8(&t, 5); /* Family */
        b_put(&t, "Apple / WindowsiOS\0", 19);
        b_put(&t, "iPhone Duo ARM64 PC\0", 20);
        b_put(&t, "1.0\0", 4);
        b_put(&t, "DUO-WIN11-ARM64\0", 16);
        b_put(&t, "WindowsiOS\0", 11);
        b_u8(&t, 0);
    }

    /* Type 3: System Enclosure (len 21) */
    {
        b_u8(&t, 3);
        b_u8(&t, 21);
        b_u16(&t, 0x0300U);
        b_u8(&t, 1);    /* Manufacturer */
        b_u8(&t, 0x1e); /* Tablet / Foldable */
        b_u8(&t, 2);    /* Version */
        b_u8(&t, 0);
        b_u8(&t, 0);
        b_u8(&t, 3);
        b_u8(&t, 3);
        b_u8(&t, 3);
        b_u8(&t, 3);
        b_u32(&t, 0);
        b_u8(&t, 0);
        b_u8(&t, 0);
        b_u8(&t, 0);
        b_u8(&t, 0);
        b_put(&t, "Apple\0", 6);
        b_put(&t, "iPhone19,4\0", 11);
        b_u8(&t, 0);
    }

    /* Type 4: Processor Information (len 42) */
    {
        b_u8(&t, 4);
        b_u8(&t, 42);
        b_u16(&t, 0x0400U);
        b_u8(&t, 1);
        b_u8(&t, 3);
        b_u8(&t, 0xfe);
        b_u8(&t, 2);
        b_u64(&t, 0x414fd0b1ULL);
        b_u8(&t, 3);
        b_u8(&t, 0x8b);
        b_u16(&t, 100U);
        b_u16(&t, 4000U);
        b_u16(&t, 3600U);
        b_u8(&t, 0x41);
        b_u8(&t, 1);
        b_u16(&t, 0xffffU);
        b_u16(&t, 0xffffU);
        b_u16(&t, 0xffffU);
        b_u8(&t, 0);
        b_u8(&t, 0);
        b_u8(&t, 0);
        b_u8(&t, (uint8_t)ncpus);
        b_u8(&t, (uint8_t)ncpus);
        b_u8(&t, (uint8_t)ncpus);
        b_u16(&t, 0x0004U);
        b_u16(&t, 0x0101U);
        b_put(&t, "SOC0\0", 5);
        b_put(&t, "Apple / ARM\0", 12);
        b_put(&t, "Apple A20 Pro (WindowsiOS JIT)\0", 31);
        b_u8(&t, 0);
    }

    /* Type 16: Physical Memory Array (len 23) */
    {
        b_u8(&t, 16);
        b_u8(&t, 23);
        b_u16(&t, 0x1000U);
        b_u8(&t, 3);
        b_u8(&t, 3);
        b_u8(&t, 3);
        b_u32(&t, 0x80000000U);
        b_u16(&t, 0xfffeU);
        b_u16(&t, 1U);
        b_u64(&t, ram_size);
        b_u16(&t, 0);
    }

    /* Type 17: Memory Device (len 40) */
    {
        uint16_t mb = (uint16_t)(ram_size >> 20);
        b_u8(&t, 17);
        b_u8(&t, 40);
        b_u16(&t, 0x1100U);
        b_u16(&t, 0x1000U);
        b_u16(&t, 0xfffeU);
        b_u16(&t, 64U);
        b_u16(&t, 64U);
        b_u16(&t, mb);
        b_u8(&t, 0x09);
        b_u8(&t, 0);
        b_u8(&t, 1);
        b_u8(&t, 0);
        b_u8(&t, 0x1e);
        b_u16(&t, 0x0080U);
        b_u16(&t, 6400U);
        b_u8(&t, 2);
        b_u8(&t, 0);
        b_u8(&t, 0);
        b_u8(&t, 0);
        b_u8(&t, 0);
        b_u32(&t, 0);
        b_u16(&t, 6400U);
        b_u16(&t, 1100U);
        b_u16(&t, 1100U);
        b_u16(&t, 1100U);
        b_put(&t, "LPDDR5X\0", 8);
        b_put(&t, "Apple\0", 6);
        b_u8(&t, 0);
    }

    /* Type 19: Memory Array Mapped Address (len 31) */
    {
        b_u8(&t, 19);
        b_u8(&t, 31);
        b_u16(&t, 0x1300U);
        b_u32(&t, 0xffffffffU);
        b_u32(&t, 0xffffffffU);
        b_u16(&t, 0x1000U);
        b_u8(&t, 1);
        b_u64(&t, 0x40000000ULL);
        b_u64(&t, 0x40000000ULL + ram_size - 1ULL);
        b_u16(&t, 0);
    }

    /* Type 32: System Boot Information (len 11) */
    {
        b_u8(&t, 32);
        b_u8(&t, 11);
        b_u16(&t, 0x2000U);
        b_zero(&t, 7);
        b_u16(&t, 0);
    }

    /* Type 127: End-of-Table (len 4) */
    {
        b_u8(&t, 127);
        b_u8(&t, 4);
        b_u16(&t, 0x7f00U);
        b_u16(&t, 0);
    }

    uint8_t anchor[24] = {0};
    memcpy(anchor + 0, "_SM3_", 5);
    anchor[6] = 24;
    anchor[7] = 3;
    anchor[8] = 0;
    anchor[10] = 1;
    uint32_t max_sz = (uint32_t)t.len;
    memcpy(anchor + 12, &max_sz, 4);
    anchor[5] = acpi_cksum(anchor, 24);

    hv_fw_cfg_add_file(fw, "etc/smbios/smbios-anchor", anchor, sizeof anchor);
    hv_fw_cfg_add_file(fw, "etc/smbios/smbios-tables", t.buf, t.len);
    free(t.buf);
}

void hv_acpi_build_and_register(HvFwCfg *fw, int ncpus, uint64_t ram_size, int spi_msi_base, int spi_msi_count) {
    Buf tabs = {0};
    Loader ldr = {0};

    loader_alloc(&ldr, "etc/acpi/rsdp", 16, 1);   /* Zone 1 = HIGH (ARM64 has no <1MB FSEG RAM!) */
    loader_alloc(&ldr, "etc/acpi/tables", 64, 1); /* Zone 1 = HIGH */

    /* 1. DSDT at offset 0 */
    build_dsdt(&tabs, ncpus);
    uint32_t dsdt_len = (uint32_t)tabs.len;
    loader_add_cksum(&ldr, "etc/acpi/tables", 9, 0, dsdt_len);

    /* 2. Subtables referenced by XSDT */
    size_t tbl_offs[8];
    uint32_t tbl_lens[8];
    uint32_t facp_xdsdt_ptr_off = 0;

    tbl_offs[0] = build_facp(&tabs, 0, &facp_xdsdt_ptr_off);
    tbl_lens[0] = (uint32_t)(tabs.len - tbl_offs[0]);
    loader_add_ptr(&ldr, "etc/acpi/tables", "etc/acpi/tables", facp_xdsdt_ptr_off, 8);
    loader_add_cksum(&ldr, "etc/acpi/tables", (uint32_t)tbl_offs[0] + 9, (uint32_t)tbl_offs[0], tbl_lens[0]);

    tbl_offs[1] = build_madt(&tabs, ncpus, spi_msi_base, spi_msi_count);
    tbl_lens[1] = (uint32_t)(tabs.len - tbl_offs[1]);

    tbl_offs[2] = build_pptt(&tabs, ncpus);
    tbl_lens[2] = (uint32_t)(tabs.len - tbl_offs[2]);

    tbl_offs[3] = build_gtdt(&tabs);
    tbl_lens[3] = (uint32_t)(tabs.len - tbl_offs[3]);

    tbl_offs[4] = build_mcfg(&tabs);
    tbl_lens[4] = (uint32_t)(tabs.len - tbl_offs[4]);

    tbl_offs[5] = build_spcr(&tabs);
    tbl_lens[5] = (uint32_t)(tabs.len - tbl_offs[5]);

    tbl_offs[6] = build_dbg2_fixed(&tabs);
    tbl_lens[6] = (uint32_t)(tabs.len - tbl_offs[6]);

    tbl_offs[7] = build_iort(&tabs);
    tbl_lens[7] = (uint32_t)(tabs.len - tbl_offs[7]);

    for (int i = 1; i < 8; i++) {
        loader_add_cksum(&ldr, "etc/acpi/tables", (uint32_t)tbl_offs[i] + 9, (uint32_t)tbl_offs[i], tbl_lens[i]);
    }

    /* 3. XSDT */
    size_t xsdt_off = tabs.len;
    acpi_hdr(&tabs, "XSDT", 0, 1, "WINVIRT ");
    for (int i = 0; i < 8; i++) {
        uint32_t ptr_off = (uint32_t)tabs.len;
        b_u64(&tabs, (uint64_t)tbl_offs[i]);
        loader_add_ptr(&ldr, "etc/acpi/tables", "etc/acpi/tables", ptr_off, 8);
    }
    finalize_table(&tabs, xsdt_off);
    uint32_t xsdt_len = (uint32_t)(tabs.len - xsdt_off);
    loader_add_cksum(&ldr, "etc/acpi/tables", (uint32_t)xsdt_off + 9, (uint32_t)xsdt_off, xsdt_len);

    /* Pad etc/acpi/tables to 65536 bytes */
    if (tabs.len < 65536) b_zero(&tabs, 65536 - tabs.len);

    /* 4. RSDP (36 bytes, ACPI 2.0+) */
    uint8_t rsdp[36] = {0};
    memcpy(rsdp + 0, "RSD PTR ", 8);
    memcpy(rsdp + 9, "WINIOS", 6);
    rsdp[15] = 2; /* Revision 2 (ACPI 2.0+) */
    uint32_t rsdp_len = 36U;
    memcpy(rsdp + 20, &rsdp_len, 4);
    uint64_t xsdt_rel = (uint64_t)xsdt_off;
    memcpy(rsdp + 24, &xsdt_rel, 8);
    /* rsdp[8] and rsdp[32] stay 0; BIOS linker ADD_CHECKSUM computes them */

    loader_add_cksum(&ldr, "etc/acpi/rsdp", 8, 0, 20);
    loader_add_ptr(&ldr, "etc/acpi/rsdp", "etc/acpi/tables", 24, 8);
    loader_add_cksum(&ldr, "etc/acpi/rsdp", 32, 0, 36);

    uint32_t boot_fail_wait = 0;
    hv_fw_cfg_add_file(fw, "etc/acpi/rsdp", rsdp, sizeof rsdp);
    hv_fw_cfg_add_file(fw, "etc/acpi/tables", tabs.buf, tabs.len);
    hv_fw_cfg_add_file(fw, "etc/boot-fail-wait", &boot_fail_wait, 4);
    hv_fw_cfg_add_file(fw, "etc/table-loader", ldr.buf.buf, ldr.buf.len);

    build_smbios(fw, ncpus, ram_size);

    free(tabs.buf);
    free(ldr.buf.buf);
}
