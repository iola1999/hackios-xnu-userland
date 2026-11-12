#!/usr/bin/env python3
"""执行 SpringBoard 注入初始化，检查卸载仍由原生方法处理。

宿主使用 Objective-C 替身记录调用与延迟回调；安装服务和设备容器另行真机验证。
--source-root 可指向旧版源码副本，执行相同场景。
"""

import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = source.index("static void\n" + name + "(")
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PREFIX = r'''
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <mach-o/loader.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static const char *program = "SpringBoard";
static int matches_build = 1, callbacks, uuid_calls, prefs_calls, wifi_calls;
static int brightness_calls, loc_calls, bitmap_calls, unlock_calls, layer_calls;
static int rate_calls, power_calls, delete_calls, unregister_calls, sync_calls;
static int native_calls, request_calls, completion_calls;
static BOOL allow_delete, bundle_present = YES, data_present = YES;
static NSUInteger captured_options;
static id captured_app;
static void (^pending)(NSError *);
static NSError *failure;

@interface SBApplication : NSObject
- (NSString *)bundleIdentifier;
- (NSURL *)bundleURL;
- (BOOL)iconSupportsUninstall:(id)icon;
@end
@implementation SBApplication
- (NSString *)bundleIdentifier { return @"org.example.uninstall-test"; }
- (NSURL *)bundleURL { return [NSURL fileURLWithPath:@"/private/var/containers/Bundle/Application/test/App.app"]; }
- (BOOL)iconSupportsUninstall:(id)icon { (void)icon; return allow_delete; }
@end
@interface SBLeafIcon : NSObject
- (id)application;
- (BOOL)isUninstallSupported;
@end
@implementation SBLeafIcon
- (id)application { return [SBApplication new]; }
- (BOOL)isUninstallSupported { return [[self application] iconSupportsUninstall:self]; }
@end
@interface SBApplicationIcon : SBLeafIcon @end
@implementation SBApplicationIcon @end

@interface SBApplicationController : NSObject
- (void)uninstallApplication:(id)app;
- (void)requestUninstallApplication:(id)app options:(NSUInteger)options withCompletion:(void (^)(NSError *))completion;
@end
@implementation SBApplicationController
- (void)uninstallApplication:(id)app {
    native_calls++;
    captured_app = app;
    assert(delete_calls == 0 && unregister_calls == 0 && sync_calls == 0);
    if (!failure) { bundle_present = NO; data_present = NO; }
}
- (void)requestUninstallApplication:(id)app options:(NSUInteger)options withCompletion:(void (^)(NSError *))completion {
    request_calls++;
    captured_app = app;
    captured_options = options;
    pending = [completion copy];
}
@end

/* 旧版对照只记录文件操作；测试进程不访问设备容器。 */
@interface TestLSApplicationProxy : NSObject
+ (id)applicationProxyForIdentifier:(id)identifier;
- (NSURL *)bundleURL;
@end
@implementation TestLSApplicationProxy
+ (id)applicationProxyForIdentifier:(id)identifier { (void)identifier; return [self new]; }
- (NSURL *)bundleURL { return [[SBApplication new] bundleURL]; }
@end
@interface TestLSApplicationWorkspace : NSObject
+ (id)defaultWorkspace;
- (BOOL)unregisterApplication:(id)url;
@end
@implementation TestLSApplicationWorkspace
+ (id)defaultWorkspace { return [self new]; }
- (BOOL)unregisterApplication:(id)url { (void)url; unregister_calls++; return NO; }
@end
static Class record_objc_getClass(const char *name) {
    if (!strcmp(name, "LSApplicationProxy")) return TestLSApplicationProxy.class;
    if (!strcmp(name, "LSApplicationWorkspace")) return TestLSApplicationWorkspace.class;
    if (!strcmp(name, "MCMAppDataContainer")) return Nil;
    return objc_getClass(name);
}
static DIR *record_opendir(const char *path) { (void)path; return NULL; }
static int record_unlink(const char *path) { (void)path; delete_calls++; return -1; }
static int record_rmdir(const char *path) { (void)path; delete_calls++; return -1; }
static void record_sync(void) { sync_calls++; }
static void *record_dlopen(const char *path, int flags) { (void)path; (void)flags; return NULL; }

static const char *test_getprogname(void) { return program; }
#define getprogname test_getprogname
static void sb26_log_self_uuid(const char *p) { (void)p; uuid_calls++; }
static void sb26_ensure_chinese_preferences(void) { prefs_calls++; }
static void sb26_clear_wifi_join_failures(void) { wifi_calls++; }
static void sb26_sync_initial_brightness(int value) { (void)value; brightness_calls++; }
static void sb26_install_localization_hooks(void) { loc_calls++; }
static int is_build_23g90(void) { return matches_build; }
static void (*image_callback)(const struct mach_header *, intptr_t);
static void _dyld_register_func_for_add_image(void (*cb)(const struct mach_header *, intptr_t)) {
    callbacks++;
    image_callback = cb;
    cb(NULL, 0);
}
static const void *_dyld_get_shared_cache_range(size_t *size) {
    *size = 0x10000;
    return (void *)0x180000000UL;
}
static Method checked_method(const char *c, const char *s, const char *e, uintptr_t a, const unsigned char b[16]) {
    (void)c; (void)s; (void)e; (void)a; (void)b; return NULL;
}
static void setup_activation_state(id s, SEL c) { (void)s; (void)c; }
static uintptr_t code_address(IMP p) { return (uintptr_t)p; }
static void install_bitmap_selection(uintptr_t slide) { (void)slide; bitmap_calls++; }
static void install_unlock(void) { unlock_calls++; }
static void sb26_install_layer_dump(void) { layer_calls++; }
static void sb26_install_rate(uintptr_t slide) { (void)slide; rate_calls++; }
static void install_powerdown_hooks(void) { power_calls++; }
static void install_airdrop_hooks(Class c) { (void)c; }
static void install_screen_mirroring_hooks(Class c) { (void)c; }
static void install_mru_mirroring_hooks(Class c) { (void)c; }
static void install_calendar_icon_hooks(Class c) { (void)c; }
'''

MAIN = r'''
int main(int argc, char **argv) {
    @autoreleasepool {
        assert(argc == 2);
        const char *scenario = argv[1];
        if (!strcmp(scenario, "preferences")) program = "Preferences";
        if (!strcmp(scenario, "lsd")) program = "lsd";
        if (!strcmp(scenario, "wifid")) program = "wifid";
        if (!strcmp(scenario, "unrelated")) program = "OtherProcess";
        if (!strcmp(scenario, "build-mismatch")) matches_build = 0;
        Method simple = class_getInstanceMethod(SBApplicationController.class, @selector(uninstallApplication:));
        Method request = class_getInstanceMethod(SBApplicationController.class, @selector(requestUninstallApplication:options:withCompletion:));
        Method icon = class_getInstanceMethod(SBLeafIcon.class, @selector(isUninstallSupported));
        IMP simple_imp = method_getImplementation(simple);
        IMP request_imp = method_getImplementation(request);
        IMP icon_imp = method_getImplementation(icon);
        install_sb26();
        BOOL springboard = !strcmp(program, "SpringBoard") && matches_build;
        if (!strcmp(scenario, "initialization")) {
            assert(uuid_calls == 1 && prefs_calls == 1 && wifi_calls == 1);
            assert(brightness_calls == 1 && callbacks == 1 && loc_calls == 3);
            assert(bitmap_calls == 1 && unlock_calls == 1 && layer_calls == 1);
            assert(rate_calls == 1 && power_calls == 1);
        } else if (!strcmp(scenario, "preferences")) {
            assert(uuid_calls == 1 && callbacks == 1 && loc_calls == 2);
            assert(brightness_calls == 0 && power_calls == 0);
        } else if (!strcmp(scenario, "lsd") || !strcmp(scenario, "wifid")) {
            assert(uuid_calls == 1 && prefs_calls == 1 && loc_calls == 1);
            assert(callbacks == 0 && brightness_calls == 0 && layer_calls == 0);
        } else if (!strcmp(scenario, "unrelated")) {
            assert(uuid_calls == 0 && prefs_calls == 0 && callbacks == 0);
        } else if (!strcmp(scenario, "build-mismatch")) {
            assert(uuid_calls == 1 && brightness_calls == 1 && loc_calls == 1);
            assert(callbacks == 0 && bitmap_calls == 0 && power_calls == 0);
        } else if (!strcmp(scenario, "methods")) {
            assert(springboard && image_callback);
            image_callback(NULL, 0);
            image_callback(NULL, 0);
            assert(method_getImplementation(simple) == simple_imp);
            assert(method_getImplementation(request) == request_imp);
            assert(method_getImplementation(icon) == icon_imp);
            assert(![SBApplicationController instancesRespondToSelector:sel_registerName("uninstallApplication:withOptions:completion:")]);
        } else if (!strcmp(scenario, "eligibility")) {
            SBApplicationIcon *app_icon = [SBApplicationIcon new];
            allow_delete = NO;
            assert(![app_icon isUninstallSupported]);
            allow_delete = YES;
            assert([app_icon isUninstallSupported]);
        } else {
            SBApplication *app = [SBApplication new];
            SBApplicationController *controller = [SBApplicationController new];
            BOOL fail = strstr(scenario, "failure") != NULL;
            failure = fail ? [NSError errorWithDomain:@"NativeInstallService" code:27 userInfo:nil] : nil;
            if (!strncmp(scenario, "simple-", 7)) {
                [controller uninstallApplication:app];
                assert(native_calls == 1 && request_calls == 0 && captured_app == app);
            } else {
                NSUInteger options = 0x100000001UL;
                void (^completion)(NSError *) = ^(NSError *error) {
                    completion_calls++;
                    assert(error == failure);
                };
                [controller requestUninstallApplication:app options:options withCompletion:completion];
                assert(request_calls == 1 && native_calls == 0 && captured_app == app);
                assert(captured_options == options && pending == completion);
                assert(completion_calls == 0 && bundle_present && data_present);
                if (!failure) { bundle_present = NO; data_present = NO; }
                pending(failure);
                assert(completion_calls == 1);
            }
            assert(delete_calls == 0 && unregister_calls == 0 && sync_calls == 0);
            assert(bundle_present == fail && data_present == fail);
        }
    }
    return 0;
}
'''


class NativeUninstall(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="sb-uninstall-")
        base = Path(cls.tmp.name)
        inject = ROOT / "userland/src/inject"
        source = (inject / "inject_sb26.c").read_text()
        previous = ""
        if '#include "inject_sb26_uninstall.inc"' in source:
            previous = "\n#define objc_getClass record_objc_getClass\n#define opendir record_opendir\n#define unlink record_unlink\n#define rmdir record_rmdir\n#define sync record_sync\n#define dlopen record_dlopen\n"
            previous += (inject / "inject_sb26_uninstall.inc").read_text()
            previous += "\n#undef objc_getClass\n#undef opendir\n#undef unlink\n#undef rmdir\n#undef sync\n#undef dlopen\n"
        body = PREFIX + previous + function(source, "discovery_image_callback") + "\n"
        body += function(source, "install_sb26") + MAIN
        path = base / "native-uninstall.m"
        path.write_text(body)
        cls.binary = base / "native-uninstall"
        subprocess.run(["xcrun", "clang", "-fblocks", "-Werror", "-Wno-unused-function",
                        "-framework", "Foundation", "-framework", "CoreFoundation",
                        str(path), "-o", str(cls.binary)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_runtime_contract(self):
        scenarios = ("initialization", "preferences", "lsd", "wifid", "unrelated",
                     "build-mismatch", "methods", "eligibility", "simple-success",
                     "simple-failure", "request-success", "request-failure")
        for scenario in scenarios:
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.binary), scenario], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args, remaining = parser.parse_known_args()
    ROOT = args.source_root.resolve()
    unittest.main(argv=[__file__, *remaining])
