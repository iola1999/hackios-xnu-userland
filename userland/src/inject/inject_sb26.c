/*
 * SpringBoard startup state and unlocking for 23G90.
 * MobileActivation is not implemented; this sets SBLockdownManager's local
 * activation state so SpringBoard can finish startup. The implementation and
 * object layout are specific to the checked 23G90 shared cache.
 *
 * The lock screen is dismissed once after boot, as 16.5's inject_sb.c does, and
 * again whenever the Darwin notification com.annibale.unlock is posted
 * (anntouch notify com.annibale.unlock). autounlock=0 among the boot arguments
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
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <os/lock.h>
#include <pthread.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *);
void sb26_install_layer_dump(void);   /* inject_sb26_layers.c */
void sb26_install_rate(uintptr_t slide);   /* inject_sb26_rate.c */
static void sb26_sync_initial_brightness(int call_bks_set);

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

/* CoverSheet unlock source on iOS 26. */
#define UNLOCK_SOURCE_COVER_SHEET 25
/*
 * The lock screen takes input about 21 s after SpringBoard connects to backboardd: on 2026-10-29
 * the boot overlay came down 18 s in and the backlight stayed off 3 s more, and two swipes sent
 * before that were dropped. isUILocked is not yet true at the first attempts either, so the boot
 * unlock is done only once the lock screen has been seen up and then gone.
 */
#define UNLOCK_FIRST_SECONDS 2
#define UNLOCK_EVERY_SECONDS 1
#define UNLOCK_ATTEMPTS 120

static id
lock_screen_manager(void)
{
    Class cls = objc_getClass("SBLockScreenManager");
    SEL shared = sel_registerName("sharedInstance");
    if (!cls || !class_getClassMethod(cls, shared)) return nil;
    return ((id (*)(Class, SEL))objc_msgSend)(cls, shared);
}

/* 1: locked, an unlock was asked for; 0: not locked; -1: no lock screen manager to ask. */
static int sb26_wallpaper_suspended = -1;
static int sb26_suspend_token = NOTIFY_TOKEN_INVALID;
static uint64_t sb26_suspend_gen;

#define SB26_WALLPAPER_SUSPEND_DELAY_MS 1500

static int
sb26_cover_sheet_is_presented(void)
{
    Class cspm_cls = objc_getClass("SBCoverSheetPresentationManager");
    SEL shared_sel = sel_registerName("sharedInstanceIfExists");
    if (!cspm_cls || !class_getClassMethod(cspm_cls, shared_sel)) return -1;
    id cspm = ((id (*)(Class, SEL))objc_msgSend)(cspm_cls, shared_sel);
    if (!cspm) return -1;
    SEL presented_sel = sel_registerName("isPresented");
    if (!class_respondsToSelector(object_getClass(cspm), presented_sel)) return -1;
    return ((BOOL (*)(id, SEL))objc_msgSend)(cspm, presented_sel) ? 1 : 0;
}

static void
sb26_set_wallpaper_suspended(int suspend, const char *reason)
{
    if (sb26_wallpaper_suspended == suspend) return;
    sb26_wallpaper_suspended = suspend;
    if (sb26_suspend_token == NOTIFY_TOKEN_INVALID &&
        notify_register_check("com.annibale.wallpaper.suspend", &sb26_suspend_token) != NOTIFY_STATUS_OK) {
        sb26_suspend_token = NOTIFY_TOKEN_INVALID;
    }
    if (sb26_suspend_token != NOTIFY_TOKEN_INVALID) {
        notify_set_state(sb26_suspend_token, suspend ? 1 : 0);
    }
    notify_post(suspend ? "com.annibale.wallpaper.suspend" : "com.annibale.wallpaper.resume");
    fprintf(stderr, "sb26: wallpaper %s (%s)\n", suspend ? "suspended" : "resumed", reason);
}

static void
sb26_resume_wallpaper(const char *reason)
{
    sb26_suspend_gen++;
    sb26_set_wallpaper_suspended(0, reason);
}

static void
sb26_schedule_wallpaper_suspend(const char *reason)
{
    uint64_t gen = ++sb26_suspend_gen;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)SB26_WALLPAPER_SUSPEND_DELAY_MS * NSEC_PER_MSEC),
        dispatch_get_main_queue(), ^{
        if (gen != sb26_suspend_gen) return;
        if (sb26_cover_sheet_is_presented() == 1) return;
        sb26_set_wallpaper_suspended(1, reason);
    });
}

static void
sb26_on_unlocked(void)
{
    sb26_schedule_wallpaper_suspend("unlock");
}

typedef void (^sb26_notif_block_t)(id note);

static void
sb26_observe_notification(const char *name, sb26_notif_block_t block)
{
    Class nc_cls = objc_getClass("NSNotificationCenter");
    if (!nc_cls) return;
    id center = ((id (*)(Class, SEL))objc_msgSend)(nc_cls, sel_registerName("defaultCenter"));
    if (!center) return;
    CFStringRef cf_name = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
    if (!cf_name) return;
    (void)((id (*)(id, SEL, id, id, id, sb26_notif_block_t))objc_msgSend)(
        center,
        sel_registerName("addObserverForName:object:queue:usingBlock:"),
        (id)cf_name, nil, nil, block);
    CFRelease(cf_name);
}

/*
 * Keep MercuryPosterExtension synchronized with CoverSheet / LockScreen visibility:
 * resume synchronously when CoverSheet or LockScreen is about to present so the live
 * wallpaper renders Notification Center / Lock Screen instead of staying frozen on black,
 * and suspend after dismissal settles to avoid idle GPU work on Home Screen.
 */
static void
sb26_install_wallpaper_lifecycle(void)
{
    static int installed;
    if (installed) return;
    installed = 1;

    static const char *const present_names[] = {
        "SBCoverSheetWillPresentNotification",
        "SBLockScreenUIWillPresentNotification",
    };
    static const char *const dismiss_names[] = {
        "SBCoverSheetDidDismissNotification",
        "SBLockScreenUIDidDismissNotification",
    };

    for (unsigned i = 0; i < sizeof(present_names) / sizeof(present_names[0]); i++) {
        const char *reason = present_names[i];
        sb26_observe_notification(reason, ^(id note) {
            (void)note;
            if (pthread_main_np()) {
                sb26_resume_wallpaper(reason);
            } else {
                dispatch_async(dispatch_get_main_queue(), ^{
                    sb26_resume_wallpaper(reason);
                });
            }
        });
    }
    for (unsigned i = 0; i < sizeof(dismiss_names) / sizeof(dismiss_names[0]); i++) {
        const char *reason = dismiss_names[i];
        sb26_observe_notification(reason, ^(id note) {
            (void)note;
            if (pthread_main_np()) {
                sb26_schedule_wallpaper_suspend(reason);
            } else {
                dispatch_async(dispatch_get_main_queue(), ^{
                    sb26_schedule_wallpaper_suspend(reason);
                });
            }
        });
    }
    fprintf(stderr, "sb26: wallpaper lifecycle observers installed\n");
}

static void
sb26_reset_idle_timer(const char *reason)
{
    Class cls = objc_getClass("SBIdleTimerGlobalCoordinator");
    SEL shared = sel_registerName("sharedInstanceIfExists");
    if (!cls || !class_getClassMethod(cls, shared)) return;
    id coord = ((id (*)(Class, SEL))objc_msgSend)(cls, shared);
    if (!coord) return;
    SEL reset_sel = sel_registerName("resetIdleTimerForReason:");
    if (!class_respondsToSelector(object_getClass(coord), reset_sel)) return;
    CFStringRef cf_reason = CFStringCreateWithCString(kCFAllocatorDefault, reason, kCFStringEncodingUTF8);
    if (!cf_reason) return;
    ((void (*)(id, SEL, id))objc_msgSend)(coord, reset_sel, (id)cf_reason);
    CFRelease(cf_reason);
}

static void
wake_screen_for_unlock(void)
{
    sb26_reset_idle_timer("annibale-boot-unlock");
    Class bl_cls = objc_getClass("SBBacklightController");
    if (!bl_cls) return;
    SEL shared = sel_registerName("sharedInstanceIfExists");
    if (!class_getClassMethod(bl_cls, shared))
        shared = sel_registerName("sharedInstance");
    if (!class_getClassMethod(bl_cls, shared)) return;
    id bl = ((id (*)(Class, SEL))objc_msgSend)(bl_cls, shared);
    if (!bl) return;
    SEL turn_on = sel_registerName("turnOnScreenFullyWithBacklightSource:");
    if (class_getInstanceMethod(bl_cls, turn_on))
        ((void (*)(id, SEL, long))objc_msgSend)(bl, turn_on, 1);
    sb26_sync_initial_brightness(1);
}

static unsigned boot_unlock_wait_appear;

static id
object_if_responds(id object, const char *name)
{
    SEL selector = sel_registerName(name);
    if (!object || !class_respondsToSelector(object_getClass(object), selector)) return nil;
    return ((id (*)(id, SEL))objc_msgSend)(object, selector);
}

static id
home_screen_controller(void)
{
    Class cls = objc_getClass("SBIconController");
    SEL shared = sel_registerName("sharedInstanceIfExists");
    if (!cls || !class_getClassMethod(cls, shared)) return nil;
    return object_if_responds(((id (*)(Class, SEL))objc_msgSend)(cls, shared), "_mainDisplayHomeScreenController");
}

/*
 * SBHomeScreenController holds icons and widgets back while iconOcclusionReasons carries the lock
 * screen's bit 0x10. Two writers race at boot: the SBLockScreenUIWillDismissNotification handler
 * clears the bit, and the display layout publisher's observer queues a block to the main queue
 * with the mask of the layout it was called with, which sets the bit again when that layout still
 * held com.apple.lock-screen. On an iPhone the first layout is delivered long before anyone can
 * unlock. The unlock therefore waits until the published layout holds the lock screen's element.
 * 1: it does; 0: not yet; -1: this build has no such getters.
 */
static int
lock_screen_layout_published(void)
{
    id publisher = object_if_responds(object_if_responds(home_screen_controller(), "windowScene"),
        "displayLayoutPublisher");
    if (!publisher || !class_respondsToSelector(object_getClass(publisher), sel_registerName("currentLayout")))
        return -1;
    id elements = object_if_responds(object_if_responds(publisher, "currentLayout"), "elements");
    if (!elements || !class_respondsToSelector(object_getClass(elements), sel_registerName("objectAtIndex:")))
        return 0;
    unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(elements, sel_registerName("count"));
    for (unsigned long i = 0; i < count; i++) {
        id element = ((id (*)(id, SEL, unsigned long))objc_msgSend)(elements, sel_registerName("objectAtIndex:"), i);
        id identifier = object_if_responds(element, "identifier");
        const char *text = identifier
            ? ((const char *(*)(id, SEL))objc_msgSend)(identifier, sel_registerName("UTF8String")) : NULL;
        if (text && !strcmp(text, "com.apple.lock-screen")) return 1;
    }
    return 0;
}

/* What the race above left behind, a few seconds after the unlock: 0 is the settled home screen. */
static void
report_icon_occlusion(void *context)
{
    (void)context;
    id home = home_screen_controller();
    SEL reasons = sel_registerName("iconOcclusionReasons");
    if (!home || !class_respondsToSelector(object_getClass(home), reasons)) {
        fprintf(stderr, "sb26: icons: no home screen controller to read iconOcclusionReasons from\n");
        return;
    }
    fprintf(stderr, "sb26: icons: occlusion reasons %#llx, lock screen element in the layout %d\n",
        ((unsigned long long (*)(id, SEL))objc_msgSend)(home, reasons), lock_screen_layout_published());
}

#define LAYOUT_WAITS 20
static unsigned boot_unlock_wait_layout;
static int boot_unlock_layout_seen;

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
        return 0;
    }
    wake_screen_for_unlock();
    Class cspm_cls = objc_getClass("SBCoverSheetPresentationManager");
    SEL shared_if_exists = sel_registerName("sharedInstanceIfExists");
    id cspm = (cspm_cls && class_getClassMethod(cspm_cls, shared_if_exists))
        ? ((id (*)(Class, SEL))objc_msgSend)(cspm_cls, shared_if_exists) : nil;
    id sliding = nil;
    if (cspm) {
        SEL sliding_if_loaded_sel = sel_registerName("coverSheetSlidingViewControllerIfLoaded");
        SEL sliding_sel = sel_registerName("coverSheetSlidingViewController");
        if (class_respondsToSelector(object_getClass(cspm), sliding_if_loaded_sel)) {
            sliding = ((id (*)(id, SEL))objc_msgSend)(cspm, sliding_if_loaded_sel);
        } else if (class_respondsToSelector(object_getClass(cspm), sliding_sel)) {
            sliding = ((id (*)(id, SEL))objc_msgSend)(cspm, sliding_sel);
        }
    }
    SEL appear_sel = sel_registerName("_appearState");
    int32_t appear = (sliding && class_respondsToSelector(object_getClass(sliding), appear_sel))
        ? ((int32_t (*)(id, SEL))objc_msgSend)(sliding, appear_sel) : -1;
    if (appear != 2) {
        boot_unlock_wait_appear++;
        fprintf(stderr, "sb26: unlock %s: waiting for CoverSheet _appearState==2 (appear=%d wait=%u)\n",
            why, appear, boot_unlock_wait_appear);
        return 1;
    }
    int published = lock_screen_layout_published();
    if (published == 0 && boot_unlock_wait_layout < LAYOUT_WAITS) {
        boot_unlock_wait_layout++;
        fprintf(stderr, "sb26: unlock %s: waiting for the lock screen's layout element (wait=%u)\n",
            why, boot_unlock_wait_layout);
        return 1;
    }
    if (published == 1 && !boot_unlock_layout_seen) {
        /* The observer's block for this layout is on the main queue behind us: unlock on the next attempt. */
        boot_unlock_layout_seen = 1;
        return 1;
    }
    if (published != 1) {
        fprintf(stderr, "sb26: unlock %s: unlocking without the lock screen's layout element (state=%d wait=%u)\n",
            why, published, boot_unlock_wait_layout);
    }
    BOOL answer = ((BOOL (*)(id, SEL, long, id))objc_msgSend)(manager, unlock_ui, UNLOCK_SOURCE_COVER_SHEET, nil);
    int locked_after = ((BOOL (*)(id, SEL))objc_msgSend)(manager, locked);
    fprintf(stderr, "sb26: unlock %s: answer=%d locked_after=%d\n", why, answer, locked_after);
    if (!locked_after) {
        sb26_on_unlocked();
        return 2;
    }
    return 1;
}

static int boot_unlock_seen_locked;

static void
boot_unlock(void *context)
{
    uintptr_t attempt = (uintptr_t)context + 1;
    sb26_reset_idle_timer("annibale-boot-unlock");
    int state = unlock_once("boot");
    if (state >= 1) boot_unlock_seen_locked = 1;
    if (state == 2 || (state == 0 && boot_unlock_seen_locked)) {
        fprintf(stderr, "sb26: boot unlock done after %lu attempts\n", (unsigned long)attempt);
        sb26_on_unlocked();
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL,
            report_icon_occlusion);
        return;
    }
    if (attempt >= UNLOCK_ATTEMPTS) {
        fprintf(stderr, "sb26: boot unlock gave up after %lu attempts, lock screen seen=%d\n",
            (unsigned long)attempt, boot_unlock_seen_locked);
        return;
    }
    int64_t delay_ns = (state == 1) ? (NSEC_PER_SEC / 2) : (UNLOCK_EVERY_SECONDS * NSEC_PER_SEC);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, delay_ns),
        dispatch_get_main_queue(), (void *)attempt, boot_unlock);
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
    sb26_install_wallpaper_lifecycle();

    int token;
    notify_register_dispatch("com.annibale.unlock", &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        if (unlock_once("notification") == 0) {
            fprintf(stderr, "sb26: unlock notification: not locked\n");
            sb26_on_unlocked();
            return;
        }
        id manager = lock_screen_manager();
        if (manager && ((BOOL (*)(id, SEL))objc_msgSend)(manager, sel_registerName("isUILocked"))) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * (int64_t)NSEC_PER_MSEC),
                dispatch_get_main_queue(), ^{
                unlock_once("notification-retry");
            });
        }
    });

    if (!sb26_boot_arg_present("autounlock=1")) {
        fprintf(stderr, "sb26: autounlock!=1, the lock screen stays until it is swiped away\n");
        return;
    }
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, UNLOCK_FIRST_SECONDS * NSEC_PER_SEC),
        dispatch_get_main_queue(), NULL, boot_unlock);
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
 * Preferences and SpringBoard get true for MGCopyAnswer("airdrop") and ("airdrop-p2p"). The disk
 * capacity keys are MobileGestalt's own: it reads disk0's kIOMediaSizeKey and statfs("/private/var").
 */
extern CFTypeRef MGCopyAnswer(CFStringRef property);

static CFTypeRef
sb26_hook_MGCopyAnswer(CFStringRef property)
{
    const char *prog = getprogname();
    if (property && prog && (strcmp(prog, "Preferences") == 0 || strcmp(prog, "SpringBoard") == 0) &&
        (CFEqual(property, CFSTR("airdrop-p2p")) || CFEqual(property, CFSTR("airdrop")))) {
        return kCFBooleanTrue;
    }
    return MGCopyAnswer(property);
}

/*
 * Append Chinese language identifiers if the reported localizations contain no zh identifier
 * and the bundle contains localization resource files. This checks resource presence only;
 * it does not inspect which languages the files contain.
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

static uint64_t s_loc_cache[256];

static uint64_t
fnv1a64_str(const char *s)
{
    uint64_t h = 14695981039346656037ULL;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
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
    uint64_t tag = fnv1a64_str(path) & ~3ULL;
    if (tag == 0) tag = 4ULL;
    unsigned slot = (unsigned)((tag >> 2) & 255U);
    uint64_t cached = __atomic_load_n(&s_loc_cache[slot], __ATOMIC_RELAXED);
    if ((cached & ~3ULL) == tag && (cached & 3ULL) != 0) {
        return (cached & 3ULL) == 2ULL;
    }
    int found = 0;
    if (dir_contains_localization_file(path)) found = 1;
    else if (snprintf(sub, sizeof(sub), "%s/Resources", path) > 0 && dir_contains_localization_file(sub)) found = 1;
    else if (snprintf(sub, sizeof(sub), "%s/Contents/Resources", path) > 0 && dir_contains_localization_file(sub)) found = 1;
    __atomic_store_n(&s_loc_cache[slot], tag | (found ? 2ULL : 1ULL), __ATOMIC_RELAXED);
    return found;
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
        CFSTR("zh-Hans"), CFSTR("zh_CN"), CFSTR("zh_CN~iphone"), CFSTR("zh-Hans~iphone"),
        CFSTR("zh-Hant"), CFSTR("zh_TW"), CFSTR("zh_HK"),
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

static int
sb26_atomic_write_file(const char *target_path, const void *bytes, size_t len, mode_t mode, uid_t uid, gid_t gid)
{
    char tmp_path[PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", target_path, getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) return -1;

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;

    ssize_t written = write(fd, bytes, len);
    if (written < 0 || (size_t)written != len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);
    if (uid != (uid_t)-1 && gid != (gid_t)-1) {
        chown(tmp_path, uid, gid);
    }
    chmod(tmp_path, mode);
    if (rename(tmp_path, target_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/*
 * Ensure .GlobalPreferences.plist in /private/var/mobile/Library/Preferences/
 * has AppleLanguages with "zh-Hans-CN" prioritized.
 */
static void
sb26_ensure_chinese_preferences(void)
{
    const char *prog = getprogname();
    if (!prog || strcmp(prog, "SpringBoard") != 0) return;

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
                    } else {
                        free(buf);
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
            sb26_atomic_write_file(pref_path, CFDataGetBytePtr(xml_data), (size_t)CFDataGetLength(xml_data), 0644, 501, 501);
            CFRelease(xml_data);
        }
        CFPreferencesSetAppValue(CFSTR("AppleLanguages"), new_langs, kCFPreferencesAnyApplication);
        CFPreferencesSetAppValue(CFSTR("AppleLocale"), CFSTR("zh_CN"), kCFPreferencesAnyApplication);
        CFPreferencesAppSynchronize(kCFPreferencesAnyApplication);
        CFRelease(new_langs);
        fprintf(stderr, "sb26: AppleLanguages ensured zh-Hans-CN prioritized (updated)\n");
    }
    {
        CFDataRef sync_xml = CFPropertyListCreateData(kCFAllocatorDefault, dict, kCFPropertyListXMLFormat_v1_0, 0, NULL);
        if (sync_xml) {
            static const char *const sys_prefs[] = {
                "/private/var/preferences/.GlobalPreferences.plist",
                "/private/var/root/Library/Preferences/.GlobalPreferences.plist",
            };
            mkdir("/private/var/preferences", 0755);
            mkdir("/private/var/root", 0750);
            mkdir("/private/var/root/Library", 0750);
            mkdir("/private/var/root/Library/Preferences", 0750);
            for (unsigned i = 0; i < sizeof(sys_prefs) / sizeof(sys_prefs[0]); i++) {
                sb26_atomic_write_file(sys_prefs[i], CFDataGetBytePtr(sync_xml), (size_t)CFDataGetLength(sync_xml), 0644, (uid_t)-1, (gid_t)-1);
            }
            CFRelease(sync_xml);
        }
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
#include "inject_sb26_loc.inc"

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
    sysctlbyname("kern.annibale.halt", NULL, NULL, &one, sizeof(one));
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

    sb26_install_localization_hooks();
}

static void
sb26_log_self_uuid(const char *prog)
{
    Dl_info info;
    char uuid_str[40] = "none", line[160];

    if (dladdr((const void *)&sb26_log_self_uuid, &info) && info.dli_fbase) {
        const struct mach_header_64 *mh = (const struct mach_header_64 *)info.dli_fbase;
        if (mh->magic == MH_MAGIC_64) {
            const struct load_command *lc = (const struct load_command *)(mh + 1);
            for (uint32_t i = 0; i < mh->ncmds; i++) {
                if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
                    const uint8_t *u = ((const struct uuid_command *)lc)->uuid;
                    snprintf(uuid_str, sizeof(uuid_str),
                        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                        u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                        u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
                    break;
                }
                lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
            }
        }
    }
    snprintf(line, sizeof(line), "sb26: loaded in %s uuid=%s\n", prog, uuid_str);
    sysctlbyname("kern.annibale.say", NULL, NULL, line, strlen(line) + 1);
}

__attribute__((constructor)) static void
install_sb26(void)
{
    const char *prog = getprogname();
    if (!prog || (strcmp(prog, "SpringBoard") && strcmp(prog, "Preferences") &&
        strcmp(prog, "lsd") && strcmp(prog, "wifid"))) return;

    sb26_log_self_uuid(prog);
    if (!strcmp(prog, "SpringBoard")) {
        sb26_ensure_chinese_preferences();
        sb26_sync_initial_brightness(0);
    }
    sb26_clear_wifi_join_failures();
    sb26_install_localization_hooks();
    if (!strcmp(prog, "lsd") || !strcmp(prog, "wifid")) return;

    if (!is_build_23g90()) {
        fprintf(stderr, "sb26: skipped build mismatch or unreadable SystemVersion.plist\n");
        return;
    }

    _dyld_register_func_for_add_image(discovery_image_callback);
    Class airdrop_cls = objc_getClass("SFAirDropDiscoveryController");
    if (airdrop_cls) install_airdrop_hooks(airdrop_cls);
    Class session_cls = objc_getClass("AVOutputDeviceDiscoverySession");
    if (session_cls) install_screen_mirroring_hooks(session_cls);
    Class mru_cls = objc_getClass("MRUMirroringController");
    if (mru_cls) install_mru_mirroring_hooks(mru_cls);

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
    install_unlock();
    sb26_install_layer_dump();
    sb26_install_rate(slide);
    install_powerdown_hooks();
    sb26_install_localization_hooks();
}
