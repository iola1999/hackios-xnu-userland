#!/usr/bin/env python3
"""Verify native frame boundaries, RAW10 layout, and iOS 26 capture and flashlight consumers."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SERVICES_DIR = ROOT / "userland/src/services"

HARNESS = r"""
#include <assert.h>
#include <Block.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "annibale_camera.h"

/* Stub dlopen/dlsym for CMCaptureCore / CMCaptureDevice symbols on macOS host */
static CFStringRef k_back = CFSTR("PortTypeBackFacingCamera");
static CFStringRef k_front = CFSTR("PortTypeFrontFacingCamera");
static CFStringRef k_md_dict = CFSTR("MetadataDictionary");
static struct { const char *symbol; CFStringRef value; } metadata_symbols[] = {
    { "kFigCaptureStreamMetadata_PortType", CFSTR("PortType") },
    { "kFigCaptureSampleBufferAttachmentKey_OriginalPresentationTimeStamp", CFSTR("PTS") },
    { "kFigCaptureStreamMetadata_ExposureTime", CFSTR("ExposureTime") },
    { "kFigCaptureStreamMetadata_AGC", CFSTR("AGC") },
    { "kFigCaptureStreamMetadata_ispDGain", CFSTR("ispDGain") },
    { "kFigCaptureStreamMetadata_CurrentFrameRate", CFSTR("CurrentFrameRate") },
    { "kFigCaptureStreamMetadata_ZoomFactor", CFSTR("ZoomFactor") },
};

enum { PROFILE_FD = 100, CAMERA_FD = 101, TORCH_FD = 102, PROFILE_BYTES = 6440 };
static uint8_t profile_bytes[PROFILE_BYTES];
static uint8_t *frame_pixels;
static struct annibale_camera_profile fixture;
static struct annibale_camera_capture next_capture;
static struct annibale_camera_frame next_frame;
static size_t profile_offset;
static int profile_present = 1, native_capable = 1, device_open, installed;
static int profile_eintr, short_install, short_frame, capture_errno, hold_after_capture;
static unsigned install_calls, capture_calls, pixel_reads;
static int torch_present, torch_open, short_torch_read, short_torch_write, fail_stream_create;
static uint32_t torch_shown = 173, torch_wanted;
static unsigned torch_reads, torch_writes;

static void init_fixture(void) {
    fixture = (struct annibale_camera_profile) {
        .bytes = PROFILE_BYTES, .sensor = 2, .mode = 4,
        .width = 2592, .height = 1458, .stride = 3248, .bayer = 2, .fps_milli = 30003,
    };
    memcpy(fixture.magic, ANNIBALE_CAMERA_PROFILE_MAGIC, sizeof(fixture.magic));
    for (size_t i = 0; i < sizeof(profile_bytes); i++) profile_bytes[i] = (uint8_t)(37 * i + 11);
    memcpy(profile_bytes, &fixture, sizeof(fixture));
    frame_pixels = calloc(fixture.stride, fixture.height);
    assert(frame_pixels != NULL);
    const uint8_t level200[5] = { 50, 50, 50, 50, 0 };
    for (uint32_t row = 0; row < fixture.height; row++) {
        for (uint32_t x = 0; x < fixture.width / 4; x++)
            memcpy(frame_pixels + (size_t)row * fixture.stride + x * 5, level200, 5);
        memset(frame_pixels + (size_t)row * fixture.stride + fixture.width * 5 / 4, 0xe7, 8);
    }
}

static int fake_open(const char *path, int flags, ...) {
    if (strcmp(path, "/annibale/fw/camera-ov20b-front-mode4.tab") == 0) {
        if (!profile_present) { errno = ENOENT; return -1; }
        assert(flags == O_RDONLY);
        profile_offset = 0;
        profile_eintr = 1;
        return PROFILE_FD;
    }
    if (strcmp(path, ANNIBALE_CAMERA_DEVICE) == 0) {
        assert(flags == O_RDWR && !device_open);
        device_open = 1;
        installed = 0;
        return CAMERA_FD;
    }
    if (strcmp(path, "/dev/annibale-torch") == 0) {
        assert(flags == O_RDWR);
        if (!torch_present) { errno = ENODEV; return -1; }
        assert(!torch_open);
        torch_open = 1;
        return TORCH_FD;
    }
    return open(path, flags);
}

static int fake_fstat(int fd, struct stat *st) {
    if (fd != PROFILE_FD) return fstat(fd, st);
    memset(st, 0, sizeof(*st));
    st->st_size = sizeof(profile_bytes);
    return 0;
}

static ssize_t fake_read(int fd, void *buffer, size_t count) {
    if (fd == PROFILE_FD) {
        if (profile_eintr) { profile_eintr = 0; errno = EINTR; return -1; }
        size_t n = sizeof(profile_bytes) - profile_offset;
        if (n > count) n = count;
        if (n > 127) n = 127;
        memcpy(buffer, profile_bytes + profile_offset, n);
        profile_offset += n;
        return (ssize_t)n;
    }
    if (fd == CAMERA_FD) {
        assert(device_open && installed);
        assert(count == sizeof(next_frame) + (size_t)fixture.stride * fixture.height);
        pixel_reads++;
        memcpy(buffer, &next_frame, sizeof(next_frame));
        memcpy((uint8_t *)buffer + sizeof(next_frame), frame_pixels, count - sizeof(next_frame));
        return (ssize_t)count - short_frame;
    }
    if (fd == TORCH_FD) {
        assert(torch_open && count == sizeof(torch_shown));
        torch_reads++;
        memcpy(buffer, &torch_shown, sizeof(torch_shown));
        return (ssize_t)count - short_torch_read;
    }
    return read(fd, buffer, count);
}

static ssize_t fake_write(int fd, const void *buffer, size_t count) {
    if (fd == TORCH_FD) {
        assert(torch_open && count == sizeof(torch_wanted));
        torch_writes++;
        if (!short_torch_write) memcpy(&torch_wanted, buffer, sizeof(torch_wanted));
        return (ssize_t)count - short_torch_write;
    }
    if (fd != CAMERA_FD) return write(fd, buffer, count);
    const struct annibale_camera_command *command = buffer;
    assert(device_open && count == sizeof(*command) + sizeof(profile_bytes));
    assert(command->command == ANNIBALE_CAMERA_PROFILE && command->bytes == sizeof(profile_bytes));
    assert(command->a == 0 && command->b == 0 && command->c == 0 && command->d == 0);
    assert(memcmp((const uint8_t *)buffer + sizeof(*command), profile_bytes, sizeof(profile_bytes)) == 0);
    install_calls++;
    installed = !short_install;
    return (ssize_t)count - short_install;
}

static int fake_ioctl(int fd, unsigned long request, ...) {
    assert(fd == CAMERA_FD && device_open);
    va_list ap;
    va_start(ap, request);
    void *out = va_arg(ap, void *);
    va_end(ap);
    if (request == CAMIOC_G_INFO) {
        struct annibale_camera_info *info = out;
        memset(info, 0, sizeof(*info));
        info->flags = (native_capable ? ANNIBALE_CAM_INFO_FLAG_FRONT_MODE4 : 0) |
            (installed ? ANNIBALE_CAM_INFO_FLAG_NATIVE_PROFILE : 0);
        return 0;
    }
    assert(request == CAMIOC_CAPTURE && installed);
    capture_calls++;
    if (capture_errno) { errno = capture_errno; return -1; }
    memcpy(out, &next_capture, sizeof(next_capture));
    if (hold_after_capture) next_capture.state = ANNIBALE_CAM_CAPTURE_HELD;
    return 0;
}

static int fake_close(int fd) {
    if (fd == PROFILE_FD) return 0;
    if (fd == CAMERA_FD) { assert(device_open); device_open = installed = 0; return 0; }
    if (fd == TORCH_FD) { assert(torch_open); torch_open = 0; return 0; }
    return close(fd);
}

static OSStatus fake_derived_create(CFAllocatorRef alloc, const void *vtable, const void *class_id, void **out) {
    (void)alloc;
    if (class_id == (const void *)2 && fail_stream_create) { *out = NULL; return -1; }
    const struct { const void *res; const struct { uint64_t ver; uint64_t size; } *base; } *vt = vtable;
    CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, (CFIndex)vt->base->size);
    CFDataSetLength(data, (CFIndex)vt->base->size);
    memset(CFDataGetMutableBytePtr(data), 0, vt->base->size);
    *out = data;
    return 0;
}

static void *fake_derived_storage(void *o) {
    return CFDataGetMutableBytePtr((CFMutableDataRef)o);
}

static const void *fake_device_class_id(void) { return (const void *)1; }
static const void *fake_stream_class_id(void) { return (const void *)2; }

static void *fake_dlsym(void *handle, const char *symbol) {
    (void)handle;
    if (strcmp(symbol, "CMDerivedObjectCreate") == 0) return (void *)fake_derived_create;
    if (strcmp(symbol, "CMBaseObjectGetDerivedStorage") == 0) return (void *)fake_derived_storage;
    if (strcmp(symbol, "FigCaptureDeviceGetClassID") == 0) return (void *)fake_device_class_id;
    if (strcmp(symbol, "FigCaptureStreamGetClassID") == 0) return (void *)fake_stream_class_id;
    if (strcmp(symbol, "kFigCapturePortType_BackFacingCamera") == 0) return &k_back;
    if (strcmp(symbol, "kFigCapturePortType_FrontFacingCamera") == 0) return &k_front;
    if (strcmp(symbol, "kFigCaptureSampleBufferAttachmentKey_MetadataDictionary") == 0) return &k_md_dict;
    for (size_t i = 0; i < sizeof(metadata_symbols) / sizeof(metadata_symbols[0]); i++)
        if (strcmp(symbol, metadata_symbols[i].symbol) == 0) return &metadata_symbols[i].value;
    return NULL;
}

#define dlsym fake_dlsym
#define open fake_open
#define fstat fake_fstat
#define read fake_read
#define write fake_write
#define ioctl fake_ioctl
#define close fake_close
#include "camera_device.c"
#undef dlsym
#undef open
#undef fstat
#undef read
#undef write
#undef ioctl
#undef close

static int64_t get_int(CFDictionaryRef d, CFStringRef k) {
    CFNumberRef n = CFDictionaryGetValue(d, k);
    int64_t v = -1;
    assert(n != NULL && CFGetTypeID(n) == CFNumberGetTypeID());
    CFNumberGetValue(n, kCFNumberSInt64Type, &v);
    return v;
}

static double get_double(CFDictionaryRef d, CFStringRef k) {
    CFNumberRef n = CFDictionaryGetValue(d, k);
    double v = -1;
    assert(n != NULL && CFGetTypeID(n) == CFNumberGetTypeID());
    assert(CFNumberGetValue(n, kCFNumberDoubleType, &v));
    return v;
}

static CMBaseObjectRef stream_for_port(CMBaseObjectRef device, CFStringRef port) {
    CFArrayRef streams = NULL;
    CMBaseObjectRef found = NULL;
    assert(device_copy_property(device, CFSTR("StreamArray"), NULL, &streams) == 0);
    for (CFIndex i = 0; i < CFArrayGetCount(streams); i++) {
        CMBaseObjectRef stream = (CMBaseObjectRef)CFArrayGetValueAtIndex(streams, i);
        CFStringRef value = NULL;
        assert(stream_copy_property(stream, CFSTR("PortType"), NULL, &value) == 0);
        if (CFEqual(value, port)) { assert(found == NULL); found = stream; }
        CFRelease(value);
    }
    CFRelease(streams);
    return found;
}

/* The host CFData objects require explicit production finalizers. */
static void destroy_device(CMBaseObjectRef device) {
    struct device *d = derived_storage(device);
    for (CFIndex i = 0; i < CFArrayGetCount(d->streams); i++)
        stream_finalize((CMBaseObjectRef)CFArrayGetValueAtIndex(d->streams, i));
    device_finalize(device);
    CFRelease(device);
    for (int i = 0; i < MAX_STREAMS; i++) assert(all_streams[i] == NULL);
}

static void flashlight_consumer(CMBaseObjectRef device, CMBaseObjectRef back, CMBaseObjectRef front) {
    CFDictionaryRef properties = NULL;
    assert(stream_copy_property(back, CFSTR("SupportedPropertiesDictionary"), NULL, &properties) == 0);
    assert(CFDictionaryContainsKey(properties, CFSTR("TorchLevel")));
    assert(!CFDictionaryContainsKey(properties, CFSTR("HighCurrentTorchEnabled")));
    assert(!CFDictionaryContainsKey(properties, CFSTR("TorchBeamWidth")));
    assert(!CFDictionaryContainsKey(properties, CFSTR("VideoOutputHandlers")));
    CFRelease(properties);
    CFArrayRef formats = NULL;
    assert(stream_copy_property(back, CFSTR("SupportedFormatsArray"), NULL, &formats) == 0);
    assert(CFArrayGetCount(formats) == 0);
    CFRelease(formats);
    CFArrayRef selected = CFArrayCreate(NULL, (const void **)&back, 1, &kCFTypeArrayCallBacks);
    unsigned captures = capture_calls, reads = pixel_reads, installs = install_calls;
    assert(device_request_streams(device, selected, NULL) == 0);
    assert(!stream_of(back)->worker_started && stream_of(back)->state == CAM_STREAM_STOPPED);
    assert(stream_start(back) == ERR_NOT_AVAILABLE);
    assert(device_relinquish_streams(device, selected, NULL) == 0);
    assert(capture_calls == captures && pixel_reads == reads && install_calls == installs);
    CFRelease(selected);
    CFTypeRef value = NULL;
    assert(stream_copy_property(back, CFSTR("SensorRawOutputBufferPoolRequirements"), NULL, &value) == ERR_NOT_AVAILABLE);
    assert(value == NULL);
    value = number_f(0.37);
    assert(stream_set_property(back, CFSTR("TorchLevel"), value) == 0 && torch_wanted == 370);
    CFRelease(value);
    value = NULL;
    assert(stream_copy_property(back, CFSTR("TorchLevel"), NULL, &value) == 0);
    double level = 0;
    assert(CFNumberGetValue(value, kCFNumberDoubleType, &level) && fabs(level - 0.173) < 1e-9);
    CFRelease(value);

    short_torch_write = 1;
    value = number_f(0.8885);
    assert(stream_set_property(back, CFSTR("TorchLevel"), value) == ERR_NOT_AVAILABLE && torch_wanted == 370);
    short_torch_write = 0;
    CFRelease(value);
    short_torch_read = 1;
    value = NULL;
    assert(stream_copy_property(back, CFSTR("TorchLevel"), NULL, &value) == ERR_NOT_AVAILABLE && value == NULL);
    short_torch_read = 0;
    value = number_f(INFINITY);
    assert(stream_set_property(back, CFSTR("TorchLevel"), value) == ERR_PARAM);
    CFRelease(value);
    assert(stream_set_property(back, CFSTR("TorchLevel"), kCFBooleanTrue) == ERR_PARAM);
    assert(stream_set_property(back, CFSTR("TorchLevel"), NULL) == ERR_PARAM);
    const double input[] = { 0.12345, -0.2, 1.25 };
    const uint32_t expected[] = { 123, 0, 1000 };
    for (size_t i = 0; i < sizeof(input) / sizeof(input[0]); i++) {
        value = number_f(input[i]);
        assert(stream_set_property(back, CFSTR("TorchLevel"), value) == 0 && torch_wanted == expected[i]);
        CFRelease(value);
    }
    if (front != NULL) {
        unsigned writes = torch_writes, queries = torch_reads;
        assert(stream_copy_property(front, CFSTR("SupportedPropertiesDictionary"), NULL, &properties) == 0);
        assert(!CFDictionaryContainsKey(properties, CFSTR("TorchLevel")));
        CFRelease(properties);
        value = number_f(1);
        assert(stream_set_property(front, CFSTR("TorchLevel"), value) == ERR_NOT_AVAILABLE);
        CFRelease(value);
        value = NULL;
        assert(stream_copy_property(front, CFSTR("TorchLevel"), NULL, &value) == ERR_NOT_AVAILABLE && value == NULL);
        assert(torch_writes == writes && torch_reads == queries);
    }
}

static void complete_capture(void) {
    next_capture = (struct annibale_camera_capture) {
        .state = ANNIBALE_CAM_CAPTURE_STOPPED, .flags = ANNIBALE_CAM_CAPTURE_COMPLETE,
        .width = fixture.width, .height = fixture.height, .stride = fixture.stride,
        .bytes = fixture.stride * fixture.height, .sequence = 7, .time = 12345,
    };
    next_frame = (struct annibale_camera_frame) {
        .header_bytes = sizeof(next_frame),
        .width = fixture.width, .height = fixture.height, .stride = fixture.stride,
        .format = 1, .bayer = fixture.bayer, .sensor = fixture.sensor,
        .sequence = next_capture.sequence, .time = next_capture.time, .focus = ANNIBALE_CAMERA_NO_FOCUS,
    };
    memcpy(next_frame.magic, ANNIBALE_CAMERA_MAGIC, sizeof(next_frame.magic));
}

static void native_boundaries(CMBaseObjectRef front) {
    struct annibale_camera_profile profile;
    struct annibale_camera_capture status;
    size_t bytes = sizeof(next_frame) + (size_t)fixture.stride * fixture.height;
    void *buffer = malloc(bytes);
    assert(buffer != NULL);
    unsigned installs = install_calls;
    short_install = 1;
    assert(camera_native_open(2, &profile) < 0 && errno == EIO);
    assert(install_calls == installs + 1 && !device_open);
    short_install = 0;
    int fd = camera_native_open(2, &profile);
    assert(fd == CAMERA_FD && install_calls == installs + 2);
    complete_capture();
    assert(camera_native_frame(fd, &profile, buffer, bytes, &status) == (ssize_t)bytes);
    assert(memcmp((uint8_t *)buffer + sizeof(next_frame), frame_pixels, bytes - sizeof(next_frame)) == 0);

    unsigned reads = pixel_reads;
    for (unsigned bit = 0; bit < 6; bit++) {
        complete_capture();
        next_capture.flags &= ~(1u << bit);
        assert(camera_native_frame(fd, &profile, buffer, bytes, &status) < 0);
        assert(pixel_reads == reads);
    }
    complete_capture();
    next_capture.error = ETIMEDOUT;
    assert(camera_native_frame(fd, &profile, buffer, bytes, &status) < 0 && errno == ETIMEDOUT);
    assert(pixel_reads == reads);
    complete_capture();
    next_capture.state = ANNIBALE_CAM_CAPTURE_HELD;
    assert(camera_native_frame(fd, &profile, buffer, bytes, &status) < 0 && pixel_reads == reads);
    complete_capture();
    next_capture.stride++;
    assert(camera_native_frame(fd, &profile, buffer, bytes, &status) < 0 && pixel_reads == reads);
    complete_capture();
    capture_errno = ENODEV;
    assert(camera_native_frame(fd, &profile, buffer, bytes, &status) < 0 && errno == ENODEV);
    assert(pixel_reads == reads);
    capture_errno = 0;
    short_frame = 1;
    assert(camera_native_frame(fd, &profile, buffer, bytes, &status) < 0 && errno == EIO);
    short_frame = 0;
    complete_capture();
    next_frame.sequence++;
    assert(camera_native_frame(fd, &profile, buffer, bytes, &status) < 0 && errno == EIO);
    complete_capture();
    next_frame.time++;
    assert(camera_native_frame(fd, &profile, buffer, bytes, &status) < 0 && errno == EIO);
    complete_capture();
    next_frame.bayer = 0;
    assert(camera_native_frame(fd, &profile, buffer, bytes, &status) < 0 && errno == EIO);
    assert(fake_close(fd) == 0);

    complete_capture();
    next_capture.state = ANNIBALE_CAM_CAPTURE_HELD;
    unsigned captures = capture_calls;
    reads = pixel_reads;
    stream_of(front)->state = CAM_STREAM_ACTIVE;
    assert(worker(front) == NULL);
    assert(capture_calls == captures + 1 && pixel_reads == reads && !device_open);
    assert(stream_of(front)->state == CAM_STREAM_FAULTED);
    free(buffer);
}

static void metadata_boundaries(void) {
    struct isp isp;
    isp_init(&isp);
    complete_capture();
    assert(isnan(lux_index(&isp, &next_frame)));
    CFDictionaryRef metadata = frame_metadata(&isp, &next_frame, k_front);
    assert(CFDictionaryGetCount(metadata) == 4);
    assert(CFDictionaryContainsKey(metadata, md_key[MD_PORT]));
    assert(CFDictionaryContainsKey(metadata, md_key[MD_PTS]));
    assert(CFDictionaryContainsKey(metadata, md_key[MD_ISP_DGAIN]));
    assert(CFDictionaryContainsKey(metadata, md_key[MD_ZOOM]));
    assert(!CFDictionaryContainsKey(metadata, md_key[MD_EXPOSURE]));
    assert(!CFDictionaryContainsKey(metadata, md_key[MD_AGC]));
    assert(!CFDictionaryContainsKey(metadata, md_key[MD_FRAME_RATE]));
    CFRelease(metadata);
    next_frame.exposure_lines = 100;
    next_frame.line_ns = 10000;
    next_frame.gain = next_frame.gain_unity = 256;
    next_frame.frame_lines = 3333;
    metadata = frame_metadata(&isp, &next_frame, k_front);
    assert(CFDictionaryGetCount(metadata) == 7);
    assert(fabs(get_double(metadata, md_key[MD_EXPOSURE]) - 0.001) < 1e-9);
    assert(get_int(metadata, md_key[MD_AGC]) == 256);
    assert(fabs(get_double(metadata, md_key[MD_FRAME_RATE]) - 1e9 / 33330000) < 1e-9);
    CFRelease(metadata);
    isp_destroy(&isp);
}

static void pixel_buffer_consumer(CMBaseObjectRef front) {
    CVPixelBufferPoolRef pool = NULL;
    CFMutableDictionaryRef attributes = (CFMutableDictionaryRef)new_dict();
    put(attributes, "Width", number_i(1280));
    put(attributes, "Height", number_i(720));
    put(attributes, "PixelFormatType", number_i(PF_BGRA));
    put(attributes, "IOSurfaceProperties", new_dict());
    assert(CVPixelBufferPoolCreate(NULL, NULL, attributes, &pool) == kCVReturnSuccess);
    CFRelease(attributes);
    CFMutableDictionaryRef configs = (CFMutableDictionaryRef)new_dict();
    CFMutableDictionaryRef primary = (CFMutableDictionaryRef)new_dict();
    put(primary, "BufferPool", pool);
    put(configs, "PrimaryScaler", primary);
    assert(stream_set_property(front, CFSTR("VideoOutputConfigurations"), configs) == 0);
    CFRelease(configs);
    __block unsigned callbacks = 0;
    pixel_buffer_handler handler = Block_copy(^(CVPixelBufferRef image, CMTime pts) {
        assert(CVPixelBufferGetWidth(image) == 1280 && CVPixelBufferGetHeight(image) == 720);
        assert(CMTimeCompare(pts, CMClockMakeHostTimeFromSystemUnits(next_frame.time)) == 0);
        assert(CVPixelBufferLockBaseAddress(image, 0) == kCVReturnSuccess);
        const uint8_t *pixels = CVPixelBufferGetBaseAddress(image);
        assert(pixels[0] > 0 && pixels[0] == pixels[1] && pixels[1] == pixels[2] && pixels[3] == 255);
        CVPixelBufferUnlockBaseAddress(image, 0);
        CFDictionaryRef metadata = CVBufferCopyAttachment(image, md_attachment, NULL);
        assert(metadata != NULL && CFDictionaryGetCount(metadata) == 4);
        CFRelease(metadata);
        callbacks++;
    });
    CFMutableDictionaryRef handlers = (CFMutableDictionaryRef)new_dict();
    CFMutableDictionaryRef entry = (CFMutableDictionaryRef)new_dict();
    put(entry, "PixelBufferHandler", handler);
    put(handlers, "PrimaryScaler", entry);
    assert(stream_set_property(front, CFSTR("VideoOutputHandlers"), handlers) == 0);
    CFRelease(handlers);
    complete_capture();
    hold_after_capture = 1;
    unsigned captures = capture_calls, reads = pixel_reads;
    stream_of(front)->state = CAM_STREAM_ACTIVE;
    assert(worker(front) == NULL);
    assert(callbacks == 1 && capture_calls == captures + 2 && pixel_reads == reads + 1);
    assert(stream_of(front)->frames == 1 && stream_of(front)->state == CAM_STREAM_FAULTED);
    hold_after_capture = 0;
    assert(stream_stop(front) == 0);
}

int main(void) {
    init_fixture();
    CMBaseObjectRef dev = NULL;
    profile_present = 0;
    dev = (CMBaseObjectRef)1;
    assert(annibale_camera_device_create(kCFAllocatorDefault, NULL, &dev) == ERR_NOT_AVAILABLE);
    assert(dev == NULL && install_calls == 0);
    profile_present = 1;
    native_capable = 0;
    assert(annibale_camera_device_create(kCFAllocatorDefault, NULL, &dev) == ERR_NOT_AVAILABLE);
    assert(dev == NULL && install_calls == 0 && !device_open);

    profile_present = 0;
    torch_present = 1;
    fail_stream_create = 1;
    assert(annibale_camera_device_create(kCFAllocatorDefault, NULL, &dev) == ERR_NOT_AVAILABLE);
    assert(dev == NULL && !torch_open);
    fail_stream_create = 0;
    assert(annibale_camera_device_create(kCFAllocatorDefault, NULL, &dev) == 0);
    assert(CFArrayGetCount(((struct device *)derived_storage(dev))->streams) == 1);
    CMBaseObjectRef back = stream_for_port(dev, k_back);
    assert(back != NULL && stream_for_port(dev, k_front) == NULL);
    flashlight_consumer(dev, back, NULL);
    unsigned writes = torch_writes;
    uint32_t requested = torch_wanted;
    destroy_device(dev);
    assert(!torch_open && torch_writes == writes && torch_wanted == requested);

    profile_present = 1;
    native_capable = 1;
    assert(annibale_camera_device_create(kCFAllocatorDefault, NULL, &dev) == 0);
    assert(dev != NULL && install_calls == 1 && capture_calls == 0 && !device_open);

    CMBaseObjectRef front = stream_for_port(dev, k_front);
    back = stream_for_port(dev, k_back);
    assert(front != NULL && back != NULL);
    assert(CFArrayGetCount(((struct device *)derived_storage(dev))->streams) == 2);
    assert(stream_of(front)->camera->sensor == 2);
    flashlight_consumer(dev, back, front);

    /* Stage 1: configure: */
    CFArrayRef formats = NULL;
    assert(stream_copy_property(front, CFSTR("SupportedFormatsArray"), kCFAllocatorDefault, &formats) == 0);
    assert(CFArrayGetCount(formats) == 3);
    const int widths[3] = { 2592, 1920, 1280 }, heights[3] = { 1458, 1080, 720 };
    for (int i = 0; i < 3; i++) {
        CFDictionaryRef format = CFArrayGetValueAtIndex(formats, i);
        CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(CFDictionaryGetValue(format, CFSTR("FormatDescription")));
        assert(dimensions.width == widths[i] && dimensions.height == heights[i]);
        assert(get_int(format, CFSTR("SensorWidth")) == 2592 && get_int(format, CFSTR("SensorHeight")) == 1458);
        assert(fabs(get_double(format, CFSTR("VideoMinFrameRate")) - 30.003) < 1e-9);
        assert(fabs(get_double(format, CFSTR("VideoMaxFrameRate")) - 30.003) < 1e-9);
        CFDictionaryRef outputs = CFDictionaryGetValue(format, CFSTR("SupportedOutputs"));
        CFDictionaryRef primary = CFDictionaryGetValue(outputs, CFSTR("PrimaryScaler"));
        CFArrayRef pixel_formats = CFDictionaryGetValue(primary, CFSTR("SupportedPixelFormats"));
        assert(CFArrayGetCount(pixel_formats) == 5);
        for (CFIndex p = 0; p < CFArrayGetCount(pixel_formats); p++) {
            int64_t pixel_format = 0;
            assert(CFNumberGetValue(CFArrayGetValueAtIndex(pixel_formats, p), kCFNumberSInt64Type, &pixel_format));
            assert(pixel_format == PF_420F || pixel_format == PF_420V || pixel_format == PF_X420 ||
                pixel_format == PF_XF20 || pixel_format == PF_BGRA);
        }
    }
    CFRelease(formats);

    CFNumberRef fmt1 = number_i(1);
    assert(stream_set_property(front, CFSTR("FormatIndex"), fmt1) == 0);
    CFRelease(fmt1);

    /* ModuleInfo, SensorRawOutputBufferPoolRequirements, and FaceDetectionConfiguration */
    CFDictionaryRef sup = NULL;
    assert(stream_copy_property(front, CFSTR("SupportedPropertiesDictionary"), kCFAllocatorDefault, &sup) == 0);
    assert(CFDictionaryContainsKey(sup, CFSTR("ModuleInfo")));
    assert(CFDictionaryContainsKey(sup, CFSTR("SensorRawOutputBufferPoolRequirements")));
    assert(CFDictionaryContainsKey(sup, CFSTR("FaceDetectionConfiguration")));
    CFRelease(sup);

    CFDictionaryRef mod_front = NULL;
    assert(stream_copy_property(front, CFSTR("ModuleInfo"), kCFAllocatorDefault, &mod_front) == 0);
    assert(get_int(mod_front, CFSTR("sensorID")) == 0x0914);
    assert(get_int(mod_front, CFSTR("SensorID")) == 0x0914);
    assert(get_int(mod_front, CFSTR("ModuleFocusID")) == 1);
    assert(get_int(mod_front, CFSTR("ModuleActuatorID")) == 1);
    assert(get_int(mod_front, CFSTR("ModuleLensID")) == 1);
    assert(get_int(mod_front, CFSTR("ModuleIntegratorID")) == 1);
    CFRelease(mod_front);

    CFDictionaryRef raw_req = NULL;
    assert(stream_copy_property(front, CFSTR("SensorRawOutputBufferPoolRequirements"), kCFAllocatorDefault, &raw_req) == 0);
    assert(get_int(raw_req, CFSTR("MinimumBufferCount")) == 4);
    assert(get_int(raw_req, CFSTR("Width")) == 2592);
    assert(get_int(raw_req, CFSTR("Height")) == 1458);
    assert(get_int(raw_req, CFSTR("BytesPerRowAlignment")) == 64);
    assert(get_int(raw_req, CFSTR("PlaneAlignment")) == 64);
    assert(get_int(raw_req, CFSTR("PixelFormatType")) == PF_BGRA);
    CFRelease(raw_req);

    CFMutableDictionaryRef fd_cfg = (CFMutableDictionaryRef)new_dict();
    put(fd_cfg, "MaxFaces", number_i(10));
    assert(stream_set_property(front, CFSTR("FaceDetectionConfiguration"), fd_cfg) == 0);
    CFRelease(fd_cfg);
    CFDictionaryRef fd_out = NULL;
    assert(stream_copy_property(front, CFSTR("FaceDetectionConfiguration"), kCFAllocatorDefault, &fd_out) == 0);
    assert(get_int(fd_out, CFSTR("MaxFaces")) == 10);
    CFRelease(fd_out);

    /* Unconditional Depth handler registration in 0x1ae9c79f4 */
    CFMutableDictionaryRef handlers = (CFMutableDictionaryRef)new_dict();
    CFMutableDictionaryRef h_entry = (CFMutableDictionaryRef)new_dict();
    put(h_entry, "PixelBufferHandler", CFRetain(kCFBooleanTrue));
    put(handlers, "PrimaryScaler", CFRetain(h_entry));
    put(handlers, "SecondaryScaler", CFRetain(h_entry));
    put(handlers, "StillImage", CFRetain(h_entry));
    put(handlers, "Depth", h_entry);
    assert(stream_set_property(front, CFSTR("VideoOutputHandlers"), handlers) == 0);
    CFRelease(handlers);

    /* Stage 1 copyProperty:VideoOutputConfigurations before setProperty */
    CFDictionaryRef cfg0 = NULL;
    assert(stream_copy_property(front, CFSTR("VideoOutputConfigurations"), kCFAllocatorDefault, &cfg0) == 0);
    CFDictionaryRef ps0 = CFDictionaryGetValue(cfg0, CFSTR("PrimaryScaler"));
    assert(ps0 != NULL);
    assert(get_int(ps0, CFSTR("BufferPoolBytesPerRowAlignmentRequirement")) == 64);
    assert(get_int(ps0, CFSTR("BufferPoolPlaneAlignmentRequirement")) == 64);
    assert(get_int(ps0, CFSTR("BufferPoolCountForCaptureStream")) == 6);

    CFDictionaryRef meta0 = CFDictionaryGetValue(cfg0, CFSTR("Metadata"));
    assert(meta0 != NULL);
    const char *expected_meta[10] = {
        "LightSourceMaskConfiguration", "KeypointDescriptorDataConfiguration",
        "HumanFullBodiesMaskConfiguration", "HumanHairMaskConfiguration",
        "HumanSkinsMaskConfiguration", "SkyMaskConfiguration",
        "WeightSegmentMapConfiguration", "PreLTMThumbnailConfiguration",
        "LTMThumbnailConfiguration", "PostColorProcessingThumbnailConfiguration"
    };
    for (int i = 0; i < 10; i++) {
        CFStringRef mk = CFStringCreateWithCString(NULL, expected_meta[i], kCFStringEncodingUTF8);
        CFDictionaryRef sub = CFDictionaryGetValue(meta0, mk);
        assert(sub != NULL);
        assert(get_int(sub, CFSTR("PixelFormat")) == PF_L008);
        assert(get_int(sub, CFSTR("BufferPoolCountForCaptureStream")) == 4);
        CFRelease(mk);
    }
    CFRelease(cfg0);

    /* Stage 2a: 0x1aea1e274 sets Width/Height/PixelFormat on VideoOutputConfigurations */
    CFMutableDictionaryRef set1 = (CFMutableDictionaryRef)new_dict();
    CFMutableDictionaryRef ps_set1 = (CFMutableDictionaryRef)new_dict();
    put(ps_set1, "Width", number_i(1920));
    put(ps_set1, "Height", number_i(1080));
    put(ps_set1, "PixelFormat", number_i(PF_420F));
    put(set1, "PrimaryScaler", ps_set1);
    assert(stream_set_property(front, CFSTR("VideoOutputConfigurations"), set1) == 0);
    CFRelease(set1);

    CFDictionaryRef cfg1 = NULL;
    assert(stream_copy_property(front, CFSTR("VideoOutputConfigurations"), kCFAllocatorDefault, &cfg1) == 0);
    CFDictionaryRef ps1 = CFDictionaryGetValue(cfg1, CFSTR("PrimaryScaler"));
    assert(get_int(ps1, CFSTR("Width")) == 1920);
    assert(get_int(ps1, CFSTR("Height")) == 1080);
    assert(get_int(ps1, CFSTR("PixelFormat")) == PF_420F);
    assert(get_int(ps1, CFSTR("BufferPoolCountForCaptureStream")) == 6);
    CFRelease(cfg1);

    /* Stage 2b: allocateOrReuseBufferPoolsFromSourceNode: sets BufferPool and RetainedBufferCount */
    CFMutableDictionaryRef set2 = (CFMutableDictionaryRef)new_dict();
    CFMutableDictionaryRef ps_set2 = (CFMutableDictionaryRef)new_dict();
    put(ps_set2, "BufferPool", CFRetain(CFSTR("FakePool")));
    put(ps_set2, "RetainedBufferCount", number_i(2));
    put(set2, "PrimaryScaler", ps_set2);
    assert(stream_set_property(front, CFSTR("VideoOutputConfigurations"), set2) == 0);
    CFRelease(set2);

    CFDictionaryRef cfg2 = NULL;
    assert(stream_copy_property(front, CFSTR("VideoOutputConfigurations"), kCFAllocatorDefault, &cfg2) == 0);
    CFDictionaryRef ps2 = CFDictionaryGetValue(cfg2, CFSTR("PrimaryScaler"));
    assert(get_int(ps2, CFSTR("Width")) == 1920);
    assert(get_int(ps2, CFSTR("Height")) == 1080);
    assert(get_int(ps2, CFSTR("PixelFormat")) == PF_420F);
    assert(get_int(ps2, CFSTR("RetainedBufferCount")) == 2);
    assert(CFEqual(CFDictionaryGetValue(ps2, CFSTR("BufferPool")), CFSTR("FakePool")));
    CFRelease(cfg2);

    /* Stage 4: start: sets Metadata configuration and VideoOutputsEnabled */
    CFMutableDictionaryRef set3 = (CFMutableDictionaryRef)new_dict();
    CFMutableDictionaryRef meta_set = (CFMutableDictionaryRef)new_dict();
    CFMutableDictionaryRef pre_ltm = (CFMutableDictionaryRef)new_dict();
    put(pre_ltm, "Width", number_i(640));
    put(meta_set, "PreLTMThumbnailConfiguration", pre_ltm);
    put(set3, "Metadata", meta_set);
    assert(stream_set_property(front, CFSTR("VideoOutputConfigurations"), set3) == 0);
    CFRelease(set3);

    CFDictionaryRef cfg3 = NULL;
    assert(stream_copy_property(front, CFSTR("VideoOutputConfigurations"), kCFAllocatorDefault, &cfg3) == 0);
    CFDictionaryRef ps3 = CFDictionaryGetValue(cfg3, CFSTR("PrimaryScaler"));
    assert(get_int(ps3, CFSTR("Width")) == 1920);
    assert(CFEqual(CFDictionaryGetValue(ps3, CFSTR("BufferPool")), CFSTR("FakePool")));
    CFDictionaryRef meta3 = CFDictionaryGetValue(cfg3, CFSTR("Metadata"));
    CFDictionaryRef pre3 = CFDictionaryGetValue(meta3, CFSTR("PreLTMThumbnailConfiguration"));
    assert(get_int(pre3, CFSTR("Width")) == 640);
    assert(get_int(pre3, CFSTR("PixelFormat")) == PF_L008);
    CFRelease(cfg3);

    CFMutableDictionaryRef en = (CFMutableDictionaryRef)new_dict();
    put(en, "PrimaryScaler", CFRetain(kCFBooleanTrue));
    put(en, "Metadata", CFRetain(kCFBooleanTrue));
    assert(stream_set_property(front, CFSTR("VideoOutputsEnabled"), en) == 0);
    CFRelease(en);
    assert(stream_of(front)->out[0].enabled == true);

    native_boundaries(front);
    metadata_boundaries();
    pixel_buffer_consumer(front);
    writes = torch_writes;
    requested = torch_wanted;
    destroy_device(dev);
    assert(!torch_open && torch_writes == writes && torch_wanted == requested);
    free(frame_pixels);
    return 0;
}
"""


ISP_HARNESS = r"""
#include <assert.h>
#include <fenv.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "camera_isp.h"

enum { WIDTH = 2592, HEIGHT = 16, STRIDE = 3248 };

static void raw_layout(void) {
    struct annibale_camera_frame frame = { .width = WIDTH, .height = HEIGHT, .stride = STRIDE, .bayer = 2 };
    struct isp isp;
    struct isp_stats stats;
    uint8_t *raw = malloc(STRIDE * HEIGHT), *bgra = malloc(WIDTH * HEIGHT * 4);
    uint8_t *before = malloc(WIDTH * HEIGHT * 3 / 2), *after = malloc(WIDTH * HEIGHT * 3 / 2);
    assert(raw && bgra && before && after);
    /* MIPI RAW10 groups for GBRG: even G=202/B=701, odd R=341/G=203. */
    const uint8_t groups[2][5] = { { 0x32, 0xaf, 0x32, 0xaf, 0x66 }, { 0x55, 0x32, 0x55, 0x32, 0xdd } };
    for (int row = 0; row < HEIGHT; row++) {
        for (int x = 0; x < WIDTH / 4; x++) memcpy(raw + row * STRIDE + x * 5, groups[row & 1], 5);
        memset(raw + row * STRIDE + WIDTH * 5 / 4, 0xe7, 8);
    }
    isp_init(&isp);
    isp_measure(&isp, &frame, raw, &stats);
    assert(fabs(stats.mean[0] - 341) < 0.001);
    assert(fabs(stats.mean[1] - 202) < 0.001);
    assert(fabs(stats.mean[2] - 701) < 0.001);
    isp_render_bgra(&isp, &frame, raw, bgra, WIDTH * 4, WIDTH, HEIGHT, 0, HEIGHT);
    assert(bgra[0] == isp.gamma[4 * 701] && bgra[1] == isp.gamma[810] && bgra[2] == isp.gamma[4 * 341]);
    isp_render_full_nv12(&isp, &frame, raw, before, WIDTH, before + WIDTH * HEIGHT, WIDTH, 0, HEIGHT, false);
    for (int row = 0; row < HEIGHT; row++) memset(raw + row * STRIDE + WIDTH * 5 / 4, 0, 8);
    isp_render_full_nv12(&isp, &frame, raw, after, WIDTH, after + WIDTH * HEIGHT, WIDTH, 0, HEIGHT, false);
    assert(memcmp(before, after, WIDTH * HEIGHT * 3 / 2) == 0);

    const uint8_t level200[5] = { 50, 50, 50, 50, 0 };
    for (int row = 0; row < HEIGHT; row++)
        for (int x = 0; x < WIDTH / 4; x++) memcpy(raw + row * STRIDE + x * 5, level200, 5);
    isp_measure(&isp, &frame, raw, &stats);
    for (int c = 0; c < 3; c++) assert(fabs(stats.mean[c] - 200) < 0.001);
    frame.black = 64;
    isp_measure(&isp, &frame, raw, &stats);
    for (int c = 0; c < 3; c++) assert(fabs(stats.mean[c] - 136) < 0.001);
    isp_render_bgra(&isp, &frame, raw, bgra, WIDTH * 4, WIDTH, HEIGHT, 0, HEIGHT);
    int index = (int)(136 * 4092.0 / 959 + 0.5);
    assert(bgra[0] == isp.gamma[index] && bgra[1] == isp.gamma[index] && bgra[2] == isp.gamma[index]);
    free(raw); free(bgra); free(before); free(after);
}

static void unknown_measurements(void) {
    struct annibale_camera_frame known = {
        .exposure_lines = 100, .line_ns = 10000, .gain = 256, .gain_unity = 256, .gain_max = 4096,
        .frame_lines_min = 3333, .frame_lines_max = 10000, .margin = 8,
    };
    struct isp_stats stats = { .mean = { 50, 50, 50 }, .bright = 50 };
    for (int unknown = 0; unknown < 5; unknown++) {
        struct annibale_camera_frame frame = known;
        if (unknown == 0) frame = (struct annibale_camera_frame){ 0 };
        if (unknown == 1) frame.exposure_lines = 0;
        if (unknown == 2) frame.line_ns = 0;
        if (unknown == 3) frame.gain = 0;
        if (unknown == 4) frame.gain_unity = 0;
        struct isp isp;
        struct annibale_camera_command command, unchanged;
        isp_init(&isp);
        memset(&command, 0xa5, sizeof(command));
        unchanged = command;
        feclearexcept(FE_ALL_EXCEPT);
        assert(!isp_ae(&isp, &frame, &stats, &command));
        assert(fetestexcept(FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW) == 0);
        assert(memcmp(&command, &unchanged, sizeof(command)) == 0 && isp.digital == 1);
    }
    struct isp isp;
    struct annibale_camera_command command;
    isp_init(&isp);
    assert(isp_ae(&isp, &known, &stats, &command));
    assert(command.command == ANNIBALE_CAMERA_EXPOSURE && command.a > 0 && command.b >= known.gain_unity);
}

int main(int argc, char **argv) {
    if (argc == 1 || strcmp(argv[1], "layout") == 0) raw_layout();
    if (argc == 1 || strcmp(argv[1], "unknown") == 0) unknown_measurements();
    return 0;
}
"""


class CameraDeviceIOS26Test(unittest.TestCase):
    def run_harness(self, source, frameworks=()):
        with tempfile.TemporaryDirectory() as tmp:
            c_path = Path(tmp) / "harness.c"
            bin_path = Path(tmp) / "harness"
            c_path.write_text(source, encoding="utf-8")
            framework_args = [part for name in frameworks for part in ("-framework", name)]
            subprocess.run(
                [
                    "clang",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-fblocks",
                    f"-I{SERVICES_DIR}",
                    str(c_path),
                    str(SERVICES_DIR / "camera_isp.c"),
                    *framework_args,
                    "-o",
                    str(bin_path),
                ],
                check=True,
            )
            subprocess.run([str(bin_path)], check=True)

    def test_four_stage_and_native_contract(self):
        self.run_harness(HARNESS, ("CoreFoundation", "CoreMedia", "CoreVideo", "IOSurface"))

    def test_raw_layout_and_unknown_measurements(self):
        self.run_harness(ISP_HARNESS)


if __name__ == "__main__":
    unittest.main()
