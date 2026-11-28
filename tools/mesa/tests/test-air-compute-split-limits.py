#!/usr/bin/env python3
"""Host ASan+UBSan unit test for split fixed-workgroup compute dispatch threadgroup guard.

Compiles production air_metal_dispatch.inc directly with lightweight Gallium stubs to
verify that a split fixed-workgroup CSO (e.g. 18x18x1 = 324 threads compiled to max_reg 31
with si.max_threads = 512) is not rejected by a stale unsplit variable-workgroup
compute->caps.max_threads_per_group (256), while unsplit dispatches and split CSOs that
fail compute_state_for_ib() remain rejected.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

AIR_DIR = pathlib.Path(__file__).resolve().parents[1] / "air"

HARNESS = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RWA_LOCKED ((void)0)
#define MESA_SHADER_COMPUTE 5
#define ALIGN_POT(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define PIPE_BARRIER_SHADER_BUFFER   (1u << 0)
#define PIPE_BARRIER_CONSTANT_BUFFER (1u << 1)
#define PIPE_BARRIER_IMAGE           (1u << 2)
#define PIPE_BARRIER_TEXTURE         (1u << 3)

struct rwa_bindings { int dummy; };
struct rwa_buffer_bind { int dummy; };
struct rwa_yuv_key { int dummy; };
struct rwa_bound { int dummy; };
struct rwa_grid_split {
   unsigned group_offset[3];
   unsigned whole_groups[3];
};

struct pipe_grid_info {
   unsigned work_dim;
   unsigned block[3];
   unsigned grid[3];
   unsigned variable_shared_mem;
   struct pipe_resource *indirect;
   unsigned indirect_offset;
};

struct pipe_context {
   void (*bind_compute_state)(struct pipe_context *, void *);
   void (*launch_grid)(struct pipe_context *, const struct pipe_grid_info *);
   void (*memory_barrier)(struct pipe_context *, unsigned);
};

struct rwa_device {
   struct pipe_context *pipe;
   bool software;
   unsigned max_threads_per_group;
};

struct air_shader_info {
   const char *entry_point;
   unsigned imageblock_data_size;
};

struct rwa_compute_caps {
   unsigned max_threads_per_group;
   unsigned static_shared_bytes;
};

struct rwa_compute {
   struct rwa_device *device;
   struct air_shader_info info;
   struct rwa_compute_caps caps;
   bool dynamic_threadgroup;
   unsigned slots[1];
   void *own_sampler[1];
   bool compare_sampler[1];
};

int rwa_dispatch_buffers(struct rwa_compute *compute, const struct rwa_bindings *bindings,
                         const struct rwa_buffer_bind *table, unsigned table_count,
                         const unsigned groups[3], const unsigned group_size[3],
                         char *why, size_t why_size);
int rwa_dispatch_threadgroup(struct rwa_compute *compute, const struct rwa_bindings *bindings,
                             const struct rwa_buffer_bind *table, unsigned table_count,
                             const unsigned groups[3], const unsigned group_size[3],
                             unsigned threadgroup_bytes, char *why, size_t why_size);

static int g_launches = 0;
static unsigned g_last_block[3] = { 0, 0, 0 };
static unsigned g_fake_split_max = 512;
static int g_fake_cso = 1;

static void
say(char *buf, size_t size, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, size, fmt, ap);
   va_end(ap);
}

static void rwa_deferred_drain_locked(struct rwa_device *d) { (void)d; }
static uint64_t device_us(void) { return 0; }
static int finish_or_say_why(struct rwa_device *d, bool w, const char *op, char *why, size_t s)
{
   (void)d; (void)w; (void)op; (void)why; (void)s;
   return 0;
}

static bool
yuv_key_for(const struct air_shader_info *info, const struct rwa_bindings *b,
            const struct rwa_buffer_bind *t, unsigned tc, struct rwa_yuv_key *out,
            char *why, size_t why_size)
{
   (void)info; (void)b; (void)t; (void)tc; (void)why; (void)why_size;
   memset(out, 0, sizeof(*out));
   return true;
}

static int
check_bindings(const struct air_shader_info *info, const unsigned *slots,
               const struct rwa_bindings *b, const struct rwa_buffer_bind *t,
               unsigned tc, const char *name, char *why, size_t why_size)
{
   (void)info; (void)slots; (void)b; (void)t; (void)tc; (void)name; (void)why; (void)why_size;
   return 0;
}

static bool
rgb2p_prepare_bindings(const struct air_shader_info *info, const struct rwa_bindings *b,
                       const struct rwa_buffer_bind *t, unsigned tc, const char *name,
                       char *why, size_t why_size)
{
   (void)info; (void)b; (void)t; (void)tc; (void)name; (void)why; (void)why_size;
   return true;
}

static int
bind_stage(struct rwa_device *d, unsigned stage, const struct air_shader_info *info,
           const unsigned *slots, void *const *own, const bool *cmp,
           const struct rwa_yuv_key *yuv, const struct rwa_bindings *b,
           const struct rwa_buffer_bind *t, unsigned tc, const char *name,
           struct rwa_bound *out, char *why, size_t why_size)
{
   (void)d; (void)stage; (void)info; (void)slots; (void)own; (void)cmp; (void)yuv;
   (void)b; (void)t; (void)tc; (void)name; (void)out; (void)why; (void)why_size;
   return 0;
}

static void
unbind_stage(struct rwa_device *d, unsigned stage, const struct rwa_bound *b)
{
   (void)d; (void)stage; (void)b;
}

static void *
compute_state_for_ib(struct rwa_compute *compute, const unsigned block[3],
                     const struct rwa_yuv_key *yuv, const unsigned ib_dim[2],
                     const struct rwa_grid_split *split, char *why, size_t why_size)
{
   (void)yuv; (void)ib_dim;
   unsigned threads = block[0] * block[1] * block[2];
   if (split) {
      if (threads > g_fake_split_max) {
         say(why, why_size, "%s: compiled for %ux%ux%u threads in a threadgroup, the shader "
                            "still takes at most %u",
             compute->info.entry_point, block[0], block[1], block[2], g_fake_split_max);
         return NULL;
      }
      return &g_fake_cso;
   }
   return &g_fake_cso;
}

static void stub_bind_cs(struct pipe_context *p, void *cso) { (void)p; (void)cso; }
static void stub_barrier(struct pipe_context *p, unsigned flags) { (void)p; (void)flags; }
static void
stub_launch_grid(struct pipe_context *p, const struct pipe_grid_info *grid)
{
   (void)p;
   g_launches++;
   g_last_block[0] = grid->block[0];
   g_last_block[1] = grid->block[1];
   g_last_block[2] = grid->block[2];
}

#include "air_metal_dispatch.inc"

int
main(void)
{
   struct pipe_context pipe = {
      .bind_compute_state = stub_bind_cs,
      .launch_grid = stub_launch_grid,
      .memory_barrier = stub_barrier,
   };
   struct rwa_device dev = {
      .pipe = &pipe,
      .software = false,
      .max_threads_per_group = 1024,
   };
   struct rwa_compute cs = {
      .device = &dev,
      .info = { .entry_point = "kernelRec709DownsampleYFilterOrder2", .imageblock_data_size = 0 },
      /* Stale unsplit variable-workgroup limit (e.g. max_reg 47 -> 256). */
      .caps = { .max_threads_per_group = 256, .static_shared_bytes = 0 },
      .dynamic_threadgroup = false,
   };
   unsigned grid[3] = { 37, 56, 1 };
   unsigned g18[3] = { 18, 18, 1 };
   struct rwa_grid_split sp = { .group_offset = { 0, 0, 0 }, .whole_groups = { 74, 56, 1 } };
   char why[512] = { 0 };

   /* 1. Split fixed-workgroup CSO with si.max_threads=512 must succeed for 18x18x1=324. */
   g_fake_split_max = 512;
   assert(rwa_dispatch_split(&cs, NULL, NULL, 0, grid, g18, 0, NULL, &sp, false,
                             why, sizeof(why)) == 0);
   assert(g_launches == 1 && g_last_block[0] == 18 && g_last_block[1] == 18);

   /* 2. Split fixed-workgroup CSO whose compiled limit is still 256 must be rejected. */
   g_fake_split_max = 256;
   why[0] = 0;
   assert(rwa_dispatch_split(&cs, NULL, NULL, 0, grid, g18, 0, NULL, &sp, false,
                             why, sizeof(why)) != 0);
   assert(strstr(why, "at most 256") != NULL);
   assert(g_launches == 1);

   /* 3. Unsplit dispatch exceeding caps.max_threads_per_group (256) must still be rejected. */
   why[0] = 0;
   assert(rwa_dispatch(&cs, NULL, grid, g18, why, sizeof(why)) != 0);
   assert(strstr(why, "at most 256") != NULL);
   assert(g_launches == 1);

   /* 4. Split dispatch exceeding device->max_threads_per_group (1024) must still be rejected. */
   unsigned g33[3] = { 33, 33, 1 };
   g_fake_split_max = 2048;
   why[0] = 0;
   assert(rwa_dispatch_split(&cs, NULL, NULL, 0, grid, g33, 0, NULL, &sp, false,
                             why, sizeof(why)) != 0);
   assert(strstr(why, "at most 1024") != NULL);
   assert(g_launches == 1);

   puts("PASS: test-air-compute-split-limits (4/4 cases)");
   return 0;
}
"""


def main() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "test_split.c"
        exe = pathlib.Path(tmp) / "test_split"
        src.write_text(HARNESS)
        subprocess.run(
            [
                "clang",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                f"-I{AIR_DIR}",
                str(src),
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
