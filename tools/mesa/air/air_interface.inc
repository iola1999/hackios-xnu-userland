/* Shader interface, system values, and I/O translation, included into air_to_nir.c. */

/* -- the interface ------------------------------------------------------ */

/* AIR's names for what NIR calls system values */
enum air_sysval {
   AIR_SV_VERTEX_ID, AIR_SV_INSTANCE_ID, AIR_SV_BASE_VERTEX, AIR_SV_BASE_INSTANCE,
   AIR_SV_AMPLIFICATION_ID,
   AIR_SV_FRONT_FACING, AIR_SV_SAMPLE_ID, AIR_SV_SAMPLE_MASK_IN, AIR_SV_POINT_COORD,
   AIR_SV_THREAD_POS_GRID, AIR_SV_THREAD_POS_TG, AIR_SV_TG_POS_GRID,
   AIR_SV_THREADS_PER_TG, AIR_SV_TGS_PER_GRID, AIR_SV_THREAD_INDEX_TG,
   AIR_SV_THREADS_PER_GRID, AIR_SV_SIMD_LANE, AIR_SV_SIMD_INDEX, AIR_SV_SIMD_WIDTH,
   AIR_SV_SIMDS_PER_TG, AIR_SV_QUAD_LANE, AIR_SV_QUAD_INDEX, AIR_SV_QUADS_PER_TG,
};

static const struct { const char *key; enum air_sysval sv; } sysvals[] = {
   { "air.vertex_id", AIR_SV_VERTEX_ID },
   { "air.instance_id", AIR_SV_INSTANCE_ID },
   { "air.base_vertex", AIR_SV_BASE_VERTEX },
   { "air.base_instance", AIR_SV_BASE_INSTANCE },
   { "air.amplification_id", AIR_SV_AMPLIFICATION_ID },
   { "air.front_facing", AIR_SV_FRONT_FACING },
   { "air.sample_id", AIR_SV_SAMPLE_ID },
   { "air.sample_mask_in", AIR_SV_SAMPLE_MASK_IN },
   { "air.point_coord", AIR_SV_POINT_COORD },
   { "air.thread_position_in_grid", AIR_SV_THREAD_POS_GRID },
   { "air.thread_position_in_threadgroup", AIR_SV_THREAD_POS_TG },
   { "air.threadgroup_position_in_grid", AIR_SV_TG_POS_GRID },
   { "air.threads_per_threadgroup", AIR_SV_THREADS_PER_TG },
   { "air.threadgroups_per_grid", AIR_SV_TGS_PER_GRID },
   { "air.thread_index_in_threadgroup", AIR_SV_THREAD_INDEX_TG },
   { "air.threads_per_grid", AIR_SV_THREADS_PER_GRID },
   { "air.thread_index_in_simdgroup", AIR_SV_SIMD_LANE },
   { "air.simdgroup_index_in_threadgroup", AIR_SV_SIMD_INDEX },
   { "air.threads_per_simdgroup", AIR_SV_SIMD_WIDTH },
   { "air.simdgroups_per_threadgroup", AIR_SV_SIMDS_PER_TG },
   { "air.thread_index_in_quadgroup", AIR_SV_QUAD_LANE },
   { "air.quadgroup_index_in_threadgroup", AIR_SV_QUAD_INDEX },
   { "air.quadgroups_per_threadgroup", AIR_SV_QUADS_PER_TG },
};

static nir_def *
load_sysval(struct air_ctx *c, enum air_sysval sv)
{
   switch (sv) {
   case AIR_SV_VERTEX_ID: return nir_load_vertex_id(&c->b);
   /* Metal's instance id counts from the base instance, GL's does not */
   case AIR_SV_INSTANCE_ID:
      return nir_iadd(&c->b, nir_load_instance_id(&c->b), nir_load_base_instance(&c->b));
   case AIR_SV_BASE_VERTEX: return nir_load_first_vertex(&c->b);
   case AIR_SV_BASE_INSTANCE: return nir_load_base_instance(&c->b);
   /* Single-view rendering (maxVertexAmplificationCount == 1): amplification_id is 0. */
   case AIR_SV_AMPLIFICATION_ID: return nir_imm_int(&c->b, 0);
   case AIR_SV_FRONT_FACING: return nir_load_front_face(&c->b, 1);
   case AIR_SV_SAMPLE_ID: return nir_load_sample_id(&c->b);
   case AIR_SV_SAMPLE_MASK_IN: return nir_load_sample_mask_in(&c->b);
   case AIR_SV_POINT_COORD: return nir_load_point_coord(&c->b);
   case AIR_SV_THREAD_POS_GRID:
      if (c->options && c->options->split) {
         const struct air_grid_split *s = c->options->split;
         nir_def *group = nir_iadd(&c->b, nir_load_workgroup_id(&c->b),
                                   nir_imm_ivec3(&c->b, s->base_group[0], s->base_group[1], s->base_group[2]));

         return nir_iadd(&c->b, nir_imul(&c->b, group, nir_imm_ivec3(&c->b, s->nominal[0], s->nominal[1], s->nominal[2])),
                         nir_load_local_invocation_id(&c->b));
      }
      return nir_load_global_invocation_id(&c->b, 32);
   case AIR_SV_THREAD_POS_TG: return nir_load_local_invocation_id(&c->b);
   case AIR_SV_TG_POS_GRID:
      if (c->options && c->options->split) {
         const struct air_grid_split *s = c->options->split;

         return nir_iadd(&c->b, nir_load_workgroup_id(&c->b),
                         nir_imm_ivec3(&c->b, s->base_group[0], s->base_group[1], s->base_group[2]));
      }
      return nir_load_workgroup_id(&c->b);
   /* (in a part of a split grid this is the part's own size: Metal's partial threadgroup) */
   case AIR_SV_THREADS_PER_TG: return nir_load_workgroup_size(&c->b);
   case AIR_SV_TGS_PER_GRID:
      if (c->options && c->options->split) {
         const struct air_grid_split *s = c->options->split;

         return nir_imm_ivec3(&c->b, s->grid_groups[0], s->grid_groups[1], s->grid_groups[2]);
      }
      return nir_load_num_workgroups(&c->b);
   case AIR_SV_THREAD_INDEX_TG: return nir_load_local_invocation_index(&c->b);
   case AIR_SV_THREADS_PER_GRID:
      if (c->options && c->options->split) {
         const struct air_grid_split *s = c->options->split;

         return nir_imm_ivec3(&c->b, s->grid_threads[0], s->grid_threads[1], s->grid_threads[2]);
      }
      return nir_imul(&c->b, nir_load_num_workgroups(&c->b), nir_load_workgroup_size(&c->b));
   case AIR_SV_SIMD_LANE: return nir_load_subgroup_invocation(&c->b);
   case AIR_SV_SIMD_INDEX: return nir_load_subgroup_id(&c->b);
   case AIR_SV_SIMD_WIDTH: return nir_load_subgroup_size(&c->b);
   /* ir3 has this one and computes it rather than reading a register:
    * ir3_nir_lower_subgroup_id_cs turns load_num_subgroups into
    * 1 + ((threads_per_threadgroup - 1) >> the SIMD-width shift), which folds
    * to a constant whenever the workgroup size is one (ir3_nir.c).
    */
   case AIR_SV_SIMDS_PER_TG: return nir_load_num_subgroups(&c->b);
   case AIR_SV_QUAD_LANE: return nir_iand_imm(&c->b, nir_load_subgroup_invocation(&c->b), 3);
   case AIR_SV_QUAD_INDEX: return nir_ushr_imm(&c->b, nir_load_local_invocation_index(&c->b), 2);
   case AIR_SV_QUADS_PER_TG: {
      nir_def *wg = nir_load_workgroup_size(&c->b);
      nir_def *tot = nir_imul(&c->b, nir_imul(&c->b, nir_channel(&c->b, wg, 0),
                                              nir_channel(&c->b, wg, 1)),
                              nir_channel(&c->b, wg, 2));
      return nir_ushr_imm(&c->b, nir_iadd_imm(&c->b, tot, 3), 2);
   }
   }
   return NULL;
}

/* a system value cut or widened to the type the shader declared */
static nir_def *
fit(struct air_ctx *c, nir_def *def, const struct air_type *t)
{
   unsigned want = air_nir_components(t);
   unsigned bits = air_nir_bit_size(c, t);
   if (def->num_components > want)
      def = nir_trim_vector(&c->b, def, want);
   else if (def->num_components < want)
      def = nir_pad_vector(&c->b, def, want);
   if (def->bit_size != bits)
      def = air_type_is_float(air_type_scalar(t))
               ? nir_u2fN(&c->b, def, bits)
               : nir_u2uN(&c->b, def, bits);
   return def;
}

/* Returns the resource it added, so that a caller with more to say about it --
 * an argument-buffer member, which carries the Metal (set, id) it came from --
 * can fill the rest in; NULL when the node was refused.
 */
static struct air_resource *
add_resource(struct air_ctx *c, enum air_resource_kind kind, unsigned index, unsigned size,
             bool read, bool write, unsigned param, const struct air_md *node)
{
   struct air_shader_info *info = c->info;
   info->resources = reralloc(c->mem_ctx, info->resources, struct air_resource,
                              info->num_resources + 1);
   /* Metal's `[[raster_order_group(n)]]`. Until this was read it was DROPPED IN
    * SILENCE, and that is worth naming rather than just fixing: the branches of
    * translate_input match on the resource's kind and return, so the loop that
    * refuses an `air.*` key nobody handled never runs over a node whose kind is
    * recognised. Every key in such a node is therefore invisible unless
    * something looks for it by name -- which is how a shader could ask for
    * ordering between overlapping fragments and get none, with nothing in any
    * log saying so. `inplace_copy_{lpf,lph}` of QuartzCore's library ask for it
    * on the texture they write.
    */
   const char *name = md_key_str(node, "air.arg_name");
   uint64_t group = 0;
   bool has_group = md_key_int(node, "air.raster_order_group", &group);
   if (has_group) {
      if (group >= 32) {
         /* the bitmask has 32 bits; a group number past them would be recorded
          * as a different group, which is worse than not being recorded */
         air_error(c, "interface: raster order group %u", (unsigned)group);
         return NULL;
      }
      info->raster_order_groups |= 1u << group;
      /* What the hardware can do is not what was asked. The ask is per
       * resource; a6xx's GRAS_SC_CNTL.SINGLE_PRIM_MODE is per draw, and
       * freedreno drives it from pipe_blend_state.blend_coherent
       * (fd6_emit.cc, build_prim_mode). Setting that bit orders the whole draw,
       * which is MORE than the shader asked for -- safe, and slower. There is
       * no way to give exactly the ask and no way to give less, so the shader
       * is translated and the difference is said out loud here instead of being
       * left to a document.
       */
      air_note(c, "raster order group %u on %s %u (%s): a6xx orders a whole draw or "
                  "nothing (GRAS_SC_CNTL.SINGLE_PRIM_MODE), never one resource, so the "
                  "runtime must set pipe_blend_state.blend_coherent and the ordering "
                  "given is coarser than the shader asked for",
               (unsigned)group,
               kind == AIR_RESOURCE_IMAGE ? "image" :
               kind == AIR_RESOURCE_TEXTURE ? "texture" :
               kind == AIR_RESOURCE_SAMPLER ? "sampler" : "buffer",
               index, name ? name : "unnamed");
   }

   info->resources[info->num_resources] = (struct air_resource) {
      .kind = kind, .index = index, .size = size, .read = read, .write = write,
      .param = param,
      .name = name,
      .type_name = md_key_str(node, "air.arg_type_name"),
      .raster_order_group = has_group ? (int)group : -1,
      /* a direct argument: Metal names it by .index and there is no set */
      .argbuf_set = ~0u, .argbuf_id = ~0u,
   };
   return &info->resources[info->num_resources++];
}

static nir_variable *
make_io_var(struct air_ctx *c, nir_variable_mode mode, const struct air_type *t,
            unsigned location, const char *name)
{
   const struct glsl_type *gt;
   unsigned comps = air_nir_components(t);
   unsigned bits = air_nir_bit_size(c, t);
   if (air_type_is_float(air_type_scalar(t)))
      gt = bits == 16 ? glsl_f16vec_type(comps) : glsl_vec_type(comps);
   else
      gt = glsl_vector_type(bits == 16 ? GLSL_TYPE_INT16 : GLSL_TYPE_INT, comps);

   nir_variable *var = nir_variable_create(c->nir, mode, gt, name ? name : "io");
   var->data.location = location;
   return var;
}

/* fragment inputs and vertex outputs take the slots in the order they appear;
 * the runtime links the two sides by name (docs/air.md section 7.5)
 */
static void
add_varying(struct air_ctx *c, const char *name, unsigned slot, unsigned comps,
            const struct air_md *node)
{
   struct air_shader_info *info = c->info;
   info->varyings = reralloc(c->mem_ctx, info->varyings, struct air_varying,
                             info->num_varyings + 1);
   info->varyings[info->num_varyings++] = (struct air_varying) {
      .name = name, .slot = slot, .components = comps,
      .flat = md_key(node, "air.flat") >= 0,
      .centroid = md_key(node, "air.centroid") >= 0,
      .sample = md_key(node, "air.sample") >= 0,
      .no_perspective = md_key(node, "air.no_perspective") >= 0,
   };
}

/* Metal's `[[stage_in]]` / `[[attribute(n)]]`: a vertex attribute, read out of
 * a vertex buffer by the fetch hardware rather than computed by the shader.
 *
 * AIR HAS NO STAGE-IN STRUCT. Apple's compiler flattens it: each member is an
 * ordinary parameter of the entry point carrying its own node, and the struct
 * type does not appear in the module at all. QuartzCore's std_vert0_lpf is the
 * shape, read out of the real library and not inferred:
 *
 *   define <{ <4 x float>, <4 x float>, <2 x float> }> @std_vert0_lpf(
 *       <4 x float> %0, <2 x float> %1, <4 x float> %2, ptr addrspace(2) %3, ...)
 *   !19 = !{!20, !21, !22, !23, !24}
 *   !20 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1,
 *           !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
 *   !21 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1,
 *           !"air.arg_type_name", !"float2", !"air.arg_name", !"texcoord0",
 *           !"air.arg_unused"}
 *   !22 = !{i32 2, !"air.vertex_input", !"air.location_index", i32 3, i32 1,
 *           !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
 *
 * Three things in that are worth saying out loud, because each one would be
 * guessed wrong:
 *
 *   - the index is Metal's `[[attribute(n)]]` and NOT a position in the list.
 *     These three are 0, 1 and 3, so a translator that numbered them in order
 *     would feed `color` from attribute 2 -- a wrong vertex, drawn, with
 *     nothing to say so.
 *   - `air.arg_unused` occurs ON an attribute, and eight of QuartzCore's
 *     fourteen such shaders have one. Nothing reads it, so no NIR input is made:
 *     an input costs a VFD fetch slot and makes the pipeline demand a binding
 *     that the application's MTLVertexDescriptor may legally not have for an
 *     attribute the shader does not read.
 *   - the format, offset, stride and buffer of the attribute are NOWHERE in
 *     the module. They are the MTLVertexDescriptor's, which is API state. What
 *     this builds is the shader's half of the match, recorded in
 *     air_shader_info.vertex_attributes for the runtime to check against it.
 *
 * The NIR input is always 32 bits wide and the value narrowed afterwards, which
 * is a decision with a measurement behind it rather than a convention copied
 * from GL: ir3 is perfectly willing to put a 16-bit vertex input in the half
 * register file -- a `half4` attribute compiles to `@in(hr0.x)` and a
 * `cov.f16f32` -- but `ir3_shader_variant.inputs[]` has no half flag (only
 * `outputs[]` does), prints that input as `r0.x`, and
 * `fd6_program.cc:emit_vfd_dest` programs VFD_DEST_CNTL_INSTR_REGID straight
 * out of that field. The fetch would write the full register while the shader
 * read the half one. 32 bits in and a narrowing instruction keeps this in the
 * regime both freedreno drivers use, and the VFD converts the format either way.
 */
static void
add_vertex_attribute(struct air_ctx *c, const struct air_md *node, unsigned index,
                     unsigned comps, unsigned bits, bool is_float, bool used,
                     unsigned driver_location)
{
   struct air_shader_info *info = c->info;
   const char *tn = md_key_str(node, "air.arg_type_name");

   /* `int4` and `uint4` are the same `<4 x i32>`, so the sign is in Apple's
    * reflection string and in nothing the translator can see. `packed_uchar4`
    * is the same question with a prefix.
    */
   const char *base = tn;
   if (base && !strncmp(base, "packed_", 7))
      base += 7;
   enum air_attribute_base_type bt =
      is_float ? AIR_ATTRIB_FLOAT :
      (base && base[0] == 'u') ? AIR_ATTRIB_UINT : AIR_ATTRIB_INT;

   const struct air_md *fc = md_key_node(node, "air.function_constant");

   info->vertex_attributes = reralloc(c->mem_ctx, info->vertex_attributes,
                                      struct air_vertex_attribute,
                                      info->num_vertex_attributes + 1);
   info->vertex_attributes[info->num_vertex_attributes++] =
      (struct air_vertex_attribute) {
         .index = index,
         .location = VERT_ATTRIB_GENERIC0 + index,
         .driver_location = driver_location,
         .components = comps,
         .bit_size = bits,
         .base_type = bt,
         .name = md_key_str(node, "air.arg_name"),
         .type_name = tn,
         .used = used,
         /* operand 2 of the predicate node is its name; 0 is the global and 1
          * its type (`!"bool"`) */
         .function_constant = md_str(fc, 2),
      };
   if (used)
      info->vertex_attributes_read |= 1u << index;
}

static bool
translate_vertex_attribute(struct air_ctx *c, const struct air_md *node, unsigned param,
                           const struct air_type *t)
{
   const char *tn = md_key_str(node, "air.arg_type_name");
   const char *name = md_key_str(node, "air.arg_name");

   int li = md_key(node, "air.location_index");
   uint64_t index = 0, span = 1;
   if (li < 0 || !md_int(node, li + 1, &index))
      return air_error(c, "interface: a vertex input (%s) with no air.location_index",
                       name ? name : "unnamed");

   /* The second integer after the key. What it MEANS is inferred -- Apple's own
    * form for an array of resources, where it is the number of consecutive
    * indices the argument takes -- and nothing in reach can exercise another
    * value: all 974 air.location_index nodes of the five system libraries and
    * all 4 764 on Resident Evil 4's vertex inputs carry 1. An attribute
    * spanning several indices (a matrix member of a stage-in struct is the case
    * that would) is therefore refused by name rather than silently read from
    * the first of them.
    */
   if (md_int(node, li + 2, &span) && span != 1)
      return air_error(c, "interface: a vertex input (%s) spanning %u attribute indices",
                       name ? name : "unnamed", (unsigned)span);

   /* Metal has 31 attribute slots; NIR's gl_vert_attrib has 16 generic ones,
    * and VERT_ATTRIB_GENERIC0 + 16 is VERT_ATTRIB_EDGEFLAG. Unchecked, index
    * 16 translated onto the edge flag and index 30 past the end of the enum --
    * both compiled through ir3 without a word, which is the failure this
    * refusal exists to replace.
    */
   if (index >= MAX_VERTEX_GENERIC_ATTRIBS)
      return air_error(c, "interface: vertex attribute %u (%s), and NIR has %u generic "
                          "vertex attributes",
                       (unsigned)index, name ? name : "unnamed",
                       MAX_VERTEX_GENERIC_ATTRIBS);

   /* Two members at one index would make two NIR inputs at one location, two
    * VFD fetches into the same register, and a pipeline whose element array is
    * longer than the attribute set it describes. Metal forbids it; refusing is
    * what keeps a malformed module from being drawn.
    */
   for (unsigned i = 0; i < c->info->num_vertex_attributes; i++)
      if (c->info->vertex_attributes[i].index == index)
         return air_error(c, "interface: two vertex inputs at attribute %u (%s and %s)",
                          (unsigned)index,
                          c->info->vertex_attributes[i].name
                             ? c->info->vertex_attributes[i].name : "unnamed",
                          name ? name : "unnamed");

   if (!ret_member_is_number(t))
      return air_error(c, "interface: a vertex input (%s) of type %s, which is neither a "
                          "scalar nor a vector",
                       name ? name : "unnamed", tn ? tn : "?");

   unsigned comps = air_nir_components(t);
   unsigned bits = air_nir_bit_size(c, t);
   bool is_float = air_type_is_float(air_type_scalar(t));
   /* A vertex attribute is one fetch of at most four components; `double` has
    * no place in MSL at all, and an `i1` or an `i64` attribute has no
    * MTLVertexFormat to come from.
    */
   if (comps > 4 || (is_float ? (bits != 16 && bits != 32)
                              : (bits != 8 && bits != 16 && bits != 32)))
      return air_error(c, "interface: a vertex input (%s) of type %s",
                       name ? name : "unnamed", tn ? tn : "?");

   if (md_key(node, "air.arg_unused") >= 0) {
      add_vertex_attribute(c, node, index, comps, bits, is_float, false, ~0u);
      c->params[param] = air_val_ssa(c, nir_undef(&c->b, comps, bits));
      return true;
   }

   unsigned location = VERT_ATTRIB_GENERIC0 + index;
   const struct glsl_type *gt = is_float ? glsl_vec_type(comps)
                                         : glsl_vector_type(GLSL_TYPE_INT, comps);
   nir_variable *var = nir_variable_create(c->nir, nir_var_shader_in, gt,
                                           name ? name : "attr");
   var->data.location = location;
   unsigned driver_location = c->nir->num_inputs++;
   var->data.driver_location = driver_location;
   /* An interpolation mode is a fragment-stage word and INTERP_MODE_SMOOTH on a
    * vertex attribute would be a claim about something that does not happen.
    */
   var->data.interpolation = INTERP_MODE_NONE;

   /* nir_shader_gather_info does not run on this path (ir3 calls it only for a
    * binning variant), so what it would have set is set here, as for the
    * fragment position above. A runtime matching an MTLVertexDescriptor can
    * read either this or air_shader_info.vertex_attributes_read.
    */
   c->nir->info.inputs_read |= BITFIELD64_BIT(location);

   nir_def *v = nir_load_var(&c->b, var);
   if (bits != 32) {
      /* The fetch delivered a full register; the shader asked for a narrower
       * type. For an integer this is a truncation and the same instruction for
       * either sign -- which of the two the HARDWARE must do when it widens an
       * 8- or 16-bit component to 32 bits is the format's business, and the
       * format is the MTLVertexDescriptor's.
       */
      v = is_float ? nir_f2f16(&c->b, v) : nir_u2uN(&c->b, v, bits);
   }
   add_vertex_attribute(c, node, index, comps, bits, is_float, true, driver_location);

   /* An attribute the module reads only when a function-constant predicate is
    * true is still a fetch here, and that is a caveat rather than an error: the
    * parameter exists whatever the predicate says, so the input is built and
    * vertex_attributes_read gets its bit. A runtime that required a binding for
    * every bit of that mask would refuse a perfectly legal pipeline -- one
    * whose function constants say the attribute is absent and whose
    * MTLVertexDescriptor therefore configures nothing at that index. Which it
    * is cannot be decided here, because the value of the predicate is the
    * pipeline's: these globals are written by the module's air.static_init
    * constructor from the constants the API supplies. So the condition goes in
    * the log at translation time rather than only in a document.
    */
   const struct air_md *fc = md_key_node(node, "air.function_constant");
   if (fc) {
      const char *pred = md_str(fc, 2);
      air_note(c, "vertex attribute %u (%s) is gated by the function-constant predicate "
                  "%s: the parameter exists either way, so the attribute is fetched and "
                  "its bit is in vertex_attributes_read, and a pipeline whose constants "
                  "make the predicate false may legally configure nothing at that index "
                  "-- the runtime must read air_vertex_attribute.function_constant before "
                  "demanding a binding",
               (unsigned)index, name ? name : "unnamed", pred ? pred : "(unnamed)");
   }

   c->params[param] = air_val_ssa(c, v);
   return true;
}

#include "air_argbuf.inc"

static bool
translate_input(struct air_ctx *c, const struct air_md *node, unsigned *next_varying)
{
   uint64_t param;
   if (!md_int(node, 0, &param))
      return air_error(c, "interface: an input without a parameter number");
   if (param >= c->fn->num_args)
      return air_error(c, "interface: input for parameter %u of %u", (unsigned)param,
                       c->fn->num_args);
   const struct air_type *t = c->fn->args[param].type;

   if (md_key(node, "air.buffer") >= 0) {
      uint64_t as = 0, index = 0, size = 0;
      md_key_int(node, "air.address_space", &as);
      md_key_int(node, "air.location_index", &index);
      md_key_int(node, "air.buffer_size", &size);
      bool read = md_key(node, "air.read") >= 0 || md_key(node, "air.read_write") >= 0;
      bool write = md_key(node, "air.write") >= 0 || md_key(node, "air.read_write") >= 0;

      struct air_ptr p = { .binding = index, .pointee = t->elem };
      enum air_resource_kind rk;
      switch (as) {
      case 1: p.class = AIR_PTR_SSBO; rk = AIR_RESOURCE_DEVICE_BUFFER; break;
      case 2: p.class = AIR_PTR_UBO; rk = AIR_RESOURCE_CONSTANT_BUFFER; break;
      case 3:
         p.class = AIR_PTR_SHARED;
         rk = AIR_RESOURCE_THREADGROUP_BUFFER;
         /* Specialization can retain the argument after removing its uses. */
         if (md_key(node, "air.arg_unused") >= 0) {
            c->params[param] = air_val_ptr(c, p);
            return true;
         }
         if (index != 0)
            return air_error(c, "interface: a second threadgroup buffer (index %u)",
                             (unsigned)index);
         break;
      default:
         return air_error(c, "interface: a buffer in address space %u", (unsigned)as);
      }
      add_resource(c, rk, index, size, read, write, param, node);
      c->params[param] = air_val_ptr(c, p);
      if (p.class == AIR_PTR_SHARED)
         c->dynamic_threadgroup = c->params[param];
      return true;
   }

   if (md_key(node, "air.indirect_buffer") >= 0)
      return translate_argument_buffer(c, node, param);

   if (md_key(node, "air.imageblock") >= 0) {
      uint64_t ib_size = 0;
      md_key_int(node, "air.imageblock_data_size", &ib_size);
      return air_translate_imageblock_input(c, (unsigned)param, t,
                                            md_key(node, "explicit") >= 0, ib_size);
   }

   if (md_key(node, "air.texture") >= 0) {
      int li = md_key(node, "air.location_index");
      uint64_t index = 0, span = 1;
      if (li >= 0) {
         md_int(node, li + 1, &index);
         if (!md_int(node, li + 2, &span) || !span)
            span = 1;
      }
      bool sample = md_key(node, "air.sample") >= 0;
      bool read = md_key(node, "air.read") >= 0 || md_key(node, "air.read_write") >= 0;
      bool write = md_key(node, "air.write") >= 0 || md_key(node, "air.read_write") >= 0;
      struct air_ptr p = {
         .class = span > 1 ? AIR_PTR_TEXTURE_ARRAY : AIR_PTR_TEXTURE,
         .binding = index,
         .span = span,
         .is_image = write,
      };
      if (md_key(node, "air.arg_unused") < 0) {
         for (uint64_t k = 0; k < span; k++)
            add_resource(c, write ? AIR_RESOURCE_IMAGE : AIR_RESOURCE_TEXTURE, index + k, 0,
                         sample || read, write, param, node);
      }
      c->params[param] = air_val_ptr(c, p);
      return true;
   }

   if (md_key(node, "air.sampler") >= 0) {
      uint64_t index = 0;
      md_key_int(node, "air.location_index", &index);
      struct air_ptr p = { .class = AIR_PTR_SAMPLER, .binding = index };
      /* inactive as an unused texture is, see there */
      if (md_key(node, "air.arg_unused") < 0)
         add_resource(c, AIR_RESOURCE_SAMPLER, index, 0, true, false, param, node);
      c->params[param] = air_val_ptr(c, p);
      return true;
   }

   /* system values */
   for (unsigned i = 0; i < ARRAY_SIZE(sysvals); i++) {
      if (md_key(node, sysvals[i].key) < 0)
         continue;
      nir_def *def = load_sysval(c, sysvals[i].sv);
      if (!def)
         return air_error(c, "interface: %s", sysvals[i].key);
      c->params[param] = air_val_ssa(c, fit(c, def, t));
      return true;
   }

   /* interpolated inputs of a fragment shader, and vertex attributes */
   if (md_key(node, "air.position") >= 0) {
      if (c->nir->info.stage == MESA_SHADER_FRAGMENT) {
         /* A position the shader does not read gets no input at all, and the order of
          * these two tests is the whole reason the test is here: every one of Apple's
          * seventeen texturedFragment* declares `float4 pos [[position]]` and marks it
          * air.arg_unused, so putting the position machinery before this check would
          * give all seventeen a dead varying slot and a dead FS_COORD property -- a
          * change to 17 shaders already compared pixel by pixel, bought for nothing.
          */
         if (md_key(node, "air.arg_unused") >= 0) {
            c->params[param] = air_val_ssa(c, nir_undef(&c->b, air_nir_components(t),
                                                        air_nir_bit_size(c, t)));
            return true;
         }
         /*
          * Metal's [[position]] in a fragment function is the window coordinate with
          * the origin at the TOP left, y down, and the pixel centre at a half-integer
          * -- which is what origin_upper_left plus !pixel_center_integer says in NIR.
          * NIR's default is OpenGL's bottom-left origin, so leaving these unset would
          * give every position-dependent Metal shader a y counted from the wrong edge.
          *
          * And the value is a SYSTEM-VALUE VARIABLE, not a bare nir_load_frag_coord,
          * because the two Gallium drivers this frontend feeds want opposite forms and
          * only one direction has a pass:
          *
          *   - freedreno sets pipe_caps.fs_position_is_sysval, and ir3 compile_asserts
          *     `slot != VARYING_SLOT_POS` in its fragment-input setup
          *     (ir3_compiler_nir.c): it will only take the intrinsic, which is what
          *     nir_lower_system_values turns this variable into;
          *   - softpipe does NOT set that cap, and nir_to_tgsi turns the intrinsic into
          *     ureg_DECL_system_value(TGSI_SEMANTIC_POSITION), which softpipe fills only
          *     for compute (sp_compute.c is the one writer of machine->SystemValue). A
          *     fragment shader therefore read (0,0,0,0) -- measured, not reasoned: a
          *     hand-written module returning its own position printed four zeros while
          *     the same module returning a constant printed the constant. Softpipe wants
          *     the position as an input varying, which only
          *     nir_lower_sysvals_to_varyings produces, and only from a variable.
          *
          * So the frontend emits the form spirv_to_nir emits, for the same reason, and
          * the consumer lowers it per its screen's cap (finalize_for_gallium).
          */
         nir_variable *var = nir_variable_create(c->nir, nir_var_system_value,
                                                 glsl_vec4_type(), "position");
         var->data.location = SYSTEM_VALUE_FRAG_COORD;
         var->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
         nir_def *pos = nir_load_var(&c->b, var);
         c->params[param] = air_val_ssa(c, fit(c, pos, t));
         c->nir->info.fs.origin_upper_left = true;
         c->nir->info.fs.pixel_center_integer = false;
         /* nothing in this path calls nir_shader_gather_info, and nir_to_tgsi emits the
          * FS_COORD_ORIGIN property only for a shader whose info says it reads the
          * position -- without this the property is absent and softpipe falls back on
          * TGSI_FS_COORD_ORIGIN_UPPER_LEFT (0), which is the right answer by accident.
          */
         BITSET_SET(c->nir->info.system_values_read, SYSTEM_VALUE_FRAG_COORD);
         c->nir->info.inputs_read |= VARYING_BIT_POS;
         return true;
      }
      return air_error(c, "interface: air.position as an input of a %s shader",
                       _mesa_shader_stage_to_string(c->nir->info.stage));
   }

   /* Metal's `[[color(n)]]` argument to a fragment function: the value render target n
    * holds NOW, read inside the shader. That is framebuffer fetch, and Metal's name for
    * it is programmable blending.
    *
    * The modules say which feature it is rather than this being inferred from the name:
    * every one of the twenty in QuartzCore's library carries
    * `!"air.compile.framebuffer_fetch_enable"` among its `air.compile_options`, the
    * option Apple's compiler sets exactly when a fragment argument is `[[color(n)]]`.
    *
    * The shape is ONE integer after the key -- the target -- where the output form has
    * two, the second being the dual-source index:
    *
    *   output  !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", ...}
    *   input   !{i32 1, !"air.render_target", i32 2, !"air.arg_type_name", !"float", ...}
    *
    * so the two are read here and in translate_output at their own offsets, not through
    * one shared helper that would read a dual-source index out of an argument name.
    *
    * NIR's form of a framebuffer fetch is a READ OF THE OUTPUT: load_output carrying
    * io_semantics.fb_fetch_output, which is what nir_lower_io makes of a GLSL `inout`
    * colour and what nir_lower_blend emits for a blend it has to lower in software. The
    * intrinsic is built here directly instead of as a second nir_var_shader_out variable
    * at the same location, because both consumers of this frontend lower IO before the
    * backend sees it and two output variables at one location would take two driver
    * locations and two entries in ir3's output table for one attachment.
    *
    * It is read as FOUR 32-BIT FLOATS whatever the argument's type, and narrowed after:
    * nir_lower_fb_read -- the pass ir3 runs in ir3_nir_post_finalize -- rewrites
    * load_output into a txf_ms_fb whose destination is fixed at 4 x f32 and rewrites the
    * intrinsic's uses with it. So `float` (path_blit_frag's coverage mask) loses three
    * channels and `half4` (every _lph build) loses precision here, where the conversion
    * is visible, rather than silently inside the lowering.
    */
   int rt_in = md_key(node, "air.render_target");
   if (rt_in >= 0) {
      if (c->nir->info.stage != MESA_SHADER_FRAGMENT)
         return air_error(c, "interface: air.render_target as an input of a %s shader",
                          _mesa_shader_stage_to_string(c->nir->info.stage));
      /* An argument nothing reads must not turn the shader into a framebuffer-fetch
       * shader: info.fs.uses_fbfetch_output alone makes a6xx flush the pipeline per
       * overlapping primitive (fd6_emit.cc, build_prim_mode), and an unused argument
       * needs no value at all. None of the corpus's twenty is marked this way; the
       * order is here so that one would cost nothing rather than a flush.
       */
      if (md_key(node, "air.arg_unused") >= 0) {
         c->params[param] = air_val_ssa(c, nir_undef(&c->b, air_nir_components(t),
                                                     air_nir_bit_size(c, t)));
         return true;
      }
      uint64_t n = 0;
      if (!md_int(node, rt_in + 1, &n))
         return air_error(c, "interface: an air.render_target input without a target");
      if (n >= MAX_DRAW_BUFFERS)
         return air_error(c, "interface: air.render_target input %u", (unsigned)n);
      /* Only a float target. The lowering hands back floats converted from the
       * attachment's format, so an int4 or uint4 `[[color(n)]]` would arrive as the
       * wrong value rather than the wrong type -- and nothing would say so. The corpus
       * declares float4, half4 and float and nothing else, so the refusal is a guard on
       * a case not yet seen rather than a gap with modules behind it.
       */
      unsigned bits = air_nir_bit_size(c, t);
      if (!air_type_is_float(air_type_scalar(t)) || (bits != 16 && bits != 32)) {
         const char *tn = md_key_str(node, "air.arg_type_name");
         return air_error(c, "interface: an air.render_target input of type %s",
                          tn ? tn : "?");
      }

      nir_io_semantics sem = {
         .location = FRAG_RESULT_DATA0 + n,
         .num_slots = 1,
         .fb_fetch_output = true,
         /* Coherent, which is Metal's guarantee and not a safe default picked here: a
          * `[[color(n)]]` read sees what the primitives already drawn left behind, which
          * is the whole of how CoreAnimation blends a layer in place. On a6xx the flag
          * is what makes the GMEM path flush per overlap instead of not at all
          * (fd6_emit.cc); the sysmem path flushes either way.
          */
         .fb_fetch_output_coherent = true,
      };
      nir_def *fetched = nir_load_output(&c->b, 4, 32, nir_imm_int(&c->b, 0),
                                         .dest_type = nir_type_float32,
                                         .io_semantics = sem);
      unsigned want = air_nir_components(t);
      if (want < 4)
         fetched = nir_trim_vector(&c->b, fetched, want);
      if (bits == 16)
         fetched = nir_f2f16(&c->b, fetched);

      /* nir_shader_gather_info does not run on this path -- ir3 calls it only for a
       * binning variant -- so what it would have set from the intrinsic
       * (nir_gather_info.c) is set here. Without uses_fbfetch_output the draw gets no
       * flush at all and the read returns whatever the tile buffer happened to hold.
       */
      c->nir->info.outputs_read |= BITFIELD64_BIT(sem.location);
      c->nir->info.fs.uses_fbfetch_output = true;
      c->nir->info.fs.fbfetch_coherent = true;
      c->info->fbfetch_targets |= 1u << n;

      /* For 2D single-layer render targets, framebuffer fetch must use constant
       * layer 0 rather than uninitialized VPC registers. nir_lower_fb_read checks
       * whether VARYING_SLOT_LAYER is present in inputs_read: for 2D pipelines
       * where the vertex shader does not declare gl_Layer, it binds layer to
       * nir_imm_int(b, 0) instead of nir_load_layer_id(b). This prevents
       * ir3_nir_lower_layer_id from generating an unlinked load_input that reads
       * dirty VPC register r1.z, which would fail hardware array bounds checks
       * on Adreno A830 (array_size=1) and cause widgets to render as pure black.
       * For layered render targets, the vertex stage writes VARYING_SLOT_LAYER
       * and the true layer ID is preserved.
       *
       * Keep inputs_read consistent so that lower_fb_read defaults cleanly to 0
       * for single-layer targets without creating varying input linkage.
       */
      air_note(c, "framebuffer fetch: default layer is constant 0 for 2D targets, "
                  "preserving VARYING_SLOT_LAYER for layered render targets");
      if (!(c->nir->info.inputs_read & BITFIELD64_BIT(VARYING_SLOT_LAYER)))
         c->nir->info.inputs_read &= ~BITFIELD64_BIT(VARYING_SLOT_LAYER);

      c->params[param] = air_val_ssa(c, fetched);
      return true;
   }

   if (md_key(node, "air.fragment_input") >= 0 || md_key(node, "air.vertex_input") >= 0 ||
       md_key(node, "air.stage_in") >= 0) {
      /* A vertex attribute and an interpolated varying are two different things
       * that AIR spells with keys in the same family, and only the one below is
       * a varying: it is linked to the stage before it, interpolated, and takes
       * its slot from the order it appears in. An attribute is fetched from a
       * vertex buffer at Metal's own index and has nothing to interpolate.
       */
      if (c->nir->info.stage == MESA_SHADER_VERTEX)
         return translate_vertex_attribute(c, node, param, t);
      unsigned slot = VARYING_SLOT_VAR0 + (*next_varying)++;
      const char *name = md_key_str(node, "air.arg_name");
      nir_variable *var = make_io_var(c, nir_var_shader_in, t, slot, name);
      var->data.driver_location = c->nir->num_inputs++;
      if (md_key(node, "air.flat") >= 0)
         var->data.interpolation = INTERP_MODE_FLAT;
      else if (md_key(node, "air.no_perspective") >= 0)
         var->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
      else
         var->data.interpolation = INTERP_MODE_SMOOTH;
      var->data.centroid = md_key(node, "air.centroid") >= 0;
      var->data.sample = md_key(node, "air.sample") >= 0;
      if (c->nir->info.stage == MESA_SHADER_FRAGMENT)
         add_varying(c, name, slot, air_nir_components(t), node);
      c->params[param] = air_val_ssa(c, nir_load_var(&c->b, var));
      return true;
   }

   /* the one thing left that is not an error: an argument nothing reads */
   if (md_key(node, "air.arg_unused") >= 0) {
      if (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY)
         c->params[param] = air_val_agg(c, t->kind == AIR_TYPE_STRUCT ? t->num_members : t->count);
      else
         c->params[param] = air_val_ssa(c, nir_undef(&c->b, MAX2(air_nir_components(t), 1),
                                                     MAX2(air_nir_bit_size(c, t), 1)));
      return true;
   }

   for (unsigned i = 0; i < node->num_ops; i++) {
      const char *s = md_str(node, i);
      if (s && !strncmp(s, "air.", 4) && strcmp(s, "air.arg_name") &&
          strcmp(s, "air.arg_type_name"))
         return air_error(c, "interface: input %s", s);
   }
   return air_error(c, "interface: an input with no key this translator knows");
}

static bool
translate_output(struct air_ctx *c, const struct air_md *node, unsigned field,
                 const struct air_type *t, unsigned *next_varying)
{
   unsigned location = ~0u, index = 0;
   const char *name = md_key_str(node, "air.arg_name");

   int rt = md_key(node, "air.render_target");
   if (rt >= 0) {
      uint64_t n = 0, dual = 0;
      md_int(node, rt + 1, &n);
      md_int(node, rt + 2, &dual);
      location = FRAG_RESULT_DATA0 + n;
      index = dual;
      c->info->num_render_targets = MAX2(c->info->num_render_targets, n + 1);
   } else if (md_key(node, "air.depth") >= 0) {
      location = FRAG_RESULT_DEPTH;
      const char *q = md_key_str(node, "air.depth_qualifier");
      c->nir->info.fs.depth_layout =
         !q || !strcmp(q, "air.any") ? FRAG_DEPTH_LAYOUT_ANY :
         !strcmp(q, "air.greater") ? FRAG_DEPTH_LAYOUT_GREATER :
         !strcmp(q, "air.less") ? FRAG_DEPTH_LAYOUT_LESS : FRAG_DEPTH_LAYOUT_UNCHANGED;
   } else if (md_key(node, "air.sample_mask") >= 0) {
      location = FRAG_RESULT_SAMPLE_MASK;
   } else if (md_key(node, "air.stencil") >= 0) {
      location = FRAG_RESULT_STENCIL;
   } else if (md_key(node, "air.position") >= 0) {
      location = VARYING_SLOT_POS;
   } else if (md_key(node, "air.point_size") >= 0) {
      location = VARYING_SLOT_PSIZ;
   } else if (md_key(node, "air.clip_distance") >= 0) {
      location = VARYING_SLOT_CLIP_DIST0;
   } else if (md_key(node, "air.render_target_array_index") >= 0) {
      location = VARYING_SLOT_LAYER;
   } else if (md_key(node, "air.viewport_array_index") >= 0) {
      location = VARYING_SLOT_VIEWPORT;
   } else if (md_key(node, "air.vertex_output") >= 0) {
      location = VARYING_SLOT_VAR0 + (*next_varying)++;
      add_varying(c, md_str(node, md_key(node, "air.vertex_output") + 1), location,
                  air_nir_components(t), node);
   } else {
      for (unsigned i = 0; i < node->num_ops; i++) {
         const char *s = md_str(node, i);
         if (s && !strncmp(s, "air.", 4) && strcmp(s, "air.arg_name") &&
             strcmp(s, "air.arg_type_name"))
            return air_error(c, "interface: output %s", s);
      }
      return air_error(c, "interface: an output with no key this translator knows");
   }

   /* `air.clip_distance` arrives as an ARRAY of float: one element in Resident Evil 4's
    * vertex shaders, two in MapKit's DaVinci::elevated* (kernel-12377-lens2-w19), whose
    * buildings were left black while the pipeline was refused. NIR's form is a compact
    * float array at VARYING_SLOT_CLIP_DIST0; emit_return stores it element by element.
    */
   if (location == VARYING_SLOT_CLIP_DIST0 && t->kind == AIR_TYPE_ARRAY && t->count >= 1 &&
       t->count <= 8 && t->elem && air_type_is_float(t->elem) && air_nir_bit_size(c, t->elem) == 32) {
      nir_variable *var = nir_variable_create(c->nir, nir_var_shader_out,
                                              glsl_array_type(glsl_float_type(), t->count, 0),
                                              name ? name : "clip_distance");
      var->data.location = location;
      var->data.compact = true;
      var->data.driver_location = c->nir->num_outputs++;
      c->nir->info.clip_distance_array_size = t->count;
      c->info->clip_distances = t->count;
      c->outputs[field].location = location;
      c->outputs[field].index = 0;
      c->outputs[field].var = var;
      return true;
   }

   /* Any other output whose type is not a number has no varying here, and NIR's own form
    * for it is an output of several slots, which this frontend does not build, so it is
    * refused by name.
    */
   if (t->kind == AIR_TYPE_ARRAY || t->kind == AIR_TYPE_STRUCT) {
      const char *tn = md_key_str(node, "air.arg_type_name");
      if (t->kind == AIR_TYPE_ARRAY)
         return air_error(c, "interface: an output (%s), declared %s, that arrives as an "
                             "array of %u, which this frontend has no varying for",
                          name ? name : "unnamed", tn ? tn : "an aggregate",
                          (unsigned)t->count);
      return air_error(c, "interface: an output (%s), declared %s, that arrives as a "
                          "struct of %u members, which this frontend has no varying for",
                       name ? name : "unnamed", tn ? tn : "an aggregate", t->num_members);
   }

   nir_variable *var = make_io_var(c, nir_var_shader_out, t, location, name);
   var->data.index = index;
   var->data.driver_location = c->nir->num_outputs++;
   c->outputs[field].location = location;
   c->outputs[field].index = index;
   c->outputs[field].var = var;
   return true;
}

