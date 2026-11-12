/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 *
 * GraphSPI compute inputs and control flow, included by air_to_nir.c.
 */

static const struct air_md *
stitch_argument_type(const struct air_module *m, const struct air_function *f, unsigned arg)
{
   const struct air_named_md *visible = named_md(m, "air.visible");
   for (unsigned i = 0; i < (visible ? visible->num_ops : 0); i++) {
      const struct air_md *e = visible->ops[i];
      if (!e || e->num_ops < 4 || !e->ops[0] ||
          e->ops[0]->kind != AIR_MD_VALUE || e->ops[0]->value != &f->base)
         continue;
      const struct air_md *si = e->ops[3];
      if (!si || !md_str(si, 0) || strcmp(md_str(si, 0), "air.stitching_info") ||
          arg + 2 >= si->num_ops)
         return NULL;
      const struct air_md *a = si->ops[arg + 2];
      return a && a->num_ops > 1 ? a->ops[1] : NULL;
   }
   return NULL;
}

static const struct air_function *
stitch_helper(const struct air_md *type, unsigned slot)
{
   const struct air_md *v = type && slot < type->num_ops ? type->ops[slot] : NULL;
   return v && v->kind == AIR_MD_VALUE && v->value &&
          v->value->kind == AIR_VALUE_FUNCTION ? (const struct air_function *)v->value : NULL;
}

static bool
stitch_value_matches(struct air_ctx *c, const struct air_val *v, const struct air_type *t)
{
   if (!v || !t)
      return false;
   if (t->kind == AIR_TYPE_POINTER)
      return v->kind == AIR_VAL_PTR;
   if (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY) {
      unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : t->count;
      if (v->kind != AIR_VAL_AGG || v->num_elems != n)
         return false;
      for (unsigned i = 0; i < n; i++)
         if (!stitch_value_matches(c, v->elems[i],
                                    t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem))
            return false;
      return true;
   }
   return v->kind == AIR_VAL_SSA && v->def->num_components == air_nir_components(t) &&
          v->def->bit_size == air_nir_bit_size(c, t);
}

/* Reflection reports one API binding even if several nodes use it. Native GraphSPI
 * constant buffers are opaque, with size zero and four-byte binding alignment. */
static bool
stitch_resource(struct air_ctx *c, enum air_resource_kind kind, unsigned binding,
                 bool read, bool write, const char *type_name,
                 unsigned texture_type, unsigned data_type)
{
   unsigned binding_type = kind == AIR_RESOURCE_SAMPLER ? 3 :
                           kind == AIR_RESOURCE_CONSTANT_BUFFER ? 0 : 2;
   struct rwa_compute_reflection *ref = c->info->reflection;
   for (unsigned i = 0; i < ref->argument_count; i++) {
      struct rwa_reflection_argument *a =
         (struct rwa_reflection_argument *)&ref->arguments[i];
      if (a->type != binding_type || a->index != binding)
         continue;
      if (binding_type == 2 &&
          (a->texture_type != texture_type || a->texture_data_type != data_type))
         return air_error(c, "link: incompatible texture types at binding %u", binding);
      bool was_read = a->access != 2, was_write = a->access != 0;
      a->access = (was_read || read) ? ((was_write || write) ? 1 : 0) : 2;
      goto resource;
   }
   struct rwa_reflection_argument *args = reralloc(ref, (void *)ref->arguments,
      struct rwa_reflection_argument, ref->argument_count + 1);
   ref->arguments = args;
   struct rwa_reflection_argument *a = &args[ref->argument_count++];
   *a = (struct rwa_reflection_argument) {
      .name = "", .type = binding_type, .index = binding,
      .access = read ? (write ? 1 : 0) : 2, .active = 1, .array_length = 1,
      .constant_buffer = kind == AIR_RESOURCE_CONSTANT_BUFFER,
      .texture_type = texture_type, .texture_data_type = data_type,
   };
   if (a->constant_buffer) {
      struct rwa_reflection_type *t = rzalloc(ref, struct rwa_reflection_type);
      t->alignment = 4;
      t->name = "";
      a->buffer_type = t;
   }
resource:
   for (unsigned i = 0; i < c->info->num_resources; i++) {
      struct air_resource *r = &c->info->resources[i];
      if (r->kind == kind && r->index == binding) {
         r->read |= read;
         r->write |= write;
         return true;
      }
   }
   struct air_resource *r = add_resource(c, kind, binding, 0, read, write, ~0u, NULL);
   if (!r)
      return false;
   r->name = "";
   r->type_name = type_name;
   return true;
}

static struct air_val *
stitch_input(struct air_ctx *c, struct air_linked_module *lm, const struct air_function *f,
              unsigned arg, const struct air_stitch_input *input, struct air_val **temporary)
{
   const struct air_type *t = f->args[arg].type;
   const struct air_md *st = stitch_argument_type(lm->m, f, arg);
   const struct air_md *type = st && st->num_ops > 1 ? st->ops[1] : NULL;
   const char *kind = md_str(type, 0);
   uint64_t size = 0, align = 0;
   if (!kind || !md_int(type, 1, &size) || !md_int(type, 2, &align) ||
       !align || (align & (align - 1)) || size > UINT32_MAX || align > UINT32_MAX) {
      air_error(c, "link: %s argument %u lacks a supported stitching type", f->base.name, arg);
      return NULL;
   }
   if (input->kind == AIR_STITCH_THREAD_POSITION) {
      const struct air_type *scalar = air_type_scalar(t);
      unsigned n = air_nir_components(t);
      const char *scalar_kind = !strcmp(kind, "air.vector_type") && type->num_ops > 4
                                   ? md_str(type->ops[4], 0) : kind;
      if (scalar->kind != AIR_TYPE_INT || scalar->bits != 32 || n < 1 || n > 3 ||
          !scalar_kind || strcmp(scalar_kind, "air.uint_type")) {
         air_error(c, "link: thread position requires uint, uint2 or uint3");
         return NULL;
      }
      return air_val_ssa(c, nir_trim_vector(&c->b,
         nir_load_global_invocation_id(&c->b, 32), n));
   }
   if (input->kind == AIR_STITCH_BUFFER_ADDRESS) {
      if (input->binding >= 31 || input->byte_offset > UINT32_MAX ||
          size > UINT32_MAX - input->byte_offset) {
         air_error(c, "link: constant buffer binding or byte range is out of bounds");
         return NULL;
      }
      if (!stitch_resource(c, AIR_RESOURCE_CONSTANT_BUFFER, input->binding,
                           true, false, "", 0, 0))
         return NULL;
      struct air_val *buffer = air_val_ptr(c, (struct air_ptr) {
         .class = AIR_PTR_UBO, .binding = input->binding,
         .offset = nir_imm_int(&c->b, input->byte_offset), .pointee = t->elem,
      });
      if (!input->dereference) {
         if (strcmp(kind, "air.pointer_type") ||
             t->kind != AIR_TYPE_POINTER || t->addrspace != 2) {
            air_error(c, "link: a buffer address requires a constant pointer argument");
            return NULL;
         }
         return buffer;
      }
      const struct air_function *copy = stitch_helper(st, 5);
      const struct air_function *load = stitch_helper(st, 2);
      const struct air_function *destroy = stitch_helper(st, 4);
      uint64_t scratch_offset = ALIGN_POT((uint64_t)c->scratch_size, align);
      if (!copy || copy->is_declaration || copy->num_args != 2 ||
          copy->fn_type->elem->kind != AIR_TYPE_VOID ||
          !load || load->is_declaration || load->num_args != 1 ||
          !destroy || destroy->is_declaration || destroy->num_args != 1 ||
          destroy->fn_type->elem->kind != AIR_TYPE_VOID ||
          scratch_offset + size > UINT32_MAX) {
         air_error(c, "link: invalid buffer copy/load helpers for %s argument %u",
                   f->base.name, arg);
         return NULL;
      }
      c->scratch_size = scratch_offset;
      struct air_val *scratch = air_val_ptr(c, (struct air_ptr) {
         .class = AIR_PTR_SCRATCH, .offset = nir_imm_int(&c->b, c->scratch_size),
      });
      c->scratch_size += size;
      struct air_val *argv[] = { scratch, buffer };
      inline_linked_function(c, lm, copy, argv);
      if (c->error[0])
         return NULL;
      struct air_val *v = inline_linked_function(c, lm, load, argv);
      /* The C++ matrix helper returns its one-field wrapper; the callable AIR
       * signature lowers that wrapper to its array of columns. */
      while (v && !stitch_value_matches(c, v, t) &&
             v->kind == AIR_VAL_AGG && v->num_elems == 1)
         v = v->elems[0];
      if (!stitch_value_matches(c, v, t)) {
         air_error(c, "link: buffer helper result does not match %s argument %u",
                   f->base.name, arg);
         return NULL;
      }
      *temporary = scratch;
      return v;
   }
   if (input->kind == AIR_STITCH_SAMPLER) {
      if (strcmp(kind, "air.sampler_type") || input->binding >= 16 ||
          t->kind != AIR_TYPE_POINTER) {
         air_error(c, "link: incompatible sampler input");
         return NULL;
      }
      if (!stitch_resource(c, AIR_RESOURCE_SAMPLER, input->binding,
                           true, false, "sampler", 0, 0))
         return NULL;
      return air_val_ptr(c, (struct air_ptr) {
         .class = AIR_PTR_SAMPLER, .binding = input->binding,
      });
   }
   if (input->kind == AIR_STITCH_TEXTURE) {
      const char *access = md_str(type, 5);
      const char *element = type->num_ops > 4 ? md_str(type->ops[4], 0) : NULL;
      bool read = access && (!strcmp(access, "sample") || !strcmp(access, "read") ||
                             !strcmp(access, "read_write"));
      bool write = access && (!strcmp(access, "write") || !strcmp(access, "read_write"));
      unsigned dt = element && !strcmp(element, "air.float_type") ? 3 :
                    element && !strcmp(element, "air.half_type") ? 16 :
                    element && !strcmp(element, "air.int_type") ? 29 :
                    element && !strcmp(element, "air.uint_type") ? 33 : 0;
      if (strcmp(kind, "air.texture_2d_type") || !dt || (!read && !write) ||
          input->binding >= 128 || t->kind != AIR_TYPE_POINTER) {
         air_error(c, "link: unsupported stitched texture type or binding");
         return NULL;
      }
      const char *type_name = dt == 3 ? "texture2d<float>" : dt == 16 ? "texture2d<half>" :
                              dt == 29 ? "texture2d<int>" : "texture2d<uint>";
      if (!stitch_resource(c, write ? AIR_RESOURCE_IMAGE : AIR_RESOURCE_TEXTURE,
                           input->binding, read, write, type_name, 2, dt))
         return NULL;
      return air_val_ptr(c, (struct air_ptr) {
         .class = AIR_PTR_TEXTURE, .binding = input->binding, .is_image = write,
      });
   }
   air_error(c, "link: unknown stitched input kind %u", input->kind);
   return NULL;
}

static bool
translate_stitched_kernel(struct air_ctx *c, const struct air_stitched_kernel *kernel)
{
   const struct air_linked_function *g = &kernel->graph;
   if ((kernel->num_inputs && !kernel->inputs) ||
       !validate_stitch_graph(c, g, kernel->num_inputs, true))
      return air_error(c, "link: missing stitched kernel inputs");
   for (unsigned i = 0; i < kernel->num_inputs; i++)
      if (kernel->inputs[i].kind == AIR_STITCH_SAMPLER && kernel->inputs[i].binding >= 16)
         return air_error(c, "link: sampler input binding is out of bounds");
   c->info->reflection = rzalloc(c->mem_ctx, struct rwa_compute_reflection);
   struct air_val **results = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(g->num_nodes, 1));
   for (unsigned k = 0; k < g->num_nodes; k++) {
      const struct air_stitch_node *n = &g->nodes[k];
      if (!n->bitcode && !n->function) {
         int from = n->args[0];
         struct air_val *condition = from < 0 ? results[-1 - from] : NULL;
         if (!condition || condition->kind != AIR_VAL_SSA ||
             condition->def->num_components != 1 || condition->def->bit_size != 1)
            return air_error(c, "link: EarlyReturnNode requires a preceding boolean result");
         nir_block *next = nir_block_create(c->b.impl);
         exec_list_push_tail(&c->b.impl->body, &next->cf_node.node);
         next->cf_node.parent = &c->b.impl->cf_node;
         nir_goto_if(&c->b, c->b.impl->end_block, condition->def, next);
         c->b.cursor = nir_after_block(next);
         continue;
      }
      struct air_linked_module *lm = linked_module(c, n->bitcode, n->size);
      if (!lm)
         return false;
      const struct air_function *f = module_function(lm->m, n->function);
      if (!f || f->num_args != n->num_args)
         return air_error(c, "link: node %u has an unknown function or wrong argument count", k);
      struct air_val **argv = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(n->num_args, 1));
      struct air_val **temporaries = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(n->num_args, 1));
      for (unsigned a = 0; a < n->num_args; a++) {
         int from = n->args[a];
         argv[a] = from < 0 ? results[-1 - from] :
                    stitch_input(c, lm, f, a, &kernel->inputs[from], &temporaries[a]);
         if (!stitch_value_matches(c, argv[a], f->args[a].type))
            return air_error(c, "link: node %u argument %u has no compatible value", k, a);
         if (c->error[0])
            return false;
      }
      results[k] = inline_linked_function(c, lm, f, argv);
      for (unsigned a = 0; a < n->num_args && !c->error[0]; a++)
         if (temporaries[a])
            inline_linked_function(c, lm,
               stitch_helper(stitch_argument_type(lm->m, f, a), 4), &temporaries[a]);
      if (c->error[0])
         return false;
   }
   nir_goto(&c->b, c->b.impl->end_block);
   return true;
}
