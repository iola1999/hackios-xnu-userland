/* TGSI has no native 16-bit registers. Keep half values as their low 16 raw bits
 * in 32-bit registers, using Mesa's conversion pass for half rounding. This
 * covers casts, copies/selects/phis, 2D integer-coordinate texture reads and
 * texture writes and direct half stage I/O; other half operations
 * return a compilation error before entering nir_to_tgsi. */
static bool
half_copy_op(nir_op op)
{
   return op == nir_op_mov || nir_op_is_vec(op) || op == nir_op_bcsel;
}

static bool
half_cast_op(nir_op op)
{
   return op == nir_op_u2f16 || op == nir_op_i2f16 || op == nir_op_f2f16 ||
          op == nir_op_f2f16_rtne || op == nir_op_f2f16_rtz;
}

static bool
has_half_source(nir_src *src, void *data)
{
   if (src->ssa && src->ssa->bit_size == 16)
      *(bool *)data = true;
   return true;
}

static bool
half_image_declared(const nir_intrinsic_instr *in, const struct air_shader_info *info)
{
   /* Older AIR writes use src_type=uint even for half data. The resource's
    * declared element type distinguishes half bits from 16-bit integer data. */
   if (!info || !nir_src_is_const(in->src[0]))
      return false;
   unsigned binding = nir_src_as_uint(in->src[0]);
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *resource = &info->resources[i];
      if (resource->kind != AIR_RESOURCE_IMAGE || resource->index != binding || !resource->type_name)
         continue;
      const char *element = strchr(resource->type_name, '<');
      return element && (!strncmp(element, "<half,", 6) || !strncmp(element, "<half>", 6));
   }
   return false;
}

static bool
check_half_instructions(nir_shader *nir, const struct air_shader_info *info,
                        bool *has_half, char *why, size_t why_size)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_def *def = nir_instr_def(instr);
            bool half_source = false;
            nir_foreach_src(instr, has_half_source, &half_source);
            if ((!def || def->bit_size != 16) && !half_source)
               continue;
            *has_half = true;
            switch (instr->type) {
            case nir_instr_type_load_const:
            case nir_instr_type_undef:
            case nir_instr_type_phi:
               continue;
            case nir_instr_type_alu: {
               nir_alu_instr *alu = nir_instr_as_alu(instr);
               if (half_copy_op(alu->op) ||
                   (half_cast_op(alu->op) && alu->src[0].src.ssa->bit_size == 32) ||
                   (alu->op == nir_op_f2f32 && alu->src[0].src.ssa->bit_size == 16))
                  continue;
               say(why, why_size, "softpipe half lowering does not support ALU %s",
                   nir_op_infos[alu->op].name);
               return false;
            }
            case nir_instr_type_intrinsic: {
               nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
               if (in->intrinsic == nir_intrinsic_image_store &&
                   in->src[3].ssa->bit_size == 16 &&
                   in->src[0].ssa->bit_size == 32 && in->src[1].ssa->bit_size == 32 &&
                   in->src[2].ssa->bit_size == 32 && in->src[4].ssa->bit_size == 32 &&
                   half_image_declared(in, info))
                  continue;
               say(why, why_size, "softpipe half lowering does not support intrinsic %s",
                   nir_intrinsic_infos[in->intrinsic].name);
               return false;
            }
            default:
               say(why, why_size, "softpipe half lowering does not support instruction type %u", instr->type);
               return false;
            }
         }
      }
   }
   return true;
}

static bool
split_half_integer_cast(nir_builder *b, nir_instr *instr, void *data)
{
   (void)data;
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_u2f16 && alu->op != nir_op_i2f16)
      return false;
   b->cursor = nir_before_instr(instr);
   b->fp_math_ctrl = alu->fp_math_ctrl;
   nir_def *input = nir_ssa_for_alu_src(b, alu, 0);
   /* Every 32-bit integer inside half's finite/overflow boundary is exactly
    * representable in FP32. Larger magnitudes overflow half in either path. */
   nir_def *wide = alu->op == nir_op_u2f16 ? nir_u2f32(b, input) : nir_i2f32(b, input);
   nir_def_replace(&alu->def, nir_f2f16(b, wide));
   return true;
}

static nir_def *
unpack_half_components(nir_builder *b, nir_def *raw)
{
   nir_def *components[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < raw->num_components; i++)
      components[i] = nir_channel(b, nir_unpack_half_2x16(b, nir_channel(b, raw, i)), 0);
   return nir_vec(b, components, raw->num_components);
}

static bool
lower_half_txf(nir_builder *b, nir_instr *instr, void *data)
{
   (void)data;
   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   if (tex->op != nir_texop_txf || tex->dest_type != nir_type_float16 ||
       tex->def.bit_size != 16 || tex->sampler_dim != GLSL_SAMPLER_DIM_2D ||
       tex->is_array || tex->is_shadow || tex->is_sparse ||
       tex->coord_components != 2 || tex->num_srcs != 2)
      return false;
   unsigned sources = 0;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      nir_tex_src *src = &tex->src[i];
      if (!src->src.ssa || src->src.ssa->bit_size != 32)
         return false;
      if (src->src_type == nir_tex_src_coord && src->src.ssa->num_components == 2)
         sources |= 1;
      else if (src->src_type == nir_tex_src_lod && src->src.ssa->num_components == 1)
         sources |= 2;
      else
         return false;
   }
   if (sources != 3)
      return false;

   /* TGSI fetches float32. An explicit conversion preserves the declared half
    * result, including rounding for source formats with more than 16 bits. */
   tex->def.bit_size = 32;
   tex->dest_type = nir_type_float32;
   b->cursor = nir_after_instr(instr);
   nir_def *half = nir_f2f16(b, &tex->def);
   nir_def_rewrite_uses_after(&tex->def, half);
   return true;
}

/* TGSI I/O and IR3's VS user-varying export use float32 registers. Preserve
 * half rounding at loads and numerically widen stores before changing variable
 * types. Raw half bits must never be passed to the rasterizer as float32. */
static bool
half_io_variable(const nir_variable *var, bool vertex_outputs_only)
{
   if (!(var->data.mode & (nir_var_shader_in | nir_var_shader_out)) ||
       !glsl_type_is_vector_or_scalar(var->type) ||
       glsl_get_base_type(var->type) != GLSL_TYPE_FLOAT16)
      return false;
   return !vertex_outputs_only ||
          (var->data.mode == nir_var_shader_out &&
           var->data.location >= VARYING_SLOT_VAR0 && var->data.location <= VARYING_SLOT_VAR31);
}
static bool
widen_half_stage_io(nir_shader *nir, bool vertex_outputs_only, char *why, size_t why_size)
{
   if (vertex_outputs_only) {
      nir_foreach_variable_with_modes(var, nir, nir_var_shader_out) {
         if (var->data.location >= VARYING_SLOT_VAR0 && var->data.location <= VARYING_SLOT_VAR31 &&
             glsl_get_base_type(glsl_without_array(var->type)) == GLSL_TYPE_FLOAT16 &&
             !glsl_type_is_vector_or_scalar(var->type)) {
            say(why, why_size, "half VS user-varying exports require scalar/vector types");
            return false;
         }
      }
   }
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_deref)
               continue;
            nir_deref_instr *deref = nir_instr_as_deref(instr);
            if (deref->deref_type != nir_deref_type_var ||
                !half_io_variable(deref->var, vertex_outputs_only))
               continue;
            nir_foreach_use(src, &deref->def) {
               nir_instr *use = nir_src_is_if(src) ? NULL : nir_src_use_instr(src);
               if (!use || use->type != nir_instr_type_intrinsic ||
                   (nir_instr_as_intrinsic(use)->intrinsic != nir_intrinsic_load_deref &&
                    nir_instr_as_intrinsic(use)->intrinsic != nir_intrinsic_store_deref)) {
                  say(why, why_size, "half stage I/O requires direct loads/stores");
                  return false;
               }
            }
         }
      }
   }
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_load_deref && in->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_deref_instr *deref = nir_src_as_deref(in->src[0]);
            if (!deref || deref->deref_type != nir_deref_type_var ||
                !half_io_variable(deref->var, vertex_outputs_only))
               continue;
            if (in->intrinsic == nir_intrinsic_load_deref) {
               in->def.bit_size = 32;
               b.cursor = nir_after_instr(instr);
               nir_def *half = nir_f2f16(&b, &in->def);
               nir_def_rewrite_uses_after(&in->def, half);
            } else {
               b.cursor = nir_before_instr(instr);
               nir_def *wide = nir_f2f32(&b, in->src[1].ssa);
               /* This export consumes an already-rounded half value. Keep its
                * intermediate rounding through algebraic optimization. */
               if (vertex_outputs_only)
                  nir_def_as_alu(wide)->fp_math_ctrl |= nir_fp_no_transform;
               nir_src_rewrite(&in->src[1], wide);
            }
         }
      }
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_deref)
               continue;
            nir_deref_instr *deref = nir_instr_as_deref(instr);
            if (deref->deref_type == nir_deref_type_var &&
                half_io_variable(deref->var, vertex_outputs_only))
               deref->type = glsl_vector_type(GLSL_TYPE_FLOAT, glsl_get_vector_elements(deref->type));
         }
      }
      nir_progress(true, impl, nir_metadata_control_flow);
   }
   nir_foreach_variable_with_modes(var, nir, nir_var_shader_in | nir_var_shader_out) {
      if (half_io_variable(var, vertex_outputs_only))
         var->type = glsl_vector_type(GLSL_TYPE_FLOAT, glsl_get_vector_elements(var->type));
   }
   return true;
}

static bool
lower_half_for_backend(nir_shader *nir, struct rwa_device *device,
                        const struct air_shader_info *info, char *why, size_t why_size)
{
   if (!device->software) {
      /* IR3's VPC output register mapping has no half-register selector. Keep
       * half computations intact, but export VS user varyings numerically. */
      if (nir->info.stage == MESA_SHADER_VERTEX && !device->screen->shader_caps[MESA_SHADER_VERTEX].fp16)
         return widen_half_stage_io(nir, true, why, why_size);
      return true;
   }
   if (strcmp(device->screen->get_name(device->screen), "softpipe") ||
       device->screen->shader_caps[nir->info.stage].fp16)
      return true;
   if (!widen_half_stage_io(nir, false, why, why_size))
      return false;
   NIR_PASS(_, nir, nir_shader_instructions_pass, lower_half_txf,
            nir_metadata_control_flow, NULL);
   bool has_half = false;
   if (!check_half_instructions(nir, info, &has_half, why, why_size))
      return false;
   if (!has_half)
      return true;

   NIR_PASS(_, nir, nir_shader_instructions_pass, split_half_integer_cast,
            nir_metadata_control_flow, NULL);
   NIR_PASS(_, nir, nir_lower_fp16_casts, nir_lower_fp16_all);
   NIR_PASS(_, nir, nir_opt_dce);

   /* Remember the half definitions before widening: consumers must still
    * distinguish raw half bits from ordinary 32-bit values. */
   struct set *halves = _mesa_pointer_set_create(NULL);
   if (!halves) {
      say(why, why_size, "out of memory lowering half registers for softpipe");
      return false;
   }
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_def *def = nir_instr_def(instr);
            if (!def || def->bit_size != 16)
               continue;
            _mesa_set_add(halves, def);
            if (instr->type == nir_instr_type_load_const) {
               nir_load_const_instr *load = nir_instr_as_load_const(instr);
               for (unsigned i = 0; i < def->num_components; i++)
                  load->value[i].u32 = load->value[i].u16;
            }
            def->bit_size = 32;
         }
      }
   }
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            b.cursor = nir_before_instr(instr);
            if (instr->type == nir_instr_type_alu) {
               nir_alu_instr *alu = nir_instr_as_alu(instr);
               if (alu->op == nir_op_u2u16 && _mesa_set_search(halves, &alu->def)) {
                  nir_def *raw = nir_ssa_for_alu_src(&b, alu, 0);
                  nir_def *wide = nir_iand_imm(&b, raw, 0xffff);
                  _mesa_set_add(halves, wide);
                  nir_def_replace(&alu->def, wide);
               } else if (alu->op == nir_op_f2f32 && _mesa_set_search(halves, alu->src[0].src.ssa)) {
                  nir_def_replace(&alu->def,
                     unpack_half_components(&b, nir_ssa_for_alu_src(&b, alu, 0)));
               }
            } else if (instr->type == nir_instr_type_intrinsic) {
               nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
               if (in->intrinsic == nir_intrinsic_image_store && _mesa_set_search(halves, in->src[3].ssa)) {
                  nir_src_rewrite(&in->src[3], unpack_half_components(&b, in->src[3].ssa));
                  nir_intrinsic_set_src_type(in, nir_type_float32);
               }
            }
         }
      }
      nir_progress(true, impl, nir_metadata_control_flow);
   }
   _mesa_set_destroy(halves, NULL);
   NIR_PASS(_, nir, nir_opt_dce);
   return true;
}

/* What a Gallium driver expects of a shader it is handed: NIR's own system values
 * lowered to the hardware's, and I/O as intrinsics rather than variables. This is
 * air_run.c's finalize_for_gallium, and the same three passes are in
 * tools/mesa/air-ir3.patch; a fourth copy would be the point at which they should
 * become one function of the frontend.
 */
static unsigned
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static void
finalize_for_gallium(nir_shader *nir, struct pipe_screen *screen)
{
   /* A fragment shader's [[position]]: the frontend hands it over as a system-value
    * variable, because that is the one form both of the drivers behind this library can
    * be served from (the long version is beside air.position in air_to_nir.c and in
    * docs/metal.md 10.28). freedreno sets pipe_caps.fs_position_is_sysval and wants the
    * intrinsic; softpipe does not set it and reads (0,0,0,0) from the intrinsic, because
    * nothing fills TGSI's system values for a fragment shader. For such a screen the
    * variable becomes an input varying here -- and then needs a driver_location, which
    * nothing else would give it.
    */
   if (nir->info.stage == MESA_SHADER_FRAGMENT && !screen->caps.fs_position_is_sysval) {
      static const struct nir_lower_sysvals_to_varyings_options pos_opts = {
         .frag_coord = true,
      };
      if (nir_lower_sysvals_to_varyings(nir, &pos_opts)) {
         nir_foreach_shader_in_variable(var, nir)
            if (var->data.location == VARYING_SLOT_POS)
               var->data.driver_location = nir->num_inputs++;
      }
   }
   NIR_PASS(_, nir, nir_lower_system_values);
   /* An options struct of all-false and not NULL, which are not the same thing here and
    * the difference is a trap. nir_lower_system_values *introduces*
    * load_base_workgroup_id when it lowers SYSTEM_VALUE_WORKGROUP_ID, and the only
    * thing that removes it again is
    *
    *     if (options && !options->has_base_workgroup_id)
    *        return nir_imm_zero(...);
    *
    * so NULL reads as "the driver has base workgroup ids" -- the opposite of what is
    * wanted -- and the intrinsic survives into the backend. With softpipe that is an
    * assertion in nir_to_tgsi rather than a message, and it stopped every kernel that
    * asks for its workgroup id. Zero is also the right answer here: rwa_dispatch
    * launches its grid from the origin, as Metal's own dispatch does.
    */
   static const nir_lower_compute_system_values_options cs_options = { 0 };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &cs_options);
   /*
    * nir_lower_io_use_interpolated_input_intrinsics, and WITHOUT it every varying this
    * library drew was FLAT on the Adreno while softpipe interpolated it -- the mistake
    * that looked like a vertically flipped picture for three boots (docs/metal.md 10.26,
    * 10.32). Mesa's own comment on the flag says why: without it nir_lower_io "generates
    * regular load_input intrinsics and interpolation information must be inferred from
    * the list of input nir_variables", and the two consumers infer it from different
    * places. nir_to_tgsi reads each FS input's variable (INTERP_MODE_SMOOTH, as the
    * frontend sets it); ir3's setup_input does `so->inputs[n].flat = !coord` and builds
    * `coord` only for load_interpolated_input, so a plain load_input is flat.
    *
    * Nothing caught it because the only draws whose varying VARIES are Apple's
    * texturedVertex2D/3D feeding texturedFragment*: the generated vertex stage hands
    * every vertex the same texture coordinate (RWA_TEX_* below), and over a constant
    * flat and smooth are the same number.
    */
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_vec4, nir_lower_io_use_interpolated_input_intrinsics);
}
