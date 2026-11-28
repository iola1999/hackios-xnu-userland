/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * air_metal_test: the library of air_metal.h, driven the way the Metal driver will
 * drive it, on this machine and without the phone.
 *
 * It exists to answer one question that air_run cannot: does a caller holding nothing
 * but air_metal.h and a function's bitcode get a pipeline and a dispatch out of it?
 * air_run is linked against the whole frontend and reaches into air_shader_info for
 * every binding it makes; this one knows only air_metal.h and calls every one of its
 * functions, so anything the contract fails to provide shows up here as code that
 * cannot be written.
 *
 * The textures were where that bit hardest, and the gap was worth naming: the contract
 * reported how many textures a kernel binds and not what shape or format each one wants,
 * because a driver never needs to ask -- the application's MTLTextureDescriptor says.
 * So this program picks, per slot, out of the third argument (a 4x4 2D or 4x4x4 3D
 * texture of one of the three 32-bit RGBA formats), and the library's refusal names
 * what the shader wanted when the pick is wrong: "reads texture 0 as a texture2d<uint>
 * and MTLPixelFormatRGBA32Float is a floating-point or normalised format" is this test
 * being told what to pass next, and every spelling in the lines below was found that
 * way.
 *
 * The gap is now closed from the other side as well: rwa_module_textures answers what a
 * module declares, because a test standing in for an application has no descriptor to
 * read and Metal's own pipeline reflection has to report the same thing. --textures
 * prints that answer for a whole library, so a caller no longer has to find the spelling
 * by being refused.
 *
 * The .metallib is taken apart here rather than by the library, because that is how it
 * goes on the device too: Metal has already parsed the container and -[_MTLFunction
 * bitcodeData] hands the driver one module (docs/metal.md 10.12, where those bytes were
 * checked against this frontend's own reader over all eleven functions of AV1SW --
 * same lengths, same FNV-1a, 11 of 11). The container's layout is docs/metal.md section
 * 9, and tools/metallib.py reads the same fields from Python.
 *
 * The screen is softpipe, through RWA_SOFTWARE=1, which this program sets itself unless
 * the environment already says: there is no Adreno on this machine and the point is the
 * path, not the hardware. RWA_SOFTWARE=0 asks for the GPU and so prints what a driver
 * on a phone booted without gpu=1 is told.
 *
 *   air_metal_test FILE.metallib [FUNCTION [MTLPixelFormat[,...] [FILL]]]
 *   air_metal_test --textures FILE.metallib [FUNCTION]
 *   air_metal_test --draw FILE.metallib FUNCTION
 *                  [MTLPixelFormat[,...] [MTLPixelFormat[:MTLTextureType][,...]]]
 *   air_metal_test --fbfetch FILE.bc FUNCTION
 *   air_metal_test --draw-depth FILE.metallib FUNCTION
 *                  [MTLPixelFormat[,...]|none [DEPTHFORMAT]]
 *   air_metal_test --draw-ms FILE.metallib FUNCTION [SAMPLES [FILLFUNCTION]]
 *   air_metal_test --draw-index FILE.metallib FUNCTION [VERTEXFUNCTION]
 *   air_metal_test --draw-vertex FILE.metallib VERTEXFUNCTION FUNCTION
 *                  [MTLPixelFormat[,...] [SLOT:COMPONENTS|scale:S[,...]
 *                  [MTLPixelFormat[:MTLTextureType][,...]]]]
 *   air_metal_test --draw-attrib VERTEXLIB VERTEXFUNCTION FRAGMENTLIB FRAGMENTFUNCTION
 *                  [GATEDVERTEXFUNCTION]
 *   air_metal_test --attrib-sweep VERTEXLIB FRAGMENTLIB FRAGMENTFUNCTION
 *   air_metal_test --argbufs FILE[.metallib|.bc] [FUNCTION]
 *   air_metal_test --draw-argbuf SETMODULE DIRECTMODULE
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* --attrib-sweep builds one pipeline per child process, because softpipe ASSERTS on a
 * 16-bit opcode rather than declining it (nir_to_tgsi.c's "Unknown NIR opcode") and an
 * abort in the middle of a sweep of 141 modules would report nothing about the other
 * 140. Nothing else in this program forks.
 */
#include <sys/wait.h>
#include <unistd.h>

#include "air_metal.h"

#define WHY 512
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/* The texture is 4x4 and so is the grid, which is air_run's TEX_SIZE and GRID: the
 * same texel pattern read by the same number of threads, so a number out of this
 * program and a number out of that one are the same measurement.
 */
#define TEX 4
#define TEXELS (TEX * TEX * TEX * 4)

/* The texture types and pixel formats this program can fill. Only three formats,
 * because the texel values below (100 + x, 200 + y, 300 + x + y + z, 400) have to be
 * exact in whatever format holds them -- in RGBA8Unorm they would all clamp to 1.0 and
 * a kernel reading the wrong texel would read the right answer. The library translates
 * forty-two formats and all ten texture types it can allocate (air_metal.c); this
 * program writes known texels into three formats and two types, which is what Apple's
 * compute kernels bind.
 */
enum {
   MTL_TEXTURE_TYPE_2D = 2, MTL_TEXTURE_TYPE_2D_MULTISAMPLE = 4, MTL_TEXTURE_TYPE_3D = 7,
   MTL_USAGE_SHADER_READ = 1, MTL_USAGE_SHADER_WRITE = 2, MTL_USAGE_RENDER_TARGET = 4,
   MTL_RGBA32_UINT = 123, MTL_RGBA32_SINT = 124, MTL_RGBA32_FLOAT = 125,
   /* and the depth formats --draw-depth makes an attachment of. Depth32Float is the
    * default there because it is one plane on both back ends and stores 0.25, 0.5 and
    * 0.75 exactly, so an expected depth and a measured one are comparable with == and
    * not with a tolerance that would hide a real difference.
    */
   MTL_DEPTH16_UNORM = 250, MTL_DEPTH32_FLOAT = 252, MTL_STENCIL8 = 253,
   MTL_DEPTH32_FLOAT_STENCIL8 = 260,
};

/* MTLVertexFormat and MTLVertexStepFunction, the few --draw-attrib configures and the
 * few it expects to be refused. The numbers are Apple's own, out of the iOS 16.5 SDK's
 * MTLVertexDescriptor.h -- the same header userland/src/annibalemetal.m asserts the
 * library's table against -- and MTLVertexFormatFloatRGB9E5 (55) is in that list
 * BECAUSE it is not in that header: iOS 17 added it, so nothing on the device can ask
 * for it, and what this program checks about it is that the sentence names it rather
 * than printing a number.
 */
enum {
   MTL_VF_UCHAR4_NORMALIZED = 9, MTL_VF_SHORT4_NORMALIZED = 24, MTL_VF_HALF2 = 25,
   MTL_VF_UCHAR4_NORMALIZED_BGRA = 42,
   MTL_VF_FLOAT2 = 29, MTL_VF_FLOAT4 = 31, MTL_VF_INT4 = 35,
   MTL_VF_UINT1010102_NORMALIZED = 41, MTL_VF_FLOAT_RGB9E5 = 55,
   MTL_STEP_CONSTANT = 0, MTL_STEP_PER_VERTEX = 1, MTL_STEP_PER_INSTANCE = 2,
   MTL_STEP_PER_PATCH = 3,
};

/* air_run's texel: (100 + x + 1000n, 200 + y, 300 + x + y + z, 400) for slot n. Every
 * component differs from every other, and from the same component of another slot and
 * another texel, so a kernel reading the wrong texel or the wrong slot cannot come
 * back with the right number -- and neither can a readback with the wrong stride.
 */
static void
texel_value(unsigned slot, unsigned x, unsigned y, unsigned z, float out[4])
{
   out[0] = 100.0f + (float)x + 1000.0f * (float)slot;
   out[1] = 200.0f + (float)y;
   out[2] = 300.0f + (float)x + (float)y + (float)z;
   out[3] = 400.0f;
}

/* where texel (x, y, z) sits in a tightly packed level of TEX x TEX x TEX */
static unsigned
texel_index(unsigned x, unsigned y, unsigned z)
{
   return ((z * TEX + y) * TEX + x) * 4;
}

/*
 * The packed 16-bit probe, and the one pixel a draw read back.
 *
 * Metal's four packed 16-bit colour formats are five, six and one bit wide per channel,
 * so this program's texel pattern -- 100 + x and up -- cannot be written into one at
 * all, and a texture filled with the nearest representable thing would measure nothing.
 * Instead every texel gets ONE 16-BIT WORD WITH ONE FIELD SET, and the question the
 * draw answers is which CHANNEL that field became. A coordinate cannot change that
 * answer, which is why every texel is the same.
 *
 * A file-scope variable and not an argument because it reaches the middle of run_draw,
 * which already takes six: --pixel-formats sets it for each probe, and a texspec naming
 * a packed format outside that mode is refused rather than filled with something.
 */
static unsigned probe_fill16;
static bool probe_fill16_given;
static float last_pixel[4];
static unsigned last_word;

/* One texel, in the format's own type: a uint texture read as floats prints 1.4e-43
 * and looks like a failure of the kernel rather than of whoever printed it.
 */
static void
print_texel(const char *what, unsigned slot, unsigned x, unsigned y, unsigned z,
            const void *texels, bool is_float)
{
   unsigned k = texel_index(x, y, z);
   if (is_float) {
      const float *f = texels;
      printf("air_metal_test: %s %u texel (%u,%u,%u) = %.9g %.9g %.9g %.9g\n", what, slot, x,
             y, z, (double)f[k], (double)f[k + 1], (double)f[k + 2], (double)f[k + 3]);
   } else {
      const int32_t *i = texels;
      printf("air_metal_test: %s %u texel (%u,%u,%u) = %d %d %d %d\n", what, slot, x, y, z,
             i[k], i[k + 1], i[k + 2], i[k + 3]);
   }
}

/* One function's module out of an MTLB container. The offsets are the header's: the
 * function list at 0x18, the bitcode region at 0x48, and inside a function's record a
 * NAME tag, an OFFT triple whose third word is where that function's module starts
 * inside the region, and MDSZ, how long it is. Everything is little endian and nothing
 * is aligned.
 */
struct metallib {
   uint8_t *bytes;
   size_t size;
};

static uint64_t
u64_at(const struct metallib *lib, size_t off)
{
   uint64_t v = 0;
   if (off + 8 <= lib->size)
      memcpy(&v, lib->bytes + off, 8);
   return v;
}

static uint32_t
u32_at(const struct metallib *lib, size_t off)
{
   uint32_t v = 0;
   if (off + 4 <= lib->size)
      memcpy(&v, lib->bytes + off, 4);
   return v;
}

static uint16_t
u16_at(const struct metallib *lib, size_t off)
{
   uint16_t v = 0;
   if (off + 2 <= lib->size)
      memcpy(&v, lib->bytes + off, 2);
   return v;
}

/* Every function of the container, with each one's module: exactly the bytes
 * tools/metallib.py hands to llvm-dis, the blob MDSZ sizes, wrapper and all. One record
 * loop, because find_function below is one pick out of this table and --textures is the
 * whole of it -- and a second loop over the same unaligned tags would be a second reader
 * of a format that has only ever been read once correctly.
 *
 * A record whose module falls outside the file keeps its name and gets no bytes, so the
 * sentence about it names the function rather than the file.
 */
struct metallib_table {
   unsigned count;
   char **names;
   const uint8_t **bitcode;
   size_t *size;
};

static void
free_functions(struct metallib_table *t)
{
   for (unsigned i = 0; i < t->count; i++)
      free(t->names[i]);
   free(t->names);
   free(t->bitcode);
   free(t->size);
   memset(t, 0, sizeof(*t));
}

static bool
parse_functions(const struct metallib *lib, struct metallib_table *t)
{
   memset(t, 0, sizeof(*t));
   if (lib->size < 0x50 || memcmp(lib->bytes, "MTLB", 4) != 0) {
      fprintf(stderr, "air_metal_test: not a .metallib\n");
      return false;
   }
   size_t list = u64_at(lib, 0x18);
   uint64_t bc_off = u64_at(lib, 0x48), bc_len = u64_at(lib, 0x50);
   unsigned count = u32_at(lib, list);

   /* The count comes out of the file, so it is bounded by the file: the records live
    * between the list's start and the end offset at 0x20, and the smallest one is a
    * size word and an ENDT.
    */
   uint64_t list_end = u64_at(lib, 0x20);
   if (list_end > lib->size || list_end < list) {
      fprintf(stderr, "air_metal_test: the function list runs to %llu and the file is "
                      "%zu bytes: truncated\n", (unsigned long long)list_end, lib->size);
      return false;
   }
   if (count > (list_end - list) / 8) {
      fprintf(stderr, "air_metal_test: a function list of %u records in %llu bytes\n",
              count, (unsigned long long)(list_end - list));
      return false;
   }

   t->names = calloc(count ? count : 1, sizeof(*t->names));
   t->bitcode = calloc(count ? count : 1, sizeof(*t->bitcode));
   t->size = calloc(count ? count : 1, sizeof(*t->size));
   if (!t->names || !t->bitcode || !t->size) {
      fprintf(stderr, "air_metal_test: out of memory for %u functions\n", count);
      free_functions(t);
      return false;
   }

   size_t p = list + 4;
   for (unsigned i = 0; i < count; i++) {
      uint64_t off = 0, len = 0;
      uint32_t size = u32_at(lib, p);   /* counts from this field itself */
      size_t q = p + 4, end = p + size;
      while (q + 6 <= end && q + 6 <= lib->size) {
         const char *tag = (const char *)lib->bytes + q;
         if (!memcmp(tag, "ENDT", 4))
            break;
         uint16_t tlen = u16_at(lib, q + 4);
         if (!memcmp(tag, "NAME", 4))
            t->names[i] = strndup((const char *)lib->bytes + q + 6, tlen);
         else if (!memcmp(tag, "OFFT", 4) && tlen >= 24)
            off = u64_at(lib, q + 6 + 16);
         else if (!memcmp(tag, "MDSZ", 4) && tlen >= 8)
            len = u64_at(lib, q + 6);
         q += 6 + tlen;
      }
      uint64_t start = bc_off + off, stop = bc_off + off + len;
      if (start < lib->size && stop <= lib->size && stop > start &&
          stop <= bc_off + bc_len) {
         t->bitcode[i] = lib->bytes + start;
         t->size[i] = stop - start;
      }
      p = end;
      t->count++;
   }
   return true;
}

/* Finds `want` (or the first function when it is NULL) and returns its module. */
static bool
find_function(const struct metallib *lib, const char *want, char *name, size_t name_size,
              const uint8_t **bitcode, size_t *bitcode_size)
{
   /* A file that is not a container is one module, as air_run reads one. It is here so a
    * hand-written probe can go through THIS library and not only through air_run: the
    * corpus has no fragment shader that reads its own position (all seventeen that
    * declare [[position]] mark it air.arg_unused), so the only way to exercise the
    * driver's own position path is a module written for it -- tools/air/fragpos.ll.
    */
   if (lib->size >= 4 && memcmp(lib->bytes, "MTLB", 4) != 0) {
      snprintf(name, name_size, "%s", want ? want : "(the only module)");
      *bitcode = lib->bytes;
      *bitcode_size = lib->size;
      return true;
   }

   struct metallib_table t;
   if (!parse_functions(lib, &t))
      return false;

   unsigned pick = t.count;
   for (unsigned i = 0; i < t.count; i++)
      if (t.names[i] && (!want || !strcmp(want, t.names[i]))) {
         pick = i;
         break;
      }
   bool ok = pick < t.count;
   if (!ok) {
      fprintf(stderr, "air_metal_test: no function %s in a library of %u\n",
              want ? want : "(any)", t.count);
   } else if (!t.bitcode[pick]) {
      fprintf(stderr, "air_metal_test: %s is outside the file\n", t.names[pick]);
      ok = false;
   } else {
      snprintf(name, name_size, "%s", t.names[pick]);
      *bitcode = t.bitcode[pick];
      *bitcode_size = t.size[pick];
   }

   free_functions(&t);
   return ok;
}

/* The whole file into memory, the way Metal has the container in hand before it hands a
 * driver one function's bitcode. Returns false and says why on stderr.
 */
static bool
load_metallib(const char *path, struct metallib *lib)
{
   FILE *f = fopen(path, "rb");
   if (!f) {
      perror(path);
      return false;
   }
   fseek(f, 0, SEEK_END);
   lib->size = ftell(f);
   fseek(f, 0, SEEK_SET);
   lib->bytes = malloc(lib->size);
   if (fread(lib->bytes, 1, lib->size, f) != lib->size) {
      fprintf(stderr, "air_metal_test: short read of %s\n", path);
      fclose(f);
      return false;
   }
   fclose(f);
   return true;
}

/* air_run's buffer pattern, (i * 7 + 13) & 0xff: a byte that varies with its offset, so
 * a shader reading the wrong offset reads a wrong value and not a plausible one, and a
 * number out of this draw and a number out of air_run are the same measurement because
 * the bytes under them are the same.
 */
static uint8_t
pattern_byte(unsigned i)
{
   return (i * 7 + 13) & 0xff;
}

/* The library's own full-screen quad, in clip space -- the same four corners as
 * rwa_quad_verts inside air_metal.c -- and the texture coordinate of each corner.
 *
 * These are what a SUPPLIED vertex shader is fed. Apple's three vertex shaders all read
 * their vertices out of a buffer indexed by the vertex id, so handing them this quad
 * makes them emit the geometry the library's generated stage draws: the two roads then
 * cover the same pixels, and a difference in the picture is a difference in the shader
 * rather than in where the triangles went. The third texture coordinate is 0.125, the
 * centre of slice 0 of a 4-deep 3D texture, so a float3 coordinate lands in slice 0.
 */
static const float draw_quad_pos[4][4] = {
   { -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
};
static const float draw_quad_uv[4][3] = {
   { 0, 0, 0.125f }, { 1, 0, 0.125f }, { 0, 1, 0.125f }, { 1, 1, 0.125f },
};

/* What one buffer of a supplied vertex stage holds: `comps` is 0 for the quad's
 * clip-space positions and 2 or 3 for texture coordinates of that many components.
 *
 * The element stride is the caller's to know, and that is the contract working as
 * designed rather than a gap in it: rwa_render_info reports how many buffers a stage
 * binds and not what one element of one of them is, exactly as it reports how many
 * textures and not their formats, because a driver never has to ask -- the application
 * filled the MTLBuffer and knows what it put there. air64's own strides are at work:
 * texturedVertex2D walks a <2 x float> every 8 bytes while texturedVertex3D walks a
 * <3 x float> every 16, because this target aligns a 96-bit vector to 128 bits, and a
 * test that guessed one stride for both would read the second vertex of one of them out
 * of the middle of the first.
 */
static void
fill_vertex_buffer(void *mem, unsigned comps, float scale)
{
   float *f = mem;
   if (comps == 0) {
      for (unsigned v = 0; v < 4; v++) {
         /* x and y only: scaling z and w would move the quad out of the near plane or
          * divide by something other than one, and what this knob is for is a smaller
          * quad in the same plane
          */
         f[v * 4 + 0] = draw_quad_pos[v][0] * scale;
         f[v * 4 + 1] = draw_quad_pos[v][1] * scale;
         f[v * 4 + 2] = draw_quad_pos[v][2];
         f[v * 4 + 3] = draw_quad_pos[v][3];
      }
      return;
   }
   unsigned stride = comps == 3 ? 4 : comps;
   for (unsigned v = 0; v < 4; v++)
      for (unsigned c = 0; c < stride; c++)
         f[v * stride + c] = c < comps ? draw_quad_uv[v][c] : 0.0f;
}

/* What a supplied vertex stage's buffers hold: a comma-separated list of
 *
 *   SLOT:COMPONENTS   that slot holds texture coordinates of that many components,
 *                     as in "1:2" for texturedVertex2D, whose buffer 1 is one float2
 *                     per vertex. Every slot not named holds the quad's positions, for
 *                     the reason the fragment buffers are all filled alike -- the
 *                     contract does not say which index the shader reads, so every
 *                     index holds something the shader could read.
 *   scale:S           the quad's corners are S times as far from the origin.
 *
 * The scale is the canary, and it is here because the first run of this mode agreed with
 * the generated stage to the bit, which is exactly what a mode that quietly drew the
 * generated quad anyway would also have printed. "scale:0.5" covers only the middle of a
 * 4x4 attachment, so the pixel at (0, 0) must come back as the clear colour; if it still
 * came back as the shader's value, the supplied vertex shader is not what put the
 * triangles on the screen and nothing else here is evidence.
 */
static void
parse_vertex_spec(const char *spec, unsigned *comps, unsigned n, float *scale)
{
   while (spec && *spec) {
      const char *colon = strchr(spec, ':');
      const char *comma = strchr(spec, ',');
      if (!strncmp(spec, "scale:", 6)) {
         *scale = strtof(spec + 6, NULL);
      } else if (colon && (!comma || colon < comma)) {
         unsigned slot = (unsigned)strtoul(spec, NULL, 0);
         if (slot < n)
            comps[slot] = (unsigned)strtoul(colon + 1, NULL, 0);
      }
      spec = comma ? comma + 1 : NULL;
   }
}

/* MTLTextureType, in the SDK's own spelling. The numbers are air_metal.c's
 * texture_type_name table and nothing else: a type printed as a number here would be
 * read against a header by hand every time the table is read.
 */
static const char *
texture_type_name(unsigned type)
{
   static const char *const names[] = {
      "MTLTextureType1D", "MTLTextureType1DArray", "MTLTextureType2D",
      "MTLTextureType2DArray", "MTLTextureType2DMultisample", "MTLTextureTypeCube",
      "MTLTextureTypeCubeArray", "MTLTextureType3D", "MTLTextureType2DMultisampleArray",
      "MTLTextureTypeTextureBuffer",
   };
   return type < COUNT_OF(names) ? names[type] : "an MTLTextureType with no name here";
}

static const char *
element_name(unsigned element)
{
   switch (element) {
   case RWA_ELEMENT_FLOAT: return "float";
   case RWA_ELEMENT_INT:   return "int";
   case RWA_ELEMENT_UINT:  return "uint";
   default:                return "-";
   }
}

#include "air_metal_test_draw.inc"
#include "air_metal_test_depth.inc"
#include "air_metal_test_ms.inc"
#include "air_metal_test_index.inc"
#include "air_metal_test_attrib.inc"
#include "air_metal_test_argbuf.inc"
#include "air_metal_test_blend.inc"

int
main(int argc, char **argv)
{
   /* --draw puts a fragment shader through rwa_render_*; without it the program
    * dispatches a kernel through rwa_compute_*, as it did before. The two are separate
    * because a fragment shader handed to rwa_compute_create is refused ("is a fragment
    * shader, and this makes compute pipelines"), which is the right answer and not a
    * test.
    */
   /* --textures asks what a library's functions declare and binds nothing, so it comes
    * first: it is the answer a caller needs BEFORE it can make the textures the two
    * modes below make.
    */
   if (argc >= 2 && !strcmp(argv[1], "--textures")) {
      if (argc < 3) {
         fprintf(stderr, "usage: air_metal_test --textures FILE.metallib [FUNCTION]\n");
         return 2;
      }
      return run_textures(argv[2], argc > 3 ? argv[3] : NULL);
   }

   /* --argbufs is the same kind of question as --textures and comes next to it: what
    * does a module declare that a caller has to allocate before it can bind anything.
    */
   if (argc >= 2 && !strcmp(argv[1], "--argbufs")) {
      if (argc < 3) {
         fprintf(stderr, "usage: air_metal_test --argbufs FILE [FUNCTION]\n");
         return 2;
      }
      return run_argbufs(argv[2], argc > 3 ? argv[3] : NULL);
   }

   /* --draw-argbuf is the runtime half of docs/air.md 8.2: the same picture through an
    * MTLArgumentEncoder's records as through direct arguments, and a named different
    * picture for each record that is deliberately wrong. Two modules, because the
    * measurement IS the comparison between them.
    */
   if (argc >= 2 && !strcmp(argv[1], "--draw-argbuf")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --draw-argbuf SETMODULE DIRECTMODULE\n");
         return 2;
      }
      return run_draw_argbuf(argv[2], argv[3]);
   }

   /* --surface takes no .metallib at all: it is about memory, not about a shader. */
   if (argc >= 2 && !strcmp(argv[1], "--surface")) {
      return run_surface(argc > 2 ? atoi(argv[2]) : 1,
                         argc > 3 ? (unsigned) atoi(argv[3]) : 1080,
                         argc > 4 ? (unsigned) atoi(argv[4]) : 2400,
                         argc > 5 ? (unsigned) atoi(argv[5]) : 4352);
   }

   if (argc >= 2 && !strcmp(argv[1], "--draw")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --draw FILE.metallib FUNCTION "
                         "[MTLPixelFormat[,...] [MTLPixelFormat[:MTLTextureType][,...]]]"
                         "\n");
         return 2;
      }
      /* no vertex-buffer argument: the generated stage reads none. The texture one is
       * here so the generated stage can be the reference for a textured fragment
       * shader -- texturedFragment3DUInt wants a texture3d, and it is the comparison
       * that needs it rather than the stage.
       */
      return run_draw(argv[2], NULL, argv[3], argc > 4 ? argv[4] : NULL, NULL,
                      argc > 5 ? argv[5] : NULL);
   }

   /* --pixel-formats is the draw used as an instrument: the same shader sixteen times
    * over textures of Metal's four packed 16-bit formats, each holding one 16-bit word
    * with one field set, to measure which field became which channel.
    */
   /* --draw-blend: colour blending, with each way of being wrong priced beside the
    * right answer. Two arguments, like --pixel-formats: a container and a fragment
    * function whose output this program controls through the texture it samples.
    */
   if (argc >= 2 && !strcmp(argv[1], "--draw-blend")) {
      if (argc < 4) {
         fprintf(stderr, "usage: %s --draw-blend FILE.metallib FUNCTION\n", argv[0]);
         return 2;
      }
      return run_blend(argv[2], argv[3]);
   }

   if (argc >= 2 && !strcmp(argv[1], "--fbfetch")) {
      if (argc != 4) {
         fprintf(stderr, "usage: %s --fbfetch FILE.bc FUNCTION\n", argv[0]);
         return 2;
      }
      return run_fbfetch(argv[2], argv[3]);
   }

   if (argc >= 2 && !strcmp(argv[1], "--pixel-formats")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --pixel-formats FILE.metallib FUNCTION\n");
         return 2;
      }
      return run_pixel_formats(argv[2], argv[3]);
   }

   /* --draw-depth is the draw with a depth attachment under it, which no pass in this
    * port has ever had. It is its own flag because what it measures is not a pixel but
    * whether a draw was discarded, and that takes four draws of one quad rather than
    * one (run_depth says why each).
    */
   if (argc >= 2 && !strcmp(argv[1], "--draw-depth")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --draw-depth FILE.metallib FUNCTION "
                         "[MTLPixelFormat[,...]|none [DEPTHFORMAT]]\n");
         return 2;
      }
      return run_depth(argv[2], argv[3], argc > 4 ? argv[4] : NULL,
                       argc > 5 ? argv[5] : NULL);
   }

   if (argc >= 2 && !strcmp(argv[1], "--depth-store-filter")) {
      if (argc < 5) {
         fprintf(stderr, "usage: air_metal_test --depth-store-filter FILE.metallib "
                         "TEMP_FUNC DEPTH_FUNC\n");
         return 2;
      }
      return run_depth_store_filter(argv[2], argv[3], argv[4]);
   }

   /* --draw-ms is the draw whose TEXTURE is multisample, which takes two passes and not
    * one: nothing puts a known texel in a multisample texture from the processor, so the
    * first pass renders the fill into it and the second is the shader under test reading
    * it (run_ms says why at length, and what each of the two configurations measures).
    */
   if (argc >= 2 && !strcmp(argv[1], "--draw-ms")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --draw-ms FILE.metallib FUNCTION "
                         "[SAMPLES [FILLFUNCTION]]\n");
         return 2;
      }
      return run_ms(argv[2], argv[3], argc > 4 ? argv[4] : NULL,
                    argc > 5 ? argv[5] : NULL);
   }

   /* --draw-index is the draw that goes through an index buffer, which every drawing
    * call in a real application does and none in this library did. It is its own flag
    * because what it measures is not a pixel but whether the indices were read at all:
    * the same three indices are drawn from two places in one buffer, and the two
    * pictures have to differ (run_index says what each step would print if it were
    * wrong).
    */
   if (argc >= 2 && !strcmp(argv[1], "--draw-index")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --draw-index FILE.metallib FUNCTION "
                         "[VERTEXFUNCTION]\n");
         return 2;
      }
      return run_index(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
   }

   /* --draw-vertex is the same draw with one thing changed: the vertex stage is a real
    * module out of the container instead of the one this library generates. It is its own
    * flag and not an optional argument of --draw so that the two can be run back to back
    * over the same fragment shader and the pixels compared -- which is the only way the
    * supplied stage can be said to be right about anything, since there is no second
    * implementation of Apple's vertex shaders to check it against.
    */
   /* --draw-attrib is the vertex ATTRIBUTE path: a real MTLVertexDescriptor over a real
    * [[stage_in]] vertex function, which is what the phone's window server asks for and
    * what this contract could not express until now. It takes two libraries because a
    * Metal pipeline may take its two functions from two of them, and the pair this was
    * measured with is QuartzCore's std_vert0_lpf over AppleMetalGLRenderer's
    * texturedFragment2DUInt: the first has the attributes and the second turns the
    * varying they feed into a texel a pixel can be read as.
    */
   /* --attrib-sweep is the same creation call over every function of a library, which is
    * what the window server does one pipeline at a time: no draw, no picture, just how
    * many of them get a pipeline out of this contract and what the rest are refused for.
    */
   if (argc >= 2 && !strcmp(argv[1], "--attrib-sweep")) {
      if (argc < 5) {
         fprintf(stderr, "usage: air_metal_test --attrib-sweep VERTEXLIB FRAGMENTLIB "
                         "FRAGMENTFUNCTION\n");
         return 2;
      }
      return run_attrib_sweep(argv[2], argv[3], argv[4]);
   }

   if (argc >= 2 && !strcmp(argv[1], "--draw-attrib")) {
      if (argc < 6) {
         fprintf(stderr, "usage: air_metal_test --draw-attrib VERTEXLIB VERTEXFUNCTION "
                         "FRAGMENTLIB FRAGMENTFUNCTION [GATEDVERTEXFUNCTION]\n");
         return 2;
      }
      return run_attrib(argv[2], argv[3], argv[4], argv[5],
                        argc > 6 ? argv[6] : NULL);
   }

   if (argc >= 2 && !strcmp(argv[1], "--draw-vertex")) {
      if (argc < 5) {
         fprintf(stderr, "usage: air_metal_test --draw-vertex FILE.metallib VERTEXFUNCTION "
                         "FUNCTION [MTLPixelFormat[,...] [SLOT:COMPONENTS[,...] "
                         "[MTLPixelFormat[:MTLTextureType][,...]]]]\n");
         return 2;
      }
      return run_draw(argv[2], argv[3], argv[4], argc > 5 ? argv[5] : NULL,
                      argc > 6 ? argv[6] : NULL, argc > 7 ? argv[7] : NULL);
   }

   if (argc < 2) {
      fprintf(stderr, "usage: air_metal_test FILE.metallib [FUNCTION "
                      "[MTLPixelFormat[,...] [FILL]]]\n"
                      "       air_metal_test --textures FILE.metallib [FUNCTION]\n"
                      "       air_metal_test --draw FILE.metallib FUNCTION "
                      "[MTLPixelFormat[,...] "
                      "[MTLPixelFormat[:MTLTextureType][,...]]]\n"
                      "       air_metal_test --fbfetch FILE.bc FUNCTION\n"
                      "       air_metal_test --draw-depth FILE.metallib FUNCTION "
                      "[MTLPixelFormat[,...]|none [DEPTHFORMAT]]\n"
                      "       air_metal_test --draw-ms FILE.metallib FUNCTION "
                      "[SAMPLES [FILLFUNCTION]]\n"
                      "       air_metal_test --draw-index FILE.metallib FUNCTION "
                      "[VERTEXFUNCTION]\n"
                      "       air_metal_test --draw-vertex FILE.metallib VERTEXFUNCTION "
                      "FUNCTION [MTLPixelFormat[,...] [SLOT:COMPONENTS[,...] "
                      "[MTLPixelFormat[:MTLTextureType][,...]]]]\n"
                      "       air_metal_test --draw-attrib VERTEXLIB VERTEXFUNCTION "
                      "FRAGMENTLIB FRAGMENTFUNCTION [GATEDVERTEXFUNCTION]\n"
                      "       air_metal_test --attrib-sweep VERTEXLIB FRAGMENTLIB "
                      "FRAGMENTFUNCTION\n");
      return 2;
   }

   /* What every buffer is filled with, 0xa5 unless the fourth argument says. This is
    * air_run's --zero and --ones in one byte and for the same reason (docs/air.md
    * section 5): a kernel whose buffers carry counts, bounds or sentinels does nothing
    * at all on a byte pattern, and which fill gets it to the end says what it was
    * reading them as. Zero is the degenerate input that makes a bounds check pass;
    * 0xa5 is the one that makes a dispatch which wrote nothing distinguishable from
    * one that wrote zeros.
    */
   unsigned fill = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 0) & 0xff : 0xa5;

   /* One format per texture slot, comma separated, each optionally followed by ":" and
    * an MTLTextureType (2 is 2D and the default, 7 is 3D). Per slot and not per kernel
    * because Apple's own kernels mix both: BlitKernel_Unorm reads a texture2d<uint>
    * and writes a texture2d<float>, so "123,125" is that kernel, and
    * MetalFaceMask_PopulateCube reads a texture2d and writes a texture3d, so
    * "125,125:7" is that one. A single format or a single type would be refused for
    * whichever of the two slots it is not -- which is the library naming what the
    * shader declared, and is how these spellings were found.
    */
   unsigned format[16], type[16];
   bool is_float[COUNT_OF(format)];
   const char *arg = argc > 3 ? argv[3] : NULL;
   for (unsigned i = 0; i < COUNT_OF(format); i++) {
      const char *colon = arg ? strchr(arg, ':') : NULL;
      const char *comma = arg ? strchr(arg, ',') : NULL;
      type[i] = colon && (!comma || colon < comma) ? (unsigned)strtoul(colon + 1, NULL, 0)
                                                   : MTL_TEXTURE_TYPE_2D;
      if (type[i] != MTL_TEXTURE_TYPE_2D && type[i] != MTL_TEXTURE_TYPE_3D) {
         fprintf(stderr, "air_metal_test: texture type %u: this program makes "
                         "MTLTextureType2D (%u) and MTLTextureType3D (%u); the library makes "
                         "the other eight too\n", type[i], MTL_TEXTURE_TYPE_2D,
                 MTL_TEXTURE_TYPE_3D);
         return 2;
      }
      format[i] = arg ? (unsigned)strtoul(arg, NULL, 0) : MTL_RGBA32_FLOAT;
      if (format[i] != MTL_RGBA32_FLOAT && format[i] != MTL_RGBA32_UINT &&
          format[i] != MTL_RGBA32_SINT) {
         fprintf(stderr, "air_metal_test: pixel format %u: this program writes known texels "
                         "in MTLPixelFormatRGBA32Float (%u), RGBA32Uint (%u) or RGBA32Sint "
                         "(%u)\n", format[i], MTL_RGBA32_FLOAT, MTL_RGBA32_UINT,
                 MTL_RGBA32_SINT);
         return 2;
      }
      is_float[i] = format[i] == MTL_RGBA32_FLOAT;
      if (comma)
         arg = comma + 1;      /* otherwise the last one stands for the rest */
   }

   struct metallib lib = { 0 };
   FILE *f = fopen(argv[1], "rb");
   if (!f) {
      perror(argv[1]);
      return 1;
   }
   fseek(f, 0, SEEK_END);
   lib.size = ftell(f);
   fseek(f, 0, SEEK_SET);
   lib.bytes = malloc(lib.size);
   if (fread(lib.bytes, 1, lib.size, f) != lib.size) {
      fprintf(stderr, "air_metal_test: short read of %s\n", argv[1]);
      return 1;
   }
   fclose(f);

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, argc > 2 ? argv[2] : NULL, name, sizeof(name), &bitcode,
                      &bitcode_size))
      return 1;
   printf("air_metal_test: %s, %zu bytes of bitcode, %s\n", name, bitcode_size,
          bitcode[0] == 0xde ? "in its 0x0B17C0DE wrapper" : "a bare module");

   /* This machine has no Adreno, and rwa_device_open does not fall back to software by
    * itself: on the phone an absent GPU means a boot without gpu=1, and a driver has to
    * report that rather than quietly compute the answer on the processor. Not
    * overwritten, so that RWA_SOFTWARE=0 in the environment shows what a driver would be
    * told there.
    */
   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: device is \"%s\"\n", rwa_device_name(device));

   struct rwa_compute_info info = { 0 };
   struct rwa_compute *compute =
      rwa_compute_create(device, bitcode, bitcode_size, argc > 2 ? argv[2] : NULL, &info,
                         why, sizeof(why));
   if (!compute) {
      printf("air_metal_test: rwa_compute_create: %s\n", why);
      rwa_device_close(device);
      return 1;
   }
   printf("air_metal_test: pipeline: %u threads per group, execution width %u, "
          "%u bytes of threadgroup memory, %u buffers, %u textures, %u samplers\n",
          info.max_threads_per_group, info.thread_execution_width,
          info.static_shared_bytes, info.buffers, info.textures, info.samplers);

   /* One buffer per binding the pipeline says it reads, each filled with a byte the
    * kernel could not have produced itself, so that a dispatch which wrote nothing is
    * distinguishable from one that wrote zeros -- the mistake air_run made first and
    * docs/air.md section 5 records.
    */
   struct rwa_buffer *buffers[16] = { 0 };
   unsigned count = info.buffers < COUNT_OF(buffers) ? info.buffers
                                                      : (unsigned)COUNT_OF(buffers);
   for (unsigned i = 0; i < count; i++) {
      buffers[i] = rwa_buffer_create(device, 4096, why, sizeof(why));
      if (!buffers[i]) {
         printf("air_metal_test: rwa_buffer_create: %s\n", why);
         return 1;
      }
      memset(rwa_buffer_map(buffers[i]), (int)fill, rwa_buffer_size(buffers[i]));
   }
   printf("air_metal_test: %u buffer%s of 4096 bytes, filled with 0x%02x\n", count,
          count == 1 ? "" : "s", fill);

   /* One texture per slot the pipeline says it binds, each with air_run's texel
    * pattern in it. A slot the kernel only writes gets the pattern too, which is the
    * same argument as the buffers' 0xa5: a kernel that writes nothing has to be
    * distinguishable from one that writes zeros, and InitMinMax writing zeros would
    * look identical to a dispatch that never ran (docs/air.md section 5).
    */
   const unsigned origin[3] = { 0, 0, 0 };
   struct rwa_texture *textures[16] = { 0 };
   union { float f[TEXELS]; int32_t i[TEXELS]; } written[16];
   unsigned extent[COUNT_OF(textures)][3];
   unsigned ntex = info.textures < COUNT_OF(textures) ? info.textures
                                                      : (unsigned)COUNT_OF(textures);
   for (unsigned i = 0; i < ntex; i++) {
      unsigned depth = type[i] == MTL_TEXTURE_TYPE_3D ? TEX : 1;
      extent[i][0] = TEX;
      extent[i][1] = TEX;
      extent[i][2] = depth;
      const struct rwa_texture_shape shape = {
         .pixel_format = format[i],
         .type = type[i],
         .width = TEX, .height = TEX, .depth = depth,
         .array_length = 1, .mip_levels = 1, .sample_count = 1,
         /* both, because the contract says how many textures a kernel binds and not
          * which of them it writes -- and MTLTextureUsageShaderRead alone would be
          * refused at the dispatch for exactly the slot the kernel stores to
          */
         .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_SHADER_WRITE,
      };
      textures[i] = rwa_texture_create(device, &shape, why, sizeof(why));
      if (!textures[i]) {
         printf("air_metal_test: rwa_texture_create: %s\n", why);
         return 1;
      }
      memset(&written[i], 0, sizeof(written[i]));
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX; y++)
            for (unsigned x = 0; x < TEX; x++) {
               float v[4];
               texel_value(i, x, y, z, v);
               for (unsigned c = 0; c < 4; c++) {
                  unsigned k = texel_index(x, y, z) + c;
                  if (is_float[i])
                     written[i].f[k] = v[c];
                  else
                     written[i].i[k] = (int32_t)v[c];
               }
            }
      if (rwa_texture_write(textures[i], 0, 0, origin, extent[i], &written[i], 0, why,
                            sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_write: %s\n", why);
         return 1;
      }
   }
   for (unsigned i = 0; i < ntex; i++)
      printf("air_metal_test: texture %u is %ux%ux%u of pixel format %u, texel (x,y,z) = "
             "(100+x+%u, 200+y, 300+x+y+z, 400)\n", i, extent[i][0], extent[i][1],
             extent[i][2], format[i], 1000 * i);

   /* and a sampler per slot the API is expected to fill. Nearest, clamped and in
    * pixels, which is what air_run binds in its compute path: a kernel's own
    * coordinates are integer texels, and nearest filtering at a texel centre has no
    * weights to round. A sampler the shader declares for itself is not counted here
    * and not bound -- the library builds that one out of the shader's word.
    */
   struct rwa_sampler *samplers[16] = { 0 };
   unsigned nsamp = info.samplers < COUNT_OF(samplers) ? info.samplers
                                                        : (unsigned)COUNT_OF(samplers);
   for (unsigned i = 0; i < nsamp; i++) {
      const struct rwa_sampler_state state = {
         .address = { 0, 0, 0 },          /* MTLSamplerAddressModeClampToEdge */
         .min_filter = 0, .mag_filter = 0,/* MTLSamplerMinMagFilterNearest */
         .mip_filter = 0,                 /* MTLSamplerMipFilterNotMipmapped */
         .compare_function = 0,           /* MTLCompareFunctionNever, and nothing reads it */
         .max_anisotropy = 1,
         .normalized_coordinates = 0,
         .lod_min = 0.0f, .lod_max = 65504.0f,   /* the range Apple's own default carries */
      };
      samplers[i] = rwa_sampler_create(device, &state, why, sizeof(why));
      if (!samplers[i]) {
         printf("air_metal_test: rwa_sampler_create: %s\n", why);
         return 1;
      }
   }
   if (nsamp)
      printf("air_metal_test: %u sampler%s from the API: nearest, clamp to edge, in pixels\n",
             nsamp, nsamp == 1 ? "" : "s");

   const struct rwa_bindings bindings = {
      .buffers = buffers, .buffer_count = count,
      .textures = textures, .texture_count = ntex,
      .samplers = samplers, .sampler_count = nsamp,
   };
   const unsigned groups[3] = { 1, 1, 1 };
   const unsigned group_size[3] = { TEX, TEX, 1 };
   if (rwa_dispatch(compute, &bindings, groups, group_size, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_dispatch: %s\n", why);
      rwa_compute_destroy(compute);
      for (unsigned i = 0; i < count; i++)
         rwa_buffer_destroy(buffers[i]);
      for (unsigned i = 0; i < ntex; i++)
         rwa_texture_destroy(textures[i]);
      for (unsigned i = 0; i < nsamp; i++)
         rwa_sampler_destroy(samplers[i]);
      rwa_device_close(device);
      return 1;
   }
   if (rwa_finish(device, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_finish: %s\n", why);
      return 1;
   }
   printf("air_metal_test: dispatched %ux%ux%u threadgroups of %ux%ux%u threads\n",
          groups[0], groups[1], groups[2], group_size[0], group_size[1], group_size[2]);

   for (unsigned i = 0; i < count; i++) {
      const uint32_t *words = rwa_buffer_map(buffers[i]);
      uint32_t as_went_in = fill * 0x01010101u;
      bool changed = false;
      for (unsigned w = 0; w < rwa_buffer_size(buffers[i]) / 4 && !changed; w++)
         changed = words[w] != as_went_in;
      printf("air_metal_test: buffer %u %s; first six words %08x %08x %08x %08x %08x %08x\n",
             i, changed ? "was written" : "came back as it went in", words[0], words[1],
             words[2], words[3], words[4], words[5]);
   }

   /* and every texture read back, which is the half of this a buffer cannot stand in
    * for: most of the corpus's texture kernels write a texture and nothing else, so
    * without this the program would report a dispatch it had not looked at.
    */
   for (unsigned i = 0; i < ntex; i++) {
      union { float f[TEXELS]; int32_t i[TEXELS]; } got;
      memset(&got, 0, sizeof(got));
      if (rwa_texture_read(textures[i], 0, 0, origin, extent[i], &got, 0, why,
                           sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_read: %s\n", why);
         return 1;
      }
      bool changed = memcmp(&got, &written[i], sizeof(got)) != 0;
      printf("air_metal_test: texture %u %s\n", i,
             changed ? "was written" : "came back as it went in");
      print_texel("texture", i, 0, 0, 0, &got, is_float[i]);
      print_texel("texture", i, 1, 1, extent[i][2] > 1 ? 1 : 0, &got, is_float[i]);

      /* And texel (1,1) again on its own, which is the part a whole-level read cannot
       * check: an origin that is ignored or a row stride taken from the wrong side
       * comes back as the texel at (0,0) or as a neighbour, and every component of
       * every texel here differs from every other, so it cannot come back right by
       * accident.
       */
      unsigned z = extent[i][2] > 1 ? 1 : 0;
      const unsigned one[3] = { 1, 1, z }, single[3] = { 1, 1, 1 };
      union { float f[4]; int32_t i[4]; } texel;
      if (rwa_texture_read(textures[i], 0, 0, one, single, &texel, 0, why,
                           sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_read of one texel: %s\n", why);
         return 1;
      }
      printf("air_metal_test: texture %u texel (1,1,%u) read on its own %s what the whole "
             "level gave\n", i, z,
             memcmp(&texel, (const char *)&got + texel_index(1, 1, z) * sizeof(float),
                    sizeof(texel)) == 0 ? "is" : "IS NOT");
   }

   /* And the same question on the write side, which the dispatch cannot ask: one texel
    * written at an origin a whole-level write never visits. Afterwards exactly that
    * texel must differ from the pattern and all fifteen others must not -- an origin
    * dropped, or a row stride measured in texels instead of bytes, lands the value
    * somewhere else and leaves two texels wrong instead of one.
    */
   if (ntex) {
      const unsigned at[3] = { 2, 3, extent[0][2] > 1 ? 2u : 0u }, single[3] = { 1, 1, 1 };
      union { float f[4]; int32_t i[4]; } mark;
      for (unsigned c = 0; c < 4; c++) {
         if (is_float[0])
            mark.f[c] = -1.0f - (float)c;
         else
            mark.i[c] = -1 - (int32_t)c;
      }
      if (rwa_texture_write(textures[0], 0, 0, at, single, &mark, 0, why, sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_write of one texel: %s\n", why);
         return 1;
      }
      union { float f[TEXELS]; int32_t i[TEXELS]; } level;
      memset(&level, 0, sizeof(level));
      if (rwa_texture_read(textures[0], 0, 0, origin, extent[0], &level, 0, why,
                           sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_read after it: %s\n", why);
         return 1;
      }
      unsigned moved = 0;
      bool landed = false;
      for (unsigned z = 0; z < extent[0][2]; z++)
         for (unsigned y = 0; y < TEX; y++)
            for (unsigned x = 0; x < TEX; x++) {
               size_t off = texel_index(x, y, z) * sizeof(float);
               const char *there = (const char *)&level + off;
               if (x == at[0] && y == at[1] && z == at[2])
                  landed = memcmp(there, &mark, sizeof(mark)) == 0;
               else if (memcmp(there, (const char *)&written[0] + off, sizeof(mark)) != 0)
                  moved++;
            }
      printf("air_metal_test: one texel written at (%u,%u,%u) %s, and %u other texel%s "
             "changed\n", at[0], at[1], at[2], landed ? "is there" : "IS NOT THERE", moved,
             moved == 1 ? "" : "s");
   }

   rwa_compute_destroy(compute);
   for (unsigned i = 0; i < count; i++)
      rwa_buffer_destroy(buffers[i]);
   for (unsigned i = 0; i < ntex; i++)
      rwa_texture_destroy(textures[i]);
   for (unsigned i = 0; i < nsamp; i++)
      rwa_sampler_destroy(samplers[i]);
   rwa_device_close(device);
   return 0;
}
