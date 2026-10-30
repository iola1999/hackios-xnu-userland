/*
 * SpringBoard's layer tree on request, for telling an empty layer from one the compositor did not
 * draw: a missing icon is either a layer with no contents (nothing was rendered into it) or one
 * whose contents are there and did not reach the screen.
 *
 * `notify com.redwood.layers.<filter>` prints, to stderr and so to the console, every layer whose
 * own class or whose view's class contains <filter> (case sensitive), at any depth, with the layers
 * below it up to LAYER_BELOW levels. `com.redwood.layers.all` prints the whole tree. Each line is the depth,
 * the layer class, the view class, the bounds size and position in points (in the superlayer),
 * hidden/opacity, the contents (CGImage and IOSurface with their size, and the surface's ID and
 * FourCC; any other object by its CF type or class) and the effects (describe_effects). The total is capped at LAYER_LINES, because the console ring is 1.3 MB.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>

void sb26_install_layer_dump(void);

enum { LAYER_LINES = 1500, LAYER_BELOW = 3, LAYER_DEPTH = 64 };

struct rect { double x, y, w, h; };
struct point { double x, y; };

static size_t (*image_width)(const void *), (*image_height)(const void *);
static CFTypeID (*image_type)(void), (*surface_type)(void);
static size_t (*surface_width)(const void *), (*surface_height)(const void *);
static uint32_t (*surface_id)(const void *), (*surface_format)(const void *);
static unsigned lines;

static id
send(id object, const char *selector)
{
    SEL sel = sel_registerName(selector);
    if (!object || !class_respondsToSelector(object_getClass(object), sel)) return nil;
    return ((id (*)(id, SEL))objc_msgSend)(object, sel);
}

static const char *
name_of(id object)
{
    return object ? class_getName(object_getClass(object)) : "-";
}

static void
describe_contents(id contents, char *out, size_t size)
{
    if (!contents) {
        snprintf(out, size, "none");
        return;
    }
    CFTypeID type = CFGetTypeID((CFTypeRef)contents);
    if (image_type && type == image_type()) {
        snprintf(out, size, "CGImage %zux%zu", image_width(contents), image_height(contents));
    } else if (surface_type && type == surface_type()) {
        uint32_t f = surface_format(contents);
        snprintf(out, size, "IOSurface %u %zux%zu '%c%c%c%c'", surface_id(contents),
            surface_width(contents), surface_height(contents),
            (char)(f >> 24), (char)(f >> 16), (char)(f >> 8), (char)f);
    } else {
        CFStringRef described = CFCopyTypeIDDescription(type);
        char text[64] = "";
        if (described) {
            CFStringGetCString(described, text, sizeof(text), kCFStringEncodingUTF8);
            CFRelease(described);
        }
        snprintf(out, size, "%s", text[0] ? text : name_of(contents));
    }
}

/* What changes how a layer is drawn beyond its contents, as "fx=COMPOSITING/FILTERS/MASK": the
 * compositing filter's name, how many filters and background filters, and the mask's class. */
static void
describe_effects(id layer, char *out, size_t size)
{
    id compositing = send(layer, "compositingFilter"), name = send(compositing, "name");
    id filters = send(layer, "filters"), backdrop = send(layer, "backgroundFilters");
    SEL count = sel_registerName("count");
    const char *text = name ? ((const char *(*)(id, SEL))objc_msgSend)(name, sel_registerName("UTF8String"))
        : compositing ? name_of(compositing) : "-";
    snprintf(out, size, "%s/%lu+%lu/%s", text,
        filters ? ((unsigned long (*)(id, SEL))objc_msgSend)(filters, count) : 0,
        backdrop ? ((unsigned long (*)(id, SEL))objc_msgSend)(backdrop, count) : 0,
        name_of(send(layer, "mask")));
}

static int
matches(id layer, const char *filter)
{
    if (!filter) return 1;
    return strstr(name_of(layer), filter) || strstr(name_of(send(layer, "delegate")), filter);
}

static void
walk(id layer, unsigned depth, const char *filter, int below)
{
    if (!layer || lines >= LAYER_LINES || depth > LAYER_DEPTH) return;
    if (below < 0 && matches(layer, filter)) below = LAYER_BELOW;
    if (below >= 0) {
        struct rect bounds = ((struct rect (*)(id, SEL))objc_msgSend)(layer, sel_registerName("bounds"));
        BOOL hidden = ((BOOL (*)(id, SEL))objc_msgSend)(layer, sel_registerName("isHidden"));
        float opacity = ((float (*)(id, SEL))objc_msgSend)(layer, sel_registerName("opacity"));
        struct point at = ((struct point (*)(id, SEL))objc_msgSend)(layer, sel_registerName("position"));
        char contents[96], effects[96];
        describe_contents(send(layer, "contents"), contents, sizeof(contents));
        describe_effects(layer, effects, sizeof(effects));
        fprintf(stderr, "sb26 layer %2u %s view=%s %.0fx%.0f at %.0f,%.0f%s a=%.2f contents=%s fx=%s\n",
            depth, name_of(layer), name_of(send(layer, "delegate")), bounds.w, bounds.h, at.x, at.y,
            hidden ? " hidden" : "", opacity, contents, effects);
        lines++;
    }
    id sublayers = send(layer, "sublayers");
    unsigned long count = sublayers ? ((unsigned long (*)(id, SEL))objc_msgSend)(sublayers,
        sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < count && lines < LAYER_LINES; i++) {
        id child = ((id (*)(id, SEL, unsigned long))objc_msgSend)(sublayers,
            sel_registerName("objectAtIndex:"), i);
        /* past the levels shown under a match, the search goes on for further matches */
        walk(child, depth + 1, filter, below > 0 ? below - 1 : -1);
    }
}

static void
dump(const char *filter)
{
    Class window_class = objc_getClass("UIWindow");
    SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    id windows = window_class && class_respondsToSelector(object_getClass((id)window_class), all)
        ? ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)window_class, all, YES, NO) : nil;
    unsigned long count = windows ? ((unsigned long (*)(id, SEL))objc_msgSend)(windows,
        sel_registerName("count")) : 0;
    lines = 0;
    fprintf(stderr, "sb26 layers begin filter=%s windows=%lu\n", filter ? filter : "all", count);
    for (unsigned long i = 0; i < count && lines < LAYER_LINES; i++) {
        id window = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows,
            sel_registerName("objectAtIndex:"), i);
        fprintf(stderr, "sb26 layer window %lu %s\n", i, name_of(window));
        walk(send(window, "layer"), 0, filter, filter ? -1 : 1 << 30);
    }
    fprintf(stderr, "sb26 layers end lines=%u%s\n", lines, lines >= LAYER_LINES ? " (cut)" : "");
}

/* Looked up when the first dump is asked for, by which time SpringBoard has both frameworks. */
static void
resolve(void *context)
{
    (void)context;
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOLOAD);
    void *io = dlopen("/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_NOLOAD);
    if (cg) {
        *(void **)&image_type = dlsym(cg, "CGImageGetTypeID");
        *(void **)&image_width = dlsym(cg, "CGImageGetWidth");
        *(void **)&image_height = dlsym(cg, "CGImageGetHeight");
        if (!image_width || !image_height) image_type = NULL;
    }
    if (io) {
        *(void **)&surface_type = dlsym(io, "IOSurfaceGetTypeID");
        *(void **)&surface_width = dlsym(io, "IOSurfaceGetWidth");
        *(void **)&surface_height = dlsym(io, "IOSurfaceGetHeight");
        *(void **)&surface_id = dlsym(io, "IOSurfaceGetID");
        *(void **)&surface_format = dlsym(io, "IOSurfaceGetPixelFormat");
        if (!surface_width || !surface_height || !surface_id || !surface_format) surface_type = NULL;
    }
}

void
sb26_install_layer_dump(void)
{
    static const char *filters[] = { "all", "Icon", "Wallpaper", "Poster", "StatusBar", "Battery", "Widget" };
    for (size_t i = 0; i < sizeof(filters) / sizeof(filters[0]); i++) {
        char name[64];
        int token;
        const char *filter = i ? filters[i] : NULL;
        snprintf(name, sizeof(name), "com.redwood.layers.%s", filters[i]);
        notify_register_dispatch(name, &token, dispatch_get_main_queue(), ^(int t) {
            static dispatch_once_t once;
            (void)t;
            dispatch_once_f(&once, NULL, resolve);
            dump(filter);
        });
    }
}
