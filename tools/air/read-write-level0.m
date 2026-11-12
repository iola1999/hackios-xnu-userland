#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static void deadline(int signal_number) {
    (void)signal_number;
    _exit(124);
}

static void expected(float v[4], unsigned x, unsigned y, unsigned z) {
    v[0] = x * .25f;
    v[1] = y * .5f;
    v[2] = z + .125f;
    v[3] = -.5f;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2)
            return 2;
        signal(SIGALRM, deadline);
        alarm(30);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!device || !library || !queue) {
            printf("create-failed error=%s\n", error.description.UTF8String);
            return 2;
        }
        printf("device=%s\n", device.name.UTF8String);
        const float delta[4] = {.5f, -.25f, 1.f, 2.f};
        unsigned total = 0, failed = 0;
        for (unsigned k = 0; k < 8; k++) {
            BOOL array = k >= 4, half = (k & 2) != 0;
            unsigned layers = array ? 3 : 1, count = 7 * 3 * layers;
            NSString *suffix = [NSString stringWithFormat:@"%s_%s_%s", array ? "array" : "2d",
                                half ? "half" : "float", k & 1 ? "ushort" : "uint"];
            MTLTextureDescriptor *desc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:half ? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA32Float
                                             width:7 height:3 mipmapped:NO];
            desc.textureType = array ? MTLTextureType2DArray : MTLTextureType2D;
            desc.arrayLength = layers;
            desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            desc.storageMode = MTLStorageModeShared;
            id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
            id<MTLFunction> functions[2];
            id<MTLComputePipelineState> pipelines[2];
            id<MTLBuffer> outputs[2];
            if (!texture)
                return 2;
            unsigned bpp = half ? 8 : 16;
            for (unsigned z = 0; z < layers; z++) {
                float pixels[84];
                _Float16 halves[84];
                for (unsigned y = 0; y < 3; y++)
                    for (unsigned x = 0; x < 7; x++) {
                        unsigned index = (y * 7 + x) * 4;
                        expected(pixels + index, x, y, z);
                        for (unsigned c = 0; c < 4; c++)
                            halves[index + c] = pixels[index + c];
                    }
                [texture replaceRegion:MTLRegionMake2D(0, 0, 7, 3) mipmapLevel:0 slice:z
                              withBytes:half ? (void *)halves : (void *)pixels bytesPerRow:7 * bpp
                          bytesPerImage:21 * bpp];
            }
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            for (unsigned pass = 0; pass < 2; pass++) {
                functions[pass] = [library newFunctionWithName:[NSString stringWithFormat:@"%s_%@",
                                                               pass ? "read" : "rw", suffix]];
                pipelines[pass] = [device newComputePipelineStateWithFunction:functions[pass] error:&error];
                outputs[pass] = [device newBufferWithLength:count * 16 options:MTLResourceStorageModeShared];
                if (!functions[pass] || !pipelines[pass] || !outputs[pass]) {
                    printf("case=%s create-failed error=%s\n", suffix.UTF8String, error.description.UTF8String);
                    return 2;
                }
                float *out = outputs[pass].contents;
                for (unsigned i = 0; i < count * 4; i++)
                    out[i] = NAN;
                id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
                [encoder setComputePipelineState:pipelines[pass]];
                [encoder setTexture:texture atIndex:0];
                [encoder setBuffer:outputs[pass] offset:0 atIndex:0];
                [encoder dispatchThreads:MTLSizeMake(7, 3, layers) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [encoder endEncoding];
            }
            [cb commit];
            [cb waitUntilCompleted];
            unsigned errors[3] = {0};
            for (unsigned z = 0; z < layers; z++) {
                float pixels[84];
                _Float16 halves[84];
                [texture getBytes:half ? (void *)halves : (void *)pixels bytesPerRow:7 * bpp bytesPerImage:21 * bpp
                       fromRegion:MTLRegionMake2D(0, 0, 7, 3) mipmapLevel:0 slice:z];
                for (unsigned y = 0; y < 3; y++)
                    for (unsigned x = 0; x < 7; x++) {
                        float value[4];
                        expected(value, x, y, z);
                        for (unsigned c = 0; c < 4; c++) {
                            unsigned local = (y * 7 + x) * 4 + c, index = z * 84 + local;
                            errors[0] += ((float *)outputs[0].contents)[index] != value[c];
                            errors[1] += ((float *)outputs[1].contents)[index] != value[c] + delta[c];
                            float actual = half ? (float)halves[local] : pixels[local];
                            errors[2] += actual != value[c] + delta[c];
                        }
                    }
            }
            printf("case=%s components=%u before_errors=%u after_errors=%u readback_errors=%u status=%lu\n",
                   suffix.UTF8String, count * 12, errors[0], errors[1], errors[2], (unsigned long)cb.status);
            total += count * 12;
            failed += errors[0] + errors[1] + errors[2] + (cb.status != MTLCommandBufferStatusCompleted);
            for (unsigned pass = 0; pass < 2; pass++) {
                [outputs[pass] release];
                [pipelines[pass] release];
                [functions[pass] release];
            }
            [texture release];
        }
        printf("RESULT cases=8 components=%u errors=%u\n", total, failed);
        [queue release];
        [library release];
        [device release];
        return failed ? 1 : 0;
    }
}
