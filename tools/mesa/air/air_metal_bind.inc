/* Flush, texture validation, and stage resource binding, included into air_metal.c. */

static int
finish_or_say_why(struct rwa_device *device, bool required, const char *what,
                  char *why, size_t why_size)
{
   struct pipe_fence_handle *fence = NULL;

   device->pipe->flush(device->pipe, &fence, 0);
   device->submit_seq++;
   if (fence) {
      device->screen->fence_reference(device->screen, &device->pending, NULL);
      device->pending = fence;
   }
   if (!device->pending) {
      if (!required)
         return 0;
      say(why, why_size, "the GPU driver gave no fence for the %s, so nothing can tell "
                         "whether it finished; a dispatch nobody waited for is not a "
                         "result", what);
      return -1;
   }

   bool done = device->screen->fence_finish(device->screen, NULL, device->pending,
                                            RWA_FENCE_TIMEOUT_NS);
   device->screen->fence_reference(device->screen, &device->pending, NULL);
   if (!done) {
      say(why, why_size, "the GPU did not finish the %s in time. Anything in "
                         "the buffers is what was put there, not what the shader "
                         "computed. The likeliest reason is the shader's input: a "
                         "kernel whose buffers carry counts, indices or sentinels, "
                         "given values it was never written for, loops on a condition "
                         "that never comes -- both kernels measured this way turned out "
                         "to be exactly that (docs/air.md section 5). And one timeout "
                         "wedges this GPU for the rest of the boot, so every later "
                         "dispatch in it is vacuous too", what);
      return -1;
   }
   return 0;
}

int
rwa_flush(struct rwa_device *device, char *why, size_t why_size)
{
   RWA_LOCKED;
   rwa_deferred_drain_locked(device);
   struct pipe_fence_handle *fence = NULL;

   (void)why, (void)why_size;
   device->pipe->flush(device->pipe, &fence, 0);
   device->submit_seq++;
   if (fence) {
      device->screen->fence_finish(device->screen, NULL, fence, 0);
      device->screen->fence_reference(device->screen, &device->pending, NULL);
      device->pending = fence;
   }
   return 0;
}

/* What the shader calls a texture slot, in the words a Metal programmer wrote it in,
 * so that a refusal about a mismatch names both sides in one vocabulary.
 */
static const char *
shader_texture_name(enum glsl_sampler_dim dim, bool is_array)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:   return is_array ? "texture1d_array" : "texture1d";
   case GLSL_SAMPLER_DIM_2D:   return is_array ? "texture2d_array" : "texture2d";
   case GLSL_SAMPLER_DIM_3D:   return "texture3d";
   case GLSL_SAMPLER_DIM_CUBE: return is_array ? "texturecube_array" : "texturecube";
   case GLSL_SAMPLER_DIM_MS:   return is_array ? "texture2d_ms_array" : "texture2d_ms";
   case GLSL_SAMPLER_DIM_BUF:  return "texture_buffer";
   default:                    return "a texture of a kind with no Metal name";
   }
}

static const char *
base_type_name(nir_alu_type type)
{
   switch (type) {
   case nir_type_int:   return "int";
   case nir_type_uint:  return "uint";
   case nir_type_float: return "float";
   default:             return "a type with no Metal name";
   }
}

/* The slot's kind as MTLTextureType, which is the number rwa_texture_shape.type takes.
 *
 * One table, read by the two callers that must agree: texture_fits, which refuses a
 * bound texture whose type is not this number, and rwa_module_textures, which reports
 * this number so a caller can ask for a texture that fits. The mismatch the sweep in
 * userland/src/metal_drv_test.m measured came from a caller having no way to ask; two
 * tables pointing opposite ways would have been the next way to get it wrong.
 *
 * False when the dimension has no Metal spelling at all -- a kind no AIR module can
 * declare, which is refused by name rather than compared against.
 */
static bool
metal_texture_type(enum glsl_sampler_dim dim, bool is_array, unsigned *out)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:
      *out = is_array ? RWA_TYPE_1D_ARRAY : RWA_TYPE_1D;
      return true;
   case GLSL_SAMPLER_DIM_2D:
      *out = is_array ? RWA_TYPE_2D_ARRAY : RWA_TYPE_2D;
      return true;
   case GLSL_SAMPLER_DIM_3D:
      *out = RWA_TYPE_3D;
      return true;
   case GLSL_SAMPLER_DIM_CUBE:
      *out = is_array ? RWA_TYPE_CUBE_ARRAY : RWA_TYPE_CUBE;
      return true;
   case GLSL_SAMPLER_DIM_MS:
      *out = is_array ? RWA_TYPE_2D_MULTISAMPLE_ARRAY : RWA_TYPE_2D_MULTISAMPLE;
      return true;
   case GLSL_SAMPLER_DIM_BUF:
      *out = RWA_TYPE_TEXTURE_BUFFER;
      return true;
   default:
      return false;
   }
}

/*
 * What element type the shader asks of a slot, and in whose words -- Apple's declaration
 * first, the translated instructions only where there is none.
 *
 * That order is the whole of the element check, and it is here rather than inline in it
 * because rwa_module_textures has to report the same answer the check will judge by: a
 * query that preferred the NIR would tell a caller to bind a format the check then
 * refuses. An image_store's src_type is a transport type -- "raw bits, the bound format
 * decides" -- so judging by it refuses every float or normalised image the corpus writes
 * (slot_shapes says what that cost); a txf's dest_type is a real statement and is the
 * fallback for a slot whose metadata carries no type name.
 *
 * False when neither exists, and that is the right way round: this exists to catch a
 * caller binding an integer texture where the shader reads floats, and a slot nothing
 * describes is a slot there are no grounds to refuse. `bits` may be NULL, and is 0 for
 * the NIR fallback, which carries the class and not the width.
 */
static bool
slot_element(const struct rwa_slot *s, nir_alu_type *type, const char **word,
             unsigned *bits)
{
   if (s->declared_element) {
      *type = s->declared_type;
      *word = s->declared_element;
      if (bits)
         *bits = s->declared_bits;
      return true;
   }
   if (s->type_known) {
      *type = s->type;
      *word = base_type_name(s->type);
      if (bits)
         *bits = 0;
      return true;
   }
   return false;
}

/*
 * Whether the texture the caller bound is the texture the shader declared.
 *
 * Metal checks this itself: a texture2d<float> parameter takes neither a 3D texture nor
 * an integer format, and a driver that let either through would be the lenient one. The
 * reason to check rather than trust is in docs/air.md section 5 -- with a 2D texture
 * bound where three shaders sample a 3D one, all three "passed", and only a real 4x4x4
 * texture turned their answers into evidence. A shape that is almost right is the
 * failure that reads like a result.
 */
static bool
texture_fits(const struct rwa_texture *t, const struct rwa_slot *s, const char *name,
             unsigned index, char *why, size_t why_size)
{
   if (!s->shape_known)
      return true;   /* a computed binding: nothing was declared to compare against */

   /*
    * A COMPARISON NEEDS A DEPTH TEXTURE, and this is Metal's own rule rather than a
    * caution: sample_compare is declared on depth2d and depth2d_array and on nothing
    * else, so a colour texture bound under one is a caller's mistake and Metal itself
    * catches it.
    *
    * It is checked here because the Adreno does not. The first device run of
    * tools/air/sampcheck.py's comparison cases put a shadow sample over an
    * r32g32b32a32_float texture, which is what the harness bound, and every case came
    * back with THE TEXEL -- 101 where a comparison must answer 0 or 1, and for
    * compare_func::always and compare_func::never too, which no compare function and no
    * clamped reference can explain. The chain above it was all correct and the device
    * log says so: ir3 emitted the shadow form (`samgq.s ... s#0, t#0`), air_run reported
    * `APPLIED (a shadow sample reads this slot)` for all 32, and fd6_sampler_state_create
    * writes A6XX_TEX_SAMP_1_COMPARE_FUNC whenever compare_mode is set. The texture unit
    * does the comparison in its depth path and a colour format never enters it.
    *
    * A shadow lookup that answers the texel instead of 0 or 1 is the exact shape of
    * wrongness this port refuses on principle, and the hardware will not say no, so the
    * library does.
    */
   if (s->shadow && !format_is_depth_stencil(t->res->format)) {
      say(why, why_size, "%s samples texture %u with a comparison and the texture bound "
                         "there is %s, which is a colour format: Metal declares "
                         "sample_compare on depth2d alone, and this GPU answers a "
                         "comparison of a colour texture with the texel instead of 0 or 1",
          name, index, t->format_name);
      return false;
   }

   /* Compared as MTLTextureType numbers, in the one direction metal_texture_type goes:
    * the bound texture's type is already Metal's own, so mapping the DECLARATION to
    * Metal rather than the binding to Gallium leaves one table instead of two, and that
    * table is the one rwa_module_textures reports from.
    */
   unsigned declared;
   if (!metal_texture_type(s->dim, s->is_array, &declared)) {
      say(why, why_size, "%s declares texture %u as %s, which has no MTLTextureType, so "
                         "there is no texture a caller could bind there", name, index,
          shader_texture_name(s->dim, s->is_array));
      return false;
   }
   if (declared != t->shape.type) {
      say(why, why_size, "%s declares texture %u as a %s and the texture bound there is a "
                         "%s", name, index, shader_texture_name(s->dim, s->is_array),
          texture_type_name(t->shape.type));
      return false;
   }

   /* The element type, which Metal writes into the shader's own type and the API into
    * the pixel format. Integer formats are exactly the ones a shader must read as
    * integers: the bits of a float read as a uint, or a uint fed through a format's
    * conversion, both come back as numbers. Which declaration counts, and why, is
    * slot_element's -- shared with the query so that what a caller is told to bind and
    * what it is refused for cannot be two different things.
    */
   enum pipe_format sampled = texture_format(rgb2p_format(t->shape.pixel_format) && t->chroma
                                               ? t->storage_owner : t);
   nir_alu_type want = util_format_is_pure_uint(sampled) ? nir_type_uint
                     : util_format_is_pure_sint(sampled) ? nir_type_int
                                                                   : nir_type_float;
   nir_alu_type have;
   const char *have_word;
   if (!slot_element(s, &have, &have_word, NULL))
      return true;
   if (want != have) {
      say(why, why_size, "%s reads texture %u as a %s<%s> and %s is %s format", name, index,
          shader_texture_name(s->dim, s->is_array), have_word, t->format_name,
          want == nir_type_float ? "a floating-point or normalised"
                                 : want == nir_type_uint ? "an unsigned integer"
                                                         : "a signed integer");
      return false;
   }
   return true;
}

/* The state for a texture slot no sampler reaches: nearest, clamped, in pixels.
 *
 * It is not a guess about what the shader wanted -- a slot gets this only when the
 * shader declared no sampler for it at all, which means every fetch from it is a Metal
 * read(), a txf that carries its own integer coordinate and ignores the sampler
 * entirely. What needs the state to exist is softpipe: set_shader_sampler() hands the
 * executor samplers[unit] for every unit the shader touches (sp_state_derived.c) and
 * the texture code dereferences it, so a NULL in a used slot is a crash and not a
 * default. These are also the values air_run binds in its compute path, so a number
 * measured here and a number measured there are comparable.
 */
static void *
default_sampler(struct rwa_device *device)
{
   if (!device->default_sampler) {
      device->default_sampler = device->pipe->create_sampler_state(device->pipe,
         &(struct pipe_sampler_state){
            .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
            .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
            .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
            .min_img_filter = PIPE_TEX_FILTER_NEAREST,
            .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
            .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
            .unnormalized_coords = true });
   }
   return device->default_sampler;
}

/*
 * Which Metal buffer indices a module declares ARGUMENT BUFFERS at, one bit each, and a
 * refusal when one is past the 32 a mask holds.
 *
 * A driver needs this for the reason it needs the vertex stream mask: Metal has one
 * buffer table per stage, an index in it is a stream, a buffer argument or a set, and
 * only the shader and the vertex descriptor together say which. Thirty-two is not a
 * narrowing in practice -- every module of both corpora carries
 * `air.max_device_buffers i32 31`, which is Metal's own limit on this device -- but a
 * set past it has to be named rather than dropped out of a mask, because an index
 * missing from this mask is an index a driver would hand over as a plain buffer.
 */
static int
argbuf_mask_of(const struct air_shader_info *info, const char *name, unsigned *mask,
               char *why, size_t why_size)
{
   *mask = 0;
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];

      if (r->argbuf_set == ~0u)
         continue;
      if (r->argbuf_set >= 32) {
         say(why, why_size, "%s declares an argument buffer at Metal buffer index %u, and "
                            "this contract reports the first 32 indices", name,
             r->argbuf_set);
         return -1;
      }
      *mask |= 1u << r->argbuf_set;
   }
   return 0;
}

/*
 * Everything a stage declares has to be bound, of the right kind and big enough -- and
 * the refusals are the same sentences whether the stage is a dispatch's kernel or a
 * draw's fragment shader, so they are written once and pointed at a stage's own
 * resources and texture slots. The counterpart that actually binds is bind_stage below;
 * this one only decides whether it may.
 *
 * A stage dispatched with nothing in a slot it reads does not fail: it reads whatever
 * the slot holds and writes a plausible answer, which is the one outcome worse than an
 * error (docs/air.md section 4). The buffer sizes are checked against air.buffer_size,
 * which Apple's own metadata carries for most arguments -- a shader handed a buffer
 * shorter than it declares writes past the end of it, and on this GPU that is not a
 * fault but another shader's memory.
 */
static int
check_bindings(const struct air_shader_info *info, const struct rwa_slot *slots,
               const struct rwa_bindings *bindings,
               const struct rwa_buffer_bind *table, unsigned table_count,
               const char *name, char *why, size_t why_size)
{
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->unused)
         continue;

      /*
       * A MEMBER OF AN ARGUMENT BUFFER comes from the encoder's record and not from the
       * caller's arrays, and everything below this point is the same for both: the
       * member's own `index` is the Gallium slot either way, and what is checked about a
       * texture or a buffer does not depend on how the application named it.
       *
       * argbuf_member is where this feature's refusals live, the unencoded buffer among
       * them. A member resolved here is a resource of this device that exists.
       */
      struct rwa_buffer *mbuffer = NULL;
      struct rwa_texture *mtexture = NULL;
      size_t moffset = 0;
      bool member = r->argbuf_set != ~0u;

      if (member && argbuf_member(r, table, table_count, name, &mbuffer, &moffset,
                                  &mtexture, NULL, why, why_size) != 0)
         return -1;

      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER:
      case AIR_RESOURCE_DEVICE_BUFFER: {
         struct rwa_buffer *b = mbuffer;

         if (!member) {
            bool unbound = r->index >= bindings->buffer_count || !bindings->buffers[r->index];

            if (unbound && r->kind == AIR_RESOURCE_DEVICE_BUFFER && r->nullable)
               break;
            /* Metal lets a pointer argument go unbound where the shader does not read through
             * it: Tungsten's fragment_main walks `const float4 *shaderP` only for sprites with
             * shader opcodes, and Photos binds nothing there for the grid. A reference argument
             * carries air.buffer_size and must be bound. */
            if (unbound && r->kind == AIR_RESOURCE_CONSTANT_BUFFER &&
                (r->size == 0 || r->nullable))
               break;
            if (unbound) {
               say(why, why_size, "%s reads buffer %u and %u buffer%s bound", name,
                   r->index, bindings->buffer_count,
                   bindings->buffer_count == 1 ? " was" : "s were");
               return -1;
            }
            b = bindings->buffers[r->index];
            /* METAL'S OWN `offset:` on a buffer argument, which this contract used to
             * have nowhere to put: an application that packs several uniform blocks
             * into one allocation and moves the offset per draw is the ordinary case,
             * and the window server of this port does it 55 times a boot.
             */
            char what[96];

            if (direct_buffer_offset(r, bindings, table, table_count, name, &moffset, why,
                                     why_size) != 0)
               return -1;
            snprintf(what, sizeof(what), "buffer %u of %s", r->index, name);
            if (buffer_offset_ok(b->device, r, b, moffset, name, what, why, why_size) != 0)
               return -1;
         }
         if (member && !b)
            break;
         /* `r->size` is air.buffer_size, which a member never carries: the frontend
          * adds a member's resource with a size of zero because the nested descriptor
          * has no such key (air_to_nir.c's allocate_argbuf_indices). Where it IS
          * carried, what the shader can read has to be there AFTER the offset: a
          * buffer bound 1164 bytes in is 1164 bytes shorter for the shader than it is
          * for the application.
          */
         if (r->size && b->size - MIN2(b->size, moffset) < r->size) {
            say(why, why_size, "buffer %u is %zu bytes%s and %s declares it as %u",
                r->index, b->size - MIN2(b->size, moffset),
                moffset ? " after the offset it is bound at" : "", name, r->size);
            return -1;
         }
         break;
      }

      case AIR_RESOURCE_TEXTURE:
      case AIR_RESOURCE_IMAGE: {
         bool stored = r->kind == AIR_RESOURCE_IMAGE;
         if (r->index >= RWA_MAX_TEXTURES) {
            say(why, why_size, "%s binds a texture at index %u, past the %u slots this "
                               "library fills", name, r->index, RWA_MAX_TEXTURES);
            return -1;
         }
         /* a texture the application may leave unbound, as on Metal, gets no view
          * (air_resource.nullable, air_texture_may_be_unbound) */
         if (!member && r->nullable &&
             (r->index >= bindings->texture_count || !bindings->textures[r->index]))
            break;
         if (!member && (r->index >= bindings->texture_count ||
                         !bindings->textures[r->index])) {
            say(why, why_size, "%s %s texture %u and %u texture%s bound", name,
                stored ? "writes" : "reads", r->index, bindings->texture_count,
                bindings->texture_count == 1 ? " was" : "s were");
            return -1;
         }
         if (member && !mtexture)
            break;
         const struct rwa_texture *t = member ? mtexture : bindings->textures[r->index];
         /* MTLTextureUsage, honoured rather than noted: Metal refuses to bind a
          * texture to a shader that writes it unless the application asked for
          * ShaderWrite, and a texture allocated without it may not even be laid out so
          * that a store can reach it.
          */
         if (stored && !t->shader_write) {
            say(why, why_size, "%s writes texture %u and that texture was made without "
                               "MTLTextureUsageShaderWrite", name, r->index);
            return -1;
         }
         if (!stored && !t->shader_read) {
            say(why, why_size, "%s samples texture %u and that texture was made without "
                               "MTLTextureUsageShaderRead", name, r->index);
            return -1;
         }
         if (!texture_fits(t, &slots[r->index], name, r->index, why, why_size))
            return -1;
         break;
      }

      case AIR_RESOURCE_SAMPLER:
         if (r->index >= RWA_MAX_TEXTURES) {
            say(why, why_size, "%s binds a sampler at index %u, past the %u slots this "
                               "library fills", name, r->index, RWA_MAX_TEXTURES);
            return -1;
         }
         /* A `constexpr sampler` the shader declares is nothing for the caller to bind
          * -- rwa_compute_info does not count it -- so this library builds it, below.
          */
         if (r->inline_sampler)
            break;
         /* A member's sampler was resolved above, and there is nothing further to check
          * about a sampler state: it is a created object of this device or the encoding
          * was refused.
          */
         if (member)
            break;
         if (r->nullable && (r->index >= bindings->sampler_count || !bindings->samplers[r->index]))
            break;
         if (r->index >= bindings->sampler_count || !bindings->samplers[r->index]) {
            say(why, why_size, "%s takes sampler %u from the API and %u sampler%s bound",
                name, r->index, bindings->sampler_count,
                bindings->sampler_count == 1 ? " was" : "s were");
            return -1;
         }
         break;

      case AIR_RESOURCE_THREADGROUP_BUFFER:
         break;   /* a dispatch refuses one before it reaches here; no draw stage has it */
      }
   }
   return 0;
}

/* A microsecond clock for the two phase timers below: rwa_render_draw's per-draw
 * breakdown and bind_stage's per-call one. It reads the clock the row ring uses, so a phase
 * timed here sits on the timeline of the `tl:` lines the rest of the boot prints. 0 unless
 * FD_PERF_DRAW=1, as the driver's own stamps (util/perf_draw.h): the reads on every draw and
 * bind were most of a fifth of backboardd's samples in kernel-12377-cmd39-bo15.
 */
static uint64_t
draw_mark(void)
{
   static int timed = -1;

   if (timed < 0) {
      const char *wanted = getenv("FD_PERF_DRAW");

      timed = wanted && wanted[0] == '1';
   }
   return timed ? device_us() : 0;
}

/* What bind_stage bound, so unbind_stage can take it all back once the fence is in. The
 * buffers are not here on purpose: they are left bound and rebound by the next draw or
 * dispatch, because Gallium holds a reference on a bound resource -- it is the sampler
 * states and views, which it does not, that must not outlive the caller's next
 * rwa_sampler_destroy or rwa_texture_destroy.
 */
struct rwa_bound {
   unsigned num_ssbo, num_views, num_images, num_samplers;
   /* Bytes for the folded constant buffers bound at the last UBO slot (air_entry.inc);
    * user_buffer uploads happen at the draw, so it lives until unbind_stage. */
   void *const_win;
};

/*
 * Bind one stage's resources, each as the kind its own instructions ask for, and record
 * what was bound. This is air_run's compute binding with the shader stage lifted out of
 * it, so the dispatch and a draw's fragment stage share it rather than keep two copies
 * that drift: a Metal [[buffer(n)]]/[[texture(n)]]/[[sampler(n)]] index means the same
 * thing in a kernel and in a fragment shader, and only the pipe_shader_type differs.
 *
 * `own_sampler` is the pipeline's cache of the samplers the shader declares for itself,
 * built once and kept; the caller passes the array that belongs to this stage.
 * `compare_sampler` is slot_shapes' answer for the same stage: which sampler slots a
 * shadow texture instruction reads, which is what decides, per slot, which of a
 * sampler's two state objects is bound.
 */
/* Where a bind_stage call's processor time goes. The per-draw line says bindings take
 * 490 of a draw's 630 us on the device; this says which of the four state calls it is,
 * printed every 512 calls and averaged over them. Everything else in the call is the
 * resolution of each declared resource against the encoder's record, which is where a
 * lookup per resource would show up.
 */
enum { BIND_PHASE_TOTAL, BIND_PHASE_RESOLVE, BIND_PHASE_CONSTANT, BIND_PHASE_DEFAULTS,
       BIND_PHASE_SSBO, BIND_PHASE_VIEWS, BIND_PHASE_IMAGES, BIND_PHASE_SAMPLERS,
       BIND_PHASE_COUNT };
static uint64_t bind_phase_us[BIND_PHASE_COUNT];
static unsigned bind_phase_calls;
static unsigned bind_phase_resources;

static void
bind_phase_add(unsigned phase, uint64_t began)
{
   bind_phase_us[phase] += draw_mark() - began;
}

static void
bind_phase_done(uint64_t began)
{
   if (++bind_phase_calls != 512) {
      bind_phase_add(BIND_PHASE_TOTAL, began);
      return;
   }
   bind_phase_add(BIND_PHASE_TOTAL, began);
   uint64_t calls = bind_phase_calls;
   /* The constant buffer's time is inside the resolve, and printed beside it rather than
    * taken out of the sum.
    */
   uint64_t rest = bind_phase_us[BIND_PHASE_TOTAL] - bind_phase_us[BIND_PHASE_RESOLVE] -
                   bind_phase_us[BIND_PHASE_DEFAULTS] - bind_phase_us[BIND_PHASE_SSBO] -
                   bind_phase_us[BIND_PHASE_VIEWS] - bind_phase_us[BIND_PHASE_IMAGES] -
                   bind_phase_us[BIND_PHASE_SAMPLERS];
   printf("rwa bind: %llu calls, %llu us each; resolve %llu (constant buffer %llu), "
          "defaults %llu, ssbo %llu, views %llu, images %llu, samplers %llu, rest %llu; "
          "%.1f resources each\n",
          (unsigned long long)calls,
          (unsigned long long)(bind_phase_us[BIND_PHASE_TOTAL] / calls),
          (unsigned long long)(bind_phase_us[BIND_PHASE_RESOLVE] / calls),
          (unsigned long long)(bind_phase_us[BIND_PHASE_CONSTANT] / calls),
          (unsigned long long)(bind_phase_us[BIND_PHASE_DEFAULTS] / calls),
          (unsigned long long)(bind_phase_us[BIND_PHASE_SSBO] / calls),
          (unsigned long long)(bind_phase_us[BIND_PHASE_VIEWS] / calls),
          (unsigned long long)(bind_phase_us[BIND_PHASE_IMAGES] / calls),
          (unsigned long long)(bind_phase_us[BIND_PHASE_SAMPLERS] / calls),
          (unsigned long long)(rest / calls),
          (double)bind_phase_resources / (double)calls);
   fflush(stdout);
   memset(bind_phase_us, 0, sizeof(bind_phase_us));
   bind_phase_calls = 0;
   bind_phase_resources = 0;
}

static int
bind_stage(struct rwa_device *device, mesa_shader_stage stage,
           const struct air_shader_info *info, const struct rwa_slot *slots, void **own_sampler,
           const bool *compare_sampler, const struct rwa_yuv_key *yuv,
           const struct rwa_bindings *bindings, const struct rwa_buffer_bind *table,
           unsigned table_count, const char *name, struct rwa_bound *bound, char *why,
           size_t why_size)
{
   struct pipe_context *pipe = device->pipe;
   uint64_t entry = draw_mark();

   /* The folded-constants window has to exist before the loop copies each folded
    * buffer's bytes into it; it is bound once after the loop. */
   if (info->const_win_size) {
      bound->const_win = calloc(1, info->const_win_size);
      if (!bound->const_win) {
         say(why, why_size, "%s could not allocate %u bytes of folded constants", name,
             info->const_win_size);
         return -1;
      }
   }

   /* Each buffer bound as whatever the shader declared the index to be. Metal has one
    * [[buffer(n)]] index space and Gallium has two, so a `constant` parameter is a UBO
    * at n and a `device` one is an SSBO at n, and binding a constant buffer as an SSBO
    * would leave the shader reading an unbound slot.
    */
   struct pipe_shader_buffer sb[PIPE_MAX_SHADER_BUFFERS] = { 0 };
   unsigned num_ssbo = 0, writable = 0;

   /* and each texture as the kind of view its own instructions ask for. A read-only
    * texture stays a texture: Metal's read() on one becomes a txf, a texture fetch, so
    * it needs a sampler view even inside a compute shader, while a written one wants
    * an image view (air_to_nir.c, air_run.c's texture_shapes). BlitKernel_Unorm has
    * one of each, and binding only the image left its fetch reading nothing.
    */
   struct pipe_sampler_view *views[RWA_MAX_TEXTURES] = { 0 };
   struct pipe_image_view images[RWA_MAX_IMAGES] = { 0 };
   void *samplers[RWA_MAX_TEXTURES] = { 0 };
   unsigned num_views = 0, num_images = 0, num_samplers = 0;
   unsigned rgb2p_samples = 0, cubic_samplers = 0, unlowered_cubic = 0;

   uint64_t resolve_mark = draw_mark();

   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->unused)
         continue;
      struct rwa_buffer *b = NULL;
      struct rwa_texture *t = NULL;
      struct rwa_sampler *sm = NULL;
      size_t boffset = 0;

      /* A member of an argument buffer comes from the encoder's record; everything
       * below binds it exactly as a direct argument, at the index the frontend gave it.
       * check_bindings has already resolved every member once and refused what could
       * not be -- this cannot fail where that succeeded, and it is asked again rather
       * than carried between the two because a lookup is cheaper than a table that
       * could disagree with itself.
       */
      if (r->argbuf_set != ~0u) {
         if (argbuf_member(r, table, table_count, name, &b, &boffset, &t, &sm, why,
                           why_size) != 0)
            return -1;
      } else {
         switch (r->kind) {
         case AIR_RESOURCE_DEVICE_BUFFER:
         case AIR_RESOURCE_CONSTANT_BUFFER:
            b = r->index < bindings->buffer_count ? bindings->buffers[r->index] : NULL;
            /* and Metal's own offset on it, which check_bindings has already judged */
            if (b && direct_buffer_offset(r, bindings, table, table_count, name, &boffset, why,
                                          why_size) != 0)
               return -1;
            break;
         case AIR_RESOURCE_TEXTURE:
         case AIR_RESOURCE_IMAGE:
            t = r->index < bindings->texture_count ? bindings->textures[r->index] : NULL;
            break;
         case AIR_RESOURCE_SAMPLER:
            if (!r->inline_sampler)
               sm = r->index < bindings->sampler_count ? bindings->samplers[r->index] : NULL;
            break;
         default:
            break;
         }
      }

      switch (r->kind) {
      case AIR_RESOURCE_DEVICE_BUFFER:
         if (r->index >= ARRAY_SIZE(sb)) {
            say(why, why_size, "%s binds a buffer at index %u, past the %u Gallium has "
                               "slots for", name, r->index, (unsigned)ARRAY_SIZE(sb));
            return -1;
         }
         if (!b) {
            if (!device->null_ssbo) {
               device->null_ssbo = pipe_buffer_create(device->screen,
                                                      PIPE_BIND_SHADER_BUFFER,
                                                      PIPE_USAGE_IMMUTABLE, 16);
               if (!device->null_ssbo) {
                  say(why, why_size, "%s could not allocate placeholder SSBO", name);
                  return -1;
               }
            }
            sb[r->index].buffer = device->null_ssbo;
            sb[r->index].buffer_offset = 0;
            sb[r->index].buffer_size = 0;
            num_ssbo = MAX2(num_ssbo, r->index + 1);
            break;
         }
         sb[r->index].buffer = b->res;
         /* Metal's own `offset:`, for a member of a set and for a buffer argument
          * alike, and an offset this GPU cannot fetch from has already been refused by
          * name (buffer_offset_ok: a `device` buffer may be written, so the upload that
          * serves a `constant` one would lose the writes). The SIZE is what is left of
          * the buffer after the offset, so a shader reading past its own block reads
          * past its binding rather than into the next one.
          */
         sb[r->index].buffer_offset = (unsigned)boffset;
         sb[r->index].buffer_size = b->size - MIN2(b->size, boffset);
         num_ssbo = MAX2(num_ssbo, r->index + 1);
         if (r->write)
            writable |= 1u << r->index;
         break;

      case AIR_RESOURCE_CONSTANT_BUFFER: {
         if (r->index >= PIPE_MAX_CONSTANT_BUFFERS) {
            say(why, why_size, "%s binds a constant buffer at index %u, past the %u "
                               "Gallium has slots for", name, r->index,
                (unsigned)PIPE_MAX_CONSTANT_BUFFERS);
            return -1;
         }
         if (r->const_win) {
            /* Folded past the sixteen UBO slots (air_entry.inc): its bytes go into the
             * window bound once below at the last slot; an unbound or unmapped buffer
             * leaves the zeros calloc wrote, which is what it read before. */
            if (b && b->map && bound->const_win)
               memcpy((char *)bound->const_win + r->win_base, (const char *)b->map + boffset,
                      MIN2((size_t)r->size, b->size - MIN2(b->size, boffset)));
            break;
         }
         if (!b) {   /* an unbound pointer argument (check_bindings): a page of zeros */
            /* (once a shader and index: a shader that does read it computes with zeros) */
            static char said[16][96];
            char line[160], key[96];

            snprintf(key, sizeof key, "%s %u", name, r->index);
            for (unsigned i = 0; i < 16; i++) {
               if (!strcmp(said[i], key))
                  break;
               if (!said[i][0]) {
                  snprintf(said[i], sizeof said[i], "%s", key);
                  int n = snprintf(line, sizeof line, "air: %s: constant buffer %u unbound, zeros given\n",
                                   name, r->index);
                  if (n > 0)
                     (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
                  break;
               }
            }
            if (!device->null_ubo) {
               static const uint8_t zeros[4096];

               device->null_ubo = pipe_buffer_create_with_data(pipe, PIPE_BIND_CONSTANT_BUFFER,
                                                               PIPE_USAGE_IMMUTABLE, sizeof(zeros),
                                                               zeros);
               if (!device->null_ubo) {
                  say(why, why_size, "%s could not allocate placeholder constant buffer", name);
                  return -1;
               }
            }
            struct pipe_constant_buffer none = {
               .buffer = device->null_ubo,
               .buffer_size = 4096,
            };

            pipe->set_constant_buffer(pipe, stage, r->slot, &none);
            break;
         }
         size_t left = b->size - MIN2(b->size, boffset);
         /*
          * The buffer and Metal's own offset when 16 divides the offset, and otherwise a
          * copy of the block the shader declares, which freedreno uploads to a 64-byte
          * aligned place. The A830 reads a UBO at a base that is not a multiple of 16 from
          * the wrong place: in rounds render64 to render68, RenderBox draws whose uniforms
          * sat at such offsets drew nothing, and the same pipelines at multiples of 16
          * (736, 1216, 2352, not all multiples of the 64 freedreno advertises) drew.
          *
          * The copy is air.buffer_size bytes and reads the processor's mapping, so a block
          * the GPU wrote earlier in the same submission would be read before that work has
          * run. Copying at every offset 64 does not divide was measured at 221 us a bind
          * when it was the rule here before, which is why the threshold is 16.
          */
         struct pipe_constant_buffer cb = {
            .buffer = b->res,
            .buffer_offset = (unsigned)boffset,
            .buffer_size = (unsigned)left,
         };
         if (boffset % MIN2(device->ubo_offset_align, 16) != 0 && b->map) {
            cb.buffer = NULL;
            cb.buffer_offset = 0;
            cb.user_buffer = (const uint8_t *)b->map + boffset;
            cb.buffer_size = (unsigned)MIN2(left, r->size ? r->size : 65536);
         }
         uint64_t cb_mark = draw_mark();

         pipe->set_constant_buffer(pipe, stage, r->slot, &cb);
         bind_phase_add(BIND_PHASE_CONSTANT, cb_mark);
         break;
      }

      case AIR_RESOURCE_TEXTURE:
         if (!t) {   /* nullable and unbound: check_bindings let it through */
            num_views = MAX2(num_views, r->index + 1);
            break;
         }
         if (t->chroma && rgb2p_format(t->shape.pixel_format) && !rgb2p_full(t->shape.pixel_format))
            rgb2p_samples |= 1u << r->index;
         /* The view is made once and kept with the texture: a device has one context,
          * so a view of this texture is a view for every pipeline that will sample it.
          */
         if (!texture_ensure_sampler_view(device, pipe, t, r->index, why, why_size))
            return -1;
         views[r->index] = t->view;
         num_views = MAX2(num_views, r->index + 1);
         if (t->chroma && !rgb2p_format(t->shape.pixel_format)) {
            unsigned uv = yuv->uv_slot[r->index];
            if (!texture_ensure_sampler_view(device, pipe, t->chroma, uv, why, why_size))
               return -1;
            views[uv] = t->chroma->view;
            num_views = MAX2(num_views, uv + 1);
         }
         break;

      case AIR_RESOURCE_IMAGE:
         if (!t)
            break;
         if (!texture_require_backing(t, "image binding", why, why_size) ||
             buffer_texture_sync_aliases(t, why, why_size) != 0)
            return -1;
         if (r->write) texture_written(t);
         images[r->index] = (struct pipe_image_view){
            .resource = t->res,
            .format = texture_format(t),
            /* Access from shader metadata; write-only on a read-write image reads zeros. */
            .access = (r->read ? PIPE_IMAGE_ACCESS_READ : 0) |
                      (r->write ? PIPE_IMAGE_ACCESS_WRITE : 0),
            .shader_access = (r->read ? PIPE_IMAGE_ACCESS_READ : 0) |
                             (r->write ? PIPE_IMAGE_ACCESS_WRITE : 0),
            .u.tex.level = t->first_level,
            .u.tex.first_layer = t->shape.type == RWA_TYPE_3D ? 0 : t->first_layer,
            .u.tex.last_layer = (t->shape.type == RWA_TYPE_3D
                                    ? u_minify(t->res->depth0, t->first_level)
                                    : (t->first_layer + t->layers)) - 1,
         };
         num_images = MAX2(num_images, r->index + 1);
         /* One view per level for a shader that picks the level itself. Slots past the
          * texture's last level repeat that level: Metal leaves a write there undefined,
          * and an empty slot would be a fault. */
         for (unsigned l = 0; l < r->level_count && r->level_base + l < RWA_MAX_IMAGES; l++) {
            unsigned level = t->first_level + MIN2(l, texture_levels(t) - 1);
            struct pipe_image_view *v = &images[r->level_base + l];

            *v = images[r->index];
            v->u.tex.level = level;
            if (t->shape.type == RWA_TYPE_3D)
               v->u.tex.last_layer = u_minify(t->res->depth0, level) - 1;
            num_images = MAX2(num_images, r->level_base + l + 1);
         }
         break;

      case AIR_RESOURCE_SAMPLER:
         if (r->inline_sampler) {
            if (r->sampler.mag_filter == AIR_SAMPLER_BICUBIC ||
                r->sampler.min_filter == AIR_SAMPLER_BICUBIC) {
               cubic_samplers |= 1u << r->index;
               if (!rgb2p_inline_filter(r)) unlowered_cubic |= 1u << r->index;
            }
            /* The shader's own word, built once and kept with the pipeline. It goes
             * through Metal's vocabulary on the way (sampler_state_from_air), which is
             * what makes this sampler and one a driver creates from
             * MTLSamplerDescriptor the same object -- the argument of docs/air.md 3.3
             * and of air_metal.h.
             */
            bool compares = compare_sampler && compare_sampler[r->index];
            if (!own_sampler[r->index]) {
               struct rwa_sampler_state as_metal;
               struct pipe_sampler_state st;
               if (!sampler_state_from_air(&r->sampler, r->index, &as_metal, why, why_size) ||
                   !sampler_state_from_metal(&as_metal, &st, why, why_size))
                  return -1;
               /* One state object and not two, unlike an API sampler: this word
                * belongs to this shader, so whether a shadow instruction reads it is
                * already known and the other object would never be bound.
                */
               if (compares) {
                  /* `none` is the one value of the word that has no
                   * MTLCompareFunction, and it reached as_metal as Never --
                   * MTLSamplerDescriptor's default, which is right for the plain
                   * sample that is all either can do. A comparison is where they part
                   * company: AGX keeps its NONE and its NEVER apart, and what it does
                   * differently is not readable from here, so a shadow instruction
                   * through such a sampler is refused rather than compared with a
                   * function Apple spells differently (sampler_state_from_air).
                   */
                  if (r->sampler.compare_func == 0) {
                     say(why, why_size, "%s samples sampler %u with a comparison and its "
                                        "own word says compare_func::none, which is not "
                                        "one of MTLCompareFunction's eight: a sampler "
                                        "that was not made to compare", name, r->index);
                     return -1;
                  }
                  st.compare_mode = PIPE_TEX_COMPARE_R_TO_TEXTURE;
               }
               own_sampler[r->index] = pipe->create_sampler_state(pipe, &st);
               if (!own_sampler[r->index]) {
                  say(why, why_size, "%s would not make the sampler %s declares at index %u",
                      rwa_device_name(device), name, r->index);
                  return -1;
               }
            }
            samplers[r->index] = own_sampler[r->index];
         } else {
            if (!sm) {
               samplers[r->index] = default_sampler(device);
               if (!samplers[r->index]) {
                  say(why, why_size, "%s would not make a sampler at all",
                      rwa_device_name(device));
                  return -1;
               }
               num_samplers = MAX2(num_samplers, r->index + 1);
               break;
            }
            if (sm->cubic) {
               cubic_samplers |= 1u << r->index;
               unlowered_cubic |= 1u << r->index;
            }
            /* Metal's sampler, and the instruction's choice of which of its two state
             * objects: struct rwa_sampler.
             */
            samplers[r->index] = (compare_sampler && compare_sampler[r->index])
                                    ? sm->cso_compare : sm->cso;
         }
         num_samplers = MAX2(num_samplers, r->index + 1);
         break;

      case AIR_RESOURCE_THREADGROUP_BUFFER:
         break;
      }
   }

   for (unsigned i = 0; i < RWA_MAX_TEXTURES; i++) {
      if ((rgb2p_samples & (1u << i)) && (slots[i].sampler_mask & cubic_samplers) &&
          (stage == MESA_SHADER_VERTEX || !(yuv->rgb2p & (1u << i)) ||
           (slots[i].sampler_mask & unlowered_cubic))) {
         say(why, why_size, "combined RGB2P texture %u uses an unsupported cubic sampler", i);
         return -1;
      }
   }

   bind_phase_add(BIND_PHASE_RESOLVE, resolve_mark);
   bind_phase_resources += info->num_resources;

   /* A sampler in every slot a sampler view went into, whether a sampler was declared
    * for it or not: see default_sampler() -- softpipe dereferences the state of every
    * unit the shader touches, so a slot left empty is a crash rather than a default.
    */
   uint64_t defaults_mark = draw_mark();

   for (unsigned i = 0; i < num_views; i++) {
      if (samplers[i])
         continue;
      samplers[i] = default_sampler(device);
      if (!samplers[i]) {
         say(why, why_size, "%s would not make a sampler at all", rwa_device_name(device));
         return -1;
      }
      num_samplers = MAX2(num_samplers, i + 1);
   }
   bind_phase_add(BIND_PHASE_DEFAULTS, defaults_mark);

   uint64_t mark = draw_mark();

   if (num_ssbo)
      pipe->set_shader_buffers(pipe, stage, 0, num_ssbo, sb, writable);
   bind_phase_add(BIND_PHASE_SSBO, mark);
   mark = draw_mark();
   if (stage != MESA_SHADER_COMPUTE)
      state_views(device, stage, num_views, views);
   else if (num_views)
      pipe->set_sampler_views(pipe, stage, 0, num_views, 0, views);
   bind_phase_add(BIND_PHASE_VIEWS, mark);
   mark = draw_mark();
   if (num_images)
      pipe->set_shader_images(pipe, stage, 0, num_images, 0, images);
   bind_phase_add(BIND_PHASE_IMAGES, mark);
   mark = draw_mark();
   if (stage != MESA_SHADER_COMPUTE)
      state_samplers(device, stage, num_samplers, samplers);
   else if (num_samplers)
      pipe->bind_sampler_states(pipe, stage, 0, num_samplers, samplers);
   bind_phase_add(BIND_PHASE_SAMPLERS, mark);
   bind_phase_done(entry);

   if (bound->const_win) {
      pipe->set_constant_buffer(pipe, stage, AIR_UBO_SLOTS - 1,
                                &(struct pipe_constant_buffer){
                                   .user_buffer = bound->const_win,
                                   .buffer_size = info->const_win_size });
      static unsigned win_said;
      if (win_said++ < 16) {
         char line[160];
         int n = snprintf(line, sizeof line,
                          "air: %s: %u bytes of folded constants at UBO slot %u\n", name,
                          info->const_win_size, AIR_UBO_SLOTS - 1);
         if (n > 0)
            (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
      }
   }

   bound->num_ssbo = num_ssbo;
   bound->num_views = num_views;
   bound->num_images = num_images;
   bound->num_samplers = num_samplers;
   return 0;
}

/* Take back the views, images and samplers once the fence is in, which is why it can be
 * done at all: the caller's next call may be rwa_texture_destroy or rwa_sampler_destroy,
 * and a Gallium context that still holds a sampler state is holding a pointer the next
 * delete_sampler_state frees -- softpipe's is a plain FREE (sp_state_sampler.c). The
 * buffers are left bound (see struct rwa_bound).
 */
static void
unbind_stage(struct rwa_device *device, mesa_shader_stage stage,
             const struct rwa_bound *bound)
{
   struct pipe_context *pipe = device->pipe;
   free(bound->const_win);
   if (bound->num_views)
      pipe->set_sampler_views(pipe, stage, 0, 0, bound->num_views, NULL);
   if (bound->num_images)
      pipe->set_shader_images(pipe, stage, 0, 0, bound->num_images, NULL);
   if (bound->num_samplers) {
      void *none[RWA_MAX_TEXTURES] = { 0 };
      pipe->bind_sampler_states(pipe, stage, 0, bound->num_samplers, none);
   }
}

