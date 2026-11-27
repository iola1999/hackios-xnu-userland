#!/usr/bin/env python3
"""Minimal public AIR 2.8 2d_ms / depth_2d_ms read_sampler translation test."""
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import air_env
import metallib

SHADER = r"""
#include <metal_stdlib>
using namespace metal;

fragment float4 ms_read_color(texture2d_ms<float> tex [[texture(0)]],
                              float4 pos [[position]])
{
    uint2 p = uint2(pos.xy);
    uint s = (p.x + p.y) & 3u;
    return tex.read(p, s);
}

fragment float4 ms_read_depth(depth2d_ms<float> dtex [[texture(0)]],
                              float4 pos [[position]])
{
    uint2 p = uint2(pos.xy);
    float d = dtex.read(p, 0u);
    return float4(d, d, d, 1.0f);
}
"""


def translate_bc(air2nir: str, bc: bytes, td: str, name: str) -> str:
    bp = os.path.join(td, f"{name}.bc")
    with open(bp, "wb") as f:
        f.write(bc)
    res = subprocess.run([air2nir, bp], capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"air2nir failed for {name}:\n{res.stdout}\n{res.stderr}")
    return res.stdout


def main() -> int:
    air2nir = air_env.air2nir_path()
    with tempfile.TemporaryDirectory(prefix="air-msaa-read-") as td:
        msl = os.path.join(td, "msaa.metal")
        air = os.path.join(td, "msaa.air")
        mlib = os.path.join(td, "msaa.metallib")
        with open(msl, "w") as f:
            f.write(SHADER)
        subprocess.run([
            "xcrun", "--sdk", "iphoneos", "metal", "-std=metal3.0",
            "-target", "air64-apple-ios16.5.0", "-c", msl, "-o", air,
        ], check=True)
        subprocess.run(["xcrun", "--sdk", "iphoneos", "metallib", air, "-o", mlib], check=True)

        b, hdr, ents = metallib.parse(mlib)
        seen = set()
        for e in ents:
            name = e["name"]
            if name not in ("ms_read_color", "ms_read_depth"):
                continue
            bc, _ = metallib.bitcode_of(b, hdr, e)
            nir = translate_bc(air2nir, bc, td, name)
            if "txf_ms" not in nir:
                raise RuntimeError(f"{name}: expected txf_ms in NIR output")
            seen.add(name)
        if seen != {"ms_read_color", "ms_read_depth"}:
            raise RuntimeError(f"missing functions: {seen}")

        gl_lib = HERE.parents[1] / "ios26/rootfs/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib"
        gl_checked = 0
        if gl_lib.is_file():
            p = air_env.materialize_metallib(str(gl_lib), td)
            if isinstance(p, tuple):
                p = p[0]
            gb, ghdr, gents = metallib.parse(p)
            for e in gents:
                if "Multisample" in e["name"]:
                    bc, _ = metallib.bitcode_of(gb, ghdr, e)
                    nir = translate_bc(air2nir, bc, td, e["name"])
                    if "txf_ms" not in nir:
                        raise RuntimeError(f"{e['name']}: expected txf_ms in NIR output")
                    gl_checked += 1
            if gl_checked != 6:
                raise RuntimeError(f"expected 6 AppleMetalGLRenderer Multisample shaders, got {gl_checked}")

    print(f"test-air-msaa-read: PASS (public=2, AppleMetalGLRenderer={gl_checked})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
