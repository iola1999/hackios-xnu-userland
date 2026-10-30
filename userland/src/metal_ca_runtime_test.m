/* 23G90 process-local registration and native CARenderer pixel check.
 * The supplied queue orders Core Animation rendering before the readback blit.
 * Device registration lasts until process exit; the candidate stays loaded. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <QuartzCore/CARenderer.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { Width = 32, Height = 32, Pitch = 256, WatchdogSeconds = 90 };
static const char *phase = "arguments";

static void stage(const char *name)
{
    phase = name;
    printf("metalca26: stage=%s\n", name);
}

static BOOL fail(const char *reason)
{
    printf("metalca26: FAIL stage=%s reason=%s\n", phase, reason);
    return NO;
}

static void watchdog(int signo)
{
    (void)signo;
    static const char message[] = "metalca26: TIMEOUT limit_seconds=90; see last stage\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static BOOL method_matches(Class cls, BOOL class_method, const char *name, const char *encoding)
{
    SEL selector = sel_registerName(name);
    Method method = class_method ? class_getClassMethod(cls, selector) : class_getInstanceMethod(cls, selector);
    const char *actual = method ? method_getTypeEncoding(method) : NULL;
    printf("metalca26: method=%s encoding=%s\n", name, actual ?: "missing");
    return actual && !strcmp(actual, encoding);
}

static BOOL check_renderer(void)
{
    Class cls = objc_getClass("CARenderer");
    if (!cls) return fail("CARenderer unavailable");
    const char *image = class_getImageName(cls);
    printf("metalca26: renderer_image=%s\n", image ?: "unknown");
    if (!image || strcmp(image, "/System/Library/Frameworks/QuartzCore.framework/QuartzCore"))
        return fail("CARenderer image mismatch");
    const char *rect = "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16";
    if (!method_matches(cls, YES, "rendererWithMTLTexture:options:", "@32@0:8@16@24") ||
        !method_matches(cls, NO, "setLayer:", "v24@0:8@16") ||
        !method_matches(cls, NO, "setBounds:", rect) ||
        !method_matches(cls, NO, "addUpdateRect:", rect) ||
        !method_matches(cls, NO, "beginFrameAtTime:timeStamp:", "v32@0:8d16^{?=IiqQdq{CVSMPTETime=ssIIIssss}QQ}24") ||
        !method_matches(cls, NO, "render", "v16@0:8") ||
        !method_matches(cls, NO, "endFrame", "v16@0:8"))
        return fail("CARenderer 23G90 method encoding mismatch");
    return YES;
}

static id<MTLDevice> register_device(const char *path, BOOL software)
{
    stage("default device before registration");
    id<MTLDevice> before = MTLCreateSystemDefaultDevice();
    printf("metalca26: default_before=%s\n", before ? object_getClassName(before) : "nil");
    if (before) { [before release]; fail("default device already registered"); return nil; }
    if (objc_getClass("RedwoodMetalDevice")) { fail("candidate class already loaded"); return nil; }

    stage("resolve MTLAddDevice");
    void (*add_device)(id) = dlsym(RTLD_DEFAULT, "MTLAddDevice");
    Dl_info info = { 0 };
    if (!add_device || !dladdr((const void *)add_device, &info) || !info.dli_fname ||
        strcmp(info.dli_fname, "/System/Library/Frameworks/Metal.framework/Metal")) {
        fail("MTLAddDevice export unavailable or image mismatch"); return nil;
    }
    printf("metalca26: registration_image=%s\n", info.dli_fname);

    stage("load candidate");
    if (!dlopen(path, RTLD_NOW | RTLD_LOCAL)) { fail(dlerror()); return nil; }
    Class cls = objc_getClass("RedwoodMetalDevice");
    if (!cls || !method_matches(cls, NO, "initWithAcceleratorPort:", "@20@0:8I16")) {
        fail("candidate initializer mismatch"); return nil;
    }
    stage("IOAcceleratorES lookup");
    io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
    if (!service) { fail("IOAcceleratorES unavailable"); return nil; }
    stage("candidate device initialization");
    id<MTLDevice> device = nil;
    @try {
        device = [((id (*)(id, SEL, io_service_t))objc_msgSend)([cls alloc],
            sel_registerName("initWithAcceleratorPort:"), service) autorelease];
    } @finally { IOObjectRelease(service); }
    Protocol *spi = objc_getProtocol("MTLDeviceSPI");
    if (!device || !spi || ![device conformsToProtocol:spi] ||
        ![device conformsToProtocol:@protocol(MTLDevice)]) {
        fail("candidate does not conform to MTLDevice and MTLDeviceSPI"); return nil;
    }
    SEL mesa_selector = sel_registerName("mesaDevice");
    void *backend = dlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW | RTLD_NOLOAD);
    const char *(*backend_name)(void *) = backend ? dlsym(backend, "rwa_device_name") : NULL;
    void *mesa = [device respondsToSelector:mesa_selector] ?
        ((void *(*)(id, SEL))objc_msgSend)(device, mesa_selector) : NULL;
    const char *name = mesa && backend_name ? backend_name(mesa) : NULL;
    if (!name || (!!strstr(name, "softpipe") != !!software)) {
        printf("metalca26: actual_backend=%s\n", name ?: "nil");
        fail("requested backend mismatch"); return nil;
    }
    printf("metalca26: candidate_class=%s backend=%s\n", object_getClassName(device), name);
    stage("MTLAddDevice native limits and features initialization");
    add_device(device);
    stage("default device identity after registration");
    id<MTLDevice> after = MTLCreateSystemDefaultDevice();
    BOOL matches = after == device;
    printf("metalca26: identity_same=%d default_after=%s\n", matches,
        after ? object_getClassName(after) : "nil");
    [after release];
    if (!matches) { fail("registered device identity mismatch"); return nil; }
    return device;
}

static void dump_pixels(const uint8_t *bytes, unsigned frame, const char *kind)
{
    unsigned changed_padding = 0;
    for (unsigned y = 0; y < Height; y++) {
        printf("metalca26: pixels frame=%u kind=%s row=%u bgra=", frame, kind, y);
        for (unsigned x = 0; x < Width * 4; x++) printf("%02x", bytes[y * Pitch + x]);
        putchar('\n');
        for (unsigned x = Width * 4; x < Pitch; x++)
            changed_padding += bytes[y * Pitch + x] != 0xcc;
    }
    printf("metalca26: pixels frame=%u kind=%s changed_padding=%u\n", frame, kind, changed_padding);
}

static BOOL read_pixels(id<MTLDevice> device, id<MTLCommandQueue> queue,
                        id<MTLTexture> target, unsigned frame)
{
    stage("same queue readback blit");
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatBGRA8Unorm width:Width height:Height mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> copied = [[device newTextureWithDescriptor:descriptor] autorelease];
    if (!copied) return fail("readback texture unavailable");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (!command) return fail("readback command unavailable");
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    if (!encoder) return fail("readback encoder unavailable");
    @try {
        [encoder copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(Width, Height, 1) toTexture:copied destinationSlice:0
            destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    } @finally { [encoder endEncoding]; }
    [command commit];
    stage("readback waitUntilCompleted");
    [command waitUntilCompleted];
    printf("metalca26: frame=%u command_status=%lu error=%s\n", frame,
        (unsigned long)command.status, command.error.description.UTF8String ?: "nil");
    if (command.status != MTLCommandBufferStatusCompleted || command.error)
        return fail("readback command failed");
    stage("pixel comparison");
    uint8_t bytes[Pitch * Height];
    memset(bytes, 0xcc, sizeof(bytes));
    [copied getBytes:bytes bytesPerRow:Pitch fromRegion:MTLRegionMake2D(0, 0, Width, Height)
        mipmapLevel:0];
    for (unsigned y = 0; y < Height; y++) {
        for (unsigned x = 0; x < Pitch; x++) {
            uint8_t expected = 0xcc;
            if (x < Width * 4) {
                BOOL center = x / 4 >= 8 && x / 4 < 24 && y >= 8 && y < 24;
                unsigned channel = center ? (frame == 0 ? 2 : 1) : 0;
                expected = x % 4 == channel || x % 4 == 3 ? 255 : 0;
            }
            if (bytes[y * Pitch + x] != expected) {
                printf("metalca26: frame=%u row=%u byte=%u actual=%02x expected=%02x\n",
                    frame, y, x, bytes[y * Pitch + x], expected);
                dump_pixels(bytes, frame, "copy");
                uint8_t direct[Pitch * Height];
                memset(direct, 0xcc, sizeof(direct));
                [target getBytes:direct bytesPerRow:Pitch
                    fromRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0];
                dump_pixels(direct, frame, "source-after-copy");
                return fail("pixel or padding mismatch");
            }
        }
    }
    printf("metalca26: FRAME_PASS frame=%u pixels=%u padding_bytes=%u\n",
        frame, Width * Height, (Pitch - Width * 4) * Height);
    return YES;
}

static BOOL render_layers(id<MTLDevice> device)
{
    stage("create queue and target texture");
    id<MTLCommandQueue> queue = [[device newCommandQueue] autorelease];
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatBGRA8Unorm width:Width height:Height mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [[device newTextureWithDescriptor:descriptor] autorelease];
    if (!queue || queue.device != device || !target || target.device != device)
        return fail("queue or texture association failed");
    uint8_t sentinel[Width * Height * 4];
    memset(sentinel, 0x55, sizeof(sentinel));
    [target replaceRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0
        withBytes:sentinel bytesPerRow:Width * 4];
    stage("native CARenderer creation");
    CARenderer *renderer = [CARenderer rendererWithMTLTexture:target
        options:@{ kCARendererMetalCommandQueue: queue }];
    if (!renderer || object_getClass(renderer) != objc_getClass("CARenderer"))
        return fail("native CARenderer creation failed");
    stage("CALayer tree setup");
    CALayer *root = [CALayer layer], *center = [CALayer layer];
    CGRect bounds = CGRectMake(0, 0, Width, Height);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    root.frame = bounds;
    root.contentsScale = 1;
    center.frame = CGRectMake(8, 8, 16, 16);
    CGColorRef blue = CGColorCreateSRGB(0, 0, 1, 1);
    root.backgroundColor = blue;
    CGColorRelease(blue);
    [root addSublayer:center];
    renderer.layer = root;
    renderer.bounds = bounds;
    [CATransaction commit];
    for (unsigned frame = 0; frame < 2; frame++) {
        printf("metalca26: frame=%u\n", frame);
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        CGColorRef color = CGColorCreateSRGB(frame == 0, frame == 1, 0, 1);
        center.backgroundColor = color;
        CGColorRelease(color);
        [CATransaction commit];
        [CATransaction flush];
        stage("native beginFrameAtTime");
        [renderer beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
        [renderer addUpdateRect:bounds];
        stage("native render");
        [renderer render];
        stage("native endFrame");
        [renderer endFrame];
        if (!read_pixels(device, queue, target, frame)) return NO;
    }
    renderer.layer = nil;
    return YES;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    BOOL software = NO;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--software")) software = YES;
        else if (!strcmp(argv[i], "--library") && i + 1 < argc) path = argv[++i];
        else { path = NULL; break; }
    }
    if (!path) {
        fprintf(stderr, "usage: %s --library DYLIB [--software]\n", argv[0]);
        return 2;
    }
    int console = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (console < 0 || dup2(console, STDOUT_FILENO) < 0 || dup2(console, STDERR_FILENO) < 0) {
        perror("metalca26: console setup failed");
        if (console >= 0) close(console);
        return 1;
    }
    if (console > STDERR_FILENO) close(console);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog);
    alarm(WatchdogSeconds);
    if (software) setenv("RWA_SOFTWARE", "1", 1);
    else unsetenv("RWA_SOFTWARE");
    int result = 1;
    @autoreleasepool {
        @try {
            stage("23G90 identity and entrypoints");
            NSDictionary *version = [NSDictionary dictionaryWithContentsOfFile:
                @"/System/Library/CoreServices/SystemVersion.plist"];
            NSString *build = version[@"ProductBuildVersion"];
            printf("metalca26: build=%s mode=%s library=%s watchdog_seconds=%u\n",
                build.UTF8String ?: "unknown", software ? "software" : "hardware", path, WatchdogSeconds);
            if (![build isEqualToString:@"23G90"]) fail("expected ProductBuildVersion 23G90");
            else if (check_renderer()) {
                id<MTLDevice> device = register_device(path, software);
                if (device && render_layers(device)) result = 0;
            }
        } @catch (NSException *exception) {
            printf("metalca26: EXCEPTION stage=%s name=%s reason=%s\n", phase,
                exception.name.UTF8String, exception.reason.UTF8String);
        }
    }
    alarm(0);
    printf("metalca26: %s test=native_carenderer mode=%s\n", result ? "FAIL" : "PASS",
        software ? "software" : "hardware");
    return result;
}
