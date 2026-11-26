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
 *   xfm_log=1  sample SBIconScrollView's horizontal scrolling state on its smooth-scroll display
 *              link and in setContentOffset:, buffered in memory and written in batches.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ptrauth.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <os/lock.h>
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
static mach_timebase_info_data_t dl_timebase;

/*
 * Microseconds since boot, on the timeline of the kernel's "tl:" lines. Without one, a display
 * link line can only be placed against the frame stream by the lines that happen to surround it,
 * and whether a rate request came before or inside a settle stall is exactly what dl_log=1 is
 * meant to answer.
 */
static unsigned long long
dl_micros(void)
{
    if (dl_timebase.denom == 0) {
        mach_timebase_info(&dl_timebase);
    }
    return (unsigned long long)((mach_absolute_time() * dl_timebase.numer / dl_timebase.denom) / 1000);
}

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
        fprintf(stderr, "sb26: display link %p made for -[%s %s] by %s at %llu\n", (void *)link,
            target ? object_getClassName(target) : "nil", action ? sel_getName(action) : "",
            caller(from, who, sizeof(who)), dl_micros());
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
        fprintf(stderr, "sb26: display link %p asked for %ld a second%s by %s at %llu\n", (void *)self, fps,
            full ? " (dropped)" : "", caller(__builtin_return_address(0), who, sizeof(who)), dl_micros());
    original_set_fps(self, selector, full ? 0 : fps);
}

static void
set_range(id self, SEL selector, frame_rate_range range)
{
    char who[256];
    int full = full_rate(self);
    if (log_slot())
        fprintf(stderr, "sb26: display link %p asked for %.0f..%.0f, %.0f a second%s by %s at %llu\n", (void *)self,
            range.minimum, range.maximum, range.preferred, full ? " (dropped)" : "",
            caller(__builtin_return_address(0), who, sizeof(who)), dl_micros());
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

/* ---- page transform samples --------------------------------------------------------------------- */

/*
 * xfm_log=1: the motion truth probe for home-screen page scrolling. SBIconScrollView inherits
 * _smoothScrollDisplayLink: from UIScrollView on the checked 23G90 cache and overrides
 * setContentOffset: itself. Both scrolling hooks therefore go onto SBIconScrollView: inherited methods are
 * added as subclass overrides, so UIScrollView and the other scroll views in the process keep their
 * original implementations. The display-link hook samples after the original callback has advanced
 * the animation. The content-offset hook covers synchronous scrolling and suppresses the nested
 * sample when the display-link implementation itself sets the offset.
 *
 * Samples are appended to a fixed ring on the calling thread. A serial dispatch queue copies and
 * writes them in batches: once when the ring is three quarters full, and once 900 ms after the last
 * finger of a touch sequence leaves the screen. The delay covers the settle segment whose motion the
 * probe is meant to resolve, and finishes before the measurement tour's 1.2 s page-turn settle
 * window ends. No sample is written to stderr on the main RunLoop.
 *
 * Line fields: off is contentOffset.x; mx/px are model/presentation layer position.x; mtx/ptx are
 * the x translations of their transforms; mbx/pbx are their bounds.origin.x. Bounds x is included
 * because UIScrollView represents scrolling through the layer bounds; position and transform alone
 * can stay constant while the content scrolls.
 */
#define XFM_RING_CAP 1024
#define XFM_RING_FLUSH_AT (XFM_RING_CAP * 3 / 4)
/* 900 ms covers the roughly 0.7 s spring settle and still leaves a quiet gap before the 1.2 s
 * page-turn settle window used by the measurement tour ends. */
#define XFM_FLUSH_AFTER_LIFT_NS (900LL * 1000LL * 1000LL)

struct xfm_point { double x, y; };
struct xfm_rect { double x, y, w, h; };
struct xfm_transform3d {
    double m11, m12, m13, m14;
    double m21, m22, m23, m24;
    double m31, m32, m33, m34;
    double m41, m42, m43, m44;
};

struct xfm_record {
    unsigned long long micros;
    const void *link;
    const void *view;
    double offset_x;
    double model_x, model_tx, model_bx;
    double presentation_x, presentation_tx, presentation_bx;
    unsigned gesture;
    unsigned char source;
};

static int xfm_log;
static os_unfair_lock xfm_ring_lock = OS_UNFAIR_LOCK_INIT;
static struct xfm_record xfm_ring[XFM_RING_CAP];
static struct xfm_record xfm_flush_copy[XFM_RING_CAP];
static unsigned xfm_write, xfm_read, xfm_dropped;
static dispatch_queue_t xfm_flush_queue;
static atomic_int xfm_flush_pending;
static atomic_uint xfm_gesture;
static unsigned xfm_active_touches;
static __thread int xfm_in_smooth;

static SEL xfm_sel_content_offset, xfm_sel_layer, xfm_sel_presentation_layer;
static SEL xfm_sel_position, xfm_sel_transform, xfm_sel_bounds;
static SEL xfm_sel_type, xfm_sel_all_touches, xfm_sel_count, xfm_sel_any_object;
static SEL xfm_sel_object_enumerator, xfm_sel_next_object, xfm_sel_phase;

static void (*xfm_original_smooth)(id, SEL, id);
static void (*xfm_original_set_offset)(id, SEL, struct xfm_point);
static void (*xfm_original_send_event)(id, SEL, id);

static id
xfm_object0(id object, SEL selector)
{
    if (!object) return nil;
    return ((id (*)(id, SEL))objc_msgSend)(object, selector);
}

static struct xfm_point
xfm_point0(id object, SEL selector)
{
    if (!object) return (struct xfm_point){ NAN, NAN };
    return ((struct xfm_point (*)(id, SEL))objc_msgSend)(object, selector);
}

static struct xfm_rect
xfm_rect0(id object, SEL selector)
{
    if (!object) return (struct xfm_rect){ NAN, NAN, NAN, NAN };
    return ((struct xfm_rect (*)(id, SEL))objc_msgSend)(object, selector);
}

static struct xfm_transform3d
xfm_transform0(id object, SEL selector)
{
    struct xfm_transform3d zero = { 0 };
    if (!object) return zero;
    return ((struct xfm_transform3d (*)(id, SEL))objc_msgSend)(object, selector);
}

static void
xfm_fill_layer(id layer, double *x, double *tx, double *bx)
{
    struct xfm_point at = xfm_point0(layer, xfm_sel_position);
    struct xfm_transform3d transform = xfm_transform0(layer, xfm_sel_transform);
    struct xfm_rect bounds = xfm_rect0(layer, xfm_sel_bounds);
    *x = at.x;
    *tx = transform.m41;
    *bx = bounds.x;
}

static void xfm_flush_now(void);
static void xfm_flush_entry(void *context);

static void
xfm_request_flush(void)
{
    if (!xfm_flush_queue) return;
    if (!atomic_exchange_explicit(&xfm_flush_pending, 1, memory_order_acq_rel))
        dispatch_async_f(xfm_flush_queue, NULL, xfm_flush_entry);
}

static void
xfm_ring_push(const struct xfm_record *record)
{
    int near_full;
    os_unfair_lock_lock(&xfm_ring_lock);
    if (xfm_write - xfm_read == XFM_RING_CAP) {
        xfm_read++;
        xfm_dropped++;
    }
    xfm_ring[xfm_write % XFM_RING_CAP] = *record;
    xfm_write++;
    near_full = xfm_write - xfm_read >= XFM_RING_FLUSH_AT;
    os_unfair_lock_unlock(&xfm_ring_lock);
    if (near_full) xfm_request_flush();
}

static void
xfm_flush_now(void)
{
    unsigned count, dropped;

    os_unfair_lock_lock(&xfm_ring_lock);
    count = xfm_write - xfm_read;
    for (unsigned i = 0; i < count; i++)
        xfm_flush_copy[i] = xfm_ring[(xfm_read + i) % XFM_RING_CAP];
    xfm_read = xfm_write;
    dropped = xfm_dropped;
    xfm_dropped = 0;
    os_unfair_lock_unlock(&xfm_ring_lock);

    if (!count && !dropped) return;
    fprintf(stderr, "sb26: xfm batch at %llu n=%u dropped=%u\n", dl_micros(), count, dropped);
    for (unsigned i = 0; i < count; i++) {
        const struct xfm_record *r = &xfm_flush_copy[i];
        fprintf(stderr,
            "sb26: xfm t=%llu g=%u src=%c dl=%p sv=%p off=%.3f mx=%.3f mtx=%.3f mbx=%.3f "
            "px=%.3f ptx=%.3f pbx=%.3f\n",
            r->micros, r->gesture, r->source, r->link, r->view, r->offset_x,
            r->model_x, r->model_tx, r->model_bx,
            r->presentation_x, r->presentation_tx, r->presentation_bx);
    }
}

static void
xfm_flush_entry(void *context)
{
    (void)context;
    for (;;) {
        xfm_flush_now();
        os_unfair_lock_lock(&xfm_ring_lock);
        int empty = xfm_write == xfm_read;
        if (empty)
            atomic_store_explicit(&xfm_flush_pending, 0, memory_order_release);
        os_unfair_lock_unlock(&xfm_ring_lock);
        if (empty) break;
    }
}

static void
xfm_delayed_flush(void *context)
{
    (void)context;
    xfm_flush_now();
}

static void
xfm_schedule_settle_flush(void)
{
    if (!xfm_flush_queue) return;
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, XFM_FLUSH_AFTER_LIFT_NS),
        xfm_flush_queue, NULL, xfm_delayed_flush);
}

/* UIEventTypeTouches and UITouchPhaseBegan/Ended/Cancelled, kept numeric because this file does not use UIKit. */
static void
xfm_note_event(id event)
{
    if (!event || ((long (*)(id, SEL))objc_msgSend)(event, xfm_sel_type) != 0) return;
    id touches = xfm_object0(event, xfm_sel_all_touches);
    long touch_count = touches ? ((long (*)(id, SEL))objc_msgSend)(touches, xfm_sel_count) : 0;
    unsigned began = 0, finished = 0;

    /* The measured scrolling path is single-touch; keep that path off the enumerator and reserve it
     * for multi-touch events. */
    if (touch_count == 1) {
        id touch = xfm_object0(touches, xfm_sel_any_object);
        long phase = touch ? ((long (*)(id, SEL))objc_msgSend)(touch, xfm_sel_phase) : -1;
        if (phase == 0) began = 1;
        else if (phase == 3 || phase == 4) finished = 1;
    } else if (touch_count > 1) {
        id enumerator = xfm_object0(touches, xfm_sel_object_enumerator);
        id touch;
        while ((touch = xfm_object0(enumerator, xfm_sel_next_object)) != nil) {
            long phase = ((long (*)(id, SEL))objc_msgSend)(touch, xfm_sel_phase);
            if (phase == 0) began++;
            else if (phase == 3 || phase == 4) finished++;
        }
    }
    if (!began && !finished) return;

    unsigned active_before = xfm_active_touches;
    if (began) {
        if (!xfm_active_touches)
            atomic_fetch_add_explicit(&xfm_gesture, 1, memory_order_relaxed);
        xfm_active_touches += began;
    }
    if (finished)
        xfm_active_touches = finished >= xfm_active_touches ? 0 : xfm_active_touches - finished;
    if (active_before && !xfm_active_touches) xfm_schedule_settle_flush();
}

static void
xfm_send_event(id self, SEL selector, id event)
{
    /* Count before the original: samples produced while the first touch is handled carry its gesture. */
    xfm_note_event(event);
    xfm_original_send_event(self, selector, event);
}

static void
xfm_sample(id view, id link, unsigned char source, double fallback_offset_x)
{
    unsigned gesture = atomic_load_explicit(&xfm_gesture, memory_order_relaxed);

    struct xfm_record record = {
        .micros = dl_micros(),
        .link = (const void *)link,
        .view = (const void *)view,
        .offset_x = fallback_offset_x,
        .model_x = NAN, .model_tx = NAN, .model_bx = NAN,
        .presentation_x = NAN, .presentation_tx = NAN, .presentation_bx = NAN,
        .gesture = gesture,
        .source = source,
    };

    /* UIKit layers belong to the main thread. The display-link callback runs there; an unusual
     * background setContentOffset: still leaves a time-stamped offset sample, without layer reads. */
    if (pthread_main_np()) {
        struct xfm_point offset = xfm_point0(view, xfm_sel_content_offset);
        if (!isnan(offset.x)) record.offset_x = offset.x;
        id layer = xfm_object0(view, xfm_sel_layer);
        if (layer) {
            xfm_fill_layer(layer, &record.model_x, &record.model_tx, &record.model_bx);
            id presentation = xfm_object0(layer, xfm_sel_presentation_layer);
            if (presentation)
                xfm_fill_layer(presentation, &record.presentation_x, &record.presentation_tx,
                    &record.presentation_bx);
        }
    }
    xfm_ring_push(&record);
}

static void
xfm_smooth_display_link(id self, SEL selector, id link)
{
    xfm_in_smooth++;
    xfm_original_smooth(self, selector, link);
    xfm_in_smooth--;
    xfm_sample(self, link, 'd', NAN);
}

static void
xfm_set_content_offset(id self, SEL selector, struct xfm_point offset)
{
    xfm_original_set_offset(self, selector, offset);
    if (!xfm_in_smooth) xfm_sample(self, nil, 's', offset.x);
}

/*
 * class_getInstanceMethod walks up the inheritance chain. When the method it finds belongs to a
 * superclass, add a method with the same selector to the subclass and keep the superclass IMP as the
 * original; when the class owns the method, replace it in place. The expected encodings are checked
 * because the replacement ABI has to match exactly.
 */
static IMP
xfm_hook_method(Class cls, const char *name, const char *encoding, IMP replacement)
{
    SEL selector = sel_registerName(name);
    Method method = class_getInstanceMethod(cls, selector);
    const char *found = method ? method_getTypeEncoding(method) : NULL;
    if (!method || !found || strcmp(found, encoding)) {
        fprintf(stderr, "sb26: xfm: -[%s %s] not hooked (encoding %s)\n",
            class_getName(cls), name, found ? found : "missing");
        return NULL;
    }
    IMP original = method_getImplementation(method);
    if (!original) return NULL;

    Class superclass = class_getSuperclass(cls);
    Method inherited = superclass ? class_getInstanceMethod(superclass, selector) : NULL;
    if (inherited == method) {
        if (!class_addMethod(cls, selector, replacement, found)) {
            fprintf(stderr, "sb26: xfm: -[%s %s] override add failed\n", class_getName(cls), name);
            return NULL;
        }
        return original;
    }
    return method_setImplementation(method, replacement);
}

static void
xfm_restore_method(Class cls, const char *name, IMP original)
{
    if (!original) return;
    Method method = class_getInstanceMethod(cls, sel_registerName(name));
    if (method) method_setImplementation(method, original);
}

static os_unfair_lock xfm_install_lock = OS_UNFAIR_LOCK_INIT;
static int xfm_probe_installed, xfm_probe_failed, xfm_image_callback_registered;
static void xfm_install_probe(void);

static void
xfm_image_added(__unused const struct mach_header *header, __unused intptr_t slide)
{
    xfm_install_probe();
}

/*
 * dyld invokes a newly registered add-image callback for the images already loaded. Registration
 * therefore stays outside xfm_install_lock; the callback re-enters xfm_install_probe for those
 * images before the explicit initial attempt below.
 */
static void
xfm_start_probe(void)
{
    xfm_flush_queue = dispatch_queue_create("annibale.sb26.xfm", DISPATCH_QUEUE_SERIAL);
    if (!xfm_flush_queue) {
        xfm_probe_failed = 1;
        fprintf(stderr, "sb26: xfm: no flush queue, probe not installed\n");
        return;
    }
    if (!xfm_image_callback_registered) {
        xfm_image_callback_registered = 1;
        _dyld_register_func_for_add_image(xfm_image_added);
    }
    xfm_install_probe();
}

static void
xfm_install_probe(void)
{
    os_unfair_lock_lock(&xfm_install_lock);
    if (xfm_probe_installed || xfm_probe_failed) {
        os_unfair_lock_unlock(&xfm_install_lock);
        return;
    }

    Class scroll = objc_getClass("SBIconScrollView");
    Class application = objc_getClass("SpringBoard");
    if (!scroll || !application) {
        os_unfair_lock_unlock(&xfm_install_lock);
        return;
    }
    (void)dl_micros();
    xfm_sel_content_offset = sel_registerName("contentOffset");
    xfm_sel_layer = sel_registerName("layer");
    xfm_sel_presentation_layer = sel_registerName("presentationLayer");
    xfm_sel_position = sel_registerName("position");
    xfm_sel_transform = sel_registerName("transform");
    xfm_sel_bounds = sel_registerName("bounds");
    xfm_sel_type = sel_registerName("type");
    xfm_sel_all_touches = sel_registerName("allTouches");
    xfm_sel_count = sel_registerName("count");
    xfm_sel_any_object = sel_registerName("anyObject");
    xfm_sel_object_enumerator = sel_registerName("objectEnumerator");
    xfm_sel_next_object = sel_registerName("nextObject");
    xfm_sel_phase = sel_registerName("phase");

    xfm_original_smooth = (void (*)(id, SEL, id))xfm_hook_method(scroll,
        "_smoothScrollDisplayLink:", "v24@0:8@16", (IMP)xfm_smooth_display_link);
    xfm_original_set_offset = (void (*)(id, SEL, struct xfm_point))xfm_hook_method(scroll,
        "setContentOffset:", "v32@0:8{CGPoint=dd}16", (IMP)xfm_set_content_offset);
    xfm_original_send_event = (void (*)(id, SEL, id))xfm_hook_method(application,
        "sendEvent:", "v24@0:8@16", (IMP)xfm_send_event);

    if (xfm_original_smooth && xfm_original_set_offset && xfm_original_send_event) {
        xfm_probe_installed = 1;
        fprintf(stderr, "sb26: xfm: scroll probe installed, ring %u, batch flush at %u or 900 ms after touch-up\n",
            XFM_RING_CAP, XFM_RING_FLUSH_AT);
    } else {
        xfm_restore_method(scroll, "_smoothScrollDisplayLink:", (IMP)xfm_original_smooth);
        xfm_restore_method(scroll, "setContentOffset:", (IMP)xfm_original_set_offset);
        xfm_restore_method(application, "sendEvent:", (IMP)xfm_original_send_event);
        xfm_original_smooth = NULL;
        xfm_original_set_offset = NULL;
        xfm_original_send_event = NULL;
        xfm_probe_failed = 1;
        fprintf(stderr, "sb26: xfm: probe not installed (smooth/offset/event hooks incomplete)\n");
    }
    os_unfair_lock_unlock(&xfm_install_lock);
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
 * resolves symbols with dladdr(), and logs via kern.annibale.say. Every frame prints
 * image+offset first so the line resolves offline against the shared cache; the dladdr
 * symbol name, when a real one is near, follows in parentheses.
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
    sysctlbyname("kern.annibale.say", NULL, NULL, line, strlen(line) + 1);
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

            /*
             * image+offset leads every frame: it is independent of the shared cache slide and
             * resolves offline against the cache files. The dladdr name is only a supplement,
             * and the cache symbol table's own "<redacted>" entries carry nothing the offset
             * does not, so they are left out. With two writes in one iteration the second has
             * to be skipped once the first filled the line, or at would index past the buffer.
             */
            at += snprintf(line + at, sizeof(line) - at, " %s+%#lx",
                image != NULL ? image + 1 : info.dli_fname, pcs[i] - (uintptr_t)info.dli_fbase);
            if (at < (int)sizeof(line) && info.dli_sname != NULL &&
                strcmp(info.dli_sname, "<redacted>") != 0 &&
                pcs[i] - (uintptr_t)info.dli_saddr < 0x4000) {
                at += snprintf(line + at, sizeof(line) - at, "(%s+%#lx)", info.dli_sname,
                    pcs[i] - (uintptr_t)info.dli_saddr);
            }
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

    /*
     * A sample at 25 ms into a turn, then one every 15 ms. The 5 ms wakeups put the real
     * spacing at 15..20 ms (a wakeup lands within 5 ms of the threshold), so a 140 ms turn
     * yields 6..8 samples where the 50 ms spacing yielded 2. Each sample holds the main
     * thread only for the state read and the frame walk; the dladdr formatting and the say
     * run after thread_resume, on this thread.
     */
    for (;;) {
        usleep(5000);
        uint64_t began = turn_began;
        if (began == 0) {
            continue;
        }
        uint64_t ms = (mach_absolute_time() - began) * timebase.numer / timebase.denom / 1000000;

        if (began != said_turn) {
            said_turn = began;
            next_ms = 25;
        }
        if (ms >= next_ms && ms < 2000) {
            stall_say(main_thread, ms);
            next_ms = ms + 15;
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
    xfm_log = sb26_boot_arg_present("xfm_log=1");
    if (dl_max || dl_log) install_display_links();
    if (xfm_log) xfm_start_probe();
    watch_main_loop();
    /* once the home screen is up, and again once the boot's work has settled */
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(),
        (void *)"30s", rates_later);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC), dispatch_get_main_queue(),
        (void *)"120s", rates_later);
}

