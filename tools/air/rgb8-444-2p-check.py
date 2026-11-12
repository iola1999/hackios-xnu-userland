#!/usr/bin/env python3
"""Build isolated RGB8_444_2P probes, verify native pixels, and sign the iOS binary."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


HERE = Path(__file__).resolve().parent
PREFIX = "rgb8-444-2p"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True, help="new directory for all outputs")
    args = parser.parse_args()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    inputs = work / "inputs"
    inputs.mkdir()
    record = {"commands": [], "inputs": {}, "device_tested": False}

    def run(argv, name, allowed=(0,), limit=60):
        argv = [str(x) for x in argv]
        try:
            result = subprocess.run(argv, cwd=work, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=limit)
        except subprocess.TimeoutExpired as error:
            (work / name).write_bytes(error.stdout or b"")
            record["commands"].append({"argv": argv, "timeout_seconds": limit, "log": name})
            raise RuntimeError(f"{name}: host timeout after {limit} seconds") from error
        (work / name).write_bytes(result.stdout)
        record["commands"].append({"argv": argv, "returncode": result.returncode, "log": name})
        if result.returncode not in allowed:
            raise RuntimeError(f"{name}: exit {result.returncode}, expected {allowed}")
        return result.returncode, result.stdout.decode(errors="replace")

    def pixels(text, mode):
        matches = re.findall(r"^RESULT mode=" + re.escape(mode) +
                             r" pixels=(\d+) sample_mismatches=(\d+) read_mismatches=(\d+)$", text, re.M)
        if len(matches) != 1 or "command status=4 error=none" not in text:
            raise RuntimeError(f"{mode}: missing completed command and unique pixel result")
        count, sample, read = map(int, matches[0])
        if count != 153 or not (0 <= sample <= count and 0 <= read <= count):
            raise RuntimeError(f"{mode}: invalid comparison counts")
        return {"pixels": count, "sample_mismatches": sample, "read_mismatches": read}

    try:
        for suffix in (".m", ".metal", "-check.py", ".md"):
            source = HERE / (PREFIX + suffix)
            shutil.copy2(source, inputs / source.name)
            record["inputs"][source.name] = sha256(source)
        for sdk, target, arch, minimum in (
            ("macosx", "air64-apple-macos14.0.0", "arm64", "-mmacosx-version-min=14.0"),
            ("iphoneos", "air64-apple-ios16.5.0", "arm64e", "-miphoneos-version-min=26.0"),
        ):
            _, version = run(["xcrun", "--sdk", sdk, "--show-sdk-version"], sdk + "-sdk.log")
            record[sdk + "_sdk"] = version.strip()
            run(["xcrun", "--sdk", sdk, "metal", "-std=metal3.0", "-target", target,
                 "-fno-fast-math", "-c", inputs / (PREFIX + ".metal"), "-o", work / (sdk + ".air")],
                sdk + "-metal.log")
            run(["xcrun", "--sdk", sdk, "metallib", work / (sdk + ".air"), "-o",
                 work / (sdk + ".metallib")], sdk + "-metallib.log")
            run(["xcrun", "--sdk", sdk, "clang", "-arch", arch, minimum, "-O1", "-Wall", "-Wextra",
                 "-Werror", "-fno-objc-arc", inputs / (PREFIX + ".m"), "-framework", "Foundation",
                 "-framework", "IOSurface", "-framework", "Metal", "-lobjc", "-o",
                 work / (sdk + "-probe")], sdk + "-probe.log")
        run(["codesign", "--force", "--sign", "-", work / "iphoneos-probe"], "ios-sign.log")
        run(["codesign", "--verify", "--strict", work / "iphoneos-probe"], "ios-sign-verify.log")
        run(["file", work / "iphoneos-probe"], "ios-file.log")
        run(["otool", "-l", work / "iphoneos-probe"], "ios-load-commands.log")
        probe = [work / "macosx-probe", work / "macosx.metallib"]
        _, native = run(probe + ["--output", work / "native.rgba32f"], "native.log", limit=40)
        record["native"] = pixels(native, "normal")
        if record["native"] != {"pixels": 153, "sample_mismatches": 0, "read_mismatches": 0}:
            raise RuntimeError("native pixel reference failed")
        _, swapped = run(probe + ["--swap-br", "--output", work / "swap-br.rgba32f"],
                         "swap-br.log", allowed=(1,), limit=40)
        record["swap_br"] = pixels(swapped, "swap-br")
        if record["swap_br"] != {"pixels": 153, "sample_mismatches": 153, "read_mismatches": 153}:
            raise RuntimeError("B/R exchange did not fail at every pixel for both operations")
        status, half = run(probe + ["--half-plane", "--output", work / "half-plane.rgba32f"],
                           "half-plane.log", allowed=(1, 78), limit=40)
        if "preflight format=542 normal-import=success" not in half:
            raise RuntimeError("half-plane case lacks a successful normal-layout import")
        if status == 78:
            if not re.search(r"^REJECTED mode=half-plane stage=(surface-create|texture-import)( |$)", half, re.M):
                raise RuntimeError("half-plane rejection lacks its stage")
            record["half_plane"] = {"result": "rejected", "returncode": status}
        else:
            record["half_plane"] = pixels(half, "half-plane")
            if not record["half_plane"]["sample_mismatches"] or not record["half_plane"]["read_mismatches"]:
                raise RuntimeError("half-plane case was not detected by both operations")
        readbacks = ["native.rgba32f", "swap-br.rgba32f"]
        if status == 1:
            readbacks.append("half-plane.rgba32f")
        record["readback_bytes"] = {name: (work / name).stat().st_size for name in readbacks}
        if any(size != 153 * 8 * 4 for size in record["readback_bytes"].values()):
            raise RuntimeError("pixel readback has an unexpected length")
        _, output_error = run(probe + ["--output", work / "missing-parent" / "pixels"],
                              "output-error.log", allowed=(2,), limit=40)
        if ("command status=4 error=none" not in output_error or
                "ERROR stage=output-write" not in output_error or "RESULT " in output_error):
            raise RuntimeError("output error did not originate in readback file writing")
        record["outputs"] = {name: sha256(work / name) for name in
                             ("macosx-probe", "macosx.metallib", "iphoneos-probe", "iphoneos.metallib")}
        record["result"] = "passed"
        print("PASS: native sample/read, B/R exchange, half-plane sensitivity, signed arm64e probe")
        print("A830 execution is pending; unsupported format 542 returns 77.")
    finally:
        (work / "validation.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        sys.exit(str(error))
