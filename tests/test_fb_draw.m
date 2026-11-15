/* devices/fb_metal.m's shaders, drawn off screen: what each way of drawing the guest's picture puts in every
   drawable pixel. The picture is a few guest pixels of known colours; the drawable is read back and compared
   with the arithmetic. At a whole number of drawable pixels to the guest pixel every guest pixel is drawn as it
   is. At 2.4 (the phone's 3x panel with Windows at five pixels to four points) a drawable pixel that lies inside
   one guest pixel takes its colour and one across the edge between two takes each by its share; the plain
   linear sampler does not do that, it softens every pixel, which is why it is kept for reductions only. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "fb_display.h"

static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llu, expected %llu\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

/* The shaders' Quad: where the quad is in the drawable and the texture coordinates across it. */
typedef struct {
    float origin[2], size[2], uv0[2], uvSize[2];
} Quad;

enum { PW = 5, PH = 5 };
static uint8_t picture[PH][PW][4];

static void make_picture(void) {
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) {
            picture[y][x][0] = (uint8_t)((x * 53 + y * 101) % 256);
            picture[y][x][1] = (x & 1) ? 255 : 0;
            picture[y][x][2] = (y & 1) ? 240 : 10;
            picture[y][x][3] = 255;
        }
}

static id<MTLDevice> device;
static id<MTLLibrary> library;
static id<MTLTexture> texture;

/* Draws the picture with one fragment shader into an ow by oh drawable and returns its BGRA bytes. */
static uint8_t *draw(NSString *fragment, int ow, int oh, Quad quad) {
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [library newFunctionWithName:@"fb_vs"];
    pd.fragmentFunction = [library newFunctionWithName:fragment];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *err = nil;
    id<MTLRenderPipelineState> pipe = [device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pipe) return NULL;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:(NSUInteger)ow
                                                                                 height:(NSUInteger)oh
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pipe];
    [enc setVertexBytes:&quad length:sizeof quad atIndex:0];
    [enc setFragmentTexture:texture atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    uint8_t *out = malloc((size_t)ow * (size_t)oh * 4);
    [target getBytes:out bytesPerRow:(NSUInteger)ow * 4 fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)ow, (NSUInteger)oh) mipmapLevel:0];
    return out;
}

static int guest_index(double g, int n) {
    int k = (int)floor(g);
    return k < 0 ? 0 : k >= n ? n - 1 : k;
}

/* Along one axis: the share of each guest pixel in drawable pixel i, for a picture that starts `at` drawable
   pixels in and has `scale` drawable pixels to the guest pixel. Beyond the picture its edge pixel continues. */
static void shares(int i, double at, double scale, int n, double w[]) {
    double a = (i - at) / scale, b = (i + 1 - at) / scale;
    for (int k = 0; k < n; k++) w[k] = 0;
    for (double g = floor(a); g < b; g += 1) {
        double lo = g > a ? g : a, hi = g + 1 < b ? g + 1 : b;
        if (hi > lo) w[guest_index(g, n)] += (hi - lo) / (b - a);
    }
}

/* How many colour values of the drawable are further than `slack` from the picture drawn by coverage; `solid`
   gets how many drawable pixels lie inside a single guest pixel. */
static int off_coverage(const uint8_t *out, int ow, int oh, double ax, double ay, double scale, int slack, int *solid) {
    int bad = 0;
    if (solid) *solid = 0;
    for (int j = 0; j < oh; j++)
        for (int i = 0; i < ow; i++) {
            double wx[PW], wy[PH];
            shares(i, ax, scale, PW, wx);
            shares(j, ay, scale, PH, wy);
            int one = 0;
            for (int y = 0; y < PH; y++)
                for (int x = 0; x < PW; x++) one += wx[x] * wy[y] > 0.999999;
            if (solid) *solid += one;
            for (int c = 0; c < 3; c++) {
                double want = 0;
                for (int y = 0; y < PH; y++)
                    for (int x = 0; x < PW; x++) want += wx[x] * wy[y] * picture[y][x][c];
                if (fabs(out[(j * ow + i) * 4 + c] - want) > (one ? 1 : slack)) bad++;
            }
        }
    return bad;
}

int main(void) {
    @autoreleasepool {
        printf("which way of drawing a scale gets\n");
        check("3 drawable pixels to the guest pixel: as it is", hv_fb_draw_for(3.0), HV_FB_DRAW_EXACT);
        check("2: as it is", hv_fb_draw_for(2.0), HV_FB_DRAW_EXACT);
        check("1: as it is", hv_fb_draw_for(1.0), HV_FB_DRAW_EXACT);
        check("2 but for the fit's rounding: as it is", hv_fb_draw_for(2.0005), HV_FB_DRAW_EXACT);
        check("1 but for the fit's rounding: as it is", hv_fb_draw_for(0.9995), HV_FB_DRAW_EXACT);
        check("2.4: by coverage", hv_fb_draw_for(2.4), HV_FB_DRAW_COVER);
        check("2.4005: by coverage", hv_fb_draw_for(2.4005), HV_FB_DRAW_COVER);
        check("1.5: by coverage", hv_fb_draw_for(1.5), HV_FB_DRAW_COVER);
        check("2.002: by coverage", hv_fb_draw_for(2.002), HV_FB_DRAW_COVER);
        check("1.002: by coverage", hv_fb_draw_for(1.002), HV_FB_DRAW_COVER);
        check("0.75: smoothed", hv_fb_draw_for(0.75), HV_FB_DRAW_SMOOTH);
        check("0.5: smoothed", hv_fb_draw_for(0.5), HV_FB_DRAW_SMOOTH);
        check("nothing to draw: smoothed", hv_fb_draw_for(0.0), HV_FB_DRAW_SMOOTH);

        device = MTLCreateSystemDefaultDevice();
        check("there is a Metal device to draw with", device != nil, 1);
        if (!device) {
            printf("test_fb_draw: %d checks, %d failures\n", checks, failures);
            return 1;
        }
        NSError *err = nil;
        library = [device newLibraryWithSource:[NSString stringWithUTF8String:hv_fb_metal_shader_source()] options:nil error:&err];
        check("the presenter's shaders compile", library != nil, 1);
        if (!library) {
            printf("  %s\n", err.localizedDescription.UTF8String);
            printf("test_fb_draw: %d checks, %d failures\n", checks, failures);
            return 1;
        }
        make_picture();
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:PW
                                                                                     height:PH
                                                                                  mipmapped:NO];
        texture = [device newTextureWithDescriptor:td];
        [texture replaceRegion:MTLRegionMake2D(0, 0, PW, PH) mipmapLevel:0 withBytes:picture bytesPerRow:PW * 4];
        Quad whole = {{0, 0}, {1, 1}, {0, 0}, {1, 1}};
        int solid = 0;

        printf("2.4 drawable pixels to the guest pixel (5 by 5 drawn 12 by 12)\n");
        uint8_t *out = draw(@"fb_fs_cover", 12, 12, whole);
        check("the coverage shader draws", out != NULL, 1);
        if (out) {
            check("every pixel is its guest pixels by their share", (uint64_t)off_coverage(out, 12, 12, 0, 0, 2.4, 3, &solid), 0);
            check("8 of 12 pixels a side lie inside one guest pixel, and are that pixel", (uint64_t)solid, 64);
            /* Pixel 2 of the top row lies across the first edge: 0.4 of guest pixel 0 and 0.6 of pixel 1. */
            check("across an edge: green is 0.6 of 255", (uint64_t)(abs(out[2 * 4 + 1] - 153) <= 3), 1);
            check("the pixel before the edge is guest pixel 0", out[1 * 4 + 1], 0);
            check("the pixel after it is guest pixel 1", out[3 * 4 + 1], 255);
            free(out);
        }
        out = draw(@"fb_fs_linear", 12, 12, whole);
        check("the linear shader draws", out != NULL, 1);
        if (out) {
            int soft = off_coverage(out, 12, 12, 0, 0, 2.4, 3, NULL);
            check("the linear sampler is not that picture (it softens most pixels)", (uint64_t)(soft > 100), 1);
            free(out);
        }

        /* Retina on a 3x panel: 1.5, where every third drawable pixel lies across an edge. 5 guest pixels are 7.5
           drawable pixels, so the picture is drawn into 9 with its edge pixel continued over the last 1.5. */
        printf("1.5 drawable pixels to the guest pixel\n");
        Quad wider = {{0, 0}, {1, 1}, {0, 0}, {9.0f / 7.5f, 9.0f / 7.5f}};
        out = draw(@"fb_fs_cover", 9, 9, wider);
        check("the coverage shader draws", out != NULL, 1);
        if (out) {
            check("every pixel is its guest pixels by their share", (uint64_t)off_coverage(out, 9, 9, 0, 0, 1.5, 3, NULL), 0);
            /* Pixel 1 of the top row is half guest pixel 0 and half pixel 1; pixels 0 and 2 are one each. */
            check("across an edge: green is half of 255", (uint64_t)(abs(out[1 * 4 + 1] - 128) <= 3), 1);
            check("the pixel before the edge is guest pixel 0", out[0 * 4 + 1], 0);
            check("the pixel after it is guest pixel 1", out[2 * 4 + 1], 255);
            free(out);
        }

        printf("a whole number: coverage is the picture as it is\n");
        uint8_t *cover = draw(@"fb_fs_cover", 15, 15, whole), *exact = draw(@"fb_fs_nearest", 15, 15, whole);
        check("both draw", cover != NULL && exact != NULL, 1);
        if (cover && exact) {
            int differ = 0, wrong = 0;
            for (int j = 0; j < 15; j++)
                for (int i = 0; i < 15; i++)
                    for (int c = 0; c < 3; c++) {
                        differ += abs(cover[(j * 15 + i) * 4 + c] - exact[(j * 15 + i) * 4 + c]) > 1;
                        wrong += exact[(j * 15 + i) * 4 + c] != picture[j / 3][i / 3][c];
                    }
            check("at 3 the nearest shader gives each guest pixel 3 by 3", (uint64_t)wrong, 0);
            check("and the coverage shader the same", (uint64_t)differ, 0);
        }
        free(cover);
        free(exact);

        /* The phone's case in small: the picture stops short of the drawable's edges (2 pixels in from the left,
           1 from the top, 12 by 12 of a 16 by 14 drawable) and its edge pixels continue around it. */
        printf("the picture inside a larger drawable, its edges continued\n");
        Quad inside = {{0, 0}, {1, 1}, {-2.0f / 12, -1.0f / 12}, {16.0f / 12, 14.0f / 12}};
        out = draw(@"fb_fs_cover", 16, 14, inside);
        check("draws", out != NULL, 1);
        if (out) {
            check("every pixel is its guest pixels by their share", (uint64_t)off_coverage(out, 16, 14, 2, 1, 2.4, 3, NULL), 0);
            /* Red, which no corner pixel has at 0 (the cleared drawable's value). */
            check("the corner beyond the picture is the picture's corner pixel", out[2], picture[0][0][2]);
            check("and so is the far corner", out[(13 * 16 + 15) * 4 + 2], picture[PH - 1][PW - 1][2]);
            free(out);
        }

        printf("test_fb_draw: %d checks, %d failures\n", checks, failures);
    }
    return failures != 0;
}
