/*
 * The cameras' frames made into pictures by the processor (see camera_isp.h): one pixel per 2x2
 * cell, the nearest cell for each pixel of the picture (previews), or every pixel by bilinear
 * demosaic (stills); per pixel, in this order: the black level off, lens shading (a grid of gains per
 * colour, when there is one), the white balance's and AE's gains, the colour matrix (identity until
 * one is set), AE's tone curve, sRGB's curve. Grey-world white balance; no noise reduction yet. The
 * colour matrix and the lens shading can come from the camera's own tuning (isp_load_tuning(),
 * isp_tune()).
 */
#include <arm_neon.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "camera_isp.h"

#define WHITE           1023
#define LUT_WHITE       (WHITE * 4)

/*
 * The gamma table for a tone curve of gain drc at black: drc * v / (1 + (drc - 1) * v) on the linear
 * value (white staying white, what is near it compressed), then sRGB's curve.
 */
static void
tone(struct isp *isp, float drc)
{
	for (int i = 0; i < 4096; i++) {
		double v = i >= LUT_WHITE ? 1 : (double)i / LUT_WHITE;

		v = drc * v / (1 + (drc - 1) * v);
		v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
		isp->gamma[i] = (uint8_t)(v * 255 + 0.5);
	}
	isp->drc = drc;
}

void
isp_init(struct isp *isp)
{
	memset(isp, 0, sizeof(*isp));
	isp->wb[0] = isp->wb[1] = isp->wb[2] = 1;
	isp->digital = 1;
	isp->bias = 1;
	isp->zoom = 1;
	for (int i = 0; i < 3; i++) {
		isp->ccm[i][i] = 1;
	}
	tone(isp, 1);
}

/* where red and blue are in a cell (0 top left, 1 top right, 2 bottom left, 3 bottom right) */
static void
cell_places(uint32_t bayer, int *r, int *b)
{
	static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };

	*r = red[bayer & 3];
	*b = blue[bayer & 3];
}

/* the four values of the cell at cx of the pair of lines from `top' (above black, 0 and up) */
static inline void
cell(const uint8_t *top, size_t stride, uint32_t cx, uint32_t black, int v[4])
{
	const uint8_t *g0 = top + (size_t)(cx >> 1) * 5, *g1 = g0 + stride;
	unsigned o = (cx & 1) * 2;

	v[0] = (g0[o] << 2 | (g0[4] >> (o * 2) & 3)) - (int)black;
	v[1] = (g0[o + 1] << 2 | (g0[4] >> (o * 2 + 2) & 3)) - (int)black;
	v[2] = (g1[o] << 2 | (g1[4] >> (o * 2) & 3)) - (int)black;
	v[3] = (g1[o + 1] << 2 | (g1[4] >> (o * 2 + 2) & 3)) - (int)black;
	for (int i = 0; i < 4; i++) {
		v[i] = v[i] < 0 ? 0 : v[i];
	}
}

float
isp_sharpness(const struct annibale_camera_frame *f, const uint8_t *raw, float x0, float y0, float x1, float y1, float *level)
{
	enum { BLOCK = 4, MAX_BLOCKS = 512 };
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	uint32_t cx0 = (uint32_t)(x0 * cells_w), cx1 = (uint32_t)(x1 * cells_w), cy0 = (uint32_t)(y0 * cells_h),
	    cy1 = (uint32_t)(y1 * cells_h);
	float above[MAX_BLOCKS], here[MAX_BLOCKS];
	double steps = 0, sum = 0;
	uint32_t blocks_w, rows = 0, n = 0;
	int r, b;

	cell_places(f->bayer, &r, &b);
	cx1 = cx1 > cells_w ? cells_w : cx1;
	cy1 = cy1 > cells_h ? cells_h : cy1;
	blocks_w = cx1 > cx0 ? (cx1 - cx0) / BLOCK : 0;
	blocks_w = blocks_w > MAX_BLOCKS ? MAX_BLOCKS : blocks_w;
	/*
	 * Green's mean over blocks of 4x4 cells, and the squares of the steps between neighbouring
	 * blocks: a cell's own noise is a quarter of itself in a block's mean, and the picture's edges are
	 * not -- in the dark, at 32x and more, the steps from cell to cell were the noise's alone and the
	 * focus went wherever it liked.
	 */
	for (uint32_t by = cy0; by + BLOCK <= cy1; by += BLOCK, rows++) {
		for (uint32_t bx = 0; bx < blocks_w; bx++) {
			uint32_t g = 0;

			for (uint32_t cy = by; cy < by + BLOCK; cy++) {
				const uint8_t *top = raw + (size_t)cy * 2 * f->stride;

				for (uint32_t cx = cx0 + bx * BLOCK; cx < cx0 + bx * BLOCK + BLOCK; cx++) {
					int v[4];

					cell(top, f->stride, cx, f->black, v);
					for (int i = 0; i < 4; i++) {
						g += i != r && i != b ? (uint32_t)v[i] : 0;
					}
				}
			}
			here[bx] = g / (2.0f * BLOCK * BLOCK);
			sum += here[bx];
			n++;
			if (bx > 0) {
				steps += (here[bx] - here[bx - 1]) * (here[bx] - here[bx - 1]);
			}
			if (rows > 0) {
				steps += (here[bx] - above[bx]) * (here[bx] - above[bx]);
			}
		}
		memcpy(above, here, sizeof(float) * blocks_w);
	}
	double mean = n ? sum / n : 0;

	if (level != NULL) {
		*level = (float)mean;
	}
	return mean > 0 ? (float)(steps / n / (mean * mean)) : 0;
}

static inline void lsc_at(const struct isp *isp, float u, float v, float g[3]);

void
isp_measure(const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw, struct isp_stats *s)
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	double sum[3] = { 0, 0, 0 }, grey[2] = { 0, 0 }, neutral[2] = { 0, 0 }, logs = 0;
	float gains[3] = { 1, 1, 1 };
	int r, b;

	memset(s, 0, sizeof(*s));
	cell_places(f->bayer, &r, &b);
	for (uint32_t cy = 0; cy < cells_h; cy += 4) {
		const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
		float v_pos = cells_h > 1 ? (float)cy / (cells_h - 1) : 0;

		for (uint32_t cx = 0; cx < cells_w; cx += 4) {
			int v[4], g = 0, top_v = 0;

			cell(top, f->stride, cx, f->black, v);
			for (int i = 0; i < 4; i++) {
				g += i != r && i != b ? v[i] : 0;
				top_v = v[i] > top_v ? v[i] : top_v;
			}
			g /= 2;
			sum[0] += v[r];
			sum[1] += g;
			sum[2] += v[b];
			/*
			 * As the picture will have it: the lens shading raises the corners several times, and
			 * the light there counts for that much more than in the frame as it comes.
			 */
			float cr = (float)v[r], cg = (float)g, cb = (float)v[b];

			if (isp != NULL && isp->lsc.on) {
				lsc_at(isp, cells_w > 1 ? (float)cx / (cells_w - 1) : 0, v_pos, gains);
				cr *= gains[0];
				cg *= gains[1];
				cb *= gains[2];
			}
			/* (a cell near black counts as the near black below: there it is noise more than light) */
			logs += logf(cg > 24 ? cg : 24);
			/* the brightest colour, white balanced: a bluish screen reaches white in blue first */
			float top = isp != NULL ? fmaxf(cg, fmaxf(cr * isp->wb[0], cb * isp->wb[2])) : fmaxf(cg, fmaxf(cr, cb));
			/*
			 * The white balance leaves out a cell with a colour near white -- a lamp, a screen:
			 * green, the most sensitive, stops there first and the cell would count as magenta --
			 * and one near black, where noise cut off at the black level is all there is.
			 */
			if (top_v < (int)(WHITE - f->black) * 9 / 10 && g >= 24) {
				/*
				 * Each cell's colour, not its light: summed, a lamp or a screen in the picture -- tens of
				 * times brighter than the room it is in -- would have the white balance to itself, and
				 * the room would be left the colour of its light.
				 */
				grey[0] += cr / cg;
				grey[1] += cb / cg;
				s->grey_cells++;
				/*
				 * Grey, or within a third of it in red and blue, with the white balance as it is: what
				 * the white balance should make grey, without the coloured things around it (the
				 * colour matrix makes any mistake in it twice as plain).
				 */
				if (isp != NULL) {
					float wr = cr * isp->wb[0] / cg, wb = cb * isp->wb[2] / cg;

					if (wr > 0.75f && wr < 1.33f && wb > 0.75f && wb < 1.33f) {
						neutral[0] += cr / cg;
						neutral[1] += cb / cg;
						s->neutral_cells++;
					}
				}
			}
			s->hist[top >= 1008 ? 63 : (int)top >> 4]++;
			s->cells++;
		}
	}
	for (int c = 0; c < 3; c++) {
		s->mean[c] = s->cells ? sum[c] / s->cells : 0;
	}
	s->key = s->cells ? exp(logs / s->cells) : 0;
	for (int c = 0; c < 2; c++) {
		s->grey[c] = s->grey_cells ? grey[c] / s->grey_cells : 0;
		s->neutral[c] = s->neutral_cells ? neutral[c] / s->neutral_cells : 0;
	}
	for (int i = 63, above = 0; i >= 0; i--) {
		above += s->hist[i];
		if (above * 100 >= (int)s->cells) {
			s->bright = i * 16 + 8;
			break;
		}
	}
}

/*
 * The picture's middle grey (the key: green's log average, lens-shaded) towards 18 % of white (times
 * the exposure bias asked for), unless
 * that would put 1 % of the picture into white (then down to half of it, the tone curve's drc making
 * up for it). 18 %: a grey card's, which ISO 12232's standard output sensitivity puts at 118 of 255 in
 * sRGB, as the gamma here does. The log average: the scene in stops as exposure counts them, so that a
 * lamp or a window in it does not darken the rest as it would a mean's. The exposure, longest first:
 * the frame's lines at the shortest frame, then gain to 16x, then frames up to max_frame_lines, then
 * gain to the most; beyond that, digital gain (up to 4x).
 */
bool
isp_ae(struct isp *isp, const struct annibale_camera_frame *f, const struct isp_stats *s, struct annibale_camera_command *c)
{
	if (f->exposure_lines == 0 || f->line_ns == 0 || f->gain == 0 || f->gain_unity == 0 ||
	    f->gain_max < f->gain_unity || f->frame_lines_min <= f->margin || f->frame_lines_max < f->frame_lines_min) {
		return false;
	}
	double now = (double)f->exposure_lines * f->gain / f->gain_unity * isp->digital, ratio, want;
	double white = WHITE - f->black, target = 0.18 * white * isp->bias, mean = s->key * isp->digital > 0.5 ? s->key * isp->digital : 0.5;
	double bright = s->bright * isp->digital;
	uint32_t fast = f->frame_lines_min - f->margin, longest = isp->max_frame_lines ? isp->max_frame_lines : 2 * f->frame_lines_min;
	uint32_t lines, gain, frame_lines = 0;
	double digital = 1, drc;

	/*
	 * As Qualcomm's ADRC does, on this frame's picture: where the highlights keep the exposure short of
	 * what the key wants (to half of it at most, below), the tone curve raises the rest by what is
	 * missing and compresses the highlights under white instead of cutting them.
	 */
	drc = target / mean * (bright > 0.9 * white ? 1 : bright / (0.9 * white));
	drc = drc < 1 ? 1 : drc > 2 ? 2 : drc;
	double next_drc = isp->drc > 0 ? isp->drc * 0.75 + drc * 0.25 : drc;
	if (fabs(next_drc - isp->drc) > 0.01 * isp->drc) {
		tone(isp, (float)next_drc);
	}
	if (isp_ae_pending(isp, f)) {
		isp->ae_wait--;
		return false;                   /* (the last change not in the frames yet) */
	}
	isp->ae_wait = 0;
	/* (the measured levels are the sensor's: digital gain is applied to the picture only) */
	ratio = target / mean;
	if (bright * ratio > 0.9 * white && bright > 0) {
		ratio = 0.9 * white / bright > target / mean * 0.5 ? 0.9 * white / bright : target / mean * 0.5;
	}
	ratio = ratio < 0.25 ? 0.25 : ratio > 4 ? 4 : ratio;
	if (ratio > 0.94 && ratio < 1.06) {
		return false;
	}
	double damp = (ratio < 0.5 || ratio > 2.0) ? 0.85 : 0.7;
	want = now * pow(ratio, damp);
	if (want <= fast) {
		lines = want < 4 ? 4 : (uint32_t)want;
		gain = f->gain_unity;
	} else if (want <= (double)fast * 16) {
		lines = fast;
		gain = (uint32_t)(want / fast * f->gain_unity);
	} else {
		lines = (uint32_t)(want / 16);
		lines = lines > longest - f->margin ? longest - f->margin : lines < fast ? fast : lines;
		frame_lines = lines + f->margin;
		double g = want / lines;

		if (g > (double)f->gain_max / f->gain_unity) {
			digital = g / ((double)f->gain_max / f->gain_unity);
			digital = digital > 4 ? 4 : digital;
			g = (double)f->gain_max / f->gain_unity;
		}
		gain = (uint32_t)(g * f->gain_unity);
	}
	isp->digital = (float)digital;
	isp->asked_lines = lines;
	isp->asked_gain = gain;
	isp->asked_frame_lines = frame_lines;
	isp->ae_wait = ISP_AE_READBACK_FRAMES;
	memset(c, 0, sizeof(*c));
	c->command = ANNIBALE_CAMERA_EXPOSURE;
	c->a = lines;
	c->b = gain;
	c->c = frame_lines;
	return true;
}

/*
 * The point of the reference lights' line nearest a light seen as red over green rg and blue over
 * green bg, on their logarithms: where along it (segment i and how far into it, 0 to 1).
 */
static void
on_reference_line(const struct isp_tuning *t, float rg, float bg, uint32_t *segment, float *along)
{
	float x = logf(rg), y = logf(bg), best = INFINITY;

	*segment = 0;
	*along = 0;
	for (uint32_t i = 0; i + 1 < t->refs; i++) {
		float x0 = logf(t->ref[i][0]), y0 = logf(t->ref[i][1]), dx = logf(t->ref[i + 1][0]) - x0,
		    dy = logf(t->ref[i + 1][1]) - y0, length = dx * dx + dy * dy;
		float u = length > 0 ? ((x - x0) * dx + (y - y0) * dy) / length : 0, d;

		u = u < 0 ? 0 : u > 1 ? 1 : u;
		d = (x0 + u * dx - x) * (x0 + u * dx - x) + (y0 + u * dy - y) * (y0 + u * dy - y);
		if (d < best) {
			best = d;
			*segment = i;
			*along = u;
		}
	}
}

float
isp_cct(const struct isp *isp)
{
	const struct isp_tuning *t = isp->tuning;
	uint32_t i;
	float u;

	if (t == NULL || t->refs < 2 || isp->wb[0] <= 0 || isp->wb[2] <= 0) {
		return 0;
	}
	on_reference_line(t, 1 / isp->wb[0], 1 / isp->wb[2], &i, &u);
	return t->ref[i][2] + u * (t->ref[i + 1][2] - t->ref[i][2]);
}

void
isp_awb(struct isp *isp, const struct isp_stats *s)
{
	/*
	 * The nearly grey cells, when there are enough of them (1 in 32); else every cell neither near
	 * white nor near black, when there are enough of those (1 in 16); else every cell's light.
	 */
	float r, b;

	if (s->neutral_cells * 32 >= s->cells && s->neutral[0] > 0.01 && s->neutral[1] > 0.01) {
		r = (float)(1 / s->neutral[0]);
		b = (float)(1 / s->neutral[1]);
	} else if (s->grey_cells * 16 >= s->cells && s->grey[0] > 0.01 && s->grey[1] > 0.01) {
		r = (float)(1 / s->grey[0]);
		b = (float)(1 / s->grey[1]);
	} else if (s->mean[0] >= 2 && s->mean[1] >= 2 && s->mean[2] >= 2) {
		r = (float)(s->mean[1] / s->mean[0]);
		b = (float)(s->mean[1] / s->mean[2]);
	} else {
		return;
	}

	/*
	 * Onto the line of the reference lights: a scene's own colours pull grey world off it -- a pink
	 * screen, a green wall -- and off it is what shows as green or magenta, while along it is a
	 * warmer or colder white. (The wide camera under a warm white LED: grey world of the room with a
	 * screen in it 15 % short in both red and blue, the light measured on a white sheet; kept to the
	 * line, 2 % off.)
	 */
	if (isp->tuning != NULL && isp->tuning->refs >= 2) {
		const struct isp_tuning *t = isp->tuning;
		uint32_t i;
		float u;

		on_reference_line(t, 1 / r, 1 / b, &i, &u);
		r = 1 / expf(logf(t->ref[i][0]) + u * (logf(t->ref[i + 1][0]) - logf(t->ref[i][0])));
		b = 1 / expf(logf(t->ref[i][1]) + u * (logf(t->ref[i + 1][1]) - logf(t->ref[i][1])));
	}
	r = r < 0.5f ? 0.5f : r > 4 ? 4 : r;
	b = b < 0.5f ? 0.5f : b > 4 ? 4 : b;
	isp->wb[0] = isp->wb[0] * 0.9f + r * 0.1f;
	isp->wb[2] = isp->wb[2] * 0.9f + b * 0.1f;
}

/* the part of the sensor's cells the picture shows: its middle, in the picture's proportions, 1/zoom of it each way */
static void
crop(const struct isp *isp, const struct annibale_camera_frame *f, uint32_t width, uint32_t height, uint32_t *x0, uint32_t *y0,
    uint32_t *cw, uint32_t *ch)
{
	uint32_t w = f->width / 2, h = f->height / 2;
	float zoom = isp->zoom < 1 ? 1 : isp->zoom;

	if ((uint64_t)width * h > (uint64_t)height * w) {
		*cw = w;
		*ch = (uint32_t)((uint64_t)w * height / width);
	} else {
		*ch = h;
		*cw = (uint32_t)((uint64_t)h * width / height);
	}
	*cw = (uint32_t)(*cw / zoom) < 2 ? 2 : (uint32_t)(*cw / zoom);
	*ch = (uint32_t)(*ch / zoom) < 2 ? 2 : (uint32_t)(*ch / zoom);
	*x0 = (w - *cw) / 2;
	*y0 = (h - *ch) / 2;
}

void
isp_set_ccm(struct isp *isp, const float m[9])
{
	for (int i = 0; i < 9; i++) {
		isp->ccm[i / 3][i % 3] = m[i];
	}
}

/*
 * The colour matrix with the white balance's and AE's gains folded in, in the gamma table's steps
 * (per 10-bit step above black): out[c] = sum over j of m[c][j] * value[j] (each value lens-shaded).
 */
struct pipe {
	float           m[3][3];
	bool            identity;
};

static void
pipe_init(const struct isp *isp, struct pipe *p, uint32_t black)
{
	p->identity = true;
	for (int c = 0; c < 3; c++) {
		for (int j = 0; j < 3; j++) {
			p->m[c][j] = isp->ccm[c][j] * isp->wb[j] * isp->digital * (LUT_WHITE / (float)(black < WHITE ? WHITE - black : 1));
			p->identity = p->identity && (c == j ? isp->ccm[c][j] == 1 : isp->ccm[c][j] == 0);
		}
	}
}

static inline uint8_t
curve(const struct isp *isp, float v)
{
	int i = (int)(v + 0.5f);

	return isp->gamma[i < 0 ? 0 : i > 4095 ? 4095 : i];
}

static inline void
pixel(const struct isp *isp, const struct pipe *p, float r, float g, float b, uint8_t *rgb)
{
	if (p->identity) {
		rgb[0] = curve(isp, r * p->m[0][0]);
		rgb[1] = curve(isp, g * p->m[1][1]);
		rgb[2] = curve(isp, b * p->m[2][2]);
		return;
	}
	rgb[0] = curve(isp, p->m[0][0] * r + p->m[0][1] * g + p->m[0][2] * b);
	rgb[1] = curve(isp, p->m[1][0] * r + p->m[1][1] * g + p->m[1][2] * b);
	rgb[2] = curve(isp, p->m[2][0] * r + p->m[2][1] * g + p->m[2][2] * b);
}

/* the lens shading's gains at (u, v), 0 to 1 across the sensor */
static inline void
lsc_at(const struct isp *isp, float u, float v, float g[3])
{
	float gx = u * (LSC_W - 1), gy = v * (LSC_H - 1);
	int x = (int)gx, y = (int)gy;

	x = x < 0 ? 0 : x > LSC_W - 2 ? LSC_W - 2 : x;
	y = y < 0 ? 0 : y > LSC_H - 2 ? LSC_H - 2 : y;
	float fx = gx - x, fy = gy - y;

	for (int c = 0; c < 3; c++) {
		const float (*t)[LSC_W] = isp->lsc.gain[c];

		g[c] = (t[y][x] * (1 - fx) + t[y][x + 1] * fx) * (1 - fy) + (t[y + 1][x] * (1 - fx) + t[y + 1][x + 1] * fx) * fy;
	}
}

/*
 * A line of cells (cy) as R, G, B above black, lens-shaded: what the magnified picture is made of,
 * each line made once for the two or three lines of the picture between it and the next.
 */
struct x_sample {
	uint32_t left, right;          /* channel offsets in the source RGB row */
	float weight;
};

struct cell_lines {
	struct x_sample *horizontal;
	uint32_t x0, y0, cw, ch;
	uint32_t first_cell, last_cell;
	int32_t         cy[2];          /* which line each holds (-1: none) */
	float           *rgb[2];        /* 3 floats a cell */
	float           *vert;          /* the two blended top to bottom, once a line */
};

static void
cell_line(const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw, uint32_t cy, int r, int b,
    const struct cell_lines *lines, float *out)
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
	float row[3][LSC_W];

	if (isp->lsc.on) {
		/* (the grid's line here, once: across the line only x changes) */
		float gy = cells_h > 1 ? (float)cy / (cells_h - 1) * (LSC_H - 1) : 0;
		int y = (int)gy > LSC_H - 2 ? LSC_H - 2 : (int)gy;
		float fy = gy - y;

		for (int c = 0; c < 3; c++) {
			for (int x = 0; x < LSC_W; x++) {
				row[c][x] = isp->lsc.gain[c][y][x] * (1 - fy) + isp->lsc.gain[c][y + 1][x] * fy;
			}
		}
	}
	for (uint32_t cx = lines->first_cell; cx <= lines->last_cell; cx++) {
		int v[4], g = 0;
		float gains[3] = { 1, 1, 1 };

		cell(top, f->stride, cx, f->black, v);
		for (int j = 0; j < 4; j++) {
			g += j != r && j != b ? v[j] : 0;
		}
		if (isp->lsc.on) {
			float gx = cells_w > 1 ? (float)cx / (cells_w - 1) * (LSC_W - 1) : 0;
			int x = (int)gx > LSC_W - 2 ? LSC_W - 2 : (int)gx;
			float fx = gx - x;

			for (int c = 0; c < 3; c++) {
				gains[c] = row[c][x] + (row[c][x + 1] - row[c][x]) * fx;
			}
		}
		out[3 * cx] = v[r] * gains[0];
		out[3 * cx + 1] = g * 0.5f * gains[1];
		out[3 * cx + 2] = v[b] * gains[2];
	}
}

/* the cells of line cy, made into one of the two slots unless one has them (keeping `keep''s) */
static const float *
cells_of(const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw, uint32_t cy, int r, int b,
    struct cell_lines *lines, int32_t keep)
{
	for (int i = 0; i < 2; i++) {
		if (lines->cy[i] == (int32_t)cy) {
			return lines->rgb[i];
		}
	}
	int slot = lines->cy[0] == keep ? 1 : 0;

	cell_line(isp, f, raw, cy, r, b, lines, lines->rgb[slot]);
	lines->cy[slot] = (int32_t)cy;
	return lines->rgb[slot];
}

/* one line of the picture as 8-bit R, G, B (lines: the cells made for the line before, kept) */
static void
render_line(const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw, uint32_t width, uint32_t height,
    uint32_t row, uint8_t *rgb, struct cell_lines *lines)
{
	uint32_t x0 = lines->x0, y0 = lines->y0, cw = lines->cw, ch = lines->ch;
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	int r, b;
	struct pipe p;

	cell_places(f->bayer, &r, &b);
	pipe_init(isp, &p, f->black);
	if (isp->turned) {
		row = height - 1 - row;         /* (and each line from its end, below) */
	}
	if (lines->horizontal != NULL) {
		/*
		 * Magnified (the zoom): each pixel between the four cells around it. The nearest cell blown up
		 * to a square of pixels showed every cell's own colour, its noise and the colours a 2x2 cell
		 * gets wrong at an edge, as coloured dots.
		 */
		float fy = y0 + (row + 0.5f) * ch / height - 0.5f, ty;
		uint32_t cy0 = fy < 0 ? 0 : (uint32_t)fy, cy1;

		cy0 = cy0 > cells_h - 1 ? cells_h - 1 : cy0;
		cy1 = cy0 + 1 > cells_h - 1 ? cells_h - 1 : cy0 + 1;
		ty = fy < (float)cy0 ? 0 : fy - cy0;
		const float *a = cells_of(isp, f, raw, cy0, r, b, lines, -1);
		const float *c1 = cells_of(isp, f, raw, cy1, r, b, lines, (int32_t)cy0);
		/*
		 * The two cell lines are blended top to bottom once for the whole line, so each pixel's
		 * colour is one lerp across cells instead of three. And since the colour matrix is a
		 * linear map, a run of pixels between the same two cells (a `sample->left' that does not
		 * change) shares one pair of line equations: out[c] = curve(m_c.A + m_c.D * tx), where
		 * every pixel used to lerp twice and apply the matrix in full.
		 */
		float *vert = lines->vert;
		uint32_t i0 = lines->first_cell * 3, n = (lines->last_cell - lines->first_cell + 1) * 3;
		uint8_t *dst = rgb + (isp->turned ? 3 * (width - 1) : 0);
		const int dstep = isp->turned ? -3 : 3;

		for (uint32_t i = 0; i < n; i++) {
			vert[i] = a[i0 + i] + (c1[i0 + i] - a[i0 + i]) * ty;
		}
		for (uint32_t x = 0; x < width;) {
			const struct x_sample *sample = &lines->horizontal[x];
			const float *v = vert + sample->left - i0;
			uint32_t d = sample->right - sample->left, xe = x + 1;
			float e[3], g[3];

			while (xe < width && lines->horizontal[xe].left == sample->left) {
				xe++;
			}
			for (int c = 0; c < 3; c++) {
				e[c] = p.m[c][0] * v[0] + p.m[c][1] * v[1] + p.m[c][2] * v[2];
				g[c] = p.m[c][0] * (v[d] - v[0]) + p.m[c][1] * (v[d + 1] - v[1]) + p.m[c][2] * (v[d + 2] - v[2]);
			}
			float32x4_t ve = { e[0], e[1], e[2], 0 }, vg = { g[0], g[1], g[2], 0 };

			for (; x < xe; x++, dst += dstep) {
				float32x4_t colour = vfmaq_n_f32(ve, vg, lines->horizontal[x].weight);
				int32x4_t at = vcvtq_s32_f32(vaddq_f32(colour, vdupq_n_f32(0.5f)));

				at = vmaxq_s32(at, vdupq_n_s32(0));
				at = vminq_s32(at, vdupq_n_s32(LUT_WHITE + 3));
				dst[0] = isp->gamma[vgetq_lane_s32(at, 0)];
				dst[1] = isp->gamma[vgetq_lane_s32(at, 1)];
				dst[2] = isp->gamma[vgetq_lane_s32(at, 2)];
			}
		}
		return;
	}
	uint32_t cy = y0 + (uint32_t)((uint64_t)row * ch / height);
	const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
	uint64_t step = ((uint64_t)cw << 16) / width, at = (uint64_t)x0 << 16;
	float v_pos = cells_h > 1 ? (float)cy / (cells_h - 1) : 0, gains[3] = { 1, 1, 1 };

	for (uint32_t x = 0; x < width; x++, at += step) {
		uint32_t cx = (uint32_t)(at >> 16);
		int v[4], g = 0;

		cell(top, f->stride, cx, f->black, v);
		for (int j = 0; j < 4; j++) {
			g += j != r && j != b ? v[j] : 0;
		}
		if (isp->lsc.on) {
			lsc_at(isp, cells_w > 1 ? (float)cx / (cells_w - 1) : 0, v_pos, gains);
		}
		pixel(isp, &p, v[r] * gains[0], g * 0.5f * gains[1], v[b] * gains[2], rgb + 3 * (isp->turned ? width - 1 - x : x));
	}
}

/* A worker uses the same source columns for every row in its output slice. */
static struct cell_lines
thread_cell_lines(const struct isp *isp, const struct annibale_camera_frame *f, uint32_t width, uint32_t height)
{
	static __thread void *storage;
	static __thread size_t capacity;
	struct cell_lines lines = { .cy = { -1, -1 } };
	size_t cells = f->width / 2, row_bytes = cells * 3 * sizeof(float);
	size_t need = 3 * row_bytes + width * sizeof(struct x_sample);

	crop(isp, f, width, height, &lines.x0, &lines.y0, &lines.cw, &lines.ch);
	if (lines.cw >= width || lines.ch >= height)
		return lines;
	if (need > capacity) {
		void *resized = realloc(storage, need);
		if (resized == NULL)
			return lines;
		storage = resized;
		capacity = need;
	}
	lines.horizontal = storage;
	lines.rgb[0] = (float *)(lines.horizontal + width);
	lines.rgb[1] = lines.rgb[0] + 3 * cells;
	lines.vert = lines.rgb[1] + 3 * cells;
	for (uint32_t x = 0; x < width; x++) {
		float fx = lines.x0 + (x + 0.5f) * lines.cw / width - 0.5f;
		uint32_t left = fx < 0 ? 0 : (uint32_t)fx;
		left = left >= cells ? (uint32_t)cells - 1 : left;
		uint32_t right = left + 1 < cells ? left + 1 : left;

		lines.horizontal[x] = (struct x_sample){ 3 * left, 3 * right, fx < (float)left ? 0 : fx - left };
	}
	/* The interpolation endpoints include the neighbouring cells at both crop edges. */
	lines.first_cell = lines.horizontal[0].left / 3;
	lines.last_cell = lines.horizontal[width - 1].right / 3;
	return lines;
}

/*
 * Two lines of 8-bit R, G, B as NV12 (BT.601's matrix, which CoreVideo's 420f and 420v and
 * tools/camraw.py take): each pixel's Y (y1 NULL: the second line has none, the picture's last of an
 * odd height), each 2x2's Cb and Cr from the sum of its four. The coefficients in 1/65536ths, video
 * range's scaled to its 219 steps of luma (16 to 235) and 224 of chroma (16 to 240); every value
 * rounded to the nearest step, so that the primaries come back within one step of 0 and 255 (red's and
 * blue's cannot be exact: no 8-bit Y, Cb, Cr is both).
 */
static void
nv12_pair(const uint8_t *rgb0, const uint8_t *rgb1, uint32_t width, uint8_t *y0, uint8_t *y1, uint8_t *uv, bool video_range)
{
	static const int32_t full[9] = { 19595, 38470, 7471, -11058, -21710, 32768, 32768, -27439, -5329 };
	static const int32_t video[9] = { 16829, 33039, 6416, -9714, -19070, 28784, 28784, -24103, -4681 };
	const int32_t *k = video_range ? video : full;
	int32_t oy = (video_range ? 16 << 16 : 0) + (1 << 15);

	for (int l = 0; l < 2; l++) {
		const uint8_t *p = l ? rgb1 : rgb0;
		uint8_t *out = l ? y1 : y0;
		uint32_t x = 0;

		for (; out != NULL && x + 16 <= width; x += 16, p += 48) {
			uint8x16x3_t rgbv = vld3q_u8(p);
			uint8x8_t y8[2];

			for (int h = 0; h < 2; h++) {
				uint16x8_t r16 = vmovl_u8(h ? vget_high_u8(rgbv.val[0]) : vget_low_u8(rgbv.val[0]));
				uint16x8_t g16 = vmovl_u8(h ? vget_high_u8(rgbv.val[1]) : vget_low_u8(rgbv.val[1]));
				uint16x8_t b16 = vmovl_u8(h ? vget_high_u8(rgbv.val[2]) : vget_low_u8(rgbv.val[2]));
				uint32x4_t a0 = vmull_u16(vget_low_u16(r16), vdup_n_u16(k[0]));
				uint32x4_t a1 = vmull_u16(vget_high_u16(r16), vdup_n_u16(k[0]));

				a0 = vmlal_u16(a0, vget_low_u16(g16), vdup_n_u16(k[1]));
				a1 = vmlal_u16(a1, vget_high_u16(g16), vdup_n_u16(k[1]));
				a0 = vmlal_u16(a0, vget_low_u16(b16), vdup_n_u16(k[2]));
				a1 = vmlal_u16(a1, vget_high_u16(b16), vdup_n_u16(k[2]));
				y8[h] = vmovn_u16(vcombine_u16(
				    vmovn_u32(vshrq_n_u32(vaddq_u32(a0, vdupq_n_u32(oy)), 16)),
				    vmovn_u32(vshrq_n_u32(vaddq_u32(a1, vdupq_n_u32(oy)), 16))));
			}
			vst1q_u8(out + x, vcombine_u8(y8[0], y8[1]));
		}
		for (; out != NULL && x < width; x++, p += 3) {
			out[x] = (uint8_t)((k[0] * p[0] + k[1] * p[1] + k[2] * p[2] + oy) >> 16);
		}
	}
	uint32_t x = 0;
	for (; x + 16 <= width; x += 16) {
		uint8x16x3_t a = vld3q_u8(rgb0 + 3 * x), b = vld3q_u8(rgb1 + 3 * x);
		uint16x8_t R = vpaddq_u16(vaddl_u8(vget_low_u8(a.val[0]), vget_low_u8(b.val[0])),
		    vaddl_u8(vget_high_u8(a.val[0]), vget_high_u8(b.val[0])));
		uint16x8_t G = vpaddq_u16(vaddl_u8(vget_low_u8(a.val[1]), vget_low_u8(b.val[1])),
		    vaddl_u8(vget_high_u8(a.val[1]), vget_high_u8(b.val[1])));
		uint16x8_t B = vpaddq_u16(vaddl_u8(vget_low_u8(a.val[2]), vget_low_u8(b.val[2])),
		    vaddl_u8(vget_high_u8(a.val[2]), vget_high_u8(b.val[2])));
		int16x4_t cb16[2], cr16[2];

		for (int h = 0; h < 2; h++) {
			uint16x4_t rh = h ? vget_high_u16(R) : vget_low_u16(R);
			uint16x4_t gh = h ? vget_high_u16(G) : vget_low_u16(G);
			uint16x4_t bh = h ? vget_high_u16(B) : vget_low_u16(B);
			/*
			 * The negative coefficients subtract (the widening multiplies are unsigned): the
			 * rounding offset first, the positive terms added, the rest taken away. The offset
			 * alone is larger than what the subtractions can take, so the running sum stays
			 * above zero as the plain arithmetic's would.
			 */
			uint32x4_t cb = vdupq_n_u32((128 << 18) + (1 << 17) - 1);
			uint32x4_t cr = vdupq_n_u32((128 << 18) + (1 << 17) - 1);

			cb = vmlal_u16(cb, bh, vdup_n_u16(k[5]));
			cr = vmlal_u16(cr, rh, vdup_n_u16(k[6]));
			cb = vmlsl_u16(cb, rh, vdup_n_u16((uint16_t)-k[3]));
			cb = vmlsl_u16(cb, gh, vdup_n_u16((uint16_t)-k[4]));
			cr = vmlsl_u16(cr, gh, vdup_n_u16((uint16_t)-k[7]));
			cr = vmlsl_u16(cr, bh, vdup_n_u16((uint16_t)-k[8]));
			cb16[h] = vqmovn_s32(vshrq_n_s32(vreinterpretq_s32_u32(cb), 18));
			cr16[h] = vqmovn_s32(vshrq_n_s32(vreinterpretq_s32_u32(cr), 18));
		}
		uint8x8x2_t pair;

		pair.val[0] = vqmovun_s16(vcombine_s16(cb16[0], cb16[1]));
		pair.val[1] = vqmovun_s16(vcombine_s16(cr16[0], cr16[1]));
		vst2_u8(uv + x, pair);
	}
	for (; x < width; x += 2) {
		uint32_t x1 = x + 1 < width ? x + 1 : x;
		int32_t R = rgb0[3 * x] + rgb0[3 * x1] + rgb1[3 * x] + rgb1[3 * x1];
		int32_t G = rgb0[3 * x + 1] + rgb0[3 * x1 + 1] + rgb1[3 * x + 1] + rgb1[3 * x1 + 1];
		int32_t B = rgb0[3 * x + 2] + rgb0[3 * x1 + 2] + rgb1[3 * x + 2] + rgb1[3 * x1 + 2];
		/*
		 * (the sums are four pixels': 18 bits off, with 128 added and just under half a step to round;
		 * a tie goes down, so that chroma's -127.5, a full colour's, comes out 0 as its +127.5 does 255)
		 */
		int32_t cb = (k[3] * R + k[4] * G + k[5] * B + (128 << 18) + (1 << 17) - 1) >> 18;
		int32_t cr = (k[6] * R + k[7] * G + k[8] * B + (128 << 18) + (1 << 17) - 1) >> 18;

		uv[x] = (uint8_t)(cb < 0 ? 0 : cb > 255 ? 255 : cb);
		uv[x + 1] = (uint8_t)(cr < 0 ? 0 : cr > 255 ? 255 : cr);
	}
}

void
isp_render_nv12(const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint8_t line[2][3 * 8192];

	if (width == 0 || width > 8192 || height == 0) {
		return;
	}
	struct cell_lines lines = thread_cell_lines(isp, f, width, height);
	for (uint32_t row = first_row & ~1u; row < end_row && row < height; row += 2) {
		bool second = row + 1 < height;

		render_line(isp, f, raw, width, height, row, line[0], &lines);
		render_line(isp, f, raw, width, height, second ? row + 1 : row, line[1], &lines);
		nv12_pair(line[0], line[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

void
isp_render_bgra(const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row)
{
	uint8_t line[3 * 8192];

	if (width == 0 || width > 8192 || height == 0) {
		return;
	}
	struct cell_lines lines = thread_cell_lines(isp, f, width, height);
	for (uint32_t row = first_row; row < end_row && row < height; row++) {
		uint8_t *o = out + (size_t)row * stride;

		render_line(isp, f, raw, width, height, row, line, &lines);
		for (uint32_t x = 0; x < width; x++) {
			o[4 * x] = line[3 * x + 2];
			o[4 * x + 1] = line[3 * x + 1];
			o[4 * x + 2] = line[3 * x];
			o[4 * x + 3] = 255;
		}
	}
}

/* ---- every pixel: bilinear demosaic -------------------------------------------------------------------- */

/* the colour of each site of a 2x2 cell (0 R, 1 G, 2 B), position y * 2 + x */
static void
cfa(uint32_t bayer, int colour[4])
{
	int r, b;

	cell_places(bayer, &r, &b);
	for (int i = 0; i < 4; i++) {
		colour[i] = i == r ? 0 : i == b ? 2 : 1;
	}
}

/* line y of the frame as values above black (y clamped into the frame) */
static void
unpack_line(const struct annibale_camera_frame *f, const uint8_t *raw, int y, int *out)
{
	y = y < 0 ? 0 : y >= (int)f->height ? (int)f->height - 1 : y;
	const uint8_t *line = raw + (size_t)y * f->stride;

	for (uint32_t x = 0; x + 3 < f->width + 3 && x < f->width; x += 4) {
		const uint8_t *g = line + (x >> 2) * 5;

		for (int i = 0; i < 4 && x + i < f->width; i++) {
			int v = (g[i] << 2 | (g[4] >> (i * 2) & 3)) - (int)f->black;

			out[x + i] = v < 0 ? 0 : v;
		}
	}
}

#include "camera_isp_demosaic.inc"

void
isp_render_full_nv12(const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint32_t width = f->width;
	int colour[4];
	struct pipe p;
	static __thread int *lines[4];
	static __thread uint8_t *rgb[2];
	static __thread uint32_t have;

	if (width > have) {
		for (int i = 0; i < 4; i++) {
			free(lines[i]);
			lines[i] = malloc(sizeof(int) * width);
		}
		for (int i = 0; i < 2; i++) {
			free(rgb[i]);
			rgb[i] = malloc(3 * (size_t)width);
		}
		have = width;
	}
	cfa(f->bayer, colour);
	pipe_init(isp, &p, f->black);
	for (uint32_t row = first_row & ~1u; row < end_row && row < f->height; row += 2) {
		for (int i = 0; i < 4; i++) {
			unpack_line(f, raw, (int)row - 1 + i, lines[i]);
		}
		const int *const above[3] = { lines[0], lines[1], lines[2] }, *const below[3] = { lines[1], lines[2], lines[3] };

		bool second = row + 1 < f->height;

		full_line(isp, f, above, row, colour, &p, rgb[0]);
		full_line(isp, f, below, second ? row + 1 : row, colour, &p, rgb[1]);
		nv12_pair(rgb[0], rgb[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

/* one line (y) of the sensor demosaiced, as 8-bit R, G, B (the unpacked lines around it in this thread's l) */
static void
demosaic_row(const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw, uint32_t row, const int colour[4],
    const struct pipe *p, int *const l[3], uint8_t *rgb)
{
	for (int i = 0; i < 3; i++) {
		unpack_line(f, raw, (int)row - 1 + i, l[i]);
	}
	full_line(isp, f, (const int *const *)l, row, colour, p, rgb);
}

void
isp_render_still_nv12(const struct isp *isp, const struct annibale_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint32_t x0, y0, cw, ch, fw = f->width;
	int colour[4];
	struct pipe p;
	static __thread int *lines[3];
	static __thread uint8_t *rows[2], *out[2];
	static __thread uint32_t have_fw, have_width;
	int32_t cached[2] = { -1, -1 };

	if (fw > have_fw) {
		for (int i = 0; i < 3; i++) {
			free(lines[i]);
			lines[i] = malloc(sizeof(int) * fw);
		}
		for (int i = 0; i < 2; i++) {
			free(rows[i]);
			rows[i] = malloc(3 * (size_t)fw);
		}
		have_fw = fw;
	}
	if (width > have_width) {
		for (int i = 0; i < 2; i++) {
			free(out[i]);
			out[i] = malloc(3 * (size_t)width);
		}
		have_width = width;
	}
	if (lines[2] == NULL || rows[1] == NULL || out[1] == NULL) {
		return;
	}
	crop(isp, f, width, height, &x0, &y0, &cw, &ch);
	cfa(f->bayer, colour);
	pipe_init(isp, &p, f->black);
	float sx_step = 2.0f * cw / width, sy_step = 2.0f * ch / height;

	for (uint32_t row = first_row & ~1u; row < end_row && row < height; row += 2) {
		for (int k = 0; k < 2; k++) {
			uint32_t r = row + k < height ? row + k : row, at = isp->turned ? height - 1 - r : r;
			float sy = 2.0f * y0 + (at + 0.5f) * sy_step - 0.5f, ty;
			uint32_t sy0 = sy < 0 ? 0 : (uint32_t)sy, sy1, slot[2];

			sy0 = sy0 > f->height - 1 ? f->height - 1 : sy0;
			sy1 = sy0 + 1 > f->height - 1 ? f->height - 1 : sy0 + 1;
			ty = sy < (float)sy0 ? 0 : sy - sy0;
			/* the two lines of the sensor it is between, demosaiced (kept from the line before where they are the same) */
			for (int n = 0; n < 2; n++) {
				uint32_t want = n ? sy1 : sy0;

				if (cached[0] == (int32_t)want || cached[1] == (int32_t)want) {
					slot[n] = cached[0] == (int32_t)want ? 0 : 1;
					continue;
				}
				/* (into the slot the other line does not need: sy0's for sy1, and sy1's kept for sy0) */
				slot[n] = n == 1 ? slot[0] ^ 1 : cached[0] == (int32_t)sy1 ? 1 : 0;
				demosaic_row(isp, f, raw, want, colour, &p, lines, rows[slot[n]]);
				cached[slot[n]] = (int32_t)want;
			}
			const uint8_t *a = rows[slot[0]], *b = rows[slot[1]];
			uint8_t *o = out[k];

			for (uint32_t x = 0; x < width; x++) {
				float sx = 2.0f * x0 + (x + 0.5f) * sx_step - 0.5f, tx;
				uint32_t sx0 = sx < 0 ? 0 : (uint32_t)sx, sx1;

				sx0 = sx0 > fw - 1 ? fw - 1 : sx0;
				sx1 = sx0 + 1 > fw - 1 ? fw - 1 : sx0 + 1;
				tx = sx < (float)sx0 ? 0 : sx - sx0;
				uint8_t *px = o + 3 * (isp->turned ? width - 1 - x : x);

				for (int c = 0; c < 3; c++) {
					float top = a[3 * sx0 + c] + tx * (a[3 * sx1 + c] - a[3 * sx0 + c]);
					float bottom = b[3 * sx0 + c] + tx * (b[3 * sx1 + c] - b[3 * sx0 + c]);

					px[c] = (uint8_t)(top + ty * (bottom - top) + 0.5f);
				}
			}
		}
		bool second = row + 1 < height;

		nv12_pair(out[0], out[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

#include "camera_isp_tuning.inc"
