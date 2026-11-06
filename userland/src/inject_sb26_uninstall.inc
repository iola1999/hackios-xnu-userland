/*
 * inject_sb26_uninstall.inc: Home Screen and SpringBoard App Uninstallation Pipeline.
 * Pure C99 implementation using ObjC runtime and CoreFoundation primitives.
 * Wires up SpringBoard uninstall hooks to LSApplicationWorkspace unregisterApplication,
 * bundle container deletion (/private/var/containers/Bundle/Application/<UUID>),
 * and MCMAppDataContainer data container cleanup.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <notify.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SB26_BUNDLE_ROOT "/private/var/containers/Bundle/Application"
#define SB26_DATA_ROOT "/private/var/mobile/Containers/Data/Application"

static void (*orig_sb_app_ctrl_uninstall_opts)(id self, SEL cmd, id app, id opts, id completion);
static void (*orig_sb_app_ctrl_uninstall)(id self, SEL cmd, id app);
static BOOL (*orig_sb_app_is_deletable)(id self, SEL cmd);
static BOOL (*orig_sb_icon_is_uninstall_supported)(id self, SEL cmd);

static void
sb26_rmtree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *ent;
    char subpath[1024];
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        snprintf(subpath, sizeof(subpath), "%s/%s", path, ent->d_name);
        struct stat st;
        if (lstat(subpath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                sb26_rmtree(subpath);
            } else {
                unlink(subpath);
            }
        }
    }
    closedir(d);
    rmdir(path);
}

static CFDictionaryRef
sb26_load_plist(const char *plist_path)
{
    FILE *f = fopen(plist_path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 10 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    UInt8 *buf = (UInt8 *)malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read_bytes = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if ((long)read_bytes != len) {
        free(buf);
        return NULL;
    }
    CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, buf, (CFIndex)len, kCFAllocatorMalloc);
    if (!data) return NULL;
    CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
        kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        return (CFDictionaryRef)plist;
    }
    if (plist) CFRelease(plist);
    return NULL;
}

static int
sb26_perform_full_uninstall(const char *bundle_id)
{
    if (!bundle_id || !bundle_id[0]) return -1;
    fprintf(stderr, "sb26: performing full uninstall for bundle ID %s\n", bundle_id);

    char matched_bundle_container[1024] = "";
    char matched_app_path[1024] = "";

    /* 1. Locate bundle container from LSApplicationProxy or directory scan */
    Class proxy_cls = objc_getClass("LSApplicationProxy");
    if (proxy_cls) {
        SEL proxy_sel = sel_registerName("applicationProxyForIdentifier:");
        CFStringRef cf_id = CFStringCreateWithCString(kCFAllocatorDefault, bundle_id, kCFStringEncodingUTF8);
        if (cf_id) {
            id proxy = ((id (*)(Class, SEL, CFStringRef))objc_msgSend)(proxy_cls, proxy_sel, cf_id);
            if (proxy) {
                SEL url_sel = sel_registerName("bundleURL");
                id bundle_url = ((id (*)(id, SEL))objc_msgSend)(proxy, url_sel);
                if (bundle_url) {
                    SEL path_sel = sel_registerName("path");
                    CFStringRef cf_path = ((CFStringRef (*)(id, SEL))objc_msgSend)(bundle_url, path_sel);
                    if (cf_path) {
                        char path_buf[1024];
                        if (CFStringGetCString(cf_path, path_buf, sizeof(path_buf), kCFStringEncodingUTF8)) {
                            snprintf(matched_app_path, sizeof(matched_app_path), "%s", path_buf);
                            char *slash = strrchr(path_buf, '/');
                            if (slash) {
                                *slash = '\0';
                                snprintf(matched_bundle_container, sizeof(matched_bundle_container), "%s", path_buf);
                            }
                        }
                    }
                }
            }
            CFRelease(cf_id);
        }
    }

    /* Fallback scan /private/var/containers/Bundle/Application */
    if (!matched_bundle_container[0] || strncmp(matched_bundle_container, SB26_BUNDLE_ROOT, strlen(SB26_BUNDLE_ROOT)) != 0) {
        DIR *bundle_dir = opendir(SB26_BUNDLE_ROOT);
        if (bundle_dir) {
            struct dirent *ent;
            while ((ent = readdir(bundle_dir)) != NULL) {
                if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
                char dir_path[1024];
                snprintf(dir_path, sizeof(dir_path), "%s/%s", SB26_BUNDLE_ROOT, ent->d_name);
                DIR *sub = opendir(dir_path);
                if (!sub) continue;
                struct dirent *sub_ent;
                while ((sub_ent = readdir(sub)) != NULL) {
                    const char *dot = strrchr(sub_ent->d_name, '.');
                    if (dot && !strcmp(dot, ".app")) {
                        char plist_file[1024];
                        snprintf(plist_file, sizeof(plist_file), "%s/%s/Info.plist", dir_path, sub_ent->d_name);
                        CFDictionaryRef info = sb26_load_plist(plist_file);
                        if (info) {
                            CFStringRef bid = (CFStringRef)CFDictionaryGetValue(info, CFSTR("CFBundleIdentifier"));
                            if (bid && CFGetTypeID(bid) == CFStringGetTypeID()) {
                                char bid_buf[256];
                                if (CFStringGetCString(bid, bid_buf, sizeof(bid_buf), kCFStringEncodingUTF8) &&
                                    !strcmp(bid_buf, bundle_id)) {
                                    snprintf(matched_bundle_container, sizeof(matched_bundle_container), "%s", dir_path);
                                    snprintf(matched_app_path, sizeof(matched_app_path), "%s/%s", dir_path, sub_ent->d_name);
                                    CFRelease(info);
                                    break;
                                }
                            }
                            CFRelease(info);
                        }
                    }
                }
                closedir(sub);
                if (matched_bundle_container[0]) break;
            }
            closedir(bundle_dir);
        }
    }

    /* 2. Unregister application from LaunchServices */
    Class ws_cls = objc_getClass("LSApplicationWorkspace");
    Class url_cls = objc_getClass("NSURL");
    if (ws_cls && url_cls && matched_app_path[0]) {
        id ws = ((id (*)(Class, SEL))objc_msgSend)(ws_cls, sel_registerName("defaultWorkspace"));
        CFStringRef cf_path = CFStringCreateWithCString(kCFAllocatorDefault, matched_app_path, kCFStringEncodingUTF8);
        if (cf_path) {
            id bundle_url = ((id (*)(Class, SEL, CFStringRef))objc_msgSend)(url_cls, sel_registerName("fileURLWithPath:"), cf_path);
            if (ws && bundle_url) {
                BOOL unreg_ok = ((BOOL (*)(id, SEL, id))objc_msgSend)(ws, sel_registerName("unregisterApplication:"), bundle_url);
                fprintf(stderr, "sb26: LSApplicationWorkspace unregisterApplication: %s\n",
                    unreg_ok ? "OK" : "FAILED");
            }
            CFRelease(cf_path);
        }
    }

    /* 3. Delete application bundle directory */
    if (matched_bundle_container[0] &&
        strncmp(matched_bundle_container, SB26_BUNDLE_ROOT, strlen(SB26_BUNDLE_ROOT)) == 0) {
        sb26_rmtree(matched_bundle_container);
        fprintf(stderr, "sb26: removed bundle container at %s\n", matched_bundle_container);
    }

    /* 4. Delete data container via MCMAppDataContainer and direct filesystem scan */
    dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
    Class mcm_cls = objc_getClass("MCMAppDataContainer");
    if (mcm_cls) {
        CFStringRef cf_id = CFStringCreateWithCString(kCFAllocatorDefault, bundle_id, kCFStringEncodingUTF8);
        if (cf_id) {
            id container = ((id (*)(Class, SEL, CFStringRef, BOOL, BOOL *, void *))objc_msgSend)(
                mcm_cls, sel_registerName("containerWithIdentifier:createIfNecessary:existed:error:"),
                cf_id, NO, NULL, NULL);
            if (container) {
                id data_url = ((id (*)(id, SEL))objc_msgSend)(container, sel_registerName("url"));
                if (data_url) {
                    CFStringRef cf_dpath = ((CFStringRef (*)(id, SEL))objc_msgSend)(data_url, sel_registerName("path"));
                    if (cf_dpath) {
                        char dpath_buf[1024];
                        if (CFStringGetCString(cf_dpath, dpath_buf, sizeof(dpath_buf), kCFStringEncodingUTF8)) {
                            sb26_rmtree(dpath_buf);
                            fprintf(stderr, "sb26: removed MCM data container at %s\n", dpath_buf);
                        }
                    }
                }
            }
            CFRelease(cf_id);
        }
    }

    DIR *data_dir = opendir(SB26_DATA_ROOT);
    if (data_dir) {
        struct dirent *ent;
        while ((ent = readdir(data_dir)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            char meta_path[1024];
            snprintf(meta_path, sizeof(meta_path), "%s/%s/.com.apple.mobile_container_manager.metadata.plist",
                SB26_DATA_ROOT, ent->d_name);
            CFDictionaryRef meta = sb26_load_plist(meta_path);
            if (meta) {
                CFStringRef mid = (CFStringRef)CFDictionaryGetValue(meta, CFSTR("MCMMetadataIdentifier"));
                if (mid && CFGetTypeID(mid) == CFStringGetTypeID()) {
                    char mid_buf[256];
                    if (CFStringGetCString(mid, mid_buf, sizeof(mid_buf), kCFStringEncodingUTF8) &&
                        !strcmp(mid_buf, bundle_id)) {
                        char full_data_dir[1024];
                        snprintf(full_data_dir, sizeof(full_data_dir), "%s/%s", SB26_DATA_ROOT, ent->d_name);
                        sb26_rmtree(full_data_dir);
                        fprintf(stderr, "sb26: removed data container directory %s\n", full_data_dir);
                        CFRelease(meta);
                        break;
                    }
                }
                CFRelease(meta);
            }
        }
        closedir(data_dir);
    }

    /* 5. Trigger var_store persistence sync so uninstall persists across reboot */
    notify_post("com.redwood.var_save");
    fprintf(stderr, "sb26: posted com.redwood.var_save for %s uninstall\n", bundle_id);

    return 0;
}

static char *
sb26_extract_bundle_id(id target, char *buf, size_t buf_sz)
{
    if (!target || !buf || buf_sz == 0) return NULL;
    buf[0] = '\0';

    Class str_cls = objc_getClass("NSString");
    if (str_cls && ((BOOL (*)(id, SEL, Class))objc_msgSend)(target, sel_registerName("isKindOfClass:"), str_cls)) {
        if (CFStringGetCString((CFStringRef)target, buf, (CFIndex)buf_sz, kCFStringEncodingUTF8)) {
            return buf;
        }
    }

    SEL bid_sel = sel_registerName("bundleIdentifier");
    if (class_respondsToSelector(object_getClass(target), bid_sel)) {
        CFStringRef bid = ((CFStringRef (*)(id, SEL))objc_msgSend)(target, bid_sel);
        if (bid && CFGetTypeID(bid) == CFStringGetTypeID()) {
            if (CFStringGetCString(bid, buf, (CFIndex)buf_sz, kCFStringEncodingUTF8)) {
                return buf;
            }
        }
    }

    SEL app_sel = sel_registerName("application");
    if (class_respondsToSelector(object_getClass(target), app_sel)) {
        id app = ((id (*)(id, SEL))objc_msgSend)(target, app_sel);
        if (app && class_respondsToSelector(object_getClass(app), bid_sel)) {
            CFStringRef bid = ((CFStringRef (*)(id, SEL))objc_msgSend)(app, bid_sel);
            if (bid && CFGetTypeID(bid) == CFStringGetTypeID()) {
                if (CFStringGetCString(bid, buf, (CFIndex)buf_sz, kCFStringEncodingUTF8)) {
                    return buf;
                }
            }
        }
    }

    return NULL;
}

static void
sb26_uninstall_application_opts(id self, SEL cmd, id app, id opts, id completion)
{
    char bundle_id_buf[256] = "";
    char *bid = sb26_extract_bundle_id(app, bundle_id_buf, sizeof(bundle_id_buf));
    fprintf(stderr, "sb26: uninstallApplication:withOptions:completion: called for %s\n",
        bid ? bid : "unknown");

    if (bid) {
        sb26_perform_full_uninstall(bid);
    }

    if (orig_sb_app_ctrl_uninstall_opts) {
        orig_sb_app_ctrl_uninstall_opts(self, cmd, app, opts, completion);
    } else if (completion) {
        void (^blk)(BOOL, id) = (void (^)(BOOL, id))completion;
        blk(YES, nil);
    }
}

static void
sb26_uninstall_application_simple(id self, SEL cmd, id app)
{
    char bundle_id_buf[256] = "";
    char *bid = sb26_extract_bundle_id(app, bundle_id_buf, sizeof(bundle_id_buf));
    fprintf(stderr, "sb26: uninstallApplication: called for %s\n",
        bid ? bid : "unknown");

    if (bid) {
        sb26_perform_full_uninstall(bid);
    }

    if (orig_sb_app_ctrl_uninstall) {
        orig_sb_app_ctrl_uninstall(self, cmd, app);
    }
}

static BOOL
sb26_app_is_deletable(id self, SEL cmd)
{
    SEL burl_sel = sel_registerName("bundleURL");
    if (class_respondsToSelector(object_getClass(self), burl_sel)) {
        id url = ((id (*)(id, SEL))objc_msgSend)(self, burl_sel);
        if (url) {
            SEL path_sel = sel_registerName("path");
            CFStringRef path = ((CFStringRef (*)(id, SEL))objc_msgSend)(url, path_sel);
            if (path && CFGetTypeID(path) == CFStringGetTypeID()) {
                char pbuf[512];
                if (CFStringGetCString(path, pbuf, sizeof(pbuf), kCFStringEncodingUTF8) &&
                    !strncmp(pbuf, SB26_BUNDLE_ROOT, strlen(SB26_BUNDLE_ROOT))) {
                    return YES;
                }
            }
        }
    }
    if (orig_sb_app_is_deletable) {
        return orig_sb_app_is_deletable(self, cmd);
    }
    return YES;
}

static BOOL
sb26_icon_is_uninstall_supported(id self, SEL cmd)
{
    SEL app_sel = sel_registerName("application");
    if (class_respondsToSelector(object_getClass(self), app_sel)) {
        id app = ((id (*)(id, SEL))objc_msgSend)(self, app_sel);
        if (app) {
            SEL burl_sel = sel_registerName("bundleURL");
            if (class_respondsToSelector(object_getClass(app), burl_sel)) {
                id url = ((id (*)(id, SEL))objc_msgSend)(app, burl_sel);
                if (url) {
                    SEL path_sel = sel_registerName("path");
                    CFStringRef path = ((CFStringRef (*)(id, SEL))objc_msgSend)(url, path_sel);
                    if (path && CFGetTypeID(path) == CFStringGetTypeID()) {
                        char pbuf[512];
                        if (CFStringGetCString(path, pbuf, sizeof(pbuf), kCFStringEncodingUTF8) &&
                            !strncmp(pbuf, SB26_BUNDLE_ROOT, strlen(SB26_BUNDLE_ROOT))) {
                            return YES;
                        }
                    }
                }
            }
        }
    }
    if (orig_sb_icon_is_uninstall_supported) {
        return orig_sb_icon_is_uninstall_supported(self, cmd);
    }
    return YES;
}

static void
install_uninstall_hooks(void)
{
    static int uninstall_done = 0;
    if (uninstall_done) return;

    Class app_ctrl_cls = objc_getClass("SBApplicationController");
    if (app_ctrl_cls) {
        Method m_opts = class_getInstanceMethod(app_ctrl_cls,
            sel_registerName("uninstallApplication:withOptions:completion:"));
        if (m_opts) {
            orig_sb_app_ctrl_uninstall_opts = (void (*)(id, SEL, id, id, id))method_getImplementation(m_opts);
            method_setImplementation(m_opts, (IMP)sb26_uninstall_application_opts);
        }
        Method m_simple = class_getInstanceMethod(app_ctrl_cls,
            sel_registerName("uninstallApplication:"));
        if (m_simple) {
            orig_sb_app_ctrl_uninstall = (void (*)(id, SEL, id))method_getImplementation(m_simple);
            method_setImplementation(m_simple, (IMP)sb26_uninstall_application_simple);
        }
        fprintf(stderr, "sb26: SBApplicationController uninstall hooks installed\n");
    }

    Class app_cls = objc_getClass("SBApplication");
    if (app_cls) {
        Method m_del = class_getInstanceMethod(app_cls, sel_registerName("isDeletable"));
        if (m_del) {
            orig_sb_app_is_deletable = (BOOL (*)(id, SEL))method_getImplementation(m_del);
            method_setImplementation(m_del, (IMP)sb26_app_is_deletable);
            fprintf(stderr, "sb26: SBApplication isDeletable hook installed\n");
        }
    }

    Class icon_cls = objc_getClass("SBApplicationIcon");
    if (icon_cls) {
        Method m_un = class_getInstanceMethod(icon_cls, sel_registerName("isUninstallSupported"));
        if (m_un) {
            orig_sb_icon_is_uninstall_supported = (BOOL (*)(id, SEL))method_getImplementation(m_un);
            method_setImplementation(m_un, (IMP)sb26_icon_is_uninstall_supported);
            fprintf(stderr, "sb26: SBApplicationIcon isUninstallSupported hook installed\n");
        }
    }

    uninstall_done = 1;
}
