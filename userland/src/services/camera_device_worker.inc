/* Exposure/AF/tuning helpers and capture worker loop, included into camera_device_stream.inc. */

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
