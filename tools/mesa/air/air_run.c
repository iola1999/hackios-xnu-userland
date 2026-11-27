/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * air_run: draw with a Metal shader and read the pixels back.
 *
 * This is the half of M1 that compiling cannot give. The shader comes out of a
 * .metallib, through air_to_nir, into a Gallium pipe_shader_state as
 * PIPE_SHADER_IR_NIR -- which both drivers of interest accept natively: ir3
 * is a NIR compiler, and softpipe takes NIR through nir_to_tgsi. One quad is
 * drawn into a 32-bit-float render target and one pixel is printed, so the
 * result is a number and not a picture.
 *
 * On this machine the screen is softpipe, so the whole path can be run with no
 * GPU at all. On the phone the same source wants the freedreno screen that
 * tools/mesa/freedreno-darwin.patch already creates inside the port's Mesa
 * (osmesa_create_gpu_screen), and nothing else about it changes.
 *
 * usage: air_run FILE[.metallib] [FUNCTION]
 */

#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_air.h"
#include "air_module.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_shader_tokens.h"
#include "pipe/p_state.h"
#include "target-helpers/inline_sw_helper.h"
#include "null/null_sw_winsys.h"
#ifdef GALLIUM_FREEDRENO
#include "freedreno/drm/freedreno_drm_public.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include <xf86drm.h>
#endif
#include "util/format/u_format.h"
#include "util/half_float.h"
#include "util/u_inlines.h"
#include "util/u_sampler.h"
#include "util/u_debug.h"
#include "util/u_memory.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <signal.h>
/* <sys/ucontext.h>, not <ucontext.h>: the latter refuses to be included
 * without _XOPEN_SOURCE on the iOS SDK, and userland/src/inject.c takes the
 * same route for the same reason.
 */
#include <sys/ucontext.h>
#endif

#define WIDTH 4
#define HEIGHT 4

/* The texture bound for every sampled texture the shader declares: four by
 * four, 32-bit floats, each texel a number that says which texel it is. The
 * sampler is nearest with no mip filter and clamped addressing, and the
 * coordinate the vertex shader hands over is the centre of texel (1, 1), so
 * a correct sample is an exact equality and not a tolerance -- no filtering
 * weights, no wrap arithmetic, nothing to round.
 *
 * This verifies the texture operations whose sampler the API chooses (nine
 * modules of the corpus) and those that take no sampler at all (twenty-nine,
 * which fetch by integer coordinate). It cannot verify the two whose sampler
 * the shader declares itself: that needs Metal's 64-bit sampler word decoded,
 * which it is not.
 */
#define TEX_SIZE 4
#define TEX_X 1
#define TEX_Y 1

/* Slot is in the value on purpose. Binding one texture to every slot makes a
 * shader that reads the wrong slot return the right texel, which is a test
 * passing for the wrong reason -- the third time that shape of mistake has
 * turned up here, after the 2D texture bound for a 3D sampler and the colour
 * attachment nothing stored to. Looked for deliberately this time rather than
 * tripped over: slot n adds a thousand times n, so a misbinding is visible.
 */
static void
texel_value(unsigned slot, unsigned x, unsigned y, unsigned z, float out[4])
{
   out[0] = 100.0f + x + 1000.0f * slot;
   out[1] = 200.0f + y;
   out[2] = 300.0f + x + y + z;
   out[3] = 400.0f;
}

#ifdef __APPLE__
/*
 * A program that dies on this port leaves nothing behind: the kernel says
 * "Failed to send exception EXC_CORPSE_NOTIFY" and that is the end of it. So
 * print the fault before dying. These two functions are userland/src/inject.c's
 * say_the_fault()/watch_for_faults() (branch sileo2, 6683521), with the slide
 * taken from this program's own image rather than from the shared cache, since
 * the pc of interest is in air_run or in libOSMesa and not in iOS.
 */
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
   ucontext_t *context = context_in;
   uintptr_t slide = (uintptr_t)_dyld_get_image_vmaddr_slide(0);
   uintptr_t pc = 0, lr = 0, fp = 0;

   if (context != NULL && context->uc_mcontext != NULL) {
      pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
      lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
      fp = (uintptr_t)context->uc_mcontext->__ss.__fp;
   }
   /* who called, by the frame pointers: on arm64 each frame holds the previous
    * at [fp] and its return address at [fp + 8]
    */
   char frames[8 * 20 + 1] = "";
   size_t used = 0;
   for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
      uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
      if (ret == 0)
         break;
      used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
      fp = next > fp ? next : 0;   /* the chain only ever goes up the stack */
   }
   fprintf(stderr, "air_run: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n",
           signal_number, info != NULL ? info->si_addr : NULL, pc - slide, lr - slide,
           frames);
   fflush(stderr);
   signal(signal_number, SIG_DFL);
   raise(signal_number);
}

static void
watch_for_faults(void)
{
   struct sigaction action;

   memset(&action, 0, sizeof(action));
   action.sa_sigaction = say_the_fault;
   action.sa_flags = SA_SIGINFO | SA_RESETHAND;
   sigaction(SIGSEGV, &action, NULL);
   sigaction(SIGBUS, &action, NULL);
}
#else
static void watch_for_faults(void) {}
#endif

/* set by --library: the fill below is then floats, for the reason given there */
static bool library_fill = false;

/* set by --zero: every buffer the shader reads is zeroed instead of patterned.
 *
 * For the 14 kernels whose loops leave on a count or a sentinel read out of a
 * buffer, the pattern is not an input at all -- the loop never ends and the GPU
 * runs forever (docs/air.md section 5, tools/air/kstruct.py). Zeros are a
 * degenerate input but a *defined* one: a count of zero means the loop runs no
 * times, so the kernel reaches its end and the hardware answers. What that
 * proves is that the translated shader runs on the Adreno without hanging; it
 * proves nothing about what the kernel computes, and the harness says so rather
 * than printing the zeros as a result.
 */
static bool zero_fill = false;

/* set by --ones: every byte is 0xff.
 *
 * The other half of the same idea. A loop that counts up to a value from a
 * buffer ends at once when that value is zero; a loop that walks until it meets
 * a sentinel -- MetalFaceMask's span tables leave on -1 -- never meets it in a
 * buffer of zeros, and ends at once in a buffer of 0xff. Neither fill is an
 * input the kernel was written for, and between them they let most of these
 * kernels reach their end instead of running forever. Which of the two a kernel
 * terminates under is itself a fact about its shape.
 */
static bool ones_fill = false;

/* The argument buffer of a wrapped library function holds floats, not the byte
 * pattern.
 *
 * Every one of these functions is compiled with no-nans-fp-math and
 * no-infs-fp-math: the module promises its inputs are finite. Arbitrary bytes
 * are mostly not -- they are infinities, NaNs and denormals -- so the inputs
 * would land outside the contract the shader was compiled under, where
 * 0 * inf may legally be either NaN or inf and a disagreement means nothing.
 * One case did disagree exactly there (folded NaN, softpipe +inf) and said
 * nothing about either side.
 *
 * Word w is ((w * 13) % 29 - 14) / 4: finite, exactly representable in
 * binary32, and different for 29 words running -- so a harness that mixed up
 * two slots, or two lanes of one slot, would get a different answer. cpuref.py
 * fills the same words.
 */
static uint8_t
library_byte(unsigned i)
{
   float f = (float)((int)((i / 4 * 13) % 29) - 14) / 4.0f;
   uint32_t u;
   memcpy(&u, &f, sizeof(u));
   return (u >> (8 * (i % 4))) & 0xff;
}

/* the pattern cpuref.py and air2nir --eval fill their buffers with, so that
 * all three agree on what the shader was given
 */
static uint8_t
pattern_byte(unsigned i)
{
   if (zero_fill)
      return 0;
   if (ones_fill)
      return 0xff;
   return library_fill ? library_byte(i) : (i * 7 + 13) & 0xff;
}

/* --buffer N@OFF=HEX puts those bytes into buffer N at that offset, over
 * whatever fill the rest of it keeps.
 *
 * It exists because the kernels that never finish do not need a whole buffer
 * chosen for them: they need a span table in one slot and a row index in
 * another, and the value their exit test compares against is a field or two
 * (tools/air/kinput.py works out which, and prints these arguments). A file is
 * one fill for every buffer, so it cannot say "this slot and not that one".
 * Offsets are decimal so that a job line stays readable; init.c splits a line
 * on blanks and knows no variables, so each kernel has to be one line.
 */
struct buffer_patch {
   unsigned index;
   unsigned off;
   unsigned len;
   uint8_t *bytes;
};
static struct buffer_patch patches[64];
static unsigned num_patches;

/* Parses "N@OFF=HEX". Returns false when the argument is not that shape, so
 * that --buffer keeps meaning a file name and a typo is not read as one.
 */
static bool
add_buffer_patch(const char *spec)
{
   char *end;
   unsigned long index = strtoul(spec, &end, 10);
   if (end == spec || *end != '@')
      return false;
   const char *p = end + 1;
   unsigned long off = strtoul(p, &end, 10);
   if (end == p || *end != '=')
      return false;

   const char *hex = end + 1;
   size_t n = strlen(hex);
   if (!n || n % 2)
      return false;
   for (size_t i = 0; i < n; i++)
      if (!isxdigit((unsigned char)hex[i]))
         return false;

   if (num_patches == ARRAY_SIZE(patches)) {
      fprintf(stderr, "air_run: more than %zu --buffer patches\n", ARRAY_SIZE(patches));
      exit(2);
   }
   struct buffer_patch *q = &patches[num_patches++];
   q->index = index;
   q->off = off;
   q->len = n / 2;
   q->bytes = malloc(q->len);
   for (unsigned i = 0; i < q->len; i++) {
      char byte[3] = { hex[2 * i], hex[2 * i + 1], 0 };
      q->bytes[i] = strtoul(byte, NULL, 16);
   }
   return true;
}

/* Writes the patches for one buffer and returns how many bytes they wrote.
 *
 * A patch that does not fit is refused out loud rather than clipped: these
 * bytes are what makes a loop end, and a kernel that silently did not get its
 * sentinel hangs the GPU, which the next boot pays for.
 */
static unsigned
apply_buffer_patches(unsigned index, uint8_t *bits, unsigned bytes)
{
   unsigned written = 0;
   for (unsigned i = 0; i < num_patches; i++) {
      const struct buffer_patch *q = &patches[i];
      if (q->index != index)
         continue;
      if (q->off + q->len > bytes) {
         printf("air_run: --buffer %u@%u= is %u bytes past the end of this %u-byte "
                "buffer and was NOT written\n", q->index, q->off,
                q->off + q->len - bytes, bytes);
         continue;
      }
      memcpy(bits + q->off, q->bytes, q->len);
      written += q->len;
   }
   return written;
}

/* Fill a buffer the shader is about to read.
 *
 * AIR_RUN_BUFFER=FILE puts that file's bytes there instead of the pattern,
 * which is what lets a test choose the arguments an operation is called on.
 * The reason it has to be a buffer and not a constant in the shader: ir3
 * folds, so a shader that computes air.fast_round(0.5) from an immediate is
 * answered by the compiler and the Adreno never runs the instruction -- the
 * test would pass without the hardware having been asked. A value loaded from
 * a buffer cannot be folded, and the harness knows what it put there.
 *
 * Returns how many bytes came from the file, so the caller can say so rather
 * than claim a fill it did not do. A file shorter than the buffer leaves the
 * rest as the pattern.
 */
static unsigned
fill_buffer(unsigned index, uint8_t *bits, unsigned bytes)
{
   for (unsigned k = 0; k < bytes; k++)
      bits[k] = pattern_byte(k);

   unsigned patched = apply_buffer_patches(index, bits, bytes);
   if (patched)
      printf("air_run: buffer at index %u, %u of its %u bytes from --buffer %u@...\n",
             index, patched, bytes, index);

   const char *path = getenv("AIR_RUN_BUFFER");
   if (!path || !*path)
      return 0;

   FILE *f = fopen(path, "rb");
   if (!f) {
      fprintf(stderr, "air_run: AIR_RUN_BUFFER=%s: %s\n", path, strerror(errno));
      return 0;
   }
   size_t got = fread(bits, 1, bytes, f);
   fclose(f);
   /* the file is the coarser statement -- one fill for every buffer -- so the
    * per-slot patches go on top of it and not under it
    */
   apply_buffer_patches(index, bits, bytes);
   return got;
}

/* A vertex shader for the quad: it takes the position as attribute 0 and
 * passes it through. Written here rather than translated, because what is
 * under test is the fragment shader; when the .metallib has a vertex shader of
 * its own that is a separate run.
 */
static nir_shader *
quad_vertex_shader(const nir_shader_compiler_options *options, unsigned num_varyings)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options, "quad");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "pos_in");
   in->data.location = VERT_ATTRIB_GENERIC0;
   in->data.driver_location = 0;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "pos_out");
   out->data.location = VARYING_SLOT_POS;
   out->data.driver_location = 0;
   nir_store_var(&b, out, nir_load_var(&b, in), 0xf);

   /* One constant coordinate per varying the fragment shader reads: the
    * centre of texel (TEX_X, TEX_Y), so every fragment samples the same
    * texel and the expected value is a single known one.
    */
   float c = (TEX_X + 0.5f) / TEX_SIZE;
   float d = (TEX_Y + 0.5f) / TEX_SIZE;
   for (unsigned i = 0; i < num_varyings; i++) {
      nir_variable *v = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "varying");
      v->data.location = VARYING_SLOT_VAR0 + i;
      v->data.driver_location = 1 + i;
      v->data.interpolation = INTERP_MODE_SMOOTH;
      nir_store_var(&b, v, nir_imm_vec4(&b, c, d, c, 1.0f), 0xf);
   }
   b.shader->num_inputs = 1;
   b.shader->num_outputs = 1 + num_varyings;
   return b.shader;
}

#include "air_run_gallium.inc"
#include "air_run_compute.inc"

int
main(int argc, char **argv)
{
   /* --disasm: ask ir3 to print the Adreno instructions it generates. The
    * screen's name proves fd_screen_create won; it does not prove that the
    * shader itself went through ir3 rather than some later software path, and
    * the four floats look identical either way. ir3's own disassembly in the
    * log is the positive proof. IR3_SHADER_DEBUG is read once, at compiler
    * creation, so it has to be set before the screen is made.
    */
   bool want_disasm = false;
   /* --library runs an air.visible library function through the generated
    * wrapper (nir_air.h): there is nothing to dispatch otherwise, so these 135
    * modules were never run at all.
    */
   bool want_library = false;
   /* --buffer FILE is AIR_RUN_BUFFER as an argument, and it exists because the
    * device has no shell to set one with. iOS 16.5's own root carries no sh at
    * all -- /bin holds df and ps -- and the job list that runs this is read by
    * our init, which splits a line on blanks and has no notion of a variable
    * (userland/src/init.c, run_command). So a generated suite whose every
    * module reads its own bytes could not be run on the phone until this
    * option existed: with it each module is one ordinary job line.
    */
   while (argc > 1 && argv[1][0] == '-') {
      if (!strcmp(argv[1], "--disasm")) {
         want_disasm = true;
         setenv("IR3_SHADER_DEBUG", "disasm", 1);
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--zero")) {
         zero_fill = true;
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--ones")) {
         ones_fill = true;
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--library")) {
         want_library = library_fill = true;
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--block") && argc > 2) {
         unsigned x = 0, y = 0, z = 0;

         if (sscanf(argv[2], "%u,%u,%u", &x, &y, &z) != 3 || !x || !y || !z) {
            fprintf(stderr, "air_run: --block wants X,Y,Z and all three above zero\n");
            return 2;
         }
         block[0] = x, block[1] = y, block[2] = z;
         argv += 2;
         argc -= 2;
      } else if (!strcmp(argv[1], "--buffer") && argc > 2) {
         if (!add_buffer_patch(argv[2]))
            setenv("AIR_RUN_BUFFER", argv[2], 1);
         argv += 2;
         argc -= 2;
      } else {
         break;
      }
   }
   if (argc < 2) {
      fprintf(stderr, "usage: air_run [--disasm] [--library] [--block X,Y,Z] "
                      "[--buffer FILE | --buffer N@OFF=HEX] "
                      "[--zero | --ones] FILE[.metallib] [FUNCTION]\n");
      return 2;
   }

   watch_for_faults();
   glsl_type_singleton_init_or_ref();
   void *ctx = ralloc_context(NULL);

   /* the module */
   FILE *f = fopen(argv[1], "rb");
   if (!f) {
      perror(argv[1]);
      return 1;
   }
   fseek(f, 0, SEEK_END);
   size_t size = ftell(f);
   fseek(f, 0, SEEK_SET);
   void *data = ralloc_size(ctx, size);
   if (fread(data, 1, size, f) != size) {
      fprintf(stderr, "air_run: short read of %s\n", argv[1]);
      return 1;
   }
   fclose(f);

   const void *bitcode = data;
   size_t bitcode_size = size;
   const char *want = argc > 2 ? argv[2] : NULL;
   const char *name = argv[1];
   if (size >= 4 && !memcmp(data, "MTLB", 4)) {
      struct air_metallib lib;
      const char *err;
      if (!air_metallib_parse(ctx, data, size, &lib, &err)) {
         fprintf(stderr, "air_run: %s: %s\n", argv[1], err);
         return 1;
      }
      unsigned pick = lib.num_functions;
      for (unsigned i = 0; i < lib.num_functions; i++) {
         if (!want || !strcmp(want, lib.functions[i].name)) {
            pick = i;
            break;
         }
      }
      if (pick == lib.num_functions) {
         fprintf(stderr, "air_run: no function %s\n", want ? want : "(any)");
         return 1;
      }
      bitcode = lib.functions[pick].bitcode;
      bitcode_size = lib.functions[pick].bitcode_size;
      name = lib.functions[pick].name;
   }

   /* The screen: the GPU's where the kernel has one to drive, softpipe
    * otherwise. The same source therefore runs on this machine (no msm
    * device, so it falls back and draws in software) and on the phone, where
    * it opens the Adreno exactly as the port's own Mesa does
    * (osmesa_create_gpu_screen in tools/mesa/freedreno-darwin.patch).
    * AIR_RUN_SOFTWARE=1 forces software even where a GPU is present.
    */
   struct pipe_screen *screen = NULL;
   const char *how = "software";
#ifdef GALLIUM_FREEDRENO
   if (!debug_get_bool_option("AIR_RUN_SOFTWARE", false)) {
      int fd = drmOpenWithType("msm", NULL, DRM_NODE_RENDER);
      if (fd >= 0) {
         /* The driver option cache has to be real, not a zeroed struct:
          * fd_screen_create passes it straight to driParseConfigFiles, which
          * dereferences it. A zeroed one died at address 0x10 on the phone --
          * the first thing this program ever did on a machine that has a GPU,
          * and invisible on any machine that has not. This is the sequence
          * osmesa_create_gpu_screen() uses in tools/mesa/freedreno-darwin.patch,
          * which is the one CoreAnimation goes through on this device daily.
          */
         static const driOptionDescription freedreno_options[] = {
#include "freedreno/driinfo_freedreno.h"
         };
         static driOptionCache info, cache;
         static struct pipe_screen_config config;

         const driConfigFileParseParams params = {
            .screenNum = 0,
            .driverName = "msm",
         };
         driParseOptionInfo(&info, freedreno_options, ARRAY_SIZE(freedreno_options));
         driParseConfigFiles(&cache, &info, &params);
         config.options = &cache;
         config.options_info = &info;

         screen = fd_drm_screen_create_renderonly(fd, NULL, &config);
         if (screen)
            how = "the GPU, through the kernel's msm device";
         else
            fprintf(stderr, "air_run: the msm device opened but gave no screen\n");
      }
   }
#endif
   if (!screen)
      screen = sw_screen_create(null_sw_create());
   if (!screen) {
      fprintf(stderr, "air_run: no gallium screen\n");
      return 1;
   }
   printf("air_run: %s on %s (%s)\n", name, screen->get_name(screen), how);

   /* 26.x filled these into a table on the screen at its creation instead of
    * answering a call, with a null entry for a stage it has no NIR for -- which is
    * exactly what the check below reads as "no NIR here". */
   const void *options = screen->nir_options[MESA_SHADER_FRAGMENT];
   if (!options) {
      fprintf(stderr, "air_run: this screen takes no NIR\n");
      return 1;
   }

   /* the shader */
   struct air_constant_value *fc;
   unsigned num_fc = air_module_function_constants(ctx, bitcode, bitcode_size, &fc);
   struct air_to_nir_options air_options = { .constants = fc, .num_constants = num_fc,
                                            .library_wrapper = want_library };
   struct air_shader_info info;
   const char *err;
   nir_shader *fs = air_to_nir(ctx, bitcode, bitcode_size, NULL, &air_options, options,
                               &info, &err);
   if (!fs) {
      fprintf(stderr, "air_run: %s: %s\n", name, err);
      return 1;
   }
   if (info.stage == MESA_SHADER_COMPUTE)
      return dispatch_kernel(screen, &info, fs, name, how);
   if (info.stage != MESA_SHADER_FRAGMENT) {
      fprintf(stderr, "air_run: %s is a %s shader; only fragment shaders are drawn here\n",
              name, _mesa_shader_stage_to_string(info.stage));
      return 1;
   }
   /* The render target's format follows the shader's own output type. A Metal
    * fragment shader returning uint4 writes integers, and reading those back
    * through a float target gives the right bits read the wrong way -- which
    * looks exactly like a translation bug and is not one.
    */
   enum pipe_format rt_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   bool rt_is_float = true;
   bool rt_is_half = false;
   /* Which attachment to read back: the lowest one the shader writes, not
    * target 0. clearFragment04 writes FRAG_RESULT_DATA1 alone, and target 0
    * then holds the clear colour -- a correct result that looks exactly like
    * a shader which stored nothing.
    */
   unsigned read_rt = ~0u;
   nir_foreach_variable_with_modes(var, fs, nir_var_shader_out) {
      if (var->data.location < FRAG_RESULT_DATA0)
         continue;
      unsigned idx = var->data.location - FRAG_RESULT_DATA0;
      if (idx >= read_rt)
         continue;
      read_rt = idx;
      enum glsl_base_type base = glsl_get_base_type(glsl_without_array(var->type));
      if (base != GLSL_TYPE_FLOAT && base != GLSL_TYPE_FLOAT16 &&
          base != GLSL_TYPE_DOUBLE) {
         /* unsigned, not signed: an LLVM integer carries no signedness, so
          * both this and tools/air/cpuref.py print the bits unsigned and the
          * two can be compared
          */
         rt_format = PIPE_FORMAT_R32G32B32A32_UINT;
         rt_is_float = false;
      } else if (base == GLSL_TYPE_FLOAT16) {
         /* A half return needs a half target. Through a 32-bit one the half is
          * widened the moment it is written, and NIR then folds the conversion
          * that produced it into a 32-bit one: the generated Adreno code for
          * air.convert.f.f16.u.i32 was four cov.u32f32 and not one cov.u32f16,
          * so the shader computed f32(x) while the reference computed f16(x)
          * and the comparison meant nothing. Ten declarations of the corpus
          * return half and every one of them was being reported as agreeing on
          * the strength of a case that was never compared (docs/air.md,
          * section 4 item 4).
          *
          * The chain is still u32 -> f32 -> f16 rather than a single
          * conversion wherever NIR widens before the store, so a value needing
          * to round at both steps can differ from one direct conversion by
          * double rounding. None of the pool's values does, but a future one
          * might, and that is a property of the harness and not of the GPU.
          */
         /* As of now nothing reaches this branch: tools/air/intrincheck.py packs
          * every float-ish result into a float4, so no generated shader declares
          * a half output. It is here because the target was one of the two things
          * missing and is the half that belongs on this side; the other half needs
          * a half render target in the generator, whose air.arg_type_name the
          * corpus does not attest (docs/air.md, section 4 item 4).
          */
         rt_format = PIPE_FORMAT_R16G16B16A16_FLOAT;
         rt_is_float = true;
         rt_is_half = true;
      } else {
         rt_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
         rt_is_float = true;
      }
   }
   if (read_rt == ~0u) {
      fprintf(stderr, "air_run: %s writes no colour attachment\n", name);
      return 1;
   }

   /* A shader may declare a render target and never store to it:
    * texturedFragment3DDepth returns {undef, depth} and its only real output
    * is FRAG_RESULT_DEPTH. Reading the colour then gives the clear value,
    * which is a number that means nothing -- worse than no number, because it
    * looks like a result. This harness binds no depth attachment, so say so.
    */
   bool stores_colour = false;
   nir_foreach_function_impl(impl, fs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_deref_instr *d = nir_src_as_deref(intr->src[0]);
            if (d && d->deref_type == nir_deref_type_var &&
                d->var->data.location == (int)(FRAG_RESULT_DATA0 + read_rt))
               stores_colour = true;
         }
      }
   }
   if (!stores_colour) {
      fprintf(stderr, "air_run: %s stores nothing to a colour attachment -- its output is "
                      "depth or stencil, which this harness does not read back\n", name);
      return 1;
   }

   /* A shader that discards cannot be run by this screen, and that is
    * softpipe's limit rather than the frontend's. air.discard_fragment becomes
    * NIR's `demote` (air_intrinsics.c says at length why it is demote and not
    * terminate); nir_to_tgsi sends it to TGSI_OPCODE_DEMOTE, which the
    * interpreter does not implement -- tgsi_exec.c has a case for KILL and
    * none for DEMOTE, so exec_instruction falls through to its `assert(0)` and
    * the whole run aborts at tgsi_exec.c:5943. An abort inside a check reads
    * like a crash in the thing being checked, so it is refused here by name,
    * the way the multisample textures below are.
    *
    * ir3 has both instructions and compiles these, so what is open is the
    * VALUE a discarding shader leaves in the framebuffer, and that is a
    * question for the phone.
    */
   if (fs->info.fs.uses_discard) {
      fprintf(stderr, "air_run: %s discards fragments, and this screen's TGSI "
                      "interpreter has no DEMOTE case at all (tgsi_exec.c), so it is "
                      "not run rather than run against the wrong thing\n", name);
      return 1;
   }

   /* And the rest of the family this interpreter does not have.
    *
    * `grep -c TGSI_OPCODE_VOTE_ALL tgsi_exec.c` is 0, and so are BALLOT,
    * READ_FIRST, READ_INVOC, READ_HELPER and DEMOTE: nir_to_tgsi emits all of
    * them and softpipe's exec_instruction implements none, so it falls through
    * to its own assert(0) -- an abort in the middle of a check, which reads
    * like a crash in the thing being checked rather than like a screen that
    * cannot do this. The quad swaps are worse again: nir_to_tgsi has no case
    * for them either, prints "Unknown intrinsic" and then asserts inside
    * ureg_swizzle on the register it never made.
    *
    * So a shader that asks for a cross-lane value is refused here by name. It
    * is not a frontend limit and not a hardware one -- ir3 has real
    * instructions for every one of these, and compiles them -- which is why
    * the refusal names the screen.
    */
   const char *no_lanes = NULL;
   nir_foreach_function_impl(impl, fs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_op op = nir_instr_as_intrinsic(instr)->intrinsic;
            switch (op) {
            case nir_intrinsic_vote_all:
            case nir_intrinsic_vote_any:
            case nir_intrinsic_vote_feq:
            case nir_intrinsic_vote_ieq:
            case nir_intrinsic_ballot:
            case nir_intrinsic_read_first_invocation:
            case nir_intrinsic_read_invocation:
            case nir_intrinsic_reduce:
            case nir_intrinsic_inclusive_scan:
            case nir_intrinsic_exclusive_scan:
            case nir_intrinsic_shuffle:
            case nir_intrinsic_shuffle_xor:
            case nir_intrinsic_shuffle_up:
            case nir_intrinsic_shuffle_down:
            case nir_intrinsic_quad_broadcast:
            case nir_intrinsic_quad_swap_horizontal:
            case nir_intrinsic_quad_swap_vertical:
            case nir_intrinsic_quad_swap_diagonal:
            case nir_intrinsic_is_helper_invocation:
               no_lanes = nir_intrinsic_infos[op].name;
               break;
            default:
               break;
            }
            if (no_lanes)
               break;
         }
         if (no_lanes)
            break;
      }
      if (no_lanes)
         break;
   }
   if (no_lanes) {
      fprintf(stderr, "air_run: %s reads another lane (NIR's %s), and this screen's "
                      "TGSI interpreter implements none of the subgroup or quad "
                      "opcodes nir_to_tgsi emits for them, so it is not run rather "
                      "than aborting inside softpipe\n", name, no_lanes);
      return 1;
   }

   finalize_for_gallium(fs, screen);

   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);

   if (!pipe || !bind_software_constants(pipe, fs)) {
      if (pipe) pipe->destroy(pipe);
      screen->destroy(screen);
      ralloc_free(ctx);
      glsl_type_singleton_decref();
      return 1;
   }

   /* One target per render target the shader writes: a shader whose outputs
    * outnumber the framebuffer's attachments draws nothing at all, which
    * looks like a translation that produced no stores.
    */
   unsigned num_rt = MAX2(info.num_render_targets, read_rt + 1);
   if (num_rt > PIPE_MAX_COLOR_BUFS) {
      fprintf(stderr, "air_run: %u render targets, more than this can bind\n", num_rt);
      return 1;
   }
   struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = rt_format,
      .width0 = WIDTH,
      .height0 = HEIGHT,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
   };
   struct pipe_resource *target = NULL;
   struct pipe_framebuffer_state fb = {
      .width = WIDTH, .height = HEIGHT, .nr_cbufs = num_rt,
   };
   for (unsigned i = 0; i < num_rt; i++) {
      struct pipe_resource *res = screen->resource_create(screen, &templ);
      /* A surface is data the caller owns now: it is filled here, handed to the
       * context by value below, and its one reference -- the texture -- is this
       * loop's to keep, which is why the resource is never released either: the
       * framebuffer is dropped when the process ends. */
      pipe_surface_init(pipe, &fb.cbufs[i], res, 0, 0);
      fb.cbufs[i].format = templ.format;
      if (i == read_rt)
         target = res;
   }
   pipe->set_framebuffer_state(pipe, &fb);

   pipe->set_viewport_states(pipe, 0, 1, &(struct pipe_viewport_state){
      /* The y scale is NEGATIVE, and that is Metal's rule rather than a convention picked
       * here. Metal's render-target origin is the top left and its NDC y points up, so clip
       * y = +1 belongs in memory row 0; a positive y scale puts clip y = -1 there, which is
       * OpenGL's drawable rule and what this code did until now (docs/metal.md 10.28). Mesa
       * negates y in exactly this place, for exactly this reason, when it draws to a window.
       *
       * It is landed only now because until the varying was fixed (10.32) nothing could see
       * it: every draw of the corpus either writes a constant or samples one fixed texel, and
       * over a constant an orientation is invisible. The sixteen-pixel grid of 10.32 is what
       * it is landed against -- it must turn from (100+x, 200+y, ...) into
       * (100+x, 200+(3-y), ...), the texture mirrored vertically, on both back ends.
       */
      .scale = { WIDTH / 2.0f, -(HEIGHT / 2.0f), 1.0f },
      .translate = { WIDTH / 2.0f, HEIGHT / 2.0f, 0.0f },
   });

   void *blend = pipe->create_blend_state(pipe, &(struct pipe_blend_state){
      .rt[0].colormask = PIPE_MASK_RGBA,
   });
   pipe->bind_blend_state(pipe, blend);
   void *dsa = pipe->create_depth_stencil_alpha_state(pipe,
      &(struct pipe_depth_stencil_alpha_state){ 0 });
   pipe->bind_depth_stencil_alpha_state(pipe, dsa);
   void *rast = pipe->create_rasterizer_state(pipe, &(struct pipe_rasterizer_state){
      .cull_face = PIPE_FACE_NONE, .half_pixel_center = 1, .bottom_edge_rule = 1,
      .depth_clip_near = 1, .depth_clip_far = 1,
   });
   pipe->bind_rasterizer_state(pipe, rast);

   /* every constant buffer the shader reads, filled with the pattern */
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];
      if (r->kind != AIR_RESOURCE_CONSTANT_BUFFER && r->kind != AIR_RESOURCE_DEVICE_BUFFER)
         continue;
      unsigned bytes = r->size ? r->size : 256;
      uint8_t *bits = malloc(bytes);
      unsigned from_file = fill_buffer(r->index, bits, bytes);
      struct pipe_resource *buf = pipe_buffer_create(screen, PIPE_BIND_CONSTANT_BUFFER,
                                                     PIPE_USAGE_DEFAULT, bytes);
      pipe_buffer_write(pipe, buf, 0, bytes, bits);
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, r->slot,
                                &(struct pipe_constant_buffer){
                                   .buffer = buf, .buffer_size = bytes,
                                });
      if (from_file)
         printf("air_run: %s buffer at index %u, %u bytes, %u of them from "
                "AIR_RUN_BUFFER\n",
                r->kind == AIR_RESOURCE_CONSTANT_BUFFER ? "constant" : "device",
                r->index, bytes, from_file);
      else
         printf("air_run: %s buffer at index %u, %u bytes of (i * 7 + 13) & 0xff\n",
                r->kind == AIR_RESOURCE_CONSTANT_BUFFER ? "constant" : "device",
                r->index, bytes);
      free(bits);
   }

   /* every sampled texture the shader declares, and a sampler for it */
   unsigned num_tex = 0;
   for (unsigned i = 0; i < info.num_resources; i++)
      if (info.resources[i].kind == AIR_RESOURCE_TEXTURE)
         num_tex = MAX2(num_tex, info.resources[i].index + 1);
   if (num_tex) {
      /* The texture's format follows what the shader samples into: a Metal
       * texture2d<uint> read out of a float texture gives numbers that are
       * neither right nor obviously wrong.
       */
      enum pipe_format tex_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
      bool tex_is_float = true;
      /* and the target, which has to match too: a 3D sample from a 2D
       * texture can come back right by accident, which is not evidence
       */
      enum pipe_texture_target target = PIPE_TEXTURE_2D;
      enum glsl_sampler_dim dim = GLSL_SAMPLER_DIM_2D;
      bool is_array = false;
      /* A SHADOW SAMPLE WANTS A DEPTH TEXTURE, and binding a colour one is not a
       * harmless approximation: GL and Vulkan both leave a shadow lookup on a
       * non-depth texture undefined, and the Adreno says so by ignoring the
       * comparison. The first device run of these cases came back with the TEXEL
       * -- 101 where a comparison must answer 0 or 1, and for `always` and `never`
       * too, which no compare function and no clamp can explain -- against
       * r32g32b32a32_float bound for a Metal depth2d<float>, which is the one thing
       * in that run that was this harness's own doing (docs/air.md 3.3).
       *
       * So a slot a shadow instruction reads gets a real depth texture, and its
       * texels are the only pattern that fits: (x + TEX*y) / 16, which is sixteen
       * distinct values, every one exactly representable in binary32 and inside the
       * [0, 1] a depth format is normalised to. The colour pattern cannot be used --
       * 100 does not fit in a depth texel at all.
       */
      bool shadow = false;
      nir_foreach_function_impl(impl, fs) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type == nir_instr_type_tex &&
                   nir_instr_as_tex(instr)->is_shadow)
                  shadow = true;
            }
         }
      }
      nir_foreach_function_impl(impl, fs) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_tex)
                  continue;
               nir_tex_instr *t = nir_instr_as_tex(instr);
               nir_alu_type base = nir_alu_type_get_base_type(t->dest_type);
               if (base == nir_type_int) {
                  tex_format = PIPE_FORMAT_R32G32B32A32_SINT;
                  tex_is_float = false;
               } else if (base == nir_type_uint) {
                  tex_format = PIPE_FORMAT_R32G32B32A32_UINT;
                  tex_is_float = false;
               }
               dim = t->sampler_dim;
               is_array = t->is_array;
               if (dim == GLSL_SAMPLER_DIM_3D)
                  target = PIPE_TEXTURE_3D;
               else if (dim == GLSL_SAMPLER_DIM_MS)
                  target = PIPE_TEXTURE_2D;   /* refused below */
               goto have_format;
            }
         }
      }
   have_format:;
      /* and a depth format where a comparison reads it, whatever the dest_type said:
       * a shadow sample's result is a comparison and not a texel, so the element type
       * the instruction carries says nothing about the texture's format.
       */
      if (shadow) {
         if (dim != GLSL_SAMPLER_DIM_2D || is_array) {
            fprintf(stderr, "air_run: %s compares against a texture this harness makes no "
                            "depth version of (dim %u%s); it is not run rather than run "
                            "against a colour texture, which is what the Adreno answers "
                            "with the texel\n", name, dim, is_array ? ", array" : "");
            return 1;
         }
         tex_format = PIPE_FORMAT_Z32_FLOAT;
         tex_is_float = true;
      }
      if (dim == GLSL_SAMPLER_DIM_MS) {
         fprintf(stderr, "air_run: %s samples a multisample texture; this harness binds "
                         "none, so it is not run rather than run against the wrong thing\n",
                 name);
         return 1;
      }
      if (dim != GLSL_SAMPLER_DIM_1D && dim != GLSL_SAMPLER_DIM_2D &&
          dim != GLSL_SAMPLER_DIM_3D) {
         fprintf(stderr, "air_run: %s samples a texture of a kind this harness does not "
                         "bind (dim %u)\n", name, dim);
         return 1;
      }

      /* An ARRAY texture is a different resource and not a 2D one sampled with
       * a third coordinate, so the harness has to make one or the check would
       * be about nothing: a layer index handed to a 2D texture is ignored, and
       * every layer would answer with layer 0's texels. TEX_SIZE layers, filled
       * by the same rule as a 3D texture's slices -- texel (x, y, z) of slot n
       * is (100+x+1000n, 200+y, 300+x+y+z, 400) -- so a lowering that dropped
       * the layer, or put it in the wrong component, comes back with a number
       * from the wrong slice rather than a plausible one.
       *
       * Gallium addresses an array's layers through pipe_box.z and .depth, the
       * same fields a 3D texture's slices use, while the RESOURCE carries them
       * in array_size rather than depth0 -- which is why depth0 and the box's
       * depth part company here.
       */
      if (is_array)
         target = dim == GLSL_SAMPLER_DIM_1D ? PIPE_TEXTURE_1D_ARRAY
                                             : PIPE_TEXTURE_2D_ARRAY;
      else if (dim == GLSL_SAMPLER_DIM_1D)
         target = PIPE_TEXTURE_1D;
      unsigned height = dim == GLSL_SAMPLER_DIM_1D ? 1 : TEX_SIZE;
      unsigned slices = (target == PIPE_TEXTURE_3D || is_array) ? TEX_SIZE : 1;
      unsigned tex_z = slices > 1 ? TEX_Y : 0;

      struct pipe_resource ttempl = {
         .target = target,
         .format = tex_format,
         .width0 = TEX_SIZE, .height0 = height,
         .depth0 = is_array ? 1 : slices,
         .array_size = is_array ? slices : 1,
         .bind = PIPE_BIND_SAMPLER_VIEW,
      };
      struct pipe_box box = { .width = TEX_SIZE, .height = height, .depth = slices };
      struct pipe_resource *texes[PIPE_MAX_SAMPLERS] = { 0 };
      for (unsigned slot = 0; slot < num_tex && slot < PIPE_MAX_SAMPLERS; slot++) {
         float texels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
         int32_t itexels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
         texes[slot] = screen->resource_create(screen, &ttempl);
         if (shadow) {
            /* one float per texel, not four, and a depth row is a quarter as wide */
            for (unsigned y = 0; y < TEX_SIZE; y++)
               for (unsigned x = 0; x < TEX_SIZE; x++)
                  texels[y * TEX_SIZE + x] = depth_texel_value(x, y);
            pipe->texture_subdata(pipe, texes[slot], 0, PIPE_MAP_WRITE, &box, texels,
                                  TEX_SIZE * sizeof(float),
                                  TEX_SIZE * TEX_SIZE * sizeof(float));
            continue;
         }
         for (unsigned z = 0; z < slices; z++) {
            for (unsigned y = 0; y < height; y++) {
               for (unsigned x = 0; x < TEX_SIZE; x++) {
                  unsigned i = ((z * height + y) * TEX_SIZE + x) * 4;
                  texel_value(slot, x, y, z, &texels[i]);
                  for (unsigned c = 0; c < 4; c++)
                     itexels[i + c] = (int32_t)texels[i + c];
               }
            }
         }
         pipe->texture_subdata(pipe, texes[slot], 0, PIPE_MAP_WRITE, &box,
                               tex_is_float ? (const void *)texels : (const void *)itexels,
                               TEX_SIZE * 4 * sizeof(float),
                               height * TEX_SIZE * 4 * sizeof(float));
      }

      /* the API's sampler where the shader declares none, and its own where it
       * does -- per slot, since a shader may mix the two
       */

      struct pipe_sampler_view *views[PIPE_MAX_SHADER_SAMPLER_VIEWS] = { 0 };
      void *samplers[PIPE_MAX_SAMPLERS] = { 0 };
      bool compare_slot[PIPE_MAX_SAMPLERS] = { 0 };
      compare_sampled_slots(fs, compare_slot, PIPE_MAX_SAMPLERS);
      if (compare_without_a_function(&info, compare_slot, PIPE_MAX_SAMPLERS, name))
         return 1;
      for (unsigned i = 0; i < num_tex && i < PIPE_MAX_SAMPLERS; i++) {
         struct pipe_sampler_view vtempl = { 0 };
         u_sampler_view_default_template(&vtempl, texes[i], texes[i]->format);
         views[i] = pipe->create_sampler_view(pipe, texes[i], &vtempl);
         const struct air_sampler_state *own = NULL;
         struct pipe_sampler_state sst = sampler_for_slot(&info, i, false,
                                                          compare_slot[i], &own);
         samplers[i] = pipe->create_sampler_state(pipe, &sst);
         if (own)
            print_own_sampler(i, own, compare_slot[i]);
      }
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, num_tex, 0, views);
      pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, num_tex, samplers);

      float want[4];
      texel_value(0, TEX_X, TEX_Y, tex_z, want);
      if (shadow)
         printf("air_run: %u DEPTH texture%s bound: %ux%u %s, texel (x,y) = (x + 4y)/16, "
                "because a shadow sample of a colour texture is undefined and the Adreno "
                "answers it with the texel\n", num_tex, num_tex == 1 ? "" : "s",
                TEX_SIZE, TEX_SIZE, util_format_short_name(tex_format));
      else
      printf("air_run: %u texture%s bound: %ux%ux%u %s%s, texel (x,y,z) of slot n = "
             "(100+x+1000n, 200+y, 300+x+y+z, 400), nearest, clamped\n",
             num_tex, num_tex == 1 ? "" : "s", TEX_SIZE, height, slices,
             util_format_short_name(tex_format),
             is_array ? " array (z is the layer)" : "");
      if (!shadow)
         printf("air_run: expected %.9g %.9g %.9g %.9g  (the centre of texel %u,%u,%u "
                "sampled once)\n", (double)want[0], (double)want[1], (double)want[2],
                (double)want[3], TEX_X, TEX_Y, tex_z);
      /* A comparison's result is not a texel, so no "expected" line is printed for one:
       * the first device run of these cases printed the colour pattern's texel there and
       * the GPU then answered WITH that texel, which read as agreement until the numbers
       * were looked at. The line said what the comparison reads and the device said what
       * it returned, and they were the same sentence.
       */
      for (unsigned i = 0; i < num_tex && i < PIPE_MAX_SAMPLERS; i++)
         if (compare_slot[i]) {
            printf("air_run: sampler %u COMPARES against depth texel (%u,%u) = %.9g, so "
                   "the result is a 0, a 1 or a filtered fraction of them and never a "
                   "texel\n", i, TEX_X, TEX_Y,
                   (double)depth_texel_value(TEX_X, TEX_Y));
            break;
         }
   }

   /* the quad */
   static const float verts[] = {
      -1, -1, 0, 1,   1, -1, 0, 1,   -1, 1, 0, 1,   1, 1, 0, 1,
   };
   struct pipe_resource *vbuf = pipe_buffer_create(screen, PIPE_BIND_VERTEX_BUFFER,
                                                   PIPE_USAGE_DEFAULT, sizeof(verts));
   pipe_buffer_write(pipe, vbuf, 0, sizeof(verts), verts);
   pipe->set_vertex_buffers(pipe, 1, &(struct pipe_vertex_buffer){
      .buffer.resource = vbuf, .buffer_offset = 0,
   });
   void *velem = pipe->create_vertex_elements_state(pipe, 1,
      &(struct pipe_vertex_element){
         .src_offset = 0, .src_stride = 16, .vertex_buffer_index = 0,
         .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
      });
   pipe->bind_vertex_elements_state(pipe, velem);

   nir_shader *vs = quad_vertex_shader(options, info.num_varyings);
   finalize_for_gallium(vs, screen);
   void *vs_cso = pipe->create_vs_state(pipe, &(struct pipe_shader_state){
      .type = PIPE_SHADER_IR_NIR, .ir.nir = vs,
   });
   pipe->bind_vs_state(pipe, vs_cso);
   void *fs_cso = pipe->create_fs_state(pipe, &(struct pipe_shader_state){
      .type = PIPE_SHADER_IR_NIR, .ir.nir = fs,
   });
   pipe->bind_fs_state(pipe, fs_cso);

   union pipe_color_union clear = { .ui = { 0, 0, 0, 0 } };
   /* 26.x names the channels and stencil bits to write instead of taking a surface to
    * write them through (p_context.h). 0xf is all four channels and 0xff all stencil
    * bits, which is the whole-surface clearing the surface-carrying form did; the
    * buffers mask still decides which attachments are touched at all.
    */
   pipe->clear(pipe, PIPE_CLEAR_COLOR, 0xf, 0xff, NULL, &clear, 0.0, 0);

   struct pipe_draw_info draw = {
      .mode = MESA_PRIM_TRIANGLE_STRIP, .index_bounds_valid = true,
      .max_index = 3, .instance_count = 1,
   };
   struct pipe_draw_start_count_bias sc = { .start = 0, .count = 4 };
   pipe->draw_vbo(pipe, &draw, 0, NULL, &sc, 1);
   if (!finish_or_say_why(screen, pipe, "draw")) {
      fprintf(stderr, "air_run: %s: the draw did not finish, so no pixel here is "
                      "evidence\n", name);
      return 1;
   }

   /* the pixel */
   struct pipe_transfer *xfer;
   void *pix = pipe->texture_map(pipe, target, 0, PIPE_MAP_READ,
                                 &(struct pipe_box){ .width = WIDTH, .height = HEIGHT,
                                                     .depth = 1 }, &xfer);
   if (!pix) {
      fprintf(stderr, "air_run: the render target could not be read back\n");
      return 1;
   }
   /* Named again here, beside the numbers: the line at the top says which
    * screen was created, this one says which screen the pixel just came out
    * of. Neither is proof that no stage in between went software -- --disasm
    * is, by making ir3 print what it compiled.
    */
   printf("air_run: read back from %s (%s)%s\n", screen->get_name(screen), how,
          want_disasm ? ", ir3's instructions are above" : "");
   printf("air_run: the pixel at (0, 0) of render target %u came back as:\n",
          read_rt);
   for (unsigned c = 0; c < 4; c++) {
      if (rt_is_half)
         printf("  %.9g\n", (double)_mesa_half_to_float(((uint16_t *)pix)[c]));
      else if (rt_is_float)
         printf("  %.9g\n", (double)((float *)pix)[c]);
      else
         printf("  %u\n", ((uint32_t *)pix)[c]);
   }
   pipe->texture_unmap(pipe, xfer);

   printf("air_run: drew %ux%u into %s, %u render target%s\n",
          WIDTH, HEIGHT, util_format_short_name(rt_format), num_rt,
          num_rt == 1 ? "" : "s");

   pipe->destroy(pipe);
   screen->destroy(screen);
   ralloc_free(ctx);
   glsl_type_singleton_decref();
   return 0;
}
