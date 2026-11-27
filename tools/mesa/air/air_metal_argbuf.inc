/* Argument buffer encoding and slot shape extraction, included into air_metal.c. */


/*
 * ---------------------------------------------------------------------------------------
 * THE ARGUMENT BUFFER RECORDER: MTLArgumentEncoder's three set calls, and nothing
 * written into the buffer.
 *
 * air_metal.h carries the whole of why. In one line: the frontend resolved every member
 * of every set to a binding while translating, Metal's 8-byte handles mean nothing to a
 * driver that binds by index, and so an encoder here records (resource, Metal id)
 * against (destination buffer, destination offset) and the draw binds what it finds.
 */

/* The encoding at `offset` of this buffer, or NULL. A buffer with no encodings -- which
 * is every buffer that is only ever a buffer -- answers in one comparison.
 */
static struct rwa_argbuf_encoding *
argbuf_find(const struct rwa_buffer *dest, size_t offset)
{
   for (struct rwa_argbuf_encoding *e = dest ? dest->encodings : NULL; e; e = e->next) {
      if (e->offset == offset)
         return e;
   }
   return NULL;
}

/* The encoding at `offset`, made if it is not there, with room for `id`.
 *
 * It is NOT cleared when it is found, and that is Metal's behaviour rather than a
 * shortcut: -setArgumentBuffer:offset: starts an encoding over a buffer whose BYTES are
 * still whatever the last encoding wrote there, so an application that sets three of
 * five members leaves the other two as they were. A record that cleared itself would
 * refuse a draw Metal would have made.
 */
static struct rwa_argbuf_encoding *
argbuf_encoding_for(struct rwa_buffer *dest, size_t offset, unsigned id, char *why,
                    size_t why_size)
{
   struct rwa_argbuf_encoding *e = argbuf_find(dest, offset);

   if (!e) {
      e = calloc(1, sizeof(*e));
      if (!e) {
         say(why, why_size, "out of memory recording an argument buffer");
         return NULL;
      }
      e->offset = offset;
      e->next = dest->encodings;
      dest->encodings = e;
      /* and the buffer joins the device's list of buffers that hold records, once, so
       * that destroying a resource can scrub itself out of them */
      if (!dest->listed) {
         dest->listed = true;
         dest->next_encoded = dest->device->encoded;
         dest->device->encoded = dest;
      }
   }

   if (id >= e->slots) {
      unsigned want = (id + 8) & ~7u;
      struct rwa_argbuf_slot *grown = realloc(e->slot, want * sizeof(*grown));

      if (!grown) {
         say(why, why_size, "out of memory recording member %u of an argument buffer", id);
         return NULL;
      }
      memset(grown + e->slots, 0, (want - e->slots) * sizeof(*grown));
      e->slot = grown;
      e->slots = want;
   }
   return e;
}

/* What every one of the three set calls checks before it records anything. `where` names
 * the call in the sentence, because "a texture" and "a buffer" are different mistakes.
 */
static struct rwa_argbuf_encoding *
argbuf_begin_member(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
                    const char *what, char *why, size_t why_size)
{
   if (!dest) {
      say(why, why_size, "%s was encoded into no argument buffer at all: Metal's "
                         "-setArgumentBuffer:offset: has to name one first", what);
      return NULL;
   }
   if (id >= RWA_MAX_ARGBUF_MEMBERS) {
      say(why, why_size, "%s was encoded at id %u and this library records %u members of "
                         "a set", what, id, RWA_MAX_ARGBUF_MEMBERS);
      return NULL;
   }
   /* The handle Metal would write is eight bytes at dest_offset + 8 * id, and an
    * encoding that does not fit the buffer is an application that allocated it from
    * something other than -encodedLength. Nothing of ours would read those bytes, which
    * is exactly why it has to be said here: this library would otherwise be the only
    * thing in the system that did not notice.
    */
   size_t need = dest_offset + (size_t)8 * id + 8;
   if (need > dest->size) {
      say(why, why_size, "%s was encoded at id %u of an argument buffer at offset %zu, "
                         "which Metal would write %zu bytes into, and that buffer holds "
                         "%zu", what, id, dest_offset, need, dest->size);
      return NULL;
   }
   return argbuf_encoding_for(dest, dest_offset, id, why, why_size);
}

int
rwa_argbuf_encode_buffer(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
                         struct rwa_buffer *buffer, size_t offset, char *why,
                         size_t why_size)
{
   RWA_ARGBUF_LOCKED;
   struct rwa_argbuf_encoding *e =
      argbuf_begin_member(dest, dest_offset, id, "a buffer", why, why_size);

   if (!e)
      return -1;
   if (!buffer) {
      if (e->slot[id].set) {
         e->slot[id] = (struct rwa_argbuf_slot){ 0 };
         if (e->members > 0)
            e->members--;
      }
      return 0;
   }
   if (buffer->device != dest->device) {
      say(why, why_size, "the buffer encoded at id %u belongs to another device", id);
      return -1;
   }
   if (offset > buffer->size) {
      say(why, why_size, "the buffer encoded at id %u is bound at offset %zu of %zu "
                         "bytes", id, offset, buffer->size);
      return -1;
   }
   if (!e->slot[id].set)
      e->members++;
   e->slot[id] = (struct rwa_argbuf_slot){ .set = true, .kind = RWA_ARGBUF_BUFFER,
                                           .buffer = buffer, .offset = offset };
   return 0;
}

int
rwa_argbuf_encode_texture(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
                          struct rwa_texture *texture, char *why, size_t why_size)
{
   RWA_ARGBUF_LOCKED;
   struct rwa_argbuf_encoding *e =
      argbuf_begin_member(dest, dest_offset, id, "a texture", why, why_size);

   if (!e)
      return -1;
   if (!texture) {
      if (e->slot[id].set) {
         e->slot[id] = (struct rwa_argbuf_slot){ 0 };
         if (e->members > 0)
            e->members--;
      }
      return 0;
   }
   if (texture->device != dest->device) {
      say(why, why_size, "the texture encoded at id %u belongs to another device", id);
      return -1;
   }
   if (!e->slot[id].set)
      e->members++;
   e->slot[id] = (struct rwa_argbuf_slot){ .set = true, .kind = RWA_ARGBUF_TEXTURE,
                                           .texture = texture };
   return 0;
}

int
rwa_argbuf_encode_sampler(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
                          struct rwa_sampler *sampler, char *why, size_t why_size)
{
   RWA_ARGBUF_LOCKED;
   struct rwa_argbuf_encoding *e =
      argbuf_begin_member(dest, dest_offset, id, "a sampler", why, why_size);

   if (!e)
      return -1;
   if (!sampler) {
      if (e->slot[id].set) {
         e->slot[id] = (struct rwa_argbuf_slot){ 0 };
         if (e->members > 0)
            e->members--;
      }
      return 0;
   }
   if (sampler->device != dest->device) {
      say(why, why_size, "the sampler encoded at id %u belongs to another device", id);
      return -1;
   }
   if (!e->slot[id].set)
      e->members++;
   e->slot[id] = (struct rwa_argbuf_slot){ .set = true, .kind = RWA_ARGBUF_SAMPLER,
                                           .sampler = sampler };
   return 0;
}

unsigned
rwa_argbuf_encoded_members(const struct rwa_buffer *dest, size_t dest_offset)
{
   RWA_ARGBUF_LOCKED;
   const struct rwa_argbuf_encoding *e = argbuf_find(dest, dest_offset);

   return e ? e->members : 0;
}

static bool
argbuf_has_encodings(struct rwa_buffer *buffer)
{
   if (!buffer)
      return false;
   RWA_ARGBUF_LOCKED;
   return buffer->encodings != NULL;
}

/*
 * A resource destroyed after it was encoded, scrubbed out of every record that names it.
 *
 * Metal would let the handle go stale and the GPU read freed memory, which is what
 * -useResource: and MTLHeap residency exist to prevent on that side. Here the record
 * holds a pointer this library would dereference at the next draw, so the alternative to
 * scrubbing is not a wrong picture but a crash in this library -- and a flag left behind
 * turns it into a sentence naming the member instead.
 */
static void
argbuf_scrub(struct rwa_device *device, const struct rwa_buffer *buffer,
             const struct rwa_texture *texture, const struct rwa_sampler *sampler)
{
   RWA_ARGBUF_LOCKED;
   for (struct rwa_buffer *b = device ? device->encoded : NULL; b; b = b->next_encoded) {
      for (struct rwa_argbuf_encoding *e = b->encodings; e; e = e->next) {
         for (unsigned i = 0; i < e->slots; i++) {
            struct rwa_argbuf_slot *sl = &e->slot[i];

            if (!sl->set || sl->destroyed)
               continue;
            if ((buffer && sl->buffer == buffer) ||
                (texture && sl->texture == texture) ||
                (sampler && sl->sampler == sampler)) {
               sl->destroyed = true;
               sl->buffer = NULL;
               sl->texture = NULL;
               sl->sampler = NULL;
            }
         }
      }
   }
}

/* And the records of a buffer that is itself going away. Called from
 * rwa_buffer_destroy, which also scrubs that buffer out of everybody else's records.
 */
static void
argbuf_forget_all(struct rwa_buffer *dest)
{
   RWA_ARGBUF_LOCKED;
   struct rwa_argbuf_encoding *e = dest->encodings;

   while (e) {
      struct rwa_argbuf_encoding *next = e->next;

      free(e->slot);
      free(e);
      e = next;
   }
   dest->encodings = NULL;
   if (dest->listed) {
      struct rwa_buffer **link = &dest->device->encoded;

      while (*link && *link != dest)
         link = &(*link)->next_encoded;
      if (*link == dest)
         *link = dest->next_encoded;
      dest->listed = false;
      dest->next_encoded = NULL;
   }
}

/*
 * WHAT A BUFFER'S OFFSET HAS TO SATISFY, for a member of an argument buffer and for a
 * plain buffer argument alike -- one rule and one sentence, because they are the same
 * arithmetic: Metal's own `offset:` from -setFragmentBuffer:offset:atIndex: or from
 * -[MTLArgumentEncoder setBuffer:offset:atIndex:], and Gallium's binding underneath.
 *
 * `constant` (a UBO) is NOT refused for its alignment here, because bind_stage serves any
 * offset: an aligned one as the binding's own offset, and one this GPU cannot fetch from
 * as a `user_buffer`, which both back ends upload themselves. `device` (an SSBO) is
 * refused, and the asymmetry is the point rather than an omission -- the shader may WRITE
 * an SSBO, so the copy that serves a UBO would take the writes with it when it was thrown
 * away, and Gallium has no user_buffer on a shader buffer for that very reason.
 */
static int
buffer_offset_ok(const struct rwa_device *device, const struct air_resource *r,
                 const struct rwa_buffer *b, size_t offset, const char *name,
                 const char *what, char *why, size_t why_size)
{
   if (offset > b->size) {
      say(why, why_size, "%s is bound at offset %zu of a buffer that holds %zu bytes",
          what, offset, b->size);
      return -1;
   }
   if (r->kind == AIR_RESOURCE_DEVICE_BUFFER &&
       offset % device->ssbo_offset_align != 0) {
      say(why, why_size, "%s is bound at offset %zu, and %s declares it in the `device` "
                         "address space, whose offsets this GPU fetches in multiples of "
                         "%u bytes. A `constant` buffer at such an offset is uploaded "
                         "instead; a `device` one cannot be, because the shader may write "
                         "it and the copy would throw the writes away", what, offset,
          name, device->ssbo_offset_align);
      return -1;
   }
   return 0;
}

/*
 * THE OFFSET A DIRECT BUFFER ARGUMENT WAS BOUND AT, out of the stage's buffer table, and
 * the cross-check between the two tables a driver fills.
 *
 * `rwa_bindings.buffers` says which buffer is at index n and the table says where in it;
 * a driver fills both from the one table Metal has, so where both name a buffer they must
 * name the same one. Two that disagree are a driver filling one of them from the wrong
 * place -- and the alternative to refusing is binding one buffer's memory at the other
 * one's offset, which is a wrong picture and not an error.
 */
static int
direct_buffer_offset(const struct air_resource *r, const struct rwa_bindings *bindings,
                     const struct rwa_buffer_bind *table, unsigned table_count,
                     const char *name, size_t *offset, char *why, size_t why_size)
{
   *offset = 0;
   if (!table || r->index >= table_count || !table[r->index].buffer)
      return 0;
   if (bindings->buffers && r->index < bindings->buffer_count &&
       bindings->buffers[r->index] &&
       bindings->buffers[r->index] != table[r->index].buffer) {
      say(why, why_size, "the two buffer tables of this draw disagree about index %u: %s "
                         "reads a buffer there, and one table names one buffer and the "
                         "other names another", r->index, name);
      return -1;
   }
   *offset = table[r->index].offset;
   return 0;
}

/*
 * ONE MEMBER, AT THE DRAW: what is bound for the resource the frontend resolved out of a
 * set, and the five refusals that are the whole point of this feature.
 *
 * `r` is a resource whose `argbuf_set` is not ~0u: a member of the set bound at Metal
 * buffer index `r->argbuf_set`, named `r->argbuf_id` by the application and bound at
 * `r->index` by this library. What comes back is whichever of the three pointers the
 * member's kind asks for, or -1 and a sentence.
 */
static int
argbuf_member(const struct air_resource *r, const struct rwa_buffer_bind *table,
              unsigned table_count, const char *name, struct rwa_buffer **buffer,
              size_t *offset, struct rwa_texture **texture, struct rwa_sampler **sampler,
              char *why, size_t why_size)
{
   const char *kind = r->kind == AIR_RESOURCE_SAMPLER   ? "sampler"
                    : r->kind == AIR_RESOURCE_TEXTURE   ? "texture"
                    : r->kind == AIR_RESOURCE_IMAGE     ? "texture"
                                                        : "buffer";
   const char *member = r->name ? r->name : "an unnamed member";

   if (buffer)
      *buffer = NULL;
   if (texture)
      *texture = NULL;
   if (sampler)
      *sampler = NULL;
   if (offset)
      *offset = 0;

   /* Nothing bound at the set's own Metal index. The shader would read a descriptor out
    * of memory that is not there at all.
    */
   const struct rwa_buffer_bind *bind =
      table && r->argbuf_set < table_count ? &table[r->argbuf_set] : NULL;
   if (!bind || !bind->buffer) {
      say(why, why_size, "%s reads %s `%s` out of the argument buffer at Metal buffer "
                         "index %u and nothing is bound there (%u slot%s given)", name,
          kind, member, r->argbuf_set, table_count, table_count == 1 ? "" : "s");
      return -1;
   }
   /* Inline `air.indirect_constant` bytes read straight from the bound set buffer. */
   if (r->argbuf_id == ~0u && r->kind == AIR_RESOURCE_CONSTANT_BUFFER) {
      if (buffer) *buffer = bind->buffer;
      if (offset) *offset = bind->offset;
      return 0;
   }
   /* Resource descriptors must have been recorded via MTLArgumentEncoder. */
   RWA_ARGBUF_LOCKED;
   const struct rwa_argbuf_encoding *e = argbuf_find(bind->buffer, bind->offset);
   if (!e || e->members == 0) {
      say(why, why_size, "the buffer bound as the argument buffer at Metal buffer index "
                         "%u (offset %zu) was never filled through an MTLArgumentEncoder, "
                         "so there is no record of what %s's %s `%s` (id %u) should be. "
                         "This driver binds an argument buffer's members from what the "
                         "encoder recorded and never from the handles in the buffer, "
                         "which are an Apple GPU's descriptors; one written by the "
                         "processor, by a kernel or by a blit cannot be read here at all",
          r->argbuf_set, bind->offset, name, kind, member, r->argbuf_id);
      return -1;
   }

   const struct rwa_argbuf_slot *sl =
      r->argbuf_id < e->slots ? &e->slot[r->argbuf_id] : NULL;

   /* Encoded, but not this member: Metal would have written a handle of zero there and
    * the GPU would read descriptor zero of the device, which is some other resource.
    */
   if (!sl || !sl->set) {
      say(why, why_size, "%s reads %s `%s` at id %u of the argument buffer at Metal "
                         "buffer index %u, and that id was never set on the encoder "
                         "(%u member%s encoded at offset %zu)", name, kind, member,
          r->argbuf_id, r->argbuf_set, e ? e->members : 0,
          e && e->members == 1 ? "" : "s", bind->offset);
      return -1;
   }
   if (sl->destroyed) {
      say(why, why_size, "the %s encoded at id %u of the argument buffer at Metal buffer "
                         "index %u has been destroyed since, and %s reads `%s` out of it",
          kind, r->argbuf_id, r->argbuf_set, name, member);
      return -1;
   }

   /* Encoded as the wrong kind of thing. The id is the same number in both namings, so
    * an application that sent -setTexture:atIndex: where the shader declares a buffer
    * has either the wrong shader or the wrong id, and the handle Metal wrote would be a
    * texture descriptor read as a pointer.
    */
   unsigned want = r->kind == AIR_RESOURCE_SAMPLER ? RWA_ARGBUF_SAMPLER
                 : r->kind == AIR_RESOURCE_TEXTURE ? RWA_ARGBUF_TEXTURE
                 : r->kind == AIR_RESOURCE_IMAGE   ? RWA_ARGBUF_TEXTURE
                                                   : RWA_ARGBUF_BUFFER;
   if (sl->kind != want) {
      static const char *const encoded_as[] = { "a buffer", "a texture", "a sampler" };

      say(why, why_size, "%s declares %s `%s` at id %u of the argument buffer at Metal "
                         "buffer index %u, and %s was encoded there", name, kind, member,
          r->argbuf_id, r->argbuf_set,
          sl->kind < 3 ? encoded_as[sl->kind] : "something else");
      return -1;
   }

   if (want == RWA_ARGBUF_BUFFER) {
      /* Metal's own offset:, honoured, by the rule a direct argument's offset follows:
       * see buffer_offset_ok, which is where both sentences live.
       */
      char what[192];

      snprintf(what, sizeof(what), "the buffer encoded at id %u of the argument buffer at "
                                   "Metal buffer index %u (member `%s`)", r->argbuf_id,
               r->argbuf_set, member);
      if (buffer_offset_ok(sl->buffer->device, r, sl->buffer, sl->offset, name, what, why,
                           why_size) != 0)
         return -1;
      if (buffer)
         *buffer = sl->buffer;
      if (offset)
         *offset = sl->offset;
   } else if (want == RWA_ARGBUF_TEXTURE) {
      if (texture)
         *texture = sl->texture;
   } else if (sampler) {
      *sampler = sl->sampler;
   }
   return 0;
}

/*
 * The element type of a texture argument as Apple's metadata declares it, onto the slot:
 * the word inside the angle brackets of air.arg_type_name, which is the Metal
 * programmer's own `texture2d<half, write>` or `texture2d<uint, sample>`.
 *
 * It is read from the name rather than from the shader's instructions because it is the
 * declaration a driver has to honour, and the only statement of the element type in
 * hand that did not come through this front end's translation. All three fields are
 * written or none: a name this does not recognise leaves declared_element NULL, which
 * is how every reader asks whether there is a declaration at all.
 */
static void
declare_slot_element(const char *type_name, struct rwa_slot *slot)
{
   const char *lt = type_name ? strchr(type_name, '<') : NULL;
   if (!lt)
      return;
   for (lt++; *lt == ' '; lt++)
      ;

   /* Every element type MSL allows a texture: 32- and 16-bit, and no 8-bit one -- a
    * texture of chars is spelled with a normalised format and read as float.
    */
   static const struct { const char *word; nir_alu_type type; unsigned bits; } words[] = {
      { "float",  nir_type_float, 32 }, { "half",   nir_type_float, 16 },
      { "int",    nir_type_int,   32 }, { "short",  nir_type_int,   16 },
      { "uint",   nir_type_uint,  32 }, { "ushort", nir_type_uint,  16 },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
      size_t n = strlen(words[i].word);
      /* the whole word and not a prefix of it: "ushort" must not read as "uint" */
      if (strncmp(lt, words[i].word, n) ||
          (lt[n] != ',' && lt[n] != '>' && lt[n] != ' '))
         continue;
      slot->declared_type = words[i].type;
      slot->declared_element = words[i].word;
      slot->declared_bits = words[i].bits;
      return;
   }
}

/* The shape of every texture slot a kernel touches, off its own instructions.
 *
 * This is air_run's image_shapes() and texture_shapes() in one pass, and the reason
 * they read instructions rather than the Metal type name in the metadata is theirs:
 * air.read_texture_* and air.write_texture_* become nir_image_load and nir_image_store
 * while a read-only texture's read() becomes a txf (air_intrinsics.c), so whether a
 * slot wants a sampler view or an image view, and what element type it is read as, are
 * properties of the instruction the GPU will execute. air_run reads them to invent a
 * texture; here they are read to check the one the caller brought, which is the same
 * knowledge pointed the other way.
 *
 * Per slot and not per shader: BlitLumaKernel_Unorm reads a uint image and writes a
 * half one, so one answer for the kernel would be right for one of its two images and
 * quietly wrong for the other.
 *
 * `compare_sampler` is the one answer here that is about SAMPLER slots and not texture
 * slots, and is optional. It records which samplers a shadow texture instruction reads,
 * which is the question struct rwa_sampler exists to answer: Metal decides at the
 * sample call that a comparison happens, Gallium decides it in the sampler state, and
 * the two only agree if the comparison is switched on for exactly these slots. Read off
 * the instructions for the same reason everything else here is: a `constexpr sampler`
 * carrying compare_func::less is still a plain sampler to every sample() that reads it.
 */
/* Track the slots reachable through a sampler select. Other computed indices
 * retain all slots so a capability check cannot assume one filter. */
static unsigned
sampler_slot_mask(nir_def *value, unsigned base)
{
   const unsigned all = (1u << RWA_MAX_TEXTURES) - 1;
   if (!value) return all;
   nir_instr *in = nir_def_instr(value);
   nir_src src = nir_src_for_ssa(value);
   if (nir_src_is_const(src)) {
      uint64_t slot = (uint64_t)base + nir_src_as_uint(src);
      return slot < RWA_MAX_TEXTURES ? 1u << slot : all;
   }
   if (in->type != nir_instr_type_alu) return all;
   nir_alu_instr *alu = nir_instr_as_alu(in);
   if (alu->op == nir_op_bcsel) {
      if (nir_src_is_const(alu->src[0].src))
         return sampler_slot_mask(alu->src[nir_src_as_bool(alu->src[0].src) ? 1 : 2].src.ssa,
                                  base);
      return sampler_slot_mask(alu->src[1].src.ssa, base) |
             sampler_slot_mask(alu->src[2].src.ssa, base);
   }
   if (alu->op == nir_op_iadd) {
      for (unsigned i = 0; i < 2; i++) {
         if (!nir_src_is_const(alu->src[i].src)) continue;
         uint64_t next = (uint64_t)base + nir_src_as_uint(alu->src[i].src);
         if (next >= RWA_MAX_TEXTURES) return all;
         return sampler_slot_mask(alu->src[1-i].src.ssa, next);
      }
   }
   return all;
}

static void
slot_shapes(nir_shader *cs, struct rwa_slot slots[RWA_MAX_TEXTURES],
            bool compare_sampler[RWA_MAX_TEXTURES])
{
   nir_foreach_function_impl(impl, cs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            unsigned binding;
            nir_alu_type type;
            enum glsl_sampler_dim dim;
            bool is_array;

            bool is_tex = instr->type == nir_instr_type_tex;

            if (is_tex) {
               nir_tex_instr *t = nir_instr_as_tex(instr);
               binding = t->texture_index;
               type = t->dest_type;
               dim = t->sampler_dim;
               is_array = t->is_array;
               if (compare_sampler && t->is_shadow &&
                   t->sampler_index < RWA_MAX_TEXTURES)
                  compare_sampler[t->sampler_index] = true;
               if (t->is_shadow && binding < RWA_MAX_TEXTURES)
                  slots[binding].shadow = true;
               if (nir_tex_instr_need_sampler(t)) {
                  int offset = nir_tex_instr_src_index(t, nir_tex_src_sampler_offset);
                  unsigned mask = offset < 0 ?
                     (t->sampler_index < RWA_MAX_TEXTURES ? 1u << t->sampler_index :
                      (1u << RWA_MAX_TEXTURES) - 1) :
                     sampler_slot_mask(t->src[offset].src.ssa, t->sampler_index);
                  if (nir_tex_instr_src_index(t, nir_tex_src_texture_offset) >= 0) {
                     for (unsigned k = 0; k < RWA_MAX_TEXTURES; k++)
                        slots[k].sampler_mask |= mask;
                  } else if (binding < RWA_MAX_TEXTURES) {
                     slots[binding].sampler_mask |= mask;
                  }
               }
            } else if (instr->type == nir_instr_type_intrinsic) {
               nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
               if (in->intrinsic == nir_intrinsic_image_load)
                  type = nir_intrinsic_dest_type(in);
               else if (in->intrinsic == nir_intrinsic_image_store)
                  type = nir_intrinsic_src_type(in);
               else
                  continue;
               /* a computed binding: there is nothing here to check a texture against,
                * and the slot stays unknown rather than wrongly shaped
                */
               if (!nir_src_is_const(in->src[0]))
                  continue;
               binding = nir_src_as_uint(in->src[0]);
               dim = nir_intrinsic_image_dim(in);
               is_array = nir_intrinsic_image_array(in);
            } else {
               continue;
            }

            if (binding >= RWA_MAX_TEXTURES)
               continue;   /* an index past the slots is refused by name in rwa_dispatch */
            if (!slots[binding].shape_known) {
               slots[binding].shape_known = true;
               slots[binding].dim = dim;
               slots[binding].is_array = is_array;
            }

            /* The element type is taken from a tex instruction and from nothing else.
             *
             * nir_tex_instr::dest_type is a real statement about the texture: a half
             * texture read through Metal's read() becomes a txf whose dest_type is
             * float16, and that is the type the slot holds. An image_load's dest_type
             * and an image_store's src_type are not: with format=none they mean "raw
             * bits, the bound image's format decides the conversion", so the front end
             * emits uint for both, and BlitKernel_Unorm -- whose dst is declared
             * texture2d<half, write> -- stores through src_type=uint. Reading that as a
             * claim about the texture refused 5 of AV1SW's 11 kernels when each was
             * bound the formats its own metadata asks for, measured with
             * air_metal_test: every one of the _Unorm kernels, whose destination is
             * half, and for binding a float format to what was taken for an integer
             * shader -- the opposite of what Apple declared.
             *
             * The shape (dim, array) is a different matter and is taken from either,
             * because an image intrinsic's image_dim does say which texture it is.
             */
            if (is_tex && !slots[binding].type_known) {
               slots[binding].type_known = true;
               slots[binding].type = nir_alu_type_get_base_type(type);
            }
         }
      }
   }
}

