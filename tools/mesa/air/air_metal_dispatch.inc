/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

/* The dispatch entry points, included by air_metal.c after bind_stage and finish_or_say_why. */

int
rwa_dispatch(struct rwa_compute *compute, const struct rwa_bindings *bindings,
             const unsigned groups[3], const unsigned group_size[3],
             char *why, size_t why_size)
{
   RWA_LOCKED;
   /* No argument buffers, which is what every caller written before they existed means.
    * A kernel that declares one is refused by argbuf_member, naming the Metal buffer
    * index it wanted a set at and that nothing was given.
    */
   return rwa_dispatch_buffers(compute, bindings, NULL, 0, groups, group_size, why,
                               why_size);
}

int
rwa_dispatch_buffers(struct rwa_compute *compute, const struct rwa_bindings *bindings,
                     const struct rwa_buffer_bind *table, unsigned table_count,
                     const unsigned groups[3], const unsigned group_size[3],
                     char *why, size_t why_size)
{
   return rwa_dispatch_threadgroup(compute, bindings, table, table_count, groups,
                                   group_size, 0, why, why_size);
}

static int
dispatch_grid(struct rwa_compute *compute, const struct rwa_bindings *bindings,
              const struct rwa_buffer_bind *table, unsigned table_count,
              const unsigned groups[3], const unsigned group_size[3],
              unsigned threadgroup_bytes, const unsigned ib_dim[2],
              const struct rwa_grid_split *split, bool wait, char *why, size_t why_size)
{
   RWA_LOCKED;
   struct rwa_device *device = compute->device;
   rwa_deferred_drain_locked(device);
   struct pipe_context *pipe = device->pipe;
   const char *name = compute->info.entry_point ? compute->info.entry_point : "the shader";

   /* A caller with nothing to bind may say so with NULL rather than with a zeroed
    * struct. A shader that declares anything is then refused by the counts below,
    * which is the same answer in the same words.
    */
   static const struct rwa_bindings nothing = { 0 };
   if (!bindings)
      bindings = &nothing;

   for (unsigned i = 0; i < 3; i++) {
      if (groups[i] == 0 || group_size[i] == 0) {
         /* Zero threads is the trap air_run walked into from the other side: a grid
          * that runs nothing leaves every buffer exactly as it was, which reads like a
          * kernel that computed nothing rather than like a dispatch that never ran.
          */
         say(why, why_size, "a grid of %ux%ux%u threadgroups of %ux%ux%u threads has no "
                            "threads in it, so it would leave every buffer as it is",
             groups[0], groups[1], groups[2],
             group_size[0], group_size[1], group_size[2]);
         return -1;
      }
   }

   /* (the hardware's own limit here; what the compiled shader takes is checked once the
    * state for this threadgroup is made, below) */
   unsigned threads = group_size[0] * group_size[1] * group_size[2];
   unsigned limit = device->software ? compute->caps.max_threads_per_group
                                     : device->max_threads_per_group;
   if (limit && threads > limit) {
      say(why, why_size, "%s: %ux%ux%u is %u threads in one threadgroup and this GPU takes "
                         "at most %u", name, group_size[0], group_size[1], group_size[2],
          threads, limit);
      return -1;
   }

   if (compute->dynamic_threadgroup && threadgroup_bytes == 0) {
      say(why, why_size, "%s takes a threadgroup buffer whose size only the dispatch can "
                         "give (Metal's -setThreadgroupMemoryLength:atIndex:), and this "
                         "dispatch gave none", name);
      return -1;
   }

   /* Everything the kernel declares has to be bound, of the right kind and big enough.
    * The same check, in the same words, guards a draw's stages -- check_bindings.
    */
   if (check_bindings(&compute->info, compute->slots, bindings, table, table_count,
                      name, why, why_size) != 0)
      return -1;

   unsigned ib_bytes = 0;
   if (compute->info.imageblock_data_size > 0) {
      unsigned ib_w = (ib_dim && ib_dim[0]) ? ib_dim[0] : group_size[0];
      unsigned ib_h = (ib_dim && ib_dim[1]) ? ib_dim[1] : group_size[1];
      ib_bytes = ALIGN_POT(ib_w * ib_h * compute->info.imageblock_data_size, 16);
   }
   unsigned var_shared = ib_bytes + (compute->dynamic_threadgroup ? threadgroup_bytes : 0);
   if (!device->software && compute->caps.static_shared_bytes + var_shared > 32768) {
      say(why, why_size, "%s asks for %u bytes of shared/imageblock memory and this GPU takes at most 32768",
          name, compute->caps.static_shared_bytes + var_shared);
      return -1;
   }

   struct rwa_yuv_key yuv;
   if (!yuv_key_for(&compute->info, bindings, table, table_count, &yuv, why, why_size))
      return -1;
   void *cso = compute_state_for_ib(compute, group_size, &yuv, ib_dim, split, why, why_size);
   if (!cso)
      return -1;
   /* Re-check threadgroup limit for unsplit CSOs once a deferred CSO materializes with
    * compiled register pressure. Split CSOs are always compiled fixed for group_size and
    * already validated against si.max_threads inside compute_state_for_ib(). */
   if (!device->software && !split && compute->caps.max_threads_per_group &&
       threads > compute->caps.max_threads_per_group) {
      say(why, why_size, "%s: %ux%ux%u is %u threads in one threadgroup and this GPU takes "
                         "at most %u", name, group_size[0], group_size[1], group_size[2],
          threads, compute->caps.max_threads_per_group);
      return -1;
   }

   /* The binding is bind_stage's, shared with the fragment stage of a draw. On a
    * failure mid-bind nothing has been set yet (the set_* calls are at its end), so
    * there is nothing to take back; the compute state stays bound and is replaced by
    * the next dispatch or dropped at delete, as before.
    */
   struct rwa_bound bound = { 0 };
   if (!rgb2p_prepare_bindings(&compute->info, bindings, table, table_count, name, why, why_size))
      return -1;
   if (bind_stage(device, MESA_SHADER_COMPUTE, &compute->info, compute->slots, compute->own_sampler,
                  compute->compare_sampler, &yuv,
                  bindings, table, table_count, name, &bound, why, why_size) != 0)
      return -1;
   pipe->bind_compute_state(pipe, cso);

   struct pipe_grid_info grid = {
      .work_dim = 3,
      .block = { group_size[0], group_size[1], group_size[2] },
      .grid = { groups[0], groups[1], groups[2] },
      .variable_shared_mem = var_shared,
   };
   pipe->launch_grid(pipe, &grid);
   /* Metal runs the dispatches of a serial compute encoder in order, each one seeing what the ones
    * before it wrote. freedreno puts a cache flush and a wait between two grids of one batch only
    * when memory_barrier asks for it (fd6_barrier.cc add_flushes); without one the Deep Fusion
    * chain read its own intermediate textures before they were written, and the photograph came
    * out dark green unless every dispatch was waited for (kernel-12377-lens2-w24 against w25). */
   pipe->memory_barrier(pipe, PIPE_BARRIER_SHADER_BUFFER | PIPE_BARRIER_CONSTANT_BUFFER |
                              PIPE_BARRIER_IMAGE | PIPE_BARRIER_TEXTURE);

   /* The waiting entry points wait here, where a dispatch that does not come back can
    * be named, and because the buffers stay mapped: a caller reading them before the
    * fence reads what it wrote. rwa_dispatch_nowait leaves the batch to the next flush.
    */
   uint64_t began = wait ? device_us() : 0;
   int rc = wait ? finish_or_say_why(device, true, "dispatch", why, why_size) : 0;

   /* (waited for, a dispatch the GPU spends a tenth of a second on is named: the slow one of
    * a command buffer that holds dozens) */
   if (wait && device_us() - began > 100000) {
      char line[200];
      int n = snprintf(line, sizeof line, "air: %s: %ux%ux%u threadgroups of %ux%ux%u took %llu ms\n",
                       name, groups[0], groups[1], groups[2], group_size[0], group_size[1],
                       group_size[2], (unsigned long long)(device_us() - began) / 1000);
      if (n > 0)
         (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
   }
   unbind_stage(device, MESA_SHADER_COMPUTE, &bound);
   return rc;
}

int
rwa_dispatch_threadgroup(struct rwa_compute *compute, const struct rwa_bindings *bindings,
                         const struct rwa_buffer_bind *table, unsigned table_count,
                         const unsigned groups[3], const unsigned group_size[3],
                         unsigned threadgroup_bytes, char *why, size_t why_size)
{
   return dispatch_grid(compute, bindings, table, table_count, groups, group_size,
                        threadgroup_bytes, NULL, NULL, true, why, why_size);
}

int
rwa_dispatch_nowait(struct rwa_compute *compute, const struct rwa_bindings *bindings,
                    const struct rwa_buffer_bind *table, unsigned table_count,
                    const unsigned groups[3], const unsigned group_size[3],
                    unsigned threadgroup_bytes, char *why, size_t why_size)
{
   return dispatch_grid(compute, bindings, table, table_count, groups, group_size,
                        threadgroup_bytes, NULL, NULL, false, why, why_size);
}

int
rwa_dispatch_nowait_imageblock(struct rwa_compute *compute, const struct rwa_bindings *bindings,
                               const struct rwa_buffer_bind *table, unsigned table_count,
                               const unsigned groups[3], const unsigned group_size[3],
                               unsigned threadgroup_bytes, const unsigned imageblock_size[2],
                               char *why, size_t why_size)
{
   return dispatch_grid(compute, bindings, table, table_count, groups, group_size,
                        threadgroup_bytes, imageblock_size, NULL, false, why, why_size);
}

int
rwa_dispatch_split(struct rwa_compute *compute, const struct rwa_bindings *bindings,
                   const struct rwa_buffer_bind *table, unsigned table_count,
                   const unsigned groups[3], const unsigned group_size[3],
                   unsigned threadgroup_bytes, const unsigned imageblock_size[2],
                   const struct rwa_grid_split *split, bool wait, char *why, size_t why_size)
{
   return dispatch_grid(compute, bindings, table, table_count, groups, group_size,
                        threadgroup_bytes, imageblock_size, split, wait, why, why_size);
}
