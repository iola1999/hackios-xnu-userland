/* Bitcode metadata block reader, included into air_module.c. */

/* -- metadata ----------------------------------------------------------- */

static struct air_md *
new_md(struct reader *r, enum air_md_kind k)
{
   struct air_md *md = rzalloc(r->m, struct air_md);
   md->kind = k;
   md->id = r->num_md;
   md->function_local = r->cur_fn != NULL;
   if (r->num_md == r->cap_md) {
      r->cap_md = MAX2(256, r->cap_md * 2);
      r->md = reralloc(r->m, r->md, struct air_md *, r->cap_md);
   }
   r->md[r->num_md++] = md;
   return md;
}

static bool
read_md_strings(struct reader *r, const struct air_bs_entry *e)
{
   if (e->num_ops < 2 || !e->blob)
      return fail(r, "METADATA_STRINGS without counts or blob");
   unsigned count = e->ops[0];
   size_t lengths_size = e->ops[1];
   if (lengths_size > e->blob_size)
      return fail(r, "METADATA_STRINGS offsets past the blob");

   /* the lengths are a little bitstream of VBR6 values */
   struct air_bitstream lens = {
      .mem_ctx = r->m,
      .data = e->blob,
      .size_bits = lengths_size * 8,
   };
   const char *chars = (const char *)e->blob + lengths_size;
   size_t avail = e->blob_size - lengths_size;
   size_t off = 0;
   for (unsigned i = 0; i < count; i++) {
      uint64_t len = 0, shift = 0, chunk;
      do {
         if (lens.pos + 6 > lens.size_bits)
            return fail(r, "METADATA_STRINGS lengths run out");
         chunk = 0;
         for (unsigned b = 0; b < 6; b++, lens.pos++)
            chunk |= (uint64_t)((lens.data[lens.pos >> 3] >> (lens.pos & 7)) & 1) << b;
         len |= (chunk & 31) << shift;
         shift += 5;
      } while (chunk & 32);
      if (off + len > avail)
         return fail(r, "METADATA_STRINGS string past the blob");
      struct air_md *md = new_md(r, AIR_MD_STRING);
      md->str = chars + off;
      md->len = len;
      off += len;
   }
   return true;
}

static bool
md_other_code(unsigned code)
{
   /* records that define one metadata node each and that the frontend does
    * not look into: locations and the DI* family
    */
   return code == METADATA_LOCATION || (code >= 12 && code <= 34) || code == 37 ||
          code == 40 || code == 41 || code == 44 || code == 45 || code == 47;
}

static bool
read_metadata_block(struct reader *r)
{
   unsigned first = r->num_md;

   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         break;
      if (e.kind == AIR_BS_ENTER_BLOCK) {
         air_bs_skip_block(&r->bs);
         continue;
      }
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "metadata block ends early");

      struct air_md *md;
      switch (e.id) {
      case METADATA_STRINGS:
         if (!read_md_strings(r, &e))
            return false;
         break;
      case METADATA_STRING_OLD:
         md = new_md(r, AIR_MD_STRING);
         md->str = air_bs_ops_to_string(r->m, e.ops, e.num_ops);
         md->len = e.num_ops;
         break;
      case METADATA_VALUE:
         if (e.num_ops != 2)
            return fail(r, "METADATA_VALUE of %u operands", e.num_ops);
         md = new_md(r, AIR_MD_VALUE);
         md->value_type = type_by_id(r, e.ops[0]);
         md->value_id = e.ops[1];
         break;
      case METADATA_NODE:
      case METADATA_DISTINCT_NODE:
         md = new_md(r, AIR_MD_NODE);
         md->distinct = e.id == METADATA_DISTINCT_NODE;
         md->num_ops = e.num_ops;
         md->ops = rzalloc_array(r->m, struct air_md *, MAX2(e.num_ops, 1));
         md->pending = rzalloc_array(r->m, unsigned, MAX2(e.num_ops, 1));
         for (unsigned i = 0; i < e.num_ops; i++)
            md->pending[i] = e.ops[i]; /* id + 1, 0 for null */
         break;
      case METADATA_NAME:
         r->pending_md_name = air_bs_ops_to_string(r->m, e.ops, e.num_ops);
         break;
      case METADATA_NAMED_NODE: {
         if (!r->pending_md_name)
            return fail(r, "NAMED_NODE without a NAME");
         struct air_module *m = r->m;
         m->named_md = reralloc(m, m->named_md, struct air_named_md, m->num_named_md + 1);
         struct air_named_md *n = &m->named_md[m->num_named_md++];
         n->name = r->pending_md_name;
         r->pending_md_name = NULL;
         n->num_ops = e.num_ops;
         n->ops = rzalloc_array(m, struct air_md *, MAX2(e.num_ops, 1));
         /* resolved below: these ids are plain, not + 1 */
         for (unsigned i = 0; i < e.num_ops; i++)
            n->ops[i] = (struct air_md *)(uintptr_t)(e.ops[i] + 1);
         break;
      }
      case METADATA_KIND: {
         if (e.num_ops < 1)
            return fail(r, "METADATA_KIND without an id");
         struct air_module *m = r->m;
         unsigned id = e.ops[0];
         if (id >= m->num_md_kinds) {
            m->md_kind_names = rerzalloc(m, m->md_kind_names, const char *, m->num_md_kinds, id + 1);
            m->num_md_kinds = id + 1;
         }
         m->md_kind_names[id] = air_bs_ops_to_string(m, e.ops + 1, e.num_ops - 1);
         break;
      }
      case METADATA_INDEX_OFFSET:
      case METADATA_INDEX:
      case METADATA_GLOBAL_DECL_ATTACHMENT:
         break; /* for lazy loading and global attachments; nothing AIR needs */
      case METADATA_OLD_NODE:
      case METADATA_OLD_FN_NODE:
      case METADATA_ARG_LIST:
         return fail(r, "unsupported metadata record %u", e.id);
      default:
         if (md_other_code(e.id)) {
            md = new_md(r, AIR_MD_OTHER);
            md->code = e.id;
            break;
         }
         return fail(r, "unknown metadata record %u", e.id);
      }
      if (r->m->error[0])
         return false;
   }

   /* nodes may name later nodes */
   for (unsigned i = first; i < r->num_md; i++) {
      struct air_md *md = r->md[i];
      if (md->kind != AIR_MD_NODE || !md->pending)
         continue;
      for (unsigned k = 0; k < md->num_ops; k++) {
         unsigned id = md->pending[k];
         if (id == 0)
            continue;
         if (id - 1 >= r->num_md)
            return fail(r, "metadata node names !%u of %u", id - 1, r->num_md);
         md->ops[k] = r->md[id - 1];
      }
      md->pending = NULL;
   }
   return true;
}

/* values named by metadata, once the values exist */
static bool
resolve_md_values(struct reader *r, unsigned first)
{
   for (unsigned i = first; i < r->num_md; i++) {
      struct air_md *md = r->md[i];
      if (md->kind == AIR_MD_VALUE && !md->value)
         if (!(md->value = value_by_id(r, md->value_id)))
            return false;
   }
   return true;
}

static bool
resolve_named_md(struct reader *r)
{
   struct air_module *m = r->m;
   for (unsigned i = 0; i < m->num_named_md; i++) {
      struct air_named_md *n = &m->named_md[i];
      for (unsigned k = 0; k < n->num_ops; k++) {
         uintptr_t id = (uintptr_t)n->ops[k] - 1;
         if (id >= r->num_md)
            return fail(r, "named metadata %s names !%u", n->name, (unsigned)id);
         n->ops[k] = r->md[id];
      }
   }
   return true;
}

static bool
read_metadata_kind_block(struct reader *r)
{
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "metadata kind block ends early");
      if (e.id == METADATA_KIND && e.num_ops >= 1) {
         struct air_module *m = r->m;
         unsigned id = e.ops[0];
         if (id >= m->num_md_kinds) {
            m->md_kind_names = rerzalloc(m, m->md_kind_names, const char *, m->num_md_kinds, id + 1);
            m->num_md_kinds = id + 1;
         }
         m->md_kind_names[id] = air_bs_ops_to_string(m, e.ops + 1, e.num_ops - 1);
      }
   }
}

