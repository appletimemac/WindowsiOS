#include "../core/machine.h"
#include "disk.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

/* A read-only image still has to take writes: Windows mounts the tools disk read-write and NTFS keeps
   retrying every metadata write the namespace refuses, which stalls the end of every shutdown for
   minutes. The overlay holds those writes in 4 KB pages (an open-addressing table keyed by page
   number) and is capped so a runaway guest cannot eat the host. */
#define OV_PAGE 4096u
#define OV_MAX_PAGES 16384u

typedef struct {
    uint64_t idx;
    uint8_t *data;
} OvSlot;

typedef struct {
    OvSlot *slot;
    uint64_t cap, used;
} Overlay;

static uint8_t *ov_find(const Overlay *ov, uint64_t idx) {
    if (!ov || !ov->used) return NULL;
    for (uint64_t i = (idx * 0x9e3779b97f4a7c15ull) & (ov->cap - 1);; i = (i + 1) & (ov->cap - 1)) {
        if (!ov->slot[i].data) return NULL;
        if (ov->slot[i].idx == idx) return ov->slot[i].data;
    }
}

static void ov_insert(Overlay *ov, uint64_t idx, uint8_t *data) {
    uint64_t i = (idx * 0x9e3779b97f4a7c15ull) & (ov->cap - 1);
    while (ov->slot[i].data) i = (i + 1) & (ov->cap - 1);
    ov->slot[i] = (OvSlot){idx, data};
    ov->used++;
}

/* The page holding `idx`, made from the file's current contents on first touch. NULL when out of memory, capped,
   or the file cannot be read (the write then fails, as it would on a disk). */
static uint8_t *ov_page(HvDisk *d, uint64_t idx) {
    Overlay *ov = d->overlay;
    if (!ov) {
        ov = calloc(1, sizeof *ov);
        if (!ov) return NULL;
        d->overlay = ov;
    }
    uint8_t *pg = ov_find(ov, idx);
    if (pg) return pg;
    if (ov->used >= OV_MAX_PAGES) return NULL;
    if (ov->used * 2 >= ov->cap) {
        Overlay grown = {.cap = ov->cap ? ov->cap * 2 : 256};
        grown.slot = calloc(grown.cap, sizeof *grown.slot);
        if (!grown.slot) return NULL;
        for (uint64_t i = 0; i < ov->cap; i++)
            if (ov->slot[i].data) ov_insert(&grown, ov->slot[i].idx, ov->slot[i].data);
        free(ov->slot);
        *ov = grown;
    }
    pg = calloc(1, OV_PAGE);
    if (!pg) return NULL;
    uint64_t base = idx * OV_PAGE, want = d->size - base < OV_PAGE ? d->size - base : OV_PAGE;
    for (uint64_t got = 0; got < want;) {
        ssize_t n = pread(d->fd, pg + got, want - got, (off_t)(d->offset + base + got));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { /* an error, or a file cut short since it was opened: the rest of the page is not zeros */
            free(pg);
            return NULL;
        }
        got += (uint64_t)n;
    }
    ov_insert(ov, idx, pg);
    return pg;
}

int hv_disk_open(HvDisk *d, const char *path, uint64_t offset, int readonly) {
    d->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (d->fd < 0) return -1;
    struct stat st;
    if (fstat(d->fd, &st) != 0 || (uint64_t)st.st_size < offset) {
        close(d->fd);
        return -1;
    }
    d->offset = offset;
    d->size = (uint64_t)st.st_size - offset;
    struct statfs fs;
    d->block_size = fstatfs(d->fd, &fs) == 0 && fs.f_bsize ? fs.f_bsize :
                    st.st_blksize > 0 ? (uint32_t)st.st_blksize : 4096u;
    d->readonly = readonly;
    d->overlay = NULL;
    return 0;
}

int hv_disk_grow(const char *path, uint64_t target) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    struct stat st;
    char magic[8];
    int rc = -1;
    /* Two launches growing one image take turns, so the size read here is the size cut to below: without the
       lock one could cut back what another, with a larger target, had just added. Released by close. */
    if (flock(fd, LOCK_EX) != 0) goto done;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) goto done;
    uint64_t size = (uint64_t)st.st_size;
    target &= ~511ull; /* whole 512-byte NVMe blocks */
    if (size >= target) { /* a disk that is big enough, or a larger one, is never touched */
        rc = 0;
        goto done;
    }
    /* A packed image is not a disk: growing it would only append zeros after its end record. */
    if (pread(fd, magic, sizeof magic, 0) == (ssize_t)sizeof magic && !memcmp(magic, "HVSPARSE", sizeof magic)) goto done;
    if (ftruncate(fd, (off_t)target) != 0) goto done; /* extends with a hole: no space is used */
    const char *name = strrchr(path, '/');
    fprintf(stderr, "winvm: grew disk image %s from %llu to %llu MiB (sparse)\n", name ? name + 1 : path,
            (unsigned long long)(size >> 20), (unsigned long long)(target >> 20));
    rc = 1;
done:
    close(fd);
    return rc;
}

void hv_disk_close(HvDisk *d) {
    if (d->fd >= 0) close(d->fd);
    d->fd = -1;
    Overlay *ov = d->overlay;
    if (ov) {
        for (uint64_t i = 0; i < ov->cap; i++) free(ov->slot[i].data);
        free(ov->slot);
        free(ov);
        d->overlay = NULL;
    }
}

int hv_disk_read(HvDisk *d, uint64_t off, void *buf, uint64_t len) {
    if (off > d->size || len > d->size - off) return -1;
    uint8_t *p = buf;
    while (len) {
        uint64_t in_page = OV_PAGE - off % OV_PAGE;
        const uint8_t *pg = d->overlay ? ov_find(d->overlay, off / OV_PAGE) : NULL;
        if (pg) {
            uint64_t n = len < in_page ? len : in_page;
            memcpy(p, pg + off % OV_PAGE, n);
            p += n;
            off += n;
            len -= n;
            continue;
        }
        ssize_t n = pread(d->fd, p, d->overlay && len > in_page ? in_page : len, (off_t)(d->offset + off));
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

int hv_disk_write(HvDisk *d, uint64_t off, const void *buf, uint64_t len) {
    if (off > d->size || len > d->size - off) return -1;
    const uint8_t *p = buf;
    if (d->readonly) {
        while (len) {
            uint8_t *pg = ov_page(d, off / OV_PAGE);
            if (!pg) return -1;
            uint64_t n = OV_PAGE - off % OV_PAGE;
            if (n > len) n = len;
            memcpy(pg + off % OV_PAGE, p, n);
            p += n;
            off += n;
            len -= n;
        }
        return 0;
    }
    while (len) {
        ssize_t n = pwrite(d->fd, p, len, (off_t)(d->offset + off));
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

static int disk_zero(HvDisk *d, uint64_t off, uint64_t len) {
    static const uint8_t zero[65536];
    while (len) {
        uint64_t n = len < sizeof zero ? len : sizeof zero;
        if (hv_disk_write(d, off, zero, n) != 0) return -1;
        off += n;
        len -= n;
    }
    return 0;
}

int hv_disk_discard(HvDisk *d, uint64_t off, uint64_t len) {
    if (off > d->size || len > d->size - off) return -1;
    if (!len) return 0;
    if (d->readonly) return disk_zero(d, off, len); /* into the overlay: the guest was told deallocated blocks read as zeros */
    uint64_t bs = d->block_size ? d->block_size : 4096; /* a disk that was never opened has none */
    uint64_t rem = (d->offset + off) % bs;
    uint64_t head = rem ? bs - rem : 0;
    if (head > len) head = len;
    if (disk_zero(d, off, head) != 0) return -1;
    off += head;
    len -= head;

    /* Align in the backing file, which may contain an image prefix. */
    uint64_t middle = len - len % bs;
    if (middle) {
        fpunchhole_t hole = {.fp_offset = (off_t)(d->offset + off), .fp_length = (off_t)middle};
        if (fcntl(d->fd, F_PUNCHHOLE, &hole) == -1) {
            if (errno != ENOTSUP && errno != EINVAL) return -1;
            if (disk_zero(d, off, middle) != 0) return -1;
        }
        off += middle;
        len -= middle;
    }
    return disk_zero(d, off, len);
}

/* HV_FLUSH: 0 fsync; 1 nothing here, what was written is safe in the host's cache as long as the
   host runs; 2 fcntl(F_BARRIERFSYNC). */
int hv_disk_flush(HvDisk *d) {
    if (d->readonly) return 0;
    switch (hv_knob[HV_K_FLUSH]) {
    case 1: return 0;
    case 2: return fcntl(d->fd, F_BARRIERFSYNC) == -1 ? -1 : 0;
    default: return fsync(d->fd);
    }
}

int hv_disk_sync(HvDisk *d) {
    if (d->fd < 0 || d->readonly) return 0;
    return fsync(d->fd);
}

int hv_disk_barrier(HvDisk *d) {
    if (d->fd < 0 || d->readonly) return 0;
    return fcntl(d->fd, F_BARRIERFSYNC) == -1 ? -1 : 0;
}
