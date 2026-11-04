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
    /* once the home screen is up, and again once the boot's work has settled */
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(),
        (void *)"30s", rates_later);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC), dispatch_get_main_queue(),
        (void *)"120s", rates_later);
}
