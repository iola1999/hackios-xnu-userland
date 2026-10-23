#!/usr/bin/env python3
"""Check that on-demand jobs retain separate filenames and Mach services."""
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest


class JobGenerationTest(unittest.TestCase):
    def test_on_demand_jobs_with_environment_setting(self):
        script = Path(__file__).with_name('mkjobs.py')
        for packed in (None, '1'):
            with self.subTest(packed=packed), tempfile.TemporaryDirectory() as directory:
                root = Path(directory) / 'ios'
                jobs = [('LaunchDaemons', 'com.example.base'),
                        ('LaunchDaemons', 'com.example.worker'),
                        ('LaunchAngels', 'com.example.poster')]
                for kind, label in jobs:
                    folder = root / 'System/Library' / kind
                    folder.mkdir(parents=True, exist_ok=True)
                    program = '/usr/libexec/' + label
                    binary = root / program.lstrip('/')
                    binary.parent.mkdir(parents=True, exist_ok=True)
                    binary.touch()
                    data = dict(Label=label, Program=program, RunAtLoad=True,
                                KeepAlive=True, MachServices={label: True})
                    (folder / (label + '.plist')).write_bytes(plistlib.dumps(data))
                dest = Path(directory) / 'output'
                env = dict(os.environ)
                env.pop('RWA_PACKED16', None)
                if packed is not None:
                    env['RWA_PACKED16'] = packed
                subprocess.run([sys.executable, str(script), '--rest', str(root),
                                str(dest), jobs[0][1]], env=env, check=True,
                               stdout=subprocess.DEVNULL)
                self.assertEqual({p.name for p in dest.iterdir()},
                                 {label + '.plist' for _, label in jobs})
                for index, (_, label) in enumerate(jobs):
                    data = plistlib.loads((dest / (label + '.plist')).read_bytes())
                    self.assertEqual(data['Label'], label)
                    self.assertEqual(data['MachServices'], {label: True})
                    self.assertEqual(data.get('EnvironmentVariables', {}).get('RWA_PACKED16'), packed)
                    if index:
                        self.assertNotIn('RunAtLoad', data)
                        self.assertNotIn('KeepAlive', data)


if __name__ == '__main__':
    unittest.main()
