/*
 * mtlprobe — P3 first Metal test: does the VirtIOAccel -> VMTLDriver chain
 * produce a usable MTLDevice?
 *
 * Stage 1: MTLCopyAllDevices() must list our device (plugin loaded,
 *          MTLIOAccelDevice initWithAcceleratorPort: succeeded).
 * Stage 2: command queue + texture + blit round trip (kernel-side plumbing,
 *          no shaders yet — the compiler lane is proven separately).
 * Stage 3: render pass with a runtime-compiled shader (MSL via
 *          newLibraryWithSource:) drawing a full-screen quad; read the
 *          texture back through a blit and count lit pixels.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(fmt, ...) do { printf(fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)
static int gFail = 0;
#define CHECK(cond, what) do { \
    if (cond) LOG("  PASS: %s", what); \
    else      { LOG("  FAIL: %s", what); gFail++; } } while (0)

static const char *kMSL =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut vmain(uint vid [[vertex_id]],\n"
    "                  constant float2 *quad [[buffer(0)]]) {\n"
    "    VOut o; o.pos = float4(quad[vid], 0.0, 1.0); return o;\n"
    "}\n"
    "fragment float4 fmain(VOut in [[stage_in]]) {\n"
    "    return float4(0.9, 0.3, 0.2, 1.0);\n"
    "}\n";

int main(int argc, char **argv)
{
    @autoreleasepool {
        /* ---- stage 1: device enumeration -------------------------------- */
        NSArray<id<MTLDevice>> *devs = MTLCopyAllDevices();
        LOG("MTLCopyAllDevices -> %lu device(s)", (unsigned long)devs.count);
        for (id<MTLDevice> d in devs)
            LOG("  device: %s  registryID=%llu  lowPower=%d",
                d.name.UTF8String, d.registryID, d.isLowPower);
        CHECK(devs.count >= 1, "at least one Metal device exists");

        id<MTLDevice> dev = devs.firstObject;
        if (!dev) { LOG("no device — plugin did not load"); return 1; }

        id<MTLCommandQueue> queue = [dev newCommandQueue];
        CHECK(queue != nil, "newCommandQueue");
        if (!queue) return 1;

        const NSUInteger W = 64, H = 64;

        /* ---- stage 2: texture + blit round trip -------------------------- */
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        CHECK(tex != nil, "newTextureWithDescriptor (64x64 RGBA8)");

        /* ---- stage 3: runtime-compiled shader + render ------------------ */
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kMSL) options:nil error:&err];
        if (!lib) {
            LOG("  FAIL: newLibraryWithSource: %s",
                err ? err.localizedDescription.UTF8String : "(nil err)");
            gFail++;
        } else {
            CHECK(lib != nil, "newLibraryWithSource: (MSL triangle)");
            id<MTLFunction> vs = [lib newFunctionWithName:@"vmain"];
            id<MTLFunction> fs = [lib newFunctionWithName:@"fmain"];
            CHECK(vs && fs, "vertex+fragment functions resolved");
            if (vs && fs) {
                MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
                pd.vertexFunction = vs;
                pd.fragmentFunction = fs;
                pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                NSError *perr = nil;
                id<MTLRenderPipelineState> pso =
                    [dev newRenderPipelineStateWithDescriptor:pd error:&perr];
                if (!pso) {
                    LOG("  FAIL: newRenderPipelineState: %s",
                        perr ? perr.localizedDescription.UTF8String : "(nil)");
                    gFail++;
                } else {
                    CHECK(pso != nil, "render pipeline state (MSL -> device ISA)");

                    /* full-screen quad in clip space */
                    float quad[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
                    id<MTLBuffer> qbuf = [dev newBufferWithBytes:quad
                                                          length:sizeof quad
                                                         options:MTLResourceStorageModeShared];
                    CHECK(qbuf != nil, "vertex buffer (shared)");

                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
                    rp.colorAttachments[0].texture = tex;
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

                    id<MTLCommandBuffer> cb = [queue commandBuffer];
                    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                    [enc setRenderPipelineState:pso];
                    [enc setVertexBuffer:qbuf offset:0 atIndex:0];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                    [enc endEncoding];
                    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
                        LOG("  command buffer completed: status=%ld error=%s",
                            (long)b.status,
                            b.error ? b.error.localizedDescription.UTF8String : "none");
                    }];
                    [cb commit];
                    [cb waitUntilCompleted];
                    CHECK(cb.status == MTLCommandBufferStatusCompleted, "GPU finished the render pass");

                    /* read back: texture -> shared buffer */
                    NSUInteger bpr = W * 4;
                    id<MTLBuffer> rb = [dev newBufferWithLength:bpr * H
                                                        options:MTLResourceStorageModeShared];
                    id<MTLCommandBuffer> cb2 = [queue commandBuffer];
                    id<MTLBlitCommandEncoder> blit = [cb2 blitCommandEncoder];
                    [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0
                             sourceOrigin:MTLOriginMake(0, 0, 0)
                               sourceSize:MTLSizeMake(W, H, 1)
                                 toBuffer:rb destinationOffset:0
                        destinationBytesPerRow:bpr destinationBytesPerImage:bpr * H];
                    [blit endEncoding];
                    [cb2 commit];
                    [cb2 waitUntilCompleted];
                    CHECK(cb2.status == MTLCommandBufferStatusCompleted, "blit readback finished");

                    const unsigned char *px = (const unsigned char *)rb.contents;
                    unsigned lit = 0, teal = 0;
                    for (NSUInteger i = 0; i < W * H; i++) {
                        unsigned r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
                        if (r > 40 || g > 40 || b > 40) lit++;
                        if (r > 200 && g > 60 && g < 140 && b < 80) teal++;
                    }
                    LOG("  pixels lit=%u/4096  rust=%u", lit, teal);
                    CHECK(lit > 3900, "the full-screen quad covers the viewport");
                    CHECK(teal > 1000, "fragment color is the rust quad (0.9,0.3,0.2)");
                }
            }
        }

        LOG(gFail ? "mtlprobe: %d FAIL(s)" : "mtlprobe: ALL PASS", gFail);
        return gFail ? 1 : 0;
    }
}
