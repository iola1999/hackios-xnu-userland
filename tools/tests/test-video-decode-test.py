#!/usr/bin/env python3
"""Exercise the production VideoToolbox probe on macOS with generated H.264.

Requires Xcode and ffmpeg with libx264. Assets and binaries remain under out/.
The software result is compared with ffmpeg's decoded NV12 bytes. Hardware success
requires the actual VT property; unavailable hardware is reported as a skipped test.
This does not test the device decoder, GPU composition, presentation or audio.
"""
from fractions import Fraction
from pathlib import Path
import os
import re
import shutil
import signal
import struct
import subprocess
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'userland/src/tests/video_decode_test.m'
FRAME_BYTES = 160 * 96 * 3 // 2


def records(output, stage):
    result = []
    for line in output.splitlines():
        if line.startswith('video_decode_test: ') and f'stage={stage} ' in line:
            result.append(dict(re.findall(r'(\w+)=([^ ]+)', line)))
    return result


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f'{value:016x}'


def build(source, binary):
    subprocess.run(['xcrun', '--sdk', 'macosx', 'clang', '-fobjc-arc', '-O1',
                    '-Wall', '-Wextra', '-Werror', '-framework', 'Foundation',
                    '-framework', 'AVFoundation', '-framework', 'CoreMedia',
                    '-framework', 'CoreVideo', '-framework', 'VideoToolbox',
                    str(source), '-o', str(binary)], check=True)


class VideoDecodeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ffmpeg = shutil.which('ffmpeg')
        if cls.ffmpeg is None:
            raise unittest.SkipTest('ffmpeg is required for synthetic test media')
        cls.directory = Path(os.environ.get('VIDEO_DECODE_TEST_OUT', ROOT / 'out/video-decode-test'))
        cls.directory.mkdir(parents=True, exist_ok=True)
        cls.binary = cls.directory / 'video_decode_test_macos'
        build(SOURCE, cls.binary)
        cls.movie = cls.directory / 'synthetic-h264.mp4'
        subprocess.run([cls.ffmpeg, '-y', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                        '-i', 'testsrc2=size=160x96:rate=12', '-frames:v', '12', '-c:v', 'libx264',
                        '-preset', 'veryfast', '-pix_fmt', 'yuv420p', '-bf', '2', '-movflags',
                        '+faststart', str(cls.movie)], check=True, timeout=30)
        raw = subprocess.check_output([cls.ffmpeg, '-hide_banner', '-loglevel', 'error',
                                       '-i', str(cls.movie), '-f', 'rawvideo', '-pix_fmt', 'nv12',
                                       '-'], timeout=30)
        if len(raw) != 12 * FRAME_BYTES:
            raise AssertionError('synthetic reference must contain 12 complete frames')
        cls.reference = [fnv1a(raw[i:i + FRAME_BYTES]) for i in range(0, len(raw), FRAME_BYTES)]
        cls.invalid = cls.directory / 'invalid.mp4'
        cls.invalid.write_bytes(b'invalid video input\n')
        cls.truncated = cls.directory / 'truncated.mp4'
        cls.truncated.write_bytes(cls.movie.read_bytes()[:64])
        data = bytearray(cls.movie.read_bytes())
        offset = 0
        while offset + 8 <= len(data):
            size, kind = struct.unpack_from('>I4s', data, offset)
            if size < 8 or offset + size > len(data):
                raise AssertionError('unexpected synthetic MP4 box')
            if kind == b'mdat':
                data[offset + 8:offset + size] = bytes(size - 8)
                break
            offset += size
        else:
            raise AssertionError('synthetic MP4 has no media-data box')
        cls.damaged = cls.directory / 'damaged-nal.mp4'
        cls.damaged.write_bytes(data)

    def run_probe(self, *args, status=0, binary=None):
        result = subprocess.run([str(binary or self.binary), *map(str, args)],
                                capture_output=True, text=True, timeout=15)
        log = result.stdout + result.stderr
        (self.directory / f'{self._testMethodName}.log').write_text(log)
        self.assertEqual(result.returncode, status, log)
        self.assertNotIn(str(self.directory), log)
        return result.stdout

    def check_frames(self, output, count):
        summary = records(output, 'summary')[-1]
        self.assertEqual(summary['result'], 'pass')
        for key in ('submitted', 'accepted', 'callbacks', 'frames'):
            self.assertEqual(int(summary[key]), count, output)
        self.assertEqual(int(summary['output_bytes']), count * FRAME_BYTES)
        frames = records(output, 'callback')
        self.assertEqual(len(frames), count)
        for frame in frames:
            self.assertEqual(frame['status'], '0')
            self.assertEqual(frame['pixel_error'], '0')
            self.assertEqual(frame['format'], '0x34323076')
            self.assertEqual((frame['width'], frame['height']), ('160', '96'))
            self.assertEqual(int(frame['bytes']), FRAME_BYTES)
            self.assertLess(int(frame['min']), int(frame['max']))
            self.assertTrue(int(frame['pts_flags'], 16) & 1)
        return sorted(frames, key=lambda row: Fraction(row['pts']))

    def test_software_bytes_match_independent_decoder(self):
        output = self.run_probe('--software', self.movie)
        frames = self.check_frames(output, 12)
        self.assertEqual([row['fnv1a64'] for row in frames], self.reference)
        # Apple's software decoder can report PropertyNotSupported for this query.
        self.assertIn(records(output, 'summary')[-1]['hardware'], ('-1', '0'))

    def test_callback_report(self):
        report = self.directory / 'callbacks.txt'
        report.unlink(missing_ok=True)
        output = self.run_probe('--software', '--report-file', report, self.movie)
        frames = self.check_frames(output, 12)
        recorded = report.read_text()
        self.assertEqual(records(recorded, 'callback'), records(output, 'callback'))
        self.assertEqual([row['fnv1a64'] for row in frames], self.reference)
        completion = records(recorded, 'report_complete')
        self.assertEqual(len(completion), 1)
        self.assertEqual((completion[0]['callbacks'], completion[0]['frames']), ('12', '12'))

    def test_existing_report_is_preserved(self):
        report = self.directory / 'existing-report.txt'
        contents = b'report content to preserve\n'
        report.write_bytes(contents)
        output = self.run_probe('--software', '--report-file', report, self.movie, status=1)
        self.assertTrue(records(output, 'report_open'))
        self.assertEqual(report.read_bytes(), contents)

    def test_required_hardware(self):
        result = subprocess.run([str(self.binary), '--require-hardware', str(self.movie)],
                                capture_output=True, text=True, timeout=15)
        (self.directory / f'{self._testMethodName}.log').write_text(result.stdout + result.stderr)
        created = records(result.stdout, 'create')
        unavailable = {'-12906', '-12910', '-12913'}
        if result.returncode == 1 and created and created[0]['status'] in unavailable:
            self.skipTest(f"hardware session unavailable: status={created[0]['status']}")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        frames = self.check_frames(result.stdout, 12)
        self.assertEqual([row['fnv1a64'] for row in frames], self.reference)
        self.assertEqual(records(result.stdout, 'summary')[-1]['hardware'], '1')

    def test_frame_limit(self):
        output = self.run_probe('--software', '--frames', '3', self.movie)
        self.check_frames(output, 3)
        self.assertEqual(records(output, 'summary')[-1]['limit_reached'], '1')

    def test_invalid_and_truncated_input(self):
        for path in (self.invalid, self.truncated):
            with self.subTest(input=path.name):
                output = self.run_probe(path, status=1)
                self.assertEqual(records(output, 'summary')[-1]['frames'], '0')

    def test_damaged_compressed_samples(self):
        output = self.run_probe('--software', self.damaged, status=1)
        self.assertEqual(records(output, 'create')[0]['status'], '0')
        self.assertEqual(records(output, 'summary')[-1]['result'], 'fail')
        errors = [row for row in records(output, 'submit') + records(output, 'callback')
                  if row['status'] != '0']
        self.assertTrue(errors, output)

    def test_no_private_path_in_errors(self):
        output = self.run_probe(self.directory / 'private-path-marker.mp4', status=1)
        self.assertNotIn('private-path-marker', output)

    def test_argument_validation(self):
        for flag, value in [('--frames', '0'), ('--frames', '-1'), ('--frames', '1x'),
                            ('--frames', '10001'), ('--deadline', '0'), ('--deadline', 'nan'),
                            ('--deadline', 'inf'), ('--deadline', '-1'), ('--deadline', '3601')]:
            with self.subTest(flag=flag, value=value):
                self.run_probe(flag, value, self.movie, status=2)
        self.run_probe('--software', '--require-hardware', self.movie, status=2)
        self.run_probe('--frames', status=2)

    def test_process_deadline(self):
        start = time.monotonic()
        self.run_probe('--deadline', '0.000001', self.movie, status=124)
        self.assertLess(time.monotonic() - start, 3)

    def test_inherited_blocked_alarm(self):
        result = subprocess.run([str(self.binary), '--deadline', '0.000001', str(self.movie)],
                                capture_output=True, text=True, timeout=3,
                                preexec_fn=lambda: signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGALRM}))
        self.assertEqual(result.returncode, 124, result.stdout + result.stderr)

    def test_required_hardware_rejects_software_session(self):
        # Inject a decoder-selection defect while retaining production validation.
        # Native VT still decodes the generated media and reports its real property.
        text = SOURCE.read_text()
        for original in ('@(!options->software)', '@(options->require_hardware)'):
            self.assertEqual(text.count(original), 1)
            text = text.replace(original, '@NO')
        source = self.directory / 'forced-software.m'
        source.write_text(text)
        binary = self.directory / 'forced-software'
        build(source, binary)
        output = self.run_probe('--require-hardware', self.movie, binary=binary, status=1)
        summary = records(output, 'summary')[-1]
        self.assertEqual(summary['frames'], '12')
        self.assertIn(summary['hardware'], ('-1', '0'))
        self.assertEqual(summary['result'], 'fail')


if __name__ == '__main__':
    unittest.main(verbosity=2)
