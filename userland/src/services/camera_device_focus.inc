/*
 * The focus settings CMCapture gives the stream (camera_device.c includes this after struct stream):
 * the search's positions and early stop (AFSearchPositions, AFEarlyOutParams, AFPeakPredictionEnabled)
 * and where to focus (AFWindowParams). The search itself is af_frame() in camera_device_stream.inc.
 */

static int32_t
dict_i32(CFDictionaryRef d, const char *name, int32_t otherwise)
{
	CFStringRef k = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CFTypeRef v = CFDictionaryGetValue(d, k);
	int32_t n;

	CFRelease(k);
	return v != NULL && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberSInt32Type, &n) ? n : otherwise;
}

static void
af_search_set(struct stream *s, CFStringRef key, CFTypeRef value)
{
	say_value("stream: focus:", key, value);
	pthread_mutex_lock(&s->lock);
	struct af_search *a = &s->af_search;

	if (key_is(key, "AFSearchPositions") && value != NULL && CFGetTypeID(value) == CFArrayGetTypeID()) {
		CFIndex n = CFArrayGetCount(value);
		struct af_search got = *a;

		got.count = 0;
		for (CFIndex i = 0; i < n && got.count < AF_POSITIONS; i++) {
			CFTypeRef v = CFArrayGetValueAtIndex(value, i);
			int32_t pos;

			if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberSInt32Type, &pos) && pos >= 0 &&
			    pos <= 255 && (got.count == 0 || pos > got.position[got.count - 1])) {
				got.position[got.count++] = (uint8_t)pos;
			}
		}
		if (got.count >= 2) {
			*a = got;
		}
	} else if (key_is(key, "AFEarlyOutParams") && value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
		a->early_out = dict_i32(value, "AFEarlyOutEnable", a->early_out) != 0;
		a->early_out_count = (unsigned)dict_i32(value, "AFEarlyOutThreshold", (int32_t)a->early_out_count);
		a->early_out_drop = dict_i32(value, "AFEarlyOutDecrementAmount", (int32_t)(a->early_out_drop * 256)) / 256.0f;
		a->early_out_at_start = dict_i32(value, "AFEarlyOutAllowPeakAtStart", a->early_out_at_start) != 0;
	} else if (key_is(key, "AFPeakPredictionEnabled") && value != NULL && CFGetTypeID(value) == CFBooleanGetTypeID()) {
		a->peak_prediction = CFBooleanGetValue(value);
	}
	pthread_mutex_unlock(&s->lock);
}

/*
 * AFWindowParams: the windows, in thousandths of the frame. With AutoFocusModeMatrix CMCapture sends
 * a grid of them (15 cells of 105 x 147 over 237..762 x 280..721 on 23G90) and a tap sends one; the
 * sharpness is measured over the box around every window with a weight. None is the middle.
 */
static void
af_window(struct stream *s, CFTypeRef value)
{
	double x0 = 1, y0 = 1, x1 = 0, y1 = 0;
	CFIndex count = value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() ? CFArrayGetCount(value) : 0;

	for (CFIndex i = 0; i < count; i++) {
		CFDictionaryRef w = CFArrayGetValueAtIndex(value, i);
		double width, height, h, v;

		if (w == NULL || CFGetTypeID(w) != CFDictionaryGetTypeID() || (width = dict_number(w, "width", 0)) <= 0 ||
		    (height = dict_number(w, "height", 0)) <= 0 || dict_number(w, "weight", 1) <= 0) {
			continue;
		}
		h = dict_number(w, "hStart", 0) / 1000;
		v = dict_number(w, "vStart", 0) / 1000;
		x0 = h < x0 ? h : x0;
		y0 = v < y0 ? v : y0;
		x1 = h + width / 1000 > x1 ? h + width / 1000 : x1;
		y1 = v + height / 1000 > y1 ? v + height / 1000 : y1;
	}
	if (x1 <= x0 || y1 <= y0) {
		s->af_window[0] = s->af_window[1] = 0.3f;
		s->af_window[2] = s->af_window[3] = 0.7f;
		return;
	}
	if (x1 - x0 < 0.1) {
		x0 -= (0.1 - (x1 - x0)) / 2;
		x1 = x0 + 0.1;
	}
	if (y1 - y0 < 0.1) {
		y0 -= (0.1 - (y1 - y0)) / 2;
		y1 = y0 + 0.1;
	}
	x0 = x0 < 0 ? 0 : x1 > 1 ? x0 - (x1 - 1) : x0;
	y0 = y0 < 0 ? 0 : y1 > 1 ? y0 - (y1 - 1) : y0;
	x1 = x1 > 1 ? 1 : x1;
	y1 = y1 > 1 ? 1 : y1;
	s->af_window[0] = (float)x0;
	s->af_window[1] = (float)y0;
	s->af_window[2] = (float)x1;
	s->af_window[3] = (float)y1;
}

/*
 * The FocusNow command id echoed in the frames' metadata. CMCapture latches the last frame id
 * that reads as a positive int32 into its most-recent-valid-command slot (a signed >= 1 test;
 * afstatus-semantics.md §3), so the frames carry the command's id while its search is
 * outstanding and the idle sentinel otherwise: int32 -559038737, bit pattern 0xDEADBEEF, which
 * that signed test sorts out of the valid ids. When the native pipeline switches a completed
 * command back to the sentinel is not traced (§7); here the echo ends once the search the
 * command asked for has been seen in AF_SEARCH/AF_SETTLE and left it -- the AF_STEADY frames
 * before the search starts are the command's, not its end.
 */
#define FOCUS_CMDID_IDLE    (-559038737)    /* the idle echo as an int32: bit pattern 0xDEADBEEF */

struct focus_cmdid_echo {
	int64_t         id;             /* the command being answered, 0 once its search is done */
	bool            search_seen;    /* its search has run: a pre-search frame is not its end */
};

/* `arrived` is the command newly taken from the stream's mailbox this frame, 0 when none. */
static int64_t
focus_cmdid_echo_frame(struct focus_cmdid_echo *echo, int64_t arrived, bool searching)
{
	if (arrived != 0) {
		echo->id = arrived;
		echo->search_seen = false;
	}
	if (searching) {
		echo->search_seen = true;
	} else if (echo->search_seen) {
		echo->id = 0;
		echo->search_seen = false;
	}
	return echo->id;
}
