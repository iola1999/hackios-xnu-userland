/* CFG traversal, library wrappers, and body translation, included into air_to_nir.c. */

/* -- the CFG ------------------------------------------------------------ */

static nir_block *
nir_block_of(struct air_ctx *c, const struct air_block *b)
{
   struct hash_entry *e = _mesa_hash_table_search(c->blocks, b);
   if (e)
      return e->data;
   nir_block *n = nir_block_create(c->b.impl);
   exec_list_push_tail(&c->b.impl->body, &n->cf_node.node);
   n->cf_node.parent = &c->b.impl->cf_node;
   _mesa_hash_table_insert(c->blocks, b, n);
   return n;
}

static bool emit_callee_agg_return(struct air_ctx *c, const struct air_type *t,
                                   const struct air_val *v, unsigned *fidx);

static bool
emit_return(struct air_ctx *c, const struct air_inst *in)
{
   if (in->num_operands == 0)
      return true;
   struct air_val *v = air_get(c, in->operands[0]);
   if (!v)
      return false;

   /* Inside a callee the return value goes through the out-parameter the
    * caller passed, because a NIR call has no destination. The entry point
    * has no such parameter and its return is its render targets, below.
    */
   if (c->num_ret_fields) {
      unsigned fidx = 0;
      return emit_callee_agg_return(c, c->fn->fn_type->elem, v, &fidx);
   }
   if (c->ret_deref) {
      nir_def *d = air_get_ssa(c, in->operands[0]);
      if (!d)
         return air_error(c, "instruction: a return of a value this cannot put in a "
                             "register, from a function called in the module");
      nir_store_deref(&c->b, c->ret_deref, d, nir_component_mask(d->num_components));
      return true;
   }

   const struct air_type *rt = in->operands[0]->type;
   if (rt->kind == AIR_TYPE_STRUCT) {
      if (v->kind != AIR_VAL_AGG || v->num_elems != rt->num_members)
         return air_error(c, "instruction: a returned struct that was never built");
      for (unsigned i = 0; i < v->num_elems; i++) {
         if (!c->outputs[i].var)
            return air_error(c, "interface: field %u of the result has no output", i);
         if (c->outputs[i].var->data.compact) {
            const struct air_val *a = v->elems[i];
            for (unsigned e = 0; a && a->kind == AIR_VAL_AGG && e < a->num_elems; e++)
               if (a->elems[e] && a->elems[e]->kind == AIR_VAL_SSA)
                  nir_store_deref(&c->b, nir_build_deref_array_imm(&c->b,
                                  nir_build_deref_var(&c->b, c->outputs[i].var), e),
                                  a->elems[e]->def, 1);
            if (a && a->kind == AIR_VAL_SSA)
               for (unsigned e = 0; e < a->def->num_components; e++)
                  nir_store_deref(&c->b, nir_build_deref_array_imm(&c->b,
                                  nir_build_deref_var(&c->b, c->outputs[i].var), e),
                                  nir_channel(&c->b, a->def, e), 1);
            continue;
         }
         if (!v->elems[i] || v->elems[i]->kind != AIR_VAL_SSA)
            continue; /* a field left undef is a target nothing writes */
         nir_store_var(&c->b, c->outputs[i].var, v->elems[i]->def,
                       BITFIELD_MASK(v->elems[i]->def->num_components));
      }
      return true;
   }
   if (v->kind != AIR_VAL_SSA)
      return air_error(c, "instruction: a returned value that is not a number");
   if (!c->num_outputs || !c->outputs[0].var)
      return air_error(c, "interface: a returned value with no output");
   nir_store_var(&c->b, c->outputs[0].var, v->def, BITFIELD_MASK(v->def->num_components));
   return true;
}

/* The blocks in reverse post-order, so that every value is translated before
 * it is used: LLVM lays blocks out in its own order, and a block whose only
 * predecessor comes after it in that order is ordinary (a loop's exit, say).
 * Reverse post-order respects dominance, which is exactly the guarantee the
 * translation needs. Blocks the entry cannot reach are left out, and the phi
 * pass below ignores edges from them.
 */
static unsigned
reverse_post_order(struct air_ctx *c, const struct air_function *f, unsigned *order)
{
   unsigned n = f->num_blocks;
   uint8_t *state = rzalloc_array(c->mem_ctx, uint8_t, MAX2(n, 1)); /* 0 new, 1 open, 2 done */
   unsigned *stack = rzalloc_array(c->mem_ctx, unsigned, MAX2(n, 1));
   unsigned *next_succ = rzalloc_array(c->mem_ctx, unsigned, MAX2(n, 1));
   unsigned sp = 0, num_post = 0;

   stack[sp++] = 0;
   state[0] = 1;
   while (sp) {
      unsigned bi = stack[sp - 1];
      const struct air_block *blk = &f->blocks[bi];
      const struct air_inst *term = blk->insts[blk->num_insts - 1];
      if (next_succ[bi] < term->num_targets) {
         unsigned to = term->targets[next_succ[bi]++]->index;
         if (state[to] == 0) {
            state[to] = 1;
            next_succ[to] = 0;
            stack[sp++] = to;
         }
         continue;
      }
      state[bi] = 2;
      order[num_post++] = bi; /* post-order */
      sp--;
   }
   /* reverse it in place */
   for (unsigned i = 0; i < num_post / 2; i++) {
      unsigned t = order[i];
      order[i] = order[num_post - 1 - i];
      order[num_post - 1 - i] = t;
   }
   return num_post;
}

static bool translate_body(struct air_ctx *c);
static bool inline_return(struct air_ctx *c, const struct air_inst *in);
struct air_linked_function;
static const char *visible_reference_name(struct air_ctx *c, const struct air_function *f);
static const struct air_linked_function *linked_function(struct air_ctx *c, const char *name);
static bool call_linked_function(struct air_ctx *c, const struct air_inst *in,
                                 const struct air_function *decl,
                                 const struct air_linked_function *lf);
static void materialise_globals(struct air_ctx *c);
static struct air_val *inline_module_function(struct air_ctx *c, const struct air_function *f,
                                              struct air_val **argv);

#include "air_call.inc"

/* The air.visible_input node for argument i, or NULL. The nodes carry a
 * parameter number as their first operand rather than being in order, so they
 * are searched by it.
 */
static const struct air_md *
arg_md(const struct air_md *inputs, unsigned i)
{
   for (unsigned k = 0; k < (inputs ? inputs->num_ops : 0); k++) {
      const struct air_md *n = inputs->ops[k];
      uint64_t which;
      if (n && md_int(n, 0, &which) && which == i)
         return n;
   }
   return NULL;
}

/* An air.visible library function, inside an entry point we generate for it.
 *
 * A library function is not a shader. Metal links one into a caller at
 * pipeline time through the ABI its own metadata describes -- air.stitching_info
 * names the four functions that do it (load_argument, copy_from_buffer,
 * store_return_value, destroy) and gives each argument's size and alignment --
 * and none of that is implemented here: it belongs to the runtime, not to the
 * compiler. So 135 modules of the corpus, a third of it, had nothing that
 * could be dispatched and were counted only as refused.
 *
 * This wraps one instead: argument i is read from device buffer 0 at byte
 * 16*i, the function is called, and the return value is written to device
 * buffer 1 at offset 0, by a single thread. The 16-byte slot is Apple's own
 * size and alignment for a float4 (the air.vector_type node beside each
 * stitching argument says 16 and 16), and a fixed slot makes the layout
 * derivable from the signature alone -- which is what lets a harness on this
 * machine fill the same bytes and compare the result.
 *
 * What that measures is the body: its arithmetic, its control flow, its calls.
 * What it does not touch is the stitching ABI, so a wrapped function is
 * reported in a row of its own and never counted among the corpus's entry
 * points.
 */
static bool
wrap_library_function(struct air_ctx *c, const struct air_md *out_md,
                      const struct air_md *in_md)
{
   const struct air_function *f = c->fn;

   /* The result buffer carries the function's own air.arg_type_name ("float4"
    * and the like, from air.visible_output): a harness that fills the argument
    * buffer has to know how to read the bytes that come back, and Apple's name
    * for the type is a better source for that than our reading of the LLVM
    * type -- the two can then be checked against each other.
    */
   add_resource(c, AIR_RESOURCE_DEVICE_BUFFER, 0, 16 * MAX2(f->num_args, 1),
                true, false, ~0u, NULL);
   add_resource(c, AIR_RESOURCE_DEVICE_BUFFER, 1, 16, false, true, ~0u, out_md);

   struct air_val **argv = rzalloc_array(c->mem_ctx, struct air_val *,
                                         MAX2(f->num_args, 1));
   unsigned char *kind = rzalloc_array(c->mem_ctx, unsigned char,
                                       MAX2(f->num_args, 1));

   /* Eleven of the corpus's library functions are CoreImage's sampling helpers,
    * and they take what CoreImage hands a stitched function: a texture, a
    * sampler and a transform. Apple passes the first two as one-field structs
    * named metal::texture2d and metal::sampler -- the field being exactly the
    * address-space-1 or -2 pointer an entry point's texture or sampler
    * argument is -- and the transform as a bare constant-space pointer. Those
    * three shapes are bound rather than loaded: a texture is not a value in
    * this frontend (section 7.4), so the wrapper resolves each to a binding
    * and the harness binds its own texture, sampler and constant buffer there.
    *
    * Keyed on the struct's name, which is Apple's and is in these modules, and
    * not on "a struct with one pointer in it": the second would quietly accept
    * anything shaped alike.
    */
   unsigned next_tex = 0, next_samp = 0, next_ubo = 0;
   for (unsigned i = 0; i < f->num_args; i++) {
      const struct air_type *t = f->args[i].type;
      const char *sname = t->kind == AIR_TYPE_STRUCT ? t->name : NULL;
      bool is_tex = sname && !strncmp(sname, "struct.metal::texture", 21);
      bool is_samp = sname && !strcmp(sname, "struct.metal::sampler");

      if (is_tex || is_samp) {
         if (t->num_members != 1)
            return air_error(c, "library: %s has %u members, not the one pointer this "
                                "expects", sname, t->num_members);
         struct air_ptr p = {
            .class = is_tex ? AIR_PTR_TEXTURE : AIR_PTR_SAMPLER,
            .binding = is_tex ? next_tex : next_samp,
         };
         add_resource(c, is_tex ? AIR_RESOURCE_TEXTURE : AIR_RESOURCE_SAMPLER,
                      p.binding, 0, true, false, i, arg_md(in_md, i));
         if (is_tex)
            next_tex++;
         else
            next_samp++;
         /* the value is the struct, so the pointer goes inside an aggregate of
          * one: the body reaches it with extractvalue
          */
         struct air_val *agg = air_val_agg(c, 1);
         agg->elems[0] = air_val_ptr(c, p);
         argv[i] = agg;
         kind[i] = CALL_PARAM_NONE;
         continue;
      }

      if (t->kind == AIR_TYPE_POINTER && t->addrspace == 2) {
         /* the transform, as a constant buffer the harness fills */
         struct air_ptr p = { .class = AIR_PTR_UBO, .binding = next_ubo,
                              .pointee = t->elem };
         /* with the argument's own metadata, so the resource carries Apple's
          * name for it -- "float4x4" for the transform these functions take,
          * which is what lets the harness put something meaningful there
          * rather than the byte pattern
          */
         add_resource(c, AIR_RESOURCE_CONSTANT_BUFFER, next_ubo++, 256, true, false,
                      i, arg_md(in_md, i));
         argv[i] = air_val_ptr(c, p);
         kind[i] = CALL_PARAM_NONE;
         continue;
      }

      const struct air_type *e = t->kind == AIR_TYPE_VECTOR ? t->elem : t;
      bool numeric = e && (e->kind == AIR_TYPE_FLOAT || e->kind == AIR_TYPE_HALF ||
                           e->kind == AIR_TYPE_INT);
      if (!numeric || (t->kind != AIR_TYPE_VECTOR && t->kind != AIR_TYPE_FLOAT &&
                       t->kind != AIR_TYPE_HALF && t->kind != AIR_TYPE_INT))
         return air_error(c, "library: argument %u is none of a scalar, a vector, a "
                             "metal::texture, a metal::sampler or a constant pointer, "
                             "which is all this wrapper can supply", i);
      if (air_nir_components(t) * air_nir_bit_size(c, t) > 16 * 8)
         return air_error(c, "library: argument %u does not fit the wrapper's 16-byte "
                             "slot", i);

      struct air_ptr p = { .class = AIR_PTR_SSBO, .binding = 0, .pointee = t };
      nir_def *v = air_load_ptr(c, air_ptr_add(c, p, nir_imm_int(&c->b, 16 * i)), t, 16);
      if (!v)
         return false;
      argv[i] = air_val_ssa(c, v);
      kind[i] = CALL_PARAM_VALUE;
   }

   const struct air_type *rt = f->fn_type->elem;
   const struct air_type *re = rt && rt->kind == AIR_TYPE_VECTOR ? rt->elem : rt;
   if (!rt || rt->kind == AIR_TYPE_VOID)
      return air_error(c, "library: the function returns nothing, so a wrapper would "
                          "have nothing to write");
   if (!re || (re->kind != AIR_TYPE_FLOAT && re->kind != AIR_TYPE_HALF &&
               re->kind != AIR_TYPE_INT))
      return air_error(c, "library: the return value is neither a scalar nor a vector");

   nir_function *fn = translate_callee(c, f, argv, kind);
   if (!fn)
      return false;   /* translate_callee has said why */

   unsigned bits = air_nir_bit_size(c, rt);
   unsigned comps = air_nir_components(rt);
   const struct glsl_type *gt =
      glsl_vector_type(bits == 16 ? GLSL_TYPE_FLOAT16 :
                       bits == 64 ? GLSL_TYPE_UINT64 : GLSL_TYPE_UINT, comps);
   nir_variable *tmp = nir_local_variable_create(c->b.impl, gt, "libret");

   nir_call_instr *call = nir_call_instr_create(c->nir, fn);
   unsigned idx = 0;
   call->params[idx++] = nir_src_for_ssa(&nir_build_deref_var(&c->b, tmp)->def);
   for (unsigned i = 0; i < f->num_args; i++) {
      /* only the arguments translate_callee made parameters: a texture, a
       * sampler or a constant pointer was bound into the callee instead, and
       * has no SSA def to pass. Passing one anyway inserts a call whose source
       * has no value, which NIR dereferences while adding uses -- a segfault
       * inside nir_instr_insert rather than a message.
       */
      if (kind[i] != CALL_PARAM_VALUE)
         continue;
      nir_def *a = argv[i] ? argv[i]->def : NULL;
      if (!a)
         return air_error(c, "library: argument %u has no value to pass", i);
      call->params[idx++] = nir_src_for_ssa(a);
   }
   nir_builder_instr_insert(&c->b, &call->instr);

   struct air_ptr out = { .class = AIR_PTR_SSBO, .binding = 1, .pointee = rt };
   if (!air_store_ptr(c, out, nir_load_var(&c->b, tmp), 16))
      return false;

   /* the generated block ends the way a translated one does: this impl is
    * unstructured until nir_lower_goto_ifs runs over it
    */
   nir_goto(&c->b, c->b.impl->end_block);
   return true;
}

#include "air_ptr_phi.inc"

static bool
translate_body(struct air_ctx *c)
{
   const struct air_function *f = c->fn;

   /* phis first: each becomes a register, loaded where the phi is and stored
    * at the end of every predecessor (docs/air.md: NIR's own
    * nir_lower_goto_ifs does the same to make an unstructured CFG safe) --
    * except a phi of POINTERS, which has no bits and is resolved where it
    * stands (resolve_pointer_phi). It gets no register, and the store loop at
    * the end skips a phi that has none.
    */
   nir_builder entry = nir_builder_at(nir_before_impl(c->b.impl));
   for (unsigned bi = 0; bi < f->num_blocks; bi++) {
      const struct air_block *blk = &f->blocks[bi];
      for (unsigned i = 0; i < blk->num_insts; i++) {
         const struct air_inst *in = blk->insts[i];
         if (in->op != AIR_OP_PHI || in->base.type->kind == AIR_TYPE_POINTER)
            continue;
         /* A phi of an AGGREGATE, named here rather than by whatever notices
          * the consequence further down. A struct is a tree of values in this
          * translator and never a NIR value, so there is no register to carry
          * one across an edge; the honest form would be one phi per member,
          * which this does not build. Until it was named, a register of ZERO
          * bits was declared for it and the module failed later with
          * "a returned struct that was never built" -- true of the module, and
          * not what is wrong with it.
          */
         if (in->base.type->kind == AIR_TYPE_STRUCT ||
             in->base.type->kind == AIR_TYPE_ARRAY) {
            if (agg_type_has_pointer(in->base.type))
               return air_error(c, "instruction: a phi of %s containing a pointer",
                                in->base.type->kind == AIR_TYPE_STRUCT ? "a struct"
                                                                       : "an array");
            _mesa_hash_table_insert(c->phi_regs, in,
                                    decl_agg_phi_regs(c, &entry, in->base.type));
            continue;
         }
         nir_def *reg = nir_decl_reg(&entry, air_nir_components(in->base.type),
                                     air_nir_bit_size(c, in->base.type), 0);
         _mesa_hash_table_insert(c->phi_regs, in, reg);
      }
   }

   /* NIR's first block is the function's first; the rest are made as they
    * are reached, and nir_lower_goto_ifs puts them in order afterwards
    */
   _mesa_hash_table_insert(c->blocks, &f->blocks[0],
                           c->inline_entry ? c->inline_entry : nir_start_block(c->b.impl));

   unsigned *order = rzalloc_array(c->mem_ctx, unsigned, MAX2(f->num_blocks, 1));
   unsigned num_order = reverse_post_order(c, f, order);
   bool *reachable = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_blocks, 1));
   for (unsigned oi = 0; oi < num_order; oi++)
      reachable[order[oi]] = true;

   bool is_entrypoint = (c->fn == c->entry_fn && !c->inline_entry);
   for (unsigned oi = 0; oi < num_order; oi++) {
      unsigned bi = order[oi];
      c->in_entry_block = is_entrypoint && (bi == 0);
      const struct air_block *blk = &f->blocks[bi];
      nir_block *nb = nir_block_of(c, blk);
      c->b.cursor = nir_after_block(nb);

      /* the phis of this block are its first instructions */
      for (unsigned i = 0; i < blk->num_insts; i++) {
         const struct air_inst *in = blk->insts[i];
         if (in->op != AIR_OP_PHI)
            continue;
         if (in->base.type->kind == AIR_TYPE_POINTER) {
            if (!resolve_pointer_phi(c, in, reachable))
               return false;
            continue;
         }
         struct hash_entry *e = _mesa_hash_table_search(c->phi_regs, in);
         if (in->base.type->kind == AIR_TYPE_STRUCT ||
             in->base.type->kind == AIR_TYPE_ARRAY) {
            air_set(c, &in->base, load_agg_phi_regs(c, in->base.type, e->data));
            continue;
         }
         air_set(c, &in->base,
                 air_val_ssa(c, nir_load_reg(&c->b, e->data)));
      }

      for (unsigned i = 0; i < blk->num_insts; i++) {
         const struct air_inst *in = blk->insts[i];
         if (in->op == AIR_OP_PHI)
            continue;
         /* c->error as well as the return value: a refusal raised by something
          * that cannot report one upwards -- air_nir_bit_size, handed a width
          * NIR does not have -- must stop the walk here rather than let the
          * next instruction be built on top of it.
          */
         if (!translate_instruction(c, in) || c->error[0]) {
            /* say where, so the corpus report points at a line of the module */
            char where[64];
            snprintf(where, sizeof(where), " (block %u, instruction %u, opcode %u)",
                     bi, i, in->op);
            if (strlen(c->error) + strlen(where) < sizeof(c->error))
               strcat(c->error, where);
            return false;
         }
      }

      /* Where the phi stores of this block's successors go. A nop marks the
       * spot: without it, a block whose only instruction is its terminator
       * leaves the cursor at the end of the block, and a store put there
       * lands after the jump -- which NIR asserts on. vtn marks its blocks
       * the same way (vtn_cfg.c's end_nop).
       */
      nir_instr *end = &nir_nop(&c->b)->instr;
      nir_cursor at_end = nir_after_instr(end);
      _mesa_hash_table_insert(c->phi_cursors, blk,
                              ralloc_memdup(c->mem_ctx, &at_end, sizeof(at_end)));

      const struct air_inst *term = blk->insts[blk->num_insts - 1];
      switch (term->op) {
      case AIR_OP_RET:
         if (c->inline_exit) {
            if (!inline_return(c, term))
               return false;
            nir_goto(&c->b, c->inline_exit);
            break;
         }
         if (!emit_return(c, term))
            return false;
         nir_goto(&c->b, c->b.impl->end_block);
         break;
      case AIR_OP_BR:
         if (term->num_targets == 1) {
            nir_goto(&c->b, nir_block_of(c, term->targets[0]));
         } else {
            nir_def *cond = air_get_ssa(c, term->operands[0]);
            if (!cond)
               return false;
            if (term->targets[0] == term->targets[1])
               nir_goto(&c->b, nir_block_of(c, term->targets[0]));
            else
               nir_goto_if(&c->b, nir_block_of(c, term->targets[0]), cond,
                           nir_block_of(c, term->targets[1]));
         }
         break;
      case AIR_OP_SWITCH: {
         /* NIR has no switch, so the cases become a chain of nir_goto_if:
          * each test gets its own block, and the last one falls to the
          * default. nir_lower_goto_ifs structures the result like every other
          * branch this translator emits.
          *
          * What makes the chain safe is where the phi stores already are: all
          * of this block's successors' stores sit at phi_cursors[blk], above
          * the chain, and each successor's phis have registers of their own --
          * so storing on an edge the chain does not take writes a register
          * that the block actually entered never reads. That is the same
          * property the conditional br above relies on, and the reason a
          * switch needs no per-edge bookkeeping.
          */
         nir_def *cond = air_get_ssa(c, term->operands[0]);
         if (!cond)
            return false;
         nir_block *deflt = nir_block_of(c, term->targets[0]);
         unsigned n = term->num_targets - 1;
         for (unsigned k = 0; k < n; k++) {
            nir_def *val = air_get_ssa(c, term->operands[1 + k]);
            if (!val)
               return false;
            nir_block *dest = nir_block_of(c, term->targets[1 + k]);
            bool last = k + 1 == n;
            nir_block *next = deflt;
            if (!last) {
               next = nir_block_create(c->b.impl);
               exec_list_push_tail(&c->b.impl->body, &next->cf_node.node);
               next->cf_node.parent = &c->b.impl->cf_node;
            }
            /* a last case that goes where the default goes is not a branch at
             * all, and nir_goto_if with one block on both sides is what the
             * conditional br above refuses for the same reason
             */
            if (dest == next)
               nir_goto(&c->b, dest);
            else
               nir_goto_if(&c->b, dest, nir_ieq(&c->b, cond, val), next);
            if (!last)
               c->b.cursor = nir_after_block(next);
         }
         if (n == 0)
            nir_goto(&c->b, deflt);
         break;
      }
      case AIR_OP_UNREACHABLE:
         nir_goto(&c->b, c->b.impl->end_block);
         break;
      default:
         c->in_entry_block = false;
         return air_error(c, "instruction: block %u ends with opcode %u", bi, term->op);
      }
   }
   c->in_entry_block = false;

   /* the phi stores, now that every value exists */
   for (unsigned bi = 0; bi < f->num_blocks; bi++) {
      const struct air_block *blk = &f->blocks[bi];
      for (unsigned i = 0; i < blk->num_insts; i++) {
         const struct air_inst *in = blk->insts[i];
         if (in->op != AIR_OP_PHI)
            continue;
         struct hash_entry *re = _mesa_hash_table_search(c->phi_regs, in);
         if (!re)
            continue; /* a phi in an unreachable block, or a pointer phi resolved in place */
         for (unsigned k = 0; k < in->num_operands; k++) {
            struct hash_entry *ce = _mesa_hash_table_search(c->phi_cursors, in->targets[k]);
            if (!ce)
               continue; /* an edge from a block the entry cannot reach */
            c->b.cursor = *(nir_cursor *)ce->data;
            nir_def *val;
            if (in->base.type->kind == AIR_TYPE_POINTER) {
               struct hash_entry *pe = _mesa_hash_table_search(c->values, &in->base);
               struct hash_entry *oe = _mesa_hash_table_search(c->values, in->operands[k]);
               struct air_val *phi = pe ? pe->data : NULL, *from = oe ? oe->data : NULL;
               if (phi && phi->kind == AIR_VAL_PTR_SELECT) {
                  struct air_ptr_phi_reg *r = re->data;
                  nir_def *idx_def = NULL, *off_def = NULL;
                  if (phi_find_place_and_offset(c, r, from, &idx_def, &off_def)) {
                     nir_store_reg(&c->b, idx_def, r->idx_reg);
                     nir_store_reg(&c->b, off_def, r->reg);
                  }
                  *(nir_cursor *)ce->data = c->b.cursor;
                  continue;
               }
               bool phi_is_tex = choosable_texture(phi) ||
                  (phi && phi->kind == AIR_VAL_PTR && phi->ptr.class == AIR_PTR_TEXTURE);
               bool from_is_tex = choosable_texture(from) ||
                  (from && from->kind == AIR_VAL_PTR && from->ptr.class == AIR_PTR_TEXTURE && from->ptr.is_null);
               if (phi_is_tex && from_is_tex) {
                  val = texture_slot(c, &from->ptr);   /* resolve_texture_phi */
               } else {
                  if (!phi || !from || from->kind != AIR_VAL_PTR ||
                      !same_resource(&phi->ptr, &from->ptr))
                     return air_error(c, "instruction: a pointer carried round a loop that "
                                         "names a different resource on the way back");
                  val = from->ptr.offset ? from->ptr.offset : nir_imm_int(&c->b, 0);
               }
            } else if (in->base.type->kind == AIR_TYPE_STRUCT ||
                       in->base.type->kind == AIR_TYPE_ARRAY) {
               struct air_val *agg_from = air_get(c, in->operands[k]);
               if (!agg_from || !store_agg_phi_regs(c, in->base.type, re->data, agg_from))
                  return false;
               *(nir_cursor *)ce->data = c->b.cursor;
               continue;
            } else {
               val = air_get_ssa(c, in->operands[k]);
            }
            if (!val)
               return false;
            nir_store_reg(&c->b, val, re->data);
            /* later stores into the same block go after this one */
            *(nir_cursor *)ce->data = c->b.cursor;
         }
      }
   }
   return true;
}

