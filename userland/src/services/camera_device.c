/*
 * FigCaptureDevice and FigCaptureStream objects for mediaserverd / cameracaptured's
 * BWFigCaptureDeviceVendor in place of the device Apple's H10ISP/H16ISP.mediacapture makes of the
 * iPhone's ISP (part of libannibale-media.dylib; inject_media.c gives the vendor our device's create
 * function when the kernel has a camera: /dev/annibale-camera).
 *
 * Both are CoreMedia "derived objects" (CMDerivedObjectCreate): a table of functions for the base
 * class (properties) and one for the device or stream class, signed with the exact arm64e PAC
 * discriminators expected by CoreMedia and CMCaptureDevice on iOS 26 (23G90__iPhone18,3):
 *   device  RequestStreams, RelinquishStreams (and a processing session and Pearl calibration, not here)
 *   stream  Start, Stop, AddAttachmentsToSampleBuffer
 * A stream's frames go to the capture graph through the blocks it is given in its property
 * "VideoOutputHandlers" ({ output: { PixelBufferHandler: ^(CVPixelBufferRef, CMTime) } }), for the
 * outputs "VideoOutputConfigurations" describes (the graph's own buffer pools) and "VideoOutputsEnabled"
 * switches on. Native frames require a checked RWCTAB03 profile and CAMIOC_CAPTURE completion before
 * read() copies RAW10 from /dev/annibale-camera. Frames are converted by
 * camera_isp.c (exposure, white balance kept to the sensor's reference lights, the colour matrix and
 * lens shading of the camera's own tuning, gamma, zoom), one pixel buffer for each output, with the
 * metadata from known frame fields and processing values. The native sensor mode supplies the
 * format's geometry and nominal sensor frame rate; the ISP also provides 1920x1080 and 1280x720 scaled outputs.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define CM_PTRAUTH_DATA(disc) __ptrauth(ptrauth_key_process_independent_data, 1, disc)
#define CM_PTRAUTH_FUNC(disc) __ptrauth(ptrauth_key_function_pointer, 1, disc)
#else
#define CM_PTRAUTH_DATA(disc)
#define CM_PTRAUTH_FUNC(disc)
#endif
#include "camera_isp.h"
#include "camera_device_native.inc"

void annibale_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));

void
annibale_camera_say(const char *format, ...)
{
	char line[400];
	va_list ap;
	/* (seconds since boot: the time between two lines is what a step took) */
	int n = snprintf(line, sizeof(line), "annibale: camera: %.3f ", clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9);

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
}
#define say annibale_camera_say

/* ---- CoreMedia's derived objects, with arm64e PAC discriminators for iOS 26 ------------------------- */

typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef const void *CMBaseClassID;

struct base_class {
	uint64_t        version;
	uint64_t        storage_size;
	Boolean         (* CM_PTRAUTH_FUNC(0x0000) equal)(CMBaseObjectRef, CMBaseObjectRef);
	OSStatus        (* CM_PTRAUTH_FUNC(0x3af8) invalidate)(CMBaseObjectRef);
	void            (* CM_PTRAUTH_FUNC(0x5680) finalize)(CMBaseObjectRef);
	CFStringRef     (* CM_PTRAUTH_FUNC(0xf18e) copy_debug_description)(CMBaseObjectRef);
	OSStatus        (* CM_PTRAUTH_FUNC(0x9725) copy_property)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
	OSStatus        (* CM_PTRAUTH_FUNC(0x2724) set_property)(CMBaseObjectRef, CFStringRef, CFTypeRef);
	OSStatus        (* CM_PTRAUTH_FUNC(0xd563) notification_barrier)(CMBaseObjectRef);
	const void *    protocols;
	const void *    reserved;
};
struct device_class {
	uint64_t        version;                /* 3 */
	void *          processing_session_create;
	OSStatus        (* CM_PTRAUTH_FUNC(0x0d64) request_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
	OSStatus        (* CM_PTRAUTH_FUNC(0x8abf) relinquish_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
	void *          pearl_calibration;
};
struct stream_class {
	uint64_t        version;                /* 2 */
	OSStatus        (* CM_PTRAUTH_FUNC(0x0d9f) start)(CMBaseObjectRef);
	OSStatus        (* CM_PTRAUTH_FUNC(0x82f5) stop)(CMBaseObjectRef);
	OSStatus        (* CM_PTRAUTH_FUNC(0xd980) add_attachments)(CMBaseObjectRef, CMSampleBufferRef, CFDictionaryRef);
};
struct device_vtable {
	const void *    reserved;
	const struct base_class * CM_PTRAUTH_DATA(0xb911) base;
	const struct device_class * CM_PTRAUTH_DATA(0xe0cc) derived;
};
struct stream_vtable {
	const void *    reserved;
	const struct base_class * CM_PTRAUTH_DATA(0xb911) base;
	const struct stream_class * CM_PTRAUTH_DATA(0x49c6) derived;
};

#define ERR_PARAM               (-12780)
#define ERR_NOT_AVAILABLE       (-12783)
#define ERR_UNSUPPORTED         (-12782)

static OSStatus (*derived_create)(CFAllocatorRef, const void *, CMBaseClassID, CMBaseObjectRef *);
static void *(*derived_storage)(CMBaseObjectRef);
static CMBaseClassID (*device_class_id)(void);
static CMBaseClassID (*stream_class_id)(void);
static CFStringRef port_back;           /* kFigCapturePortType_BackFacingCamera */
static CFStringRef port_front;          /* kFigCapturePortType_FrontFacingCamera */

/*
 * The frames' metadata: what H10ISP/H16ISP's stream attaches to every frame (a dictionary under
 * kFigCaptureSampleBufferAttachmentKey_MetadataDictionary, through the stream's
 * AddAttachmentsToSampleBuffer when the graph defers it), and what the capture device keeps its frame
 * statistics of.
 */
enum { MD_PORT, MD_PTS, MD_EXPOSURE, MD_AGC, MD_ISP_DGAIN, MD_FRAME_RATE, MD_ZOOM, MD_AFSTATUS, MD_AFLOCKED,
    MD_FOCUSPOS, MD_FOCUSCMDID, MD_AFSTABLE, MD_FOCUSPEAKS, MD_KEYS };
static const char *const md_names[MD_KEYS] = {
	"kFigCaptureStreamMetadata_PortType", "kFigCaptureSampleBufferAttachmentKey_OriginalPresentationTimeStamp",
	"kFigCaptureStreamMetadata_ExposureTime", "kFigCaptureStreamMetadata_AGC", "kFigCaptureStreamMetadata_ispDGain",
	"kFigCaptureStreamMetadata_CurrentFrameRate", "kFigCaptureStreamMetadata_ZoomFactor",
	"kFigCaptureStreamMetadata_AFStatus", "kFigCaptureStreamMetadata_AFLocked",
	"kFigCaptureStreamMetadata_CurrentFocusPosition", "kFigCaptureStreamMetadata_FocusNowCommandID",
	"kFigCaptureStreamMetadata_AFStable", "kFigCaptureStreamMetadata_FocusPeakSumArray",
};
static CFStringRef md_key[MD_KEYS];
static CFStringRef md_attachment;       /* kFigCaptureSampleBufferAttachmentKey_MetadataDictionary */
static CFArrayRef md_focus_peaks;       /* the zeros every frame's FocusPeakSumArray is (find_functions()); never released */

static bool
find_functions(void)
{
	CFStringRef *port;

	/* On iOS 26, FigCaptureDeviceGetClassID / FigCaptureStreamGetClassID live in CMCaptureDevice
	 * and kFigCapturePortType_* live in CMCaptureCore. */
	(void)dlopen("/System/Library/PrivateFrameworks/CMCaptureCore.framework/CMCaptureCore", RTLD_LAZY | RTLD_GLOBAL);
	(void)dlopen("/System/Library/PrivateFrameworks/CMCaptureDevice.framework/CMCaptureDevice", RTLD_LAZY | RTLD_GLOBAL);

	derived_create = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
	derived_storage = dlsym(RTLD_DEFAULT, "CMBaseObjectGetDerivedStorage");
	device_class_id = dlsym(RTLD_DEFAULT, "FigCaptureDeviceGetClassID");
	stream_class_id = dlsym(RTLD_DEFAULT, "FigCaptureStreamGetClassID");
	port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_BackFacingCamera");
	port_back = port != NULL ? *port : NULL;
	/* On iPhone18,3 (V57), Front Ultra Wide and Front Virtual bind PortTypeFrontSuperWide. */
	port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_FrontFacingSuperWideCamera");
	if (port == NULL || *port == NULL) {
		port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_FrontFacingCamera");
	}
	port_front = port != NULL ? *port : NULL;
	port = dlsym(RTLD_DEFAULT, "kFigCaptureSampleBufferAttachmentKey_MetadataDictionary");
	md_attachment = port != NULL ? *port : NULL;
	for (int i = 0; i < MD_KEYS; i++) {
		port = dlsym(RTLD_DEFAULT, md_names[i]);
		md_key[i] = port != NULL ? *port : NULL;
		if (md_key[i] == NULL) {
			say("%s is not there: the frames' metadata go without it", md_names[i]);
		}
	}
	/*
	 * FocusPeakSumArray's content: the peak sums of the focus windows, up to 16 of them read as
	 * SInt64. The autofocus processor skips its whole FocusNow command confirmation without the
	 * key, and a photograph's reference-frame choice wants it too (focuspeaksem-array.md). The
	 * driver has no focus statistics to fill them with -- the frames come up as RAW, and which
	 * window each element means is not reversed -- so they are zeros: constant from frame to frame,
	 * where one measured sharpness copied into every window would tell the continuous focus'
	 * retrigger statistics a made-up uniform scene (§8). The plugin's own watch of the scene
	 * searches again without waiting for that retrigger.
	 */
	if (md_focus_peaks == NULL) {
		int64_t zero = 0;
		CFNumberRef z = CFNumberCreate(NULL, kCFNumberSInt64Type, &zero);

		if (z != NULL) {
			const void *peaks[16];

			for (size_t i = 0; i < sizeof(peaks) / sizeof(peaks[0]); i++) {
				peaks[i] = z;
			}
			md_focus_peaks = CFArrayCreate(NULL, peaks, (CFIndex)(sizeof(peaks) / sizeof(peaks[0])), &kCFTypeArrayCallBacks);
			CFRelease(z);
		}
	}
	if (md_attachment == NULL || md_key[MD_PORT] == NULL) {
		say("kFigCaptureSampleBufferAttachmentKey_MetadataDictionary or the port's key is not there: frames without "
		    "metadata, and the shutter will wait for ever");
		md_attachment = NULL;
	}
	if (derived_create == NULL || derived_storage == NULL || device_class_id == NULL || stream_class_id == NULL || port_back == NULL ||
	    port_front == NULL) {
		say("CoreMedia's or CMCapture's functions are not there: no camera");
		return false;
	}
	return true;
}

/* ---- what is asked and not answered: said once per name ------------------------------------------- */

static CFMutableSetRef said_names;
static pthread_mutex_t said_lock = PTHREAD_MUTEX_INITIALIZER;

static void
say_value(const char *what, CFStringRef key, CFTypeRef value)
{
	char name[160] = "?", text[320] = "";

	CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
	if (value != NULL) {
		CFStringRef d = CFCopyDescription(value);
		CFIndex used = 0;

		if (d != NULL) {
			CFStringGetBytes(d, CFRangeMake(0, CFStringGetLength(d)), kCFStringEncodingUTF8,
			    '?', false, (UInt8 *)text, sizeof(text) - 1, &used);
			text[used] = '\0';
			CFRelease(d);
		}
		for (char *c = text; *c; c++) {
			*c = (*c == '\n' || *c == '\t') ? ' ' : *c;
		}
	}
	say("%s %s%s%s", what, name, value ? " = " : "", text);
}

/* true the first time (the value said), false after */
static bool
say_once(const char *what, CFStringRef key, CFTypeRef value)
{
	CFStringRef tag = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s %@"), what, key);

	pthread_mutex_lock(&said_lock);
	if (said_names == NULL) {
		said_names = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	bool first = !CFSetContainsValue(said_names, tag);

	CFSetAddValue(said_names, tag);
	pthread_mutex_unlock(&said_lock);
	CFRelease(tag);
	if (first) {
		say_value(what, key, value);
	}
	return first;
}

/* ---- numbers and dictionaries ---------------------------------------------------------------------- */

static CFNumberRef
number_i(int64_t v)
{
	return CFNumberCreate(NULL, kCFNumberSInt64Type, &v);
}

static CFNumberRef
number_i32(int32_t v)
{
	return CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
}

static CFNumberRef
number_f(double v)
{
	return CFNumberCreate(NULL, kCFNumberDoubleType, &v);
}

static void
put(CFMutableDictionaryRef d, const char *key, CFTypeRef value)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);

	CFDictionarySetValue(d, k, value);
	CFRelease(k);
	CFRelease(value);
}

static CFDictionaryRef
new_dict(void)
{
	return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static bool
key_is(CFStringRef key, const char *name)
{
	char buffer[128];

	return CFStringGetCString(key, buffer, sizeof(buffer), kCFStringEncodingUTF8) && strcmp(buffer, name) == 0;
}

/* ---- what the device and the stream say they have ---------------------------------------------------- */

struct prop {
	const char *name, *type, *access;
};

static const struct prop stream_props[] = {
	{ "PortType", "String", "ReadOnly" }, { "UniqueID", "String", "ReadOnly" }, { "NonLocalizedName", "String", "ReadOnly" },
	{ "ModuleInfo", "Collection", "ReadOnly" }, { "SensorRawOutputBufferPoolRequirements", "Collection", "ReadOnly" },
	{ "SupportedFormatsArray", "Collection", "ReadOnly" },
	{ "MaxZoomFactor", "Number", "ReadOnly" }, { "SupportedColorSpaces", "Collection", "ReadOnly" },
	{ "HasAutoFocusPositionSensor", "Boolean", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" },
	{ "PixelSize", "Number", "ReadOnly" }, { "FormatIndex", "Number", "ReadWrite" }, { "MinimumFrameRate", "Number", "ReadWrite" }, { "MaximumFrameRate", "Number", "ReadWrite" },
	{ "MaximumAllowedFrameRate", "Number", "ReadWrite" }, { "VideoOutputConfigurations", "Collection", "ReadWrite" },
	{ "VideoOutputsEnabled", "Collection", "ReadWrite" }, { "VideoOutputHandlers", "Collection", "WriteOnly" },
	{ "DeferAdditionOfAttachments", "Boolean", "ReadWrite" },
	{ "ColorSpace", "Number", "ReadWrite" }, { "ZoomFactor", "Number", "ReadWrite" },
	{ "ExposureBias", "Number", "ReadWrite" }, { "DeviceOrientation", "Number", "ReadWrite" }, { "FlashMode", "String", "ReadWrite" },
	{ "StillImageCaptureNow", "Collection", "WriteOnly" }, { "LockAENow", "Boolean", "WriteOnly" }, { "UnlockAENow", "Boolean", "WriteOnly" },
	{ "LockAWBNow", "Boolean", "WriteOnly" }, { "UnlockAWBNow", "Boolean", "WriteOnly" }, { "MaxIntegrationTime", "Number", "ReadWrite" },
	{ "AEMaxGain", "Number", "ReadWrite" }, { "AEMinGain", "Number", "ReadWrite" }, { "FaceDetectionEnabled", "Boolean", "ReadWrite" },
	{ "FaceDetectionConfiguration", "Collection", "ReadWrite" },
	{ "VideoRecordingInProgress", "Boolean", "ReadWrite" }, { "MemoryPoolID", "Number", "ReadWrite" },
	{ "ChromaNoiseReductionEnabled", "Boolean", "ReadWrite" }, { "TemporalNoiseReductionEnabled", "Boolean", "ReadWrite" },
	{ "MultipleOutputSupportEnabled", "Boolean", "ReadWrite" }, { "VideoOutputsColorInfo", "Collection", "ReadOnly" },
	{ "GeometricDistortionCorrectedRects", "Collection", "ReadOnly" },
	{ "RegionOfInterestForCameraControls", "Collection", "ReadWrite" }, { "AEMeteringMode", "Number", "ReadWrite" },
	{ "MaxFrameCoalescingDuration", "Number", "ReadWrite" }, { "LimitAERange", "Boolean", "ReadWrite" },
	{ "TimeMachineSuspendNow", "Boolean", "WriteOnly" }, { "TimeMachineResumeNow", "Boolean", "WriteOnly" },
	/* focus (the wide camera's lens motor; see af_frame()) */
	{ "AutoFocusNow", "Collection", "WriteOnly" }, { "ContinuousAutoFocusNow", "Collection", "WriteOnly" },
	{ "FocusNow", "Collection", "WriteOnly" }, { "PeakTrackingFocusNow", "Collection", "WriteOnly" },
	{ "LockAFNow", "Boolean", "WriteOnly" }, { "ManualFocusNow", "Collection", "WriteOnly" },
	{ "AFWindowParams", "Collection", "ReadWrite" }, { "AutoFocusMode", "Number", "ReadWrite" },
	{ "AFPeakPredictionEnabled", "Boolean", "ReadWrite" }, { "AFSearchPositions", "Collection", "ReadWrite" },
	{ "AFEarlyOutParams", "Collection", "ReadWrite" },
}, device_props[] = {
	{ "StreamArray", "Collection", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" }, { "ClientIsActive", "Boolean", "ReadWrite" },
	{ "SupportedSynchronizedStreamsGroups", "Collection", "ReadOnly" }, { "MemoryPoolID", "Number", "ReadWrite" },
}, torch_props[] = {
	{ "PortType", "String", "ReadOnly" }, { "UniqueID", "String", "ReadOnly" }, { "NonLocalizedName", "String", "ReadOnly" },
	{ "ModuleInfo", "Collection", "ReadOnly" }, { "SupportedFormatsArray", "Collection", "ReadOnly" },
	{ "Clock", "Clock", "ReadOnly" }, { "TorchLevel", "Number", "ReadWrite" },
};

static CFDictionaryRef
supported(const struct prop *p, size_t n)
{
	CFMutableDictionaryRef all = (CFMutableDictionaryRef)new_dict();

	for (size_t i = 0; i < n; i++) {
		CFMutableDictionaryRef one = (CFMutableDictionaryRef)new_dict();

		put(one, "PropertyType", CFStringCreateWithCString(NULL, p[i].type, kCFStringEncodingUTF8));
		put(one, "ReadWriteStatus", CFStringCreateWithCString(NULL, p[i].access, kCFStringEncodingUTF8));
		put(all, p[i].name, one);
	}
	return all;
}

/* ---- the formats ----------------------------------------------------------------------------------- */

#include "camera_formats_ios26.inc"

/* ---- the stream ------------------------------------------------------------------------------------- */

#define HELD    3               /* the moving pictures kept from the pool after they are given (see deliver) */

struct output {
	const char *    name;
	CFDictionaryRef config;                 /* from VideoOutputConfigurations */
	bool            enabled;
	void *          handler;                /* a block: ^(CVPixelBufferRef, CMTime) */
	bool            said_shape;
	uint64_t        delivered, refused;
	CVPixelBufferRef held[HELD];            /* the pictures given last, kept from the pool (see deliver) */
	unsigned        held_next;
	bool            said_pool;
};

static void let_go(struct output *out);

#define OUTPUTS 6
static const char *const output_names[OUTPUTS] = {
	"PrimaryScaler", "SecondaryScaler", "StillImage",
	"StillImageQuadra", "StillImageRaw", "Depth"
};
#define PICTURE_OUTPUTS 4               /* the first four: YCbCr pictures, of colour_attachments() */

/* sRGB as CMCapture tags it (AVCaptureColorSpace_sRGB): BT.709 primaries and transfer, BT.601 matrix */
static CFDictionaryRef
colour_attachments(void)
{
	static CFDictionaryRef attachments;
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		const void *keys[] = { kCVImageBufferColorPrimariesKey, kCVImageBufferYCbCrMatrixKey,
		    kCVImageBufferTransferFunctionKey };
		const void *values[] = { kCVImageBufferColorPrimaries_ITU_R_709_2, kCVImageBufferYCbCrMatrix_ITU_R_601_4,
		    kCVImageBufferTransferFunction_ITU_R_709_2 };

		attachments = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
	});
	return attachments;
}

#define MAX_ZOOM                16      /* (the most the iPhone's wide camera zooms digitally) */
/*
 * Physical identities are independent of a mode's native-capture support. The optics come from the
 * two sensor modules whose profiles the kernel accepts (FRONT_SOURCE_SHA256 and BACK_SOURCE_SHA256 in
 * tools/camsensor_profile.py): field_of_view is lensInformation.horizontalViewAngle in degrees, which
 * CMCapture copies into every resolved format and aborts without; pixel_size is the pixel pitch in
 * micrometres, which a capture stream must report between 0 and 100 or the graph is not built.
 */
static const struct camera {
	uint32_t        sensor;
	const char      *name, *unique_id;
	double          field_of_view, pixel_size;
} cameras[2] = {
	{ 1, "Back Camera", "com.apple.avfoundation.avcapturedevice.built-in_video:0", 69.4, 1.008 },
	{ 2, "Front Camera", "com.apple.avfoundation.avcapturedevice.built-in_video:1", 77.3, 0.702 },
};

enum camera_stream_state {
	CAM_STREAM_STOPPED = 0,
	CAM_STREAM_ACTIVE  = 2,
	CAM_STREAM_FAULTED = 3,
};

/* how a focus search goes, as CMCapture asks (AFSearchPositions, AFEarlyOutParams, AFPeakPredictionEnabled) */
#define AF_POSITIONS    16
struct af_search {
	unsigned        count;
	uint8_t         position[AF_POSITIONS];         /* 0 to 255 over the lens's travel, ascending */
	bool            early_out;      /* stop once ... */
	unsigned        early_out_count;        /* ... this many positions in a row are ... */
	float           early_out_drop;         /* ... this much less sharp than the sharpest (AFEarlyOutDecrementAmount
	                                         * read as 256ths: an assumption, its unit not traced in CMCapture) */
	bool            early_out_at_start;     /* ... even when the sharpest is the first */
	bool            peak_prediction;
};

struct stream {
	CFArrayRef      formats;
	CFDictionaryRef supported;
	CFDictionaryRef face_config;    /* from FaceDetectionConfiguration */
	CFDictionaryRef meta_config;    /* from VideoOutputConfigurations["Metadata"] */
	CFDictionaryRef metadata;       /* the last frame's (frame_metadata()), for a buffer that has none of its own */
	pthread_mutex_t metadata_lock;  /* (not `lock': AddAttachmentsToSampleBuffer may come while a frame is given out) */
	unsigned        stills_wanted;  /* photographs asked for by StillImageCaptureNow, not yet given */
	int             format_index;
	double          min_fps, max_fps;
	float           zoom;           /* ZoomFactor: the picture is the middle 1/zoom of the sensor each way */
	/*
	 * ExposureBias: the exposure the user asked for over AE's, 2^EV in 1/256ths, 1 to 65535 -- as
	 * CMCapture's sender (0x1aea2eb08) makes it of the EV, llroundf(exp2f(EV) * 256): 256 none.
	 */
	uint32_t        exposure_bias, said_bias;
	int64_t         device_orientation;     /* the last DeviceOrientation set (0: none yet) */
	int             exif_mode;      /* the rigging's: what the photographs' ExifOrientation is (camera_device_orient.inc) */
	struct af_search af_search;
	const struct camera *camera;
	struct annibale_camera_profile profile;
	CFStringRef     port;
	bool            af_now;         /* a focus asked for (AutoFocusNow and the like): a search at the next frame */
	bool            af_locked;      /* LockAFNow: no search but one asked for */
	int64_t         focus_cmdid;    /* mailbox: the command id a FocusNow collection carried, until the worker takes it */
	float           af_window[4];   /* AFWindowParams: where to focus, as fractions of the frame (x0, y0, x1, y1) */
	struct output   out[OUTPUTS];
	pthread_mutex_t lock;
	pthread_t       worker;
	dispatch_queue_t still_queue;     /* the photographs' renders, off the worker (still_enqueue/still_run) */
	volatile int    state;
	bool            worker_started;
	int             torch;
	int             flash_mode;     /* 0 ForceFlashOff, 1 ForceFlashOn, 2 Auto */
	bool            still_flash_wanted;
	bool            still_redeye_wanted;
	bool            flash_lit;
	float           last_lux, last_cct;
	uint64_t        frames, stills_given;
};

static struct stream *
stream_of(CMBaseObjectRef o)
{
	return derived_storage(o);
}

static int
output_index(CFStringRef name)
{
	for (int i = 0; i < OUTPUTS; i++) {
		if (key_is(name, output_names[i])) {
			return i;
		}
	}
	return -1;
}

static CFDictionaryRef stream_module_info(const struct stream *s);
static CFDictionaryRef stream_raw_pool_requirements(const struct stream *s);
static CFDictionaryRef stream_gdc_rects(const struct stream *s);

static OSStatus
stream_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
	struct stream *s = stream_of(o);
	CFTypeRef value = NULL;

	(void)allocator;
	if (s->profile.bytes == 0 && !key_is(key, "SupportedPropertiesDictionary") &&
	    !CFDictionaryContainsKey(s->supported, key)) {
		return ERR_NOT_AVAILABLE;
	}
	if (key_is(key, "PortType")) {
		value = CFRetain(s->port);
	} else if (key_is(key, "ModuleInfo")) {
		value = stream_module_info(s);
	} else if (key_is(key, "SensorRawOutputBufferPoolRequirements")) {
		pthread_mutex_lock(&s->lock);
		value = stream_raw_pool_requirements(s);
		pthread_mutex_unlock(&s->lock);
	} else if (key_is(key, "FaceDetectionConfiguration")) {
		pthread_mutex_lock(&s->lock);
		value = s->face_config != NULL ? CFRetain(s->face_config) : new_dict();
		pthread_mutex_unlock(&s->lock);
	} else if (key_is(key, "UniqueID") || key_is(key, "ModelID")) {
		value = CFStringCreateWithCString(NULL, s->camera->unique_id, kCFStringEncodingUTF8);
	} else if (key_is(key, "NonLocalizedName") || key_is(key, "LocalizedName")) {
		value = CFStringCreateWithCString(NULL, s->camera->name, kCFStringEncodingUTF8);
	} else if (key_is(key, "Manufacturer")) {
		value = CFRetain(CFSTR("Apple Inc."));
	} else if (key_is(key, "SupportedFormatsArray")) {
		value = CFRetain(s->formats);
	} else if (key_is(key, "PixelSize")) {
		value = number_f(s->camera->pixel_size);
	} else if (key_is(key, "FormatIndex")) {
		value = number_i(s->format_index);
	} else if (key_is(key, "MinimumFrameRate")) {
		value = number_f(s->min_fps);
	} else if (key_is(key, "MaximumFrameRate")) {
		value = number_f(s->max_fps);
	} else if (key_is(key, "Clock")) {
		value = CFRetain(CMClockGetHostTimeClock());
	} else if (key_is(key, "TorchLevel")) {
		uint32_t level;

		if (s->torch < 0 || read(s->torch, &level, sizeof(level)) != (ssize_t)sizeof(level)) {
			return ERR_NOT_AVAILABLE;
		}
		value = number_f(level / 1000.0);
	} else if (key_is(key, "ExposureBias")) {
		pthread_mutex_lock(&s->lock);
		value = number_i32((int32_t)s->exposure_bias);
		pthread_mutex_unlock(&s->lock);
	} else if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled")) {
		bool configs = key_is(key, "VideoOutputConfigurations");
		CFMutableDictionaryRef d = (CFMutableDictionaryRef)new_dict();

		pthread_mutex_lock(&s->lock);
		for (int i = 0; i < OUTPUTS; i++) {
			CFStringRef name = CFStringCreateWithCString(NULL, output_names[i], kCFStringEncodingUTF8);

			if (configs) {
				CFMutableDictionaryRef oc = s->out[i].config != NULL ?
				    CFDictionaryCreateMutableCopy(NULL, 0, s->out[i].config) :
				    (CFMutableDictionaryRef)new_dict();

				if (!CFDictionaryContainsKey(oc, CFSTR("BufferPoolBytesPerRowAlignmentRequirement"))) {
					put(oc, "BufferPoolBytesPerRowAlignmentRequirement", number_i(64));
				}
				if (!CFDictionaryContainsKey(oc, CFSTR("BufferPoolPlaneAlignmentRequirement"))) {
					put(oc, "BufferPoolPlaneAlignmentRequirement", number_i(64));
				}
				if (!CFDictionaryContainsKey(oc, CFSTR("BufferPoolCountForCaptureStream"))) {
					put(oc, "BufferPoolCountForCaptureStream", number_i(6));
				}
				CFDictionarySetValue(d, name, oc);
				CFRelease(oc);
			} else {
				CFDictionarySetValue(d, name, s->out[i].enabled ? kCFBooleanTrue : kCFBooleanFalse);
			}
			CFRelease(name);
		}
		if (configs) {
			static const char *const mask_keys[10] = {
				"LightSourceMaskConfiguration", "KeypointDescriptorDataConfiguration",
				"HumanFullBodiesMaskConfiguration", "HumanHairMaskConfiguration",
				"HumanSkinsMaskConfiguration", "SkyMaskConfiguration",
				"WeightSegmentMapConfiguration", "PreLTMThumbnailConfiguration",
				"LTMThumbnailConfiguration", "PostColorProcessingThumbnailConfiguration"
			};
			CFMutableDictionaryRef meta = (CFMutableDictionaryRef)new_dict();

			for (int k = 0; k < 10; k++) {
				CFMutableDictionaryRef mc = (CFMutableDictionaryRef)new_dict();

				put(mc, "Width", number_i(320));
				put(mc, "Height", number_i(240));
				put(mc, "PixelFormat", number_i(PF_L008));
				put(mc, "DataBufferSize", number_i(4096));
				put(mc, "BufferPoolBytesPerRowAlignmentRequirement", number_i(64));
				put(mc, "BufferPoolPlaneAlignmentRequirement", number_i(64));
				put(mc, "BufferPoolCountForCaptureStream", number_i(4));
				if (s->meta_config != NULL) {
					CFStringRef mk = CFStringCreateWithCString(NULL, mask_keys[k], kCFStringEncodingUTF8);
					CFTypeRef sub = CFDictionaryGetValue(s->meta_config, mk);

					if (sub != NULL && CFGetTypeID(sub) == CFDictionaryGetTypeID()) {
						CFIndex sn = CFDictionaryGetCount(sub);
						const void **skeys = calloc((size_t)sn + 1, sizeof(void *));
						const void **svals = calloc((size_t)sn + 1, sizeof(void *));

						CFDictionaryGetKeysAndValues(sub, skeys, svals);
						for (CFIndex si = 0; si < sn; si++) {
							CFDictionarySetValue(mc, skeys[si], svals[si]);
						}
						free(skeys);
						free(svals);
					}
					CFRelease(mk);
				}
				put(meta, mask_keys[k], mc);
			}
			put(d, "Metadata", meta);
		}
		pthread_mutex_unlock(&s->lock);
		value = d;
	} else if (key_is(key, "VideoOutputsColorInfo")) {
		/*
		 * The colour of each picture output, as its buffers' attachments: BWMultiStreamCameraSourceNode
		 * compares a still image output's with the colour space it asked for before it makes its buffer
		 * pools, and the session does not start when they differ.
		 */
		CFMutableDictionaryRef d = (CFMutableDictionaryRef)new_dict();

		for (int i = 0; i < PICTURE_OUTPUTS; i++) {
			put(d, output_names[i], CFRetain(colour_attachments()));
		}
		value = d;
	} else if (key_is(key, "SupportedPropertiesDictionary")) {
		value = CFRetain(s->supported);
	} else if (key_is(key, "MaxZoomFactor")) {
		value = number_f(MAX_ZOOM);
	} else if (key_is(key, "ZoomFactor")) {
		value = number_f(s->zoom);
	} else if (key_is(key, "SupportedColorSpaces")) {
		CFNumberRef srgb = number_i(0);

		value = CFArrayCreate(NULL, (const void **)&srgb, 1, &kCFTypeArrayCallBacks);
		CFRelease(srgb);
	} else if (key_is(key, "DeviceOrientation")) {
		pthread_mutex_lock(&s->lock);
		value = number_i(s->device_orientation);
		pthread_mutex_unlock(&s->lock);
	} else if (key_is(key, "ColorSpace")) {
		value = number_i(0);
	} else if (key_is(key, "FlashMode")) {
		pthread_mutex_lock(&s->lock);
		int fm = s->flash_mode;
		pthread_mutex_unlock(&s->lock);
		value = CFRetain(fm == 1 ? CFSTR("ForceFlashOn") : (fm == 2 ? CFSTR("Auto") : CFSTR("ForceFlashOff")));
	} else if (key_is(key, "HasAutoFocusPositionSensor") || key_is(key, "DeferAdditionOfAttachments")) {
		value = CFRetain(kCFBooleanFalse);
	} else if (key_is(key, "GeometricDistortionCorrectedRects")) {
		pthread_mutex_lock(&s->lock);
		value = stream_gdc_rects(s);
		pthread_mutex_unlock(&s->lock);
	} else {
		say_once("stream: no answer for", key, NULL);
		return ERR_NOT_AVAILABLE;
	}
	*(CFTypeRef *)out = value;
	return 0;
}

static OSStatus torch_set(struct stream *s, CFTypeRef value);

static bool
dict_bool(CFDictionaryRef d, CFStringRef k)
{
	CFTypeRef v = d != NULL ? CFDictionaryGetValue(d, k) : NULL;

	if (v == NULL) {
		return false;
	}
	if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
		return CFBooleanGetValue(v);
	}
	if (CFGetTypeID(v) == CFNumberGetTypeID()) {
		int64_t n = 0;

		return CFNumberGetValue(v, kCFNumberSInt64Type, &n) && n != 0;
	}
	return false;
}

static int
parse_flash_mode(CFTypeRef value, int fallback)
{
	if (value == NULL) {
		return fallback;
	}
	if (CFGetTypeID(value) == CFStringGetTypeID()) {
		if (key_is(value, "ForceFlashOn") || key_is(value, "On")) {
			return 1;
		}
		if (key_is(value, "Auto")) {
			return 2;
		}
		if (key_is(value, "ForceFlashOff") || key_is(value, "Off")) {
			return 0;
		}
		return fallback;
	}
	if (CFGetTypeID(value) == CFNumberGetTypeID()) {
		int64_t n = fallback;
		if (CFNumberGetValue(value, kCFNumberSInt64Type, &n)) {
			return n == 1 ? 1 : (n == 2 ? 2 : 0);
		}
	}
	return fallback;
}

static double
dict_number(CFDictionaryRef d, const char *name, double otherwise)
{
	CFStringRef k = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CFTypeRef v = CFDictionaryGetValue(d, k);
	double n = otherwise;

	CFRelease(k);
	if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
		CFNumberGetValue(v, kCFNumberDoubleType, &n);
	}
	return n;
}

#include "camera_device_focus.inc"
#include "camera_device_orient.inc"

static OSStatus
stream_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	struct stream *s = stream_of(o);

	if (key_is(key, "TorchLevel")) {
		return torch_set(s, value);
	}
	if (s->profile.bytes == 0) {
		return ERR_NOT_AVAILABLE;
	}
	if (key_is(key, "FormatIndex")) {
		int64_t i = 0;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberSInt64Type, &i);
		}
		if (i < 0 || i >= CFArrayGetCount(s->formats)) {
			return ERR_PARAM;
		}
		s->format_index = (int)i;
		say("stream: format %lld", (long long)i);
		return 0;
	}
	if (key_is(key, "ZoomFactor")) {
		double zoom = 1;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberDoubleType, &zoom);
		}
		zoom = zoom < 1 ? 1 : zoom > MAX_ZOOM ? MAX_ZOOM : zoom;
		if (fabs(zoom - s->zoom) >= 0.25 || (zoom == 1) != (s->zoom == 1)) {
			say("stream: zoom %.2fx", zoom);
		}
		s->zoom = (float)zoom;
		return 0;
	}
	if (key_is(key, "MinimumFrameRate") || key_is(key, "MaximumFrameRate")) {
		double fps = 0;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberDoubleType, &fps);
		}
		*(key_is(key, "MinimumFrameRate") ? &s->min_fps : &s->max_fps) = fps;
		say_once("stream: set", key, value);
		return 0;
	}
	if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled") || key_is(key, "VideoOutputHandlers")) {
		bool configs = key_is(key, "VideoOutputConfigurations"), enabled = key_is(key, "VideoOutputsEnabled");

		say_once("stream: set", key, value);
		if (value == NULL || CFGetTypeID(value) != CFDictionaryGetTypeID()) {
			return 0;
		}
		pthread_mutex_lock(&s->lock);
		CFIndex n = CFDictionaryGetCount(value);
		const void **keys = calloc((size_t)n + 1, sizeof(void *)), **values = calloc((size_t)n + 1, sizeof(void *));

		CFDictionaryGetKeysAndValues(value, keys, values);
		for (CFIndex i = 0; i < n; i++) {
			int k = CFGetTypeID(keys[i]) == CFStringGetTypeID() ? output_index(keys[i]) : -1;
			struct output *out;

			if (k < 0) {
				if (configs && CFGetTypeID(keys[i]) == CFStringGetTypeID() &&
				    key_is(keys[i], "Metadata") && CFGetTypeID(values[i]) == CFDictionaryGetTypeID()) {
					CFMutableDictionaryRef mc = s->meta_config != NULL ?
					    CFDictionaryCreateMutableCopy(NULL, 0, s->meta_config) :
					    (CFMutableDictionaryRef)new_dict();
					CFIndex mn = CFDictionaryGetCount(values[i]);
					const void **mkeys = calloc((size_t)mn + 1, sizeof(void *));
					const void **mvals = calloc((size_t)mn + 1, sizeof(void *));

					CFDictionaryGetKeysAndValues(values[i], mkeys, mvals);
					for (CFIndex mi = 0; mi < mn; mi++) {
						CFDictionarySetValue(mc, mkeys[mi], mvals[mi]);
					}
					free(mkeys);
					free(mvals);
					if (s->meta_config != NULL) {
						CFRelease(s->meta_config);
					}
					s->meta_config = mc;
					continue;
				}
				if (CFGetTypeID(keys[i]) != CFStringGetTypeID() || !key_is(keys[i], "Metadata")) {
					say_once("stream: an output not here:", keys[i], NULL);
				}
				continue;
			}
			out = &s->out[k];
			if (configs) {
				let_go(out);
				if (CFGetTypeID(values[i]) == CFDictionaryGetTypeID()) {
					CFMutableDictionaryRef merged = out->config != NULL ?
					    CFDictionaryCreateMutableCopy(NULL, 0, out->config) :
					    (CFMutableDictionaryRef)new_dict();
					CFIndex cn = CFDictionaryGetCount(values[i]);
					const void **ckeys = calloc((size_t)cn + 1, sizeof(void *));
					const void **cvals = calloc((size_t)cn + 1, sizeof(void *));

					CFDictionaryGetKeysAndValues(values[i], ckeys, cvals);
					for (CFIndex ci = 0; ci < cn; ci++) {
						CFDictionarySetValue(merged, ckeys[ci], cvals[ci]);
					}
					free(ckeys);
					free(cvals);
					if (out->config != NULL) {
						CFRelease(out->config);
					}
					out->config = merged;
				} else if (out->config != NULL) {
					CFRelease(out->config);
					out->config = NULL;
				}
			} else if (enabled) {
				int64_t on = 1;

				if (CFGetTypeID(values[i]) == CFBooleanGetTypeID()) {
					on = CFBooleanGetValue(values[i]);
				} else if (CFGetTypeID(values[i]) == CFNumberGetTypeID()) {
					CFNumberGetValue(values[i], kCFNumberSInt64Type, &on);
				}
				out->enabled = on != 0;
			} else {
				CFTypeRef h = CFGetTypeID(values[i]) == CFDictionaryGetTypeID() ?
				    CFDictionaryGetValue(values[i], CFSTR("PixelBufferHandler")) : NULL;

				if (out->handler != NULL) {
					CFRelease(out->handler);
				}
				out->handler = h != NULL ? (void *)CFRetain(h) : NULL;
			}
		}
		free(keys);
		free(values);
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (key_is(key, "AFSearchPositions") || key_is(key, "AFEarlyOutParams") || key_is(key, "AFPeakPredictionEnabled")) {
		af_search_set(s, key, value);
		return 0;
	}
	if (key_is(key, "ExposureBias")) {
		/* a number, or with the command it answers to: { ExposureBias, CommandID } */
		CFTypeRef v = value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID() ?
		    CFDictionaryGetValue(value, CFSTR("ExposureBias")) : value;
		int32_t bias;

		if (v == NULL || CFGetTypeID(v) != CFNumberGetTypeID() || !CFNumberGetValue(v, kCFNumberSInt32Type, &bias)) {
			return ERR_PARAM;
		}
		bias = bias < 1 ? 1 : bias > 65535 ? 65535 : bias;
		pthread_mutex_lock(&s->lock);
		s->exposure_bias = (uint32_t)bias;
		pthread_mutex_unlock(&s->lock);
		/* (a drag of the slider sends one for each step) */
		if (fabs(log2((double)bias / s->said_bias)) >= 0.5 || bias == 256) {
			say("stream: exposure bias %+.2f EV", log2(bias / 256.0));
			s->said_bias = (uint32_t)bias;
		}
		return 0;
	}
	if (key_is(key, "FlashMode")) {
		int fm = parse_flash_mode(value, -1);

		if (fm < 0) {
			return ERR_PARAM;
		}
		pthread_mutex_lock(&s->lock);
		s->flash_mode = fm;
		pthread_mutex_unlock(&s->lock);
		say("stream: flash mode %s", fm == 1 ? "ForceFlashOn" : (fm == 2 ? "Auto" : "ForceFlashOff"));
		return 0;
	}
	if (key_is(key, "DeviceOrientation")) {
		device_orientation_set(s, value);
		return 0;
	}
	if (key_is(key, "StillImageCaptureNow")) {
		pthread_mutex_lock(&s->lock);
		int fm = s->flash_mode;
		bool redeye = false;

		if (value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
			fm = parse_flash_mode(CFDictionaryGetValue(value, CFSTR("FlashMode")), fm);
			redeye = dict_bool(value, CFSTR("GenerateRedEyeAuxiliaryImage"));
			CFTypeRef ov = CFDictionaryGetValue(value, CFSTR("BracketedCaptureOverrides"));

			if (ov != NULL && CFGetTypeID(ov) == CFDictionaryGetTypeID()) {
				fm = parse_flash_mode(CFDictionaryGetValue(ov, CFSTR("FlashMode")), fm);
			}
		}
		if (s->torch >= 0 && (redeye || fm == 1 || (fm == 2 && s->last_lux > 320.0f))) {
			s->still_flash_wanted = true;
		}
		if (redeye) {
			s->still_redeye_wanted = true;
			s->stills_wanted += 2;
		} else {
			s->stills_wanted++;
		}
		pthread_mutex_unlock(&s->lock);
		say_value("stream: a photograph asked for", key, value);
		return 0;
	}
	if (key_is(key, "FaceDetectionConfiguration")) {
		pthread_mutex_lock(&s->lock);
		if (s->face_config != NULL) {
			CFRelease(s->face_config);
		}
		s->face_config = value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID() ? CFRetain(value) : NULL;
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (key_is(key, "AutoFocusNow") || key_is(key, "ContinuousAutoFocusNow") || key_is(key, "FocusNow") ||
	    key_is(key, "PeakTrackingFocusNow") || key_is(key, "LockAFNow") || key_is(key, "ManualFocusNow") ||
	    key_is(key, "AFWindowParams") || key_is(key, "AutoFocusMode")) {
		if (!key_is(key, "AFWindowParams") || (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() &&
		    CFArrayGetCount(value) > 0)) {
			say_value("stream: focus:", key, value);
		}
		pthread_mutex_lock(&s->lock);
		if (key_is(key, "AFWindowParams")) {
			af_window(s, value);
		} else if (key_is(key, "LockAFNow")) {
			s->af_locked = true;
		} else if (key_is(key, "AutoFocusNow") || key_is(key, "ContinuousAutoFocusNow") || key_is(key, "FocusNow") ||
		    key_is(key, "PeakTrackingFocusNow")) {
			s->af_locked = false;
			s->af_now = true;
			if (value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
				CFTypeRef id = CFDictionaryGetValue((CFDictionaryRef)value, CFSTR("CommandID"));
				int64_t n = 0;
				if (id != NULL && CFGetTypeID(id) == CFNumberGetTypeID() &&
				    CFNumberGetValue(id, kCFNumberSInt64Type, &n)) {
					s->focus_cmdid = n;
				}
			}
		}
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (!say_once("stream: set (not used)", key, value)) {
		/* (the name only: the order of what comes, a tap's for one) */
		say_value("stream: set (not used) again:", key, NULL);
	}
	return 0;
}

#include "camera_device_stream.inc"

/* ---- the device ------------------------------------------------------------------------------------- */

struct device {
	CFArrayRef      streams;
	CFDictionaryRef supported;
};

static OSStatus
device_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
	struct device *d = derived_storage(o);
	CFTypeRef value = NULL;

	(void)allocator;
	if (key_is(key, "StreamArray")) {
		value = CFRetain(d->streams);
	} else if (key_is(key, "Clock")) {
		value = CFRetain(CMClockGetHostTimeClock());
	} else if (key_is(key, "SupportedPropertiesDictionary")) {
		value = CFRetain(d->supported);
	} else if (key_is(key, "SupportedSynchronizedStreamsGroups")) {
		value = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
	} else {
		say_once("device: no answer for", key, NULL);
		return ERR_NOT_AVAILABLE;
	}
	*(CFTypeRef *)out = value;
	return 0;
}

static OSStatus
device_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	(void)o;
	say_once("device: set (not used)", key, value);
	return 0;
}

static unsigned requests, relinquishes;

static OSStatus
device_request_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
	(void)o;
	if (requests++ < 3) {
		say("device: streams asked for (%ld)%s", streams ? (long)CFArrayGetCount(streams) : 0L, options ? ", with options" : "");
		if (options != NULL) {
			say_once("device: the request's options", CFSTR("RequestStreams"), options);
		}
	}
	/*
	 * Control alone starts no frames: building the camera list, CMCapture takes control of every
	 * stream in turn to read its properties. The sensor starts with the stream's Start.
	 */
	return 0;
}

static OSStatus
device_relinquish_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
	(void)o;
	(void)options;
	if (relinquishes++ < 3) {
		say("device: streams given back (%ld)", streams ? (long)CFArrayGetCount(streams) : 0L);
	} else if (relinquishes % 1000 == 0) {
		say("device: streams given back %u times", relinquishes);
	}
	if (streams != NULL) {
		CFIndex n = CFArrayGetCount(streams);

		for (CFIndex i = 0; i < n; i++) {
			CMBaseObjectRef st = (CMBaseObjectRef)CFArrayGetValueAtIndex(streams, i);

			if (st != NULL) {
				(void)stream_stop(st);
			}
		}
	}
	return 0;
}

static OSStatus
device_invalidate(CMBaseObjectRef o)
{
	(void)o;
	return 0;
}

static void
device_finalize(CMBaseObjectRef o)
{
	struct device *d = derived_storage(o);

	if (d->streams) {
		CFRelease(d->streams);
	}
	if (d->supported) {
		CFRelease(d->supported);
	}
}

static CFStringRef
device_description(CMBaseObjectRef o)
{
	(void)o;
	return CFRetain(CFSTR("<AnnibaleCaptureDevice>"));
}

static const struct base_class device_base = {
	1, sizeof(struct device), NULL, device_invalidate, device_finalize, device_description, device_copy_property, device_set_property,
	NULL, NULL, NULL
};
static const struct device_class device_derived = { 3, NULL, device_request_streams, device_relinquish_streams, NULL };
static const struct device_vtable device_vtable = { NULL, &device_base, &device_derived };

/* The vendor's device create function (as H10ISPCaptureDeviceCreate / H16ISP is called). */
OSStatus annibale_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out);

OSStatus
annibale_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out)
{
	static bool found, usable;
	CMBaseObjectRef device = NULL, stream[MAX_STREAMS] = { NULL };
	CFIndex nstreams = 0;
	struct device *d;

	(void)options;
	if (out != NULL) {
		*out = NULL;
	}
	if (!found) {
		found = true;
		usable = find_functions();
	}
	if (!usable || out == NULL) {
		return ERR_UNSUPPORTED;
	}
	for (unsigned i = 0; i < sizeof(cameras) / sizeof(cameras[0]); i++) {
		struct annibale_camera_profile profile;
		int torch_fd = cameras[i].sensor == 1 ? open("/dev/annibale-torch", O_RDWR) : -1;
		int fd = camera_native_open(cameras[i].sensor, &profile);
		for (int attempt = 0; fd < 0 && errno == EBUSY && attempt < 10; attempt++) {
			usleep(100000);
			fd = camera_native_open(cameras[i].sensor, &profile);
		}

		if (fd < 0 && torch_fd < 0) {
			continue;
		}
		if (fd >= 0) {
			close(fd);
		}
		/* FigFlashlight requests the back port without stream control on iOS 26. */
		CMBaseObjectRef one = stream_create(allocator, &cameras[i], cameras[i].sensor == 1 ? port_back : port_front,
		    fd >= 0 ? &profile : NULL, torch_fd);
		if (one != NULL) {
			stream[nstreams++] = one;
		} else if (torch_fd >= 0) {
			close(torch_fd);
		}
	}
	if (nstreams == 0) {
		return ERR_NOT_AVAILABLE;
	}
	if (derived_create(allocator, &device_vtable, device_class_id(), &device) != 0 || device == NULL) {
		for (CFIndex i = 0; i < nstreams; i++) {
			CFRelease(stream[i]);
		}
		say("the device cannot be made");
		return ERR_UNSUPPORTED;
	}
	d = derived_storage(device);
	memset(d, 0, sizeof(*d));
	d->streams = CFArrayCreate(NULL, (const void **)stream, nstreams, &kCFTypeArrayCallBacks);
	for (CFIndex i = 0; i < nstreams; i++) {
		all_streams[i] = stream[i];
		CFRelease(stream[i]);
	}
	d->supported = supported(device_props, sizeof(device_props) / sizeof(device_props[0]));
	*out = device;
	say("the device made: %ld streams", (long)nstreams);
	return 0;
}
