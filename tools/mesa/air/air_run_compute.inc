/* Compute kernel dispatch and sampler setup, included into air_run.c. */

/* -- compute ---------------------------------------------------------------
 *
 * A kernel writes through pointers instead of returning anything, so what is
 * read back is a device buffer. Two rules decide whether that readback is
 * evidence, and both come from the same mistake made three times with
 * textures: a test can pass for the wrong reason, and the way to stop it is
 * to make every input distinguishable so any confusion gives a wrong number
 * rather than the right one.
 *
 *   - the grid is more than one thread, and every element of the output is
 *     printed, not just the first. With one thread, or one element looked at,
 *     a wrong workgroup index or a wrong global id reads correct.
 *   - the input buffers vary with position (the same (i * 7 + 13) & 0xff
 *     pattern), so a thread reading the wrong offset produces a wrong value
 *     and not a plausible one.
 *
 * A kernel of Apple's that uses atomics and produces sensible numbers here is
 * evidence that it ran and no evidence about atomicity: a sum written by
 * threads that never raced looks exactly like a sum written by threads that
 * did. The kernel that can tell them apart is written for the purpose rather
 * than Apple's -- tools/air/atomiccheck.py, which makes each thread report the
 * value its own fetch_add returned, so the sixteen of them must be 0..15 with
 * nothing repeated whatever order the threads ran in. Even that is only
 * evidence of atomicity where the threads really do run at once, which here
 * they need not.
 */
#define GRID 4

/* The threadgroup this harness dispatches. 4x4x1 by default, because a grid with more than
 * one thread in two dimensions is what makes a wrong index show, and --block X,Y,Z because
 * some kernels are written for a particular one and say so in their own code: both fbs_pcg_*
 * kernels end with `if (tid == 0)` summing 128 entries of a threadgroup array, which is a
 * 128x1x1 reduction. Under 4x4x1 four threads have tid == 0 -- Metal's scalar
 * thread_position_in_threadgroup is the x component, not the flattened index -- so four of
 * them run the whole reduction over the four entries this shape writes, and the word they
 * add up is a property of the dispatch rather than of the kernel (docs/air.md section 5).
 */
static unsigned block[3] = { GRID, GRID, 1 };

#define MAX_IMG 16

/* The shape of each image a kernel touches, read off the intrinsics the
 * frontend emitted rather than off the Metal type name.
 *
 * air.read_texture_* and air.write_texture_* become nir_image_load and
 * nir_image_store (air_intrinsics.c), not texture instructions, so they want
 * PIPE_BIND_SHADER_IMAGE and set_shader_images and not a sampler view -- which
 * is why the draw path's texture code cannot simply be reused here.
 *
 * Per binding and not one format for the kernel: BlitLumaKernel_Unorm reads a
 * uint image and writes a half one, so a single format would be right for one
 * of its two images and quietly wrong for the other. The binding is src[0] and
 * the frontend always emits it as an immediate, so it can be read here.
 */
static void
image_shapes(nir_shader *cs, enum pipe_format fmt[MAX_IMG], bool isf[MAX_IMG],
             enum glsl_sampler_dim dim[MAX_IMG], bool seen[MAX_IMG], unsigned *num)
{
   nir_foreach_function_impl(impl, cs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            nir_alu_type t;
            if (in->intrinsic == nir_intrinsic_image_load)
               t = nir_intrinsic_dest_type(in);
            else if (in->intrinsic == nir_intrinsic_image_store)
               t = nir_intrinsic_src_type(in);
            else
               continue;
            if (!nir_src_is_const(in->src[0]))
               continue;   /* a computed binding; refused by the caller's count */
            unsigned b = nir_src_as_uint(in->src[0]);
            if (b >= MAX_IMG || seen[b])
               continue;
            /* The WIDTH as well as the base type.
             *
             * A 32-bit format for a 16-bit store is not a conversion, it is a
             * descriptor that disagrees with the instruction: ir3 emits
             * `stib.b.typed.2d.u16.4` from air.write_texture_2d.v4f16 and the
             * image it is given says 32 bits per component. MetalFaceMask_DrawSpans
             * is that case -- it declares texture2d<half, write> -- and it is the
             * kernel whose dispatch took a whole boot with it in
             * logs/070755-run.log, printing its bindings and then nothing at all.
             * 22 kernels of the corpus emit a narrow typed image store, so this is
             * not one shader's accident. GL has no 16-bit image store, which is why
             * neither Mesa nor this harness had met it; the draw path already picks
             * the narrow format when a fragment shader returns halves.
             */
            unsigned bits = nir_alu_type_get_type_size(t);

            switch (nir_alu_type_get_base_type(t)) {
            case nir_type_int:
               fmt[b] = bits == 16 ? PIPE_FORMAT_R16G16B16A16_SINT
                      : bits == 8  ? PIPE_FORMAT_R8G8B8A8_SINT
                                   : PIPE_FORMAT_R32G32B32A32_SINT;
               isf[b] = false;
               break;
            case nir_type_uint:
               fmt[b] = bits == 16 ? PIPE_FORMAT_R16G16B16A16_UINT
                      : bits == 8  ? PIPE_FORMAT_R8G8B8A8_UINT
                                   : PIPE_FORMAT_R32G32B32A32_UINT;
               isf[b] = false;
               break;
            default:
               fmt[b] = bits == 16 ? PIPE_FORMAT_R16G16B16A16_FLOAT
                                   : PIPE_FORMAT_R32G32B32A32_FLOAT;
               isf[b] = true;
               break;
            }
            dim[b] = nir_intrinsic_image_dim(in);
            seen[b] = true;
            *num = MAX2(*num, b + 1);
         }
      }
   }
}

/* The same, for the read-only textures a kernel fetches from. A texture is an
 * AIR_RESOURCE_IMAGE only when it is written (air_to_nir.c): a read-only one
 * stays a texture and Metal's read() on it becomes a txf, a texture fetch, so
 * it needs a sampler view even inside a compute shader. BlitKernel_Unorm has
 * one of each -- a txf from its source and an image_store to its destination --
 * and binding only the image left the fetch reading nothing.
 */
static void
texture_shapes(nir_shader *cs, enum pipe_format fmt[MAX_IMG], bool isf[MAX_IMG],
               enum glsl_sampler_dim dim[MAX_IMG], bool seen[MAX_IMG], unsigned *num)
{
   nir_foreach_function_impl(impl, cs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *t = nir_instr_as_tex(instr);
            unsigned b = t->texture_index;
            if (b >= MAX_IMG || seen[b])
               continue;
            switch (nir_alu_type_get_base_type(t->dest_type)) {
            case nir_type_int:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_SINT; isf[b] = false; break;
            case nir_type_uint:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_UINT; isf[b] = false; break;
            default:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_FLOAT; isf[b] = true; break;
            }
            dim[b] = t->sampler_dim;
            seen[b] = true;
            *num = MAX2(*num, b + 1);
         }
      }
   }
}

/* Wait for the work to finish, and say so when it does not.
 *
 * This matters more than it looks. On 20 October a dispatch hung the GPU --
 * our own driver said "gpu: no answer: submission 2 is the last, the core has
 * done 1" -- and every dispatch for the rest of that boot was accepted,
 * returned, and did nothing. air_run did not wait on a fence, so it printed
 * the buffer it had filled itself as though the GPU had answered: a page of
 * numbers that were a statement about the harness and not about the shader.
 * 21 of the 47 kernels of one run were vacuous that way before anyone noticed
 * (docs/air.md section 5).
 *
 * The hang itself turned out to be this harness too: the kernels that did it
 * loop until a buffer value they were never given, and the fill is a byte
 * pattern. So the message below leads with that rather than with the GPU.
 *
 * So the fence is waited on with a timeout, and a timeout is reported as
 * loudly as a crash. A dispatch nobody waited for is not a measurement.
 */
static bool
finish_or_say_why(struct pipe_screen *screen, struct pipe_context *pipe, const char *what)
{
   struct pipe_fence_handle *fence = NULL;
   pipe->flush(pipe, &fence, 0);
   if (!fence) {
      printf("air_run: the driver gave no fence for the %s, so nothing here waited for "
             "the GPU and the numbers below may be the fill\n", what);
      return true;
   }
   bool done = screen->fence_finish(screen, NULL, fence, 2000000000ull); /* 2 s */
   screen->fence_reference(screen, &fence, NULL);
   if (!done)
      printf("air_run: THE GPU DID NOT FINISH the %s within two seconds. Everything below is "
             "what this harness wrote, not what the shader computed. The likeliest reason is "
             "this harness: a kernel whose buffers carry counts, indices or sentinels is being "
             "given (i * 7 + 13) & 0xff, and a loop that leaves on a value it was never given "
             "does not leave at all -- both kernels measured this way turned out to be exactly "
             "that (docs/air.md section 5). And one timeout wedges this GPU for the rest of the "
             "boot, so every later run in it is vacuous too.\n", what);
   return done;
}

/* What a shader's own sampler says, in one line. Printed rather than assumed:
 * the ten modules of the corpus that carry one have no reference to be checked
 * against, so the only thing a reader can verify is that the harness bound
 * what the word says.
 */
static void
print_own_sampler(unsigned slot, const struct air_sampler_state *a, bool compares)
{
   static const char *const addr[] = { "clamp_to_zero", "clamp_to_edge", "repeat",
                                       "mirrored_repeat", "clamp_to_border" };
   static const char *const filt[] = { "nearest", "linear", "bicubic" };
   /* Metal's compare_func, in its own order: `none` at 0 and `never` at 8, which is
    * the value Apple's default carries (docs/air.md 3.3).
    */
   static const char *const cmp[] = { "none", "less", "less_equal", "greater",
                                      "greater_equal", "equal", "not_equal", "always",
                                      "never" };
   printf("air_run: sampler %u is the shader's own: %s/%s filtering, %s addressing, "
          "%s coordinates, compare_func::%s%s\n", slot,
          filt[a->mag_filter % ARRAY_SIZE(filt)], filt[a->min_filter % ARRAY_SIZE(filt)],
          addr[a->address[0] % ARRAY_SIZE(addr)],
          a->pixel_coord ? "pixel" : "normalized",
          a->compare_func < ARRAY_SIZE(cmp) ? cmp[a->compare_func] : "?",
          compares ? " APPLIED (a shadow sample reads this slot)"
                   : " not read (nothing samples this slot with a comparison)");
}

/* The depth texel a shadow sample reads, which is the only pattern that fits.
 *
 * A depth format is normalised to [0, 1] and this harness's colour pattern starts at
 * 100, so the two cannot be the same rule. (x + TEX_SIZE*y) / 16 gives sixteen
 * distinct values over a 4x4 texture, every one exactly representable in binary32 and
 * every one a different answer -- so a comparison that read the wrong texel answers
 * differently, and a reference landing between two of them is exact on both sides.
 * tools/air/sampcheck.py computes its expectations from this same expression.
 */
static float
depth_texel_value(unsigned x, unsigned y)
{
   return (float)(x + TEX_SIZE * y) / 16.0f;
}

/* Which sampler slots a shadow texture instruction reads, out of the translated
 * shader itself.
 *
 * Metal decides AT THE SAMPLE CALL that a comparison happens: sample() and
 * sample_compare() are two instructions over one sampler, and the sampler word
 * only says WHICH comparison. Gallium decides it in the sampler state, through
 * pipe_sampler_state.compare_mode, and softpipe compares on every sample of a
 * sampler that has it set (sp_tex_sample.c: `if (compare_mode != NONE)
 * sample_compare(...)`). So switching it on for a slot a plain sample() reads
 * would turn every texel into a 0 or a 1, and leaving it off for a slot a
 * sample_compare() reads would return the texel instead of the comparison.
 * Exactly these slots, and the shader is the only thing that knows which.
 *
 * On the Adreno the same two lines mean something slightly different and the
 * answer is the same: a6xx has no compare-enable bit at all, only
 * A6XX_TEX_SAMP_1_COMPARE_FUNC, which fd6_sampler_state_create writes only when
 * compare_mode is set -- and the hardware reads it only for the shadow form of
 * the sample instruction, which ir3 selects from nir_tex_instr::is_shadow
 * (IR3_INSTR_S). So the field has to be programmed for these slots, and
 * programming it for the others would be harmless there and wrong here.
 */
static void
compare_sampled_slots(nir_shader *s, bool *slot, unsigned count)
{
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *t = nir_instr_as_tex(instr);
            if (t->is_shadow && t->sampler_index < count)
               slot[t->sampler_index] = true;
         }
      }
   }
}

/* A comparison through a sampler whose own word says `compare_func::none`,
 * refused by name and in the same words air_metal.c's bind_stage refuses it.
 *
 * MTLCompareFunction has eight values and no "not configured", so `none` --
 * value 0 of the shader word's nine -- has no API spelling and arrives at a
 * driver as Never, which is MTLSamplerDescriptor's own default and is right for
 * the plain sample that is all either can do. A COMPARISON is where the two
 * part company: AGX keeps its NONE and its NEVER apart (AGXCompilerCore's
 * _AGCCompareFunction), and what it does differently is not readable from here.
 * Answering such a sample as `never` would be a harness quietly agreeing with
 * itself where the library this is a reference for says no.
 */
static bool
compare_without_a_function(const struct air_shader_info *info,
                           const bool *compare_slot, unsigned count,
                           const char *name)
{
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->kind != AIR_RESOURCE_SAMPLER || !r->inline_sampler)
         continue;
      if (r->index >= count || !compare_slot[r->index])
         continue;
      if (r->sampler.compare_func != 0)
         continue;
      fprintf(stderr, "air_run: %s samples sampler %u with a comparison and its own "
                      "word says compare_func::none, which is not one of "
                      "MTLCompareFunction's eight: a sampler that was not made to "
                      "compare\n", name, r->index);
      return true;
   }
   return false;
}

/* The sampler state a shader declared for itself, as Gallium wants it.
 *
 * Metal's clamp_to_zero is a wrap mode and not a border colour: outside the
 * texture it reads transparent black, in every channel including alpha, and
 * the border colour is not configurable for it. Gallium has no such mode, so
 * it becomes CLAMP_TO_BORDER with a zero border -- which is the same thing and
 * is worth writing down, because the obvious reading of that enum value is
 * clamp_to_border with a default border, and one of Apple's own binaries
 * prints it that way (nir_air.h).
 *
 * Fields with no Gallium equivalent, or none this harness can honour, are left
 * at the default and named by the caller rather than silently dropped:
 * bicubic filtering and the reduction mode AGX itself does not read.
 *
 * The comparison function is honoured, and only for the slots a shadow
 * instruction reads -- `compares`, from compare_sampled_slots above. Its own
 * enumeration is not MTLCompareFunction's and not Gallium's: in the word 0 is
 * `none` and `never` is 8, which is the value Apple's own default carries, so
 * every `constexpr sampler` that names no compare_func arrives here with 8 and
 * must mean no comparison rather than an unknown one.
 */
static struct pipe_sampler_state
sampler_from_air(const struct air_sampler_state *a, bool compares)
{
   static const unsigned wrap[] = {
      [AIR_SAMPLER_CLAMP_TO_ZERO] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
      [AIR_SAMPLER_CLAMP_TO_EDGE] = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      [AIR_SAMPLER_REPEAT] = PIPE_TEX_WRAP_REPEAT,
      [AIR_SAMPLER_MIRRORED_REPEAT] = PIPE_TEX_WRAP_MIRROR_REPEAT,
      [AIR_SAMPLER_CLAMP_TO_BORDER] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
   };
   static const unsigned filt[] = {
      [AIR_SAMPLER_NEAREST] = PIPE_TEX_FILTER_NEAREST,
      [AIR_SAMPLER_LINEAR] = PIPE_TEX_FILTER_LINEAR,
      [AIR_SAMPLER_BICUBIC] = PIPE_TEX_FILTER_LINEAR,
   };
   static const unsigned mip[] = {
      [AIR_SAMPLER_MIP_NONE] = PIPE_TEX_MIPFILTER_NONE,
      [AIR_SAMPLER_MIP_NEAREST] = PIPE_TEX_MIPFILTER_NEAREST,
      [AIR_SAMPLER_MIP_LINEAR] = PIPE_TEX_MIPFILTER_LINEAR,
   };
   struct pipe_sampler_state st = {
      .wrap_s = wrap[a->address[0] % ARRAY_SIZE(wrap)],
      .wrap_t = wrap[a->address[1] % ARRAY_SIZE(wrap)],
      .wrap_r = wrap[a->address[2] % ARRAY_SIZE(wrap)],
      .mag_img_filter = filt[a->mag_filter % ARRAY_SIZE(filt)],
      .min_img_filter = filt[a->min_filter % ARRAY_SIZE(filt)],
      .min_mip_filter = mip[a->mip_filter % ARRAY_SIZE(mip)],
      .unnormalized_coords = a->pixel_coord,
      .max_anisotropy = a->max_anisotropy > 1 ? a->max_anisotropy : 0,
      .min_lod = _mesa_half_to_float(a->lod_min),
      .max_lod = _mesa_half_to_float(a->lod_max),
   };
   /* The comparison, where the shader asked for one. The table is the one in
    * air_metal.c's sampler_state_from_air -- MTLCompareFunction and
    * PIPE_FUNC_x run in the same order, so one table serves both -- and it is
    * written out here rather than cast because 2 `less_equal` would arrive as
    * PIPE_FUNC_EQUAL and 3 `greater` as PIPE_FUNC_LEQUAL, which is the
    * opposite comparison and still a picture.
    */
   if (compares) {
      static const unsigned func[] = {
         [0] = PIPE_FUNC_NEVER,     /* none: no MTLCompareFunction has a name for it */
         [1] = PIPE_FUNC_LESS,      /* less */
         [2] = PIPE_FUNC_LEQUAL,    /* less_equal */
         [3] = PIPE_FUNC_GREATER,   /* greater */
         [4] = PIPE_FUNC_GEQUAL,    /* greater_equal */
         [5] = PIPE_FUNC_EQUAL,     /* equal */
         [6] = PIPE_FUNC_NOTEQUAL,  /* not_equal */
         [7] = PIPE_FUNC_ALWAYS,    /* always */
         [8] = PIPE_FUNC_NEVER,     /* never */
      };
      st.compare_mode = PIPE_TEX_COMPARE_R_TO_TEXTURE;
      st.compare_func = func[a->compare_func % ARRAY_SIZE(func)];
   }

   /* Metal's border_color: 0 transparent black, 1 opaque black, 2 opaque white.
    * clamp_to_zero does not read it -- it is transparent black by definition.
    */
   if (a->address[0] == AIR_SAMPLER_CLAMP_TO_BORDER ||
       a->address[1] == AIR_SAMPLER_CLAMP_TO_BORDER ||
       a->address[2] == AIR_SAMPLER_CLAMP_TO_BORDER) {
      float v = a->border_color == 2 ? 1.0f : 0.0f;
      st.border_color.f[0] = st.border_color.f[1] = st.border_color.f[2] = v;
      st.border_color.f[3] = a->border_color == 0 ? 0.0f : 1.0f;
   }
   return st;
}

/* The state for sampler slot `idx`: the shader's own if it declared one there,
 * and this harness's nearest/clamp-to-edge otherwise -- which is what the API
 * would have bound and what every measurement before inline samplers were
 * decoded was made with.
 */
static struct pipe_sampler_state
sampler_for_slot(const struct air_shader_info *info, unsigned idx, bool unnormalized,
                 bool compares, const struct air_sampler_state **from_shader)
{
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->kind == AIR_RESOURCE_SAMPLER && r->inline_sampler && r->index == idx) {
         if (from_shader)
            *from_shader = &r->sampler;
         return sampler_from_air(&r->sampler, compares);
      }
   }
   if (from_shader)
      *from_shader = NULL;
   /* The API's sampler, and a comparison only if the shader asked for one.
    * MTLSamplerDescriptor's own compareFunction defaults to Never, so this is
    * the API's default sampler said completely rather than left out.
    */
   return (struct pipe_sampler_state){
      .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .min_img_filter = PIPE_TEX_FILTER_NEAREST,
      .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
      .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      .unnormalized_coords = unnormalized,
      .compare_mode = compares ? PIPE_TEX_COMPARE_R_TO_TEXTURE
                               : PIPE_TEX_COMPARE_NONE,
      .compare_func = PIPE_FUNC_NEVER,
   };
}

static int
dispatch_kernel(struct pipe_screen *screen, const struct air_shader_info *info,
                nir_shader *cs, const char *name, const char *how)
{
   /* Metal's threadgroup size is a dispatch argument, not a shader constant,
    * so the frontend leaves it variable (docs/air.md section 7.4) and whoever
    * dispatches has to fix it. air_run is that whoever. It is not optional:
    * softpipe reads the block size from TGSI_PROPERTY_CS_FIXED_BLOCK_*, and
    * with a variable size those are zero, so it runs ZERO threads and the
    * output buffer comes back exactly as it went in. That looked like a
    * kernel that did nothing, and was only visible because the buffer was
    * filled with a distinctive pattern -- had it been zeroed, a kernel named
    * InitMinMax writing zeros would have been indistinguishable from this.
    */
   cs->info.workgroup_size[0] = block[0];
   cs->info.workgroup_size[1] = block[1];
   cs->info.workgroup_size[2] = block[2];
   cs->info.workgroup_size_variable = false;

   finalize_for_gallium(cs, screen);
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   if (!pipe) {
      fprintf(stderr, "air_run: no context for a kernel\n");
      return 1;
   }
   if (!bind_software_constants(pipe, cs)) {
      pipe->destroy(pipe);
      return 1;
   }
   printf("air_run: %s on %s (%s), as a compute kernel\n", name,
          screen->get_name(screen), how);

   /* every buffer the kernel declares; the device ones are what it writes */
   struct pipe_resource *bufs[32] = { 0 };
   /* The bytes this harness wrote, kept until the readback. The dump used to
    * recompute them from pattern_byte(), which --buffer N@OFF=HEX makes a lie:
    * a patched word looked changed by the kernel. Keeping the fill is also the
    * only way to say which words a kernel touched when it touches them past
    * the sixteen the dump prints -- and that is exactly what a prediction out
    * of tools/air/kinput.py has to be compared against.
    */
   uint8_t *before[32] = { 0 };
   unsigned sizes[32] = { 0 };
   bool writable[32] = { false };
   bool img_read[MAX_IMG] = { false }, img_write[MAX_IMG] = { false };
   unsigned num_img = 0;
   bool tex_used[MAX_IMG] = { false };
   unsigned num_tex = 0;
   unsigned num_ssbo = 0, num_written = 0;
   void *const_win = NULL;
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      unsigned bytes = r->size ? r->size : 4096;
      if (r->index >= ARRAY_SIZE(bufs))
         continue;
      uint8_t *bits = calloc(1, bytes);
      bool kept = false;
      unsigned from_file = fill_buffer(r->index, bits, bytes);
      if (from_file)
         printf("air_run: buffer at index %u, %u bytes, %u of them from "
                "AIR_RUN_BUFFER\n", r->index, bytes, from_file);

      if (r->kind == AIR_RESOURCE_CONSTANT_BUFFER) {
         /* A transform matrix filled with the byte pattern sends every sample
          * somewhere arbitrary, so what the shader computes says nothing about
          * what it is for. The identity is the one distinguished value a matrix
          * has, and the resource says it is one: air.arg_type_name is Apple's
          * own ("float4x4"), carried through from the module's metadata. Any
          * other constant buffer keeps the pattern -- this is the only place
          * the harness pretends to know what a buffer means, and it says so.
          */
         if (r->type_name && !strncmp(r->type_name, "float4x4", 8) && bytes >= 64) {
            float *m = (float *)bits;
            memset(m, 0, 64);
            m[0] = m[5] = m[10] = m[15] = 1.0f;
            printf("air_run: constant buffer %u is a %s, so it holds the identity and "
                   "not the fill\n", r->index, r->type_name);
         }
         if (r->const_win) {   /* folded past the sixteen slots into the window (bind below) */
            if (!const_win)
               const_win = calloc(1, info->const_win_size);
            if (const_win)
               memcpy((char *)const_win + r->win_base, bits,
                      MIN2((size_t)r->size, (size_t)bytes));
         } else {
            struct pipe_resource *b = pipe_buffer_create(screen, PIPE_BIND_CONSTANT_BUFFER,
                                                         PIPE_USAGE_DEFAULT, bytes);
            pipe_buffer_write(pipe, b, 0, bytes, bits);
            pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, r->slot,
                                      &(struct pipe_constant_buffer){
                                         .buffer = b, .buffer_size = bytes });
         }
      } else if (r->kind == AIR_RESOURCE_DEVICE_BUFFER) {
         bufs[r->index] = pipe_buffer_create(screen, PIPE_BIND_SHADER_BUFFER,
                                             PIPE_USAGE_STAGING, bytes);
         pipe_buffer_write(pipe, bufs[r->index], 0, bytes, bits);
         sizes[r->index] = bytes;
         before[r->index] = bits;
         kept = true;
         writable[r->index] = r->write;
         num_ssbo = MAX2(num_ssbo, r->index + 1);
         num_written += r->write ? 1 : 0;
      } else if (r->kind == AIR_RESOURCE_IMAGE) {
         if (r->index >= MAX_IMG) {
            free(bits);
            fprintf(stderr, "air_run: %s binds an image at %u, past the %u this harness "
                            "keeps room for\n", name, r->index, MAX_IMG);
            return 1;
         }
         img_read[r->index] = r->read;
         img_write[r->index] = r->write;
         num_img = MAX2(num_img, r->index + 1);
      } else if (r->kind == AIR_RESOURCE_TEXTURE) {
         if (r->index >= MAX_IMG) {
            free(bits);
            fprintf(stderr, "air_run: %s binds a texture at %u, past the %u this harness "
                            "keeps room for\n", name, r->index, MAX_IMG);
            return 1;
         }
         tex_used[r->index] = true;
         num_tex = MAX2(num_tex, r->index + 1);
      }
      if (!kept)
         free(bits);
   }
   unsigned num_img_written = 0;
   for (unsigned i = 0; i < num_img; i++)
      num_img_written += img_write[i] ? 1 : 0;
   if (!num_written && !num_img_written) {
      fprintf(stderr, "air_run: %s writes no device buffer and no image, so a dispatch "
                      "would leave nothing to read back\n", name);
      return 1;
   }

   struct pipe_shader_buffer sb[32] = { 0 };
   unsigned mask = 0;
   for (unsigned i = 0; i < num_ssbo; i++) {
      sb[i].buffer = bufs[i];
      sb[i].buffer_size = sizes[i];
      if (writable[i])
         mask |= 1u << i;
   }
   pipe->set_shader_buffers(pipe, MESA_SHADER_COMPUTE, 0, num_ssbo, sb, mask);

   /* The images, one resource each, with the format the kernel's own intrinsics
    * declare. A readable one is filled with the same texel pattern the draw
    * path uses -- texel (x,y,z) of slot n is (100+x+1000n, 200+y, 300+x+y+z,
    * 400) -- so that a kernel reading the wrong slot or the wrong texel cannot
    * come back with the right answer. A write-only one is left as the pattern
    * bytes so that a kernel which writes nothing is distinguishable from one
    * that writes zeros.
    */
   enum pipe_format ifmt[MAX_IMG];
   bool iisf[MAX_IMG];
   enum glsl_sampler_dim idim[MAX_IMG];
   bool iseen[MAX_IMG] = { false };
   unsigned nshape = 0;
   for (unsigned i = 0; i < MAX_IMG; i++) {
      ifmt[i] = PIPE_FORMAT_R32G32B32A32_FLOAT;
      iisf[i] = true;
      idim[i] = GLSL_SAMPLER_DIM_2D;
   }
   image_shapes(cs, ifmt, iisf, idim, iseen, &nshape);
   struct pipe_resource *imgs[MAX_IMG] = { 0 };
   struct pipe_image_view ivs[MAX_IMG] = { 0 };
   for (unsigned i = 0; i < num_img; i++) {
      if (idim[i] != GLSL_SAMPLER_DIM_2D && idim[i] != GLSL_SAMPLER_DIM_3D) {
         fprintf(stderr, "air_run: %s uses an image of a kind this harness does not bind "
                         "(dim %u at %u)\n", name, idim[i], i);
         return 1;
      }
      unsigned depth = idim[i] == GLSL_SAMPLER_DIM_3D ? TEX_SIZE : 1;
      struct pipe_resource itempl = {
         .target = idim[i] == GLSL_SAMPLER_DIM_3D ? PIPE_TEXTURE_3D : PIPE_TEXTURE_2D,
         .format = ifmt[i],
         .width0 = TEX_SIZE, .height0 = TEX_SIZE, .depth0 = depth, .array_size = 1,
         .bind = PIPE_BIND_SHADER_IMAGE | PIPE_BIND_SAMPLER_VIEW,
      };
      imgs[i] = screen->resource_create(screen, &itempl);
      if (!imgs[i]) {
         fprintf(stderr, "air_run: the driver would not make image %u (%s)\n", i,
                 util_format_short_name(ifmt[i]));
         return 1;
      }
      float texels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      int32_t itexels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX_SIZE; y++)
            for (unsigned x = 0; x < TEX_SIZE; x++) {
               unsigned k = ((z * TEX_SIZE + y) * TEX_SIZE + x) * 4;
               texel_value(i, x, y, z, &texels[k]);
               for (unsigned c = 0; c < 4; c++)
                  itexels[k + c] = (int32_t)texels[k + c];
            }
      struct pipe_box box = { .width = TEX_SIZE, .height = TEX_SIZE, .depth = depth };
      pipe->texture_subdata(pipe, imgs[i], 0, PIPE_MAP_WRITE, &box,
                            iisf[i] ? (const void *)texels : (const void *)itexels,
                            TEX_SIZE * 4 * sizeof(float),
                            TEX_SIZE * TEX_SIZE * 4 * sizeof(float));
      ivs[i] = (struct pipe_image_view){
         .resource = imgs[i],
         .format = ifmt[i],
         .access = (img_read[i] ? PIPE_IMAGE_ACCESS_READ : 0) |
                   (img_write[i] ? PIPE_IMAGE_ACCESS_WRITE : 0),
         .shader_access = (img_read[i] ? PIPE_IMAGE_ACCESS_READ : 0) |
                          (img_write[i] ? PIPE_IMAGE_ACCESS_WRITE : 0),
         .u.tex.level = 0, .u.tex.first_layer = 0,
         .u.tex.last_layer = depth - 1,
      };
   }
   if (num_img)
      pipe->set_shader_images(pipe, MESA_SHADER_COMPUTE, 0, num_img, 0, ivs);

   /* and the read-only textures, as sampler views with the same texel pattern */
   enum pipe_format tfmt[MAX_IMG];
   bool tisf[MAX_IMG];
   enum glsl_sampler_dim tdim[MAX_IMG];
   bool tseen[MAX_IMG] = { false };
   unsigned ntshape = 0;
   for (unsigned i = 0; i < MAX_IMG; i++) {
      tfmt[i] = PIPE_FORMAT_R32G32B32A32_FLOAT;
      tisf[i] = true;
      tdim[i] = GLSL_SAMPLER_DIM_2D;
   }
   texture_shapes(cs, tfmt, tisf, tdim, tseen, &ntshape);
   struct pipe_resource *texes[MAX_IMG] = { 0 };
   struct pipe_sampler_view *views[MAX_IMG] = { 0 };
   void *csampler = NULL;
   for (unsigned i = 0; i < num_tex; i++) {
      if (!tex_used[i])
         continue;
      if (tdim[i] != GLSL_SAMPLER_DIM_2D && tdim[i] != GLSL_SAMPLER_DIM_3D) {
         fprintf(stderr, "air_run: %s fetches a texture of a kind this harness does not "
                         "bind (dim %u at %u)\n", name, tdim[i], i);
         return 1;
      }
      unsigned depth = tdim[i] == GLSL_SAMPLER_DIM_3D ? TEX_SIZE : 1;
      struct pipe_resource ttempl = {
         .target = tdim[i] == GLSL_SAMPLER_DIM_3D ? PIPE_TEXTURE_3D : PIPE_TEXTURE_2D,
         .format = tfmt[i],
         .width0 = TEX_SIZE, .height0 = TEX_SIZE, .depth0 = depth, .array_size = 1,
         .bind = PIPE_BIND_SAMPLER_VIEW,
      };
      texes[i] = screen->resource_create(screen, &ttempl);
      if (!texes[i]) {
         fprintf(stderr, "air_run: the driver would not make texture %u (%s)\n", i,
                 util_format_short_name(tfmt[i]));
         return 1;
      }
      float texels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      int32_t itexels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX_SIZE; y++)
            for (unsigned x = 0; x < TEX_SIZE; x++) {
               unsigned k = ((z * TEX_SIZE + y) * TEX_SIZE + x) * 4;
               texel_value(i, x, y, z, &texels[k]);
               for (unsigned c = 0; c < 4; c++)
                  itexels[k + c] = (int32_t)texels[k + c];
            }
      struct pipe_box tbox = { .width = TEX_SIZE, .height = TEX_SIZE, .depth = depth };
      pipe->texture_subdata(pipe, texes[i], 0, PIPE_MAP_WRITE, &tbox,
                            tisf[i] ? (const void *)texels : (const void *)itexels,
                            TEX_SIZE * 4 * sizeof(float),
                            TEX_SIZE * TEX_SIZE * 4 * sizeof(float));
      struct pipe_sampler_view vtempl = { 0 };
      u_sampler_view_default_template(&vtempl, texes[i], texes[i]->format);
      views[i] = pipe->create_sampler_view(pipe, texes[i], &vtempl);
   }
   if (num_tex) {
      /* one entry per texture, not the address of one pointer. bind_sampler_states
       * reads num_tex pointers from what it is given, so passing &csampler made
       * the driver take whatever followed that single variable on the stack and
       * store it as samplers[1..]; fd6_texture_state then dereferenced those for
       * ->seqno and died at a wild address. It only showed with two or more
       * textures, which is why 22 kernels bound images here without trouble and
       * four did not. The draw path below always did this correctly.
       *
       * One state per slot, and not one shared: a slot's sampler is whatever
       * that slot declares, and a shader may mix its own with the API's.
       */
      void *csamplers[MAX_IMG] = { 0 };
      bool compare_slot[MAX_IMG] = { 0 };
      compare_sampled_slots(cs, compare_slot, MAX_IMG);
      if (compare_without_a_function(info, compare_slot, MAX_IMG, name))
         return 1;
      for (unsigned i = 0; i < num_tex && i < MAX_IMG; i++) {
         const struct air_sampler_state *own = NULL;
         struct pipe_sampler_state st = sampler_for_slot(info, i, true,
                                                         compare_slot[i], &own);
         csamplers[i] = pipe->create_sampler_state(pipe, &st);
         csampler = csamplers[i];
         if (own)
            print_own_sampler(i, own, compare_slot[i]);
      }
      pipe->bind_sampler_states(pipe, MESA_SHADER_COMPUTE, 0, num_tex, csamplers);
      pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 0, num_tex, 0, views);
   }

   void *cso = pipe->create_compute_state(pipe, &(struct pipe_compute_state){
      .ir_type = PIPE_SHADER_IR_NIR, .prog = cs,
      .static_shared_mem = cs->info.shared_size });
   if (!cso) {
      fprintf(stderr, "air_run: the driver would not make a compute state of it\n");
      return 1;
   }
   pipe->bind_compute_state(pipe, cso);
   if (const_win)
      pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, AIR_UBO_SLOTS - 1,
                                &(struct pipe_constant_buffer){
                                   .user_buffer = const_win,
                                   .buffer_size = info->const_win_size });

   /* more than one thread, and the whole grid, so that a wrong index shows */
   struct pipe_grid_info grid = {
      .work_dim = 3,
      .block = { block[0], block[1], block[2] },
      .grid = { 1, 1, 1 },
   };
   printf("air_run: dispatching %ux%ux%u threads in one workgroup; %u device buffer%s bound, "
          "%u of them written, filled with %s\n",
          block[0], block[1], block[2], num_ssbo, num_ssbo == 1 ? "" : "s", num_written,
          zero_fill ? "ZEROS (--zero: a degenerate input, see docs/air.md section 5)"
                    : ones_fill ? "0xff BYTES (--ones: a degenerate input)"
                    : library_fill ? "the floats ((w * 13) % 29 - 14) / 4"
                                   : "(i * 7 + 13) & 0xff");
   if (num_img || num_tex)
      printf("air_run: %u image%s bound (%u written) and %u texture%s, %ux%u, texel (x,y,z) of "
             "slot n = (100+x+1000n, 200+y, 300+x+y+z, 400)\n", num_img,
             num_img == 1 ? "" : "s", num_img_written, num_tex, num_tex == 1 ? "" : "s",
             TEX_SIZE, TEX_SIZE);
   pipe->launch_grid(pipe, &grid);
   bool finished = finish_or_say_why(screen, pipe, "dispatch");

   for (unsigned i = 0; i < num_ssbo; i++) {
      if (!writable[i])
         continue;
      struct pipe_transfer *xfer;
      uint32_t *p = pipe_buffer_map(pipe, bufs[i], PIPE_MAP_READ, &xfer);
      if (!p) {
         fprintf(stderr, "air_run: device buffer %u could not be read back\n", i);
         return 1;
      }
      unsigned words = MIN2(sizes[i] / 4, block[0] * block[1] * block[2]);
      printf("air_run: device buffer %u after the dispatch, first %u words:\n", i, words);
      for (unsigned w = 0; w < words; w++)
         printf("  [%2u] %11u  0x%08x\n", w, p[w], p[w]);

      /* A buffer the shader writes that comes back bit-identical to the fill
       * is not a result, and the only thing worse than no number is a number
       * that looks like one.
       *
       * This is not hypothetical and not rare. On the phone, a boot survives
       * about two dozen dispatches that bind textures or images; after that
       * every dispatch still "succeeds", every readback still prints, and
       * nothing the GPU writes is ever visible again -- measured on
       * 20 October (logs/182513-run.log: writes land for the 13th, 15th and
       * 23rd kernel of the run and for nothing after it, while the same
       * kernels and four probe kernels all write correctly in a short run,
       * logs/182805-run.log). The same boot later refuses an allocation
       * outright with "bo_allocate: alloc failed". So the words below can be
       * a statement about how long the run had been going rather than about
       * the shader, and whoever reads them has to be told which.
       */
      /* and then every word of the whole buffer that differs from what went in,
       * capped so that a kernel which rewrites all 1024 of them does not push
       * the rest of a run out of the boot log (the loader keeps the first
       * 256 KB and the last 1 MB and drops the middle).
       */
      unsigned changed = 0, shown = 0;
      const uint32_t *was = (const uint32_t *)before[i];
      for (unsigned w = 0; was && w < sizes[i] / 4; w++) {
         if (p[w] == was[w])
            continue;
         changed++;
         if (shown < 24) {
            printf("  changed [%4u] 0x%08x -> 0x%08x\n", w, was[w], p[w]);
            shown++;
         }
      }
      if (was && changed)
         printf("air_run: device buffer %u: %u of its %u words differ from what this "
                "harness wrote%s\n", i, changed, sizes[i] / 4,
                shown < changed ? " (the first 24 are above)" : "");
      else if (was)
         printf("air_run: device buffer %u came back bit-identical to the fill, so this "
                "dispatch wrote nothing visible. Either the shader writes elsewhere, or "
                "the GPU has stopped making writes visible -- which it does after a few "
                "dozen dispatches in one boot (docs/air.md section 4).\n", i);
      pipe_buffer_unmap(pipe, xfer);
   }

   if (!finished) {
      fprintf(stderr, "air_run: %s: the dispatch did not finish, so no number here is "
                      "evidence\n", name);
      return 1;
   }

   /* and every image the kernel wrote: without this a dispatch that writes only
    * an image -- which is most of the corpus's kernels -- would have nothing to
    * read back, and air_run would have reported a successful run of something
    * it had not looked at.
    */
   for (unsigned i = 0; i < num_img; i++) {
      if (!img_write[i])
         continue;
      struct pipe_transfer *xfer;
      unsigned depth = idim[i] == GLSL_SAMPLER_DIM_3D ? TEX_SIZE : 1;
      void *pix = pipe->texture_map(pipe, imgs[i], 0, PIPE_MAP_READ,
                                    &(struct pipe_box){ .width = TEX_SIZE,
                                                        .height = TEX_SIZE,
                                                        .depth = depth }, &xfer);
      if (!pix) {
         fprintf(stderr, "air_run: image %u could not be read back\n", i);
         return 1;
      }
      printf("air_run: image %u after the dispatch, texels (0,0) and (1,1):\n", i);
      for (unsigned t = 0; t < 2; t++) {
         unsigned x = t, y = t;
         for (unsigned c = 0; c < 4; c++) {
            unsigned k = (y * (xfer->stride / (4 * sizeof(float))) + x) * 4 + c;
            if (iisf[i])
               printf("  (%u,%u)[%u] %.9g\n", x, y, c, (double)((float *)pix)[k]);
            else
               printf("  (%u,%u)[%u] %11d\n", x, y, c, ((int32_t *)pix)[k]);
         }
      }
      pipe->texture_unmap(pipe, xfer);
   }

   pipe->destroy(pipe);
   screen->destroy(screen);
   return 0;
}

