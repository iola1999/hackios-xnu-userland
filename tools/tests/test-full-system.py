#!/usr/bin/env python3
"""Verify full iOS metadata, extraction state and build refusal with stale inputs."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'ios26/full-system.py'
spec = importlib.util.spec_from_file_location('full_system', SCRIPT)
full = importlib.util.module_from_spec(spec)
spec.loader.exec_module(full)


def entry(path, mode='-rwxr-xr-x', target='', size=100 << 20):
    return (f'Path = {path}\nMode = {mode}\nUser ID = 205\nGroup ID = 501\n'
            f'Size = {size}\nSymbolic Link = {target}\n\n')


class FullSystemTest(unittest.TestCase):
    def test_rsync_preserves_unreadable_empty_file_and_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            source, destination = base / 'source', base / 'destination'
            source.mkdir()
            empty = source / '.file'
            empty.touch()
            empty.chmod(0)
            os.utime(empty, ns=(1700000000123456789, 1700000000123456789))
            (source / 'regular').write_bytes(b'original content')
            (source / 'link').symlink_to('regular')
            listing = base / 'listing'
            listing.write_text(entry('.file', '----------', size=0))
            full.prepare_empty(listing, source, destination)
            self.assertEqual((destination / '.file').stat().st_mtime_ns, empty.stat().st_mtime_ns)
            result = subprocess.run(['rsync', '-a', str(source) + '/', str(destination) + '/'],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            actual = (destination / '.file').stat()
            self.assertEqual(actual.st_size, 0)
            self.assertEqual(actual.st_mode & 0o777, 0)
            self.assertEqual(int(actual.st_mtime), int(empty.stat().st_mtime))
            self.assertEqual((destination / 'regular').read_bytes(), b'original content')
            self.assertEqual((destination / 'link').readlink(), Path('regular'))

    def test_unreadable_nonempty_file_fails_before_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            source = base / 'source'
            source.mkdir()
            (source / 'file').write_bytes(b'content')
            (source / 'file').chmod(0)
            listing = base / 'listing'
            listing.write_text(entry('file', '----------', size=7))
            with self.assertRaisesRegex(ValueError, 'nonempty or mismatched'):
                full.prepare_empty(listing, source, base / 'destination')
            self.assertFalse((base / 'destination').exists())

    def test_manifest_retains_all_resources_and_metadata(self):
        paths = ['usr/libexec/locationd', 'usr/sbin/fairplayd.H2',
                 'System/Library/PrivateFrameworks/Example.framework/model.espresso.net',
                 'System/Library/Fonts/AppleColorEmoji.ttc', 'System/DriverKit/example',
                 'Applications/Example.app/fr.lproj/Localizable.strings',
                 'private/var/MobileAsset/example.dat', 'private/var/staged_system_apps/Example.app/Example']
        with tempfile.TemporaryDirectory() as temporary:
            listing, output = Path(temporary) / 'listing', Path(temporary) / 'manifest'
            listing.write_text(''.join(entry(p) for p in paths) + entry('var', 'lrwxr-xr-x', 'private/var'))
            full.manifest(listing, output)
            rows = {p[3]: p for p in (line.split('\t') for line in output.read_text().splitlines())}
            self.assertEqual(set(rows), set(paths) | {'var', 'Applications/Example.app/Example'})
            for path in paths:
                self.assertEqual(rows[path][:3], ['100755', '205', '501'])
            self.assertEqual(rows['var'], ['120755', '205', '501', 'var', 'private/var'])

    def test_state_rejects_empty_legacy_and_changed_scripts(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / 'ios26'
            directory.mkdir()
            for name in ['full-system.py', 'fetch.sh']:
                (directory / name).write_bytes((ROOT / 'ios26' / name).read_bytes())
            image = directory / 'image.dmg'
            image.write_bytes(b'original image')
            command = [sys.executable, str(directory / 'full-system.py')]
            state = subprocess.check_output(command + ['fingerprint', str(image)])
            for name in ['rootfs', 'cryptex', 'cryptex-app']:
                (directory / name).mkdir()
                (directory / ('.done-' + name)).write_bytes(state)
            (directory / 'system.manifest').touch()
            subprocess.run(command + ['check', str(directory)], check=True)
            image.write_bytes(b'updated image')
            self.assertNotEqual(state, subprocess.check_output(command + ['fingerprint', str(image)]))
            stamp = directory / '.done-rootfs'
            stamp.write_bytes(b'')
            self.assertNotEqual(subprocess.run(command + ['check', str(directory)], capture_output=True).returncode, 0)
            stamp.write_bytes(state)
            old = (directory / 'fetch.sh').stat()
            with (directory / 'fetch.sh').open('a') as stream:
                stream.write('\n# changed preparation\n')
            os.utime(directory / 'fetch.sh', ns=(old.st_atime_ns, old.st_mtime_ns))
            self.assertNotEqual(subprocess.run(command + ['check', str(directory)], capture_output=True).returncode, 0)

    def test_compile_only_refuses_stale_extraction_before_compiling(self):
        with tempfile.TemporaryDirectory() as temporary:
            ios = Path(temporary) / 'ios26'
            ios.mkdir()
            for name in ['full-system.py', 'fetch.sh']:
                (ios / name).write_bytes((ROOT / 'ios26' / name).read_bytes())
            (ios / 'system.manifest').touch()
            (ios / '.done-rootfs').touch()
            result = subprocess.run(['bash', str(ROOT / 'userland/build.sh')],
                                    env=dict(os.environ, IOS=str(ios), COMPILE_ONLY='1'),
                                    capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Expecting value', result.stderr)

    def test_missing_binary_stays_out_of_job_cache(self):
        import plistlib
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'root'
            jobs = root / 'System/Library/LaunchDaemons'
            jobs.mkdir(parents=True)
            for label in ['com.example.present', 'com.example.missing']:
                (jobs / (label + '.plist')).write_bytes(plistlib.dumps(dict(
                    Label=label, Program='/usr/libexec/' + label, MachServices={label: True})))
            binary = root / 'usr/libexec/com.example.present'
            binary.parent.mkdir(parents=True)
            binary.touch()
            output = Path(temporary) / 'jobs'
            subprocess.run([sys.executable, str(ROOT / 'userland/mkjobs.py'), '--rest', str(root), str(output)],
                           check=True, stdout=subprocess.DEVNULL)
            self.assertEqual([p.name for p in output.iterdir()], ['com.example.present.plist'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
