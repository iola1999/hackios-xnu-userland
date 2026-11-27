/*
 * Stream frame rendering, metadata attachment, contrast autofocus state machine,
 * and /dev/annibale-camera worker thread (included by camera_device.c).
 */
#include <sys/sysctl.h>

/*
 * ModuleInfo follows V57/CameraSetup.plist's ModuleIDSchemeVersion 1 and BWFigVideoCaptureDevice.
 */
static CFDictionaryRef
stream_module_info(const struct stream *s)
{
	CFMutableDictionaryRef m = (CFMutableDictionaryRef)new_dict();
	int32_t sensor_id = s->camera->sensor == 1 ? 0x0904 : 0x0914;

	put(m, "sensorID", number_i32(sensor_id));
	put(m, "SensorID", number_i32(sensor_id));
	put(m, "ModuleFocusID", number_i32(1));
	put(m, "ModuleActuatorID", number_i32(1));
	put(m, "ModuleLensID", number_i32(1));
	put(m, "ModuleIntegratorID", number_i32(1));
	return m;
}

static CFDictionaryRef
stream_raw_pool_requirements(const struct stream *s)
{
	CFMutableDictionaryRef r = (CFMutableDictionaryRef)new_dict();
	int32_t w = (int32_t)s->profile.width;
	int32_t h = (int32_t)s->profile.height;

	put(r, "MinimumBufferCount", number_i32(4));
	put(r, "Width", number_i32(w));
	put(r, "Height", number_i32(h));
	put(r, "BytesPerRowAlignment", number_i32(64));
	put(r, "PlaneAlignment", number_i32(64));
	put(r, "PixelFormatType", number_i32((int32_t)PF_BGRA));
	return r;
}

static CFDictionaryRef
stream_gdc_rects(const struct stream *s)
{
	CFMutableDictionaryRef d = (CFMutableDictionaryRef)new_dict();
	int32_t def_w = (int32_t)s->profile.width;
	int32_t def_h = (int32_t)s->profile.height;

	if (s->formats != NULL && s->format_index >= 0 && s->format_index < CFArrayGetCount(s->formats)) {
		CFDictionaryRef fmt = CFArrayGetValueAtIndex(s->formats, s->format_index);
		CFNumberRef fw = fmt != NULL ? CFDictionaryGetValue(fmt, CFSTR("VideoMaxWidth")) : NULL;
		CFNumberRef fh = fmt != NULL ? CFDictionaryGetValue(fmt, CFSTR("VideoMaxHeight")) : NULL;

		if (fw != NULL && CFGetTypeID(fw) == CFNumberGetTypeID()) {
			CFNumberGetValue(fw, kCFNumberSInt32Type, &def_w);
		}
		if (fh != NULL && CFGetTypeID(fh) == CFNumberGetTypeID()) {
			CFNumberGetValue(fh, kCFNumberSInt32Type, &def_h);
		}
	}
	for (int i = 0; i < OUTPUTS; i++) {
		CFDictionaryRef oc = s->out[i].config;
		CFTypeRef cr = oc != NULL ? CFDictionaryGetValue(oc, CFSTR("CropRect")) : NULL;

		if (cr != NULL && CFGetTypeID(cr) == CFDictionaryGetTypeID()) {
			put(d, output_names[i], CFRetain(cr));
			continue;
		}
		int32_t w = def_w, h = def_h;
		CFNumberRef cw = oc != NULL ? CFDictionaryGetValue(oc, CFSTR("Width")) : NULL;
		CFNumberRef ch = oc != NULL ? CFDictionaryGetValue(oc, CFSTR("Height")) : NULL;

		if (cw != NULL && CFGetTypeID(cw) == CFNumberGetTypeID()) {
			CFNumberGetValue(cw, kCFNumberSInt32Type, &w);
		}
		if (ch != NULL && CFGetTypeID(ch) == CFNumberGetTypeID()) {
			CFNumberGetValue(ch, kCFNumberSInt32Type, &h);
		}
		CFMutableDictionaryRef r = (CFMutableDictionaryRef)new_dict();

		put(r, "X", number_f(0));
		put(r, "Y", number_f(0));
		put(r, "Width", number_f(w));
		put(r, "Height", number_f(h));
		put(d, output_names[i], r);
	}
	return d;
}

/* TorchLevel uses 0..1; /dev/annibale-torch accepts uint32_t thousandths. */
static OSStatus
torch_set(struct stream *s, CFTypeRef value)
{
	double level = 0;
	uint32_t thousandths;

	if (s->torch < 0) {
		return ERR_NOT_AVAILABLE;
	}
	if (value == NULL || CFGetTypeID(value) != CFNumberGetTypeID() ||
	    !CFNumberGetValue(value, kCFNumberDoubleType, &level) || !isfinite(level)) {
		return ERR_PARAM;
	}
	level = level < 0 ? 0 : level > 1 ? 1 : level;
	thousandths = (uint32_t)(level * 1000 + 0.5);
	if (write(s->torch, &thousandths, sizeof(thousandths)) != (ssize_t)sizeof(thousandths)) {
		say("the flashlight write failed at %u of 1000", thousandths);
		return ERR_NOT_AVAILABLE;
	}
	say("the flashlight requested at %u of 1000", thousandths);
	return 0;
}

/* ---- the frames ------------------------------------------------------------------------------------- */

struct render_job {
	const struct isp *isp;
	const struct annibale_camera_frame *f;
	const uint8_t *raw;
	CVPixelBufferRef pb;
	uint32_t format, width, height, pieces;
	bool still;                     /* the photograph: every pixel of the sensor (isp_render_still_nv12) */
};

static void
render_piece(void *context, size_t i)
{
	struct render_job *j = context;
	uint32_t rows = (j->height + j->pieces - 1) / j->pieces, first = (uint32_t)i * rows, end = first + rows;

	first &= ~1u;
	end = i + 1 == j->pieces ? j->height : end & ~1u;
	if (j->format == PF_BGRA) {
		isp_render_bgra(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddress(j->pb), CVPixelBufferGetBytesPerRow(j->pb), j->width,
		    j->height, first, end);
	} else if (j->still) {
		isp_render_still_nv12(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddressOfPlane(j->pb, 0),
		    CVPixelBufferGetBytesPerRowOfPlane(j->pb, 0), CVPixelBufferGetBaseAddressOfPlane(j->pb, 1),
		    CVPixelBufferGetBytesPerRowOfPlane(j->pb, 1), j->width, j->height, first, end,
		    j->format == PF_420V || j->format == PF_X420 || j->format == PF_X422);
	} else {
		isp_render_nv12(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddressOfPlane(j->pb, 0), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 0),
		    CVPixelBufferGetBaseAddressOfPlane(j->pb, 1), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 1), j->width, j->height, first, end,
		    j->format == PF_420V || j->format == PF_X420 || j->format == PF_X422);
	}
}

static CVPixelBufferRef
output_buffer(struct output *out)
{
	CVPixelBufferPoolRef pool = NULL;
	CVPixelBufferRef pb = NULL;
	CFTypeRef value = out->config != NULL ? CFDictionaryGetValue(out->config, CFSTR("BufferPool")) : NULL;

	/*
	 * The graph makes the pool itself and hands it over in the output's configuration
	 * (kFigCaptureStreamVideoOutputConfigurationKey_BufferPool), sized and laid out as the node
	 * behind the output wants it (an IOSurface each, extra rows at the bottom where a scaler wants
	 * them, as few buffers as the node holds at once) -- there is nothing for this side to choose,
	 * and a buffer of our own making is not one the graph would take. The configuration carries no
	 * width or height: the pool's buffers have them.
	 */
	if (value != NULL && CFGetTypeID(value) == CVPixelBufferPoolGetTypeID()) {
		pool = (CVPixelBufferPoolRef)value;
	}
	if (pool == NULL) {
		if (!out->said_shape) {
			out->said_shape = true;
			say("%s: no pool of the graph's in the configuration: no pictures", out->name);
		}
		return NULL;
	}
	for (unsigned i = 0; CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) != kCVReturnSuccess && i < HELD; i++) {
		/* a pool that will not grow that far: the oldest picture kept is the one to have */
		unsigned oldest = (out->held_next + i) % HELD;

		if (out->held[oldest] != NULL) {
			CVPixelBufferRelease(out->held[oldest]);
			out->held[oldest] = NULL;
		}
		if (!out->said_pool) {
			out->said_pool = true;
			say("%s: the graph's pool will not grow by %u: fewer pictures kept", out->name, HELD);
		}
	}
	if (pb == NULL) {
		/*
		 * The preview pool holds one buffer (MinimumBufferCount 1): while the graph still has it,
		 * there is none to be had, and this frame is not given out. Said at most every 300th frame.
		 */
		if (out->refused++ % 300 == 0) {
			say("%s: the graph's pool has no buffer to spare (%llu frames so)", out->name,
			    (unsigned long long)out->refused);
		}
		return NULL;
	}
	return pb;
}

typedef void (^pixel_buffer_handler)(CVPixelBufferRef, CMTime);

static void
let_go(struct output *out)
{
	for (unsigned i = 0; i < HELD; i++) {
		if (out->held[i] != NULL) {
			CVPixelBufferRelease(out->held[i]);
			out->held[i] = NULL;
		}
	}
}

static void
md_put(CFMutableDictionaryRef m, int key, CFTypeRef value)
{
	if (md_key[key] != NULL && value != NULL) {
		CFDictionarySetValue(m, md_key[key], value);
	}
	if (value != NULL) {
		CFRelease(value);
	}
}

/* the frame's metadata, as H10ISP's stream gives them (see md_names) */
static CFDictionaryRef
frame_metadata(const struct isp *isp, const struct annibale_camera_frame *f, CFStringRef port, int af_status,
    bool af_locked, bool af_stable, unsigned focus_pos, int64_t focus_cmdid)
{
	CFMutableDictionaryRef m;
	if (md_attachment == NULL) {
		return NULL;
	}
	m = (CFMutableDictionaryRef)new_dict();
	md_put(m, MD_PORT, CFRetain(port));
	md_put(m, MD_PTS, CMTimeCopyAsDictionary(CMClockMakeHostTimeFromSystemUnits(f->time), NULL));
	if (f->exposure_lines != 0 && f->line_ns != 0) {
		md_put(m, MD_EXPOSURE, number_f(f->exposure_lines * (double)f->line_ns / 1e9));
	}
	if (f->gain != 0 && f->gain_unity != 0) {
		md_put(m, MD_AGC, number_i((int64_t)((double)f->gain / f->gain_unity * 256 + 0.5)));
	}
	md_put(m, MD_ISP_DGAIN, number_i((int64_t)(isp->digital * 256 + 0.5)));
	if (f->frame_lines != 0 && f->line_ns != 0) {
		md_put(m, MD_FRAME_RATE, number_f(1e9 / ((double)f->frame_lines * f->line_ns)));
	}
	/*
	 * The zoom the picture was made with: the source node takes it for what the ISP applied
	 * (-[BWMultiStreamCameraSourceNode _ispAppliedZoomFactorFromSampleBufferMetadataDictionary:outputIndex:])
	 * and would make up the rest of what was asked for itself; without it, a zoomed preview stood still.
	 */
	md_put(m, MD_ZOOM, number_f(isp->zoom));
	/*
	 * The focus state as CMCapture consumes it (afstatus-semantics.md): AFStatus 2 while the lens
	 * searches or settles on the peak, 4 parked and watching -- control-mode transitions wait on
	 * ==2, frames weigh a quarter at ==2 and full at ==4, the stable-frame count goes on only at
	 * ==4. AFStable is a CFBoolean, read with CFBooleanGetValue; it is also one of the keys the
	 * autofocus processor requires present before it latches FocusNowCommandID. That id is the
	 * command being answered, or the int32 idle sentinel (focus_cmdid_echo_frame()).
	 */
	if (af_status >= 0) {
		md_put(m, MD_AFSTATUS, number_i(af_status));
		md_put(m, MD_AFLOCKED, af_locked ? kCFBooleanTrue : kCFBooleanFalse);
		md_put(m, MD_AFSTABLE, af_stable ? kCFBooleanTrue : kCFBooleanFalse);
		md_put(m, MD_FOCUSPOS, number_i((int64_t)focus_pos));
		md_put(m, MD_FOCUSCMDID, focus_cmdid != 0 ? number_i(focus_cmdid) : number_i32(FOCUS_CMDID_IDLE));
	}
	/* wanted on every frame, a motor or not: the photograph's reference-frame choice reads it too */
	md_put(m, MD_FOCUSPEAKS, md_focus_peaks != NULL ? CFRetain(md_focus_peaks) : NULL);
	return m;
}

struct shot_env {
	float           lux, cct;
	bool            redeye, flash_lit, torch;
	int32_t         exif_orientation;       /* the photograph's ExifOrientation attachment (0: none written) */
};

static CVPixelBufferRef
picture_buffer(struct output *out, uint32_t *pf, uint32_t *width, uint32_t *height)
{
	CVPixelBufferRef pb = output_buffer(out);

	if (pb == NULL) {
		return NULL;
	}
	*pf = CVPixelBufferGetPixelFormatType(pb);
	*width = (uint32_t)CVPixelBufferGetWidth(pb);
	*height = (uint32_t)CVPixelBufferGetHeight(pb);
	if (*width == 0 || *height == 0 ||
	    (*pf != PF_420F && *pf != PF_420V && *pf != PF_X420 && *pf != PF_X422 && *pf != PF_XF20 && *pf != PF_BGRA)) {
		if (out->delivered == 0 && out->said_shape == false) {
			out->said_shape = true;
			say("%s: %ux%u %c%c%c%c buffers are not for this ISP", out->name, *width, *height, *pf >> 24,
			    *pf >> 16 & 255, *pf >> 8 & 255, *pf & 255);
		}
		CVPixelBufferRelease(pb);
		return NULL;
	}
	return pb;
}

/*
 * A photograph's width when governed: half the cores the kernel runs, two at least. hw.ncpu
 * follows the rigs' cpus= boot-arg -- the camera rounds boot cpus=4, so a photograph renders
 * two pieces wide there.
 */
static uint32_t
still_render_pieces(void)
{
	int ncpu = 0;
	size_t size = sizeof(ncpu);
	unsigned half;

	if (sysctlbyname("hw.ncpu", &ncpu, &size, NULL, 0) != 0 || ncpu < 1) {
		return 2;
	}
	half = (unsigned)ncpu / 2;
	return half < 2 ? 2 : half;
}

/*
 * The picture into the buffer: the render in pieces, the photograph's mean levels, and for the
 * X formats the 8-to-16-bit expansion in place. A photograph's turn through this was ~110 ms
 * (isp_render_still_nv12) and is longer governed, on purpose: it runs on the stills' queue
 * (still_enqueue/still_run), and the worker that owes the next preview frame is not made to
 * wait for it.
 *
 * Governed, a photograph's pieces go half the machine's cores wide at QOS_CLASS_UTILITY; the
 * previews' pieces stay 24 wide at QOS_CLASS_USER_INTERACTIVE. As it was, one photograph's
 * every-pixel demosaic spread over all four cores the rig boots with (cpus=4) for ~100 ms;
 * the preview's pieces, same queue and same priority, waited behind it, and a frame's
 * processing here inflated to a mean of twice, at worst five times, its ~10 ms baseline
 * through both photographs' renders (out/goal-1124/batch5/post-capture-gap-v2.md, b5r6 §2.4).
 * At utility the pieces sit below the capture worker and every interactive piece: nothing a
 * photograph does holds a core against a frame's capture or render, and at half width they
 * leave half the machine even if nothing preempts them. The picture itself is untouched --
 * the same rows through the same math, only the scheduling changes; what the photograph pays
 * is wall time, the render at least doubling inside the seconds its encoding already takes.
 * The rigging's switch returns the swarm as it was for the comparison round; it is read for
 * each photograph, and the render's line says the pieces and QoS as they ran.
 */
static void
render_picture(struct output *out, CVPixelBufferRef pb, uint32_t pf, uint32_t width, uint32_t height,
    const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw, bool still)
{
	struct render_job job = { isp, f, raw, pb, pf, width, height, 24, still && pf != PF_BGRA };
	bool governed = still && !rigging_switch_on("ANNIBALE_STILL_SWARM", "/private/var/tmp/annibale_still_swarm");
	uint64_t began, applied, sampled, expanded;
	double still_y = 0, still_cb = 0, still_cr = 0;

	if (governed) {
		job.pieces = still_render_pieces();
	}
	CVPixelBufferLockBaseAddress(pb, 0);
	began = mach_absolute_time();
	dispatch_apply_f(job.pieces, dispatch_get_global_queue(governed ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INTERACTIVE, 0), &job,
	    render_piece);
	applied = mach_absolute_time();
	if (job.still) {
		/* (the picture as made, 8 bits a value, every 16th pixel each way: what the photograph starts from) */
		const uint8_t *yp = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *cp = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
		size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), cs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
		unsigned n = 0;

		for (uint32_t y = 0; y + 1 < height; y += 16) {
			for (uint32_t x = 0; x + 1 < width; x += 16, n++) {
				still_y += yp[(size_t)y * ys + x];
				still_cb += cp[(size_t)(y / 2) * cs + (x & ~1u)];
				still_cr += cp[(size_t)(y / 2) * cs + (x & ~1u) + 1];
			}
		}
		if (n > 0) {
			still_y /= n;
			still_cb /= n;
			still_cr /= n;
		}
	}
	sampled = mach_absolute_time();
	if (pf == PF_X420 || pf == PF_X422 || pf == PF_XF20) {
		uint8_t *y_base = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
		size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
		uint8_t *uv_base = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
		size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);

		for (uint32_t y = 0; y < height; y++) {
			uint8_t *r8 = y_base + (size_t)y * y_stride;
			uint16_t *r16 = (uint16_t *)r8;

			for (int32_t x = (int32_t)width - 1; x >= 0; x--) {
				r16[x] = (uint16_t)r8[x] << 8;
			}
		}
		if (pf == PF_X422) {
			for (int32_t y = (int32_t)height - 1; y >= 0; y--) {
				const uint8_t *r8 = uv_base + (size_t)(y / 2) * uv_stride;
				uint16_t *r16 = (uint16_t *)(uv_base + (size_t)y * uv_stride);

				for (int32_t x = (int32_t)width - 1; x >= 0; x--) {
					r16[x] = (uint16_t)r8[x] << 8;
				}
			}
		} else {
			for (uint32_t y = 0; y < height / 2; y++) {
				uint8_t *r8 = uv_base + (size_t)y * uv_stride;
				uint16_t *r16 = (uint16_t *)r8;

				for (int32_t x = (int32_t)width - 1; x >= 0; x--) {
					r16[x] = (uint16_t)r8[x] << 8;
				}
			}
		}
	}
	expanded = mach_absolute_time();
	if (job.still) {
		static mach_timebase_info_data_t tb;

		if (tb.denom == 0) {
			mach_timebase_info(&tb);
		}
		double ms = (double)tb.numer / tb.denom / 1e6;

		say("%s: %ux%u %c%c%c%c made of every pixel in %.0f ms (demosaic %.0f, means %.0f, 8-to-16 expansion %.0f; "
		    "%u pieces on %s); Y %.0f Cb %.0f Cr %.0f; exposure %u lines gain %.2fx, AE's tone curve %.2f",
		    out->name, width, height, pf >> 24, pf >> 16 & 255, pf >> 8 & 255, pf & 255,
		    (mach_absolute_time() - began) * ms, (applied - began) * ms, (sampled - applied) * ms, (expanded - sampled) * ms,
		    job.pieces, governed ? "QOS_CLASS_UTILITY" : "QOS_CLASS_USER_INTERACTIVE", still_y, still_cb, still_cr,
		    f->exposure_lines, f->gain_unity ? (double)f->gain / f->gain_unity : 0, isp->drc);
	}
	CVPixelBufferUnlockBaseAddress(pb, 0);
}

static void
attach_metadata(struct stream *s, struct output *out, CVPixelBufferRef pb, CFDictionaryRef metadata,
    const struct isp *isp, const struct shot_env *env)
{
	CFMutableDictionaryRef m;

	if (metadata == NULL) {
		return;
	}
	m = CFDictionaryCreateMutableCopy(NULL, 0, metadata);
	if (strncmp(out->name, "StillImage", 10) == 0) {
		put(m, "SensorID", number_i32(s->camera->sensor == 1 ? 0x0904 : 0x0914));
		if (env->exif_orientation != 0) {
			put(m, "ExifOrientation", number_i32(env->exif_orientation));
		}
		put(m, "AWBRGain", number_i32((int32_t)(isp->wb[0] * 4096.0f + 0.5f)));
		put(m, "AWBGGain", number_i32(4096));
		put(m, "AWBBGain", number_i32((int32_t)(isp->wb[2] * 4096.0f + 0.5f)));
		if (env->lux >= 0 && isfinite(env->lux)) {
			put(m, "LuxLevel", number_f(env->lux));
		}
		if (env->cct > 0 && isfinite(env->cct)) {
			put(m, "CCT", number_f(env->cct));
		}
		if (env->redeye) {
			put(m, "RedEyeAuxiliaryImage", CFRetain(kCFBooleanTrue));
		}
		if (env->torch) {
			CFMutableDictionaryRef exif = (CFMutableDictionaryRef)new_dict();

			put(exif, "Flash", number_i32(env->flash_lit ? 1 : 16));
			put(m, "{Exif}", exif);
		}
	}
	CVBufferSetAttachment(pb, md_attachment, m, kCVAttachmentMode_ShouldPropagate);
	CFRelease(m);
}

static void
deliver(struct stream *s, struct output *out, const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw,
    CFDictionaryRef metadata)
{
	uint32_t pf, width, height;
	CVPixelBufferRef pb = picture_buffer(out, &pf, &width, &height);
	struct shot_env env;
	bool is_still;

	if (pb == NULL) {
		return;
	}
	is_still = strncmp(out->name, "StillImage", 10) == 0;
	render_picture(out, pb, pf, width, height, isp, f, raw, is_still);
	CVBufferSetAttachments(pb, colour_attachments(), kCVAttachmentMode_ShouldPropagate);
	env = (struct shot_env){ s->last_lux, s->last_cct, s->still_redeye_wanted, s->flash_lit, s->torch >= 0,
	    still_exif_orientation(s) };
	attach_metadata(s, out, pb, metadata, isp, &env);

	CMTime pts = CMClockMakeHostTimeFromSystemUnits(f->time);

	((pixel_buffer_handler)out->handler)(pb, pts);
	/*
	 * The moving picture's buffers are kept for HELD frames after they are given. The preview's pool
	 * would otherwise hand a buffer straight back -- the graph lets go of it as soon as it has passed
	 * it on -- while it is still on its way to the screen (the preview sink, Camera.app's image queue,
	 * the window server copying it), and the next frame drawn into it showed as bands of two frames,
	 * torn across. Kept, they make the pool take others. (One kept was not enough.)
	 */
	if (!is_still) {
		if (out->held[out->held_next] != NULL) {
			CVPixelBufferRelease(out->held[out->held_next]);
		}
		out->held[out->held_next] = pb;
		out->held_next = (out->held_next + 1) % HELD;
	} else {
		CVPixelBufferRelease(pb);
	}
	if (out->delivered++ == 0) {
		say("%s: the first picture given (%ux%u %c%c%c%c)", out->name, width, height, pf >> 24, pf >> 16 & 255,
		    pf >> 8 & 255, pf & 255);
	}
}

/*
 * A photograph does not render on this worker: ~110 ms of every-pixel demosaic is ~130 ms of no
 * preview frames on screen (the shutter-stall analysis in out/goal-1124/batch2/u21-shutter.md).
 * The still outputs' frames go to the stills' serial queue instead -- each as a self-contained job:
 * the frame's bytes, the ISP as this frame had it, the pool's buffer and the handler taken while the
 * frame is here -- and the worker goes on to the next frame. The queue's order keeps the
 * photographs' order; the render itself fans out governed, half width at utility priority
 * where the previews' renders stay wide and interactive (render_picture).
 */
struct still_job {
	struct stream *s;
	struct output *out;
	void *          handler;        /* the block, kept for the queue's run of it */
	CVPixelBufferRef pb;
	CFDictionaryRef metadata;
	struct isp      isp;            /* this frame's state; the tuning pointer is borrowed, not used */
	struct shot_env env;            /* the frame's, for the photograph's own metadata */
	uint32_t        pf, width, height;
	uint32_t        frame_bytes;
	int             slot_idx;       /* index into s->still_pool, or -1 once released */
	CMTime          pts;
	uint64_t        enqueued;       /* when it went on the queue (mach_absolute_time): the wait before it renders */
};

static void
still_release_slot(struct stream *s, int *slot_idx)
{
	if (*slot_idx >= 0 && *slot_idx < STILL_POOL_SLOTS) {
		pthread_mutex_lock(&s->lock);
		s->still_pool[*slot_idx].busy = false;
		pthread_mutex_unlock(&s->lock);
		*slot_idx = -1;
	}
}

static void
still_free(struct still_job *j)
{
	still_release_slot(j->s, &j->slot_idx);
	CVPixelBufferRelease(j->pb);
	if (j->handler != NULL) {
		CFRelease(j->handler);
	}
	if (j->metadata != NULL) {
		CFRelease(j->metadata);
	}
	free(j);
}

static void
still_deliver(void *context)
{
	struct still_job *j = context;
	struct stream *s = j->s;
	static mach_timebase_info_data_t tb;
	bool live;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	pthread_mutex_lock(&s->lock);
	live = s->state == CAM_STREAM_ACTIVE;
	pthread_mutex_unlock(&s->lock);
	if (live && j->handler != NULL) {
		uint64_t hand = mach_absolute_time();

		((pixel_buffer_handler)j->handler)(j->pb, j->pts);
		double handler_ms = (double)(mach_absolute_time() - hand) * tb.numer / tb.denom / 1e6;

		if (j->out->delivered++ == 0) {
			say("%s: the first picture given (%ux%u %c%c%c%c)", j->out->name, j->width, j->height,
			    j->pf >> 24, j->pf >> 16 & 255, j->pf >> 8 & 255, j->pf & 255);
		}
		pthread_mutex_lock(&s->lock);
		say("the photograph is given (%llu so far): %.0f ms in the graph's handler",
		    (unsigned long long)++s->stills_given, handler_ms);
		pthread_mutex_unlock(&s->lock);
	}
	still_free(j);
}

static void
still_run(void *context)
{
	struct still_job *j = context;
	struct stream *s = j->s;
	const uint8_t *frame = s->still_pool[j->slot_idx].buf;
	const struct annibale_camera_frame *f = (const void *)frame;
	const uint8_t *raw = frame + f->header_bytes;
	static mach_timebase_info_data_t tb;
	bool live;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	pthread_mutex_lock(&s->lock);
	live = s->state == CAM_STREAM_ACTIVE;
	pthread_mutex_unlock(&s->lock);
	/*
	 * A stopped stream drops the photographs queued but not begun; one already rendering finishes
	 * and hands its rendered buffer to still_deliver_queue. The RAW pool slot is returned as soon
	 * as demosaic finishes so bursts reuse it while CMCapture encodes the previous photograph.
	 */
	if (live) {
		j->pts = CMClockMakeHostTimeFromSystemUnits(f->time);
		say("the %s's photograph of frame %llu begins rendering after %.0f ms in the queue", j->out->name, f->sequence,
		    (double)(mach_absolute_time() - j->enqueued) * tb.numer / tb.denom / 1e6);
		render_picture(j->out, j->pb, j->pf, j->width, j->height, &j->isp, f, raw, true);
		CVBufferSetAttachments(j->pb, colour_attachments(), kCVAttachmentMode_ShouldPropagate);
		attach_metadata(s, j->out, j->pb, j->metadata, &j->isp, &j->env);
	}
	still_release_slot(s, &j->slot_idx);
	if (live) {
		pthread_mutex_lock(&s->lock);
		live = s->state == CAM_STREAM_ACTIVE;
		pthread_mutex_unlock(&s->lock);
	}
	if (live && s->still_deliver_queue != NULL) {
		dispatch_async_f(s->still_deliver_queue, j, still_deliver);
		return;
	}
	if (live) {
		still_deliver(j);
		return;
	}
	still_free(j);
}

/*
 * Reserve a RAW ring-pool slot and snapshot metadata under s->lock; the ~6 MB RAW copy and queue
 * dispatch happen after s->lock is released so preview/control callers never stall on memcpy.
 */
static struct still_job *
still_prepare(struct stream *s, struct output *out, const struct isp *isp, uint32_t frame_bytes,
    CFDictionaryRef metadata)
{
	uint32_t pf, width, height;
	int slot_idx = -1;

	for (int i = 0; i < STILL_POOL_SLOTS; i++) {
		if (!s->still_pool[i].busy) {
			slot_idx = i;
			break;
		}
	}
	if (slot_idx < 0) {
		say("%s: all %d still RAW slots busy: photograph skipped", out->name, STILL_POOL_SLOTS);
		return NULL;
	}
	if (s->still_pool[slot_idx].cap < frame_bytes) {
		uint8_t *nb = realloc(s->still_pool[slot_idx].buf, frame_bytes);
		if (nb == NULL) {
			say("%s: no room for the photograph's frame: skipped", out->name);
			return NULL;
		}
		s->still_pool[slot_idx].buf = nb;
		s->still_pool[slot_idx].cap = frame_bytes;
	}
	CVPixelBufferRef pb = picture_buffer(out, &pf, &width, &height);
	if (pb == NULL) {
		return NULL;
	}
	struct still_job *j = calloc(1, sizeof(*j));
	if (j == NULL) {
		CVPixelBufferRelease(pb);
		say("%s: no room for the photograph's job: skipped", out->name);
		return NULL;
	}
	s->still_pool[slot_idx].busy = true;
	j->s = s;
	j->out = out;
	j->handler = out->handler != NULL ? (void *)CFRetain(out->handler) : NULL;
	j->pb = pb;
	j->metadata = metadata != NULL ? (CFDictionaryRef)CFRetain(metadata) : NULL;
	j->isp = *isp;
	j->isp.tuning = NULL;
	j->env = (struct shot_env){ s->last_lux, s->last_cct, s->still_redeye_wanted, s->flash_lit, s->torch >= 0,
	    still_exif_orientation(s) };
	j->pf = pf;
	j->width = width;
	j->height = height;
	j->frame_bytes = frame_bytes;
	j->slot_idx = slot_idx;
	return j;
}

static void
still_dispatch(struct stream *s, struct still_job *j, const uint8_t *frame)
{
	memcpy(s->still_pool[j->slot_idx].buf, frame, j->frame_bytes);
	j->enqueued = mach_absolute_time();
	if (s->still_queue != NULL) {
		dispatch_async_f(s->still_queue, j, still_run);
	} else {
		say("%s: the stills' queue is not there: the photograph is skipped", j->out->name);
		still_free(j);
	}
}

#include "camera_device_worker.inc"

/* Frames are read from the kernel from the graph's Start of a stream until its Stop. */
#define MAX_STREAMS 2
static CMBaseObjectRef all_streams[MAX_STREAMS];
static OSStatus stream_stop(CMBaseObjectRef o);

static OSStatus
stream_start(CMBaseObjectRef o)
{
	struct stream *s = stream_of(o);

	if (s->profile.bytes == 0) {
		return ERR_NOT_AVAILABLE;
	}
	if (s->state == CAM_STREAM_FAULTED && s->worker_started) {
		pthread_join(s->worker, NULL);
		s->worker_started = false;
		s->state = CAM_STREAM_STOPPED;
	}
	if (s->state == CAM_STREAM_ACTIVE) {
		return 0;
	}
	/* one camera at a time: the kernel gives one sensor's frames (the other stream's worker stopped first) */
	for (int i = 0; i < MAX_STREAMS; i++) {
		if (all_streams[i] != NULL && all_streams[i] != o &&
		    stream_of(all_streams[i])->state != CAM_STREAM_STOPPED) {
			say("stream: the %s's frames stop for the %s's", stream_of(all_streams[i])->camera->name, s->camera->name);
			(void)stream_stop(all_streams[i]);
		}
	}
	s->state = CAM_STREAM_ACTIVE;
	s->frames = 0;
	say("stream: the graph starts the stream (format %d)", s->format_index);
	if (pthread_create(&s->worker, NULL, worker, o) != 0) {
		s->state = CAM_STREAM_STOPPED;
		return ERR_UNSUPPORTED;
	}
	s->worker_started = true;
	return 0;
}

static OSStatus
stream_stop(CMBaseObjectRef o)
{
	struct stream *s = stream_of(o);

	say("stream: stop (%llu frames, %llu pictures given)", (unsigned long long)s->frames,
	    (unsigned long long)s->out[0].delivered);
	pthread_mutex_lock(&s->lock);
	s->state = CAM_STREAM_STOPPED;
	for (int i = 0; i < OUTPUTS; i++) {
		let_go(&s->out[i]);
	}
	pthread_mutex_unlock(&s->lock);
	if (s->worker_started) {
		pthread_join(s->worker, NULL);
		s->worker_started = false;
	}
	pthread_mutex_lock(&s->lock);
	if (s->flash_lit && s->torch >= 0) {
		uint32_t dark = 0;

		(void)write(s->torch, &dark, sizeof(dark));
		s->flash_lit = false;
	}
	s->still_flash_wanted = false;
	s->still_redeye_wanted = false;
	s->stills_wanted = 0;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

static void
add_absent(const void *key, const void *value, void *context)
{
	if (!CFDictionaryContainsKey(context, key)) {
		CFDictionarySetValue(context, key, value);
	}
}

/*
 * The frame's metadata into the sample buffer the graph made of our pixel buffer: the pixel buffer's
 * own dictionary (deliver()), the same object, else the last frame's; whatever the graph has put in a
 * dictionary of the sample buffer's already stays as it is.
 */
static OSStatus
stream_add_attachments(CMBaseObjectRef o, CMSampleBufferRef sbuf, CFDictionaryRef options)
{
	struct stream *s = stream_of(o);
	static bool said;
	CVImageBufferRef image = sbuf != NULL ? CMSampleBufferGetImageBuffer(sbuf) : NULL;
	CFTypeRef ours = image != NULL && md_attachment != NULL ? CVBufferCopyAttachment(image, md_attachment, NULL) : NULL, theirs;
	CFMutableDictionaryRef m;

	if (!said) {
		said = true;
		say("stream: AddAttachmentsToSampleBuffer (options %s): the frames' metadata %s", options ? "given" : "none",
		    md_attachment != NULL ? "attached" : "NOT THERE");
	}
	if (md_attachment == NULL || sbuf == NULL) {
		if (ours != NULL) {
			CFRelease(ours);
		}
		return 0;
	}
	if (ours == NULL || CFGetTypeID(ours) != CFDictionaryGetTypeID()) {
		if (ours != NULL) {
			CFRelease(ours);
		}
		pthread_mutex_lock(&s->metadata_lock);
		ours = s->metadata != NULL ? CFDictionaryCreateMutableCopy(NULL, 0, s->metadata) : NULL;
		pthread_mutex_unlock(&s->metadata_lock);
	}
	if (ours == NULL) {
		return 0;
	}
	theirs = CMGetAttachment(sbuf, md_attachment, NULL);
	if (theirs == ours) {
		CFRelease(ours);
		return 0;
	}
	if (theirs != NULL && CFGetTypeID(theirs) == CFDictionaryGetTypeID()) {
		m = CFDictionaryCreateMutableCopy(NULL, 0, theirs);
		CFDictionaryApplyFunction(ours, add_absent, m);
	} else {
		m = (CFMutableDictionaryRef)CFRetain(ours);
	}
	CMSetAttachment(sbuf, md_attachment, m, kCMAttachmentMode_ShouldPropagate);
	CFRelease(m);
	CFRelease(ours);
	return 0;
}

static OSStatus
stream_invalidate(CMBaseObjectRef o)
{
	(void)stream_stop(o);
	return 0;
}

static void
stream_finalize(CMBaseObjectRef o)
{
	struct stream *s = stream_of(o);

	(void)stream_stop(o);
	for (int i = 0; i < MAX_STREAMS; i++) {
		if (all_streams[i] == o) {
			all_streams[i] = NULL;
		}
	}
	for (int i = 0; i < OUTPUTS; i++) {
		if (s->out[i].config) {
			CFRelease(s->out[i].config);
		}
		if (s->out[i].handler) {
			CFRelease(s->out[i].handler);
		}
	}
	if (s->formats) {
		CFRelease(s->formats);
	}
	if (s->supported) {
		CFRelease(s->supported);
	}
	if (s->face_config) {
		CFRelease(s->face_config);
	}
	if (s->meta_config) {
		CFRelease(s->meta_config);
	}
	if (s->metadata) {
		CFRelease(s->metadata);
	}
	if (s->still_queue != NULL) {
		dispatch_release(s->still_queue);
	}
	if (s->still_deliver_queue != NULL) {
		dispatch_release(s->still_deliver_queue);
	}
	for (int i = 0; i < STILL_POOL_SLOTS; i++) {
		free(s->still_pool[i].buf);
	}
	if (s->torch >= 0) {
		close(s->torch);
	}
}

static CFStringRef
stream_description(CMBaseObjectRef o)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("<AnnibaleCaptureStream: %s>"), stream_of(o)->camera->name);
}

static const struct base_class stream_base = {
	1, sizeof(struct stream), NULL, stream_invalidate, stream_finalize, stream_description, stream_copy_property, stream_set_property,
	NULL, NULL, NULL
};
static const struct stream_class stream_derived = { 2, stream_start, stream_stop, stream_add_attachments };
static const struct stream_vtable stream_vtable = { NULL, &stream_base, &stream_derived };

static CMBaseObjectRef
stream_create(CFAllocatorRef allocator, const struct camera *camera, CFStringRef port,
    const struct annibale_camera_profile *profile, int torch_fd)
{
	CMBaseObjectRef o = NULL;
	struct stream *s;

	if (derived_create(allocator, &stream_vtable, stream_class_id(), &o) != 0 || o == NULL) {
		say("the stream cannot be made");
		return NULL;
	}
	s = stream_of(o);
	memset(s, 0, sizeof(*s));
	pthread_mutex_init(&s->lock, NULL);
	pthread_mutex_init(&s->metadata_lock, NULL);
	dispatch_queue_attr_t still_attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
	    QOS_CLASS_USER_INITIATED, 0);
	s->still_queue = dispatch_queue_create("annibale-camera-still", still_attr);
	s->still_deliver_queue = dispatch_queue_create("annibale-camera-still-deliver", still_attr);
	s->torch = torch_fd;
	s->zoom = 1;
	s->exposure_bias = s->said_bias = 256;
	/* (CMCapture's for the wide camera, until it sends them) */
	s->af_search = (struct af_search){ 13, { 13, 31, 48, 66, 86, 107, 128, 149, 171, 192, 213, 234, 255 }, true, 2, 56 / 256.0f,
		true, true };
	s->af_window[0] = s->af_window[1] = 0.3f;
	s->af_window[2] = s->af_window[3] = 0.7f;
	s->camera = camera;
	s->port = port;
	for (int i = 0; i < OUTPUTS; i++) {
		s->out[i].name = output_names[i];
	}

	if (profile != NULL) {
		s->profile = *profile;
		s->min_fps = s->max_fps = profile->fps_milli / 1000.0;
	}
	s->formats = build_stream_formats(profile, camera->field_of_view);
	if (profile != NULL) {
		CFMutableDictionaryRef sup = (CFMutableDictionaryRef)supported(stream_props,
		    sizeof(stream_props) / sizeof(stream_props[0]));

		if (torch_fd >= 0) {
			CFMutableDictionaryRef tprop = (CFMutableDictionaryRef)new_dict();

			put(tprop, "PropertyType", CFRetain(CFSTR("Number")));
			put(tprop, "ReadWriteStatus", CFRetain(CFSTR("ReadWrite")));
			put(sup, "TorchLevel", tprop);
		}
		s->supported = sup;
	} else {
		s->supported = supported(torch_props, sizeof(torch_props) / sizeof(torch_props[0]));
	}
	return o;
}
