/*
 * VideoToolbox Hardware Decode Plugin for SM8750 Iris VPU v3.5
 * Registers hardware-accelerated decoders for H.264 (avc1), HEVC (hvc1), and VP9 (vp09).
 * Bridges decoded NV12 ('420v') frames to CoreMedia / CoreVideo and emits frames via
 * VideoToolbox decompression session callbacks.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define say(...) iris_plugin_log("iris_vt: " __VA_ARGS__)

static void
iris_plugin_log(const char *format, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		write(2, line, (size_t)n);
	}
}

/* FourCC Codec Types */
#define CODEC_TYPE_AVC1         0x61766331u /* 'avc1' */
#define CODEC_TYPE_HVC1         0x68766331u /* 'hvc1' */
#define CODEC_TYPE_VP09         0x76703039u /* 'vp09' */

/* Driver IOCTL definitions matching annibale_iris.c */
struct iris_session_params {
	uint32_t codec_type;    /* 1: H.264, 2: HEVC, 3: VP9 */
	uint32_t width;
	uint32_t height;
	uint32_t session_id;
};

struct iris_decode_frame {
	uint32_t session_id;
	uint64_t input_user_addr;
	uint32_t input_len;
	uint64_t out_user_y;    /* In: 用户态 CVPixelBuffer 平面 0 (Y) 目标地址 */
	uint64_t out_user_uv;   /* In: 用户态 CVPixelBuffer 平面 1 (UV) 目标地址 */
	uint64_t out_phys_y;    /* Out: 硬件解码平面 0 物理基址 */
	uint64_t out_phys_uv;   /* Out: 硬件解码平面 1 物理基址 */
	uint32_t out_stride;    /* In/Out: 跨距 (对齐至 128 字节) */
	uint32_t flags;         /* Out: 状态标志位 (0x01: 成功) */
};

#define IRIS_IOC_OPEN_SESSION   _IOWR('V', 1, struct iris_session_params)
#define IRIS_IOC_DECODE_FRAME   _IOWR('V', 2, struct iris_decode_frame)
#define IRIS_IOC_CLOSE_SESSION  _IOW('V', 3, uint32_t)

#define IRIS_DEV_PATH           "/dev/annibale-iris"

/* VideoToolbox SPI Declarations */
typedef OSStatus (*VTRegisterVideoDecoderWithInfoFunc)(
	uint32_t codecType,
	CFDictionaryRef decoderInfo,
	void *createInstanceCallback
);

typedef void (*VTDecoderSessionEmitDecodedFrameFunc)(
	void *session,
	CVPixelBufferRef pixelBuffer,
	CMTime pts,
	CMTime duration,
	uint32_t flags
);

static VTRegisterVideoDecoderWithInfoFunc g_VTRegisterVideoDecoderWithInfo = NULL;
static VTDecoderSessionEmitDecodedFrameFunc g_VTDecoderSessionEmitDecodedFrame = NULL;

static CFStringRef *g_kVTVideoDecoderList_DecoderID = NULL;
static CFStringRef *g_kVTVideoDecoderList_DecoderName = NULL;
static CFStringRef *g_kVTVideoDecoderList_CodecType = NULL;
static CFStringRef *g_kVTVideoDecoderList_IsHardwareAccelerated = NULL;
static CFStringRef *g_kVTVideoDecoder_IsHardwareAccelerated = NULL;

/* Iris Decoder Session Context */
typedef struct IrisDecoderSession {
	uint32_t codec_type;
	uint32_t width;
	uint32_t height;
	int dev_fd;
	uint32_t session_id;
	void *session_ref;
	CVPixelBufferPoolRef pool;
	pthread_mutex_t lock;
	bool invalid;
} IrisDecoderSession;

/* VideoToolbox Decoder Instance Callbacks */

static OSStatus
iris_decoder_decode_frame(void *decoderInstance, CMSampleBufferRef sampleBuffer, uint32_t flags __unused)
{
	IrisDecoderSession *session = (IrisDecoderSession *)decoderInstance;
	if (session == NULL || session->invalid) {
		return -12905; /* kVTInvalidSessionErr */
	}

	pthread_mutex_lock(&session->lock);

	CMTime pts = kCMTimeInvalid;
	CMTime duration = kCMTimeInvalid;
	if (sampleBuffer != NULL) {
		pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
		duration = CMSampleBufferGetDuration(sampleBuffer);

		/* Extract dimensions dynamically from CMFormatDescriptionRef */
		CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
		if (formatDesc != NULL) {
			CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(formatDesc);
			if (dims.width > 0 && dims.height > 0) {
				session->width = (uint32_t)dims.width;
				session->height = (uint32_t)dims.height;
			}
		}
	}

	/* Extract bitstream data */
	CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
	size_t bitstream_len = 0;
	char *bitstream_ptr = NULL;
	if (blockBuffer != NULL) {
		(void)CMBlockBufferGetDataPointer(blockBuffer, 0, NULL, &bitstream_len, &bitstream_ptr);
	}

	/* Create or allocate CVPixelBuffer in NV12 ('420v') format */
	CVPixelBufferRef pixelBuffer = NULL;
	uint32_t w = session->width ? session->width : 1920;
	uint32_t h = session->height ? session->height : 1080;

	CFMutableDictionaryRef ioProps = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (ioProps != NULL) {
		CFDictionarySetValue(ioProps, kIOSurfaceIsGlobal, kCFBooleanTrue);
	}

	CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 4,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	int format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	int iw = (int)w;
	int ih = (int)h;
	CFNumberRef cfFormat = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &format);
	CFNumberRef cfW = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &iw);
	CFNumberRef cfH = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ih);

	if (attrs != NULL) {
		if (cfFormat) CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, cfFormat);
		if (cfW) CFDictionarySetValue(attrs, kCVPixelBufferWidthKey, cfW);
		if (cfH) CFDictionarySetValue(attrs, kCVPixelBufferHeightKey, cfH);
		if (ioProps) CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, ioProps);
	}

	CVReturn cvret = CVPixelBufferCreate(
		kCFAllocatorDefault,
		w,
		h,
		kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
		attrs,
		&pixelBuffer
	);

	if (cfFormat) CFRelease(cfFormat);
	if (cfW) CFRelease(cfW);
	if (cfH) CFRelease(cfH);
	if (ioProps) CFRelease(ioProps);
	if (attrs) CFRelease(attrs);

	if (cvret != kCVReturnSuccess || pixelBuffer == NULL) {
		pthread_mutex_unlock(&session->lock);
		return -12911; /* kVTVideoDecoderMalfunctionErr */
	}

	/* Lock CVPixelBuffer and map plane pointers */
	if (CVPixelBufferLockBaseAddress(pixelBuffer, 0) != kCVReturnSuccess) {
		CVPixelBufferRelease(pixelBuffer);
		pthread_mutex_unlock(&session->lock);
		return -12911; /* kVTVideoDecoderMalfunctionErr */
	}

	void *y_base = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
	void *uv_base = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);
	size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);

	/* Submit frame to hardware driver via IOCTL */
	struct iris_decode_frame df;
	bzero(&df, sizeof(df));
	df.session_id = session->session_id;
	df.input_user_addr = (uint64_t)(uintptr_t)bitstream_ptr;
	df.input_len = (uint32_t)bitstream_len;
	df.out_user_y = (uint64_t)(uintptr_t)y_base;
	df.out_user_uv = (uint64_t)(uintptr_t)uv_base;
	df.out_stride = (uint32_t)y_stride;

	if (session->dev_fd < 0) {
		CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
		CVPixelBufferRelease(pixelBuffer);
		pthread_mutex_unlock(&session->lock);
		return -12905; /* kVTInvalidSessionErr */
	}

	if (ioctl(session->dev_fd, IRIS_IOC_DECODE_FRAME, &df) < 0) {
		int err = errno;
		say("IRIS_IOC_DECODE_FRAME failed: %d\n", err);
		CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
		CVPixelBufferRelease(pixelBuffer);
		pthread_mutex_unlock(&session->lock);
		if (err == EBUSY || err == EAGAIN) {
			return -12902; /* kVTVideoDecoderNotAvailableNowErr */
		} else if (err == EINVAL) {
			return -12909; /* kVTVideoDecoderBadDataErr */
		}
		return -12911; /* kVTVideoDecoderMalfunctionErr */
	}

	CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

	if (!(df.flags & 0x01) || df.out_phys_y == 0) {
		/* No valid frame decoded, do not emit empty buffer */
		CVPixelBufferRelease(pixelBuffer);
		pthread_mutex_unlock(&session->lock);
		return noErr;
	}

	/* Emit frame to CoreMedia player */
	if (g_VTDecoderSessionEmitDecodedFrame != NULL && session->session_ref != NULL) {
		g_VTDecoderSessionEmitDecodedFrame(session->session_ref, pixelBuffer, pts, duration, 0);
	}
	CVPixelBufferRelease(pixelBuffer);

	pthread_mutex_unlock(&session->lock);
	return noErr;
}

static OSStatus
iris_decoder_finish_delayed_frames(void *decoderInstance)
{
	IrisDecoderSession *session = (IrisDecoderSession *)decoderInstance;
	if (session == NULL) {
		return noErr;
	}
	say("flushing delayed frames\n");
	return noErr;
}

static OSStatus
iris_decoder_invalidate(void *decoderInstance)
{
	IrisDecoderSession *session = (IrisDecoderSession *)decoderInstance;
	if (session == NULL) {
		return noErr;
	}
	pthread_mutex_lock(&session->lock);
	session->invalid = true;
	if (session->dev_fd >= 0) {
		(void)ioctl(session->dev_fd, IRIS_IOC_CLOSE_SESSION, &session->session_id);
		close(session->dev_fd);
		session->dev_fd = -1;
	}
	if (session->pool != NULL) {
		CVPixelBufferPoolRelease(session->pool);
		session->pool = NULL;
	}
	pthread_mutex_unlock(&session->lock);
	pthread_mutex_destroy(&session->lock);
	free(session);
	return noErr;
}

/* Dispatch method table layout */
typedef struct IrisDecoderCallbacks {
	uint32_t version;
	OSStatus (*DecodeFrame)(void *, CMSampleBufferRef, uint32_t);
	OSStatus (*FinishDelayedFrames)(void *);
	OSStatus (*Invalidate)(void *);
} IrisDecoderCallbacks;

static const IrisDecoderCallbacks g_IrisCallbacks = {
	.version = 1,
	.DecodeFrame = iris_decoder_decode_frame,
	.FinishDelayedFrames = iris_decoder_finish_delayed_frames,
	.Invalidate = iris_decoder_invalidate,
};

static OSStatus
iris_decoder_create_instance(
	uint32_t codecType,
	CFDictionaryRef decoderSpecification __unused,
	void *decompressionSession,
	void **decoderInstanceOut
)
{
	say("creating hardware decoder instance for codec %#x\n", codecType);

	IrisDecoderSession *session = (IrisDecoderSession *)calloc(1, sizeof(IrisDecoderSession));
	if (session == NULL) {
		return -12906; /* kVTAllocationFailedErr */
	}

	session->codec_type = codecType;
	session->width = 1920;
	session->height = 1080;
	session->session_ref = decompressionSession;
	pthread_mutex_init(&session->lock, NULL);

	session->dev_fd = open(IRIS_DEV_PATH, O_RDWR);
	if (session->dev_fd >= 0) {
		struct iris_session_params sp;
		bzero(&sp, sizeof(sp));
		sp.codec_type = (codecType == CODEC_TYPE_HVC1) ? 2 : ((codecType == CODEC_TYPE_VP09) ? 3 : 1);
		sp.width = session->width;
		sp.height = session->height;
		if (ioctl(session->dev_fd, IRIS_IOC_OPEN_SESSION, &sp) == 0) {
			session->session_id = sp.session_id;
		}
	} else {
		say("warning: failed to open %s: %d\n", IRIS_DEV_PATH, errno);
	}

	*decoderInstanceOut = session;
	return noErr;
}

/* Resolve VideoToolbox symbols dynamically */
static bool
resolve_vt_symbols(void)
{
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	if (vt == NULL) {
		say("failed to dlopen VideoToolbox.framework\n");
		return false;
	}

	g_VTRegisterVideoDecoderWithInfo = (VTRegisterVideoDecoderWithInfoFunc)dlsym(vt, "VTRegisterVideoDecoderWithInfo");
	g_VTDecoderSessionEmitDecodedFrame = (VTDecoderSessionEmitDecodedFrameFunc)dlsym(vt, "VTDecoderSessionEmitDecodedFrame");

	g_kVTVideoDecoderList_DecoderID = (CFStringRef *)dlsym(vt, "kVTVideoDecoderList_DecoderID");
	g_kVTVideoDecoderList_DecoderName = (CFStringRef *)dlsym(vt, "kVTVideoDecoderList_DecoderName");
	g_kVTVideoDecoderList_CodecType = (CFStringRef *)dlsym(vt, "kVTVideoDecoderList_CodecType");
	g_kVTVideoDecoderList_IsHardwareAccelerated = (CFStringRef *)dlsym(vt, "kVTVideoDecoderList_IsHardwareAccelerated");
	g_kVTVideoDecoder_IsHardwareAccelerated = (CFStringRef *)dlsym(vt, "kVTVideoDecoder_IsHardwareAccelerated");

	if (g_VTRegisterVideoDecoderWithInfo == NULL) {
		say("warning: VTRegisterVideoDecoderWithInfo symbol not found in VideoToolbox\n");
		return false;
	}
	return true;
}

/* Register codec helper */
static void
register_codec(uint32_t codec, const char *id_str, const char *name_str)
{
	CFStringRef cf_id = CFStringCreateWithCString(kCFAllocatorDefault, id_str, kCFStringEncodingUTF8);
	CFStringRef cf_name = CFStringCreateWithCString(kCFAllocatorDefault, name_str, kCFStringEncodingUTF8);

	const void *keys[3];
	const void *values[3];
	int count = 0;

	if (g_kVTVideoDecoderList_DecoderID != NULL) {
		keys[count] = *g_kVTVideoDecoderList_DecoderID;
		values[count++] = cf_id;
	}
	if (g_kVTVideoDecoderList_DecoderName != NULL) {
		keys[count] = *g_kVTVideoDecoderList_DecoderName;
		values[count++] = cf_name;
	}
	if (g_kVTVideoDecoder_IsHardwareAccelerated != NULL) {
		keys[count] = *g_kVTVideoDecoder_IsHardwareAccelerated;
		values[count++] = kCFBooleanTrue;
	}

	CFDictionaryRef info = CFDictionaryCreate(
		kCFAllocatorDefault,
		keys,
		values,
		count,
		&kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks
	);

	OSStatus status = g_VTRegisterVideoDecoderWithInfo(codec, info, (void *)iris_decoder_create_instance);
	say("registered hardware decoder for '%c%c%c%c' (%s): status %d\n",
	    (codec >> 24) & 0xff, (codec >> 16) & 0xff, (codec >> 8) & 0xff, codec & 0xff,
	    name_str, (int)status);

	CFRelease(info);
	CFRelease(cf_id);
	CFRelease(cf_name);
}

void
register_iris_videotoolbox_decoders(void)
{
	static bool done = false;
	if (done) {
		return;
	}
	done = true;

	say("initializing SM8750 Iris VPU v3.5 VideoToolbox hardware decode plugin...\n");

	if (!resolve_vt_symbols()) {
		say("skipping hardware decoder registration due to missing VideoToolbox symbols\n");
		return;
	}

	/* Register H.264 (avc1), HEVC (hvc1), and VP9 (vp09) */
	register_codec(CODEC_TYPE_AVC1, "com.apple.videotoolbox.videodecoder.iris.h264", "Iris H.264 Hardware Decoder");
	register_codec(CODEC_TYPE_HVC1, "com.apple.videotoolbox.videodecoder.iris.hevc", "Iris HEVC Hardware Decoder");
	register_codec(CODEC_TYPE_VP09, "com.apple.videotoolbox.videodecoder.iris.vp9",  "Iris VP9 Hardware Decoder");

	say("all hardware decoders registered with kVTVideoDecoder_IsHardwareAccelerated: true\n");
}

/* Constructor runs automatically when library is loaded into videocodecd or WebKit GPU process */
__attribute__((constructor))
static void
iris_plugin_init(void)
{
	register_iris_videotoolbox_decoders();
}
