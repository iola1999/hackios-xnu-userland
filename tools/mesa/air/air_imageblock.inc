/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 *
 * Metal compute explicit imageblocks lowered to workgroup shared memory,
 * included by air_intrinsics.c after air_texture.inc.
 */

bool
air_translate_imageblock_input(struct air_ctx *c, unsigned param,
                               const struct air_type *t, bool is_explicit,
                               uint64_t data_size)
{
   if (c->nir->info.stage != MESA_SHADER_COMPUTE)
      return air_error(c, "interface: air.imageblock in a %s shader",
                       _mesa_shader_stage_to_string(c->nir->info.stage));
   if (!is_explicit)
      return air_error(c, "interface: implicit air.imageblock in a compute shader");
   if (data_size == 0 || data_size > 256)
      return air_error(c, "interface: air.imageblock_data_size %u out of range",
                       (unsigned)data_size);
   if (c->imageblock_data_size != 0)
      return air_error(c, "interface: multiple air.imageblock inputs");

   c->imageblock_data_size = (unsigned)data_size;
   struct air_ptr p = {
      .class = AIR_PTR_SHARED,
      .offset = nir_imm_int(&c->b, 0),
   };
   struct air_val *pv = air_val_ptr(c, p);
   c->imageblock_param_ptr = pv;

   if (t && t->kind == AIR_TYPE_STRUCT && t->num_members == 1) {
      struct air_val *agg = rzalloc(c->mem_ctx, struct air_val);
      agg->kind = AIR_VAL_AGG;
      agg->num_elems = 1;
      agg->elems = rzalloc_array(c->mem_ctx, struct air_val *, 1);
      agg->elems[0] = pv;
      c->params[param] = agg;
   } else {
      c->params[param] = pv;
   }
   return true;
}

void
air_plan_imageblock(struct air_ctx *c)
{
   if (!c->imageblock_data_size)
      return;

   c->shared_size = ALIGN_POT(c->shared_size, 16);
   c->imageblock_base = c->shared_size;
   c->info->imageblock_data_size = c->imageblock_data_size;
   if (c->imageblock_param_ptr)
      c->imageblock_param_ptr->ptr.offset = nir_imm_int(&c->b, c->imageblock_base);

   /* Stored in a shader_temp uvec2 so standalone NIR consumers fold it to
    * load_workgroup_size.xy while rwa_dispatch_nowait_imageblock can override
    * the dimensions when -[MTLComputeCommandEncoder setImageblockWidth:height:]
    * specifies an explicit tile size larger than threadsPerThreadgroup.
    */
   c->imageblock_dim_var =
      nir_variable_create(c->nir, nir_var_shader_temp, glsl_uvec_type(2), "__air_ib_dim");
   nir_def *wg_xy = (c->options && c->options->imageblock_width && c->options->imageblock_height)
      ? nir_imm_ivec2(&c->b, c->options->imageblock_width, c->options->imageblock_height)
      : nir_channels(&c->b, nir_load_workgroup_size(&c->b), 0x3);
   nir_store_var(&c->b, c->imageblock_dim_var, wg_xy, 0x3);

   if (c->dynamic_threadgroup) {
      nir_def *ib_elems = nir_imul(&c->b, nir_channel(&c->b, wg_xy, 0),
                                   nir_channel(&c->b, wg_xy, 1));
      nir_def *ib_bytes = nir_imul_imm(&c->b, ib_elems, c->imageblock_data_size);
      nir_def *ib_aligned = nir_iand_imm(&c->b, nir_iadd_imm(&c->b, ib_bytes, 15), ~15u);
      c->dynamic_threadgroup->ptr.offset =
         nir_iadd_imm(&c->b, ib_aligned, c->imageblock_base);
   }
}

static bool
parse_imageblock_slice_elem(const char *part, unsigned *comps_out,
                            unsigned *bits_out, bool *is_float_out)
{
   const char *p = part;
   unsigned comps = 1;
   if (p[0] == 'v' && p[1] >= '1' && p[1] <= '4') {
      comps = (unsigned)(p[1] - '0');
      p += 2;
   }
   if (p[0] != 'f' && p[0] != 'i')
      return false;
   bool is_fp = (p[0] == 'f');
   unsigned bits = (unsigned)strtoul(p + 1, NULL, 10);
   if (bits != 8 && bits != 16 && bits != 32)
      return false;
   if (is_fp && bits == 8)
      return false;
   *comps_out = comps;
   *bits_out = bits;
   *is_float_out = is_fp;
   return true;
}

static bool
image_slot_is_signed_int(const struct air_ctx *c, unsigned binding,
                         const struct air_name *n)
{
   for (unsigned i = 1; i + 1 < n->num; i++) {
      if (!strcmp(n->part[i], "s"))
         return true;
      if (!strcmp(n->part[i], "u"))
         return false;
   }
   for (unsigned i = 0; i < c->info->num_resources; i++) {
      const struct air_resource *r = &c->info->resources[i];
      if (r->kind != AIR_RESOURCE_IMAGE || r->index != binding || !r->type_name)
         continue;
      if (strstr(r->type_name, "<int") || strstr(r->type_name, "<short") ||
          strstr(r->type_name, "<char"))
         return true;
      return false;
   }
   return false;
}

static bool
air_imageblock_op(struct air_ctx *c, const struct air_inst *in,
                  const struct air_name *n, bool *handled)
{
   const char *op = n->part[0];

   if (!strcmp(op, "get_imageblock_width") || !strcmp(op, "get_imageblock_height")) {
      *handled = true;
      if (!c->imageblock_dim_var)
         return air_error(c, "intrinsic: air.%s without an air.imageblock input", op);
      unsigned ch = !strcmp(op, "get_imageblock_height") ? 1 : 0;
      nir_def *v = nir_channel(&c->b, nir_load_var(&c->b, c->imageblock_dim_var), ch);
      air_set(c, &in->base,
              air_val_ssa(c, nir_u2uN(&c->b, v, air_nir_bit_size(c, in->base.type))));
      return true;
   }

   if (!strcmp(op, "imageblock_data")) {
      *handled = true;
      if (!c->imageblock_data_size || !c->imageblock_dim_var)
         return air_error(c, "intrinsic: air.imageblock_data without an air.imageblock input");
      if (in->num_operands < 2)
         return air_error(c, "intrinsic: air.imageblock_data without a coordinate");
      nir_def *coord = air_get_ssa(c, in->operands[1]);
      if (!coord || coord->num_components < 2)
         return air_error(c, "intrinsic: air.imageblock_data requires a 2D coordinate");
      nir_def *c32 = nir_u2u32(&c->b, coord);
      nir_def *ib_w = nir_channel(&c->b, nir_load_var(&c->b, c->imageblock_dim_var), 0);
      nir_def *idx = nir_iadd(&c->b,
                              nir_imul(&c->b, nir_channel(&c->b, c32, 1), ib_w),
                              nir_channel(&c->b, c32, 0));
      nir_def *off = nir_iadd_imm(&c->b,
                                  nir_imul_imm(&c->b, idx, c->imageblock_data_size),
                                  c->imageblock_base);
      air_set(c, &in->base,
              air_val_ptr(c, (struct air_ptr){ .class = AIR_PTR_SHARED, .offset = off }));
      return true;
   }

   bool is_2d = !strcmp(op, "write_imageblock_slice_to_texture_2d");
   bool is_2d_array = !strcmp(op, "write_imageblock_slice_to_texture_2d_array");
   if (!is_2d && !is_2d_array) {
      *handled = false;
      return true;
   }
   *handled = true;

   if (!c->imageblock_data_size || !c->imageblock_dim_var)
      return air_error(c, "intrinsic: air.%s without an air.imageblock input", op);
   unsigned want_ops = is_2d_array ? 11 : 10;
   if (in->num_operands != want_ops)
      return air_error(c, "intrinsic: air.%s of %u arguments (expected %u)",
                       op, in->num_operands - 1, want_ops - 1);

   unsigned comps = 0, bits = 0;
   bool is_fp = false;
   if (n->num < 2 || !parse_imageblock_slice_elem(n->part[n->num - 1], &comps, &bits, &is_fp))
      return air_error(c, "intrinsic: air.%s has unrecognized slice element %s",
                       op, n->part[n->num - 1]);

   struct air_ptr tex;
   if (!tex_handle(c, in, 1, AIR_PTR_TEXTURE, &tex))
      return false;
   if (tex.is_null)
      return true;
   if (!tex.is_image)
      return air_error(c, "intrinsic: air.%s to a texture not declared writable", op);

   struct air_val *slice_val = air_get(c, in->operands[2]);
   if (!slice_val || slice_val->kind != AIR_VAL_PTR ||
       slice_val->ptr.class != AIR_PTR_SHARED)
      return air_error(c, "intrinsic: air.%s slice argument is not an imageblock pointer", op);

   nir_def *has_region = air_get_ssa(c, in->operands[3]);
   nir_def *ib_origin = air_get_ssa(c, in->operands[4]);
   nir_def *region_size = air_get_ssa(c, in->operands[5]);
   nir_def *dst_origin = air_get_ssa(c, in->operands[6]);
   if (!has_region || !ib_origin || !region_size || !dst_origin)
      return false;

   nir_def *layer32 = nir_imm_int(&c->b, 0);
   unsigned lod_arg = 7;
   if (is_2d_array) {
      nir_def *layer = air_get_ssa(c, in->operands[7]);
      if (!layer)
         return false;
      layer32 = nir_u2u32(&c->b, layer);
      lod_arg = 8;
   }

   uint64_t level;
   nir_def *index;
   if (air_const_uint(in->operands[lod_arg], &level) && level == 0) {
      index = nir_imm_int(&c->b, tex.binding);
   } else {
      nir_def *lod = air_get_ssa(c, in->operands[lod_arg]);
      if (!lod || !(index = image_level_slot(c, &tex, lod, n)))
         return false;
   }

   nir_def *slice_off = slice_val->ptr.offset ? slice_val->ptr.offset
                                              : nir_imm_int(&c->b, c->imageblock_base);
   nir_def *field_off = nir_umod_imm(&c->b,
                                     nir_isub(&c->b, slice_off,
                                              nir_imm_int(&c->b, c->imageblock_base)),
                                     c->imageblock_data_size);
   nir_def *ib_dim = nir_load_var(&c->b, c->imageblock_dim_var);
   nir_def *cond2 = nir_replicate(&c->b, has_region, 2);
   nir_def *orig = nir_bcsel(&c->b, cond2, nir_u2u32(&c->b, ib_origin),
                             nir_imm_ivec2(&c->b, 0, 0));
   nir_def *ext = nir_bcsel(&c->b, cond2, nir_u2u32(&c->b, region_size), ib_dim);
   nir_def *dst0 = nir_u2u32(&c->b, dst_origin);

   nir_alu_type src_type = is_fp ? (nir_type_float | bits) :
      ((image_slot_is_signed_int(c, tex.binding, n) ? nir_type_int : nir_type_uint) | bits);

   /* Emit the 2D slice write loop inside a structured internal callee so the
    * caller's unstructured CFG block and phi_cursors remain untouched;
    * air_entry.inc inlines and removes internal callees after nir_lower_goto_ifs.
    */
   char fn_name[64];
   snprintf(fn_name, sizeof(fn_name), "__air_ib_write_%u", c->num_ib_helpers++);
   nir_function *helper = nir_function_create(c->nir, fn_name);
   helper->num_params = 6;
   helper->params = ralloc_array(c->nir, nir_parameter, 6);
   static const uint8_t param_comps[6] = { 1, 1, 2, 2, 2, 1 };
   for (unsigned i = 0; i < 6; i++)
      helper->params[i] = (nir_parameter){ .num_components = param_comps[i], .bit_size = 32 };
   _mesa_hash_table_insert(c->fns, helper, helper);

   nir_builder hb = nir_builder_at(nir_before_impl(nir_function_impl_create(helper)));
   nir_def *p_index = nir_load_param(&hb, 0);
   nir_def *p_field = nir_load_param(&hb, 1);
   nir_def *p_orig = nir_load_param(&hb, 2);
   nir_def *p_ext = nir_load_param(&hb, 3);
   nir_def *p_dst0 = nir_load_param(&hb, 4);
   nir_def *p_layer = nir_load_param(&hb, 5);
   nir_def *h_ib_w = nir_channel(&hb, nir_load_var(&hb, c->imageblock_dim_var), 0);

   nir_variable *vy = nir_local_variable_create(hb.impl, glsl_uint_type(), "ib_y");
   nir_variable *vx = nir_local_variable_create(hb.impl, glsl_uint_type(), "ib_x");
   nir_store_var(&hb, vy, nir_imm_int(&hb, 0), 0x1);
   nir_loop *ly = nir_push_loop(&hb);
   {
      nir_def *y = nir_load_var(&hb, vy);
      nir_break_if(&hb, nir_uge(&hb, y, nir_channel(&hb, p_ext, 1)));
      nir_store_var(&hb, vx, nir_imm_int(&hb, 0), 0x1);
      nir_loop *lx = nir_push_loop(&hb);
      {
         nir_def *x = nir_load_var(&hb, vx);
         nir_break_if(&hb, nir_uge(&hb, x, nir_channel(&hb, p_ext, 0)));
         nir_def *sx = nir_iadd(&hb, nir_channel(&hb, p_orig, 0), x);
         nir_def *sy = nir_iadd(&hb, nir_channel(&hb, p_orig, 1), y);
         nir_def *t_idx = nir_iadd(&hb, nir_imul(&hb, sy, h_ib_w), sx);
         nir_def *byte_off = nir_iadd(&hb,
            nir_iadd_imm(&hb, nir_imul_imm(&hb, t_idx, c->imageblock_data_size),
                         c->imageblock_base),
            p_field);
         nir_def *val = nir_load_shared(&hb, comps, bits, byte_off,
                                        .align_mul = MAX2(bits / 8, 1));
         nir_def *dx = nir_iadd(&hb, nir_channel(&hb, p_dst0, 0), x);
         nir_def *dy = nir_iadd(&hb, nir_channel(&hb, p_dst0, 1), y);
         nir_def *coord4 = nir_vec4(&hb, dx, dy,
                                    is_2d_array ? p_layer : nir_imm_int(&hb, 0),
                                    nir_imm_int(&hb, 0));
         nir_image_store(&hb, p_index, coord4, nir_imm_int(&hb, 0),
                         nir_pad_vector(&hb, val, 4), nir_imm_int(&hb, 0),
                         .image_dim = GLSL_SAMPLER_DIM_2D, .image_array = is_2d_array,
                         .src_type = src_type);
         nir_store_var(&hb, vx, nir_iadd_imm(&hb, x, 1), 0x1);
      }
      nir_pop_loop(&hb, lx);
      nir_store_var(&hb, vy, nir_iadd_imm(&hb, y, 1), 0x1);
   }
   nir_pop_loop(&hb, ly);

   nir_call_instr *call = nir_call_instr_create(c->nir, helper);
   call->params[0] = nir_src_for_ssa(index);
   call->params[1] = nir_src_for_ssa(field_off);
   call->params[2] = nir_src_for_ssa(orig);
   call->params[3] = nir_src_for_ssa(ext);
   call->params[4] = nir_src_for_ssa(dst0);
   call->params[5] = nir_src_for_ssa(layer32);
   nir_builder_instr_insert(&c->b, &call->instr);
   return true;
}
