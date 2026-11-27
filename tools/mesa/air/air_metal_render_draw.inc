/* Attachment validation, clear, and render draw submission, included into air_metal.c. */


/*
 * Whether one attachment of a pass has the shape the pipeline was compiled for, in the
 * two ways that are not its pixel format.
 *
 * Metal spells "more than one sample" twice -- in the texture's TYPE
 * (MTLTextureType2DMultisample) and in its sampleCount -- and requires the pipeline's
 * sampleCount to be the attachment's. Both are checked, in both directions: a
 * multisample texture in a one-sample pass and a plain 2D texture in a multisample pass
 * are each a descriptor that disagrees with itself, and neither can be honoured by
 * halves. Gallium gives no second opinion to fall back on here, because a framebuffer's
 * sample count IS the attachment's resource's (util_framebuffer_get_num_samples): a
 * mismatch would silently become whatever the first attachment happened to be.
 */
static bool
attachment_shape_fits(const struct rwa_texture *t, unsigned samples, const char *what,
                      char *why, size_t why_size)
{
   if (!texture_require_backing(t, what, why, why_size)) return false;
   unsigned want_type = samples > 1 ? RWA_TYPE_2D_MULTISAMPLE : RWA_TYPE_2D;
   if (t->shape.type != want_type) {
      say(why, why_size, "%s is a %s and a pass of %u sample%s per pixel renders into a %s",
          what, texture_type_name(t->shape.type), samples, samples == 1 ? "" : "s", texture_type_name(want_type));
      return false;
   }
   unsigned have = t->res->nr_samples > 1 ? t->res->nr_samples : 1;
   if (have != samples) {
      say(why, why_size, "%s has %u sample%s per pixel and the pipeline was made for %u",
          what, have, have == 1 ? "" : "s", samples);
      return false;
   }
   return true;
}

static uint64_t draw_phase_us[6];
static void *draw_phase_identity;
static unsigned draw_phase_cuts_now, draw_phase_cuts_prev;
/* Vertices and indices walked in this window: the corner rounding is built out of geometry, so a
 * per-draw cost that follows the vertex count is vertex work and one that does not is per-call
 * overhead. */
static uint64_t draw_phase_verts;
static void *draw_phase_flush;
static unsigned draw_phase_count;

/* Cleared if the clock turns out to cost enough that six extra reads a draw would be measuring
 * itself. The window boundaries are still read either way, so `total` and the draw and vertex
 * counts survive; only the split is lost. */
static int draw_phase_marks_on = 1;

static void
draw_phase_add(unsigned phase, uint64_t began)
{
   if (!draw_phase_marks_on)
      return;
   draw_phase_us[phase] += draw_mark() - began;
}

static void
draw_phase_done(uint64_t began)
{
   static uint64_t whole;

   whole += draw_mark() - began;
   if (++draw_phase_count == 256) {
      /* `draw` keeps its old meaning, the whole of the clear, the draw call and the resolve, so
       * the lines this replaces stay comparable; the three that follow split it. The difference
       * between `total` and the sum of the parts is the work this layer does around the driver's
       * calls, which is what tells a Metal-side cost from a Mesa-side one. */
      if (draw_phase_identity == NULL) {
         /* Which function the pipe actually dispatches to. The driver's own fd_draw_vbo and
          * fd_context_flush are timed inside the pipe library, and both disagree with the time
          * spent at the call site, so naming the targets settles whether the difference is in the
          * driver, in a queue in front of it, or in a wrapper nobody has timed. */
         void *draw_symbol = dlsym(RTLD_DEFAULT, "fd_perf_draw_vbo_symbol");
         void *flush_symbol = dlsym(RTLD_DEFAULT, "fd_perf_context_flush_symbol");
         uint64_t spin = 0, spin_was = 0;

         /* What one pair of clock reads costs, so a later reading of the three phases can be told
          * apart from the cost of the clock itself. */
         for (unsigned i = 0; i < 256; i++) {
            spin_was = draw_mark();
            spin += draw_mark() - spin_was;
         }
         printf("rwa draw_vbo: pipe %p %s the timed fd_draw_vbo (%p); flush %p %s the timed "
                "fd_context_flush (%p); clock pair adds %llu us over 256\n", draw_phase_identity,
                draw_phase_identity == draw_symbol ? "is" : "is not", draw_symbol,
                draw_phase_flush, draw_phase_flush == flush_symbol ? "is" : "is not",
                flush_symbol, (unsigned long long)spin);
         /* Two reads a draw is the floor; six more are what the split adds. Above a microsecond a
          * pair the marks would be the cost they are meant to measure, so they are dropped and the
          * window carries the answer instead. */
         if (spin / 256 > 1) {
            draw_phase_marks_on = 0;
            printf("rwa draw: phase marks off, the clock pair costs %llu us\n",
                   (unsigned long long)(spin / 256));
         }
      }
      {
         unsigned *cuts = dlsym(RTLD_DEFAULT, "fd_perf_batch_cuts");

         draw_phase_cuts_now = cuts ? *cuts : 0;
      }
      printf("rwa draw: 256 draws, checks %llu us, attachments+state %llu us, bindings "
             "%llu us, draw %llu us, total %llu us; clear %llu, draw_vbo %llu, resolve %llu, "
             "batch cuts %u, verts %llu\n",
              (unsigned long long)draw_phase_us[0], (unsigned long long)draw_phase_us[1],
              (unsigned long long)draw_phase_us[2],
              (unsigned long long)(draw_phase_us[3] + draw_phase_us[4] + draw_phase_us[5]),
              (unsigned long long)whole,
              (unsigned long long)draw_phase_us[3], (unsigned long long)draw_phase_us[4],
              (unsigned long long)draw_phase_us[5], draw_phase_cuts_now - draw_phase_cuts_prev,
              (unsigned long long)draw_phase_verts);
      draw_phase_cuts_prev = draw_phase_cuts_now;
      draw_phase_verts = 0;
      for (unsigned i = 0; i < 6; i++)
         draw_phase_us[i] = 0;
      whole = 0;
      draw_phase_count = 0;
      fflush(stdout);
   }
}

#include "air_metal_clear.inc"

int
rwa_render_draw(struct rwa_render *render, const struct rwa_draw *draw,
                char *why, size_t why_size)
{
   RWA_LOCKED;
   struct rwa_device *device = render->device;
   struct pipe_context *pipe = device->pipe;
   uint64_t began = draw_mark(), mark;
   /* Taken before anything else so that one draw longer than a frame can be reported with its
    * own split. The window below accumulates six phases over 256 draws and averages a second
    * away; a process's first draw carries the library's setup and is the case that matters. */
   uint64_t phase_before[6];

   for (unsigned i = 0; i < 6; i++)
      phase_before[i] = draw_phase_us[i];
   const char *fname = render->finfo.entry_point ? render->finfo.entry_point
                                                 : "the fragment shader";

   if (!draw) {
      say(why, why_size, "a draw with no description");
      return -1;
   }
   /* The attachments of the pass must be the attachments the pipeline was compiled for:
    * Metal requires the render pass descriptor's colour formats to match the pipeline
    * state's, and a fragment shader writing a colour a different format would read back
    * as the right bits the wrong way.
    */
   if (draw->color_count != render->shape.color_count) {
      say(why, why_size, "drawn into %u colour attachments and the pipeline was made for "
                         "%u", draw->color_count, render->shape.color_count);
      return -1;
   }
   /* And the depth attachment either is there or is not, on both sides. Metal requires
    * the pass's formats to be the pipeline's; a pass with a depth texture the pipeline
    * knows nothing of would be tested against state the pipeline was not compiled with,
    * and a pipeline made for depth drawn without one would quietly do no test -- which
    * is the failure that reads as "depth testing does not work here".
    */
   unsigned zs_format = render->shape.depth_format ? render->shape.depth_format
                                                   : render->shape.stencil_format;
   if (!!draw->depth != !!zs_format) {
      say(why, why_size, "%s and %s", draw->depth ? "the pass has a depth attachment"
                                                  : "the pass has no depth attachment",
          zs_format ? "the pipeline was made with one" : "the pipeline was made without one");
      return -1;
   }

   enum mesa_prim prim;
   if (!mesa_prim_from_metal(draw->primitive, &prim, why, why_size))
      return -1;
   if (draw->vertex_count == 0) {
      /* A draw of no vertices is the quad trap from the other side: it rasterises
       * nothing and leaves every attachment as it was, which reads like a shader that
       * stored nothing rather than like a draw that never ran.
       */
      say(why, why_size, "a draw of no vertices, which would leave every attachment as it "
                         "is");
      return -1;
   }

   /*
    * Indexed or not, and everything the index buffer has to satisfy before a draw is
    * made of it. rwa_draw's comment carries the reasons; this is the order they are
    * checked in, which is cheapest-and-most-wrong first: the type decides the size, the
    * size decides whether the offset converts to Gallium's element start at all, and
    * only then is it worth reading the indices.
    *
    * `index_size` 0 below means "not indexed" exactly as it does in pipe_draw_info, so
    * the draw at the bottom reads one field and not a flag plus a field.
    */
   unsigned index_size = 0, index_start = 0;
   unsigned min_index = draw->vertex_start;
   unsigned max_index = draw->vertex_start + draw->vertex_count - 1;
   if (!draw->index_buffer) {
      /* Metal's non-indexed forms have no baseVertex and no index type or offset to
       * give. A caller that filled one in asked for something -drawPrimitives: cannot
       * express -- or is a driver whose copy of this struct is a different shape, which
       * is the same sentence and the more likely one.
       */
      if (draw->base_vertex != 0 || draw->index_type != 0 ||
          draw->index_buffer_offset != 0) {
         say(why, why_size, "a draw with no index buffer, and a base vertex of %d, index "
                            "type %u and index buffer offset %zu: -drawPrimitives: has "
                            "none of the three", draw->base_vertex, draw->index_type,
             draw->index_buffer_offset);
         return -1;
      }
      if (render->generated_vertex && draw->vertex_start + draw->vertex_count > 4) {
         say(why, why_size, "vertices %u..%u of the generated full-screen quad, which has "
                            "four (0..3)", draw->vertex_start,
             draw->vertex_start + draw->vertex_count - 1);
         return -1;
      }
   } else {
      if (draw->vertex_start != 0) {
         say(why, why_size, "an indexed draw starting at vertex %u: Metal's indexed draws "
                            "have no vertexStart, and where the indices start is the index "
                            "buffer offset", draw->vertex_start);
         return -1;
      }
      if (!index_size_from_metal(draw->index_type, &index_size, why, why_size))
         return -1;
      /* Apple's own rule, in Apple's own words in MTLRenderCommandEncoder.h:
       * "indexBufferOffset must be a multiple of the index size". Gallium counts the
       * start in elements, so this division is the whole of the conversion and it has to
       * be exact.
       */
      if (draw->index_buffer_offset % index_size != 0) {
         say(why, why_size, "an index buffer offset of %zu bytes with %s, whose indices "
                            "are %u bytes each: Metal requires the offset to be a "
                            "multiple of the index size", draw->index_buffer_offset,
             index_type_name(draw->index_type), index_size);
         return -1;
      }
      size_t need = draw->index_buffer_offset + (size_t)draw->vertex_count * index_size;
      size_t have = rwa_buffer_size(draw->index_buffer);
      if (need > have) {
         /* Not a formality: past the end, softpipe reads index 0 instead ("if the index
          * buffer would overflow we return index 0", draw_private.h's DRAW_GET_IDX) and
          * a6xx clamps the draw against max_indices computed from the resource's width.
          * Both of those are a picture, and neither is the one that was asked for.
          */
         say(why, why_size, "%u indices of %u bytes at offset %zu want %zu bytes of an "
                            "index buffer that holds %zu", draw->vertex_count, index_size,
             draw->index_buffer_offset, need, have);
         return -1;
      }
      index_start = (unsigned)(draw->index_buffer_offset / index_size);
      index_range((const uint8_t *)rwa_buffer_map(draw->index_buffer) +
                  draw->index_buffer_offset, draw->vertex_count, index_size, &min_index,
                  &max_index);
      /* The generated vertex stage's own quad has four corners and this library owns the
       * buffer they are in, so an index past it is not the application's mistake to make
       * -- it is a fetch outside a resource of ours, and both back ends answer one with
       * a vertex rather than an error. This is the indexed form of the vertex_start
       * check above, with base_vertex where Metal puts it.
       */
      if (render->generated_vertex) {
         int64_t low = (int64_t)min_index + draw->base_vertex;
         int64_t high = (int64_t)max_index + draw->base_vertex;
         if (low < 0 || high > 3) {
            say(why, why_size, "indices %u..%u and a base vertex of %d, which is vertices "
                               "%lld..%lld of the generated full-screen quad, and it has "
                               "four (0..3)", min_index, max_index, draw->base_vertex,
                (long long)low, (long long)high);
            return -1;
         }
      }
   }
   /* Metal's instanceCount, with zero read as one: see rwa_draw. An instance count of
    * one is what every draw this library made before this field existed asked for, and
    * what Metal's non-instanced forms mean.
    */
   unsigned instances = draw->instance_count ? draw->instance_count : 1;

   /*
    * THE VERTEX STREAMS, checked here -- before a surface or a binding is made, so a
    * refusal costs nothing to undo -- and bound further down with the elements.
    *
    * This runs only for a pipeline whose vertex stage fetches attributes. Every other
    * draw binds the library's own quad at slot 0, exactly as it did before the layout
    * existed, and passes nothing here.
    */
   const struct rwa_vertex_state *vst = &render->vstate;
   int64_t low_vertex = (int64_t)min_index + (index_size ? draw->base_vertex : 0);
   int64_t high_vertex = (int64_t)max_index + (index_size ? draw->base_vertex : 0);
   for (unsigned k = 0; vst->count && k < RWA_MAX_VERTEX_BUFFERS; k++) {
      if (!(vst->stream_mask & (1u << k)))
         continue;
      const struct rwa_vertex_stream *s =
         k < draw->vertex_stream_count && draw->vertex_streams ? &draw->vertex_streams[k]
                                                               : NULL;
      if (!s || !s->buffer) {
         say(why, why_size, "the vertex descriptor fetches from buffer index %u and "
                            "nothing is bound there (the draw covers %u stream slot%s): "
                            "the fetch would read whatever that slot held", k,
             draw->vertex_stream_count, draw->vertex_stream_count == 1 ? "" : "s");
         return -1;
      }
      size_t have = rwa_buffer_size(s->buffer);
      if (s->offset > have) {
         say(why, why_size, "vertex stream %u is bound at offset %zu of a buffer that "
                            "holds %zu bytes", k, s->offset, have);
         return -1;
      }
      /* The element this draw's last vertex reads, against what is left of the buffer.
       * Only a PER-VERTEX stream is bounded this way: which element an instanced stream
       * reads is the hardware's arithmetic over the base instance and the divisor, and a
       * bound computed from the wrong formula would refuse legal draws (air_metal.h).
       */
      if (!(vst->per_vertex & (1u << k)))
         continue;
      int64_t first = (int64_t)s->offset + (int64_t)vst->stride[k] * low_vertex;
      if (first < 0) {
         say(why, why_size, "vertex %lld of stream %u is %lld bytes before the start of "
                            "its buffer: a base vertex of %d at a stride of %u",
             (long long)low_vertex, k, (long long)-first, draw->base_vertex,
             vst->stride[k]);
         return -1;
      }
      size_t need = (size_t)((int64_t)s->offset +
                             (int64_t)vst->stride[k] * high_vertex) + vst->bytes[k];
      if (need > have) {
         say(why, why_size, "vertex %lld of stream %u ends %zu bytes into a buffer that "
                            "holds %zu: %u attribute byte%s at a stride of %u from offset "
                            "%zu", (long long)high_vertex, k, need, have, vst->bytes[k],
             vst->bytes[k] == 1 ? "" : "s", vst->stride[k], s->offset);
         return -1;
      }
   }

   /* Each colour attachment: a 2D texture, made as a render target, of the format the
    * pipeline was compiled for. The pass draws into the area all attachments share, as Metal
    * does: CoreAnimation's blur draws one mip level of a texture as attachment 0 beside a
    * full-size attachment 1.
    */
   unsigned samples = render->shape.sample_count ? render->shape.sample_count : 1;
   unsigned w = 0, h = 0;
   bool have_size = false;
   for (unsigned i = 0; i < draw->color_count; i++) {
      struct rwa_texture *t = draw->color ? draw->color[i] : NULL;
      char what[48];
      if (render->shape.color_format[i] == 0 && !t)
         continue;
      if (!t) {
         say(why, why_size, "colour attachment %u is not a texture", i);
         return -1;
      }
      snprintf(what, sizeof(what), "colour attachment %u", i);
      if (!attachment_shape_fits(t, samples, what, why, why_size))
         return -1;
      if (!(t->res->bind & PIPE_BIND_RENDER_TARGET)) {
         say(why, why_size, "colour attachment %u (%s) was made without "
                            "MTLTextureUsageRenderTarget", i, t->format_name);
         return -1;
      }
      if (rgb2p_format(t->shape.pixel_format) && !t->rgb2p_pass_open) {
         say(why, why_size, "RGB2P draw requires a begun pass");
         return -1;
      }
      if (t->shape.pixel_format != render->shape.color_format[i]) {
         say(why, why_size, "colour attachment %u is pixel format %u and the pipeline's "
                            "attachment %u is %u", i, t->shape.pixel_format, i,
             render->shape.color_format[i]);
         return -1;
      }
      unsigned tw = t->shape.width;
      unsigned th = t->shape.height ? t->shape.height : 1;
      w = !have_size ? tw : MIN2(w, tw);
      h = !have_size ? th : MIN2(h, th);
      have_size = true;
   }

   /* The depth/stencil attachment, by the same checks and one more: it is the only
    * attachment whose format decides which Gallium binding it needed, so a texture made
    * without MTLTextureUsageRenderTarget has no PIPE_BIND_DEPTH_STENCIL and could not be
    * a zsbuf at all.
    */
   if (draw->depth) {
      struct rwa_texture *t = draw->depth;
      if (!attachment_shape_fits(t, samples, "the depth attachment", why, why_size))
         return -1;
      if (!format_is_depth_stencil(t->res->format)) {
         say(why, why_size, "the depth attachment is %s, which is a colour format",
             t->format_name);
         return -1;
      }
      if (t->shape.pixel_format != zs_format) {
         say(why, why_size, "the depth attachment is pixel format %u and the pipeline's is "
                            "%u", t->shape.pixel_format, zs_format);
         return -1;
      }
      if (!(t->res->bind & PIPE_BIND_DEPTH_STENCIL)) {
         say(why, why_size, "the depth attachment (%s) was made without "
                            "MTLTextureUsageRenderTarget, so the GPU was never told it "
                            "could be drawn into", t->format_name);
         return -1;
      }
      unsigned tw = t->shape.width;
      unsigned th = t->shape.height ? t->shape.height : 1;
      w = !have_size ? tw : MIN2(w, tw);   /* a depth-only pass: its size */
      h = !have_size ? th : MIN2(h, th);
      have_size = true;
   }


   /* Everything each stage declares has to be bound, the same check a dispatch makes. */
   if (check_bindings(&render->finfo, render->frag_slots, &draw->fragment,
                      draw->fragment_buffer_table, draw->fragment_buffer_table_count, fname, why,
                      why_size) != 0)
      return -1;
   const char *vname = NULL;
   if (!render->generated_vertex) {
      vname = render->vinfo.entry_point ? render->vinfo.entry_point : "the vertex shader";
      if (check_bindings(&render->vinfo, render->vert_slots, &draw->vertex,
                         draw->vertex_buffer_table, draw->vertex_buffer_table_count, vname, why,
                         why_size) != 0)
         return -1;
   }

   struct rwa_yuv_key fyuv, vyuv = { 0 };
   if (!yuv_key_for(&render->finfo, &draw->fragment, draw->fragment_buffer_table,
                     draw->fragment_buffer_table_count, &fyuv, why, why_size) ||
       (!render->generated_vertex &&
        !yuv_key_for(&render->vinfo, &draw->vertex, draw->vertex_buffer_table,
                      draw->vertex_buffer_table_count, &vyuv, why, why_size)))
      return -1;
   if (device->software && fyuv.mask) {
      say(why, why_size, "combined NV12 requires separate texture/sampler indices unavailable in softpipe");
      return -1;
   }
   if (vyuv.mask) {
      say(why, why_size, "combined NV12 sampling in a vertex stage is unsupported");
      return -1;
   }
   void *fragment_cso = render->fs_cso;
   if (fyuv.mask || fyuv.rgb2p) {
      if (!render->yuv_fs_cso || memcmp(&fyuv, &render->yuv, sizeof(fyuv)) != 0) {
         void *variant = make_stage_cso(device, render->fragment_nir, &render->finfo,
                                         &fyuv, why, why_size);
         if (!variant)
            return -1;
         if (render->yuv_fs_cso) {
            state_forget(device);
            pipe->bind_fs_state(pipe, NULL);
            pipe->delete_fs_state(pipe, render->yuv_fs_cso);
         }
         render->yuv_fs_cso = variant;
         render->yuv = fyuv;
      }
      fragment_cso = render->yuv_fs_cso;
   }

   /* The framebuffer, from the caller's own textures. A surface is plain value data
    * in 26.x -- the framebuffer state embeds the structs instead of pointing at ones
    * the driver allocated -- so filling one in cannot fail and the only cost is the
    * texture reference each takes, released again below before the caller may destroy
    * the textures under it.
    */
   if (draw_phase_identity == NULL) {
      draw_phase_identity = (void *)pipe->draw_vbo;
      draw_phase_flush = (void *)pipe->flush;
   }
   draw_phase_add(0, began);
   mark = draw_mark();
   if (!rgb2p_prepare_bindings(&render->finfo, &draw->fragment, draw->fragment_buffer_table,
                               draw->fragment_buffer_table_count, fname, why, why_size) ||
       (!render->generated_vertex &&
        !rgb2p_prepare_bindings(&render->vinfo, &draw->vertex, draw->vertex_buffer_table,
                                draw->vertex_buffer_table_count, vname, why, why_size)))
      return -1;
   struct pipe_surface surf[PIPE_MAX_COLOR_BUFS] = { 0 };
   struct pipe_surface zsurf = { 0 };
   /* pipe_framebuffer_state.samples is filled in although nothing here reads it: both
    * drivers take the pass's sample count off the attachments instead
    * (util_framebuffer_get_num_samples ignores this field whenever there is one), and a
    * field left at zero that happens to mean "one sample" is a thing to be found out by
    * the next driver rather than by this comment.
    */
   struct pipe_framebuffer_state fb = { .width = w, .height = h,
                                        .nr_cbufs = draw->color_count,
                                        .samples = samples };
   for (unsigned i = 0; i < draw->color_count; i++) {
      if (!draw->color[i])
         continue;
      pipe_surface_init(pipe, &surf[i], draw->color[i]->res, draw->color[i]->first_level, 0);
      texture_written(draw->color[i]);
      surf[i].format = texture_format(draw->color[i]);
      fb.cbufs[i] = surf[i];
   }
   if (draw->depth) {
      pipe_surface_init(pipe, &zsurf, draw->depth->res, draw->depth->first_level, 0);
      texture_written(draw->depth);
      zsurf.format = draw->depth->res->format;
      fb.zsbuf = zsurf;
   }
   state_framebuffer(device, &fb);
   /* rwa_draw.discard: preserve rsc->valid; marking undefined drops GMEM tile resolve
    * when writeMask is 0 and corrupts multi-pass offscreen widget rendering. */
   /* The viewport, and now its z as well. Gallium's window depth is
    * clip_z/w * scale[2] + translate[2], and with clip_halfz (the rasterizer state
    * above) the depth range [znear, zfar] is scale = zfar - znear, translate = znear --
    * which is what util_viewport_zmin_zmax reads back out of it (util/u_viewport.h, and
    * freedreno hands exactly that to GRAS_CL_Z_CLAMP). The default is { 0, 1 }, the
    * identity, which is the 1.0/0.0 every draw before depth existed used.
    */
   float znear = draw->depth_range ? draw->depth_range[0] : 0.0f;
   float zfar = draw->depth_range ? draw->depth_range[1] : 1.0f;
   /* Preserve the viewport transform outside the attachment. The framebuffer and scissor
    * clip rasterization; clipping this rectangle would rescale the vertex positions. */
   float vx = 0.0f, vy = 0.0f, vw = (float)w, vh = (float)h;
   if (draw->viewport != NULL) {
      vx = draw->viewport[0];
      vy = draw->viewport[1];
      vw = draw->viewport[2];
      vh = draw->viewport[3];
   }
   state_viewport(device, &(struct pipe_viewport_state){
      /* The y scale is NEGATIVE, and that is Metal's rule rather than a convention picked
       * here. Metal's render-target origin is the top left and its NDC y points up, so clip
       * y = +1 belongs in memory row 0; a positive y scale puts clip y = -1 there, which is
       * OpenGL's drawable rule and what this code did until now (docs/metal.md 10.28). Mesa
       * negates y in exactly this place, for exactly this reason, when it draws to a window.
       *
       * It is landed only now because until the varying was fixed (10.32) nothing could see
       * it: every draw of the corpus either writes a constant or samples one fixed texel, and
       * over a constant an orientation is invisible. The sixteen-pixel grid of 10.32 is what
       * it is landed against -- it must turn from (100+x, 200+y, ...) into
       * (100+x, 200+(3-y), ...), the texture mirrored vertically, on both back ends.
       *
       * The rectangle goes in the same two numbers: with the y scale negative, its translate
       * is the row of clip y = +1, which is the rectangle's TOP edge plus half its height. */
      .scale = { vw / 2.0f, -(vh / 2.0f), zfar - znear },
      .translate = { vx + vw / 2.0f, vy + vh / 2.0f, znear } });

   /* Metal keeps the reference value on the encoder and not in the state object, so it
    * is set per draw. Gallium's is one byte per face, which is the stencil buffer's
    * width; Metal's is a uint32 and only those bits can match anything.
    */
   state_stencil_ref(device, (struct pipe_stencil_ref){
      .ref_value = { (uint8_t)(draw->stencil_reference[0] & 0xff),
                     (uint8_t)(draw->stencil_reference[1] & 0xff) } });
   /* The clip rectangle, in the same pixels and from the same corner as Metal's: Gallium's
    * scissor is minx/miny/maxx/maxy from the attachment's top left, with maxx and maxy
    * exclusive. Scissor enable is part of the rasterizer state, so it is turned on with the
    * rectangle and off without one -- the render's own rasterizer state does not clip.
    */
   if (draw->scissor != NULL) {
      state_scissor(device, &(struct pipe_scissor_state){
         .minx = (unsigned)CLAMP((float)draw->scissor[0], 0.0f, (float)w),
         .miny = (unsigned)CLAMP((float)draw->scissor[1], 0.0f, (float)h),
         .maxx = (unsigned)CLAMP((float)(draw->scissor[0] + draw->scissor[2]), 0.0f, (float)w),
         .maxy = (unsigned)CLAMP((float)(draw->scissor[1] + draw->scissor[3]), 0.0f, (float)h) });
   }
   /* The caller's depth/stencil state, or the pipeline's zeroed one, which is Metal's
    * behaviour with no MTLDepthStencilState set: no test and no write. A pass without a
    * depth/stencil attachment also takes the zeroed one: Metal skips both tests there,
    * while freedreno's batch_draw_tracking reads pfb->zsbuf.texture whenever the bound
    * state enables either test (MapKit's snapshot service crashed on that NULL).
    */
   state_objects(device, render->blend,
                 draw->depth_stencil && draw->depth ? draw->depth_stencil->cso : render->dsa,
                 draw->scissor != NULL ? render->rast_scissor : render->rast,
                 render->vs_cso, fragment_cso);

   /*
    * The vertex state, bound on BOTH paths and never NULL.
    *
    * It used to be bound only for the generated stage, on the reasoning that a supplied
    * vertex shader reads no attributes and so needs no vertex buffer. That reasoning is
    * sound about the shader and wrong about the driver: freedreno reads the bound
    * vertex-element state with no NULL check at all. fd6_emit_3d_state's
    * FD6_GROUP_VTXSTATE case is
    *
    *     state = fd6_vertex_stateobj(ctx->vtx.vtx)->stateobj;
    *
    * and `stateobj` is at offset 0x208 of that struct (pipe[32] is 0x180, strides[32]
    * takes it to 0x200, num_elements to 0x204, and the pointer aligns to 0x208). So
    * clearing it faulted at exactly 0x208 on the Adreno, for every draw with a supplied
    * vertex shader, while the generated stage drew correctly -- and softpipe tolerates a
    * NULL there, so nothing on this machine could have caught it.
    *
    * Binding the same state on both paths also means the shader module is the only thing
    * that differs between a draw already proven on the device and a new one. An
    * attribute the vertex shader never declares is never fetched into a register -- ir3
    * emits fetches for the inputs the shader has -- which is the ordinary case of a GL
    * program with more arrays enabled than it reads.
    *
    * A reference is taken for the driver, not handed to it out of ours.
    *
    * set_vertex_buffers TAKES OWNERSHIP of the reference in the pipe_vertex_buffer:
    * both drivers here pass take_ownership=true to util_set_vertex_buffers_* (softpipe
    * in sp_state_vertex.c, freedreno in freedreno_state.c), so a caller that passes
    * its own only reference has given it away and must not release it again. This did
    * release it again -- the pipeline's destroy unreferenced render->vbuf -- which
    * freed the quad out from under a context still holding it, and softpipe's own
    * teardown then decremented a dead reference. It aborted on
    * `pipe_reference_described: count != -1` in 53 of the corpus's 255 drawn fragment
    * shaders and not in the other 202, which is what a use-after-free looks like when
    * the assert only fires if the freed slab happens to hold a zero at that offset.
    */
   /*
    * The streams, when the stage fetches attributes: one pipe_vertex_buffer per slot
    * from 0 to the highest index any element names, because set_vertex_buffers takes a
    * COUNT from slot 0 where Metal's -setVertexBuffer:atIndex: takes an index. A slot no
    * element names is left empty on purpose -- a descriptor that uses index 5 alone is
    * an array of six with five empty entries -- and the slot one past Metal's highest is
    * where the placeholder for a gated, unconfigured attribute reads this library's own
    * quad.
    */
   struct pipe_vertex_buffer vbs[PIPE_MAX_ATTRIBS] = { 0 };
   unsigned nvb = 1;
   if (vst->count) {
      nvb = MIN2(vst->stream_count, (unsigned)PIPE_MAX_ATTRIBS);
      for (unsigned k = 0; k < nvb; k++) {
         if (k == RWA_MAX_VERTEX_BUFFERS) {
            pipe_resource_reference(&vbs[k].buffer.resource, render->vbuf);
            continue;
         }
         if (!(vst->stream_mask & (1u << k)))
            continue;
         const struct rwa_vertex_stream *s = &draw->vertex_streams[k];
         vbs[k].buffer_offset = (unsigned)s->offset;
         pipe_resource_reference(&vbs[k].buffer.resource, s->buffer->res);
      }
   } else {
      pipe_resource_reference(&vbs[0].buffer.resource, render->vbuf);
   }
   /* Elements first, buffers second, which is the order p_context.h requires of a
    * frontend: "Frontends MUST call set_vertex_buffers after bind_vertex_elements_state
    * and before the next draw", because a driver is allowed to merge the two in
    * set_vertex_buffers rather than in draw_vbo. Neither driver here does, so the
    * other order worked; it worked by permission and not by contract.
    */
   state_vertex_elements(device, render->velem);
   pipe->set_vertex_buffers(pipe, nvb, vbs);
   draw_phase_add(1, mark);
   mark = draw_mark();

   struct rwa_bound fbound = { 0 }, vbound = { 0 };
   int rc = -1;
   if (bind_stage(device, MESA_SHADER_FRAGMENT, &render->finfo, render->frag_slots, render->frag_own_sampler,
                  render->frag_compare_sampler, &fyuv,
                  &draw->fragment, draw->fragment_buffer_table, draw->fragment_buffer_table_count,
                  fname, &fbound, why, why_size) != 0)
      goto unbind;
   if (!render->generated_vertex &&
       bind_stage(device, MESA_SHADER_VERTEX, &render->vinfo, render->vert_slots, render->vert_own_sampler,
                  render->vert_compare_sampler, &vyuv,
                  &draw->vertex, draw->vertex_buffer_table, draw->vertex_buffer_table_count, vname,
                  &vbound, why, why_size) != 0)
      goto unbind;
   draw_phase_add(2, mark);
   mark = draw_mark();
   clear_bound_attachments(pipe, draw);
   draw_phase_add(3, mark);
   mark = draw_mark();
   {
      /*
       * The one call both forms go through, which is Gallium's shape and not two of
       * ours: `index_size` 0 is a non-indexed draw there as it is here, `start` is in
       * elements of whatever is being walked (vertices, or indices from the index
       * buffer's base), and `index_bias` is the base vertex. Nothing below is
       * conditional except what the index buffer itself provides.
       *
       * primitive_restart is on for every indexed draw, with the largest value of the index
       * type: Metal has no switch for it, and its -drawIndexedPrimitives: documentation says
       * 0xFFFF (MTLIndexTypeUInt16) or 0xFFFFFFFF (UInt32) ends the current primitive and
       * starts a new one, for every primitive type.
       *
       * take_index_buffer_ownership stays off as well: the reference belongs to the
       * caller's rwa_buffer, which outlives this draw and may be drawn with again.
       */
      struct pipe_draw_info info = {
         .mode = prim, .index_bounds_valid = true,
         .index_size = (uint16_t)index_size,
         .primitive_restart = index_size != 0,
         .restart_index = index_size == 2 ? 0xffffu : 0xffffffffu,
         .min_index = min_index, .max_index = max_index,
         .instance_count = instances,
         .start_instance = draw->base_instance,
      };
      if (index_size)
         info.index.resource = draw->index_buffer->res;
      struct pipe_draw_start_count_bias sc = {
         .start = index_size ? index_start : draw->vertex_start,
         .count = draw->vertex_count,
         .index_bias = index_size ? draw->base_vertex : 0,
      };
      draw_phase_verts += sc.count;
      /* Which function the vtable entry points at, on the calls that cost something: a draw
       * reported as 1,374,219 us here while the driver's own timing of fd6_draw_vbos covered at
       * most 26 us means either that this call does not reach fd6_draw_vbos at all, or that the
       * time sits in fd_draw_vbo outside that function. The address decides which. */
      {
         uint64_t call_began = draw_mark(), call_took;
         pipe->draw_vbo(pipe, &info, 0, NULL, &sc, 1);
         call_took = draw_mark() - call_began;
         if (call_took > 20000) {
            Dl_info where = { 0 };
            const char *what = dladdr((void *)pipe->draw_vbo, &where) != 0 && where.dli_sname != NULL
                               ? where.dli_sname : "?";
            printf("rwa draw call: pipe %p draw_vbo %p wall %llu us (%s)\n", (void *)pipe,
                   (void *)pipe->draw_vbo, (unsigned long long)call_took, what);
            fflush(stdout);
         }
      }
   }
   draw_phase_add(4, mark);
   mark = draw_mark();

   /* Metal's multisample resolve, which has no call of its own here: a Metal pass is a
    * run of draws carrying the same attachments, and the resolved result is written when
    * the encoder ends. With no end-of-pass entry point it is done at the end of every
    * draw that names a resolve target -- the multisample attachment keeps its contents,
    * so the last resolve of a pass leaves what the pass would have written.
    *
    * A blit rather than a second render pass: the driver's own blit resolves between
    * sample counts (util_blitter's resolve path), and a pass would need a framebuffer, a
    * viewport and a shader to do what one copy does.
    */
   if (draw->resolve) {
      for (unsigned i = 0; i < draw->color_count; i++) {
         struct rwa_texture *into = draw->resolve[i];

         if (!into || !draw->color[i])
            continue;
         struct pipe_resource *from = draw->color[i]->res;
         /* CoreAnimation names a resolve texture on passes whose attachment is NOT
          * multisampled -- measured: 354 times in one 420 s run, all of them with
          * storeAction 2 on a one-sample target. The driver draws those straight into
          * the resolve texture and does not pass it here; the rest (a pass that loads,
          * storeAction 4, another pixel format) is skipped rather than refused. */
         if (from->nr_samples <= 1)
            continue;
         if (into->first_level != 0) {
            say(why, why_size, "the resolve texture of colour attachment %u is a view from "
                               "level %u, and a resolve here writes level 0", i,
                into->first_level);
            rc = -1;
            goto unbind;
         }
         if (into->res->nr_samples > 1 || into->res->width0 != from->width0 ||
             into->res->height0 != from->height0 ||
             into->res->format != from->format) {
            say(why, why_size, "the resolve texture of colour attachment %u is %ux%u of "
                               "format %u with %u samples, and the attachment is %ux%u "
                               "of format %u with %u", i, into->res->width0,
                into->res->height0, into->res->format, into->res->nr_samples,
                from->width0, from->height0, from->format, from->nr_samples);
            rc = -1;
            goto unbind;
         }
         texture_written(into);
         pipe->blit(pipe, &(struct pipe_blit_info){
            .src = { .resource = from, .level = 0, .format = from->format,
                     .box = { .x = 0, .y = 0, .z = 0, .width = (int)from->width0,
                              .height = (int)from->height0, .depth = 1 } },
            .dst = { .resource = into->res, .level = 0, .format = into->res->format,
                     .box = { .x = 0, .y = 0, .z = 0, .width = (int)into->res->width0,
                              .height = (int)into->res->height0, .depth = 1 } },
            .mask = PIPE_MASK_RGBA,
            .filter = PIPE_TEX_FILTER_NEAREST,
         });
      }
   }
   draw_phase_add(5, mark);

   /* NOT waited for here.
    *
    * It used to be, on the grounds the dispatch still gives: a picture that was never
    * drawn must not read as a result. What that cost was measured on the device: the
    * compositor draws ~226 times per frame while the Control Center animates, and a
    * flush and a fence per draw made each of those a GPU round trip, 1.5 ms of the
    * processor's time for 0.4 ms of the GPU's -- 300 ms for a frame that should take
    * six. Metal's own contract has nothing to wait for at this point: a command
    * buffer's work is handed over by -commit and fenced by -waitUntilCompleted, and a
    * readback through rwa_texture_read or a mapped buffer is synchronized by the
    * driver's own map.
    *
    * So the draws accumulate in the batch and -commit flushes it once through
    * rwa_flush: one submission per command buffer instead of one per draw.
    */
   rc = 0;

unbind:
   /* The framebuffer, views and samplers stay bound for the next draw (air_metal_state.inc,
    * which holds references of its own); these are this call's references.
    */
   for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; i++)
      pipe_resource_reference(&surf[i].texture, NULL);
   pipe_resource_reference(&zsurf.texture, NULL);
   /* One draw that cost more than an eighth of a second, split the same way the window is.
    * `mark` is not used here: the whole draw is what decides whether the split is worth a line,
    * and the six phases are the difference of the accumulators across this call. */
   if (draw_mark() - began > 125000) {
      printf("rwa draw slow: %llu us in all; checks %llu, attachments+state %llu, bindings "
             "%llu, clear %llu, draw_vbo %llu, resolve %llu, %u verts%s\n",
             (unsigned long long)(draw_mark() - began),
             (unsigned long long)(draw_phase_us[0] - phase_before[0]),
             (unsigned long long)(draw_phase_us[1] - phase_before[1]),
             (unsigned long long)(draw_phase_us[2] - phase_before[2]),
             (unsigned long long)(draw_phase_us[3] - phase_before[3]),
             (unsigned long long)(draw_phase_us[4] - phase_before[4]),
             (unsigned long long)(draw_phase_us[5] - phase_before[5]),
             draw->vertex_count, draw_phase_marks_on ? "" : " (split off)");
      fflush(stdout);
   }
   draw_phase_done(began);
   return rc;
}

