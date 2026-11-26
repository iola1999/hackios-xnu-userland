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
	uint64_t        enqueued;       /* when it went on the queue (mach_absolute_time): the wait before it renders */
	uint8_t         frame[];        /* the frame and the RAW picture after it, copied */
};

static void
still_free(struct still_job *j)
{
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
still_run(void *context)
{
	struct still_job *j = context;
	struct stream *s = j->s;
	const struct annibale_camera_frame *f = (const void *)j->frame;
	const uint8_t *raw = j->frame + f->header_bytes;
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
	 * and gives its buffer, as a frame mid-deliver() does. The handlers outlive the queue: the
	 * worker's exit waits for what is queued before join returns.
	 */
	if (live) {
		say("the %s's photograph of frame %llu begins rendering after %.0f ms in the queue", j->out->name, f->sequence,
		    (double)(mach_absolute_time() - j->enqueued) * tb.numer / tb.denom / 1e6);
		render_picture(j->out, j->pb, j->pf, j->width, j->height, &j->isp, f, raw, true);
		CVBufferSetAttachments(j->pb, colour_attachments(), kCVAttachmentMode_ShouldPropagate);
		attach_metadata(s, j->out, j->pb, j->metadata, &j->isp, &j->env);
		pthread_mutex_lock(&s->lock);
		live = s->state == CAM_STREAM_ACTIVE;
		pthread_mutex_unlock(&s->lock);
	}
	if (live) {
		CMTime pts = CMClockMakeHostTimeFromSystemUnits(f->time);
		uint64_t hand = mach_absolute_time();

		((pixel_buffer_handler)j->handler)(j->pb, pts);
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

/*
 * A still output's frame is spent here only for the copying (~6 MB of RAW, single-digit ms); the
 * render waits its turn on the queue. Called under s->lock, as the output loop around it is.
 */
static void
still_enqueue(struct stream *s, struct output *out, const struct isp *isp, const uint8_t *frame,
    uint32_t frame_bytes, CFDictionaryRef metadata)
{
	uint32_t pf, width, height;
	CVPixelBufferRef pb = picture_buffer(out, &pf, &width, &height);
	struct still_job *j;

	if (pb == NULL) {
		return;
	}
	j = malloc(sizeof(*j) + frame_bytes);
	if (j == NULL) {
		CVPixelBufferRelease(pb);
		say("%s: no room for the photograph's frame: skipped", out->name);
		return;
	}
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
	memcpy(j->frame, frame, frame_bytes);
	j->enqueued = mach_absolute_time();
	if (s->still_queue != NULL) {
		dispatch_async_f(s->still_queue, j, still_run);
	} else {
		say("%s: the stills' queue is not there: the photograph is skipped", out->name);
		still_free(j);
	}
}

/*
 * The scene's light as the cameras' tuning measures it (Qualcomm's lux index, higher is darker): about
 * 46 a doubling of exposure times gain, 197 at a 33 ms exposure and 1x -- the black level's ranges in
 * the tuning of the wide camera pair lux indexes with gains, 195-200 with 1-1.1x and 380-385 with
 * 15.9-16x, at the longest exposure a frame at 30 a second allows.
 */
static float
lux_index(const struct isp *isp, const struct annibale_camera_frame *f)
{
	if (f->exposure_lines == 0 || f->line_ns == 0 || f->gain == 0 || f->gain_unity == 0) {
		return NAN;
	}
	double exposure = f->exposure_lines * (double)f->line_ns / 1e9 * f->gain / f->gain_unity * isp->digital;

	return (float)(197 + 46 * log2(exposure > 1e-6 ? exposure / 0.0333 : 1e-6 / 0.0333));
}

/*
 * The light's colour temperature: where the white balance is on the line of the tuning's reference
 * lights; without them, guessed from how much more the blue is raised than the red, by what is typical of a phone's sensor --
 * blue's gain over red's about 0.8 in daylight (6500 K), 1.3 under a fluorescent tube (4000 K), 2 under
 * a filament lamp (2850 K) -- between those on the logarithm of the ratio, and no further.
 */
static float
colour_temperature(const struct isp *isp)
{
	static const float ratio[3] = { 0.8f, 1.3f, 2.0f }, kelvin[3] = { 6500, 4000, 2850 };
	float r = isp->wb[0] > 0 ? isp->wb[2] / isp->wb[0] : 1, cct = isp_cct(isp);

	if (cct > 0) {
		return cct;     /* (by the tuning's reference lights) */
	}

	if (r <= ratio[0]) {
		return kelvin[0];
	}
	for (int i = 1; i < 3; i++) {
		if (r <= ratio[i]) {
			float t = logf(r / ratio[i - 1]) / logf(ratio[i] / ratio[i - 1]);

			return kelvin[i - 1] + t * (kelvin[i] - kelvin[i - 1]);
		}
	}
	return kelvin[2];
}

/*
 * Focus, where the camera has a lens motor (the frames' focus is a position, not ANNIBALE_CAMERA_NO_FOCUS:
 * the wide camera's): by contrast, as there is nothing else to go by -- the sensor's phase-detection
 * pixels are not read. A search goes through CMCapture's search positions (AFSearchPositions, 0 to 255
 * over the lens's travel), from the end nearer the lens, and stops early as AFEarlyOutParams say; the
 * lens then goes to the peak of a parabola through the sharpest position and its two neighbours
 * (AFPeakPredictionEnabled). Each position is measured on the first frame taken there: the lens moves
 * while the frame before it is exposed.
 */
enum { AF_WATCH, AF_STEADY, AF_SEARCH, AF_SETTLE };
#define AF_NEAR         16              /* (off the stop at 0) */
#define AF_FAR          768             /* (its module file's actuator region goes to 700) */

#define AF_LONG_MOVE    100             /* lens positions: a move this long takes a frame more to settle */

struct af {
	int             state;
	uint32_t        at;             /* where the lens was told to go */
	struct af_search search;        /* the stream's, as it was when this search began */
	uint32_t        pos[AF_POSITIONS];      /* the search's lens positions, in the order taken */
	float           sharp[AF_POSITIONS];
	unsigned        next, best, below;      /* the position being measured; the sharpest; how many since below it */
	float           sharpness, level;       /* in focus: the middle's, to watch */
	float           last_sharpness, last_level;
	unsigned        unlike, steady, steps;
	bool            now;            /* asked for: no waiting for the picture to keep still */
	float           sum_sharpness, sum_level;
	unsigned        measured;
	unsigned        lag;            /* frames not yet taken where the lens was told to go */
	unsigned        skip;           /* frames taken there to pass over before measuring */
	uint64_t        began;          /* when the search began (mach_absolute_time) */
};

static void
af_go(int device, struct af *af, uint32_t target)
{
	struct annibale_camera_command c = { .command = ANNIBALE_CAMERA_FOCUS };
	uint32_t from = af->at;

	af->at = c.a = target < AF_NEAR ? AF_NEAR : target > AF_FAR ? AF_FAR : target;
	af->steps++;
	af->lag = 0;
	/*
	 * The frame after a move of a search step (62 positions) already has the new position's sharpness;
	 * after a longer one the lens is still moving there: the first frame after the move back to the
	 * peak (130 positions) read 0.011 against 0.025 searched beside it, and one at the start of a
	 * search (330 positions) read as sharp as the place it left (kernel-12377-af1-w9, -w10).
	 */
	af->skip = (from > af->at ? from - af->at : af->at - from) > AF_LONG_MOVE ? 1 : 0;
	af->sum_sharpness = af->sum_level = 0;
	af->measured = 0;
	(void)write(device, &c, sizeof(c));
}

static void
af_search(int device, struct af *af)
{
	const struct af_search *p = &af->search;
	uint32_t first = AF_NEAR + p->position[0] * (AF_FAR - AF_NEAR) / 255,
	    last = AF_NEAR + p->position[p->count - 1] * (AF_FAR - AF_NEAR) / 255;
	bool backwards = (af->at > first ? af->at - first : first - af->at) > (af->at > last ? af->at - last : last - af->at);

	for (unsigned i = 0; i < p->count; i++) {
		af->pos[i] = AF_NEAR + p->position[backwards ? p->count - 1 - i : i] * (AF_FAR - AF_NEAR) / 255;
	}
	af->state = AF_SEARCH;
	af->next = af->best = af->below = 0;
	af->steps = 0;
	af->began = mach_absolute_time();
	af_go(device, af, af->pos[0]);
}

/* the sharpest position, or the peak of the parabola through it and its neighbours */
static uint32_t
af_peak(const struct af *af)
{
	unsigned b = af->best;

	if (!af->search.peak_prediction || b == 0 || b >= af->next) {
		return af->pos[b];
	}
	double x0 = af->pos[b - 1], x1 = af->pos[b], x2 = af->pos[b + 1];
	double y0 = af->sharp[b - 1], y1 = af->sharp[b], y2 = af->sharp[b + 1];
	double d = (x0 - x1) * (x0 - x2) * (x1 - x2);
	double a = (x2 * (y1 - y0) + x1 * (y0 - y2) + x0 * (y2 - y1)) / d;
	double bb = (x2 * x2 * (y0 - y1) + x1 * x1 * (y2 - y0) + x0 * x0 * (y1 - y2)) / d;

	if (a >= 0) {
		return af->pos[b];
	}
	double peak = -bb / (2 * a), lo = x0 < x2 ? x0 : x2, hi = x0 < x2 ? x2 : x0;

	return (uint32_t)(peak < lo ? lo : peak > hi ? hi : peak + 0.5);
}

static void
af_frame(int device, struct af *af, const struct annibale_camera_frame *f, const uint8_t *raw, const float window[4], bool locked)
{
	float level, sharpness;
	unsigned needed = f->gain >= 16 * f->gain_unity ? 2 : 1;      /* (in the dark, two frames to a measurement) */

	if (f->focus == ANNIBALE_CAMERA_NO_FOCUS) {
		return;
	}
	if (af->at == 0) {
		af->at = f->focus;              /* (where the kernel put it) */
	}
	/* A frame reports the lens position it was taken at: measure the ones taken where it was told. */
	if (f->focus != af->at && af->lag < 4) {
		af->lag++;
		return;
	}
	af->lag = 0;
	if (af->skip > 0) {
		af->skip--;
		return;
	}
	sharpness = isp_sharpness(f, raw, window[0], window[1], window[2], window[3], &level);
	af->sum_sharpness += sharpness;
	af->sum_level += level;
	if (++af->measured < needed) {
		return;
	}
	sharpness = af->sum_sharpness / af->measured;
	level = af->sum_level / af->measured;
	af->sum_sharpness = af->sum_level = 0;
	af->measured = 0;
	switch (af->state) {
	case AF_WATCH:
		if (locked || (af->sharpness > 0 && sharpness > af->sharpness * 0.6f && level > af->level * 0.7f &&
		    level < af->level * 1.4f)) {
			af->unlike = 0;
			return;
		}
		if (af->sharpness > 0 && ++af->unlike < 30) {
			return;
		}
		af->state = AF_STEADY;
		af->steady = 0;
		af->last_sharpness = sharpness;
		af->last_level = level;
		return;
	case AF_STEADY:
		if (fabsf(sharpness - af->last_sharpness) < 0.15f * af->last_sharpness && fabsf(level - af->last_level) < 0.1f * af->last_level) {
			af->steady++;
		} else {
			af->steady = 0;
		}
		af->last_sharpness = sharpness;
		af->last_level = level;
		if (af->steady < 10 && !af->now) {
			return;
		}
		af->now = false;
		af_search(device, af);
		return;
	case AF_SEARCH: {
		const struct af_search *p = &af->search;

		say("focus: searched %u: sharpness %.4f level %.1f frame %llu", af->at, sharpness, level, f->sequence);
		af->sharp[af->next] = sharpness;
		if (sharpness > af->sharp[af->best]) {
			af->best = af->next;
			af->below = 0;
		} else if (sharpness < af->sharp[af->best] * (1 - p->early_out_drop) && (af->best > 0 || p->early_out_at_start)) {
			af->below++;
		}
		if (af->next + 1 < p->count && !(p->early_out && af->below >= p->early_out_count)) {
			af_go(device, af, af->pos[++af->next]);
			return;
		}
		af->state = AF_SETTLE;
		af_go(device, af, af_peak(af));
		return;
	}
	case AF_SETTLE: {
		static mach_timebase_info_data_t tb;

		if (tb.denom == 0) {
			mach_timebase_info(&tb);
		}
		af->state = AF_WATCH;
		af->sharpness = sharpness;
		af->level = level;
		af->unlike = 0;
		say("focus: the lens at %u after %u moves in %.0f ms (sharpness %.3f, the best searched %.3f at %u)", af->at,
		    af->steps, (double)(mach_absolute_time() - af->began) * tb.numer / tb.denom / 1e6, sharpness,
		    af->sharp[af->best], af->pos[af->best]);
		return;
	}
	}
}

/* the sensor's colour tuning, when the image has one (fw/camera-<sensor>.isp, tools/camtuning.py) */
static void
load_tuning(struct isp *isp, uint32_t sensor)
{
	/* by sensor number (cameras[] in camera_device.c), as build.sh names the file of each tuning */
	static const char *const names[] = { NULL, "ovx8000", "ov20b40" };
	char path[96];

	if (sensor >= sizeof(names) / sizeof(names[0]) || names[sensor] == NULL) {
		return;
	}
	snprintf(path, sizeof(path), "/annibale/fw/camera-%s.isp", names[sensor]);
	if (isp_load_tuning(isp, path)) {
		say("the %s's colour tuning from %s: %u colour matrices' lights, %u lens shadings'", names[sensor], path,
		    isp->tuning->ccm.lights, isp->tuning->lsc.lights);
	} else {
		say("no colour tuning for the %s (%s): no colour matrix, no lens shading", names[sensor], path);
		isp_set_ccm(isp, (const float[9]){ 1, 0, 0, 0, 1, 0, 0, 0, 1 });
		isp->lsc.on = false;
	}
}

struct flash_meter {
	unsigned frames;
	uint32_t lines, gain;
};

static bool
flash_wait_for_exposure(struct flash_meter *meter, const struct isp *isp, const struct annibale_camera_frame *f)
{
	/* Two queued frames can predate the lamp. Wait for the first metered
	 * correction, even if AE has already requested another for the next frame. */
	const unsigned warmup = 2;
	if (++meter->frames <= warmup)
		return true;
	if (meter->frames == warmup + 1 && isp_ae_pending(isp, f)) {
		meter->lines = isp->asked_lines;
		meter->gain = isp->asked_gain;
	}
	return meter->lines != 0 && meter->frames <= warmup + ISP_AE_READBACK_FRAMES &&
	    (f->exposure_lines != meter->lines || f->gain != meter->gain);
}

static void *
worker(void *context)
{
	CMBaseObjectRef o = context;
	struct stream *s = stream_of(o);
	struct annibale_camera_profile profile;
	struct annibale_camera_capture capture;
	struct isp isp;
	struct flash_meter flash = {0};
	struct isp_stats st;
	uint8_t *buffer = NULL;
	size_t bytes = 0;
	uint32_t tuned_for = ~0u;
	float lux = -1, cct = -1;
	struct af af = { 0 };
	struct focus_cmdid_echo cmd_echo = { 0 };
	int device = camera_native_open(s->camera->sensor, &profile);

	pthread_setname_np("annibale-camera");
	if (device < 0) {
		say("no %s (%s): no frames", ANNIBALE_CAMERA_DEVICE, strerror(errno));
		s->state = CAM_STREAM_FAULTED;
		return NULL;
	}
	bytes = sizeof(struct annibale_camera_frame) + (size_t)profile.stride * profile.height;
	buffer = malloc(bytes);
	if (buffer == NULL) {
		close(device);
		s->state = CAM_STREAM_FAULTED;
		return NULL;
	}
	unsigned consecutive_failures = 0;
	/* the pace over the last 300 frames: how long between frames at most, how long each took here */
	mach_timebase_info_data_t tb;
	uint64_t got = 0, pace_from = 0, longest = 0, work = 0, rendering = 0;
	uint32_t pace_frames = 0, skipped = 0, last_sequence = 0;
	/* the shutter's window: a photograph asked for arms the frames' spacing one line a frame */
	unsigned pace_armed = 0, wanted = 0;

	mach_timebase_info(&tb);
	isp_init(&isp);
	/*
	 * The rigging's orientation switches, once a stream start (camera_device_orient.inc): the front
	 * camera's frames turned half round in the pixel domain as they used to be, against the
	 * sensor-native delivery that leaves righting them to the pipeline.
	 */
	isp.turned = s->camera->sensor == 2 && rigging_switch_on("ANNIBALE_FRONT_TURNED", "/private/var/tmp/annibale_front_turned");
	if (isp.turned) {
		say("stream: the front camera's frames turned half round here (the rigging's): the pipeline's correction comes on top");
	}
	s->exif_mode = orient_exif_mode();
	if (s->exif_mode > 0) {
		say("stream: the photographs' ExifOrientation fixed at %d (the rigging's)", s->exif_mode);
	} else if (s->exif_mode < 0) {
		say("stream: the photographs' ExifOrientation from the device orientation (the rigging's)");
	}
	while (s->state == CAM_STREAM_ACTIVE) {
		uint64_t asked = mach_absolute_time();

		if (camera_native_frame(device, &profile, buffer, bytes, &capture) < 0) {
			if (capture.state == ANNIBALE_CAM_CAPTURE_FAILED && ++consecutive_failures <= 3) {
				say("transient capture error (%u/3): state %u flags %#x error %u (%s)",
				    consecutive_failures, capture.state, capture.flags, capture.error, strerror(errno));
				continue;
			}
			say("capture failed: state %u flags %#x error %u (%s)", capture.state, capture.flags,
			    capture.error, strerror(errno));
			break;
		}
		consecutive_failures = 0;

		const struct annibale_camera_frame *f = (const void *)buffer;
		uint64_t now = mach_absolute_time();

		if (got != 0 && now - got > longest) {
			longest = now - got;
		}
		if (got != 0 && (now - got) * tb.numer / tb.denom > 200000000ull) {
			/* (which side the time went to: the kernel's wait for the frame, or this loop's work) */
			say("frame %llu came %.0f ms after the one before: %.0f ms of it waiting for it", f->sequence,
			    (now - got) * (double)tb.numer / tb.denom / 1e6, (now - asked) * (double)tb.numer / tb.denom / 1e6);
		}
		if (pace_armed != 0 && got != 0) {
			/* (the spacing exactly and only around a shutter: the 300 frames' longest cannot say where it was) */
			pace_armed--;
			say("frame %llu came %.1f ms after the one before: %.1f ms of it waiting for it, %.1f ms here",
			    f->sequence, (now - got) * (double)tb.numer / tb.denom / 1e6,
			    (now - asked) * (double)tb.numer / tb.denom / 1e6, (asked - got) * (double)tb.numer / tb.denom / 1e6);
		}
		if (last_sequence != 0 && f->sequence > last_sequence + 1) {
			skipped += f->sequence - last_sequence - 1;
		}
		last_sequence = (uint32_t)f->sequence;
		got = now;
		if (pace_from == 0) {
			pace_from = now;
		}
		const uint8_t *raw = buffer + f->header_bytes;
		struct annibale_camera_command c;

		uint64_t mark[5];

		isp_measure(&isp, f, raw, &st);
		isp_awb(&isp, &st);
		mark[0] = mach_absolute_time();
		if (f->sensor != tuned_for) {
			tuned_for = f->sensor;
			load_tuning(&isp, f->sensor);
		}
		/*
		 * Slowly, a second or so: the colour temperature is guessed from the white balance, the tuning it
		 * picks changes what the white balance sees, and the colour matrix makes every swing of it plain.
		 */
		float measured_lux = lux_index(&isp, f);
		if (isfinite(measured_lux)) {
			lux = lux < 0 ? measured_lux : lux * 0.9f + measured_lux * 0.1f;
		}
		cct = cct < 0 ? colour_temperature(&isp) : cct * 0.97f + colour_temperature(&isp) * 0.03f;
		if (lux >= 0) {
			isp_tune(&isp, lux, cct);
		}
		isp.zoom = s->zoom;
		pthread_mutex_lock(&s->lock);
		isp.bias = s->exposure_bias / 256.0f;
		pthread_mutex_unlock(&s->lock);
		mark[1] = mach_absolute_time();
		if (isp_ae(&isp, f, &st, &c)) {
			(void)write(device, &c, sizeof(c));
		}
		mark[2] = mach_absolute_time();
		bool af_now, af_locked;
		float af_win[4];
		pthread_mutex_lock(&s->lock);
		if (af.state != AF_SEARCH && af.state != AF_SETTLE) {
			af.search = s->af_search;
		}
		s->last_lux = lux;
		s->last_cct = cct;
		af_now = s->af_now;
		s->af_now = false;
		af_locked = s->af_locked;
		memcpy(af_win, s->af_window, sizeof(af_win));
		pthread_mutex_unlock(&s->lock);
		if (af_now) {
			/* (whatever it was doing: at once, from the next frame) */
			af.state = AF_STEADY;
			af.now = true;
		}
		af_frame(device, &af, f, raw, af_win, af_locked);
		mark[3] = mach_absolute_time();
		s->frames++;

		int64_t focus_cmdid;
		pthread_mutex_lock(&s->lock);
		focus_cmdid = s->focus_cmdid;
		s->focus_cmdid = 0;             /* (taken: the echo keeps it until its search is done) */
		pthread_mutex_unlock(&s->lock);
		bool af_searching = af.state == AF_SEARCH || af.state == AF_SETTLE;
		int af_status = f->focus == ANNIBALE_CAMERA_NO_FOCUS ? -1 : (af_searching ? 2 : 4);
		/* parked and the scene still what it was focused on: the frame is a stable focus result */
		bool af_stable = af.state == AF_WATCH && af.unlike == 0;
		focus_cmdid = focus_cmdid_echo_frame(&cmd_echo, focus_cmdid, af_searching);
		CFDictionaryRef metadata = frame_metadata(&isp, f, s->port, af_status, af_locked, af_stable, af.at, focus_cmdid);

		/*
		 * Only once the graph has started the stream. Its PixelBufferHandler blocks hand the buffer
		 * to multiStreamCameraSourceNode_sendBufferToBufferServicingThread, which reaches into the
		 * source node's innards: called before the graph is started (or after it has been torn down)
		 * it walks a pointer that is not there yet and mediaserverd dies of SIGSEGV. The frames
		 * before that are read and thrown away, which is also what settles the exposure.
		 */
		if (metadata != NULL) {
			pthread_mutex_lock(&s->metadata_lock);
			if (s->metadata != NULL) {
				CFRelease(s->metadata);
			}
			s->metadata = CFRetain(metadata);
			pthread_mutex_unlock(&s->metadata_lock);
		}
		pthread_mutex_lock(&s->lock);
		/* a rise is a shutter just asked for, read on the way in: a lone photograph is asked for and given within one pass */
		if (s->stills_wanted > wanted) {
			pace_armed = 15;        /* (~500 ms at 30 frames a second: both photographs' renders are inside) */
		}
		wanted = s->stills_wanted;
		bool hold_still = false;
		if (s->stills_wanted > 0 && !s->still_redeye_wanted && s->still_flash_wanted && s->torch >= 0) {
			if (!s->flash_lit) {
				uint32_t full = 1000;

				if (write(s->torch, &full, sizeof(full)) == (ssize_t)sizeof(full)) {
					s->flash_lit = true;
					flash = (struct flash_meter){0};
					hold_still = true;
					say("still flash lit (1000/1000) for photograph warmup");
				} else {
					s->still_flash_wanted = false;
					say("still flash write failed; delivering photograph without flash");
				}
			} else {
				hold_still = flash_wait_for_exposure(&flash, &isp, f);
			}
		}
		bool gave_still = false;
		uint64_t render_from = mach_absolute_time();

		for (int i = 0; s->state == CAM_STREAM_ACTIVE && i < OUTPUTS; i++) {
			struct output *out = &s->out[i];

			if (strcmp(out->name, "Depth") == 0) {
				continue;
			}
			/*
			 * The scalers' outputs are the moving picture and are given every frame. The still image
			 * outputs are the photograph: a frame goes to them only when the graph has asked for one,
			 * which is not something to make thirty times a second.
			 */
			bool still = strncmp(out->name, "StillImage", 10) == 0;

			if (out->handler == NULL || out->config == NULL || !(out->enabled || i == 0)) {
				continue;
			}
			if (still && (s->stills_wanted == 0 || hold_still)) {
				continue;
			}
			if (still) {
				still_enqueue(s, out, &isp, buffer, (uint32_t)bytes, metadata);
				gave_still = true;
			} else {
				deliver(s, out, &isp, f, raw, metadata);
			}
		}
		mark[4] = mach_absolute_time();
		rendering += mark[4] - render_from;
		if (gave_still && s->stills_wanted > 0) {
			s->stills_wanted--;
			s->still_redeye_wanted = false;
			if (s->stills_wanted == 0) {
				if (s->flash_lit && s->torch >= 0) {
					uint32_t dark = 0;

					(void)write(s->torch, &dark, sizeof(dark));
					say("still flash extinguished (0/1000) after photograph");
				}
				s->flash_lit = false;
				s->still_flash_wanted = false;
			}
		}
		pthread_mutex_unlock(&s->lock);
		if (metadata != NULL) {
			CFRelease(metadata);
		}
		work += mach_absolute_time() - got;
		if ((mach_absolute_time() - got) * tb.numer / tb.denom > 150000000ull) {
			double ms = (double)tb.numer / tb.denom / 1e6;

			say("frame %llu took %.0f ms here: measuring %.0f, tuning %.0f, AE %.0f, focus %.0f, giving pictures %.0f",
			    f->sequence, (mach_absolute_time() - got) * ms, (mark[0] - got) * ms, (mark[1] - mark[0]) * ms,
			    (mark[2] - mark[1]) * ms, (mark[3] - mark[2]) * ms, (mark[4] - render_from) * ms);
		}
		if (++pace_frames == 300) {
			double ms = (double)tb.numer / tb.denom / 1e6;

			say("pace: %u frames in %.1f s, %u of the sensor's skipped; at most %.0f ms between two; %.1f ms each here (%.1f giving pictures)",
			    pace_frames, (got - pace_from) * ms / 1000, skipped, longest * ms, work * ms / pace_frames,
			    rendering * ms / pace_frames);
			pace_frames = skipped = 0;
			pace_from = got;
			longest = work = rendering = 0;
		}
		if (s->frames == 1 || s->frames % 300 == 0) {
			say("frame %llu: %ux%u RAW10, input mean R %.0f G %.0f B %.0f above black; white balance R %.2f B %.2f",
			    f->sequence, f->width, f->height, st.mean[0], st.mean[1], st.mean[2], isp.wb[0], isp.wb[2]);
		}
	}
	/*
	 * The stills' queue drains before the worker returns: a job's out, handler and stream are this
	 * stream's to keep or release, and the caller's join is what lets stop/finalize go on. A job
	 * not yet begun sees the stream's state and lets its frame go at once; one running finishes.
	 */
	if (s->still_queue != NULL) {
		dispatch_sync(s->still_queue, ^{});
	}
	free(buffer);
	isp_destroy(&isp);
	close(device);
	pthread_mutex_lock(&s->lock);
	if (s->flash_lit && s->torch >= 0) {
		uint32_t dark = 0;

		(void)write(s->torch, &dark, sizeof(dark));
	}
	s->flash_lit = false;
	s->still_flash_wanted = false;
	s->still_redeye_wanted = false;
	pthread_mutex_unlock(&s->lock);
	if (s->state == CAM_STREAM_ACTIVE) {
		s->state = CAM_STREAM_FAULTED;
	}
	say("the frames stopped (%llu)", s->frames);
	return NULL;
}

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
	s->still_queue = dispatch_queue_create("annibale-camera-still", NULL);
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
