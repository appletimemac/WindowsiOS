#include "virtio_gpu.h"

#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTRLQ 0
#define CURSORQ 1

#define F_EDID (1ull << 1)
#define EVENT_DISPLAY 1u

#define FLAG_FENCE 1u
#define FLAG_INFO_RING_IDX 2u

#define CMD_GET_DISPLAY_INFO 0x0100
#define CMD_RESOURCE_CREATE_2D 0x0101
#define CMD_RESOURCE_UNREF 0x0102
#define CMD_SET_SCANOUT 0x0103
#define CMD_RESOURCE_FLUSH 0x0104
#define CMD_TRANSFER_TO_HOST_2D 0x0105
#define CMD_RESOURCE_ATTACH_BACKING 0x0106
#define CMD_RESOURCE_DETACH_BACKING 0x0107
#define CMD_GET_EDID 0x010a
#define CMD_UPDATE_CURSOR 0x0300
#define CMD_MOVE_CURSOR 0x0301

#define RESP_OK_NODATA 0x1100
#define RESP_OK_DISPLAY_INFO 0x1101
#define RESP_OK_EDID 0x1104
#define RESP_ERR_UNSPEC 0x1200
#define RESP_ERR_OUT_OF_MEMORY 0x1201
#define RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define RESP_ERR_INVALID_PARAMETER 0x1205

/* Pixel formats; the names give the byte order in memory. */
#define FMT_B8G8R8A8 1
#define FMT_B8G8R8X8 2
#define FMT_A8R8G8B8 3
#define FMT_X8R8G8B8 4

#define HDR_SIZE 24
#define MAX_RES_DIM 16384
#define MAX_RES_BYTES (64ull << 20)
#define MAX_HOST_BYTES (192ull << 20)
#define MAX_BACKING_ENTRIES 16384
#define REQ_MAX (HDR_SIZE + 8 + MAX_BACKING_ENTRIES * 16)
#define DISPLAY_INFO_SIZE (HDR_SIZE + 16 * 24)
#define EDID_RESP_SIZE (HDR_SIZE + 8 + 1024)
#define MIN_WIDTH 640
#define MIN_HEIGHT 480
#define MAX_WIDTH 3840
#define MAX_HEIGHT 2160

static int logging(void) {
    static int on = -1;
    if (on < 0) on = getenv("HV_VGPU_LOG") != NULL;
    return on;
}

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void wr32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static void wr64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}

/* ---- EDID ---- */

static void put_dtd(uint8_t *d, uint32_t w, uint32_t h, uint32_t wmm, uint32_t hmm) {
    /* CVT reduced blanking at 60 Hz */
    const uint32_t hblank = 160, hfp = 48, hsync = 32, vfp = 3, vbp_min = 6, refresh = 60;
    uint32_t vsync = 10;
    if ((uint64_t)w * 3 == (uint64_t)h * 4) vsync = 4;
    else if ((uint64_t)w * 9 == (uint64_t)h * 16) vsync = 5;
    else if ((uint64_t)w * 10 == (uint64_t)h * 16) vsync = 6;
    else if ((uint64_t)w * 4 == (uint64_t)h * 5 || (uint64_t)w * 9 == (uint64_t)h * 15) vsync = 7;
    double h_period_us = (1000000.0 / refresh - 460.0) / h;
    uint32_t vbi = (uint32_t)(460.0 / h_period_us) + 1;
    uint32_t min_vbi = vfp + vsync + vbp_min;
    if (vbi < min_vbi) vbi = min_vbi;
    uint64_t clock_hz = (uint64_t)(w + hblank) * (h + vbi) * refresh;
    clock_hz -= clock_hz % 250000; /* CVT-RB steps of 0.25 MHz */
    uint32_t clock = (uint32_t)(clock_hz / 10000);
    if (clock > 0xffff) clock = 0xffff;
    d[0] = clock & 0xff;
    d[1] = clock >> 8;
    d[2] = w & 0xff;
    d[3] = hblank & 0xff;
    d[4] = (uint8_t)((w >> 8) << 4 | (hblank >> 8));
    d[5] = h & 0xff;
    d[6] = vbi & 0xff;
    d[7] = (uint8_t)((h >> 8) << 4 | (vbi >> 8));
    d[8] = hfp & 0xff;
    d[9] = hsync & 0xff;
    d[10] = (uint8_t)((vfp & 0xf) << 4 | (vsync & 0xf));
    d[11] = (uint8_t)((hfp >> 8) << 6 | (hsync >> 8) << 4 | (vfp >> 4) << 2 | (vsync >> 4));
    d[12] = wmm & 0xff;
    d[13] = hmm & 0xff;
    d[14] = (uint8_t)((wmm >> 8) << 4 | (hmm >> 8));
    d[15] = d[16] = 0;
    d[17] = 0x1a; /* digital separate sync, +HSync -VSync (reduced blanking) */
}

void hv_virtio_gpu_build_edid(uint8_t e[128], uint32_t w, uint32_t h) {
    memset(e, 0, 128);
    static const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    memcpy(e, header, 8);
    const char *mfg = "WIO"; /* unregistered PNP id */
    uint16_t id = (uint16_t)((mfg[0] - '@') << 10 | (mfg[1] - '@') << 5 | (mfg[2] - '@'));
    e[8] = id >> 8;
    e[9] = id & 0xff;
    e[10] = 0x01; /* product code, little endian */
    e[12] = 0x01; /* serial */
    e[16] = 1;    /* week */
    e[17] = 2026 - 1990;
    e[18] = 1; /* EDID 1.4 */
    e[19] = 4;
    e[20] = 0xa5; /* digital, 8 bits per colour, DisplayPort */
    uint32_t wmm = hv_vgpu_phys_mm(w), hmm = hv_vgpu_phys_mm(h);
    e[21] = (uint8_t)((wmm + 5) / 10);
    e[22] = (uint8_t)((hmm + 5) / 10);
    e[23] = 120;  /* gamma 2.2 */
    e[24] = 0x07; /* sRGB, preferred timing is the first descriptor, continuous frequency */
    static const uint8_t srgb[10] = {0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54};
    memcpy(e + 25, srgb, 10);
    for (int i = 0; i < 8; i++) e[38 + 2 * i] = e[39 + 2 * i] = 0x01; /* standard timings: unused */
    put_dtd(e + 54, w, h, wmm, hmm);
    uint8_t *r = e + 72; /* monitor range limits */
    r[3] = 0xfd;
    r[5] = 24;  /* vertical Hz */
    r[6] = 75;
    r[7] = 30;  /* horizontal kHz */
    r[8] = 250;
    r[9] = 66;  /* max pixel clock, 10 MHz units */
    r[10] = 0x01; /* range limits only */
    r[11] = 0x0a;
    memset(r + 12, 0x20, 6);
    uint8_t *n = e + 90; /* monitor name */
    n[3] = 0xfc;
    memcpy(n + 5, "WindowsiOS\n  ", 13);
    e[108 + 3] = 0x10; /* dummy descriptor */
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++) sum = (uint8_t)(sum + e[i]);
    e[127] = (uint8_t)(0x100 - sum);
}

/* ---- resources ---- */

static HvVgpuRes *res_find(HvVirtioGpu *g, uint32_t id) {
    if (!id) return NULL;
    for (int i = 0; i < HV_VGPU_MAX_RESOURCES; i++)
        if (g->res[i].id == id) return &g->res[i];
    return NULL;
}

static void scanout_disable(HvVirtioGpu *g) {
    g->scanout_res = 0;
    hv_fb_display_set_scanout(g->fb, NULL, 0, 0, 0);
}

static void backing_free(HvVgpuRes *r) {
    free(r->seg);
    r->seg = NULL;
    r->nseg = 0;
    r->backing_bytes = 0;
}

static void res_free(HvVirtioGpu *g, HvVgpuRes *r) {
    /* the display must stop reading the pixels before they go */
    if (g->scanout_res == r->id) scanout_disable(g);
    g->host_bytes -= (uint64_t)r->stride * r->height;
    backing_free(r);
    free(r->pixels);
    memset(r, 0, sizeof *r);
}

/* Copy up to len bytes of the backing starting at off. Returns how many were there. */
static uint64_t backing_read(const HvVgpuRes *r, uint64_t off, uint8_t *dst, uint64_t len) {
    if (off >= r->backing_bytes) return 0;
    if (len > r->backing_bytes - off) len = r->backing_bytes - off;
    uint32_t lo = 0, hi = r->nseg - 1;
    while (lo < hi) { /* last entry starting at or before off */
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (r->seg[mid].start <= off) lo = mid;
        else hi = mid - 1;
    }
    uint64_t done = 0;
    for (uint32_t i = lo; i < r->nseg && done < len; i++) {
        const HvVgpuSeg *s = &r->seg[i];
        uint64_t skip = off + done - s->start;
        uint64_t n = s->len - skip;
        if (n > len - done) n = len - done;
        memcpy(dst + done, s->ptr + skip, n);
        done += n;
    }
    return done;
}

/* The part of the rectangle (x, y, w, h) of resource r that is on the scanout, as [x0, x1) x [y0, y1) in resource pixels.
   0 when r is not the scanout resource or the two do not overlap. */
static int scanout_clip(const HvVirtioGpu *g, const HvVgpuRes *r, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        uint64_t *cx0, uint64_t *cy0, uint64_t *cx1, uint64_t *cy1) {
    if (r->id != g->scanout_res) return 0;
    uint64_t x0 = x > g->scanout_x ? x : g->scanout_x, y0 = y > g->scanout_y ? y : g->scanout_y;
    uint64_t x1 = (uint64_t)x + w, y1 = (uint64_t)y + h;
    if (x1 > (uint64_t)g->scanout_x + g->scanout_w) x1 = (uint64_t)g->scanout_x + g->scanout_w;
    if (y1 > (uint64_t)g->scanout_y + g->scanout_h) y1 = (uint64_t)g->scanout_y + g->scanout_h;
    if (x0 >= x1 || y0 >= y1) return 0;
    *cx0 = x0, *cy0 = y0, *cx1 = x1, *cy1 = y1;
    return 1;
}

/* Rectangle of resource r that is on the scanout, in scanout pixels, handed to the display as changed. */
static void mark_scanout_dirty(HvVirtioGpu *g, const HvVgpuRes *r, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint64_t x0, y0, x1, y1;
    if (scanout_clip(g, r, x, y, w, h, &x0, &y0, &x1, &y1))
        hv_fb_display_mark_dirty(g->fb, (uint32_t)(x0 - g->scanout_x), (uint32_t)(y0 - g->scanout_y), (uint32_t)(x1 - x0),
                                 (uint32_t)(y1 - y0));
}

/* Counters for a flush: only the part on the scanout counts, by size and by where its center falls. */
static void count_flush(HvVirtioGpu *g, const HvVgpuRes *r, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint64_t x0, y0, x1, y1;
    if (!scanout_clip(g, r, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    uint64_t area = (x1 - x0) * (y1 - y0);
    g->flush_px += area;
    if (area < 16384) g->rect_small++;
    else if (area < 262144) g->rect_mid++;
    else g->rect_big++;
    uint64_t gx = ((x0 + x1) / 2 - g->scanout_x) * HV_VGPU_GRID_W / g->scanout_w;
    uint64_t gy = ((y0 + y1) / 2 - g->scanout_y) * HV_VGPU_GRID_H / g->scanout_h;
    if (gx >= HV_VGPU_GRID_W) gx = HV_VGPU_GRID_W - 1;
    if (gy >= HV_VGPU_GRID_H) gy = HV_VGPU_GRID_H - 1;
    g->grid[gy * HV_VGPU_GRID_W + gx] += area;
}

static int rect_in(const HvVgpuRes *r, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return (uint64_t)x + w <= r->width && (uint64_t)y + h <= r->height;
}

/* ---- control queue commands: each returns the response type and may fill resp (header already zeroed) ---- */

static uint32_t cmd_display_info(HvVirtioGpu *g, uint8_t *resp, uint32_t *rlen) {
    uint8_t *m = resp + HDR_SIZE; /* pmodes[0]: rect, enabled, flags */
    wr32(m + 8, g->width);
    wr32(m + 12, g->height);
    wr32(m + 16, 1);
    *rlen = DISPLAY_INFO_SIZE;
    return RESP_OK_DISPLAY_INFO;
}

static uint32_t cmd_get_edid(HvVirtioGpu *g, const uint8_t *req, uint32_t n, uint8_t *resp, uint32_t *rlen) {
    if (n < HDR_SIZE + 8) return RESP_ERR_INVALID_PARAMETER;
    if (rd32(req + HDR_SIZE) != 0) return RESP_ERR_INVALID_PARAMETER;
    wr32(resp + HDR_SIZE, 128);
    memcpy(resp + HDR_SIZE + 8, g->edid, 128);
    *rlen = EDID_RESP_SIZE;
    return RESP_OK_EDID;
}

static uint32_t cmd_create_2d(HvVirtioGpu *g, const uint8_t *req, uint32_t n) {
    if (n < HDR_SIZE + 16) return RESP_ERR_INVALID_PARAMETER;
    uint32_t id = rd32(req + 24), format = rd32(req + 28), w = rd32(req + 32), h = rd32(req + 36);
    if (!id || res_find(g, id)) return RESP_ERR_INVALID_RESOURCE_ID;
    int alpha, swap;
    switch (format) {
    case FMT_B8G8R8A8: alpha = 1, swap = 0; break;
    case FMT_B8G8R8X8: alpha = 0, swap = 0; break;
    case FMT_A8R8G8B8: alpha = 1, swap = 1; break;
    case FMT_X8R8G8B8: alpha = 0, swap = 1; break;
    default: return RESP_ERR_INVALID_PARAMETER;
    }
    if (!w || !h || w > MAX_RES_DIM || h > MAX_RES_DIM) return RESP_ERR_INVALID_PARAMETER;
    uint64_t bytes = (uint64_t)w * 4 * h;
    if (bytes > MAX_RES_BYTES || g->host_bytes + bytes > MAX_HOST_BYTES) return RESP_ERR_OUT_OF_MEMORY;
    HvVgpuRes *r = NULL;
    for (int i = 0; i < HV_VGPU_MAX_RESOURCES && !r; i++)
        if (!g->res[i].id) r = &g->res[i];
    if (!r) return RESP_ERR_OUT_OF_MEMORY;
    uint8_t *px = calloc(1, (size_t)bytes); /* zero pages cost nothing until the guest draws */
    if (!px) return RESP_ERR_OUT_OF_MEMORY;
    *r = (HvVgpuRes){.id = id, .format = format, .width = w, .height = h, .stride = w * 4, .has_alpha = alpha, .swap = swap, .pixels = px};
    g->host_bytes += bytes;
    if (logging()) fprintf(stderr, "virtio-gpu: resource %u format %u %ux%u\n", id, format, w, h);
    return RESP_OK_NODATA;
}

static uint32_t cmd_unref(HvVirtioGpu *g, const uint8_t *req, uint32_t n) {
    if (n < HDR_SIZE + 8) return RESP_ERR_INVALID_PARAMETER;
    HvVgpuRes *r = res_find(g, rd32(req + 24));
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    res_free(g, r);
    return RESP_OK_NODATA;
}

static uint32_t cmd_set_scanout(HvVirtioGpu *g, const uint8_t *req, uint32_t n) {
    if (n < HDR_SIZE + 24) return RESP_ERR_INVALID_PARAMETER;
    uint32_t x = rd32(req + 24), y = rd32(req + 28), w = rd32(req + 32), h = rd32(req + 36);
    uint32_t sid = rd32(req + 40), rid = rd32(req + 44);
    if (sid != 0) return RESP_ERR_INVALID_SCANOUT_ID;
    if (rid == 0) {
        scanout_disable(g);
        return RESP_OK_NODATA;
    }
    HvVgpuRes *r = res_find(g, rid);
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    if (!w || !h || !rect_in(r, x, y, w, h)) return RESP_ERR_INVALID_PARAMETER;
    g->scanout_res = rid;
    g->scanout_x = x, g->scanout_y = y, g->scanout_w = w, g->scanout_h = h;
    hv_fb_display_set_scanout(g->fb, r->pixels + (uint64_t)y * r->stride + (uint64_t)x * 4, w, h, r->stride);
    if (logging()) fprintf(stderr, "virtio-gpu: scanout is resource %u at %u,%u %ux%u\n", rid, x, y, w, h);
    return RESP_OK_NODATA;
}

static uint32_t cmd_flush(HvVirtioGpu *g, const uint8_t *req, uint32_t n) {
    if (n < HDR_SIZE + 24) return RESP_ERR_INVALID_PARAMETER;
    uint32_t x = rd32(req + 24), y = rd32(req + 28), w = rd32(req + 32), h = rd32(req + 36);
    HvVgpuRes *r = res_find(g, rd32(req + 40));
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    if (!rect_in(r, x, y, w, h)) return RESP_ERR_INVALID_PARAMETER;
    g->flushes++;
    count_flush(g, r, x, y, w, h);
    mark_scanout_dirty(g, r, x, y, w, h);
    return RESP_OK_NODATA;
}

static uint32_t cmd_transfer(HvVirtioGpu *g, const uint8_t *req, uint32_t n) {
    if (n < HDR_SIZE + 32) return RESP_ERR_INVALID_PARAMETER;
    uint32_t x = rd32(req + 24), y = rd32(req + 28), w = rd32(req + 32), h = rd32(req + 36);
    uint64_t off = rd64(req + 40);
    HvVgpuRes *r = res_find(g, rd32(req + 48));
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    if (!rect_in(r, x, y, w, h)) return RESP_ERR_INVALID_PARAMETER;
    if (!r->seg) return RESP_ERR_UNSPEC;
    if (!w || !h) return RESP_OK_NODATA;
    if (off >= r->backing_bytes) return RESP_ERR_INVALID_PARAMETER;
    g->transfers++;
    /* As QEMU does it: row k of the rectangle comes from backing offset + k * stride, and goes to
       (x, y + k) of the host copy, so the guest's offset is normally y * stride + x * 4. What lies past the
       end of the backing is left alone. */
    uint64_t row = (uint64_t)w * 4;
    uint8_t *dst = r->pixels + (uint64_t)y * r->stride + (uint64_t)x * 4;
    uint64_t copied = 0, t0 = mach_absolute_time();
    if (row == r->stride) {
        copied = backing_read(r, off, dst, row * h);
    } else {
        for (uint32_t k = 0; k < h; k++) {
            uint64_t got = backing_read(r, off + (uint64_t)k * r->stride, dst + (uint64_t)k * r->stride, row);
            if (got == 0) break;
            copied += got;
        }
    }
    if (r->swap) {
        for (uint32_t k = 0; k < h; k++) {
            uint32_t *p = (uint32_t *)(dst + (uint64_t)k * r->stride);
            for (uint32_t i = 0; i < w; i++) p[i] = __builtin_bswap32(p[i]);
        }
    }
    g->xfer_bytes += copied;
    g->xfer_ticks += mach_absolute_time() - t0; /* the channel swap is part of getting the pixels into the host copy */
    mark_scanout_dirty(g, r, x, y, w, h);
    return RESP_OK_NODATA;
}

static uint32_t cmd_attach_backing(HvVirtioGpu *g, const uint8_t *req, uint32_t n) {
    if (n < HDR_SIZE + 8) return RESP_ERR_INVALID_PARAMETER;
    HvVgpuRes *r = res_find(g, rd32(req + 24));
    uint32_t count = rd32(req + 28);
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    if (r->seg) return RESP_ERR_UNSPEC;
    if (!count || count > MAX_BACKING_ENTRIES || n < HDR_SIZE + 8 + (uint64_t)count * 16) return RESP_ERR_INVALID_PARAMETER;
    HvVgpuSeg *seg = calloc(count, sizeof *seg);
    if (!seg) return RESP_ERR_OUT_OF_MEMORY;
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = req + HDR_SIZE + 8 + (size_t)i * 16;
        uint64_t addr = rd64(e);
        uint32_t len = rd32(e + 8);
        uint8_t *p = len ? hv_phys_ptr(g->vio.m, addr, len) : NULL;
        if (!p) {
            free(seg);
            return RESP_ERR_INVALID_PARAMETER;
        }
        seg[i] = (HvVgpuSeg){.ptr = p, .start = total, .len = len};
        total += len;
    }
    r->seg = seg;
    r->nseg = count;
    r->backing_bytes = total;
    return RESP_OK_NODATA;
}

static uint32_t cmd_detach_backing(HvVirtioGpu *g, const uint8_t *req, uint32_t n) {
    if (n < HDR_SIZE + 8) return RESP_ERR_INVALID_PARAMETER;
    HvVgpuRes *r = res_find(g, rd32(req + 24));
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    backing_free(r);
    return RESP_OK_NODATA;
}

static uint32_t dispatch(HvVirtioGpu *g, uint32_t cmd, const uint8_t *req, uint32_t n, uint8_t *resp, uint32_t *rlen) {
    switch (cmd) {
    case CMD_GET_DISPLAY_INFO: return cmd_display_info(g, resp, rlen);
    case CMD_GET_EDID: return cmd_get_edid(g, req, n, resp, rlen);
    case CMD_RESOURCE_CREATE_2D: return cmd_create_2d(g, req, n);
    case CMD_RESOURCE_UNREF: return cmd_unref(g, req, n);
    case CMD_SET_SCANOUT: return cmd_set_scanout(g, req, n);
    case CMD_RESOURCE_FLUSH: return cmd_flush(g, req, n);
    case CMD_TRANSFER_TO_HOST_2D: return cmd_transfer(g, req, n);
    case CMD_RESOURCE_ATTACH_BACKING: return cmd_attach_backing(g, req, n);
    case CMD_RESOURCE_DETACH_BACKING: return cmd_detach_backing(g, req, n);
    default: return RESP_ERR_UNSPEC; /* capsets, blobs, 3D: not offered */
    }
}

static uint32_t readable(const HvVirtqElem *e) {
    uint32_t n = 0;
    for (int s = 0; s < e->nseg; s++)
        if (!e->seg[s].write) n += e->seg[s].len;
    return n;
}

static void control(HvVirtioGpu *g, const HvVirtqElem *e) {
    HvVirtio *v = &g->vio;
    uint32_t avail = readable(e);
    uint32_t n = hv_virtq_read(v, e, 0, g->req, avail < REQ_MAX ? avail : REQ_MAX);
    uint8_t resp[EDID_RESP_SIZE];
    memset(resp, 0, sizeof resp);
    uint32_t type = RESP_ERR_UNSPEC, rlen = HDR_SIZE, cmd = 0;
    uint32_t flags = 0, ctx = 0;
    uint64_t fence = 0;
    uint8_t ring = 0;
    if (n >= HDR_SIZE) {
        cmd = rd32(g->req);
        flags = rd32(g->req + 4);
        fence = rd64(g->req + 8);
        ctx = rd32(g->req + 16);
        ring = g->req[20];
        g->cmds++;
        type = dispatch(g, cmd, g->req, n, resp, &rlen);
    }
    wr32(resp, type);
    if (flags & FLAG_FENCE) {
        /* Commands run to completion in order here, so the fence has passed by the time it is reported. */
        wr32(resp + 4, flags & (FLAG_FENCE | FLAG_INFO_RING_IDX));
        wr64(resp + 8, fence);
        wr32(resp + 16, ctx);
        if (flags & FLAG_INFO_RING_IDX) resp[20] = ring;
    }
    if (type >= RESP_ERR_UNSPEC) {
        g->errors++;
        static int said;
        if (said++ < 20 || logging())
            fprintf(stderr, "virtio-gpu: command %#x (%u bytes) -> error %#x\n", cmd, n, type);
    } else if (logging()) {
        fprintf(stderr, "virtio-gpu: command %#x (%u bytes) -> %#x\n", cmd, n, type);
    }
    uint32_t room = hv_virtq_writable(e);
    if (rlen > room) rlen = room;
    if (rlen) hv_virtq_write(v, e, 0, resp, rlen);
    hv_virtq_push(v, CTRLQ, e, rlen);
}

/* ---- cursor queue: no responses ---- */

static void cursor_command(HvVirtioGpu *g, const HvVirtqElem *e) {
    uint8_t req[HDR_SIZE + 32];
    uint32_t n = hv_virtq_read(&g->vio, e, 0, req, sizeof req);
    if (n < HDR_SIZE + 28) return;
    uint32_t cmd = rd32(req);
    uint32_t scanout = rd32(req + 24);
    int32_t x = (int32_t)rd32(req + 28), y = (int32_t)rd32(req + 32);
    uint32_t rid = rd32(req + 40), hot_x = rd32(req + 44), hot_y = rd32(req + 48);
    if (scanout != 0) return;
    if (cmd == CMD_UPDATE_CURSOR) {
        if (!rid) {
            hv_fb_display_set_cursor_image(g->fb, NULL, 0, 0);
        } else {
            HvVgpuRes *r = res_find(g, rid);
            if (!r || r->width != HV_FB_CURSOR_DIM || r->height != HV_FB_CURSOR_DIM) return;
            uint32_t img[HV_FB_CURSOR_DIM * HV_FB_CURSOR_DIM];
            memcpy(img, r->pixels, sizeof img);
            if (!r->has_alpha)
                for (unsigned i = 0; i < HV_FB_CURSOR_DIM * HV_FB_CURSOR_DIM; i++) img[i] |= 0xff000000u;
            hv_fb_display_set_cursor_image(g->fb, img, hot_x, hot_y);
        }
        hv_fb_display_move_cursor(g->fb, x, y, rid != 0);
    } else if (cmd == CMD_MOVE_CURSOR) {
        hv_fb_display_move_cursor(g->fb, x, y, rid != 0);
    }
}

static void notify(HvVirtio *v, int q) {
    HvVirtioGpu *g = v->dev;
    HvVirtqElem *e = &g->elem;
    int any = 0, r;
    while ((r = hv_virtq_pop(v, q, e)) != 0) {
        if (r < 0) { /* a chain this model cannot follow: still give it back */
            hv_virtq_push(v, q, e, 0);
        } else if (q == CTRLQ) {
            control(g, e);
        } else {
            cursor_command(g, e);
            hv_virtq_push(v, q, e, 0);
        }
        any = 1;
    }
    if (any) hv_virtio_interrupt(v);
}

/* ---- device ---- */

static void set_events(HvVirtioGpu *g, uint32_t events) {
    wr32(g->vio.config, events);
}

static void do_reset(HvVirtio *v) {
    HvVirtioGpu *g = v->dev;
    if (!g) return;
    if (logging()) fprintf(stderr, "virtio-gpu: reset (%u resources, scanout %u)\n", (unsigned)(g->host_bytes != 0), g->scanout_res);
    scanout_disable(g);
    for (int i = 0; i < HV_VGPU_MAX_RESOURCES; i++)
        if (g->res[i].id) res_free(g, &g->res[i]);
    hv_fb_display_set_cursor_image(g->fb, NULL, 0, 0);
    hv_fb_display_move_cursor(g->fb, 0, 0, 0);
    set_events(g, 0);
}

void hv_virtio_gpu_reset(HvVirtioGpu *g) {
    if (!g) return;
    hv_dev_lock(g->vio.m);
    hv_virtio_reset(&g->vio); /* the transport too: queues, status and vectors (it calls do_reset) */
    hv_dev_unlock(g->vio.m);
}

static void config_write(HvVirtio *v, uint32_t off, uint64_t val, unsigned size) {
    HvVirtioGpu *g = v->dev;
    if (off < 4 || off >= 8) return; /* only events_clear is writable */
    unsigned shift = (off - 4) * 8;
    uint32_t mask = size >= 4 ? 0xffffffffu : (1u << (size * 8)) - 1;
    uint32_t clear = ((uint32_t)val & mask) << shift;
    set_events(g, rd32(v->config) & ~clear);
}

void hv_virtio_gpu_set_size(HvVirtioGpu *g, uint32_t w, uint32_t h) {
    if (!g) return;
    w = w < MIN_WIDTH ? MIN_WIDTH : w > MAX_WIDTH ? MAX_WIDTH : w;
    h = h < MIN_HEIGHT ? MIN_HEIGHT : h > MAX_HEIGHT ? MAX_HEIGHT : h;
    w &= ~1u;
    h &= ~1u;
    hv_dev_lock(g->vio.m);
    if (w != g->width || h != g->height) {
        g->width = w;
        g->height = h;
        hv_virtio_gpu_build_edid(g->edid, w, h);
        set_events(g, rd32(g->vio.config) | EVENT_DISPLAY);
        hv_virtio_config_changed(&g->vio);
        if (logging()) fprintf(stderr, "virtio-gpu: display size now %ux%u\n", w, h);
    }
    hv_dev_unlock(g->vio.m);
}

static void on_resize(void *opaque, uint32_t w, uint32_t h) {
    hv_virtio_gpu_set_size(opaque, w, h);
}

HvVirtioGpu *hv_virtio_gpu_create(HvMachine *m, HvPcie *bus, HvFbDisplay *fb) {
    HvVirtioGpu *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->req = malloc(REQ_MAX);
    if (!g->req) {
        free(g);
        return NULL;
    }
    g->fb = fb;
    HvVirtio *v = &g->vio;
    hv_virtio_init(v, m, 16, 0x038000, 2, F_EDID); /* display controller, other */
    v->pci.config[0x2e] = 0x00; /* SUBSYS_11001AF4: the primary hardware ID in viogpudo.inf */
    v->pci.config[0x2f] = 0x11;
    v->dev = g;
    v->notify = notify;
    v->config_write = config_write;
    v->reset = do_reset;
    v->msix_vectors = 3; /* config plus one per queue, as QEMU's virtio-gpu-pci; see HvVirtio */
    uint32_t scanouts = 1;
    memcpy(v->config + 8, &scanouts, 4); /* events_read, events_clear, num_scanouts, num_capsets = 0 */
    v->config_len = 16;
    g->width = HV_VGPU_DEFAULT_WIDTH;
    g->height = HV_VGPU_DEFAULT_HEIGHT;
    hv_virtio_gpu_build_edid(g->edid, g->width, g->height);
    hv_pcie_attach(bus, &v->pci);
    hv_fb_display_set_resize_handler(fb, on_resize, g);
    return g;
}
