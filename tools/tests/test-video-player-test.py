#!/usr/bin/env python3
"""Run the production AVPlayer probe on macOS and compare pixels with ffmpeg.

Requires Xcode, ffmpeg and ffprobe. VIDEO_PLAYER_TEST_MEDIA can select an existing
12-frame 160x96 H.264 sample; otherwise the test generates one. Outputs stay under
out/. This verifies local playback and pixel extraction, without GPU presentation.
"""
from fractions import Fraction
from pathlib import Path
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'userland/src/tests/video_player_test.m'
FRAME_BYTES = 160 * 96 * 3 // 2


def records(output, stage):
    return [dict(re.findall(r'(\w+)=([^ ]+)', line)) for line in output.splitlines()
            if line.startswith('video_player_test: ') and f'stage={stage} ' in line]


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f'{value:016x}'


def build(source, binary):
    subprocess.run(['xcrun', '--sdk', 'macosx', 'clang', '-fobjc-arc', '-O1',
                    '-Wall', '-Wextra', '-Werror', '-framework', 'Foundation',
                    '-framework', 'AVFoundation', '-framework', 'CoreMedia',
                    '-framework', 'CoreVideo', str(source), '-o', str(binary)], check=True)


class VideoPlayerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
            raise unittest.SkipTest('ffmpeg and ffprobe are required')
        cls.directory = Path(os.environ.get('VIDEO_PLAYER_TEST_OUT', ROOT / 'out/video-player-test'))
        cls.directory.mkdir(parents=True, exist_ok=True)
        cls.binary = cls.directory / 'video_player_test_macos'
        build(SOURCE, cls.binary)
        cls.movie = cls.directory / 'synthetic-h264.mp4'
        existing = os.environ.get('VIDEO_PLAYER_TEST_MEDIA')
        if existing:
            shutil.copyfile(existing, cls.movie)
        else:
            subprocess.run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                            '-i', 'testsrc2=size=160x96:rate=12', '-frames:v', '12', '-c:v', 'libx264',
                            '-preset', 'veryfast', '-pix_fmt', 'yuv420p', '-bf', '2', '-movflags',
                            '+faststart', str(cls.movie)], check=True, timeout=30)
        raw = subprocess.check_output(['ffmpeg', '-hide_banner', '-loglevel', 'error',
                                       '-i', str(cls.movie), '-f', 'rawvideo', '-pix_fmt', 'nv12', '-'], timeout=30)
        media = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
            '-show_entries', 'stream=time_base:frame=best_effort_timestamp', '-of', 'json', str(cls.movie)], timeout=30))
        if len(raw) != 12 * FRAME_BYTES or len(media['frames']) != 12:
            raise AssertionError('reference must have 12 complete 160x96 NV12 frames')
        base = Fraction(media['streams'][0]['time_base'])
        cls.reference = {int(frame['best_effort_timestamp']) * base: fnv1a(raw[i*FRAME_BYTES:(i+1)*FRAME_BYTES])
                         for i, frame in enumerate(media['frames'])}
        (cls.directory / 'reference.json').write_text(json.dumps({str(k): v for k, v in cls.reference.items()}, indent=2))
        cls.invalid = cls.directory / 'invalid.mp4'
        cls.invalid.write_bytes(b'invalid video input\n')
        cls.truncated = cls.directory / 'truncated.mp4'
        cls.truncated.write_bytes(cls.movie.read_bytes()[:64])
        data = bytearray(cls.movie.read_bytes())
        offset = 0
        while offset + 8 <= len(data):
            size, kind = struct.unpack_from('>I4s', data, offset)
            if size < 8 or offset + size > len(data):
                raise AssertionError('unexpected MP4 box')
            if kind == b'mdat':
                data[offset + 8:offset + size] = bytes(size - 8)
                break
            offset += size
        else:
            raise AssertionError('sample has no mdat box')
        cls.damaged = cls.directory / 'damaged-nal.mp4'
        cls.damaged.write_bytes(data)
        cls.long_movie = cls.directory / 'synthetic-h264-72.mp4'
        subprocess.run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                        '-i', 'testsrc2=size=160x96:rate=12', '-frames:v', '72', '-c:v', 'libx264',
                        '-preset', 'veryfast', '-pix_fmt', 'yuv420p', '-bf', '2', '-movflags',
                        '+faststart', str(cls.long_movie)], check=True, timeout=30)

    def run_probe(self, *args, status=0, binary=None, label='', **kwargs):
        result = subprocess.run([str(binary or self.binary), *map(str, args)],
                                capture_output=True, text=True, timeout=15, **kwargs)
        log = result.stdout + result.stderr
        (self.directory / f'{self._testMethodName}{label}.log').write_text(log)
        self.assertEqual(result.returncode, status, log)
        self.assertNotIn(str(self.directory), log)
        self.assertNotIn('file://', log)
        return result.stdout

    def test_default_player_pixels_and_end(self):
        output = self.run_probe(self.movie)
        summary = records(output, 'summary')[-1]
        self.assertEqual(summary['result'], 'pass')
        for key in ('ready', 'playing', 'advanced', 'ended'):
            self.assertEqual(summary[key], '1', output)
        self.assertEqual(summary['pixel_errors'], '0')
        self.assertEqual(records(output, 'end')[0]['received'], '1')
        self.assertEqual(len(records(output, 'end')), 1)
        self.assertGreaterEqual(Fraction(summary['max_time']), Fraction(11, 12))
        frames = records(output, 'frame')
        # Every frame is summarized in memory; only the first three and each sixtieth are logged.
        self.assertGreaterEqual(len(frames), 2)
        self.assertLessEqual(len(frames), 12)
        self.assertEqual(int(summary['frame_logs']), len(frames))
        self.assertGreaterEqual(int(summary['frames']), len(frames))
        self.assertEqual(int(summary['output_bytes']), int(summary['frames']) * FRAME_BYTES)
        self.assertEqual(records(output, 'first_frame')[0]['elapsed_ms'], summary['first_frame_ms'])
        self.assertGreater(int(records(output, 'start')[0]['epoch_us']), 0)
        for stage in ('item_created', 'output_added', 'player_created', 'play', 'play_returned'):
            self.assertIn('elapsed_ms', records(output, stage)[0])
        states = records(output, 'state')
        self.assertTrue(states)
        for key in ('elapsed_ms', 'waiting_reason', 'buffer_empty', 'likely_to_keep_up', 'loaded', 'has_new'):
            self.assertIn(key, states[-1])
        self.assertTrue(any(state['likely_to_keep_up'] == '1' for state in states))
        previous = Fraction(-1)
        for frame in frames:
            self.assertEqual(frame['pixel_error'], '0')
            self.assertEqual(frame['format'], '0x34323076')
            self.assertEqual((frame['width'], frame['height']), ('160', '96'))
            self.assertEqual(int(frame['bytes']), FRAME_BYTES)
            self.assertTrue(int(frame['pts_flags'], 16) & 1)
            pts = Fraction(frame['pts'])
            self.assertGreater(pts, previous)
            self.assertEqual(frame['fnv1a64'], self.reference[pts])
            self.assertLess(int(frame['min']), int(frame['max']))
            previous = pts

    def test_invalid_and_truncated_input(self):
        for path in (self.invalid, self.truncated):
            with self.subTest(input=path.name):
                output = self.run_probe(path, status=1, label='-' + path.stem)
                self.assertEqual(records(output, 'summary')[-1]['result'], 'fail')
                self.assertNotEqual(records(output, 'item')[-1]['error'], '0')
                self.assertNotIn(path.name, output)

    def test_damaged_media_data(self):
        output = self.run_probe(self.damaged, status=1)
        summary = records(output, 'summary')[-1]
        self.assertEqual(summary['result'], 'fail')
        self.assertEqual(summary['frames'], '0')

    def test_missing_input(self):
        output = self.run_probe(self.directory / 'private-path-marker.mp4', status=1)
        self.assertNotIn('private-path-marker', output)
        self.assertTrue(records(output, 'input'))
        self.assertEqual(records(output, 'summary')[-1]['result'], 'fail')

    def test_argument_validation(self):
        for index, value in enumerate(('0', '-1', 'nan', 'inf', '3601', '1x')):
            self.run_probe('--deadline', value, self.movie, status=2, label=str(index))
        self.run_probe('--deadline', status=2, label='-missing')
        self.run_probe('--software', self.movie, status=2, label='-unknown')

    def test_process_deadline(self):
        start = time.monotonic()
        output = self.run_probe('--deadline', '0.000001', self.movie, status=124)
        self.assertLess(time.monotonic() - start, 3)
        self.assertEqual(records(output, 'summary')[-1]['result'], 'fail')

    def test_inherited_blocked_alarm(self):
        output = self.run_probe('--deadline', '0.000001', self.movie, status=124,
            preexec_fn=lambda: signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGALRM}))
        self.assertEqual(records(output, 'summary')[-1]['result'], 'fail')

    def test_deadline_during_playback(self):
        output = self.run_probe('--deadline', '0.4', self.movie, status=124)
        self.assertIn('video_player_test: stage=start ', output)
        summary = records(output, 'summary')[-1]
        self.assertEqual(summary['result'], 'fail')
        self.assertIn('elapsed_ms', summary)
        self.assertIn('frame_logs', summary)

    def test_frame_log_downsampling(self):
        output = self.run_probe(self.long_movie)
        summary = records(output, 'summary')[-1]
        frames = records(output, 'frame')
        self.assertEqual(summary['result'], 'pass')
        self.assertGreater(int(summary['frames']), 3)
        self.assertEqual(int(summary['frame_logs']), len(frames))
        self.assertLess(len(frames), int(summary['frames']))
        self.assertEqual([int(frame['index']) for frame in frames[:3]], [1, 2, 3])
        for frame in frames[3:]:
            self.assertEqual(int(frame['index']) % 60, 0)
        self.assertEqual(int(summary['output_bytes']), int(summary['frames']) * FRAME_BYTES)

    def test_end_without_pixels_fails(self):
        source = self.directory / 'no-pixels.m'
        text = SOURCE.read_text()
        original = 'if ([output hasNewPixelBufferForItemTime:request])'
        self.assertEqual(text.count(original), 1)
        source.write_text(text.replace(original, '(void)request; if (false)'))
        binary = self.directory / 'no-pixels'
        build(source, binary)
        output = self.run_probe(self.movie, binary=binary, status=1)
        summary = records(output, 'summary')[-1]
        self.assertEqual(summary['ended'], '1')
        self.assertEqual(summary['frames'], '0')
        self.assertEqual(summary['result'], 'fail')


if __name__ == '__main__':
    unittest.main(verbosity=2)
