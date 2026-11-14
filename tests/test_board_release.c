/* devices/winvirt.c: a board that has stopped gives back its flash banks, the NVRAM's descriptor, and the guest's
   RAM when it mapped that itself; RAM that was handed in stays the caller's. Windows started again in one process
   relies on it: without the release every start kept 128 MiB of flash, a descriptor and the RAM of the one before. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cpu.h"
#include "machine.h"
#include "pflash.h"
#include "winvirt.h"

#define MIB (1ull << 20)
#define RAM_BYTES (64 * MIB)

static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llu, expected %llu\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static int open_descriptors(void) {
    int n = 0;
    for (int fd = 0; fd < 256; fd++)
        if (fcntl(fd, F_GETFD) != -1) n++;
    return n;
}

/* msync refuses an address nothing is mapped at. */
static int mapped(const void *p) {
    return msync((void *)p, 4096, MS_ASYNC) == 0;
}

static void make(const char *path, uint8_t fill, size_t size) {
    uint8_t *buf = malloc(size);
    memset(buf, fill, size);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || pwrite(fd, buf, size, 0) != (ssize_t)size) {
        perror("make");
        exit(2);
    }
    close(fd);
    free(buf);
}

static HvMachine m;
static HvCpu cpus[2];
static HvWinVirt b;

static int board(void *ram, const char *code, const char *vars) {
    if (hv_machine_init(&m, 16 * MIB) != 0) {
        printf("  cannot set up a machine\n");
        exit(2);
    }
    memset(cpus, 0, sizeof cpus);
    return hv_winvirt_create(&b, &m, cpus, 1, RAM_BYTES, ram, code, vars, NULL, NULL, NULL, 0);
}

int main(void) {
    char dir[] = "/tmp/test_board_release.XXXXXX";
    if (!mkdtemp(dir)) return 2;
    char code[256], vars[256], ramfile[256];
    snprintf(code, sizeof code, "%s/code.fd", dir);
    snprintf(vars, sizeof vars, "%s/vars.fd", dir);
    snprintf(ramfile, sizeof ramfile, "%s/ram.bin", dir);
    make(code, 0xc0, 1 * MIB);
    make(vars, 0x5a, 256 << 10);
    setenv("HV_RAM_NO_LEDGER", "1", 1); /* plain mappings, which the checks below can ask about */
    unsetenv("HV_RAM_FILE");
    int base = open_descriptors();

    printf("a board that mapped its own RAM\n");
    check("it is created", (uint64_t)board(NULL, code, vars), 0);
    uint8_t *ram = b.ram, *f0 = b.flash0->data, *f1 = b.flash1->data;
    check("its RAM is its own", (uint64_t)b.ram_owned, 1);
    check("the firmware is in the first bank", f0[0], 0xc0);
    check("the NVRAM is in the second", f1[0], 0x5a);
    check("the NVRAM's file is held open", (uint64_t)(open_descriptors() > base), 1);
    ram[RAM_BYTES / 2] = 0x11;
    hv_winvirt_release(&b);
    check("its RAM is unmapped", (uint64_t)mapped(ram), 0);
    check("the first flash bank is unmapped", (uint64_t)mapped(f0), 0);
    check("the second flash bank is unmapped", (uint64_t)mapped(f1), 0);
    check("no descriptor is left", (uint64_t)open_descriptors(), (uint64_t)base);
    check("the board is empty", (uint64_t)(!b.ram && !b.flash0 && !b.flash1 && !b.gpu && !b.dtb), 1);
    hv_winvirt_release(&b); /* nothing to give back: nothing happens */
    hv_machine_free(&m);
    check("a freed machine is empty", (uint64_t)(!m.blocks && !m.slots && m.nram == 0), 1);
    hv_machine_free(&m); /* and can be freed again */

    printf("RAM that was handed in\n");
    uint8_t *mine = mmap(NULL, RAM_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mine == MAP_FAILED) return 2;
    mine[RAM_BYTES / 2] = 0x77;
    check("it is created", (uint64_t)board(mine, code, vars), 0);
    check("it uses that RAM", (uint64_t)(b.ram == mine), 1);
    check("which is not its own", (uint64_t)b.ram_owned, 0);
    f0 = b.flash0->data;
    hv_winvirt_release(&b);
    check("the RAM is still mapped", (uint64_t)mapped(mine), 1);
    check("and still holds what it held", mine[RAM_BYTES / 2], 0x77);
    check("the flash went all the same", (uint64_t)mapped(f0), 0);
    check("no descriptor is left", (uint64_t)open_descriptors(), (uint64_t)base);
    hv_machine_free(&m);
    munmap(mine, RAM_BYTES);

    printf("RAM in a file, released while its pages are still being touched\n");
    setenv("HV_RAM_FILE", ramfile, 1);
    for (int round = 0; round < 3; round++) {
        check("it is created", (uint64_t)board(NULL, code, vars), 0);
        ram = b.ram;
        check("a thread is touching its pages", (uint64_t)b.prefault_started, 1);
        hv_winvirt_release(&b); /* waits for that thread: it would fault on the unmapped RAM */
        check("its RAM is unmapped", (uint64_t)mapped(ram), 0);
        check("no descriptor is left", (uint64_t)open_descriptors(), (uint64_t)base);
        hv_machine_free(&m);
    }
    unsetenv("HV_RAM_FILE");

    printf("a board that could not be finished\n");
    check("no firmware: it is refused", (uint64_t)(board(NULL, "/nonexistent/code.fd", vars) != 0), 1);
    ram = b.ram;
    check("it had mapped its RAM by then", (uint64_t)(ram && mapped(ram)), 1);
    hv_winvirt_release(&b);
    check("which is unmapped", (uint64_t)mapped(ram), 0);
    hv_machine_free(&m);
    check("no NVRAM: it is refused", (uint64_t)(board(NULL, code, "/nonexistent/vars.fd") != 0), 1);
    ram = b.ram;
    f0 = b.flash0 ? b.flash0->data : NULL;
    check("it had loaded the firmware by then", (uint64_t)(f0 && mapped(f0)), 1);
    hv_winvirt_release(&b);
    check("its RAM is unmapped", (uint64_t)mapped(ram), 0);
    check("and the firmware bank", (uint64_t)mapped(f0), 0);
    check("no descriptor is left", (uint64_t)open_descriptors(), (uint64_t)base);
    hv_machine_free(&m);

    unlink(code);
    unlink(vars);
    rmdir(dir);
    printf("test_board_release: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
