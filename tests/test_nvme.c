#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "machine.h"
#include "nvme.h"

#define GPA 0x40000000ull
#define RAM_SIZE (1u << 20)
#define ASQ (GPA + 0x1000)
#define ACQ (GPA + 0x2000)
#define SQ (GPA + 0x3000)
#define CQ (GPA + 0x4000)
#define DATA (GPA + 0x10000)
#define QSIZE 32
#define IMAGE_OFFSET 512u
#define DISK_SIZE (8u << 20)

static HvMachine m;
static HvNvme *nv;
static HvDisk disk;
static uint8_t *ram, *expected;
static uint64_t block_size;
static int checks, failures;
static uint16_t next_cid;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llx, expected %llx\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static uint8_t *host(uint64_t pa) { return ram + (pa - GPA); }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void bar_write(uint64_t off, uint64_t val, unsigned size) {
    nv->pci.bar_write(&nv->pci, 0, off, val, size);
}

static uint16_t submit(int q, uint8_t cmd[64]) {
    HvNvmeSq *sq = &nv->sq[q];
    HvNvmeCq *cq = &nv->cq[q];
    uint16_t tail = (sq->tail + 1) % sq->size;
    uint16_t cid = ++next_cid;
    uint8_t *cqe = host(cq->pa + (uint64_t)cq->tail * 16);
    uint8_t phase = cq->phase;
    memcpy(cmd + 2, &cid, 2);
    memcpy(host(sq->pa + (uint64_t)sq->tail * 64), cmd, 64);
    memset(cqe, 0xcc, 16);
    bar_write(0x1000 + q * 8, tail, 4);
    check("completion CID", rd16(cqe + 12), cid);
    check("completion SQ", rd16(cqe + 10), q);
    check("completion phase", rd16(cqe + 14) & 1, phase);
    uint16_t status = rd16(cqe + 14) >> 1;
    bar_write(0x1004 + q * 8, cq->tail, 4);
    return status;
}

static void driver_init(void) {
    bar_write(0x14, 0, 4);
    bar_write(0x24, (QSIZE - 1) | (QSIZE - 1) << 16, 4);
    bar_write(0x28, ASQ, 8);
    bar_write(0x30, ACQ, 8);
    bar_write(0x14, 1 | 6u << 16 | 4u << 20, 4);
    uint8_t cmd[64] = {0x05};
    wr64(cmd + 24, CQ);
    wr32(cmd + 40, 1 | (QSIZE - 1) << 16);
    wr32(cmd + 44, 1);
    check("create CQ", submit(0, cmd), 0);
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x01;
    wr64(cmd + 24, SQ);
    wr32(cmd + 40, 1 | (QSIZE - 1) << 16);
    wr32(cmd + 44, 1 | 1u << 16);
    check("create SQ", submit(0, cmd), 0);
}

static uint16_t io(uint8_t opc, uint64_t lba, uint32_t nlb, uint32_t flags) {
    uint8_t cmd[64] = {0};
    cmd[0] = opc;
    wr32(cmd + 4, 1);
    wr64(cmd + 24, DATA);
    wr32(cmd + 40, (uint32_t)lba);
    wr32(cmd + 44, (uint32_t)(lba >> 32));
    wr32(cmd + 48, (nlb - 1) | flags);
    return submit(1, cmd);
}

static void range(uint8_t *p, uint64_t lba, uint32_t nlb) {
    wr32(p, 0);
    wr32(p + 4, nlb);
    wr64(p + 8, lba);
}

static uint16_t trim(const uint8_t *ranges, unsigned nr, int ad) {
    uint8_t cmd[64] = {0x09};
    /* Split even the first range across two noncontiguous PRP pages. */
    memcpy(host(DATA + 4088), ranges, 8);
    memcpy(host(DATA + 0x3000), ranges + 8, nr * 16 - 8);
    wr32(cmd + 4, 1);
    wr64(cmd + 24, DATA + 4088);
    wr64(cmd + 32, DATA + 0x3000);
    wr32(cmd + 40, nr - 1);
    wr32(cmd + 44, ad ? 4 : 3);
    return submit(1, cmd);
}

static void fill(void) {
    memset(expected, 0xa5, DISK_SIZE + IMAGE_OFFSET);
    for (uint64_t off = 0; off < DISK_SIZE + IMAGE_OFFSET;) {
        size_t n = DISK_SIZE + IMAGE_OFFSET - off;
        if (n > 65536) n = 65536;
        ssize_t wrote = pwrite(disk.fd, expected + off, n, (off_t)off);
        if (wrote <= 0) { perror("fill"); exit(1); }
        off += (uint64_t)wrote;
    }
    check("sync image", fsync(disk.fd), 0);
}

static void expect_zero(uint64_t off, uint64_t len) {
    memset(expected + IMAGE_OFFSET + off, 0, len);
}

static void check_image(void) {
    uint8_t buf[65536];
    int same = 1;
    for (uint64_t off = 0; off < DISK_SIZE + IMAGE_OFFSET; off += sizeof buf) {
        size_t n = DISK_SIZE + IMAGE_OFFSET - off;
        if (n > sizeof buf) n = sizeof buf;
        if (pread(disk.fd, buf, n, (off_t)off) != (ssize_t)n || memcmp(buf, expected + off, n)) same = 0;
    }
    check("whole image including offset prefix and neighbours", same, 1);
}

static void read_zero(uint64_t off, uint64_t len) {
    int zero = 1;
    while (len) {
        uint32_t n = len < 4096 ? (uint32_t)len : 4096;
        memset(host(DATA), 0xcc, n);
        check("NVMe read", io(0x02, off / 512, n / 512, 0), 0);
        for (uint32_t i = 0; i < n; i++) if (host(DATA)[i]) zero = 0;
        off += n;
        len -= n;
    }
    check("NVMe reads deallocated bytes as zero", zero, 1);
}

static void test_trim(void) {
    uint8_t ranges[4096] = {0};
    struct stat before, after;
    uint64_t trim_n = nv->trim_n, trim_bytes = nv->trim_bytes;
    fill();
    uint64_t off = (1u << 20) - IMAGE_OFFSET, len = 2u << 20;
    range(ranges, off / 512, (uint32_t)(len / 512));
    check("stat before trim", fstat(disk.fd, &before), 0);
    check("aligned deallocate", trim(ranges, 1, 1), 0);
    check("sync trim", fsync(disk.fd), 0);
    check("stat after trim", fstat(disk.fd, &after), 0);
    check("allocated blocks decrease", after.st_blocks < before.st_blocks, 1);
    expect_zero(off, len);
    read_zero(off, len);
    check_image();

    fill();
    off = block_size * 2;
    len = block_size * 3;
    range(ranges, off / 512, (uint32_t)(len / 512));
    check("unaligned deallocate", trim(ranges, 1, 1), 0);
    expect_zero(off, len);
    read_zero(off, len);
    check_image();

    fill();
    range(ranges, 0, 1);
    range(ranges + 16, UINT64_MAX, 0);
    range(ranges + 32, 123, 5);
    check("multiple ranges and zero-length skip", trim(ranges, 3, 1), 0);
    expect_zero(0, 512);
    expect_zero(123 * 512, 5 * 512);
    read_zero(0, 512);
    read_zero(123 * 512, 5 * 512);
    check_image();

    fill();
    range(ranges, 10, 10);
    range(ranges + 16, DISK_SIZE / 512 - 1, 2);
    check("invalid later range rejects entire command", trim(ranges, 2, 1), 0x80);
    check_image();
    range(ranges + 16, UINT64_MAX, 1);
    check("overflowing LBA rejected", trim(ranges, 2, 1), 0x80);
    check_image();
    check("AD=0 hints ignore range bounds", trim(ranges, 2, 0), 0);
    check_image();

    memset(ranges, 0, sizeof ranges);
    range(ranges + 255 * 16, 77, 1);
    check("maximum NR", trim(ranges, 256, 1), 0);
    expect_zero(77 * 512, 512);
    read_zero(77 * 512, 512);
    check_image();
    check("DSM command count", nv->trim_n - trim_n, 4);
    check("DSM byte count", nv->trim_bytes - trim_bytes, (2u << 20) + block_size * 3 + 7 * 512);
}

static void test_zeroes(void) {
    for (int deac = 0; deac < 2; deac++) {
        uint64_t trim_n = nv->trim_n, trim_bytes = nv->trim_bytes;
        fill();
        check("Write Zeroes", io(0x08, 1, 40, deac ? 1u << 25 : 0), 0);
        expect_zero(512, 40 * 512);
        read_zero(512, 40 * 512);
        check_image();
        check("Write Zeroes out of range", io(0x08, DISK_SIZE / 512 - 1, 2, deac ? 1u << 25 : 0), 0x80);
        check("Write Zeroes overflowing LBA", io(0x08, UINT64_MAX, 1, deac ? 1u << 25 : 0), 0x80);
        check_image();
        check("Write Zeroes discard count", nv->trim_n - trim_n, deac);
        check("Write Zeroes discard bytes", nv->trim_bytes - trim_bytes, deac ? 40 * 512 : 0);
    }
}

static void identify(int readonly) {
    uint8_t cmd[64] = {0x06};
    wr32(cmd + 4, 1);
    wr64(cmd + 24, DATA);
    check("Identify Namespace", submit(0, cmd), 0);
    check("NSATTR write protected", host(DATA)[99] & 1, readonly);
}

static void test_readonly(const char *path) {
    uint64_t trim_n = nv->trim_n, trim_bytes = nv->trim_bytes;
    fill();
    identify(0);
    hv_disk_close(&disk);
    check("open read-only image", hv_disk_open(&disk, path, IMAGE_OFFSET, 1), 0);
    identify(1);
    check("read-only Write", io(0x01, 0, 1, 0), 0x20);
    check("read-only Write Zeroes", io(0x08, 0, 1, 0), 0x20);
    check("read-only Write Zeroes DEAC", io(0x08, 0, 1, 1u << 25), 0x20);
    uint8_t ranges[16];
    range(ranges, 0, 1);
    check("read-only DSM AD", trim(ranges, 1, 1), 0x20);
    check("read-only DSM hints", trim(ranges, 1, 0), 0);
    memset(host(DATA), 0, 512);
    check("read-only Read", io(0x02, 0, 1, 0), 0);
    check("read-only Read data", memcmp(host(DATA), expected + IMAGE_OFFSET, 512), 0);
    check_image();
    check("read-only discard count unchanged", nv->trim_n, trim_n);
    check("read-only discard bytes unchanged", nv->trim_bytes, trim_bytes);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char path[] = "/tmp/windowsios-nvme-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    if (ftruncate(fd, DISK_SIZE + IMAGE_OFFSET) != 0) return 1;
    close(fd);
    if (hv_disk_open(&disk, path, IMAGE_OFFSET, 0) != 0) return 1;
    struct statfs fs;
    if (fstatfs(disk.fd, &fs) != 0) return 1;
    block_size = fs.f_bsize;
    expected = malloc(DISK_SIZE + IMAGE_OFFSET);
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    ram = aligned_alloc(4096, RAM_SIZE);
    if (!ram || !expected) return 1;
    memset(ram, 0, RAM_SIZE);
    hv_machine_add_ram(&m, GPA, RAM_SIZE, ram, 0);
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x50000000, 0x10000000, 0x20000000, 0x10000000, 35);
    nv = hv_nvme_create(&m, bus, &disk, "TRIMTEST");
    driver_init();
    test_trim();
    test_zeroes();
    test_readonly(path);
    hv_disk_close(&disk);
    unlink(path);
    free(expected);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
