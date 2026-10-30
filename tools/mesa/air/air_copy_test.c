/* Standalone copy checks against libredwoodair. Pass --software on a host;
 * without it, the library uses its normal GPU selection. Hardware submission
 * counters are still required to distinguish Gallium's CPU fallback. */
#include "air_metal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char why[512];
static unsigned buffer_calls, buffer_success, texture_calls, texture_success;

static int
counted_buffer_copy(struct rwa_buffer *src, size_t src_offset,
                    struct rwa_buffer *dst, size_t dst_offset, size_t size,
                    char *error, size_t error_size)
{
   buffer_calls++;
   int rc = rwa_buffer_copy(src, src_offset, dst, dst_offset, size, error, error_size);
   buffer_success += rc == 0;
   return rc;
}

static int
counted_texture_copy(struct rwa_texture *src, unsigned src_level, unsigned src_slice,
                     const unsigned src_origin[3], struct rwa_texture *dst,
                     unsigned dst_level, unsigned dst_slice, const unsigned dst_origin[3],
                     const unsigned size[3], char *error, size_t error_size)
{
   texture_calls++;
   int rc = rwa_texture_copy(src, src_level, src_slice, src_origin, dst, dst_level,
                            dst_slice, dst_origin, size, error, error_size);
   texture_success += rc == 0;
   return rc;
}
#define rwa_buffer_copy counted_buffer_copy
#define rwa_texture_copy counted_texture_copy

static void
buffer_checks(struct rwa_device *device, struct rwa_device *other)
{
   struct rwa_buffer *a = rwa_buffer_create(device, 1024, why, sizeof(why));
   struct rwa_buffer *b = rwa_buffer_create(device, 1024, why, sizeof(why));
   struct rwa_buffer *c = rwa_buffer_create(device, 1024, why, sizeof(why));
   struct rwa_buffer *foreign = rwa_buffer_create(other, 1024, why, sizeof(why));
   assert(a && b && c && foreign);
   uint8_t *ap = rwa_buffer_map(a), *bp = rwa_buffer_map(b), *cp = rwa_buffer_map(c);
   uint8_t expected[1024];
   for (unsigned i = 0; i < 1024; i++) ap[i] = i * 7 + i / 13;
   memset(bp, 0xa5, 1024);
   memset(expected, 0xa5, sizeof(expected));
   memcpy(expected + 53, ap + 29, 257);
   assert(rwa_buffer_copy(a, 29, b, 53, 257, why, sizeof(why)) == 0);
   assert(memcmp(bp, expected, sizeof(expected)) == 0);
   assert(rwa_buffer_copy(b, 53, c, 64, 257, why, sizeof(why)) == 0);
   assert(memcmp(cp + 64, ap + 29, 257) == 0);
   memcpy(expected, bp, 1024);
   memcpy(expected + 700, bp + 53, 100);
   assert(rwa_buffer_copy(b, 53, b, 700, 100, why, sizeof(why)) == 0);
   assert(memcmp(bp, expected, sizeof(expected)) == 0);
   assert(rwa_buffer_copy(a, SIZE_MAX, b, 0, 1, why, sizeof(why)) != 0);
   assert(rwa_buffer_copy(a, 0, b, 0, SIZE_MAX, why, sizeof(why)) != 0);
   assert(rwa_buffer_copy(a, 0, b, 1024, 1, why, sizeof(why)) != 0);
   assert(rwa_buffer_copy(a, 0, b, 0, 0, why, sizeof(why)) != 0);
   assert(rwa_buffer_copy(b, 53, b, 54, 100, why, sizeof(why)) != 0);
   assert(rwa_buffer_copy(a, 0, foreign, 0, 1, why, sizeof(why)) != 0);
   assert(rwa_buffer_copy(NULL, 0, b, 0, 1, why, sizeof(why)) != 0);
   assert(memcmp(bp, expected, sizeof(expected)) == 0);
   assert(rwa_argbuf_encode_buffer(a, 0, 0, c, 0, why, sizeof(why)) == 0);
   assert(rwa_buffer_copy(a, 0, b, 0, 8, why, sizeof(why)) != 0);
   assert(rwa_buffer_copy(b, 0, a, 0, 8, why, sizeof(why)) != 0);
   assert(memcmp(bp, expected, sizeof(expected)) == 0);
   rwa_buffer_destroy(foreign);
   rwa_buffer_destroy(c);
   rwa_buffer_destroy(b);
   rwa_buffer_destroy(a);
   puts("air_copy_test: buffer offsets, sentinels, dependency and refusals PASS");
}

static void
texture_checks(struct rwa_device *device, struct rwa_device *other, unsigned format,
               unsigned texel)
{
   enum { Width = 19, Height = 11 };
   struct rwa_texture_shape shape = { .pixel_format = format, .type = 3,
      .width = Width, .height = Height, .depth = 1, .array_length = 2,
      .mip_levels = 2, .sample_count = 1, .usage = 7 };
   struct rwa_texture *a = rwa_texture_create(device, &shape, why, sizeof(why));
   struct rwa_texture *b = rwa_texture_create(device, &shape, why, sizeof(why));
   struct rwa_texture *foreign = rwa_texture_create(other, &shape, why, sizeof(why));
   shape.pixel_format = format == 80 ? 115 : 80;
   struct rwa_texture *different = rwa_texture_create(device, &shape, why, sizeof(why));
   assert(a && b && foreign && different);
   size_t write_stride = Width * texel + 16, read_stride = Width * texel + 40;
   uint8_t *input = calloc(Height, write_stride), *clear = calloc(Height, write_stride);
   uint8_t *read = malloc(Height * read_stride), *expected = malloc(Height * read_stride);
   assert(input && clear && read && expected);
   for (unsigned y = 0; y < Height; y++) {
      for (unsigned x = 0; x < Width * texel; x++) input[y * write_stride + x] = x * 3 + y * 17;
      if (format == 115) {
         for (unsigned x = 0; x < Width * 4; x++) {
            uint16_t finite_half = 0x3000 + y * 37 + x * 3;
            memcpy(input + y * write_stride + x * 2, &finite_half, 2);
         }
      }
   }
   unsigned from[3] = { 2, 3, 0 }, to[3] = { 5, 1, 0 }, size[3] = { 7, 5, 1 };
   assert(rwa_texture_write(a, 0, 1, NULL, NULL, input, write_stride, why, sizeof(why)) == 0);
   assert(rwa_texture_write(b, 0, 0, NULL, NULL, clear, write_stride, why, sizeof(why)) == 0);
   assert(rwa_texture_copy(a, 0, 1, from, b, 0, 0, to, size, why, sizeof(why)) == 0);
   memset(expected, 0xcc, Height * read_stride);
   for (unsigned y = 0; y < Height; y++) memset(expected + y * read_stride, 0, Width * texel);
   for (unsigned y = 0; y < size[1]; y++)
      memcpy(expected + (y + to[1]) * read_stride + to[0] * texel,
             input + (y + from[1]) * write_stride + from[0] * texel, size[0] * texel);
   memset(read, 0xcc, Height * read_stride);
   assert(rwa_texture_read(b, 0, 0, NULL, NULL, read, read_stride, why, sizeof(why)) == 0);
   assert(memcmp(read, expected, Height * read_stride) == 0);
   unsigned self_to[3] = { 0, 8, 0 }, self_size[3] = { 3, 2, 1 };
   for (unsigned y = 0; y < self_size[1]; y++)
      memcpy(expected + (self_to[1] + y) * read_stride,
             expected + (to[1] + y) * read_stride + to[0] * texel, self_size[0] * texel);
   assert(rwa_texture_copy(b, 0, 0, to, b, 0, 0, self_to, self_size, why, sizeof(why)) == 0);
   memset(read, 0xcc, Height * read_stride);
   assert(rwa_texture_read(b, 0, 0, NULL, NULL, read, read_stride, why, sizeof(why)) == 0);
   assert(memcmp(read, expected, Height * read_stride) == 0);
   assert(rwa_texture_copy(a, 9, 1, from, b, 0, 0, to, size, why, sizeof(why)) != 0);
   assert(rwa_texture_copy(a, 0, 2, from, b, 0, 0, to, size, why, sizeof(why)) != 0);
   assert(rwa_texture_copy(a, 0, 1, from, foreign, 0, 0, to, size, why, sizeof(why)) != 0);
   assert(rwa_texture_copy(a, 0, 1, from, different, 0, 0, to, size, why, sizeof(why)) != 0);
   assert(rwa_texture_copy(b, 0, 0, to, b, 0, 0, to, size, why, sizeof(why)) != 0);
   unsigned bad[3] = { UINT32_MAX, 0, 0 }, empty[3] = { 0, 5, 1 };
   assert(rwa_texture_copy(a, 0, 1, bad, b, 0, 0, to, size, why, sizeof(why)) != 0);
   assert(rwa_texture_copy(a, 0, 1, from, b, 0, 0, to, empty, why, sizeof(why)) != 0);
   assert(rwa_texture_copy(NULL, 0, 1, from, b, 0, 0, to, size, why, sizeof(why)) != 0);
   memset(read, 0xcc, Height * read_stride);
   assert(rwa_texture_read(b, 0, 0, NULL, NULL, read, read_stride, why, sizeof(why)) == 0);
   assert(memcmp(read, expected, Height * read_stride) == 0);
   free(input); free(clear); free(read); free(expected);
   rwa_texture_destroy(different);
   rwa_texture_destroy(foreign);
   rwa_texture_destroy(b);
   rwa_texture_destroy(a);
   printf("air_copy_test: format %u, array slice, origins, row pitch and refusals PASS\n", format);
}

int main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "--software") == 0) setenv("RWA_SOFTWARE", "1", 1);
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) { fprintf(stderr, "%s\n", why); return 1; }
   struct rwa_device *other = rwa_device_open(why, sizeof(why));
   assert(other);
   printf("air_copy_test: device %s\n", rwa_device_name(device));
   buffer_checks(device, other);
   texture_checks(device, other, 80, 4);
   texture_checks(device, other, 115, 8);
   printf("air_copy_test: buffer calls=%u completed=%u; texture calls=%u completed=%u\n",
          buffer_calls, buffer_success, texture_calls, texture_success);
   rwa_device_close(other);
   rwa_device_close(device);
   return 0;
}
