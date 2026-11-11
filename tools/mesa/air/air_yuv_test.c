/* Exercise production YUV lowering and ownership. Texture reads become constants
 * after checking plane bindings; the host TGSI path lacks separate sampler indices. */
#include "air_metal.c"
#include <math.h>
#define CHECK(x)                                                                                   \
   do {                                                                                            \
      if (!(x)) {                                                                                  \
         fprintf(stderr, "FAIL line %d: %s\n", __LINE__, why);                                     \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)
static const float expected[6][4] = {{.419433594f, .166503906f, 0, 1},
                                     {.444335938f, .179199219f, 0, 1},
                                     {.429835225f, .161063796f, 0, 1},
                                     {.426269531f, .204589844f, .029785156f, 1},
                                     {.447753906f, .215820312f, .019042969f, 1},
                                     {.436028235f, .199930980f, .014883137f, 1}};
int main(int argc, char **argv) {
   char why[1024] = {0};
   CHECK(argc == 3);
   setenv("RWA_SOFTWARE", "1", 1);
   FILE *f = fopen(argv[1], "rb");
   CHECK(f);
   fseek(f, 0, SEEK_END);
   size_t length = ftell(f);
   rewind(f);
   void *air = malloc(length);
   CHECK(fread(air, 1, length, f) == length);
   fclose(f);
   struct rwa_device *d = rwa_device_open(why, sizeof why);
   CHECK(d);
   struct rwa_compute *cs =
       rwa_compute_create(d, air, length, "combined_sample", NULL, why, sizeof why);
   CHECK(cs);
   for (unsigned c = 0; c < 6; c++) {
      nir_shader *n = nir_shader_clone(NULL, cs->nir);
      struct rwa_yuv_key key = {.mask = 1,
                                .full_range = c / 3,
                                .bt709 = (c % 3) == 1,
                                .bt2020 = (c % 3) == 2,
                                .uv_slot = {2}};
      CHECK(lower_yuv(n, &key, why, sizeof why));
      CHECK(BITSET_TEST(n->info.textures_used, 2));
      unsigned y = 0, uv = 0, guard = 0;
      nir_foreach_function_impl(impl, n) {
         nir_builder b = nir_builder_create(impl);
         nir_foreach_block(block, impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type != nir_instr_type_tex)
                  continue;
               nir_tex_instr *tex = nir_instr_as_tex(instr);
               CHECK(nir_tex_instr_src_index(tex, nir_tex_src_plane) < 0);
               if (nir_def_is_unused(&tex->def))
                  continue;
               unsigned idx = tex->texture_index;
               CHECK(idx < 3);
               CHECK(tex->sampler_index == 0);
               if (idx == 0)
                  y++;
               else if (idx == 2)
                  uv++;
               else
                  guard++;
               if (idx != 1)
                  CHECK(tex->op == nir_texop_txl);
               b.cursor = nir_after_instr(instr);
               nir_def *v = idx == 2
                                ? nir_imm_vec4(&b, 96.f / 255, 160.f / 255, 0, 1)
                                : nir_imm_vec4(&b, idx == 0 ? 64.f / 255 : 128.f / 255, 0, 0, 1);
               nir_def_rewrite_uses(&tex->def, v);
            }
         }
      }
      CHECK(y == 1 && uv == 1 && guard == 1);
      while (nir_opt_constant_folding(n)) {
      }
      unsigned stores = 0;
      nir_foreach_function_impl(impl, n) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
               if (in->intrinsic != nir_intrinsic_store_ssbo)
                  continue;
               CHECK(nir_src_is_const(in->src[0]) && nir_src_is_const(in->src[2]));
               unsigned offset = nir_src_as_uint(in->src[2]);
               const nir_const_value *values = nir_src_as_const_value(in->src[0]);
               const float gv[] = {128.f / 255, 0, 0, 1};
               const float *want = offset ? gv : expected[c];
               for (unsigned i = 0; i < 4; i++)
                  CHECK(isfinite(values[i].f32) && fabsf(values[i].f32 - want[i]) < .002f);
               stores++;
            }
         }
      }
      CHECK(stores == 2);
      printf("nir case=%u Y=%u UV=%u guard=%u stores=%u pass\n", c, y, uv, guard, stores);
      ralloc_free(n);
   }
   f = fopen(argv[2], "rb");
   CHECK(f);
   fseek(f, 0, SEEK_END);
   size_t fragment_length = ftell(f);
   rewind(f);
   void *fragment = malloc(fragment_length);
   CHECK(fread(fragment, 1, fragment_length, f) == fragment_length);
   fclose(f);
   struct rwa_render_shape rs = {.color_format = {125}, .color_count = 1, .sample_count = 1};
   struct rwa_render *render = rwa_render_create(d, NULL, 0, NULL, fragment, fragment_length,
       "combined_fragment", &rs, NULL, why, sizeof why);
   CHECK(render);
   nir_shader *fs = nir_shader_clone(NULL, render->fragment_nir);
   struct rwa_yuv_key fragment_key = {.mask = 1, .uv_slot = {1}};
   CHECK(lower_yuv(fs, &fragment_key, why, sizeof why));
   finalize_for_gallium(fs, d->screen);
   /* IR3 uses this mask to size the fragment texture descriptors. */
   CHECK(BITSET_LAST_BIT(fs->info.textures_used) == 2);
   unsigned fragment_planes = 0;
   nir_foreach_function_impl(impl, fs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            if (nir_def_is_unused(&tex->def))
               continue;
            CHECK(tex->texture_index < 2 && tex->sampler_index == 0);
            CHECK(BITSET_TEST(fs->info.textures_used, tex->texture_index));
            fragment_planes |= 1u << tex->texture_index;
         }
      }
   }
   CHECK(fragment_planes == 3);
   ralloc_free(fs);
   rwa_render_destroy(render);
   free(fragment);
   puts("fragment Y/UV descriptor mask passed");
   struct rwa_texture_shape s = {.pixel_format = 10,
                                 .type = 2,
                                 .width = 32,
                                 .height = 16,
                                 .depth = 1,
                                 .array_length = 1,
                                 .mip_levels = 1,
                                 .sample_count = 1,
                                 .usage = 1};
   struct rwa_texture *y = rwa_texture_create(d, &s, why, sizeof why);
   CHECK(y);
   s.pixel_format = 30;
   s.width = 16;
   s.height = 8;
   struct rwa_texture *uv = rwa_texture_create(d, &s, why, sizeof why);
   CHECK(uv);
   struct rwa_texture *t = rwa_texture_combine_yuv(y, uv, false, 6, why, sizeof why);
   CHECK(t);
   CHECK(y->references == 2 && uv->references == 2);
   struct rwa_texture *t2020 = rwa_texture_combine_yuv(y, uv, false, 9, why, sizeof why);
   CHECK(t2020 && t2020->yuv_bt2020 && !t2020->yuv_bt709);
   rwa_texture_destroy(t2020);
   struct rwa_texture_shape s10 = s;
   s10.pixel_format = 20;
   s10.width = 32;
   s10.height = 16;
   struct rwa_texture *y10 = rwa_texture_create(d, &s10, why, sizeof why);
   CHECK(y10);
   s10.pixel_format = 60;
   s10.width = 16;
   s10.height = 8;
   struct rwa_texture *uv10 = rwa_texture_create(d, &s10, why, sizeof why);
   CHECK(uv10);
   struct rwa_texture *t10 = rwa_texture_combine_yuv(y10, uv10, false, 9, why, sizeof why);
   CHECK(t10 && t10->shape.pixel_format == 505 && t10->yuv_bt2020);
   rwa_texture_destroy(t10);
   rwa_texture_destroy(y10);
   rwa_texture_destroy(uv10);
   CHECK(!rwa_texture_combine_yuv(y, uv, false, 99, why, sizeof why));
   CHECK(!rwa_texture_view(t, 10, 0, 1, why, sizeof why));
   unsigned level = 0;
   CHECK(!resource_level(t, &level, why, sizeof why));
   struct rwa_texture *textures[] = {t, y};
   struct rwa_bindings bindings = {.textures = textures, .texture_count = 2};
   struct rwa_yuv_key key;
   CHECK(yuv_key_for(&cs->info, &bindings, NULL, 0, &key, why, sizeof why));
   CHECK(key.mask == 1 && key.uv_slot[0] == 2);
   rwa_texture_destroy(y);
   rwa_texture_destroy(uv);
   CHECK(t->storage_owner->references == 1 && t->chroma->references == 1);
   rwa_texture_destroy(t);
   nir_shader *bad = nir_shader_clone(NULL, cs->nir);
   nir_foreach_function_impl(impl, bad) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               nir_instr_as_tex(instr)->op = nir_texop_txf;
               break;
            }
         }
      }
   }
   CHECK(!lower_yuv(bad, &key, why, sizeof why));
   ralloc_free(bad);
   rwa_compute_destroy(cs);
   rwa_device_close(d);
   free(air);
   puts("ownership, binding collision, unsupported matrix/view/read passed");
   return 0;
}
