#include "harness.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

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

/* A device whose write asks the CPU to leave, as a timer tick or another CPU's request does. */
static int kick_armed;
static void kick_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    (void)off, (void)val, (void)size;
    if (kick_armed) __atomic_store_n(&((HvMachine *)opaque)->cpus[0]->exit_req, 1, __ATOMIC_SEQ_CST);
}

static uint32_t inval_slot(HvMachine *m, uint64_t pa) {
    HvRam *r = hv_ram_lookup(m, pa);
    for (uint32_t s = r->chain_in[(pa - r->pa) >> HV_TLB_PAGE_BITS]; s; s = m->slot_xnext[s - 1])
        if (m->slot_xpa[s - 1] == (pa | 1)) return s - 1;
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
    {
        /* A chain too far for the stub's B leaves that B where it is, and the thunk follows the filled slot: no trip
           through the dispatcher (icount counts those) between the two blocks. Put the B back to the stub to see. */
        uint32_t idx = slots[0], *site = m.slot_site[idx];
        uint32_t chained = *site, to_stub = 0x14000000u | ((m.slot_stub[idx] - site) & 0x3ffffff);
        hv_jit_write(&m.jit, site, &to_stub, 4);
        hv_jit_finish(&m.jit, site, 1);
        uint64_t n0 = cpu[0].icount;
        bad += inval_run(&cpu[0], base + 5 * PAGE, base + targets[0] * PAGE, 1);
        uint64_t from_stub = cpu[0].icount - n0;
        hv_jit_write(&m.jit, site, &chained, 4);
        hv_jit_finish(&m.jit, site, 1);
        n0 = cpu[0].icount;
        bad += inval_run(&cpu[0], base + 5 * PAGE, base + targets[0] * PAGE, 1);
        bad += inval_check("a filled slot is followed from its stub", from_stub, cpu[0].icount - n0);
        /* But not when the CPU was asked to leave: a block that stores to the device above and then takes the same
           chained exit goes through the dispatcher (the stub's thunk sees exit_req before it follows the slot). */
        const uint64_t dev = 0x20000000;
        hv_machine_add_mmio(&m, dev, 0x1000, &m, NULL, kick_write, "kick");
        code[9 * PAGE / 4] = 0xb9000041; /* str w1, [x2] */
        code[9 * PAGE / 4 + 1] = 0x14000000u | ((uint32_t)(-(5 * (int)PAGE + 4) / 4) & 0x3ffffff); /* b the fourth target (the page that no DMA below touches) */
        cpu[0].x[1] = 0, cpu[0].x[2] = dev;
        for (int twice = 0; twice < 2; twice++) bad += inval_run(&cpu[0], base + 9 * PAGE, base + 4 * PAGE, 4);
        n0 = cpu[0].icount;
        bad += inval_run(&cpu[0], base + 9 * PAGE, base + 4 * PAGE, 4);
        uint64_t calm = cpu[0].icount - n0;
        kick_armed = 1;
        n0 = cpu[0].icount;
        bad += inval_run(&cpu[0], base + 9 * PAGE, base + 4 * PAGE, 4);
        uint64_t kicked = cpu[0].icount - n0;
        kick_armed = 0;
        bad += inval_check("a kick makes a chained exit leave", kicked > calm, 1);
    }
    if (bad) goto done;
    HvRam *ram = &m.ram[0];
    uint64_t survivor = m.slots[slots[3]];
    uint32_t survivor_insn = *m.slot_site[slots[3]];
    /* A chain listed under a page without code (it cannot happen, but its list must stay put). */
    uint32_t unflagged = m.nslots++;
    m.slots[unflagged] = 0x12345678;
    m.slot_xpa[unflagged] = (base + PAGE) | 1;
    m.slot_xnext[unflagged] = ram->chain_in[1];
    ram->chain_in[1] = unflagged + 1;
    m.nxchained++;
    memcpy(replacement, code, 4 * PAGE);
    replacement[0] = 0xd2800140; /* mov x0, #10 */
    replacement[2 * PAGE / 4] = 0xd2800160; /* mov x0, #11 */
    replacement[3 * PAGE / 4] = 0xd2800180; /* mov x0, #12 */
    uint64_t seq[HV_MAX_CPUS];
    for (int i = 0; i < HV_MAX_CPUS; i++) seq[i] = m.percpu[i].req_seq;
    HvStats before = m.stats;
    uint32_t listed = m.nxchained;
    bad += inval_check("physical DMA write", hv_phys_write(&m, base, replacement, 4 * PAGE), 0);
    bad += inval_check("DMA pages", m.stats.flush_smc - before.flush_smc, 3);
    bad += inval_check("DMA calls", m.stats.inval_dev - before.inval_dev, 1);
    bad += inval_check("DMA unchains just its pages' lists", m.stats.inval_scanned - before.inval_scanned, 3);
    bad += inval_check("DMA chain count", listed - m.nxchained, 3);
    for (unsigned i = 0; i < 3; i++) bad += inval_check("DMA page lists emptied", ram->chain_in[targets[i]], 0);
    bad += inval_check("unrelated page list kept", ram->chain_in[4] != 0, 1);
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
    ram->chain_in[1] = m.slot_xnext[unflagged]; /* out again: only code pages have lists, which a flush relies on */
    m.nxchained--;
    bad += inval_check("unrelated chain target", m.slot_xpa[slots[3]], (base + 4 * PAGE) | 1);
    bad += inval_check("unrelated chain slot", m.slots[slots[3]], survivor);
    bad += inval_check("unrelated chain instruction", *m.slot_site[slots[3]], survivor_insn);
    before = m.stats;
    hv_ram_written(&m, base + PAGE, PAGE);
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
    hv_ram_written(&m, base + 1, 4 * PAGE - 1);
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
    hv_ram_written(&m, base, 261 * PAGE);
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

/* Writes to a page holding code drop its blocks only when they cover a translated 32-byte piece. */
static int test_grains(void) {
    enum { PAGE = 1 << HV_TLB_PAGE_BITS, PAGES = 8 };
    const uint64_t base = 0x40000000;
    static HvMachine m;
    static HvCpu cpu[HV_MAX_CPUS];
    uint32_t *ram_host = aligned_alloc(16384, PAGES * PAGE);
    if (!ram_host || hv_machine_init(&m, 4 << 20)) return 1;
    memset(ram_host, 0, PAGES * PAGE);
    hv_machine_add_ram(&m, base, PAGES * PAGE, ram_host, 0);
    for (int i = 0; i < HV_MAX_CPUS; i++) hv_cpu_reset_index(&cpu[i], &m, i);
    m.stats_interval = 1;
    m.gen_checks = 1;
    HvRam *ram = &m.ram[0];
    int bad = 0;
    uint32_t *page0 = ram_host, *page1 = ram_host + PAGE / 4;
    page0[0x100 / 4] = 0xd2800020; /* mov x0, #1 */
    page0[0x104 / 4] = 0x14000001; /* b .+4 */
    bad += inval_run(&cpu[0], base + 0x100, base + 0x100, 1);
    bad += inval_check("translated piece marked", hv_code_grains_hit(ram, 0, 0x100, 0x108), 1);
    bad += inval_check("piece before unmarked", hv_code_grains_hit(ram, 0, 0, 0x100), 0);
    bad += inval_check("pieces after unmarked", hv_code_grains_hit(ram, 0, 0x120, PAGE), 0);

    /* A store elsewhere in the page: slow path, but the block stays. */
    uint32_t gen = ram->page_gen[0], v = 0x11223344;
    HvStats before = m.stats;
    bad += inval_check("store to other piece", hv_vmem_xfer(&cpu[0], base + 0x400, &v, 4, 1, cpu[0].el, base), 0);
    bad += inval_check("other piece keeps generation", ram->page_gen[0], gen);
    bad += inval_check("other piece keeps protection", ram->code_pages[0], HV_CODE_PROTECTED);
    bad += inval_check("other piece counted", m.stats.code_writes - before.code_writes, 1);
    bad += inval_check("other piece not a hit", m.stats.code_write_hits - before.code_write_hits, 0);
    bad += inval_check("other piece no reset", m.stats.reset_why[4] - before.reset_why[4], 0);
    bad += inval_check("other piece written", page0[0x400 / 4], v);
    before = m.stats;
    bad += inval_run(&cpu[0], base + 0x100, base + 0x100, 1);
    bad += inval_check("block reused after miss", m.stats.translations - before.translations, 0);

    /* The same 32-byte piece counts as a hit even off the instructions (the price of the granule). */
    bad += inval_check("store in same piece", hv_vmem_xfer(&cpu[0], base + 0x118, &v, 4, 1, cpu[0].el, base), 0);
    bad += inval_check("same piece drops page", ram->page_gen[0], gen + 1);
    bad += inval_check("same piece clears marks", hv_code_grains_hit(ram, 0, 0, PAGE), 0);
    bad += inval_run(&cpu[0], base + 0x100, base + 0x100, 1);
    uint32_t insn = 0xd2800040; /* mov x0, #2 */
    /* The page is hot now (a write hit its code): a small write over code retires that block, not the page. */
    bad += inval_check("page hot after a hit", ram->code_hot[0], 1);
    before = m.stats;
    bad += inval_check("store over code", hv_vmem_xfer(&cpu[0], base + 0x100, &insn, 4, 1, cpu[0].el, base), 0);
    bad += inval_check("code store keeps the page", ram->page_gen[0], gen + 1);
    bad += inval_check("code store retires the block", m.stats.code_block_kills - before.code_block_kills, 1);
    bad += inval_check("code store resets no cache", m.stats.reset_why[4] - before.reset_why[4], 0);
    bad += inval_run(&cpu[0], base + 0x100, base + 0x100, 2);

    /* Atomics check after their store: LDADD and CAS on another piece, then on the code. */
    page1[0] = 0xb8210062; /* ldadd w1, w2, [x3] */
    page1[1] = 0x14000001;
    page1[0x40 / 4] = 0x88a17c62; /* cas w1, w2, [x3] */
    page1[0x44 / 4] = 0x14000001;
    gen = ram->page_gen[0];
    cpu[0].x[1] = 1, cpu[0].x[3] = base + 0x400;
    page0[0x400 / 4] = 5;
    cpu[0].pc = base + PAGE, m.stop_pc = base + PAGE + 8;
    bad += inval_check("ldadd run", hv_run(&cpu[0]), HV_EXIT_HALT);
    bad += inval_check("ldadd result", page0[0x400 / 4], 6);
    bad += inval_check("ldadd elsewhere keeps page", ram->page_gen[0], gen);
    cpu[0].x[1] = 0x12345678, cpu[0].x[2] = 0, cpu[0].x[3] = base + 0x100; /* compare fails: nothing stored */
    cpu[0].pc = base + PAGE + 0x40, m.stop_pc = base + PAGE + 0x48;
    bad += inval_check("failed cas run", hv_run(&cpu[0]), HV_EXIT_HALT);
    bad += inval_check("failed cas keeps page", ram->page_gen[0], gen);
    cpu[0].x[1] = 0xd2800040, cpu[0].x[2] = 0xd2800060; /* mov x0, #2 -> mov x0, #3 */
    cpu[0].pc = base + PAGE + 0x40, m.stop_pc = base + PAGE + 0x48;
    bad += inval_check("cas run", hv_run(&cpu[0]), HV_EXIT_HALT);
    bad += inval_check("cas over code keeps the page", ram->page_gen[0], gen);
    bad += inval_run(&cpu[0], base + 0x100, base + 0x100, 3);

    /* An exclusive pair: STXR to another piece keeps the page, to the code drops it. */
    page1[0x80 / 4] = 0x885f7c64; /* ldxr w4, [x3] */
    page1[0x84 / 4] = 0x88057c62; /* stxr w5, w2, [x3] */
    page1[0x88 / 4] = 0x14000001;
    gen = ram->page_gen[0];
    cpu[0].x[2] = 9, cpu[0].x[3] = base + 0x400;
    cpu[0].pc = base + PAGE + 0x80, m.stop_pc = base + PAGE + 0x8c;
    bad += inval_check("stxr run", hv_run(&cpu[0]), HV_EXIT_HALT);
    bad += inval_check("stxr stored", (uint64_t)page0[0x400 / 4] << 8 | cpu[0].x[5], 9 << 8);
    bad += inval_check("stxr elsewhere keeps page", ram->page_gen[0], gen);
    cpu[0].x[2] = 0xd2800080, cpu[0].x[3] = base + 0x100; /* mov x0, #4 */
    cpu[0].pc = base + PAGE + 0x80, m.stop_pc = base + PAGE + 0x8c;
    bad += inval_check("stxr code run", hv_run(&cpu[0]), HV_EXIT_HALT);
    bad += inval_check("stxr over code keeps the page", ram->page_gen[0], gen);
    bad += inval_run(&cpu[0], base + 0x100, base + 0x100, 4);

    /* A page written mostly as data gives up its blocks after HV_CODE_MISS_MAX misses, once. */
    gen = ram->page_gen[0];
    before = m.stats;
    for (int i = ram->code_miss[0]; i < HV_CODE_MISS_MAX; i++) /* retired blocks count toward it too */
        bad += inval_check("miss store", hv_vmem_xfer(&cpu[0], base + 0x800 + 4 * (i & 63), &v, 4, 1, cpu[0].el, base), 0);
    bad += inval_check("misses keep page", ram->page_gen[0], gen);
    bad += inval_check("miss store past the limit", hv_vmem_xfer(&cpu[0], base + 0x800, &v, 4, 1, cpu[0].el, base), 0);
    bad += inval_check("limit drops page", ram->page_gen[0], gen + 1);
    bad += inval_check("limit counted as forced", m.stats.code_write_forced - before.code_write_forced, 1);
    bad += inval_check("limit unprotects", ram->code_pages[0], 0);
    bad += inval_check("limit restarts the count", ram->code_miss[0], 0);
    bad += inval_run(&cpu[0], base + 0x100, base + 0x100, 4);
    bad += inval_check("protected again", ram->code_pages[0], HV_CODE_PROTECTED);

    /* Masks across the map's 64-bit words, and at the page's last piece. */
    hv_code_grains_mark(ram, 2, 0x7f0, 0x810);
    bad += inval_check("piece 63 marked", hv_code_grains_hit(ram, 2, 0x7e0, 0x7e4), 1);
    bad += inval_check("piece 64 marked", hv_code_grains_hit(ram, 2, 0x800, 0x801), 1);
    bad += inval_check("piece 62 clear", hv_code_grains_hit(ram, 2, 0x7c0, 0x7e0), 0);
    bad += inval_check("piece 65 clear", hv_code_grains_hit(ram, 2, 0x820, 0x840), 0);
    hv_code_grains_mark(ram, 3, PAGE - 4, PAGE);
    bad += inval_check("last piece marked", hv_code_grains_hit(ram, 3, PAGE - 32, PAGE - 28), 1);
    bad += inval_check("before last piece clear", hv_code_grains_hit(ram, 3, 0, PAGE - 32), 0);

    hv_flush_blocks(&m);
    bad += inval_check("flush clears marks", hv_code_grains_hit(ram, 0, 0, PAGE), 0);
    printf("sub-page code writes: %s\n", bad ? "FAIL" : "ok");
    hv_machine_free(&m);
    free(ram_host);
    return bad != 0;
}

/* On a hot page a small write over code retires only the blocks made from it. Callers chained to the old block
   from another page, from the same page and through the indirect-branch cache all reach the new one. */
static int test_retire(void) {
    enum { PAGE = 1 << HV_TLB_PAGE_BITS, PAGES = 8 };
    const uint64_t base = 0x40000000;
    static HvMachine m;
    static HvCpu cpu[HV_MAX_CPUS];
    uint32_t *host = aligned_alloc(16384, PAGES * PAGE);
    if (!host || hv_machine_init(&m, 4 << 20)) return 1;
    memset(host, 0, PAGES * PAGE);
    hv_machine_add_ram(&m, base, PAGES * PAGE, host, 0);
    for (int i = 0; i < HV_MAX_CPUS; i++) hv_cpu_reset_index(&cpu[i], &m, i);
    m.stats_interval = 1;
    m.gen_checks = 1;
    HvRam *ram = &m.ram[0];
    uint32_t *p0 = host, *p1 = host + PAGE / 4;
    const uint64_t fn = base + 0x100, far = base + PAGE, near = base + 0x400, ind = base + PAGE + 0x40;
    int bad = 0;
    p0[0x100 / 4] = 0xd2800020; /* fn: mov x0, #1 */
    p0[0x104 / 4] = 0xd65f03c0; /* ret */
    p0[0x200 / 4] = 0xd2800540; /* an unrelated function in the same page: mov x0, #42 */
    p0[0x204 / 4] = 0xd65f03c0;
    p1[0] = 0x94000000u | (uint32_t)(((int64_t)fn - (int64_t)far) / 4 & 0x3ffffff); /* far: bl fn (another page) */
    p1[1] = 0x14000001;
    p0[0x400 / 4] = 0x94000000u | (uint32_t)(((int64_t)fn - (int64_t)near) / 4 & 0x3ffffff); /* near: bl fn (same page) */
    p0[0x404 / 4] = 0x14000001;
    p1[0x40 / 4] = 0xd63f0120; /* ind: blr x9 */
    p1[0x44 / 4] = 0x14000001;
    p1[0x80 / 4] = 0x94000000u | (uint32_t)(((int64_t)(base + 0x200) - (int64_t)(far + 0x80)) / 4 & 0x3ffffff); /* bl other */
    p1[0x84 / 4] = 0x14000001;
    cpu[0].x[9] = fn;
#define RUN3(value)                                           \
    do {                                                      \
        for (int twice = 0; twice < 2; twice++) {             \
            bad += inval_run(&cpu[0], far, far, value);       \
            bad += inval_run(&cpu[0], near, near, value);     \
            bad += inval_run(&cpu[0], ind, ind, value);       \
        }                                                     \
    } while (0)
    RUN3(1);
    bad += inval_run(&cpu[0], far + 0x80, far + 0x80, 42);
    /* The first write over the page's code drops it whole, and makes it hot. */
    uint32_t insn = 0xd2800060; /* mov x0, #3 */
    uint32_t gen = ram->page_gen[0];
    bad += inval_check("first hit", hv_vmem_xfer(&cpu[0], fn, &insn, 4, 1, cpu[0].el, base), 0);
    bad += inval_check("first hit drops the page", ram->page_gen[0], gen + 1);
    bad += inval_check("first hit makes it hot", ram->code_hot[0], 1);
    RUN3(3);
    bad += inval_run(&cpu[0], far + 0x80, far + 0x80, 42);
    bad += inval_run(&cpu[0], far + 0x80, far + 0x80, 42);
    bad += inval_check("hot page lists its blocks", ram->block_head[0] != 0, 1);
    /* From here a patch retires just fn's block. */
    for (uint32_t value = 7; value < 10; value++) {
        insn = 0xd2800000u | value << 5;
        HvStats before = m.stats;
        uint64_t resets = m.stats.ijc_resets;
        bad += inval_check("patch", hv_vmem_xfer(&cpu[0], fn, &insn, 4, 1, cpu[0].el, base), 0);
        bad += inval_check("patch keeps the page", ram->page_gen[0], gen + 1);
        bad += inval_check("patch retires one block", m.stats.code_block_kills - before.code_block_kills, 1);
        bad += inval_check("patch drops no page", m.stats.flush_smc - before.flush_smc, 0);
        bad += inval_check("patch resets no branch cache", m.stats.ijc_resets - resets, 0);
        bad += inval_check("patch kicks nobody", m.stats.kick_src[2] - before.kick_src[2], 0);
        before = m.stats;
        RUN3(value);
        bad += inval_check("only the patched function is translated again", m.stats.translations - before.translations, 1);
        bad += inval_run(&cpu[0], far + 0x80, far + 0x80, 42);
        bad += inval_check("the other function's block is reused", m.stats.translations - before.translations, 1);
    }
    /* A long write is code replaced wholesale: the page goes, as before. */
    uint32_t body[4] = {0xd2800160, 0xd65f03c0, 0, 0}; /* mov x0, #11; ret */
    HvStats before = m.stats;
    bad += inval_check("long write", hv_vmem_xfer(&cpu[0], fn, body, 16, 1, cpu[0].el, base), 0);
    bad += inval_check("long write drops the page", m.stats.flush_smc - before.flush_smc, 1);
    bad += inval_check("long write empties the list", ram->block_head[0], 0);
    RUN3(11);
    /* The page's share of retirements, then it is dropped once and starts over. */
    gen = ram->page_gen[0];
    before = m.stats;
    for (int i = ram->code_miss[0]; i < HV_CODE_MISS_MAX; i++) {
        insn = 0xd2800000u | (uint32_t)(20 + (i & 1)) << 5;
        bad += inval_check("patch within the share", hv_vmem_xfer(&cpu[0], fn, &insn, 4, 1, cpu[0].el, base), 0);
        bad += inval_run(&cpu[0], ind, ind, 20 + (i & 1)); /* translated again, so the next patch has a block to retire */
    }
    bad += inval_check("share used without dropping", ram->page_gen[0], gen);
    insn = 0xd28003e0; /* mov x0, #31 */
    bad += inval_check("patch past the share", hv_vmem_xfer(&cpu[0], fn, &insn, 4, 1, cpu[0].el, base), 0);
    bad += inval_check("past the share the page drops", ram->page_gen[0], gen + 1);
    RUN3(31);
    hv_flush_blocks(&m);
    bad += inval_check("flush forgets hot pages", ram->code_hot[0], 0);
    bad += inval_check("flush empties block lists", ram->block_head[0], 0);
    RUN3(31);
    /* A forward chains only to a block of its own physical page: by the time the dispatcher looks the address
       up it may be mapped elsewhere, and the retired block is shared by every address space on the old page. */
    insn = 0xd2800400; /* mov x0, #32 */
    bad += inval_check("hit to make it hot again", hv_vmem_xfer(&cpu[0], fn, &insn, 4, 1, cpu[0].el, base), 0);
    RUN3(32);
    HvBlock *live = NULL;
    for (int i = 0; i < m.nblocks; i++)
        if (m.blocks[i].va == fn && m.blocks[i].gen == ram->page_gen[0]) live = &m.blocks[i];
    bad += inval_check("the function's block has a forward", live && live->fwd, 1);
    if (live && live->fwd) {
        uint32_t slot = live->fwd - 1;
        bad += inval_check("a forward names its physical page", m.slot_xpa[slot], base | 2);
        m.slot_xpa[slot] = (base + 2 * PAGE) | 2; /* as if the block had come from another page */
        insn = 0xd2800420; /* mov x0, #33 */
        bad += inval_check("patch", hv_vmem_xfer(&cpu[0], fn, &insn, 4, 1, cpu[0].el, base), 0);
        RUN3(33);
        bad += inval_check("a forward to another page's block is refused", m.slots[slot], 0);
    }
#undef RUN3
    /* Retiring a block asks no CPU to leave, so what a guest uses to synchronise with new code has to make this
       core fetch again: a guest ISB, and an HVC answered in place, each carry a host isb. */
    uint32_t *p3 = host + 3 * PAGE / 4;
    const uint64_t sync = base + 3 * PAGE;
    p3[0] = 0xd5033fdf;        /* isb */
    p3[1] = 0xd28000e0;        /* mov x0, #7 */
    p3[2] = 0x14000001;
    p3[0x40 / 4] = 0xd2b08000; /* mov x0, #0x84000000 (PSCI_VERSION) */
    p3[0x44 / 4] = 0xd4000002; /* hvc #0 */
    p3[0x48 / 4] = 0x14000001;
    bad += inval_run(&cpu[0], sync, sync + 4, 7);
    bad += inval_run(&cpu[0], sync + 0x40, sync + 0x44, 0x10000);
    for (int which = 0; which < 2; which++) {
        const HvBlock *b = NULL;
        for (int i = 0; i < m.nblocks; i++)
            if (m.blocks[i].va == sync + (which ? 0x40 : 0)) b = &m.blocks[i];
        int isb = 0;
        for (int k = 0; b && k < (which ? 24 : 8); k++) isb |= b->code[k] == 0xd5033fdfu;
        bad += inval_check(which ? "an HVC answered in place synchronises the host" : "a guest ISB is a host isb", isb, 1);
    }
    printf("block retirement on hot pages: %s\n", bad ? "FAIL" : "ok");
    hv_machine_free(&m);
    free(host);
    return bad != 0;
}

/* A write that lands while lookup() is making a block from the same bytes, at the two moments nothing else
   covers: after the words are copied (the check after marking must see it), and after that check on a hot page
   (the page's sequence must stop the publication). */
static struct { int stage, armed; uint64_t pa; uint32_t insn; } race;
static void race_hook(HvMachine *m, int stage) {
    if (!race.armed || stage != race.stage) return;
    race.armed = 0;
    hv_phys_write(m, race.pa, &race.insn, 4);
}

static int test_lookup_races(void) {
    enum { PAGE = 1 << HV_TLB_PAGE_BITS, PAGES = 4 };
    const uint64_t base = 0x40000000, fn = base + 0x100;
    static HvMachine m;
    static HvCpu cpu[HV_MAX_CPUS];
    uint32_t *host = aligned_alloc(16384, PAGES * PAGE);
    if (!host || hv_machine_init(&m, 4 << 20)) return 1;
    memset(host, 0, PAGES * PAGE);
    hv_machine_add_ram(&m, base, PAGES * PAGE, host, 0);
    for (int i = 0; i < HV_MAX_CPUS; i++) hv_cpu_reset_index(&cpu[i], &m, i);
    m.stats_interval = 1;
    m.gen_checks = 1;
    m.lookup_hook = race_hook;
    HvRam *ram = &m.ram[0];
    int bad = 0;
    host[0x100 / 4] = 0xd2800020; /* mov x0, #1 */
    host[0x104 / 4] = 0x14000001;
    /* The first translation of the page: no mark yet, so the write passes unnoticed and the check must catch it. */
    race = (__typeof__(race)){.stage = 0, .armed = 1, .pa = fn, .insn = 0xd28000a0}; /* mov x0, #5 */
    HvStats before = m.stats;
    bad += inval_run(&cpu[0], fn, fn, 5);
    bad += inval_check("write under the first translation: made again", m.stats.code_retranslates - before.code_retranslates, 1);
    bad += inval_check("the hook fired", race.armed, 0);
    /* Make the page hot, then write after a translation's check: its sequence must hold the block back. */
    uint32_t insn = 0xd28000c0; /* mov x0, #6 */
    bad += inval_check("first hit", hv_vmem_xfer(&cpu[0], fn, &insn, 4, 1, cpu[0].el, base), 0);
    bad += inval_check("hot", ram->code_hot[0], 1);
    race = (__typeof__(race)){.stage = 1, .armed = 1, .pa = fn, .insn = 0xd2800100}; /* mov x0, #8 */
    before = m.stats;
    bad += inval_run(&cpu[0], fn, fn, 8);
    bad += inval_check("write before a hot page's publication: made again", m.stats.code_retranslates - before.code_retranslates, 1);
    bad += inval_check("the second hook fired", race.armed, 0);
    /* The same moment on a page that is not hot: the write drops the page, so the block is born stale. Its
       maker runs it once (it had the old words in hand before the write, like a fetched instruction), and never
       again. */
    host[PAGE / 4] = 0xd2800020;
    host[PAGE / 4 + 1] = 0x14000001;
    race = (__typeof__(race)){.stage = 1, .armed = 1, .pa = base + PAGE, .insn = 0xd2800120}; /* mov x0, #9 */
    uint32_t gen1 = ram->page_gen[1];
    bad += inval_run(&cpu[0], base + PAGE, base + PAGE, 1);
    bad += inval_check("the third hook fired", race.armed, 0);
    bad += inval_check("the write dropped the page", ram->page_gen[1], gen1 + 1);
    bad += inval_run(&cpu[0], base + PAGE, base + PAGE, 9);
    m.lookup_hook = NULL;
    printf("writes during translation: %s\n", bad ? "FAIL" : "ok");
    hv_machine_free(&m);
    free(host);
    return bad != 0;
}

/* Two CPUs: one rewrites a function (and data beside it in the same page), then publishes the new value and
   waits for the other to make a round; the other reads the value, synchronizes (ISB) and calls the function, which
   must never be older. Under the old invalidate-then-store order this failed in 5 runs of 6. */
typedef struct { HvCpu *cpu; int r; } StressRun;
static void *stress_thread(void *arg) {
    StressRun *s = arg;
    s->r = hv_run(s->cpu);
    return NULL;
}

static int stress_done;
static HvMachine *stress_m;
static void *stress_watchdog(void *arg) {
    (void)arg;
    for (int i = 0; i < 3000 && !__atomic_load_n(&stress_done, __ATOMIC_ACQUIRE); i++) usleep(10000);
    if (!__atomic_load_n(&stress_done, __ATOMIC_ACQUIRE)) { /* stuck: stop both, and the checks fail */
        for (int i = 0; i < 2; i++) __atomic_store_n(&stress_m->cpus[i]->halted, 1, __ATOMIC_SEQ_CST);
        hv_machine_kick_all(stress_m);
    }
    return NULL;
}

/* paced: the writer waits for a reader round after each write (every write meets a translated function);
   else it runs free (writes land while the reader is translating, publishing and chaining). */
static int smc_stress(int paced) {
    enum { PAGE = 1 << HV_TLB_PAGE_BITS, PAGES = 8 };
    const uint32_t WRITES = paced ? 20000 : 60000;
    const uint64_t base = 0x40000000;
    static HvMachine m;
    static HvCpu cpu[HV_MAX_CPUS];
    uint32_t *host = aligned_alloc(16384, PAGES * PAGE);
    if (!host || hv_machine_init(&m, 16 << 20)) return 1;
    memset(host, 0, PAGES * PAGE);
    hv_machine_add_ram(&m, base, PAGES * PAGE, host, 0);
    for (int i = 0; i < HV_MAX_CPUS; i++) hv_cpu_reset_index(&cpu[i], &m, i);
    m.gen_checks = 1;
    uint32_t *fn = host + PAGE / 4, *rd = host + 2 * PAGE / 4, *wr = host + 3 * PAGE / 4;
    const uint64_t fn_pa = base + PAGE, rd_pa = base + 2 * PAGE, wr_pa = base + 3 * PAGE, flag_pa = base + 4 * PAGE;
    fn[0] = 0x52800000; /* mov w0, #0 */
    fn[1] = 0xd65f03c0; /* ret */
    static const uint32_t reader[] = {
        0x88dffd45, /* 0: ldar w5, [x10]: the value published */
        0xd5033fdf, /* isb */
        0xd63f0160, /* blr x11 */
        0x6b05001f, /* cmp w0, w5 */
        0x540000e3, /* b.lo 44: stale */
        0x9100058c, /* 20: add x12, x12, #1 */
        0xf900054c, /* str x12, [x10, #8]: progress */
        0xf9400944, /* ldr x4, [x10, #16]: done? */
        0xb4ffff04, /* cbz x4, 0 */
        0x14000004, /* b 52 */
        0xd503201f, /* nop */
        0xd280002d, /* 44: mov x13, #1 */
        0x17fffff9, /* b 20 */
    };
    static const uint32_t writer[] = {
        0x531b68c7, /* 0: lsl w7, w6, #5 */
        0x2a0900e7, /* orr w7, w7, w9: mov w0, #w6 */
        0xb9000167, /* str w7, [x11]: over the function's first instruction */
        0xb90001c6, /* str w6, [x14]: data in another piece of the same page */
        0x889ffd46, /* stlr w6, [x10]: publish */
        0xf9400548, /* ldr x8, [x10, #8] */
        0xf9400550, /* 24: ldr x16, [x10, #8]: wait for a reader round */
        0xeb08021f, /* cmp x16, x8 */
        0x54ffffc0, /* b.eq 24 */
        0x110004c6, /* add w6, w6, #1 */
        0x6b0f00df, /* cmp w6, w15 */
        0x54fffea1, /* b.ne 0 */
        0xd2800024, /* mov x4, #1 */
        0xf9000944, /* str x4, [x10, #16]: done */
    };
    static const uint32_t writer_free[] = {
        0x531b68c7, /* 0: lsl w7, w6, #5 */
        0x2a0900e7, /* orr w7, w7, w9 */
        0xb9000167, /* str w7, [x11] */
        0xb90001c6, /* str w6, [x14] */
        0x889ffd46, /* stlr w6, [x10] */
        0x110004c6, /* add w6, w6, #1 */
        0x6b0f00df, /* cmp w6, w15 */
        0x54ffff21, /* b.ne 0 */
        0xd2800024, /* mov x4, #1 */
        0xf9000944, /* str x4, [x10, #16] */
    };
    memcpy(rd, reader, sizeof reader);
    if (paced) memcpy(wr, writer, sizeof writer);
    else memcpy(wr, writer_free, sizeof writer_free);
    cpu[0].pc = rd_pa, cpu[0].x[10] = flag_pa, cpu[0].x[11] = fn_pa, cpu[0].x[12] = 0, cpu[0].x[13] = 0;
    cpu[1].pc = wr_pa, cpu[1].x[6] = 1, cpu[1].x[9] = 0x52800000, cpu[1].x[10] = flag_pa, cpu[1].x[11] = fn_pa;
    cpu[1].x[14] = fn_pa + 0x800, cpu[1].x[15] = WRITES;
    m.stop_pc = rd_pa + 52, m.stop_pc2 = wr_pa + (paced ? sizeof writer : sizeof writer_free);
    StressRun runs[2] = {{&cpu[0], -1}, {&cpu[1], -1}};
    pthread_t t[2], dog;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    stress_done = 0, stress_m = &m;
    pthread_create(&dog, NULL, stress_watchdog, NULL);
    for (int i = 0; i < 2; i++) pthread_create(&t[i], &attr, stress_thread, &runs[i]);
    pthread_attr_destroy(&attr);
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
    __atomic_store_n(&stress_done, 1, __ATOMIC_RELEASE);
    pthread_join(dog, NULL);
    int bad = 0;
    bad += inval_check("stress reader exit", runs[0].r, HV_EXIT_HALT);
    bad += inval_check("stress writer exit", runs[1].r, HV_EXIT_HALT);
    bad += inval_check("stress writer finished", cpu[1].x[6], WRITES);
    bad += inval_check("stress reader at its end", cpu[0].pc, rd_pa + 52);
    bad += inval_check("stress never ran stale code", cpu[0].x[13], 0);
    bad += inval_check("stress data written", fn[0x800 / 4], WRITES - 1);
    printf("two-CPU code rewriting, %s (%llu rounds, %llu writes to code pages, %llu blocks retired, %llu pages dropped, %llu retranslated): %s\n",
           paced ? "paced" : "free", (unsigned long long)cpu[0].x[12], (unsigned long long)m.stats.code_writes,
           (unsigned long long)m.stats.code_block_kills, (unsigned long long)m.stats.flush_smc,
           (unsigned long long)m.stats.code_retranslates, bad ? "FAIL" : "ok");
    hv_machine_free(&m);
    free(host);
    return bad != 0;
}

static int test_smc_stress(void) {
    return smc_stress(1) + smc_stress(0);
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
    failed += test_grains();
    failed += test_retire();
    failed += test_lookup_races();
    failed += test_smc_stress();
    printf("%zu programs x 2 seeds + invalidation, %d failures, %d blocks translated\n", count, failed, m.nblocks);
    return failed != 0;
}
