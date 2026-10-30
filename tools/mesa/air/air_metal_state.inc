/*
 * What the draws leave bound in the Gallium context, kept on the device (struct rwa_bound_state)
 * so that a draw sets only what differs from the draw before it. Included into air_metal.c ahead
 * of the paths that bind, unbind and delete state.
 *
 * freedreno takes every set and bind as a change. fd_set_framebuffer_state, with reorder on,
 * drops the context's batch and marks every state group dirty whenever the framebuffer differs,
 * and each bind marks its own group dirty whatever was bound before. A draw that reset the
 * framebuffer and unbound its textures afterwards therefore made the next draw look its batch up
 * again, re-emit every group and re-track every bound resource
 * (batch_draw_tracking_for_dirty_bits): "tracking", "prog" and "emit" in the perf driver lines,
 * most of a draw's processor time.
 *
 * What stays bound, and what keeps it valid:
 * - the framebuffer, with a reference of its own on each attachment, until a draw into other
 *   attachments, the destruction of one of its textures or the device's close;
 * - the fragment and vertex sampler views, which the context references (fd_set_sampler_views):
 *   a texture destroyed while its view is bound releases the view (rwa_texture_destroy) and the
 *   context's reference keeps it alive until it is unbound;
 * - the fragment and vertex samplers, which the context holds by pointer only: state_forget
 *   unbinds them, and every path that deletes a sampler state calls it first;
 * - the pipeline's state objects, which rwa_render_destroy and rwa_depth_stencil_destroy unbind
 *   before deleting them; state_forget then makes the next draw bind all of them again.
 * The compute stage keeps binding and unbinding per dispatch (bind_stage, unbind_stage).
 */
#include "util/u_framebuffer.h"

#define STATE_UNKNOWN ((void *)~(uintptr_t)0)

/* How often each kind of state had to be set, per 4096 framebuffer requests (one per draw): a
 * "rwa state" line in the perf log. A cache that misses shows as sets close to the draws. */
static struct {
   unsigned draws, fb, viewport, objects, views, samplers;
} state_count;

static void
state_count_draw(void)
{
   if (++state_count.draws < 4096)
      return;
   printf("rwa state: %u draws: framebuffer set %u, viewport %u, state objects %u, views %u, "
          "samplers %u\n", state_count.draws, state_count.fb, state_count.viewport,
          state_count.objects, state_count.views, state_count.samplers);
   memset(&state_count, 0, sizeof(state_count));
}

static unsigned
state_stage(mesa_shader_stage stage)
{
   return stage == MESA_SHADER_VERTEX ? 1 : 0;
}

/* The next draw binds every state object again, and no sampler is left bound. */
static void
state_forget(struct rwa_device *device)
{
   struct rwa_bound_state *b = &device->bound;

   b->blend = b->dsa = b->rast = b->vs = b->fs = b->velem = STATE_UNKNOWN;
   b->viewport_set = b->stencil_set = b->scissor_set = false;
   for (unsigned s = 0; s < 2; s++) {
      if (b->num_samplers[s]) {
         void *none[RWA_MAX_TEXTURES] = { 0 };

         device->pipe->bind_sampler_states(device->pipe,
                                           s ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT, 0,
                                           b->num_samplers[s], none);
         b->num_samplers[s] = 0;
      }
   }
}

static void
state_framebuffer(struct rwa_device *device, const struct pipe_framebuffer_state *fb)
{
   struct rwa_bound_state *b = &device->bound;

   state_count_draw();
   if (b->fb_set && util_framebuffer_state_equal(&b->fb, fb))
      return;
   state_count.fb++;
   device->pipe->set_framebuffer_state(device->pipe, fb);
   util_copy_framebuffer_state(&b->fb, fb);
   b->fb_set = true;
}

static void
state_release_framebuffer(struct rwa_device *device)
{
   struct rwa_bound_state *b = &device->bound;

   if (!b->fb_set)
      return;
   device->pipe->set_framebuffer_state(device->pipe, &(struct pipe_framebuffer_state){ 0 });
   util_unreference_framebuffer_state(&b->fb);
   b->fb_set = false;
}

/* A texture going away: the framebuffer lets go of it now rather than at the next draw into
 * other attachments, which may be a while for the last pass of a frame. */
static void
state_texture_gone(struct rwa_device *device, const struct pipe_resource *res)
{
   struct rwa_bound_state *b = &device->bound;

   if (!b->fb_set || !res)
      return;
   for (unsigned i = 0; i < b->fb.nr_cbufs; i++)
      if (b->fb.cbufs[i].texture == res) {
         state_release_framebuffer(device);
         return;
      }
   if (b->fb.zsbuf.texture == res)
      state_release_framebuffer(device);
}

static void
state_viewport(struct rwa_device *device, const struct pipe_viewport_state *vp)
{
   struct rwa_bound_state *b = &device->bound;

   if (b->viewport_set && memcmp(b->viewport.scale, vp->scale, sizeof(vp->scale)) == 0 &&
       memcmp(b->viewport.translate, vp->translate, sizeof(vp->translate)) == 0)
      return;
   state_count.viewport++;
   device->pipe->set_viewport_states(device->pipe, 0, 1, vp);
   b->viewport = *vp;
   b->viewport_set = true;
}

static void
state_stencil_ref(struct rwa_device *device, struct pipe_stencil_ref ref)
{
   struct rwa_bound_state *b = &device->bound;

   if (b->stencil_set && b->stencil.ref_value[0] == ref.ref_value[0] &&
       b->stencil.ref_value[1] == ref.ref_value[1])
      return;
   device->pipe->set_stencil_ref(device->pipe, ref);
   b->stencil = ref;
   b->stencil_set = true;
}

static void
state_scissor(struct rwa_device *device, const struct pipe_scissor_state *sc)
{
   struct rwa_bound_state *b = &device->bound;

   if (b->scissor_set && b->scissor.minx == sc->minx && b->scissor.miny == sc->miny &&
       b->scissor.maxx == sc->maxx && b->scissor.maxy == sc->maxy)
      return;
   device->pipe->set_scissor_states(device->pipe, 0, 1, sc);
   b->scissor = *sc;
   b->scissor_set = true;
}

/* The pipeline's state objects: each bound only when it is not the one bound already. */
static void
state_objects(struct rwa_device *device, void *blend, void *dsa, void *rast, void *vs, void *fs)
{
   struct pipe_context *pipe = device->pipe;
   struct rwa_bound_state *b = &device->bound;

   state_count.objects += (b->blend != blend) + (b->dsa != dsa) + (b->rast != rast) + (b->vs != vs) +
                          (b->fs != fs);
   if (b->blend != blend)
      pipe->bind_blend_state(pipe, b->blend = blend);
   if (b->dsa != dsa)
      pipe->bind_depth_stencil_alpha_state(pipe, b->dsa = dsa);
   if (b->rast != rast)
      pipe->bind_rasterizer_state(pipe, b->rast = rast);
   if (b->vs != vs)
      pipe->bind_vs_state(pipe, b->vs = vs);
   if (b->fs != fs)
      pipe->bind_fs_state(pipe, b->fs = fs);
}

static void
state_vertex_elements(struct rwa_device *device, void *velem)
{
   if (device->bound.velem != velem)
      device->pipe->bind_vertex_elements_state(device->pipe, device->bound.velem = velem);
}

/* The views of a fragment or vertex draw, from slot 0; slots past `count` that the draw before
 * left bound are unbound with them. */
static void
state_views(struct rwa_device *device, mesa_shader_stage stage, unsigned count,
            struct pipe_sampler_view **views)
{
   struct rwa_bound_state *b = &device->bound;
   unsigned s = state_stage(stage), before = b->num_views[s];

   if (count == before && memcmp(b->views[s], views, count * sizeof(*views)) == 0)
      return;
   state_count.views++;
   device->pipe->set_sampler_views(device->pipe, stage, 0, count,
                                   before > count ? before - count : 0, views);
   memcpy(b->views[s], views, count * sizeof(*views));
   b->num_views[s] = count;
}

static void
state_samplers(struct rwa_device *device, mesa_shader_stage stage, unsigned count,
               void **samplers)
{
   struct rwa_bound_state *b = &device->bound;
   unsigned s = state_stage(stage), before = b->num_samplers[s];
   void *all[RWA_MAX_TEXTURES] = { 0 };

   if (count == before && memcmp(b->samplers[s], samplers, count * sizeof(*samplers)) == 0)
      return;
   state_count.samplers++;
   memcpy(all, samplers, count * sizeof(*samplers));
   device->pipe->bind_sampler_states(device->pipe, stage, 0, MAX2(count, before), all);
   memcpy(b->samplers[s], all, count * sizeof(*samplers));
   b->num_samplers[s] = count;
}

/* Before the context goes: nothing of the draws' stays referenced. */
static void
state_release(struct rwa_device *device)
{
   struct rwa_bound_state *b = &device->bound;

   state_forget(device);
   state_release_framebuffer(device);
   for (unsigned s = 0; s < 2; s++) {
      if (b->num_views[s])
         device->pipe->set_sampler_views(device->pipe,
                                         s ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT, 0, 0,
                                         b->num_views[s], NULL);
      b->num_views[s] = 0;
   }
}
