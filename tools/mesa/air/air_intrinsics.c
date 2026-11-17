/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * The air.* functions. There are 125 distinct declarations in the corpus but
 * their names are <operation>.<type suffix>, so this is a table of about 50
 * operations in four families (docs/metal.md 9.2). The suffix is not trusted
 * for anything: the types come from the call's own LLVM types, and the suffix
 * is only what picks the handler.
 *
 * What each name means is not guessed: it is read off Apple's own compiler.
 * AGXCompilerCore carries the whole vocabulary as a table of names at
 * 0x16d082..0x16d400 of the library in this device's shared cache (the list is
 * in docs/air.md section 3.2), and that table distinguishes exactly the
 * functions the Metal Shading Language Specification defines -- round from
 * rint, exp from exp2, fmod from mod, pow from powr. Two of this file's
 * handlers were guesses before that table was read; one of them was wrong.
 */

#include "air_private.h"

#include <string.h>

#include "nir_builtin_builder.h"
#include "util/u_math.h"
#include "air_math_private.h"

/* the dot-separated parts of an air.* name, after "air." */
#define AIR_MAX_PARTS 8
/* 48 and not 32: air.normalize_function_constant_predicate's first part is 37
 * characters, and at 32 split_name refused the whole name rather than the
 * operation -- "a name this cannot take apart", which reads like a malformed
 * module and was a buffer. It is the longest part in Apple's own vocabulary
 * list (section 3.2); the longest multi-part name there,
 * air.next_intersection_query.instancing.triangle_data, has no part over 14.
 */
struct air_name {
   unsigned num;
   char part[AIR_MAX_PARTS][48];
};

static bool
split_name(const char *name, struct air_name *out)
{
   out->num = 0;
   out->part[0][0] = 0;
   const char *p = name + 4; /* past "air." */
   /* "air." with nothing after it names no operation. Without this the loop
    * below never runs, split_name returns true all the same, and the
    * dispatcher compares a part[0] nothing wrote -- which gcc points out as
    * soon as the first comparison is made unconditionally.
    */
   if (!*p)
      return false;
   while (*p && out->num < AIR_MAX_PARTS) {
      const char *dot = strchr(p, '.');
      size_t len = dot ? (size_t)(dot - p) : strlen(p);
      if (len >= sizeof(out->part[0]))
         return false;
      memcpy(out->part[out->num], p, len);
      out->part[out->num][len] = 0;
      out->num++;
      if (!dot)
         return true;
      p = dot + 1;
   }
   return out->num < AIR_MAX_PARTS;
}

static bool
is_signed_part(const char *s)
{
   return !strcmp(s, "s");
}

/* Metal's fmod: the remainder of x/y, with the SIGN OF x, correctly rounded.
 *
 * nir_frem is not that function. Its constant expression is, literally,
 *
 *     src0 - src1 * truncf(src0 / src1)                (nir_opcodes.py)
 *
 * and ir3 lowers it to the same three instructions (ir3_compiler.c sets
 * .lower_fmod). Two things are wrong with the formula, and
 * tools/air/intrincheck.py counted both: of 264 cases of air.fmod.f32 it
 * reported 47 wrong, 25 of them a sign and 22 of them a number.
 *
 *   - THE SIGN OF A ZERO IS LOST. fmod(-0, 5) is -0 and fmod(-1.5, 0.5) is
 *     -0, and the subtraction gives +0 for both: -0 - (0.5 * -0) is -0 - -0,
 *     which is +0. This is the shape of the air.sign bug recorded further
 *     down this file -- a sign of zero that a formula does not carry -- and
 *     it is the half of this that is not about precision at all.
 *   - THE PRODUCT IS ROUNDED BEFORE THE SUBTRACTION. y * trunc(x/y) needs up
 *     to 48 significant bits and a float has 24, so the subtraction is exact
 *     but of the wrong product: fmod(1.5, 0x1.fffffep-2) came out 1.19e-07
 *     against 8.94e-08, one whole ulp of the dividend, and fmod(1e8, 3) came
 *     out 0 where the answer is 1. A number, not a sign.
 *
 * Whose definition is followed here, and why it is Metal's: the Metal Shading
 * Language Specification's table entry for fmod gives the same subtraction
 * formula, but it inherits OpenCL's accuracy table along with it, where fmod
 * is 0 ULP -- an exactly specified function, not a licensed approximation --
 * and OpenCL's edge cases are C99 Annex F's, which say fmod(+-0, y) is +-0.
 * Two references on this machine agree with that reading and none with the
 * formula: this machine's libm answers -0 for fmodf(-0, 0.5) and for
 * fmodf(-1.5, 0.5), and LLVM's own `frem` -- JIT-executed by lli, which is
 * the second engine and not a reading of a document -- answers -0 for both
 * and for frem(-2.5, -0.5). The corpus itself says nothing either way: not
 * one of its 128 libraries contains a single `frem` instruction, because
 * Apple's compiler always emits the call.
 *
 * So:
 *
 *   q = trunc(x / y)
 *   r = fma(-y, q, x)        one rounding, not two
 *   if (|r| >= |y|)   r -= copysign(|y|, r)      q was one too small
 *   if (sign r != sign x) r += copysign(|y|, x)  q was one too large
 *   if (r == 0)       r  = copysign(0, x)        a zero keeps x's sign
 *
 * The fma is the whole of the precision argument: when q is an exact integer
 * the true remainder x - q*y is itself exactly representable (that is why
 * IEEE 754 can require `remainder` to be exact), so ONE rounding of it is no
 * rounding at all. The two corrections are there because fl(x/y) can round
 * across an integer and move trunc by one either way, and because ir3 does
 * not divide: it lowers fdiv to fmul(a, frcp(b)) and frcp is approximate, so
 * a quotient off by one has to be survivable rather than assumed away.
 *
 * WHAT THIS STILL DOES NOT DO, measured and not guessed: q must be exactly
 * representable, which means |x/y| < 2^24 for a float and 2^11 for a half.
 * Above that no correction of a bounded size can help -- fmod(1e8, 1e-8)
 * wants a quotient of 1e16 -- and the exact answer needs an iterated
 * reduction: scale y up by 2^k so the partial quotient stays under 2^23,
 * subtract, repeat. Each pass removes only 23 - 1 bits of the exponent
 * difference, that difference reaches 253 for two normal floats, so twelve
 * passes of about ten instructions each would be the price, and its exactness
 * ON THE ADRENO could not be established from this machine at all for the
 * frcp reason above. It is not done here; section 4 of docs/air.md carries
 * the condition and the measurement.
 *
 * Over the reference pool this leaves 7 of 264 cases differing, every one of
 * them with |x/y| >= 2^24; over 195 906 random pairs of normal floats every
 * single disagreement with this machine's libm has |x/y| >= 2^24, and over
 * 376 438 random pairs whose quotient IS representable there is none.
 */
static nir_def *
air_fmod(nir_builder *b, nir_def *x, nir_def *y)
{
   uint64_t sign_bit = 1ull << (x->bit_size - 1);
   /* The sign of a float is taken and planted with integer bit operations and
    * never with a multiply by zero or a copysign built out of one: this
    * builder runs with exact off (Metal's fast library), and
    * nir_opt_algebraic's inexact rule (('~fmul', a, 0.0), 0.0) would throw
    * away exactly the bit being carried. iand and ior cannot be rewritten by
    * a float rule because they are not float operations.
    */
   nir_def *sx = nir_iand_imm(b, x, sign_bit);
   nir_def *ay = nir_fabs(b, y);
   nir_def *y_as_x = nir_ior(b, ay, sx);              /* copysign(|y|, x) */

   nir_def *q = nir_ftrunc(b, nir_fdiv(b, x, y));
   nir_def *r = nir_ffma(b, nir_fneg(b, y), q, x);

   /* |r| >= |y|: the quotient was one too small. A NaN fails this comparison
    * (an ordered fge is false for one) and fails the feq against zero at the
    * end; the middle correction may well fire for a NaN, and adding to a NaN
    * gives a NaN, so a NaN comes out of all three unchanged.
    */
   nir_def *y_as_r = nir_ior(b, ay, nir_iand_imm(b, r, sign_bit));
   r = nir_bcsel(b, nir_fge(b, nir_fabs(b, r), ay), nir_fsub(b, r, y_as_r), r);

   /* r's sign is not x's: the quotient was one too large */
   nir_def *wrong_sign = nir_ine(b, nir_iand_imm(b, r, sign_bit), sx);
   r = nir_bcsel(b, nir_iand(b, wrong_sign, nir_fneu_imm(b, r, 0.0)),
                 nir_fadd(b, r, y_as_x), r);

   /* and a zero result is the dividend's zero */
   return nir_bcsel(b, nir_feq_imm(b, r, 0.0), sx, r);
}

/* -- the families ------------------------------------------------------- */

static bool
math_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n, bool *handled)
{
   const char *op = n->part[0];
   unsigned argc = in->num_operands - 1;

   /* the families this file does not handle are named before the arguments
    * are touched, so that their error says so rather than complaining about
    * a texture handle used as a number
    */
   if (strstr(op, "texture") || strstr(op, "_depth_") || !strncmp(op, "sample", 6) ||
       !strncmp(op, "gather", 6) || !strncmp(op, "read_", 5) || !strncmp(op, "write_", 6) ||
       strstr(op, "_lod_")) {
      *handled = false;
      return true;
   }

   nir_def *a[4] = { 0 };
   for (unsigned i = 0; i < argc && i < 4; i++)
      if (!(a[i] = air_get_ssa(c, in->operands[1 + i])))
         return false;

   bool is_float = air_type_is_float(air_type_scalar(in->base.type));
   /* the sign of an integer operation is the last-but-one part: air.min.s.i32 */
   bool is_signed = n->num >= 3 ? is_signed_part(n->part[1]) : true;
   nir_def *r = NULL;
   *handled = true;

   /* fast math is what every one of these means: Metal's "fast" library */
   uint32_t was_math = c->b.fp_math_ctrl;
   air_builder_set_exact(&c->b, false);

   if (!strcmp(op, "fast_fabs") || !strcmp(op, "fabs")) {
      r = nir_fabs(&c->b, a[0]);
   } else if (!strcmp(op, "fast_sqrt") || !strcmp(op, "sqrt")) {
      r = nir_fsqrt(&c->b, a[0]);
   } else if (!strcmp(op, "fast_rsqrt") || !strcmp(op, "rsqrt")) {
      r = nir_frsq(&c->b, a[0]);
   } else if (!strcmp(op, "fast_floor") || !strcmp(op, "floor")) {
      r = nir_ffloor(&c->b, a[0]);
   } else if (!strcmp(op, "fast_ceil") || !strcmp(op, "ceil")) {
      r = nir_fceil(&c->b, a[0]);
   } else if (!strcmp(op, "fast_round") || !strcmp(op, "round")) {
      /* round() takes a half away from zero; rint() takes it to even, and
       * Apple's compiler has both air.fast_round and air.fast_rint, so this
       * one is not the even one. (It was nir_fround_even here until the
       * vocabulary table said otherwise.)
       *
       * Truncate, then step one away from zero if the remainder reaches a
       * half. Not trunc(x + copysign(0.5, x)): that addition rounds for an x
       * just under a half and answers 1 where the answer is 0. x - trunc(x)
       * is exact for every float, so this needs no magnitude guard, and it
       * keeps the sign of a negative zero. Checked bit-exact against C's
       * roundf over the halves, the sub-halves, both zeros and 2^24.
       */
      nir_def *t = nir_ftrunc(&c->b, a[0]);
      nir_def *d = nir_fabs(&c->b, nir_fsub(&c->b, a[0], t));
      r = nir_bcsel(&c->b, nir_fge_imm(&c->b, d, 0.5),
                    nir_fadd(&c->b, t, nir_fsign(&c->b, a[0])), t);
   } else if (!strcmp(op, "fast_rint") || !strcmp(op, "rint")) {
      r = nir_fround_even(&c->b, a[0]);
   } else if (!strcmp(op, "fast_trunc") || !strcmp(op, "trunc")) {
      r = nir_ftrunc(&c->b, a[0]);
   } else if (!strcmp(op, "fast_fract") || !strcmp(op, "fract")) {
      r = nir_ffract(&c->b, a[0]);
   } else if (!strcmp(op, "fast_sin") || !strcmp(op, "sin")) {
      r = nir_fsin(&c->b, a[0]);
   } else if (!strcmp(op, "fast_cos") || !strcmp(op, "cos")) {
      r = nir_fcos(&c->b, a[0]);
   } else if (!strcmp(op, "cospi") || !strcmp(op, "fast_cospi")) {
      if (argc != 1 || !is_float || (a[0]->bit_size != 16 && a[0]->bit_size != 32))
         return air_error(c, "intrinsic: cospi requires one half or float argument");
      r = air_cospi(&c->b, a[0]);
   } else if (!strcmp(op, "sinpi") || !strcmp(op, "fast_sinpi")) {
      if (argc != 1 || !is_float || (a[0]->bit_size != 16 && a[0]->bit_size != 32))
         return air_error(c, "intrinsic: sinpi requires one half or float argument");
      r = nir_fsin(&c->b, nir_fmul_imm(&c->b, a[0], M_PI));
   } else if (!strcmp(op, "tanpi") || !strcmp(op, "fast_tanpi")) {
      if (argc != 1 || !is_float || (a[0]->bit_size != 16 && a[0]->bit_size != 32))
         return air_error(c, "intrinsic: tanpi requires one half or float argument");
      nir_def *ang = nir_fmul_imm(&c->b, a[0], M_PI);
      r = nir_fdiv(&c->b, nir_fsin(&c->b, ang), nir_fcos(&c->b, ang));
   } else if (!strcmp(op, "fast_tan") || !strcmp(op, "tan")) {
      r = nir_fdiv(&c->b, nir_fsin(&c->b, a[0]), nir_fcos(&c->b, a[0]));
   } else if (!strcmp(op, "fast_atan2") || !strcmp(op, "atan2")) {
      /* Mesa's nir_atan2 is right everywhere but the origin, where its own
       * comment says it deliberately pretends 0/0 = 1 because GLSL licenses an
       * implementation to deviate from IEEE at (0, 0). That gives
       * atan2(0, 0) = 3*pi/4. Metal's atan2 is OpenCL's, which keeps the IEEE
       * values:
       *
       *    atan2(+-0, x > 0) = +-0        atan2(+-0, +0) = +-0
       *    atan2(+-0, x < 0) = +-pi       atan2(+-0, -0) = +-pi
       *
       * so a shader taking the angle of a vector that happens to be zero gets
       * 135 degrees instead of 0. That is not a precision deviation of the
       * kind the fast_ prefix allows, so the origin is handled here and the
       * rest left to Mesa.
       *
       * The sign of a zero cannot be had from a float comparison -- -0 == 0 --
       * so the sign bit is read as an integer. A NIR value is a bit vector and
       * ilt reinterprets it; nir_atan2 avoids this only to match the GLSL
       * front-end, which is not a constraint here.
       */
      nir_def *y = a[0], *x = a[1];
      nir_def *origin = nir_iand(&c->b, nir_feq_imm(&c->b, y, 0.0),
                                 nir_feq_imm(&c->b, x, 0.0));
      nir_def *mag = nir_fmul_imm(&c->b,
                                  nir_b2fN(&c->b, nir_ilt_imm(&c->b, x, 0),
                                           y->bit_size),
                                  M_PI);
      nir_def *at_origin =
         nir_bcsel(&c->b, nir_ilt_imm(&c->b, y, 0), nir_fneg(&c->b, mag), mag);
      r = nir_bcsel(&c->b, origin, at_origin, nir_atan2(&c->b, y, x));
   } else if (!strcmp(op, "fast_asin") || !strcmp(op, "asin") ||
              !strcmp(op, "fast_acos") || !strcmp(op, "acos")) {
      /* The inverse sine and cosine, reduced to atan2, which this file
       * already has and which Mesa implements:
       *
       *    asin(x) = atan2(x, sqrt(1 - x*x))
       *    acos(x) = atan2(sqrt(1 - x*x), x)
       *
       * The reduction is exact rather than an approximation of its own, which
       * is why it is preferred to the polynomial in Mesa's SPIR-V front end
       * (build_asin in vtn_glsl450.c): that one's own comment says it "isn't
       * precise enough to meet half-float precision requirements" and names
       * this formula as the alternative it rejected for cost. Cost is the
       * right thing to spend here -- air.fast_acos is called by 183 of
       * Resident Evil 4's modules and the shape of a terminator's lighting
       * depends on it.
       *
       * (1 - x)*(1 + x) and not 1 - x*x: the two are the same number in exact
       * arithmetic and not in a float. Near |x| = 1 the subtraction of two
       * close values loses most of the significand -- at x = 1 - 2^-20 the
       * product form keeps the result to a few ulp where 1 - x*x has none of
       * the low bits left -- and |x| near 1 is exactly where a normal dotted
       * with a light direction spends its time.
       *
       * The edges fall out of atan2 rather than needing a case: the second
       * argument is a square root and so never negative, both forms reach
       * atan2(0, +-1) = {0, pi} at x = +-1, |x| > 1 takes the root of a
       * negative and gives NaN as the domain demands, and neither form can
       * reach atan2(0, 0) -- x and 1 - x*x are not both zero -- so the origin
       * this file corrects for plain atan2 is unreachable from here.
       */
      nir_def *x = a[0];
      nir_def *root = nir_fsqrt(&c->b,
         nir_fmul(&c->b, nir_fsub(&c->b, nir_imm_floatN_t(&c->b, 1.0, x->bit_size), x),
                  nir_fadd_imm(&c->b, x, 1.0)));
      bool is_asin = !strcmp(op, "fast_asin") || !strcmp(op, "asin");
      r = is_asin ? nir_atan2(&c->b, x, root) : nir_atan2(&c->b, root, x);
   } else if (!strcmp(op, "fast_atan") || !strcmp(op, "atan")) {
      /* one argument: the arc tangent of a ratio already taken, which is
       * Metal's atan(y_over_x) and GLSL's atan(y_over_x) both
       */
      r = nir_atan(&c->b, a[0]);
   } else if (!strcmp(op, "fast_sinh") || !strcmp(op, "sinh")) {
      /* 0.5 * (e^x - e^-x), which is Mesa's own lowering of GLSL sinh
       * (vtn_glsl450.c), with the small-argument case taken out of it.
       *
       * A DIFFERENCE OF TWO EXPONENTIALS HAS NO LOW BITS LEFT NEAR ZERO: at
       * x = 1e-8 both e^x and e^-x round to 1.0 in a float, so the formula
       * answers 0 where sinh(1e-8) is 1e-8 -- not an imprecise answer but a
       * wrong one, and the first thing tools/air/intrincheck.py said about
       * this lowering. sinh(x) = x + x^3/6, so below the x where x^3/6 falls
       * under the type's own precision the argument IS the answer, exactly;
       * 2^-12 is that point for a float (x^2/6 < 2^-24) and 2^-5 for a half.
       * Above it the formula keeps a few thousand ulp of relative error near
       * the threshold, which is what a difference of exponentials costs and
       * what the fast_ prefix licenses.
       */
      unsigned bits = a[0]->bit_size;
      nir_def *big = nir_fge_imm(&c->b, nir_fabs(&c->b, a[0]),
                                 bits > 16 ? 0x1p-12 : 0x1p-5);
      r = nir_bcsel(&c->b, big,
                    nir_fmul_imm(&c->b,
                                 nir_fsub(&c->b, nir_fexp(&c->b, a[0]),
                                          nir_fexp(&c->b, nir_fneg(&c->b, a[0]))), 0.5),
                    a[0]);
   } else if (!strcmp(op, "fast_cosh") || !strcmp(op, "cosh")) {
      r = nir_fmul_imm(&c->b, nir_fadd(&c->b, nir_fexp(&c->b, a[0]),
                                       nir_fexp(&c->b, nir_fneg(&c->b, a[0]))), 0.5);
   } else if (!strcmp(op, "fast_tanh") || !strcmp(op, "tanh")) {
      /* (e^x - e^-x) / (e^x + e^-x), with the two guards Mesa's own lowering
       * of GLSL tanh carries and for the same reasons (vtn_glsl450.c):
       *
       *  - the argument is clamped to +-10, above which e^x overflows while
       *    the answer has been 1.0 to the last bit of a float since |x| = 9;
       *  - and the clamp would turn a NaN into 10 and so a NaN into 1, so the
       *    result is taken only where the argument is large enough and handed
       *    back otherwise. That also keeps tanh(-0) = -0, which the division
       *    would give as +0... and this one is EXACTLY the shape of the
       *    air.sign bug recorded further up this file: a sign of zero that
       *    NIR's folder keeps and a backend need not.
       *
       * The threshold is 2^-12 (2^-5 for a half) and not zero, which is where
       * this differs from Mesa's: tanh(x) = x - x^3/3, so below that point
       * the argument is the answer to the last bit, and above it the
       * difference of exponentials loses the low bits the same way sinh's
       * does. With Mesa's threshold of zero, tanh(1e-8) came back 0.
       */
      unsigned bits = a[0]->bit_size;
      nir_def *x = nir_fmin(&c->b, nir_fmax(&c->b, a[0],
                                            nir_imm_floatN_t(&c->b, bits > 16 ? -10.0 : -4.2, bits)),
                            nir_imm_floatN_t(&c->b, bits > 16 ? 10.0 : 4.2, bits));
      nir_def *ex = nir_fexp(&c->b, x), *enx = nir_fexp(&c->b, nir_fneg(&c->b, x));
      uint32_t was = c->b.fp_math_ctrl;
      air_builder_set_exact(&c->b, true);
      nir_def *regular = nir_fge_imm(&c->b, nir_fabs(&c->b, a[0]),
                                     bits > 16 ? 0x1p-12 : 0x1p-5);
      c->b.fp_math_ctrl = was;
      r = nir_bcsel(&c->b, regular,
                    nir_fdiv(&c->b, nir_fsub(&c->b, ex, enx), nir_fadd(&c->b, ex, enx)),
                    a[0]);
   } else if (!strcmp(op, "fast_exp") || !strcmp(op, "exp")) {
      /* the natural one: Apple's compiler names exp and exp2 separately */
      r = nir_fexp2(&c->b, nir_fmul_imm(&c->b, a[0], M_LOG2E));
   } else if (!strcmp(op, "fast_exp2") || !strcmp(op, "exp2")) {
      r = nir_fexp2(&c->b, a[0]);
   } else if (!strcmp(op, "fast_exp10") || !strcmp(op, "exp10")) {
      r = nir_fexp2(&c->b, nir_fmul_imm(&c->b, a[0], 3.321928094887362)); /* log2(10) */
   } else if (!strcmp(op, "fast_log") || !strcmp(op, "log")) {
      r = nir_fmul_imm(&c->b, nir_flog2(&c->b, a[0]), 1.0 / M_LOG2E);
   } else if (!strcmp(op, "fast_log2") || !strcmp(op, "log2")) {
      r = nir_flog2(&c->b, a[0]);
   } else if (!strcmp(op, "fast_log10") || !strcmp(op, "log10")) {
      r = nir_fmul_imm(&c->b, nir_flog2(&c->b, a[0]), 0.30102999566398120); /* 1/log2(10) */
   } else if (!strcmp(op, "fast_pow") || !strcmp(op, "pow") ||
              !strcmp(op, "fast_powr") || !strcmp(op, "powr")) {
      /* powr is pow with x >= 0 promised, which is what nir_fpow computes */
      r = nir_fpow(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "fast_fmod") || !strcmp(op, "fmod") ||
              !strcmp(op, "fast_mod") || !strcmp(op, "mod")) {
      /* One lowering for the fast form as well as the plain one. Metal's
       * fast-math does license a lower-precision fmod and does license
       * ignoring the sign of a zero -- 131 of the corpus's 139 fmod
       * declarations are air.fast_fmod.f32, so the cheap form would be legal
       * for nearly all of them -- but a reference that is right for one
       * spelling and relaxed for another is a reference nobody can read, and
       * the extra instructions are about a dozen. If the cost ever matters,
       * the one-step form is nir_frem plus the zero's sign, and the condition
       * under which the two differ is written in air_fmod.
       */
      r = air_fmod(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "fast_divide") || !strcmp(op, "divide")) {
      r = nir_fdiv(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "saturate") || !strcmp(op, "fast_saturate")) {
      r = nir_fsat(&c->b, a[0]);
   } else if (!strcmp(op, "sign") && is_float) {
      /* Metal's sign is OpenCL's: sign(-0) is -0, sign(+0) is +0, sign(NaN)
       * is +0. nir_fsign will not promise that -- its own documentation says
       * that for a zero source "there is a preference for the result to have
       * the same sign, but this is not required (it is required by OpenCL)",
       * and likewise for NaN. A backend is therefore licensed to lose it, and
       * softpipe does: it answered +0 for -0 where NIR's own constant folder
       * answered -0, so folding the shader could never have shown this.
       *
       * Built from comparisons instead, which promise what Metal needs:
       * +1 where positive, -1 where negative, 0 for NaN because neither
       * comparison holds, and a zero handed back as it came so it keeps its
       * sign. No immediate is replicated, so this is the same for a vector.
       */
      nir_def *x = a[0];
      nir_def *pos = nir_b2fN(&c->b, nir_fgt_imm(&c->b, x, 0.0), x->bit_size);
      nir_def *neg = nir_b2fN(&c->b, nir_flt_imm(&c->b, x, 0.0), x->bit_size);
      r = nir_bcsel(&c->b, nir_feq_imm(&c->b, x, 0.0), x,
                    nir_fsub(&c->b, pos, neg));
   } else if (!strcmp(op, "sign")) {
      r = nir_isign(&c->b, a[0]);   /* the float case is above */
   } else if (!strcmp(op, "mix")) {
      /* mix(x, y, a) = x + a * (y - x), and that is what Metal defines */
      r = nir_fadd(&c->b, a[0],
                   nir_fmul(&c->b, a[2], nir_fsub(&c->b, a[1], a[0])));
   } else if (!strcmp(op, "dot")) {
      r = nir_fdot(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "fast_fmin") || !strcmp(op, "fmin") || !strcmp(op, "min")) {
      r = is_float ? nir_fmin(&c->b, a[0], a[1])
                   : (is_signed ? nir_imin(&c->b, a[0], a[1]) : nir_umin(&c->b, a[0], a[1]));
   } else if (!strcmp(op, "fast_fmax") || !strcmp(op, "fmax") || !strcmp(op, "max")) {
      r = is_float ? nir_fmax(&c->b, a[0], a[1])
                   : (is_signed ? nir_imax(&c->b, a[0], a[1]) : nir_umax(&c->b, a[0], a[1]));
   } else if (!strcmp(op, "fast_fmin3") || !strcmp(op, "fmin3") || !strcmp(op, "min3")) {
      /* min3, max3 and median3 (MSL 2.1): backboardd's fragment module TimgBadcA2S1Xhfu_Ialp
       * calls air.fmax3.f16, and Core Animation aborts when that pipeline is refused */
      r = is_float ? nir_fmin(&c->b, nir_fmin(&c->b, a[0], a[1]), a[2])
                   : is_signed ? nir_imin(&c->b, nir_imin(&c->b, a[0], a[1]), a[2])
                               : nir_umin(&c->b, nir_umin(&c->b, a[0], a[1]), a[2]);
   } else if (!strcmp(op, "fast_fmax3") || !strcmp(op, "fmax3") || !strcmp(op, "max3")) {
      r = is_float ? nir_fmax(&c->b, nir_fmax(&c->b, a[0], a[1]), a[2])
                   : is_signed ? nir_imax(&c->b, nir_imax(&c->b, a[0], a[1]), a[2])
                               : nir_umax(&c->b, nir_umax(&c->b, a[0], a[1]), a[2]);
   } else if (!strcmp(op, "fast_fmedian3") || !strcmp(op, "fmedian3") || !strcmp(op, "median3")) {
      /* median(x, y, z) = max(min(x, y), min(max(x, y), z)) */
      if (is_float)
         r = nir_fmax(&c->b, nir_fmin(&c->b, a[0], a[1]),
                      nir_fmin(&c->b, nir_fmax(&c->b, a[0], a[1]), a[2]));
      else if (is_signed)
         r = nir_imax(&c->b, nir_imin(&c->b, a[0], a[1]),
                      nir_imin(&c->b, nir_imax(&c->b, a[0], a[1]), a[2]));
      else
         r = nir_umax(&c->b, nir_umin(&c->b, a[0], a[1]),
                      nir_umin(&c->b, nir_umax(&c->b, a[0], a[1]), a[2]));
   } else if (!strcmp(op, "fast_clamp") || !strcmp(op, "clamp")) {
      if (is_float)
         r = nir_fmin(&c->b, nir_fmax(&c->b, a[0], a[1]), a[2]);
      else if (is_signed)
         r = nir_imin(&c->b, nir_imax(&c->b, a[0], a[1]), a[2]);
      else
         r = nir_umin(&c->b, nir_umax(&c->b, a[0], a[1]), a[2]);
   } else if (!strcmp(op, "fast_ldexp") || !strcmp(op, "ldexp")) {
      r = nir_ldexp(&c->b, a[0], nir_i2i32(&c->b, a[1]));
   } else if (!strcmp(op, "fast_fma") || !strcmp(op, "fma") || !strcmp(op, "mad")) {
      /* AFN permits an approximate intrinsic result. Keep strict FMA fused;
       * contract alone only permits fusion of separate multiply/add ops. */
      bool approximate = in->flags & (AIR_FMF_UNSAFE_ALGEBRA | AIR_FMF_AFN);
      air_builder_set_exact(&c->b, !approximate);
      r = approximate ? nir_ffma_weak(&c->b, a[0], a[1], a[2])
                      : nir_ffma(&c->b, a[0], a[1], a[2]);
   } else if (!strcmp(op, "abs")) {
      r = is_float ? nir_fabs(&c->b, a[0]) : nir_iabs(&c->b, a[0]);
   } else if (!strcmp(op, "abs_diff")) {
      r = is_signed ? nir_iabs(&c->b, nir_isub(&c->b, a[0], a[1]))
                    : nir_isub(&c->b, nir_umax(&c->b, a[0], a[1]),
                               nir_umin(&c->b, a[0], a[1]));
   } else if (!strcmp(op, "add_sat")) {
      r = is_signed ? nir_iadd_sat(&c->b, a[0], a[1]) : nir_uadd_sat(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "sub_sat")) {
      r = is_signed ? nir_isub_sat(&c->b, a[0], a[1]) : nir_usub_sat(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "mul_hi")) {
      r = is_signed ? nir_imul_high(&c->b, a[0], a[1]) : nir_umul_high(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "popcount")) {
      r = nir_bit_count(&c->b, a[0]);
   } else if (!strcmp(op, "clz")) {
      r = nir_uclz(&c->b, a[0]);
   } else if (!strcmp(op, "ctz")) {
      /* Metal's ctz returns the type's width when the value is zero */
      nir_def *lsb = nir_find_lsb(&c->b, a[0]);
      r = nir_bcsel(&c->b, nir_ieq_imm(&c->b, a[0], 0),
                    nir_imm_intN_t(&c->b, a[0]->bit_size, a[0]->bit_size), lsb);
   } else if (!strcmp(op, "reverse_bits")) {
      r = nir_bitfield_reverse(&c->b, a[0]);
   } else if (!strcmp(op, "rotate")) {
      r = nir_urol(&c->b, a[0], nir_u2u32(&c->b, a[1]));
   } else if (!strcmp(op, "extract_bits")) {
      r = is_signed ? nir_ibitfield_extract(&c->b, a[0], nir_u2u32(&c->b, a[1]),
                                            nir_u2u32(&c->b, a[2]))
                    : nir_ubitfield_extract(&c->b, a[0], nir_u2u32(&c->b, a[1]),
                                            nir_u2u32(&c->b, a[2]));
   } else if (!strcmp(op, "insert_bits")) {
      r = nir_bitfield_insert(&c->b, a[0], a[1], nir_u2u32(&c->b, a[2]),
                              nir_u2u32(&c->b, a[3]));
   } else if (!strcmp(op, "dfdx") || !strcmp(op, "dfdy") || !strcmp(op, "fwidth")) {
      /* In 32 bits and per pixel, whatever the operand. ir3 issues dsx/dsy as TYPE_F32 on the
       * register it is given, so a half operand is read as half of a float: the dada port
       * measured fwidth.f16 as inf/NaN on the Adreno 830 where Apple's GPU gives 0.5
       * (glass_background_sdf_lph takes its edge width this way). Apple differentiates per
       * pixel; ir3's plain dsx/dsy are per quad, which put fwidth-antialiased edges in a
       * different place. AIR_COARSE_DERIVATIVES=1 restores the old form, for a comparison. */
      bool coarse = getenv("AIR_COARSE_DERIVATIVES") != NULL;
      nir_def *x = a[0]->bit_size == 32 || coarse ? a[0] : nir_f2f32(&c->b, a[0]);
      nir_def *dx = coarse ? nir_ddx(&c->b, x) : nir_ddx_fine(&c->b, x);
      nir_def *dy = coarse ? nir_ddy(&c->b, x) : nir_ddy_fine(&c->b, x);

      if (!strcmp(op, "dfdx"))
         r = dx;
      else if (!strcmp(op, "dfdy"))
         r = dy;
      else
         r = nir_fadd(&c->b, nir_fabs(&c->b, dx), nir_fabs(&c->b, dy));
      if (r->bit_size != a[0]->bit_size)
         r = nir_f2fN(&c->b, r, a[0]->bit_size);
   } else if (!strcmp(op, "get_simd_width") || !strcmp(op, "get_thread_execution_width")) {
      r = nir_load_subgroup_size(&c->b);
   } else if (!strcmp(op, "get_local_size")) {
      nir_def *wg = nir_load_workgroup_size(&c->b);
      nir_def *ch = nir_vector_extract(&c->b, wg, nir_umin_imm(&c->b, nir_u2u32(&c->b, a[0]), 2));
      r = nir_u2uN(&c->b, ch, air_nir_bit_size(c, in->base.type));
   } else if (!strcmp(op, "pack") && n->num >= 2 &&
              (!strcmp(n->part[1], "snorm2x16") || !strcmp(n->part[1], "unorm2x16") ||
               !strcmp(n->part[1], "snorm4x8") || !strcmp(n->part[1], "unorm4x8"))) {
      nir_def *f = nir_f2f32(&c->b, a[0]);
      r = !strcmp(n->part[1], "snorm2x16") ? nir_pack_snorm_2x16(&c->b, f) :
          !strcmp(n->part[1], "unorm2x16") ? nir_pack_unorm_2x16(&c->b, f) :
          !strcmp(n->part[1], "snorm4x8")  ? nir_pack_snorm_4x8(&c->b, f)  :
                                             nir_pack_unorm_4x8(&c->b, f);
   } else if (!strcmp(op, "pack") && n->num >= 3 &&
              !strcmp(n->part[1], "unorm") && !strcmp(n->part[2], "rgb10a2")) {
      nir_def *f = nir_fsat(&c->b, nir_f2f32(&c->b, a[0]));
      nir_def *rc = nir_f2u32(&c->b, nir_ffloor(&c->b, nir_fadd_imm(&c->b, nir_fmul_imm(&c->b, nir_channel(&c->b, f, 0), 1023.0), 0.5)));
      nir_def *gc = nir_f2u32(&c->b, nir_ffloor(&c->b, nir_fadd_imm(&c->b, nir_fmul_imm(&c->b, nir_channel(&c->b, f, 1), 1023.0), 0.5)));
      nir_def *bc = nir_f2u32(&c->b, nir_ffloor(&c->b, nir_fadd_imm(&c->b, nir_fmul_imm(&c->b, nir_channel(&c->b, f, 2), 1023.0), 0.5)));
      nir_def *ac = nir_f2u32(&c->b, nir_ffloor(&c->b, nir_fadd_imm(&c->b, nir_fmul_imm(&c->b, nir_channel(&c->b, f, 3), 3.0), 0.5)));
      r = nir_ior(&c->b, nir_ior(&c->b, rc, nir_ishl_imm(&c->b, gc, 10)),
                  nir_ior(&c->b, nir_ishl_imm(&c->b, bc, 20), nir_ishl_imm(&c->b, ac, 30)));
   } else if (!strcmp(op, "all")) {
      r = nir_ball(&c->b, a[0]);
   } else if (!strcmp(op, "any")) {
      r = nir_bany(&c->b, a[0]);
   } else if (!strcmp(op, "simd_sum")) {
      r = nir_reduce(&c->b, a[0],
                     .reduction_op = is_float ? nir_op_fadd : nir_op_iadd);
   } else if (!strcmp(op, "simd_product")) {
      r = nir_reduce(&c->b, a[0],
                     .reduction_op = is_float ? nir_op_fmul : nir_op_imul);
   } else if (!strcmp(op, "simd_min")) {
      r = nir_reduce(&c->b, a[0],
                     .reduction_op = is_float ? nir_op_fmin : (is_signed ? nir_op_imin : nir_op_umin));
   } else if (!strcmp(op, "simd_max")) {
      r = nir_reduce(&c->b, a[0],
                     .reduction_op = is_float ? nir_op_fmax : (is_signed ? nir_op_imax : nir_op_umax));
   } else if (!strcmp(op, "simd_and") || !strcmp(op, "simd_or") || !strcmp(op, "simd_xor")) {
      nir_op rop = !strcmp(op, "simd_and") ? nir_op_iand :
                   !strcmp(op, "simd_or")  ? nir_op_ior  : nir_op_ixor;
      r = nir_reduce(&c->b, a[0], .reduction_op = rop);
   } else if (!strcmp(op, "simd_prefix_inclusive_sum") ||
              !strcmp(op, "simd_prefix_exclusive_sum")) {
      nir_op rop = is_float ? nir_op_fadd : nir_op_iadd;
      r = !strcmp(op, "simd_prefix_inclusive_sum")
         ? nir_inclusive_scan(&c->b, a[0], .reduction_op = rop)
         : nir_exclusive_scan(&c->b, a[0], .reduction_op = rop);
   } else if (!strcmp(op, "simd_prefix_inclusive_product") ||
              !strcmp(op, "simd_prefix_exclusive_product")) {
      nir_op rop = is_float ? nir_op_fmul : nir_op_imul;
      r = !strcmp(op, "simd_prefix_inclusive_product")
         ? nir_inclusive_scan(&c->b, a[0], .reduction_op = rop)
         : nir_exclusive_scan(&c->b, a[0], .reduction_op = rop);
   } else if (!strcmp(op, "normalize_function_constant_predicate")) {
      /* Metal's compiler stores the predicates that say whether a dependent
       * function constant applies in `__metal_implicit_fc_pred_N` bytes, and
       * puts every one through this before storing it. The name is in Apple's
       * own vocabulary (it sits between air.nextafter and air.pack.snorm1x8 in
       * AGXCompilerCore's list of operation names, section 3.2), but no string
       * or symbol in that binary says what it computes, so the reading here is
       * C's: a byte is a predicate if it is not zero.
       *
       * THE CORPUS CANNOT TELL THAT APART FROM THE IDENTITY, and saying so is
       * the point: every argument it is given in these modules is a
       * `zext i1 ... to i8`, already 0 or 1, and any function that agrees with
       * the identity on {0, 1} gives the same answer. A byte of 2 would
       * discriminate, and nothing produces one here -- so this is a reading
       * that no measurement available on this machine can confirm or refute,
       * and a shader that fed it a wider byte would be the first test of it.
       */
      r = nir_b2iN(&c->b, nir_ine_imm(&c->b, a[0], 0),
                   air_nir_bit_size(c, in->base.type));
   } else if (!strcmp(op, "unpack") && n->num >= 2 &&
              !strcmp(n->part[1], "unorm4x8")) {
      r = nir_unpack_unorm_4x8(&c->b, a[0]);
      if (air_nir_bit_size(c, in->base.type) == 16)
         r = nir_f2f16(&c->b, r);
   } else if (!strcmp(op, "unpack") && n->num >= 3 &&
              !strcmp(n->part[1], "unorm") && !strcmp(n->part[2], "rgb10a2")) {
      nir_def *w = nir_u2u32(&c->b, a[0]);
      nir_def *rc = nir_fmul_imm(&c->b, nir_u2f32(&c->b, nir_iand_imm(&c->b, w, 0x3ff)), 1.0 / 1023.0);
      nir_def *gc = nir_fmul_imm(&c->b, nir_u2f32(&c->b, nir_ubitfield_extract_imm(&c->b, w, 10, 10)), 1.0 / 1023.0);
      nir_def *bc = nir_fmul_imm(&c->b, nir_u2f32(&c->b, nir_ubitfield_extract_imm(&c->b, w, 20, 10)), 1.0 / 1023.0);
      nir_def *ac = nir_fmul_imm(&c->b, nir_u2f32(&c->b, nir_ushr_imm(&c->b, w, 30)), 1.0 / 3.0);
      r = nir_vec4(&c->b, rc, gc, bc, ac);
      if (air_nir_bit_size(c, in->base.type) == 16)
         r = nir_f2f16(&c->b, r);
   } else if (!strcmp(op, "convert")) {
      /* air.convert.<dst sign>.<dst type>.<src sign>.<src type> */
      if (n->num < 5) {
         c->b.fp_math_ctrl = was_math;
         return air_error(c, "intrinsic: %s (a convert without both types)", op);
      }
      const struct air_type *dt = in->base.type;
      const struct air_type *st = in->operands[1]->type;
      unsigned bits = air_nir_bit_size(c, dt);
      bool dst_float = n->part[1][0] == 'f';
      bool src_float = n->part[3][0] == 'f';
      bool src_signed = n->part[3][0] == 's';
      nir_def *x = a[0];
      if (!src_float && air_type_bit_size(air_type_scalar(st)) == 1)
         x = src_signed ? nir_ineg(&c->b, nir_b2i32(&c->b, x)) : nir_b2i32(&c->b, x);
      /* A BOOL DESTINATION IS NOT A CONVERSION. C's rule, which MSL inherits,
       * is `x != 0`, and in NIR a boolean is a 1-bit def -- so asking
       * nir_f2uN for one bit produces f2u1, an opcode ir3 has no instruction
       * for and no lowering of. That is the whole of why
       * variable_blur_frag_{lpf,lph} were the only two modules of QuartzCore's
       * library to reach the backend and die there: "Unhandled ALU op: f2u1",
       * from air.convert.u.i1.f.f32. The comparison is also the right answer
       * rather than a way round the backend: a conversion would have a
       * rounding mode, and bool(0.5) is true, not false.
       */
      if (bits == 1)
         r = src_float ? nir_fneu_imm(&c->b, x, 0.0) : nir_ine_imm(&c->b, x, 0);
      else if (dst_float && src_float)
         r = nir_f2fN(&c->b, x, bits);
      else if (dst_float)
         r = src_signed ? nir_i2fN(&c->b, x, bits) : nir_u2fN(&c->b, x, bits);
      else if (src_float)
         r = n->part[1][0] == 's' ? nir_f2iN(&c->b, x, bits) : nir_f2uN(&c->b, x, bits);
      else
         r = src_signed ? nir_i2iN(&c->b, x, bits) : nir_u2uN(&c->b, x, bits);
   } else {
      *handled = false;
   }

   c->b.fp_math_ctrl = was_math;
   if (!*handled)
      return true;
   if (!r)
      return false;
   air_set(c, &in->base, air_val_ssa(c, r));
   return true;
}

/* -- function constants ------------------------------------------------- */

/* air.is_function_constant_defined(ptr to the constant's global)
 *
 * Metal's companion to `[[function_constant(n)]]`: a shader may ask whether
 * the pipeline supplied a value for a constant, and guard the code that reads
 * it. The answer is known at translation time -- it is exactly whether the
 * caller passed a value for that index in air_to_nir_options -- so this folds
 * to a constant and the guarded branch folds away with it.
 *
 * It is handled here, before math_op, because its argument is a POINTER and
 * math_op resolves every argument as a number: the four fixed_vert_* modules
 * stopped at "a pointer where a number belongs" with no instruction named,
 * because the failure was in reading the arguments and not in the operation.
 */
static bool
fc_defined_op(struct air_ctx *c, const struct air_inst *in)
{
   if (in->num_operands != 2)
      return air_error(c, "intrinsic: air.is_function_constant_defined of %u arguments",
                       in->num_operands - 1);
   struct air_val *p = air_get(c, in->operands[1]);
   if (!p)
      return false;
   if (p->kind != AIR_VAL_PTR || p->ptr.class != AIR_PTR_FUNCTION_CONSTANT)
      return air_error(c, "intrinsic: air.is_function_constant_defined of something "
                          "that is not a function constant");
   bool defined = false;
   for (unsigned i = 0; c->options && i < c->options->num_constants; i++)
      if (c->options->constants[i].index == p->ptr.binding)
         defined = true;
   air_set(c, &in->base, air_val_ssa(c, nir_imm_bool(&c->b, defined)));
   return true;
}

/* -- atomics ------------------------------------------------------------ */

static bool
atomic_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n)
{
   /* air.atomic.{global,local}.<op>[.<sign>].<type>(ptr, value?, order,
    * scope, volatile)
    */
   if (n->num == 2 && !strcmp(n->part[1], "fence")) {
      uint64_t flags = 0, scope_arg = 2;
      if (in->num_operands > 1)
         air_const_uint(in->operands[1], &flags);
      if (in->num_operands > 3)
         air_const_uint(in->operands[3], &scope_arg);
      nir_variable_mode modes = 0;
      if (flags & 1)
         modes |= nir_var_mem_ssbo | nir_var_mem_global;
      if (flags & (2 | 8))
         modes |= nir_var_mem_shared;
      if (flags & 4)
         modes |= nir_var_image;
      if (!modes)
         modes = nir_var_mem_ssbo | nir_var_mem_global | nir_var_mem_shared;
      nir_barrier(&c->b, .execution_scope = SCOPE_NONE,
                  .memory_scope = scope_arg == 1 ? SCOPE_DEVICE : SCOPE_WORKGROUP,
                  .memory_semantics = NIR_MEMORY_ACQ_REL,
                  .memory_modes = modes);
      return true;
   }
   if (n->num < 3)
      return air_error(c, "intrinsic: an atomic without a name");
   const char *where = n->part[1];
   const char *op = n->part[2];
   bool is_signed = n->num >= 5 ? is_signed_part(n->part[3]) : true;

   struct air_val *pv = air_get(c, in->operands[1]);
   if (!pv)
      return false;
   if (pv->kind != AIR_VAL_PTR)
      return air_error(c, "intrinsic: an atomic on something that is not a pointer");
   struct air_ptr p = pv->ptr;

   bool shared = !strcmp(where, "local");
   if (shared != (p.class == AIR_PTR_SHARED))
      return air_error(c, "intrinsic: air.atomic.%s on %s memory", where,
                       p.class == AIR_PTR_SHARED ? "threadgroup" : "device");

   nir_def *off = p.offset ? p.offset : nir_imm_int(&c->b, 0);

   /* Compare-exchange, refused before the memory order is read -- because its
    * arguments are in different places and reading them as the others' gave a
    * sentence about the wrong thing. Its signature, decoded from the call
    * sites so that implementing it needs no second reading:
    *
    *   i32 air.atomic.<global|local>.cmpxchg.weak.i32(
    *          ptr object, ptr expected, i32 desired,
    *          i32 success_order, i32 failure_order, i32 scope, i1 volatile)
    *
    * so the order this used to read as argument 3 is the DESIRED VALUE, which
    * is not a constant, and 4 modules of Resident Evil 4 were told "with a
    * memory order that is not a constant" about an operation this does not
    * implement at all.
    *
    * THE RESULT IS THE OBJECT'S OLD VALUE and not a boolean, which is read off
    * what the callers do with it rather than from the name: Metal's
    * atomic_compare_exchange_weak_explicit returns bool, and every call site
    * here instead feeds the result to `icmp ne i32 %expected, %result` and
    * then re-loads *expected -- the weak-CAS retry loop HLSL's
    * InterlockedCompareExchange compiles to. A lowering is three operations
    * (load *expected, nir_atomic_op_cmpxchg, store the old value back through
    * `expected`, whose conditional-on-failure store may be unconditional
    * because on success it writes the value already there), and what stops it
    * is not the operation but the check: nothing in tools/air/atomiccheck.py
    * can make a wrong compare-exchange show, and an atomic that is only
    * compiled is the one kind this project will not claim.
    */
   if (!strcmp(op, "cmpxchg")) {
      if (in->num_operands < 6)
         return air_error(c, "intrinsic: air.atomic.%s.cmpxchg with %u arguments",
                          where, in->num_operands - 1);
      uint64_t sorder = 0, forder = 0;
      if (!air_const_uint(in->operands[4], &sorder) ||
          !air_const_uint(in->operands[5], &forder))
         return air_error(c, "intrinsic: air.atomic.%s.cmpxchg with a memory order "
                             "that is not a constant", where);
      if (sorder != 0 || forder != 0)
         return air_error(c, "intrinsic: air.atomic.%s.cmpxchg with memory_order "
                             "%u/%u; only relaxed (0) is translated",
                          where, (unsigned)sorder, (unsigned)forder);
      struct air_val *ev = air_get(c, in->operands[2]);
      nir_def *desired = air_get_ssa(c, in->operands[3]);
      if (!ev || !desired)
         return false;
      if (ev->kind != AIR_VAL_PTR)
         return air_error(c, "intrinsic: air.atomic.%s.cmpxchg expected argument "
                             "is not a pointer", where);
      nir_def *cmp = air_load_ptr(c, ev->ptr, in->base.type, 0);
      if (!cmp)
         return false;
      nir_def *old;
      if (shared) {
         old = nir_shared_atomic_swap(&c->b, desired->bit_size, off, cmp, desired,
                                      .atomic_op = nir_atomic_op_cmpxchg);
      } else if (p.class == AIR_PTR_SSBO) {
         old = nir_ssbo_atomic_swap(&c->b, desired->bit_size,
                                    nir_imm_int(&c->b, p.binding), off, cmp, desired,
                                    .atomic_op = nir_atomic_op_cmpxchg);
      } else {
         return air_error(c, "intrinsic: an atomic on a pointer that is not a buffer");
      }
      if (!air_store_ptr(c, ev->ptr, old, 0))
         return false;
      if (in->base.type && in->base.type->kind != AIR_TYPE_VOID)
         air_set(c, &in->base, air_val_ssa(c, old));
      return true;
   }

   /* The memory order, which this lowering is right for exactly one value of.
    * That argument is Metal's memory_order, and every atomic in the corpus
    * passes 0 -- memory_order_relaxed. A relaxed load or store of an aligned
    * 32-bit word is a plain load or store, which is what happens below, and a
    * relaxed read-modify-write needs no barrier either. A stronger order would
    * need one that is not emitted, so rather than ignore the argument and be
    * quietly wrong on a shader that asks for more, refuse it by name. Read
    * through air_const_uint because a zero arrives as CST_CODE_NULL about as
    * often as as an integer.
    */
   unsigned iorder = !strcmp(op, "load") ? 2 : 3;
   uint64_t order;
   if (in->num_operands <= iorder ||
       !air_const_uint(in->operands[iorder], &order))
      return air_error(c, "intrinsic: air.atomic.%s.%s with a memory order that is "
                          "not a constant", where, op);
   if (order != 0)
      return air_error(c, "intrinsic: air.atomic.%s.%s with memory_order %u; only "
                          "relaxed (0) is translated, and a stronger one needs a "
                          "barrier this does not emit", where, op, (unsigned)order);

   if (!strcmp(op, "load")) {
      nir_def *r = air_load_ptr(c, p, in->base.type, 0);
      if (!r)
         return false;
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }
   if (!strcmp(op, "store")) {
      nir_def *v = air_get_ssa(c, in->operands[2]);
      return v && air_store_ptr(c, p, v, 0);
   }

   nir_atomic_op aop;
   if (!strcmp(op, "add")) aop = nir_atomic_op_iadd;
   else if (!strcmp(op, "min")) aop = is_signed ? nir_atomic_op_imin : nir_atomic_op_umin;
   else if (!strcmp(op, "max")) aop = is_signed ? nir_atomic_op_imax : nir_atomic_op_umax;
   else if (!strcmp(op, "and")) aop = nir_atomic_op_iand;
   else if (!strcmp(op, "or")) aop = nir_atomic_op_ior;
   else if (!strcmp(op, "xor")) aop = nir_atomic_op_ixor;
   else if (!strcmp(op, "xchg")) aop = nir_atomic_op_xchg;
   else return air_error(c, "intrinsic: atomic %s", op);

   nir_def *v = air_get_ssa(c, in->operands[2]);
   if (!v)
      return false;
   nir_def *r;
   if (shared) {
      r = nir_shared_atomic(&c->b, v->bit_size, off, v, .atomic_op = aop);
   } else if (p.class == AIR_PTR_SSBO) {
      r = nir_ssbo_atomic(&c->b, v->bit_size, nir_imm_int(&c->b, p.binding), off, v,
                          .atomic_op = aop);
   } else {
      return air_error(c, "intrinsic: an atomic on a pointer that is not a buffer");
   }
   if (in->base.type && in->base.type->kind != AIR_TYPE_VOID)
      air_set(c, &in->base, air_val_ssa(c, r));
   return true;
}

/* -- barriers ----------------------------------------------------------- */

static bool
barrier_op(struct air_ctx *c, const struct air_inst *in, mesa_scope scope)
{
   /* air.{wg,simdgroup}.barrier(i32 flags, i32 scope): the flags say which
    * memory is ordered. 2 is threadgroup memory, 1 device, 4 texture, 0 none.
    */
   uint64_t flags = scope == SCOPE_WORKGROUP ? 2 : 0;
   bool have_flags = in->num_operands > 1 && air_const_uint(in->operands[1], &flags);

   nir_variable_mode modes = 0;
   if (flags & 1)
      modes |= nir_var_mem_ssbo | nir_var_mem_global;
   if (flags & (2 | 8))
      modes |= nir_var_mem_shared;
   if (flags & 4)
      modes |= nir_var_image;
   if (!modes && scope == SCOPE_WORKGROUP && (!have_flags || flags != 0))
      modes = nir_var_mem_shared;

   nir_barrier(&c->b, .execution_scope = scope,
               .memory_scope = scope,
               .memory_semantics = modes ? NIR_MEMORY_ACQ_REL : 0,
               .memory_modes = modes);
   return true;
}

static bool
sincos_op(struct air_ctx *c, const struct air_inst *in)
{
   if (in->num_operands != 3)
      return air_error(c, "intrinsic: air.sincos with %u arguments",
                       in->num_operands - 1);
   nir_def *x = air_get_ssa(c, in->operands[1]);
   struct air_val *cp = air_get(c, in->operands[2]);
   if (!x || !cp)
      return false;
   if (cp->kind != AIR_VAL_PTR)
      return air_error(c, "intrinsic: air.sincos cosine output is not a pointer");
   uint32_t was_math = c->b.fp_math_ctrl;
   air_builder_set_exact(&c->b, false);
   nir_def *s = nir_fsin(&c->b, x);
   nir_def *cs = nir_fcos(&c->b, x);
   c->b.fp_math_ctrl = was_math;
   if (!air_store_ptr(c, cp->ptr, cs, 0))
      return false;
   air_set(c, &in->base, air_val_ssa(c, s));
   return true;
}

/* -- fragment flow control ---------------------------------------------- */

/* air.discard_fragment()
 *
 * NIR'S `demote` AND NOT NIR'S `terminate`, and that choice is the whole of
 * this function. The two differ in what the thread does afterwards: terminate
 * ends the invocation, so the lane stops contributing to its quad's
 * derivatives and performs no further memory writes, while demote turns it
 * into a HELPER invocation -- it keeps executing, its framebuffer and memory
 * writes are dropped, and it still supplies the neighbour values that a
 * derivative or an implicit-level-of-detail sample in the OTHER three lanes
 * of the quad needs.
 *
 * It is a real distinction in NIR and in every backend that has one: ir3
 * keeps the two apart in its own IR (ir3_compiler_nir.c emits ir3_DEMOTE
 * against ir3_KILL, and they carry different scheduling barriers), and
 * nir_to_tgsi sends them to TGSI's DEMOTE and KILL, which are separate
 * opcodes. ONE PLACE COLLAPSES THEM, AND IT IS WORTH KNOWING ABOUT: the a6xx
 * instruction encoder maps DEMOTE onto the hardware's KILL --
 * `} else if (instr->opc == OPC_DEMOTE) { return OPC_KILL; }` in
 * src/freedreno/isa/encode.c -- and the ISA description has no demote bitset
 * at all (ir3-cat0.xml carries `kill` and nothing beside it). So on this GPU,
 * with this Mesa, both come out as one instruction and the helper lanes Metal
 * promises are not delivered by the hardware path today. That is a gap to
 * name, not a reason to translate this as `terminate`: terminate would lose
 * the same lanes and would be wrong on a backend that does tell them apart.
 * What it costs is measurable only on the phone -- a shader that discards and
 * then takes a derivative -- and that is written as a job rather than assumed.
 *
 * ONE THING IS NOT A DIFFERENCE BETWEEN THE TWO, and saying so is the
 * correction of a sentence that used to stand here: stores. NIR's own
 * definition of demote (nir_intrinsics.py) is "Any stores to memory after
 * this instruction are suppressed and the fragment does not write outputs to
 * the framebuffer. Unlike discard, demote needs to ensure that derivatives
 * will still work for invocations that were not demoted." So demote drops the
 * lane's later writes as well -- it differs from terminate in the derivatives
 * and in the lane going on executing, not in the memory. Whether METAL drops
 * them is a third question, and nothing on this machine answers it: no header
 * on the device says, and a corpus cannot show it. If Metal keeps them, both
 * of NIR's intrinsics are wrong about it by the same amount.
 *
 * Nothing in a corpus COUNT can decide it -- both translate, both validate,
 * both compile -- so it is decided from two independent readings of what
 * Metal means, and both say the thread lives:
 *
 *  1. The shaders themselves. Resident Evil 4's `PS_DrawByTiles` calls
 *     air.discard_fragment at the ninth instruction of a 1 800-line function
 *     and then calls `air.simd_is_helper_thread()` EIGHT TIMES further down
 *     it. A terminated thread cannot be asked whether it is a helper thread;
 *     a demoted one answers yes, which is the only reading under which that
 *     shader means anything. 24 of the library's 1 288 discard-calling modules
 *     call that function.
 *  2. Apple's own declaration of it. `llvm-dis` of that module reads
 *     `declare void @air.discard_fragment() local_unnamed_addr #8` with
 *     `attributes #8 = { nounwind }` -- and NOT `noreturn`. LLVM is entitled
 *     to delete everything after a call it is told does not return, and
 *     Apple's compiler kept the rest of the function; the call is followed by
 *     `br` and not `unreachable` in 73 of 85 call sites sampled. A discard
 *     that killed the thread would be a noreturn call.
 *
 * And the cost of getting it wrong is not hypothetical here: of 85 sampled
 * call sites, 45 are followed in the same function by an air.sample_* with an
 * implicit level of detail and 6 by air.dfdx, both of which read the other
 * lanes of the quad. Under `terminate` those neighbours would read a dead
 * lane.
 *
 * uses_discard is set by hand because nir_shader_gather_info does not run on
 * this path (ir3 calls it only for a binning variant), exactly as the
 * framebuffer-fetch flags are set in air_to_nir.c. It is not bookkeeping: it
 * is what nir_to_tgsi_info turns into `uses_kill`, which is what a Gallium
 * driver reads before it allows an early depth test, and what freedreno reads
 * before it writes LRZ. A discarding shader whose interface says it discards
 * nothing gets its depth written for pixels it threw away.
 */
static bool
discard_op(struct air_ctx *c, const struct air_inst *in)
{
   if (c->nir->info.stage != MESA_SHADER_FRAGMENT)
      return air_error(c, "intrinsic: air.discard_fragment in a %s shader",
                       _mesa_shader_stage_to_string(c->nir->info.stage));
   nir_demote(&c->b);
   c->nir->info.fs.uses_discard = true;
   return true;
}

#include "air_simd.inc"
#include "air_llvm.inc"
#include "air_texture.inc"
#include "air_imageblock.inc"

/* -- the dispatcher ----------------------------------------------------- */

bool
air_translate_call(struct air_ctx *c, const struct air_inst *in, const char *name)
{
   if (!strncmp(name, "llvm.", 5))
      return llvm_op(c, in, name);

   if (strncmp(name, "air.", 4))
      return air_call_in_module(c, in, name);

   struct air_name n;
   if (!split_name(name, &n))
      return air_error(c, "intrinsic: %s (a name this cannot take apart)", name);

   if (!strcmp(name, "air.get_read_sampler"))
      return read_sampler_op(c, in);

   if (!strcmp(n.part[0], "is_function_constant_defined"))
      return fc_defined_op(c, in);
   if (n.num >= 2 && !strcmp(n.part[0], "wg") && !strcmp(n.part[1], "barrier"))
      return barrier_op(c, in, SCOPE_WORKGROUP);
   if (n.num >= 2 && !strcmp(n.part[0], "simdgroup") && !strcmp(n.part[1], "barrier"))
      return barrier_op(c, in, SCOPE_SUBGROUP);
   if (!strcmp(n.part[0], "sincos") || !strcmp(n.part[0], "fast_sincos"))
      return sincos_op(c, in);
   if (!strcmp(n.part[0], "atomic"))
      return atomic_op(c, in, &n);
   if (!strcmp(n.part[0], "discard_fragment"))
      return discard_op(c, in);

   bool handled = false;
   if (!air_imageblock_op(c, in, &n, &handled))
      return false;
   if (handled)
      return true;

   /* before math_op, which resolves every argument as a number: these take a
    * lane number or nothing at all, and air.simd_sum falls through to it
    */
   if (!simd_op(c, in, &n, &handled))
      return false;
   if (handled)
      return true;

   if (!math_op(c, in, &n, &handled))
      return false;
   if (handled)
      return true;

   const char *op = n.part[0];
   if (strstr(op, "texture") || strstr(op, "_depth_") || !strncmp(op, "sample", 6) ||
       !strncmp(op, "gather", 6) || !strncmp(op, "read_", 5) || !strncmp(op, "write_", 6) ||
       strstr(op, "_lod_"))
      return texture_op(c, in, &n);

   return air_error(c, "intrinsic: %s", name);
}

/*
 * What nir_shader_gather_info records about helper invocations, which nothing on this path
 * runs (air_to_nir.c sets the other fields it would have set by hand). ir3 turns these two
 * into the A830's PIXLODENABLE and LODPIXMASK; without them a derivative or an implicit
 * level of detail reads the lanes of a 2x2 quad that the primitive does not cover, and
 * those lanes did not run. The rules are nir_gather_info.c's: plain ddx and ddy count as
 * coarse, any other quad operation needs the full quad.
 */
void
air_gather_quad_helpers(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT)
      return;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex &&
                nir_tex_instr_has_implicit_derivative(nir_instr_as_tex(instr)))
               nir->info.fs.needs_coarse_quad_helper_invocations = true;
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (!nir_intrinsic_has_semantic(intr, NIR_INTRINSIC_QUADGROUP))
               continue;
            nir->info.fs.needs_coarse_quad_helper_invocations = true;
            if (intr->intrinsic != nir_intrinsic_ddx && intr->intrinsic != nir_intrinsic_ddy &&
                intr->intrinsic != nir_intrinsic_ddx_coarse &&
                intr->intrinsic != nir_intrinsic_ddy_coarse)
               nir->info.fs.needs_full_quad_helper_invocations = true;
         }
      }
   }
}

#include "air_debug.inc"
