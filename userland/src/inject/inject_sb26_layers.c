/*
 * SpringBoard's layer tree on request, for telling an empty layer from one the compositor did not
 * draw: a missing icon is either a layer with no contents (nothing was rendered into it) or one
 * whose contents are there and did not reach the screen.
 *
 * `notify com.redwood.layers.<filter>` prints, to stderr and so to the console, every layer whose
 * own class or whose view's class contains <filter> (case sensitive), at any depth, with the layers
 * below it up to LAYER_BELOW levels. `com.redwood.layers.all` prints the whole tree.
 * `com.redwood.layers.full` prints the full subtree without truncation and exports layer visual
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
    LAYER_BELOW = 3,
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
static int to_say;      /* a process whose stderr goes nowhere: lines go to kern.redwood.say */
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
walk(id layer, unsigned depth, const char *filter, int below, int full)
{
    unsigned max_lines = full ? LAYER_LINES_FULL : LAYER_LINES;
    if (!layer || lines >= max_lines || depth > LAYER_DEPTH) return;
    if (below < 0 && matches(layer, filter)) below = LAYER_BELOW;
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
    }
    id sublayers = send(layer, "sublayers");
    unsigned long count = sublayers ? ((unsigned long (*)(id, SEL))objc_msgSend)(sublayers,
        sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < count && lines < max_lines; i++) {
        id child = ((id (*)(id, SEL, unsigned long))objc_msgSend)(sublayers,
            sel_registerName("objectAtIndex:"), i);
        walk(child, depth + 1, filter, full ? (1 << 30) : (below > 0 ? below - 1 : -1), full);
    }
}

static void
dump(const char *filter, int full)
{
    FILE *fp = NULL;
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
    out("sb26 layers begin filter=%s full=%d windows=%lu\n", filter ? filter : (full ? "full" : "all"), full, count);
    for (unsigned long i = 0; i < count && lines < max_lines; i++) {
        id window = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows,
            sel_registerName("objectAtIndex:"), i);
        SEL context = sel_registerName("_contextId");
        unsigned id_of_context = class_respondsToSelector(object_getClass(window), context)
            ? ((unsigned (*)(id, SEL))objc_msgSend)(window, context) : 0;
        out("sb26 layer window %lu %s context %x\n", i, name_of(window), id_of_context);
        walk(send(window, "layer"), 0, filter, filter ? -1 : 1 << 30, full);
    }
    out("sb26 layers end lines=%u%s\n", lines, lines >= max_lines ? " (cut)" : "");
    if (dump_fp) {
        fclose(dump_fp);
        dump_fp = NULL;
    }
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

void
sb26_install_layer_dump(void)
{
    static const char *filters[] = { "all", "Icon", "Wallpaper", "Poster", "StatusBar", "Battery", "Widget",
                                     "DateView", "Time", "SDF" };
    for (size_t i = 0; i < sizeof(filters) / sizeof(filters[0]); i++) {
        char name[64];
        int token;
        const char *filter = i ? filters[i] : NULL;
        snprintf(name, sizeof(name), "com.redwood.layers.%s", filters[i]);
        notify_register_dispatch(name, &token, dispatch_get_main_queue(), ^(int t) {
            static dispatch_once_t once;
            (void)t;
            dispatch_once_f(&once, NULL, resolve);
            dump(filter, 0);
        });
    }
    int token;
    notify_register_dispatch("com.redwood.layers.full", &token, dispatch_get_main_queue(), ^(int t) {
        static dispatch_once_t once;
        (void)t;
        dispatch_once_f(&once, NULL, resolve);
        dump(NULL, 1);
    });
    notify_register_dispatch("com.redwood.battery.nofx", &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        clear_battery_filters();
    });
    notify_register_dispatch("com.redwood.shapes.test", &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        add_test_layers();
    });
}

/* MercuryPosterExtension draws the wallpaper SpringBoard hosts (PBUIPosterSceneLayerHostView). */
__attribute__((constructor)) static void
install_poster_dump(void)
{
    if (!getprogname() || strcmp(getprogname(), "MercuryPosterExtension")) return;
    to_say = 1;
    sb26_install_layer_dump();
    out("sb26 layers: dump installed in %s\n", getprogname());
    for (int64_t seconds = 40; seconds <= 160; seconds += 120) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, seconds * (int64_t)NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            static dispatch_once_t once;
            dispatch_once_f(&once, NULL, resolve);
            dump(NULL, 0);
        });
    }
}
