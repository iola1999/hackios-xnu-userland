/* Exercise actual Metal library, function, pipeline and encoder objects without
 * registering a default device. This fixed-input check covers the selected AIR
 * entrypoints; system-library compatibility and GPU submissions need separate
 * checks. The IOSurface mode also releases caller references before a recorded
 * command is committed, exercising command-buffer resource ownership. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *phase = "arguments";
static unsigned computes, draws, copies, completed_buffers;
static const float positions[4][4] = {
    { -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 }
};
/* Metal maps clip-space +Y to the top of the viewport. Texture row zero is
 * sampled there; nearest filtering maps each 8x8 screen block to one texel. */
static const float coordinates[4][2] = { { 0, 1 }, { 1, 1 }, { 0, 0 }, { 1, 0 } };

static void stage(const char *name)
{
    phase = name;
    printf("metalpipeline26: stage=%s\n", phase);
}

static BOOL fail(const char *format, ...)
{
    va_list args;
    printf("metalpipeline26: FAIL stage=%s: ", phase);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    return NO;
}

static double milliseconds(void)
{
    struct timespec now = { 0 };
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

static BOOL submit(id<MTLCommandBuffer> command, const char *name)
{
    stage(name);
    if (!command) return fail("command buffer is nil");
    double begin = milliseconds();
    [command commit];
    printf("metalpipeline26: commit status=%lu error=%s elapsed_ms=%.3f\n",
        (unsigned long)command.status, command.error.description.UTF8String ?: "-",
        milliseconds() - begin);
    if (command.status == MTLCommandBufferStatusError || command.error)
        return fail("commit failed");
    [command waitUntilCompleted];
    printf("metalpipeline26: wait status=%lu error=%s total_ms=%.3f\n",
        (unsigned long)command.status, command.error.description.UTF8String ?: "-",
        milliseconds() - begin);
    if (command.status != MTLCommandBufferStatusCompleted || command.error)
        return fail("command did not complete successfully");
    completed_buffers++;
    return YES;
}

static id<MTLLibrary> load_library(id<MTLDevice> device, const char *path, const char *expected_hash)
{
    NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
    if (!data.length || data.length > UINT32_MAX) {
        fail("cannot read metallib %s", path);
        return nil;
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    char hash[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    for (unsigned i = 0; i < sizeof(digest); i++) snprintf(hash + 2 * i, 3, "%02x", digest[i]);
    printf("metalpipeline26: metallib=%s bytes=%lu sha256=%s\n", path, (unsigned long)data.length, hash);
    if (expected_hash && strcmp(hash, expected_hash)) {
        fail("metallib hash does not match the fixed shader input");
        return nil;
    }
    dispatch_data_t wrapped = dispatch_data_create(data.bytes, data.length, NULL,
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    id<MTLLibrary> library = nil;
    @try { library = [[device newLibraryWithData:wrapped error:&error] autorelease]; }
    @finally { dispatch_release(wrapped); }
    if (!library || error) {
        fail("newLibraryWithData: %s", error.description.UTF8String ?: "nil library");
        return nil;
    }
    printf("metalpipeline26: library_class=%s functions=%lu\n", object_getClassName(library),
        (unsigned long)library.functionNames.count);
    return library;
}

static id<MTLFunction> function(id<MTLLibrary> library, NSString *name, MTLFunctionType type)
{
    id<MTLFunction> result = [[library newFunctionWithName:name] autorelease];
    SEL selector = sel_registerName("bitcodeData");
    if (!result || result.functionType != type || ![result respondsToSelector:selector]) {
        fail("function %s missing, wrong type, or lacks bitcodeData", name.UTF8String);
        return nil;
    }
    NSData *bitcode = ((NSData *(*)(id, SEL))objc_msgSend)(result, selector);
    if (!bitcode.length) {
        fail("function %s has empty bitcodeData", name.UTF8String);
        return nil;
    }
    printf("metalpipeline26: function=%s type=%lu class=%s bitcode_bytes=%lu\n",
        name.UTF8String, (unsigned long)type, object_getClassName(result), (unsigned long)bitcode.length);
    return result;
}

static id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format, unsigned width,
                             unsigned height, MTLTextureUsage usage)
{
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
        width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = usage;
    id<MTLTexture> result = [[device newTextureWithDescriptor:descriptor] autorelease];
    if (!result || result.pixelFormat != format || result.width != width || result.height != height)
        { fail("texture creation/geometry failed format=%lu %ux%u", (unsigned long)format, width, height); return nil; }
    if (result.iosurface != NULL || result.iosurfacePlane != 0)
        { fail("ordinary texture must report iosurface=nil and plane=0"); return nil; }
    return result;
}

static BOOL compare(const uint8_t *actual, const uint8_t *expected, size_t length, size_t pitch)
{
    for (size_t i = 0; i < length; i++) {
        if (actual[i] != expected[i])
            return fail("readback row=%zu byte_in_row=%zu actual=%02x expected=%02x",
                i / pitch, i % pitch, actual[i], expected[i]);
    }
    printf("metalpipeline26: readback verified=%zu bytes pitch=%zu\n", length, pitch);
    return YES;
}

static id<MTLTexture> compute_pattern(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                     id<MTLFunction> kernel)
{
    stage("compute pipeline");
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline = [[device newComputePipelineStateWithFunction:kernel error:&error] autorelease];
    if (!pipeline || error) { fail("compute pipeline: %s", error.description.UTF8String ?: "nil"); return nil; }
    stage("compute texture and encoder");
    id<MTLTexture> output = texture(device, MTLPixelFormatRGBA16Float, 8, 8,
        MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    if (!output) return nil;
    uint16_t sentinel[8 * 8 * 4];
    for (unsigned i = 0; i < sizeof(sentinel) / sizeof(sentinel[0]); i++) sentinel[i] = 0x3800;
    [output replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:sentinel bytesPerRow:64];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder || encoder.device != device) { fail("compute encoder creation failed"); return nil; }
    @try {
        [encoder setComputePipelineState:pipeline];
        [encoder setTexture:output atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake(2, 2, 1) threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
        computes++;
    } @finally { [encoder endEncoding]; }
    if (!submit(command, "compute commit/wait")) return nil;
    stage("compute RGBA16Float readback");
    enum { Pitch = 80 };
    uint8_t actual[8 * Pitch], expected[8 * Pitch];
    memset(actual, 0xcc, sizeof(actual));
    memset(expected, 0xcc, sizeof(expected));
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            uint16_t pixel[4] = { (x & 1) ? 0x3c00 : 0, (y & 1) ? 0x3c00 : 0,
                (((x >> 1) ^ (y >> 1)) & 1) ? 0x3c00 : 0, 0x3c00 };
            memcpy(expected + y * Pitch + x * 8, pixel, sizeof(pixel));
        }
    }
    [output getBytes:actual bytesPerRow:Pitch fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    return compare(actual, expected, sizeof(actual), Pitch) ? output : nil;
}

static id<MTLRenderPipelineState> render_pipeline(id<MTLDevice> device, id<MTLFunction> vertex,
                                                 id<MTLFunction> fragment, unsigned count)
{
    MTLRenderPipelineDescriptor *descriptor = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.rasterSampleCount = 1;
    for (unsigned i = 0; i < count; i++) descriptor.colorAttachments[i].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *error = nil;
    id<MTLRenderPipelineState> result = [[device newRenderPipelineStateWithDescriptor:descriptor error:&error] autorelease];
    if (!result || error) { fail("render pipeline: %s", error.description.UTF8String ?: "nil"); return nil; }
    return result;
}

static void fill_bgra(uint8_t *bytes, size_t pitch, BOOL pattern)
{
    for (unsigned y = 0; y < 64; y++) {
        for (unsigned x = 0; x < 64; x++) {
            unsigned tx = x / 8, ty = y / 8;
            uint8_t pixel[4] = { 255, 0, 255, 255 };
            if (pattern) {
                pixel[0] = (((tx >> 1) ^ (ty >> 1)) & 1) * 255;
                pixel[1] = (ty & 1) * 255;
                pixel[2] = (tx & 1) * 255;
            }
            memcpy(bytes + y * pitch + x * 4, pixel, sizeof(pixel));
        }
    }
}

static BOOL check_bgra(id<MTLTexture> output, BOOL pattern)
{
    enum { Pitch = 64 * 4 + 32 };
    uint8_t actual[64 * Pitch], expected[64 * Pitch];
    memset(actual, 0xcc, sizeof(actual));
    memset(expected, 0xcc, sizeof(expected));
    fill_bgra(expected, Pitch, pattern);
    [output getBytes:actual bytesPerRow:Pitch fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
    return compare(actual, expected, sizeof(actual), Pitch);
}

static BOOL check_surface(IOSurfaceRef surface, BOOL pattern)
{
    stage("IOSurface CPU lock/read after GPU completion");
    size_t size = IOSurfaceGetAllocSize(surface), pitch = IOSurfaceGetBytesPerRow(surface);
    uint8_t *expected = malloc(size);
    if (!expected) return fail("IOSurface expected bytes allocation failed");
    memset(expected, 0xa5, size);
    fill_bgra(expected, pitch, pattern);
    uint32_t seed = 0;
    kern_return_t status = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, &seed);
    BOOL valid = NO;
    if (status == KERN_SUCCESS) {
        valid = compare(IOSurfaceGetBaseAddress(surface), expected, size, pitch);
        status = IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, &seed);
    }
    free(expected);
    printf("metalpipeline26: IOSurface CPU read seed=%u lock/unlock=%#x\n", seed, status);
    return status == KERN_SUCCESS ? valid : fail("IOSurface read lock/unlock failed");
}

static BOOL draw_test(id<MTLDevice> device, id<MTLCommandQueue> queue,
                     id<MTLFunction> vertex, id<MTLFunction> fragment, id<MTLTexture> input,
                     id<MTLTexture> destination, BOOL expected_pattern, IOSurfaceRef surface)
{
    BOOL sampling = input != nil;
    unsigned count = sampling ? 4 : 1;
    stage(sampling ? "sample render pipeline" : "constant render pipeline");
    id<MTLRenderPipelineState> pipeline = render_pipeline(device, vertex, fragment, count);
    if (!pipeline) return NO;
    stage(sampling ? "sample render resources" : "constant render resources");
    id<MTLTexture> outputs[4] = { nil };
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    for (unsigned i = 0; i < count; i++) {
        outputs[i] = i == 0 && destination ? destination :
            texture(device, MTLPixelFormatBGRA8Unorm, 64, 64,
                MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        if (!outputs[i]) return NO;
        pass.colorAttachments[i].texture = outputs[i];
        pass.colorAttachments[i].loadAction = MTLLoadActionClear;
        pass.colorAttachments[i].storeAction = MTLStoreActionStore;
        pass.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
    }
    id<MTLBuffer> position_buffer = [[device newBufferWithBytes:positions length:sizeof(positions)
        options:MTLResourceStorageModeShared] autorelease];
    id<MTLBuffer> coordinate_buffer = nil, color_buffer = nil;
    id<MTLSamplerState> sampler = nil;
    if (sampling) {
        coordinate_buffer = [[device newBufferWithBytes:coordinates length:sizeof(coordinates)
            options:MTLResourceStorageModeShared] autorelease];
        MTLSamplerDescriptor *descriptor = [[[MTLSamplerDescriptor alloc] init] autorelease];
        descriptor.minFilter = MTLSamplerMinMagFilterNearest;
        descriptor.magFilter = MTLSamplerMinMagFilterNearest;
        descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
        descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        descriptor.normalizedCoordinates = YES;
        sampler = [[device newSamplerStateWithDescriptor:descriptor] autorelease];
        if (!coordinate_buffer || !sampler) return fail("texture-coordinate buffer or sampler creation failed");
    } else {
        // clearFragment01's constant buffer contains float4, int4 and uint4.
        const float color[12] = { 1, 0, 1, 1 };
        color_buffer = [[device newBufferWithBytes:color length:sizeof(color)
            options:MTLResourceStorageModeShared] autorelease];
        if (!color_buffer) return fail("fragment constant buffer creation failed");
    }
    if (!position_buffer) return fail("position buffer creation failed");
    stage(sampling ? "sample render encoding" : "constant render encoding");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder || encoder.device != device) return fail("render encoder creation failed");
    @try {
        [encoder setRenderPipelineState:pipeline];
        [encoder setViewport:(MTLViewport){ 0, 0, 64, 64, 0, 1 }];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setVertexBuffer:position_buffer offset:0 atIndex:sampling ? 0 : 14];
        if (sampling) {
            [encoder setVertexBuffer:coordinate_buffer offset:0 atIndex:1];
            [encoder setFragmentTexture:input atIndex:0];
            [encoder setFragmentSamplerState:sampler atIndex:0];
        } else {
            [encoder setFragmentBuffer:color_buffer offset:0 atIndex:14];
        }
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        draws++;
    } @finally { [encoder endEncoding]; }
    if (surface) {
        /* Keep blit transfer/map operations after the direct CPU visibility check. */
        if (!submit(command, "IOSurface render-only commit/wait") ||
            !check_surface(surface, expected_pattern)) return NO;
        command = [queue commandBuffer];
        if (!command) return fail("IOSurface post-read blit command buffer is nil");
    }
    id<MTLTexture> copied = nil;
    if (sampling) {
        stage("sample output blit encoding");
        copied = texture(device, MTLPixelFormatBGRA8Unorm, 64, 64, MTLTextureUsageShaderRead);
        if (!copied) return NO;
        uint8_t sentinel[64 * 64 * 4];
        memset(sentinel, 0x35, sizeof(sentinel));
        [copied replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0
            withBytes:sentinel bytesPerRow:256];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!blit || blit.device != device) return fail("blit encoder creation failed");
        @try {
            [blit copyFromTexture:outputs[0] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(64, 64, 1) toTexture:copied destinationSlice:0 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            copies++;
        } @finally { [blit endEncoding]; }
    }
    if (!submit(command, surface ? "IOSurface post-read blit commit/wait" :
        sampling ? "sample draw/blit commit/wait" : "constant draw commit/wait")) return NO;
    for (unsigned i = 0; i < count; i++) {
        stage(sampling ? "sample attachment readback" : "constant attachment readback");
        printf("metalpipeline26: attachment=%u\n", i);
        if (!check_bgra(outputs[i], expected_pattern)) return NO;
    }
    if (copied) { stage("blit target readback"); if (!check_bgra(copied, expected_pattern)) return NO; }
    return YES;
}

static BOOL write_surface(IOSurfaceRef surface)
{
    stage("IOSurface CPU write magenta and sentinels");
    uint32_t seed = 0;
    kern_return_t status = IOSurfaceLock(surface, 0, &seed);
    if (status != KERN_SUCCESS) return fail("IOSurface write lock=%#x", status);
    uint8_t *bytes = IOSurfaceGetBaseAddress(surface);
    memset(bytes, 0xa5, IOSurfaceGetAllocSize(surface));
    fill_bgra(bytes, IOSurfaceGetBytesPerRow(surface), NO);
    status = IOSurfaceUnlock(surface, 0, &seed);
    printf("metalpipeline26: IOSurface CPU write seed=%u unlock=%#x\n", seed, status);
    return status == KERN_SUCCESS ? YES : fail("IOSurface write unlock failed");
}

static BOOL surface_test(id<MTLDevice> device, id<MTLCommandQueue> queue,
                         id<MTLFunction> vertex, id<MTLFunction> fragment, id<MTLTexture> pattern)
{
    stage("IOSurface create/import plane zero");
    enum { Pitch = 320, Rows = 64 };
    NSDictionary *properties = @{
        (id)kIOSurfaceWidth: @64, (id)kIOSurfaceHeight: @64,
        (id)kIOSurfacePixelFormat: @0x42475241, (id)kIOSurfaceBytesPerElement: @4,
        (id)kIOSurfaceBytesPerRow: @(Pitch), (id)kIOSurfaceAllocSize: @(Pitch * Rows + 64)
    };
    IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)properties);
    if (!surface) return fail("IOSurfaceCreate returned NULL");
    id<MTLTexture> imported = nil;
    BOOL valid = NO;
    BOOL owns_surface = YES;
    @try {
        size_t size = IOSurfaceGetAllocSize(surface);
        printf("metalpipeline26: IOSurface id=%u dimensions=%zux%zu pitch=%zu alloc=%zu planes=%zu\n",
            IOSurfaceGetID(surface), IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
            IOSurfaceGetBytesPerRow(surface), size, IOSurfaceGetPlaneCount(surface));
        if (!IOSurfaceGetID(surface) || IOSurfaceGetWidth(surface) != 64 ||
            IOSurfaceGetHeight(surface) != Rows || IOSurfaceGetBytesPerRow(surface) != Pitch ||
            IOSurfaceGetPixelFormat(surface) != 0x42475241 || size < Pitch * Rows + 64 ||
            size > 65536 || !IOSurfaceGetBaseAddress(surface)) {
            fail("IOSurface geometry/allocation differs from requested bounded layout");
            return NO;
        }
        if (!write_surface(surface)) return NO;
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        imported = [device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
        if (!imported || imported.width != 64 || imported.height != 64 ||
            imported.pixelFormat != MTLPixelFormatBGRA8Unorm) return fail("IOSurface import failed");
        if (imported.iosurface != surface || imported.iosurfacePlane != 0)
            return fail("imported IOSurface identity/plane property mismatch");
        id<MTLTexture> views[3] = {
            [imported newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm],
            [imported newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm
                textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)],
            [imported newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm
                textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)
                swizzle:MTLTextureSwizzleChannelsDefault]
        };
        BOOL refused = YES;
        for (unsigned i = 0; i < 3; i++) { refused &= views[i] == nil; [views[i] release]; }
        if (!refused) return fail("texture views unexpectedly accepted");
        printf("metalpipeline26: IOSurface identity/plane verified; three view forms refused\n");
        CFRelease(surface);
        owns_surface = NO;
        surface = imported.iosurface; /* Borrowed while the owned texture remains alive. */
        printf("metalpipeline26: caller IOSurface reference released before shared-content commands\n");
        @autoreleasepool {
            stage("IOSurface CPU-written pixels sampled by GPU");
            if (!draw_test(device, queue, vertex, fragment, imported, nil, NO, NULL)) return NO;
            stage("IOSurface GPU pattern write and CPU mapping read");
            if (!draw_test(device, queue, vertex, fragment, pattern, imported, YES, surface)) return NO;
            if (!write_surface(surface)) return NO;
            stage("IOSurface CPU overwrite sampled by GPU after prior GPU write");
            if (!draw_test(device, queue, vertex, fragment, imported, nil, NO, NULL)) return NO;
            if (!check_surface(surface, NO)) return NO;
        }
        /* Drain earlier command buffers before testing the last CB's resource ownership. */
        @autoreleasepool {
            stage("IOSurface caller texture release before final blit commit");
            id<MTLTexture> copied = texture(device, MTLPixelFormatBGRA8Unorm, 64, 64, MTLTextureUsageShaderRead);
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            if (!copied || !command || !blit) return fail("lifetime blit resources unavailable");
            @try {
                [blit copyFromTexture:imported sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(64, 64, 1) toTexture:copied destinationSlice:0 destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
                copies++;
            } @finally { [blit endEncoding]; }
            [imported release];
            imported = nil;
            surface = NULL;
            printf("metalpipeline26: caller texture reference released; final CB owns recorded source\n");
            if (!submit(command, "IOSurface released-source blit commit/wait")) return NO;
            valid = check_bgra(copied, NO);
        }
    } @finally {
        [imported release];
        if (owns_surface) CFRelease(surface);
    }
    if (valid) printf("metalpipeline26: IOSurface shared-content and caller-release lifetime PASS\n");
    return valid;
}

int main(int argc, char **argv)
{
    BOOL software = NO;
    BOOL surface_mode = NO;
    const char *library_path = "/redwood/lib/libredwoodmetal.dylib";
    const char *compute_path = "/redwood/share/metal-pipeline-pattern.metallib";
    const char *system_path = "/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--software")) software = YES;
        else if (!strcmp(argv[i], "--iosurface")) surface_mode = YES;
        else if (!strcmp(argv[i], "--library") && i + 1 < argc) library_path = argv[++i];
        else if (!strcmp(argv[i], "--compute") && i + 1 < argc) compute_path = argv[++i];
        else if (!strcmp(argv[i], "--system-shaders") && i + 1 < argc) system_path = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--software] [--iosurface] [--library DYLIB] [--compute METALLIB] [--system-shaders METALLIB]\n", argv[0]);
            return 2;
        }
    }
    if (surface_mode && software) {
        fprintf(stderr, "metalpipeline26: --iosurface requires hardware import; softpipe imports no shared IOSurface memory\n");
        return 2;
    }
    int console = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (console >= 0) {
        dup2(console, STDOUT_FILENO);
        dup2(console, STDERR_FILENO);
        if (console > STDERR_FILENO) close(console);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (software) setenv("RWA_SOFTWARE", "1", 1);
    else unsetenv("RWA_SOFTWARE");
    void *plugin = NULL, *backend = NULL;
    int result = 1;
    @autoreleasepool {
        @try {
            NSDictionary *version = [NSDictionary dictionaryWithContentsOfFile:@"/System/Library/CoreServices/SystemVersion.plist"];
            printf("metalpipeline26: build=%s mode=%s library=%s\n",
                [version[@"ProductBuildVersion"] UTF8String] ?: "unknown", software ? "software" : "hardware", library_path);
            stage("dlopen Metal candidate");
            plugin = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
            if (!plugin) { fail("dlopen: %s", dlerror()); goto finished; }
            Class cls = objc_getClass("RedwoodMetalDevice");
            SEL init = sel_registerName("initWithAcceleratorPort:");
            if (!cls || ![cls instancesRespondToSelector:init]) { fail("device initializer unavailable"); goto finished; }
            stage("initWithAcceleratorPort:0");
            id<MTLDevice> device = [((id (*)(id, SEL, uint32_t))objc_msgSend)([cls alloc], init, 0) autorelease];
            SEL mesa_selector = sel_registerName("mesaDevice");
            if (!device || ![device respondsToSelector:mesa_selector]) { fail("device/Mesa accessor unavailable"); goto finished; }
            void *mesa = ((void *(*)(id, SEL))objc_msgSend)(device, mesa_selector);
            backend = dlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW | RTLD_NOLOAD);
            const char *(*backend_name)(void *) = backend ? dlsym(backend, "rwa_device_name") : NULL;
            const char *name = mesa && backend_name ? backend_name(mesa) : NULL;
            if (!name || (!!strstr(name, "softpipe") != !!software)) {
                fail("requested mode does not match backend %s", name ?: "null"); goto finished;
            }
            printf("metalpipeline26: device_class=%s backend=%s _MTLDevice_size=%zu\n",
                object_getClassName(device), name, class_getInstanceSize(objc_getClass("_MTLDevice")));
            stage("newCommandQueue");
            id<MTLCommandQueue> queue = [[device newCommandQueue] autorelease];
            if (!queue || queue.device != device) { fail("queue creation/association failed"); goto finished; }
            stage("compute newLibrary/newFunction");
            id<MTLLibrary> compute_library = load_library(device, compute_path, NULL);
            if (!compute_library) goto finished;
            id<MTLFunction> kernel = function(compute_library, @"pipeline_pattern", MTLFunctionTypeKernel);
            if (!kernel) goto finished;
            id<MTLTexture> input = compute_pattern(device, queue, kernel);
            if (!input) goto finished;
            stage("23G90 newLibrary/newFunction");
            id<MTLLibrary> system_library = load_library(device, system_path,
                "efe897e875b709b1aa8c89a4729067a7798ffe7eb2a77ddbead0a7eb405455c4");
            if (!system_library) goto finished;
            id<MTLFunction> clear_vertex = function(system_library, @"clearVertex", MTLFunctionTypeVertex);
            id<MTLFunction> clear_fragment = function(system_library, @"clearFragment01", MTLFunctionTypeFragment);
            if (!clear_vertex || !clear_fragment || !draw_test(device, queue, clear_vertex, clear_fragment, nil, nil, NO, NULL)) goto finished;
            stage("23G90 sampling newFunction");
            id<MTLFunction> sample_vertex = function(system_library, @"texturedVertex2D", MTLFunctionTypeVertex);
            id<MTLFunction> sample_fragment = function(system_library, @"texturedFragment2DFloat", MTLFunctionTypeFragment);
            if (!sample_vertex || !sample_fragment || !draw_test(device, queue, sample_vertex, sample_fragment, input, nil, YES, NULL)) goto finished;
            if (surface_mode && !surface_test(device, queue, sample_vertex, sample_fragment, input)) goto finished;
            if (computes != 1 || draws != (surface_mode ? 5u : 2u) ||
                copies != (surface_mode ? 5u : 1u) || completed_buffers != (surface_mode ? 8u : 3u)) {
                fail("incomplete pipeline coverage"); goto finished;
            }
            result = 0;
        finished:;
        } @catch (NSException *exception) {
            fail("exception %s: %s", exception.name.UTF8String, exception.reason.UTF8String);
        }
    }
    if (backend) dlclose(backend);
    if (plugin) dlclose(plugin);
    printf("metalpipeline26: %s mode=%s iosurface=%d compute=%u draw=%u blit=%u completed_buffers=%u\n",
        result ? "FAIL" : "PASS", software ? "software" : "hardware",
        surface_mode, computes, draws, copies, completed_buffers);
    return result;
}
