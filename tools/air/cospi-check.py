#!/usr/bin/env python3
"""Check AIR cospi dispatch, scalar/vector lanes and old-lowering rejection.

The exhaustive numerical check is cospi-nir.c; cospi-native.m compares
its output with Apple Metal. This script reuses the AIR integration emitter.
"""
import argparse
import ctypes
import math
import tempfile

import intrincheck as ic


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", help="old air2nir; must reject cospi")
    args = parser.parse_args()
    reference = ctypes.CDLL(None).__cospi
    reference.argtypes = [ctypes.c_double]
    reference.restype = ctypes.c_double
    values = [0.0, -0.0, 0.5, -0.5, 1, -1, 1.5, -1.5, 2, -2, 2.5,
              0.499755859375, 0.50048828125, 1.4990234375, 1.5009765625,
              2**-24, 2**-14, 0.25, 0.75, 2047, 2048, 65504, -65504,
              math.inf, -math.inf, math.nan]
    checked = 0
    with tempfile.TemporaryDirectory(prefix="air-cospi-") as tmp:
        for base, suffix, lanes in (("half", "f16", 1), ("float", "f32", 1),
                                     ("half", "v4f16", 4), ("float", "v4f32", 4)):
            ty = base if lanes == 1 else f"<{lanes} x {base}>"
            name = "air.cospi." + suffix
            decl = f"declare {ty} @{name}({ty})"
            for i in range(len(values)):
                row = [values[(i + lane) % len(values)] for lane in range(lanes)]
                got, error = ic.emit(decl, name, [ty], ty, [[row]], tmp, False)
                assert error is None, error
                for x, result in zip(row, got[0]):
                    expected = reference(x)
                    exponent = math.frexp(expected)[1] if math.isfinite(expected) else 0
                    ulp = 2**max(exponent - (11 if base == "half" else 24), -24 if base == "half" else -149)
                    assert (math.isnan(result) if math.isnan(expected) else
                            math.isfinite(result) and abs(result - expected) <= ulp * (1 if base == "half" else 4)), (name, x, result, expected)
                    if expected == 0:
                        assert result == 0, (name, x, result)
                    checked += 1
        _, error = ic.emit("declare double @air.cospi.f64(double)", "air.cospi.f64",
                           ["double"], "double", [[[0.5]]], tmp, False)
        assert error and "requires one half or float" in error, error
        print("PASS unsupported double cospi rejected")
        if args.baseline:
            ic.AIR2NIR = args.baseline
            _, error = ic.emit("declare half @air.cospi.f16(half)", "air.cospi.f16",
                               ["half"], "half", [[[0.5]]], tmp, False)
            assert error and "cospi" in error, error
            print("PASS old lowering rejects air.cospi.f16")
    print(f"PASS {checked} AIR scalar/vector cospi component checks")


if __name__ == "__main__":
    main()
