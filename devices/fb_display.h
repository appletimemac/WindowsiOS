#ifndef WINVM_FB_DISPLAY_H
#define WINVM_FB_DISPLAY_H

#include <pthread.h>
#include <stdint.h>

#include "fw_cfg.h"
#include "machine.h"
#include "pcie.h"

/* Linear framebuffer display supporting QEMU `ramfb` (etc/ramfb in fw_cfg, used by
   EDK2 ArmVirtQemu GOP -> winload.efi -> Windows 11 BasicDisplay.sys + DWM)
   as well as optional VBE/bochs scanout. Zero MMIO exits per pixel write!

   A scanout provider (virtio-gpu) can take over from ramfb: while one is set its
   pixels are what get_frame and take_frame report. */

#define HV_FB_CURSOR_DIM 64

typedef void (*HvFbResizeFn)(void *opaque, uint32_t width, uint32_t height);

typedef struct HvFbDisplay {
    HvMachine *m;
    pthread_mutex_t mu;

    /* Active scanout geometry (protected by mu) */
    uint64_t fb_pa;
    const uint8_t *fb_host;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t fourcc; /* DRM fourcc, e.g. XR24 / AR24 (32-bit BGRX) */
    int active;

    /* Scanout provider override, BGRX8888 rows (protected by mu) */
    const uint8_t *sc_pixels;
    uint32_t sc_width, sc_height, sc_stride;
    uint64_t generation;

    /* Hardware cursor image (protected by mu) */
    uint32_t cursor_image[HV_FB_CURSOR_DIM * HV_FB_CURSOR_DIM];
    int cursor_has_image;
    uint32_t cursor_hot_x, cursor_hot_y;
    uint64_t cursor_generation;

    /* Resize handler and the last requested size (protected by mu) */
    HvFbResizeFn resize_fn;
    void *resize_opaque;
    uint32_t req_width, req_height;

    /* Hot state: changes on every flush or pointer move, so it has its own lock
       (taken inside mu where both are needed) and never waits for a presenter
       that holds mu while it reads pixels. */
    pthread_mutex_t hot_mu;
    uint32_t hot_width, hot_height;                  /* scanout size, for clipping dirty rectangles */
    int dirty_valid;
    uint32_t dirty_x0, dirty_y0, dirty_x1, dirty_y1; /* [x0,x1) x [y0,y1) in scanout pixels */
    int cursor_visible;
    int32_t cursor_x, cursor_y;

    /* Metal presenter handle (created lazily by fb_metal.m) */
    void *metal;
} HvFbDisplay;

HvFbDisplay *hv_fb_display_create(HvMachine *m, HvFwCfg *fw_cfg);
/* Thread-safe snapshot of current scanout pointer and dimensions; returns 0 if active, -1 if not yet set.
   The pointer stays valid only while the source does (a provider switches the scanout away before it frees). */
int hv_fb_display_get_frame(HvFbDisplay *fb, const uint8_t **pixels, uint32_t *w, uint32_t *h, uint32_t *stride);
/* A private, tightly packed (w * 4 bytes per row) copy of the current frame, taken under fb->mu so a
   provider cannot free it mid-copy. The caller frees *out. Returns 0, or -1 with nothing to copy. */
int hv_fb_display_copy_frame(HvFbDisplay *fb, uint8_t **out, uint32_t *w, uint32_t *h);

/* Scanout provider (virtio-gpu): takes precedence over ramfb while set.
   pixels: provider-owned BGRX8888 rows; NULL disables the override. Takes fb->mu. */
void hv_fb_display_set_scanout(HvFbDisplay *fb, const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride);
/* Union a changed rectangle (scanout pixels) into the pending dirty region. */
void hv_fb_display_mark_dirty(HvFbDisplay *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* Hardware cursor: 64x64 BGRA8 image copied into fb-owned storage (NULL hides it); position in scanout pixels.
   (x, y) is where the cursor's hot spot is: the image's top-left goes at (x - hot_x, y - hot_y). */
void hv_fb_display_set_cursor_image(HvFbDisplay *fb, const uint32_t *bgra64x64, uint32_t hot_x, uint32_t hot_y);
void hv_fb_display_move_cursor(HvFbDisplay *fb, int32_t x, int32_t y, int visible);

typedef struct HvFbFrame {
    const uint8_t *pixels; uint32_t width, height, stride;
    int from_scanout;                 /* 1 = virtio-gpu scanout, 0 = ramfb */
    int has_dirty;                    /* scanout: 1 when dirty_* holds the exact union since the last take (w == h == 0: nothing changed); ramfb: always 0 (presenter diffs) */
    uint32_t dirty_x, dirty_y, dirty_w, dirty_h;
    uint64_t generation;              /* bumps whenever the source, geometry or stride changes */
    int cursor_visible; int32_t cursor_x, cursor_y; uint32_t cursor_hot_x, cursor_hot_y;
    const uint32_t *cursor_image;     /* 64x64 BGRA8 or NULL */
    uint64_t cursor_generation;       /* bumps when the image changes */
} HvFbFrame;
/* Presenter snapshot; consumes (clears) the pending dirty region. -1 when nothing is active.
   Call hv_fb_display_lock() first and keep it held while reading pixels/cursor_image, then unlock. */
int hv_fb_display_take_frame(HvFbDisplay *fb, HvFbFrame *out);
void hv_fb_display_lock(HvFbDisplay *fb);
void hv_fb_display_unlock(HvFbDisplay *fb);

/* Host window size in pixels the guest should render at; forwarded to the provider's resize handler. */
void hv_fb_display_set_resize_handler(HvFbDisplay *fb, HvFbResizeFn fn, void *opaque);
/* Any thread. Clamps to even values in 640x480..3840x2160; the handler is called without fb->mu held. */
void hv_fb_display_request_size(HvFbDisplay *fb, uint32_t width, uint32_t height);

/* A rectangle in fractions of a drawable's width and height. */
typedef struct HvFbRect {
    double x, y, w, h;
} HvFbRect;
/* Where a tex_w by tex_h picture goes when it is fitted into `content` of a draw_w by draw_h pixel drawable:
   aspect kept, centred, as large as fits. */
HvFbRect hv_fb_fit(double draw_w, double draw_h, uint32_t tex_w, uint32_t tex_h, HvFbRect content);

/* Metal layer presentation & PNG screenshot helpers (implemented in devices/fb_metal.m) */
void hv_fb_metal_set_layer(HvFbDisplay *fb, void *ca_metal_layer);
/* Where in the layer the guest picture goes, as fractions of its width and height. The rest of the layer
   continues the picture's edge pixels, so the picture can stop short of a display's rounded corners while
   nothing around it is left black. Width or height 0: the whole layer, letterboxed in black (the default). */
void hv_fb_metal_set_content(HvFbDisplay *fb, double x, double y, double w, double h);
uint64_t hv_fb_metal_frames(HvFbDisplay *fb);
int hv_fb_save_png(HvFbDisplay *fb, const char *path);

#endif
