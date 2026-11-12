#!/usr/bin/env python3
"""编译并运行真实身份探针，检查输入、缺失应用和系统 TextEdit 查询。

需要 macOS、Xcode 命令行工具，以及本机已注册的系统 TextEdit。
私有框架的宿主结果只验证探针行为；iOS 查询权限和字段仍需设备验证。
"""

import argparse
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
import uuid

SOURCE = Path(__file__).with_name("app-identity.m")


@unittest.skipUnless(sys.platform == "darwin", "requires macOS private frameworks")
class AppIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="app-identity-tests-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = Path(cls.temporary.name) / "app-identity"
        subprocess.run([
            "xcrun", "--sdk", "macosx", "clang", "-fobjc-arc", "-Os",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-framework", "Foundation", str(SOURCE), "-o", str(cls.binary),
        ], check=True, capture_output=True, timeout=120)

    def run_probe(self, arguments, expected_exit):
        result = subprocess.run(
            [str(self.binary), *arguments], capture_output=True, timeout=20,
        )
        self.assertEqual(result.returncode, expected_exit, result.stderr.decode(errors="replace"))
        self.assertNotIn(b"Sanitizer", result.stderr)
        return result

    def test_no_arguments(self):
        self.assertEqual(self.run_probe([], 2).stdout, b"")

    def test_empty_identifier(self):
        self.assertEqual(self.run_probe([""], 2).stdout, b"")

    def test_extra_argument(self):
        self.assertEqual(self.run_probe(["com.apple.TextEdit", "extra"], 2).stdout, b"")

    def test_invalid_utf8(self):
        self.assertEqual(self.run_probe([b"\xff"], 2).stdout, b"")

    def test_help(self):
        self.assertEqual(self.run_probe(["--help"], 0).stdout, b"")

    def test_missing_application_skips_fbs(self):
        identifier = f"org.example.appidentity.missing.{uuid.uuid4().hex}"
        report = json.loads(self.run_probe([identifier], 1).stdout)
        self.assertEqual(report["bundle_id"], identifier)
        self.assertIs(report["query_complete"], False)
        self.assertTrue(report["errors"])
        self.assertEqual(report["fbs"], {"status": "skipped"})
        self.assertTrue(any(item["field"] == "LSApplicationRecord" for item in report["errors"]))
        self.assertTrue(any(item["field"] == "FBSApplicationInfo" for item in report["errors"]))

    def test_registered_system_application(self):
        metadata = Path("/System/Applications/TextEdit.app/Contents/Info.plist")
        if not metadata.is_file():
            self.skipTest("requires the system TextEdit at /System/Applications/TextEdit.app")
        identifier = plistlib.loads(metadata.read_bytes())["CFBundleIdentifier"]
        report = json.loads(self.run_probe([identifier], 0).stdout)
        self.assertIs(report["query_complete"], True)
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["proxy"]["bundleIdentifier"], identifier)
        self.assertIs(report["proxy"]["isInstalled"], True)
        self.assertIs(report["proxy"]["appState"]["isValid"], True)
        self.assertEqual(report["record"]["bundleIdentifier"], identifier)
        self.assertTrue(report["record"]["identities"])
        self.assertEqual(report["fbs"]["status"], "queried")
        self.assertEqual(report["fbs"]["applicationIdentity"]["bundleIdentifier"], identifier)
        self.assertEqual(report["caller"], {"uid": os.getuid(), "euid": os.geteuid()})
        for field, inode_field in (("URL", "bundleInode"), ("executableURL", "executableInode")):
            observed = report["files"][f"record.{field}"]
            actual = os.stat(observed["path"])
            self.assertEqual(observed["st_dev"], actual.st_dev)
            self.assertEqual(observed["st_ino"], actual.st_ino)
            self.assertEqual(report["record"][inode_field], actual.st_ino)
        self.assertEqual(report["files"]["proxy.canonicalExecutablePath"]["path"],
                         report["files"]["fbs.executableURL"]["path"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    args, remaining = parser.parse_known_args()
    SOURCE = args.source.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
