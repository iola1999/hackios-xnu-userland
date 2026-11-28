/* Stage CSO creation, blend tables, and render pipeline lifecycle, included into air_metal.c. */

/* Clone, finalise and compile one stage, handing the driver a copy it may steal. The
 * clone is not freed on failure, for compute_state_for's reason: a driver that refused
 * it may or may not have taken it, and neither driver in this build reaches this (both
 * return a state object and report a bad shader later, at the draw).
 */
static void *
make_stage_cso(struct rwa_device *device, nir_shader *nir, const struct air_shader_info *info,
               const struct rwa_yuv_key *yuv, char *why, size_t why_size)
{
   nir_shader *clone = prepare_stage_nir(device, nir, info, yuv, why, why_size);
   return clone ? create_stage_cso(device, clone, nir->info.stage, why, why_size) : NULL;
}

/* Whether a vertex shader reads vertex attributes, which used to be the one thing the
 * contract could not feed it -- rwa_render_shape carried no formats, strides or offsets,
 * and a [[stage_in]] shader was refused by name rather than drawn over garbage. The
 * layout is in the shape now (docs/metal.md 10.44), so this is no longer a refusal: it
 * is the NIR half of a cross-check against air_shader_info's table, because the elements
 * are built from that table and a disagreement between the two would mean an element
 * array that does not describe the fetches. Apple's own three vertex shaders in the GL
 * renderer read their vertices out of a buffer indexed by the vertex id and so have no
 * such input at all.
 */
/*
 * Whether the fragment shader stores to [[depth]], which decides what a depth
 * attachment ends up holding: the interpolated z, or whatever the shader computed.
 *
 * A declaration is not enough and that is the whole point of walking the stores. Both
 * of Apple's depth shaders declare a colour output and never store to it
 * (texturedFragment2DDepth returns {undef, depth}), so a shader's variables say what it
 * might write and only its stores say what it does -- air_run makes the same walk for
 * the colour side and refuses to read back an attachment nothing stored to.
 */
static bool
is_frag_depth_store_deref(const nir_deref_instr *d)
{
   return d && d->deref_type == nir_deref_type_var &&
          d->var->data.mode == nir_var_shader_out &&
          d->var->data.location == FRAG_RESULT_DEPTH;
}

static bool
fragment_writes_depth(nir_shader *fs)
{
   nir_foreach_function_impl(impl, fs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_deref_instr *d = nir_src_as_deref(in->src[0]);
            if (is_frag_depth_store_deref(d))
               return true;
         }
      }
   }
   return false;
}

/*
 * Remove every store to the [[depth]] output, which is what a pipeline with no depth attachment
 * needs: the value has nowhere to go, and it is the store that reached the GPU and hung it. The
 * output variable stays -- a declared output nothing writes is what Apple's own depth shaders
 * already are on the colour side -- and with no store there is no output write to lower.
 */
static void
drop_frag_depth_stores(nir_shader *fs)
{
   nir_foreach_function_impl(impl, fs) {
      bool dropped = false;
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_deref_instr *d = nir_src_as_deref(in->src[0]);
            if (is_frag_depth_store_deref(d)) {
               nir_instr_remove(instr);
               dropped = true;
            }
         }
      }
      nir_progress(dropped, impl, nir_metadata_none);
   }
}

static bool
vertex_reads_attributes(nir_shader *vs)
{
   nir_foreach_variable_with_modes(var, vs, nir_var_shader_in)
      if (var->data.location >= VERT_ATTRIB_GENERIC0 &&
          var->data.location < VERT_ATTRIB_MAX)
         return true;
   return false;
}

/*
 * ===================== METAL'S BLEND STATE, MAPPED BY TABLE =====================
 *
 * Two enumerations and a bitfield, and not one of the three may be cast.
 *
 * MTLBlendFactor against pipe_blendfactor. Gallium's enumeration is built out of an
 * INVERT BIT: PIPE_BLENDFACTOR_INVERT_BIT is 0x10 and PIPE_BLENDFACTOR_ZERO is literally
 * `INVERT_BIT | ONE` (src/util/blend.h), so the plain factors run 1..10 and their
 * one-minus forms run 0x11..0x1a with two holes. Metal's run 0..18 with every factor next
 * to its own one-minus form. The two therefore agree on EXACTLY TWO of nineteen values:
 *
 *   Metal                      value   a cast lands on              is that right?
 *   MTLBlendFactorZero             0    (no enumerator at all)      undefined
 *   ...One                         1    PIPE_BLENDFACTOR_ONE        YES, by luck
 *   ...SourceColor                 2    PIPE_BLENDFACTOR_SRC_COLOR  YES, by luck
 *   ...OneMinusSourceColor         3    SRC_ALPHA                   no
 *   ...SourceAlpha                 4    DST_ALPHA                   no
 *   ...OneMinusSourceAlpha         5    DST_COLOR                   no  <-- the compositor's
 *   ...DestinationColor            6    SRC_ALPHA_SATURATE          no
 *   ...OneMinusDestinationColor    7    CONST_COLOR                 no
 *   ...DestinationAlpha            8    CONST_ALPHA                 no
 *   ...OneMinusDestinationAlpha    9    SRC1_COLOR                  no
 *   ...SourceAlphaSaturated       10    SRC1_ALPHA                  no
 *   ...BlendColor                 11    (no enumerator)             undefined
 *   ...OneMinusBlendColor         12    (no enumerator)             undefined
 *   ...BlendAlpha                 13    (no enumerator)             undefined
 *   ...OneMinusBlendAlpha         14    (no enumerator)             undefined
 *   ...Source1Color               15    (no enumerator)             undefined
 *   ...OneMinusSource1Color       16    (no enumerator)             undefined
 *   ...Source1Alpha               17    PIPE_BLENDFACTOR_ZERO       no
 *   ...OneMinusSource1Alpha       18    INV_SRC_COLOR               no
 *
 * Two right, ten silently wrong, seven landing on no enumerator. And the one that
 * matters most is the fifth: CoreAnimation's destination factor is OneMinusSourceAlpha,
 * and a cast turns it into DST_COLOR -- the destination multiplied by ITSELF. On the home
 * screen that is a dark wash that deepens wherever two layers overlap, which is a picture
 * and not a failure.
 *
 * MTLBlendOperation against pipe_blend_func: Add 0, Subtract 1, ReverseSubtract 2, Min 3,
 * Max 4 on both sides, so this one IS the identity -- and it is written out for exactly
 * the reason compare_function_from_metal is. A cast would be a bet that two enumerations
 * nobody here controls stay aligned; a table of five lines is a statement that they are,
 * checkable by reading it.
 *
 * MTLColorWriteMask against Gallium's colormask, and this is the one a test is most
 * likely to pass with backwards. Metal's bits are Red = 1 << 3, Green = 1 << 2,
 * Blue = 1 << 1, Alpha = 1 << 0; Gallium's are PIPE_MASK_R 0x1, G 0x2, B 0x4, A 0x8
 * (src/util/format/u_formats.h). The nibble is REVERSED. Four of the sixteen masks are
 * palindromes and survive a cast -- None 0x0, All 0xf, green|blue 0x6, red|alpha 0x9 --
 * and the other twelve exchange red with alpha and green with blue. A pipeline asking to
 * write colour and not alpha (R|G|B, Metal's 0xe) would, cast, write alpha and not red.
 *
 * WHY EVERY REFUSAL BELOW IS WRITTEN HERE AND NOT LEFT TO THE DRIVER UNDERNEATH, which
 * is the same argument the vertex layout's refusals are made of and it was read in
 * freedreno's own source: fd_blend_factor (freedreno_util.c) ends
 *
 *     default:
 *        DBG("invalid blend factor: %x", factor);
 *        return 0;
 *
 * and 0 is FACTOR_ZERO. DBG is gated on a debug flag, so in an ordinary build a factor
 * freedreno does not recognise produces NO diagnostic and blends with zero -- the term
 * vanishes. fd6_blend.cc's blend_func does the same and its 0 is BLEND_DST_PLUS_SRC, so
 * an unrecognised EQUATION silently becomes Add. softpipe's four factor switches end in
 * a bare `assert(0 && "invalid rgb src factor")`, which is nothing at all in a release
 * build. Neither back end will ever tell anyone; this library has to.
 *
 * `dither` is deliberately left at 0, and that is not a default being accepted: GL's
 * GL_DITHER is on by default and Mesa's state tracker sets the bit, a6xx honours it
 * (A6XX_RB_DITHER_CNTL, DITHER_ALWAYS, in fd6_blend.cc) and softpipe ignores it
 * entirely. Metal has no dither control, so there is nothing to carry -- and a bit set
 * here would make the phone's bytes differ from this machine's for no reason anyone
 * asked for.
 *
 * WHERE THESE NUMBERS COME FROM, and it is two sources that do not share a copy:
 *
 *   1. Apple's own MTLRenderPipeline.h for this release, which is on the build machine
 *      and not in this tree. userland/src/annibalemetal.m compiles against it and
 *      _Static_asserts every value this table assumes, so a disagreement is a BUILD
 *      FAILURE of the driver rather than a wrong picture. (The same declarations are
 *      byte for byte identical in every iOS SDK from 10.3 to 16.5 and in macOS 13.3's,
 *      which is six copies agreeing.)
 *   2. Metal.framework's own code on the DEVICE, which carries the numbering as data:
 *      MTLBlendFactorString (0x181b08730) bounds its argument at 18 and indexes a table
 *      of nineteen CFStrings at 0x1d59dbe18, in the order this table has them;
 *      MTLBlendOperation's is five entries at 0x1d59dbeb0; and
 *      MTLColorWriteMaskString (0x181b08754) does not use a table at all -- it tests
 *      bit 3 for "R", bit 2 for "G", bit 1 for "B" and bit 0 for "A", which IS the
 *      reversal, read out of the framework that defines it.
 *
 * Neither source is this library's reading of the other, and the second one is what the
 * phone actually runs. blend_tables_are_shaped_right below is a third kind of check and
 * not a third source: it asserts the shape of each table -- the length, the one-minus
 * pairing, the bit reversal -- at every pipeline creation, so a row edited by hand
 * cannot quietly go missing.
 */

/*
 * THE SHAPE OF THE TWO STRUCTS THE METAL DRIVER TYPES OUT BY HAND, stated here as
 * numbers so that an edit to one side alone fails to build on that side.
 *
 * The same four numbers are asserted in userland/src/annibalemetal.m against ITS
 * definitions. Neither file can see the other's structs, so this is not a comparison --
 * it is two independent statements of one agreement, which is the most a hand-copied
 * struct admits of. This port has paid twice for getting it wrong: a primitive type that
 * arrived as 1871292816, and struct air_resource growing by eight bytes so that every
 * resource after the first was read eight bytes early.
 *
 * The OFFSETS are asserted as well as the sizes, and that is the point: the discipline
 * is that a new field goes on the END, and a field inserted in the middle keeps the size
 * while moving everything after it -- which is the failure that leaves no trace. The
 * blend state took rwa_render_shape from 792 bytes to 1052 (260 bytes at offset 792,
 * nothing above it moved) and rwa_render_info from 108 to 116 (8 bytes at offset 108).
 */
static_assert(sizeof(struct rwa_color_blend) == 32 &&
                 sizeof(struct rwa_render_shape) == 1052 &&
                 offsetof(struct rwa_render_shape, vertex_attribute) == 48 &&
                 offsetof(struct rwa_render_shape, vertex_layout) == 420 &&
                 offsetof(struct rwa_render_shape, color_blend) == 792 &&
                 offsetof(struct rwa_render_shape, color_blend_count) == 1048,
              "struct rwa_render_shape is not the shape annibalemetal.m asserts: a field "
              "was added in the middle, or only one of the two copies was rebuilt");
static_assert(sizeof(struct rwa_render_info) == 116 &&
                 offsetof(struct rwa_render_info, vertex_argbuf_mask) == 100 &&
                 offsetof(struct rwa_render_info, blend_enabled_mask) == 108,
              "struct rwa_render_info is not the shape annibalemetal.m asserts, and this "
              "one the LIBRARY writes into: a driver with a shorter one is overrun");

/* MTLBlendFactor, spelled out so a refusal names what the caller passed rather than
 * printing a number at them. */
static const char *
blend_factor_name(unsigned f)
{
   static const char *const names[] = {
      "MTLBlendFactorZero", "MTLBlendFactorOne",
      "MTLBlendFactorSourceColor", "MTLBlendFactorOneMinusSourceColor",
      "MTLBlendFactorSourceAlpha", "MTLBlendFactorOneMinusSourceAlpha",
      "MTLBlendFactorDestinationColor", "MTLBlendFactorOneMinusDestinationColor",
      "MTLBlendFactorDestinationAlpha", "MTLBlendFactorOneMinusDestinationAlpha",
      "MTLBlendFactorSourceAlphaSaturated",
      "MTLBlendFactorBlendColor", "MTLBlendFactorOneMinusBlendColor",
      "MTLBlendFactorBlendAlpha", "MTLBlendFactorOneMinusBlendAlpha",
      "MTLBlendFactorSource1Color", "MTLBlendFactorOneMinusSource1Color",
      "MTLBlendFactorSource1Alpha", "MTLBlendFactorOneMinusSource1Alpha",
   };
   return f < ARRAY_SIZE(names) ? names[f] : NULL;
}

static const char *
blend_operation_name(unsigned op)
{
   static const char *const names[] = {
      "MTLBlendOperationAdd", "MTLBlendOperationSubtract",
      "MTLBlendOperationReverseSubtract", "MTLBlendOperationMin",
      "MTLBlendOperationMax",
   };
   return op < ARRAY_SIZE(names) ? names[op] : NULL;
}

/* The factors this library cannot serve, and WHY each one -- the reason differs, and
 * "unsupported" would hide which of the two it is. Returns NULL for a factor that is
 * served. */
static const char *
blend_factor_refusal(unsigned f)
{
   (void)f;
   /* Blend colour factors (11..14) and dual-source factors (15..18) are served:
    * freedreno supports dual-source blending on target 0 and constant color blending. */
   return NULL;
}

/*
 * MTLBlendFactor as pipe_blendfactor. The table above says what a cast would have done
 * with each row.
 */
static bool
blend_factor_from_metal(unsigned f, bool alpha_channel, unsigned *out, const char *what,
                        char *why, size_t why_size)
{
   static const unsigned factor[] = {
      PIPE_BLENDFACTOR_ZERO,               /* MTLBlendFactorZero                     0 */
      PIPE_BLENDFACTOR_ONE,                /* ...One                                 1 */
      PIPE_BLENDFACTOR_SRC_COLOR,          /* ...SourceColor                         2 */
      PIPE_BLENDFACTOR_INV_SRC_COLOR,      /* ...OneMinusSourceColor                 3 */
      PIPE_BLENDFACTOR_SRC_ALPHA,          /* ...SourceAlpha                         4 */
      PIPE_BLENDFACTOR_INV_SRC_ALPHA,      /* ...OneMinusSourceAlpha                 5 */
      PIPE_BLENDFACTOR_DST_COLOR,          /* ...DestinationColor                    6 */
      PIPE_BLENDFACTOR_INV_DST_COLOR,      /* ...OneMinusDestinationColor            7 */
      PIPE_BLENDFACTOR_DST_ALPHA,          /* ...DestinationAlpha                    8 */
      PIPE_BLENDFACTOR_INV_DST_ALPHA,      /* ...OneMinusDestinationAlpha            9 */
      PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE, /* ...SourceAlphaSaturated               10 */
      PIPE_BLENDFACTOR_CONST_COLOR,        /* ...BlendColor            11  REFUSED, above */
      PIPE_BLENDFACTOR_INV_CONST_COLOR,    /* ...OneMinusBlendColor    12  REFUSED */
      PIPE_BLENDFACTOR_CONST_ALPHA,        /* ...BlendAlpha            13  REFUSED */
      PIPE_BLENDFACTOR_INV_CONST_ALPHA,    /* ...OneMinusBlendAlpha    14  REFUSED */
      PIPE_BLENDFACTOR_SRC1_COLOR,         /* ...Source1Color          15  REFUSED */
      PIPE_BLENDFACTOR_INV_SRC1_COLOR,     /* ...OneMinusSource1Color  16  REFUSED */
      PIPE_BLENDFACTOR_SRC1_ALPHA,         /* ...Source1Alpha          17  REFUSED */
      PIPE_BLENDFACTOR_INV_SRC1_ALPHA,     /* ...OneMinusSource1Alpha  18  REFUSED */
   };
   if (f >= ARRAY_SIZE(factor)) {
      say(why, why_size, "%s is %u, which is not one of MTLBlendFactor's nineteen values",
          what, f);
      return false;
   }
   const char *no = blend_factor_refusal(f);
   if (no) {
      say(why, why_size, "%s is %s, %s", what, blend_factor_name(f), no);
      return false;
   }
   /*
    * MTLBlendFactorSourceAlphaSaturated AS AN ALPHA FACTOR, and this is a narrow refusal
    * over a narrow disagreement: for the three colour channels the factor is
    * min(As, 1 - Ad) in Metal and in GL alike, and for the ALPHA channel GL defines it
    * as 1. Both back ends implement GL's: softpipe's alpha cases are "multiply alpha by
    * 1.0" and "NO-OP, leave blend_dest as-is" where its RGB cases compute the minimum
    * (sp_quad_blend.c), and freedreno's own a2xx driver says it in a comment --
    * "hardware doesn't support SRC_ALPHA_SATURATE for alpha, but it is equivalent to
    * ONE" (fd2_blend.c), which is the GL reading stated as a property of Adreno
    * hardware; a6xx passes the factor straight through with no fix-up at all
    * (fd_blend_factor in freedreno_util.c, no special case in fd6_blend.cc).
    *
    * Apple's own enumeration carries no such exception and Metal writes one equation for
    * all four channels. So the colour half is served -- the two readings agree on it --
    * and the alpha half is refused by the field's name, because serving it would hand a
    * Metal application GL's answer for a number the NEXT composite reads.
    */
   if (alpha_channel && f == 10 /* MTLBlendFactorSourceAlphaSaturated */) {
      say(why, why_size, "%s is MTLBlendFactorSourceAlphaSaturated, and the two "
                         "specifications disagree about what that means for the ALPHA "
                         "channel: GL (and softpipe, sp_quad_blend.c) define the alpha "
                         "factor as 1 where Metal's own equation gives min(As, 1 - Ad) "
                         "for all four channels. It is served for the RGB factors, where "
                         "they agree. Nothing on this device says which the Adreno's "
                         "blender does", what);
      return false;
   }
   *out = factor[f];
   return true;
}

/*
 * MTLBlendOperation as pipe_blend_func. The identity, written out rather than cast.
 */
static bool
blend_operation_from_metal(unsigned op, unsigned *out, const char *what, char *why,
                           size_t why_size)
{
   static const unsigned func[] = {
      PIPE_BLEND_ADD,              /* MTLBlendOperationAdd             0 */
      PIPE_BLEND_SUBTRACT,         /* ...Subtract                      1 */
      PIPE_BLEND_REVERSE_SUBTRACT, /* ...ReverseSubtract               2 */
      PIPE_BLEND_MIN,              /* ...Min                           3 */
      PIPE_BLEND_MAX,              /* ...Max                           4 */
   };
   if (op >= ARRAY_SIZE(func)) {
      say(why, why_size, "%s is %u, which is not one of MTLBlendOperation's five values",
          what, op);
      return false;
   }
   *out = func[op];
   return true;
}

/*
 * MTLColorWriteMask as Gallium's colormask: FOUR BITS IN THE OTHER ORDER.
 *
 * Written as a table over all sixteen masks and not as four shifts, so that what it does
 * can be read rather than worked out -- and so that the four palindromes (0x0, 0x6, 0x9,
 * 0xf), which are the masks a cast gets right and therefore the masks a careless test
 * would be built out of, are visible as the exceptions they are.
 */
static bool
color_write_mask_from_metal(unsigned m, unsigned *out, const char *what, char *why,
                            size_t why_size)
{
   static const unsigned mask[16] = {
      /* Metal 0x0 None        */ 0,
      /* Metal 0x1 A           */ PIPE_MASK_A,
      /* Metal 0x2 B           */ PIPE_MASK_B,
      /* Metal 0x3 B|A         */ PIPE_MASK_B | PIPE_MASK_A,
      /* Metal 0x4 G           */ PIPE_MASK_G,
      /* Metal 0x5 G|A         */ PIPE_MASK_G | PIPE_MASK_A,
      /* Metal 0x6 G|B         */ PIPE_MASK_G | PIPE_MASK_B,
      /* Metal 0x7 G|B|A       */ PIPE_MASK_G | PIPE_MASK_B | PIPE_MASK_A,
      /* Metal 0x8 R           */ PIPE_MASK_R,
      /* Metal 0x9 R|A         */ PIPE_MASK_R | PIPE_MASK_A,
      /* Metal 0xa R|B         */ PIPE_MASK_R | PIPE_MASK_B,
      /* Metal 0xb R|B|A       */ PIPE_MASK_R | PIPE_MASK_B | PIPE_MASK_A,
      /* Metal 0xc R|G         */ PIPE_MASK_R | PIPE_MASK_G,
      /* Metal 0xd R|G|A       */ PIPE_MASK_R | PIPE_MASK_G | PIPE_MASK_A,
      /* Metal 0xe R|G|B       */ PIPE_MASK_R | PIPE_MASK_G | PIPE_MASK_B,
      /* Metal 0xf All         */ PIPE_MASK_RGBA,
   };
   if (m > 0xf) {
      say(why, why_size, "%s is 0x%x, and MTLColorWriteMask is four bits "
                         "(MTLColorWriteMaskAll is 0xf)", what, m);
      return false;
   }
   *out = mask[m];
   return true;
}

/*
 * The three tables' own shape, asserted at run time rather than trusted.
 *
 * Every claim here is about THIS file and is decidable here, which is what makes it
 * worth the cycles: the factor table has one row per Metal value, each one-minus row is
 * its plain row with Gallium's invert bit set, the operation table is the identity, and
 * the mask table is the bit-reversal of its index. A row deleted, duplicated or
 * transposed by hand breaks one of those, and the library then refuses to open a device
 * at all instead of drawing with a table nobody checked.
 *
 * It does NOT check that Apple's numbering is what the left column says. Nothing on this
 * machine can: see the comment at the top of this section for how that half is
 * corroborated.
 */
static bool
blend_tables_are_shaped_right(char *why, size_t why_size)
{
   unsigned plain, inverted, got;
   char ignored[8];

   /* nineteen factors, and the one-minus of each plain factor is the plain one with
    * Gallium's invert bit set. Metal pairs them adjacently from One onwards: 1/0 is
    * One/Zero (Zero IS the one-minus of One in Gallium's construction), then 2/3, 4/5,
    * 6/7, 8/9.
    */
   static const unsigned pair[][2] = { { 1, 0 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 8, 9 } };
   for (unsigned i = 0; i < ARRAY_SIZE(pair); i++) {
      if (!blend_factor_from_metal(pair[i][0], false, &plain, "", ignored,
                                   sizeof(ignored)) ||
          !blend_factor_from_metal(pair[i][1], false, &inverted, "", ignored,
                                   sizeof(ignored)) ||
          (plain | PIPE_BLENDFACTOR_INVERT_BIT) != inverted ||
          util_blendfactor_is_inverted((enum pipe_blendfactor)plain) ||
          !util_blendfactor_is_inverted((enum pipe_blendfactor)inverted)) {
         say(why, why_size, "the blend factor table is wrong: MTLBlendFactor %u and %u "
                            "are not a factor and its one-minus form", pair[i][0],
             pair[i][1]);
         return false;
      }
   }
   if (blend_factor_from_metal(19, false, &plain, "", ignored, sizeof(ignored)) ||
       !blend_factor_name(18) || blend_factor_name(19)) {
      say(why, why_size, "the blend factor table is not nineteen values wide");
      return false;
   }
   /* the five operations, the identity */
   for (unsigned op = 0; op < 5; op++) {
      if (!blend_operation_from_metal(op, &got, "", ignored, sizeof(ignored)) ||
          got != op || !blend_operation_name(op)) {
         say(why, why_size, "the blend operation table is wrong at MTLBlendOperation %u",
             op);
         return false;
      }
   }
   if (blend_operation_from_metal(5, &got, "", ignored, sizeof(ignored)) ||
       blend_operation_name(5)) {
      say(why, why_size, "the blend operation table is not five values wide");
      return false;
   }
   /* and the write mask: the reversal of the nibble, every one of the sixteen */
   for (unsigned m = 0; m <= 0xf; m++) {
      unsigned want = ((m & 0x8) ? PIPE_MASK_R : 0) | ((m & 0x4) ? PIPE_MASK_G : 0) |
                      ((m & 0x2) ? PIPE_MASK_B : 0) | ((m & 0x1) ? PIPE_MASK_A : 0);
      if (!color_write_mask_from_metal(m, &got, "", ignored, sizeof(ignored)) ||
          got != want) {
         say(why, why_size, "the colour write mask table is wrong at MTLColorWriteMask "
                            "0x%x: it gives 0x%x where the reversal of the nibble is "
                            "0x%x", m, got, want);
         return false;
      }
   }
   if (color_write_mask_from_metal(0x10, &got, "", ignored, sizeof(ignored))) {
      say(why, why_size, "the colour write mask table accepts a fifth bit");
      return false;
   }
   return true;
}

/* Whether two mapped attachments ask for the same thing, which is what decides whether
 * independent blending has to be turned on (and therefore whether a screen without it
 * can serve this pipeline at all). */
static bool
rt_blend_equal(const struct pipe_rt_blend_state *a, const struct pipe_rt_blend_state *b)
{
   return a->blend_enable == b->blend_enable && a->rgb_func == b->rgb_func &&
          a->rgb_src_factor == b->rgb_src_factor &&
          a->rgb_dst_factor == b->rgb_dst_factor && a->alpha_func == b->alpha_func &&
          a->alpha_src_factor == b->alpha_src_factor &&
          a->alpha_dst_factor == b->alpha_dst_factor && a->colormask == b->colormask;
}

/*
 * THE WHOLE BLEND STATE, from the shape's `color_blend[]` to one pipe_blend_state.
 *
 * `color_blend_count` of 0 is a shape that carries no blend state, and it produces
 * EXACTLY what this library made before these fields existed:
 * `{ .rt[0].colormask = PIPE_MASK_RGBA }`. Every draw of the corpus, every pixel-compared
 * fragment shader and every one of air_metal_test's own designated initialisers is that
 * case, so the proven path is not merely preserved, it is the same struct.
 */
#include "air_metal_blend.inc"

struct rwa_render *
rwa_render_create(struct rwa_device *device,
                  const void *vertex_bitcode, size_t vertex_size,
                  const char *vertex_entry,
                  const void *fragment_bitcode, size_t fragment_size,
                  const char *fragment_entry,
                  const struct rwa_render_shape *shape,
                  struct rwa_render_info *info, char *why, size_t why_size)
{
   return rwa_render_create_linked(device, vertex_bitcode, vertex_size, vertex_entry,
                                   fragment_bitcode, fragment_size, fragment_entry, NULL, 0,
                                   shape, info, why, why_size);
}

/* The library's and the frontend's descriptions of a linked function are one layout, so
 * the caller's array goes to air_to_nir as it is. */
_Static_assert(sizeof(struct rwa_stitch_node) == sizeof(struct air_stitch_node) &&
              offsetof(struct rwa_stitch_node, args) == offsetof(struct air_stitch_node, args) &&
              offsetof(struct rwa_stitch_node, after) == offsetof(struct air_stitch_node, after) &&
              sizeof(struct rwa_linked_function) == sizeof(struct air_linked_function) &&
              offsetof(struct rwa_linked_function, output) ==
                 offsetof(struct air_linked_function, output),
              "rwa_linked_function and air_linked_function differ");

static bool
render_create_gallium_states(struct rwa_device *device, struct rwa_render *render,
                             nir_shader *fs_prep, nir_shader *vs_prep,
                             const void *vertex_bitcode, size_t vertex_size,
                             const char *vertex_entry, unsigned fetched,
                             const struct pipe_blend_state *blend,
                             unsigned zs_format, unsigned samples,
                             char *why, size_t why_size)
{
   render->fs_cso = create_stage_cso(device, fs_prep, MESA_SHADER_FRAGMENT, why, why_size);
   if (!render->fs_cso) {
      ralloc_free(vs_prep);
      return false;
   }
   if (!render->vs_cso) {
      render->vs_cso = create_stage_cso(device, vs_prep, MESA_SHADER_VERTEX, why, why_size);
      if (!render->vs_cso)
         return false;
      if (!render->generated_vertex && vertex_bitcode) {
         render->cached_vcso_entry = rwa_vertex_cso_insert(device, vertex_bitcode, vertex_size,
                                                           vertex_entry, render->vs_cso,
                                                           &render->vinfo, render->vert_slots,
                                                           render->vert_compare_sampler, fetched);
      }
   }

   render->blend = device->pipe->create_blend_state(device->pipe, blend);
   render->dsa = device->pipe->create_depth_stencil_alpha_state(device->pipe,
      &(struct pipe_depth_stencil_alpha_state){ 0 });
   render->rast = device->pipe->create_rasterizer_state(device->pipe,
      &(struct pipe_rasterizer_state){
         .cull_face = PIPE_FACE_NONE, .half_pixel_center = 1, .bottom_edge_rule = 1,
         .depth_clip_near = 1, .depth_clip_far = 1, .clip_halfz = zs_format != 0,
         .clip_plane_enable = BITFIELD_MASK(render->vinfo.clip_distances),
         .multisample = samples > 1, .line_width = 1.0f });
   render->rast_scissor = device->pipe->create_rasterizer_state(device->pipe,
      &(struct pipe_rasterizer_state){
         .cull_face = PIPE_FACE_NONE, .half_pixel_center = 1, .bottom_edge_rule = 1,
         .depth_clip_near = 1, .depth_clip_far = 1, .clip_halfz = zs_format != 0,
         .clip_plane_enable = BITFIELD_MASK(render->vinfo.clip_distances),
         .multisample = samples > 1, .line_width = 1.0f, .scissor = 1 });
   if (!render->blend || !render->dsa || !render->rast || !render->rast_scissor) {
      say(why, why_size, "%s would not make the fixed render state", rwa_device_name(device));
      return false;
   }

   render->velem = render->vstate.count
      ? device->pipe->create_vertex_elements_state(device->pipe, render->vstate.count,
                                                   render->vstate.element)
      : device->pipe->create_vertex_elements_state(device->pipe, 1,
           &(struct pipe_vertex_element){
              .src_offset = 0, .src_stride = 16, .vertex_buffer_index = 0,
              .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT });
   render->vbuf = rwa_quad_vbuf_ref(device, rwa_quad_verts, sizeof(rwa_quad_verts));
   if (!render->velem || !render->vbuf) {
      say(why, why_size, "%s would not make the quad a draw covers the attachment with",
          rwa_device_name(device));
      return false;
   }
   return true;
}

struct rwa_render *
rwa_render_create_linked(struct rwa_device *device,
                         const void *vertex_bitcode, size_t vertex_size,
                         const char *vertex_entry,
                         const void *fragment_bitcode, size_t fragment_size,
                         const char *fragment_entry,
                         const struct rwa_linked_function *fragment_linked,
                         unsigned num_fragment_linked,
                         const struct rwa_render_shape *shape,
                         struct rwa_render_info *info, char *why, size_t why_size)
{
   if (!shape) {
      say(why, why_size, "a render pipeline with no attachment description");
      return NULL;
   }
   if (fragment_bitcode && fragment_size < 4) {
      say(why, why_size, "%zu bytes is not a fragment shader module", fragment_size);
      return NULL;
   }
   if (!fragment_bitcode && !vertex_bitcode) {
      say(why, why_size, "a render pipeline with neither a vertex nor a fragment function");
      return NULL;
   }
   /* No colour attachment is allowed only with a depth one, which is Metal's own rule
    * (a depth-only pass is a shadow map) and is the one case where a draw that writes
    * no colour still produces something a caller can read. Without either there is
    * nothing for the fragment shader to write at all, and the draw would "succeed"
    * having left no mark.
    */
   if (shape->color_count == 0 && shape->depth_format == 0 && shape->stencil_format == 0) {
      say(why, why_size, "a render pipeline with no colour attachments and no depth "
                         "attachment: there would be nothing for the fragment shader to "
                         "write");
      return NULL;
   }
   if (shape->color_count > PIPE_MAX_COLOR_BUFS) {
      say(why, why_size, "%u colour attachments, more than the %u this GPU can bind",
          shape->color_count, (unsigned)PIPE_MAX_COLOR_BUFS);
      return NULL;
   }
   /*
    * The pass's sample count. Metal's own MTLRenderPipelineDescriptor.sampleCount
    * defaults to 1, and 0 here is a caller's struct filled in only as far as it cared
    * about, so both mean one sample per pixel.
    *
    * Above one, every attachment of the pass is a multisample texture of exactly this
    * count. That is Metal's rule (the pipeline's sampleCount must be the attachments')
    * and Gallium's too, in a way worth knowing: a framebuffer's sample count is read
    * back off the attachment's own resource and NOT from pipe_framebuffer_state.samples,
    * which util_framebuffer_get_num_samples looks at only when there is no attachment
    * at all. So the agreement is checked here and at the draw rather than being carried
    * in the framebuffer state.
    *
    * What a multisample pass is FOR here is the only thing that fills a multisample
    * texture, in Metal or on this GPU: rendering into one. Nothing writes one from the
    * processor (rwa_texture_write says why at length), so a pass like this is how the
    * six multisample fragment shaders of AppleMetalGLRenderer get something defined to
    * read.
    */
   unsigned samples = shape->sample_count ? shape->sample_count : 1;
   /*
    * The depth/stencil attachment's format. Metal's pipeline descriptor has two
    * properties and one texture behind them (air_metal.h), so the two have to name the
    * same format: equal, or one of them zero. Two different formats is a request for a
    * separate depth surface and stencil surface, which neither Metal's own render pass
    * descriptor nor a Gallium framebuffer can hold, and honouring one of the two would
    * silently drop the other.
    */
   unsigned zs_format = shape->depth_format ? shape->depth_format : shape->stencil_format;
   if (shape->depth_format && shape->stencil_format &&
       shape->depth_format != shape->stencil_format) {
      say(why, why_size, "a depth attachment of pixel format %u and a stencil attachment "
                         "of %u: one texture carries both aspects, so this library renders "
                         "into one attachment and cannot be given two",
          shape->depth_format, shape->stencil_format);
      return NULL;
   }
   if (zs_format) {
      const struct rwa_pixel_format *fmt = pixel_format(zs_format);
      if (!fmt) {
         say(why, why_size, "the depth/stencil attachment: pixel format %u is %s", zs_format,
             pixel_format_refusal(zs_format));
         return NULL;
      }
      if (!format_is_depth_stencil(fmt->pipe)) {
         say(why, why_size, "%s as the depth/stencil attachment: it is a colour format, and "
                            "a pass tests depth against a depth format", fmt->name);
         return NULL;
      }
      /* Which aspect the format actually has, against which the pipeline asked for.
       * Metal lets an application set stencilAttachmentPixelFormat to a depth-only
       * format and get no stencil; here that is said rather than discovered at the
       * draw, because a stencil test against a buffer with no stencil aspect passes
       * every fragment and looks like a stencil test that works.
       */
      if (shape->stencil_format && !format_has_stencil(fmt->pipe)) {
         say(why, why_size, "%s as the stencil attachment: that format has a depth aspect "
                            "and no stencil one, so there would be nothing to test",
             fmt->name);
         return NULL;
      }
      if (shape->depth_format && !format_has_depth(fmt->pipe)) {
         say(why, why_size, "%s as the depth attachment: that format has a stencil aspect "
                            "and no depth one, so there would be nothing to test",
             fmt->name);
         return NULL;
      }
      unsigned q = samples > 1 ? samples : 0;
      bool supported;
      if (device->software) {
         RWA_LOCKED;
         supported = device->screen->is_format_supported(device->screen, fmt->pipe, PIPE_TEXTURE_2D, q, q, PIPE_BIND_DEPTH_STENCIL);
      } else {
         supported = device->screen->is_format_supported(device->screen, fmt->pipe, PIPE_TEXTURE_2D, q, q, PIPE_BIND_DEPTH_STENCIL);
      }
      if (!supported) {
         char ms[48] = "";
         if (q)
            snprintf(ms, sizeof(ms), " of %u samples per pixel", q);
         say(why, why_size, "%s cannot use %s%s as a depth/stencil attachment",
             rwa_device_name(device), fmt->name, ms);
         return NULL;
      }
   }

   /* Each colour format known and renderable, checked here so a bad one is a sentence
    * naming it rather than a failure deep in resource or surface creation. The sample
    * count asked is zero and not one for a single-sample pass, for the reason
    * rwa_texture_create spells out: a non-multisample attachment is zero samples to
    * Gallium and freedreno refuses some bindings asked with one. A multisample pass
    * asks with its own count, and that is where softpipe says no -- which is the whole
    * of why the measurement below this is the Adreno's and not this machine's.
    */
   for (unsigned i = 0; i < shape->color_count; i++) {
      if (shape->color_format[i] == 0)
         continue;
      const struct rwa_pixel_format *fmt = pixel_format(shape->color_format[i]);
      if (!fmt) {
         say(why, why_size, "colour attachment %u: pixel format %u is %s", i,
             shape->color_format[i], pixel_format_refusal(shape->color_format[i]));
         return NULL;
      }
      /* Named here rather than left to is_format_supported, which would decline it with
       * a sentence about the GPU: a depth format in a colour slot is a descriptor that
       * put the attachment in the wrong place, and that is what the caller needs told.
       */
      if (format_is_depth_stencil(fmt->pipe)) {
         say(why, why_size, "colour attachment %u is %s, a depth or stencil format: it "
                            "belongs in the depth attachment of the shape and not among "
                            "the colours", i, fmt->name);
         return NULL;
      }
      unsigned q = samples > 1 ? samples : 0;
      bool supported;
      if (device->software) {
         RWA_LOCKED;
         supported = device->screen->is_format_supported(device->screen, fmt->pipe, PIPE_TEXTURE_2D, q, q, PIPE_BIND_RENDER_TARGET);
      } else {
         supported = device->screen->is_format_supported(device->screen, fmt->pipe, PIPE_TEXTURE_2D, q, q, PIPE_BIND_RENDER_TARGET);
      }
      if (!supported) {
         char ms[48] = "";
         if (q)
            snprintf(ms, sizeof(ms), " of %u samples per pixel", q);
         say(why, why_size, "%s cannot render into %s%s", rwa_device_name(device),
             fmt->name, ms);
         return NULL;
      }
   }

   struct rwa_render *render = calloc(1, sizeof(*render));
   if (!render) {
      say(why, why_size, "out of memory making a render pipeline");
      return NULL;
   }
   render->device = device;
   render->mem_ctx = ralloc_context(NULL);
   render->shape = *shape;

   struct rwa_render_info out = { 0 };

   /* The fragment shader first. Its AIR -> NIR errors surface here, at creation, which
    * is the half of docs/metal.md 10.18 a driver must put in the right NSError: a link
    * error between the stages cannot surface until the draw, because ir3 compiles and
    * links at draw time (ir3_cache_lookup), so the two belong to two different calls.
    */
   const char *error;
   nir_shader *fs;
   if (!fragment_bitcode) {
      /* Metal's pipeline with no fragment function rasterises for depth and stencil
       * alone and leaves every colour attachment as it was; RenderBox's path_edges
       * pipelines are this. The stage is an empty shader, and the blend state below
       * masks every colour channel.
       */
      fs = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, device->nir_options,
                                          "no_fragment_function").shader;
      ralloc_steal(render->mem_ctx, fs);
      render->finfo.stage = MESA_SHADER_FRAGMENT;
   } else {
      struct air_constant_value *ffc;
      unsigned num_ffc =
         air_module_function_constants(render->mem_ctx, fragment_bitcode, fragment_size, &ffc);
      const struct air_to_nir_options fopts = {
         .constants = ffc, .num_constants = num_ffc,
         .linked = (const struct air_linked_function *)fragment_linked,
         .num_linked = num_fragment_linked,
      };
      fs = rwa_air_to_nir_cached(render->mem_ctx, fragment_bitcode, fragment_size,
                                 fragment_entry, &fopts, device->nir_options,
                                 device->software, &render->finfo, &error);
      if (!fs) {
         /* The stage is named because the sentence is all a boot log carries: air_to_nir's
          * own error says which construct it declined and where, and nothing in it says
          * which of the two modules that was. A driver reading this back cannot dump the
          * module that failed without the stage in the sentence.
          */
         say(why, why_size, "the fragment module: %s",
             error ? error : "could not be translated");
         goto fail;
      }
      if (render->finfo.stage != MESA_SHADER_FRAGMENT) {
         say(why, why_size, "%s is a %s shader, and this makes render pipelines with a "
                            "fragment stage",
             render->finfo.entry_point ? render->finfo.entry_point : "the function",
             render->finfo.stage >= MESA_SHADER_VERTEX &&
             render->finfo.stage < MESA_SHADER_STAGES
                ? _mesa_shader_stage_to_string(render->finfo.stage) : "non-fragment");
         goto fail;
      }
   }
   slot_shapes(fs, render->frag_slots, render->frag_compare_sampler);
   count_resources(&render->finfo, render->frag_slots, &out.fragment_buffers,
                   &out.fragment_textures, &out.fragment_samplers);
   out.fragment_writes_depth = fragment_writes_depth(fs) ? 1 : 0;

   /* A shader that stores to [[depth]] with no depth attachment to store it in.
    *
    * Letting the store reach the GPU is measured fatal: texturedFragment2DDepth is one of
    * Apple's own, it stores a depth and no colour, and the draw sweep built it a pipeline with
    * three colour attachments and no depth one -- the dispatch never came back and took the whole
    * boot with it (logs/082415-run.log, where the sweep's own "about to draw" line is the last
    * thing in it). Metal's rule agrees, that a fragment function writing depth needs a
    * depthAttachmentPixelFormat. Refusing the pipeline is what that rule says, but
    * CoreAnimation builds one anyway and exits with OS_REASON_COREANIMATION when the answer is
    * no, which cost backboardd fifteen respawns in one 300 s run on 2026-10-27. So the store is
    * dropped and the rest of the shader compiles: what is left is what this pass has an
    * attachment for.
    */
   if (out.fragment_writes_depth && !shape->depth_format) {
      drop_frag_depth_stores(fs);
      out.fragment_writes_depth = 0;
   }

   /* The vertex stage: translated when a module was supplied, generated otherwise. The
    * generated stage is the road with evidence behind it -- 255 fragment shaders drawn
    * over a hand-built quad (docs/air.md) -- and the only one this library has run end
    * to end. A supplied module is translated and bound, but a vertex shader that reads
    * vertex attributes is refused, because the contract carries no layout to feed them.
    */
   nir_shader *vs = NULL;
   unsigned fetched = 0;
   if (vertex_bitcode) {
      if (vertex_size < 4) {
         say(why, why_size, "%zu bytes is not a vertex shader module", vertex_size);
         goto fail;
      }
      struct rwa_vertex_cso_entry *vcso_cached =
         rwa_vertex_cso_lookup(device, vertex_bitcode, vertex_size, vertex_entry);
      if (vcso_cached) {
         render->cached_vcso_entry = vcso_cached;
         render->vs_cso = vcso_cached->vs_cso;
         render->vinfo = vcso_cached->vinfo;
         memcpy(render->vert_slots, vcso_cached->vert_slots, sizeof(render->vert_slots));
         memcpy(render->vert_compare_sampler, vcso_cached->vert_compare_sampler,
                sizeof(render->vert_compare_sampler));
         fetched = vcso_cached->fetched_count;
      }
      if (!vcso_cached) {
         struct air_constant_value *vfc;
         unsigned num_vfc =
            air_module_function_constants(render->mem_ctx, vertex_bitcode, vertex_size, &vfc);
         const struct air_to_nir_options vopts = { .constants = vfc, .num_constants = num_vfc };
         vs = rwa_air_to_nir_cached(render->mem_ctx, vertex_bitcode, vertex_size, vertex_entry, &vopts,
                                    device->nir_options, device->software, &render->vinfo, &error);
         if (!vs) {
            say(why, why_size, "the vertex module: %s",
                error ? error : "could not be translated");
            goto fail;
         }
         if (render->vinfo.stage != MESA_SHADER_VERTEX) {
            say(why, why_size, "%s is a %s shader, and the vertex stage of a render pipeline "
                               "takes a vertex function",
                render->vinfo.entry_point ? render->vinfo.entry_point : "the function",
                render->vinfo.stage >= MESA_SHADER_VERTEX &&
                render->vinfo.stage < MESA_SHADER_STAGES
                   ? _mesa_shader_stage_to_string(render->vinfo.stage) : "non-vertex");
            goto fail;
         }
         const char *vn = render->vinfo.entry_point ? render->vinfo.entry_point
                                                    : "the vertex shader";
         for (unsigned i = 0; i < render->vinfo.num_vertex_attributes; i++)
            fetched += render->vinfo.vertex_attributes[i].used ? 1 : 0;
         if (vertex_reads_attributes(vs) != (fetched != 0)) {
            say(why, why_size, "%s: the translated shader %s vertex attribute inputs and its "
                               "interface lists %u fetched attribute%s, and the elements are "
                               "built from the interface", vn,
                vertex_reads_attributes(vs) ? "has" : "has no", fetched,
                fetched == 1 ? "" : "s");
            goto fail;
         }
         slot_shapes(vs, render->vert_slots, render->vert_compare_sampler);
      }
      unsigned vtex = 0, vsamp = 0;
      count_resources(&render->vinfo, render->vert_slots, &out.vertex_buffers, &vtex, &vsamp);
      render->generated_vertex = false;
      out.generated_vertex_stage = 0;
   } else {
      /* A vertex descriptor with no vertex function is refused rather than ignored. The
       * generated stage draws this library's own quad out of this library's own buffer
       * -- that is the whole of what it is for -- so a layout for it would describe
       * memory the caller does not own, and honouring it would replace the one vertex
       * path that has 255 pixel-compared draws behind it with an untested one. In Metal
       * a pipeline with a vertexDescriptor and no vertexFunction is not a pipeline at
       * all; the generated stage is this library's own extension and keeps its own
       * vertices.
       */
      for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES; i++) {
         if (shape->vertex_attribute[i].format == 0)
            continue;
         say(why, why_size, "a vertex descriptor configures attribute %u and this "
                            "descriptor names no vertex function: the stage this library "
                            "would generate draws its own full-screen quad out of its own "
                            "buffer, and has no attribute to lay out", i);
         goto fail;
      }
      /* num_varyings of the fragment shader: one coordinate out of the generated stage
       * for each interpolated input the fragment shader reads, so the two stages link.
       */
      vs = quad_vertex_shader(device->nir_options, render->finfo.num_varyings);
      if (!vs) {
         say(why, why_size, "out of memory building the vertex stage");
         goto fail;
      }
      render->generated_vertex = true;
      out.vertex_buffers = 0;
      out.generated_vertex_stage = 1;
   }

   /* Prepare NIR lowering lock-free before acquiring rwa_lock for Gallium CSO creation. */
   if (!lower_rgb2p_outputs(fs, shape, why, why_size)) {
      if (render->generated_vertex) ralloc_free(vs);
      goto fail;
   }
   render->fragment_nir = fs;
   nir_shader *fs_prep = prepare_stage_nir(device, fs, &render->finfo, NULL, why, why_size);
   nir_shader *vs_prep = fs_prep && !render->vs_cso
      ? prepare_stage_nir(device, vs, &render->vinfo, NULL, why, why_size) : NULL;
   if (render->generated_vertex) ralloc_free(vs);
   if (!fs_prep || (!render->vs_cso && !vs_prep)) {
      ralloc_free(fs_prep); ralloc_free(vs_prep);
      goto fail;
   }
   /* CPU reflection/blend validation stays lock-free; hardware CSO creation takes cso_create_lock
    * instead of rwa_lock so background prewarm cannot block rwa_render_draw or dealloc. */
   if (fetched) {
      const char *vn = render->vinfo.entry_point ? render->vinfo.entry_point : "the vertex shader";
      if (!build_vertex_state(&render->vinfo, shape, vn, device->screen, &render->vstate, why, why_size)) {
         ralloc_free(fs_prep); ralloc_free(vs_prep);
         goto fail;
      }
   }
   if (!blend_tables_are_shaped_right(why, why_size)) {
      ralloc_free(fs_prep); ralloc_free(vs_prep);
      goto fail;
   }
   struct pipe_blend_state blend;
   if (!blend_state_from_metal(shape, device->screen, &blend, &out.blend_enabled_mask,
                               &out.color_write_masks, why, why_size)) {
      ralloc_free(fs_prep); ralloc_free(vs_prep);
      goto fail;
   }
   if (!fragment_bitcode) {
      blend.independent_blend_enable = true;
      for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
         blend.rt[i].blend_enable = 0;
         blend.rt[i].colormask = 0;
      }
      out.blend_enabled_mask = 0;
      out.color_write_masks = 0;
   }

   out.vertex_attribute_count = render->vstate.count;
   for (unsigned i = 0; i < render->vstate.count &&
                        i < ARRAY_SIZE(out.vertex_attribute_index); i++)
      out.vertex_attribute_index[i] = render->vstate.attribute[i];
   out.vertex_streams = render->vstate.stream_count;
   out.vertex_stream_mask = render->vstate.stream_mask;

   const char *fn = render->finfo.entry_point ? render->finfo.entry_point
                                             : "the fragment function";
   const char *vn = render->vinfo.entry_point ? render->vinfo.entry_point
                                              : "the vertex function";
   if (argbuf_mask_of(&render->finfo, fn, &out.fragment_argbuf_mask, why, why_size) != 0 ||
       (!render->generated_vertex &&
        argbuf_mask_of(&render->vinfo, vn, &out.vertex_argbuf_mask, why, why_size) != 0)) {
      ralloc_free(fs_prep); ralloc_free(vs_prep);
      goto fail;
   }
   if (out.vertex_argbuf_mask & out.vertex_stream_mask) {
      unsigned both = out.vertex_argbuf_mask & out.vertex_stream_mask;

      say(why, why_size, "the vertex descriptor fetches attributes from buffer index %u "
                         "and %s declares an argument buffer there: Metal has one table "
                         "behind -setVertexBuffer:offset:atIndex: and an index is either "
                         "a stream or a set", ffs(both) - 1, vn);
      ralloc_free(fs_prep); ralloc_free(vs_prep);
      goto fail;
   }

   bool cso_ok;
   if (device->software) {
      RWA_LOCKED;
      cso_ok = render_create_gallium_states(device, render, fs_prep, vs_prep,
                                            vertex_bitcode, vertex_size, vertex_entry,
                                            fetched, &blend, zs_format, samples,
                                            why, why_size);
   } else {
      RWA_CSO_CREATE_LOCKED;
      cso_ok = render_create_gallium_states(device, render, fs_prep, vs_prep,
                                            vertex_bitcode, vertex_size, vertex_entry,
                                            fetched, &blend, zs_format, samples,
                                            why, why_size);
   }
   if (!cso_ok)
      goto fail;

   if (info)
      *info = out;
   return render;

fail:
   rwa_render_destroy(render);
   return NULL;
}

void
rwa_render_destroy(struct rwa_render *render)
{
   RWA_LOCKED;
   if (!render)
      return;
   struct pipe_context *pipe = render->device->pipe;

   state_forget(render->device);
   /* Unbound before deleted, each of them, for the reason delete_compute_state and
    * rwa_sampler_destroy give: a Gallium context holds a pointer to the state bound in
    * it, and softpipe's delete is a plain FREE, so a state deleted while bound leaves
    * the context holding freed memory until something is bound over it.
    */
   if (render->vs_cso) {
      pipe->bind_vs_state(pipe, NULL);
      rwa_vertex_cso_release(render->device, render);
   }
   if (render->yuv_fs_cso) {
      pipe->bind_fs_state(pipe, NULL);
      pipe->delete_fs_state(pipe, render->yuv_fs_cso);
   }
   if (render->fs_cso) {
      pipe->bind_fs_state(pipe, NULL);
      pipe->delete_fs_state(pipe, render->fs_cso);
   }
   if (render->blend) {
      pipe->bind_blend_state(pipe, NULL);
      pipe->delete_blend_state(pipe, render->blend);
   }
   if (render->dsa) {
      pipe->bind_depth_stencil_alpha_state(pipe, NULL);
      pipe->delete_depth_stencil_alpha_state(pipe, render->dsa);
   }
   if (render->rast_scissor) {
      pipe->delete_rasterizer_state(pipe, render->rast_scissor);
   }
   if (render->rast) {
      pipe->bind_rasterizer_state(pipe, NULL);
      pipe->delete_rasterizer_state(pipe, render->rast);
   }
   if (render->velem) {
      /* Unbound before deleted, and the vertex buffer unbound with it: the context holds
       * a reference of its own to whatever is bound there (set_vertex_buffers took it),
       * and a draw does not clear it afterwards, so this is where it goes.
       */
      pipe->bind_vertex_elements_state(pipe, NULL);
      pipe->set_vertex_buffers(pipe, 0, NULL);
      pipe->delete_vertex_elements_state(pipe, render->velem);
   }
   if (render->vbuf)
      pipe_resource_reference(&render->vbuf, NULL);

   for (unsigned i = 0; i < RWA_MAX_TEXTURES; i++) {
      if (render->frag_own_sampler[i])
         pipe->delete_sampler_state(pipe, render->frag_own_sampler[i]);
      if (render->vert_own_sampler[i])
         pipe->delete_sampler_state(pipe, render->vert_own_sampler[i]);
   }

   ralloc_free(render->mem_ctx);
   free(render);
}

int
rwa_render_get_reflection(struct rwa_render *render,
                          struct rwa_render_reflection *out,
                          char *why, size_t why_size)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!render || !out) {
      say(why, why_size, "render reflection requires a pipeline and an output pointer");
      return -1;
   }
   if (!render->generated_vertex) {
      if (!render->vinfo.reflection) {
         say(why, why_size, "%s", render->vinfo.reflection_error ?
                                  render->vinfo.reflection_error :
                                  "vertex shader reflection is unavailable");
         return -1;
      }
      out->vertex = render->vinfo.reflection;
   }
   if (render->finfo.entry_point) {
      if (!render->finfo.reflection) {
         say(why, why_size, "%s", render->finfo.reflection_error ?
                                  render->finfo.reflection_error :
                                  "fragment shader reflection is unavailable");
         return -1;
      }
      out->fragment = render->finfo.reflection;
   }
   return 0;
}
