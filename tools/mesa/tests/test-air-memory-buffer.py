#!/usr/bin/env python3
"""Host ASan+UBSan unit test for host_bytes zero-extra-mapping path in air_metal_texture.inc.

Extracts buffer_import_handle, rwa_buffer_from_memory, rwa_buffer_from_surface,
buffer_texture_bytes, rwa_buffer_map, and rwa_buffer_destroy directly from the
production AIR sources and verifies pointer reuse, xfer == NULL, failure cleanup,
and IOSurface pipe_buffer_map fallback under ASan+UBSan.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile

AIR_DIR = pathlib.Path(__file__).resolve().parents[1] / "air"
TEXTURE_INC = AIR_DIR / "air_metal_texture.inc"
AIR_METAL_C = AIR_DIR / "air_metal.c"

HARNESS_HEAD = r"""
#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioccom.h>

#define RWA_LOCKED ((void)0)

static pthread_mutex_t rwa_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

enum {
   PIPE_BUFFER = 0,
   PIPE_FORMAT_R8_UNORM = 1,
   PIPE_USAGE_STAGING = 2,
   PIPE_BIND_SHADER_BUFFER = 1u << 0,
   PIPE_BIND_CONSTANT_BUFFER = 1u << 1,
   PIPE_BIND_INDEX_BUFFER = 1u << 2,
   PIPE_BIND_SHARED = 1u << 3,
   PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE = 1u << 0,
   PIPE_MAP_READ = 1u << 0,
   PIPE_MAP_WRITE = 1u << 1,
   WINSYS_HANDLE_TYPE_FD = 1,
   WINSYS_HANDLE_TYPE_KMS = 2,
};

struct winsys_handle {
   unsigned type;
   unsigned handle;
   unsigned stride;
   size_t size;
};

struct pipe_resource {
   unsigned target;
   unsigned format;
   unsigned usage;
   unsigned bind;
   unsigned width0;
   unsigned height0;
   unsigned depth0;
   unsigned array_size;
   unsigned refs;
};

struct pipe_transfer {
   int dummy;
};

struct pipe_context {
   int dummy;
};

struct pipe_screen {
   struct pipe_resource *(*resource_from_handle)(struct pipe_screen *screen,
                                                 const struct pipe_resource *templ,
                                                 struct winsys_handle *whandle,
                                                 unsigned usage);
};

struct rwa_device {
   struct pipe_screen *screen;
   struct pipe_context *pipe;
   bool software;
};

struct rwa_texture {
   struct pipe_resource *res;
};

struct rwa_buffer {
   unsigned references;
   struct rwa_device *device;
   struct pipe_resource *res;
   struct pipe_transfer *xfer;
   void *map;
   size_t size;
};

struct rwa_buffer_texture {
   struct rwa_texture *texture;
   struct rwa_buffer *source;
   size_t offset;
   unsigned row_bytes;
};

static unsigned g_ioctl_0x7c_calls = 0;
static unsigned g_ioctl_0x09_calls = 0;
static uint64_t g_last_ioctl_addr = 0;
static uint64_t g_last_ioctl_size = 0;
static uint32_t g_next_kms_handle = 42;
static uint32_t g_last_closed_handle = 0;
static bool g_fail_ioctl_0x7c = false;
static int g_ioctl_0x7c_errno = EINVAL;

static unsigned g_import_calls = 0;
static unsigned g_last_whandle_type = 0;
static unsigned g_last_whandle_handle = 0;
static bool g_fail_import = false;

static unsigned g_pipe_map_calls = 0;
static unsigned g_pipe_unmap_calls = 0;
static unsigned g_res_freed = 0;
static bool g_fail_pipe_map = false;
static uint8_t g_surface_backing[4096];

static void
say(char *buf, size_t size, const char *fmt, ...)
{
   if (!buf || size == 0)
      return;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, size, fmt, ap);
   va_end(ap);
}

static int
drmIoctl(int fd, unsigned long request, void *arg)
{
   (void)fd;
   unsigned long code = request & 0xffu;
   if (code == 0x7cu) {
      struct {
         uint64_t address, size;
         uint32_t handle, pad;
      } *req = arg;
      g_ioctl_0x7c_calls++;
      g_last_ioctl_addr = req->address;
      g_last_ioctl_size = req->size;
      if (g_fail_ioctl_0x7c) {
         errno = g_ioctl_0x7c_errno;
         return -1;
      }
      req->handle = g_next_kms_handle;
      return 0;
   }
   if (code == 0x09u) {
      struct {
         uint32_t handle, pad;
      } *close_arg = arg;
      g_ioctl_0x09_calls++;
      g_last_closed_handle = close_arg->handle;
      return 0;
   }
   errno = ENOTTY;
   return -1;
}

static struct pipe_resource *
stub_resource_from_handle(struct pipe_screen *screen, const struct pipe_resource *templ,
                          struct winsys_handle *whandle, unsigned usage)
{
   (void)screen;
   (void)usage;
   g_import_calls++;
   g_last_whandle_type = whandle->type;
   g_last_whandle_handle = whandle->handle;
   if (g_fail_import)
      return NULL;
   struct pipe_resource *res = calloc(1, sizeof(*res));
   assert(res != NULL);
   *res = *templ;
   res->refs = 1;
   return res;
}

static void *
pipe_buffer_map(struct pipe_context *pipe, struct pipe_resource *res, unsigned usage,
                struct pipe_transfer **out_xfer)
{
   (void)pipe;
   (void)res;
   (void)usage;
   g_pipe_map_calls++;
   if (g_fail_pipe_map) {
      *out_xfer = NULL;
      return NULL;
   }
   struct pipe_transfer *xfer = calloc(1, sizeof(*xfer));
   assert(xfer != NULL);
   *out_xfer = xfer;
   return g_surface_backing;
}

static void
pipe_buffer_unmap(struct pipe_context *pipe, struct pipe_transfer *xfer)
{
   (void)pipe;
   g_pipe_unmap_calls++;
   free(xfer);
}

static void
pipe_resource_reference(struct pipe_resource **dst, struct pipe_resource *src)
{
   assert(src == NULL);
   if (*dst) {
      assert((*dst)->refs > 0);
      if (--(*dst)->refs == 0) {
         free(*dst);
         g_res_freed++;
      }
      *dst = NULL;
   }
}

static void argbuf_scrub(struct rwa_device *d, struct rwa_buffer *b, void *t, void *s)
{
   (void)d;
   (void)b;
   (void)t;
   (void)s;
}

static void argbuf_forget_all(struct rwa_buffer *b) { (void)b; }
static void buffer_textures_forget(struct rwa_buffer *b) { (void)b; }
static void rwa_deferred_drain_locked(struct rwa_device *d) { (void)d; }
static void rwa_deferred_defer_buffer(struct rwa_device *d, struct pipe_transfer *x,
                                      struct pipe_resource *r)
{
   (void)d;
   (void)x;
   (void)r;
   assert(0 && "rwa_deferred_defer_buffer must not run on unlocked host test");
}
"""

HARNESS_TESTS = r"""
static void
reset_counters(void)
{
   g_ioctl_0x7c_calls = 0;
   g_ioctl_0x09_calls = 0;
   g_last_ioctl_addr = 0;
   g_last_ioctl_size = 0;
   g_last_closed_handle = 0;
   g_fail_ioctl_0x7c = false;
   g_import_calls = 0;
   g_last_whandle_type = 0;
   g_last_whandle_handle = 0;
   g_fail_import = false;
   g_pipe_map_calls = 0;
   g_pipe_unmap_calls = 0;
   g_res_freed = 0;
   g_fail_pipe_map = false;
}

static struct rwa_device
make_device(struct pipe_screen *screen, struct pipe_context *pipe)
{
   screen->resource_from_handle = stub_resource_from_handle;
   return (struct rwa_device){ .screen = screen, .pipe = pipe, .software = false };
}

static void
test_memory_buffer_reuses_host_bytes(void)
{
   struct pipe_screen screen = { 0 };
   struct pipe_context pipe = { 0 };
   struct rwa_device dev = make_device(&screen, &pipe);
   char why[256] = { 0 };

   void *host_bytes = NULL;
   assert(posix_memalign(&host_bytes, 4096, 4096) == 0);
   memset(host_bytes, 0x5a, 4096);

   reset_counters();
   struct rwa_buffer *buf = rwa_buffer_from_memory(&dev, host_bytes, 4096, why, sizeof(why));
   assert(buf != NULL);
   assert(g_ioctl_0x7c_calls == 1);
   assert(g_last_ioctl_addr == (uint64_t)(uintptr_t)host_bytes);
   assert(g_last_ioctl_size == 4096);
   assert(g_import_calls == 1);
   assert(g_last_whandle_type == WINSYS_HANDLE_TYPE_KMS);
   assert(g_last_whandle_handle == g_next_kms_handle);
   assert(g_pipe_map_calls == 0);
   assert(buf->xfer == NULL);
   assert(buf->map == host_bytes);
   assert(rwa_buffer_map(buf) == host_bytes);
   assert(buf->size == 4096);
   assert(buf->references == 1);

   ((uint8_t *)host_bytes)[256] = 0xc3;
   assert(((uint8_t *)rwa_buffer_map(buf))[256] == 0xc3);

   struct pipe_resource tex_res = { .height0 = 4 };
   struct rwa_texture tex = { .res = &tex_res };
   struct rwa_buffer_texture bt = {
      .texture = &tex, .source = buf, .offset = 256, .row_bytes = 64
   };
   char *alias_bytes = buffer_texture_bytes(&bt, why, sizeof(why));
   assert(alias_bytes == (char *)host_bytes + 256);

   rwa_buffer_destroy(buf);
   assert(g_pipe_unmap_calls == 0);
   assert(g_res_freed == 1);
   assert(g_ioctl_0x09_calls == 0);

   /* Caller retains ownership of host_bytes; ASan catches any invalid free in destroy. */
   assert(((uint8_t *)host_bytes)[256] == 0xc3);
   ((uint8_t *)host_bytes)[256] = 0x7e;
   free(host_bytes);
}

static void
test_memory_buffer_failure_paths(void)
{
   struct pipe_screen screen = { 0 };
   struct pipe_context pipe = { 0 };
   struct rwa_device dev = make_device(&screen, &pipe);
   char why[256] = { 0 };

   void *host_bytes = NULL;
   assert(posix_memalign(&host_bytes, 4096, 4096) == 0);

   reset_counters();
   assert(rwa_buffer_from_memory(&dev, NULL, 4096, why, sizeof(why)) == NULL);
   assert(g_ioctl_0x7c_calls == 0);
   assert(strstr(why, "not whole 4 KB pages") != NULL);

   reset_counters();
   assert(rwa_buffer_from_memory(&dev, (char *)host_bytes + 16, 4096, why, sizeof(why)) == NULL);
   assert(g_ioctl_0x7c_calls == 0);

   reset_counters();
   assert(rwa_buffer_from_memory(&dev, host_bytes, 1024, why, sizeof(why)) == NULL);
   assert(g_ioctl_0x7c_calls == 0);

   reset_counters();
   g_fail_ioctl_0x7c = true;
   assert(rwa_buffer_from_memory(&dev, host_bytes, 4096, why, sizeof(why)) == NULL);
   assert(g_ioctl_0x7c_calls == 1);
   assert(g_import_calls == 0);
   assert(g_ioctl_0x09_calls == 0);

   reset_counters();
   g_fail_import = true;
   assert(rwa_buffer_from_memory(&dev, host_bytes, 4096, why, sizeof(why)) == NULL);
   assert(g_ioctl_0x7c_calls == 1);
   assert(g_import_calls == 1);
   assert(g_pipe_map_calls == 0);
   assert(g_ioctl_0x09_calls == 1);
   assert(g_last_closed_handle == g_next_kms_handle);
   assert(strstr(why, "imported") != NULL);

   dev.software = true;
   reset_counters();
   assert(rwa_buffer_from_memory(&dev, host_bytes, 4096, why, sizeof(why)) == NULL);
   assert(g_ioctl_0x7c_calls == 0);

   free(host_bytes);
}

static void
test_surface_buffer_preserves_pipe_map(void)
{
   struct pipe_screen screen = { 0 };
   struct pipe_context pipe = { 0 };
   struct rwa_device dev = make_device(&screen, &pipe);
   char why[256] = { 0 };

   reset_counters();
   struct rwa_buffer *buf = rwa_buffer_from_surface(&dev, 7, 4096, why, sizeof(why));
   assert(buf != NULL);
   assert(g_ioctl_0x7c_calls == 0);
   assert(g_import_calls == 1);
   assert(g_last_whandle_type == WINSYS_HANDLE_TYPE_FD);
   assert(g_last_whandle_handle == 7);
   assert(g_pipe_map_calls == 1);
   assert(buf->xfer != NULL);
   assert(buf->map == g_surface_backing);
   assert(rwa_buffer_map(buf) == g_surface_backing);

   rwa_buffer_destroy(buf);
   assert(g_pipe_unmap_calls == 1);
   assert(g_res_freed == 1);
   assert(g_ioctl_0x09_calls == 0);

   reset_counters();
   g_fail_pipe_map = true;
   assert(rwa_buffer_from_surface(&dev, 9, 4096, why, sizeof(why)) == NULL);
   assert(g_pipe_map_calls == 1);
   assert(g_res_freed == 1);
   assert(g_ioctl_0x09_calls == 0);
   assert(strstr(why, "mapped") != NULL);
}

int
main(void)
{
   test_memory_buffer_reuses_host_bytes();
   test_memory_buffer_failure_paths();
   test_surface_buffer_preserves_pipe_map();
   printf("test-air-memory-buffer: PASS\n");
   return 0;
}
"""


def extract_slice(text: str, start_marker: str, end_marker: str) -> str:
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    return text[start:end]


def extract_production_c() -> str:
    tex_src = TEXTURE_INC.read_text(encoding="utf-8")
    air_src = AIR_METAL_C.read_text(encoding="utf-8")

    # air_metal_texture.inc: buffer_import_handle through buffer_texture_bytes.
    tex_slice = extract_slice(
        tex_src,
        "struct annibale_memory_request {",
        "static uint64_t\nrows_hash(",
    )
    # air_metal.c: rwa_buffer_map and rwa_buffer_destroy.
    map_slice = extract_slice(air_src, "void *\nrwa_buffer_map(", "size_t\nrwa_buffer_size(")
    destroy_slice = extract_slice(
        air_src,
        "void\nrwa_buffer_destroy(",
        "/* Metal's own numbers,",
    )
    return "\n".join([tex_slice, map_slice, destroy_slice])


def compile_and_run(prod_c: str, expect_pass: bool, label: str) -> None:
    source = "\n".join([HARNESS_HEAD, prod_c, HARNESS_TESTS])
    with tempfile.TemporaryDirectory(prefix="air-memory-buffer-") as td:
        src = pathlib.Path(td) / "harness.c"
        exe = pathlib.Path(td) / "harness"
        src.write_text(source, encoding="utf-8")
        subprocess.run(
            [
                "clang",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                str(src),
                "-o",
                str(exe),
            ],
            check=True,
        )
        res = subprocess.run([str(exe)], capture_output=True, text=True)
        if expect_pass:
            if res.returncode != 0:
                raise AssertionError(
                    f"{label} failed unexpectedly (rc={res.returncode}):\n"
                    f"{res.stdout}\n{res.stderr}"
                )
        else:
            if res.returncode == 0:
                raise AssertionError(f"mutation '{label}' unexpectedly PASSED")


def run_mutations(base_c: str) -> None:
    mutations = [
        (
            "skip_host_bytes_bypass",
            base_c.replace("if (host_bytes) {", "if (0 && host_bytes) {", 1),
        ),
        (
            "free_host_map_on_destroy",
            base_c.replace(
                "   free(buffer);\n}",
                "   free(buffer->map);\n   free(buffer);\n}",
                1,
            ),
        ),
        (
            "drop_gem_close_on_fail",
            base_c.replace(
                "buffer_import_handle(device, &whandle, size, bytes, true, what, why, why_size)",
                "buffer_import_handle(device, &whandle, size, bytes, false, what, why, why_size)",
                1,
            ),
        ),
        (
            "skip_surface_pipe_map",
            base_c.replace(
                "buffer->map = pipe_buffer_map(device->pipe, buffer->res,",
                "buffer->map = (void *)0x1; (void)pipe_buffer_map; if (0) buffer->map = pipe_buffer_map(device->pipe, buffer->res,",
                1,
            ),
        ),
    ]
    for name, mutated in mutations:
        assert mutated != base_c, f"mutation '{name}' did not match target text"
        compile_and_run(mutated, expect_pass=False, label=name)
    print(f"test-air-memory-buffer: {len(mutations)} mutations caught")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()

    assert TEXTURE_INC.is_file() and AIR_METAL_C.is_file()
    prod_c = extract_production_c()
    compile_and_run(prod_c, expect_pass=True, label="baseline")
    print("test-air-memory-buffer: PASS")
    if args.mutations:
        run_mutations(prod_c)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
