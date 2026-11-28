/*
 * Rear wide OVX8000 Mode 24 PDAF calibration parser and phase disparity estimator.
 * Included by camera_device_worker.inc.
 *
 * EEPROM layout (GT24P128F, Big-Endian / CamX endianness=0, from com.qti.sensormodule.annibale_aac_ovx8000_wide_i.bin):
 *   0x0001       : vendorId (0x10 = AAC)
 *   0x002d       : AF macro DAC (u16)
 *   0x0031       : AF infinity DAC (u16)
 *   0x0eed..0ef2 : PD header (version u16, width u16 = 17, height u16 = 13)
 *   0x0ef3..10ac : Horizontal GainMap Left  (17x13 u16 = 442 B)
 *   0x10ad..1266 : Horizontal GainMap Right (17x13 u16 = 442 B)
 *   0x126d..12cc : Horizontal DCC (8x6 int16 Q4 = 96 B)
 *   0x1399..1552 : Vertical GainMap Top    (17x13 u16 = 442 B)
 *   0x1553..170c : Vertical GainMap Bottom (17x13 u16 = 442 B)
 *   0x1713..1772 : Vertical DCC (8x6 int16 Q4 = 96 B)
 *
 * VC=1 stream (PDConfigData[6]): 2560x480 MIPI RAW10 (stride 3200 B, 1,536,000 B/frame),
 * bufferType=4 (row-interleaved L,R pairs: 1280 pairs/row x 480 rows), black level 64,
 * mapped to full 8192x6144 sensor space at xOff=1536, yOff=1152, fullW=5120, fullH=3840.
 */

#define PDAF_PD_WIDTH             2560u
#define PDAF_PD_HEIGHT            480u
#define PDAF_PD_STRIDE            3200u
#define PDAF_MAX_BYTES            (PDAF_PD_STRIDE * PDAF_PD_HEIGHT)
#define PDAF_BLACK_LEVEL          64
#define PDAF_SEARCH_RANGE         16
#define PDAF_GAIN_W               17u
#define PDAF_GAIN_H               13u
#define PDAF_GAIN_COUNT           (PDAF_GAIN_W * PDAF_GAIN_H)
#define PDAF_DCC_W                8u
#define PDAF_DCC_H                6u
#define PDAF_DCC_COUNT            (PDAF_DCC_W * PDAF_DCC_H)
#define PDAF_EEPROM_MIN_BYTES     0x1773u
#define PDAF_EEPROM_READ_BYTES    0x1840u

#define PDAF_CONF_MIN             120u
#define PDAF_CONVERGE_DAC         12.0f
#define PDAF_DEFOCUS_TRIGGER_DAC  40.0f
#define PDAF_MAX_ITERS            3u
#define PDAF_MODE24_SCALE_X       2.0f   /* PDConfigData[6] scaleX=2.0 (2 sensor px per L/R pair) */
#define PDAF_AF_NEAR              16u
#define PDAF_AF_FAR               700u

struct pdaf_cal {
	bool     valid;
	uint16_t dac_macro;
	uint16_t dac_inf;
	uint16_t gain_l[PDAF_GAIN_COUNT];
	uint16_t gain_r[PDAF_GAIN_COUNT];
	uint16_t gain_t[PDAF_GAIN_COUNT];
	uint16_t gain_b[PDAF_GAIN_COUNT];
	float    dcc_h[PDAF_DCC_COUNT]; /* DAC per pixel disparity (Q4 -> float) */
	float    dcc_v[PDAF_DCC_COUNT];
};

struct pdaf_result {
	float    disparity;  /* subpixel L/R shift in PD pair units */
	float    dcc;        /* sampled horizontal DCC (DAC/px) */
	float    delta_dac;  /* disparity * dcc */
	uint32_t target_dac; /* clamped target lens DAC */
	uint32_t confidence; /* 0..1000 */
};

static uint16_t
pdaf_u16_be(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static bool
pdaf_parse_eeprom(struct pdaf_cal *cal, const uint8_t *buf, size_t len)
{
	memset(cal, 0, sizeof(*cal));
	if (buf == NULL || len < PDAF_EEPROM_MIN_BYTES) {
		return false;
	}
	if (buf[0x0001u] != 0x10u) {
		return false;
	}
	uint16_t gw = pdaf_u16_be(buf + 0x0eefu);
	uint16_t gh = pdaf_u16_be(buf + 0x0ef1u);
	if (gw != PDAF_GAIN_W || gh != PDAF_GAIN_H) {
		return false;
	}
	uint16_t macro = pdaf_u16_be(buf + 0x002du);
	uint16_t inf = pdaf_u16_be(buf + 0x0031u);
	if (macro > 1023u || inf > 1023u || macro == inf) {
		return false;
	}
	cal->dac_macro = macro;
	cal->dac_inf = inf;

	uint64_t sum_l = 0, sum_r = 0;
	for (uint32_t i = 0; i < PDAF_GAIN_COUNT; i++) {
		cal->gain_l[i] = pdaf_u16_be(buf + 0x0ef3u + i * 2u);
		cal->gain_r[i] = pdaf_u16_be(buf + 0x10adu + i * 2u);
		cal->gain_t[i] = pdaf_u16_be(buf + 0x1399u + i * 2u);
		cal->gain_b[i] = pdaf_u16_be(buf + 0x1553u + i * 2u);
		sum_l += cal->gain_l[i];
		sum_r += cal->gain_r[i];
	}
	if (sum_l == 0 || sum_r == 0) {
		return false;
	}
	double dcc_abs_sum = 0.0;
	for (uint32_t i = 0; i < PDAF_DCC_COUNT; i++) {
		int16_t dh = (int16_t)pdaf_u16_be(buf + 0x126du + i * 2u);
		int16_t dv = (int16_t)pdaf_u16_be(buf + 0x1713u + i * 2u);
		cal->dcc_h[i] = (float)dh / 16.0f;
		cal->dcc_v[i] = (float)dv / 16.0f;
		dcc_abs_sum += fabs((double)cal->dcc_h[i]);
	}
	if (dcc_abs_sum < 1.0) {
		return false;
	}
	cal->valid = true;
	return true;
}

static void
pdaf_load_eeprom(int device, struct pdaf_cal *cal)
{
	uint8_t raw[PDAF_EEPROM_READ_BYTES];
	size_t got = camera_native_read_eeprom(device, raw, sizeof(raw));
	if (got == 0) {
		memset(cal, 0, sizeof(*cal));
		return;
	}
	if (pdaf_parse_eeprom(cal, raw, got)) {
		uint32_t g_mid = (PDAF_GAIN_H / 2u) * PDAF_GAIN_W + (PDAF_GAIN_W / 2u);
		say("pdaf: eeprom loaded (%zu B, inf=%u macro=%u, gain_mid=%u/%u, dcc_center=%.2f)",
		    got, cal->dac_inf, cal->dac_macro, cal->gain_l[g_mid], cal->gain_r[g_mid],
		    cal->dcc_h[3u * PDAF_DCC_W + 4u]);
	} else {
		say("pdaf: eeprom invalid (%zu B, vendor=%#02x)", got, raw[0x0001u]);
	}
}

static float
pdaf_bilerp_u16(const uint16_t *grid, uint32_t gw, uint32_t gh, float u, float v)
{
	float fx = u * (float)(gw - 1u);
	float fy = v * (float)(gh - 1u);
	if (fx < 0.0f) fx = 0.0f;
	if (fy < 0.0f) fy = 0.0f;
	if (fx > (float)(gw - 1u)) fx = (float)(gw - 1u);
	if (fy > (float)(gh - 1u)) fy = (float)(gh - 1u);
	uint32_t x0 = (uint32_t)fx, y0 = (uint32_t)fy;
	uint32_t x1 = x0 + 1u < gw ? x0 + 1u : x0;
	uint32_t y1 = y0 + 1u < gh ? y0 + 1u : y0;
	float tx = fx - (float)x0, ty = fy - (float)y0;
	float a = (float)grid[y0 * gw + x0] * (1.0f - tx) + (float)grid[y0 * gw + x1] * tx;
	float b = (float)grid[y1 * gw + x0] * (1.0f - tx) + (float)grid[y1 * gw + x1] * tx;
	return a * (1.0f - ty) + b * ty;
}

static float
pdaf_bilerp_f32(const float *grid, uint32_t gw, uint32_t gh, float u, float v)
{
	float fx = u * (float)(gw - 1u);
	float fy = v * (float)(gh - 1u);
	if (fx < 0.0f) fx = 0.0f;
	if (fy < 0.0f) fy = 0.0f;
	if (fx > (float)(gw - 1u)) fx = (float)(gw - 1u);
	if (fy > (float)(gh - 1u)) fy = (float)(gh - 1u);
	uint32_t x0 = (uint32_t)fx, y0 = (uint32_t)fy;
	uint32_t x1 = x0 + 1u < gw ? x0 + 1u : x0;
	uint32_t y1 = y0 + 1u < gh ? y0 + 1u : y0;
	float tx = fx - (float)x0, ty = fy - (float)y0;
	float a = grid[y0 * gw + x0] * (1.0f - tx) + grid[y0 * gw + x1] * tx;
	float b = grid[y1 * gw + x0] * (1.0f - tx) + grid[y1 * gw + x1] * tx;
	return a * (1.0f - ty) + b * ty;
}

/* Map normalized Mode 24 ROI coordinate to full 8192x6144 sensor normalized coordinate. */
static float
pdaf_mode24_u(float norm_x)
{
	return (1536.0f + norm_x * 5120.0f) / 8192.0f;
}

static float
pdaf_mode24_v(float norm_y)
{
	return (1152.0f + norm_y * 3840.0f) / 6144.0f;
}

/*
 * Unpack one row of MIPI RAW10 L/R pairs [x_start..x_end) (in pair indices, 2 pixels/pair),
 * subtract black level 64, and apply GainMap shading equalization.
 */
static void
pdaf_unpack_row(const uint8_t *row, uint32_t pairs_w, uint32_t x0, uint32_t x1,
    float gain_l_scale, float gain_r_scale, float *left_out, float *right_out)
{
	uint32_t q0 = x0 & ~1u;
	uint32_t q1 = (x1 + 1u) & ~1u;
	if (q1 > pairs_w) {
		q1 = pairs_w;
	}
	for (uint32_t p = q0; p < q1; p += 2u) {
		/* 2 L/R pairs = 4 pixels = 5 bytes in MIPI RAW10 */
		const uint8_t *g = row + (p / 2u) * 5u;
		uint8_t lsb = g[4];
		int v0 = ((int)g[0] << 2) | (lsb & 3);
		int v1 = ((int)g[1] << 2) | ((lsb >> 2) & 3);
		int v2 = ((int)g[2] << 2) | ((lsb >> 4) & 3);
		int v3 = ((int)g[3] << 2) | ((lsb >> 6) & 3);
		if (p >= x0 && p < x1) {
			left_out[p - x0] = (float)(v0 > PDAF_BLACK_LEVEL ? v0 - PDAF_BLACK_LEVEL : 0) * gain_l_scale;
			right_out[p - x0] = (float)(v1 > PDAF_BLACK_LEVEL ? v1 - PDAF_BLACK_LEVEL : 0) * gain_r_scale;
		}
		if (p + 1u >= x0 && p + 1u < x1) {
			left_out[p + 1u - x0] = (float)(v2 > PDAF_BLACK_LEVEL ? v2 - PDAF_BLACK_LEVEL : 0) * gain_l_scale;
			right_out[p + 1u - x0] = (float)(v3 > PDAF_BLACK_LEVEL ? v3 - PDAF_BLACK_LEVEL : 0) * gain_r_scale;
		}
	}
}

static bool
pdaf_measure(const struct pdaf_cal *cal, const struct annibale_camera_frame *f, const uint8_t *pd_raw,
    const float window[4], uint32_t current_dac, struct pdaf_result *out)
{
	memset(out, 0, sizeof(*out));
	if (cal == NULL || !cal->valid || f == NULL || pd_raw == NULL ||
	    (f->pd_flags & ANNIBALE_CAM_PD_VALID) == 0 ||
	    f->pd_width < 64u || f->pd_height < 16u ||
	    f->pd_stride < (f->pd_width * 5u) / 4u ||
	    f->pd_bytes < f->pd_stride * f->pd_height) {
		return false;
	}
	uint32_t pairs_w = f->pd_width / 2u;
	uint32_t pd_h = f->pd_height;
	if (pairs_w <= 2u * PDAF_SEARCH_RANGE + 8u) {
		return false;
	}
	float wx0 = window[0] < 0.0f ? 0.0f : window[0] > 0.9f ? 0.9f : window[0];
	float wy0 = window[1] < 0.0f ? 0.0f : window[1] > 0.9f ? 0.9f : window[1];
	float wx1 = window[2] > 1.0f ? 1.0f : window[2] < wx0 + 0.1f ? wx0 + 0.1f : window[2];
	float wy1 = window[3] > 1.0f ? 1.0f : window[3] < wy0 + 0.1f ? wy0 + 0.1f : window[3];
	if (wx1 > 1.0f) wx1 = 1.0f;
	if (wy1 > 1.0f) wy1 = 1.0f;

	uint32_t x0 = (uint32_t)(wx0 * (float)pairs_w);
	uint32_t x1 = (uint32_t)(wx1 * (float)pairs_w);
	uint32_t y0 = (uint32_t)(wy0 * (float)pd_h);
	uint32_t y1 = (uint32_t)(wy1 * (float)pd_h);
	if (x0 < (uint32_t)PDAF_SEARCH_RANGE) x0 = (uint32_t)PDAF_SEARCH_RANGE;
	if (x1 > pairs_w - (uint32_t)PDAF_SEARCH_RANGE) x1 = pairs_w - (uint32_t)PDAF_SEARCH_RANGE;
	if (x1 <= x0 + 16u || y1 <= y0 + 4u) {
		return false;
	}

	uint32_t read_x0 = x0 - (uint32_t)PDAF_SEARCH_RANGE;
	uint32_t read_x1 = x1 + (uint32_t)PDAF_SEARCH_RANGE;
	uint32_t span = read_x1 - read_x0;
	if (span > 1280u) {
		return false;
	}
	float left_row[1280];
	float right_row[1280];
	double sad[2 * PDAF_SEARCH_RANGE + 1];
	memset(sad, 0, sizeof(sad));

	float cx = 0.5f * (wx0 + wx1), cy = 0.5f * (wy0 + wy1);
	float u_center = pdaf_mode24_u(cx), v_center = pdaf_mode24_v(cy);
	float gl = pdaf_bilerp_u16(cal->gain_l, PDAF_GAIN_W, PDAF_GAIN_H, u_center, v_center);
	float gr = pdaf_bilerp_u16(cal->gain_r, PDAF_GAIN_W, PDAF_GAIN_H, u_center, v_center);
	float g_mean = 0.5f * (gl + gr);
	float gain_l_scale = (g_mean > 1.0f && gl > 1.0f && gr > 1.0f) ? (gl / g_mean) : 1.0f;
	float gain_r_scale = (g_mean > 1.0f && gl > 1.0f && gr > 1.0f) ? (gr / g_mean) : 1.0f;

	double signal_sum = 0.0;
	uint64_t samples = 0;
	uint32_t width = x1 - x0;
	for (uint32_t y = y0; y < y1; y += 2u) {
		pdaf_unpack_row(pd_raw + (size_t)y * f->pd_stride, pairs_w, read_x0, read_x1,
		    gain_l_scale, gain_r_scale, left_row, right_row);
		for (uint32_t i = 0; i < width; i++) {
			float lv = left_row[i + (uint32_t)PDAF_SEARCH_RANGE];
			signal_sum += lv;
		}
		samples += width;
		for (int d = -PDAF_SEARCH_RANGE; d <= PDAF_SEARCH_RANGE; d++) {
			double acc = 0.0;
			int r_base = PDAF_SEARCH_RANGE + d;
			for (uint32_t i = 0; i < width; i++) {
				acc += fabsf(left_row[i + (uint32_t)PDAF_SEARCH_RANGE] - right_row[i + (uint32_t)r_base]);
			}
			sad[d + PDAF_SEARCH_RANGE] += acc;
		}
	}
	if (samples == 0 || signal_sum / (double)samples < 8.0) {
		return false;
	}

	int best_idx = 0;
	double min_sad = sad[0], sum_sad = 0.0;
	const int n_shifts = 2 * PDAF_SEARCH_RANGE + 1;
	for (int k = 0; k < n_shifts; k++) {
		sum_sad += sad[k];
		if (sad[k] < min_sad) {
			min_sad = sad[k];
			best_idx = k;
		}
	}
	double mean_sad = sum_sad / (double)n_shifts;
	if (mean_sad <= 1e-6) {
		return false;
	}
	double ratio = (mean_sad - min_sad) / mean_sad;
	if (ratio < 0.0) ratio = 0.0;
	uint32_t conf = (uint32_t)(ratio * 1000.0 + 0.5);
	if (conf > 1000u) conf = 1000u;

	float sub = 0.0f;
	if (best_idx > 0 && best_idx < n_shifts - 1) {
		double s_prev = sad[best_idx - 1], s_mid = sad[best_idx], s_next = sad[best_idx + 1];
		double denom = s_prev - 2.0 * s_mid + s_next;
		if (denom > 1e-6) {
			sub = (float)(0.5 * (s_prev - s_next) / denom);
			if (sub < -0.5f) sub = -0.5f;
			if (sub > 0.5f) sub = 0.5f;
		}
	}
	float disparity = (float)(best_idx - PDAF_SEARCH_RANGE) + sub;
	float dcc = pdaf_bilerp_f32(cal->dcc_h, PDAF_DCC_W, PDAF_DCC_H, u_center, v_center);
	/*
	 * Sign and scale verified on b5r68 silicon: at DAC 275 (true focus ~464), sad(L[x]-R[x+d])
	 * yields disp=-2.31 PD pairs with dcc=40.39 and PDConfigData[6] scaleX=2.0 -> +186.6 DAC (462).
	 */
	float delta = -disparity * dcc * PDAF_MODE24_SCALE_X;
	float target_f = (float)current_dac + delta;
	uint32_t target = target_f < (float)PDAF_AF_NEAR ? PDAF_AF_NEAR :
	    target_f > (float)PDAF_AF_FAR ? PDAF_AF_FAR : (uint32_t)(target_f + 0.5f);

	out->disparity = disparity;
	out->dcc = dcc;
	out->delta_dac = delta;
	out->target_dac = target;
	out->confidence = conf;
	return conf >= PDAF_CONF_MIN;
}
