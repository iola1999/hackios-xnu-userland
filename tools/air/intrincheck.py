#!/usr/bin/env python3
"""Check every air.* arithmetic operation the corpus uses against a reference.

The corpus exercises each operation only incidentally: a shader that calls
air.fast_round once and a hundred other things tells us the module translated,
not that the rounding is right. One of these was wrong for a week -- fast_round
was lowered to nir_fround_even, which is rint, and the two differ at every
half-way value -- and nothing in the 456-module corpus could have shown it,
because no module's output was ever compared with a number computed any other
way.

So the operations are tested one at a time, on inputs chosen to break them:

  - air2nir --list over the corpus gives the declarations that are actually
    used (78 arithmetic ones today). Apple's vocabulary in AGXCompilerCore has
    137 names; the ones no module calls are not translated and not tested, and
    this prints that difference rather than hiding it.
  - for each declaration a fragment shader is generated that calls it on
    constant arguments and returns the result as its render target, llvm-as
    assembles it, and `air2nir --eval` folds it. Nothing is read from the
    corpus but the list of names.
  - the reference is this file's Python, which for every exactly-defined
    operation is the IEEE result rounded once to the shader's precision.

What a disagreement means, in order of what it is most likely to be: the
lowering in air_intrinsics.c is the wrong NIR opcode (fast_round); the
reference here is the wrong Metal semantics; NIR's constant folder differs
from libm. The third has never happened yet.

What this does NOT prove, and no amount of it will:

  - The reference for a transcendental is this machine's libm, not Apple's and
    not the Adreno's. Metal's `fast_` prefix *permits* a lower-precision
    implementation, so the printed difference for fast_sin is a measurement,
    not a verdict. Those are reported separately and are never counted as
    exact. The only thing checked there is that the lowering picked an
    operation of the right shape.
  - NIR's constant folder is not the Adreno. A difference between what
    nir_opt_constant_folding computes for nir_ffloor and what the hardware's
    floor instruction computes would be invisible here. Checking that needs
    the phone, and for these operations it has not been done.
  - Denormals. Apple compiles with air.compile.denorms_disable, so the
    hardware may flush where this check does not. Inputs are kept normal.

usage: intrincheck.py [-v] [--decls FILE] [OP ...]           fold it, exhaustively
       intrincheck.py --run [--few] [--cases N] [OP ...]     run it through a real driver
       intrincheck.py --emit DIR [--few] [--cases N]         write it out to carry somewhere
       intrincheck.py --check DIR RUN.LOG                    judge what the runner printed
"""

import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir")
)

from intrincheck_ref import *  # noqa: F401,F403

# ---------------------------------------------------------------------------
# how a result is carried back through a four-lane render target
#
# A render target lane is 32 bits wide -- air_run reads back through
# R32G32B32A32, and --eval prints the four components of an int4 or float4 --
# so a result component that is wider than 32 bits has to be carried in two
# lanes and put back together here.
#
# It was not. `trunc i64 %c to i32` was emitted into the generated shader and
# the low half was then compared against a 64-bit reference, so EVERY i64
# result whose top word was not zero was reported as the frontend's mistake:
# 16 declarations and 2 164 cases, with `air.min.u.i64(-1, -1)` reading "got
# 4294967295 want 18446744073709551615". The frontend was right all along --
# air2nir folds that call to 0xffffffffffffffff and softpipe executes it to
# the same bits (docs/air.md section 4) -- and the truncation was the
# harness's own. A measurement instrument that quietly drops the top half of
# what it measures is worse than no instrument: this one accused the compiler.


def lanes_per_comp(bty):
    """32-bit render-target lanes one component of type `bty` occupies.

    A `double` is here for the same reason an `i64` is, and it was NOT: the
    generated shader put a double straight into a <4 x float> and llvm-as said
    so -- "'%c0' defined with type 'double' but expected 'float'" -- which was
    reported as `2 could not be built or did not fold`, a REFUSED row whose
    reason was a formatting mistake in this harness and nothing about the
    frontend. The frontend is fine: air.floor.f64(0x1.ccccccccccccdp+0) folds
    to 0x3FF0000000000000, exactly 1.0. So a double is carried as its two
    32-bit halves through an int4 target and put back together by unlane,
    exactly as an i64 is, and compared at 64 bits by same_float.
    """
    return 2 if bty in ("i64", "double") else 1


def comp_batches(retty):
    """[(components, cases per shader)]: how to split a result over shaders.

    A four-lane target holds `4 // lanes_per_comp` components, so a v4i64
    result does not fit in one shader at all and its components are split over
    two. Returning the split rather than assuming one shader per case is what
    keeps a component from being silently dropped -- the packing below cuts
    the lane list to four, so a v4i64 would have had components 2 and 3
    quietly never measured.
    """
    bty, rn = parse_type(retty)
    lanes = lanes_per_comp(bty)
    per_group = max(1, 4 // lanes)
    out = []
    for k in range(0, rn, per_group):
        comps = list(range(k, min(k + per_group, rn)))
        out.append((comps, max(1, 4 // (lanes * len(comps)))))
    return out


def unlane(flat, ncases, comps, bty, signed):
    """The four lanes of one shader's target, back as [[component]] per case.

    `flat` holds the lanes as they were printed: a float for a float target,
    an unsigned 32-bit word for an integer one.
    """
    lanes = lanes_per_comp(bty)
    out = []
    for i in range(ncases):
        row = []
        for p in range(len(comps)):
            base = (i * len(comps) + p) * lanes
            if base + lanes > len(flat):
                return None          # a short row is the caller's to report
            if lanes == 2:
                bits = ((flat[base] & 0xFFFFFFFF)
                        | ((flat[base + 1] & 0xFFFFFFFF) << 32))
                if bty == "double":
                    row.append(struct.unpack("<d", struct.pack("<Q", bits))[0])
                else:
                    row.append(wrap(bits, 64, bool(signed)))
            elif isinstance(flat[base], float):
                row.append(flat[base])
            else:
                row.append(wrap(flat[base], 32, bool(signed)))
        out.append(row)
    return out


def not_run_at_all(why):
    """Does this reason mean the BINARY could not be run, rather than that the
    library could not be read?

    The distinction decides whether a run is contaminated or merely limited.
    `not LLVM bitcode` is a fact about the library and will say the same thing
    tomorrow; EACCES, exit 126 and exit 127 are a fact about the moment -- this
    tree is shared and air2nir is relinked under the check -- so a run that saw
    one measured less than it thinks and must not be reported as a pass.
    """
    return ("Permission denied" in why or "Text file busy" in why
            or "No such file" in why
            or why.startswith("exit 126") or why.startswith("exit 127"))


def collect_declarations(libs):
    """(the declarations, the libraries that could not be read).

    THE SECOND HALF OF THAT PAIR IS THE POINT. This used to be `except
    OSError: continue` over a subprocess whose exit code was never looked at,
    so a library air2nir could not be executed for -- and this tree is shared,
    so the binary is relinked under the check several times a night, which is
    EACCES or exit 126 -- dropped out of the vocabulary without a word. Fewer
    declarations is fewer operations tested and a HIGHER pass rate, which is
    the one direction a broken meter must never move in: older logs read
    "134 agree, 0 differ" where the vocabulary is 553 declarations wide.
    A library that was not read is named and counted by the caller.
    """
    out = set()
    unread = []
    for lib in libs:
        try:
            q = subprocess.run([AIR2NIR, "--ll", lib], capture_output=True)
        except OSError as e:
            unread.append((lib, str(e)))
            continue
        if q.returncode:
            err = q.stderr.decode("utf-8", "replace").strip().splitlines()
            unread.append((lib, "exit %d%s" % (q.returncode,
                                               ": " + err[-1] if err else "")))
            continue
        for line in q.stdout.decode("utf-8", "replace").splitlines():
            if line.startswith("declare ") and ("@air." in line or
                    re.search(r"@llvm\.(fmuladd|fabs)\.", line)):
                line = re.sub(r"\s+local_unnamed_addr.*$", "", line)
                line = re.sub(r"\s+#\d+$", "", line)
                out.add(line.strip())
    return sorted(out), unread


# ---------------------------------------------------------------------------
# the generated shader

SKELETON = """source_filename = "ic"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%(decl)s

define <{ <4 x %(outty)s> }> @ic() local_unnamed_addr {
%(body)s
  %%r = insertvalue <{ <4 x %(outty)s> }> undef, <4 x %(outty)s> %%v3, 0
  ret <{ <4 x %(outty)s> }> %%r
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ic, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"%(outname)s", !"air.arg_name", !"out"}
!6 = !{}
"""

# The same shader with its arguments read from a constant buffer instead of
# written in as immediates. That is the only form worth running on a GPU: ir3
# folds, so air.fast_round(0.5) written as a constant is answered by the
# compiler and the hardware never sees the instruction -- the test would pass
# with the GPU switched off. A value loaded from a buffer cannot be folded, and
# air_run puts bytes we choose there (AIR_RUN_BUFFER).
BUF_SKELETON = """source_filename = "ic"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%(decl)s

define <{ <4 x %(outty)s> }> @ic(ptr addrspace(2) %%in) local_unnamed_addr {
%(body)s
  %%r = insertvalue <{ <4 x %(outty)s> }> undef, <4 x %(outty)s> %%v3, 0
  ret <{ <4 x %(outty)s> }> %%r
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ic, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"%(outname)s", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 4096, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"in"}
"""

# One case's arguments occupy MAXARG * SLOTS_PER_ARG four-byte slots of that
# buffer, so a component's address is arithmetic on its indices and nothing has
# to be packed tightly.
#
# SLOTS_PER_ARG is eight and not four because an i64 component takes two slots:
# with four, a `load i64` of component 0 of a v4i64 argument read its own low
# word and COMPONENT 1's low word as its high word, so every 64-bit vector
# operand in the run stage was fed a number nobody chose. The slots are also
# the reason a component's address stays 8-byte aligned: 24 slots a case, 8 a
# argument, 2 a component, all even.
MAXARG = 3
SLOTS_PER_ARG = 8
SLOTS_PER_CASE = MAXARG * SLOTS_PER_ARG


def slot_bytes(bty, v):
    """The slots holding `v` as type `bty`, little-endian: 4 bytes, or 8 for i64.

    A narrow type is written into the low bytes of its slot, which is where a
    little-endian load of that width at a four-byte-aligned address reads it.
    An i64 gets both of its slots -- it used to get `struct.pack("<i", ...)` of
    its low word alone, and the load then took the next slot as its high word.
    """
    if bty == "float":
        return struct.pack("<f", f32(v))
    if bty == "half":
        h = f16(v)
        b = (struct.pack("<H", 0x7C00 if h > 0 else 0xFC00) if math.isinf(h)
             else struct.pack("<e", h))
        return b + b"\0\0"
    if bty == "double":
        return struct.pack("<d", v)
    w = int(bty[1:])
    if w == 1:
        return struct.pack("<B", 1 if v else 0) + b"\0\0\0"
    if w == 64:
        return struct.pack("<q", wrap(v, 64, True))
    return struct.pack("<i", wrap(v, 32, True) if w >= 32
                       else wrap(v, w, True) & ((1 << w) - 1))


def split64(lines, i, k, s_):
    """Append the IR that splits one i64 into its low and high words.

    Two lanes of the render target, low first, which is what unlane() puts
    back together. `lshr` and not `ashr`: the halves are bit patterns here and
    the sign is applied once, to the whole 64-bit value, by wrap() in unlane.
    """
    lo = "%%lo%d_%d" % (i, k)
    sh = "%%sh%d_%d" % (i, k)
    hi = "%%hi%d_%d" % (i, k)
    lines.append("  %s = trunc i64 %s to i32" % (lo, s_))
    lines.append("  %s = lshr i64 %s, 32" % (sh, s_))
    lines.append("  %s = trunc i64 %s to i32" % (hi, sh))
    return [lo, hi]


def emit_buffered(decl, name, argtys, retty, cases, signed, rcomps=None):
    """The .ll of a shader that loads its arguments, and the bytes to load.

    Returns (source, input bytes). The caller assembles the source and gives
    the bytes to air_run.

    `rcomps` are the result components this shader carries back, which is all
    of them for every type but i64: an i64 component needs two of the target's
    four 32-bit lanes, so a v4i64 result is split over two shaders. See
    comp_batches.

    A half result is written to a HALF render target rather than widened into a
    float4. The widening used to happen inside the shader (fpext half to float),
    and NIR then folded the conversion that produced the value into a 32-bit
    one: air.convert.f.f16.u.i32 came out as four cov.u32f32 and not one
    cov.u32f16, so the shader computed f32(x) while the reference computed
    f16(x) and the comparison meant nothing. Ten declarations of the corpus
    return half and every one of them had to be set aside for it.

    half4 as a render target type name is not invented here: Apple's own
    GNR.videoprocessor library, built by the same compiler for the same AIR
    version, has 24 such declarations across 18 fragment entry points --
    !"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"half4" over a
    define returning <4 x half>. It is not attested by the 456-module corpus,
    which is a statement about how that corpus was collected (.metallib files
    in rootfs, not libraries embedded in dylibs) and not about Metal.
    """
    rty, rn = parse_type(retty)
    outty = "half" if rty == "half" else "float" if rty == "float" else "i32"
    outname = {"half": "half4", "float": "float4"}.get(outty, "int4")
    if rcomps is None:
        rcomps = list(range(rn))

    data = bytearray()
    lines = []
    slots = []
    for i, args in enumerate(cases):
        while len(data) < (i + 1) * SLOTS_PER_CASE * 4:
            data += b"\0\0\0\0"
        argvals = []
        for j, (ty, v) in enumerate(zip(argtys, args)):
            bty, bn = parse_type(ty)
            cslots = lanes_per_comp(bty)
            align = 8 if cslots == 2 else 4
            names = []
            for c in range(bn):
                slot = i * SLOTS_PER_CASE + j * SLOTS_PER_ARG + c * cslots
                b = slot_bytes(bty, v[c])
                data[slot * 4:slot * 4 + len(b)] = b
                nm = "%%l%d_%d_%d" % (i, j, c)
                lines.append("  %sp = getelementptr inbounds i8, "
                             "ptr addrspace(2) %%in, i64 %d" % (nm, slot * 4))
                lines.append("  %s = load %s, ptr addrspace(2) %sp, align %d"
                             % (nm, bty, nm, align))
                names.append(nm)
            if bn == 1:
                argvals.append("%s %s" % (bty, names[0]))
            else:
                prev = "undef"
                for c, nm in enumerate(names):
                    vv = "%%g%d_%d_%d" % (i, j, c)
                    lines.append("  %s = insertelement <%d x %s> %s, %s %s, i32 %d"
                                 % (vv, bn, bty, prev, bty, nm, c))
                    prev = vv
                argvals.append("<%d x %s> %s" % (bn, bty, prev))

        lines.append("  %%c%d = call %s @%s(%s)"
                     % (i, retty, name, ", ".join(argvals)))
        for k in rcomps:
            s_ = "%%c%d" % i
            if rn > 1:
                s_ = "%%e%d_%d" % (i, k)
                lines.append("  %s = extractelement %s %%c%d, i32 %d"
                             % (s_, retty, i, k))
            if rty == "half" and outty == "float":
                lines.append("  %%f%d_%d = fpext half %s to float" % (i, k, s_))
                slots.append("%%f%d_%d" % (i, k))
            elif rty in ("i1", "i8", "i16") and outty == "i32":
                lines.append("  %%f%d_%d = %s %s %s to i32"
                             % (i, k, "sext" if signed else "zext", rty, s_))
                slots.append("%%f%d_%d" % (i, k))
            elif rty == "i64" and outty == "i32":
                slots.extend(split64(lines, i, k, s_))
            elif rty == "double" and outty == "i32":
                lines.append("  %%b%d_%d = bitcast double %s to i64" % (i, k, s_))
                slots.extend(split64(lines, i, k, "%%b%d_%d" % (i, k)))
            else:
                slots.append(s_)

    pad = "0xH0000" if outty == "half" else "0.0" if outty == "float" else "0"
    if len(slots) > 4:
        raise AssertionError(
            "%s: %d lanes for a four-lane render target; comp_batches and this "
            "emitter disagree" % (name, len(slots)))
    packed = (slots + [pad] * 4)[:4]
    prev = "undef"
    for i, s_ in enumerate(packed):
        lines.append("  %%v%d = insertelement <4 x %s> %s, %s %s, i32 %d"
                     % (i, outty, prev, outty, s_, i))
        prev = "%%v%d" % i

    return (BUF_SKELETON % dict(decl=decl, outty=outty, outname=outname,
                                body="\n".join(lines)),
            bytes(data), outname)



def fconst(ty, x):
    """A float constant LLVM will read back bit-for-bit."""
    if ty == "float":
        bits = struct.unpack("<Q", struct.pack("<d", f32(x)))[0]
        return "float 0x%016X" % bits
    if ty == "half":
        # LLVM writes a half as the bits of its own 16-bit encoding
        v = f16(x)
        bits = (0x7C00 if v > 0 else 0xFC00) if math.isinf(v) else \
            struct.unpack("<H", struct.pack("<e", v))[0]
        return "half 0xH%04X" % bits
    if ty == "double":
        return "double 0x%016X" % struct.unpack("<Q", struct.pack("<d", x))[0]
    raise ValueError(ty)


def iconst(ty, x):
    w = int(ty[1:])
    return "%s %d" % (ty, wrap(x, w, True))


def const(ty, x):
    return fconst(ty, x) if ty in ("float", "half", "double") else iconst(ty, x)


def emit(decl, name, argtys, retty, cases, tmp, signed, rcomps=None):
    """Build, assemble and evaluate one shader.

    Returns a list of results, one per case, each a list of the components in
    `rcomps` -- all of them unless the caller split them, which it does for an
    i64 result because each of those takes two of the target's four lanes.
    Every component and not just the first: a v4 operation whose fourth
    component was wrong would otherwise pass.
    """
    rty, rn = parse_type(retty)
    # a double is NOT a float target: it goes out as its two 32-bit halves
    # through an int4, which is the only way a 64-bit value fits four 32-bit
    # lanes at all (lanes_per_comp)
    outty = "float" if rty in ("float", "half") else "i32"
    outname = "float4" if outty == "float" else "int4"
    if rcomps is None:
        rcomps = list(range(rn))

    lines = []
    slots = []
    for i, args in enumerate(cases):
        a = []
        for ty, v in zip(argtys, args):
            bty, bn = parse_type(ty)
            if bn == 1:
                a.append(const(bty, v[0]))
            else:
                a.append("<%d x %s> <%s>"
                         % (bn, bty, ", ".join(const(bty, x) for x in v[:bn])))
        lines.append("  %%c%d = call %s @%s(%s)" % (i, retty, name, ", ".join(a)))

        for k in rcomps:
            s_ = "%%c%d" % i
            if rn > 1:
                s_ = "%%e%d_%d" % (i, k)
                lines.append("  %s = extractelement %s %%c%d, i32 %d"
                             % (s_, retty, i, k))
            if rty == "half":
                lines.append("  %%f%d_%d = fpext half %s to float" % (i, k, s_))
                slots.append("%%f%d_%d" % (i, k))
            elif rty in ("i1", "i8", "i16") and outty == "i32":
                # sext for a signed result: the render target is read back as
                # an unsigned 32-bit word, so a zero-extended -1 would come
                # out as 65535 and read as the translator's mistake rather
                # than this harness's
                lines.append("  %%f%d_%d = %s %s %s to i32"
                             % (i, k, "sext" if signed else "zext", rty, s_))
                slots.append("%%f%d_%d" % (i, k))
            elif rty == "i64" and outty == "i32":
                # TWO LANES, NOT A trunc. See lanes_per_comp.
                slots.extend(split64(lines, i, k, s_))
            elif rty == "double" and outty == "i32":
                lines.append("  %%b%d_%d = bitcast double %s to i64" % (i, k, s_))
                slots.extend(split64(lines, i, k, "%%b%d_%d" % (i, k)))
            else:
                slots.append(s_)

    # the insertelement below names the type, so a pad is the bare value
    pad = "0.0" if outty == "float" else "0"
    if len(slots) > 4:
        raise AssertionError(
            "%s: %d lanes for a four-lane render target; comp_batches and this "
            "emitter disagree" % (name, len(slots)))
    packed = (slots + [pad] * 4)[:4]
    prev = "undef"
    for i, s_ in enumerate(packed):
        lines.append("  %%v%d = insertelement <4 x %s> %s, %s %s, i32 %d"
                     % (i, outty, prev, outty, s_, i))
        prev = "%%v%d" % i

    src = SKELETON % dict(decl=decl, outty=outty, outname=outname,
                          body="\n".join(lines))
    ll = os.path.join(tmp, "ic.ll")
    bc = os.path.join(tmp, "ic.bc")
    with open(ll, "w") as f:
        f.write(src)
    q = subprocess.run(["llvm-as", "-o", bc, ll], capture_output=True)
    if q.returncode:
        return None, "llvm-as: " + q.stderr.decode().strip().splitlines()[0]
    q = subprocess.run([AIR2NIR, "--eval", bc], capture_output=True)
    for line in q.stdout.decode().splitlines():
        m = re.match(r"\s+out\s+(.*)$", line)
        if m:
            if "did not fold" in m.group(1):
                return None, "did not fold to a constant"
            vals = m.group(1).split()
            # The lanes exactly as --eval printed them: a float for a float
            # target, an unsigned 32-bit word for an integer one. unlane()
            # does the signedness and puts an i64's two words back together,
            # because a half-value cannot be signed before it is whole.
            flat = ([float(v) for v in vals] if outty == "float"
                    else [int(v) for v in vals])
            res = unlane(flat, len(cases), rcomps, rty, signed)
            if res is None:
                return None, ("--eval printed %d lanes, not the %d this shader "
                              "filled" % (len(flat),
                                          len(cases) * len(rcomps)
                                          * lanes_per_comp(rty)))
            return res, None
    err = q.stderr.decode().strip().splitlines()
    return None, (err[0] if err else "no output stored")


# ---------------------------------------------------------------------------
# the check


def base_and_sign(name):
    """'air.clamp.s.v2i16' -> ('clamp', True); 'air.fast_round.f32' -> ('fast_round', None)

    The same split works for llvm.fmuladd.v4f32 -> ('fmuladd', None): both
    vocabularies put the operation first and the type last.
    """
    parts = name.split(".")[1:]
    op = parts[0]
    sign = None
    if len(parts) >= 2 and parts[1] in ("s", "u"):
        sign = parts[1] == "s"
    return op, sign


# Dot products are checked on values whose products and sums are exact, so
# that the accumulation order does not enter into it.
DOT_FLOATS = [0.0, 1.0, -1.0, 2.0, 3.0, -4.0, 0.5, -0.25, 8.0, -16.0]


# A short pool, for the run on real hardware: 300 ms a module here and more on
# the phone, so the full cross product is 12 031 modules and an hour. What the
# GPU run has to establish is that each operation *executes* correctly, not the
# input coverage -- the folding check above already does the grid exhaustively.
# So these are the values that have actually broken something: both zeros, the
# half-way cases that separate round from rint, the largest float below a half,
# and the integer extremes.
#
# THE ORDER OF EVERY POOL BELOW IS LOAD-BEARING: --cases N keeps the first N and
# nothing else, so a value's position decides whether it is ever asked on the
# phone. Do not sort these, and do not add a value at the front: put it after
# the ones that decide something. Getting this wrong does not make a check fail,
# it narrows the question and stays green -- which is how the Adreno's u32 -> f32
# truncation came to be found by luck rather than by design. The suite carried on
# 20 October used --cases 4, which took [0, 1, -1, 7] from the integer pool, and
# -1 read as unsigned happened to be the one value in it that rounding decides.
FEW_FLOATS = [0.0, -0.0, 0.5, -0.5,
              struct.unpack("<f", struct.pack("<I", 0x3EFFFFFF))[0],
              2.5, -2.5, 3.7]
FEW_FLOATS2 = [1.0, -1.0, 0.0, 0.25]
FEW_INTS = [0, 1, -1, 7, -32768, 32767, 2147483647, -2147483648]
# Integer-to-float conversion gets its own pool, because what breaks there is
# not the magnitude but the rounding, and only a value that falls between two
# representable floats exercises it at all. The Adreno truncates u32 -> f32
# (section 4 of docs/air.md), and 0xFFFFFFFF was enough to see that something
# was wrong while saying nothing about which rule is in use. These say which:
#
#   0x00FFFFFF  2^24-1, the last integer every float can hold exactly: every
#               rounding rule must agree here, so a disagreement is not rounding
#   0x01000001  2^24+1, exactly half way between 2^24 and 2^24+2. The lower
#               neighbour has an even mantissa, so half-to-even rounds DOWN here
#               and half-away-from-zero rounds up
#   0x01000003  2^24+3, half way between 2^24+2 and 2^24+4, where the lower
#               neighbour is odd: half-to-even rounds UP. With the line above it,
#               the pair separates to-even from away-from-zero, which one midpoint
#               alone cannot do
#   0x7FFFFFFF  and 0xFFFFFFFF (-1): 1 below a power of two, where the nearest
#               float is 255 away on one side and 1 on the other -- truncation
#               and rounding differ by a whole ulp, which is what was seen
#
# First in the list are the ones that decide a rule, because --cases N keeps the
# first N: the suite carried to the phone used 4, which took [0, 1, -1, 7] and
# so never asked about a midpoint at all.
# A half destination needs its own midpoints, and for the same reason one level
# down: f16 holds integers exactly only to 2048, above which the spacing is 2.
#   2051  half way between 2050 and 2052, whose lower neighbour is odd, so
#         round-to-nearest-even gives 2052 and truncation gives 2050 -- the f16
#         discriminator, the counterpart of 16777219
#   2049  half way between 2048 and 2050, where both rules give 2048: carried to
#         show that a midpoint is not automatically a test
#   2048  the last integer f16 holds exactly
# Without these the four half-destination declarations of the corpus were asked
# only about values that overflow f16, and answered inf to all of them: the
# expectations read [0, inf, inf, inf], which tests saturation and says nothing
# whatever about rounding. They agreed on the Adreno and that agreement was
# empty. The order interleaves the two precisions so that --cases 4 reaches a
# discriminator for each: 0 for sanity, -1 for a whole-ulp f32 difference,
# 16777219 for the f32 rule, 2051 for the f16 rule.
CONV_INTS = [0, -1, 16777219, 2051, 16777217, 2049, 2048, 16777215,
             2147483647, 1, 7, -32768, 32767, -2147483648]
FEW_INTS2 = [1, -1, 0, 7]
FEW_DOT = [0.0, 1.0, -1.0, 2.0, -4.0, 0.5, -0.25, 8.0]


def pool_for(ty, family, few=False, nth=0):
    if few:
        bty, _ = parse_type(ty)
        if bty == "i1":
            return [0, 1]
        if bty.startswith("i"):
            if family == "convert" and bty == "i32":
                return CONV_INTS
            # I64S belongs in the SHORT pool as well as the long one, because
            # the short one is what gets carried to the phone -- the only
            # measurement that can say what the Adreno does -- and without
            # these that suite cannot tell a 64-bit answer from a 32-bit one.
            # Only the first argument's pool: --few gives the others four
            # values and two, so an appended value is unreachable there, and
            # one operand of this shape decides a comparison.
            if bty == "i64" and nth == 0:
                return FEW_INTS + I64S
            return FEW_INTS if nth == 0 else FEW_INTS2
        if family == "dot":
            return FEW_DOT
        return FEW_FLOATS if nth == 0 else FEW_FLOATS2
    return pool_for_full(ty, family)


def pool_for_full(ty, family):
    bty, _ = parse_type(ty)
    if bty == "i1":
        return [0, 1]
    if bty.startswith("i"):
        # The exhaustive pool has 0x7FFFFFFF and -1, both of which rounding
        # decides, but no exact midpoint between two floats -- so it could tell
        # a wrong conversion from a right one and could not tell which rule was
        # in use. The three added here can (see CONV_INTS), and only the convert
        # family pays for them: INTS is the pool for every integer operation,
        # and widening it would grow the whole exhaustive grid for one question
        # that belongs to one family.
        if family == "convert" and bty == "i32":
            return INTS + [16777215, 16777217, 16777219, 2048, 2049, 2051]
        # see I64S: every value of INTS fits in 32 bits, so without these the
        # 64-bit operations are never asked a question a 32-bit answer gets
        # wrong
        if bty == "i64":
            return INTS + I64S
        return INTS
    return DOT_FLOATS if family == "dot" else FLOATS


def inputs_for(argtys, family, few=False, cases=0):
    """The cases to try: one list per argument, holding its components.

    Every component of a vector argument gets a different value. With the same
    value in all of them, a lowering that read the wrong component, or dropped
    one, or swizzled, would return the right answer for the wrong reason --
    which is the mistake this port has already made four times with textures.
    """
    pools = [pool_for(t, family, few, n) for n, t in enumerate(argtys)]
    widths = [parse_type(t)[1] for t in argtys]

    # the whole pool for the first argument, less of it for the others, or a
    # three-argument operation is tens of thousands of shader builds
    # --cases N takes the first N of the first argument's pool and narrows the
    # others with it: that is how a suite small enough to carry to the phone is
    # cut, and the values are still in the order of how dangerous they are
    first = min(cases, len(pools[0])) if cases else len(pools[0])
    limits = [first]
    limits += [min(max(1, first // 2) if cases else (4 if few else 12), len(p))
               for p in pools[1:2]]
    limits += [min(max(1, first // 4) if cases else (2 if few else 6), len(p))
               for p in pools[2:]]

    grid = [()]
    for lim in limits:
        grid = [g + (i,) for g in grid for i in range(lim)]

    cases = []
    for g in grid:
        case = []
        for a, idx in enumerate(g):
            pool, n = pools[a], widths[a]
            case.append([pool[(idx + c) % len(pool)] for c in range(n)])
        cases.append(tuple(case))
    return cases


def showargs(args):
    """One case's arguments: a vector shows every component it was given."""
    out = []
    for v in args:
        f = ",".join("%.9g" % x if isinstance(x, float) else str(x) for x in v)
        out.append(f if len(v) == 1 else "(" + f + ")")
    return " ".join(out)


def ulps(a, b):
    """Distance in float32 steps, for reporting an approximate operation."""
    if math.isnan(a) and math.isnan(b):
        return 0
    if math.isnan(a) or math.isnan(b) or math.isinf(a) or math.isinf(b):
        return 0 if a == b else float("inf")
    ia, ib = to_f32_bits(f32(a)), to_f32_bits(f32(b))
    # a monotonic map of float32 onto the integers: negatives mirror through
    # zero, so adjacent floats are 1 apart. +0 and -0 both land on 0 -- this
    # number says nothing about the sign of a zero, and for an approximate
    # operation that is the right thing to leave out of it
    if ia >> 31:
        ia = -(ia & 0x7FFFFFFF)
    if ib >> 31:
        ib = -(ib & 0x7FFFFFFF)
    return abs(ia - ib)


import intrincheck_suite as _intrincheck_suite


def emit_suite(arith, outdir, want, verbose, few=False, cases=0):
    _intrincheck_suite.bind(globals())
    return _intrincheck_suite.emit_suite(arith, outdir, want, verbose, few=few, cases=cases)


def check_log(outdir, logpath, verbose):
    _intrincheck_suite.bind(globals())
    return _intrincheck_suite.check_log(outdir, logpath, verbose)


def main(argv):
    verbose = False
    want = []
    decls_file = None
    emit_dir = None
    check_pair = None
    run_here = False
    few = False
    ncases = 0
    i = 0
    while i < len(argv):
        if argv[i] == "-v":
            verbose = True
        elif argv[i] == "--decls":
            i += 1
            decls_file = argv[i]
        elif argv[i] == "--emit":
            i += 1
            emit_dir = argv[i]
        elif argv[i] == "--check":
            check_pair = (argv[i + 1], argv[i + 2])
            i += 2
        elif argv[i] == "--run":
            run_here = True
        elif argv[i] == "--few":
            few = True
        elif argv[i] == "--cases":
            i += 1
            ncases = int(argv[i])
        else:
            want.append(argv[i])
        i += 1

    if check_pair:
        return check_log(check_pair[0], check_pair[1], verbose)

    if not os.path.exists(AIR2NIR):
        print("intrincheck: run tools/air/mk-host.sh first", file=sys.stderr)
        return 1

    unread = []
    nlibs = 0
    if decls_file:
        decls = [l.strip() for l in open(decls_file) if l.strip()]
    else:
        libs = []
        for dirpath, _, names in os.walk(os.path.join(ROOT, "ios26/rootfs")):
            for n in names:
                if n.endswith(".metallib"):
                    libs.append(os.path.join(dirpath, n))
        nlibs = len(libs)
        decls, unread = collect_declarations(sorted(libs))
    # Printed BEFORE anything else and on stderr as well as stdout, because
    # the vocabulary decides how much the rest of this check is a check of:
    # a library that did not open takes its operations out of the suite and
    # pushes the pass rate UP. See collect_declarations.
    if unread:
        # on stderr only: the same count goes out on stdout in the summary at
        # the end, and printing it in both places put it through `grep
        # '^intrincheck:'` twice
        print("intrincheck: %d of %d libraries could not be read, so the "
              "operations only they declare are NOT in this run and the pass "
              "count below is an upper bound" % (len(unread), nlibs),
              file=sys.stderr)
        for lib, why in unread:
            # the reason, cut to one short line: what matters here is whether
            # it is a library this frontend cannot parse (a finding) or a
            # binary that could not be executed (a race in a shared tree)
            why = re.sub(r"^air2nir: \S+: ", "", why.strip().splitlines()[0])
            print("unread  %-34s %s" % (os.path.basename(lib), why[:70]))

    arith = []
    skipped = []
    for d in decls:
        m = DECL.match(d)
        if not m:
            continue
        retty, name, args = m.group(1), m.group(2), m.group(3)
        op, sign = base_and_sign(name)
        plain = op[5:] if op.startswith("fast_") else op
        argtys = [a.strip() for a in args.split(",")] if args.strip() else []
        family = ("convert" if plain == "convert"
                  else plain if plain in ("dot", "all", "any")
                  else "op" if plain in OPS or (plain, sign) in IOPS
                  else None)
        if family is None:
            skipped.append((name, "not an operation this check has a reference for"))
            continue
        # the integer min/max/clamp live in their own table and take their
        # arity from the name, not from OPS
        want_args = (OPS[plain][0] if plain in OPS
                     else 3 if plain == "clamp" else 2)
        if family == "op" and want_args != len(argtys):
            skipped.append((name, "%d arguments, the reference takes %d"
                            % (len(argtys), want_args)))
            continue
        arith.append((d, name, retty, argtys, op, plain, sign, family))

    if emit_dir:
        return emit_suite(arith, emit_dir, want, verbose, few, ncases)

    if run_here:
        # the same modules, drawn by whatever screen air_run opens -- softpipe
        # here, the Adreno on the phone
        d = tempfile.mkdtemp(prefix="intrincheck.run.")
        if emit_suite(arith, d, want, verbose, few, ncases):
            return 1
        log = os.path.join(d, "run.log")
        air_run = os.environ.get("AIR_RUN", os.path.join(
            ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"))
        if not os.path.exists(air_run):
            print("intrincheck: no air_run at %s" % air_run, file=sys.stderr)
            return 1
        with open(log, "w") as f:
            q = subprocess.run(["sh", os.path.join(d, "run.sh")],
                               env=dict(os.environ, AIR_RUN=air_run),
                               stdout=f, stderr=subprocess.STDOUT)
        if q.returncode:
            print("intrincheck: run.sh exited %d" % q.returncode, file=sys.stderr)
        return check_log(d, log, verbose)

    tmp = tempfile.mkdtemp(prefix="intrincheck.")
    exact_ok = exact_bad = approx = refused = denorm = notnormal = 0
    total_cases = 0
    # cases the reference declined, by the reason it gave: {why: {name: count}}
    declined = {}
    # and cases it has no answer for at all, by the same shape
    noanswer = {}
    # declarations for which not one case was compared
    nocase = []
    # declarations every one of whose disagreements is in a named set-aside,
    # by declaration: {name: cases}
    aside_cases = {}
    for d, name, retty, argtys, op, plain, sign, family in arith:
        if want and not any(w in name for w in want):
            continue
        is_exact = OPS[plain][2] if plain in OPS else True
        rty = parse_type(retty)[0]
        cases = inputs_for(argtys, family)
        bad = []
        aside = []
        worst = 0
        worst_case = None
        n = 0
        # which result components go in one shader, and how many cases fit
        # beside them in a four-lane render target. One group for everything
        # but an i64 result, whose components take two lanes each.
        for rcomps, per_shader in comp_batches(retty):
            if bad is None:
                break
            for k in range(0, len(cases), per_shader):
                batch = cases[k:k + per_shader]
                got, err = emit(d, name, argtys, retty, batch, tmp, sign, rcomps)
                if got is None:
                    refused += 1
                    print("REFUSED %-34s %s" % (name, err))
                    bad = None
                    break
                for j, args in enumerate(batch):
                    try:
                        expv = reference(family, plain, name, args, argtys, retty,
                                         True if sign is None else sign)
                    except Undecided as why:
                        per = declined.setdefault(str(why), {})
                        per[name] = per.get(name, 0) + 1
                        continue
                    except (ValueError, OverflowError, ZeroDivisionError) as why:
                        # A case the reference has no answer for -- out of the
                        # destination's range, outside a domain -- and NOT a
                        # case that passed. It used to be a bare `continue`,
                        # which is the same quiet subtraction from the total
                        # that this file has been bitten by three times: a
                        # declaration can lose most of its cases this way and
                        # still read "agrees on every case".
                        per = noanswer.setdefault(str(why) or type(why).__name__, {})
                        per[name] = per.get(name, 0) + 1
                        continue
                    for p, c in enumerate(rcomps):
                        exp = expv[c]
                        # Apple compiles with air.compile.denorms_disable, so a
                        # denormal result is allowed to be flushed to zero and
                        # this check has nothing to say about it. ldexp is the
                        # only operation here that reaches one from normal inputs.
                        #
                        # ldexp also loses an overflow. NIR's constant expression
                        # for it is
                        #
                        #     dst = ldexpf(src0, src1);
                        #     /* flush denormals to zero. */
                        #     if (!isnormal(dst)) dst = copysignf(0.0f, src0);
                        #
                        # and !isnormal is true of an infinity as well as of a
                        # denormal, so ldexp(0.5, 32767) folds to 0 where Metal,
                        # OpenCL and C all say +inf. The comment names denormals;
                        # the condition catches more than it means to. That is
                        # Mesa's to fix and not this frontend's to work around, so
                        # those cases are left out here and written down in
                        # docs/air.md instead.
                        if (isinstance(exp, float)
                                and (math.isinf(exp)
                                     or (exp != 0.0
                                         and abs(exp) < smallest_normal(rty)))):
                            if plain == "ldexp":
                                notnormal += 1
                                continue
                            if not math.isinf(exp):
                                denorm += 1
                                continue
                        n += 1
                        total_cases += 1
                        g = got[j][p]
                        if is_exact:
                            if isinstance(g, int) and isinstance(exp, int):
                                same = g == exp
                            else:
                                same = same_float(g, exp, rty)
                            if not same:
                                # a disagreement goes into the set-aside only
                                # if THIS case satisfies the condition; see
                                # FMOD_ASIDE
                                if plain == "fmod" and fmod_aside(args, c, rty):
                                    aside.append((args, c, g, exp))
                                else:
                                    bad.append((args, c, g, exp))
                        else:
                            u = ulps(g, exp)
                            if u > worst:
                                worst, worst_case = u, (args, c, g, exp)
        if bad is None:
            continue
        # A DECLARATION NOBODY COMPARED A CASE OF IS NOT A DECLARATION THAT
        # AGREES, and it used to be counted as one: `if bad:` is false over an
        # empty list, so zero comparisons fell into the `else` and incremented
        # the agreeing count. Six declarations were in exactly that state --
        # the float/half conversions whose reference crashed (see ref_convert)
        # -- and the summary called them agreement. Named, counted apart, and
        # fatal, because this is the shape of every wrong green this check has
        # ever had.
        if not n:
            nocase.append(name)
            print("NOCASE  %-34s not one case was compared, so this is not "
                  "agreement" % name)
            continue
        if is_exact:
            if bad:
                exact_bad += 1
                print("WRONG   %-34s %d of %d cases differ%s"
                      % (name, len(bad), n,
                         " (and %d more set aside)" % len(aside) if aside else ""))
                for args, c, g, exp in bad[:4]:
                    fmt = (lambda v: "%.9g" % v) if isinstance(g, float) else str
                    print("          %-34s [%d] got %-16s want %s"
                          % (showargs(args), c, fmt(g), fmt(exp)))
            elif aside and len(aside) == n:
                # EVERY case set aside is agreement about nothing, and the
                # set-aside must not be able to swallow a whole declaration:
                # that is the NOCASE failure wearing a different hat.
                nocase.append(name)
                print("NOCASE  %-34s all %d of its compared cases are in the "
                      "set-aside, so nothing was decided" % (name, n))
            elif aside:
                # every disagreement satisfied the condition, so the operation
                # agrees everywhere it is claimed to and the stage passes --
                # with the count and the reason printed, never implied
                aside_cases[name] = len(aside)
                print("ASIDE   %-34s %d of %d cases set aside, the other %d agree"
                      % (name, len(aside), n, n - len(aside)))
                args, c, g, exp = aside[0]
                print("          %-34s [%d] got %-16s want %s"
                      % (showargs(args), c, "%.9g" % g, "%.9g" % exp))
            else:
                exact_ok += 1
                if verbose:
                    print("exact   %-34s %d cases" % (name, n))
        else:
            approx += 1
            # where the worst case is, not just how bad: a large ulp count on a
            # polynomial approximation is ordinary, and on a quadrant boundary
            # or a sign of zero it is a different thing entirely
            where = ""
            if worst_case and worst > 64:
                args, c, g, exp = worst_case
                where = "   at %s [%d]: %.9g against %.9g" % (
                    showargs(args), c, g, exp)
            print("approx  %-34s %d cases, worst %s ulp from libm%s"
                  % (name, n, worst if worst != float("inf") else "inf", where))

    print()
    # Repeated at the end as well as at the start: every caller of this script
    # reads it through `tail`, and the first copy is exactly the line that got
    # cut off while the pass count stayed green.
    if unread:
        print("intrincheck: %d of %d libraries could not be read, so this run's "
              "vocabulary is incomplete and its pass rate is an upper bound (%s)"
              % (len(unread), nlibs,
                 ", ".join(sorted(os.path.basename(q) for q, _ in unread))))
    print("intrincheck: %d declarations of the corpus have a reference here, %d others do not"
          % (len(arith), len(skipped)))
    print("intrincheck: %d exactly defined operations agree on every case, %d differ"
          % (exact_ok, exact_bad))
    if aside_cases:
        print("intrincheck: %d more agree on every case they are claimed to, with %d "
              "cases set aside by name (%s)"
              % (len(aside_cases), sum(aside_cases.values()),
                 ", ".join("%s: %d" % (k, aside_cases[k])
                           for k in sorted(aside_cases))))
        print("intrincheck: the set-aside is %s" % FMOD_ASIDE)
    print("intrincheck: %d are approximate, compared with this machine's libm and not counted"
          % approx)
    if refused:
        print("intrincheck: %d could not be built or did not fold" % refused)
    if denorm:
        print("intrincheck: %d cases left out: the reference result is denormal and "
              "denorms_disable permits zero" % denorm)
    if notnormal:
        print("intrincheck: %d ldexp cases left out: NIR's constant expression flushes "
              "anything !isnormal, infinities included" % notnormal)
    # A case the reference declines is not a case that passed and not a case
    # that was quietly dropped, so it is named here with the reason -- the
    # whole point of Undecided being an exception that carries one.
    for why in sorted(declined):
        per = declined[why]
        print("intrincheck: %d cases the reference declines to answer -- %s (%s)"
              % (sum(per.values()), why,
                 ", ".join("%s: %d" % (k, per[k]) for k in sorted(per))))
    # Named too, and for the same reason: a case nobody could ask is not a
    # case that agreed. These are the reference's own limits (a conversion out
    # of the destination's range, a domain a function does not have) and not
    # the frontend's, which is why they are a separate line from the ones
    # above and not an error.
    for why in sorted(noanswer):
        per = noanswer[why]
        print("intrincheck: %d cases the reference has no answer for -- %s (%s)"
              % (sum(per.values()), why,
                 ", ".join("%s: %d" % (k, per[k]) for k in sorted(per)[:8])
                 + (", ..." if len(per) > 8 else "")))
    print("intrincheck: %d cases in all" % total_cases)
    if verbose and skipped:
        print()
        for name, why in sorted(set(skipped)):
            print("no ref  %-34s %s" % (name, why))

    # A GREEN RUN THAT COMPARED NOTHING IS THE WORST OUTCOME THIS SCRIPT CAN
    # HAVE, and until this it was reachable: point AIR2NIR at a binary that
    # cannot be executed -- which is what a relink in this shared tree looks
    # like for the second or two it takes -- and every library dropped out,
    # the vocabulary came back empty, and the summary read "0 agree, 0 differ"
    # and exited 0. Two rules close it, and they are different claims:
    #   - nothing was compared, so there is no result either way;
    #   - something could not be EXECUTED (see not_run_at_all), so what was
    #     compared is less than this run believes. A library that is simply
    #     not bitcode is NOT in this set: that is a fact about the library and
    #     would make the check permanently red for the fourteen it is true of.
    if nocase:
        print("intrincheck: %d declarations had NOT ONE case compared, which is not "
              "agreement and is counted as a failure: %s"
              % (len(nocase), ", ".join(sorted(nocase))))
    contaminated = [(p_, w) for p_, w in unread if not_run_at_all(w)]
    if not total_cases:
        print("intrincheck: NOTHING WAS COMPARED, so this is not a pass; %d "
              "libraries went unread" % len(unread), file=sys.stderr)
    if contaminated:
        print("intrincheck: %d libraries could not be EXECUTED for, not merely not "
              "read (%s) -- a relink under the run, not a result"
              % (len(contaminated),
                 ", ".join("%s: %s" % (os.path.basename(p_), w)
                           for p_, w in contaminated[:4])), file=sys.stderr)
    return 1 if (exact_bad or refused or nocase or not total_cases
                 or contaminated) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
