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

static BOOL
sb26_airdrop_is_available(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return NO;
}

static void
sb26_airdrop_set_discoverable_mode(id self, SEL cmd, long long mode)
{
    (void)self;
    (void)cmd;
    (void)mode;
}

static long long
sb26_airdrop_discoverable_mode(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return 0;
}

static BOOL
sb26_airdrop_is_action_disabled(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return YES;
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
static id (*orig_mru_available_devices)(id self, SEL cmd);
static id (*orig_av_available_devices)(id self, SEL cmd);
static void (*orig_mru_start_discovery)(id self, SEL cmd);

static void
sb26_update_cached_devices(void)
{
    Class arr_cls = objc_getClass("NSArray");
    if (!arr_cls) return;
    if (cached_output_devices == NULL) {
        id empty = ((id (*)(Class, SEL))objc_msgSend)(arr_cls, sel_registerName("array"));
        if (empty) {
            cached_output_devices = ((id (*)(id, SEL))objc_msgSend)(empty, sel_registerName("retain"));
        }
    }
}

static id
sb26_output_available_devices(id self, SEL cmd)
{
    /*
     * When called on the main thread, immediately return the cached device list
     * (or empty array) without blocking for 10 seconds in mediaserverd Bonjour/AWDL scans.
     */
    Class thread_cls = objc_getClass("NSThread");
    BOOL on_main = thread_cls ? ((BOOL (*)(Class, SEL))objc_msgSend)(thread_cls, sel_registerName("isMainThread")) : YES;

    if (on_main) {
        if (cached_output_devices) {
            return cached_output_devices;
        }
        Class arr_cls = objc_getClass("NSArray");
        if (arr_cls) {
            return ((id (*)(Class, SEL))objc_msgSend)(arr_cls, sel_registerName("array"));
        }
        return NULL;
    }

    /* Background thread: query real devices if underlying implementation exists */
    if (orig_mru_available_devices) {
        id devs = orig_mru_available_devices(self, cmd);
        if (devs) {
            if (cached_output_devices) {
                ((void (*)(id, SEL))objc_msgSend)(cached_output_devices, sel_registerName("release"));
            }
            cached_output_devices = ((id (*)(id, SEL))objc_msgSend)(devs, sel_registerName("retain"));
            return devs;
        }
    }
    if (orig_av_available_devices) {
        id devs = orig_av_available_devices(self, cmd);
        if (devs) {
            if (cached_output_devices) {
                ((void (*)(id, SEL))objc_msgSend)(cached_output_devices, sel_registerName("release"));
            }
            cached_output_devices = ((id (*)(id, SEL))objc_msgSend)(devs, sel_registerName("retain"));
            return devs;
        }
    }

    if (cached_output_devices) {
        return cached_output_devices;
    }
    Class arr_cls = objc_getClass("NSArray");
    return arr_cls ? ((id (*)(Class, SEL))objc_msgSend)(arr_cls, sel_registerName("array")) : NULL;
}

static void
sb26_mru_start_detailed_discovery(id self, SEL cmd)
{
    /*
     * Run discovery trigger asynchronously in a global background queue
     * so that SpringBoard main thread is never stalled.
     */
    if (self) {
        ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("retain"));
    }
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        if (orig_mru_start_discovery) {
            orig_mru_start_discovery(self, cmd);
        }
        sb26_update_cached_devices();
        if (self) {
            ((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("release"));
        }
    });
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
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0), ^{
        sb26_update_cached_devices();
    });
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

    /* Register notifications to drive background route updates via CFNotificationCenter */
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

static id (*orig_calendar_layer)(id self, SEL cmd, void *info, id tc, unsigned long opts);

static id
sb26_calendar_icon_layer_fallback(id self, SEL _cmd, void *info, id tc, unsigned long opts)
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
        id (*img_fn)(id, SEL, void *, id, unsigned long) = (id (*)(id, SEL, void *, id, unsigned long))method_getImplementation(m_img);
        id image = img_fn(self, img_sel, info, tc, opts);
        if (image != NULL) {
            SEL cg_sel = sel_registerName("CGImage");
            Class img_cls = object_getClass(image);
            if (img_cls && class_getInstanceMethod(img_cls, cg_sel)) {
                id cg_img = ((id (*)(id, SEL))objc_msgSend)(image, cg_sel);
                if (cg_img != NULL) {
                    Class layer_cls = objc_getClass("CALayer");
                    if (layer_cls) {
                        id new_layer = ((id (*)(Class, SEL))objc_msgSend)(layer_cls, sel_registerName("layer"));
                        if (new_layer != NULL) {
                            ((void (*)(id, SEL, id))objc_msgSend)(new_layer, sel_registerName("setContents:"), cg_img);
                            return new_layer;
                        }
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
        orig_calendar_layer = (id (*)(id, SEL, void *, id, unsigned long))method_getImplementation(m_layer);
        method_setImplementation(m_layer, (IMP)sb26_calendar_icon_layer_fallback);
        cal_done = 1;
        fprintf(stderr, "sb26: SBCalendarIconImageProvider fallback hook installed\n");
    }
}
