/* Module reflection, index helpers, depth/stencil, and vertex state, included into air_metal.c. */

int
rwa_finish(struct rwa_device *device, char *why, size_t why_size)
{
   RWA_LOCKED;
   rwa_deferred_drain_locked(device);
   return finish_or_say_why(device, false, "work", why, why_size);
}

/*
 * ---------------------------------------------------------------------------------------
 * Drawing.
 *
 * air_run draws every fragment shader of the corpus and 255 of them have been compared
 * pixel by pixel (docs/air.md): a quad vertex shader built by hand in NIR plus the real
 * fragment shader, a framebuffer of plain textures, clear, draw_vbo of a TRIANGLE_STRIP
 * of four, and the pixel read back. This is that path as the three contract calls, and
 * the code and its WHY comments are lifted from air_run.c because each is a mistake
 * someone already made.
 *
 * Where it differs from a dispatch is only the stage: a draw binds the fragment shader's
 * resources with the very same bind_stage the dispatch uses, and when a vertex module is
 * supplied binds its resources too. The reconnaissance that fixed the contract's shape
 * is docs/metal.md 10.18 -- nothing in Apple's Metal reads MTLRenderPassDescriptor, the
 * render target is a plain texture, and the render path adds no kernel ABI over compute.
 */

/* A full-screen quad covering the whole attachment, in clip space. A TRIANGLE_STRIP of
 * these four is what air_run draws and what rwa_render_draw expects when the library
 * supplied the vertex stage (rwa_quad_verts is defined in air_metal.c).
 */

/* The texel the generated vertex stage puts under every fragment, as air_run does
 * (TEX_SIZE/TEX_X/TEX_Y there). It matters only to a fragment shader that samples a
 * texture: there is no real vertex stage to interpolate a coordinate, so one constant
 * coordinate is handed to every fragment, and a shader reading a texture reads the same
 * texel at every pixel. The value is air_run's exactly, so a pixel drawn here and one
 * drawn there are the same measurement; a driver that wants interpolated coordinates
 * across real geometry needs a real vertex shader, which is the vertex_bitcode path.
 */
#define RWA_TEX_SIZE 4
#define RWA_TEX_X 1
#define RWA_TEX_Y 1

/* A vertex shader for the quad: it takes the position as attribute 0 and passes it
 * through. Written here rather than translated, because what is under test is the
 * fragment shader; when a .metallib has a vertex shader of its own that is the
 * vertex_bitcode path and nothing is generated. This is air_run.c's quad_vertex_shader.
 */
static nir_shader *
quad_vertex_shader(const struct nir_shader_compiler_options *options, unsigned num_varyings)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options, "quad");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "pos_in");
   in->data.location = VERT_ATTRIB_GENERIC0;
   in->data.driver_location = 0;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "pos_out");
   out->data.location = VARYING_SLOT_POS;
   out->data.driver_location = 0;
   nir_store_var(&b, out, nir_load_var(&b, in), 0xf);

   /* One constant coordinate per varying the fragment shader reads: the centre of texel
    * (TEX_X, TEX_Y), so every fragment samples the same texel and the expected value is
    * a single known one.
    */
   float c = (RWA_TEX_X + 0.5f) / RWA_TEX_SIZE;
   float d = (RWA_TEX_Y + 0.5f) / RWA_TEX_SIZE;
   for (unsigned i = 0; i < num_varyings; i++) {
      nir_variable *v = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "varying");
      v->data.location = VARYING_SLOT_VAR0 + i;
      v->data.driver_location = 1 + i;
      v->data.interpolation = INTERP_MODE_SMOOTH;
      nir_store_var(&b, v, nir_imm_vec4(&b, c, d, c, 1.0f), 0xf);
   }
   b.shader->num_inputs = 1;
   b.shader->num_outputs = 1 + num_varyings;
   return b.shader;
}

/* MTLPrimitiveType in Gallium's terms. The names are Metal's own enumeration. */
static bool
mesa_prim_from_metal(unsigned p, enum mesa_prim *out, char *why, size_t why_size)
{
   switch (p) {
   case 0: *out = MESA_PRIM_POINTS;         return true;
   case 1: *out = MESA_PRIM_LINES;          return true;
   case 2: *out = MESA_PRIM_LINE_STRIP;     return true;
   case 3: *out = MESA_PRIM_TRIANGLES;      return true;
   case 4: *out = MESA_PRIM_TRIANGLE_STRIP; return true;
   default:
      say(why, why_size, "primitive type %u is not one of MTLPrimitiveType's five", p);
      return false;
   }
}

/*
 * MTLIndexType in bytes. Metal has exactly two values, UInt16 = 0 and UInt32 = 1
 * (MTLStageInputOutputDescriptor.h), and there is no third: an eight-bit index type
 * exists on both of these back ends (`pc_di_index_size` has INDEX_SIZE_8_BIT, and the
 * draw module switches on eltSize 1) and in Metal it does not, so a value this does not
 * know is refused by name rather than guessed into a size. The number is in the refusal
 * because that is the one thing a caller whose struct layout does not match this
 * library's will show here (see rwa_draw's own comment).
 */
static bool
index_size_from_metal(unsigned type, unsigned *out, char *why, size_t why_size)
{
   switch (type) {
   case 0: *out = 2; return true;   /* MTLIndexTypeUInt16 */
   case 1: *out = 4; return true;   /* MTLIndexTypeUInt32 */
   default:
      say(why, why_size, "index type %u is neither of MTLIndexType's two values "
                         "(MTLIndexTypeUInt16 = 0, MTLIndexTypeUInt32 = 1)", type);
      return false;
   }
}

/* The name of an index type, for the sentences below. */
static const char *
index_type_name(unsigned type)
{
   return type == 0 ? "MTLIndexTypeUInt16" : "MTLIndexTypeUInt32";
}

/*
 * The smallest and largest index the draw will read, which has to be walked rather than
 * assumed, and this is the only reader of the index buffer on the processor.
 *
 * Mesa does the same walk in the same place and with the same convention
 * (u_vbuf_get_minmax_index: `count` elements from offset start * index_size, and the
 * values are the raw indices with no index_bias added), so these numbers mean what a
 * Gallium driver's min_index/max_index mean. Gallium's own helper for an indexed draw,
 * util_draw_elements, instead leaves index_bounds_valid false with max_index 0xffffffff,
 * which is legal and is what the draw module falls back to (draw_pt.c: min 0, max ~0);
 * walking is still worth the pass, for two reasons that have nothing to do with speed:
 * the generated vertex stage has a vertex buffer of exactly four elements and an index
 * past it must be refused rather than clamped, and the bounds let a driver's fetch stay
 * inside what it was told.
 *
 * The cost is honest and it is O(count) on the processor at every draw: an application
 * drawing a hundred thousand indices pays for a hundred thousand loads here. Mesa pays
 * the same cost in u_vbuf and caches it per buffer; nothing is cached here yet, and the
 * draw already waits for the GPU on this thread (rwa_render_draw's fence), which is the
 * larger of the two by far.
 */
/* The restart index (all ones for the index size, see rwa_render_draw's pipe_draw_info) is left
 * out: it names no vertex. With nothing else in the buffer both bounds are 0. */
static void
index_range(const uint8_t *indices, unsigned count, unsigned index_size, unsigned *min_out,
            unsigned *max_out)
{
   unsigned min = UINT32_MAX, max = 0;
   unsigned restart = index_size == 2 ? 0xffffu : 0xffffffffu;
   for (unsigned i = 0; i < count; i++) {
      unsigned v;
      if (index_size == 2) {
         uint16_t u;
         memcpy(&u, indices + (size_t)i * 2, sizeof(u));
         v = u;
      } else {
         uint32_t u;
         memcpy(&u, indices + (size_t)i * 4, sizeof(u));
         v = u;
      }
      if (v == restart)
         continue;
      if (v < min)
         min = v;
      if (v > max)
         max = v;
   }
   *min_out = min <= max ? min : 0;
   *max_out = max;
}

/* What a stage expects to be bound, counted the way Metal counts it: by the highest
 * index plus one, because the indices are the binding numbers. This is rwa_compute_create's
 * own counting, pointed at a stage's resources, and it fills in the texture slots the
 * same way (sampled vs stored, read, write) so texture_fits has something to check a
 * bound texture against.
 */
static void
count_resources(const struct air_shader_info *info, struct rwa_slot slots[RWA_MAX_TEXTURES],
                unsigned *buffers, unsigned *textures, unsigned *samplers)
{
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->unused)
         continue;
      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER:
      case AIR_RESOURCE_DEVICE_BUFFER:
         *buffers = MAX2(*buffers, r->index + 1);
         break;
      case AIR_RESOURCE_TEXTURE:
         *textures = MAX2(*textures, r->index + 1);
         if (r->index < RWA_MAX_TEXTURES) {
            slots[r->index].sampled = true;
            slots[r->index].read |= r->read;
            slots[r->index].write |= r->write;
            declare_slot_element(r->type_name, &slots[r->index]);
         }
         break;
      case AIR_RESOURCE_IMAGE:
         *textures = MAX2(*textures, r->index + 1);
         if (r->index < RWA_MAX_TEXTURES) {
            slots[r->index].stored = true;
            slots[r->index].read |= r->read;
            slots[r->index].write |= r->write;
            declare_slot_element(r->type_name, &slots[r->index]);
         }
         break;
      case AIR_RESOURCE_SAMPLER:
         if (!r->inline_sampler)
            *samplers = MAX2(*samplers, r->index + 1);
         break;
      case AIR_RESOURCE_THREADGROUP_BUFFER:
         /* Metal has no threadgroup memory in a fragment or vertex stage; nothing to
          * count. A dispatch is where one is met, and refused there.
          */
         break;
      }
   }
}

/*
 * What textures a module declares (air_metal.h). Here rather than beside the texture
 * calls because it is count_resources and slot_shapes that answer it, and those are the
 * two halves a pipeline fills its own slots with: the same metadata and the same
 * instructions, read once more for a caller that has to bind before it has a pipeline.
 *
 * It takes a device and not just the bitcode on purpose. air_to_nir is handed
 * device->nir_options, exactly as rwa_compute_create and rwa_render_create hand it, so
 * the shader this reads is the shader that pipeline will hold. A query that translated
 * with options of its own would be a second translation with a second answer, and the
 * point of this call is that there is only one.
 *
 * It is NOT asked of a pipeline, which would avoid the second translation altogether and
 * was the first shape tried. A pipeline cannot answer for a module that gets none -- an
 * air.visible library function, or a fragment shader handed to the compute path -- and a
 * third of the corpus is air.visible, so the host-side table of what every function
 * declares would have holes exactly where a reader would most want a row. The cost of
 * the extra translation is one shader per query, which is nothing beside a draw.
 */
int
rwa_module_textures(struct rwa_device *device, const void *bitcode, size_t size,
                    const char *entry, struct rwa_texture_decl *out, unsigned max,
                    unsigned *count, char *why, size_t why_size)
{
   if (count)
      *count = 0;
   if (!device) {
      say(why, why_size, "no device to read a module with");
      return -1;
   }
   if (!bitcode || size < 4) {
      say(why, why_size, "%zu bytes is not a shader module", size);
      return -1;
   }
   if (!out && max) {
      say(why, why_size, "nowhere to put %u texture declarations", max);
      return -1;
   }

   void *mem_ctx = ralloc_context(NULL);
   if (!mem_ctx) {
      say(why, why_size, "out of memory reading a module");
      return -1;
   }

   /* Function constants taken as zero, which is rwa_compute_create's reason verbatim:
    * there is nothing to take them from, and the translator refuses what it cannot fold.
    */
   struct air_constant_value *constants;
   unsigned num_constants =
      air_module_function_constants(mem_ctx, bitcode, size, &constants);
   const struct air_to_nir_options options = {
      .constants = constants,
      .num_constants = num_constants,
   };
   struct air_shader_info info = { 0 };
   const char *error;
   nir_shader *nir = air_to_nir(mem_ctx, bitcode, size, entry, &options,
                                device->nir_options, &info, &error);
   if (!nir) {
      say(why, why_size, "%s", error ? error : "the module could not be translated");
      ralloc_free(mem_ctx);
      return -1;
   }
   const char *name = info.entry_point ? info.entry_point : "the function";

   /* A slot past the sixteen is refused here and by name, on check_bindings' grounds: a
    * caller told "three textures" that cannot be told WHICH three would bind the wrong
    * ones and dispatch over them.
    */
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];
      if (r->unused)
         continue;
      if ((r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_IMAGE) &&
          r->index >= RWA_MAX_TEXTURES) {
         say(why, why_size, "%s binds a texture at index %u, past the %u slots this "
                            "library fills", name, r->index, RWA_MAX_TEXTURES);
         ralloc_free(mem_ctx);
         return -1;
      }
   }

   struct rwa_slot slots[RWA_MAX_TEXTURES] = { 0 };
   unsigned buffers = 0, textures = 0, samplers = 0;
   count_resources(&info, slots, &buffers, &textures, &samplers);
   slot_shapes(nir, slots, NULL);

   unsigned n = 0;
   for (unsigned i = 0; i < RWA_MAX_TEXTURES; i++) {
      if (!slots[i].sampled && !slots[i].stored)
         continue;
      if (n >= max) {
         say(why, why_size, "%s declares a texture at slot %u and there is room for %u "
                            "declaration%s", name, i, max, max == 1 ? "" : "s");
         ralloc_free(mem_ctx);
         return -1;
      }

      struct rwa_texture_decl *d = &out[n++];
      memset(d, 0, sizeof(*d));
      d->index = i;

      /* The usage bits check_bindings will insist on, and not a superset of them: a
       * texture made ShaderRead|ShaderWrite where the shader only samples is a texture
       * this GPU may lay out differently for nothing.
       */
      if (slots[i].sampled || slots[i].read)
         d->usage |= RWA_USAGE_SHADER_READ;
      if (slots[i].stored || slots[i].write)
         d->usage |= RWA_USAGE_SHADER_WRITE;

      if (slots[i].shape_known)
         d->type_known = metal_texture_type(slots[i].dim, slots[i].is_array, &d->type);

      nir_alu_type have;
      const char *word;
      unsigned bits;
      if (slot_element(&slots[i], &have, &word, &bits)) {
         d->element = have == nir_type_float ? RWA_ELEMENT_FLOAT
                    : have == nir_type_int   ? RWA_ELEMENT_INT
                    : have == nir_type_uint  ? RWA_ELEMENT_UINT
                                             : RWA_ELEMENT_UNKNOWN;
         if (d->element != RWA_ELEMENT_UNKNOWN)
            d->element_bits = bits;
      }

      /* Apple's own words for the whole type, copied: the strings live under mem_ctx,
       * which this call frees, and a caller printing a table wants the declaration as
       * its author wrote it rather than this library's reconstruction of it.
       */
      for (unsigned k = 0; k < info.num_resources; k++) {
         const struct air_resource *r = &info.resources[k];
         if (r->unused)
            continue;
         if ((r->kind != AIR_RESOURCE_TEXTURE && r->kind != AIR_RESOURCE_IMAGE) ||
             r->index != i || !r->type_name)
            continue;
         snprintf(d->metal_type, sizeof(d->metal_type), "%s", r->type_name);
         break;
      }
   }

   if (count)
      *count = n;
   ralloc_free(mem_ctx);
   return 0;
}

/*
 * What argument buffers a module declares, and the two numbers an application allocates
 * its buffer from (air_metal.h). Beside rwa_module_textures because it is the same kind
 * of call and has the same reason to exist: a driver answering
 * -[MTLFunction newArgumentEncoderWithBufferIndex:] holds a function's bitcode and
 * nothing else -- no pipeline, and no descriptor the layout could be read off.
 *
 * `encoded_length` is 8 bytes per member the set DECLARES, used by this shader or not,
 * and that is Apple's number rather than a convenience of ours: `air.arg_type_size` on
 * the set's own node says 8 x members in every one of Resident Evil 4's 7 857 sets, and
 * `air.arg_type_align_size` says 8 in all of them (docs/air.md 8.2.1). The frontend
 * keeps every member in the resource list for exactly this reason -- a numbering that
 * skipped the unread ones would differ between two shaders sharing one set, and an
 * application that allocated from the shorter number would overlap its own encodings.
 *
 * The density of the ids is CHECKED and not assumed, because it is what makes 8 x members
 * the same number as Apple's: Metal's encodedLength is over the highest `[[id(n)]]`, so a
 * set whose ids have a gap in them (an MSL struct with `[[id(0)]]` and `[[id(5)]]`) is
 * longer than its member count says. SPIRV-Cross numbers densely and every set in reach
 * is dense, so this refuses rather than guesses which of the two readings was meant.
 */
int
rwa_module_argbufs(struct rwa_device *device, const void *bitcode, size_t size,
                   const char *entry, struct rwa_argbuf_layout *out, unsigned max,
                   unsigned *count, char *why, size_t why_size)
{
   if (count)
      *count = 0;
   if (!device) {
      say(why, why_size, "no device to read a module with");
      return -1;
   }
   if (!bitcode || size < 4) {
      say(why, why_size, "%zu bytes is not a shader module", size);
      return -1;
   }
   if (!out && max) {
      say(why, why_size, "nowhere to put %u argument buffer layouts", max);
      return -1;
   }

   void *mem_ctx = ralloc_context(NULL);
   if (!mem_ctx) {
      say(why, why_size, "out of memory reading a module");
      return -1;
   }

   /* Function constants taken as zero, which is rwa_module_textures' reason verbatim:
    * there is nothing to take them from, and the translator refuses what it cannot fold.
    */
   struct air_constant_value *constants;
   unsigned num_constants =
      air_module_function_constants(mem_ctx, bitcode, size, &constants);
   const struct air_to_nir_options options = {
      .constants = constants,
      .num_constants = num_constants,
   };
   struct air_shader_info info = { 0 };
   const char *error;
   nir_shader *nir = air_to_nir(mem_ctx, bitcode, size, entry, &options,
                                device->nir_options, &info, &error);
   if (!nir) {
      say(why, why_size, "%s", error ? error : "the module could not be translated");
      ralloc_free(mem_ctx);
      return -1;
   }
   const char *name = info.entry_point ? info.entry_point : "the function";

   /* The sets, in the order the module declares them, which is the order the frontend
    * lists their members in: one pass, appending a layout the first time a set's Metal
    * index is seen.
    */
   unsigned n = 0;
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];

      if (r->argbuf_set == ~0u)
         continue;

      struct rwa_argbuf_layout *l = NULL;
      for (unsigned k = 0; k < n; k++) {
         if (out[k].metal_index == r->argbuf_set) {
            l = &out[k];
            break;
         }
      }
      if (!l) {
         if (n >= max) {
            say(why, why_size, "%s declares an argument buffer at Metal buffer index %u "
                               "and there is room for %u layout%s", name, r->argbuf_set,
                max, max == 1 ? "" : "s");
            ralloc_free(mem_ctx);
            return -1;
         }
         l = &out[n++];
         memset(l, 0, sizeof(*l));
         l->metal_index = r->argbuf_set;
         l->alignment = 8;
      }
      if (r->argbuf_id == ~0u) {
         l->encoded_length = MAX2(l->encoded_length, r->size);
         continue;
      }

      if (l->member_count >= RWA_MAX_ARGBUF_MEMBERS) {
         say(why, why_size, "the argument buffer %s declares at Metal buffer index %u has "
                            "more than the %u members this library records", name,
             r->argbuf_set, RWA_MAX_ARGBUF_MEMBERS);
         ralloc_free(mem_ctx);
         return -1;
      }

      struct rwa_argbuf_member *m = &l->members[l->member_count++];
      m->id = r->argbuf_id;
      m->index = r->index;
      m->write = r->write ? 1 : 0;
      m->kind = r->kind == AIR_RESOURCE_SAMPLER ? RWA_ARGBUF_SAMPLER
              : r->kind == AIR_RESOURCE_TEXTURE ? RWA_ARGBUF_TEXTURE
              : r->kind == AIR_RESOURCE_IMAGE   ? RWA_ARGBUF_TEXTURE
                                                : RWA_ARGBUF_BUFFER;
      /* Copied and not pointed at: the strings live under mem_ctx, which this call
       * frees before it returns. */
      snprintf(m->name, sizeof(m->name), "%s", r->name ? r->name : "");
   }

   for (unsigned i = 0; i < info.num_argbuf_constants; i++) {
      const struct air_argbuf_const_member *cm = &info.argbuf_constants[i];
      struct rwa_argbuf_layout *l = NULL;
      for (unsigned k = 0; k < n; k++) {
         if (out[k].metal_index == cm->argbuf_set) {
            l = &out[k];
            break;
         }
      }
      if (!l) {
         if (n >= max) {
            say(why, why_size, "%s declares an argument buffer at Metal buffer index %u "
                               "and there is room for %u layout%s", name, cm->argbuf_set,
                max, max == 1 ? "" : "s");
            ralloc_free(mem_ctx);
            return -1;
         }
         l = &out[n++];
         memset(l, 0, sizeof(*l));
         l->metal_index = cm->argbuf_set;
         l->alignment = cm->alignment > 8 ? cm->alignment : 8;
      } else if (cm->alignment > l->alignment) {
         l->alignment = cm->alignment;
      }
      if (l->member_count >= RWA_MAX_ARGBUF_MEMBERS) {
         say(why, why_size, "the argument buffer %s declares at Metal buffer index %u has "
                            "more than the %u members this library records", name,
             cm->argbuf_set, RWA_MAX_ARGBUF_MEMBERS);
         ralloc_free(mem_ctx);
         return -1;
      }
      struct rwa_argbuf_member *m = &l->members[l->member_count++];
      memset(m, 0, sizeof(*m));
      m->id = cm->argbuf_id;
      m->kind = RWA_ARGBUF_CONSTANT;
      m->index = ~0u;
      m->write = 0;
      m->offset = cm->offset;
      m->size = cm->size;
      snprintf(m->name, sizeof(m->name), "%s", cm->name ? cm->name : "");
   }

   for (unsigned k = 0; k < n; k++) {
      struct rwa_argbuf_layout *l = &out[k];
      unsigned max_id = 0;
      for (unsigned i = 0; i < l->member_count; i++) {
         for (unsigned j = 0; j < i; j++) {
            if (l->members[j].id == l->members[i].id) {
               say(why, why_size, "two members of the argument buffer %s declares at Metal "
                                  "buffer index %u are both id %u", name, l->metal_index,
                   l->members[i].id);
               ralloc_free(mem_ctx);
               return -1;
            }
         }
         if (l->members[i].id > max_id)
            max_id = l->members[i].id;
         if (l->members[i].kind == RWA_ARGBUF_CONSTANT) {
            uint64_t end = (uint64_t)l->members[i].offset + l->members[i].size;
            if (end > l->encoded_length)
               l->encoded_length = (unsigned)end;
         }
      }
      if (l->member_count > 0) {
         if (l->encoded_length == 0 && max_id < RWA_MAX_ARGBUF_MEMBERS) {
            l->encoded_length = 8 * (max_id + 1);
         } else {
            unsigned min_len = 8 * l->member_count;
            if (l->encoded_length < min_len)
               l->encoded_length = min_len;
         }
      }
   }

   if (count)
      *count = n;
   ralloc_free(mem_ctx);
   return 0;
}

#include "air_metal_depth_stencil.inc"

/*
 * MTLVertexFormat in Gallium's terms, and what each format DELIVERS to the register.
 *
 * The numbers are Apple's own and were read out of the SDK this driver is built against
 * -- iPhoneOS16.5.sdk's Metal.framework/Headers/MTLVertexDescriptor.h -- rather than
 * remembered, and userland/src/annibalemetal.m carries a _Static_assert per row against
 * those enumerators, so a disagreement between this table and Apple's header is a build
 * failure of the driver and not a wrong fetch on the phone. That SDK's enumeration ends
 * at Half (53): FloatRG11B10 and FloatRGB9E5 are iOS 17's, and nothing on this device
 * can name them.
 *
 * The mapping is one rule, as docs/air.md 3.1 says: Char/Short/Int and their unsigned
 * forms are R8/R16/R32 _SINT/_UINT of the same channel count, `...Normalized` is
 * _SNORM/_UNORM instead, Half is _FLOAT at 16 bits and Float at 32, and
 * UChar4Normalized_BGRA is B8G8R8A8_UNORM -- whose channel order rides in
 * VFD_DECODE_INSTR.swap, which fd6_vertex_swap() reads off the pipe_format itself.
 *
 * `delivers` is what the shader's register holds afterwards, and it is here because that
 * is what has to match the member's declared type: a _UINT format delivers an integer,
 * a _UNORM one delivers a float in [0, 1]. Gallium's _USCALED/_SSCALED -- an integer in
 * memory widened to a float without normalising -- has no Metal counterpart and is
 * deliberately absent; Metal's plain integer formats feed an integer member, which is
 * what this check is for.
 *
 * `bytes` is one element's size in memory, which no pipe_format query gives for a
 * three-component vertex format the way the fetch sees it (util_format_get_blocksize of
 * R32G32B32_FLOAT is 12, which is right, but writing the number down is what lets the
 * stream bounds check below say what it bounded).
 */
struct rwa_vertex_format {
   unsigned mtl;
   const char *name;
   enum pipe_format pipe;
   enum air_attribute_base_type delivers;
   unsigned bytes;
};

static const struct rwa_vertex_format rwa_vertex_formats[] = {
   {  1, "MTLVertexFormatUChar2", PIPE_FORMAT_R8G8_UINT, AIR_ATTRIB_UINT, 2 },
   {  2, "MTLVertexFormatUChar3", PIPE_FORMAT_R8G8B8_UINT, AIR_ATTRIB_UINT, 3 },
   {  3, "MTLVertexFormatUChar4", PIPE_FORMAT_R8G8B8A8_UINT, AIR_ATTRIB_UINT, 4 },
   {  4, "MTLVertexFormatChar2", PIPE_FORMAT_R8G8_SINT, AIR_ATTRIB_INT, 2 },
   {  5, "MTLVertexFormatChar3", PIPE_FORMAT_R8G8B8_SINT, AIR_ATTRIB_INT, 3 },
   {  6, "MTLVertexFormatChar4", PIPE_FORMAT_R8G8B8A8_SINT, AIR_ATTRIB_INT, 4 },
   {  7, "MTLVertexFormatUChar2Normalized", PIPE_FORMAT_R8G8_UNORM, AIR_ATTRIB_FLOAT, 2 },
   {  8, "MTLVertexFormatUChar3Normalized", PIPE_FORMAT_R8G8B8_UNORM, AIR_ATTRIB_FLOAT, 3 },
   {  9, "MTLVertexFormatUChar4Normalized", PIPE_FORMAT_R8G8B8A8_UNORM, AIR_ATTRIB_FLOAT, 4 },
   { 10, "MTLVertexFormatChar2Normalized", PIPE_FORMAT_R8G8_SNORM, AIR_ATTRIB_FLOAT, 2 },
   { 11, "MTLVertexFormatChar3Normalized", PIPE_FORMAT_R8G8B8_SNORM, AIR_ATTRIB_FLOAT, 3 },
   { 12, "MTLVertexFormatChar4Normalized", PIPE_FORMAT_R8G8B8A8_SNORM, AIR_ATTRIB_FLOAT, 4 },
   { 13, "MTLVertexFormatUShort2", PIPE_FORMAT_R16G16_UINT, AIR_ATTRIB_UINT, 4 },
   { 14, "MTLVertexFormatUShort3", PIPE_FORMAT_R16G16B16_UINT, AIR_ATTRIB_UINT, 6 },
   { 15, "MTLVertexFormatUShort4", PIPE_FORMAT_R16G16B16A16_UINT, AIR_ATTRIB_UINT, 8 },
   { 16, "MTLVertexFormatShort2", PIPE_FORMAT_R16G16_SINT, AIR_ATTRIB_INT, 4 },
   { 17, "MTLVertexFormatShort3", PIPE_FORMAT_R16G16B16_SINT, AIR_ATTRIB_INT, 6 },
   { 18, "MTLVertexFormatShort4", PIPE_FORMAT_R16G16B16A16_SINT, AIR_ATTRIB_INT, 8 },
   { 19, "MTLVertexFormatUShort2Normalized", PIPE_FORMAT_R16G16_UNORM, AIR_ATTRIB_FLOAT, 4 },
   { 20, "MTLVertexFormatUShort3Normalized", PIPE_FORMAT_R16G16B16_UNORM, AIR_ATTRIB_FLOAT, 6 },
   { 21, "MTLVertexFormatUShort4Normalized", PIPE_FORMAT_R16G16B16A16_UNORM, AIR_ATTRIB_FLOAT, 8 },
   { 22, "MTLVertexFormatShort2Normalized", PIPE_FORMAT_R16G16_SNORM, AIR_ATTRIB_FLOAT, 4 },
   { 23, "MTLVertexFormatShort3Normalized", PIPE_FORMAT_R16G16B16_SNORM, AIR_ATTRIB_FLOAT, 6 },
   { 24, "MTLVertexFormatShort4Normalized", PIPE_FORMAT_R16G16B16A16_SNORM, AIR_ATTRIB_FLOAT, 8 },
   { 25, "MTLVertexFormatHalf2", PIPE_FORMAT_R16G16_FLOAT, AIR_ATTRIB_FLOAT, 4 },
   { 26, "MTLVertexFormatHalf3", PIPE_FORMAT_R16G16B16_FLOAT, AIR_ATTRIB_FLOAT, 6 },
   { 27, "MTLVertexFormatHalf4", PIPE_FORMAT_R16G16B16A16_FLOAT, AIR_ATTRIB_FLOAT, 8 },
   { 28, "MTLVertexFormatFloat", PIPE_FORMAT_R32_FLOAT, AIR_ATTRIB_FLOAT, 4 },
   { 29, "MTLVertexFormatFloat2", PIPE_FORMAT_R32G32_FLOAT, AIR_ATTRIB_FLOAT, 8 },
   { 30, "MTLVertexFormatFloat3", PIPE_FORMAT_R32G32B32_FLOAT, AIR_ATTRIB_FLOAT, 12 },
   { 31, "MTLVertexFormatFloat4", PIPE_FORMAT_R32G32B32A32_FLOAT, AIR_ATTRIB_FLOAT, 16 },
   { 32, "MTLVertexFormatInt", PIPE_FORMAT_R32_SINT, AIR_ATTRIB_INT, 4 },
   { 33, "MTLVertexFormatInt2", PIPE_FORMAT_R32G32_SINT, AIR_ATTRIB_INT, 8 },
   { 34, "MTLVertexFormatInt3", PIPE_FORMAT_R32G32B32_SINT, AIR_ATTRIB_INT, 12 },
   { 35, "MTLVertexFormatInt4", PIPE_FORMAT_R32G32B32A32_SINT, AIR_ATTRIB_INT, 16 },
   { 36, "MTLVertexFormatUInt", PIPE_FORMAT_R32_UINT, AIR_ATTRIB_UINT, 4 },
   { 37, "MTLVertexFormatUInt2", PIPE_FORMAT_R32G32_UINT, AIR_ATTRIB_UINT, 8 },
   { 38, "MTLVertexFormatUInt3", PIPE_FORMAT_R32G32B32_UINT, AIR_ATTRIB_UINT, 12 },
   { 39, "MTLVertexFormatUInt4", PIPE_FORMAT_R32G32B32A32_UINT, AIR_ATTRIB_UINT, 16 },
   { 42, "MTLVertexFormatUChar4Normalized_BGRA", PIPE_FORMAT_B8G8R8A8_UNORM,
         AIR_ATTRIB_FLOAT, 4 },
   { 45, "MTLVertexFormatUChar", PIPE_FORMAT_R8_UINT, AIR_ATTRIB_UINT, 1 },
   { 46, "MTLVertexFormatChar", PIPE_FORMAT_R8_SINT, AIR_ATTRIB_INT, 1 },
   { 47, "MTLVertexFormatUCharNormalized", PIPE_FORMAT_R8_UNORM, AIR_ATTRIB_FLOAT, 1 },
   { 48, "MTLVertexFormatCharNormalized", PIPE_FORMAT_R8_SNORM, AIR_ATTRIB_FLOAT, 1 },
   { 49, "MTLVertexFormatUShort", PIPE_FORMAT_R16_UINT, AIR_ATTRIB_UINT, 2 },
   { 50, "MTLVertexFormatShort", PIPE_FORMAT_R16_SINT, AIR_ATTRIB_INT, 2 },
   { 51, "MTLVertexFormatUShortNormalized", PIPE_FORMAT_R16_UNORM, AIR_ATTRIB_FLOAT, 2 },
   { 52, "MTLVertexFormatShortNormalized", PIPE_FORMAT_R16_SNORM, AIR_ATTRIB_FLOAT, 2 },
   { 53, "MTLVertexFormatHalf", PIPE_FORMAT_R16_FLOAT, AIR_ATTRIB_FLOAT, 2 },
};

static const struct rwa_vertex_format *
vertex_format(unsigned mtl)
{
   for (unsigned i = 0; i < ARRAY_SIZE(rwa_vertex_formats); i++)
      if (rwa_vertex_formats[i].mtl == mtl)
         return &rwa_vertex_formats[i];
   return NULL;
}

/* What a vertex format this library declines IS, so the refusal names it rather than
 * printing a number. The four it names are the packed ones -- see the table above and
 * the pixel format table's own paragraph about packed channel orders -- and the rest is
 * a number Apple's enumeration does not carry at all, which on this device means a
 * caller's struct is a different shape from this one.
 */
static const char *
vertex_format_refusal(unsigned f)
{
   switch (f) {
   case 0:
      return "MTLVertexFormatInvalid, which is what an unconfigured attribute holds";
   case 40:
      return "MTLVertexFormatInt1010102Normalized: four channels packed in one word, and "
             "this library declines a packed format because Metal's channel order and "
             "Gallium's are two conventions -- getting it wrong shifts a channel by two "
             "bits in a picture that still looks like a picture. R10G10B10A2_SNORM does "
             "have an a6xx vertex entry, so what this needs is a measurement and not a "
             "format";
   case 41:
      return "MTLVertexFormatUInt1010102Normalized: the same packed word as 40, declined "
             "for the same unmeasured channel order";
   case 54:
      return "MTLVertexFormatFloatRG11B10, which the iOS 16.5 SDK's MTLVertexFormat has "
             "not got (it ends at Half, 53): three channels packed in one word, and "
             "declined for the channel order the same way";
   case 55:
      return "MTLVertexFormatFloatRGB9E5, which the iOS 16.5 SDK's MTLVertexFormat has "
             "not got: PIPE_FORMAT_R9G9B9E5_FLOAT is texture-only in a6xx's format "
             "table, so fd6_vertex_format() answers FMT6_NONE and the hardware cannot "
             "fetch it at all";
   case 43: case 44:
      return "a gap in Apple's own MTLVertexFormat enumeration";
   default:
      return "not a value of MTLVertexFormat";
   }
}

/* MTLVertexStepFunction's five, named for the refusals. */
static const char *
step_function_name(unsigned f)
{
   static const char *const names[] = {
      "MTLVertexStepFunctionConstant", "MTLVertexStepFunctionPerVertex",
      "MTLVertexStepFunctionPerInstance", "MTLVertexStepFunctionPerPatch",
      "MTLVertexStepFunctionPerPatchControlPoint",
   };
   return f < ARRAY_SIZE(names) ? names[f] : NULL;
}

static const char *
attribute_type_name(enum air_attribute_base_type t)
{
   switch (t) {
   case AIR_ATTRIB_FLOAT: return "floating-point";
   case AIR_ATTRIB_INT:   return "signed integer";
   case AIR_ATTRIB_UINT:  return "unsigned integer";
   }
   return "?";
}

/*
 * The vertex elements of one pipeline, built from the module's own interface and the
 * descriptor's layout, and the whole of why this is not a loop over the descriptor:
 *
 *   - GALLIUM MATCHES ELEMENTS TO INPUTS BY POSITION. ir3's setup_input indexes
 *     so->inputs[] by nir_intrinsic_base -- the driver_location nir_lower_io wrote --
 *     and fd6_vertex_state_create emits VFD_DECODE[i] from elements[i] while
 *     emit_vfd_dest emits VFD_DEST_CNTL_INSTR[i] from vs->inputs[i]. So
 *     elements[i] feeds the input whose driver_location is i, and those are numbered in
 *     the order AIR lists the attributes WITH THE UNUSED ONES SKIPPED. For
 *     std_vert0_lpf -- attributes 0, 1 (air.arg_unused) and 3 -- element 0 is attribute
 *     0 and element 1 is attribute 3. An array built by attribute index would put
 *     attribute 1's format, offset and buffer under attribute 3's fetch, and the
 *     geometry would be right while the colour came out of the wrong stream.
 *   - the table comes from air_shader_info AND NOT FROM ir3's own input table, because
 *     ir3 may delete the read: Resident Evil 4's PhotonSplattingVS loads
 *     in_var_POSITION and then writes a constant position, so after optimisation its
 *     input entry is zero-filled -- slot 0, cm 0, regid 63 -- while emit_vfd_dest still
 *     counts it. Reading the interface off that entry would describe an attribute the
 *     shader never declared.
 *
 * An attribute gated by a function-constant predicate that the descriptor leaves
 * unconfigured gets a PLACEHOLDER element rather than being skipped, because skipping it
 * would shift every later driver_location by one. The placeholder fetches from the
 * library's own quad buffer with a stride of 0, so it reads sixteen bytes this library
 * owns at every vertex instead of whatever address an empty VFD slot points at -- which
 * on this hardware is 0 (docs/metal.md 10.18 found what a NULL vertex state does).
 */
struct rwa_vertex_state {
   struct pipe_vertex_element element[16];   /* NIR's sixteen generic slots */
   unsigned count;
   unsigned attribute[16];                   /* Metal's [[attribute(n)]] per element */
   unsigned stream_mask;                     /* buffer indices fetched from */
   unsigned stream_count;                    /* the highest of them plus one */
   unsigned stride[RWA_MAX_VERTEX_BUFFERS];  /* per buffer index, for the draw's bounds */
   unsigned per_vertex;                      /* which of them step per vertex */
   unsigned bytes[RWA_MAX_VERTEX_BUFFERS];   /* the largest element end in each */
};

static bool
build_vertex_state(const struct air_shader_info *vinfo,
                   const struct rwa_render_shape *shape, const char *vname,
                   struct pipe_screen *screen, struct rwa_vertex_state *out,
                   char *why, size_t why_size)
{
   memset(out, 0, sizeof(*out));

   for (unsigned i = 0; i < vinfo->num_vertex_attributes; i++) {
      const struct air_vertex_attribute *a = &vinfo->vertex_attributes[i];

      /* An attribute nothing reads needs no fetch and no binding, and Metal does not
       * require the application's descriptor to configure one: eight of QuartzCore's 46
       * are air.arg_unused.
       */
      if (!a->used)
         continue;
      if (a->index >= RWA_MAX_VERTEX_ATTRIBUTES) {
         say(why, why_size, "%s reads vertex attribute %u and Metal has %u", vname,
             a->index, RWA_MAX_VERTEX_ATTRIBUTES);
         return false;
      }
      if (a->driver_location >= ARRAY_SIZE(out->element)) {
         say(why, why_size, "%s puts vertex attribute %u at driver location %u, past the "
                            "%u NIR has generic slots for", vname, a->index,
             a->driver_location, (unsigned)ARRAY_SIZE(out->element));
         return false;
      }

      const struct rwa_vertex_attribute *d = &shape->vertex_attribute[a->index];
      unsigned loc = a->driver_location;

      out->count = MAX2(out->count, loc + 1);
      out->attribute[loc] = a->index;

      if (d->format == 0) {
         /* Not configured. Legal only for an attribute the module gates with a
          * function-constant predicate, whose value is the pipeline's and not this
          * library's to know (docs/air.md 3.1): the parameter exists either way, so the
          * attribute is in vertex_attributes_read either way, and a pipeline whose
          * constants make the predicate false may configure nothing here.
          */
         if (!a->function_constant) {
            say(why, why_size, "%s reads vertex attribute %u (%s, %s) and the vertex "
                               "descriptor configures no format there, so the fetch "
                               "would read a slot nothing was set up for", vname,
                a->index, a->name ? a->name : "unnamed",
                a->type_name ? a->type_name : "?");
            return false;
         }
         /* The placeholder: this library's own quad buffer, read at stride 0. */
         out->element[loc] = (struct pipe_vertex_element){
            .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
            .src_offset = 0, .src_stride = 0,
            .vertex_buffer_index = RWA_MAX_VERTEX_BUFFERS,
         };
         continue;
      }

      const struct rwa_vertex_format *f = vertex_format(d->format);
      if (!f) {
         say(why, why_size, "vertex attribute %u of %s: format %u is %s", a->index, vname,
             d->format, vertex_format_refusal(d->format));
         return false;
      }
      if (f->delivers != a->base_type) {
         say(why, why_size, "vertex attribute %u of %s is %s, which wants %s components, "
                            "and %s delivers %s ones", a->index, vname,
             a->type_name ? a->type_name : (a->name ? a->name : "unnamed"),
             attribute_type_name(a->base_type), f->name,
             attribute_type_name(f->delivers));
         return false;
      }
      /* The polite question, asked of the screen rather than assumed: fd6_screen.cc
       * answers it from fd6_vertex_format() != FMT6_NONE, which is the same test
       * fd6_vertex_state_create then asserts on. Asking means a format this library
       * maps but that hardware cannot fetch is a sentence here instead of an assertion
       * there.
       */
      if (!screen->is_format_supported(screen, f->pipe, PIPE_BUFFER, 0, 0,
                                       PIPE_BIND_VERTEX_BUFFER)) {
         say(why, why_size, "%s cannot fetch %s (%s) as a vertex attribute",
             screen->get_name(screen), f->name, util_format_short_name(f->pipe));
         return false;
      }
      if (d->buffer_index >= RWA_MAX_VERTEX_BUFFERS) {
         say(why, why_size, "vertex attribute %u of %s is fetched from buffer index %u, "
                            "and Metal has %u", a->index, vname, d->buffer_index,
             RWA_MAX_VERTEX_BUFFERS);
         return false;
      }
      /* Twelve bits at shift 5 in VFD_DECODE_INSTR, set with a masking setter: 4096
       * would fetch offset 0. Metal allows more, so this is ours to refuse.
       */
      if (d->offset > 4095) {
         say(why, why_size, "vertex attribute %u of %s is at offset %u, and a6xx's "
                            "VFD_DECODE_INSTR.offset is twelve bits (4095): the setter "
                            "masks rather than asserts, so this would silently fetch "
                            "offset %u", a->index, vname, d->offset, d->offset & 4095);
         return false;
      }

      const struct rwa_vertex_layout *l = &shape->vertex_layout[d->buffer_index];
      const char *sf = step_function_name(l->step_function);
      unsigned rate = l->step_rate ? l->step_rate : 1;
      unsigned divisor = 0;
      unsigned stride = l->stride;

      if (!sf) {
         say(why, why_size, "buffer index %u of %s's vertex descriptor has step function "
                            "%u, which is not one of MTLVertexStepFunction's five",
             d->buffer_index, vname, l->step_function);
         return false;
      }
      switch (l->step_function) {
      case 0:   /* Constant: every vertex and every instance reads element 0 */
         stride = 0;
         break;
      case 1:   /* PerVertex */
         if (rate != 1) {
            say(why, why_size, "buffer index %u of %s's vertex descriptor steps per "
                               "vertex at a rate of %u: Gallium has a divisor for "
                               "instances and nothing to express a rate over vertices "
                               "with", d->buffer_index, vname, rate);
            return false;
         }
         out->per_vertex |= 1u << d->buffer_index;
         break;
      case 2:   /* PerInstance: Gallium's instance_divisor exactly */
         divisor = rate;
         break;
      default:
         say(why, why_size, "buffer index %u of %s's vertex descriptor steps with %s, "
                            "which is tessellation: there is no pipe_vertex_element "
                            "field for it and no tessellation stage in this port",
             d->buffer_index, vname, sf);
         return false;
      }
      if (stride > 4095) {
         /* The offset's twelve bits are in this tree's own register header; the stride's
          * width is not -- fd6_context.cc writes VFD_FETCH_STRIDE as a whole word with
          * no mask and no assert in Mesa 25.0.7, and Gallium's own src_stride is
          * "technically only uint16_t". So 4095 here is the hardware field docs/air.md
          * 3.1 read, taken conservatively: a refusal can be lifted by a measurement on
          * the phone, and a stride truncated in the ringbuffer cannot be seen at all.
          */
         say(why, why_size, "buffer index %u of %s's vertex descriptor has a stride of "
                            "%u bytes, and a6xx's vertex fetch stride is twelve bits "
                            "(4095)", d->buffer_index, vname, stride);
         return false;
      }
      if (l->step_function != 0 && stride == 0) {
         say(why, why_size, "buffer index %u of %s's vertex descriptor steps with %s and "
                            "has a stride of 0, so every vertex would read the same "
                            "element: that is what %s is for", d->buffer_index, vname,
             sf, step_function_name(0));
         return false;
      }
      if (stride && (size_t)d->offset + f->bytes > stride) {
         say(why, why_size, "vertex attribute %u of %s is %s at offset %u of a %u-byte "
                            "element, so it reads %u bytes of the next vertex", a->index,
             vname, f->name, d->offset, stride,
             (unsigned)((size_t)d->offset + f->bytes - stride));
         return false;
      }

      out->element[loc] = (struct pipe_vertex_element){
         .src_format = f->pipe,
         .src_offset = (uint16_t)d->offset,
         .src_stride = stride,
         .vertex_buffer_index = d->buffer_index,
         .instance_divisor = divisor,
      };
      out->stream_mask |= 1u << d->buffer_index;
      out->stream_count = MAX2(out->stream_count, d->buffer_index + 1);
      out->stride[d->buffer_index] = stride;
      out->bytes[d->buffer_index] = MAX2(out->bytes[d->buffer_index],
                                         d->offset + f->bytes);
   }

   /* Metal's one buffer table, which both -setVertexBuffer: and a [[buffer(n)]] argument
    * read out of, so an index is a stream or an argument and never both. The frontend
    * knows which indices the module declares; this is where the two sets meet.
    */
   for (unsigned i = 0; i < vinfo->num_resources; i++) {
      const struct air_resource *r = &vinfo->resources[i];
      if (r->unused || r->argbuf_set != ~0u ||
          (r->kind != AIR_RESOURCE_CONSTANT_BUFFER && r->kind != AIR_RESOURCE_DEVICE_BUFFER))
         continue;
      if (r->index < RWA_MAX_VERTEX_BUFFERS && (out->stream_mask & (1u << r->index))) {
         say(why, why_size, "buffer index %u is a vertex stream in this descriptor and "
                            "%s reads it as [[buffer(%u)]] (%s): Metal has one table "
                            "behind -setVertexBuffer:offset:atIndex: and an index can be "
                            "a stream or an argument, not both", r->index, vname,
             r->index, r->name ? r->name : "unnamed");
         return false;
      }
   }

   /* The placeholder's buffer index, chosen after the real ones are known: one past
    * Metal's highest, so it can never collide with a stream or an argument, and the draw
    * binds this library's quad there.
    */
   if (out->count) {
      bool placeholder = false;
      for (unsigned i = 0; i < out->count; i++)
         placeholder = placeholder ||
                       out->element[i].vertex_buffer_index == RWA_MAX_VERTEX_BUFFERS;
      if (placeholder)
         out->stream_count = MAX2(out->stream_count, RWA_MAX_VERTEX_BUFFERS + 1);
   }
   return true;
}

/* A compiled pipeline for drawing. The shaders are kept pristine under mem_ctx and a
 * clone is handed to the driver, for the reason compute_state_for gives: a Gallium
 * driver steals the NIR it is given, so what this library frees at destroy must be its
 * own copy and not the driver's.
 */
struct rwa_render {
   struct rwa_device *device;
   void *mem_ctx;

   struct air_shader_info finfo;
   struct air_shader_info vinfo;      /* valid only when a vertex module was supplied */
   bool generated_vertex;

   struct rwa_render_shape shape;

   /* the fragment and (real) vertex texture slots and the samplers each stage declares
    * for itself, kept with the pipeline like rwa_compute's
    */
   struct rwa_slot frag_slots[RWA_MAX_TEXTURES];
   struct rwa_slot vert_slots[RWA_MAX_TEXTURES];
   void *frag_own_sampler[RWA_MAX_TEXTURES];
   void *vert_own_sampler[RWA_MAX_TEXTURES];
   bool frag_compare_sampler[RWA_MAX_TEXTURES];
   bool vert_compare_sampler[RWA_MAX_TEXTURES];

   /* the compiled state, made once: a render pipeline has a fixed shape, so unlike a
    * compute state there is no threadgroup size to recompile for
    */
   void *vs_cso, *fs_cso;
   nir_shader *fragment_nir;
   void *yuv_fs_cso;
   struct rwa_yuv_key yuv;
   struct rwa_vertex_cso_entry *cached_vcso_entry;
   void *blend, *dsa, *rast, *velem;
   void *rast_scissor;                /* the same state with scissor enabled */
   struct pipe_resource *vbuf;        /* the full-screen quad, for the generated stage */

   /* What the vertex descriptor was turned into, empty for a stage that fetches no
    * attributes -- which is the generated quad and every vertex module this library
    * drew before the layout existed. `velem` is built from it when it is not empty, and
    * the draw binds the streams it names instead of the quad.
    */
   struct rwa_vertex_state vstate;
};

