/*
 * SpringBoard's frame rate on 23G90: what CoreAnimation and UIKit believe the screen's rate is, the
 * rates SpringBoard's display links ask for, and two accessibility settings for measuring what the
 * blurs and the zoom of an opening cost. Called from inject_sb26.c once the build and the shared
 * cache slide are checked.
 *
 * Boot arguments (all off by default):
 *   dl_max=1   the display links UIKit makes for its in-process animation manager (the animations done
 *              in SpringBoard's own process, a page settling among them) run at the panel's rate. On
 *              16.5 the class is UIViewInProcessAnimationManager and asks for 20..30, and a home screen
 *              page settled at 30 a second for 0.7 s (oosexgang/hakios-xnu-kernel 7caa457); on 23G90 it
 *              is the Swift class UIKit.InProcessAnimationManager, which asked for 20..120 at 30, 30..120
 *              and 48..120 (kernel-12377-rate1-rt1). The other display links keep what they ask.
 *   dl_log=1   every display link made and every rate asked of one, with the caller (first 300).
 *   ax_rt=1    Reduce Transparency, ax_rm=1 Reduce Motion, set through libAccessibility's own setters
 *              as SpringBoard starts (oosexgang/hakios-xnu-kernel a48d2b2).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ptrauth.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <unistd.h>

int sb26_boot_arg_present(const char *wanted);      /* inject_sb26.c */

/*
 * QuartzCore's high frame rate restriction, read where 23G90 keeps it. The Info.plist key
 * CADisableMinimumFrameDurationOnPhone is consulted only while the restriction is on, and the
 * restriction's default comes from whether the display is taken for a variable refresh one; both
 * are dispatch_once values. Each value is printed with whether its once has run (guard == -1).
 * The stores that write them are checked byte for byte first, so another build prints nothing.
 */
struct qc_value {
    const char *name;
    uintptr_t value, guard;
    int is_float;
};

static const struct qc_value qc_values[] = {
    { "restriction", 0x1ecd951f8UL, 0x1ecd95640UL, 0 },
    { "restriction_default", 0x1ecd95268UL, 0x1ecd95650UL, 0 },
    { "variable_refresh_seen", 0x1ecd98cccUL, 0x1ecd98d90UL, 0 },
    { "plist_key", 0x1e9cca538UL, 0x1ecd95230UL, 0 },
    { "restriction_setting", 0x1ecd993bcUL, 0x1e9ccd5c0UL, 1 },
    { "default_setting", 0x1ecd993c0UL, 0x1e9ccd5c0UL, 1 },
};

static const struct {
    uintptr_t address;
    unsigned char bytes[16];
} qc_stores[] = {
    { 0x183fcf7f0UL, { 0x1f,0x09,0x00,0x71,0xe8,0x03,0x89,0x1a,0x29,0x6e,0x34,0xd0,0x28,0xe1,0x07,0x39 } },
    { 0x183fcf870UL, { 0x7b,0xa0,0xfd,0x97,0xc8,0xe7,0x32,0xf0,0x00,0xe1,0x14,0x39,0xfd,0x7b,0xc1,0xa8 } },
    { 0x183fcdbf0UL, { 0x1f,0x09,0x00,0x71,0xe8,0x03,0x89,0x1a,0x49,0x6e,0x34,0x90,0x28,0xa1,0x09,0x39 } },
};

static uintptr_t qc_slide;
static int qc_checked;

static void
print_restriction(const char *when)
{
    char line[512];
    size_t used = 0;

    if (!qc_checked) return;
    for (size_t i = 0; i < sizeof(qc_values) / sizeof(qc_values[0]) && used < sizeof(line); i++) {
        const struct qc_value *v = &qc_values[i];
        int ran = *(const volatile intptr_t *)(v->guard + qc_slide) == -1;
        if (v->is_float)
            used += snprintf(line + used, sizeof(line) - used, " %s=%g%s", v->name,
                (double)*(const volatile float *)(v->value + qc_slide), ran ? "" : "(unset)");
        else
            used += snprintf(line + used, sizeof(line) - used, " %s=%u%s", v->name,
                *(const volatile uint8_t *)(v->value + qc_slide), ran ? "" : "(unset)");
    }
    fprintf(stderr, "sb26: rate %s: quartzcore%s\n", when, line);
}

/* What CoreAnimation and UIKit answer for the screen, each only where the selector exists. */
static void
print_screen_rates(const char *when)
{
    Class screen_class = objc_getClass("UIScreen"), display_class = objc_getClass("CADisplay");
    SEL main_screen = sel_registerName("mainScreen"), main_display = sel_registerName("mainDisplay");
    SEL maximum = sel_registerName("maximumFramesPerSecond"), current_mode = sel_registerName("currentMode");
    SEL refresh_rate = sel_registerName("refreshRate");
    long screen_max = -1, display_max = -1;
    double display_rate = -1, mode_rate = -1;

    id screen = screen_class && class_getClassMethod(screen_class, main_screen) ?
        ((id (*)(Class, SEL))objc_msgSend)(screen_class, main_screen) : nil;
    if (screen && class_getInstanceMethod(object_getClass(screen), maximum))
        screen_max = ((long (*)(id, SEL))objc_msgSend)(screen, maximum);
    id display = display_class && class_getClassMethod(display_class, main_display) ?
        ((id (*)(Class, SEL))objc_msgSend)(display_class, main_display) : nil;
    if (display && class_getInstanceMethod(object_getClass(display), maximum))
        display_max = ((long (*)(id, SEL))objc_msgSend)(display, maximum);
    if (display && class_getInstanceMethod(object_getClass(display), refresh_rate))
        display_rate = ((double (*)(id, SEL))objc_msgSend)(display, refresh_rate);
    id mode = display && class_getInstanceMethod(object_getClass(display), current_mode) ?
        ((id (*)(id, SEL))objc_msgSend)(display, current_mode) : nil;
    if (mode && class_getInstanceMethod(object_getClass(mode), refresh_rate))
        mode_rate = ((double (*)(id, SEL))objc_msgSend)(mode, refresh_rate);
    fprintf(stderr, "sb26: rate %s: UIScreen.maximumFramesPerSecond=%ld CADisplay.maximumFramesPerSecond=%ld "
        "refreshRate=%g currentMode.refreshRate=%g\n", when, screen_max, display_max, display_rate, mode_rate);
    print_restriction(when);
}

static void
rates_later(void *context)
{
    print_screen_rates((const char *)context);
}

/* ---- display links ------------------------------------------------------------------------------ */

typedef struct {
    float minimum, maximum, preferred;
} frame_rate_range;

static const char *const dl_full_for[] = { "UIViewInProcessAnimationManager", "UIKit.InProcessAnimationManager" };
#define DL_LOG_MAX 300

static id (*original_with_target)(id, SEL, id, SEL);
static id (*original_with_display)(id, SEL, id, id, SEL);
static void (*original_set_fps)(id, SEL, long);
static void (*original_set_range)(id, SEL, frame_rate_range);
static int dl_max, dl_log;
static atomic_uint dl_logged;
static char dl_full_key;                /* the associated object a display link made for DL_FULL_FOR carries */

static int
full_rate(id link)
{
    return dl_max && objc_getAssociatedObject(link, &dl_full_key) != nil;
}

static int
log_slot(void)
{
    return dl_log && atomic_fetch_add_explicit(&dl_logged, 1, memory_order_relaxed) < DL_LOG_MAX;
}

static const char *
caller(const void *address, char *buffer, size_t size)
{
    Dl_info info;
    if (!dladdr(address, &info) || !info.dli_fname) {
        snprintf(buffer, size, "%p", address);
        return buffer;
    }
    const char *image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
    snprintf(buffer, size, "%s+%#lx", image, (unsigned long)((uintptr_t)address - (uintptr_t)info.dli_fbase));
    return buffer;
}

static void
made(id link, id target, SEL action, const void *from)
{
    char who[256];
    for (size_t i = 0; link && target && i < sizeof(dl_full_for) / sizeof(dl_full_for[0]); i++)
        if (!strcmp(object_getClassName(target), dl_full_for[i]))
            objc_setAssociatedObject(link, &dl_full_key, (id)kCFBooleanTrue, OBJC_ASSOCIATION_ASSIGN);
    if (log_slot())
        fprintf(stderr, "sb26: display link %p made for -[%s %s] by %s\n", (void *)link,
            target ? object_getClassName(target) : "nil", action ? sel_getName(action) : "",
            caller(from, who, sizeof(who)));
}

static id
with_target(id self, SEL selector, id target, SEL action)
{
    id link = original_with_target(self, selector, target, action);
    made(link, target, action, __builtin_return_address(0));
    return link;
}

static id
with_display(id self, SEL selector, id display, id target, SEL action)
{
    id link = original_with_display(self, selector, display, target, action);
    made(link, target, action, __builtin_return_address(0));
    return link;
}

static void
set_fps(id self, SEL selector, long fps)
{
    char who[256];
    int full = full_rate(self);
    if (log_slot())
        fprintf(stderr, "sb26: display link %p asked for %ld a second%s by %s\n", (void *)self, fps,
            full ? " (dropped)" : "", caller(__builtin_return_address(0), who, sizeof(who)));
    original_set_fps(self, selector, full ? 0 : fps);
}

static void
set_range(id self, SEL selector, frame_rate_range range)
{
    char who[256];
    int full = full_rate(self);
    if (log_slot())
        fprintf(stderr, "sb26: display link %p asked for %.0f..%.0f, %.0f a second%s by %s\n", (void *)self,
            range.minimum, range.maximum, range.preferred, full ? " (dropped)" : "",
            caller(__builtin_return_address(0), who, sizeof(who)));
    if (full) range = (frame_rate_range){ 0, 0, 0 };
    original_set_range(self, selector, range);
}

static IMP
replace(Class cls, int class_method, const char *name, const char *encoding, IMP with)
{
    SEL selector = sel_registerName(name);
    Method method = class_method ? class_getClassMethod(cls, selector) : class_getInstanceMethod(cls, selector);
    const char *found = method ? method_getTypeEncoding(method) : NULL;
    if (!found || strcmp(found, encoding)) {
        fprintf(stderr, "sb26: display link %s not replaced (encoding %s)\n", name, found ? found : "missing");
        return NULL;
    }
    return method_setImplementation(method, with);
}

static void
install_display_links(void)
{
    Class link = objc_getClass("CADisplayLink");
    if (!link) return;
    original_with_target = (id (*)(id, SEL, id, SEL))replace(link, 1, "displayLinkWithTarget:selector:",
        "@32@0:8@16:24", (IMP)with_target);
    original_with_display = (id (*)(id, SEL, id, id, SEL))replace(link, 1, "displayLinkWithDisplay:target:selector:",
        "@40@0:8@16@24:32", (IMP)with_display);
    original_set_fps = (void (*)(id, SEL, long))replace(link, 0, "setPreferredFramesPerSecond:", "v24@0:8q16",
        (IMP)set_fps);
    original_set_range = (void (*)(id, SEL, frame_rate_range))replace(link, 0, "setPreferredFrameRateRange:",
        "v28@0:8{CAFrameRateRange=fff}16", (IMP)set_range);
    fprintf(stderr, "sb26: display links: made %d/%d, fps %d, range %d%s%s\n", original_with_target != NULL,
        original_with_display != NULL, original_set_fps != NULL, original_set_range != NULL,
        dl_max ? "; the in-process animation manager's at the panel's rate (dl_max=1)" : "",
        dl_log ? "; logged (dl_log=1)" : "");
}

/* ---- accessibility ------------------------------------------------------------------------------ */

static void
set_accessibility(const char *argument, const char *setter)
{
    if (!sb26_boot_arg_present(argument)) return;
    void (*set)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, setter);
    if (!set) {
        void *library = dlopen("/usr/lib/libAccessibility.dylib", RTLD_NOW);
        set = library ? (void (*)(int))dlsym(library, setter) : NULL;
    }
    if (set) set(1);
    fprintf(stderr, "sb26: %s: %s %s\n", argument, setter, set ? "called" : "not found");
}

/*
 * SpringBoard main thread RunLoop stall watcher (adapted for arm64e with touch_lat=1).
 * When a turn takes >25 ms, stall_watch wakes up, suspends the main thread,
 * walks up to 14 frames, strips arm64e PAC from lr using ptrauth_strip,
 * resolves symbols with dladdr(), and logs via kern.redwood.say.
 */
static volatile uint64_t turn_began;
static uintptr_t main_stack_top, main_stack_bottom;
static mach_timebase_info_data_t timebase;

static void __attribute__((format(printf, 1, 2)))
say(const char *format, ...)
{
    char line[1024];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line) + 1);
}

static void
stall_say(thread_t thread, uint64_t ms)
{
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    uintptr_t pcs[14];
    unsigned depth = 0;
    char line[1024];
    int at;

    if (thread_suspend(thread) != KERN_SUCCESS) {
        return;
    }
    if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count) == KERN_SUCCESS) {
        uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(state);
        uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(state);

        pcs[depth++] = (uintptr_t)ptrauth_strip((void *)pc, ptrauth_key_return_address);
        while (depth < sizeof(pcs) / sizeof(pcs[0]) &&
               fp >= main_stack_bottom && fp + 16 <= main_stack_top && (fp & 15) == 0) {
            uintptr_t next = ((uintptr_t *)fp)[0];
            uintptr_t lr = ((uintptr_t *)fp)[1];

            if (lr == 0) {
                break;
            }
            pcs[depth++] = (uintptr_t)ptrauth_strip((void *)lr, ptrauth_key_return_address);
            if (next <= fp) {
                break;
            }
            fp = next;
        }
    }
    thread_resume(thread);
    at = snprintf(line, sizeof(line), "tl: t %llu", (unsigned long long)ms);
    for (unsigned i = 0; i < depth && at < (int)sizeof(line) - 96; i++) {
        Dl_info info;

        if (dladdr((void *)pcs[i], &info) != 0 && info.dli_fname != NULL) {
            const char *image = strrchr(info.dli_fname, '/');

            at += info.dli_sname != NULL && pcs[i] - (uintptr_t)info.dli_saddr < 0x4000 ?
                snprintf(line + at, sizeof(line) - at, " %s", info.dli_sname) :
                snprintf(line + at, sizeof(line) - at, " %s+%#lx", image != NULL ? image + 1 : info.dli_fname, pcs[i] - (uintptr_t)info.dli_fbase);
        } else {
            at += snprintf(line + at, sizeof(line) - at, " %#lx", pcs[i]);
        }
    }
    say("%s\n", line);
}

static void *
stall_watch(void *argument)
{
    thread_t main_thread = (thread_t)(uintptr_t)argument;
    uint64_t said_turn = 0, next_ms = 25;

    for (;;) {
        uint64_t began = turn_began;

        usleep(10000);
        if (began == 0 || began != turn_began) {
            continue;
        }
        uint64_t ms = (mach_absolute_time() - began) * timebase.numer / timebase.denom / 1000000;

        if (began != said_turn) {
            said_turn = began;
            next_ms = 25;
        }
        if (ms >= next_ms && ms < 2000) {
            stall_say(main_thread, ms);
            next_ms = ms + 50;
        }
    }
    return NULL;
}

static void
main_loop_turn(__unused CFRunLoopObserverRef observer, CFRunLoopActivity activity, __unused void *info)
{
    if (activity & (kCFRunLoopBeforeSources | kCFRunLoopAfterWaiting)) {
        if (turn_began == 0) {
            turn_began = mach_absolute_time();
        }
    } else if (activity & (kCFRunLoopBeforeWaiting | kCFRunLoopExit)) {
        turn_began = 0;
    }
}

static void
watch_main_loop(void)
{
    if (!sb26_boot_arg_present("touch_lat=1")) {
        return;
    }
    mach_timebase_info(&timebase);
    main_stack_top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    main_stack_bottom = main_stack_top - pthread_get_stacksize_np(pthread_self());

    pthread_t watcher;
    if (pthread_create(&watcher, NULL, stall_watch, (void *)(uintptr_t)mach_thread_self()) == 0) {
        pthread_detach(watcher);
    }

    CFRunLoopObserverRef obs = CFRunLoopObserverCreate(NULL,
        kCFRunLoopBeforeSources | kCFRunLoopAfterWaiting | kCFRunLoopBeforeWaiting | kCFRunLoopExit,
        true, 0, main_loop_turn, NULL);
    if (obs != NULL) {
        CFRunLoopAddObserver(CFRunLoopGetMain(), obs, kCFRunLoopCommonModes);
        CFRelease(obs);
    }
    fprintf(stderr, "sb26: stall_watch: main loop observer installed for touch_lat=1\n");
}

void
sb26_install_rate(uintptr_t slide)
{
    qc_slide = slide;
    qc_checked = 1;
    for (size_t i = 0; i < sizeof(qc_stores) / sizeof(qc_stores[0]); i++)
        qc_checked = qc_checked && !memcmp((const void *)(qc_stores[i].address + slide), qc_stores[i].bytes, 16);
    fprintf(stderr, "sb26: rate: QuartzCore restriction stores %s\n", qc_checked ? "match" : "differ, not read");

    set_accessibility("ax_rt=1", "_AXSSetEnhanceBackgroundContrastEnabled");
    set_accessibility("ax_rm=1", "_AXSSetReduceMotionEnabled");
    dl_max = sb26_boot_arg_present("dl_max=1");
    dl_log = sb26_boot_arg_present("dl_log=1");
    if (dl_max || dl_log) install_display_links();
    watch_main_loop();
    /* once the home screen is up, and again once the boot's work has settled */
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(),
        (void *)"30s", rates_later);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC), dispatch_get_main_queue(),
        (void *)"120s", rates_later);
}

