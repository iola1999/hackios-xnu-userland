/* Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

static void *compute_state_for(struct rwa_compute *compute, const unsigned block[3],
                               char *why, size_t why_size);

struct rwa_compute *
rwa_compute_create(struct rwa_device *device, const void *bitcode, size_t size,
                   const char *entry, struct rwa_compute_info *info,
                   char *why, size_t why_size)
{
   RWA_LOCKED;
   if (!bitcode || size < 4) {
      say(why, why_size, "%zu bytes is not a shader module", size);
      return NULL;
   }

   struct rwa_compute *compute = calloc(1, sizeof(*compute));
   if (!compute) {
      say(why, why_size, "out of memory compiling a shader");
      return NULL;
   }
   compute->device = device;
   compute->mem_ctx = ralloc_context(NULL);

   /* Function constants are taken as zero. There is nothing to take them from: Metal's
    * specialization goes through -[MTLFunction newFunctionWithConstantValues:], which
    * is one of the methods that run inside Metal's own classes through a nil compiler
    * and return nil before a driver sees them (docs/metal.md 10.7). A module that needs
    * real values will say so -- the translator refuses what it cannot fold.
    */
   struct air_constant_value *constants;
   unsigned num_constants =
      air_module_function_constants(compute->mem_ctx, bitcode, size, &constants);

   /* library_wrapper is false, unlike air_run --library: a wrapped air.visible function
    * is a shader made up to be measurable, not a pipeline anyone can dispatch. Metal
    * stitches those into a caller at pipeline time (air.stitching_info), which this
    * frontend does not implement, and a driver asked to make a compute pipeline out of
    * one should be told that rather than handed a wrapper.
    */
   const struct air_to_nir_options options = {
      .constants = constants,
      .num_constants = num_constants,
   };
   const char *error;
   compute->nir = air_to_nir(compute->mem_ctx, bitcode, size, entry, &options,
                             device->nir_options, &compute->info, &error);
   if (!compute->nir) {
      /* The sentence the translator wrote, which is the one that matters: it begins
       * with its own category (reader, stage, interface, instruction, intrinsic) and
       * names what it met. 135 of the corpus's 456 modules are refused with one of
       * these, and a driver should repeat it to the application rather than swallow it.
       */
      say(why, why_size, "%s", error ? error : "the module could not be translated");
      goto fail;
   }

   if (compute->info.stage != MESA_SHADER_COMPUTE) {
      /* Nothing reaches the first branch as of now: the translator refuses an
       * air.visible function itself, with "stage: air.visible, a library function (not
       * yet)", and never returns info for one. It is here because that refusal is the
       * frontend's own wording about its own limit, and the day the frontend translates
       * these without a wrapper this is the sentence a driver should get instead.
       */
      if (compute->info.stage == MESA_SHADER_NONE)
         say(why, why_size, "%s is an air.visible library function and not an entry "
                            "point, so it is not a compute pipeline: Metal stitches "
                            "these into a caller at pipeline time",
             compute->info.entry_point ? compute->info.entry_point : "the function");
      else
         /* the stage in words and not as its enumerator's name: this sentence ends up
          * in an NSError that an application shows a person
          */
         say(why, why_size, "%s is a %s shader, and this makes compute pipelines",
             compute->info.entry_point ? compute->info.entry_point : "the function",
             compute->info.stage >= MESA_SHADER_VERTEX &&
             compute->info.stage < MESA_SHADER_STAGES
                ? _mesa_shader_stage_to_string(compute->info.stage) : "non-compute");
      goto fail;
   }

   /* What the shader expects to be bound, counted the way Metal counts it: by the
    * highest index plus one, because the indices are the binding numbers and a kernel
    * using [[buffer(14)]] alone still needs fifteen slots to be reachable.
    */
   for (unsigned i = 0; i < compute->info.num_resources; i++) {
      const struct air_resource *r = &compute->info.resources[i];
      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER:
      case AIR_RESOURCE_DEVICE_BUFFER:
         compute->caps.buffers = MAX2(compute->caps.buffers, r->index + 1);
         break;
      case AIR_RESOURCE_THREADGROUP_BUFFER:
         /* Metal sizes this one at dispatch, with
          * -setThreadgroupMemoryLength:atIndex:, which rwa_dispatch_threadgroup takes.
          */
         compute->dynamic_threadgroup = true;
         break;
      case AIR_RESOURCE_TEXTURE:
         compute->caps.textures = MAX2(compute->caps.textures, r->index + 1);
         /* and which slot it is, so that a dispatch can bind a sampler view here and
          * an image view at the next index: Metal gives a sampled texture and a
          * written one the same [[texture(n)]] index space, and a driver told only
          * "textures: 2" cannot tell which of the two it has.
          */
         if (r->index < RWA_MAX_TEXTURES) {
            compute->slots[r->index].sampled = true;
            compute->slots[r->index].read |= r->read;
            compute->slots[r->index].write |= r->write;
            declare_slot_element(r->type_name, &compute->slots[r->index]);
         }
         break;
      case AIR_RESOURCE_IMAGE:
         /* A texture the kernel writes. Metal gives it a [[texture(n)]] index like any
          * other, so it counts in the same total a driver has to fill; what differs is
          * that Gallium wants it as an image view and not a sampler view
          * (air_to_nir.c), which is why the two are counted apart as well.
          */
         compute->caps.textures = MAX2(compute->caps.textures, r->index + 1);
         if (r->index < RWA_MAX_TEXTURES) {
            compute->slots[r->index].stored = true;
            compute->slots[r->index].read |= r->read;
            compute->slots[r->index].write |= r->write;
            declare_slot_element(r->type_name, &compute->slots[r->index]);
         }
         break;
      case AIR_RESOURCE_SAMPLER:
         /* Only the ones the API has to bind. A `constexpr sampler` the shader declares
          * itself is already decoded (air_resource.sampler) and is nothing for a driver
          * to create; counting it here would tell the driver to bind a slot it must
          * leave alone.
          */
         if (!r->inline_sampler)
            compute->caps.samplers = MAX2(compute->caps.samplers, r->index + 1);
         break;
      }
   }

   slot_shapes(compute->nir, compute->slots, compute->compare_sampler);

   compute->caps.static_shared_bytes = compute->info.threadgroup_bytes;
   compute->caps.group_free = compute->info.group_free;
   compute->caps.max_threads_per_group = device->max_threads_per_group;
   compute->caps.thread_execution_width = device->thread_execution_width;
   /* And which of those buffer indices are sets rather than arguments, so that a driver
    * knows which of its bound buffers to pass as rwa_dispatch_buffers' array.
    */
   if (argbuf_mask_of(&compute->info,
                      compute->info.entry_point ? compute->info.entry_point
                                                : "the function",
                      &compute->caps.argbuf_mask, why, why_size) != 0)
      goto fail;

   /* The hardware pipeline reports the limit of the same variable-workgroup
    * CSO that dispatch will use. Softpipe requires a fixed block at dispatch. */
   if (!device->software && !compute_state_for(compute, NULL, why, why_size))
      goto fail;
   if (info)
      *info = compute->caps;
   return compute;

fail:
   ralloc_free(compute->mem_ctx);
   free(compute);
   return NULL;
}

/* Unbound before it is deleted, always. A Gallium context keeps a pointer to the state
 * bound in it, and softpipe asserts that the one being deleted is not that pointer
 * (sp_state_shader.c, softpipe_delete_compute_state) -- rightly, since the alternative
 * is a context holding freed memory until something else is bound. air_run never meets
 * this because a harness that runs one kernel and exits never deletes anything; a
 * library whose caller makes and drops pipelines all day meets it at once.
 *
 * Unbinding another pipeline's state in passing costs nothing: the device has one
 * context, and rwa_dispatch binds the state it is about to launch every time rather than
 * trusting what was bound before.
 */
static void
delete_compute_state(struct rwa_device *device, void *cso)
{
   device->pipe->bind_compute_state(device->pipe, NULL);
   device->pipe->delete_compute_state(device->pipe, cso);
}

void
rwa_compute_destroy(struct rwa_compute *compute)
{
   RWA_LOCKED;
   if (!compute)
      return;
   if (compute->cso)
      delete_compute_state(compute->device, compute->cso);
   /* and the states built for the samplers the shader declared itself. Nothing is
    * unbound first, for the reason rwa_sampler_destroy gives: a dispatch leaves
    * nothing of its own bound.
    */
   for (unsigned i = 0; i < RWA_MAX_TEXTURES; i++)
      if (compute->own_sampler[i])
         compute->device->pipe->delete_sampler_state(compute->device->pipe,
                                                     compute->own_sampler[i]);
   ralloc_free(compute->mem_ctx);
   free(compute);
}

/* Hardware keeps one variable-workgroup CSO for queries and dispatch.
 * Softpipe's TGSI path requires a fixed block, so it recompiles on size changes.
 * Compile a clone because the driver takes ownership of the input NIR. */
static void *
compute_state_for(struct rwa_compute *compute, const unsigned block[3],
                  char *why, size_t why_size)
{
   struct rwa_device *device = compute->device;

   if (compute->cso && (!device->software ||
       (compute->cso_block[0] == block[0] && compute->cso_block[1] == block[1] &&
        compute->cso_block[2] == block[2])))
      return compute->cso;

   if (compute->cso) {
      delete_compute_state(device, compute->cso);
      compute->cso = NULL;
   }

   nir_shader *cs = nir_shader_clone(NULL, compute->nir);
   if (!cs) {
      say(why, why_size, "out of memory compiling the shader");
      return NULL;
   }
   if (device->software) {
      cs->info.workgroup_size[0] = block[0];
      cs->info.workgroup_size[1] = block[1];
      cs->info.workgroup_size[2] = block[2];
      cs->info.workgroup_size_variable = false;
   }

   if (!lower_half_for_backend(cs, device, &compute->info, why, why_size)) {
      ralloc_free(cs);
      return NULL;
   }
   finalize_for_gallium(cs, device->screen);

   void *cso = device->pipe->create_compute_state(device->pipe,
      &(struct pipe_compute_state){
         .ir_type = PIPE_SHADER_IR_NIR, .prog = cs,
         .static_shared_mem = cs->info.shared_size });
   if (!cso) {
      /* The clone is not freed here: a driver that refused it may or may not have taken
       * it already, and neither of the two drivers in this build can reach this at all
       * (both return a state object and report a bad shader later, at launch).
       */
      say(why, why_size, "%s would not make a compute pipeline of %s",
          device->screen->get_name(device->screen),
          compute->info.entry_point ? compute->info.entry_point : "the shader");
      return NULL;
   }

   struct pipe_compute_state_object_info si = { 0 };
   if (device->pipe->get_compute_state_info)
      device->pipe->get_compute_state_info(device->pipe, cso, &si);
   if (!device->software) {
      if (!si.max_threads) {
         say(why, why_size, "the hardware driver did not report a compiled threadgroup limit");
         delete_compute_state(device, cso);
         return NULL;
      }
      compute->caps.max_threads_per_group = MIN2(compute->caps.max_threads_per_group,
                                                 si.max_threads);
      if (si.preferred_simd_size)
         compute->caps.thread_execution_width = si.preferred_simd_size;
   } else if (si.max_threads && block[0] * block[1] * block[2] > si.max_threads) {
      say(why, why_size, "this dispatch asks for %u threads in a threadgroup and the "
                         "compiled shader takes at most %u: it uses too many "
                         "registers for a group that large",
          block[0] * block[1] * block[2], si.max_threads);
      delete_compute_state(device, cso);
      return NULL;
   }

   compute->cso = cso;
   if (device->software) {
      compute->cso_block[0] = block[0];
      compute->cso_block[1] = block[1];
      compute->cso_block[2] = block[2];
   }
   return cso;
}

