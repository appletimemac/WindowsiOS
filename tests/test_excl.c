/* Exclusive loads and stores (LDXR/STXR, LDXP/STXP) between CPUs: core/translate.c translate_excl and
   translate_excl_pair, core/memop.c do_exclusive.

   An exclusive store succeeds only if nothing wrote to the place since this CPU's exclusive load. Comparing the
   value the load saw with what is there now is not that: a value that was changed and changed back compares
   equal. Windows on ARM64 relies on the difference. Its interlocked lists (the kernel's lookaside lists, every
   allocation from them) pop with LDXP {depth, first}, read first->next, and STXP {depth - 1, next}, with no
   sequence number: while one CPU sits between its load and its store, others can pop that entry, pop the next
   and push the first back, and the head reads {depth, first} again. A store that takes that for "nothing
   happened" makes an entry that is in use the head of the list; the list then hands it out twice and takes its
   owner's data for a link. On the phone that ended with every CPU faulting on the same bad link for ever.

   The tests: a cell another CPU touches between one CPU's load and store (changed and changed back, changed by
   an exclusive store, changed by a plain store, left alone, held by a store at the time of the load, and no
   longer where the load read it), in
   every form (pair of 64, pair of 32, 64, 32, 16, 8 bits), in a data page (translated code does it) and in a
   page that holds translated code (the C path does the store); the forms and registers one store can have; and
   Windows' own pop and push, copied from a dump, on four CPUs of which one dawdles between its load and its
   store. The whole of it runs a second time with every exclusive in C (HV_C_EXCL). */
#include <mach-o/dyld.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cpu.h"
#include "dispatch.h"
#include "machine.h"

static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %#llx, expected %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

#define PROGRAM(name) extern const uint32_t name[], name##_end[]
PROGRAM(excl_a_p64);
PROGRAM(excl_b_p64);
PROGRAM(excl_a_p32);
PROGRAM(excl_b_p32);
PROGRAM(excl_a_x);
PROGRAM(excl_b_x);
PROGRAM(excl_a_w);
PROGRAM(excl_b_w);
PROGRAM(excl_a_h);
PROGRAM(excl_b_h);
PROGRAM(excl_a_b);
PROGRAM(excl_b_b);
PROGRAM(excl_forms);
PROGRAM(excl_slist);

enum { PAGE = 1 << HV_TLB_PAGE_BITS, PAGES = 8, DATA = 2 * PAGE, FLAGS = 3 * PAGE };
#define BASE 0x40000000ull

static HvMachine m;
static HvCpu cpu[HV_MAX_CPUS];
static uint8_t *host;
static uint32_t code_words;
static int in_c;     /* this is the run with every exclusive in C */
static int gave_up;  /* a run got stuck: the rest is not tried (each would wait for the watchdog) */

/* A machine with eight pages of RAM: page 0 is code (its first word is where every program ends). */
static int world(void) {
    if (gave_up) return 1;
    host = aligned_alloc(16384, PAGES * PAGE);
    if (!host || hv_machine_init(&m, 16 << 20)) return 1;
    memset(host, 0, PAGES * PAGE);
    hv_machine_add_ram(&m, BASE, PAGES * PAGE, host, 0);
    for (int i = 0; i < HV_MAX_CPUS; i++) hv_cpu_reset_index(&cpu[i], &m, (uint32_t)i);
    m.gen_checks = 1;
    ((uint32_t *)host)[0] = 0x14000000; /* b . */
    m.stop_pc = BASE;
    code_words = 4;
    return 0;
}

static void world_end(void) {
    hv_machine_free(&m);
    free(host);
}

static uint64_t program(const uint32_t *from, const uint32_t *to) {
    uint64_t at = BASE + 4ull * code_words;
    memcpy(host + 4 * code_words, from, (size_t)((const char *)to - (const char *)from));
    code_words += (uint32_t)(to - from) + 4;
    return at;
}

typedef struct {
    HvCpu *cpu;
    int r;
} Run;

static void *run_thread(void *arg) {
    Run *r = arg;
    r->r = hv_run(r->cpu);
    return NULL;
}

static int all_done;
static void *watchdog(void *arg) {
    (void)arg;
    for (int i = 0; i < 2000 && !__atomic_load_n(&all_done, __ATOMIC_ACQUIRE); i++) usleep(10000);
    if (!__atomic_load_n(&all_done, __ATOMIC_ACQUIRE)) { /* stuck for 20 s (a run takes under one): stop them, and the checks fail */
        gave_up = 1;
        for (int i = 0; i < HV_MAX_CPUS; i++) __atomic_store_n(&cpu[i].halted, 1, __ATOMIC_SEQ_CST);
        hv_machine_kick_all(&m);
    }
    return NULL;
}

/* What the test itself does while the CPUs run (NULL: nothing). */
static void (*meanwhile)(void);

/* Runs the first n CPUs to the stop address, each on its own thread. Returns how many did not end there. */
static int run_cpus(int n) {
    Run runs[HV_MAX_CPUS];
    pthread_t t[HV_MAX_CPUS], dog;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    all_done = 0;
    pthread_create(&dog, NULL, watchdog, NULL);
    for (int i = 0; i < n; i++) {
        runs[i] = (Run){&cpu[i], -1};
        cpu[i].x[29] = BASE;
        pthread_create(&t[i], &attr, run_thread, &runs[i]);
    }
    pthread_attr_destroy(&attr);
    if (meanwhile) meanwhile();
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    __atomic_store_n(&all_done, 1, __ATOMIC_RELEASE);
    pthread_join(dog, NULL);
    int bad = 0;
    for (int i = 0; i < n; i++) bad += runs[i].r != HV_EXIT_HALT || cpu[i].pc != BASE;
    return bad;
}

static uint64_t rd(const uint8_t *p, uint32_t bytes) {
    uint64_t v = 0;
    memcpy(&v, p, bytes);
    return v;
}

/* Mode 5's part: once A has loaded, its monitor is made to say the load read another page of host memory (what
   a guest address mapped elsewhere between load and store comes to), and A is let go on. */
static void moved(void) {
    uint64_t *flags = (uint64_t *)(host + FLAGS);
    for (int i = 0; i < 1500000 && !__atomic_load_n(&flags[0], __ATOMIC_ACQUIRE); i++) usleep(10);
    cpu[0].excl_host ^= (uint64_t)PAGE;
    __atomic_store_n(&flags[1], 1, __ATOMIC_RELEASE);
}

/* CPU A loads the cell exclusively, tells B, waits for B, stores, and then tries once more with a load of its
   own. mode 1: B changes the cell and changes it back, with exclusive stores. 2: B changes it with one and leaves
   it. 3: B changes it with a plain store. 0: B leaves it alone. 4: as 0, but the cell's bucket is held (as a
   store that never finishes would hold it) from before A's load. 5: nobody touches the cell, but A's store is no
   longer to the host address its load read (B does not run; see moved). */
static void changed_between(const char *form, const uint32_t *a, const uint32_t *a_end, const uint32_t *b, const uint32_t *b_end,
                            uint32_t esize, int pair, int mode, uint32_t cell_off) {
    char what[200];
    if (world()) {
        check("a machine", 1, 0);
        return;
    }
    static const uint64_t first[2] = {0x1111111111111111ull, 0x2222222222222222ull};
    static const uint64_t other[2] = {0x3333333333333333ull, 0x4444444444444444ull};
    static const uint64_t mine[2] = {0x5555555555555555ull, 0x6666666666666666ull};
    static const uint64_t again[2] = {0x7777777777777777ull, 0x8888888888888888ull};
    uint64_t mask = esize == 8 ? ~0ull : (1ull << (8 * esize)) - 1;
    uint8_t *cell = host + cell_off;
    uint64_t *flags = (uint64_t *)(host + FLAGS);
    memcpy(cell, &first[0], esize);
    if (pair) memcpy(cell + esize, &first[1], esize);
    cpu[0].pc = program(a, a_end);
    cpu[1].pc = program(b, b_end);
    for (int i = 0; i < 2; i++) cpu[i].x[0] = BASE + cell_off, cpu[i].x[10] = BASE + FLAGS;
    cpu[0].x[2] = mine[0], cpu[0].x[3] = mine[1], cpu[0].x[16] = again[0], cpu[0].x[17] = again[1];
    cpu[1].x[6] = other[0], cpu[1].x[7] = other[1], cpu[1].x[12] = first[0], cpu[1].x[13] = first[1];
    cpu[1].x[15] = mode == 4 ? 0 : (uint64_t)mode;
    uint64_t *bucket = hv_excl_bucket(&m, cell);
    if (mode == 4) *bucket = 1;
    const char *where = cell_off < PAGE ? "code page" : "data page";
    static const char *const dids[] = {"left alone", "changed and changed back", "changed", "changed by a plain store", "held by a store",
                                       "mapped elsewhere"};
    const char *did = dids[mode];
#define WHAT(text) snprintf(what, sizeof what, "%s, %s, %s: " text, form, where, did)
    WHAT("the CPUs ran to the end");
    meanwhile = mode == 5 ? moved : NULL;
    check(what, (uint64_t)run_cpus(mode == 5 ? 1 : 2), 0);
    meanwhile = NULL;
    WHAT("the load saw the first value");
    check(what, flags[3], first[0] & mask);
    if (pair) check(what, flags[4], first[1] & mask);
    WHAT("the store's status");
    check(what, flags[2], mode ? 1 : 0);
    const uint64_t *then = mode == 1 || mode == 4 || mode == 5 ? first : mode == 0 ? mine : other;
    WHAT("the cell after it");
    check(what, flags[6], then[0] & mask);
    if (pair) check(what, flags[7], then[1] & mask);
    /* The second try has a load of its own and nobody in its way: it stores, unless the bucket is still held. */
    WHAT("a second try's status");
    check(what, flags[5], mode == 4 ? 1 : 0);
    const uint64_t *last = mode == 4 ? first : again;
    WHAT("the cell in the end");
    check(what, rd(cell, esize), last[0] & mask);
    if (pair) check(what, rd(cell + esize, esize), last[1] & mask);
    /* Two on for every exclusive store that was made, in the bucket hv_excl_bucket names for the cell (the
       translated code works it out by itself): A's first if nobody was in its way, B's one or two, A's second. */
    static const uint64_t stores[] = {2, 3, 2, 1, 0, 1};
    WHAT("the bucket counts the exclusive stores made");
    check(what, *bucket, mode == 4 ? 1 : 2 * stores[mode]);
    /* Which path did the stores: the C one for a page with translated code, and for everything in the second run. */
    if (in_c || cell_off < PAGE) {
        WHAT("the stores were done in C");
        check(what, m.stat_slow_mem != 0, 1);
    }
#undef WHAT
    world_end();
}

#define FORM(text, name, esize, pair) \
    for (int mode = 0; mode < 6; mode++) { \
        changed_between(text, excl_a_##name, excl_a_##name##_end, excl_b_##name, excl_b_##name##_end, esize, pair, mode, DATA + 0x100); \
        changed_between(text, excl_a_##name, excl_a_##name##_end, excl_b_##name, excl_b_##name##_end, esize, pair, mode, 0xf00); \
    }

static void test_changed_between(void) {
    printf("a cell another CPU touches between this CPU's exclusive load and store\n");
    FORM("pair of 64 bits", p64, 8, 1)
    FORM("pair of 32 bits", p32, 4, 1)
    FORM("64 bits", x, 8, 0)
    FORM("32 bits", w, 4, 0)
    FORM("16 bits", h, 2, 0)
    FORM("8 bits", b, 1, 0)
}

static void test_forms(void) {
    printf("one CPU: the forms a store can have\n");
    if (world()) {
        check("a machine", 1, 0);
        return;
    }
    uint64_t *cell = (uint64_t *)(host + DATA + 0x100), *res = (uint64_t *)(host + FLAGS);
    cell[0] = 0xa0a0, cell[1] = 0xa1a1, cell[2] = 0xc0c0;
    cpu[0].pc = program(excl_forms, excl_forms_end);
    cpu[0].x[0] = BASE + DATA + 0x100, cpu[0].x[10] = BASE + FLAGS;
    check("ran to the end", (uint64_t)run_cpus(1), 0);
    check("a pair from two registers, one of them borrowed by the store", res[0], 0);
    check("both zero registers", res[1], 0);
    check("the load before it saw the first pair's low half", res[2], 0x1111);
    check("and its high half", res[3], 0x2222);
    check("from registers kept in memory, status into one", res[4], 0);
    check("after CLREX the store fails", res[5], 1);
    check("a store of another size than the load fails", res[6], 1);
    check("the pair is what the last good store wrote", cell[0], 0x6666);
    check("and its high half", cell[1], 0x7777);
    check("a single", res[7], 0);
    check("a store to another address than the load fails", res[8], 1);
    check("one byte", res[9], 0);
    check("a second store after one load fails", res[10], 1);
    check("and the first had not", res[13], 0);
    check("the two zero registers were stored as zeros: low half", res[11], 0);
    check("and high half", res[12], 0);
    check("the byte went into its place", res[15], 0xab33);
    check("the store before the failed one wrote", res[14], 0xab);
    check("a pair's store after a single's load of the same 8 bytes fails", res[16], 1);
    check("a single's store after a pair's load fails", res[17], 1);
    check("neither wrote: the cell is what the store with its status thrown away left", cell[2], 0x4444);
    world_end();
}

/* Windows' interlocked list on n CPUs. dawdle: how many times CPU 0 goes round an empty loop between its load
   and its store when it pops. Returns the nanoseconds it took. */
static uint64_t slist(const char *what_run, int n, uint64_t rounds, uint64_t dawdle) {
    enum { ENTRIES = 8 };
    char what[160];
    if (world()) {
        check("a machine", 1, 0);
        return 0;
    }
    uint64_t *head = (uint64_t *)(host + DATA + 0x100), *entry = (uint64_t *)(host + DATA + 0x200);
    uint64_t *poison = (uint64_t *)(host + DATA + 0x400), *res = (uint64_t *)(host + FLAGS), *stop = (uint64_t *)(host + FLAGS + 0x800);
    const uint64_t entry_pa = BASE + DATA + 0x200, poison_pa = BASE + DATA + 0x400;
    for (int i = 0; i < ENTRIES; i++) entry[2 * i] = i + 1 < ENTRIES ? entry_pa + 16ull * (uint64_t)(i + 1) : 0, entry[2 * i + 1] = 0;
    head[0] = ENTRIES, head[1] = entry_pa;
    poison[0] = poison_pa, poison[1] = 0xbad;
    uint64_t pc = program(excl_slist, excl_slist_end);
    for (int i = 0; i < n; i++) {
        cpu[i].pc = pc;
        cpu[i].x[0] = BASE + DATA + 0x100;
        cpu[i].x[20] = i == 0 && dawdle ? rounds / 16 + 1 : rounds;
        cpu[i].x[21] = (uint64_t)i + 1;
        cpu[i].x[22] = poison_pa;
        cpu[i].x[23] = i == 0 ? dawdle : 0;
        cpu[i].x[24] = BASE + FLAGS + 32ull * (uint64_t)i;
        cpu[i].x[25] = BASE + FLAGS + 0x800;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int stuck = run_cpus(n);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    snprintf(what, sizeof what, "%s: every CPU ran to the end", what_run);
    check(what, (uint64_t)stuck, 0);
    uint64_t wrong = 0, done = 0, due = 0;
    for (int i = 0; i < n; i++) {
        if (res[4 * i + 1] && !wrong) wrong = res[4 * i + 1] | (uint64_t)(i + 1) << 8; /* what, and which CPU saw it */
        done += res[4 * i];
        due += i == 0 && dawdle ? rounds / 16 + 1 : rounds;
    }
    /* 1: the list handed out an owner's data as an entry. 2: it handed out an entry someone holds. 3: someone
       took the entry a CPU holds. The high byte says which CPU saw it. */
    snprintf(what, sizeof what, "%s: no entry was handed out twice (after %llu rounds)", what_run, (unsigned long long)done);
    check(what, wrong, 0);
    snprintf(what, sizeof what, "%s: every CPU did all its rounds", what_run);
    check(what, done, due);
    (void)stop;
    /* The list afterwards: all eight entries, each once, none held. */
    uint64_t seen = 0, at = head[1], count = 0;
    while (at && count <= ENTRIES) {
        uint64_t k = (at - entry_pa) / 16;
        if (at < entry_pa || k >= ENTRIES || (at & 15) || (seen >> k & 1) || entry[2 * k + 1]) break;
        seen |= 1ull << k;
        at = entry[2 * k];
        count++;
    }
    snprintf(what, sizeof what, "%s: the list holds its eight entries again", what_run);
    check(what, at == 0 ? count : 0xbadull, ENTRIES);
    snprintf(what, sizeof what, "%s: and says so", what_run);
    check(what, head[0], ENTRIES);
    world_end();
    return (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
}

static void test_slist(void) {
    printf("Windows' interlocked list, popped and pushed by several CPUs\n");
    /* The run that broke: three CPUs at full speed while a fourth dawdles between its load and its store. */
    for (int round = 0; round < 3; round++) slist("four CPUs, one slow between load and store", 4, 150000, 300);
    slist("four CPUs at full speed", 4, 400000, 0);
    slist("two CPUs at full speed", 2, 400000, 0);
    uint64_t ns = slist("one CPU", 1, 2000000, 0);
    if (ns) printf("  one CPU alone: %.0f ns for a pop and a push\n", (double)ns / 2000000.0);
}

extern char **environ;

int main(int argc, char **argv) {
    (void)argc;
    setvbuf(stdout, NULL, _IONBF, 0);
    in_c = getenv("HV_C_EXCL") != NULL;
    if (in_c) printf("again, every exclusive in C\n");
    test_forms();
    test_changed_between();
    test_slist();
    if (gave_up) printf("  FAIL a run was stuck for 20 s; what came after it was not run\n"), failures++;
    printf("test_excl%s: %d checks, %d failures\n", in_c ? " (in C)" : "", checks, failures);
    if (!in_c) { /* the same again in a process of its own: the translator reads HV_C_EXCL once */
        char path[4096];
        uint32_t size = sizeof path;
        pid_t pid;
        int status = -1;
        setenv("HV_C_EXCL", "1", 1);
        if (_NSGetExecutablePath(path, &size) != 0 || posix_spawn(&pid, path, NULL, NULL, argv, environ) != 0 ||
            waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("  FAIL the run with every exclusive in C\n");
            failures++;
        }
    }
    return failures != 0;
}
