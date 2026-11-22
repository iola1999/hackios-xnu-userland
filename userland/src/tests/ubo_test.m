/*
 * ubo_test LIBRARY: whether a compute kernel reads Metal constant buffers at high indices and packed
 * halves correctly. The kernel (tools: ubo21.metal, `ubo_probe`) copies float4 [[buffer(3)]] and
 * [[buffer(21)]], half4 [[buffer(5)]] and [[buffer(22)]] into its output; each is set with
 * -setBytes:, as MapKit sets DaVinci's styleColor (a half4 at buffer 21).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 2) {
			fprintf(stderr, "usage: ubo_test LIBRARY.metallib\n");
			return 2;
		}
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		NSError *error = nil;
		id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
		id<MTLFunction> function = [library newFunctionWithName:@"ubo_probe"];
		id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
		if (!pipeline) {
			printf("ubo_test: no pipeline: %s\n", error.description.UTF8String);
			return 1;
		}
		id<MTLBuffer> out = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
		memset(out.contents, 0xee, 64);
		float a[4] = { 1, 2, 3, 4 }, b[4] = { 5, 6, 7, 8 };
		__fp16 c[4] = { 0.25, 0.5, 0.75, 1 }, d[4] = { 0.791, 0.776, 0.760, 1 };
		id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
		id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
		[encoder setComputePipelineState:pipeline];
		[encoder setBytes:a length:sizeof(a) atIndex:3];
		[encoder setBytes:b length:sizeof(b) atIndex:21];
		[encoder setBytes:c length:sizeof(c) atIndex:5];
		[encoder setBytes:d length:sizeof(d) atIndex:22];
		[encoder setBuffer:out offset:0 atIndex:0];
		[encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
		[encoder endEncoding];
		[commands commit];
		[commands waitUntilCompleted];
		const float *o = out.contents;
		const char *what[4] = { "float4 buffer(3)", "float4 buffer(21)", "half4 buffer(5)", "half4 buffer(22)" };
		for (int i = 0; i < 4; i++)
			printf("ubo_test: %s = %g %g %g %g\n", what[i], o[4 * i], o[4 * i + 1], o[4 * i + 2], o[4 * i + 3]);
		printf("ubo_test: command buffer status %ld %s\n", (long)commands.status,
		       commands.error ? commands.error.description.UTF8String : "");
	}
	return 0;
}
