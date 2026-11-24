#!/usr/bin/env python3
"""Check which dynamic threadgroup inputs a specialized kernel requires."""
import tempfile

from argbufcheck import assemble, interface_of


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
