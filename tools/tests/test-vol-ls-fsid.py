#!/usr/bin/env python3
"""Compile vol_ls with ASan+UBSan on macOS and verify the read-only fsid subcommand."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'userland/src/apps/vol_ls.c'
UUID_RE = re.compile(r'^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$')
FSID_RE = re.compile(r'^[0-9a-f]{8}:[0-9a-f]{8}$')
FIELD_RE = re.compile(r'(\w+)=(?:"([^"]*)"|([^ ]+))')


def parse_fsid_lines(output):
    rows = []
    for line in output.splitlines():
        if not line.startswith('vol_ls: fsid '):
            continue
        fields = {}
        for match in FIELD_RE.finditer(line[len('vol_ls: fsid '):]):
            fields[match.group(1)] = match.group(2) if match.group(2) is not None else match.group(3)
        rows.append(fields)
    return rows


class VolLsFsidTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmpdir = tempfile.TemporaryDirectory(prefix='vol-ls-fsid-')
        cls.workdir = Path(cls.tmpdir.name)
        cls.binary = cls.workdir / 'vol_ls_macos'
        subprocess.run(
            ['xcrun', '--sdk', 'macosx', 'clang', '-O1', '-g',
             '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             str(SOURCE), '-o', str(cls.binary)],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmpdir.cleanup()

    def run_vol_ls(self, *args):
        return subprocess.run(
            [str(self.binary), *map(str, args)],
            capture_output=True, text=True, timeout=15,
        )

    def test_root_dir_file_and_symlink_roundtrip(self):
        nested_dir = self.workdir / 'dir with spaces'
        nested_dir.mkdir()
        sample_file = nested_dir / 'sample file.txt'
        sample_file.write_text('hackios fsid check\n')
        symlink = self.workdir / 'link-to-sample'
        symlink.symlink_to(sample_file)

        targets = [Path('/'), nested_dir, sample_file, symlink]
        proc = self.run_vol_ls('fsid', *targets)
        self.assertEqual(proc.returncode, 0, msg=proc.stdout + proc.stderr)
        rows = parse_fsid_lines(proc.stdout)
        self.assertEqual(len(rows), len(targets), msg=proc.stdout)

        for target, row in zip(targets, rows):
            st = os.stat(target)
            self.assertEqual(row.get('result'), 'ok', msg=row)
            self.assertEqual(row.get('roundtrip'), 'ok', msg=row)
            self.assertIn(row.get('src'), ('statfs', 'getattrlist'), msg=row)
            self.assertEqual(row.get('fs'), 'apfs', msg=row)
            self.assertEqual(int(row['dev'], 16), st.st_dev & 0xffffffff, msg=row)
            self.assertEqual(int(row['ino']), st.st_ino, msg=row)
            self.assertEqual(int(row['fileid']), st.st_ino, msg=row)
            self.assertRegex(row.get('sf_fsid', ''), FSID_RE)
            self.assertRegex(row.get('ga_fsid', ''), FSID_RE)
            self.assertRegex(row.get('objid', ''), FSID_RE)
            self.assertRegex(row.get('vol_uuid', ''), UUID_RE)
            self.assertNotEqual(row['vol_uuid'], '00000000-0000-0000-0000-000000000000')
            self.assertEqual(row.get('path'), str(target), msg=row)
            resolved = row.get('resolved', '')
            self.assertTrue(resolved.startswith('/'), msg=row)
            st2 = os.stat(resolved)
            self.assertEqual((st2.st_dev, st2.st_ino), (st.st_dev, st.st_ino), msg=row)

    def test_missing_path_fails_without_aborting_remaining_paths(self):
        existing = self.workdir / 'present.txt'
        existing.write_bytes(b'ok')
        missing = self.workdir / 'does-not-exist'

        proc = self.run_vol_ls('fsid', missing, existing)
        self.assertEqual(proc.returncode, 1, msg=proc.stdout + proc.stderr)
        rows = parse_fsid_lines(proc.stdout)
        self.assertEqual(len(rows), 2, msg=proc.stdout)
        self.assertEqual(rows[0].get('result'), 'fail', msg=rows[0])
        self.assertEqual(rows[0].get('stage'), 'stat', msg=rows[0])
        self.assertEqual(rows[1].get('result'), 'ok', msg=rows[1])
        self.assertEqual(rows[1].get('roundtrip'), 'ok', msg=rows[1])

    def test_missing_argument_and_unknown_subcommand_exit_two(self):
        proc_short = self.run_vol_ls('fsid')
        self.assertEqual(proc_short.returncode, 2, msg=proc_short.stdout + proc_short.stderr)
        self.assertIn('fsid PATH...', proc_short.stdout)

        proc_bad = self.run_vol_ls('no_such_cmd', '/')
        self.assertEqual(proc_bad.returncode, 2, msg=proc_bad.stdout + proc_bad.stderr)


if __name__ == '__main__':
    unittest.main()
