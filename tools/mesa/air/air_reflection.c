/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

#include "air_reflection.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
scalar_type(const char *name, struct air_type *scalar, unsigned *rows, unsigned *cols,
            bool *packed)
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
   if (packed)
      *packed = false;
   if (!name)
      return 0;
   bool is_packed = false;
   if (!strncmp(name, "packed_", 7)) {
      is_packed = true;
      name += 7;
   }
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
             !suffix[3] && !is_packed && (value == 3 || value == 16)) {
            *cols = *rows;
            *rows = suffix[2] - '0';
            value += 4 + (*cols - 2) * 3 + (*rows - 2);
         } else if (!suffix[1]) {
            value += *rows - 1;
         } else {
            continue;
         }
      } else if (is_packed) {
         continue;
      }
      if (packed)
         *packed = is_packed;
      *scalar = (struct air_type) { .kind = types[i].kind, .bits = types[i].bits };
      return value;
   }
   return 0;
}

static bool
scalar_matches(const struct air_type *actual, const struct air_type *scalar,
               unsigned rows, unsigned cols, bool packed)
{
   if (!actual)
      return true;
   if (cols > 1) {
      if (actual->kind == AIR_TYPE_STRUCT && actual->num_members == 1 &&
          actual->members[0]->kind == AIR_TYPE_ARRAY && actual->members[0]->count == cols)
         return scalar_matches(actual->members[0]->elem, scalar, rows, 1, false);
      if (actual->kind == AIR_TYPE_ARRAY && actual->count == cols)
         return scalar_matches(actual->elem, scalar, rows, 1, false);
      if (actual->kind != AIR_TYPE_STRUCT || actual->num_members != cols)
         return false;
      for (unsigned i = 0; i < cols; i++)
         if (!scalar_matches(actual->members[i], scalar, rows, 1, false))
            return false;
      return true;
   }
   if (rows > 1) {
      if (packed && actual->kind == AIR_TYPE_ARRAY && actual->count == rows) {
         actual = actual->elem;
      } else if (actual->kind == AIR_TYPE_VECTOR && actual->count == rows) {
         actual = actual->elem;
      } else {
         return false;
      }
   }
   return actual && actual->kind == scalar->kind &&
          (scalar->kind != AIR_TYPE_INT || actual->bits == scalar->bits ||
           (scalar->bits == 1 && actual->bits == 8));
}

static struct rwa_reflection_type *
reflect_struct_from_md(struct reflection_ctx *c, const char *name,
                       const struct air_md *members, unsigned size,
                       unsigned alignment, unsigned base_id, unsigned depth);

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
      return reflect_struct_from_md(c, name, members, size, alignment, 0, depth);
   struct air_type scalar, vector, matrix;
   unsigned rows, cols;
   bool packed = false;
   unsigned data_type = scalar_type(name, &scalar, &rows, &cols, &packed);
   if (!data_type || !scalar_matches(actual, &scalar, rows, cols, packed)) {
      reflection_error(c, "reflection: type %s has no complete scalar or struct description", name);
      return NULL;
   }
   if (packed) {
      unsigned elem_bytes = scalar.bits == 1 ? 1 : scalar.bits / 8;
      if (size != rows * elem_bytes || alignment != elem_bytes) {
         reflection_error(c, "reflection: type %s has mismatched size or alignment", name);
         return NULL;
      }
   } else {
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
   }
   struct rwa_reflection_type *out = rzalloc(c->mem_ctx, struct rwa_reflection_type);
   *out = (struct rwa_reflection_type) {
      .data_type = data_type, .data_size = size, .alignment = alignment,
      .name = ralloc_strdup(c->mem_ctx, name),
   };
   return out;
}

static bool
parse_texture_type_name(const char *name, unsigned expected_array_len, bool strict_array,
                        const struct air_md *node, unsigned *texture_type,
                        unsigned *texture_data_type, unsigned *depth_texture,
                        unsigned *access, const char **err_reason)
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
   char unwrapped[96];
   if (name && !strncmp(name, "array<", 6)) {
      const char *inner = name + 6;
      const char *inner_end = strchr(inner, '>');
      if (!inner_end || inner_end[1] != ',') {
         if (err_reason) *err_reason = "malformed array type";
         return false;
      }
      const char *len_str = inner_end + 2;
      while (*len_str == ' ')
         len_str++;
      char *after = NULL;
      unsigned long parsed_len = strtoul(len_str, &after, 10);
      if (!after || strcmp(after, ">") ||
          (strict_array && parsed_len != expected_array_len) ||
          (size_t)(inner_end + 1 - inner) >= sizeof(unwrapped)) {
         if (err_reason) *err_reason = "array length mismatch";
         return false;
      }
      memcpy(unwrapped, inner, (size_t)(inner_end + 1 - inner));
      unwrapped[inner_end + 1 - inner] = '\0';
      name = unwrapped;
   } else if (strict_array && expected_array_len != 1) {
      if (err_reason) *err_reason = "missing array<> wrapper";
      return false;
   }
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
                           len == 4 && !strncmp(element, "uint", len) ? 33 :
                           len == 5 && !strncmp(element, "short", len) ? 37 :
                           len == 6 && !strncmp(element, "ushort", len) ? 41 :
                           len == 4 && !strncmp(element, "char", len) ? 45 :
                           len == 5 && !strncmp(element, "uchar", len) ? 49 : 0;
      if (!data_type || (kinds[i].depth && data_type != 3))
         break;
      unsigned parsed_access = 0;
      if (comma) {
         const char *acc = comma + 1, *acc_end = end;
         while (acc < acc_end && *acc == ' ')
            acc++;
         while (acc_end > acc && acc_end[-1] == ' ')
            acc_end--;
         size_t acc_len = acc_end - acc;
         const char *access_key = acc_len == 6 && !strncmp(acc, "sample", acc_len) ? "air.sample" :
                                  acc_len == 4 && !strncmp(acc, "read", acc_len) ? "air.read" :
                                  acc_len == 5 && !strncmp(acc, "write", acc_len) ? "air.write" :
                                  acc_len == 10 && !strncmp(acc, "read_write", acc_len) ? "air.read_write" : NULL;
         if (!access_key)
            break;
         parsed_access = !strcmp(access_key, "air.read_write") ? 1 :
                         !strcmp(access_key, "air.write") ? 2 : 0;
         if (strict_array && node) {
            /* Sample and read share MTLBindingAccessReadOnly; AIR declares distinct keys. */
            static const char *const keys[] = { "air.sample", "air.read", "air.write", "air.read_write" };
            for (unsigned k = 0; k < ARRAY_SIZE(keys); k++) {
               if ((md_key(node, keys[k]) >= 0) != !strcmp(access_key, keys[k])) {
                  if (err_reason) *err_reason = "mismatched access";
                  return false;
               }
            }
         }
      } else if (node) {
         bool r = md_key(node, "air.read") >= 0 || md_key(node, "air.sample") >= 0 ||
                  md_key(node, "air.read_write") >= 0;
         bool w = md_key(node, "air.write") >= 0 || md_key(node, "air.read_write") >= 0;
         parsed_access = (r && w) ? 1 : w ? 2 : 0;
      }
      if (texture_type) *texture_type = kinds[i].type;
      if (texture_data_type) *texture_data_type = data_type;
      if (depth_texture) *depth_texture = kinds[i].depth ? 1 : 0;
      if (access) *access = parsed_access;
      return true;
   }
   if (err_reason) *err_reason = "unsupported texture type";
   return false;
}

static bool
reflect_texture(struct reflection_ctx *c, const char *name, const struct air_md *node,
                struct rwa_reflection_argument *out)
{
   const char *reason = NULL;
   if (!parse_texture_type_name(name, out->array_length, true, node,
                                &out->texture_type, &out->texture_data_type,
                                &out->depth_texture, NULL, &reason)) {
      return reflection_error(c, "reflection: texture %s (%s): %s",
                              out->name, reason ? reason : "invalid", name ? name : "(missing)");
   }
   return true;
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
      "air.simdgroups_per_threadgroup", "air.imageblock",
      "air.thread_index_in_quadgroup", "air.quadgroup_index_in_threadgroup",
      "air.quadgroups_per_threadgroup",
      "air.vertex_input", "air.fragment_input", "air.vertex_id",
      "air.instance_id", "air.base_vertex", "air.base_instance",
      "air.amplification_id", "air.position", "air.front_facing",
      "air.point_coord", "air.sample_id", "air.sample_mask_in",
      "air.sample_mask", "air.color", "air.barycentric_coord",
      "air.primitive_id", "air.render_target_array_index",
      "air.viewport_array_index",
      "air.render_target", "air.stage_in", "air.patch_control_point_input",
      "air.depth", "air.stencil", "air.amplification_count",
      "air.threads_per_patch", "air.position_in_patch", "air.patch_id",
      "air.mesh_grid_properties", "air.object_payload",
   };
   for (unsigned i = 0; i < ARRAY_SIZE(keys); i++)
      if (md_key(node, keys[i]) >= 0)
         return true;
   return false;
}

static struct rwa_reflection_type *
reflect_struct_from_md(struct reflection_ctx *c, const char *name,
                       const struct air_md *members, unsigned size,
                       unsigned alignment, unsigned base_id, unsigned depth)
{
   if (depth > 32) {
      reflection_error(c, "reflection: struct %s nested too deeply",
                       name ? name : "(unnamed)");
      return NULL;
   }
   struct rwa_reflection_type *out = rzalloc(c->mem_ctx, struct rwa_reflection_type);
   *out = (struct rwa_reflection_type) {
      .data_type = 1, .data_size = size, .alignment = alignment ? alignment : 1,
      .name = ralloc_strdup(c->mem_ctx, name ? name : "Struct"),
   };
   if (!members || members->kind != AIR_MD_NODE || !members->num_ops)
      return out;

   unsigned cap = members->num_ops / 5 + 1;
   struct rwa_reflection_member *fields =
      rzalloc_array(c->mem_ctx, struct rwa_reflection_member, cap);
   unsigned count = 0;
   unsigned max_end = size;
   unsigned max_align = alignment;
   unsigned cursor = 0;
   while (cursor < members->num_ops) {
      const struct air_md *nested = NULL;
      const char *key = md_string(members, cursor);
      if (key && !strcmp(key, "air.struct_type_info")) {
         if (cursor + 1 >= members->num_ops || !members->ops[cursor + 1] ||
             members->ops[cursor + 1]->kind != AIR_MD_NODE)
            goto malformed;
         nested = members->ops[cursor + 1];
         cursor += 2;
      }
      unsigned offset = 0, elem_size = 0, elem_count = 0;
      const char *type_name = md_string(members, cursor + 3);
      const char *field_name = md_string(members, cursor + 4);
      if (!md_uint(members, cursor, &offset) ||
          !md_uint(members, cursor + 1, &elem_size) ||
          !md_uint(members, cursor + 2, &elem_count) ||
          !type_name || !field_name || !*field_name)
         goto malformed;
      cursor += 5;

      for (unsigned j = 0; j < count; j++)
         if (!strcmp(fields[j].name, field_name))
            goto malformed;

      const struct air_md *desc = NULL;
      unsigned ind_idx = 0;
      unsigned child_base_id = base_id;
      const char *next_key = md_string(members, cursor);
      if (next_key && !strcmp(next_key, "air.indirect_argument")) {
         if (cursor + 1 >= members->num_ops || !members->ops[cursor + 1])
            goto malformed;
         if (members->ops[cursor + 1]->kind == AIR_MD_NODE) {
            desc = members->ops[cursor + 1];
            int mli = md_key(desc, "air.location_index");
            unsigned rel_id = 0;
            if (mli >= 0 && md_uint(desc, mli + 1, &rel_id))
               ind_idx = base_id + rel_id;
         } else {
            unsigned sub_id = 0;
            if (!md_uint(members, cursor + 1, &sub_id))
               goto malformed;
            child_base_id = base_id + sub_id;
            ind_idx = child_base_id;
         }
         cursor += 2;
      }

      struct rwa_reflection_type *mtype = NULL;
      if (desc && md_key(desc, "air.texture") >= 0) {
         unsigned ttype = 2, tdtype = 3, tdepth = 0, taccess = 0;
         if (!parse_texture_type_name(type_name, elem_count ? elem_count : 1, false, desc,
                                      &ttype, &tdtype, &tdepth, &taccess, NULL)) {
            bool r = md_key(desc, "air.read") >= 0 || md_key(desc, "air.sample") >= 0 ||
                     md_key(desc, "air.read_write") >= 0;
            bool w = md_key(desc, "air.write") >= 0 || md_key(desc, "air.read_write") >= 0;
            taccess = (r && w) ? 1 : w ? 2 : 0;
         }
         mtype = rzalloc(c->mem_ctx, struct rwa_reflection_type);
         *mtype = (struct rwa_reflection_type) {
            .data_type = 58, .data_size = elem_size ? elem_size : 8, .alignment = 8,
            .name = ralloc_strdup(c->mem_ctx, type_name),
            .texture_type = ttype, .texture_data_type = tdtype,
            .depth_texture = tdepth, .access = taccess,
         };
      } else if (desc && md_key(desc, "air.sampler") >= 0) {
         mtype = rzalloc(c->mem_ctx, struct rwa_reflection_type);
         *mtype = (struct rwa_reflection_type) {
            .data_type = 59, .data_size = elem_size ? elem_size : 8, .alignment = 8,
            .name = ralloc_strdup(c->mem_ctx, type_name),
         };
      } else if (desc && md_key(desc, "air.buffer") >= 0) {
         bool r = md_key(desc, "air.read") >= 0 || md_key(desc, "air.read_write") >= 0;
         bool w = md_key(desc, "air.write") >= 0 || md_key(desc, "air.read_write") >= 0;
         unsigned paccess = (r && w) ? 1 : w ? 2 : 0;
         unsigned addrspace = 1;
         md_key_uint(desc, "air.address_space", &addrspace);
         unsigned ptr_size = 0, ptr_align = 0;
         if (!md_key_uint(desc, "air.arg_type_size", &ptr_size))
            md_key_uint(desc, "air.buffer_size", &ptr_size);
         md_key_uint(desc, "air.arg_type_align_size", &ptr_align);
         const char *ptr_type_name = md_key_string(desc, "air.arg_type_name");
         if (!ptr_type_name)
            ptr_type_name = type_name;
         const struct air_md *ptr_sti = md_key_node(desc, "air.struct_type_info");
         struct rwa_reflection_type *elem = NULL;
         if (ptr_sti) {
            elem = reflect_struct_from_md(c, ptr_type_name, ptr_sti, ptr_size,
                                          ptr_align, 0, depth + 1);
            if (!elem)
               return NULL;
         } else {
            struct air_type pscalar;
            unsigned prows = 1, pcols = 1;
            bool ppacked = false;
            unsigned pdt = scalar_type(ptr_type_name, &pscalar, &prows, &pcols, &ppacked);
            unsigned palign = ptr_align;
            if (!palign) {
               if (pdt) {
                  unsigned eb = pscalar.bits == 1 ? 1 : pscalar.bits / 8;
                  palign = ppacked ? eb : (prows == 3 ? 4 * eb : prows * eb);
               } else {
                  palign = 1;
               }
            }
            elem = rzalloc(c->mem_ctx, struct rwa_reflection_type);
            *elem = (struct rwa_reflection_type) {
               .data_type = pdt,
               .data_size = ptr_size,
               .alignment = palign,
               .name = ralloc_strdup(c->mem_ctx, ptr_type_name),
            };
         }
         mtype = rzalloc(c->mem_ctx, struct rwa_reflection_type);
         *mtype = (struct rwa_reflection_type) {
            .data_type = 60, .data_size = elem_size ? elem_size : 8, .alignment = 8,
            .name = ralloc_strdup(c->mem_ctx, type_name),
            .element = elem, .access = paccess,
            .constant_buffer = addrspace == 2 ? 1 : 0,
         };
      } else if (nested) {
         mtype = reflect_struct_from_md(c, type_name, nested, elem_size, 0,
                                        child_base_id, depth + 1);
         if (!mtype)
            return NULL;
      } else {
         struct air_type scalar;
         unsigned rows = 1, cols = 1;
         bool packed = false;
         unsigned dt = scalar_type(type_name, &scalar, &rows, &cols, &packed);
         unsigned malign = 1;
         if (dt) {
            unsigned eb = scalar.bits == 1 ? 1 : scalar.bits / 8;
            malign = packed ? eb : (rows == 3 ? 4 * eb : rows * eb);
         } else {
            malign = elem_size >= 8 ? 8 : (elem_size ? elem_size : 1);
         }
         mtype = rzalloc(c->mem_ctx, struct rwa_reflection_type);
         *mtype = (struct rwa_reflection_type) {
            .data_type = dt ? dt : 1,
            .data_size = elem_size,
            .alignment = malign,
            .name = ralloc_strdup(c->mem_ctx, type_name),
         };
      }

      unsigned total_size = elem_size;
      if (elem_count > 0) {
         total_size = elem_size * elem_count;
         struct rwa_reflection_type *array = rzalloc(c->mem_ctx, struct rwa_reflection_type);
         *array = (struct rwa_reflection_type) {
            .data_type = 2,
            .data_size = total_size,
            .alignment = mtype->alignment ? mtype->alignment : 1,
            .array_length = elem_count,
            .array_stride = elem_size,
            .element = mtype,
         };
         mtype = array;
      }

      if (mtype->alignment > max_align)
         max_align = mtype->alignment;
      if (count >= cap) {
         cap = cap ? cap * 2 : 8;
         fields = reralloc(c->mem_ctx, fields, struct rwa_reflection_member, cap);
      }
      fields[count++] = (struct rwa_reflection_member) {
         .name = ralloc_strdup(c->mem_ctx, field_name),
         .offset = offset,
         .indirect_argument_index = ind_idx,
         .type = mtype,
      };
      if (offset + total_size > max_end)
         max_end = offset + total_size;
      continue;
malformed:
      reflection_error(c, "reflection: member %u of struct %s has incomplete or mismatched layout",
                       count, name ? name : "(unnamed)");
      return NULL;
   }
   out->data_size = max_end;
   out->alignment = max_align ? max_align : 1;
   out->members = fields;
   out->member_count = count;
   return out;
}

struct rwa_compute_reflection *
air_stage_reflection(void *mem_ctx, const struct air_module *module,
                     const struct air_function *function,
                     const struct air_md *inputs, const char **error)
{
   struct rwa_compute_reflection *out = rzalloc(mem_ctx, struct rwa_compute_reflection);
   struct reflection_ctx c = { .mem_ctx = out, .module = module };
   if (error)
      *error = NULL;
   if (!module || !function || (function->num_args && !inputs) ||
       (inputs && (inputs->kind != AIR_MD_NODE || inputs->num_ops != function->num_args))) {
      reflection_error(&c, "reflection: stage inputs are incomplete");
      goto fail;
   }
   struct rwa_reflection_argument *args =
      rzalloc_array(out, struct rwa_reflection_argument, function->num_args);
   bool *seen = rzalloc_array(out, bool, function->num_args);
   bool *conditional = rzalloc_array(out, bool, function->num_args);
   out->arguments = args;
   for (unsigned i = 0; i < (inputs ? inputs->num_ops : 0); i++) {
      const struct air_md *node = inputs->ops[i];
      unsigned param;
      if (!md_uint(node, 0, &param) || param >= function->num_args || seen[param]) {
         reflection_error(&c, "reflection: input %u has an invalid or duplicate parameter", i);
         goto fail;
      }
      seen[param] = true;
      bool cond = md_key(node, "air.function_constant") >= 0;
      bool indirect = md_key(node, "air.indirect_buffer") >= 0;
      bool buffer = md_key(node, "air.buffer") >= 0;
      bool texture = md_key(node, "air.texture") >= 0;
      bool sampler = md_key(node, "air.sampler") >= 0;
      bool vis_ft = md_key(node, "air.visible_function_table") >= 0;
      bool prim_as = md_key(node, "air.primitive_acceleration_structure") >= 0;
      bool inst_as = md_key(node, "air.instance_acceleration_structure") >= 0;
      bool inter_ft = md_key(node, "air.intersection_function_table") >= 0;
      bool special = vis_ft || prim_as || inst_as || inter_ft;
      if (md_key(node, "air.indirect_argument") >= 0) {
         reflection_error(&c, "reflection: parameter %u has no supported direct argument declaration",
                          param);
         goto fail;
      }
      if (is_builtin(node)) {
         if (!buffer && !texture && !sampler && !indirect && !special)
            continue;
         reflection_error(&c, "reflection: parameter %u declares a builtin and a resource", param);
         goto fail;
      }
      if (indirect) {
         const char *name = md_key_string(node, "air.arg_name");
         const char *type_name = md_key_string(node, "air.arg_type_name");
         int location = md_key(node, "air.location_index");
         unsigned idx = 0, span = 1;
         if (!name || !*name || location < 0 || !md_uint(node, location + 1, &idx)) {
            reflection_error(&c, "reflection: argument buffer parameter %u lacks name or location",
                             param);
            goto fail;
         }
         md_uint(node, location + 2, &span);
         unsigned size = 0, alignment = 8, addrspace = 2;
         if (!md_key_uint(node, "air.arg_type_size", &size))
            md_key_uint(node, "air.buffer_size", &size);
         if (md_key_uint(node, "air.arg_type_align_size", &alignment) && alignment < 8)
            alignment = 8;
         md_key_uint(node, "air.address_space", &addrspace);
         const struct air_md *members = md_key_node(node, "air.struct_type_info");
         struct rwa_reflection_type *btype =
            reflect_struct_from_md(&c, type_name ? type_name : name, members, size,
                                   alignment, 0, 0);
         if (!btype)
            goto fail;
         btype->is_argument_buffer = 1;
         bool read = md_key(node, "air.read") >= 0 || md_key(node, "air.read_write") >= 0;
         bool write = md_key(node, "air.write") >= 0 || md_key(node, "air.read_write") >= 0;
         struct rwa_reflection_argument arg = {
            .name = ralloc_strdup(out, name),
            .type = 0,
            .index = idx,
            .access = (read && write) ? 1 : write ? 2 : 0,
            .active = md_key(node, "air.arg_unused") < 0,
            .array_length = span ? span : 1,
            .buffer_type = btype,
            .constant_buffer = addrspace == 2,
         };
         conditional[out->argument_count] = cond;
         args[out->argument_count++] = arg;
         continue;
      }
      if (buffer + texture + sampler + special != 1) {
         reflection_error(&c, "reflection: parameter %u has no supported direct argument declaration",
                          param);
         goto fail;
      }
      const char *name = md_key_string(node, "air.arg_name");
      const char *type_name = md_key_string(node, "air.arg_type_name");
      int location = md_key(node, "air.location_index");
      struct rwa_reflection_argument arg = {
         .type = buffer ? 0 : texture ? 2 : sampler ? 3 :
                 vis_ft ? 24 : prim_as ? 25 : inst_as ? 26 : 27,
         .active = md_key(node, "air.arg_unused") < 0,
      };
      bool has_span = location >= 0 && md_uint(node, location + 2, &arg.array_length);
      if (special && !has_span) {
         arg.array_length = 1;
         has_span = true;
      }
      if (!name || !*name || (!special && !type_name) || location < 0 ||
          !md_uint(node, location + 1, &arg.index) ||
          !has_span || !arg.array_length ||
          (!texture && arg.array_length != 1)) {
         reflection_error(&c, "reflection: parameter %u has incomplete name, type or location span",
                          param);
         goto fail;
      }
      arg.name = ralloc_strdup(out, name);
      bool read = md_key(node, "air.read") >= 0 || md_key(node, "air.sample") >= 0 ||
                  md_key(node, "air.read_write") >= 0 || sampler || special;
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
         unsigned addrspace, size = 0, alignment = 1;
         const struct air_type *pointer = function->args[param].type;
         bool is_void = !strcmp(type_name, "void");
         bool has_size = md_key_uint(node, "air.arg_type_size", &size);
         bool has_align = md_key_uint(node, "air.arg_type_align_size", &alignment);
         if (!md_key_uint(node, "air.address_space", &addrspace) ||
             addrspace < 1 || addrspace > 3 || !pointer ||
             pointer->kind != AIR_TYPE_POINTER || pointer->addrspace != addrspace ||
             (!is_void && (!has_size || !has_align))) {
            reflection_error(&c, "reflection: buffer %s has incomplete type size or address space", name);
            goto fail;
         }
         if (alignment > UINT16_MAX || (addrspace != 3 && size == UINT32_MAX)) {
            reflection_error(&c, "reflection: buffer %s size or alignment exceeds the native representation", name);
            goto fail;
         }
         arg.type = addrspace == 3 ? 1 : 0;
         arg.constant_buffer = addrspace == 2;
         if (is_void && (!has_size || !has_align)) {
            struct rwa_reflection_type *vt = rzalloc(out, struct rwa_reflection_type);
            *vt = (struct rwa_reflection_type) {
               .data_type = 0, .data_size = 0, .alignment = alignment ? alignment : 1,
               .name = ralloc_strdup(out, "void"),
            };
            arg.buffer_type = vt;
         } else {
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
      }
      for (unsigned j = 0; j < out->argument_count; j++) {
         if (!cond && !conditional[j] &&
             args[j].type == arg.type &&
             arg.index < args[j].index + args[j].array_length &&
             args[j].index < arg.index + arg.array_length) {
            reflection_error(&c, "reflection: arguments %s and %s share one binding index",
                             args[j].name, arg.name);
            goto fail;
         }
      }
      conditional[out->argument_count] = cond;
      args[out->argument_count++] = arg;
   }
   ralloc_free(conditional);
   ralloc_free(seen);
   return out;
fail:
   if (error)
      *error = ralloc_strdup(mem_ctx, c.error ? c.error : "reflection: out of memory");
   ralloc_free(out);
   return NULL;
}

struct rwa_compute_reflection *
air_compute_reflection(void *mem_ctx, const struct air_module *module,
                       const struct air_function *function,
                       const struct air_md *inputs, const char **error)
{
   return air_stage_reflection(mem_ctx, module, function, inputs, error);
}

struct rwa_compute_reflection *
air_module_reflect(void *mem_ctx, const void *bitcode, size_t size,
                   const char *entry, const char **error)
{
   if (error)
      *error = NULL;
   if (!bitcode || size < 4) {
      if (error)
         *error = ralloc_strdup(mem_ctx, "reflection: empty bitcode");
      return NULL;
   }
   void *scratch = ralloc_context(NULL);
   if (!scratch)
      return NULL;
   const char *read_error = NULL;
   struct air_module *m = air_module_read(scratch, bitcode, size, &read_error);
   if (!m) {
      if (error)
         *error = ralloc_asprintf(mem_ctx, "reflection: %s", read_error ? read_error : "bad bitcode");
      ralloc_free(scratch);
      return NULL;
   }
   static const char *const stage_md[] = { "air.kernel", "air.vertex", "air.fragment" };
   const struct air_md *found_entry = NULL;
   for (unsigned s = 0; s < ARRAY_SIZE(stage_md) && !found_entry; s++) {
      for (unsigned i = 0; i < m->num_named_md; i++) {
         if (strcmp(m->named_md[i].name, stage_md[s]))
            continue;
         for (unsigned k = 0; k < m->named_md[i].num_ops; k++) {
            const struct air_md *e = m->named_md[i].ops[k];
            if (!e || e->kind != AIR_MD_NODE || e->num_ops < 1 || !e->ops[0] ||
                e->ops[0]->kind != AIR_MD_VALUE ||
                e->ops[0]->value->kind != AIR_VALUE_FUNCTION)
               continue;
            const struct air_function *fn = (const struct air_function *)e->ops[0]->value;
            if (!entry || !strcmp(entry, fn->base.name ? fn->base.name : "")) {
               found_entry = e;
               break;
            }
         }
         if (found_entry)
            break;
      }
   }
   if (!found_entry) {
      if (error)
         *error = ralloc_strdup(mem_ctx, "reflection: entry point metadata not found");
      ralloc_free(scratch);
      return NULL;
   }
   const struct air_function *fn = (const struct air_function *)found_entry->ops[0]->value;
   const struct air_md *in_node = found_entry->num_ops > 2 ? found_entry->ops[2] : NULL;
   struct rwa_compute_reflection *out =
      air_stage_reflection(mem_ctx, m, fn, in_node, error);
   ralloc_free(scratch);
   return out;
}

static struct rwa_reflection_type *
clone_reflection_type(void *mem_ctx, const struct rwa_reflection_type *src)
{
   if (!src)
      return NULL;
   struct rwa_reflection_type *dst = rzalloc(mem_ctx, struct rwa_reflection_type);
   *dst = *src;
   dst->name = src->name ? ralloc_strdup(mem_ctx, src->name) : NULL;
   if (src->member_count && src->members) {
      struct rwa_reflection_member *m =
         rzalloc_array(mem_ctx, struct rwa_reflection_member, src->member_count);
      for (unsigned i = 0; i < src->member_count; i++) {
         m[i] = src->members[i];
         m[i].name = src->members[i].name ? ralloc_strdup(mem_ctx, src->members[i].name) : NULL;
         m[i].type = clone_reflection_type(mem_ctx, src->members[i].type);
      }
      dst->members = m;
   } else {
      dst->members = NULL;
   }
   dst->element = clone_reflection_type(mem_ctx, src->element);
   return dst;
}

struct rwa_compute_reflection *
air_reflection_clone(void *mem_ctx, const struct rwa_compute_reflection *src)
{
   if (!src)
      return NULL;
   struct rwa_compute_reflection *dst = rzalloc(mem_ctx, struct rwa_compute_reflection);
   dst->argument_count = src->argument_count;
   if (src->argument_count && src->arguments) {
      struct rwa_reflection_argument *args =
         rzalloc_array(dst, struct rwa_reflection_argument, src->argument_count);
      for (unsigned i = 0; i < src->argument_count; i++) {
         args[i] = src->arguments[i];
         args[i].name = src->arguments[i].name ? ralloc_strdup(dst, src->arguments[i].name) : NULL;
         args[i].buffer_type = clone_reflection_type(dst, src->arguments[i].buffer_type);
      }
      dst->arguments = args;
   }
   return dst;
}
