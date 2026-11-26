/*
 * libannibale-media.dylib: code of ours inside mediaserverd and cameracaptured (see inject.c).
 *
 * On iOS 16.5, mediaserverd hosts both the audio server and CMCapture's capture stack. On iOS 26,
 * the capture stack runs inside cameracaptured (with mediaserverd also loading CMCapture pieces).
 *
 * The flashlight (only where the kernel has one: torch=3, /dev/annibale-torch): Control Center's
 * button and AVFlashlight reach FigFlashlight in here, which asks the capture stack's device vendor
 * (+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]) for the camera device and its back
 * camera's stream, and sets the stream's "TorchLevel" (0 to 1).
 *
 * The camera (where the kernel has /dev/annibale-camera): the vendor is initialized with the create
 * function of our capture device (camera_device.c, annibale_camera_device_create) in place of
 * H10ISP/H16ISP.mediacapture; the capture graph is built around it and its frames reach Camera.app's
 * preview.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define STRIP_FN(p) ((uintptr_t)ptrauth_strip((void *)(p), ptrauth_key_function_pointer))
#else
#define STRIP_FN(p) ((uintptr_t)(p))
#endif

extern uint32_t IOSurfaceGetID(IOSurfaceRef buffer);
extern int IOSurfaceLock(IOSurfaceRef buffer, uint32_t options, uint32_t *seed);
extern void *IOSurfaceGetBaseAddress(IOSurfaceRef buffer);
extern void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef buffer, size_t plane);
extern size_t IOSurfaceGetWidth(IOSurfaceRef buffer);
extern size_t IOSurfaceGetHeight(IOSurfaceRef buffer);
extern size_t IOSurfaceGetAllocSize(IOSurfaceRef buffer);

/* ---- CPU reads of an IOSurface wait for pending GPU writes ---------------------------------------
 *
 * A processor lock or base-address lookup on a surface must stand on the far side of the GPU
 * commit that last wrote it; on unified memory that ordering is the driver's contract. The still
 * pipeline's encoder reads the fusion output surface on the CPU, and when the still commits queue
 * behind cold-start work the read landed before the write (first photo black; see
 * kernel-12377-say16-ubo12). libAnnibaleMetal records, per surface, the newest commit touching it
 * and exports annibale_surface_writes_wait; processes without the driver resolve nothing and run
 * unchanged. */

static void say(const char *format, ...);
static void (*pending_writes_wait)(uint32_t sid);
static unsigned pwrite_log_count;

/* Name the frame that called the interposed function: first frame outside this dylib. */
static const char *
pwrite_caller(void)
{
	static char name[160];
	void *pcs[8];
	int n = backtrace(pcs, 8);

	if (n <= 0) {
		return "?";
	}
	const void *self = NULL;
	{
		Dl_info info;
		if (dladdr((const void *)&pwrite_log_count, &info)) {
			self = info.dli_fbase;
		}
	}
	for (int i = 1; i < n; i++) {
		Dl_info info;
		if (!dladdr(pcs[i], &info) || info.dli_fbase == self) {
			continue;
		}
		if (info.dli_sname != NULL) {
			snprintf(name, sizeof(name), "%s", info.dli_sname);
		} else if (info.dli_fname != NULL) {
			snprintf(name, sizeof(name), "%s+%#lx", info.dli_fname,
			    (uintptr_t)pcs[i] - (uintptr_t)info.dli_fbase);
		}
		return name;
	}
	return "?";
}

static void
wait_pending_writes(IOSurfaceRef sf, const char *how)
{
	if (sf == NULL) {
		return;
	}
	uint32_t sid = IOSurfaceGetID(sf);
	BOOL is_emit = how != NULL && how[0] == 'e';
	if (is_emit || pwrite_log_count < 32) {
		pwrite_log_count++;
		say("annibale: media: pwrite call %s surf=%u by %s\n", how, sid, pwrite_caller());
	}
	if (pending_writes_wait == NULL) {
		/* The driver may load after the first lock; retry until it answers. */
		pending_writes_wait = dlsym(RTLD_DEFAULT, "annibale_surface_writes_wait");
		if (pending_writes_wait == NULL) {
			return;
		}
	}
	uint64_t start = mach_absolute_time();
	pending_writes_wait(sid);
	uint64_t waited = mach_absolute_time() - start;
	if (waited > 0) {
		static mach_timebase_info_data_t tb;
		if (tb.denom == 0) {
			mach_timebase_info(&tb);
		}
		unsigned us = (unsigned)(waited * tb.numer / tb.denom / 1000);
		if (us > 100) {
			say("annibale: media: pwrite surf=%u wait=%uus\n", sid, us);
		}
	}
}

static void
fourcc_text(uint32_t value, char text[5])
{
	if (value == 0) {
		memcpy(text, "----", 5);
		return;
	}
	for (unsigned i = 0; i < 4; i++) {
		unsigned char c = (unsigned char)(value >> (24 - i * 8));
		text[i] = c >= 0x20 && c <= 0x7e ? (char)c : '?';
	}
	text[4] = '\0';
}

static CVReturn
watched_CVPixelBufferLockBaseAddress(CVPixelBufferRef pixelBuffer, CVOptionFlags lockFlags)
{
	wait_pending_writes(CVPixelBufferGetIOSurface(pixelBuffer), "cvpb-lock");
	return CVPixelBufferLockBaseAddress(pixelBuffer, lockFlags);
}

static void *
watched_CVPixelBufferGetBaseAddress(CVPixelBufferRef pixelBuffer)
{
	wait_pending_writes(CVPixelBufferGetIOSurface(pixelBuffer), "cvpb-base");
	return CVPixelBufferGetBaseAddress(pixelBuffer);
}

static void *
watched_CVPixelBufferGetBaseAddressOfPlane(CVPixelBufferRef pixelBuffer, size_t planeIndex)
{
	wait_pending_writes(CVPixelBufferGetIOSurface(pixelBuffer), "cvpb-plane");
	return CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, planeIndex);
}

static int
watched_IOSurfaceLock(IOSurfaceRef buffer, uint32_t options, uint32_t *seed)
{
	wait_pending_writes(buffer, "surf-lock");
	return IOSurfaceLock(buffer, options, seed);
}

static void *
watched_IOSurfaceGetBaseAddress(IOSurfaceRef buffer)
{
	wait_pending_writes(buffer, "surf-base");
	return IOSurfaceGetBaseAddress(buffer);
}

static void *
watched_IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef buffer, size_t plane)
{
	wait_pending_writes(buffer, "surf-plane");
	return IOSurfaceGetBaseAddressOfPlane(buffer, plane);
}

__attribute__((used)) static const struct {
	const void *ours, *theirs;
} lock_interpose[] __attribute__((section("__DATA,__interpose"))) = {
	{ watched_CVPixelBufferLockBaseAddress, CVPixelBufferLockBaseAddress },
	{ watched_CVPixelBufferGetBaseAddress, CVPixelBufferGetBaseAddress },
	{ watched_CVPixelBufferGetBaseAddressOfPlane, CVPixelBufferGetBaseAddressOfPlane },
	{ watched_IOSurfaceLock, IOSurfaceLock },
	{ watched_IOSurfaceGetBaseAddress, IOSurfaceGetBaseAddress },
	{ watched_IOSurfaceGetBaseAddressOfPlane, IOSurfaceGetBaseAddressOfPlane },
};

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;

#include "inject_media_mg.inc"

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/* ---- the flashlight & camera vendor --------------------------------------------------------------- */

static int torch = -1;                  /* /dev/annibale-torch */
static bool camera;                     /* /dev/annibale-camera is there */
extern int annibale_camera_device_create(const void *allocator, const void *options, void **out);
static Class vendor_class, device_class, stream_class;
static IMP original_shared_vendor;
static IMP original_shared_vendor_with_func;
static unsigned torch_said;

/*
 * FigFlashlight's functions that ask for the vendor, unslid (20F66): flashlight_IsAvailable,
 * _NotifyForCurrentState and _setupDevice lie together, _teardownDevice apart.
 */
static const struct { uintptr_t from, to; } flashlight_code[] = {
	{ 0x1a1fb0830UL, 0x1a1fb0c5cUL }, { 0x1a1eee5c0UL, 0x1a1eee6e8UL },
};

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:] (iOS 26) */
static id
shared_vendor_with_func(id self, SEL selector, void *func)
{
	if (!camera && access("/dev/annibale-camera", F_OK) == 0) {
		camera = true;
		say("annibale: media: the kernel has a camera (/dev/annibale-camera)\n");
	}
	if (func == NULL && camera) {
		func = (void *)annibale_camera_device_create;
		say("annibale: media: supplying annibale_camera_device_create to sharedCaptureDeviceVendor\n");
	}
	return ((id (*)(id, SEL, void *))original_shared_vendor_with_func)(self, selector, func);
}

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor] */
static id
shared_vendor(id self, SEL selector)
{
	uintptr_t caller = STRIP_FN(__builtin_return_address(0)) - slide;
	static id ours, camera_vendor;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	if (!camera && access("/dev/annibale-camera", F_OK) == 0) {
		camera = true;          /* (should the kernel have made it after the daemon started) */
		say("annibale: media: the kernel has a camera (/dev/annibale-camera)\n");
	}
	if (camera && original_shared_vendor_with_func != NULL) {
		return shared_vendor_with_func(self,
		    sel_registerName("sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:"),
		    (void *)annibale_camera_device_create);
	}

	id vendor = ((id (*)(id, SEL))original_shared_vendor)(self, selector);
	if (vendor != NULL) {
		return vendor;
	}
	if (camera) {
		pthread_mutex_lock(&lock);
		if (camera_vendor == NULL) {
			Class vcls = objc_getClass("BWFigCaptureDeviceVendor");
			id made = ((id (*)(Class, SEL))objc_msgSend)(vcls, sel_registerName("alloc"));

			if (class_getInstanceMethod(vcls, sel_registerName("initWithDeviceCreateFunction:")) != NULL) {
				camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(made,
				    sel_registerName("initWithDeviceCreateFunction:"),
				    (void *)annibale_camera_device_create);
			} else if (class_getInstanceMethod(vcls,
			    sel_registerName("initWithDefaultDeviceCreateFunction:cmioExtensionDeviceCreateFunction:")) != NULL) {
				camera_vendor = ((id (*)(id, SEL, void *, void *))objc_msgSend)(made,
				    sel_registerName("initWithDefaultDeviceCreateFunction:cmioExtensionDeviceCreateFunction:"),
				    (void *)annibale_camera_device_create, NULL);
			}
			say("annibale: media: the camera's device vendor %s\n", camera_vendor ? "made (camera_device.c)" :
			    "CANNOT BE MADE");
		}
		pthread_mutex_unlock(&lock);
		return camera_vendor;
	}
	for (size_t i = 0; torch >= 0 && i < sizeof(flashlight_code) / sizeof(flashlight_code[0]); i++) {
		if (caller >= flashlight_code[i].from && caller < flashlight_code[i].to) {
			if (ours == NULL) {
				ours = class_createInstance(vendor_class, 0);
				say("annibale: media: no camera, so no device vendor: FigFlashlight gets the flashlight's own\n");
			}
			return ours;
		}
	}
	return NULL;
}

static void
flashlight_set(uint32_t level)
{
	ssize_t written = write(torch, &level, sizeof(level));

	if (written != (ssize_t)sizeof(level) || torch_said < 40) {
		torch_said++;
		say("annibale: mediaserverd: the flashlight at %u of 1000%s\n", level, written == (ssize_t)sizeof(level) ? "" :
		    " (NOT TAKEN by /dev/annibale-torch)");
	}
}

/* the vendor's: one client, one device, always available to it */
static int
flashlight_register(__unused id self, __unused SEL selector, __unused int pid, __unused id description, __unused int priority,
    __unused BOOL steal, __unused BOOL share, __unused id handler)
{
	return 1;
}

static id
flashlight_copy_device(__unused id self, __unused SEL selector, __unused int client, __unused BOOL inform, int *error)
{
	if (error != NULL) {
		*error = 0;
	}
	return class_createInstance(device_class, 0);
}

static id
flashlight_copy_stream(__unused id self, __unused SEL selector, __unused int position, __unused int type, id device)
{
	return device != NULL && object_getClass(device) == device_class ? class_createInstance(stream_class, 0) : NULL;
}

static void
flashlight_take_back(__unused id self, __unused SEL selector, __unused id device, __unused int client, __unused BOOL inform)
{
	flashlight_set(0);
}

static BOOL
flashlight_active_equals(__unused id self, __unused SEL selector, id device)
{
	return device == NULL || object_getClass(device) == device_class;
}

/* the stream's -supportedProperties: none that FigFlashlight looks for ("HighCurrentTorchEnabled") */
static id
flashlight_stream_properties(__unused id self, __unused SEL selector)
{
	static CFDictionaryRef none;

	if (none == NULL) {
		none = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return (id)none;
}

/* the stream's -setProperty:value: -- "TorchLevel", 0 to 1, as thousandths to the kernel */
static int
flashlight_stream_set(__unused id self, __unused SEL selector, id key, id value)
{
	float level = 0;

	if (key != NULL && CFEqual((CFTypeRef)key, CFSTR("TorchLevel"))) {
		if (value != NULL && CFGetTypeID((CFTypeRef)value) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)value, kCFNumberFloatType, &level);
		}
		level = level < 0 ? 0 : level > 1 ? 1 : level;
		flashlight_set((uint32_t)(level * 1000 + 0.5f));
	}
	return 0;
}

/*
 * Video stabilization, of which this hardware has none: 0 means kFigCaptureVideoStabilizationType_None,
 * so the capture graph is built without a VIS pipeline.
 */
static int
video_stabilization_method(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;
}

static int
video_stabilization_type(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;                       /* kFigCaptureVideoStabilizationType_None */
}

/*
 * The microphone, when the capture audio HAL plug-in is absent ('nope'): swallow that error so the
 * camera preview and photo graph continue running.
 */
#define ERROR_NO_SUCH_THING     1852797029      /* 'nope' */

static IMP original_audio_source_start;

static BOOL
audio_source_start(id self, SEL selector, id *error)
{
	BOOL started = ((BOOL (*)(id, SEL, id *))original_audio_source_start)(self, selector, error);
	id made = error != NULL ? *error : NULL;
	static bool said;

	if (started || made == NULL || ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
		return started;
	}
	*error = NULL;                  /* (autoreleased by the capture stack: not ours to give back) */
	if (!said) {
		said = true;
		say("annibale: mediaserverd: there is no capture audio on this phone: the session's audio source gives nothing, "
		    "and the camera runs without it\n");
	}
	return true;
}

/*
 * A photograph's quality prioritization, "speed" at the most (one frame, no SIS bracket).
 */
static IMP original_quality_prioritization;

static int
quality_prioritization(id self, SEL selector)
{
	int asked = ((int (*)(id, SEL))original_quality_prioritization)(self, selector);
	static bool said;

	if (asked > 1 && !said) {
		said = true;
		say("annibale: mediaserverd: a photograph's quality prioritization %d taken as 1 (speed): one frame, no SIS bracket\n",
		    asked);
	}
	return asked > 1 ? 1 : asked;
}

/*
 * A photograph's noise reduction (BWGNRNode): pass the sample buffer straight to the node's output.
 */
static void
gnr_render(id self, SEL selector, void *sample_buffer, id input)
{
	id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));
	static bool said;

	(void)selector, (void)input;
	if (!said) {
		said = true;
		say("annibale: mediaserverd: a photograph goes past the noise reduction, which has nothing to do it with here\n");
	}
	if (output != nil) {
		((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
	}
}

static bool
is_node(id node, const char *name)
{
	return node != nil && strcmp(object_getClassName(node), name) == 0;
}

static void
emit_image_format(id node, CVImageBufferRef img, unsigned surf, unsigned seq)
{
	uint32_t fmt = 0;
	char fmt_text[5] = "----";
	size_t pc = 0, shown = 0, w[3] = { 0 }, h[3] = { 0 }, stride[3] = { 0 };
	size_t ep_left = 0, ep_right = 0, ep_top = 0, ep_bottom = 0;
	size_t sw = 0, sh = 0, alloc = 0;

	if (img != NULL) {
		fmt = CVPixelBufferGetPixelFormatType(img);
		pc = CVPixelBufferGetPlaneCount(img);
		shown = pc == 0 ? 1 : pc;
		if (shown > 3) {
			shown = 3;
		}
		for (size_t i = 0; i < shown; i++) {
			if (pc == 0) {
				w[i] = CVPixelBufferGetWidth(img);
				h[i] = CVPixelBufferGetHeight(img);
				stride[i] = CVPixelBufferGetBytesPerRow(img);
			} else {
				w[i] = CVPixelBufferGetWidthOfPlane(img, i);
				h[i] = CVPixelBufferGetHeightOfPlane(img, i);
				stride[i] = CVPixelBufferGetBytesPerRowOfPlane(img, i);
			}
		}
		CVPixelBufferGetExtendedPixels(img, &ep_left, &ep_right, &ep_top, &ep_bottom);
		IOSurfaceRef sf = CVPixelBufferGetIOSurface(img);
		if (sf != NULL) {
			sw = IOSurfaceGetWidth(sf);
			sh = IOSurfaceGetHeight(sf);
			alloc = IOSurfaceGetAllocSize(sf);
		}
		fourcc_text(fmt, fmt_text);
	}
	say("annibale: media: emitfmt %s img=%p fmt=%s/%08x pc=%zu p0=%zux%zur%zu p1=%zux%zur%zu p2=%zux%zur%zu more=%zu surf=%u sw=%zu sh=%zu alloc=%zu ep=%zu,%zu,%zu,%zu seq=%u\n",
	    object_getClassName(node), img, fmt_text, fmt, pc,
	    w[0], h[0], stride[0], w[1], h[1], stride[1], w[2], h[2], stride[2],
	    pc > shown ? pc - shown : 0, surf, sw, sh, alloc, ep_left, ep_right, ep_top, ep_bottom, seq);
}

/*
 * How long a photograph's encoding took, from the picture handed to the photo encoder node to the one
 * it hands on (HEVC in software, see register_hevc_encoder below: the slowest step of a photograph).
 */
static void
time_photo_encoder(id output, CMSampleBufferRef sample_buffer)
{
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buffer);
	CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buffer);
	id node = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("node"));
	static uint64_t began;
	static mach_timebase_info_data_t timebase;
	id connection, input;

	if (is_node(node, "BWPhotoEncoderNode")) {
		if (began != 0) {
			if (timebase.denom == 0) {
				mach_timebase_info(&timebase);
			}
			size_t blk_len = block != NULL ? CMBlockBufferGetDataLength(block) : 0;
			size_t w = image != NULL ? CVPixelBufferGetWidth(image) : 0;
			size_t h = image != NULL ? CVPixelBufferGetHeight(image) : 0;
			say("annibale: mediaserverd: a photograph (%zux%zu, block=%zu B) encoded in %.0f ms\n",
			    w, h, blk_len,
			    (double)(mach_absolute_time() - began) * timebase.numer / timebase.denom / 1e6);
		}
		began = 0;
		return;
	}
	if (image == NULL || CVPixelBufferGetWidth(image) < 2000) {
		return;                 /* not a photograph */
	}
	connection = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("connection"));
	input = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("input")) : nil;
	if (input != nil && is_node(((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("node")), "BWPhotoEncoderNode")) {
		began = mach_absolute_time();
	}
}

/*
 * A node's output given no sample buffer at all: drop NULL sample buffers before they reach downstream nodes.
 */
static IMP original_emit_sample_buffer;
static IMP original_emit_node_error;
static IMP original_photo_encoder_render;
static IMP original_ms_configure;
static IMP original_ms_allocate_pools;
static IMP original_ms_prepare;
static IMP original_ms_start;

/* The encoder input is downstream of every still-chain emit; a separate entry line shows whether
 * the frame reached BWPhotoEncoderNode before the first node error. */
static void
photo_encoder_render(id self, SEL selector, CMSampleBufferRef sample_buffer, id input)
{
	static unsigned encinput_count;
	CVImageBufferRef img = sample_buffer != NULL ? CMSampleBufferGetImageBuffer(sample_buffer) : NULL;
	CMBlockBufferRef block = sample_buffer != NULL ? CMSampleBufferGetDataBuffer(sample_buffer) : NULL;
	uint32_t fmt = img != NULL ? CVPixelBufferGetPixelFormatType(img) : 0;
	char fmt_text[5] = "----";
	unsigned surf = 0;

	if (encinput_count < 32) {
		encinput_count++;
		if (img != NULL) {
			IOSurfaceRef sf = CVPixelBufferGetIOSurface(img);
			if (sf != NULL) {
				surf = IOSurfaceGetID(sf);
			}
			fourcc_text(fmt, fmt_text);
		}
		say("annibale: media: encinput node=%p input=%p sbuf=%p img=%zux%zu@%p fmt=%s/%08x surf=%u blk=%zu seq=%u\n",
		    self, input, sample_buffer,
		    img != NULL ? CVPixelBufferGetWidth(img) : 0,
		    img != NULL ? CVPixelBufferGetHeight(img) : 0, img,
		    fmt_text, fmt, surf, block != NULL ? CMBlockBufferGetDataLength(block) : 0, encinput_count);
	}
	if (original_photo_encoder_render != NULL) {
		((void (*)(id, SEL, CMSampleBufferRef, id))original_photo_encoder_render)(self, selector, sample_buffer, input);
	}
}

static int
ms_configure(id self, SEL selector, id *error)
{
	int err = ((int (*)(id, SEL, id *))original_ms_configure)(self, selector, error);
	say("annibale: media: -[BWMultiStreamCameraSourceNode configure:] -> %d\n", err);
	return err;
}

static int
ms_allocate_pools(id self, SEL selector, id node)
{
	int err = ((int (*)(id, SEL, id))original_ms_allocate_pools)(self, selector, node);
	say("annibale: media: -[BWMultiStreamCameraSourceNode allocateOrReuseBufferPoolsFromSourceNode:] -> %d\n", err);
	return err;
}

static void
ms_prepare(id self, SEL selector)
{
	say("annibale: media: -[BWMultiStreamCameraSourceNode prepareForCurrentConfigurationToBecomeLive] begin\n");
	((void (*)(id, SEL))original_ms_prepare)(self, selector);
	say("annibale: media: -[BWMultiStreamCameraSourceNode prepareForCurrentConfigurationToBecomeLive] done\n");
}

static BOOL
ms_start(id self, SEL selector, id *error)
{
	BOOL ok = ((BOOL (*)(id, SEL, id *))original_ms_start)(self, selector, error);
	say("annibale: media: -[BWMultiStreamCameraSourceNode start:] -> %d\n", (int)ok);
	return ok;
}

static void
emit_sample_buffer(id self, SEL selector, CMSampleBufferRef sample_buffer)
{
	static bool said;
	static unsigned emit_trace_count;

	if (sample_buffer == NULL) {
		if (!said) {
			said = true;
			say("annibale: mediaserverd: a node's output was given no sample buffer (the silent audio source's): not passed on\n");
		}
		return;
	}
	id node = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("node"));
	BOOL still = is_node(node, "BWStillImageCoordinatorNode") ||
	    is_node(node, "BWPhotonicEngineNode") || is_node(node, "BWStillImageFilterNode") ||
	    is_node(node, "BWStillImageTurnstileNode") || is_node(node, "BWStillImageScalerNode") ||
	    is_node(node, "BWPhotoEncoderNode");
	CVImageBufferRef img = CMSampleBufferGetImageBuffer(sample_buffer);
	/* The still graph's own contract: a node emits a buffer whose contents are complete.
	 * On our driver the engine's GPU write can still be in flight here (first shot dark),
	 * so wait for that surface's pending write before the handoff. */
	if (still && img != NULL) {
		wait_pending_writes(CVPixelBufferGetIOSurface(img), "emit");
	}
	if (emit_trace_count < 32) {
		if (still) {
			emit_trace_count++;
			CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sample_buffer);
			/* Still-capture triage: every tap delivers an auxiliary (RedEyeAuxiliaryImage
			 * in the metadata attachment) plus the photo; the first capture of each
			 * session saves dark. The flag on each emit shows which frame went where. */
			int redeye = -1;
			CFDictionaryRef atts = img != NULL ?
			    CVBufferCopyAttachments(img, kCVAttachmentMode_ShouldPropagate) : NULL;
			if (atts != NULL) {
				redeye = 0;
				CFIndex count = CFDictionaryGetCount(atts);
				const void **vals = count > 0 ? malloc((size_t)count * sizeof(void *)) : NULL;
				if (vals != NULL) {
					CFDictionaryGetKeysAndValues(atts, NULL, vals);
					for (CFIndex i = 0; i < count; i++) {
						if (vals[i] == NULL || CFGetTypeID(vals[i]) != CFDictionaryGetTypeID()) {
							continue;
						}
						if (CFDictionaryGetValue((CFDictionaryRef)vals[i],
						        CFSTR("RedEyeAuxiliaryImage")) != NULL) {
							redeye = 1;
							break;
						}
					}
					free(vals);
				}
				CFRelease(atts);
			}
			/* CPU-readable probe of the image's centre: stills arrive as Y~10 (aux) or
			 * Y~150 (photo). A dark emit here splits coordinator/engine/encoder. */
			unsigned surf = 0;
			IOSurfaceRef sf = img != NULL ? CVPixelBufferGetIOSurface(img) : NULL;
			if (sf != NULL) {
				surf = IOSurfaceGetID(sf);
			}
			unsigned lum = 0;
			int lum_ok = -1;
			if (img != NULL && CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess) {
				size_t w = CVPixelBufferGetWidth(img), h = CVPixelBufferGetHeight(img);
				size_t stride = CVPixelBufferGetBytesPerRow(img);
				const uint8_t *base = (const uint8_t *)CVPixelBufferGetBaseAddress(img);
				uint32_t fmt = CVPixelBufferGetPixelFormatType(img);
				if (base != NULL && stride > 0) {
					size_t y0 = h / 4, x0 = w / 4;
					const uint8_t *px = base + y0 * stride + x0;
					if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
					    fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
						for (unsigned i = 0; i < 64; i++)
							lum += px[(i % 8) * 97 + (i / 8) * stride];
						lum /= 64;
						lum_ok = 1;
					} else {  /* treat as 4 bytes per pixel (BGRA-ish) */
						for (unsigned i = 0; i < 64; i++)
							lum += px[((i % 8) * 97 + (i / 8) * stride) * 4 + 1];
						lum /= 64;
						lum_ok = 2;
					}
				}
				CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
			}
			say("annibale: media: emit %s img=%zux%zu@%p blk=%zu redeye=%d lum=%d:%u surf=%u seq=%u\n",
			    object_getClassName(node),
			    img != NULL ? CVPixelBufferGetWidth(img) : 0,
			    img != NULL ? CVPixelBufferGetHeight(img) : 0, img,
			    bb != NULL ? CMBlockBufferGetDataLength(bb) : 0,
			    redeye, lum_ok, lum, surf, emit_trace_count);
			emit_image_format(node, img, surf, emit_trace_count);
		}
	}
	time_photo_encoder(self, sample_buffer);
	((void (*)(id, SEL, CMSampleBufferRef))original_emit_sample_buffer)(self, selector, sample_buffer);
}

static void
emit_node_error(id self, __unused SEL selector, id nodeError)
{
	id node = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("node"));
	id connection = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("connection"));
	id input = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("input")) : nil;
	SEL output_sel = sel_registerName("output");
	BOOL has_output = connection != nil && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(connection,
	    sel_registerName("respondsToSelector:"), output_sel);
	id output_end = has_output ? ((id (*)(id, SEL))objc_msgSend)(connection, output_sel) : self;
	id out_node = output_end != nil ? ((id (*)(id, SEL))objc_msgSend)(output_end, sel_registerName("node")) : node;
	id in_node = input != nil ? ((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("node")) : nil;
	int code = 0;

	if (out_node == nil) {
		out_node = node;
	}
	if (nodeError != nil &&
	    ((BOOL (*)(id, SEL, SEL))objc_msgSend)(nodeError, sel_registerName("respondsToSelector:"),
	        sel_registerName("errorCode"))) {
		code = ((int (*)(id, SEL))objc_msgSend)(nodeError, sel_registerName("errorCode"));
	}
	say("annibale: media: emitNodeError node=%s err=%p code=%d dir=%s@%p->%s@%p out=%p conn=%p in=%p\n",
	    node != nil ? object_getClassName(node) : "nil", nodeError, code,
	    out_node != nil ? object_getClassName(out_node) : "nil", out_node,
	    in_node != nil ? object_getClassName(in_node) : "nil", in_node, self, connection, input);
	if (original_emit_node_error != NULL) {
		((void (*)(id, SEL, id))original_emit_node_error)(self, selector, nodeError);
	}
}

/*
 * Register Apple's software HEVC encoder in VCPHEVC.videocodec with VideoToolbox so CMPhoto can encode HEIC.
 */
static void
register_hevc_encoder(void)
{
	void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *create = codec != NULL ? dlsym(codec, "HEVCVideoEncoder_CreateInstance") : NULL;
	int (*register_encoder)(uint32_t, CFDictionaryRef, void *) = vt != NULL ? dlsym(vt, "VTRegisterVideoEncoderWithInfo") : NULL;
	CFStringRef *id_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderID") : NULL;
	CFStringRef *name_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderName") : NULL;
	CFStringRef *hardware_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated") : NULL;
	int status;

	if (create == NULL || register_encoder == NULL || id_key == NULL || name_key == NULL || hardware_key == NULL) {
		say("annibale: mediaserverd: no software HEVC encoder to register (%s)\n", codec == NULL ? "no VCPHEVC.videocodec" :
		    create == NULL ? "no HEVCVideoEncoder_CreateInstance" : "VideoToolbox without VTRegisterVideoEncoderWithInfo");
		return;
	}
	const void *keys[] = { *id_key, *name_key, *hardware_key };
	const void *values[] = { CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
		kCFBooleanFalse };
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);

	status = register_encoder('hvc1', info, create);
	CFRelease(info);
	say("annibale: mediaserverd: the software HEVC encoder of VCPHEVC.videocodec registered for 'hvc1' (%d)\n", status);
}

struct method_spec {
	const char *selector, *types;
	IMP imp;
};

static Class
make_class(const char *name, const struct method_spec *methods, size_t count)
{
	Class cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);

	for (size_t i = 0; i < count; i++) {
		class_addMethod(cls, sel_registerName(methods[i].selector), methods[i].imp, methods[i].types);
	}
	objc_registerClassPair(cls);
	return cls;
}

static void
flashlight_classes(void)
{
	static const struct method_spec vendor[] = {
		{ "registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
		  "deviceSharingWithOtherClientsAllowed:deviceAvailabilityChangedHandler:", "i@:i@iBB@", (IMP)flashlight_register },
		{ "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:", "@@:iB^i", (IMP)flashlight_copy_device },
		{ "copyStreamForFlashlightWithPosition:deviceType:forDevice:", "@@:ii@", (IMP)flashlight_copy_stream },
		{ "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:", "v@:@iB", (IMP)flashlight_take_back },
		{ "activeDeviceEquals:", "B@:@", (IMP)flashlight_active_equals },
	}, stream[] = {
		{ "supportedProperties", "@@:", (IMP)flashlight_stream_properties },
		{ "setProperty:value:", "i@:@@", (IMP)flashlight_stream_set },
	};

	vendor_class = make_class("AnnibaleFlashlightVendor", vendor, sizeof(vendor) / sizeof(vendor[0]));
	device_class = make_class("AnnibaleFlashlightDevice", NULL, 0);
	stream_class = make_class("AnnibaleFlashlightStream", stream, sizeof(stream) / sizeof(stream[0]));
}

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the code in iOS 16.5 (20F66); on iOS 26 (arm64e), methods are matched by selector via the
 * Objective-C runtime without requiring the iOS 16.5 unslid address.
 */
static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement, *original;
	bool instance;                  /* an instance method, not a class method */
	bool done;
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0x1a2115c0cUL,
	  (IMP)prewarm_portrait_filter, NULL, false, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:", 0UL,
	  (IMP)shared_vendor_with_func, &original_shared_vendor_with_func, false, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor", 0x1a1ef0d04UL,
	  (IMP)shared_vendor, &original_shared_vendor, false, false },
	{ "BWAudioSourceNode", "start:", 0x1a1eefa18UL, (IMP)audio_source_start, &original_audio_source_start, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", 0x1a1e99ef8UL,
	  (IMP)video_stabilization_method, NULL, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationType", 0x1a20d8f48UL,
	  (IMP)video_stabilization_type, NULL, true, false },
	{ "FigCaptureStillImageSettings", "qualityPrioritization", 0x1a1f533dcUL,
	  (IMP)quality_prioritization, &original_quality_prioritization, true, false },
	{ "BWGNRNode", "renderSampleBuffer:forInput:", 0x1a1ec83b8UL, (IMP)gnr_render, NULL, true, false },
	{ "BWNodeOutput", "emitSampleBuffer:", 0x1a1ea37e0UL, (IMP)emit_sample_buffer, &original_emit_sample_buffer, true, false },
	{ "BWNodeOutput", "emitNodeError:", 0UL, (IMP)emit_node_error, &original_emit_node_error, true, false },
	{ "BWPhotoEncoderNode", "renderSampleBuffer:forInput:", 0UL, (IMP)photo_encoder_render,
	  &original_photo_encoder_render, true, false },
	{ "BWMultiStreamCameraSourceNode", "configure:", 0UL, (IMP)ms_configure, &original_ms_configure, true, false },
	{ "BWMultiStreamCameraSourceNode", "allocateOrReuseBufferPoolsFromSourceNode:", 0UL, (IMP)ms_allocate_pools,
	  &original_ms_allocate_pools, true, false },
	{ "BWMultiStreamCameraSourceNode", "prepareForCurrentConfigurationToBecomeLive", 0UL, (IMP)ms_prepare, &original_ms_prepare, true, false },
	{ "BWMultiStreamCameraSourceNode", "start:", 0UL, (IMP)ms_start, &original_ms_start, true, false },
};

/* ---- the Metal device leaves this process on iOS 16.5 when cvadepth-iOS.metallib is absent -------- */

#define MTL_DEVICE_ARRAY        0x1e1efe9a0UL   /* Metal's static NSMutableArray *_deviceArray (20F66) */

extern id objc_retain(id object);

static bool metal_left_alone;                   /* boot-arg mediamtl=1, or iOS 26 */

static bool
boot_arg(const char *what)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	return sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    strstr(arguments, what) != NULL;
}

static void
keep_metal_from_the_capture_stack(void)
{
	static bool done;
	unsigned long (*ask)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	void (*tell)(id, SEL) = (void (*)(id, SEL))objc_msgSend;
	id *slot = (id *)(MTL_DEVICE_ARRAY + slide);
	id (*create)(void);
	id device, array;
	unsigned long held;

	if (done || metal_left_alone) {
		return;
	}
	if ((create = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice")) == NULL) {
		return;
	}
	done = true;
	if ((device = create()) == NULL) {
		say("annibale: mediaserverd: no Metal device in this process, so the capture stack's shader "
		    "preload has nothing to ask\n");
		return;
	}
	if ((array = *slot) == NULL) {
		say("annibale: mediaserverd: a device, but nothing at %#lx; the device list is NOT emptied\n",
		    MTL_DEVICE_ARRAY);
		return;
	}
	if (!((BOOL (*)(id, SEL, Class))objc_msgSend)(array, sel_registerName("isKindOfClass:"),
	    objc_getClass("NSMutableArray"))) {
		say("annibale: mediaserverd: %#lx holds a %s, not Metal's device array; left alone\n",
		    MTL_DEVICE_ARRAY, object_getClassName(array));
		return;
	}
	held = ask(array, sel_registerName("count"));
	objc_retain(device);
	tell(array, sel_registerName("removeAllObjects"));
	say("annibale: mediaserverd: %s taken out of the capture stack (%lu in the list, %lu now); "
	    "MTLCreateSystemDefaultDevice() = %s\n", object_getClassName(device), held,
	    ask(array, sel_registerName("count")), create() != NULL ? "A DEVICE STILL" : "nil");
}

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	bool is_ios26 = false;
#if __has_feature(ptrauth_calls)
	is_ios26 = true;
#else
	if (objc_getClass("BWFigCaptureDevice") != NULL) {
		is_ios26 = true;
	}
#endif
	if (!is_ios26) {
		keep_metal_from_the_capture_stack();
	}
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		method = r->instance ? class_getInstanceMethod(cls, sel_registerName(r->selector)) :
		    class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		if (original == NULL || (!is_ios26 && r->expected != 0 && STRIP_FN(original) != r->expected + slide)) {
			if (r->expected != 0) {
				r->done = true;
				say("annibale: %c[%s %s]: found %#lx, expected %#lx; left alone\n", r->instance ? '-' : '+',
				    r->class_name, r->selector, original ? STRIP_FN(original) - slide : 0, r->expected);
			}
			continue;
		}
		r->done = true;
		if (r->original != NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("annibale: media: %c[%s %s] replaced\n", r->instance ? '-' : '+', r->class_name, r->selector);
	}
}

/*
 * CoreVideo registers Apple universal/HTPC compressed pixel formats only when the
 * hardware DeviceTree advertises buffer-compression, whereas CMCapture hardcodes
 * FigCapturePlatformSupports*Compression() to 1 for the V57 platform ID. When
 * CoreVideo has no compressed pixel format descriptions, align CMCapture's
 * platform capability flags to 0 so BWPixelBufferPool negotiates uncompressed
 * formats instead of failing with kCVReturnInvalidPixelFormat (-6680).
 */
static int
clear_cmcapture_compression_flag(const char *sym)
{
	typedef uint8_t (*cap_fn_t)(void);
	void *raw = dlsym(RTLD_DEFAULT, sym);
	if (raw == NULL) {
		return -1;
	}
	cap_fn_t fn = (cap_fn_t)raw;
	if (fn() == 0) {
		return 0;
	}
	uintptr_t pc = STRIP_FN(raw) + 0x10u;
	const uint32_t *insns = (const uint32_t *)pc;
	uint32_t i_adrp = insns[0];
	uint32_t i_ldrb = insns[1];
	uint32_t i_ret = insns[2];
	if ((i_adrp & 0x9f00001fu) != 0x90000008u ||
	    (i_ldrb & 0xffc003ffu) != 0x39400100u ||
	    i_ret != 0xd65f03c0u) {
		return -2;
	}
	int64_t imm21 = (int64_t)(((i_adrp >> 5) & 0x7ffffu) << 2 | ((i_adrp >> 29) & 0x3u));
	imm21 = (imm21 << 43) >> 43;
	uint32_t imm12 = (i_ldrb >> 10) & 0xfffu;
	uintptr_t addr = (pc & ~(uintptr_t)0xfffu) + (uintptr_t)((uint64_t)imm21 << 12) + imm12;
	*(volatile uint8_t *)addr = 0;
	return fn() == 0 ? 1 : -3;
}

static void
align_cmcapture_compression_caps(void)
{
	CFDictionaryRef d10 = CVPixelFormatDescriptionCreateWithPixelFormatType(
	    kCFAllocatorDefault, 0x26786632u /* '&xf2' */);
	if (d10 != NULL) {
		CFRelease(d10);
		return;
	}
	int r_univ = clear_cmcapture_compression_flag("FigCapturePlatformSupportsUniversalCompression");
	int r_lossy = clear_cmcapture_compression_flag("FigCapturePlatformSupportsUniversalLossyCompression");
	int r_h16 = clear_cmcapture_compression_flag("FigCapturePlatformSupportsHTPC16x8Compression");
	int r_h32 = clear_cmcapture_compression_flag("FigCapturePlatformSupportsHTPC32x4Compression");
	say("annibale: media: aligned CMCapture compression caps (univ=%d lossy=%d htpc16=%d htpc32=%d)\n",
	    r_univ, r_lossy, r_h16, r_h32);
}

__attribute__((constructor))
static void
annibale_media_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name == NULL || (strcmp(name, "mediaserverd") != 0 &&
	    strcmp(name, "cameracaptured") != 0 &&
	    strcmp(name, "deferredmediad") != 0)) {
		return;
	}
	if (strcmp(name, "deferredmediad") == 0) {
		register_hevc_encoder();
		(void)dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture", RTLD_LAZY | RTLD_GLOBAL);
		align_cmcapture_compression_caps();
		sysctlbyname("kern.annibale.say", NULL, NULL,
		    "media: loaded in deferredmediad\n", sizeof("media: loaded in deferredmediad\n"));
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
#if __has_feature(ptrauth_calls)
	metal_left_alone = !boot_arg("mediamtl=0");
#else
	metal_left_alone = boot_arg("mediamtl=1") || strcmp(name, "cameracaptured") == 0;
#endif
	if (!metal_left_alone) {
		say("annibale: %s: the Metal device leaves this process as soon as Metal is in "
		    "it (mediamtl=1 leaves it there)\n", name);
	}
	torch = open("/dev/annibale-torch", O_WRONLY);
	camera = access("/dev/annibale-camera", F_OK) == 0;
	if (torch >= 0) {
		flashlight_classes();
		say("annibale: %s: the kernel has a flashlight (/dev/annibale-torch)\n", name);
	}
	if (camera) {
		say("annibale: %s: the kernel has a camera (/dev/annibale-camera)\n", name);
		register_hevc_encoder();
	}
	/* On iOS 26, cameracaptured loads CMCapture on demand or via dyld shared cache; ensure CMCapture is loaded so hooks attach immediately */
	if (strcmp(name, "cameracaptured") == 0) {
		unlink("/private/var/mobile/Library/Preferences/com.apple.cameracapture.volatile.plist");
		unlink("/private/var/root/Library/Preferences/com.apple.cameracapture.volatile.plist");
		unlink("/private/var/preferences/com.apple.cameracapture.volatile.plist");
		CFPreferencesSetValue(CFSTR("CaptureSourceInfo"), NULL, CFSTR("com.apple.cameracapture.volatile"),
		    kCFPreferencesCurrentUser, kCFPreferencesCurrentHost);
		CFPreferencesSynchronize(CFSTR("com.apple.cameracapture.volatile"),
		    kCFPreferencesCurrentUser, kCFPreferencesCurrentHost);
		const char *fn = getenv("ANNIBALE_FIG_NOTES");
		bool want_notes = (fn != NULL && fn[0] != '\0' && strcmp(fn, "0") != 0) ||
		    access("/private/var/tmp/fig_notes", F_OK) == 0;
		CFPreferencesSetAppValue(CFSTR("fig_notes"), want_notes ? CFSTR("stderr") : NULL,
		    CFSTR("com.apple.coremedia"));
		CFPreferencesAppSynchronize(CFSTR("com.apple.coremedia"));
		(void)dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture", RTLD_LAZY | RTLD_GLOBAL);
		align_cmcapture_compression_caps();
	}
	front_cam_rotation_watch_start();
	{
		Dl_info info;
		char uuid_str[40] = "none", line[160];

		if (dladdr((const void *)&annibale_media_init, &info) && info.dli_fbase) {
			const struct mach_header_64 *mh = (const struct mach_header_64 *)info.dli_fbase;
			if (mh->magic == MH_MAGIC_64) {
				const struct load_command *lc = (const struct load_command *)(mh + 1);
				for (uint32_t i = 0; i < mh->ncmds; i++) {
					if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
						const uint8_t *u = ((const struct uuid_command *)lc)->uuid;
						snprintf(uuid_str, sizeof(uuid_str),
						    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
						    u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
						    u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
						break;
					}
					lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
				}
			}
		}
		snprintf(line, sizeof(line), "media: loaded in %s uuid=%s camera=%d torch=%d\n",
		    name, uuid_str, camera, torch >= 0);
		sysctlbyname("kern.annibale.say", NULL, NULL, line, strlen(line) + 1);
	}
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}
