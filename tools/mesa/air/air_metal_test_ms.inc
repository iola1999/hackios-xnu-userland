/* Multisample tests, included into air_metal_test.c. */

/* The pixel format for a declared element class: the 32-bit RGBA format of that class,
 * which is what the three test programs of this port bind and read back, so a texel
 * written here and a texel read on the phone are the same number. */
static unsigned
ms_format_for_element(unsigned element)
{
   switch (element) {
   case RWA_ELEMENT_INT:  return MTL_RGBA32_SINT;
   case RWA_ELEMENT_UINT: return MTL_RGBA32_UINT;
   default:               return MTL_RGBA32_FLOAT;
   }
}

static void
print_components(unsigned element, const uint32_t v[4])
{
   for (unsigned c = 0; c < 4; c++) {
      if (element == RWA_ELEMENT_INT) {
         printf(" %d", (int32_t)v[c]);
      } else if (element == RWA_ELEMENT_UINT) {
         printf(" %u", v[c]);
      } else {
         float f;
         memcpy(&f, &v[c], sizeof(f));
         printf(" %.9g", (double)f);
      }
   }
}

/* A descriptor that must not be made, and the sentence it is refused with. The sentence
 * is the point: a refusal nobody can read is a refusal that will be mistaken for a
 * limitation of the hardware. */
static bool
ms_must_refuse(struct rwa_device *device, const struct rwa_texture_shape *ts,
               const char *what)
{
   char why[WHY] = "";
   struct rwa_texture *t = rwa_texture_create(device, ts, why, sizeof(why));

   if (t) {
      printf("air_metal_test: %s was ACCEPTED and it must not be\n", what);
      rwa_texture_destroy(t);
      return false;
   }
   printf("air_metal_test: %s is refused: %s\n", what, why);
   return true;
}

static int
run_ms(const char *path, const char *func, const char *samplespec, const char *fillfunc)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      return 1;

   /* The fill shader, out of the same container: on the device both stages' bytes come
    * from two -[_MTLFunction bitcodeData] calls on one MTLLibrary, which is these two
    * find_function calls. clearFragment20 by default because its colour is already
    * attested -- it is the shader of docs/metal.md 10.22, whose pixel is the four words
    * 2458616957 2930221209 3401825461 3873429713 in a RGBA32Uint attachment.
    */
   char fname[256];
   const uint8_t *fbitcode = NULL;
   size_t fbitcode_size = 0;
   if (!fillfunc || !*fillfunc)
      fillfunc = "clearFragment20";
   if (!find_function(&lib, fillfunc, fname, sizeof(fname), &fbitcode, &fbitcode_size))
      return 1;

   unsigned samples = samplespec && *samplespec
                         ? (unsigned)strtoul(samplespec, NULL, 0) : 4;
   if (samples == 0)
      samples = 1;

   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: --draw-ms %s on \"%s\", %u sample%s per pixel, filled by %s\n",
          name, rwa_device_name(device), samples, samples == 1 ? "" : "s", fname);

   /*
    * What the shader says it wants bound, asked of the same query the binding check
    * judges by. The texture this mode makes is that declaration plus the sample count
    * asked for on the command line, and nothing else: a type invented here would make
    * the refusal below measure this program.
    */
   struct rwa_texture_decl decl[16];
   unsigned ndecl = 0;
   if (rwa_module_textures(device, bitcode, bitcode_size, NULL, decl, COUNT_OF(decl),
                           &ndecl, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_module_textures: %s\n", why);
      rwa_device_close(device);
      return 1;
   }
   if (ndecl != 1 || decl[0].index != 0) {
      printf("air_metal_test: %s declares %u texture slots, and this mode fills one at "
             "index 0\n", name, ndecl);
      rwa_device_close(device);
      return 1;
   }
   unsigned type = decl[0].type_known ? decl[0].type : MTL_TEXTURE_TYPE_2D;
   unsigned element = decl[0].element;
   unsigned format = ms_format_for_element(element);
   printf("air_metal_test: it declares texture 0 as %s: %s, element %s%s, usage 0x%x; "
          "bound as %s of pixel format %u\n", decl[0].metal_type[0] ? decl[0].metal_type
                                                                    : "(no type name)",
          texture_type_name(type), element_name(element),
          decl[0].element_bits ? "" : " (from the instructions, width unstated)",
          decl[0].usage, texture_type_name(type), format);

   /*
    * The descriptors that must be refused, each for its own reason. Five of the six are
    * Metal's own rules or the shader language's, and only the last is a back end's --
    * which is what makes the last one the interesting line: it is the one whose answer
    * differs between this machine and the phone.
    */
   /* Four samples whatever the command line asked for: these five lines are about
    * Metal's rules and the back end's list of counts, not about the count under test,
    * and with SAMPLES = 1 (the one-sample control run) a battery built from it would
    * measure nothing -- every line would be refused by the first rule.
    */
   const struct rwa_texture_shape base = {
      .pixel_format = format, .type = MTL_TEXTURE_TYPE_2D_MULTISAMPLE,
      .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 4,
      .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
   };
   unsigned refused = 0, must = 0;
   struct rwa_texture_shape ts;

   ts = base; ts.sample_count = 1;
   must++; refused += ms_must_refuse(device, &ts,
      "a MTLTextureType2DMultisample with a sampleCount of 1") ? 1 : 0;
   ts = base; ts.mip_levels = 2;
   must++; refused += ms_must_refuse(device, &ts,
      "a four-sample texture with two mip levels") ? 1 : 0;
   ts = base; ts.usage |= MTL_USAGE_SHADER_WRITE;
   must++; refused += ms_must_refuse(device, &ts,
      "a four-sample texture with MTLTextureUsageShaderWrite") ? 1 : 0;
   ts = base; ts.type = MTL_TEXTURE_TYPE_2D;
   must++; refused += ms_must_refuse(device, &ts,
      "a MTLTextureType2D with a sampleCount of 4") ? 1 : 0;
   ts = base; ts.sample_count = 3;
   must++; refused += ms_must_refuse(device, &ts,
      "a multisample texture of three samples, which no back end has") ? 1 : 0;
   printf("air_metal_test: %u of %u descriptors that must be refused were\n", refused,
          must);

   /*
    * One of the six cannot be compiled on this machine at all, and the reason is neither
    * Metal's nor this library's: texturedFragment2DStencilMultisample reads 16-bit
    * texels (texture2d_ms<ushort, read>, and its read returns <4 x i16>), and the path
    * softpipe takes NIR through has no 16-bit texture return type --
    * nir_to_tgsi.c's ntt_emit_texture switches on nir_type_float32/int32/uint32 and
    * calls unreachable("unknown texture type") on anything else, which ABORTS THE
    * PROCESS. Its non-multisample twin texturedFragment2DStencil aborts the same way and
    * always has, which is part of why 263 of Apple's fragment shaders draw on the Adreno
    * and only 256 are compared with a host reference.
    *
    * So it is said and skipped rather than run into. This is the one place this program
    * looks at which back end it got, because the difference is the back end's: ir3
    * compiles 16-bit texture reads, and a crash here would read as a fault of the
    * library under test.
    */
   if (decl[0].element_bits == 16 && !strcmp(rwa_device_name(device), "softpipe")) {
      printf("air_metal_test: %s reads %u-bit texels, and nir_to_tgsi -- the path softpipe "
             "takes NIR through -- has no 16-bit texture return type: ntt_emit_texture "
             "calls unreachable(\"unknown texture type\") and the process dies. Not "
             "attempted here; on the Adreno ir3 compiles it\n", name, decl[0].element_bits);
      rwa_device_close(device);
      return 0;
   }

   /*
    * Pass 1 at ONE sample, into an ordinary 2D texture, read back. Two things come out
    * of this and both are needed to read the multisample run:
    *
    *   - the fill colour. Whatever clearFragment20 writes, the averaging shaders have to
    *     print exactly that when every sample holds it, and this is the only place the
    *     number can be measured rather than asserted (a multisample texture cannot be
    *     read back at all).
    *   - the coverage map of the triangle, which says which pixels of the quad's first
    *     triangle are inside it at one sample. The pixel the split configuration hangs
    *     on is a pixel this map shows as covered, drawn again with the same triangle at
    *     N samples: its samples are then the ones the hypotenuse cuts.
    */
   struct rwa_render_shape fill_shape = { .color_count = 3, .sample_count = 1 };
   for (unsigned i = 0; i < 3; i++)
      fill_shape.color_format[i] = format;
   struct rwa_render_info finfo = { 0 };
   struct rwa_render *fill =
      rwa_render_create(device, NULL, 0, NULL, fbitcode, fbitcode_size, fillfunc,
                        &fill_shape, &finfo, why, sizeof(why));
   if (!fill) {
      printf("air_metal_test: rwa_render_create (the fill pass at one sample): %s\n", why);
      rwa_device_close(device);
      return 1;
   }
   struct rwa_buffer *fbuf[16] = { 0 };
   unsigned nfbuf = finfo.fragment_buffers < COUNT_OF(fbuf) ? finfo.fragment_buffers
                                                            : (unsigned)COUNT_OF(fbuf);
   for (unsigned i = 0; i < nfbuf; i++) {
      fbuf[i] = rwa_buffer_create(device, 48, why, sizeof(why));
      if (!fbuf[i]) {
         printf("air_metal_test: rwa_buffer_create (fill buffer %u): %s\n", i, why);
         return 1;
      }
      uint8_t *bits = rwa_buffer_map(fbuf[i]);
      for (unsigned k = 0; k < rwa_buffer_size(fbuf[i]); k++)
         bits[k] = pattern_byte(k);
   }

   uint32_t filled[4] = { 0 };
   {
      struct rwa_texture *one[3] = { 0 };
      for (unsigned i = 0; i < 3; i++) {
         const struct rwa_texture_shape ots = {
            .pixel_format = format, .type = MTL_TEXTURE_TYPE_2D,
            .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
            .sample_count = 1,
            .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
         };
         one[i] = rwa_texture_create(device, &ots, why, sizeof(why));
         if (!one[i]) {
            printf("air_metal_test: rwa_texture_create (one-sample attachment %u): %s\n",
                   i, why);
            return 1;
         }
      }
      for (unsigned tri = 0; tri < 2; tri++) {
         const struct rwa_draw d = {
            .color = one, .color_count = 3,
            .clear_color = (const float[]){ 0, 0, 0, 0 },
            .fragment = { .buffers = fbuf, .buffer_count = nfbuf },
            .primitive = 4,                    /* MTLPrimitiveTypeTriangleStrip */
            .vertex_start = 0, .vertex_count = tri ? 3 : 4,
         };
         if (rwa_render_draw(fill, &d, why, sizeof(why)) != 0) {
            printf("air_metal_test: rwa_render_draw (the fill pass, %u vertices): %s\n",
                   d.vertex_count, why);
            return 1;
         }
         const unsigned one_texel[3] = { 1, 1, 1 };
         printf("air_metal_test: the fill pass at one sample, %u vertices: attachment 2 "
                "holds\n", d.vertex_count);
         for (unsigned y = 0; y < TEX; y++) {
            printf("air_metal_test:  ");
            for (unsigned x = 0; x < TEX; x++) {
               const unsigned at[3] = { x, y, 0 };
               uint32_t px[4] = { 0 };
               if (rwa_texture_read(one[2], 0, 0, at, one_texel, px, 0, why,
                                    sizeof(why)) != 0) {
                  printf("air_metal_test: rwa_texture_read: %s\n", why);
                  return 1;
               }
               printf(" %s", px[0] || px[1] || px[2] || px[3] ? "drawn" : "clear");
               if (!tri && x == 0 && y == 0)
                  memcpy(filled, px, sizeof(filled));
            }
            printf("\n");
         }
      }
      printf("air_metal_test: so the fill colour is");
      print_components(element, filled);
      printf(", and that is what a mean over samples that all hold it must come to\n");

      /* And the attachment check itself, measured rather than described. The helper that
       * judges an attachment now reads its sample count as well as its type, and a
       * wrong TYPE is the half of it this machine can exercise: a 3D texture is
       * makeable here and a multisample one is not.
       */
      {
         const struct rwa_texture_shape vol = {
            .pixel_format = format, .type = MTL_TEXTURE_TYPE_3D,
            .width = TEX, .height = TEX, .depth = TEX, .array_length = 1,
            .mip_levels = 1, .sample_count = 1,
            .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
         };
         struct rwa_texture *t3 = rwa_texture_create(device, &vol, why, sizeof(why));
         if (t3) {
            struct rwa_texture *wrong[3] = { t3, one[1], one[2] };
            const struct rwa_draw d = {
               .color = wrong, .color_count = 3,
               .fragment = { .buffers = fbuf, .buffer_count = nfbuf },
               .primitive = 4, .vertex_start = 0, .vertex_count = 4,
            };
            if (rwa_render_draw(fill, &d, why, sizeof(why)) == 0)
               printf("air_metal_test: a 3D texture as colour attachment 0 was ACCEPTED "
                      "and it must not be\n");
            else
               printf("air_metal_test: a 3D texture as colour attachment 0 is refused: "
                      "%s\n", why);
            rwa_texture_destroy(t3);
         }
      }
      for (unsigned i = 0; i < 3; i++)
         rwa_texture_destroy(one[i]);
   }
   rwa_render_destroy(fill);

   /*
    * The measuring pipeline: an ordinary one-sample pass with the shader under test.
    * Four colour attachments because the four averaging shaders write four render
    * targets; a shader that writes one leaves the other three holding the clear.
    *
    * A depth attachment only if the shader stores to [[depth]], which is asked by
    * trying without one: texturedFragment2DDepthMultisample writes depth and nothing to
    * colour, and this library refuses such a pipeline by name when there is nowhere for
    * the store to go (docs/metal.md 10.30). Refusing first and retrying is how the
    * question gets asked without a table in this program saying which shaders they are.
    */
   struct rwa_render_shape shape = { .color_count = 4, .sample_count = 1 };
   for (unsigned i = 0; i < 4; i++)
      shape.color_format[i] = format;
   struct rwa_render_info info = { 0 };
   struct rwa_render *render =
      rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &shape, &info,
                        why, sizeof(why));
   if (!render) {
      printf("air_metal_test: rwa_render_create with no depth attachment: %s\n", why);
      shape.depth_format = MTL_DEPTH32_FLOAT;
      render = rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func,
                                 &shape, &info, why, sizeof(why));
      if (!render) {
         printf("air_metal_test: rwa_render_create with one: %s\n", why);
         rwa_device_close(device);
         return 1;
      }
   }
   printf("air_metal_test: pipeline: %u fragment buffers, %u fragment textures, %u "
          "samplers, vertex stage %s, and the fragment shader stores %s to [[depth]], "
          "depth attachment %s\n", info.fragment_buffers, info.fragment_textures,
          info.fragment_samplers, info.generated_vertex_stage ? "generated" : "supplied",
          info.fragment_writes_depth ? "a value" : "nothing",
          shape.depth_format ? "there" : "none");

   struct rwa_buffer *buffers[16] = { 0 };
   unsigned nbuf = info.fragment_buffers < COUNT_OF(buffers) ? info.fragment_buffers
                                                             : (unsigned)COUNT_OF(buffers);
   for (unsigned i = 0; i < nbuf; i++) {
      buffers[i] = rwa_buffer_create(device, 48, why, sizeof(why));
      if (!buffers[i]) {
         printf("air_metal_test: rwa_buffer_create (buffer %u): %s\n", i, why);
         return 1;
      }
      uint8_t *bits = rwa_buffer_map(buffers[i]);
      for (unsigned k = 0; k < rwa_buffer_size(buffers[i]); k++)
         bits[k] = pattern_byte(k);
   }
   /* One sampler per slot the contract counts. These shaders read with
    * read(coord, sample), and Apple marks their sampler argument air.arg_unused, which
    * the frontend does not list, so the count is normally 0. In pixels and nearest,
    * which is what a read() would want if it looked. */
   struct rwa_sampler *samplers[16] = { 0 };
   unsigned nsamp = info.fragment_samplers < COUNT_OF(samplers)
                       ? info.fragment_samplers : (unsigned)COUNT_OF(samplers);
   for (unsigned i = 0; i < nsamp; i++) {
      const struct rwa_sampler_state state = {
         .address = { 0, 0, 0 }, .min_filter = 0, .mag_filter = 0, .mip_filter = 0,
         .compare_function = 0, .max_anisotropy = 1, .normalized_coordinates = 0,
         .lod_min = 0.0f, .lod_max = 0.0f,
      };
      samplers[i] = rwa_sampler_create(device, &state, why, sizeof(why));
      if (!samplers[i]) {
         printf("air_metal_test: rwa_sampler_create (sampler %u): %s\n", i, why);
         return 1;
      }
   }

   /* Pass 2's own attachments: four colour, one sample, and a depth one if the shader
    * writes depth -- in which case the depth buffer IS the result and the colour is
    * not, as in docs/metal.md 10.30. */
   struct rwa_texture *color[4] = { 0 };
   for (unsigned i = 0; i < 4; i++) {
      const struct rwa_texture_shape cts = {
         .pixel_format = format, .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1,
         .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
      };
      color[i] = rwa_texture_create(device, &cts, why, sizeof(why));
      if (!color[i]) {
         printf("air_metal_test: rwa_texture_create (attachment %u): %s\n", i, why);
         return 1;
      }
   }
   struct rwa_texture *zt = NULL;
   if (shape.depth_format) {
      const struct rwa_texture_shape zts = {
         .pixel_format = shape.depth_format, .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1, .usage = MTL_USAGE_RENDER_TARGET,
      };
      zt = rwa_texture_create(device, &zts, why, sizeof(why));
      if (!zt) {
         printf("air_metal_test: rwa_texture_create (the depth attachment): %s\n", why);
         return 1;
      }
   }

   /*
    * And the two configurations. Each makes the texture the shader declared -- with the
    * sample count asked for -- renders the fill into it, and then draws the shader under
    * test with it bound.
    */
   static const struct { const char *what; unsigned size, verts; } config[] = {
      { "uniform: every sample of every texel covered", TEX, 4 },
      { "split: one pixel, cut corner to corner by the triangle's hypotenuse", 1, 3 },
   };
   int rc = 0;
   for (unsigned c = 0; c < COUNT_OF(config); c++) {
      printf("air_metal_test: --- %s\n", config[c].what);

      struct rwa_texture *ms[3] = { 0 };
      bool made = true;
      for (unsigned i = 0; i < 3 && made; i++) {
         const struct rwa_texture_shape mts = {
            .pixel_format = format, .type = type,
            .width = config[c].size, .height = config[c].size, .depth = 1,
            .array_length = 1, .mip_levels = 1,
            .sample_count = samples,
            .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
         };
         ms[i] = rwa_texture_create(device, &mts, why, sizeof(why));
         if (!ms[i]) {
            printf("air_metal_test: the %ux%u %s of %u samples could not be made: %s\n",
                   config[c].size, config[c].size, texture_type_name(type), samples, why);
            made = false;
            rc = 1;
         }
      }
      if (made) {
         /* Neither the processor's way in nor its way out exists for a multisample
          * texture, and both refusals are measured here rather than described: a caller
          * that believed either worked would be reading or writing bytes in no defined
          * sample. */
         if (samples > 1) {
            uint32_t one_texel[4] = { 1, 2, 3, 4 };
            const unsigned at[3] = { 0, 0, 0 }, sz[3] = { 1, 1, 1 };
            if (rwa_texture_write(ms[0], 0, 0, at, sz, one_texel, 0, why,
                                  sizeof(why)) == 0)
               printf("air_metal_test: rwa_texture_write into a multisample texture was "
                      "ACCEPTED and it must not be\n");
            else
               printf("air_metal_test: writing texels into it is refused: %s\n", why);
            if (rwa_texture_read(ms[0], 0, 0, at, sz, one_texel, 0, why, sizeof(why)) == 0)
               printf("air_metal_test: rwa_texture_read of a multisample texture was "
                      "ACCEPTED and it must not be\n");
            else
               printf("air_metal_test: reading its texels back is refused: %s\n", why);
         }

         /* The fill pass of this configuration's own sample count. On softpipe this is
          * where a multisample run stops if the texture did not already stop it. */
         struct rwa_render_shape ms_shape = { .color_count = 3, .sample_count = samples };
         for (unsigned i = 0; i < 3; i++)
            ms_shape.color_format[i] = format;
         struct rwa_render_info ms_info = { 0 };
         struct rwa_render *ms_fill =
            rwa_render_create(device, NULL, 0, NULL, fbitcode, fbitcode_size, fillfunc,
                              &ms_shape, &ms_info, why, sizeof(why));
         if (!ms_fill) {
            printf("air_metal_test: the fill pipeline of %u samples: %s\n", samples, why);
            rc = 1;
         } else {
            const struct rwa_draw fd = {
               .color = ms, .color_count = 3,
               .clear_color = (const float[]){ 0, 0, 0, 0 },
               .fragment = { .buffers = fbuf, .buffer_count = nfbuf },
               .primitive = 4,
               .vertex_start = 0, .vertex_count = config[c].verts,
            };
            if (rwa_render_draw(ms_fill, &fd, why, sizeof(why)) != 0) {
               printf("air_metal_test: the fill draw of %u vertices into %u samples: %s\n",
                      config[c].verts, samples, why);
               rc = 1;
            } else {
               printf("air_metal_test: filled a %ux%u %s of %u sample%s with %u "
                      "vertices\n", config[c].size, config[c].size,
                      texture_type_name(type), samples, samples == 1 ? "" : "s",
                      config[c].verts);

               struct rwa_texture *bound[1] = { ms[2] };
               const struct rwa_draw d = {
                  .color = color, .color_count = 4,
                  .clear_color = (const float[]){ 0, 0, 0, 0 },
                  .depth = zt,
                  .clear_depth = zt ? (const float[]){ 1.0f } : NULL,
                  .fragment = { .buffers = buffers, .buffer_count = nbuf,
                                .textures = bound, .texture_count = 1,
                                .samplers = samplers, .sampler_count = nsamp },
                  .primitive = 4, .vertex_start = 0, .vertex_count = 4,
               };
               if (rwa_render_draw(render, &d, why, sizeof(why)) != 0) {
                  printf("air_metal_test: the measuring draw: %s\n", why);
                  rc = 1;
               } else if (info.fragment_writes_depth) {
                  float z[TEX * TEX];
                  const unsigned at[3] = { 0, 0, 0 }, sz[3] = { TEX, TEX, 1 };
                  if (rwa_depth_read(zt, 0, 0, at, sz, z, NULL, why, sizeof(why)) != 0) {
                     printf("air_metal_test: rwa_depth_read: %s\n", why);
                     rc = 1;
                  } else {
                     bool same = true;
                     for (unsigned i = 1; i < TEX * TEX; i++)
                        same = same && z[i] == z[0];
                     printf("air_metal_test: %s stored %.9g to [[depth]]%s\n", name,
                            (double)z[0], same ? ", in all 16 pixels" : " at (0, 0)");
                  }
               } else {
                  const unsigned at[3] = { 0, 0, 0 }, sz[3] = { 1, 1, 1 };
                  uint32_t px[4] = { 0 };
                  if (rwa_texture_read(color[0], 0, 0, at, sz, px, 0, why,
                                       sizeof(why)) != 0) {
                     printf("air_metal_test: rwa_texture_read (attachment 0): %s\n", why);
                     rc = 1;
                  } else {
                     printf("air_metal_test: %s printed", name);
                     print_components(element, px);
                     printf("\n");
                  }
               }
            }
            rwa_render_destroy(ms_fill);
         }
      }
      for (unsigned i = 0; i < 3; i++)
         rwa_texture_destroy(ms[i]);
   }

   rwa_render_destroy(render);
   for (unsigned i = 0; i < nbuf; i++)
      rwa_buffer_destroy(buffers[i]);
   for (unsigned i = 0; i < nfbuf; i++)
      rwa_buffer_destroy(fbuf[i]);
   for (unsigned i = 0; i < nsamp; i++)
      rwa_sampler_destroy(samplers[i]);
   for (unsigned i = 0; i < 4; i++)
      rwa_texture_destroy(color[i]);
   rwa_texture_destroy(zt);
   rwa_device_close(device);
   return rc;
}

/*
 * --draw-index: whether the index buffer is read, and whether it is read as Metal says.
 *
 * Every drawing call in a real application is -drawIndexedPrimitives:, and until this
 * mode nothing in this library had drawn one. The hard part of measuring it is not
 * getting a picture -- it is getting a picture that could not have come out of a driver
 * that ignored the index buffer altogether. A draw of indices 0,1,2,3 over the generated
 * quad is exactly the picture the non-indexed draw of vertices 0..3 gives, so on its own
 * it proves nothing: it is the control, not the measurement.
 *
 * So the measurement is a PERMUTATION. One index buffer holds
 *
 *     0 1 2 3   1 2 3
 *
 * and three indices out of it make one triangle of the quad:
 *
 *   - 0,1,2 (at byte 0) is (-1,-1), (1,-1), (-1,1): the half of the attachment below the
 *     diagonal, and the pixel at (0, 0) is its right angle;
 *   - 1,2,3 (at byte 8) is (1,-1), (-1,1), (1,1): the other half, and (3, 3) is its.
 *
 * Those two draws differ in nothing but WHERE IN THE INDEX BUFFER they start -- same
 * count, same primitive, same base vertex, same buffer -- so a driver that ignored the
 * index buffer or the offset would draw the first triangle twice and (3, 3) would come
 * back as the clear colour both times. The two corners are what the verdict is made of
 * because their pixel centres are strictly inside one triangle each: the four pixels on
 * the diagonal (x + y = 3) have their centres exactly ON the shared edge, and which
 * triangle gets them is the rasteriser's fill rule, so the total covered count is
 * printed and nothing is concluded from it.
 *
 * The same indices are then drawn as MTLIndexTypeUInt32, which must give the same two
 * pictures: an implementation that read 32-bit indices two bytes at a time would see
 * 0, 0, 1 -- a triangle of zero area -- and leave the attachment at its clear colour.
 *
 * baseVertex is measured against the permutation rather than on its own: indices 0,1,2
 * with baseVertex 1 are the vertex sequence 1,2,3, which is the very sequence the second
 * triangle above is drawn from. So that draw must give the SECOND picture exactly -- not
 * merely something different from the first -- and a driver that dropped baseVertex
 * would give the first.
 *
 * instanceCount cannot be seen in a colour attachment at all: every instance of this
 * geometry covers the same pixels with the same colour, so one instance and three are the
 * same picture. It is measured in the STENCIL instead, with the operation that counts:
 * compare Always and depthStencilPassOperation IncrementClamp over a buffer cleared to
 * zero leaves the number of fragments that covered each pixel, so the buffer holds the
 * instance count exactly. Three instances must leave 3; a draw that dropped the count
 * leaves 1, which is a different number and not a different-looking picture.
 *
 *   air_metal_test --draw-index FILE.metallib FUNCTION [VERTEXFUNCTION]
 *
 * With a VERTEXFUNCTION the two halves are drawn a second time over a SUPPLIED vertex
 * stage -- Apple's own clearVertex is `position = vtx[vertex_id]` -- and that is the path
 * a real application is on: there an index is not a row of a vertex attribute but the
 * [[vertex_id]] the shader indexes its own buffer with, so the permutation has to move
 * the triangle the same way through the frontend's air.vertex_id.
 */

