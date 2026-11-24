/* Clamping repeats some sites at the four borders, changing their weights. */
static void
border_rgb(const int *const l[3], int x, int y, int w, int height, const int colour[4], float v[3])
{
	float sum[3] = { 0, 0, 0 };
	int n[3] = { 0, 0, 0 };

	/* each colour: its own value where the site has it, else the mean of its sites in the clamped 3x3 window */
	for (int dy = -1; dy <= 1; dy++) {
		const int *line = l[dy + 1];
		int yy = (int)y + dy;

		yy = yy < 0 ? 0 : yy >= height ? height - 1 : yy;
		for (int dx = -1; dx <= 1; dx++) {
			int xx = x + dx < 0 ? 0 : x + dx >= w ? w - 1 : x + dx;
			int c = colour[(yy & 1) * 2 + (xx & 1)];

			if (dx == 0 && dy == 0) {
				sum[c] = (float)line[xx] * 64;          /* (its own value wins over the neighbours') */
				n[c] = 64;
			} else if (n[c] < 64) {
				sum[c] += (float)line[xx];
				n[c]++;
			}
		}
	}
	for (int c = 0; c < 3; c++) {
		v[c] = n[c] ? sum[c] / n[c] : 0;
	}
}

/* The interior has two green sites for each red or blue site in the Bayer pattern. */
static void
full_line(const struct isp *isp, const struct annibale_camera_frame *f, const int *const l[3], uint32_t y, const int colour[4],
    const struct pipe *p, uint8_t *rgb)
{
	int w = (int)f->width;
	float v_pos = f->height > 1 ? (float)y / (f->height - 1) : 0, gains[3] = { 1, 1, 1 };
	const int *top = l[0], *mid = l[1], *bottom = l[2];

	for (int x = 0; x < w; x++) {
		float v[3];
		int site = (int)(y & 1) * 2 + (x & 1);
		int own = colour[site];

		if (x == 0 || x == w - 1 || y == 0 || y == f->height - 1) {
			border_rgb(l, x, (int)y, w, (int)f->height, colour, v);
		} else {
			int horizontal = mid[x - 1] + mid[x + 1];
			int vertical = top[x] + bottom[x];

			v[own] = (float)mid[x];
			if (own == 1) {
				v[colour[site ^ 1]] = horizontal * 0.5f;
				v[colour[site ^ 2]] = vertical * 0.5f;
			} else {
				v[1] = (horizontal + vertical) * 0.25f;
				v[2 - own] = (top[x - 1] + top[x + 1] + bottom[x - 1] + bottom[x + 1]) * 0.25f;
			}
		}
		if (isp->lsc.on) {
			lsc_at(isp, w > 1 ? (float)x / (w - 1) : 0, v_pos, gains);
		}
		pixel(isp, p, v[0] * gains[0], v[1] * gains[1], v[2] * gains[2], rgb + 3 * x);
	}
}
