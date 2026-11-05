/*
 * In-process vertex stage CSO caching and reuse for libredwoodair.
 * CoreAnimation and backboardd share identical vertex shaders (e.g. VfxU10Xh, VfxU11Xh)
 * across multiple render pipelines with different fragment materials. Reusing the compiled
 * vs_cso eliminates redundant air_to_nir and backend compilation.
 */
#ifndef AIR_METAL_VERTEX_CACHE_INC
#define AIR_METAL_VERTEX_CACHE_INC

#include <CommonCrypto/CommonDigest.h>

struct rwa_vertex_cso_entry {
   uint8_t hash[32];
   char entry_name[64];
   void *vs_cso;
   struct air_shader_info vinfo;
   struct rwa_slot vert_slots[RWA_MAX_TEXTURES];
   bool vert_compare_sampler[RWA_MAX_TEXTURES];
   unsigned fetched_count;
   unsigned refcnt;
   struct rwa_vertex_cso_entry *next;
};

static void
rwa_vertex_compute_hash(const void *bitcode, size_t size, uint8_t out[32])
{
   CC_SHA256_CTX ctx;
   CC_SHA256_Init(&ctx);
   if (bitcode && size)
      CC_SHA256_Update(&ctx, bitcode, size);
   CC_SHA256_Final(out, &ctx);
}

static struct rwa_vertex_cso_entry *
rwa_vertex_cso_lookup(struct rwa_device *device, const void *bitcode, size_t size,
                      const char *entry_name)
{
   if (!device || !bitcode || size < 4)
      return NULL;

   uint8_t hash[32];
   rwa_vertex_compute_hash(bitcode, size, hash);

   const char *name = entry_name ? entry_name : "";
   struct rwa_vertex_cso_entry *e = device->vertex_cso_cache;
   while (e) {
      if (memcmp(e->hash, hash, 32) == 0 && strcmp(e->entry_name, name) == 0) {
         e->refcnt++;
         return e;
      }
      e = e->next;
   }
   return NULL;
}

static struct rwa_vertex_cso_entry *
rwa_vertex_cso_insert(struct rwa_device *device, const void *bitcode, size_t size,
                      const char *entry_name, void *vs_cso,
                      const struct air_shader_info *vinfo,
                      const struct rwa_slot vert_slots[RWA_MAX_TEXTURES],
                      const bool vert_compare_sampler[RWA_MAX_TEXTURES],
                      unsigned fetched_count)
{
   if (!device || !bitcode || size < 4 || !vs_cso)
      return NULL;

   struct rwa_vertex_cso_entry *e = calloc(1, sizeof(*e));
   if (!e)
      return NULL;

   rwa_vertex_compute_hash(bitcode, size, e->hash);
   snprintf(e->entry_name, sizeof(e->entry_name), "%s", entry_name ? entry_name : "");
   e->vs_cso = vs_cso;
   if (vinfo)
      e->vinfo = *vinfo;
   if (vert_slots)
      memcpy(e->vert_slots, vert_slots, sizeof(e->vert_slots));
   if (vert_compare_sampler)
      memcpy(e->vert_compare_sampler, vert_compare_sampler, sizeof(e->vert_compare_sampler));
   e->fetched_count = fetched_count;
   e->refcnt = 1;

   e->next = device->vertex_cso_cache;
   device->vertex_cso_cache = e;
   return e;
}

static void
rwa_vertex_cso_release(struct rwa_device *device, struct rwa_render *render)
{
   if (!render)
      return;

   struct pipe_context *pipe = device ? device->pipe : render->device->pipe;

   if (render->cached_vcso_entry) {
      struct rwa_vertex_cso_entry *e = render->cached_vcso_entry;
      if (e->refcnt > 0)
         e->refcnt--;
      render->cached_vcso_entry = NULL;
      render->vs_cso = NULL;
   } else if (render->vs_cso) {
      if (pipe)
         pipe->delete_vs_state(pipe, render->vs_cso);
      render->vs_cso = NULL;
   }
}

static void
rwa_vertex_cso_cache_destroy(struct rwa_device *device)
{
   if (!device)
      return;

   struct pipe_context *pipe = device->pipe;
   struct rwa_vertex_cso_entry *e = device->vertex_cso_cache;
   while (e) {
      struct rwa_vertex_cso_entry *next = e->next;
      if (e->vs_cso && pipe)
         pipe->delete_vs_state(pipe, e->vs_cso);
      free(e);
      e = next;
   }
   device->vertex_cso_cache = NULL;
}

#endif /* AIR_METAL_VERTEX_CACHE_INC */
