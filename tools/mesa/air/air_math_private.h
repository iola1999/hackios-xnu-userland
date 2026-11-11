/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef AIR_MATH_PRIVATE_H
#define AIR_MATH_PRIVATE_H

/* MSL specification (2026-06-04), sections 6.6 and 8.4: cos(pi*x),
 * with <=1 ULP for half and <=4 ULP for float.
 * Reduce before multiplying by pi: half's largest finite inputs are exact
 * even integers, but multiplying them by pi would overflow. Widen half's
 * intermediate operations and use sin around each half-integer so the zero
 * and the nearby small results do not depend on an approximation of pi/2.
 * https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf
 */
static nir_def *
air_cospi(nir_builder *b, nir_def *x)
{
   unsigned bits = x->bit_size;
   nir_def *a = nir_fabs(b, nir_f2f32(b, x));
   nir_def *period = nir_ffloor(b, nir_fmul_imm(b, a, 0.5));
   nir_def *r = nir_fsub(b, a, nir_fmul_imm(b, period, 2.0));
   r = nir_bcsel(b, nir_fge_imm(b, r, 1.0), nir_fsub(b, nir_imm_float(b, 2.0), r), r);
   nir_def *angle = nir_fsub(b, nir_imm_float(b, 0.5), r);
   /* The degree-13 sine series has remainder below 6.7e-10 on this interval.
    * Keep each float32 operation: IR3's generic sin reduction subtracts pi
    * after a rounded multiplication and loses small half-integer results. */
   uint32_t math = b->fp_math_ctrl;
   b->fp_math_ctrl = nir_fp_no_fast_math;
   nir_def *y = nir_fmul_imm(b, angle, M_PI);
   nir_def *z = nir_fmul(b, y, y);
   nir_def *p = nir_imm_float(b, 1.0 / 6227020800.0);
   static const double coefficients[] = {
      -1.0 / 39916800.0, 1.0 / 362880.0, -1.0 / 5040.0, 1.0 / 120.0, -1.0 / 6.0,
   };
   for (unsigned i = 0; i < ARRAY_SIZE(coefficients); i++)
      p = nir_fadd_imm(b, nir_fmul(b, p, z), coefficients[i]);
   nir_def *value = nir_fadd(b, y, nir_fmul(b, nir_fmul(b, y, z), p));
   b->fp_math_ctrl = math;
   value = nir_bcsel(b, nir_feq_imm(b, angle, 0.0), nir_imm_float(b, 0.0), value);
   /* Integer classification preserves the nonfinite result under fast math. */
   value = nir_bcsel(b, nir_ult_imm(b, a, 0x7f800000), value,
                    nir_imm_int(b, 0x7fc00000));
   return nir_f2fN(b, value, bits);
}

#endif
