/* Compare production NIR results with Apple's Metal cospi on the host GPU.
 * Input is the (float input, float result) stream from cospi-nir.c. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2 || argc > 3) return 2;
        NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]];
        if (data.length != 81920 * 2 * sizeof(float)) return 2;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return 2;
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void check(device const float2 *in [[buffer(0)]], "
            "device float *out [[buffer(1)]], uint i [[thread_position_in_grid]]) { "
            "out[i] = i < 65536 ? float(cospi(half(in[i].x))) : cospi(in[i].x); }";
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.fastMathEnabled = NO;
        NSError *error = nil;
        id<MTLLibrary> library = argc == 3 ? [device newLibraryWithURL:
            [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]] error:&error] :
            [device newLibraryWithSource:source options:options error:&error];
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:
            [library newFunctionWithName:@"check"] error:&error];
        if (!pipeline) { NSLog(@"%@", error); return 2; }
        id<MTLBuffer> input = [device newBufferWithBytes:data.bytes length:data.length options:MTLResourceStorageModeShared];
        id<MTLBuffer> output = [device newBufferWithLength:data.length / 2 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(81920, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted) return 2;
        const float *pairs = input.contents, *native = output.contents;
        double maximum[2] = {0};
        for (unsigned i = 0; i < 81920; i++) {
            float x = pairs[i * 2], nir = pairs[i * 2 + 1], got = native[i];
            double reference = __cospi((double)x);
            BOOL half = i < 65536;
            int exponent = 0;
            if (isfinite(reference)) frexp(reference, &exponent);
            double ulp = ldexp(1.0, MAX(exponent - (half ? 11 : 24), half ? -24 : -149));
            double difference = fabs(nir - got) / ulp;
            BOOL ok = isnan(reference) ? isnan(got) :
                isfinite(got) && fabs(got - reference) <= ulp * (half ? 1 : 4);
            /* MSL specifies cos(pi*x), without a zero-sign rule. Native half
             * and float cospi can return different zero signs at the same x. */
            if (reference == 0) ok = got == 0;
            if (!ok) {
                fprintf(stderr, "native cospi input=%a NIR=%a native=%a\n", x, nir, got);
                return 1;
            }
            if (isfinite(difference)) maximum[half ? 0 : 1] = MAX(maximum[half ? 0 : 1], difference);
        }
        printf("PASS 81920 native Metal comparisons; maximum difference half=%g ULP float=%g ULP\n", maximum[0], maximum[1]);
    }
    return 0;
}
