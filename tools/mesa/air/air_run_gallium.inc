/* What a Gallium driver expects of a shader it is handed: NIR's own system
 * values lowered to the hardware's, and I/O as intrinsics rather than
 * variables. The same three passes are in tools/mesa/air-ir3.patch; when a
 * fourth place needs them they should become one function of the frontend.
 */
static unsigned
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static bool
use_constant_buffer(nir_builder *b, nir_intrinsic_instr *load, void *data)
{
   if (load->intrinsic != nir_intrinsic_load_constant)
      return false;

   unsigned slot = *(const unsigned *)data;
   b->cursor = nir_before_instr(&load->instr);
   nir_def *offset = nir_iadd_imm(b, load->src[0].ssa, nir_intrinsic_base(load));
   nir_def *value = nir_load_ubo(b, load->def.num_components, load->def.bit_size,
                                 nir_imm_int(b, slot), offset,
                                 .align_mul = nir_intrinsic_align_mul(load),
                                 .align_offset = nir_intrinsic_align_offset(load),
                                 .range_base = nir_intrinsic_base(load),
                                 .range = nir_intrinsic_range(load));
   nir_def_rewrite_uses(&load->def, value);
   nir_instr_remove(&load->instr);
   return true;
}

/* The software screens accept UBO loads. Bind the same immutable byte section
 * that freedreno embeds in the shader binary. */
static bool
bind_software_constants(struct pipe_context *pipe, nir_shader *shader)
{
   const char *name = pipe->screen->get_name(pipe->screen);
   if (!shader->constant_data_size || (!strstr(name, "llvmpipe") && !strstr(name, "softpipe")))
      return true;
   unsigned slot = shader->info.num_ubos;
   if (slot >= pipe->screen->shader_caps[shader->info.stage].max_const_buffers) {
      fprintf(stderr, "air_run: no constant-buffer slot for the shader constant data\n");
      return false;
   }
   if (!nir_shader_intrinsics_pass(shader, use_constant_buffer, nir_metadata_control_flow, &slot))
      return true;
   struct pipe_resource *buf = pipe_buffer_create(pipe->screen, PIPE_BIND_CONSTANT_BUFFER,
                                                   PIPE_USAGE_IMMUTABLE, shader->constant_data_size);
   if (!buf)
      return false;
   pipe_buffer_write(pipe, buf, 0, shader->constant_data_size, shader->constant_data);
   pipe->set_constant_buffer(pipe, shader->info.stage, slot, &(struct pipe_constant_buffer){
      .buffer = buf, .buffer_size = shader->constant_data_size });
   pipe_resource_reference(&buf, NULL);
   shader->info.num_ubos = slot + 1;
   return true;
}

static void
finalize_for_gallium(nir_shader *nir, struct pipe_screen *screen)
{
   /* A fragment shader's [[position]]: the frontend hands it over as a system-value
    * variable, because that is the one form both of these drivers can be served from
    * (the long version is beside air.position in air_to_nir.c). freedreno sets
    * pipe_caps.fs_position_is_sysval and wants the intrinsic; softpipe does not set it
    * and reads (0,0,0,0) from the intrinsic, because nothing fills TGSI's system values
    * for a fragment shader. For such a screen the variable becomes an input varying
    * here -- and then needs a driver_location, which nothing else would give it.
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
   /* An options struct of all-false and not NULL, which are not the same thing
    * here and the difference is a trap. nir_lower_system_values *introduces*
    * load_base_workgroup_id when it lowers SYSTEM_VALUE_WORKGROUP_ID, and the
    * only thing that removes it again is
    *
    *     if (options && !options->has_base_workgroup_id)
    *        return nir_imm_zero(...);
    *
    * so NULL reads as "the driver has base workgroup ids" -- the opposite of
    * what a harness wants -- and the intrinsic survives into the backend. With
    * softpipe that is an assertion in nir_to_tgsi rather than a message, and it
    * stopped every kernel that asks for its workgroup id. Zero is also the
    * right answer here: this dispatches one workgroup from the origin.
    */
   static const nir_lower_compute_system_values_options cs_options = { 0 };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &cs_options);
   /*
    * nir_lower_io_use_interpolated_input_intrinsics, and WITHOUT it every varying
    * was FLAT on the Adreno while softpipe interpolated it. Mesa's own comment on the
    * flag says exactly why: without it nir_lower_io "generates regular load_input
    * intrinsics and interpolation information must be inferred from the list of input
    * nir_variables" -- and the two consumers infer it from different places.
    * nir_to_tgsi declares each FS input from its variable's data.interpolation (SMOOTH,
    * as the frontend sets it), so softpipe was right by accident; ir3's setup_input
    * does `so->inputs[n].flat = !coord` and only builds `coord` for
    * load_interpolated_input, so a plain load_input is flat, full stop.
    *
    * What that cost: the one draw in this project whose varying actually varies --
    * Apple's texturedVertex2D handing a texcoord to texturedFragment2D* -- read the
    * provoking vertex's texcoord at every fragment on the phone, and the picture looked
    * VERTICALLY FLIPPED for three boots (docs/metal.md 10.26, 10.32). It is not flipped.
    * The quad's pixel (0,0) sits in the triangle whose last vertex carries uv (0,1), so
    * flat shading hands it texel (0,3) -- `100 203 303 400`, bit for bit the number the
    * device printed, and reproduced here on softpipe by forcing INTERP_MODE_FLAT.
    *
    * Why nothing caught it: the 255 fragment shaders compared pixel by pixel are drawn
    * over the generated vertex stage, which gives every vertex the SAME texcoord -- flat
    * and smooth are identical over a constant. And ir3_compiler, the tool the corpus's
    * "compiles through ir3" column comes from, passes this flag already
    * (tools/mesa/air-ir3.patch): the tool was lowering correctly while the two harnesses
    * that actually draw were not.
    */
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_vec4, nir_lower_io_use_interpolated_input_intrinsics);
}
