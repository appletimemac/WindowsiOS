// Stage 1 walker: granules, block sizes, TTBR0/TTBR1, TBI, permissions and
// fault codes, checked directly against hv_mmu_walk.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "mmu.h"
#include "sysreg.h"

#define PT_PA 0x80000000ull
#define PT_SIZE (1u << 20)

static uint8_t *pt;
static uint64_t pt_next;
static int failures, checks;

/* The engine's own calls of these two land here and are counted, to show that a TLB refill makes none: it
   comes from translated code with the guest's vector registers live, and libc is free to use those. */
static int getenv_calls, calloc_calls;
char *getenv(const char *name) {
    (void)name;
    getenv_calls++;
    return NULL;
}
void *calloc(size_t count, size_t size) {
    static void *(*real)(size_t, size_t);
    if (!real) real = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    calloc_calls++;
    return real(count, size);
}

static uint64_t *host(uint64_t pa) {
    return (uint64_t *)(pt + (pa - PT_PA));
}

static uint64_t alloc_table(void) {
    uint64_t pa = PT_PA + pt_next;
    pt_next += 65536; /* big enough and aligned for any granule */
    memset(host(pa), 0, 65536);
    return pa;
}

/* Install a leaf for va at `leaf_level`, creating tables from the start level. */
static uint64_t *map(uint64_t root, int g, int inputsize, uint64_t va, int leaf_level, uint64_t desc,
                     uint64_t table_bits) {
    int stride = g - 3;
    int levels = (inputsize - g + stride - 1) / stride;
    uint64_t table = root;
    for (int level = 4 - levels;; level++) {
        int shift = g + stride * (3 - level);
        int idx_bits = level == 4 - levels ? inputsize - shift : stride;
        uint64_t *e = &host(table)[(va >> shift) & ((1ull << idx_bits) - 1)];
        if (level == leaf_level) {
            *e = desc;
            return e;
        }
        if (!(*e & 1)) *e = alloc_table() | 3 | table_bits;
        table = *e & 0x0000fffffffff000ull & ~((1ull << g) - 1);
    }
}

#define SLOT_OF(va, table) hv_tlb_ent(&cpu, (uint32_t)((va) >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), table)
#define AF (1ull << 10)
#define PAGE 3ull
#define BLOCK 1ull
#define AP(x) ((uint64_t)(x) << 6)
#define PXN (1ull << 53)
#define UXN (1ull << 54)

static void expect(const char *what, HvCpu *cpu, uint64_t va, HvAccess acc, int el, uint64_t want_pa, uint32_t want_fsc) {
    HvWalk w;
    hv_mmu_walk(cpu, va, acc, el, &w);
    checks++;
    if (w.fsc != want_fsc || (!want_fsc && w.pa != want_pa)) {
        printf("  FAIL %s: va %llx -> pa %llx fsc %02x, expected pa %llx fsc %02x\n", what, (unsigned long long)va,
               (unsigned long long)w.pa, w.fsc, (unsigned long long)want_pa, want_fsc);
        failures++;
    }
}

int main(void) {
    static HvMachine m;
    static HvCpu cpu;
    hv_machine_init(&m, 1 << 20);
    hv_knob[HV_K_FLUSH_CHECK] = 1; /* every full flush below is looked through by the engine: anything left ends the run */
    pt = aligned_alloc(65536, PT_SIZE);
    hv_machine_add_ram(&m, PT_PA, PT_SIZE, pt, 0);
    hv_cpu_reset(&cpu, &m);

    expect("mmu off", &cpu, 0x12345678, HV_ACC_WRITE, 1, 0x12345678, 0);

    /* 16 KB granule, 48-bit TTBR0 (four levels), 39-bit TTBR1 (three levels). */
    uint64_t r0 = alloc_table(), r1 = alloc_table();
    cpu.ttbr0_el1 = r0 | 0x1234ull << 48; /* an ASID must not leak into the table address */
    cpu.ttbr1_el1 = r1;
    cpu.mair_el1 = 0x00ff;
    cpu.tcr_el1 = 16 | 2u << 14 | 25u << 16 | 1u << 30 | 5ull << 32 | 1ull << 37; /* T0SZ 16, TG0 16K, T1SZ 25, TG1 16K, TBI0 */
    cpu.sctlr_el1 |= 1;

    /* The first kernel refill of the run, then one with the kept-entry checker on: neither may reach libc.
       Settings are read and the checker's state is made while the machine is set up, not at first use. */
    {
        const uint64_t kv = 0xffffff8000004000ull, read8_el1 = 1u << 1 | 7u << 3;
        map(r1, 14, 39, kv, 3, (PT_PA + 0xf0000) | PAGE | AF, 0);
        int env = getenv_calls, made = calloc_calls;
        int missed = hv_tlb_refill(&cpu, kv, read8_el1);
        checks += 3;
        if (missed) printf("  FAIL refill: the kernel page did not fill\n"), failures++;
        if (getenv_calls != env) printf("  FAIL refill: the first kernel refill read the environment\n"), failures++;
        if (calloc_calls != made) printf("  FAIL refill: the first kernel refill allocated\n"), failures++;
        hv_ktlb_check_set(1);
        hv_tlb_flush(&cpu);
        env = getenv_calls, made = calloc_calls;
        missed = hv_tlb_refill(&cpu, kv + 0x10, read8_el1); /* another address: the same one again counts as a miss */
        checks += 3;
        if (missed) printf("  FAIL refill: the kernel page did not fill with the checker on\n"), failures++;
        if (getenv_calls != env) printf("  FAIL refill: a refill with the checker on read the environment\n"), failures++;
        if (calloc_calls != made) printf("  FAIL refill: a refill with the checker on allocated\n"), failures++;
        hv_ktlb_check_set(0);
        hv_tlb_flush(&cpu);
    }

    map(r0, 14, 48, 0x100004000, 3, 0x40008000 | PAGE | AF | AP(0), 0);
    expect("16K page", &cpu, 0x100004abc, HV_ACC_READ, 1, 0x40008abc, 0);
    expect("16K page, next page unmapped", &cpu, 0x100008000, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(3));
    expect("16K, no level 1 table", &cpu, 0x7f0000000000, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(1));
    expect("16K, VA above T0SZ", &cpu, 0x0001000000000000, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(0));
    expect("TBI0 ignores the top byte", &cpu, 0x5a00000100004010, HV_ACC_READ, 1, 0x40008010, 0);
    cpu.tcr_el1 &= ~(1ull << 37);
    expect("without TBI0 a tag faults", &cpu, 0x5a00000100004010, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(0));
    cpu.tcr_el1 |= 1ull << 37;

    map(r0, 14, 48, 0x200000000, 2, 0x82000000 | BLOCK | AF, 0);
    expect("16K: 32 MB block", &cpu, 0x201234560, HV_ACC_WRITE, 1, 0x83234560, 0);
    map(r0, 14, 48, 0x300000000, 3, 0x40010000 | PAGE, 0);
    expect("access flag clear", &cpu, 0x300000000, HV_ACC_READ, 1, 0, HV_FSC_ACCESS(3));

    /* Permissions. */
    map(r0, 14, 48, 0x100010000, 3, 0x40010000 | PAGE | AF | AP(2), 0); /* EL1 RO */
    expect("EL1 RO: read", &cpu, 0x100010000, HV_ACC_READ, 1, 0x40010000, 0);
    expect("EL1 RO: write", &cpu, 0x100010000, HV_ACC_WRITE, 1, 0, HV_FSC_PERMISSION(3));
    expect("EL1 only: EL0 read", &cpu, 0x100004000, HV_ACC_READ, 0, 0, HV_FSC_PERMISSION(3));
    map(r0, 14, 48, 0x100014000, 3, 0x40014000 | PAGE | AF | AP(1), 0); /* EL0 and EL1 RW */
    expect("EL0 RW: EL0 write", &cpu, 0x100014000, HV_ACC_WRITE, 0, 0x40014000, 0);
    expect("EL0 RW: EL1 cannot execute", &cpu, 0x100014000, HV_ACC_EXEC, 1, 0, HV_FSC_PERMISSION(3));
    expect("EL0 RW: EL0 executes", &cpu, 0x100014000, HV_ACC_EXEC, 0, 0x40014000, 0);
    cpu.pstate_extra |= HV_PSTATE_PAN;
    expect("PAN: EL1 read of an EL0 page", &cpu, 0x100014000, HV_ACC_READ, 1, 0, HV_FSC_PERMISSION(3));
    expect("PAN: EL1 page unaffected", &cpu, 0x100004000, HV_ACC_READ, 1, 0x40008000, 0);
    expect("PAN: LDTR uses EL0 rights", &cpu, 0x100014000, HV_ACC_READ, 0, 0x40014000, 0);
    cpu.pstate_extra &= ~(uint64_t)HV_PSTATE_PAN;
    map(r0, 14, 48, 0x100018000, 3, 0x40018000 | PAGE | AF | AP(3) | UXN, 0); /* RO both, EL0 XN */
    expect("UXN: EL0 exec", &cpu, 0x100018000, HV_ACC_EXEC, 0, 0, HV_FSC_PERMISSION(3));
    expect("UXN: EL1 exec", &cpu, 0x100018000, HV_ACC_EXEC, 1, 0x40018000, 0);
    map(r0, 14, 48, 0x10001c000, 3, 0x4001c000 | PAGE | AF | PXN, 0);
    expect("PXN: EL1 exec", &cpu, 0x10001c000, HV_ACC_EXEC, 1, 0, HV_FSC_PERMISSION(3));
    cpu.sctlr_el1 |= 1u << 19;
    expect("WXN: writable is not executable", &cpu, 0x100004000, HV_ACC_EXEC, 1, 0, HV_FSC_PERMISSION(3));
    cpu.sctlr_el1 &= ~(1ull << 19);
    expect("no WXN: executable", &cpu, 0x100004000, HV_ACC_EXEC, 1, 0x40008000, 0);

    /* APTable on a new level 1 table: bit 61 = no EL0, bit 62 = no writes. */
    map(r0, 14, 48, 0x500000000000, 3, 0x40020000 | PAGE | AF | AP(1), 1ull << 61);
    expect("APTable no-EL0", &cpu, 0x500000000000, HV_ACC_READ, 0, 0, HV_FSC_PERMISSION(3));
    map(r0, 14, 48, 0x600000000000, 3, 0x40024000 | PAGE | AF | AP(0), 1ull << 62);
    expect("APTable read-only", &cpu, 0x600000000000, HV_ACC_WRITE, 1, 0, HV_FSC_PERMISSION(3));
    expect("APTable read-only: read", &cpu, 0x600000000000, HV_ACC_READ, 1, 0x40024000, 0);

    /* TTBR1, 39-bit: starts at level 1. */
    map(r1, 14, 39, 0xffffffc012340000, 3, 0x40028000 | PAGE | AF, 0);
    expect("TTBR1 page", &cpu, 0xffffffc012343210, HV_ACC_READ, 1, 0x4002b210, 0);
    expect("TTBR1: hole between ranges", &cpu, 0xffff000012340000, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(0));
    cpu.tcr_el1 |= 1u << 23;
    expect("EPD1 disables TTBR1 walks", &cpu, 0xffffffc012343210, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(0));
    cpu.tcr_el1 &= ~(1ull << 23);

    /* Non-RAM mappings do not enter the TLB. */
    uint64_t pa;
    hv_tlb_flush(&cpu);
    hv_mmu_access(&cpu, 0x100004000, HV_ACC_READ, 1, &pa);
    checks++;
    /* 0x40008000 is not RAM here, so nothing may be filled */
    if ((*hv_tlb_ent(&cpu, (0x100004000 >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), 1)).read_tag != HV_TLB_INVALID) {
        printf("  FAIL TLB filled for non-RAM\n");
        failures++;
    }

    /* 4 KB granule, 39-bit TTBR0 (levels 1-3): pages, 2 MB and 1 GB blocks. */
    pt_next = 2 * 65536;
    uint64_t r4 = alloc_table();
    cpu.ttbr0_el1 = r4;
    cpu.tcr_el1 = 25 | 0u << 14 | 1u << 23 | 5ull << 32;
    map(r4, 12, 39, 0x12345000, 3, (PT_PA + 0xf0000) | PAGE | AF, 0);
    expect("4K page", &cpu, 0x12345678, HV_ACC_READ, 1, PT_PA + 0xf0678, 0);
    map(r4, 12, 39, 0x40200000, 2, 0x80400000 | BLOCK | AF, 0);
    expect("4K: 2 MB block", &cpu, 0x40212345, HV_ACC_READ, 1, 0x80412345, 0);
    map(r4, 12, 39, 0x4000000000 - 0x40000000, 1, 0xc0000000 | BLOCK | AF, 0);
    expect("4K: 1 GB block", &cpu, 0x3fc0123456, HV_ACC_READ, 1, 0xc0123456, 0);
    hv_tlb_flush(&cpu);
    hv_mmu_access(&cpu, 0x12345678, HV_ACC_READ, 1, &pa);
    checks++;
    if ((*hv_tlb_ent(&cpu, (0x12345678 >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), 1)).read_tag == HV_TLB_INVALID) {
        printf("  FAIL a 4K RAM mapping did not enter the 4K TLB\n");
        failures++;
    }

    /* 64 KB granule, 42-bit TTBR0 (levels 2-3): pages and 512 MB blocks. */
    uint64_t r6 = alloc_table();
    cpu.ttbr0_el1 = r6;
    cpu.tcr_el1 = 22 | 1u << 14 | 1u << 23 | 5ull << 32;
    map(r6, 16, 42, 0x123450000, 3, 0x40050000 | PAGE | AF, 0);
    expect("64K page", &cpu, 0x12345abcd, HV_ACC_READ, 1, 0x4005abcd, 0);
    map(r6, 16, 42, 0x200000000, 2, 0x60000000 | BLOCK | AF, 0);
    expect("64K: 512 MB block", &cpu, 0x21234abcd, HV_ACC_READ, 1, 0x7234abcd, 0);
    map(r6, 16, 42, 0x300000000, 3, 0x40060000 | BLOCK | AF, 0);
    expect("block descriptor at level 3 is invalid", &cpu, 0x300000000, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(3));

    /* TLBI RVAE1IS: 6 pages of 4 KB from the base drop, the ones after stay; also for a kernel range. */
    {
        static const uint64_t bases[2] = {0x100000000ull, 0xfffffe0012340000ull};
        for (int b = 0; b < 2; b++) {
            hv_tlb_flush(&cpu);
            int table = (bases[b] >> 55) & 1 ? 1 : 0;
            for (int k = 0; k < 8; k++) {
                uint64_t va = bases[b] + ((uint64_t)k << HV_TLB_PAGE_BITS);
                HvTlbEntry *t = &(*hv_tlb_ent(&cpu, (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), table));
                hv_tlb_note(&cpu, (uint32_t)(t - cpu.tlb));
                t->read_tag = va - HV_TLB_ADJ;
                t->addend = 0;
            }
            cpu.el = 1;
            cpu.x[1] = 5ull << 48 | 1ull << 46 | 0ull << 44 | 2ull << 39 | ((bases[b] >> 12) & 0x1fffffffffull);
            hv_sysreg_sys(&cpu, 0xd5088221u, 0); /* tlbi rvae1is, x1 */
            for (int k = 0; k < 8; k++) {
                uint64_t va = bases[b] + ((uint64_t)k << HV_TLB_PAGE_BITS);
                int dropped = (*hv_tlb_ent(&cpu, (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), table)).read_tag == HV_TLB_INVALID;
                checks++;
                if (dropped != (k < 6)) {
                    printf("  FAIL range TLBI, base %llx page %d: %s\n", (unsigned long long)bases[b], k,
                           dropped ? "dropped" : "kept");
                    failures++;
                }
            }
        }
    }

    /* A big kernel-range TLBI drops the kernel pages in the range and keeps the rest, the processes' entries included
       (it used to flush everything). 512 pages goes page by page, 6144 pages by a scan. */
    {
        static const struct { uint32_t scale, num, pages; } ranges[2] = {{1, 7, 512}, {2, 2, 6144}};
        const uint64_t base = 0xfffffe0040000000ull;
        for (int r = 0; r < 2; r++) {
            hv_tlb_flush(&cpu);
            const int32_t ks[6] = {0, 1, (int32_t)ranges[r].pages / 2, (int32_t)ranges[r].pages - 1, (int32_t)ranges[r].pages, (int32_t)ranges[r].pages + 300};
            for (int k = 0; k < 6; k++) {
                uint64_t va = base + ((uint64_t)ks[k] << HV_TLB_PAGE_BITS);
                HvTlbEntry *t = hv_tlb_ent(&cpu, (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), 1);
                hv_tlb_note(&cpu, (uint32_t)(t - cpu.tlb));
                t->read_tag = va - HV_TLB_ADJ;
                t->addend = 0;
            }
            uint64_t uva = 0x100000000ull + ((uint64_t)7000 << HV_TLB_PAGE_BITS); /* a process's page, in a slot outside the range's */
            HvTlbEntry *u = hv_tlb_ent(&cpu, (uva >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), 0);
            hv_tlb_note(&cpu, (uint32_t)(u - cpu.tlb));
            u->read_tag = uva - HV_TLB_ADJ;
            u->addend = 0;
            cpu.el = 1;
            cpu.x[1] = 0ull << 48 | 1ull << 46 | (uint64_t)ranges[r].scale << 44 | (uint64_t)ranges[r].num << 39 | ((base >> 12) & 0x1fffffffffull);
            hv_sysreg_sys(&cpu, 0xd5088221u, 0); /* tlbi rvae1is, x1 */
            for (int k = 0; k < 6; k++) {
                uint64_t va = base + ((uint64_t)ks[k] << HV_TLB_PAGE_BITS);
                int dropped = hv_tlb_ent(&cpu, (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), 1)->read_tag == HV_TLB_INVALID;
                checks++;
                if (dropped != (ks[k] < (int32_t)ranges[r].pages)) {
                    printf("  FAIL big kernel range %u pages, page %d: %s\n", ranges[r].pages, ks[k], dropped ? "dropped" : "kept");
                    failures++;
                }
            }
            checks++;
            if (hv_tlb_ent(&cpu, (uva >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), 0)->read_tag == HV_TLB_INVALID) {
                printf("  FAIL big kernel range %u pages dropped a process's entry\n", ranges[r].pages);
                failures++;
            }
        }
    }

    /* A TLBI by VA and ASID forgets the indirect-branch cache entries of that page
       for that address space, and nothing else. Entries are filled the way the dispatcher does it. */
    {
        hv_cpu_reset(&cpu, &m);
        cpu.tcr_el1 = 0;
        cpu.ttbr0_el1 = 7ull << 48;
        hv_ijc_switch(&cpu);
        uint64_t salt = cpu.ijc_salt[0];
        static const uint64_t vas[3] = {0x100004010ull, 0x100004ffcull, 0x100008010ull}; /* page P twice, then page Q */
        uint64_t *ents[3];
        static HvBlock fake[3];
        for (int i = 0; i < 3; i++) {
            fake[i] = (HvBlock){.va = vas[i], .flags = 0, .code = (uint32_t *)(uintptr_t)0x1234};
            hv_ijc_fill(&cpu, &fake[i]);
            ents[i] = m.percpu[0].ijc + (size_t)hv_ijc_index(vas[i] ^ salt) * 2;
        }
        hv_tlb_flush_va_asid(&cpu, 0x100004000ull, 7);
        int want[3] = {0, 0, 1};
        for (int i = 0; i < 3; i++) {
            checks++;
            if ((ents[i][0] != 0) != want[i]) {
                printf("  FAIL branch cache after TLBI of page P: entry %d %s\n", i, ents[i][0] ? "kept" : "dropped");
                failures++;
            }
        }
        /* Another address space's TLBI of page Q leaves this one's entry alone. */
        hv_tlb_flush_va_asid(&cpu, 0x100008000ull, 9);
        checks++;
        if (!ents[2][0]) {
            printf("  FAIL another ASID's TLBI dropped our entry\n");
            failures++;
        }
        hv_tlb_flush_va_asid(&cpu, 0x100008000ull, -1); /* all address spaces */
        checks++;
        uint64_t now = vas[2] ^ cpu.ijc_salt[0]; /* what a lookup would compare against now */
        if (m.percpu[0].ijc[(size_t)hv_ijc_index(now) * 2] == now) {
            printf("  FAIL an all-ASID TLBI kept the entry\n");
            failures++;
        }
    }

    /* Windows 11 ARM64 TCR.A1 == 1 process switch (ASID in TTBR1[63:48], TTBR0[63:48] == 0):
       KiSwapProcess switches TTBR1 -> (0, KPA_SYS), TTBR0 -> PA_B, TTBR1 -> (ASID_B, KPA_B).
       Verify:
       1. Switching back to (ASID_A, PA_A) restores Process A's stashed user TLB entry.
       2. Reusing freed PA_A for a new process ASID_C != ASID_A does NOT restore dead Process A's entry.
       3. Transient ASID 0 switches with no user IJC fills do not churn next_cid. */
    {
        hv_cpu_reset(&cpu, &m);
        cpu.el = 1;
        cpu.tcr_el1 = (1ull << 22) | (1ull << 36); /* A1 = 1, AS = 1 (16-bit ASID in TTBR1) */
        cpu.ttbr0_el1 = 0x40010000ull;             /* PA_A */
        cpu.ttbr1_el1 = (11ull << 48) | 0x40020000ull; /* ASID 11, KPA_A */
        hv_ijc_switch(&cpu);

        /* Simulate a user TLB fill in Process A (ASID 11, PA_A). */
        uint64_t uva = 0x7ff700001000ull;
        uint32_t sl = (uva >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
        HvTlbEntry *ue = hv_tlb_ent(&cpu, sl, 0);
        hv_tlb_note(&cpu, (uint32_t)(ue - cpu.tlb));
        ue->read_tag = uva - HV_TLB_ADJ;
        ue->addend = 0x1234000;
        cpu.user_fills[0] = (uint16_t)hv_tlb_code(sl, 0);
        cpu.nuser_fills = 1;

        uint32_t cid_before = cpu.next_cid;

        /* KiSwapProcess A -> B (ASID 22, PA_B = 0x40030000). */
        cpu.x[0] = 0x40000000ull; /* ASID 0, KPA_SYS */
        hv_sysreg_msr(&cpu, 0xd5182020u, 0); /* msr ttbr1_el1, x0 */
        cpu.x[0] = 0x40030000ull; /* PA_B */
        hv_sysreg_msr(&cpu, 0xd5182000u, 0); /* msr ttbr0_el1, x0 */
        cpu.x[0] = (22ull << 48) | 0x40040000ull; /* ASID 22, KPA_B */
        hv_sysreg_msr(&cpu, 0xd5182020u, 0); /* msr ttbr1_el1, x0 */

        checks++;
        if (hv_tlb_ent(&cpu, sl, 0)->read_tag != HV_TLB_INVALID) {
            printf("  FAIL TCR.A1 switch A->B kept Process A's user TLB entry\n");
            failures++;
        }

        /* Switch B -> A: Process A's user TLB entry should be restored from stash. */
        cpu.x[0] = 0x40000000ull;
        hv_sysreg_msr(&cpu, 0xd5182020u, 0);
        cpu.x[0] = 0x40010000ull; /* PA_A */
        hv_sysreg_msr(&cpu, 0xd5182000u, 0);
        cpu.x[0] = (11ull << 48) | 0x40020000ull; /* ASID 11, KPA_A */
        hv_sysreg_msr(&cpu, 0xd5182020u, 0);

        checks++;
        if (hv_tlb_ent(&cpu, sl, 0)->read_tag != uva - HV_TLB_ADJ) {
            printf("  FAIL TCR.A1 switch B->A did not restore Process A's user TLB entry\n");
            failures++;
        }

        /* Now Process A exits and PA_A (0x40010000) is reused by Process C (ASID 33) without TLBI ASIDE1:
           Process C must NOT inherit Process A's stashed user TLB entry! */
        cpu.x[0] = 0x40000000ull;
        hv_sysreg_msr(&cpu, 0xd5182020u, 0);
        cpu.x[0] = 0x40010000ull; /* same PA_A! */
        hv_sysreg_msr(&cpu, 0xd5182000u, 0);
        cpu.x[0] = (33ull << 48) | 0x40020000ull; /* new ASID 33, even same KPA_A! */
        hv_sysreg_msr(&cpu, 0xd5182020u, 0);

        checks++;
        if (hv_tlb_ent(&cpu, sl, 0)->read_tag != HV_TLB_INVALID) {
            printf("  FAIL TCR.A1 reused PA_A under new ASID 33 leaked dead Process A's user TLB entry\n");
            failures++;
        }

        /* Transient ASID 0 switches with no user IJC fills must allocate at most 1 ASID-0 cid. */
        uint32_t cid_mid = cpu.next_cid;
        for (int iter = 0; iter < 100; iter++) {
            cpu.x[0] = 0x40000000ull;
            hv_sysreg_msr(&cpu, 0xd5182020u, 0);
            cpu.x[0] = 0x40050000ull + ((uint64_t)(iter & 3) << 16);
            hv_sysreg_msr(&cpu, 0xd5182000u, 0);
            cpu.x[0] = (22ull << 48) | 0x40040000ull;
            hv_sysreg_msr(&cpu, 0xd5182020u, 0);
        }
        checks++;
        if (cpu.next_cid != cid_mid || cid_mid - cid_before > 4) {
            printf("  FAIL transient ASID 0 switches churned next_cid (%u -> %u -> %u)\n",
                   cid_before, cid_mid, cpu.next_cid);
            failures++;
        }
    }

    /* A TLBI of a kernel address has to find what is cached for it wherever it is: branch targets in the other
       pages of a large mapping, the return table of a CPU it is done for from outside, the pages of a range that
       ends where the address space does. */
    {
#define CHECK(what, cond)                        \
    do {                                         \
        checks++;                                \
        if (!(cond)) {                           \
            printf("  FAIL kernel TLBI: %s\n", what); \
            failures++;                          \
        }                                        \
    } while (0)
#define SLOT(va, table) hv_tlb_ent(&cpu, (uint32_t)((va) >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), table)
        const uint64_t kg = 0xffffffc000100000ull, kn = 0xffffffc000200000ull; /* a code page, a data page */
        const uint64_t pg = PT_PA + 0xe0000, pn = PT_PA + 0xe1000;
        hv_cpu_reset(&cpu, &m);
        cpu.el = 1;
        pt_next = 1 * 65536;
        uint64_t kt = alloc_table(), got;
        cpu.mair_el1 = 0x00ff;
        cpu.tcr_el1 = 25 | 25u << 16 | 1u << 22 | 2u << 30 | 5ull << 32 | 1ull << 36; /* 4K, 39 bits each, the ASID in TTBR1 */
        cpu.sctlr_el1 |= 1;
        cpu.ttbr1_el1 = (22ull << 48) | kt;
        hv_ijc_reset(&cpu, 1);
        map(kt, 12, 39, kg, 3, pg | PAGE | AF, 0);
        map(kt, 12, 39, kn, 3, pn | PAGE | AF | PXN, 0);
        /* In a 2 MB mapping: the targets in every page of it, also where the TLB entry is gone. */
        const uint64_t blk = 0xffffffc000400000ull, far = blk + 0x1000 + ((uint64_t)HV_TLB_SIZE << HV_TLB_PAGE_BITS);
        map(kt, 12, 39, blk, 2, PT_PA | BLOCK | AF, 0);
        map(kt, 12, 39, far, 3, pg | PAGE | AF, 0); /* lands in the same TLB slot as blk + 0x1000 */
        CHECK("code in a 2 MB mapping fetches", hv_mmu_access(&cpu, blk + 0x1000, HV_ACC_EXEC, 1, &got) == 0 && got == PT_PA + 0x1000);
        static HvBlock in_block;
        in_block = (HvBlock){.va = blk + 0x1010, .flags = HV_BLOCK_EL1, .code = (uint32_t *)(uintptr_t)0x1234};
        hv_ijc_fill(&cpu, &in_block);
        uint64_t tag = in_block.va ^ cpu.ijc_salt[1];
        uint64_t *cached = m.percpu[0].ijc + (size_t)(HV_BLOCK_EL1 & (HV_IJC_TABLES - 1)) * (HV_IJC_TABLE_BYTES / 8) + (size_t)hv_ijc_index(tag) * 2;
        CHECK("its branch target is cached", cached[0] == tag);
        hv_mmu_access(&cpu, far, HV_ACC_READ, 1, &got); /* evicts the block's TLB entry */
        CHECK("the 2 MB mapping's TLB entry is evicted", SLOT(blk + 0x1000, 1)->exec_tag == HV_TLB_INVALID || SLOT(blk + 0x1000, 1)->read_tag == far - HV_TLB_ADJ);
        hv_tlb_flush_va_asid(&cpu, blk, -1); /* the guest invalidates the mapping by its first page */
        tag = in_block.va ^ cpu.ijc_salt[1];
        cached = m.percpu[0].ijc + (size_t)(HV_BLOCK_EL1 & (HV_IJC_TABLES - 1)) * (HV_IJC_TABLE_BYTES / 8) + (size_t)hv_ijc_index(tag) * 2;
        CHECK("TLBI of a 2 MB mapping forgets the targets in its other pages", cached[0] != tag && cached[2] != tag);
        /* A big range that starts inside a 2 MB mapping takes the whole mapping (range TLBI: xnu's, not Windows'). */
        hv_mmu_access(&cpu, blk + 0x1000, HV_ACC_READ, 1, &got);
        CHECK("(the page is cached again)", SLOT(blk + 0x1000, 1)->read_tag == blk + 0x1000 - HV_TLB_ADJ);
        hv_tlb_flush_kernel_range(&cpu, blk + 0x100000, 32ull << 20);
        CHECK("a range from the middle of a 2 MB mapping drops the pages before it", SLOT(blk + 0x1000, 1)->read_tag == HV_TLB_INVALID);
        /* Another CPU's TLBI of a kernel code page does it here itself: the entry goes, and because code could be
           fetched through it, so does the kernel's return table (it has to notice before it clears the entry). */
        hv_tlb_flush(&cpu);
        CHECK("global code fetches", hv_mmu_access(&cpu, kg, HV_ACC_EXEC, 1, &got) == 0 && got == pg);
        uint64_t before = cpu.ret_salt[1];
        CHECK("a remote TLBI is done from the other CPU", hv_tlbi_drop_remote_va(&cpu, kg, -1) == 1);
        CHECK("it drops the entry", SLOT(kg, 1)->exec_tag == HV_TLB_INVALID && SLOT(kg, 1)->read_tag == HV_TLB_INVALID);
        CHECK("and renews the kernel's return table", cpu.ret_salt[1] != before);
        hv_mmu_access(&cpu, kn, HV_ACC_READ, 1, &got); /* data (PXN): no return can lead into it */
        before = cpu.ret_salt[1];
        hv_tlbi_drop_remote_va(&cpu, kn, -1);
        CHECK("a remote TLBI of a data page leaves the return table", cpu.ret_salt[1] == before && SLOT(kn, 1)->read_tag == HV_TLB_INVALID);
        /* A range that ends where the address space does, 16 pages and then a whole 1 GB mapping. */
        const uint64_t top = 0xffffffffc0000000ull, last = 0xfffffffffffff000ull;
        map(kt, 12, 39, last, 3, pg | PAGE | AF, 0);
        CHECK("the last page reads", hv_mmu_access(&cpu, last, HV_ACC_READ, 1, &got) == 0 && got == pg);
        CHECK("and is in the TLB", SLOT(last, 1)->read_tag == last - HV_TLB_ADJ);
        hv_tlb_flush_kernel_range(&cpu, 0xffffffffffff0000ull, 0x10000);
        CHECK("a range of pages that ends at the top drops them", SLOT(last, 1)->read_tag == HV_TLB_INVALID);
        hv_tlb_flush(&cpu);
        map(kt, 12, 39, top, 1, PT_PA | BLOCK | AF, 0); /* the level 1 entry becomes a block */
        CHECK("a page of the last 1 GB mapping reads", hv_mmu_access(&cpu, top + 0x1000, HV_ACC_READ, 1, &got) == 0 && got == PT_PA + 0x1000);
        CHECK("and is in the TLB", SLOT(top + 0x1000, 1)->read_tag == top + 0x1000 - HV_TLB_ADJ);
        hv_tlb_flush_va_asid(&cpu, top, -1);
        CHECK("TLBI of the last 1 GB mapping drops its other pages", SLOT(top + 0x1000, 1)->read_tag == HV_TLB_INVALID);
#undef SLOT
#undef CHECK
    }

    /* Windows writes TTBR1 twice on every process switch: each process has its own top-level kernel table and,
       with TCR.A1, its ASID there. The kernel's global mappings stay in the TLB through that, with the branch
       targets cached from them, as on hardware; what belongs to the ASID goes, and the kernel's return table
       is renewed as it always was. A TLBI removes either kind. */
    {
#define CHECK(what, cond)                          \
    do {                                           \
        checks++;                                  \
        if (!(cond)) {                             \
            printf("  FAIL kernel switch: %s\n", what); \
            failures++;                            \
        }                                          \
    } while (0)
#define MSR_TTBR1(v) (cpu.x[0] = (v), hv_sysreg_msr(&cpu, 0xd5182020u, 0))
#define MSR_TTBR0(v) (cpu.x[0] = (v), hv_sysreg_msr(&cpu, 0xd5182000u, 0))
#define SLOT(va, table) hv_tlb_ent(&cpu, (uint32_t)((va) >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), table)
        const uint64_t NG = 1ull << 11, TCR_A1 = 25 | 25u << 16 | 1u << 22 | 2u << 30 | 5ull << 32 | 1ull << 36;
        const uint64_t kg = 0xffffffc000100000ull, kn = 0xffffffc000200000ull, kx = 0xffffffc000300000ull;
        const uint64_t pg = PT_PA + 0xe0000, pn_a = PT_PA + 0xe1000, pn_b = PT_PA + 0xe2000, px = PT_PA + 0xe3000;
        hv_cpu_reset(&cpu, &m);
        cpu.el = 1;
        pt_next = 1 * 65536;
        uint64_t ka = alloc_table(), kb = alloc_table(); /* two processes' kernel tables */
        cpu.mair_el1 = 0x00ff;
        cpu.tcr_el1 = TCR_A1; /* 4K, 39 bits each, the ASID in TTBR1, 16 bits of it */
        cpu.sctlr_el1 |= 1;
        for (int t = 0; t < 2; t++) {
            uint64_t root = t ? kb : ka;
            map(root, 12, 39, kg, 3, pg | PAGE | AF, 0);                          /* global, the same in both */
            map(root, 12, 39, kn, 3, (t ? pn_b : pn_a) | PAGE | AF | NG | PXN, 0); /* each process's own data */
            map(root, 12, 39, kx, 3, px | PAGE | AF | NG, 0);                     /* its own, and code can run from it */
        }
        HvTlbEntry *eg = SLOT(kg, 1), *en = SLOT(kn, 1);
        uint64_t got;
        /* Process A's tables, set the way the guest does. */
        MSR_TTBR1((11ull << 48) | ka);
        MSR_TTBR0(0x40010000ull);
        CHECK("global page reads", hv_mmu_access(&cpu, kg, HV_ACC_READ, 1, &got) == 0 && got == pg);
        CHECK("process A's page reads", hv_mmu_access(&cpu, kn, HV_ACC_READ, 1, &got) == 0 && got == pn_a);
        CHECK("both are in the TLB", eg->read_tag == kg - HV_TLB_ADJ && en->read_tag == kn - HV_TLB_ADJ);
        uint64_t ksalt = cpu.ijc_salt[1], rsalt = cpu.ret_salt[1];
        /* To process B as KiSwapProcess goes: TTBR1 to (ASID 0, B's table), TTBR0, TTBR1 to (B's ASID, B's table). */
        MSR_TTBR1(kb);
        MSR_TTBR0(0x40050000ull);
        MSR_TTBR1((22ull << 48) | kb);
        CHECK("the global page stays through a process switch", eg->read_tag == kg - HV_TLB_ADJ);
        CHECK("the other process's page is gone", en->read_tag == HV_TLB_INVALID);
        CHECK("cached kernel branch targets are kept", cpu.ijc_salt[1] == ksalt);
        CHECK("the kernel's return table is renewed", cpu.ret_salt[1] != rsalt);
        CHECK("process B reads its own page", hv_mmu_access(&cpu, kn, HV_ACC_READ, 1, &got) == 0 && got == pn_b);
        CHECK("and the TLB holds B's", en->read_tag == kn - HV_TLB_ADJ &&
                                           en->addend == (int64_t)((uint64_t)(uintptr_t)(pt + 0xe2000) - kn));
        /* With TCR.A1 the top bits of TTBR0 are not the ASID: writing them ends nothing in the kernel half. */
        MSR_TTBR0((77ull << 48) | 0x40050000ull);
        CHECK("TTBR0's top bits are not the ASID here", en->read_tag == kn - HV_TLB_ADJ && eg->read_tag == kg - HV_TLB_ADJ);
        /* Many switches later the global page is still there. */
        for (int i = 0; i < 100; i++) {
            MSR_TTBR1(i & 1 ? kb : ka);
            MSR_TTBR1(((uint64_t)(i & 1 ? 22 : 11) << 48) | (i & 1 ? kb : ka));
        }
        CHECK("the global page stays through a hundred switches", eg->read_tag == kg - HV_TLB_ADJ && cpu.ijc_salt[1] == ksalt);
        /* TLBI by ASID is not for global pages; TLBI by address is. */
        hv_mmu_access(&cpu, kn, HV_ACC_READ, 1, &got);
        cpu.x[0] = 22ull << 48;
        hv_sysreg_sys(&cpu, 0xd5088740u, 0); /* tlbi aside1, x0 */
        CHECK("TLBI by ASID keeps the global page", eg->read_tag == kg - HV_TLB_ADJ);
        CHECK("TLBI by ASID drops the ASID's kernel page", en->read_tag == HV_TLB_INVALID);
        CHECK("TLBI by ASID keeps the kernel branch targets", cpu.ijc_salt[1] == ksalt);
        cpu.x[0] = (kg >> 12) & 0xfffffffffffull;
        hv_sysreg_sys(&cpu, 0xd5088760u, 0); /* tlbi vaae1, x0 */
        CHECK("TLBI by address drops the global page", eg->read_tag == HV_TLB_INVALID);
        /* Code that EL1 can run from a kernel page of the ASID: its cached branch targets must not outlive the ASID. */
        CHECK("the ASID's code page fetches", hv_mmu_access(&cpu, kx, HV_ACC_EXEC, 1, &got) == 0 && got == px);
        cpu.x[0] = 22ull << 48;
        hv_sysreg_sys(&cpu, 0xd5088740u, 0);
        CHECK("TLBI by ASID after the ASID's code ran: the kernel branch targets go", cpu.ijc_salt[1] != ksalt);
        ksalt = cpu.ijc_salt[1];
        hv_mmu_access(&cpu, kx, HV_ACC_EXEC, 1, &got);
        MSR_TTBR1(kb);
        CHECK("a switch after the ASID's code ran: the kernel branch targets go", cpu.ijc_salt[1] != ksalt);
        ksalt = cpu.ijc_salt[1];
        MSR_TTBR1((11ull << 48) | ka);
        CHECK("and only once", cpu.ijc_salt[1] == ksalt);
        /* Under ASID 0 (the middle of every switch) a page of one table must not outlive the table. */
        hv_tlb_flush(&cpu);
        MSR_TTBR1(ka);
        hv_mmu_access(&cpu, kg, HV_ACC_READ, 1, &got);
        hv_mmu_access(&cpu, kn, HV_ACC_READ, 1, &got);
        MSR_TTBR1(kb);
        CHECK("same ASID, another table: the table's own page goes", en->read_tag == HV_TLB_INVALID);
        CHECK("same ASID, another table: the global page stays", eg->read_tag == kg - HV_TLB_ADJ);
        /* Without TCR.A1 the ASID is in TTBR0: a TTBR0 write ends it, and a TTBR1 write still changes tables. */
        cpu.tcr_el1 &= ~(1ull << 22);
        hv_tlb_flush(&cpu);
        cpu.ttbr0_el1 = (5ull << 48) | 0x40010000ull;
        cpu.ttbr1_el1 = ka;
        hv_mmu_access(&cpu, kg, HV_ACC_READ, 1, &got);
        hv_mmu_access(&cpu, kn, HV_ACC_READ, 1, &got);
        MSR_TTBR0((6ull << 48) | 0x40050000ull);
        CHECK("ASID in TTBR0: its kernel page goes on a TTBR0 write", en->read_tag == HV_TLB_INVALID);
        CHECK("ASID in TTBR0: the global page stays", eg->read_tag == kg - HV_TLB_ADJ);
        hv_mmu_access(&cpu, kn, HV_ACC_READ, 1, &got);
        MSR_TTBR1(kb);
        CHECK("ASID in TTBR0: a TTBR1 write drops the old table's own page", en->read_tag == HV_TLB_INVALID);
        CHECK("ASID in TTBR0: and keeps the global page", eg->read_tag == kg - HV_TLB_ADJ);
        /* No kernel table before, or none after: nothing cached under the other is kept. */
        cpu.tcr_el1 = TCR_A1;
        hv_tlb_flush(&cpu);
        cpu.ttbr1_el1 = (11ull << 48) | ka;
        hv_mmu_access(&cpu, kg, HV_ACC_READ, 1, &got);
        MSR_TTBR1(0);
        CHECK("no kernel table any more: the global page goes too", eg->read_tag == HV_TLB_INVALID);
        cpu.ttbr1_el1 = (11ull << 48) | ka;
        hv_mmu_access(&cpu, kg, HV_ACC_READ, 1, &got);
        cpu.ttbr1_el1 = 0; /* as if the entry were left from before any table */
        MSR_TTBR1((22ull << 48) | kb);
        CHECK("the first kernel table: nothing older is kept", eg->read_tag == HV_TLB_INVALID);

        /* HV_KTLB_CHECK walks the kept kernel entries again at a switch. One counts when two checks running
           find it wrong the same way, and is told apart by whether the table it was filled from still maps it. */
#define SWITCH_TO(asid, root) (MSR_TTBR1(root), MSR_TTBR1(((uint64_t)(asid) << 48) | (root)))
#define COUNTED(why) (hv_ktlb_check_count(why, 0) + hv_ktlb_check_count(why, 1))
        hv_ktlb_check_set(1);
        hv_tlb_flush(&cpu);
        cpu.ttbr1_el1 = (11ull << 48) | ka;
        hv_mmu_access(&cpu, kg, HV_ACC_WRITE, 1, &got);
        SWITCH_TO(22, kb);
        SWITCH_TO(11, ka);
        uint64_t all = 0;
        for (int w = 1; w <= 7; w++) all += COUNTED(w);
        CHECK("check: an entry the tables agree with is not counted", eg->read_tag == kg - HV_TLB_ADJ && all == 0);
        /* taken away in every table with no TLBI (Windows flushes such addresses later, before using them again) */
        map(ka, 12, 39, kg, 3, 0, 0);
        map(kb, 12, 39, kg, 3, 0, 0);
        MSR_TTBR1(kb);
        CHECK("check: wrong once is not counted yet", COUNTED(1) == 0);
        MSR_TTBR1((22ull << 48) | kb);
        CHECK("check: unmapped everywhere counts as gone at its source", hv_ktlb_check_count(1, 1) > 0 && hv_ktlb_check_count(1, 0) == 0);
        /* another page there now: what would make keeping the entry wrong */
        map(ka, 12, 39, kg, 3, pn_a | PAGE | AF, 0);
        map(kb, 12, 39, kg, 3, pn_a | PAGE | AF, 0);
        SWITCH_TO(22, kb);
        SWITCH_TO(11, ka);
        SWITCH_TO(22, kb);
        CHECK("check: another page under the address counts", COUNTED(2) > 0);
        /* read-only now while the entry lets writes through */
        map(ka, 12, 39, kg, 3, pg | PAGE | AF | AP(2), 0);
        map(kb, 12, 39, kg, 3, pg | PAGE | AF | AP(2), 0);
        SWITCH_TO(11, ka);
        SWITCH_TO(22, kb);
        SWITCH_TO(11, ka);
        CHECK("check: a writable entry for a read-only page counts", COUNTED(3) > 0);
        /* a global mapping that only one table has: the claim that global means the same everywhere fails */
        hv_tlb_flush(&cpu);
        map(ka, 12, 39, kg, 3, pg | PAGE | AF, 0);
        map(kb, 12, 39, kg, 3, 0, 0);
        hv_mmu_access(&cpu, kg, HV_ACC_READ, 1, &got); /* filled under A's table */
        uint64_t before = hv_ktlb_check_count(1, 0);
        SWITCH_TO(22, kb);
        MSR_TTBR1((23ull << 48) | kb); /* a second check with B's table still there */
        CHECK("check: mapped by the table it came from, not by this one", hv_ktlb_check_count(1, 0) > before);
        CHECK("check: nothing is repaired", eg->read_tag == kg - HV_TLB_ADJ);
        hv_ktlb_check_set(0);
        map(kb, 12, 39, kg, 3, pg | PAGE | AF, 0);
        hv_tlb_flush(&cpu);
#undef COUNTED
#undef SWITCH_TO

#undef SLOT
#undef MSR_TTBR0
#undef MSR_TTBR1
#undef CHECK
    }

    /* A full flush empties the TLB and the tables beside the branch cache by what was noted as written since
       they were last empty (4.3 MiB in all, and firmware flushes a quarter of a million times in a boot). So
       it must leave nothing behind however the entries got there, and must not rewrite what nothing touched. */
    {
#define BIG_PA 0xc0000000ull
#define BIG_SIZE (64u << 20)
#define CHECK(what, cond)                          \
    do {                                           \
        checks++;                                  \
        if (!(cond)) {                             \
            printf("  FAIL flush: %s\n", what);    \
            failures++;                            \
        }                                          \
    } while (0)
#define MSR_TTBR1(v) (cpu.x[0] = (v), hv_sysreg_msr(&cpu, 0xd5182020u, 0))
#define MSR_TTBR0(v) (cpu.x[0] = (v), hv_sysreg_msr(&cpu, 0xd5182000u, 0))
#define SWITCH_TO(asid, uroot) (MSR_TTBR1(kr), MSR_TTBR0(uroot), MSR_TTBR1(((uint64_t)(asid) << 48) | kr))
        const uint64_t NG = 1ull << 11, TCR_A1 = 25 | 25u << 16 | 1u << 22 | 2u << 30 | 5ull << 32 | 1ull << 36;
        const uint64_t UV = 0x4000000000ull, KG = 0xffffffc000000000ull, KN = 0xffffffc040000000ull;
        HvPerCpu *p = &m.percpu[0];
        uint8_t *big = aligned_alloc(65536, BIG_SIZE);
        hv_machine_add_ram(&m, BIG_PA, BIG_SIZE, big, 0);
        hv_cpu_reset(&cpu, &m);
        hv_knob[HV_K_FLUSH_CHECK] = 0; /* this part looks for itself, and plants what the engine's look would end the run on */
        size_t left;
#define COUNT(table, bytes, empty)                                   \
    do {                                                             \
        left = 0;                                                    \
        for (size_t i_ = 0; i_ < (bytes) / 8; i_++) left += ((const uint64_t *)(table))[i_] != (empty); \
    } while (0)
#define ALL_EMPTY(when)                                                                              \
    do {                                                                                             \
        COUNT(cpu.tlb, sizeof cpu.tlb, HV_TLB_INVALID);                                              \
        CHECK(when ": the TLB is empty", left == 0);                                                 \
        COUNT(cpu.tlb_dirty, sizeof cpu.tlb_dirty, 0);                                               \
        CHECK(when ": no piece of it is noted", left == 0);                                          \
        COUNT(p->ijc_hint, sizeof p->ijc_hint, 0);                                                   \
        CHECK(when ": the branch-cache hints are empty", left == 0);                                 \
        COUNT(p->ijc_hint_dirty, sizeof p->ijc_hint_dirty, 0);                                       \
        CHECK(when ": none of them is noted", left == 0);                                            \
        COUNT(p->ijc_pages, sizeof p->ijc_pages, 0);                                                 \
        CHECK(when ": the branch-cache page maps are empty", left == 0);                             \
        COUNT(p->ijc_pages_dirty, sizeof p->ijc_pages_dirty, 0);                                     \
        CHECK(when ": none of their words is noted", left == 0);                                     \
        COUNT(p->ucode, (size_t)8 << HV_UCODE_BITS, 0);                                              \
        CHECK(when ": the set of user code pages is empty", left == 0 && p->ucode_count == 0);       \
        COUNT(cpu.asid_cid, sizeof cpu.asid_cid, 0);                                                 \
        CHECK(when ": only the current ASID has a user id", left <= 1);                              \
    } while (0)
        ALL_EMPTY("after a reset (the struct was zeros, which are not invalid tags)");

        cpu.el = 1;
        pt_next = 1 * 65536;
        uint64_t ur = alloc_table(), ur2 = alloc_table(), kr = alloc_table();
        cpu.mair_el1 = 0x00ff;
        cpu.tcr_el1 = TCR_A1; /* 4K, 39 bits each, the ASID in TTBR1, 16 bits of it */
        cpu.sctlr_el1 |= 1;
        map(ur, 12, 39, UV, 1, BIG_PA | BLOCK | AF | AP(1) | NG, 0);  /* a process's 1 GB, on the 64 MiB of RAM */
        map(ur2, 12, 39, UV, 1, BIG_PA | BLOCK | AF | AP(1) | NG, 0); /* another process's */
        map(kr, 12, 39, KG, 1, BIG_PA | BLOCK | AF, 0);               /* the kernel's, global */
        map(kr, 12, 39, KN, 1, BIG_PA | BLOCK | AF | NG, 0);          /* the kernel's, per process */
        SWITCH_TO(11, ur);
        uint64_t got;
        int filled = 0;
        /* More user pages than the list of user fills holds, more per-process kernel pages than theirs holds,
           global kernel pages (listed nowhere), every kind of access, and the table for PAN. */
        for (uint32_t i = 0; i < 3000; i++)
            filled += hv_mmu_access(&cpu, UV + ((uint64_t)i << 12), i % 3 == 0 ? HV_ACC_WRITE : i % 3 == 1 ? HV_ACC_READ : HV_ACC_EXEC, 0, &got) == 0;
        for (uint32_t i = 0; i < 5000; i++)
            filled += hv_mmu_access(&cpu, KG + ((uint64_t)i << 12), i & 1 ? HV_ACC_WRITE : HV_ACC_EXEC, 1, &got) == 0;
        for (uint32_t i = 0; i < 4200; i++)
            filled += hv_mmu_access(&cpu, KN + ((uint64_t)(8000 + i) << 12), HV_ACC_READ, 1, &got) == 0;
        cpu.pstate_extra |= HV_PSTATE_PAN;
        for (uint32_t i = 0; i < 700; i++)
            filled += hv_mmu_access(&cpu, KG + ((uint64_t)(13000 + i) << 12), HV_ACC_WRITE, 1, &got) == 0;
        cpu.pstate_extra &= ~(uint64_t)HV_PSTATE_PAN;
        CHECK("every access above filled an entry", filled == 3000 + 5000 + 4200 + 700);
        CHECK("the lists of fills overflowed, as meant", cpu.nuser_fills > HV_USER_FILLS && cpu.nkernel_fills > HV_KERNEL_FILLS);
        COUNT(cpu.tlb, sizeof cpu.tlb, HV_TLB_INVALID);
        CHECK("the TLB holds them", left > 12000);
        left = 0;
        for (uint32_t sl = 0; sl < HV_TLB_SIZE; sl++) left += hv_tlb_ent(&cpu, sl, 2)->write_tag != HV_TLB_INVALID;
        CHECK("700 of them in the table for PAN", left == 700);
        /* Branch targets cached for user and kernel code in many pages, under more ASIDs than are noted singly. */
        static HvBlock blocks[900];
        for (int i = 0; i < 40; i++) {
            SWITCH_TO(100 + i, i & 1 ? ur2 : ur);
            for (int k = 0; k < 10; k++) {
                HvBlock *b = &blocks[i * 20 + k];
                *b = (HvBlock){.va = UV + ((uint64_t)(i * 37 + k * 211) << 12) + 0x40, .flags = 0, .code = (uint32_t *)(uintptr_t)0x1234};
                hv_ijc_fill(&cpu, b);
                b = &blocks[i * 20 + 10 + k];
                *b = (HvBlock){.va = KG + ((uint64_t)(i * 53 + k * 97) << 12) + 0x80, .flags = HV_BLOCK_EL1, .code = (uint32_t *)(uintptr_t)0x1234};
                hv_ijc_fill(&cpu, b);
            }
        }
        COUNT(p->ijc_hint, sizeof p->ijc_hint, 0);
        CHECK("hints were written for both halves", left > 400);
        COUNT(p->ijc_pages, sizeof p->ijc_pages, 0);
        CHECK("the page map was written", left > 0);
        CHECK("user code pages were recorded", p->ucode_count > 300);
        left = 0;
        for (uint32_t a = 0; a < 65536; a++) left += cpu.asid_cid[a] != 0;
        CHECK("more ASIDs have user ids than are noted singly", left > HV_ASID_NOTES && cpu.nasid_notes > HV_ASID_NOTES);
        hv_tlb_flush(&cpu);
        ALL_EMPTY("after a flush of all that");

        /* Entries that came back from a process's stash are noted like fresh ones. */
        SWITCH_TO(11, ur);
        for (uint32_t i = 0; i < 500; i++) hv_mmu_access(&cpu, UV + ((uint64_t)(1000 + i) << 12), HV_ACC_WRITE, 0, &got);
        SWITCH_TO(22, ur2);
        COUNT(cpu.tlb, sizeof cpu.tlb, HV_TLB_INVALID);
        CHECK("the first process's entries left with it", left == 0);
        /* What notes they had goes too, as a flush with nothing else to do takes them: only the restore's own are left. */
        memset(cpu.tlb_dirty, 0, sizeof cpu.tlb_dirty);
        SWITCH_TO(11, ur);
        COUNT(cpu.tlb, sizeof cpu.tlb, HV_TLB_INVALID);
        CHECK("and came back from the stash", left >= 500);
        hv_tlb_flush(&cpu);
        ALL_EMPTY("after a flush of restored entries");

        /* Few fills, as between two of firmware's flushes: the same result, and what nothing noted is not
           rewritten. The marks below are in places nothing was written to since the last flush. */
        SWITCH_TO(11, ur);
        hv_mmu_access(&cpu, KG + 0x5000, HV_ACC_EXEC, 1, &got);
        hv_mmu_access(&cpu, UV + 0x9000, HV_ACC_WRITE, 0, &got);
        blocks[0] = (HvBlock){.va = KG + 0x5040, .flags = HV_BLOCK_EL1, .code = (uint32_t *)(uintptr_t)0x1234};
        hv_ijc_fill(&cpu, &blocks[0]);
        HvTlbEntry *far_entry = &cpu.tlb[HV_TLB_SIZE * 3 + 12345]; /* in this layout, in the table nothing uses */
        far_entry->addend = 0x5a5a;
        p->ijc_hint[0][77].key = 0x77;
        p->ijc_pages[1][9] = 0x99;
        cpu.asid_cid[4000] = 9;
        uint32_t had = p->ucode_count;
        p->ucode_count = 0; /* as if nothing had been recorded: the set is then left alone */
        uint64_t *recorded = NULL;
        for (size_t i = 0; i < (size_t)1 << HV_UCODE_BITS && !recorded; i++)
            if (p->ucode[i]) recorded = &p->ucode[i];
        if (!recorded) recorded = &p->ucode[5], *recorded = 0x55;
        hv_tlb_flush(&cpu);
        CHECK("a flush leaves a TLB piece nothing noted as it was", far_entry->addend == 0x5a5a);
        CHECK("and a hint nothing noted", p->ijc_hint[0][77].key == 0x77);
        CHECK("and a page-map word nothing noted", p->ijc_pages[1][9] == 0x99);
        CHECK("and an ASID's user id nothing noted", cpu.asid_cid[4000] == 9);
        CHECK("and the user code pages when none was counted", *recorded != 0);
        CHECK("while what was filled is gone", SLOT_OF(KG + 0x5000, 1)->exec_tag == HV_TLB_INVALID && SLOT_OF(UV + 0x9000, 0)->write_tag == HV_TLB_INVALID);
        (void)had;
        /* HV_FLUSH_WHOLE clears every table whole, whatever was noted: the marks go. */
        hv_knob[HV_K_FLUSH_WHOLE] = 1;
        p->ucode_count = 1;
        hv_tlb_flush(&cpu);
        hv_knob[HV_K_FLUSH_WHOLE] = 0;
        CHECK("with HV_FLUSH_WHOLE the marks are cleared too", far_entry->addend == (int64_t)HV_TLB_INVALID && p->ijc_hint[0][77].key == 0 &&
                                                              p->ijc_pages[1][9] == 0 && cpu.asid_cid[4000] == 0 && *recorded == 0);
        ALL_EMPTY("after a whole flush");

        /* As many ASIDs as are noted singly: a flush clears those and nothing else. One more: all of asid_cid. */
#define ASIDS_WITH_IDS()                                                         \
    do {                                                                         \
        left = 0;                                                                \
        for (uint32_t a_ = 0; a_ < 65536; a_++) left += cpu.asid_cid[a_] != 0;   \
    } while (0)
        for (int more = 0; more < 2; more++) {
            hv_tlb_flush(&cpu);
            for (int a = 500; cpu.nasid_notes < HV_ASID_NOTES + (uint32_t)more; a++) SWITCH_TO(a, ur);
            CHECK(more ? "one ASID more than is noted singly" : "exactly as many ASIDs as are noted singly", cpu.nasid_notes == HV_ASID_NOTES + (uint32_t)more);
            cpu.asid_cid[60000] = 7; /* nothing noted this one */
            hv_tlb_flush(&cpu);
            ASIDS_WITH_IDS();
            if (!more) CHECK("the noted ones are cleared, and what nothing noted is left", left == 2 && cpu.asid_cid[60000] == 7);
            else CHECK("with more than fit, all of it is cleared", left == 1 && cpu.asid_cid[60000] == 0);
            cpu.asid_cid[60000] = 0;
        }
        /* The user ids run out while an ASID is given one: everything is emptied first, and that ASID is noted after. */
        hv_tlb_flush(&cpu);
        cpu.next_cid = 0x10000;
        SWITCH_TO(300, ur);
        CHECK("the ids started over, with ASID 0 (passed through on the way) and this one given the first two", cpu.next_cid == 3 && cpu.asid_cid[0] && cpu.asid_cid[300]);
        SWITCH_TO(900, ur2);
        hv_tlb_flush(&cpu);
        ASIDS_WITH_IDS();
        CHECK("a flush after that clears the ASID that was given an id as they ran out", left == 1 && cpu.asid_cid[300] == 0 && cpu.asid_cid[0] == 0);
        /* The set of user code pages is half full while a branch target is cached: the user half starts over
           inside the fill, which then goes on to record the page. */
        SWITCH_TO(11, ur);
        blocks[1] = (HvBlock){.va = UV + 0x222040, .flags = 0, .code = (uint32_t *)(uintptr_t)0x1234};
        hv_ijc_fill(&cpu, &blocks[1]);
        p->ucode_count = (1u << HV_UCODE_BITS) / 2;
        blocks[2] = (HvBlock){.va = UV + 0x333040, .flags = 0, .code = (uint32_t *)(uintptr_t)0x1234};
        hv_ijc_fill(&cpu, &blocks[2]);
        CHECK("the user half started over inside the fill, and the page was recorded after", p->ucode_count == 1);
        hv_tlb_flush(&cpu);
        ALL_EMPTY("after a flush that follows a restart inside a fill");
        /* The branch cache's filter is an eighth full: both halves start over inside the fill. */
        SWITCH_TO(11, ur);
        blocks[3] = (HvBlock){.va = KG + 0x444080, .flags = HV_BLOCK_EL1, .code = (uint32_t *)(uintptr_t)0x1234};
        hv_ijc_fill(&cpu, &blocks[3]);
        p->ijc_keyed_count = 1u << 17;
        blocks[4] = (HvBlock){.va = UV + 0x555040, .flags = 0, .code = (uint32_t *)(uintptr_t)0x1234};
        hv_ijc_fill(&cpu, &blocks[4]);
        CHECK("the filter started over inside the fill", p->ijc_keyed_count <= 1);
        hv_tlb_flush(&cpu);
        ALL_EMPTY("after a flush that follows both halves starting over inside a fill");
        /* The kernel's generations run out: its half of the cache and its hints are cleared whole, notes and all. */
        blocks[5] = (HvBlock){.va = KG + 0x666080, .flags = HV_BLOCK_EL1, .code = (uint32_t *)(uintptr_t)0x1234};
        hv_ijc_fill(&cpu, &blocks[5]);
        cpu.kernel_gen = 0xfffe;
        hv_tlb_flush(&cpu);
        CHECK("the kernel's generations started over", cpu.kernel_gen == 1);
        ALL_EMPTY("after the kernel's generations ran out");
#undef ASIDS_WITH_IDS
        hv_knob[HV_K_FLUSH_CHECK] = 1;
#undef ALL_EMPTY
#undef COUNT
#undef SWITCH_TO
#undef MSR_TTBR0
#undef MSR_TTBR1
#undef CHECK
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
