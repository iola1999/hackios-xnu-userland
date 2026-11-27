/* Attribute sweep, IOSurface, and argument buffer tests, included into air_metal_test.c. */


/*
 * --attrib-sweep: how many of a library's vertex functions get a pipeline out of this
 * contract now, which is the one number on this machine that predicts what the phone's
 * window server will do with the layout.
 *
 * It asks exactly what -newRenderPipelineStateWithDescriptor: asks and nothing more: a
 * vertex module, a fragment module and a descriptor, in and a pipeline out. No draw --
 * there is nothing to check in a picture here, because the descriptor below is not any
 * application's and the buffers are not filled.
 *
 * The descriptor is the same for every module and is deliberately the most permissive
 * one that is still a real layout: all 31 attributes configured as MTLVertexFormatFloat4
 * at offset 0 of a 64-byte element stepping per vertex out of buffer index 0. So an
 * attribute index any module reads is configured, and what the sweep measures is
 * everything else -- the base types, the element order, the buffer index space, the step
 * function, and whether ir3 compiles a vertex stage that was never reached while the
 * contract refused it.
 *
 * What it is NOT: a measurement of the descriptors QuartzCore actually builds. Float4 is
 * the right base type for every attribute in that library (`float4`, `float2`, `float`
 * and `half4` are all floating-point members) and the wrong one for a quarter of
 * Resident Evil 4's, which are integers -- so a module refused here for a base type is
 * being told about this sweep's descriptor and not about its own.
 *
 *   air_metal_test --attrib-sweep VERTEXLIB FRAGMENTLIB FRAGMENTFUNCTION
 */
static int
run_attrib_sweep(const char *vpath, const char *fpath, const char *ffunc)
{
   struct metallib vlib = { 0 }, flib = { 0 };
   char fname[256];
   const uint8_t *fbitcode = NULL;
   size_t fbitcode_size = 0;

   if (!load_metallib(vpath, &vlib) || !load_metallib(fpath, &flib))
      return 1;
   if (!find_function(&flib, ffunc, fname, sizeof(fname), &fbitcode, &fbitcode_size))
      return 1;

   struct metallib_table t;
   if (!parse_functions(&vlib, &t))
      return 1;

   setenv("RWA_SOFTWARE", "1", 0);
   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: --attrib-sweep %s over %s on \"%s\": %u functions\n", vpath,
          fname, rwa_device_name(device), t.count);

   struct rwa_render_shape shape = { .color_count = 4, .sample_count = 1 };
   for (unsigned i = 0; i < shape.color_count; i++)
      shape.color_format[i] = MTL_RGBA32_UINT;
   for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES; i++)
      shape.vertex_attribute[i] = (struct rwa_vertex_attribute){
         .format = MTL_VF_FLOAT4, .offset = 0, .buffer_index = 0 };
   shape.vertex_layout[0] = (struct rwa_vertex_layout){
      .stride = 64, .step_function = MTL_STEP_PER_VERTEX, .step_rate = 1 };

   /*
    * One child per module, and the exit status is the outcome: 0 built with attributes,
    * 3 built without any, 1 refused by name (the child printed the sentence), 2 not a
    * vertex function at all. A child that DIES says something no status could: softpipe
    * asserts on a 16-bit NIR opcode instead of declining it, and a `half` attribute's
    * narrowing is exactly that opcode -- so those modules are measurable on the Adreno
    * and not here, which is the same boundary docs/air.md draws around softpipe.
    */
   enum { SWEPT_ATTRIBS = 0, SWEPT_REFUSED = 1, SWEPT_NOT_VERTEX = 2, SWEPT_PLAIN = 3 };
   unsigned built = 0, fetching = 0, refused = 0, not_vertex = 0, died = 0;
   for (unsigned i = 0; i < t.count; i++) {
      if (!t.bitcode[i] || !t.names[i])
         continue;
      fflush(stdout);
      pid_t pid = fork();
      if (pid < 0) {
         printf("air_metal_test: fork: %s\n", strerror(errno));
         return 1;
      }
      if (pid == 0) {
         struct rwa_render_info info = { 0 };
         int status = SWEPT_PLAIN;

         why[0] = '\0';
         struct rwa_render *r =
            rwa_render_create(device, t.bitcode[i], t.size[i], t.names[i], fbitcode,
                              fbitcode_size, ffunc, &shape, &info, why, sizeof(why));
         if (r && info.vertex_attribute_count) {
            printf("air_metal_test:   %-34s %u element%s, %u stream slot%s, mask 0x%x, "
                   "attributes", t.names[i], info.vertex_attribute_count,
                   info.vertex_attribute_count == 1 ? "" : "s", info.vertex_streams,
                   info.vertex_streams == 1 ? "" : "s", info.vertex_stream_mask);
            for (unsigned k = 0; k < info.vertex_attribute_count; k++)
               printf(" %u", info.vertex_attribute_index[k]);
            printf("\n");
            status = SWEPT_ATTRIBS;
         } else if (!r) {
            status = strstr(why, "takes a vertex function") ? SWEPT_NOT_VERTEX
                                                            : SWEPT_REFUSED;
            if (status == SWEPT_REFUSED)
               printf("air_metal_test:   %-34s REFUSED -- %s\n", t.names[i], why);
         }
         /* _exit and not exit: the pipeline and the device are the parent's as well, and
          * running this child's atexit handlers over them would free the parent's work
          * twice in two processes that share nothing.
          */
         fflush(stdout);
         _exit(status);
      }
      int wstatus = 0;
      if (waitpid(pid, &wstatus, 0) < 0) {
         printf("air_metal_test: waitpid: %s\n", strerror(errno));
         return 1;
      }
      if (!WIFEXITED(wstatus)) {
         died++;
         printf("air_metal_test:   %-34s DIED (signal %d), which on this machine means "
                "softpipe's own assertion and not a refusal of ours\n", t.names[i],
                WIFSIGNALED(wstatus) ? WTERMSIG(wstatus) : 0);
         continue;
      }
      switch (WEXITSTATUS(wstatus)) {
      case SWEPT_ATTRIBS:    built++; fetching++; break;
      case SWEPT_PLAIN:      built++; break;
      case SWEPT_REFUSED:    refused++; break;
      case SWEPT_NOT_VERTEX: not_vertex++; break;
      default:               refused++; break;
      }
   }
   printf("air_metal_test: %u vertex pipelines built, %u of them fetching attributes, %u "
          "refused by name, %u died inside the back end, %u functions are not vertex "
          "functions\n", built, fetching, refused, died, not_vertex);

   rwa_device_close(device);
   free_functions(&t);
   return refused ? 1 : 0;
}

/*
 * --surface: rwa_texture_from_surface, and the refusals it owes.
 *
 * This is the drawable path of the Metal driver, from the library's side (docs/metal.md
 * 10.46): CoreAnimation makes an IOSurface and asks the device for a texture over it, and
 * the one call that can answer is this one. The test exists because the answer is a NULL
 * and a sentence, and a sentence is only worth anything if the right one comes out of the
 * right wrong input.
 *
 * So it sweeps six shapes against one surface ID, and every one but the first is a case
 * this call is supposed to decline BY NAME:
 *
 *   1  a plain 2D texture of the format and geometry a drawable has -- the only one that
 *      can succeed, and on a machine with no GPU it still cannot: the software screen has
 *      no resource_from_handle, and saying so is the refusal under test
 *   2  a mip chain over it           (the import is told one pitch and nothing else)
 *   3  an array of slices over it    (one plane of one image)
 *   4  a multisample type over it    (a sample count the layout would have to agree about)
 *   5  a cube over it                (the same, with six faces)
 *   6  a pitch of zero              (the import takes the pitch from the caller)
 *
 * The surface ID is the caller's, because this machine has no IOSurface at all: on the
 * phone it comes from a surface the caller made, and here any number reaches the refusal
 * that does not depend on there being a surface. An ID of 0 is its own refusal and is
 * checked too, which is why the sweep is run twice when no ID is given.
 */
static int
run_surface(int surface_id, unsigned width, unsigned height, unsigned row_bytes)
{
   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: surface sweep on %s: IOSurface %d, %ux%u, %u bytes a row\n",
          rwa_device_name(device), surface_id, width, height, row_bytes);

   const struct rwa_texture_shape base = {
      .pixel_format = 80 /* MTLPixelFormatBGRA8Unorm */, .type = 2 /* MTLTextureType2D */,
      .width = width, .height = height, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1, .usage = MTL_USAGE_RENDER_TARGET,
   };
   struct {
      const char *what;
      struct rwa_texture_shape shape;
      unsigned row_bytes;
      bool must_refuse;
   } cases[] = {
      { "a drawable's own shape",       base, row_bytes, false },
      { "four mip levels",             base, row_bytes, true },
      { "two array slices",            base, row_bytes, true },
      { "four samples a texel",        base, row_bytes, true },
      { "a cube's six faces",          base, row_bytes, true },
      { "no bytes a row",              base, 0,         true },
   };
   cases[1].shape.mip_levels = 4;
   cases[2].shape.type = 3 /* MTLTextureType2DArray */;
   cases[2].shape.array_length = 2;
   cases[3].shape.type = 4 /* MTLTextureType2DMultisample */;
   cases[3].shape.sample_count = 4;
   cases[4].shape.type = 5 /* MTLTextureTypeCube */;
   cases[4].shape.height = width;

   unsigned wrong = 0;
   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      why[0] = 0;
      struct rwa_texture *texture =
         rwa_texture_from_surface(device, &cases[i].shape, surface_id, 0,
                                  cases[i].row_bytes, why, sizeof(why));
      if (texture) {
         printf("  %-24s MADE a texture over the surface\n", cases[i].what);
         if (cases[i].must_refuse) {
            printf("  ^^ WRONG: that shape cannot be laid over an imported surface and "
                   "should have been refused by name\n");
            wrong++;
         }
         rwa_texture_destroy(texture);
         continue;
      }
      printf("  %-24s refused: %s\n", cases[i].what, why);
      if (!why[0]) {
         printf("  ^^ WRONG: refused without a sentence, which is the one thing a caller "
                "cannot act on\n");
         wrong++;
      }
   }

   /* An ID of zero has a refusal of its own, and it must not be the pitch one: a caller
    * that passes a surface nobody made should be told that, not told about alignment. */
   why[0] = 0;
   if (rwa_texture_from_surface(device, &base, 0, 0, row_bytes, why, sizeof(why))) {
      printf("  %-24s MADE a texture, which is wrong: there is no surface 0\n",
             "surface ID zero");
      wrong++;
   } else {
      printf("  %-24s refused: %s\n", "surface ID zero", why);
      if (!strstr(why, "not a surface")) {
         printf("  ^^ WRONG: that is not the sentence an ID of zero owes\n");
         wrong++;
      }
   }

   rwa_device_close(device);
   printf("air_metal_test: surface sweep: %u answer%s wrong\n", wrong,
          wrong == 1 ? "" : "s");
   return wrong ? 1 : 0;
}

/*
 * --argbufs: what argument buffers a module declares, and the two numbers an
 * application allocates its buffer from.
 *
 * This is the reflection half, and it is printed rather than asserted because the
 * numbers have to be Apple's: -[MTLArgumentEncoder encodedLength] is what the
 * application passes to -newBufferWithLength:, so a row here can be put beside
 * `air.arg_type_size` in the module's own metadata (tools/air/argbuflen.py counts that
 * over a whole library) and the two readings compared.
 *
 *   air_metal_test --argbufs FILE[.metallib|.bc] [FUNCTION]
 */
static int
run_argbufs(const char *path, const char *only)
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

   /* One module, or every function of a container, by the same reader the draw modes
    * use: on the device these bytes come from -[_MTLFunction bitcodeData].
    */
   struct metallib_table t = { 0 };
   bool container = lib.size >= 4 && memcmp(lib.bytes, "MTLB", 4) == 0;
   if (container && !parse_functions(&lib, &t)) {
      rwa_device_close(device);
      return 1;
   }
   unsigned count = container ? t.count : 1;
   int rc = 0;

   for (unsigned i = 0; i < count; i++) {
      const char *name = container ? t.names[i] : "(the only module)";
      const uint8_t *bitcode = container ? t.bitcode[i] : lib.bytes;
      size_t size = container ? t.size[i] : lib.size;

      if (!bitcode || (only && name && strcmp(only, name)))
         continue;

      struct rwa_argbuf_layout sets[4];
      unsigned nsets = 0;
      if (rwa_module_argbufs(device, bitcode, size, container ? name : NULL, sets,
                             COUNT_OF(sets), &nsets, why, sizeof(why)) != 0) {
         printf("argbufs %s  refused: %s\n", name, why);
         rc = 1;
         continue;
      }
      if (nsets == 0) {
         printf("argbufs %s  none\n", name);
         continue;
      }
      for (unsigned k = 0; k < nsets; k++) {
         const struct rwa_argbuf_layout *l = &sets[k];

         printf("argbufs %s  set at [[buffer(%u)]]  encodedLength %u  alignment %u  "
                "%u members\n", name, l->metal_index, l->encoded_length, l->alignment,
                l->member_count);
         for (unsigned m = 0; m < l->member_count; m++) {
            static const char *const kind[] = { "buffer", "texture", "sampler" };

            printf("argbufs %s    id %u  %s  index %u%s  %s\n", name, l->members[m].id,
                   l->members[m].kind < 3 ? kind[l->members[m].kind] : "?",
                   l->members[m].index, l->members[m].write ? "  written" : "",
                   l->members[m].name);
         }
      }
   }

   if (container)
      free_functions(&t);
   rwa_device_close(device);
   return rc;
}

/*
 * --draw-argbuf: the same picture through an argument buffer as through direct
 * arguments, and a NAMED DIFFERENT picture for every record that is deliberately wrong.
 *
 * What this measures and why it is shaped this way:
 *
 *   - THE DIFFERENTIAL. tools/air/argbuf_set.ll and tools/air/argbuf_direct.ll compute
 *     the same four floats out of the same five resources; the only difference is that
 *     one reaches them through `air.indirect_buffer` members and the other through
 *     [[buffer(0)]], [[sampler(0)]], [[texture(0)]], [[texture(1)]], [[sampler(1)]].
 *     So "the encoder works" needs no oracle: the two draws must produce the same
 *     pixel, and the direct draw is a path this library has drawn 255 shaders through
 *     (docs/air.md section 5).
 *   - THE WRONG RECORDS. A differential that only ever passes says nothing about what
 *     would have failed, so every member is also encoded WRONG once -- the two samplers
 *     exchanged, the two textures exchanged, the buffer member at another offset -- and
 *     each has a picture of its own written out below. Those are the two gaps the
 *     frontend's own check could not close: air_run binds one sampler state to every
 *     slot, so a sampler member's index was invisible there, and softpipe's COMPUTE path
 *     answers every texture from the view bound at slot 0, so a texture member's index
 *     was invisible as well (argbufcheck.py says both in as many words). A fragment
 *     shader on the same softpipe distinguishes them, and R and A of one pixel are the
 *     same texture through two samplers, which is what makes a swap visible at all.
 *   - THE REFUSALS. An argument buffer nobody encoded, a member nobody set, a member set
 *     with the wrong call, no buffer at the set's index at all, a resource destroyed
 *     after it was encoded, and an offset this GPU cannot fetch from. Each must come
 *     back with the sentence this feature was written for and not with a picture.
 *
 * One buffer holds FOUR encodings at four offsets here, and they are drawn in turn: that
 * is Metal's own object model (an application allocates one argument buffer and encodes
 * many sets into it at multiples of encodedLength) and the reason the records are keyed
 * by offset rather than kept on the encoder.
 *
 *   air_metal_test --draw-argbuf SETMODULE DIRECTMODULE
 */

/* The four floats of one pixel, and what they mean (argbuf_set.ll):
 *   [0] texture member 2 sampled through sampler member 1 (nearest)
 *   [1] texture member 3 sampled through sampler member 4 (linear)
 *   [2] the float at the start of buffer member 0, wherever it was encoded from
 *   [3] texture member 2 FETCHED at texel (2, 3), with no sampler at all
 *
 * With air_run's texel pattern -- channel 0 of texel (x, y) of the texture at slot s is
 * 100 + x + 1000 * s -- the reference picture is therefore
 *
 *   101    nearest at (0.3125, 0.375) of slot 0 is texel (1, 1)
 *   1100.75  linear there is a quarter of texel (0, 1) and three quarters of (1, 1),
 *            of slot 1
 *   7.5    the buffer
 *   102    texel (2, 3) of slot 0
 *
 * and three of those four are exact whatever the filter weights are, which is why they
 * are checked against the arithmetic below and the linear one is only bounded.
 */
struct argbuf_pixel {
   float c[4];
};

static bool
argbuf_pixel_eq(const struct argbuf_pixel *a, const struct argbuf_pixel *b)
{
   for (unsigned i = 0; i < 4; i++)
      if (a->c[i] != b->c[i])
         return false;
   return true;
}

static void
argbuf_print_pixel(const char *what, const struct argbuf_pixel *p)
{
   printf("draw-argbuf: %-34s %.9g %.9g %.9g %.9g\n", what, (double)p->c[0],
          (double)p->c[1], (double)p->c[2], (double)p->c[3]);
}

/* One draw and the pixel it left, read at two corners: the shader writes the same value
 * at every fragment (it takes no varying at all), so two corners that disagree would be
 * a rasterisation or readback fault rather than anything about a binding, and this says
 * so instead of reading one pixel and trusting it.
 */
static bool
argbuf_draw_pixel(struct rwa_render *render, const struct rwa_draw *draw,
                  struct rwa_texture *attachment, struct argbuf_pixel *out, char *why,
                  size_t why_size)
{
   if (rwa_render_draw(render, draw, why, why_size) != 0)
      return false;

   const unsigned one[3] = { 1, 1, 1 };
   const unsigned corner[2][3] = { { 0, 0, 0 }, { TEX - 1, TEX - 1, 0 } };
   struct argbuf_pixel got[2] = { { { 0 } } };

   for (unsigned k = 0; k < 2; k++) {
      if (rwa_texture_read(attachment, 0, 0, corner[k], one, got[k].c, 0, why,
                           why_size) != 0)
         return false;
   }
   if (!argbuf_pixel_eq(&got[0], &got[1])) {
      snprintf(why, why_size, "the two corners of the attachment came back different "
                              "(%.9g... and %.9g...)", (double)got[0].c[0],
               (double)got[1].c[0]);
      return false;
   }
   *out = got[0];
   return true;
}

static int
run_draw_argbuf(const char *setpath, const char *directpath)
{
   struct metallib setlib = { 0 }, directlib = { 0 };
   if (!load_metallib(setpath, &setlib) || !load_metallib(directpath, &directlib))
      return 1;

   char setname[256], directname[256];
   const uint8_t *setbits = NULL, *directbits = NULL;
   size_t setsize = 0, directsize = 0;
   if (!find_function(&setlib, NULL, setname, sizeof(setname), &setbits, &setsize) ||
       !find_function(&directlib, NULL, directname, sizeof(directname), &directbits,
                      &directsize))
      return 1;

   setenv("RWA_SOFTWARE", "1", 0);
   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("draw-argbuf: device is \"%s\"\n", rwa_device_name(device));

   int failed = 0, checks = 0;

   /* The set's layout, which is what an application would allocate its buffer from.
    * Checked against the module this test wrote: five members, 8 bytes each, and the
    * flat indices the frontend allocates in (set, member) order per kind.
    */
   struct rwa_argbuf_layout layout[2];
   unsigned nsets = 0;
   if (rwa_module_argbufs(device, setbits, setsize, NULL, layout, COUNT_OF(layout),
                          &nsets, why, sizeof(why)) != 0) {
      printf("draw-argbuf: rwa_module_argbufs: %s\n", why);
      rwa_device_close(device);
      return 1;
   }
   static const struct { unsigned id, kind, index; } want_member[] = {
      { 0, RWA_ARGBUF_BUFFER, 0 }, { 1, RWA_ARGBUF_SAMPLER, 0 },
      { 2, RWA_ARGBUF_TEXTURE, 0 }, { 3, RWA_ARGBUF_TEXTURE, 1 },
      { 4, RWA_ARGBUF_SAMPLER, 1 },
   };
   bool layout_ok = nsets == 1 && layout[0].metal_index == 0 &&
                    layout[0].encoded_length == 8 * COUNT_OF(want_member) &&
                    layout[0].alignment == 8 &&
                    layout[0].member_count == COUNT_OF(want_member);
   for (unsigned i = 0; layout_ok && i < COUNT_OF(want_member); i++)
      layout_ok = layout[0].members[i].id == want_member[i].id &&
                  layout[0].members[i].kind == want_member[i].kind &&
                  layout[0].members[i].index == want_member[i].index;
   printf("draw-argbuf: %s: %u set%s, encodedLength %u, alignment %u, %u members -- %s\n",
          layout_ok ? "ok" : "FAIL", nsets, nsets == 1 ? "" : "s",
          nsets ? layout[0].encoded_length : 0, nsets ? layout[0].alignment : 0,
          nsets ? layout[0].member_count : 0,
          layout_ok ? "the five members this module declares, at the flat indices "
                      "(buffer 0; samplers 0, 1; textures 0, 1) the frontend allocates"
                    : "NOT what tools/air/argbuf_set.ll declares");
   failed += !layout_ok;
   checks++;

   /* And the control module declares none, which is the other half of that answer: a
    * module without an `air.indirect_buffer` is 0 sets and not an error.
    */
   unsigned ndirect = 99;
   bool direct_none = rwa_module_argbufs(device, directbits, directsize, NULL, layout,
                                         COUNT_OF(layout), &ndirect, why,
                                         sizeof(why)) == 0 && ndirect == 0;
   printf("draw-argbuf: %s: the direct module declares %u argument buffers\n",
          direct_none ? "ok" : "FAIL", ndirect);
   failed += !direct_none;
   checks++;

   /*
    * The rig, shared by every draw below so that nothing but the binding differs: one
    * RGBA32Float attachment, two textures with air_run's texel pattern (slot 0 and slot
    * 1, which differ by 1000 in channel 0 -- that is what makes a texture swap visible),
    * two samplers that differ in their filter, and one buffer holding two floats 64
    * bytes apart.
    */
   const unsigned origin[3] = { 0, 0, 0 };
   const struct rwa_texture_shape ats = {
      .pixel_format = MTL_RGBA32_FLOAT, .type = MTL_TEXTURE_TYPE_2D,
      .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1, .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
   };
   struct rwa_texture *attachment = rwa_texture_create(device, &ats, why, sizeof(why));
   if (!attachment) {
      printf("draw-argbuf: rwa_texture_create (attachment): %s\n", why);
      rwa_device_close(device);
      return 1;
   }

   struct rwa_texture *tex[2] = { NULL, NULL };
   for (unsigned i = 0; i < 2; i++) {
      const struct rwa_texture_shape ts = {
         .pixel_format = MTL_RGBA32_FLOAT, .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1, .usage = MTL_USAGE_SHADER_READ,
      };
      tex[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!tex[i]) {
         printf("draw-argbuf: rwa_texture_create (texture %u): %s\n", i, why);
         return 1;
      }
      static float texels[TEXELS];
      memset(texels, 0, sizeof(texels));
      for (unsigned y = 0; y < TEX; y++)
         for (unsigned x = 0; x < TEX; x++) {
            float v[4];
            texel_value(i, x, y, 0, v);
            for (unsigned c = 0; c < 4; c++)
               texels[texel_index(x, y, 0) + c] = v[c];
         }
      const unsigned extent[3] = { TEX, TEX, 1 };
      if (rwa_texture_write(tex[i], 0, 0, origin, extent, texels, 0, why,
                            sizeof(why)) != 0) {
         printf("draw-argbuf: rwa_texture_write (texture %u): %s\n", i, why);
         return 1;
      }
   }

   /* Nearest and linear, and the coordinate the shader samples at is between two texels
    * on purpose: nearest there is texel (1, 1) and linear is a weighted sum, so the two
    * states cannot be confused for one another. Everything else about them is equal.
    */
   struct rwa_sampler *samp[2] = { NULL, NULL };
   for (unsigned i = 0; i < 2; i++) {
      const struct rwa_sampler_state state = {
         .address = { 0, 0, 0 },                 /* ClampToEdge */
         .min_filter = i, .mag_filter = i,       /* 0 Nearest, 1 Linear */
         .mip_filter = 0, .compare_function = 0, .max_anisotropy = 1,
         .normalized_coordinates = 1, .lod_min = 0.0f, .lod_max = 65504.0f,
      };
      samp[i] = rwa_sampler_create(device, &state, why, sizeof(why));
      if (!samp[i]) {
         printf("draw-argbuf: rwa_sampler_create (sampler %u): %s\n", i, why);
         return 1;
      }
   }

   /* The buffer the shader's member 0 points at: 7.5 at offset 0 and 9.25 at offset 64.
    * 64 and not 16, so that the same encoding is legal on the Adreno as well -- a6xx
    * fetches a UBO at multiples of 64 and softpipe at multiples of 16, and the point of
    * the offset case is the offset reaching the GPU rather than an alignment refusal.
    */
   struct rwa_buffer *values = rwa_buffer_create(device, 2048, why, sizeof(why));
   /* ... and the argument buffer itself, four encodings of 40 bytes at 64-byte spacing */
   struct rwa_buffer *argbuf = rwa_buffer_create(device, 512, why, sizeof(why));
   struct rwa_buffer *unencoded = rwa_buffer_create(device, 512, why, sizeof(why));
   if (!values || !argbuf || !unencoded) {
      printf("draw-argbuf: rwa_buffer_create: %s\n", why);
      return 1;
   }
   {
      float *f = rwa_buffer_map(values);
      memset(f, 0, rwa_buffer_size(values));
      f[0] = 7.5f;
      f[1] = 3.25f;       /* one float in = byte 4, which no GPU here can fetch from */
      f[16] = 9.25f;      /* 16 floats in = byte 64, which both can */
      /* and byte 1164, which is the offset the window server of this port binds its
       * fragment buffer 2 at, 55 times in one boot: 1164 is a multiple of 4 (Metal's own
       * rule) and of neither 16 nor 64, so it is the uploaded path at its own number */
      f[291] = 5.75f;
      /* The unencoded buffer is not left as zeros: zeros would be a handle of zero,
       * which is what an application that forgot to encode would have, and this test
       * must not pass because the bytes happened to be unreadable. These are handles
       * that LOOK like handles.
       */
      uint64_t *h = rwa_buffer_map(unencoded);
      for (unsigned i = 0; i < rwa_buffer_size(unencoded) / 8; i++)
         h[i] = 0x0000000100000000ull + i * 0x40;
   }

   /* The two pipelines. Neither is given a vertex module, so the library supplies its
    * quad: these shaders read no varying at all, which is what makes every fragment's
    * value the same and a pixel comparable with ==.
    */
   struct rwa_render_shape shape = { .color_count = 1, .sample_count = 1,
                                     .color_format[0] = MTL_RGBA32_FLOAT };
   struct rwa_render_info setinfo = { 0 }, directinfo = { 0 };
   struct rwa_render *setpipe =
      rwa_render_create(device, NULL, 0, NULL, setbits, setsize, NULL, &shape, &setinfo,
                        why, sizeof(why));
   if (!setpipe) {
      printf("draw-argbuf: rwa_render_create (the set module): %s\n", why);
      return 1;
   }
   struct rwa_render *directpipe =
      rwa_render_create(device, NULL, 0, NULL, directbits, directsize, NULL, &shape,
                        &directinfo, why, sizeof(why));
   if (!directpipe) {
      printf("draw-argbuf: rwa_render_create (the direct module): %s\n", why);
      return 1;
   }
   bool mask_ok = setinfo.fragment_argbuf_mask == 1u &&
                  directinfo.fragment_argbuf_mask == 0u;
   printf("draw-argbuf: %s: the set pipeline reports fragment_argbuf_mask %#x and the "
          "direct one %#x\n", mask_ok ? "ok" : "FAIL",
          setinfo.fragment_argbuf_mask, directinfo.fragment_argbuf_mask);
   failed += !mask_ok;
   checks++;

   /* THE DIRECT DRAW, which is the reference every encoded draw is compared against. */
   struct rwa_buffer *const direct_buffers[] = { values };
   struct rwa_texture *const direct_textures[] = { tex[0], tex[1] };
   struct rwa_sampler *const direct_samplers[] = { samp[0], samp[1] };
   const struct rwa_draw direct_draw = {
      .color = (struct rwa_texture *const[]){ attachment }, .color_count = 1,
      .clear_color = (const float[]){ 0, 0, 0, 0 },
      .fragment = { .buffers = direct_buffers, .buffer_count = 1,
                    .textures = direct_textures, .texture_count = 2,
                    .samplers = direct_samplers, .sampler_count = 2 },
      .primitive = 4, .vertex_start = 0, .vertex_count = 4,
   };
   struct argbuf_pixel reference;
   if (!argbuf_draw_pixel(directpipe, &direct_draw, attachment, &reference, why,
                          sizeof(why))) {
      printf("draw-argbuf: the direct draw failed: %s\n", why);
      return 1;
   }
   argbuf_print_pixel("direct, the reference:", &reference);

   /*
    * THE ENCODED DRAW. Nothing is in draw.fragment at all: the five resources reach the
    * shader only through the records, so a lookup that failed would draw nothing rather
    * than draw this.
    */
   struct rwa_buffer_bind at0[1] = { { argbuf, 0 } };
   struct rwa_draw encoded_draw = direct_draw;
   encoded_draw.fragment = (struct rwa_bindings){ 0 };
   encoded_draw.fragment_buffer_table = at0;
   encoded_draw.fragment_buffer_table_count = 1;

   bool enc = rwa_argbuf_encode_buffer(argbuf, 0, 0, values, 0, why, sizeof(why)) == 0 &&
              rwa_argbuf_encode_sampler(argbuf, 0, 1, samp[0], why, sizeof(why)) == 0 &&
              rwa_argbuf_encode_texture(argbuf, 0, 2, tex[0], why, sizeof(why)) == 0 &&
              rwa_argbuf_encode_texture(argbuf, 0, 3, tex[1], why, sizeof(why)) == 0 &&
              rwa_argbuf_encode_sampler(argbuf, 0, 4, samp[1], why, sizeof(why)) == 0;
   if (!enc) {
      printf("draw-argbuf: encoding at offset 0: %s\n", why);
      return 1;
   }
   printf("draw-argbuf: %u members recorded at offset 0 of the argument buffer\n",
          rwa_argbuf_encoded_members(argbuf, 0));

   struct argbuf_pixel through;
   if (!argbuf_draw_pixel(setpipe, &encoded_draw, attachment, &through, why,
                          sizeof(why))) {
      printf("draw-argbuf: FAIL: the encoded draw failed: %s\n", why);
      failed += 2;
      checks += 2;
   } else {
      argbuf_print_pixel("through the encoder:", &through);
      bool same = argbuf_pixel_eq(&through, &reference);
      printf("draw-argbuf: %s: the same pixel through an argument buffer as through "
             "direct arguments\n", same ? "ok" : "FAIL");
      failed += !same;
      checks++;

      /* And the reference is what the texel pattern says it should be, so that "the two
       * agree" cannot be two draws agreeing on the same wrong resource. Three channels
       * are exact arithmetic; the linear one is only bounded, because the weights are
       * the rasteriser's and this test is not about them.
       */
      bool arith = through.c[0] == 101.0f && through.c[3] == 102.0f &&
                   through.c[2] == 7.5f &&
                   through.c[1] > 1100.0f && through.c[1] < 1101.0f;
      printf("draw-argbuf: %s: and it is the picture the texel pattern names -- texel "
             "(1,1) of texture 0 sampled, texel (2,3) of texture 0 fetched, 7.5 out of "
             "the buffer, and texture 1 filtered between two of its texels\n",
             arith ? "ok" : "FAIL");
      failed += !arith;
      checks++;

      /* All four different, which is what makes every comparison below worth anything:
       * a picture whose channels agree would read the same with two members exchanged.
       */
      bool distinct = through.c[0] != through.c[1] && through.c[0] != through.c[2] &&
                      through.c[0] != through.c[3] && through.c[1] != through.c[2] &&
                      through.c[1] != through.c[3] && through.c[2] != through.c[3];
      printf("draw-argbuf: %s: the four channels are four different numbers (%s)\n",
             distinct ? "ok" : "FAIL",
             distinct ? "so exchanging any two members has to show"
                      : "two channels agree, so a swap would be invisible");
      failed += !distinct;
      checks++;
   }

   /*
    * THE SAMPLERS EXCHANGED, at offset 64 of the same buffer -- a second encoding in one
    * buffer, which is what the records are keyed by offset for.
    *
    * This is the gap argbufcheck.py could not close: air_run binds one sampler state to
    * every slot, so nothing there could tell sampler member 1 from member 4. Here the
    * two states differ and the picture has both in it, so exchanging the records must
    * exchange R and A and change nothing else.
    */
   if (rwa_argbuf_encode_buffer(argbuf, 64, 0, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 64, 1, samp[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 64, 2, tex[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 64, 3, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 64, 4, samp[0], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 64: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at64[1] = { { argbuf, 64 } };
   struct rwa_draw swapped_samplers = encoded_draw;
   swapped_samplers.fragment_buffer_table = at64;

   struct argbuf_pixel got;
   if (!argbuf_draw_pixel(setpipe, &swapped_samplers, attachment, &got, why,
                          sizeof(why))) {
      printf("draw-argbuf: FAIL: the draw with the samplers exchanged failed: %s\n", why);
      failed++;
      checks++;
   } else {
      argbuf_print_pixel("samplers exchanged:", &got);
      /* R is texture 0 through what was the linear sampler, so it is the linear number
       * of slot 0, which is G minus its 1000; G is texture 1 through nearest, which is
       * R plus 1000. The FETCH does not move at all, because it names no sampler -- and
       * that is the half of this that says a sampler record cannot disturb a texture
       * binding.
       */
      bool as_named = got.c[0] == through.c[1] - 1000.0f &&
                      got.c[1] == through.c[0] + 1000.0f &&
                      got.c[2] == through.c[2] && got.c[3] == through.c[3];
      printf("draw-argbuf: %s: exchanging sampler members 1 and 4 exchanges the two "
             "FILTERS and leaves the fetch alone -- so a sampler member is bound at ITS "
             "OWN index\n", as_named ? "ok" : "FAIL");
      failed += !as_named;
      checks++;
   }

   /*
    * THE TEXTURES EXCHANGED, at offset 128. The other gap: softpipe's compute path
    * answers every texture from the view bound at slot 0, so a texture member's index
    * was invisible to argbufcheck.py's dispatch. A fragment shader distinguishes them,
    * and the two textures differ by 1000 in channel 0, so the picture says which
    * texture answered which channel.
    */
   if (rwa_argbuf_encode_buffer(argbuf, 128, 0, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 128, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 128, 2, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 128, 3, tex[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 128, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 128: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at128[1] = { { argbuf, 128 } };
   struct rwa_draw swapped_textures = encoded_draw;
   swapped_textures.fragment_buffer_table = at128;

   if (!argbuf_draw_pixel(setpipe, &swapped_textures, attachment, &got, why,
                          sizeof(why))) {
      printf("draw-argbuf: FAIL: the draw with the textures exchanged failed: %s\n", why);
      failed++;
      checks++;
   } else {
      argbuf_print_pixel("textures exchanged:", &got);
      /* Channel 0 of texel (x, y) of slot s is 100 + x + 1000 * s, so exchanging the two
       * textures moves exactly 1000 through every channel that names one: R and A gain
       * it (they were slot 0 and are now slot 1) and G loses it. The buffer does not
       * move. This picture is also unlike the samplers-exchanged one above in A, so the
       * two mistakes cannot be confused for each other.
       */
      bool as_named = got.c[0] == through.c[0] + 1000.0f &&
                      got.c[1] == through.c[1] - 1000.0f &&
                      got.c[2] == through.c[2] &&
                      got.c[3] == through.c[3] + 1000.0f;
      printf("draw-argbuf: %s: exchanging texture members 2 and 3 moves 1000 through "
             "every channel that names a texture, the sampled and the fetched alike -- "
             "so a texture member is bound at ITS OWN index\n",
             as_named ? "ok" : "FAIL");
      failed += !as_named;
      checks++;
   }

   /*
    * THE BUFFER MEMBER AT ANOTHER OFFSET, at encoding offset 192. Metal's own `offset:`
    * on -setBuffer:offset:atIndex:, which is the one buffer offset this contract takes
    * at all, and the only channel that may move is B.
    */
   if (rwa_argbuf_encode_buffer(argbuf, 192, 0, values, 64, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 192, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 192, 2, tex[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 192, 3, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 192, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 192: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at192[1] = { { argbuf, 192 } };
   struct rwa_draw moved_buffer = encoded_draw;
   moved_buffer.fragment_buffer_table = at192;

   if (!argbuf_draw_pixel(setpipe, &moved_buffer, attachment, &got, why, sizeof(why))) {
      printf("draw-argbuf: FAIL: the draw with the buffer member at offset 64 failed: "
             "%s\n", why);
      failed++;
      checks++;
   } else {
      argbuf_print_pixel("buffer member at offset 64:", &got);
      bool as_named = got.c[2] == 9.25f && through.c[2] == 7.5f &&
                      got.c[0] == through.c[0] && got.c[1] == through.c[1] &&
                      got.c[3] == through.c[3];
      printf("draw-argbuf: %s: the buffer member's own offset reaches the GPU (B is "
             "9.25 against 7.5) and no other channel moves\n",
             as_named ? "ok" : "FAIL");
      failed += !as_named;
      checks++;
   }

   /* And all four encodings are still there, in one buffer, each with its five members:
    * the draws above each found their own and none of them cleared another.
    */
   bool kept = rwa_argbuf_encoded_members(argbuf, 0) == 5 &&
               rwa_argbuf_encoded_members(argbuf, 64) == 5 &&
               rwa_argbuf_encoded_members(argbuf, 128) == 5 &&
               rwa_argbuf_encoded_members(argbuf, 192) == 5;
   printf("draw-argbuf: %s: one buffer holds four encodings at four offsets, five "
          "members each\n", kept ? "ok" : "FAIL");
   failed += !kept;
   checks++;

   /*
    * AN OFFSET THIS GPU CANNOT FETCH FROM, served and not refused -- 4 bytes, which is
    * legal in Metal and a multiple of neither softpipe's 16 nor a6xx's 64. The library
    * binds it as a `user_buffer`, which the back end uploads itself, and the only channel
    * that may move is B: 3.25 is the float four bytes into the buffer.
    */
   if (rwa_argbuf_encode_buffer(argbuf, 384, 0, values, 4, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 384, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 384, 2, tex[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 384, 3, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 384, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 384: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at384[1] = { { argbuf, 384 } };
   struct rwa_draw unaligned_member = encoded_draw;
   unaligned_member.fragment_buffer_table = at384;

   if (!argbuf_draw_pixel(setpipe, &unaligned_member, attachment, &got, why,
                          sizeof(why))) {
      printf("draw-argbuf: FAIL: the draw with the buffer member at offset 4 failed: "
             "%s\n", why);
      failed++;
   } else {
      argbuf_print_pixel("buffer member at offset 4:", &got);
      bool as_named = got.c[2] == 3.25f && got.c[0] == through.c[0] &&
                      got.c[1] == through.c[1] && got.c[3] == through.c[3];
      printf("draw-argbuf: %s: an offset this GPU cannot fetch from is uploaded instead "
             "of refused (B is 3.25, the float four bytes in)\n",
             as_named ? "ok" : "FAIL");
      failed += !as_named;
   }
   checks++;

   /*
    * AND THE SAME TWO OFFSETS ON A DIRECT BUFFER ARGUMENT, which is the window server's
    * own case rather than an argument buffer's: 55 bindings of one boot arrived as
    * "fragment buffer 2 at offset 1164" and every one of them was a refused binding
    * before this. The direct pipeline is drawn with its one `constant` argument bound at
    * 64 (which this GPU can fetch from) and at 4 (which it cannot), and the answer must
    * be the same two floats the member path gave.
    */
   {
      struct rwa_buffer_bind direct_table[1];
      struct rwa_draw moved_direct = direct_draw;
      static const struct { size_t offset; float want; const char *what; } cases[] = {
         { 64, 9.25f, "a direct buffer argument at offset 64 (aligned: no copy)" },
         { 4, 3.25f, "a direct buffer argument at offset 4 (uploaded)" },
         { 1164, 5.75f, "a direct buffer argument at offset 1164, which is the window "
                        "server's own (uploaded)" },
      };

      for (unsigned i = 0; i < COUNT_OF(cases); i++) {
         direct_table[0] = (struct rwa_buffer_bind){ values, cases[i].offset };
         moved_direct.fragment_buffer_table = direct_table;
         moved_direct.fragment_buffer_table_count = 1;
         if (!argbuf_draw_pixel(directpipe, &moved_direct, attachment, &got, why,
                                sizeof(why))) {
            printf("draw-argbuf: FAIL: %s: %s\n", cases[i].what, why);
            failed++;
         } else {
            bool as_named = got.c[2] == cases[i].want &&
                            got.c[0] == reference.c[0] && got.c[1] == reference.c[1] &&
                            got.c[3] == reference.c[3];
            printf("draw-argbuf: %s: %s reads %.9g and moves nothing else\n",
                   as_named ? "ok" : "FAIL", cases[i].what, (double)got.c[2]);
            failed += !as_named;
         }
         checks++;
      }
   }

   /*
    * THE REFUSALS. Each is a draw that must come back with the sentence, and the test is
    * on the WORDS and not merely on the failure: a refusal for another reason would be a
    * pass for the wrong reason, which this project has paid for before.
    */
   struct {
      const char *what;
      const char *words;
      struct rwa_draw draw;
   } refusal[6];
   unsigned nrefusal = 0;

   /* 1. an argument buffer nobody encoded -- the refusal this feature is mostly made of */
   struct rwa_buffer_bind at_unencoded[1] = { { unencoded, 0 } };
   refusal[nrefusal].what = "an argument buffer nobody encoded";
   refusal[nrefusal].words = "was never filled through an MTLArgumentEncoder";
   refusal[nrefusal].draw = encoded_draw;
   refusal[nrefusal].draw.fragment_buffer_table = at_unencoded;
   nrefusal++;

   /* 2. nothing bound at the set's Metal index at all */
   refusal[nrefusal].what = "no buffer at the set's own index";
   refusal[nrefusal].words = "nothing is bound there";
   refusal[nrefusal].draw = encoded_draw;
   refusal[nrefusal].draw.fragment_buffer_table = NULL;
   refusal[nrefusal].draw.fragment_buffer_table_count = 0;
   nrefusal++;

   /* 3. an encoding with one member missing: id 3 is never set at offset 256 */
   if (rwa_argbuf_encode_buffer(argbuf, 256, 0, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 256, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 256, 2, tex[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 256, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 256: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at256[1] = { { argbuf, 256 } };
   refusal[nrefusal].what = "a member nobody set";
   refusal[nrefusal].words = "never set on the encoder";
   refusal[nrefusal].draw = encoded_draw;
   refusal[nrefusal].draw.fragment_buffer_table = at256;
   nrefusal++;

   /* 4. a member set with the wrong call: a buffer where the shader declares a texture */
   if (rwa_argbuf_encode_buffer(argbuf, 320, 0, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 320, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_buffer(argbuf, 320, 2, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 320, 3, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 320, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 320: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at320[1] = { { argbuf, 320 } };
   refusal[nrefusal].what = "a buffer encoded where a texture is declared";
   refusal[nrefusal].words = "and a buffer was encoded there";
   refusal[nrefusal].draw = encoded_draw;
   refusal[nrefusal].draw.fragment_buffer_table = at320;
   nrefusal++;

   for (unsigned i = 0; i < nrefusal; i++) {
      why[0] = '\0';
      int rc = rwa_render_draw(setpipe, &refusal[i].draw, why, sizeof(why));
      bool named = rc != 0 && strstr(why, refusal[i].words) != NULL;

      printf("draw-argbuf: %s: %s -- %s\n", named ? "ok" : "FAIL", refusal[i].what,
             rc == 0 ? "DREW A PICTURE instead of refusing" : why);
      failed += !named;
      checks++;
   }

   /*
    * And the last one, which has to be last because it destroys a resource: a texture
    * encoded and then destroyed. Metal would leave a stale handle for the GPU to read;
    * here the record holds a pointer this library would dereference, so the destroy
    * scrubs it and the draw names the member.
    */
   struct rwa_texture *doomed = rwa_texture_create(device, &(struct rwa_texture_shape){
      .pixel_format = MTL_RGBA32_FLOAT, .type = MTL_TEXTURE_TYPE_2D,
      .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1, .usage = MTL_USAGE_SHADER_READ }, why, sizeof(why));
   if (!doomed) {
      printf("draw-argbuf: rwa_texture_create (the doomed texture): %s\n", why);
      return 1;
   }
   if (rwa_argbuf_encode_buffer(argbuf, 448, 0, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 448, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 448, 2, doomed, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 448, 3, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 448, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 448: %s\n", why);
      return 1;
   }
   rwa_texture_destroy(doomed);
   struct rwa_buffer_bind at448[1] = { { argbuf, 448 } };
   struct rwa_draw after_destroy = encoded_draw;
   after_destroy.fragment_buffer_table = at448;
   why[0] = '\0';
   int rc = rwa_render_draw(setpipe, &after_destroy, why, sizeof(why));
   bool named = rc != 0 && strstr(why, "has been destroyed since") != NULL;
   printf("draw-argbuf: %s: a texture destroyed after it was encoded -- %s\n",
          named ? "ok" : "FAIL",
          rc == 0 ? "DREW A PICTURE instead of refusing" : why);
   failed += !named;
   checks++;

   /* And the scrub took out exactly one member: the encoding at offset 0 still draws
    * what it drew before, with the texture that was NOT destroyed. A scrub that cleared
    * too much would refuse this, and one that cleared too little would have crashed
    * above.
    */
   if (!argbuf_draw_pixel(setpipe, &encoded_draw, attachment, &got, why, sizeof(why))) {
      printf("draw-argbuf: FAIL: the first encoding no longer draws: %s\n", why);
      failed++;
   } else {
      bool unchanged = argbuf_pixel_eq(&got, &through);
      printf("draw-argbuf: %s: the other encodings are untouched by that destroy\n",
             unchanged ? "ok" : "FAIL");
      failed += !unchanged;
   }
   checks++;

   printf("draw-argbuf: %d of %d checks failed\n", failed, checks);

   rwa_render_destroy(setpipe);
   rwa_render_destroy(directpipe);
   rwa_buffer_destroy(argbuf);
   rwa_buffer_destroy(unencoded);
   rwa_buffer_destroy(values);
   for (unsigned i = 0; i < 2; i++) {
      rwa_texture_destroy(tex[i]);
      rwa_sampler_destroy(samp[i]);
   }
   rwa_texture_destroy(attachment);
   rwa_device_close(device);
   return failed ? 1 : 0;
}
