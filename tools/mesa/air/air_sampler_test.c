/* Exercise the production inline-sampler parser without a GPU or bitcode
 * fixture. Real system modules are checked separately through air2nir. */
#include "air_module.h"
#include "nir_air.h"
#include <stdarg.h>
#include <stdio.h>

struct air_ctx { bool failed; };
static bool air_error(struct air_ctx *c, const char *format, ...)
{
   (void)format;
   c->failed = true;
   return false;
}
#include "air_sampler.inc"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "sampler:%d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void)
{
   const struct air_type i64 = { .kind = AIR_TYPE_INT, .bits = 64 };
   const struct air_type pair = { .kind = AIR_TYPE_ARRAY, .count = 2, .elem = &i64 };
   struct air_constant first = { .base = { .kind = AIR_VALUE_CONSTANT, .type = &i64 },
      .ckind = AIR_CONST_INT, .u = 34901797601020489ull };
   struct air_constant second = { .base = { .kind = AIR_VALUE_CONSTANT, .type = &i64 },
      .ckind = AIR_CONST_INT };
   struct air_value *elems[] = { &first.base, &second.base };
   struct air_constant array = { .base = { .kind = AIR_VALUE_CONSTANT, .type = &pair },
      .ckind = AIR_CONST_AGGREGATE, .num_elems = 2, .elems = elems };
   struct air_global global = { .value_type = &i64, .init = &first.base };
   struct air_ctx ctx = {0};
   uint64_t word = 0;
   CHECK(inline_sampler_word(&ctx, &global, &word) && word == first.u && !ctx.failed);
   global.value_type = &pair; global.init = &array.base;
   CHECK(inline_sampler_word(&ctx, &global, &word) && word == first.u);
   struct air_sampler_state state = air_decode_sampler_state(word);
   CHECK(state.address[0] == AIR_SAMPLER_CLAMP_TO_EDGE &&
         state.mag_filter == AIR_SAMPLER_LINEAR && state.min_filter == AIR_SAMPLER_LINEAR &&
         state.mip_filter == AIR_SAMPLER_MIP_NONE && !state.pixel_coord);
   uint64_t data[] = { first.u, 0 };
   array.ckind = AIR_CONST_DATA; array.data = data;
   CHECK(inline_sampler_word(&ctx, &global, &word) && word == first.u);
   for (unsigned bit = 0; bit < 64; bit++) {
      data[1] = 1ull << bit; ctx.failed = false;
      CHECK(!inline_sampler_word(&ctx, &global, &word) && ctx.failed);
   }
   data[1] = 0; array.num_elems = 1; ctx.failed = false;
   CHECK(!inline_sampler_word(&ctx, &global, &word) && ctx.failed);
   array.num_elems = 2; array.ckind = AIR_CONST_AGGREGATE;
   second.ckind = AIR_CONST_UNDEF; ctx.failed = false;
   CHECK(!inline_sampler_word(&ctx, &global, &word) && ctx.failed);
   global.init = NULL; ctx.failed = false;
   CHECK(!inline_sampler_word(&ctx, &global, &word) && ctx.failed);
   puts("PASS scalar/aggregate/data sampler state, 64 nonzero extension bits and malformed input refusal");
   return 0;
}
