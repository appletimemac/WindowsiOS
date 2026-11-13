/* host/disk.c on real files. hv_disk_grow: grows a raw image sparse, never shrinks, never touches a larger or
   packed one. A read-only disk's overlay: a page that cannot be read from the file is not made up. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "disk.h"

#define MIB (1ull << 20)

static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llu, expected %llu\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static uint64_t size_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (uint64_t)st.st_size : ~0ull;
}

static void make(const char *path, const void *data, size_t n, uint64_t size) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || pwrite(fd, data, n, 0) != (ssize_t)n || ftruncate(fd, (off_t)size) != 0) {
        perror("make");
        exit(2);
    }
    close(fd);
}

int main(void) {
    char path[] = "/tmp/test_disk_grow.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 2; }
    close(fd);

    uint8_t head[512], back[512];
    memset(head, 0xa5, sizeof head);
    make(path, head, sizeof head, 1 * MIB);

    check("grow 1 MiB -> 4 MiB", (uint64_t)hv_disk_grow(path, 4 * MIB), 1);
    check("size after grow", size_of(path), 4 * MIB);
    struct stat st;
    stat(path, &st);
    check("grown space is a hole", (uint64_t)st.st_blocks * 512 <= 1 * MIB, 1);
    fd = open(path, O_RDONLY);
    check("old data kept", pread(fd, back, sizeof back, 0) == (ssize_t)sizeof back && !memcmp(back, head, sizeof head), 1);
    check("new space reads zero", pread(fd, back, sizeof back, (off_t)(4 * MIB - 512)) == (ssize_t)sizeof back &&
                                      back[0] == 0 && !memcmp(back, back + 1, sizeof back - 1), 1);
    close(fd);

    check("same size is left alone", (uint64_t)hv_disk_grow(path, 4 * MIB), 0);
    check("size unchanged", size_of(path), 4 * MIB);
    check("smaller target never shrinks", (uint64_t)hv_disk_grow(path, 2 * MIB), 0);
    check("size still 4 MiB", size_of(path), 4 * MIB);
    check("target rounds down to a block", (uint64_t)hv_disk_grow(path, 8 * MIB + 100), 1);
    check("size is whole blocks", size_of(path), 8 * MIB);
    check("a larger image is left alone", (uint64_t)hv_disk_grow(path, 6 * MIB), 0);
    check("larger size unchanged", size_of(path), 8 * MIB);

    make(path, "HVSPARSE", 8, 1 * MIB);
    check("packed image refused", (uint64_t)(int64_t)hv_disk_grow(path, 4 * MIB), (uint64_t)(int64_t)-1);
    check("packed image untouched", size_of(path), 1 * MIB);

    check("missing file", (uint64_t)(int64_t)hv_disk_grow("/tmp/test_disk_grow.does.not.exist", 4 * MIB), (uint64_t)(int64_t)-1);
    check("directory", (uint64_t)(int64_t)hv_disk_grow("/tmp", 4 * MIB), (uint64_t)(int64_t)-1);

    /* A read-only disk's overlay page is made from the file on first touch. A file that can no longer be read
       there (cut short since it was opened) must fail the write, not leave zeros where data was. */
    uint8_t page[8192];
    memset(page, 0x5a, sizeof page);
    make(path, page, sizeof page, sizeof page);
    HvDisk ro;
    check("open read-only", (uint64_t)hv_disk_open(&ro, path, 0, 1), 0);
    check("a write into a readable page", (uint64_t)hv_disk_write(&ro, 0, head, 512), 0);
    check("the rest of that page is still the file's", hv_disk_read(&ro, 512, back, 512) == 0 && back[0] == 0x5a && back[511] == 0x5a, 1);
    check("cut the file", (uint64_t)truncate(path, 4096), 0);
    check("a write into a page the file no longer has fails", (uint64_t)(int64_t)hv_disk_write(&ro, 4096, head, 512), (uint64_t)(int64_t)-1);
    check("and so does reading it", (uint64_t)(int64_t)hv_disk_read(&ro, 4096 + 512, back, 512), (uint64_t)(int64_t)-1);
    check("the page made earlier is unharmed", hv_disk_read(&ro, 0, back, 512) == 0 && !memcmp(back, head, 512), 1);
    hv_disk_close(&ro);

    unlink(path);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
