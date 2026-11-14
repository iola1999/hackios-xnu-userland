/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

static bool
translate_instruction(struct air_ctx *c, const struct air_inst *in)
{
   switch (in->op) {
   case AIR_OP_BINOP:
      return translate_binop(c, in);
   case AIR_OP_UNOP: {
      nir_def *x = air_get_ssa(c, in->operands[0]);
      if (!x)
         return false;
      air_set(c, &in->base, air_val_ssa(c, nir_fneg(&c->b, x)));
      return true;
   }
   case AIR_OP_CAST:
      return translate_cast(c, in);
   case AIR_OP_CMP:
      return translate_cmp(c, in);
   case AIR_OP_GEP:
      return translate_gep(c, in);
   case AIR_OP_SELECT: {
      /* A select between two POINTERS, which the GL-on-Metal uber shaders use
       * to fall back on a module constant array when the pipeline bound no
       * buffer. It is not a number, so it cannot be a bcsel here; the choice
       * is kept and made where the pointer is loaded through.
       */
      struct air_val *pa = air_get(c, in->operands[1]);
      struct air_val *pb = air_get(c, in->operands[2]);
      if (!pa || !pb)
         return false;
      if (val_is_pointerish(pa) || val_is_pointerish(pb)) {
         if (!val_is_pointerish(pa) || !val_is_pointerish(pb))
            return air_error(c, "instruction: a select between a pointer and "
                                "something that is not one");
         /* A choice between two members of an argument buffer cannot be kept
          * the way a choice between two buffers can: AIR_VAL_PTR_SELECT loads
          * both sides and selects the values, and a texture or a sampler is
          * not a value to load. Two names for ONE member is no choice at all
          * and is answered; anything else is refused rather than drawn over
          * whichever of the two the code happened to take.
          */
         if ((pa->kind == AIR_VAL_PTR && pa->ptr.class == AIR_PTR_ARGBUF) ||
             (pb->kind == AIR_VAL_PTR && pb->ptr.class == AIR_PTR_ARGBUF)) {
            if (pa->kind == AIR_VAL_PTR && pb->kind == AIR_VAL_PTR &&
                same_ptr(&pa->ptr, &pb->ptr)) {
               air_set(c, &in->base, pa);
               return true;
            }
            return air_error(c, "instruction: a select between two different places in "
                                "an argument buffer");
         }
         nir_def *pc = air_get_ssa(c, in->operands[0]);
         if (!pc)
            return false;
         if (pc->num_components != 1)
            return air_error(c, "instruction: a vector select between pointers");
         if (pa->kind == AIR_VAL_PTR && pb->kind == AIR_VAL_PTR &&
             pa->ptr.class == AIR_PTR_SAMPLER && pb->ptr.class == AIR_PTR_SAMPLER) {
            struct air_ptr p = pa->ptr;
            if (!same_ptr(&pa->ptr, &pb->ptr)) {
               nir_def *a = pa->ptr.offset ? pa->ptr.offset : nir_imm_int(&c->b, 0);
               nir_def *b = pb->ptr.offset ? pb->ptr.offset : nir_imm_int(&c->b, 0);
               p.binding = 0;
               p.offset = nir_bcsel(&c->b, pc, nir_iadd_imm(&c->b, a, pa->ptr.binding),
                                    nir_iadd_imm(&c->b, b, pb->ptr.binding));
            }
            air_set(c, &in->base, air_val_ptr(c, p));
            return true;
         }
         if (choosable_texture(pa) && choosable_texture(pb)) {
            struct air_ptr p = pa->ptr;
            if (!same_ptr(&pa->ptr, &pb->ptr)) {
               p.binding = 0;
               p.offset = nir_bcsel(&c->b, pc, texture_slot(c, &pa->ptr),
                                    texture_slot(c, &pb->ptr));
            }
            air_set(c, &in->base, air_val_ptr(c, p));
            return true;
         }
         /* Two places in ONE memory differ only in their byte offset, and an offset is
          * a value: the choice is a bcsel of the offsets and the result an ordinary
          * pointer, which a phi can carry round a loop. CoreAnimation's path vertex
          * shaders binary-search their segment buffer this way. A choice between two
          * buffers still loads both sides.
          */
         if (pa->kind == AIR_VAL_PTR && pb->kind == AIR_VAL_PTR &&
             same_resource(&pa->ptr, &pb->ptr) && offset_is_value(pa->ptr.class)) {
            struct air_ptr p = pa->ptr;
            p.offset = nir_bcsel(&c->b, pc, ptr_offset(c, pa->ptr), ptr_offset(c, pb->ptr));
            air_set(c, &in->base, air_val_ptr(c, p));
            return true;
         }
         air_set(c, &in->base, air_val_ptr_select(c, pc, pa, pb));
         return true;
      }
      nir_def *cond = air_get_ssa(c, in->operands[0]);
      nir_def *x = air_get_ssa(c, in->operands[1]);
      nir_def *y = air_get_ssa(c, in->operands[2]);
      if (!cond || !x || !y)
         return false;
      if (cond->num_components > 1 && cond->num_components != x->num_components)
         return air_error(c, "instruction: a select whose condition does not match its values");
      air_set(c, &in->base, air_val_ssa(c, nir_bcsel(&c->b, cond, x, y)));
      return true;
   }
   case AIR_OP_EXTRACTELT: {
      nir_def *v = air_get_ssa(c, in->operands[0]);
      nir_def *i = air_get_ssa(c, in->operands[1]);
      if (!v || !i)
         return false;
      air_set(c, &in->base, air_val_ssa(c, nir_vector_extract(&c->b, v, nir_u2u32(&c->b, i))));
      return true;
   }
   case AIR_OP_INSERTELT: {
      nir_def *v = air_get_ssa(c, in->operands[0]);
      nir_def *x = air_get_ssa(c, in->operands[1]);
      nir_def *i = air_get_ssa(c, in->operands[2]);
      if (!v || !x || !i)
         return false;
      /* A constant index still becomes nir_vector_insert_imm inside the helper; a run-time index
       * becomes one bcsel per component, which is what this port's fragment shaders need: Metal's
       * PBGRA-to-RGBA swizzle reaches here with an index computed in the shader, and refusing it
       * failed the whole pipeline (kernel-12377-attr26-driver-revert, s005.log: "instruction:
       * insertelement at a run-time index (block 6, instruction 1, opcode 10)"). */
      air_set(c, &in->base,
              air_val_ssa(c, nir_vector_insert(&c->b, v, x, nir_u2u32(&c->b, i))));
      return true;
   }
   case AIR_OP_SHUFFLEVEC: {
      nir_def *x = air_get_ssa(c, in->operands[0]);
      nir_def *y = air_get_ssa(c, in->operands[1]);
      struct air_val *mv = air_get(c, in->operands[2]);
      if (!x || !y || !mv)
         return false;
      unsigned n = air_nir_components(in->base.type);
      nir_def *comps[NIR_MAX_VEC_COMPONENTS];
      if (n > NIR_MAX_VEC_COMPONENTS)
         return air_error(c, "instruction: a shuffle of %u components", n);
      /* the mask is a constant vector, or zeroinitializer, or undef */
      const struct air_constant *mk = air_value_as_constant(in->operands[2]);
      for (unsigned i = 0; i < n; i++) {
         int64_t sel = 0;
         bool undef = false;
         if (mk && mk->ckind == AIR_CONST_DATA && i < mk->num_elems) {
            sel = (int32_t)mk->data[i];
         } else if (mk && mk->ckind == AIR_CONST_AGGREGATE && i < mk->num_elems) {
            const struct air_constant *e = air_value_as_constant(mk->elems[i]);
            /* The mask is an aggregate rather than plain data whenever a lane
             * is poison, so <i32 0, i32 poison, ...> -- what a Metal
             * half2-to-half4 widening compiles to -- mixes LLVM's two
             * spellings of a constant integer. air_const_uint reads both; this
             * code used to test ckind == AIR_CONST_INT by hand instead, which
             * is the whole bug.
             */
            uint64_t lane = 0;
            bool have = air_const_uint(mk->elems[i], &lane);
            if (!e || (!have && e->ckind != AIR_CONST_UNDEF &&
                       e->ckind != AIR_CONST_POISON))
               return air_error(c, "instruction: element %u of a shuffle mask is %s",
                                i, air_value_kind_name(mk->elems[i]));
            undef = !have;
            sel = have ? (int32_t)lane : 0;
         } else if (mk && (mk->ckind == AIR_CONST_NULL)) {
            sel = 0;
         } else if (mk && (mk->ckind == AIR_CONST_UNDEF || mk->ckind == AIR_CONST_POISON)) {
            undef = true;
         } else {
            return air_error(c, "instruction: a shuffle mask that is %s, with %u of the "
                                "%u elements it needs",
                             air_value_kind_name(in->operands[2]),
                             mk ? mk->num_elems : 0, n);
         }
         if (undef) {
            comps[i] = nir_undef(&c->b, 1, x->bit_size);
         } else if (sel < x->num_components) {
            comps[i] = nir_channel(&c->b, x, sel);
         } else if (sel - x->num_components < y->num_components) {
            comps[i] = nir_channel(&c->b, y, sel - x->num_components);
         } else {
            return air_error(c, "instruction: a shuffle index past its inputs");
         }
      }
      air_set(c, &in->base, air_val_ssa(c, nir_vec(&c->b, comps, n)));
      return true;
   }
   case AIR_OP_EXTRACTVAL: {
      struct air_val *agg = air_get(c, in->operands[0]);
      if (!agg)
         return false;
      struct air_val **slot = agg_slot(c, agg, in->indices, in->num_indices);
      if (!slot)
         return false;
      if (!*slot) {
         /* a field of an undef aggregate */
         air_set(c, &in->base,
                 air_val_ssa(c, nir_undef(&c->b, air_nir_components(in->base.type),
                                          air_nir_bit_size(c, in->base.type))));
         return true;
      }
      air_set(c, &in->base, *slot);
      return true;
   }
   case AIR_OP_INSERTVAL: {
      struct air_val *agg = air_get(c, in->operands[0]);
      struct air_val *val = air_get(c, in->operands[1]);
      if (!agg || !val)
         return false;
      struct air_val *copy = clone_val(c, agg);
      struct air_val **slot = agg_slot(c, copy, in->indices, in->num_indices);
      if (!slot)
         return false;
      *slot = val;
      air_set(c, &in->base, copy);
      return true;
   }
   case AIR_OP_FREEZE:
      air_set(c, &in->base, air_get(c, in->operands[0]));
      return c->error[0] == 0;
   case AIR_OP_ALLOCA: {
      uint64_t count;
      if (!air_const_uint(in->operands[0], &count))
         return air_error(c, "instruction: an alloca of a run-time size");
      /* A local that holds a POINTER keeps it in a translation-time slot rather
       * than in scratch memory (store_pointer).
       */
      uint64_t size = air_type_alloc_size(c->m, in->aux_type) * count;
      unsigned align = MAX2(in->align, air_type_abi_align(c->m, in->aux_type));
      c->scratch_size = ALIGN_POT(c->scratch_size, MAX2(align, 1));
      struct air_ptr p = {
         .class = AIR_PTR_SCRATCH,
         .offset = nir_imm_int(&c->b, c->scratch_size),
         .pointee = in->aux_type,
      };
      c->scratch_size += size;
      air_set(c, &in->base, air_val_ptr(c, p));
      return true;
   }
   case AIR_OP_LOAD: {
      struct air_val *p = air_get(c, in->operands[0]);
      if (!p)
         return false;
      if (p->kind == AIR_VAL_PTR_SELECT) {
         nir_def *res = load_through_choice(c, p, in->base.type, in->align);
         if (!res)
            return false;
         air_set(c, &in->base, air_val_ssa(c, res));
         return true;
      }
      if (p->kind != AIR_VAL_PTR)
         return air_error(c, "instruction: a load from something that is not a pointer");
      struct air_val *stored = in->base.type && in->base.type->kind == AIR_TYPE_POINTER
         ? stored_pointer(c, p) : NULL;
      if (stored) {
         struct air_val *value = val_in_current_impl(c, stored);
         if (!value)
            return false;
         air_set(c, &in->base, value);
         return true;
      }
      /* An argument buffer resolves before it loads: the byte offset names a
       * member and the member's descriptor names a binding, so this load
       * becomes a pointer and not an instruction.
       */
      if (p->ptr.class == AIR_PTR_ARGBUF) {
         struct air_val *res = argbuf_load(c, p->ptr, in->base.type);
         if (!res)
            return false;
         air_set(c, &in->base, res);
         return true;
      }
      nir_def *res = air_load_ptr(c, p->ptr, in->base.type, in->align);
      if (!res)
         return false;
      air_set(c, &in->base, air_val_ssa(c, res));
      return true;
   }
   case AIR_OP_STORE: {
      struct air_val *p = air_get(c, in->operands[1]);
      if (!p)
         return false;
      struct air_val *sv = air_get(c, in->operands[0]);
      if (!sv)
         return false;
      if (val_is_pointerish(sv))
         return store_pointer(c, p, sv);
      if (p->kind == AIR_VAL_PTR_SELECT)
         return air_error(c, "instruction: a store through a chosen pointer, which would "
                             "have to write both of them");
      nir_def *v = air_get_ssa(c, in->operands[0]);
      if (!v)
         return false;
      if (p->kind != AIR_VAL_PTR)
         return air_error(c, "instruction: a store to something that is not a pointer");
      return air_store_ptr(c, p->ptr, v, in->align);
   }
   case AIR_OP_CALL: {
      const struct air_value *callee = in->operands[0];
      if (callee->kind != AIR_VALUE_FUNCTION || !callee->name)
         return air_error(c, "instruction: a call through a pointer");
      return air_translate_call(c, in, callee->name);
   }
   case AIR_OP_RET:
   case AIR_OP_BR:
   case AIR_OP_SWITCH:
   case AIR_OP_UNREACHABLE:
   case AIR_OP_PHI:
      return true; /* handled by the CFG walk */
   default:
      return air_error(c, "instruction: opcode %u", in->op);
   }
}
