/* Two-attachment framebuffer fetch through public Metal render commands.
 * Use Apple's device by default, or select the port's driver with --driver. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { Width = 16, Height = 12, Pitch = Width * 4, Bytes = Pitch * Height };
static const MTLScissorRect Scissor = {3, 2, 9, 7};

static void deadline(int signal_number)
{
    (void)signal_number;
    static const char message[] = "fbfetch-mrt: 30-second deadline exceeded\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void initial_pixel(unsigned texture, unsigned x, unsigned y, uint8_t pixel[4])
{
    pixel[0] = texture ? 255 : 0;
    pixel[1] = (x & 1) ? 255 : 0;
    pixel[2] = (y & 1) ? 255 : 0;
    pixel[3] = texture ? 0 : 255;
}

static void seed_pixel(unsigned target, unsigned x, unsigned y, uint8_t pixel[4])
{
    if (target == 0) {
        pixel[0] = 0;
        pixel[1] = ((y >> 1) & 1) ? 255 : 0;
        pixel[2] = (x & 1) ? 255 : 0;
        pixel[3] = 255;
    } else {
        pixel[0] = 255;
        pixel[1] = (y & 1) ? 255 : 0;
        pixel[2] = ((x >> 1) & 1) ? 0 : 255;
        pixel[3] = 0;
    }
}

static id<MTLRenderPipelineState> pipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                          NSString *fragment)
{
    id<MTLFunction> vertex = [[library newFunctionWithName:@"fbf_vertex"] autorelease];
    id<MTLFunction> pixel = [[library newFunctionWithName:fragment] autorelease];
    if (!vertex || !pixel) {
        fprintf(stderr, "fbfetch-mrt: missing function %s\n", fragment.UTF8String);
        return nil;
    }
    MTLRenderPipelineDescriptor *desc = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
    desc.vertexFunction = vertex;
    desc.fragmentFunction = pixel;
    desc.rasterSampleCount = 1;
    for (unsigned i = 0; i < 2; i++) {
        desc.colorAttachments[i].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.colorAttachments[i].blendingEnabled = NO;
        desc.colorAttachments[i].writeMask = MTLColorWriteMaskAll;
    }
    NSError *error = nil;
    id<MTLRenderPipelineState> result = [[device newRenderPipelineStateWithDescriptor:desc
        error:&error] autorelease];
    if (!result || error) {
        fprintf(stderr, "fbfetch-mrt: pipeline %s: %s\n", fragment.UTF8String,
                error.description.UTF8String ?: "nil pipeline");
        return nil;
    }
    return result;
}

static int run_case(id<MTLDevice> device, id<MTLCommandQueue> queue,
                     id<MTLRenderPipelineState> seed_pipeline,
                     id<MTLRenderPipelineState> fetch_pipeline,
                     BOOL seeded, BOOL swapped, BOOL reversed, NSString *output)
{
    NSString *name = [NSString stringWithFormat:@"%s-%s-%s", seeded ? "written" : "loaded",
        swapped ? "swap" : "pass", reversed ? "reversed" : "normal"];
    id<MTLTexture> textures[2];
    uint8_t bytes[Bytes];
    for (unsigned i = 0; i < 2; i++) {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatBGRA8Unorm width:Width height:Height mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        textures[i] = [[device newTextureWithDescriptor:desc] autorelease];
        if (!textures[i]) {
            fprintf(stderr, "fbfetch-mrt: texture %u unavailable\n", i);
            return 2;
        }
        for (unsigned y = 0; y < Height; y++)
            for (unsigned x = 0; x < Width; x++)
                initial_pixel(i, x, y, bytes + y * Pitch + x * 4);
        [textures[i] replaceRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0
            withBytes:bytes bytesPerRow:Pitch];
    }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    for (unsigned logical = 0; logical < 2; logical++) {
        pass.colorAttachments[logical].texture = textures[reversed ? 1 - logical : logical];
        pass.colorAttachments[logical].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[logical].storeAction = MTLStoreActionStore;
    }
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!command || !encoder) {
        fprintf(stderr, "fbfetch-mrt: command/encoder unavailable\n");
        return 2;
    }
    [encoder setViewport:(MTLViewport){0, 0, Width, Height, 0, 1}];
    [encoder setScissorRect:Scissor];
    [encoder setCullMode:MTLCullModeNone];
    if (seeded) {
        [encoder setRenderPipelineState:seed_pipeline];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder setRenderPipelineState:fetch_pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted || command.error) {
        fprintf(stderr, "fbfetch-mrt: %s command status=%lu error=%s\n", name.UTF8String,
            (unsigned long)command.status, command.error.description.UTF8String ?: "none");
        return 2;
    }
    unsigned mismatches = 0;
    for (unsigned physical = 0; physical < 2; physical++) {
        memset(bytes, 0xcc, sizeof(bytes));
        [textures[physical] getBytes:bytes bytesPerRow:Pitch
            fromRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0];
        if (output) {
            NSString *path = [output stringByAppendingPathComponent:
                [NSString stringWithFormat:@"%@-%u.bgra", name, physical]];
            if (![[NSData dataWithBytes:bytes length:sizeof(bytes)] writeToFile:path atomically:YES]) {
                fprintf(stderr, "fbfetch-mrt: cannot write readback %s\n", path.UTF8String);
                return 2;
            }
        }
        unsigned logical = reversed ? 1 - physical : physical;
        unsigned source = swapped ? 1 - logical : logical;
        for (unsigned y = 0; y < Height; y++) {
            for (unsigned x = 0; x < Width; x++) {
                uint8_t expected[4];
                BOOL inside = x >= Scissor.x && x < Scissor.x + Scissor.width &&
                              y >= Scissor.y && y < Scissor.y + Scissor.height;
                if (!inside) initial_pixel(physical, x, y, expected);
                else if (seeded) seed_pixel(source, x, y, expected);
                else initial_pixel(reversed ? 1 - source : source, x, y, expected);
                const uint8_t *got = bytes + y * Pitch + x * 4;
                if (memcmp(got, expected, 4)) {
                    if (!mismatches)
                        fprintf(stderr, "fbfetch-mrt: %s texture=%u x=%u y=%u BGRA=%u,%u,%u,%u "
                            "expected=%u,%u,%u,%u\n", name.UTF8String, physical, x, y,
                            got[0], got[1], got[2], got[3], expected[0], expected[1], expected[2], expected[3]);
                    mismatches++;
                }
            }
        }
    }
    printf("%s %s pixels=%u mismatches=%u\n", mismatches ? "FAIL" : "PASS", name.UTF8String,
        Width * Height * 2, mismatches);
    return mismatches ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *metallib = NULL, *driver = NULL, *output_path = NULL;
    BOOL wrong_data1 = NO;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--metallib") && i + 1 < argc) metallib = argv[++i];
        else if (!strcmp(argv[i], "--driver") && i + 1 < argc) driver = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) output_path = argv[++i];
        else if (!strcmp(argv[i], "--wrong-data1")) wrong_data1 = YES;
        else {
            fprintf(stderr, "usage: %s --metallib FILE [--driver DYLIB] [--output DIR] [--wrong-data1]\n", argv[0]);
            return 2;
        }
    }
    if (!metallib) return 2;
    signal(SIGALRM, deadline);
    alarm(30);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    @autoreleasepool {
        @try {
            id<MTLDevice> device;
            if (driver) {
                unsetenv("RWA_SOFTWARE");
                if (!dlopen(driver, RTLD_NOW | RTLD_LOCAL)) {
                    fprintf(stderr, "fbfetch-mrt: dlopen: %s\n", dlerror());
                    return 2;
                }
                Class cls = objc_getClass("AnnibaleMetalDevice");
                SEL init = sel_registerName("initWithAcceleratorPort:");
                if (!cls || ![cls instancesRespondToSelector:init]) return 2;
                device = [((id (*)(id, SEL, uint32_t))objc_msgSend)([cls alloc], init, 0) autorelease];
                SEL getter = sel_registerName("mesaDevice");
                if (!device || ![device respondsToSelector:getter]) return 2;
                void *mesa_device = ((void *(*)(id, SEL))objc_msgSend)(device, getter);
                void *backend = dlopen("/annibale/lib/libannibaleair.dylib", RTLD_NOW | RTLD_NOLOAD);
                const char *(*backend_name)(void *) = backend ? dlsym(backend, "rwa_device_name") : NULL;
                const char *name = mesa_device && backend_name ? backend_name(mesa_device) : NULL;
                if (!name || strstr(name, "softpipe")) {
                    fprintf(stderr, "fbfetch-mrt: hardware backend required, got %s\n", name ?: "none");
                    return 2;
                }
                printf("fbfetch-mrt: backend=%s\n", name);
            } else device = [MTLCreateSystemDefaultDevice() autorelease];
            if (!device) return 2;
            printf("fbfetch-mrt: device=%s class=%s wrong_data1=%d\n", device.name.UTF8String,
                object_getClassName(device), wrong_data1);
            NSString *output = output_path ? [NSString stringWithUTF8String:output_path] : nil;
            if (output && ![[NSFileManager defaultManager] createDirectoryAtPath:output
                withIntermediateDirectories:YES attributes:nil error:NULL]) return 2;
            NSError *error = nil;
            NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];
            if (!data.length) return 2;
            dispatch_data_t wrapped = dispatch_data_create(data.bytes, data.length, NULL,
                DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            id<MTLLibrary> library = [[device newLibraryWithData:wrapped error:&error] autorelease];
            dispatch_release(wrapped);
            if (!library || error) {
                fprintf(stderr, "fbfetch-mrt: metallib: %s\n", error.description.UTF8String ?: "nil");
                return 2;
            }
            id<MTLCommandQueue> queue = [[device newCommandQueue] autorelease];
            id<MTLRenderPipelineState> seed = pipeline(device, library, @"fbf_seed");
            id<MTLRenderPipelineState> pass = pipeline(device, library,
                wrong_data1 ? @"fbf_wrong_data1" : @"fbf_pass");
            id<MTLRenderPipelineState> swap = pipeline(device, library,
                wrong_data1 ? @"fbf_wrong_data1" : @"fbf_swap");
            if (!queue || !seed || !pass || !swap) return 2;
            unsigned failed = 0;
            for (unsigned seeded = 0; seeded < 2; seeded++)
                for (unsigned swapped = 0; swapped < 2; swapped++)
                    for (unsigned reversed = 0; reversed < 2; reversed++) {
                        int status = run_case(device, queue, seed, swapped ? swap : pass,
                            seeded, swapped, reversed, output);
                        if (status == 2) return 2;
                        failed += status;
                    }
            printf("fbfetch-mrt: cases=8 failed=%u pixels=%u\n", failed, 8 * Width * Height * 2);
            return failed ? 1 : 0;
        } @catch (NSException *exception) {
            fprintf(stderr, "fbfetch-mrt: exception %s: %s\n", exception.name.UTF8String,
                exception.reason.UTF8String);
            return 2;
        }
    }
}
