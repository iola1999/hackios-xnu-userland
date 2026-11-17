/* Private llvm.* lowering, included by air_intrinsics.c. */
/* -- llvm.* ------------------------------------------------------------- */

static bool
llvm_op(struct air_ctx *c, const struct air_inst *in, const char *name)
{
   if (!strncmp(name, "llvm.lifetime.", 14) || !strcmp(name, "llvm.assume"))
      return true; /* scope markers and optimizer hints: nothing to emit */
   if (!strncmp(name, "llvm.memcpy.", 12)) {
      /* a copy between two of our pointers, of a constant length */
      struct air_val *dst = air_get(c, in->operands[1]);
      struct air_val *src = air_get(c, in->operands[2]);
      if (!dst || !src)
         return false;
      if (dst->kind != AIR_VAL_PTR || src->kind != AIR_VAL_PTR)
         return air_error(c, "intrinsic: memcpy that is not between pointers");
      uint64_t bytes;
      if (!air_const_uint(in->operands[3], &bytes))
         return air_error(c, "intrinsic: memcpy of a run-time length");
      if (src->ptr.class == AIR_PTR_TEXTURE_ARRAY && dst->ptr.class == AIR_PTR_SCRATCH &&
          (bytes % 8) == 0 && (!src->ptr.offset || nir_src_is_const(nir_src_for_ssa(src->ptr.offset)))) {
         uint64_t src_off = src->ptr.offset ? nir_src_as_uint(nir_src_for_ssa(src->ptr.offset)) : 0;
         for (uint64_t off = 0; off < bytes; off += 8) {
            unsigned idx = (unsigned)((src_off + off) >> 3);
            if (idx >= src->ptr.span)
               idx = src->ptr.span - 1;
            struct air_ptr tp = {
               .class = AIR_PTR_TEXTURE,
               .binding = src->ptr.binding + idx,
               .is_image = src->ptr.is_image,
               .pointee = src->ptr.pointee,
            };
            struct air_val *at = air_val_ptr(c, air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off)));
            if (!air_store_pointer_slot(c, at, air_val_ptr(c, tp)))
               return false;
         }
         return true;
      }
      uint64_t off = 0;
      for (; off + 4 <= bytes; off += 4) {
         static const struct air_type u32 = { .kind = AIR_TYPE_INT, .bits = 32 };
         nir_def *v = air_load_ptr(c, air_ptr_add(c, src->ptr, nir_imm_int(&c->b, off)), &u32, 4);
         if (!v || !air_store_ptr(c, air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off)), v, 4))
            return false;
      }
      for (; off + 2 <= bytes; off += 2) {
         static const struct air_type u16 = { .kind = AIR_TYPE_INT, .bits = 16 };
         nir_def *v = air_load_ptr(c, air_ptr_add(c, src->ptr, nir_imm_int(&c->b, off)), &u16, 2);
         if (!v || !air_store_ptr(c, air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off)), v, 2))
            return false;
      }
      for (; off < bytes; off += 1) {
         static const struct air_type u8 = { .kind = AIR_TYPE_INT, .bits = 8 };
         nir_def *v = air_load_ptr(c, air_ptr_add(c, src->ptr, nir_imm_int(&c->b, off)), &u8, 1);
         if (!v || !air_store_ptr(c, air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off)), v, 1))
            return false;
      }
      return true;
   }
   if (!strncmp(name, "llvm.memset.", 12)) {
      struct air_val *dst = air_get(c, in->operands[1]);
      nir_def *byte = air_get_ssa(c, in->operands[2]);
      if (!dst || !byte)
         return false;
      if (dst->kind != AIR_VAL_PTR)
         return air_error(c, "intrinsic: memset of something that is not a pointer");
      uint64_t bytes;
      if (!air_const_uint(in->operands[3], &bytes))
         return air_error(c, "intrinsic: memset of a run-time length");
      if (!nir_src_is_const(nir_src_for_ssa(byte)))
         return air_error(c, "intrinsic: memset of a run-time byte");
      uint32_t b = nir_src_as_uint(nir_src_for_ssa(byte)) & 0xff;
      uint32_t word = b | b << 8 | b << 16 | b << 24;
      uint64_t off = 0;
      for (; off + 4 <= bytes; off += 4) {
         struct air_ptr d = air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off));
         if (!air_store_ptr(c, d, nir_imm_int(&c->b, word), 4))
            return false;
      }
      for (; off + 2 <= bytes; off += 2) {
         struct air_ptr d = air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off));
         if (!air_store_ptr(c, d, nir_imm_intN_t(&c->b, b | (b << 8), 16), 2))
            return false;
      }
      for (; off < bytes; off += 1) {
         struct air_ptr d = air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off));
         if (!air_store_ptr(c, d, nir_imm_intN_t(&c->b, b, 8), 1))
            return false;
      }
      return true;
   }
   /* llvm.fmuladd(a, b, c): a multiply-add that MAY be fused.
    *
    * Translated as a fused one -- nir_ffma, a single rounding -- and the
    * licence for that is the intrinsic's own definition rather than the
    * module's fast-math flag. LLVM's Language Reference says of llvm.fmuladd
    * that the expression "is equivalent to a * b + c, except that it is
    * unspecified whether rounding will be performed between the
    * multiplication and addition steps", and that a caller who needs the
    * fusion guaranteed must use llvm.fma instead. A producer that emitted
    * llvm.fmuladd rather than `fmul` + `fadd` therefore accepted both answers
    * when it chose the name, and the choice between them is the back end's.
    *
    * SO THE ABSENCE OF air.compile.fast_math_enable CHANGES NOTHING HERE, and
    * that is worth saying because the measurement invites the opposite
    * reading: 383 of a 394-module sample of Resident Evil 4 carry the flag, so
    * contraction is permitted under the stricter reading too -- but the eleven
    * that do not are not a different case for THIS intrinsic. A module that
    * wanted two roundings would carry two instructions.
    *
    * Which answer the Adreno gives is then ir3's: nir_ffma is one rounding,
    * and a backend that has no fused multiply-add would lower it. softpipe
    * does not have one -- TGSI's own specification lets MAD round the product
    * first and its interpreter does -- which is why tools/air/intrincheck.py
    * sets air.fma aside on softpipe and leaves it to the phone. The same
    * applies to every llvm.fmuladd this translates.
    */
   if (!strncmp(name, "llvm.fmuladd.", 13)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      nir_def *b = air_get_ssa(c, in->operands[2]);
      nir_def *d = air_get_ssa(c, in->operands[3]);
      if (!a || !b || !d)
         return false;
      air_set(c, &in->base, air_val_ssa(c, nir_ffma(&c->b, a, b, d)));
      return true;
   }
   if (!strncmp(name, "llvm.fabs.", 10) || !strncmp(name, "llvm.abs.", 9)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      if (!a)
         return false;
      air_set(c, &in->base,
              air_val_ssa(c, name[5] == 'f' ? nir_fabs(&c->b, a) : nir_iabs(&c->b, a)));
      return true;
   }
   /* NIR's min/max return the numeric operand for a single NaN and retain
    * the input element width and vector shape. */
   if (!strncmp(name, "llvm.minnum.", 12) || !strncmp(name, "llvm.maxnum.", 12)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      nir_def *b = air_get_ssa(c, in->operands[2]);
      if (!a || !b)
         return false;
      nir_def *r = name[6] == 'a' ? nir_fmax(&c->b, a, b) : nir_fmin(&c->b, a, b);
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }
   /* LLVM's integer minimum and maximum, which the Metal compiler emits for
    * min()/max() of shorts (space::rasterizeArcFragment: llvm.smax.i16). */
   if (!strncmp(name, "llvm.smax.", 10) || !strncmp(name, "llvm.smin.", 10) ||
       !strncmp(name, "llvm.umax.", 10) || !strncmp(name, "llvm.umin.", 10)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      nir_def *b = air_get_ssa(c, in->operands[2]);
      if (!a || !b)
         return false;
      bool max = name[7] == 'a', is_signed = name[5] == 's';
      nir_def *r = is_signed ? (max ? nir_imax(&c->b, a, b) : nir_imin(&c->b, a, b))
                             : (max ? nir_umax(&c->b, a, b) : nir_umin(&c->b, a, b));
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }
   if (!strncmp(name, "llvm.uadd.sat.", 14) || !strncmp(name, "llvm.usub.sat.", 14) ||
       !strncmp(name, "llvm.sadd.sat.", 14) || !strncmp(name, "llvm.ssub.sat.", 14)) {
      nir_def *a = air_get_ssa(c, in->operands[1]);
      nir_def *b = air_get_ssa(c, in->operands[2]);
      if (!a || !b)
         return false;
      bool add = name[6] == 'a', is_signed = name[5] == 's';
      nir_def *r = is_signed ? (add ? nir_iadd_sat(&c->b, a, b) : nir_isub_sat(&c->b, a, b))
                             : (add ? nir_uadd_sat(&c->b, a, b) : nir_usub_sat(&c->b, a, b));
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }
   if (!strcmp(name, "llvm.trap"))
      return air_error(c, "intrinsic: llvm.trap");
   return air_error(c, "intrinsic: %s", name);
}

