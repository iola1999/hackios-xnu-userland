/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
#include "air_metal_reflection.h"

static unsigned errors;
static char why[512];
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (%s)\n", \
   __LINE__, #x, why); errors++; goto cleanup; } } while (0)

static struct rwa_stitch_node
module(const char *dir, const char *name, const int *args, unsigned nargs)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s.bc", dir, name);
   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); exit(2); }
   fseek(f, 0, SEEK_END);
   size_t size = ftell(f);
   rewind(f);
   void *bytes = malloc(size);
   if (fread(bytes, 1, size, f) != size) exit(2);
   fclose(f);
   return (struct rwa_stitch_node) {
      .bitcode = bytes, .size = size, .function = name, .args = args, .num_args = nargs,
   };
}

static void
original_ci(const char *dir)
{
   const int a0[] = {0, 1}, a1[] = {-1}, a2[] = {0, 2, 1};
   const int a3[] = {3, 4, -3, 5}, a4[] = {-4, 6, -3}, a5[] = {7, -5, 0, 2};
   const unsigned after[] = {1};
   struct rwa_stitch_node nodes[] = {
      module(dir, "_ci_early_out", a0, 2), {.num_args = 1, .args = a1},
      module(dir, "_ci_init_destcoord", a2, 3), module(dir, "_ci_read_pixel", a3, 4),
      module(dir, "_ci_crop", a4, 3), module(dir, "_ci_write_pixel", a5, 4),
   };
   for (unsigned i = 2; i < 6; i++) { nodes[i].after = after; nodes[i].num_after = 1; }
   struct rwa_stitch_input inputs[] = {
      {.kind = RWA_STITCH_THREAD_POSITION},
      {.kind = RWA_STITCH_BUFFER_ADDRESS, .binding = 2, .byte_offset = 16, .dereference = 1},
      {.kind = RWA_STITCH_BUFFER_ADDRESS, .binding = 2, .byte_offset = 80, .dereference = 1},
      {.kind = RWA_STITCH_TEXTURE, .binding = 1},
      {.kind = RWA_STITCH_SAMPLER, .binding = 1},
      {.kind = RWA_STITCH_BUFFER_ADDRESS, .binding = 2, .byte_offset = 96, .dereference = 1},
      {.kind = RWA_STITCH_BUFFER_ADDRESS, .binding = 2, .byte_offset = 144, .dereference = 1},
      {.kind = RWA_STITCH_TEXTURE, .binding = 3},
   };
   struct rwa_stitched_kernel kernel = {
      .graph = {.name = "ci_affine_crop", .num_nodes = 6, .nodes = nodes, .output = UINT_MAX},
      .num_inputs = 8, .inputs = inputs,
   };
   struct rwa_device *d = rwa_device_open(why, sizeof(why));
   struct rwa_compute *c = NULL;
   struct rwa_buffer *buffer = NULL;
   struct rwa_texture *src = NULL, *dst = NULL;
   struct rwa_sampler *sampler = NULL;
   CHECK(d);
   c = rwa_compute_create_stitched(d, &kernel, NULL, why, sizeof(why));
   CHECK(c);
   const struct rwa_compute_reflection *ref;
   CHECK(rwa_compute_get_reflection(c, &ref, why, sizeof(why)) == 0);
   CHECK(ref->argument_count == 4);
   for (unsigned i = 0; i < ref->argument_count; i++)
      if (ref->arguments[i].type == 0)
         CHECK(ref->arguments[i].buffer_type->alignment == 4);
   float constants[40] = {0};
   const float transform4[] = {1,0,0,19, 0,1,0,29, 0,0,1,39, 1,1,6,3};
   const float rect[] = {0,0,8,4};
   const float transform3[] = {0.125f,0,0,101, 0,0.25f,0,202, 0,0,1,303};
   const float crop[] = {0,6,0,3};
   memcpy((char *)constants + 16, transform4, sizeof(transform4));
   memcpy((char *)constants + 80, rect, sizeof(rect));
   memcpy((char *)constants + 96, transform3, sizeof(transform3));
   memcpy((char *)constants + 144, crop, sizeof(crop));
   buffer = rwa_buffer_create(d, sizeof(constants), why, sizeof(why));
   CHECK(buffer);
   memcpy(rwa_buffer_map(buffer), constants, sizeof(constants));
   struct rwa_texture_shape shape = {.pixel_format = 125, .type = 2, .width = 8,
      .height = 4, .depth = 1, .array_length = 1, .mip_levels = 1, .sample_count = 1, .usage = 3};
   src = rwa_texture_create(d, &shape, why, sizeof(why));
   dst = rwa_texture_create(d, &shape, why, sizeof(why));
   CHECK(src && dst);
   float source[128], pixels[128];
   for (unsigned y = 0; y < 4; y++)
      for (unsigned x = 0; x < 8; x++) {
         unsigned p = (y * 8 + x) * 4;
         source[p] = x + 1; source[p + 1] = y + 2;
         source[p + 2] = x + y + 0.25f; source[p + 3] = 1;
      }
   for (unsigned i = 0; i < 128; i++) pixels[i] = -20;
   const unsigned origin[] = {0, 0, 0}, size[] = {8, 4, 1}, groups[] = {1, 1, 1};
   CHECK(rwa_texture_write(src, 0, 0, origin, size, source, 128, why, sizeof(why)) == 0);
   CHECK(rwa_texture_write(dst, 0, 0, origin, size, pixels, 128, why, sizeof(why)) == 0);
   struct rwa_sampler_state state = {.address = {0,0,0}, .max_anisotropy = 1,
      .normalized_coordinates = 1, .lod_max = 1000};
   sampler = rwa_sampler_create(d, &state, why, sizeof(why));
   CHECK(sampler);
   struct rwa_buffer *buffers[] = {NULL, NULL, buffer};
   struct rwa_texture *textures[] = {NULL, src, NULL, dst};
   struct rwa_sampler *samplers[] = {NULL, sampler};
   struct rwa_bindings bindings = {.buffers = buffers, .buffer_count = 3,
      .textures = textures, .texture_count = 4, .samplers = samplers, .sampler_count = 2};
   CHECK(rwa_dispatch(c, &bindings, groups, size, why, sizeof(why)) == 0);
   CHECK(rwa_texture_read(dst, 0, 0, origin, size, pixels, 128, why, sizeof(why)) == 0);
   for (unsigned y = 0; y < 4; y++)
      for (unsigned x = 0; x < 8; x++)
         for (unsigned ch = 0; ch < 4; ch++) {
            unsigned p = (y * 8 + x) * 4 + ch;
            float factor = (x == 0 || x == 5 ? 0.5f : 1.0f) * (y == 0 || y == 2 ? 0.5f : 1.0f);
            float expected = x < 6 && y < 3 ? source[p] * factor : -20;
            if (pixels[p] != expected)
               fprintf(stderr, "CI pixel %u,%u,%u: %g expected %g\n", x, y, ch, pixels[p], expected);
            CHECK(pixels[p] == expected);
         }
   CHECK(!memcmp(rwa_buffer_map(buffer), constants, sizeof(constants)));
   puts("PASS original CI five functions, six nodes, float4x4/float3x3, sampling/crop/store");
cleanup:
   if (c) rwa_compute_destroy(c);
   if (sampler) rwa_sampler_destroy(sampler);
   if (src) rwa_texture_destroy(src);
   if (dst) rwa_texture_destroy(dst);
   if (buffer) rwa_buffer_destroy(buffer);
   if (d) rwa_device_close(d);
   for (unsigned i = 0; i < 6; i++) free((void *)nodes[i].bitcode);
}

int main(int argc, char **argv)
{
   if (argc != 2 && argc != 3) return 2;
   setenv("RWA_SOFTWARE", "1", 1);
   struct rwa_device *d = rwa_device_open(why, sizeof(why));
   struct rwa_compute *c = NULL;
   struct rwa_buffer *buffer = NULL;
   struct rwa_texture *tex = NULL;
   const int stop_args[] = {0, 1}, early_args[] = {-1}, read_args[] = {2};
   const int store_args[] = {3, -3, 0};
   const unsigned after[] = {1};
   struct rwa_stitch_node nodes[] = {
      module(argv[1], "stop", stop_args, 2),
      {.num_args = 1, .args = early_args},
      module(argv[1], "read_value", read_args, 1),
      module(argv[1], "store", store_args, 3),
   };
   nodes[2].after = after; nodes[2].num_after = 1;
   nodes[3].after = after; nodes[3].num_after = 1;
   struct rwa_stitch_input inputs[] = {
      {.kind = RWA_STITCH_THREAD_POSITION},
      {.kind = RWA_STITCH_BUFFER_ADDRESS, .binding = 2, .byte_offset = 4, .dereference = 1},
      {.kind = RWA_STITCH_BUFFER_ADDRESS, .binding = 2, .byte_offset = 8},
      {.kind = RWA_STITCH_TEXTURE, .binding = 3},
   };
   struct rwa_stitched_kernel kernel = {
      .graph = {.name = "test_kernel", .num_nodes = 4, .nodes = nodes, .output = UINT_MAX},
      .num_inputs = 4, .inputs = inputs,
   };
   CHECK(d);
   c = rwa_compute_create_stitched(d, &kernel, NULL, why, sizeof(why));
   CHECK(c);
   const struct rwa_compute_reflection *ref;
   CHECK(rwa_compute_get_reflection(c, &ref, why, sizeof(why)) == 0);
   CHECK(ref->argument_count == 2);
   for (unsigned i = 0; i < ref->argument_count; i++) {
      const struct rwa_reflection_argument *a = &ref->arguments[i];
      CHECK(a->name && !a->name[0]);
      if (a->type == 0) {
         CHECK(a->index == 2 && a->constant_buffer && a->access == 0);
         CHECK(a->buffer_type && a->buffer_type->data_type == 0 && a->buffer_type->data_size == 0);
         CHECK(a->buffer_type->alignment == 4);
      } else {
         CHECK(a->type == 2 && a->index == 3 && a->access == 2);
         CHECK(a->texture_type == 2 && a->texture_data_type == 3);
      }
   }
   buffer = rwa_buffer_create(d, 16, why, sizeof(why));
   CHECK(buffer);
   uint32_t words[] = {0xeeeeeeee, 4, 0x40200000, 0xdddddddd};
   memcpy(rwa_buffer_map(buffer), words, sizeof(words));
   struct rwa_texture_shape shape = {.pixel_format = 55, .type = 2, .width = 8,
      .height = 1, .depth = 1, .array_length = 1, .mip_levels = 1, .sample_count = 1, .usage = 3};
   tex = rwa_texture_create(d, &shape, why, sizeof(why));
   CHECK(tex);
   float pixels[8];
   for (unsigned i = 0; i < 8; i++) pixels[i] = -20.0f;
   const unsigned origin[] = {0, 0, 0}, size[] = {8, 1, 1}, groups[] = {1, 1, 1};
   CHECK(rwa_texture_write(tex, 0, 0, origin, size, pixels, sizeof(pixels), why, sizeof(why)) == 0);
   struct rwa_buffer *buffers[] = {NULL, NULL, buffer};
   struct rwa_texture *textures[] = {NULL, NULL, NULL, tex};
   struct rwa_bindings bindings = {.buffers = buffers, .buffer_count = 3,
      .textures = textures, .texture_count = 4};
   CHECK(rwa_dispatch(c, &bindings, groups, size, why, sizeof(why)) == 0);
   CHECK(rwa_texture_read(tex, 0, 0, origin, size, pixels, sizeof(pixels), why, sizeof(why)) == 0);
   for (unsigned i = 0; i < 8; i++) {
      printf("pixel %u: %.2f\n", i, pixels[i]);
      CHECK(pixels[i] == (i < 4 ? 2.5f + i : -20.0f));
   }
   CHECK(!memcmp(rwa_buffer_map(buffer), words, sizeof(words)));
   rwa_compute_destroy(c); c = NULL;
   puts("PASS special bindings, constant offsets, void store, early return, reflection");

   /* Every invalid reference must be rejected before a results-array access. */
   const int bad_args[][2] = {{INT_MIN, 1}, {-1, 1}, {-4, 1}, {4, 1}};
   for (unsigned i = 0; i < sizeof(bad_args) / sizeof(bad_args[0]); i++) {
      nodes[0].args = bad_args[i];
      c = rwa_compute_create_stitched(d, &kernel, NULL, why, sizeof(why));
      CHECK(!c);
   }
   nodes[0].args = stop_args;
   unsigned bad_after = 99;
   nodes[2].after = &bad_after;
   c = rwa_compute_create_stitched(d, &kernel, NULL, why, sizeof(why));
   CHECK(!c);
   bad_after = 3; /* read -> store -> read is a cycle */
   c = rwa_compute_create_stitched(d, &kernel, NULL, why, sizeof(why));
   CHECK(!c);
   nodes[2].after = after;
   inputs[1].byte_offset = UINT64_MAX;
   c = rwa_compute_create_stitched(d, &kernel, NULL, why, sizeof(why));
   CHECK(!c);
   inputs[1].byte_offset = 4;
   struct rwa_stitch_node huge = module(argv[1], "stop_huge", stop_args, 2);
   huge.function = "stop";
   struct rwa_stitch_node saved = nodes[2];
   nodes[2] = huge;
   kernel.graph.num_nodes = 3;
   c = rwa_compute_create_stitched(d, &kernel, NULL, why, sizeof(why));
   free((void *)huge.bitcode);
   nodes[2] = saved;
   CHECK(!c);
   CHECK(strstr(why, "invalid buffer copy/load helpers") != NULL);
   puts("PASS invalid input/node indices, forward references, cycle, byte/scratch overflow");
cleanup:
   if (c) rwa_compute_destroy(c);
   if (tex) rwa_texture_destroy(tex);
   if (buffer) rwa_buffer_destroy(buffer);
   if (d) rwa_device_close(d);
   for (unsigned i = 0; i < 4; i++) free((void *)nodes[i].bitcode);
   if (!errors && argc == 3) original_ci(argv[2]);
   return errors ? 1 : 0;
}
