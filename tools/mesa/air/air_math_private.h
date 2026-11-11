/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef AIR_MATH_PRIVATE_H
#define AIR_MATH_PRIVATE_H

/* MSL specification (2026-06-04), sections 6.6 and 8.4: cos(pi*x),
 * with <=1 ULP for half.
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
   nir_def *value = nir_fsin(b, nir_fmul_imm(b, angle, M_PI));
   value = nir_bcsel(b, nir_feq_imm(b, angle, 0.0), nir_imm_float(b, 0.0), value);
   /* Integer classification preserves the nonfinite result under fast math. */
   value = nir_bcsel(b, nir_ult_imm(b, a, 0x7f800000), value,
                    nir_imm_int(b, 0x7fc00000));
   return nir_f2fN(b, value, bits);
}

#endif
