#!/usr/bin/env python3
"""Host ASan+UBSan test for M13 same-command-buffer IOSurface copy-wrapper alias sync."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEX_INC = ROOT / "mesa/air/air_metal_texture.inc"


def cut_func(text: str, marker: str) -> str:
    start = text.index(marker)
    opening = text.index("{", start)
    tokens = re.finditer(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
        text[opening:], re.S,
    )
    depth = 0
    for tok in tokens:
        if tok.group() == "{":
            depth += 1
        elif tok.group() == "}":
            depth -= 1
            if depth == 0:
                return text[start:opening + tok.end()]
    raise ValueError(marker)


HARNESS = r"""
#include <assert.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define RWA_LOCKED
#define RWA_BTEX_LOCKED
#define PIPE_MAP_READ 1u

struct pipe_context { int dummy; };
struct pipe_transfer { int dummy; };
struct pipe_resource { unsigned width0, height0; };
struct rwa_buffer_texture;
struct rwa_device {
    struct pipe_context *pipe;
    struct rwa_buffer_texture *buffer_textures;
    unsigned written_buffer_textures, buffer_textures_count;
};
struct rwa_buffer {
    unsigned references;
    struct rwa_device *device;
    struct pipe_resource *res;
    void *map, *mirror;
    size_t size;
    uint64_t mirror_hash;
    bool mirror_hashed;
};
struct rwa_texture {
    unsigned references;
    struct rwa_device *device;
    struct pipe_resource *res;
    struct rwa_buffer_texture *backing;
    uint8_t *pixels;
    const char *format_name;
};
struct rwa_buffer_texture {
    struct rwa_buffer_texture *next;
    struct rwa_texture *texture;
    struct rwa_buffer *source;
    size_t offset;
    unsigned row_bytes;
    unsigned refs;
    uint64_t hash;
    bool unlinked, hashed, written;
};
struct rwa_btex_pin {
    struct rwa_buffer_texture *b;
    struct rwa_texture *tex;
    struct rwa_buffer *src;
};
static void rwa_texture_destroy(struct rwa_texture *t) {
    if (t) __atomic_sub_fetch(&t->references, 1u, __ATOMIC_ACQ_REL);
}
static void rwa_buffer_destroy(struct rwa_buffer *b) {
    if (b) __atomic_sub_fetch(&b->references, 1u, __ATOMIC_ACQ_REL);
}

static unsigned g_uploads, g_downloads;
static struct { unsigned refreshes, uploads, unchanged; } upload_count;
static void say(char *why, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(why, n, fmt, ap); va_end(ap);
}
static void *pipe_buffer_map_range(struct pipe_context *p, struct pipe_resource *r,
                                   size_t off, size_t len, unsigned fl, struct pipe_transfer **t) {
    (void)p; (void)r; (void)off; (void)len; (void)fl; *t = NULL; return NULL;
}
static void pipe_buffer_unmap(struct pipe_context *p, struct pipe_transfer *t) { (void)p; (void)t; }
static int rwa_texture_write(struct rwa_texture *t, unsigned a, unsigned b, const void *c, const void *d,
                             const void *src, unsigned row, char *why, size_t n) {
    (void)a; (void)b; (void)c; (void)d; (void)why; (void)n;
    g_uploads++;
    for (unsigned y = 0; y < t->res->height0; y++)
        memcpy(t->pixels + y * t->res->width0 * 4u, (const uint8_t *)src + y * row, t->res->width0 * 4u);
    return 0;
}
static int rwa_texture_read(struct rwa_texture *t, unsigned a, unsigned b, const void *c, const void *d,
                            void *dst, unsigned row, char *why, size_t n) {
    (void)a; (void)b; (void)c; (void)d; (void)why; (void)n;
    g_downloads++;
    for (unsigned y = 0; y < t->res->height0; y++)
        memcpy((uint8_t *)dst + y * row, t->pixels + y * t->res->width0 * 4u, t->res->width0 * 4u);
    return 0;
}
static inline void buffer_texture_clear_written(struct rwa_device *d, struct rwa_buffer_texture *b) {
    if (b && b->written) {
        b->written = false;
        __atomic_fetch_sub(&d->written_buffer_textures, 1u, __ATOMIC_RELEASE);
    }
}
static inline void mark_written(struct rwa_texture *t) {
    if (t->backing && !t->backing->written) {
        t->backing->written = true;
        __atomic_fetch_add(&t->device->written_buffer_textures, 1u, __ATOMIC_RELEASE);
    }
}

__AIR_FUNCS__

static struct rwa_buffer_texture *make_wrapper(struct rwa_device *dev, struct rwa_buffer *buf,
                                               void *mirror, size_t buf_sz, size_t off, unsigned row,
                                               unsigned w, unsigned h, struct rwa_texture *tex,
                                               struct pipe_resource *res) {
    *buf = (struct rwa_buffer){ .references = 1, .device = dev, .map = calloc(1, buf_sz), .size = buf_sz };
    if (mirror) {
        memcpy(buf->map, mirror, buf_sz);
        rwa_buffer_set_mirror(buf, mirror);
    }
    *res = (struct pipe_resource){ .width0 = w, .height0 = h };
    *tex = (struct rwa_texture){ .references = 1, .device = dev, .res = res, .pixels = calloc(w * h, 4u), .format_name = "BGRA8" };
    struct rwa_buffer_texture *bt = calloc(1, sizeof(*bt));
    *bt = (struct rwa_buffer_texture){
        .next = dev->buffer_textures, .texture = tex, .source = buf, .offset = off, .row_bytes = row, .refs = 1,
    };
    dev->buffer_textures = bt;
    tex->backing = bt;
    dev->buffer_textures_count++;
    char why[128] = {0};
    assert(buffer_texture_upload(bt, why, sizeof(why)) == 0);
    return bt;
}

int main(void) {
    struct rwa_device dev = { 0 };
    uint8_t surface_a[256], surface_b[256];
    memset(surface_a, 0x11, sizeof(surface_a));
    memset(surface_b, 0x22, sizeof(surface_b));

    struct rwa_buffer b1, b2, b3;
    struct rwa_texture t1, t2, t3;
    struct pipe_resource r1, r2, r3;
    struct rwa_buffer_texture *bt1 = make_wrapper(&dev, &b1, surface_a, 256, 0, 32, 4, 4, &t1, &r1);
    struct rwa_buffer_texture *bt2 = make_wrapper(&dev, &b2, surface_a, 256, 0, 32, 4, 4, &t2, &r2);
    struct rwa_buffer_texture *bt3 = make_wrapper(&dev, &b3, surface_b, 256, 0, 32, 4, 4, &t3, &r3);
    char why[128] = {0};

    /* 1. Read-only fast path: zero downloads/uploads when nothing is written. */
    g_uploads = g_downloads = 0;
    assert(buffer_texture_sync_aliases(&t2, why, sizeof(why)) == 0);
    assert(g_uploads == 0 && g_downloads == 0);

    /* 2. Same-CB write-then-read across two copy wrappers sharing surface_a mirror. */
    memset(t1.pixels, 0x7a, 64);
    mark_written(&t1);
    g_uploads = g_downloads = 0;
    /* Unrelated surface_b wrapper must NOT trigger sync. */
    assert(buffer_texture_sync_aliases(&t3, why, sizeof(why)) == 0);
    assert(g_uploads == 0 && g_downloads == 0 && bt1->written);
    /* Aliased surface_a wrapper t2 MUST sync t1 -> mirror -> t2. */
    assert(buffer_texture_sync_aliases(&t2, why, sizeof(why)) == 0);
    assert(g_downloads == 1u && g_uploads == 1u && !bt1->written && dev.written_buffer_textures == 0);
    assert(t2.pixels[0] == 0x7a && t2.pixels[63] == 0x7a && surface_a[0] == 0x7a);

    /* 3. rwa_buffer_textures_refresh list-order independence (t2 precedes t1 in list). */
    memset(t1.pixels, 0x9c, 64);
    mark_written(&t1);
    rwa_buffer_textures_refresh(&dev);
    assert(!bt1->written && t2.pixels[0] == 0x9c && t2.pixels[63] == 0x9c && surface_a[0] == 0x9c);

    /* 4. Same rwa_buffer (no mirror) overlapping slices sync; non-overlapping do not. */
    struct rwa_buffer shared_buf = { .references = 1, .device = &dev, .map = calloc(1, 512), .size = 512 };
    struct rwa_texture s1 = { .references = 1, .device = &dev, .res = &r1, .pixels = calloc(64, 1), .format_name = "BGRA8" };
    struct rwa_texture s2 = { .references = 1, .device = &dev, .res = &r2, .pixels = calloc(64, 1), .format_name = "BGRA8" };
    struct rwa_texture s3 = { .references = 1, .device = &dev, .res = &r3, .pixels = calloc(64, 1), .format_name = "BGRA8" };
    struct rwa_buffer_texture bs1 = { .next = dev.buffer_textures, .texture = &s1, .source = &shared_buf, .offset = 0, .row_bytes = 32, .refs = 1 };
    struct rwa_buffer_texture bs2 = { .next = &bs1, .texture = &s2, .source = &shared_buf, .offset = 64, .row_bytes = 32, .refs = 1 };
    struct rwa_buffer_texture bs3 = { .next = &bs2, .texture = &s3, .source = &shared_buf, .offset = 256, .row_bytes = 32, .refs = 1 };
    dev.buffer_textures = &bs3; s1.backing = &bs1; s2.backing = &bs2; s3.backing = &bs3;
    memset(s1.pixels, 0x55, 64); mark_written(&s1);
    g_uploads = g_downloads = 0;
    assert(buffer_texture_sync_aliases(&s3, why, sizeof(why)) == 0 && g_downloads == 0 && bs1.written);
    assert(buffer_texture_sync_aliases(&s2, why, sizeof(why)) == 0 && g_downloads == 1u && g_uploads == 1u && !bs1.written);
    assert(s2.pixels[0] == 0x55);

    free(b1.map); free(b2.map); free(b3.map); free(shared_buf.map);
    free(t1.pixels); free(t2.pixels); free(t3.pixels);
    free(s1.pixels); free(s2.pixels); free(s3.pixels);
    free(bt1); free(bt2); free(bt3);
    puts("PASS: M13 IOSurface copy-wrapper same-CB alias sync & refresh order (ASan+UBSan)");
    return 0;
}
"""


def main() -> None:
    text = TEX_INC.read_text()
    funcs = "\n".join(cut_func(text, m) for m in (
        "static bool\nmirror_read(",
        "static void\nbuffer_mirror_pull(",
        "void\nrwa_buffer_set_mirror(",
        "static char *\nbuffer_texture_bytes(",
        "static uint64_t\nrows_hash(",
        "static int\nbuffer_texture_upload(",
        "static int\nbuffer_texture_download(",
        "static size_t\nbuffer_texture_span(",
        "static bool\nbuffer_textures_alias(",
        "static bool\nrwa_try_retain_u(",
        "static unsigned\nbuffer_textures_collect(",
        "static void\nbuffer_textures_release(",
        "static int\nbuffer_texture_sync_aliases(",
        "void\nrwa_buffer_textures_refresh(",
        "void\nrwa_buffer_textures_writeback(",
    ))
    code = HARNESS.replace("__AIR_FUNCS__", funcs)
    with tempfile.TemporaryDirectory(prefix="air-surface-alias-") as d:
        src = Path(d) / "test.c"
        exe = Path(d) / "test"
        src.write_text(code)
        subprocess.run([
            "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fsanitize=address,undefined", str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
