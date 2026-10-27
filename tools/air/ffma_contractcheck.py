#!/usr/bin/env python3
"""Keep AIR FMA fast-math lowering distinct from strict and contract-only calls."""

import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
AIR2NIR = Path(os.environ.get("AIR2NIR", ROOT / "build/mesa-host/build/src/compiler/air/air2nir"))
FIXTURE = ROOT / "tools/air/ffma-contract.ll"


def main():
    source = FIXTURE.read_text()
    call = "tail call fast float @air.fma.f32"
    if source.count(call) != 1:
        raise RuntimeError("fixture must contain exactly one fast AIR FMA call")

    with tempfile.TemporaryDirectory(prefix="air-ffma-") as directory:
        work = Path(directory)
        for name, flags, expected in (
            ("fast", "fast", "ffma_weak"),
            ("strict", "", "// exact"),
            ("contract", "contract", "// exact"),
        ):
            ll = work / f"{name}.ll"
            bitcode = work / f"{name}.bc"
            ll.write_text(source.replace(call, f"tail call {flags} float @air.fma.f32", 1))
            subprocess.run(
                ["clang", "-Wno-override-module", "-x", "ir", "-emit-llvm", "-c", str(ll), "-o", str(bitcode)],
                check=True,
                capture_output=True,
                text=True,
            )
            result = subprocess.run(
                [str(AIR2NIR), str(bitcode), "plane__vertex"],
                check=True,
                capture_output=True,
                text=True,
            )
            fma = [line for line in result.stdout.splitlines() if "ffma" in line]
            if len(fma) != 1 or expected not in fma[0]:
                raise RuntimeError(f"{name}: expected {expected!r}, got {fma}")
            print(f"{name}: {fma[0].strip()}")


if __name__ == "__main__":
    main()
