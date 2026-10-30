/* Run the frozen pipeline pattern and integer/float rounding boundaries through
 * a real softpipe dispatch. The expected half encodings are fixed test inputs. */
#include "air_metal.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char why[512];
#define CHECK(condition) do { if (!(condition)) { \
   fprintf(stderr, "air_half_test:%d: %s: %s\n", __LINE__, #condition, why); exit(1); \
} } while (0)

static struct rwa_compute *
pipeline(struct rwa_device *device, const char *path, const char *entry)
{
   FILE *file = fopen(path, "rb");
   CHECK(file != NULL);
   CHECK(fseek(file, 0, SEEK_END) == 0);
   long size = ftell(file);
   CHECK(size > 0 && fseek(file, 0, SEEK_SET) == 0);
   void *bytes = malloc((size_t)size);
   CHECK(bytes != NULL && fread(bytes, 1, (size_t)size, file) == (size_t)size);
   fclose(file);
   struct rwa_compute *compute = rwa_compute_create(device, bytes, (size_t)size, entry, NULL, why, sizeof(why));
   free(bytes);
   CHECK(compute != NULL);
   return compute;
}

static struct rwa_texture *
target(struct rwa_device *device, unsigned width, unsigned height)
{
   struct rwa_texture_shape shape = { .pixel_format = 115, .type = 2,
      .width = width, .height = height, .depth = 1, .array_length = 1,
      .mip_levels = 1, .sample_count = 1, .usage = 3 };
   struct rwa_texture *texture = rwa_texture_create(device, &shape, why, sizeof(why));
   CHECK(texture != NULL);
   return texture;
}

static void pattern(struct rwa_device *device, const char *path)
{
   struct rwa_compute *compute = pipeline(device, path, "pipeline_pattern");
   struct rwa_texture *texture = target(device, 8, 8);
   struct rwa_bindings bindings = { .textures = &texture, .texture_count = 1 };
   const unsigned groups[3] = { 2, 2, 1 }, block[3] = { 4, 4, 1 };
   CHECK(rwa_dispatch(compute, &bindings, groups, block, why, sizeof(why)) == 0);
   uint16_t actual[8][40];
   memset(actual, 0xcc, sizeof(actual));
   CHECK(rwa_texture_read(texture, 0, 0, NULL, NULL, actual, sizeof(actual[0]), why, sizeof(why)) == 0);
   for (unsigned y = 0; y < 8; y++) {
      for (unsigned x = 0; x < 8; x++) {
         uint16_t expected[4] = { (x & 1) ? 0x3c00 : 0, (y & 1) ? 0x3c00 : 0,
            (((x >> 1) ^ (y >> 1)) & 1) ? 0x3c00 : 0, 0x3c00 };
         if (memcmp(actual[y] + x * 4, expected, sizeof(expected))) {
            fprintf(stderr, "pattern x=%u y=%u got=%04x,%04x,%04x,%04x\n", x, y,
                actual[y][x * 4], actual[y][x * 4 + 1], actual[y][x * 4 + 2], actual[y][x * 4 + 3]);
            CHECK(0);
         }
      }
      for (unsigned i = 32; i < 40; i++) CHECK(actual[y][i] == 0xcccc);
   }
   rwa_compute_destroy(compute);
   rwa_texture_destroy(texture);
   puts("air_half_test: frozen 8x8 pattern, dispatch/readback/padding PASS");
}

static void boundaries(struct rwa_device *device, const char *path)
{
   struct input { uint32_t u; int32_t i; float f; uint32_t pad; };
   static const struct input input[] = {
      { 0, 0, 0.0f, 0 }, { 1, -1, -0.0f, 0 },
      { 2049, -2049, 0x1.002p0f, 0 }, { 2051, -2051, 0x1.006p0f, 0 },
      { 65504, -65504, 65504.0f, 0 }, { 65519, -65519, 65519.0f, 0 },
      { 65520, -65520, 65520.0f, 0 }, { UINT32_MAX, INT32_MIN, -65520.0f, 0 },
      { 3, -3, 0x1.001ffep0f, 0 }, { 4, -4, 0x1.002002p0f, 0 },
      { 5, -5, -0x1.002p0f, 0 }, { 6, -6, -0x1.006p0f, 0 },
      { 7, -7, 0x1p-24f, 0 }, { 8, -8, 0x1p-25f, 0 },
      { 9, -9, 0x1.8p-24f, 0 }, { 10, -10, -0x1p-24f, 0 },
   };
   static const uint16_t expected[][4] = {
      { 0, 0, 0, 0x3c00 }, { 0x3c00, 0xbc00, 0x8000, 0x3c00 },
      { 0x6800, 0xe800, 0x3c00, 0x3c00 }, { 0x6802, 0xe802, 0x3c02, 0x3c00 },
      { 0x7bff, 0xfbff, 0x7bff, 0x3c00 }, { 0x7bff, 0xfbff, 0x7bff, 0x3c00 },
      { 0x7c00, 0xfc00, 0x7c00, 0x3c00 }, { 0x7c00, 0xfc00, 0xfc00, 0x3c00 },
      { 0x4200, 0xc200, 0x3c00, 0x3c00 }, { 0x4400, 0xc400, 0x3c01, 0x3c00 },
      { 0x4500, 0xc500, 0xbc00, 0x3c00 }, { 0x4600, 0xc600, 0xbc02, 0x3c00 },
      { 0x4700, 0xc700, 1, 0x3c00 }, { 0x4800, 0xc800, 0, 0x3c00 },
      { 0x4880, 0xc880, 2, 0x3c00 }, { 0x4900, 0xc900, 0x8001, 0x3c00 },
   };
   struct rwa_compute *compute = pipeline(device, path, "half_boundaries");
   struct rwa_buffer *buffer = rwa_buffer_create(device, sizeof(input), why, sizeof(why));
   CHECK(buffer != NULL);
   memcpy(rwa_buffer_map(buffer), input, sizeof(input));
   struct rwa_texture *texture = target(device, 16, 1);
   struct rwa_bindings bindings = { .buffers = &buffer, .buffer_count = 1,
      .textures = &texture, .texture_count = 1 };
   const unsigned groups[3] = { 4, 1, 1 }, block[3] = { 4, 1, 1 };
   CHECK(rwa_dispatch(compute, &bindings, groups, block, why, sizeof(why)) == 0);
   uint16_t actual[16][4];
   CHECK(rwa_texture_read(texture, 0, 0, NULL, NULL, actual, sizeof(actual), why, sizeof(why)) == 0);
   for (unsigned i = 0; i < 16; i++) {
      for (unsigned j = 0; j < 4; j++) {
         if (actual[i][j] != expected[i][j]) {
            fprintf(stderr, "boundary row=%u channel=%u got=%04x expected=%04x\n",
                i, j, actual[i][j], expected[i][j]);
            CHECK(0);
         }
      }
   }
   rwa_compute_destroy(compute);
   rwa_texture_destroy(texture);
   rwa_buffer_destroy(buffer);
   puts("air_half_test: signed/unsigned integers, half midpoints, overflow, signed zero/subnormals PASS");
}

static void refusal(struct rwa_device *device, const char *path)
{
   struct rwa_compute *compute = pipeline(device, path, "half_unsupported");
   struct rwa_buffer *buffer = rwa_buffer_create(device, 16 * 16, why, sizeof(why));
   CHECK(buffer != NULL);
   memset(rwa_buffer_map(buffer), 0, 16 * 16);
   struct rwa_texture *texture = target(device, 16, 1);
   struct rwa_bindings bindings = { .buffers = &buffer, .buffer_count = 1,
      .textures = &texture, .texture_count = 1 };
   const unsigned groups[3] = { 4, 1, 1 }, block[3] = { 4, 1, 1 };
   CHECK(rwa_dispatch(compute, &bindings, groups, block, why, sizeof(why)) != 0);
   CHECK(strstr(why, "softpipe half lowering does not support ALU fadd") != NULL);
   printf("air_half_test: expected refusal: %s\n", why);
   rwa_compute_destroy(compute);
   rwa_texture_destroy(texture);
   rwa_buffer_destroy(buffer);
}

static void texture_reads(struct rwa_device *device, const char *path)
{
   static const struct { float input; uint16_t expected; } cases[] = {
      {0.0f, 0}, {-0.0f, 0x8000}, {0x1.002p0f, 0x3c00}, {0x1.006p0f, 0x3c02},
      {-0x1.002p0f, 0xbc00}, {-0x1.006p0f, 0xbc02}, {65504.0f, 0x7bff}, {65520.0f, 0x7c00},
      {-65520.0f, 0xfc00}, {0x1p-24f, 1}, {0x1p-25f, 0}, {0x1.8p-24f, 2},
      {-0x1p-24f, 0x8001}, {0x1.001ffep0f, 0x3c00}, {0x1.002002p0f, 0x3c01}, {INFINITY, 0x7c00},
   };
   struct rwa_compute *compute = pipeline(device, path, "read_write_surf_compute");
   for (unsigned wide = 0; wide < 2; wide++) {
      float floats[16][4], float_readback[16][4];
      uint16_t halves[16][4], half_readback[16][4], result[16][4];
      for (unsigned i = 0; i < 16; i++) for (unsigned c = 0; c < 4; c++) {
         floats[i][c] = cases[i].input;
         halves[i][c] = cases[i].expected;
      }
      struct rwa_texture_shape shape = { .pixel_format = wide ? 125 : 115, .type = 2,
         .width = 16, .height = 1, .depth = 1, .array_length = 1,
         .mip_levels = 1, .sample_count = 1, .usage = 1 };
      struct rwa_texture *textures[] = {
         rwa_texture_create(device, &shape, why, sizeof(why)), target(device, 16, 1)
      };
      CHECK(textures[0] != NULL);
      const void *source = wide ? (const void *)floats : (const void *)halves;
      size_t pitch = wide ? sizeof(floats) : sizeof(halves);
      CHECK(rwa_texture_write(textures[0], 0, 0, NULL, NULL, source, pitch, why, sizeof(why)) == 0);
      struct rwa_bindings bindings = { .textures = textures, .texture_count = 2 };
      const unsigned groups[3] = {4, 1, 1}, threads[3] = {4, 1, 1};
      CHECK(rwa_dispatch(compute, &bindings, groups, threads, why, sizeof(why)) == 0);
      CHECK(rwa_texture_read(textures[1], 0, 0, NULL, NULL, result, sizeof(result), why, sizeof(why)) == 0);
      for (unsigned i = 0; i < 16; i++) for (unsigned c = 0; c < 4; c++) {
         if (result[i][c] != cases[i].expected)
            fprintf(stderr, "txf wide=%u row=%u channel=%u got=%04x expected=%04x\n",
                    wide, i, c, result[i][c], cases[i].expected);
         CHECK(result[i][c] == cases[i].expected);
      }
      void *unchanged = wide ? (void *)float_readback : (void *)half_readback;
      CHECK(rwa_texture_read(textures[0], 0, 0, NULL, NULL, unchanged, pitch, why, sizeof(why)) == 0);
      CHECK(memcmp(source, unchanged, pitch) == 0);
      rwa_texture_destroy(textures[1]);
      rwa_texture_destroy(textures[0]);
      printf("air_half_test: real QuartzCore txf source=%s, 16 half boundary rows and unchanged source PASS\n",
             wide ? "RGBA32Float" : "RGBA16Float");
   }
   rwa_compute_destroy(compute);
}

int main(int argc, char **argv)
{
   CHECK(argc == 4 || argc == 5);
   setvbuf(stdout, NULL, _IONBF, 0);
   setenv("RWA_SOFTWARE", "1", 1);
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   CHECK(device != NULL && !strcmp(rwa_device_name(device), "softpipe"));
   puts("air_half_test: backend=softpipe");
   pattern(device, argv[1]);
   boundaries(device, argv[2]);
   refusal(device, argv[3]);
   pattern(device, argv[1]);
   if (argc == 5) texture_reads(device, argv[4]);
   rwa_device_close(device);
   printf("air_half_test: PASS dispatches=%u refused=1 recovery=1\n", argc == 5 ? 5 : 3);
   return 0;
}
