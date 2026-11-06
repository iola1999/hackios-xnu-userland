/*
 * SpringBoard startup state, software icon selection and unlocking for 23G90.
 * MobileActivation is not implemented; this sets SBLockdownManager's local
 * activation state so SpringBoard can finish startup. The implementation and
 * object layout are specific to the checked 23G90 shared cache.
 *
 * The lock screen is dismissed once after boot, as 16.5's inject_sb.c does, and
 * again whenever the Darwin notification com.redwood.unlock is posted
 * (rwtouch notify com.redwood.unlock). autounlock=0 among the boot arguments
 * keeps the lock screen until it is swiped away.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <ptrauth.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *);
extern id MTLCreateSystemDefaultDevice(void);
void sb26_install_layer_dump(void);   /* inject_sb26_layers.c */
void sb26_install_rate(uintptr_t slide);   /* inject_sb26_rate.c */

static atomic_uint activation_calls;

static uintptr_t
code_address(IMP implementation)
{
    return (uintptr_t)ptrauth_strip(implementation, ptrauth_key_function_pointer);
}

static int
method_encoding_matches(Method method, const char *expected)
{
    const char *encoding = method ? method_getTypeEncoding(method) : NULL;
    return encoding && !strcmp(encoding, expected);
}

static Method
checked_method(const char *class_name, const char *selector_name, const char *encoding,
    uintptr_t expected, const unsigned char bytes[16])
{
    Class cls = objc_getClass(class_name);
    Method method = cls ? class_getInstanceMethod(cls, sel_registerName(selector_name)) : NULL;
    IMP implementation = method ? method_getImplementation(method) : NULL;
    uintptr_t address = implementation ? code_address(implementation) : 0;
    int match = address == expected && method_encoding_matches(method, encoding);
    if (match) match = !memcmp((const void *)address, bytes, 16);
    fprintf(stderr, "sb26: validate %s address=%lx expected=%lx encoding=%s match=%d\n",
        selector_name, address, expected, method ? method_getTypeEncoding(method) : "missing", match);
    return match ? method : NULL;
}

static IMP original_bitmap_selection;
static dispatch_once_t metal_device_once;
static int has_metal_device;
static SEL appearance_selector, glass_selector;

static void
query_metal_device(void *context)
{
    (void)context;
    id device = MTLCreateSystemDefaultDevice();
    has_metal_device = device != nil;
    /* MTLCreateSystemDefaultDevice returns a retained object. */
    if (device) ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("release"));
    fprintf(stderr, "sb26: icon rendering Metal device=%d\n", has_metal_device);
}

static BOOL
software_bitmap_selection(id self, SEL selector)
{
    BOOL original = ((BOOL (*)(id, SEL))original_bitmap_selection)(self, selector);
    if (original) return original;

    /*
     * If the current icon view carries a Calendar icon (e.g. SBHCalendarApplicationIcon),
     * force bitmap rasterization. In iOS 26 with Liquid Glass enabled, SBCalendarIconImageProvider
     * attempts to invoke [ISImage ICRIconLayer], which is unimplemented and returns nil,
     * leaving CALayer.contents empty and making the icon transparent.
     */
    id icon = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("icon"));
    if (icon != nil) {
        const char *icon_class = object_getClassName(icon);
        if (icon_class && strstr(icon_class, "Calendar") != NULL) {
            return YES;
        }
    }

    Method appearance_method = class_getInstanceMethod(object_getClass(self), appearance_selector);
    if (!method_encoding_matches(appearance_method, "@16@0:8")) return original;
    id appearance = ((id (*)(id, SEL))objc_msgSend)(self, appearance_selector);
    Method glass = appearance ? class_getInstanceMethod(object_getClass(appearance), glass_selector) : NULL;
    if (!method_encoding_matches(glass, "B16@0:8")) return original;
    if (((BOOL (*)(id, SEL))objc_msgSend)(appearance, glass_selector)) return original;
    dispatch_once_f(&metal_device_once, NULL, query_metal_device);
    return has_metal_device ? original : YES;
}

static void
install_bitmap_selection(uintptr_t slide)
{
    static const unsigned char bytes[16] = {
        0x7f,0x23,0x03,0xd5,0xf6,0x57,0xbd,0xa9,0xf4,0x4f,0x01,0xa9,0xfd,0x7b,0x02,0xa9
    };
    Method method = checked_method("SBIconImageView", "shouldForceBitmapImages", "B16@0:8",
        0x1c1a37ae0UL + slide, bytes);
    Method appearance = checked_method("SBIconImageView", "effectiveIconImageAppearance", "@16@0:8",
        0x1c17cc92cUL + slide, bytes);
    if (!method || !appearance) return;
    appearance_selector = sel_registerName("effectiveIconImageAppearance");
    glass_selector = sel_registerName("hasGlass");
    original_bitmap_selection = method_getImplementation(method);
    method_setImplementation(method, (IMP)software_bitmap_selection);
    int installed = code_address(method_getImplementation(method)) == code_address((IMP)software_bitmap_selection);
    fprintf(stderr, "sb26: build=23G90 software_icons_installed=%d\n", installed);
}

static void
setup_activation_state(id self, SEL selector)
{
    (void)selector;
    int previous;
    memcpy(&previous, (const char *)self + 8, sizeof(previous));
    int activated = 2;
    memcpy((char *)self + 8, &activated, sizeof(activated));
    unsigned call = atomic_fetch_add_explicit(&activation_calls, 1, memory_order_relaxed);
    if (call < 16)
        fprintf(stderr, "sb26: activation call=%u previous=%d applied=2\n", call + 1, previous);
}

/* What the cover sheet passes to -[SBLockScreenManager unlockUIFromSource:withOptions:] on 16.5. */
#define UNLOCK_SOURCE_COVER_SHEET 24
/*
 * The lock screen takes input about 21 s after SpringBoard connects to backboardd: on 2026-10-29
 * the boot overlay came down 18 s in and the backlight stayed off 3 s more, and two swipes sent
 * before that were dropped. isUILocked is not yet true at the first attempts either, so the boot
 * unlock is done only once the lock screen has been seen up and then gone.
 */
#define UNLOCK_FIRST_SECONDS 10
#define UNLOCK_EVERY_SECONDS 3
#define UNLOCK_ATTEMPTS 60

static id
lock_screen_manager(void)
{
    Class cls = objc_getClass("SBLockScreenManager");
    SEL shared = sel_registerName("sharedInstance");
    if (!cls || !class_getClassMethod(cls, shared)) return nil;
    return ((id (*)(Class, SEL))objc_msgSend)(cls, shared);
}

/* 1: locked, an unlock was asked for; 0: not locked; -1: no lock screen manager to ask. */
static int
unlock_once(const char *why)
{
    SEL locked = sel_registerName("isUILocked");
    SEL unlock_ui = sel_registerName("unlockUIFromSource:withOptions:");
    Class cls = objc_getClass("SBLockScreenManager");
    if (!cls || !class_getInstanceMethod(cls, locked) || !class_getInstanceMethod(cls, unlock_ui)) {
        fprintf(stderr, "sb26: unlock %s: SBLockScreenManager lacks isUILocked or unlockUIFromSource:withOptions:\n", why);
        return -1;
    }
    id manager = lock_screen_manager();
    if (!manager) return -1;
    if (!((BOOL (*)(id, SEL))objc_msgSend)(manager, locked)) {
        notify_post("com.redwood.wallpaper.suspend");
        return 0;
    }
    BOOL answer = ((BOOL (*)(id, SEL, long, id))objc_msgSend)(manager, unlock_ui, UNLOCK_SOURCE_COVER_SHEET, nil);
    int locked_after = ((BOOL (*)(id, SEL))objc_msgSend)(manager, locked);
    fprintf(stderr, "sb26: unlock %s: answer=%d locked_after=%d\n", why, answer, locked_after);
    if (!locked_after) {
        notify_post("com.redwood.wallpaper.suspend");
    }
    return 1;
}

static int boot_unlock_seen_locked;

static void
boot_unlock(void *context)
{
    uintptr_t attempt = (uintptr_t)context + 1;
    int state = unlock_once("boot");
    if (state == 1) boot_unlock_seen_locked = 1;
    if (state == 0 && boot_unlock_seen_locked) {
        fprintf(stderr, "sb26: boot unlock done after %lu attempts\n", (unsigned long)attempt);
        notify_post("com.redwood.wallpaper.suspend");
        return;
    }
    if (attempt >= UNLOCK_ATTEMPTS) {
        fprintf(stderr, "sb26: boot unlock gave up after %lu attempts, lock screen seen=%d\n",
            (unsigned long)attempt, boot_unlock_seen_locked);
        return;
    }
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, UNLOCK_EVERY_SECONDS * NSEC_PER_SEC),
        dispatch_get_main_queue(), (void *)attempt, boot_unlock);
}

static BOOL (*orig_unlock_ui)(id, SEL, long, id);
static BOOL
sb26_hook_unlock_ui(id self, SEL cmd, long source, id options)
{
    BOOL ans = orig_unlock_ui ? orig_unlock_ui(self, cmd, source, options) : NO;
    SEL locked = sel_registerName("isUILocked");
    if (!((BOOL (*)(id, SEL))objc_msgSend)(self, locked)) {
        notify_post("com.redwood.wallpaper.suspend");
    }
    return ans;
}

int
sb26_boot_arg_present(const char *wanted)
{
    char args[1024] = "";
    size_t size = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) return 0;
    size_t length = strlen(wanted);
    for (const char *at = strstr(args, wanted); at; at = strstr(at + 1, wanted))
        if ((at == args || at[-1] == ' ') && (at[length] == ' ' || at[length] == 0)) return 1;
    return 0;
}

static void
install_unlock(void)
{
    int token;
    notify_register_dispatch("com.redwood.unlock", &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        if (unlock_once("notification") == 0) {
            fprintf(stderr, "sb26: unlock notification: not locked\n");
            notify_post("com.redwood.wallpaper.suspend");
        }
    });

    Class cls = objc_getClass("SBLockScreenManager");
    if (cls) {
        Method m = class_getInstanceMethod(cls, sel_registerName("unlockUIFromSource:withOptions:"));
        if (m) {
            orig_unlock_ui = (BOOL (*)(id, SEL, long, id))method_getImplementation(m);
            method_setImplementation(m, (IMP)sb26_hook_unlock_ui);
        }
    }

    if (sb26_boot_arg_present("autounlock=0")) {
        fprintf(stderr, "sb26: autounlock=0, the lock screen stays until it is swiped away\n");
        return;
    }
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, UNLOCK_FIRST_SECONDS * NSEC_PER_SEC),
        dispatch_get_main_queue(), NULL, boot_unlock);
}

static BOOL
haptic_return_no(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return NO;
}

static BOOL
haptic_start_and_return_error(id self, SEL cmd, id *err)
{
    (void)self;
    (void)cmd;
    if (err) *err = nil;
    return YES;
}

static void
haptic_start_with_completion(id self, SEL cmd, void (^handler)(id))
{
    (void)self;
    (void)cmd;
    if (handler) handler(nil);
}

static BOOL
haptic_stop_and_return_error(id self, SEL cmd, id *err)
{
    (void)self;
    (void)cmd;
    if (err) *err = nil;
    return YES;
}

static void
haptic_stop_with_completion(id self, SEL cmd, void (^handler)(id))
{
    (void)self;
    (void)cmd;
    if (handler) handler(nil);
}

static id (*orig_haptic_caps)(id, SEL);
static id
haptic_capabilities_for_hardware(id self, SEL cmd)
{
    id caps = orig_haptic_caps ? orig_haptic_caps(self, cmd) : nil;
    if (caps) {
        Class caps_cls = object_getClass(caps);
        Method m_h = class_getInstanceMethod(caps_cls, sel_registerName("supportsHaptics"));
        if (m_h) method_setImplementation(m_h, (IMP)haptic_return_no);
        Method m_a = class_getInstanceMethod(caps_cls, sel_registerName("supportsAudio"));
        if (m_a) method_setImplementation(m_a, (IMP)haptic_return_no);
    }
    return caps;
}

static void
install_haptic_hooks(Class engine_cls)
{
    if (!engine_cls) return;

    Method m_caps = class_getClassMethod(engine_cls, sel_registerName("capabilitiesForHardware"));
    if (m_caps) {
        orig_haptic_caps = (id (*)(id, SEL))method_getImplementation(m_caps);
        method_setImplementation(m_caps, (IMP)haptic_capabilities_for_hardware);
    }

    Method m_start = class_getInstanceMethod(engine_cls, sel_registerName("startAndReturnError:"));
    if (m_start) method_setImplementation(m_start, (IMP)haptic_start_and_return_error);

    Method m_start_cb = class_getInstanceMethod(engine_cls, sel_registerName("startWithCompletionHandler:"));
    if (m_start_cb) method_setImplementation(m_start_cb, (IMP)haptic_start_with_completion);

    Method m_stop = class_getInstanceMethod(engine_cls, sel_registerName("stopAndReturnError:"));
    if (m_stop) method_setImplementation(m_stop, (IMP)haptic_stop_and_return_error);

    Method m_stop_cb = class_getInstanceMethod(engine_cls, sel_registerName("stopWithCompletionHandler:"));
    if (m_stop_cb) method_setImplementation(m_stop_cb, (IMP)haptic_stop_with_completion);

    fprintf(stderr, "sb26: CHHapticEngine non-blocking interception installed\n");
}

static void
haptic_image_callback(const struct mach_header *header, intptr_t slide)
{
    (void)header;
    (void)slide;
    static int haptics_done;
    if (haptics_done) return;
    Class cls = objc_getClass("CHHapticEngine");
    if (cls) {
        haptics_done = 1;
        install_haptic_hooks(cls);
    }
}

static int
is_build_23g90(void)
{
    FILE *file = fopen("/System/Library/CoreServices/SystemVersion.plist", "rb");
    if (!file) return 0;
    UInt8 bytes[8192];
    size_t size = fread(bytes, 1, sizeof(bytes), file);
    int valid = !ferror(file) && size && size < sizeof(bytes);
    fclose(file);
    if (!valid) return 0;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)size);
    if (!data) return 0;
    CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
        kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    valid = 0;
    if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        CFTypeRef build = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("ProductBuildVersion"));
        valid = build && CFGetTypeID(build) == CFStringGetTypeID() && CFEqual(build, CFSTR("23G90"));
    }
    if (plist) CFRelease(plist);
    return valid;
}

/*
 * Settings "About" capacity fix:
 * Preferences reads total capacity from MGCopyAnswer(CFSTR("DiskUsage")) or
 * MGCopyAnswer(CFSTR("TotalDiskCapacity")). Because HackiOS lacks Apple NVMe
 * controller driver, MobileGestalt returns 0 KB.
 * We interpose MGCopyAnswer to report a standard 256 GB capacity with
 * real free space read from statfs("/private/var").
 */
extern CFTypeRef MGCopyAnswer(CFStringRef property);

static void
sb26_query_capacities(int64_t *total_disk, int64_t *total_sys, int64_t *total_data, int64_t *avail_val)
{
    uint64_t sys_b = 0, data_b = 0, used_b = 0;
    size_t sz = sizeof(uint64_t);
    struct statfs st;

    (void)sysctlbyname("kern.redwood.darwin_bytes", &sys_b, &sz, NULL, 0);
    sz = sizeof(uint64_t);
    (void)sysctlbyname("kern.redwood.darwin_var_bytes", &data_b, &sz, NULL, 0);
    if (sys_b == 0) sys_b = 8ULL << 30;
    if (data_b == 0) data_b = 112ULL << 30;
    if (statfs("/private/var", &st) == 0 && st.f_blocks > st.f_bavail) {
        used_b = (uint64_t)(st.f_blocks - st.f_bavail) * (uint64_t)st.f_bsize;
    }
    *total_sys  = (int64_t)sys_b;
    *total_data = (int64_t)data_b;
    *total_disk = (int64_t)(sys_b + data_b);
    *avail_val  = (int64_t)(data_b > used_b ? data_b - used_b : data_b);
}

static CFTypeRef
sb26_hook_MGCopyAnswer(CFStringRef property)
{
    const char *prog = getprogname();
    if (property && prog && (strcmp(prog, "Preferences") == 0 || strcmp(prog, "SpringBoard") == 0)) {
        int64_t total_disk = 0, total_sys = 0, total_data = 0, avail_val = 0;
        if (CFEqual(property, CFSTR("DiskUsage"))) {
            sb26_query_capacities(&total_disk, &total_sys, &total_data, &avail_val);

            CFNumberRef num_total_disk = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_disk);
            CFNumberRef num_total_sys  = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_sys);
            CFNumberRef num_total_data = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_data);
            CFNumberRef num_avail      = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &avail_val);

            const void *keys[] = {
                CFSTR("TotalDiskCapacity"),
                CFSTR("TotalSystemCapacity"),
                CFSTR("TotalDataCapacity"),
                CFSTR("TotalDataAvailable"),
                CFSTR("AmountDataAvailable")
            };
            const void *values[] = {
                num_total_disk,
                num_total_sys,
                num_total_data,
                num_avail,
                num_avail
            };

            CFDictionaryRef dict = CFDictionaryCreate(
                kCFAllocatorDefault,
                keys,
                values,
                sizeof(keys) / sizeof(keys[0]),
                &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks
            );

            if (num_total_disk) CFRelease(num_total_disk);
            if (num_total_sys)  CFRelease(num_total_sys);
            if (num_total_data) CFRelease(num_total_data);
            if (num_avail)      CFRelease(num_avail);

            return dict;
        }

        if (CFEqual(property, CFSTR("TotalDiskCapacity"))) {
            sb26_query_capacities(&total_disk, &total_sys, &total_data, &avail_val);
            return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_disk);
        }

        if (CFEqual(property, CFSTR("TotalSystemCapacity"))) {
            sb26_query_capacities(&total_disk, &total_sys, &total_data, &avail_val);
            return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_sys);
        }

        if (CFEqual(property, CFSTR("TotalDataCapacity"))) {
            sb26_query_capacities(&total_disk, &total_sys, &total_data, &avail_val);
            return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &total_data);
        }

        if (CFEqual(property, CFSTR("TotalDataAvailable")) || CFEqual(property, CFSTR("AmountDataAvailable"))) {
            sb26_query_capacities(&total_disk, &total_sys, &total_data, &avail_val);
            return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &avail_val);
        }

        if (CFEqual(property, CFSTR("airdrop-p2p")) || CFEqual(property, CFSTR("airdrop"))) {
            return kCFBooleanFalse;
        }
    }

    return MGCopyAnswer(property);
}

/*
 * CoreFoundation bundle localization fix:
 * In pruned images (select-system.py), zh*.lproj directories are absent,
 * causing CFBundleCopyBundleLocalizations to assume the bundle only supports English.
 * System .loctable files retain Chinese translation entries.
 * We interpose CFBundleCopyBundleLocalizations to append Chinese language identifiers
 * whenever the bundle has localization resources (.loctable or strings).
 */
extern CFArrayRef CFBundleCopyBundleLocalizations(CFBundleRef bundle);

static int
dir_contains_localization_file(const char *dirpath)
{
    DIR *d = opendir(dirpath);
    if (!d) return 0;
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;
        size_t len = strlen(name);
        if ((len > 9 && !strcmp(name + len - 9, ".loctable")) ||
            (len > 8 && !strcmp(name + len - 8, ".strings")) ||
            (len > 12 && !strcmp(name + len - 12, ".stringsdict")) ||
            (len > 6 && !strcmp(name + len - 6, ".lproj"))) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int
bundle_has_localization_resources(CFBundleRef bundle)
{
    if (!bundle) return 0;
    CFURLRef url = CFBundleCopyBundleURL(bundle);
    if (!url) return 1;
    char path[PATH_MAX], sub[PATH_MAX];
    Boolean ok = CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof(path));
    CFRelease(url);
    if (!ok) return 1;
    if (dir_contains_localization_file(path)) return 1;
    if (snprintf(sub, sizeof(sub), "%s/Resources", path) > 0 && dir_contains_localization_file(sub)) return 1;
    if (snprintf(sub, sizeof(sub), "%s/Contents/Resources", path) > 0 && dir_contains_localization_file(sub)) return 1;
    return 0;
}

static CFArrayRef
sb26_hook_CFBundleCopyBundleLocalizations(CFBundleRef bundle)
{
    CFArrayRef orig = CFBundleCopyBundleLocalizations(bundle);
    if (!orig || CFGetTypeID(orig) != CFArrayGetTypeID()) return orig;
    CFIndex count = CFArrayGetCount(orig);
    if (count <= 0) return orig;

    for (CFIndex i = 0; i < count; i++) {
        CFTypeRef item = CFArrayGetValueAtIndex(orig, i);
        if (item && CFGetTypeID(item) == CFStringGetTypeID() && CFStringHasPrefix((CFStringRef)item, CFSTR("zh"))) {
            return orig;
        }
    }
    if (!bundle_has_localization_resources(bundle)) return orig;

    static const CFStringRef zh_codes[] = {
        CFSTR("zh-Hans"), CFSTR("zh_CN"), CFSTR("zh-Hant"), CFSTR("zh_TW"), CFSTR("zh_HK"),
    };
    const CFIndex num_zh = (CFIndex)(sizeof(zh_codes) / sizeof(zh_codes[0]));
    CFMutableArrayRef mut = CFArrayCreateMutableCopy(kCFAllocatorDefault, count + num_zh, orig);
    if (!mut) return orig;

    for (CFIndex i = 0; i < num_zh; i++) {
        CFStringRef code = zh_codes[i];
        int exists = 0;
        CFIndex cur = CFArrayGetCount(mut);
        for (CFIndex j = 0; j < cur; j++) {
            CFTypeRef v = CFArrayGetValueAtIndex(mut, j);
            if (v && CFGetTypeID(v) == CFStringGetTypeID() && CFEqual(v, code)) { exists = 1; break; }
        }
        if (!exists) CFArrayAppendValue(mut, code);
    }
    CFRelease(orig);
    return (CFArrayRef)mut;
}

/*
 * Ensure .GlobalPreferences.plist in /private/var/mobile/Library/Preferences/
 * has AppleLanguages with "zh-Hans-CN" prioritized.
 */
static void
sb26_ensure_chinese_preferences(void)
{
    static int preferences_ensured = 0;
    if (preferences_ensured) return;
    preferences_ensured = 1;

    const char *pref_dir = "/private/var/mobile/Library/Preferences";
    const char *pref_path = "/private/var/mobile/Library/Preferences/.GlobalPreferences.plist";

    struct stat st;
    if (stat(pref_dir, &st) != 0) {
        mkdir("/private/var/mobile", 0755);
        chown("/private/var/mobile", 501, 501);
        mkdir("/private/var/mobile/Library", 0755);
        chown("/private/var/mobile/Library", 501, 501);
        mkdir(pref_dir, 0755);
        chown(pref_dir, 501, 501);
    }

    CFMutableDictionaryRef dict = NULL;
    int fd = open(pref_path, O_RDONLY);
    if (fd >= 0) {
        if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < 1024 * 1024) {
            void *buf = malloc((size_t)st.st_size);
            if (buf) {
                if (read(fd, buf, (size_t)st.st_size) == st.st_size) {
                    CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const UInt8 *)buf, st.st_size, kCFAllocatorMalloc);
                    if (data) {
                        CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListMutableContainersAndLeaves, NULL, NULL);
                        CFRelease(data);
                        if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) dict = (CFMutableDictionaryRef)plist;
                        else if (plist) CFRelease(plist);
                    }
                } else {
                    free(buf);
                }
            }
        }
        close(fd);
    }
    if (!dict) dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!dict) return;

    CFArrayRef orig_langs = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("AppleLanguages"));
    int need_update = 0;
    if (!orig_langs || CFGetTypeID(orig_langs) != CFArrayGetTypeID() || CFArrayGetCount(orig_langs) == 0) {
        need_update = 1;
    } else {
        CFTypeRef first = CFArrayGetValueAtIndex(orig_langs, 0);
        if (!first || CFGetTypeID(first) != CFStringGetTypeID() ||
            (!CFEqual(first, CFSTR("zh-Hans-CN")) && !CFEqual(first, CFSTR("zh-Hans")))) {
            need_update = 1;
        }
    }

    if (need_update) {
        CFMutableArrayRef new_langs = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        CFArrayAppendValue(new_langs, CFSTR("zh-Hans-CN"));
        CFArrayAppendValue(new_langs, CFSTR("zh-Hans"));
        if (orig_langs && CFGetTypeID(orig_langs) == CFArrayGetTypeID()) {
            CFIndex count = CFArrayGetCount(orig_langs);
            for (CFIndex i = 0; i < count; i++) {
                CFTypeRef item = CFArrayGetValueAtIndex(orig_langs, i);
                if (item && CFGetTypeID(item) == CFStringGetTypeID() &&
                    !CFEqual(item, CFSTR("zh-Hans-CN")) && !CFEqual(item, CFSTR("zh-Hans"))) {
                    CFArrayAppendValue(new_langs, item);
                }
            }
        } else {
            CFArrayAppendValue(new_langs, CFSTR("en-CN"));
            CFArrayAppendValue(new_langs, CFSTR("en"));
        }
        CFDictionarySetValue(dict, CFSTR("AppleLanguages"), new_langs);
        if (!CFDictionaryContainsKey(dict, CFSTR("AppleLocale"))) {
            CFDictionarySetValue(dict, CFSTR("AppleLocale"), CFSTR("zh_CN"));
        }

        CFDataRef xml_data = CFPropertyListCreateData(kCFAllocatorDefault, dict, kCFPropertyListXMLFormat_v1_0, 0, NULL);
        if (xml_data) {
            int wfd = open(pref_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (wfd >= 0) {
                ssize_t written = write(wfd, CFDataGetBytePtr(xml_data), (size_t)CFDataGetLength(xml_data));
                (void)written;
                close(wfd);
                chown(pref_path, 501, 501);
                chmod(pref_path, 0644);
            }
            CFRelease(xml_data);
        }
        CFPreferencesSetAppValue(CFSTR("AppleLanguages"), new_langs, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("AppleLocale"), CFSTR("zh_CN"), kCFPreferencesAnyApplication);
        CFPreferencesAppSynchronize(kCFPreferencesAnyApplication);
        CFRelease(new_langs);
        fprintf(stderr, "sb26: AppleLanguages ensured zh-Hans-CN prioritized (updated)\n");
    }
    CFRelease(dict);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
    const void *replacement;
    const void *original;
} sb26_interposers[] = {
    { (const void *)sb26_hook_MGCopyAnswer, (const void *)MGCopyAnswer },
    { (const void *)sb26_hook_CFBundleCopyBundleLocalizations, (const void *)CFBundleCopyBundleLocalizations },
};

#include "inject_sb26_airdrop.inc"

static void
sb26_trigger_poweroff(id self, SEL _cmd, id arg1, id arg2)
{
    (void)self;
    (void)_cmd;
    (void)arg1;
    (void)arg2;
    int one = 1;
    fprintf(stderr, "sb26: poweroff requested via %s, syncing and halting\n",
        _cmd ? sel_getName(_cmd) : "unknown");
    sync();
    sysctlbyname("kern.redwood.halt", NULL, NULL, &one, sizeof(one));
}

static void
install_powerdown_hooks(void)
{
    static const struct {
        const char *cls;
        const char *sel;
    } targets[] = {
        { "SBPowerDownViewController", "powerDownViewRequestPowerDown:" },
        { "SBMainWorkspace", "powerDownViewControllerRequestsPowerDown:" },
        { "SBRestartManager", "shutdownForReason:" },
        { "SBRestartManager", "shutdownWithContext:" },
        { "FBSystemService", "shutdownWithOptions:" },
        { "FBSystemService", "shutdownWithOptions:origin:" },
    };
    unsigned patched = 0;
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        Class cls = objc_getClass(targets[i].cls);
        if (!cls) continue;
        Method m = class_getInstanceMethod(cls, sel_registerName(targets[i].sel));
        if (!m) continue;
        method_setImplementation(m, (IMP)sb26_trigger_poweroff);
        patched++;
    }
    fprintf(stderr, "sb26: powerdown hooks installed (%u methods)\n", patched);
}

static void
discovery_image_callback(const struct mach_header *header, intptr_t slide)
{
    (void)header;
    (void)slide;
    Class airdrop = objc_getClass("SFAirDropDiscoveryController");
    if (airdrop) install_airdrop_hooks(airdrop);

    Class session = objc_getClass("AVOutputDeviceDiscoverySession");
    if (session) install_screen_mirroring_hooks(session);

    Class mru = objc_getClass("MRUMirroringController");
    if (mru) install_mru_mirroring_hooks(mru);

    Class cal = objc_getClass("SBCalendarIconImageProvider");
    if (cal) install_calendar_icon_hooks(cal);
}

__attribute__((constructor)) static void
install_sb26(void)
{
    const char *prog = getprogname();
    if (!prog || (strcmp(prog, "SpringBoard") && strcmp(prog, "Preferences"))) return;

    sb26_ensure_chinese_preferences();

    if (!is_build_23g90()) {
        fprintf(stderr, "sb26: skipped build mismatch or unreadable SystemVersion.plist\n");
        return;
    }

    if (strcmp(prog, "SpringBoard") != 0) return;

    size_t size = 0;
    uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&size);
    if (base < 0x180000000UL || !size) {
        fprintf(stderr, "sb26: skipped invalid shared cache range\n");
        return;
    }
    uintptr_t slide = base - 0x180000000UL;
    static const unsigned char expected_bytes[16] = {
        0x08,0x30,0x40,0x39,0xe8,0x07,0x00,0x37,0x7f,0x23,0x03,0xd5,0xff,0x43,0x01,0xd1
    };
    Method method = checked_method("SBLockdownManager", "_setupActivationState", "v16@0:8",
        0x22452fdc0UL + slide, expected_bytes);
    if (method) {
        method_setImplementation(method, (IMP)setup_activation_state);
        int installed = code_address(method_getImplementation(method)) == code_address((IMP)setup_activation_state);
        fprintf(stderr, "sb26: build=23G90 activation_installed=%d\n", installed);
    }
    install_bitmap_selection(slide);
    install_unlock();
    sb26_install_layer_dump();
    sb26_install_rate(slide);
    install_powerdown_hooks();

    _dyld_register_func_for_add_image(haptic_image_callback);
    Class engine_cls = objc_getClass("CHHapticEngine");
    if (engine_cls) {
        install_haptic_hooks(engine_cls);
    }

    _dyld_register_func_for_add_image(discovery_image_callback);
    Class airdrop_cls = objc_getClass("SFAirDropDiscoveryController");
    if (airdrop_cls) install_airdrop_hooks(airdrop_cls);
    Class session_cls = objc_getClass("AVOutputDeviceDiscoverySession");
    if (session_cls) install_screen_mirroring_hooks(session_cls);
    Class mru_cls = objc_getClass("MRUMirroringController");
    if (mru_cls) install_mru_mirroring_hooks(mru_cls);
    Class cal_cls = objc_getClass("SBCalendarIconImageProvider");
    if (cal_cls) install_calendar_icon_hooks(cal_cls);
}
