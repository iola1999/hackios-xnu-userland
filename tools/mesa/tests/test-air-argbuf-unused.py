#!/usr/bin/env python3
"""Host verification for unused/conditional argument-buffer members, sparse ids, and NIR cache v2."""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

USERLAND = pathlib.Path(__file__).resolve().parents[3]
REPLAY = USERLAND / "build/mesa-host-replay"
AIR_DIR = USERLAND / "tools/mesa/air"

METAL_SYNTH = r"""
#include <metal_stdlib>
using namespace metal;

struct ArgSet {
   texture2d<float> always_tex [[id(0)]];
   texture2d<float> cond_tex   [[id(1)]];
   texture2d<float> dead_tex   [[id(2)]];
   sampler smp                 [[id(3)]];
   float4 tint                 [[id(4)]];
   texture2d<float> callee_tex [[id(5)]];
};

static __attribute__((noinline)) float4
sample_in_callee(constant ArgSet &args, float2 uv) {
   return args.callee_tex.sample(args.smp, uv);
}

fragment float4 synth_frag(constant ArgSet &args [[buffer(0)]],
                           constant uint &flag [[buffer(1)]],
                           float4 pos [[position]]) {
   float2 uv = pos.xy * 0.01;
   float4 c = args.always_tex.sample(args.smp, uv) * args.tint;
   if (flag != 0u) {
      c += args.cond_tex.sample(args.smp, uv);
   }
   if (flag > 1u) {
      c += sample_in_callee(args, uv);
   }
   return c;
}

fragment float4 unused_argbuf_frag(constant ArgSet &args [[buffer(0)]],
                                   float4 pos [[position]]) {
   return float4(pos.xy, 0.0, 1.0);
}

constant bool kSort [[function_constant(0)]];
constant bool kHasExtra [[function_constant(1)]];

struct ParticleVertArgs {
   device const float4 *positions;
   device const uint *prev_next;
};

vertex float4 synth_pq_vert(device const float4 *positions [[buffer(0)]],
                            device const uint *order [[buffer(9), function_constant(kSort)]],
                            constant float4 &dead_ubo [[buffer(10)]],
                            device const uint *prev_next [[buffer(13)]],
                            uint vid [[vertex_id]]) {
   ParticleVertArgs a;
   a.positions = positions;
   a.prev_next = prev_next; /* stored into local struct, never dereferenced */
   uint idx = kSort ? order[vid] : vid;
   return a.positions[idx];
}

kernel void synth_fc_direct(constant float4 &req_ubo [[buffer(0)]],
                            constant float4 &fc_ubo [[buffer(1), function_constant(kHasExtra)]],
                            device float4 *out_buf [[buffer(2)]],
                            texture2d<float, access::sample> fc_tex [[texture(0), function_constant(kHasExtra)]],
                            texture2d<float, access::write> fc_img [[texture(1), function_constant(kHasExtra)]],
                            sampler fc_smp [[sampler(0), function_constant(kHasExtra)]],
                            uint gid [[thread_position_in_grid]]) {
   float4 v = req_ubo;
   if (kHasExtra) {
      v += fc_ubo + fc_tex.sample(fc_smp, float2(0.5));
      fc_img.write(v, uint2(gid, 0));
   }
   out_buf[gid] = v;
}
"""

C_LIVE_HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const nir_shader_compiler_options test_nir_options = { .lower_fdiv = false };

static uint8_t *
read_file_bytes(const char *path, size_t *out_sz)
{
   FILE *fp = fopen(path, "rb");
   if (!fp) return NULL;
   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz <= 0) { fclose(fp); return NULL; }
   uint8_t *buf = (uint8_t *)malloc((size_t)sz);
   MUST(buf && fread(buf, 1, (size_t)sz, fp) == (size_t)sz);
   fclose(fp);
   *out_sz = (size_t)sz;
   return buf;
}

static void
assert_str_eq(const char *a, const char *b)
{
   MUST((a == NULL) == (b == NULL));
   if (a) MUST(strcmp(a, b) == 0);
}

static void
assert_refl_type_eq(const struct rwa_reflection_type *a, const struct rwa_reflection_type *b)
{
   MUST((a == NULL) == (b == NULL));
   if (!a) return;
   MUST(a->data_type == b->data_type && a->data_size == b->data_size && a->alignment == b->alignment);
   assert_str_eq(a->name, b->name);
   MUST(a->member_count == b->member_count && a->array_length == b->array_length && a->array_stride == b->array_stride);
   MUST(a->texture_type == b->texture_type && a->texture_data_type == b->texture_data_type &&
        a->depth_texture == b->depth_texture && a->access == b->access &&
        a->constant_buffer == b->constant_buffer && a->is_argument_buffer == b->is_argument_buffer);
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
   MUST(a->num_resources == b->num_resources);
   for (unsigned i = 0; i < a->num_resources; i++) {
      const struct air_resource *ra = &a->resources[i], *rb = &b->resources[i];
      MUST(ra->kind == rb->kind && ra->index == rb->index && ra->slot == rb->slot);
      MUST(ra->nullable == rb->nullable && ra->unused == rb->unused);
      MUST(ra->argbuf_set == rb->argbuf_set && ra->argbuf_id == rb->argbuf_id);
      assert_str_eq(ra->name, rb->name);
   }
   MUST(a->num_argbuf_constants == b->num_argbuf_constants);
   for (unsigned i = 0; i < a->num_argbuf_constants; i++) {
      const struct air_argbuf_const_member *ca = &a->argbuf_constants[i];
      const struct air_argbuf_const_member *cb = &b->argbuf_constants[i];
      MUST(ca->argbuf_set == cb->argbuf_set && ca->argbuf_id == cb->argbuf_id);
      MUST(ca->offset == cb->offset && ca->size == cb->size && ca->alignment == cb->alignment);
      assert_str_eq(ca->name, cb->name);
   }
   MUST((a->reflection == NULL) == (b->reflection == NULL));
   if (a->reflection) {
      MUST(a->reflection->argument_count == b->reflection->argument_count);
      for (unsigned i = 0; i < a->reflection->argument_count; i++)
         assert_refl_type_eq(a->reflection->arguments[i].buffer_type,
                             b->reflection->arguments[i].buffer_type);
   }
}

static void
check_cache_roundtrip_fc(const void *bc, size_t sz, const char *entry,
                         const struct air_constant_value *consts, unsigned nconsts,
                         struct air_shader_info *out_info, nir_shader **out_nir, void *mem_ctx)
{
   struct air_to_nir_options opts = { .constants = consts, .num_constants = nconsts };
   const char *err = NULL;

   /* Cold miss -> populates cache */
   struct air_shader_info cold_info = { 0 };
   nir_shader *cold = rwa_air_to_nir_cached(mem_ctx, bc, sz, entry, &opts, &test_nir_options,
                                            false, &cold_info, &err);
   if (!cold || err) {
      fprintf(stderr, "air_to_nir failed for %s: %s\n", entry ? entry : "(auto)", err ? err : "(null)");
      abort();
   }

   /* Warm hit -> loads from cache */
   struct air_shader_info warm_info = { 0 };
   nir_shader *warm = rwa_air_to_nir_cached(mem_ctx, bc, sz, entry, &opts, &test_nir_options,
                                            false, &warm_info, &err);
   if (!warm || err) {
      fprintf(stderr, "warm cache failed for %s: %s\n", entry ? entry : "(auto)", err ? err : "(null)");
      abort();
   }
   assert_info_eq(&cold_info, &warm_info);
   *out_info = warm_info;
   if (out_nir) *out_nir = warm;
}

static void
check_cache_roundtrip(const void *bc, size_t sz, const char *entry,
                      struct air_shader_info *out_info, void *mem_ctx)
{
   struct air_constant_value *consts = NULL;
   unsigned nconsts = air_module_function_constants(mem_ctx, bc, sz, &consts);
   check_cache_roundtrip_fc(bc, sz, entry, consts, nconsts, out_info, NULL, mem_ctx);
}

int
main(int argc, char **argv)
{
   MUST(argc == 3);
   const char *synth_lib = argv[1];
   const char *cache_dir = argv[2];
   setenv("RWA_NIR_CACHE", "1", 1);
   setenv("RWA_NIR_CACHE_DIR", cache_dir, 1);

   void *mem_ctx = ralloc_context(NULL);

   size_t lib_sz = 0;
   uint8_t *lib_data = read_file_bytes(synth_lib, &lib_sz);
   MUST(lib_data != NULL);
   struct air_metallib lib = { 0 };
   const char *merr = NULL;
   MUST(air_metallib_parse(mem_ctx, lib_data, lib_sz, &lib, &merr));
   MUST(lib.num_functions == 4);
   for (unsigned f = 0; f < lib.num_functions; f++) {
      if (!strcmp(lib.functions[f].name, "synth_frag")) {
         struct air_shader_info sinfo = { 0 };
         check_cache_roundtrip(lib.functions[f].bitcode, lib.functions[f].bitcode_size,
                               "synth_frag", &sinfo, mem_ctx);
         bool saw_id0 = false, saw_id1 = false, saw_id2 = false, saw_id3 = false, saw_id5 = false, saw_const = false;
         for (unsigned i = 0; i < sinfo.num_resources; i++) {
            const struct air_resource *r = &sinfo.resources[i];
            if (r->argbuf_set != 0) continue;
            if (r->argbuf_id == ~0u) { saw_const = true; MUST(!r->unused && !r->nullable); }
            else if (r->argbuf_id == 0) { saw_id0 = true; MUST(!r->unused && !r->nullable); }
            else if (r->argbuf_id == 1) { saw_id1 = true; MUST(!r->unused && r->nullable); }
            else if (r->argbuf_id == 2) { saw_id2 = true; MUST(r->unused); }
            else if (r->argbuf_id == 3) { saw_id3 = true; MUST(!r->unused && !r->nullable); }
            else if (r->argbuf_id == 5) { saw_id5 = true; MUST(!r->unused && r->nullable); }
         }
         MUST(saw_id0 && saw_id1 && saw_id2 && saw_id3 && saw_id5 && saw_const);
         printf("  [PASS] synth_frag: unconditional(id=0,3,const), conditional(id=1,callee=5), unused(id=2) + nircache v3\n");
      } else if (!strcmp(lib.functions[f].name, "unused_argbuf_frag")) {
         struct air_shader_info uinfo = { 0 };
         check_cache_roundtrip(lib.functions[f].bitcode, lib.functions[f].bitcode_size,
                               "unused_argbuf_frag", &uinfo, mem_ctx);
         unsigned set0_total = 0;
         bool saw_const = false;
         for (unsigned i = 0; i < uinfo.num_resources; i++) {
            const struct air_resource *r = &uinfo.resources[i];
            if (r->argbuf_set != 0) continue;
            set0_total++;
            MUST(r->unused);
            if (r->argbuf_id == ~0u) saw_const = true;
         }
         MUST(set0_total == 6 && saw_const);
         printf("  [PASS] unused_argbuf_frag: all %u set-0 argbuf resources (incl ~0u) unused=true + nircache v3\n",
                set0_total);
      } else if (!strcmp(lib.functions[f].name, "synth_pq_vert")) {
         /* kSort=1: buffer 13 (prev_next stored to local struct) & buffer 10 (dead_ubo) unused=true; buffer 9 nullable=true. */
         struct air_constant_value fc_on[1] = { { .index = 0, .bits = 1 } };
         struct air_shader_info pq_on = { 0 };
         nir_shader *nir_on = NULL;
         check_cache_roundtrip_fc(lib.functions[f].bitcode, lib.functions[f].bitcode_size,
                                  "synth_pq_vert", fc_on, 1, &pq_on, &nir_on, mem_ctx);
         bool saw0 = false, saw9 = false, saw10 = false, saw13 = false;
         for (unsigned i = 0; i < pq_on.num_resources; i++) {
            const struct air_resource *r = &pq_on.resources[i];
            if (r->index == 0) { saw0 = true; MUST(!r->unused && !r->nullable); }
            else if (r->index == 9) { saw9 = true; MUST(!r->unused && r->nullable); }
            else if (r->index == 10) { saw10 = true; MUST(r->unused); }
            else if (r->index == 13) { saw13 = true; MUST(r->unused); }
         }
         MUST(saw0 && saw9 && saw10 && saw13);
         MUST(nir_on->info.num_ssbos == 10); /* dropped from 14! */

         /* kSort=0: buffer 10 & 13 unused=true, buffer 9 unused or nullable (passes unbound). */
         struct air_constant_value fc_off[1] = { { .index = 0, .bits = 0 } };
         struct air_shader_info pq_off = { 0 };
         nir_shader *nir_off = NULL;
         check_cache_roundtrip_fc(lib.functions[f].bitcode, lib.functions[f].bitcode_size,
                                  "synth_pq_vert", fc_off, 1, &pq_off, &nir_off, mem_ctx);
         for (unsigned i = 0; i < pq_off.num_resources; i++) {
            const struct air_resource *r = &pq_off.resources[i];
            if (r->index == 10 || r->index == 13) MUST(r->unused);
            if (r->index == 9) MUST(r->unused || r->nullable);
         }
         MUST(nir_off->info.num_ssbos <= 10);
         printf("  [PASS] synth_pq_vert: buf13/buf10 unused=true, buf9 nullable=true, num_ssbos=%u (<14)\n",
                nir_on->info.num_ssbos);
      } else if (!strcmp(lib.functions[f].name, "synth_fc_direct")) {
         struct air_constant_value fc_on[1] = { { .index = 1, .bits = 1 } };
         struct air_shader_info fc_info = { 0 };
         check_cache_roundtrip_fc(lib.functions[f].bitcode, lib.functions[f].bitcode_size,
                                  "synth_fc_direct", fc_on, 1, &fc_info, NULL, mem_ctx);
         bool saw_ubo1 = false, saw_tex0 = false, saw_img1 = false, saw_smp0 = false;
         for (unsigned i = 0; i < fc_info.num_resources; i++) {
            const struct air_resource *r = &fc_info.resources[i];
            if (r->kind == AIR_RESOURCE_CONSTANT_BUFFER && r->index == 0) MUST(!r->unused && !r->nullable);
            if (r->kind == AIR_RESOURCE_CONSTANT_BUFFER && r->index == 1) { saw_ubo1 = true; MUST(!r->unused && r->nullable); }
            if (r->kind == AIR_RESOURCE_DEVICE_BUFFER && r->index == 2) MUST(!r->unused && !r->nullable);
            if (r->kind == AIR_RESOURCE_TEXTURE && r->index == 0) { saw_tex0 = true; MUST(!r->unused && r->nullable); }
            if (r->kind == AIR_RESOURCE_IMAGE && r->index == 1) { saw_img1 = true; MUST(!r->unused && r->nullable); }
            if (r->kind == AIR_RESOURCE_SAMPLER && r->index == 0) { saw_smp0 = true; MUST(!r->unused && r->nullable); }
         }
         MUST(saw_ubo1 && saw_tex0 && saw_img1 && saw_smp0);
         printf("  [PASS] synth_fc_direct: FC-gated UBO/tex/img/sampler nullable=true + nircache v3\n");
      }
   }
   free(lib_data);
   ralloc_free(mem_ctx);
   return 0;
}
"""

C_ARGBUF_MEMBER_HARNESS = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RWA_MAX_ARGBUF_MEMBERS 64
#define RWA_ARGBUF_LOCKED ((void)0)

enum rwa_argbuf_kind {
   RWA_ARGBUF_BUFFER = 0,
   RWA_ARGBUF_TEXTURE = 1,
   RWA_ARGBUF_SAMPLER = 2,
   RWA_ARGBUF_CONSTANT = 3,
};

enum air_resource_kind {
   AIR_RESOURCE_CONSTANT_BUFFER,
   AIR_RESOURCE_DEVICE_BUFFER,
   AIR_RESOURCE_THREADGROUP_BUFFER,
   AIR_RESOURCE_TEXTURE,
   AIR_RESOURCE_IMAGE,
   AIR_RESOURCE_SAMPLER,
};

struct rwa_device {
   struct rwa_buffer *encoded;
   unsigned ssbo_offset_align;
};

struct rwa_argbuf_slot {
   unsigned id;
   bool set;
   bool destroyed;
   unsigned kind;
   struct rwa_buffer *buffer;
   size_t offset;
   struct rwa_texture *texture;
   struct rwa_sampler *sampler;
};

struct rwa_argbuf_encoding {
   struct rwa_argbuf_encoding *next;
   size_t offset;
   unsigned members;
   unsigned slots;
   struct rwa_argbuf_slot *slot;
};

struct rwa_buffer {
   struct rwa_device *device;
   size_t size;
   bool listed;
   struct rwa_buffer *next_encoded;
   struct rwa_argbuf_encoding *encodings;
};

struct rwa_texture { struct rwa_device *device; };
struct rwa_sampler { struct rwa_device *device; };

struct rwa_buffer_bind {
   struct rwa_buffer *buffer;
   size_t offset;
};

struct air_resource {
   enum air_resource_kind kind;
   unsigned index;
   const char *name;
   bool nullable;
   bool unused;
   unsigned argbuf_set;
   unsigned argbuf_id;
};

static void
say(char *why, size_t why_size, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   if (why && why_size) vsnprintf(why, why_size, fmt, ap);
   va_end(ap);
}

static int
buffer_offset_ok(const struct rwa_device *device, const struct air_resource *r,
                 const struct rwa_buffer *b, size_t offset, const char *name,
                 const char *what, char *why, size_t why_size)
{
   (void)device; (void)r; (void)name;
   if (offset > b->size) {
      say(why, why_size, "%s offset OOB", what);
      return -1;
   }
   return 0;
}

__ARGBUF_RUNTIME__

int
main(void)
{
   struct rwa_device dev = { .ssbo_offset_align = 64 };
   struct rwa_buffer dest = { .device = &dev, .size = 176 };
   struct rwa_buffer unencoded = { .device = &dev, .size = 176 };
   struct rwa_buffer subbuf = { .device = &dev, .size = 256 };
   struct rwa_texture tex0 = { .device = &dev };
   struct rwa_sampler smp0 = { .device = &dev };
   char why[512] = { 0 };

   /* 1. Sparse [[id(n)]] > 63 in 176-byte dest buffer. */
   assert(rwa_argbuf_encode_texture(&dest, 0, 200, &tex0, why, sizeof(why)) == 0);
   assert(rwa_argbuf_encode_sampler(&dest, 0, 201, &smp0, why, sizeof(why)) == 0);
   assert(rwa_argbuf_encode_texture(&dest, 0, 230, &tex0, why, sizeof(why)) == 0);
   assert(rwa_argbuf_encode_sampler(&dest, 0, 231, &smp0, why, sizeof(why)) == 0);
   assert(rwa_argbuf_encode_buffer(&dest, 0, 500, &subbuf, 64, why, sizeof(why)) == 0);
   assert(rwa_argbuf_encode_buffer(&dest, 0, 571, &subbuf, 0, why, sizeof(why)) == 0);
   assert(rwa_argbuf_encoded_members(&dest, 0) == 6);

   struct rwa_buffer_bind table[1] = { { .buffer = &dest, .offset = 0 } };
   struct rwa_buffer *out_b = NULL;
   struct rwa_texture *out_t = NULL;
   struct rwa_sampler *out_s = NULL;
   size_t out_off = 0;

   struct air_resource r500 = { .kind = AIR_RESOURCE_DEVICE_BUFFER, .name = "sceneBuf", .argbuf_set = 0, .argbuf_id = 500 };
   assert(argbuf_member(&r500, table, 1, "vfx", &out_b, &out_off, &out_t, &out_s, why, sizeof(why)) == 0);
   assert(out_b == &subbuf && out_off == 64);

   /* 2. Unconditional (nullable=false) refuses unbound set, unencoded buffer, and unset id. */
   struct air_resource r_uncond = { .kind = AIR_RESOURCE_TEXTURE, .name = "always_tex", .nullable = false, .argbuf_set = 0, .argbuf_id = 12 };
   assert(argbuf_member(&r_uncond, NULL, 0, "pq", &out_b, &out_off, &out_t, &out_s, why, sizeof(why)) == -1);
   assert(strstr(why, "nothing is bound there") != NULL);

   struct rwa_buffer_bind unenc_table[1] = { { .buffer = &unencoded, .offset = 0 } };
   assert(argbuf_member(&r_uncond, unenc_table, 1, "pq", &out_b, &out_off, &out_t, &out_s, why, sizeof(why)) == -1);
   assert(strstr(why, "was never filled through an MTLArgumentEncoder") != NULL);

   assert(argbuf_member(&r_uncond, table, 1, "pq", &out_b, &out_off, &out_t, &out_s, why, sizeof(why)) == -1);
   assert(strstr(why, "never set on the encoder") != NULL);

   /* 3. Conditional (nullable=true) allows all three missing cases, returning NULL outputs. */
   struct air_resource r_cond = r_uncond;
   r_cond.nullable = true;
   assert(argbuf_member(&r_cond, NULL, 0, "pq", &out_b, &out_off, &out_t, &out_s, why, sizeof(why)) == 0 && out_t == NULL);
   assert(argbuf_member(&r_cond, unenc_table, 1, "pq", &out_b, &out_off, &out_t, &out_s, why, sizeof(why)) == 0 && out_t == NULL);
   assert(argbuf_member(&r_cond, table, 1, "pq", &out_b, &out_off, &out_t, &out_s, why, sizeof(why)) == 0 && out_t == NULL);

   /* 4. Conditional (nullable=true) STILL refuses wrong kind and destroyed resource. */
   struct air_resource r_wrong_kind = { .kind = AIR_RESOURCE_DEVICE_BUFFER, .name = "wrong", .nullable = true, .argbuf_set = 0, .argbuf_id = 200 };
   assert(argbuf_member(&r_wrong_kind, table, 1, "pq", &out_b, &out_off, &out_t, &out_s, why, sizeof(why)) == -1);
   assert(strstr(why, "was encoded there") != NULL);

   argbuf_scrub(&dev, NULL, &tex0, NULL);
   struct air_resource r_destroyed = { .kind = AIR_RESOURCE_TEXTURE, .name = "gone", .nullable = true, .argbuf_set = 0, .argbuf_id = 200 };
   assert(argbuf_member(&r_destroyed, table, 1, "pq", &out_b, &out_off, &out_t, &out_s, why, sizeof(why)) == -1);
   assert(strstr(why, "has been destroyed since") != NULL);

   /* 5. Unused (unused=true, nullable=false) argbuf texture (shadowMap id 23) returns 0 defensively. */
   struct air_resource r_unused_tex = {
      .kind = AIR_RESOURCE_TEXTURE, .name = "shadowMap",
      .unused = true, .nullable = false, .argbuf_set = 0, .argbuf_id = 23,
   };
   assert(argbuf_member(&r_unused_tex, table, 1, "particle_quad_frag", &out_b, &out_off, &out_t, &out_s, why, sizeof(why)) == 0 && out_t == NULL);

   argbuf_forget_all(&dest);
   printf("  [PASS] argbuf_member: sparse id 200..571, nullable vs unconditional, unused, destroyed/wrong-kind refusals\n");
   return 0;
}
"""

C_YUV_RGB2P_HARNESS = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RWA_MAX_TEXTURES 16
#define RWA_PIXEL_NV12 100
#define RWA_PIXEL_NV12_FULL 101
#define RWA_PIXEL_P010 102
#define RWA_PIXEL_RGB2P 200

enum air_resource_kind {
   AIR_RESOURCE_CONSTANT_BUFFER,
   AIR_RESOURCE_DEVICE_BUFFER,
   AIR_RESOURCE_THREADGROUP_BUFFER,
   AIR_RESOURCE_TEXTURE,
   AIR_RESOURCE_IMAGE,
   AIR_RESOURCE_SAMPLER,
};

struct rwa_yuv_key {
   unsigned mask, full_range, bt709, bt2020, p010, rgb2p;
   uint8_t uv_slot[RWA_MAX_TEXTURES];
};
struct rwa_texture_shape { unsigned pixel_format; };
struct rwa_texture {
   struct rwa_texture_shape shape;
   struct rwa_texture *chroma;
   bool yuv_full_range, yuv_bt709, yuv_bt2020;
};
struct rwa_bindings {
   unsigned texture_count;
   struct rwa_texture **textures;
};
struct rwa_buffer_bind { void *buffer; size_t offset; };
struct air_resource {
   enum air_resource_kind kind;
   unsigned index;
   bool nullable, unused;
   unsigned argbuf_set, argbuf_id;
};
struct air_shader_info {
   const char *entry_point;
   unsigned num_resources;
   struct air_resource *resources;
};

static void say(char *why, size_t sz, const char *fmt, ...) {
   va_list ap; va_start(ap, fmt);
   if (why && sz) vsnprintf(why, sz, fmt, ap);
   va_end(ap);
}
static bool rgb2p_format(unsigned fmt) { return fmt == RWA_PIXEL_RGB2P; }
static bool rgb2p_full(unsigned fmt) { (void)fmt; return false; }
static bool rgb2p_prepare_sample(struct rwa_texture *t, char *w, size_t ws) { (void)t; (void)w; (void)ws; return true; }

static int argbuf_member(const struct air_resource *r, const struct rwa_buffer_bind *t,
                         unsigned tc, const char *n, void *b, void *o,
                         struct rwa_texture **tx, void *sm, char *w, size_t ws) {
   (void)t; (void)tc; (void)n; (void)b; (void)o; (void)sm;
   if (tx) *tx = NULL;
   if (r->unused) return 0;
   if (r->argbuf_id == 23) {
      say(w, ws, "never set on the encoder");
      return -1;
   }
   return 0;
}

__YUV_KEY_FOR__
__RGB2P_PREPARE__

int
main(void)
{
   struct rwa_texture uv = { 0 };
   struct rwa_texture nv12 = { .shape = { .pixel_format = RWA_PIXEL_NV12 }, .chroma = &uv };
   struct rwa_texture *texs[1] = { &nv12 };
   struct rwa_bindings binds = { .texture_count = 1, .textures = texs };
   struct rwa_yuv_key key = { 0 };
   char why[256] = { 0 };

   /* 1. Unused argbuf texture (id=23, slot=1) is skipped by both yuv_key_for & rgb2p_prepare_bindings,
    *    and does NOT block slot 1 from being chosen as UV plane for active NV12 texture at slot 0. */
   struct air_resource res_unused[] = {
      { .kind = AIR_RESOURCE_TEXTURE, .index = 0, .argbuf_set = ~0u, .argbuf_id = ~0u },
      { .kind = AIR_RESOURCE_TEXTURE, .index = 1, .unused = true, .nullable = false, .argbuf_set = 0, .argbuf_id = 23 },
   };
   struct air_shader_info info_ok = { .entry_point = "particle_quad_frag", .num_resources = 2, .resources = res_unused };
   assert(yuv_key_for(&info_ok, &binds, NULL, 0, &key, why, sizeof(why)) == true);
   assert(key.mask == 1u && key.uv_slot[0] == 1u);
   assert(rgb2p_prepare_bindings(&info_ok, &binds, NULL, 0, "particle_quad_frag", why, sizeof(why)) == true);

   /* 2. Active (unused=false, nullable=false) unset argbuf texture (id=23) still fails both. */
   res_unused[1].unused = false;
   assert(yuv_key_for(&info_ok, &binds, NULL, 0, &key, why, sizeof(why)) == false);
   assert(strstr(why, "never set on the encoder") != NULL);
   why[0] = '\0';
   assert(rgb2p_prepare_bindings(&info_ok, &binds, NULL, 0, "particle_quad_frag", why, sizeof(why)) == false);
   assert(strstr(why, "never set on the encoder") != NULL);

   printf("  [PASS] yuv_key_for + rgb2p_prepare_bindings: skip unused argbuf textures (no UV slot steal), refuse active unset\n");
   return 0;
}
"""

C_CHECK_BINDINGS_HARNESS = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RWA_MAX_TEXTURES 16
#define MIN2(a, b) ((a) < (b) ? (a) : (b))

enum air_resource_kind {
   AIR_RESOURCE_CONSTANT_BUFFER,
   AIR_RESOURCE_DEVICE_BUFFER,
   AIR_RESOURCE_THREADGROUP_BUFFER,
   AIR_RESOURCE_TEXTURE,
   AIR_RESOURCE_IMAGE,
   AIR_RESOURCE_SAMPLER,
};

struct rwa_device { unsigned ssbo_offset_align; };
struct rwa_buffer { struct rwa_device *device; size_t size; };
struct rwa_texture { bool shader_read, shader_write; };
struct rwa_sampler { int dummy; };
struct rwa_slot { int dummy; };

struct rwa_bindings {
   unsigned buffer_count;
   struct rwa_buffer **buffers;
   unsigned texture_count;
   struct rwa_texture **textures;
   unsigned sampler_count;
   struct rwa_sampler **samplers;
};
struct rwa_buffer_bind { struct rwa_buffer *buffer; size_t offset; };

struct air_resource {
   enum air_resource_kind kind;
   unsigned index, size;
   bool inline_sampler, nullable, unused;
   unsigned argbuf_set, argbuf_id;
};
struct air_shader_info {
   unsigned num_resources;
   struct air_resource *resources;
};

static void say(char *why, size_t sz, const char *fmt, ...) {
   va_list ap; va_start(ap, fmt);
   if (why && sz) vsnprintf(why, sz, fmt, ap);
   va_end(ap);
}
static int argbuf_member(const struct air_resource *r, const struct rwa_buffer_bind *t,
                         unsigned tc, const char *n, struct rwa_buffer **b, size_t *o,
                         struct rwa_texture **tx, struct rwa_sampler **sm, char *w, size_t ws) {
   (void)r; (void)t; (void)tc; (void)n; (void)b; (void)o; (void)tx; (void)sm; (void)w; (void)ws;
   return 0;
}
static int direct_buffer_offset(const struct air_resource *r, const struct rwa_bindings *b,
                                const struct rwa_buffer_bind *t, unsigned tc, const char *n,
                                size_t *o, char *w, size_t ws) {
   (void)r; (void)b; (void)t; (void)tc; (void)n; (void)w; (void)ws; *o = 0; return 0;
}
static int buffer_offset_ok(const struct rwa_device *d, const struct air_resource *r,
                            const struct rwa_buffer *b, size_t o, const char *n,
                            const char *what, char *w, size_t ws) {
   (void)d; (void)r; (void)b; (void)o; (void)n; (void)what; (void)w; (void)ws; return 0;
}
static bool texture_fits(const struct rwa_texture *t, const struct rwa_slot *s,
                         const char *n, unsigned i, char *w, size_t ws) {
   (void)t; (void)s; (void)n; (void)i; (void)w; (void)ws; return true;
}

__CHECK_BINDINGS__

int
main(void)
{
   struct rwa_device dev = { .ssbo_offset_align = 64 };
   struct rwa_buffer b0 = { .device = &dev, .size = 256 };
   struct rwa_buffer *bufs[7] = { &b0, &b0, &b0, &b0, &b0, &b0, &b0 };
   struct rwa_bindings binds = { .buffer_count = 7, .buffers = bufs };
   struct rwa_slot slots[RWA_MAX_TEXTURES] = { 0 };
   char why[256] = { 0 };

   /* 1. particle_quad_vert shape: buf0 active, buf9 FC-nullable, buf13 unused, FC-nullable UBO/tex/img/smp unbound. */
   struct air_resource res[] = {
      { .kind = AIR_RESOURCE_DEVICE_BUFFER, .index = 0, .argbuf_set = ~0u, .argbuf_id = ~0u },
      { .kind = AIR_RESOURCE_DEVICE_BUFFER, .index = 9, .nullable = true, .argbuf_set = ~0u, .argbuf_id = ~0u },
      { .kind = AIR_RESOURCE_DEVICE_BUFFER, .index = 13, .unused = true, .argbuf_set = ~0u, .argbuf_id = ~0u },
      { .kind = AIR_RESOURCE_CONSTANT_BUFFER, .index = 8, .size = 64, .nullable = true, .argbuf_set = ~0u, .argbuf_id = ~0u },
      { .kind = AIR_RESOURCE_TEXTURE, .index = 0, .nullable = true, .argbuf_set = ~0u, .argbuf_id = ~0u },
      { .kind = AIR_RESOURCE_IMAGE, .index = 1, .nullable = true, .argbuf_set = ~0u, .argbuf_id = ~0u },
      { .kind = AIR_RESOURCE_SAMPLER, .index = 0, .nullable = true, .argbuf_set = ~0u, .argbuf_id = ~0u },
   };
   struct air_shader_info info = { .num_resources = 7, .resources = res };
   assert(check_bindings(&info, slots, &binds, NULL, 0, "particle_quad_vert", why, sizeof(why)) == 0);

   /* 2. Active non-FC-gated SSBO/UBO/texture/sampler must still fail when unbound. */
   struct air_resource req_ssbo = { .kind = AIR_RESOURCE_DEVICE_BUFFER, .index = 13, .argbuf_set = ~0u, .argbuf_id = ~0u };
   struct air_shader_info i1 = { .num_resources = 1, .resources = &req_ssbo };
   assert(check_bindings(&i1, slots, &binds, NULL, 0, "req_ssbo", why, sizeof(why)) == -1);
   assert(strstr(why, "reads buffer 13") != NULL);

   struct air_resource req_ubo = { .kind = AIR_RESOURCE_CONSTANT_BUFFER, .index = 8, .size = 64, .argbuf_set = ~0u, .argbuf_id = ~0u };
   struct air_shader_info i2 = { .num_resources = 1, .resources = &req_ubo };
   assert(check_bindings(&i2, slots, &binds, NULL, 0, "req_ubo", why, sizeof(why)) == -1);

   struct air_resource req_tex = { .kind = AIR_RESOURCE_TEXTURE, .index = 0, .argbuf_set = ~0u, .argbuf_id = ~0u };
   struct air_shader_info i3 = { .num_resources = 1, .resources = &req_tex };
   assert(check_bindings(&i3, slots, &binds, NULL, 0, "req_tex", why, sizeof(why)) == -1);

   struct air_resource req_smp = { .kind = AIR_RESOURCE_SAMPLER, .index = 0, .argbuf_set = ~0u, .argbuf_id = ~0u };
   struct air_shader_info i4 = { .num_resources = 1, .resources = &req_smp };
   assert(check_bindings(&i4, slots, &binds, NULL, 0, "req_smp", why, sizeof(why)) == -1);

   printf("  [PASS] check_bindings: unused/nullable direct resources pass unbound; required ones refuse\n");
   return 0;
}
"""


def find_replay_dirs() -> tuple[pathlib.Path, pathlib.Path]:
    src = next(REPLAY.glob("mesa-*"))
    bld = next(REPLAY.glob("build-*"))
    return src, bld


def cut_between(path: pathlib.Path, start: str, end: str) -> str:
    text = path.read_text()
    s = text.index(start)
    e = text.index(end, s)
    return text[s:e]


def main() -> int:
    mesa_src, mesa_bld = find_replay_dirs()
    with tempfile.TemporaryDirectory(prefix="air-argbuf-unused-") as td:
        tdp = pathlib.Path(td)

        # Part A: compile synthetic Metal shader and run live air_to_nir + nircache v2 checks
        metal_file = tdp / "synth.metal"
        air_file = tdp / "synth.air"
        lib_file = tdp / "synth.metallib"
        metal_file.write_text(METAL_SYNTH)
        subprocess.run(
            ["xcrun", "-sdk", "iphoneos", "metal", "-c", str(metal_file), "-o", str(air_file)],
            check=True,
        )
        subprocess.run(
            ["xcrun", "-sdk", "iphoneos", "metallib", str(air_file), "-o", str(lib_file)],
            check=True,
        )

        live_c = tdp / "live_test.c"
        live_exe = tdp / "live_test"
        live_c.write_text(C_LIVE_HARNESS)
        subprocess.run([
            "xcrun", "clang++", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-Wno-unused-parameter", "-g", "-O1", "-DNDEBUG",
            "-DHAVE_FUNC_ATTRIBUTE_UNUSED", "-DHAVE_STRUCT_TIMESPEC", "-DHAVE_PTHREAD",
            "-DHAVE_DLADDR", "-DHAVE_ZLIB", "-DHAVE_ZSTD", "-DHAVE_COMPRESSION",
            "-D__STDC_CONSTANT_MACROS", "-D__STDC_FORMAT_MACROS", "-D__STDC_LIMIT_MACROS",
            "-x", "c", str(live_c), "-x", "none",
            f"-I{AIR_DIR}",
            f"-I{mesa_src}/include", f"-I{mesa_src}/src", f"-I{mesa_src}/src/util",
            f"-I{mesa_src}/src/compiler", f"-I{mesa_src}/src/compiler/nir",
            f"-I{mesa_bld}/include", f"-I{mesa_bld}/src", f"-I{mesa_bld}/src/util",
            f"-I{mesa_bld}/src/compiler", f"-I{mesa_bld}/src/compiler/nir",
            str(mesa_bld / "src/compiler/air/libair.a"),
            str(mesa_bld / "src/compiler/nir/libnir.a"),
            str(mesa_bld / "src/compiler/libcompiler.a"),
            str(mesa_bld / "src/util/libmesa_util.a"),
            str(mesa_bld / "src/util/libmesa_util_simd.a"),
            str(mesa_bld / "src/util/blake3/libblake3.a"),
            str(mesa_bld / "src/c11/impl/libmesa_util_c11.a"),
            "-lm", "-lz", "/opt/homebrew/opt/zstd/lib/libzstd.dylib",
            "-o", str(live_exe),
        ], check=True)
        cache_dir = tdp / "nircache"
        subprocess.run([
            str(live_exe), str(lib_file), str(cache_dir),
        ], check=True)

        # Part B: ASan+UBSan unit test for air_metal_argbuf.inc (argbuf_member + sparse slots)
        argbuf_inc = AIR_DIR / "air_metal_argbuf.inc"
        part1 = cut_between(argbuf_inc, "static struct rwa_argbuf_encoding *\nargbuf_find(", "\n/*\n * WHAT A BUFFER'S OFFSET")
        part2 = cut_between(argbuf_inc, "static int\nargbuf_member(", "\n/*\n * The element type of a texture")
        mem_c = tdp / "argbuf_member_test.c"
        mem_exe = tdp / "argbuf_member_test"
        mem_c.write_text(C_ARGBUF_MEMBER_HARNESS.replace("__ARGBUF_RUNTIME__", part1 + "\n" + part2))
        subprocess.run([
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-fsanitize=address,undefined", str(mem_c), "-o", str(mem_exe),
        ], check=True)
        subprocess.run([str(mem_exe)], check=True)

        # Part C: ASan+UBSan unit test for check_bindings() in air_metal_bind.inc
        bind_inc = AIR_DIR / "air_metal_bind.inc"
        cb_body = cut_between(bind_inc, "static int\ncheck_bindings(", "\n/* A microsecond clock")
        cb_c = tdp / "check_bindings_test.c"
        cb_exe = tdp / "check_bindings_test"
        cb_c.write_text(C_CHECK_BINDINGS_HARNESS.replace("__CHECK_BINDINGS__", cb_body))
        subprocess.run([
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-fsanitize=address,undefined", str(cb_c), "-o", str(cb_exe),
        ], check=True)
        subprocess.run([str(cb_exe)], check=True)

        # Part D: ASan+UBSan unit test for yuv_key_for() & rgb2p_prepare_bindings()
        yuv_inc = AIR_DIR / "air_metal_yuv.inc"
        rgb2p_inc = AIR_DIR / "air_metal_rgb2p.inc"
        yuv_body = cut_between(yuv_inc, "static bool\nyuv_key_for(", "\nstatic bool\nlower_yuv(")
        rgb2p_body = cut_between(rgb2p_inc, "static bool\nrgb2p_prepare_bindings(", "\nint\nrwa_texture_rgb2p_pass(")
        yr_c = tdp / "yuv_rgb2p_test.c"
        yr_exe = tdp / "yuv_rgb2p_test"
        yr_c.write_text(
            C_YUV_RGB2P_HARNESS
            .replace("__YUV_KEY_FOR__", yuv_body)
            .replace("__RGB2P_PREPARE__", rgb2p_body)
        )
        subprocess.run([
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-fsanitize=address,undefined", str(yr_c), "-o", str(yr_exe),
        ], check=True)
        subprocess.run([str(yr_exe)], check=True)

    print("test-air-argbuf-unused: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
