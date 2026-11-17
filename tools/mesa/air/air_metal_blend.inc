/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 *
 * Metal render-pipeline blend state translation (including sparse MRT slots
 * with MTLPixelFormatInvalid == 0), included by air_metal.c.
 */

static bool
blend_state_from_metal(const struct rwa_render_shape *shape, struct pipe_screen *screen,
                       struct pipe_blend_state *out, unsigned *enabled_mask,
                       unsigned *write_masks, char *why, size_t why_size)
{
   *out = (struct pipe_blend_state){ .rt[0].colormask = PIPE_MASK_RGBA };
   *enabled_mask = 0;
   *write_masks = 0;

   if (shape->color_blend_count == 0) {
      bool sparse = false;
      for (unsigned i = 0; i < shape->color_count && i < 8; i++) {
         if (shape->color_format[i] == 0) {
            sparse = true;
            out->rt[i].colormask = 0;
         } else {
            out->rt[i].colormask = PIPE_MASK_RGBA;
            *write_masks |= PIPE_MASK_RGBA << (4 * i);
         }
      }
      if (sparse) {
         out->max_rt = shape->color_count ? shape->color_count - 1 : 0;
         out->independent_blend_enable = 1;
      }
      return true;
   }
   if (shape->color_blend_count < shape->color_count) {
      say(why, why_size, "this pipeline has %u colour attachment%s and blend state for "
                         "only %u: the attachment%s after that would silently draw with "
                         "blending off and all four channels written, which is what a "
                         "struct filled in only as far as the caller cared about looks "
                         "like", shape->color_count, shape->color_count == 1 ? "" : "s",
          shape->color_blend_count, shape->color_count - shape->color_blend_count == 1
                                       ? "" : "s");
      return false;
   }
   if (shape->color_blend_count > 8) {
      say(why, why_size, "blend state for %u colour attachments, and Metal's descriptor "
                         "has eight", shape->color_blend_count);
      return false;
   }

   out->max_rt = shape->color_count ? shape->color_count - 1 : 0;

   for (unsigned i = 0; i < shape->color_count; i++) {
      const struct rwa_color_blend *b = &shape->color_blend[i];
      struct pipe_rt_blend_state *rt = &out->rt[i];
      char what[160];
      unsigned v;

      if (shape->color_format[i] == 0) {
         rt->colormask = 0;
         rt->blend_enable = 0;
         continue;
      }

      snprintf(what, sizeof(what), "colour attachment %u's writeMask", i);
      if (!color_write_mask_from_metal(b->write_mask, &v, what, why, why_size))
         return false;
      rt->colormask = v;
      *write_masks |= v << (4 * i);

      if (!b->blending_enabled) {
         rt->blend_enable = 0;
         continue;
      }

      const struct rwa_pixel_format *pf = pixel_format(shape->color_format[i]);
      if (pf && util_format_is_pure_integer(pf->pipe)) {
         say(why, why_size, "colour attachment %u is %s and this pipeline enables "
                            "blending on it: a blend equation over integers has no "
                            "defined meaning in Metal, in GL or in Vulkan", i, pf->name);
         return false;
      }

      rt->blend_enable = 1;
      *enabled_mask |= 1u << i;

      snprintf(what, sizeof(what), "colour attachment %u's rgbBlendOperation", i);
      if (!blend_operation_from_metal(b->rgb_blend_operation, &v, what, why, why_size))
         return false;
      rt->rgb_func = v;
      snprintf(what, sizeof(what), "colour attachment %u's alphaBlendOperation", i);
      if (!blend_operation_from_metal(b->alpha_blend_operation, &v, what, why, why_size))
         return false;
      rt->alpha_func = v;

      snprintf(what, sizeof(what), "colour attachment %u's sourceRGBBlendFactor", i);
      if (!blend_factor_from_metal(b->source_rgb_blend_factor, false, &v, what, why,
                                   why_size))
         return false;
      rt->rgb_src_factor = v;
      snprintf(what, sizeof(what), "colour attachment %u's destinationRGBBlendFactor", i);
      if (!blend_factor_from_metal(b->destination_rgb_blend_factor, false, &v, what, why,
                                   why_size))
         return false;
      rt->rgb_dst_factor = v;
      snprintf(what, sizeof(what), "colour attachment %u's sourceAlphaBlendFactor", i);
      if (!blend_factor_from_metal(b->source_alpha_blend_factor, true, &v, what, why,
                                   why_size))
         return false;
      rt->alpha_src_factor = v;
      snprintf(what, sizeof(what), "colour attachment %u's destinationAlphaBlendFactor",
               i);
      if (!blend_factor_from_metal(b->destination_alpha_blend_factor, true, &v, what, why,
                                   why_size))
         return false;
      rt->alpha_dst_factor = v;

      for (unsigned half = 0; half < 2; half++) {
         unsigned op = half ? rt->alpha_func : rt->rgb_func;
         unsigned sf = half ? rt->alpha_src_factor : rt->rgb_src_factor;
         unsigned df = half ? rt->alpha_dst_factor : rt->rgb_dst_factor;
         unsigned msf = half ? b->source_alpha_blend_factor : b->source_rgb_blend_factor;
         unsigned mdf = half ? b->destination_alpha_blend_factor
                             : b->destination_rgb_blend_factor;

         if ((op != PIPE_BLEND_MIN && op != PIPE_BLEND_MAX) ||
             (sf == PIPE_BLENDFACTOR_ONE && df == PIPE_BLENDFACTOR_ONE))
            continue;
         say(why, why_size, "colour attachment %u's %sBlendOperation is %s and its "
                            "factors are %s and %s: Metal's equation multiplies by the "
                            "factors and then takes the minimum or maximum, GL ignores "
                            "the factors for these two operations entirely, and the two "
                            "readings differ unless both factors are "
                            "MTLBlendFactorOne -- which is served, and is the only form "
                            "a maximum-blend is written in", i,
             half ? "alpha" : "rgb",
             blend_operation_name(half ? b->alpha_blend_operation
                                       : b->rgb_blend_operation),
             blend_factor_name(msf), blend_factor_name(mdf));
         return false;
      }

      if (pf && !util_format_has_alpha(pf->pipe)) {
         rt->rgb_dst_factor =
            util_blend_dst_alpha_to_one((enum pipe_blendfactor)rt->rgb_dst_factor);
         rt->rgb_src_factor =
            util_blend_dst_alpha_to_one((enum pipe_blendfactor)rt->rgb_src_factor);
         rt->alpha_dst_factor =
            util_blend_dst_alpha_to_one((enum pipe_blendfactor)rt->alpha_dst_factor);
         rt->alpha_src_factor =
            util_blend_dst_alpha_to_one((enum pipe_blendfactor)rt->alpha_src_factor);
      }
   }

   bool differ = false;
   for (unsigned i = 1; i < shape->color_count; i++)
      differ = differ || !rt_blend_equal(&out->rt[i], &out->rt[0]);
   if (differ) {
      bool enable = screen->caps.indep_blend_enable;
      bool func = screen->caps.indep_blend_func;
      if (!enable || !func) {
         say(why, why_size, "this pipeline's %u colour attachments do not all ask for "
                            "the same blend state, and %s reports %s: with independent "
                            "blending off Gallium reads render target 0's state for "
                            "every attachment, so the others would silently blend with "
                            "the first one's equation", shape->color_count,
             screen->get_name(screen),
             !enable ? "no independent blend enable at all"
                     : "independent enables but a single blend equation");
         return false;
      }
      out->independent_blend_enable = 1;
   }
   return true;
}
