/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 *
 * Dispatch CoreImage's original five-function ci_affine_crop graph.
 * Host: xcrun -sdk macosx clang -O1 -fno-objc-arc -framework Foundation
 *       -framework Metal ci-graph-probe.m -o ci-graph-probe
 *       ./ci-graph-probe --native /path/to/ci_stdlib_stitchable.metallib
 * Device: add -arch arm64e -miphoneos-version-min=26.0, use -sdk iphoneos,
 *         sign the executable, then run it without arguments.
 * The device path loads the installed Annibale Metal driver and rejects softpipe.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void deadline(int sig) {
    (void)sig;
    _exit(124);
}
static id object(NSString *name) { return [[[NSClassFromString(name) alloc] init] autorelease]; }
static void integer(id o, const char *name, NSUInteger value) {
    ((void (*)(id, SEL, NSUInteger))objc_msgSend)(o, sel_getUid(name), value);
}
static id input(NSString *name, NSUInteger binding, NSUInteger offset) {
    id value = object(name);
    integer(value, "setBindIndex:", binding);
    if ([name isEqualToString:@"MTLFunctionStitchingInputBufferAddress"]) {
        integer(value, "setByteOffset:", offset);
        integer(value, "setDereference:", 1);
    }
    return value;
}
static id node(NSString *name, NSArray *args, NSArray *after) {
    return [[[MTLFunctionStitchingFunctionNode alloc] initWithName:name
                                                         arguments:args
                                               controlDependencies:after] autorelease];
}
static BOOL function_label(id<MTLFunction> function) {
    if (!function || ![function respondsToSelector:@selector(label)] ||
        ![function respondsToSelector:@selector(setLabel:)]) {
        printf("ci-graph: function label selectors missing\n");
        return NO;
    }
    NSMutableString *label = [[NSMutableString alloc] initWithString:
        @"ci_affine_crop mutable diagnostic label"];
    NSString *expected = [label copy];
    function.label = label;
    [label appendString:@" changed"];
    BOOL valid = [function.label isEqualToString:expected];
    function.label = function.label;
    valid &= [function.label isEqualToString:expected];
    function.label = @"ci_affine_crop";
    function.label = @"ci_affine_crop";
    valid &= [function.label isEqualToString:@"ci_affine_crop"];
    function.label = nil;
    valid &= function.label == nil;
    function.label = @"ci_affine_crop";
    [expected release];
    [label release];
    printf("ci-graph: function label %s copy/self/repeated/nil\n", valid ? "PASS" : "FAIL");
    return valid;
}
int main(int argc, char **argv) {
    @autoreleasepool {
        @try {
            signal(SIGALRM, deadline);
            alarm(45);
            setbuf(stdout, NULL);
            setbuf(stderr, NULL);
            BOOL native = argc == 3 && !strcmp(argv[1], "--native");
            if ((!native && argc != 1))
                return 2;
            NSString *path = native ? [NSString stringWithUTF8String:argv[2]]
                                    : @"/System/Library/Frameworks/CoreImage.framework/"
                                      @"ci_stdlib_stitchable.metallib";
            id<MTLDevice> device = nil;
            if (native)
                device = MTLCreateSystemDefaultDevice();
            else {
                int console = open("/dev/console", O_WRONLY | O_NOCTTY);
                if (console >= 0) {
                    dup2(console, 1);
                    dup2(console, 2);
                    if (console > 2)
                        close(console);
                }
                unsetenv("RWA_SOFTWARE");
                if (!dlopen("/annibale/lib/libannibalemetal.dylib", RTLD_NOW | RTLD_LOCAL)) {
                    printf("ci-graph: dlopen %s\n", dlerror());
                    return 2;
                }
                Class cls = objc_getClass("AnnibaleMetalDevice");
                SEL init = sel_getUid("initWithAcceleratorPort:");
                if (!cls || ![cls instancesRespondToSelector:init])
                    return 2;
                device = ((id (*)(id, SEL, uint32_t))objc_msgSend)([cls alloc], init, 0);
                void *air = dlopen("/annibale/lib/libannibaleair.dylib", RTLD_NOW | RTLD_NOLOAD);
                const char *(*device_name)(void *) = air ? dlsym(air, "rwa_device_name") : NULL;
                SEL mesa = sel_getUid("mesaDevice");
                if (!device_name || ![device respondsToSelector:mesa])
                    return 2;
                const char *backend = device_name(((void *(*)(id, SEL))objc_msgSend)(device, mesa));
                if (!backend || strstr(backend, "softpipe"))
                    return 2;
                printf("ci-graph: backend=%s\n", backend);
            }
            if (!device)
                return 2;
            NSData *data = [NSData dataWithContentsOfFile:path];
            if (!data.length) {
                printf("ci-graph: library data missing\n");
                return 3;
            }
            dispatch_data_t wrapped = dispatch_data_create(data.bytes, data.length, NULL,
                                                           DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithData:wrapped error:&error];
            dispatch_release(wrapped);
            if (!library) {
                printf("ci-graph: library=%s\n", error.description.UTF8String);
                return 3;
            }
            NSMutableArray *functions = [NSMutableArray array];
            for (NSString *name in @[
                     @"_ci_early_out", @"_ci_init_destcoord", @"_ci_read_pixel", @"_ci_crop",
                     @"_ci_write_pixel"
                 ]) {
                id f = [library newFunctionWithName:name];
                if (!f) {
                    printf("ci-graph: missing %s\n", name.UTF8String);
                    return 3;
                }
                [functions addObject:f];
                [f release];
            }
            id position = object(@"MTLFunctionStitchingBuiltinThreadPositionInGrid");
            id bounds = input(@"MTLFunctionStitchingInputBufferAddress", 2, 16);
            id rect = input(@"MTLFunctionStitchingInputBufferAddress", 2, 80);
            id transform = input(@"MTLFunctionStitchingInputBufferAddress", 2, 96);
            id crop = input(@"MTLFunctionStitchingInputBufferAddress", 2, 144);
            id src = input(@"MTLFunctionStitchingInputTexture", 1, 0);
            id sampler = input(@"MTLFunctionStitchingInputSampler", 5, 0);
            id dst = input(@"MTLFunctionStitchingInputTexture", 3, 0);
            id n0 = node(@"_ci_early_out", @[ position, bounds ], @[]);
            id n1 = [((id (*)(id, SEL, id, id))objc_msgSend)(
                [NSClassFromString(@"MTLFunctionStitchingEarlyReturnNode") alloc],
                sel_getUid("initWithCondition:controlDependencies:"), n0, @[]) autorelease];
            if (!n1)
                return 3;
            id n2 = node(@"_ci_init_destcoord", @[ position, rect, bounds ], @[ n1 ]);
            id n3 = node(@"_ci_read_pixel", @[ src, sampler, n2, transform ], @[ n1 ]);
            id n4 = node(@"_ci_crop", @[ n3, crop, n2 ], @[ n1 ]);
            id n5 = node(@"_ci_write_pixel", @[ dst, n4, position, rect ], @[ n1 ]);
            id graph = [[[NSClassFromString(@"MTLFunctionStitchingGraphSPI") alloc]
                initWithFunctionName:@"ci_affine_crop"
                               nodes:@[ n0, n1, n2, n3, n4, n5 ]
                          outputNode:nil
                          attributes:@[]] autorelease];
            MTLStitchedLibraryDescriptor *descriptor =
                [[[MTLStitchedLibraryDescriptor alloc] init] autorelease];
            descriptor.functions = functions;
            descriptor.functionGraphs = @[ graph ];
            id<MTLLibrary> linked = [device newLibraryWithStitchedDescriptor:descriptor
                                                                       error:&error];
            if (!linked) {
                printf("ci-graph: link=%s\n", error.description.UTF8String);
                return 4;
            }
            id<MTLFunction> function = [linked newFunctionWithName:@"ci_affine_crop"];
            printf("ci-graph: function type=%lu\n", (unsigned long)function.functionType);
            if (!function_label(function)) return 4;
            MTLComputePipelineReflection *reflection = nil;
            id<MTLComputePipelineState> pipeline =
                [device newComputePipelineStateWithFunction:function
                                                    options:MTLPipelineOptionArgumentInfo |
                                                            MTLPipelineOptionBufferTypeInfo
                                                 reflection:&reflection
                                                      error:&error];
            if (!pipeline) {
                printf("ci-graph: pipeline=%s\n", error.description.UTF8String);
                return 5;
            }
            printf("ci-graph: reflection arguments=%lu\n",
                   (unsigned long)reflection.arguments.count);
            unsigned reflected = 0;
            for (MTLArgument *argument in reflection.arguments) {
                if (argument.type == MTLArgumentTypeBuffer && argument.index == 2 &&
                    argument.access == MTLArgumentAccessReadOnly &&
                    argument.bufferDataType == MTLDataTypeNone && argument.bufferDataSize == 0 &&
                    argument.bufferAlignment == 4)
                    reflected |= 1;
                if (argument.type == MTLArgumentTypeTexture && argument.index == 1 &&
                    argument.access == MTLArgumentAccessReadOnly)
                    reflected |= 2;
                if (argument.type == MTLArgumentTypeSampler && argument.index == 5 &&
                    argument.access == MTLArgumentAccessReadOnly)
                    reflected |= 4;
                if (argument.type == MTLArgumentTypeTexture && argument.index == 3 &&
                    argument.access == MTLArgumentAccessWriteOnly)
                    reflected |= 8;
            }
            if (reflection.arguments.count != 4 || reflected != 15) {
                printf("ci-graph: reflection mismatch mask=%u\n", reflected);
                return 5;
            }
            float constants[40] = {0};
            const float m4[] = {1, 0, 0, 19, 0, 1, 0, 29, 0, 0, 1, 39, 1, 1, 6, 3};
            const float r[] = {0, 0, 8, 4};
            const float m3[] = {.125f, 0, 0, 101, 0, .25f, 0, 202, 0, 0, 1, 303};
            const float c[] = {0, 6, 0, 3};
            memcpy((char *)constants + 16, m4, sizeof(m4));
            memcpy((char *)constants + 80, r, sizeof(r));
            memcpy((char *)constants + 96, m3, sizeof(m3));
            memcpy((char *)constants + 144, c, sizeof(c));
            id<MTLBuffer> buffer = [device newBufferWithBytes:constants
                                                       length:sizeof(constants)
                                                      options:MTLResourceStorageModeShared];
            MTLTextureDescriptor *td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                   width:8
                                                                  height:4
                                                               mipmapped:NO];
            td.storageMode = MTLStorageModeShared;
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            id<MTLTexture> source = [device newTextureWithDescriptor:td],
                           destination = [device newTextureWithDescriptor:td];
            MTLSamplerDescriptor *sd = [[[MTLSamplerDescriptor alloc] init] autorelease];
            sd.normalizedCoordinates = YES;
            sd.minFilter = MTLSamplerMinMagFilterNearest;
            sd.magFilter = MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
            sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
            id<MTLSamplerState> state = [device newSamplerStateWithDescriptor:sd];
            id<MTLCommandQueue> queue = [device newCommandQueue];
            if (!buffer || !source || !destination || !state || !queue)
                return 6;
            float pixels[128], expected_source[128];
            for (unsigned y = 0; y < 4; y++)
                for (unsigned x = 0; x < 8; x++) {
                    unsigned p = (y * 8 + x) * 4;
                    expected_source[p] = x + 1;
                    expected_source[p + 1] = y + 2;
                    expected_source[p + 2] = x + y + .25f;
                    expected_source[p + 3] = 1;
                }
            for (unsigned i = 0; i < 128; i++)
                pixels[i] = -20;
            MTLRegion region = MTLRegionMake2D(0, 0, 8, 4);
            [source replaceRegion:region mipmapLevel:0 withBytes:expected_source bytesPerRow:128];
            [destination replaceRegion:region mipmapLevel:0 withBytes:pixels bytesPerRow:128];
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [command computeCommandEncoder];
            [enc setComputePipelineState:pipeline];
            [enc setTexture:source atIndex:1];
            [enc setTexture:destination atIndex:3];
            [enc setSamplerState:state atIndex:5];
            [enc setBuffer:buffer offset:0 atIndex:2];
            [enc dispatchThreads:MTLSizeMake(8, 4, 1) threadsPerThreadgroup:MTLSizeMake(8, 4, 1)];
            [enc endEncoding];
            [command commit];
            [command waitUntilCompleted];
            unsigned errors = command.status != MTLCommandBufferStatusCompleted;
            [destination getBytes:pixels bytesPerRow:128 fromRegion:region mipmapLevel:0];
            for (unsigned y = 0; y < 4; y++)
                for (unsigned x = 0; x < 8; x++)
                    for (unsigned ch = 0; ch < 4; ch++) {
                        unsigned p = (y * 8 + x) * 4 + ch;
                        float factor = (x == 0 || x == 5 ? .5f : 1) * (y == 0 || y == 2 ? .5f : 1);
                        float expected = x < 6 && y < 3 ? expected_source[p] * factor : -20;
                        if (pixels[p] != expected) {
                            if (errors < 8)
                                printf("ci-graph: pixel %u,%u,%u got=%g expected=%g\n", x, y, ch,
                                       pixels[p], expected);
                            errors++;
                        }
                    }
            [source getBytes:pixels bytesPerRow:128 fromRegion:region mipmapLevel:0];
            if (memcmp(pixels, expected_source, sizeof(pixels)))
                errors++;
            if (memcmp(buffer.contents, constants, sizeof(constants)))
                errors++;
            printf("ci-graph: %s status=%lu errors=%u components=128 texture=1 sampler=5 nodes=6 "
                   "functions=5\n",
                   errors ? "FAIL" : "PASS", (unsigned long)command.status, errors);
            [queue release];
            [state release];
            [source release];
            [destination release];
            [buffer release];
            [pipeline release];
            [function release];
            [linked release];
            [library release];
            [device release];
            alarm(0);
            return errors ? 1 : 0;
        } @catch (NSException *e) {
            printf("ci-graph: exception=%s reason=%s\n", e.name.UTF8String, e.reason.UTF8String);
            return 8;
        }
    }
}
