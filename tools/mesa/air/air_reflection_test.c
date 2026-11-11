/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

#include "air_reflection.h"
#include "nir_air.h"
#include "util/ralloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "reflection:%d: %s\n", __LINE__, #x); return 1; } } while (0)

static struct air_type i32_type = { .kind = AIR_TYPE_INT, .bits = 32 };

static struct air_md *
md_text(void *ctx, const char *text)
{
   struct air_md *md = rzalloc(ctx, struct air_md);
   md->kind = AIR_MD_STRING;
   md->str = ralloc_strdup(ctx, text);
   md->len = strlen(text);
   return md;
}

static struct air_md *
md_integer(void *ctx, unsigned value)
{
   struct air_constant *constant = rzalloc(ctx, struct air_constant);
   constant->base = (struct air_value) { .kind = AIR_VALUE_CONSTANT, .type = &i32_type };
   constant->ckind = AIR_CONST_INT;
   constant->u = value;
   struct air_md *md = rzalloc(ctx, struct air_md);
   md->kind = AIR_MD_VALUE;
   md->value = &constant->base;
   return md;
}

static struct air_md *
md_node(void *ctx, struct air_md *const *ops, unsigned count)
{
   struct air_md *md = rzalloc(ctx, struct air_md);
   md->kind = AIR_MD_NODE;
   md->num_ops = count;
   md->ops = ralloc_memdup(ctx, ops, count * sizeof(*ops));
   return md;
}

#define S(x) md_text(scratch, x)
#define I(x) md_integer(scratch, x)
#define NODE(...) md_node(scratch, (struct air_md *[]){ __VA_ARGS__ }, \
                         sizeof((struct air_md *[]){ __VA_ARGS__ }) / sizeof(struct air_md *))

static int
unit_tests(void)
{
   void *scratch = ralloc_context(NULL), *result_ctx = ralloc_context(NULL);
   const struct air_type fp = { .kind = AIR_TYPE_FLOAT };
   const struct air_type fp2 = { .kind = AIR_TYPE_VECTOR, .elem = &fp, .count = 2 };
   const struct air_type points = { .kind = AIR_TYPE_ARRAY, .elem = &fp2, .count = 4 };
   const struct air_type *fields[] = { &points, &i32_type };
   struct air_type record = { .kind = AIR_TYPE_STRUCT, .name = "struct.TestParams",
      .num_members = 2, .members = fields };
   const struct air_type cp = { .kind = AIR_TYPE_POINTER, .addrspace = 2, .elem = &record };
   const struct air_type tp = { .kind = AIR_TYPE_POINTER, .addrspace = 1 };
   const struct air_type shared = { .kind = AIR_TYPE_POINTER, .addrspace = 3, .elem = &fp };
   struct air_value params[] = { { .type = &cp }, { .type = &tp }, { .type = &tp },
                                { .type = &i32_type }, { .type = &shared } };
   struct air_function function = { .num_args = 5, .args = params };
   const struct air_module module = { .num_types = 1, .types = &record,
      .datalayout = "e-p:64:64:64-i32:32:32-f32:32:32-v64:64:64" };
   struct air_md *array_count = I(4), *field_offset = I(32);
   struct air_md *members = NODE(I(0), I(8), array_count, S("float2"), S("points"),
                                field_offset, I(4), I(0), S("int"), S("count"));
   struct air_md *buffer = NODE(I(0), S("air.buffer"), S("air.location_index"), I(0), I(1),
      S("air.read"), S("air.address_space"), I(2), S("air.arg_type_size"), I(40),
      S("air.arg_type_align_size"), I(8), S("air.struct_type_info"), members,
      S("air.arg_type_name"), S("TestParams"), S("air.arg_name"), S("params"));
   struct air_md *span = I(1);
   struct air_md *texture = NODE(I(1), S("air.texture"), S("air.location_index"), I(3), span,
      S("air.sample"), S("air.arg_type_name"), S("texture2d<half, sample>"),
      S("air.arg_name"), S("optionalInput"), S("air.arg_unused"));
   struct air_md *sampler = NODE(I(2), S("air.sampler"), S("air.location_index"), I(2), I(1),
      S("air.arg_type_name"), S("sampler"), S("air.arg_name"), S("filter"));
   struct air_md *builtin = NODE(I(3), S("air.thread_index_in_threadgroup"),
      S("air.arg_type_name"), S("uint"), S("air.arg_name"), S("tid"));
   struct air_md *threadgroup = NODE(I(4), S("air.buffer"), S("air.location_index"), I(0), I(1),
      S("air.read_write"), S("air.address_space"), I(3), S("air.arg_type_size"), I(4),
      S("air.arg_type_align_size"), I(4), S("air.arg_type_name"), S("float"),
      S("air.arg_name"), S("scratchData"));
   struct air_md *inputs = NODE(buffer, texture, sampler, builtin, threadgroup);
   const char *error = NULL;
   struct rwa_compute_reflection *out =
      air_compute_reflection(result_ctx, &module, &function, inputs, &error);
   CHECK(out && !error && out->argument_count == 4);
   const struct rwa_reflection_type *type = out->arguments[0].buffer_type;
   CHECK(type && type->data_type == 1 && type->data_size == 40 && type->alignment == 8);
   CHECK(type->member_count == 2 && type->members[1].offset == 32);
   const struct rwa_reflection_type *array = type->members[0].type;
   CHECK(array->data_type == 2 && array->array_length == 4 && array->array_stride == 8);
   CHECK(array->data_size == 32 && array->element->data_type == 4);
   CHECK(out->arguments[1].type == 2 && out->arguments[1].index == 3 &&
         !out->arguments[1].active && out->arguments[1].texture_data_type == 16);
   CHECK(out->arguments[2].type == 3 && out->arguments[2].access == 0);
   CHECK(out->arguments[3].type == 1 && out->arguments[3].access == 1);
   CHECK(out->arguments[3].buffer_type->alignment == 4);

   const char *texture_types[] = { "texture2d<half, sample>", "texture2d<half, read>",
                                  "texture2d<half, write>", "texture2d<half, read_write>" };
   const char *access_keys[] = { "air.sample", "air.read", "air.write", "air.read_write" };
   const unsigned access_values[] = { 0, 0, 2, 1 };
   struct air_md *original_type = texture->ops[7], *original_access = texture->ops[5];
   struct air_md *original_unused = texture->ops[10];
   for (unsigned i = 0; i < 4; i++) {
      texture->ops[7] = S(texture_types[i]);
      for (unsigned j = 0; j < 4; j++) {
         texture->ops[5] = S(access_keys[j]);
         struct rwa_compute_reflection *candidate =
            air_compute_reflection(result_ctx, &module, &function, inputs, &error);
         if (i == j) {
            CHECK(candidate && !error && candidate->arguments[1].access == access_values[i]);
            ralloc_free(candidate);
         } else {
            CHECK(!candidate && error && strstr(error, "access"));
         }
      }
   }
   for (unsigned i = 0; i < 4; i++) {
      texture->ops[7] = S(texture_types[i]);
      texture->ops[5] = S(access_keys[i]);
      for (unsigned j = 0; j < 4; j++) {
         if (i == j)
            continue;
         texture->ops[10] = S(access_keys[j]);
         CHECK(!air_compute_reflection(result_ctx, &module, &function, inputs, &error));
         CHECK(error && strstr(error, "access"));
      }
   }
   texture->ops[10] = original_unused;
   texture->ops[7] = S("texture2d<half>");
   for (unsigned i = 0; i < 4; i++) {
      texture->ops[5] = S(access_keys[i]);
      struct rwa_compute_reflection *candidate =
         air_compute_reflection(result_ctx, &module, &function, inputs, &error);
      CHECK(candidate && !error && candidate->arguments[1].access == access_values[i]);
      ralloc_free(candidate);
   }
   texture->ops[5] = S("air.sample");
   texture->ops[10] = S("air.read");
   struct rwa_compute_reflection *candidate =
      air_compute_reflection(result_ctx, &module, &function, inputs, &error);
   CHECK(candidate && !error && candidate->arguments[1].access == 0);
   ralloc_free(candidate);
   texture->ops[5] = S("air.read_write");
   texture->ops[10] = S("air.write");
   candidate = air_compute_reflection(result_ctx, &module, &function, inputs, &error);
   CHECK(candidate && !error && candidate->arguments[1].access == 1);
   ralloc_free(candidate);
   texture->ops[10] = original_unused;
   texture->ops[7] = S("texture2d<half, read_write >");
   candidate = air_compute_reflection(result_ctx, &module, &function, inputs, &error);
   CHECK(candidate && !error && candidate->arguments[1].access == 1);
   ralloc_free(candidate);
   const char *malformed_types[] = { "texture2d<half,>", "texture2d<half, unknown>",
                                     "texture2d<half, sample, write>" };
   texture->ops[5] = original_access;
   for (unsigned i = 0; i < 3; i++) {
      texture->ops[7] = S(malformed_types[i]);
      CHECK(!air_compute_reflection(result_ctx, &module, &function, inputs, &error));
      CHECK(error && strstr(error, "unsupported type"));
   }
   texture->ops[7] = original_type;
   puts("PASS texture access: 4 matching modes, 12 contradictions, 12 mixed keys, 4 omitted suffixes, 2 omitted mixed keys, whitespace and 3 malformed suffixes");

   ((struct air_constant *)field_offset->value)->u = 36;
   CHECK(!air_compute_reflection(result_ctx, &module, &function, inputs, &error));
   CHECK(error && strstr(error, "layout"));
   ((struct air_constant *)field_offset->value)->u = 32;
   ((struct air_constant *)array_count->value)->u = 3;
   CHECK(!air_compute_reflection(result_ctx, &module, &function, inputs, &error));
   CHECK(error && strstr(error, "layout"));
   ((struct air_constant *)array_count->value)->u = 4;
   ((struct air_constant *)span->value)->u = 2;
   CHECK(!air_compute_reflection(result_ctx, &module, &function, inputs, &error));
   CHECK(error && strstr(error, "span"));
   ((struct air_constant *)span->value)->u = 1;
   struct air_md *original = buffer->ops[1];
   buffer->ops[1] = S("air.indirect_buffer");
   CHECK(!air_compute_reflection(result_ctx, &module, &function, inputs, &error));
   CHECK(error && strstr(error, "direct argument"));
   buffer->ops[1] = original;
   original = texture->ops[1];
   texture->ops[1] = S("air.unknown_resource");
   CHECK(!air_compute_reflection(result_ctx, &module, &function, inputs, &error));
   CHECK(error && strstr(error, "direct argument"));
   texture->ops[1] = original;
   original = builtin->ops[2];
   builtin->ops[2] = S("air.indirect_buffer");
   CHECK(!air_compute_reflection(result_ctx, &module, &function, inputs, &error));
   CHECK(error && strstr(error, "direct argument"));
   builtin->ops[2] = original;
   inputs->ops[4] = texture;
   CHECK(!air_compute_reflection(result_ctx, &module, &function, inputs, &error));
   CHECK(error && strstr(error, "duplicate parameter"));
   inputs->ops[4] = threadgroup;
   ralloc_free(scratch);
   CHECK(!strcmp(out->arguments[0].name, "params"));
   CHECK(!strcmp(type->members[0].name, "points"));
   CHECK(!strcmp(type->name, "TestParams"));
   ralloc_free(result_ctx);
   puts("PASS compute reflection: scalar/struct/array, inactive resource, sampler, threadgroup, lifetime and malformed metadata");
   return 0;
}

static void
print_type(const struct rwa_reflection_type *type, unsigned depth)
{
   if (!type)
      return;
   for (unsigned i = 0; i < type->member_count; i++) {
      const struct rwa_reflection_member *member = &type->members[i];
      printf("%*smember %s offset=%u dataType=%u size=%u array=%u stride=%u\n",
             depth * 2, "", member->name, member->offset, member->type->data_type,
             member->type->data_size, member->type->array_length, member->type->array_stride);
      print_type(member->type, depth + 1);
   }
   if (type->element)
      print_type(type->element, depth + 1);
}

static int
module_test(const char *path, const char *entry)
{
   FILE *file = fopen(path, "rb");
   CHECK(file && fseek(file, 0, SEEK_END) == 0);
   long size = ftell(file);
   CHECK(size > 0 && fseek(file, 0, SEEK_SET) == 0);
   void *ctx = ralloc_context(NULL);
   void *bytes = ralloc_size(ctx, size);
   CHECK(bytes && fread(bytes, 1, size, file) == (size_t)size);
   fclose(file);
   const void *bitcode = bytes;
   size_t bitcode_size = size;
   const char *error = NULL;
   if (size >= 4 && !memcmp(bytes, "MTLB", 4)) {
      struct air_metallib library;
      CHECK(air_metallib_parse(ctx, bytes, size, &library, &error));
      bitcode = NULL;
      for (unsigned i = 0; i < library.num_functions; i++)
         if (!strcmp(library.functions[i].name, entry)) {
            bitcode = library.functions[i].bitcode;
            bitcode_size = library.functions[i].bitcode_size;
         }
      CHECK(bitcode);
   }
   struct air_module *module = air_module_read(ctx, bitcode, bitcode_size, &error);
   CHECK(module);
   const struct air_md *inputs = NULL;
   const struct air_function *function = air_module_find_function(module, entry);
   for (unsigned i = 0; i < module->num_named_md; i++) {
      const struct air_named_md *named = &module->named_md[i];
      if (strcmp(named->name, "air.kernel"))
         continue;
      for (unsigned j = 0; j < named->num_ops; j++) {
         const struct air_md *md = named->ops[j];
         if (md && md->num_ops > 2 && md->ops[0] &&
             md->ops[0]->kind == AIR_MD_VALUE &&
             md->ops[0]->value == (const struct air_value *)function)
            inputs = md->ops[2];
      }
   }
   void *result_ctx = ralloc_context(NULL);
   struct rwa_compute_reflection *out =
      air_compute_reflection(result_ctx, module, function, inputs, &error);
   if (!out) {
      fprintf(stderr, "%s\n", error ? error : "reflection unavailable");
      ralloc_free(ctx);
      ralloc_free(result_ctx);
      return 1;
   }
   ralloc_free(ctx);
   printf("%s arguments=%u\n", entry, out->argument_count);
   for (unsigned i = 0; i < out->argument_count; i++) {
      const struct rwa_reflection_argument *a = &out->arguments[i];
      printf("argument %s type=%u index=%u access=%u active=%u textureType=%u textureDataType=%u size=%u alignment=%u\n",
             a->name, a->type, a->index, a->access, a->active, a->texture_type,
             a->texture_data_type, a->buffer_type ? a->buffer_type->data_size : 0,
             a->buffer_type ? a->buffer_type->alignment : 0);
      print_type(a->buffer_type, 1);
   }
   ralloc_free(result_ctx);
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc == 1)
      return unit_tests();
   if (argc == 3)
      return module_test(argv[1], argv[2]);
   fprintf(stderr, "usage: air_reflection_test [module-or-metallib entry]\n");
   return 2;
}
