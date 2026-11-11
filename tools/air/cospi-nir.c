/* Evaluate the production NIR helper against Darwin libm's double cospi.
 * Every half encoding is covered; float samples include adjacent values at
 * integers/half-integers and deterministic raw bit patterns. */
#include "nir.h"
#include "nir_builder.h"
#include "util/u_math.h"
#include "util/half_float.h"
#include "air_math_private.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT (65536 + 16384)
#define BATCH 256

int main(int argc, char **argv)
{
   if (argc != 2) return 2;
   FILE *output = fopen(argv[1], "wb");
   if (!output) return 2;
   glsl_type_singleton_init_or_ref();
   uint32_t random = 0x9635a174;
   double maximum[2] = {0};
   for (unsigned start = 0; start < COUNT; start += BATCH) {
      nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL, "cospi");
      nir_intrinsic_instr *stores[BATCH];
      float inputs[BATCH];
      for (unsigned i = 0; i < BATCH; i++) {
         unsigned index = start + i;
         unsigned bits = index < 65536 ? 16 : 32;
         float x;
         if (bits == 16) {
            x = _mesa_half_to_float(index);
         } else if (index < 65536 + 1024) {
            float center = ((int)(index - 65536) / 4 - 128) * 0.5f;
            x = index % 4 == 0 ? center : nextafterf(center, index % 4 == 1 ? INFINITY : -INFINITY);
         } else {
            random = random * 1664525u + 1013904223u;
            memcpy(&x, &random, sizeof(x));
         }
         inputs[i] = x;
         nir_def *arg = nir_imm_floatN_t(&b, x, bits);
         nir_def *result = nir_f2f32(&b, air_cospi(&b, arg));
         stores[i] = nir_store_ssbo(&b, result, nir_imm_int(&b, 0), nir_imm_int(&b, i * 4),
                                    .write_mask = 1, .align_mul = 4);
      }
      nir_validate_shader(b.shader, "cospi test");
      nir_opt_constant_folding(b.shader);
      for (unsigned i = 0; i < BATCH; i++) {
         if (!nir_src_is_const(stores[i]->src[0])) return 3;
         float got = nir_src_as_float(stores[i]->src[0]), x = inputs[i];
         double expected = __cospi((double)x);
         unsigned half = start < 65536;
         int exponent = 0;
         if (isfinite(expected)) frexp(expected, &exponent);
         double ulp = ldexp(1.0, MAX2(exponent - (half ? 11 : 24), half ? -24 : -149));
         double error = fabs(got - expected) / ulp;
         bool ok = isnan(expected) ? isnan(got) : isfinite(got) && error <= (half ? 1 : 4);
         if (expected == 0) ok = got == 0 && !signbit(got);
         if (!ok) {
            fprintf(stderr, "cospi bits=%u input=%a got=%a expected=%a error=%g ULP\n",
                    half ? 16 : 32, x, got, expected, error);
            return 1;
         }
         if (isfinite(error)) maximum[half ? 0 : 1] = MAX2(maximum[half ? 0 : 1], error);
         float pair[] = {x, got};
         if (fwrite(pair, sizeof(pair), 1, output) != 1) return 2;
      }
      ralloc_free(b.shader);
   }
   fclose(output);
   glsl_type_singleton_decref();
   printf("PASS %u values; maximum error half=%g ULP float=%g ULP\n", COUNT, maximum[0], maximum[1]);
   return 0;
}
