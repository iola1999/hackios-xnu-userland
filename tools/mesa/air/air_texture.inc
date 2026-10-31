/* Private texture lowering, included by air_intrinsics.c. */
/* -- textures ------------------------------------------------------------
 *
 * The largest family and the one with the long tail. Its vocabulary is
 * Apple's, read off AGXCompilerCore the same way the arithmetic was
 * (docs/air.md section 3.2): the names there end in an underscore --
 * air.sample_texture_, air.gather_compare_depth_, air.get_num_mip_levels_texture_
 * -- because the dimensionality is appended to them. That is why the earlier
 * survey counted "about 117 get_*_texture_* variants": it is some twenty
 * operations crossed with nine dimensionalities, not a hundred separate
 * things.
 *
 * The signatures are measured from the corpus, not guessed:
 *
 *   sample_<kind>_<dim> (tex, samp, [plane,] coord, [i32 array,]
 *                        [i1 has_offset, offset,]
 *                        i1 lod_is_level, float lod_or_bias, float min_lod,
 *                        i32 flags)                  -> { <4 x T>, i8 }
 *   sample_<kind>_<dim>_grad
 *                       (tex, samp, [plane,] coord, [i32 array,]
 *                        dPdx, dPdy, float min_lod,
 *                        [i1 has_offset, offset,] i32 flags)
 *   gather_<kind>_<dim> (tex, samp, coord, [i32 array,]
 *                        [i1 has_offset, offset,]
 *                        i32 component, i32 flags)   -> { <4 x T>, i8 }
 *   read_<kind>_<dim>   (tex, [plane,] coord, [i32 array,]
 *                        i32 lod_or_sample, i32 flags)
 *   read_texture_2d    (tex, builtin_read_sampler, coord, zero_offset, lod, access)
 *                      AIR 2.8 form for read-only and sampled textures
 *   write_<kind>_<dim>  (tex, coord, [i32 array,] value, i32 lod, i32 flags)
 *   get_<what>_<kind>_<dim> (tex[, i32 lod])         -> i32
 *
 * Three things in those brackets, and none of them is a guess. They are read
 * off Apple's own metal_stdlib on this device --
 * GPUCompiler.framework/.../include/metal/metal_texture, where every
 * `texture2d_array<T>::sample` and its fifty neighbours are one-line
 * forwarders to the builtin, so the header states the argument ORDER for all
 * nine dimensionalities and six operations -- and then confirmed against the
 * LLVM declaration in the modules that call them. The two agree everywhere:
 *
 *   AN ARRAY INDEX IS ITS OWN ARGUMENT, straight after the coordinate, in
 *   every one of sample, gather, read and write. It is not inside the
 *   coordinate vector, which is what this file used to require of it:
 *   air.sample_texture_2d_array takes a <2 x float> and an i32, and the
 *   refusal it used to earn said "with a 2-component coordinate, wanting 3".
 *
 *   THE GRADIENT FORM REORDERS WHAT FOLLOWS. dPdx, dPdy and the minimum level
 *   come before the offset pair, where the plain form has the offset pair
 *   first and the level last. A reader that assumed one order for both would
 *   take a gradient for an offset flag.
 *
 *   A CUBE HAS NO OFFSET PAIR AT ALL -- Metal defines no offset for cube
 *   sampling -- so air.sample_texture_cube goes from the coordinate straight
 *   to the level flag. Reading the 2d shape there is what made 621 modules
 *   stop at "argument 5 is not a constant": the float minimum level was being
 *   read as the i1 that says whether the next float is a level.
 *
 * The i1 before the level is read as "the float is a level, not a bias", so
 * (false, 0.0) is a plain sample -- which is what every one of the system
 * corpus's sample calls passes. Resident Evil 4 exercises both: 23 823
 * level-form calls against 5 961 bias-form.
 *
 * The second field of the returned struct is Metal's sparse-residency byte.
 * No module in the corpus reads it (every extractvalue of a texture result
 * takes field 0), so it is produced as zero rather than invented.
 */

struct air_tex_name {
   const char *op;        /* "sample", "gather", "read", "write", "get", "lod" */
   const char *query;     /* for "get": width, height, depth, array_size, ... */
   bool compare;          /* sample_compare / gather_compare */
   bool depth;            /* a depth texture rather than a colour one */
   enum glsl_sampler_dim dim;
   bool is_array, is_ms, is_buffer;
   bool grad;             /* the _grad form: explicit dPdx and dPdy */
   bool no_offset;        /* this dimensionality takes no offset argument */
};

static bool
read_sampler_op(struct air_ctx *c, const struct air_inst *in)
{
   if (in->num_operands != 1 || in->base.type->kind != AIR_TYPE_POINTER ||
       in->base.type->addrspace != 2)
      return air_error(c, "intrinsic: air.get_read_sampler requires no arguments and an address-space-2 pointer result");
   air_set(c, &in->base, air_val_ptr(c, (struct air_ptr){ .class = AIR_PTR_READ_SAMPLER }));
   return true;
}

static bool
eat(const char **s, const char *prefix)
{
   size_t n = strlen(prefix);
   if (strncmp(*s, prefix, n))
      return false;
   *s += n;
   return true;
}

/* air.<op>_<kind>_<dim> -> the pieces, or false when it is not a texture name */
static bool
parse_tex_name(const char *name, struct air_tex_name *t)
{
   memset(t, 0, sizeof(*t));
   const char *s = name;

   if (eat(&s, "sample_compare_")) {
      t->op = "sample";
      t->compare = true;
   } else if (eat(&s, "sample_")) {
      t->op = "sample";
   } else if (eat(&s, "gather_compare_")) {
      t->op = "gather";
      t->compare = true;
   } else if (eat(&s, "gather_")) {
      t->op = "gather";
   } else if (eat(&s, "read_")) {
      t->op = "read";
   } else if (eat(&s, "write_")) {
      t->op = "write";
   } else if (eat(&s, "calculate_clamped_lod_") || eat(&s, "calculate_unclamped_lod_")) {
      t->op = "lod";
   } else if (eat(&s, "get_")) {
      t->op = "get";
      static const char *queries[] = {
         "num_mip_levels_", "num_samples_", "array_size_", "width_", "height_", "depth_",
      };
      static const char *names[] = {
         "num_mip_levels", "num_samples", "array_size", "width", "height", "depth",
      };
      for (unsigned i = 0; i < ARRAY_SIZE(queries); i++) {
         if (eat(&s, queries[i])) {
            t->query = names[i];
            break;
         }
      }
      if (!t->query)
         return false;
   } else {
      return false;
   }

   if (eat(&s, "texture_buffer_1d")) {
      t->dim = GLSL_SAMPLER_DIM_BUF;
      t->is_buffer = true;
      return *s == 0;
   }
   if (eat(&s, "depth_"))
      t->depth = true;
   else if (!eat(&s, "texture_"))
      return false;

   /* The gradient form appends _grad to the DIMENSIONALITY and not to the
    * operation: Apple's builtin is __metal_sample_texture_2d_array_t_grad, so
    * the air.* name is air.sample_texture_2d_array_grad. Taking it off here
    * rather than giving the table nine more rows is what keeps one name for
    * one dimensionality -- and the exact comparison below is why 858 modules
    * used to stop at "a texture name this cannot take apart".
    */
   size_t len = strlen(s);
   if (len > 5 && !strcmp(s + len - 5, "_grad")) {
      t->grad = true;
      len -= 5;
   }

   /* the dimensionality, and whether it has an offset argument at all */
   static const struct {
      const char *suffix;
      enum glsl_sampler_dim dim;
      bool array, ms, no_offset;
   } dims[] = {
      { "cube_array", GLSL_SAMPLER_DIM_CUBE, true, false, true },
      { "2d_ms_array", GLSL_SAMPLER_DIM_MS, true, true, false },
      { "2d_array", GLSL_SAMPLER_DIM_2D, true, false, false },
      { "1d_array", GLSL_SAMPLER_DIM_1D, true, false, false },
      { "2d_ms", GLSL_SAMPLER_DIM_MS, false, true, false },
      { "cube", GLSL_SAMPLER_DIM_CUBE, false, false, true },
      { "1d", GLSL_SAMPLER_DIM_1D, false, false, false },
      { "2d", GLSL_SAMPLER_DIM_2D, false, false, false },
      { "3d", GLSL_SAMPLER_DIM_3D, false, false, false },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(dims); i++) {
      if (strlen(dims[i].suffix) == len && !strncmp(s, dims[i].suffix, len)) {
         t->dim = dims[i].dim;
         t->is_array = dims[i].array;
         t->is_ms = dims[i].ms;
         t->no_offset = dims[i].no_offset;
         return true;
      }
   }
   return false;
}

static unsigned
coord_components_of(const struct air_tex_name *t)
{
   unsigned n = t->dim == GLSL_SAMPLER_DIM_1D ? 1 :
                t->dim == GLSL_SAMPLER_DIM_3D ? 3 :
                t->dim == GLSL_SAMPLER_DIM_CUBE ? 3 :
                t->dim == GLSL_SAMPLER_DIM_BUF ? 1 : 2;
   return n + (t->is_array ? 1 : 0);
}

/* the result type of a texture operation, from the LLVM type it returns */
static nir_alu_type
tex_dest_type(struct air_ctx *c, const struct air_type *ret, const struct air_name *n)
{
   const struct air_type *t = ret;
   if (t->kind == AIR_TYPE_STRUCT && t->num_members)
      t = t->members[0];
   const struct air_type *scalar = air_type_scalar(t);
   unsigned bits = air_type_bit_size(scalar);
   if (air_type_is_float(scalar))
      return nir_type_float | bits;
   /* the signedness is in the name, not in the type: .s or .u */
   for (unsigned i = 1; i < n->num; i++) {
      if (!strcmp(n->part[i], "s"))
         return nir_type_int | bits;
      if (!strcmp(n->part[i], "u"))
         return nir_type_uint | bits;
   }
   return nir_type_uint | bits;
}

/* The pointer a texture or sampler argument carries. */
static bool
tex_handle(struct air_ctx *c, const struct air_inst *in, unsigned operand,
           enum air_ptr_class want, struct air_ptr *out)
{
   struct air_val *v = air_get(c, in->operands[operand]);
   if (!v)
      return false;
   if (v->kind != AIR_VAL_PTR || v->ptr.class != want)
      return air_error(c, "intrinsic: argument %u is not a %s", operand - 1,
                       want == AIR_PTR_TEXTURE ? "texture" :
                       want == AIR_PTR_READ_SAMPLER ? "builtin read sampler" : "sampler");
   *out = v->ptr;
   return true;
}

/* a constant i1/i32 argument, which these intrinsics use as flags */
static bool
tex_flag(struct air_ctx *c, const struct air_inst *in, unsigned operand, uint64_t *out)
{
   if (!air_const_uint(in->operands[operand], out))
      return air_error(c, "intrinsic: argument %u is not a constant", operand - 1);
   return true;
}

/* the { value, residency } struct these return */
static void
set_tex_result(struct air_ctx *c, const struct air_inst *in, nir_def *value)
{
   const struct air_type *ret = in->base.type;
   if (ret->kind != AIR_TYPE_STRUCT) {
      air_set(c, &in->base, air_val_ssa(c, value));
      return;
    }
   struct air_val *agg = rzalloc(c->mem_ctx, struct air_val);
   agg->kind = AIR_VAL_AGG;
   agg->num_elems = ret->num_members;
   agg->elems = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(ret->num_members, 1));
   agg->elems[0] = air_val_ssa(c, value);
   for (unsigned i = 1; i < ret->num_members; i++) {
      /* the sparse-residency byte: nothing in the corpus reads it */
      unsigned bits = air_nir_bit_size(c, ret->members[i]);
      agg->elems[i] = air_val_ssa(c, nir_imm_intN_t(&c->b, 0, bits));
   }
   air_set(c, &in->base, agg);
}


/* A query: a size, a level count or a sample count, of a texture or an image. */
static nir_def *
query_texture(struct air_ctx *c, const struct air_ptr *tex, const struct air_tex_name *t,
              nir_texop op, nir_def *lod, unsigned comps, nir_def *image_slot)
{
   if (tex->is_image) {
      /* a level slot (image_level_slot) is that level alone, measured at its own level 0 */
      nir_def *index = image_slot ? image_slot : nir_imm_int(&c->b, tex->binding);
      if (image_slot)
         lod = NULL;
      switch (op) {
      case nir_texop_txs: {
         /* an image size has as many components as the image has dimensions;
          * the caller passes 0 for "as many as there are"
          */
         unsigned n = comps ? comps :
            (t->dim == GLSL_SAMPLER_DIM_1D || t->dim == GLSL_SAMPLER_DIM_BUF ? 1 :
             t->dim == GLSL_SAMPLER_DIM_3D ? 3 : 2) + (t->is_array ? 1 : 0);
         return nir_image_size(&c->b, n, 32, index, lod ? lod : nir_imm_int(&c->b, 0),
                               .image_dim = t->dim, .image_array = t->is_array);
      }
      case nir_texop_query_levels:
         return nir_image_levels(&c->b, 32, index, .image_dim = t->dim,
                                 .image_array = t->is_array);
      default:
         return nir_image_samples(&c->b, 32, index, .image_dim = t->dim,
                                  .image_array = t->is_array);
      }
   }

   nir_tex_instr *q = nir_tex_instr_create(c->nir, (lod ? 1 : 0) + (tex->offset ? 1 : 0));
   q->op = op;
   q->sampler_dim = t->dim;
   q->is_array = t->is_array;
   q->dest_type = nir_type_int32;
   q->texture_index = tex->binding;
   q->sampler_index = 0;
   unsigned qsrc = 0;
   if (lod)
      q->src[qsrc++] = (nir_tex_src){ .src_type = nir_tex_src_lod, .src = nir_src_for_ssa(lod) };
   if (tex->offset)   /* a texture chosen at run time (choosable_texture, air_to_nir.c) */
      q->src[qsrc++] = (nir_tex_src){ .src_type = nir_tex_src_texture_offset,
                                      .src = nir_src_for_ssa(tex->offset) };
   /* NIR fixes the size of a query's result itself -- txs is two components
    * for a 2D texture whatever the caller asks one of them for -- so it is
    * asked here and the component picked afterwards.
    */
   if (comps == 0)
      comps = nir_tex_instr_dest_size(q);
   nir_def_init(&q->instr, &q->def, comps, 32);
   nir_builder_instr_insert(&c->b, &q->instr);
   return &q->def;
}

/* is every component of this value a constant zero? */
static bool
all_zero(nir_def *def)
{
   if (!nir_src_is_const(nir_src_for_ssa(def)))
      return false;
   for (unsigned i = 0; i < def->num_components; i++)
      if (nir_src_comp_as_int(nir_src_for_ssa(def), i) != 0)
         return false;
   return true;
}

/* The image slot of one mip level of a written texture.
 *
 * A Gallium image is one level, and a Metal kernel may write or measure any
 * level of a texture it was given, chosen at run time:
 * MercuryPosterExtension's VariableBlurrer::downsample writes level n of a
 * blur chain with n read from its constant buffer, and while it was refused
 * the extension set up no renderer at all and the wallpaper stayed black.
 * Such an image takes AIR_IMAGE_LEVELS more slots above every image the shader
 * declares, level L at level_base + L, and the runtime binds a view of each
 * level there (air_metal.c). ir3 reaches images through bindless descriptors,
 * so the slot may be a value computed in the shader.
 */
#define AIR_IMAGE_LEVELS 15   /* a 16384 x 16384 texture has 15 levels */
#define AIR_IMAGE_SLOTS  32   /* ir3's IR3_BINDLESS_IMAGE_COUNT */

static nir_def *
image_level_slot(struct air_ctx *c, const struct air_ptr *tex, nir_def *lod,
                 const struct air_name *n)
{
   struct air_resource *own = NULL;
   unsigned next = 0;

   for (unsigned i = 0; i < c->info->num_resources; i++) {
      struct air_resource *r = &c->info->resources[i];
      if (r->kind != AIR_RESOURCE_IMAGE)
         continue;
      next = MAX2(next, MAX2(r->index + 1, r->level_base + r->level_count));
      if (r->index == tex->binding)
         own = r;
   }
   if (!own) {
      air_error(c, "intrinsic: air.%s at a computed level of image %u, which the "
                   "interface does not declare", n->part[0], tex->binding);
      return NULL;
   }
   if (own->level_count == 0) {
      if (next + AIR_IMAGE_LEVELS > AIR_IMAGE_SLOTS) {
         air_error(c, "intrinsic: air.%s at a computed level of image %u, with no room "
                      "for its %u level slots above slot %u", n->part[0], tex->binding,
                   AIR_IMAGE_LEVELS, next);
         return NULL;
      }
      own->level_base = next;
      own->level_count = AIR_IMAGE_LEVELS;
   }
   return nir_iadd_imm(&c->b, nir_u2u32(&c->b, lod), own->level_base);
}

/* The coordinate, with the array index that follows it folded into it.
 *
 * NIR wants one coordinate with the layer as its last component; AIR passes
 * the two separately (see the signatures above). Which way the layer is
 * converted follows the coordinate's own type, which is also the only place
 * the answer can come from: a sample's coordinate is floating point and NIR's
 * array layer is then a float like the rest of it, while a read's is an
 * integer and the layer stays one.
 *
 * The float conversion is exact and not a rounding to be worried about. Metal
 * hands over an integer layer; u2f of an integer below 2^24 is exact in a
 * float, and 2^24 layers is 16 million slices of a texture array whose limit
 * on this hardware is 2 048. What a backend does with a *fractional* layer --
 * Vulkan says round to even, GLSL says nearest -- therefore never arises
 * here, because the value handed to it is always a whole number.
 *
 * An integer coordinate is widened to 32 bits first so that the layer, which
 * arrives as i32, does not have to be narrowed to meet it: air.read_texture_2d
 * has a 16-bit coordinate variant and the layer in those signatures is 32-bit
 * all the same.
 */
static nir_def *
tex_coord_and_layer(struct air_ctx *c, const struct air_inst *in,
                    const struct air_tex_name *t, const struct air_name *n,
                    unsigned *arg, unsigned ncoord)
{
   if (in->num_operands <= *arg) {
      air_error(c, "intrinsic: air.%s without a coordinate", n->part[0]);
      return NULL;
   }
   const struct air_value *cv = in->operands[*arg];
   nir_def *coord = air_get_ssa(c, cv);
   if (!coord)
      return NULL;
   (*arg)++;
   bool is_float = air_type_is_float(air_type_scalar(cv->type));
   if (!is_float)
      coord = nir_u2u32(&c->b, coord);

   if (t->is_array) {
      if (in->num_operands <= *arg) {
         air_error(c, "intrinsic: air.%s without the array index its signature has",
                   n->part[0]);
         return NULL;
      }
      nir_def *layer = air_get_ssa(c, in->operands[(*arg)++]);
      if (!layer)
         return NULL;
      if (layer->num_components != 1) {
         air_error(c, "intrinsic: air.%s with a %u-component array index",
                   n->part[0], layer->num_components);
         return NULL;
      }
      layer = is_float ? nir_u2fN(&c->b, layer, coord->bit_size)
                       : nir_u2u32(&c->b, layer);

      nir_def *comps[4];
      unsigned nc = MIN2(coord->num_components, 3);
      for (unsigned i = 0; i < nc; i++)
         comps[i] = nir_channel(&c->b, coord, i);
      comps[nc] = layer;
      coord = nir_vec(&c->b, comps, nc + 1);
   }

   if (coord->num_components != ncoord) {
      air_error(c, "intrinsic: air.%s with a %u-component coordinate, wanting %u",
                n->part[0], coord->num_components, ncoord);
      return NULL;
   }
   return coord;
}

/* Metal's null texture. air.get_null_texture_<dim> is a texture with nothing behind
 * it -- space::rasterizeCylinderFragment passes one to the light it evaluates without
 * a gobo -- and reading it gives zeros. air.is_null_texture_<dim> is true for it, and
 * on a texture argument it is a test made at run time: the argument is marked
 * nullable, the runtime binds no view when the application bound no texture, and an
 * empty slot measures 0 wide (fd6 gives it an all-zero descriptor).
 */
/* Metal lets a shader be given no texture where it does not read one: a texture tested with
 * is_null_texture, and the textures a run-time choice does not pick (texture_slot in
 * air_to_nir.c; space::rasterizeBackgroundFragment picks one of four gobo textures and the
 * wallpaper binds only that one). The runtime gives an unbound one no view, so it reads as
 * zero and measures 0 wide. */
void
air_texture_may_be_unbound(struct air_ctx *c, unsigned binding)
{
   for (unsigned i = 0; i < c->info->num_resources; i++) {
      struct air_resource *r = &c->info->resources[i];
      if ((r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_IMAGE) &&
          r->index == binding && r->argbuf_set == ~0u)
         r->nullable = true;
   }
}

static bool
null_texture_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n,
                bool *handled)
{
   const char *op = n->part[0];

   *handled = !strncmp(op, "get_null_texture_", 17) || !strncmp(op, "is_null_texture_", 16);
   if (!*handled)
      return true;
   if (op[0] == 'g') {
      air_set(c, &in->base, air_val_ptr(c, (struct air_ptr){ .class = AIR_PTR_TEXTURE,
                                                            .binding = ~0u,
                                                            .is_null = true }));
      return true;
   }

   struct air_ptr tex;
   if (in->num_operands != 2)
      return air_error(c, "intrinsic: air.%s takes one texture", op);
   if (!tex_handle(c, in, 1, AIR_PTR_TEXTURE, &tex))
      return false;
   if (tex.is_null) {
      air_set(c, &in->base, air_val_ssa(c, nir_imm_true(&c->b)));
      return true;
   }
   char size_name[64];
   struct air_tex_name t;
   snprintf(size_name, sizeof(size_name), "get_width_%s", op + strlen("is_null_"));
   if (!parse_tex_name(size_name, &t))
      return air_error(c, "intrinsic: air.%s (a texture name this cannot take apart)", op);
   if (!tex.offset)
      air_texture_may_be_unbound(c, tex.binding);
   nir_def *size = query_texture(c, &tex, &t, nir_texop_txs, nir_imm_int(&c->b, 0), 0, NULL);
   air_set(c, &in->base, air_val_ssa(c, nir_ieq_imm(&c->b, nir_channel(&c->b, size, 0), 0)));
   return true;
}

static bool
texture_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n)
{
   bool null_handled;
   if (!null_texture_op(c, in, n, &null_handled))
      return false;
   if (null_handled)
      return true;

   struct air_tex_name t;
   if (!parse_tex_name(n->part[0], &t))
      return air_error(c, "intrinsic: air.%s (a texture name this cannot take apart)",
                       n->part[0]);
   if (t.is_buffer)
      return air_error(c, "intrinsic: air.%s (texture buffers, not yet)", n->part[0]);
   /* A comparison belongs to a depth texture and to a sample: Apple's header declares
    * sample_compare and gather_compare on depth2d and depth2d_array and on nothing
    * else, so a name claiming either of the other two shapes is a misparse and not a
    * gap. Said by name rather than translated into a plausible instruction.
    */
   if (t.compare && !t.depth)
      return air_error(c, "intrinsic: air.%s (a comparison of a colour texture, which "
                          "Metal declares on depth textures only)", n->part[0]);
   if (t.compare && !strcmp(t.op, "gather"))
      return air_error(c, "intrinsic: air.%s (gather_compare: four comparisons in one "
                          "instruction, which nothing measured here attests -- no module "
                          "of the 456 and none of Resident Evil 4's 7 879 calls one -- so "
                          "there is nothing to check an answer against)", n->part[0]);
   /* Only a sample has a gradient form -- Apple's header has
    * __metal_sample_*_t_grad and no gather, read or write counterpart -- and a
    * name that claimed one would be read by the sample branch's rules below,
    * which is the kind of silent misparse this file refuses instead.
    */
   if (t.grad && strcmp(t.op, "sample"))
      return air_error(c, "intrinsic: air.%s (a gradient form of %s, which Metal has no "
                          "signature for)", n->part[0], t.op);

   struct air_ptr tex;
   if (!tex_handle(c, in, 1, AIR_PTR_TEXTURE, &tex))
      return false;

   unsigned arg = 2;   /* operands[0] is the callee, [1] the texture */
   struct air_ptr samp = { 0 };
   bool read_sampler = !strcmp(t.op, "read") && in->num_operands > arg &&
                       in->operands[arg]->type->kind == AIR_TYPE_POINTER;
   if (read_sampler) {
      /* AIR 2.8 ordinary read adds a builtin sampler and zero offset. Explicit
       * sampler reads also use this signature and need separate addressing semantics. */
      if (t.dim != GLSL_SAMPLER_DIM_2D || t.depth || t.is_array || t.is_ms || tex.is_image ||
          in->num_operands != 7)
         return air_error(c, "intrinsic: builtin read sampler requires a read-only 2D (texture, sampler, coord, offset, lod, access) signature");
      if (!tex_handle(c, in, arg++, AIR_PTR_READ_SAMPLER, &samp))
         return false;
      const struct air_type *coord = in->operands[arg]->type;
      const struct air_type *offset = in->operands[arg + 1]->type;
      const struct air_type *lod = in->operands[arg + 2]->type;
      uint64_t access;
      if (coord->kind != AIR_TYPE_VECTOR || coord->count != 2 ||
          coord->elem->kind != AIR_TYPE_INT || (coord->elem->bits != 16 && coord->elem->bits != 32) ||
          offset->kind != AIR_TYPE_VECTOR || offset->count != 2 ||
          offset->elem->kind != AIR_TYPE_INT || offset->elem->bits != coord->elem->bits ||
          lod->kind != AIR_TYPE_INT || (lod->bits != 16 && lod->bits != 32) ||
          in->operands[arg + 3]->type->kind != AIR_TYPE_INT ||
          in->operands[arg + 3]->type->bits != 32 ||
          !air_const_uint(in->operands[arg + 3], &access) || (access != 0 && access != 1))
         return air_error(c, "intrinsic: builtin read sampler has invalid coordinate, offset, lod or access types");
   }
   bool have_sampler = !strcmp(t.op, "sample") || !strcmp(t.op, "gather") ||
                       !strcmp(t.op, "lod");
   if (have_sampler) {
      if (!tex_handle(c, in, arg, AIR_PTR_SAMPLER, &samp))
         return false;
      arg++;
   }

   /* A depth texture takes a plane or aspect selector before the coordinate,
    * and every call in the corpus passes 1. What else it can be is not known
    * from the data, so anything else is refused rather than assumed.
    */
   if (t.depth) {
      uint64_t plane;
      if (!tex_flag(c, in, arg, &plane))
         return false;
      if (plane != 1)
         return air_error(c, "intrinsic: air.%s with aspect %u (only 1 is understood)",
                          n->part[0], (unsigned)plane);
      arg++;
   }

   unsigned ncoord = coord_components_of(&t);
   nir_alu_type dest_type = tex_dest_type(c, in->base.type, n);
   unsigned dest_bits = nir_alu_type_get_type_size(dest_type);

   /* a null texture (null_texture_op): nothing is written, and what is read is zero */
   if (tex.is_null) {
      if (!strcmp(t.op, "write"))
         return true;
      if (!strcmp(t.op, "get")) {
         air_set(c, &in->base, air_val_ssa(c, nir_imm_intN_t(&c->b, 0, dest_bits)));
         return true;
      }
      if (strcmp(t.op, "sample") && strcmp(t.op, "read") && strcmp(t.op, "gather"))
         return air_error(c, "intrinsic: air.%s of a null texture", n->part[0]);
      set_tex_result(c, in, nir_imm_zero(&c->b, 4, dest_bits));
      return true;
   }

   if (!strcmp(t.op, "get")) {
      /* the queries: a size, a level count or a sample count */
      nir_def *res;
      if (!strcmp(t.query, "num_samples")) {
         res = query_texture(c, &tex, &t, nir_texop_texture_samples, NULL, 1, NULL);
      } else if (!strcmp(t.query, "num_mip_levels")) {
         res = query_texture(c, &tex, &t, nir_texop_query_levels, NULL, 1, NULL);
      } else {
         nir_def *lod = nir_imm_int(&c->b, 0);
         if (in->num_operands > arg) {
            nir_def *given = air_get_ssa(c, in->operands[arg]);
            if (!given)
               return false;
            lod = nir_u2u32(&c->b, given);
         }
         nir_def *slot = NULL;
         if (tex.is_image && !all_zero(lod) && !(slot = image_level_slot(c, &tex, lod, n)))
            return false;
         nir_def *size = query_texture(c, &tex, &t, nir_texop_txs, lod, 0, slot);
         unsigned comp = !strcmp(t.query, "width") ? 0 :
                         !strcmp(t.query, "height") ? 1 :
                         !strcmp(t.query, "depth") ? 2 : size->num_components - 1;
         if (!strcmp(t.query, "array_size") && !t.is_array)
            return air_error(c, "intrinsic: air.%s on a texture that is not an array",
                             n->part[0]);
         if (comp >= size->num_components)
            return air_error(c, "intrinsic: air.%s asks for component %u of %u",
                             n->part[0], comp, size->num_components);
         res = nir_channel(&c->b, size, comp);
      }
      air_set(c, &in->base, air_val_ssa(c, nir_u2uN(&c->b, res, dest_bits)));
      return true;
   }

   if (!strcmp(t.op, "write")) {
      nir_def *coord = tex_coord_and_layer(c, in, &t, n, &arg, ncoord);
      if (!coord)
         return false;
      nir_def *value = air_get_ssa(c, in->operands[arg++]);
      if (!value)
         return false;
      if (!tex.is_image)
         return air_error(c, "intrinsic: air.%s to a texture the interface says is not "
                             "written", n->part[0]);
      if (in->num_operands <= arg)
         return air_error(c, "intrinsic: air.%s without its level", n->part[0]);
      uint64_t level;
      nir_def *index;
      if (air_const_uint(in->operands[arg], &level) && level == 0) {
         index = nir_imm_int(&c->b, tex.binding);
      } else {
         nir_def *lod = air_get_ssa(c, in->operands[arg]);
         if (!lod || !(index = image_level_slot(c, &tex, lod, n)))
            return false;
      }
      nir_image_store(&c->b, index,
                      nir_pad_vector(&c->b, nir_u2u32(&c->b, coord), 4),
                      nir_imm_int(&c->b, 0), nir_pad_vector(&c->b, value, 4),
                      nir_imm_int(&c->b, 0),
                      .image_dim = t.dim, .image_array = t.is_array,
                      .src_type = dest_type);
      return true;
   }

   /* sample, gather, read: a texture instruction, or an image load */
   nir_def *coord = tex_coord_and_layer(c, in, &t, n, &arg, ncoord);
   if (!coord)
      return false;

   if (read_sampler) {
      nir_def *offset = air_get_ssa(c, in->operands[arg++]);
      if (!offset || !all_zero(offset))
         return air_error(c, "intrinsic: builtin read sampler requires a constant zero offset");
   }

   if (!strcmp(t.op, "read") && tex.is_image) {
      uint64_t extra;
      if (!tex_flag(c, in, arg, &extra))
         return false;
      nir_def *res = nir_image_load(&c->b, 4, dest_bits, nir_imm_int(&c->b, tex.binding),
                                    nir_pad_vector(&c->b, nir_u2u32(&c->b, coord), 4),
                                    nir_imm_int(&c->b, t.is_ms ? extra : 0),
                                    nir_imm_int(&c->b, t.is_ms ? 0 : extra),
                                    .image_dim = t.dim, .image_array = t.is_array,
                                    .dest_type = dest_type);
      set_tex_result(c, in, res);
      return true;
   }

   /* The value a depth texel is compared against, which Metal passes straight after
    * the coordinate AND AFTER THE ARRAY INDEX -- __metal_sample_compare_depth_2d_t
    * takes (tex, samp, coord, compare_value, ...) and the _2d_array form takes (tex,
    * samp, coord, array, compare_value, ...), so the layer comes first in exactly the
    * place tex_coord_and_layer has already consumed it. Read here and not in the
    * branches below because it is before the offset pair in the plain form and before
    * the gradients in the _grad one: one position, not two.
    *
    * At the AIR level there is also the aspect selector the depth branch above ate, as
    * there is in air.sample_depth_2d -- Apple's header has no such argument and the
    * declaration in texturedFragment2DDepth does:
    *   { float, i8 } air.sample_depth_2d.f32(ptr as(1), ptr as(2), i32, <2 x float>,
    *                                         i1, <2 x i32>, i1, float, float, i32)
    * which is where the `i32` between the sampler and the coordinate comes from, and
    * the only reason the compare form's shape could be read off the header at all.
    */
   nir_def *compare = NULL;
   if (t.compare) {
      if (in->num_operands <= arg)
         return air_error(c, "intrinsic: air.%s without the value its signature compares "
                             "against", n->part[0]);
      if (!(compare = air_get_ssa(c, in->operands[arg++])))
         return false;
      if (compare->num_components != 1)
         return air_error(c, "intrinsic: air.%s compares against %u components, and a "
                             "depth comparison takes one", n->part[0],
                          compare->num_components);
      /* The texel and the value compared with it go into one register tuple on the
       * Adreno -- ir3 puts the comparison straight after the coordinates in src0 --
       * so a 16-bit comparison value beside 32-bit coordinates is a half register in a
       * full tuple. Metal's own signature is `float`, so this never fires on anything
       * Apple's compiler emits; it is here so that if something does, it is widened
       * and not mixed.
       */
      if (compare->bit_size != coord->bit_size)
         compare = nir_f2fN(&c->b, compare, coord->bit_size);
   }

   /* Seven and not six: a compare sample in the gradient form carries a coordinate, a
    * comparison value, an offset, two derivatives and a minimum level at once, which is
    * exactly six, and the next source anyone adds should not be the one that overflows
    * this silently.
    */
   nir_tex_src srcs[8];
   unsigned nsrc = 0;
   srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_coord,
                                 .src = nir_src_for_ssa(coord) };
   if (compare)
      srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_comparator,
                                    .src = nir_src_for_ssa(compare) };

   nir_texop op;
   if (!strcmp(t.op, "sample")) {
      /* The offset pair, for the dimensionalities that have one. A cube does
       * not, and reading one there consumed the level flag and the level and
       * left the minimum level to be read as the flags word -- which is the
       * refusal 621 of Resident Evil 4's modules used to earn, "argument 5 is
       * not a constant", from a float that indeed is not an integer constant.
       *
       * In the gradient form this pair comes AFTER the gradients, so the
       * reading is a small state machine over the signature rather than one
       * fixed order: see the table at the head of this section.
       */
      uint64_t has_offset = 0, is_level = 0;
      nir_def *offset = NULL, *ddx = NULL, *ddy = NULL, *min_lod = NULL, *level = NULL;

      if (t.grad) {
         ddx = air_get_ssa(c, in->operands[arg++]);
         ddy = air_get_ssa(c, in->operands[arg++]);
         min_lod = air_get_ssa(c, in->operands[arg++]);
         if (!ddx || !ddy || !min_lod)
            return false;
         if (!t.no_offset) {
            if (!tex_flag(c, in, arg, &has_offset))
               return false;
            arg++;
            if (!(offset = air_get_ssa(c, in->operands[arg++])))
               return false;
         }
      } else {
         if (!t.no_offset) {
            if (!tex_flag(c, in, arg, &has_offset))
               return false;
            arg++;
            if (!(offset = air_get_ssa(c, in->operands[arg++])))
               return false;
         }
         if (!tex_flag(c, in, arg, &is_level))
            return false;
         arg++;
         level = air_get_ssa(c, in->operands[arg++]);
         min_lod = air_get_ssa(c, in->operands[arg++]);
         if (!level || !min_lod)
            return false;
      }

      if (offset && has_offset && !all_zero(offset))
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_offset,
                                       .src = nir_src_for_ssa(nir_u2u32(&c->b, offset)) };

      if (t.grad) {
         /* An explicit pair of derivatives of the coordinate, which is what
          * Metal's sample(..., gradient2d(dPdx, dPdy)) is for: a shader that
          * knows better than the quad's own differences -- a ray-marched or
          * reprojected coordinate, or one sampled inside a branch the quad
          * does not take together. NIR has the operation (txd) and so does
          * the Adreno; ir3 emits it and softpipe's interpreter has TXD, so
          * unlike most of this family it can be both compiled and run here.
          *
          * The gradients carry the coordinate's components WITHOUT the array
          * layer, which is what NIR asks for too (nir_tex_instr_src_size:
          * coord_components minus is_array for ddx and ddy) and what AIR
          * passes -- <2 x float> for a 2d_array whose coordinate is also
          * <2 x float>. So nothing is added or dropped here, and that
          * agreement is the reason this needs no reshaping.
          */
         op = nir_texop_txd;
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_ddx,
                                       .src = nir_src_for_ssa(ddx) };
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_ddy,
                                       .src = nir_src_for_ssa(ddy) };
      } else {
         bool level_is_zero = nir_src_is_const(nir_src_for_ssa(level)) &&
                              nir_src_as_float(nir_src_for_ssa(level)) == 0.0;
         if (is_level) {
            op = nir_texop_txl;
            srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_lod,
                                          .src = nir_src_for_ssa(level) };
         } else if (!level_is_zero) {
            op = nir_texop_txb;
            srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_bias,
                                          .src = nir_src_for_ssa(level) };
         } else {
            op = nir_texop_tex;
         }
      }

      if (!(nir_src_is_const(nir_src_for_ssa(min_lod)) &&
            nir_src_as_float(nir_src_for_ssa(min_lod)) == 0.0))
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_min_lod,
                                       .src = nir_src_for_ssa(min_lod) };
   } else if (!strcmp(t.op, "gather")) {
      uint64_t has_offset = 0;
      nir_def *offset = NULL;
      if (!t.no_offset) {
         if (!tex_flag(c, in, arg, &has_offset))
            return false;
         arg++;
         if (!(offset = air_get_ssa(c, in->operands[arg++])))
            return false;
      }
      if (offset && has_offset && !all_zero(offset))
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_offset,
                                       .src = nir_src_for_ssa(nir_u2u32(&c->b, offset)) };
      op = nir_texop_tg4;
   } else {
      /* read: a fetch by integer coordinate, with a level or a sample. Neither
       * has to be a constant -- read_texture_2d_ms is called with a computed
       * sample index throughout AppleMetalGLRenderer.
       */
      nir_def *which = air_get_ssa(c, in->operands[arg++]);
      if (!which)
         return false;
      which = nir_u2u32(&c->b, which);
      op = t.is_ms ? nir_texop_txf_ms : nir_texop_txf;
      srcs[nsrc++] = (nir_tex_src){
         .src_type = t.is_ms ? nir_tex_src_ms_index : nir_tex_src_lod,
         .src = nir_src_for_ssa(which),
      };
   }

   if (tex.offset)   /* a texture chosen at run time (choosable_texture, air_to_nir.c) */
      srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_texture_offset,
                                    .src = nir_src_for_ssa(tex.offset) };
   nir_tex_instr *tex_instr = nir_tex_instr_create(c->nir, nsrc);
   tex_instr->op = op;
   tex_instr->sampler_dim = t.dim;
   tex_instr->is_array = t.is_array;
   /* is_new_style_shadow because Metal's sample_compare returns ONE value, which the
    * declaration says too -- air.sample_compare_depth_2d.f32 returns { float, i8 }, not
    * { <4 x float>, i8 } the way a colour sample does. NIR's old-style shadow
    * replicates the comparison into four components and would leave the shader reading
    * component 0 of four where the signature has one, which is the same answer by
    * accident; this says what the operation is.
    */
   tex_instr->is_shadow = t.compare;
   tex_instr->is_new_style_shadow = t.compare;
   tex_instr->dest_type = dest_type;
   tex_instr->coord_components = ncoord;
   tex_instr->texture_index = tex.binding;
   tex_instr->sampler_index = have_sampler ? samp.binding : 0;
   if (op == nir_texop_tg4) {
      uint64_t component;
      if (!tex_flag(c, in, arg, &component))
         return false;
      tex_instr->component = component;
   }
   for (unsigned i = 0; i < nsrc; i++)
      tex_instr->src[i] = srcs[i];

   const struct air_type *value_type = in->base.type;
   if (value_type->kind == AIR_TYPE_STRUCT && value_type->num_members)
      value_type = value_type->members[0];

   /* The width of the result is NIR's to decide: a sample is four components
    * whatever the shader does with them. Metal's depth2d.sample() returns one
    * float, which is channel 0 of those four -- the same thing GLSL gets from
    * a depth texture read without a comparison.
    */
   unsigned nir_comps = nir_tex_instr_dest_size(tex_instr);
   nir_def_init(&tex_instr->instr, &tex_instr->def, nir_comps, dest_bits);
   nir_builder_instr_insert(&c->b, &tex_instr->instr);

   nir_def *res = &tex_instr->def;
   unsigned want = air_nir_components(value_type);
   if (want < nir_comps)
      res = nir_trim_vector(&c->b, res, want);
   else if (want > nir_comps)
      return air_error(c, "intrinsic: air.%s gives %u components, the shader wants %u",
                       n->part[0], nir_comps, want);
   set_tex_result(c, in, res);
   return true;
}

