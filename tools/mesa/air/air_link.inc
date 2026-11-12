/*
 * Functions linked into a shader at pipeline time, included into air_to_nir.c.
 *
 * A Metal shader can call an `extern [[visible]]` function it only declares; the pipeline
 * supplies the body through MTLLinkedFunctions, either a visible function of another library or
 * a function a stitching graph made. RenderBox's custom effects in iOS 26 are this: the fragment
 * shader calls `custom_fn`, a graph over [[stitchable]] functions of RenderBox and
 * IconRendering. The graph is given as struct air_linked_function, and each node is translated
 * INLINE into the caller: its module is read once, its globals planned once, and its blocks are
 * emitted between a goto from the call site and a block the call continues in. Inline rather
 * than as a nir_function because the nodes pass textures, pointers and structs holding them to
 * one another, which a NIR parameter cannot carry.
 *
 * Function constants of a linked module read the caller's values by index; the stitchable
 * functions met so far declare none.
 */

struct air_linked_module {
   const void *bitcode;
   const struct air_module *m;
   struct air_global_plan *plan;
};

/* The name the module's air.visible_function_references gives a declared function, or NULL. */
static const char *
visible_reference_name(struct air_ctx *c, const struct air_function *f)
{
   const struct air_named_md *n = named_md(c->m, "air.visible_function_references");
   for (unsigned i = 0; i < (n ? n->num_ops : 0); i++) {
      const struct air_md *node = n->ops[i];
      if (!node || node->num_ops < 3 || !node->ops[1] || node->ops[1]->kind != AIR_MD_VALUE ||
          node->ops[1]->value != &f->base)
         continue;
      return md_str(node, 2);
   }
   return NULL;
}

static const struct air_linked_function *
linked_function(struct air_ctx *c, const char *name)
{
   for (unsigned i = 0; name && c->options && i < c->options->num_linked; i++)
      if (c->options->linked[i].name && !strcmp(c->options->linked[i].name, name))
         return &c->options->linked[i];
   return NULL;
}

static struct air_linked_module *
linked_module(struct air_ctx *c, const void *bitcode, size_t size)
{
   for (unsigned i = 0; i < c->num_linked_modules; i++)
      if (c->linked_modules[i].bitcode == bitcode)
         return &c->linked_modules[i];

   const char *err = NULL;
   const struct air_module *m = air_module_read(c->mem_ctx, bitcode, size, &err);
   if (!m) {
      air_error(c, "link: a linked module could not be read: %s", err ? err : "?");
      return NULL;
   }
   /* plan_globals works on c->m and c->global_plan; the linked module's inline samplers
    * and scratch go after the caller's own
    */
   const struct air_module *saved_m = c->m;
   struct air_global_plan *saved_plan = c->global_plan;
   c->m = m;
   plan_globals(c);
   struct air_global_plan *plan = c->global_plan;
   c->m = saved_m;
   c->global_plan = saved_plan;
   if (c->error[0])
      return NULL;

   c->linked_modules = reralloc(c->mem_ctx, c->linked_modules, struct air_linked_module,
                                c->num_linked_modules + 1);
   c->linked_modules[c->num_linked_modules] = (struct air_linked_module) {
      .bitcode = bitcode, .m = m, .plan = plan,
   };
   return &c->linked_modules[c->num_linked_modules++];
}

static const struct air_function *
module_function(const struct air_module *m, const char *name)
{
   for (unsigned i = 0; i < m->num_functions; i++) {
      const struct air_function *f = m->functions[i];
      if (f->base.name && !strcmp(f->base.name, name) && !f->is_declaration)
         return f;
   }
   return NULL;
}

/* A return inside an inlined function: the value is kept, or stored to the register when the
 * function returns in more than one place. */
static bool
inline_return(struct air_ctx *c, const struct air_inst *in)
{
   if (in->num_operands == 0)
      return true;
   struct air_val *v = air_get(c, in->operands[0]);
   if (!v)
      return false;
   if (!c->inline_ret_reg) {
      c->inline_ret = v;
      return true;
   }
   if (v->kind != AIR_VAL_SSA)
      return air_error(c, "link: a linked function returns something other than a number "
                          "from more than one place");
   nir_store_reg(&c->b, v->def, c->inline_ret_reg);
   return true;
}

/* Translates f of the linked module lm in place of a call, with argv as its arguments, and
 * returns what it returned (NULL for void, or on an error, which is then set). The caller's
 * cursor ends in the block after the inlined body.
 */
static struct air_val *
inline_linked_function(struct air_ctx *c, struct air_linked_module *lm,
                       const struct air_function *f, struct air_val **argv)
{
   const struct air_type *rt = f->fn_type->elem;
   bool has_ret = rt && rt->kind != AIR_TYPE_VOID;
   unsigned rets = 0;
   for (unsigned bi = 0; bi < f->num_blocks; bi++) {
      const struct air_block *blk = &f->blocks[bi];
      rets += blk->num_insts && blk->insts[blk->num_insts - 1]->op == AIR_OP_RET;
   }

   const struct air_module *saved_m = c->m;
   struct air_global_plan *saved_plan = c->global_plan;
   const struct air_function *saved_fn = c->fn;
   struct hash_table *sv = c->values, *sb = c->blocks,
                     *sp = c->phi_cursors, *sr = c->phi_regs;
   nir_block *saved_entry = c->inline_entry, *saved_exit = c->inline_exit;
   struct air_val *saved_ret = c->inline_ret;
   nir_def *saved_ret_reg = c->inline_ret_reg;

   c->m = lm->m;
   c->global_plan = lm->plan;
   c->fn = f;
   c->values = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->blocks = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->phi_cursors = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->phi_regs = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->inline_ret = NULL;
   c->inline_ret_reg = NULL;
   if (has_ret && rets > 1) {
      nir_builder entry = nir_builder_at(nir_before_impl(c->b.impl));
      c->inline_ret_reg = nir_decl_reg(&entry, air_nir_components(rt),
                                       air_nir_bit_size(c, rt), 0);
   }

   materialise_globals(c);
   for (unsigned i = 0; i < f->num_args; i++)
      air_set(c, &f->args[i], argv[i]);

   nir_block *entry = nir_block_create(c->b.impl);
   exec_list_push_tail(&c->b.impl->body, &entry->cf_node.node);
   entry->cf_node.parent = &c->b.impl->cf_node;
   nir_block *exit = nir_block_create(c->b.impl);
   exec_list_push_tail(&c->b.impl->body, &exit->cf_node.node);
   exit->cf_node.parent = &c->b.impl->cf_node;
   nir_goto(&c->b, entry);
   c->inline_entry = entry;
   c->inline_exit = exit;

   bool ok = translate_body(c);
   c->b.cursor = nir_after_block(exit);

   struct air_val *result = NULL;
   if (ok && has_ret) {
      if (c->inline_ret_reg)
         result = air_val_ssa(c, nir_load_reg(&c->b, c->inline_ret_reg));
      else if (c->inline_ret)
         result = c->inline_ret;
      else
         ok = air_error(c, "link: %s returns a value and has no return",
                        f->base.name ? f->base.name : "a linked function");
   }

   c->m = saved_m;
   c->global_plan = saved_plan;
   c->fn = saved_fn;
   c->values = sv;
   c->blocks = sb;
   c->phi_cursors = sp;
   c->phi_regs = sr;
   c->inline_entry = saved_entry;
   c->inline_exit = saved_exit;
   c->inline_ret = saved_ret;
   c->inline_ret_reg = saved_ret_reg;
   return ok ? result : NULL;
}

/* A call of the declared function decl, which the pipeline linked as lf: the graph's nodes
 * inlined in dependency order, and the output node's result as the call's. */
static bool
call_linked_function(struct air_ctx *c, const struct air_inst *in,
                     const struct air_function *decl, const struct air_linked_function *lf)
{
   unsigned nin = in->num_operands - 1;
   struct air_val **inputs = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(nin, 1));
   for (unsigned i = 0; i < nin; i++) {
      inputs[i] = air_get(c, in->operands[i + 1]);
      if (!inputs[i])
         return false;
   }
   if (lf->output >= lf->num_nodes)
      return air_error(c, "link: %s has %u nodes and its output is node %u", lf->name,
                       lf->num_nodes, lf->output);

   struct air_val **results = rzalloc_array(c->mem_ctx, struct air_val *,
                                            MAX2(lf->num_nodes, 1));
   bool *done = rzalloc_array(c->mem_ctx, bool, MAX2(lf->num_nodes, 1));
   for (unsigned pass = 0, left = lf->num_nodes; left; pass++) {
      bool progress = false;
      for (unsigned k = 0; k < lf->num_nodes; k++) {
         const struct air_stitch_node *node = &lf->nodes[k];
         bool ready = !done[k];
         for (unsigned a = 0; ready && node->args && a < node->num_args; a++)
            ready = node->args[a] >= 0 || done[-1 - node->args[a]];
         for (unsigned a = 0; ready && a < node->num_after; a++)
            ready = node->after[a] < lf->num_nodes && done[node->after[a]];
         if (!ready)
            continue;

         struct air_linked_module *lm = linked_module(c, node->bitcode, node->size);
         if (!lm)
            return false;
         const struct air_function *f = module_function(lm->m, node->function);
         if (!f)
            return air_error(c, "link: node %u of %s calls %s, which its module does not "
                                "define", k, lf->name, node->function);
         unsigned nargs = node->args ? node->num_args : f->num_args;
         if (f->num_args != nargs)
            return air_error(c, "link: node %u of %s passes %u arguments to %s, which takes "
                                "%u", k, lf->name, nargs, node->function, f->num_args);
         struct air_val **argv = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(nargs, 1));
         for (unsigned a = 0; a < nargs; a++) {
            int from = node->args ? node->args[a] : (int)a;
            if (from >= 0 && (unsigned)from >= nin)
               return air_error(c, "link: node %u of %s reads argument %d of a call with %u",
                                k, lf->name, from, nin);
            if (from < 0 && (unsigned)(-1 - from) >= lf->num_nodes)
               return air_error(c, "link: node %u of %s reads node %d of %u", k, lf->name,
                                -1 - from, lf->num_nodes);
            argv[a] = from >= 0 ? inputs[from] : results[-1 - from];
            if (!argv[a])
               return air_error(c, "link: node %u of %s reads the result of a node that "
                                   "returns nothing", k, lf->name);
         }
         results[k] = inline_linked_function(c, lm, f, argv);
         if (c->error[0])
            return false;
         done[k] = true;
         left--;
         progress = true;
      }
      if (!progress)
         return air_error(c, "link: the nodes of %s depend on one another in a cycle",
                          lf->name);
   }

   const struct air_type *rt = decl->fn_type->elem;
   if (rt && rt->kind != AIR_TYPE_VOID) {
      if (!results[lf->output])
         return air_error(c, "link: the output node of %s returns nothing and the call "
                             "wants a value", lf->name);
      air_set(c, &in->base, results[lf->output]);
   }
   return true;
}
