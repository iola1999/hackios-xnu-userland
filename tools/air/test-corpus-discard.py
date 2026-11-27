#!/usr/bin/env python3
"""Host unit tests for tools/metallib.py, tools/air/air_env.py, corpus.py, and discardcheck.py."""
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

AIR_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.dirname(AIR_DIR)
for p in (TOOLS_DIR, AIR_DIR):
    if p not in sys.path:
        sys.path.insert(0, p)

import air_env  # noqa: E402
import metallib  # noqa: E402


def make_fat_container(slices):
    """Build a big-endian 0xCAFEBABE fat header followed by (cputype, payload) slices."""
    hdr = bytearray(struct.pack(">II", 0xCAFEBABE, len(slices)))
    off = 8 + len(slices) * 20
    body = bytearray()
    for cputype, payload in slices:
        hdr.extend(struct.pack(">IIIII", cputype, 0, off, len(payload), 0))
        body.extend(payload)
        off += len(payload)
    return bytes(hdr) + bytes(body)


def write_exec(path, body):
    with open(path, "w") as f:
        f.write("#!/usr/bin/env python3\n" + body)
    os.chmod(path, 0o755)


FAKE_AIR2NIR = """
import os, sys
args = sys.argv[1:]
if args[0] == "--report":
    raw = open(args[1], "rb").read()
    if raw[:4] == b"BC\\xc0\\xde":
        print("module=frag_bc\\tstage=fragment\\treader=ok\\ttranslate=ok\\tcalls=air.discard_fragment")
        sys.exit(0)
    if raw[:4] != b"MTLB":
        sys.exit(1)
    print("module=frag_a\\tlibrary=x\\tstage=fragment\\tcontainer=fragment\\treader=ok\\ttranslate=ok\\tcalls=air.discard_fragment,air.sample_texture_2d.v4f32module=crashed_v\\tnotes=")
    print("module=crashed_v\\tlibrary=x\\tstage=vertex\\tcontainer=vertex\\treader=ok\\ttranslate=fail\\treason=crashed: signal 6")
elif args[0].endswith(".bc"):
    print("@demote")
else:
    sys.exit(2)
"""

FAKE_LLVM_AS = """
import sys
out_idx = sys.argv.index("-o") + 1
open(sys.argv[out_idx], "wb").write(b"BC\\xc0\\xde")
"""

FAKE_AIR_RUN = """
import sys
print("air_run: fragment shader discards fragments", file=sys.stderr)
sys.exit(1)
"""

FAKE_IR3 = """
import sys
log = __import__("os").environ.get("IR3_LOG")
if log:
    open(log, "a").write(" ".join(sys.argv[1:]) + "\\n")
print("; FRAG prog 0/0: 2 instr, 0 nops, 0 half, 0 full")
print("  kill (0)")
"""


class AirToolsTest(unittest.TestCase):
    def test_chip_id_defaults_to_a830(self):
        self.assertEqual(air_env.A830_CHIP_ID, "0x44050001")
        self.assertEqual(air_env.resolve_chip_id("a830"), "0x44050001")
        self.assertEqual(air_env.resolve_chip_id("A830"), "0x44050001")
        self.assertEqual(air_env.resolve_chip_id("a635"), "0x007506030500")
        self.assertEqual(air_env.resolve_chip_id("0x1234"), "0x1234")

    def test_unwrap_fat_metallib(self):
        macho = b"\xcf\xfa\xed\xfe" + b"\x00" * 16
        mtlb = b"MTLB" + b"\x01\x00\x02\x00" + b"\x00" * 16
        fat = make_fat_container([(0x0100000C, macho), (0x01000013, mtlb)])
        unwrapped, kind = metallib.unwrap_fat_metallib(fat)
        self.assertEqual(kind, "fat")
        self.assertEqual(unwrapped, mtlb)

        no_mtlb = make_fat_container([(0x0100000C, macho)])
        self.assertEqual(metallib.unwrap_fat_metallib(no_mtlb), (no_mtlb, "fat-no-mtlb"))
        self.assertEqual(metallib.unwrap_fat_metallib(mtlb), (mtlb, "raw"))

    def test_corpus_fat_archive_and_ir3_skipped_vs_enabled(self):
        with tempfile.TemporaryDirectory() as td:
            air2nir = os.path.join(td, "air2nir")
            ir3 = os.path.join(td, "ir3_compiler")
            write_exec(air2nir, FAKE_AIR2NIR)

            fat_lib = os.path.join(td, "qc_default.metallib")
            macho = b"\xcf\xfa\xed\xfe" + b"\x00" * 16
            mtlb = b"MTLB" + b"\x00" * 16
            with open(fat_lib, "wb") as f:
                f.write(make_fat_container([(0x0100000C, macho), (0x01000013, mtlb)]))

            bin_lib = os.path.join(td, "ci_bin.metallib")
            with open(bin_lib, "wb") as f:
                f.write(macho)

            json_out = os.path.join(td, "report.json")
            md_out = os.path.join(td, "report.md")
            env = dict(os.environ, AIR2NIR=air2nir, IR3=os.path.join(td, "missing-ir3"))
            proc = subprocess.run(
                [sys.executable, os.path.join(AIR_DIR, "corpus.py"),
                 "--ir3", "--json", json_out, "--md", md_out, fat_lib, bin_lib],
                env=env, capture_output=True, text=True, check=True,
            )
            self.assertIn("ir3 is SKIPPED (not counted as passed)", proc.stderr)
            self.assertIn("ir3 0/2 (SKIPPED 1), archives SKIPPED 1", proc.stderr)

            with open(json_out) as f:
                data = json.load(f)
            self.assertEqual(data["chip_id"], "0x44050001")
            self.assertEqual(data["stages"], {"container": 2, "reader": 2, "translate": 1, "ir3": 0})
            self.assertEqual(data["skipped"]["ir3"], 1)
            self.assertEqual(len(data["skipped"]["archives"]), 1)
            self.assertEqual(data["rows"][0]["calls"],
                             ["air.discard_fragment", "air.sample_texture_2d.v4f32"])

            with open(md_out) as f:
                md = f.read()
            self.assertIn("| ir3 | 0 | 0% (SKIPPED: 1) |", md)
            self.assertIn("SKIPPED (precompiled-bin)", md)

            # With fake ir3_compiler present, --chip-id must be forwarded and ir3 counted.
            write_exec(ir3, FAKE_IR3)
            ir3_log = os.path.join(td, "ir3.log")
            env2 = dict(env, IR3=ir3, IR3_LOG=ir3_log)
            subprocess.run(
                [sys.executable, os.path.join(AIR_DIR, "corpus.py"),
                 "--ir3", "--chip-id", "a830", "--json", json_out, fat_lib],
                env=env2, capture_output=True, text=True, check=True,
            )
            with open(json_out) as f:
                data2 = json.load(f)
            self.assertEqual(data2["stages"]["ir3"], 1)
            self.assertNotIn("ir3", data2.get("skipped", {}))
            with open(ir3_log) as f:
                self.assertIn("--chip-id 0x44050001", f.read())

    def test_discardcheck_chip_id_skipped_and_check_roundtrip(self):
        with tempfile.TemporaryDirectory() as td:
            air2nir = os.path.join(td, "air2nir")
            llvm_as = os.path.join(td, "llvm-as")
            air_run = os.path.join(td, "air_run")
            ir3 = os.path.join(td, "ir3_compiler")
            write_exec(air2nir, FAKE_AIR2NIR)
            write_exec(llvm_as, FAKE_LLVM_AS)
            write_exec(air_run, FAKE_AIR_RUN)

            base_env = dict(os.environ, AIR2NIR=air2nir, LLVM_AS=llvm_as, AIR_RUN=air_run,
                            IR3=os.path.join(td, "missing-ir3"))
            proc = subprocess.run(
                [sys.executable, os.path.join(AIR_DIR, "discardcheck.py")],
                env=base_env, capture_output=True, text=True, check=True,
            )
            self.assertIn("ir3: 0 compiled, 2 SKIPPED (chip 0x44050001)", proc.stdout)

            write_exec(ir3, FAKE_IR3)
            ir3_log = os.path.join(td, "ir3.log")
            proc2 = subprocess.run(
                [sys.executable, os.path.join(AIR_DIR, "discardcheck.py"), "--chip-id", "a830"],
                env=dict(base_env, IR3=ir3, IR3_LOG=ir3_log),
                capture_output=True, text=True, check=True,
            )
            self.assertIn("ir3: 2 compiled, 0 SKIPPED (chip 0x44050001)", proc2.stdout)
            with open(ir3_log) as f:
                self.assertIn("--chip-id 0x44050001", f.read())

            emit_dir = os.path.join(td, "emit")
            subprocess.run(
                [sys.executable, os.path.join(AIR_DIR, "discardcheck.py"), "--emit", emit_dir],
                env=base_env, capture_output=True, text=True, check=True,
            )
            good_log = os.path.join(td, "good.log")
            with open(good_log, "w") as f:
                f.write(
                    "=== discard_all.bc\ncame back as\n0\n0\n0\n0\n"
                    "=== discard_quad.bc\ncame back as\n3\n3\n3\n3\n"
                )
            subprocess.run(
                [sys.executable, os.path.join(AIR_DIR, "discardcheck.py"),
                 "--check", emit_dir, good_log],
                env=base_env, capture_output=True, text=True, check=True,
            )
            bad_log = os.path.join(td, "bad.log")
            with open(bad_log, "w") as f:
                f.write(
                    "=== discard_all.bc\ncame back as\n7\n8\n9\n10\n"
                    "=== discard_quad.bc\ncame back as\n3\n3\n3\n3\n"
                )
            fail = subprocess.run(
                [sys.executable, os.path.join(AIR_DIR, "discardcheck.py"),
                 "--check", emit_dir, bad_log],
                env=base_env, capture_output=True, text=True,
            )
            self.assertEqual(fail.returncode, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
