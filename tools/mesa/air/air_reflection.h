/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef AIR_REFLECTION_H
#define AIR_REFLECTION_H

#include "air_metal_reflection.h"
#include "air_module.h"

struct rwa_compute_reflection *
air_compute_reflection(void *mem_ctx, const struct air_module *module,
                       const struct air_function *function,
                       const struct air_md *inputs, const char **error);

#endif
