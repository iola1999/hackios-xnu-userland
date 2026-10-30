/*
 * Calls of functions the module defines or declares, included into air_to_nir.c.
 *
 * A defined callee is translated as a nir_function, and nir_inline_functions folds the call in
 * after the whole shader is translated. A declared callee that the pipeline linked is inlined
 * at the call site by air_link.inc.
 */
/* Does this value hold a pointer anywhere inside it? */
static bool
val_holds_ptr(const struct air_val *v)
{
   if (!v)
      return false;
   if (val_is_pointerish(v))
      return true;
   if (v->kind != AIR_VAL_AGG)
      return false;
   for (unsigned i = 0; i < v->num_elems; i++)
      if (val_holds_ptr(v->elems[i]))
         return true;
   return false;
}

/* Does this value hold a def the CALLER's impl built? A pointer is a resolution
 * rather than a def, and an aggregate of undef fields (Metal's `[8 x i8]` tail
 * padding, passed as undef) holds nothing at all; anything else would be a use
 * of one function's def inside another.
 */
static bool
val_carries_def(const struct air_val *v)
{
   if (!v)
      return false;
   if (v->kind == AIR_VAL_SSA)
      return true;
   if (v->kind != AIR_VAL_AGG)
      return false;
   for (unsigned i = 0; i < v->num_elems; i++)
      if (val_carries_def(v->elems[i]))
         return true;
   return false;
}

/* What a call passes for one argument, decided at the call site and read again
 * inside the callee: one decision, so the two sides cannot disagree about the
 * order of the parameters.
 *
 * A pointer argument travels as CALL_PARAM_NONE because it is a resolution and
 * not a value, with one exception: a pointer whose byte offset was computed in
 * the caller -- `getelementptr` on a run-time index -- carries a def of the
 * caller's impl in air_ptr.offset, and a def cannot cross into another impl.
 * That offset is passed as a parameter of its own and the callee rebuilds the
 * pointer around it.
 */
enum call_param_kind {
   CALL_PARAM_NONE,
   CALL_PARAM_VALUE,
   CALL_PARAM_OFFSET,
};

/* A function of the module, translated as a nir_function so that
 * nir_inline_functions can fold it into its callers afterwards.
 *
 * Translating it in place at the call site was the other option and is worse:
 * the callees here are not straight-line code -- FilmGrainLuma has 17 blocks
 * and 24 branches -- so inlining by hand would mean reproducing a CFG inside
 * another one, with its own block map and phis. NIR already has that pass.
 *
 * The return goes through an out-parameter at index 0, with is_return set,
 * because a NIR call has no destination; this is the convention spirv_to_nir
 * uses (vtn_cfg.c) and the one nir_inline_functions expects.
 *
 * The per-function tables are swapped rather than shared: values, blocks and
 * phis are all keyed on this function's own objects, and leaving the caller's
 * entries in place would let a callee resolve a value it cannot see.
 */
static nir_function *
translate_callee(struct air_ctx *c, const struct air_function *f,
                 struct air_val **argv, unsigned char *kind)
{
   /* A texture, sampler or buffer argument is not a value in this frontend: it
    * is resolved at translation time into a binding, and tex_handle wants an
    * AIR_VAL_PTR rather than an SSA def. Such an argument is bound from the
    * caller's own resolution and takes no NIR parameter, which specialises the
    * callee to its call site -- so it is not cached. Each of the four modules
    * that needs this has exactly one call site, counted beforehand.
    *
    * A pointer carrying a byte offset the caller computed is the same crossing
    * in a smaller piece: the offset is an nir_def of the caller's impl, so it
    * travels as a parameter of its own and the callee puts the pointer back
    * together around it. Before that, such a call was refused, which is what
    * CoreAnimation's glass and variable-blur shaders ran into on the phone.
    */
   bool specialised = false;
   for (unsigned i = 0; i < f->num_args; i++) {
      /* A struct holding a pointer counts as one too: CoreImage passes a
       * texture and a sampler as metal::texture2d and metal::sampler, which
       * are one-field structs wrapping the address-space-1 and -2 pointers.
       * Passing such a struct as a NIR value would hand the callee something
       * it then takes extractvalue of, and extractvalue of a value that is not
       * an aggregate here is exactly the error this used to produce.
       */
      bool ptr = argv && argv[i] && (argv[i]->kind == AIR_VAL_PTR ||
                                     val_holds_ptr(argv[i]));
      if (argv && argv[i] && argv[i]->kind == AIR_VAL_PTR_SELECT) {
         air_error(c, "intrinsic: a call passing a pointer chosen at run time as "
                      "argument %u", i);
         return NULL;
      }
      if (ptr) {
         /* The pointer itself is a resolution, which the callee shares as it
          * is; its byte offset is a def of the caller's impl and travels as a
          * parameter instead, so that the callee's own builder holds the only
          * use of it. A constant offset travels the same way even though it
          * folds: a `load_const` also belongs to the impl that built it, and one
          * used inside another function's call is the same crossing -- it was
          * what nir_lower_goto_ifs asserted on the first module this let
          * through.
          */
         kind[i] = argv[i]->kind == AIR_VAL_PTR && argv[i]->ptr.offset
                      ? CALL_PARAM_OFFSET : CALL_PARAM_NONE;
      } else if (ret_member_is_number(f->args[i].type)) {
         kind[i] = CALL_PARAM_VALUE;
      } else if (val_carries_def(argv[i])) {
         /* An array or a nested struct by value with something in it: NIR has
          * no parameter that could carry it, and handing the callee the
          * caller's defs would be a use outside its own impl.
          */
         air_error(c, "intrinsic: a call passing argument %u (%s) by value, which "
                      "has no parameter to travel as",
                   i, f->args[i].type->kind == AIR_TYPE_ARRAY ? "an array" :
                      f->args[i].type->kind == AIR_TYPE_STRUCT ? "a struct" :
                      "a value of no width");
         return NULL;
      } else {
         /* Metal's `[8 x i8]` tail padding, passed as undef: nothing to carry,
          * and nothing the callee can read out of it either.
          */
         kind[i] = CALL_PARAM_NONE;
      }
      specialised |= ptr;
   }

   struct hash_entry *e = specialised ? NULL : _mesa_hash_table_search(c->fns, f);
   if (e)
      return e->data;

   if (f->is_declaration || !f->num_blocks) {
      air_error(c, "intrinsic: a call of %s, which the module only declares",
                f->base.name ? f->base.name : "a function");
      return NULL;
   }

   const struct air_type *rt = f->fn_type->elem;   /* FUNCTION: the return type */
   bool has_ret = rt && rt->kind != AIR_TYPE_VOID;
   /* A struct return takes one out-parameter per member rather than one for the
    * whole value. Four of QuartzCore's vertex shaders are nothing but a wrapper
    * around such a function -- `tail call fastcc`, then extractvalue of each
    * field -- and before this they stopped at the call with "an aggregate where
    * a number belongs", which was emit_return inside the callee asking for the
    * returned struct as a register.
    */
   bool ret_struct = has_ret && rt->kind == AIR_TYPE_STRUCT;
   unsigned nret = 0;
   if (ret_struct) {
      for (unsigned i = 0; i < rt->num_members; i++)
         nret += ret_member_is_number(rt->members[i]);
   } else if (has_ret) {
      nret = 1;
   }
   unsigned nparams = nret;
   for (unsigned i = 0; i < f->num_args; i++)
      nparams += kind[i] != CALL_PARAM_NONE;
   nir_function *fn = nir_function_create(c->nir, f->base.name ? f->base.name : "callee");
   fn->num_params = nparams;
   fn->params = rzalloc_array(c->nir, nir_parameter, MAX2(nparams, 1));
   fn->should_inline = true;

   unsigned idx = 0;
   const struct glsl_type *ret_glsl = NULL;
   if (ret_struct) {
      for (unsigned i = 0; i < rt->num_members; i++) {
         if (!ret_member_is_number(rt->members[i]))
            continue;
         fn->params[idx++] = (nir_parameter){
            .num_components = 1, .bit_size = 32,
            .is_return = true,
            .mode = nir_var_function_temp,
            .type = param_glsl_type(c, rt->members[i]),
         };
      }
   } else if (has_ret) {
      ret_glsl = param_glsl_type(c, rt);
      fn->params[idx++] = (nir_parameter){
         .num_components = 1, .bit_size = 32,
         .is_return = true,
         .mode = nir_var_function_temp,
         .type = ret_glsl,
      };
   }
   for (unsigned i = 0; i < f->num_args; i++) {
      const struct air_type *at = f->args[i].type;
      if (kind[i] == CALL_PARAM_OFFSET) {
         /* the pointer's byte offset, 32-bit like every offset in air_ptr */
         fn->params[idx++] = (nir_parameter){ .num_components = 1, .bit_size = 32 };
         continue;
      }
      if (kind[i] != CALL_PARAM_VALUE)
         continue;
      fn->params[idx++] = (nir_parameter){
         .num_components = (uint8_t)air_nir_components(at),
         .bit_size = (uint8_t)air_nir_bit_size(c, at),
      };
   }
   fn->num_params = idx;   /* the pointers took no parameter */

   /* cached before the body is translated, so that a callee which calls itself
    * meets its own declaration rather than being translated for ever */
   if (!specialised)
      _mesa_hash_table_insert(c->fns, f, fn);
   else
      _mesa_hash_table_insert(c->fns, fn, fn);

   nir_builder saved_b = c->b;
   const struct air_function *saved_fn = c->fn;
   struct hash_table *sv = c->values, *sb = c->blocks,
                     *sp = c->phi_cursors, *sr = c->phi_regs;
   nir_deref_instr *saved_ret = c->ret_deref;
   nir_deref_instr **saved_ret_fields = c->ret_fields;
   unsigned saved_num_ret_fields = c->num_ret_fields;
   nir_block *saved_inline_entry = c->inline_entry, *saved_inline_exit = c->inline_exit;
   /* a constant of the caller is a def of the caller's impl, so the place
    * constants are built belongs to the function being translated, like the
    * values table above */
   nir_cursor saved_consts = c->const_cursor;

   c->fn = f;
   c->values = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->blocks = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->phi_cursors = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->phi_regs = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->b = nir_builder_at(nir_before_impl(nir_function_impl_create(fn)));
   /* as for the entry point: this translator emits an unstructured CFG with
    * nir_goto, and nir_goto_if asserts on a structured impl. A new impl is
    * structured by default, so a callee crashed on its first branch -- which
    * is what the corpus's reader count caught, 456 falling to 454. */
   c->b.impl->structured = false;
   c->const_cursor = nir_before_impl(c->b.impl);
   c->ret_deref = NULL;
   c->ret_fields = NULL;
   c->num_ret_fields = 0;
   c->inline_entry = NULL;
   c->inline_exit = NULL;

   /* this function's own globals, built in its own builder */
   materialise_globals(c);

   bool ok = true;
   /* The caller passes each deref's own def and the callee casts it back, which
    * is the convention nir_functions.c documents: load_param is replaced by the
    * call's source at inline time and nir_opt_deref removes the cast afterwards.
    */
   if (ret_struct) {
      c->ret_fields = rzalloc_array(c->mem_ctx, nir_deref_instr *,
                                    MAX2(rt->num_members, 1));
      c->num_ret_fields = rt->num_members;
      unsigned p = 0;
      for (unsigned i = 0; i < rt->num_members; i++) {
         if (!ret_member_is_number(rt->members[i]))
            continue;
         c->ret_fields[i] =
            nir_build_deref_cast(&c->b, nir_load_param(&c->b, p++),
                                 nir_var_function_temp,
                                 param_glsl_type(c, rt->members[i]), 0);
      }
   } else if (has_ret) {
      c->ret_deref = nir_build_deref_cast(&c->b, nir_load_param(&c->b, 0),
                                          nir_var_function_temp, ret_glsl, 0);
   }
   unsigned pidx = nret;
   for (unsigned i = 0; i < f->num_args && ok; i++) {
      if (kind[i] == CALL_PARAM_OFFSET) {
         /* the caller's resolution, with the offset it computed replaced by the
          * parameter that carried it here */
         struct air_ptr p = argv[i]->ptr;
         p.offset = nir_load_param(&c->b, pidx++);
         air_set(c, &f->args[i], air_val_ptr(c, p));
         continue;
      }
      if (kind[i] != CALL_PARAM_VALUE) {
         air_set(c, &f->args[i], argv[i]);   /* the caller's own resolution */
         continue;
      }
      air_set(c, &f->args[i], air_val_ssa(c, nir_load_param(&c->b, pidx++)));
   }
   if (ok)
      ok = translate_body(c);

   c->b = saved_b;
   c->fn = saved_fn;
   c->values = sv;
   c->blocks = sb;
   c->phi_cursors = sp;
   c->phi_regs = sr;
   c->const_cursor = saved_consts;
   c->ret_deref = saved_ret;
   c->ret_fields = saved_ret_fields;
   c->num_ret_fields = saved_num_ret_fields;
   c->inline_entry = saved_inline_entry;
   c->inline_exit = saved_inline_exit;
   return ok ? fn : NULL;
}

/* A call of a function this module defines. The callee is translated as a
 * nir_function (above) and the call becomes a nir_call, which
 * nir_inline_functions folds in after the whole shader is translated.
 *
 * This used to be refused outright, with a comment saying no system shader
 * needed it because Apple's own inliner had already flattened every one. That
 * was not true: six entry points of the corpus call a function in their module
 * and five of them are compute kernels, which is how the claim went unnoticed
 * -- nothing dispatched a kernel until there was something to dispatch it with.
 */
bool
air_call_in_module(struct air_ctx *c, const struct air_inst *in, const char *name)
{
   const struct air_function *f = NULL;
   for (unsigned i = 0; i < c->m->num_functions; i++) {
      const char *fname = c->m->functions[i]->base.name;
      if (fname && !strcmp(fname, name)) {
         f = c->m->functions[i];
         break;
      }
   }
   if (!f)
      return air_error(c, "intrinsic: a call of %s, which is not a function of this "
                          "module", name);

   if (in->num_operands - 1 != f->num_args)
      return air_error(c, "intrinsic: a call of %s with %u arguments, wanting %u", name,
                       in->num_operands - 1, f->num_args);

   if (f->is_declaration) {
      const struct air_linked_function *lf = linked_function(c, visible_reference_name(c, f));
      if (lf)
         return call_linked_function(c, in, f, lf);
   }

   /* resolved in the CALLER's value map, which is the live one here */
   struct air_val **argv = rzalloc_array(c->mem_ctx, struct air_val *,
                                         MAX2(f->num_args, 1));
   /* what each argument travels as, filled in by translate_callee and read
    * again here: one array, so the call's sources come out in the order the
    * callee's parameters were made
    */
   unsigned char *kind = rzalloc_array(c->mem_ctx, unsigned char, MAX2(f->num_args, 1));
   for (unsigned i = 0; i < f->num_args; i++) {
      argv[i] = air_get(c, in->operands[i + 1]);
      if (!argv[i])
         return false;
   }

   nir_function *fn = translate_callee(c, f, argv, kind);
   if (!fn)
      return false;   /* translate_callee has said why */

   const struct air_type *rt = f->fn_type->elem;   /* FUNCTION: the return type */
   bool has_ret = rt && rt->kind != AIR_TYPE_VOID;
   bool ret_struct = has_ret && rt->kind == AIR_TYPE_STRUCT;

   nir_variable *tmp = NULL;
   /* One local per member for a struct return, in the member order
    * translate_callee made its parameters from; the ones with no parameter stay
    * NULL and the aggregate's element for them stays NULL too, which is the
    * same thing an extractvalue of an undef field already reads.
    */
   nir_variable **fields = ret_struct ?
      rzalloc_array(c->mem_ctx, nir_variable *, MAX2(rt->num_members, 1)) : NULL;
   nir_call_instr *call = nir_call_instr_create(c->nir, fn);
   unsigned idx = 0;
   if (ret_struct) {
      for (unsigned i = 0; i < rt->num_members; i++) {
         if (!ret_member_is_number(rt->members[i]))
            continue;
         fields[i] = nir_local_variable_create(c->b.impl,
                                               param_glsl_type(c, rt->members[i]),
                                               "callret");
         call->params[idx++] =
            nir_src_for_ssa(&nir_build_deref_var(&c->b, fields[i])->def);
      }
   } else if (has_ret) {
      tmp = nir_local_variable_create(c->b.impl, param_glsl_type(c, rt), "callret");
      call->params[idx++] = nir_src_for_ssa(&nir_build_deref_var(&c->b, tmp)->def);
   }
   /* operand 0 is the callee; the arguments follow it */
   for (unsigned i = 0; i < f->num_args; i++) {
      if (kind[i] == CALL_PARAM_OFFSET) {
         call->params[idx++] = nir_src_for_ssa(argv[i]->ptr.offset);
         continue;
      }
      if (kind[i] != CALL_PARAM_VALUE)
         continue;
      nir_def *a = air_get_ssa(c, in->operands[i + 1]);
      if (!a)
         return air_error(c, "intrinsic: argument %u of %s is not a value this can pass",
                          i, name);
      call->params[idx++] = nir_src_for_ssa(a);
   }
   nir_builder_instr_insert(&c->b, &call->instr);

   if (ret_struct) {
      struct air_val *agg = air_val_agg(c, rt->num_members);
      for (unsigned i = 0; i < rt->num_members; i++)
         if (fields[i])
            agg->elems[i] = air_val_ssa(c, nir_load_var(&c->b, fields[i]));
      air_set(c, &in->base, agg);
   } else if (has_ret) {
      air_set(c, &in->base, air_val_ssa(c, nir_load_var(&c->b, tmp)));
   }
   return true;
}
