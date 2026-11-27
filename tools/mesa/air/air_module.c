/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * Bitcode records -> struct air_module. The record layouts are those LLVM
 * has written since the 4.0/5.0 days (string table, relative value ids,
 * explicit types on loads, GEPs and calls), which is what Apple's
 * metalfe-31001 emits: typed pointers, MODULE_CODE_VERSION 2.
 */

#include "air_module.h"
#include "air_bitcode.h"

#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

#include "util/macros.h"
#include "util/ralloc.h"

/* block ids */
enum {
   MODULE_BLOCK_ID = 8,
   PARAMATTR_BLOCK_ID = 9,
   PARAMATTR_GROUP_BLOCK_ID = 10,
   CONSTANTS_BLOCK_ID = 11,
   FUNCTION_BLOCK_ID = 12,
   IDENTIFICATION_BLOCK_ID = 13,
   VALUE_SYMTAB_BLOCK_ID = 14,
   METADATA_BLOCK_ID = 15,
   METADATA_ATTACHMENT_ID = 16,
   TYPE_BLOCK_ID_NEW = 17,
   USELIST_BLOCK_ID = 18,
   OPERAND_BUNDLE_TAGS_BLOCK_ID = 21,
   METADATA_KIND_BLOCK_ID = 22,
   STRTAB_BLOCK_ID = 23,
   SYMTAB_BLOCK_ID = 25,
   SYNC_SCOPE_NAMES_BLOCK_ID = 26,
};

/* module records */
enum {
   MODULE_CODE_VERSION = 1,
   MODULE_CODE_TRIPLE = 2,
   MODULE_CODE_DATALAYOUT = 3,
   MODULE_CODE_SECTIONNAME = 5,
   MODULE_CODE_GLOBALVAR = 7,
   MODULE_CODE_FUNCTION = 8,
   MODULE_CODE_GCNAME = 11,
   MODULE_CODE_VSTOFFSET = 13,
   MODULE_CODE_SOURCE_FILENAME = 16,
   MODULE_CODE_HASH = 17,
};

/* type records */
enum {
   TYPE_CODE_NUMENTRY = 1,
   TYPE_CODE_VOID = 2,
   TYPE_CODE_FLOAT = 3,
   TYPE_CODE_DOUBLE = 4,
   TYPE_CODE_LABEL = 5,
   TYPE_CODE_OPAQUE = 6,
   TYPE_CODE_INTEGER = 7,
   TYPE_CODE_POINTER = 8,
   TYPE_CODE_HALF = 10,
   TYPE_CODE_ARRAY = 11,
   TYPE_CODE_VECTOR = 12,
   TYPE_CODE_X86_FP80 = 13,
   TYPE_CODE_FP128 = 14,
   TYPE_CODE_PPC_FP128 = 15,
   TYPE_CODE_METADATA = 16,
   TYPE_CODE_X86_MMX = 17,
   TYPE_CODE_STRUCT_ANON = 18,
   TYPE_CODE_STRUCT_NAME = 19,
   TYPE_CODE_STRUCT_NAMED = 20,
   TYPE_CODE_FUNCTION = 21,
   TYPE_CODE_TOKEN = 22,
   TYPE_CODE_BFLOAT = 23,
   TYPE_CODE_OPAQUE_POINTER = 25,
};

/* constants records */
enum {
   CST_CODE_SETTYPE = 1,
   CST_CODE_NULL = 2,
   CST_CODE_UNDEF = 3,
   CST_CODE_INTEGER = 4,
   CST_CODE_WIDE_INTEGER = 5,
   CST_CODE_FLOAT = 6,
   CST_CODE_AGGREGATE = 7,
   CST_CODE_STRING = 8,
   CST_CODE_CSTRING = 9,
   CST_CODE_CE_BINOP = 10,
   CST_CODE_CE_CAST = 11,
   CST_CODE_CE_GEP_OLD = 12,
   CST_CODE_CE_SELECT = 13,
   CST_CODE_CE_EXTRACTELT = 14,
   CST_CODE_CE_INSERTELT = 15,
   CST_CODE_CE_SHUFFLEVEC = 16,
   CST_CODE_CE_CMP = 17,
   CST_CODE_CE_INBOUNDS_GEP = 20,
   CST_CODE_DATA = 22,
   CST_CODE_CE_GEP_WITH_INRANGE_INDEX = 24,
   CST_CODE_CE_UNOP = 25,
   CST_CODE_POISON = 26,
};

/* function records */
enum {
   FUNC_CODE_DECLAREBLOCKS = 1,
   FUNC_CODE_INST_BINOP = 2,
   FUNC_CODE_INST_CAST = 3,
   FUNC_CODE_INST_SELECT = 5,
   FUNC_CODE_INST_EXTRACTELT = 6,
   FUNC_CODE_INST_INSERTELT = 7,
   FUNC_CODE_INST_SHUFFLEVEC = 8,
   FUNC_CODE_INST_CMP = 9,
   FUNC_CODE_INST_RET = 10,
   FUNC_CODE_INST_BR = 11,
   FUNC_CODE_INST_SWITCH = 12,
   FUNC_CODE_INST_UNREACHABLE = 15,
   FUNC_CODE_INST_PHI = 16,
   FUNC_CODE_INST_ALLOCA = 19,
   FUNC_CODE_INST_LOAD = 20,
   FUNC_CODE_INST_EXTRACTVAL = 26,
   FUNC_CODE_INST_INSERTVAL = 27,
   FUNC_CODE_INST_CMP2 = 28,
   FUNC_CODE_INST_VSELECT = 29,
   FUNC_CODE_DEBUG_LOC_AGAIN = 33,
   FUNC_CODE_INST_CALL = 34,
   FUNC_CODE_DEBUG_LOC = 35,
   FUNC_CODE_INST_FENCE = 36,
   FUNC_CODE_INST_GEP = 43,
   FUNC_CODE_INST_STORE = 44,
   FUNC_CODE_INST_CMPXCHG = 46,
   FUNC_CODE_OPERAND_BUNDLE = 55,
   FUNC_CODE_INST_UNOP = 56,
   FUNC_CODE_INST_FREEZE = 58,
   FUNC_CODE_INST_ATOMICRMW = 59,
};

/* metadata records */
enum {
   METADATA_STRING_OLD = 1,
   METADATA_VALUE = 2,
   METADATA_NODE = 3,
   METADATA_NAME = 4,
   METADATA_DISTINCT_NODE = 5,
   METADATA_KIND = 6,
   METADATA_LOCATION = 7,
   METADATA_OLD_NODE = 8,
   METADATA_OLD_FN_NODE = 9,
   METADATA_NAMED_NODE = 10,
   METADATA_ATTACHMENT = 11,
   METADATA_STRINGS = 35,
   METADATA_GLOBAL_DECL_ATTACHMENT = 36,
   METADATA_INDEX_OFFSET = 38,
   METADATA_INDEX = 39,
   METADATA_ARG_LIST = 46,
};

enum { VST_CODE_ENTRY = 1, VST_CODE_BBENTRY = 2, VST_CODE_FNENTRY = 3 };

/* call record CC field */
#define CALL_TAIL_BIT 0
#define CALL_CCONV_SHIFT 1
#define CALL_EXPLICIT_TYPE_BIT 15
#define CALL_FMF_BIT 17

struct reader {
   struct air_module *m;
   struct air_bitstream bs;

   /* values of the function being read, after the module's */
   unsigned num_values, cap_values;
   struct air_value **values;

   /* the types the record gave for operands not yet defined, by value id */
   unsigned cap_fwd;
   const struct air_type **fwd;

   /* metadata ids: the module's, then the current function's */
   unsigned num_md, cap_md;
   struct air_md **md;

   const uint8_t *strtab;
   size_t strtab_size;

   unsigned num_bodies_read;
   struct air_function *cur_fn;
   unsigned cur_inst_id;     /* all instructions, for attachments */
   unsigned num_fn_insts, cap_fn_insts;
   struct air_inst **fn_insts;

   const char *pending_md_name;
};

static bool
fail(struct reader *r, const char *fmt, ...)
{
   if (r->m->error[0])
      return false;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(r->m->error, sizeof(r->m->error), fmt, ap);
   va_end(ap);
   return false;
}

static bool
check_bs(struct reader *r)
{
   if (r->bs.error[0])
      return fail(r, "bitstream: %s", r->bs.error);
   return true;
}

/* -- types -------------------------------------------------------------- */

static const struct air_type *
type_by_id(struct reader *r, uint64_t id)
{
   if (id >= r->m->num_types) {
      fail(r, "type id %" PRIu64 " out of range (%u types)", id, r->m->num_types);
      return NULL;
   }
   return &r->m->types[id];
}

/* pointers that the bitcode does not list (to functions, to globals with an
 * explicit type), made on the side
 */
static const struct air_type *
pointer_to(struct reader *r, const struct air_type *elem, unsigned as)
{
   for (unsigned i = 0; i < r->m->num_types; i++) {
      const struct air_type *t = &r->m->types[i];
      if (t->kind == AIR_TYPE_POINTER && t->elem == elem && t->addrspace == as)
         return t;
   }
   struct air_type *t = rzalloc(r->m, struct air_type);
   t->kind = AIR_TYPE_POINTER;
   t->id = ~0u;
   t->elem = elem;
   t->addrspace = as;
   return t;
}

static bool
read_type_block(struct reader *r)
{
   struct air_module *m = r->m;
   unsigned next = 0;
   const char *pending_name = NULL;

   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind == AIR_BS_ENTER_BLOCK) {
         air_bs_skip_block(&r->bs);
         continue;
      }
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "type block ends early");

      if (e.id == TYPE_CODE_NUMENTRY) {
         if (e.num_ops < 1)
            return fail(r, "NUMENTRY without a count");
         m->num_types = e.ops[0];
         m->types = rzalloc_array(m, struct air_type, MAX2(m->num_types, 1));
         for (unsigned i = 0; i < m->num_types; i++) {
            m->types[i].id = i;
            m->types[i].kind = AIR_TYPE_UNKNOWN;
         }
         continue;
      }
      if (e.id == TYPE_CODE_STRUCT_NAME) {
         pending_name = air_bs_ops_to_string(m, e.ops, e.num_ops);
         continue;
      }
      if (next >= m->num_types)
         return fail(r, "more types than NUMENTRY said");

      struct air_type *t = &m->types[next];
      switch (e.id) {
      case TYPE_CODE_VOID: t->kind = AIR_TYPE_VOID; break;
      case TYPE_CODE_HALF: t->kind = AIR_TYPE_HALF; break;
      case TYPE_CODE_BFLOAT: t->kind = AIR_TYPE_BFLOAT; break;
      case TYPE_CODE_FLOAT: t->kind = AIR_TYPE_FLOAT; break;
      case TYPE_CODE_DOUBLE: t->kind = AIR_TYPE_DOUBLE; break;
      case TYPE_CODE_LABEL: t->kind = AIR_TYPE_LABEL; break;
      case TYPE_CODE_METADATA: t->kind = AIR_TYPE_METADATA; break;
      case TYPE_CODE_TOKEN: t->kind = AIR_TYPE_TOKEN; break;
      case TYPE_CODE_X86_FP80:
      case TYPE_CODE_FP128:
      case TYPE_CODE_PPC_FP128:
      case TYPE_CODE_X86_MMX:
         t->kind = AIR_TYPE_UNKNOWN;
         break;
      case TYPE_CODE_INTEGER:
         if (e.num_ops < 1)
            return fail(r, "INTEGER type without a width");
         t->kind = AIR_TYPE_INT;
         t->bits = e.ops[0];
         break;
      case TYPE_CODE_POINTER:
         if (e.num_ops < 1)
            return fail(r, "POINTER type without a pointee");
         t->kind = AIR_TYPE_POINTER;
         t->elem = type_by_id(r, e.ops[0]);
         t->addrspace = e.num_ops > 1 ? e.ops[1] : 0;
         break;
      case TYPE_CODE_OPAQUE_POINTER:
         t->kind = AIR_TYPE_POINTER;
         t->addrspace = e.num_ops > 0 ? e.ops[0] : 0;
         break;
      case TYPE_CODE_ARRAY:
      case TYPE_CODE_VECTOR:
         if (e.num_ops < 2)
            return fail(r, "ARRAY/VECTOR type without a size and element");
         t->kind = e.id == TYPE_CODE_ARRAY ? AIR_TYPE_ARRAY : AIR_TYPE_VECTOR;
         t->count = e.ops[0];
         t->elem = type_by_id(r, e.ops[1]);
         if (e.id == TYPE_CODE_VECTOR && e.num_ops > 2 && e.ops[2])
            return fail(r, "scalable vector type");
         break;
      case TYPE_CODE_OPAQUE:
         t->kind = AIR_TYPE_STRUCT;
         t->opaque = true;
         t->name = pending_name;
         pending_name = NULL;
         break;
      case TYPE_CODE_STRUCT_ANON:
      case TYPE_CODE_STRUCT_NAMED:
         if (e.num_ops < 1)
            return fail(r, "STRUCT type without the packed flag");
         t->kind = AIR_TYPE_STRUCT;
         t->packed = e.ops[0];
         t->num_members = e.num_ops - 1;
         t->members = ralloc_array(m, const struct air_type *, MAX2(t->num_members, 1));
         for (unsigned i = 0; i < t->num_members; i++)
            t->members[i] = type_by_id(r, e.ops[1 + i]);
         if (e.id == TYPE_CODE_STRUCT_NAMED) {
            t->name = pending_name;
            pending_name = NULL;
         }
         break;
      case TYPE_CODE_FUNCTION:
         if (e.num_ops < 2)
            return fail(r, "FUNCTION type without vararg and return");
         t->kind = AIR_TYPE_FUNCTION;
         t->vararg = e.ops[0];
         t->elem = type_by_id(r, e.ops[1]);
         t->num_members = e.num_ops - 2;
         t->members = ralloc_array(m, const struct air_type *, MAX2(t->num_members, 1));
         for (unsigned i = 0; i < t->num_members; i++)
            t->members[i] = type_by_id(r, e.ops[2 + i]);
         break;
      default:
         return fail(r, "unknown type record %u", e.id);
      }
      if (m->error[0])
         return false;
      next++;
   }
}

/* -- values ------------------------------------------------------------- */

static void
push_value(struct reader *r, struct air_value *v)
{
   if (r->num_values == r->cap_values) {
      r->cap_values = MAX2(256, r->cap_values * 2);
      r->values = reralloc(r->m, r->values, struct air_value *, r->cap_values);
   }
   v->id = r->num_values;
   r->values[r->num_values++] = v;
}

static struct air_value *
value_by_id(struct reader *r, unsigned id)
{
   if (id >= r->num_values || !r->values[id]) {
      fail(r, "value %u is not defined (%u values)", id, r->num_values);
      return NULL;
   }
   return r->values[id];
}

static int64_t
decode_signed(uint64_t v)
{
   if ((v & 1) == 0)
      return (int64_t)(v >> 1);
   if (v != 1)
      return -(int64_t)(v >> 1);
   return INT64_MIN;
}

static uint64_t
truncate_to(const struct air_type *t, uint64_t v)
{
   const struct air_type *s = air_type_scalar(t);
   if (s->kind == AIR_TYPE_INT && s->bits < 64)
      return v & ((1ull << s->bits) - 1);
   return v;
}

static struct air_constant *
new_constant(struct reader *r, const struct air_type *t, enum air_const_kind k)
{
   struct air_constant *c = rzalloc(r->m, struct air_constant);
   c->base.kind = AIR_VALUE_CONSTANT;
   c->base.type = t;
   c->ckind = k;
   push_value(r, &c->base);
   return c;
}

static void
expr_operands(struct reader *r, struct air_constant *c, unsigned n)
{
   c->num_operands = n;
   c->operands = rzalloc_array(r->m, struct air_value *, MAX2(n, 1));
   c->pending = rzalloc_array(r->m, unsigned, MAX2(n, 1));
}

static bool
read_constants_block(struct reader *r)
{
   const struct air_type *cur = NULL;
   unsigned first = r->num_values;

   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         break;
      if (e.kind == AIR_BS_ENTER_BLOCK) {
         air_bs_skip_block(&r->bs);
         continue;
      }
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "constants block ends early");

      if (e.id == CST_CODE_SETTYPE) {
         if (e.num_ops < 1 || !(cur = type_by_id(r, e.ops[0])))
            return fail(r, "SETTYPE without a type");
         continue;
      }
      if (!cur)
         return fail(r, "constant before SETTYPE");

      struct air_constant *c;
      switch (e.id) {
      case CST_CODE_NULL:
         new_constant(r, cur, AIR_CONST_NULL);
         break;
      case CST_CODE_UNDEF:
         new_constant(r, cur, AIR_CONST_UNDEF);
         break;
      case CST_CODE_POISON:
         new_constant(r, cur, AIR_CONST_POISON);
         break;
      case CST_CODE_INTEGER:
         if (e.num_ops < 1)
            return fail(r, "INTEGER constant without a value");
         c = new_constant(r, cur, AIR_CONST_INT);
         c->u = truncate_to(cur, (uint64_t)decode_signed(e.ops[0]));
         break;
      case CST_CODE_WIDE_INTEGER:
         return fail(r, "integer constant wider than 64 bits");
      case CST_CODE_FLOAT:
         if (e.num_ops < 1)
            return fail(r, "FLOAT constant without a value");
         if (!air_type_is_float(cur))
            return fail(r, "FLOAT constant of a non-float type");
         c = new_constant(r, cur, AIR_CONST_FLOAT);
         c->u = e.ops[0];
         break;
      case CST_CODE_AGGREGATE:
         c = new_constant(r, cur, AIR_CONST_AGGREGATE);
         c->num_elems = e.num_ops;
         c->elems = rzalloc_array(r->m, struct air_value *, MAX2(e.num_ops, 1));
         c->pending = rzalloc_array(r->m, unsigned, MAX2(e.num_ops, 1));
         for (unsigned i = 0; i < e.num_ops; i++)
            c->pending[i] = e.ops[i];
         break;
      case CST_CODE_STRING:
      case CST_CODE_CSTRING:
      case CST_CODE_DATA: {
         unsigned n = e.num_ops + (e.id == CST_CODE_CSTRING);
         c = new_constant(r, cur, AIR_CONST_DATA);
         c->num_elems = n;
         c->data = rzalloc_array(r->m, uint64_t, MAX2(n, 1));
         for (unsigned i = 0; i < e.num_ops; i++)
            c->data[i] = e.ops[i];
         break;
      }
      case CST_CODE_CE_CAST:
         if (e.num_ops < 3)
            return fail(r, "CE_CAST too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_CAST;
         c->expr_sub = e.ops[0];
         expr_operands(r, c, 1);
         c->pending[0] = e.ops[2];
         break;
      case CST_CODE_CE_BINOP:
         if (e.num_ops < 3)
            return fail(r, "CE_BINOP too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_BINOP;
         c->expr_sub = e.ops[0];
         expr_operands(r, c, 2);
         c->pending[0] = e.ops[1];
         c->pending[1] = e.ops[2];
         break;
      case CST_CODE_CE_UNOP:
         if (e.num_ops < 2)
            return fail(r, "CE_UNOP too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_UNOP;
         c->expr_sub = e.ops[0];
         expr_operands(r, c, 1);
         c->pending[0] = e.ops[1];
         break;
      case CST_CODE_CE_GEP_OLD:
      case CST_CODE_CE_INBOUNDS_GEP:
      case CST_CODE_CE_GEP_WITH_INRANGE_INDEX: {
         unsigned i = 0;
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_GEP;
         if (e.id == CST_CODE_CE_GEP_WITH_INRANGE_INDEX || (e.num_ops & 1))
            c->src_type = type_by_id(r, e.ops[i++]);
         if (e.id == CST_CODE_CE_GEP_WITH_INRANGE_INDEX)
            c->inbounds = e.ops[i++] & 1;
         else
            c->inbounds = e.id == CST_CODE_CE_INBOUNDS_GEP;
         if ((e.num_ops - i) & 1)
            return fail(r, "CE_GEP with an odd operand list");
         expr_operands(r, c, (e.num_ops - i) / 2);
         for (unsigned k = 0; i < e.num_ops; i += 2, k++)
            c->pending[k] = e.ops[i + 1]; /* (type, value) pairs */
         break;
      }
      case CST_CODE_CE_SELECT:
         if (e.num_ops < 3)
            return fail(r, "CE_SELECT too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_SELECT;
         expr_operands(r, c, 3);
         for (unsigned i = 0; i < 3; i++)
            c->pending[i] = e.ops[i];
         break;
      case CST_CODE_CE_EXTRACTELT:
         if (e.num_ops < 3)
            return fail(r, "CE_EXTRACTELT too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_EXTRACTELT;
         expr_operands(r, c, 2);
         c->pending[0] = e.ops[1];
         c->pending[1] = e.ops[e.num_ops == 4 ? 3 : 2];
         break;
      case CST_CODE_CE_INSERTELT:
         if (e.num_ops < 3)
            return fail(r, "CE_INSERTELT too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_INSERTELT;
         expr_operands(r, c, 3);
         c->pending[0] = e.ops[0];
         c->pending[1] = e.ops[1];
         c->pending[2] = e.ops[e.num_ops == 4 ? 3 : 2];
         break;
      case CST_CODE_CE_SHUFFLEVEC:
         if (e.num_ops < 3)
            return fail(r, "CE_SHUFFLEVEC too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_SHUFFLEVEC;
         expr_operands(r, c, 3);
         for (unsigned i = 0; i < 3; i++)
            c->pending[i] = e.ops[i];
         break;
      case CST_CODE_CE_CMP:
         if (e.num_ops < 4)
            return fail(r, "CE_CMP too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_CMP;
         c->expr_sub = e.ops[3];
         expr_operands(r, c, 2);
         c->pending[0] = e.ops[1];
         c->pending[1] = e.ops[2];
         break;
      default:
         return fail(r, "unknown constant record %u", e.id);
      }
      if (r->m->error[0])
         return false;
   }

   /* aggregates and expressions may name constants that come after them */
   for (unsigned i = first; i < r->num_values; i++) {
      struct air_constant *c = (struct air_constant *)r->values[i];
      if (!c->pending)
         continue;
      if (c->ckind == AIR_CONST_AGGREGATE) {
         for (unsigned k = 0; k < c->num_elems; k++)
            if (!(c->elems[k] = value_by_id(r, c->pending[k])))
               return false;
      } else {
         for (unsigned k = 0; k < c->num_operands; k++)
            if (!(c->operands[k] = value_by_id(r, c->pending[k])))
               return false;
      }
      c->pending = NULL;
   }
   return true;
}

#include "air_module_metadata.inc"

/* -- module records ----------------------------------------------------- */

static bool
read_globalvar(struct reader *r, const struct air_bs_entry *e)
{
   struct air_module *m = r->m;
   if (r->m->version < 2)
      return fail(r, "module version %u: only bitcode with a string table is read", m->version);
   if (e->num_ops < 8)
      return fail(r, "GLOBALVAR too short");

   struct air_global *g = rzalloc(m, struct air_global);
   g->base.kind = AIR_VALUE_GLOBAL;
   g->strtab_offset = e->ops[0];
   g->strtab_size = e->ops[1];
   const struct air_type *t = type_by_id(r, e->ops[2]);
   if (!t)
      return false;
   uint64_t flags = e->ops[3];
   g->is_const = flags & 1;
   if (flags & 2) {
      g->value_type = t;
      g->addrspace = flags >> 2;
   } else {
      if (t->kind != AIR_TYPE_POINTER || !t->elem)
         return fail(r, "GLOBALVAR of a non-pointer type");
      g->value_type = t->elem;
      g->addrspace = t->addrspace;
   }
   g->base.type = pointer_to(r, g->value_type, g->addrspace);
   g->init_id = e->ops[4];
   g->linkage = e->ops[5];
   g->align = e->ops[6] ? 1u << (e->ops[6] - 1) : 0;
   if (e->ops[7]) {
      if (e->ops[7] - 1 >= m->num_section_names)
         return fail(r, "GLOBALVAR names section %" PRIu64, e->ops[7] - 1);
      g->section = m->section_names[e->ops[7] - 1];
   }
   if (e->num_ops > 10)
      g->unnamed_addr = e->ops[10];
   if (e->num_ops > 11)
      g->externally_initialized = e->ops[11];
   if (e->num_ops > 9 && e->ops[9])
      return fail(r, "thread-local global");

   m->globals = reralloc(m, m->globals, struct air_global *, m->num_globals + 1);
   m->globals[m->num_globals++] = g;
   push_value(r, &g->base);
   return true;
}

static bool
read_function_record(struct reader *r, const struct air_bs_entry *e)
{
   struct air_module *m = r->m;
   if (m->version < 2)
      return fail(r, "module version %u: only bitcode with a string table is read", m->version);
   if (e->num_ops < 8)
      return fail(r, "FUNCTION too short");

   struct air_function *f = rzalloc(m, struct air_function);
   f->base.kind = AIR_VALUE_FUNCTION;
   f->strtab_offset = e->ops[0];
   f->strtab_size = e->ops[1];
   const struct air_type *t = type_by_id(r, e->ops[2]);
   if (!t)
      return false;
   if (t->kind == AIR_TYPE_POINTER && t->elem)
      t = t->elem;
   if (t->kind != AIR_TYPE_FUNCTION)
      return fail(r, "FUNCTION record of a non-function type");
   f->fn_type = t;
   unsigned as = e->num_ops > 18 ? e->ops[18] : 0;
   f->base.type = pointer_to(r, t, as);
   f->cc = e->ops[3];
   f->is_declaration = e->ops[4];
   f->linkage = e->ops[5];
   f->paramattr = e->ops[6];
   f->align = e->ops[7] ? 1u << (e->ops[7] - 1) : 0;
   if (e->ops[8]) {
      if (e->ops[8] - 1 >= m->num_section_names)
         return fail(r, "FUNCTION names section %" PRIu64, e->ops[8] - 1);
      f->section = m->section_names[e->ops[8] - 1];
   }
   if (e->num_ops > 11)
      f->unnamed_addr = e->ops[11];
   if (e->num_ops > 16 && e->ops[16])
      return fail(r, "function with a personality");

   f->num_args = t->num_members;
   f->args = rzalloc_array(m, struct air_value, MAX2(f->num_args, 1));
   for (unsigned i = 0; i < f->num_args; i++) {
      f->args[i].kind = AIR_VALUE_ARGUMENT;
      f->args[i].type = t->members[i];
   }

   m->functions = reralloc(m, m->functions, struct air_function *, m->num_functions + 1);
   m->functions[m->num_functions++] = f;
   push_value(r, &f->base);
   return true;
}

#include "air_module_function.inc"

/* -- the module block --------------------------------------------------- */

static bool
read_module_block(struct reader *r)
{
   struct air_module *m = r->m;
   bool module_md_resolved = false;

   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         break;

      if (e.kind == AIR_BS_ENTER_BLOCK) {
         bool ok = true;
         switch (e.id) {
         case TYPE_BLOCK_ID_NEW:
            air_bs_enter(&r->bs, e.id);
            ok = read_type_block(r);
            break;
         case CONSTANTS_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_constants_block(r);
            break;
         case METADATA_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_metadata_block(r);
            break;
         case METADATA_KIND_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_metadata_kind_block(r);
            break;
         case FUNCTION_BLOCK_ID:
            if (!module_md_resolved) {
               /* module-level values are complete once bodies begin */
               m->num_module_values = r->num_values;
               ok = resolve_md_values(r, 0) && resolve_named_md(r);
               m->num_md = r->num_md;
               module_md_resolved = true;
            }
            air_bs_enter(&r->bs, e.id);
            ok = ok && read_function_block(r);
            break;
         default:
            /* attributes, symbol table, operand bundle tags, sync scopes,
             * use lists: nothing the translation depends on
             */
            ok = air_bs_skip_block(&r->bs);
            break;
         }
         if (!ok)
            return false;
         continue;
      }
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "module block ends early");

      switch (e.id) {
      case MODULE_CODE_VERSION:
         m->version = e.num_ops ? e.ops[0] : 0;
         break;
      case MODULE_CODE_TRIPLE:
         m->triple = air_bs_ops_to_string(m, e.ops, e.num_ops);
         break;
      case MODULE_CODE_DATALAYOUT:
         m->datalayout = air_bs_ops_to_string(m, e.ops, e.num_ops);
         break;
      case MODULE_CODE_SOURCE_FILENAME:
         m->source_filename = air_bs_ops_to_string(m, e.ops, e.num_ops);
         break;
      case MODULE_CODE_SECTIONNAME:
         m->section_names = reralloc(m, m->section_names, const char *, m->num_section_names + 1);
         m->section_names[m->num_section_names++] = air_bs_ops_to_string(m, e.ops, e.num_ops);
         break;
      case MODULE_CODE_GLOBALVAR:
         if (!read_globalvar(r, &e))
            return false;
         break;
      case MODULE_CODE_FUNCTION:
         if (!read_function_record(r, &e))
            return false;
         break;
      case MODULE_CODE_VSTOFFSET:
      case MODULE_CODE_HASH:
      case MODULE_CODE_GCNAME:
         break;
      default:
         return fail(r, "unsupported module record %u", e.id);
      }
   }

   if (!module_md_resolved) {
      m->num_module_values = r->num_values;
      if (!resolve_md_values(r, 0) || !resolve_named_md(r))
         return false;
      m->num_md = r->num_md;
   }
   /* (the arrays may have moved while function bodies grew them) */
   m->module_values = r->values;
   m->md = r->md;

   /* global initializers name module-level constants */
   for (unsigned i = 0; i < m->num_globals; i++) {
      struct air_global *g = m->globals[i];
      if (g->init_id && !(g->init = value_by_id(r, g->init_id - 1)))
         return false;
   }
   if (r->num_bodies_read != 0) {
      unsigned bodies = 0;
      for (unsigned i = 0; i < m->num_functions; i++)
         bodies += !m->functions[i]->is_declaration;
      if (bodies != r->num_bodies_read)
         return fail(r, "%u function bodies for %u definitions", r->num_bodies_read, bodies);
   }
   return true;
}

static bool
read_strtab_block(struct reader *r)
{
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "string table ends early");
      if (e.id == 1 && e.blob) {
         r->strtab = e.blob;
         r->strtab_size = e.blob_size;
      }
   }
}

static bool
read_identification_block(struct reader *r)
{
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "identification block ends early");
      if (e.id == 1)
         r->m->identification = air_bs_ops_to_string(r->m, e.ops, e.num_ops);
   }
}

static const char *
strtab_name(struct reader *r, uint64_t off, uint64_t size)
{
   if (!r->strtab || off + size > r->strtab_size) {
      fail(r, "name at %" PRIu64 "+%" PRIu64 " outside the string table", off, size);
      return NULL;
   }
   return ralloc_strndup(r->m, (const char *)r->strtab + off, size);
}

struct air_module *
air_module_read(void *mem_ctx, const void *data, size_t size, const char **error)
{
   struct air_module *m = rzalloc(mem_ctx, struct air_module);
   struct reader r = { .m = m };
   m->mem_ctx = m;

   const uint8_t *p = data;
   if (size >= 20 && p[0] == 0xde && p[1] == 0xc0 && p[2] == 0x17 && p[3] == 0x0b) {
      uint32_t off = p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24;
      uint32_t len = p[12] | p[13] << 8 | p[14] << 16 | (uint32_t)p[15] << 24;
      if ((uint64_t)off + len > size) {
         fail(&r, "bitcode wrapper points past the data");
         goto out;
      }
      p += off;
      size = len;
   }

   if (!air_bs_init(&r.bs, m, p, size)) {
      fail(&r, "%s", r.bs.error);
      goto out;
   }

   bool seen_module = false;
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r.bs);
      if (e.kind == AIR_BS_END)
         break;
      if (e.kind != AIR_BS_ENTER_BLOCK) {
         check_bs(&r) || fail(&r, "a record at the top level");
         goto out;
      }
      bool ok;
      switch (e.id) {
      case MODULE_BLOCK_ID:
         if (seen_module) {
            fail(&r, "two modules in one bitcode file");
            goto out;
         }
         seen_module = true;
         air_bs_enter(&r.bs, e.id);
         ok = read_module_block(&r);
         break;
      case STRTAB_BLOCK_ID:
         air_bs_enter(&r.bs, e.id);
         ok = read_strtab_block(&r);
         break;
      case IDENTIFICATION_BLOCK_ID:
         air_bs_enter(&r.bs, e.id);
         ok = read_identification_block(&r);
         break;
      default:
         ok = air_bs_skip_block(&r.bs); /* SYMTAB */
         break;
      }
      if (!ok)
         goto out;
   }
   if (!seen_module) {
      fail(&r, "no module block");
      goto out;
   }

   /* the string table comes after the module, so names come last */
   for (unsigned i = 0; i < m->num_globals; i++)
      if (!(m->globals[i]->base.name =
               strtab_name(&r, m->globals[i]->strtab_offset, m->globals[i]->strtab_size)))
         goto out;
   for (unsigned i = 0; i < m->num_functions; i++)
      if (!(m->functions[i]->base.name =
               strtab_name(&r, m->functions[i]->strtab_offset, m->functions[i]->strtab_size)))
         goto out;

out:
   if (m->error[0]) {
      if (error)
         *error = ralloc_strdup(mem_ctx, m->error);
      ralloc_free(m);
      return NULL;
   }
   if (error)
      *error = NULL;
   return m;
}

const struct air_function *
air_module_find_function(const struct air_module *m, const char *name)
{
   for (unsigned i = 0; i < m->num_functions; i++)
      if (m->functions[i]->base.name && !strcmp(m->functions[i]->base.name, name))
         return m->functions[i];
   return NULL;
}

const char *
air_md_string(const struct air_md *md)
{
   if (!md || md->kind != AIR_MD_STRING)
      return NULL;
   /* strings from METADATA_STRINGS point into the blob and are not terminated */
   return ralloc_strndup((void *)md, md->str, md->len);
}
