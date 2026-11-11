#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
struct Input {
    uint32_t x, y, layer, level;
};
static void deadline(int s) {
    (void)s;
    _exit(124);
}
static void expected(float *v, unsigned x, unsigned y, unsigned z, unsigned m) {
    v[0] = z * 32 + m * 8 + x;
    v[1] = y + 0.5f;
    v[2] = x + y + z + m;
    v[3] = -0.25f * (x + 1);
}
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2)
            return 2;
        signal(SIGALRM, deadline);
        alarm(30);
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
        if (!d || !lib) {
            printf("library error=%s\n", e.description.UTF8String);
            return 2;
        }
        printf("device=%s\n", d.name.UTF8String);
        id<MTLCommandQueue> q = [d newCommandQueue];
        const char *names[] = {"array_float_read32", "array_float_sample32", "array_float_read16", "array_half_read32",
                               "plain_float_read"};
        unsigned total = 0, failed = 0;
        for (unsigned k = 0; k < 5; k++) {
            BOOL array = k != 4, half = k == 3;
            MTLTextureDescriptor *desc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:half ? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA32Float
                                             width:8
                                            height:4
                                         mipmapped:YES];
            desc.textureType = array ? MTLTextureType2DArray : MTLTextureType2D;
            desc.arrayLength = array ? 3 : 1;
            desc.mipmapLevelCount = 3;
            desc.usage = MTLTextureUsageShaderRead;
            desc.storageMode = MTLStorageModeShared;
            id<MTLTexture> t = [d newTextureWithDescriptor:desc];
            struct Input in[126];
            unsigned count = 0;
            for (unsigned m = 0; m < 3; m++)
                for (unsigned z = 0; z < desc.arrayLength; z++) {
                    unsigned w = 8 >> m, h = 4 >> m;
                    float pixels[128];
                    _Float16 halves[128];
                    for (unsigned y = 0; y < h; y++)
                        for (unsigned x = 0; x < w; x++) {
                            expected(pixels + 4 * (y * w + x), x, y, z, m);
                            for (unsigned c = 0; c < 4; c++)
                                halves[4 * (y * w + x) + c] = pixels[4 * (y * w + x) + c];
                            in[count++] = (struct Input){x, y, z, m};
                        }
                    unsigned bpp = half ? 8 : 16;
                    [t replaceRegion:MTLRegionMake2D(0, 0, w, h)
                          mipmapLevel:m
                                slice:z
                            withBytes:half ? (void *)halves : (void *)pixels
                          bytesPerRow:w * bpp
                        bytesPerImage:w * h * bpp];
                }
            id<MTLFunction> f = [lib newFunctionWithName:@(names[k])];
            id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:f error:&e];
            id<MTLBuffer> ib = [d newBufferWithBytes:in
                                              length:sizeof(in[0]) * count
                                             options:MTLResourceStorageModeShared];
            id<MTLBuffer> ob = [d newBufferWithLength:count * 16 options:MTLResourceStorageModeShared];
            if (!t || !f || !ps || !ib || !ob) {
                printf("case=%s create-failed %s\n", names[k], e.description.UTF8String);
                return 2;
            }
            float *out = ob.contents;
            for (unsigned i = 0; i < count * 4; i++)
                out[i] = NAN;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            [ce setTexture:t atIndex:0];
            [ce setBuffer:ib offset:0 atIndex:0];
            [ce setBuffer:ob offset:0 atIndex:1];
            [ce dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            unsigned errors = 0;
            for (unsigned i = 0; i < count; i++) {
                float v[4];
                expected(v, in[i].x, in[i].y, in[i].layer, in[i].level);
                for (unsigned c = 0; c < 4; c++)
                    errors += out[4 * i + c] != v[c];
            }
            printf("case=%s pixels=%u components=%u errors=%u status=%lu\n", names[k], count, count * 4, errors,
                   (unsigned long)cb.status);
            total += count * 4;
            failed += errors + (cb.status != MTLCommandBufferStatusCompleted);
            [ob release];
            [ib release];
            [ps release];
            [f release];
            [t release];
        }
        printf("RESULT components=%u errors=%u\n", total, failed);
        [q release];
        [lib release];
        [d release];
        return failed ? 1 : 0;
    }
}
