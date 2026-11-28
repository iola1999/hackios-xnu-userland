/* Extra per-layer diagnostics for SDF and gradient layers, included by inject_sb26_layers.c. */

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

static void
describe_gradient(id layer, unsigned depth)
{
    Class gcls = objc_getClass("CAGradientLayer");
    Class cls = object_getClass(layer), sup = class_getSuperclass(cls);
    BOOL match = strstr(name_of(layer), "GradientLayer") != NULL;
    for (Class c = cls; c && !match; c = class_getSuperclass(c))
        match = (c == gcls);
    if (!match) return;
    id type = send(layer, "type"), colors = send(layer, "colors"), locs = send(layer, "locations");
    const char *ts = (type && CFGetTypeID((CFTypeRef)type) == CFStringGetTypeID())
        ? ((const char *(*)(id, SEL))objc_msgSend)(type, sel_registerName("UTF8String")) : "-";
    struct point sp = { NAN, NAN }, ep = { NAN, NAN };
    SEL sp_s = sel_registerName("startPoint"), ep_s = sel_registerName("endPoint");
    if (class_respondsToSelector(cls, sp_s))
        sp = ((struct point (*)(id, SEL))objc_msgSend)(layer, sp_s);
    if (class_respondsToSelector(cls, ep_s))
        ep = ((struct point (*)(id, SEL))objc_msgSend)(layer, ep_s);
    SEL cnt = sel_registerName("count"), idx = sel_registerName("objectAtIndex:");
    unsigned long nc = colors ? ((unsigned long (*)(id, SEL))objc_msgSend)(colors, cnt) : 0;
    unsigned long nl = locs ? ((unsigned long (*)(id, SEL))objc_msgSend)(locs, cnt) : 0;
    char cb[160] = "", lb[96] = "", one[24];
    for (unsigned long i = 0, a = 0; i < nc && i < 6 && a < sizeof(cb); i++) {
        describe_color(((id (*)(id, SEL, unsigned long))objc_msgSend)(colors, idx, i), one, sizeof(one));
        a += (size_t)snprintf(cb + a, sizeof(cb) - a, "%s%s", i ? "," : "", one);
    }
    for (unsigned long i = 0, a = 0; i < nl && i < 6 && a < sizeof(lb); i++) {
        id n = ((id (*)(id, SEL, unsigned long))objc_msgSend)(locs, idx, i);
        double v = n ? ((double (*)(id, SEL))objc_msgSend)(n, sel_registerName("doubleValue")) : NAN;
        a += (size_t)snprintf(lb + a, sizeof(lb) - a, "%s%.3g", i ? "," : "", v);
    }
    out("sb26 layer %2u gradient super=%s type=%s start=%.2f,%.2f end=%.2f,%.2f colors=%lu[%s] locs=%lu[%s]\n",
        depth, sup ? class_getName(sup) : "-", ts ? ts : "-", sp.x, sp.y, ep.x, ep.y, nc, cb, nl, lb);
    lines++;
}
