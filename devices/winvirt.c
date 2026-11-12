/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "winvirt.h"

#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/vm_map.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "mmu.h"
#include "timer.h"

extern kern_return_t mach_memory_entry_ownership(mach_port_t entry, task_t owner, int ledger_tag, int ledger_flags);

uint8_t *hv_alloc_no_footprint(uint64_t size) {
    enum {
        CHUNK = 128u << 20,
        MAP_MEM_LEDGER_TAGGED_ = 0x002000,
        MAP_MEM_NAMED_CREATE_ = 0x020000,
        LEDGER_TAG_DEFAULT = 1,
        LEDGER_NO_FOOTPRINT = 1,
    };
    vm_address_t base = 0;
    if (vm_allocate(mach_task_self(), &base, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return NULL;
    for (uint64_t off = 0; off < size; off += CHUNK) {
        uint64_t len = size - off < CHUNK ? size - off : CHUNK;
        memory_object_size_t got = len;
        mach_port_t entry = MACH_PORT_NULL;
        kern_return_t kr = mach_make_memory_entry_64(
            mach_task_self(), &got, 0,
            MAP_MEM_NAMED_CREATE_ | MAP_MEM_LEDGER_TAGGED_ | VM_PROT_READ | VM_PROT_WRITE,
            &entry, MACH_PORT_NULL);
        if (kr == KERN_SUCCESS)
            kr = mach_memory_entry_ownership(entry, TASK_NULL, LEDGER_TAG_DEFAULT, LEDGER_NO_FOOTPRINT);
        if (kr == KERN_SUCCESS) {
            vm_address_t to = base + off;
            kr = vm_map(mach_task_self(), &to, len, 0, VM_FLAGS_OVERWRITE, entry, 0, FALSE,
                        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        }
        if (entry != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), entry);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "winvm: guest RAM outside footprint refused (%s), falling back\n", mach_error_string(kr));
            vm_deallocate(mach_task_self(), base, size);
            return NULL;
        }
    }
    fprintf(stderr, "winvm: guest RAM (%llu MB) allocated outside Jetsam footprint\n",
            (unsigned long long)(size >> 20));
    return (uint8_t *)base;
}

struct PrefaultArgs {
    uint8_t *ram;
    uint64_t size;
};

static void *prefault_thread(void *arg) {
    struct PrefaultArgs a = *(struct PrefaultArgs *)arg;
    free(arg);
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    for (uint64_t off = 0; off < a.size; off += 0x1000)
        __atomic_fetch_add((volatile uint64_t *)(a.ram + off), 0, __ATOMIC_RELAXED);
    return NULL;
}

/* ACPI0013 Generic Event Device (GED) at 0x09080000: read clears latched event bits. */
static uint64_t ged_read(void *opaque, uint64_t off, unsigned size) {
    (void)size;
    HvWinVirt *b = opaque;
    if (off == 0) {
        uint32_t ev = __atomic_exchange_n(&b->ged_events, 0, __ATOMIC_ACQ_REL);
        return ev;
    }
    return 0;
}

static void ged_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)opaque;
    (void)off;
    (void)v;
    (void)size;
}

static uint64_t tick_locked(HvMachine *m, HvCpu *cpu) {
    HvWinVirt *b = m->tick_opaque;
    uint64_t deadline = hv_timer_update(b->gic, cpu);
    if (cpu->index != 0) return deadline;
    hv_pl031_tick(b->rtc);
    if (b->poll) b->poll();
    hv_xhci_poll(b->usb);
    if (b->hda) hv_hda_poll(b->hda);
    if (b->snd) hv_virtio_snd_poll(b->snd);
    return deadline;
}

static uint64_t tick(HvMachine *m, HvCpu *cpu) {
    if (cpu->index != 0) return hv_timer_update(((HvWinVirt *)m->tick_opaque)->gic, cpu);
    hv_dev_lock(m);
    uint64_t deadline = tick_locked(m, cpu);
    hv_dev_unlock(m);
    return deadline;
}

static void uart_stdout_cb(void *opaque, uint8_t c) {
    (void)opaque;
    fputc(c, stderr);
    if (c == '\n') fflush(stderr);
}

int hv_winvirt_create(HvWinVirt *b, HvMachine *m, HvCpu *cpu, int ncpus, uint64_t ram_size,
                      void *prealloc_ram, const char *code_fd_path, const char *vars_fd_path,
                      HvDisk *disk0, HvDisk *disk1, const uint8_t mac[6], int enable_sound) {
    memset(b, 0, sizeof *b);
    b->m = m;
    b->cpu = cpu;
    b->ncpus = ncpus < 1 ? 1 : ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : ncpus;
    b->ram_size = ram_size;

    /* 1. Allocate Guest RAM (pre-allocated ledger-exempt on iOS, debugger ledger-exempt, file-backed, or anon mmap) */
    const char *ram_file = getenv("HV_RAM_FILE");
    b->ram = prealloc_ram ? (uint8_t *)prealloc_ram : MAP_FAILED;
    if (b->ram == MAP_FAILED && !getenv("HV_RAM_NO_LEDGER")) {
        uint8_t *p = hv_alloc_no_footprint(ram_size);
        if (p) b->ram = p;
    }
    if (b->ram == MAP_FAILED && ram_file && *ram_file) {
        int fd = open(ram_file, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            unlink(ram_file);
            if (ftruncate(fd, (off_t)ram_size) == 0)
                b->ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
        }
        if (b->ram != MAP_FAILED && !getenv("HV_RAM_NO_PREFAULT")) {
            struct PrefaultArgs *a = malloc(sizeof *a);
            if (a) {
                a->ram = b->ram;
                a->size = ram_size;
                pthread_t t;
                if (pthread_create(&t, NULL, prefault_thread, a) == 0) pthread_detach(t);
                else free(a);
            }
        }
    }
    if (b->ram == MAP_FAILED)
        b->ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b->ram == MAP_FAILED) return -1;

    /* 2. Build Flattened Device Tree (placed at 0x40000000 on every reset) */
    b->dtb = hv_fdt_build_virt(b->ncpus, WINVIRT_RAM_PA, ram_size, &b->dtb_size);
    if (!b->dtb || b->dtb_size > ram_size) return -1;

    /* 3. Flash banks: pflash0 (64 MB read-only UEFI code HvRam) & pflash1 (64 MB persistent CFI01 NVRAM) */
    b->flash0 = hv_pflash_create(m, WINVIRT_FLASH0_PA, code_fd_path, 1);
    if (!b->flash0) {
        fprintf(stderr, "winvm: failed to load UEFI firmware %s\n", code_fd_path ? code_fd_path : "(null)");
        return -1;
    }
    b->flash1 = hv_pflash_create(m, WINVIRT_FLASH1_PA, vars_fd_path, 0);
    if (!b->flash1) {
        fprintf(stderr, "winvm: failed to open UEFI NVRAM %s\n", vars_fd_path ? vars_fd_path : "(null)");
        return -1;
    }

    /* 4. Register Guest RAM at 0x40000000 */
    hv_machine_add_ram(m, WINVIRT_RAM_PA, ram_size, b->ram, 0);

    /* 5. Interrupt controller: GICv3 + GICv3 ITS (LPI MSI-X for NVMe, xHCI, VirtIO) */
    b->gic = hv_gicv3_create(m, WINVIRT_GIC_DIST_PA, WINVIRT_GIC_REDIST_PA);
    b->its = hv_gicv3_its_create(m, b->gic, WINVIRT_GIC_ITS_PA);

    /* 6. Platform MMIO peripherals: PL011 UART, PL031 RTC, PL061 GPIO, ACPI GED, fw_cfg + ramfb + ACPI */
    b->uart = hv_pl011_create(m, b->gic, WINVIRT_UART_PA, WINVIRT_INTID_UART);
    if (getenv("HV_SERIAL_STDERR")) b->uart->output = uart_stdout_cb;
    b->rtc = hv_pl031_create(m, b->gic, WINVIRT_RTC_PA, WINVIRT_INTID_RTC);
    b->gpio = hv_pl061_create(m, b->gic, WINVIRT_GPIO_PA, WINVIRT_INTID_GPIO);
    hv_machine_add_mmio(m, WINVIRT_GED_PA, 0x1000, b, ged_read, ged_write, "acpi-ged");

    b->fw_cfg = hv_fw_cfg_create(m, WINVIRT_FW_CFG_PA, (uint16_t)ncpus);
    b->fb = hv_fb_display_create(m, b->fw_cfg);
    hv_acpi_build_and_register(b->fw_cfg, b->ncpus, ram_size, 0, 0);

    /* 7. PCIe Root Complex (GPEX):
     *    Slot 0: Host Bridge (1b36:0008)
     *    Slot 1: VirtIO-Net  (1af4:1000)
     *    Slot 2: NVMe 0      (1b36:0010, serial WIN11IOS01 -> matches UEFI Boot0001!)
     *    Slot 3: xHCI USB 3  (1b36:000d, Port 1 Keyboard + Port 2 Absolute Tablet)
     *    Slot 4: NVMe 1      (optional installer/secondary image)
     *    Slot 5: VirtIO-Snd  (optional audio)
     *    Next free slot after those (5, or 6 with the installer image): VirtIO-GPU (1af4:1050, INTx like the rest) */
    b->pcie = hv_pcie_create(m, b->gic, WINVIRT_PCIE_ECAM_PA, WINVIRT_PCIE_ECAM_SZ,
                             WINVIRT_PCIE_MMIO_PA, WINVIRT_PCIE_MMIO_SZ, WINVIRT_INTID_PCIE);
    hv_pcie_add_pio(b->pcie, WINVIRT_PCIE_PIO_PA, WINVIRT_PCIE_PIO_SZ);
    hv_pcie_add_high_mmio(b->pcie, WINVIRT_PCIE_HIGH_PA, WINVIRT_PCIE_HIGH_SZ);
    hv_pcie_set_gpex_swizzle(b->pcie, 1);
    hv_pcie_set_its(b->pcie, b->its, WINVIRT_GIC_TRANSLATER);

    static const uint8_t default_mac[6] = {0x02, 0x57, 0x49, 0x4e, 0x00, 0x01};
    b->net = hv_virtio_net_create(m, b->pcie, mac ? mac : default_mac);
    if (disk0) b->nvme0 = hv_nvme_create(m, b->pcie, disk0, "WIN11IOS01");
    b->usb = hv_xhci_create(m, b->pcie);
    b->hda = hv_hda_create(m, b->pcie);
    /* Off unless HV_VIRTIO_GPU is set: virtio-win's viogpudo bug-checks Windows on this board
       (IRQL_NOT_LESS_OR_EQUAL seconds after it starts), so Windows stays on ramfb for now. */
    if (getenv("HV_VIRTIO_GPU")) b->gpu = hv_virtio_gpu_create(m, b->pcie, b->fb);
    if (disk1) b->nvme1 = hv_nvme_create(m, b->pcie, disk1, "WIN11INST");
    (void)enable_sound;

    m->tick = tick;
    m->tick_opaque = b;
    return 0;
}

void hv_winvirt_reset_cpu(HvWinVirt *b) {
    /* Place fresh DTB at 0x40000000 for EDK2 ArmVirtQemu PrePi entry */
    if (b->ram && b->dtb && b->dtb_size <= b->ram_size) {
        memcpy(b->ram, b->dtb, b->dtb_size);
    }
    for (int i = 0; i < b->ncpus; i++) {
        HvCpu *c = &b->cpu[i];
        hv_cpu_reset_index(c, b->m, (uint32_t)i);
        c->mpidr = 0x80000000ULL | (uint32_t)i;
        c->pc = WINVIRT_FLASH0_PA;
        c->x[0] = WINVIRT_RAM_PA; /* DTB physical address convention */
        c->off = (i != 0);
        c->halted = 0;
    }
}

void hv_winvirt_power_button(HvWinVirt *b) {
    if (!b || !b->gic) return;
    __atomic_fetch_or(&b->ged_events, 0x2U, __ATOMIC_ACQ_REL);
    hv_gicv3_pend(b->gic, WINVIRT_INTID_GED);
}
