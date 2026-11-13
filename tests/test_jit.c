#include "harness.h"

#include <stdlib.h>

#include "memop.h"
#include "mmu.h"

DECLARE_PROGRAM(arith);
DECLARE_PROGRAM(loop);
DECLARE_PROGRAM(condbr);
DECLARE_PROGRAM(csel_ops);
DECLARE_PROGRAM(bl_sub);
DECLARE_PROGRAM(simd_fp);
DECLARE_PROGRAM(stolen);
DECLARE_PROGRAM(pcrel);
DECLARE_PROGRAM(long_block);
DECLARE_PROGRAM(pstate_bits);
DECLARE_PROGRAM(flagm);
DECLARE_PROGRAM(pac);
extern const char jit_programs_begin[], jit_programs_finish[];

static const TestProgram programs[] = {
    TEST_PROGRAM(arith),   TEST_PROGRAM(loop),   TEST_PROGRAM(condbr),
    TEST_PROGRAM(csel_ops), TEST_PROGRAM(bl_sub), TEST_PROGRAM(simd_fp),
    TEST_PROGRAM(stolen),  TEST_PROGRAM(pcrel),  TEST_PROGRAM(long_block), TEST_PROGRAM(pstate_bits),
    TEST_PROGRAM(flagm),
};

static int inval_check(const char *what, uint64_t got, uint64_t want) {
    if (got == want) return 0;
    printf("  invalidation: %s = %llx, expected %llx\n", what, (unsigned long long)got, (unsigned long long)want);
    return 1;
}

static int inval_run(HvCpu *cpu, uint64_t from, uint64_t to, uint64_t value) {
    cpu->pc = from;
    cpu->m->stop_pc = to + 8;
    int bad = inval_check("run exit", hv_run(cpu), HV_EXIT_HALT);
    return bad + inval_check("new code executed", cpu->x[0], value);
}

static uint32_t inval_slot(HvMachine *m, uint64_t pa) {
    for (uint32_t k = 0; k < m->nxchained; k++) {
        uint32_t idx = m->xchained[k];
        if (m->slot_xpa[idx] == (pa | 1)) return idx;
    }
    return ~0u;
}

static int test_invalidation(void) {
    enum { PAGE = 1 << HV_TLB_PAGE_BITS, PAGES = 264 };
    const uint64_t base = 0x40000000;
    static HvMachine m;
    static HvCpu cpu[HV_MAX_CPUS];
    uint32_t *code = aligned_alloc(16384, PAGES * PAGE);
    uint32_t *replacement = malloc(4 * PAGE);
    if (!code || !replacement || hv_machine_init(&m, 4 << 20)) return 1;
    memset(code, 0, PAGES * PAGE);
    hv_machine_add_ram(&m, base, PAGES * PAGE, code, 0);
    /* A second region also has to lose its flags at a full flush. */
    uint32_t other[2] = {0xd28000e0, 0x14000001}; /* mov x0, #7; b .+4 */
    hv_machine_add_ram(&m, base + 0x200000, sizeof other, other, 0);
    for (int i = 0; i < HV_MAX_CPUS; i++) hv_cpu_reset_index(&cpu[i], &m, i);
    m.stats_interval = 1;
    m.gen_checks = 1;
    const unsigned targets[] = {0, 2, 3, 4};
    uint32_t slots[4];
    int bad = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned target = targets[i], source = 5 + i;
        code[target * PAGE / 4] = 0xd2800000u | (i + 1) << 5; /* mov x0, #value */
        code[target * PAGE / 4 + 1] = 0x14000001;
        code[source * PAGE / 4] = 0x14000000u | (((target - source) * (PAGE / 4)) & 0x3ffffff);
        bad += inval_run(&cpu[0], base + source * PAGE, base + target * PAGE, i + 1);
        bad += inval_run(&cpu[0], base + source * PAGE, base + target * PAGE, i + 1);
        slots[i] = inval_slot(&m, base + target * PAGE);
        bad += inval_check("cross-page chain installed", slots[i] != ~0u, 1);
    }
    if (bad) goto done;
    HvRam *ram = &m.ram[0];
    uint64_t survivor = m.slots[slots[3]];
    uint32_t survivor_insn = *m.slot_site[slots[3]];
    /* Exercise the exact set even with bookkeeping for an unflagged page. */
    uint32_t unflagged = m.nslots++;
    m.slots[unflagged] = 0x12345678;
    m.slot_xpa[unflagged] = (base + PAGE) | 1;
    m.xchained[m.nxchained++] = unflagged;
    memcpy(replacement, code, 4 * PAGE);
    replacement[0] = 0xd2800140; /* mov x0, #10 */
    replacement[2 * PAGE / 4] = 0xd2800160; /* mov x0, #11 */
    replacement[3 * PAGE / 4] = 0xd2800180; /* mov x0, #12 */
    uint64_t seq[HV_MAX_CPUS];
    for (int i = 0; i < HV_MAX_CPUS; i++) seq[i] = m.percpu[i].req_seq;
    HvStats before = m.stats;
    uint32_t scanned = m.nxchained;
    bad += inval_check("physical DMA write", hv_phys_write(&m, base, replacement, 4 * PAGE), 0);
    bad += inval_check("DMA pages", m.stats.flush_smc - before.flush_smc, 3);
    bad += inval_check("DMA calls", m.stats.inval_dev - before.inval_dev, 1);
    bad += inval_check("DMA scans", m.stats.inval_scanned - before.inval_scanned, scanned);
    bad += inval_check("DMA resets", m.stats.reset_why[4] - before.reset_why[4], 1);
    bad += inval_check("DMA kicks", m.stats.kick_src[2] - before.kick_src[2], 1);
    for (int i = 0; i < HV_MAX_CPUS; i++) {
        bad += inval_check("one remote reset request", m.percpu[i].req_seq - seq[i], 1);
        bad += inval_check("remote reset posted", m.percpu[i].req_flags & HV_REQ_IJC_RESET, HV_REQ_IJC_RESET);
    }
    for (unsigned pg = 0; pg < PAGES; pg++)
        bad += inval_check("exact page generations", ram->page_gen[pg], pg == 0 || pg == 2 || pg == 3);
    for (unsigned i = 0; i < 3; i++) {
        uint32_t idx = slots[i];
        bad += inval_check("DMA code flag cleared", ram->code_pages[targets[i]], 0);
        bad += inval_check("DMA chain target cleared", m.slot_xpa[idx], 0);
        bad += inval_check("DMA chain slot cleared", m.slots[idx], 0);
        uint32_t stub = 0x14000000u | ((m.slot_stub[idx] - m.slot_site[idx]) & 0x3ffffff);
        bad += inval_check("DMA chain reverted to stub", *m.slot_site[idx], stub);
    }
    bad += inval_check("unflagged page chain target", m.slot_xpa[unflagged], (base + PAGE) | 1);
    bad += inval_check("unflagged page chain slot", m.slots[unflagged], 0x12345678);
    bad += inval_check("unrelated chain target", m.slot_xpa[slots[3]], (base + 4 * PAGE) | 1);
    bad += inval_check("unrelated chain slot", m.slots[slots[3]], survivor);
    bad += inval_check("unrelated chain instruction", *m.slot_site[slots[3]], survivor_insn);
    before = m.stats;
    hv_dma_written(&m, base + PAGE, PAGE);
    bad += inval_check("non-code range leaves caches alone", m.stats.reset_why[4], before.reset_why[4]);
    bad += inval_run(&cpu[0], base + 5 * PAGE, base, 10);
    bad += inval_run(&cpu[0], base + 6 * PAGE, base + 2 * PAGE, 11);
    bad += inval_run(&cpu[0], base + 7 * PAGE, base + 3 * PAGE, 12);
    before = m.stats;
    bad += inval_run(&cpu[0], base + 8 * PAGE, base + 4 * PAGE, 4);
    bad += inval_check("unrelated block reused", m.stats.translations, before.translations);

    /* Direct DMA uses the same batch, including the local CPU's two cache halves. */
    code[0] = 0xd2800180; /* mov x0, #12 */
    code[2 * PAGE / 4] = 0xd28001a0; /* mov x0, #13 */
    code[3 * PAGE / 4] = 0xd28001c0; /* mov x0, #14 */
    before = m.stats;
    for (int i = 0; i < HV_MAX_CPUS; i++) seq[i] = m.percpu[i].req_seq;
    hv_self = &cpu[0];
    hv_dev_lock(&m);
    hv_dma_written(&m, base + 1, 4 * PAGE - 1);
    hv_dev_unlock(&m);
    hv_self = NULL;
    bad += inval_check("direct DMA pages", m.stats.flush_smc - before.flush_smc, 3);
    bad += inval_check("direct DMA calls", m.stats.inval_dev - before.inval_dev, 1);
    bad += inval_check("local cache halves reset", m.stats.ijc_resets - before.ijc_resets, 2);
    bad += inval_check("local reset not posted", m.percpu[0].req_seq, seq[0]);
    for (int i = 1; i < HV_MAX_CPUS; i++)
        bad += inval_check("direct DMA remote reset", m.percpu[i].req_seq - seq[i], 1);
    bad += inval_run(&cpu[0], base + 5 * PAGE, base, 12);
    bad += inval_run(&cpu[0], base + 6 * PAGE, base + 2 * PAGE, 13);
    bad += inval_run(&cpu[0], base + 7 * PAGE, base + 3 * PAGE, 14);
    bad += inval_run(&cpu[0], base + 0x200000, base + 0x200000, 7);

    uint32_t gens[PAGES];
    memcpy(gens, ram->page_gen, sizeof gens);
    hv_flush_blocks(&m);
    bad += inval_check("full flush dropped blocks", m.nblocks, 0);
    for (int r = 0; r < m.nram; r++)
        for (size_t pg = 0; pg < (size_t)1 << m.ram[r].pages_log2; pg++)
            bad += inval_check("full flush cleared code flags", m.ram[r].code_pages[pg], 0);
    bad += inval_check("full flush preserved generations", memcmp(gens, ram->page_gen, sizeof gens), 0);
    bad += inval_check("old write tag stays conservative", hv_tlb_ent(&cpu[0], (base >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), 1)->write_tag, HV_TLB_INVALID);
    m.gen_checks = 1;
    /* Refill writable entries while no code exists, including another CPU's hint. */
    for (int i = 0; i < HV_MAX_CPUS; i++) {
        uint64_t pa;
        hv_tlb_flush(&cpu[i]);
        bad += inval_check("writable fill", hv_mmu_access(&cpu[i], base, HV_ACC_WRITE, cpu[i].el, &pa), 0);
        bad += inval_check("write tag installed", hv_tlb_ent(&cpu[i], (base >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), 1)->write_tag != HV_TLB_INVALID, 1);
    }
    before = m.stats;
    bad += inval_run(&cpu[0], base, base, 12);
    bad += inval_check("translation protects again", m.stats.code_protects - before.code_protects, 1);
    for (int i = 0; i < HV_MAX_CPUS; i++)
        bad += inval_check("write tag revoked again", hv_tlb_ent(&cpu[i], (base >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), 1)->write_tag, HV_TLB_INVALID);
    uint32_t changed = 0xd28001c0; /* mov x0, #14 */
    bad += inval_check("store after retranslation", hv_vmem_xfer(&cpu[0], base, &changed, 4, 1, cpu[0].el, base), 0);
    bad += inval_check("store invalidated retranslation", ram->page_gen[0], gens[0] + 1);
    bad += inval_run(&cpu[0], base, base, 14);

    /* The bitmap spans several bytes, with a code page at each end. */
    code[260 * PAGE / 4] = 0xd28002a0; /* mov x0, #21 */
    code[260 * PAGE / 4 + 1] = 0x14000001;
    bad += inval_run(&cpu[0], base + 260 * PAGE, base + 260 * PAGE, 21);
    code[0] = 0xd28002c0; /* mov x0, #22 */
    code[260 * PAGE / 4] = 0xd28002e0; /* mov x0, #23 */
    before = m.stats;
    hv_dma_written(&m, base, 261 * PAGE);
    bad += inval_check("large DMA pages", m.stats.flush_smc - before.flush_smc, 2);
    bad += inval_check("large DMA reset", m.stats.reset_why[4] - before.reset_why[4], 1);
    bad += inval_check("large DMA last page generation", ram->page_gen[260], 1);
    bad += inval_run(&cpu[0], base, base, 22);
    bad += inval_run(&cpu[0], base + 260 * PAGE, base + 260 * PAGE, 23);
done:
    printf("DMA and full-flush invalidation: %s\n", bad ? "FAIL" : "ok");
    hv_machine_free(&m);
    free(replacement);
    free(code);
    return bad != 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    static HvMachine m;
    if (hv_machine_init(&m, 4 << 20) != 0) {
        printf("machine init failed\n");
        return 1;
    }
    /* The programs' own text is guest RAM at PA = host address, so ADR and
       pointer values agree with the native run. */
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)jit_programs_begin, jit_programs_finish - jit_programs_begin,
                       (void *)jit_programs_begin, 1);
    uint8_t *stack = aligned_alloc(16384, 65536);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)stack, 65536, stack, 0);
    uint64_t sp = (uint64_t)(uintptr_t)stack + 32768;

    m.trace = getenv("HV_TRACE") != NULL;
    int failed = 0;
    size_t count = sizeof programs / sizeof programs[0];
    for (size_t i = 0; i < count; i++) {
        const TestProgram *p = &programs[i];
        /* Two seeds per program: one with flags clear, one with NZCV set going in. */
        for (int variant = 0; variant < 2; variant++) {
            static HvCpu init, native, jit;
            memset(&init, 0, sizeof init);
            seed_cpu(&init, i * 2 + variant + 1, sp);
            if (variant) init.nzcv = 0xa0000000;
            run_native(p, &init, &native, NULL);
            int bad = run_jit(&m, p, &init, &jit, NULL);
            if (!bad) bad = compare_cpus(p->name, &native, &jit);
            printf("%-12s seed %d: %s\n", p->name, variant, bad ? "FAIL" : "ok");
            failed += bad != 0;
        }
    }
    /* PAC: compare against the inputs, with canonical pointers in the registers. */
    {
        static HvCpu init, jit;
        static const TestProgram pac_prog = TEST_PROGRAM(pac);
        memset(&init, 0, sizeof init);
        seed_cpu(&init, 99, sp);
        for (int r = 0; r < 31; r++) init.x[r] = r & 1 ? 0xfffffe0007123450ull + r * 8 : 0x100004000ull + r * 8;
        int bad = run_jit(&m, &pac_prog, &init, &jit, NULL);
        HvCpu want = init;
        want.x[4] = 0;
        want.x[9] = init.x[30];  /* RETAB came back here, so x30 was restored */
        want.x[10] = init.x[30];
        if (!bad) bad = compare_cpus("pac", &want, &jit);
        printf("%-12s       : %s\n", "pac", bad ? "FAIL" : "ok");
        failed += bad != 0;
    }
    /* Again with a JIT region so small that it fills (and everything is flushed)
       in the middle of a program, chains pending and all. */
    {
        static HvMachine small;
        if (hv_machine_init(&small, 1024) != 0) return 1;
        hv_machine_add_ram(&small, (uint64_t)(uintptr_t)jit_programs_begin, jit_programs_finish - jit_programs_begin,
                           (void *)jit_programs_begin, 1);
        hv_machine_add_ram(&small, (uint64_t)(uintptr_t)stack, 65536, stack, 0);
        for (size_t i = 0; i < count; i++) {
            static HvCpu init, native, jit;
            memset(&init, 0, sizeof init);
            seed_cpu(&init, i * 2 + 1, sp);
            run_native(&programs[i], &init, &native, NULL);
            int bad = run_jit(&small, &programs[i], &init, &jit, NULL);
            if (!bad) bad = compare_cpus(programs[i].name, &native, &jit);
            if (bad) printf("%-12s with a full JIT region: FAIL\n", programs[i].name);
            failed += bad != 0;
        }
        printf("with a 1 KB JIT region: %llu flushes\n", (unsigned long long)small.stats.block_flushes);
    }
    failed += test_invalidation();
    printf("%zu programs x 2 seeds + invalidation, %d failures, %d blocks translated\n", count, failed, m.nblocks);
    return failed != 0;
}
