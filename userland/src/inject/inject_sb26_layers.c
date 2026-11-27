/*
 * SpringBoard's layer tree on request, for telling an empty layer from one the compositor did not
 * draw: a missing icon is either a layer with no contents (nothing was rendered into it) or one
 * whose contents are there and did not reach the screen.
 *
 * `notify com.annibale.layers.<filter>` prints, to stderr and so to the console, every layer whose
 * own class or whose view's class contains <filter> (case sensitive), at any depth, with the layers
 * below it up to LAYER_BELOW levels. `com.annibale.layers.all` prints the whole tree.
 * `com.annibale.layers.animated` prints every layer with animations attached (key, class, timing, frame
 * rate range). `com.annibale.layers.full` prints the full subtree without truncation and exports layer visual
 * styling (background, border, corner, masks, and filter parameters) to /private/var/tmp/layers-dump.txt.
 * Each line contains depth, layer class, view class, bounds and position, hidden/opacity, visual styles,
 * contents (CGImage/IOSurface with dimensions, mean colour and black ratio) and effects/filters.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

void sb26_install_layer_dump(void);

enum {
    LAYER_LINES = 1500,
    LAYER_LINES_FULL = 20000,
    LAYER_BELOW = 8,
    LAYER_DEPTH = 64
};

struct rect { double x, y, w, h; };
struct point { double x, y; };

static size_t (*image_width)(const void *), (*image_height)(const void *);
static size_t (*image_row_bytes)(const void *), (*image_pixel_bits)(const void *);
static const void *(*image_provider)(const void *);
static CFDataRef (*provider_copy)(const void *);
static CFTypeID (*image_type)(void), (*surface_type)(void);
static size_t (*surface_width)(const void *), (*surface_height)(const void *);
static uint32_t (*surface_id)(const void *), (*surface_format)(const void *);
static size_t (*surface_plane_count)(const void *);
static kern_return_t (*surface_lock)(const void *, uint32_t, uint32_t *);
static kern_return_t (*surface_unlock)(const void *, uint32_t, uint32_t *);
static void *(*surface_base_address)(const void *);
static size_t (*surface_row_bytes)(const void *);

static size_t (*color_count)(const void *);
static const double *(*color_components)(const void *);

static unsigned lines;
static int to_say;      /* a process whose stderr goes nowhere: lines go to kern.annibale.say */
static FILE *dump_fp;

/* One line of the dump: to stderr, to dump file if full, and in a process whose stderr goes
 * nowhere to the kernel log in pieces of at most 240 bytes. */
static void __attribute__((format(printf, 1, 2)))
out(const char *format, ...)
{
    char line[4600];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fputs(line, stderr);
    if (dump_fp) fputs(line, dump_fp);
    for (size_t at = 0, length = strlen(line); to_say && at < length; at += 240) {
        char piece[248];

        snprintf(piece, sizeof(piece), "%s%.240s", at ? "+ " : "", line + at);
        sysctlbyname("kern.annibale.say", NULL, NULL, piece, strlen(piece) + 1);
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

/* Adaptive grid sampling across arbitrary size CGImage (8, 16, 32-bit formats).
 * Computes mean RRGGBBAA and black percentage (black P%). */
static void
describe_pixels(id image, char *out_buf, size_t size)
{
    if (!image || !image_width || !image_height || !image_row_bytes || !image_pixel_bits || !provider_copy)
        return;
    size_t width = image_width(image), height = image_height(image);
    size_t bpp = image_pixel_bits(image);
    if (width == 0 || height == 0 || bpp == 0) return;

    CFDataRef data = provider_copy(image_provider(image));
    if (!data) return;
    const uint8_t *bytes = CFDataGetBytePtr(data);
    size_t row = image_row_bytes(image), length = (size_t)CFDataGetLength(data);
    if (!bytes || length == 0) {
        CFRelease(data);
        return;
    }

    size_t grid_x = width > 32 ? 32 : width;
    size_t grid_y = height > 32 ? 32 : height;
    unsigned long sum[4] = { 0 }, count = 0, black_count = 0;
    size_t bytes_per_pixel = bpp / 8;
    if (bytes_per_pixel == 0) bytes_per_pixel = 1;

    for (size_t gy = 0; gy < grid_y; gy++) {
        size_t y = gy * height / grid_y;
        for (size_t gx = 0; gx < grid_x; gx++) {
            size_t x = gx * width / grid_x;
            size_t at = y * row + x * bytes_per_pixel;
            if (at + bytes_per_pixel > length) continue;
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (bpp == 8) {
                r = g = b = bytes[at];
            } else if (bpp == 16) {
                r = bytes[at];
                g = bytes[at + 1];
                b = 0;
            } else if (bpp == 24) {
                r = bytes[at];
                g = bytes[at + 1];
                b = bytes[at + 2];
                a = 255;
            } else if (bpp >= 32) {
                r = bytes[at];
                g = bytes[at + 1];
                b = bytes[at + 2];
                a = bytes[at + 3];
            } else {
                continue;
            }
            if (r <= 2 && g <= 2 && b <= 2) black_count++;
            sum[0] += r; sum[1] += g; sum[2] += b; sum[3] += a;
            count++;
        }
    }
    CFRelease(data);
    if (count) {
        unsigned black_pct = (unsigned)(black_count * 100 / count);
        snprintf(out_buf, size, " mean %02lx%02lx%02lx%02lx black %u%%",
            sum[0] / count, sum[1] / count, sum[2] / count, sum[3] / count, black_pct);
    }
}

/* Sample 16x16 grid of an IOSurface under read lock and output mean RRGGBBAA and black %. */
static void
describe_surface_pixels(const void *surface, char *out_buf, size_t size)
{
    if (!surface || !surface_lock || !surface_unlock || !surface_base_address || !surface_row_bytes || !surface_format)
        return;
    size_t sw = surface_width(surface), sh = surface_height(surface);
    if (sw == 0 || sh == 0) return;

    size_t plane_count = surface_plane_count ? surface_plane_count(surface) : 1;
    if (plane_count > 1) return;

    uint32_t f = surface_format(surface);
    if (f != 0x42475241 /* 'BGRA' */ &&
        f != 0x52474241 /* 'RGBA' */ &&
        f != 0x41524742 /* 'ARGB' */ &&
        f != 0x41424752 /* 'ABGR' */) {
        return;
    }

    /* Lock read-only: kIOSurfaceLockReadOnly = 1 */
    kern_return_t kr = surface_lock(surface, 1, NULL);
    if (kr != KERN_SUCCESS) return;

    const uint8_t *base = (const uint8_t *)surface_base_address(surface);
    size_t row = surface_row_bytes(surface);
    if (!base || row == 0 || row < sw * 4) {
        surface_unlock(surface, 1, NULL);
        return;
    }

    size_t grid_x = sw > 16 ? 16 : sw;
    size_t grid_y = sh > 16 ? 16 : sh;
    unsigned long sum[4] = { 0 }, count = 0, black_count = 0;

    for (size_t gy = 0; gy < grid_y; gy++) {
        size_t y = gy * sh / grid_y;
        for (size_t gx = 0; gx < grid_x; gx++) {
            size_t x = gx * sw / grid_x;
            size_t at = y * row + x * 4;
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (f == 0x42475241 /* 'BGRA' */) {
                b = base[at]; g = base[at + 1]; r = base[at + 2]; a = base[at + 3];
            } else if (f == 0x52474241 /* 'RGBA' */) {
                r = base[at]; g = base[at + 1]; b = base[at + 2]; a = base[at + 3];
            } else if (f == 0x41524742 /* 'ARGB' */) {
                a = base[at]; r = base[at + 1]; g = base[at + 2]; b = base[at + 3];
            } else if (f == 0x41424752 /* 'ABGR' */) {
                a = base[at]; b = base[at + 1]; g = base[at + 2]; r = base[at + 3];
            }
            if (r <= 2 && g <= 2 && b <= 2) black_count++;
            sum[0] += r; sum[1] += g; sum[2] += b; sum[3] += a;
            count++;
        }
    }
    surface_unlock(surface, 1, NULL);
    if (count) {
        unsigned black_pct = (unsigned)(black_count * 100 / count);
        snprintf(out_buf, size, " mean %02lx%02lx%02lx%02lx black %u%%",
            sum[0] / count, sum[1] / count, sum[2] / count, sum[3] / count, black_pct);
    }
}

/* CoreGraphics' own description of an image on one line. */
static void
describe_image(id contents, unsigned depth)
{
    if (!contents || !image_type || CFGetTypeID((CFTypeRef)contents) != image_type())
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
describe_contents(id contents, char *out_buf, size_t size)
{
    if (!contents) {
        snprintf(out_buf, size, "none");
        return;
    }
    CFTypeID type = CFGetTypeID((CFTypeRef)contents);
    if (image_type && type == image_type()) {
        size_t written = (size_t)snprintf(out_buf, size, "CGImage %zux%zu", image_width(contents),
            image_height(contents));
        describe_pixels(contents, out_buf + written, size > written ? size - written : 0);
    } else if (surface_type && type == surface_type()) {
        uint32_t f = surface_format(contents);
        size_t written = (size_t)snprintf(out_buf, size, "IOSurface %u %zux%zu '%c%c%c%c'", surface_id(contents),
            surface_width(contents), surface_height(contents),
            (char)(f >> 24), (char)(f >> 16), (char)(f >> 8), (char)f);
        describe_surface_pixels(contents, out_buf + written, size > written ? size - written : 0);
    } else {
        CFStringRef described = CFCopyTypeIDDescription(type);
        char text[64] = "";
        if (described) {
            CFStringGetCString(described, text, sizeof(text), kCFStringEncodingUTF8);
            CFRelease(described);
        }
        snprintf(out_buf, size, "%s", text[0] ? text : name_of(contents));
    }
}

static inline unsigned
clamp_to_255(double val)
{
    if (val <= 0.0) return 0;
    if (val >= 1.0) return 255;
    return (unsigned)(val * 255.0 + 0.5);
}

/* Extract RGBA hex string from a CGColor object. */
static void
describe_color(id cgcolor, char *out_buf, size_t size)
{
    if (!cgcolor || !color_count || !color_components) {
        snprintf(out_buf, size, "none");
        return;
    }
    size_t nc = color_count((CFTypeRef)cgcolor);
    const double *c = color_components((CFTypeRef)cgcolor);
    if (!c || nc == 0) {
        snprintf(out_buf, size, "none");
        return;
    }
    if (nc >= 4) {
        unsigned r = clamp_to_255(c[0]);
        unsigned g = clamp_to_255(c[1]);
        unsigned b = clamp_to_255(c[2]);
        unsigned a = clamp_to_255(c[3]);
        snprintf(out_buf, size, "%02x%02x%02x%02x", r, g, b, a);
    } else if (nc >= 2) {
        unsigned v = clamp_to_255(c[0]);
        unsigned a = clamp_to_255(c[1]);
        snprintf(out_buf, size, "%02x%02x%02x%02x", v, v, v, a);
    } else {
        unsigned v = clamp_to_255(c[0]);
        snprintf(out_buf, size, "%02x%02x%02xff", v, v, v);
    }
}

/* Detailed visual style attributes: background, border, corner, and masksToBounds. */
static void
describe_style(id layer, char *out_buf, size_t size)
{
    char bg[32] = "none", border[48] = "none", corner[48] = "0";
    id bgcolor = send(layer, "backgroundColor");
    if (bgcolor) describe_color(bgcolor, bg, sizeof(bg));

    double bw = 0.0;
    SEL bw_sel = sel_registerName("borderWidth");
    if (class_respondsToSelector(object_getClass(layer), bw_sel))
        bw = ((double (*)(id, SEL))objc_msgSend)(layer, bw_sel);
    if (bw > 0.0) {
        id bcolor = send(layer, "borderColor");
        char bc[32] = "none";
        if (bcolor) describe_color(bcolor, bc, sizeof(bc));
        snprintf(border, sizeof(border), "%.1f@%s", bw, bc);
    }

    double cr = 0.0;
    SEL cr_sel = sel_registerName("cornerRadius");
    if (class_respondsToSelector(object_getClass(layer), cr_sel))
        cr = ((double (*)(id, SEL))objc_msgSend)(layer, cr_sel);
    if (cr > 0.0) {
        id curve = send(layer, "cornerCurve");
        const char *cname = curve ? ((const char *(*)(id, SEL))objc_msgSend)(curve, sel_registerName("UTF8String")) : NULL;
        if (cname && strlen(cname) > 0)
            snprintf(corner, sizeof(corner), "%.1f(%s)", cr, cname);
        else
            snprintf(corner, sizeof(corner), "%.1f", cr);
    }

    BOOL masks = NO;
    SEL masks_sel = sel_registerName("masksToBounds");
    if (class_respondsToSelector(object_getClass(layer), masks_sel))
        masks = ((BOOL (*)(id, SEL))objc_msgSend)(layer, masks_sel);

    snprintf(out_buf, size, "bg=%s border=%s corner=%s masks=%d", bg, border, corner, masks ? 1 : 0);
}

/* List filter and backgroundFilter names and key parameters. */
static void
describe_layer_filters(id layer, char *out_buf, size_t size)
{
    id filters = send(layer, "filters");
    id bfilters = send(layer, "backgroundFilters");
    SEL count_sel = sel_registerName("count");
    SEL obj_sel = sel_registerName("objectAtIndex:");
    unsigned long n_f = filters ? ((unsigned long (*)(id, SEL))objc_msgSend)(filters, count_sel) : 0;
    unsigned long n_bf = bfilters ? ((unsigned long (*)(id, SEL))objc_msgSend)(bfilters, count_sel) : 0;
    if (n_f == 0 && n_bf == 0) {
        snprintf(out_buf, size, "none");
        return;
    }
    size_t at = 0;
    at += (size_t)snprintf(out_buf + at, size > at ? size - at : 0, "[");
    for (unsigned long i = 0; i < n_f && at < size; i++) {
        id f = ((id (*)(id, SEL, unsigned long))objc_msgSend)(filters, obj_sel, i);
        id name = send(f, "name");
        id type = send(f, "type");
        const char *t = name ? ((const char *(*)(id, SEL))objc_msgSend)(name, sel_registerName("UTF8String"))
                             : type ? ((const char *(*)(id, SEL))objc_msgSend)(type, sel_registerName("UTF8String"))
                             : name_of(f);
        at += (size_t)snprintf(out_buf + at, size > at ? size - at : 0, "%s%s", i > 0 ? "," : "", t ? t : "-");
    }
    if (n_bf > 0 && at < size) {
        at += (size_t)snprintf(out_buf + at, size > at ? size - at : 0, "|bg:");
        for (unsigned long i = 0; i < n_bf && at < size; i++) {
            id f = ((id (*)(id, SEL, unsigned long))objc_msgSend)(bfilters, obj_sel, i);
            id name = send(f, "name");
            id type = send(f, "type");
            const char *t = name ? ((const char *(*)(id, SEL))objc_msgSend)(name, sel_registerName("UTF8String"))
                                 : type ? ((const char *(*)(id, SEL))objc_msgSend)(type, sel_registerName("UTF8String"))
                                 : name_of(f);
            at += (size_t)snprintf(out_buf + at, size > at ? size - at : 0, "%s%s", i > 0 ? "," : "", t ? t : "-");
        }
    }
    if (at < size) snprintf(out_buf + at, size - at, "]");
}

/* What changes how a layer is drawn beyond its contents, as "fx=COMPOSITING/FILTERS/MASK". */
static void
describe_effects(id layer, char *out_buf, size_t size)
{
    id compositing = send(layer, "compositingFilter"), name = send(compositing, "name");
    id filters = send(layer, "filters"), backdrop = send(layer, "backgroundFilters");
    SEL count = sel_registerName("count");
    const char *text = name ? ((const char *(*)(id, SEL))objc_msgSend)(name, sel_registerName("UTF8String"))
        : compositing ? name_of(compositing) : "-";
    snprintf(out_buf, size, "%s/%lu+%lu/%s", text,
        filters ? ((unsigned long (*)(id, SEL))objc_msgSend)(filters, count) : 0,
        backdrop ? ((unsigned long (*)(id, SEL))objc_msgSend)(backdrop, count) : 0,
        name_of(send(layer, "mask")));
}

/* A scene host view's scene on one line. */
static const char *
text_of(id object, char *line, size_t size)
{
    id text = send(object, "description");
    const char *utf8 = text ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : NULL;

    snprintf(line, size, "%s", utf8 ? utf8 : "-");
    for (char *c = line; *c; c++) if (*c == '\n') *c = ' ';
    return line;
}

/* Private getters use the measured 23G90 signatures; reject a different ABI before calling. */
static SEL
checked_getter(id object, const char *name, const char *encoding)
{
    SEL selector = sel_registerName(name);
    Method method = object ? class_getInstanceMethod(object_getClass(object), selector) : NULL;
    const char *actual = method ? method_getTypeEncoding(method) : NULL;
    return actual && strcmp(actual, encoding) == 0 ? selector : NULL;
}

static int
checked_flag(id object, const char *name)
{
    SEL selector = checked_getter(object, name, "B16@0:8");
    return selector ? ((BOOL (*)(id, SEL))objc_msgSend)(object, selector) : -1;
}

static id
checked_object(id object, const char *name)
{
    SEL selector = checked_getter(object, name, "@16@0:8");
    return selector ? ((id (*)(id, SEL))objc_msgSend)(object, selector) : nil;
}

static long long
number_of(id object, const char *name, const char *encoding)
{
    SEL selector = checked_getter(object, name, encoding);
    if (!selector) return -1;
    switch (encoding[0]) {
    case 'c': return ((signed char (*)(id, SEL))objc_msgSend)(object, selector);
    case 'q': return ((long long (*)(id, SEL))objc_msgSend)(object, selector);
    case 'Q': return ((unsigned long long (*)(id, SEL))objc_msgSend)(object, selector);
    default: return -1;
    }
}

static void
describe_scene(id view, unsigned depth)
{
    id scene = checked_object(view, "scene");
    char process[256], client[1200], value_text[1600], identifier[256];

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
    /* This dump visits hosted scenes; a process without a hosted scene is absent. */
    if (!scene) return;
    id client_process = checked_object(scene, "clientProcess");
    id settings = checked_object(scene, "settings");
    id scene_id = checked_object(scene, "identifier");
    id layers = checked_object(checked_object(scene, "layerManager"), "layers");
    out("sb26 layer %2u scene of %s: %s valid %d active %d content %lld layers %lld process %s client %s"
        " sceneID %s finishedLaunching %d isPendingExit %d taskState %lld versionedPID %lld"
        " isForeground %d activityMode %lld\n",
        depth, name_of(view), name_of(scene), checked_flag(scene, "isValid"), checked_flag(scene, "isActive"),
        number_of(scene, "contentState", "q16@0:8"), number_of(layers, "count", "Q16@0:8"),
        text_of(client_process, process, sizeof(process)),
        text_of(checked_object(scene, "clientSettings"), client, sizeof(client)),
        scene_id ? text_of(scene_id, identifier, sizeof(identifier)) : "unavailable",
        checked_flag(client_process, "finishedLaunching"), checked_flag(client_process, "isPendingExit"),
        number_of(client_process, "taskState", "q16@0:8"), number_of(client_process, "versionedPID", "q16@0:8"),
        checked_flag(settings, "isForeground"), number_of(settings, "activityMode", "c16@0:8"));
    lines++;
}

/* An unavailable SDF value is printed as nan or -1. */
static double
sdf_number(id object, const char *name)
{
    SEL selector = checked_getter(object, name, "d16@0:8");
    return selector ? ((double (*)(id, SEL))objc_msgSend)(object, selector) : NAN;
}

static const char *
sdf_string(id object, const char *name, char *buffer, size_t size)
{
    SEL selector = checked_getter(object, name, "@16@0:8");
    id value = selector ? ((id (*)(id, SEL))objc_msgSend)(object, selector) : nil;
    if (!value || CFGetTypeID((CFTypeRef)value) != CFStringGetTypeID() ||
        !CFStringGetCString((CFStringRef)value, buffer, size, kCFStringEncodingUTF8))
        snprintf(buffer, size, "unavailable");
    return buffer;
}

static void
describe_sdf(id layer, unsigned depth)
{
    BOOL sdf = NO, element = NO;
    Class sdf_class = objc_getClass("CASDFLayer"), element_class = objc_getClass("CASDFElementLayer");
    for (Class cls = object_getClass(layer); cls; cls = class_getSuperclass(cls)) {
        sdf |= cls == sdf_class;
        element |= cls == element_class;
    }
    if (sdf) {
        SEL selector = checked_getter(layer, "effect", "@16@0:8");
        id effect = selector ? ((id (*)(id, SEL))objc_msgSend)(layer, selector) : nil;
        char color[32] = "unavailable";
        selector = checked_getter(effect, "color", "^{CGColor=}16@0:8");
        if (selector) describe_color(((id (*)(id, SEL))objc_msgSend)(effect, selector), color, sizeof(color));
        out("sb26 layer %2u sdf effect=%.96s offset=%.6g smoothness=%.6g gaussian=%.6g merge=%d color=%s height=%.6g angle=%.6g curvature=%.6g amount=%.6g spread=%.6g maskOffset=%.6g global=%d\n",
            depth, name_of(effect), sdf_number(layer, "effectOffset"), sdf_number(layer, "smoothness"),
            sdf_number(layer, "gaussianRadius"), checked_flag(layer, "mergeElements"), color,
            sdf_number(effect, "height"), sdf_number(effect, "angle"), sdf_number(effect, "curvature"),
            sdf_number(effect, "amount"), sdf_number(effect, "spread"), sdf_number(effect, "maskOffset"),
            checked_flag(effect, "global"));
        lines++;
    } else if (element) {
        char mode[48], operation[48];
        out("sb26 layer %2u sdf-element mode=%s operation=%s zeroDistance=%.6g oneDistance=%.6g ovalization=%.6g hitTestsAsFill=%d\n",
            depth, sdf_string(layer, "mode", mode, sizeof(mode)),
            sdf_string(layer, "operation", operation, sizeof(operation)),
            sdf_number(layer, "contentsZeroValueDistance"), sdf_number(layer, "contentsOneValueDistance"),
            sdf_number(layer, "gradientOvalization"), checked_flag(layer, "hitTestsAsFill"));
        lines++;
    }
}

/* `animated`: every layer with animations attached, each animation's key, class, timing and frame rate range */
static void
describe_animations(id layer, unsigned depth)
{
    id keys = send(layer, "animationKeys");
    unsigned long count = keys ? ((unsigned long (*)(id, SEL))objc_msgSend)(keys, sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < count; i++) {
        id key = ((id (*)(id, SEL, unsigned long))objc_msgSend)(keys, sel_registerName("objectAtIndex:"), i);
        id animation = ((id (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("animationForKey:"), key);
        const char *text = ((const char *(*)(id, SEL))objc_msgSend)(key, sel_registerName("UTF8String"));
        struct { float minimum, maximum, preferred; } range = { 0, 0, 0 };
        SEL range_getter = checked_getter(animation, "preferredFrameRateRange", "{CAFrameRateRange=fff}16@0:8");
        if (range_getter)
            range = ((__typeof__(range) (*)(id, SEL))objc_msgSend)(animation, range_getter);
        out("sb26 layer %2u animated %s view=%s key=%.80s %s duration=%.3g repeat=%.3g rate=%.0f..%.0f@%.0f\n",
            depth, name_of(layer), name_of(send(layer, "delegate")), text ? text : "?", name_of(animation),
            ((double (*)(id, SEL))objc_msgSend)(animation, sel_registerName("duration")),
            (double)((float (*)(id, SEL))objc_msgSend)(animation, sel_registerName("repeatCount")),
            range.minimum, range.maximum, range.preferred);
        lines++;
    }
}

static int
matches(id layer, const char *filter)
{
    if (!filter) return 1;
    return strstr(name_of(layer), filter) || strstr(name_of(send(layer, "delegate")), filter);
}

static void
walk(id layer, unsigned depth, const char *filter, int below, int below_limit, int full)
{
    unsigned max_lines = full ? LAYER_LINES_FULL : LAYER_LINES;
    if (!layer || lines >= max_lines || depth > LAYER_DEPTH) return;
    if (filter && strcmp(filter, "animated") == 0) describe_animations(layer, depth);
    if (below < 0 && matches(layer, filter)) below = below_limit;
    if (below >= 0 || full) {
        struct rect bounds = ((struct rect (*)(id, SEL))objc_msgSend)(layer, sel_registerName("bounds"));
        BOOL hidden = ((BOOL (*)(id, SEL))objc_msgSend)(layer, sel_registerName("isHidden"));
        float opacity = ((float (*)(id, SEL))objc_msgSend)(layer, sel_registerName("opacity"));
        struct point at = ((struct point (*)(id, SEL))objc_msgSend)(layer, sel_registerName("position"));
        char contents[160], effects[96], style[160], flist[160];
        describe_contents(send(layer, "contents"), contents, sizeof(contents));
        describe_effects(layer, effects, sizeof(effects));
        describe_style(layer, style, sizeof(style));
        describe_layer_filters(layer, flist, sizeof(flist));
        out("sb26 layer %2u %s view=%s %.0fx%.0f at %.0f,%.0f%s a=%.2f %s contents=%s fx=%s filters=%s\n",
            depth, name_of(layer), name_of(send(layer, "delegate")), bounds.w, bounds.h, at.x, at.y,
            hidden ? " hidden" : "", opacity, style, contents, effects, flist);
        describe_image(send(layer, "contents"), depth);
        lines++;
        describe_scene(send(layer, "delegate"), depth);
        if ((full || (filter && strcmp(filter, "SDF") == 0)) && lines < max_lines)
            describe_sdf(layer, depth);
    }
    id sublayers = send(layer, "sublayers");
    unsigned long count = sublayers ? ((unsigned long (*)(id, SEL))objc_msgSend)(sublayers,
        sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < count && lines < max_lines; i++) {
        id child = ((id (*)(id, SEL, unsigned long))objc_msgSend)(sublayers,
            sel_registerName("objectAtIndex:"), i);
        walk(child, depth + 1, filter, full ? (1 << 30) : (below > 0 ? below - 1 : -1), below_limit, full);
    }
}

static void
dump_ex(const char *filter, int below_limit, int full)
{
    FILE *fp = NULL;
    if (below_limit < 0) below_limit = LAYER_BELOW;
    if (full) {
        fp = fopen("/private/var/tmp/layers-dump.txt", "w");
        if (!fp) fp = fopen("/tmp/layers-dump.txt", "w");
        dump_fp = fp;
    }
    Class window_class = objc_getClass("UIWindow");
    SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    id windows = window_class && class_respondsToSelector(object_getClass((id)window_class), all)
        ? ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)window_class, all, YES, NO) : nil;
    unsigned long count = windows ? ((unsigned long (*)(id, SEL))objc_msgSend)(windows,
        sel_registerName("count")) : 0;
    lines = 0;
    unsigned max_lines = full ? LAYER_LINES_FULL : LAYER_LINES;
    out("sb26 layers begin proc=%s filter=%s below=%d full=%d windows=%lu\n",
        getprogname() ? getprogname() : "-", filter ? filter : (full ? "full" : "all"), below_limit, full, count);
    for (unsigned long i = 0; i < count && lines < max_lines; i++) {
        id window = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows,
            sel_registerName("objectAtIndex:"), i);
        SEL context = sel_registerName("_contextId");
        unsigned id_of_context = class_respondsToSelector(object_getClass(window), context)
            ? ((unsigned (*)(id, SEL))objc_msgSend)(window, context) : 0;
        out("sb26 layer window %lu %s context %x\n", i, name_of(window), id_of_context);
        walk(send(window, "layer"), 0, filter, filter ? -1 : 1 << 30, below_limit, full);
    }
    out("sb26 layers end lines=%u%s\n", lines, lines >= max_lines ? " (cut)" : "");
    if (dump_fp) {
        fclose(dump_fp);
        dump_fp = NULL;
    }
}

static void
dump(const char *filter, int full)
{
    dump_ex(filter, LAYER_BELOW, full);
}

static void
dump_from_query_file(void)
{
    FILE *fp = fopen("/private/var/tmp/layers-query.txt", "r");
    if (!fp) fp = fopen("/tmp/layers-query.txt", "r");
    char buf[512] = "", filter_buf[128] = "";
    int below = LAYER_BELOW, full = 0;

    if (fp) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[n] = '\0';
        fclose(fp);
    }
    char *rest = NULL;
    for (char *tok = strtok_r(buf, " \t\r\n", &rest); tok != NULL; tok = strtok_r(NULL, " \t\r\n", &rest)) {
        if (strncmp(tok, "filter=", 7) == 0) {
            snprintf(filter_buf, sizeof(filter_buf), "%s", tok + 7);
        } else if (strncmp(tok, "below=", 6) == 0) {
            below = atoi(tok + 6);
        } else if (strncmp(tok, "depth=", 6) == 0) {
            below = atoi(tok + 6);
        } else if (strncmp(tok, "full=", 5) == 0) {
            full = atoi(tok + 5) != 0;
        } else if (strchr(tok, '=') == NULL && filter_buf[0] == '\0') {
            snprintf(filter_buf, sizeof(filter_buf), "%s", tok);
        }
    }
    const char *filter = (filter_buf[0] && strcmp(filter_buf, "all") != 0) ? filter_buf : NULL;
    dump_ex(filter, below, full);
}

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

        *(void **)&color_count = dlsym(cg, "CGColorGetNumberOfComponents");
        *(void **)&color_components = dlsym(cg, "CGColorGetComponents");
    }
    if (io) {
        *(void **)&surface_type = dlsym(io, "IOSurfaceGetTypeID");
        *(void **)&surface_width = dlsym(io, "IOSurfaceGetWidth");
        *(void **)&surface_height = dlsym(io, "IOSurfaceGetHeight");
        *(void **)&surface_id = dlsym(io, "IOSurfaceGetID");
        *(void **)&surface_format = dlsym(io, "IOSurfaceGetPixelFormat");
        *(void **)&surface_plane_count = dlsym(io, "IOSurfaceGetPlaneCount");
        *(void **)&surface_lock = dlsym(io, "IOSurfaceLock");
        *(void **)&surface_unlock = dlsym(io, "IOSurfaceUnlock");
        *(void **)&surface_base_address = dlsym(io, "IOSurfaceGetBaseAddress");
        *(void **)&surface_row_bytes = dlsym(io, "IOSurfaceGetBytesPerRow");
        if (!surface_width || !surface_height || !surface_id || !surface_format) surface_type = NULL;
    }
}

/*
 * Boot-arg idlelog=1: every idle timer the coordinator applies, every behavior a provider proposes
 * and every disable assertion taken, with their descriptions, to tell why the screen never
 * dims and locks by itself.
 */
int sb26_boot_arg_present(const char *wanted);      /* inject_sb26.c */

static const char *
described(id object, char *text, size_t size)
{
    id d = object ? send(object, "description") : nil;
    const char *c = d ? ((const char *(*)(id, SEL))objc_msgSend)(d, sel_registerName("UTF8String")) : NULL;
    snprintf(text, size, "%s", c ? c : "(nil)");
    for (char *p = text; *p; p++)
        if (*p == '\n') *p = ' ';
    return text;
}

static void (*idle_set_original)(id, SEL, id, id);
static void
idle_set(id self, SEL cmd, id descriptor, id reason)
{
    char a[600], b[160];
    out("sb26 idletimer: apply %s for %s\n", described(descriptor, a, sizeof(a)), described(reason, b, sizeof(b)));
    idle_set_original(self, cmd, descriptor, reason);
}

static void (*idle_propose_original)(id, SEL, id, id, id);
static void
idle_propose(id self, SEL cmd, id provider, id behavior, id reason)
{
    char a[200], b[400], c[160];
    out("sb26 idletimer: %s proposes %s for %s\n", described(provider, a, sizeof(a)), described(behavior, b, sizeof(b)),
        described(reason, c, sizeof(c)));
    idle_propose_original(self, cmd, provider, behavior, reason);
}

static id (*idle_disable_original)(id, SEL, id);
static id
idle_disable(id self, SEL cmd, id reason)
{
    char a[160];
    out("sb26 idletimer: disable assertion for %s\n", described(reason, a, sizeof(a)));
    return idle_disable_original(self, cmd, reason);
}

static void (*idle_event_original)(id, SEL, id);
static void
idle_event(id self, SEL cmd, id event)
{
    char a[400], b[100];
    double timeout = -1;
    id object = nil;

    if (event && class_respondsToSelector(object_getClass(event), sel_registerName("attentionLostTimeout")))
        timeout = ((double (*)(id, SEL))objc_msgSend)(event, sel_registerName("attentionLostTimeout"));
    if (event && class_respondsToSelector(object_getClass(event), sel_registerName("associatedObject")))
        object = send(event, "associatedObject");
    out("sb26 idletimer: %s handles %s; timeout %.17g, associatedObject %s\n", described(self, b, sizeof(b)),
        described(event, a, sizeof(a)), timeout, object ? described(object, b, sizeof(b)) : "(nil)");
    idle_event_original(self, cmd, event);
}

static BOOL (*aw_resume_original)(id, SEL, id *);
static BOOL (*aw_suspend_original)(id, SEL, id *);
static BOOL (*aw_configure_original)(id, SEL, id, BOOL, id *);

static const char *
aw_identifier(id client, char *text, size_t size)
{
    id config = client ? send(client, "configuration") : nil;
    id ident = config ? send(config, "identifier") : nil;
    return described(ident, text, size);
}

static void
aw_dump_ivars(id client, const char *when)
{
    unsigned n = 0;
    Ivar *list = class_copyIvarList(object_getClass(client), &n);
    char line[1200];
    size_t at = (size_t)snprintf(line, sizeof(line), "sb26 idletimer: AW ivars %s:", when);
    for (unsigned i = 0; i < n && at < sizeof(line) - 80; i++) {
        const char *type = ivar_getTypeEncoding(list[i]);
        ptrdiff_t off = ivar_getOffset(list[i]);
        long long v = 0;
        if (type && (type[0] == 'i' || type[0] == 'I'))
            v = *(int *)((char *)client + off);
        else if (type && (type[0] == 'B' || type[0] == 'c' || type[0] == 'C'))
            v = *(unsigned char *)((char *)client + off);
        else if (type && (type[0] == 'q' || type[0] == 'Q' || type[0] == '@' || type[0] == '^'))
            v = *(long long *)((char *)client + off);
        at += (size_t)snprintf(line + at, sizeof(line) - at, " %s(%s)=%#llx", ivar_getName(list[i]), type ? type : "?", v);
    }
    free(list);
    out("%s\n", line);
}

static BOOL
aw_resume(id self, SEL cmd, id *error)
{
    char a[120], b[200];
    BOOL ok = aw_resume_original(self, cmd, error);
    out("sb26 idletimer: AW resume %s -> %d %s\n", aw_identifier(self, a, sizeof(a)), ok,
        error && *error ? described(*error, b, sizeof(b)) : "");
    if (strstr(a, "GlobalBacklight"))
        aw_dump_ivars(self, "after resume");
    return ok;
}

static BOOL
aw_suspend(id self, SEL cmd, id *error)
{
    char a[120], b[200];
    BOOL ok = aw_suspend_original(self, cmd, error);
    out("sb26 idletimer: AW suspend %s -> %d %s\n", aw_identifier(self, a, sizeof(a)), ok,
        error && *error ? described(*error, b, sizeof(b)) : "");
    return ok;
}

static BOOL
aw_configure(id self, SEL cmd, id config, BOOL reset, id *error)
{
    char a[120], b[200];
    id ident0 = config ? send(config, "identifier") : nil;
    out("sb26 idletimer: AW configure begin %s reset %d\n", described(ident0, a, sizeof(a)), reset);
    BOOL ok = aw_configure_original(self, cmd, config, reset, error);
    id ident = config ? send(config, "identifier") : nil;
    out("sb26 idletimer: AW configure %s reset %d -> %d %s\n", described(ident, a, sizeof(a)), reset, ok,
        error && *error ? described(*error, b, sizeof(b)) : "");
    return ok;
}

static void (*aw_configure2_original)(id, SEL, id, BOOL);
static void (*aw_configure1_original)(id, SEL, id);

static void
aw_configure2(id self, SEL cmd, id config, BOOL reset)
{
    char a[120], b[400];
    id ident = config ? send(config, "identifier") : nil;
    id timeouts = config ? send(config, "attentionLostTimeoutDictionary") : nil;
    out("sb26 idletimer: AW configure2 %s reset %d timeouts %s\n", described(ident, a, sizeof(a)), reset,
        described(timeouts, b, sizeof(b)));
    aw_configure2_original(self, cmd, config, reset);
    if (strstr(a, "GlobalBacklight"))
        aw_dump_ivars(self, "after configure2");
}

static void
aw_configure1(id self, SEL cmd, id config)
{
    char a[120];
    id ident = config ? send(config, "identifier") : nil;
    out("sb26 idletimer: AW configure1 %s\n", described(ident, a, sizeof(a)));
    aw_configure1_original(self, cmd, config);
}

static void (*idle_reset_original)(id, SEL, id);
static void (*idle_update_original)(id, SEL, id);
static unsigned idle_reset_seen, idle_update_seen;

static const char *
caller_name(void *caller, char *text, size_t size)
{
    Dl_info info;
    size_t length = 0;
    extern const void *_dyld_get_shared_cache_range(size_t *);
    uintptr_t slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000ul;
    if (dladdr(caller, &info) && info.dli_sname)
        snprintf(text, size, "%s+%#lx (%#lx)", info.dli_sname, (unsigned long)((char *)caller - (char *)info.dli_saddr),
                 (unsigned long)((uintptr_t)caller - slide));
    else
        snprintf(text, size, "%#lx", (unsigned long)((uintptr_t)caller - slide));
    return text;
}

static void
idle_reset(id self, SEL cmd, id reason)
{
    char a[160], b[160];
    if (idle_reset_seen++ < 30)
        out("sb26 idletimer: reset for %s from %s\n", described(reason, a, sizeof(a)),
            caller_name(__builtin_return_address(0), b, sizeof(b)));
    idle_reset_original(self, cmd, reason);
}

static void
idle_update(id self, SEL cmd, id reason)
{
    char a[160], b[160];
    if (idle_update_seen++ < 60)
        out("sb26 idletimer: update for %s from %s\n", described(reason, a, sizeof(a)),
            caller_name(__builtin_return_address(0), b, sizeof(b)));
    idle_update_original(self, cmd, reason);
}

static IMP telephony_update_original;
static unsigned telephony_update_seen;

static void
telephony_update(id self, SEL cmd)
{
    char a[160];
    if (telephony_update_seen++ < 12)
        out("sb26 idletimer: -[SBTelephonyManager %s] from %s\n", sel_getName(cmd),
            caller_name(__builtin_return_address(0), a, sizeof(a)));
    ((void (*)(id, SEL))telephony_update_original)(self, cmd);
}

static IMP telephony_calls_original;
static unsigned telephony_calls_seen;

static void
telephony_calls(id self, SEL cmd)
{
    char a[160], b[160];
    if (telephony_calls_seen++ < 12)
    {
        /* two frames up, through the frame pointer chain (x29) */
        void **fp = (void **)__builtin_frame_address(0);
        void **up = fp ? (void **)fp[0] : NULL;
        out("sb26 idletimer: -[SBTelephonyManager updateCalls] from %s, %s\n",
            caller_name(__builtin_return_address(0), a, sizeof(a)),
            caller_name(up ? __builtin_extract_return_addr(up[1]) : NULL, b, sizeof(b)));
    }
    ((void (*)(id, SEL))telephony_calls_original)(self, cmd);
}

static IMP telephony_status_original;
static unsigned telephony_status_seen;

static void
telephony_status(id self, SEL cmd, id argument)
{
    char a[160], b[300];
    if (telephony_status_seen++ < 12)
        out("sb26 idletimer: -[SBTelephonyManager %s] %s from %s\n", sel_getName(cmd), described(argument, b, sizeof(b)),
            caller_name(__builtin_return_address(0), a, sizeof(a)));
    ((void (*)(id, SEL, id))telephony_status_original)(self, cmd, argument);
}

/* SBTelephonyManager's methods at their shared cache addresses (dsc.py), and a hook on the one at
 * 0x22438214c: the "TelephonyManager:UpdateSB" idle timer reset */
static void
install_telephony_log(void)
{
    Class t = objc_getClass("SBTelephonyManager");
    Dl_info info;
    if (!t || !dladdr((void *)t, &info)) return;
    size_t length = 0;
    extern const void *_dyld_get_shared_cache_range(size_t *);
    uintptr_t slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000ul;
    unsigned n = 0;
    Method *list = class_copyMethodList(t, &n);
    for (unsigned i = 0; i < n; i++) {
        uintptr_t at = (uintptr_t)method_getImplementation(list[i]) - slide;
        if (at >= 0x22445c700ul && at <= 0x22445c9d4ul)
            out("sb26 idletimer: SBTelephonyManager %s at %#lx (args %u) contains the caller\n",
                sel_getName(method_getName(list[i])), (unsigned long)at, method_getNumberOfArguments(list[i]));
        if (at >= 0x22445c700ul && at <= 0x22445c9d4ul && method_getNumberOfArguments(list[i]) == 3) {
            telephony_status_original = method_setImplementation(list[i], (IMP)telephony_status);
        }
        if (at == 0x22445c770ul && method_getNumberOfArguments(list[i]) == 2) {
            telephony_calls_original = method_setImplementation(list[i], (IMP)telephony_calls);
            out("sb26 idletimer: SBTelephonyManager updateCalls hooked\n");
        }
        if (at == 0x22438214cul && method_getNumberOfArguments(list[i]) == 2) {
            telephony_update_original = method_setImplementation(list[i], (IMP)telephony_update);
            out("sb26 idletimer: SBTelephonyManager %s is the UpdateSB reset; hooked\n", sel_getName(method_getName(list[i])));
        }
    }
    free(list);
}

static void
install_idle_log(void)
{
    Class coordinator = objc_getClass("SBIdleTimerGlobalCoordinator");
    Method m;

    if (!sb26_boot_arg_present("idlelog=1") || !coordinator) return;
    if ((m = class_getInstanceMethod(coordinator, sel_registerName("_setIdleTimerWithDescriptor:forReason:"))))
        idle_set_original = (void (*)(id, SEL, id, id))method_setImplementation(m, (IMP)idle_set);
    if ((m = class_getInstanceMethod(coordinator, sel_registerName("idleTimerProvider:didProposeBehavior:forReason:"))))
        idle_propose_original = (void (*)(id, SEL, id, id, id))method_setImplementation(m, (IMP)idle_propose);
    if ((m = class_getInstanceMethod(coordinator, sel_registerName("acquireIdleTimerDisableAssertionForReason:"))))
        idle_disable_original = (id (*)(id, SEL, id))method_setImplementation(m, (IMP)idle_disable);
    if ((m = class_getInstanceMethod(coordinator, sel_registerName("resetIdleTimerForReason:"))))
        idle_reset_original = (void (*)(id, SEL, id))method_setImplementation(m, (IMP)idle_reset);
    if ((m = class_getInstanceMethod(coordinator, sel_registerName("_updateIdleTimerForReason:"))))
        idle_update_original = (void (*)(id, SEL, id))method_setImplementation(m, (IMP)idle_update);
    install_telephony_log();
    Class attention = objc_getClass("ITAttentionAwarenessClient");
    if (attention && (m = class_getInstanceMethod(attention, sel_registerName("_handleAttentionAwarenessEvent:"))))
        idle_event_original = (void (*)(id, SEL, id))method_setImplementation(m, (IMP)idle_event);
    Class aw = objc_getClass("AWAttentionAwarenessClient");
    if (aw && (m = class_getInstanceMethod(aw, sel_registerName("resumeWithError:"))))
        aw_resume_original = (BOOL (*)(id, SEL, id *))method_setImplementation(m, (IMP)aw_resume);
    if (aw && (m = class_getInstanceMethod(aw, sel_registerName("suspendWithError:"))))
        aw_suspend_original = (BOOL (*)(id, SEL, id *))method_setImplementation(m, (IMP)aw_suspend);
    if (aw && (m = class_getInstanceMethod(aw, sel_registerName("setConfiguration:shouldReset:error:"))))
        aw_configure_original = (BOOL (*)(id, SEL, id, BOOL, id *))method_setImplementation(m, (IMP)aw_configure);
    if (aw && (m = class_getInstanceMethod(aw, sel_registerName("setConfiguration:shouldReset:"))))
        aw_configure2_original = (void (*)(id, SEL, id, BOOL))method_setImplementation(m, (IMP)aw_configure2);
    if (aw && (m = class_getInstanceMethod(aw, sel_registerName("setConfiguration:"))))
        aw_configure1_original = (void (*)(id, SEL, id))method_setImplementation(m, (IMP)aw_configure1);
    out("sb26 idletimer: attention events %s\n", idle_event_original ? "logged" : "not found");
    out("sb26 idletimer: logged (apply %d, propose %d, disable %d)\n", idle_set_original != NULL,
        idle_propose_original != NULL, idle_disable_original != NULL);
}

void
sb26_install_layer_dump(void)
{
    install_idle_log();
    static const char *filters[] = { "all", "Icon", "Wallpaper", "Poster", "StatusBar", "Battery", "Widget",
                                     "DateView", "Time", "SDF", "animated" };
    for (size_t i = 0; i < sizeof(filters) / sizeof(filters[0]); i++) {
        char name[64];
        int token;
        const char *filter = i ? filters[i] : NULL;
        snprintf(name, sizeof(name), "com.annibale.layers.%s", filters[i]);
        notify_register_dispatch(name, &token, dispatch_get_main_queue(), ^(int t) {
            static dispatch_once_t once;
            (void)t;
            dispatch_once_f(&once, NULL, resolve);
            dump(filter, 0);
        });
    }
    int token;
    notify_register_dispatch("com.annibale.layers.full", &token, dispatch_get_main_queue(), ^(int t) {
        static dispatch_once_t once;
        (void)t;
        dispatch_once_f(&once, NULL, resolve);
        dump(NULL, 1);
    });
    notify_register_dispatch("com.annibale.layers.query", &token, dispatch_get_main_queue(), ^(int t) {
        static dispatch_once_t once;
        (void)t;
        dispatch_once_f(&once, NULL, resolve);
        dump_from_query_file();
    });
    notify_register_dispatch("com.annibale.battery.nofx", &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        clear_battery_filters();
    });
    notify_register_dispatch("com.annibale.shapes.test", &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        add_test_layers();
    });
}

/* Remote scene hosts (wallpaper, widgets, share sheet) whose layer trees live outside SpringBoard. */
__attribute__((constructor)) static void
install_poster_dump(void)
{
    const char *prog = getprogname();
    if (!prog || (strcmp(prog, "MercuryPosterExtension") &&
        strcmp(prog, "WidgetRenderer_Default") &&
        strcmp(prog, "SharingUIService") &&
        strcmp(prog, "Weather"))) return;
    to_say = 1;
    sb26_install_layer_dump();
    out("sb26 layers: dump installed in %s\n", prog);
    if (getenv("ANNIBALE_POSTER_AUTO_DUMP") != NULL) {
        for (int64_t seconds = 40; seconds <= 160; seconds += 120) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, seconds * (int64_t)NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                static dispatch_once_t once;
                dispatch_once_f(&once, NULL, resolve);
                dump(NULL, 0);
            });
        }
    }
}
