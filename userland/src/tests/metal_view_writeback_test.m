/* Check writes through nested texture views against their original shared storage.
 * --native uses the host Metal device; the default loads the supplied Redwood driver. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <stdio.h>
#include <string.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

static BOOL verify(void *base, size_t pitch, unsigned width, unsigned height) {
    unsigned errors = 0;
    for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < width; x++) {
        const unsigned char *p = (const unsigned char *)base + y * pitch + x * 4;
        errors += p[0] != 64 || p[1] != 128 || p[2] != 191 || p[3] != 255;
    }
    printf("view_writeback: pixels=%u errors=%u pitch=%zu\n", width * height, errors, pitch);
    return errors == 0;
}

static void watchdog(int signo) {
    (void)signo;
    static const char message[] = "view_writeback: deadline exceeded\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

int main(int argc, char **argv) { @autoreleasepool {
    BOOL native = NO, software = NO;
    const char *library = "/redwood/lib/libredwoodmetal.dylib";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--native")) native = YES;
        else if (!strcmp(argv[i], "--software")) software = YES;
        else if (!strcmp(argv[i], "--library") && i + 1 < argc) library = argv[++i];
        else return 2;
    }
    if (native && software) return 2;
    if (!native) {
        int console = open("/dev/console", O_WRONLY | O_NOCTTY);
        if (console >= 0) {
            dup2(console, STDOUT_FILENO); dup2(console, STDERR_FILENO);
            if (console > STDERR_FILENO) close(console);
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog); alarm(15);
    id<MTLDevice> d = nil;
    if (native) d = MTLCreateSystemDefaultDevice();
    else {
        if (software) setenv("RWA_SOFTWARE", "1", 1); else unsetenv("RWA_SOFTWARE");
        if (!dlopen(library, RTLD_NOW | RTLD_LOCAL)) {
            printf("view_writeback: driver load error=%s\n", dlerror()); return 2;
        }
        Class cls = objc_getClass("RedwoodMetalDevice");
        SEL init = sel_registerName("initWithAcceleratorPort:");
        if (!cls || ![cls instancesRespondToSelector:init]) return 2;
        d = ((id (*)(id, SEL, uint32_t))objc_msgSend)([cls alloc], init, 0);
        void *air = dlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW | RTLD_NOLOAD);
        const char *(*device_name)(void *) = air ? dlsym(air, "rwa_device_name") : NULL;
        SEL mesa = sel_registerName("mesaDevice");
        if (!device_name || ![d respondsToSelector:mesa]) return 2;
        const char *name = device_name(((void *(*)(id, SEL))objc_msgSend)(d, mesa));
        if (!name || (!!strstr(name, "softpipe") != !!software)) return 2;
        printf("view_writeback: backend=%s\n", name);
    }
    id<MTLCommandQueue> q = [d newCommandQueue];
    if (!d || !q) return 2;
    BOOL ok = YES;
    for (unsigned kind = 0; kind < (software ? 1u : 2u); kind++) {
        const unsigned width = 17, height = 8;
        size_t pitch = kind ? 96 : [d minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm];
        while (pitch < width * 4) pitch *= 2;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
        td.storageMode = MTLStorageModeShared;
        IOSurfaceRef surface = NULL;
        id<MTLBuffer> buffer = nil;
        id<MTLTexture> texture = nil;
        void *bytes;
        if (kind) {
            surface = IOSurfaceCreate((CFDictionaryRef)@{
                (id)kIOSurfaceWidth:@(width), (id)kIOSurfaceHeight:@(height),
                (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfaceBytesPerRow:@(pitch),
                (id)kIOSurfaceAllocSize:@(4096), (id)kIOSurfacePixelFormat:@(0x42475241)});
            if (!surface || IOSurfaceGetBytesPerRow(surface) != pitch) return 3;
            bytes = IOSurfaceGetBaseAddress(surface);
            texture = [d newTextureWithDescriptor:td iosurface:surface plane:0];
        } else {
            buffer = [d newBufferWithLength:pitch * height options:MTLResourceStorageModeShared];
            bytes = buffer.contents;
            texture = [buffer newTextureWithDescriptor:td offset:0 bytesPerRow:pitch];
        }
        if (!texture || !bytes) return 4;
        memset(bytes, 0x33, pitch * height);
        id<MTLTexture> view = [texture newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
        id<MTLTexture> nested = [view newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
        [texture release]; [view release];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = nested;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.75, 0.5, 0.25, 1);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
        [e endEncoding];
        pass.colorAttachments[0].texture = nil;
        [nested release];
        [cb commit]; [cb waitUntilCompleted];
        printf("view_writeback: target=%s status=%lu error=%s\n", kind ? "IOSurface" : "MTLBuffer", (unsigned long)cb.status, cb.error.description.UTF8String ?: "none");
        ok &= cb.status == MTLCommandBufferStatusCompleted && verify(bytes, pitch, width, height);
        [buffer release]; if (surface) CFRelease(surface);
    }
    [q release]; [d release]; alarm(0); return ok ? 0 : 1;
} }
