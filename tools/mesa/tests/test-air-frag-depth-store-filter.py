#!/usr/bin/env python3
"""Host regression test for fragment_writes_depth / drop_frag_depth_stores mode filtering.

Verifies two contracts under a no-depth-attachment pipeline (depth_format == 0):
  1. A fragment shader with nir_var_function_temp stores (whose var->data.location is 0,
     matching FRAG_RESULT_DEPTH) keeps its temp stores intact, reports
     info.fragment_writes_depth == 0, and draws the expected non-black color.
  2. A fragment shader that writes [[depth(any)]] has its depth store dropped, reports
     info.fragment_writes_depth == 0, and keeps its color output intact.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
AIR_DIR = ROOT / "tools" / "mesa" / "air"
REPLAY_DIR = ROOT / "build" / "mesa-host-replay"

METAL_SOURCE = r"""
#include <metal_stdlib>
using namespace metal;

struct Params {
    uint idx;
};

// Local array with dynamic index produces nir_var_function_temp store_deref (location == 0).
fragment float4 fs_temp_store(constant Params &p [[buffer(0)]]) {
    float table[4];
    table[0] = 0.25f;
    table[1] = 0.50f;
    table[2] = 0.75f;
    table[3] = 1.00f;
    uint i = p.idx & 3u;
    return float4(table[i], table[(i + 1u) & 3u], table[(i + 2u) & 3u], table[(i + 3u) & 3u]);
}

struct DepthOut {
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

fragment DepthOut fs_depth_store() {
    DepthOut out;
    out.color = float4(0.50f, 0.25f, 0.75f, 1.00f);
    out.depth = 0.375f;
    return out;
}
"""


def find_replay_build() -> tuple[pathlib.Path, pathlib.Path]:
    builds = sorted(REPLAY_DIR.glob("build-*"))
    srcs = sorted(REPLAY_DIR.glob("mesa-26.2.5-*"))
    assert builds and srcs, f"missing host replay build in {REPLAY_DIR}; run tools/air/mk-replay.sh"
    return builds[-1], srcs[-1]


def build_air_metal_test(build_dir: pathlib.Path, src_dir: pathlib.Path, tmp: pathlib.Path) -> pathlib.Path:
    inc = [
        f"-I{src_dir}/include",
        f"-I{src_dir}/include/drm-uapi",
        f"-I{ROOT}/tools/mesa/annibale-drm",
        f"-I{src_dir}/src",
        f"-I{src_dir}/src/compiler",
        f"-I{src_dir}/src/compiler/nir",
        f"-I{AIR_DIR}",
        f"-I{src_dir}/src/gallium/include",
        f"-I{src_dir}/src/gallium/auxiliary",
        f"-I{src_dir}/src/gallium/drivers",
        f"-I{src_dir}/src/gallium/winsys",
        f"-I{src_dir}/src/gallium/winsys/sw",
        f"-I{src_dir}/src/mesa",
        f"-I{src_dir}/src/util",
        f"-I{build_dir}/src",
        f"-I{build_dir}/src/compiler",
        f"-I{build_dir}/src/compiler/nir",
        f"-I{build_dir}/src/util",
    ]
    defs = [
        "-DNDEBUG",
        "-DHAVE_STRUCT_TIMESPEC",
        "-DHAVE_PTHREAD",
        "-DHAVE_FUNC_ATTRIBUTE_PACKED",
        "-DGALLIUM_SOFTPIPE",
        "-DMESA_DEBUG=0",
        "-Wno-unused-function",
        "-Wno-unused-variable",
        "-Wno-missing-field-initializers",
    ]
    air_metal_o = tmp / "air_metal.o"
    air_test_o = tmp / "air_metal_test.o"
    drm_stub_c = tmp / "drm_stub.c"
    drm_stub_o = tmp / "drm_stub.o"
    bin_path = tmp / "air_metal_test"
    drm_stub_c.write_text("int drmIoctl(int fd, unsigned long req, void *arg) { (void)fd; (void)req; (void)arg; return -1; }\n")

    subprocess.run(
        [
            "clang", "-O2", "-std=c11", *defs, *inc,
            "-include", "drm_fourcc.h", "-include", "xf86drm.h",
            "-c", str(AIR_DIR / "air_metal.c"), "-o", str(air_metal_o),
        ],
        check=True,
    )
    subprocess.run(["clang", "-O2", "-c", str(drm_stub_c), "-o", str(drm_stub_o)], check=True)
    subprocess.run(
        ["clang", "-O2", "-std=c11", *defs, *inc, "-c", str(AIR_DIR / "air_metal_test.c"), "-o", str(air_test_o)],
        check=True,
    )
    libs = [
        build_dir / "src/compiler/air/libair.a",
        build_dir / "src/gallium/drivers/softpipe/libsoftpipe.a",
        build_dir / "src/gallium/auxiliary/libgallium.a",
        build_dir / "src/gallium/auxiliary/libgalliumvl_stub.a",
        build_dir / "src/gallium/winsys/sw/null/libws_null.a",
        build_dir / "src/compiler/nir/libnir.a",
        build_dir / "src/compiler/libcompiler.a",
        build_dir / "src/util/libmesa_util.a",
        build_dir / "src/util/blake3/libblake3.a",
        build_dir / "src/c11/impl/libmesa_util_c11.a",
    ]
    llvm_libdir = subprocess.check_output(
        ["/opt/homebrew/opt/llvm/bin/llvm-config", "--libdir"], text=True
    ).strip()
    subprocess.run(
        [
            "clang++", "-O2", str(air_metal_o), str(air_test_o), str(drm_stub_o),
            *[str(lib) for lib in libs],
            "-L/opt/homebrew/opt/zstd/lib", "-lzstd",
            f"-L{llvm_libdir}", "-lLLVM",
            "-lz", "-lm", "-lpthread",
            "-o", str(bin_path),
        ],
        check=True,
    )
    return bin_path


def main() -> int:
    build_dir, src_dir = find_replay_build()
    air2nir = REPLAY_DIR / "air2nir"
    assert air2nir.is_file(), f"missing {air2nir}"

    with tempfile.TemporaryDirectory(prefix="air-depth-filter-") as td:
        tmp = pathlib.Path(td)
        metal_file = tmp / "cases.metal"
        air_file = tmp / "cases.air"
        lib_file = tmp / "cases.metallib"
        metal_file.write_text(METAL_SOURCE)

        subprocess.run(
            ["xcrun", "-sdk", "iphoneos", "metal", "-c", str(metal_file), "-o", str(air_file)],
            check=True,
        )
        subprocess.run(
            ["xcrun", "-sdk", "iphoneos", "metallib", str(air_file), "-o", str(lib_file)],
            check=True,
        )

        # Confirm the test fixtures genuinely exercise function_temp store_deref and FRAG_RESULT_DEPTH.
        temp_nir = subprocess.check_output([str(air2nir), str(lib_file), "fs_temp_store"], text=True)
        assert "function_temp" in temp_nir and "@store_deref" in temp_nir, (
            "fs_temp_store fixture did not emit function_temp @store_deref"
        )
        depth_nir = subprocess.check_output([str(air2nir), str(lib_file), "fs_depth_store"], text=True)
        assert "FRAG_RESULT_DEPTH" in depth_nir and "@store_deref" in depth_nir, (
            "fs_depth_store fixture did not emit FRAG_RESULT_DEPTH @store_deref"
        )

        test_bin = build_air_metal_test(build_dir, src_dir, tmp)
        res = subprocess.run(
            [str(test_bin), "--depth-store-filter", str(lib_file), "fs_temp_store", "fs_depth_store"],
            capture_output=True, text=True, check=True,
        )
        print(res.stdout.strip())
        assert "function_temp_preserved (fs_temp_store): fragment_writes_depth=0" in res.stdout
        assert "depth_store_dropped (fs_depth_store): fragment_writes_depth=0" in res.stdout
        print("test-air-frag-depth-store-filter: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
