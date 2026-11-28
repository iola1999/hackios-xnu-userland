#!/usr/bin/env python3
"""Host ASan+UBSan unit test for AIR render reflection, argbuf struct reflection, clone, and depth-only."""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

AIR_DIR = pathlib.Path(__file__).resolve().parents[1] / "air"
AIR_REFL_C = AIR_DIR / "air_reflection.c"
AIR_METAL_REFL_H = AIR_DIR / "air_metal_reflection.h"
RENDER_CREATE_INC = AIR_DIR / "air_metal_render_create.inc"


def cut_between(path: pathlib.Path, start_marker: str, end_marker: str, include_end: bool = False) -> str:
    text = path.read_text()
    s = text.index(start_marker)
    e = text.index(end_marker, s)
    if include_end:
        e += len(end_marker)
    return text[s:e]


HARNESS = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_metal_reflection.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Minimal ralloc/air_md stubs to execute the production reflection helpers under ASan+UBSan. */
static void *rzalloc_bytes(const void *ctx, size_t sz) { (void)ctx; return calloc(1, sz); }
#define rzalloc(ctx, type) ((type *)rzalloc_bytes((ctx), sizeof(type)))
#define rzalloc_array(ctx, type, n) ((type *)rzalloc_bytes((ctx), sizeof(type) * (size_t)(n)))
#define reralloc(ctx, ptr, type, n) ((void)(ctx), (type *)realloc((ptr), sizeof(type) * (size_t)(n)))
static char *ralloc_strdup(void *ctx, const char *s) { (void)ctx; return s ? strdup(s) : NULL; }

enum air_md_kind { AIR_MD_STRING, AIR_MD_VALUE, AIR_MD_NODE };
enum air_type_kind {
   AIR_TYPE_FLOAT, AIR_TYPE_HALF, AIR_TYPE_INT,
   AIR_TYPE_VECTOR, AIR_TYPE_ARRAY, AIR_TYPE_STRUCT,
};
struct air_type {
   enum air_type_kind kind;
   unsigned bits;
   unsigned count;
   const struct air_type *elem;
   unsigned num_members;
   const struct air_type *const *members;
};
struct air_value { uint64_t u; };
struct air_md {
   enum air_md_kind kind;
   const char *str;
   const struct air_value *value;
   unsigned num_ops;
   const struct air_md *const *ops;
};
struct reflection_ctx {
   void *mem_ctx;
   const void *module;
   const char *error;
};

static bool
reflection_error(struct reflection_ctx *c, const char *fmt, ...)
{
   (void)fmt;
   if (!c->error)
      c->error = "error";
   return false;
}

static const char *air_md_string(const struct air_md *m) { return m && m->kind == AIR_MD_STRING ? m->str : NULL; }
static bool air_const_uint(const struct air_value *v, uint64_t *out) { if (!v) return false; *out = v->u; return true; }

static const char *md_string(const struct air_md *md, unsigned index) {
   return md && md->kind == AIR_MD_NODE && index < md->num_ops ? air_md_string(md->ops[index]) : NULL;
}
static bool md_uint(const struct air_md *md, unsigned index, unsigned *out) {
   uint64_t v = 0;
   if (!md || md->kind != AIR_MD_NODE || index >= md->num_ops || !md->ops[index] ||
       md->ops[index]->kind != AIR_MD_VALUE || !air_const_uint(md->ops[index]->value, &v))
      return false;
   *out = (unsigned)v;
   return true;
}
static int md_key(const struct air_md *md, const char *key) {
   for (unsigned i = 0; i < (md ? md->num_ops : 0); i++) {
      const char *s = md_string(md, i);
      if (s && !strcmp(s, key)) return (int)i;
   }
   return -1;
}
static const char *md_key_string(const struct air_md *md, const char *key) {
   int i = md_key(md, key);
   return i >= 0 ? md_string(md, (unsigned)i + 1) : NULL;
}
static bool md_key_uint(const struct air_md *md, const char *key, unsigned *out) {
   int i = md_key(md, key);
   return i >= 0 && md_uint(md, (unsigned)i + 1, out);
}
static const struct air_md *md_key_node(const struct air_md *md, const char *key) {
   int i = md_key(md, key);
   return i >= 0 && (unsigned)i + 1 < md->num_ops && md->ops[i + 1] &&
          md->ops[i + 1]->kind == AIR_MD_NODE ? md->ops[i + 1] : NULL;
}

static void say(char *why, size_t why_size, const char *fmt, ...) {
   va_list ap; va_start(ap, fmt);
   if (why && why_size) vsnprintf(why, why_size, fmt, ap);
   va_end(ap);
}

__SCALAR_FUNCS__
__REFLECT_TEXTURE__
__IS_BUILTIN__
__REFLECT_STRUCT_FROM_MD__
__CLONE_FUNCS__

struct air_shader_info {
   const char *entry_point;
   struct rwa_compute_reflection *reflection;
   const char *reflection_error;
};
struct rwa_render {
   bool generated_vertex;
   struct air_shader_info vinfo;
   struct air_shader_info finfo;
};

__RENDER_GET_REFLECTION__

static void free_type(const struct rwa_reflection_type *t) {
   if (!t) return;
   free((void *)t->name);
   for (unsigned i = 0; i < t->member_count; i++) {
      free((void *)t->members[i].name);
      free_type(t->members[i].type);
   }
   free((void *)t->members);
   free_type(t->element);
   free((void *)t);
}

static void free_refl(struct rwa_compute_reflection *r) {
   if (!r) return;
   for (unsigned i = 0; i < r->argument_count; i++) {
      free((void *)r->arguments[i].name);
      free_type(r->arguments[i].buffer_type);
   }
   free((void *)r->arguments);
   free(r);
}

int
main(void)
{
   /* 1. is_builtin covers vertex/fragment/tessellation/mesh builtins and stage I/O. */
   const struct air_md s_vi = { .kind = AIR_MD_STRING, .str = "air.vertex_input" };
   const struct air_md s_fi = { .kind = AIR_MD_STRING, .str = "air.fragment_input" };
   const struct air_md s_pos = { .kind = AIR_MD_STRING, .str = "air.position" };
   const struct air_md s_vid = { .kind = AIR_MD_STRING, .str = "air.vertex_id" };
   const struct air_md s_rt = { .kind = AIR_MD_STRING, .str = "air.render_target" };
   const struct air_md s_si = { .kind = AIR_MD_STRING, .str = "air.stage_in" };
   const struct air_md s_dep = { .kind = AIR_MD_STRING, .str = "air.depth" };
   const struct air_md s_buf = { .kind = AIR_MD_STRING, .str = "air.buffer" };
   const struct air_md *op_vi[] = { &s_vi };
   const struct air_md *op_fi[] = { &s_fi };
   const struct air_md *op_pos[] = { &s_pos };
   const struct air_md *op_vid[] = { &s_vid };
   const struct air_md *op_rt[] = { &s_rt };
   const struct air_md *op_si[] = { &s_si };
   const struct air_md *op_dep[] = { &s_dep };
   const struct air_md *op_buf[] = { &s_buf };
   struct air_md n_vi = { .kind = AIR_MD_NODE, .num_ops = 1, .ops = op_vi };
   struct air_md n_fi = { .kind = AIR_MD_NODE, .num_ops = 1, .ops = op_fi };
   struct air_md n_pos = { .kind = AIR_MD_NODE, .num_ops = 1, .ops = op_pos };
   struct air_md n_vid = { .kind = AIR_MD_NODE, .num_ops = 1, .ops = op_vid };
   struct air_md n_rt = { .kind = AIR_MD_NODE, .num_ops = 1, .ops = op_rt };
   struct air_md n_si = { .kind = AIR_MD_NODE, .num_ops = 1, .ops = op_si };
   struct air_md n_dep = { .kind = AIR_MD_NODE, .num_ops = 1, .ops = op_dep };
   struct air_md n_buf = { .kind = AIR_MD_NODE, .num_ops = 1, .ops = op_buf };
   assert(is_builtin(&n_vi) && is_builtin(&n_fi) && is_builtin(&n_pos) && is_builtin(&n_vid));
   assert(is_builtin(&n_rt) && is_builtin(&n_si) && is_builtin(&n_dep));
   assert(!is_builtin(&n_buf));

   /* 1b. packed_* vectors, 1-member metal::matrix wrapper, and ushort/short/uchar/char textures. */
   struct air_type sc = { 0 };
   unsigned rows = 0, cols = 0;
   bool packed = false;
   assert(scalar_type("packed_float3", &sc, &rows, &cols, &packed) == 5 && rows == 3 && cols == 1 && packed);
   const struct air_type t_f32 = { .kind = AIR_TYPE_FLOAT, .bits = 32 };
   const struct air_type t_pf3 = { .kind = AIR_TYPE_ARRAY, .count = 3, .elem = &t_f32 };
   assert(scalar_matches(&t_pf3, &sc, rows, cols, packed));
   assert(scalar_type("float4x4", &sc, &rows, &cols, &packed) == 15 && rows == 4 && cols == 4 && !packed);
   const struct air_type t_f4 = { .kind = AIR_TYPE_VECTOR, .count = 4, .elem = &t_f32 };
   const struct air_type t_mat_arr = { .kind = AIR_TYPE_ARRAY, .count = 4, .elem = &t_f4 };
   const struct air_type *mat_m[] = { &t_mat_arr };
   const struct air_type t_mat_wrap = { .kind = AIR_TYPE_STRUCT, .num_members = 1, .members = mat_m };
   assert(scalar_matches(&t_mat_wrap, &sc, rows, cols, packed));

   struct reflection_ctx tctx = { 0 };
   const struct air_md s_read = { .kind = AIR_MD_STRING, .str = "air.read" };
   const struct air_md *op_read[] = { &s_read };
   struct air_md n_read = { .kind = AIR_MD_NODE, .num_ops = 1, .ops = op_read };
   struct rwa_reflection_argument targ = { .name = "reproj", .array_length = 1 };
   assert(reflect_texture(&tctx, "texture2d<ushort, read>", &n_read, &targ));
   assert(targ.texture_type == 2 && targ.texture_data_type == 41 && !targ.depth_texture);

   /* 2. reflect_struct_from_md parses VFX indirect_argument members, arrays, and nested sub_id. */
   const struct air_value v0 = { 0 }, v1 = { 1 }, v2 = { 2 }, v4 = { 4 }, v8 = { 8 }, v10 = { 10 }, v16 = { 16 }, v24 = { 24 }, v26 = { 26 }, v27 = { 27 }, v32 = { 32 }, v40 = { 40 };
   const struct air_md m0 = { .kind = AIR_MD_VALUE, .value = &v0 };
   const struct air_md m1 = { .kind = AIR_MD_VALUE, .value = &v1 };
   const struct air_md m2 = { .kind = AIR_MD_VALUE, .value = &v2 };
   const struct air_md m4 = { .kind = AIR_MD_VALUE, .value = &v4 };
   const struct air_md m8 = { .kind = AIR_MD_VALUE, .value = &v8 };
   const struct air_md m10 = { .kind = AIR_MD_VALUE, .value = &v10 };
   const struct air_md m16 = { .kind = AIR_MD_VALUE, .value = &v16 };
   const struct air_md m24 = { .kind = AIR_MD_VALUE, .value = &v24 };
   const struct air_md m26 = { .kind = AIR_MD_VALUE, .value = &v26 };
   const struct air_md m27 = { .kind = AIR_MD_VALUE, .value = &v27 };
   const struct air_md m32 = { .kind = AIR_MD_VALUE, .value = &v32 };
   const struct air_md m40 = { .kind = AIR_MD_VALUE, .value = &v40 };
   const struct air_md s_sti = { .kind = AIR_MD_STRING, .str = "air.struct_type_info" };
   const struct air_md s_ia = { .kind = AIR_MD_STRING, .str = "air.indirect_argument" };
   const struct air_md s_ic = { .kind = AIR_MD_STRING, .str = "air.indirect_constant" };
   const struct air_md s_loc = { .kind = AIR_MD_STRING, .str = "air.location_index" };
   const struct air_md s_tex = { .kind = AIR_MD_STRING, .str = "air.texture" };
   const struct air_md s_samp = { .kind = AIR_MD_STRING, .str = "air.sampler" };
   const struct air_md s_f4 = { .kind = AIR_MD_STRING, .str = "float4" };
   const struct air_md s_f = { .kind = AIR_MD_STRING, .str = "float" };
   const struct air_md s_tint = { .kind = AIR_MD_STRING, .str = "tint" };
   const struct air_md s_t2d = { .kind = AIR_MD_STRING, .str = "texture2d<float, sample>" };
   const struct air_md s_tex0 = { .kind = AIR_MD_STRING, .str = "tex0" };
   const struct air_md s_smpt = { .kind = AIR_MD_STRING, .str = "sampler" };
   const struct air_md s_samp0 = { .kind = AIR_MD_STRING, .str = "samp0" };
   const struct air_md s_bptr = { .kind = AIR_MD_STRING, .str = "device float*" };
   const struct air_md s_buf0 = { .kind = AIR_MD_STRING, .str = "buf0" };
   const struct air_md s_sub_t = { .kind = AIR_MD_STRING, .str = "SubBlock" };
   const struct air_md s_sub_f = { .kind = AIR_MD_STRING, .str = "sub" };
   const struct air_md s_weights = { .kind = AIR_MD_STRING, .str = "weights" };

   const struct air_md s_as = { .kind = AIR_MD_STRING, .str = "air.address_space" };
   const struct air_md s_ats = { .kind = AIR_MD_STRING, .str = "air.arg_type_size" };
   const struct air_md s_ata = { .kind = AIR_MD_STRING, .str = "air.arg_type_align_size" };
   const struct air_md s_atn = { .kind = AIR_MD_STRING, .str = "air.arg_type_name" };
   const struct air_md s_rw = { .kind = AIR_MD_STRING, .str = "air.read_write" };
   const struct air_md *d0_ops[] = { &s_ic, &s_loc, &m0 };
   const struct air_md *d1_ops[] = { &s_buf, &s_loc, &m10, &s_rw, &s_as, &m2, &s_ats, &m4, &s_ata, &m4, &s_atn, &s_f };
   const struct air_md *d2_ops[] = { &s_tex, &s_loc, &m26 };
   const struct air_md *d3_ops[] = { &s_samp, &s_loc, &m27 };
   const struct air_md *d_sub_ops[] = { &s_ic, &s_loc, &m2 };
   const struct air_md d0 = { .kind = AIR_MD_NODE, .num_ops = ARRAY_SIZE(d0_ops), .ops = d0_ops };
   const struct air_md d1 = { .kind = AIR_MD_NODE, .num_ops = ARRAY_SIZE(d1_ops), .ops = d1_ops };
   const struct air_md d2 = { .kind = AIR_MD_NODE, .num_ops = ARRAY_SIZE(d2_ops), .ops = d2_ops };
   const struct air_md d3 = { .kind = AIR_MD_NODE, .num_ops = ARRAY_SIZE(d3_ops), .ops = d3_ops };
   const struct air_md d_sub = { .kind = AIR_MD_NODE, .num_ops = ARRAY_SIZE(d_sub_ops), .ops = d_sub_ops };
   const struct air_md *sub_sti_ops[] = {
      &m0, &m4, &m2, &s_f, &s_weights, &s_ia, &d_sub,
   };
   const struct air_md sub_sti = { .kind = AIR_MD_NODE, .num_ops = ARRAY_SIZE(sub_sti_ops), .ops = sub_sti_ops };

   const struct air_md *sti_ops[] = {
      &m0,  &m16, &m0, &s_f4,    &s_tint,  &s_ia, &d0,
      &m16, &m8,  &m0, &s_bptr,  &s_buf0,  &s_ia, &d1,
      &m24, &m8,  &m0, &s_t2d,   &s_tex0,  &s_ia, &d2,
      &m32, &m8,  &m0, &s_smpt,  &s_samp0, &s_ia, &d3,
      &s_sti, &sub_sti, &m40, &m8, &m0, &s_sub_t, &s_sub_f, &s_ia, &m1,
   };
   const struct air_md sti = { .kind = AIR_MD_NODE, .num_ops = ARRAY_SIZE(sti_ops), .ops = sti_ops };
   struct reflection_ctx ctx = { 0 };
   struct rwa_reflection_type *st = reflect_struct_from_md(&ctx, "VFXPass", &sti, 48, 16, 0, 0);
   assert(st != NULL && st->data_type == 1 && st->data_size == 48 && st->alignment == 16);
   st->is_argument_buffer = 1;
   assert(st->member_count == 5);
   assert(st->members[0].offset == 0 && st->members[0].indirect_argument_index == 0 && st->members[0].type->data_type == 6);
   assert(st->members[1].offset == 16 && st->members[1].indirect_argument_index == 10 && st->members[1].type->data_type == 60);
   assert(st->members[1].type->access == 1 && st->members[1].type->constant_buffer == 1);
   assert(st->members[1].type->element != NULL && st->members[1].type->element->data_type == 3 &&
          st->members[1].type->element->data_size == 4 && st->members[1].type->element->alignment == 4);
   assert(st->members[2].offset == 24 && st->members[2].indirect_argument_index == 26 && st->members[2].type->data_type == 58);
   assert(st->members[2].type->texture_type == 2 && st->members[2].type->texture_data_type == 3 &&
          st->members[2].type->depth_texture == 0 && st->members[2].type->access == 0);
   assert(st->members[3].offset == 32 && st->members[3].indirect_argument_index == 27 && st->members[3].type->data_type == 59);
   assert(st->members[4].offset == 40 && st->members[4].indirect_argument_index == 1 && st->members[4].type->data_type == 1);
   assert(st->members[4].type->member_count == 1);
   assert(st->members[4].type->members[0].indirect_argument_index == 3);
   assert(st->members[4].type->members[0].type->data_type == 2 && st->members[4].type->members[0].type->array_length == 2);

   /* 3. air_reflection_clone preserves all fields after source is freed (vertex cache reuse). */
   struct rwa_reflection_argument *orig_args = calloc(1, sizeof(*orig_args));
   orig_args[0] = (struct rwa_reflection_argument){
      .name = strdup("passArgs"), .type = 0, .index = 0, .active = 1, .array_length = 1,
      .buffer_type = st, .constant_buffer = 1,
   };
   struct rwa_compute_reflection *orig = calloc(1, sizeof(*orig));
   orig->argument_count = 1;
   orig->arguments = orig_args;
   struct rwa_compute_reflection *cloned = air_reflection_clone(NULL, orig);
   free_refl(orig);
   assert(cloned != NULL && cloned->argument_count == 1);
   assert(!strcmp(cloned->arguments[0].name, "passArgs"));
   assert(cloned->arguments[0].buffer_type->member_count == 5);
   assert(cloned->arguments[0].buffer_type->members[2].indirect_argument_index == 26);
   assert(cloned->arguments[0].buffer_type->members[4].type->members[0].indirect_argument_index == 3);

   /* 4. rwa_render_get_reflection: full, depth-only, generated-vertex, missing error. */
   struct rwa_render r_full = {
      .generated_vertex = false,
      .vinfo = { .entry_point = "custom_mesh_vert", .reflection = cloned },
      .finfo = { .entry_point = "vfx_fragment", .reflection = cloned },
   };
   struct rwa_render_reflection out = { 0 };
   char why[256] = { 0 };
   assert(rwa_render_get_reflection(&r_full, &out, why, sizeof(why)) == 0);
   assert(out.vertex == cloned && out.fragment == cloned);

   struct rwa_render r_depth = {
      .generated_vertex = false,
      .vinfo = { .entry_point = "particle_quad_vert", .reflection = cloned },
      .finfo = { .entry_point = NULL, .reflection = NULL },
   };
   assert(rwa_render_get_reflection(&r_depth, &out, why, sizeof(why)) == 0);
   assert(out.vertex == cloned && out.fragment == NULL);

   struct rwa_render r_gen = {
      .generated_vertex = true,
      .finfo = { .entry_point = "post_process", .reflection = cloned },
   };
   assert(rwa_render_get_reflection(&r_gen, &out, why, sizeof(why)) == 0);
   assert(out.vertex == NULL && out.fragment == cloned);

   struct rwa_render r_err = {
      .generated_vertex = false,
      .vinfo = { .entry_point = "bad_vert", .reflection = NULL, .reflection_error = "unsupported" },
   };
   assert(rwa_render_get_reflection(&r_err, &out, why, sizeof(why)) == -1);
   assert(strstr(why, "unsupported") != NULL);

   free_refl(cloned);
   printf("test-air-render-reflection: PASS\n");
   return 0;
}
"""


def main() -> int:
    scalar_funcs = cut_between(
        AIR_REFL_C,
        "static unsigned\nscalar_type(",
        "\nstatic struct rwa_reflection_type *\nreflect_struct_from_md(",
    )
    reflect_texture = cut_between(
        AIR_REFL_C,
        "static bool\nparse_texture_type_name(",
        "\nstatic bool\nis_builtin(",
    )
    is_builtin = cut_between(
        AIR_REFL_C,
        "static bool\nis_builtin(",
        "\nstatic struct rwa_reflection_type *\nreflect_struct_from_md(struct reflection_ctx *c, const char *name,\n                       const struct air_md *members, unsigned size,\n                       unsigned alignment, unsigned base_id, unsigned depth)\n{",
    )
    reflect_struct_from_md = cut_between(
        AIR_REFL_C,
        "static struct rwa_reflection_type *\nreflect_struct_from_md(struct reflection_ctx *c, const char *name,\n                       const struct air_md *members, unsigned size,\n                       unsigned alignment, unsigned base_id, unsigned depth)\n{",
        "\nstruct rwa_compute_reflection *\nair_stage_reflection(",
    )
    clone_funcs = AIR_REFL_C.read_text()[
        AIR_REFL_C.read_text().index("static struct rwa_reflection_type *\nclone_reflection_type("):
    ]
    render_get_refl = RENDER_CREATE_INC.read_text()[
        RENDER_CREATE_INC.read_text().index("int\nrwa_render_get_reflection("):
    ]
    code = (
        HARNESS
        .replace("__SCALAR_FUNCS__", scalar_funcs)
        .replace("__REFLECT_TEXTURE__", reflect_texture)
        .replace("__IS_BUILTIN__", is_builtin)
        .replace("__REFLECT_STRUCT_FROM_MD__", reflect_struct_from_md)
        .replace("__CLONE_FUNCS__", clone_funcs)
        .replace("__RENDER_GET_REFLECTION__", render_get_refl)
    )
    with tempfile.TemporaryDirectory(prefix="air-render-refl-") as td:
        cpath = pathlib.Path(td) / "harness.c"
        exe = pathlib.Path(td) / "harness"
        cpath.write_text(code)
        subprocess.run(
            [
                "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(AIR_DIR),
                str(cpath), "-o", str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
