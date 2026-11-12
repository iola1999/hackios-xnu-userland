/* RGB8_444_2P read contract: full-size G and interleaved B,R IOSurface planes. */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <mach/kern_return.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { Width = 17, Height = 9, Pixels = Width * Height, Format = 542 };
static const OSType FourCC = 0x5f524234;
static size_t (*plane_offset)(IOSurfaceRef, size_t);

static void deadline(int number)
{
    (void)number;
    static const char message[] = "rgb8-444-2p: 30-second deadline exceeded\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void pixel(unsigned x, unsigned y, uint8_t rgb[3])
{
    rgb[0] = (uint8_t)(17 * x + 31 * y + 11);
    rgb[1] = (uint8_t)(29 * x + 7 * y + 53);
    rgb[2] = 255 - rgb[0]; /* Every pixel detects a B/R exchange. */
}

static int create_surface(BOOL half, IOSurfaceRef *result)
{
    size_t widths[2] = {Width, half ? (Width + 1) / 2 : Width};
    size_t heights[2] = {Height, half ? (Height + 1) / 2 : Height};
    size_t rows[2] = {IOSurfaceAlignProperty(kIOSurfacePlaneBytesPerRow, Width), 0};
    rows[1] = IOSurfaceAlignProperty(kIOSurfacePlaneBytesPerRow, rows[0] + 128);
    size_t sizes[2] = {IOSurfaceAlignProperty(kIOSurfacePlaneSize, rows[0] * Height),
                       IOSurfaceAlignProperty(kIOSurfacePlaneSize, rows[1] * Height)};
    size_t offsets[2] = {IOSurfaceAlignProperty(kIOSurfacePlaneOffset, 4096), 0};
    offsets[1] = IOSurfaceAlignProperty(kIOSurfacePlaneOffset, offsets[0] + sizes[0] + 4096);
    size_t allocation = offsets[1] + sizes[1];
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned p = 0; p < 2; p++)
        [planes addObject:@{(id)kIOSurfacePlaneWidth:@(widths[p]),
            (id)kIOSurfacePlaneHeight:@(heights[p]), (id)kIOSurfacePlaneBytesPerElement:@(p + 1),
            (id)kIOSurfacePlaneBytesPerRow:@(rows[p]), (id)kIOSurfacePlaneOffset:@(offsets[p]),
            (id)kIOSurfacePlaneSize:@(sizes[p]), (id)kIOSurfacePlaneElementWidth:@1}];
    /* The invalid half-plane case keeps the full allocation and strides, including
     * all rows a full-size RGB8_444_2P implementation could access. */
    IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)@{
        (id)kIOSurfaceWidth:@(Width), (id)kIOSurfaceHeight:@(Height),
        (id)kIOSurfacePixelFormat:@(FourCC), (id)kIOSurfacePlaneInfo:planes,
        (id)kIOSurfaceAllocSize:@(allocation)});
    if (!surface) {
        printf("%s mode=%s stage=surface-create\n", half ? "REJECTED" : "ERROR", half ? "half-plane" : "normal");
        return half ? 78 : 2;
    }
    size_t actual_allocation = IOSurfaceGetAllocSize(surface);
    BOOL valid = IOSurfaceGetPixelFormat(surface) == FourCC && IOSurfaceGetPlaneCount(surface) == 2 &&
        IOSurfaceGetWidth(surface) == Width && IOSurfaceGetHeight(surface) == Height &&
        actual_allocation >= allocation && offsets[0] > 0 && rows[0] != rows[1];
    printf("surface half=%d width=%u height=%u allocation=%zu\n", half, Width, Height, actual_allocation);
    for (unsigned p = 0; p < 2; p++) {
        size_t width = IOSurfaceGetWidthOfPlane(surface, p), height = IOSurfaceGetHeightOfPlane(surface, p);
        size_t row = IOSurfaceGetBytesPerRowOfPlane(surface, p), offset = plane_offset(surface, p);
        size_t bpe = IOSurfaceGetBytesPerElementOfPlane(surface, p);
        printf("plane=%u width=%zu height=%zu row=%zu offset=%zu bpe=%zu reserved=%zu\n",
            p, width, height, row, offset, bpe, sizes[p]);
        valid &= width == widths[p] && height == heights[p] && row == rows[p] &&
            offset == offsets[p] && bpe == p + 1 && row >= Width * bpe &&
            offset <= actual_allocation && row <= (actual_allocation - offset) / Height;
    }
    if (!valid) {
        puts("ERROR stage=surface-geometry reason=requested-layout-not-preserved");
        CFRelease(surface);
        return 2;
    }
    *result = surface;
    return 0;
}

static id<MTLTexture> import_texture(id<MTLDevice> device, IOSurfaceRef surface)
{
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        (MTLPixelFormat)Format width:Width height:Height mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:desc iosurface:surface plane:0];
}

static BOOL texture_matches(id<MTLTexture> texture, IOSurfaceRef surface)
{
    return texture.pixelFormat == Format && texture.width == Width && texture.height == Height &&
        texture.iosurfacePlane == 0 && texture.iosurface == surface;
}

static int fill_surface(IOSurfaceRef surface, BOOL swap)
{
    if (IOSurfaceLock(surface, 0, NULL) != KERN_SUCCESS) return 2;
    uint8_t *allocation = IOSurfaceGetBaseAddress(surface);
    BOOL valid = allocation != NULL;
    for (unsigned p = 0; p < 2; p++)
        valid &= (uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface, p) ==
            (uintptr_t)allocation + plane_offset(surface, p);
    if (!valid) {
        IOSurfaceUnlock(surface, 0, NULL);
        puts("ERROR stage=surface-address");
        return 2;
    }
    memset(allocation, 0xcd, IOSurfaceGetAllocSize(surface));
    for (unsigned p = 0; p < 2; p++) {
        uint8_t *base = IOSurfaceGetBaseAddressOfPlane(surface, p);
        size_t row = IOSurfaceGetBytesPerRowOfPlane(surface, p);
        for (unsigned y = 0; y < IOSurfaceGetHeightOfPlane(surface, p); y++)
            for (unsigned x = 0; x < IOSurfaceGetWidthOfPlane(surface, p); x++) {
                uint8_t rgb[3]; pixel(x, y, rgb);
                if (p == 0) base[y * row + x] = rgb[1];
                else {
                    base[y * row + x * 2] = rgb[swap ? 0 : 2];
                    base[y * row + x * 2 + 1] = rgb[swap ? 2 : 0];
                }
            }
    }
    return IOSurfaceUnlock(surface, 0, NULL) == KERN_SUCCESS ? 0 : 2;
}

static int compare_pixels(id<MTLDevice> device, id<MTLTexture> texture,
                           const char *metallib, const char *mode, const char *output_path)
{
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];
    if (!data.length) { puts("ERROR stage=metallib-read"); return 2; }
    dispatch_data_t wrapped = dispatch_data_create(data.bytes, data.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> library = [[device newLibraryWithData:wrapped error:&error] autorelease];
    dispatch_release(wrapped);
    id<MTLFunction> function = [[library newFunctionWithName:@"rgb8_444_2p"] autorelease];
    id<MTLComputePipelineState> pipeline = function ?
        [[device newComputePipelineStateWithFunction:function error:&error] autorelease] : nil;
    if (!pipeline || error) {
        printf("ERROR stage=pipeline detail=%s\n", error.description.UTF8String ?: "unavailable");
        return 2;
    }
    id<MTLBuffer> output = [[device newBufferWithLength:Pixels * 8 * sizeof(float)
        options:MTLResourceStorageModeShared] autorelease];
    id<MTLCommandQueue> queue = [[device newCommandQueue] autorelease];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!output.contents || !encoder) { puts("ERROR stage=command-setup"); return 2; }
    float *values = output.contents;
    for (unsigned i = 0; i < Pixels * 8; i++) values[i] = NAN;
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake((Width + 7) / 8, (Height + 3) / 4, 1)
        threadsPerThreadgroup:MTLSizeMake(8, 4, 1)];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    printf("command status=%lu error=%s\n", (unsigned long)command.status, command.error.description.UTF8String ?: "none");
    if (command.status != MTLCommandBufferStatusCompleted || command.error) return 2;
    if (output_path && ![[NSData dataWithBytes:values length:output.length]
        writeToFile:[NSString stringWithUTF8String:output_path] atomically:YES]) {
        puts("ERROR stage=output-write"); return 2;
    }
    unsigned failures[2] = {0, 0};
    for (unsigned y = 0; y < Height; y++) for (unsigned x = 0; x < Width; x++) {
        uint8_t rgb[3]; pixel(x, y, rgb);
        float expected[] = {rgb[0] / 255.f, rgb[1] / 255.f, rgb[2] / 255.f, 1};
        for (unsigned op = 0; op < 2; op++) {
            const float *got = values + ((y * Width + x) * 2 + op) * 4;
            BOOL mismatch = NO;
            for (unsigned c = 0; c < 4; c++) mismatch |= !isfinite(got[c]) || fabsf(got[c] - expected[c]) > .0005f;
            if (mismatch && !failures[op])
                printf("first-mismatch op=%s x=%u y=%u rgba=%.9f,%.9f,%.9f,%.9f expected=%.9f,%.9f,%.9f,%.9f\n",
                    op ? "read" : "sample", x, y, got[0], got[1], got[2], got[3],
                    expected[0], expected[1], expected[2], expected[3]);
            failures[op] += mismatch;
        }
    }
    printf("RESULT mode=%s pixels=%u sample_mismatches=%u read_mismatches=%u\n",
        mode, Pixels, failures[0], failures[1]);
    return failures[0] || failures[1] ? 1 : 0;
}

static id<MTLDevice> select_device(const char *driver)
{
    if (!driver) return [MTLCreateSystemDefaultDevice() autorelease];
    unsetenv("RWA_SOFTWARE");
    if (!dlopen(driver, RTLD_NOW | RTLD_LOCAL)) {
        printf("ERROR stage=driver-load detail=%s\n", dlerror()); return nil;
    }
    Class cls = objc_getClass("AnnibaleMetalDevice");
    SEL initialize = sel_registerName("initWithAcceleratorPort:");
    if (!cls || ![cls instancesRespondToSelector:initialize]) return nil;
    id<MTLDevice> device = [((id (*)(id, SEL, uint32_t))objc_msgSend)([cls alloc], initialize, 0) autorelease];
    SEL getter = sel_registerName("mesaDevice");
    void *air = dlopen("/annibale/lib/libannibaleair.dylib", RTLD_NOW | RTLD_NOLOAD);
    const char *(*name)(void *) = air ? dlsym(air, "rwa_device_name") : NULL;
    if (!device || ![device respondsToSelector:getter] || !name) return nil;
    void *mesa = ((void *(*)(id, SEL))objc_msgSend)(device, getter);
    const char *backend = mesa ? name(mesa) : NULL;
    if (!backend || strstr(backend, "softpipe") || strstr(backend, "llvmpipe")) {
        printf("ERROR stage=hardware-backend detail=%s\n", backend ?: "unavailable"); return nil;
    }
    printf("rgb8-444-2p: backend=%s\n", backend);
    return device;
}

int main(int argc, char **argv) { @autoreleasepool { @try {
    setbuf(stdout, NULL); setbuf(stderr, NULL);
#if TARGET_OS_IPHONE
    int console = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (console >= 0) { dup2(console, 1); dup2(console, 2); if (console > 2) close(console); }
#endif
    signal(SIGALRM, deadline); alarm(30);
    if (argc < 2) { fprintf(stderr, "usage: %s METALLIB [--driver DYLIB] [--swap-br | --half-plane] [--output FILE]\n", argv[0]); return 2; }
    const char *driver = NULL, *output = NULL, *mode = "normal";
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--driver") && i + 1 < argc) driver = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
        else if (!strcmp(mode, "normal") && !strcmp(argv[i], "--swap-br")) mode = "swap-br";
        else if (!strcmp(mode, "normal") && !strcmp(argv[i], "--half-plane")) mode = "half-plane";
        else return 2;
    }
    plane_offset = dlsym(RTLD_DEFAULT, "IOSurfaceGetOffsetOfPlane");
    if (!plane_offset) { puts("ERROR stage=plane-offset-api"); return 2; }
    id<MTLDevice> device = select_device(driver);
    if (!device) { puts("ERROR stage=device"); return 2; }
    printf("rgb8-444-2p: device=%s class=%s mode=%s\n", device.name.UTF8String, object_getClassName(device), mode);
    IOSurfaceRef surface = NULL;
    int status = create_surface(NO, &surface);
    if (status) return status;
    id<MTLTexture> texture = import_texture(device, surface);
    if (!texture) {
        puts("UNSUPPORTED format=542 stage=normal-import"); CFRelease(surface); return 77;
    }
    if (!texture_matches(texture, surface)) {
        puts("ERROR stage=texture-identity"); [texture release]; CFRelease(surface); return 2;
    }
    puts("preflight format=542 normal-import=success");
    if (!strcmp(mode, "half-plane")) {
        [texture release]; CFRelease(surface); surface = NULL;
        status = create_surface(YES, &surface);
        if (status) return status;
        texture = import_texture(device, surface);
        if (!texture) { puts("REJECTED mode=half-plane stage=texture-import"); CFRelease(surface); return 78; }
    }
    if (!texture_matches(texture, surface)) {
        puts("ERROR stage=texture-identity"); status = 2;
    } else {
        status = fill_surface(surface, !strcmp(mode, "swap-br"));
        if (!status) status = compare_pixels(device, texture, argv[1], mode, output);
    }
    /* Both objects remain owned until compare_pixels has completed the command. */
    [texture release]; CFRelease(surface);
    return status;
} @catch (NSException *exception) {
    printf("ERROR stage=exception name=%s reason=%s\n", exception.name.UTF8String, exception.reason.UTF8String);
    return 2;
} } }
