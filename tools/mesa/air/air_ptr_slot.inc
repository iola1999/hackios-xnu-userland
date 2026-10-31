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
