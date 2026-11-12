#!/usr/bin/env python3
"""Check builtin read samplers on texture arrays and build an arm64e pixel probe."""
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
    record = []

    def run(argv, name, expected=0):
        result = subprocess.run([str(x) for x in argv], capture_output=True, text=True, timeout=60)
        (out / (name + ".log")).write_text(result.stdout + result.stderr)
        record.append({"name": name, "exit": result.returncode, "expected": expected})
        if result.returncode != expected:
            raise RuntimeError(f"{name}: exit {result.returncode}, expected {expected}")
        return result.stdout + result.stderr

    try:
        for suffix in (".m", ".metal"):
            shutil.copy2(here / ("read-array" + suffix), out / ("read-array" + suffix))
        for sdk, target, arch, minimum in (
            ("macosx", "air64_v28-apple-macos26.0.0", "arm64", "-mmacosx-version-min=26.0"),
            ("iphoneos", "air64_v28-apple-ios26.6.0", "arm64e", "-miphoneos-version-min=26.0"),
        ):
            run(["xcrun", "--sdk", sdk, "metal", "-std=metal3.2", "-target", target,
                 "-c", out / "read-array.metal", "-o", out / (sdk + ".air")], sdk + "-compile")
            run(["xcrun", "--sdk", sdk, "metallib", out / (sdk + ".air"), "-o",
                 out / (sdk + ".metallib")], sdk + "-library")
            run(["xcrun", "--sdk", sdk, "clang", "-arch", arch, minimum, "-O1", "-Wall", "-Wextra",
                 "-Werror", out / "read-array.m", "-framework", "Foundation", "-framework", "Metal",
                 "-o", out / (sdk + "-probe")], sdk + "-probe")
        run(["codesign", "--force", "--sign", "-", out / "iphoneos-probe"], "sign")
        run(["codesign", "--verify", "--strict", out / "iphoneos-probe"], "verify-signature")
        native = run([out / "macosx-probe", out / "macosx.metallib"], "native")
        if "RESULT components=2184 errors=0" not in native or native.count("errors=0 status=4") != 5:
            raise RuntimeError("native output lacks the five completed pixel comparisons")
        run(["xcrun", "--sdk", "macosx", "metal", "-std=metal3.2", "-DWRONG_LAYER",
             "-target", "air64_v28-apple-macos26.0.0", "-c", out / "read-array.metal",
             "-o", out / "wrong.air"], "wrong-compile")
        run(["xcrun", "--sdk", "macosx", "metallib", out / "wrong.air", "-o",
             out / "wrong.metallib"], "wrong-library")
        wrong = run([out / "macosx-probe", out / "wrong.metallib"], "wrong-layer", 1)
        if "RESULT components=2184 errors=672" not in wrong:
            raise RuntimeError("the layer-zero variant did not fail at the expected coordinates")

        spec = importlib.util.spec_from_file_location("metallib", here.parent / "metallib.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        data, header, functions = module.parse(out / "iphoneos.metallib")
        for function in functions:
            name = function["name"]
            bitcode, _ = module.bitcode_of(data, header, function)
            path = out / (name + ".bc")
            path.write_bytes(bitcode)
            run([args.air2nir, path], name + "-nir")
            if args.ir3:
                run([args.ir3, path, name, out / (name + ".ir3.bin")], name + "-ir3")
            if args.baseline:
                run([args.baseline, path], name + "-baseline", 0 if name == "plain_float_read" else 1)
        ll = run([args.air2nir, "--ll", out / "array_float_read32.bc"], "fixture-llvm")
        if "@air.get_read_sampler" not in ll:
            raise RuntimeError("the compiler did not emit a builtin read sampler")
        call = next(line for line in ll.splitlines() if "tail call" in line and "@air.read_texture" in line)
        declaration = next(line for line in ll.splitlines() if line.startswith("declare") and "@air.read_texture" in line)
        layer_call = re.sub(r"i32 %\d+, <2 x i32> zeroinitializer", "float 0.0, <2 x i32> zeroinitializer", call)
        layer_declaration = declaration.replace("<2 x i32>, i32, <2 x i32>", "<2 x i32>, float, <2 x i32>")
        invalid = {
            "layer-type": ll.replace(call, layer_call).replace(declaration, layer_declaration),
            "offset": ll.replace(call, call.replace("zeroinitializer", "<i32 1, i32 0>")),
            "access": ll.replace(call, re.sub(r"i32 1\)$", "i32 2)", call)),
        }
        for name, source in invalid.items():
            if source == ll:
                raise RuntimeError(f"{name}: mutation did not change the fixture")
            path = out / (name + ".ll")
            path.write_text(source)
            bc = path.with_suffix(".bc")
            run([args.llvm_as, path, "-o", bc], name + "-assemble")
            rejected = run([args.air2nir, bc], name + "-reject", 1)
            if "intrinsic: builtin read sampler" not in rejected:
                raise RuntimeError(f"{name}: failure did not originate in sampler validation")
        if args.original:
            name = "mps_stgf_computeInvertedGuideCovarianceMatrix3"
            run([args.air2nir, args.original], "original-nir")
            if args.ir3:
                run([args.ir3, args.original, name, out / "original.ir3.bin"], "original-ir3")
        stages = "NIR/IR3" if args.ir3 else "NIR"
        print(f"PASS: 2184 native components, layer sensitivity, malformed signatures, {stages}; A830 pending")
    finally:
        (out / "results.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    main()
