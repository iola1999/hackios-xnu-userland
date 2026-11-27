#!/usr/bin/env python3
"""Minimal public function-constant dead-branch / null-select pointer load test."""
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

constant bool has_weights [[function_constant(0)]];
constant uint mode [[function_constant(1)]];

static half sample_weights(device const half *w, uint idx)
{
    return w[idx] + w[idx + 1u];
}

fragment half4 fc_null_select(float4 pos [[position]],
                              device const half *weights [[buffer(0), function_constant(has_weights)]])
{
    device const half *ptr = has_weights ? weights : nullptr;
    half v = half(0.25h);
    if (mode == 9u && has_weights) {
        v = sample_weights(ptr, uint(pos.x) & 3u);
    }
    return half4(v, v, v, half(1.0h));
}
"""


def run_air2nir(air2nir: str, args: list[str], label: str) -> str:
    res = subprocess.run([air2nir, *args], capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"air2nir failed ({label}):\n{res.stdout}\n{res.stderr}")
    return res.stdout


def main() -> int:
    air2nir = air_env.air2nir_path()
    with tempfile.TemporaryDirectory(prefix="air-fc-prune-") as td:
        msl = os.path.join(td, "fc.metal")
        air = os.path.join(td, "fc.air")
        mlib = os.path.join(td, "fc.metallib")
        with open(msl, "w") as f:
            f.write(SHADER)
        subprocess.run([
            "xcrun", "--sdk", "iphoneos", "metal", "-std=metal3.0",
            "-target", "air64-apple-ios16.5.0", "-c", msl, "-o", air,
        ], check=True)
        subprocess.run(["xcrun", "--sdk", "iphoneos", "metallib", air, "-o", mlib], check=True)

        # Both default (all-zero) and specialized (--fc 0=1,1=9) must translate and validate cleanly.
        run_air2nir(air2nir, [mlib, "fc_null_select"], "default fc=0")
        run_air2nir(air2nir, ["--fc", "0=0,1=9", mlib, "fc_null_select"], "fc 0=0,1=9")
        run_air2nir(air2nir, ["--fc", "0=1,1=9", mlib, "fc_null_select"], "fc 0=1,1=9")

        qc_lib = HERE.parents[1] / "ios26/cryptex/System/Library/Frameworks/QuartzCore.framework/default.metallib"
        qc_checked = 0
        if qc_lib.is_file():
            p = air_env.materialize_metallib(str(qc_lib), td)
            if isinstance(p, tuple):
                p = p[0]
            for fn in ("fixed_frag_lph_cph", "fixed_frag_lph_cpf"):
                run_air2nir(air2nir, ["--interface", p, fn], f"QuartzCore:{fn}")
                qc_checked += 1

    print(f"test-air-fc-prune: PASS (public=3, QuartzCore={qc_checked})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
