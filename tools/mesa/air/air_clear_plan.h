#ifndef AIR_CLEAR_PLAN_H
#define AIR_CLEAR_PLAN_H

#define RWA_MAX_COLOR_ATTACHMENTS 8

struct rwa_color_clear {
	unsigned attachment;
	unsigned write_mask;
	float rgba[4];
};

static inline unsigned
rwa_color_clear_plan(unsigned color_count, unsigned discard, const float *clear_colors,
		     struct rwa_color_clear out[RWA_MAX_COLOR_ATTACHMENTS])
{
	if (clear_colors == 0 || out == 0 || color_count == 0) {
		return 0;
	}
	if (color_count > RWA_MAX_COLOR_ATTACHMENTS) {
		color_count = RWA_MAX_COLOR_ATTACHMENTS;
	}

	unsigned mask = (discard >> 8) & 0xffu;
	if (mask == 0) {
		mask = 1;
	}

	unsigned count = 0;
	for (unsigned i = 0; i < color_count; i++) {
		if ((mask & (1u << i)) == 0) {
			continue;
		}
		out[count].attachment = i;
		out[count].write_mask = 0xfu << (i * 4);
		for (unsigned c = 0; c < 4; c++) {
			out[count].rgba[c] = clear_colors[i * 4 + c];
		}
		count++;
	}
	return count;
}

#endif
