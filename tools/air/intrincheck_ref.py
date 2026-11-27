"""Reference math/float/conversion models and type parser for intrincheck.py."""
import math
import re
import struct

# ---------------------------------------------------------------------------
# precision


def f32(x):
    """x rounded once to float32, the way a shader's float holds it."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def f16(x):
    try:
        return struct.unpack("<e", struct.pack("<e", float(x)))[0]
    except (OverflowError, struct.error):
        return math.copysign(float("inf"), x)


def f64(x):
    """x as the shader's `double` holds it, which is what a Python float is."""
    return float(x)


def to_f32_bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def to_f64_bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def same_float(g, exp, rty):
    """Did the shader return the expected float, bit for bit at its own width?

    Bit patterns and not `==`, because that is what separates 0 from -0.
    AT ITS OWN WIDTH, because `to_f32_bits(f32(...))` on a `double` compares
    two numbers after rounding both to a type neither of them is -- which
    would have made air.floor.f64 agree with half its mantissa thrown away.
    """
    if math.isnan(g) and math.isnan(exp):
        return True
    if rty == "double":
        return to_f64_bits(g) == to_f64_bits(exp)
    return to_f32_bits(f32(g)) == to_f32_bits(f32(exp))


def smallest_normal(rty):
    """The threshold below which a result is not compared, by type.

    A denormal result is not compared at all (Metal compiles with
    air.compile.denorms_disable, so flushing one to zero is permitted), and
    f32's 1.17549435e-38 applied to a `double` would call 1e-300 a denormal
    and drop a case that is perfectly normal where it lives.

    A `half` deliberately keeps f32's threshold rather than its own 2^-14.
    Its own would be the defensible reading of denorms_disable, but it would
    also stop comparing every half result between 2^-14 and 2^-126 -- cases
    that are compared today and agree -- and quietly narrowing a check that
    passes is the one move this file exists to prevent. Whether a half
    denormal may be flushed is a separate question and is open.
    """
    return 2.2250738585072014e-308 if rty == "double" else 1.17549435e-38


def wrap(bits, width, signed):
    bits &= (1 << width) - 1
    if signed and bits >> (width - 1):
        bits -= 1 << width
    return bits


# ---------------------------------------------------------------------------
# the inputs
#
# Every value here is one that has broken something, in this port or in
# somebody else's: the half-way cases that separate round from rint, the
# largest float below a half-way case (0x1.fffffep-2, which the first fix for
# fast_round got wrong), the powers of two where exponent arithmetic goes
# wrong, zero of both signs, and the first float above 2^24 where consecutive
# integers stop being representable.

FLOATS = [
    0.0,
    -0.0,
    0.5,
    -0.5,
    struct.unpack("<f", struct.pack("<I", 0x3EFFFFFF))[0],  # 0x1.fffffep-2
    1.5,
    -1.5,
    2.5,
    -2.5,
    1.0,
    -1.0,
    3.7,
    -3.7,
    0.1,
    -0.1,
    2.0,
    16777216.0,  # 2^24
    16777217.0,  # not representable: becomes 2^24
    1e-8,
    1e8,
    123.456,
    -123.456,
]

# the second and third arguments want their own values, not the same ones, or
# min(a, b) is only ever tested with a == b
# -0.0 belongs here as well as in FLOATS: all four of (+-0, +-0) are separate
# cases for atan2, and three of them were wrong until this pool had them
FLOATS2 = [1.0, -1.0, 0.0, -0.0, 0.25, -0.25, 2.0, 3.0, -3.0, 0.5, 100.0, -100.0, 1e-8]

# inputs where the answer is defined but unusual. Reported separately: a
# disagreement here is worth reading, not necessarily worth fixing, because
# fast-math shaders are allowed to give up on them.
EDGE_FLOATS = [float("inf"), float("-inf"), float("nan")]

INTS = [0, 1, -1, 2, 7, -7, 127, -128, 255, 32767, -32768, 65535, 1000000, -1000000,
        2147483647, -2147483648]

# Added to INTS for an i64 operand only, because WITHOUT ONE OF THESE THE WHOLE
# 64-BIT FAMILY IS DECIDED BY VALUES WHOSE TOP WORD IS 0 OR ALL ONES. Every
# value of INTS fits in 32 bits, so a lowering that computed a 64-bit min on
# the low word alone and sign-extended it would answer correctly on all 192
# cases of air.min.u.i64: the operand whose low word is smaller is also the
# operand that is smaller.
#
# What separates the two is a value whose low word is SMALL while the value
# itself is LARGE. 2^32 is the cleanest -- low word 0, so a low-word
# comparison picks it every time -- and umin(2^32, 1) is 1 while the truncating
# answer is 2^32 or 0, neither of which is 1. The pool's first argument is
# taken whole and the others are not (inputs_for), which is enough: one operand
# of this shape in a comparison decides it.
#
# These go at the END, where they cannot move what `--cases N` keeps.
I64S = [1 << 32,              # low word 0, high word 1
        (1 << 32) - 1,        # the largest value with a zero high word
        (1 << 32) + 1,        # low word 1, high word 1: 2^32 + 1 > 2^32 - 1
        (1 << 40) + 3,
        (1 << 63) - 1,        # the largest signed i64
        -(1 << 63),           # the smallest, and 0x8000000000000000 unsigned
        -(1 << 32)]           # low word 0, high word all ones

# ---------------------------------------------------------------------------
# the reference
#
# One entry per base operation: how many arguments, what it computes, and
# whether the result is exactly defined. `exact=False` means the reference is
# this machine's libm and the shader is allowed to differ -- the number is
# printed and nothing is concluded from it.


class Undecided(ValueError):
    """A case this reference declines to answer, and the reason why.

    Not the same thing as a case that is out of range or out of domain, which
    is simply skipped: this is a case where the sources disagree about what
    the answer should be, so asserting any of them would be inventing a
    verdict. The reason travels with the exception, is counted per operation,
    and is printed -- a declined case must never look like a case that passed.

    It subclasses ValueError on purpose: emit_suite, airdefs.py and the
    run-stage generator already drop a case whose reference raises ValueError,
    so they keep working unchanged and only the folding check, which can
    report it, tells them apart.
    """


def _signed_int_result(f):
    """math.floor and friends return a Python int, which cannot hold -0. IEEE
    says floor(-0) is -0, and the shader gets that right, so the reference has
    to as well -- the first thing this check found was its own reference."""
    def g(x):
        if math.isnan(x) or math.isinf(x):
            return x
        return math.copysign(float(f(x)), x)
    return g


# The precision the operation under test computes at, set by reference()
# before it calls one of these. An operation on halves rounds every
# intermediate to half: mix(3.7h, 1h, 0.25h) is 3.0234375 that way and
# 3.02441406 with float32 intermediates, and the shader is right.
ROUND = f32


def _mix(a, b, t):
    return ROUND(a + ROUND(ROUND(b - a) * t))


def _fract(x):
    if math.isnan(x):
        return x
    if math.isinf(x):
        # Three sources, three answers, and no oracle on this machine -- so
        # this is declined rather than decided:
        #
        #   Metal says fract returns a value in [0, 1), which an infinity is
        #     not: the argument is outside the function as specified.
        #   OpenCL, whose math library MSL's is, has an edge-case table that
        #     gives fract(+-inf) = +-0.
        #   NIR's ffract is "src0 - floorf(src0)" (nir_opcodes.py), which is
        #     inf - inf, so the frontend folds a NaN -- and that is what the
        #     Adreno's instruction would have to be asked about.
        #
        # This reference used to answer `inf`, which is the one answer none of
        # the three supports: the guard above returned the argument for an
        # infinity as well as for a NaN, bypassing this function's own clamp
        # to below 1 four lines down. air.fract.f16 was folded for the first
        # time on 21 October and reported three cases wrong on the strength of
        # it.
        #
        # It is only ever asked because the pool's 2^24, 2^24+1 and 1e8
        # overflow f16, which makes air.fract.f16 the one declaration here
        # given an infinity at all -- and the device's own metal_math routes
        # `half fract(half)` through __METAL_MAYBE_FAST_MATH__, which
        # metal_common defines as __METAL_FAST_MATH__ whenever __FAST_MATH__
        # is set. The corpus says it is: its modules carry `fmul fast` and
        # air.compile.denorms_disable. Under fast math an implementation is
        # permitted to assume there are no infinities at all. docs/air.md 4.
        raise Undecided("fract of an infinity: Metal's result is in [0, 1), "
                        "OpenCL's edge table says +-0, NIR's ffract computes "
                        "inf - inf = nan, and nothing here can decide between "
                        "them")
    # the sign of floor's zero decides the sign of this zero:
    # -0 - (-0) is +0, but -0 - 0 is -0, and Python's math.floor returns an
    # int, which has no -0 to carry
    r = ROUND(x - math.copysign(float(math.floor(x)), x))
    # Metal: "returns a value in [0, 1)", so a result that rounds up to 1 is
    # clamped to the largest value below it in this precision
    return min(r, ROUND(1.0 - ROUND(2.0 ** -12)))


def _sign(x):
    if math.isnan(x):
        return 0.0
    if x > 0:
        return 1.0
    if x < 0:
        return -1.0
    return x  # keeps the sign of zero


def _roundf(x):
    """C roundf: half away from zero, which is Metal's round."""
    if math.isnan(x) or math.isinf(x) or x == 0.0:
        return x
    a = math.floor(abs(x) + 0.5)
    # floor(|x| + 0.5) is wrong where |x| + 0.5 rounds up in float; do it the
    # way the shader does, and the way roundf is specified
    t = math.trunc(abs(x))
    a = t + 1 if abs(x) - t >= 0.5 else t
    return math.copysign(a, x)


def _fmin(a, b):
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    if a == b == 0.0:  # fmin(+0, -0) may be either; Metal does not say
        return b if math.copysign(1, b) < 0 else a
    return min(a, b)


def _fmax(a, b):
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    if a == b == 0.0:
        return b if math.copysign(1, b) > 0 else a
    return max(a, b)


def _clamp_f(x, lo, hi):
    return _fmin(_fmax(x, lo), hi)


def _powr(x, y):
    # powr is pow for x >= 0 only, undefined elsewhere
    if x < 0:
        return float("nan")
    return math.pow(x, y)


# Python raises where C returns a value. Metal's math functions are OpenCL's,
# which keep the IEEE results at the edge of the domain, so raising here would
# silently drop the cases that matter most -- it had already dropped every case
# of air.fast_sqrt.v4f32, whose rotated components always include a negative.
def _sqrt(x):
    if math.isnan(x):
        return x
    return float("nan") if x < 0 else math.sqrt(x)   # sqrt(-0) is -0


def _log(f, zero):
    def g(x):
        if math.isnan(x):
            return x
        if x == 0.0:
            return zero
        return float("nan") if x < 0 else f(x)
    return g


def _rsqrt(x):
    if math.isnan(x):
        return x
    if x == 0.0:
        return math.copysign(float("inf"), x)
    return float("nan") if x < 0 else 1.0 / math.sqrt(x)


def _fmod(x, y):
    if y == 0.0 or math.isnan(x) or math.isnan(y) or math.isinf(x):
        return float("nan")
    return math.fmod(x, y)


def _pow(x, y):
    # C: pow(+-0, y) for a negative odd integer y is +-inf, keeping the zero's
    # sign; for any other negative y it is +inf. Python raises for both, and
    # the fallback that guessed +inf made the shader's correct -inf for
    # pow(-0, -1) look like an infinite-ulp disagreement.
    if x == 0.0 and y < 0:
        odd = y == int(y) and int(y) % 2 != 0
        return math.copysign(float("inf"), x) if odd else float("inf")
    try:
        return math.pow(x, y)
    except ValueError:
        return float("nan")        # x < 0 with a fractional y
    except OverflowError:
        return math.copysign(float("inf"), x if x < 0 and y == int(y)
                             and int(y) % 2 != 0 else 1.0)


def _inverse_trig(f):
    """asin and acos, which are NaN outside [-1, 1] rather than absent.

    Python raises where C returns a value, and a raise here would drop the
    cases that decide the most: the shader's lowering goes through a square
    root of (1 - x)(1 + x), so an argument outside the domain is exactly the
    one that must come back NaN and not a plausible angle. The same reason
    _sqrt above returns NaN rather than raising.
    """
    def g(x):
        if math.isnan(x):
            return x
        if x < -1.0 or x > 1.0:
            return float("nan")
        return f(x)
    return g


def _hyperbolic(f, odd):
    """sinh and cosh, whose answer for a large argument is an infinity.

    math.sinh raises OverflowError where the shader -- and C, and OpenCL,
    whose library Metal's is -- gives +-inf. Skipping those would leave the
    pool's 1e8 and 2^24 untested, which are the only arguments that reach the
    saturation at all. sinh is odd and keeps the argument's sign; cosh is even
    and overflows to +inf from either side.
    """
    def g(x):
        try:
            return f(x)
        except OverflowError:
            return math.copysign(float("inf"), x) if odd else float("inf")
    return g


def _ldexp(x, n):
    try:
        return math.ldexp(x, n)
    except OverflowError:
        return math.copysign(float("inf"), x)


OPS = {
    # name:        (nargs, fn, exact)
    "floor":       (1, _signed_int_result(math.floor), True),
    "ceil":        (1, _signed_int_result(math.ceil), True),
    "trunc":       (1, _signed_int_result(math.trunc), True),
    "rint":        (1, _signed_int_result(round), True),
    "round":       (1, _roundf, True),
    "fabs":        (1, abs, True),
    "fract":       (1, _fract, True),
    "sign":        (1, _sign, True),
    "saturate":    (1, lambda x: _clamp_f(x, 0.0, 1.0), True),
    "fmin":        (2, _fmin, True),
    "fmax":        (2, _fmax, True),
    "clamp":       (3, _clamp_f, True),
    "mix":         (3, _mix, True),
    "fmod":        (2, _fmod, True),
    "mod":         (2, _fmod, True),
    "divide":      (2, lambda a, b: a / b if b else math.copysign(float("inf"), a) if a else float("nan"), True),
    "sqrt":        (1, _sqrt, True),      # IEEE: exactly rounded
    "fma":         (3, lambda a, b, c: math.fma(a, b, c), True),
    # llvm.fmuladd, which this frontend translates FUSED -- one rounding, the
    # same as fma. Counted as exactly defined although the intrinsic itself is
    # not: LLVM's reference leaves it unspecified whether the product is
    # rounded first, so a disagreement here does not mean the frontend is
    # wrong, it means the frontend changed its mind about which of the two
    # legal answers it gives. That is worth catching either way, and the
    # softpipe run stage sets it aside beside air.fma for the reason written
    # there (TGSI's MAD is allowed to round the product, and does).
    "fmuladd":     (3, lambda a, b, c: math.fma(a, b, c), True),
    "ldexp":       (2, _ldexp, True),
    # approximate: the reference is libm, the shader is allowed to differ
    "sin":         (1, math.sin, False),
    "cos":         (1, math.cos, False),
    "tan":         (1, math.tan, False),
    "atan2":       (2, math.atan2, False),
    # The inverse trigonometric and hyperbolic ones. Approximate for the same
    # reason as sin and cos -- the reference is this machine's libm, and
    # Metal's fast_ prefix licenses a deviation -- but the number is still
    # worth having: the lowering of acos and asin goes through atan2, whose
    # own polynomial is Mesa's, so a large ulp count here is a measurement of
    # that polynomial and a wrong formula would show as a quadrant, a sign or
    # a NaN rather than as a few ulp.
    "acos":        (1, _inverse_trig(math.acos), False),
    "asin":        (1, _inverse_trig(math.asin), False),
    "atan":        (1, math.atan, False),
    "sinh":        (1, _hyperbolic(math.sinh, True), False),
    "cosh":        (1, _hyperbolic(math.cosh, False), False),
    "tanh":        (1, math.tanh, False),
    "exp":         (1, math.exp, False),
    "exp2":        (1, lambda x: math.pow(2.0, x), False),
    "exp10":       (1, lambda x: math.pow(10.0, x), False),
    "log":         (1, _log(math.log, float("-inf")), False),
    "log2":        (1, _log(math.log2, float("-inf")), False),
    "log10":       (1, _log(math.log10, float("-inf")), False),
    "pow":         (2, _pow, False),
    "powr":        (2, _powr, False),
    "rsqrt":       (1, _rsqrt, False),
}

# integer operations, by (base name, signed)
IOPS = {
    ("min", True):  lambda w: (2, min),
    ("min", False): lambda w: (2, min),
    ("max", True):  lambda w: (2, max),
    ("max", False): lambda w: (2, max),
    ("clamp", True): lambda w: (3, lambda x, lo, hi: min(max(x, lo), hi)),
    ("clamp", False): lambda w: (3, lambda x, lo, hi: min(max(x, lo), hi)),
}


# The one set-aside this check applies to a DISAGREEMENT rather than to a case
# nobody can ask. It is in the same family as the fract-of-an-infinity case the
# reference declines and the narrow types softpipe cannot run: a difference
# whose cause is written down, whose extent is a stated condition, and which is
# counted and named rather than either hidden or left as an unknown failure.
#
# What is being set aside: air_fmod computes fma(-y, trunc(x/y), x) with the
# quotient corrected by one either way and the sign of a zero taken from the
# dividend. That is bit-exact for every pair whose quotient is exactly
# representable and CANNOT be for any other -- measured, not argued: over
# 195 906 random pairs of normal floats every disagreement with this machine's
# libm has |x/y| >= 2^24, and over 376 438 whose quotient is representable
# there is none. The exact answer above that needs an iterated reduction of
# twelve passes (each removes only 22 bits of an exponent difference that
# reaches 253), and its exactness ON THE ADRENO cannot be established from this
# machine at all, because ir3 lowers fdiv to fmul(a, frcp(b)) and frcp is
# approximate. Shipping it would mean shipping a routine whose only claim
# cannot be checked here, so it is not shipped and this is the honest verdict
# instead.
#
# WHAT MAKES IT NARROW RATHER THAN AN EXCUSE: it is decided case by case, not
# per declaration. A disagreement that does not satisfy the condition is a
# failure and fails the stage, which is the whole point -- if the Adreno's
# frcp costs more than air_fmod's two corrections can absorb, the cases that
# show it have |x/y| < 2^24 and will come through as WRONG.
FMOD_ASIDE = (
    "fmod with |x/y| >= 2^24 (2^11 for a half): trunc(x/y) is not exactly "
    "representable there, so no correction of a bounded size can recover it, "
    "and the exact answer needs a twelve-pass reduction whose exactness on the "
    "Adreno cannot be established on this machine because ir3 lowers fdiv to "
    "fmul(a, frcp(b)). Decided case by case -- a disagreement that does not "
    "satisfy it fails. air_fmod in air_intrinsics.c, docs/air.md section 4")


def fmod_aside(args, c, rty):
    """Is THIS differing case of an fmod one the lowering never claimed?

    True only for a finite pair with a non-zero divisor whose quotient is
    outside the type's exactly representable integers. Everything else --
    including a disagreement at a small ratio, which is what a wrong lowering
    would look like -- is a failure.
    """
    if len(args) < 2:
        return False
    try:
        rnd = f16 if rty == "half" else f64 if rty == "double" else f32
        x = rnd(args[0][c] if len(args[0]) > c else args[0][0])
        y = rnd(args[1][c] if len(args[1]) > c else args[1][0])
    except (TypeError, IndexError, OverflowError, ValueError):
        return False
    if not (math.isfinite(x) and math.isfinite(y)) or y == 0.0:
        return False
    try:
        return abs(x / y) >= 2.0 ** (11 if rty == "half" else 24)
    except (ZeroDivisionError, OverflowError):
        return False


def ref_elementwise(op, args, argtys, signed):
    """One component of an elementwise operation.

    The arguments must be rounded to the shader's precision first. mix(3.7, 1,
    1) is exactly 1 in float32 and 0.99999994 in double, and this check
    reported the shader wrong for a while because the reference was given
    Python's 3.7 and the shader f32(3.7). Everything insensitive to the low
    bits of its input -- floor, min, clamp -- hid the mistake.
    """
    bty, _ = parse_type(argtys[0])
    if bty.startswith("i"):
        # integer min/max/clamp: no rounding anywhere, and the result is
        # whatever the declared width holds
        w = int(bty[1:])
        fn = {"min": min, "max": max,
              "clamp": lambda x, lo, hi: min(max(x, lo), hi)}[op]
        # air.min.u is an unsigned minimum: umin(0, 0xffff) is 0, not -1, and
        # taking the declaration's `u` for granted as signed made the shader
        # look wrong on 175 of 256 cases
        return wrap(fn(*[wrap(v, w, bool(signed)) for v in args]), w, bool(signed))

    global ROUND
    # AT THE RESULT'S OWN PRECISION. `f32` for a double would round the answer
    # to 24 bits of mantissa and then compare it with a shader that kept 53,
    # which is how air.floor.f64 would have "agreed" about something else.
    ROUND = f16 if bty == "half" else f64 if bty == "double" else f32
    nargs, fn, exact = OPS[op]
    rounded = [ROUND(v) if parse_type(ty)[0] in ("half", "float") else v
               for ty, v in zip(argtys, args)]
    return ROUND(fn(*rounded))


def ref_convert(name, retty, srcty, v):
    """air.convert.<dk>.<dt>.<sk>.<st>(v), one component.

    The two kind letters are not decoration. `s`/`u` on the source say how its
    integer is read, which decides sign- against zero-extension; `s`/`u` on the
    destination say how the result is read, which decides whether a float is
    converted signed or unsigned and what a narrowing wraps to. The corpus has
    air.convert.u.v2i16.s.v2i32 -- a signed 32-bit value narrowed to an
    unsigned 16-bit one -- where getting either letter the wrong way round
    still produces a plausible number.
    """
    parts = name.split(".")
    dk, sk = parts[2], parts[4]
    dbase, _ = parse_type(retty)
    sbase, _ = parse_type(srcty)

    if dbase == "i1":
        # A BOOL DESTINATION IS NOT A CONVERSION, and this reference used to
        # treat it as one: truncate toward zero, then keep one bit. That makes
        # bool(0.1) false, and bool(2.0) false again, which is not C's rule,
        # not C++11's, and not the one MSL inherits from either -- the rule is
        # `x != 0`.
        #
        # Apple's own compiler says the same thing by what it does with the
        # result. Every call to air.convert.u.i1.f.f32 in the device image --
        # two in QuartzCore's default.metallib, two in AppleCVAPhoto's
        # cvadepth-iOS.metallib -- feeds a `br i1` directly, e.g.
        #
        #     %590 = fpext half %336 to float
        #     %591 = tail call i1 @air.convert.u.i1.f.f32(float %590)
        #     br i1 %591, label %592, label %593
        #
        # which is an `if (x)` on a floating-point value. Under a truncating
        # reading that branch would also be a branch on poison, because LLVM's
        # own rule for `fptoui float to i1` is poison whenever the truncated
        # value does not fit in the destination, which is every |x| >= 2. No
        # compiler emits that for a condition.
        #
        # air_intrinsics.c lowers this as a comparison and says why
        # ("A BOOL DESTINATION IS NOT A CONVERSION", nir_fneu_imm): the
        # reference is what was wrong. Nothing had evaluated it before. The
        # declaration only entered this check's list late on 21 October --
        # 688f5de's own commit message records check.sh still reading
        # "intrincheck 71/0" with that commit's f2u1 fix already in -- so an
        # expectation nobody had ever folded had no reason to be right.
        if sbase in ("half", "float"):
            x = f16(v) if sbase == "half" else f32(v)
            truth = x != 0.0          # a NaN is true, as `fcmp une` makes it
        else:
            truth = wrap(v, int(sbase[1:]), sk == "s") != 0
        return wrap(1 if truth else 0, 1, dk == "s")

    if sbase in ("half", "float"):
        x = f16(v) if sbase == "half" else f32(v)
        if dbase in ("half", "float"):
            # FLOAT -> FLOAT, which this branch used to fall straight through
            # into the float -> int code below and crash in it: `int("alf")`
            # for a `half` destination, `int("loat")` for a `float` one. Every
            # case of the six float/half conversions the corpus declares --
            # air.convert.f.v{2,3,4}f16.f.v{2,3,4}f32 and the three the other
            # way -- therefore raised ValueError, was dropped by the bare
            # `continue` that used to catch it, and left the declaration
            # reported as "agrees on every case" over ZERO cases. 123 cases
            # and 6 declarations. It took counting the dropped cases to see
            # it, which is the whole argument for counting them.
            #
            # The answer is one rounding to the destination's precision:
            # exact widening one way, round-to-nearest-even the other. An
            # infinity and a NaN pass through, and f16() turns an overflow
            # into the infinity f32 -> f16 must produce.
            return f16(x) if dbase == "half" else f32(x)
        # float -> int: Metal truncates toward zero. Out of the destination's
        # range it is undefined, in C and in the MSL specification both, so
        # those are not cases.
        if math.isnan(x) or math.isinf(x):
            raise ValueError("not defined")
        w = int(dbase[1:])
        t = math.trunc(x)
        lo, hi = ((-(1 << (w - 1)), (1 << (w - 1)) - 1) if dk == "s"
                  else (0, (1 << w) - 1))
        if t < lo or t > hi:
            raise ValueError("outside the destination's range")
        return wrap(t, w, dk == "s")

    w = int(sbase[1:])
    x = wrap(v, w, sk == "s")
    if dbase in ("half", "float"):
        # int -> float, rounded once to the destination's precision
        return f16(x) if dbase == "half" else f32(x)
    return wrap(x, int(dbase[1:]), dk == "s")


def reference(family, op, name, args, argtys, retty, signed):
    """The reference result for one case, as a list of result components.

    `args` is one list per argument, holding that argument's components.
    """
    rty, rn = parse_type(retty)

    if family == "dot":
        # The inputs are small dyadic rationals, so every product and every
        # partial sum is exact and the answer does not depend on the order the
        # accumulation is taken in -- checked, not assumed: over all 300 cases
        # of the DOT_FLOATS pool the left fold, the right fold, the pairwise
        # order NIR's own fdot3/fdot4 constant expressions use ((p2+p1)+p0 and
        # (p3+p2)+(p1+p0)) and the once-rounded exact sum are one and the same
        # number, in f16 as in f32. What this pins is which components are
        # paired with which -- a lowering that dropped a component or paired
        # them crosswise gives a different number -- and not the rounding of a
        # general dot product, which it cannot.
        #
        # THE PRECISION IS THE RESULT'S, not f32. This branch rounded to f32
        # whatever the declaration returned, and for a half dot product that
        # is an answer the type cannot hold: (-0.25, 8, -16) against itself is
        # exactly 320.0625, and f16's step at 320 is 0.25, so 320 is the only
        # thing a `half` can be. air.dot.v3f16 and air.dot.v4f16 were folded
        # for the first time on 21 October and were duly reported wrong on the
        # three of their 200 cases whose exact sum wants a 13-bit significand
        # from a type that has 11 (air.dot.v2f16 never reaches one). Three say
        # the same 320: f16 arithmetic itself, llc on this processor through
        # the half `fmul`/`fadd` bodies of airdefs.py, and air2nir's folder.
        # The reference was the only one of the four that said otherwise.
        rnd = f16 if rty == "half" else f32
        n = parse_type(argtys[0])[1]
        acc = 0.0
        for k in range(n):
            # +0.0 + p0, then + p1 ...: the order airdefs.py's IR emits, so
            # the two references stay comparable instruction for instruction
            acc = rnd(acc + rnd(rnd(args[0][k]) * rnd(args[1][k])))
        return [acc]

    if family in ("all", "any"):
        n = parse_type(argtys[0])[1]
        v = args[0][:n]
        return [1 if (all(v) if family == "all" else any(v)) else 0]

    if family == "convert":
        return [ref_convert(name, retty, argtys[0], v) for v in args[0][:rn]]

    out = []
    for k in range(rn):
        # a scalar argument of a vector operation would be broadcast; none of
        # the corpus's declarations mix the two, but take component 0 if so
        per = [a[k] if len(a) > k else a[0] for a in args]
        out.append(ref_elementwise(op, per, argtys, signed))
    return out


# ---------------------------------------------------------------------------
# the declarations
#
# air.<op>.<type>                       air.fast_round.f32
# air.<op>.<s|u>.<type>                 air.clamp.s.v2i16
# air.convert.<f|s|u>.<dst>.<f|s|u>.<src>
# air.dot.<type>                        returns a scalar from two vectors

# air.* and the handful of llvm.* this frontend translates. llvm.fmuladd and
# llvm.fabs belong in this check for the same reason as the arithmetic they sit
# among: they are operations with a reference, 124 of Resident Evil 4's modules
# stop at one of them when it is missing, and nothing else here would notice if
# the lowering changed.
DECL = re.compile(r"^declare\s+(.*?)\s+@((?:air|llvm)\.[A-Za-z0-9_.]+)\((.*)\)")


def parse_type(s):
    """'<4 x float>' -> ('float', 4); 'i32' -> ('i32', 1)"""
    s = s.strip()
    m = re.match(r"<(\d+) x (.+)>$", s)
    if m:
        return m.group(2).strip(), int(m.group(1))
    return s, 1


