#!/usr/bin/env python3
"""Check LLVM minnum/maxnum lowering with the existing AIR constant evaluator."""
import math
import tempfile

import intrincheck as ic


def expected(op, a, b):
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    if a == b == 0.0:
        # NIR chooses IEEE minimumNumber/maximumNumber zero ordering. This
        # also satisfies LLVM versions that permit either mixed-zero result.
        negative = (math.copysign(1, a) < 0, math.copysign(1, b) < 0)
        return -0.0 if (all(negative) if op == "maxnum" else any(negative)) else 0.0
    return max(a, b) if op == "maxnum" else min(a, b)


pairs = [
    (-3.0, 2.0), (2.0, -3.0), (1.5, 1.5),
    (0.0, 0.0), (-0.0, -0.0), (0.0, -0.0), (-0.0, 0.0),
    (float("nan"), 2.0), (2.0, float("nan")),
    (float("nan"), float("nan")),
    (float("inf"), -2.0), (-2.0, float("inf")),
    (float("-inf"), 2.0), (2.0, float("-inf")),
    (float("inf"), float("-inf")),
    (1.0, 1.0009765625), (65504.0, -65504.0),
    (2**-14, 2**-13), (2**-24, 0.0),
]
checks = 0
with tempfile.TemporaryDirectory(prefix="air-llvm-minmax-") as tmp:
    for op in ("minnum", "maxnum"):
        for base, suffix, width in (("half", "f16", 1), ("float", "f32", 1),
                                    ("double", "f64", 1), ("half", "v4f16", 4),
                                    ("float", "v4f32", 4)):
            ty = base if width == 1 else f"<{width} x {base}>"
            name = f"llvm.{op}.{suffix}"
            decl = f"declare {ty} @{name}({ty}, {ty})"
            rounding = {"half": ic.f16, "float": ic.f32, "double": float}[base]
            for index in range(len(pairs)):
                lanes = [pairs[(index + lane) % len(pairs)] for lane in range(width)]
                args = [[rounding(pair[k]) for pair in lanes] for k in (0, 1)]
                result, error = ic.emit(decl, name, [ty, ty], ty, [args], tmp, False)
                assert error is None, f"{name}: {error}"
                for lane, (a, b) in enumerate(zip(*args)):
                    want, got = expected(op, a, b), result[0][lane]
                    assert ic.same_float(got, want, base), (name, lane, a, b, got, want)
                    checks += 1
print(f"{checks} LLVM minnum/maxnum component checks passed")
