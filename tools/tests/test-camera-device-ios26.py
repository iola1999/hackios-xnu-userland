#!/usr/bin/env python3
"""Verify the iOS 26 BWMultiStreamCameraSourceNode 4-stage contract on camera_device.c."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SERVICES_DIR = ROOT / "userland/src/services"

HARNESS = r"""
#include <assert.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <stdlib.h>
#include <string.h>

/* Stub dlopen/dlsym for CMCaptureCore / CMCaptureDevice symbols on macOS host */
static CFStringRef k_back = CFSTR("PortTypeBackFacingCamera");
static CFStringRef k_front = CFSTR("PortTypeFrontFacingCamera");
static CFStringRef k_front_sw = CFSTR("PortTypeFrontFacingSuperWideCamera");
static CFStringRef k_md_dict = CFSTR("MetadataDictionary");
static CFStringRef k_md_port = CFSTR("PortType");

static OSStatus fake_derived_create(CFAllocatorRef alloc, const void *vtable, const void *class_id, void **out) {
    (void)alloc; (void)class_id;
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
    if (strcmp(symbol, "kFigCapturePortType_FrontFacingSuperWideCamera") == 0) return &k_front_sw;
    if (strcmp(symbol, "kFigCaptureSampleBufferAttachmentKey_MetadataDictionary") == 0) return &k_md_dict;
    return &k_md_port;
}

#define dlsym fake_dlsym
#include "camera_device.c"
#undef dlsym

static int64_t get_int(CFDictionaryRef d, CFStringRef k) {
    CFNumberRef n = CFDictionaryGetValue(d, k);
    int64_t v = -1;
    assert(n != NULL && CFGetTypeID(n) == CFNumberGetTypeID());
    CFNumberGetValue(n, kCFNumberSInt64Type, &v);
    return v;
}

int main(void) {
    CMBaseObjectRef dev = NULL;
    assert(annibale_camera_device_create(kCFAllocatorDefault, NULL, &dev) == 0);
    assert(dev != NULL);

    CMBaseObjectRef back = all_streams[0];
    CMBaseObjectRef front = all_streams[1];
    CMBaseObjectRef front_sw = all_streams[2];
    assert(back != NULL && front != NULL && front_sw != NULL);

    /* Stage 1: configure: */
    CFArrayRef formats = NULL;
    assert(stream_copy_property(back, CFSTR("SupportedFormatsArray"), kCFAllocatorDefault, &formats) == 0);
    assert(CFArrayGetCount(formats) == 13);
    CFRelease(formats);

    CFNumberRef fmt3 = number_i(3);
    assert(stream_set_property(back, CFSTR("FormatIndex"), fmt3) == 0);
    CFRelease(fmt3);

    /* ModuleInfo, SensorRawOutputBufferPoolRequirements, and FaceDetectionConfiguration */
    CFDictionaryRef sup = NULL;
    assert(stream_copy_property(back, CFSTR("SupportedPropertiesDictionary"), kCFAllocatorDefault, &sup) == 0);
    assert(CFDictionaryContainsKey(sup, CFSTR("ModuleInfo")));
    assert(CFDictionaryContainsKey(sup, CFSTR("SensorRawOutputBufferPoolRequirements")));
    assert(CFDictionaryContainsKey(sup, CFSTR("FaceDetectionConfiguration")));
    CFRelease(sup);

    CFDictionaryRef mod_back = NULL, mod_front = NULL;
    assert(stream_copy_property(back, CFSTR("ModuleInfo"), kCFAllocatorDefault, &mod_back) == 0);
    assert(get_int(mod_back, CFSTR("sensorID")) == 0x0904);
    assert(get_int(mod_back, CFSTR("SensorID")) == 0x0904);
    assert(get_int(mod_back, CFSTR("ModuleFocusID")) == 1);
    assert(get_int(mod_back, CFSTR("ModuleActuatorID")) == 1);
    assert(get_int(mod_back, CFSTR("ModuleLensID")) == 1);
    assert(get_int(mod_back, CFSTR("ModuleIntegratorID")) == 1);
    CFRelease(mod_back);

    assert(stream_copy_property(front_sw, CFSTR("ModuleInfo"), kCFAllocatorDefault, &mod_front) == 0);
    assert(get_int(mod_front, CFSTR("sensorID")) == 0x0914);
    assert(get_int(mod_front, CFSTR("SensorID")) == 0x0914);
    CFRelease(mod_front);

    CFDictionaryRef raw_req = NULL;
    assert(stream_copy_property(back, CFSTR("SensorRawOutputBufferPoolRequirements"), kCFAllocatorDefault, &raw_req) == 0);
    assert(get_int(raw_req, CFSTR("MinimumBufferCount")) == 4);
    assert(get_int(raw_req, CFSTR("Width")) == 4224);
    assert(get_int(raw_req, CFSTR("Height")) == 3024);
    assert(get_int(raw_req, CFSTR("BytesPerRowAlignment")) == 64);
    assert(get_int(raw_req, CFSTR("PlaneAlignment")) == 64);
    assert(get_int(raw_req, CFSTR("PixelFormatType")) == PF_BGRA);
    CFRelease(raw_req);

    CFMutableDictionaryRef fd_cfg = (CFMutableDictionaryRef)new_dict();
    put(fd_cfg, "MaxFaces", number_i(10));
    assert(stream_set_property(back, CFSTR("FaceDetectionConfiguration"), fd_cfg) == 0);
    CFRelease(fd_cfg);
    CFDictionaryRef fd_out = NULL;
    assert(stream_copy_property(back, CFSTR("FaceDetectionConfiguration"), kCFAllocatorDefault, &fd_out) == 0);
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
    assert(stream_set_property(back, CFSTR("VideoOutputHandlers"), handlers) == 0);
    CFRelease(handlers);

    /* Stage 1 copyProperty:VideoOutputConfigurations before setProperty */
    CFDictionaryRef cfg0 = NULL;
    assert(stream_copy_property(back, CFSTR("VideoOutputConfigurations"), kCFAllocatorDefault, &cfg0) == 0);
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
    put(ps_set1, "Height", number_i(1440));
    put(ps_set1, "PixelFormat", number_i(PF_420F));
    put(set1, "PrimaryScaler", ps_set1);
    assert(stream_set_property(back, CFSTR("VideoOutputConfigurations"), set1) == 0);
    CFRelease(set1);

    CFDictionaryRef cfg1 = NULL;
    assert(stream_copy_property(back, CFSTR("VideoOutputConfigurations"), kCFAllocatorDefault, &cfg1) == 0);
    CFDictionaryRef ps1 = CFDictionaryGetValue(cfg1, CFSTR("PrimaryScaler"));
    assert(get_int(ps1, CFSTR("Width")) == 1920);
    assert(get_int(ps1, CFSTR("Height")) == 1440);
    assert(get_int(ps1, CFSTR("PixelFormat")) == PF_420F);
    assert(get_int(ps1, CFSTR("BufferPoolCountForCaptureStream")) == 6);
    CFRelease(cfg1);

    /* Stage 2b: allocateOrReuseBufferPoolsFromSourceNode: sets BufferPool and RetainedBufferCount */
    CFMutableDictionaryRef set2 = (CFMutableDictionaryRef)new_dict();
    CFMutableDictionaryRef ps_set2 = (CFMutableDictionaryRef)new_dict();
    put(ps_set2, "BufferPool", CFRetain(CFSTR("FakePool")));
    put(ps_set2, "RetainedBufferCount", number_i(2));
    put(set2, "PrimaryScaler", ps_set2);
    assert(stream_set_property(back, CFSTR("VideoOutputConfigurations"), set2) == 0);
    CFRelease(set2);

    CFDictionaryRef cfg2 = NULL;
    assert(stream_copy_property(back, CFSTR("VideoOutputConfigurations"), kCFAllocatorDefault, &cfg2) == 0);
    CFDictionaryRef ps2 = CFDictionaryGetValue(cfg2, CFSTR("PrimaryScaler"));
    assert(get_int(ps2, CFSTR("Width")) == 1920);
    assert(get_int(ps2, CFSTR("Height")) == 1440);
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
    assert(stream_set_property(back, CFSTR("VideoOutputConfigurations"), set3) == 0);
    CFRelease(set3);

    CFDictionaryRef cfg3 = NULL;
    assert(stream_copy_property(back, CFSTR("VideoOutputConfigurations"), kCFAllocatorDefault, &cfg3) == 0);
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
    assert(stream_set_property(back, CFSTR("VideoOutputsEnabled"), en) == 0);
    CFRelease(en);
    assert(stream_of(back)->out[0].enabled == true);

    return 0;
}
"""


class CameraDeviceIOS26Test(unittest.TestCase):
    def test_four_stage_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            c_path = Path(tmp) / "harness.c"
            bin_path = Path(tmp) / "harness"
            c_path.write_text(HARNESS, encoding="utf-8")
            subprocess.run(
                [
                    "clang",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    f"-I{SERVICES_DIR}",
                    str(c_path),
                    str(SERVICES_DIR / "camera_isp.c"),
                    "-framework",
                    "CoreFoundation",
                    "-framework",
                    "CoreMedia",
                    "-framework",
                    "CoreVideo",
                    "-framework",
                    "IOSurface",
                    "-o",
                    str(bin_path),
                ],
                check=True,
            )
            subprocess.run([str(bin_path)], check=True)


if __name__ == "__main__":
    unittest.main()
