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

static unsigned hal_lookup_polls;
static unsigned hal_devlist_polls;
static bool hal_fail_duid;
static AudioObjectID hal_lookup_target_id;
static OSStatus fake_hal_get_prop(AudioObjectID obj, const AudioObjectPropertyAddress *addr, UInt32 qsize,
    const void *qdata, UInt32 *data_size, void *data) {
    /* Mirror HALS_System.cpp:4511: 'uidd' on system object requires sizeof(CFStringRef) qualifier. */
    if (obj == 1 && addr->mSelector == kAudioPlugInPropertyTranslateUIDToDevice) {
        if (qsize != sizeof(CFStringRef) || qdata == NULL) {
            return kAudioHardwareBadPropertySizeError;
        }
        return kAudioHardwareUnknownPropertyError;
    }
    if (obj == 1 && addr->mSelector == PROP_SYSTEM_TRANSLATE_UID_TO_DEVICE) {
        assert(addr->mScope == kAudioObjectPropertyScopeGlobal);
        assert(qsize == 0 && qdata == NULL);
        assert(*data_size == sizeof(AudioValueTranslation));
        AudioValueTranslation *t = (AudioValueTranslation *)data;
        assert(t->mInputDataSize == sizeof(CFStringRef) && t->mOutputDataSize == sizeof(AudioObjectID));
        assert(CFEqual(*(const CFStringRef *)t->mInputData, CFSTR("VirtualAudioDevice_Actuator")));
        hal_lookup_polls++;
        if (hal_fail_duid) return kAudioHardwareUnknownPropertyError;
        *(AudioObjectID *)t->mOutputData = hal_lookup_polls >= 3 ? hal_lookup_target_id : 0;
        return 0;
    }
    if (obj == 1 && addr->mSelector == PROP_SYSTEM_DEVICES) {
        assert(addr->mScope == kAudioObjectPropertyScopeGlobal);
        hal_devlist_polls++;
        if (hal_devlist_polls < 2) {
            *data_size = 0;
            return 0;
        }
        const AudioObjectID list[4] = { 42, 48, hal_lookup_target_id, 55 };
        assert(*data_size >= sizeof(list));
        memcpy(data, list, sizeof(list));
        *data_size = sizeof(list);
        return 0;
    }
    if (addr->mSelector == kAudioDevicePropertyDeviceUID) {
        assert(*data_size >= sizeof(CFStringRef));
        CFStringRef s = obj == 42 ? CFSTR("Speaker") : obj == 48 ? CFSTR("Hawking") :
                        obj == hal_lookup_target_id ? CFSTR("VirtualAudioDevice_Actuator") : CFSTR("Codec");
        CFRetain(s);
        *(CFStringRef *)data = s;
        *data_size = sizeof(CFStringRef);
        return 0;
    }
    return kAudioHardwareUnknownPropertyError;
}

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
    if (!strcmp(argv[1], "actuator_hal_id_resolve") || !strcmp(argv[1], "actuator_hal_id_fallback_devlist")) {
        hal_lookup_polls = 0;
        hal_devlist_polls = 0;
        hal_fail_duid = !strcmp(argv[1], "actuator_hal_id_fallback_devlist");
        hal_lookup_target_id = 51;
        actuator_hal_get_prop_override = fake_hal_get_prop;
    }
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
    } else if (!strcmp(argv[1], "actuator_vad_properties")) {
        /* TranslateUIDToDevice finds VirtualAudioDevice_Actuator -> OBJECT_ACTUATOR */
        AudioObjectPropertyAddress uidd = { kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, 0 };
        CFStringRef want_uid = CFSTR("VirtualAudioDevice_Actuator");
        AudioObjectID dev_id = 0;
        UInt32 size = sizeof(dev_id);
        assert((*plugin)->GetPropertyData(plugin, OBJECT_PLUGIN, 1, &uidd, sizeof(want_uid), &want_uid, sizeof(dev_id), &size, &dev_id) == 0);
        assert(size == sizeof(dev_id) && dev_id == OBJECT_ACTUATOR);

        /* 'cust' declares 'dacp' ('plst') and 'ptyp' ('ui32') on OBJECT_ACTUATOR */
        AudioObjectPropertyAddress cust = { kAudioObjectPropertyCustomPropertyInfoList, kAudioObjectPropertyScopeGlobal, 0 };
        AudioServerPlugInCustomPropertyInfo infos[4];
        size = sizeof(infos);
        assert((*plugin)->GetPropertyData(plugin, OBJECT_ACTUATOR, 1, &cust, 0, NULL, sizeof(infos), &size, infos) == 0);
        assert(size == 2 * sizeof(AudioServerPlugInCustomPropertyInfo));
        assert(infos[0].mSelector == PROP_ACTUATOR_ACTIVE_PORTS && infos[0].mPropertyDataType == kAudioServerPlugInCustomPropertyDataTypeCFPropertyList);
        assert(infos[1].mSelector == PROP_ACTUATOR_PORT_TYPE && infos[1].mPropertyDataType == kAudioServerPlugInCustomPropertyDataTypeUInt32);

        /* 'dacp' returns CFArray with port AudioObjectID whose 'ptyp' == 'pact' */
        AudioObjectPropertyAddress dacp = { PROP_ACTUATOR_ACTIVE_PORTS, kAudioObjectPropertyScopeGlobal, 0 };
        CFArrayRef ports = NULL;
        size = sizeof(ports);
        assert((*plugin)->GetPropertyData(plugin, OBJECT_ACTUATOR, 1, &dacp, 0, NULL, sizeof(ports), &size, &ports) == 0);
        assert(size == sizeof(ports) && ports != NULL && CFGetTypeID(ports) == CFArrayGetTypeID() && CFArrayGetCount(ports) == 1);
        SInt32 port_id = 0;
        assert(CFNumberGetValue((CFNumberRef)CFArrayGetValueAtIndex(ports, 0), kCFNumberSInt32Type, &port_id));
        CFRelease(ports);
        assert((AudioObjectID)port_id == OBJECT_ACTUATOR);

        AudioObjectPropertyAddress ptyp = { PROP_ACTUATOR_PORT_TYPE, kAudioObjectPropertyScopeGlobal, 0 };
        UInt32 port_type = 0;
        size = sizeof(port_type);
        assert((*plugin)->GetPropertyData(plugin, (AudioObjectID)port_id, 1, &ptyp, 0, NULL, sizeof(port_type), &size, &port_type) == 0);
        assert(size == sizeof(UInt32) && port_type == PORT_TYPE_ACTUATOR);

        /* 'ssrc' / 'ssc#' (Global, Output, Input) and 'mute' / 'vold' (Output) */
        AudioObjectPropertyAddress ssrc_in = { kAudioDevicePropertyDataSource, kAudioObjectPropertyScopeInput, 0 };
        UInt32 src = 0;
        size = sizeof(src);
        assert((*plugin)->GetPropertyData(plugin, OBJECT_ACTUATOR, 1, &ssrc_in, 0, NULL, sizeof(src), &size, &src) == 0);
        assert(size == sizeof(UInt32) && src == PORT_TYPE_ACTUATOR);
        assert((*plugin)->SetPropertyData(plugin, OBJECT_ACTUATOR, 1, &ssrc_in, 0, NULL, sizeof(src), &src) == 0);

        AudioObjectPropertyAddress mute_out = { kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput, 0 };
        AudioObjectPropertyAddress vold_out = { kAudioDevicePropertyVolumeDecibels, kAudioObjectPropertyScopeOutput, 0 };
        UInt32 mval = 99;
        Float32 dval = -1.0f;
        size = sizeof(mval);
        assert((*plugin)->GetPropertyData(plugin, OBJECT_ACTUATOR, 1, &mute_out, 0, NULL, sizeof(mval), &size, &mval) == 0 && mval == 0);
        size = sizeof(dval);
        assert((*plugin)->GetPropertyData(plugin, OBJECT_ACTUATOR, 1, &vold_out, 0, NULL, sizeof(dval), &size, &dval) == 0 && dval == 0.0f);

        /* HAL device properties queried by audiomxd on OBJECT_ACTUATOR */
        const struct { UInt32 sel; UInt32 expect; } u32_props[] = {
            { kAudioDevicePropertyClockAlgorithm, kAudioDeviceClockAlgorithmRaw },
            { kAudioDevicePropertyClockIsStable, 1 },
            { PROP_DEVICE_SISO, 0 },
            { PROP_DEVICE_EXSM, 0 },
            { PROP_DEVICE_DRTE, 0 },
            { PROP_DEVICE_AUTO, 0 },
        };
        for (size_t i = 0; i < sizeof(u32_props) / sizeof(u32_props[0]); i++) {
            AudioObjectPropertyAddress a = { u32_props[i].sel, kAudioObjectPropertyScopeGlobal, 0 };
            UInt32 v = 0xdeadbeefu;
            size = sizeof(v);
            assert((*plugin)->GetPropertyData(plugin, OBJECT_ACTUATOR, 1, &a, 0, NULL, sizeof(v), &size, &v) == 0);
            assert(size == sizeof(UInt32) && v == u32_props[i].expect);
        }
        AudioObjectPropertyAddress accs = { PROP_DEVICE_ACCS, kAudioObjectPropertyScopeGlobal, 0 };
        CFArrayRef accs_arr = NULL;
        size = sizeof(accs_arr);
        assert((*plugin)->GetPropertyData(plugin, OBJECT_ACTUATOR, 1, &accs, 0, NULL, sizeof(accs_arr), &size, &accs_arr) == 0);
        assert(size == sizeof(accs_arr) && accs_arr != NULL && CFGetTypeID(accs_arr) == CFArrayGetTypeID());
        CFRelease(accs_arr);

        /* Speaker remains untouched: does not advertise Actuator's custom properties */
        assert(!(*plugin)->HasProperty(plugin, OBJECT_SPEAKER, 1, &dacp));
        assert(!(*plugin)->HasProperty(plugin, OBJECT_SPEAKER, 1, &ptyp));
    } else if (!strcmp(argv[1], "actuator_hal_id_resolve") || !strcmp(argv[1], "actuator_hal_id_fallback_devlist")) {
        /* Verify old broken combo ('uidd' + qualifierSize=0) is rejected as bad property size */
        AudioObjectPropertyAddress bad_uidd = { kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, 0 };
        AudioValueTranslation dummy = {0};
        UInt32 dummy_sz = sizeof(dummy);
        assert(fake_hal_get_prop(1, &bad_uidd, 0, NULL, &dummy_sz, &dummy) == kAudioHardwareBadPropertySizeError);

        /* Initialize() spawned the one-shot resolver thread returning HAL ID 51 */
        for (unsigned wait = 0; wait < 200; wait++) {
            pthread_mutex_lock(&lock);
            AudioObjectID cur = actuator_hal_id;
            pthread_mutex_unlock(&lock);
            if (cur == 51 && prop_note_count > 0) break;
            usleep(1000);
        }
        if (hal_fail_duid) {
            assert(hal_lookup_polls >= 2 && hal_devlist_polls == 2);
        } else {
            assert(hal_lookup_polls >= 2);
        }
        assert(prop_note_count == 1);
        assert(prop_notes[0].obj == OBJECT_ACTUATOR && prop_notes[0].sel == PROP_ACTUATOR_ACTIVE_PORTS &&
               prop_notes[0].scope == kAudioObjectPropertyScopeGlobal);

        AudioObjectPropertyAddress dacp = { PROP_ACTUATOR_ACTIVE_PORTS, kAudioObjectPropertyScopeGlobal, 0 };
        CFArrayRef ports = NULL;
        UInt32 size = sizeof(ports);
        assert((*plugin)->GetPropertyData(plugin, OBJECT_ACTUATOR, 1, &dacp, 0, NULL, sizeof(ports), &size, &ports) == 0);
        assert(size == sizeof(ports) && ports != NULL && CFArrayGetCount(ports) == 1);
        SInt32 port_id = 0;
        assert(CFNumberGetValue((CFNumberRef)CFArrayGetValueAtIndex(ports, 0), kCFNumberSInt32Type, &port_id));
        CFRelease(ports);
        assert(port_id == 51);
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
                 'volume_scope_and_notify', 'actuator_vad_properties', 'actuator_hal_id_resolve',
                 'actuator_hal_id_fallback_devlist')
        for case in cases:
            with self.subTest(case=case):
                result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
