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

/* AFWindowParams: the first window, as fractions of the frame; none (or not one) is the middle */
static void
af_window(struct stream *s, CFTypeRef value)
{
	CFDictionaryRef w = NULL;
	double x0, y0, width, height;

	if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() && CFArrayGetCount(value) > 0) {
		w = CFArrayGetValueAtIndex(value, 0);
	}
	if (w == NULL || CFGetTypeID(w) != CFDictionaryGetTypeID() || (width = dict_number(w, "width", 0)) <= 0 ||
	    (height = dict_number(w, "height", 0)) <= 0) {
		s->af_window[0] = s->af_window[1] = 0.3f;
		s->af_window[2] = s->af_window[3] = 0.7f;
		return;
	}
	x0 = dict_number(w, "hStart", 0) / 1000;
	y0 = dict_number(w, "vStart", 0) / 1000;
	width /= 1000;
	height /= 1000;
	if (width < 0.1) {
		x0 -= (0.1 - width) / 2;
		width = 0.1;
	}
	if (height < 0.1) {
		y0 -= (0.1 - height) / 2;
		height = 0.1;
	}
	x0 = x0 < 0 ? 0 : x0 + width > 1 ? 1 - width : x0;
	y0 = y0 < 0 ? 0 : y0 + height > 1 ? 1 - height : y0;
	s->af_window[0] = (float)x0;
	s->af_window[1] = (float)y0;
	s->af_window[2] = (float)(x0 + width);
	s->af_window[3] = (float)(y0 + height);
}
