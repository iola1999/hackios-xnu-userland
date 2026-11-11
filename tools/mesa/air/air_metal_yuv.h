/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef AIR_METAL_YUV_H
#define AIR_METAL_YUV_H
#include <stddef.h>
#include <stdbool.h>

struct rwa_texture;

/* Retains both planes. Matrix uses ISO/IEC 23091-2 code points: 1 = BT.709,
 * 6 = BT.601, 9 = BT.2020. The caller resolves unspecified metadata before this call.
 * Sampling returns clamped RGB with alpha 1; storage remains biplanar NV12/P010. */
struct rwa_texture *rwa_texture_combine_yuv(struct rwa_texture *y,
                                          struct rwa_texture *uv,
                                          bool full_range, unsigned matrix,
                                          char *why, size_t why_size);
#endif
