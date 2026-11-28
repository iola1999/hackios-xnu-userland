/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

/* Argument buffers, included by air_to_nir.c where they were defined: the member layout
 * read from metadata and checked against the struct type, the index allocation, and the
 * load that turns a member into a resource pointer. */

/* -- argument buffers ---------------------------------------------------- */

/* The module's own declaration of a named struct type, by that name.
 *
 * This is the SECOND statement of what an argument buffer holds. Apple's
 * metadata is the first; it names every member's kind, index and access but
 * not its address space -- not one of the 82 048 members of Resident Evil 4's
 * 7 857 argument buffers carries `air.address_space` -- and a constant buffer
 * bound as a device buffer is a shader reading the wrong descriptor with
 * nothing to say so. The struct type says it, member by member, and the two
 * descriptions are checked against each other before anything is bound: the
 * member count, every member's byte offset and every member's size. They
 * agree on all 7 857 sets of that library, which is what makes using it a
 * reading rather than a guess.
 */
static const struct air_type *
named_struct(const struct air_module *m, const char *name)
{
   for (unsigned i = 0; i < m->num_types; i++)
      if (m->types[i].kind == AIR_TYPE_STRUCT && !m->types[i].opaque &&
          m->types[i].name && !strcmp(m->types[i].name, name))
         return &m->types[i];
   return NULL;
}

/* The `air.*` key of a descriptor this frontend does not bind, for the refusal
 * to name -- `air.command_buffer` and `air.render_pipeline_state` are the two
 * in reach, both in the one kernel of Resident Evil 4 that encodes draws on
 * the GPU. The housekeeping keys every descriptor carries are skipped so that
 * the message names the kind and not the first word in the node.
 */
static const char *
descriptor_kind_key(const struct air_md *desc)
{
   static const char *const plumbing[] = {
      "air.location_index", "air.arg_name", "air.arg_type_name", "air.arg_type_size",
      "air.arg_type_align_size", "air.struct_type_info", "air.address_space",
      "air.buffer_size", "air.read", "air.write", "air.read_write", "air.sample",
      "air.arg_unused", "air.raster_order_group", "air.indirect_argument",
   };
   for (unsigned i = 0; i < (desc ? desc->num_ops : 0); i++) {
      const char *s = md_str(desc, i);
      if (!s || strncmp(s, "air.", 4))
         continue;
      bool known = false;
      for (unsigned k = 0; k < ARRAY_SIZE(plumbing); k++)
         known |= !strcmp(s, plumbing[k]);
      if (!known)
         return s;
   }
   return NULL;
}

/* `air.struct_type_info` of an argument buffer, into a member table.
 *
 * The shape, read off Apple's own output: five fixed fields -- byte offset,
 * size, a flag, the type name, the member name -- and then the key
 * `air.indirect_argument` and a node, which is an ORDINARY resource
 * descriptor. It differs from a direct `air.buffer` / `air.texture` /
 * `air.sampler` argument in one operand, the leading integer, which is the
 * member's number in the set where a direct argument's is its parameter
 * number. So everything needed to bind it is in the vocabulary
 * translate_input has read since the beginning.
 *
 * A member whose key comes BEFORE the five fields is how Apple writes one
 * whose own type is a struct (`!"air.struct_type_info", !N, i32 off, ...`),
 * which in an argument buffer is inline constant data rather than a resource;
 * that falls into the first refusal below, as it must -- the runtime's half of
 * this feature records descriptors against (buffer, offset) and has no bytes
 * to record for a member that is not one.
 */
static bool
is_struct_padding_field(const struct air_type *t)
{
   if (!t)
      return false;
   if (t->kind == AIR_TYPE_INT && t->bits == 8)
      return true;
   return t->kind == AIR_TYPE_ARRAY && t->elem &&
          t->elem->kind == AIR_TYPE_INT && t->elem->bits == 8;
}

static bool
argbuf_members_rec(struct air_ctx *c, const struct air_md *sti, const struct air_type *st,
                   uint64_t base_off, unsigned base_id, struct air_argbuf *ab)
{
   unsigned field = 0, st_field = 0;
   for (unsigned i = 0; i < sti->num_ops; field++) {
      uint64_t off = 0, msize = 0, flag = 0, sub_id = 0;
      const char *first = md_str(sti, i);
      if (first && !strcmp(first, "air.struct_type_info")) {
         const struct air_md *sub_sti = (i + 1 < sti->num_ops && sti->ops[i + 1] &&
                                         sti->ops[i + 1]->kind == AIR_MD_NODE)
                                           ? sti->ops[i + 1] : NULL;
         const char *mtype = md_str(sti, i + 5), *mname = md_str(sti, i + 6);
         const char *key = md_str(sti, i + 7);
         if (!sub_sti || !md_int(sti, i + 2, &off) || !md_int(sti, i + 3, &msize) ||
             !md_int(sti, i + 4, &flag) || !mtype || !mname ||
             !key || strcmp(key, "air.indirect_argument") || !md_int(sti, i + 8, &sub_id)) {
            return air_error(c, "interface: nested struct member %u of the argument buffer %s "
                                "has an invalid air.struct_type_info descriptor",
                             field, ab->name ? ab->name : "unnamed");
         }
         const struct air_type *mt = NULL;
         if (st) {
            while (st_field < st->num_members &&
                   air_struct_member_offset(c->m, st, st_field) < off &&
                   is_struct_padding_field(st->members[st_field]))
               st_field++;
            if (st_field >= st->num_members)
               return air_error(c, "interface: the argument buffer %s declares more members "
                                   "than the %u its struct type has",
                                ab->name ? ab->name : "unnamed", st->num_members);
            mt = st->members[st_field];
            uint64_t tyoff = air_struct_member_offset(c->m, st, st_field);
            uint64_t tysize = air_type_alloc_size(c->m, mt);
            if (mt->kind != AIR_TYPE_STRUCT || tyoff != off || tysize != msize)
               return air_error(c, "interface: nested struct member %s of the argument buffer %s "
                                   "does not match the module's struct layout",
                                mname, ab->name ? ab->name : "unnamed");
            st_field++;
         } else {
            char *sub_sname = ralloc_asprintf(c->mem_ctx, "struct.%s", mtype);
            mt = named_struct(c->m, sub_sname);
            if (mt && air_type_alloc_size(c->m, mt) != msize)
               return air_error(c, "interface: nested struct member %s of the argument buffer %s "
                                   "does not match struct.%s size",
                                mname, ab->name ? ab->name : "unnamed", mtype);
         }
         i += 9;
         if (!argbuf_members_rec(c, sub_sti, mt, base_off + off, base_id + (unsigned)sub_id, ab))
            return false;
         continue;
      }

      const char *mtype = md_str(sti, i + 3), *mname = md_str(sti, i + 4);
      if (!md_int(sti, i, &off) || !md_int(sti, i + 1, &msize) ||
          !md_int(sti, i + 2, &flag) || !mtype || !mname) {
         const char *key = md_str(sti, i);
         return air_error(c, "interface: member %u of the argument buffer %s is "
                             "described by %s and not by five fields and a descriptor",
                          field, ab->name ? ab->name : "unnamed",
                          key ? key : "something this translator cannot read");
      }

      const char *key = md_str(sti, i + 5);
      const struct air_md *desc = NULL;
      if (key && !strcmp(key, "air.indirect_argument") && i + 6 < sti->num_ops &&
          sti->ops[i + 6] && sti->ops[i + 6]->kind == AIR_MD_NODE)
         desc = sti->ops[i + 6];
      if (!desc)
         return air_error(c, "interface: member %s of the argument buffer %s is followed "
                             "by %s and not by an air.indirect_argument descriptor",
                          mname, ab->name ? ab->name : "unnamed", key ? key : "nothing");
      i += 7;

      if (md_key(desc, "air.indirect_buffer") >= 0)
         return air_error(c, "interface: member %s of the argument buffer %s is itself an "
                             "argument buffer", mname, ab->name ? ab->name : "unnamed");

      const struct air_type *mt = NULL;
      if (st) {
         while (st_field < st->num_members &&
                air_struct_member_offset(c->m, st, st_field) < off &&
                is_struct_padding_field(st->members[st_field]))
            st_field++;
         if (st_field >= st->num_members)
            return air_error(c, "interface: the argument buffer %s declares more members "
                                "than the %u its struct type has",
                             ab->name ? ab->name : "unnamed", st->num_members);
         mt = st->members[st_field];
         uint64_t tyoff = air_struct_member_offset(c->m, st, st_field);
         uint64_t tysize = air_type_alloc_size(c->m, mt);
         if (tyoff != off || tysize != msize)
            return air_error(c, "interface: member %s of the argument buffer %s is %u bytes "
                                "at byte %u, and the module's struct type puts %u bytes "
                                "there at %u", mname, ab->name ? ab->name : "unnamed",
                             (unsigned)msize, (unsigned)off, (unsigned)tysize,
                             (unsigned)tyoff);
         st_field++;
      }

      int mli = md_key(desc, "air.location_index");
      uint64_t mid = 0, mspan = 1;
      if (mli < 0 || !md_int(desc, mli + 1, &mid))
         return air_error(c, "interface: member %s of the argument buffer %s has no "
                             "air.location_index", mname,
                          ab->name ? ab->name : "unnamed");

      if (md_key(desc, "air.indirect_constant") >= 0) {
         if (mt && mt->kind == AIR_TYPE_POINTER)
            return air_error(c, "interface: inline constant %s of the argument buffer %s "
                                "is declared as a pointer", mname, ab->name ? ab->name : "unnamed");
         unsigned n = ab->num_members;
         ab->members = reralloc(c->mem_ctx, ab->members, struct air_argbuf_member, n + 1);
         ab->members[n] = (struct air_argbuf_member) {
            .offset = base_off + off, .size = msize,
            .kind = AIR_RESOURCE_CONSTANT_BUFFER, .metal_id = base_id + (unsigned)mid,
            .index = ~0u, .span = 1, .slots = 0, .read = true, .is_constant = true,
            .name = mname, .type_name = mtype, .node = desc,
         };
         ab->num_members = n + 1;
         ab->has_constants = true;
         continue;
      }

      bool is_buffer = md_key(desc, "air.buffer") >= 0;
      bool is_texture = md_key(desc, "air.texture") >= 0;
      bool is_sampler = md_key(desc, "air.sampler") >= 0;
      if (is_buffer + is_texture + is_sampler != 1) {
         const char *what = descriptor_kind_key(desc);
         return air_error(c, "interface: member %s of the argument buffer %s is %s, and "
                             "this frontend binds a buffer, a texture or a sampler",
                          mname, ab->name ? ab->name : "unnamed",
                          is_buffer + is_texture + is_sampler > 1
                             ? "more than one kind at once"
                             : what ? what : "a kind this translator does not know");
      }

      bool read = md_key(desc, "air.read") >= 0 || md_key(desc, "air.read_write") >= 0 ||
                  md_key(desc, "air.sample") >= 0;
      bool write = md_key(desc, "air.write") >= 0 || md_key(desc, "air.read_write") >= 0;
      if (md_int(desc, mli + 2, &mspan) && mspan != 1) {
         if (!is_texture || write || !mspan)
            return air_error(c, "interface: member %s of the argument buffer %s spans %u "
                                "descriptor indices", mname,
                             ab->name ? ab->name : "unnamed", (unsigned)mspan);
      }

      enum air_resource_kind rk;
      unsigned addrspace;
      if (is_buffer) {
         uint64_t md_as = 0;
         if (mt) {
            if (mt->kind != AIR_TYPE_POINTER || (mt->addrspace != 1 && mt->addrspace != 2))
               return air_error(c, "interface: the buffer member %s of the argument buffer "
                                   "%s is not a device or constant pointer in the module's "
                                   "struct type", mname, ab->name ? ab->name : "unnamed");
            addrspace = mt->addrspace;
         } else if (md_key_int(desc, "air.address_space", &md_as) && (md_as == 1 || md_as == 2)) {
            addrspace = (unsigned)md_as;
         } else {
            return air_error(c, "interface: an argument buffer (%s) whose struct type %s the "
                                "module does not declare, so its members' address spaces "
                                "cannot be read", ab->name ? ab->name : "unnamed",
                             ab->type_name ? ab->type_name : "(unnamed)");
         }
         rk = addrspace == 1 ? AIR_RESOURCE_DEVICE_BUFFER : AIR_RESOURCE_CONSTANT_BUFFER;
      } else if (mspan > 1) {
         if (mt) {
            const struct air_type *et = (mt->kind == AIR_TYPE_STRUCT && mt->name &&
                                         !strncmp(mt->name, "struct.metal::array", 19) &&
                                         mt->num_members == 1 &&
                                         mt->members[0]->kind == AIR_TYPE_ARRAY &&
                                         mt->members[0]->count == mspan)
                                           ? mt->members[0]->elem : NULL;
            if (!et || et->kind != AIR_TYPE_STRUCT || !et->name ||
                strncmp(et->name, "struct.metal::", 14) || et->num_members != 1 ||
                et->members[0]->kind != AIR_TYPE_POINTER)
               return air_error(c, "interface: texture array member %s of the argument buffer "
                                   "%s is not a metal::array of metal:: texture wrappers",
                                mname, ab->name ? ab->name : "unnamed");
            addrspace = et->members[0]->addrspace;
         } else {
            addrspace = 1;
         }
         rk = AIR_RESOURCE_TEXTURE;
      } else {
         if (mt) {
            if (mt->kind != AIR_TYPE_STRUCT || !mt->name ||
                strncmp(mt->name, "struct.metal::", 14) || mt->num_members != 1 ||
                mt->members[0]->kind != AIR_TYPE_POINTER)
               return air_error(c, "interface: the %s member %s of the argument buffer %s is "
                                   "not one of Apple's one-field metal:: wrappers in the "
                                   "module's struct type", is_sampler ? "sampler" : "texture",
                                mname, ab->name ? ab->name : "unnamed");
            addrspace = mt->members[0]->addrspace;
         } else {
            addrspace = is_sampler ? 2 : 1;
         }
         rk = is_sampler ? AIR_RESOURCE_SAMPLER
                         : write ? AIR_RESOURCE_IMAGE : AIR_RESOURCE_TEXTURE;
      }

      unsigned n = ab->num_members;
      ab->members = reralloc(c->mem_ctx, ab->members, struct air_argbuf_member, n + 1);
      ab->members[n] = (struct air_argbuf_member) {
         .offset = base_off + off, .size = msize, .kind = rk,
         .metal_id = base_id + (unsigned)mid, .index = ~0u,
         .span = (unsigned)mspan, .slots = 1,
         .addrspace = addrspace, .read = read, .write = write,
         .name = mname, .type_name = mtype, .node = desc,
      };
      ab->num_members = n + 1;
   }

   if (st) {
      while (st_field < st->num_members && is_struct_padding_field(st->members[st_field]))
         st_field++;
      if (st_field != st->num_members)
         return air_error(c, "interface: the argument buffer %s declares %u members and its "
                             "struct type has %u", ab->name ? ab->name : "unnamed", field,
                          st->num_members);
   }
   return true;
}

static bool
argbuf_members(struct air_ctx *c, const struct air_md *sti, const struct air_type *st,
               struct air_argbuf *ab)
{
   return argbuf_members_rec(c, sti, st, 0, 0, ab);
}

/* Metal's argument buffer: `air.indirect_buffer`, one struct of descriptors
 * bound at one buffer index, which is how a Vulkan descriptor set reaches
 * Metal through SPIRV-Cross and how an MSL `[[id(n)]]` struct reaches it from
 * Metal itself. 7 620 of Resident Evil 4's 7 879 modules have one and 0 of
 * iOS 16.5's 671 do, so this is the one interface word that is invisible from
 * Apple's own libraries and is 97% of a game (docs/air.md section 8.2).
 *
 * The parameter does not become a buffer to read: it becomes a pointer of its
 * own class, and the member table built here is what a load through it
 * resolves against (argbuf_load). No address, no descriptor indirection, and
 * nothing of the `32bit_index_offset` model the rest of the frontend is built
 * on changes.
 */
static bool
translate_argument_buffer(struct air_ctx *c, const struct air_md *node, unsigned param)
{
   const char *name = md_key_str(node, "air.arg_name");
   const char *tn = md_key_str(node, "air.arg_type_name");

   int li = md_key(node, "air.location_index");
   uint64_t set_index = 0, span = 1, size = 0, align = 0;
   if (li < 0 || !md_int(node, li + 1, &set_index))
      return air_error(c, "interface: an argument buffer (%s) with no air.location_index",
                       name ? name : "unnamed");
   if (md_int(node, li + 2, &span) && span != 1)
      return air_error(c, "interface: an argument buffer (%s) spanning %u buffer indices",
                       name ? name : "unnamed", (unsigned)span);
   md_key_int(node, "air.buffer_size", &size);
   md_key_int(node, "air.arg_type_align_size", &align);
   unsigned alignment = align > 8 ? (unsigned)align : 8u;

   const struct air_md *sti = md_key_node(node, "air.struct_type_info");
   if (!sti)
      return air_error(c, "interface: an argument buffer (%s) with no "
                          "air.struct_type_info, so nothing says what is in it",
                       name ? name : "unnamed");

   const struct air_type *st = NULL;
   if (tn) {
      char *sname = ralloc_asprintf(c->mem_ctx, "struct.%s", tn);
      st = named_struct(c->m, sname);
   }

   /* Two sets at one Metal buffer index would have one record at draw time and
    * two sets of members wanting it. */
   for (unsigned i = 0; i < c->num_argbufs; i++)
      if (c->argbufs[i].metal_index == set_index)
         return air_error(c, "interface: two argument buffers at buffer index %u (%s and "
                             "%s)", (unsigned)set_index,
                          c->argbufs[i].name ? c->argbufs[i].name : "unnamed",
                          name ? name : "unnamed");

   c->argbufs = reralloc(c->mem_ctx, c->argbufs, struct air_argbuf, c->num_argbufs + 1);
   struct air_argbuf *ab = &c->argbufs[c->num_argbufs];
   *ab = (struct air_argbuf) {
      .metal_index = (unsigned)set_index, .size = (unsigned)size, .alignment = alignment,
      .param = param, .const_ubo_index = ~0u, .name = name, .type_name = tn,
   };
   if (!argbuf_members(c, sti, st, ab))
      return false;
   if (st) {
      uint64_t expected_size = air_type_alloc_size(c->m, st);
      if (size != expected_size)
         return air_error(c, "interface: the argument buffer %s is %u bytes, and its struct "
                             "type is %u bytes", name ? name : "unnamed", (unsigned)size,
                          (unsigned)expected_size);
   } else {
      uint64_t max_end = 0;
      for (unsigned i = 0; i < ab->num_members; i++)
         max_end = MAX2(max_end, ab->members[i].offset + ab->members[i].size);
      if (size < max_end)
         return air_error(c, "interface: the argument buffer %s is %u bytes, smaller than "
                             "its members (%u bytes)", name ? name : "unnamed",
                          (unsigned)size, (unsigned)max_end);
   }

   struct air_ptr p = { .class = AIR_PTR_ARGBUF, .binding = c->num_argbufs++ };
   c->params[param] = air_val_ptr(c, p);
   return true;
}

/* The flat per-kind index of every member of every argument buffer, and one
 * air_resource each.
 *
 * Why this is a decision and not a transcription: a member's own
 * `air.location_index` is Metal's `[[id(n)]]`, which is per set, so set 0's
 * member 0 and set 1's member 0 both say 0 and 237 of Resident Evil 4's
 * modules have two sets. One counter per kind in (set, member) order gives
 * each member a binding of its own, and `argbuf_set` / `argbuf_id` on the
 * resource keep the Metal name the application uses -- the runtime binds by
 * the first and the application names by the second.
 *
 * The counters start above the highest index a DIRECT argument of that kind
 * already took, so a member and an argument cannot land in one Gallium slot.
 * None of Resident Evil 4's 7 620 modules mixes the two -- those shaders have
 * no direct resources at all -- so this is the part of the allocation nothing
 * in reach exercises, and it is here because the collision it prevents would
 * be a draw over the wrong resource rather than an error.
 *
 * Called after every input is translated, so that the direct arguments are all
 * known, and before plan_globals, so that a shader's own `constexpr sampler`
 * still lands after the samplers the API binds.
 */
static void
allocate_argbuf_indices(struct air_ctx *c)
{
   unsigned next_buffer = 0, next_texture = 0, next_sampler = 0;
   for (unsigned i = 0; i < c->info->num_resources; i++) {
      const struct air_resource *r = &c->info->resources[i];
      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER:
      case AIR_RESOURCE_DEVICE_BUFFER:
         next_buffer = MAX2(next_buffer, r->index + 1);
         break;
      case AIR_RESOURCE_TEXTURE:
      case AIR_RESOURCE_IMAGE:
         next_texture = MAX2(next_texture, r->index + 1);
         break;
      case AIR_RESOURCE_SAMPLER:
         next_sampler = MAX2(next_sampler, r->index + 1);
         break;
      default:
         break;
      }
   }

   for (unsigned s = 0; s < c->num_argbufs; s++) {
      struct air_argbuf *ab = &c->argbufs[s];
      if (ab->has_constants) {
         ab->const_ubo_index = next_buffer++;
         struct air_resource *cr = add_resource(c, AIR_RESOURCE_CONSTANT_BUFFER,
                                                ab->const_ubo_index, ab->size, true, false,
                                                ab->param, NULL);
         if (!cr)
            return;
         cr->argbuf_set = ab->metal_index;
         cr->argbuf_id = ~0u;
      }
      for (unsigned k = 0; k < ab->num_members; k++) {
         struct air_argbuf_member *m = &ab->members[k];
         if (m->is_constant) {
            unsigned nc = c->info->num_argbuf_constants;
            c->info->argbuf_constants = reralloc(c->mem_ctx, c->info->argbuf_constants,
                                                 struct air_argbuf_const_member, nc + 1);
            c->info->argbuf_constants[nc] = (struct air_argbuf_const_member) {
               .argbuf_set = ab->metal_index,
               .argbuf_id = m->metal_id,
               .offset = (unsigned)m->offset,
               .size = (unsigned)m->size,
               .alignment = ab->alignment,
               .name = m->name,
            };
            c->info->num_argbuf_constants = nc + 1;
            continue;
         }
         if (m->span > 1) {
            unsigned avail = next_texture < 16 ? (16 - next_texture) : 1;
            m->slots = MIN2(m->span, avail);
            m->index = next_texture;
            next_texture += m->slots;
            for (unsigned j = 0; j < m->slots; j++) {
               struct air_resource *r = add_resource(c, m->kind, m->index + j, 0,
                                                     m->read, m->write, ab->param, m->node);
               if (!r)
                  return;
               r->argbuf_set = ab->metal_index;
               r->argbuf_id = m->metal_id + j;
               r->nullable = true;
            }
            continue;
         }
         switch (m->kind) {
         case AIR_RESOURCE_CONSTANT_BUFFER:
         case AIR_RESOURCE_DEVICE_BUFFER:
            m->index = next_buffer++;
            break;
         case AIR_RESOURCE_TEXTURE:
         case AIR_RESOURCE_IMAGE:
            m->index = next_texture++;
            break;
         default:
            m->index = next_sampler++;
            break;
         }
         m->slots = 1;
         struct air_resource *r = add_resource(c, m->kind, m->index, 0, m->read, m->write,
                                               ab->param, m->node);
         if (!r)
            return;   /* add_resource has said why */
         r->argbuf_set = ab->metal_index;
         r->argbuf_id = m->metal_id;
      }
   }
}

/* A load through an argument-buffer pointer RESOLVES instead of loading: the
 * byte offset names a member, and the member's descriptor names a binding.
 *
 * Keying on the byte offset rather than on the member number is what makes the
 * three shapes SPIRV-Cross emits one case. A buffer member is the pointer
 * itself; a texture or sampler member is a one-field wrapper, and the
 * `getelementptr` into that wrapper adds zero bytes -- so the texture shape
 * resolves by the same lookup as the buffer shape, with nothing special about
 * it and no second path to keep right.
 */
static struct air_val *
argbuf_load(struct air_ctx *c, struct air_ptr ptr, const struct air_type *type,
            unsigned align)
{
   struct air_argbuf *ab = &c->argbufs[ptr.binding];
   const char *abname = ab->name ? ab->name : "unnamed";

   uint64_t off = 0;
   if (ptr.offset) {
      if (!nir_src_is_const(nir_src_for_ssa(ptr.offset))) {
         if (type->kind == AIR_TYPE_POINTER) {
            struct air_argbuf_member *arr = NULL;
            for (unsigned i = 0; i < ab->num_members; i++) {
               if (!ab->members[i].is_constant && ab->members[i].span > 1) {
                  arr = &ab->members[i];
                  break;
               }
            }
            if (arr && type->addrspace == arr->addrspace && arr->slots > 0) {
               arr->used = true;
               nir_def *rel = arr->offset ? nir_iadd_imm(&c->b, ptr.offset, -(int)arr->offset)
                                          : ptr.offset;
               nir_def *elem = nir_umin_imm(&c->b, nir_ushr_imm(&c->b, rel, 3),
                                            arr->slots - 1);
               struct air_ptr tp = {
                  .class = AIR_PTR_TEXTURE,
                  .is_image = (arr->kind == AIR_RESOURCE_IMAGE),
                  .binding = 0,
                  .offset = nir_iadd_imm(&c->b, elem, arr->index),
                  .pointee = type->elem,
               };
               if (!tp.is_image) {
                  for (unsigned k = 0; k < arr->slots; k++)
                     air_texture_may_be_unbound(c, arr->index + k);
               }
               return air_val_ptr(c, tp);
            }
         } else if (ab->has_constants && ab->const_ubo_index != ~0u) {
            ab->const_used = true;
            if (c->in_entry_block)
               ab->const_unconditional = true;
            struct air_ptr up = {
               .class = AIR_PTR_UBO, .binding = ab->const_ubo_index,
               .offset = ptr.offset, .pointee = type,
            };
            nir_def *val = air_load_ptr(c, up, type, align);
            return val ? air_val_ssa(c, val) : NULL;
         }
         air_error(c, "instruction: a load through the argument buffer %s at an offset "
                      "that is not constant, which is a descriptor chosen at run time",
                   abname);
         return NULL;
      }
      off = nir_src_as_uint(nir_src_for_ssa(ptr.offset));
   }

   struct air_argbuf_member *m = NULL;
   for (unsigned i = 0; i < ab->num_members; i++) {
      struct air_argbuf_member *cand = &ab->members[i];
      if (cand->is_constant) {
         if (off >= cand->offset && off < cand->offset + cand->size) {
            m = cand;
            break;
         }
      } else if (cand->span > 1) {
         if (off >= cand->offset && off < cand->offset + 8ull * cand->span &&
             ((off - cand->offset) & 7ull) == 0) {
            m = cand;
            break;
         }
      } else if (cand->offset == off) {
         m = cand;
         break;
      }
   }
   if (!m) {
      air_error(c, "instruction: a load from byte %u of the argument buffer %s, which "
                   "declares no member there", (unsigned)off, abname);
      return NULL;
   }

   if (m->is_constant) {
      if (type->kind == AIR_TYPE_POINTER) {
         air_error(c, "instruction: a pointer load from inline constant %s of the "
                      "argument buffer %s", m->name ? m->name : "unnamed", abname);
         return NULL;
      }
      ab->const_used = true;
      if (c->in_entry_block)
         ab->const_unconditional = true;
      struct air_ptr up = {
         .class = AIR_PTR_UBO, .binding = ab->const_ubo_index,
         .offset = nir_imm_int(&c->b, off), .pointee = type,
      };
      nir_def *val = air_load_ptr(c, up, type, align);
      return val ? air_val_ssa(c, val) : NULL;
   }

   if (type->kind != AIR_TYPE_POINTER) {
      air_error(c, "instruction: a load of a value out of the argument buffer %s, whose "
                   "member %s at byte %u is a resource and has no bytes to read",
                abname, m->name ? m->name : "unnamed", (unsigned)off);
      return NULL;
   }

   if (type->addrspace != m->addrspace) {
      air_error(c, "instruction: a pointer in address space %u loaded from member %s of "
                   "the argument buffer %s, which the module's struct type puts in "
                   "address space %u", type->addrspace, m->name ? m->name : "unnamed",
                abname, m->addrspace);
      return NULL;
   }

   m->used = true;
   if (c->in_entry_block && m->span == 1)
      m->unconditional = true;

   unsigned elem_idx = (unsigned)((off - m->offset) >> 3);
   if (elem_idx >= m->slots)
      elem_idx = m->slots - 1;
   struct air_ptr p = { .binding = m->index + elem_idx, .pointee = type->elem };
   switch (m->kind) {
   case AIR_RESOURCE_CONSTANT_BUFFER: p.class = AIR_PTR_UBO; break;
   case AIR_RESOURCE_DEVICE_BUFFER:   p.class = AIR_PTR_SSBO; break;
   case AIR_RESOURCE_SAMPLER:         p.class = AIR_PTR_SAMPLER; break;
   case AIR_RESOURCE_IMAGE:
      p.class = AIR_PTR_TEXTURE;
      p.is_image = true;
      break;
   default:
      p.class = AIR_PTR_TEXTURE;
      break;
   }
   return air_val_ptr(c, p);
}
