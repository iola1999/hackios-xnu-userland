#!/usr/bin/env python3
"""Compare triangle coverage from a K90 vertex capture using llvmpipe and macOS Metal."""
import argparse
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    default_tools = os.environ.get("AIR_HOST_DIR", root / "build/mesa-host-replay")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--tools", type=Path, default=Path(default_tools))
    args = parser.parse_args()
    with args.capture.open("rb") as stream:
        draw = plistlib.load(stream)
    if draw["primitive"] != 3 or draw["indexed"] or draw["start"] or draw["baseInstance"]:
        parser.error("coverage replay requires non-indexed triangles, vertex start 0 and base instance 0")
    if not draw["count"] or not draw["instances"] or not draw["vertex"]:
        parser.error("empty draw or vertex module")

    with tempfile.TemporaryDirectory() as temp:
        inputs = Path(temp)
        module, library = inputs / "vertex.bc", inputs / "vertex.metallib"
        module.write_bytes(draw["vertex"])
        for index, data in draw["buffers"].items():
            if not index.isascii() or not index.isdecimal() or int(index) >= 31:
                parser.error("invalid Metal vertex buffer index")
            (inputs / f"{int(index)}.bin").write_bytes(data)
        geometry = [inputs, str(draw["count"]), str(draw["instances"])]
        mesa, metal = inputs / "mesa.rgba", inputs / "metal.rgba"
        subprocess.run([args.tools / "air_vertex", module, *geometry, mesa], check=True)
        subprocess.run(["xcrun", "metallib", module, "-o", library], check=True)
        subprocess.run([args.tools / "vertex-metal", library, draw["entry"], *geometry, metal], check=True)
        a, b = mesa.read_bytes(), metal.read_bytes()
        if len(a) != 128 * 128 * 4 or len(b) != len(a):
            parser.exit(1, "incomplete RGBA8 readback\n")
        differences = sum(a[i:i+4] != b[i:i+4] for i in range(0, len(a), 4))
        shaded = [sum(bool(v) for v in image[::4]) for image in (a, b)]
        print(f"{draw['entry']}: {draw['count']} vertices x {draw['instances']} instances, "
              f"{len(draw['buffers'])} buffers; coverage llvmpipe={shaded[0]}, Metal={shaded[1]}, "
              f"{differences} differing pixels out of {len(a)//4}")
        return int(differences != 0 or not all(shaded))


if __name__ == "__main__":
    raise SystemExit(main())
