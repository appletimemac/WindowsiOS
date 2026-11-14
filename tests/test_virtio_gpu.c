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
static HvGic *gic;
static HvPcie *pcie;
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

/* viogpudo maps the config vector even on INTx and gives up when the mapping reads back NO_VECTOR. */
static void test_msix_vectors(void) {
    check("one vector for config changes and one per queue", gpu->vio.msix_vectors, 3);
    check("config vector starts unmapped", bar_read(COMMON + 0x10, 2), 0xffff);
    bar_write(COMMON + 0x10, 0, 2);
    check("config vector 0 echoes", bar_read(COMMON + 0x10, 2), 0);
    bar_write(COMMON + 0x10, 3, 2);
    check("a vector past the table reads NO_VECTOR", bar_read(COMMON + 0x10, 2), 0xffff);
    bar_write(COMMON + 0x10, 2, 2);
    bar_write(COMMON + 0x16, 1, 2);
    check("queue vector starts unmapped", bar_read(COMMON + 0x1a, 2), 0xffff);
    bar_write(COMMON + 0x1a, 1, 2);
    check("queue 1 vector echoes", bar_read(COMMON + 0x1a, 2), 1);
    bar_write(COMMON + 0x16, 0, 2);
    check("queue 0 keeps its own vector", bar_read(COMMON + 0x1a, 2), 0xffff);
    bar_write(COMMON + 0x1a, 0xffff, 2);
    check("NO_VECTOR unmaps", bar_read(COMMON + 0x1a, 2), 0xffff);
    bar_write(COMMON + 0x16, 9, 2);
    check("no queue selected: NO_VECTOR", bar_read(COMMON + 0x1a, 2), 0xffff);
    bar_write(COMMON + 0x14, 0, 1);
    check("reset unmaps the config vector", bar_read(COMMON + 0x10, 2), 0xffff);
    bar_write(COMMON + 0x16, 1, 2);
    check("reset unmaps queue vectors", bar_read(COMMON + 0x1a, 2), 0xffff);
    bar_write(COMMON + 0x16, 0, 2);
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

static void check_grid(const char *what, const uint64_t *want) {
    for (int i = 0; i < HV_VGPU_GRID_W * HV_VGPU_GRID_H; i++) {
        char name[96];
        snprintf(name, sizeof name, "%s: grid cell %d (col %d, row %d)", what, i, i % HV_VGPU_GRID_W, i / HV_VGPU_GRID_W);
        check(name, gpu->grid[i], want[i]);
    }
}

static void zero_flush_counters(void) {
    gpu->flush_px = gpu->rect_small = gpu->rect_mid = gpu->rect_big = 0;
    memset(gpu->grid, 0, sizeof gpu->grid);
}

/* The counters the host reads from another thread: flushed pixels by rectangle size and by where on the scanout,
   and the transfer bytes and time. A flush counts only the part of its rectangle that is on the scanout. */
static void test_counters(void) {
    const uint32_t W = 640, H = 480;
    const uint64_t total = (uint64_t)W * H * 4;
    uint64_t want[HV_VGPU_GRID_W * HV_VGPU_GRID_H] = {0};
    check("grid is 8x6", HV_VGPU_GRID_W == 8 && HV_VGPU_GRID_H == 6, 1);
    check("create", create_2d(30, 2, W, H), 0x1100);
    check("create (never the scanout)", create_2d(31, 2, 64, 64), 0x1100);
    check("attach", attach(30, (uint64_t[]){BACKING_PA + 0x400000}, (uint32_t[]){(uint32_t)total}, 1), 0x1100);

    /* the whole resource is the scanout: cells are 80x80 pixels */
    check("scanout", set_scanout(0, 30, 0, 0, W, H), 0x1100);
    zero_flush_counters();
    check("flush small", flush(30, 0, 0, 100, 100), 0x1100); /* 10000 px, center (50,50): col 0, row 0 */
    want[0] += 10000;
    check("flush mid", flush(30, 320, 240, 200, 100), 0x1100); /* 20000 px, center (420,290): col 5, row 3 */
    want[3 * 8 + 5] += 20000;
    check("flush big", flush(30, 0, 0, 640, 480), 0x1100); /* 307200 px, center (320,240): col 4, row 3 */
    want[3 * 8 + 4] += 307200;
    check("flush of 16384 px is mid", flush(30, 512, 352, 128, 128), 0x1100); /* center (576,416): col 7, row 5 */
    want[5 * 8 + 7] += 16384;
    check("flush of 16383 px is small", flush(30, 0, 0, 127, 129), 0x1100); /* center (63,64): col 0, row 0 */
    want[0] += 16383;
    check("flush of 261760 px is mid", flush(30, 0, 0, 640, 409), 0x1100); /* center (320,204): col 4, row 2 */
    want[2 * 8 + 4] += 261760;
    check("flush of 262400 px is big", flush(30, 0, 0, 640, 410), 0x1100); /* center (320,205): col 4, row 2 */
    want[2 * 8 + 4] += 262400;
    check("flush_px", gpu->flush_px, 10000 + 20000 + 307200 + 16384 + 16383 + 261760 + 262400);
    check("small rects", gpu->rect_small, 2);
    check("mid rects", gpu->rect_mid, 3);
    check("big rects", gpu->rect_big, 2);
    check_grid("whole resource", want);

    /* a part of the resource is the scanout: x 160..480, y 120..360, cells 40x40 pixels */
    check("scanout part", set_scanout(0, 30, 160, 120, 320, 240), 0x1100);
    zero_flush_counters();
    memset(want, 0, sizeof want);
    uint64_t flushes = gpu->flushes;
    check("flush over the top left corner", flush(30, 100, 60, 120, 100), 0x1100); /* clipped to x 160..220, y 120..160 */
    want[0] += 60 * 40;                                                            /* center (190,140): col 0, row 0 */
    check("flush over the bottom right corner", flush(30, 400, 300, 200, 180), 0x1100); /* clipped to x 400..480, y 300..360 */
    want[5 * 8 + 7] += 80 * 60;                                                    /* center (440,330): col 7, row 5 */
    check("flush_px is the clipped area", gpu->flush_px, 60 * 40 + 80 * 60);
    check("clipped rects are small", gpu->rect_small, 2);
    check("no mid", gpu->rect_mid, 0);
    check("no big", gpu->rect_big, 0);
    check_grid("clipped", want);
    check("flush wholly left of the part", flush(30, 0, 0, 100, 100), 0x1100);
    check("flush wholly below it", flush(30, 0, 400, 640, 80), 0x1100);
    check("nothing counted off the scanout", gpu->flush_px, 60 * 40 + 80 * 60);
    check("no bucket either", gpu->rect_small + gpu->rect_mid + gpu->rect_big, 2);
    check("the flush counter still counts them", gpu->flushes - flushes, 4);
    check_grid("off the scanout", want);
    check("flush covering the whole part", flush(30, 0, 0, 640, 480), 0x1100); /* clipped to 320x240; center (320,240): col 4, row 3 */
    want[3 * 8 + 4] += 320 * 240;
    check("flush_px with the whole part", gpu->flush_px, 60 * 40 + 80 * 60 + 320 * 240);
    check("the whole part is a mid rect", gpu->rect_mid, 1);
    check_grid("whole part", want);

    /* a resource that is not on the scanout, and no scanout at all */
    zero_flush_counters();
    check("flush of a resource that is not the scanout", flush(31, 0, 0, 64, 64), 0x1100);
    check("scanout off", set_scanout(0, 0, 0, 0, 0, 0), 0x1100);
    check("flush with no scanout", flush(30, 0, 0, 100, 100), 0x1100);
    check("flush_px stays 0", gpu->flush_px, 0);
    check("no buckets", gpu->rect_small + gpu->rect_mid + gpu->rect_big, 0);
    memset(want, 0, sizeof want);
    check_grid("no scanout", want);

    /* transfers: the bytes actually copied, and the host time that took */
    gpu->xfer_bytes = gpu->xfer_ticks = 0;
    check("transfer all", transfer(30, 0, 0, W, H, 0), 0x1100);
    check("bytes of a whole transfer", gpu->xfer_bytes, total);
    check("it took some time", gpu->xfer_ticks > 0, 1);
    uint64_t ticks = gpu->xfer_ticks;
    check("transfer a rect", transfer(30, 100, 50, 40, 30, (50ull * W + 100) * 4), 0x1100);
    check("bytes of a rectangle", gpu->xfer_bytes, total + 40 * 30 * 4);
    check("time only grows", gpu->xfer_ticks >= ticks, 1);
    check("a rejected transfer", transfer(30, 600, 0, 100, 10, 0), 0x1205);
    check("an empty transfer", transfer(30, 0, 0, 0, 0, 0), 0x1100);
    check("neither counts bytes", gpu->xfer_bytes, total + 40 * 30 * 4);
    check("transfer past the end of the backing", transfer(30, 0, H - 1, W, 1, total - 8), 0x1100);
    check("only the 8 bytes that were there count", gpu->xfer_bytes, total + 40 * 30 * 4 + 8);

    check("unref", simple(0x0102, 30), 0x1100);
    check("unref the other", simple(0x0102, 31), 0x1100);
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
    check("config change reads 3 in the ISR, as QEMU's", bar_read(ISR, 1), 3);
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

/* How the driver is interrupted. On INTx a set ISR bit holds a level line up until the driver reads the
   register, and the line is shared (with the network device on the board): raised when the driver is not
   looking, it is taken again for ever. So the driver's wish for no interrupts on a queue is honoured, MSI-X
   delivers messages and leaves the ISR and the line alone, and a function whose bus mastering the OS turned
   off has no running driver. Config space is written through ECAM, as the guest writes it. */
#define ECAM 0x4010000000ull
#define DOORBELL 0x08020040ull
#define MSIX 0x98
static void cfg_write16(uint32_t reg, uint16_t val) {
    uint64_t pa = ECAM + ((uint64_t)gpu->vio.pci.slot << 15) + reg;
    HvMmio *e = hv_mmio_lookup(&m, pa);
    hv_dev_lock(&m);
    e->write(e->opaque, pa - e->pa, val, 2);
    hv_dev_unlock(&m);
}
static void set_avail_flags(int q, uint16_t flags) {
    memcpy(host(RING_PA(q) + 0x1000), &flags, 2);
}
static void display_info(void) {
    hdr(req_buf, 0x0100);
    submit(0, req_buf, 24, resp_buf, 408);
}
/* the INTx line as the device, the bus and the interrupt controller each hold it */
static int line_up(void) {
    HvPciDev *d = &gpu->vio.pci;
    int line = d->slot & 3; /* pin A with the gpex swizzle */
    return d->intx + pcie->line_count[line] * 2 + gic->level[35 + line] * 4 + ((d->config[0x06] >> 3) & 1) * 8;
}
static void test_interrupts(void) {
    HvPciDev *d = &gpu->vio.pci;
    const uint8_t *c = d->config;
    check("an MSI-X capability heads the list", c[0x34] == MSIX && c[MSIX] == 0x11, 1);
    check("and leads to the virtio ones", c[MSIX + 1], 0x40);
    check("it lies past the last virtio capability", c[0x71] == 0 && MSIX >= 0x70 + 20, 1);
    check("table size 3", (c[MSIX + 2] | c[MSIX + 3] << 8) & 0x7ff, 2);
    check("MSI-X starts disabled", hv_pci_msix_enabled(d), 0);
    check("table at the start of BAR 1", rd32(c + MSIX + 4), 1);
    check("pending bits in BAR 1", rd32(c + MSIX + 8), 0x800 | 1);
    check("BAR 1 is 4 KB of 32-bit memory", d->bar_size[1] == 0x1000 && !d->bar64[1], 1);

    /* INTx: no interrupt for a queue whose driver asked for none. */
    cfg_write16(0x04, 0x0006);
    bar_read(ISR, 1);
    set_avail_flags(0, 1);
    display_info();
    check("NO_INTERRUPT: no line", line_up(), 0);
    check("NO_INTERRUPT: no ISR bit", bar_read(ISR, 1), 0);
    set_avail_flags(0, 0);
    display_info();
    check("flag clear: the line, all the way to the interrupt controller", line_up(), 1 + 2 + 4 + 8);
    check("flag clear: ISR reads 1", bar_read(ISR, 1), 1);
    check("the read drops the line", line_up(), 0);
    check("the cursor queue's flag is its own", (set_avail_flags(1, 1), cursor_cmd(0x0301, 1, 1, 0, 0, 0), line_up()), 0);
    set_avail_flags(1, 0);

    /* The guest turns MSI-X on with an INTx interrupt still unread: the line goes down at once. */
    display_info();
    check("an unread INTx interrupt", line_up(), 15);
    cfg_write16(MSIX + 2, 0x8000);
    check("MSI-X on: the line is down everywhere but the status bit", line_up(), 8);
    check("the unread ISR bit is still the driver's to read", bar_read(ISR, 1), 1);
    check("and then nothing is left", line_up(), 0);

    /* MSI-X: each source is its vector's message. */
    hv_pcie_set_msi_doorbell(pcie, DOORBELL);
    for (uint32_t i = 0; i < 3; i++)
        d->msix_table[i] = (HvPciMsixEntry){.addr_lo = (uint32_t)DOORBELL, .addr_hi = 0, .data = 100 + i, .ctrl = 0};
    bar_write(COMMON + 0x10, 0, 2);
    bar_write(COMMON + 0x16, 0, 2);
    bar_write(COMMON + 0x1a, 1, 2);
    bar_write(COMMON + 0x16, 1, 2);
    bar_write(COMMON + 0x1a, 2, 2);
    bar_write(COMMON + 0x16, 0, 2);
    gic->latch[100] = gic->latch[101] = gic->latch[102] = 0;
    display_info();
    check("MSI-X: the control queue's message", gic->latch[101], 1);
    check("MSI-X: nobody else's", gic->latch[100] | gic->latch[102], 0);
    check("MSI-X: no line, no status bit", line_up(), 0);
    check("MSI-X: the ISR register stays clear", gpu->vio.isr, 0);
    cursor_cmd(0x0301, 2, 2, 0, 0, 0);
    check("MSI-X: the cursor queue's message", gic->latch[102], 1);
    hv_fb_display_request_size(fb, 1000, 700);
    check("MSI-X: a config change is the config vector's message", gic->latch[100], 1);
    check("MSI-X: still no line and no ISR bit", line_up() == 0 && gpu->vio.isr == 0, 1);
    gic->latch[101] = 0;
    set_avail_flags(0, 1);
    display_info();
    check("MSI-X: NO_INTERRUPT holds for messages too", gic->latch[101], 0);
    set_avail_flags(0, 0);
    bar_write(COMMON + 0x1a, 0xffff, 2);
    display_info();
    check("MSI-X: a queue with no vector sends nothing", gic->latch[101] == 0 && line_up() == 0, 1);
    bar_write(COMMON + 0x1a, 1, 2);

    /* The function mask holds a message back; lifting it sends that message once. */
    cfg_write16(MSIX + 2, 0xc000);
    display_info();
    check("function mask: the message waits in the pending bits", gic->latch[101] == 0 && d->msix_pba == 2, 1);
    cfg_write16(MSIX + 2, 0x8000);
    check("mask lifted: the message goes", gic->latch[101] == 1 && d->msix_pba == 0, 1);
    gic->latch[101] = 0;
    cfg_write16(MSIX + 2, 0x8000);
    check("and only once", gic->latch[101], 0);

    /* The guest turns MSI-X off again, with no device reset in between: what went out as messages must not
       come back as a line. */
    display_info();
    hv_fb_display_request_size(fb, 1002, 702);
    cfg_write16(MSIX + 2, 0x0000);
    check("MSI-X off: no line, no status bit, no ISR bit", line_up() == 0 && gpu->vio.isr == 0, 1);
    display_info();
    check("and INTx works again", line_up(), 15);
    bar_read(ISR, 1);

    /* A vector the driver masked, then a device reset: the pending message is not for the next driver. */
    cfg_write16(MSIX + 2, 0x8000);
    bar_write(COMMON + 0x1a, 1, 2);
    d->msix_table[1].ctrl = 1;
    gic->latch[101] = 0;
    display_info();
    check("a masked vector waits in the pending bits", gic->latch[101] == 0 && d->msix_pba == 2, 1);
    bar_write(COMMON + 0x14, 0, 1);
    check("a device reset drops what was pending", d->msix_pba, 0);
    d->msix_table[1].ctrl = 0;
    cfg_write16(MSIX + 2, 0x0000);
    driver_init();

    /* Bus mastering off: no running driver. */
    check("bus master on: the status is kept", bar_read(COMMON + 0x14, 1), 1 | 2 | 4 | 8);
    cfg_write16(0x04, 0x0002);
    check("bus master off: DRIVER_OK goes", bar_read(COMMON + 0x14, 1), 1 | 2 | 8);
    hv_fb_display_request_size(fb, 1280, 900);
    check("bus master off: a config change raises nothing", line_up() == 0 && gpu->vio.isr == 0, 1);
    hv_dev_lock(&m);
    hv_virtio_interrupt(&gpu->vio, 0); /* a completion that was still on its way */
    hv_dev_unlock(&m);
    check("bus master off: a late completion raises nothing", line_up() == 0 && gpu->vio.isr == 0, 1);
    uint16_t used_before, used_after, idx = (uint16_t)(avail_idx[0] + 1);
    memcpy(&used_before, host(RING_PA(0) + 0x2000) + 2, 2);
    memcpy(host(RING_PA(0) + 0x1000) + 2, &idx, 2);
    bar_write(NOTIFY, 0, 2);
    memcpy(&used_after, host(RING_PA(0) + 0x2000) + 2, 2);
    check("bus master off: a kick does nothing", used_after, used_before);
    cfg_write16(0x04, 0x0006);
    check("bus master back: the driver has to start the device again", bar_read(COMMON + 0x14, 1) & 4, 0);
    driver_init();
    bar_write(0x2000 + 4, 1, 4);
    check("and then it works", create_2d(9, 2, 16, 16), 0x1100);
    check("its resource can go", simple(0x0102, 9), 0x1100);
    bar_read(ISR, 1);
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
    bar_write(COMMON + 0x10, 1, 2);
    hv_virtio_gpu_reset(gpu);
    check("explicit reset", gpu->host_bytes, 0);
    check("explicit reset clears the status", bar_read(COMMON + 0x14, 1), 0);
    check("explicit reset unmaps vectors", bar_read(COMMON + 0x10, 2), 0xffff);
    bar_write(COMMON + 0x16, 0, 2);
    check("explicit reset disables queues", bar_read(COMMON + 0x1c, 2), 0);
    driver_init(); /* as the next OS's driver would */
}

/* A guest that goes on showing another size than it was told is told again: Windows lets an announcement go by
   while a session is starting, and the same size is never announced twice otherwise. */
static void tick_at(uint64_t t) {
    hv_dev_lock(&m);
    hv_virtio_gpu_tick(gpu, t);
    hv_dev_unlock(&m);
}
static uint32_t event_and_clear(void) {
    uint32_t ev = (uint32_t)bar_read(0x2000, 4);
    bar_read(ISR, 1);
    bar_write(0x2000 + 4, 1, 4);
    return ev;
}
static void test_told_again(void) {
    const uint64_t S = HV_VGPU_TOLD_AGAIN_TICKS / 3, T = 1000 * S; /* a second of host ticks; some time after boot */
    hv_fb_display_request_size(fb, 800, 600);
    event_and_clear();
    check("create 800x600", create_2d(30, 2, 800, 600), 0x1100);
    check("scanout 800x600", set_scanout(0, 30, 0, 0, 800, 600), 0x1100);
    tick_at(T);
    check("showing the size it was told: nothing to say", event_and_clear(), 0);

    hv_fb_display_request_size(fb, 1000, 700);
    check("a new size is announced", event_and_clear(), 1);
    tick_at(T + S); /* the wait starts with this tick */
    tick_at(T + 3 * S);
    check("not again before three seconds", event_and_clear(), 0);
    check("nor counted", gpu->told_again, 0);
    tick_at(T + 4 * S);
    check("told again after three seconds at the old size", bar_read(0x2000, 4), 1);
    check("with an interrupt", gpu->vio.pci.intx, 1);
    check("which reads as a config change", bar_read(ISR, 1), 3);
    event_and_clear();
    check("counted", gpu->told_again, 1);
    tick_at(T + 6 * S);
    check("the next wait is three seconds too", event_and_clear(), 0);
    tick_at(T + 7 * S);
    check("told a second time", event_and_clear(), 1);

    check("create 1000x700", create_2d(31, 2, 1000, 700), 0x1100);
    check("the guest follows", set_scanout(0, 31, 0, 0, 1000, 700), 0x1100);
    tick_at(T + 20 * S);
    check("nothing more once it shows the size", event_and_clear(), 0);
    check("and no wait is left running", gpu->told_at, 0);

    check("create 1000x600", create_2d(32, 2, 1000, 600), 0x1100);
    check("the guest goes to another height on its own", set_scanout(0, 32, 0, 0, 1000, 600), 0x1100);
    tick_at(T + 30 * S);
    check("which starts a new wait, not an announcement", event_and_clear(), 0);
    tick_at(T + 33 * S);
    check("then it is told again", event_and_clear(), 1);
    uint32_t told = 0;
    for (uint64_t t = T + 36 * S; t < T + 200 * S; t += 3 * S) {
        tick_at(t);
        told += event_and_clear();
    }
    check("but not for ever", gpu->told_again, HV_VGPU_TOLD_AGAIN_MAX);
    check("each of those was announced", told, HV_VGPU_TOLD_AGAIN_MAX - 3);

    hv_fb_display_request_size(fb, 1002, 702);
    check("another size is announced", event_and_clear(), 1);
    check("and starts the count over", gpu->told_again, 0);
    tick_at(T + 300 * S);
    tick_at(T + 303 * S);
    check("told again for the new size", event_and_clear(), 1);

    check("no scanout", set_scanout(0, 0, 0, 0, 0, 0), 0x1100);
    tick_at(T + 400 * S);
    tick_at(T + 403 * S);
    check("a guest showing nothing is not told again", event_and_clear(), 0);

    check("scanout 800x600 once more", set_scanout(0, 30, 0, 0, 800, 600), 0x1100);
    tick_at(T + 500 * S);
    bar_write(COMMON + 0x14, 0, 1); /* the driver resets the device */
    check("a driver starting over starts the count over", gpu->told_again + gpu->told_at, 0);
    driver_init();
    hv_fb_display_request_size(fb, 1280, 900);
    event_and_clear();
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

/* A value to the thousandth of a drawable pixel, so fractions compare exactly. */
/* Thousandths of a pixel. A negative place is never right, and must not pass for zero. */
static uint64_t milli(double fraction, double pixels) {
    if (fraction < 0) return UINT64_MAX;
    return (uint64_t)(fraction * pixels * 1000.0 + 0.5);
}

/* Where the presenter puts the picture: fitted into the place the app gives it, which stops short of a phone's
   rounded corners. */
static void test_fit(void) {
    HvFbRect whole = {0, 0, 1, 1};
    HvFbRect q = hv_fb_fit(1600, 1200, 800, 600, whole);
    check("same aspect fills: x", milli(q.x, 1600), 0);
    check("same aspect fills: w", milli(q.w, 1600), 1600000);
    check("same aspect fills: h", milli(q.h, 1200), 1200000);
    q = hv_fb_fit(2000, 1000, 800, 600, whole); /* a wider drawable: bars left and right */
    check("pillarbox: w", milli(q.w, 2000), 1333333);
    check("pillarbox: x", milli(q.x, 2000), 333333);
    check("pillarbox: h", milli(q.h, 1000), 1000000);
    check("pillarbox: y", milli(q.y, 1000), 0);
    q = hv_fb_fit(1000, 2000, 800, 600, whole); /* a taller one: bars above and below */
    check("letterbox: h", milli(q.h, 2000), 750000);
    check("letterbox: y", milli(q.y, 2000), 625000);
    check("letterbox: x", milli(q.x, 1000), 0);
    check("letterbox: w", milli(q.w, 1000), 1000000);
    /* Bars above and below inside a place that is not the whole drawable: pixels 100 to 900 across and 400
       to 1400 down take an 800 by 600 picture at 100, 600. */
    HvFbRect inset = {0.1, 0.2, 0.8, 0.5};
    q = hv_fb_fit(1000, 2000, 800, 600, inset);
    check("letterbox in a place: x", milli(q.x, 1000), 100000);
    check("letterbox in a place: y", milli(q.y, 2000), 600000);
    check("letterbox in a place: w", milli(q.w, 1000), 800000);
    check("letterbox in a place: h", milli(q.h, 2000), 600000);
    /* The phone: a 2853 by 2007 drawable, the picture's place 49 pixels in from every edge except 50 on the
       left (the pixel grid), and Windows at 1376 by 954: exactly two drawable pixels per guest pixel. */
    HvFbRect place = {50.0 / 2853, 49.0 / 2007, 2752.0 / 2853, 1908.0 / 2007};
    q = hv_fb_fit(2853, 2007, 1376, 954, place);
    check("phone: x", milli(q.x, 2853), 50000);
    check("phone: y", milli(q.y, 2007), 49000);
    check("phone: w is two pixels per guest pixel", milli(q.w, 2853), 2752000);
    check("phone: h", milli(q.h, 2007), 1908000);
    /* The firmware's 4:3 screen in the same place: centred inside it, not in the drawable. */
    q = hv_fb_fit(2853, 2007, 1024, 768, place);
    check("firmware: h fills the place", milli(q.h, 2007), 1908000);
    check("firmware: w keeps 4:3", milli(q.w, 2853), 2544000);
    check("firmware: centred in the place", milli(q.x, 2853), 50000 + (2752000 - 2544000) / 2);
    check("firmware: y is the place's", milli(q.y, 2007), 49000);
    /* Nothing to fit, or nowhere to fit it: the place comes back as given. */
    q = hv_fb_fit(2853, 2007, 0, 954, place);
    check("no picture: unchanged", milli(q.w, 2853), 2752000);
    q = hv_fb_fit(0, 0, 1376, 954, whole);
    check("no drawable: unchanged", milli(q.w, 1000), 1000000);
    q = hv_fb_fit(2853, 2007, 1376, 0, place);
    check("no picture height: unchanged", milli(q.h, 2007), 1908000);
    q = hv_fb_fit(2853, 2007, 1376, 954, (HvFbRect){0.1, 0.1, 0, 0.5});
    check("a place with no width: unchanged", milli(q.h, 2007), 1003500);
    q = hv_fb_fit(2853, 2007, 1376, 954, (HvFbRect){0.1, 0.1, 0.5, 0});
    check("a place with no height: unchanged", milli(q.w, 2853), 1426500);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    hv_machine_init(&m, 1 << 20);
    ram = aligned_alloc(65536, RAM_SIZE);
    memset(ram, 0, RAM_SIZE);
    hv_machine_add_ram(&m, RAM_PA, RAM_SIZE, ram, 0);
    gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    pcie = hv_pcie_create(&m, gic, 0x4010000000ull, 0x10000000, 0x10000000, 0x2eff0000, 35);
    hv_pcie_set_gpex_swizzle(pcie, 1);
    fb = hv_fb_display_create(&m, NULL);
    gpu = hv_virtio_gpu_create(&m, pcie, fb);
    if (!gpu) return 1;

    test_identity();
    test_msix_vectors();
    driver_init();
    test_display_info();
    test_edid();
    test_errors_and_fences();
    test_scanout_flow();
    test_counters();
    test_formats();
    test_cursor();
    test_resize();
    test_fit();
    test_interrupts();
    test_reset();
    test_told_again();
    test_threads();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
