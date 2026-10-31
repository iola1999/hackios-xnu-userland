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
 * FourCC, and a full-screen CGImage's mean colour; any other object by its CF type or class) and the effects (describe_effects). The total is capped at LAYER_LINES, because the console ring is 1.3 MB.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

void sb26_install_layer_dump(void);

enum { LAYER_LINES = 1500, LAYER_BELOW = 3, LAYER_DEPTH = 64 };

struct rect { double x, y, w, h; };
struct point { double x, y; };

static size_t (*image_width)(const void *), (*image_height)(const void *);
static size_t (*image_row_bytes)(const void *), (*image_pixel_bits)(const void *);
static const void *(*image_provider)(const void *);
static CFDataRef (*provider_copy)(const void *);
static CFTypeID (*image_type)(void), (*surface_type)(void);
static size_t (*surface_width)(const void *), (*surface_height)(const void *);
static uint32_t (*surface_id)(const void *), (*surface_format)(const void *);
static unsigned lines;
static int to_say;      /* a process whose stderr goes nowhere: lines go to kern.redwood.say */

/* One line of the dump: to stderr, and in a process whose stderr goes nowhere to the kernel log in
 * pieces of at most 240 bytes (kern.redwood.say keeps 255), paced under its 200 lines a second. */
static void __attribute__((format(printf, 1, 2)))
out(const char *format, ...)
{
    char line[4600];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fputs(line, stderr);
    for (size_t at = 0, length = strlen(line); to_say && at < length; at += 240) {
        char piece[248];

        snprintf(piece, sizeof(piece), "%s%.240s", at ? "+ " : "", line + at);
        sysctlbyname("kern.redwood.say", NULL, NULL, piece, strlen(piece) + 1);
        usleep(6000);
    }
}

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

/* A full-screen image's mean over a 64x64 grid, as its four bytes in memory order: tells a black
 * wallpaper snapshot from one that has a picture. Only 32-bit images of at least 512x512. */
static void
describe_pixels(id image, char *out, size_t size)
{
    size_t width = image_width(image), height = image_height(image);
    if (!provider_copy || width < 512 || height < 512 || image_pixel_bits(image) != 32) return;
    CFDataRef data = provider_copy(image_provider(image));
    if (!data) return;
    const uint8_t *bytes = CFDataGetBytePtr(data);
    size_t row = image_row_bytes(image), length = (size_t)CFDataGetLength(data);
    unsigned long sum[4] = { 0 }, count = 0;
    for (size_t y = 0; y < 64; y++) {
        for (size_t x = 0; x < 64; x++) {
            size_t at = (y * height / 64) * row + (x * width / 64) * 4;
            if (at + 4 > length) continue;
            for (int c = 0; c < 4; c++) sum[c] += bytes[at + c];
            count++;
        }
    }
    CFRelease(data);
    if (count)
        snprintf(out, size, " mean %02lx%02lx%02lx%02lx", sum[0] / count, sum[1] / count,
            sum[2] / count, sum[3] / count);
}

/* CoreGraphics' own description of a full-screen image, on one line: says what provides its bytes,
 * which the wallpaper snapshot needs because QuartzCore sends it as a block-compressed image. */
static void
describe_image(id contents, unsigned depth)
{
    if (!contents || !image_type || CFGetTypeID((CFTypeRef)contents) != image_type() ||
        image_width(contents) < 512 || image_height(contents) < 512)
        return;
    CFStringRef described = CFCopyDescription((CFTypeRef)contents);
    char text[1024] = "";
    if (described) {
        CFStringGetCString(described, text, sizeof(text), kCFStringEncodingUTF8);
        CFRelease(described);
    }
    for (char *c = text; *c; c++)
        if (*c == '\n' || *c == '\t') *c = ' ';
    out("sb26 layer %2u image %s\n", depth, text);
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
        size_t written = (size_t)snprintf(out, size, "CGImage %zux%zu", image_width(contents),
            image_height(contents));
        describe_pixels(contents, out + written, size - written);
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

/* A scene host view's scene (FBScene: client process, settings, content state) on one line, for
 * telling a poster scene that was never given content from one whose layer was never hosted. */
static const char *
text_of(id object, char *line, size_t size)
{
    id text = send(object, "description");
    const char *utf8 = text ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : NULL;

    snprintf(line, size, "%s", utf8 ? utf8 : "-");
    for (char *c = line; *c; c++) if (*c == '\n') *c = ' ';
    return line;
}

static long
number_of(id object, const char *selector)
{
    SEL sel = sel_registerName(selector);
    return object && class_respondsToSelector(object_getClass(object), sel)
        ? ((long (*)(id, SEL))objc_msgSend)(object, sel) : -1;
}

static void
describe_scene(id view, unsigned depth)
{
    id scene = send(view, "scene");
    char process[256], client[1200], value_text[1600];

    /* what the container hosts: its layers, its host views and the context that filters them */
    if (strcmp(name_of(view), "_UISceneLayerHostContainerView") == 0) {
        static const char *keys[] = { "_hostedLayers", "_hostViews", "_presentationContext", "_hiddenLayers" };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            CFStringRef key = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
            id value = ((id (*)(id, SEL, id))objc_msgSend)(view, sel_registerName("valueForKey:"), (id)key);
            out("sb26 layer %2u %s of %s: %s\n", depth, keys[i], name_of(view),
                value ? text_of(value, value_text, sizeof(value_text)) : "nil");
            CFRelease(key);
        }
    }
    if (!scene) return;
    out("sb26 layer %2u scene of %s: %s valid %ld active %ld content %ld layers %ld process %s client %s\n",
        depth, name_of(view), name_of(scene), number_of(scene, "isValid") & 0xff, number_of(scene, "isActive") & 0xff,
        number_of(scene, "contentState"), number_of(send(send(scene, "layerManager"), "layers"), "count"),
        text_of(send(scene, "clientProcess"), process, sizeof(process)),
        text_of(send(scene, "clientSettings"), client, sizeof(client)));
    lines++;
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
        out("sb26 layer %2u %s view=%s %.0fx%.0f at %.0f,%.0f%s a=%.2f contents=%s fx=%s\n",
            depth, name_of(layer), name_of(send(layer, "delegate")), bounds.w, bounds.h, at.x, at.y,
            hidden ? " hidden" : "", opacity, contents, effects);
        describe_image(send(layer, "contents"), depth);
        lines++;
        describe_scene(send(layer, "delegate"), depth);
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
    out("sb26 layers begin filter=%s windows=%lu\n", filter ? filter : "all", count);
    for (unsigned long i = 0; i < count && lines < LAYER_LINES; i++) {
        id window = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows,
            sel_registerName("objectAtIndex:"), i);
        SEL context = sel_registerName("_contextId");
        unsigned id_of_context = class_respondsToSelector(object_getClass(window), context)
            ? ((unsigned (*)(id, SEL))objc_msgSend)(window, context) : 0;
        /* the context ID is the one backboardd names in "serialization error from context %x" */
        out("sb26 layer window %lu %s context %x\n", i, name_of(window), id_of_context);
        walk(send(window, "layer"), 0, filter, filter ? -1 : 1 << 30);
    }
    out("sb26 layers end lines=%u%s\n", lines, lines >= LAYER_LINES ? " (cut)" : "");
}

/*
 * `notify com.redwood.battery.nofx` takes the compositing filters off every layer under the battery
 * view. A probe: the GPU path loses the whole battery while the processor path draws it, and the
 * view's one unusual layer is the percentage label that cuts through the fill with destOut. If
 * the battery comes back without the cut, the group the filter makes CoreAnimation render is what
 * the GPU path loses.
 */
static void
clear_filters(id layer, int inside)
{
    if (!layer) return;
    inside = inside || strstr(name_of(send(layer, "delegate")), "Battery") != NULL;
    if (inside && send(layer, "compositingFilter")) {
        out("sb26 nofx %s view=%s\n", name_of(layer), name_of(send(layer, "delegate")));
        ((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("setCompositingFilter:"), nil);
    }
    id sublayers = send(layer, "sublayers");
    unsigned long count = sublayers ? ((unsigned long (*)(id, SEL))objc_msgSend)(sublayers,
        sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < count; i++)
        clear_filters(((id (*)(id, SEL, unsigned long))objc_msgSend)(sublayers,
            sel_registerName("objectAtIndex:"), i), inside);
}

static void
clear_battery_filters(void)
{
    Class window_class = objc_getClass("UIWindow");
    SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    id windows = window_class && class_respondsToSelector(object_getClass((id)window_class), all)
        ? ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)window_class, all, YES, NO) : nil;
    unsigned long count = windows ? ((unsigned long (*)(id, SEL))objc_msgSend)(windows,
        sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < count; i++)
        clear_filters(send(((id (*)(id, SEL, unsigned long))objc_msgSend)(windows,
            sel_registerName("objectAtIndex:"), i), "layer"), 0);
    out("sb26 nofx done windows=%lu\n", count);
}

/*
 * `notify com.redwood.shapes.test` puts six test layers into the status bar window, in a row under
 * the status bar: a red CALayer, the same with a corner radius, with continuous corners, a filled
 * CAShapeLayer ellipse, a stroked one, and a filled one in white at 35% (the battery body's kind of
 * colour). A probe: the battery's two shape layers and its plain fill layer are missing under GPU
 * compositing while bitmap contents beside them are drawn, and the row says which kind of layer
 * the GPU path loses.
 */
static void
add_test_layers(void)
{
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOLOAD);
    void *(*color)(double, double, double, double) = cg ? dlsym(cg, "CGColorCreateSRGB") : NULL;
    void *(*ellipse)(struct rect, const void *) = cg ? dlsym(cg, "CGPathCreateWithEllipseInRect") : NULL;
    Class window_class = objc_getClass("UIWindow"), layer_class = objc_getClass("CALayer");
    Class shape_class = objc_getClass("CAShapeLayer");
    SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    id windows = window_class && class_respondsToSelector(object_getClass((id)window_class), all)
        ? ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)window_class, all, YES, NO) : nil;
    unsigned long count = windows ? ((unsigned long (*)(id, SEL))objc_msgSend)(windows,
        sel_registerName("count")) : 0;
    id host = nil;

    for (unsigned long i = 0; i < count && !host; i++) {
        id window = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i);
        if (strcmp(name_of(window), "SBStatusBarWindow") == 0) host = send(window, "layer");
    }
    if (!host || !color || !ellipse || !layer_class || !shape_class) {
        out("sb26 shapes: nothing to add them to (window %p)\n", (void *)host);
        return;
    }
    void *red = color(1, 0, 0, 1), *faint = color(1, 1, 1, 0.35);
    for (int i = 0; i < 6; i++) {
        struct rect frame = { 15 + i * 60, 60, 50, 40 }, local = { 0, 0, 50, 40 };
        id layer = ((id (*)(id, SEL))objc_msgSend)((id)(i < 3 ? layer_class : shape_class), sel_registerName("new"));
        ((void (*)(id, SEL, struct rect))objc_msgSend)(layer, sel_registerName("setFrame:"), frame);
        if (i < 3) {
            ((void (*)(id, SEL, void *))objc_msgSend)(layer, sel_registerName("setBackgroundColor:"), red);
            if (i > 0) ((void (*)(id, SEL, double))objc_msgSend)(layer, sel_registerName("setCornerRadius:"), 10);
            if (i == 2) ((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("setCornerCurve:"),
                ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
                    sel_registerName("stringWithUTF8String:"), "continuous"));
        } else {
            void *path = ellipse(local, NULL);
            ((void (*)(id, SEL, void *))objc_msgSend)(layer, sel_registerName("setPath:"), path);
            ((void (*)(id, SEL, void *))objc_msgSend)(layer, sel_registerName("setFillColor:"), i == 4 ? NULL : i == 5 ? faint : red);
            if (i == 4) {
                ((void (*)(id, SEL, void *))objc_msgSend)(layer, sel_registerName("setStrokeColor:"), red);
                ((void (*)(id, SEL, double))objc_msgSend)(layer, sel_registerName("setLineWidth:"), 4);
            }
            CFRelease(path);
        }
        ((void (*)(id, SEL, id))objc_msgSend)(host, sel_registerName("addSublayer:"), layer);
    }
    CFRelease(red);
    CFRelease(faint);
    out("sb26 shapes: six test layers added to %s\n", name_of(host));
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
        *(void **)&image_row_bytes = dlsym(cg, "CGImageGetBytesPerRow");
        *(void **)&image_pixel_bits = dlsym(cg, "CGImageGetBitsPerPixel");
        *(void **)&image_provider = dlsym(cg, "CGImageGetDataProvider");
        *(void **)&provider_copy = dlsym(cg, "CGDataProviderCopyData");
        if (!image_row_bytes || !image_pixel_bits || !image_provider) provider_copy = NULL;
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
    int token;
    notify_register_dispatch("com.redwood.battery.nofx", &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        clear_battery_filters();
    });
    notify_register_dispatch("com.redwood.shapes.test", &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        add_test_layers();
    });
}

/* MercuryPosterExtension draws the wallpaper SpringBoard hosts (PBUIPosterSceneLayerHostView): the
 * same dump of its own windows, by the same notifications. */
__attribute__((constructor)) static void
install_poster_dump(void)
{
    if (!getprogname() || strcmp(getprogname(), "MercuryPosterExtension")) return;
    to_say = 1;
    sb26_install_layer_dump();
    out("sb26 layers: dump installed in %s\n", getprogname());
    /* The sandbox does not deliver the notifications here (kernel-12377-cmd37-bo13): dump on a timer. */
    for (int64_t seconds = 40; seconds <= 160; seconds += 120) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, seconds * (int64_t)NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            static dispatch_once_t once;
            dispatch_once_f(&once, NULL, resolve);
            dump(NULL);
        });
    }
}
