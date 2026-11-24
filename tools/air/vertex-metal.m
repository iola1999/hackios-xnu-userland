/* Draw the captured vertex module over the same 128 x 128 coverage target as air_vertex. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int main(int argc, const char **argv)
{
    @autoreleasepool {
        if (argc != 7) {
            fprintf(stderr, "usage: %s LIBRARY FUNCTION INPUTS VERTICES INSTANCES OUTPUT.rgba\n", argv[0]);
            return 2;
        }
        NSError *error = nil;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSURL *url = [NSURL fileURLWithPath:@(argv[1])];
        id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
        id<MTLFunction> vertex = [library newFunctionWithName:@(argv[2])];
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 coverage() { return 1.0f; }";
        id<MTLLibrary> marker = [device newLibraryWithSource:source options:nil error:&error];
        if (!vertex || !marker) { NSLog(@"loading vertex/fragment functions: %@", error); return 1; }
        MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = vertex;
        desc.fragmentFunction = [marker newFunctionWithName:@"coverage"];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!pipeline) { NSLog(@"creating pipeline: %@", error); return 1; }

        const NSUInteger side = 128;
        MTLTextureDescriptor *textureDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatRGBA8Unorm width:side height:side mipmapped:NO];
        textureDesc.storageMode = MTLStorageModeShared;
        textureDesc.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:textureDesc];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,0);

        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return 1;
        [encoder setRenderPipelineState:pipeline];
        for (NSUInteger i = 0; i < 31; i++) {
            NSString *path = [@(argv[3]) stringByAppendingPathComponent:[NSString stringWithFormat:@"%lu.bin", (unsigned long)i]];
            if (![[NSFileManager defaultManager] fileExistsAtPath:path]) continue;
            NSData *data = [NSData dataWithContentsOfFile:path];
            if (!data.length) { NSLog(@"empty buffer %@", path); return 1; }
            id<MTLBuffer> buffer = [device newBufferWithBytes:data.bytes length:data.length options:MTLResourceStorageModeShared];
            if (!buffer) { NSLog(@"cannot allocate buffer %@", path); return 1; }
            [encoder setVertexBuffer:buffer offset:0 atIndex:i];
        }
        NSInteger vertices = [@(argv[4]) integerValue], instances = [@(argv[5]) integerValue];
        if (vertices <= 0 || instances <= 0) return 2;
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertices instanceCount:instances];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) { NSLog(@"draw: %@", command.error); return 1; }
        NSMutableData *pixels = [NSMutableData dataWithLength:side * side * 4];
        [texture getBytes:pixels.mutableBytes bytesPerRow:side * 4 fromRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0];
        return [pixels writeToFile:@(argv[6]) atomically:YES] ? 0 : 1;
    }
}
