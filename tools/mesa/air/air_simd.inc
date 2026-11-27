/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 *
 * Metal SIMD-group and quad-group operations, included by air_intrinsics.c.
 */

/* Mesa 26.2.5 ir3 does not normalize 1-bit subgroup shuffle/broadcast results from
 * inactive source lanes (upstream 9b0a38ca). Widen 1-bit booleans to 32-bit before
 * shuffling and mask bit 0 afterward so nir_opt_algebraic preserves the normalization.
 */
static nir_def *
air_simd_shuffle(nir_builder *b, nir_def *val, nir_def *lane)
{
   if (val->bit_size == 1) {
      nir_def *wide = nir_shuffle(b, nir_b2i32(b, val), lane);
      return nir_ine_imm(b, nir_iand_imm(b, wide, 1), 0);
   }
   return nir_shuffle(b, val, lane);
}

static nir_def *
air_quad_broadcast(nir_builder *b, nir_def *val, nir_def *lane)
{
   if (val->bit_size == 1) {
      nir_def *wide = nir_quad_broadcast(b, nir_b2i32(b, val), lane);
      return nir_ine_imm(b, nir_iand_imm(b, wide, 1), 0);
   }
   return nir_quad_broadcast(b, val, lane);
}

static nir_def *
quad_reduce_pair(nir_builder *b, const char *op, bool is_float, bool is_signed,
                 nir_def *x, nir_def *y)
{
   if (!strcmp(op, "quad_sum"))
      return is_float ? nir_fadd(b, x, y) : nir_iadd(b, x, y);
   if (!strcmp(op, "quad_product"))
      return is_float ? nir_fmul(b, x, y) : nir_imul(b, x, y);
   if (!strcmp(op, "quad_min"))
      return is_float ? nir_fmin(b, x, y) : (is_signed ? nir_imin(b, x, y) : nir_umin(b, x, y));
   if (!strcmp(op, "quad_max"))
      return is_float ? nir_fmax(b, x, y) : (is_signed ? nir_imax(b, x, y) : nir_umax(b, x, y));
   if (!strcmp(op, "quad_and"))
      return nir_iand(b, x, y);
   if (!strcmp(op, "quad_or"))
      return nir_ior(b, x, y);
   return nir_ixor(b, x, y);
}

static bool
simd_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n,
        bool *handled)
{
   const char *op = n->part[0];

   if (strncmp(op, "simd_", 5) && strncmp(op, "quad_", 5) &&
       strcmp(op, "is_uniform") && strcmp(op, "get_sample_position") &&
       strcmp(op, "get_simdgroup_size") && strcmp(op, "get_simd_width") &&
       strcmp(op, "get_simdgroup_index_in_threadgroup") &&
       strcmp(op, "get_simdgroups_per_threadgroup") &&
       strcmp(op, "get_thread_index_in_simdgroup") &&
       strcmp(op, "get_thread_index_in_threadgroup") &&
       strcmp(op, "get_group_id") && strcmp(op, "get_local_id") &&
       strcmp(op, "get_global_id") && strcmp(op, "get_num_groups") &&
       strcmp(op, "get_global_size")) {
      *handled = false;
      return true;
   }
   *handled = true;

   if (!strcmp(op, "get_simdgroup_size") || !strcmp(op, "get_simd_width")) {
      unsigned bits = air_nir_bit_size(c, in->base.type);
      air_set(c, &in->base,
              air_val_ssa(c, nir_u2uN(&c->b, nir_load_subgroup_size(&c->b), bits)));
      return true;
   }
   if (!strcmp(op, "get_simdgroup_index_in_threadgroup")) {
      unsigned bits = air_nir_bit_size(c, in->base.type);
      air_set(c, &in->base,
              air_val_ssa(c, nir_u2uN(&c->b, nir_load_subgroup_id(&c->b), bits)));
      return true;
   }
   if (!strcmp(op, "get_simdgroups_per_threadgroup")) {
      unsigned bits = air_nir_bit_size(c, in->base.type);
      air_set(c, &in->base,
              air_val_ssa(c, nir_u2uN(&c->b, nir_load_num_subgroups(&c->b), bits)));
      return true;
   }
   if (!strcmp(op, "get_thread_index_in_simdgroup")) {
      unsigned bits = air_nir_bit_size(c, in->base.type);
      air_set(c, &in->base,
              air_val_ssa(c, nir_u2uN(&c->b, nir_load_subgroup_invocation(&c->b), bits)));
      return true;
   }
   if (!strcmp(op, "get_thread_index_in_threadgroup")) {
      unsigned bits = air_nir_bit_size(c, in->base.type);
      air_set(c, &in->base,
              air_val_ssa(c, nir_u2uN(&c->b, nir_load_local_invocation_index(&c->b), bits)));
      return true;
   }
   if (!strcmp(op, "get_group_id") || !strcmp(op, "get_local_id") ||
       !strcmp(op, "get_global_id") || !strcmp(op, "get_num_groups") ||
       !strcmp(op, "get_global_size")) {
      if (in->num_operands < 2)
         return air_error(c, "intrinsic: air.%s requires a dimension", op);
      nir_def *dim = air_get_ssa(c, in->operands[1]);
      if (!dim)
         return false;
      nir_def *vec = !strcmp(op, "get_group_id")   ? nir_load_workgroup_id(&c->b) :
                     !strcmp(op, "get_local_id")   ? nir_load_local_invocation_id(&c->b) :
                     !strcmp(op, "get_global_id")  ? nir_load_global_invocation_id(&c->b, 32) :
                     !strcmp(op, "get_num_groups") ? nir_load_num_workgroups(&c->b) :
                     nir_imul(&c->b, nir_load_num_workgroups(&c->b), nir_load_workgroup_size(&c->b));
      nir_def *ch = nir_vector_extract(&c->b, vec, nir_umin_imm(&c->b, nir_u2u32(&c->b, dim), 2));
      air_set(c, &in->base,
              air_val_ssa(c, nir_u2uN(&c->b, ch, air_nir_bit_size(c, in->base.type))));
      return true;
   }

   if (!strcmp(op, "simd_is_helper_thread")) {
      if (c->nir->info.stage != MESA_SHADER_FRAGMENT)
         return air_error(c, "intrinsic: air.simd_is_helper_thread in a %s shader "
                             "(there are no helper threads outside the fragment "
                             "stage)", _mesa_shader_stage_to_string(c->nir->info.stage));
      air_set(c, &in->base, air_val_ssa(c, nir_is_helper_invocation(&c->b, 1)));
      return true;
   }

   if (!strcmp(op, "simd_is_first")) {
      air_note(c, "air.%s elects the first active lane of the hardware SIMD group", n->part[0]);
      air_set(c, &in->base, air_val_ssa(c, nir_elect(&c->b, 1)));
      return true;
   }

   if (!strcmp(op, "simd_ballot"))
      return air_error(c, "intrinsic: air.%s -- a 64-bit mask with one bit per lane "
                          "of the SIMD group, and ir3 runs this hardware's groups at "
                          "64 or 128 lanes (threadsize_base 64, doubled per variant), "
                          "so the mask this would produce cannot be known to hold "
                          "every lane; a truncated one makes popcount answer a "
                          "smaller number rather than an obviously wrong one",
                       n->part[0]);

   if (!strcmp(op, "get_sample_position"))
      return air_error(c, "intrinsic: air.%s -- NIR's sample position is an offset "
                          "from the pixel centre and Metal's convention is not "
                          "attested anywhere on this machine; the two differ by half "
                          "a pixel, so this is left to a measurement", n->part[0]);

   /* Let simd_sum, simd_min, simd_max, simd_product, simd_and, simd_or, simd_xor,
    * and simd_prefix_* fall through to math_op, which already decodes float/signedness.
    */
   if (!strcmp(op, "simd_sum") || !strcmp(op, "simd_min") || !strcmp(op, "simd_max") ||
       !strcmp(op, "simd_product") || !strcmp(op, "simd_and") || !strcmp(op, "simd_or") ||
       !strcmp(op, "simd_xor") || !strncmp(op, "simd_prefix_", 12)) {
      *handled = false;
      return true;
   }

   nir_def *a0 = in->num_operands > 1 ? air_get_ssa(c, in->operands[1]) : NULL;
   if (in->num_operands > 1 && !a0)
      return false;

   if (!strcmp(op, "is_uniform")) {
      if (!a0)
         return air_error(c, "intrinsic: air.is_uniform without an operand");
      bool cst = nir_src_is_const(nir_src_for_ssa(a0));
      air_set(c, &in->base, air_val_ssa(c, cst ? nir_imm_true(&c->b) : nir_imm_false(&c->b)));
      return true;
   }

   if (!strcmp(op, "simd_all") || !strcmp(op, "simd_any")) {
      bool all = !strcmp(op, "simd_all");
      air_note(c, "air.%s is taken over the SIMD group this implementation has, "
                  "which is not Metal's 32 lanes: ir3 runs 64 or 128 of them on this "
                  "hardware. air.get_simd_width answers with the same number, so a "
                  "shader that reads it is right and one with 32 built into it is not",
               n->part[0]);
      air_set(c, &in->base,
              air_val_ssa(c, all ? nir_vote_all(&c->b, 1, a0)
                                 : nir_vote_any(&c->b, 1, a0)));
      return true;
   }

   if (!strcmp(op, "simd_broadcast_first")) {
      air_note(c, "air.%s reads the first active lane of the SIMD group this "
                  "implementation has, which is not Metal's 32 lanes; the value is "
                  "the same one Metal would give wherever it is uniform across the "
                  "group, and another lane's where it is not", n->part[0]);
      air_set(c, &in->base, air_val_ssa(c, nir_read_first_invocation(&c->b, a0)));
      return true;
   }

   if (!strcmp(op, "simd_shuffle") || !strcmp(op, "simd_broadcast")) {
      nir_def *lane = in->num_operands > 2 ? air_get_ssa(c, in->operands[2]) : NULL;
      if (!lane)
         return false;
      air_note(c, "air.%s takes its lane number in the SIMD group this "
                  "implementation has, which is not Metal's 32 lanes", n->part[0]);
      air_set(c, &in->base,
              air_val_ssa(c, air_simd_shuffle(&c->b, a0, nir_u2u32(&c->b, lane))));
      return true;
   }

   if (!strcmp(op, "simd_shuffle_down") || !strcmp(op, "simd_shuffle_up") ||
       !strcmp(op, "simd_shuffle_xor")) {
      nir_def *arg = in->num_operands > 2 ? air_get_ssa(c, in->operands[2]) : NULL;
      if (!arg)
         return false;
      nir_def *id = nir_load_subgroup_invocation(&c->b);
      nir_def *d = nir_u2u32(&c->b, arg);
      nir_def *src_lane = !strcmp(op, "simd_shuffle_down") ? nir_iadd(&c->b, id, d) :
                          !strcmp(op, "simd_shuffle_up")   ? nir_isub(&c->b, id, d) :
                                                             nir_ixor(&c->b, id, d);
      air_note(c, "air.%s shifts across the hardware SIMD group", n->part[0]);
      air_set(c, &in->base, air_val_ssa(c, air_simd_shuffle(&c->b, a0, src_lane)));
      return true;
   }

   if (!strcmp(op, "simd_shuffle_and_fill_down") ||
       !strcmp(op, "simd_shuffle_and_fill_up")) {
      if (in->num_operands < 5)
         return air_error(c, "intrinsic: air.%s requires 4 arguments", n->part[0]);
      nir_def *fill = air_get_ssa(c, in->operands[2]);
      nir_def *delta = air_get_ssa(c, in->operands[3]);
      nir_def *mod = air_get_ssa(c, in->operands[4]);
      if (!fill || !delta || !mod)
         return false;
      nir_def *id = nir_load_subgroup_invocation(&c->b);
      nir_def *d = nir_u2u32(&c->b, delta);
      nir_def *m = nir_umax(&c->b, nir_u2u32(&c->b, mod), nir_imm_int(&c->b, 1));
      nir_def *sub = nir_umod(&c->b, id, m);
      nir_def *res;
      if (!strcmp(op, "simd_shuffle_and_fill_down")) {
         nir_def *in_data = nir_ult(&c->b, nir_iadd(&c->b, sub, d), m);
         nir_def *v_data = air_simd_shuffle(&c->b, a0, nir_iadd(&c->b, id, d));
         nir_def *v_fill = air_simd_shuffle(&c->b, fill, nir_isub(&c->b, nir_iadd(&c->b, id, d), m));
         res = nir_bcsel(&c->b, in_data, v_data, v_fill);
      } else {
         nir_def *in_data = nir_uge(&c->b, sub, d);
         nir_def *v_data = air_simd_shuffle(&c->b, a0, nir_isub(&c->b, id, d));
         nir_def *v_fill = air_simd_shuffle(&c->b, fill, nir_isub(&c->b, nir_iadd(&c->b, id, m), d));
         res = nir_bcsel(&c->b, in_data, v_data, v_fill);
      }
      air_set(c, &in->base, air_val_ssa(c, res));
      return true;
   }

   if (!strcmp(op, "quad_shuffle") || !strcmp(op, "quad_broadcast")) {
      nir_def *lane = in->num_operands > 2 ? air_get_ssa(c, in->operands[2]) : NULL;
      if (!lane)
         return false;
      nir_def *lane32 = nir_iand_imm(&c->b, nir_u2u32(&c->b, lane), 3);
      if (nir_src_is_const(nir_src_for_ssa(lane32))) {
         air_set(c, &in->base, air_val_ssa(c, air_quad_broadcast(&c->b, a0, lane32)));
         return true;
      }
      nir_def *q0 = air_quad_broadcast(&c->b, a0, nir_imm_int(&c->b, 0));
      nir_def *q1 = air_quad_broadcast(&c->b, a0, nir_imm_int(&c->b, 1));
      nir_def *q2 = air_quad_broadcast(&c->b, a0, nir_imm_int(&c->b, 2));
      nir_def *q3 = air_quad_broadcast(&c->b, a0, nir_imm_int(&c->b, 3));
      nir_def *r01 = nir_bcsel(&c->b, nir_ieq_imm(&c->b, lane32, 0), q0, q1);
      nir_def *r23 = nir_bcsel(&c->b, nir_ieq_imm(&c->b, lane32, 2), q2, q3);
      air_set(c, &in->base,
              air_val_ssa(c, nir_bcsel(&c->b, nir_ult_imm(&c->b, lane32, 2), r01, r23)));
      return true;
   }

   if (!strcmp(op, "quad_shuffle_xor")) {
      nir_def *mask = in->num_operands > 2 ? air_get_ssa(c, in->operands[2]) : NULL;
      if (!mask)
         return false;
      if (!nir_src_is_const(nir_src_for_ssa(mask)))
         return air_error(c, "intrinsic: air.%s with a mask that is not a constant",
                          n->part[0]);
      uint64_t m = nir_src_as_uint(nir_src_for_ssa(mask));
      switch (m) {
      case 0: air_set(c, &in->base, air_val_ssa(c, a0)); return true;
      case 1: air_set(c, &in->base, air_val_ssa(c, nir_quad_swap_horizontal(&c->b, a0))); return true;
      case 2: air_set(c, &in->base, air_val_ssa(c, nir_quad_swap_vertical(&c->b, a0))); return true;
      case 3: air_set(c, &in->base, air_val_ssa(c, nir_quad_swap_diagonal(&c->b, a0))); return true;
      default:
         return air_error(c, "intrinsic: air.%s with mask %u, which leaves the quad",
                          n->part[0], (unsigned)m);
      }
   }

   if (!strcmp(op, "quad_sum") || !strcmp(op, "quad_product") ||
       !strcmp(op, "quad_min") || !strcmp(op, "quad_max") ||
       !strcmp(op, "quad_and") || !strcmp(op, "quad_or") || !strcmp(op, "quad_xor")) {
      if (!a0)
         return air_error(c, "intrinsic: air.%s without an operand", op);
      bool is_float = air_type_is_float(air_type_scalar(in->base.type));
      bool is_signed = n->num >= 3 ? is_signed_part(n->part[1]) : true;
      nir_def *t = quad_reduce_pair(&c->b, op, is_float, is_signed, a0,
                                    nir_quad_swap_horizontal(&c->b, a0));
      nir_def *r = quad_reduce_pair(&c->b, op, is_float, is_signed, t,
                                    nir_quad_swap_vertical(&c->b, t));
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }

   *handled = false;
   return true;
}
