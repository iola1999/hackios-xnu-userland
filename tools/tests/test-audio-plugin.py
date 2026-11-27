#!/usr/bin/env python3
"""Call the complete production HAL plugin with host device and clock stubs.

This checks PCM format, write results, clock generations and speaker statistics.
It does not load the plugin into coreaudiod or access audio hardware.
"""
from pathlib import Path
import json
import os
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get('AUDIO_PLUGIN_SOURCE', ROOT / 'userland/src/services/audio_plugin.c'))

HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "annibale_audio.h"

static uint64_t fake_ticks = 1000000;
static bool device_available = true;
static int write_mode, device_writes;
static unsigned ioctl_vol_calls;
static uint32_t last_vol_below, last_vol_muted;
static size_t last_bytes;
static unsigned char last_samples[64];

static int fake_open(const char *path, int flags, ...) {
    (void)path; (void)flags;
    if (!device_available) { errno = ENOENT; return -1; }
    return 10;
}
static int fake_close(int fd) { (void)fd; return 0; }
static int fake_ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    if (request == ANNIBALE_AUDIO_SET_VOLUME) {
        va_list ap;
        va_start(ap, request);
        const struct annibale_audio_volume *v = va_arg(ap, const struct annibale_audio_volume *);
        va_end(ap);
        ioctl_vol_calls++;
        last_vol_below = v->below;
        last_vol_muted = v->muted;
    }
    return 0;
}
static ssize_t fake_read(int fd, void *buffer, size_t count) {
    (void)fd; (void)buffer; (void)count; errno = EAGAIN; return -1;
}
static ssize_t fake_write(int fd, const void *buffer, size_t count) {
    if (fd == STDERR_FILENO) return (ssize_t)count;
    assert(fd == 10 && count <= sizeof(last_samples));
    device_writes++;
    last_bytes = count;
    memcpy(last_samples, buffer, count);
    if (write_mode == 1) { errno = EIO; return -1; }
    if (write_mode == 2) return 8;
    if (write_mode == 3) return 10;
    return (ssize_t)count;
}
static uint64_t fake_absolute_time(void) { return fake_ticks; }
static kern_return_t fake_timebase_info(mach_timebase_info_t value) {
    value->numer = 625; value->denom = 12;
    return KERN_SUCCESS;
}
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define read fake_read
#define write fake_write
#define mach_absolute_time fake_absolute_time
#define mach_timebase_info fake_timebase_info
SOURCE_INCLUDE
#undef open
#undef close
#undef ioctl
#undef read
#undef write
#undef mach_absolute_time
#undef mach_timebase_info

static AudioServerPlugInDriverRef plugin;
static const int16_t pcm[12] = { -32768, 32767, -2000, 2000, -1, 1, 0, 0, 101, -101, 1234, -1234 };
static struct { AudioObjectID obj; UInt32 sel, scope; } prop_notes[32];
static unsigned prop_note_count;

static OSStatus fake_props_changed(AudioServerPlugInHostRef h, AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *addrs) {
    (void)h;
    for (UInt32 i = 0; i < n && prop_note_count < 32; i++) {
        prop_notes[prop_note_count].obj = obj;
        prop_notes[prop_note_count].sel = addrs[i].mSelector;
        prop_notes[prop_note_count].scope = addrs[i].mScope;
        prop_note_count++;
    }
    return 0;
}
static const AudioServerPlugInHostInterface fake_host = { .PropertiesChanged = fake_props_changed };

static void setup(void) {
    plugin = AnnibaleAudio_Create(NULL, kAudioServerPlugInTypeUUID);
    assert(plugin != NULL);
    assert((*plugin)->Initialize(plugin, &fake_host) == 0);
}
static void start(AudioObjectID device) {
    assert((*plugin)->StartIO(plugin, device, 1) == 0);
}
static void stop(AudioObjectID device) {
    assert((*plugin)->StopIO(plugin, device, 1) == 0);
}
static UInt64 timestamp(Float64 *sample, UInt64 *host_ticks) {
    UInt64 seed;
    assert((*plugin)->GetZeroTimeStamp(plugin, OBJECT_SPEAKER, 1, sample, host_ticks, &seed) == 0);
    return seed;
}
static OSStatus output(void) {
    return (*plugin)->DoIOOperation(plugin, OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, 1,
        kAudioServerPlugInIOOperationWriteMix, 6, NULL, (void *)pcm, NULL);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "missing_device")) device_available = false;
    setup();
    start(OBJECT_SPEAKER);
    if (!strcmp(argv[1], "write_full")) {
        assert(output() == 0);
        assert(written == 6 && refused == 0 && device_writes == 1);
        assert(last_bytes == sizeof(pcm) && !memcmp(last_samples, pcm, sizeof(pcm)));
    } else if (!strcmp(argv[1], "write_error")) {
        write_mode = 1;
        assert(output() != 0);
        assert(written == 6 && refused == 6 && device_writes == 1);
    } else if (!strcmp(argv[1], "short_write") || !strcmp(argv[1], "unaligned_short_write")) {
        write_mode = !strcmp(argv[1], "short_write") ? 2 : 3;
        assert(output() != 0);
        assert(written == 6 && refused == 4 && device_writes == 1);
    } else if (!strcmp(argv[1], "missing_device")) {
        assert(output() != 0);
        assert(written == 6 && refused == 6 && device_writes == 0);
    } else if (!strcmp(argv[1], "clock_restart")) {
        Float64 sample; UInt64 host_ticks;
        UInt64 first = timestamp(&sample, &host_ticks);
        fake_ticks += 3 * 16384 * 400;
        assert(timestamp(&sample, &host_ticks) == first && sample == 3 * 16384);
        stop(OBJECT_SPEAKER);
        fake_ticks += 12345;
        start(OBJECT_SPEAKER);
        assert(timestamp(&sample, &host_ticks) != first);
        assert(sample == 0 && host_ticks == fake_ticks);
    } else if (!strcmp(argv[1], "shared_clock")) {
        Float64 sample; UInt64 host_ticks;
        UInt64 first = timestamp(&sample, &host_ticks);
        UInt64 origin = host_ticks;
        fake_ticks += 4 * 16384 * 400 + 111;
        start(OBJECT_SPEAKER);
        assert(timestamp(&sample, &host_ticks) == first && sample == 4 * 16384);
        assert(host_ticks == origin + 4 * 16384 * 400);
        stop(OBJECT_SPEAKER);
        assert(timestamp(&sample, &host_ticks) == first && sample == 4 * 16384);
    } else if (!strcmp(argv[1], "speaker_statistics")) {
        assert(output() == 0 && written == 6);
        start(OBJECT_ACTUATOR);
        assert(written == 6 && refused == 0);
        stop(OBJECT_ACTUATOR);
        assert(written == 6 && refused == 0);
    } else if (!strcmp(argv[1], "pcm_format")) {
        AudioObjectPropertyAddress address = { kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, 0 };
        AudioStreamBasicDescription format = {0}; UInt32 size = sizeof(format);
        assert((*plugin)->GetPropertyData(plugin, OBJECT_SPEAKER_OUTPUT, 1, &address, 0, NULL,
            sizeof(format), &size, &format) == 0);
        assert(size == sizeof(format) && format.mSampleRate == 48000);
        assert(format.mFormatID == kAudioFormatLinearPCM);
        assert(format.mFormatFlags == (kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked));
        assert(format.mChannelsPerFrame == 2 && format.mBitsPerChannel == 16);
        assert(format.mBytesPerFrame == 4 && format.mBytesPerPacket == 4 && format.mFramesPerPacket == 1);
    } else if (!strcmp(argv[1], "volume_scope_and_notify")) {
        /* initialize() synced initial full-scale volume (0 dB, unmuted) to kernel */
        assert(ioctl_vol_calls == 1 && last_vol_below == 0 && last_vol_muted == 0);
        AudioObjectPropertyAddress vol_in = { kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeInput, 0 };
        AudioObjectPropertyAddress mute_in = { kAudioDevicePropertyMute, kAudioObjectPropertyScopeInput, 0 };
        AudioObjectPropertyAddress vol_out = { kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, 0 };
        AudioObjectPropertyAddress mute_out = { kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput, 0 };
        Boolean settable = true;
        assert(!(*plugin)->HasProperty(plugin, OBJECT_SPEAKER, 1, &vol_in));
        assert(!(*plugin)->HasProperty(plugin, OBJECT_SPEAKER, 1, &mute_in));
        assert((*plugin)->IsPropertySettable(plugin, OBJECT_SPEAKER, 1, &vol_in, &settable) == kAudioHardwareUnknownPropertyError);
        assert((*plugin)->HasProperty(plugin, OBJECT_SPEAKER, 1, &vol_out));
        assert((*plugin)->IsPropertySettable(plugin, OBJECT_SPEAKER, 1, &vol_out, &settable) == 0 && settable);
        /* Set 0.50 scalar on Output scope -> -24.0 dB (below=192, muted=0) + notifies both volume control and speaker */
        Float32 half = 0.50f;
        prop_note_count = 0;
        assert((*plugin)->SetPropertyData(plugin, OBJECT_SPEAKER, 1, &vol_out, 0, NULL, sizeof(half), &half) == 0);
        assert(ioctl_vol_calls == 2 && last_vol_below == 192 && last_vol_muted == 0);
        assert(prop_note_count == 5);
        assert(prop_notes[0].obj == OBJECT_SPEAKER_VOLUME && prop_notes[0].sel == kAudioLevelControlPropertyScalarValue);
        assert(prop_notes[1].obj == OBJECT_SPEAKER_VOLUME && prop_notes[1].sel == kAudioLevelControlPropertyDecibelValue);
        assert(prop_notes[2].obj == OBJECT_SPEAKER && prop_notes[2].sel == kAudioDevicePropertyVolumeScalar && prop_notes[2].scope == kAudioObjectPropertyScopeOutput);
        assert(prop_notes[3].obj == OBJECT_SPEAKER && prop_notes[3].sel == PROP_VIRTUAL_MASTER_VOL && prop_notes[3].scope == kAudioObjectPropertyScopeOutput);
        assert(prop_notes[4].obj == OBJECT_SPEAKER && prop_notes[4].sel == kAudioDevicePropertyVolumeDecibels && prop_notes[4].scope == kAudioObjectPropertyScopeOutput);
        /* Set mute -> kernel muted=1 + notifies OBJECT_SPEAKER mute in Output scope */
        UInt32 muted = 1;
        prop_note_count = 0;
        assert((*plugin)->SetPropertyData(plugin, OBJECT_SPEAKER, 1, &mute_out, 0, NULL, sizeof(muted), &muted) == 0);
        assert(ioctl_vol_calls == 3 && last_vol_below == 192 && last_vol_muted == 1);
        assert(prop_note_count == 1 && prop_notes[0].obj == OBJECT_SPEAKER && prop_notes[0].sel == kAudioDevicePropertyMute && prop_notes[0].scope == kAudioObjectPropertyScopeOutput);
        /* Unmute and set 0.0 -> muted=1; set 1.0 -> below=0, muted=0 */
        muted = 0;
        assert((*plugin)->SetPropertyData(plugin, OBJECT_SPEAKER, 1, &mute_out, 0, NULL, sizeof(muted), &muted) == 0);
        assert(ioctl_vol_calls == 4 && last_vol_muted == 0);
        Float32 zero = 0.0f, full = 1.0f;
        assert((*plugin)->SetPropertyData(plugin, OBJECT_SPEAKER, 1, &vol_out, 0, NULL, sizeof(zero), &zero) == 0);
        assert(ioctl_vol_calls == 5 && last_vol_below == 384 && last_vol_muted == 1);
        assert((*plugin)->SetPropertyData(plugin, OBJECT_SPEAKER, 1, &vol_out, 0, NULL, sizeof(full), &full) == 0);
        assert(ioctl_vol_calls == 6 && last_vol_below == 0 && last_vol_muted == 0);
    } else return 2;
    stop(OBJECT_SPEAKER);
    puts("audio plugin case passed");
    return 0;
}
'''


class AudioPluginTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = Path(os.environ.get('AUDIO_PLUGIN_TEST_OUT', ROOT / 'out/audio-plugin-test'))
        cls.directory.mkdir(parents=True, exist_ok=True)
        harness = cls.directory / 'audio-plugin-host.c'
        harness.write_text(HARNESS.replace('SOURCE_INCLUDE', '#include ' + json.dumps(str(SOURCE.resolve()))))
        cls.binary = cls.directory / 'audio-plugin-host'
        subprocess.run(['xcrun', '--sdk', 'macosx', 'clang', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(ROOT / 'userland/src/services'), '-framework', 'CoreFoundation',
                        str(harness), '-o', str(cls.binary)], check=True)

    def test_production_interface(self):
        cases = ('pcm_format', 'write_full', 'write_error', 'short_write', 'unaligned_short_write',
                 'missing_device', 'clock_restart', 'shared_clock', 'speaker_statistics',
                 'volume_scope_and_notify')
        for case in cases:
            with self.subTest(case=case):
                result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
