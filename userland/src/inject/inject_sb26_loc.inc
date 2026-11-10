/*
 * Localized bundle display name resolution from InfoPlist.loctable for iOS 26
 * (where system .app bundles carry InfoPlist.loctable without zh*.lproj folders),
 * plus clearing stale FailureThresholdMet flags in com.apple.wifi.plist.
 */

static CFMutableDictionaryRef sb26_loctable_name_cache;
static os_unfair_lock sb26_loctable_lock = OS_UNFAIR_LOCK_INIT;

static int
sb26_system_prefers_zh(void)
{
    static int cached = -1;
    if (cached != -1) return cached;

    int is_zh = 1;
    const char *pref_path = "/private/var/mobile/Library/Preferences/.GlobalPreferences.plist";
    int fd = open(pref_path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < 256 * 1024) {
            void *buf = malloc((size_t)st.st_size);
            if (buf) {
                if (read(fd, buf, (size_t)st.st_size) == st.st_size) {
                    CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
                        (const UInt8 *)buf, st.st_size, kCFAllocatorMalloc);
                    if (data) {
                        CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault,
                            data, kCFPropertyListImmutable, NULL, NULL);
                        CFRelease(data);
                        if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
                            CFArrayRef langs = (CFArrayRef)CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("AppleLanguages"));
                            if (langs && CFGetTypeID(langs) == CFArrayGetTypeID() && CFArrayGetCount(langs) > 0) {
                                CFTypeRef first = CFArrayGetValueAtIndex(langs, 0);
                                if (first && CFGetTypeID(first) == CFStringGetTypeID()) {
                                    is_zh = CFStringHasPrefix((CFStringRef)first, CFSTR("zh")) ? 1 : 0;
                                }
                            }
                        }
                        if (plist) CFRelease(plist);
                    }
                } else {
                    free(buf);
                }
            }
        }
        close(fd);
    }
    cached = is_zh;
    return is_zh;
}

static CFStringRef
sb26_loctable_display_name_for_path(const char *bundle_path)
{
    if (!bundle_path || !bundle_path[0] || !sb26_system_prefers_zh()) return NULL;

    CFStringRef path_key = CFStringCreateWithCString(kCFAllocatorDefault, bundle_path, kCFStringEncodingUTF8);
    if (!path_key) return NULL;

    os_unfair_lock_lock(&sb26_loctable_lock);
    if (!sb26_loctable_name_cache) {
        sb26_loctable_name_cache = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    CFTypeRef existing = sb26_loctable_name_cache ? CFDictionaryGetValue(sb26_loctable_name_cache, path_key) : NULL;
    if (existing) {
        os_unfair_lock_unlock(&sb26_loctable_lock);
        CFRelease(path_key);
        return existing == (CFTypeRef)kCFNull ? NULL : (CFStringRef)existing;
    }
    os_unfair_lock_unlock(&sb26_loctable_lock);

    char loc_path[PATH_MAX];
    snprintf(loc_path, sizeof(loc_path), "%s/InfoPlist.loctable", bundle_path);
    int fd = open(loc_path, O_RDONLY);
    CFStringRef resolved = NULL;
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < 1024 * 1024) {
            void *buf = malloc((size_t)st.st_size);
            if (buf) {
                if (read(fd, buf, (size_t)st.st_size) == st.st_size) {
                    CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
                        (const UInt8 *)buf, st.st_size, kCFAllocatorMalloc);
                    if (data) {
                        CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault,
                            data, kCFPropertyListImmutable, NULL, NULL);
                        CFRelease(data);
                        if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
                            static const CFStringRef locales[] = {
                                CFSTR("zh_CN~iphone"), CFSTR("zh_CN"),
                                CFSTR("zh-Hans~iphone"), CFSTR("zh-Hans"),
                                CFSTR("zh_Hans"),
                            };
                            static const CFStringRef name_keys[] = {
                                CFSTR("CFBundleDisplayName#CH"),
                                CFSTR("CFBundleDisplayName"),
                                CFSTR("CFBundleName"),
                            };
                            for (size_t i = 0; i < sizeof(locales) / sizeof(locales[0]) && !resolved; i++) {
                                CFDictionaryRef sub = (CFDictionaryRef)CFDictionaryGetValue((CFDictionaryRef)plist, locales[i]);
                                if (!sub || CFGetTypeID(sub) != CFDictionaryGetTypeID()) continue;
                                for (size_t k = 0; k < sizeof(name_keys) / sizeof(name_keys[0]); k++) {
                                    CFStringRef val = (CFStringRef)CFDictionaryGetValue(sub, name_keys[k]);
                                    if (val && CFGetTypeID(val) == CFStringGetTypeID() && CFStringGetLength(val) > 0) {
                                        resolved = (CFStringRef)CFRetain(val);
                                        break;
                                    }
                                }
                            }
                        }
                        if (plist) CFRelease(plist);
                    }
                } else {
                    free(buf);
                }
            }
        }
        close(fd);
    }

    os_unfair_lock_lock(&sb26_loctable_lock);
    if (sb26_loctable_name_cache) {
        CFDictionarySetValue(sb26_loctable_name_cache, path_key, resolved ? (CFTypeRef)resolved : (CFTypeRef)kCFNull);
    }
    CFTypeRef out = sb26_loctable_name_cache ? CFDictionaryGetValue(sb26_loctable_name_cache, path_key) : NULL;
    os_unfair_lock_unlock(&sb26_loctable_lock);
    if (resolved) CFRelease(resolved);
    CFRelease(path_key);

    return (out && out != (CFTypeRef)kCFNull) ? (CFStringRef)out : NULL;
}

static CFStringRef
sb26_resolve_bundle_obj_zh_name(id obj)
{
    if (!obj || !sb26_system_prefers_zh()) return NULL;
    SEL url_sel = sel_registerName("bundleURL");
    SEL info_sel = sel_registerName("info");
    SEL app_sel = sel_registerName("application");
    SEL ds_sel = sel_registerName("activeDataSource");
    id target = obj;
    for (int depth = 0; depth < 3 && target; depth++) {
        Class cls = object_getClass(target);
        if (class_respondsToSelector(cls, url_sel)) {
            id url = ((id (*)(id, SEL))objc_msgSend)(target, url_sel);
            if (url) {
                char path[PATH_MAX];
                if (CFURLGetFileSystemRepresentation((CFURLRef)url, true, (UInt8 *)path, sizeof(path))) {
                    return sb26_loctable_display_name_for_path(path);
                }
            }
            break;
        }
        if (class_respondsToSelector(cls, info_sel)) {
            target = ((id (*)(id, SEL))objc_msgSend)(target, info_sel);
        } else if (class_respondsToSelector(cls, app_sel)) {
            target = ((id (*)(id, SEL))objc_msgSend)(target, app_sel);
        } else if (class_respondsToSelector(cls, ds_sel)) {
            target = ((id (*)(id, SEL))objc_msgSend)(target, ds_sel);
        } else {
            break;
        }
    }
    return NULL;
}

static IMP orig_sbapp_displayName;
static IMP orig_sbapp_iconDisplayName;
static IMP orig_sbleaf_displayNameForLoc;
static IMP orig_sbicon_displayName;
static IMP orig_fbsbundle_displayName;
static IMP orig_lsrecord_localizedName;
static IMP orig_lsrecord_localizedShortName;
static IMP orig_lsproxy_localizedName;

static id
sb26_hook_sbapp_displayName(id self, SEL _cmd)
{
    CFStringRef zh = sb26_resolve_bundle_obj_zh_name(self);
    if (zh) return (id)zh;
    return ((id (*)(id, SEL))orig_sbapp_displayName)(self, _cmd);
}

static id
sb26_hook_sbapp_iconDisplayName(id self, SEL _cmd, id icon, id loc)
{
    CFStringRef zh = sb26_resolve_bundle_obj_zh_name(self);
    if (zh) return (id)zh;
    return ((id (*)(id, SEL, id, id))orig_sbapp_iconDisplayName)(self, _cmd, icon, loc);
}

static id
sb26_hook_sbleaf_displayNameForLoc(id self, SEL _cmd, id loc)
{
    CFStringRef zh = sb26_resolve_bundle_obj_zh_name(self);
    if (zh) return (id)zh;
    return ((id (*)(id, SEL, id))orig_sbleaf_displayNameForLoc)(self, _cmd, loc);
}

static id
sb26_hook_sbicon_displayName(id self, SEL _cmd)
{
    CFStringRef zh = sb26_resolve_bundle_obj_zh_name(self);
    if (zh) return (id)zh;
    return ((id (*)(id, SEL))orig_sbicon_displayName)(self, _cmd);
}

static id
sb26_hook_fbsbundle_displayName(id self, SEL _cmd)
{
    CFStringRef zh = sb26_resolve_bundle_obj_zh_name(self);
    if (zh) return (id)zh;
    return ((id (*)(id, SEL))orig_fbsbundle_displayName)(self, _cmd);
}

static id
sb26_hook_lsrecord_localizedName(id self, SEL _cmd)
{
    CFStringRef zh = sb26_resolve_bundle_obj_zh_name(self);
    if (zh) return (id)zh;
    return ((id (*)(id, SEL))orig_lsrecord_localizedName)(self, _cmd);
}

static id
sb26_hook_lsrecord_localizedShortName(id self, SEL _cmd)
{
    CFStringRef zh = sb26_resolve_bundle_obj_zh_name(self);
    if (zh) return (id)zh;
    return ((id (*)(id, SEL))orig_lsrecord_localizedShortName)(self, _cmd);
}

static id
sb26_hook_lsproxy_localizedName(id self, SEL _cmd)
{
    CFStringRef zh = sb26_resolve_bundle_obj_zh_name(self);
    if (zh) return (id)zh;
    return ((id (*)(id, SEL))orig_lsproxy_localizedName)(self, _cmd);
}

static void
sb26_install_localization_hooks(void)
{
    Class sbapp = objc_getClass("SBApplication");
    if (sbapp) {
        Method m = class_getInstanceMethod(sbapp, sel_registerName("displayName"));
        if (m && !orig_sbapp_displayName) {
            orig_sbapp_displayName = method_getImplementation(m);
            method_setImplementation(m, (IMP)sb26_hook_sbapp_displayName);
        }
        Method m2 = class_getInstanceMethod(sbapp, sel_registerName("icon:displayNameForLocation:"));
        if (m2 && !orig_sbapp_iconDisplayName) {
            orig_sbapp_iconDisplayName = method_getImplementation(m2);
            method_setImplementation(m2, (IMP)sb26_hook_sbapp_iconDisplayName);
        }
    }
    Class sbleaf = objc_getClass("SBLeafIcon");
    if (sbleaf) {
        Method m = class_getInstanceMethod(sbleaf, sel_registerName("displayNameForLocation:"));
        if (m && !orig_sbleaf_displayNameForLoc) {
            orig_sbleaf_displayNameForLoc = method_getImplementation(m);
            method_setImplementation(m, (IMP)sb26_hook_sbleaf_displayNameForLoc);
        }
    }
    Class sbicon = objc_getClass("SBIcon");
    if (sbicon) {
        Method m = class_getInstanceMethod(sbicon, sel_registerName("displayName"));
        if (m && !orig_sbicon_displayName) {
            orig_sbicon_displayName = method_getImplementation(m);
            method_setImplementation(m, (IMP)sb26_hook_sbicon_displayName);
        }
    }
    Class fbs = objc_getClass("FBSBundleInfo");
    if (fbs) {
        Method m = class_getInstanceMethod(fbs, sel_registerName("displayName"));
        if (m && !orig_fbsbundle_displayName) {
            orig_fbsbundle_displayName = method_getImplementation(m);
            method_setImplementation(m, (IMP)sb26_hook_fbsbundle_displayName);
        }
    }
    Class lsr = objc_getClass("LSBundleRecord");
    if (lsr) {
        Method m1 = class_getInstanceMethod(lsr, sel_registerName("localizedName"));
        if (m1 && !orig_lsrecord_localizedName) {
            orig_lsrecord_localizedName = method_getImplementation(m1);
            method_setImplementation(m1, (IMP)sb26_hook_lsrecord_localizedName);
        }
        Method m2 = class_getInstanceMethod(lsr, sel_registerName("localizedShortName"));
        if (m2 && !orig_lsrecord_localizedShortName) {
            orig_lsrecord_localizedShortName = method_getImplementation(m2);
            method_setImplementation(m2, (IMP)sb26_hook_lsrecord_localizedShortName);
        }
    }
    Class lsp = objc_getClass("LSApplicationProxy");
    if (lsp) {
        Method m = class_getInstanceMethod(lsp, sel_registerName("localizedName"));
        if (m && !orig_lsproxy_localizedName) {
            orig_lsproxy_localizedName = method_getImplementation(m);
            method_setImplementation(m, (IMP)sb26_hook_lsproxy_localizedName);
        }
    }
}

static void
sb26_clear_wifi_join_failures(void)
{
    const char *wifi_plist = "/private/var/preferences/SystemConfiguration/com.apple.wifi.plist";
    int fd = open(wifi_plist, O_RDONLY);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 2 * 1024 * 1024) {
        close(fd);
        return;
    }
    void *buf = malloc((size_t)st.st_size);
    if (!buf) {
        close(fd);
        return;
    }
    if (read(fd, buf, (size_t)st.st_size) != st.st_size) {
        free(buf);
        close(fd);
        return;
    }
    close(fd);

    CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const UInt8 *)buf, st.st_size, kCFAllocatorMalloc);
    if (!data) return;
    CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
        kCFPropertyListMutableContainersAndLeaves, NULL, NULL);
    CFRelease(data);
    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        if (plist) CFRelease(plist);
        return;
    }

    int modified = 0;
    CFMutableArrayRef known = (CFMutableArrayRef)CFDictionaryGetValue((CFMutableDictionaryRef)plist,
        CFSTR("List of known networks"));
    if (known && CFGetTypeID(known) == CFArrayGetTypeID()) {
        CFIndex count = CFArrayGetCount(known);
        for (CFIndex i = 0; i < count; i++) {
            CFMutableDictionaryRef net = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(known, i);
            if (!net || CFGetTypeID(net) != CFDictionaryGetTypeID()) continue;
            if (CFDictionaryContainsKey(net, CFSTR("FailureThresholdMet")) ||
                CFDictionaryContainsKey(net, CFSTR("FailureCountCurrent")) ||
                CFDictionaryContainsKey(net, CFSTR("LastJoinFailureAt")) ||
                CFDictionaryContainsKey(net, CFSTR("FirstJoinFailureAt"))) {
                CFDictionaryRemoveValue(net, CFSTR("FailureThresholdMet"));
                CFDictionaryRemoveValue(net, CFSTR("FailureCountCurrent"));
                CFDictionaryRemoveValue(net, CFSTR("LastJoinFailureAt"));
                CFDictionaryRemoveValue(net, CFSTR("FirstJoinFailureAt"));
                modified = 1;
            }
        }
    }
    if (modified) {
        CFDataRef out = CFPropertyListCreateData(kCFAllocatorDefault, plist, kCFPropertyListBinaryFormat_v1_0, 0, NULL);
        if (out) {
            int wfd = open(wifi_plist, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (wfd >= 0) {
                ssize_t w = write(wfd, CFDataGetBytePtr(out), (size_t)CFDataGetLength(out));
                (void)w;
                close(wfd);
            }
            CFRelease(out);
            fprintf(stderr, "sb26: cleared stale Wi-Fi join failure counters in com.apple.wifi.plist\n");
        }
    }
    CFRelease(plist);
}

static int
sb26_backlight_was_restored(void)
{
    unsigned int (*get_match)(unsigned int, CFDictionaryRef) =
        (unsigned int (*)(unsigned int, CFDictionaryRef))dlsym(RTLD_DEFAULT, "IOServiceGetMatchingService");
    CFMutableDictionaryRef (*svc_matching)(const char *) =
        (CFMutableDictionaryRef (*)(const char *))dlsym(RTLD_DEFAULT, "IOServiceMatching");
    CFTypeRef (*create_prop)(unsigned int, CFStringRef, CFAllocatorRef, unsigned int) =
        (CFTypeRef (*)(unsigned int, CFStringRef, CFAllocatorRef, unsigned int))dlsym(RTLD_DEFAULT, "IORegistryEntryCreateCFProperty");
    int (*obj_rel)(unsigned int) = (int (*)(unsigned int))dlsym(RTLD_DEFAULT, "IOObjectRelease");
    if (!get_match || !svc_matching || !create_prop || !obj_rel) return 0;

    unsigned int svc = get_match(0, svc_matching("AppleARMBacklight"));
    if (!svc) return 0;
    CFTypeRef val = create_prop(svc, CFSTR("annibale-bl-restored"), kCFAllocatorDefault, 0);
    obj_rel(svc);
    int restored = (val && CFGetTypeID(val) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)val)) ? 1 : 0;
    if (val) CFRelease(val);
    return restored;
}

static void
sb26_sync_initial_brightness(int call_bks_set)
{
    if (sb26_backlight_was_restored()) return;

    float full = 1.0f;
    CFNumberRef one = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &full);
    if (one) {
        CFPreferencesSetAppValue(CFSTR("SBBacklightLevel2"), one, CFSTR("com.apple.springboard"));
        CFPreferencesSetAppValue(CFSTR("BacklightLevel"), one, CFSTR("com.apple.backboardd"));
        CFPreferencesSetAppValue(CFSTR("BKEnableALS"), kCFBooleanFalse, CFSTR("com.apple.backboardd"));
        CFPreferencesAppSynchronize(CFSTR("com.apple.springboard"));
        CFPreferencesAppSynchronize(CFSTR("com.apple.backboardd"));
        CFRelease(one);
    }
    Class bks_cls = objc_getClass("BKSDefaults");
    if (bks_cls) {
        SEL ext_sel = sel_registerName("externalDefaults");
        SEL sb_sel = sel_registerName("springBoardDefaults");
        SEL set_sel = sel_registerName("setBrightness:");
        if (class_getClassMethod(bks_cls, ext_sel)) {
            id ext = ((id (*)(Class, SEL))objc_msgSend)(bks_cls, ext_sel);
            if (ext && class_getInstanceMethod(object_getClass(ext), sb_sel)) {
                id sb_def = ((id (*)(id, SEL))objc_msgSend)(ext, sb_sel);
                if (sb_def && class_getInstanceMethod(object_getClass(sb_def), set_sel)) {
                    ((void (*)(id, SEL, float))objc_msgSend)(sb_def, set_sel, 1.0f);
                }
            }
        }
    }
    if (call_bks_set) {
        void (*bks_set)(float, int) = (void (*)(float, int))dlsym(RTLD_DEFAULT, "BKSDisplayBrightnessSet");
        if (bks_set) {
            bks_set(1.0f, 1);
        }
    }
}
