/* Vertex attribute layout tests, included into air_metal_test.c. */


/*
 * --draw-attrib: whether the vertex attribute layout is read, and read as Metal says.
 *
 * This is the half of the contract the phone's window server asked for and did not get:
 * 120 of its render pipelines were refused because rwa_render_shape carried no formats,
 * strides or offsets (docs/metal.md 10.44). What is hard to measure about a layout is
 * not getting a picture -- a quad drawn out of a float4 at offset 0 of a 16-byte element
 * is the same quad the library's own generated stage draws, so on its own it proves
 * nothing. What makes this a measurement is that EVERY NUMBER IN THE LAYOUT IS WRONG FOR
 * THE DATA unless it is honoured:
 *
 *   - the position is NOT at offset 0 and the element is NOT the size of the position, so
 *     a fetch that ignored the offset or the stride reads a texture coordinate as a
 *     clip-space position and draws somewhere else;
 *   - the stream is at BUFFER INDEX 5, so a fetch that bound it at slot 0 reads nothing;
 *   - the second measured attribute is Metal's [[attribute(3)]] while its NIR
 *     driver_location is 1, because the module's attribute 1 is air.arg_unused -- and
 *     this mode CONFIGURES that unused attribute with a layout of its own, pointing at
 *     bytes that hold a different texture coordinate. An element array built by
 *     attribute index, or built over all three attributes in order, feeds attribute 1's
 *     layout to attribute 3's fetch, and then the picture is flat where it should be a
 *     gradient. That is the one thing in docs/air.md 3.1 that cannot be re-derived, and
 *     it is the only wrong answer here that still draws the right geometry;
 *   - the same four vertices are then drawn a second time with the SAME numbers in
 *     different formats -- UChar4Normalized and Short4Normalized where the first draw
 *     had Float4 -- at a different stride and different offsets. Those two pictures must
 *     agree to the bit, which no implementation that ignores a format can manage: 0 and
 *     255 read as floats are 0.0 and 3.57e-43.
 *
 * The pipeline is Apple's own code on both sides and from two libraries, which is a
 * legal Metal pipeline and the shape a real one has: QuartzCore's std_vert0_lpf, whose
 * position is `mvp_matrix * attribute(0)` (so an IDENTITY matrix in its [[buffer(2)]]
 * makes the position the attribute verbatim) and whose [[attribute(3)]] `color` becomes
 * the first varying, over AppleMetalGLRenderer's texturedFragment2DUInt, which samples
 * texture 0 at that varying and writes the texel to four RGBA32Uint targets. So the
 * COVERAGE of the picture measures attribute 0 and every WORD in it measures attribute
 * 3: pixel (x, y) must come back as the texel the interpolated coordinate lands in,
 * (100 + x, 200 + y, 300 + x + y, 400) up to the vertical orientation of 10.32.
 *
 * The texture coordinate is the only thing this mode puts in a buffer that it does not
 * know the meaning of: mvp_matrix is a float4x4 by Apple's own air.arg_type_name and the
 * identity is the identity in either column order, and texmat (its [[buffer(3)]]) is
 * air.arg_unused in this module and bound only because the module declares it.
 *
 *   air_metal_test --draw-attrib VERTEXLIB VERTEXFUNCTION FRAGMENTLIB FRAGMENTFUNCTION
 *                  [GATEDVERTEXFUNCTION]
 *
 * Both paths may be a .metallib or a bare module, as everywhere else in this program. The
 * fifth argument is a second vertex function of the same library whose attributes are
 * gated by function constants (QuartzCore's fixed_vert_lph_spc), for the step that
 * measures the gate; without it that step says it was not run.
 */

/* The eight elements of the stream: the four corners of the full-screen quad with a
 * texture coordinate that runs the whole [0, 1] square, and then four more at half the
 * distance from the origin whose coordinate is the far corner. Elements 4..7 exist for
 * one step only -- the one that draws the same data at twice the stride, where the
 * fetch then walks 0, 2, 4, 6 and must produce a picture nothing else here produces.
 */
static const float attrib_pos[8][4] = {
   { -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
   { -0.5f, -0.5f, 0, 1 }, { 0.5f, -0.5f, 0, 1 },
   { -0.5f, 0.5f, 0, 1 }, { 0.5f, 0.5f, 0, 1 },
};
static const float attrib_uv[8][2] = {
   { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 },
   { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 },
};

/* The DECOY: what the module's unused [[attribute(1)]] points at, in every element. A
 * coordinate at the centre of texel (0, 0), so an implementation that fed this layout to
 * the fetch of attribute 3 comes back with (100, 200, 300, 400) in all sixteen pixels --
 * one number, nameable in advance, and not a plausible gradient.
 */
static const float attrib_decoy_uv[2] = { 0.125f, 0.125f };

/* One attribute's place in the element, as MTLVertexAttributeDescriptor has it. */
struct attrib_slot {
   unsigned format;   /* MTLVertexFormat */
   unsigned offset;
   unsigned buffer;
};

/* One shape of the stream: where each of the three attributes sits and how far apart two
 * vertices are. The two that matter are `wide` and `narrow` -- the same numbers in two
 * encodings at two strides with three different offsets -- and every other step is one
 * of those two with one field changed.
 */
struct attrib_layout {
   const char *what;
   unsigned stride;
   struct attrib_slot pos, uv, decoy;
};

static int16_t
attrib_snorm16(float v)
{
   /* Metal's Short4Normalized: value / 32767, so 1.0 is 32767 exactly and the quad's
    * corners are exact. The halves are not, and nothing here compares them.
    */
   float s = v * 32767.0f;
   return (int16_t)(s < 0 ? s - 0.5f : s + 0.5f);
}

static uint8_t
attrib_unorm8(float v)
{
   return (uint8_t)(v * 255.0f + 0.5f);   /* UChar4Normalized: 255 is 1.0 exactly */
}

/* The stream, written in whatever encoding the layout names. The buffer is always eight
 * elements of the layout's own stride, so a step that reads it at twice the stride has
 * something defined to read rather than running off the end.
 */
static void
attrib_fill(uint8_t *mem, size_t size, const struct attrib_layout *l)
{
   memset(mem, 0, size);
   for (unsigned i = 0; i < 8; i++) {
      uint8_t *e = mem + (size_t)i * l->stride;

      if (l->pos.format == MTL_VF_FLOAT4) {
         float v[4] = { attrib_pos[i][0], attrib_pos[i][1], attrib_pos[i][2],
                        attrib_pos[i][3] };
         memcpy(e + l->pos.offset, v, sizeof(v));
      } else {
         int16_t v[4] = { attrib_snorm16(attrib_pos[i][0]),
                          attrib_snorm16(attrib_pos[i][1]),
                          attrib_snorm16(attrib_pos[i][2]),
                          attrib_snorm16(attrib_pos[i][3]) };
         memcpy(e + l->pos.offset, v, sizeof(v));
      }
      if (l->uv.format == MTL_VF_FLOAT4) {
         float v[4] = { attrib_uv[i][0], attrib_uv[i][1], 0.0f, 1.0f };
         memcpy(e + l->uv.offset, v, sizeof(v));
      } else if (l->uv.format == MTL_VF_UCHAR4_NORMALIZED_BGRA) {
         /* The one place here where the bytes are NOT in the order the components are:
          * MTLVertexFormatUChar4Normalized_BGRA delivers (x, y, z, w) as
          * (byte2, byte1, byte0, byte3), so the coordinate goes in bytes 2 and 1 and byte 0
          * is ZERO -- read in memory order the shader's x would be that zero, and every
          * pixel would sample column 0 of the texture.
          */
         uint8_t v[4] = { 0, attrib_unorm8(attrib_uv[i][1]),
                          attrib_unorm8(attrib_uv[i][0]), 255 };
         memcpy(e + l->uv.offset, v, sizeof(v));
      } else {
         uint8_t v[4] = { attrib_unorm8(attrib_uv[i][0]), attrib_unorm8(attrib_uv[i][1]),
                          0, 255 };
         memcpy(e + l->uv.offset, v, sizeof(v));
      }
      if (l->decoy.format == MTL_VF_FLOAT2) {
         float v[2] = { attrib_decoy_uv[0], attrib_decoy_uv[1] };
         memcpy(e + l->decoy.offset, v, sizeof(v));
      } else {
         /* 0.125 as a half, written out because this program includes air_metal.h and
          * nothing of Mesa: 2^-3 is exponent field 15 - 3 = 12 with a zero mantissa,
          * which is 0x3000.
          */
         uint16_t v[2] = { 0x3000, 0x3000 };
         memcpy(e + l->decoy.offset, v, sizeof(v));
      }
   }
}

/* The per-instance stream of the instancing steps: four texture coordinates, one per
 * element, in the two corners and the two opposite ones. A stream that steps per
 * instance hands ALL FOUR VERTICES of one instance the same element, so the varying is
 * constant over the quad and every pixel samples one texel; a stream that stepped per
 * vertex instead would hand the four vertices four different coordinates and the picture
 * would be a gradient. One flat number against a gradient is the whole measurement.
 */
static const float attrib_instance_uv[4][2] = {
   { 0.125f, 0.125f }, { 0.875f, 0.875f }, { 0.125f, 0.875f }, { 0.875f, 0.125f },
};

/* How the shape's vertex descriptor is filled in from a layout: Metal's own two arrays,
 * indexed by Metal's own indices. Attribute 1 is configured although the module marks it
 * air.arg_unused, which is legal and is this mode's decoy.
 */
static void
attrib_configure(struct rwa_render_shape *shape, const struct attrib_layout *l,
                 unsigned step_function, unsigned step_rate, unsigned instance_buffer)
{
   memset(shape->vertex_attribute, 0, sizeof(shape->vertex_attribute));
   memset(shape->vertex_layout, 0, sizeof(shape->vertex_layout));

   shape->vertex_attribute[0] = (struct rwa_vertex_attribute){
      .format = l->pos.format, .offset = l->pos.offset, .buffer_index = l->pos.buffer };
   shape->vertex_attribute[1] = (struct rwa_vertex_attribute){
      .format = l->decoy.format, .offset = l->decoy.offset,
      .buffer_index = l->decoy.buffer };
   shape->vertex_attribute[3] = (struct rwa_vertex_attribute){
      .format = l->uv.format, .offset = l->uv.offset, .buffer_index = l->uv.buffer };

   shape->vertex_layout[l->pos.buffer] = (struct rwa_vertex_layout){
      .stride = l->stride, .step_function = MTL_STEP_PER_VERTEX, .step_rate = 1 };
   if (instance_buffer != ~0u) {
      shape->vertex_layout[instance_buffer] = (struct rwa_vertex_layout){
         .stride = 8, .step_function = step_function, .step_rate = step_rate };
   }
}

/* The four words of one pixel, and what the texel the coordinate should have landed in
 * holds. 10.32 established that the picture comes back mirrored vertically against a
 * naive reading of the coordinate, so BOTH orientations are counted and the one that
 * matched is printed rather than one of them being assumed: what this mode is about is
 * which attribute fed the fetch, and that question has the same answer either way.
 */
struct attrib_verdict {
   unsigned x_gradient;    /* pixels whose first word is 100 + x */
   unsigned mirrored;      /* ... and whose second is 200 + (3 - y) */
   unsigned upright;       /* ... or 200 + y */
   unsigned decoy_texel;   /* pixels holding texel (0, 0) exactly: the decoy's answer */
   unsigned column0;       /* pixels holding texel (0, y): what a dropped BGRA swap gives */
   unsigned uniform;       /* pixels equal to the pixel at (0, 0) */
};

static struct attrib_verdict
attrib_judge(const struct index_picture *p)
{
   struct attrib_verdict v = { 0 };
   float t[4];

   texel_value(0, 0, 0, 0, t);
   for (unsigned y = 0; y < TEX; y++)
      for (unsigned x = 0; x < TEX; x++) {
         const uint32_t *w = p->word[y * TEX + x];

         v.x_gradient += w[0] == 100 + x;
         v.mirrored += w[0] == 100 + x && w[1] == 200 + (TEX - 1 - y);
         v.upright += w[0] == 100 + x && w[1] == 200 + y;
         v.decoy_texel += w[0] == (uint32_t)t[0] && w[1] == (uint32_t)t[1] &&
                          w[2] == (uint32_t)t[2] && w[3] == (uint32_t)t[3];
         v.column0 += w[0] == 100;
         v.uniform += w[0] == p->word[0][0] && w[1] == p->word[0][1] &&
                      w[2] == p->word[0][2] && w[3] == p->word[0][3];
      }
   return v;
}

static void
attrib_print(const struct index_picture *p, const struct attrib_verdict *v)
{
   index_print(p);
   printf("air_metal_test:   the pixel at (0,0) is %u %u %u %u and at (3,3) %u %u %u %u; "
          "%u of 16 pixels hold texel x, %u hold (x, 3-y), %u hold (x, y), %u hold texel "
          "(0,0), %u hold column 0, %u are the same as (0,0)\n",
          p->word[0][0], p->word[0][1], p->word[0][2], p->word[0][3],
          p->word[TEX * TEX - 1][0], p->word[TEX * TEX - 1][1],
          p->word[TEX * TEX - 1][2], p->word[TEX * TEX - 1][3],
          v->x_gradient, v->mirrored, v->upright, v->decoy_texel, v->column0,
          v->uniform);
}

static int
run_attrib(const char *vpath, const char *vfunc, const char *fpath, const char *ffunc,
           const char *gated)
{
   struct metallib vlib = { 0 }, flib = { 0 };
   char vname[256], fname[256];
   const uint8_t *vbitcode = NULL, *fbitcode = NULL;
   size_t vbitcode_size = 0, fbitcode_size = 0;

   if (!load_metallib(vpath, &vlib) || !load_metallib(fpath, &flib))
      return 1;
   if (!find_function(&vlib, vfunc, vname, sizeof(vname), &vbitcode, &vbitcode_size))
      return 1;
   if (!find_function(&flib, ffunc, fname, sizeof(fname), &fbitcode, &fbitcode_size))
      return 1;

   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: --draw-attrib %s over %s on \"%s\"\n", vname, fname,
          rwa_device_name(device));

   /* Four RGBA32Uint attachments, because texturedFragment2DUInt writes four, and 4x4
    * as everywhere else in this program: a pixel here and a pixel in air_run are the
    * same measurement.
    */
   const unsigned color_count = 4;
   struct rwa_texture *color[4] = { 0 };
   for (unsigned i = 0; i < color_count; i++) {
      const struct rwa_texture_shape ts = {
         .pixel_format = MTL_RGBA32_UINT, .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1,
         .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
      };
      color[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!color[i]) {
         printf("air_metal_test: rwa_texture_create (attachment %u): %s\n", i, why);
         return 1;
      }
   }

   /* The two shapes of the stream. Everything about them differs except the numbers they
    * encode: the stride, all three offsets and all three formats.
    */
   static const struct attrib_layout wide = {
      "Float4 at offset 16 of a 48-byte element",
      48,
      { MTL_VF_FLOAT4, 16, 5 },   /* the position */
      { MTL_VF_FLOAT4, 0, 5 },    /* attribute 3, the texture coordinate */
      { MTL_VF_FLOAT2, 32, 5 },   /* attribute 1, unused by the module: the decoy */
   };
   static const struct attrib_layout narrow = {
      "Short4Normalized at offset 4 of a 16-byte element",
      16,
      { MTL_VF_SHORT4_NORMALIZED, 4, 5 },
      { MTL_VF_UCHAR4_NORMALIZED, 0, 5 },
      { MTL_VF_HALF2, 12, 5 },
   };

   /* One buffer per shape, eight elements each, and one more for the instanced steps. */
   struct rwa_buffer *wide_stream = rwa_buffer_create(device, 8 * wide.stride, why,
                                                     sizeof(why));
   struct rwa_buffer *narrow_stream = rwa_buffer_create(device, 8 * narrow.stride, why,
                                                        sizeof(why));
   struct rwa_buffer *instance_stream = rwa_buffer_create(device,
                                                          sizeof(attrib_instance_uv), why,
                                                          sizeof(why));
   if (!wide_stream || !narrow_stream || !instance_stream) {
      printf("air_metal_test: rwa_buffer_create (a vertex stream): %s\n", why);
      return 1;
   }
   attrib_fill(rwa_buffer_map(wide_stream), rwa_buffer_size(wide_stream), &wide);
   attrib_fill(rwa_buffer_map(narrow_stream), rwa_buffer_size(narrow_stream), &narrow);
   memcpy(rwa_buffer_map(instance_stream), attrib_instance_uv,
          sizeof(attrib_instance_uv));
   printf("air_metal_test: the stream is 8 elements: four corners of the quad with the "
          "texture coordinate running the whole [0,1] square, then four at half the "
          "distance with the coordinate (1,1). The unused attribute 1 points at "
          "(%.3f, %.3f), the centre of texel (0,0)\n",
          (double)attrib_decoy_uv[0], (double)attrib_decoy_uv[1]);

   /* The index buffer of the indexed steps: the strip of four, then its last three, as
    * run_index's own permutation -- the difference being that here an index selects the
    * ELEMENT THE FETCH READS and not a vertex id the shader indexes a buffer with.
    */
   static const uint16_t attrib_indices[7] = { 0, 1, 2, 3, 1, 2, 3 };
   struct rwa_buffer *indices = rwa_buffer_create(device, sizeof(attrib_indices), why,
                                                  sizeof(why));
   if (!indices) {
      printf("air_metal_test: rwa_buffer_create (the index buffer): %s\n", why);
      return 1;
   }
   memcpy(rwa_buffer_map(indices), attrib_indices, sizeof(attrib_indices));

   struct rwa_render_shape shape = { .color_count = color_count, .sample_count = 1 };
   for (unsigned i = 0; i < color_count; i++)
      shape.color_format[i] = MTL_RGBA32_UINT;

   /* The first pipeline, and the first thing it says is what the layout became. */
   struct rwa_render_shape wide_shape = shape;
   attrib_configure(&wide_shape, &wide, MTL_STEP_PER_VERTEX, 1, ~0u);

   struct rwa_render_info info = { 0 };
   struct rwa_render *wide_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &wide_shape, &info, why, sizeof(why));
   if (!wide_pipe) {
      printf("air_metal_test: rwa_render_create: %s\n", why);
      return 1;
   }
   printf("air_metal_test: pipeline: %u vertex buffers, %u fragment textures, %u "
          "samplers; %u vertex elements, %u stream slots, mask 0x%x; elements are",
          info.vertex_buffers, info.fragment_textures, info.fragment_samplers,
          info.vertex_attribute_count, info.vertex_streams, info.vertex_stream_mask);
   for (unsigned i = 0; i < info.vertex_attribute_count; i++)
      printf(" [%u]=attribute(%u)", i, info.vertex_attribute_index[i]);
   printf("\n");

   /* This mode's arithmetic is this module's: two fetched attributes, Metal's 0 and 3,
    * and a third configured attribute the module does not read. A module of another
    * shape is refused here rather than measured against the wrong expectation.
    */
   if (info.vertex_attribute_count != 2 || info.vertex_attribute_index[0] != 0 ||
       info.vertex_attribute_index[1] != 3) {
      printf("air_metal_test: this mode needs a vertex function of std_vert0_lpf's shape "
             "-- [[attribute(0)]] into the position, an unused [[attribute(1)]], and "
             "[[attribute(3)]] into the first varying -- and %s fetches %u attribute(s). "
             "Nothing below would be comparable with the expectations above\n", vname,
             info.vertex_attribute_count);
      return 2;
   }
   if (info.vertex_streams != 6 || info.vertex_stream_mask != (1u << 5)) {
      printf("air_metal_test: the layout puts both fetched attributes in buffer index 5, "
             "so six slots with only the sixth in the mask is the answer; this says %u "
             "and 0x%x\n", info.vertex_streams, info.vertex_stream_mask);
      return 2;
   }
   if (info.fragment_textures != 1 || info.fragment_samplers != 1) {
      printf("air_metal_test: this mode needs a fragment function that samples one "
             "texture with the first varying, as texturedFragment2DUInt does, and %s "
             "samples %u\n", fname, info.fragment_textures);
      return 2;
   }

   /* The vertex stage's own buffers: mvp_matrix at [[buffer(2)]] gets the IDENTITY, which
    * is what makes the position the attribute verbatim and the picture computable;
    * texmat at [[buffer(3)]] is air.arg_unused in this module and is bound because the
    * module declares it. Both are 64 bytes, the size Apple's metadata gives the matrix.
    */
   struct rwa_buffer *vbuffers[16] = { 0 };
   unsigned nvbuf = info.vertex_buffers < COUNT_OF(vbuffers) ? info.vertex_buffers
                                                             : (unsigned)COUNT_OF(vbuffers);
   for (unsigned i = 0; i < nvbuf; i++) {
      vbuffers[i] = rwa_buffer_create(device, 64, why, sizeof(why));
      if (!vbuffers[i]) {
         printf("air_metal_test: rwa_buffer_create (vertex buffer %u): %s\n", i, why);
         return 1;
      }
      float *f = rwa_buffer_map(vbuffers[i]);
      memset(f, 0, 64);
      for (unsigned k = 0; k < 4; k++)
         f[k * 4 + k] = 1.0f;         /* the identity, in either column order */
   }

   /* The texture and the sampler, air_run's texels and air_run's sampler state: nearest,
    * so the texel the coordinate falls in is the texel that comes back and there are no
    * weights to round.
    */
   struct rwa_texture *tex = NULL;
   {
      const struct rwa_texture_shape ts = {
         .pixel_format = MTL_RGBA32_UINT, .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1, .usage = MTL_USAGE_SHADER_READ,
      };
      tex = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!tex) {
         printf("air_metal_test: rwa_texture_create (the sampled texture): %s\n", why);
         return 1;
      }
      static int32_t texels[TEXELS];
      memset(texels, 0, sizeof(texels));
      for (unsigned y = 0; y < TEX; y++)
         for (unsigned x = 0; x < TEX; x++) {
            float v[4];
            texel_value(0, x, y, 0, v);
            for (unsigned c = 0; c < 4; c++)
               texels[texel_index(x, y, 0) + c] = (int32_t)v[c];
         }
      const unsigned origin[3] = { 0, 0, 0 }, extent[3] = { TEX, TEX, 1 };
      if (rwa_texture_write(tex, 0, 0, origin, extent, texels, 0, why, sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_write: %s\n", why);
         return 1;
      }
   }
   const struct rwa_sampler_state sstate = {
      .address = { 0, 0, 0 }, .min_filter = 0, .mag_filter = 0, .mip_filter = 0,
      .compare_function = 0, .max_anisotropy = 1, .normalized_coordinates = 1,
      .lod_min = 0.0f, .lod_max = 65504.0f,
   };
   struct rwa_sampler *sampler = rwa_sampler_create(device, &sstate, why, sizeof(why));
   if (!sampler) {
      printf("air_metal_test: rwa_sampler_create: %s\n", why);
      return 1;
   }

   struct rwa_texture *textures[1] = { tex };
   struct rwa_sampler *samplers[1] = { sampler };
   struct rwa_vertex_stream streams[8] = { 0 };
   const struct rwa_draw base = {
      .color = color, .color_count = color_count,
      .vertex = { .buffers = vbuffers, .buffer_count = nvbuf },
      .fragment = { .textures = textures, .texture_count = 1,
                    .samplers = samplers, .sampler_count = 1 },
      .primitive = 4,                /* MTLPrimitiveTypeTriangleStrip */
      .vertex_streams = streams, .vertex_stream_count = COUNT_OF(streams),
   };

   bool ok = true;
   struct index_picture pw, pn, pstride, pindex7, pfull, phalf, pinst, prate;
   struct rwa_draw d;

   /* 1. The wide layout. If the offsets, the stride and the buffer index are honoured,
    *    the quad covers the attachment and the coordinate runs the square: every pixel
    *    must hold the texel under it. If attribute 1's layout had been fed to attribute
    *    3's fetch instead -- which is what an element array built by attribute index
    *    gives -- all sixteen pixels would hold texel (0,0), (100, 200, 300, 400).
    */
   streams[5] = (struct rwa_vertex_stream){ .buffer = wide_stream, .offset = 0 };
   d = base;
   d.vertex_count = 4;
   if (!index_draw(wide_pipe, &d, color, color_count, &pw, wide.what))
      return 1;
   struct attrib_verdict vw = attrib_judge(&pw);
   printf("air_metal_test: 1. %s, stream at buffer index 5\n", wide.what);
   attrib_print(&pw, &vw);
   if (pw.covered != TEX * TEX || vw.x_gradient != TEX * TEX ||
       (vw.mirrored != TEX * TEX && vw.upright != TEX * TEX)) {
      printf("air_metal_test:   NOT the picture the layout describes%s\n",
             vw.decoy_texel == TEX * TEX
                ? ": every pixel holds texel (0,0), which is where the UNUSED "
                  "attribute 1 points -- the elements were not built in "
                  "driver_location order"
                : "");
      ok = false;
   } else {
      printf("air_metal_test:   the coordinate of [[attribute(3)]] reached every pixel "
             "and the quad of [[attribute(0)]] covered all 16: the offsets, the stride "
             "and buffer index 5 are READ, and element 1 is attribute 3 and not the "
             "unused attribute 1 (%s orientation, 10.32)\n",
             vw.mirrored == TEX * TEX ? "mirrored" : "upright");
   }

   /* 2. The same numbers in other formats at another stride and other offsets. Equal to
    *    the bit, or the formats are not what decoded the bytes: 0 and 255 read as a
    *    float are 0.0 and 3.57e-43, and a Short4Normalized position read as Float4 is
    *    four denormals -- a quad collapsed into the origin, covering nothing.
    */
   struct rwa_render_shape narrow_shape = shape;
   attrib_configure(&narrow_shape, &narrow, MTL_STEP_PER_VERTEX, 1, ~0u);
   struct rwa_render_info ninfo = { 0 };
   struct rwa_render *narrow_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &narrow_shape, &ninfo, why, sizeof(why));
   if (!narrow_pipe) {
      printf("air_metal_test: rwa_render_create (%s): %s\n", narrow.what, why);
      return 1;
   }
   streams[5] = (struct rwa_vertex_stream){ .buffer = narrow_stream, .offset = 0 };
   d = base;
   d.vertex_count = 4;
   if (!index_draw(narrow_pipe, &d, color, color_count, &pn, narrow.what))
      return 1;
   printf("air_metal_test: 2. %s, the same four vertices\n", narrow.what);
   {
      struct attrib_verdict v = attrib_judge(&pn);
      attrib_print(&pn, &v);
      unsigned diff = index_picture_diff(&pw, &pn);
      printf("air_metal_test:   %u of %u words differ from step 1: %s\n", diff,
             TEX * TEX * 4,
             diff == 0 ? "two encodings, two strides, three different offsets, ONE "
                         "PICTURE -- the formats decoded the bytes"
                       : "the same numbers in another format came back as another "
                         "picture");
      if (diff)
         ok = false;
   }

   /*
    * 2a. THE BGRA ORDER, which is a translation here and a hardware field on the phone:
    *     MTLVertexFormatUChar4Normalized_BGRA becomes PIPE_FORMAT_B8G8R8A8_UNORM, and
    *     nothing of the byte order is in the fetch's format -- freedreno puts it in
    *     VFD_DECODE_INSTR.swap, which fd6_vertex_swap() reads off the pipe_format, while
    *     softpipe's own fetch reads the format description. So what this step settles is
    *     that the TRANSLATION is right, and it is the reference the same step on the device
    *     is compared with (metal_drv_test --draw-attrib, step 3).
    *
    *     The coordinate is stored with its first byte ZERO and the two components in bytes
    *     2 and 1, which the BGRA order delivers as x and y. Read in memory order instead,
    *     the shader's x is that zero and every pixel samples column 0 of the texture.
    */
   struct attrib_layout bgra = narrow;
   bgra.what = "the coordinate as UChar4Normalized_BGRA";
   bgra.uv.format = MTL_VF_UCHAR4_NORMALIZED_BGRA;
   struct rwa_buffer *bgra_stream = rwa_buffer_create(device, 8 * bgra.stride, why,
                                                      sizeof(why));
   if (!bgra_stream) {
      printf("air_metal_test: rwa_buffer_create (the BGRA stream): %s\n", why);
      return 1;
   }
   attrib_fill(rwa_buffer_map(bgra_stream), rwa_buffer_size(bgra_stream), &bgra);
   struct rwa_render_shape bgra_shape = shape;
   attrib_configure(&bgra_shape, &bgra, MTL_STEP_PER_VERTEX, 1, ~0u);
   struct rwa_render *bgra_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &bgra_shape, NULL, why, sizeof(why));
   if (!bgra_pipe) {
      printf("air_metal_test: rwa_render_create (%s): %s\n", bgra.what, why);
      return 1;
   }
   {
      struct index_picture pbgra;

      streams[5] = (struct rwa_vertex_stream){ .buffer = bgra_stream, .offset = 0 };
      d = base;
      d.vertex_count = 4;
      if (!index_draw(bgra_pipe, &d, color, color_count, &pbgra, bgra.what))
         return 1;
      struct attrib_verdict v = attrib_judge(&pbgra);
      printf("air_metal_test: 2a. %s, whose first byte is zero\n", bgra.what);
      attrib_print(&pbgra, &v);
      unsigned diff = index_picture_diff(&pn, &pbgra);
      printf("air_metal_test:   %u of %u words differ from step 2: %s\n", diff,
             TEX * TEX * 4,
             diff == 0 ? "the BGRA order is honoured -- bytes 2 and 1 arrived as x and y"
             : v.column0 == TEX * TEX
                  ? "every pixel holds column 0, which is what reading the bytes in memory "
                    "order gives: the order was NOT applied"
                  : "neither picture, which is a third answer and wants reading");
      if (diff)
         ok = false;
      streams[5] = (struct rwa_vertex_stream){ .buffer = narrow_stream, .offset = 0 };
   }

   /* 3. The stride, with a picture that nothing else here produces: the narrow layout
    *    read at 32 bytes instead of 16, over a buffer that holds eight elements. The
    *    fetch then walks elements 0, 2, 4 and 6 -- two corners of the full quad and two
    *    of the half-size one -- so the coverage must differ from step 2 and the pixels
    *    it does cover must hold the far texel the half-size elements carry.
    */
   struct attrib_layout stride_layout = narrow;
   stride_layout.what = "the narrow layout at twice its stride";
   stride_layout.stride = 32;
   struct rwa_render_shape stride_shape = shape;
   attrib_configure(&stride_shape, &stride_layout, MTL_STEP_PER_VERTEX, 1, ~0u);
   struct rwa_render *stride_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &stride_shape, NULL, why, sizeof(why));
   if (!stride_pipe) {
      printf("air_metal_test: rwa_render_create (%s): %s\n", stride_layout.what, why);
      return 1;
   }
   d = base;
   d.vertex_count = 4;
   if (!index_draw(stride_pipe, &d, color, color_count, &pstride, stride_layout.what))
      return 1;
   printf("air_metal_test: 3. %s: the fetch walks elements 0, 2, 4, 6\n",
          stride_layout.what);
   {
      struct attrib_verdict v = attrib_judge(&pstride);
      attrib_print(&pstride, &v);
      unsigned diff = index_picture_diff(&pn, &pstride);
      printf("air_metal_test:   %u of %u words differ from step 2: %s\n", diff,
             TEX * TEX * 4,
             diff ? "the stride decides which element a vertex reads"
                  : "A STRIDE OF 32 DREW WHAT A STRIDE OF 16 DREW, so the stride is not "
                    "read");
      if (!diff)
         ok = false;
   }

   /* 4. The buffer index, from the other side: the same narrow layout with both
    *    attributes fetched from index 7, and the stream bound at 7 and nowhere else. An
    *    implementation that bound the stream at slot 0 and ignored Metal's index would
    *    fetch from an empty slot here and draw nothing.
    */
   struct attrib_layout seven = narrow;
   seven.what = "the narrow layout fetched from buffer index 7";
   seven.pos.buffer = seven.uv.buffer = seven.decoy.buffer = 7;
   struct rwa_render_shape seven_shape = shape;
   attrib_configure(&seven_shape, &seven, MTL_STEP_PER_VERTEX, 1, ~0u);
   struct rwa_render_info sinfo = { 0 };
   struct rwa_render *seven_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &seven_shape, &sinfo, why, sizeof(why));
   if (!seven_pipe) {
      printf("air_metal_test: rwa_render_create (%s): %s\n", seven.what, why);
      return 1;
   }
   streams[5] = (struct rwa_vertex_stream){ 0 };
   streams[7] = (struct rwa_vertex_stream){ .buffer = narrow_stream, .offset = 0 };
   d = base;
   d.vertex_count = 4;
   if (!index_draw(seven_pipe, &d, color, color_count, &pindex7, seven.what))
      return 1;
   printf("air_metal_test: 4. %s (%u slots, mask 0x%x)\n", seven.what,
          sinfo.vertex_streams, sinfo.vertex_stream_mask);
   {
      unsigned diff = index_picture_diff(&pn, &pindex7);
      printf("air_metal_test:   %u of %u words differ from step 2, and it covered %u "
             "pixels: %s\n", diff, TEX * TEX * 4, pindex7.covered,
             diff == 0 ? "the same picture out of another slot -- Metal's buffer index "
                         "is carried all the way down"
                       : "the index moved the picture, which it must not");
      if (diff)
         ok = false;
   }
   streams[7] = (struct rwa_vertex_stream){ 0 };
   streams[5] = (struct rwa_vertex_stream){ .buffer = narrow_stream, .offset = 0 };

   /* 5 and 6. The indexed forms over a real layout, which is what every draw in a real
    *          application is: here an index is the ELEMENT THE FETCH READS, where in
    *          10.35 it was the [[vertex_id]] a shader indexed its own buffer with. Four
    *          indices must give step 2's picture and the last three must give the half
    *          of it above the diagonal.
    */
   d = base;
   d.vertex_count = 4;
   d.index_buffer = indices;
   if (!index_draw(narrow_pipe, &d, color, color_count, &pfull, "indices [0 1 2 3]"))
      return 1;
   printf("air_metal_test: 5. -drawIndexedPrimitives: 4 indices [0 1 2 3] over the "
          "narrow layout\n");
   {
      unsigned diff = index_picture_diff(&pn, &pfull);
      printf("air_metal_test:   %u of %u words differ from step 2: %s\n", diff,
             TEX * TEX * 4,
             diff ? "an index fed the fetch the wrong element"
                  : "the index buffer selects the element the fetch reads");
      if (diff)
         ok = false;
   }
   d = base;
   d.vertex_count = 3;
   d.index_buffer = indices;
   d.index_buffer_offset = 4 * 2;
   if (!index_draw(narrow_pipe, &d, color, color_count, &phalf, "indices [1 2 3]"))
      return 1;
   printf("air_metal_test: 6. the same buffer's last three indices [1 2 3] (UInt16 at "
          "byte 8)\n");
   {
      struct attrib_verdict v = attrib_judge(&phalf);
      attrib_print(&phalf, &v);
      printf("air_metal_test:   %u of %u words differ from step 5, covering %u pixels of "
             "16: %s\n", index_picture_diff(&pfull, &phalf), TEX * TEX * 4, phalf.covered,
             phalf.covered < TEX * TEX && phalf.covered > 0
                ? "one triangle of the quad out of three of its four elements"
                : "NOT one triangle, which is what an offset that was not applied or an "
                  "index that was not read would give");
      if (phalf.covered == 0 || phalf.covered == TEX * TEX)
         ok = false;
   }

   /* 7. MTLVertexStepFunctionPerInstance, which cannot be seen in a colour attachment
    *    the way 10.35 saw the instance COUNT (there, in a stencil buffer): two instances
    *    of this geometry cover the same pixels. So the coordinate comes out of a SECOND
    *    stream that steps per instance, holding four coordinates -- and the picture says
    *    which way it stepped. Per instance: all four vertices of one instance share one
    *    coordinate, the varying is constant, every pixel holds ONE texel, and the second
    *    instance is drawn over the first, so that texel is element 1's: (0.875, 0.875)
    *    is texel (3, 3), which is 103 203 306 400. Per vertex: the four vertices get
    *    four different coordinates and the picture is a gradient of four texels.
    */
   struct attrib_layout split = narrow;
   split.what = "the coordinate from a second stream that steps per instance";
   split.uv.format = MTL_VF_FLOAT2;
   split.uv.offset = 0;
   split.uv.buffer = 6;
   struct rwa_render_shape inst_shape = shape;
   attrib_configure(&inst_shape, &split, MTL_STEP_PER_INSTANCE, 1, 6);
   struct rwa_render_info iinfo = { 0 };
   struct rwa_render *inst_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &inst_shape, &iinfo, why, sizeof(why));
   if (!inst_pipe) {
      printf("air_metal_test: rwa_render_create (%s): %s\n", split.what, why);
      return 1;
   }
   streams[6] = (struct rwa_vertex_stream){ .buffer = instance_stream, .offset = 0 };
   d = base;
   d.vertex_count = 4;
   d.instance_count = 2;
   if (!index_draw(inst_pipe, &d, color, color_count, &pinst, split.what))
      return 1;
   printf("air_metal_test: 7. %s, 2 instances (%u slots, mask 0x%x)\n", split.what,
          iinfo.vertex_streams, iinfo.vertex_stream_mask);
   {
      struct attrib_verdict v = attrib_judge(&pinst);
      float t[4];
      texel_value(0, TEX - 1, TEX - 1, 0, t);
      attrib_print(&pinst, &v);
      bool flat_far = v.uniform == TEX * TEX && pinst.word[0][0] == (uint32_t)t[0] &&
                      pinst.word[0][1] == (uint32_t)t[1];
      printf("air_metal_test:   expecting %u %u %u %u in all 16 pixels (texel (3,3), the "
             "second instance's element): %s\n", (unsigned)t[0], (unsigned)t[1],
             (unsigned)t[2], (unsigned)t[3],
             flat_far ? "the stream stepped PER INSTANCE"
                      : v.uniform != TEX * TEX
                           ? "the picture is a gradient, so that stream stepped per "
                             "VERTEX"
                           : "flat, but not that texel");
      if (!flat_far)
         ok = false;
   }

   /* 8. The step RATE, which is Gallium's instance_divisor: two instances at a rate of 2
    *    share element 0, so the picture must be the NEAR texel instead -- 100 200 300
    *    400 -- and a rate that was dropped would leave step 7's far texel.
    */
   struct rwa_render_shape rate_shape = shape;
   attrib_configure(&rate_shape, &split, MTL_STEP_PER_INSTANCE, 2, 6);
   struct rwa_render *rate_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &rate_shape, NULL, why, sizeof(why));
   if (!rate_pipe) {
      printf("air_metal_test: rwa_render_create (a step rate of 2): %s\n", why);
      return 1;
   }
   d = base;
   d.vertex_count = 4;
   d.instance_count = 2;
   if (!index_draw(rate_pipe, &d, color, color_count, &prate, "a step rate of 2"))
      return 1;
   printf("air_metal_test: 8. the same, at a step rate of 2: both instances share "
          "element 0\n");
   {
      struct attrib_verdict v = attrib_judge(&prate);
      float t[4];
      texel_value(0, 0, 0, 0, t);
      attrib_print(&prate, &v);
      bool flat_near = v.uniform == TEX * TEX && prate.word[0][0] == (uint32_t)t[0] &&
                       prate.word[0][1] == (uint32_t)t[1];
      printf("air_metal_test:   expecting %u %u %u %u in all 16 pixels (texel (0,0)), "
             "and step 7's picture would mean the rate was dropped: %s\n",
             (unsigned)t[0], (unsigned)t[1], (unsigned)t[2], (unsigned)t[3],
             flat_near ? "the step rate IS the instance divisor"
                       : index_picture_diff(&pinst, &prate) == 0
                            ? "step 7's picture exactly, so the rate was dropped"
                            : "neither, which is a third answer and wants reading");
      if (!flat_near)
         ok = false;
   }

   /*
    * 9. THE FUNCTION-CONSTANT GATE, which nothing above touches and which is the one rule in
    *    docs/air.md 3.1 a runtime cannot derive from the module alone: an attribute the
    *    module reads only when an implicit predicate is true is FETCHED either way -- the
    *    parameter exists whatever the predicate says -- so its bit is in
    *    vertex_attributes_read, and a pipeline whose function constants make the predicate
    *    false may legally configure nothing at that index. QuartzCore's fixed_vert_lph_spc is
    *    the shape of it: five attributes, listed 0, 4, 1, 2, 3, with 4 (point_size), 1 and 2
    *    (texcoord0, texcoord1) gated and 0 (position) and 3 (color) not.
    *
    *    Two configurations, differing in one field, say whether the gate is honoured:
    *
    *      - position AND color configured, the three gated ones left out: the layout must be
    *        ACCEPTED. The three get a placeholder element each -- this library's own quad
    *        buffer at a stride of 0 -- because skipping them would shift every later
    *        driver_location by one.
    *      - position alone: the layout must be REFUSED, and the sentence must name COLOR. That
    *        is the whole measurement: color is the fifth attribute in AIR's own list and the
    *        first UNGATED one that is missing, so a runtime that demanded a binding for every
    *        bit of vertex_attributes_read would have stopped at point_size, the second.
    *
    *    "Accepted" cannot be read off a returned pipeline on this machine, and that is
    *    softpipe's limit rather than a gap in the measurement: every module of this shape
    *    dies inside softpipe -- the `_lph` ones on the 16-bit narrowing of a `half4`
    *    attribute (nir_to_tgsi ASSERTS on a 16-bit opcode instead of declining it), the
    *    `_lpf` ones on `store_scratch` -- and in both cases the assertion's own text is in
    *    the log above the verdict. So the attempt is made in a child process and the outcome
    *    read off its exit: a child that printed a refusal was stopped by the layout, and a
    *    child that DIED got past the layout and was stopped by the back end. The two are
    *    distinguishable, which is what makes this a comparison and not a hope.
    */
   if (gated) {
      struct metallib glib = { 0 };
      char gname[256];
      const uint8_t *gbitcode = NULL;
      size_t gbitcode_size = 0;
      bool gated_ok = true;

      if (!load_metallib(vpath, &glib) ||
          !find_function(&glib, gated, gname, sizeof(gname), &gbitcode, &gbitcode_size))
         return 1;
      printf("air_metal_test: 9. the function-constant gate, over %s\n", gname);

      static const struct { const char *what; bool color; } gcase[2] = {
         { "position and color configured, the gated three left out", true },
         { "position alone configured", false },
      };
      for (unsigned c = 0; c < 2; c++) {
         struct rwa_render_shape gs = shape;

         memset(gs.vertex_attribute, 0, sizeof(gs.vertex_attribute));
         memset(gs.vertex_layout, 0, sizeof(gs.vertex_layout));
         gs.vertex_attribute[0] = (struct rwa_vertex_attribute){
            .format = MTL_VF_FLOAT4, .offset = 0, .buffer_index = 0 };
         if (gcase[c].color)
            gs.vertex_attribute[3] = (struct rwa_vertex_attribute){
               .format = MTL_VF_UCHAR4_NORMALIZED, .offset = 16, .buffer_index = 0 };
         gs.vertex_layout[0] = (struct rwa_vertex_layout){
            .stride = 32, .step_function = MTL_STEP_PER_VERTEX, .step_rate = 1 };

         fflush(stdout);
         pid_t pid = fork();
         if (pid < 0) {
            printf("air_metal_test:   fork: %s\n", strerror(errno));
            return 1;
         }
         if (pid == 0) {
            struct rwa_render_info gi = { 0 };

            why[0] = '\0';
            struct rwa_render *r =
               rwa_render_create(device, gbitcode, gbitcode_size, gated, fbitcode,
                                 fbitcode_size, ffunc, &gs, &gi, why, sizeof(why));
            if (r) {
               printf("air_metal_test:     built: %u elements, attributes", 
                      gi.vertex_attribute_count);
               for (unsigned k = 0; k < gi.vertex_attribute_count; k++)
                  printf(" %u", gi.vertex_attribute_index[k]);
               printf("\n");
            } else {
               printf("air_metal_test:     refused -- %s\n", why);
            }
            fflush(stdout);
            _exit(r ? 0 : 1);
         }
         int wstatus = 0;
         if (waitpid(pid, &wstatus, 0) < 0) {
            printf("air_metal_test:   waitpid: %s\n", strerror(errno));
            return 1;
         }
         bool refused_by_layout = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 1;
         bool got_past = !WIFEXITED(wstatus) ||
                         (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

         printf("air_metal_test:   %s: %s\n", gcase[c].what,
                refused_by_layout ? "the LAYOUT refused it"
                : WIFEXITED(wstatus) ? "built"
                                     : "got past the layout and died in the back end, which "
                                       "on this machine is softpipe's own assertion -- its "
                                       "text is in the lines above -- and not a refusal of "
                                       "the layout");
         if (gcase[c].color && !got_past) {
            printf("air_metal_test:     and it must not have been: the three unconfigured "
                   "attributes are gated by function constants, so a pipeline is allowed to "
                   "configure nothing at those indices\n");
            gated_ok = false;
         }
         if (!gcase[c].color && !refused_by_layout) {
            printf("air_metal_test:     and it must have been: color is read, ungated and "
                   "unconfigured\n");
            gated_ok = false;
         }
      }
      printf("air_metal_test:   %s\n",
             gated_ok ? "the gate is honoured: a gated attribute needs no configuration and "
                        "an ungated one does, and the refusal named the ungated one -- which "
                        "is the fifth attribute in AIR's list, not the second"
                      : "THE GATE IS NOT HONOURED as docs/air.md 3.1 requires");
      if (!gated_ok)
         ok = false;
   } else {
      printf("air_metal_test: 9. the function-constant gate was NOT measured: give a fifth "
             "argument naming a module whose attributes are gated (QuartzCore's "
             "fixed_vert_lph_spc) to measure it\n");
   }

   /*
    * And the layouts that must be REFUSED, each by name. Every one of them is a
    * descriptor a real application can build -- Metal accepts all but the last two -- and
    * what makes them worth a sentence rather than a silent clamp is in air_metal.h: the
    * offset's twelve bits are set with a masking setter, so 4096 would fetch 0, and
    * fd6_vertex_state_create asserts on a format it cannot fetch instead of refusing it.
    */
   struct {
      const char *what;
      struct attrib_layout layout;
      unsigned step_function, step_rate, instance_buffer;
      bool generated;
   } bad[] = {
      { "attribute 0 configured with no format at all", narrow, MTL_STEP_PER_VERTEX, 1,
        ~0u, false },
      { "an integer format for a float4 attribute", narrow, MTL_STEP_PER_VERTEX, 1, ~0u,
        false },
      { "MTLVertexFormatFloatRGB9E5, which the VFD cannot fetch", narrow,
        MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "MTLVertexFormatUInt1010102Normalized, a packed word", narrow,
        MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "a format of 99, which Metal has not", narrow, MTL_STEP_PER_VERTEX, 1, ~0u,
        false },
      { "an attribute at offset 4096", narrow, MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "a stride of 4096", narrow, MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "an attribute that reads into the next vertex", narrow, MTL_STEP_PER_VERTEX, 1,
        ~0u, false },
      { "MTLVertexStepFunctionPerPatch", split, MTL_STEP_PER_PATCH, 1, 6, false },
      { "per-vertex at a step rate of 3", narrow, MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "a stream at the buffer index the module reads mvp_matrix from", narrow,
        MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "MTLVertexStepFunctionPerVertex with a stride of 0", narrow, MTL_STEP_PER_VERTEX,
        1, ~0u, false },
      { "a vertex descriptor with no vertex function at all", narrow, MTL_STEP_PER_VERTEX,
        1, ~0u, true },
   };
   unsigned refused = 0;
   printf("air_metal_test: and the vertex layouts that must be refused, each by name:\n");
   for (unsigned i = 0; i < COUNT_OF(bad); i++) {
      struct rwa_render_shape s = shape;

      attrib_configure(&s, &bad[i].layout, bad[i].step_function, bad[i].step_rate,
                       bad[i].instance_buffer);
      switch (i) {
      case 0: s.vertex_attribute[0].format = 0; break;
      case 1: s.vertex_attribute[0].format = MTL_VF_INT4; break;
      case 2: s.vertex_attribute[0].format = MTL_VF_FLOAT_RGB9E5; break;
      case 3: s.vertex_attribute[0].format = MTL_VF_UINT1010102_NORMALIZED; break;
      case 4: s.vertex_attribute[0].format = 99; break;
      case 5: s.vertex_attribute[0].offset = 4096; break;
      case 6: s.vertex_layout[5].stride = 4096; break;
      case 7: s.vertex_attribute[0].offset = 12; break;   /* 12 + 8 > a 16-byte element */
      case 8: break;                                      /* the step function is above */
      case 9: s.vertex_layout[5].step_rate = 3; break;
      case 10:
         s.vertex_attribute[0].buffer_index = 2;          /* mvp_matrix's own index */
         s.vertex_layout[2] = s.vertex_layout[5];
         break;
      case 11: s.vertex_layout[5].stride = 0; break;
      case 12: break;                                     /* no vertex module, below */
      }
      why[0] = '\0';
      struct rwa_render *r = bad[i].generated
         ? rwa_render_create(device, NULL, 0, NULL, fbitcode, fbitcode_size, ffunc, &s,
                             NULL, why, sizeof(why))
         : rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode,
                             fbitcode_size, ffunc, &s, NULL, why, sizeof(why));
      if (r) {
         printf("air_metal_test:   %s: BUILT, and it should not have\n", bad[i].what);
         rwa_render_destroy(r);
         ok = false;
      } else {
         printf("air_metal_test:   %s: refused -- %s\n", bad[i].what, why);
         refused++;
      }
   }
   printf("air_metal_test: %u of %u refused by name\n", refused, (unsigned)COUNT_OF(bad));
   if (refused != COUNT_OF(bad))
      ok = false;

   /* And the draws that must be refused although the pipeline is sound: the stream the
    * layout fetches from, missing, and a buffer too small for the vertices asked for.
    * Both are the application's mistake and both are bounded rather than wild on these
    * back ends -- freedreno programs VFD_FETCH_SIZE off the resource and softpipe clamps
    * in the draw module -- so what they would otherwise produce is a picture made partly
    * of zeros.
    */
   printf("air_metal_test: and the draws that must be refused:\n");
   {
      static const struct { const char *what; unsigned slot, count; size_t offset; } dbad[] = {
         { "nothing bound at the slot the layout fetches from", 9, 4, 0 },
         { "four vertices of a stream bound 16 bytes from its end", 5, 4, 112 },
         { "sixteen vertices of a stream that holds eight", 5, 16, 0 },
      };
      unsigned draw_refused = 0;
      for (unsigned i = 0; i < COUNT_OF(dbad); i++) {
         struct rwa_draw r = base;
         static const float black[4] = { 0, 0, 0, 0 };

         memset(streams, 0, sizeof(streams));
         if (dbad[i].slot < COUNT_OF(streams))
            streams[dbad[i].slot] = (struct rwa_vertex_stream){
               .buffer = narrow_stream, .offset = dbad[i].offset };
         r.clear_color = black;
         r.vertex_count = dbad[i].count;
         why[0] = '\0';
         if (rwa_render_draw(narrow_pipe, &r, why, sizeof(why)) == 0) {
            printf("air_metal_test:   %s: DREW, and it should not have\n", dbad[i].what);
            ok = false;
         } else {
            printf("air_metal_test:   %s: refused -- %s\n", dbad[i].what, why);
            draw_refused++;
         }
      }
      if (draw_refused != COUNT_OF(dbad))
         ok = false;
   }

   printf("air_metal_test: %s\n",
          ok ? "THE VERTEX ATTRIBUTE LAYOUT IS READ -- the offsets, the stride, the "
               "formats, the buffer index, the step function and its rate each changed "
               "what came back, and the element order is the shader's driver_location "
               "order and not Metal's attribute order"
             : "SOMETHING ABOVE DISAGREES, and a layout that is nearly right draws a "
               "picture of the wrong vertices");

   rwa_render_destroy(wide_pipe);
   rwa_render_destroy(narrow_pipe);
   rwa_render_destroy(stride_pipe);
   rwa_render_destroy(seven_pipe);
   rwa_render_destroy(inst_pipe);
   rwa_render_destroy(rate_pipe);
   rwa_sampler_destroy(sampler);
   rwa_texture_destroy(tex);
   rwa_render_destroy(bgra_pipe);
   rwa_buffer_destroy(bgra_stream);
   rwa_buffer_destroy(wide_stream);
   rwa_buffer_destroy(narrow_stream);
   rwa_buffer_destroy(instance_stream);
   rwa_buffer_destroy(indices);
   for (unsigned i = 0; i < nvbuf; i++)
      rwa_buffer_destroy(vbuffers[i]);
   for (unsigned i = 0; i < color_count; i++)
      rwa_texture_destroy(color[i]);
   rwa_device_close(device);
   return ok ? 0 : 1;
}
