/*
 * iOS 26 VideoToolbox hardware decoder plugin for SM8750 Iris VPU v3.5 (/dev/annibale-iris).
 * Probes IRIS_IOC_GET_INFO at load time and registers 'avc1', 'hvc1', and 'hev1' only when
 * IRIS_INFO_HW_READY is set; otherwise leaves Apple's software decoders untouched.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef _SYS_ANNIBALE_IRIS_H_
#define _SYS_ANNIBALE_IRIS_H_
#define IRIS_ABI_VERSION        1u
#define IRIS_CODEC_H264         1u
#define IRIS_CODEC_HEVC         2u
#define IRIS_CODEC_MASK_H264    (1u << IRIS_CODEC_H264)
#define IRIS_CODEC_MASK_HEVC    (1u << IRIS_CODEC_HEVC)
#define IRIS_INFO_DEV_PRESENT   0x01u
#define IRIS_INFO_FW_PRESENT    0x02u
#define IRIS_INFO_HW_READY      0x04u
#define IRIS_FRAME_DECODED      0x01u
#define IRIS_FRAME_NEED_MORE    0x04u
#define IRIS_FRAME_DRAIN        0x08u
#define IRIS_FRAME_POLL         0x10u

struct iris_device_info {
	uint32_t version;
	uint32_t flags;
	uint32_t codecs_mask;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t fw_size;
};

struct iris_session_params {
	uint32_t codec_type;
	uint32_t width;
	uint32_t height;
	uint32_t session_id;
};

struct iris_decode_frame {
	uint32_t session_id;
	uint32_t input_len;
	uint64_t input_user_addr;
	uint64_t in_tag;
	uint64_t out_tag;
	uint64_t out_user_y;
	uint64_t out_user_uv;
	uint32_t out_stride;
	uint32_t flags;
};

#define IRIS_IOC_OPEN_SESSION   _IOWR('V', 1, struct iris_session_params)
#define IRIS_IOC_DECODE_FRAME   _IOWR('V', 2, struct iris_decode_frame)
#define IRIS_IOC_CLOSE_SESSION  _IOW('V', 3, uint32_t)
#define IRIS_IOC_GET_INFO       _IOR('V', 4, struct iris_device_info)
#define IRIS_IOC_FLUSH_SESSION  _IOW('V', 5, uint32_t)
#endif /* _SYS_ANNIBALE_IRIS_H_ */

#ifndef IRIS_VT_HOST_TEST
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

/* VT passes a borrowed context handle and a pointer-sized integer frame token.
 * Neither has a CF object header; return each frame token unchanged to VT.
 */
typedef struct OpaqueVTDecoderSession *VTDecoderSessionRef;
typedef void *VTDecodeFrameRef;

extern const CFStringRef kVTVideoDecoder_IsHardwareAccelerated;
extern const CFStringRef kVTVideoDecoderInfo_Rating;

extern CFTypeID VTVideoDecoderGetClassID(void);
extern OSStatus CMDerivedObjectCreate(CFAllocatorRef allocator, const void *vtable, CFTypeID classID, CFTypeRef *outObject);
extern void *CMBaseObjectGetDerivedStorage(CFTypeRef obj);
extern OSStatus VTRegisterVideoDecoderWithInfo(CMVideoCodecType codecType, CFDictionaryRef decoderInfo,
    OSStatus (*createInstance)(CMVideoCodecType codecType, CFAllocatorRef allocator, CFTypeRef *outDecoder));
extern OSStatus VTDecoderSessionSetPixelBufferAttributes(VTDecoderSessionRef session, CFDictionaryRef attributes);
extern OSStatus VTDecoderSessionCreatePixelBuffer(VTDecoderSessionRef session, VTDecodeFrameRef frameRef, CVPixelBufferRef *outPixelBuffer);
extern OSStatus VTDecoderSessionEmitDecodedFrame(VTDecoderSessionRef session, VTDecodeFrameRef frameRef,
    OSStatus status, VTDecodeInfoFlags infoFlags, CVImageBufferRef imageBuffer);

static int
iris_sys_open(const char *path, int flags)
{
	return open(path, flags);
}

static int
iris_sys_close(int fd)
{
	return close(fd);
}

static int
iris_sys_ioctl(int fd, unsigned long cmd, void *arg)
{
	return ioctl(fd, cmd, arg);
}
#endif /* !IRIS_VT_HOST_TEST */

#if defined(__has_feature) && __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define IRIS_PAC_DA(disc) __ptrauth(ptrauth_key_asda, 1, disc)
#define IRIS_PAC_IA(disc) __ptrauth(ptrauth_key_asia, 1, disc)
#else
#define IRIS_PAC_DA(disc)
#define IRIS_PAC_IA(disc)
#endif

#ifndef paramErr
#define paramErr                        ((OSStatus)-50)
#endif
#ifndef memFullErr
#define memFullErr                      ((OSStatus)-108)
#endif
#define IRIS_CM_PROP_NOT_FOUND          ((OSStatus)-12784)
#define IRIS_CM_PROP_NOT_SUPPORTED      ((OSStatus)-12787)
#define IRIS_VT_PENDING_MAX             32u
#define IRIS_VT_PARAM_CAP               16384u
#define IRIS_VT_BITSTREAM_CAP           (4u * 1024u * 1024u)

struct iris_cm_base_class {
	size_t version;
	size_t derived_storage_size;
	OSStatus (* IRIS_PAC_IA(0x201c) initialize)(CFTypeRef obj);
	OSStatus (* IRIS_PAC_IA(0x3af8) invalidate)(CFTypeRef obj);
	void (* IRIS_PAC_IA(0x5680) finalize)(CFTypeRef obj);
	CFStringRef (* IRIS_PAC_IA(0xf18e) copy_debug_description)(CFTypeRef obj);
	OSStatus (* IRIS_PAC_IA(0x9725) copy_property)(CFTypeRef obj, CFStringRef key, CFAllocatorRef allocator, void *out_val);
	OSStatus (* IRIS_PAC_IA(0x2724) set_property)(CFTypeRef obj, CFStringRef key, CFTypeRef val);
	Boolean (* IRIS_PAC_IA(0xd563) equal)(CFTypeRef a, CFTypeRef b);
	CFHashCode (* IRIS_PAC_IA(0x06e4) hash)(CFTypeRef obj);
};

struct iris_vt_decoder_class {
	size_t version;
	OSStatus (* IRIS_PAC_IA(0x7be9) start_session)(CFTypeRef decoder, VTDecoderSessionRef session, CMVideoFormatDescriptionRef format_desc);
	OSStatus (* IRIS_PAC_IA(0xaf83) decode_frame)(CFTypeRef decoder, VTDecodeFrameRef frame_ref, CMSampleBufferRef sample_buf,
	    VTDecodeFrameFlags decode_flags, VTDecodeInfoFlags *info_flags_out);
	OSStatus (* IRIS_PAC_IA(0x50eb) copy_supported_properties)(CFTypeRef decoder, CFDictionaryRef *out_props);
	void (* IRIS_PAC_IA(0x2568) slot_20)(void);
	void (* IRIS_PAC_IA(0xb0f8) slot_28)(void);
	Boolean (* IRIS_PAC_IA(0x3f3c) can_accept_format)(CFTypeRef decoder, CMFormatDescriptionRef new_format);
	OSStatus (* IRIS_PAC_IA(0xbfc3) finish_delayed_frames)(CFTypeRef decoder);
	OSStatus (* IRIS_PAC_IA(0x9605) reset_format)(CFTypeRef decoder, VTDecoderSessionRef session, CMVideoFormatDescriptionRef format_desc);
	void (* IRIS_PAC_IA(0xba86) slot_48)(void);
};

struct iris_cm_vtable {
	uintptr_t reserved;
	const struct iris_cm_base_class * IRIS_PAC_DA(0xb911) base_class;
	const struct iris_vt_decoder_class * IRIS_PAC_DA(0x2356) derived_class;
};

struct iris_pending_frame {
	bool in_use;
	uint64_t tag;
	uint64_t seq;
	VTDecodeFrameRef frame_ref;
};

struct iris_vt_decoder {
	pthread_mutex_t lock;
	bool lock_init;
	CMVideoCodecType codec;
	uint32_t iris_codec;
	int fd;
	uint32_t session_id;
	uint32_t width;
	uint32_t height;
	uint8_t nal_len_size;
	bool sent_params;
	bool ioctl_error_reported;
	VTDecoderSessionRef session;
	uint8_t *param_annexb;
	size_t param_annexb_len;
	uint8_t *annexb_buf;
	size_t annexb_cap;
	uint64_t next_tag;
	struct iris_pending_frame pending[IRIS_VT_PENDING_MAX];
};

static void
iris_plugin_log(const char *format, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, format);
	int n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1u;
		(void)write(STDERR_FILENO, line, len);
	}
}

static bool
iris_append_annexb_nal(uint8_t *dst, size_t cap, size_t *io_len, const uint8_t *nal, size_t nal_len)
{
	if (nal_len == 0 || *io_len + 4u + nal_len > cap) {
		return false;
	}
	size_t off = *io_len;
	dst[off + 0] = 0x00;
	dst[off + 1] = 0x00;
	dst[off + 2] = 0x00;
	dst[off + 3] = 0x01;
	memcpy(dst + off + 4u, nal, nal_len);
	*io_len = off + 4u + nal_len;
	return true;
}

static bool
iris_parse_avcc(const uint8_t *data, size_t len, uint8_t *out_nal_len_size,
    uint8_t *out_annexb, size_t cap, size_t *out_annexb_len)
{
	if (data == NULL || len < 7u) {
		return false;
	}
	*out_nal_len_size = (uint8_t)((data[4] & 0x03u) + 1u);
	if (*out_nal_len_size == 3u) {
		return false;
	}
	size_t off = 5u;
	size_t out_len = 0;
	uint8_t num_sps = data[off++] & 0x1fu;
	for (uint8_t i = 0; i < num_sps; i++) {
		if (off + 2u > len) {
			return false;
		}
		uint16_t nlen = (uint16_t)(((uint16_t)data[off] << 8) | data[off + 1u]);
		off += 2u;
		if (off + nlen > len || !iris_append_annexb_nal(out_annexb, cap, &out_len, data + off, nlen)) {
			return false;
		}
		off += nlen;
	}
	if (off + 1u > len) {
		return false;
	}
	uint8_t num_pps = data[off++];
	for (uint8_t i = 0; i < num_pps; i++) {
		if (off + 2u > len) {
			return false;
		}
		uint16_t nlen = (uint16_t)(((uint16_t)data[off] << 8) | data[off + 1u]);
		off += 2u;
		if (off + nlen > len || !iris_append_annexb_nal(out_annexb, cap, &out_len, data + off, nlen)) {
			return false;
		}
		off += nlen;
	}
	*out_annexb_len = out_len;
	return true;
}

static bool
iris_parse_hvcc(const uint8_t *data, size_t len, uint8_t *out_nal_len_size,
    uint8_t *out_annexb, size_t cap, size_t *out_annexb_len)
{
	if (data == NULL || len < 23u) {
		return false;
	}
	*out_nal_len_size = (uint8_t)((data[21] & 0x03u) + 1u);
	if (*out_nal_len_size == 3u) {
		return false;
	}
	uint8_t num_arrays = data[22];
	size_t off = 23u;
	size_t out_len = 0;
	for (uint8_t a = 0; a < num_arrays; a++) {
		if (off + 3u > len) {
			return false;
		}
		uint16_t num_nalus = (uint16_t)(((uint16_t)data[off + 1u] << 8) | data[off + 2u]);
		off += 3u;
		for (uint16_t i = 0; i < num_nalus; i++) {
			if (off + 2u > len) {
				return false;
			}
			uint16_t nlen = (uint16_t)(((uint16_t)data[off] << 8) | data[off + 1u]);
			off += 2u;
			if (off + nlen > len || !iris_append_annexb_nal(out_annexb, cap, &out_len, data + off, nlen)) {
				return false;
			}
			off += nlen;
		}
	}
	*out_annexb_len = out_len;
	return true;
}

static bool
iris_convert_sample_to_annexb(const uint8_t *src, size_t src_len, uint8_t nal_len_size,
    const uint8_t *prefix, size_t prefix_len, uint8_t *dst, size_t dst_cap, size_t *out_len)
{
	if (src == NULL || src_len == 0 || (nal_len_size != 1u && nal_len_size != 2u && nal_len_size != 4u)) {
		return false;
	}
	if (prefix_len > dst_cap) {
		return false;
	}
	size_t w = 0;
	if (prefix != NULL && prefix_len > 0) {
		memcpy(dst, prefix, prefix_len);
		w = prefix_len;
	}
	size_t off = 0;
	size_t nal_count = 0;
	while (off < src_len) {
		if (off + nal_len_size > src_len) {
			return false;
		}
		uint32_t nlen = 0;
		for (uint8_t b = 0; b < nal_len_size; b++) {
			nlen = (nlen << 8) | src[off + b];
		}
		off += nal_len_size;
		if (nlen == 0 || off + nlen > src_len) {
			return false;
		}
		if (!iris_append_annexb_nal(dst, dst_cap, &w, src + off, nlen)) {
			return false;
		}
		off += nlen;
		nal_count++;
	}
	if (nal_count == 0) {
		return false;
	}
	*out_len = w;
	return true;
}

static bool
iris_pending_push(struct iris_vt_decoder *dec, uint64_t tag, VTDecodeFrameRef frame_ref)
{
	for (uint32_t i = 0; i < IRIS_VT_PENDING_MAX; i++) {
		if (!dec->pending[i].in_use) {
			dec->pending[i].in_use = true;
			dec->pending[i].tag = tag;
			dec->pending[i].seq = dec->next_tag;
			dec->pending[i].frame_ref = frame_ref;
			return true;
		}
	}
	return false;
}

static VTDecodeFrameRef
iris_pending_pop(struct iris_vt_decoder *dec, uint64_t tag)
{
	if (tag != 0) {
		for (uint32_t i = 0; i < IRIS_VT_PENDING_MAX; i++) {
			if (dec->pending[i].in_use && dec->pending[i].tag == tag) {
				VTDecodeFrameRef ref = dec->pending[i].frame_ref;
				memset(&dec->pending[i], 0, sizeof(dec->pending[i]));
				return ref;
			}
		}
		return NULL;
	}
	uint32_t best = IRIS_VT_PENDING_MAX;
	uint64_t best_seq = UINT64_MAX;
	for (uint32_t i = 0; i < IRIS_VT_PENDING_MAX; i++) {
		if (dec->pending[i].in_use && dec->pending[i].seq < best_seq) {
			best_seq = dec->pending[i].seq;
			best = i;
		}
	}
	if (best < IRIS_VT_PENDING_MAX) {
		VTDecodeFrameRef ref = dec->pending[best].frame_ref;
		memset(&dec->pending[best], 0, sizeof(dec->pending[best]));
		return ref;
	}
	return NULL;
}

static void
iris_pending_abort_all(struct iris_vt_decoder *dec, OSStatus err_status)
{
	for (uint32_t i = 0; i < IRIS_VT_PENDING_MAX; i++) {
		if (dec->pending[i].in_use) {
			VTDecodeFrameRef ref = dec->pending[i].frame_ref;
			memset(&dec->pending[i], 0, sizeof(dec->pending[i]));
			if (dec->session != NULL && ref != NULL) {
				(void)VTDecoderSessionEmitDecodedFrame(dec->session, ref,
				    err_status, kVTDecodeInfo_FrameDropped, NULL);
			}
		}
	}
}

static void
iris_close_hw_session(struct iris_vt_decoder *dec)
{
	iris_pending_abort_all(dec, kVTVideoDecoderMalfunctionErr);
	if (dec->fd >= 0 && dec->session_id != 0) {
		uint32_t sid = dec->session_id;
		(void)iris_sys_ioctl(dec->fd, IRIS_IOC_CLOSE_SESSION, &sid);
		dec->session_id = 0;
	}
	if (dec->fd >= 0) {
		(void)iris_sys_close(dec->fd);
		dec->fd = -1;
	}
	dec->session = NULL;
	dec->sent_params = false;
}

static OSStatus
iris_vt_invalidate(CFTypeRef obj)
{
	struct iris_vt_decoder *dec = (struct iris_vt_decoder *)CMBaseObjectGetDerivedStorage(obj);
	if (dec == NULL) {
		return noErr;
	}
	if (dec->lock_init) {
		pthread_mutex_lock(&dec->lock);
	}
	iris_close_hw_session(dec);
	if (dec->lock_init) {
		pthread_mutex_unlock(&dec->lock);
	}
	return noErr;
}

static void
iris_vt_finalize(CFTypeRef obj)
{
	struct iris_vt_decoder *dec = (struct iris_vt_decoder *)CMBaseObjectGetDerivedStorage(obj);
	if (dec == NULL) {
		return;
	}
	iris_close_hw_session(dec);
	free(dec->param_annexb);
	dec->param_annexb = NULL;
	free(dec->annexb_buf);
	dec->annexb_buf = NULL;
	if (dec->lock_init) {
		pthread_mutex_destroy(&dec->lock);
		dec->lock_init = false;
	}
}

static CFStringRef
iris_vt_copy_debug_description(CFTypeRef obj)
{
	struct iris_vt_decoder *dec = (struct iris_vt_decoder *)CMBaseObjectGetDerivedStorage(obj);
	return CFStringCreateWithFormat(kCFAllocatorDefault, NULL,
	    CFSTR("<AnnibaleIrisDecoder %p sid=%u %ux%u>"), dec,
	    dec ? dec->session_id : 0u, dec ? dec->width : 0u, dec ? dec->height : 0u);
}

static OSStatus
iris_vt_copy_property(CFTypeRef obj, CFStringRef key, CFAllocatorRef allocator, void *out_val)
{
	(void)obj;
	(void)allocator;
	if (key == NULL || out_val == NULL) {
		return paramErr;
	}
	if (CFEqual(key, CFSTR("UsingHardwareAcceleratedVideoDecoder")) ||
	    CFEqual(key, CFSTR("IsHardwareAccelerated"))) {
		*(CFTypeRef *)out_val = CFRetain(kCFBooleanTrue);
		return noErr;
	}
	if (CFEqual(key, CFSTR("RealTime")) ||
	    CFEqual(key, CFSTR("AllowFrameReordering"))) {
		*(CFTypeRef *)out_val = CFRetain(kCFBooleanTrue);
		return noErr;
	}
	return IRIS_CM_PROP_NOT_FOUND;
}

static OSStatus
iris_vt_set_property(CFTypeRef obj, CFStringRef key, CFTypeRef val)
{
	(void)obj;
	(void)val;
	if (key == NULL) {
		return paramErr;
	}
	if (CFEqual(key, CFSTR("RealTime")) ||
	    CFEqual(key, CFSTR("MaximizePowerEfficiency")) ||
	    CFEqual(key, CFSTR("AllowFrameReordering")) ||
	    CFEqual(key, CFSTR("ThreadCount")) ||
	    CFEqual(key, CFSTR("ReducedResolutionDecode"))) {
		return noErr;
	}
	return IRIS_CM_PROP_NOT_SUPPORTED;
}

static OSStatus
iris_vt_copy_supported_properties(CFTypeRef decoder, CFDictionaryRef *out_props)
{
	(void)decoder;
	if (out_props == NULL) {
		return paramErr;
	}
	CFDictionaryRef empty = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *keys[] = {
		CFSTR("UsingHardwareAcceleratedVideoDecoder"),
		CFSTR("RealTime"),
		CFSTR("MaximizePowerEfficiency"),
		CFSTR("AllowFrameReordering"),
	};
	const void *vals[] = { empty, empty, empty, empty };
	*out_props = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 4,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (empty != NULL) {
		CFRelease(empty);
	}
	return *out_props != NULL ? noErr : memFullErr;
}

static OSStatus
iris_configure_session_Locked(struct iris_vt_decoder *dec, VTDecoderSessionRef session,
    CMVideoFormatDescriptionRef format_desc)
{
	if (session == NULL || format_desc == NULL) {
		return paramErr;
	}
	CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(format_desc);
	if (dims.width < 64 || dims.height < 64 || dims.width > 4096 || dims.height > 4096) {
		return kVTVideoDecoderUnsupportedDataFormatErr;
	}

	if (dec->param_annexb == NULL) {
		dec->param_annexb = (uint8_t *)malloc(IRIS_VT_PARAM_CAP);
		if (dec->param_annexb == NULL) {
			return memFullErr;
		}
	}
	dec->param_annexb_len = 0;
	dec->nal_len_size = 4u;

	CFDictionaryRef atoms = (CFDictionaryRef)CMFormatDescriptionGetExtension(format_desc,
	    kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
	if (atoms != NULL) {
		CFDataRef cfg = (CFDataRef)CFDictionaryGetValue(atoms,
		    dec->iris_codec == IRIS_CODEC_H264 ? CFSTR("avcC") : CFSTR("hvcC"));
		if (cfg != NULL) {
			const uint8_t *ptr = CFDataGetBytePtr(cfg);
			size_t len = (size_t)CFDataGetLength(cfg);
			bool ok = (dec->iris_codec == IRIS_CODEC_H264) ?
			    iris_parse_avcc(ptr, len, &dec->nal_len_size, dec->param_annexb, IRIS_VT_PARAM_CAP, &dec->param_annexb_len) :
			    iris_parse_hvcc(ptr, len, &dec->nal_len_size, dec->param_annexb, IRIS_VT_PARAM_CAP, &dec->param_annexb_len);
			if (!ok) {
				return kVTVideoDecoderBadDataErr;
			}
		}
	}

	if (dec->annexb_buf == NULL) {
		dec->annexb_cap = 512u * 1024u;
		dec->annexb_buf = (uint8_t *)malloc(dec->annexb_cap);
		if (dec->annexb_buf == NULL) {
			return memFullErr;
		}
	}

	iris_close_hw_session(dec);
	dec->fd = iris_sys_open("/dev/annibale-iris", O_RDWR | O_CLOEXEC);
	if (dec->fd < 0) {
		return kVTVideoDecoderMalfunctionErr;
	}

	struct iris_session_params sp = {
		.codec_type = dec->iris_codec,
		.width = (uint32_t)dims.width,
		.height = (uint32_t)dims.height,
		.session_id = 0,
	};
	if (iris_sys_ioctl(dec->fd, IRIS_IOC_OPEN_SESSION, &sp) != 0 || sp.session_id == 0) {
		iris_close_hw_session(dec);
		return kVTVideoDecoderMalfunctionErr;
	}

	dec->session_id = sp.session_id;
	dec->width = (uint32_t)dims.width;
	dec->height = (uint32_t)dims.height;
	dec->sent_params = false;
	dec->session = session;
	dec->ioctl_error_reported = false;

	int32_t fmt_val = (int32_t)kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	int32_t w_val = dims.width;
	int32_t h_val = dims.height;
	CFNumberRef cf_fmt = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &fmt_val);
	CFNumberRef cf_w = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &w_val);
	CFNumberRef cf_h = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &h_val);
	CFDictionaryRef iosurf_props = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *pkeys[] = {
		kCVPixelBufferPixelFormatTypeKey,
		kCVPixelBufferWidthKey,
		kCVPixelBufferHeightKey,
		kCVPixelBufferIOSurfacePropertiesKey,
	};
	const void *pvals[] = { cf_fmt, cf_w, cf_h, iosurf_props };
	CFDictionaryRef pb_attrs = CFDictionaryCreate(kCFAllocatorDefault, pkeys, pvals, 4,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	OSStatus attr_err = VTDecoderSessionSetPixelBufferAttributes(session, pb_attrs);
	if (pb_attrs) CFRelease(pb_attrs);
	if (iosurf_props) CFRelease(iosurf_props);
	if (cf_h) CFRelease(cf_h);
	if (cf_w) CFRelease(cf_w);
	if (cf_fmt) CFRelease(cf_fmt);
	return attr_err;
}

static OSStatus
iris_vt_start_session(CFTypeRef decoder, VTDecoderSessionRef session, CMVideoFormatDescriptionRef format_desc)
{
	iris_plugin_log("iris_vt[%d]: start decoder=%p session=%p format=%p\n",
	    getpid(), decoder, (void *)session, format_desc);
	struct iris_vt_decoder *dec = (struct iris_vt_decoder *)CMBaseObjectGetDerivedStorage(decoder);
	if (dec == NULL) {
		return paramErr;
	}
	pthread_mutex_lock(&dec->lock);
	OSStatus status = iris_configure_session_Locked(dec, session, format_desc);
	iris_plugin_log("iris_vt[%d]: start result=%d session_id=%u\n",
	    getpid(), (int)status, dec->session_id);
	pthread_mutex_unlock(&dec->lock);
	return status;
}

static OSStatus
iris_vt_reset_format(CFTypeRef decoder, VTDecoderSessionRef session, CMVideoFormatDescriptionRef format_desc)
{
	return iris_vt_start_session(decoder, session, format_desc);
}

static Boolean
iris_vt_can_accept_format(CFTypeRef decoder, CMFormatDescriptionRef new_format)
{
	struct iris_vt_decoder *dec = (struct iris_vt_decoder *)CMBaseObjectGetDerivedStorage(decoder);
	if (dec == NULL || new_format == NULL) {
		return false;
	}
	if (CMFormatDescriptionGetMediaSubType(new_format) != dec->codec) {
		return false;
	}
	CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions((CMVideoFormatDescriptionRef)new_format);
	return (uint32_t)dims.width == dec->width && (uint32_t)dims.height == dec->height;
}

static OSStatus
iris_run_decode_step_Locked(struct iris_vt_decoder *dec, VTDecodeFrameRef alloc_frame_ref,
    const uint8_t *annexb, uint32_t annexb_len, uint64_t in_tag, uint32_t mode_flags, bool *out_emitted)
{
	*out_emitted = false;
	CVPixelBufferRef pixbuf = NULL;
	OSStatus status = VTDecoderSessionCreatePixelBuffer(dec->session, alloc_frame_ref, &pixbuf);
	if (status != noErr || pixbuf == NULL) {
		return status != noErr ? status : kVTVideoDecoderMalfunctionErr;
	}
	if (CVPixelBufferLockBaseAddress(pixbuf, 0) != kCVReturnSuccess) {
		CFRelease(pixbuf);
		return kVTVideoDecoderMalfunctionErr;
	}

	struct iris_decode_frame req;
	memset(&req, 0, sizeof(req));
	req.session_id = dec->session_id;
	req.input_user_addr = (uint64_t)(uintptr_t)annexb;
	req.input_len = annexb_len;
	req.in_tag = in_tag;
	req.out_user_y = (uint64_t)(uintptr_t)CVPixelBufferGetBaseAddressOfPlane(pixbuf, 0);
	req.out_user_uv = (uint64_t)(uintptr_t)CVPixelBufferGetBaseAddressOfPlane(pixbuf, 1);
	req.out_stride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(pixbuf, 0);
	req.flags = mode_flags;

	int rc = iris_sys_ioctl(dec->fd, IRIS_IOC_DECODE_FRAME, &req);
	/* CoreVideo cleanup may change errno before the failed ioctl is reported. */
	int ioctl_error = rc != 0 ? errno : 0;
	CVPixelBufferUnlockBaseAddress(pixbuf, 0);
	if (rc != 0) {
		if (!dec->ioctl_error_reported) {
			dec->ioctl_error_reported = true;
			iris_plugin_log("iris_vt[%d]: decode ioctl failed sid=%u mode=0x%x tag=%llu bytes=%u errno=%d\n",
			    getpid(), dec->session_id, mode_flags, (unsigned long long)in_tag, annexb_len, ioctl_error);
		}
		CFRelease(pixbuf);
		return kVTVideoDecoderBadDataErr;
	}

	if ((req.flags & IRIS_FRAME_DECODED) != 0) {
		VTDecodeFrameRef target_ref = iris_pending_pop(dec, req.out_tag);
		if (target_ref != NULL) {
			status = VTDecoderSessionEmitDecodedFrame(dec->session, target_ref, noErr, 0, pixbuf);
			*out_emitted = true;
		}
	}
	CFRelease(pixbuf);
	return status;
}

static bool
iris_sample_is_sync(CMSampleBufferRef sample_buf)
{
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample_buf, false);
	if (attachments == NULL || CFArrayGetCount(attachments) == 0) {
		return true;
	}
	CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
	if (dict == NULL) {
		return true;
	}
	CFBooleanRef not_sync = (CFBooleanRef)CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync);
	return not_sync == NULL || !CFBooleanGetValue(not_sync);
}

static OSStatus
iris_vt_decode_frame(CFTypeRef decoder, VTDecodeFrameRef frame_ref, CMSampleBufferRef sample_buf,
    VTDecodeFrameFlags decode_flags, VTDecodeInfoFlags *info_flags_out)
{
	(void)decode_flags;
	if (info_flags_out != NULL) {
		*info_flags_out = 0;
	}
	struct iris_vt_decoder *dec = (struct iris_vt_decoder *)CMBaseObjectGetDerivedStorage(decoder);
	if (dec == NULL || frame_ref == NULL) {
		return paramErr;
	}

	OSStatus status = noErr;
	pthread_mutex_lock(&dec->lock);
	if (sample_buf == NULL) {
		status = paramErr;
		goto failed;
	}
	CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buf);
	size_t sample_len = block != NULL ? CMBlockBufferGetDataLength(block) : 0;
	if (block == NULL || sample_len == 0 || sample_len > IRIS_VT_BITSTREAM_CAP - IRIS_VT_PARAM_CAP) {
		status = kVTVideoDecoderBadDataErr;
		goto failed;
	}
	if (dec->fd < 0 || dec->session_id == 0 || dec->session == NULL) {
		status = kVTVideoDecoderMalfunctionErr;
		goto failed;
	}
	if (dec->next_tag == 0) {
		iris_plugin_log("iris_vt[%d]: decode session_id=%u frame_token=%p bytes=%zu\n",
		    getpid(), dec->session_id, frame_ref, sample_len);
	}

	size_t needed = sample_len + dec->param_annexb_len + 1024u;
	if (needed > dec->annexb_cap) {
		if (needed > IRIS_VT_BITSTREAM_CAP) {
			status = kVTVideoDecoderBadDataErr;
			goto failed;
		}
		uint8_t *grown = (uint8_t *)realloc(dec->annexb_buf, needed);
		if (grown == NULL) {
			status = memFullErr;
			goto failed;
		}
		dec->annexb_buf = grown;
		dec->annexb_cap = needed;
	}

	char *contig_ptr = NULL;
	size_t len_at_off = 0, total_len = 0;
	uint8_t *heap_copy = NULL;
	const uint8_t *raw_src = NULL;
	if (CMBlockBufferGetDataPointer(block, 0, &len_at_off, &total_len, &contig_ptr) == kCMBlockBufferNoErr &&
	    contig_ptr != NULL && len_at_off == sample_len) {
		raw_src = (const uint8_t *)contig_ptr;
	} else {
		heap_copy = (uint8_t *)malloc(sample_len);
		if (heap_copy == NULL) {
			status = memFullErr;
			goto failed;
		}
		if (CMBlockBufferCopyDataBytes(block, 0, sample_len, heap_copy) != kCMBlockBufferNoErr) {
			free(heap_copy);
			status = kVTVideoDecoderBadDataErr;
			goto failed;
		}
		raw_src = heap_copy;
	}

	bool prepend = !dec->sent_params || iris_sample_is_sync(sample_buf);
	size_t annexb_len = 0;
	bool conv_ok = iris_convert_sample_to_annexb(raw_src, sample_len, dec->nal_len_size,
	    prepend ? dec->param_annexb : NULL, prepend ? dec->param_annexb_len : 0,
	    dec->annexb_buf, dec->annexb_cap, &annexb_len);
	free(heap_copy);
	if (!conv_ok) {
		status = kVTVideoDecoderBadDataErr;
		goto failed;
	}
	dec->sent_params = true;

	uint64_t tag = ++dec->next_tag;
	if (tag == 0) {
		tag = ++dec->next_tag;
	}
	if (!iris_pending_push(dec, tag, frame_ref)) {
		status = kVTVideoDecoderMalfunctionErr;
		goto failed;
	}

	bool emitted = false;
	status = iris_run_decode_step_Locked(dec, frame_ref, dec->annexb_buf, (uint32_t)annexb_len,
	    tag, 0, &emitted);
	if (status != noErr) {
		if (iris_pending_pop(dec, tag) != NULL) {
			goto failed;
		}
		pthread_mutex_unlock(&dec->lock);
		return status;
	}

	while (emitted) {
		emitted = false;
		bool any_pending = false;
		for (uint32_t i = 0; i < IRIS_VT_PENDING_MAX; i++) {
			if (dec->pending[i].in_use) {
				any_pending = true;
				break;
			}
		}
		if (!any_pending) {
			break;
		}
		status = iris_run_decode_step_Locked(dec, NULL, NULL, 0, 0, IRIS_FRAME_POLL, &emitted);
		if (status != noErr) {
			iris_pending_abort_all(dec, status);
			break;
		}
	}

	pthread_mutex_unlock(&dec->lock);
	return status;

failed:
	/* iOS 26 waits for EmitDecodedFrame even after decode_frame returns an error. */
	if (dec->session != NULL) {
		(void)VTDecoderSessionEmitDecodedFrame(dec->session, frame_ref, status,
		    kVTDecodeInfo_FrameDropped, NULL);
	}
	if (info_flags_out != NULL) {
		*info_flags_out |= kVTDecodeInfo_FrameDropped;
	}
	pthread_mutex_unlock(&dec->lock);
	return status;
}

static OSStatus
iris_vt_finish_delayed_frames(CFTypeRef decoder)
{
	struct iris_vt_decoder *dec = (struct iris_vt_decoder *)CMBaseObjectGetDerivedStorage(decoder);
	if (dec == NULL) {
		return paramErr;
	}
	pthread_mutex_lock(&dec->lock);
	if (dec->fd >= 0 && dec->session_id != 0 && dec->session != NULL) {
		for (uint32_t step = 0; step < IRIS_VT_PENDING_MAX; step++) {
			bool any_pending = false;
			for (uint32_t i = 0; i < IRIS_VT_PENDING_MAX; i++) {
				if (dec->pending[i].in_use) {
					any_pending = true;
					break;
				}
			}
			if (!any_pending) {
				break;
			}
			bool emitted = false;
			OSStatus st = iris_run_decode_step_Locked(dec, NULL, NULL, 0, 0, IRIS_FRAME_DRAIN, &emitted);
			if (st != noErr || !emitted) {
				break;
			}
		}
		uint32_t sid = dec->session_id;
		(void)iris_sys_ioctl(dec->fd, IRIS_IOC_FLUSH_SESSION, &sid);
		iris_pending_abort_all(dec, kVTVideoDecoderMalfunctionErr);
	}
	pthread_mutex_unlock(&dec->lock);
	return noErr;
}

static const struct iris_cm_base_class g_iris_cm_base_class = {
	.version = 1,
	.derived_storage_size = sizeof(struct iris_vt_decoder),
	.initialize = NULL,
	.invalidate = iris_vt_invalidate,
	.finalize = iris_vt_finalize,
	.copy_debug_description = iris_vt_copy_debug_description,
	.copy_property = iris_vt_copy_property,
	.set_property = iris_vt_set_property,
	.equal = NULL,
	.hash = NULL,
};

static const struct iris_vt_decoder_class g_iris_vt_decoder_class = {
	.version = 2,
	.start_session = iris_vt_start_session,
	.decode_frame = iris_vt_decode_frame,
	.copy_supported_properties = iris_vt_copy_supported_properties,
	.slot_20 = NULL,
	.slot_28 = NULL,
	.can_accept_format = iris_vt_can_accept_format,
	.finish_delayed_frames = iris_vt_finish_delayed_frames,
	.reset_format = iris_vt_reset_format,
	.slot_48 = NULL,
};

static const struct iris_cm_vtable g_iris_vt_vtable = {
	.reserved = 0,
	.base_class = &g_iris_cm_base_class,
	.derived_class = &g_iris_vt_decoder_class,
};

static OSStatus
iris_decoder_create_instance(CMVideoCodecType codec_type, CFAllocatorRef allocator, CFTypeRef *out_decoder)
{
	iris_plugin_log("iris_vt[%d]: create codec=0x%08x allocator=%p out=%p\n",
	    getpid(), codec_type, allocator, (void *)out_decoder);
	if (out_decoder == NULL) {
		return paramErr;
	}
	uint32_t iris_codec = 0;
	if (codec_type == kCMVideoCodecType_H264) {
		iris_codec = IRIS_CODEC_H264;
	} else if (codec_type == kCMVideoCodecType_HEVC || codec_type == (CMVideoCodecType)'hev1') {
		iris_codec = IRIS_CODEC_HEVC;
	} else {
		return kVTCouldNotFindVideoDecoderErr;
	}

	CFTypeRef obj = NULL;
	CFTypeID class_id = VTVideoDecoderGetClassID();
	iris_plugin_log("iris_vt[%d]: create class=%p vtable=%p\n",
	    getpid(), (void *)(uintptr_t)class_id, (const void *)&g_iris_vt_vtable);
	OSStatus status = CMDerivedObjectCreate(allocator, &g_iris_vt_vtable, class_id, &obj);
	iris_plugin_log("iris_vt[%d]: create result=%d object=%p\n", getpid(), (int)status, obj);
	if (status != noErr || obj == NULL) {
		return status != noErr ? status : memFullErr;
	}
	struct iris_vt_decoder *dec = (struct iris_vt_decoder *)CMBaseObjectGetDerivedStorage(obj);
	memset(dec, 0, sizeof(*dec));
	pthread_mutex_init(&dec->lock, NULL);
	dec->lock_init = true;
	dec->codec = codec_type;
	dec->iris_codec = iris_codec;
	dec->fd = -1;
	*out_decoder = obj;
	return noErr;
}

static void
iris_register_once(void)
{
	int fd = iris_sys_open("/dev/annibale-iris", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return;
	}
	struct iris_device_info info;
	memset(&info, 0, sizeof(info));
	int rc = iris_sys_ioctl(fd, IRIS_IOC_GET_INFO, &info);
	(void)iris_sys_close(fd);
	if (rc != 0 || (info.flags & IRIS_INFO_HW_READY) == 0) {
		iris_plugin_log("iris_vt: VPU not ready (rc=%d flags=0x%x); keeping software VideoToolbox decoders\n",
		    rc, info.flags);
		return;
	}

	int32_t rating = 200;
	CFNumberRef cf_rating = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &rating);
	const void *keys[] = { kVTVideoDecoder_IsHardwareAccelerated, kVTVideoDecoderInfo_Rating };
	const void *vals[] = { kCFBooleanTrue, cf_rating };
	CFDictionaryRef info_dict = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 2,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	OSStatus s_avc = noErr, s_hvc1 = noErr, s_hev1 = noErr;
	if ((info.codecs_mask & IRIS_CODEC_MASK_H264) != 0) {
		s_avc = VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_H264, info_dict, iris_decoder_create_instance);
	}
	if ((info.codecs_mask & IRIS_CODEC_MASK_HEVC) != 0) {
		s_hvc1 = VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_HEVC, info_dict, iris_decoder_create_instance);
		s_hev1 = VTRegisterVideoDecoderWithInfo((CMVideoCodecType)'hev1', info_dict, iris_decoder_create_instance);
	}
	if (info_dict) CFRelease(info_dict);
	if (cf_rating) CFRelease(cf_rating);
	iris_plugin_log("iris_vt: registered SM8750 Iris VPU v3.5 decoders (avc1=%d hvc1=%d hev1=%d)\n",
	    (int)s_avc, (int)s_hvc1, (int)s_hev1);
}

void
register_iris_videotoolbox_decoders(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
#ifdef IRIS_VT_HOST_TEST
	(void)once;
	iris_register_once();
#else
	pthread_once(&once, iris_register_once);
#endif
}

#ifndef IRIS_VT_HOST_TEST
__attribute__((constructor))
static void
iris_plugin_init(void)
{
	register_iris_videotoolbox_decoders();
}
#endif
