#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <TargetConditionals.h>

#include <os/lock.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "fb_display.h"

/* Presents the guest display through Metal.
   Pacing comes from a CAMetalDisplayLink on its own run-loop thread (up to 120 Hz), and a frame
   is drawn only when something changed: the exact rectangles virtio-gpu flushed, or for ramfb a
   full-coverage row diff against a shadow copy (a sampled hash used to miss carets and hovers).
   Only the changed box is uploaded. The virtio-gpu hardware cursor is drawn as a second quad. */

static NSString *const kFbShaderSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut { float4 pos [[position]]; float2 uv; };\n"
     "struct Quad { float2 origin; float2 size; float2 uv0; float2 uvSize; };\n"
     "vertex VOut fb_vs(uint vid [[vertex_id]], constant Quad &q [[buffer(0)]]) {\n"
     "    const float2 corner[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };\n"
     "    float2 c = corner[vid];\n"
     "    VOut o;\n"
     "    float2 p = q.origin + c * q.size;\n"
     "    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
     "    o.uv = q.uv0 + c * q.uvSize;\n"
     "    return o;\n"
     "}\n"
     "fragment float4 fb_fs_linear(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {\n"
     "    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);\n"
     "    return float4(tex.sample(s, in.uv).rgb, 1.0);\n"
     "}\n"
     "fragment float4 fb_fs_nearest(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {\n"
     "    constexpr sampler s(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);\n"
     "    return float4(tex.sample(s, in.uv).rgb, 1.0);\n"
     "}\n"
     "fragment float4 fb_fs_cursor(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {\n"
     "    constexpr sampler s(mag_filter::nearest, min_filter::nearest, address::clamp_to_edge);\n"
     "    return tex.sample(s, in.uv);\n"
     "}\n";

typedef struct {
    float origin[2];
    float size[2];
    float uv0[2];    /* texture coordinates at origin and across size: outside 0..1 the samplers */
    float uvSize[2]; /* clamp, so a quad larger than the picture repeats the picture's edge */
} FbQuad;

@interface HvFbMetalPresenter : NSObject <CAMetalDisplayLinkDelegate> {
@public
    HvFbDisplay *fb;
    os_unfair_lock lock; /* guards layer/link handoff; never held across a draw */
    CAMetalLayer *layer;
    CAMetalDisplayLink *link;
    NSThread *renderThread;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> pipeLinear, pipeNearest, pipeCursor;
    id<MTLTexture> texture, cursorTexture;
    uint32_t texW, texH;
    uint8_t *shadow; /* ramfb only: last uploaded pixels, for change detection */
    size_t shadowBytes;
    uint64_t lastGeneration;
    uint64_t lastCursorGeneration;
    int lastFromScanout;
    int lastCursorVisible;
    int32_t lastCursorX, lastCursorY;
    CGSize lastDrawableSize;
    /* Where in the drawable the picture goes (0..1 of each axis), under lock. Unset: the whole drawable,
       letterboxed in black. Set: the rest of the drawable continues the picture's edge pixels. */
    double content[4];
    BOOL hasContent, contentMoved;
    _Atomic(uint64_t) framesPresented;
}
- (instancetype)initWithDisplay:(HvFbDisplay *)display;
- (void)setTargetLayer:(CAMetalLayer *)newLayer;
- (void)setContentX:(double)x y:(double)y width:(double)w height:(double)h;
@end

@implementation HvFbMetalPresenter

static id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *frag, BOOL blend) {
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"fb_vs"];
    pd.fragmentFunction = [lib newFunctionWithName:frag];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (blend) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
        ca.blendingEnabled = YES;
        ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    NSError *err = nil;
    return [dev newRenderPipelineStateWithDescriptor:pd error:&err];
}

- (instancetype)initWithDisplay:(HvFbDisplay *)display {
    if ((self = [super init])) {
        fb = display;
        lock = OS_UNFAIR_LOCK_INIT;
        device = MTLCreateSystemDefaultDevice();
        if (device) {
            queue = [device newCommandQueue];
            NSError *err = nil;
            id<MTLLibrary> lib = [device newLibraryWithSource:kFbShaderSource options:nil error:&err];
            if (lib) {
                pipeLinear = make_pipeline(device, lib, @"fb_fs_linear", NO);
                pipeNearest = make_pipeline(device, lib, @"fb_fs_nearest", NO);
                pipeCursor = make_pipeline(device, lib, @"fb_fs_cursor", YES);
            }
        }
        renderThread = [[NSThread alloc] initWithTarget:self selector:@selector(renderThreadMain) object:nil];
        renderThread.name = @"winvm.fb.render";
        renderThread.qualityOfService = NSQualityOfServiceUserInteractive;
        [renderThread start];
    }
    return self;
}

- (void)renderThreadMain {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    NSRunLoop *rl = [NSRunLoop currentRunLoop];
    /* A port keeps the run loop alive between display links. */
    [rl addPort:[NSMachPort port] forMode:NSDefaultRunLoopMode];
    while (true) {
        @autoreleasepool {
            [rl runMode:NSDefaultRunLoopMode beforeDate:[NSDate distantFuture]];
        }
    }
}

- (void)setTargetLayer:(CAMetalLayer *)newLayer {
    os_unfair_lock_lock(&lock);
    layer = newLayer;
    os_unfair_lock_unlock(&lock);

    if (newLayer && device) {
        dispatch_block_t cfg = ^{
            newLayer.device = self->device;
            newLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            newLayer.framebufferOnly = YES;
            newLayer.opaque = YES;
        };
        if ([NSThread isMainThread]) cfg();
        else dispatch_async(dispatch_get_main_queue(), cfg);
    }
    [self performSelector:@selector(attachLinkTo:) onThread:renderThread withObject:newLayer waitUntilDone:NO];
}

/* Any thread. A rectangle with no area goes back to the whole drawable. The next tick redraws. */
- (void)setContentX:(double)x y:(double)y width:(double)w height:(double)h {
    os_unfair_lock_lock(&lock);
    hasContent = w > 0 && h > 0;
    content[0] = x;
    content[1] = y;
    content[2] = w;
    content[3] = h;
    contentMoved = YES;
    os_unfair_lock_unlock(&lock);
}

/* Runs on the render thread. */
- (void)attachLinkTo:(CAMetalLayer *)target {
    [link invalidate];
    link = nil;
    /* Here, not in setTargetLayer: ticks also run on this thread, so an old layer's tick in
       flight cannot overwrite the reset and leave the new layer blank. */
    lastDrawableSize = CGSizeZero;
    lastGeneration = 0;
    lastFromScanout = 0;
    lastCursorGeneration = 0;
    lastCursorVisible = 0;
    if (!target || !device) return;
    link = [[CAMetalDisplayLink alloc] initWithMetalLayer:target];
    link.delegate = self;
    link.preferredFrameLatency = 1.0f;
    link.preferredFrameRateRange = CAFrameRateRangeMake(60, 120, 120);
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
}

/* ramfb: compare each row with the shadow copy, refresh the shadow and return the changed box. */
- (BOOL)diffRamfb:(const uint8_t *)px width:(uint32_t)w height:(uint32_t)h stride:(uint32_t)stride
          forceAll:(BOOL)force box:(MTLRegion *)box {
    size_t rowBytes = (size_t)w * 4;
    size_t need = rowBytes * h;
    if (shadowBytes != need) {
        free(shadow);
        shadow = malloc(need);
        shadowBytes = shadow ? need : 0;
        force = YES;
    }
    if (!shadow) return NO;
    uint32_t y0 = UINT32_MAX, y1 = 0, x0 = UINT32_MAX, x1 = 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = px + (size_t)y * stride;
        uint8_t *dst = shadow + (size_t)y * rowBytes;
        if (!force && memcmp(src, dst, rowBytes) == 0) continue;
        /* Narrow the box in 16-pixel steps: enough to keep uploads small, cheap to find. */
        uint32_t first = 0, last = w;
        if (!force) {
            const size_t chunk = 64;
            size_t a = 0, b = rowBytes;
            while (a + chunk <= rowBytes && memcmp(src + a, dst + a, chunk) == 0) a += chunk;
            while (b >= a + chunk && memcmp(src + b - chunk, dst + b - chunk, chunk) == 0) b -= chunk;
            first = (uint32_t)(a / 4);
            last = (uint32_t)((b + 3) / 4);
            if (last > w) last = w;
            if (first >= last) { /* the guest wrote the old bytes back between the compares */
                memcpy(dst, src, rowBytes);
                continue;
            }
        }
        memcpy(dst, src, rowBytes);
        if (y < y0) y0 = y;
        y1 = y + 1;
        if (first < x0) x0 = first;
        if (last > x1) x1 = last;
    }
    if (y0 == UINT32_MAX || x0 >= x1) return NO;
    *box = MTLRegionMake2D(x0, y0, x1 - x0, y1 - y0);
    return YES;
}

- (void)metalDisplayLink:(CAMetalDisplayLink *)l needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    @autoreleasepool {
        os_unfair_lock_lock(&lock);
        CAMetalLayer *curLayer = layer;
        BOOL extend = hasContent, moved = contentMoved;
        double rx = extend ? content[0] : 0.0, ry = extend ? content[1] : 0.0;
        double rw = extend ? content[2] : 1.0, rh = extend ? content[3] : 1.0;
        contentMoved = NO;
        os_unfair_lock_unlock(&lock);
        if (!curLayer || !pipeLinear || !queue) return;

        CGSize dsize = curLayer.drawableSize;
        if (dsize.width < 4 || dsize.height < 4) return;
        BOOL sizeChanged = moved || !CGSizeEqualToSize(dsize, lastDrawableSize);

        HvFbFrame f;
        hv_fb_display_lock(fb);
        if (hv_fb_display_take_frame(fb, &f) != 0) {
            hv_fb_display_unlock(fb);
            return;
        }
        BOOL newSource = f.generation != lastGeneration || f.from_scanout != lastFromScanout ||
                         !texture || texW != f.width || texH != f.height;
        if (!texture || texW != f.width || texH != f.height) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:f.width
                                                                                         height:f.height
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            texture = [device newTextureWithDescriptor:td];
            texW = f.width;
            texH = f.height;
        }

        BOOL contentChanged = NO;
        MTLRegion box;
        if (f.from_scanout) {
            if (newSource) {
                box = MTLRegionMake2D(0, 0, f.width, f.height);
                contentChanged = YES;
            } else if (f.has_dirty && f.dirty_w && f.dirty_h) {
                uint32_t x = f.dirty_x < f.width ? f.dirty_x : f.width;
                uint32_t y = f.dirty_y < f.height ? f.dirty_y : f.height;
                uint32_t w = MIN(f.dirty_w, f.width - x), h = MIN(f.dirty_h, f.height - y);
                box = MTLRegionMake2D(x, y, w, h);
                contentChanged = w && h;
            }
            if (contentChanged) {
                const uint8_t *src = f.pixels + (size_t)box.origin.y * f.stride + (size_t)box.origin.x * 4;
                [texture replaceRegion:box mipmapLevel:0 withBytes:src bytesPerRow:f.stride];
            }
        } else {
            contentChanged = [self diffRamfb:f.pixels width:f.width height:f.height stride:f.stride
                                    forceAll:newSource box:&box];
            if (contentChanged) {
                const uint8_t *src = shadow + (size_t)box.origin.y * f.width * 4 + (size_t)box.origin.x * 4;
                [texture replaceRegion:box mipmapLevel:0 withBytes:src bytesPerRow:(NSUInteger)f.width * 4];
            }
        }

        BOOL cursorVisible = f.from_scanout && f.cursor_visible && f.cursor_image;
        BOOL cursorChanged = cursorVisible != lastCursorVisible ||
                             (cursorVisible && (f.cursor_x != lastCursorX || f.cursor_y != lastCursorY ||
                                                f.cursor_generation != lastCursorGeneration));
        if (cursorVisible && (f.cursor_generation != lastCursorGeneration || !cursorTexture)) {
            /* A new texture per shape: a frame still in flight keeps sampling the old one. */
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:64
                                                                                         height:64
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            cursorTexture = [device newTextureWithDescriptor:td];
            [cursorTexture replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:f.cursor_image bytesPerRow:64 * 4];
        }
        int32_t cx = f.cursor_x - (int32_t)f.cursor_hot_x, cy = f.cursor_y - (int32_t)f.cursor_hot_y;
        hv_fb_display_unlock(fb);

        lastGeneration = f.generation;
        lastFromScanout = f.from_scanout;
        lastCursorGeneration = f.cursor_generation;
        lastCursorVisible = cursorVisible;
        lastCursorX = f.cursor_x;
        lastCursorY = f.cursor_y;

        if (!contentChanged && !cursorChanged && !sizeChanged) return; /* nothing new: skip this vsync */

        /* Aspect fit inside the content rectangle, in normalized drawable coordinates. */
        HvFbRect fit = hv_fb_fit(dsize.width, dsize.height, texW, texH, (HvFbRect){rx, ry, rw, rh});
        double qx = fit.x, qy = fit.y, qw = fit.w, qh = fit.h;
        /* The picture alone, or the whole drawable with the picture at its place in it. */
        FbQuad quad = {{(float)qx, (float)qy}, {(float)qw, (float)qh}, {0, 0}, {1, 1}};
        if (extend) quad = (FbQuad){{0, 0}, {1, 1}, {(float)(-qx / qw), (float)(-qy / qh)}, {(float)(1.0 / qw), (float)(1.0 / qh)}};
        double pxScale = (qw * dsize.width) / (double)texW;
        BOOL integerScale = fabs(pxScale - round(pxScale)) < 0.001 && round(pxScale) >= 1.0;

        id<CAMetalDrawable> drawable = update.drawable;
        if (!drawable) return;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:integerScale ? pipeNearest : pipeLinear];
        [enc setVertexBytes:&quad length:sizeof quad atIndex:0];
        [enc setFragmentTexture:texture atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        if (cursorVisible && cursorTexture && pipeCursor) {
            FbQuad cq = {{(float)(qx + qw * (double)cx / texW), (float)(qy + qh * (double)cy / texH)},
                         {(float)(qw * 64.0 / texW), (float)(qh * 64.0 / texH)}, {0, 0}, {1, 1}};
            [enc setRenderPipelineState:pipeCursor];
            [enc setVertexBytes:&cq length:sizeof cq atIndex:0];
            [enc setFragmentTexture:cursorTexture atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        [enc endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];

        lastDrawableSize = dsize;
        atomic_fetch_add_explicit(&framesPresented, 1, memory_order_relaxed);
    }
}

@end

static HvFbMetalPresenter *ensure_presenter(HvFbDisplay *fb) {
    if (!fb) return nil;
    pthread_mutex_lock(&fb->mu);
    HvFbMetalPresenter *p = (__bridge HvFbMetalPresenter *)fb->metal;
    if (!p) {
        p = [[HvFbMetalPresenter alloc] initWithDisplay:fb];
        fb->metal = (__bridge_retained void *)p;
    }
    pthread_mutex_unlock(&fb->mu);
    return p;
}

void hv_fb_metal_set_layer(HvFbDisplay *fb, void *ca_metal_layer) {
    HvFbMetalPresenter *p = ensure_presenter(fb);
    if (!p) return;
    [p setTargetLayer:(__bridge CAMetalLayer *)ca_metal_layer];
}

void hv_fb_metal_set_content(HvFbDisplay *fb, double x, double y, double w, double h) {
    HvFbMetalPresenter *p = ensure_presenter(fb);
    if (!p) return;
    [p setContentX:x y:y width:w height:h];
}

uint64_t hv_fb_metal_frames(HvFbDisplay *fb) {
    if (!fb || !fb->metal) return 0;
    HvFbMetalPresenter *p = (__bridge HvFbMetalPresenter *)fb->metal;
    return atomic_load_explicit(&p->framesPresented, memory_order_relaxed);
}

int hv_fb_save_png(HvFbDisplay *fb, const char *path) {
    if (!fb || !path) return -1;
    uint8_t *copy = NULL;
    uint32_t w = 0, h = 0;
    if (hv_fb_display_copy_frame(fb, &copy, &w, &h) != 0) return -1;

    @autoreleasepool {
        size_t stride = (size_t)w * 4, total = stride * h;
        /* Ensure alpha byte is 0xff (XRGB8888 leaves X=0x00) */
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = copy + (size_t)y * stride;
            for (uint32_t x = 0; x < w; x++) row[x * 4 + 3] = 0xff;
        }

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, copy, total, NULL);
        CGBitmapInfo bmi = kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst;
        CGImageRef img = CGImageCreate(w, h, 8, 32, stride, cs, bmi, prov, NULL, false, kCGRenderingIntentDefault);
        int rc = -1;
        if (img) {
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL);
            if (dst) {
                CGImageDestinationAddImage(dst, img, NULL);
                if (CGImageDestinationFinalize(dst)) rc = 0;
                CFRelease(dst);
            }
            CGImageRelease(img);
        }
        CGDataProviderRelease(prov);
        CGColorSpaceRelease(cs);
        free(copy);
        return rc;
    }
}
