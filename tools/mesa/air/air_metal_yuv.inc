/* Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

struct rwa_texture *
rwa_texture_combine_yuv(struct rwa_texture *y, struct rwa_texture *uv,
                        bool full_range, unsigned matrix, char *why, size_t why_size)
{
   RWA_LOCKED;
   if (!y || !uv || y->device != uv->device || y->shape.pixel_format != 10 ||
       uv->shape.pixel_format != 30 || y->shape.type != 2 || uv->shape.type != 2 ||
       y->shape.sample_count != 1 || uv->shape.sample_count != 1 ||
       texture_levels(y) != 1 || texture_levels(uv) != 1 ||
       uv->shape.width != (y->shape.width + 1) / 2 ||
       uv->shape.height != (y->shape.height + 1) / 2 ||
       !y->shader_read || !uv->shader_read || (matrix != 1 && matrix != 6)) {
      say(why, why_size, "combined NV12 requires R8/RG8 2D planes and matrix 1 or 6");
      return NULL;
   }
   struct rwa_texture *t = calloc(1, sizeof(*t));
   if (!t) {
      say(why, why_size, "out of memory creating a combined NV12 texture");
      return NULL;
   }
   t->references = 1;
   t->device = y->device;
   t->shape = y->shape;
   t->shape.pixel_format = 500;
   t->shape.usage = 1;
   t->format_name = "MTLPixelFormatYCBCR8_420_2P";
   t->shader_read = true;
   t->layers = 1;
   t->storage_owner = y;
   t->chroma = uv;
   t->yuv_full_range = full_range;
   t->yuv_bt709 = matrix == 1;
   y->references++;
   uv->references++;
   pipe_resource_reference(&t->res, y->res);
   return t;
}

/* Reserve chroma slots after accounting for every declared texture. The mapping is
 * shared by shader lowering and binding, including argument-buffer members. */
static bool
yuv_key_for(const struct air_shader_info *info, const struct rwa_bindings *bindings,
            const struct rwa_buffer_bind *table, unsigned table_count,
            struct rwa_yuv_key *key, char *why, size_t why_size)
{
   memset(key, 0, sizeof(*key));
   unsigned used = 0;
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->kind == AIR_RESOURCE_TEXTURE && r->index < RWA_MAX_TEXTURES)
         used |= 1u << r->index;
   }
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->kind != AIR_RESOURCE_TEXTURE)
         continue;
      struct rwa_texture *t = NULL;
      if (r->argbuf_set != ~0u) {
         if (argbuf_member(r, table, table_count, info->entry_point, NULL, NULL,
                           &t, NULL, why, why_size) != 0)
            return false;
      } else if (bindings && r->index < bindings->texture_count) {
         t = bindings->textures[r->index];
      }
      if (!t || !t->chroma)
         continue;
      if (r->index >= RWA_MAX_TEXTURES) {
         say(why, why_size, "combined NV12 texture index exceeds the binding limit");
         return false;
      }
      unsigned bit = 1u << r->index;
      if (key->mask & bit)
         continue;
      unsigned slot = 0;
      while (slot < RWA_MAX_TEXTURES && (used & (1u << slot)))
         slot++;
      if (slot == RWA_MAX_TEXTURES) {
         say(why, why_size, "combined NV12 needs a spare texture slot for its UV plane");
         return false;
      }
      used |= 1u << slot;
      key->mask |= bit;
      key->full_range |= t->yuv_full_range ? bit : 0;
      key->bt709 |= t->yuv_bt709 ? bit : 0;
      key->uv_slot[r->index] = slot;
   }
   return true;
}

static bool
lower_yuv(nir_shader *nir, const struct rwa_yuv_key *key, char *why, size_t why_size)
{
   if (!key->mask)
      return true;
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            /* Dynamic indexing can select either plane layout. A fixed shader
             * variant cannot describe that choice without an explicit lowering. */
            if (nir_tex_instr_src_index(tex, nir_tex_src_texture_offset) >= 0 ||
                nir_tex_instr_src_index(tex, nir_tex_src_texture_deref) >= 0) {
               say(why, why_size, "combined NV12 with dynamic texture indexing is unsupported");
               return false;
            }
            if (tex->texture_index >= RWA_MAX_TEXTURES ||
                !(key->mask & (1u << tex->texture_index)) || nir_tex_instr_is_query(tex))
               continue;
            if ((tex->op != nir_texop_tex && tex->op != nir_texop_txl &&
                 tex->op != nir_texop_txb && tex->op != nir_texop_txd) ||
                tex->sampler_dim != GLSL_SAMPLER_DIM_2D || tex->is_array ||
                tex->is_shadow || tex->def.num_components != 4 ||
                nir_alu_type_get_base_type(tex->dest_type) != nir_type_float) {
               say(why, why_size, "combined NV12 supports 2D floating-point sample operations");
               return false;
            }
            b.cursor = nir_after_instr(instr);
            nir_def *clamped = nir_fsat(&b, &tex->def);
            nir_def_rewrite_uses_after(&tex->def, clamped);
         }
      }
   }
   nir_lower_tex_options options = {
      .lower_y_uv_external = key->mask,
      .yuv_full_range_external = key->full_range,
      .bt709_external = key->bt709,
   };
   nir_lower_tex(nir, &options);
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            int source = nir_tex_instr_src_index(tex, nir_tex_src_plane);
            if (source < 0)
               continue;
            unsigned plane = nir_src_as_uint(tex->src[source].src);
            if (plane == 1)
               tex->texture_index = key->uv_slot[tex->texture_index];
            nir_tex_instr_remove_src(tex, source);
         }
      }
   }
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   nir_validate_shader(nir, "combined NV12 lowering");
   return true;
}
