#!/usr/bin/env python3
"""Host test for Weather VFX AIR contracts: packed argbuf padding and air.amplification_id."""
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import air_env

SHADER = r"""
#include <metal_stdlib>
using namespace metal;

struct MatParams {
    float intensity [[id(0)]];
    float3 myColorTint [[id(1)]];
    texture2d<half> skyTex [[id(2)]];
};

struct VOut {
    float4 pos [[position]];
    float2 uv;
};

vertex VOut vfx_amp_vert(uint vid [[vertex_id]], uint amp [[amplification_id]])
{
    VOut o;
    float x = float(vid & 1u) * 2.0f - 1.0f + float(amp) * 0.01f;
    float y = float((vid >> 1) & 1u) * 2.0f - 1.0f;
    o.pos = float4(x, y, 0.0f, 1.0f);
    o.uv = float2(x, y);
    return o;
}

fragment half4 vfx_padded_argbuf_frag(VOut in [[stage_in]],
                                      constant MatParams &mp [[buffer(0)]],
                                      sampler s [[sampler(0)]])
{
    half4 tex = mp.skyTex.sample(s, in.uv);
    return half4(tex.rgb * half3(mp.myColorTint) * half(mp.intensity), tex.a);
}
"""


def main() -> int:
    air2nir = air_env.air2nir_path()
    with tempfile.TemporaryDirectory(prefix="air-weather-vfx-") as td:
        msl = os.path.join(td, "vfx.metal")
        air = os.path.join(td, "vfx.air")
        mlib = os.path.join(td, "vfx.metallib")
        with open(msl, "w") as f:
            f.write(SHADER)
        subprocess.run([
            "xcrun", "--sdk", "iphoneos", "metal", "-std=metal3.0",
            "-target", "air64-apple-ios16.5.0", "-c", msl, "-o", air,
        ], check=True)
        subprocess.run(["xcrun", "--sdk", "iphoneos", "metallib", air, "-o", mlib], check=True)

        for fn in ("vfx_amp_vert", "vfx_padded_argbuf_frag"):
            res = subprocess.run([air2nir, mlib, fn], capture_output=True, text=True)
            if res.returncode != 0:
                raise RuntimeError(f"air2nir failed for {fn}:\n{res.stdout}\n{res.stderr}")
    print("test-air-weather-vfx: PASS (amplification_id + packed argbuf padding)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
