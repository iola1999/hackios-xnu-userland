/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nir.h"
#include "nir_air.h"
#include "util/ralloc.h"

static void *read_module(const char *dir, const char *name, size_t *size)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s.bc", dir, name);
   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); exit(2); }
   fseek(f, 0, SEEK_END); *size = ftell(f); rewind(f);
   void *bytes = malloc(*size);
   if (fread(bytes, 1, *size, f) != *size) exit(2);
   fclose(f);
   return bytes;
}

static int run(const char *dir, bool is_void)
{
   size_t size, caller_size;
   void *bytes = read_module(dir, is_void ? "store" : "twice", &size);
   void *caller = read_module(dir, is_void ? "call_void" : "call_value", &caller_size);
   const int args[] = {0, 1, 2};
   struct air_stitch_node node = {.bitcode = bytes, .size = size,
      .function = is_void ? "store" : "twice", .num_args = is_void ? 3 : 1, .args = args};
   struct air_linked_function linked = {.name = is_void ? "linked_void" : "linked_value",
      .num_nodes = 1, .nodes = &node, .output = is_void ? ~0u : 0};
   struct air_to_nir_options options = {.linked = &linked, .num_linked = 1};
   const nir_shader_compiler_options nopts = {0};
   void *ctx = ralloc_context(NULL);
   const char *error = NULL;
   nir_shader *nir = air_to_nir(ctx, caller, caller_size, NULL, &options, &nopts, NULL, &error);
   int failures = 0;
   if (!nir) {
      fprintf(stderr, "FAIL visible %s: %s\n", is_void ? "void" : "value", error);
      failures++;
      goto done;
   }
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, NULL, NULL);
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_dce);
   } while (progress);
   unsigned stores = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != (is_void ? nir_intrinsic_image_store : nir_intrinsic_store_ssbo))
               continue;
            unsigned source = is_void ? 3 : 0;
            if (!nir_src_is_const(in->src[source])) { failures++; continue; }
            unsigned components = is_void ? 4 : 1;
            for (unsigned c = 0; c < components; c++) {
               union { uint32_t u; float f; } v = {.u = nir_src_comp_as_uint(in->src[source], c)};
               if (v.f != (is_void ? 1.5f : 6.5f)) failures++;
            }
            stores++;
         }
      }
   }
   if (stores != 1) failures++;
   nir_validate_shader(nir, "stitch visible regression");
   printf("%s visible %s numeric output and side effect\n", failures ? "FAIL" : "PASS",
          is_void ? "void" : "value");
done:
   ralloc_free(ctx); free(bytes); free(caller);
   return failures;
}

static int sampler_bindings(const char *dir)
{
   size_t size;
   void *bytes = read_module(dir, "read_mixed", &size);
   const int args[] = {0, 1};
   struct air_stitch_node node = {.bitcode = bytes, .size = size, .function = "read_mixed",
      .num_args = 2, .args = args};
   const struct air_stitch_input inputs[] = {
      {.kind = AIR_STITCH_TEXTURE, .binding = 0},
      {.kind = AIR_STITCH_SAMPLER, .binding = 0},
   };
   struct air_stitched_kernel kernel = {
      .graph = {.name = "mixed", .num_nodes = 1, .nodes = &node, .output = ~0u},
      .num_inputs = 2, .inputs = inputs,
   };
   struct air_to_nir_options options = {.kernel = &kernel};
   const nir_shader_compiler_options nopts = {0};
   void *ctx = ralloc_context(NULL);
   struct air_shader_info info;
   const char *error;
   nir_shader *nir = air_to_nir(ctx, NULL, 0, NULL, &options, &nopts, &info, &error);
   unsigned api = 0, local = 0;
   if (nir) {
      for (unsigned i = 0; i < info.num_resources; i++) {
         const struct air_resource *r = &info.resources[i];
         if (r->kind != AIR_RESOURCE_SAMPLER) continue;
         api += r->index == 0 && !r->inline_sampler;
         local += r->index == 1 && r->inline_sampler;
      }
   } else {
      fprintf(stderr, "FAIL sampler graph: %s\n", error);
   }
   int failed = api != 1 || local != 1;
   printf("%s API sampler zero and constexpr sampler remain separate\n", failed ? "FAIL" : "PASS");
   ralloc_free(ctx); free(bytes);
   return failed;
}

int main(int argc, char **argv)
{
   if (argc != 2) return 2;
   glsl_type_singleton_init_or_ref();
   int errors = run(argv[1], false) + run(argv[1], true) + sampler_bindings(argv[1]);
   glsl_type_singleton_decref();
   return errors ? 1 : 0;
}
