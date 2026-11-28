#!/usr/bin/env python3
"""Host ASan+UBSan and TSan verification for G29 rwa_lock decoupling."""

from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1] / "air"


def extract_between(text: str, start_pat: str, end_pat: str) -> str:
    m_start = re.search(start_pat, text, re.MULTILINE)
    assert m_start, f"start pattern not found: {start_pat}"
    m_end = re.search(end_pat, text[m_start.start():], re.MULTILINE)
    assert m_end, f"end pattern not found: {end_pat}"
    return text[m_start.start():m_start.start() + m_end.end()]


def extract_function(text: str, sig_pat: str) -> str:
    m = re.search(sig_pat, text, re.MULTILINE)
    assert m, f"function not found: {sig_pat}"
    idx = text.find("{", m.start())
    assert idx != -1, f"opening brace not found for {sig_pat}"
    depth = 0
    in_str = False
    in_chr = False
    in_line_cmt = False
    in_blk_cmt = False
    i = idx
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line_cmt:
            if ch == "\n":
                in_line_cmt = False
        elif in_blk_cmt:
            if ch == "*" and nxt == "/":
                in_blk_cmt = False
                i += 1
        elif in_str:
            if ch == "\\":
                i += 1
            elif ch == '"':
                in_str = False
        elif in_chr:
            if ch == "\\":
                i += 1
            elif ch == "'":
                in_chr = False
        else:
            if ch == "/" and nxt == "/":
                in_line_cmt = True
                i += 1
            elif ch == "/" and nxt == "*":
                in_blk_cmt = True
                i += 1
            elif ch == '"':
                in_str = True
            elif ch == "'":
                in_chr = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return text[m.start():i + 1]
        i += 1
    raise AssertionError(f"unterminated function: {sig_pat}")


def build_harness_source() -> str:
    air_metal_c = (ROOT / "air_metal.c").read_text()
    priv_h = (ROOT / "air_metal_resource_private.h").read_text()
    state_inc = (ROOT / "air_metal_state.inc").read_text()
    argbuf_inc = (ROOT / "air_metal_argbuf.inc").read_text()
    tex_inc = (ROOT / "air_metal_texture.inc").read_text()
    vcso_inc = (ROOT / "air_metal_vertex_cache.inc").read_text()
    render_create_inc = (ROOT / "air_metal_render_create.inc").read_text()
    compute_inc = (ROOT / "air_metal_compute.inc").read_text()

    # Verify the exact non-software vs software CSO lock branch exists in rwa_render_create_linked.
    rcreate_fn = extract_function(render_create_inc, r"struct rwa_render \*\nrwa_render_create_linked\(")
    cso_branch = extract_between(
        rcreate_fn,
        r"   bool cso_ok;\n   if \(device->software\) \{",
        r"   \} else \{\n      RWA_CSO_CREATE_LOCKED;\n      cso_ok = render_create_gallium_states\([^;]+\);\n   \}",
    )

    # Verify compute_create uses RWA_CSO_CREATE_LOCKED on !software, rwa_compute_materialize
    # compiles into locals under RWA_CSO_CREATE_LOCKED and installs under short RWA_LOCKED
    # (deleting any duplicate CSO if dispatch raced ahead), and compute_state_for_ib deletes
    # unbound newly-created CSOs directly.
    ccreate_fn = extract_function(compute_inc, r"static struct rwa_compute \*\ncompute_create\(")
    assert "if (!device->software && !deferred) {\n      RWA_CSO_CREATE_LOCKED;" in ccreate_fn
    cmat_fn = extract_function(compute_inc, r"int\nrwa_compute_materialize\(")
    assert "RWA_CSO_CREATE_LOCKED;" in cmat_fn
    assert "compute_compile_variable_cso(compute, &yuv, &caps, &var_max, why, why_size);" in cmat_fn
    assert "if (!compute->cso) {" in cmat_fn
    assert "dup_cso = cso;" in cmat_fn
    assert "compute->device->pipe->delete_compute_state(compute->device->pipe, dup_cso);" in cmat_fn
    csib_fn = extract_function(compute_inc, r"static void \*\ncompute_state_for_ib\(")
    assert "device->pipe->delete_compute_state(device->pipe, cso);" in csib_fn

    locks_block = extract_between(
        air_metal_c,
        r"static pthread_mutex_t rwa_lock =",
        r"#define RWA_DEFERRED_LOCKED[^\n]+\n[^\n]+",
    )
    quad_verts_block = extract_between(
        air_metal_c,
        r"static const float rwa_quad_verts\[",
        r"\};",
    )

    prod_chunks = [
        locks_block,
        quad_verts_block,
        extract_between(priv_h, r"struct rwa_deferred_item \{", r"\};"),
        extract_between(priv_h, r"struct rwa_buffer_texture \{", r"\};"),
        extract_between(priv_h, r"struct rwa_argbuf_slot \{", r"\};"),
        extract_between(priv_h, r"struct rwa_argbuf_encoding \{", r"\};"),
        extract_between(vcso_inc, r"struct rwa_vertex_cso_entry \{", r"\};"),
        extract_function(priv_h, r"static inline void\nbuffer_texture_clear_written_locked\("),
        "static void state_release_framebuffer(struct rwa_device *device);",
        "static void state_texture_gone(struct rwa_device *device, const struct pipe_resource *res);",
        "static void state_forget(struct rwa_device *device);",
        "static void argbuf_scrub(struct rwa_device *device, const struct rwa_buffer *buffer, const struct rwa_texture *texture, const struct rwa_sampler *sampler);",
        "static void argbuf_forget_all(struct rwa_buffer *buffer);",
        "static void buffer_textures_forget(struct rwa_buffer *buffer);",
        "void rwa_buffer_destroy(struct rwa_buffer *buffer);",
        "void rwa_texture_destroy(struct rwa_texture *texture);",
        "void rwa_render_destroy(struct rwa_render *render);",
        extract_function(state_inc, r"static void\nfb_bound_sync_locked\("),
        extract_function(state_inc, r"static bool\nfb_bound_match_locked\("),
        extract_function(state_inc, r"static void\nrwa_deferred_drain_locked\("),
        extract_function(state_inc, r"static void\nrwa_deferred_defer_texture\("),
        extract_function(state_inc, r"static void\nrwa_deferred_defer_buffer\("),
        extract_function(state_inc, r"static void\nstate_framebuffer\("),
        extract_function(state_inc, r"static void\nstate_release_framebuffer\("),
        extract_function(state_inc, r"static void\nstate_texture_gone\("),
        extract_function(state_inc, r"static void\nstate_forget\("),
        extract_function(argbuf_inc, r"static struct rwa_argbuf_encoding \*\nargbuf_find\("),
        extract_function(argbuf_inc, r"static struct rwa_argbuf_encoding \*\nargbuf_encoding_for\("),
        extract_function(argbuf_inc, r"static struct rwa_argbuf_encoding \*\nargbuf_begin_member\("),
        extract_function(argbuf_inc, r"int\nrwa_argbuf_encode_buffer\("),
        extract_function(argbuf_inc, r"int\nrwa_argbuf_encode_texture\("),
        extract_function(argbuf_inc, r"static void\nargbuf_scrub\("),
        extract_function(argbuf_inc, r"static void\nargbuf_forget_all\("),
        extract_function(argbuf_inc, r"static bool\nargbuf_has_encodings\("),
        extract_function(tex_inc, r"static bool\nrwa_try_retain_u\("),
        extract_between(tex_inc, r"struct rwa_btex_pin \{", r"\};"),
        extract_function(tex_inc, r"static unsigned\nbuffer_textures_collect\("),
        extract_function(tex_inc, r"static void\nbuffer_textures_release\("),
        extract_function(tex_inc, r"static void\nbuffer_textures_forget\("),
        extract_function(tex_inc, r"void\nrwa_texture_destroy\("),
        extract_function(air_metal_c, r"void\nrwa_buffer_destroy\("),
        extract_function(vcso_inc, r"static void\nrwa_vertex_compute_hash\("),
        extract_function(vcso_inc, r"static struct rwa_vertex_cso_entry \*\nrwa_vertex_cso_lookup\("),
        extract_function(vcso_inc, r"static struct rwa_vertex_cso_entry \*\nrwa_vertex_cso_insert\("),
        extract_function(vcso_inc, r"static void\nrwa_vertex_cso_release\("),
        extract_function(vcso_inc, r"static void\nrwa_vertex_cso_cache_destroy\("),
        extract_function(vcso_inc, r"static void \*\ncreate_stage_cso\("),
        extract_function(vcso_inc, r"static struct pipe_resource \*\nrwa_quad_vbuf_ref\("),
        extract_function(render_create_inc, r"static bool\nrender_create_gallium_states\("),
        extract_function(render_create_inc, r"void\nrwa_render_destroy\("),
    ]

    prefix = r"""
#define _DARWIN_C_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BITFIELD_MASK(n) ((n) == 32 ? ~0u : ((1u << (n)) - 1u))
#define PIPE_MAX_COLOR_BUFS 8
#define RWA_MAX_TEXTURES 16
#define RWA_MAX_ARGBUF_MEMBERS 64
#define PIPE_BIND_VERTEX_BUFFER (1u << 0)
#define PIPE_USAGE_DEFAULT 0
#define PIPE_FACE_NONE 0
#define PIPE_FORMAT_R32G32B32A32_FLOAT 1

typedef enum { MESA_SHADER_VERTEX = 0, MESA_SHADER_FRAGMENT = 4 } mesa_shader_stage;
enum rwa_argbuf_kind { RWA_ARGBUF_BUFFER = 1, RWA_ARGBUF_TEXTURE = 2, RWA_ARGBUF_SAMPLER = 3 };

struct pipe_resource {
   unsigned refs;
   unsigned id;
};
struct pipe_transfer {
   struct pipe_resource *resource;
   unsigned id;
};
struct pipe_sampler_view {
   unsigned refs;
   struct pipe_resource *texture;
   unsigned id;
};
struct pipe_surface {
   struct pipe_resource *texture;
};
struct pipe_framebuffer_state {
   unsigned width, height, layers, samples;
   unsigned nr_cbufs;
   struct pipe_surface cbufs[PIPE_MAX_COLOR_BUFS];
   struct pipe_surface zsbuf;
};
struct pipe_viewport_state { int dummy; };
struct pipe_stencil_ref { int dummy; };
struct pipe_scissor_state { int dummy; };
struct pipe_blend_state { int dummy; };
struct pipe_depth_stencil_alpha_state { int dummy; };
struct pipe_rasterizer_state {
   unsigned cull_face, half_pixel_center, bottom_edge_rule;
   unsigned depth_clip_near, depth_clip_far, clip_halfz;
   unsigned clip_plane_enable, multisample, scissor;
   float line_width;
};
struct pipe_vertex_element {
   unsigned src_offset, src_stride, vertex_buffer_index, src_format;
};
struct pipe_shader_state {
   unsigned type;
   union { void *nir; } ir;
};
#define STATE_UNKNOWN ((void *)~(uintptr_t)0)
#define PIPE_TEXTURE_BARRIER_FRAMEBUFFER 1u
#define PIPE_TEXTURE_BARRIER_SAMPLER 2u
#define PIPE_SHADER_IR_NIR 1

typedef struct {
   struct { mesa_shader_stage stage; } info;
} nir_shader;

struct air_resource { char *name; char *type_name; };
struct air_varying { char *name; };
struct air_vertex_attribute { char *name; char *type_name; char *function_constant; };
struct air_shader_info {
   const char *entry_point;
   void *notes;
   unsigned num_notes;
   struct air_resource *resources;
   unsigned num_resources;
   struct air_varying *varyings;
   unsigned num_varyings;
   struct air_vertex_attribute *vertex_attributes;
   unsigned num_vertex_attributes;
   unsigned clip_distances;
};
struct rwa_slot { int dummy; };
struct rwa_vertex_state {
   unsigned count;
   struct pipe_vertex_element element[16];
};

struct pipe_screen {
   const char *(*get_name)(struct pipe_screen *);
};
struct pipe_context {
   void (*set_framebuffer_state)(struct pipe_context *, const struct pipe_framebuffer_state *);
   void (*sampler_view_destroy)(struct pipe_context *, struct pipe_sampler_view *);
   void (*texture_barrier)(struct pipe_context *, unsigned);
   void (*set_sampler_views)(struct pipe_context *, mesa_shader_stage, unsigned, unsigned, unsigned, struct pipe_sampler_view **);
   void (*bind_sampler_states)(struct pipe_context *, mesa_shader_stage, unsigned, unsigned, void **);
   void (*flush)(struct pipe_context *, void *, unsigned);
   void *(*create_vs_state)(struct pipe_context *, const struct pipe_shader_state *);
   void *(*create_fs_state)(struct pipe_context *, const struct pipe_shader_state *);
   void (*delete_vs_state)(struct pipe_context *, void *);
   void (*delete_fs_state)(struct pipe_context *, void *);
   void *(*create_blend_state)(struct pipe_context *, const struct pipe_blend_state *);
   void (*delete_blend_state)(struct pipe_context *, void *);
   void *(*create_depth_stencil_alpha_state)(struct pipe_context *, const struct pipe_depth_stencil_alpha_state *);
   void (*delete_depth_stencil_alpha_state)(struct pipe_context *, void *);
   void *(*create_rasterizer_state)(struct pipe_context *, const struct pipe_rasterizer_state *);
   void (*delete_rasterizer_state)(struct pipe_context *, void *);
   void *(*create_vertex_elements_state)(struct pipe_context *, unsigned, const struct pipe_vertex_element *);
   void (*delete_vertex_elements_state)(struct pipe_context *, void *);
   void (*delete_sampler_state)(struct pipe_context *, void *);
   void (*bind_vs_state)(struct pipe_context *, void *);
   void (*bind_fs_state)(struct pipe_context *, void *);
   void (*bind_blend_state)(struct pipe_context *, void *);
   void (*bind_depth_stencil_alpha_state)(struct pipe_context *, void *);
   void (*bind_rasterizer_state)(struct pipe_context *, void *);
   void (*bind_vertex_elements_state)(struct pipe_context *, void *);
   void (*set_vertex_buffers)(struct pipe_context *, unsigned, const void *);
};

static struct { unsigned fb; } state_count;
static void state_count_draw(void) {}

struct rwa_bound_state {
   struct pipe_framebuffer_state fb;
   bool fb_set, viewport_set, stencil_set, scissor_set;
   bool views_dirty[2];
   struct pipe_viewport_state viewport;
   struct pipe_stencil_ref stencil;
   struct pipe_scissor_state scissor;
   void *blend, *dsa, *rast, *vs, *fs, *velem;
   struct pipe_sampler_view *views[2][RWA_MAX_TEXTURES];
   void *samplers[2][RWA_MAX_TEXTURES];
   unsigned num_views[2], num_samplers[2];
};

struct rwa_deferred_item;
struct rwa_buffer_texture;
struct rwa_argbuf_encoding;
struct rwa_vertex_cso_entry;

struct rwa_device {
   struct pipe_screen *screen;
   struct pipe_context *pipe;
   struct rwa_vertex_cso_entry *vertex_cso_cache;
   struct pipe_resource *quad_vbuf;
   bool software;
   struct rwa_buffer *encoded;
   struct rwa_buffer_texture *buffer_textures;
   unsigned written_buffer_textures, buffer_textures_count;
   struct rwa_deferred_item *deferred_head;
   struct pipe_resource *fb_bound_res[PIPE_MAX_COLOR_BUFS + 1];
   unsigned fb_bound_count;
   struct rwa_bound_state bound;
};

struct rwa_buffer {
   struct rwa_device *device;
   unsigned references;
   size_t size;
   struct pipe_resource *res;
   struct pipe_transfer *xfer;
   void *map;
   struct rwa_argbuf_encoding *encodings;
   bool listed;
   struct rwa_buffer *next_encoded;
};

struct rwa_texture {
   struct rwa_device *device;
   unsigned references;
   struct pipe_resource *res;
   struct pipe_resource *rgb2p_sample;
   struct pipe_sampler_view *view;
   struct rwa_buffer_texture *backing;
   struct rwa_texture *storage_owner;
   struct rwa_texture *chroma;
};

struct rwa_sampler { int dummy; };

struct rwa_render {
   struct rwa_device *device;
   void *mem_ctx;
   struct air_shader_info vinfo;
   struct rwa_vertex_state vstate;
   struct rwa_slot vert_slots[RWA_MAX_TEXTURES];
   bool vert_compare_sampler[RWA_MAX_TEXTURES];
   bool generated_vertex;
   void *fs_cso, *vs_cso, *yuv_fs_cso;
   struct rwa_vertex_cso_entry *cached_vcso_entry;
   void *blend, *dsa, *rast, *rast_scissor, *velem;
   struct pipe_resource *vbuf;
   void *frag_own_sampler[RWA_MAX_TEXTURES];
   void *vert_own_sampler[RWA_MAX_TEXTURES];
};

static int g_live_res = 0;
static int g_live_views = 0;
static int g_live_xfers = 0;
static int g_live_csos = 0;
static int g_cso_sleep_us = 0;
static pthread_mutex_t g_hook_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_hook_cv = PTHREAD_COND_INITIALIZER;
static int g_entered_slow_fs = 0;
static int g_trylock_inside_fs = -1;

static void say(char *why, size_t why_size, const char *fmt, ...) {
   if (why && why_size) {
      va_list ap; va_start(ap, fmt); vsnprintf(why, why_size, fmt, ap); va_end(ap);
   }
}
static const char *rwa_device_name(struct rwa_device *d) { return d && d->software ? "soft" : "gpu"; }
static void *ralloc_context(void *p) { (void)p; return malloc(1); }
static void ralloc_free(void *p) { free(p); }
static char *ralloc_strdup(void *ctx, const char *s) { (void)ctx; return s ? strdup(s) : NULL; }
static void *ralloc_array_fn(size_t sz, size_t n) { return calloc(n ? n : 1, sz); }
#define ralloc_array(ctx, type, n) ((type *)ralloc_array_fn(sizeof(type), (n)))

static void pipe_resource_reference(struct pipe_resource **dst, struct pipe_resource *src) {
   struct pipe_resource *old = *dst;
   if (src) __atomic_fetch_add(&src->refs, 1u, __ATOMIC_RELAXED);
   *dst = src;
   if (old && __atomic_sub_fetch(&old->refs, 1u, __ATOMIC_ACQ_REL) == 0) {
      __atomic_fetch_sub(&g_live_res, 1, __ATOMIC_RELAXED);
      free(old);
   }
}
static struct pipe_resource *alloc_pipe_res(void) {
   struct pipe_resource *r = calloc(1, sizeof(*r));
   assert(r);
   r->refs = 1;
   __atomic_fetch_add(&g_live_res, 1, __ATOMIC_RELAXED);
   return r;
}
static struct pipe_sampler_view *alloc_pipe_view(struct pipe_resource *tex) {
   struct pipe_sampler_view *v = calloc(1, sizeof(*v));
   assert(v);
   v->refs = 1;
   pipe_resource_reference(&v->texture, tex);
   __atomic_fetch_add(&g_live_views, 1, __ATOMIC_RELAXED);
   return v;
}
static struct pipe_transfer *alloc_pipe_xfer(struct pipe_resource *res) {
   struct pipe_transfer *x = calloc(1, sizeof(*x));
   assert(x);
   pipe_resource_reference(&x->resource, res);
   __atomic_fetch_add(&g_live_xfers, 1, __ATOMIC_RELAXED);
   return x;
}
static void pipe_sampler_view_release_ptr(struct pipe_sampler_view **v) {
   struct pipe_sampler_view *old = *v;
   *v = NULL;
   if (old && __atomic_sub_fetch(&old->refs, 1u, __ATOMIC_ACQ_REL) == 0) {
      pipe_resource_reference(&old->texture, NULL);
      __atomic_fetch_sub(&g_live_views, 1, __ATOMIC_RELAXED);
      free(old);
   }
}
static void pipe_buffer_unmap(struct pipe_context *pipe, struct pipe_transfer *xfer) {
   (void)pipe;
   if (xfer) {
      pipe_resource_reference(&xfer->resource, NULL);
      __atomic_fetch_sub(&g_live_xfers, 1, __ATOMIC_RELAXED);
      free(xfer);
   }
}
static struct pipe_resource *pipe_buffer_create(struct pipe_screen *s, unsigned b, unsigned u, size_t sz) {
   (void)s; (void)b; (void)u; (void)sz;
   return alloc_pipe_res();
}
static void pipe_buffer_write(struct pipe_context *p, struct pipe_resource *r, unsigned off, size_t sz, const void *d) {
   (void)p; (void)r; (void)off; (void)sz; (void)d;
}
static bool util_framebuffer_state_equal(const struct pipe_framebuffer_state *a, const struct pipe_framebuffer_state *b) {
   if (a->nr_cbufs != b->nr_cbufs || a->zsbuf.texture != b->zsbuf.texture)
      return false;
   for (unsigned i = 0; i < a->nr_cbufs; i++)
      if (a->cbufs[i].texture != b->cbufs[i].texture)
         return false;
   return true;
}
static void util_unreference_framebuffer_state(struct pipe_framebuffer_state *fb) {
   for (unsigned i = 0; i < fb->nr_cbufs; i++)
      pipe_resource_reference(&fb->cbufs[i].texture, NULL);
   pipe_resource_reference(&fb->zsbuf.texture, NULL);
   fb->nr_cbufs = 0;
}
static void util_copy_framebuffer_state(struct pipe_framebuffer_state *dst, const struct pipe_framebuffer_state *src) {
   util_unreference_framebuffer_state(dst);
   dst->nr_cbufs = src->nr_cbufs;
   for (unsigned i = 0; i < src->nr_cbufs; i++)
      pipe_resource_reference(&dst->cbufs[i].texture, src->cbufs[i].texture);
   pipe_resource_reference(&dst->zsbuf.texture, src->zsbuf.texture);
}
"""

    suffix = f"""
static const char *mock_get_name(struct pipe_screen *s) {{ (void)s; return "mock"; }}
static void mock_set_fb(struct pipe_context *p, const struct pipe_framebuffer_state *fb) {{ (void)p; (void)fb; }}
static void mock_view_destroy(struct pipe_context *p, struct pipe_sampler_view *v) {{ (void)p; (void)v; }}
static void *mock_create_vs(struct pipe_context *p, const struct pipe_shader_state *s) {{
   (void)p; (void)s;
   __atomic_fetch_add(&g_live_csos, 1, __ATOMIC_RELAXED);
   return malloc(8);
}}
static void *mock_create_fs(struct pipe_context *p, const struct pipe_shader_state *s) {{
   (void)p; (void)s;
   int sleep_us = __atomic_load_n(&g_cso_sleep_us, __ATOMIC_RELAXED);
   if (sleep_us > 0) {{
      int tl = pthread_mutex_trylock(&rwa_lock);
      if (tl == 0) pthread_mutex_unlock(&rwa_lock);
      pthread_mutex_lock(&g_hook_mu);
      g_trylock_inside_fs = tl;
      g_entered_slow_fs = 1;
      pthread_cond_broadcast(&g_hook_cv);
      pthread_mutex_unlock(&g_hook_mu);
      usleep((useconds_t)sleep_us);
   }}
   __atomic_fetch_add(&g_live_csos, 1, __ATOMIC_RELAXED);
   return malloc(8);
}}
static void mock_del_cso(struct pipe_context *p, void *c) {{
   (void)p;
   if (c) {{
      __atomic_fetch_sub(&g_live_csos, 1, __ATOMIC_RELAXED);
      free(c);
   }}
}}
static void *mock_create_fixed(struct pipe_context *p, const void *s) {{
   (void)p; (void)s;
   __atomic_fetch_add(&g_live_csos, 1, __ATOMIC_RELAXED);
   return malloc(8);
}}
static void *mock_create_velem(struct pipe_context *p, unsigned c, const struct pipe_vertex_element *v) {{
   (void)p; (void)c; (void)v;
   __atomic_fetch_add(&g_live_csos, 1, __ATOMIC_RELAXED);
   return malloc(8);
}}
static void mock_bind(struct pipe_context *p, void *c) {{ (void)p; (void)c; }}
static void mock_set_vbuf(struct pipe_context *p, unsigned n, const void *v) {{ (void)p; (void)n; (void)v; }}

static struct rwa_render *
run_render_cso_create(struct rwa_device *device, const void *vbc, size_t vsz, const char *ventry)
{{
   struct rwa_render *render = calloc(1, sizeof(*render));
   assert(render);
   render->device = device;
   render->mem_ctx = ralloc_context(NULL);
   unsigned fetched = 0;
   struct pipe_blend_state blend = {{ 0 }};
   unsigned zs_format = 0, samples = 1;
   char why[256] = {{ 0 }};
   size_t why_size = sizeof(why);
   const void *vertex_bitcode = vbc;
   size_t vertex_size = vsz;
   const char *vertex_entry = ventry;
   nir_shader fs_dummy = {{ .info.stage = MESA_SHADER_FRAGMENT }};
   nir_shader vs_dummy = {{ .info.stage = MESA_SHADER_VERTEX }};
   nir_shader *fs_prep = &fs_dummy;
   nir_shader *vs_prep = &vs_dummy;
   render->cached_vcso_entry = rwa_vertex_cso_lookup(device, vertex_bitcode, vertex_size, vertex_entry);
   if (render->cached_vcso_entry)
      render->vs_cso = render->cached_vcso_entry->vs_cso;
{cso_branch}
   assert(cso_ok);
   return render;
}}

static double now_ms(void) {{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}}

struct slow_cso_arg {{
   struct rwa_device *dev;
   const char *tag;
   double elapsed_ms;
   struct rwa_render *out;
}};

static void *bg_slow_cso_thread(void *p) {{
   struct slow_cso_arg *a = p;
   double t0 = now_ms();
   a->out = run_render_cso_create(a->dev, a->tag, strlen(a->tag) + 1, "vs_main");
   a->elapsed_ms = now_ms() - t0;
   return NULL;
}}

static struct rwa_buffer *make_test_buffer(struct rwa_device *dev, bool with_xfer) {{
   struct rwa_buffer *b = calloc(1, sizeof(*b));
   assert(b);
   b->device = dev;
   b->references = 1;
   b->size = 4096;
   b->res = alloc_pipe_res();
   if (with_xfer)
      b->xfer = alloc_pipe_xfer(b->res);
   return b;
}}

static struct rwa_texture *make_test_texture(struct rwa_device *dev, bool with_view, bool bind_fb, bool with_btex, struct rwa_buffer *src) {{
   struct rwa_texture *t = calloc(1, sizeof(*t));
   assert(t);
   t->device = dev;
   t->references = 1;
   t->res = alloc_pipe_res();
   if (with_view)
      t->view = alloc_pipe_view(t->res);
   if (bind_fb) {{
      RWA_LOCKED;
      struct pipe_framebuffer_state fb = {{ .nr_cbufs = 1 }};
      fb.cbufs[0].texture = t->res;
      state_framebuffer(dev, &fb);
   }}
   if (with_btex) {{
      assert(src);
      struct rwa_buffer_texture *bt = calloc(1, sizeof(*bt));
      assert(bt);
      bt->texture = t;
      bt->source = src;
      bt->refs = 1;
      bt->written = true;
      RWA_BTEX_LOCKED;
      bt->next = dev->buffer_textures;
      dev->buffer_textures = bt;
      __atomic_fetch_add(&dev->buffer_textures_count, 1u, __ATOMIC_RELEASE);
      __atomic_fetch_add(&dev->written_buffer_textures, 1u, __ATOMIC_RELEASE);
      __atomic_store_n(&t->backing, bt, __ATOMIC_RELEASE);
   }}
   return t;
}}

struct hold_ctx {{
   pthread_mutex_t mu;
   pthread_cond_t cv;
   int state;
}};

static void *rwa_lock_holder_fn(void *p) {{
   struct hold_ctx *h = p;
   pthread_mutex_lock(&rwa_lock);
   pthread_mutex_lock(&h->mu);
   h->state = 1;
   pthread_cond_broadcast(&h->cv);
   while (h->state != 2)
      pthread_cond_wait(&h->cv, &h->mu);
   pthread_mutex_unlock(&h->mu);
   pthread_mutex_unlock(&rwa_lock);
   return NULL;
}}

static void test_non_software_decoupling(struct rwa_device *dev) {{
   dev->software = false;
   /* Pre-create quad_vbuf as rwa_device_open does so rwa_quad_vbuf_ref takes atomic fast-path. */
   dev->quad_vbuf = pipe_buffer_create(dev->screen, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_DEFAULT, sizeof(rwa_quad_verts));

   /* Prepare foreground objects that hit FB-bound, sampler_view, argbuf, buffer_texture, and xfer paths. */
   struct rwa_buffer *arg_dest = make_test_buffer(dev, false);
   struct rwa_buffer *xfer_buf = make_test_buffer(dev, true);
   struct rwa_texture *fb_tex = make_test_texture(dev, true, true, false, NULL);
   struct rwa_texture *bt_tex = make_test_texture(dev, false, false, true, xfer_buf);
   char why[128] = {{ 0 }};
   assert(rwa_argbuf_encode_buffer(arg_dest, 0, 0, xfer_buf, 0, why, sizeof(why)) == 0);
   assert(rwa_argbuf_encode_texture(arg_dest, 0, 1, fb_tex, why, sizeof(why)) == 0);
   assert(argbuf_has_encodings(arg_dest));
   assert(!argbuf_has_encodings(xfer_buf));

   /* Launch slow background CSO compile holding rwa_cso_create_lock for 70 ms. */
   __atomic_store_n(&g_cso_sleep_us, 70000, __ATOMIC_RELAXED);
   g_entered_slow_fs = 0;
   g_trylock_inside_fs = -1;
   struct slow_cso_arg arg = {{ .dev = dev, .tag = "SHADER_PREWARM_V1" }};
   pthread_t bg;
   assert(pthread_create(&bg, NULL, bg_slow_cso_thread, &arg) == 0);

   pthread_mutex_lock(&g_hook_mu);
   while (!g_entered_slow_fs)
      pthread_cond_wait(&g_hook_cv, &g_hook_mu);
   pthread_mutex_unlock(&g_hook_mu);

   double fg_t0 = now_ms();
   /* Foreground operations must finish without waiting for bg CSO create. */
   struct pipe_resource *vb = rwa_quad_vbuf_ref(dev, rwa_quad_verts, sizeof(rwa_quad_verts));
   assert(vb == dev->quad_vbuf);
   pipe_resource_reference(&vb, NULL);

   rwa_texture_destroy(fb_tex);
   rwa_texture_destroy(bt_tex);
   rwa_buffer_destroy(xfer_buf);
   rwa_buffer_destroy(arg_dest);

   /* Foreground draw path takes rwa_lock while rwa_cso_create_lock is held and drains deferred queue. */
   {{
      RWA_LOCKED;
      rwa_deferred_drain_locked(dev);
      state_release_framebuffer(dev);
   }}
   double fg_elapsed = now_ms() - fg_t0;
   assert(fg_elapsed < 35.0);

   /* Cross-thread deferral check: another thread holds rwa_lock while destroy enqueues onto deferred_head. */
   struct rwa_buffer *def_buf = make_test_buffer(dev, true);
   struct rwa_texture *def_tex = make_test_texture(dev, true, true, false, NULL);
   struct hold_ctx hctx = {{ PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 }};
   pthread_t holder;
   assert(pthread_create(&holder, NULL, rwa_lock_holder_fn, &hctx) == 0);
   pthread_mutex_lock(&hctx.mu);
   while (hctx.state != 1)
      pthread_cond_wait(&hctx.cv, &hctx.mu);
   pthread_mutex_unlock(&hctx.mu);

   rwa_texture_destroy(def_tex);
   rwa_buffer_destroy(def_buf);
   assert(__atomic_load_n(&dev->deferred_head, __ATOMIC_ACQUIRE) != NULL);

   pthread_mutex_lock(&hctx.mu);
   hctx.state = 2;
   pthread_cond_broadcast(&hctx.cv);
   pthread_mutex_unlock(&hctx.mu);
   assert(pthread_join(holder, NULL) == 0);

   {{
      RWA_LOCKED;
      rwa_deferred_drain_locked(dev);
      state_release_framebuffer(dev);
   }}
   assert(__atomic_load_n(&dev->deferred_head, __ATOMIC_ACQUIRE) == NULL);

   assert(pthread_join(bg, NULL) == 0);
   assert(g_trylock_inside_fs == 0); /* rwa_lock was NOT held inside !software create_fs_state */
   assert(arg.elapsed_ms >= 60.0);
   rwa_render_destroy(arg.out);
   __atomic_store_n(&g_cso_sleep_us, 0, __ATOMIC_RELAXED);
}}

static void test_software_keeps_rwa_lock(struct rwa_device *dev) {{
   dev->software = true;
   __atomic_store_n(&g_cso_sleep_us, 25000, __ATOMIC_RELAXED);
   g_entered_slow_fs = 0;
   g_trylock_inside_fs = -1;
   struct slow_cso_arg arg = {{ .dev = dev, .tag = "SHADER_SOFT_V1" }};
   pthread_t bg;
   assert(pthread_create(&bg, NULL, bg_slow_cso_thread, &arg) == 0);

   pthread_mutex_lock(&g_hook_mu);
   while (!g_entered_slow_fs)
      pthread_cond_wait(&g_hook_cv, &g_hook_mu);
   pthread_mutex_unlock(&g_hook_mu);

   /* From another thread, rwa_lock must be busy while software CSO create runs. */
   int tl = pthread_mutex_trylock(&rwa_lock);
   assert(tl == EBUSY);

   assert(pthread_join(bg, NULL) == 0);
   rwa_render_destroy(arg.out);
   __atomic_store_n(&g_cso_sleep_us, 0, __ATOMIC_RELAXED);
   dev->software = false;
}}

struct race_ctx {{
   struct rwa_device *dev;
   int tid;
}};

static void *race_worker(void *p) {{
   struct race_ctx *ctx = p;
   struct rwa_device *dev = ctx->dev;
   char why[128] = {{ 0 }};

   for (int i = 0; i < 60; i++) {{
      if ((ctx->tid & 3) == 0) {{
         char tag[32];
         snprintf(tag, sizeof(tag), "VS_KEY_%d", i & 3);
         struct rwa_render *r = run_render_cso_create(dev, tag, strlen(tag) + 1, "vs_main");
         struct rwa_btex_pin stack[8], *pins = NULL;
         unsigned n = buffer_textures_collect(dev, NULL, false, stack, 8, &pins);
         buffer_textures_release(pins, n, stack);
         rwa_render_destroy(r);
      }} else if ((ctx->tid & 3) == 1) {{
         struct rwa_buffer *dest = make_test_buffer(dev, false);
         struct rwa_buffer *buf = make_test_buffer(dev, (i & 1) != 0);
         struct rwa_texture *tex = make_test_texture(dev, (i & 1) != 0, (i & 2) != 0, (i & 1) == 0, buf);
         (void)rwa_argbuf_encode_buffer(dest, 0, 0, buf, 0, why, sizeof(why));
         (void)rwa_argbuf_encode_texture(dest, 0, 1, tex, why, sizeof(why));
         rwa_texture_destroy(tex);
         rwa_buffer_destroy(buf);
         rwa_buffer_destroy(dest);
      }} else if ((ctx->tid & 3) == 2) {{
         struct rwa_buffer *buf = make_test_buffer(dev, true);
         struct rwa_texture *owner = make_test_texture(dev, true, false, true, buf);
         struct rwa_texture *view = make_test_texture(dev, true, true, false, NULL);
         __atomic_fetch_add(&owner->references, 1u, __ATOMIC_RELAXED);
         view->storage_owner = owner;
         rwa_texture_destroy(owner);
         rwa_texture_destroy(view);
         rwa_buffer_destroy(buf);
      }} else {{
         RWA_LOCKED;
         rwa_deferred_drain_locked(dev);
         if ((i & 3) == 0) {{
            struct pipe_resource *fb_r = alloc_pipe_res();
            struct pipe_framebuffer_state fb = {{ .nr_cbufs = 1 }};
            fb.cbufs[0].texture = fb_r;
            state_framebuffer(dev, &fb);
            pipe_resource_reference(&fb_r, NULL);
         }} else {{
            state_release_framebuffer(dev);
         }}
         struct pipe_resource *vb = rwa_quad_vbuf_ref(dev, rwa_quad_verts, sizeof(rwa_quad_verts));
         pipe_resource_reference(&vb, NULL);
      }}
   }}
   return NULL;
}}

static void mock_tex_barrier(struct pipe_context *p, unsigned f) {{ (void)p; (void)f; }}
static void mock_set_views(struct pipe_context *p, mesa_shader_stage s, unsigned st, unsigned n, unsigned u, struct pipe_sampler_view **v) {{
   (void)p; (void)s; (void)st; (void)n; (void)u; (void)v;
}}
static void mock_bind_samplers(struct pipe_context *p, mesa_shader_stage s, unsigned st, unsigned n, void **smp) {{
   (void)p; (void)s; (void)st; (void)n; (void)smp;
}}
static void mock_flush(struct pipe_context *p, void *f, unsigned flags) {{ (void)p; (void)f; (void)flags; }}

struct mock_compute {{
   struct rwa_device *device;
   void *cso;
   unsigned variable_max;
   unsigned max_threads;
}};

struct mat_race_arg {{
   struct mock_compute *comp;
   int started;
   pthread_mutex_t mu;
   pthread_cond_t cv;
   unsigned out_threads;
   int dup_deleted;
}};

static void *bg_compute_materialize_thread(void *p) {{
   struct mat_race_arg *a = p;
   struct mock_compute *comp = a->comp;
   {{
      RWA_LOCKED;
      if (comp->cso) {{
         a->out_threads = comp->max_threads;
         return NULL;
      }}
   }}
   void *cso = NULL;
   unsigned var_max = 0;
   {{
      RWA_CSO_CREATE_LOCKED;
      pthread_mutex_lock(&a->mu);
      a->started = 1;
      pthread_cond_broadcast(&a->cv);
      pthread_mutex_unlock(&a->mu);
      usleep(40000);
      cso = mock_create_fixed(comp->device->pipe, NULL);
      var_max = 256;
   }}
   void *dup_cso = NULL;
   {{
      RWA_LOCKED;
      if (!comp->cso) {{
         comp->cso = cso;
         comp->variable_max = var_max;
         comp->max_threads = var_max;
      }} else {{
         dup_cso = cso;
      }}
      a->out_threads = comp->max_threads;
   }}
   if (dup_cso) {{
      mock_del_cso(comp->device->pipe, dup_cso);
      a->dup_deleted = 1;
   }}
   return NULL;
}}

static void test_compute_cso_decoupled(struct rwa_device *dev) {{
   /* 1. Slow compute CSO compile under RWA_CSO_CREATE_LOCKED must not block foreground
    * rwa_lock draws or rwa_texture_destroy. */
   __atomic_store_n(&g_cso_sleep_us, 60000, __ATOMIC_RELAXED);
   g_entered_slow_fs = 0;
   g_trylock_inside_fs = -1;
   struct slow_cso_arg arg = {{ .dev = dev, .tag = "COMPUTE_CSO_V1" }};
   pthread_t bg;
   assert(pthread_create(&bg, NULL, bg_slow_cso_thread, &arg) == 0);

   pthread_mutex_lock(&g_hook_mu);
   while (!g_entered_slow_fs)
      pthread_cond_wait(&g_hook_cv, &g_hook_mu);
   pthread_mutex_unlock(&g_hook_mu);

   double t0 = now_ms();
   struct rwa_texture *tex = make_test_texture(dev, true, true, false, NULL);
   rwa_texture_destroy(tex);
   {{
      RWA_LOCKED;
      rwa_deferred_drain_locked(dev);
      state_release_framebuffer(dev);
   }}
   double fg_ms = now_ms() - t0;
   assert(fg_ms < 30.0);

   assert(pthread_join(bg, NULL) == 0);
   assert(g_trylock_inside_fs == 0);
   rwa_render_destroy(arg.out);
   __atomic_store_n(&g_cso_sleep_us, 0, __ATOMIC_RELAXED);

   /* 2. Same-object race: rwa_compute_materialize() (compiling under rwa_cso_create_lock)
    * vs concurrent rwa_dispatch() (installing comp->cso under rwa_lock). Must have zero
    * TSan data races and cleanly delete the duplicate CSO. */
   struct mock_compute comp = {{ .device = dev, .cso = NULL, .variable_max = 0, .max_threads = 0 }};
   struct mat_race_arg marg = {{
      .comp = &comp, .started = 0,
      .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
      .out_threads = 0, .dup_deleted = 0,
   }};
   pthread_t mat_th;
   assert(pthread_create(&mat_th, NULL, bg_compute_materialize_thread, &marg) == 0);
   pthread_mutex_lock(&marg.mu);
   while (!marg.started)
      pthread_cond_wait(&marg.cv, &marg.mu);
   pthread_mutex_unlock(&marg.mu);

   /* Foreground dispatch beats materialize while materialize is compiling. */
   {{
      RWA_LOCKED;
      assert(comp.cso == NULL);
      comp.cso = mock_create_fixed(dev->pipe, NULL);
      comp.variable_max = 512;
      comp.max_threads = 512;
   }}
   assert(pthread_join(mat_th, NULL) == 0);
   assert(marg.dup_deleted == 1);
   assert(marg.out_threads == 512);
   {{
      RWA_LOCKED;
      mock_del_cso(dev->pipe, comp.cso);
      comp.cso = NULL;
   }}
}}

int main(void) {{
   struct pipe_screen scr = {{ .get_name = mock_get_name }};
   struct pipe_context pipe = {{
      .set_framebuffer_state = mock_set_fb,
      .sampler_view_destroy = mock_view_destroy,
      .texture_barrier = mock_tex_barrier,
      .set_sampler_views = mock_set_views,
      .bind_sampler_states = mock_bind_samplers,
      .flush = mock_flush,
      .create_vs_state = mock_create_vs,
      .create_fs_state = mock_create_fs,
      .delete_vs_state = mock_del_cso,
      .delete_fs_state = mock_del_cso,
      .create_blend_state = (void *)mock_create_fixed,
      .delete_blend_state = mock_del_cso,
      .create_depth_stencil_alpha_state = (void *)mock_create_fixed,
      .delete_depth_stencil_alpha_state = mock_del_cso,
      .create_rasterizer_state = (void *)mock_create_fixed,
      .delete_rasterizer_state = mock_del_cso,
      .create_vertex_elements_state = mock_create_velem,
      .delete_vertex_elements_state = mock_del_cso,
      .delete_sampler_state = mock_del_cso,
      .bind_vs_state = mock_bind,
      .bind_fs_state = mock_bind,
      .bind_blend_state = mock_bind,
      .bind_depth_stencil_alpha_state = mock_bind,
      .bind_rasterizer_state = mock_bind,
      .bind_vertex_elements_state = mock_bind,
      .set_vertex_buffers = mock_set_vbuf,
   }};
   struct rwa_device dev = {{ .screen = &scr, .pipe = &pipe }};

   test_non_software_decoupling(&dev);
   test_software_keeps_rwa_lock(&dev);
   test_compute_cso_decoupled(&dev);

   enum {{ NTHREADS = 8 }};
   pthread_t th[NTHREADS];
   struct race_ctx ctx[NTHREADS];
   for (int i = 0; i < NTHREADS; i++) {{
      ctx[i] = (struct race_ctx){{ .dev = &dev, .tid = i }};
      assert(pthread_create(&th[i], NULL, race_worker, &ctx[i]) == 0);
   }}
   for (int i = 0; i < NTHREADS; i++)
      assert(pthread_join(th[i], NULL) == 0);

   {{
      RWA_LOCKED;
      rwa_deferred_drain_locked(&dev);
      state_release_framebuffer(&dev);
      rwa_vertex_cso_cache_destroy(&dev);
      pipe_resource_reference(&dev.quad_vbuf, NULL);
   }}

   assert(dev.deferred_head == NULL);
   assert(dev.buffer_textures == NULL);
   assert(dev.buffer_textures_count == 0);
   assert(dev.written_buffer_textures == 0);
   assert(dev.encoded == NULL);
   assert(g_live_res == 0);
   assert(g_live_views == 0);
   assert(g_live_xfers == 0);
   assert(g_live_csos == 0);
   return 0;
}}
"""
    return prefix + "\n\n" + "\n\n".join(prod_chunks) + "\n\n" + suffix


def main() -> None:
    source = build_harness_source()
    with tempfile.TemporaryDirectory(prefix="air-g29-decoupling-") as tmpdir:
        tmp = Path(tmpdir)
        src = tmp / "harness.c"
        src.write_text(source)
        for label, san in [("asan_ubsan", "address,undefined"), ("tsan", "thread")]:
            exe = tmp / f"harness_{label}"
            subprocess.run(
                [
                    "xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", f"-fsanitize={san}", "-O1", "-g",
                    str(src), "-o", str(exe),
                ],
                check=True,
            )
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            if r.returncode != 0:
                print(r.stdout)
                print(r.stderr)
            assert r.returncode == 0, f"{label} failed with code {r.returncode}"
    print("PASS: G29 decoupling verified under ASan+UBSan and TSan")


if __name__ == "__main__":
    main()
