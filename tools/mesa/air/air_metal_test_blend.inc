/* Pixel format, blend, and framebuffer fetch tests, included into air_metal_test.c. */


/*
 * Metal's four packed 16-bit colour formats, measured rather than read: which FIELD of
 * the 16-bit word becomes which CHANNEL.
 *
 * The window server asks for one of them -- MTLPixelFormatBGR5A1Unorm, 43 -- twice a
 * boot for the home screen, and the library declined all four until now because the
 * question below had no answer that was not a guess.
 *
 * THE QUESTION IS ONE BIT OF INFORMATION. Gallium's side is mechanical: on a
 * little-endian host the first channel of a packed name sits at the LEAST significant
 * bits, which the generated unpackers in u_format_table.c state in so many words.
 * Metal's side is whether MTLPixelFormat lists a packed format's channels from the
 * least significant end or the most -- and Gallium happens to carry BOTH spellings of
 * each of these four, so the two readings are both expressible and they EXCHANGE RED
 * AND BLUE. air_metal.c's format table says which reading was taken and why.
 *
 * WHAT THIS MEASURES AND WHAT IT CANNOT. It writes a word with exactly one field set,
 * draws a shader that samples it, and reads which channel came back as 1. That decides
 * everything on this side of the contract: whether the pipe_format this library chose
 * really has the fields the reading claims, and whether the driver's texture unit reads
 * them as it says. It CANNOT decide Apple's convention, because nothing on this machine
 * states the bit positions -- Apple's MTLPixelFormat.h carries no comment on them and
 * Metal.framework's binary carries the names and the values and no layout. So a clean
 * run here means "the reading is implemented exactly", and the reading itself is
 * settled by a boot: red and blue exchanged in the window server's own surfaces is a
 * blue clock on an orange ground, not a subtle shift.
 *
 * Every texel of the probe texture holds the same word, so no coordinate, no filter and
 * no addressing can change the answer. A unorm field of all ones is exactly 1.0 and a
 * field of zeros exactly 0.0, so the expected pixel has no rounding in it.
 *
 *   air_metal_test --pixel-formats FILE.metallib FUNCTION
 */
static int
run_pixel_formats(const char *path, const char *func)
{
   struct packed16 {
      unsigned mtl;
      const char *name;
      const char *taken;      /* the pipe_format this library chose */
      const char *other;      /* ... and the one the other reading would have chosen */
      unsigned shift[4];      /* r, g, b, a: where the field starts */
      unsigned width[4];      /* ... and how wide it is; 0 means the format has none */
   };
   static const struct packed16 formats[] = {
      { 40, "MTLPixelFormatB5G6R5Unorm", "PIPE_FORMAT_R5G6B5_UNORM",
        "PIPE_FORMAT_B5G6R5_UNORM", { 0, 5, 11, 0 }, { 5, 6, 5, 0 } },
      { 41, "MTLPixelFormatA1BGR5Unorm", "PIPE_FORMAT_A1R5G5B5_UNORM",
        "PIPE_FORMAT_A1B5G5R5_UNORM", { 1, 6, 11, 0 }, { 5, 5, 5, 1 } },
      { 42, "MTLPixelFormatABGR4Unorm", "PIPE_FORMAT_A4R4G4B4_UNORM",
        "PIPE_FORMAT_A4B4G4R4_UNORM", { 4, 8, 12, 0 }, { 4, 4, 4, 4 } },
      { 43, "MTLPixelFormatBGR5A1Unorm", "PIPE_FORMAT_R5G5B5A1_UNORM",
        "PIPE_FORMAT_B5G5R5A1_UNORM", { 0, 5, 10, 15 }, { 5, 5, 5, 1 } },
   };
   static const char *const channel[4] = { "red", "green", "blue", "alpha" };

   /* The library serves these by default now; RWA_PACKED16=0 takes them out of the
    * picture, and this measurement is about the table and not about that switch, so it
    * asks for them whatever the environment says.
    */
   setenv("RWA_PACKED16", "1", 1);

   unsigned cases = 0, exact = 0, wrong = 0, failed_run = 0;
   for (unsigned f = 0; f < COUNT_OF(formats); f++) {
      const struct packed16 *p = &formats[f];
      for (unsigned c = 0; c < 4; c++) {
         if (!p->width[c])
            continue;
         unsigned word = ((1u << p->width[c]) - 1u) << p->shift[c];
         /* what the reading says that word must sample as: the one channel whose field
          * is set comes back 1, every other channel the format HAS comes back 0, and a
          * channel the format does not have is 1 for alpha and 0 otherwise -- which is
          * util_format's own rule for an absent channel and Metal's for a format with
          * no alpha.
          */
         float want[4];
         for (unsigned k = 0; k < 4; k++)
            want[k] = !p->width[k] ? (k == 3 ? 1.0f : 0.0f) : (k == c ? 1.0f : 0.0f);

         char texspec[32];
         snprintf(texspec, sizeof(texspec), "%u", p->mtl);
         probe_fill16 = word;
         probe_fill16_given = true;
         memset(last_pixel, 0, sizeof(last_pixel));
         printf("air_metal_test: --pixel-formats %s, the %u-bit %s field at bit %u, "
                "word 0x%04x\n", p->name, p->width[c], channel[c], p->shift[c], word);
         int rc = run_draw(path, NULL, func, "125", NULL, texspec);
         cases++;
         if (rc != 0) {
            printf("pixfmt: FAIL  %s %s field: the draw did not happen, so this case "
                   "measured nothing\n", p->name, channel[c]);
            failed_run++;
            continue;
         }
         bool ok = true;
         for (unsigned k = 0; k < 4; k++)
            ok = ok && last_pixel[k] == want[k];
         printf("pixfmt: %s  %s word 0x%04x -> %g %g %g %g (want %g %g %g %g, which is "
                "%s read as %s)\n", ok ? "ok  " : "WRONG", p->name, word,
                (double)last_pixel[0], (double)last_pixel[1], (double)last_pixel[2],
                (double)last_pixel[3], (double)want[0], (double)want[1],
                (double)want[2], (double)want[3], channel[c], p->taken);
         if (ok) {
            exact++;
         } else {
            wrong++;
            printf("pixfmt:        if %s came back 1 instead, the other reading is the "
                   "right one and this format wants %s\n",
                   c == 0 ? "blue" : c == 2 ? "red" : channel[c], p->other);
         }

         /* AND THE OTHER DIRECTION. The run above sampled the word; this one renders
          * the result back INTO a target of the same format and reads the raw 16-bit
          * word out. The two together are the round trip, and the word that comes back
          * must be the word that went in: a write that placed the fields somewhere else
          * than the read takes them from would come back as a different word, which
          * four floats could never show -- they would have gone out through the same
          * unpacking the write used.
          *
          * The alpha needs no special case. A format without one samples as 1 and the
          * target has nowhere to put it; a format with one carries whatever the probe
          * word set, which for a colour probe is 0 and for the alpha probe is 1.
          */
         probe_fill16 = word;
         probe_fill16_given = true;
         last_word = ~0u;
         rc = run_draw(path, NULL, func, texspec, NULL, texspec);
         cases++;
         if (rc != 0 || last_word == ~0u) {
            printf("pixfmt: FAIL  %s %s field: the round trip did not draw into a "
                   "%s target\n", p->name, channel[c], p->name);
            failed_run++;
         } else if (last_word != word) {
            printf("pixfmt: WRONG %s round trip: 0x%04x went in and 0x%04x came out, so "
                   "the write does not put the fields where the read takes them\n",
                   p->name, word, last_word);
            wrong++;
         } else {
            printf("pixfmt: ok    %s round trip: 0x%04x in, 0x%04x out -- the write and "
                   "the read agree on where every field is\n", p->name, word, last_word);
            exact++;
         }
      }
   }
   probe_fill16_given = false;

   printf("\npixfmt: %u probes over %u packed 16-bit formats: %u put the field in the "
          "channel the reading says, %u did not, %u could not be drawn\n", cases,
          (unsigned)COUNT_OF(formats), exact, wrong, failed_run);
   printf("pixfmt: every texel held the same word, so no coordinate, filter or address "
          "mode could have changed which channel answered\n");
   printf("pixfmt: a field of all ones is exactly 1.0 and a field of zeros exactly 0.0, "
          "so there is no rounding in these expectations\n");
   printf("pixfmt: this does NOT settle Apple's convention -- nothing on this machine "
          "states the bit positions -- it settles that the pipe_format chosen for each "
          "name has the fields that reading claims. The reading is settled by a boot: "
          "the other one exchanges red and blue\n");
   return (wrong || failed_run || !exact) ? 1 : 0;
}

/*
 * ===== --draw-blend: colour blending, against the arithmetic and against the ways of
 * ===== being wrong.
 *
 * WHY THIS MODE EXISTS. The contract carried no blend state at all until now, so every
 * pipeline this library made was opaque -- `.rt[0].colormask = PIPE_MASK_RGBA` and
 * nothing else -- and the window server's own compositor draws premultiplied-alpha
 * layers through it. A transparent-black label layer therefore landed as OPAQUE BLACK:
 * the black box behind every icon name on the home screen. That is the failure this
 * mode is built to make impossible to ship again, and it is a failure with a particular
 * shape -- nothing refused, nothing crashed, a picture came out wrong.
 *
 * A PASS HERE IS NOT "IT BLENDED". Blending at all is easy to achieve by accident, and
 * four plausible mistakes all produce a picture that looks blended:
 *
 *   - the source and destination factors exchanged
 *   - premultiplied alpha read as straight alpha (source factor One read as SourceAlpha)
 *   - the alpha half of the equation taken from the RGB half, or the other way round
 *   - the enumerations CAST instead of mapped: MTLBlendFactor and pipe_blendfactor agree
 *     on two of nineteen values, and MTLColorWriteMask's bits run backwards from
 *     Gallium's colormask
 *
 * So every case below computes, besides the correct answer, WHAT EACH OF THOSE MISTAKES
 * WOULD HAVE PRODUCED, prints them side by side, and says which of them the case can
 * tell apart. A case that cannot distinguish a mistake is counted as not distinguishing
 * it, and the run FAILS if any mistake is distinguished by no case at all -- a test that
 * cannot fail for the right reason is the thing this project has been bitten by before
 * (docs/air.md: "ask what else would have given the same answer").
 *
 * THE NUMBERS. Source (0.25, 0.5, 0.125, 0.25) over destination (0.5, 0.25, 0.75, 0.5),
 * all dyadic rationals, so every product, sum and difference in these equations is EXACT
 * in binary32 and the comparison is `==` and not a tolerance. The source alpha is 0.25
 * and not 0.5 deliberately: with alpha 0.5 the source-over equation is symmetrical in
 * the alpha channel and "the factors exchanged" would produce the SAME alpha as the
 * correct state. Nothing in these numbers is round.
 *
 * TWO ATTACHMENT FORMATS, for two different questions:
 *
 *   - MTLPixelFormatRGBA32Float (125) for the arithmetic. softpipe does not clamp a
 *     float attachment (sp_quad_blend.c sets its clamp flag from
 *     channel[0].normalized), so a Subtract that goes negative comes back negative
 *     instead of being hidden at zero -- which is what tells Subtract from
 *     ReverseSubtract.
 *   - MTLPixelFormatBGRA8Unorm (80) for the compositor's own case, because that is the
 *     format the home screen is composited in and an eight-bit round trip is a different
 *     path: softpipe blends in float in its tile cache and converts on write-back with
 *     float_to_ubyte, whose rounding is a bit trick and NOT (int)(f * 255 + 0.5f) --
 *     they disagree at 128 of the 255 tie points. The expectation here replicates that
 *     function rather than rounding its own way, and the destination is opaque WHITE on
 *     purpose: with every destination channel 255, source-over gives
 *     src + (255 - alpha)/255, which is an exact multiple of 1/255 and so an exact byte.
 *
 * WHAT IT CANNOT DO. It runs on softpipe, so it says nothing about the Adreno's blender
 * -- in particular nothing about Min or Max with a factor other than One (refused by
 * name: Metal's equation multiplies by the factors first and GL ignores them, and the
 * two back ends behind this library do different things), nothing about dither (a6xx
 * honours the bit and softpipe ignores it; this library leaves it clear), and nothing
 * about whether Apple's enumerations really hold the values the tables claim. That last
 * one is settled by the SDK header and by Metal.framework's own code, and in the end by
 * a photograph of the home screen.
 *
 *   air_metal_test --draw-blend FILE.metallib FUNCTION
 */

enum {
   /* MTLBlendFactor, MTLBlendOperation and MTLColorWriteMask, Apple's own numbers */
   BF_ZERO = 0, BF_ONE = 1, BF_SRC_COLOR = 2, BF_INV_SRC_COLOR = 3,
   BF_SRC_ALPHA = 4, BF_INV_SRC_ALPHA = 5, BF_DST_COLOR = 6, BF_INV_DST_COLOR = 7,
   BF_DST_ALPHA = 8, BF_INV_DST_ALPHA = 9, BF_SRC_ALPHA_SAT = 10,
   BF_BLEND_COLOR = 11, BF_SRC1_COLOR = 15, BF_PAST_END = 19,
   BO_ADD = 0, BO_SUB = 1, BO_RSUB = 2, BO_MIN = 3, BO_MAX = 4, BO_PAST_END = 5,
   CWM_NONE = 0x0, CWM_A = 0x1, CWM_B = 0x2, CWM_G = 0x4, CWM_R = 0x8,
   CWM_GB = 0x6, CWM_RA = 0x9, CWM_GBA = 0x7, CWM_RGB = 0xe, CWM_ALL = 0xf,
   MTL_BGRA8_UNORM = 80,
};

static float
blend_min(float a, float b)
{
   return a < b ? a : b;
}

/*
 * Metal's blend factor, as Apple's own equation defines it: the number multiplied into
 * one channel of one term. For channel 3 a "Color" factor means that term's alpha, which
 * is what every specification of this equation means by it.
 *
 * MTLBlendFactorSourceAlphaSaturated is written with GL's alpha rule -- 1 -- because
 * that is what BOTH back ends implement (softpipe says "multiply alpha by 1.0";
 * freedreno's a2xx driver says "hardware doesn't support SRC_ALPHA_SATURATE for alpha,
 * but it is equivalent to ONE"), and the library refuses that factor for the two alpha
 * fields for exactly that reason: Metal's one equation says min(As, 1 - Ad) there. The
 * alpha case below is therefore unreachable from a served pipeline and is written out so
 * the two readings can be printed next to each other.
 */
static float
metal_factor(unsigned f, unsigned channel, const float src[4], const float dst[4],
             bool *modelled)
{
   float sc = src[channel], dc = dst[channel];

   switch (f) {
   case BF_ZERO:          return 0.0f;
   case BF_ONE:           return 1.0f;
   case BF_SRC_COLOR:     return sc;
   case BF_INV_SRC_COLOR: return 1.0f - sc;
   case BF_SRC_ALPHA:     return src[3];
   case BF_INV_SRC_ALPHA: return 1.0f - src[3];
   case BF_DST_COLOR:     return dc;
   case BF_INV_DST_COLOR: return 1.0f - dc;
   case BF_DST_ALPHA:     return dst[3];
   case BF_INV_DST_ALPHA: return 1.0f - dst[3];
   case BF_SRC_ALPHA_SAT:
      return channel < 3 ? blend_min(src[3], 1.0f - dst[3]) : 1.0f;
   default:
      *modelled = false;
      return 0.0f;
   }
}

/*
 * The same question asked of GALLIUM's enumeration, which is what a CAST would have
 * produced. Not a hypothetical: these are the values pipe_blendfactor really holds
 * (src/util/blend.h -- the plain factors run 1..10 and their one-minus forms are those
 * numbers with 0x10 set, so ZERO is 0x11), and a Metal number landing in this table is
 * the mistake this library's table exists to prevent.
 *
 * The two constant-colour factors answer 0 and their inverses 1, because that is what a
 * Gallium context holds before anyone calls set_blend_color -- which nothing here does,
 * and which is why the library refuses Metal's BlendColor factors outright.
 */
static float
gallium_factor(unsigned pf, unsigned channel, const float src[4], const float dst[4],
               bool *modelled)
{
   float sc = src[channel], dc = dst[channel];

   switch (pf) {
   case 1:    return 1.0f;                                        /* ONE */
   case 2:    return sc;                                           /* SRC_COLOR */
   case 3:    return src[3];                                       /* SRC_ALPHA */
   case 4:    return dst[3];                                       /* DST_ALPHA */
   case 5:    return dc;                                           /* DST_COLOR */
   case 6:    return channel < 3 ? blend_min(src[3], 1.0f - dst[3]) : 1.0f;
   case 7:    return 0.0f;                                         /* CONST_COLOR */
   case 8:    return 0.0f;                                         /* CONST_ALPHA */
   case 0x11: return 0.0f;                                         /* ZERO */
   case 0x12: return 1.0f - sc;                                    /* INV_SRC_COLOR */
   case 0x13: return 1.0f - src[3];                                /* INV_SRC_ALPHA */
   case 0x14: return 1.0f - dst[3];                                /* INV_DST_ALPHA */
   case 0x15: return 1.0f - dc;                                    /* INV_DST_COLOR */
   case 0x17: return 1.0f;                                         /* INV_CONST_COLOR */
   case 0x18: return 1.0f;                                         /* INV_CONST_ALPHA */
   default:
      /* 0 and 0x0b..0x10 and 0x16 are no enumerator at all, and 9, 10, 0x19, 0x1a are
       * dual-source factors reading a second output that does not exist here. Both are
       * "this program cannot say what would have come out", which is a stronger thing to
       * report than a number.
       */
      *modelled = false;
      return 0.0f;
   }
}

static float
blend_equation(unsigned op, float s, float d, bool *modelled)
{
   switch (op) {
   case BO_ADD:  return s + d;
   case BO_SUB:  return s - d;
   case BO_RSUB: return d - s;
   case BO_MIN:  return blend_min(s, d);
   case BO_MAX:  return s > d ? s : d;
   default:
      *modelled = false;
      return 0.0f;
   }
}

/* The ways of being wrong this mode distinguishes, each one a real mistake someone could
 * make in the fifty lines of translation this is a test of. */
enum blend_mistake {
   MISTAKE_NONE = 0,              /* the correct answer */
   MISTAKE_FACTORS_EXCHANGED,
   MISTAKE_STRAIGHT_ALPHA,
   MISTAKE_ALPHA_FROM_RGB,
   MISTAKE_RGB_FROM_ALPHA,
   MISTAKE_FACTOR_CAST,
   MISTAKE_MASK_CAST,
   MISTAKE_NOT_BLENDED,
   MISTAKE_COUNT,
};

static const char *const mistake_name[MISTAKE_COUNT] = {
   "the mapping as written",
   "the source and destination factors exchanged",
   "premultiplied alpha read as straight alpha (source factor One as SourceAlpha)",
   "the alpha operation and factors taken from the RGB ones",
   "the RGB operation and factors taken from the alpha ones",
   "MTLBlendFactor CAST to pipe_blendfactor instead of mapped",
   "MTLColorWriteMask CAST to a Gallium colormask instead of reversed",
   "not blended at all: the source replacing the destination, which is what this "
      "library did until now",
};

/*
 * One case's answer under one reading. `clamp01` is the attachment's own rule: softpipe
 * clamps a normalised format at every step and does not clamp a float one.
 */
static void
blend_answer(enum blend_mistake how, const struct rwa_color_blend *b, const float src[4],
             const float dst[4], bool clamp01, float out[4], bool *modelled)
{
   struct rwa_color_blend c = *b;

   *modelled = true;

   switch (how) {
   case MISTAKE_FACTORS_EXCHANGED:
      c.source_rgb_blend_factor = b->destination_rgb_blend_factor;
      c.destination_rgb_blend_factor = b->source_rgb_blend_factor;
      c.source_alpha_blend_factor = b->destination_alpha_blend_factor;
      c.destination_alpha_blend_factor = b->source_alpha_blend_factor;
      break;
   case MISTAKE_STRAIGHT_ALPHA:
      if (c.source_rgb_blend_factor == BF_ONE)
         c.source_rgb_blend_factor = BF_SRC_ALPHA;
      if (c.source_alpha_blend_factor == BF_ONE)
         c.source_alpha_blend_factor = BF_SRC_ALPHA;
      break;
   case MISTAKE_ALPHA_FROM_RGB:
      c.alpha_blend_operation = b->rgb_blend_operation;
      c.source_alpha_blend_factor = b->source_rgb_blend_factor;
      c.destination_alpha_blend_factor = b->destination_rgb_blend_factor;
      break;
   case MISTAKE_RGB_FROM_ALPHA:
      c.rgb_blend_operation = b->alpha_blend_operation;
      c.source_rgb_blend_factor = b->source_alpha_blend_factor;
      c.destination_rgb_blend_factor = b->destination_alpha_blend_factor;
      break;
   case MISTAKE_NOT_BLENDED:
      c.blending_enabled = 0;
      c.write_mask = CWM_ALL;
      break;
   default:
      break;
   }

   for (unsigned ch = 0; ch < 4; ch++) {
      unsigned sf = ch < 3 ? c.source_rgb_blend_factor : c.source_alpha_blend_factor;
      unsigned df = ch < 3 ? c.destination_rgb_blend_factor
                           : c.destination_alpha_blend_factor;
      unsigned op = ch < 3 ? c.rgb_blend_operation : c.alpha_blend_operation;
      bool written;
      float v;

      if (!c.blending_enabled) {
         v = src[ch];
      } else if (how == MISTAKE_FACTOR_CAST) {
         v = blend_equation(op, src[ch] * gallium_factor(sf, ch, src, dst, modelled),
                            dst[ch] * gallium_factor(df, ch, src, dst, modelled),
                            modelled);
      } else {
         v = blend_equation(op, src[ch] * metal_factor(sf, ch, src, dst, modelled),
                            dst[ch] * metal_factor(df, ch, src, dst, modelled),
                            modelled);
      }
      if (clamp01)
         v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);

      /* MTLColorWriteMask has Red at 1 << 3 and Alpha at 1 << 0, so channel ch is bit
       * 3 - ch. The cast mistake reads channel ch at bit ch, which is Gallium's order.
       */
      written = how == MISTAKE_MASK_CAST ? (c.write_mask & (1u << ch)) != 0
                                         : (c.write_mask & (1u << (3 - ch))) != 0;
      out[ch] = written ? v : dst[ch];
   }
}

/*
 * softpipe's own float-to-byte conversion, replicated bit for bit (src/util/u_math.h's
 * float_to_ubyte). It is NOT (int)(f * 255 + 0.5f) and it is not lrintf(f * 255): it is
 * a bit trick that rounds half to even, and the two disagree at 128 of the 255 tie
 * points. An expectation computed any other way would be a second rounding rule
 * disagreeing with the first, which is a test failing for a reason that has nothing to
 * do with blending.
 */
static uint8_t
expect_ubyte(float f)
{
   union { float f; uint32_t i; } t;

   if (!(f > 0.0f))
      return 0;
   if (f >= 1.0f)
       return 255;
   t.f = f * (255.0f / 256.0f) + 32768.0f;
   return (uint8_t)t.i;
}

static float
byte_as_float(uint8_t b)
{
   return (float)b * (1.0f / 255.0f);   /* u_math.h's ubyte_to_float */
}

struct blend_case {
   const char *what;
   unsigned attachment;          /* the colour attachment's MTLPixelFormat */
   unsigned attachments;         /* how many attachments the pipeline declares */
   unsigned blend_count;         /* color_blend_count passed; 0 is "no blend state" */
   struct rwa_color_blend b;
   bool must_refuse;             /* this case must be refused, and the sentence printed */
   /* attachment 1's entry, for the one case that has two and wants them to DIFFER.
    * Appended so that every row above can leave it out -- which is also why it is read
    * only when `blend_count` is 2 or more. */
   struct rwa_color_blend b1;
};

/* A served case, spelled out so the table reads as Metal's own descriptor. */
#define BLEND_ON(sr, dr, opr, sa, da, opa, mask)                                        \
   { .blending_enabled = 1, .rgb_blend_operation = (opr),                               \
     .alpha_blend_operation = (opa), .source_rgb_blend_factor = (sr),                   \
     .destination_rgb_blend_factor = (dr), .source_alpha_blend_factor = (sa),           \
     .destination_alpha_blend_factor = (da), .write_mask = (mask) }

static int
run_blend(const char *path, const char *func)
{
   /* The source and the destination: dyadic, asymmetrical, and the source alpha is not
    * 0.5 so that exchanging the factors changes the alpha channel too.
    */
   static const float src[4] = { 0.25f, 0.5f, 0.125f, 0.25f };
   static const float dst[4] = { 0.5f, 0.25f, 0.75f, 0.5f };
   /* and the eight-bit case's pair: an OPAQUE WHITE destination, which makes
    * source-over land on an exact multiple of 1/255 in every channel
    */
   static const uint8_t dst8[4] = { 255, 255, 255, 255 };          /* B, G, R, A */
   static const float src8[4] = { 32.0f / 255.0f, 64.0f / 255.0f, 16.0f / 255.0f,
                                  64.0f / 255.0f };

   static const struct blend_case cases[] = {
      /* The old path, unchanged: no blend state in the shape at all. */
      { "no blend state in the shape (color_blend_count 0)", MTL_RGBA32_FLOAT, 1, 0,
        { 0 }, false },
      { "blending disabled, writeMask All", MTL_RGBA32_FLOAT, 1, 1,
        { .write_mask = CWM_ALL }, false },
      /*
       * APPLE'S OWN UNTOUCHED DESCRIPTOR, which is the single most important row here:
       * 504 of the 1079 pipelines in this device's CoreAnimation archive arrive exactly
       * like this, because they blend inside their fragment shader and never touch the
       * descriptor. The word
       * -[MTLRenderPipelineColorAttachmentDescriptorInternal init] writes is 0x78001080:
       * blendingEnabled NO, both operations Add, source factors One, destination
       * factors Zero, writeMask All.
       *
       * EVERY ONE OF THOSE FIELDS IS NON-ZERO EXCEPT blendingEnabled. So a driver that
       * decided "this descriptor carries blend state because a field is set" would blend
       * these 504 twice, and this row is what catches that: the answer must be the
       * source, unchanged, exactly as the row above it.
       */
      { "Apple's untouched descriptor: One/Zero, Add, writeMask All, blending OFF -- the "
        "504-of-1079 case", MTL_RGBA32_FLOAT, 1, 1,
        { .blending_enabled = 0, .rgb_blend_operation = BO_ADD,
          .alpha_blend_operation = BO_ADD, .source_rgb_blend_factor = BF_ONE,
          .destination_rgb_blend_factor = BF_ZERO, .source_alpha_blend_factor = BF_ONE,
          .destination_alpha_blend_factor = BF_ZERO, .write_mask = CWM_ALL }, false },

      /* The compositor's own state, and the two things it is most often confused with. */
      { "premultiplied source-over (One, OneMinusSourceAlpha, Add) -- CoreAnimation's",
        MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_ALL), false },
      { "straight alpha (SourceAlpha, OneMinusSourceAlpha, Add)", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_SRC_ALPHA, BF_INV_SRC_ALPHA, BO_ADD, BF_SRC_ALPHA, BF_INV_SRC_ALPHA,
                 BO_ADD, CWM_ALL), false },
      { "source-over with the factors the other way round", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_INV_SRC_ALPHA, BF_ONE, BO_ADD, BF_INV_SRC_ALPHA, BF_ONE, BO_ADD,
                 CWM_ALL), false },

      /* The two halves of the equation, made to differ. */
      { "colour blended, alpha accumulated (rgb One/OneMinusSourceAlpha, alpha One/One)",
        MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_ONE, BO_ADD, CWM_ALL),
        false },
      { "rgb Add and alpha Max, both One/One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ONE, BO_ADD, BF_ONE, BF_ONE, BO_MAX, CWM_ALL), false },

      /* The five operations. Min and Max take One and One, which is the only form this
       * library serves them in and the only one both back ends agree about.
       */
      { "Subtract, One/One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ONE, BO_SUB, BF_ONE, BF_ONE, BO_SUB, CWM_ALL), false },
      { "ReverseSubtract, One/One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ONE, BO_RSUB, BF_ONE, BF_ONE, BO_RSUB, CWM_ALL), false },
      { "Min, One/One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ONE, BO_MIN, BF_ONE, BF_ONE, BO_MIN, CWM_ALL), false },
      { "Max, One/One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ONE, BO_MAX, BF_ONE, BF_ONE, BO_MAX, CWM_ALL), false },

      /* Rows of the factor table a cast gets wrong in a way no source-over case would
       * show: 2 and 3 are the only two a cast gets RIGHT or nearly so, 6 and 7 land on
       * SourceAlphaSaturated and CONST_COLOR, 8 and 9 on CONST_ALPHA and a dual-source
       * factor, and 0 lands on no enumerator at all.
       */
      { "modulate (DestinationColor, Zero)", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_DST_COLOR, BF_ZERO, BO_ADD, BF_DST_COLOR, BF_ZERO, BO_ADD, CWM_ALL),
        false },
      { "OneMinusDestinationColor and SourceColor", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_INV_DST_COLOR, BF_SRC_COLOR, BO_ADD, BF_INV_DST_COLOR, BF_SRC_COLOR,
                 BO_ADD, CWM_ALL), false },
      { "DestinationAlpha and OneMinusDestinationAlpha", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_DST_ALPHA, BF_INV_DST_ALPHA, BO_ADD, BF_DST_ALPHA, BF_INV_DST_ALPHA,
                 BO_ADD, CWM_ALL), false },
      { "OneMinusSourceColor and SourceColor", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_INV_SRC_COLOR, BF_SRC_COLOR, BO_ADD, BF_INV_SRC_COLOR, BF_SRC_COLOR,
                 BO_ADD, CWM_ALL), false },
      { "SourceAlphaSaturated for the colour, One for the alpha", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_SRC_ALPHA_SAT, BF_ZERO, BO_ADD, BF_ONE, BF_ZERO, BO_ADD, CWM_ALL),
        false },

      /* THE WRITE MASKS, and the point of this group is the two that are NOT palindromes
       * under a bit reversal. A suite built out of All and None alone would pass with the
       * nibble backwards.
       */
      { "writeMask Red only (0x8), blending on", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_R), false },
      { "writeMask Alpha only (0x1), blending on", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_A), false },
      { "writeMask Red|Green|Blue (0xe): colour written, alpha kept", MTL_RGBA32_FLOAT, 1,
        1, BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                    CWM_RGB), false },
      { "writeMask Green|Blue|Alpha (0x7): red kept", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_GBA), false },
      { "writeMask Green|Blue (0x6), a palindrome a cast gets right", MTL_RGBA32_FLOAT, 1,
        1, BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                    CWM_GB), false },
      { "writeMask None (0x0): nothing written at all", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_NONE), false },

      /* The home screen's own format and own equation, in eight bits. */
      { "premultiplied source-over into MTLPixelFormatBGRA8Unorm, over opaque white",
        MTL_BGRA8_UNORM, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_ALL), false },

      /*
       * TWO ATTACHMENTS WHOSE BLEND STATES DIFFER, which must be SERVED and not refused:
       * it is the shape CoreAnimation itself builds. create_pipeline_state's
       * multiple-attachment loop sends `setWriteMask: 0` to every attachment that is not
       * the primary one and sends `setBlendingEnabled:` to none of them, so attachment 0
       * blends with every channel written and the rest write nothing at all. With
       * Gallium's independent_blend_enable clear, rt[0] would govern them all and
       * attachment 1 would be written -- so what this row checks is that the bit went
       * on. Both back ends have the capability (freedreno_screen.c and sp_screen.c each
       * set indep_blend_enable and indep_blend_func).
       */
      { "two attachments, one blending with writeMask All and one with writeMask None "
        "-- CoreAnimation's own multiple-attachment shape", MTL_RGBA32_FLOAT, 2, 2,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_ALL), false,
        { .blending_enabled = 0, .write_mask = CWM_NONE } },

      /* AND WHAT MUST BE REFUSED BY NAME. Every one of these would otherwise draw
       * something.
       */
      { "MTLBlendFactorBlendColor, whose colour is encoder state", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_BLEND_COLOR, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA,
                 BO_ADD, CWM_ALL), true },
      { "MTLBlendFactorSource1Color: dual-source, with no second output in the contract",
        MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_SRC1_COLOR, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA,
                 BO_ADD, CWM_ALL), true },
      { "a blend factor of 19, one past Metal's last", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_PAST_END, BF_ZERO, BO_ADD, BF_ONE, BF_ZERO, BO_ADD, CWM_ALL), true },
      { "a blend operation of 5, one past Metal's last", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ZERO, BO_PAST_END, BF_ONE, BF_ZERO, BO_ADD, CWM_ALL), true },
      { "a writeMask of 0x10, a fifth bit MTLColorWriteMask has not got",
        MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ZERO, BO_ADD, BF_ONE, BF_ZERO, BO_ADD, 0x10), true },
      { "MTLBlendFactorSourceAlphaSaturated as the sourceAlphaBlendFactor",
        MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ZERO, BO_ADD, BF_SRC_ALPHA_SAT, BF_ZERO, BO_ADD, CWM_ALL),
        true },
      { "Min with a factor other than One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_SRC_ALPHA, BF_ONE, BO_MIN, BF_ONE, BF_ONE, BO_ADD, CWM_ALL), true },
      { "blending enabled on an MTLPixelFormatRGBA32Uint attachment", MTL_RGBA32_UINT, 1,
        1, BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                    CWM_ALL), true },
      { "blend state for one attachment on a pipeline that has two", MTL_RGBA32_FLOAT, 2,
        1, BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                    CWM_ALL), true },
   };

   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      return 1;
   printf("air_metal_test: --draw-blend %s, %zu bytes of bitcode\n", name, bitcode_size);

   setenv("RWA_SOFTWARE", "1", 0);
   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: device is \"%s\"\n", rwa_device_name(device));
   printf("blend: source (%g %g %g %g) over destination (%g %g %g %g), and the eight-bit "
          "case is (%u %u %u)/255 alpha %u/255 over opaque white\n",
          (double)src[0], (double)src[1], (double)src[2], (double)src[3], (double)dst[0],
          (double)dst[1], (double)dst[2], (double)dst[3], 16u, 64u, 32u, 64u);

   unsigned served = 0, right = 0, wrong = 0, refused_right = 0, refused_wrong = 0;
   unsigned distinguishes[MISTAKE_COUNT] = { 0 };

   for (unsigned ci = 0; ci < COUNT_OF(cases); ci++) {
      const struct blend_case *c = &cases[ci];
      bool eight = c->attachment == MTL_BGRA8_UNORM;
      const float *s = eight ? src8 : src;
      float d[4];

      for (unsigned ch = 0; ch < 4; ch++)
         d[ch] = eight ? byte_as_float(dst8[ch == 0 ? 2 : ch == 2 ? 0 : ch]) : dst[ch];

      printf("\nblend: %s\n", c->what);

      /* The pipeline, which is where every refusal happens: blend state is compiled into
       * a Metal pipeline and so is checked when one is made, not at a draw.
       */
      struct rwa_render_shape shape = {
         .color_count = c->attachments, .sample_count = 1,
         .color_blend_count = c->blend_count,
      };
      for (unsigned i = 0; i < c->attachments; i++)
         shape.color_format[i] = c->attachment;
      if (c->blend_count)
         shape.color_blend[0] = c->b;
      if (c->blend_count >= 2)
         shape.color_blend[1] = c->b1;

      struct rwa_render_info info = { 0 };
      why[0] = '\0';
      struct rwa_render *render =
         rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &shape,
                           &info, why, sizeof(why));
      if (c->must_refuse) {
         if (render) {
            printf("blend: NOT REFUSED  %s: the pipeline was made, so this would have "
                   "drawn something\n", c->what);
            refused_wrong++;
            rwa_render_destroy(render);
         } else {
            printf("blend: refused     %s\n", why);
            refused_right++;
         }
         continue;
      }
      if (!render) {
         printf("blend: FAIL  rwa_render_create: %s\n", why);
         wrong++;
         continue;
      }
      printf("blend: the library blends attachments 0x%x and writes 0x%08x (four Gallium "
             "bits per attachment, R 0x1 .. A 0x8)\n", info.blend_enabled_mask,
             info.color_write_masks);

      /* The attachment, pre-filled with the destination and NOT cleared by the draw:
       * clear_color NULL is MTLLoadActionLoad, so what the blender reads is what was
       * written here.
       */
      const unsigned origin[3] = { 0, 0, 0 }, extent[3] = { TEX, TEX, 1 };
      /* One per attachment the pipeline declares, all filled with the destination: a
       * draw must cover every attachment the pipeline was made for, which the library
       * refuses a mismatch of by name.
       */
      struct rwa_texture *color[2] = { 0 };
      union { float f[TEX * TEX * 4]; uint8_t b[TEX * TEX * 4]; } fill;
      for (unsigned t = 0; t < TEX * TEX; t++)
         for (unsigned ch = 0; ch < 4; ch++) {
            if (eight)
               fill.b[t * 4 + ch] = dst8[ch];
            else
               fill.f[t * 4 + ch] = dst[ch];
         }
      bool made = true;
      for (unsigned i = 0; i < c->attachments && i < COUNT_OF(color) && made; i++) {
         const struct rwa_texture_shape cts = {
            .pixel_format = c->attachment, .type = MTL_TEXTURE_TYPE_2D,
            .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
            .sample_count = 1,
            .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
         };
         color[i] = rwa_texture_create(device, &cts, why, sizeof(why));
         made = color[i] != NULL &&
                rwa_texture_write(color[i], 0, 0, origin, extent, &fill, 0, why,
                                  sizeof(why)) == 0;
      }
      if (!made) {
         printf("blend: FAIL  making attachment %u: %s\n", c->attachments, why);
         for (unsigned i = 0; i < COUNT_OF(color); i++)
            rwa_texture_destroy(color[i]);
         rwa_render_destroy(render);
         wrong++;
         continue;
      }

      /* The source, which is a texture the fragment shader samples: every texel the same
       * value, so no coordinate, filter or address mode can change what the source
       * colour is. It is always RGBA32Float whatever the attachment is -- the source of a
       * blend is the shader's output and has nothing to do with the attachment's format.
       */
      struct rwa_texture *tex[8] = { 0 };
      struct rwa_sampler *samp[8] = { 0 };
      struct rwa_buffer *buf[16] = { 0 };
      unsigned ntex = info.fragment_textures < COUNT_OF(tex) ? info.fragment_textures
                                                             : (unsigned)COUNT_OF(tex);
      unsigned nsamp = info.fragment_samplers < COUNT_OF(samp) ? info.fragment_samplers
                                                                : (unsigned)COUNT_OF(samp);
      unsigned nbuf = info.fragment_buffers < COUNT_OF(buf) ? info.fragment_buffers
                                                             : (unsigned)COUNT_OF(buf);
      bool built = true;
      for (unsigned i = 0; i < ntex && built; i++) {
         const struct rwa_texture_shape ts = {
            .pixel_format = MTL_RGBA32_FLOAT, .type = MTL_TEXTURE_TYPE_2D,
            .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
            .sample_count = 1, .usage = MTL_USAGE_SHADER_READ,
         };
         tex[i] = rwa_texture_create(device, &ts, why, sizeof(why));
         built = tex[i] != NULL;
         if (built) {
            float texels[TEX * TEX * 4];
            for (unsigned t = 0; t < TEX * TEX; t++)
               for (unsigned ch = 0; ch < 4; ch++)
                  texels[t * 4 + ch] = s[ch];
            built = rwa_texture_write(tex[i], 0, 0, origin, extent, texels, 0, why,
                                      sizeof(why)) == 0;
         }
      }
      for (unsigned i = 0; i < nsamp && built; i++) {
         const struct rwa_sampler_state st = {
            .address = { 0, 0, 0 }, .min_filter = 0, .mag_filter = 0, .mip_filter = 0,
            .compare_function = 0, .max_anisotropy = 1, .normalized_coordinates = 1,
            .lod_min = 0.0f, .lod_max = 65504.0f,
         };
         samp[i] = rwa_sampler_create(device, &st, why, sizeof(why));
         built = samp[i] != NULL;
      }
      for (unsigned i = 0; i < nbuf && built; i++) {
         buf[i] = rwa_buffer_create(device, 48, why, sizeof(why));
         built = buf[i] != NULL;
         if (built) {
            uint8_t *bits = rwa_buffer_map(buf[i]);
            for (unsigned k = 0; k < rwa_buffer_size(buf[i]); k++)
               bits[k] = pattern_byte(k);
         }
      }

      union { float f[TEX * TEX * 4]; uint8_t b[TEX * TEX * 4]; } got;
      memset(&got, 0, sizeof(got));
      if (built) {
         const struct rwa_draw draw = {
            .color = color, .color_count = c->attachments,
            .clear_color = NULL,           /* MTLLoadActionLoad: keep the destination */
            .fragment = { .buffers = buf, .buffer_count = nbuf, .textures = tex,
                          .texture_count = ntex, .samplers = samp,
                          .sampler_count = nsamp },
            .primitive = 4,                /* MTLPrimitiveTypeTriangleStrip */
            .vertex_start = 0, .vertex_count = 4,
         };
         if (rwa_render_draw(render, &draw, why, sizeof(why)) != 0) {
            printf("blend: FAIL  rwa_render_draw: %s\n", why);
            built = false;
         } else if (rwa_texture_read(color[0], 0, 0, origin, extent, &got, 0, why,
                                     sizeof(why)) != 0) {
            printf("blend: FAIL  rwa_texture_read: %s\n", why);
            built = false;
         }
      } else {
         printf("blend: FAIL  setting up the draw: %s\n", why);
      }

      for (unsigned i = 0; i < ntex; i++)
         rwa_texture_destroy(tex[i]);
      for (unsigned i = 0; i < nsamp; i++)
         rwa_sampler_destroy(samp[i]);
      for (unsigned i = 0; i < nbuf; i++)
         rwa_buffer_destroy(buf[i]);
      for (unsigned i = 0; i < COUNT_OF(color); i++)
         rwa_texture_destroy(color[i]);
      rwa_render_destroy(render);

      if (!built) {
         wrong++;
         continue;
      }
      served++;

      /*
       * The verdict, and every candidate answer printed beside it. `clamp01` is the
       * attachment's rule and not a choice made here: softpipe clamps a normalised
       * format at every step and leaves a float one alone.
       */
      struct rwa_color_blend eff = c->blend_count ? c->b : (struct rwa_color_blend){
         .write_mask = CWM_ALL };
      float want[MISTAKE_COUNT][4];
      bool modelled[MISTAKE_COUNT];
      for (unsigned m = 0; m < MISTAKE_COUNT; m++) {
         modelled[m] = true;
         blend_answer((enum blend_mistake)m, &eff, s, d, eight, want[m], &modelled[m]);
      }

      /* every texel, not one: a blend applied to some fragments and not others is a
       * thing this would otherwise miss entirely
       */
      unsigned differing_texels = 0;
      float measured[4];
      for (unsigned ch = 0; ch < 4; ch++)
         measured[ch] = eight ? byte_as_float(got.b[ch == 0 ? 2 : ch == 2 ? 0 : ch])
                              : got.f[ch];
      for (unsigned t = 0; t < TEX * TEX; t++)
         for (unsigned ch = 0; ch < 4; ch++) {
            bool same = eight ? got.b[t * 4 + ch] == got.b[ch]
                              : got.f[t * 4 + ch] == got.f[ch];
            differing_texels += same ? 0 : 1;
         }

      bool ok;
      if (eight) {
         /* in bytes, with softpipe's own rounding replicated for the expectation */
         ok = true;
         for (unsigned ch = 0; ch < 4; ch++) {
            unsigned mem = ch == 0 ? 2 : ch == 2 ? 0 : ch;   /* B G R A in memory */
            ok = ok && got.b[mem] == expect_ubyte(want[MISTAKE_NONE][ch]);
         }
         printf("blend: %s  got bytes B %u G %u R %u A %u, want B %u G %u R %u A %u\n",
                ok ? "ok  " : "WRONG", got.b[0], got.b[1], got.b[2], got.b[3],
                expect_ubyte(want[MISTAKE_NONE][2]),
                expect_ubyte(want[MISTAKE_NONE][1]),
                expect_ubyte(want[MISTAKE_NONE][0]),
                expect_ubyte(want[MISTAKE_NONE][3]));
      } else {
         ok = true;
         for (unsigned ch = 0; ch < 4; ch++)
            ok = ok && got.f[ch] == want[MISTAKE_NONE][ch];
         printf("blend: %s  got %g %g %g %g, want %g %g %g %g\n",
                ok ? "ok  " : "WRONG", (double)got.f[0], (double)got.f[1],
                (double)got.f[2], (double)got.f[3], (double)want[MISTAKE_NONE][0],
                (double)want[MISTAKE_NONE][1], (double)want[MISTAKE_NONE][2],
                (double)want[MISTAKE_NONE][3]);
      }
      if (differing_texels)
         printf("blend: and %u of the %u channels of this 4x4 differ from texel (0,0), "
                "so the blend was not uniform over the quad\n", differing_texels,
                TEX * TEX * 4);

      /* What each mistake would have drawn, and whether this case can tell it from the
       * right answer. A mistake that matches the right answer HERE is not a mistake this
       * case catches, and saying so is the difference between a suite and a number.
       */
      for (unsigned m = MISTAKE_NONE + 1; m < MISTAKE_COUNT; m++) {
         bool same = true;
         for (unsigned ch = 0; ch < 4; ch++)
            same = same && want[m][ch] == want[MISTAKE_NONE][ch];
         if (!modelled[m]) {
            printf("blend:        %-72s would not have drawn a number this program can "
                   "name (a Gallium enumerator that does not exist, or a dual-source "
                   "read)\n", mistake_name[m]);
            distinguishes[m]++;
            continue;
         }
         /* In the attachment's own units, and that matters: an eight-bit case measured
          * in bytes and priced in floats is two scales in one table, and two of these
          * candidates differ by less than 1/255 -- which is a distinction the bytes do
          * not have and the floats would claim.
          */
         if (eight) {
            same = true;
            for (unsigned ch = 0; ch < 4; ch++)
               same = same && expect_ubyte(want[m][ch]) ==
                                 expect_ubyte(want[MISTAKE_NONE][ch]);
            printf("blend:        %-72s %s bytes B %u G %u R %u A %u\n", mistake_name[m],
                   same ? "gives the SAME" : "would give", expect_ubyte(want[m][2]),
                   expect_ubyte(want[m][1]), expect_ubyte(want[m][0]),
                   expect_ubyte(want[m][3]));
         } else {
            printf("blend:        %-72s %s %g %g %g %g\n", mistake_name[m],
                   same ? "gives the SAME" : "would give", (double)want[m][0],
                   (double)want[m][1], (double)want[m][2], (double)want[m][3]);
         }
         if (!same) {
            distinguishes[m]++;
            /* and if the measurement matches a mistake, name it rather than leave the
             * reader to compare two rows of numbers
             */
            bool matches = true;
            for (unsigned ch = 0; ch < 4; ch++) {
               unsigned mem = ch == 0 ? 2 : ch == 2 ? 0 : ch;
               matches = matches && (eight ? got.b[mem] == expect_ubyte(want[m][ch])
                                           : measured[ch] == want[m][ch]);
            }
            if (!ok && matches)
               printf("blend:        ... AND THAT IS WHAT CAME BACK: %s\n",
                      mistake_name[m]);
         }
      }

      if (ok)
         right++;
      else
         wrong++;
   }

   rwa_device_close(device);

   printf("\nblend: %u cases drawn: %u gave the arithmetic exactly, %u did not; %u of %u "
          "cases that must be refused were refused by name, %u were not\n",
          served, right, wrong, refused_right,
          refused_right + refused_wrong, refused_wrong);
   printf("blend: the expectations are Metal's own equation -- source times its factor, "
          "the operation, destination times its factor -- and not this library's reading "
          "of it: every input is a dyadic rational, so the float cases are compared with "
          "== and the eight-bit case against softpipe's own float_to_ubyte\n");
   printf("blend: what each way of being wrong would have drawn, and whether any case "
          "tells it apart:\n");
   unsigned thin = 0;
   for (unsigned m = MISTAKE_NONE + 1; m < MISTAKE_COUNT; m++) {
      printf("blend:   %-72s %u of %u cases distinguish it\n", mistake_name[m],
             distinguishes[m], served);
      if (!distinguishes[m])
         thin++;
   }
   if (thin)
      printf("blend: %u way%s of being wrong that NO case here distinguishes, so a pass "
             "says nothing about %s\n", thin, thin == 1 ? "" : "s",
             thin == 1 ? "it" : "them");
   printf("blend: nothing here is measured on the Adreno. What this cannot reach: Min or "
          "Max with a factor other than One (refused by name -- Metal's equation "
          "multiplies by the factors and GL ignores them, and softpipe and fd6_blend.cc "
          "do different things with what survives), dither (a6xx honours the bit, "
          "softpipe ignores it, and this library leaves it clear), and whether Apple's "
          "enumerations hold the values these tables claim -- which the home screen's "
          "own photograph answers\n");
   return (wrong || refused_wrong || thin || !served) ? 1 : 0;
}

/* --fbfetch isolates the fragment input [[color(0)]] on the device. The attachment
 * starts at opaque RGB 234 and the shader returns dest_0 unchanged, with fixed blending
 * disabled. Every pixel in the Calendar draw's scissor must retain that value; zero RGB
 * exposes a failed fetch without involving textures, uniforms or vertex inputs.
 * Softpipe cannot supply this value, so this mode always requests the hardware screen.
 */
static int
run_fbfetch(const char *path, const char *func)
{
   enum {
      WIDTH = 1206, HEIGHT = 2622,
      SCISSOR_X = 632, SCISSOR_Y = 369, SCISSOR_W = 446, SCISSOR_H = 446,
   };
   const unsigned origin[3] = { 0, 0, 0 };
   const unsigned extent[3] = { WIDTH, HEIGHT, 1 };
   const unsigned read_origin[3] = { SCISSOR_X, SCISSOR_Y, 0 };
   const unsigned read_extent[3] = { SCISSOR_W, SCISSOR_H, 1 };
   const unsigned scissor[4] = { SCISSOR_X, SCISSOR_Y, SCISSOR_W, SCISSOR_H };
   const size_t fill_size = (size_t)WIDTH * HEIGHT * 4;
   const size_t read_size = (size_t)SCISSOR_W * SCISSOR_H * 4;
   struct metallib lib = { 0 };
   struct rwa_device *device = NULL;
   struct rwa_render *render = NULL;
   struct rwa_texture *color = NULL;
   uint8_t *fill = NULL, *got = NULL;
   char why[WHY] = "";
   int result = 1;

   if (!load_metallib(path, &lib))
      goto out;
   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      goto out;
   printf("fbfetch: %s, %zu bytes of bitcode\n", name, bitcode_size);

   setenv("RWA_SOFTWARE", "0", 1);
   device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("fbfetch: FAIL rwa_device_open: %s\n", why);
      goto out;
   }
   printf("fbfetch: device is \"%s\"\n", rwa_device_name(device));
   if (!strcmp(rwa_device_name(device), "softpipe")) {
      printf("fbfetch: FAIL softpipe cannot validate a framebuffer-fetch value\n");
      goto out;
   }

   struct rwa_render_shape shape = {
      .color_count = 1, .sample_count = 1, .color_blend_count = 1,
   };
   shape.color_format[0] = MTL_BGRA8_UNORM;
   shape.color_blend[0] = (struct rwa_color_blend){
      .blending_enabled = 0, .rgb_blend_operation = BO_ADD,
      .alpha_blend_operation = BO_ADD, .source_rgb_blend_factor = BF_ONE,
      .destination_rgb_blend_factor = BF_ZERO, .source_alpha_blend_factor = BF_ONE,
      .destination_alpha_blend_factor = BF_ZERO, .write_mask = CWM_ALL,
   };
   struct rwa_render_info info = { 0 };
   render = rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &shape,
                              &info, why, sizeof(why));
   if (!render) {
      printf("fbfetch: FAIL rwa_render_create: %s\n", why);
      goto out;
   }
   if (info.fragment_buffers || info.fragment_textures || info.fragment_samplers ||
       info.blend_enabled_mask) {
      printf("fbfetch: FAIL unexpected pipeline state: %u buffers, %u textures, "
             "%u samplers, blend mask 0x%x\n", info.fragment_buffers,
             info.fragment_textures, info.fragment_samplers, info.blend_enabled_mask);
      goto out;
   }

   const struct rwa_texture_shape texture_shape = {
      .pixel_format = MTL_BGRA8_UNORM, .type = MTL_TEXTURE_TYPE_2D,
      .width = WIDTH, .height = HEIGHT, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1, .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
   };
   color = rwa_texture_create(device, &texture_shape, why, sizeof(why));
   if (!color) {
      printf("fbfetch: FAIL rwa_texture_create: %s\n", why);
      goto out;
   }
   fill = malloc(fill_size);
   got = malloc(read_size);
   if (!fill || !got) {
      printf("fbfetch: FAIL allocating %zu fill bytes and %zu readback bytes\n",
             fill_size, read_size);
      goto out;
   }
   for (size_t i = 0; i < fill_size; i += 4) {
      fill[i] = fill[i + 1] = fill[i + 2] = 234;
      fill[i + 3] = 255;
   }
   if (rwa_texture_write(color, 0, 0, origin, extent, fill, 0, why, sizeof(why)) != 0) {
      printf("fbfetch: FAIL prefill: %s\n", why);
      goto out;
   }

   const struct rwa_draw draw = {
      .color = &color, .color_count = 1,
      .clear_color = NULL,              /* MTLLoadActionLoad */
      .primitive = 4,                   /* MTLPrimitiveTypeTriangleStrip */
      .vertex_start = 0, .vertex_count = 4,
      .scissor = scissor,
   };
   if (rwa_render_draw(render, &draw, why, sizeof(why)) != 0) {
      printf("fbfetch: FAIL rwa_render_draw: %s\n", why);
      goto out;
   }
   memset(got, 0, read_size);
   if (rwa_texture_read(color, 0, 0, read_origin, read_extent, got, 0, why,
                        sizeof(why)) != 0) {
      printf("fbfetch: FAIL readback: %s\n", why);
      goto out;
   }

   unsigned wrong = 0, zero_rgb = 0;
   for (unsigned y = 0; y < SCISSOR_H; y++) {
      for (unsigned x = 0; x < SCISSOR_W; x++) {
         const uint8_t *pixel = got + ((size_t)y * SCISSOR_W + x) * 4;
         if (pixel[0] == 234 && pixel[1] == 234 && pixel[2] == 234 && pixel[3] == 255)
            continue;
         wrong++;
         zero_rgb += !pixel[0] && !pixel[1] && !pixel[2];
         if (wrong == 1)
            printf("fbfetch: first mismatch at %u,%u: BGRA %u,%u,%u,%u, "
                   "want 234,234,234,255\n", SCISSOR_X + x, SCISSOR_Y + y,
                   pixel[0], pixel[1], pixel[2], pixel[3]);
      }
   }
   printf("fbfetch: %u,%u %ux%u, %u/%u pixels differ, %u zero RGB\n",
          SCISSOR_X, SCISSOR_Y, SCISSOR_W, SCISSOR_H, wrong, SCISSOR_W * SCISSOR_H,
          zero_rgb);
   if (wrong) {
      printf("fbfetch: FAIL expected every scissor pixel to remain BGRA "
             "234,234,234,255\n");
      goto out;
   }
   printf("fbfetch: PASS all %u scissor pixels retained BGRA 234,234,234,255\n",
          SCISSOR_W * SCISSOR_H);
   result = 0;

out:
   free(got);
   free(fill);
   if (color)
      rwa_texture_destroy(color);
   if (render)
      rwa_render_destroy(render);
   if (device)
      rwa_device_close(device);
   free(lib.bytes);
   return result;
}

