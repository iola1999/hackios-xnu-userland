#!/usr/bin/env python3
"""Compile stitching fixtures and run the focused AIR graph regressions.

From the repository root, with an existing Mesa host build:
  ninja -C "$BUILD" src/compiler/air/air_stitch_link_test
  cc -Itools/mesa/air tools/mesa/air/air_stitch_test.c \
     -L"$BUILD/src/gallium/drivers/freedreno" -lannibaleair -o /tmp/air_stitch_test
  python3 tools/air/stitchcheck.py --air2nir "$BUILD/src/compiler/air/air2nir" \
     --stitch-test /tmp/air_stitch_test \
     --link-test "$BUILD/src/compiler/air/air_stitch_link_test" --out /tmp/air-stitch

Set the host library search path for libannibaleair if needed. The fixtures use
Xcode's Metal compiler; --llvm-as selects an LLVM assembler. To include the
original CI graph, pass --original-ci-metallib with the local system library.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--air2nir", type=Path, required=True)
parser.add_argument("--stitch-test", type=Path, required=True)
parser.add_argument("--link-test", type=Path, required=True)
parser.add_argument("--llvm-as", default="llvm-as")
parser.add_argument("--original-ci-metallib", type=Path)
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
source = ROOT / "tools/mesa/air/air_stitch_test.metal"
subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-std=metal3.1", "-c", str(source),
                "-o", str(args.out / "stitch.air")], check=True)
subprocess.run(["xcrun", "-sdk", "macosx", "metallib", str(args.out / "stitch.air"),
                "-o", str(args.out / "stitch.metallib")], check=True)
spec = importlib.util.spec_from_file_location("metallib", ROOT / "tools/metallib.py")
metallib = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metallib)


def extract(path, dest, names=None):
    dest.mkdir(exist_ok=True)
    data, header, functions = metallib.parse(str(path))
    found = set()
    for function in functions:
        name = function["name"]
        if names is not None and name not in names:
            continue
        bitcode, _ = metallib.bitcode_of(data, header, function)
        (dest / (name + ".bc")).write_bytes(bitcode)
        found.add(name)
    if names is not None and found != set(names):
        raise RuntimeError("the original CI library lacks requested functions")


modules = args.out / "modules"
extract(args.out / "stitch.metallib", modules)
original = None
if args.original_ci_metallib:
    original = args.out / "original"
    extract(args.original_ci_metallib, original,
            ["_ci_early_out", "_ci_init_destcoord", "_ci_read_pixel", "_ci_crop", "_ci_write_pixel"])

# The first ordinary node allocates four scratch bytes. The second node's
# legitimate power-of-two alignment plus its declared size exceeds 32 bits.
printed = subprocess.run([str(args.air2nir), "--ll", str(modules / "stop.bc")],
                         check=True, text=True, capture_output=True).stdout
modified, count = re.subn(r'(!"air.uint_type", i32 )4, i32 4,',
                          r'\g<1>4294967280, i32 2147483648,', printed)
if count != 1:
    raise RuntimeError("unexpected uint stitching metadata in stop fixture")
ll = modules / "stop_huge.ll"
ll.write_text(modified)
subprocess.run([args.llvm_as, str(ll), "-o", str(modules / "stop_huge.bc")], check=True)

for name, command in [
    ("stitch", [str(args.stitch_test), str(modules)] + ([str(original)] if original else [])),
    ("linked", [str(args.link_test), str(modules)]),
]:
    result = subprocess.run(command, capture_output=True, text=True, timeout=45)
    (args.out / (name + ".log")).write_text(result.stdout + result.stderr)
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="")
    result.check_returncode()
