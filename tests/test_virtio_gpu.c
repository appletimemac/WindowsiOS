// virtio-gpu: the command handlers driven through real virtqueues in guest RAM, and the
// display state they leave behind (scanout, dirty rectangles, cursor, resize events).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gicv3.h"
#include "machine.h"
#include "pcie.h"
#include "virtio_gpu.h"

#define RAM_PA 0x40000000ull
#define RAM_SIZE (32u << 20)
#define RING_PA(q) (RAM_PA + 0x10000 + (q) * 0x4000)  /* desc; avail +0x1000; used +0x2000 */
#define REQ_PA (RAM_PA + 0x20000)
#define RESP_PA (RAM_PA + 0x60000)
#define BACKING_PA (RAM_PA + 0x100000)
#define QSIZE 8

#define BAR 4
#define COMMON 0x0000
#define ISR 0x1000
#define NOTIFY 0x3000

static HvMachine m;
static uint8_t *ram;
static HvVirtioGpu *gpu;
static HvFbDisplay *fb;
static int checks, failures;
static uint16_t avail_idx[2];

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llx, expected %llx\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static uint8_t *host(uint64_t pa) { return ram + (pa - RAM_PA); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* As the machine's MMIO path does it: every device access under the device lock. */
static void bar_write(uint64_t off, uint64_t val, unsigned size) {
    hv_dev_lock(&m);
    gpu->vio.pci.bar_write(&gpu->vio.pci, BAR, off, val, size);
    hv_dev_unlock(&m);
}
static uint64_t bar_read(uint64_t off, unsigned size) {
    hv_dev_lock(&m);
    uint64_t v = gpu->vio.pci.bar_read(&gpu->vio.pci, BAR, off, size);
    hv_dev_unlock(&m);
    return v;
}

static void driver_init(void) {
    bar_write(COMMON + 0x14, 0, 1);
    bar_write(COMMON + 0x14, 1 | 2, 1);
    bar_write(COMMON + 0x08, 0, 4);
    bar_write(COMMON + 0x0c, 2, 4);
    bar_write(COMMON + 0x08, 1, 4);
    bar_write(COMMON + 0x0c, 1, 4);
    bar_write(COMMON + 0x14, 1 | 2 | 8, 1);
    for (int q = 0; q < 2; q++) {
        bar_write(COMMON + 0x16, q, 2);
        bar_write(COMMON + 0x18, QSIZE, 2);
        bar_write(COMMON + 0x20, RING_PA(q), 8);
        bar_write(COMMON + 0x28, RING_PA(q) + 0x1000, 8);
        bar_write(COMMON + 0x30, RING_PA(q) + 0x2000, 8);
        bar_write(COMMON + 0x1c, 1, 2);
        avail_idx[q] = 0;
    }
    bar_write(COMMON + 0x14, 1 | 2 | 4 | 8, 1);
}

/* One command on queue q: request in a read descriptor, response (if resp_cap) in a write descriptor.
   Returns the response type (0 for the cursor queue). resp gets the whole response. */
static uint32_t submit(int q, const void *req, uint32_t len, uint8_t *resp, uint32_t resp_cap) {
    memcpy(host(REQ_PA), req, len);
    memset(host(RESP_PA), 0xcc, resp_cap ? resp_cap : 1);
    uint8_t *desc = host(RING_PA(q));
    wr64(desc, REQ_PA);
    wr32(desc + 8, len);
    uint16_t flags = resp_cap ? 1 : 0, next = 1;
    memcpy(desc + 12, &flags, 2);
    memcpy(desc + 14, &next, 2);
    if (resp_cap) {
        wr64(desc + 16, RESP_PA);
        wr32(desc + 24, resp_cap);
        flags = 2, next = 0;
        memcpy(desc + 28, &flags, 2);
        memcpy(desc + 30, &next, 2);
    }
    uint8_t *avail = host(RING_PA(q) + 0x1000);
    uint16_t head = 0;
    memcpy(avail + 4 + 2 * (avail_idx[q] % QSIZE), &head, 2);
    avail_idx[q]++;
    memcpy(avail + 2, &avail_idx[q], 2);
    uint16_t used_before;
    memcpy(&used_before, host(RING_PA(q) + 0x2000) + 2, 2);
    bar_write(NOTIFY + q * 4, q, 2);
    uint16_t used_after;
    memcpy(&used_after, host(RING_PA(q) + 0x2000) + 2, 2);
    check("request completed", (uint16_t)(used_after - used_before), 1);
    if (!resp_cap) return 0;
    memcpy(resp, host(RESP_PA), resp_cap);
    return rd32(resp);
}

static uint8_t resp_buf[2048];
static uint32_t ctrl(const void *req, uint32_t len) {
    return submit(0, req, len, resp_buf, 1056);
}

static uint8_t req_buf[16384 * 16 + 64];
static void hdr(uint8_t *r, uint32_t cmd) {
    memset(r, 0, 24);
    wr32(r, cmd);
}

static uint32_t create_2d(uint32_t id, uint32_t fmt, uint32_t w, uint32_t h) {
    hdr(req_buf, 0x0101);
    wr32(req_buf + 24, id), wr32(req_buf + 28, fmt), wr32(req_buf + 32, w), wr32(req_buf + 36, h);
    return ctrl(req_buf, 40);
}

static uint32_t attach(uint32_t id, const uint64_t *addr, const uint32_t *len, uint32_t n) {
    hdr(req_buf, 0x0106);
    wr32(req_buf + 24, id), wr32(req_buf + 28, n);
    for (uint32_t i = 0; i < n; i++) {
        wr64(req_buf + 32 + 16 * i, addr[i]);
        wr32(req_buf + 40 + 16 * i, len[i]);
        wr32(req_buf + 44 + 16 * i, 0);
    }
    return ctrl(req_buf, 32 + 16 * n);
}

static uint32_t transfer(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint64_t off) {
    hdr(req_buf, 0x0105);
    wr32(req_buf + 24, x), wr32(req_buf + 28, y), wr32(req_buf + 32, w), wr32(req_buf + 36, h);
    wr64(req_buf + 40, off);
    wr32(req_buf + 48, id);
    return ctrl(req_buf, 56);
}

static uint32_t flush(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    hdr(req_buf, 0x0104);
    wr32(req_buf + 24, x), wr32(req_buf + 28, y), wr32(req_buf + 32, w), wr32(req_buf + 36, h);
    wr32(req_buf + 40, id);
    return ctrl(req_buf, 48);
}

static uint32_t set_scanout(uint32_t sid, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    hdr(req_buf, 0x0103);
    wr32(req_buf + 24, x), wr32(req_buf + 28, y), wr32(req_buf + 32, w), wr32(req_buf + 36, h);
    wr32(req_buf + 40, sid), wr32(req_buf + 44, id);
    return ctrl(req_buf, 48);
}

static uint32_t simple(uint32_t cmd, uint32_t id) {
    hdr(req_buf, cmd);
    wr32(req_buf + 24, id);
    return ctrl(req_buf, 32);
}

static void cursor_cmd(uint32_t cmd, int32_t x, int32_t y, uint32_t rid, uint32_t hot_x, uint32_t hot_y) {
    hdr(req_buf, cmd);
    wr32(req_buf + 28, (uint32_t)x), wr32(req_buf + 32, (uint32_t)y);
    wr32(req_buf + 40, rid), wr32(req_buf + 44, hot_x), wr32(req_buf + 48, hot_y);
    submit(1, req_buf, 56, NULL, 0);
}

static HvFbFrame take(void) {
    HvFbFrame f;
    hv_fb_display_lock(fb);
    int r = hv_fb_display_take_frame(fb, &f);
    hv_fb_display_unlock(fb);
    if (r != 0) memset(&f, 0, sizeof f);
    return f;
}

static uint32_t px(const HvFbFrame *f, uint32_t x, uint32_t y) {
    return rd32(f->pixels + (uint64_t)y * f->stride + x * 4);
}

static uint32_t pattern(uint32_t x, uint32_t y) { return 0xff000000u | (y & 0xff) << 16 | (x & 0xff) << 8 | ((x ^ y) & 0xff); }

static void test_identity(void) {
    const uint8_t *c = gpu->vio.pci.config;
    check("vendor", c[0] | c[1] << 8, 0x1af4);
    check("device (modern 0x1040 + 16)", c[2] | c[3] << 8, 0x1050);
    check("class code: display, other", (c[0x0b] << 16) | (c[0x0a] << 8) | c[0x09], 0x038000);
    check("revision", c[0x08], 1);
    check("subsystem vendor", c[0x2c] | c[0x2d] << 8, 0x1af4);
    check("subsystem id (SUBSYS_11001AF4)", c[0x2e] | c[0x2f] << 8, 0x1100);
    bar_write(COMMON + 0x00, 0, 4);
    check("feature word 0 is EDID only", bar_read(COMMON + 0x04, 4), 2);
    bar_write(COMMON + 0x00, 1, 4);
    check("feature word 1 is VERSION_1", bar_read(COMMON + 0x04, 4), 1);
    check("two queues", bar_read(COMMON + 0x12, 2), 2);
    check("num_scanouts", bar_read(0x2000 + 8, 4), 1);
    check("num_capsets", bar_read(0x2000 + 12, 4), 0);
    check("events_read starts clear", bar_read(0x2000, 4), 0);
}

static void test_edid(void) {
    uint8_t e[128];
    static const struct { uint32_t w, h; } sizes[] = {{1280, 900}, {640, 480}, {3840, 2160}, {1920, 1080}, {2560, 1440}, {1000, 700}};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        hv_virtio_gpu_build_edid(e, sizes[i].w, sizes[i].h);
        unsigned sum = 0;
        for (int k = 0; k < 128; k++) sum += e[k];
        check("EDID checksum", sum & 0xff, 0);
        const uint8_t *d = e + 54;
        uint32_t w = d[2] | (d[4] >> 4) << 8, h = d[5] | (d[7] >> 4) << 8;
        check("DTD width", w, sizes[i].w);
        check("DTD height", h, sizes[i].h);
        uint32_t hb = d[3] | (d[4] & 0xf) << 8, vb = d[6] | (d[7] & 0xf) << 8, clock = d[0] | d[1] << 8;
        double rate = clock * 10000.0 / ((double)(w + hb) * (h + vb));
        checks++;
        if (rate < 59.0 || rate > 60.5) { /* CVT-RB rounds the clock down to 0.25 MHz */
            printf("  FAIL refresh %.2f Hz for %ux%u\n", rate, sizes[i].w, sizes[i].h);
            failures++;
        }
        check("DTD hsize mm", d[12] | (d[14] >> 4) << 8, hv_vgpu_phys_mm(sizes[i].w));
        check("DTD vsize mm", d[13] | (d[14] & 0xf) << 8, hv_vgpu_phys_mm(sizes[i].h));
    }
    hv_virtio_gpu_build_edid(e, 1280, 900);
    static const uint8_t header[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    check("EDID header", memcmp(e, header, 8), 0);
    check("EDID version", e[18] << 8 | e[19], 0x0104);
    check("digital input", e[20] >> 7, 1);
    check("preferred timing is the first DTD", (e[24] >> 1) & 1, 1);
    check("no extensions", e[126], 0);
    check("range limits descriptor", e[75], 0xfd);
    check("name descriptor", e[93], 0xfc);
    check("base block width cm", e[21], (hv_vgpu_phys_mm(1280) + 5) / 10);
    check("base block height cm", e[22], (hv_vgpu_phys_mm(900) + 5) / 10);

    hdr(req_buf, 0x010a);
    wr32(req_buf + 24, 0);
    check("GET_EDID", ctrl(req_buf, 32), 0x1104);
    check("EDID size", rd32(resp_buf + 24), 128);
    check("EDID equals the device's", memcmp(resp_buf + 32, gpu->edid, 128), 0);
    wr32(req_buf + 24, 1);
    check("GET_EDID scanout 1", ctrl(req_buf, 32), 0x1205);
}

static void test_display_info(void) {
    hdr(req_buf, 0x0100);
    check("GET_DISPLAY_INFO", submit(0, req_buf, 24, resp_buf, 408), 0x1101);
    check("scanout 0 width", rd32(resp_buf + 24 + 8), 1280);
    check("scanout 0 height", rd32(resp_buf + 24 + 12), 900);
    check("scanout 0 enabled", rd32(resp_buf + 24 + 16), 1);
    check("scanout 1 disabled", rd32(resp_buf + 24 + 24 + 16), 0);
}

static void test_errors_and_fences(void) {
    hdr(req_buf, 0x0108); /* GET_CAPSET_INFO */
    check("unsupported command", submit(0, req_buf, 32, resp_buf, 64), 0x1200);
    hdr(req_buf, 0x0100);
    wr32(req_buf + 4, 1);
    wr64(req_buf + 8, 0x1122334455667788ull);
    wr32(req_buf + 16, 7);
    check("fenced GET_DISPLAY_INFO", submit(0, req_buf, 24, resp_buf, 408), 0x1101);
    check("fence flag echoed", rd32(resp_buf + 4), 1);
    check("fence id echoed", rd64(resp_buf + 8), 0x1122334455667788ull);
    check("ctx id echoed", rd32(resp_buf + 16), 7);
    hdr(req_buf, 0x0100);
    submit(0, req_buf, 24, resp_buf, 408);
    check("no fence, no flag", rd32(resp_buf + 4), 0);
    check("no fence, no id", rd64(resp_buf + 8), 0);
    check("short command", submit(0, req_buf, 8, resp_buf, 64), 0x1200);
    check("create: id 0", create_2d(0, 2, 64, 64), 0x1203);
    check("create: bad format", create_2d(9, 67, 64, 64), 0x1205);
    check("create: zero width", create_2d(9, 2, 0, 64), 0x1205);
    check("create: huge", create_2d(9, 2, 0xffffffffu, 0xffffffffu), 0x1205);
    check("create: over the per-resource limit", create_2d(9, 2, 16384, 16384), 0x1201);
    check("unref unknown", simple(0x0102, 99), 0x1203);
    check("attach to unknown", attach(99, (uint64_t[]){BACKING_PA}, (uint32_t[]){4096}, 1), 0x1203);
    check("detach unknown", simple(0x0107, 99), 0x1203);
    check("flush unknown", flush(99, 0, 0, 1, 1), 0x1203);
    check("transfer unknown", transfer(99, 0, 0, 1, 1, 0), 0x1203);
    check("scanout id 1", set_scanout(1, 0, 0, 0, 0, 0), 0x1202);
}

static void test_scanout_flow(void) {
    const uint32_t W = 640, H = 480;
    check("create 1", create_2d(1, 2, W, H), 0x1100);
    check("create duplicate", create_2d(1, 2, W, H), 0x1203);
    check("transfer without backing", transfer(1, 0, 0, W, H, 0), 0x1200);
    /* backing: three pieces, with the pixels laid out as the guest's frame buffer */
    uint64_t total = (uint64_t)W * H * 4;
    uint64_t a0 = BACKING_PA, a1 = BACKING_PA + 0x80000, a2 = BACKING_PA + 0x200000;
    uint32_t l0 = 0x70000, l1 = 0x90000, l2 = (uint32_t)(total - l0 - l1);
    uint8_t *fbmem = malloc(total);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) wr32(fbmem + ((uint64_t)y * W + x) * 4, pattern(x, y));
    memcpy(host(a0), fbmem, l0);
    memcpy(host(a1), fbmem + l0, l1);
    memcpy(host(a2), fbmem + l0 + l1, l2);
    check("attach", attach(1, (uint64_t[]){a0, a1, a2}, (uint32_t[]){l0, l1, l2}, 3), 0x1100);
    check("attach twice", attach(1, (uint64_t[]){a0}, (uint32_t[]){4096}, 1), 0x1200);

    check("scanout rect outside the resource", set_scanout(0, 1, 0, 0, W + 2, H), 0x1205);
    check("scanout rect empty", set_scanout(0, 1, 0, 0, 0, 0), 0x1205);
    check("scanout of unknown resource", set_scanout(0, 77, 0, 0, W, H), 0x1203);
    HvFbFrame f = take();
    check("no scanout yet", f.from_scanout, 0);
    check("set scanout", set_scanout(0, 1, 0, 0, W, H), 0x1100);
    f = take();
    check("scanout active", f.from_scanout, 1);
    check("scanout width", f.width, W);
    check("scanout height", f.height, H);
    check("scanout stride", f.stride, W * 4);
    check("first take: dirty is exact", f.has_dirty, 1);
    check("first take: whole frame dirty", f.dirty_w == W && f.dirty_h == H && !f.dirty_x && !f.dirty_y, 1);
    uint64_t gen = f.generation;
    check("blank before the transfer", px(&f, 10, 10), 0);

    check("transfer all", transfer(1, 0, 0, W, H, 0), 0x1100);
    f = take();
    check("pixel (0,0)", px(&f, 0, 0), pattern(0, 0));
    check("pixel (10,20)", px(&f, 10, 20), pattern(10, 20));
    check("pixel across piece 1 (row 112)", px(&f, 5, 112), pattern(5, 112));
    check("pixel across piece 2 (row 300)", px(&f, 600, 300), pattern(600, 300));
    check("pixel last", px(&f, W - 1, H - 1), pattern(W - 1, H - 1));
    check("transfer marks its rectangle", f.dirty_w == W && f.dirty_h == H, 1);
    check("generation unchanged by a transfer", f.generation, gen);
    f = take();
    check("nothing dirty after a take", f.has_dirty && f.dirty_w == 0 && f.dirty_h == 0, 1);

    /* the guest redraws a rectangle and sends only that, with the offset of its first pixel */
    for (uint32_t y = 50; y < 80; y++)
        for (uint32_t x = 100; x < 140; x++) wr32(fbmem + ((uint64_t)y * W + x) * 4, 0xff00ff00u ^ x);
    memcpy(host(a0), fbmem, l0);
    memcpy(host(a1), fbmem + l0, l1);
    memcpy(host(a2), fbmem + l0 + l1, l2);
    check("transfer rect", transfer(1, 100, 50, 40, 30, (50ull * W + 100) * 4), 0x1100);
    check("flush rect", flush(1, 100, 50, 40, 30), 0x1100);
    f = take();
    check("rect dirty x", f.dirty_x, 100);
    check("rect dirty y", f.dirty_y, 50);
    check("rect dirty w", f.dirty_w, 40);
    check("rect dirty h", f.dirty_h, 30);
    check("rect pixel", px(&f, 101, 51), 0xff00ff00u ^ 101);
    check("outside the rect unchanged", px(&f, 99, 51), pattern(99, 51));
    check("flush out of bounds", flush(1, 600, 0, 100, 10), 0x1205);
    check("transfer out of bounds", transfer(1, 600, 0, 100, 10, 0), 0x1205);
    check("transfer overflowing rect", transfer(1, 0xfffffff0u, 0, 0x20, 1, 0), 0x1205);
    check("transfer offset past the backing", transfer(1, 0, 0, 1, 1, total), 0x1205);
    check("transfer offset overflow", transfer(1, 0, 0, 1, 1, ~0ull), 0x1205);
    check("transfer near the end is clipped, not a crash", transfer(1, 0, H - 1, W, 1, total - 8), 0x1100);

    /* two flushes union into one rectangle */
    take();
    flush(1, 10, 10, 5, 5);
    flush(1, 100, 200, 10, 10);
    f = take();
    check("union x", f.dirty_x, 10);
    check("union y", f.dirty_y, 10);
    check("union w", f.dirty_w, 100);
    check("union h", f.dirty_h, 200);

    /* a sub-rectangle of the resource as the scanout */
    check("scanout part", set_scanout(0, 1, 16, 8, 320, 200), 0x1100);
    f = take();
    check("part: width", f.width, 320);
    check("part: height", f.height, 200);
    check("part: stride is the resource's", f.stride, W * 4);
    check("part: generation bumped", f.generation > gen, 1);
    check("part: first pixel is (16,8)", px(&f, 0, 0), pattern(16, 8));
    flush(1, 0, 0, 16, 8);
    f = take();
    check("flush outside the shown part marks nothing", f.has_dirty && f.dirty_w == 0, 1);
    flush(1, 10, 4, 20, 20);
    f = take();
    check("flush straddling the part's corner is clipped: x", f.dirty_x, 0);
    check("clipped: w", f.dirty_w, 14);
    check("clipped: h", f.dirty_h, 16);
    set_scanout(0, 1, 0, 0, W, H);

    /* freeing the scanout's resource switches the display away first */
    check("unref the scanout", simple(0x0102, 1), 0x1100);
    f = take();
    check("scanout gone after unref", f.from_scanout, 0);
    check("no source left", f.pixels == NULL, 1);
    check("get_frame agrees", hv_fb_display_get_frame(fb, &(const uint8_t *){0}, &(uint32_t){0}, &(uint32_t){0}, &(uint32_t){0}), -1);
    check("host memory released", gpu->host_bytes, 0);
    free(fbmem);
}

static void test_formats(void) {
    check("create ARGB", create_2d(2, 3, 64, 64), 0x1100);
    uint8_t *src = host(BACKING_PA + 0x300000);
    for (int i = 0; i < 64 * 64; i++) {
        src[4 * i + 0] = 0x80; /* A */
        src[4 * i + 1] = 0x11; /* R */
        src[4 * i + 2] = 0x22; /* G */
        src[4 * i + 3] = 0x33; /* B */
    }
    attach(2, (uint64_t[]){BACKING_PA + 0x300000}, (uint32_t[]){64 * 64 * 4}, 1);
    check("transfer ARGB", transfer(2, 0, 0, 64, 64, 0), 0x1100);
    const uint8_t *h = NULL;
    for (int i = 0; i < HV_VGPU_MAX_RESOURCES; i++)
        if (gpu->res[i].id == 2) h = gpu->res[i].pixels;
    check("ARGB resource found", h != NULL, 1);
    check("B", h[0], 0x33);
    check("G", h[1], 0x22);
    check("R", h[2], 0x11);
    check("A", h[3], 0x80);
    simple(0x0102, 2);

    check("create BGRA", create_2d(3, 1, 8, 8), 0x1100);
    check("create BGRX", create_2d(4, 2, 8, 8), 0x1100);
    check("create XRGB", create_2d(5, 4, 8, 8), 0x1100);
    simple(0x0102, 3), simple(0x0102, 4), simple(0x0102, 5);

    uint64_t bad = RAM_PA + RAM_SIZE - 16;
    check("create for backing tests", create_2d(6, 2, 64, 64), 0x1100);
    check("backing outside RAM", attach(6, (uint64_t[]){bad}, (uint32_t[]){4096}, 1), 0x1205);
    check("backing at address 0", attach(6, (uint64_t[]){0}, (uint32_t[]){4096}, 1), 0x1205);
    check("backing with zero length", attach(6, (uint64_t[]){BACKING_PA}, (uint32_t[]){0}, 1), 0x1205);
    check("no entries", attach(6, (uint64_t[]){BACKING_PA}, (uint32_t[]){4096}, 0), 0x1205);
    hdr(req_buf, 0x0106);
    wr32(req_buf + 24, 6), wr32(req_buf + 28, 16384);
    check("entries missing from the request", ctrl(req_buf, 32 + 16), 0x1205);
    wr32(req_buf + 28, 0xffffffffu);
    check("absurd entry count", ctrl(req_buf, 32 + 16), 0x1205);
    check("a failed attach leaves no backing", transfer(6, 0, 0, 1, 1, 0), 0x1200);
    check("detach", simple(0x0107, 6), 0x1100);
    simple(0x0102, 6);
}

static void test_cursor(void) {
    /* the display reports a cursor only while something is on screen */
    check("create scanout resource", create_2d(9, 2, 640, 480), 0x1100);
    check("scanout for the cursor", set_scanout(0, 9, 0, 0, 640, 480), 0x1100);
    check("create cursor resource", create_2d(7, 1, 64, 64), 0x1100);
    uint8_t *src = host(BACKING_PA + 0x400000);
    for (int i = 0; i < 64 * 64; i++) wr32(src + 4 * i, 0x80000000u | (uint32_t)i);
    attach(7, (uint64_t[]){BACKING_PA + 0x400000}, (uint32_t[]){64 * 64 * 4}, 1);
    transfer(7, 0, 0, 64, 64, 0);
    HvFbFrame f = take();
    check("cursor hidden at first", f.cursor_visible, 0);
    check("no cursor image", f.cursor_image == NULL, 1);
    cursor_cmd(0x0300, 120, 80, 7, 3, 5);
    f = take();
    check("cursor visible", f.cursor_visible, 1);
    check("cursor x", (uint32_t)f.cursor_x, 120);
    check("cursor y", (uint32_t)f.cursor_y, 80);
    check("hot x", f.cursor_hot_x, 3);
    check("hot y", f.cursor_hot_y, 5);
    check("image present", f.cursor_image != NULL, 1);
    if (f.cursor_image) {
        check("image pixel 0", f.cursor_image[0], 0x80000000u);
        check("image pixel 4095", f.cursor_image[4095], 0x80000000u | 4095);
    }
    uint64_t gen = f.cursor_generation;
    cursor_cmd(0x0301, -10, 20, 7, 0, 0);
    f = take();
    check("moved x (negative)", (uint32_t)f.cursor_x, (uint32_t)-10);
    check("moved y", (uint32_t)f.cursor_y, 20);
    check("move keeps the image generation", f.cursor_generation, gen);
    check("move keeps it visible", f.cursor_visible, 1);
    cursor_cmd(0x0301, 5, 6, 0, 0, 0);
    f = take();
    check("move with resource 0 hides", f.cursor_visible, 0);
    cursor_cmd(0x0300, 1, 2, 7, 0, 0);
    cursor_cmd(0x0300, 1, 2, 0, 0, 0);
    f = take();
    check("update with resource 0 hides and drops the image", f.cursor_visible == 0 && f.cursor_image == NULL, 1);
    check("image change bumped the generation", f.cursor_generation > gen, 1);
    cursor_cmd(0x0300, 1, 2, 1234, 0, 0);
    f = take();
    check("unknown cursor resource ignored", f.cursor_visible, 0);
    simple(0x0102, 7);
    simple(0x0102, 9);
}

static void test_resize(void) {
    uint8_t isr_before = (uint8_t)bar_read(ISR, 1); /* clears anything pending */
    (void)isr_before;
    uint8_t gen = (uint8_t)bar_read(COMMON + 0x15, 1);
    hv_fb_display_request_size(fb, 1001, 701);
    check("display width follows (even)", gpu->width, 1000);
    check("display height follows (even)", gpu->height, 700);
    check("DISPLAY event", bar_read(0x2000, 4), 1);
    check("config generation moved", (uint8_t)bar_read(COMMON + 0x15, 1), (uint8_t)(gen + 1));
    check("INTx raised", gpu->vio.pci.intx, 1);
    check("config bit in the ISR", bar_read(ISR, 1), 2);
    check("ISR read drops the line", gpu->vio.pci.intx, 0);
    hdr(req_buf, 0x0100);
    submit(0, req_buf, 24, resp_buf, 408);
    check("display info: new width", rd32(resp_buf + 24 + 8), 1000);
    check("display info: new height", rd32(resp_buf + 24 + 12), 700);
    hdr(req_buf, 0x010a);
    wr32(req_buf + 24, 0);
    ctrl(req_buf, 32);
    const uint8_t *d = resp_buf + 32 + 54;
    check("EDID: new width", d[2] | (d[4] >> 4) << 8, 1000);
    check("EDID: new height", d[5] | (d[7] >> 4) << 8, 700);
    bar_write(0x2000 + 4, 1, 4);
    check("events_clear clears", bar_read(0x2000, 4), 0);

    gen = (uint8_t)bar_read(COMMON + 0x15, 1);
    check("the commands' own interrupt is a queue interrupt", bar_read(ISR, 1), 1);
    hv_fb_display_request_size(fb, 1000, 700);
    check("same size: no new config generation", (uint8_t)bar_read(COMMON + 0x15, 1), gen);
    check("same size: no event", bar_read(0x2000, 4), 0);
    check("same size: no interrupt", gpu->vio.pci.intx, 0);

    hv_fb_display_request_size(fb, 100, 100);
    check("clamped up: width", gpu->width, 640);
    check("clamped up: height", gpu->height, 480);
    hv_fb_display_request_size(fb, 9000, 9000);
    check("clamped down: width", gpu->width, 3840);
    check("clamped down: height", gpu->height, 2160);
    hv_fb_display_request_size(fb, 1280, 900);
    bar_read(ISR, 1);
    bar_write(0x2000 + 4, 1, 4);
    check("back to the default", gpu->width == 1280 && gpu->height == 900, 1);
    hv_virtio_gpu_set_size(gpu, 1280, 900);
}

static void test_reset(void) {
    check("create", create_2d(8, 2, 64, 64), 0x1100);
    check("scanout", set_scanout(0, 8, 0, 0, 64, 64), 0x1100);
    cursor_cmd(0x0300, 1, 2, 8, 0, 0);
    hv_fb_display_request_size(fb, 800, 600);
    bar_write(COMMON + 0x14, 0, 1); /* the driver resets the device */
    check("resources gone", gpu->host_bytes, 0);
    check("scanout gone", gpu->scanout_res, 0);
    HvFbFrame f = take();
    check("display scanout gone", f.from_scanout, 0);
    check("cursor gone", f.cursor_visible, 0);
    check("events cleared", bar_read(0x2000, 4), 0);
    check("size is kept", gpu->width == 800 && gpu->height == 600, 1);
    check("ISR/INTx clear", gpu->vio.pci.intx, 0);
    check("no interrupt from a config change before DRIVER_OK", (uint32_t)(hv_virtio_config_changed(&gpu->vio), gpu->vio.pci.intx), 0);
    driver_init();
    check("queues work after the reset", create_2d(8, 2, 64, 64), 0x1100);
    hv_virtio_gpu_reset(gpu);
    check("explicit reset", gpu->host_bytes, 0);
}

/* Host threads: a presenter taking frames and a window resizing, against the guest drawing. */
static _Atomic int stop;

static void *presenter(void *arg) {
    (void)arg;
    uint64_t frames = 0;
    while (!stop) {
        hv_fb_display_lock(fb);
        HvFbFrame f;
        if (hv_fb_display_take_frame(fb, &f) == 0 && f.from_scanout) {
            volatile uint32_t sum = 0; /* read what a presenter reads while holding the lock */
            for (uint32_t y = 0; y < f.height; y += 16) sum += rd32(f.pixels + (uint64_t)y * f.stride);
            if (f.cursor_image) sum += f.cursor_image[0];
            frames++;
        }
        hv_fb_display_unlock(fb);
    }
    return (void *)(uintptr_t)frames;
}

static void *resizer(void *arg) {
    (void)arg;
    for (uint32_t i = 0; !stop; i++) hv_fb_display_request_size(fb, 640 + (i * 14) % 1600, 480 + (i * 10) % 900);
    return NULL;
}

static void test_threads(void) {
    pthread_t p, r;
    stop = 0;
    pthread_create(&p, NULL, presenter, NULL);
    pthread_create(&r, NULL, resizer, NULL);
    for (int i = 0; i < 300; i++) {
        uint32_t id = 20 + (uint32_t)(i & 1);
        create_2d(id, 2, 320, 200);
        set_scanout(0, id, 0, 0, 320, 200);
        transfer(id, 0, 0, 320, 200, 0);
        flush(id, 0, 0, 320, 200);
        cursor_cmd(0x0301, i, i, id, 0, 0);
        simple(0x0102, id); /* freed while it is the scanout, with the presenter running */
    }
    stop = 1;
    void *frames;
    pthread_join(p, &frames);
    pthread_join(r, NULL);
    check("threads: no resources left", gpu->host_bytes, 0);
    check("threads: presenter saw frames", (uintptr_t)frames > 0, 1);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    hv_machine_init(&m, 1 << 20);
    ram = aligned_alloc(65536, RAM_SIZE);
    memset(ram, 0, RAM_SIZE);
    hv_machine_add_ram(&m, RAM_PA, RAM_SIZE, ram, 0);
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *pcie = hv_pcie_create(&m, gic, 0x4010000000ull, 0x10000000, 0x10000000, 0x2eff0000, 35);
    hv_pcie_set_gpex_swizzle(pcie, 1);
    fb = hv_fb_display_create(&m, NULL);
    gpu = hv_virtio_gpu_create(&m, pcie, fb);
    if (!gpu) return 1;

    test_identity();
    driver_init();
    test_display_info();
    test_edid();
    test_errors_and_fences();
    test_scanout_flow();
    test_formats();
    test_cursor();
    test_resize();
    test_reset();
    test_threads();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
