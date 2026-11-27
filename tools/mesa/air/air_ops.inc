/* ALU, cast, compare, GEP, and aggregate instruction helpers, included into air_to_nir.c. */

/* -- instructions ------------------------------------------------------- */

/* A load through a pointer this frontend could not resolve: load through BOTH
 * sides of the choice and select the values. That is the only form NIR has --
 * there is no select of pointers in the 32bit_index_offset model -- and it is
 * not free of consequence, so it is said out loud: both sides are read, so a
 * side that is not a valid place to read from is read anyway. In the shaders
 * that need this one side is a module constant array and the other a buffer
 * the pipeline binds, so both are readable; a pipeline that bound neither
 * would read a descriptor it did not set up.
 */
static nir_def *
load_through_choice(struct air_ctx *c, struct air_val *v, const struct air_type *t,
                    unsigned align)
{
   if (v->kind == AIR_VAL_PTR)
      return air_load_ptr(c, v->ptr, t, align);
   if (v->kind != AIR_VAL_PTR_SELECT) {
      air_error(c, "instruction: a load through something that is not a pointer");
      return NULL;
   }
   nir_def *a = load_through_choice(c, v->elems[0], t, align);
   nir_def *b = load_through_choice(c, v->elems[1], t, align);
   if (!a || !b)
      return NULL;
   air_note(c, "a pointer chosen at run time (`select` between two buffers): both sides "
               "are LOADED and the values selected, because NIR has no select of "
               "pointers -- so a side the pipeline left unbound is read anyway, and its "
               "value discarded");
   return nir_bcsel(&c->b, v->def, a, b);
}

#include "air_gep.inc"

static bool
translate_gep(struct air_ctx *c, const struct air_inst *in)
{
   struct air_val *base = air_get(c, in->operands[0]);
   if (!base)
      return false;
   struct air_val *res = NULL;
   if (!gep_into(c, base, in, &res))
      return false;
   air_set(c, &in->base, res);
   return true;
}

static bool
translate_binop(struct air_ctx *c, const struct air_inst *in)
{
   nir_def *x = air_get_ssa(c, in->operands[0]);
   nir_def *y = air_get_ssa(c, in->operands[1]);
   if (!x || !y)
      return false;
   bool is_float = air_type_is_float(air_type_scalar(in->base.type));
   nir_def *res;

   /* Metal compiles with fast math on, and the modules say so per
    * instruction. Without the flags the arithmetic must not be reassociated,
    * so the builder is told to keep it exact.
    */
   uint32_t was_math = c->b.fp_math_ctrl;
   if (is_float)
      air_builder_set_exact(&c->b, !(in->flags & (AIR_FMF_UNSAFE_ALGEBRA | AIR_FMF_REASSOC)));

   switch (in->sub) {
   case AIR_BINOP_ADD: res = is_float ? nir_fadd(&c->b, x, y) : nir_iadd(&c->b, x, y); break;
   case AIR_BINOP_SUB: res = is_float ? nir_fsub(&c->b, x, y) : nir_isub(&c->b, x, y); break;
   case AIR_BINOP_MUL: res = is_float ? nir_fmul(&c->b, x, y) : nir_imul(&c->b, x, y); break;
   case AIR_BINOP_UDIV: res = is_float ? nir_fdiv(&c->b, x, y) : nir_udiv(&c->b, x, y); break;
   case AIR_BINOP_SDIV: res = is_float ? nir_fdiv(&c->b, x, y) : nir_idiv(&c->b, x, y); break;
   case AIR_BINOP_UREM: res = is_float ? nir_frem(&c->b, x, y) : nir_umod(&c->b, x, y); break;
   case AIR_BINOP_SREM: res = is_float ? nir_frem(&c->b, x, y) : nir_irem(&c->b, x, y); break;
   case AIR_BINOP_SHL: res = nir_ishl(&c->b, x, nir_u2u32(&c->b, y)); break;
   case AIR_BINOP_LSHR: res = nir_ushr(&c->b, x, nir_u2u32(&c->b, y)); break;
   case AIR_BINOP_ASHR: res = nir_ishr(&c->b, x, nir_u2u32(&c->b, y)); break;
   case AIR_BINOP_AND: res = nir_iand(&c->b, x, y); break;
   case AIR_BINOP_OR: res = nir_ior(&c->b, x, y); break;
   case AIR_BINOP_XOR: res = nir_ixor(&c->b, x, y); break;
   default:
      c->b.fp_math_ctrl = was_math;
      return air_error(c, "instruction: binary operator %u", in->sub);
   }
   c->b.fp_math_ctrl = was_math;
   air_set(c, &in->base, air_val_ssa(c, res));
   return true;
}

static bool
translate_cast(struct air_ctx *c, const struct air_inst *in)
{
   const struct air_type *dt = in->base.type;
   const struct air_type *st = in->operands[0]->type;

   if (in->sub == AIR_CAST_PTRTOINT) {
      struct air_val *v = air_get(c, in->operands[0]);
      if (!v || !val_is_pointerish(v))
         return air_error(c, "instruction: ptrtoint of something that is not a pointer");
      air_set(c, &in->base, v);
      return true;
   }
   if ((in->sub == AIR_CAST_BITCAST || in->sub == AIR_CAST_INTTOPTR) &&
       dt->kind == AIR_TYPE_POINTER) {
      struct air_val *v = air_get(c, in->operands[0]);
      if (!v || !val_is_pointerish(v))
         return air_error(c, "instruction: a pointer cast of something that is not a pointer");
      if (v->kind == AIR_VAL_PTR) {
         struct air_ptr p = v->ptr;
         p.pointee = dt->elem;
         air_set(c, &in->base, air_val_ptr(c, p));
      } else {
         air_set(c, &in->base, v);
      }
      return true;
   }

   nir_def *x = air_get_ssa(c, in->operands[0]);
   if (!x)
      return false;
   unsigned bits = air_nir_bit_size(c, dt);
   nir_def *res;
   switch (in->sub) {
   case AIR_CAST_TRUNC:
      res = bits == 1 ? nir_i2b(&c->b, nir_iand_imm(&c->b, x, 1)) : nir_u2uN(&c->b, x, bits);
      break;
   case AIR_CAST_ZEXT:
      res = x->bit_size == 1 ? nir_b2iN(&c->b, x, bits) : nir_u2uN(&c->b, x, bits);
      break;
   case AIR_CAST_SEXT:
      res = x->bit_size == 1 ? nir_ineg(&c->b, nir_b2iN(&c->b, x, bits))
                             : nir_i2iN(&c->b, x, bits);
      break;
   case AIR_CAST_FPTOUI: res = nir_f2uN(&c->b, x, bits); break;
   case AIR_CAST_FPTOSI: res = nir_f2iN(&c->b, x, bits); break;
   case AIR_CAST_UITOFP:
      res = x->bit_size == 1 ? nir_b2fN(&c->b, x, bits) : nir_u2fN(&c->b, x, bits);
      break;
   case AIR_CAST_SITOFP:
      res = x->bit_size == 1 ? nir_fneg(&c->b, nir_b2fN(&c->b, x, bits))
                             : nir_i2fN(&c->b, x, bits);
      break;
   case AIR_CAST_FPTRUNC: res = nir_f2fN(&c->b, x, bits); break;
   case AIR_CAST_FPEXT: res = nir_f2fN(&c->b, x, bits); break;
   /* A bitcast moves no bits: it renames them. When the two types have the
    * same shape -- the same component count and the same component width --
    * the rename is a NIR `mov`, because a NIR def carries a width and a
    * component count and no notion of float or integer, so `i32 -> float` and
    * `<4 x i32> -> <4 x float>` are already the same def.
    *
    * When the shape differs the bits have to be redistributed between
    * components, and NIR has an operation for exactly one redistribution of
    * exactly one width: pack_32_2x16 and unpack_32_2x16. That is the whole of
    * what this corpus asks for, and the number is the reason this is two cases
    * and not a general repacker -- `tools/air/typecensus.py` over all 7 879 of
    * Resident Evil 4's modules, reading the types out of llvm-dis rather than
    * out of our own reader, finds 111 657 bitcasts of 14 distinct type pairs,
    * and the ONLY two that change shape are:
    *
    *     i32        -> <2 x half>    44 136 of them, in 2 927 modules
    *     <2 x half> -> i32            4 333 of them, in   688 modules
    *
    * with 0 shape-changing bitcasts in the whole 456-module system corpus.
    * There is no third shape to be found behind these, so the pair below is
    * not a sample of a family: it is the family. Anything else keeps a refusal
    * -- and one that names the two shapes, so that a third, if some other
    * library has one, arrives as an arithmetic fact instead of a mystery.
    *
    * The half/half-pair of an `i32` is how an HLSL `min16float2` reaches here
    * through SPIRV-Cross: the pair travels through the shader packed in one
    * 32-bit register and is unpacked where it is used.
    *
    * Keyed on the SHAPE and not on the AIR type names, deliberately. NIR's
    * SSA has no float at 32 bits that is distinct from an integer at 32 bits,
    * so `float -> <2 x half>` -- which this corpus does not contain -- is the
    * same bit movement as `i32 -> <2 x half>` and is right for the same
    * reason. What a case keyed on the names would buy is a refusal for a
    * module whose bits are being moved correctly.
    *
    * Which half goes where is little-endian, which is not a choice: LLVM lays
    * element 0 of a vector at the lowest address, so element 0 of the
    * `<2 x half>` is bits 0..15 of the `i32`, and `unpack_32_2x16` is defined
    * `dst.x = src0.x; dst.y = src0.x >> 16` -- the same halves in the same
    * order. Getting that backwards would not crash and would not look wrong:
    * both halves are still finite halves, so the shader would draw with every
    * pair of 16-bit values transposed. tools/air/loweringcheck.py gives the
    * two halves values that cannot be confused for each other (13 and 113, 1
    * and -2) and checks them twice -- against LLVM running the same module on
    * this processor, and against the four numbers written out in the case
    * list, which come from Python's own binary16 codec and so are not LLVM's
    * endianness restated. It also says what the transposed answer would have
    * been. The drawn reference is NOT available for these: softpipe's
    * nir_to_tgsi has no 16-bit arithmetic at all, with or without a bitcast.
    */
   case AIR_CAST_BITCAST: {
      unsigned dn = air_nir_components(dt), sn = air_nir_components(st);
      unsigned db = bits, sb = air_nir_bit_size(c, st);
      if (dn == sn && db == sb)
         res = nir_mov(&c->b, x);
      else if (sn * sb == dn * db && (db == 8 || db == 16 || db == 32 || db == 64))
         res = nir_bitcast_vector(&c->b, x, db);
      else
         return air_error(c, "instruction: a bitcast from %u x %u bits to %u x %u bits, "
                             "which is a repacking this frontend does not do",
                          sn, sb, dn, db);
      break;
   }
   default:
      return air_error(c, "instruction: cast %u", in->sub);
   }
   air_set(c, &in->base, air_val_ssa(c, res));
   return true;
}

static bool
translate_cmp(struct air_ctx *c, const struct air_inst *in)
{
   nir_def *x = air_get_ssa(c, in->operands[0]);
   nir_def *y = air_get_ssa(c, in->operands[1]);
   if (!x || !y)
      return false;
   nir_def *res;
   switch (in->sub) {
   /* NaN never appears in these shaders (nnan is set on every one), so the
    * ordered and unordered forms of a comparison are the same comparison;
    * what differs is only the answer when an operand is NaN.
    */
   case AIR_FCMP_OEQ: case AIR_FCMP_UEQ: res = nir_feq(&c->b, x, y); break;
   case AIR_FCMP_ONE: case AIR_FCMP_UNE: res = nir_fneu(&c->b, x, y); break;
   case AIR_FCMP_OGT: case AIR_FCMP_UGT: res = nir_flt(&c->b, y, x); break;
   case AIR_FCMP_OGE: case AIR_FCMP_UGE: res = nir_fge(&c->b, x, y); break;
   case AIR_FCMP_OLT: case AIR_FCMP_ULT: res = nir_flt(&c->b, x, y); break;
   case AIR_FCMP_OLE: case AIR_FCMP_ULE: res = nir_fge(&c->b, y, x); break;
   case AIR_FCMP_ORD: res = nir_iand(&c->b, nir_feq(&c->b, x, x), nir_feq(&c->b, y, y)); break;
   case AIR_FCMP_UNO: res = nir_ior(&c->b, nir_fneu(&c->b, x, x), nir_fneu(&c->b, y, y)); break;
   case AIR_FCMP_FALSE: res = nir_imm_false(&c->b); break;
   case AIR_FCMP_TRUE: res = nir_imm_true(&c->b); break;
   case AIR_ICMP_EQ: res = nir_ieq(&c->b, x, y); break;
   case AIR_ICMP_NE: res = nir_ine(&c->b, x, y); break;
   case AIR_ICMP_UGT: res = nir_ult(&c->b, y, x); break;
   case AIR_ICMP_UGE: res = nir_uge(&c->b, x, y); break;
   case AIR_ICMP_ULT: res = nir_ult(&c->b, x, y); break;
   case AIR_ICMP_ULE: res = nir_uge(&c->b, y, x); break;
   case AIR_ICMP_SGT: res = nir_ilt(&c->b, y, x); break;
   case AIR_ICMP_SGE: res = nir_ige(&c->b, x, y); break;
   case AIR_ICMP_SLT: res = nir_ilt(&c->b, x, y); break;
   case AIR_ICMP_SLE: res = nir_ige(&c->b, y, x); break;
   default:
      return air_error(c, "instruction: comparison %u", in->sub);
   }
   air_set(c, &in->base, air_val_ssa(c, res));
   return true;
}

/* an aggregate value's field, by the index path of extract/insertvalue */
static struct air_val **
agg_slot(struct air_ctx *c, struct air_val *agg, const uint32_t *idx, unsigned n)
{
   /* extractvalue and insertvalue always carry at least one index. Without this
    * the loop below never runs and the function returns the address of its own
    * parameter -- which gcc points out (-Wreturn-local-addr) and which would be
    * a write through a dead stack slot if anything ever took the path.
    */
   if (!n) {
      air_error(c, "instruction: an aggregate access with no index");
      return NULL;
   }
   struct air_val **slot = &agg;
   for (unsigned i = 0; i < n; i++) {
      if (!*slot || (*slot)->kind != AIR_VAL_AGG || idx[i] >= (*slot)->num_elems) {
         air_error(c, "instruction: an aggregate index out of its value");
         return NULL;
      }
      slot = &(*slot)->elems[idx[i]];
   }
   return slot;
}

static struct air_val *
clone_val(struct air_ctx *c, const struct air_val *v)
{
   if (!v)
      return NULL;
   struct air_val *n = rzalloc(c->mem_ctx, struct air_val);
   *n = *v;
   if (v->kind == AIR_VAL_AGG) {
      n->elems = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(v->num_elems, 1));
      for (unsigned i = 0; i < v->num_elems; i++)
         n->elems[i] = clone_val(c, v->elems[i]);
   }
   return n;
}

/* What a value turned out to be, for a message that has to be read without the
 * module in hand. A refusal that only says "not constant" sends the reader to
 * llvm-dis to find out which of six things it was, and the three kernels that
 * stopped on a shuffle mask cost exactly that detour.
 */
static const char *
air_value_kind_name(const struct air_value *v)
{
   if (!v)
      return "nothing";
   const struct air_constant *c = air_value_as_constant(v);
   if (!c) {
      switch (v->kind) {
      case AIR_VALUE_GLOBAL:      return "a global";
      case AIR_VALUE_FUNCTION:    return "a function";
      case AIR_VALUE_ARGUMENT:    return "a function argument";
      case AIR_VALUE_INSTRUCTION: return "an instruction";
      case AIR_VALUE_METADATA:    return "metadata";
      default:                    return "a value of an unknown kind";
      }
   }
   switch (c->ckind) {
   case AIR_CONST_INT:       return "an integer";
   case AIR_CONST_FLOAT:     return "a float";
   case AIR_CONST_NULL:      return "zeroinitializer";
   case AIR_CONST_UNDEF:     return "undef";
   case AIR_CONST_POISON:    return "poison";
   case AIR_CONST_AGGREGATE: return "an aggregate of constants";
   case AIR_CONST_DATA:      return "a vector of plain integers";
   case AIR_CONST_EXPR:      return "a constant expression";
   }
   return "a constant of an unknown kind";
}

#include "air_instruction.inc"

