/* A POINTER stored into memory. A pointer in this frontend is a resolution -- a binding and
 * a byte offset decided while translating -- and not an address, so it has no bytes to write.
 * A store into a local at a constant offset is kept in c->ptr_slots instead, and a load of a
 * pointer from the same place answers it (stored_pointer). SwiftUI's custom effects store their
 * texture into a local RB::Layer and sample the argument itself; the GL-on-Metal fragment
 * shaders store two buffer arguments into a local and never read them. Anywhere else, or a
 * second, different pointer in the same place, a store of a pointer is refused
 * (docs/air.md section 3). Scratch offsets are unique within one shader, so the offset is
 * the key. */
static uint64_t
ptr_slot_key(const struct air_ptr *at)
{
   return (at->offset ? nir_src_as_uint(nir_src_for_ssa(at->offset)) : 0) + 1;
}

static bool
ptr_slot_place(const struct air_val *at)
{
   return at->kind == AIR_VAL_PTR && at->ptr.class == AIR_PTR_SCRATCH &&
          (!at->ptr.offset || nir_src_is_const(nir_src_for_ssa(at->ptr.offset)));
}

static bool
store_pointer(struct air_ctx *c, const struct air_val *at, struct air_val *v)
{
   if (!ptr_slot_place(at))
      return air_error(c, "instruction: a store of a pointer into memory other than a local "
                          "at a constant offset, which this frontend has no address to write");
   if (!c->ptr_slots)
      c->ptr_slots = _mesa_hash_table_u64_create(c->mem_ctx);
   struct air_val *had = _mesa_hash_table_u64_search(c->ptr_slots, ptr_slot_key(&at->ptr));
   if (had && had != v &&
       !(had->kind == AIR_VAL_PTR && v->kind == AIR_VAL_PTR && same_ptr(&had->ptr, &v->ptr)))
      return air_error(c, "instruction: a second, different pointer stored into one local");
   _mesa_hash_table_u64_insert(c->ptr_slots, ptr_slot_key(&at->ptr), v);
   return true;
}

/* The pointer stored at the place a load reads, or NULL where none was: such a load is
 * translated as before. */
static struct air_val *
stored_pointer(struct air_ctx *c, const struct air_val *at)
{
   return ptr_slot_place(at) && c->ptr_slots
      ? _mesa_hash_table_u64_search(c->ptr_slots, ptr_slot_key(&at->ptr)) : NULL;
}

/* Pointer slots retain values across function translation. Constants are
 * recreated in the current impl; run-time defs must travel through NIR
 * parameters before another impl can use them.
 */
static struct air_val *
val_in_current_impl(struct air_ctx *c, struct air_val *v)
{
   if (!v)
      return NULL;
   if (v->kind == AIR_VAL_AGG) {
      struct air_val *copy = air_val_agg(c, v->num_elems);
      for (unsigned i = 0; i < v->num_elems; i++) {
         copy->elems[i] = val_in_current_impl(c, v->elems[i]);
         if (v->elems[i] && !copy->elems[i])
            return NULL;
      }
      return copy;
   }

   nir_def *def = v->kind == AIR_VAL_PTR ? v->ptr.offset : v->def;
   if (!def || nir_cf_node_get_function(&nir_def_block(def)->cf_node) == c->b.impl)
      return v;
   if (!nir_src_is_const(nir_src_for_ssa(def))) {
      air_error(c, "instruction: a %s crossing a function without a parameter",
                v->kind == AIR_VAL_PTR ? "pointer byte offset" :
                v->kind == AIR_VAL_PTR_SELECT ? "pointer choice" : "value");
      return NULL;
   }
   if (v->kind == AIR_VAL_PTR) {
      struct air_ptr ptr = v->ptr;
      ptr.offset = nir_imm_int(&c->b, nir_src_as_uint(nir_src_for_ssa(def)));
      return air_val_ptr(c, ptr);
   }
   if (v->kind == AIR_VAL_PTR_SELECT) {
      air_error(c, "instruction: a pointer choice crossing a function without a parameter");
      return NULL;
   }
   return air_val_ssa(c, nir_build_imm(&c->b, def->num_components, def->bit_size,
                                      nir_src_as_const_value(nir_src_for_ssa(def))));
}
