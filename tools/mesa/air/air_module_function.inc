/* Bitcode function body reader, included into air_module.c. */

/* -- function bodies ---------------------------------------------------- */

struct rec {
   const struct air_bs_entry *e;
   unsigned i;
   bool bad;
};

static uint64_t
take(struct rec *rc)
{
   if (rc->i >= rc->e->num_ops) {
      rc->bad = true;
      return 0;
   }
   return rc->e->ops[rc->i++];
}

static bool
more(const struct rec *rc)
{
   return rc->i < rc->e->num_ops;
}

/* a relative operand without a type (the type is known from context) */
static unsigned
take_value(struct reader *r, struct rec *rc)
{
   return (uint32_t)r->num_values - (uint32_t)take(rc);
}

/* a relative operand that carries its type when it is a forward reference */
static unsigned
take_value_type(struct reader *r, struct rec *rc)
{
   unsigned id = (uint32_t)r->num_values - (uint32_t)take(rc);
   if (id >= r->num_values) {
      const struct air_type *t = type_by_id(r, take(rc));
      if (id >= r->cap_fwd) {
         unsigned cap = MAX2(id + 1, MAX2(64, r->cap_fwd * 2));
         r->fwd = rerzalloc(r->m, r->fwd, const struct air_type *, r->cap_fwd, cap);
         r->cap_fwd = cap;
      }
      r->fwd[id] = t;
   }
   return id;
}

static struct air_inst *
new_inst(struct reader *r, enum air_opcode op, const struct air_type *type, unsigned num_operands)
{
   struct air_inst *in = rzalloc(r->m, struct air_inst);
   in->base.kind = AIR_VALUE_INSTRUCTION;
   in->base.type = type;
   in->op = op;
   in->num_operands = num_operands;
   in->operands = rzalloc_array(r->m, struct air_value *, MAX2(num_operands, 1));
   in->operand_ids = rzalloc_array(r->m, unsigned, MAX2(num_operands, 1));
   return in;
}

static const struct air_type *
type_of_id(struct reader *r, unsigned id)
{
   if (id < r->num_values && r->values[id])
      return r->values[id]->type;
   if (id < r->cap_fwd && r->fwd[id])
      return r->fwd[id];
   fail(r, "operand %u has no type (a forward reference whose record gave none)", id);
   return NULL;
}

static unsigned
decode_align(uint64_t v)
{
   unsigned log2 = v & 31;
   return log2 ? 1u << (log2 - 1) : 0;
}

static const struct air_type *
gep_result_type(struct reader *r, const struct air_type *src, const struct air_type *ptr_type,
                struct air_inst *in, unsigned first_index)
{
   /* the result is a pointer in the base's address space; its element type
    * (for printing typed pointers) is what the indices reach
    */
   /* index 0 steps over the pointer; the ones after it go into the type */
   const struct air_type *t = src;
   for (unsigned i = first_index + 2; i < in->num_operands && t; i++) {
      if (t->kind == AIR_TYPE_STRUCT) {
         unsigned id = in->operand_ids[i];
         uint64_t n;
         if (id >= r->num_values || !r->values[id] || !air_const_uint(r->values[id], &n) ||
             n >= t->num_members)
            return NULL;
         t = t->members[n];
      } else if (t->kind == AIR_TYPE_ARRAY || t->kind == AIR_TYPE_VECTOR) {
         t = t->elem;
      } else {
         return NULL;
      }
   }
   unsigned as = air_type_scalar(ptr_type)->addrspace;
   if (ptr_type->kind == AIR_TYPE_VECTOR)
      return NULL; /* vector GEPs: not in AIR */
   return pointer_to(r, t, as);
}

static bool
add_inst(struct reader *r, struct air_block **block, struct air_inst *in)
{
   struct air_block *b = *block;
   if (!b)
      return fail(r, "instruction after the last block");
   if (b->num_insts == b->cap_insts) {
      b->cap_insts = MAX2(16, b->cap_insts * 2);
      b->insts = reralloc(r->m, b->insts, struct air_inst *, b->cap_insts);
   }
   b->insts[b->num_insts++] = in;
   in->parent = b;

   if (r->num_fn_insts == r->cap_fn_insts) {
      r->cap_fn_insts = MAX2(64, r->cap_fn_insts * 2);
      r->fn_insts = reralloc(r->m, r->fn_insts, struct air_inst *, r->cap_fn_insts);
   }
   r->fn_insts[r->num_fn_insts++] = in;

   if (in->base.type && in->base.type->kind != AIR_TYPE_VOID)
      push_value(r, &in->base);

   /* terminators move on to the next block */
   if (in->op == AIR_OP_RET || in->op == AIR_OP_BR || in->op == AIR_OP_SWITCH ||
       in->op == AIR_OP_UNREACHABLE) {
      struct air_function *f = r->cur_fn;
      unsigned next = b->index + 1;
      *block = next < f->num_blocks ? &f->blocks[next] : NULL;
   }
   return true;
}

static bool
read_instruction(struct reader *r, const struct air_bs_entry *e, struct air_block **block)
{
   struct air_module *m = r->m;
   struct rec rc = { .e = e };
   struct air_inst *in = NULL;
   unsigned a, b, c;

   switch (e->id) {
   case FUNC_CODE_INST_BINOP: {
      a = take_value_type(r, &rc);
      b = take_value(r, &rc);
      unsigned opc = take(&rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_BINOP, t, 2);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      in->sub = opc;
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_UNOP: {
      a = take_value_type(r, &rc);
      unsigned opc = take(&rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_UNOP, t, 1);
      in->operand_ids[0] = a;
      in->sub = opc;
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_CAST: {
      a = take_value_type(r, &rc);
      const struct air_type *t = type_by_id(r, take(&rc));
      unsigned opc = take(&rc);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_CAST, t, 1);
      in->operand_ids[0] = a;
      in->sub = opc;
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_GEP: {
      bool inbounds = take(&rc) & 1;
      const struct air_type *src = type_by_id(r, take(&rc));
      unsigned ids[64], n = 0;
      while (more(&rc) && n < ARRAY_SIZE(ids))
         ids[n++] = take_value_type(r, &rc);
      if (!src || n == 0 || more(&rc))
         return fail(r, "GEP of %u operands", n);
      const struct air_type *pt = type_of_id(r, ids[0]);
      if (!pt)
         return false;
      in = new_inst(r, AIR_OP_GEP, NULL, n);
      memcpy(in->operand_ids, ids, n * sizeof(unsigned));
      in->inbounds = inbounds;
      in->aux_type = src;
      in->base.type = gep_result_type(r, src, pt, in, 0);
      if (!in->base.type)
         return fail(r, "GEP whose result type cannot be worked out");
      break;
   }
   case FUNC_CODE_INST_SELECT:
   case FUNC_CODE_INST_VSELECT: {
      a = take_value_type(r, &rc);
      b = take_value(r, &rc);
      c = e->id == FUNC_CODE_INST_VSELECT ? take_value_type(r, &rc) : take_value(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_SELECT, t, 3);
      in->operand_ids[0] = c; /* condition first, like LLVM's operand order */
      in->operand_ids[1] = a;
      in->operand_ids[2] = b;
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_EXTRACTELT: {
      a = take_value_type(r, &rc);
      b = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      if (t->kind != AIR_TYPE_VECTOR)
         return fail(r, "extractelement of a non-vector");
      in = new_inst(r, AIR_OP_EXTRACTELT, t->elem, 2);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      break;
   }
   case FUNC_CODE_INST_INSERTELT: {
      a = take_value_type(r, &rc);
      b = take_value(r, &rc);
      c = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_INSERTELT, t, 3);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      in->operand_ids[2] = c;
      break;
   }
   case FUNC_CODE_INST_SHUFFLEVEC: {
      a = take_value_type(r, &rc);
      b = take_value(r, &rc);
      c = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      const struct air_type *mt = type_of_id(r, c);
      if (!t || !mt)
         return false;
      if (t->kind != AIR_TYPE_VECTOR || mt->kind != AIR_TYPE_VECTOR)
         return fail(r, "shufflevector of non-vectors");
      /* the result has the mask's length and the inputs' element */
      const struct air_type *rt = NULL;
      for (unsigned i = 0; i < m->num_types; i++) {
         const struct air_type *x = &m->types[i];
         if (x->kind == AIR_TYPE_VECTOR && x->elem == t->elem && x->count == mt->count) {
            rt = x;
            break;
         }
      }
      if (!rt)
         return fail(r, "shufflevector result type not in the type table");
      in = new_inst(r, AIR_OP_SHUFFLEVEC, rt, 3);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      in->operand_ids[2] = c;
      break;
   }
   case FUNC_CODE_INST_CMP:
   case FUNC_CODE_INST_CMP2: {
      a = take_value_type(r, &rc);
      b = take_value(r, &rc);
      unsigned pred = take(&rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      /* i1, or a vector of i1 as long as the operands */
      const struct air_type *rt = NULL;
      for (unsigned i = 0; i < m->num_types; i++) {
         const struct air_type *x = &m->types[i];
         if (t->kind == AIR_TYPE_VECTOR) {
            if (x->kind == AIR_TYPE_VECTOR && x->count == t->count && x->elem->kind == AIR_TYPE_INT &&
                x->elem->bits == 1) {
               rt = x;
               break;
            }
         } else if (x->kind == AIR_TYPE_INT && x->bits == 1) {
            rt = x;
            break;
         }
      }
      if (!rt)
         return fail(r, "compare result type not in the type table");
      in = new_inst(r, AIR_OP_CMP, rt, 2);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      in->sub = pred;
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_RET:
      if (e->num_ops == 0) {
         in = new_inst(r, AIR_OP_RET, NULL, 0);
      } else {
         a = take_value_type(r, &rc);
         in = new_inst(r, AIR_OP_RET, NULL, 1);
         in->operand_ids[0] = a;
      }
      break;
   case FUNC_CODE_INST_BR:
      if (e->num_ops == 1) {
         in = new_inst(r, AIR_OP_BR, NULL, 0);
         in->num_targets = 1;
         in->target_ids = ralloc_array(m, unsigned, 1);
         in->target_ids[0] = take(&rc);
      } else if (e->num_ops == 3) {
         in = new_inst(r, AIR_OP_BR, NULL, 1);
         in->num_targets = 2;
         in->target_ids = ralloc_array(m, unsigned, 2);
         in->target_ids[0] = take(&rc);
         in->target_ids[1] = take(&rc);
         in->operand_ids[0] = take_value(r, &rc);
      } else {
         return fail(r, "br of %u operands", e->num_ops);
      }
      break;
   case FUNC_CODE_INST_SWITCH: {
      /* [ type of the condition, condition, default block,
       *   (case constant, block)* ]
       *
       * The condition is a RELATIVE value number and each case constant is an
       * ABSOLUTE one, because that is what the writer does -- pushValue for
       * the condition, getValueID for each case value. Reading them the same
       * way gives the wrong constant for every case in any function that has
       * more than a handful of values, which is every function here.
       *
       * The blocks are block numbers, as br's are, not value numbers.
       */
      if (e->num_ops < 3 || (e->num_ops - 3) % 2)
         return fail(r, "a switch of %u operands", e->num_ops);
      unsigned ty = take(&rc);
      /* LLVM 3.0 wrote switches over ranges, with the case count and a bit
       * width packed into the first operand under this magic instead of a type
       * id. Nothing Apple's compiler emits uses it, and reading it as a type
       * would silently take the wrong path, so it is named and refused.
       */
      if ((ty >> 16) == 0x4B5)
         return fail(r, "a switch in LLVM 3.0's range form (SWITCH_INST_MAGIC)");
      unsigned cond = take_value(r, &rc);
      unsigned deflt = take(&rc);
      unsigned n = (e->num_ops - 3) / 2;
      in = new_inst(r, AIR_OP_SWITCH, NULL, 1 + n);
      in->num_targets = 1 + n;
      in->target_ids = ralloc_array(m, unsigned, 1 + n);
      /* target 0 and operand 0 are the default edge and the condition; the
       * cases follow, so case k is operand k+1 and target k+1
       */
      in->target_ids[0] = deflt;
      in->operand_ids[0] = cond;
      for (unsigned k = 0; k < n; k++) {
         in->operand_ids[1 + k] = take(&rc);
         in->target_ids[1 + k] = take(&rc);
      }
      break;
   }
   case FUNC_CODE_INST_UNREACHABLE:
      in = new_inst(r, AIR_OP_UNREACHABLE, NULL, 0);
      break;
   case FUNC_CODE_INST_PHI: {
      const struct air_type *t = type_by_id(r, take(&rc));
      if (!t)
         return false;
      unsigned n = (e->num_ops - 1) / 2;
      in = new_inst(r, AIR_OP_PHI, t, n);
      in->num_targets = n;
      in->target_ids = ralloc_array(m, unsigned, MAX2(n, 1));
      for (unsigned i = 0; i < n; i++) {
         int64_t rel = decode_signed(take(&rc));
         in->operand_ids[i] = (uint32_t)((int64_t)r->num_values - rel);
         in->target_ids[i] = take(&rc);
      }
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_ALLOCA: {
      uint64_t ty = take(&rc), opty = take(&rc), size = take(&rc), align = take(&rc);
      (void)opty;
      const struct air_type *t = type_by_id(r, ty);
      if (!t)
         return false;
      bool explicit_type = align & (1u << 6);
      const struct air_type *allocated = explicit_type ? t : t->elem;
      if (!allocated)
         return fail(r, "alloca without an allocated type");
      unsigned as = more(&rc) ? take(&rc) : 0;
      in = new_inst(r, AIR_OP_ALLOCA, pointer_to(r, allocated, as), 1);
      in->aux_type = allocated;
      in->operand_ids[0] = size; /* absolute, unlike other operands */
      in->align = ((align & 31) | ((align >> 8) << 5)) ? 1u << (((align & 31) | ((align >> 8) << 5)) - 1) : 0;
      break;
   }
   case FUNC_CODE_INST_LOAD: {
      a = take_value_type(r, &rc);
      const struct air_type *t;
      if (rc.e->num_ops - rc.i == 3) {
         t = type_by_id(r, take(&rc));
      } else {
         const struct air_type *pt = type_of_id(r, a);
         t = pt ? pt->elem : NULL;
      }
      if (!t)
         return fail(r, "load of unknown type");
      in = new_inst(r, AIR_OP_LOAD, t, 1);
      in->operand_ids[0] = a;
      in->align = decode_align(take(&rc));
      in->is_volatile = take(&rc);
      break;
   }
   case FUNC_CODE_INST_STORE: {
      a = take_value_type(r, &rc); /* the pointer */
      b = take_value_type(r, &rc); /* the value */
      in = new_inst(r, AIR_OP_STORE, NULL, 2);
      in->operand_ids[0] = b; /* value first, as LLVM orders them */
      in->operand_ids[1] = a;
      in->align = decode_align(take(&rc));
      in->is_volatile = take(&rc);
      break;
   }
   case FUNC_CODE_INST_EXTRACTVAL: {
      a = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      unsigned n = e->num_ops - rc.i;
      uint32_t *idx = ralloc_array(m, uint32_t, MAX2(n, 1));
      for (unsigned i = 0; i < n; i++) {
         idx[i] = take(&rc);
         if (t->kind == AIR_TYPE_STRUCT && idx[i] < t->num_members)
            t = t->members[idx[i]];
         else if (t->kind == AIR_TYPE_ARRAY && idx[i] < t->count)
            t = t->elem;
         else
            return fail(r, "extractvalue index out of the aggregate");
      }
      in = new_inst(r, AIR_OP_EXTRACTVAL, t, 1);
      in->operand_ids[0] = a;
      in->num_indices = n;
      in->indices = idx;
      break;
   }
   case FUNC_CODE_INST_INSERTVAL: {
      a = take_value_type(r, &rc);
      b = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      unsigned n = e->num_ops - rc.i;
      in = new_inst(r, AIR_OP_INSERTVAL, t, 2);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      in->num_indices = n;
      in->indices = ralloc_array(m, uint32_t, MAX2(n, 1));
      for (unsigned i = 0; i < n; i++)
         in->indices[i] = take(&rc);
      break;
   }
   case FUNC_CODE_INST_CALL: {
      take(&rc); /* paramattrs: an index into PARAMATTR, nothing the translation uses */
      uint64_t cc = take(&rc);
      unsigned fmf = 0;
      if (cc & (1u << CALL_FMF_BIT))
         fmf = take(&rc);
      const struct air_type *fty = NULL;
      if (cc & (1u << CALL_EXPLICIT_TYPE_BIT))
         fty = type_by_id(r, take(&rc));
      unsigned callee = take_value_type(r, &rc);
      if (!fty) {
         const struct air_type *pt = type_of_id(r, callee);
         fty = pt ? pt->elem : NULL;
      }
      if (!fty || fty->kind != AIR_TYPE_FUNCTION)
         return fail(r, "call without a function type");
      unsigned n = fty->num_members;
      unsigned ids[64];
      if (n >= ARRAY_SIZE(ids))
         return fail(r, "call of %u arguments", n);
      for (unsigned i = 0; i < n; i++) {
         if (fty->members[i]->kind == AIR_TYPE_LABEL)
            return fail(r, "call with a label argument");
         ids[i] = take_value(r, &rc);
      }
      unsigned extra = 0;
      if (fty->vararg) {
         while (more(&rc) && n + extra < ARRAY_SIZE(ids)) {
            ids[n + extra] = take_value_type(r, &rc);
            extra++;
         }
      }
      if (rc.bad || more(&rc))
         return fail(r, "call record does not match its function type");
      in = new_inst(r, AIR_OP_CALL, fty->elem, 1 + n + extra);
      in->operand_ids[0] = callee;
      memcpy(in->operand_ids + 1, ids, (n + extra) * sizeof(unsigned));
      in->aux_type = fty;
      in->tail = cc & (1u << CALL_TAIL_BIT);
      in->flags = fmf;
      in->cc = (cc >> CALL_CCONV_SHIFT) & 0x3ff;
      break;
   }
   case FUNC_CODE_INST_FREEZE: {
      a = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_FREEZE, t, 1);
      in->operand_ids[0] = a;
      break;
   }
   case FUNC_CODE_DEBUG_LOC:
   case FUNC_CODE_DEBUG_LOC_AGAIN:
      return true; /* not instructions */
   case FUNC_CODE_DECLAREBLOCKS:
      return fail(r, "DECLAREBLOCKS twice");
   case FUNC_CODE_OPERAND_BUNDLE:
      return fail(r, "operand bundles");
   default:
      return fail(r, "unsupported instruction record %u", e->id);
   }

   if (rc.bad)
      return fail(r, "instruction record %u too short", e->id);
   r->cur_inst_id++;
   return add_inst(r, block, in);
}

static bool
read_attachments(struct reader *r)
{
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "attachment block ends early");
      if (e.id != METADATA_ATTACHMENT)
         continue;
      if (e.num_ops % 2 == 0)
         continue; /* attachments of the function itself */
      unsigned inst = e.ops[0];
      if (inst >= r->num_fn_insts)
         return fail(r, "attachment to instruction %u of %u", inst, r->num_fn_insts);
      struct air_inst *in = r->fn_insts[inst];
      unsigned n = (e.num_ops - 1) / 2;
      in->attachments = ralloc_array(r->m, __typeof__(*in->attachments), n);
      in->num_attachments = n;
      for (unsigned i = 0; i < n; i++) {
         unsigned id = e.ops[2 + 2 * i];
         if (id >= r->num_md)
            return fail(r, "attachment of !%u of %u", id, r->num_md);
         in->attachments[i].kind = e.ops[1 + 2 * i];
         in->attachments[i].md = r->md[id];
      }
   }
}

static bool
read_function_vst(struct reader *r, unsigned first_local)
{
   struct air_function *f = r->cur_fn;
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "symbol table ends early");
      if (e.num_ops < 1)
         continue;
      const char *name = air_bs_ops_to_string(r->m, e.ops + 1, e.num_ops - 1);
      if (e.id == VST_CODE_ENTRY) {
         if (e.ops[0] < first_local)
            continue;
         struct air_value *v = value_by_id(r, e.ops[0]);
         if (!v)
            return false;
         v->name = name;
      } else if (e.id == VST_CODE_BBENTRY) {
         if (e.ops[0] >= f->num_blocks)
            return fail(r, "name for block %" PRIu64, e.ops[0]);
         f->blocks[e.ops[0]].name = name;
      }
   }
}

static struct air_function *
next_body(struct reader *r)
{
   struct air_module *m = r->m;
   unsigned seen = 0;
   for (unsigned i = 0; i < m->num_functions; i++) {
      if (m->functions[i]->is_declaration)
         continue;
      if (seen++ == r->num_bodies_read) {
         r->num_bodies_read++;
         return m->functions[i];
      }
   }
   return NULL;
}

static bool
read_function_block(struct reader *r)
{
   struct air_function *f = next_body(r);
   if (!f)
      return fail(r, "a function body for no function");

   unsigned module_values = r->num_values;
   unsigned module_md = r->num_md;
   r->cur_fn = f;
   r->cur_inst_id = 0;
   r->num_fn_insts = 0;
   if (r->cap_fwd)
      memset(r->fwd, 0, r->cap_fwd * sizeof(*r->fwd));

   for (unsigned i = 0; i < f->num_args; i++)
      push_value(r, &f->args[i]);
   unsigned first_local = module_values;

   struct air_block *block = NULL;
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         break;
      if (e.kind == AIR_BS_ENTER_BLOCK) {
         bool ok;
         switch (e.id) {
         case CONSTANTS_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_constants_block(r);
            break;
         case METADATA_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_metadata_block(r);
            break;
         case METADATA_ATTACHMENT_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_attachments(r);
            break;
         case VALUE_SYMTAB_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_function_vst(r, first_local);
            break;
         default:
            ok = air_bs_skip_block(&r->bs); /* USELIST and the like */
            break;
         }
         if (!ok)
            return false;
         continue;
      }
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "function block ends early");

      if (e.id == FUNC_CODE_DECLAREBLOCKS) {
         if (e.num_ops < 1 || f->blocks)
            return fail(r, "bad DECLAREBLOCKS");
         f->num_blocks = e.ops[0];
         f->blocks = rzalloc_array(r->m, struct air_block, MAX2(f->num_blocks, 1));
         for (unsigned i = 0; i < f->num_blocks; i++)
            f->blocks[i].index = i;
         block = f->num_blocks ? &f->blocks[0] : NULL;
         continue;
      }
      if (!read_instruction(r, &e, &block))
         return false;
   }

   /* operands, now that every value of the function exists */
   for (unsigned i = 0; i < r->num_fn_insts; i++) {
      struct air_inst *in = r->fn_insts[i];
      for (unsigned k = 0; k < in->num_operands; k++) {
         if (in->op == AIR_OP_CALL && k > 0 && k - 1 < in->aux_type->num_members &&
             in->aux_type->members[k - 1]->kind == AIR_TYPE_METADATA) {
            /* a metadata argument: the id is a metadata id */
            unsigned id = in->operand_ids[k];
            if (id >= r->num_md)
               return fail(r, "metadata argument !%u", id);
            struct air_value *v = rzalloc(r->m, struct air_value);
            v->kind = AIR_VALUE_METADATA;
            v->type = in->aux_type->members[k - 1];
            v->id = id;
            in->operands[k] = v;
            continue;
         }
         if (!(in->operands[k] = value_by_id(r, in->operand_ids[k])))
            return fail(r, "instruction %u of %s: operand %u (value %u) undefined", i,
                        f->base.name ? f->base.name : "?", k, in->operand_ids[k]);
      }
      if (in->num_targets) {
         in->targets = ralloc_array(r->m, struct air_block *, in->num_targets);
         for (unsigned k = 0; k < in->num_targets; k++) {
            if (in->target_ids[k] >= f->num_blocks)
               return fail(r, "branch to block %u of %u", in->target_ids[k], f->num_blocks);
            in->targets[k] = &f->blocks[in->target_ids[k]];
         }
      }
      in->operand_ids = NULL;
      in->target_ids = NULL;
   }
   for (unsigned i = 0; i < f->num_blocks; i++) {
      if (f->blocks[i].num_insts == 0)
         return fail(r, "block %u of a function is empty", i);
   }
   if (!resolve_md_values(r, module_md))
      return false;

   f->num_values = r->num_values - module_values;
   /* the function's values and metadata are its own: the next one starts over */
   r->num_values = module_values;
   r->num_md = module_md;
   r->cur_fn = NULL;
   return true;
}

