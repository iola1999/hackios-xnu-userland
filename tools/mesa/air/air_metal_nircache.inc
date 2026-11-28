/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

/* Lock-free per-shader AIR -> (air_shader_info, nir_shader) disk cache.
 * Keys include binary LC_UUID, backend mode, entrypoint, bitcode, constants,
 * linked-function graph, imageblock size, and split window so cache hits never
 * collide across driver updates or shader specializations.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/loader.h>
#endif

#include "air_metal_reflection.h"
#include "air_reflection.h"
#include "nir_serialize.h"
#include "util/blob.h"
#include "util/crc32.h"
#include "util/mesa-blake3.h"

#define NIRC_DEFAULT_DIR   "/private/var/db/annibale-nircache"
#define NIRC_MAGIC         0x4352494eu /* "NIRC" */
#define NIRC_VERSION       2u
#define NIRC_MAX_FILE_SIZE (64u << 20)
#define NIRC_MAX_ARRAY_LEN 65536u
#define NIRC_MAX_REFL_DEPTH 64u

struct nircache_header {
   uint32_t magic;
   uint32_t version;
   uint32_t payload_size;
   uint32_t payload_crc32;
};

static unsigned nircache_tmp_seq;

static bool
nircache_enabled(void)
{
   const char *v = getenv("RWA_NIR_CACHE");
   return !v || strcmp(v, "0") != 0;
}

static bool
nircache_mkdir_777(const char *path)
{
   if (mkdir(path, 0777) == 0) {
      chmod(path, 0777);
      return true;
   }
   if (errno == EEXIST) {
      /* chmod fails with EPERM when another UID created a shared directory;
       * keeping EEXIST lets other UIDs still create entries inside it. */
      (void)chmod(path, 0777);
      return true;
   }
   return false;
}

static bool
nircache_uuid_string(char out[37])
{
#ifdef __APPLE__
   Dl_info info;
   if (!dladdr((const void *)nircache_uuid_string, &info) || !info.dli_fbase)
      return false;
   const struct mach_header_64 *hdr = (const struct mach_header_64 *)info.dli_fbase;
   if (hdr->magic != MH_MAGIC_64)
      return false;
   const uint8_t *ptr = (const uint8_t *)(hdr + 1);
   for (uint32_t i = 0; i < hdr->ncmds; i++) {
      const struct load_command *cmd = (const struct load_command *)ptr;
      if (cmd->cmdsize < sizeof(*cmd))
         return false;
      if (cmd->cmd == LC_UUID && cmd->cmdsize >= sizeof(struct uuid_command)) {
         const struct uuid_command *ucmd = (const struct uuid_command *)cmd;
         const uint8_t *u = ucmd->uuid;
         snprintf(out, 37,
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                  u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
         return true;
      }
      ptr += cmd->cmdsize;
   }
   return false;
#else
   snprintf(out, 37, "host-nonmacho");
   return true;
#endif
}

static void
nircache_hash_str(struct mesa_blake3 *ctx, const char *s)
{
   uint32_t len = s ? (uint32_t)strlen(s) + 1u : 0u;
   _mesa_blake3_update(ctx, &len, sizeof(len));
   if (len)
      _mesa_blake3_update(ctx, s, len);
}

static void
nircache_hash_key(const void *bitcode, size_t size, const char *entry,
                  const struct air_to_nir_options *options, bool software,
                  char hex_out[BLAKE3_HEX_LEN])
{
   static const char tag[] = "annibale-nircache-v3";
   struct mesa_blake3 ctx;
   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, tag, sizeof(tag));

   uint8_t sw = software ? 1 : 0;
   _mesa_blake3_update(&ctx, &sw, sizeof(sw));
   nircache_hash_str(&ctx, entry);

   uint64_t bsz = (uint64_t)size;
   _mesa_blake3_update(&ctx, &bsz, sizeof(bsz));
   _mesa_blake3_update(&ctx, bitcode, size);

   uint8_t flags = (options && options->library_wrapper) ? 1 : 0;
   uint32_t ib_w = options ? options->imageblock_width : 0;
   uint32_t ib_h = options ? options->imageblock_height : 0;
   uint8_t has_split = (options && options->split) ? 1 : 0;
   uint32_t num_constants = options ? options->num_constants : 0;
   uint32_t num_linked = options ? options->num_linked : 0;

   _mesa_blake3_update(&ctx, &flags, sizeof(flags));
   _mesa_blake3_update(&ctx, &ib_w, sizeof(ib_w));
   _mesa_blake3_update(&ctx, &ib_h, sizeof(ib_h));
   _mesa_blake3_update(&ctx, &has_split, sizeof(has_split));
   if (has_split) {
      _mesa_blake3_update(&ctx, options->split->base_group,
                          sizeof(options->split->base_group));
      _mesa_blake3_update(&ctx, options->split->nominal,
                          sizeof(options->split->nominal));
      _mesa_blake3_update(&ctx, options->split->grid_threads,
                          sizeof(options->split->grid_threads));
      _mesa_blake3_update(&ctx, options->split->grid_groups,
                          sizeof(options->split->grid_groups));
   }

   _mesa_blake3_update(&ctx, &num_constants, sizeof(num_constants));
   for (uint32_t i = 0; i < num_constants; i++) {
      uint32_t idx = options->constants[i].index;
      uint64_t bits = options->constants[i].bits;
      _mesa_blake3_update(&ctx, &idx, sizeof(idx));
      _mesa_blake3_update(&ctx, &bits, sizeof(bits));
   }

   _mesa_blake3_update(&ctx, &num_linked, sizeof(num_linked));
   for (uint32_t i = 0; i < num_linked; i++) {
      const struct air_linked_function *lf = &options->linked[i];
      nircache_hash_str(&ctx, lf->name);
      uint32_t out = lf->output;
      uint32_t nn = lf->num_nodes;
      _mesa_blake3_update(&ctx, &out, sizeof(out));
      _mesa_blake3_update(&ctx, &nn, sizeof(nn));
      for (uint32_t j = 0; j < nn; j++) {
         const struct air_stitch_node *nd = &lf->nodes[j];
         nircache_hash_str(&ctx, nd->function);
         uint64_t nsz = nd->bitcode ? (uint64_t)nd->size : 0u;
         _mesa_blake3_update(&ctx, &nsz, sizeof(nsz));
         if (nsz)
            _mesa_blake3_update(&ctx, nd->bitcode, nd->size);
         uint32_t nargs = nd->args ? nd->num_args : 0u;
         _mesa_blake3_update(&ctx, &nargs, sizeof(nargs));
         if (nargs)
            _mesa_blake3_update(&ctx, nd->args, nargs * sizeof(nd->args[0]));
         uint32_t nafter = nd->after ? nd->num_after : 0u;
         _mesa_blake3_update(&ctx, &nafter, sizeof(nafter));
         if (nafter)
            _mesa_blake3_update(&ctx, nd->after, nafter * sizeof(nd->after[0]));
      }
   }

   blake3_hash hash;
   _mesa_blake3_final(&ctx, hash);
   _mesa_blake3_format(hex_out, hash);
}

static bool
nircache_paths(const char *hex, char *bucket_dir, size_t bucket_sz,
               char *uuid_dir, size_t uuid_sz, char *file_path, size_t file_sz)
{
   const char *root = getenv("RWA_NIR_CACHE_DIR");
   if (!root || !root[0])
      root = NIRC_DEFAULT_DIR;

   char uuid[37];
   if (!nircache_uuid_string(uuid))
      return false;

   int n1 = snprintf(uuid_dir, uuid_sz, "%s/%s", root, uuid);
   int n2 = snprintf(bucket_dir, bucket_sz, "%s/%s/%.2s", root, uuid, hex);
   int n3 = snprintf(file_path, file_sz, "%s/%s/%.2s/%s", root, uuid, hex, hex);
   return n1 > 0 && (size_t)n1 < uuid_sz &&
          n2 > 0 && (size_t)n2 < bucket_sz &&
          n3 > 0 && (size_t)n3 < file_sz;
}

static void
nircache_write_str(struct blob *blob, const char *s)
{
   blob_write_uint8(blob, s != NULL);
   if (s)
      blob_write_string(blob, s);
}

static const char *
nircache_read_str(void *mem_ctx, struct blob_reader *blob)
{
   uint8_t present = blob_read_uint8(blob);
   if (blob->overrun || !present)
      return NULL;
   const char *s = blob_read_string(blob);
   if (!s || blob->overrun) {
      blob->overrun = true;
      return NULL;
   }
   return ralloc_strdup(mem_ctx, s);
}

static bool
nircache_write_refl_type(struct blob *blob, const struct rwa_reflection_type *t,
                         unsigned depth)
{
   if (depth > NIRC_MAX_REFL_DEPTH)
      return false;
   blob_write_uint8(blob, t != NULL);
   if (!t)
      return !blob->out_of_memory;
   blob_write_uint32(blob, t->data_type);
   blob_write_uint32(blob, t->data_size);
   blob_write_uint32(blob, t->alignment);
   nircache_write_str(blob, t->name);
   blob_write_uint32(blob, t->member_count);
   blob_write_uint32(blob, t->array_length);
   blob_write_uint32(blob, t->array_stride);
   blob_write_uint32(blob, t->texture_type);
   blob_write_uint32(blob, t->texture_data_type);
   blob_write_uint32(blob, t->depth_texture);
   blob_write_uint32(blob, t->access);
   blob_write_uint32(blob, t->constant_buffer);
   blob_write_uint32(blob, t->is_argument_buffer);
   for (unsigned i = 0; i < t->member_count; i++) {
      nircache_write_str(blob, t->members[i].name);
      blob_write_uint32(blob, t->members[i].offset);
      blob_write_uint32(blob, t->members[i].indirect_argument_index);
      if (!nircache_write_refl_type(blob, t->members[i].type, depth + 1))
         return false;
   }
   return nircache_write_refl_type(blob, t->element, depth + 1) &&
          !blob->out_of_memory;
}

static const struct rwa_reflection_type *
nircache_read_refl_type(void *mem_ctx, struct blob_reader *blob, unsigned depth)
{
   if (depth > NIRC_MAX_REFL_DEPTH) {
      blob->overrun = true;
      return NULL;
   }
   uint8_t present = blob_read_uint8(blob);
   if (blob->overrun || !present)
      return NULL;
   struct rwa_reflection_type *t = rzalloc(mem_ctx, struct rwa_reflection_type);
   if (!t) {
      blob->overrun = true;
      return NULL;
   }
   t->data_type = blob_read_uint32(blob);
   t->data_size = blob_read_uint32(blob);
   t->alignment = blob_read_uint32(blob);
   t->name = nircache_read_str(t, blob);
   t->member_count = blob_read_uint32(blob);
   t->array_length = blob_read_uint32(blob);
   t->array_stride = blob_read_uint32(blob);
   t->texture_type = blob_read_uint32(blob);
   t->texture_data_type = blob_read_uint32(blob);
   t->depth_texture = blob_read_uint32(blob);
   t->access = blob_read_uint32(blob);
   t->constant_buffer = blob_read_uint32(blob);
   t->is_argument_buffer = blob_read_uint32(blob);
   if (blob->overrun || t->member_count > NIRC_MAX_ARRAY_LEN) {
      blob->overrun = true;
      return NULL;
   }
   if (t->member_count) {
      struct rwa_reflection_member *members =
         rzalloc_array(t, struct rwa_reflection_member, t->member_count);
      if (!members) {
         blob->overrun = true;
         return NULL;
      }
      for (unsigned i = 0; i < t->member_count; i++) {
         members[i].name = nircache_read_str(t, blob);
         members[i].offset = blob_read_uint32(blob);
         members[i].indirect_argument_index = blob_read_uint32(blob);
         members[i].type = nircache_read_refl_type(t, blob, depth + 1);
         if (blob->overrun)
            return NULL;
      }
      t->members = members;
   }
   t->element = nircache_read_refl_type(t, blob, depth + 1);
   return blob->overrun ? NULL : t;
}

static bool
nircache_write_info(struct blob *blob, const struct air_shader_info *info)
{
   blob_write_uint32(blob, (uint32_t)info->stage);
   nircache_write_str(blob, info->entry_point);
   blob_write_uint32(blob, info->const_win_size);
   blob_write_uint32(blob, info->vertex_attributes_read);
   blob_write_uint32(blob, info->num_render_targets);
   blob_write_uint32(blob, info->fbfetch_targets);
   blob_write_uint32(blob, info->raster_order_groups);
   blob_write_uint32(blob, info->threadgroup_bytes);
   blob_write_uint8(blob, info->group_free);
   blob_write_uint32(blob, info->clip_distances);
   blob_write_uint32(blob, info->imageblock_data_size);

   blob_write_uint32(blob, info->num_resources);
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      blob_write_uint32(blob, (uint32_t)r->kind);
      blob_write_uint32(blob, r->index);
      blob_write_uint32(blob, r->size);
      blob_write_uint32(blob, r->slot);
      blob_write_uint8(blob, r->const_win);
      blob_write_uint32(blob, r->win_base);
      blob_write_uint8(blob, r->read);
      blob_write_uint8(blob, r->write);
      blob_write_uint32(blob, r->param);
      nircache_write_str(blob, r->name);
      nircache_write_str(blob, r->type_name);
      blob_write_uint8(blob, r->inline_sampler);
      blob_write_uint64(blob, r->sampler_state);
      for (unsigned a = 0; a < 3; a++)
         blob_write_uint32(blob, (uint32_t)r->sampler.address[a]);
      blob_write_uint32(blob, (uint32_t)r->sampler.mag_filter);
      blob_write_uint32(blob, (uint32_t)r->sampler.min_filter);
      blob_write_uint32(blob, (uint32_t)r->sampler.mip_filter);
      blob_write_uint8(blob, r->sampler.pixel_coord);
      blob_write_uint32(blob, r->sampler.compare_func);
      blob_write_uint32(blob, r->sampler.max_anisotropy);
      blob_write_uint16(blob, r->sampler.lod_min);
      blob_write_uint16(blob, r->sampler.lod_max);
      blob_write_uint32(blob, r->sampler.border_color);
      blob_write_uint32(blob, r->sampler.reduction);
      blob_write_uint32(blob, (uint32_t)r->raster_order_group);
      blob_write_uint32(blob, r->level_base);
      blob_write_uint32(blob, r->level_count);
      blob_write_uint8(blob, r->nullable);
      blob_write_uint8(blob, r->unused);
      blob_write_uint32(blob, r->argbuf_set);
      blob_write_uint32(blob, r->argbuf_id);
   }

   blob_write_uint32(blob, info->num_argbuf_constants);
   for (unsigned i = 0; i < info->num_argbuf_constants; i++) {
      const struct air_argbuf_const_member *cm = &info->argbuf_constants[i];
      blob_write_uint32(blob, cm->argbuf_set);
      blob_write_uint32(blob, cm->argbuf_id);
      blob_write_uint32(blob, cm->offset);
      blob_write_uint32(blob, cm->size);
      blob_write_uint32(blob, cm->alignment);
      nircache_write_str(blob, cm->name);
   }

   blob_write_uint32(blob, info->num_varyings);
   for (unsigned i = 0; i < info->num_varyings; i++) {
      const struct air_varying *v = &info->varyings[i];
      nircache_write_str(blob, v->name);
      blob_write_uint32(blob, v->slot);
      blob_write_uint32(blob, v->components);
      blob_write_uint8(blob, v->flat);
      blob_write_uint8(blob, v->centroid);
      blob_write_uint8(blob, v->sample);
      blob_write_uint8(blob, v->no_perspective);
   }

   blob_write_uint32(blob, info->num_vertex_attributes);
   for (unsigned i = 0; i < info->num_vertex_attributes; i++) {
      const struct air_vertex_attribute *va = &info->vertex_attributes[i];
      blob_write_uint32(blob, va->index);
      blob_write_uint32(blob, va->location);
      blob_write_uint32(blob, va->components);
      blob_write_uint32(blob, va->driver_location);
      blob_write_uint32(blob, va->bit_size);
      blob_write_uint32(blob, (uint32_t)va->base_type);
      nircache_write_str(blob, va->name);
      nircache_write_str(blob, va->type_name);
      blob_write_uint8(blob, va->used);
      nircache_write_str(blob, va->function_constant);
   }

   blob_write_uint32(blob, info->num_notes);
   for (unsigned i = 0; i < info->num_notes; i++)
      nircache_write_str(blob, info->notes[i]);

   nircache_write_str(blob, info->reflection_error);
   blob_write_uint8(blob, info->reflection != NULL);
   if (info->reflection) {
      blob_write_uint32(blob, info->reflection->argument_count);
      for (unsigned i = 0; i < info->reflection->argument_count; i++) {
         const struct rwa_reflection_argument *arg = &info->reflection->arguments[i];
         nircache_write_str(blob, arg->name);
         blob_write_uint32(blob, arg->type);
         blob_write_uint32(blob, arg->index);
         blob_write_uint32(blob, arg->access);
         blob_write_uint32(blob, arg->active);
         blob_write_uint32(blob, arg->array_length);
         if (!nircache_write_refl_type(blob, arg->buffer_type, 0))
            return false;
         blob_write_uint32(blob, arg->constant_buffer);
         blob_write_uint32(blob, arg->texture_type);
         blob_write_uint32(blob, arg->texture_data_type);
         blob_write_uint32(blob, arg->depth_texture);
      }
   }
   return !blob->out_of_memory;
}

static bool
nircache_read_info(void *mem_ctx, struct blob_reader *blob,
                   struct air_shader_info *info)
{
   memset(info, 0, sizeof(*info));
   info->stage = (mesa_shader_stage)blob_read_uint32(blob);
   info->entry_point = nircache_read_str(mem_ctx, blob);
   info->const_win_size = blob_read_uint32(blob);
   info->vertex_attributes_read = blob_read_uint32(blob);
   info->num_render_targets = blob_read_uint32(blob);
   info->fbfetch_targets = blob_read_uint32(blob);
   info->raster_order_groups = blob_read_uint32(blob);
   info->threadgroup_bytes = blob_read_uint32(blob);
   info->group_free = blob_read_uint8(blob) != 0;
   info->clip_distances = blob_read_uint32(blob);
   info->imageblock_data_size = blob_read_uint32(blob);

   info->num_resources = blob_read_uint32(blob);
   if (blob->overrun || info->num_resources > NIRC_MAX_ARRAY_LEN)
      return false;
   if (info->num_resources) {
      info->resources = rzalloc_array(mem_ctx, struct air_resource, info->num_resources);
      if (!info->resources)
         return false;
      for (unsigned i = 0; i < info->num_resources; i++) {
         struct air_resource *r = &info->resources[i];
         r->kind = (enum air_resource_kind)blob_read_uint32(blob);
         r->index = blob_read_uint32(blob);
         r->size = blob_read_uint32(blob);
         r->slot = blob_read_uint32(blob);
         r->const_win = blob_read_uint8(blob) != 0;
         r->win_base = blob_read_uint32(blob);
         r->read = blob_read_uint8(blob) != 0;
         r->write = blob_read_uint8(blob) != 0;
         r->param = blob_read_uint32(blob);
         r->name = nircache_read_str(mem_ctx, blob);
         r->type_name = nircache_read_str(mem_ctx, blob);
         r->inline_sampler = blob_read_uint8(blob) != 0;
         r->sampler_state = blob_read_uint64(blob);
         for (unsigned a = 0; a < 3; a++)
            r->sampler.address[a] = (enum air_sampler_address)blob_read_uint32(blob);
         r->sampler.mag_filter = (enum air_sampler_filter)blob_read_uint32(blob);
         r->sampler.min_filter = (enum air_sampler_filter)blob_read_uint32(blob);
         r->sampler.mip_filter = (enum air_sampler_mip_filter)blob_read_uint32(blob);
         r->sampler.pixel_coord = blob_read_uint8(blob) != 0;
         r->sampler.compare_func = blob_read_uint32(blob);
         r->sampler.max_anisotropy = blob_read_uint32(blob);
         r->sampler.lod_min = blob_read_uint16(blob);
         r->sampler.lod_max = blob_read_uint16(blob);
         r->sampler.border_color = blob_read_uint32(blob);
         r->sampler.reduction = blob_read_uint32(blob);
         r->raster_order_group = (int)blob_read_uint32(blob);
         r->level_base = blob_read_uint32(blob);
         r->level_count = blob_read_uint32(blob);
         r->nullable = blob_read_uint8(blob) != 0;
         r->unused = blob_read_uint8(blob) != 0;
         r->argbuf_set = blob_read_uint32(blob);
         r->argbuf_id = blob_read_uint32(blob);
         if (blob->overrun)
            return false;
      }
   }

   info->num_argbuf_constants = blob_read_uint32(blob);
   if (blob->overrun || info->num_argbuf_constants > NIRC_MAX_ARRAY_LEN)
      return false;
   if (info->num_argbuf_constants) {
      info->argbuf_constants =
         rzalloc_array(mem_ctx, struct air_argbuf_const_member, info->num_argbuf_constants);
      if (!info->argbuf_constants)
         return false;
      for (unsigned i = 0; i < info->num_argbuf_constants; i++) {
         struct air_argbuf_const_member *cm = &info->argbuf_constants[i];
         cm->argbuf_set = blob_read_uint32(blob);
         cm->argbuf_id = blob_read_uint32(blob);
         cm->offset = blob_read_uint32(blob);
         cm->size = blob_read_uint32(blob);
         cm->alignment = blob_read_uint32(blob);
         cm->name = nircache_read_str(mem_ctx, blob);
         if (blob->overrun)
            return false;
      }
   }

   info->num_varyings = blob_read_uint32(blob);
   if (blob->overrun || info->num_varyings > NIRC_MAX_ARRAY_LEN)
      return false;
   if (info->num_varyings) {
      info->varyings = rzalloc_array(mem_ctx, struct air_varying, info->num_varyings);
      if (!info->varyings)
         return false;
      for (unsigned i = 0; i < info->num_varyings; i++) {
         struct air_varying *v = &info->varyings[i];
         v->name = nircache_read_str(mem_ctx, blob);
         v->slot = blob_read_uint32(blob);
         v->components = blob_read_uint32(blob);
         v->flat = blob_read_uint8(blob) != 0;
         v->centroid = blob_read_uint8(blob) != 0;
         v->sample = blob_read_uint8(blob) != 0;
         v->no_perspective = blob_read_uint8(blob) != 0;
         if (blob->overrun)
            return false;
      }
   }

   info->num_vertex_attributes = blob_read_uint32(blob);
   if (blob->overrun || info->num_vertex_attributes > NIRC_MAX_ARRAY_LEN)
      return false;
   if (info->num_vertex_attributes) {
      info->vertex_attributes =
         rzalloc_array(mem_ctx, struct air_vertex_attribute, info->num_vertex_attributes);
      if (!info->vertex_attributes)
         return false;
      for (unsigned i = 0; i < info->num_vertex_attributes; i++) {
         struct air_vertex_attribute *va = &info->vertex_attributes[i];
         va->index = blob_read_uint32(blob);
         va->location = blob_read_uint32(blob);
         va->components = blob_read_uint32(blob);
         va->driver_location = blob_read_uint32(blob);
         va->bit_size = blob_read_uint32(blob);
         va->base_type = (enum air_attribute_base_type)blob_read_uint32(blob);
         va->name = nircache_read_str(mem_ctx, blob);
         va->type_name = nircache_read_str(mem_ctx, blob);
         va->used = blob_read_uint8(blob) != 0;
         va->function_constant = nircache_read_str(mem_ctx, blob);
         if (blob->overrun)
            return false;
      }
   }

   info->num_notes = blob_read_uint32(blob);
   if (blob->overrun || info->num_notes > NIRC_MAX_ARRAY_LEN)
      return false;
   if (info->num_notes) {
      info->notes = rzalloc_array(mem_ctx, const char *, info->num_notes);
      if (!info->notes)
         return false;
      for (unsigned i = 0; i < info->num_notes; i++) {
         info->notes[i] = nircache_read_str(mem_ctx, blob);
         if (blob->overrun)
            return false;
      }
   }

   info->reflection_error = nircache_read_str(mem_ctx, blob);
   uint8_t has_refl = blob_read_uint8(blob);
   if (blob->overrun)
      return false;
   if (has_refl) {
      struct rwa_compute_reflection *refl = rzalloc(mem_ctx, struct rwa_compute_reflection);
      if (!refl)
         return false;
      refl->argument_count = blob_read_uint32(blob);
      if (blob->overrun || refl->argument_count > NIRC_MAX_ARRAY_LEN)
         return false;
      if (refl->argument_count) {
         struct rwa_reflection_argument *args =
            rzalloc_array(refl, struct rwa_reflection_argument, refl->argument_count);
         if (!args)
            return false;
         for (unsigned i = 0; i < refl->argument_count; i++) {
            args[i].name = nircache_read_str(refl, blob);
            args[i].type = blob_read_uint32(blob);
            args[i].index = blob_read_uint32(blob);
            args[i].access = blob_read_uint32(blob);
            args[i].active = blob_read_uint32(blob);
            args[i].array_length = blob_read_uint32(blob);
            args[i].buffer_type = nircache_read_refl_type(refl, blob, 0);
            args[i].constant_buffer = blob_read_uint32(blob);
            args[i].texture_type = blob_read_uint32(blob);
            args[i].texture_data_type = blob_read_uint32(blob);
            args[i].depth_texture = blob_read_uint32(blob);
            if (blob->overrun)
               return false;
         }
         refl->arguments = args;
      }
      info->reflection = refl;
   }
   return !blob->overrun;
}

static nir_shader *
nircache_load(void *mem_ctx, const char *file_path,
              const nir_shader_compiler_options *nir_options,
              struct air_shader_info *info)
{
   int fd = open(file_path, O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return NULL;

   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
       st.st_size < (off_t)sizeof(struct nircache_header) ||
       st.st_size > (off_t)NIRC_MAX_FILE_SIZE) {
      close(fd);
      return NULL;
   }

   size_t total = (size_t)st.st_size;
   uint8_t *buf = malloc(total);
   if (!buf) {
      close(fd);
      return NULL;
   }

   size_t got = 0;
   while (got < total) {
      ssize_t r = read(fd, buf + got, total - got);
      if (r < 0) {
         if (errno == EINTR)
            continue;
         free(buf);
         close(fd);
         return NULL;
      }
      if (r == 0)
         break;
      got += (size_t)r;
   }
   close(fd);
   if (got != total) {
      free(buf);
      return NULL;
   }

   struct nircache_header hdr;
   memcpy(&hdr, buf, sizeof(hdr));
   if (hdr.magic != NIRC_MAGIC || hdr.version != NIRC_VERSION ||
       hdr.payload_size != total - sizeof(hdr) ||
       util_hash_crc32(buf + sizeof(hdr), hdr.payload_size) != hdr.payload_crc32) {
      free(buf);
      (void)unlink(file_path);
      return NULL;
   }

   void *tmp_ctx = ralloc_context(mem_ctx);
   if (!tmp_ctx) {
      free(buf);
      return NULL;
   }
   struct blob_reader reader;
   blob_reader_init(&reader, buf + sizeof(hdr), hdr.payload_size);
   if (!nircache_read_info(tmp_ctx, &reader, info)) {
      ralloc_free(tmp_ctx);
      memset(info, 0, sizeof(*info));
      free(buf);
      (void)unlink(file_path);
      return NULL;
   }
   nir_shader *nir = nir_deserialize(tmp_ctx, nir_options, &reader);
   free(buf);
   if (!nir || reader.overrun || reader.current != reader.end) {
      ralloc_free(tmp_ctx);
      memset(info, 0, sizeof(*info));
      (void)unlink(file_path);
      return NULL;
   }
   return nir;
}

static void
nircache_store(const char *uuid_dir, const char *bucket_dir, const char *file_path,
               const struct air_shader_info *info, const nir_shader *nir)
{
   const char *root = getenv("RWA_NIR_CACHE_DIR");
   if (!root || !root[0])
      root = NIRC_DEFAULT_DIR;
   if (!nircache_mkdir_777(root) || !nircache_mkdir_777(uuid_dir) ||
       !nircache_mkdir_777(bucket_dir))
      return;

   struct blob blob;
   blob_init(&blob);
   struct nircache_header hdr = {0};
   blob_write_bytes(&blob, &hdr, sizeof(hdr));
   if (!nircache_write_info(&blob, info)) {
      blob_finish(&blob);
      return;
   }
   /* strip=false keeps shader name, interface variable names, and debug metadata
    * that runtime reflection and downstream passes expect. */
   nir_serialize(&blob, nir, false);
   if (blob.out_of_memory || blob.size < sizeof(hdr) ||
       blob.size > NIRC_MAX_FILE_SIZE) {
      blob_finish(&blob);
      return;
   }

   hdr.magic = NIRC_MAGIC;
   hdr.version = NIRC_VERSION;
   hdr.payload_size = (uint32_t)(blob.size - sizeof(hdr));
   hdr.payload_crc32 = util_hash_crc32(blob.data + sizeof(hdr), hdr.payload_size);
   memcpy(blob.data, &hdr, sizeof(hdr));

   char tmp_path[PATH_MAX];
   unsigned seq = __atomic_add_fetch(&nircache_tmp_seq, 1, __ATOMIC_RELAXED);
   int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld.%u",
                    file_path, (long)getpid(), seq);
   if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
      blob_finish(&blob);
      return;
   }

   int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
   if (fd < 0) {
      blob_finish(&blob);
      return;
   }
   fchmod(fd, 0666);

   size_t written = 0;
   bool ok = true;
   while (written < blob.size) {
      ssize_t w = write(fd, blob.data + written, blob.size - written);
      if (w < 0) {
         if (errno == EINTR)
            continue;
         ok = false;
         break;
      }
      written += (size_t)w;
   }
   if (close(fd) != 0)
      ok = false;
   if (ok) {
      if (rename(tmp_path, file_path) != 0)
         (void)unlink(tmp_path);
   } else {
      (void)unlink(tmp_path);
   }
   blob_finish(&blob);
}

static nir_shader *
rwa_air_to_nir_cached(void *mem_ctx, const void *bitcode, size_t size,
                      const char *entry, const struct air_to_nir_options *options,
                      const nir_shader_compiler_options *nir_options,
                      bool software, struct air_shader_info *info,
                      const char **error)
{
   if (!nircache_enabled() || !bitcode || size < 4 ||
       (options && options->kernel) || !info)
      return air_to_nir(mem_ctx, bitcode, size, entry, options, nir_options, info, error);

   char hex[BLAKE3_HEX_LEN];
   nircache_hash_key(bitcode, size, entry, options, software, hex);

   char uuid_dir[PATH_MAX], bucket_dir[PATH_MAX], file_path[PATH_MAX];
   bool have_paths = nircache_paths(hex, bucket_dir, sizeof(bucket_dir),
                                    uuid_dir, sizeof(uuid_dir),
                                    file_path, sizeof(file_path));
   if (have_paths) {
      nir_shader *cached = nircache_load(mem_ctx, file_path, nir_options, info);
      if (cached) {
         if (!info->reflection && !info->reflection_error)
            info->reflection = air_module_reflect(mem_ctx, bitcode, size, entry,
                                                  &info->reflection_error);
         return cached;
      }
   }

   nir_shader *nir = air_to_nir(mem_ctx, bitcode, size, entry, options,
                                nir_options, info, error);
   if (nir && have_paths)
      nircache_store(uuid_dir, bucket_dir, file_path, info, nir);
   return nir;
}
