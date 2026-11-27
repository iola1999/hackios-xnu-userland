/* Indexed draw tests, included into air_metal_test.c. */

/* The index buffer every picture step is drawn from: the strip of four, then the second
 * triangle, so that no two steps need two buffers. One list, widened for UInt32, so the
 * two index types cannot quietly be given different geometry.
 */
static const uint16_t index_values[7] = { 0, 1, 2, 3, 1, 2, 3 };

/* One picture: the whole 4x4 attachment, and how many of its pixels the draw covered.
 * The clear colour is zero and clearFragment20's first word is 2458616957, which is
 * checked before anything is counted -- for a shader whose first word were zero,
 * "covered" would be counting the clear.
 */
struct index_picture {
   uint32_t word[TEX * TEX][4];
   unsigned covered;
};

static bool
index_covered(const struct index_picture *p, unsigned x, unsigned y)
{
   return p->word[y * TEX + x][0] != 0;
}

/* How many of the 64 words of two pictures differ. Zero is "the same picture", and it is
 * the whole attachment and not one pixel: two triangles that differ only along the
 * diagonal differ in four pixels, which a corner-and-corner comparison would miss.
 */
static unsigned
index_picture_diff(const struct index_picture *a, const struct index_picture *b)
{
   unsigned n = 0;
   for (unsigned i = 0; i < TEX * TEX; i++)
      for (unsigned c = 0; c < 4; c++)
         if (a->word[i][c] != b->word[i][c])
            n++;
   return n;
}

/* One draw, into attachments cleared to zero, and the picture that came of it. The clear
 * is forced here rather than left to the caller so that every step starts from the same
 * attachment, and a step that drew nothing is distinguishable from a step that was not
 * run.
 */
static bool
index_draw(struct rwa_render *render, struct rwa_draw *draw,
           struct rwa_texture *const *color, unsigned color_count,
           struct index_picture *out, const char *what)
{
   char why[WHY] = "";
   const unsigned origin[3] = { 0, 0, 0 };
   const unsigned extent[3] = { TEX, TEX, 1 };
   static const float black[4] = { 0, 0, 0, 0 };

   draw->clear_color = black;
   memset(out, 0, sizeof(*out));
   if (rwa_render_draw(render, draw, why, sizeof(why)) != 0) {
      printf("air_metal_test: %s: rwa_render_draw: %s\n", what, why);
      return false;
   }
   if (rwa_texture_read(color[color_count - 1], 0, 0, origin, extent, out->word, 0, why,
                        sizeof(why)) != 0) {
      printf("air_metal_test: %s: rwa_texture_read: %s\n", what, why);
      return false;
   }
   for (unsigned i = 0; i < TEX * TEX; i++)
      out->covered += out->word[i][0] != 0;
   return true;
}

/* The picture, as four rows of four, and the two corners the verdicts are made of. */
static void
index_print(const struct index_picture *p)
{
   printf("air_metal_test:   coverage ");
   for (unsigned y = 0; y < TEX; y++) {
      for (unsigned x = 0; x < TEX; x++)
         printf("%c", index_covered(p, x, y) ? '#' : '.');
      printf("%s", y + 1 < TEX ? "/" : "");
   }
   printf("  %u of %u pixels, (0,0) %s, (3,3) %s\n", p->covered, TEX * TEX,
          index_covered(p, 0, 0) ? "drawn" : "clear",
          index_covered(p, TEX - 1, TEX - 1) ? "drawn" : "clear");
}

static int
run_index(const char *path, const char *func, const char *vfunc)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      return 1;
   char vname[256];
   const uint8_t *vbitcode = NULL;
   size_t vbitcode_size = 0;
   if (vfunc && !find_function(&lib, vfunc, vname, sizeof(vname), &vbitcode,
                               &vbitcode_size))
      return 1;

   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: --draw-index %s on \"%s\"\n", name, rwa_device_name(device));

   /* clearFragment20's three RGBA32Uint attachments, as --draw's default: it writes
    * render target 2, so 0 and 1 have to exist for 2 to be bound at all, and 2 is the one
    * read back. 4x4 is air_run's attachment, so a pixel here and a pixel there are the
    * same measurement.
    */
   const unsigned color_count = 3;
   struct rwa_texture *color[3] = { 0 };
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

   /*
    * TWO pipelines of the same fragment module, and what each is for:
    *
    *   - `plain` has no depth attachment, so the picture steps go through exactly the
    *     state the 256 pixel-compared fragment shaders went through (10.30: clip_halfz
    *     is set only for a pipeline that has a depth attachment). An indexed picture and
    *     a non-indexed one are then comparable, which is the whole of step 2.
    *   - `counted` has Depth32Float_Stencil8, because the instance count can only be
    *     measured in a stencil buffer and Metal requires a pass's formats to be the
    *     pipeline's -- so it cannot be the same pipeline.
    */
   struct rwa_render_shape shape = { .color_count = color_count, .sample_count = 1 };
   for (unsigned i = 0; i < color_count; i++)
      shape.color_format[i] = MTL_RGBA32_UINT;
   struct rwa_render_shape zshape = shape;
   zshape.depth_format = MTL_DEPTH32_FLOAT_STENCIL8;
   zshape.stencil_format = MTL_DEPTH32_FLOAT_STENCIL8;

   struct rwa_render_info info = { 0 }, zinfo = { 0 };
   struct rwa_render *plain =
      rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &shape, &info,
                        why, sizeof(why));
   if (!plain) {
      printf("air_metal_test: rwa_render_create: %s\n", why);
      return 1;
   }
   struct rwa_render *counted =
      rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &zshape,
                        &zinfo, why, sizeof(why));
   if (!counted) {
      printf("air_metal_test: rwa_render_create (with a stencil attachment): %s\n", why);
      return 1;
   }
   printf("air_metal_test: pipeline: %u fragment buffers, %u fragment textures, %u "
          "samplers, vertex stage %s\n", info.fragment_buffers, info.fragment_textures,
          info.fragment_samplers, info.generated_vertex_stage ? "generated" : "supplied");
   if (info.fragment_textures || info.fragment_samplers) {
      printf("air_metal_test: this mode binds no textures and this shader samples %u, so "
             "it would be refused for the binding and not measured for the indices: pick "
             "one that samples nothing, as clearFragment20 does\n",
             info.fragment_textures);
      return 2;
   }

   /* The fragment stage's constant buffers, air_run's pattern, the same in every step:
    * what is compared here is coverage, so the colour must NOT change between draws
    * (run_depth varies it on purpose, for the opposite reason -- there a draw that was
    * not discarded has to leave a colour that was not there).
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

   /* The two index buffers, one list of indices in two widths, and one more holding an
    * index past the quad for the refusal that needs it. An MTLBuffer is what Metal's
    * index buffer is, so these are rwa_buffers like any other -- nothing was added to
    * the contract to make a buffer that could be one.
    */
   struct rwa_buffer *i16 = rwa_buffer_create(device, sizeof(index_values), why,
                                              sizeof(why));
   struct rwa_buffer *i32 = rwa_buffer_create(device, sizeof(index_values) * 2, why,
                                              sizeof(why));
   static const uint16_t past_values[3] = { 0, 1, 4 };
   struct rwa_buffer *past = rwa_buffer_create(device, sizeof(past_values), why,
                                               sizeof(why));
   if (!i16 || !i32 || !past) {
      printf("air_metal_test: rwa_buffer_create (an index buffer): %s\n", why);
      return 1;
   }
   memcpy(rwa_buffer_map(i16), index_values, sizeof(index_values));
   for (unsigned i = 0; i < COUNT_OF(index_values); i++)
      ((uint32_t *)rwa_buffer_map(i32))[i] = index_values[i];
   memcpy(rwa_buffer_map(past), past_values, sizeof(past_values));
   printf("air_metal_test: the index buffer holds");
   for (unsigned i = 0; i < COUNT_OF(index_values); i++)
      printf(" %u", index_values[i]);
   printf(", as %zu bytes of MTLIndexTypeUInt16 and %zu of UInt32\n",
          rwa_buffer_size(i16), rwa_buffer_size(i32));

   /* What never changes between steps: the attachments, the fragment bindings and the
    * primitive. MTLPrimitiveTypeTriangleStrip is what all 256 compared shaders are drawn
    * with, and a strip of three is one triangle of it -- which is how 10.34's split
    * configuration got a partly covered pixel without adding anything to the contract.
    */
   const struct rwa_draw base = {
      .color = color, .color_count = color_count,
      .fragment = { .buffers = buffers, .buffer_count = nbuf },
      .primitive = 4,
   };

   bool ok = true;
   struct index_picture full, idx_full, lo, hi, bias, lo32, hi32;
   struct rwa_draw d;

   /* 1. The reference: -drawPrimitives:vertexStart:0 vertexCount:4, the only form this
    *    library had. Its four words are the fill colour every step below is counted by,
    *    and they have to be 10.22's or nothing here is comparable with anything.
    */
   d = base;
   d.vertex_count = 4;
   if (!index_draw(plain, &d, color, color_count, &full, "the non-indexed strip of 4"))
      return 1;
   printf("air_metal_test: 1. -drawPrimitives: vertexStart 0 vertexCount 4\n");
   printf("air_metal_test:   the fill colour is %u %u %u %u\n", full.word[0][0],
          full.word[0][1], full.word[0][2], full.word[0][3]);
   index_print(&full);
   if (full.word[0][0] == 0) {
      printf("air_metal_test: this shader's first word is zero, so a covered pixel cannot "
             "be told from the clear colour and nothing below would mean anything\n");
      return 2;
   }
   if (full.covered != TEX * TEX)
      ok = false;

   /* 2. The same four vertices through the index buffer. This is the CONTROL and says so:
    *    it must be the same picture, and a driver ignoring the index buffer passes it too.
    */
   d = base;
   d.vertex_count = 4;
   d.index_buffer = i16;
   if (!index_draw(plain, &d, color, color_count, &idx_full, "the indexed strip of 4"))
      return 1;
   printf("air_metal_test: 2. -drawIndexedPrimitives: 4 indices [0 1 2 3], UInt16, "
          "offset 0\n");
   index_print(&idx_full);
   {
      unsigned diff = index_picture_diff(&full, &idx_full);
      printf("air_metal_test:   %s step 1: %u of %u words differ%s\n",
             diff ? "DIFFERS from" : "the same as", diff, TEX * TEX * 4,
             diff ? "" : " -- the control: a draw that ignored the index buffer passes "
                         "this one as well");
      if (diff)
         ok = false;
   }

   /* 3 and 4. The measurement: the same three-index draw at two places in one buffer. */
   static const struct { const char *what; size_t at16, at32; } tri[2] = {
      { "indices [0 1 2]: the half below the diagonal", 0,     0 },
      { "indices [1 2 3]: the half above it",           4 * 2, 4 * 4 },
   };
   struct index_picture *half[2] = { &lo, &hi };
   for (unsigned t = 0; t < 2; t++) {
      d = base;
      d.vertex_count = 3;
      d.index_buffer = i16;
      d.index_buffer_offset = tri[t].at16;
      if (!index_draw(plain, &d, color, color_count, half[t], tri[t].what))
         return 1;
      printf("air_metal_test: %u. %s (UInt16 at byte %zu)\n", 3 + t, tri[t].what,
             tri[t].at16);
      index_print(half[t]);
   }
   {
      /*
       * EACH TRIANGLE'S OWN RIGHT ANGLE, in the corner Metal's orientation puts it in --
       * and the corners are not the ones this check was written with, which is worth the
       * paragraph because the test has been failing on it for a day.
       *
       * The quad is a strip of (-1,-1), (1,-1), (-1,1), (1,1), so indices [0 1 2] are a
       * triangle whose right angle is at clip (-1,-1) and [1 2 3] one whose right angle
       * is at (1,1). Where those land in memory is the viewport's business, and the
       * viewport changed after this check was written: `air_metal_test --draw-index`
       * came in on 21 October at 11:37 and the NEGATIVE y scale -- clip y = +1 in memory
       * row 0, which is Metal's rule and not OpenGL's (docs/metal.md 10.28) -- came in at
       * 13:49 the same day. So clip (-1,-1) is the BOTTOM-left pixel now where it used to
       * be the top-left one, and a check that asks for the first triangle's right angle
       * at (0, 0) is asking for the picture OpenGL would have drawn.
       *
       * After the flip: [0 1 2] holds (0, TEX-1) and not (TEX-1, 0), and [1 2 3] the
       * other way about. The two pixels on the shared diagonal, (0,0) and (TEX-1,TEX-1),
       * are deliberately not asked about: a pixel centre on an edge belongs to whichever
       * triangle the fill rule gives it, which is a rasteriser's business and not an
       * index buffer's.
       */
      unsigned diff = index_picture_diff(&lo, &hi);
      bool read_them = index_covered(&lo, 0, TEX - 1) &&
                       !index_covered(&lo, TEX - 1, 0) &&
                       index_covered(&hi, TEX - 1, 0) &&
                       !index_covered(&hi, 0, TEX - 1);
      printf("air_metal_test:   the two halves differ in %u of %u words, and %s\n", diff,
             TEX * TEX * 4,
             read_them ? "each right angle is drawn in its own triangle only -- THE INDEX "
                         "BUFFER AND THE OFFSET ARE READ"
                       : "a right angle came back on the wrong side, which is what an "
                         "ignored index buffer or offset gives: the first triangle twice");
      if (!read_them || diff == 0)
         ok = false;
   }

   /* 5. baseVertex, against the permutation: 0,1,2 plus one IS 1,2,3. */
   d = base;
   d.vertex_count = 3;
   d.index_buffer = i16;
   d.base_vertex = 1;
   if (!index_draw(plain, &d, color, color_count, &bias,
                   "indices [0 1 2] with baseVertex 1"))
      return 1;
   printf("air_metal_test: 5. indices [0 1 2] with baseVertex 1, which is the vertex "
          "sequence 1,2,3\n");
   index_print(&bias);
   {
      unsigned from_hi = index_picture_diff(&bias, &hi);
      unsigned from_lo = index_picture_diff(&bias, &lo);
      printf("air_metal_test:   %u of %u words differ from step 4 and %u from step 3: "
             "%s\n", from_hi, TEX * TEX * 4, from_lo,
             from_hi == 0 ? "baseVertex IS added to every index"
             : from_lo == 0 ? "baseVertex was DROPPED -- this is the unbiased picture"
                            : "neither picture, which is a third answer and wants reading");
      if (from_hi != 0)
         ok = false;
   }

   /* 6 and 7. The same indices as UInt32. A reader taking 32-bit indices two bytes at a
    *          time would see 0, 0, 1 and draw a triangle of no area.
    */
   struct index_picture *half32[2] = { &lo32, &hi32 };
   for (unsigned t = 0; t < 2; t++) {
      d = base;
      d.vertex_count = 3;
      d.index_buffer = i32;
      d.index_type = 1;             /* MTLIndexTypeUInt32 */
      d.index_buffer_offset = tri[t].at32;
      if (!index_draw(plain, &d, color, color_count, half32[t], tri[t].what))
         return 1;
      unsigned diff = index_picture_diff(half32[t], half[t]);
      printf("air_metal_test: %u. the same %s as UInt32 at byte %zu: %u of %u words "
             "differ from the UInt16 picture, and it covered %u pixels%s\n", 6 + t,
             t ? "[1 2 3]" : "[0 1 2]", tri[t].at32, diff, TEX * TEX * 4,
             half32[t]->covered,
             half32[t]->covered == 0 ? " -- nothing at all, which is what reading them as "
                                       "UInt16 gives" : "");
      if (diff)
         ok = false;
   }

   /*
    * The refusals. Each is a draw this library must decline by name, and the sentence is
    * printed rather than counted so the reason can be read. They matter beyond tidiness:
    * a driver whose copy of struct rwa_draw is a different shape sends garbage in exactly
    * these fields, and a garbage index type, an offset no index size divides and a count
    * no buffer holds are three of the sentences below -- which is the difference between
    * a mismatched header and a picture of the wrong geometry.
    */
   enum { NO_BUFFER, BUF16, BUF32, BUF_PAST };
   static const struct { const char *what; unsigned count, type, vertex_start, buffer;
                         size_t offset; int base_vertex; } bad[] = {
      { "an index type of 2, which MTLIndexType has not",       3, 2, 0, BUF16,    0, 0 },
      { "UInt16 at an odd offset",                              3, 0, 0, BUF16,    1, 0 },
      { "UInt32 at an offset of 2, which 4 does not divide",    3, 1, 0, BUF32,    2, 0 },
      { "8 indices of a buffer that holds 7",                   8, 0, 0, BUF16,    0, 0 },
      { "an index of 4 over a quad of four corners",            3, 0, 0, BUF_PAST, 0, 0 },
      { "a baseVertex of -1, below the first corner",           3, 0, 0, BUF16,    0, -1 },
      { "a baseVertex with no index buffer at all",             4, 0, 0, NO_BUFFER, 0, 1 },
      { "an indexed draw with a vertexStart, which Metal's "
        "indexed forms have not",                               3, 0, 1, BUF16,    0, 0 },
      { "an indexed draw of no indices",                        0, 0, 0, BUF16,    0, 0 },
   };
   unsigned refused = 0;
   printf("air_metal_test: and the draws that must be refused, each by name:\n");
   for (unsigned i = 0; i < COUNT_OF(bad); i++) {
      static const float black[4] = { 0, 0, 0, 0 };
      struct rwa_draw r = base;

      r.clear_color = black;
      r.vertex_count = bad[i].count;
      r.vertex_start = bad[i].vertex_start;
      r.index_type = bad[i].type;
      r.index_buffer_offset = bad[i].offset;
      r.base_vertex = bad[i].base_vertex;
      r.index_buffer = bad[i].buffer == BUF16 ? i16
                       : bad[i].buffer == BUF32 ? i32
                       : bad[i].buffer == BUF_PAST ? past : NULL;
      why[0] = '\0';
      if (rwa_render_draw(plain, &r, why, sizeof(why)) == 0) {
         printf("air_metal_test:   %s: DREW, and it should not have\n", bad[i].what);
         ok = false;
      } else {
         printf("air_metal_test:   %s: refused -- %s\n", bad[i].what, why);
         refused++;
      }
   }
   printf("air_metal_test: %u of %u refused by name\n", refused, (unsigned)COUNT_OF(bad));
   if (refused != COUNT_OF(bad))
      ok = false;

   /*
    * The instance count, in the stencil buffer, because no colour attachment can hold
    * it: three instances of this geometry are the same pixels in the same colour as one.
    * The depth test is Always with no depth write, so nothing but the instances can add
    * anything to what the stencil counts.
    */
   const struct rwa_texture_shape zts = {
      .pixel_format = MTL_DEPTH32_FLOAT_STENCIL8, .type = MTL_TEXTURE_TYPE_2D,
      .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1, .usage = MTL_USAGE_RENDER_TARGET,
   };
   struct rwa_texture *zt = rwa_texture_create(device, &zts, why, sizeof(why));
   if (!zt) {
      printf("air_metal_test: rwa_texture_create (the stencil attachment): %s\n", why);
      return 1;
   }
   struct rwa_depth_stencil_state count_state = {
      .depth_compare_function = 7,              /* MTLCompareFunctionAlways */
      .depth_write_enabled = 0,
      .front = { .compare_function = 7,
                 .depth_stencil_pass_operation = 3,  /* MTLStencilOperationIncrementClamp */
                 .read_mask = 0xffffffff, .write_mask = 0xffffffff },
   };
   count_state.back = count_state.front;
   struct rwa_depth_stencil *counter = rwa_depth_stencil_create(device, &count_state, why,
                                                                sizeof(why));
   if (!counter) {
      printf("air_metal_test: rwa_depth_stencil_create (the counter): %s\n", why);
      return 1;
   }
   static const struct { const char *what; bool indexed; unsigned instances, base; } runs[] = {
      { "-drawPrimitives: with 1 instance",                        false, 1, 0 },
      { "-drawPrimitives: with 3 instances",                       false, 3, 0 },
      { "-drawIndexedPrimitives: with 1 instance",                 true,  1, 0 },
      { "-drawIndexedPrimitives: with 3 instances",                true,  3, 0 },
      { "-drawIndexedPrimitives: with 3 and a baseInstance of 5",  true,  3, 5 },
   };
   printf("air_metal_test: the instance count, counted in the stencil (Always + "
          "IncrementClamp over a clear of 0):\n");
   for (unsigned i = 0; i < COUNT_OF(runs); i++) {
      const unsigned clear_stencil = 0;
      uint8_t stencil[TEX * TEX];
      const unsigned origin[3] = { 0, 0, 0 };
      const unsigned extent[3] = { TEX, TEX, 1 };
      struct rwa_draw id = base;

      id.clear_color = (const float[]){ 0, 0, 0, 0 };
      id.depth = zt;
      id.clear_stencil = &clear_stencil;
      id.depth_stencil = counter;
      id.vertex_count = 4;
      id.instance_count = runs[i].instances;
      id.base_instance = runs[i].base;
      if (runs[i].indexed)
         id.index_buffer = i16;
      if (rwa_render_draw(counted, &id, why, sizeof(why)) != 0) {
         printf("air_metal_test:   %s: rwa_render_draw: %s\n", runs[i].what, why);
         ok = false;
         continue;
      }
      if (rwa_depth_read(zt, 0, 0, origin, extent, NULL, stencil, why, sizeof(why)) != 0) {
         printf("air_metal_test:   %s: rwa_depth_read: %s\n", runs[i].what, why);
         return 1;
      }
      bool uniform = true;
      for (unsigned k = 1; k < TEX * TEX; k++)
         uniform = uniform && stencil[k] == stencil[0];
      printf("air_metal_test:   %s: the stencil holds %u%s, expecting %u\n", runs[i].what,
             stencil[0], uniform ? " in all 16 pixels" : " at (0,0) and NOT everywhere",
             runs[i].instances);
      if (!uniform || stencil[0] != runs[i].instances)
         ok = false;
   }
   printf("air_metal_test: a baseInstance cannot be measured here and is not claimed: the "
          "generated vertex stage reads no [[instance_id]], and none of the corpus's three "
          "vertex functions declares one either\n");

   /*
    * And the same permutation over a SUPPLIED vertex stage, which is the path an
    * application is on: clearVertex is `position = vtx[vertex_id]`, so here an index is
    * not a row of a vertex attribute but the [[vertex_id]] the shader indexes its own
    * buffer with. Nothing about the geometry changes, and that is the point -- the two
    * halves must come back exactly as the generated stage drew them.
    */
   if (vfunc) {
      struct rwa_render_info vinfo = { 0 };
      struct rwa_render *supplied =
         rwa_render_create(device, vbitcode, vbitcode_size, vfunc, bitcode, bitcode_size,
                           func, &shape, &vinfo, why, sizeof(why));
      if (!supplied) {
         printf("air_metal_test: rwa_render_create (%s over %s): %s\n", vfunc, func, why);
         ok = false;
      } else {
         struct rwa_buffer *vbuffers[16] = { 0 };
         unsigned nvbuf = vinfo.vertex_buffers < COUNT_OF(vbuffers)
                             ? vinfo.vertex_buffers : (unsigned)COUNT_OF(vbuffers);
         struct index_picture vhalves[2], vbias;
         struct index_picture *vhalf[2] = { &vhalves[0], &vhalves[1] };

         for (unsigned i = 0; i < nvbuf; i++) {
            vbuffers[i] = rwa_buffer_create(device, sizeof(draw_quad_pos), why,
                                            sizeof(why));
            if (!vbuffers[i]) {
               printf("air_metal_test: rwa_buffer_create (vertex buffer %u): %s\n", i,
                      why);
               return 1;
            }
            fill_vertex_buffer(rwa_buffer_map(vbuffers[i]), 0, 1.0f);
         }
         printf("air_metal_test: and over the supplied vertex stage %s, whose %u buffer%s "
                "hold the four corners:\n", vname, nvbuf, nvbuf == 1 ? "" : "s");
         for (unsigned t = 0; t < 2; t++) {
            struct rwa_draw vd = base;

            vd.vertex.buffers = vbuffers;
            vd.vertex.buffer_count = nvbuf;
            vd.vertex_count = 3;
            vd.index_buffer = i16;
            vd.index_buffer_offset = tri[t].at16;
            if (!index_draw(supplied, &vd, color, color_count, vhalf[t], tri[t].what))
               return 1;
            unsigned diff = index_picture_diff(vhalf[t], half[t]);
            printf("air_metal_test:   %s: %u of %u words differ from the generated "
                   "stage's\n", tri[t].what, diff, TEX * TEX * 4);
            index_print(vhalf[t]);
            if (diff)
               ok = false;
         }
         struct rwa_draw vd = base;

         vd.vertex.buffers = vbuffers;
         vd.vertex.buffer_count = nvbuf;
         vd.vertex_count = 3;
         vd.index_buffer = i16;
         vd.base_vertex = 1;
         if (!index_draw(supplied, &vd, color, color_count, &vbias,
                         "indices [0 1 2] with baseVertex 1")) {
            return 1;
         } else {
            unsigned diff = index_picture_diff(&vbias, vhalf[1]);
            printf("air_metal_test:   baseVertex 1 over [0 1 2]: %u of %u words differ "
                   "from [1 2 3], so [[vertex_id]] %s\n", diff, TEX * TEX * 4,
                   diff ? "did NOT take the base vertex"
                        : "is the index plus the base vertex");
            if (diff)
               ok = false;
         }

         /*
          * And the canary, which this mode needs for the reason --draw-vertex needed
          * one: the three draws above agreed with the generated stage TO THE BIT, and
          * that is also exactly what a run which quietly drew the generated quad anyway
          * would have printed. So the supplied stage's buffers are refilled with the
          * same corners at half the distance from the origin and [0 1 2] drawn once
          * more: a quad of half the size covers only the middle of a 4x4 attachment, so
          * the pixel at (0, 0) must now be CLEAR. If it still came back drawn, nothing
          * above is evidence about the supplied stage -- and the index buffer would have
          * been measured through the generated quad twice.
          */
         for (unsigned i = 0; i < nvbuf; i++)
            fill_vertex_buffer(rwa_buffer_map(vbuffers[i]), 0, 0.5f);
         struct index_picture small;
         struct rwa_draw sd = base;

         sd.vertex.buffers = vbuffers;
         sd.vertex.buffer_count = nvbuf;
         sd.vertex_count = 3;
         sd.index_buffer = i16;
         if (!index_draw(supplied, &sd, color, color_count, &small,
                         "the canary: [0 1 2] over corners at half the distance"))
            return 1;
         printf("air_metal_test:   the canary, the same indices over corners at half the "
                "distance: %u of %u words differ, and (0,0) is %s\n",
                index_picture_diff(&small, vhalf[0]), TEX * TEX * 4,
                index_covered(&small, 0, 0)
                   ? "STILL DRAWN, so the supplied vertex stage is not what put the "
                     "triangle there and nothing above is evidence about it"
                   : "clear, so the vertices came from the shader's own buffer");
         index_print(&small);
         if (index_covered(&small, 0, 0) || index_picture_diff(&small, vhalf[0]) == 0)
            ok = false;

         for (unsigned i = 0; i < nvbuf; i++)
            rwa_buffer_destroy(vbuffers[i]);
         rwa_render_destroy(supplied);
      }
   }

   printf("air_metal_test: %s\n",
          ok ? "INDEXED DRAWING READS THE INDICES -- the permutation, the offset, the "
               "index type, the base vertex and the instance count each changed what came "
               "back"
             : "SOMETHING ABOVE DISAGREES, and an indexed draw that is nearly right draws "
               "a picture of the wrong geometry");

   rwa_depth_stencil_destroy(counter);
   rwa_texture_destroy(zt);
   rwa_buffer_destroy(i16);
   rwa_buffer_destroy(i32);
   rwa_buffer_destroy(past);
   for (unsigned i = 0; i < nbuf; i++)
      rwa_buffer_destroy(buffers[i]);
   rwa_render_destroy(plain);
   rwa_render_destroy(counted);
   for (unsigned i = 0; i < color_count; i++)
      rwa_texture_destroy(color[i]);
   rwa_device_close(device);
   return ok ? 0 : 1;
}
