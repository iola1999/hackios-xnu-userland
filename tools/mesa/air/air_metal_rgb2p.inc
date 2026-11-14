/* RGB2P render resources keep per-fragment codes until the pass stores its planes. */

static unsigned
rgb2p_clear_code(unsigned format, float value)
{
   value = CLAMP(value, 0.0f, 1.0f);
   uint16_t half = format == 540 ? _mesa_float_to_half(value)
                                 : _mesa_float_to_float16_rtz_slow(value);
   return (unsigned)floorf(_mesa_half_to_float(half) * (format == 540 ? 255.0f : 1023.0f) + 0.5f);
}

static nir_def *
rgb2p_quantize(nir_builder *b, nir_def *value, unsigned format)
{
   if (value->bit_size == 16)
      value = nir_f2f32(b, value);
   value = nir_fsat(b, value);
   nir_def *half = format == 540 ? nir_f2f16_rtne(b, value) : nir_f2f16_rtz(b, value);
   nir_def *scaled = nir_fmul_imm(b, nir_f2f32(b, half), format == 540 ? 255.0f : 1023.0f);
   return nir_f2u32(b, nir_ffloor(b, nir_fadd_imm(b, scaled, 0.5f)));
}

static bool
lower_rgb2p_outputs(nir_shader *nir, const struct rwa_render_shape *shape,
                     char *why, size_t why_size)
{
   unsigned mask = 0;
   for (unsigned i = 0; i < shape->color_count; i++)
      if (rgb2p_format(shape->color_format[i])) mask |= 1u << i;
   if (!mask) return true;
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *store = nir_instr_as_intrinsic(instr);
            if (store->intrinsic != nir_intrinsic_store_deref) continue;
            nir_deref_instr *deref = nir_src_as_deref(store->src[0]);
            nir_variable *var = nir_deref_instr_get_variable(deref);
            if (!var || var->data.mode != nir_var_shader_out ||
                var->data.location < FRAG_RESULT_DATA0) continue;
            unsigned index = var->data.location - FRAG_RESULT_DATA0;
            if (index >= shape->color_count || !(mask & (1u << index))) continue;
            if (!glsl_type_is_vector_or_scalar(var->type) ||
                (glsl_get_base_type(var->type) != GLSL_TYPE_FLOAT &&
                 glsl_get_base_type(var->type) != GLSL_TYPE_FLOAT16)) {
               say(why, why_size, "RGB2P attachment %u requires a floating-point fragment output", index);
               return false;
            }
            b.cursor = nir_before_instr(instr);
            nir_def *codes = rgb2p_quantize(&b, store->src[1].ssa, shape->color_format[index]);
            if (codes->num_components == 4)
               codes = nir_vec4(&b, nir_channel(&b, codes, 0), nir_channel(&b, codes, 1),
                               nir_channel(&b, codes, 2), nir_imm_int(&b, 1));
            nir_src_rewrite(&store->src[1], codes);
         }
      }
   }
   nir_foreach_shader_out_variable(var, nir) {
      if (var->data.location < FRAG_RESULT_DATA0) continue;
      unsigned index = var->data.location - FRAG_RESULT_DATA0;
      if (index < shape->color_count && (mask & (1u << index)))
         var->type = glsl_vector_type(GLSL_TYPE_UINT, glsl_get_vector_elements(var->type));
   }
   nir_fixup_deref_types(nir);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   nir_validate_shader(nir, "RGB2P output codes");
   return true;
}

struct rwa_texture *
rwa_texture_combine_rgb2p(struct rwa_texture *y, struct rwa_texture *uv,
                          unsigned format, unsigned usage, char *why, size_t why_size)
{
   RWA_LOCKED;
   bool ten = format != 540;
   unsigned vertical = format == 544 ? 1 : 2;
   unsigned effective = usage ? usage : 5;
   bool render = (effective & 4) != 0;
   if (!rgb2p_format(format) || !y || !uv || y->device != uv->device ||
       y->shape.pixel_format != (ten ? 20 : 10) || uv->shape.pixel_format != (ten ? 60 : 30) ||
       y->shape.type != 2 || uv->shape.type != 2 || y->shape.sample_count != 1 ||
       uv->shape.sample_count != 1 || texture_levels(y) != 1 || texture_levels(uv) != 1 ||
       (y->shape.width & 1) || (y->shape.height & 1) ||
       uv->shape.width != y->shape.width/2 ||
       uv->shape.height != y->shape.height/vertical ||
       !y->shader_read || !uv->shader_read || (effective & ~0x15u) ||
       (render && (!y->shader_write || !uv->shader_write))) {
      say(why, why_size, "RGB2P requires matching R8/RG8 or R16/RG16 single-level 2D planes and sample/render usage");
      return NULL;
   }
   struct rwa_texture *t;
   if (render) {
      struct rwa_texture_shape shape = y->shape;
      shape.pixel_format = ten ? 113 : 73;
      shape.usage = 7;
      t = texture_of_shape(y->device, &shape, NULL, why, why_size);
      if (!t) return NULL;
   } else {
      t = calloc(1, sizeof(*t));
      if (!t) {
         say(why, why_size, "out of memory creating an RGB2P texture");
         return NULL;
      }
      t->references = 1;
      t->device = y->device;
      t->shape = y->shape;
      t->layers = 1;
      pipe_resource_reference(&t->res, y->res);
   }
   t->shape.pixel_format = format;
   t->shape.usage = usage;
   t->shader_read = (effective & 1) != 0;
   t->shader_write = false;
   t->format_name = format == 540 ? "MTLPixelFormatRGB8_420_2P"
                     : format == 543 ? "MTLPixelFormatRGB10_420_2P" : "MTLPixelFormatRGB10_422_2P";
   t->storage_owner = y;
   t->chroma = uv;
   y->references++;
   uv->references++;
   return t;
}

static nir_def *
rgb2p_image_load(nir_builder *b, unsigned slot, nir_def *coord, enum pipe_format format,
                  bool integer)
{
   return nir_image_load(b, 4, 32, nir_imm_int(b, slot), coord, nir_imm_int(b, 0),
                         nir_imm_int(b, 0), .image_dim = GLSL_SAMPLER_DIM_2D,
                         .format = format, .dest_type = integer ? nir_type_uint32 : nir_type_float32);
}

static void
rgb2p_image_store(nir_builder *b, unsigned slot, nir_def *coord, nir_def *value,
                   enum pipe_format format, bool integer)
{
   nir_image_store(b, nir_imm_int(b, slot), coord, nir_imm_int(b, 0), value, nir_imm_int(b, 0),
                   .image_dim = GLSL_SAMPLER_DIM_2D, .format = format,
                   .src_type = integer ? nir_type_uint32 : nir_type_float32);
}

static nir_shader *
rgb2p_conversion_shader(const struct nir_shader_compiler_options *options,
                         unsigned format, bool begin)
{
   bool ten = format != 540;
   unsigned vertical = format == 544 ? 1 : 2;
   enum pipe_format storage = ten ? PIPE_FORMAT_R16G16B16A16_UINT : PIPE_FORMAT_R8G8B8A8_UINT;
   enum pipe_format luma = ten ? PIPE_FORMAT_R16_UNORM : PIPE_FORMAT_R8_UNORM;
   enum pipe_format chroma = ten ? PIPE_FORMAT_R16G16_UNORM : PIPE_FORMAT_R8G8_UNORM;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
                                                  "RGB2P_%u_%s", format, begin ? "load" : "store");
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_images = 3;
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *id = nir_load_global_invocation_id(&b, 32);
   nir_def *x = nir_channel(&b, id, 0), *y = nir_channel(&b, id, 1);
   nir_def *size = nir_image_size(&b, 2, 32, zero, zero, .image_dim = GLSL_SAMPLER_DIM_2D,
                                 .format = storage);
   nir_def *width = nir_channel(&b, size, 0), *height = nir_channel(&b, size, 1);
   nir_push_if(&b, nir_iand(&b, nir_ult(&b, x, width), nir_ult(&b, y, height)));
   nir_def *coord = nir_vec4(&b, x, y, zero, zero);
   nir_def *uv_coord = nir_vec4(&b, nir_ushr_imm(&b, x, 1),
                               vertical == 2 ? nir_ushr_imm(&b, y, 1) : y, zero, zero);
   if (begin) {
      nir_def *yy = nir_channel(&b, rgb2p_image_load(&b, 1, coord, luma, false), 0);
      nir_def *uv = rgb2p_image_load(&b, 2, uv_coord, chroma, false);
      nir_def *v = nir_vec4(&b, nir_channel(&b, uv, 1), yy, nir_channel(&b, uv, 0),
                            nir_imm_float(&b, 0));
      v = nir_f2u32(&b, nir_fround_even(&b, nir_fmul_imm(&b, v, ten ? 65535.0f : 255.0f)));
      if (ten) v = nir_ushr_imm(&b, v, 6);
      v = nir_vec4(&b, nir_channel(&b, v, 0), nir_channel(&b, v, 1), nir_channel(&b, v, 2),
                    nir_imm_int(&b, 1));
      rgb2p_image_store(&b, 0, coord, v, storage, true);
   } else {
      nir_def *v = rgb2p_image_load(&b, 0, coord, storage, true);
      nir_def *yy = nir_channel(&b, v, 1);
      if (ten) yy = nir_ishl_imm(&b, yy, 6);
      nir_def *yf = nir_fmul_imm(&b, nir_u2f32(&b, yy), 1.0f/(ten ? 65535.0f : 255.0f));
      rgb2p_image_store(&b, 1, coord, nir_vec4(&b, yf, nir_imm_float(&b, 0),
                         nir_imm_float(&b, 0), nir_imm_float(&b, 1)), luma, false);
      nir_def *even = nir_ieq_imm(&b, nir_iand_imm(&b, x, 1), 0);
      if (vertical == 2)
         even = nir_iand(&b, even, nir_ieq_imm(&b, nir_iand_imm(&b, y, 1), 0));
      nir_push_if(&b, even);
      nir_def *sum = nir_imm_ivec2(&b, 0, 0), *count = zero;
      for (unsigned j = 0; j < vertical; j++) {
         for (unsigned i = 0; i < 2; i++) {
            nir_def *px = nir_iadd_imm(&b, x, i), *py = nir_iadd_imm(&b, y, j);
            nir_def *inside = nir_iand(&b, nir_ult(&b, px, width), nir_ult(&b, py, height));
            nir_def *at = nir_vec4(&b, nir_umin(&b, px, nir_iadd_imm(&b, width, -1)),
                                   nir_umin(&b, py, nir_iadd_imm(&b, height, -1)), zero, zero);
            nir_def *s = rgb2p_image_load(&b, 0, at, storage, true);
            nir_def *pair = nir_vec2(&b, nir_channel(&b, s, 2), nir_channel(&b, s, 0));
            sum = nir_iadd(&b, sum, nir_bcsel(&b, inside, pair, nir_imm_ivec2(&b, 0, 0)));
            count = nir_iadd(&b, count, nir_b2i32(&b, inside));
         }
      }
      sum = nir_udiv(&b, nir_iadd(&b, sum, nir_ushr_imm(&b, count, 1)), count);
      if (ten) sum = nir_ishl_imm(&b, sum, 6);
      nir_def *uv = nir_fmul_imm(&b, nir_u2f32(&b, sum), 1.0f/(ten ? 65535.0f : 255.0f));
      rgb2p_image_store(&b, 2, uv_coord,
                         nir_vec4(&b, nir_channel(&b, uv, 0), nir_channel(&b, uv, 1),
                                  nir_imm_float(&b, 0), nir_imm_float(&b, 1)), chroma, false);
      nir_pop_if(&b, NULL);
   }
   nir_pop_if(&b, NULL);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_validate_shader(b.shader, "RGB2P plane conversion");
   return b.shader;
}

/* Native RGB2P samples integer-rounded chroma at luma centers before the user filter. */
static nir_shader *
rgb2p_sample_shader(const struct nir_shader_compiler_options *options, unsigned format)
{
   bool ten = format != 540;
   unsigned vertical = format == 544 ? 1 : 2;
   enum pipe_format storage = ten ? PIPE_FORMAT_R10G10B10A2_UNORM : PIPE_FORMAT_R8G8B8A8_UNORM;
   enum pipe_format luma = ten ? PIPE_FORMAT_R16_UNORM : PIPE_FORMAT_R8_UNORM;
   enum pipe_format chroma = ten ? PIPE_FORMAT_R16G16_UNORM : PIPE_FORMAT_R8G8_UNORM;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
                                                  "RGB2P_%u_sample", format);
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_images = 3;
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *id = nir_load_global_invocation_id(&b, 32);
   nir_def *x = nir_channel(&b, id, 0), *y = nir_channel(&b, id, 1);
   nir_def *size = nir_image_size(&b, 2, 32, zero, zero, .image_dim = GLSL_SAMPLER_DIM_2D,
                                 .format = storage);
   nir_def *width = nir_channel(&b, size, 0), *height = nir_channel(&b, size, 1);
   nir_push_if(&b, nir_iand(&b, nir_ult(&b, x, width), nir_ult(&b, y, height)));
   nir_def *coord = nir_vec4(&b, x, y, zero, zero);
   nir_def *yy = nir_f2u32(&b, nir_fround_even(&b, nir_fmul_imm(&b,
      nir_channel(&b, rgb2p_image_load(&b, 1, coord, luma, false), 0), ten ? 65535.0f : 255.0f)));
   if (ten) yy = nir_ushr_imm(&b, yy, 6);
   nir_def *wx = nir_bcsel(&b, nir_ieq_imm(&b, nir_iand_imm(&b, x, 1), 0),
                          nir_imm_int(&b, 3), nir_imm_int(&b, 1));
   nir_def *wy = vertical == 2 ? nir_bcsel(&b,
      nir_ieq_imm(&b, nir_iand_imm(&b, y, 1), 0), nir_imm_int(&b, 3), nir_imm_int(&b, 1)) : zero;
   nir_def *ux = nir_isub(&b, nir_ushr_imm(&b, x, 1), nir_b2i32(&b,
      nir_ieq_imm(&b, nir_iand_imm(&b, x, 1), 0)));
   nir_def *uy = vertical == 2 ? nir_isub(&b, nir_ushr_imm(&b, y, 1), nir_b2i32(&b,
      nir_ieq_imm(&b, nir_iand_imm(&b, y, 1), 0))) : y;
   nir_def *sum = nir_imm_ivec2(&b, 0, 0);
   for (unsigned j = 0; j < vertical; j++) for (unsigned i = 0; i < 2; i++) {
      nir_def *cx = nir_imin(&b, nir_imax(&b, nir_iadd_imm(&b, ux, i), zero),
                            nir_iadd_imm(&b, nir_ushr_imm(&b, width, 1), -1));
      nir_def *cy = nir_imin(&b, nir_imax(&b, nir_iadd_imm(&b, uy, j), zero),
                            nir_iadd_imm(&b, vertical == 2 ? nir_ushr_imm(&b, height, 1) : height, -1));
      nir_def *at = nir_vec4(&b, cx, cy, zero, zero);
      nir_def *pair = nir_f2u32(&b, nir_fround_even(&b, nir_fmul_imm(&b,
         nir_channels(&b, rgb2p_image_load(&b, 2, at, chroma, false), 3), ten ? 65535.0f : 255.0f)));
      if (ten) pair = nir_ushr_imm(&b, pair, 6);
      nir_def *weight = i ? wx : nir_isub(&b, nir_imm_int(&b, 4), wx);
      if (vertical == 2)
         weight = nir_imul(&b, weight, j ? wy : nir_isub(&b, nir_imm_int(&b, 4), wy));
      sum = nir_iadd(&b, sum, nir_imul(&b, pair, weight));
   }
   sum = nir_ushr_imm(&b, nir_iadd_imm(&b, sum, vertical == 2 ? 8 : 2), vertical == 2 ? 4 : 2);
   nir_def *codes = nir_vec4(&b, nir_channel(&b, sum, 1), yy, nir_channel(&b, sum, 0),
                             nir_imm_int(&b, ten ? 1023 : 255));
   nir_def *color = nir_fmul_imm(&b, nir_u2f32(&b, codes), 1.0f/(ten ? 1023.0f : 255.0f));
   rgb2p_image_store(&b, 0, coord, color, storage, false);
   nir_pop_if(&b, NULL);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_validate_shader(b.shader, "RGB2P sample interpolation");
   return b.shader;
}

static void *
rgb2p_compute_state(struct rwa_device *device, unsigned format, unsigned kind,
                     char *why, size_t why_size)
{
   unsigned index = format == 540 ? 0 : format == 543 ? 1 : 2;
   void **saved = &device->rgb2p_csos[index][kind];
   if (*saved) return *saved;
   nir_shader *nir = kind == 2 ? rgb2p_sample_shader(device->nir_options, format)
                               : rgb2p_conversion_shader(device->nir_options, format, kind == 0);
   finalize_for_gallium(nir, device->screen);
   struct pipe_compute_state state = {.ir_type = PIPE_SHADER_IR_NIR, .prog = nir};
   *saved = device->pipe->create_compute_state(device->pipe, &state);
   if (!*saved) say(why, why_size, "the GPU would not compile RGB2P plane conversion");
   return *saved;
}

static bool
rgb2p_convert(struct rwa_texture *t, struct pipe_resource *output, unsigned kind,
                char *why, size_t why_size)
{
   struct rwa_device *device = t->device;
   void *cso = rgb2p_compute_state(device, t->shape.pixel_format, kind, why, why_size);
   if (!cso) return false;
   struct pipe_resource *parts[] = {output, t->storage_owner->res, t->chroma->res};
   enum pipe_format formats[] = {output->format, texture_format(t->storage_owner), texture_format(t->chroma)};
   struct pipe_image_view images[3];
   for (unsigned i = 0; i < 3; i++) {
      images[i] = (struct pipe_image_view){.resource = parts[i],
         .format = formats[i], .u.tex = {.level = 0, .first_layer = 0, .last_layer = 0},
         .access = (kind == 1 ? i != 0 : i == 0) ? PIPE_IMAGE_ACCESS_WRITE : PIPE_IMAGE_ACCESS_READ,
         .shader_access = (kind == 1 ? i != 0 : i == 0) ? PIPE_IMAGE_ACCESS_WRITE : PIPE_IMAGE_ACCESS_READ};
   }
   struct pipe_context *pipe = device->pipe;
   pipe->memory_barrier(pipe, PIPE_BARRIER_FRAMEBUFFER | PIPE_BARRIER_IMAGE | PIPE_BARRIER_TEXTURE);
   pipe->bind_compute_state(pipe, cso);
   pipe->set_shader_images(pipe, MESA_SHADER_COMPUTE, 0, 3, 0, images);
   struct pipe_grid_info grid = {.work_dim = 2, .block = {8,8,1},
      .grid = {DIV_ROUND_UP(t->shape.width,8), DIV_ROUND_UP(t->shape.height,8),1}};
   pipe->launch_grid(pipe, &grid);
   pipe->memory_barrier(pipe, PIPE_BARRIER_FRAMEBUFFER | PIPE_BARRIER_IMAGE | PIPE_BARRIER_TEXTURE);
   pipe->set_shader_images(pipe, MESA_SHADER_COMPUTE, 0, 0, 3, NULL);
   pipe->bind_compute_state(pipe, NULL);
   return true;
}

static bool
rgb2p_prepare_sample(struct rwa_texture *t, char *why, size_t why_size)
{
   if (!t->shader_read || t->rgb2p_pass_open) {
      say(why, why_size, "RGB2P sampling requires readable, stored planes");
      return false;
   }
   if (!t->rgb2p_sample) {
      struct rwa_texture_shape shape = t->shape;
      shape.pixel_format = t->shape.pixel_format == 540 ? 70 : 90;
      shape.usage = 3;
      struct rwa_texture *made = texture_of_shape(t->device, &shape, NULL, why, why_size);
      if (!made) return false;
      pipe_resource_reference(&t->rgb2p_sample, made->res);
      rwa_texture_destroy(made);
   }
   return rgb2p_convert(t, t->rgb2p_sample, 2, why, why_size);
}

int
rwa_texture_rgb2p_pass(struct rwa_texture *t, bool begin, char *why, size_t why_size)
{
   RWA_LOCKED;
   if (!t || !rgb2p_format(t->shape.pixel_format) || !t->chroma || !t->storage_owner ||
       !(t->res->bind & PIPE_BIND_RENDER_TARGET) ||
       !(t->res->bind & PIPE_BIND_SHADER_IMAGE) || t->rgb2p_pass_open == begin) {
      say(why, why_size, "RGB2P pass requires a renderable texture and matched begin/end calls");
      return -1;
   }
   if (!rgb2p_convert(t, t->res, begin ? 0 : 1, why, why_size)) return -1;
   t->rgb2p_pass_open = begin;
   if (!begin) {
      texture_written(t->storage_owner);
      texture_written(t->chroma);
   }
   return 0;
}

static void
rgb2p_device_close(struct rwa_device *device)
{
   for (unsigned i = 0; i < 3; i++)
      for (unsigned j = 0; j < 3; j++)
         if (device->rgb2p_csos[i][j])
            device->pipe->delete_compute_state(device->pipe, device->rgb2p_csos[i][j]);
}
