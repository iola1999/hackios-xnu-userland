/*
 * Purgeable texture state machine and guarded BO reclamation/rebuild for rwa_texture.
 * Included into air_metal.c after air_metal_texture.inc.
 */

static inline unsigned
texture_purgeable_current(const struct rwa_texture *texture)
{
   return texture->purgeable_state ? texture->purgeable_state : RWA_PURGEABLE_NON_VOLATILE;
}

static bool
texture_require_backing(const struct rwa_texture *texture, const char *op,
                        char *why, size_t why_size)
{
   if (!texture || !texture->res) {
      say(why, why_size, "%s: texture backing was purged (MTLPurgeableStateEmpty)",
          op ? op : "texture operation");
      return false;
   }
   return true;
}

static bool
texture_can_purge(const struct rwa_texture *texture, char *why, size_t why_size)
{
   if (!texture)
      return false;
   if (texture->imported) {
      say(why, why_size, "imported IOSurface texture cannot be purged");
      return false;
   }
   if (texture->backing) {
      say(why, why_size, "buffer-backed texture cannot be purged independently");
      return false;
   }
   if (texture->storage_owner) {
      say(why, why_size, "texture view cannot be purged independently");
      return false;
   }
   if (texture->chroma) {
      say(why, why_size, "multi-plane texture cannot be purged");
      return false;
   }
   if (__atomic_load_n(&texture->references, __ATOMIC_RELAXED) > 1) {
      say(why, why_size, "texture with active views cannot be purged");
      return false;
   }
   if (texture->rgb2p_pass_open) {
      say(why, why_size, "texture in an open render pass cannot be purged");
      return false;
   }
   if (!texture->has_res_templ) {
      say(why, why_size, "texture has no private allocation template");
      return false;
   }
   return true;
}

unsigned
rwa_texture_set_purgeable(struct rwa_texture *texture, unsigned state,
                          char *why, size_t why_size)
{
   RWA_LOCKED;
   if (!texture)
      return RWA_PURGEABLE_NON_VOLATILE;
   unsigned prev = texture_purgeable_current(texture);
   if (state == RWA_PURGEABLE_KEEP_CURRENT ||
       state < RWA_PURGEABLE_NON_VOLATILE || state > RWA_PURGEABLE_EMPTY)
      return prev;

   if (state == RWA_PURGEABLE_NON_VOLATILE) {
      if (prev == RWA_PURGEABLE_EMPTY || !texture->res) {
         if (!texture->has_res_templ || !texture->device || !texture->device->screen ||
             !texture->device->screen->resource_create) {
            say(why, why_size, "purged texture cannot be restored without device/template");
            return prev;
         }
         texture->res = texture->device->screen->resource_create(texture->device->screen,
                                                                 &texture->res_templ);
         if (!texture->res) {
            say(why, why_size, "failed to re-allocate purged texture backing");
            return prev;
         }
         texture_start_zeroed(texture->device, texture);
      }
      texture->purgeable_state = RWA_PURGEABLE_NON_VOLATILE;
      return prev;
   }

   if (!texture_can_purge(texture, why, why_size))
      return prev;

   if (state == RWA_PURGEABLE_VOLATILE) {
      /* Metal contract: requesting Volatile on an Empty resource leaves it Empty. */
      if (prev == RWA_PURGEABLE_EMPTY)
         return prev;
      texture->purgeable_state = RWA_PURGEABLE_VOLATILE;
      return prev;
   }

   /* RWA_PURGEABLE_EMPTY: detach from bound state, flush in-flight refs, release BO. */
   if (texture->res) {
      argbuf_scrub(texture->device, NULL, texture, NULL);
      state_texture_gone(texture->device, texture->res);
      if (texture->device->pipe && texture->device->pipe->flush)
         texture->device->pipe->flush(texture->device->pipe, NULL, 0);
      pipe_sampler_view_release_ptr(&texture->view);
      pipe_resource_reference(&texture->rgb2p_sample, NULL);
      pipe_resource_reference(&texture->res, NULL);
   }
   texture->purgeable_state = RWA_PURGEABLE_EMPTY;
   return prev;
}

bool
rwa_texture_is_purgeable(const struct rwa_texture *texture)
{
   RWA_LOCKED;
   if (!texture)
      return false;
   unsigned cur = texture_purgeable_current(texture);
   return cur == RWA_PURGEABLE_VOLATILE || cur == RWA_PURGEABLE_EMPTY;
}
