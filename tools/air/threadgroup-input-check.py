#!/usr/bin/env python3
"""Check dynamic threadgroup bindings and a shared pointer used under its defining condition."""
import re
import subprocess
import tempfile
from pathlib import Path

from argbufcheck import AIR2NIR, AIR_RUN, assemble, interface_of


with tempfile.TemporaryDirectory() as work:
    for active in (False, True):
        store = "store i32 1, ptr addrspace(3) %scratch" if active else ""
        unused = "" if active else ', !"air.arg_unused"'
        source = f'''
target triple = "air64-apple-ios26.0.0"
define void @test(ptr addrspace(3) %scratch) {{
  {store}
  ret void
}}
!air.kernel = !{{!0}}
!0 = !{{ptr @test, !1, !2}}
!1 = !{{}}
!2 = !{{!3}}
!3 = !{{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1,
       !"air.read_write", !"air.address_space", i32 3, !"air.arg_type_size",
       i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name",
       !"uint", !"air.arg_name", !"scratch"{unused}}}
'''
        bitcode, error = assemble(work, "threadgroup", source)
        if error:
            raise SystemExit(error)
        slots, error = interface_of(bitcode)
        expected = [(2, 0, -1, -1)] if active else []
        if error or slots != expected:
            raise SystemExit(f"active={active}: expected {expected}, got {slots}, error={error}")
print("PASS: only the active dynamic threadgroup input needs a binding")

with tempfile.TemporaryDirectory() as work:
    source = Path(__file__).with_name("guarded-shared-phi.ll").read_text()
    bitcode, error = assemble(work, "guarded", source)
    if error:
        raise SystemExit(error)
    for mode, expected in [(0, 83), (1, 151)]:
        patch = f"0@4={mode.to_bytes(4, 'little').hex()}"
        run = subprocess.run([AIR_RUN, "--block", "1,1,1", "--zero", "--buffer", patch, bitcode],
                             capture_output=True, text=True)
        if run.returncode or not re.search(rf"^  \[ *0\] +{expected}  0x", run.stdout, re.M):
            raise SystemExit(f"mode {mode}: expected {expected} in word 0\n{run.stdout}{run.stderr}")

    unguarded = source.replace("br i1 %cond, label %use0, label %other",
                               "br i1 %cond, label %other, label %use0")
    bitcode, error = assemble(work, "unguarded", unguarded)
    if error:
        raise SystemExit(error)
    run = subprocess.run([AIR2NIR, bitcode], capture_output=True, text=True)
    if not run.returncode or "different resource" not in run.stderr:
        raise SystemExit(f"unguarded pointer: expected translation to fail\n{run.stdout}{run.stderr}")
print("PASS: shared pointer reads return 83 and 151 on the two defined paths")
