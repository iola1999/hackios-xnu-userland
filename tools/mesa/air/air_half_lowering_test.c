/* Exercise the production NIR pass's rounding controls, rejection and backend
 * gate. Actual Gallium execution is covered separately by air_half_test.c. */
#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "nir_air.h"
#include "pipe/p_screen.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct rwa_device { bool software; struct pipe_screen *screen; };
static void say(char *why, size_t size, const char *format, ...)
{
   va_list args;
   va_start(args, format);
   vsnprintf(why, size, format, args);
   va_end(args);
}
#include "air_metal_lowering.inc"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "half_lowering:%d: %s\n", __LINE__, #c); return 1; } } while (0)
static const char *softpipe_name(struct pipe_screen *screen) { (void)screen; return "softpipe"; }

static void fold_constants(nir_shader *shader)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, shader, nir_opt_constant_folding);
      NIR_PASS(progress, shader, nir_opt_dce);
      NIR_PASS(progress, shader, nir_opt_if, 0);
      NIR_PASS(progress, shader, nir_opt_dead_cf);
      NIR_PASS(progress, shader, nir_opt_remove_phis);
   } while (progress);
}

static int rounding(struct rwa_device *device, bool rtz, bool integer, bool negative)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL, "half_rounding");
   b.shader->info.float_controls_execution_mode = rtz ? FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP16 : FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP16;
   const float values[] = { 1.00048828125f, 1.00146484375f, 65520.0f, 2049.0f, 2051.0f };
   const float nearest[] = { 1.0f, 1.001953125f, INFINITY, 2048.0f, 2052.0f };
   const float toward_zero[] = { 1.0f, 1.0009765625f, 65504.0f, 2048.0f, 2050.0f };
   nir_intrinsic_instr *stores[5];
   for (unsigned i = 0; i < 5; i++) {
      float input = negative ? -values[i] : values[i];
      nir_def *half;
      if (integer)
         half = negative ? nir_i2f16(&b, nir_imm_int(&b, (int32_t)input)) :
                           nir_u2f16(&b, nir_imm_int(&b, (uint32_t)input));
      else
         half = nir_f2f16(&b, nir_imm_float(&b, input));
      stores[i] = nir_store_ssbo(&b, nir_f2f32(&b, half), nir_imm_int(&b, 0), nir_imm_int(&b, i * 4),
                                .write_mask = 1, .align_mul = 4);
   }
   char why[512] = { 0 };
   CHECK(lower_half_for_backend(b.shader, device, NULL, why, sizeof(why)));
   fold_constants(b.shader);
   for (unsigned i = 0; i < 5; i++) {
      float expected = integer && i < 2 ? 1.0f : (rtz ? toward_zero[i] : nearest[i]);
      if (negative) expected = -expected;
      CHECK(nir_src_is_const(stores[i]->src[0]));
      float result = nir_src_as_float(stores[i]->src[0]);
      if (result != expected) fprintf(stderr, "rtz=%u integer=%u negative=%u case=%u got=%g expected=%g\n",
          rtz, integer, negative, i, result, expected);
      CHECK(result == expected);
   }
   ralloc_free(b.shader);
   return 0;
}

static int texture_case(struct rwa_device *device, unsigned variant, bool rtz, bool accepted)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL, "half_txf");
   b.shader->info.float_controls_execution_mode = rtz ? FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP16 : FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP16;
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, variant == 7 ? 3 : 2);
   tex->op = variant == 1 ? nir_texop_tex : nir_texop_txf;
   tex->sampler_dim = variant == 2 ? GLSL_SAMPLER_DIM_MS : GLSL_SAMPLER_DIM_2D;
   tex->is_array = variant == 3;
   tex->is_shadow = variant == 4;
   tex->is_sparse = variant == 5;
   tex->dest_type = variant == 6 ? nir_type_uint16 : nir_type_float16;
   tex->coord_components = 2;
   tex->src[0].src_type = nir_tex_src_coord;
   nir_def *coord = variant == 8 ? nir_vec2(&b, nir_imm_intN_t(&b, 0, 16), nir_imm_intN_t(&b, 0, 16)) :
                                  nir_imm_ivec2(&b, 0, 0);
   tex->src[0].src = nir_src_for_ssa(coord);
   tex->src[1].src_type = nir_tex_src_lod;
   tex->src[1].src = nir_src_for_ssa(nir_imm_int(&b, 0));
   if (variant == 7) {
      tex->src[2].src_type = nir_tex_src_offset;
      tex->src[2].src = nir_src_for_ssa(nir_imm_ivec2(&b, 1, 0));
   }
   nir_def_init(&tex->instr, &tex->def, 4, 16);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_intrinsic_instr *store = nir_store_ssbo(&b, nir_f2f32(&b, &tex->def),
      nir_imm_int(&b, 0), nir_imm_int(&b, 0), .write_mask = 15, .align_mul = 4);
   char why[512] = {0};
   CHECK(lower_half_for_backend(b.shader, device, NULL, why, sizeof(why)) == accepted);
   if (!accepted || !device->software || device->screen->shader_caps[MESA_SHADER_COMPUTE].fp16) {
      CHECK(tex->def.bit_size == 16);
      ralloc_free(b.shader);
      return 0;
   }
   CHECK(tex->def.bit_size == 32 && tex->dest_type == nir_type_float32);
   /* Replace the fetch result to check the production pass's conversion at half
    * midpoints and overflow independently of a texture-format implementation. */
   b.cursor = nir_before_instr(&tex->instr);
   nir_def_replace(&tex->def, nir_imm_vec4(&b, 1.00048828125f, 1.00146484375f,
                                         -1.00048828125f, 65520.0f));
   fold_constants(b.shader);
   const float nearest[] = {1, 1.001953125f, -1, INFINITY};
   const float toward_zero[] = {1, 1.0009765625f, -1, 65504};
   CHECK(nir_src_is_const(store->src[0]));
   nir_const_value *values = nir_src_as_const_value(store->src[0]);
   for (unsigned i = 0; i < 4; i++) CHECK(values[i].f32 == (rtz ? toward_zero[i] : nearest[i]));
   ralloc_free(b.shader);
   return 0;
}

static int stage_io_case(struct rwa_device *device, bool rtz, unsigned variant)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, NULL, "half_stage_io");
   b.shader->info.float_controls_execution_mode = rtz ? FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP16 : FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP16;
   const struct glsl_type *type = glsl_vector_type(variant == 1 ? GLSL_TYPE_UINT16 : GLSL_TYPE_FLOAT16, 4);
   nir_variable *input = nir_variable_create(b.shader, nir_var_shader_in,
      variant == 2 ? glsl_array_type(type, 2, 0) : type, "half_input");
   nir_variable *output = nir_variable_create(b.shader, nir_var_shader_out, type, "half_output");
   input->data.location = VARYING_SLOT_VAR0;
   input->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
   output->data.location = FRAG_RESULT_DATA0;
   nir_deref_instr *source = nir_build_deref_var(&b, input);
   if (variant == 2) source = nir_build_deref_array_imm(&b, source, 0);
   nir_def *load = nir_load_deref(&b, source);
   nir_store_var(&b, output, load, 15);
   if (variant == 3) nir_copy_var(&b, output, input);
   nir_intrinsic_instr *store = NULL;
   nir_foreach_block(block, b.impl) nir_foreach_instr(instr, block) {
      if (instr->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_store_deref)
         store = nir_instr_as_intrinsic(instr);
   }
   CHECK(store);
   char why[512] = {0};
   if (variant) {
      CHECK(!lower_half_for_backend(b.shader, device, NULL, why, sizeof(why)));
      ralloc_free(b.shader);
      return 0;
   }
   bool active = device->software && !device->screen->shader_caps[MESA_SHADER_FRAGMENT].fp16;
   CHECK(lower_half_for_backend(b.shader, device, NULL, why, sizeof(why)));
   CHECK(load->bit_size == (active ? 32 : 16));
   CHECK(glsl_get_bit_size(input->type) == (active ? 32 : 16));
   CHECK(glsl_get_bit_size(output->type) == (active ? 32 : 16));
   CHECK(input->data.interpolation == INTERP_MODE_NOPERSPECTIVE);
   if (active) {
      CHECK(store->src[1].ssa->bit_size == 32);
      b.cursor = nir_before_instr(nir_def_instr(load));
      nir_def_replace(load, nir_imm_vec4(&b, 1.00048828125f, 1.00146484375f, -1.00048828125f, 65520));
      fold_constants(b.shader);
      const float nearest[] = {1, 1.001953125f, -1, INFINITY};
      const float zero[] = {1, 1.0009765625f, -1, 65504};
      CHECK(nir_src_is_const(store->src[1]));
      nir_const_value *values = nir_src_as_const_value(store->src[1]);
      for (unsigned i = 0; i < 4; i++) CHECK(values[i].f32 == (rtz ? zero[i] : nearest[i]));
   }
   ralloc_free(b.shader);
   return 0;
}

static int hardware_vertex_case(bool rtz, unsigned variant)
{
   struct pipe_screen screen = { .get_name = softpipe_name };
   struct pipe_screen fp16 = { .get_name = softpipe_name,
      .shader_caps = { [MESA_SHADER_VERTEX] = { .fp16 = true } } };
   struct rwa_device device = { .screen = variant == 4 ? &fp16 : &screen };
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, NULL, "half_vs_export");
   const struct glsl_type *type = glsl_vector_type(GLSL_TYPE_FLOAT16, 4);
   nir_variable *output = nir_variable_create(b.shader, nir_var_shader_out,
      variant == 2 ? glsl_array_type(type, 2, 0) : type, "varying");
   output->data.location = variant == 1 ? VARYING_SLOT_POS : VARYING_SLOT_VAR0;
   nir_variable *input = nir_variable_create(b.shader, nir_var_shader_in, type, "unchanged_input");
   nir_def *input_load = nir_load_var(&b, input);
   nir_def *value = nir_imm_vec4(&b, 1.00048828125f, 1.00146484375f, -1.00048828125f, 65520);
   nir_def *half = rtz ? nir_f2f16_rtz(&b, value) : nir_f2f16_rtne(&b, value);
   nir_deref_instr *deref = nir_build_deref_var(&b, output);
   if (variant == 2) deref = nir_build_deref_array_imm(&b, deref, 0);
   nir_store_deref(&b, deref, half, 15);
   if (variant == 3) nir_copy_var(&b, output, input);
   char why[512] = {0};
   bool accepted = lower_half_for_backend(b.shader, &device, NULL, why, sizeof(why));
   if (variant == 2 || variant == 3) {
      CHECK(!accepted);
      ralloc_free(b.shader);
      return 0;
   }
   CHECK(accepted && input_load->bit_size == 16 && half->bit_size == 16);
   CHECK(glsl_get_bit_size(input->type) == 16);
   bool widened = variant == 0;
   CHECK(glsl_get_bit_size(output->type) == (widened ? 32 : 16));
   if (widened) {
      nir_intrinsic_instr *store = NULL;
      nir_foreach_block(block, b.impl) nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_intrinsic && nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_store_deref)
            store = nir_instr_as_intrinsic(instr);
      }
      CHECK(store && store->src[1].ssa->bit_size == 32);
      fold_constants(b.shader);
      CHECK(nir_src_is_const(store->src[1]));
      nir_const_value *values = nir_src_as_const_value(store->src[1]);
      const float nearest[] = {1, 1.001953125f, -1, INFINITY};
      const float zero[] = {1, 1.0009765625f, -1, 65504};
      for (unsigned i = 0; i < 4; i++) CHECK(values[i].f32 == (rtz ? zero[i] : nearest[i]));
   }
   ralloc_free(b.shader);
   return 0;
}

static int dynamic_hardware_rounding(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options, "dynamic_half_export");
   nir_variable *input = nir_variable_create(b.shader, nir_var_shader_in, glsl_vec4_type(), "input32");
   input->data.location = VERT_ATTRIB_GENERIC3;
   nir_variable *output = nir_variable_create(b.shader, nir_var_shader_out,
      glsl_vector_type(GLSL_TYPE_FLOAT16, 4), "output16");
   output->data.location = VARYING_SLOT_VAR0;
   nir_def *load = nir_load_var(&b, input);
   nir_store_var(&b, output, nir_f2f16(&b, load), 15);
   struct pipe_screen screen = { .get_name = softpipe_name };
   struct rwa_device device = { .screen = &screen };
   char why[512] = {0};
   CHECK(lower_half_for_backend(b.shader, &device, NULL, why, sizeof(why)));
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, b.shader, nir_opt_algebraic);
      NIR_PASS(progress, b.shader, nir_opt_constant_folding);
      NIR_PASS(progress, b.shader, nir_opt_dce);
   } while (progress);
   unsigned half_casts = 0, wide_casts = 0;
   nir_intrinsic_instr *store = NULL;
   nir_foreach_block(block, b.impl) nir_foreach_instr(instr, block) {
      if (instr->type == nir_instr_type_alu) {
         nir_alu_instr *alu = nir_instr_as_alu(instr);
         half_casts += alu->op == nir_op_f2f16;
         wide_casts += alu->op == nir_op_f2f32;
      } else if (instr->type == nir_instr_type_intrinsic &&
                 nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_store_deref)
         store = nir_instr_as_intrinsic(instr);
   }
   CHECK(half_casts == 1 && wide_casts == 1 && store);
   b.cursor = nir_before_instr(nir_def_instr(load));
   nir_def_replace(load, nir_imm_vec4(&b, 1.000244140625f, 1.001708984375f, 0.3333333432674408f, 1.5f));
   fold_constants(b.shader);
   CHECK(nir_src_is_const(store->src[1]));
   const float expected[] = {1, 1.001953125f, 0.333251953125f, 1.5f};
   nir_const_value *values = nir_src_as_const_value(store->src[1]);
   for (unsigned i = 0; i < 4; i++) CHECK(values[i].f32 == expected[i]);
   ralloc_free(b.shader);
   puts("dynamic half export: default f2f16 survives nir_opt_algebraic and preserves numeric rounding PASS");
   return 0;
}

static int actual_hardware_shader(const char *path, const char *entry, mesa_shader_stage stage)
{
   FILE *file = fopen(path, "rb");
   CHECK(file);
   fseek(file, 0, SEEK_END);
   long size = ftell(file);
   rewind(file);
   CHECK(size > 0);
   void *bytes = malloc((size_t)size);
   CHECK(bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size);
   fclose(file);
   void *ctx = ralloc_context(NULL);
   const char *error = NULL;
   struct air_shader_info info = {0};
   static const nir_shader_compiler_options options = {0};
   nir_shader *nir = air_to_nir(ctx, bytes, (size_t)size, entry, NULL, &options, &info, &error);
   if (!nir) fprintf(stderr, "actual shader: %s\n", error);
   CHECK(nir && nir->info.stage == stage);
   struct pipe_screen screen = { .get_name = softpipe_name,
      .shader_caps = { [MESA_SHADER_FRAGMENT] = { .fp16 = true } } };
   struct rwa_device device = { .screen = &screen };
   char why[512] = {0};
   CHECK(lower_half_for_backend(nir, &device, &info, why, sizeof(why)));
   unsigned checked = 0;
   nir_foreach_variable_with_modes(var, nir, nir_var_shader_in | nir_var_shader_out) {
      if (stage == MESA_SHADER_VERTEX && var->data.mode == nir_var_shader_out && var->data.location == VARYING_SLOT_VAR0) {
         CHECK(glsl_get_bit_size(var->type) == 32);
         checked++;
      } else if (stage == MESA_SHADER_FRAGMENT) {
         CHECK(glsl_get_bit_size(var->type) == 16);
         checked++;
      }
   }
   CHECK(checked == (stage == MESA_SHADER_VERTEX ? 1 : 2));
   nir_print_shader(nir, stdout);
   printf("actual hardware NIR: %s VS export width/FS half scope PASS\n", entry);
   ralloc_free(ctx);
   free(bytes);
   return 0;
}

int main(int argc, char **argv)
{
   glsl_type_singleton_init_or_ref();
   CHECK(dynamic_hardware_rounding() == 0);
   CHECK(hardware_vertex_case(false, 0) == 0);
   CHECK(hardware_vertex_case(true, 0) == 0);
   for (unsigned variant = 1; variant <= 4; variant++) CHECK(hardware_vertex_case(false, variant) == 0);
   if (argc == 3) {
      CHECK(actual_hardware_shader(argv[1], "VfxXh", MESA_SHADER_VERTEX) == 0);
      CHECK(actual_hardware_shader(argv[2], "A2Xhf", MESA_SHADER_FRAGMENT) == 0);
   } else CHECK(argc == 1);
   puts("air_half_lowering_test: hardware VS user exports RNE/RTZ; input/POS/FS/compute/fp16 preservation; array/copy refusal PASS");
   struct pipe_screen screen = { .get_name = softpipe_name };
   struct rwa_device device = { .software = true, .screen = &screen };
   CHECK(stage_io_case(&device, false, 0) == 0);
   CHECK(stage_io_case(&device, true, 0) == 0);
   for (unsigned variant = 1; variant <= 3; variant++) CHECK(stage_io_case(&device, false, variant) == 0);
   device.software = false;
   CHECK(stage_io_case(&device, false, 0) == 0);
   device.software = true;
   struct pipe_screen io_fp16 = { .get_name = softpipe_name,
      .shader_caps = { [MESA_SHADER_FRAGMENT] = { .fp16 = true } } };
   device.screen = &io_fp16;
   CHECK(stage_io_case(&device, false, 0) == 0);
   device.screen = &screen;
   puts("air_half_lowering_test: direct half IO RNE/RTZ; integer/array/copy refusal; hardware/fp16 gates PASS");
   for (unsigned flags = 0; flags < 8; flags++)
      CHECK(rounding(&device, flags & 1, flags & 2, flags & 4) == 0);

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL, "unsupported_half");
   nir_def *half = nir_fadd(&b, nir_imm_floatN_t(&b, 1, 16), nir_imm_floatN_t(&b, 2, 16));
   nir_store_ssbo(&b, nir_f2f32(&b, half), nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = 1, .align_mul = 4);
   char why[512] = { 0 };
   CHECK(!lower_half_for_backend(b.shader, &device, NULL, why, sizeof(why)));
   CHECK(strstr(why, "fadd") != NULL && half->bit_size == 16);
   device.software = false;
   CHECK(lower_half_for_backend(b.shader, &device, NULL, why, sizeof(why)) && half->bit_size == 16);
   device.software = true;
   struct pipe_screen fp16_screen = { .get_name = softpipe_name,
      .shader_caps = { [MESA_SHADER_COMPUTE] = { .fp16 = true } } };
   device.screen = &fp16_screen;
   CHECK(lower_half_for_backend(b.shader, &device, NULL, why, sizeof(why)) && half->bit_size == 16);
   ralloc_free(b.shader);
   device.screen = &screen;
   b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL, "half_image_type");
   nir_intrinsic_instr *store = nir_image_store(&b, nir_imm_int(&b, 0), nir_imm_ivec4(&b, 0, 0, 0, 0),
      nir_imm_int(&b, 0), nir_imm_vec4_16(&b, 1, 1, 1, 1), nir_imm_int(&b, 0),
      .image_dim = GLSL_SAMPLER_DIM_2D, .src_type = nir_type_uint);
   struct air_resource resource = { .kind = AIR_RESOURCE_IMAGE, .index = 0,
      .type_name = "texture2d<ushort, write>" };
   struct air_shader_info info = { .num_resources = 1, .resources = &resource };
   CHECK(!lower_half_for_backend(b.shader, &device, NULL, why, sizeof(why)));
   CHECK(!lower_half_for_backend(b.shader, &device, &info, why, sizeof(why)));
   resource.type_name = "texture2d<half, write>";
   CHECK(lower_half_for_backend(b.shader, &device, &info, why, sizeof(why)));
   CHECK(store->src[3].ssa->bit_size == 32 && nir_intrinsic_src_type(store) == nir_type_float32);
   ralloc_free(b.shader);
   CHECK(texture_case(&device, 0, false, true) == 0);
   CHECK(texture_case(&device, 0, true, true) == 0);
   for (unsigned variant = 1; variant <= 8; variant++) CHECK(texture_case(&device, variant, false, false) == 0);
   device.software = false;
   CHECK(texture_case(&device, 0, false, true) == 0);
   device.software = true;
   device.screen = &fp16_screen;
   CHECK(texture_case(&device, 0, false, true) == 0);
   puts("air_half_lowering_test: 40 RNE/RTZ cases; half/ushort image distinction; unsupported half refusal; hardware/fp16 gates PASS");
   puts("air_half_lowering_test: half txf RNE/RTZ, 8 rejected forms, hardware/fp16 gates PASS");
   glsl_type_singleton_decref();
   return 0;
}
