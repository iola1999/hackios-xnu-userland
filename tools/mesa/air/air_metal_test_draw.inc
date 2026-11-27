/* Texture and basic draw tests, included into air_metal_test.c. */

/*
 * --textures: what every function of a .metallib declares it wants bound, read here.
 *
 * This is the host half of the sweep in userland/src/metal_drv_test.m. That sweep binds
 * one texture kind and one format for a whole library, and so refused 13 of Apple's 271
 * fragment shaders by name -- they declare a texture3d, a texture2d<int> or a multisample
 * texture -- which measured the harness and not the shader. rwa_module_textures was added
 * so a caller can bind what each shader asks for, and this prints that same answer for a
 * whole library, so the expected binding of every function can be read without a boot and
 * a device log can be checked against it afterwards.
 *
 * It is the library's answer and not a second reading of Apple's metadata, which is the
 * only way the table and the device's behaviour can be compared at all: the numbers here
 * come from the same two helpers that refuse a wrong binding there.
 *
 * The entry point is left to the module (NULL): a .metallib holds one LLVM module per
 * function, so there is exactly one to find, and naming it would turn a container whose
 * record name differs from the function's into a refusal about nothing.
 *
 *   air_metal_test --textures FILE.metallib [FUNCTION]
 */
static int
run_textures(const char *path, const char *only)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   struct metallib_table t;
   if (!parse_functions(&lib, &t))
      return 1;

   /* softpipe: the library translates with the screen's own NIR options and this machine
    * has no Adreno. Which screen it is cannot change what a shader declares -- the
    * declaration is Apple's metadata and the shader's own instructions -- but the call
    * takes a device because the pipeline path does, and one translation is the point.
    */
   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      free_functions(&t);
      return 1;
   }

   unsigned functions = 0, with_textures = 0, declared = 0, refused = 0, plain = 0;
   for (unsigned i = 0; i < t.count; i++) {
      if (!t.names[i] || (only && strcmp(only, t.names[i]) != 0))
         continue;
      functions++;

      if (!t.bitcode[i]) {
         refused++;
         printf("textures %-40s outside the file\n", t.names[i]);
         continue;
      }

      struct rwa_texture_decl decl[16];
      unsigned n = 0;
      if (rwa_module_textures(device, t.bitcode[i], t.size[i], NULL, decl, COUNT_OF(decl),
                              &n, why, sizeof(why)) != 0) {
         /* The frontend's own sentence: a module refused here is a module that would be
          * refused at pipeline creation, so this is a row of the table and not a failure
          * of the table.
          */
         refused++;
         printf("textures %-40s no answer: %s\n", t.names[i], why);
         continue;
      }
      if (n == 0) {
         printf("textures %-40s none\n", t.names[i]);
         continue;
      }
      with_textures++;
      for (unsigned k = 0; k < n; k++) {
         declared++;
         /* "a plain texture2d<float>" is the one declaration the sweep could bind
          * before this existed, so it is worth counting apart: everything else is a
          * shader that went unmeasured for want of this call.
          */
         bool is_plain = decl[k].type_known && decl[k].type == MTL_TEXTURE_TYPE_2D &&
                         decl[k].element == RWA_ELEMENT_FLOAT && decl[k].element_bits == 32;
         if (is_plain)
            plain++;
         printf("textures %-40s slot %u  %-32s %s%u%s  usage %u  %s\n", t.names[i],
                decl[k].index,
                decl[k].type_known ? texture_type_name(decl[k].type)
                                   : "(no kind: a computed binding)",
                element_name(decl[k].element),
                decl[k].element_bits,
                decl[k].element_bits ? "" : " (width not declared)", decl[k].usage,
                decl[k].metal_type[0] ? decl[k].metal_type : "(no air.arg_type_name)");
      }
   }

   printf("textures: %s: %u function%s, %u %s %u texture%s in all, %u of those "
          "declarations are a plain 32-bit texture2d<float>, %u modules have no answer\n",
          path, functions, functions == 1 ? "" : "s", with_textures,
          with_textures == 1 ? "declares" : "declare", declared,
          declared == 1 ? "" : "s", plain, refused);

   rwa_device_close(device);
   free_functions(&t);
   return 0;
}

/*
 * The other half of the library, driven the way the Metal driver will drive a draw: a
 * fragment shader out of the container, a vertex stage over it, a few plain textures as
 * colour attachments, and the pixel read back. This is air_run's draw in terms of
 * air_metal.h alone.
 *
 * `vfunc` is NULL for the library's own generated vertex stage, which is the road with
 * 255 drawn and pixel-compared fragment shaders behind it, or the name of a vertex
 * function in the same container -- Apple's own clearVertex, texturedVertex2D or
 * texturedVertex3D -- which is the frontend's vertex path run end to end. The two are
 * one function on purpose: the same fragment shader, the same attachments, the same
 * buffer bytes, so the only thing that changed between two runs is where the vertices
 * came from, and the pixels can be put side by side.
 *
 * The attachments' count and formats are the caller's, as a render pipeline's are in
 * Metal (MTLRenderPipelineDescriptor's colour formats): the contract reports how many
 * buffers, textures and samplers the fragment stage binds, but not how many colours it
 * writes, because a driver has the pipeline descriptor for that. So they are chosen
 * here, defaulting to what clearFragment20 wants -- three MTLPixelFormatRGBA32Uint.
 *
 *   air_metal_test --draw FILE.metallib FUNCTION [MTLPixelFormat[,...]]
 *   air_metal_test --draw-vertex FILE.metallib VERTEXFUNCTION FUNCTION
 *                  [MTLPixelFormat[,...] [VERTEXBUFFERS [MTLPixelFormat[:TYPE][,...]]]]
 */
static int
run_draw(const char *path, const char *vfunc, const char *func, const char *colorspec,
         const char *vtxspec, const char *texspec)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      return 1;
   printf("air_metal_test: --draw %s, %zu bytes of bitcode, %s\n", name, bitcode_size,
          bitcode[0] == 0xde ? "in its 0x0B17C0DE wrapper" : "a bare module");

   /* The vertex module, out of the same container and by the same reader: on the device
    * a driver gets both stages' bytes from two -[_MTLFunction bitcodeData] calls on two
    * functions of one MTLLibrary, which is these two find_function calls.
    */
   char vname[256];
   const uint8_t *vbitcode = NULL;
   size_t vbitcode_size = 0;
   if (vfunc) {
      if (!find_function(&lib, vfunc, vname, sizeof(vname), &vbitcode, &vbitcode_size))
         return 1;
      printf("air_metal_test: vertex stage %s, %zu bytes of bitcode, %s\n", vname,
             vbitcode_size,
             vbitcode[0] == 0xde ? "in its 0x0B17C0DE wrapper" : "a bare module");
   }

   /* The colour attachments. Comma-separated MTLPixelFormats, one per attachment; the
    * count is how many are given. The default is three RGBA32Uint, which is
    * clearFragment20: it writes render target 2, so targets 0 and 1 have to exist for
    * target 2 to be bound at all. Only the three 32-bit RGBA formats, for the readback
    * reason the texture path has: a uint printed as a float reads as 1.4e-43.
    */
   unsigned format[8];
   bool fmt_is_float[COUNT_OF(format)];
   unsigned color_count = 0;
   const char *arg = colorspec;
   if (!arg || !*arg)
      arg = "123,123,123";    /* empty too, so a later argument can be given alone */
   while (arg && *arg && color_count < COUNT_OF(format)) {
      unsigned v = (unsigned)strtoul(arg, NULL, 0);
      /* ... and one of Metal's packed 16-bit colour formats, which --pixel-formats
       * renders INTO: the pixel then comes back as a 16-bit WORD rather than as four
       * numbers, which is the whole point of that direction -- four floats would have
       * gone back out through the same unpacking the write used and said nothing.
       */
      if (!(v >= 40 && v <= 43) && v != MTL_RGBA32_FLOAT && v != MTL_RGBA32_UINT &&
          v != MTL_RGBA32_SINT) {
         fprintf(stderr, "air_metal_test: attachment pixel format %u: this program reads "
                         "back MTLPixelFormatRGBA32Float (%u), RGBA32Uint (%u), "
                         "RGBA32Sint (%u) or one of the packed 16-bit four (40-43)\n",
                 v, MTL_RGBA32_FLOAT, MTL_RGBA32_UINT, MTL_RGBA32_SINT);
         return 2;
      }
      format[color_count] = v;
      fmt_is_float[color_count] = v == MTL_RGBA32_FLOAT;
      color_count++;
      const char *comma = strchr(arg, ',');
      arg = comma ? comma + 1 : NULL;
   }

   /* softpipe, for the reason the compute path sets it: this machine has no Adreno and
    * the library does not fall back by itself, so RWA_SOFTWARE=0 in the environment
    * still shows what a driver on a phone without gpu=1 is told.
    */
   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: device is \"%s\"\n", rwa_device_name(device));

   struct rwa_render_shape shape = { .color_count = color_count, .sample_count = 1 };
   for (unsigned i = 0; i < color_count; i++)
      shape.color_format[i] = format[i];

   struct rwa_render_info info = { 0 };
   struct rwa_render *render =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, bitcode, bitcode_size,
                        func, &shape, &info, why, sizeof(why));
   if (!render) {
      printf("air_metal_test: rwa_render_create: %s\n", why);
      rwa_device_close(device);
      return 1;
   }
   printf("air_metal_test: pipeline: %u vertex buffers, %u fragment buffers, %u fragment "
          "textures, %u fragment samplers, vertex stage %s\n", info.vertex_buffers,
          info.fragment_buffers, info.fragment_textures, info.fragment_samplers,
          info.generated_vertex_stage ? "generated" : "supplied");

   /* One colour attachment per format: a 4x4 2D texture, made a render target and also
    * readable so the pixel can be fetched back. 4x4 is air_run's WIDTH x HEIGHT, so the
    * same grid of fragments.
    */
   const unsigned origin[3] = { 0, 0, 0 };
   struct rwa_texture *color[COUNT_OF(format)] = { 0 };
   for (unsigned i = 0; i < color_count; i++) {
      const struct rwa_texture_shape ts = {
         .pixel_format = format[i], .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1,
         .array_length = 1, .mip_levels = 1, .sample_count = 1,
         .usage = MTL_USAGE_SHADER_READ | 4 /* MTLTextureUsageRenderTarget */,
      };
      color[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!color[i]) {
         printf("air_metal_test: rwa_texture_create (attachment %u): %s\n", i, why);
         return 1;
      }
   }
   printf("air_metal_test: %u colour attachment%s of 4x4\n", color_count,
          color_count == 1 ? "" : "s");

   /* One constant buffer per binding the pipeline says the fragment stage reads, each 48
    * bytes and filled with (i * 7 + 13) & 0xff -- the size and pattern air_run gives
    * clearFragment20's buffer at index 14 (air_run.c: "constant buffer at index 14, 48
    * bytes"). The pattern is position-based, so every index holds the same bytes and the
    * one the shader actually reads, 14, holds what air_run read.
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
      uint8_t *bits = rwa_buffer_map(buffers[i]);
      for (unsigned k = 0; k < rwa_buffer_size(buffers[i]); k++)
         bits[k] = pattern_byte(k);
   }
   printf("air_metal_test: %u constant buffer%s of 48 bytes of (i * 7 + 13) & 0xff\n", nbuf,
          nbuf == 1 ? "" : "s");

   /* And one per binding the VERTEX stage reads, which is nothing at all for the
    * generated stage (it reads an attribute, which the library feeds itself) and the
    * quad for a supplied one.
    *
    * The count is the highest index plus one, as Metal counts bindings: clearVertex
    * reads [[buffer(14)]] and so asks for fifteen. All fifteen are filled, and with the
    * same bytes, on the fragment buffers' argument -- the contract does not say which
    * index the shader reads, so whichever one it reads holds the quad. 64 bytes is four
    * float4s, the largest of the three element strides below.
    */
   struct rwa_buffer *vbuffers[16] = { 0 };
   unsigned vcomps[COUNT_OF(vbuffers)] = { 0 };
   float vscale = 1.0f;
   unsigned nvbuf = info.vertex_buffers < COUNT_OF(vbuffers)
                       ? info.vertex_buffers : (unsigned)COUNT_OF(vbuffers);
   parse_vertex_spec(vtxspec, vcomps, COUNT_OF(vbuffers), &vscale);
   for (unsigned i = 0; i < nvbuf; i++) {
      vbuffers[i] = rwa_buffer_create(device, sizeof(draw_quad_pos), why, sizeof(why));
      if (!vbuffers[i]) {
         printf("air_metal_test: rwa_buffer_create (vertex buffer %u): %s\n", i, why);
         return 1;
      }
      memset(rwa_buffer_map(vbuffers[i]), 0, rwa_buffer_size(vbuffers[i]));
      fill_vertex_buffer(rwa_buffer_map(vbuffers[i]), vcomps[i], vscale);
   }
   /* One line for all the position slots and one per coordinate slot: clearVertex asks
    * for fifteen buffers and fifteen identical lines said nothing fourteen did not.
    */
   if (nvbuf) {
      unsigned npos = 0;
      for (unsigned i = 0; i < nvbuf; i++)
         npos += vcomps[i] == 0;
      if (npos)
         printf("air_metal_test: %u vertex buffer%s hold%s the four clip-space corners of "
                "the quad, one float4 each, scaled by %g\n", npos, npos == 1 ? "" : "s",
                npos == 1 ? "s" : "", (double)vscale);
      for (unsigned i = 0; i < nvbuf; i++)
         if (vcomps[i])
            printf("air_metal_test: vertex buffer %u holds a float%u texture coordinate "
                   "per corner\n", i, vcomps[i]);
   }

   /* One texture per slot the pipeline says the fragment stage samples, with air_run's
    * texel pattern in it, so the pixel that comes back names the texel it was sampled
    * from: (100+x, 200+y, 300+x+y+z, 400) can be read by eye, and that is the whole
    * point here -- it is how a coordinate interpolated by a real vertex stage is told
    * apart from the one constant coordinate the generated stage hands every fragment.
    *
    * The format and type come from the last argument, defaulting to attachment 0's
    * format as a 2D texture, which is what texturedFragment2DUInt wants. A wrong pick is
    * refused by the library naming what the shader declared, as the compute path's are.
    */
   struct rwa_texture *textures[16] = { 0 };
   unsigned tex_format[COUNT_OF(textures)], tex_type[COUNT_OF(textures)];
   bool tex_is_float[COUNT_OF(textures)];
   unsigned ntex = info.fragment_textures < COUNT_OF(textures) ? info.fragment_textures
                                                               : (unsigned)COUNT_OF(textures);
   arg = texspec;
   for (unsigned i = 0; i < ntex; i++) {
      const char *colon = arg ? strchr(arg, ':') : NULL;
      const char *comma = arg ? strchr(arg, ',') : NULL;
      tex_type[i] = colon && (!comma || colon < comma)
                       ? (unsigned)strtoul(colon + 1, NULL, 0) : MTL_TEXTURE_TYPE_2D;
      tex_format[i] = arg ? (unsigned)strtoul(arg, NULL, 0) : format[0];
      /* The packed 16-bit formats are not written with the texel pattern -- their
       * channels are five and six bits wide and 100 does not fit in one -- so they are
       * accepted only with a probe word, which fills every texel and is the whole point
       * of asking for one: see probe_fill16.
       */
      bool packed16 = tex_format[i] >= 40 && tex_format[i] <= 43;
      if (packed16 && !probe_fill16_given) {
         fprintf(stderr, "air_metal_test: texture pixel format %u is one of Metal's packed "
                         "16-bit formats, whose channels are too narrow for this program's "
                         "texel pattern -- 100 does not fit in five bits. "
                         "--pixel-formats is the mode that fills them, one 16-bit word "
                         "per texel\n", tex_format[i]);
         return 2;
      }
      if (!packed16 && tex_format[i] != MTL_RGBA32_FLOAT &&
          tex_format[i] != MTL_RGBA32_UINT && tex_format[i] != MTL_RGBA32_SINT) {
         fprintf(stderr, "air_metal_test: texture pixel format %u: this program writes "
                         "known texels in MTLPixelFormatRGBA32Float (%u), RGBA32Uint (%u) "
                         "or RGBA32Sint (%u)\n", tex_format[i], MTL_RGBA32_FLOAT,
                 MTL_RGBA32_UINT, MTL_RGBA32_SINT);
         return 2;
      }
      if (tex_type[i] != MTL_TEXTURE_TYPE_2D && tex_type[i] != MTL_TEXTURE_TYPE_3D) {
         fprintf(stderr, "air_metal_test: texture type %u: this program makes "
                         "MTLTextureType2D (%u) and MTLTextureType3D (%u)\n", tex_type[i],
                 MTL_TEXTURE_TYPE_2D, MTL_TEXTURE_TYPE_3D);
         return 2;
      }
      tex_is_float[i] = tex_format[i] == MTL_RGBA32_FLOAT;
      if (comma)
         arg = comma + 1;      /* otherwise the last one stands for the rest */

      unsigned depth = tex_type[i] == MTL_TEXTURE_TYPE_3D ? TEX : 1;
      const struct rwa_texture_shape ts = {
         .pixel_format = tex_format[i], .type = tex_type[i],
         .width = TEX, .height = TEX, .depth = depth,
         .array_length = 1, .mip_levels = 1, .sample_count = 1,
         /* read only: a fragment shader that sampled and stored would want
          * MTLTextureUsageShaderWrite as well, and none of Apple's does
          */
         .usage = MTL_USAGE_SHADER_READ,
      };
      textures[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!textures[i]) {
         printf("air_metal_test: rwa_texture_create (texture %u): %s\n", i, why);
         return 1;
      }
      static union { float f[TEXELS]; int32_t i[TEXELS]; uint16_t h[TEXELS * 2]; } written;
      memset(&written, 0, sizeof(written));
      const unsigned extent[3] = { TEX, TEX, depth };
      if (packed16) {
         /* One 16-bit word in every texel, the caller's. Every texel the same on
          * purpose: this measurement is about which FIELD of the word became which
          * channel, so a coordinate must not be able to change the answer.
          */
         for (unsigned k = 0; k < TEX * TEX * depth; k++)
            written.h[k] = (uint16_t)probe_fill16;
         if (rwa_texture_write(textures[i], 0, 0, origin, extent, &written, 0, why,
                               sizeof(why)) != 0) {
            printf("air_metal_test: rwa_texture_write (texture %u): %s\n", i, why);
            return 1;
         }
         printf("air_metal_test: texture %u is %ux%u of pixel format %u, every texel the "
                "16-bit word 0x%04x\n", i, TEX, TEX, tex_format[i],
                (unsigned)(probe_fill16 & 0xffff));
      } else {
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX; y++)
            for (unsigned x = 0; x < TEX; x++) {
               float v[4];
               texel_value(i, x, y, z, v);
               for (unsigned c = 0; c < 4; c++) {
                  unsigned k = texel_index(x, y, z) + c;
                  if (tex_is_float[i])
                     written.f[k] = v[c];
                  else
                     written.i[k] = (int32_t)v[c];
               }
            }
      if (rwa_texture_write(textures[i], 0, 0, origin, extent, &written, 0, why,
                            sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_write (texture %u): %s\n", i, why);
         return 1;
      }
      printf("air_metal_test: texture %u is %ux%ux%u of pixel format %u, texel (x,y,z) = "
             "(100+x+%u, 200+y, 300+x+y+z, 400)\n", i, TEX, TEX, depth, tex_format[i],
             1000 * i);
      }
   }

   /* And a sampler per slot the API is expected to fill. Nearest and clamped as in the
    * compute path, but in NORMALISED coordinates and not pixels, which is the one place
    * the two paths differ and it is not arbitrary: a kernel computes its own integer
    * texel, while these fragment shaders are handed a coordinate by the vertex stage,
    * and the coordinate AppleMetalGLRenderer's vertex stage carries is a GL texture
    * coordinate in [0, 1]. Nearest, so the texel the coordinate falls in is the texel
    * that comes back with no weights to round.
    */
   struct rwa_sampler *samplers[16] = { 0 };
   unsigned nsamp = info.fragment_samplers < COUNT_OF(samplers) ? info.fragment_samplers
                                                                : (unsigned)COUNT_OF(samplers);
   for (unsigned i = 0; i < nsamp; i++) {
      const struct rwa_sampler_state state = {
         .address = { 0, 0, 0 },           /* MTLSamplerAddressModeClampToEdge */
         .min_filter = 0, .mag_filter = 0, /* MTLSamplerMinMagFilterNearest */
         .mip_filter = 0,                  /* MTLSamplerMipFilterNotMipmapped */
         .compare_function = 0,            /* MTLCompareFunctionNever, nothing reads it */
         .max_anisotropy = 1,
         .normalized_coordinates = 1,
         .lod_min = 0.0f, .lod_max = 65504.0f,
      };
      samplers[i] = rwa_sampler_create(device, &state, why, sizeof(why));
      if (!samplers[i]) {
         printf("air_metal_test: rwa_sampler_create (sampler %u): %s\n", i, why);
         return 1;
      }
   }
   if (nsamp)
      printf("air_metal_test: %u sampler%s from the API: nearest, clamp to edge, "
             "normalised\n", nsamp, nsamp == 1 ? "" : "s");

   const struct rwa_draw draw = {
      .color = color, .color_count = color_count,
      .clear_color = (const float[]){ 0, 0, 0, 0 },
      .vertex = { .buffers = vbuffers, .buffer_count = nvbuf },
      .fragment = { .buffers = buffers, .buffer_count = nbuf,
                    .textures = textures, .texture_count = ntex,
                    .samplers = samplers, .sampler_count = nsamp },
      .primitive = 4,                       /* MTLPrimitiveTypeTriangleStrip */
      .vertex_start = 0, .vertex_count = 4, /* the four corners, generated or supplied */
   };
   if (rwa_render_draw(render, &draw, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_render_draw: %s\n", why);
      rwa_render_destroy(render);
      for (unsigned i = 0; i < nbuf; i++)
         rwa_buffer_destroy(buffers[i]);
      for (unsigned i = 0; i < nvbuf; i++)
         rwa_buffer_destroy(vbuffers[i]);
      for (unsigned i = 0; i < ntex; i++)
         rwa_texture_destroy(textures[i]);
      for (unsigned i = 0; i < nsamp; i++)
         rwa_sampler_destroy(samplers[i]);
      for (unsigned i = 0; i < color_count; i++)
         rwa_texture_destroy(color[i]);
      rwa_device_close(device);
      return 1;
   }
   printf("air_metal_test: drew a %s of %u vertices into %u attachment%s\n",
          "MTLPrimitiveTypeTriangleStrip", draw.vertex_count, color_count,
          color_count == 1 ? "" : "s");

   /* The pixel at (0, 0) of the last attachment, which for clearFragment20 is render
    * target 2 -- the one it writes. Read as a single texel of its own, the way the
    * contract reads a texture back.
    *
    * And then the opposite corner, which is not decoration. A fragment shader that
    * samples what the vertex stage interpolated came back at (0, 0) with texel (0, 0) --
    * and so would a shader whose varying had been dropped and read as zero, since a
    * coordinate of (0, 0) lands in texel (0, 0) too. The far corner tells those apart:
    * an interpolated coordinate reaches texel (3, 3) there and a dropped one is still
    * (0, 0). The second number is the one that says the varying carries anything.
    */
   unsigned read_rt = color_count - 1;
   const unsigned one[3] = { 1, 1, 1 };
   const unsigned corner[2][3] = { { 0, 0, 0 }, { TEX - 1, TEX - 1, 0 } };
   for (unsigned k = 0; k < 2; k++) {
      union { uint32_t u[4]; float f[4]; } pixel;
      memset(&pixel, 0, sizeof(pixel));
      if (rwa_texture_read(color[read_rt], 0, 0, corner[k], one, &pixel, 0, why,
                           sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_read (attachment %u): %s\n", read_rt, why);
         return 1;
      }
      printf("air_metal_test: the pixel at (%u, %u) of attachment %u came back as:\n",
             corner[k][0], corner[k][1], read_rt);
      for (unsigned c = 0; c < 4; c++) {
         if (fmt_is_float[read_rt])
            printf("  %.9g\n", (double)pixel.f[c]);
         else
            printf("  %u\n", pixel.u[c]);
      }
      /* and kept, so a caller in this process can judge it rather than parse its own
       * output: --pixel-formats runs this function sixteen times and compares
       */
      if (k == 0 && fmt_is_float[read_rt])
         for (unsigned c = 0; c < 4; c++)
            last_pixel[c] = pixel.f[c];
      /* A packed 16-bit attachment comes back as its WORD: rwa_texture_read wrote two
       * bytes into the union above, and those two bytes ARE the texel.
       */
      if (k == 0 && format[read_rt] >= 40 && format[read_rt] <= 43) {
         last_word = pixel.u[0] & 0xffff;
         printf("air_metal_test: ... which as a packed 16-bit word is 0x%04x\n",
                last_word);
      }
   }

   rwa_render_destroy(render);
   for (unsigned i = 0; i < nbuf; i++)
      rwa_buffer_destroy(buffers[i]);
   for (unsigned i = 0; i < nvbuf; i++)
      rwa_buffer_destroy(vbuffers[i]);
   for (unsigned i = 0; i < ntex; i++)
      rwa_texture_destroy(textures[i]);
   for (unsigned i = 0; i < nsamp; i++)
      rwa_sampler_destroy(samplers[i]);
   for (unsigned i = 0; i < color_count; i++)
      rwa_texture_destroy(color[i]);
   rwa_device_close(device);
   return 0;
}

/*
 * --draw-depth: whether a depth test actually discards anything, measured.
 *
 * Nothing in this port has ever drawn with a depth attachment, so "the depth path
 * works" needs a number that could have come out otherwise. Two things are measured
 * here and they are different measurements:
 *
 *   - a fragment shader that writes COLOUR, drawn three times at three depths with
 *     MTLCompareFunctionLess and depth writes on, and then once more with
 *     MTLCompareFunctionAlways. The quad is the same quad every time; what changes is
 *     the depth it is drawn at (rwa_draw.depth_range, which the generated vertex stage
 *     turns into the quad's window depth exactly) and the bytes in its constant buffer,
 *     so each draw that is NOT discarded leaves a different colour. The colour after
 *     the draw at the far depth is the whole test: unchanged means the test discarded
 *     it, and the Always control at the same depth afterwards proves the compare
 *     function is what discarded it and not the draw.
 *   - a fragment shader that writes DEPTH -- Apple's texturedFragment2DDepth and
 *     texturedFragment3DDepth, the two the corpus has -- where the depth buffer must
 *     come back holding what the shader computed and not the interpolated z. Those two
 *     sample a texture and store channel x to [[depth]], so the sampled texels are
 *     seeded in [0, 1] (depth_texel_value below, NOT air_run's pattern, whose 100.0
 *     would clamp to 1.0 and be indistinguishable from the clear value) and the answer
 *     is the one texel the generated stage's constant coordinate selects.
 *
 * Which of the two it runs is the pipeline's own answer, rwa_render_info's
 * fragment_writes_depth, and not a flag here: the test cannot be told to measure the
 * wrong thing about a shader.
 *
 *   air_metal_test --draw-depth FILE.metallib FUNCTION
 *                  [MTLPixelFormat[,...]|none [DEPTHFORMAT]]
 *
 * "none" in place of the colour formats is a depth-only pass, which Metal allows and this
 * library allows only with a depth attachment; it needs a shader whose output is the depth,
 * since there is then no colour attachment for a discard to show in.
 */

