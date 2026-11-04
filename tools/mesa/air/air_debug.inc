/*
 * Debugging switches for one fragment shader, read from the environment when a shader is
 * translated, so that `respin.py --env PROCESS:AIR_DEBUG_...=TEXT --restart PROCESS` changes a
 * running process's shader without a new library. TEXT is matched against the entry point name.
 *
 *   AIR_DEBUG_SOLID=TEXT      every colour the shader stores is opaque magenta; its discards stay.
 *                             Where the draw reaches the screen shows where its fragments survive.
 *   AIR_DEBUG_NODISCARD=TEXT  its discards and demotes are removed, its colours are left alone.
 *   AIR_DEBUG_TEX=TEXT        every colour stored is what the shader's first texture read returned,
 *                             with alpha 1, and its discards are removed: what it sees in that texture.
 *   AIR_DEBUG_COORD=TEXT      the same with that read's coordinate in place of its result (red the
 *                             first component, green the second), so a wrong varying shows.
 *   AIR_DEBUG_SCALE=NUMBER    multiplies what TEX or COORD shows, before the attachment clamps it:
 *                             a distance field in pixels needs about 1/64, its negative half -1/64.
 *
 * TEX keeps the read as ir3 compiles it, so a fault in how the read is issued (a fragment texture
 * prefetch, for one) shows in its result. COORD leaves the read unused and it is removed, so the
 * coordinate comes through the ordinary varying path whatever the read would have done.
 * Included by air_intrinsics.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool
air_debug_wanted(const nir_shader *nir, const char *variable)
{
   const char *text = getenv(variable);

   return text != NULL && text[0] != '\0' && nir->info.name != NULL && strstr(nir->info.name, text);
}

void
air_debug_fragment(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT)
      return;
   bool solid = air_debug_wanted(nir, "AIR_DEBUG_SOLID");
   bool coord = air_debug_wanted(nir, "AIR_DEBUG_COORD");
   bool probe = coord || air_debug_wanted(nir, "AIR_DEBUG_TEX");
   bool keep = probe || air_debug_wanted(nir, "AIR_DEBUG_NODISCARD");
   const char *scale_text = getenv("AIR_DEBUG_SCALE");
   float scale = scale_text != NULL ? strtof(scale_text, NULL) : 1.0f;
   nir_def *first_tex = NULL;
   if (!solid && !keep)
      return;
   /* the first texture read in the entry block, which every store comes after */
   nir_foreach_instr(instr, nir_start_block(nir_shader_get_entrypoint(nir))) {
      if (!probe || first_tex != NULL || instr->type != nir_instr_type_tex)
         continue;
      nir_tex_instr *tex = nir_instr_as_tex(instr);
      int at = nir_tex_instr_src_index(tex, nir_tex_src_coord);
      first_tex = coord ? (at >= 0 ? tex->src[at].src.ssa : NULL) : &tex->def;
   }

   unsigned discards = 0, colours = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);

      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

            if (keep && (intr->intrinsic == nir_intrinsic_demote ||
                         intr->intrinsic == nir_intrinsic_demote_if ||
                         intr->intrinsic == nir_intrinsic_terminate ||
                         intr->intrinsic == nir_intrinsic_terminate_if)) {
               nir_instr_remove(instr);
               discards++;
               continue;
            }
            if ((!solid && first_tex == NULL) || intr->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_variable *var = nir_intrinsic_get_var(intr, 0);
            if (var == NULL || var->data.mode != nir_var_shader_out ||
                var->data.location < FRAG_RESULT_DATA0)
               continue;
            nir_def *value = intr->src[1].ssa;
            b.cursor = nir_before_instr(instr);
            nir_def *colour = value->bit_size == 16 ? nir_imm_vec4_16(&b, 1.0, 0.0, 1.0, 1.0)
                                                    : nir_imm_vec4(&b, 1.0, 0.0, 1.0, 1.0);
            if (first_tex != NULL) {
               nir_def *seen = nir_fmul_imm(&b, nir_pad_vector_imm_int(&b, first_tex, 0, 4), scale);
               seen = nir_vector_insert_imm(&b, seen, nir_imm_floatN_t(&b, 1.0, seen->bit_size), 3);
               colour = nir_f2fN(&b, seen, value->bit_size);
            }
            nir_src_rewrite(&intr->src[1], nir_trim_vector(&b, colour, value->num_components));
            colours++;
         }
      }
      nir_progress(true, impl, nir_metadata_none);
   }
   printf("air: debug %s: %u colour stores made %s (scale %g), %u discards removed\n",
          nir->info.name, colours,
          first_tex ? (coord ? "the first texture read's coordinate" : "the first texture read")
                    : "magenta", scale, discards);
   fflush(stdout);
}
