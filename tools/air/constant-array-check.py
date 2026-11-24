#!/usr/bin/env python3
"""Check dynamic halfword offsets across two contiguous 5x5 read-only filter tables."""
import re
import struct
import subprocess
import tempfile

from argbufcheck import AIR2NIR, AIR_RUN, assemble


def table(name, values):
    rows = []
    for y in range(5):
        bits = [struct.pack(">e", value).hex() for value in values[y * 5:y * 5 + 5]]
        rows.append("[5 x half] [" + ", ".join("half 0xH" + word for word in bits) + "]")
    return f"@{name} = internal addrspace(2) constant [5 x [5 x half]] [{', '.join(rows)}], align 2"


# A signed asymmetric kernel exposes transposed indices and swapped halfwords.
first = [(i - 12) / 16 for i in range(25)]
second = [(12 - i * 2) / 32 for i in range(25)]
source = table("a", first) + "\n" + table("b", second) + """
define void @filter(ptr addrspace(1) %out) {
entry:
  %param = getelementptr i32, ptr addrspace(1) %out, i32 2
  %in = load i32, ptr addrspace(1) %param
  %index = urem i32 %in, 25
  %row = udiv i32 %index, 5
  %col = urem i32 %index, 5
  %pa = getelementptr [5 x [5 x half]], ptr addrspace(2) @a, i32 0, i32 %row, i32 %col
  %pb = getelementptr [5 x [5 x half]], ptr addrspace(2) @b, i32 0, i32 %row, i32 %col
  %a = load half, ptr addrspace(2) %pa, align 2
  %b = load half, ptr addrspace(2) %pb, align 2
  %fa = fpext half %a to float
  %fb = fpext half %b to float
  %out1 = getelementptr float, ptr addrspace(1) %out, i32 1
  store float %fa, ptr addrspace(1) %out
  store float %fb, ptr addrspace(1) %out1
  ret void
}
!air.kernel = !{!0}
!0 = !{ptr @filter, !1, !2}
!1 = !{}
!2 = !{!3}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write",
       !"air.address_space", i32 1, !"air.buffer_size", i32 16, !"air.arg_type_size", i32 4,
       !"air.arg_type_align_size", i32 4, !"air.arg_name", !"out"}
"""

with tempfile.TemporaryDirectory() as work:
    bitcode, error = assemble(work, "constant-arrays", 'target triple = "air64-apple-ios26.0.0"\n' + source)
    if error:
        raise SystemExit(error)
    nir = subprocess.check_output([AIR2NIR, bitcode], text=True)
    if nir.count("@load_constant (") != 2:
        raise SystemExit("expected two dynamic loads from initialized constant data")
    for i, expected in enumerate(zip(first, second)):
        run = subprocess.check_output([AIR_RUN, "--zero", "--block", "2,1,1", "--buffer",
                                       f"0@8={i.to_bytes(4, 'little').hex()}", bitcode], text=True)
        words = dict(re.findall(r"^  \[ *(\d+)\] +[-\d]+ +0x([0-9a-fA-F]{8})", run, re.M))
        actual = tuple(struct.unpack(">f", bytes.fromhex(words[str(slot)]))[0] for slot in (0, 1))
        if actual != expected:
            raise SystemExit(f"tap {i}: expected {expected}, got {actual}")
print("PASS: 50 half coefficients read at dynamic offsets in two consecutive filter tables")


# The three half lanes occupy six bytes but consecutive vectors start eight
# bytes apart. The innermost vector is packed in each of the other tables.
vectors = [
    ("offsets", "i32", 2, [[-2, -3], [17, 31], [-47, 63], [79, -95]]),
    ("mask", "i8", 4, [[1, 2, 3, 4], [-8, 6, -5, 7], [8, 9, 10, 11], [12, 13, -14, 15]]),
    ("padded", "i16", 3, [[111, 112, -113], [221, -222, 223], [-331, 332, 333], [441, 442, 443]]),
    ("weights", "float", 4, [[0.25, 0.5, 0.75, 1.0], [1.5, 2.0, 2.5, 3.0],
                              [-0.5, -0.25, 0.125, 0.25], [4.0, 4.5, 5.0, 5.5]]),
]
globals_text = []
instructions = []
expected = [[] for _ in range(4)]
slot = 0
for name, scalar, lanes, rows in vectors:
    vector = f"<{lanes} x {scalar}>"
    entries = [f"{vector} <" + ", ".join(f"{scalar} {v}" for v in values) + ">" for values in rows]
    # The real sharpening mask and coefficients use a nested array of vectors.
    array = f"[2 x [2 x {vector}]]"
    pairs = [f"[2 x {vector}] [{entries[n]}, {entries[n+1]}]" for n in (0, 2)]
    globals_text.append(f"@{name} = internal addrspace(2) constant {array} [{', '.join(pairs)}]")
    instructions.extend([
        f"  %{name}_ptr = getelementptr {array}, ptr addrspace(2) @{name}, i32 0, i32 %y, i32 %x",
        f"  %{name} = load {vector}, ptr addrspace(2) %{name}_ptr",
    ])
    for lane in range(lanes):
        value = f"%v{slot}"
        instructions.append(f"  {value} = extractelement {vector} %{name}, i32 {lane}")
        if scalar != "i32":
            op = "bitcast" if scalar == "float" else "sext"
            instructions.append(f"  %word{slot} = {op} {scalar} {value} to i32")
            value = f"%word{slot}"
        instructions.extend([f"  %dst{slot} = getelementptr i32, ptr addrspace(1) %out, i32 {slot}",
                             f"  store i32 {value}, ptr addrspace(1) %dst{slot}"])
        for i, row in enumerate(rows):
            val = row[lane]
            expected[i].append(struct.unpack("<I", struct.pack("<f", val))[0] if scalar == "float" else val & 0xffffffff)
        slot += 1

vector_source = "\n".join(globals_text) + """
define void @filter(ptr addrspace(1) %out, i32 %tid) {
entry:
  %first = icmp eq i32 %tid, 0
  br i1 %first, label %read, label %done
read:
  %arg = getelementptr i32, ptr addrspace(1) %out, i32 13
  %raw = load i32, ptr addrspace(1) %arg
  %row = and i32 %raw, 3
  %y = lshr i32 %row, 1
  %x = and i32 %row, 1
""" + "\n".join(instructions) + """
  br label %done
done:
  ret void
}
!air.kernel = !{!0}
!0 = !{ptr @filter, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write",
       !"air.address_space", i32 1, !"air.buffer_size", i32 64, !"air.arg_type_size", i32 4,
       !"air.arg_type_align_size", i32 4, !"air.arg_name", !"out"}
!4 = !{i32 1, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"""
with tempfile.TemporaryDirectory() as work:
    bitcode, error = assemble(work, "vector-tables", 'target triple = "air64-apple-ios26.0.0"\n' + vector_source)
    if error:
        raise SystemExit(error)
    nir = subprocess.check_output([AIR2NIR, bitcode], text=True)
    if "@load_constant (" not in nir or "@load_scratch (" in nir:
        raise SystemExit("expected dynamic vector loads from initialized constant data")
    for i, row in enumerate(expected):
        run = subprocess.check_output([AIR_RUN, "--zero", "--block", "16,1,1", "--buffer",
                                       f"0@52={i.to_bytes(4, 'little').hex()}", bitcode], text=True)
        words = dict(re.findall(r"^  \[ *(\d+)\] +[-\d]+ +0x([0-9a-fA-F]{8})", run, re.M))
        actual = [int(words[str(k)], 16) for k in range(slot)]
        if actual != row:
            raise SystemExit(f"vector row {i}: expected {row}, got {actual}")
print("PASS: 52 dynamically indexed vector lanes, including three-lane tail padding")
