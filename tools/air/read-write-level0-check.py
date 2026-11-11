#!/usr/bin/env python3
"""Build the level-zero read/write probe and check AIR signatures with --air2nir.

Optional --ir3 also compiles for A830; --baseline checks the MPSarray frontend
before this fix. --original accepts the separately extracted MPS apply module.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--air2nir", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--ir3", type=Path)
    parser.add_argument("--original", type=Path)
    parser.add_argument("--llvm-as", default="llvm-as")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    here = Path(__file__).resolve().parent
    records = []

    def run(argv, name, expected=0):
        result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=60)
        text = result.stdout + result.stderr
        (out / (name + ".log")).write_text(text)
        records.append({"name": name, "exit": result.returncode, "expected": expected})
        if result.returncode != expected:
            raise RuntimeError(f"{name}: exit {result.returncode}, expected {expected}")
        return text

    try:
        for suffix in (".m", ".metal"):
            shutil.copy2(here / ("read-write-level0" + suffix), out / ("probe" + suffix))
        for sdk, target, arch, minimum in (
            ("macosx", "air64_v28-apple-macos26.0.0", "arm64", "-mmacosx-version-min=26.0"),
            ("iphoneos", "air64_v28-apple-ios26.6.0", "arm64e", "-miphoneos-version-min=26.0"),
        ):
            run(["xcrun", "--sdk", sdk, "metal", "-std=metal3.2", "-target", target,
                 "-c", out / "probe.metal", "-o", out / (sdk + ".air")], sdk + "-compile")
            run(["xcrun", "--sdk", sdk, "metallib", out / (sdk + ".air"),
                 "-o", out / (sdk + ".metallib")], sdk + "-library")
            run(["xcrun", "--sdk", sdk, "clang", "-arch", arch, minimum, "-O1", "-Wall", "-Wextra",
                 "-Werror", out / "probe.m", "-framework", "Foundation", "-framework", "Metal",
                 "-o", out / (sdk + "-probe")], sdk + "-probe")
        run(["codesign", "--force", "--sign", "-", out / "iphoneos-probe"], "sign")
        run(["codesign", "--verify", "--strict", out / "iphoneos-probe"], "verify-signature")
        native = run([out / "macosx-probe", out / "macosx.metallib"], "native")
        if "RESULT cases=8 components=4032 errors=0" not in native or native.count("readback_errors=0 status=4") != 8:
            raise RuntimeError("native output lacks the eight completed comparisons")
        run(["xcrun", "--sdk", "macosx", "metal", "-std=metal3.2", "-DOMIT_WRITE",
             "-target", "air64_v28-apple-macos26.0.0", "-c", out / "probe.metal",
             "-o", out / "omit.air"], "omit-compile")
        run(["xcrun", "--sdk", "macosx", "metallib", out / "omit.air",
             "-o", out / "omit.metallib"], "omit-library")
        omitted = run([out / "macosx-probe", out / "omit.metallib"], "omit-write", 1)
        if "RESULT cases=8 components=4032 errors=2688" not in omitted:
            raise RuntimeError("omitting the write did not fail subsequent reads and CPU readback")

        spec = importlib.util.spec_from_file_location("metallib", here.parent / "metallib.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        data, header, functions = module.parse(out / "iphoneos.metallib")
        if len(functions) != 16:
            raise RuntimeError("expected eight read/write and eight independent read-only functions")
        for function in functions:
            name = function["name"]
            path = out / (name + ".bc")
            path.write_bytes(module.bitcode_of(data, header, function)[0])
            text = run([args.air2nir, path], name + "-nir")
            required = ("image_load", "image_store") if name.startswith("rw_") else ("txf",)
            if not all(op in text for op in required):
                raise RuntimeError(f"{name}: missing expected load/store operation")
            if args.ir3:
                run([args.ir3, path, name, out / (name + ".ir3.bin")], name + "-ir3")
            if args.baseline:
                run([args.baseline, path], name + "-baseline", 1 if name.startswith("rw_") else 0)
        ll = run([args.air2nir, "--ll", out / "rw_array_float_uint.bc"], "fixture-llvm")
        if "@air.get_read_sampler" not in ll:
            raise RuntimeError("compiler output has no builtin read sampler")
        call = next(line for line in ll.splitlines() if "tail call" in line and "@air.read_texture" in line)
        decl = next(line for line in ll.splitlines() if line.startswith("declare") and "@air.read_texture" in line)
        layer = re.search(r", i32 (%\d+), <2 x i32>", call)[1]
        invalid = {
            "write-only": (ll.replace('!"air.read_write"', '!"air.write"'), "requires read_write"),
            "read-only": (ll.replace('!"air.read_write"', '!"air.read"'), "requires sample or read"),
            "access2": (ll.replace(call, call.replace("i32 3)", "i32 2)")), "requires read_write"),
            "offset": (ll.replace(call, call.replace("zeroinitializer", "<i32 1, i32 0>")), "constant zero offset"),
            "coord-type": (ll.replace(call, re.sub(r"<2 x i32> %\d+, i32", "<2 x float> zeroinitializer, i32", call, count=1))
                           .replace(decl, decl.replace("<2 x i32>, i32,", "<2 x float>, i32,", 1)), "invalid coordinate"),
            "layer-type": (ll.replace(call, call.replace("i32 " + layer + ", <2 x i32>", "float 0.0, <2 x i32>"))
                           .replace(decl, decl.replace("<2 x i32>, i32, <2 x i32>", "<2 x i32>, float, <2 x i32>")),
                           "i16 or i32 array layer"),
            "lod-type": (ll.replace(call, call.replace("i32 0, i32 3)", "float 0.0, i32 3)"))
                         .replace(decl, decl.replace("<2 x i32>, i32, i32)", "<2 x i32>, float, i32)")), "invalid coordinate"),
            "lod-one": (ll.replace(call, call.replace("i32 0, i32 3)", "i32 1, i32 3)")), "only supports level 0"),
            "lod-dynamic": (ll.replace(call, call.replace("i32 0, i32 3)", "i32 " + layer + ", i32 3)")),
                            "only supports level 0"),
        }
        for name, (source, expected_error) in invalid.items():
            if source == ll:
                raise RuntimeError(f"{name}: mutation did not change the fixture")
            path = out / (name + ".ll")
            path.write_text(source)
            bc = path.with_suffix(".bc")
            run([args.llvm_as, path, "-o", bc], name + "-assemble")
            if expected_error not in run([args.air2nir, bc], name + "-reject", 1):
                raise RuntimeError(f"{name}: expected signature or permission diagnostic was absent")
        # The legacy image-read signature has no sampler or offset operands.
        legacy = run([args.air2nir, "--ll", out / "rw_2d_float_ushort.bc"], "legacy-llvm")
        call = next(line for line in legacy.splitlines() if "tail call" in line and "@air.read_texture" in line)
        decl = next(line for line in legacy.splitlines() if line.startswith("declare") and "@air.read_texture" in line)
        old_call = re.sub(r", ptr addrspace\(2\) %\d+", "", call).replace(", <2 x i16> zeroinitializer", "")
        old_decl = decl.replace(", ptr addrspace(2)", "").replace("<2 x i16>, <2 x i16>", "<2 x i16>")
        path = out / "legacy.ll"
        path.write_text(legacy.replace(call, old_call).replace(decl, old_decl))
        run([args.llvm_as, path, "-o", out / "legacy.bc"], "legacy-assemble")
        legacy_nir = run([args.air2nir, out / "legacy.bc"], "legacy-nir")
        if args.baseline and run([args.baseline, out / "legacy.bc"], "legacy-baseline") != legacy_nir:
            raise RuntimeError("legacy no-sampler NIR changed")
        if args.original:
            run([args.air2nir, args.original], "original-nir")
            if args.ir3:
                run([args.ir3, args.original, "mps_stgf_applyCoefficients3_2d", out / "original.ir3.bin"], "original-ir3")
        print("PASS: 4032 native components, omitted-write sensitivity, nine negative signatures, legacy NIR; A830 execution pending")
    finally:
        (out / "results.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()
