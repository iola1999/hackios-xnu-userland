/* SPDX-License-Identifier: MIT */

#include <stdio.h>

#include "compiler/glsl_types.h"
#include "ir3.h"
#include "ir3_compiler.h"
#include "ir3_nir.h"
#include "ir3_shader.h"

static unsigned failures;

static void
check(const char *name, bool pass)
{
   printf("%s: %s\n", pass ? "PASS" : "FAIL", name);
   failures += !pass;
}

static struct ir3_shader_variant *
new_variant(struct ir3_compiler *compiler)
{
   struct ir3_shader_variant *v = rzalloc(NULL, struct ir3_shader_variant);
   v->compiler = compiler;
   v->type = MESA_SHADER_FRAGMENT;
   v->mergedregs = compiler->mergedregs;
   v->const_state = rzalloc(v, struct ir3_const_state);
   v->const_state->consts_ubo.idx = -1;
   v->const_state->driver_params_ubo.idx = -1;
   v->const_state->primitive_map_ubo.idx = -1;
   v->const_state->primitive_param_ubo.idx = -1;
   return v;
}

/* Exercise the pass at its real input boundary: an output MOV whose source
 * is a relative constant. The offset and address must survive alias selection.
 */
static void
test_alias(struct ir3_compiler *compiler, bool relative, bool immediate)
{
   struct ir3_shader_variant *v = new_variant(compiler);
   struct ir3 *ir = v->ir = ir3_create(compiler, v);
   struct ir3_block *block = ir3_block_create(ir);
   list_addtail(&block->node, &ir->block_list);
   struct ir3_builder b = ir3_builder_at(ir3_after_block(block));
   struct ir3_instruction *mov;
   struct ir3_instruction *address = NULL;
   if (relative) {
      address = ir3_MOV(&b, create_immed_typed(&b, 4, TYPE_S16), TYPE_S16);
      address->dsts[0]->num = regid(REG_A0, 0);
      mov = create_uniform_indirect(&b, 12, TYPE_F32, address);
   } else if (immediate) {
      mov = create_immed_typed(&b, fui(0.5f), TYPE_F32);
   } else {
      mov = create_uniform(&b, 12);
   }
   struct ir3_instruction *end = ir3_build_instr(&b, OPC_END, 0, 1);
   struct ir3_register *output = __ssa_src(end, mov, 0);
   end->end.outidxs = ir3_alloc(ir, sizeof(*end->end.outidxs));
   end->end.outidxs[0] = 0;
   v->outputs_count = 1;
   v->outputs[0].slot = FRAG_RESULT_DATA0;

   bool changed = ir3_create_alias_rt(ir, v);
   unsigned aliases = 0;
   bool source_preserved = true;
   foreach_block (blk, &ir->block_list) {
      foreach_instr (instr, &blk->instr_list) {
         if (instr->opc != OPC_ALIAS || instr->cat7.alias_scope != ALIAS_RT)
            continue;
         aliases++;
         source_preserved &= immediate ?
            (instr->srcs[0]->flags & IR3_REG_IMMED) &&
               instr->srcs[0]->uim_val == fui(0.5f) :
            (instr->srcs[0]->flags & IR3_REG_CONST) &&
               instr->srcs[0]->num == 12;
      }
   }
   printf("alias %s: changed=%u aliases=%u mask=%u live=%u\n",
          relative ? "relative" : immediate ? "immediate" : "absolute",
          changed, aliases, v->outputs[0].aliased_components, output->wrmask);
   if (relative) {
      check("relative constant remains a live output",
            !changed && !aliases && !v->outputs[0].aliased_components &&
            output->wrmask == 1 && output->def == mov->dsts[0] &&
            (mov->srcs[0]->flags & IR3_REG_RELATIV) &&
            mov->srcs[0]->array.offset == 12 && mov->address &&
            mov->address->def == address->dsts[0]);
   } else {
      check(immediate ? "immediate alias preserves value" :
                        "absolute constant alias preserves register",
            changed && aliases == 1 && source_preserved &&
            v->outputs[0].aliased_components == 1 &&
            !output->wrmask && !output->def);
   }
   ralloc_free(v);
}

static void
test_fsat(struct ir3_compiler *compiler, nir_op op, bool second_use)
{
   const char *name = nir_op_infos[op].name;
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, ir3_get_compiler_options(compiler),
      "fsat_%s_%s", name, second_use ? "two_uses" : "one_use");
   nir_def *inputs = nir_load_input(&b, 2, 32, nir_imm_int(&b, 0),
      .base = 0, .dest_type = nir_type_uint32,
      .io_semantics = {.location = VARYING_SLOT_VAR0, .num_slots = 1});
   nir_def *value = nir_build_alu(&b, op, nir_channel(&b, inputs, 0),
                                 nir_channel(&b, inputs, 1), NULL, NULL);
   nir_store_output(&b, nir_fsat(&b, value), nir_imm_int(&b, 0),
      .base = 0, .write_mask = 1, .src_type = nir_type_float32,
      .io_semantics = {.location = FRAG_RESULT_DATA0, .num_slots = 1});
   if (second_use) {
      nir_store_output(&b, value, nir_imm_int(&b, 0),
         .base = 1, .write_mask = 1, .src_type = nir_type_float32,
         .io_semantics = {.location = FRAG_RESULT_DATA1, .num_slots = 1});
   }
   b.shader->num_inputs = 1;
   b.shader->num_outputs = second_use ? 2 : 1;
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_validate_shader(b.shader, "fsat regression input");

   struct ir3_shader_options options = {0};
   ir3_finalize_nir(compiler, &options.nir_options, b.shader);
   struct ir3_shader *shader = ir3_shader_from_nir(compiler, b.shader, &options);
   ir3_nir_post_finalize(shader);
   struct ir3_shader_variant *v = new_variant(compiler);
   v->shader = shader;
   v->shader_id = shader->id;
   v->name = ralloc_strdup(v, b.shader->info.name);
   v->const_state->num_app_ubos = 1;
   int ret = ir3_compile_shader_nir(compiler, shader, v);
   check(b.shader->info.name, ret == 0);
   if (!ret) {
      unsigned nonfloat_sat = 0, float_sat = 0, producers = 0;
      opc_t producer = op == nir_op_ixor ? OPC_XOR_B :
                      op == nir_op_iadd ? OPC_ADD_U : OPC_ADD_F;
      foreach_block (block, &v->ir->block_list) {
         foreach_instr (instr, &block->instr_list) {
            producers += instr->opc == producer;
            if (!(instr->flags & IR3_INSTR_SAT))
               continue;
            if (is_cat2_float(instr->opc) || is_cat3_float(instr->opc))
               float_sat++;
            else
               nonfloat_sat++;
         }
      }
      printf("fsat %s uses=%u: producer=%u float_sat=%u nonfloat_sat=%u\n",
             name, second_use ? 2 : 1, producers, float_sat, nonfloat_sat);
      check("fsat uses a floating-point saturating instruction",
            producers >= 1 && float_sat >= 1 && nonfloat_sat == 0);
      void *binary = ir3_shader_assemble(v);
      check("compiled shader assembles", binary != NULL);
      if (binary) {
         ir3_shader_disasm(v, binary, stdout);
      }
   }
   ralloc_free(v);
   ir3_shader_destroy(shader);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   const struct fd_dev_id id = {.chip_id = 0xffff44050000};
   struct ir3_compiler *compiler = ir3_compiler_create(
      NULL, &id, fd_dev_info_raw(&id),
      &(struct ir3_compiler_options){.disable_cache = true});
   check("A830 supports alias.rt", compiler->info->props.has_alias_rt);
   test_alias(compiler, true, false);
   test_alias(compiler, false, false);
   test_alias(compiler, false, true);
   test_fsat(compiler, nir_op_ixor, false);
   test_fsat(compiler, nir_op_iadd, false);
   test_fsat(compiler, nir_op_ixor, true);
   test_fsat(compiler, nir_op_fadd, false);
   ir3_compiler_destroy(compiler);
   glsl_type_singleton_decref();
   printf("failures=%u\n", failures);
   return failures ? 1 : 0;
}
