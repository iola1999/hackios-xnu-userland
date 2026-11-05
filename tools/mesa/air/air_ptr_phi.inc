/* A phi of textures (choosable_texture): one register for the slot when the operands
 * name different textures or one comes round a loop. False when an operand is not such
 * a texture, and resolve_pointer_phi's general rules apply. */
static bool
resolve_texture_phi(struct air_ctx *c, const struct air_inst *in, const bool *reachable)
{
   struct air_val *first = NULL;
   struct air_val *null_tex = NULL;
   bool differ = false, loop_carried = false;
   for (unsigned k = 0; k < in->num_operands; k++) {
      if (!in->targets[k] || !reachable[in->targets[k]->index])
         continue;
      struct hash_entry *e = _mesa_hash_table_search(c->values, in->operands[k]);
      if (!e) {
         loop_carried = true;
         continue;
      }
      struct air_val *v = e->data;
      if (v && v->kind == AIR_VAL_PTR && v->ptr.class == AIR_PTR_TEXTURE && v->ptr.is_null) {
         null_tex = v;
         continue;
      }
      if (!choosable_texture(v))
         return false;
      if (!first)
         first = v;
      else
         differ |= !same_ptr(&first->ptr, &v->ptr);
   }
   if (!first)
      first = null_tex;
   if (!first)
      return false;

   if (null_tex && first != null_tex && !first->ptr.is_null)
      air_texture_may_be_unbound(c, first->ptr.binding);

   if (!differ && !loop_carried) {
      air_set(c, &in->base, first);
      return true;
   }
   nir_builder entry = nir_builder_at(nir_before_impl(c->b.impl));
   nir_def *reg = nir_decl_reg(&entry, 1, 32, 0);
   struct air_ptr p = first->ptr;
   p.binding = 0;
   p.offset = nir_load_reg(&c->b, reg);
   _mesa_hash_table_insert(c->phi_regs, in, reg);
   air_set(c, &in->base, air_val_ptr(c, p));
   return true;
}

struct air_ptr_phi_reg {
   nir_def *reg;
   nir_def *idx_reg;
   unsigned num_places;
   struct air_ptr places[8];
};

static bool
phi_collect_places(struct air_ctx *c, struct air_ptr_phi_reg *r, const struct air_val *v)
{
   if (!v)
      return false;
   if (v->kind == AIR_VAL_PTR_SELECT)
      return phi_collect_places(c, r, v->elems[0]) && phi_collect_places(c, r, v->elems[1]);
   if (v->kind != AIR_VAL_PTR)
      return false;
   for (unsigned p = 0; p < r->num_places; p++) {
      if (same_resource(&r->places[p], &v->ptr))
         return true;
   }
   if (r->num_places >= 8)
      return false;
   r->places[r->num_places++] = v->ptr;
   return true;
}

static bool
resolve_pointer_phi_places(struct air_ctx *c, const struct air_inst *in, const bool *reachable)
{
   struct air_ptr_phi_reg *r = rzalloc(c->mem_ctx, struct air_ptr_phi_reg);
   for (unsigned k = 0; k < in->num_operands; k++) {
      if (!in->targets[k] || !reachable[in->targets[k]->index])
         continue;
      struct hash_entry *e = _mesa_hash_table_search(c->values, in->operands[k]);
      if (!e)
         continue;
      if (!phi_collect_places(c, r, e->data))
         return false;
   }
   if (r->num_places == 0)
      return air_error(c, "instruction: a pointer phi with no places");

   nir_builder entry = nir_builder_at(nir_before_impl(c->b.impl));
   r->reg = nir_decl_reg(&entry, 1, 32, 0);
   r->idx_reg = nir_decl_reg(&entry, 1, 32, 0);
   nir_store_reg(&entry, nir_imm_int(&entry, 0), r->reg);
   nir_store_reg(&entry, nir_imm_int(&entry, 0), r->idx_reg);
   _mesa_hash_table_insert(c->phi_regs, in, r);

   nir_def *off = nir_load_reg(&c->b, r->reg);
   nir_def *idx = nir_load_reg(&c->b, r->idx_reg);
   struct air_val *v = NULL;
   for (int i = (int)r->num_places - 1; i >= 0; i--) {
      struct air_val *pi = rzalloc(c->mem_ctx, struct air_val);
      pi->kind = AIR_VAL_PTR;
      pi->ptr = r->places[i];
      pi->ptr.offset = (r->places[i].class == AIR_PTR_TEXTURE ||
                        r->places[i].class == AIR_PTR_SAMPLER) ? NULL : off;
      v = v ? air_val_ptr_select(c, nir_ieq_imm(&c->b, idx, i), pi, v) : pi;
   }
   air_set(c, &in->base, v);
   return true;
}

static bool
phi_find_place_and_offset(struct air_ctx *c, const struct air_ptr_phi_reg *r,
                          const struct air_val *from, nir_def **out_idx, nir_def **out_off)
{
   if (!from)
      return false;
   if (from->kind == AIR_VAL_PTR) {
      for (unsigned p = 0; p < r->num_places; p++) {
         if (same_resource(&r->places[p], &from->ptr)) {
            *out_idx = nir_imm_int(&c->b, p);
            *out_off = from->ptr.offset ? from->ptr.offset : nir_imm_int(&c->b, 0);
            return true;
         }
      }
      return false;
   }
   if (from->kind == AIR_VAL_PTR_SELECT) {
      nir_def *idx0 = NULL, *off0 = NULL, *idx1 = NULL, *off1 = NULL;
      if (!phi_find_place_and_offset(c, r, from->elems[0], &idx0, &off0) ||
          !phi_find_place_and_offset(c, r, from->elems[1], &idx1, &off1))
         return false;
      *out_idx = nir_bcsel(&c->b, from->def, idx0, idx1);
      *out_off = nir_bcsel(&c->b, from->def, off0, off1);
      return true;
   }
   return false;
}

/* A phi of POINTERS, resolved rather than carried in a register.
 *
 * A pointer here is *(which resource, which byte offset)* decided while
 * translating, so it has no bits to put in a register and a phi of two of them
 * is not a value NIR can hold. It need not be: LLVM duplicates a
 * `getelementptr` onto two paths and phis the copies back, so both operands
 * name the same member -- 207 of the pointer phis in Resident Evil 4's library
 * are exactly that, and not one names two different members.
 *
 * Operands in one buffer at different offsets get a register for the offset, the
 * way a loop-carried pointer does (CoreAnimation's path vertex shaders leave a
 * binary search over their segment buffer with such a phi). Operands in different
 * resources or chosen at run time go through resolve_pointer_phi_places, which
 * builds an AIR_VAL_PTR_SELECT chain over the possible places.
 *
 * `reachable` decides which edges count: the blocks are walked in reverse post
 * order, so an operand coming in over a BACK edge has not been translated yet.
 * Such a pointer is carried round the loop: its offset gets a register, and the
 * pointer that comes back over the edge has to name the same resource. An edge
 * the entry cannot reach at all is passed over.
 */
static bool
resolve_pointer_phi(struct air_ctx *c, const struct air_inst *in, const bool *reachable)
{
   if (resolve_texture_phi(c, in, reachable))
      return true;

   struct air_val *first = NULL;
   bool loop_carried = false, offsets_differ = false, places_differ = false;
   for (unsigned k = 0; k < in->num_operands; k++) {
      if (!in->targets[k] || !reachable[in->targets[k]->index])
         continue;
      struct hash_entry *e = _mesa_hash_table_search(c->values, in->operands[k]);
      if (!e) {
         loop_carried = true;
         continue;
      }
      struct air_val *v = e->data;
      if (!v || (v->kind != AIR_VAL_PTR && v->kind != AIR_VAL_PTR_SELECT))
         return air_error(c, "instruction: a phi of pointers one of whose operands is "
                             "not a resolved pointer");
      if (v->kind == AIR_VAL_PTR_SELECT) {
         places_differ = true;
         continue;
      }
      if (!first)
         first = v;
      else if (!same_resource(&first->ptr, &v->ptr) ||
               (!same_ptr(&first->ptr, &v->ptr) && !offset_is_value(v->ptr.class) &&
                !loop_carried))
         places_differ = true;
      else
         offsets_differ |= !same_ptr(&first->ptr, &v->ptr);
   }
   if (places_differ)
      return resolve_pointer_phi_places(c, in, reachable);
   if (!first)
      return air_error(c, "instruction: a phi of pointers with no reachable operand");
   if (!loop_carried && !offsets_differ) {
      air_set(c, &in->base, first);
      return true;
   }
   /* A pointer carried round a loop, or one buffer at two offsets: the resource is
    * fixed and only the byte offset changes, so the offset gets a register like any
    * other phi, and the store pass at the end of translate_body checks that every
    * incoming pointer names the same resource before it stores that pointer's offset.
    */
   nir_builder entry = nir_builder_at(nir_before_impl(c->b.impl));
   nir_def *reg = nir_decl_reg(&entry, 1, 32, 0);
   struct air_ptr p = first->ptr;
   p.offset = nir_load_reg(&c->b, reg);
   _mesa_hash_table_insert(c->phi_regs, in, reg);
   air_set(c, &in->base, air_val_ptr(c, p));
   return true;
}
