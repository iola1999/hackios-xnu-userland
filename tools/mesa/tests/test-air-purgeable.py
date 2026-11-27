#!/usr/bin/env python3
"""Host ASan+UBSan unit test for rwa_texture purgeable state machine and BO reclamation.

Compiles production code from tools/mesa/air/air_metal_purgeable.inc directly with
lightweight Gallium stubs so any state-machine, ownership-guard, or BO-lifecycle bug
is caught immediately without mocking the logic under test.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1] / "air"
PURGE_H = ROOT / "air_metal_purgeable.h"
PURGE_INC = ROOT / "air_metal_purgeable.inc"

HARNESS = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_metal_purgeable.h"

#define RWA_LOCKED ((void)0)

struct pipe_resource {
   unsigned width0;
   unsigned height0;
   unsigned format;
   unsigned refs;
   unsigned id;
};

struct pipe_sampler_view {
   unsigned refs;
};

struct pipe_context;
struct pipe_screen;

struct pipe_context {
   unsigned flush_count;
   void (*flush)(struct pipe_context *pipe, void **fence, unsigned flags);
};

struct pipe_screen {
   bool fail_alloc;
   unsigned alloc_count;
   unsigned next_id;
   struct pipe_resource *(*resource_create)(struct pipe_screen *screen,
                                            const struct pipe_resource *templ);
};

struct rwa_device {
   struct pipe_screen *screen;
   struct pipe_context *pipe;
   unsigned argbuf_scrub_count;
   unsigned state_gone_count;
   unsigned zeroed_count;
};

struct rwa_buffer_texture {
   int dummy;
};

struct rwa_texture {
   unsigned references;
   struct rwa_texture *storage_owner;
   struct rwa_texture *chroma;
   bool rgb2p_pass_open;
   bool imported;
   struct rwa_device *device;
   struct pipe_resource *res;
   struct pipe_resource *rgb2p_sample;
   struct pipe_sampler_view *view;
   struct rwa_buffer_texture *backing;
   struct pipe_resource res_templ;
   unsigned purgeable_state;
   bool has_res_templ;
};

static unsigned g_res_freed = 0;
static unsigned g_view_freed = 0;

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

static void
argbuf_scrub(struct rwa_device *dev, void *buf, struct rwa_texture *tex, void *smp)
{
   (void)buf;
   (void)tex;
   (void)smp;
   dev->argbuf_scrub_count++;
}

static void
state_texture_gone(struct rwa_device *dev, const struct pipe_resource *res)
{
   assert(res != NULL);
   dev->state_gone_count++;
}

static void
pipe_sampler_view_release_ptr(struct pipe_sampler_view **view)
{
   if (*view) {
      assert((*view)->refs > 0);
      if (--(*view)->refs == 0) {
         free(*view);
         g_view_freed++;
      }
      *view = NULL;
   }
}

static void
pipe_resource_reference(struct pipe_resource **dst, struct pipe_resource *src)
{
   if (*dst) {
      assert((*dst)->refs > 0);
      if (--(*dst)->refs == 0) {
         free(*dst);
         g_res_freed++;
      }
   }
   *dst = src;
   if (src)
      src->refs++;
}

static struct rwa_texture *
texture_start_zeroed(struct rwa_device *dev, struct rwa_texture *t)
{
   assert(t->res != NULL);
   dev->zeroed_count++;
   return t;
}

static void
stub_flush(struct pipe_context *pipe, void **fence, unsigned flags)
{
   (void)fence;
   (void)flags;
   pipe->flush_count++;
}

static struct pipe_resource *
stub_resource_create(struct pipe_screen *screen, const struct pipe_resource *templ)
{
   if (screen->fail_alloc)
      return NULL;
   struct pipe_resource *r = calloc(1, sizeof(*r));
   assert(r != NULL);
   *r = *templ;
   r->refs = 1;
   r->id = ++screen->next_id;
   screen->alloc_count++;
   return r;
}

#include "air_metal_purgeable.inc"

static struct rwa_texture
make_private_tex(struct rwa_device *dev)
{
   struct pipe_resource templ = { .width0 = 64, .height0 = 64, .format = 80 };
   struct rwa_texture t = { 0 };
   t.references = 1;
   t.device = dev;
   t.res_templ = templ;
   t.has_res_templ = true;
   t.purgeable_state = RWA_PURGEABLE_NON_VOLATILE;
   t.res = dev->screen->resource_create(dev->screen, &templ);
   t.view = calloc(1, sizeof(*t.view));
   t.view->refs = 1;
   return t;
}

static void
test_state_machine_and_rebuild(void)
{
   struct pipe_screen screen = { .resource_create = stub_resource_create };
   struct pipe_context pipe = { .flush = stub_flush };
   struct rwa_device dev = { .screen = &screen, .pipe = &pipe };
   char why[256] = { 0 };

   struct rwa_texture t = make_private_tex(&dev);
   unsigned orig_id = t.res->id;
   assert(!rwa_texture_is_purgeable(&t));
   assert(rwa_texture_set_purgeable(&t, RWA_PURGEABLE_KEEP_CURRENT, why, sizeof(why)) ==
          RWA_PURGEABLE_NON_VOLATILE);
   /* Invalid states behave as KeepCurrent without mutating. */
   assert(rwa_texture_set_purgeable(&t, 0, why, sizeof(why)) == RWA_PURGEABLE_NON_VOLATILE);
   assert(rwa_texture_set_purgeable(&t, 99, why, sizeof(why)) == RWA_PURGEABLE_NON_VOLATILE);

   /* NonVolatile -> Volatile -> NonVolatile (no BO release or realloc). */
   assert(rwa_texture_set_purgeable(&t, RWA_PURGEABLE_VOLATILE, why, sizeof(why)) ==
          RWA_PURGEABLE_NON_VOLATILE);
   assert(rwa_texture_is_purgeable(&t));
   assert(t.res != NULL && t.res->id == orig_id);
   assert(rwa_texture_set_purgeable(&t, RWA_PURGEABLE_NON_VOLATILE, why, sizeof(why)) ==
          RWA_PURGEABLE_VOLATILE);
   assert(!rwa_texture_is_purgeable(&t));
   assert(screen.alloc_count == 1);

   /* NonVolatile -> Empty releases view + BO and flushes/scrubs state. */
   unsigned prev_res_freed = g_res_freed;
   unsigned prev_view_freed = g_view_freed;
   assert(rwa_texture_set_purgeable(&t, RWA_PURGEABLE_EMPTY, why, sizeof(why)) ==
          RWA_PURGEABLE_NON_VOLATILE);
   assert(t.res == NULL && t.view == NULL);
   assert(g_res_freed == prev_res_freed + 1);
   assert(g_view_freed == prev_view_freed + 1);
   assert(dev.argbuf_scrub_count == 1 && dev.state_gone_count == 1 && pipe.flush_count == 1);
   assert(rwa_texture_is_purgeable(&t));
   assert(!texture_require_backing(&t, "sample", why, sizeof(why)));
   assert(strstr(why, "MTLPurgeableStateEmpty") != NULL);

   /* Empty is idempotent; Empty -> Volatile keeps Empty without allocating. */
   assert(rwa_texture_set_purgeable(&t, RWA_PURGEABLE_EMPTY, why, sizeof(why)) ==
          RWA_PURGEABLE_EMPTY);
   assert(rwa_texture_set_purgeable(&t, RWA_PURGEABLE_VOLATILE, why, sizeof(why)) ==
          RWA_PURGEABLE_EMPTY);
   assert(t.res == NULL && screen.alloc_count == 1);

   /* OOM during Empty -> NonVolatile keeps state Empty. */
   screen.fail_alloc = true;
   why[0] = '\0';
   assert(rwa_texture_set_purgeable(&t, RWA_PURGEABLE_NON_VOLATILE, why, sizeof(why)) ==
          RWA_PURGEABLE_EMPTY);
   assert(t.res == NULL && rwa_texture_is_purgeable(&t) && why[0] != '\0');

   /* Empty -> NonVolatile rebuilds BO from saved template and zeroes it. */
   screen.fail_alloc = false;
   assert(rwa_texture_set_purgeable(&t, RWA_PURGEABLE_NON_VOLATILE, why, sizeof(why)) ==
          RWA_PURGEABLE_EMPTY);
   assert(t.res != NULL && t.res->id != orig_id);
   assert(t.res->width0 == 64 && t.res->height0 == 64 && t.res->format == 80);
   assert(dev.zeroed_count == 1 && !rwa_texture_is_purgeable(&t));
   assert(texture_require_backing(&t, "sample", why, sizeof(why)));

   pipe_resource_reference(&t.res, NULL);
}

static void
test_ownership_guards(void)
{
   struct pipe_screen screen = { .resource_create = stub_resource_create };
   struct pipe_context pipe = { .flush = stub_flush };
   struct rwa_device dev = { .screen = &screen, .pipe = &pipe };
   struct rwa_buffer_texture backing = { 0 };
   struct rwa_texture dummy_other = { 0 };
   char why[256] = { 0 };

   for (int kind = 0; kind < 7; kind++) {
      struct rwa_texture t = make_private_tex(&dev);
      switch (kind) {
      case 0: t.imported = true; break;
      case 1: t.backing = &backing; break;
      case 2: t.storage_owner = &dummy_other; break;
      case 3: t.chroma = &dummy_other; break;
      case 4: t.references = 2; break;
      case 5: t.rgb2p_pass_open = true; break;
      case 6: t.has_res_templ = false; break;
      }
      struct pipe_resource *orig = t.res;
      assert(rwa_texture_set_purgeable(&t, RWA_PURGEABLE_VOLATILE, why, sizeof(why)) ==
             RWA_PURGEABLE_NON_VOLATILE);
      assert(!rwa_texture_is_purgeable(&t));
      assert(rwa_texture_set_purgeable(&t, RWA_PURGEABLE_EMPTY, why, sizeof(why)) ==
             RWA_PURGEABLE_NON_VOLATILE);
      assert(t.res == orig && !rwa_texture_is_purgeable(&t));
      pipe_sampler_view_release_ptr(&t.view);
      pipe_resource_reference(&t.res, NULL);
   }
}

int
main(void)
{
   test_state_machine_and_rebuild();
   test_ownership_guards();
   printf("test-air-purgeable: PASS\n");
   return 0;
}
"""


def main() -> int:
    assert PURGE_H.is_file() and PURGE_INC.is_file()
    with tempfile.TemporaryDirectory(prefix="air-purgeable-") as td:
        src = pathlib.Path(td) / "harness.c"
        exe = pathlib.Path(td) / "harness"
        src.write_text(HARNESS)
        subprocess.run(
            [
                "clang",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                f"-I{ROOT}",
                str(src),
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
