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
