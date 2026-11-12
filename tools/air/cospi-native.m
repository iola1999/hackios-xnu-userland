/* Compare production NIR results with Apple's Metal cospi on the host GPU.
 * Input is the (float input, float result) stream from cospi-nir.c. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>

static void deadline(int signal_number)
{
    (void)signal_number;
    static const char message[] = "cospi: 30-second deadline exceeded\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

int main(int argc, char **argv)
{
    @autoreleasepool { @try {
        setbuf(stdout, NULL);
#if TARGET_OS_IPHONE
        int console = open("/dev/console", O_WRONLY | O_NOCTTY);
        if (console >= 0) {
            dup2(console, STDOUT_FILENO);
            dup2(console, STDERR_FILENO);
            if (console > STDERR_FILENO) close(console);
        }
#endif
        if (argc < 2 || argc > 3) {
            fprintf(stderr, "cospi: expected values.bin and optional offline metallib\n"); return 2;
        }
        signal(SIGALRM, deadline);
        alarm(30);
        puts("cospi: starting 81920 value checks");
        NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]];
        if (data.length != 81920 * 2 * sizeof(float)) {
            fprintf(stderr, "cospi: input size=%lu, expected 655360\n", (unsigned long)data.length); return 2;
        }
#if TARGET_OS_IPHONE
        if (argc != 3) { fprintf(stderr, "cospi: offline metallib required on iOS\n"); return 2; }
        unsetenv("RWA_SOFTWARE");
        if (!dlopen("/annibale/lib/libannibalemetal.dylib", RTLD_NOW | RTLD_LOCAL)) {
            fprintf(stderr, "cospi: driver load failed: %s\n", dlerror()); return 2;
        }
        Class cls = objc_getClass("AnnibaleMetalDevice");
        SEL initialize = sel_registerName("initWithAcceleratorPort:");
        if (!cls || ![cls instancesRespondToSelector:initialize]) {
            fprintf(stderr, "cospi: Annibale device initializer unavailable\n"); return 2;
        }
        id<MTLDevice> device = ((id (*)(id, SEL, uint32_t))objc_msgSend)([cls alloc], initialize, 0);
        void *air = dlopen("/annibale/lib/libannibaleair.dylib", RTLD_NOW | RTLD_NOLOAD);
        const char *(*device_name)(void *) = air ? dlsym(air, "rwa_device_name") : NULL;
        SEL mesa = sel_registerName("mesaDevice");
        if (!device_name || ![device respondsToSelector:mesa]) {
            fprintf(stderr, "cospi: Mesa backend query unavailable\n"); return 2;
        }
        void *mesa_device = ((void *(*)(id, SEL))objc_msgSend)(device, mesa);
        const char *backend = mesa_device ? device_name(mesa_device) : NULL;
        if (!backend || strstr(backend, "softpipe")) {
            fprintf(stderr, "cospi: hardware backend required, got %s\n", backend ?: "none"); return 2;
        }
        printf("cospi: backend=%s\n", backend);
#else
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "cospi: native Metal device unavailable\n"); return 2; }
#endif
        printf("cospi: device=%s\n", device.name.UTF8String);
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void check(device const float2 *in [[buffer(0)]], "
            "device float *out [[buffer(1)]], uint i [[thread_position_in_grid]]) { "
            "out[i] = i < 65536 ? float(cospi(half(in[i].x))) : cospi(in[i].x); }";
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.fastMathEnabled = NO;
        NSError *error = nil;
        id<MTLLibrary> library;
        if (argc == 3) {
            NSData *shader = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]];
            if (!shader.length) { fprintf(stderr, "cospi: offline metallib unreadable\n"); return 2; }
            dispatch_data_t wrapped = dispatch_data_create(shader.bytes, shader.length, NULL,
                                                           DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            library = [device newLibraryWithData:wrapped error:&error];
        } else library = [device newLibraryWithSource:source options:options error:&error];
        if (!library) {
            fprintf(stderr, "cospi: library creation failed: %s\n", error.description.UTF8String ?: "no error");
            return 2;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"check"];
        if (!function) {
            fprintf(stderr, "cospi: function check unavailable; exports=%s\n",
                    [library.functionNames componentsJoinedByString:@","].UTF8String);
            return 2;
        }
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            fprintf(stderr, "cospi: pipeline creation failed: %s\n", error.description.UTF8String ?: "no error");
            return 2;
        }
        id<MTLBuffer> input = [device newBufferWithBytes:data.bytes length:data.length options:MTLResourceStorageModeShared];
        id<MTLBuffer> output = [device newBufferWithLength:data.length / 2 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        if (!input || !output || !encoder) {
            fprintf(stderr, "cospi: buffer, queue or encoder creation failed\n"); return 2;
        }
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(81920, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted || commands.error) {
            fprintf(stderr, "cospi: command status=%lu error=%s\n", (unsigned long)commands.status,
                    commands.error.description.UTF8String ?: "none"); return 2;
        }
        const float *pairs = input.contents, *native = output.contents;
        double maximum[2] = {0};
        double reference_error[2] = {0};
        unsigned failures[2] = {0};
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
                if (failures[0] + failures[1] < 8)
                    fprintf(stderr, "native cospi index=%u input=%a NIR=%a native=%a\n", i, x, nir, got);
                failures[half ? 0 : 1]++;
            }
            double error = isfinite(reference) ? fabs(got - reference) / ulp : 0;
            if (isfinite(reference) && !isfinite(got)) error = INFINITY;
            reference_error[half ? 0 : 1] = MAX(reference_error[half ? 0 : 1], error);
            if (isfinite(difference)) maximum[half ? 0 : 1] = MAX(maximum[half ? 0 : 1], difference);
        }
        printf("cospi: failures half=%u/65536 float=%u/16384; maximum reference error half=%g ULP float=%g ULP\n",
               failures[0], failures[1], reference_error[0], reference_error[1]);
        if (!failures[0] && !failures[1])
            printf("PASS 81920 native Metal comparisons; maximum difference half=%g ULP float=%g ULP\n", maximum[0], maximum[1]);
#if TARGET_OS_IPHONE
        NSData *portrait = [NSData dataWithContentsOfFile:
            @"/System/Library/VideoProcessors/CCPortrait.bundle/default.metallib"];
        if (!portrait.length) { fprintf(stderr, "cospi: original CCPortrait library unreadable\n"); return 3; }
        dispatch_data_t original = dispatch_data_create(portrait.bytes, portrait.length, NULL,
                                                        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        id<MTLLibrary> portraitLibrary = [device newLibraryWithData:original error:&error];
        if (!portraitLibrary) { fprintf(stderr, "cospi: CCPortrait library error=%s\n", error.description.UTF8String); return 3; }
        for (NSString *name in @[@"sparserendering_antialias_y", @"sparserendering_antialias_y_4chan",
                                 @"sparserendering_add_noise_only"]) {
            id<MTLFunction> function = [portraitLibrary newFunctionWithName:name];
            id<MTLComputePipelineState> originalPipeline = function ?
                [device newComputePipelineStateWithFunction:function error:&error] : nil;
            if (!originalPipeline) {
                fprintf(stderr, "cospi: original=%s pipeline=FAIL error=%s\n", name.UTF8String,
                        error.description.UTF8String ?: "function unavailable"); return 3;
            }
            printf("cospi: original=%s pipeline=PASS\n", name.UTF8String);
        }
        puts("cospi: PASS 3 original CCPortrait pipelines; resources were not dispatched");
#endif
        if (failures[0] || failures[1]) return 1;
    } @catch (NSException *error) {
        fprintf(stderr, "cospi: exception=%s reason=%s\n", error.name.UTF8String, error.reason.UTF8String);
        return 2;
    } }
    return 0;
}
