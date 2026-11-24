/* Numeric constant arrays in AIR address space 2 are initialized data.  GEP
 * uses the AIR allocation stride, including the padding between elements. */
static bool
numeric_array(const struct air_type *type)
{
   if (type->kind != AIR_TYPE_ARRAY)
      return false;
   while (type && type->kind == AIR_TYPE_ARRAY)
      type = type->elem;
   return type && (air_type_is_float(type) || type->kind == AIR_TYPE_INT) &&
          air_type_bit_size(type) && air_type_bit_size(type) <= 64;
}

static bool
write_array_constant(const struct air_module *m, const struct air_type *t,
                     const struct air_constant *k, uint8_t *dst, unsigned size)
{
   if (!k || k->base.type != t)
      return false;
   if (k->ckind == AIR_CONST_NULL || k->ckind == AIR_CONST_UNDEF ||
       k->ckind == AIR_CONST_POISON)
      return true; /* The section is zero-filled. */
   if (t->kind != AIR_TYPE_ARRAY) {
      if (k->ckind != AIR_CONST_INT && k->ckind != AIR_CONST_FLOAT)
         return false;
      unsigned bytes = DIV_ROUND_UP(air_type_bit_size(t), 8);
      if (bytes > size || bytes > sizeof(k->u))
         return false;
      for (unsigned i = 0; i < bytes; i++)
         dst[i] = k->u >> (8 * i);
      return true;
   }
   uint64_t stride = air_type_alloc_size(m, t->elem);
   if (!stride || t->count > size / stride || k->num_elems != t->count)
      return false;
   for (unsigned i = 0; i < t->count; i++) {
      uint8_t *elem = dst + i * stride;
      if (k->ckind == AIR_CONST_AGGREGATE) {
         if (!write_array_constant(m, t->elem, air_value_as_constant(k->elems[i]), elem, stride))
            return false;
      } else if (k->ckind == AIR_CONST_DATA && t->elem->kind != AIR_TYPE_ARRAY) {
         struct air_constant value = { .base.type = t->elem, .ckind = air_type_is_float(t->elem) ?
                                        AIR_CONST_FLOAT : AIR_CONST_INT, .u = k->data[i] };
         if (!write_array_constant(m, t->elem, &value, elem, stride))
            return false;
      } else {
         return false;
      }
   }
   return true;
}

static bool
plan_constant_array(struct air_ctx *c, const struct air_global *g, struct air_global_plan *pl)
{
   unsigned align = MAX2(g->align, air_type_abi_align(c->m, g->value_type));
   uint64_t off = ALIGN_POT((uint64_t)c->nir->constant_data_size, MAX2(align, 1));
   uint64_t size = air_type_alloc_size(c->m, g->value_type);
   if (off > UINT32_MAX || size > UINT32_MAX - off)
      return air_error(c, "interface: numeric constant data exceeds the addressable size");
   unsigned total = off + size;
   void *data = rerzalloc_size(c->nir, c->nir->constant_data, c->nir->constant_data_size, total);
   if (!data && total)
      return air_error(c, "interface: could not allocate numeric constant data");
   c->nir->constant_data = data;
   c->nir->constant_data_size = total;
   if (size && !write_array_constant(c->m, g->value_type, air_value_as_constant(g->init),
                                     (uint8_t *)data + off, size))
      return air_error(c, "interface: unsupported numeric constant array initializer");
   pl->class = AIR_PTR_CONSTANT_DATA;
   pl->offset = off;
   pl->has_offset = true;
   return true;
}

static void
plan_globals(struct air_ctx *c)
{
   c->global_plan = rzalloc_array(c->mem_ctx, struct air_global_plan,
                                  MAX2(c->m->num_globals, 1));
   for (unsigned i = 0; i < c->m->num_globals; i++) {
      struct air_global_plan *pl = &c->global_plan[i];
      const struct air_global *g = c->m->globals[i];
      const char *name = g->base.name ? g->base.name : "";

      /* llvm.compiler.used and llvm.global_ctors are lists for the linker,
       * not memory the shader reads
       */
      if (!strncmp(name, "llvm.", 5)) {
         pl->skip = true;
         continue;
      }

      if (g->externally_initialized) {
         int idx = function_constant_index(c, g);
         if (idx < 0) {
            air_error(c, "interface: an externally initialized variable that is not a "
                         "function constant");
            return;
         }
         pl->class = AIR_PTR_FUNCTION_CONSTANT;
         pl->binding = idx;
      } else if (!strncmp(name, "__air_sampler_state", 19)) {
         /* A sampler the shader declares itself. It needs a real index, not a
          * sentinel: the Adreno's texture instruction holds the sampler in a
          * four-bit field, and ~0u there asserts in ir3's encoder. The index
          * goes after the API's samplers, and the runtime creates the sampler
          * from the state word (docs/air.md section 7.7).
          */
         unsigned next = 0;
         const struct air_stitched_kernel *kernel = c->options ? c->options->kernel : NULL;
         for (unsigned k = 0; kernel && k < kernel->num_inputs; k++)
            if (kernel->inputs[k].kind == AIR_STITCH_SAMPLER)
               next = MAX2(next, kernel->inputs[k].binding + 1);
         for (unsigned k = 0; k < c->info->num_resources; k++)
            if (c->info->resources[k].kind == AIR_RESOURCE_SAMPLER)
               next = MAX2(next, c->info->resources[k].index + 1);
         pl->class = AIR_PTR_SAMPLER;
         pl->binding = next;

         uint64_t word = 0;
         if (!inline_sampler_word(c, g, &word))
            return;
         struct air_shader_info *info = c->info;
         info->resources = reralloc(c->mem_ctx, info->resources, struct air_resource,
                                    info->num_resources + 1);
         info->resources[info->num_resources++] = (struct air_resource) {
            .kind = AIR_RESOURCE_SAMPLER, .index = next, .read = true,
            .param = ~0u, .name = name,
            .inline_sampler = true, .sampler_state = word,
            .sampler = air_decode_sampler_state(word),
            /* not through add_resource, so the "no group" value has to be
             * written here: a zeroed field would read as group 0, and a zeroed
             * argbuf_set as "member 0 of the set at Metal index 0" */
            .raster_order_group = -1,
            .argbuf_set = ~0u, .argbuf_id = ~0u,
         };
      } else if (g->addrspace == 2 && g->is_const && g->init && numeric_array(g->value_type)) {
         if (!plan_constant_array(c, g, pl))
            return;
      } else if (g->addrspace == 3) {
         /* a static threadgroup array */
         unsigned align = MAX2(g->align, air_type_abi_align(c->m, g->value_type));
         c->shared_size = ALIGN_POT(c->shared_size, MAX2(align, 1));
         pl->class = AIR_PTR_SHARED;
         pl->offset = c->shared_size;
         pl->has_offset = true;
         c->shared_size += air_type_alloc_size(c->m, g->value_type);
      } else {
         /* a program-scope variable: private to the invocation, written by
          * the static initializers below
          */
         unsigned align = MAX2(g->align, air_type_abi_align(c->m, g->value_type));
         c->scratch_size = ALIGN_POT(c->scratch_size, MAX2(align, 1));
         pl->class = AIR_PTR_SCRATCH;
         pl->offset = c->scratch_size;
         pl->has_offset = true;
         c->scratch_size += air_type_alloc_size(c->m, g->value_type);
      }
   }
}
