/*
 * In-process vertex stage CSO caching and reuse for libannibaleair.
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
   void *mem_ctx;
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
   e->mem_ctx = ralloc_context(NULL);
   e->vs_cso = vs_cso;
   if (vinfo) {
      e->vinfo = *vinfo;
      e->vinfo.notes = NULL;
      e->vinfo.num_notes = 0;
      if (vinfo->entry_point)
         e->vinfo.entry_point = ralloc_strdup(e->mem_ctx, vinfo->entry_point);
      if (vinfo->resources && vinfo->num_resources > 0) {
         e->vinfo.resources = ralloc_array(e->mem_ctx, struct air_resource, vinfo->num_resources);
         memcpy(e->vinfo.resources, vinfo->resources, vinfo->num_resources * sizeof(struct air_resource));
         for (unsigned i = 0; i < vinfo->num_resources; i++) {
            if (vinfo->resources[i].name)
               e->vinfo.resources[i].name = ralloc_strdup(e->mem_ctx, vinfo->resources[i].name);
            if (vinfo->resources[i].type_name)
               e->vinfo.resources[i].type_name = ralloc_strdup(e->mem_ctx, vinfo->resources[i].type_name);
         }
      }
      if (vinfo->varyings && vinfo->num_varyings > 0) {
         e->vinfo.varyings = ralloc_array(e->mem_ctx, struct air_varying, vinfo->num_varyings);
         memcpy(e->vinfo.varyings, vinfo->varyings, vinfo->num_varyings * sizeof(struct air_varying));
         for (unsigned i = 0; i < vinfo->num_varyings; i++) {
            if (vinfo->varyings[i].name)
               e->vinfo.varyings[i].name = ralloc_strdup(e->mem_ctx, vinfo->varyings[i].name);
         }
      }
      if (vinfo->vertex_attributes && vinfo->num_vertex_attributes > 0) {
         e->vinfo.vertex_attributes = ralloc_array(e->mem_ctx, struct air_vertex_attribute,
                                                   vinfo->num_vertex_attributes);
         memcpy(e->vinfo.vertex_attributes, vinfo->vertex_attributes,
                vinfo->num_vertex_attributes * sizeof(struct air_vertex_attribute));
         for (unsigned i = 0; i < vinfo->num_vertex_attributes; i++) {
            if (vinfo->vertex_attributes[i].name)
               e->vinfo.vertex_attributes[i].name = ralloc_strdup(e->mem_ctx, vinfo->vertex_attributes[i].name);
            if (vinfo->vertex_attributes[i].type_name)
               e->vinfo.vertex_attributes[i].type_name = ralloc_strdup(e->mem_ctx, vinfo->vertex_attributes[i].type_name);
            if (vinfo->vertex_attributes[i].function_constant)
               e->vinfo.vertex_attributes[i].function_constant = ralloc_strdup(e->mem_ctx, vinfo->vertex_attributes[i].function_constant);
         }
      }
   }
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
      if (e->mem_ctx)
         ralloc_free(e->mem_ctx);
      free(e);
      e = next;
   }
   device->vertex_cso_cache = NULL;
}

#endif /* AIR_METAL_VERTEX_CACHE_INC */
