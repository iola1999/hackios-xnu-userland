/* Process-local experiment. The device and libraries stay loaded until exit. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#import <dispatch/dispatch.h>
#import <IOKit/IOKitLib.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
static id<MTLDevice> held_device;
static void *baseline_handle;
static void stage(const char *s) { fprintf(stderr,"sbmetal26: stage=%s\n",s); }
static BOOL fail(const char *s) { fprintf(stderr,"sbmetal26: registration failed: %s\n",s);return NO; }
static BOOL method_matches(Class cls, BOOL class_method, const char *name, const char *encoding)
{
    SEL selector = sel_registerName(name);
    Method method = class_method ? class_getClassMethod(cls, selector) : class_getInstanceMethod(cls, selector);
    const char *actual = method ? method_getTypeEncoding(method) : NULL;
    fprintf(stderr, "sbmetal26: method=%s encoding=%s\n", name, actual ?: "missing");
    return actual && !strcmp(actual, encoding);
}

#include "sb_metal_diagnostic_device.inc"

static id<MTLDevice> register_device(const char *path, BOOL software)
{
    stage("default device before registration");
    id<MTLDevice> before = MTLCreateSystemDefaultDevice();
    fprintf(stderr, "sbmetal26: default_before=%s\n", before ? object_getClassName(before) : "nil");
    if (before) { [before release]; fail("default device already registered"); return nil; }
    if (objc_getClass("RedwoodMetalDevice")) { fail("candidate class already loaded"); return nil; }

    stage("resolve MTLAddDevice");
    void (*add_device)(id) = dlsym(RTLD_DEFAULT, "MTLAddDevice");
    Dl_info info = { 0 };
    if (!add_device || !dladdr((const void *)add_device, &info) || !info.dli_fname ||
        strcmp(info.dli_fname, "/System/Library/Frameworks/Metal.framework/Metal")) {
        fail("MTLAddDevice export unavailable or image mismatch"); return nil;
    }
    fprintf(stderr, "sbmetal26: registration_image=%s\n", info.dli_fname);

    stage("load candidate");
    if (!dlopen(path, RTLD_NOW | RTLD_LOCAL)) { fail(dlerror()); return nil; }
    Class cls = objc_getClass("RedwoodMetalDevice");
    if (!cls || !method_matches(cls, NO, "initWithAcceleratorPort:", "@20@0:8I16")) {
        fail("candidate initializer mismatch"); return nil;
    }
    cls = diagnostic_device_class(cls);
    if (!cls) return nil;
    stage("IOAcceleratorES lookup");
    io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
    if (!service) { fail("IOAcceleratorES unavailable"); return nil; }
    stage("candidate device initialization");
    id<MTLDevice> device = nil;
    @try {
        device = ((id (*)(id, SEL, io_service_t))objc_msgSend)([cls alloc],
            sel_registerName("initWithAcceleratorPort:"), service);
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
    if (!name || (software ? strcmp(name, "softpipe") : strcmp(name, "Adreno (TM) 830"))) {
        fprintf(stderr, "sbmetal26: actual_backend=%s\n", name ?: "nil");
        fail("requested backend mismatch"); return nil;
    }
    fprintf(stderr, "sbmetal26: candidate_class=%s backend=%s\n", object_getClassName(device), name);
    stage("MTLAddDevice native limits and features initialization");
    add_device(device);
    fprintf(stderr, "sbmetal26: diagnostic_families apple1=%d apple2=%d common1=%d\n",
        [device supportsFamily:MTLGPUFamilyApple1], [device supportsFamily:MTLGPUFamilyApple2],
        [device supportsFamily:MTLGPUFamilyCommon1]);
    stage("default device identity after registration");
    id<MTLDevice> after = MTLCreateSystemDefaultDevice();
    BOOL matches = after == device;
    fprintf(stderr, "sbmetal26: identity_same=%d default_after=%s\n", matches,
        after ? object_getClassName(after) : "nil");
    [after release];
    if (!matches) { fail("registered device identity mismatch"); return nil; }
    return device;
}


#include "sb_metal_compression_policy.inc"

static void attempt_registration(const char *bundle_want)
{
    if (!prepare_uncompressed_policy(bundle_want)) return;
    held_device=register_device("/redwood/lib/libredwoodmetal.dylib",NO);
    fprintf(stderr,"sbmetal26: registration completed device=%d\n",held_device!=nil);
}
static void registration_exception(NSException *e)
{
    fprintf(stderr,"sbmetal26: exception name=%s reason=%s\n",e.name.UTF8String ?: "nil",e.reason.UTF8String ?: "nil");
}
static void registration_skipped(const char *b,const char *f)
{
    fprintf(stderr,"sbmetal26: skipped build=%s switch=%s\n",b ?: "nil",f ?: "unset");
}
static void log_geometry(void)
{
    @autoreleasepool {
        /* SpringBoard also owns legacy windows; these public getters do not request layout. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        UIScreen *screen=[UIScreen mainScreen];
        NSArray<UIWindow *> *windows=[UIApplication sharedApplication].windows;
#pragma clang diagnostic pop
        CGRect b=screen.bounds, n=screen.nativeBounds;
        fprintf(stderr,"sbmetal26: geometry screen bounds=%g,%g,%g,%g native=%g,%g,%g,%g scale=%g windows=%lu\n",
            b.origin.x,b.origin.y,b.size.width,b.size.height,n.origin.x,n.origin.y,n.size.width,n.size.height,screen.scale,(unsigned long)windows.count);
        for (NSUInteger i=0;i<MIN(windows.count,10u);i++) {
            UIWindow *w=windows[i];CGRect f=w.frame;UIEdgeInsets a=w.safeAreaInsets;
            fprintf(stderr,"sbmetal26: geometry window=%lu class=%s frame=%g,%g,%g,%g safe=%g,%g,%g,%g hidden=%d alpha=%g\n",
                (unsigned long)i,object_getClassName(w),f.origin.x,f.origin.y,f.size.width,f.size.height,a.top,a.left,a.bottom,a.right,w.hidden,w.alpha);
        }
    }
}
/*
 * Boot-arg touch_lat=1: one line per turn of the main run loop, same shape and same clock as
 * inject_sb.c's, so a presented frame (tl: v N ... done) can be closed against the turn that
 * produced it -- how long the turn took to build after it woke, and how long CoreAnimation's
 * commit took, in microseconds of mach_absolute_time.
 *   tl: a WOKE COMMIT DONE
 * Every sixteenth turn only: a turn is one per frame, and one line per turn over 420 s would
 * pass the 1.3 MB the loader's ring holds. Sixteenths keep the shape and cost about 60 KB.
 */
#define CA_COMMIT_ORDER 2000000
/* Volume, measured: at one line in sixteen turns bbm29 printed 60,700 lines, about 595 a second as the
 * desktop filled in, and the compositor froze at 381 s. The loop that writes is the loop that commits, so
 * output is now one heartbeat every five seconds plus one line per turn that overran a frame, rate limited
 * to twenty a second, and never more than TL_A_LIMIT in a run. */
#define TL_A_HEARTBEAT_US 5000000
#define TL_A_LONG_US 8000
#define TL_A_SPACING_US 50000
/* A hard stop. The observer runs inside the main run loop and writes there, so a blocked write stops the
 * loop and with it CoreAnimation's commits; a run that froze at 381 s with the status bar clock stopped is
 * the shape of that. Capping the output makes the next round a test of the mechanism rather than a repeat
 * of it, and the line printed at the cap says where it happened. */
#define TL_A_LIMIT 2000
static uint64_t tl_woke, tl_commit_began;
static unsigned tl_turns, tl_written;
static mach_timebase_info_data_t tl_timebase;

static uint64_t
tl_now_us(void)
{
    return mach_absolute_time() * tl_timebase.numer / tl_timebase.denom / 1000;
}

static void
tl_turn(CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
    (void)observer;
    if (activity == kCFRunLoopAfterWaiting) {
        tl_woke = tl_now_us();
    } else if (activity == kCFRunLoopBeforeWaiting && info == NULL) {
        tl_commit_began = tl_now_us();
    } else if (activity == kCFRunLoopBeforeWaiting && tl_woke != 0) {
        uint64_t done = tl_now_us();
        /* The turn's own length goes in the line so the tail is readable without keeping every
         * turn: a frame that overran its slot is exactly the long turn. */
        static uint64_t last_beat, last_long;

        tl_turns++;
        int emit = 0;
        if (tl_written < TL_A_LIMIT) {
            if (tl_woke - last_beat >= TL_A_HEARTBEAT_US) {
                last_beat = tl_woke;
                emit = 1;
            } else if (done - tl_woke > TL_A_LONG_US && done - last_long >= TL_A_SPACING_US) {
                last_long = done;
                emit = 1;
            }
        }
        if (emit) {
            fprintf(stderr, "tl: a %llu %llu %llu %llu\n", (unsigned long long)tl_woke,
                    (unsigned long long)tl_commit_began, (unsigned long long)done,
                    (unsigned long long)(done - tl_woke));
            if (++tl_written == TL_A_LIMIT)
                fprintf(stderr, "tl: a stopped after %d lines at %llu us\n", TL_A_LIMIT,
                        (unsigned long long)done);
        }
        tl_woke = 0;
    }
}

static void
tl_watch_main_loop(void)
{
    char args[1024] = "";
    size_t size = sizeof(args) - 1;
    const char *arg;

    if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 ||
        (arg = strstr(args, "touch_lat=1")) == NULL || (arg != args && arg[-1] != ' ')) {
        return;
    }
    mach_timebase_info(&tl_timebase);
    CFRunLoopRef main_loop = CFRunLoopGetMain();
    CFRunLoopAddObserver(main_loop, CFRunLoopObserverCreate(NULL, kCFRunLoopAfterWaiting, true,
        LONG_MIN, tl_turn, NULL), kCFRunLoopCommonModes);
    CFRunLoopAddObserver(main_loop, CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true,
        CA_COMMIT_ORDER - 1, tl_turn, NULL), kCFRunLoopCommonModes);
    {
        CFRunLoopObserverContext after = { .info = (void *)1 };

        CFRunLoopAddObserver(main_loop, CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true,
            CA_COMMIT_ORDER + 1, tl_turn, &after), kCFRunLoopCommonModes);
    }
}

static void load_baseline(const char *path)
{
    fprintf(stderr,"sbmetal26: baseline load begin\n");
    baseline_handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    fprintf(stderr,"sbmetal26: baseline load %s%s\n",baseline_handle?"success":"failed: ",baseline_handle?"":dlerror());
    if (baseline_handle) dispatch_after(dispatch_time(DISPATCH_TIME_NOW,45*NSEC_PER_SEC),dispatch_get_main_queue(),^{ log_geometry(); });
}
#include "sb_metal_registration_sequence.inc"
__attribute__((constructor)) static void install(void)
{
    @autoreleasepool {
        NSDictionary *version=[NSDictionary dictionaryWithContentsOfFile:@"/System/Library/CoreServices/SystemVersion.plist"];
        id build=version[@"ProductBuildVersion"];
        run_sequence(getprogname(),[build isKindOfClass:[NSString class]]?[build UTF8String]:NULL,getenv("REDWOOD_SB_METAL"));
        /* On the main queue as inject_sb.c does it: past UIApplication startup, so the observers
         * are added to the run loop that is about to serve frames rather than to a startup one. */
        dispatch_async(dispatch_get_main_queue(), ^{ tl_watch_main_loop(); });
    }
}
