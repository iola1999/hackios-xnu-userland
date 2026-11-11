/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

static bool
gep_into(struct air_ctx *c, struct air_val *base, const struct air_inst *in,
         struct air_val **out)
{
   /* A getelementptr on a pointer CHOSEN at run time: the index goes into both
    * sides and the choice stays. Both sides have the same pointee type -- the
    * select that made the choice required it -- so this is one walk per side
    * and not a different offset on each.
    */
   if (base->kind == AIR_VAL_PTR_SELECT) {
      struct air_val *a = NULL, *b = NULL;
      if (!gep_into(c, base->elems[0], in, &a) ||
          !gep_into(c, base->elems[1], in, &b))
         return false;
      *out = air_val_ptr_select(c, base->def, a, b);
      return true;
   }
   if (base->kind != AIR_VAL_PTR)
      return air_error(c, "instruction: getelementptr on something that is not a "
                          "pointer");

   struct air_ptr p = base->ptr;
   const struct air_type *t = in->aux_type;

   /* index 0 steps over objects of the source type */
   uint64_t stride = air_type_alloc_size(c->m, t);
   uint64_t first_const;
   if (air_const_uint(in->operands[1], &first_const)) {
      if (first_const)
         p = air_ptr_add(c, p, nir_imm_int(&c->b, first_const * stride));
   } else {
      nir_def *first = air_get_ssa(c, in->operands[1]);
      if (!first)
         return false;
      p = air_ptr_add(c, p, nir_imul_imm(&c->b, nir_u2u32(&c->b, first), stride));
   }

   for (unsigned i = 2; i < in->num_operands; i++) {
      nir_def *idx = air_get_ssa(c, in->operands[i]);
      if (!idx)
         return false;
      if (t->kind == AIR_TYPE_STRUCT) {
         uint64_t field;
         if (!air_const_uint(in->operands[i], &field))
            return air_error(c, "instruction: a struct field chosen at run time");
         unsigned n = field;
         if (n >= t->num_members)
            return air_error(c, "instruction: struct field %u of %u", n, t->num_members);
         uint64_t off = air_struct_member_offset(c->m, t, n);
         if (off)
            p = air_ptr_add(c, p, nir_imm_int(&c->b, off));
         t = t->members[n];
      } else if (t->kind == AIR_TYPE_ARRAY || t->kind == AIR_TYPE_VECTOR) {
         uint64_t es = t->kind == AIR_TYPE_ARRAY ? air_type_alloc_size(c->m, t->elem)
                                                 : MAX2(air_type_bit_size(t->elem) / 8, 1);
         uint64_t element;
         nir_def *bytes = air_const_uint(in->operands[i], &element)
            ? nir_imm_int(&c->b, element * es)
            : nir_imul_imm(&c->b, nir_u2u32(&c->b, idx), es);
         p = air_ptr_add(c, p, bytes);
         t = t->elem;
      } else {
         return air_error(c, "instruction: getelementptr into a scalar");
      }
   }
   p.pointee = t;
   *out = air_val_ptr(c, p);
   return true;
}
