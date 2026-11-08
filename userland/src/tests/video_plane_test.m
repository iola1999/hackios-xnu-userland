/* Import NV12 planes as R8/RG8 textures and verify GPU blits against the input bytes.
 * --native uses the host Metal device; the default loads the supplied Annibale driver. */
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

static void watchdog(int signo) {
    (void)signo;
    static const char message[] = "video_plane: deadline exceeded\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

int main(int argc, char **argv) { @autoreleasepool {
    BOOL native = NO, software = NO;
    const char *library = "/annibale/lib/libannibalemetal.dylib";
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
            printf("video_plane: driver load error=%s\n", dlerror()); return 2;
        }
        Class cls = objc_getClass("AnnibaleMetalDevice");
        SEL init = sel_registerName("initWithAcceleratorPort:");
        if (!cls || ![cls instancesRespondToSelector:init]) return 2;
        d = ((id (*)(id, SEL, uint32_t))objc_msgSend)([cls alloc], init, 0);
        void *air = dlopen("/annibale/lib/libannibaleair.dylib", RTLD_NOW | RTLD_NOLOAD);
        const char *(*device_name)(void *) = air ? dlsym(air, "rwa_device_name") : NULL;
        SEL mesa = sel_registerName("mesaDevice");
        if (!device_name || ![d respondsToSelector:mesa]) return 2;
        const char *name = device_name(((void *(*)(id, SEL))objc_msgSend)(d, mesa));
        if (!name || (!!strstr(name, "softpipe") != !!software)) return 2;
        printf("video_plane: backend=%s\n", name);
    }
    id<MTLCommandQueue> q = [d newCommandQueue];
    if (!d || !q) return 2;
    CVPixelBufferRef pixel=NULL;
    CVReturn result=CVPixelBufferCreate(NULL,32,16,kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        (CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}},&pixel);
    if(result || !pixel) return 3;
    IOSurfaceRef surface=CVPixelBufferGetIOSurface(pixel);
    if(!surface) return 3;
    if(CVPixelBufferLockBaseAddress(pixel,0)!=kCVReturnSuccess) return 3;
    for(unsigned plane=0;plane<2;plane++) {
        size_t width=CVPixelBufferGetWidthOfPlane(pixel,plane),height=CVPixelBufferGetHeightOfPlane(pixel,plane),row=CVPixelBufferGetBytesPerRowOfPlane(pixel,plane);
        unsigned char *bytes=CVPixelBufferGetBaseAddressOfPlane(pixel,plane);
        for(size_t y=0;y<height;y++) for(size_t x=0;x<width*(plane+1);x++) bytes[y*row+x]=(unsigned char)(19+x*3+y*7+plane*31);
    }
    if(CVPixelBufferUnlockBaseAddress(pixel,0)!=kCVReturnSuccess) return 3;
    BOOL ok=YES;
    for(unsigned plane=0;plane<2;plane++) {
        size_t width=CVPixelBufferGetWidthOfPlane(pixel,plane),height=CVPixelBufferGetHeightOfPlane(pixel,plane),row=width*(plane+1);
        MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:plane?MTLPixelFormatRG8Unorm:MTLPixelFormatR8Unorm width:width height:height mipmapped:NO];
        td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> input=[d newTextureWithDescriptor:td iosurface:surface plane:plane];
        id<MTLTexture> output=[d newTextureWithDescriptor:td];
        if(!input || !output){printf("video_plane: plane=%u import=fail\n",plane);ok=NO;[input release];[output release];continue;}
        id<MTLCommandBuffer> cb=[q commandBuffer];id<MTLBlitCommandEncoder> enc=[cb blitCommandEncoder];
        [enc copyFromTexture:input sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1) toTexture:output destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        [enc endEncoding];[cb commit];[cb waitUntilCompleted];
        unsigned char *bytes=calloc(height,row);if(!bytes)return 2;[output getBytes:bytes bytesPerRow:row fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
        unsigned errors=0;for(size_t y=0;y<height;y++)for(size_t x=0;x<row;x++)errors+=bytes[y*row+x]!=(unsigned char)(19+x*3+y*7+plane*31);
        printf("video_plane: plane=%u width=%zu height=%zu bytes=%zu errors=%u status=%lu\n",plane,width,height,row*height,errors,(unsigned long)cb.status);
        ok &= errors==0 && cb.status==MTLCommandBufferStatusCompleted;free(bytes);[input release];[output release];
    }
    CFRelease(pixel);
    [q release]; [d release]; alarm(0); return ok ? 0 : 1;
} }
