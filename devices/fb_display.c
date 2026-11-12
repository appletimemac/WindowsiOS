#include "fb_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RAMFBCfg written by EDK2 QemuRamFbDxe via fw_cfg DMA to "etc/ramfb" (28 bytes, big-endian):
   struct RAMFBCfg {
       uint64_t addr;
       uint32_t fourcc;
       uint32_t flags;
       uint32_t width;
       uint32_t height;
       uint32_t stride;
   } __attribute__((packed));
*/

static uint32_t be32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static uint64_t be64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

static void on_ramfb_write(void *opaque, const uint8_t *data, uint32_t len) {
    HvFbDisplay *fb = opaque;
    if (len < 28) return;

    uint64_t addr = be64(data + 0);
    uint32_t fourcc = be32(data + 8);
    uint32_t width = be32(data + 16);
    uint32_t height = be32(data + 20);
    uint32_t stride = be32(data + 24);

    if (width < 64 || width > 8192 || height < 64 || height > 8192) return;
    if (stride == 0) stride = width * 4;
    if (stride < width * 2 || stride > 65536) return;

    uint64_t total_bytes = (uint64_t)stride * (height - 1) + (uint64_t)width * 4;
    void *host_ptr = hv_phys_ptr(fb->m, addr, total_bytes);
    if (!host_ptr) {
        fprintf(stderr, "winvm: ramfb ignored invalid PA 0x%llx (%ux%u stride %u)\n",
                (unsigned long long)addr, width, height, stride);
        return;
    }

    pthread_mutex_lock(&fb->mu);
    if (!fb->active || fb->fb_host != (const uint8_t *)host_ptr || fb->width != width || fb->height != height ||
        fb->stride != stride || fb->fourcc != fourcc)
        fb->generation++;
    fb->fb_pa = addr;
    fb->fb_host = (const uint8_t *)host_ptr;
    fb->width = width;
    fb->height = height;
    fb->stride = stride;
    fb->fourcc = fourcc;
    fb->active = 1;
    pthread_mutex_unlock(&fb->mu);

    fprintf(stderr, "winvm: ramfb configured %ux%u stride=%u fourcc=0x%08x at PA 0x%llx\n",
            width, height, stride, fourcc, (unsigned long long)addr);
}

HvFbDisplay *hv_fb_display_create(HvMachine *m, HvFwCfg *fw_cfg) {
    HvFbDisplay *fb = calloc(1, sizeof *fb);
    fb->m = m;
    pthread_mutex_init(&fb->mu, NULL);
    pthread_mutex_init(&fb->hot_mu, NULL);
    if (fw_cfg) {
        hv_fw_cfg_add_writable_file(fw_cfg, "etc/ramfb", 28, on_ramfb_write, fb);
    }
    return fb;
}

int hv_fb_display_get_frame(HvFbDisplay *fb, const uint8_t **pixels, uint32_t *w, uint32_t *h, uint32_t *stride) {
    if (!fb) return -1;
    pthread_mutex_lock(&fb->mu);
    if (fb->sc_pixels) {
        *pixels = fb->sc_pixels;
        *w = fb->sc_width;
        *h = fb->sc_height;
        *stride = fb->sc_stride;
        pthread_mutex_unlock(&fb->mu);
        return 0;
    }
    if (!fb->active || !fb->fb_host || !fb->width || !fb->height) {
        pthread_mutex_unlock(&fb->mu);
        return -1;
    }
    *pixels = fb->fb_host;
    *w = fb->width;
    *h = fb->height;
    *stride = fb->stride;
    pthread_mutex_unlock(&fb->mu);
    return 0;
}

int hv_fb_display_copy_frame(HvFbDisplay *fb, uint8_t **out, uint32_t *w, uint32_t *h) {
    if (!fb || !out) return -1;
    pthread_mutex_lock(&fb->mu);
    const uint8_t *px = fb->sc_pixels;
    uint32_t fw = fb->sc_width, fh = fb->sc_height, stride = fb->sc_stride;
    if (!px) {
        px = fb->active ? fb->fb_host : NULL;
        fw = fb->width, fh = fb->height, stride = fb->stride;
    }
    uint8_t *copy = px && fw && fh ? malloc((size_t)fw * 4 * fh) : NULL;
    if (copy) {
        for (uint32_t y = 0; y < fh; y++) memcpy(copy + (size_t)y * fw * 4, px + (size_t)y * stride, (size_t)fw * 4);
    }
    pthread_mutex_unlock(&fb->mu);
    if (!copy) return -1;
    *out = copy;
    *w = fw;
    *h = fh;
    return 0;
}

void hv_fb_display_lock(HvFbDisplay *fb) {
    pthread_mutex_lock(&fb->mu);
}

void hv_fb_display_unlock(HvFbDisplay *fb) {
    pthread_mutex_unlock(&fb->mu);
}

void hv_fb_display_set_scanout(HvFbDisplay *fb, const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride) {
    if (!fb) return;
    if (!pixels || !width || !height || stride < width * 4u) {
        pixels = NULL;
        width = height = stride = 0;
    }
    pthread_mutex_lock(&fb->mu);
    int changed = fb->sc_pixels != pixels || fb->sc_width != width || fb->sc_height != height || fb->sc_stride != stride;
    fb->sc_pixels = pixels;
    fb->sc_width = width;
    fb->sc_height = height;
    fb->sc_stride = stride;
    if (changed) {
        fb->generation++;
        /* A new source or geometry is all new: the whole of it is dirty. */
        pthread_mutex_lock(&fb->hot_mu);
        fb->hot_width = width;
        fb->hot_height = height;
        fb->dirty_valid = pixels != NULL;
        fb->dirty_x0 = fb->dirty_y0 = 0;
        fb->dirty_x1 = width;
        fb->dirty_y1 = height;
        pthread_mutex_unlock(&fb->hot_mu);
    }
    pthread_mutex_unlock(&fb->mu);
}

void hv_fb_display_mark_dirty(HvFbDisplay *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!fb || !w || !h) return;
    uint64_t x1 = (uint64_t)x + w, y1 = (uint64_t)y + h;
    pthread_mutex_lock(&fb->hot_mu);
    if (x1 > fb->hot_width) x1 = fb->hot_width;
    if (y1 > fb->hot_height) y1 = fb->hot_height;
    if (x < x1 && y < y1) {
        if (!fb->dirty_valid) {
            fb->dirty_valid = 1;
            fb->dirty_x0 = x, fb->dirty_y0 = y, fb->dirty_x1 = (uint32_t)x1, fb->dirty_y1 = (uint32_t)y1;
        } else {
            if (x < fb->dirty_x0) fb->dirty_x0 = x;
            if (y < fb->dirty_y0) fb->dirty_y0 = y;
            if (x1 > fb->dirty_x1) fb->dirty_x1 = (uint32_t)x1;
            if (y1 > fb->dirty_y1) fb->dirty_y1 = (uint32_t)y1;
        }
    }
    pthread_mutex_unlock(&fb->hot_mu);
}

void hv_fb_display_set_cursor_image(HvFbDisplay *fb, const uint32_t *bgra64x64, uint32_t hot_x, uint32_t hot_y) {
    if (!fb) return;
    pthread_mutex_lock(&fb->mu);
    if (bgra64x64) {
        memcpy(fb->cursor_image, bgra64x64, sizeof fb->cursor_image);
        fb->cursor_has_image = 1;
        fb->cursor_hot_x = hot_x < HV_FB_CURSOR_DIM ? hot_x : HV_FB_CURSOR_DIM - 1;
        fb->cursor_hot_y = hot_y < HV_FB_CURSOR_DIM ? hot_y : HV_FB_CURSOR_DIM - 1;
        fb->cursor_generation++;
    } else if (fb->cursor_has_image) {
        fb->cursor_has_image = 0;
        fb->cursor_generation++;
    }
    pthread_mutex_unlock(&fb->mu);
}

void hv_fb_display_move_cursor(HvFbDisplay *fb, int32_t x, int32_t y, int visible) {
    if (!fb) return;
    pthread_mutex_lock(&fb->hot_mu);
    fb->cursor_x = x;
    fb->cursor_y = y;
    fb->cursor_visible = visible != 0;
    pthread_mutex_unlock(&fb->hot_mu);
}

int hv_fb_display_take_frame(HvFbDisplay *fb, HvFbFrame *out) {
    if (!fb || !out) return -1;
    memset(out, 0, sizeof *out);
    if (fb->sc_pixels) {
        out->pixels = fb->sc_pixels;
        out->width = fb->sc_width;
        out->height = fb->sc_height;
        out->stride = fb->sc_stride;
        out->from_scanout = 1;
        out->has_dirty = 1;
    } else if (fb->active && fb->fb_host && fb->width && fb->height) {
        out->pixels = fb->fb_host;
        out->width = fb->width;
        out->height = fb->height;
        out->stride = fb->stride;
    } else {
        return -1;
    }
    out->generation = fb->generation;
    pthread_mutex_lock(&fb->hot_mu);
    if (out->from_scanout && fb->dirty_valid) {
        out->dirty_x = fb->dirty_x0;
        out->dirty_y = fb->dirty_y0;
        out->dirty_w = fb->dirty_x1 - fb->dirty_x0;
        out->dirty_h = fb->dirty_y1 - fb->dirty_y0;
    }
    fb->dirty_valid = 0;
    out->cursor_x = fb->cursor_x;
    out->cursor_y = fb->cursor_y;
    out->cursor_visible = fb->cursor_visible && fb->cursor_has_image;
    pthread_mutex_unlock(&fb->hot_mu);
    out->cursor_hot_x = fb->cursor_hot_x;
    out->cursor_hot_y = fb->cursor_hot_y;
    out->cursor_image = fb->cursor_has_image ? fb->cursor_image : NULL;
    out->cursor_generation = fb->cursor_generation;
    return 0;
}

void hv_fb_display_set_resize_handler(HvFbDisplay *fb, HvFbResizeFn fn, void *opaque) {
    if (!fb) return;
    pthread_mutex_lock(&fb->mu);
    fb->resize_fn = fn;
    fb->resize_opaque = opaque;
    uint32_t w = fb->req_width, h = fb->req_height;
    pthread_mutex_unlock(&fb->mu);
    if (fn && w) fn(opaque, w, h); /* a size asked for before there was a provider */
}

static uint32_t clamp_even(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v & ~1u;
}

void hv_fb_display_request_size(HvFbDisplay *fb, uint32_t width, uint32_t height) {
    if (!fb) return;
    width = clamp_even(width, 640, 3840);
    height = clamp_even(height, 480, 2160);
    pthread_mutex_lock(&fb->mu);
    fb->req_width = width;
    fb->req_height = height;
    HvFbResizeFn fn = fb->resize_fn;
    void *opaque = fb->resize_opaque;
    pthread_mutex_unlock(&fb->mu);
    /* Not under mu: the handler takes the device lock, and the device lock holders take mu. */
    if (fn) fn(opaque, width, height);
}
