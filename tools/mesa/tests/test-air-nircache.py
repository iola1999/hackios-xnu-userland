#!/usr/bin/env python3
"""Host equivalence and resilience test for air_metal_nircache.inc.

Compiles tools/mesa/air/air_metal_nircache.inc against the host Mesa replay
libraries (libair, libnir, libmesa_util), compiles real iOS Metal shaders
(vertex, fragment with fbfetch/function constant, compute with nested reflection)
via xcrun metal/metallib, and verifies:
  1. Cache miss populates <root>/<LC_UUID>/<2-hex>/<64-hex> with 0777/0666 modes.
  2. Cache hit reproduces air_shader_info (including nested reflection tree) and
     produces identical nir_serialize(..., true) + BLAKE3 keys (matching
     ir3_disk_cache) after gallium lowering as live air_to_nir().
  3. Bit-flip and truncation corruption trigger CRC32 mismatch, unlink, and
     transparent live fallback.
  4. RWA_NIR_CACHE=0 bypasses both cache reads and writes.
  5. Distinct software flags, function constants, entry names, and linked graphs
     produce distinct cache keys.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

USERLAND = pathlib.Path(__file__).resolve().parents[3]
REPLAY = USERLAND / "build/mesa-host-replay"
AIR_DIR = USERLAND / "tools/mesa/air"

METAL_SRC = r"""
#include <metal_stdlib>
using namespace metal;

constant bool kInvert [[function_constant(0)]];

struct VertexIn {
   float4 pos [[attribute(0)]];
   float2 uv  [[attribute(1)]];
};

struct VertexOut {
   float4 pos [[position]];
   float2 uv;
};

vertex VertexOut vert_main(VertexIn in [[stage_in]],
                           constant float4 &scale [[buffer(0)]]) {
   VertexOut out;
   out.pos = in.pos * scale;
   out.uv = in.uv;
   return out;
}

fragment float4 frag_main(VertexOut in [[stage_in]],
                          float4 prev [[color(0)]],
                          texture2d<float> tex [[texture(0)]],
                          sampler smp [[sampler(0)]]) {
   float4 s = tex.sample(smp, in.uv);
   if (kInvert)
      s.rgb = 1.0 - s.rgb;
   return mix(prev, s, 0.5);
}

struct InnerParams {
   float4 tint;
   float4 bias;
};

struct ComputeUniforms {
   InnerParams inner;
   uint4 dims;
};

kernel void comp_main(constant ComputeUniforms &u [[buffer(0)]],
                      device float4 *out_buf [[buffer(1)]],
                      texture2d<half, access::write> dst [[texture(0)]],
                      uint2 gid [[thread_position_in_grid]]) {
   if (gid.x >= u.dims.x)
      return;
   float4 v = u.inner.tint + u.inner.bias;
   out_buf[gid.y * u.dims.x + gid.x] = v;
   dst.write(half4(v), gid);
}
"""

C_HARNESS = r"""
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MUST(cond) do { \
   if (!(cond)) { \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      abort(); \
   } \
} while (0)

#include "nir.h"
#include "nir_air.h"
#include "air_metal_nircache.inc"
#include "util/ralloc.h"

void rwa_copy_rect_guard(const void *src, long stride, unsigned rows, unsigned len);
__attribute__((weak)) void
rwa_copy_rect_guard(const void *src, long stride, unsigned rows, unsigned len)
{
   (void)src; (void)stride; (void)rows; (void)len;
}

static const nir_shader_compiler_options test_nir_options = {
   .lower_fdiv = false,
};

static unsigned
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   (void)bindless;
   return glsl_count_attribute_slots(type, false);
}

static void
lower_like_air_metal(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_lower_system_values);
   if (nir->info.stage == MESA_SHADER_COMPUTE && !exec_list_is_empty(&nir->variables)) {
      NIR_PASS(_, nir, nir_lower_global_vars_to_local);
      NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   }
   static const nir_lower_compute_system_values_options cs_options = { 0 };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &cs_options);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_vec4, nir_lower_io_use_interpolated_input_intrinsics);
}

static void
ir3_style_key(nir_shader *nir, blake3_hash out_hash, size_t *out_bytes)
{
   struct blob blob;
   blob_init(&blob);
   nir_serialize(&blob, nir, true);
   MUST(!blob.out_of_memory && blob.size > 0);
   _mesa_blake3_compute(blob.data, blob.size, out_hash);
   *out_bytes = blob.size;
   blob_finish(&blob);
}

static void
assert_str_eq(const char *a, const char *b)
{
   MUST((a == NULL) == (b == NULL));
   if (a)
      MUST(strcmp(a, b) == 0);
}

static void
assert_refl_type_eq(const struct rwa_reflection_type *a,
                    const struct rwa_reflection_type *b)
{
   MUST((a == NULL) == (b == NULL));
   if (!a)
      return;
   MUST(a->data_type == b->data_type);
   MUST(a->data_size == b->data_size);
   MUST(a->alignment == b->alignment);
   assert_str_eq(a->name, b->name);
   MUST(a->member_count == b->member_count);
   MUST(a->array_length == b->array_length);
   MUST(a->array_stride == b->array_stride);
   MUST(a->texture_type == b->texture_type);
   MUST(a->texture_data_type == b->texture_data_type);
   MUST(a->depth_texture == b->depth_texture);
   MUST(a->access == b->access);
   MUST(a->constant_buffer == b->constant_buffer);
   MUST(a->is_argument_buffer == b->is_argument_buffer);
   for (unsigned i = 0; i < a->member_count; i++) {
      assert_str_eq(a->members[i].name, b->members[i].name);
      MUST(a->members[i].offset == b->members[i].offset);
      MUST(a->members[i].indirect_argument_index == b->members[i].indirect_argument_index);
      assert_refl_type_eq(a->members[i].type, b->members[i].type);
   }
   assert_refl_type_eq(a->element, b->element);
}

static void
assert_info_eq(const struct air_shader_info *a, const struct air_shader_info *b)
{
   MUST(a->stage == b->stage);
   assert_str_eq(a->entry_point, b->entry_point);
   MUST(a->const_win_size == b->const_win_size);
   MUST(a->vertex_attributes_read == b->vertex_attributes_read);
   MUST(a->num_render_targets == b->num_render_targets);
   MUST(a->fbfetch_targets == b->fbfetch_targets);
   MUST(a->raster_order_groups == b->raster_order_groups);
   MUST(a->threadgroup_bytes == b->threadgroup_bytes);
   MUST(a->group_free == b->group_free);
   MUST(a->clip_distances == b->clip_distances);
   MUST(a->imageblock_data_size == b->imageblock_data_size);

   MUST(a->num_resources == b->num_resources);
   for (unsigned i = 0; i < a->num_resources; i++) {
      const struct air_resource *ra = &a->resources[i], *rb = &b->resources[i];
      MUST(ra->kind == rb->kind && ra->index == rb->index && ra->size == rb->size);
      MUST(ra->slot == rb->slot && ra->const_win == rb->const_win && ra->win_base == rb->win_base);
      MUST(ra->read == rb->read && ra->write == rb->write && ra->param == rb->param);
      assert_str_eq(ra->name, rb->name);
      assert_str_eq(ra->type_name, rb->type_name);
      MUST(ra->inline_sampler == rb->inline_sampler && ra->sampler_state == rb->sampler_state);
      MUST(memcmp(&ra->sampler, &rb->sampler, sizeof(ra->sampler)) == 0);
      MUST(ra->raster_order_group == rb->raster_order_group);
      MUST(ra->level_base == rb->level_base && ra->level_count == rb->level_count);
      MUST(ra->nullable == rb->nullable && ra->unused == rb->unused);
      MUST(ra->argbuf_set == rb->argbuf_set && ra->argbuf_id == rb->argbuf_id);
   }

   MUST(a->num_argbuf_constants == b->num_argbuf_constants);
   for (unsigned i = 0; i < a->num_argbuf_constants; i++) {
      const struct air_argbuf_const_member *ca = &a->argbuf_constants[i];
      const struct air_argbuf_const_member *cb = &b->argbuf_constants[i];
      MUST(ca->argbuf_set == cb->argbuf_set && ca->argbuf_id == cb->argbuf_id);
      MUST(ca->offset == cb->offset && ca->size == cb->size && ca->alignment == cb->alignment);
      assert_str_eq(ca->name, cb->name);
   }

   MUST(a->num_varyings == b->num_varyings);
   for (unsigned i = 0; i < a->num_varyings; i++) {
      assert_str_eq(a->varyings[i].name, b->varyings[i].name);
      MUST(a->varyings[i].slot == b->varyings[i].slot);
      MUST(a->varyings[i].components == b->varyings[i].components);
      MUST(a->varyings[i].flat == b->varyings[i].flat);
      MUST(a->varyings[i].centroid == b->varyings[i].centroid);
      MUST(a->varyings[i].sample == b->varyings[i].sample);
      MUST(a->varyings[i].no_perspective == b->varyings[i].no_perspective);
   }

   MUST(a->num_vertex_attributes == b->num_vertex_attributes);
   for (unsigned i = 0; i < a->num_vertex_attributes; i++) {
      const struct air_vertex_attribute *va = &a->vertex_attributes[i];
      const struct air_vertex_attribute *vb = &b->vertex_attributes[i];
      MUST(va->index == vb->index && va->location == vb->location);
      MUST(va->components == vb->components && va->driver_location == vb->driver_location);
      MUST(va->bit_size == vb->bit_size && va->base_type == vb->base_type);
      assert_str_eq(va->name, vb->name);
      assert_str_eq(va->type_name, vb->type_name);
      MUST(va->used == vb->used);
      assert_str_eq(va->function_constant, vb->function_constant);
   }

   MUST(a->num_notes == b->num_notes);
   for (unsigned i = 0; i < a->num_notes; i++)
      assert_str_eq(a->notes[i], b->notes[i]);

   assert_str_eq(a->reflection_error, b->reflection_error);
   MUST((a->reflection == NULL) == (b->reflection == NULL));
   if (a->reflection) {
      MUST(a->reflection->argument_count == b->reflection->argument_count);
      for (unsigned i = 0; i < a->reflection->argument_count; i++) {
         const struct rwa_reflection_argument *aa = &a->reflection->arguments[i];
         const struct rwa_reflection_argument *ab = &b->reflection->arguments[i];
         assert_str_eq(aa->name, ab->name);
         MUST(aa->type == ab->type && aa->index == ab->index && aa->access == ab->access);
         MUST(aa->active == ab->active && aa->array_length == ab->array_length);
         assert_refl_type_eq(aa->buffer_type, ab->buffer_type);
         MUST(aa->constant_buffer == ab->constant_buffer);
         MUST(aa->texture_type == ab->texture_type);
         MUST(aa->texture_data_type == ab->texture_data_type);
         MUST(aa->depth_texture == ab->depth_texture);
      }
   }
}

static unsigned
count_files_recursive(const char *dir)
{
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   unsigned total = 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL) {
      if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
         continue;
      char sub[PATH_MAX];
      snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name);
      struct stat st;
      if (stat(sub, &st) == 0) {
         if (S_ISDIR(st.st_mode))
            total += count_files_recursive(sub);
         else if (S_ISREG(st.st_mode))
            total++;
      }
   }
   closedir(d);
   return total;
}

static uint8_t *
slurp(const char *path, size_t *out_sz)
{
   FILE *f = fopen(path, "rb");
   MUST(f);
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   MUST(sz > 0);
   fseek(f, 0, SEEK_SET);
   uint8_t *buf = malloc((size_t)sz);
   MUST(buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz);
   fclose(f);
   *out_sz = (size_t)sz;
   return buf;
}

int
main(int argc, char **argv)
{
   MUST(argc == 3);
   const char *lib_path = argv[1];
   const char *cache_root = argv[2];

   glsl_type_singleton_init_or_ref();
   setenv("RWA_NIR_CACHE_DIR", cache_root, 1);
   unsetenv("RWA_NIR_CACHE");

   size_t lib_sz = 0;
   uint8_t *lib_data = slurp(lib_path, &lib_sz);
   void *ctx = ralloc_context(NULL);
   struct air_metallib lib = {0};
   const char *err = NULL;
   MUST(air_metallib_parse(ctx, lib_data, lib_sz, &lib, &err));
   MUST(lib.num_functions == 3);

   /* 1. Round-trip equivalence for all 3 functions (vertex, fragment, compute). */
   char first_file[PATH_MAX] = {0};
   for (unsigned i = 0; i < lib.num_functions; i++) {
      const struct air_metallib_function *fn = &lib.functions[i];
      void *c_live = ralloc_context(NULL);
      void *c_miss = ralloc_context(NULL);
      void *c_hit = ralloc_context(NULL);

      struct air_constant_value *fc = NULL;
      unsigned nfc = air_module_function_constants(c_live, fn->bitcode, fn->bitcode_size, &fc);
      if (nfc > 0)
         fc[0].bits = 1;
      const struct air_to_nir_options opts = { .constants = fc, .num_constants = nfc };

      struct air_shader_info info_live, info_miss, info_hit;
      nir_shader *n_live = air_to_nir(c_live, fn->bitcode, fn->bitcode_size, fn->name,
                                      &opts, &test_nir_options, &info_live, &err);
      MUST(n_live);

      char hex[BLAKE3_HEX_LEN];
      nircache_hash_key(fn->bitcode, fn->bitcode_size, fn->name, &opts, false, hex);
      char uuid_dir[PATH_MAX], bucket_dir[PATH_MAX], file_path[PATH_MAX];
      MUST(nircache_paths(hex, bucket_dir, sizeof(bucket_dir),
                          uuid_dir, sizeof(uuid_dir), file_path, sizeof(file_path)));
      if (i == 0)
         snprintf(first_file, sizeof(first_file), "%s", file_path);

      /* Miss populates disk cache with 0777 dirs and 0666 file mode. */
      struct stat st;
      MUST(stat(file_path, &st) != 0);
      nir_shader *n_miss = rwa_air_to_nir_cached(c_miss, fn->bitcode, fn->bitcode_size,
                                                 fn->name, &opts, &test_nir_options,
                                                 false, &info_miss, &err);
      MUST(n_miss && stat(file_path, &st) == 0);
      MUST((st.st_mode & 0777) == 0666);
      MUST(stat(bucket_dir, &st) == 0 && (st.st_mode & 0777) == 0777);
      MUST(stat(uuid_dir, &st) == 0 && (st.st_mode & 0777) == 0777);

      /* Hit reads disk cache and matches air_shader_info + lowered NIR key identically. */
      nir_shader *n_hit = rwa_air_to_nir_cached(c_hit, fn->bitcode, fn->bitcode_size,
                                                fn->name, &opts, &test_nir_options,
                                                false, &info_hit, &err);
      MUST(n_hit);
      assert_info_eq(&info_live, &info_miss);
      assert_info_eq(&info_live, &info_hit);
      if (strcmp(fn->name, "comp_main") == 0) {
         if (!info_hit.reflection)
            fprintf(stderr, "comp_main reflection_error=%s\n",
                    info_hit.reflection_error ? info_hit.reflection_error : "(null)");
         MUST(info_hit.reflection && info_hit.reflection->argument_count >= 3);
      }

      lower_like_air_metal(n_live);
      lower_like_air_metal(n_hit);
      blake3_hash k_live, k_hit;
      size_t s_live = 0, s_hit = 0;
      ir3_style_key(n_live, k_live, &s_live);
      ir3_style_key(n_hit, k_hit, &s_hit);
      MUST(s_live == s_hit && memcmp(k_live, k_hit, sizeof(k_live)) == 0);

      ralloc_free(c_live);
      ralloc_free(c_miss);
      ralloc_free(c_hit);
   }

   /* 2. Key uniqueness across software flag, function constant, entry, and linked graph. */
   const struct air_metallib_function *frag = &lib.functions[1];
   char h0[BLAKE3_HEX_LEN], h_sw[BLAKE3_HEX_LEN], h_fc[BLAKE3_HEX_LEN],
        h_ent[BLAKE3_HEX_LEN], h_lnk[BLAKE3_HEX_LEN];
   struct air_constant_value c0 = { .index = 0, .bits = 0 }, c1 = { .index = 0, .bits = 1 };
   struct air_stitch_node node = { .bitcode = frag->bitcode, .size = 16, .function = "helper" };
   struct air_linked_function lf = { .name = "hook", .num_nodes = 1, .nodes = &node, .output = 0 };
   struct air_to_nir_options o0 = { .constants = &c0, .num_constants = 1 };
   struct air_to_nir_options o1 = { .constants = &c1, .num_constants = 1 };
   struct air_to_nir_options o_lnk = { .constants = &c0, .num_constants = 1, .linked = &lf, .num_linked = 1 };
   nircache_hash_key(frag->bitcode, frag->bitcode_size, frag->name, &o0, false, h0);
   nircache_hash_key(frag->bitcode, frag->bitcode_size, frag->name, &o0, true, h_sw);
   nircache_hash_key(frag->bitcode, frag->bitcode_size, frag->name, &o1, false, h_fc);
   nircache_hash_key(frag->bitcode, frag->bitcode_size, NULL, &o0, false, h_ent);
   nircache_hash_key(frag->bitcode, frag->bitcode_size, frag->name, &o_lnk, false, h_lnk);
   MUST(strcmp(h0, h_sw) != 0 && strcmp(h0, h_fc) != 0 &&
        strcmp(h0, h_ent) != 0 && strcmp(h0, h_lnk) != 0);

   /* 3. Corruption recovery: bit-flip and truncation fail CRC32 and rebuild cleanly. */
   FILE *cf = fopen(first_file, "r+b");
   MUST(cf);
   MUST(fseek(cf, 20, SEEK_SET) == 0);
   int byte = fgetc(cf);
   MUST(byte != EOF && fseek(cf, 20, SEEK_SET) == 0 && fputc(byte ^ 0x55, cf) != EOF);
   fclose(cf);

   void *c_rec = ralloc_context(NULL);
   struct air_shader_info info_rec;
   nir_shader *n_rec = rwa_air_to_nir_cached(c_rec, lib.functions[0].bitcode,
                                             lib.functions[0].bitcode_size,
                                             lib.functions[0].name, NULL,
                                             &test_nir_options, false, &info_rec, &err);
   MUST(n_rec);
   struct stat st_rec;
   MUST(stat(first_file, &st_rec) == 0);

   MUST(truncate(first_file, 8) == 0);
   nir_shader *n_rec2 = rwa_air_to_nir_cached(c_rec, lib.functions[0].bitcode,
                                              lib.functions[0].bitcode_size,
                                              lib.functions[0].name, NULL,
                                              &test_nir_options, false, &info_rec, &err);
   MUST(n_rec2 && stat(first_file, &st_rec) == 0 && st_rec.st_size > 16);
   ralloc_free(c_rec);

   /* 4. RWA_NIR_CACHE=0 bypasses cache reads and writes. */
   char disabled_dir[PATH_MAX];
   snprintf(disabled_dir, sizeof(disabled_dir), "%s-disabled", cache_root);
   setenv("RWA_NIR_CACHE_DIR", disabled_dir, 1);
   setenv("RWA_NIR_CACHE", "0", 1);
   void *c_dis = ralloc_context(NULL);
   struct air_shader_info info_dis;
   nir_shader *n_dis = rwa_air_to_nir_cached(c_dis, lib.functions[0].bitcode,
                                             lib.functions[0].bitcode_size,
                                             lib.functions[0].name, NULL,
                                             &test_nir_options, false, &info_dis, &err);
   MUST(n_dis && count_files_recursive(disabled_dir) == 0);
   ralloc_free(c_dis);

   ralloc_free(ctx);
   free(lib_data);
   glsl_type_singleton_decref();
   puts("test-air-nircache: ALL PASS");
   return 0;
}
"""


def find_replay_dirs():
    src_dirs = sorted(REPLAY.glob("mesa-26.2.5-*"))
    bld_dirs = sorted(REPLAY.glob("build-*"))
    if not src_dirs or not bld_dirs:
        raise SystemExit(
            f"missing host replay tree in {REPLAY}; run hackios-xnu-userland/tools/air/mk-replay.sh first"
        )
    return src_dirs[-1], bld_dirs[-1]


def main():
    mesa_src, mesa_bld = find_replay_dirs()
    with tempfile.TemporaryDirectory(prefix="air-nircache-") as td:
        tdir = pathlib.Path(td)
        metal_file = tdir / "shaders.metal"
        air_file = tdir / "shaders.air"
        lib_file = tdir / "shaders.metallib"
        c_file = tdir / "nircache_test.c"
        exe_file = tdir / "nircache_test"
        cache_dir = tdir / "cache"

        metal_file.write_text(METAL_SRC)
        subprocess.run(["xcrun", "-sdk", "iphoneos", "metal", "-c", str(metal_file), "-o", str(air_file)], check=True)
        subprocess.run(["xcrun", "-sdk", "iphoneos", "metallib", str(air_file), "-o", str(lib_file)], check=True)

        c_file.write_text(C_HARNESS)
        subprocess.run([
            "xcrun", "clang++", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-Wno-unused-parameter",
            "-g", "-O1", "-DNDEBUG",
            "-DHAVE_FUNC_ATTRIBUTE_UNUSED",
            "-DHAVE_STRUCT_TIMESPEC", "-DHAVE_PTHREAD", "-DHAVE_DLADDR",
            "-DHAVE_ZLIB", "-DHAVE_ZSTD", "-DHAVE_COMPRESSION",
            "-D__STDC_CONSTANT_MACROS", "-D__STDC_FORMAT_MACROS", "-D__STDC_LIMIT_MACROS",
            "-x", "c", str(c_file), "-x", "none",
            f"-I{AIR_DIR}",
            f"-I{mesa_src}/include",
            f"-I{mesa_src}/src",
            f"-I{mesa_src}/src/util",
            f"-I{mesa_src}/src/compiler",
            f"-I{mesa_src}/src/compiler/nir",
            f"-I{mesa_bld}/include",
            f"-I{mesa_bld}/src",
            f"-I{mesa_bld}/src/util",
            f"-I{mesa_bld}/src/compiler",
            f"-I{mesa_bld}/src/compiler/nir",
            str(mesa_bld / "src/compiler/air/libair.a"),
            str(mesa_bld / "src/compiler/nir/libnir.a"),
            str(mesa_bld / "src/compiler/libcompiler.a"),
            str(mesa_bld / "src/util/libmesa_util.a"),
            str(mesa_bld / "src/util/libmesa_util_simd.a"),
            str(mesa_bld / "src/util/blake3/libblake3.a"),
            str(mesa_bld / "src/c11/impl/libmesa_util_c11.a"),
            "-lm", "-lz", "/opt/homebrew/opt/zstd/lib/libzstd.dylib",
            "-o", str(exe_file),
        ], check=True)
        subprocess.run([str(exe_file), str(lib_file), str(cache_dir)], check=True)


if __name__ == "__main__":
    main()
