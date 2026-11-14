#ifndef AIR_METAL_RGB2P_PRIVATE_H
#define AIR_METAL_RGB2P_PRIVATE_H

static bool
rgb2p_format(unsigned format)
{
   return format == 540 || format == 543 || format == 544;
}

static bool rgb2p_prepare_sample(struct rwa_texture *, char *, size_t);
static void rgb2p_device_close(struct rwa_device *);
static unsigned rgb2p_clear_code(unsigned, float);
static bool lower_rgb2p_outputs(nir_shader *, const struct rwa_render_shape *,
                                 char *, size_t);
#endif
