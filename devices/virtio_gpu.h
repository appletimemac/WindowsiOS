#ifndef HENGEVM_VIRTIO_GPU_H
#define HENGEVM_VIRTIO_GPU_H

#include "fb_display.h"
#include "virtio.h"

/* virtio-gpu (device type 16), 2D only, for Red Hat's viogpudo display-only
   driver (PCI\VEN_1AF4&DEV_1050). Control queue 0, cursor queue 1, one scanout.

   Each resource has a host copy of its pixels (BGRA byte order). The guest's
   TRANSFER_TO_HOST_2D fills it from the attached backing pages; the scanout
   resource's host copy is what the display shows (hv_fb_display_set_scanout),
   so a resource is never freed while it is the scanout.

   The host window drives the guest's resolution: hv_virtio_gpu_set_size (or the
   fb_display resize handler it is registered as) changes the size GET_DISPLAY_INFO
   and GET_EDID report and raises a config-change interrupt, which viogpudo
   answers by switching modes. */

#define HV_VGPU_MAX_RESOURCES 64
#define HV_VGPU_DEFAULT_WIDTH 1280
#define HV_VGPU_DEFAULT_HEIGHT 900
#define HV_VGPU_GRID_W 8 /* grid[]: flushed pixels by where on the scanout, cells across and down */
#define HV_VGPU_GRID_H 6
/* The EDID reports a physical size that puts the requested pixels at this density. Windows still
   starts this monitor at 100% (measured at 1280x900 and 1426x1004), so the tools installer sets 150%. */
#define HV_VGPU_EDID_DPI 144
static inline uint32_t hv_vgpu_phys_mm(uint32_t px) { return (uint32_t)((px * 254u + HV_VGPU_EDID_DPI * 5u) / (HV_VGPU_EDID_DPI * 10u)); }

typedef struct HvVgpuSeg {
    uint8_t *ptr;      /* host pointer into guest RAM */
    uint64_t start;    /* offset of this entry within the backing */
    uint32_t len;
} HvVgpuSeg;

typedef struct HvVgpuRes {
    uint32_t id;       /* 0: free slot */
    uint32_t format;
    uint32_t width, height, stride;
    int has_alpha;     /* the format has a real alpha channel (else the X byte is padding) */
    int swap;          /* the guest's bytes are A/X,R,G,B: reversed into B,G,R,A on transfer */
    uint8_t *pixels;   /* host copy, stride * height bytes, BGRA byte order */
    HvVgpuSeg *seg;    /* attached backing */
    uint32_t nseg;
    uint64_t backing_bytes;
} HvVgpuRes;

typedef struct HvVirtioGpu {
    HvVirtio vio;
    HvFbDisplay *fb;
    uint32_t width, height;            /* the size the guest is told to use */
    uint64_t told_at;                  /* host ticks when it was last told while showing another size; 0: not waiting */
    uint32_t told_again;               /* times it was told this size again (hv_virtio_gpu_tick) */
    uint8_t edid[128];
    HvVgpuRes res[HV_VGPU_MAX_RESOURCES];
    uint64_t host_bytes;               /* pixel memory held by the resources */
    uint32_t scanout_res;              /* resource id shown on the scanout, or 0 */
    uint32_t scanout_x, scanout_y, scanout_w, scanout_h; /* the part of it shown */
    uint8_t *req;                      /* request scratch */
    HvVirtqElem elem;
    uint64_t cmds, errors, transfers, flushes;
    /* Counters read racily from another thread (host ticks are mach_absolute_time units). */
    uint64_t flush_px;                       /* RESOURCE_FLUSH area that intersects the scanout, pixels */
    uint64_t xfer_bytes, xfer_ticks;         /* TRANSFER_TO_HOST_2D bytes copied and host time copying */
    uint64_t rect_small, rect_mid, rect_big; /* scanout flush rects by area: < 16384 px, < 262144 px, larger */
    uint64_t grid[HV_VGPU_GRID_W * HV_VGPU_GRID_H]; /* flushed pixels by rect center, cells over the scanout, row-major */
} HvVirtioGpu;

/* Adds the GPU to the bus and registers it as fb's resize handler. */
HvVirtioGpu *hv_virtio_gpu_create(HvMachine *m, HvPcie *bus, HvFbDisplay *fb);
/* Any thread (takes the device lock): the guest should render at this size (clamped to even values in
   640x480..3840x2160). Unchanged sizes do nothing. */
void hv_virtio_gpu_set_size(HvVirtioGpu *g, uint32_t width, uint32_t height);
/* The board's tick, device lock held, now in host ticks (mach_absolute_time): a guest that still shows another size
   three seconds after it was told one is told again, up to HV_VGPU_TOLD_AGAIN_MAX times for one size. Windows lets
   the announcement go by when it comes while a session is starting (the part of its driver that changes the mode
   is not up yet), and would otherwise keep the old size until the next change. */
#define HV_VGPU_TOLD_AGAIN_TICKS (3ull * 24000000ull)
#define HV_VGPU_TOLD_AGAIN_MAX 10u
void hv_virtio_gpu_tick(HvVirtioGpu *g, uint64_t now);
/* Any thread (takes the device lock): drop every resource, the scanout and the cursor, clear pending events (the driver
   reset the device, or the guest rebooted). */
void hv_virtio_gpu_reset(HvVirtioGpu *g);

/* Exposed for tests: fills a 128-byte EDID 1.4 block whose preferred timing is width x height. */
void hv_virtio_gpu_build_edid(uint8_t edid[128], uint32_t width, uint32_t height);

#endif
