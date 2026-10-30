/* Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/* Decode the existing sampler state word. The AIR two-word form adds LOD
 * bias in word 1; only zero extension state is currently supported. */
struct air_sampler_state
air_decode_sampler_state(uint64_t w)
{
#define FIELD(lo, n) ((unsigned)((w >> (lo)) & ((1ull << (n)) - 1)))
   struct air_sampler_state s = {
      .address = { FIELD(0, 3), FIELD(3, 3), FIELD(6, 3) },
      .mag_filter = FIELD(9, 2),
      .min_filter = FIELD(11, 2),
      .mip_filter = FIELD(13, 2),
      .pixel_coord = FIELD(15, 1) != 0,
      .compare_func = FIELD(16, 4),
      .max_anisotropy = FIELD(20, 4) + 1,
      .lod_min = FIELD(24, 16),
      .lod_max = FIELD(40, 16),
      .border_color = FIELD(56, 2),
      .reduction = FIELD(58, 2),
   };
#undef FIELD
   return s;
}


static bool
inline_sampler_word(struct air_ctx *c, const struct air_global *g, uint64_t *word)
{
   const struct air_type *type = g->value_type;
   if (type->kind == AIR_TYPE_INT && type->bits == 64 &&
       g->init && air_const_uint(g->init, word))
      return true;

   const struct air_constant *value = air_value_as_constant(g->init);
   if (type->kind != AIR_TYPE_ARRAY || type->count != 2 ||
       type->elem->kind != AIR_TYPE_INT || type->elem->bits != 64 ||
       !value || value->num_elems != 2) {
      air_error(c, "interface: inline sampler requires constant i64 or [2 x i64] state");
      return false;
   }
   uint64_t words[2];
   for (unsigned i = 0; i < 2; i++) {
      if (value->ckind == AIR_CONST_DATA)
         words[i] = value->data[i];
      else if (value->ckind != AIR_CONST_AGGREGATE ||
               !air_const_uint(value->elems[i], &words[i])) {
         air_error(c, "interface: inline sampler has a non-integer state word");
         return false;
      }
   }
   /* Apple Metal's default bias emits zero here. A nonzero word can carry
    * LOD bias or further extension state; retaining only word 0 would omit it. */
   if (words[1] != 0) {
      air_error(c, "interface: inline sampler extension state is unsupported");
      return false;
   }
   *word = words[0];
   return true;
}
