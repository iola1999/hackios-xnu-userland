/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * One AIR module -> one nir_shader. The mapping from AIR's named metadata to
 * NIR's variables, bindings and system values is the table in docs/air.md
 * section 3.1; this file is that table in code, and nothing here decides
 * anything the table does not state.
 */

#include "air_private.h"
#include "air_reflection.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util/hash_table.h"
#include "util/u_math.h"

bool
air_error(struct air_ctx *c, const char *fmt, ...)
{
   if (!c->error[0]) {
      va_list ap;
      va_start(ap, fmt);
      vsnprintf(c->error, sizeof(c->error), fmt, ap);
      va_end(ap);
   }
   return false;
}

void
air_note(struct air_ctx *c, const char *fmt, ...)
{
   struct air_shader_info *info = c->info;
   va_list ap;
   va_start(ap, fmt);
   char *text = ralloc_vasprintf(c->mem_ctx, fmt, ap);
   va_end(ap);
   /* the same note twice says nothing twice: two resources in one group, or a
    * callee translated for two call sites, would each add one */
   for (unsigned i = 0; i < info->num_notes; i++)
      if (!strcmp(info->notes[i], text))
         return;
   info->notes = reralloc(c->mem_ctx, info->notes, const char *, info->num_notes + 1);
   info->notes[info->num_notes++] = text;
}

/* -- values ------------------------------------------------------------- */

struct air_val *
air_val_ssa(struct air_ctx *c, nir_def *def)
{
   struct air_val *v = rzalloc(c->mem_ctx, struct air_val);
   v->kind = AIR_VAL_SSA;
   v->def = def;
   return v;
}

struct air_val *
air_val_ptr(struct air_ctx *c, struct air_ptr ptr)
{
   struct air_val *v = rzalloc(c->mem_ctx, struct air_val);
   v->kind = AIR_VAL_PTR;
   v->ptr = ptr;
   return v;
}

static struct air_val *
air_val_ptr_select(struct air_ctx *c, nir_def *cond,
                   struct air_val *a, struct air_val *b)
{
   struct air_val *v = rzalloc(c->mem_ctx, struct air_val);
   v->kind = AIR_VAL_PTR_SELECT;
   v->def = cond;
   v->num_elems = 2;
   v->elems = rzalloc_array(c->mem_ctx, struct air_val *, 2);
   v->elems[0] = a;
   v->elems[1] = b;
   return v;
}

/* Is this value a pointer in any of the three senses? */
static bool
val_is_pointerish(const struct air_val *v)
{
   return v && (v->kind == AIR_VAL_PTR || v->kind == AIR_VAL_PTR_SELECT);
}

/* Two resolutions that are the same resolution.
 *
 * This is what lets a `phi` or a `select` between pointers be answered without
 * choosing anything at run time, and it is needed rather than merely tidy:
 * LLVM duplicates a `getelementptr` onto two paths and then phis the copies
 * back together, so 207 of the pointer phis in Resident Evil 4's library have
 * two operands that name ONE member of ONE argument buffer. A pointer whose
 * byte offset is not a constant is compared by the def itself -- the same def
 * is the same offset, and two different defs cannot be proved equal here.
 */
static bool
same_ptr(const struct air_ptr *a, const struct air_ptr *b)
{
   if (a->class != b->class || a->binding != b->binding || a->is_image != b->is_image)
      return false;
   if (a->offset == b->offset)
      return true;
   if (!a->offset || !b->offset)
      return false;
   if (!nir_src_is_const(nir_src_for_ssa(a->offset)) ||
       !nir_src_is_const(nir_src_for_ssa(b->offset)))
      return false;
   return nir_src_as_uint(nir_src_for_ssa(a->offset)) ==
          nir_src_as_uint(nir_src_for_ssa(b->offset));
}

static bool
same_resource(const struct air_ptr *a, const struct air_ptr *b)
{
   return a->class == b->class && a->binding == b->binding && a->is_image == b->is_image;
}

/* A texture CHOSEN AT RUN TIME. space::rasterizeBackgroundFragment picks one of four
 * gobo textures by a uniform, with a `select` chain and a `phi` of texture arguments,
 * and passes the result to the function that samples it. A texture is not a value to
 * load, but its slot is: the pointer keeps binding 0 and carries the slot in .offset,
 * which a tex instruction takes as nir_tex_src_texture_offset (ir3 indexes the texture
 * state with it), and which already travels into called functions (CALL_PARAM_OFFSET)
 * and round a phi's register. Images and the null texture are not chosen this way.
 */
static bool
choosable_texture(const struct air_val *v)
{
   return v && v->kind == AIR_VAL_PTR && v->ptr.class == AIR_PTR_TEXTURE &&
          !v->ptr.is_image && !v->ptr.is_null;
}

/* The slot of one candidate of a run-time choice, which the application may leave unbound
 * when the choice does not pick it. */
static nir_def *
texture_slot(struct air_ctx *c, const struct air_ptr *p)
{
   if (p->offset)
      return nir_iadd_imm(&c->b, p->offset, p->binding);
   if (p->is_null)
      return nir_imm_int(&c->b, 0);
   air_texture_may_be_unbound(c, p->binding);
   return nir_imm_int(&c->b, p->binding);
}

/* A class whose pointers into one resource differ only by a byte offset that can be a
 * run-time value. Not a texture or a sampler (no offset), and not an argument buffer,
 * whose offset names a member and has to be known while translating.
 */
static bool
offset_is_value(enum air_ptr_class class)
{
   return class == AIR_PTR_UBO || class == AIR_PTR_SSBO || class == AIR_PTR_SHARED ||
          class == AIR_PTR_SCRATCH || class == AIR_PTR_CONSTANT_DATA ||
          class == AIR_PTR_TEXTURE_ARRAY;
}

static struct air_val *
air_val_agg(struct air_ctx *c, unsigned n)
{
   struct air_val *v = rzalloc(c->mem_ctx, struct air_val);
   v->kind = AIR_VAL_AGG;
   v->num_elems = n;
   v->elems = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(n, 1));
   return v;
}

#include "air_ptr_slot.inc"

void
air_set(struct air_ctx *c, const struct air_value *v, struct air_val *val)
{
   _mesa_hash_table_insert(c->values, v, val);
}

unsigned
air_nir_bit_size(struct air_ctx *c, const struct air_type *t)
{
   unsigned bits = air_type_bit_size(air_type_scalar(t));
   /* NIR has no i1 in SSA: booleans are 1-bit defs, which it does have.
    *
    * It has no 2- or 4-bit integer at all, though, and that is not a
    * hypothetical: an HLSL bit field reaches AIR as `i2` or `i4` through
    * SPIRV-Cross, and every NIR builder handed one ASSERTS instead of
    * refusing -- "Invalid Boolean size", "Invalid bit size" and "Invalid nir
    * alu bit size" between them aborted 495 of Resident Evil 4's modules the
    * first time the argument-buffer interface let them get as far as their
    * bodies. An abort is not a refusal: it says nothing a report can count and
    * it takes the run with it. So the width is named here, once, and 32 is
    * returned so that the instruction under construction stays well formed
    * while the error just set discards the shader.
    */
   if (bits != 1 && bits != 8 && bits != 16 && bits != 32 && bits != 64) {
      /* Zero is a different fact wearing the same symptom, and saying so in one
       * message made 79 modules read as carrying a 0-bit integer when what they
       * carry is a clip-distance ARRAY as a shader output: air_type_bit_size has
       * no answer for an aggregate, and this used to return it unexamined, so
       * `[4 x float]` became a one-component float output and four clip planes
       * became one with nothing in any log. A width NIR cannot hold and a type
       * that has no single width are both refusals, and they are not the same refusal.
       */
      if (!bits)
         air_error(c, "instruction: %s where a number belongs, which has no single "
                      "NIR width",
                   t->kind == AIR_TYPE_ARRAY ? "an array" :
                   t->kind == AIR_TYPE_STRUCT ? "a struct" :
                   t->kind == AIR_TYPE_POINTER ? "a pointer" : "a value of no size");
      else
         air_error(c, "instruction: a %u-bit integer, which is a width NIR does not have",
                   bits);
      return 32;
   }
   return bits;
}

unsigned
air_nir_components(const struct air_type *t)
{
   if (t->kind != AIR_TYPE_VECTOR)
      return 1;
   if (!nir_num_components_valid(t->count) && t->count <= NIR_MAX_VEC_COMPONENTS)
      return t->count <= 8 ? 8 : 16;
   return t->count;
}

/* The glsl type a NIR function parameter needs to carry a value of this AIR
 * type, matching what air_nir_bit_size and air_nir_components say the def will
 * be. NIR tracks a value's real type by use, so the only thing that has to be
 * right here is the shape.
 */
static const struct glsl_type *
param_glsl_type(struct air_ctx *c, const struct air_type *t)
{
   unsigned bits = air_nir_bit_size(c, t);
   return glsl_vector_type(bits == 16 ? GLSL_TYPE_FLOAT16 :
                           bits == 64 ? GLSL_TYPE_UINT64 : GLSL_TYPE_UINT,
                           air_nir_components(t));
}

/* Whether a struct member is a number this frontend can hand through a
 * function parameter -- a scalar or a vector of one. Everything else takes no
 * parameter: an array, a nested struct, a pointer. In practice the one that
 * occurs is the tail padding Metal's compiler puts on a vertex-output struct
 * (`[8 x i8]`), which the shader never writes and the caller never reads.
 */
static bool
ret_member_is_number(const struct air_type *m)
{
   if (m->kind != AIR_TYPE_VECTOR && m->kind != AIR_TYPE_INT && !air_type_is_float(m))
      return false;
   const struct air_type *sc = air_type_scalar(m);
   return sc->kind == AIR_TYPE_INT || air_type_is_float(sc);
}

static struct air_val *air_constant_val(struct air_ctx *c, const struct air_constant *k);

struct air_val *
air_get(struct air_ctx *c, const struct air_value *v)
{
   struct hash_entry *e = _mesa_hash_table_search(c->values, v);
   if (e)
      return e->data;
   const struct air_constant *k = air_value_as_constant(v);
   if (k) {
      /* A constant is built once and then reused from wherever it is asked
       * for, so it must be somewhere that dominates all of those places. A
       * constant has no operands, so the top of the entry block always will.
       *
       * Built at the cursor of its first use instead, a constant that LLVM
       * shares between a conditional block and a block after it is a use that
       * its def does not dominate -- and nothing complains, because
       * nir_lower_goto_ifs runs nir_repair_ssa, which repairs such a use into
       * a phi whose other edge is undef. That is how `phi i16 [ %30, %27 ],
       * [ -1, %22 ]` in MetalFaceMask_AddSeedPoint became `phi b3: %92,
       * b4: %0 (undefined)`: the -1 was the same constant as the one the
       * compare after the branch uses, so the store on the entry edge pointed
       * at a def inside the branch (docs/air.md section 5).
       */
      bool outer = !c->in_const;
      nir_cursor use = c->b.cursor;
      if (outer) {
         c->in_const = true;
         c->b.cursor = c->const_cursor;
      }
      struct air_val *val = air_constant_val(c, k);
      if (outer) {
         c->const_cursor = c->b.cursor;
         c->b.cursor = use;
         c->in_const = false;
      }
      if (val)
         air_set(c, v, val);
      return val;
   }
   air_error(c, "instruction: a %s value used before it is defined%s%s",
             v->kind == AIR_VALUE_GLOBAL ? "global" :
             v->kind == AIR_VALUE_FUNCTION ? "function" :
             v->kind == AIR_VALUE_ARGUMENT ? "argument" :
             v->kind == AIR_VALUE_INSTRUCTION ? "computed" : "metadata",
             v->name ? " " : "", v->name ? v->name : "");
   return NULL;
}

nir_def *
air_get_ssa(struct air_ctx *c, const struct air_value *v)
{
   struct air_val *val = air_get(c, v);
   if (!val)
      return NULL;
   if (val->kind != AIR_VAL_SSA) {
      air_error(c, "instruction: %s where a number belongs",
                val->kind == AIR_VAL_PTR ? "a pointer" :
                val->kind == AIR_VAL_PTR_SELECT ? "a chosen pointer" : "an aggregate");
      return NULL;
   }
   return val->def;
}

/* -- constants ---------------------------------------------------------- */

static nir_def *
scalar_constant(struct air_ctx *c, const struct air_type *t, uint64_t bits)
{
   unsigned n = air_type_bit_size(t);
   if (t->kind == AIR_TYPE_INT && t->bits == 1)
      return nir_imm_bool(&c->b, bits & 1);
   if (air_type_is_float(t)) {
      switch (n) {
      case 16: return nir_imm_intN_t(&c->b, bits, 16);
      case 32: return nir_imm_int(&c->b, (uint32_t)bits);
      case 64: return nir_imm_int64(&c->b, bits);
      default: break;
      }
   }
   return nir_imm_intN_t(&c->b, bits, MAX2(n, 1));
}

/* The float bits of a constant have to come through unchanged: a float
 * constant is built as an integer of the same bits and reinterpreted, so
 * that no NaN payload or denormal is rewritten on the way.
 */
static nir_def *
reinterpret_float(struct air_ctx *c, const struct air_type *t, nir_def *bits)
{
   if (!air_type_is_float(t))
      return bits;
   return nir_mov(&c->b, bits); /* same bits, and NIR tracks the type by use */
}

static struct air_val *
air_constant_val(struct air_ctx *c, const struct air_constant *k)
{
   const struct air_type *t = k->base.type;

   switch (k->ckind) {
   case AIR_CONST_INT:
   case AIR_CONST_FLOAT:
      return air_val_ssa(c, reinterpret_float(c, t, scalar_constant(c, air_type_scalar(t), k->u)));

   case AIR_CONST_NULL:
      if (t->kind == AIR_TYPE_POINTER) {
         struct air_ptr p = { .class = AIR_PTR_NONE, .pointee = t->elem };
         return air_val_ptr(c, p);
      }
      if (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY) {
         unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : t->count;
         struct air_val *v = air_val_agg(c, n);
         for (unsigned i = 0; i < n; i++) {
            const struct air_type *et = t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem;
            struct air_constant z = { .base = { .kind = AIR_VALUE_CONSTANT, .type = et },
                                      .ckind = AIR_CONST_NULL };
            if (!(v->elems[i] = air_constant_val(c, &z)))
               return NULL;
         }
         return v;
      }
      if (!nir_num_components_valid(air_nir_components(t)))
         return air_error(c, "instruction: a vector constant of %u elements",
                          air_nir_components(t)), NULL;
      return air_val_ssa(c, nir_imm_zero(&c->b, air_nir_components(t), air_nir_bit_size(c, t)));

   case AIR_CONST_UNDEF:
   case AIR_CONST_POISON:
      if (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY) {
         unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : t->count;
         struct air_val *v = air_val_agg(c, n);
         for (unsigned i = 0; i < n; i++) {
            const struct air_type *et = t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem;
            if (et->kind != AIR_TYPE_STRUCT && et->kind != AIR_TYPE_ARRAY)
               continue;
            struct air_constant u = { .base = { .kind = AIR_VALUE_CONSTANT, .type = et },
                                      .ckind = k->ckind };
            if (!(v->elems[i] = air_constant_val(c, &u)))
               return NULL;
         }
         return v;
      }
      if (t->kind == AIR_TYPE_POINTER) {
         struct air_ptr p = { .class = AIR_PTR_NONE, .pointee = t->elem };
         return air_val_ptr(c, p);
      }
      if (!nir_num_components_valid(air_nir_components(t)))
         return air_error(c, "instruction: a vector undef of %u elements",
                          air_nir_components(t)), NULL;
      return air_val_ssa(c, nir_undef(&c->b, air_nir_components(t), air_nir_bit_size(c, t)));

   case AIR_CONST_AGGREGATE: {
      if (t->kind == AIR_TYPE_VECTOR) {
         unsigned nc = air_nir_components(t);
         nir_def *comps[NIR_MAX_VEC_COMPONENTS];
         if (!nir_num_components_valid(nc) || k->num_elems > nc)
            return air_error(c, "instruction: a vector constant of %u elements", k->num_elems), NULL;
         for (unsigned i = 0; i < k->num_elems; i++)
            if (!(comps[i] = air_get_ssa(c, k->elems[i])))
               return NULL;
         for (unsigned i = k->num_elems; i < nc; i++)
            comps[i] = nir_undef(&c->b, 1, air_nir_bit_size(c, t));
         return air_val_ssa(c, nir_vec(&c->b, comps, nc));
      }
      struct air_val *v = air_val_agg(c, k->num_elems);
      for (unsigned i = 0; i < k->num_elems; i++)
         if (!(v->elems[i] = air_get(c, k->elems[i])))
            return NULL;
      return v;
   }

   case AIR_CONST_DATA: {
      const struct air_type *et = t->kind == AIR_TYPE_STRUCT ? NULL : t->elem;
      if (!et)
         return air_error(c, "instruction: a data constant of a struct type"), NULL;
      if (t->kind == AIR_TYPE_VECTOR) {
         unsigned nc = air_nir_components(t);
         nir_def *comps[NIR_MAX_VEC_COMPONENTS];
         if (!nir_num_components_valid(nc) || k->num_elems > nc)
            return air_error(c, "instruction: a vector constant of %u elements", k->num_elems), NULL;
         for (unsigned i = 0; i < k->num_elems; i++)
            comps[i] = reinterpret_float(c, et, scalar_constant(c, et, k->data[i]));
         for (unsigned i = k->num_elems; i < nc; i++)
            comps[i] = nir_undef(&c->b, 1, air_nir_bit_size(c, et));
         return air_val_ssa(c, nir_vec(&c->b, comps, nc));
      }
      struct air_val *v = air_val_agg(c, k->num_elems);
      for (unsigned i = 0; i < k->num_elems; i++)
         v->elems[i] = air_val_ssa(c, reinterpret_float(c, et, scalar_constant(c, et, k->data[i])));
      return v;
   }

   case AIR_CONST_EXPR:
      /* with opaque pointers the only constant expression left in the corpus
       * is a pointer bitcast, which is the pointer itself
       */
      if (k->expr_op == AIR_OP_CAST && k->expr_sub == AIR_CAST_BITCAST &&
          k->base.type->kind == AIR_TYPE_POINTER)
         return air_get(c, k->operands[0]);
      /* ... and a constant getelementptr, which the GL-on-Metal vertex shaders
       * use to name the first element of a module array
       * (`getelementptr inbounds ([2 x <4 x float>], ptr @g, i64 0, i64 0)`).
       * It is the instruction form with every index a constant, so the whole
       * walk is done here in integers and the result is the base pointer with
       * a constant byte offset and a narrowed pointee.
       */
      if (k->expr_op == AIR_OP_GEP) {
         if (k->num_operands < 2 || !k->src_type)
            return air_error(c, "instruction: a constant getelementptr with no base "
                                "or no source type"), NULL;
         struct air_val *base = air_get(c, k->operands[0]);
         if (!base)
            return NULL;
         if (base->kind != AIR_VAL_PTR)
            return air_error(c, "instruction: a constant getelementptr on something "
                                "that is not a pointer"), NULL;
         struct air_ptr p = base->ptr;
         const struct air_type *t = k->src_type;
         uint64_t off = 0, idx = 0;
         if (!air_const_uint(k->operands[1], &idx))
            return air_error(c, "instruction: a constant getelementptr whose first "
                                "index is not a constant"), NULL;
         off += idx * air_type_alloc_size(c->m, t);
         for (unsigned i = 2; i < k->num_operands; i++) {
            if (!air_const_uint(k->operands[i], &idx))
               return air_error(c, "instruction: a constant getelementptr with an index "
                                   "that is not a constant"), NULL;
            if (t->kind == AIR_TYPE_STRUCT) {
               if (idx >= t->num_members)
                  return air_error(c, "instruction: a constant getelementptr to struct "
                                      "field %u of %u", (unsigned)idx,
                                   t->num_members), NULL;
               off += air_struct_member_offset(c->m, t, idx);
               t = t->members[idx];
            } else if (t->kind == AIR_TYPE_ARRAY || t->kind == AIR_TYPE_VECTOR) {
               off += idx * (t->kind == AIR_TYPE_ARRAY
                                ? air_type_alloc_size(c->m, t->elem)
                                : MAX2(air_type_bit_size(t->elem) / 8, 1));
               t = t->elem;
            } else {
               return air_error(c, "instruction: a constant getelementptr into a "
                                   "scalar"), NULL;
            }
         }
         p.pointee = t;
         if (off)
            p = air_ptr_add(c, p, nir_imm_int(&c->b, off));
         return air_val_ptr(c, p);
      }
      return air_error(c, "instruction: constant expression %u", k->expr_op), NULL;
   }
   return air_error(c, "instruction: constant kind %u", k->ckind), NULL;
}

/* -- memory ------------------------------------------------------------- */

struct air_ptr
air_ptr_add(struct air_ctx *c, struct air_ptr ptr, nir_def *bytes)
{
   if (!bytes)
      return ptr;
   if (!ptr.offset) {
      ptr.offset = bytes;
      return ptr;
   }
   /* Two constant offsets become one constant, and that is a correctness fix
    * rather than a tidiness.
    *
    * A cached constant is built in the const region at the top of the entry
    * block (air_get), while a module-scope variable's byte offset is built
    * where materialise_globals runs, which is BELOW that region. A constant
    * `getelementptr` into a threadgroup array -- `load i32, ptr addrspace(3)
    * getelementptr([16 x i32], ptr @tbl, i64 0, i64 9)`, which is how
    * SPIRV-Cross reads a fixed slot of one -- is a constant whose value was
    * therefore `iadd %globaloffset, 36` sitting above the def of
    * %globaloffset: a use its def does not dominate, which nir_validate
    * rejects with "src->_parent & SRC_TAG_SEEN". 115 of Resident Evil 4's
    * modules are that shape, and every one of them ABORTED the translator the
    * first time the argument-buffer interface let their bodies be reached.
    *
    * Folding the pair removes the reference: a global's offset is always a
    * constant here, so the whole chain is one immediate built in the const
    * region with no operand at all. The latent ordering hazard it stands on is
    * written down in docs/air.md rather than left to be rediscovered.
    */
   if (nir_src_is_const(nir_src_for_ssa(ptr.offset)) &&
       nir_src_is_const(nir_src_for_ssa(bytes)) &&
       ptr.offset->bit_size == 32 && bytes->bit_size == 32 &&
       ptr.offset->num_components == 1 && bytes->num_components == 1) {
      uint32_t sum = (uint32_t)nir_src_as_uint(nir_src_for_ssa(ptr.offset)) +
                     (uint32_t)nir_src_as_uint(nir_src_for_ssa(bytes));
      ptr.offset = nir_imm_int(&c->b, sum);
      return ptr;
   }
   ptr.offset = nir_iadd(&c->b, ptr.offset, bytes);
   return ptr;
}

static nir_def *
ptr_offset(struct air_ctx *c, struct air_ptr ptr)
{
   return ptr.offset ? ptr.offset : nir_imm_int(&c->b, 0);
}

nir_def *
air_load_ptr(struct air_ctx *c, struct air_ptr ptr, const struct air_type *type, unsigned align)
{
   if (type->kind == AIR_TYPE_VECTOR && !nir_num_components_valid(type->count))
      return air_error(c, "instruction: a vector load of %u elements", (unsigned)type->count), NULL;
   unsigned comps = air_nir_components(type);
   unsigned bits = air_nir_bit_size(c, type);
   if (type->kind == AIR_TYPE_POINTER)
      return air_error(c, "instruction: a pointer loaded from memory (argument buffers)"), NULL;
   if (!align)
      align = MAX2(bits / 8, 1);

   /* a 1-bit load is a byte in memory */
   bool as_bool = bits == 1;
   if (as_bool)
      bits = 8;

   nir_def *off = ptr_offset(c, ptr);
   nir_def *res;
   switch (ptr.class) {
   case AIR_PTR_UBO:
      res = nir_load_ubo(&c->b, comps, bits, nir_imm_int(&c->b, ptr.binding), off,
                         .align_mul = align, .align_offset = 0, .range_base = 0, .range = ~0);
      break;
   case AIR_PTR_SSBO:
      res = nir_load_ssbo(&c->b, comps, bits, nir_imm_int(&c->b, ptr.binding), off,
                          .align_mul = align, .align_offset = 0);
      break;
   case AIR_PTR_SHARED:
      res = nir_load_shared(&c->b, comps, bits, off, .base = 0, .align_mul = align);
      break;
   case AIR_PTR_SCRATCH:
      res = nir_load_scratch(&c->b, comps, bits, off, .align_mul = align);
      break;
   case AIR_PTR_CONSTANT_DATA:
      res = nir_load_constant(&c->b, comps, bits, off, .base = 0, .range = c->nir->constant_data_size,
                              .align_mul = align);
      break;
   case AIR_PTR_FUNCTION_CONSTANT: {
      /* its value is known at pipeline creation, so the load is that value */
      const struct air_constant_value *v = NULL;
      for (unsigned i = 0; c->options && i < c->options->num_constants; i++)
         if (c->options->constants[i].index == ptr.binding)
            v = &c->options->constants[i];
      if (!v)
         return air_error(c, "interface: function constant %u has no value", ptr.binding), NULL;
      if (comps == 1) {
         res = nir_imm_intN_t(&c->b, v->bits, as_bool ? 8 : bits);
         break;
      }
      /* A VECTOR function constant. Metal allows one and the GL-on-Metal
       * fragment shaders use two: `uchar3` opcode triples for the texture
       * combiners (`fc_color_opcodes0/1`, declared `<3 x i8>`).
       *
       * air_constant_value carries one 64-bit word per constant, so the
       * components have to be fields of it, and the packing is this frontend's
       * own interface with the runtime rather than something read out of
       * Apple's: component i sits at bit i*width, which is the word a memcpy
       * of the value would produce on this little-endian target. NOTHING HERE
       * MEASURES THAT -- every harness in tools/air supplies zero for every
       * constant, so all components are zero whatever the packing, and the
       * first thing that would tell a wrong packing from a right one is a
       * pipeline that sets one of these two constants.
       */
      nir_def *fields[NIR_MAX_VEC_COMPONENTS];
      for (unsigned i = 0; i < comps; i++) {
         uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
         uint64_t val = (i * bits < 64) ? ((v->bits >> (i * bits)) & mask) : 0ull;
         fields[i] = nir_imm_intN_t(&c->b, val, as_bool ? 8 : bits);
      }
      res = nir_vec(&c->b, fields, comps);
      break;
   }
   case AIR_PTR_ARGBUF:
      /* argbuf_load answers a load through an argument buffer before it gets
       * here, so this is a load that resolution could not make: through a
       * pointer CHOSEN at run time between two sets, which load_through_choice
       * would read from both of. There is nothing to read either way -- the
       * bytes in an argument buffer are the runtime's 8-byte handles.
       */
      return air_error(c, "instruction: a load of bytes out of an argument buffer"), NULL;
   default:
      return air_error(c, "instruction: a load through a %s pointer",
                       ptr.class == AIR_PTR_NONE ? "null" : "texture or sampler"), NULL;
   }
   return as_bool ? nir_ine_imm(&c->b, res, 0) : res;
}

bool
air_store_ptr(struct air_ctx *c, struct air_ptr ptr, nir_def *value, unsigned align)
{
   unsigned comps = value->num_components;
   if (value->bit_size == 1) {
      value = nir_b2iN(&c->b, value, 8);
   }
   if (!align)
      align = MAX2(value->bit_size / 8, 1);

   nir_def *off = ptr_offset(c, ptr);
   switch (ptr.class) {
   case AIR_PTR_SSBO:
      nir_store_ssbo(&c->b, value, nir_imm_int(&c->b, ptr.binding), off,
                     .write_mask = BITFIELD_MASK(comps), .align_mul = align);
      return true;
   case AIR_PTR_SHARED:
      nir_store_shared(&c->b, value, off, .base = 0, .write_mask = BITFIELD_MASK(comps),
                       .align_mul = align);
      return true;
   case AIR_PTR_SCRATCH:
      nir_store_scratch(&c->b, value, off, .align_mul = align,
                        .write_mask = BITFIELD_MASK(comps));
      return true;
   case AIR_PTR_UBO:
   case AIR_PTR_CONSTANT_DATA:
      return air_error(c, "instruction: a store into constant memory");
   case AIR_PTR_ARGBUF:
      /* Writing a descriptor from inside the shader. The handle it would write
       * is the runtime's, and this frontend has resolved the set's members to
       * bindings rather than to addresses, so there is nothing to write.
       */
      return air_error(c, "instruction: a store into an argument buffer");
   default:
      return air_error(c, "instruction: a store through a %s pointer",
                       ptr.class == AIR_PTR_NONE ? "null" : "texture or sampler");
   }
}

/* -- metadata helpers --------------------------------------------------- */

static const char *
md_str(const struct air_md *node, unsigned i)
{
   if (!node || i >= node->num_ops || !node->ops[i] || node->ops[i]->kind != AIR_MD_STRING)
      return NULL;
   return air_md_string(node->ops[i]);
}

static bool
md_int(const struct air_md *node, unsigned i, uint64_t *out)
{
   if (!node || i >= node->num_ops || !node->ops[i] || node->ops[i]->kind != AIR_MD_VALUE)
      return false;
   /* zero is written as a null constant, as everywhere else in the bitcode */
   return air_const_uint(node->ops[i]->value, out);
}

/* the index of the operand after the key, or -1 */
static int
md_key(const struct air_md *node, const char *key)
{
   for (unsigned i = 0; i < (node ? node->num_ops : 0); i++) {
      const char *s = md_str(node, i);
      if (s && !strcmp(s, key))
         return i;
   }
   return -1;
}

static bool
md_key_int(const struct air_md *node, const char *key, uint64_t *out)
{
   int i = md_key(node, key);
   return i >= 0 && md_int(node, i + 1, out);
}

/* the operand after the key, when it is a nested node: the shape Apple uses for
 * a key whose value is itself a list, as `air.function_constant` is
 * (`!{ptr @__metal_implicit_fc_pred_6, !"bool", !"vtx_attr_has_texcoords"}`)
 */
static const struct air_md *
md_key_node(const struct air_md *node, const char *key)
{
   int i = md_key(node, key);
   if (i < 0 || (unsigned)(i + 1) >= node->num_ops || !node->ops[i + 1] ||
       node->ops[i + 1]->kind != AIR_MD_NODE)
      return NULL;
   return node->ops[i + 1];
}

static const char *
md_key_str(const struct air_md *node, const char *key)
{
   int i = md_key(node, key);
   return i >= 0 ? md_str(node, i + 1) : NULL;
}

static const struct air_named_md *
named_md(const struct air_module *m, const char *name)
{
   for (unsigned i = 0; i < m->num_named_md; i++)
      if (!strcmp(m->named_md[i].name, name))
         return &m->named_md[i];
   return NULL;
}

/* -- stage -------------------------------------------------------------- */

struct stage_md {
   mesa_shader_stage stage;
   const char *md_name;
};

static const struct stage_md stages[] = {
   { MESA_SHADER_VERTEX, "air.vertex" },
   { MESA_SHADER_FRAGMENT, "air.fragment" },
   { MESA_SHADER_COMPUTE, "air.kernel" },
   { MESA_SHADER_NONE, "air.visible" },
};

static int
module_stage(const struct air_module *m, const struct air_md **entry)
{
   for (unsigned i = 0; i < ARRAY_SIZE(stages); i++) {
      const struct air_named_md *n = named_md(m, stages[i].md_name);
      if (n && n->num_ops >= 1 && n->ops[0] && n->ops[0]->kind == AIR_MD_NODE) {
         if (entry)
            *entry = n->ops[0];
         return stages[i].stage;
      }
   }
   return -2; /* no entry point at all */
}

int
air_module_stage(void *mem_ctx, const void *bitcode, size_t size, const char **calls,
                 const char **error)
{
   void *ctx = ralloc_context(mem_ctx);
   struct air_module *m = air_module_read(ctx, bitcode, size, error);
   if (!m) {
      ralloc_free(ctx);
      return -1;
   }

   if (calls) {
      size_t len = 1;
      for (unsigned i = 0; i < m->num_functions; i++) {
         const char *n = m->functions[i]->base.name;
         if (m->functions[i]->is_declaration && n && !strncmp(n, "air.", 4))
            len += strlen(n) + 1;
      }
      char *buf = rzalloc_size(mem_ctx, len), *p = buf;
      for (unsigned i = 0; i < m->num_functions; i++) {
         const char *n = m->functions[i]->base.name;
         if (m->functions[i]->is_declaration && n && !strncmp(n, "air.", 4)) {
            memcpy(p, n, strlen(n) + 1);
            p += strlen(n) + 1;
         }
      }
      *calls = buf;
   }

   int stage = module_stage(m, NULL);
   ralloc_free(ctx);
   return stage;
}

unsigned
air_module_function_constants(void *mem_ctx, const void *bitcode, size_t size,
                              struct air_constant_value **out)
{
   void *ctx = ralloc_context(mem_ctx);
   const char *err;
   struct air_module *m = air_module_read(ctx, bitcode, size, &err);
   *out = NULL;
   if (!m) {
      ralloc_free(ctx);
      return 0;
   }
   const struct air_named_md *n = named_md(m, "air.function_constants");
   unsigned num = 0;
   if (n && n->num_ops) {
      *out = rzalloc_array(mem_ctx, struct air_constant_value, n->num_ops);
      for (unsigned i = 0; i < n->num_ops; i++) {
         const struct air_md *node = n->ops[i];
         for (unsigned k = 1; k < (node ? node->num_ops : 0); k++) {
            uint64_t idx;
            if (md_int(node, k, &idx)) {
               (*out)[num++].index = idx;
               break;
            }
         }
      }
   }
   ralloc_free(ctx);
   return num;
}

#include "air_interface.inc"
#include "air_ops.inc"
#include "air_cfg.inc"

/* -- module-scope variables --------------------------------------------- */

/* the index !air.function_constants gives this global, or -1 */
static int
function_constant_index(struct air_ctx *c, const struct air_global *g)
{
   const struct air_named_md *n = named_md(c->m, "air.function_constants");
   for (unsigned i = 0; i < (n ? n->num_ops : 0); i++) {
      const struct air_md *node = n->ops[i];
      if (!node || node->kind != AIR_MD_NODE || !node->num_ops)
         continue;
      if (!node->ops[0] || node->ops[0]->kind != AIR_MD_VALUE ||
          node->ops[0]->value != &g->base)
         continue;
      /* !{ptr @g, !"uint", !"name", i32 index, i1 true} */
      for (unsigned k = 1; k < node->num_ops; k++) {
         uint64_t idx;
         if (md_int(node, k, &idx))
            return (int)idx;
      }
      return -1;
   }
   return -1;
}

/* Decide what every global is: once per shader, because this appends inline
 * samplers to info->resources and lays out the shared and scratch memory. It
 * records rather than builds -- making the value needs a builder, and which
 * function's builder matters (see materialise_globals).
 */
#include "air_sampler.inc"

#include "air_globals.inc"

/* Build the globals as values of the function being translated now. Called for
 * the entry point and again for every callee, because the byte offset is an
 * nir_def and a def belongs to one impl: handing a callee the entry point's
 * defs is what crashed nir_repair_ssa. Module-level constants need no such
 * care -- air_get materialises one on demand in whatever builder asks.
 */
static void
materialise_globals(struct air_ctx *c)
{
   for (unsigned i = 0; i < c->m->num_globals; i++) {
      const struct air_global_plan *pl = &c->global_plan[i];
      if (pl->skip)
         continue;
      const struct air_global *g = c->m->globals[i];
      struct air_ptr p = {
         .pointee = g->value_type,
         .class = pl->class,
         .binding = pl->binding,
         .offset = pl->has_offset ? nir_imm_int(&c->b, pl->offset) : NULL,
      };
      air_set(c, &g->base, air_val_ptr(c, p));
   }
}

#include "air_link.inc"

/* The functions of @llvm.global_ctors, inlined at the top of the entry point:
 * this is where a function constant turns into the program-scope variable the
 * body reads (AV1SW computes kPixelShift from kBitDepth that way).
 */
static void
run_static_initializers(struct air_ctx *c)
{
   for (unsigned i = 0; i < c->m->num_functions; i++) {
      const struct air_function *f = c->m->functions[i];
      if (f->is_declaration || !f->section || strcmp(f->section, "air.static_init"))
         continue;
      if (f->num_args) {
         air_error(c, "interface: a static initializer with arguments");
         return;
      }
      if (f->num_blocks != 1) {
         /* A static initializer with branches cannot be translated in place:
          * the CFG walk owns the impl's start block, so running it a second
          * time in the same impl would map two AIR blocks onto it. It becomes
          * a nir_function instead and the entry point calls it, which is the
          * machinery a call of another function in the module already uses,
          * and nir_inline_functions folds it back in afterwards. The globals
          * it writes are at the offsets plan_globals decided, which is one
          * plan for the whole module, so the callee's stores and the entry
          * point's loads reach the same scratch.
          *
          * These are the GL-on-Metal fragment shaders, whose initializer
          * decides 58 function constants and has a switch in it -- which is
          * also why this could not be reached before the reader learnt to read
          * one.
          */
         nir_function *fn = translate_callee(c, f, NULL, NULL);
         if (!fn)
            return;   /* translate_callee has said why */
         nir_builder_instr_insert(&c->b, &nir_call_instr_create(c->nir, fn)->instr);
         continue;
      }
      /* its instructions, in place, without its ret */
      const struct air_block *blk = &f->blocks[0];
      for (unsigned k = 0; k < blk->num_insts; k++) {
         const struct air_inst *in = blk->insts[k];
         if (in->op == AIR_OP_RET)
            continue;
         if (!translate_instruction(c, in))
            return;
      }
   }
}

/* -- the entry point ---------------------------------------------------- */

#include "air_stitch.inc"
#include "air_entry.inc"
