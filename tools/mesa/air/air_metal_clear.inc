/* Render-pass clears run without a pipeline or shader bindings. */
static void
clear_bound_attachments(struct pipe_context *pipe, const struct rwa_draw *draw)
{
   struct rwa_color_clear color_clears[RWA_MAX_COLOR_ATTACHMENTS];
   unsigned color_clear_count = rwa_color_clear_plan(draw->color_count, draw->discard,
                                                     draw->clear_color, color_clears);
   for (unsigned n = 0; n < color_clear_count; n++) {
      const struct rwa_color_clear *item = &color_clears[n];
      union pipe_color_union clear = { 0 };
      for (unsigned c = 0; c < 4; c++) clear.f[c] = item->rgba[c];
      pipe->clear(pipe, PIPE_CLEAR_COLOR0 << item->attachment, item->write_mask, 0xff, NULL, &clear,
                  0.0, 0);
   }

   unsigned depth_stencil_mask = 0;
   if (draw->clear_depth && draw->depth && format_has_depth(draw->depth->res->format))
      depth_stencil_mask |= PIPE_CLEAR_DEPTH;
   if (draw->clear_stencil && draw->depth && format_has_stencil(draw->depth->res->format))
      depth_stencil_mask |= PIPE_CLEAR_STENCIL;
   union pipe_color_union unused = { 0 };
   if (depth_stencil_mask)
      pipe->clear(pipe, depth_stencil_mask, 0xf, 0xff, NULL, &unused,
                  draw->clear_depth ? (double)*draw->clear_depth : 0.0,
                  draw->clear_stencil ? *draw->clear_stencil & 0xff : 0);
}

int
rwa_render_clear(struct rwa_device *device, const struct rwa_draw *draw, char *why, size_t why_size)
{
   RWA_LOCKED;
   if (!device || !draw || draw->color_count > PIPE_MAX_COLOR_BUFS ||
       (draw->color_count && !draw->color)) {
      say(why, why_size, "invalid clear pass");
      return -1;
   }
   unsigned mask = draw->clear_color ? (draw->discard >> 8) & 0xffu : 0;
   if (draw->clear_color && !mask) mask = 1;
   for (unsigned i = 0; i < draw->color_count; i++) {
      if ((mask & (1u << i)) && !draw->color[i]) {
         say(why, why_size, "clear attachment is missing");
         return -1;
      }
   }
   struct pipe_context *pipe = device->pipe;
   struct pipe_framebuffer_state fb = { .nr_cbufs = draw->color_count };
   /* Validate all attachments before changing the context or acquiring references. */
   for (unsigned i = 0; i <= draw->color_count; i++) {
      struct rwa_texture *t = i == draw->color_count ? draw->depth : draw->color[i];
      if (!t) continue;
      if (t->device != device) {
         say(why, why_size, "clear attachment belongs to another device");
         return -1;
      }
      unsigned samples = t->res->nr_samples ? t->res->nr_samples : 1;
      if (!attachment_shape_fits(t, samples, "clear attachment", why, why_size)) return -1;
      unsigned bind = i == draw->color_count ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET;
      if (!(t->res->bind & bind)) {
         say(why, why_size, "clear attachment lacks render-target binding");
         return -1;
      }
      if (fb.samples && fb.samples != samples) {
         say(why, why_size, "clear attachments have different sample counts");
         return -1;
      }
      fb.samples = samples;
      fb.width = fb.width ? MIN2(fb.width, t->shape.width) : t->shape.width;
      fb.height = fb.height ? MIN2(fb.height, t->shape.height) : t->shape.height;
   }
   for (unsigned i = 0; i <= draw->color_count; i++) {
      struct rwa_texture *t = i == draw->color_count ? draw->depth : draw->color[i];
      if (!t) continue;
      struct pipe_surface *surf = i == draw->color_count ? &fb.zsbuf : &fb.cbufs[i];
      pipe_surface_init(pipe, surf, t->res, t->first_level, 0);
      surf->format = i == draw->color_count ? t->res->format : texture_format(t);
   }
   state_framebuffer(device, &fb);
   clear_bound_attachments(pipe, draw);
   for (unsigned i = 0; i < draw->color_count; i++) {
      if (draw->color[i] && (mask & (1u << i))) texture_written(draw->color[i]);
      pipe_resource_reference(&fb.cbufs[i].texture, NULL);
   }
   if (draw->depth && ((draw->clear_depth && format_has_depth(draw->depth->res->format)) ||
                       (draw->clear_stencil && format_has_stencil(draw->depth->res->format))))
      texture_written(draw->depth);
   pipe_resource_reference(&fb.zsbuf.texture, NULL);
   return 0;
}
