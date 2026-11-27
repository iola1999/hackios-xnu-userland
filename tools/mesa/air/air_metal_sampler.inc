/* Metal/AIR sampler state translation and lifecycle, included into air_metal.c. */

/* MTLCompareFunction as PIPE_FUNC_x. It lives with the depth/stencil state further
 * down, because a depth test is where a compare function was first needed; the sampler
 * needs the same translation now that the shader's own `constexpr sampler` word carries
 * one, and the whole argument of docs/air.md 3.3 is that the two paths speak ONE
 * vocabulary. So it is declared here rather than copied.
 */
static bool compare_function_from_metal(unsigned f, unsigned *out, const char *what,
                                        char *why, size_t why_size);

/*
 * Metal's sampler in Gallium's terms. One function, for both of the two ways a sampler
 * reaches this library: the API's (MTLSamplerDescriptor, through rwa_sampler_state) and
 * the shader's own `constexpr sampler` word, which sampler_state_from_air turns into
 * the first. Keeping one mapping is what makes the two comparable, which is the whole
 * argument of air_metal.h's comment on rwa_sampler_state and of docs/air.md 3.3's 15
 * cases.
 *
 * Metal's clamp_to_zero is a wrap mode and not a border colour: outside the texture it
 * reads transparent black, in every channel including alpha, and the border colour is
 * not configurable for it. Gallium has no such mode, so it becomes CLAMP_TO_BORDER with
 * a zero border -- which is the same thing and is worth writing down, because the
 * obvious reading of that enum value is clamp_to_border with a default border, and one
 * of Apple's own binaries prints it that way (nir_air.h, docs/air.md 3.3).
 *
 * rwa_sampler_state carries no border colour, so clamp_to_border gets Metal's own
 * default, MTLSamplerBorderColorTransparentBlack -- the same thing clamp_to_zero reads.
 * A driver that wants opaque black or white has nothing in this contract to say it
 * with, and sampler_state_from_air refuses a shader's word that asks for one rather
 * than quietly sampling the wrong colour.
 */
static bool
sampler_state_from_metal(const struct rwa_sampler_state *st, struct pipe_sampler_state *out,
                         char *why, size_t why_size)
{
   static const unsigned wrap[] = {
      [RWA_ADDRESS_CLAMP_TO_EDGE] = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      [RWA_ADDRESS_MIRROR_CLAMP_TO_EDGE] = PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE,
      [RWA_ADDRESS_REPEAT] = PIPE_TEX_WRAP_REPEAT,
      [RWA_ADDRESS_MIRROR_REPEAT] = PIPE_TEX_WRAP_MIRROR_REPEAT,
      [RWA_ADDRESS_CLAMP_TO_ZERO] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
      [RWA_ADDRESS_CLAMP_TO_BORDER] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
   };
   static const char *const axis[3] = { "s", "t", "r" };

   for (unsigned i = 0; i < 3; i++) {
      if (st->address[i] >= ARRAY_SIZE(wrap)) {
         say(why, why_size, "address mode %u on %s is not one of MTLSamplerAddressMode's six",
             st->address[i], axis[i]);
         return false;
      }
   }
   if (st->min_filter > RWA_FILTER_CUBIC || st->mag_filter > RWA_FILTER_CUBIC) {
      say(why, why_size, "filters %u and %u: MTLSamplerMinMagFilter is nearest, linear "
                         "or cubic", st->min_filter, st->mag_filter);
      return false;
   }
   if (st->mip_filter > RWA_MIP_LINEAR) {
      say(why, why_size, "mip filter %u is not one of MTLSamplerMipFilter's three",
          st->mip_filter);
      return false;
   }
   unsigned compare_func;
   if (!compare_function_from_metal(st->compare_function, &compare_func,
                                    "the sampler's compare function", why, why_size))
      return false;
   if (st->max_anisotropy > 16) {
      say(why, why_size, "a maxAnisotropy of %u, where Metal's range is 1 to 16",
          st->max_anisotropy);
      return false;
   }
   if (!(st->lod_min <= st->lod_max)) {
      say(why, why_size, "a LOD clamp of [%g, %g]", (double)st->lod_min,
          (double)st->lod_max);
      return false;
   }

   /* MTLSamplerMipFilter and PIPE_TEX_MIPFILTER are not in the same order -- Gallium's
    * NONE is last and Metal's NotMipmapped is first -- so this is a table and not a
    * cast.
    */
   static const unsigned mip[] = {
      [RWA_MIP_NOT_MIPMAPPED] = PIPE_TEX_MIPFILTER_NONE,
      [RWA_MIP_NEAREST] = PIPE_TEX_MIPFILTER_NEAREST,
      [RWA_MIP_LINEAR] = PIPE_TEX_MIPFILTER_LINEAR,
   };

   unsigned s_wrap = (st->address[0] == RWA_ADDRESS_CLAMP_TO_ZERO || st->address[0] == RWA_ADDRESS_CLAMP_TO_BORDER)
                        ? PIPE_TEX_WRAP_CLAMP_TO_EDGE : wrap[st->address[0]];
   unsigned t_wrap = (st->address[1] == RWA_ADDRESS_CLAMP_TO_ZERO || st->address[1] == RWA_ADDRESS_CLAMP_TO_BORDER)
                        ? PIPE_TEX_WRAP_CLAMP_TO_EDGE : wrap[st->address[1]];
   unsigned r_wrap = (st->address[2] == RWA_ADDRESS_CLAMP_TO_ZERO || st->address[2] == RWA_ADDRESS_CLAMP_TO_BORDER)
                        ? PIPE_TEX_WRAP_CLAMP_TO_EDGE : wrap[st->address[2]];

   *out = (struct pipe_sampler_state){
      .wrap_s = s_wrap,
      .wrap_t = t_wrap,
      .wrap_r = r_wrap,
      /* Gallium has no cubic filter; cubic is sampled linear */
      .min_img_filter = st->min_filter != RWA_FILTER_NEAREST ? PIPE_TEX_FILTER_LINEAR
                                                             : PIPE_TEX_FILTER_NEAREST,
      .mag_img_filter = st->mag_filter != RWA_FILTER_NEAREST ? PIPE_TEX_FILTER_LINEAR
                                                             : PIPE_TEX_FILTER_NEAREST,
      .min_mip_filter = mip[st->mip_filter],
      /* Metal decides at the SAMPLE CALL whether a comparison happens
       * (sample_compare) and not in the sampler, so this state is the one a plain
       * sample() wants and rwa_sampler_create builds a second object out of it with
       * the comparison switched on (struct rwa_sampler). A stage binds that one into
       * the slots a shadow instruction reads and this one everywhere else -- which is
       * Metal's rule said in Gallium's words, and is why the function below is mapped
       * through a table whether a comparison happens or not.
       */
      .compare_mode = PIPE_TEX_COMPARE_NONE,
      .compare_func = compare_func,
      /* Gallium's field means the opposite of Metal's property, and both are called
       * something like "normalized": zero anisotropy is off, and unnormalized_coords
       * is pixel space.
       */
      .unnormalized_coords = !st->normalized_coordinates,
      .max_anisotropy = st->max_anisotropy > 1 ? st->max_anisotropy : 0,
      .min_lod = st->lod_min,
      .max_lod = st->lod_max,
   };
   /* transparent black, which is both Metal's default border colour and what
    * clamp_to_zero reads by definition. Written out rather than left to the zeroing
    * above, because it is a decision and not an omission.
    */
   out->border_color.f[0] = out->border_color.f[1] = 0.0f;
   out->border_color.f[2] = out->border_color.f[3] = 0.0f;

   /* Apple's own header says non-normalized coordinates "should only be used with 1D
    * and 2D textures with the ClampToEdge wrap mode, otherwise the results of sampling
    * are undefined" -- and this library does not enforce that, on purpose: one of
    * Apple's own shaders in the corpus is sampler(coord::pixel, filter::linear,
    * address::clamp_to_zero), and sampcheck.py measured what that must return against
    * Apple's rules, 15 cases exact (docs/air.md 3.3). Refusing it here would refuse a
    * shader that has been checked.
    */
   return true;
}

/* The shader's own `constexpr sampler`, said in the API's words first.
 *
 * The two enumerations are not the same, and that is the whole reason this goes through
 * rwa_sampler_state instead of mapping the word straight to Gallium: in the 64-bit word
 * a shader carries, 0 is clamp_to_zero and 1 clamp_to_edge (nir_air.h, docs/air.md
 * 3.3); in MTLSamplerAddressMode 0 is clampToEdge and 4 clampToZero. One table indexed
 * by "the address mode" would be right for one of them and silently wrong for the other
 * -- the commonest field in the corpus, and the same trap docs/air.md 3.3 records about
 * Apple's own printer.
 */
static bool
sampler_state_from_air(const struct air_sampler_state *a, unsigned index,
                       struct rwa_sampler_state *out, char *why, size_t why_size)
{
   static const unsigned address[] = {
      [AIR_SAMPLER_CLAMP_TO_ZERO] = RWA_ADDRESS_CLAMP_TO_ZERO,
      [AIR_SAMPLER_CLAMP_TO_EDGE] = RWA_ADDRESS_CLAMP_TO_EDGE,
      [AIR_SAMPLER_REPEAT] = RWA_ADDRESS_REPEAT,
      [AIR_SAMPLER_MIRRORED_REPEAT] = RWA_ADDRESS_MIRROR_REPEAT,
      [AIR_SAMPLER_CLAMP_TO_BORDER] = RWA_ADDRESS_CLAMP_TO_BORDER,
   };

   for (unsigned i = 0; i < 3; i++) {
      if (a->address[i] >= ARRAY_SIZE(address)) {
         say(why, why_size, "the shader's own sampler at index %u carries address mode %u, "
                            "which is none of the five its word can hold", index,
             a->address[i]);
         return false;
      }
      /* The word has a border colour and this contract has not, so a border that is
       * not Metal's default has nowhere to go. Refused rather than sampled as
       * transparent black: no module of the corpus attests clamp_to_border at all
       * (docs/air.md 3.3 lists it among the decoded and unexercised fields), so this
       * is a shader nobody has seen asking for a colour nobody can bind.
       */
      if (a->address[i] == AIR_SAMPLER_CLAMP_TO_BORDER && a->border_color != 0) {
         say(why, why_size, "the shader's own sampler at index %u clamps to a border colour "
                            "%u (opaque black or white), and nothing in this contract can "
                            "bind one: Metal's default, transparent black, is the only "
                            "border a sampler here has", index, a->border_color);
         return false;
      }
   }

   /* Bicubic has no Gallium equivalent and is sampled linear, as an API sampler's cubic
    * filter is (sampler_state_from_metal); the picture is softer where the texture is
    * magnified. A refusal here leaves the wallpaper undrawn: MercuryPosterExtension's
    * Space shaders declare a constexpr bicubic sampler. */
   if (a->mag_filter > AIR_SAMPLER_BICUBIC || a->min_filter > AIR_SAMPLER_BICUBIC ||
       a->mip_filter > AIR_SAMPLER_MIP_LINEAR) {
      say(why, why_size, "the shader's own sampler at index %u carries filters %u/%u/%u, "
                         "which are not values its word can hold", index, a->mag_filter,
          a->min_filter, a->mip_filter);
      return false;
   }

   /*
    * The word's comparison function, which is NOT MTLCompareFunction's order and not
    * AGX's either -- the one field in this word where a cast would turn a comparison
    * into its opposite, so it is a table, and the table has three sources.
    *
    *   AIR, bits 16-19         MTLCompareFunction        AGX's _AGCCompareFunction
    *   0 none                  (Never: see below)        0 NONE
    *   1 less                  1 Less                    2 LESS
    *   2 less_equal            3 LessEqual               3 LEQUAL
    *   3 greater               4 Greater                 4 GREATER
    *   4 greater_equal         6 GreaterEqual            5 GEQUAL
    *   5 equal                 2 Equal                   6 EQUAL
    *   6 not_equal             5 NotEqual                7 NOTEQUAL
    *   7 always                7 Always                  8 ALWAYS
    *   8 never                 0 Never                   1 NEVER
    *
    * Casting the AIR value into MTLCompareFunction is wrong for five of the nine and
    * reverses the direction of two of them: 2 `less_equal` would arrive as Equal and 3
    * `greater` as LessEqual. Casting it into AGX's enumeration -- the other plausible
    * reading, since AGX is what Apple's own stack does with this field -- is wrong for
    * eight of the nine and, decisively, would make the DEFAULT mean `always`.
    *
    * Where the three columns come from:
    *
    *  - Apple's own metal_stdlib on the device declares `enum class compare_func` with
    *    these nine names in this order (metal_texture), and metal_config defines both
    *    __HAVE_NONE_COMPARE_FUNCTION__ and __HAVE_ALWAYS_NEVER_COMPARE_FUNCTION__ for
    *    this language version, so all nine exist.
    *  - the VALUES are clang's own, out of the macro table in libGPUCompilerImpl that
    *    defines __METAL_COMPARE_FUNC_*__: NOT_EQUAL 6, ALWAYS 7, NEVER 8, each value
    *    emitted beside its name.
    *  - and the mapping is confirmed by Apple's DECODER, AIRConstSampler::
    *    constructFlatReply in AGXCompilerCore, which reads this very field and
    *    translates it with a byte table -- `ubfx x8, x8, #16, #4; sub x8, #1;
    *    lsr x9, 0x0108070605040302, x8*8` -- so field 1 becomes AGC 2 (LESS), field 8
    *    becomes AGC 1 (NEVER), and field 0 falls out of range to AGC 0 (NONE). The
    *    same function reads the address modes at bits 0-8, the filters at 9-14 and the
    *    two LOD halves at bytes 3 and 5, which is how this reading of the whole word
    *    is checked and not only of this field.
    *
    * `never` AT 8 IS THE DEFAULT, AND THAT IS THE WHOLE POINT. _get_sampler_state() in
    * Apple's header initialises the field to __METAL_COMPARE_FUNC_NEVER__, so every
    * `constexpr sampler` whose source names no compare_func carries 8 -- including all
    * three words the 456-module corpus holds, whose every other unnamed field also
    * comes out equal to the header's default. The refusal this replaces tested for
    * `!= 0`, read `none` as the default, and so refused every inline sampler Apple's
    * compiler has ever emitted. In one boot of the window server that was 653 of 653
    * failed draws (logs/032854-run.log), all of them at value 8.
    *
    * `none` has no MTLCompareFunction: the API enumeration has eight values and no
    * "not configured". It becomes Never, which is MTLSamplerDescriptor's own default
    * for compareFunction, so a word that says `compare_func::none` and a descriptor
    * nobody touched arrive as the same sampler -- true for a plain sample, which is
    * the only thing either can do. What is NOT true is that they mean the same to a
    * comparison, where AGX keeps NONE and NEVER apart, so bind_stage refuses a shadow
    * instruction that reads a sampler whose word says `none` rather than comparing
    * with a function Apple spells differently.
    */
   static const unsigned compare[] = {
      [0] = 0,   /* none          -> MTLCompareFunctionNever, and see above */
      [1] = 1,   /* less          -> MTLCompareFunctionLess */
      [2] = 3,   /* less_equal    -> MTLCompareFunctionLessEqual */
      [3] = 4,   /* greater       -> MTLCompareFunctionGreater */
      [4] = 6,   /* greater_equal -> MTLCompareFunctionGreaterEqual */
      [5] = 2,   /* equal         -> MTLCompareFunctionEqual */
      [6] = 5,   /* not_equal     -> MTLCompareFunctionNotEqual */
      [7] = 7,   /* always        -> MTLCompareFunctionAlways */
      [8] = 0,   /* never         -> MTLCompareFunctionNever */
   };
   if (a->compare_func >= ARRAY_SIZE(compare)) {
      say(why, why_size, "the shader's own sampler at index %u carries compare function %u, "
                         "and Metal's compare_func has nine values: 0 none to 8 never",
          index, a->compare_func);
      return false;
   }

   static const unsigned filter[] = {
      [AIR_SAMPLER_NEAREST] = RWA_FILTER_NEAREST,
      [AIR_SAMPLER_LINEAR] = RWA_FILTER_LINEAR,
      [AIR_SAMPLER_BICUBIC] = RWA_FILTER_CUBIC,
   };
   /* AIR_SAMPLER_MIP_* and MTLSamplerMipFilter happen to be in the same order; said as
    * a table anyway, so that the next field someone adds is not cast through by habit.
    */
   static const unsigned mip[] = {
      [AIR_SAMPLER_MIP_NONE] = RWA_MIP_NOT_MIPMAPPED,
      [AIR_SAMPLER_MIP_NEAREST] = RWA_MIP_NEAREST,
      [AIR_SAMPLER_MIP_LINEAR] = RWA_MIP_LINEAR,
   };

   *out = (struct rwa_sampler_state){
      .address = { address[a->address[0]], address[a->address[1]], address[a->address[2]] },
      .min_filter = filter[a->min_filter],
      .mag_filter = filter[a->mag_filter],
      .mip_filter = mip[a->mip_filter],
      .compare_function = compare[a->compare_func],
      .max_anisotropy = a->max_anisotropy,
      /* the word says coord::pixel; the descriptor says normalizedCoordinates = NO */
      .normalized_coordinates = !a->pixel_coord,
      /* halves, as their bits, which is how the word carries them: the corpus's
       * samplers all come out as [0, 65504], and that range is two separate 16-bit
       * fields a wrong layout would not land on (docs/air.md 3.3).
       */
      .lod_min = _mesa_half_to_float(a->lod_min),
      .lod_max = _mesa_half_to_float(a->lod_max),
   };
   return true;
}

struct rwa_sampler *
rwa_sampler_create(struct rwa_device *device, const struct rwa_sampler_state *state,
                   char *why, size_t why_size)
{
   RWA_LOCKED;
   if (!state) {
      say(why, why_size, "a sampler with no state");
      return NULL;
   }

   struct pipe_sampler_state st;
   if (!sampler_state_from_metal(state, &st, why, why_size))
      return NULL;

   struct rwa_sampler *sampler = calloc(1, sizeof(*sampler));
   if (!sampler) {
      say(why, why_size, "out of memory making a sampler");
      return NULL;
   }
   sampler->device = device;
   sampler->cubic = state->min_filter == RWA_FILTER_CUBIC ||
                    state->mag_filter == RWA_FILTER_CUBIC;
   sampler->cso = device->pipe->create_sampler_state(device->pipe, &st);
   /* and the same state once more with the comparison switched on, for the slots a
    * sample_compare reads: struct rwa_sampler says why there have to be two. The
    * function is already in `st` -- sampler_state_from_metal mapped it whether a
    * comparison happens or not -- so this is one field and not a second translation.
    */
   st.compare_mode = PIPE_TEX_COMPARE_R_TO_TEXTURE;
   sampler->cso_compare = device->pipe->create_sampler_state(device->pipe, &st);
   if (!sampler->cso || !sampler->cso_compare) {
      say(why, why_size, "%s would not make a sampler of that state",
          rwa_device_name(device));
      if (sampler->cso)
         device->pipe->delete_sampler_state(device->pipe, sampler->cso);
      if (sampler->cso_compare)
         device->pipe->delete_sampler_state(device->pipe, sampler->cso_compare);
      free(sampler);
      return NULL;
   }
   return sampler;
}

void
rwa_sampler_destroy(struct rwa_sampler *sampler)
{
   RWA_LOCKED;
   if (!sampler)
      return;
   /* Unbound first: draws leave their samplers bound (state_forget), and a deleted
    * sampler state left in the context is freed memory the next texture emit reads.
    */
   state_forget(sampler->device);
   argbuf_scrub(sampler->device, NULL, NULL, sampler);
   sampler->device->pipe->delete_sampler_state(sampler->device->pipe, sampler->cso);
   sampler->device->pipe->delete_sampler_state(sampler->device->pipe,
                                               sampler->cso_compare);
   free(sampler);
}
