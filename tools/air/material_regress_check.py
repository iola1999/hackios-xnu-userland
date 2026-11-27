#!/usr/bin/env python3
"""Build and run ENG-09 dual-end Metal material regression suite and compare dumps.

Modes:
  1. Build & run macOS native reference baseline (+ signed arm64e iOS probe):
     material_regress_check.py --work-dir <dir>
  2. Compare two dump directories (each containing cases.json and *.bin):
     material_regress_check.py --compare <ref-dir> <test-dir> [--allow-format94-8bit]
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def f16_ulp_diff(a_bits: int, b_bits: int) -> int:
    def to_ordered(u: int) -> int:
        return (0x8000 - u) if (u & 0x8000) else (u + 0x8000)
    return abs(to_ordered(a_bits) - to_ordered(b_bits))


def unpack_bgr10a2(word: int) -> tuple[int, int, int, int]:
    return (
        word & 0x3FF,
        (word >> 10) & 0x3FF,
        (word >> 20) & 0x3FF,
        (word >> 30) & 0x3,
    )


def compare_case(ref_bytes: bytes, test_bytes: bytes, meta: dict,
                 allow_fmt94_8bit: bool) -> dict:
    fmt = meta["format"]
    w, h, bpp = meta["width"], meta["height"], meta["bpp"]
    rx, ry, rw, rh = meta["check_rect"]
    if len(ref_bytes) != w * h * bpp or len(test_bytes) != w * h * bpp:
        return {"ok": False, "reason": "byte size mismatch"}

    mismatches = 0
    max_err = 0.0
    checked = 0
    first_bad = None

    for y in range(ry, ry + rh):
        for x in range(rx, rx + rw):
            off = (y * w + x) * bpp
            checked += 1
            if fmt in ("bgra8", "bgra8_srgb", "rgba8_srgb"):
                tol = 2 if "srgb" in fmt else 1
                ra = ref_bytes[off:off + 4]
                ta = test_bytes[off:off + 4]
                diff = max(abs(ra[c] - ta[c]) for c in range(4))
                if diff > max_err:
                    max_err = float(diff)
                if diff > tol:
                    mismatches += 1
                    if first_bad is None:
                        first_bad = (x, y, list(ra), list(ta), diff)
            elif fmt == "rgba16f":
                r_bits = struct.unpack_from("<4H", ref_bytes, off)
                t_bits = struct.unpack_from("<4H", test_bytes, off)
                r_val = struct.unpack_from("<4e", ref_bytes, off)
                t_val = struct.unpack_from("<4e", test_bytes, off)
                bad = False
                for c in range(4):
                    if math.isnan(r_val[c]) or math.isnan(t_val[c]):
                        bad = True
                        continue
                    ulp = f16_ulp_diff(r_bits[c], t_bits[c])
                    adiff = abs(r_val[c] - t_val[c])
                    if adiff > max_err:
                        max_err = adiff
                    if ulp > 2 and adiff > 1e-3:
                        bad = True
                if bad:
                    mismatches += 1
                    if first_bad is None:
                        first_bad = (x, y, list(r_val), list(t_val))
            elif fmt == "bgr10a2":
                tol_rgb = 4 if allow_fmt94_8bit else 1
                tol_a = 1
                rw_word = struct.unpack_from("<I", ref_bytes, off)[0]
                tw_word = struct.unpack_from("<I", test_bytes, off)[0]
                rc = unpack_bgr10a2(rw_word)
                tc = unpack_bgr10a2(tw_word)
                d_rgb = max(abs(rc[c] - tc[c]) for c in range(3))
                d_a = abs(rc[3] - tc[3])
                if float(d_rgb) > max_err:
                    max_err = float(d_rgb)
                if d_rgb > tol_rgb or d_a > tol_a:
                    mismatches += 1
                    if first_bad is None:
                        first_bad = (x, y, list(rc), list(tc), d_rgb, d_a)
            else:
                return {"ok": False, "reason": f"unsupported format {fmt}"}

    return {
        "ok": mismatches == 0,
        "format": fmt,
        "checked_pixels": checked,
        "mismatches": mismatches,
        "max_err": max_err,
        "first_bad": first_bad,
    }


def compare_dirs(ref_dir: Path, test_dir: Path, allow_fmt94_8bit: bool) -> int:
    ref_doc = json.loads((ref_dir / "cases.json").read_text())
    test_doc = json.loads((test_dir / "cases.json").read_text())
    test_by_name = {c["name"]: c for c in test_doc["cases"]}
    failed = 0
    for meta in ref_doc["cases"]:
        name = meta["name"]
        if name not in test_by_name:
            print(f"FAIL {name}: missing in test output")
            failed += 1
            continue
        res = compare_case(
            (ref_dir / meta["file"]).read_bytes(),
            (test_dir / test_by_name[name]["file"]).read_bytes(),
            meta,
            allow_fmt94_8bit,
        )
        status = "PASS" if res["ok"] else "FAIL"
        print(f"{status} {name} fmt={meta['format']} checked={res.get('checked_pixels', 0)} "
              f"mismatches={res.get('mismatches', -1)} max_err={res.get('max_err', 0):.5g}")
        if not res["ok"]:
            if res.get("first_bad") is not None:
                print(f"  first_bad={res['first_bad']}")
            failed += 1
    print(f"COMPARE total={len(ref_doc['cases'])} failed={failed}")
    return 1 if failed else 0


def build_and_run_native(work: Path) -> int:
    work = work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    record = {"commands": [], "inputs": {}, "device_tested": False}

    def run(argv, log_name, expected=0):
        argv = [str(a) for a in argv]
        res = subprocess.run(argv, cwd=work, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
        (work / log_name).write_text(res.stdout)
        record["commands"].append({"argv": argv, "returncode": res.returncode,
                                   "expected": expected, "log": log_name})
        if res.returncode != expected:
            raise RuntimeError(f"{log_name}: exit {res.returncode} (expected {expected})\n{res.stdout}")
        return res.stdout

    for p in (HERE / "material_regress.m", HERE / "material_regress.metal"):
        record["inputs"][p.name] = sha256(p)

    for sdk, target, arch, minflag in (
        ("macosx", "air64-apple-macos14.0.0", "arm64", "-mmacosx-version-min=14.0"),
        ("iphoneos", "air64-apple-ios16.5.0", "arm64e", "-miphoneos-version-min=26.0"),
    ):
        run(["xcrun", "--sdk", sdk, "metal", "-std=metal3.0", "-target", target,
             "-fno-fast-math", "-c", HERE / "material_regress.metal", "-o", work / f"{sdk}.air"],
            f"{sdk}-metal.log")
        run(["xcrun", "--sdk", sdk, "metallib", work / f"{sdk}.air", "-o",
             work / f"{sdk}.metallib"], f"{sdk}-metallib.log")
        run(["xcrun", "--sdk", sdk, "clang", "-arch", arch, minflag, "-O1", "-Wall", "-Wextra",
             "-Werror", "-fno-objc-arc", HERE / "material_regress.m",
             "-framework", "Foundation", "-framework", "Metal", "-lobjc",
             "-o", work / f"{sdk}-probe"], f"{sdk}-probe.log")

    run(["codesign", "--force", "--sign", "-", work / "iphoneos-probe"], "ios-sign.log")
    run(["codesign", "--verify", "--strict", work / "iphoneos-probe"], "ios-sign-verify.log")

    ref_dir = work / "native-pixels"
    out = run([work / "macosx-probe", "--metallib", work / "macosx.metallib",
               "--output", ref_dir], "native.log")
    if "cases=18 ok" not in out:
        raise RuntimeError(f"expected 18 reference cases, got:\n{out}")

    # Self-comparison must pass with 0 mismatches across all 16 cases.
    if compare_dirs(ref_dir, ref_dir, allow_fmt94_8bit=False) != 0:
        raise RuntimeError("self-comparison failed")

    # Negative check: perturb 1 byte in glass_fbfetch_c0 by +5 LSB and confirm detector catches it.
    neg_dir = work / "neg-pixels"
    neg_dir.mkdir(exist_ok=True)
    for f in ref_dir.iterdir():
        (neg_dir / f.name).write_bytes(f.read_bytes())
    bad_path = neg_dir / "glass_fbfetch_c0.bin"
    raw = bytearray(bad_path.read_bytes())
    raw[0] = (raw[0] + 5) & 0xFF
    bad_path.write_bytes(bytes(raw))
    if compare_dirs(ref_dir, neg_dir, allow_fmt94_8bit=False) == 0:
        raise RuntimeError("negative perturbation was not detected")

    # Verify writeMask=0 preserved c1 vs DontCare interior redrawn (mp_mask0 != mp_dontcare).
    m0 = (ref_dir / "mp_mask0_readback.bin").read_bytes()
    dc = (ref_dir / "mp_dontcare_readback.bin").read_bytes()
    if m0 == dc:
        raise RuntimeError("mp_mask0_readback and mp_dontcare_readback unexpectedly identical")

    record["outputs"] = {
        name: sha256(work / name)
        for name in ("macosx.metallib", "iphoneos.metallib", "iphoneos-probe")
    }
    (work / "validation.json").write_text(json.dumps(record, indent=2) + "\n")
    print("PASS: 18 native material + G3 cases, self-compare ok, negative perturbation caught, signed arm64e iOS probe")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work-dir", type=Path, help="output directory for build + macOS native reference")
    p.add_argument("--compare", nargs=2, type=Path, metavar=("REF_DIR", "TEST_DIR"),
                   help="compare two material_regress dump directories")
    p.add_argument("--allow-format94-8bit", action="store_true",
                   help="relax bgr10a2 tolerance to 4/1023 for pre-S6 baseline comparisons")
    args = p.parse_args()
    if args.compare:
        return compare_dirs(args.compare[0], args.compare[1], args.allow_format94_8bit)
    if args.work_dir:
        return build_and_run_native(args.work_dir)
    p.error("specify --work-dir or --compare")
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as err:
        sys.exit(str(err))
