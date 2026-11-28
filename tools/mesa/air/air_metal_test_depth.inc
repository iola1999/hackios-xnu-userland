/* Depth/stencil tests, included into air_metal_test.c. */

/* Depth texels, for a shader that stores what it samples into [[depth]]: 64 distinct
 * values in (0, 1), each exactly representable as a float, so the one that comes back
 * names the texel it came from -- and none of them is 0 or 1, the two values a cleared
 * or untouched buffer could hold.
 */
static float
depth_texel_value(unsigned x, unsigned y, unsigned z)
{
   return ((float)(x + 4 * y + 16 * z) + 0.5f) / 64.0f;
}

/* The two aspects an MTLPixelFormat has, which decides what goes in rwa_render_shape:
 * Metal's pipeline descriptor has a property per aspect and one texture behind them, so
 * a combined format fills both and a depth-only format fills one.
 */
static bool
depth_format_aspects(unsigned f, bool *has_depth, bool *has_stencil)
{
   switch (f) {
   case 250: *has_depth = true;  *has_stencil = false; return true;  /* Depth16Unorm */
   case 252: *has_depth = true;  *has_stencil = false; return true;  /* Depth32Float */
   case 253: *has_depth = false; *has_stencil = true;  return true;  /* Stencil8 */
   case 260: *has_depth = true;  *has_stencil = true;  return true;  /* ..._Stencil8 */
   default:  return false;
   }
}

/*
 * Whether a depth that came back is the depth that was asked for, and the tolerance is
 * the format's and not a fudge: Depth32Float holds 0.25, 0.5 and 0.75 exactly, so there
 * the tolerance is zero and the comparison is ==. Depth16Unorm holds none of them --
 * 0.5 is 32767.5 of 65535 and has to round -- so one step of that format is allowed,
 * and the line that prints the comparison says which it was. A tolerance wide enough
 * for every format would be a test that cannot fail.
 */
static float
depth_tolerance(unsigned zformat)
{
   return zformat == MTL_DEPTH16_UNORM ? 1.0f / 65535.0f : 0.0f;
}

static bool
depth_is(float got, float want, float tolerance)
{
   float d = got - want;
   return (d < 0.0f ? -d : d) <= tolerance;
}

/* The whole depth buffer, and whether it is one value everywhere. The quad covers every
 * pixel, so it must be -- and a buffer that is not says the draw covered part of the
 * attachment, which no single pixel would have shown.
 */
static bool
read_depth(struct rwa_texture *zt, float *out, float *first, const char *what)
{
   char why[WHY] = "";
   const unsigned origin[3] = { 0, 0, 0 };
   const unsigned extent[3] = { TEX, TEX, 1 };

   if (rwa_depth_read(zt, 0, 0, origin, extent, out, NULL, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_depth_read (%s): %s\n", what, why);
      return false;
   }
   bool uniform = true;
   for (unsigned i = 1; i < TEX * TEX; i++)
      if (out[i] != out[0])
         uniform = false;
   *first = out[0];
   printf("air_metal_test: %s: depth at (0,0) = %.9g, and the 16 texels are %s\n", what,
          (double)out[0], uniform ? "all that value" : "NOT all the same");
   if (!uniform) {
      for (unsigned y = 0; y < TEX; y++) {
         printf("air_metal_test:   row %u:", y);
         for (unsigned x = 0; x < TEX; x++)
            printf(" %.9g", (double)out[y * TEX + x]);
         printf("\n");
      }
   }
   return uniform;
}

static int
run_depth(const char *path, const char *func, const char *colorspec, const char *depthspec)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      return 1;

   /* The colour attachments, as --draw takes them: the default is clearFragment20's
    * three RGBA32Uint, since it writes render target 2 and 0 and 1 have to exist for 2
    * to be bound at all.
    */
   unsigned format[8];
   bool fmt_is_float[COUNT_OF(format)];
   unsigned color_count = 0;
   /* "none" is a DEPTH-ONLY pass, which Metal allows and this library allows only with a
    * depth attachment (a shadow map is exactly this). It is spelled out rather than left
    * to an empty argument, because an empty argument is what a caller passes to reach the
    * one after it. */
   const char *arg = colorspec && !strcmp(colorspec, "none") ? NULL
                     : (colorspec && *colorspec) ? colorspec : "123,123,123";
   while (arg && *arg && color_count < COUNT_OF(format)) {
      unsigned v = (unsigned)strtoul(arg, NULL, 0);
      if (v != MTL_RGBA32_FLOAT && v != MTL_RGBA32_UINT && v != MTL_RGBA32_SINT) {
         fprintf(stderr, "air_metal_test: attachment pixel format %u: this program reads "
                         "back MTLPixelFormatRGBA32Float (%u), RGBA32Uint (%u) or "
                         "RGBA32Sint (%u)\n", v, MTL_RGBA32_FLOAT, MTL_RGBA32_UINT,
                 MTL_RGBA32_SINT);
         return 2;
      }
      format[color_count] = v;
      fmt_is_float[color_count] = v == MTL_RGBA32_FLOAT;
      color_count++;
      const char *comma = strchr(arg, ',');
      arg = comma ? comma + 1 : NULL;
   }

   unsigned zformat = depthspec && *depthspec ? (unsigned)strtoul(depthspec, NULL, 0)
                                              : MTL_DEPTH32_FLOAT;
   bool zs_depth = false, zs_stencil = false;
   if (!depth_format_aspects(zformat, &zs_depth, &zs_stencil)) {
      fprintf(stderr, "air_metal_test: depth pixel format %u: iOS has Depth16Unorm (250), "
                      "Depth32Float (252), Stencil8 (253) and Depth32Float_Stencil8 "
                      "(260)\n", zformat);
      return 2;
   }
   if (!zs_depth) {
      fprintf(stderr, "air_metal_test: pixel format %u has no depth aspect, and this mode "
                      "measures a depth test\n", zformat);
      return 2;
   }

   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: --draw-depth %s on \"%s\", depth attachment pixel format %u\n",
          name, rwa_device_name(device), zformat);

   struct rwa_render_shape shape = { .color_count = color_count, .sample_count = 1,
                                     .depth_format = zs_depth ? zformat : 0,
                                     .stencil_format = zs_stencil ? zformat : 0 };
   for (unsigned i = 0; i < color_count; i++)
      shape.color_format[i] = format[i];

   struct rwa_render_info info = { 0 };
   struct rwa_render *render =
      rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &shape, &info,
                        why, sizeof(why));
   if (!render) {
      printf("air_metal_test: rwa_render_create: %s\n", why);
      return 1;
   }
   printf("air_metal_test: pipeline: %u fragment buffers, %u fragment textures, %u "
          "samplers, vertex stage %s, and the fragment shader %s to [[depth]]\n",
          info.fragment_buffers, info.fragment_textures, info.fragment_samplers,
          info.generated_vertex_stage ? "generated" : "supplied",
          info.fragment_writes_depth ? "STORES" : "stores nothing");

   /* The attachments. The depth one is MTLTextureUsageRenderTarget and nothing else:
    * its texels come back through rwa_depth_read, which maps the resource rather than
    * sampling it, so it needs no shader-read binding to be measured.
    */
   const unsigned origin[3] = { 0, 0, 0 };
   struct rwa_texture *color[COUNT_OF(format)] = { 0 };
   for (unsigned i = 0; i < color_count; i++) {
      const struct rwa_texture_shape ts = {
         .pixel_format = format[i], .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1,
         .usage = MTL_USAGE_SHADER_READ | 4 /* MTLTextureUsageRenderTarget */,
      };
      color[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!color[i]) {
         printf("air_metal_test: rwa_texture_create (attachment %u): %s\n", i, why);
         return 1;
      }
   }
   const struct rwa_texture_shape zts = {
      .pixel_format = zformat, .type = MTL_TEXTURE_TYPE_2D,
      .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1, .usage = 4 /* MTLTextureUsageRenderTarget */,
   };
   struct rwa_texture *zt = rwa_texture_create(device, &zts, why, sizeof(why));
   if (!zt) {
      printf("air_metal_test: rwa_texture_create (the depth attachment): %s\n", why);
      return 1;
   }

   /* The depth texture is refused by rwa_texture_read by name, and that refusal is part
    * of the contract rather than an accident, so it is shown here: a caller that asks
    * for a depth texel the colour way is told which call to use instead.
    */
   {
      uint8_t ignored[TEX * TEX * 8];
      const unsigned extent[3] = { TEX, TEX, 1 };
      char refusal[WHY] = "";
      int rc = rwa_texture_read(zt, 0, 0, origin, extent, ignored, 0, refusal,
                                sizeof(refusal));
      printf("air_metal_test: rwa_texture_read of the depth attachment %s: %s\n",
             rc == 0 ? "RETURNED 0, which it must not" : "is refused", refusal);
      if (rc == 0)
         return 1;
   }

   /* The fragment stage's buffers, filled per step below, and its textures, made the
    * shape each slot declares -- which is rwa_module_textures, the same query the
    * driver's sweep uses, so a textured depth shader gets the texture3d it asks for
    * without an argument here saying so.
    */
   struct rwa_buffer *buffers[16] = { 0 };
   unsigned nbuf = info.fragment_buffers < COUNT_OF(buffers) ? info.fragment_buffers
                                                             : (unsigned)COUNT_OF(buffers);
   for (unsigned i = 0; i < nbuf; i++) {
      buffers[i] = rwa_buffer_create(device, 48, why, sizeof(why));
      if (!buffers[i]) {
         printf("air_metal_test: rwa_buffer_create (buffer %u): %s\n", i, why);
         return 1;
      }
   }

   struct rwa_texture_decl decl[16];
   unsigned ndecl = 0;
   if (info.fragment_textures &&
       rwa_module_textures(device, bitcode, bitcode_size, func, decl, COUNT_OF(decl),
                           &ndecl, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_module_textures: %s\n", why);
      return 1;
   }
   struct rwa_texture *textures[16] = { 0 };
   unsigned ntex = info.fragment_textures < COUNT_OF(textures) ? info.fragment_textures
                                                               : (unsigned)COUNT_OF(textures);
   for (unsigned i = 0; i < ntex; i++) {
      unsigned type = MTL_TEXTURE_TYPE_2D, fmt = MTL_RGBA32_FLOAT, bits = 32;
      for (unsigned k = 0; k < ndecl; k++) {
         if (decl[k].index != i)
            continue;
         if (decl[k].type_known)
            type = decl[k].type;
         fmt = decl[k].element == RWA_ELEMENT_UINT ? MTL_RGBA32_UINT
             : decl[k].element == RWA_ELEMENT_INT  ? MTL_RGBA32_SINT : MTL_RGBA32_FLOAT;
         if (decl[k].element_bits)
            bits = decl[k].element_bits;
      }
      if (type != MTL_TEXTURE_TYPE_2D && type != MTL_TEXTURE_TYPE_3D) {
         printf("air_metal_test: texture %u is declared %s, which this mode does not "
                "make\n", i, texture_type_name(type));
         return 1;
      }
      /* The WIDTH of the declared element and not only its class, refused rather than
       * widened. A half declaration bound a 32-bit texture is a descriptor that
       * disagrees with the instruction the GPU will run, not a conversion: air_run's
       * image_shapes() was picking a 32-bit format for a 16-bit typed store, and that
       * one took a whole boot with it. This mode writes 32-bit texels (the union
       * below), so a 16-bit declaration is said out loud -- the two shaders this mode
       * exists for declare depth2d<float>, which is 32.
       */
      if (bits != 32) {
         printf("air_metal_test: texture %u is declared with a %u-bit element and this "
                "mode writes 32-bit texels; binding a 32-bit format to a %u-bit "
                "declaration is a descriptor that disagrees with the shader, so it is "
                "refused instead\n", i, bits, bits);
         return 1;
      }
      unsigned depth = type == MTL_TEXTURE_TYPE_3D ? TEX : 1;
      const struct rwa_texture_shape ts = {
         .pixel_format = fmt, .type = type, .width = TEX, .height = TEX, .depth = depth,
         .array_length = 1, .mip_levels = 1, .sample_count = 1,
         .usage = MTL_USAGE_SHADER_READ,
      };
      textures[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!textures[i]) {
         printf("air_metal_test: rwa_texture_create (texture %u): %s\n", i, why);
         return 1;
      }
      static union { float f[TEXELS]; int32_t i[TEXELS]; } written;
      memset(&written, 0, sizeof(written));
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX; y++)
            for (unsigned x = 0; x < TEX; x++) {
               float v[4];
               if (info.fragment_writes_depth) {
                  /* a depth in [0, 1] in every channel, because the shader stores what
                   * it samples: air_run's 100.0 would clamp to the far plane and read
                   * as the clear value
                   */
                  for (unsigned c = 0; c < 4; c++)
                     v[c] = depth_texel_value(x, y, z);
               } else {
                  texel_value(i, x, y, z, v);
               }
               for (unsigned c = 0; c < 4; c++) {
                  unsigned k = texel_index(x, y, z) + c;
                  if (fmt == MTL_RGBA32_FLOAT)
                     written.f[k] = v[c];
                  else
                     written.i[k] = (int32_t)v[c];
               }
            }
      const unsigned extent[3] = { TEX, TEX, depth };
      if (rwa_texture_write(textures[i], 0, 0, origin, extent, &written, 0, why,
                            sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_write (texture %u): %s\n", i, why);
         return 1;
      }
      printf("air_metal_test: texture %u is %ux%ux%u of pixel format %u, texel (x,y,z) = "
             "%s\n", i, TEX, TEX, depth, fmt,
             info.fragment_writes_depth ? "(x + 4y + 16z + 0.5) / 64 in every channel"
                                        : "(100+x, 200+y, 300+x+y+z, 400)");
   }

   struct rwa_sampler *samplers[16] = { 0 };
   unsigned nsamp = info.fragment_samplers < COUNT_OF(samplers) ? info.fragment_samplers
                                                                : (unsigned)COUNT_OF(samplers);
   for (unsigned i = 0; i < nsamp; i++) {
      const struct rwa_sampler_state st = {
         .address = { 0, 0, 0 }, .min_filter = 0, .mag_filter = 0, .mip_filter = 0,
         .compare_function = 0, .max_anisotropy = 1, .normalized_coordinates = 1,
         .lod_min = 0.0f, .lod_max = 65504.0f,
      };
      samplers[i] = rwa_sampler_create(device, &st, why, sizeof(why));
      if (!samplers[i]) {
         printf("air_metal_test: rwa_sampler_create (sampler %u): %s\n", i, why);
         return 1;
      }
   }

   /* Two depth/stencil states and nothing else: the one under test and the control.
    * Both write depth, so that what the buffer holds is always the depth of the last
    * draw that passed.
    */
   const struct rwa_depth_stencil_state less_state = {
      .depth_compare_function = 1 /* MTLCompareFunctionLess */, .depth_write_enabled = 1,
      .front = { .compare_function = 7, .read_mask = 0xffffffff, .write_mask = 0xffffffff },
      .back  = { .compare_function = 7, .read_mask = 0xffffffff, .write_mask = 0xffffffff },
   };
   struct rwa_depth_stencil_state always_state = less_state;
   always_state.depth_compare_function = 7;   /* MTLCompareFunctionAlways */

   struct rwa_depth_stencil *less = rwa_depth_stencil_create(device, &less_state, why,
                                                             sizeof(why));
   if (!less) {
      printf("air_metal_test: rwa_depth_stencil_create (Less): %s\n", why);
      return 1;
   }
   struct rwa_depth_stencil *always = rwa_depth_stencil_create(device, &always_state, why,
                                                               sizeof(why));
   if (!always) {
      printf("air_metal_test: rwa_depth_stencil_create (Always): %s\n", why);
      return 1;
   }

   unsigned read_rt = color_count ? color_count - 1 : 0;
   float depth_map[TEX * TEX];
   float tol = depth_tolerance(zformat);
   bool ok = true;

   /* The four-draw experiment reads its answer out of a colour attachment, so a
    * depth-only pass can only be run with a shader whose output is the depth. Said
    * rather than run with a pixel nobody can read. */
   if (color_count == 0 && !info.fragment_writes_depth) {
      printf("air_metal_test: a depth-only pass needs a shader that stores to [[depth]]; "
             "%s writes colour, and the discard is measured in a colour attachment\n", name);
      return 1;
   }

   if (tol != 0.0f)
      printf("air_metal_test: depths are compared to within %.9g, one step of this "
             "format, because it stores none of 0.25, 0.5 and 0.75 exactly\n",
             (double)tol);

   if (info.fragment_writes_depth) {
      /* One draw, with the test off (Always) and depth writes on: what the buffer holds
       * afterwards is the shader's own [[depth]] and nothing else could have put it
       * there.
       *
       * The expected value is the texel the generated vertex stage's one constant
       * coordinate selects. That stage writes (c, d, c, 1) into every varying with
       * c = d = (RWA_TEX_X + 0.5) / 4 = 0.375 (air_metal.c, quad_vertex_shader), so a
       * 2D sample lands in texel (1, 1) and a 3D one in (1, 1, 1) -- the third
       * component repeats the first, and 0.375 of four slices is slice 1. air_run reads
       * the same texel of a 3D texture for the same reason (its tex_z is TEX_Y), so the
       * two harnesses are looking at the same texel and not at two nearly-right ones.
       */
      unsigned want_z = ntex && textures[0] && decl[0].type_known &&
                        decl[0].type == MTL_TEXTURE_TYPE_3D ? 1 : 0;
      float want = depth_texel_value(1, 1, want_z);
      const float clear_depth = 1.0f;
      const float range[2] = { 0.5f, 1.0f };
      const struct rwa_draw draw = {
         .color = color, .color_count = color_count,
         .clear_color = color_count ? (const float[]){ 0, 0, 0, 0 } : NULL,
         .depth = zt, .clear_depth = &clear_depth, .depth_stencil = always,
         .depth_range = range,
         .fragment = { .buffers = buffers, .buffer_count = nbuf,
                       .textures = textures, .texture_count = ntex,
                       .samplers = samplers, .sampler_count = nsamp },
         .primitive = 4, .vertex_start = 0, .vertex_count = 4,
      };
      if (rwa_render_draw(render, &draw, why, sizeof(why)) != 0) {
         printf("air_metal_test: rwa_render_draw: %s\n", why);
         return 1;
      }
      float got = 0.0f;
      ok = read_depth(zt, depth_map, &got, "after the draw") && ok;
      printf("air_metal_test: the shader sampled texel (1, 1, %u) and stored %.9g to "
             "[[depth]]; the buffer holds %.9g\n", want_z, (double)want, (double)got);
      printf("air_metal_test: the quad's own depth was %.9g and the clear was %.9g, so "
             "neither could be mistaken for it\n", (double)range[0], (double)clear_depth);
      if (!depth_is(got, want, tol)) {
         printf("air_metal_test: DIFFERENT -- the depth buffer does not hold what the "
                "shader wrote\n");
         ok = false;
      } else {
         printf("air_metal_test: THE SAME -- a Metal fragment shader's [[depth]] reached "
                "the depth buffer\n");
      }
   } else {
      /* Four draws of one quad: three with MTLCompareFunctionLess at 0.5, 0.75 and
       * 0.25, then the control with Always at 0.75. Each draw's constant buffer holds a
       * different fill, so a draw that was not discarded leaves a colour the previous
       * one did not.
       */
      static const struct { const char *what; float z; bool pattern; uint8_t fill;
                            bool clear; bool control; bool expect_draw; } step[] = {
         { "the near draw at depth 0.5",             0.5f,  true,  0x00, true,  false, true },
         { "the far draw at depth 0.75",             0.75f, false, 0xa5, false, false, false },
         { "the nearer draw at depth 0.25",          0.25f, false, 0x5a, false, false, true },
         { "the control at depth 0.75 with Always",  0.75f, false, 0x3c, false, true,  true },
      };
      unsigned pixel[COUNT_OF(step)][4];
      float depth_at[COUNT_OF(step)];

      memset(pixel, 0, sizeof(pixel));
      for (unsigned s = 0; s < COUNT_OF(step); s++) {
         for (unsigned i = 0; i < nbuf; i++) {
            uint8_t *bits = rwa_buffer_map(buffers[i]);
            for (unsigned k = 0; k < rwa_buffer_size(buffers[i]); k++)
               bits[k] = step[s].pattern ? pattern_byte(k) : step[s].fill;
         }
         const float clear_depth = 1.0f;
         const float range[2] = { step[s].z, 1.0f };
         const struct rwa_draw draw = {
            .color = color, .color_count = color_count,
            .clear_color = step[s].clear ? (const float[]){ 0, 0, 0, 0 } : NULL,
            .depth = zt,
            .clear_depth = step[s].clear ? &clear_depth : NULL,
            .depth_stencil = step[s].control ? always : less,
            .depth_range = range,
            .fragment = { .buffers = buffers, .buffer_count = nbuf,
                          .textures = textures, .texture_count = ntex,
                          .samplers = samplers, .sampler_count = nsamp },
            .primitive = 4, .vertex_start = 0, .vertex_count = 4,
         };
         if (rwa_render_draw(render, &draw, why, sizeof(why)) != 0) {
            printf("air_metal_test: rwa_render_draw (%s): %s\n", step[s].what, why);
            return 1;
         }
         const unsigned one[3] = { 1, 1, 1 };
         if (rwa_texture_read(color[read_rt], 0, 0, origin, one, pixel[s], 0, why,
                              sizeof(why)) != 0) {
            printf("air_metal_test: rwa_texture_read (attachment %u): %s\n", read_rt, why);
            return 1;
         }
         printf("air_metal_test: %s: the pixel at (0, 0) of attachment %u is", step[s].what,
                read_rt);
         for (unsigned c = 0; c < 4; c++) {
            if (fmt_is_float[read_rt])
               printf(" %.9g", (double)((const float *)pixel[s])[c]);
            else
               printf(" %u", pixel[s][c]);
         }
         printf("\n");
         ok = read_depth(zt, depth_map, &depth_at[s], step[s].what) && ok;
      }

      /* The verdicts, each naming what would have to be true for it to read otherwise. */
      bool far_discarded = memcmp(pixel[1], pixel[0], sizeof(pixel[0])) == 0;
      bool nearer_drawn = memcmp(pixel[2], pixel[0], sizeof(pixel[0])) != 0;
      bool control_drawn = memcmp(pixel[3], pixel[2], sizeof(pixel[0])) != 0;

      printf("air_metal_test: the far draw was %s, and its buffer held a different fill, "
             "so an undiscarded one would have changed the pixel\n",
             far_discarded ? "DISCARDED (the pixel did not change)"
                           : "NOT discarded -- the pixel changed");
      printf("air_metal_test: the nearer draw %s\n",
             nearer_drawn ? "PASSED (the pixel changed)"
                          : "did NOT pass, and it is in front of what was there");
      printf("air_metal_test: the Always control at the same far depth %s\n",
             control_drawn ? "DREW (so the compare function discarded the other one, not "
                             "the draw)"
                           : "did not draw either, so the fault is not the depth test");
      printf("air_metal_test: depth after each step: %.9g %.9g %.9g %.9g (0.5, 0.5 kept, "
             "0.25, 0.75)\n", (double)depth_at[0], (double)depth_at[1], (double)depth_at[2],
             (double)depth_at[3]);
      if (!depth_is(depth_at[0], 0.5f, tol) || !depth_is(depth_at[1], 0.5f, tol) ||
          !depth_is(depth_at[2], 0.25f, tol) || !depth_is(depth_at[3], 0.75f, tol))
         ok = false;
      if (!far_discarded || !nearer_drawn || !control_drawn)
         ok = false;
      printf("air_metal_test: %s\n",
             ok ? "DEPTH TESTING DISCARDS -- the compare function, the depth write and the "
                  "buffer all agree"
                : "SOMETHING ABOVE DISAGREES, and a depth test that is nearly right is "
                  "worse than none");

      /*
       * And the stencil, when the attachment has one, which is three more draws of the
       * same quad with the depth test out of the way (Always, no depth write) so that
       * only the stencil can discard anything:
       *
       *   - Equal against a reference of 1 over a buffer cleared to 0, which must
       *     discard;
       *   - Always with depthStencilPassOperation Replace and a reference of 7, which
       *     must draw AND leave 7 in the buffer -- the operation table is where Metal
       *     and Gallium disagree in order (MTLStencilOperationInvert is 5 in one and 7
       *     in the other), so what lands in the buffer is the check on that table;
       *   - Equal against 7, which must now draw, because the 7 the previous step wrote
       *     is the 7 this one reads. That pair is what tells a stencil write from a
       *     stencil read: either alone could be a no-op and look right.
       */
      static const struct { const char *what; unsigned compare, pass_op, ref; uint8_t fill;
                            bool clear; bool expect_draw; unsigned expect_stencil; } sstep[] = {
         { "stencil Equal 1 over a buffer cleared to 0", 2, 0, 1, 0x11, true,  false, 0 },
         { "stencil Always, Replace, reference 7",       7, 2, 7, 0x22, false, true,  7 },
         { "stencil Equal 7 over the 7 just written",    2, 0, 7, 0x33, false, true,  7 },
      };
      if (zs_stencil) {
         unsigned before[4];
         uint8_t stencil_map[TEX * TEX];
         bool sok = true;

         memcpy(before, pixel[COUNT_OF(step) - 1], sizeof(before));
         for (unsigned s = 0; s < COUNT_OF(sstep); s++) {
            struct rwa_depth_stencil_state st = {
               .depth_compare_function = 7,   /* Always: only the stencil may discard */
               .depth_write_enabled = 0,
               .front = { .compare_function = sstep[s].compare,
                          .depth_stencil_pass_operation = sstep[s].pass_op,
                          .read_mask = 0xffffffff, .write_mask = 0xffffffff },
            };
            st.back = st.front;
            struct rwa_depth_stencil *ds = rwa_depth_stencil_create(device, &st, why,
                                                                    sizeof(why));
            if (!ds) {
               printf("air_metal_test: rwa_depth_stencil_create (%s): %s\n", sstep[s].what,
                      why);
               return 1;
            }
            for (unsigned i = 0; i < nbuf; i++) {
               uint8_t *bits = rwa_buffer_map(buffers[i]);
               for (unsigned k = 0; k < rwa_buffer_size(buffers[i]); k++)
                  bits[k] = sstep[s].fill;
            }
            const unsigned clear_stencil = 0;
            const struct rwa_draw draw = {
               .color = color, .color_count = color_count,
               .depth = zt,
               .clear_stencil = sstep[s].clear ? &clear_stencil : NULL,
               .depth_stencil = ds,
               .stencil_reference = { sstep[s].ref, sstep[s].ref },
               .fragment = { .buffers = buffers, .buffer_count = nbuf,
                             .textures = textures, .texture_count = ntex,
                             .samplers = samplers, .sampler_count = nsamp },
               .primitive = 4, .vertex_start = 0, .vertex_count = 4,
            };
            int rc = rwa_render_draw(render, &draw, why, sizeof(why));
            rwa_depth_stencil_destroy(ds);
            if (rc != 0) {
               printf("air_metal_test: rwa_render_draw (%s): %s\n", sstep[s].what, why);
               return 1;
            }
            unsigned now[4] = { 0 };
            const unsigned one[3] = { 1, 1, 1 };
            const unsigned extent[3] = { TEX, TEX, 1 };
            if (rwa_texture_read(color[read_rt], 0, 0, origin, one, now, 0, why,
                                 sizeof(why)) != 0 ||
                rwa_depth_read(zt, 0, 0, origin, extent, NULL, stencil_map, why,
                               sizeof(why)) != 0) {
               printf("air_metal_test: reading back after %s: %s\n", sstep[s].what, why);
               return 1;
            }
            bool drew = memcmp(now, before, sizeof(before)) != 0;
            bool stencil_right = true;
            for (unsigned i = 0; i < TEX * TEX; i++)
               if (stencil_map[i] != sstep[s].expect_stencil)
                  stencil_right = false;
            printf("air_metal_test: %s: the draw %s (expected %s), and the stencil buffer "
                   "holds %u everywhere expecting %u\n", sstep[s].what,
                   drew ? "DREW" : "was DISCARDED",
                   sstep[s].expect_draw ? "to draw" : "to be discarded", stencil_map[0],
                   sstep[s].expect_stencil);
            if (drew != sstep[s].expect_draw || !stencil_right)
               sok = false;
            memcpy(before, now, sizeof(before));
         }
         printf("air_metal_test: %s\n",
                sok ? "THE STENCIL TEST DISCARDS AND THE STENCIL WRITE LANDS"
                    : "THE STENCIL DISAGREES somewhere above");
         ok = ok && sok;
      } else {
         printf("air_metal_test: this attachment has no stencil aspect, so nothing here "
                "measures the stencil test; pixel format %u does\n",
                MTL_DEPTH32_FLOAT_STENCIL8);
      }
   }

   rwa_depth_stencil_destroy(less);
   rwa_depth_stencil_destroy(always);
   rwa_render_destroy(render);
   for (unsigned i = 0; i < nbuf; i++)
      rwa_buffer_destroy(buffers[i]);
   for (unsigned i = 0; i < ntex; i++)
      rwa_texture_destroy(textures[i]);
   for (unsigned i = 0; i < nsamp; i++)
      rwa_sampler_destroy(samplers[i]);
   for (unsigned i = 0; i < color_count; i++)
      rwa_texture_destroy(color[i]);
   rwa_texture_destroy(zt);
   rwa_device_close(device);
   return ok ? 0 : 1;
}

static bool
check_no_depth_case(struct rwa_device *device, struct metallib *lib, const char *func,
                    const float want[4], const char *label)
{
   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      return false;

   char why[WHY] = "";
   struct rwa_render_shape shape = {
      .color_count = 1,
      .color_format = { MTL_RGBA32_FLOAT },
      .sample_count = 1,
      .depth_format = 0,
      .stencil_format = 0,
   };
   struct rwa_render_info info = { 0 };
   struct rwa_render *render =
      rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &shape, &info,
                        why, sizeof(why));
   if (!render) {
      printf("air_metal_test: %s (%s): rwa_render_create: %s\n", label, name, why);
      return false;
   }
   if (info.fragment_writes_depth != 0) {
      printf("air_metal_test: %s (%s): fragment_writes_depth=%u, expected 0\n",
             label, name, info.fragment_writes_depth);
      rwa_render_destroy(render);
      return false;
   }

   const struct rwa_texture_shape ts = {
      .pixel_format = MTL_RGBA32_FLOAT, .type = MTL_TEXTURE_TYPE_2D,
      .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1,
      .usage = MTL_USAGE_SHADER_READ | 4 /* MTLTextureUsageRenderTarget */,
   };
   struct rwa_texture *color = rwa_texture_create(device, &ts, why, sizeof(why));
   if (!color) {
      printf("air_metal_test: %s: rwa_texture_create: %s\n", label, why);
      rwa_render_destroy(render);
      return false;
   }

   struct rwa_buffer *buffers[4] = { 0 };
   unsigned nbuf = info.fragment_buffers < COUNT_OF(buffers) ? info.fragment_buffers
                                                             : (unsigned)COUNT_OF(buffers);
   for (unsigned i = 0; i < nbuf; i++) {
      buffers[i] = rwa_buffer_create(device, 64, why, sizeof(why));
      if (!buffers[i]) {
         printf("air_metal_test: %s: rwa_buffer_create(%u): %s\n", label, i, why);
         for (unsigned j = 0; j < i; j++)
            rwa_buffer_destroy(buffers[j]);
         rwa_texture_destroy(color);
         rwa_render_destroy(render);
         return false;
      }
      memset(rwa_buffer_map(buffers[i]), 0, 64);
   }

   const float clear_zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
   const struct rwa_draw draw = {
      .color = &color, .color_count = 1,
      .clear_color = clear_zero,
      .fragment = { .buffers = buffers, .buffer_count = nbuf },
      .primitive = 4, .vertex_start = 0, .vertex_count = 4,
   };
   bool ok = true;
   if (rwa_render_draw(render, &draw, why, sizeof(why)) != 0) {
      printf("air_metal_test: %s: rwa_render_draw: %s\n", label, why);
      ok = false;
   }
   float got[4] = { 0 };
   const unsigned origin[3] = { 0, 0, 0 };
   const unsigned one[3] = { 1, 1, 1 };
   if (ok && rwa_texture_read(color, 0, 0, origin, one, got, 0, why, sizeof(why)) != 0) {
      printf("air_metal_test: %s: rwa_texture_read: %s\n", label, why);
      ok = false;
   }
   if (ok) {
      printf("air_metal_test: %s (%s): fragment_writes_depth=%u pixel=(%.4f, %.4f, %.4f, %.4f)\n",
             label, name, info.fragment_writes_depth,
             (double)got[0], (double)got[1], (double)got[2], (double)got[3]);
      for (unsigned c = 0; c < 4; c++) {
         float d = got[c] - want[c];
         if (!(got[c] == got[c]) || (d < 0.0f ? -d : d) > 1e-3f) {
            printf("air_metal_test: %s: channel %u mismatch got=%.6f want=%.6f\n",
                   label, c, (double)got[c], (double)want[c]);
            ok = false;
         }
      }
   }
   for (unsigned i = 0; i < nbuf; i++)
      rwa_buffer_destroy(buffers[i]);
   rwa_texture_destroy(color);
   rwa_render_destroy(render);
   return ok;
}

static int
run_depth_store_filter(const char *path, const char *temp_func, const char *depth_func)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;
   setenv("RWA_SOFTWARE", "1", 0);
   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   const float want_temp[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
   const float want_depth[4] = { 0.5f, 0.25f, 0.75f, 1.0f };
   bool ok = check_no_depth_case(device, &lib, temp_func, want_temp, "function_temp_preserved");
   ok &= check_no_depth_case(device, &lib, depth_func, want_depth, "depth_store_dropped");
   rwa_device_close(device);
   return ok ? 0 : 1;
}

/*
 * --draw-ms: a multisample texture, filled the only way anything can fill one, and read
 * by the shader that declared it.
 *
 * Six of Apple's 271 fragment shaders declare one -- texture2d_ms<float, read> and its
 * int, uint, depth and stencil siblings -- and none of them had ever been drawn, because
 * nothing in this path had ever bound a multisample texture. What that cost was not a
 * binding: it was that THERE IS NO WAY TO PUT A KNOWN TEXEL IN ONE from the processor.
 * Metal forbids it twice (a multisample texture cannot be MTLStorageModeShared on this
 * device, and -replaceRegion: writes only what the processor can see) and so does
 * freedreno, which lays the samples of a texel out in an order of its own. So the fill
 * path is the only one either side has: a render pass whose attachment it is.
 *
 * That makes this mode two passes, which is the whole of its shape:
 *
 *   pass 1   the fill. A quad of Apple's clearFragment20 drawn into the multisample
 *            texture as colour attachment 2 (that shader writes render target 2, so
 *            targets 0 and 1 have to exist for 2 to be bound), after a clear to zero.
 *   pass 2   the measurement. An ORDINARY one-sample pass whose fragment shader is the
 *            one under test, with that multisample texture bound at texture 0. Only the
 *            TEXTURE is multisample; a shader reading one does not need a multisample
 *            pass to read it in.
 *
 * And it runs that twice, because the two configurations measure different things:
 *
 *   uniform  a 4x4 texture and the whole quad, so every sample of every texel holds the
 *            fill colour. Apple's four averaging shaders then have to print exactly that
 *            colour -- they loop i = 0 .. get_num_samples()-1, sum read(coord, i) and
 *            divide by the count -- so the number says the texture was filled, bound and
 *            read, and (for the integer ones, where the sum wraps) that all N reads
 *            happened. It does NOT say the sample index selected anything: four equal
 *            samples average to the same value however the index is treated.
 *   split    a 1x1 texture and the first triangle of the strip (vertexCount 3), whose
 *            hypotenuse cuts that single pixel corner to corner. Some of its samples are
 *            covered and some are not, so the texel holds two different values and the
 *            average is neither of them. THAT is the sample index measured: a shader that
 *            read one sample N times, or read sample 0 N times, prints a value that is
 *            one of the two and not the mean.
 *
 * Nothing of the second half of either configuration runs on this machine, and the
 * reason is not this library: softpipe has no multisample textures at all. Its
 * is_format_supported refuses every sample count above one for every format and every
 * binding (softpipe/sp_screen.c), and it says so about itself -- caps->fake_sw_msaa is
 * true, which asks a frontend to pretend with one sample. freedreno's a6xx takes 2 and 4
 * (a6xx/fd6_screen.cc, valid_sample_count). So on this machine the mode measures
 * everything up to the texture and then prints the back end's own refusal, which is the
 * measurement it can make; on the Adreno it measures the rest.
 *
 * What it does measure here, and what makes the phone's number isolable:
 *
 *   - the declaration, from the same query the refusal judges by (rwa_module_textures);
 *   - the six descriptors that MUST be refused, each for its own reason, with the
 *     sentence each gives -- including a sampleCount of 1 on a multisample type, which
 *     is Metal's own rule and not a limit here;
 *   - pass 1 at ONE sample into an ordinary 2D texture, read back: that is the fill
 *     colour the multisample run has to end up printing, and the coverage map of the
 *     triangle, measured rather than assumed;
 *   - the whole chain at one sample for a shader that declares a plain texture2d, which
 *     is this mode run with SAMPLES = 1 on (say) texturedFragment2DUInt: render into a
 *     texture in one pass and sample it in the next. That is the control for the phone --
 *     if the multisample run comes back wrong, this says whether render-then-sample was
 *     ever right.
 *
 *   air_metal_test --draw-ms FILE.metallib FUNCTION [SAMPLES [FILLFUNCTION]]
 */

