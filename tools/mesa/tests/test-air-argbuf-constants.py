#!/usr/bin/env python3
"""Host ASan+UBSan unit test for air_argbuf.inc + rwa_module_argbufs() inline constant export.

Extracts the real inline-constant recording loop from air_argbuf.inc and the
info.argbuf_constants emission + finalization loops from air_metal_reflect_vertex.inc,
and verifies:
  1. air.indirect_constant members are recorded into info->argbuf_constants (not resources[]).
  2. rwa_module_argbufs() exports them as RWA_ARGBUF_CONSTANT with exact offset/size,
     propagates alignment >= 8, and raises encoded_length to cover offset + size.
  3. Duplicate ids across descriptors and inline constants are rejected.
  4. Constant-only sets (no descriptor resources) still produce a valid layout.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

AIR_DIR = pathlib.Path(__file__).resolve().parents[1] / "air"
AIR_METAL_H = AIR_DIR / "air_metal.h"
NIR_AIR_H = AIR_DIR / "nir_air.h"
AIR_ARGBUF_INC = AIR_DIR / "air_argbuf.inc"
AIR_ENTRY_INC = AIR_DIR / "air_entry.inc"
REFLECT_INC = AIR_DIR / "air_metal_reflect_vertex.inc"


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

#define RWA_MAX_ARGBUF_MEMBERS 64
#define MAX2(a, b) ((a) > (b) ? (a) : (b))

__AIR_METAL_DECLS__

__CONST_MEMBER_DECL__

struct air_shader_info {
   unsigned num_argbuf_constants;
   struct air_argbuf_const_member *argbuf_constants;
};

struct air_argbuf_member {
   uint64_t offset, size;
   unsigned metal_id;
   bool is_constant;
   const char *name;
};

struct air_argbuf {
   unsigned metal_index;
   unsigned size;
   unsigned alignment;
   unsigned num_members;
   struct air_argbuf_member *members;
};

struct air_ctx {
   void *mem_ctx;
   struct air_shader_info *info;
};

#define reralloc(ctx, ptr, type, count) ((type *)realloc((ptr), sizeof(type) * (size_t)(count)))
static void *ralloc_memdup(const void *ctx, const void *src, size_t sz) {
   (void)ctx;
   void *p = malloc(sz);
   if (p && src)
      memcpy(p, src, sz);
   return p;
}
static char *ralloc_strdup(const void *ctx, const char *s) {
   (void)ctx;
   return s ? strdup(s) : NULL;
}
static void ralloc_free(void *p) { (void)p; }

static void
copy_out_argbuf_constants(void *mem_ctx, struct air_shader_info *info)
{
__COPY_OUT_CONSTANTS__
}

static void
say(char *why, size_t why_size, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   if (why && why_size)
      vsnprintf(why, why_size, fmt, ap);
   va_end(ap);
}

static void
record_constants(struct air_ctx *c, struct air_argbuf *ab)
{
   for (unsigned k = 0; k < ab->num_members; k++) {
      struct air_argbuf_member *m = &ab->members[k];
__RECORD_CONST_BODY__
   }
}

static int
emit_and_finalize(struct air_shader_info info, struct rwa_argbuf_layout *out,
                  unsigned n_initial, unsigned max, const char *name,
                  unsigned *out_n, char *why, size_t why_size)
{
   void *mem_ctx = NULL;
   unsigned n = n_initial;
__EMIT_AND_FINALIZE__
   if (out_n)
      *out_n = n;
   return 0;
}

int
main(void)
{
   char why[256] = { 0 };

   /* 1. Mixed VFX set: id=0 (352 B), id=1 (16 B), id=5 (48 B), plus descriptors at id=26,27. */
   struct air_argbuf_member members[] = {
      { .offset = 0,   .size = 352, .metal_id = 0,  .is_constant = true,  .name = "material" },
      { .offset = 352, .size = 16,  .metal_id = 1,  .is_constant = true,  .name = "tint" },
      { .offset = 384, .size = 48,  .metal_id = 5,  .is_constant = true,  .name = "bounds" },
      { .offset = 432, .size = 8,   .metal_id = 26, .is_constant = false, .name = "tex0" },
      { .offset = 440, .size = 8,   .metal_id = 27, .is_constant = false, .name = "samp0" },
   };
   struct air_argbuf ab = {
      .metal_index = 0, .size = 448, .alignment = 16,
      .num_members = 5, .members = members,
   };
   struct air_shader_info info = { 0 };
   struct air_ctx ctx = { .info = &info };
   record_constants(&ctx, &ab);
   assert(info.num_argbuf_constants == 3);
   assert(info.argbuf_constants[0].argbuf_id == 0 && info.argbuf_constants[0].offset == 0 && info.argbuf_constants[0].size == 352);
   assert(info.argbuf_constants[1].argbuf_id == 1 && info.argbuf_constants[1].offset == 352 && info.argbuf_constants[1].size == 16);
   assert(info.argbuf_constants[2].argbuf_id == 5 && info.argbuf_constants[2].offset == 384 && info.argbuf_constants[2].size == 48);

   struct rwa_argbuf_layout sets[4];
   memset(sets, 0, sizeof(sets));
   sets[0].metal_index = 0;
   sets[0].alignment = 8;
   sets[0].member_count = 2;
   sets[0].members[0] = (struct rwa_argbuf_member){ .id = 26, .kind = RWA_ARGBUF_TEXTURE, .index = 0 };
   sets[0].members[1] = (struct rwa_argbuf_member){ .id = 27, .kind = RWA_ARGBUF_SAMPLER, .index = 0 };

   unsigned n = 0;
   assert(emit_and_finalize(info, sets, 1, 4, "vfx_material", &n, why, sizeof(why)) == 0);
   assert(n == 1);
   assert(sets[0].alignment == 16);
   assert(sets[0].member_count == 5);
   assert(sets[0].encoded_length == 432);
   assert(sets[0].members[2].kind == RWA_ARGBUF_CONSTANT && sets[0].members[2].id == 0 &&
          sets[0].members[2].offset == 0 && sets[0].members[2].size == 352);
   assert(sets[0].members[3].kind == RWA_ARGBUF_CONSTANT && sets[0].members[3].id == 1 &&
          sets[0].members[3].offset == 352 && sets[0].members[3].size == 16);
   assert(sets[0].members[4].kind == RWA_ARGBUF_CONSTANT && sets[0].members[4].id == 5 &&
          sets[0].members[4].offset == 384 && sets[0].members[4].size == 48);

   /* 2. Constant-only set (n_initial = 0). */
   memset(sets, 0, sizeof(sets));
   assert(emit_and_finalize(info, sets, 0, 4, "const_only", &n, why, sizeof(why)) == 0);
   assert(n == 1 && sets[0].member_count == 3 && sets[0].encoded_length == 432 && sets[0].alignment == 16);

   /* 3. Duplicate id between descriptor and inline constant must fail. */
   memset(sets, 0, sizeof(sets));
   sets[0].metal_index = 0;
   sets[0].alignment = 8;
   sets[0].member_count = 1;
   sets[0].members[0] = (struct rwa_argbuf_member){ .id = 5, .kind = RWA_ARGBUF_BUFFER, .index = 0 };
   assert(emit_and_finalize(info, sets, 1, 4, "dup_const", &n, why, sizeof(why)) == -1);
   assert(strstr(why, "both id 5") != NULL);

   /* 4. air_entry.inc copies argbuf_constants + .name onto caller mem_ctx before freeing scratch ctx. */
   char *scratch_name0 = strdup("global_constants");
   char *scratch_name1 = strdup("frame_tint");
   struct air_argbuf_const_member *scratch_arr = calloc(2, sizeof(*scratch_arr));
   scratch_arr[0] = (struct air_argbuf_const_member){
      .argbuf_id = 0, .offset = 0, .size = 64, .alignment = 16, .name = scratch_name0,
   };
   scratch_arr[1] = (struct air_argbuf_const_member){
      .argbuf_id = 1, .offset = 64, .size = 16, .alignment = 16, .name = scratch_name1,
   };
   struct air_shader_info copied_info = {
      .num_argbuf_constants = 2,
      .argbuf_constants = scratch_arr,
   };
   copy_out_argbuf_constants(NULL, &copied_info);
   /* Free original scratch allocations; ASan will trap if copied_info still aliases scratch. */
   memset(scratch_arr, 0xdd, 2 * sizeof(*scratch_arr));
   free(scratch_arr);
   free(scratch_name0);
   free(scratch_name1);
   assert(copied_info.argbuf_constants[0].argbuf_id == 0 && copied_info.argbuf_constants[0].size == 64);
   assert(copied_info.argbuf_constants[1].argbuf_id == 1 && copied_info.argbuf_constants[1].offset == 64);
   assert(strcmp(copied_info.argbuf_constants[0].name, "global_constants") == 0);
   assert(strcmp(copied_info.argbuf_constants[1].name, "frame_tint") == 0);
   free((void *)copied_info.argbuf_constants[0].name);
   free((void *)copied_info.argbuf_constants[1].name);
   free(copied_info.argbuf_constants);

   /* 5. Sparse [[id(n)]] > 63 (VFX particle_quad_frag id=200,201,230,231,500,571 in 176 B argbuf). */
   memset(sets, 0, sizeof(sets));
   sets[0].metal_index = 0;
   sets[0].alignment = 16;
   sets[0].encoded_length = 176;
   sets[0].member_count = 6;
   sets[0].members[0] = (struct rwa_argbuf_member){ .id = 200, .kind = RWA_ARGBUF_TEXTURE, .index = 0 };
   sets[0].members[1] = (struct rwa_argbuf_member){ .id = 201, .kind = RWA_ARGBUF_SAMPLER, .index = 0 };
   sets[0].members[2] = (struct rwa_argbuf_member){ .id = 230, .kind = RWA_ARGBUF_TEXTURE, .index = 1 };
   sets[0].members[3] = (struct rwa_argbuf_member){ .id = 231, .kind = RWA_ARGBUF_SAMPLER, .index = 1 };
   sets[0].members[4] = (struct rwa_argbuf_member){ .id = 500, .kind = RWA_ARGBUF_BUFFER,  .index = 0 };
   sets[0].members[5] = (struct rwa_argbuf_member){ .id = 571, .kind = RWA_ARGBUF_BUFFER,  .index = 1 };
   struct air_shader_info empty_info = { 0 };
   assert(emit_and_finalize(empty_info, sets, 1, 4, "vfx_sparse_ids", &n, why, sizeof(why)) == 0);
   assert(n == 1 && sets[0].encoded_length == 176 && sets[0].member_count == 6);

   /* Duplicate sparse id > 63 must still be rejected. */
   sets[0].members[5].id = 500;
   assert(emit_and_finalize(empty_info, sets, 1, 4, "vfx_sparse_dup", &n, why, sizeof(why)) == -1);
   assert(strstr(why, "both id 500") != NULL);

   free(info.argbuf_constants);
   printf("test-air-argbuf-constants: PASS\n");
   return 0;
}
"""


def main() -> int:
    air_decls = cut_between(
        AIR_METAL_H,
        "enum rwa_argbuf_kind {",
        "struct rwa_argbuf_member members[RWA_MAX_ARGBUF_MEMBERS];\n};",
        include_end=True,
    )
    const_member_decl = cut_between(
        NIR_AIR_H,
        "struct air_argbuf_const_member {",
        "};",
        include_end=True,
    )
    record_const_body = cut_between(
        AIR_ARGBUF_INC,
        "         if (m->is_constant) {",
        "         if (m->span > 1) {",
    )
    copy_out_constants = cut_between(
        AIR_ENTRY_INC,
        "      if (info->num_argbuf_constants) {",
        "      if (info->num_varyings) {",
    )
    emit_and_finalize = cut_between(
        REFLECT_INC,
        "   for (unsigned i = 0; i < info.num_argbuf_constants; i++) {",
        "\n   if (count)",
    )
    code = (
        HARNESS
        .replace("__AIR_METAL_DECLS__", air_decls)
        .replace("__CONST_MEMBER_DECL__", const_member_decl)
        .replace("__RECORD_CONST_BODY__", record_const_body)
        .replace("__COPY_OUT_CONSTANTS__", copy_out_constants)
        .replace("__EMIT_AND_FINALIZE__", emit_and_finalize)
    )
    with tempfile.TemporaryDirectory(prefix="air-argbuf-const-") as td:
        cpath = pathlib.Path(td) / "harness.c"
        exe = pathlib.Path(td) / "harness"
        cpath.write_text(code)
        subprocess.run(
            ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(cpath), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
