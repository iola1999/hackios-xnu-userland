#!/usr/bin/env python3
"""Check pointer bindings and byte offsets across AIR function calls.

Usage: AIR2NIR=/path/to/air2nir LLVM_AS=/path/to/llvm-as python3 callslotcheck.py
Successful cases validate NIR and check the final SSBO store. Unsupported
run-time values in local slots or pointer aggregates must fail with an error.
"""

import os
from pathlib import Path
import re
import subprocess
import tempfile


AIR2NIR = os.environ.get("AIR2NIR", "air2nir")
LLVM_AS = os.environ.get("LLVM_AS", "llvm-as")
HEADER = 'target datalayout = "e-p:64:64:64-i32:32:32-n8:16:32"\n'
HEADER += 'target triple = "air64-apple-ios26.0.0"\n'


def module(name, entry, helper, extra_buffer=False, types=""):
    args = "ptr addrspace(1) %output, "
    if extra_buffer:
        args += "ptr addrspace(1) %other, "
    args += "i32 %gid"
    source = HEADER + types
    source += f"define void @{name}({args}) {{\nentry:\n{entry}\n  ret void\n}}\n"
    source += helper
    source += f"\n!air.kernel = !{{!0}}\n!0 = !{{ptr @{name}, !1, !2}}\n!1 = !{{}}\n"
    source += "!2 = !{!3, !4" + (", !5" if extra_buffer else "") + "}\n"
    buffers = [(0, "output", 5)]
    if extra_buffer:
        buffers.append((1, "other", 6))
    for arg, arg_name, binding in buffers:
        source += (f'!{3 + arg} = !{{i32 {arg}, !"air.buffer", !"air.location_index", '
                   f'i32 {binding}, i32 1, !"air.write", !"air.address_space", i32 1, '
                   f'!"air.buffer_size", i32 32, !"air.arg_type_name", !"int", '
                   f'!"air.arg_name", !"{arg_name}"}}\n')
    gid_arg = len(buffers)
    source += (f'!{3 + gid_arg} = !{{i32 {gid_arg}, !"air.thread_position_in_grid", '
               '!"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}\n')
    return source


SLOT_HELPER = """define internal void @write_slot(ptr %slot) {
entry:
  %target = load ptr addrspace(1), ptr %slot, align 8
  store i32 99, ptr addrspace(1) %target, align 4
  ret void
}
"""
DIRECT_HELPER = """define internal void @write_target(ptr addrspace(1) %target) {
entry:
  store i32 99, ptr addrspace(1) %target, align 4
  ret void
}
"""
AGG_TYPES = "%Bundle = type { ptr addrspace(1), i32 }\n"
AGG_HELPER = """define internal void @write_bundle(%Bundle %bundle) {
entry:
  %target = extractvalue %Bundle %bundle, 0
  %value = extractvalue %Bundle %bundle, 1
  store i32 %value, ptr addrspace(1) %target, align 4
  ret void
}
"""
SLOT = "  %slot = alloca ptr addrspace(1), align 8\n"
CALL_SLOT = """
  store ptr addrspace(1) %target, ptr %slot, align 8
  call void @write_slot(ptr %slot)"""
CONST_PTR = "  %target = getelementptr i32, ptr addrspace(1) %output, i32 1\n"
DYNAMIC_PTR = """  %index = add i32 %gid, 1
  %target = getelementptr i32, ptr addrspace(1) %output, i32 %index
"""
CHOICE_PTR = """  %condition = icmp eq i32 %gid, 0
  %target = select i1 %condition, ptr addrspace(1) %output, ptr addrspace(1) %other
"""
CALL_AGG = """  %partial = insertvalue %Bundle undef, ptr addrspace(1) %target, 0
  %bundle = insertvalue %Bundle %partial, i32 99, 1
  call void @write_bundle(%Bundle %bundle)"""


def cases():
    yield "slot_const", module("slot_const", SLOT + CONST_PTR + CALL_SLOT, SLOT_HELPER), 4
    array_ptr = ("  %target = getelementptr [4 x i32], ptr addrspace(1) %output, "
                 "i32 0, i32 2\n")
    yield "slot_array_const", module("slot_array_const", SLOT + array_ptr + CALL_SLOT,
                                     SLOT_HELPER), 8
    yield "direct_dynamic", module("direct_dynamic", DYNAMIC_PTR +
                                    "  call void @write_target(ptr addrspace(1) %target)",
                                    DIRECT_HELPER), "dynamic"
    yield "slot_dynamic", module("slot_dynamic", SLOT + DYNAMIC_PTR + CALL_SLOT,
                                 SLOT_HELPER), "pointer byte offset crossing"
    yield "slot_choice", module("slot_choice", SLOT + CHOICE_PTR + CALL_SLOT,
                                SLOT_HELPER, True), "pointer choice crossing"
    yield "aggregate_const", module("aggregate_const", CONST_PTR + CALL_AGG,
                                    AGG_HELPER, types=AGG_TYPES), 4
    yield "aggregate_dynamic", module("aggregate_dynamic", DYNAMIC_PTR + CALL_AGG,
                                      AGG_HELPER, types=AGG_TYPES), "pointer byte offset crossing"
    yield "aggregate_choice", module("aggregate_choice", CHOICE_PTR + CALL_AGG,
                                     AGG_HELPER, True, AGG_TYPES), "pointer choice crossing"


def run(arguments):
    return subprocess.run(arguments, capture_output=True, text=True, timeout=30)


def check_store(text, offset):
    constants = {}
    for match in re.finditer(r"%([0-9]+) = load_const \(0x([0-9a-fA-F]+)", text):
        constants[match[1]] = int(match[2], 16)
    stores = re.findall(r"@store_ssbo \((.*?)\) \(wrmask=", text)
    if len(stores) != 1:
        raise AssertionError(f"expected one SSBO store, got {len(stores)}")
    operands = re.findall(r"%([0-9]+)", stores[0])
    if len(operands) != 3:
        raise AssertionError(f"unexpected SSBO operands: {stores[0]}")
    value, binding, address = [constants.get(x) for x in operands]
    if (value, binding) != (99, 5):
        raise AssertionError(f"expected value=99, binding=5; got {value}, {binding}")
    if offset == "dynamic":
        if address is not None or "@load_global_invocation_id" not in text:
            raise AssertionError("run-time byte offset was lost")
    elif address != offset:
        raise AssertionError(f"expected byte offset={offset}, got {address}")


def main():
    failures = []
    checks = list(cases())
    with tempfile.TemporaryDirectory(prefix="air-call-slots-") as directory:
        for name, source, expected in checks:
            ll = Path(directory) / (name + ".ll")
            bc = ll.with_suffix(".bc")
            ll.write_text(source)
            assembled = run([LLVM_AS, str(ll), "-o", str(bc)])
            if assembled.returncode:
                raise RuntimeError(assembled.stderr)
            result = run([AIR2NIR, str(bc)])
            try:
                if isinstance(expected, str) and expected != "dynamic":
                    if result.returncode != 1 or expected not in result.stderr:
                        raise AssertionError(f"expected an explicit failure: {result.returncode}, "
                                             f"{result.stderr[:240]}")
                else:
                    if result.returncode:
                        raise AssertionError(result.stderr[:240])
                    report = run([AIR2NIR, "--report", str(bc)])
                    if report.returncode or "\ttranslate=ok\t" not in report.stdout:
                        raise AssertionError(f"NIR validation failed: {report.stderr[:240]}")
                    check_store(result.stdout, expected)
                print(f"PASS {name}")
            except AssertionError as error:
                failures.append(name)
                print(f"FAIL {name}: {error}")
    print(f"{len(checks) - len(failures)}/{len(checks)} call-slot checks passed")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
