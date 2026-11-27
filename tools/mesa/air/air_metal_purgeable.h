#ifndef AIR_METAL_PURGEABLE_H
#define AIR_METAL_PURGEABLE_H

#include <stdbool.h>
#include <stddef.h>

struct rwa_texture;

/* Matches Apple Metal MTLPurgeableState values. */
enum rwa_purgeable_state {
   RWA_PURGEABLE_KEEP_CURRENT = 1,
   RWA_PURGEABLE_NON_VOLATILE = 2,
   RWA_PURGEABLE_VOLATILE = 3,
   RWA_PURGEABLE_EMPTY = 4,
};

unsigned rwa_texture_set_purgeable(struct rwa_texture *texture, unsigned state,
                                   char *why, size_t why_size);
bool rwa_texture_is_purgeable(const struct rwa_texture *texture);

#endif
