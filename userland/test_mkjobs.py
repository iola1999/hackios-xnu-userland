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

    def test_feature_flag_conditionals(self):
        script = Path(__file__).with_name('mkjobs.py')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'ios'
            ff_dir = root / 'System/Library/FeatureFlags/Domain'
            ff_dir.mkdir(parents=True, exist_ok=True)
            (ff_dir / 'AirPlay.plist').write_bytes(plistlib.dumps({
                'airplayd': {'DevelopmentPhase': 'FeatureComplete'},
                'ugl': {'DevelopmentPhase': 'FeatureComplete'},
            }))
            (ff_dir / 'CameraCapture.plist').write_bytes(plistlib.dumps({
                'cameracaptured': {'DevelopmentPhase': 'FeatureComplete'},
            }))
            (ff_dir / 'TextComposer.plist').write_bytes(plistlib.dumps({
                'TextComposerDaemon': {'Enabled': True},
            }))

            daemons = root / 'System/Library/LaunchDaemons'
            daemons.mkdir(parents=True, exist_ok=True)
            for name in ('base', 'airplayd', 'mxd', 'cameracaptured', 'genexp', 'disabled_job'):
                prog = root / 'usr/libexec' / name
                prog.parent.mkdir(parents=True, exist_ok=True)
                prog.touch()

            (daemons / 'com.example.base.plist').write_bytes(plistlib.dumps({
                'Label': 'com.example.base',
                'Program': '/usr/libexec/base',
                'MachServices': {
                    'com.example.base': True,
                    'com.example.legacy': {'#IfFeatureFlagDisabled': 'AirPlay/airplayd', '#Then': True},
                },
            }))
            (daemons / 'com.apple.airplayd.plist').write_bytes(plistlib.dumps({
                'Label': 'com.apple.airplayd',
                'Program': '/usr/libexec/airplayd',
                'MachServices': {
                    'com.apple.coremedia.endpointmanager.xpc': {
                        '#IfFeatureFlagEnabled': 'AirPlay/airplayd', '#Then': True,
                    },
                    'com.apple.airplay.receiver.mediaremote.services': {
                        '#IfFeatureFlagEnabled': 'AirPlay/airplayd',
                        '#Then': {'#IfFeatureFlagEnabled': 'AirPlay/ugl', '#Then': True},
                    },
                },
            }))
            (daemons / 'com.example.mxd.plist').write_bytes(plistlib.dumps({
                'Label': 'com.example.mxd',
                'Program': '/usr/libexec/mxd',
                'MachServices': {
                    'com.example.mxd.core': True,
                    'com.apple.coremedia.endpointmanager.xpc': {
                        '#IfFeatureFlagDisabled': 'AirPlay/airplayd', '#Then': True,
                    },
                },
            }))
            (daemons / 'com.apple.cameracaptured.plist').write_bytes(plistlib.dumps({
                'Label': 'com.apple.cameracaptured',
                'Program': '/usr/libexec/cameracaptured',
                'MachServices': {
                    '#IfFeatureFlagEnabled': 'CameraCapture/cameracaptured',
                    '#Then': {'com.apple.coremedia.capturesource': True},
                },
            }))
            (daemons / 'com.example.genexp.plist').write_bytes(plistlib.dumps({
                'Label': 'com.example.genexp',
                'Program': '/usr/libexec/genexp',
                'MachServices': {
                    'com.example.genexp.session': True,
                    'com.example.textcomp': {
                        '#IfFeatureFlagDisabled': 'TextComposer/TextComposerDaemon',
                        '#Then': True,
                        '#Else': False,
                    },
                },
            }))
            (daemons / 'com.example.disabled.plist').write_bytes(plistlib.dumps({
                'Label': 'com.example.disabled',
                'Program': '/usr/libexec/disabled_job',
                'Disabled': {'#IfFeatureFlagDisabled': 'Missing/Flag', '#Then': True},
                'MachServices': {'com.example.disabled': True},
            }))

            dest = Path(directory) / 'output'
            subprocess.run([sys.executable, str(script), '--rest', str(root),
                            str(dest), 'com.example.base'], check=True, stdout=subprocess.DEVNULL)

            self.assertNotIn('com.example.disabled.plist', {p.name for p in dest.iterdir()})
            base = plistlib.loads((dest / 'com.example.base.plist').read_bytes())
            self.assertEqual(base['MachServices'], {'com.example.base': True})

            airplay = plistlib.loads((dest / 'com.apple.airplayd.plist').read_bytes())
            self.assertEqual(airplay['MachServices'], {
                'com.apple.coremedia.endpointmanager.xpc': True,
                'com.apple.airplay.receiver.mediaremote.services': True,
            })

            mxd = plistlib.loads((dest / 'com.example.mxd.plist').read_bytes())
            self.assertEqual(mxd['MachServices'], {'com.example.mxd.core': True})

            cam = plistlib.loads((dest / 'com.apple.cameracaptured.plist').read_bytes())
            self.assertEqual(cam['MachServices'], {'com.apple.coremedia.capturesource': True})

            genexp = plistlib.loads((dest / 'com.example.genexp.plist').read_bytes())
            self.assertEqual(genexp['MachServices'], {'com.example.genexp.session': True})


if __name__ == '__main__':
    unittest.main()
