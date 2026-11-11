/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

#include "air_reflection.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util/macros.h"
#include "util/ralloc.h"

struct reflection_ctx {
   void *mem_ctx;
   const struct air_module *module;
   const char *error;
};

static bool
reflection_error(struct reflection_ctx *c, const char *fmt, ...)
{
   if (!c->error) {
      va_list ap;
      va_start(ap, fmt);
      c->error = ralloc_vasprintf(c->mem_ctx, fmt, ap);
      va_end(ap);
   }
   return false;
}

static const char *
md_string(const struct air_md *md, unsigned index)
{
   return md && md->kind == AIR_MD_NODE && index < md->num_ops
             ? air_md_string(md->ops[index]) : NULL;
}

static bool
md_uint(const struct air_md *md, unsigned index, unsigned *out)
{
   uint64_t value;
   if (!md || md->kind != AIR_MD_NODE || index >= md->num_ops ||
       !md->ops[index] || md->ops[index]->kind != AIR_MD_VALUE ||
       !air_const_uint(md->ops[index]->value, &value) || value > UINT_MAX)
      return false;
   *out = value;
   return true;
}

static int
md_key(const struct air_md *md, const char *key)
{
   for (unsigned i = 0; i < (md ? md->num_ops : 0); i++) {
      const char *s = md_string(md, i);
      if (s && !strcmp(s, key))
         return i;
   }
   return -1;
}

static const char *
md_key_string(const struct air_md *md, const char *key)
{
   int i = md_key(md, key);
   return i >= 0 ? md_string(md, i + 1) : NULL;
}

static bool
md_key_uint(const struct air_md *md, const char *key, unsigned *out)
{
   int i = md_key(md, key);
   return i >= 0 && md_uint(md, i + 1, out);
}

static const struct air_md *
md_key_node(const struct air_md *md, const char *key)
{
   int i = md_key(md, key);
   return i >= 0 && (unsigned)i + 1 < md->num_ops && md->ops[i + 1] &&
          md->ops[i + 1]->kind == AIR_MD_NODE ? md->ops[i + 1] : NULL;
}

/* Integer signedness is present in air.arg_type_name, not in LLVM's iN type.
 * These enum values are MTLDataType's scalar/vector and matrix values.
 */
static unsigned
scalar_type(const char *name, struct air_type *scalar, unsigned *rows, unsigned *cols)
{
   static const struct {
      const char *name;
      unsigned value;
      enum air_type_kind kind;
      unsigned bits;
   } types[] = {
      { "float", 3, AIR_TYPE_FLOAT, 32 },
      { "half", 16, AIR_TYPE_HALF, 16 },
      { "int", 29, AIR_TYPE_INT, 32 },
      { "uint", 33, AIR_TYPE_INT, 32 },
      { "short", 37, AIR_TYPE_INT, 16 },
      { "ushort", 41, AIR_TYPE_INT, 16 },
      { "char", 45, AIR_TYPE_INT, 8 },
      { "uchar", 49, AIR_TYPE_INT, 8 },
      { "bool", 53, AIR_TYPE_INT, 1 },
      { "long", 81, AIR_TYPE_INT, 64 },
      { "ulong", 85, AIR_TYPE_INT, 64 },
   };
   *rows = *cols = 1;
   if (!name)
      return 0;
   for (unsigned i = 0; i < ARRAY_SIZE(types); i++) {
      size_t n = strlen(types[i].name);
      if (strncmp(name, types[i].name, n))
         continue;
      const char *suffix = name + n;
      unsigned value = types[i].value;
      if (*suffix) {
         if (suffix[0] < '2' || suffix[0] > '4')
            continue;
         *rows = suffix[0] - '0';
         if (suffix[1] == 'x' && suffix[2] >= '2' && suffix[2] <= '4' &&
             !suffix[3] && (value == 3 || value == 16)) {
            *cols = *rows;
            *rows = suffix[2] - '0';
            value += 4 + (*cols - 2) * 3 + (*rows - 2);
         } else if (!suffix[1]) {
            value += *rows - 1;
         } else {
            continue;
         }
      }
      *scalar = (struct air_type) { .kind = types[i].kind, .bits = types[i].bits };
      return value;
   }
   return 0;
}

static bool
scalar_matches(const struct air_type *actual, const struct air_type *scalar,
               unsigned rows, unsigned cols)
{
   if (!actual)
      return true;
   if (cols > 1) {
      if (actual->kind == AIR_TYPE_ARRAY && actual->count == cols)
         return scalar_matches(actual->elem, scalar, rows, 1);
      if (actual->kind != AIR_TYPE_STRUCT || actual->num_members != cols)
         return false;
      for (unsigned i = 0; i < cols; i++)
         if (!scalar_matches(actual->members[i], scalar, rows, 1))
            return false;
      return true;
   }
   if (rows > 1) {
      if (actual->kind != AIR_TYPE_VECTOR || actual->count != rows)
         return false;
      actual = actual->elem;
   }
   return actual && actual->kind == scalar->kind &&
          (scalar->kind != AIR_TYPE_INT || actual->bits == scalar->bits ||
           (scalar->bits == 1 && actual->bits == 8));
}

static const struct air_type *
named_struct(struct reflection_ctx *c, const char *name)
{
   const struct air_type *found = NULL;
   for (unsigned i = 0; i < c->module->num_types; i++) {
      const struct air_type *t = &c->module->types[i];
      if (t->kind != AIR_TYPE_STRUCT || t->opaque || !t->name)
         continue;
      const char *short_name = !strncmp(t->name, "struct.", 7) ? t->name + 7 :
                               !strncmp(t->name, "class.", 6) ? t->name + 6 : t->name;
      if (strcmp(short_name, name) && strcmp(t->name, name))
         continue;
      if (found) {
         reflection_error(c, "reflection: struct %s has ambiguous LLVM types", name);
         return NULL;
      }
      found = t;
   }
   return found;
}

static struct rwa_reflection_type *
reflect_type(struct reflection_ctx *c, const struct air_type *actual, const char *name,
             const struct air_md *members, unsigned size, unsigned alignment,
             unsigned depth);

static struct rwa_reflection_type *
reflect_struct(struct reflection_ctx *c, const struct air_type *actual, const char *name,
               const struct air_md *members, unsigned size, unsigned alignment,
               unsigned depth)
{
   if (!actual)
      actual = named_struct(c, name);
   if (!actual || actual->kind != AIR_TYPE_STRUCT || actual->opaque ||
       air_type_alloc_size(c->module, actual) != size ||
       air_type_abi_align(c->module, actual) != alignment ||
       depth > c->module->num_types) {
      reflection_error(c, "reflection: struct %s has no complete matching LLVM layout", name);
      return NULL;
   }
   struct rwa_reflection_type *out = rzalloc(c->mem_ctx, struct rwa_reflection_type);
   struct rwa_reflection_member *fields =
      rzalloc_array(c->mem_ctx, struct rwa_reflection_member, actual->num_members);
   *out = (struct rwa_reflection_type) {
      .data_type = 1, .data_size = size, .alignment = alignment,
      .name = ralloc_strdup(c->mem_ctx, name), .members = fields,
      .member_count = actual->num_members,
   };
   unsigned cursor = 0;
   for (unsigned i = 0; i < actual->num_members; i++) {
      const struct air_md *nested = NULL;
      const char *key = md_string(members, cursor);
      if (key && !strcmp(key, "air.struct_type_info")) {
         if (cursor + 1 >= members->num_ops || !members->ops[cursor + 1] ||
             members->ops[cursor + 1]->kind != AIR_MD_NODE)
            goto malformed;
         nested = members->ops[cursor + 1];
         cursor += 2;
      }
      unsigned offset, stride, count;
      const char *type_name = md_string(members, cursor + 3);
      const char *field_name = md_string(members, cursor + 4);
      if (!md_uint(members, cursor, &offset) || !md_uint(members, cursor + 1, &stride) ||
          !md_uint(members, cursor + 2, &count) || !type_name || !field_name || !*field_name)
         goto malformed;
      cursor += 5;
      const struct air_type *field_type = actual->members[i];
      const struct air_type *element = field_type;
      if (count) {
         if (field_type->kind != AIR_TYPE_ARRAY || field_type->count != count)
            goto malformed;
         element = field_type->elem;
      }
      if (air_struct_member_offset(c->module, actual, i) != offset ||
          air_type_alloc_size(c->module, element) != stride)
         goto malformed;
      for (unsigned j = 0; j < i; j++)
         if (!strcmp(fields[j].name, field_name))
            goto malformed;
      struct rwa_reflection_type *type =
         reflect_type(c, element, type_name, nested, stride,
                      air_type_abi_align(c->module, element), depth + 1);
      if (!type)
         return NULL;
      if (count) {
         struct rwa_reflection_type *array = rzalloc(c->mem_ctx, struct rwa_reflection_type);
         *array = (struct rwa_reflection_type) {
            .data_type = 2, .data_size = air_type_alloc_size(c->module, field_type),
            .alignment = air_type_abi_align(c->module, field_type),
            .array_length = count, .array_stride = stride, .element = type,
         };
         type = array;
      }
      fields[i] = (struct rwa_reflection_member) {
         .name = ralloc_strdup(c->mem_ctx, field_name), .offset = offset, .type = type,
      };
      continue;
malformed:
      reflection_error(c, "reflection: member %u of struct %s has incomplete or mismatched layout",
                       i, name);
      return NULL;
   }
   if (!members || cursor != members->num_ops) {
      reflection_error(c, "reflection: struct %s has unconsumed member metadata", name);
      return NULL;
   }
   return out;
}

static struct rwa_reflection_type *
reflect_type(struct reflection_ctx *c, const struct air_type *actual, const char *name,
             const struct air_md *members, unsigned size, unsigned alignment,
             unsigned depth)
{
   if (!name || !alignment) {
      reflection_error(c, "reflection: a buffer type has no name or alignment");
      return NULL;
   }
   if (members)
      return reflect_struct(c, actual, name, members, size, alignment, depth);
   struct air_type scalar, vector, matrix;
   unsigned rows, cols;
   unsigned data_type = scalar_type(name, &scalar, &rows, &cols);
   if (!data_type || !scalar_matches(actual, &scalar, rows, cols)) {
      reflection_error(c, "reflection: type %s has no complete scalar or struct description", name);
      return NULL;
   }
   const struct air_type *expected = &scalar;
   if (rows > 1) {
      vector = (struct air_type) { .kind = AIR_TYPE_VECTOR, .elem = &scalar, .count = rows };
      expected = &vector;
   }
   if (cols > 1) {
      matrix = (struct air_type) { .kind = AIR_TYPE_ARRAY, .elem = expected, .count = cols };
      expected = &matrix;
   }
   if (air_type_alloc_size(c->module, actual ? actual : expected) != size ||
       air_type_abi_align(c->module, actual ? actual : expected) != alignment) {
      reflection_error(c, "reflection: type %s has mismatched size or alignment", name);
      return NULL;
   }
   struct rwa_reflection_type *out = rzalloc(c->mem_ctx, struct rwa_reflection_type);
   *out = (struct rwa_reflection_type) {
      .data_type = data_type, .data_size = size, .alignment = alignment,
      .name = ralloc_strdup(c->mem_ctx, name),
   };
   return out;
}

static bool
reflect_texture(struct reflection_ctx *c, const char *name, const struct air_md *node,
                struct rwa_reflection_argument *out)
{
   static const struct { const char *name; unsigned type; bool depth; } kinds[] = {
      { "texture1d", 0, false }, { "texture1d_array", 1, false },
      { "texture2d", 2, false }, { "texture2d_array", 3, false },
      { "texture2d_ms", 4, false }, { "texturecube", 5, false },
      { "texturecube_array", 6, false }, { "texture3d", 7, false },
      { "texture2d_ms_array", 8, false }, { "texture_buffer", 9, false },
      { "depth2d", 2, true }, { "depth2d_array", 3, true },
      { "depth2d_ms", 4, true }, { "depthcube", 5, true },
      { "depthcube_array", 6, true }, { "depth2d_ms_array", 8, true },
   };
   for (unsigned i = 0; name && i < ARRAY_SIZE(kinds); i++) {
      size_t n = strlen(kinds[i].name);
      if (strncmp(name, kinds[i].name, n) || name[n] != '<')
         continue;
      const char *element = name + n + 1, *end = strchr(element, '>');
      const char *comma = strchr(element, ',');
      if (!end || end[1] || (comma && comma > end))
         break;
      size_t len = (comma ? comma : end) - element;
      while (len && element[len - 1] == ' ')
         len--;
      unsigned data_type = len == 5 && !strncmp(element, "float", len) ? 3 :
                           len == 4 && !strncmp(element, "half", len) ? 16 :
                           len == 3 && !strncmp(element, "int", len) ? 29 :
                           len == 4 && !strncmp(element, "uint", len) ? 33 : 0;
      if (!data_type || (kinds[i].depth && data_type != 3))
         break;
      if (comma) {
         const char *access = comma + 1, *access_end = end;
         while (access < access_end && *access == ' ')
            access++;
         while (access_end > access && access_end[-1] == ' ')
            access_end--;
         size_t access_len = access_end - access;
         const char *access_key = access_len == 6 && !strncmp(access, "sample", access_len) ? "air.sample" :
                                  access_len == 4 && !strncmp(access, "read", access_len) ? "air.read" :
                                  access_len == 5 && !strncmp(access, "write", access_len) ? "air.write" :
                                  access_len == 10 && !strncmp(access, "read_write", access_len) ? "air.read_write" : NULL;
         if (!access_key)
            break;
         /* Sample and read share MTLBindingAccessReadOnly; AIR declares distinct keys. */
         static const char *const keys[] = { "air.sample", "air.read", "air.write", "air.read_write" };
         for (unsigned k = 0; k < ARRAY_SIZE(keys); k++) {
            if ((md_key(node, keys[k]) >= 0) != !strcmp(access_key, keys[k]))
               return reflection_error(c, "reflection: texture %s has mismatched access in type %s",
                                       out->name, name);
         }
      }
      out->texture_type = kinds[i].type;
      out->texture_data_type = data_type;
      out->depth_texture = kinds[i].depth;
      return true;
   }
   return reflection_error(c, "reflection: texture %s has unsupported type %s",
                           out->name, name ? name : "(missing)");
}

static bool
is_builtin(const struct air_md *node)
{
   static const char *const keys[] = {
      "air.thread_position_in_grid", "air.thread_position_in_threadgroup",
      "air.threadgroup_position_in_grid", "air.threads_per_threadgroup",
      "air.threadgroups_per_grid", "air.thread_index_in_threadgroup",
      "air.threads_per_grid", "air.thread_index_in_simdgroup",
      "air.simdgroup_index_in_threadgroup", "air.threads_per_simdgroup",
      "air.simdgroups_per_threadgroup",
   };
   for (unsigned i = 0; i < ARRAY_SIZE(keys); i++)
      if (md_key(node, keys[i]) >= 0)
         return true;
   return false;
}

struct rwa_compute_reflection *
air_compute_reflection(void *mem_ctx, const struct air_module *module,
                       const struct air_function *function,
                       const struct air_md *inputs, const char **error)
{
   struct rwa_compute_reflection *out = rzalloc(mem_ctx, struct rwa_compute_reflection);
   struct reflection_ctx c = { .mem_ctx = out, .module = module };
   if (error)
      *error = NULL;
   if (!module || !function || (function->num_args && !inputs) ||
       (inputs && (inputs->kind != AIR_MD_NODE || inputs->num_ops != function->num_args))) {
      reflection_error(&c, "reflection: compute inputs are incomplete");
      goto fail;
   }
   struct rwa_reflection_argument *args =
      rzalloc_array(out, struct rwa_reflection_argument, function->num_args);
   bool *seen = rzalloc_array(out, bool, function->num_args);
   out->arguments = args;
   for (unsigned i = 0; i < (inputs ? inputs->num_ops : 0); i++) {
      const struct air_md *node = inputs->ops[i];
      unsigned param;
      if (!md_uint(node, 0, &param) || param >= function->num_args || seen[param]) {
         reflection_error(&c, "reflection: input %u has an invalid or duplicate parameter", i);
         goto fail;
      }
      seen[param] = true;
      bool buffer = md_key(node, "air.buffer") >= 0;
      bool texture = md_key(node, "air.texture") >= 0;
      bool sampler = md_key(node, "air.sampler") >= 0;
      if (md_key(node, "air.indirect_buffer") >= 0 ||
          md_key(node, "air.indirect_argument") >= 0) {
         reflection_error(&c, "reflection: parameter %u has no supported direct argument declaration",
                          param);
         goto fail;
      }
      if (is_builtin(node)) {
         if (!buffer && !texture && !sampler)
            continue;
         reflection_error(&c, "reflection: parameter %u declares a builtin and a resource", param);
         goto fail;
      }
      if (buffer + texture + sampler != 1) {
         reflection_error(&c, "reflection: parameter %u has no supported direct argument declaration",
                          param);
         goto fail;
      }
      const char *name = md_key_string(node, "air.arg_name");
      const char *type_name = md_key_string(node, "air.arg_type_name");
      int location = md_key(node, "air.location_index");
      struct rwa_reflection_argument arg = {
         .type = buffer ? 0 : texture ? 2 : 3,
         .active = md_key(node, "air.arg_unused") < 0,
      };
      if (!name || !*name || !type_name || location < 0 ||
          !md_uint(node, location + 1, &arg.index) ||
          !md_uint(node, location + 2, &arg.array_length) || arg.array_length != 1) {
         reflection_error(&c, "reflection: parameter %u has incomplete name, type or location span",
                          param);
         goto fail;
      }
      arg.name = ralloc_strdup(out, name);
      bool read = md_key(node, "air.read") >= 0 || md_key(node, "air.sample") >= 0 ||
                  md_key(node, "air.read_write") >= 0 || sampler;
      bool write = md_key(node, "air.write") >= 0 || md_key(node, "air.read_write") >= 0;
      if (!read && !write) {
         reflection_error(&c, "reflection: argument %s has no access declaration", name);
         goto fail;
      }
      arg.access = read && write ? 1 : write ? 2 : 0;
      if (texture && !reflect_texture(&c, type_name, node, &arg))
         goto fail;
      if (sampler && (strcmp(type_name, "sampler") || write)) {
         reflection_error(&c, "reflection: sampler %s has unsupported type %s", name, type_name);
         goto fail;
      }
      if (buffer) {
         unsigned addrspace, size, alignment;
         const struct air_type *pointer = function->args[param].type;
         if (!md_key_uint(node, "air.address_space", &addrspace) ||
             addrspace < 1 || addrspace > 3 || !pointer ||
             pointer->kind != AIR_TYPE_POINTER || pointer->addrspace != addrspace ||
             !md_key_uint(node, "air.arg_type_size", &size) ||
             !md_key_uint(node, "air.arg_type_align_size", &alignment)) {
            reflection_error(&c, "reflection: buffer %s has incomplete type size or address space", name);
            goto fail;
         }
         if (alignment > UINT16_MAX || (addrspace != 3 && size == UINT32_MAX)) {
            reflection_error(&c, "reflection: buffer %s size or alignment exceeds the native representation", name);
            goto fail;
         }
         arg.type = addrspace == 3 ? 1 : 0;
         arg.constant_buffer = addrspace == 2;
         const struct air_md *members = md_key_node(node, "air.struct_type_info");
         if (md_key(node, "air.struct_type_info") >= 0 && !members) {
            reflection_error(&c, "reflection: buffer %s has malformed struct metadata", name);
            goto fail;
         }
         arg.buffer_type = reflect_type(&c, pointer->elem, type_name, members,
                                       size, alignment, 0);
         if (!arg.buffer_type)
            goto fail;
      }
      for (unsigned j = 0; j < out->argument_count; j++) {
         if (args[j].type == arg.type && args[j].index == arg.index) {
            reflection_error(&c, "reflection: arguments %s and %s share one binding index",
                             args[j].name, arg.name);
            goto fail;
         }
      }
      args[out->argument_count++] = arg;
   }
   ralloc_free(seen);
   return out;
fail:
   if (error)
      *error = ralloc_strdup(mem_ctx, c.error ? c.error : "reflection: out of memory");
   ralloc_free(out);
   return NULL;
}
