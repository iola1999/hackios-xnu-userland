#!/usr/bin/env python3
"""Build isolated native/iOS framebuffer-fetch probes and run the Apple GPU reference.

Requires Xcode's Metal toolchain. --air2nir optionally checks the iOS AIR with
an existing frontend binary; no shared build or device command is invoked.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


HERE = Path(__file__).resolve().parent
FUNCTIONS = ("fbf_vertex", "fbf_seed", "fbf_pass", "fbf_swap", "fbf_wrong_data1")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True, help="new directory for all outputs")
    parser.add_argument("--air2nir", type=Path, help="existing host air2nir; read and executed only")
    args = parser.parse_args()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    record = {"commands": [], "inputs": {}, "device_tested": False}

    def run(argv, name, expected=0):
        argv = [str(arg) for arg in argv]
        result = subprocess.run(argv, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True)
        (work / name).write_text(result.stdout)
        record["commands"].append({"argv": argv, "returncode": result.returncode,
                                   "expected": expected, "log": name})
        if result.returncode != expected:
            raise RuntimeError(f"{name}: exit {result.returncode}, expected {expected}")
        return result.stdout

    try:
        for path in (HERE / "fbfetch-mrt.m", HERE / "fbfetch-mrt.metal"):
            record["inputs"][path.name] = sha256(path)
        for sdk, target, arch, minimum in (
            ("macosx", "air64-apple-macos14.0.0", "arm64", "-mmacosx-version-min=14.0"),
            ("iphoneos", "air64-apple-ios16.5.0", "arm64e", "-miphoneos-version-min=26.0"),
        ):
            record[sdk + "_sdk"] = run(["xcrun", "--sdk", sdk, "--show-sdk-version"],
                                         sdk + "-sdk.log").strip()
            run(["xcrun", "--sdk", sdk, "metal", "-std=metal3.0", "-target", target,
                 "-fno-fast-math", "-c", HERE / "fbfetch-mrt.metal", "-o", work / (sdk + ".air")],
                sdk + "-metal.log")
            run(["xcrun", "--sdk", sdk, "metallib", work / (sdk + ".air"), "-o",
                 work / (sdk + ".metallib")], sdk + "-metallib.log")
            run(["xcrun", "--sdk", sdk, "clang", "-arch", arch, minimum, "-O1", "-Wall", "-Wextra",
                 "-Werror", "-fno-objc-arc", HERE / "fbfetch-mrt.m", "-framework", "Foundation",
                 "-framework", "Metal", "-lobjc", "-o", work / (sdk + "-probe")], sdk + "-probe.log")
        run(["codesign", "--force", "--sign", "-", work / "iphoneos-probe"], "ios-sign.log")
        run(["codesign", "--verify", "--strict", work / "iphoneos-probe"], "ios-sign-verify.log")
        run(["file", work / "iphoneos-probe"], "ios-file.log")
        run(["otool", "-l", work / "iphoneos-probe"], "ios-load-commands.log")
        native = run([work / "macosx-probe", "--metallib", work / "macosx.metallib",
                      "--output", work / "native-pixels"], "native.log")
        if "cases=8 failed=0 pixels=3072" not in native:
            raise RuntimeError("native reference did not report every comparison")
        wrong = run([work / "macosx-probe", "--metallib", work / "macosx.metallib", "--wrong-data1"],
                    "wrong-data1.log", expected=1)
        if "cases=8 failed=8 pixels=3072" not in wrong:
            raise RuntimeError("wrong DATA1 read was not detected in every case")
        rejected_output = work / "rejected-output"
        (rejected_output / "loaded-pass-normal-0.bgra").mkdir(parents=True)
        run([work / "macosx-probe", "--metallib", work / "macosx.metallib", "--wrong-data1",
             "--output", rejected_output], "runtime-error.log", expected=2)
        record["native"] = {"cases": 8, "pixels": 3072, "wrong_data1_cases_rejected": 8}
        record["runtime_error"] = "readback write failure returns 2, distinct from pixel mismatch"
        run([sys.executable, HERE.parent / "metallib.py", "dump", work / "iphoneos.metallib",
             work / "ios-bitcode"], "ios-bitcode.log")
        record["air"] = "not checked: --air2nir was not supplied"
        if args.air2nir:
            air2nir = args.air2nir.resolve(strict=True)
            record["inputs"]["air2nir"] = {"path": str(air2nir), "sha256": sha256(air2nir)}
            for function in FUNCTIONS:
                nir = run([air2nir, work / "iphoneos.metallib", function], function + ".nir")
                if function in ("fbf_pass", "fbf_swap"):
                    if "uses_fbfetch_output: true" not in nir:
                        raise RuntimeError(function + ": missing framebuffer-fetch flag")
                    for target in ("FRAG_RESULT_DATA0", "FRAG_RESULT_DATA1"):
                        if f"io location={target} slots=1 fbfetch coherent" not in nir:
                            raise RuntimeError(function + ": missing coherent read from " + target)
                        declaration = re.search(r"^decl_var shader_out .* (\w+) \(" + target + r"\.", nir, re.M)
                        deref = re.search(r"(%\d+) = deref_var &" + declaration[1] + r" \(shader_out", nir) if declaration else None
                        if not deref or not re.search(r"@store_deref \(" + deref[1] + r", %\d+\)", nir):
                            raise RuntimeError(function + ": missing store to " + target)
            record["air"] = "all five functions decoded; DATA0/DATA1 coherent reads and output stores present"
        record["outputs"] = {name: sha256(work / name) for name in
                             ("macosx.metallib", "iphoneos.metallib", "iphoneos-probe")}
        print("PASS: 8 native pixel cases, 8 wrong-DATA1 rejections, signed arm64e iOS probe")
        print("AIR:", record["air"])
        print("A830 pixels and submission ordering require a later device test.")
    finally:
        (work / "validation.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        sys.exit(str(error))
