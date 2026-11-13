// GICv3 ITS: mappings made through the command queue must survive the ITS losing its state while guest RAM
// keeps it, which is what resuming Windows from hibernation looks like (it never maps its devices again).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gicv3.h"
#include "machine.h"

#define GPA 0x40000000ull
#define MEM_SIZE (4u << 20)
#define ITS_PA 0x08080000ull
#define DEV_TABLE (GPA + 0x100000)  /* 64 KB: 8192 devices of 8 bytes */
#define COLL_TABLE (GPA + 0x110000) /* 4 KB */
#define CMD_RING (GPA + 0x120000)   /* 4 KB: 128 commands */
#define ITT (GPA + 0x130000)
#define DEV_TABLE_L1 (GPA + 0x140000) /* two-level: one 4 KB level-1 page */
#define DEV_TABLE_L2 (GPA + 0x141000)
#define DEV_TABLE_NEW (GPA + 0x160000) /* 64 KB */
#define ITT_BIG (GPA + 0x180000)       /* 2048 events */

static HvMachine m;
static HvGic *gic;
static HvGicIts *its;
static uint8_t *mem;
static uint32_t cmd_tail;
static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %s: %llx, expected %llx\n", what, (unsigned long long)got, (unsigned long long)want);
    }
}

static void its_w(uint64_t off, uint64_t v) { hv_phys_write(&m, ITS_PA + off, &v, 8); }

static void cmd(uint64_t d0, uint64_t d1, uint64_t d2, uint64_t d3) {
    uint64_t c[4] = {d0, d1, d2, d3};
    memcpy(mem + (CMD_RING - GPA) + cmd_tail, c, 32);
    cmd_tail = (cmd_tail + 32) % 4096;
    its_w(0x88, cmd_tail); /* CWRITER */
}

/* What Windows writes when it starts the ITS, and again when it resumes. */
static void program(int two_level) {
    uint64_t dev = two_level ? 1ull << 63 | 1ull << 62 | DEV_TABLE_L1 | 0 /* 1 page */
                             : 1ull << 63 | DEV_TABLE | 15 /* 16 pages */;
    its_w(0x100, dev);
    its_w(0x108, 1ull << 63 | COLL_TABLE);
    its_w(0x80, 1ull << 63 | CMD_RING); /* CBASER: one page */
    cmd_tail = 0;
    its_w(0x00, 1); /* CTLR.Enabled */
}

static int pending(int cpu, uint32_t lpi) {
    uint32_t i = lpi - HV_GIC_LPI_BASE;
    return (int)(gic->cpu[cpu].lpi_pending[i / 64] >> (i % 64) & 1);
}

static void clear_pending(void) {
    for (int c = 0; c < HV_MAX_CPUS; c++) memset(gic->cpu[c].lpi_pending, 0, sizeof gic->cpu[c].lpi_pending);
}

/* RDbase as a processor number (GITS_TYPER.PTA = 0). */
static void map(uint32_t dev, uint32_t event, uint32_t lpi, uint16_t icid, int cpu) {
    cmd(0x08 | (uint64_t)dev << 32, 4 /* 5 EventID bits */, 1ull << 63 | ITT, 0);               /* MAPD */
    cmd(0x09, 0, 1ull << 63 | (uint64_t)cpu << 16 | icid, 0);                                   /* MAPC */
    cmd(0x0a | (uint64_t)dev << 32, (uint64_t)lpi << 32 | event, icid, 0);                       /* MAPTI */
}

static void test_resume(int two_level) {
    memset(mem, 0, MEM_SIZE);
    if (two_level) {
        uint64_t l1 = 1ull << 63 | DEV_TABLE_L2;
        memcpy(mem + (DEV_TABLE_L1 - GPA), &l1, 8);
    }
    hv_gicv3_its_reset(its);
    clear_pending();
    program(two_level);
    map(0x10, 4, HV_GIC_LPI_BASE + 7, 3, 2);
    hv_gicv3_its_msi(its, 0x10, 4);
    check("mapped MSI reaches its CPU", pending(2, HV_GIC_LPI_BASE + 7), 1);
    check("and no other", pending(0, HV_GIC_LPI_BASE + 7), 0);

    /* Power off: the ITS forgets everything, RAM (with its tables) is restored as it was. */
    hv_gicv3_its_reset(its);
    clear_pending();
    program(two_level);
    hv_gicv3_its_msi(its, 0x10, 4);
    check("after a resume, the mapping is read from the tables", pending(2, HV_GIC_LPI_BASE + 7), 1);
    hv_gicv3_its_msi(its, 0x10, 5);
    check("an unmapped event stays dropped", pending(2, HV_GIC_LPI_BASE + 5), 0);
    hv_gicv3_its_msi(its, 0x11, 4);
    check("an unmapped device stays dropped", pending(2, HV_GIC_LPI_BASE + 4), 0);

    /* Unmapping reaches the tables too. */
    cmd(0x0f | 0x10ull << 32, 4, 0, 0); /* DISCARD */
    hv_gicv3_its_reset(its);
    clear_pending();
    program(two_level);
    hv_gicv3_its_msi(its, 0x10, 4);
    check("a discarded event is not resurrected", pending(2, HV_GIC_LPI_BASE + 7), 0);
}

/* The OS pointing the ITS at new (zeroed) tables: whatever the cache held is stale. */
static void test_new_tables(void) {
    memset(mem, 0, MEM_SIZE);
    hv_gicv3_its_reset(its);
    clear_pending();
    program(0);
    map(0x10, 4, HV_GIC_LPI_BASE + 7, 3, 2);
    hv_gicv3_its_msi(its, 0x10, 4);
    check("mapped", pending(2, HV_GIC_LPI_BASE + 7), 1);
    clear_pending();
    its_w(0x00, 0);
    its_w(0x100, 1ull << 63 | DEV_TABLE_NEW | 15);
    its_w(0x00, 1);
    hv_gicv3_its_msi(its, 0x10, 4);
    check("new device table: the cached mapping is gone", pending(2, HV_GIC_LPI_BASE + 7), 0);
}

/* More events than the cache holds: the ones read from the tables are still delivered. */
static void test_cache_overflow(void) {
    memset(mem, 0, MEM_SIZE);
    hv_gicv3_its_reset(its);
    clear_pending();
    program(0);
    cmd(0x08 | 0x20ull << 32, 10 /* 11 EventID bits */, 1ull << 63 | ITT_BIG, 0);
    cmd(0x09, 0, 1ull << 63 | 1ull << 16 | 7, 0);
    for (uint32_t e = 0; e < 1100; e++) cmd(0x0a | 0x20ull << 32, (uint64_t)(HV_GIC_LPI_BASE + e) << 32 | e, 7, 0);
    hv_gicv3_its_msi(its, 0x20, 1099);
    check("event 1099 delivered", pending(1, HV_GIC_LPI_BASE + 1099), 1);
    hv_gicv3_its_reset(its);
    clear_pending();
    program(0);
    for (uint32_t e = 0; e < 1100; e++) hv_gicv3_its_msi(its, 0x20, e);
    int all = 1;
    for (uint32_t e = 0; e < 1100; e++) all &= pending(1, HV_GIC_LPI_BASE + e);
    check("after a resume, every one of 1100 events delivered (cache holds 1024)", all, 1);
}

/* MOVI (0x01) gives an event another collection; MOVALL is 0x0e. */
static void test_movi(void) {
    memset(mem, 0, MEM_SIZE);
    hv_gicv3_its_reset(its);
    clear_pending();
    program(0);
    map(0x10, 4, HV_GIC_LPI_BASE + 7, 3, 2);
    cmd(0x09, 0, 1ull << 63 | 1ull << 16 | 5, 0);  /* MAPC 5 -> CPU 1 */
    cmd(0x01 | 0x10ull << 32, 4, 5, 0);             /* MOVI to collection 5 */
    hv_gicv3_its_msi(its, 0x10, 4);
    check("MOVI: delivered to the new collection's CPU", pending(1, HV_GIC_LPI_BASE + 7), 1);
    check("MOVI: not the old one", pending(2, HV_GIC_LPI_BASE + 7), 0);
    hv_gicv3_its_reset(its);
    clear_pending();
    program(0);
    hv_gicv3_its_msi(its, 0x10, 4);
    check("MOVI reached the tables", pending(1, HV_GIC_LPI_BASE + 7), 1);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    mem = aligned_alloc(65536, MEM_SIZE);
    hv_machine_add_ram(&m, GPA, MEM_SIZE, mem, 0);
    gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    its = hv_gicv3_its_create(&m, gic, ITS_PA);
    if (!gic || !its) return 1;
    m.ncpus = 4;

    printf("== flat device table\n");
    test_resume(0);
    printf("== two-level device table\n");
    test_resume(1);
    printf("== new tables\n");
    test_new_tables();
    printf("== more events than the cache\n");
    test_cache_overflow();
    printf("== MOVI\n");
    test_movi();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
