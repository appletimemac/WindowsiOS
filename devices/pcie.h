#ifndef HENGEVM_PCIE_H
#define HENGEVM_PCIE_H

#include <stdio.h>
#include <stdint.h>

#include "gicv3.h"

/* PCIe host bridge (GPEX / vmapple compatible): ECAM config space, 32-bit + optional
   64-bit high MMIO windows, legacy INTx (direct or GPEX swizzled), PCIe cap, and MSI-X. */
#define HV_PCI_SLOTS 32
#define HV_PCI_MAX_MSIX 32

struct HvPcie;

typedef struct HvPciMsixEntry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t data;
    uint32_t ctrl; /* bit 0: vector mask */
} HvPciMsixEntry;

typedef struct HvPciDev {
    struct HvPcie *bus;
    int slot;
    uint8_t config[4096];
    uint8_t wmask[256];       /* writable bits of the config space (0..255) */
    uint64_t bar_size[6];     /* 0: no BAR; a 64-bit BAR's size sits in its low half */
    uint8_t bar64[6];
    int intx;                 /* current INTA level */
    /* Optional MSI-X state */
    uint8_t msix_cap;         /* config offset of MSI-X cap (0: none) */
    int msix_bar;
    uint16_t msix_nvec;
    uint32_t msix_table_off;
    uint32_t msix_pba_off;
    uint64_t msix_pba;
    HvPciMsixEntry msix_table[HV_PCI_MAX_MSIX];

    void *opaque;
    uint64_t (*bar_read)(struct HvPciDev *d, int bar, uint64_t off, unsigned size);
    void (*bar_write)(struct HvPciDev *d, int bar, uint64_t off, uint64_t v, unsigned size);
    void (*on_config_write)(struct HvPciDev *d);
} HvPciDev;

typedef struct HvPcie {
    HvMachine *m;
    HvGic *gic;
    int intx_base;            /* INTID of INTx line 0 */
    int gpex_swizzle;         /* 1: QEMU gpex (slot + pin - 1) % 4 on 4 lines; 0: vmapple direct */
    uint64_t msi_doorbell_pa; /* GICv2m MSI_SETSPI_NS PA (e.g. 0x08080040), or 0 */
    HvGicIts *its;            /* GICv3 ITS (GITS_TRANSLATER at its_pa + 0x10040), or NULL */
    uint64_t its_translater_pa;
    int line_count[HV_PCI_SLOTS];
    HvPciDev *dev[HV_PCI_SLOTS];
    HvPciDev bridge;
    uint64_t mmio_base, mmio_size;
    uint64_t high_mmio_base, high_mmio_size;
} HvPcie;

HvPcie *hv_pcie_create(HvMachine *m, HvGic *gic, uint64_t ecam_pa, uint64_t ecam_size, uint64_t mmio_pa,
                       uint64_t mmio_size, int intx_intid);
void hv_pcie_add_high_mmio(HvPcie *p, uint64_t high_pa, uint64_t high_size);
void hv_pcie_add_pio(HvPcie *p, uint64_t pio_pa, uint64_t pio_size);
void hv_pcie_set_gpex_swizzle(HvPcie *p, int enable);
void hv_pcie_set_msi_doorbell(HvPcie *p, uint64_t doorbell_pa);
void hv_pcie_set_its(HvPcie *p, HvGicIts *its, uint64_t translater_pa);
void hv_pcie_reset(HvPcie *p);
void hv_pcie_dump(HvPcie *p, FILE *f); /* interrupt lines, for the state dump */

/* Fills in the header; the caller then adds BARs and capabilities. */
void hv_pci_init(HvPciDev *d, uint16_t vendor, uint16_t device, uint32_t class_rev, uint16_t sub_vendor,
                 uint16_t sub_id);
void hv_pci_add_bar(HvPciDev *d, int bar, uint64_t size, int is64);
void hv_pci_add_bar_flags(HvPciDev *d, int bar, uint64_t size, int is64, int prefetch);
void hv_pci_add_pm_cap(HvPciDev *d, uint8_t at);
void hv_pci_add_pcie_cap(HvPciDev *d, uint8_t at);
void hv_pci_add_msix(HvPciDev *d, uint8_t at, int bar, uint32_t table_off, uint32_t pba_off, uint16_t nvec);
int hv_pci_msix_enabled(const HvPciDev *d);
void hv_pci_msix_notify(HvPciDev *d, uint16_t vec);

int hv_pcie_attach(HvPcie *p, HvPciDev *d); /* returns the slot, or -1 */
uint64_t hv_pci_bar_addr(HvPciDev *d, int bar);
int hv_pci_mem_enabled(const HvPciDev *d);
void hv_pci_set_intx(HvPciDev *d, int level);

#endif
