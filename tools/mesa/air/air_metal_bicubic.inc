/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

/* The sampled constexpr words carry LOD bounds [0, 65504]. */
static bool
rgb2p_inline_filter(const struct air_resource *r)
{
   if (!r || !r->inline_sampler)
      return false;
   const struct air_sampler_state *s = &r->sampler;
   return !s->pixel_coord && s->address[0] == AIR_SAMPLER_CLAMP_TO_EDGE &&
          s->address[1] == AIR_SAMPLER_CLAMP_TO_EDGE &&
          s->min_filter == s->mag_filter &&
          (s->min_filter == AIR_SAMPLER_LINEAR || s->min_filter == AIR_SAMPLER_BICUBIC) &&
          s->mip_filter == AIR_SAMPLER_MIP_NONE && s->max_anisotropy == 1 &&
          s->lod_min == 0 && s->lod_max == 0x7bff && s->reduction == 0;
}

static const struct air_resource *
rgb2p_inline_sampler(const struct air_shader_info *info, unsigned slot)
{
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->kind == AIR_RESOURCE_SAMPLER && r->index == slot)
         return r;
   }
   return NULL;
}

/* Saved native UNORM samples use a coordinate fraction rounded to 1/256.
 * Adreno's linear lookup has lower coordinate precision in the same batch.
 * Both filters use integer fetches here; native coefficient and accumulation
 * precision remain unconfirmed and the interpolation uses float arithmetic. */
static nir_def *
rgb2p_filter_sample(nir_builder *b, const nir_tex_instr *tex, nir_def *coord, bool cubic)
{
   nir_def *size = nir_txs(b, .texture_index = tex->texture_index,
                           .dim = GLSL_SAMPLER_DIM_2D, .dest_type = nir_type_int32,
                           .lod = nir_imm_int(b, 0));
   nir_def *sizef = nir_i2f32(b, size);
   nir_def *p = nir_fadd_imm(b, nir_fmul(b, nir_f2f32(b, coord), sizef), -0.5f);
   /* Beyond this interval every contributing texel clamps to the same edge.
    * Bounding it also keeps float-to-integer conversion within texture limits. */
   p = nir_fmin(b, nir_fmax(b, p, nir_imm_float(b, -1.0f)), sizef);
   nir_def *basef = nir_ffloor(b, p);
   nir_def *base = nir_f2i32(b, basef);
   nir_def *t = nir_fmul_imm(b, nir_fround_even(b,
      nir_fmul_imm(b, nir_fsub(b, p, basef), 256.0f)), 1.0f / 256.0f);
   nir_def *w[4];
   if (cubic) {
      nir_def *t2 = nir_fmul(b, t, t), *t3 = nir_fmul(b, t2, t);
      w[0] = nir_fsub(b, nir_fadd(b, nir_fmul_imm(b, t, -0.5f), t2),
                          nir_fmul_imm(b, t3, 0.5f));
      w[1] = nir_fadd(b, nir_fadd_imm(b, nir_fmul_imm(b, t2, -2.5f), 1.0f),
                          nir_fmul_imm(b, t3, 1.5f));
      w[2] = nir_fsub(b, nir_fadd(b, nir_fmul_imm(b, t, 0.5f), nir_fmul_imm(b, t2, 2.0f)),
                          nir_fmul_imm(b, t3, 1.5f));
      w[3] = nir_fadd(b, nir_fmul_imm(b, t2, -0.5f), nir_fmul_imm(b, t3, 0.5f));
   } else {
      w[0] = nir_fsub(b, nir_imm_float(b, 1.0f), t);
      w[1] = t;
   }
   nir_def *last = nir_iadd_imm(b, size, -1);
   nir_def *sum = nir_imm_vec4(b, 0, 0, 0, 0);
   unsigned taps = cubic ? 4 : 2;
   int first = cubic ? -1 : 0;
   for (unsigned y = 0; y < taps; y++) {
      for (unsigned x = 0; x < taps; x++) {
         nir_def *offset = nir_vec2(b, nir_imm_int(b, (int)x + first),
                                      nir_imm_int(b, (int)y + first));
         nir_def *point = nir_imin(b, nir_imax(b, nir_iadd(b, base, offset),
                                                   nir_imm_int(b, 0)), last);
         nir_def *sample = nir_txf(b, point, .texture_index = tex->texture_index,
                                    .dim = GLSL_SAMPLER_DIM_2D, .dest_type = nir_type_float32,
                                    .lod = nir_imm_int(b, 0));
         nir_def *weight = nir_fmul(b, nir_channel(b, w[x], 0), nir_channel(b, w[y], 1));
         sum = nir_fadd(b, sum, nir_fmul(b, sample, weight));
      }
   }
   return nir_f2fN(b, nir_fsat(b, sum), tex->def.bit_size);
}

static bool
lower_rgb2p_samples(nir_shader *nir, const struct air_shader_info *info,
                     const struct rwa_yuv_key *key, char *why, size_t why_size)
{
   if (!key->rgb2p)
      return true;
   nir_foreach_function_impl(impl, nir) {
      bool progress = false;
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            if (!nir_tex_instr_need_sampler(tex))
               continue;
            if (tex->texture_index >= RWA_MAX_TEXTURES ||
                !(key->rgb2p & (1u << tex->texture_index)))
               continue;
            int sampler_offset = nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset);
            unsigned reachable = sampler_offset < 0 ?
               (tex->sampler_index < RWA_MAX_TEXTURES ? 1u << tex->sampler_index : ~0u) :
               sampler_slot_mask(tex->src[sampler_offset].src.ssa, tex->sampler_index);
            unsigned cubic = 0, linear = 0;
            for (unsigned slot = 0; slot < RWA_MAX_TEXTURES; slot++) {
               const struct air_resource *r = rgb2p_inline_sampler(info, slot);
               if ((reachable & (1u << slot)) && r && r->inline_sampler &&
                   (r->sampler.min_filter == AIR_SAMPLER_BICUBIC ||
                    r->sampler.mag_filter == AIR_SAMPLER_BICUBIC))
                  cubic |= 1u << slot;
               if ((reachable & (1u << slot)) && r && r->inline_sampler &&
                   (r->sampler.min_filter == AIR_SAMPLER_LINEAR ||
                    r->sampler.mag_filter == AIR_SAMPLER_LINEAR))
                  linear |= 1u << slot;
            }
            if (!(cubic | linear))
               continue;
            if (nir_tex_instr_src_index(tex, nir_tex_src_texture_offset) >= 0 ||
                nir_tex_instr_src_index(tex, nir_tex_src_texture_deref) >= 0) {
               say(why, why_size, "combined RGB2P filtering with dynamic texture indexing is unsupported");
               return false;
            }
            if (nir->info.stage != MESA_SHADER_COMPUTE && nir->info.stage != MESA_SHADER_FRAGMENT) {
               say(why, why_size, "combined RGB2P filtering requires a compute or fragment stage");
               return false;
            }
            if (reachable == ((1u << RWA_MAX_TEXTURES) - 1) || reachable == ~0u ||
                nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref) >= 0) {
               say(why, why_size, "combined RGB2P filtering requires known constexpr sampler choices");
               return false;
            }
            for (unsigned slot = 0; slot < RWA_MAX_TEXTURES; slot++) {
               if ((reachable & (1u << slot)) &&
                   !rgb2p_inline_filter(rgb2p_inline_sampler(info, slot))) {
                  say(why, why_size, "combined RGB2P filtering requires normalized clamp-to-edge "
                     "constexpr samplers with matching filters, no mip filtering, and default LOD limits");
                  return false;
               }
            }
            int source = nir_tex_instr_src_index(tex, nir_tex_src_coord);
            if ((tex->op != nir_texop_tex && tex->op != nir_texop_txl) ||
                tex->sampler_dim != GLSL_SAMPLER_DIM_2D || tex->is_array ||
                tex->is_shadow || tex->is_sparse || tex->coord_components != 2 || source < 0 ||
                tex->def.num_components != 4 ||
                (tex->def.bit_size != 16 && tex->def.bit_size != 32) ||
                nir_alu_type_get_base_type(tex->dest_type) != nir_type_float) {
               say(why, why_size, "combined RGB2P filtering supports 2D float or half sample operations");
               return false;
            }
            for (unsigned i = 0; i < tex->num_srcs; i++) {
               if (tex->src[i].src_type == nir_tex_src_coord ||
                   tex->src[i].src_type == nir_tex_src_sampler_offset)
                  continue;
               if (tex->src[i].src_type != nir_tex_src_lod ||
                   !nir_src_is_const(tex->src[i].src) || nir_src_as_float(tex->src[i].src) != 0.0f) {
                  say(why, why_size, "combined RGB2P filtering supports level zero without offsets or bias");
                  return false;
               }
            }
            b.cursor = nir_after_instr(instr);
            nir_def *coord = tex->src[source].src.ssa;
            nir_def *result = rgb2p_filter_sample(&b, tex, coord, reachable == cubic);
            if (reachable != cubic && reachable != linear) {
               nir_def *selected = nir_iadd_imm(&b, tex->src[sampler_offset].src.ssa,
                                                  tex->sampler_index);
               nir_def *use_cubic = nir_imm_false(&b);
               for (unsigned slot = 0; slot < RWA_MAX_TEXTURES; slot++) {
                  if (cubic & (1u << slot))
                     use_cubic = nir_ior(&b, use_cubic, nir_ieq_imm(&b, selected, slot));
               }
               result = nir_bcsel(&b, use_cubic, rgb2p_filter_sample(&b, tex, coord, true), result);
            }
            nir_def_rewrite_uses(&tex->def, result);
            nir_instr_remove(instr);
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   nir_validate_shader(nir, "combined RGB2P filter lowering");
   return true;
}
