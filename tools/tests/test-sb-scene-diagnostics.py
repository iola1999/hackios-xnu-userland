#!/usr/bin/env python3
"""Run the production scene dump against Objective-C objects with measured and invalid ABIs."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
static Class mock_sdf_class;
static id test_objc_getClass(const char *name)
{
    if (mock_sdf_class && !strcmp(name, "CASDFElementLayer")) return (id)mock_sdf_class;
    return objc_getClass(name);
}
#define objc_getClass test_objc_getClass
#include SCENE_SOURCE
#undef objc_getClass

static id scene_value, process_value, settings_value, identifier_value, layers_value;
static int bad_calls, failures;
static BOOL flags_enabled = YES;

static id object_value(id self, SEL selector)
{
    (void)self;
    const char *name = sel_getName(selector);
    if (!strcmp(name, "scene")) return scene_value;
    if (!strcmp(name, "clientProcess")) return process_value;
    if (!strcmp(name, "settings")) return settings_value;
    if (!strcmp(name, "identifier")) return identifier_value;
    if (!strcmp(name, "layerManager")) return scene_value;
    if (!strcmp(name, "layers")) return layers_value;
    if (!strcmp(name, "mode") || !strcmp(name, "operation")) return @"mock";
    return nil;
}

static BOOL flag_value(id self, SEL selector)
{
    (void)self;
    const char *name = sel_getName(selector);
    return flags_enabled && strcmp(name, "isActive") && strcmp(name, "isPendingExit");
}

static long long signed_value(id self, SEL selector)
{
    (void)self;
    if (!strcmp(sel_getName(selector), "versionedPID")) return (1LL << 40) + 7;
    return 4;
}

static unsigned long long count_value(id self, SEL selector)
{
    (void)self; (void)selector;
    return 3;
}

static signed char activity_value(id self, SEL selector)
{
    (void)self; (void)selector;
    return -50;
}

static double double_value(id self, SEL selector)
{
    (void)self; (void)selector;
    return 0.25;
}

static int bad_value(id self, SEL selector)
{
    (void)self; (void)selector;
    bad_calls++;
    return 0;
}

static long long bad_argument(id self, SEL selector, id argument)
{
    (void)self; (void)selector; (void)argument;
    bad_calls++;
    return 0;
}

static Class new_class(const char *name, Class superclass)
{
    Class cls = objc_allocateClassPair(superclass, name, 0);
    if (!cls) abort();
    return cls;
}

static void add(Class cls, const char *name, IMP implementation, const char *type)
{
    if (!class_addMethod(cls, sel_registerName(name), implementation, type)) abort();
}

static id instance(Class cls)
{
    objc_registerClassPair(cls);
    return [[cls alloc] init];
}

static void expect(const char *label, BOOL condition)
{
    printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    failures += !condition;
}

static char *capture(id view, BOOL sdf)
{
    dump_fp = tmpfile();
    if (!dump_fp) abort();
    lines = 0;
    if (sdf) describe_sdf(view, 2);
    else describe_scene(view, 2);
    fflush(dump_fp);
    long size = ftell(dump_fp);
    rewind(dump_fp);
    char *text = calloc((size_t)size + 1, 1);
    if (!text || fread(text, 1, (size_t)size, dump_fp) != (size_t)size) abort();
    fclose(dump_fp);
    dump_fp = NULL;
    return text;
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        if (argc != 2) return 2;
        const char *scenario = argv[1];
        BOOL missing = !strcmp(scenario, "missing-scalars");
        BOOL wrong = !strcmp(scenario, "wrong-scalars");
        BOOL wrong_objects = !strcmp(scenario, "wrong-objects");
        BOOL wrong_arguments = !strcmp(scenario, "wrong-arguments");
        flags_enabled = strcmp(scenario, "false-flags") != 0;
        Class base = [NSObject class];
        Class process = new_class("SceneTestProcess", base);
        Class settings = new_class("SceneTestSettings", base);
        Class scene = new_class("SceneTestScene", base);
        Class layers = new_class("SceneTestLayers", base);
        Class view = new_class("SceneTestView", base);
        if (!missing) {
            const char *flags[] = {"isValid", "isActive", "finishedLaunching", "isPendingExit", "isForeground"};
            Class owners[] = {scene, scene, process, process, settings};
            for (unsigned i = 0; i < 5; i++)
                add(owners[i], flags[i], wrong ? (IMP)bad_value : (IMP)flag_value,
                    wrong ? "i16@0:8" : "B16@0:8");
            const char *numbers[] = {"contentState", "taskState", "versionedPID"};
            Class owners_number[] = {scene, process, process};
            for (unsigned i = 0; i < 3; i++)
                add(owners_number[i], numbers[i], wrong ? (IMP)bad_value : wrong_arguments ? (IMP)bad_argument : (IMP)signed_value,
                    wrong ? "i16@0:8" : wrong_arguments ? "q24@0:8@16" : "q16@0:8");
            add(settings, "activityMode", wrong ? (IMP)bad_value : (IMP)activity_value,
                wrong ? "i16@0:8" : "c16@0:8");
            add(layers, "count", wrong ? (IMP)bad_value : (IMP)count_value,
                wrong ? "i16@0:8" : "Q16@0:8");
        }
        const char *objects[] = {"clientProcess", "settings", "identifier", "layerManager", "layers"};
        for (unsigned i = 0; i < 5; i++)
            add(scene, objects[i], wrong_objects ? (IMP)bad_value : (IMP)object_value,
                wrong_objects ? "i16@0:8" : "@16@0:8");
        if (strcmp(scenario, "missing-scene"))
            add(view, "scene", !strcmp(scenario, "wrong-scene") ? (IMP)bad_value : (IMP)object_value,
                !strcmp(scenario, "wrong-scene") ? "i16@0:8" : "@16@0:8");
        process_value = instance(process);
        settings_value = instance(settings);
        layers_value = instance(layers);
        scene_value = instance(scene);
        if (!strcmp(scenario, "inherited"))
            scene_value = instance(new_class("SceneTestDerived", scene));
        identifier_value = @"scene-test\nidentifier";
        if (!strcmp(scenario, "nil-children")) process_value = settings_value = identifier_value = nil;
        if (!strcmp(scenario, "no-scene")) scene_value = nil;
        id object = instance(view);
        BOOL sdf = !strcmp(scenario, "sdf");
        if (sdf) {
            Class cls = new_class("SceneTestSDFElement", base);
            add(cls, "mode", (IMP)object_value, "@16@0:8");
            add(cls, "operation", (IMP)object_value, "@16@0:8");
            add(cls, "contentsZeroValueDistance", (IMP)double_value, "d16@0:8");
            add(cls, "hitTestsAsFill", (IMP)flag_value, "B16@0:8");
            object = instance(cls);
            mock_sdf_class = cls;
        }
        char *text = capture(object, sdf);
        if (sdf) {
            expect("SDF string and double ABI preserved", strstr(text, "mode=mock operation=mock zeroDistance=0.25") != NULL);
            expect("SDF BOOL ABI preserved", strstr(text, "hitTestsAsFill=1") != NULL);
        } else if (!strcmp(scenario, "no-scene") || !strcmp(scenario, "missing-scene") || !strcmp(scenario, "wrong-scene")) {
            expect("no hosted scene has no output", !text[0] && lines == 0);
        } else {
            expect("one scene line", lines == 1);
            expect("unavailable boolean never becomes 255", !strstr(text, "valid 255") && !strstr(text, "active 255"));
            if (missing || wrong) {
                expect("unknown scene values", strstr(text, "valid -1 active -1 content -1 layers -1") != NULL);
                expect("unknown process values", strstr(text, "finishedLaunching -1 isPendingExit -1 taskState -1 versionedPID -1") != NULL);
                expect("unknown settings values", strstr(text, "isForeground -1 activityMode -1") != NULL);
            } else {
                expect("scene booleans", strstr(text, flags_enabled ? "valid 1 active 0" : "valid 0 active 0") != NULL);
                if (wrong_arguments) {
                    expect("unexpected argument ABI rejected", strstr(text, "content -1") && strstr(text, "taskState -1 versionedPID -1"));
                } else if (!wrong_objects && strcmp(scenario, "nil-children")) {
                    expect("64 bit values retained", strstr(text, "taskState 4 versionedPID 1099511627783") != NULL);
                    expect("signed activity mode retained", strstr(text, "activityMode -50") != NULL);
                    expect("process booleans", strstr(text, flags_enabled ? "finishedLaunching 1 isPendingExit 0" : "finishedLaunching 0 isPendingExit 0") != NULL);
                    expect("settings boolean", strstr(text, flags_enabled ? "isForeground 1" : "isForeground 0") != NULL);
                }
                if (wrong_objects || !strcmp(scenario, "nil-children")) {
                    expect("unavailable scene ID", strstr(text, "sceneID unavailable") != NULL);
                    expect("unavailable process and settings", strstr(text, "finishedLaunching -1 isPendingExit -1 taskState -1 versionedPID -1 isForeground -1 activityMode -1") != NULL);
                }
            }
            if (!wrong_objects && strcmp(scenario, "nil-children"))
                expect("identifier stays on one line", strstr(text, "sceneID scene-test identifier") != NULL);
        }
        expect("mismatched getters were not called", bad_calls == 0);
        free(text);
        return failures ? 1 : 0;
    }
}
'''


class SceneDiagnostics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="sb-scene-")
        base = Path(cls.tmp.name)
        source = ROOT / "userland/src/inject/inject_sb26_layers.c"
        harness = base / "scene-diagnostics.m"
        harness.write_text(HARNESS)
        cls.binary = base / "scene-diagnostics"
        result = subprocess.run(["xcrun", "clang", "-fblocks", "-Wall", "-Wextra", "-Werror",
                                 "-framework", "Foundation", "-framework", "CoreFoundation",
                                 f'-DSCENE_SOURCE="{source}"', str(harness), "-o", str(cls.binary)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_runtime_contract(self):
        for scenario in ("valid", "false-flags", "inherited", "missing-scalars", "wrong-scalars",
                         "wrong-objects", "wrong-arguments", "nil-children", "no-scene",
                         "missing-scene", "wrong-scene", "sdf"):
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.binary), scenario], capture_output=True, text=True, timeout=10)
                print(f"{scenario}:\n{result.stdout}", end="")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args, remaining = parser.parse_known_args()
    ROOT = args.source_root.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
