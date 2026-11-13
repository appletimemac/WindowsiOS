#include "pcie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define CMD_MEMORY 0x2
#define CMD_INTX_DISABLE 0x400
#define STATUS_INTX 0x8
#define STATUS_CAPS 0x10

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t get16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void link_cap(HvPciDev *d, uint8_t at) {
    put16(d->config + PCI_STATUS, get16(d->config + PCI_STATUS) | STATUS_CAPS);
    d->config[at + 1] = d->config[0x34];
    d->config[0x34] = at;
}

void hv_pci_init(HvPciDev *d, uint16_t vendor, uint16_t device, uint32_t class_rev, uint16_t sub_vendor,
                 uint16_t sub_id) {
    memset(d->config, 0, sizeof d->config);
    memset(d->wmask, 0, sizeof d->wmask);
    put16(d->config + 0x00, vendor);
    put16(d->config + 0x02, device);
    put32(d->config + 0x08, class_rev);
    put16(d->config + 0x2c, sub_vendor);
    put16(d->config + 0x2e, sub_id);
    d->config[0x3d] = 1;                          /* INTA */
    put16(d->wmask + PCI_COMMAND, 0x0407);        /* I/O, memory, bus master, INTx disable */
    d->wmask[0x0c] = 0xff;                        /* cache line size */
    d->wmask[0x0d] = 0xff;                        /* latency timer */
    d->wmask[0x3c] = 0xff;                        /* interrupt line */
}

void hv_pci_add_bar_flags(HvPciDev *d, int bar, uint64_t size, int is64, int prefetch) {
    d->bar_size[bar] = size;
    d->bar64[bar] = (uint8_t)is64;
    uint32_t flags = (is64 ? 0x4u : 0x0u) | (prefetch ? 0x8u : 0x0u);
    put32(d->config + 0x10 + bar * 4, flags);
    put32(d->wmask + 0x10 + bar * 4, (uint32_t)~(size - 1) & ~0xfu);
    if (is64) put32(d->wmask + 0x14 + bar * 4, (uint32_t)(~(size - 1) >> 32));
}

void hv_pci_add_bar(HvPciDev *d, int bar, uint64_t size, int is64) {
    hv_pci_add_bar_flags(d, bar, size, is64, 0);
}

void hv_pci_add_pm_cap(HvPciDev *d, uint8_t at) {
    uint8_t *c = d->config + at;
    memset(c, 0, 8);
    c[0] = 0x01;                                  /* Cap ID: PCI Power Management */
    put16(c + 0x02, 0x0003);                      /* PMC: v1.2 */
    put32(c + 0x04, 0x00000008u);                 /* PMCSR: No_Soft_Reset = 1, D0 */
    put16(d->wmask + at + 0x04, 0x0003);          /* PowerState D0..D3hot writable */
    link_cap(d, at);
}

void hv_pci_add_pcie_cap(HvPciDev *d, uint8_t at) {
    uint8_t *c = d->config + at;
    memset(c, 0, 0x3c);
    c[0] = 0x10;                                  /* Cap ID: PCI Express */
    put16(c + 0x02, 0x0092);                      /* PCIe v2 Root Complex Integrated Endpoint (Type 9) */
    put32(c + 0x04, 0x00008020u);                 /* DevCap: Role-Based Error Reporting, Extended Tag */
    put16(c + 0x08, 0x0000);                      /* DevCtl */
    put16(d->wmask + at + 0x08, 0xffff);
    put32(c + 0x0c, 0x00000411u);                 /* LinkCap */
    put16(c + 0x12, 0x0011);                      /* LinkSta */
    put16(d->wmask + at + 0x10, 0xffff);          /* LinkCtl */
    put32(c + 0x24, 0x00300000u);                 /* DevCap2 */
    put16(d->wmask + at + 0x28, 0xffff);          /* DevCtl2 */
    link_cap(d, at);
    /* Also populate minimal PCIe Extended Capability header at 0x100 (Null cap) so config >= 0x100 reads valid */
    put32(d->config + 0x100, 0x00000000u);
}

void hv_pci_add_msix(HvPciDev *d, uint8_t at, int bar, uint32_t table_off, uint32_t pba_off, uint16_t nvec) {
    if (nvec < 1) nvec = 1;
    if (nvec > HV_PCI_MAX_MSIX) nvec = HV_PCI_MAX_MSIX;
    d->msix_cap = at;
    d->msix_bar = bar;
    d->msix_nvec = nvec;
    d->msix_table_off = table_off;
    d->msix_pba_off = pba_off;
    d->msix_pba = 0;
    for (uint16_t i = 0; i < nvec; i++) {
        d->msix_table[i] = (HvPciMsixEntry){.ctrl = 1}; /* masked at reset per PCI spec */
    }
    uint8_t *c = d->config + at;
    c[0] = 0x11;                                  /* Cap ID: MSI-X */
    put16(c + 2, (uint16_t)(nvec - 1));           /* Message Control: table size N-1 */
    put16(d->wmask + at + 2, 0xc000);             /* Enable (bit 15), Function Mask (bit 14) */
    put32(c + 4, (table_off & ~7u) | (uint32_t)(bar & 7));
    put32(c + 8, (pba_off & ~7u) | (uint32_t)(bar & 7));
    link_cap(d, at);
}

int hv_pci_msix_enabled(const HvPciDev *d) {
    if (!d->msix_cap) return 0;
    return (get16(d->config + d->msix_cap + 2) & 0x8000u) != 0;
}

static void msix_fire(HvPciDev *d, uint16_t vec) {
    HvPcie *p = d->bus;
    if (!p) return;
    uint64_t addr = (uint64_t)d->msix_table[vec].addr_hi << 32 | (d->msix_table[vec].addr_lo & ~3u);
    uint32_t data = d->msix_table[vec].data;
    if (!addr) return;
    if (p->its && addr == p->its_translater_pa) {
        uint32_t req_id = (uint32_t)(d->slot << 3); /* Bus 0, Dev=slot, Fn=0 */
        hv_gicv3_its_msi(p->its, req_id, data);
        return;
    }
    if (p->msi_doorbell_pa && addr == p->msi_doorbell_pa) {
        hv_gicv3_pend(p->gic, (int)(data & 0x3ff));
        return;
    }
    /* Generic fallback: write through machine MMIO */
    for (int i = 0; i < p->m->nmmio; i++) {
        HvMmio *mm = &p->m->mmio[i];
        if (addr - mm->pa < mm->size && mm->write) {
            mm->write(mm->opaque, addr - mm->pa, data, 4);
            return;
        }
    }
}

static void msix_check_pending(HvPciDev *d) {
    if (!hv_pci_msix_enabled(d)) return;
    uint16_t mctl = get16(d->config + d->msix_cap + 2);
    if (mctl & 0x4000u) return; /* Function Mask */
    for (uint16_t v = 0; v < d->msix_nvec; v++) {
        if ((d->msix_pba >> v & 1) && !(d->msix_table[v].ctrl & 1)) {
            d->msix_pba &= ~(1ull << v);
            msix_fire(d, v);
        }
    }
}

void hv_pci_msix_notify(HvPciDev *d, uint16_t vec) {
    if (!hv_pci_msix_enabled(d) || vec >= d->msix_nvec) return;
    uint16_t mctl = get16(d->config + d->msix_cap + 2);
    if ((mctl & 0x4000u) || (d->msix_table[vec].ctrl & 1)) {
        d->msix_pba |= 1ull << vec;
        return;
    }
    msix_fire(d, vec);
}

uint64_t hv_pci_bar_addr(HvPciDev *d, int bar) {
    uint64_t lo = get32(d->config + 0x10 + bar * 4) & ~0xfu;
    if (d->bar64[bar]) lo |= (uint64_t)get32(d->config + 0x14 + bar * 4) << 32;
    return lo;
}

int hv_pci_mem_enabled(const HvPciDev *d) {
    return (get16(d->config + PCI_COMMAND) & CMD_MEMORY) != 0;
}

static int intx_line_of(HvPcie *p, HvPciDev *d) {
    if (!p->gpex_swizzle) return d->slot;
    int pin = d->config[0x3d] ? d->config[0x3d] : 1;
    return (d->slot + pin - 1) & 3;
}

static void update_line(HvPcie *p, int line) {
    hv_gicv3_set_irq(p->gic, p->intx_base + line, p->line_count[line] > 0);
}

void hv_pci_set_intx(HvPciDev *d, int level) {
    HvPcie *p = d->bus;
    if (!p) return;
    uint16_t cmd = get16(d->config + PCI_COMMAND);
    int eff = level && !(cmd & CMD_INTX_DISABLE) && !hv_pci_msix_enabled(d);
    uint16_t status = get16(d->config + PCI_STATUS);
    put16(d->config + PCI_STATUS, level ? status | STATUS_INTX : status & ~STATUS_INTX);
    if (eff == d->intx) return;
    d->intx = eff;
    int line = intx_line_of(p, d);
    p->line_count[line] += eff ? 1 : -1;
    update_line(p, line);
}

/* For the state dump: every device's interrupt line, and how many devices hold each shared line up. */
void hv_pcie_dump(HvPcie *p, FILE *f) {
    for (int l = 0; l < 4; l++)
        if (p->line_count[l]) fprintf(f, "pci line %d (intid %d): held by %d\n", l, p->intx_base + l, p->line_count[l]);
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        HvPciDev *d = p->dev[s];
        if (!d) continue;
        fprintf(f, "pci %d: %04x:%04x cmd=%04x status=%04x intx=%d line=%d msix=%d pba=%llx\n", s, get16(d->config), get16(d->config + 2),
                get16(d->config + PCI_COMMAND), get16(d->config + PCI_STATUS), d->intx, intx_line_of(p, d), hv_pci_msix_enabled(d),
                (unsigned long long)d->msix_pba);
        for (int b = 0; b < 6; b++)
            if (d->bar_size[b])
                fprintf(f, "  bar %d: %llx, %llx bytes\n", b, (unsigned long long)hv_pci_bar_addr(d, b), (unsigned long long)d->bar_size[b]);
    }
}

static uint64_t ecam_read(void *opaque, uint64_t off, unsigned size) {
    HvPcie *p = opaque;
    uint32_t bus = (uint32_t)(off >> 20), slot = (off >> 15) & 31, fn = (off >> 12) & 7, reg = off & 0xfff;
    HvPciDev *d = bus == 0 && fn == 0 ? p->dev[slot] : NULL;
    if (!d) return size == 8 ? ~0ull : (1ull << (size * 8)) - 1;
    uint64_t v = 0;
    memcpy(&v, d->config + reg, size > 4096 - reg ? 4096 - reg : size);
    if (getenv("HV_PCI_LOG") && slot == (uint32_t)atoi(getenv("HV_PCI_LOG")))
        fprintf(stderr, "pci %u: read %03x (%u) = %llx\n", slot, reg, size, (unsigned long long)v);
    return v;
}

static void ecam_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPcie *p = opaque;
    uint32_t bus = (uint32_t)(off >> 20), slot = (off >> 15) & 31, fn = (off >> 12) & 7, reg = off & 0xfff;
    HvPciDev *d = bus == 0 && fn == 0 ? p->dev[slot] : NULL;
    if (!d) return;
    if (getenv("HV_PCI_LOG") && slot == (uint32_t)atoi(getenv("HV_PCI_LOG")))
        fprintf(stderr, "pci %u: write %03x (%u) = %llx\n", slot, reg, size, (unsigned long long)v);
    uint16_t old_cmd = get16(d->config + PCI_COMMAND);
    uint16_t old_msix = d->msix_cap ? get16(d->config + d->msix_cap + 2) : 0;
    for (unsigned i = 0; i < size && reg + i < 256; i++) {
        uint8_t w = d->wmask[reg + i], b = (uint8_t)(v >> (i * 8));
        d->config[reg + i] = (uint8_t)((d->config[reg + i] & ~w) | (b & w));
    }
    uint16_t new_msix = d->msix_cap ? get16(d->config + d->msix_cap + 2) : 0;
    if (((old_cmd ^ get16(d->config + PCI_COMMAND)) & CMD_INTX_DISABLE) || ((old_msix ^ new_msix) & 0x8000u)) {
        int level = (get16(d->config + PCI_STATUS) & STATUS_INTX) != 0;
        hv_pci_set_intx(d, level);
    }
    if (d->msix_cap && (old_msix ^ new_msix)) msix_check_pending(d);
    if (d->on_config_write) d->on_config_write(d);
}

static HvPciDev *bar_lookup(HvPcie *p, uint64_t pa, int *bar, uint64_t *off) {
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        HvPciDev *d = p->dev[s];
        if (!d || !(get16(d->config + PCI_COMMAND) & CMD_MEMORY)) continue;
        for (int b = 0; b < 6; b++) {
            if (!d->bar_size[b]) continue;
            uint64_t base = hv_pci_bar_addr(d, b);
            if (base && pa - base < d->bar_size[b]) {
                *bar = b;
                *off = pa - base;
                return d;
            }
        }
    }
    return NULL;
}

static int msix_bar_access(HvPciDev *d, int bar, uint64_t boff, uint64_t *val, unsigned size, int write) {
    if (!d->msix_cap || bar != d->msix_bar) return 0;
    uint32_t tlen = (uint32_t)d->msix_nvec * 16u;
    if (boff >= d->msix_table_off && boff + size <= d->msix_table_off + tlen) {
        uint8_t *tab = (uint8_t *)d->msix_table;
        uint32_t o = (uint32_t)(boff - d->msix_table_off);
        if (write) {
            memcpy(tab + o, val, size);
            msix_check_pending(d);
        } else {
            *val = 0;
            memcpy(val, tab + o, size);
        }
        return 1;
    }
    if (boff >= d->msix_pba_off && boff + size <= d->msix_pba_off + 8) {
        uint32_t o = (uint32_t)(boff - d->msix_pba_off);
        if (!write) {
            *val = (d->msix_pba >> (o * 8)) & (size >= 8 ? ~0ull : (1ull << (size * 8)) - 1);
        }
        return 1;
    }
    return 0;
}

static uint64_t dispatch_bar_read(HvPcie *p, uint64_t pa, unsigned size) {
    int bar;
    uint64_t boff, v = 0;
    HvPciDev *d = bar_lookup(p, pa, &bar, &boff);
    if (!d) return ~0ull;
    if (msix_bar_access(d, bar, boff, &v, size, 0)) return v;
    if (!d->bar_read) return ~0ull;
    return d->bar_read(d, bar, boff, size);
}

static void dispatch_bar_write(HvPcie *p, uint64_t pa, uint64_t v, unsigned size) {
    int bar;
    uint64_t boff;
    HvPciDev *d = bar_lookup(p, pa, &bar, &boff);
    if (!d) return;
    if (msix_bar_access(d, bar, boff, &v, size, 1)) return;
    if (d->bar_write) d->bar_write(d, bar, boff, v, size);
}

static uint64_t window_read(void *opaque, uint64_t off, unsigned size) {
    HvPcie *p = opaque;
    return dispatch_bar_read(p, p->mmio_base + off, size);
}

static void window_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPcie *p = opaque;
    dispatch_bar_write(p, p->mmio_base + off, v, size);
}

static uint64_t high_window_read(void *opaque, uint64_t off, unsigned size) {
    HvPcie *p = opaque;
    return dispatch_bar_read(p, p->high_mmio_base + off, size);
}

static void high_window_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPcie *p = opaque;
    dispatch_bar_write(p, p->high_mmio_base + off, v, size);
}

static uint64_t pio_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque; (void)off;
    return size >= 8 ? ~0ull : (1ull << (size * 8)) - 1;
}

static void pio_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)opaque; (void)off; (void)v; (void)size;
}

int hv_pcie_attach(HvPcie *p, HvPciDev *d) {
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        if (p->dev[s]) continue;
        p->dev[s] = d;
        d->bus = p;
        d->slot = s;
        return s;
    }
    return -1;
}

void hv_pcie_add_high_mmio(HvPcie *p, uint64_t high_pa, uint64_t high_size) {
    p->high_mmio_base = high_pa;
    p->high_mmio_size = high_size;
    hv_machine_add_mmio(p->m, high_pa, high_size, p, high_window_read, high_window_write, "pcie-high-mmio");
}

void hv_pcie_add_pio(HvPcie *p, uint64_t pio_pa, uint64_t pio_size) {
    hv_machine_add_mmio(p->m, pio_pa, pio_size, p, pio_read, pio_write, "pcie-pio");
}

void hv_pcie_set_gpex_swizzle(HvPcie *p, int enable) {
    p->gpex_swizzle = enable;
}

void hv_pcie_set_msi_doorbell(HvPcie *p, uint64_t doorbell_pa) {
    p->msi_doorbell_pa = doorbell_pa;
}

void hv_pcie_set_its(HvPcie *p, HvGicIts *its, uint64_t translater_pa) {
    p->its = its;
    p->its_translater_pa = translater_pa;
}

void hv_pcie_reset(HvPcie *p) {
    if (!p) return;
    memset(p->line_count, 0, sizeof p->line_count);
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        HvPciDev *d = p->dev[s];
        if (!d) continue;
        d->intx = 0;
        put16(d->config + PCI_COMMAND, 0);
        put16(d->config + PCI_STATUS, get16(d->config + PCI_STATUS) & ~STATUS_INTX);
        if (d->msix_cap) {
            put16(d->config + d->msix_cap + 2, get16(d->config + d->msix_cap + 2) & ~0xc000u);
            d->msix_pba = 0;
            for (uint16_t i = 0; i < d->msix_nvec; i++)
                d->msix_table[i] = (HvPciMsixEntry){.ctrl = 1};
        }
    }
}

HvPcie *hv_pcie_create(HvMachine *m, HvGic *gic, uint64_t ecam_pa, uint64_t ecam_size, uint64_t mmio_pa,
                       uint64_t mmio_size, int intx_intid) {
    HvPcie *p = calloc(1, sizeof *p);
    p->m = m;
    p->gic = gic;
    p->intx_base = intx_intid;
    p->mmio_base = mmio_pa;
    p->mmio_size = mmio_size;
    hv_pci_init(&p->bridge, 0x1b36, 0x0008, 0x06000000, 0x1af4, 0x1100); /* QEMU PCIe host bridge */
    p->bridge.config[0x3d] = 0; /* host bridge has no INTx pin */
    hv_pcie_attach(p, &p->bridge);
    hv_machine_add_mmio(m, ecam_pa, ecam_size, p, ecam_read, ecam_write, "pcie-ecam");
    hv_machine_add_mmio(m, mmio_pa, mmio_size, p, window_read, window_write, "pcie-mmio");
    return p;
}
