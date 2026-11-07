/*
 * inject_sb26_airdrop.inc: AirDrop mutex hang elimination, Screen Mirroring
 * asynchronous route discovery, and Calendar dynamic icon fallback.
 * Pure C99 implementation using ObjC runtime primitives.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <net/if.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>

/* --- AirDrop Hook Implementation --- */

static id (*orig_airdrop_init)(id self, SEL cmd);

static id
sb26_airdrop_init(id self, SEL cmd)
{
    /*
     * If the Apple Wireless Direct Link (awdl0) network interface is missing,
     * sharingd enters a deadlock/spin loop waiting for awdl0, causing
     * SFOperationCreate to block the SpringBoard main runloop on mutex [x19, #0x40].
     * Intercept -init to return an offline singleton without invoking SFOperationCreate.
     */
    if (if_nametoindex("awdl0") == 0) {
        static int logged_offline = 0;
        if (!logged_offline) {
            fprintf(stderr, "sb26: awdl0 absent, returning offline SFAirDropDiscoveryController\n");
            logged_offline = 1;
        }
        Class cls = object_getClass(self);
        Class super_cls = cls ? class_getSuperclass(cls) : NULL;
        if (super_cls) {
            struct objc_super sup;
            sup.receiver = self;
            sup.super_class = super_cls;
            id ret = ((id (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, cmd);
            return ret ? ret : self;
        }
        return self;
    }
    if (orig_airdrop_init) {
        return orig_airdrop_init(self, cmd);
    }
    return self;
}

static const char *const kAirDropModeFile = "/private/var/mobile/Library/Preferences/com.redwood.airdrop.mode";
static long long g_airdrop_mode = -1;

static long long
sb26_airdrop_load_mode(void)
{
    if (g_airdrop_mode >= 0 && g_airdrop_mode <= 2) {
        return g_airdrop_mode;
    }
    FILE *f = fopen(kAirDropModeFile, "r");
    if (f) {
        long long v = 2;
        if (fscanf(f, "%lld", &v) == 1 && v >= 0 && v <= 2) {
            g_airdrop_mode = v;
        }
        fclose(f);
    }
    if (g_airdrop_mode < 0 || g_airdrop_mode > 2) {
        g_airdrop_mode = 2;
    }
    return g_airdrop_mode;
}

static void
sb26_airdrop_save_mode(long long mode)
{
    if (mode < 0 || mode > 2) mode = 2;
    g_airdrop_mode = mode;
    FILE *f = fopen(kAirDropModeFile, "w");
    if (f) {
        fprintf(f, "%lld\n", mode);
        fclose(f);
    }
}

static BOOL
sb26_airdrop_is_available(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return YES;
}

static void
sb26_airdrop_set_discoverable_mode(id self, SEL cmd, long long mode)
{
    (void)cmd;
    sb26_airdrop_save_mode(mode);
    SEL sel_del = sel_registerName("delegate");
    if (self && class_respondsToSelector(object_getClass(self), sel_del)) {
        id del = ((id (*)(id, SEL))objc_msgSend)(self, sel_del);
        SEL sel_chg = sel_registerName("discoveryControllerSettingsDidChange:");
        if (del && class_respondsToSelector(object_getClass(del), sel_chg)) {
            ((void (*)(id, SEL, id))objc_msgSend)(del, sel_chg, self);
        }
        SEL sel_vis = sel_registerName("discoveryControllerVisibilityDidChange:");
        if (del && class_respondsToSelector(object_getClass(del), sel_vis)) {
            ((void (*)(id, SEL, id))objc_msgSend)(del, sel_vis, self);
        }
    }
}

static long long
sb26_airdrop_discoverable_mode(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return sb26_airdrop_load_mode();
}

static BOOL
sb26_airdrop_is_action_disabled(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return NO;
}

static void
install_airdrop_hooks(Class cls)
{
    static int airdrop_done;
    if (airdrop_done || !cls) return;

    Method m_init = class_getInstanceMethod(cls, sel_registerName("init"));
    if (m_init) {
        orig_airdrop_init = (id (*)(id, SEL))method_getImplementation(m_init);
        method_setImplementation(m_init, (IMP)sb26_airdrop_init);
    }
    Method m_avail = class_getInstanceMethod(cls, sel_registerName("isAirDropAvailable"));
    if (m_avail) {
        method_setImplementation(m_avail, (IMP)sb26_airdrop_is_available);
    }
    Method m_set_mode = class_getInstanceMethod(cls, sel_registerName("setDiscoverableMode:"));
    if (m_set_mode) {
        method_setImplementation(m_set_mode, (IMP)sb26_airdrop_set_discoverable_mode);
    }
    Method m_get_mode = class_getInstanceMethod(cls, sel_registerName("discoverableMode"));
    if (m_get_mode) {
        method_setImplementation(m_get_mode, (IMP)sb26_airdrop_discoverable_mode);
    }
    Method m_disabled = class_getInstanceMethod(cls, sel_registerName("isActionDisabled"));
    if (m_disabled) {
        method_setImplementation(m_disabled, (IMP)sb26_airdrop_is_action_disabled);
    }

    airdrop_done = 1;
    fprintf(stderr, "sb26: SFAirDropDiscoveryController offline interception installed\n");
}

/* --- Screen Mirroring Asynchronous Discovery Implementation --- */

static id cached_output_devices = NULL;
static dispatch_once_t cached_dev_once;
static id (*orig_mru_available_devices)(id self, SEL cmd);
static id (*orig_av_available_devices)(id self, SEL cmd);
static void (*orig_mru_start_discovery)(id self, SEL cmd);

static void
sb26_update_cached_devices(void)
{
    dispatch_once(&cached_dev_once, ^{
        Class arr_cls = objc_getClass("NSArray");
        if (arr_cls) {
            id empty = ((id (*)(Class, SEL))objc_msgSend)(arr_cls, sel_registerName("array"));
            if (empty) {
                cached_output_devices = ((id (*)(id, SEL))objc_msgSend)(empty, sel_registerName("retain"));
            }
        }
    });
}

static id
sb26_output_available_devices(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    sb26_update_cached_devices();
    return cached_output_devices;
}

static void
sb26_mru_start_detailed_discovery(id self, SEL cmd)
{
    /*
     * MediaRemoteUI / MRUMirroringController must execute on the main thread
     * so that its delegate callbacks (-[MRUMirroringViewController mirroringController:...])
     * remain on the main thread, avoiding UIKit multithreading assertions.
     */
    Class thread_cls = objc_getClass("NSThread");
    BOOL on_main = thread_cls ? ((BOOL (*)(Class, SEL))objc_msgSend)(thread_cls, sel_registerName("isMainThread")) : YES;

    if (!on_main) {
        if (self) {
            ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("retain"));
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (orig_mru_start_discovery) {
                orig_mru_start_discovery(self, cmd);
            }
            sb26_update_cached_devices();
            if (self) {
                ((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("release"));
            }
        });
        return;
    }

    if (orig_mru_start_discovery) {
        orig_mru_start_discovery(self, cmd);
    }
    sb26_update_cached_devices();
}

static void
sb26_output_set_discovery_mode(id self, SEL cmd, long long mode)
{
    (void)self;
    (void)cmd;
    (void)mode;
}

static long long
sb26_output_discovery_mode(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return 0;
}

static BOOL
sb26_output_device_presence_detected(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return NO;
}

static void
sb26_routes_changed_cf_callback(CFNotificationCenterRef center, void *observer,
                                CFStringRef name, const void *object,
                                CFDictionaryRef userInfo)
{
    (void)center;
    (void)observer;
    (void)name;
    (void)object;
    (void)userInfo;
    sb26_update_cached_devices();
}

static void
install_screen_mirroring_hooks(Class cls)
{
    static int mirroring_done;
    if (mirroring_done || !cls) return;

    Method m_devs = class_getInstanceMethod(cls, sel_registerName("availableOutputDevices"));
    if (m_devs) {
        orig_av_available_devices = (id (*)(id, SEL))method_getImplementation(m_devs);
        method_setImplementation(m_devs, (IMP)sb26_output_available_devices);
    }
    Method m_set_mode = class_getInstanceMethod(cls, sel_registerName("setDiscoveryMode:"));
    if (m_set_mode) {
        method_setImplementation(m_set_mode, (IMP)sb26_output_set_discovery_mode);
    }
    Method m_get_mode = class_getInstanceMethod(cls, sel_registerName("discoveryMode"));
    if (m_get_mode) {
        method_setImplementation(m_get_mode, (IMP)sb26_output_discovery_mode);
    }
    Method m_presence = class_getInstanceMethod(cls, sel_registerName("devicePresenceDetected"));
    if (m_presence) {
        method_setImplementation(m_presence, (IMP)sb26_output_device_presence_detected);
    }

    /* Register notifications to drive route updates via CFNotificationCenter */
    CFNotificationCenterRef local = CFNotificationCenterGetLocalCenter();
    if (local) {
        CFNotificationCenterAddObserver(local, NULL, sb26_routes_changed_cf_callback,
                                        CFSTR("AVOutputDeviceDiscoverySessionAvailableOutputDevicesDidChangeNotification"),
                                        NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
        CFNotificationCenterAddObserver(local, NULL, sb26_routes_changed_cf_callback,
                                        CFSTR("AvailableRoutesChangedNotification"),
                                        NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    }

    mirroring_done = 1;
    fprintf(stderr, "sb26: AVOutputDeviceDiscoverySession async interception installed\n");
}

static void
install_mru_mirroring_hooks(Class cls)
{
    static int mru_done;
    if (mru_done || !cls) return;

    Method m_devs = class_getInstanceMethod(cls, sel_registerName("availableOutputDevices"));
    if (m_devs) {
        orig_mru_available_devices = (id (*)(id, SEL))method_getImplementation(m_devs);
        method_setImplementation(m_devs, (IMP)sb26_output_available_devices);
    }
    Method m_start = class_getInstanceMethod(cls, sel_registerName("startDetailedDiscovery"));
    if (m_start) {
        orig_mru_start_discovery = (void (*)(id, SEL))method_getImplementation(m_start);
        method_setImplementation(m_start, (IMP)sb26_mru_start_detailed_discovery);
    }

    mru_done = 1;
    fprintf(stderr, "sb26: MRUMirroringController async interception installed\n");
}

/* --- Calendar Dynamic Icon Fallback Implementation --- */

/*
 * SBIconImageInfo is an ARM64 Homogeneous Floating-point Aggregate (HFA) consisting
 * of 4 doubles: CGSize size (width, height), double scale, double continuousCornerRadius.
 * AAPCS64 passes it in d0..d3 floating-point registers. traitCollection is in x2 and options is in x3.
 */
struct SBIconImageInfo {
    CGSize size;
    double scale;
    double continuousCornerRadius;
};
typedef struct SBIconImageInfo SBIconImageInfo;

static id (*orig_calendar_layer)(id self, SEL cmd, SBIconImageInfo info, id tc, unsigned long opts);

static id
sb26_calendar_icon_layer_fallback(id self, SEL _cmd, SBIconImageInfo info, id tc, unsigned long opts)
{
    id layer = NULL;
    if (orig_calendar_layer) {
        layer = orig_calendar_layer(self, _cmd, info, tc, opts);
    }
    if (layer != NULL) {
        return layer;
    }

    /*
     * If native layer generation returns nil (due to missing ICRIconLayer in ISImage),
     * call iconImageWithInfo:traitCollection:options: and wrap the resulting CGImage in a CALayer.
     */
    SEL img_sel = sel_registerName("iconImageWithInfo:traitCollection:options:");
    Class self_cls = object_getClass(self);
    Method m_img = self_cls ? class_getInstanceMethod(self_cls, img_sel) : NULL;
    if (m_img) {
        id (*img_fn)(id, SEL, SBIconImageInfo, id, unsigned long) =
            (id (*)(id, SEL, SBIconImageInfo, id, unsigned long))method_getImplementation(m_img);
        id image = img_fn(self, img_sel, info, tc, opts);
        if (image != NULL) {
            SEL cg_sel = sel_registerName("CGImage");
            Class img_cls = object_getClass(image);
            if (img_cls && class_getInstanceMethod(img_cls, cg_sel)) {
                id cg_img = ((id (*)(id, SEL))objc_msgSend)(image, cg_sel);
                if (cg_img != NULL) {
                    Class layer_cls = objc_getClass("CALayer");
                    if (layer_cls) {
                        /*
                         * Wrap CALayer creation and property modification with an explicit
                         * CATransaction so CA_ASSERT_MAIN_THREAD_TRANSACTIONS assertion
                         * (which aborts on implicit transactions on background threads) is avoided.
                         */
                        Class tx_cls = objc_getClass("CATransaction");
                        if (tx_cls) {
                            ((void (*)(Class, SEL))objc_msgSend)(tx_cls, sel_registerName("begin"));
                            ((void (*)(Class, SEL, BOOL))objc_msgSend)(tx_cls, sel_registerName("setDisableActions:"), YES);
                        }
                        id new_layer = ((id (*)(Class, SEL))objc_msgSend)(layer_cls, sel_registerName("layer"));
                        if (new_layer != NULL) {
                            ((void (*)(id, SEL, id))objc_msgSend)(new_layer, sel_registerName("setContents:"), cg_img);
                        }
                        if (tx_cls) {
                            ((void (*)(Class, SEL))objc_msgSend)(tx_cls, sel_registerName("commit"));
                        }
                        return new_layer;
                    }
                }
            }
        }
    }

    return NULL;
}

static void
install_calendar_icon_hooks(Class cls)
{
    static int cal_done;
    if (cal_done || !cls) return;

    Method m_layer = class_getInstanceMethod(cls, sel_registerName("iconLayerWithInfo:traitCollection:options:"));
    if (m_layer) {
        orig_calendar_layer = (id (*)(id, SEL, SBIconImageInfo, id, unsigned long))method_getImplementation(m_layer);
        method_setImplementation(m_layer, (IMP)sb26_calendar_icon_layer_fallback);
        cal_done = 1;
        fprintf(stderr, "sb26: SBCalendarIconImageProvider fallback hook installed\n");
    }
}
