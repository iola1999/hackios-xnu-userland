#!/usr/bin/env python3
"""Check promotion of a fragment-local array with halfword stores and a dynamic read."""
from pathlib import Path
import re
import subprocess
import tempfile

from argbufcheck import AIR2NIR, AIR_RUN, assemble
from texopcheck import SKELETON


source = SKELETON % {
    "decl": "",
    "outty": "float",
    "outname": "float4",
    "params": "ptr addrspace(2) %row",
    "args": "!7",
    "argmd": ('!7 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, '
              '!"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, '
              '!"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", '
              '!"air.arg_name", !"row"}'),
    "body": """
  %array = alloca [4 x half], align 2
  %b = getelementptr [4 x half], ptr %array, i32 0, i32 1
  %c = getelementptr [4 x half], ptr %array, i32 0, i32 2
  %d = getelementptr [4 x half], ptr %array, i32 0, i32 3
  store half 0xH3C00, ptr %array, align 2
  store half 0xH3400, ptr %b, align 2
  store half 0xH3800, ptr %c, align 2
  store half 0xH3A00, ptr %d, align 2
  %input = load i32, ptr addrspace(2) %row
  %index = and i32 %input, 3
  %p = getelementptr [4 x half], ptr %array, i32 0, i32 %index
  %h = load half, ptr %p, align 2
  %f = fpext half %h to float
  %v = insertelement <4 x float> zeroinitializer, float %f, i32 0
""",
}

with tempfile.TemporaryDirectory() as directory:
    bc, error = assemble(directory, "local", source)
    if error:
        raise RuntimeError(error)
    nir = subprocess.check_output([AIR2NIR, bc], text=True)
    if re.search(r"^scratch: [1-9]|@(load|store)_scratch", nir, re.MULTILINE):
        raise RuntimeError("fragment array still uses private memory")
    buffer = Path(directory) / "row.bin"
    for index, expected in enumerate([1.0, 0.25, 0.5, 0.75]):
        buffer.write_bytes(index.to_bytes(4, "little"))
        output = subprocess.check_output([AIR_RUN, "--buffer", buffer, bc], text=True)
        pixel = re.search(r"came back as:\s*([0-9.]+)", output)
        if not pixel or float(pixel[1]) != expected:
            raise RuntimeError(f"fragment output for row {index} differs from {expected}: {output}")
    print("PASS: four run-time halfword indices give the expected pixels without scratch")
