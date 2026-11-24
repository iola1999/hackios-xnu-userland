#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "nir.h"
#include "nir_air.h"
#include "nir_builder.h"

#include "null/null_sw_winsys.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "target-helpers/inline_sw_helper.h"
#include "util/u_inlines.h"

static void *load(const char *path, size_t *len) {
   FILE *f = fopen(path, "rb");
   struct stat sb;
   if (!f || fstat(fileno(f), &sb) || sb.st_size <= 0) {
      perror(path);
      exit(1);
   }
   *len = sb.st_size;
   void *data = malloc(*len);
   if (!data || fread(data, 1, *len, f) != *len) {
      fprintf(stderr, "cannot read %s\n", path);
      exit(1);
   }
   fclose(f);
   return data;
}

static unsigned parse_count(const char *s) {
   char *end;
   errno = 0;
   unsigned long value = strtoul(s, &end, 10);
   if (*s < '0' || *s > '9' || *end || !value || errno || value > UINT_MAX) {
      fprintf(stderr, "invalid draw count: %s\n", s);
      exit(2);
   }
   return value;
}

int main(int argc, char **argv) {
   if (argc != 6) {
      fprintf(stderr, "usage: %s MODULE.bc INPUTS VERTICES INSTANCES OUTPUT.rgba\n", argv[0]);
      return 2;
   }
   unsigned count = parse_count(argv[3]), instances = parse_count(argv[4]);
   size_t size;
   const void *bc = load(argv[1], &size);
   struct pipe_screen *s = sw_screen_create_named(null_sw_create(), "llvmpipe");
   if (!s)
      return 1;
   struct pipe_context *p = s->context_create(s, NULL, 0);
   if (!p)
      return 1;
   printf("screen %s\n", s->get_name(s));
   glsl_type_singleton_init_or_ref();
   void *ctx = ralloc_context(NULL);
   const char *err;
   struct air_constant_value *fc;
   unsigned nfc = air_module_function_constants(ctx, bc, size, &fc);
   if (nfc) {
      fprintf(stderr, "provide a specialized AIR module\n");
      return 1;
   }
   struct air_to_nir_options opt = {0};
   struct air_shader_info info;
   nir_shader *vs =
       air_to_nir(ctx, bc, size, NULL, &opt, s->nir_options[MESA_SHADER_VERTEX], &info, &err);
   if (!vs || info.stage != MESA_SHADER_VERTEX) {
      fprintf(stderr, "not a vertex shader: %s\n", vs ? "" : err);
      return 1;
   }
   if (info.vertex_attributes_read || info.const_win_size) {
      fprintf(stderr, "coverage replay requires direct buffer arguments and vertex/instance IDs\n");
      return 1;
   }
   /* The coverage fragment reads only position. Unused half varyings must be removed
    * before llvmpipe chooses its 32-bit vertex output format. */
   nir_foreach_shader_out_variable(var, vs) {
      if (var->data.location >= VARYING_SLOT_VAR0)
         var->data.mode = nir_var_shader_temp;
   }
   nir_fixup_deref_modes(vs);
   NIR_PASS(_, vs, nir_remove_dead_variables, nir_var_shader_temp, NULL);
   NIR_PASS(_, vs, nir_opt_dce);
   NIR_PASS(_, vs, nir_lower_system_values);
   s->finalize_nir(s, vs, true);
   nir_shader_gather_info(vs, nir_shader_get_entrypoint(vs));
   struct pipe_shader_state vstate = {.type = PIPE_SHADER_IR_NIR, .ir.nir = vs};
   void *v = p->create_vs_state(p, &vstate);
   if (!v)
      return 1;
   p->bind_vs_state(p, v);
   nir_builder fb = nir_builder_init_simple_shader(
       MESA_SHADER_FRAGMENT, s->nir_options[MESA_SHADER_FRAGMENT], "coverage");
   nir_variable *color =
       nir_variable_create(fb.shader, nir_var_shader_out, glsl_vec4_type(), "color");
   color->data.location = FRAG_RESULT_DATA0;
   nir_store_var(&fb, color, nir_imm_vec4(&fb, 1, 1, 1, 1), 0xf);
   fb.shader->num_outputs = 1;
   nir_shader_gather_info(fb.shader, nir_shader_get_entrypoint(fb.shader));
   s->finalize_nir(s, fb.shader, true);
   struct pipe_shader_state fstate = {.type = PIPE_SHADER_IR_NIR, .ir.nir = fb.shader};
   void *fs = p->create_fs_state(p, &fstate);
   if (!fs)
      return 1;
   p->bind_fs_state(p, fs);
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];
      char path[4096];
      if ((r->kind != AIR_RESOURCE_CONSTANT_BUFFER && r->kind != AIR_RESOURCE_DEVICE_BUFFER) ||
          r->argbuf_set != ~0u) {
         fprintf(stderr, "unsupported resource at index %u\n", r->index);
         return 1;
      }
      snprintf(path, sizeof(path), "%s/%u.bin", argv[2], r->index);
      size_t len;
      void *bytes = load(path, &len);
      if (len < r->size || len > UINT_MAX) {
         fprintf(stderr, "invalid buffer extent: %s (%zu bytes, require %u)\n", path, len, r->size);
         return 1;
      }
      unsigned bind = r->kind == AIR_RESOURCE_CONSTANT_BUFFER ? PIPE_BIND_CONSTANT_BUFFER
                                                              : PIPE_BIND_SHADER_BUFFER;
      struct pipe_resource *res = pipe_buffer_create(s, bind, PIPE_USAGE_DEFAULT, len);
      if (!res)
         return 1;
      pipe_buffer_write(p, res, 0, len, bytes);
      free(bytes);
      if (r->kind == AIR_RESOURCE_CONSTANT_BUFFER)
         p->set_constant_buffer(p, MESA_SHADER_VERTEX, r->slot,
                                &(struct pipe_constant_buffer){.buffer = res, .buffer_size = len});
      else
         p->set_shader_buffers(p, MESA_SHADER_VERTEX, r->index, 1,
                               &(struct pipe_shader_buffer){.buffer = res, .buffer_size = len}, 0);
      pipe_resource_reference(&res, NULL);
   }
   unsigned w = 128, h = 128;
   struct pipe_resource tmpl = {.target = PIPE_TEXTURE_2D,
                                .format = PIPE_FORMAT_R8G8B8A8_UNORM,
                                .width0 = w,
                                .height0 = h,
                                .depth0 = 1,
                                .array_size = 1,
                                .bind = PIPE_BIND_RENDER_TARGET};
   struct pipe_resource *t = s->resource_create(s, &tmpl);
   if (!t)
      return 1;
   struct pipe_framebuffer_state target = {.width = w, .height = h, .nr_cbufs = 1};
   pipe_surface_init(p, &target.cbufs[0], t, 0, 0);
   p->set_framebuffer_state(p, &target);
   p->set_viewport_states(
       p, 0, 1, &(struct pipe_viewport_state){.scale = {64, -64, 1}, .translate = {64, 64, 0}});
   p->bind_blend_state(
       p, p->create_blend_state(p, &(struct pipe_blend_state){.rt[0].colormask = PIPE_MASK_RGBA}));
   p->bind_depth_stencil_alpha_state(
       p, p->create_depth_stencil_alpha_state(p, &(struct pipe_depth_stencil_alpha_state){0}));
   p->bind_rasterizer_state(
       p, p->create_rasterizer_state(p, &(struct pipe_rasterizer_state){.half_pixel_center = 1,
                                                                        .bottom_edge_rule = 1,
                                                                        .clip_halfz = 1,
                                                                        .depth_clip_near = 1,
                                                                        .depth_clip_far = 1}));
   p->bind_vertex_elements_state(p, p->create_vertex_elements_state(p, 0, NULL));
   p->clear(p, PIPE_CLEAR_COLOR, 0xf, 0xff, NULL, &(union pipe_color_union){0}, 0, 0);
   p->draw_vbo(p,
               &(struct pipe_draw_info){.mode = MESA_PRIM_TRIANGLES, .instance_count = instances},
               0, NULL, &(struct pipe_draw_start_count_bias){.count = count}, 1);
   struct pipe_transfer *tx;
   void *map = p->texture_map(p, t, 0, PIPE_MAP_READ,
                              &(struct pipe_box){.width = w, .height = h, .depth = 1}, &tx);
   if (!map)
      return 1;
   FILE *out = fopen(argv[5], "wb");
   if (!out) {
      perror(argv[5]);
      return 1;
   }
   for (unsigned y = 0; y < h; y++)
      if (fwrite((char *)map + tx->stride * y, 4, w, out) != w) {
         perror(argv[5]);
         return 1;
      }
   p->texture_unmap(p, tx);
   return fclose(out) != 0;
}
