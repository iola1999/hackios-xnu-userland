/* AIR entry selection and common NIR finalization. Included by air_to_nir.c. */

nir_shader *
air_to_nir(void *mem_ctx, const void *bitcode, size_t size, const char *entry_point,
           const struct air_to_nir_options *options,
           const nir_shader_compiler_options *nir_options,
           struct air_shader_info *info, const char **error)
{
   void *ctx = ralloc_context(mem_ctx);
   struct air_shader_info local_info = { 0 };
   struct air_ctx c = {
      .mem_ctx = ctx,
      .options = options,
      .info = info ? info : &local_info,
   };
   memset(c.info, 0, sizeof(*c.info));

   const struct air_stitched_kernel *kernel = options ? options->kernel : NULL;
   const struct air_md *entry = NULL;
   const struct air_function *fn = NULL;
   const struct air_md *in_node = NULL;
   int stage = MESA_SHADER_COMPUTE;
   bool wrap = false;
   if (!kernel) {
      const char *read_error = NULL;
      c.m = air_module_read(ctx, bitcode, size, &read_error);
      if (!c.m) {
         if (error)
            *error = ralloc_asprintf(mem_ctx, "reader: %s", read_error);
         ralloc_free(ctx);
         return NULL;
      }

      stage = module_stage(c.m, &entry);
      if (stage == -2) {
         if (error)
            *error = ralloc_strdup(mem_ctx, "stage: no entry point metadata");
         ralloc_free(ctx);
         return NULL;
      }
      /* air.visible: a library function, linked into its caller at pipeline time
       * (docs/air.md section 7.5). Not an entry point, so not a shader -- unless
       * the caller asks for the generated wrapper, which makes one compute shader
       * whose only job is to call it (wrap_library_function).
       */
      if (stage == MESA_SHADER_NONE) {
         if (!options || !options->library_wrapper) {
            if (error)
               *error = ralloc_strdup(mem_ctx,
                                      "stage: air.visible, a library function (not yet)");
            ralloc_free(ctx);
            return NULL;
         }
         stage = MESA_SHADER_COMPUTE;
         wrap = true;
      }

      if (!entry->num_ops || !entry->ops[0] || entry->ops[0]->kind != AIR_MD_VALUE ||
          entry->ops[0]->value->kind != AIR_VALUE_FUNCTION) {
         if (error)
            *error = ralloc_strdup(mem_ctx, "stage: entry point metadata without a function");
         ralloc_free(ctx);
         return NULL;
      }
      fn = (const struct air_function *)entry->ops[0]->value;
      if (entry_point && strcmp(entry_point, fn->base.name ? fn->base.name : "")) {
         if (error)
            *error = ralloc_asprintf(mem_ctx, "stage: this module's entry point is %s",
                                     fn->base.name);
         ralloc_free(ctx);
         return NULL;
      }
      if (fn->is_declaration) {
         if (error)
            *error = ralloc_strdup(mem_ctx, "stage: the entry point has no body");
         ralloc_free(ctx);
         return NULL;
      }
   }
   c.fn = fn;

   c.nir = nir_shader_create(mem_ctx, stage, nir_options);
   const char *name = kernel ? kernel->graph.name : fn->base.name;
   c.nir->info.name = ralloc_strdup(c.nir, name ? name : "air");
   c.nir->info.internal = false;
   c.fns = _mesa_pointer_hash_table_create(ctx);
   nir_function *main = nir_function_create(c.nir, "main");
   main->is_entrypoint = true;
   c.b = nir_builder_at(nir_before_impl(nir_function_impl_create(main)));
   c.b.impl->structured = false;
   c.const_cursor = nir_before_impl(c.b.impl);

   c.values = _mesa_pointer_hash_table_create(ctx);
   c.globals = _mesa_pointer_hash_table_create(ctx);
   c.blocks = _mesa_pointer_hash_table_create(ctx);
   c.phi_cursors = _mesa_pointer_hash_table_create(ctx);
   c.phi_regs = _mesa_pointer_hash_table_create(ctx);
   c.num_params = fn ? fn->num_args : 0;
   c.params = rzalloc_array(ctx, struct air_val *, MAX2(c.num_params, 1));

   c.info->stage = stage;
   c.info->entry_point = name;

   if (kernel) {
      translate_stitched_kernel(&c, kernel);
   } else {

      /* the outputs: fields of the returned struct, in order. A wrapped library
       * function has none of this: its result is a value the wrapper stores to a
       * buffer, not a shader output, and its own metadata uses air.visible_input
       * and air.visible_output rather than the keys translate_input reads.
       */
      const struct air_md *out_node = (!wrap && entry->num_ops > 1) ? entry->ops[1] : NULL;
      const struct air_type *rt = fn->fn_type->elem;
      unsigned num_fields = wrap ? 0 :
                            rt->kind == AIR_TYPE_STRUCT ? rt->num_members :
                            rt->kind == AIR_TYPE_VOID ? 0 : 1;
      c.num_outputs = num_fields;
      c.outputs = rzalloc_array(ctx, __typeof__(*c.outputs), MAX2(num_fields, 1));

      unsigned next_out_varying = 0, next_in_varying = 0;
      if (out_node && out_node->num_ops != num_fields && num_fields) {
         air_error(&c, "interface: %u outputs for %u result fields", out_node->num_ops,
                   num_fields);
      } else {
         for (unsigned i = 0; i < num_fields && !c.error[0]; i++) {
            const struct air_type *ft = rt->kind == AIR_TYPE_STRUCT ? rt->members[i] : rt;
            if (!out_node || i >= out_node->num_ops || !out_node->ops[i]) {
               air_error(&c, "interface: result field %u has no metadata", i);
               break;
            }
            translate_output(&c, out_node->ops[i], i, ft, &next_out_varying);
         }
      }

      /* the inputs */
      in_node = (!wrap && entry->num_ops > 2) ? entry->ops[2] : NULL;
      for (unsigned i = 0; i < (in_node ? in_node->num_ops : 0) && !c.error[0]; i++)
         translate_input(&c, in_node->ops[i], &next_in_varying);

      /* Now that every direct argument's Metal index is known, the members of the
       * argument buffers can be given bindings that cannot collide with them --
       * and before plan_globals, so that a shader's own `constexpr sampler` still
       * comes after every sampler the API binds.
       */
      if (!c.error[0] && c.num_argbufs)
         allocate_argbuf_indices(&c);

      /* the entry point's parameters, as the body will read them */
      for (unsigned i = 0; i < (wrap ? 0 : c.num_params) && !c.error[0]; i++) {
         if (c.params[i])
            air_set(&c, &fn->args[i], c.params[i]);
         else
            air_error(&c, "interface: parameter %u has no metadata", i);
      }

      if (!c.error[0])
         plan_globals(&c);
      /* Gallium adds the dispatch's variable_shared_mem after static_shared_mem, so the
       * threadgroup buffer the dispatch sizes starts where the static arrays end. The
       * pointer value is the one air_set stored, and the body is translated after this.
       */
      if (!c.error[0] && c.dynamic_threadgroup && c.shared_size) {
         c.shared_size = ALIGN_POT(c.shared_size, 16);
         c.dynamic_threadgroup->ptr.offset = nir_imm_int(&c.b, c.shared_size);
      }
      if (!c.error[0])
         materialise_globals(&c);
      if (!c.error[0])
         run_static_initializers(&c);

      if (!c.error[0]) {
         if (wrap) {
            /* air.visible's second operand is the result node, whose one entry
             * carries air.visible_output and the type's name
             */
            const struct air_md *rn = entry->num_ops > 1 ? entry->ops[1] : NULL;
            wrap_library_function(&c, rn && rn->num_ops ? rn->ops[0] : NULL,
                                  entry->num_ops > 2 ? entry->ops[2] : NULL);
         }
         else
            translate_body(&c);
      }

   }
   if (c.error[0]) {
      if (error)
         *error = ralloc_strdup(mem_ctx, c.error);
      ralloc_free(c.nir);
      ralloc_free(ctx);
      return NULL;
   }

   /* What the shader expects to be bound, as NIR counts it: Metal's indices
    * are the binding numbers, so a shader using [[buffer(14)]] alone still
    * declares fifteen UBOs. ir3 and NIR's own passes read these.
    */
   for (unsigned i = 0; i < c.info->num_resources; i++) {
      const struct air_resource *r = &c.info->resources[i];
      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER:
         c.nir->info.num_ubos = MAX2(c.nir->info.num_ubos, r->index + 1);
         break;
      case AIR_RESOURCE_DEVICE_BUFFER:
         c.nir->info.num_ssbos = MAX2(c.nir->info.num_ssbos, r->index + 1);
         break;
      case AIR_RESOURCE_TEXTURE:
         c.nir->info.num_textures = MAX2(c.nir->info.num_textures, r->index + 1);
         BITSET_SET(c.nir->info.textures_used, r->index);
         break;
      case AIR_RESOURCE_IMAGE:
         c.nir->info.num_images = MAX2(c.nir->info.num_images, r->index + 1);
         BITSET_SET(c.nir->info.images_used, r->index);
         if (r->level_count) {
            c.nir->info.num_images = MAX2(c.nir->info.num_images,
                                          r->level_base + r->level_count);
            BITSET_SET_RANGE(c.nir->info.images_used, r->level_base,
                             r->level_base + r->level_count - 1);
         }
         break;
      default:
         break;
      }
   }

   c.nir->scratch_size = c.scratch_size;
   c.nir->info.shared_size = c.shared_size;
   c.info->threadgroup_bytes = c.shared_size;
   c.nir->info.io_lowered = false;
   if (stage == MESA_SHADER_COMPUTE) {
      /* the threadgroup size is a dispatch argument, not a shader constant */
      c.nir->info.workgroup_size_variable = true;
   }

   nir_lower_goto_ifs(c.nir);

   /* Fold in any function of the module the shader called, and then drop the
    * callees. This has to come after nir_lower_goto_ifs and not before: this
    * translator emits an unstructured CFG, and nir_inline_functions walks the
    * control-flow tree, which asserts on one. The pass order inside is the one
    * nir_functions.c prescribes -- inline, then nir_opt_deref to undo the cast
    * a pointer parameter leaves behind, then delete everything but the entry
    * point, which every consumer of this NIR assumes.
    */
   if (_mesa_hash_table_num_entries(c.fns)) {
      NIR_PASS(_, c.nir, nir_inline_functions);
      NIR_PASS(_, c.nir, nir_opt_deref);
      nir_remove_non_entrypoints(c.nir);
      NIR_PASS(_, c.nir, nir_lower_variable_initializers, nir_var_function_temp);
      NIR_PASS(_, c.nir, nir_split_var_copies);
      NIR_PASS(_, c.nir, nir_lower_var_copies);
      NIR_PASS(_, c.nir, nir_lower_vars_to_ssa);
      NIR_PASS(_, c.nir, nir_opt_dce);
   }

   /* Stitching copy/load helpers use addressable temporaries. All callees are
    * now inlined, so Mesa can promote that scratch to ordinary local values. */
   if (kernel) {
      NIR_PASS(_, c.nir, nir_lower_returns);
      NIR_PASS(_, c.nir, nir_lower_scratch_to_var);
   }
   nir_index_ssa_defs(nir_shader_get_entrypoint(c.nir));
   c.info->group_free = stage == MESA_SHADER_COMPUTE && !c.shared_size &&
                        !reads_threadgroup(c.nir);

   if (info) {
      if (kernel)
         ralloc_steal(mem_ctx, info->reflection);
      else if (stage == MESA_SHADER_COMPUTE && !wrap)
         info->reflection = air_compute_reflection(mem_ctx, c.m, fn, in_node,
                                                   &info->reflection_error);
      /* The info's arrays live under mem_ctx, not under the scratch context --
       * and so must every STRING in them, which is the part this used to get
       * wrong. `air_md_string` returns a NUL-terminated copy allocated as a
       * child of the metadata node it came from, so every `air.arg_name` and
       * `air.arg_type_name` in here pointed into the module, which is under
       * `ctx`, which is freed three lines below. The caller was handed
       * pointers into freed memory and mostly got away with it, because the
       * bytes usually survive; what it looks like when they do not is
       * `--interface` printing `name=on=1` for an attribute whose name is
       * `texcoord1`, which is how this was found -- vattrcheck.py counted four
       * attributes in a three-attribute shader because two garbled lines each
       * carried a stray tab.
       *
       * vertex_attributes was not copied at all, which was the same bug with
       * no bytes left to get away with.
       */
      if (info->num_resources) {
         info->resources = ralloc_memdup(mem_ctx, info->resources,
                                         info->num_resources * sizeof(*info->resources));
         for (unsigned i = 0; i < info->num_resources; i++) {
            info->resources[i].name = ralloc_strdup(mem_ctx, info->resources[i].name);
            info->resources[i].type_name =
               ralloc_strdup(mem_ctx, info->resources[i].type_name);
         }
      }
      if (info->num_varyings) {
         info->varyings = ralloc_memdup(mem_ctx, info->varyings,
                                        info->num_varyings * sizeof(*info->varyings));
         for (unsigned i = 0; i < info->num_varyings; i++)
            info->varyings[i].name = ralloc_strdup(mem_ctx, info->varyings[i].name);
      }
      if (info->num_vertex_attributes) {
         info->vertex_attributes =
            ralloc_memdup(mem_ctx, info->vertex_attributes,
                          info->num_vertex_attributes * sizeof(*info->vertex_attributes));
         for (unsigned i = 0; i < info->num_vertex_attributes; i++) {
            struct air_vertex_attribute *a = &info->vertex_attributes[i];
            a->name = ralloc_strdup(mem_ctx, a->name);
            a->type_name = ralloc_strdup(mem_ctx, a->type_name);
            a->function_constant = ralloc_strdup(mem_ctx, a->function_constant);
         }
      }
      if (info->num_notes) {
         info->notes = ralloc_memdup(mem_ctx, info->notes,
                                     info->num_notes * sizeof(*info->notes));
         for (unsigned i = 0; i < info->num_notes; i++)
            info->notes[i] = ralloc_strdup(mem_ctx, info->notes[i]);
      }
      info->entry_point = ralloc_strdup(mem_ctx, info->entry_point);
   }
   air_debug_fragment(c.nir);
   air_gather_quad_helpers(c.nir);
   ralloc_free(ctx);
   if (error) *error = NULL;
   return c.nir;
}
