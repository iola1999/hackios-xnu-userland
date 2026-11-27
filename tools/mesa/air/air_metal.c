/*
 * Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * libannibaleair: air_run's compute path, as a library a Metal driver calls.
 *
 * The contract is air_metal.h and nothing else -- no NIR, no Gallium, no ir3 in the
 * caller's view -- because the Metal side (userland/src/annibalemetal.m) must not grow a
 * dependency on Mesa's headers and this side must stay free to change how it compiles.
 *
 * Almost everything here is lifted from air_run.c (tools/mesa/air/air_run.c), which has
 * done all of it as a test program since the GPU first took work: the screen, the
 * translation, the compute state, the grid and the fence. The comments that say *why*
 * came with the code and are kept, because every one of them is a mistake someone
 * already made. What stayed in air_run is what makes it a harness and not a driver:
 * filling buffers with a pattern so a wrong offset shows, binding a texture to every
 * slot, printing the result, --zero/--ones, drawing a quad.
 *
 * What a driver gets that air_run does not: failure as a sentence rather than a line on
 * stdout. A library in another process's address space must not print; it also must not
 * return a code nobody can read out loud, because the caller's next move is to put the
 * reason in an NSError that an application will show a person.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <unistd.h>

#include "air_metal.h"
#include "air_clear_plan.h"
#include "air_metal_yuv.h"
#include "air_metal_rgb2p.h"
#include "air_metal_purgeable.h"
/* One pipe_context serves the whole process, and a Gallium context is not thread safe. Every entry
 * point that can reach it takes this lock for the length of the call: CoreAnimation calls in from
 * more than one thread, and on 2026-10-30 backboardd died in fd6_draw_vbos with ctx->batch NULL
 * while the driver's commits were already serialized. Recursive, because entry points call one
 * another. */
static pthread_mutex_t rwa_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static void
rwa_unlock_scope(int *unused)
{
   (void)unused;
   pthread_mutex_unlock(&rwa_lock);
}

/* The first statement of an entry point: held until the function returns, by whichever path. */
#define RWA_LOCKED \
   int rwa_locked_ __attribute__((cleanup(rwa_unlock_scope))) = (pthread_mutex_lock(&rwa_lock), 0); \
   (void)rwa_locked_

/* Narrower locks to keep background CSO builds and foreground resource destruction off rwa_lock.
 * Lock order (outer -> inner): rwa_cso_create_lock -> rwa_vcso_lock;
 * rwa_lock -> { rwa_vcso_lock, rwa_buffer_textures_lock, rwa_argbuf_lock, rwa_deferred_lock }. */
static pthread_mutex_t rwa_cso_create_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rwa_vcso_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rwa_buffer_textures_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rwa_argbuf_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rwa_deferred_lock = PTHREAD_MUTEX_INITIALIZER;

static inline __attribute__((unused)) void rwa_cso_create_unlock_scope(int *u) { (void)u; pthread_mutex_unlock(&rwa_cso_create_lock); }
static inline __attribute__((unused)) void rwa_vcso_unlock_scope(int *u) { (void)u; pthread_mutex_unlock(&rwa_vcso_lock); }
static inline __attribute__((unused)) void rwa_btex_unlock_scope(int *u) { (void)u; pthread_mutex_unlock(&rwa_buffer_textures_lock); }
static inline __attribute__((unused)) void rwa_argbuf_unlock_scope(int *u) { (void)u; pthread_mutex_unlock(&rwa_argbuf_lock); }
static inline __attribute__((unused)) void rwa_deferred_unlock_scope(int *u) { (void)u; pthread_mutex_unlock(&rwa_deferred_lock); }

#define RWA_CSO_CREATE_LOCKED \
   int rwa_cso_l_ __attribute__((cleanup(rwa_cso_create_unlock_scope))) = (pthread_mutex_lock(&rwa_cso_create_lock), 0); \
   (void)rwa_cso_l_
#define RWA_VCSO_LOCKED \
   int rwa_vcso_l_ __attribute__((cleanup(rwa_vcso_unlock_scope))) = (pthread_mutex_lock(&rwa_vcso_lock), 0); \
   (void)rwa_vcso_l_
#define RWA_BTEX_LOCKED \
   int rwa_btex_l_ __attribute__((cleanup(rwa_btex_unlock_scope))) = (pthread_mutex_lock(&rwa_buffer_textures_lock), 0); \
   (void)rwa_btex_l_
#define RWA_ARGBUF_LOCKED \
   int rwa_argbuf_l_ __attribute__((cleanup(rwa_argbuf_unlock_scope))) = (pthread_mutex_lock(&rwa_argbuf_lock), 0); \
   (void)rwa_argbuf_l_
#define RWA_DEFERRED_LOCKED \
   int rwa_def_l_ __attribute__((cleanup(rwa_deferred_unlock_scope))) = (pthread_mutex_lock(&rwa_deferred_lock), 0); \
   (void)rwa_def_l_

#include "nir.h"
#include "nir_builder.h"
#include "nir_air.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/blend.h"
#include "target-helpers/inline_sw_helper.h"
#include "null/null_sw_winsys.h"
#ifdef GALLIUM_FREEDRENO
#include "freedreno/drm/freedreno_drm_public.h"
/* for rwa_texture_from_surface: a resource over memory somebody else allocated is a
 * winsys_handle, and the one modifier an IOSurface of this port's kernel can carry is
 * linear (MINISURF has no tiling and no compression at all). */
#include "frontend/winsys_handle.h"
#include "drm-uapi/drm_fourcc.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include <xf86drm.h>
#endif
#include "util/format/u_format.h"
#include "util/half_float.h"
#include "util/u_inlines.h"
#include "util/u_sampler.h"
#include "util/u_debug.h"
#include "util/macros.h" /* UNREACHABLE(), renamed from the unreachable() that used to arrive with the other headers */
#include "util/u_math.h" /* CLAMP(), for the scissor rectangle */
#include "util/u_gen_mipmap.h" /* util_gen_mipmap(), for -generateMipmapsForTexture: */

/* Whether the GPU is hung is the kernel's to say, as Metal leaves it to the GPU driver: a hang
 * the GMU reports ends the wait at once, and a completed timestamp that has not moved for 30 s
 * is reported as no answer (AnnibaleGPU gpuWait, kernel db010097). Work that runs long is not
 * hung: the camera's noise reduction and fusion is one submission of 0.4 to 5.8 s at the GPU's
 * low clocks, and a two-second wait here failed its command buffer and left the photograph
 * black. This only bounds a wait the kernel never answers.
 */
#define RWA_FENCE_TIMEOUT_NS 60000000000ull

/* How many texture slots a dispatch fills, and the bound is not Gallium's 32: sixteen
 * is where air_run stops, by name, and that refusal is what keeps freedreno's own
 * view_seqno[16] out of reach of its textures[32] -- a fragility a peer session found
 * in fd6_texture_state and which this path must not become the one to reach
 * (docs/air.md section 5). No kernel of the corpus binds a tenth texture, let alone a
 * seventeenth.
 */
#define RWA_MAX_TEXTURES 16
/* Image slots, which do not share the textures' bound: an image a shader writes at a
 * computed mip level takes one slot per level above the declared ones
 * (image_level_slot in air_texture.inc), up to ir3's 32 bindless images. */
#define RWA_MAX_IMAGES 32

#include "air_metal_rgb2p_private.h"
#include "air_metal_resource_private.h"
#include "air_metal_state.inc"

/* freedreno_resource.c (tools/mesa/rebase-26/0009): the resource's contents stop mattering to the
 * next batch that draws into it, which then does not read it back into GMEM. */
void fd_resource_contents_undefined(struct pipe_context *pctx, struct pipe_resource *prsc);

/* A sampler is a Gallium state object and nothing more. The state it was built from is
 * not kept: a refusal about a sampler happens while it is being created, where the
 * caller's struct is still in hand.
 *
 * TWO state objects, because Metal and Gallium disagree about who decides that a
 * comparison happens. In Metal the INSTRUCTION decides -- sample() and sample_compare()
 * read the same MTLSamplerState, and the sampler only says WHICH comparison -- while in
 * Gallium the sampler decides, through pipe_sampler_state.compare_mode, and softpipe
 * compares on every sample of a sampler that has it set. One state object cannot serve
 * both, so each sampler carries the pair and a stage binds `cso_compare` into exactly
 * the slots a shadow texture instruction reads (compare_sampled_slots below). Both are
 * built at create time: a sampler state is cheap, and building the second one lazily
 * would mean keeping the caller's struct alive for it, which is the thing the comment
 * above says this does not do.
 *
 * This struct is private to this file -- air_metal.h declares `struct rwa_sampler`
 * without a body -- so growing it is not an ABI change and no driver reads it.
 */
struct rwa_sampler {
   struct rwa_device *device;
   void *cso;           /* no comparison: what a plain sample() wants */
   void *cso_compare;   /* the same state with the comparison switched on */
   bool cubic;
};

/* What a kernel does with texture slot n, from both of the frontend's answers.
 *
 * air_shader_info says whether the slot is sampled or stored and whether the kernel
 * reads or writes it; the instructions themselves say what shape and what element type
 * it expects there, and that is the half air_run reads too (image_shapes(),
 * texture_shapes()). The Metal type name in the metadata is a string, and the
 * instruction is what the GPU will execute.
 */
struct rwa_slot {
   bool sampled;                 /* AIR_RESOURCE_TEXTURE: wants a sampler view */
   bool stored;                  /* AIR_RESOURCE_IMAGE: wants an image view */
   bool read, write;
   bool shape_known;             /* an instruction with a constant binding was found */
   bool shadow;                  /* a shadow instruction reads it, so Metal's depth2d */
   nir_alu_type type;            /* its base type: float, int or uint */
   bool type_known;              /* ... and whether that type means anything: see below */
   enum glsl_sampler_dim dim;
   bool is_array;
   unsigned sampler_mask;

   /* The element type Apple's own metadata declares, out of air.arg_type_name, and the
    * word it wrote it in. This is what a binding is checked against where it exists,
    * because it is the only statement of the element type that did not come through our
    * translation -- and the translation does not always carry one (declared_element).
    */
   nir_alu_type declared_type;
   const char *declared_element;   /* "half", "uint", ...; NULL when none was read */
   unsigned declared_bits;         /* 16 for half/short/ushort, 32 for the rest */
};

struct rwa_compute {
   struct rwa_device *device;
   void *mem_ctx;                 /* the translation: NIR, info, and the strings in it */
   nir_shader *nir;               /* untouched; a dispatch compiles a clone */
   struct air_shader_info info;
   struct rwa_compute_info caps;

   bool dynamic_threadgroup;      /* a threadgroup buffer sized by the dispatch */

   /* the texture slots, as above, and the states built for the samplers the shader
    * declares for itself: those are nothing for a driver to create (rwa_compute_info
    * does not count them), so this library creates them, once, and keeps them with the
    * pipeline whose shader asked for them.
    */
   struct rwa_slot slots[RWA_MAX_TEXTURES];
   void *own_sampler[RWA_MAX_TEXTURES];

   /* and which sampler slots a shadow instruction reads, so a stage binds the
    * comparing state object into those and the plain one everywhere else: struct
    * rwa_sampler says why there are two of each. Read off the NIR by slot_shapes,
    * once, with everything else that comes from the instructions.
    */
   bool compare_sampler[RWA_MAX_TEXTURES];

   /* Hardware reuses one variable-workgroup state, unless a dispatch's threadgroup is
    * larger than that state's registers let in (variable_max): then one compiled for that
    * threadgroup, which ir3 keeps to the registers that let it in (rebase-26 0022).
    * Software recompiles its one fixed-block state when cso_block changes. */
   void *cso;
   bool cso_fixed;
   unsigned variable_max;
   unsigned cso_block[3];
   unsigned cso_ib_dim[2];
   struct rwa_yuv_key yuv;
   /* the states of the parts of split grids (rwa_dispatch_split), the last few kept */
   struct {
      void *cso;
      struct rwa_grid_split split;
      unsigned block[3];
      unsigned ib_dim[2];
      struct rwa_yuv_key yuv;
   } split_states[8];
   unsigned split_next;
   void *bitcode_copy;
   size_t bitcode_size;
   char *entry_copy;
};

/* The argument-buffer recorder, whose own block is further down beside the encode calls
 * but which the three destroy functions above it have to reach: a resource destroyed
 * after it was encoded has to be scrubbed out of the records that name it.
 */
static void argbuf_scrub(struct rwa_device *device, const struct rwa_buffer *buffer,
                         const struct rwa_texture *texture,
                         const struct rwa_sampler *sampler);
static void argbuf_forget_all(struct rwa_buffer *dest);
static bool argbuf_has_encodings(struct rwa_buffer *buffer);
/* the buffer textures over a buffer that is going away (air_metal_texture.inc) */
static void buffer_textures_forget(struct rwa_buffer *buffer);
static void rwa_deferred_drain_locked(struct rwa_device *device);
static void rwa_deferred_defer_texture(struct rwa_device *device, struct pipe_resource *res,
                                       struct pipe_sampler_view *view);
static void rwa_deferred_defer_buffer(struct rwa_device *device, struct pipe_transfer *xfer,
                                      struct pipe_resource *res);
/* and the mask of Metal buffer indices a module's sets are bound at, which both
 * pipeline-creating calls report and whose definition sits beside check_bindings */
static int argbuf_mask_of(const struct air_shader_info *info, const char *name,
                          unsigned *mask, char *why, size_t why_size);

/* PRINTFLIKE, so that a sentence whose arguments do not match it is a build error.
 * These strings are edited often -- they are the product -- and nothing else checks them.
 */
static void say(char *why, size_t why_size, const char *fmt, ...) PRINTFLIKE(3, 4);

static void
say(char *why, size_t why_size, const char *fmt, ...)
{
   if (!why || !why_size)
      return;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(why, why_size, fmt, ap);
   va_end(ap);
}

/* What the screen answers about compute. 26.x filled a struct once at screen creation
 * rather than answering a query per call, so there is nothing left to call and a screen
 * that declines a cap simply leaves its field zero -- which softpipe, the host build's
 * screen, does for the whole of it. What used to be the "returns false" branch is the
 * default written beside each read below.
 */

#include "air_metal_lowering.inc"

#include "air_metal_diag.inc"

/* Defined below; rwa_device_open's calibration times 1024 of them. */
static uint64_t draw_mark(void);
static void rwa_vertex_cso_cache_destroy(struct rwa_device *device);

static const float rwa_quad_verts[] = {
   -1, -1, 0, 1,   1, -1, 0, 1,   -1, 1, 0, 1,   1, 1, 0, 1,
};

struct rwa_device *
rwa_device_open(char *why, size_t why_size)
{
   /* Before the device, so a fault inside device creation is recorded too. */
   rwa_rings_install();

   struct rwa_device *device = calloc(1, sizeof(*device));
   if (!device) {
      say(why, why_size, "out of memory making a device");
      return NULL;
   }

   /* NIR's type system is a process-wide singleton with a reference count; the
    * translator builds types, so it has to be up before air_to_nir and may go down
    * again when the last device closes.
    */
   glsl_type_singleton_init_or_ref();

   /* RWA_SOFTWARE=1 asks for softpipe -- a rasterizer in plain C that needs nothing of
    * the machine but its processor. It is how this library is tested without the phone
    * (tools/mesa/air/air_metal_test.c) and it is air_run's AIR_RUN_SOFTWARE under
    * another name. A driver never sets it: on the device the only screen worth opening
    * is the Adreno's, and if there is none, saying so is more use than quietly
    * computing the answer on the processor at a thousandth of the speed.
    */
   bool software = debug_get_bool_option("RWA_SOFTWARE", false);
   device->software = software;

#ifdef GALLIUM_FREEDRENO
   if (!software) {
      int fd = drmOpenWithType("msm", NULL, DRM_NODE_RENDER);
      if (fd < 0) {
         /* The ordinary case on this port, and not an error anybody should be
          * surprised by: without gpu=1 the kernel starts no AnnibaleGPU service, so
          * there is no msm device for the shim to open (tools/mesa/annibale-drm).
          */
         say(why, why_size, "there is no GPU here: no msm device to open, which is what "
                            "a kernel booted without gpu=1 looks like");
         goto fail;
      }

      /* The driver option cache has to be real, not a zeroed struct: fd_screen_create
       * passes it straight to driParseConfigFiles, which dereferences it. A zeroed one
       * died at address 0x10 on the phone -- the first thing air_run ever did on a
       * machine that has a GPU, and invisible on any machine that has not. This is the
       * sequence osmesa_create_gpu_screen() uses in tools/mesa/freedreno-darwin.patch,
       * which is the one CoreAnimation goes through on this device daily.
       *
       * static, because fd_screen keeps the cache it was given rather than copying it.
       * One screen per process is what a Metal device amounts to; a second device would
       * share these, which is why the parse below is done once.
       */
      static const driOptionDescription freedreno_options[] = {
#include "freedreno/driinfo_freedreno.h"
      };
      static driOptionCache info, cache;
      static struct pipe_screen_config config;
      static bool options_parsed = false;

      if (!options_parsed) {
         /* 26.x gathered the seven trailing arguments into a struct of their own.
          * The four names this used to pass as NULL stay unset for the same reason
          * they were NULL: there is no application or engine here to match a
          * section of mesa.conf against, only the driver.
          */
         const driConfigFileParseParams params = {
            .screenNum = 0,
            .driverName = "msm",
         };
         driParseOptionInfo(&info, freedreno_options, ARRAY_SIZE(freedreno_options));
         driParseConfigFiles(&cache, &info, &params);
         config.options = &cache;
         config.options_info = &info;
         options_parsed = true;
      }

      device->screen = fd_drm_screen_create_renderonly(fd, NULL, &config);
      if (!device->screen) {
         say(why, why_size, "the kernel's msm device opened but freedreno made no screen "
                            "for it, so this GPU is present and unusable");
         goto fail;
      }
   }
#else
   if (!software) {
      say(why, why_size, "this build of the library has no GPU driver in it, only "
                         "software rasterisation");
      goto fail;
   }
#endif

   if (!device->screen)
      device->screen = sw_screen_create(null_sw_create());
   if (!device->screen) {
      say(why, why_size, "no Gallium screen could be created at all");
      goto fail;
   }

   /* The screen no longer answers a call for these: it fills one table per stage at
    * creation, and a screen with no NIR leaves the entry null, which is the same
    * "no NIR here" the null check below has always meant.
    */
   device->nir_options = device->screen->nir_options[MESA_SHADER_COMPUTE];
   if (!device->nir_options) {
      say(why, why_size, "%s takes no NIR, so no Metal shader can be given to it",
          device->screen->get_name(device->screen));
      goto fail;
   }

   /* The context, one per device: it is what compiles a shader, binds buffers, launches
    * a grid and hands out fences. Flags zero on purpose -- not PIPE_CONTEXT_PREFER_THREADED,
    * so there is no Gallium worker thread between this library and the driver. A map held
    * across a dispatch (rwa_buffer_map) is a direct mapping of the kernel's buffer only
    * while that is true.
    */
   device->pipe = device->screen->context_create(device->screen, NULL, 0);
   if (!device->pipe) {
      say(why, why_size, "%s gave no context, so nothing can be dispatched on it",
          device->screen->get_name(device->screen));
      goto fail;
   }

   /* The two numbers Metal requires of every pipeline and its own base class answers
    * with nothing (docs/metal.md 10.8). They are properties of the screen, asked once.
    *
    * max_threads_per_group: freedreno answers 1024, which is threadsize_base (64) times
    * max_waves (16) for the a635, and softpipe answers 1024 as well.
    *
    * thread_execution_width: freedreno answers 32 and Mesa's own comment beside that
    * number is a TODO. The a635's wave is 64 threads, doubled to 128 for a variant ir3
    * compiles double-wide -- fd6_get_compute_state_info reads exactly that, but only
    * off a compiled state object, which needs the threadgroup size a pipeline has not
    * been given yet. So what a driver reports as -threadExecutionWidth here is Mesa's
    * belief about the chip rather than the chip; it is a hint for choosing a
    * threadgroup size, and the dispatch below checks the size it is actually given
    * against the compiled variant, where the number is the hardware's.
    */
   device->max_threads_per_group = device->screen->compute_caps.max_threads_per_block;

   /* compute_caps.subgroup_sizes is a bitmask of the supported widths (rusticl
    * reads the set bit indices and shifts them back), where the query this replaces
    * answered the one width itself. freedreno sets base and, when the chip can do
    * double-wide, base | 2 * base; the width a threadgroup is measured in is the
    * narrowest, hence the lowest bit. Zero means unreported, and one lane is what
    * the old decline fell back to.
    */
   uint32_t widths = device->screen->compute_caps.subgroup_sizes;
   device->thread_execution_width = widths ? (widths & (~(widths - 1))) : 1;

   /* And what a bound buffer's offset has to be a multiple of, which an argument
    * buffer's member is the only thing here to have one. Taken from the screen and not
    * from a table of this library's: it is the driver underneath that programs the
    * register, and freedreno answers 64 for both on a6xx where softpipe answers 16 and 4.
    * A zero would mean "no constraint", so it is read as one byte rather than used as a
    * divisor.
    */
   device->ubo_offset_align = MAX2(device->screen->caps.constant_buffer_offset_alignment, 1);
   device->ssbo_offset_align = MAX2(device->screen->caps.shader_buffer_offset_alignment, 1);

   /* Pre-create the immutable full-screen quad vertex buffer so concurrent render pipeline
    * builds reference it without acquiring rwa_lock. */
   device->quad_vbuf = pipe_buffer_create(device->screen, PIPE_BIND_VERTEX_BUFFER,
                                          PIPE_USAGE_DEFAULT, sizeof(rwa_quad_verts));
   if (device->quad_vbuf)
      pipe_buffer_write(device->pipe, device->quad_vbuf, 0, sizeof(rwa_quad_verts), rwa_quad_verts);

   /* What one timing mark costs. Every number printed about this library's own work is the
    * difference of two of them, and a slow clock here reads exactly like slow work.
    */
   {
      uint64_t began = draw_mark();

      for (unsigned i = 0; i < 1024; i++)
         draw_mark();
      unsigned long long us = draw_mark() - began;

      printf("rwa clock: 1024 timing marks took %llu us, %.0f ns each\n", us,
             (double)us * 1000.0 / 1024.0);
      fflush(stdout);
   }

   return device;

fail:
   if (device->pipe)
      device->pipe->destroy(device->pipe);
   if (device->screen)
      device->screen->destroy(device->screen);
   free(device);
   glsl_type_singleton_decref();
   return NULL;
}

void
rwa_device_close(struct rwa_device *device)
{
   RWA_LOCKED;
   if (!device)
      return;
   rwa_deferred_drain_locked(device);
   state_release(device);
   rgb2p_device_close(device);
   rwa_vertex_cso_cache_destroy(device);
   if (device->default_sampler)
      device->pipe->delete_sampler_state(device->pipe, device->default_sampler);
   pipe_resource_reference(&device->null_ssbo, NULL);
   pipe_resource_reference(&device->null_ubo, NULL);
   pipe_resource_reference(&device->quad_vbuf, NULL);
   /* Before the pipe goes: a fence outlives the context that made it, and one nobody
    * waited for is still the screen's to release. */
   if (device->pending)
      device->screen->fence_reference(device->screen, &device->pending, NULL);
   device->pipe->destroy(device->pipe);
   device->screen->destroy(device->screen);
   free(device);
   glsl_type_singleton_decref();
}

const char *
rwa_device_name(struct rwa_device *device)
{
   /* Mesa's own name for the chip it opened ("Adreno 7c+ Gen 3"), which is where
    * -[AnnibaleMetalDevice name] gets the half it does not read out of the kernel.
    */
   const char *name = device->screen->get_name(device->screen);
   return name ? name : "unknown GPU";
}

struct rwa_buffer *
rwa_buffer_create(struct rwa_device *device, size_t size, char *why, size_t why_size)
{
   RWA_LOCKED;
   if (size == 0 || size > UINT32_MAX) {
      say(why, why_size, "a buffer of %zu bytes: Gallium sizes a buffer in 32 bits and "
                         "zero is not a buffer", size);
      return NULL;
   }

   struct rwa_buffer *buffer = calloc(1, sizeof(*buffer));
   if (!buffer) {
      say(why, why_size, "out of memory making a buffer");
      return NULL;
   }
   buffer->references = 1;
   buffer->device = device;
   buffer->size = size;

   /* All three bindings, because which one this buffer is cannot be known yet: Metal's
    * [[buffer(n)]] is one index space and the shader decides whether the parameter at n
    * is `device` (an SSBO) or `constant` (a UBO), and rwa_dispatch binds whichever the
    * module declared. A driver allocating a buffer has no shader in hand at all.
    *
    * PIPE_BIND_INDEX_BUFFER for the same reason one step further out: an application's
    * MTLBuffer becomes an index buffer at the draw that names it one
    * (-drawIndexedPrimitives:...indexBuffer:), not at the allocation, so the bind this
    * resource was created with has to allow it. Neither back end here places a buffer
    * differently for it -- freedreno's buffers are linear whatever the bind is -- but a
    * resource that does not declare what it is used for is a resource the next driver
    * is entitled to put somewhere a draw cannot read.
    *
    * PIPE_USAGE_STAGING: memory the processor uses as much as the GPU, as a Metal buffer is.
    * PIPE_BIND_SHARED from a page up: a buffer object of its own, not a slice of freedreno's
    * heap, so a texture can be laid over it by its handle (buffer_texture_alias). In this
    * port the bit is only the promise that such an export may happen; the bo becomes
    * uncacheable when a handle is actually exported, not here.
    */
   buffer->res = pipe_buffer_create(device->screen,
                                    PIPE_BIND_SHADER_BUFFER | PIPE_BIND_CONSTANT_BUFFER |
                                    PIPE_BIND_INDEX_BUFFER | (size >= 4096 ? PIPE_BIND_SHARED : 0),
                                    PIPE_USAGE_STAGING, size);
   if (!buffer->res) {
      say(why, why_size, "the GPU driver would not allocate %zu bytes. After a few dozen "
                         "allocations in one boot this driver starts refusing outright "
                         "(docs/air.md section 4)", size);
      free(buffer);
      return NULL;
   }

   /* Mapped once, for the buffer's whole life, because the contract has no unmap: the
    * driver holds one pointer and writes through it whenever it likes. That is cheap
    * rather than clever here -- for a linear buffer freedreno's map is the kernel's own
    * mapping of the GEM object, write combined, with no cache of this processor between
    * the program and what the GPU reads (docs/metal.md 10.11), so there is nothing to
    * flush and nothing to copy, and -[MTLBuffer didModifyRange:] has nothing to do.
    *
    * READ as well as WRITE: it is what tells the driver this mapping must be the real
    * buffer and not a write-only staging copy it would upload later.
    */
   buffer->map = pipe_buffer_map(device->pipe, buffer->res,
                                 PIPE_MAP_READ | PIPE_MAP_WRITE, &buffer->xfer);
   if (!buffer->map) {
      say(why, why_size, "%zu bytes were allocated but could not be mapped for the "
                         "processor", size);
      pipe_resource_reference(&buffer->res, NULL);
      free(buffer);
      return NULL;
   }
   return buffer;
}

void *
rwa_buffer_map(struct rwa_buffer *buffer)
{
   return buffer->map;
}

size_t
rwa_buffer_size(struct rwa_buffer *buffer)
{
   return buffer->size;
}

void
rwa_buffer_destroy(struct rwa_buffer *buffer)
{
   if (!buffer || __atomic_sub_fetch(&buffer->references, 1u, __ATOMIC_ACQ_REL) != 0)
      return;
   struct rwa_device *device = buffer->device;
   /* Out of every argument-buffer record that named it, and then its own records go:
    * see argbuf_scrub -- a pointer left in a record is memory this library would
    * dereference at the next draw, where Metal's own stale handle would at worst be
    * read by the GPU.
    */
   argbuf_scrub(device, buffer, NULL, NULL);
   argbuf_forget_all(buffer);

   buffer_textures_forget(buffer);

   if (buffer->xfer) {
      if (device && pthread_mutex_trylock(&rwa_lock) == 0) {
         rwa_deferred_drain_locked(device);
         pipe_buffer_unmap(device->pipe, buffer->xfer);
         pipe_resource_reference(&buffer->res, NULL);
         pthread_mutex_unlock(&rwa_lock);
      } else {
         rwa_deferred_defer_buffer(device, buffer->xfer, buffer->res);
         buffer->res = NULL;
      }
      buffer->xfer = NULL;
   } else {
      pipe_resource_reference(&buffer->res, NULL);
   }
   free(buffer);
}

/* Metal's own numbers, which the contract hands over raw (air_metal.h) and which this
 * file is the only one to have to know. Spelled out rather than compared as literals,
 * because a refusal that says "type 9" helps nobody and the names are the vocabulary
 * the caller used.
 */
enum {
   RWA_TYPE_1D = 0, RWA_TYPE_1D_ARRAY = 1,
   RWA_TYPE_2D = 2, RWA_TYPE_2D_ARRAY = 3, RWA_TYPE_2D_MULTISAMPLE = 4,
   RWA_TYPE_CUBE = 5, RWA_TYPE_CUBE_ARRAY = 6,
   RWA_TYPE_3D = 7, RWA_TYPE_2D_MULTISAMPLE_ARRAY = 8, RWA_TYPE_TEXTURE_BUFFER = 9,
};

enum {   /* MTLTextureUsage, a mask */
   RWA_USAGE_SHADER_READ = 0x1, RWA_USAGE_SHADER_WRITE = 0x2,
   RWA_USAGE_RENDER_TARGET = 0x4, RWA_USAGE_PIXEL_FORMAT_VIEW = 0x10,
};

enum {   /* MTLSamplerAddressMode -- and see sampler_state_from_air about its numbering */
   RWA_ADDRESS_CLAMP_TO_EDGE = 0, RWA_ADDRESS_MIRROR_CLAMP_TO_EDGE = 1,
   RWA_ADDRESS_REPEAT = 2, RWA_ADDRESS_MIRROR_REPEAT = 3,
   RWA_ADDRESS_CLAMP_TO_ZERO = 4, RWA_ADDRESS_CLAMP_TO_BORDER = 5,
};

enum {   /* MTLSamplerMinMagFilter, MTLSamplerMipFilter */
   RWA_FILTER_NEAREST = 0, RWA_FILTER_LINEAR = 1,
   /* MTLSamplerMinMagFilterCubic: not in the SDK header, named in Metal.framework's
    * strings. RenderBox asks for it in iOS 26 and aborts when the sampler is nil. */
   RWA_FILTER_CUBIC = 2,
   RWA_MIP_NOT_MIPMAPPED = 0, RWA_MIP_NEAREST = 1, RWA_MIP_LINEAR = 2,
};

static const char *
texture_type_name(unsigned type)
{
   static const char *const names[] = {
      "MTLTextureType1D", "MTLTextureType1DArray", "MTLTextureType2D",
      "MTLTextureType2DArray", "MTLTextureType2DMultisample", "MTLTextureTypeCube",
      "MTLTextureTypeCubeArray", "MTLTextureType3D", "MTLTextureType2DMultisampleArray",
      "MTLTextureTypeTextureBuffer",
   };
   return type < ARRAY_SIZE(names) ? names[type] : NULL;
}

/*
 * MTLPixelFormat in Gallium's terms, and only where the two name the same bytes.
 *
 * Every uncompressed entry is a format whose channels are whole bytes or whole 16- or
 * 32-bit words, so the translation is the identity on memory and there is nothing in it to
 * get subtly wrong. The compressed entries are the identity on memory by their block
 * specifications (the comment above them).
 *
 * The packed 32-bit formats RGB10A2, RG11B10, RGB9E5 and BGR10A2 are in the table by the
 * layouts other standards fix for them (the first-named channel in the low bits); the XR
 * formats are absent, because Gallium has no format with their extended-range bias. A
 * mistake in a packed layout does not fail: it shifts a channel by two bits in a texture
 * that still looks like a texture. docs/air.md has
 * both shapes of that already -- zero printed as clamp_to_border by one of Apple's own
 * binaries while it means clamp_to_zero (3.3), and three shaders that "passed" against
 * a 2D texture bound where they sample a 3D one (5). A declined format is one sentence;
 * an almost-right one is a wrong picture nobody reports.
 *
 * THE FOUR PACKED 16-BIT FORMATS ARE HERE, and they were the same refusal until the
 * convention on each side was read rather than assumed. The window server asks for one
 * of them -- MTLPixelFormatBGR5A1Unorm, 43 -- twice a boot, for the home screen.
 *
 * Gallium's side is mechanical: the generated unpackers in u_format_table.c say which
 * bits each name means, and on a little-endian host the FIRST channel of a packed name
 * is at the LEAST significant bits. PIPE_FORMAT_B5G6R5_UNORM reads blue out of bits
 * 0-4, green out of 5-10 and red out of 11-15; PIPE_FORMAT_B5G5R5A1_UNORM reads blue
 * 0-4, green 5-9, red 10-14 and alpha at bit 15.
 *
 * Metal's side is one bit of information: does MTLPixelFormat list a packed format's
 * COLOUR channels from the least significant end or the most? Gallium happens to carry
 * both spellings of every one of these, so the two readings are both expressible and
 * they EXCHANGE RED AND BLUE, leaving the alpha where the name puts it:
 *
 *    Metal                 colours from the LOW end   colours from the HIGH end
 *    40 B5G6R5Unorm        B5G6R5_UNORM               R5G6B5_UNORM
 *    41 A1BGR5Unorm        A1B5G5R5_UNORM             A1R5G5B5_UNORM
 *    42 ABGR4Unorm         A4B4G4R4_UNORM             A4R4G4B4_UNORM
 *    43 BGR5A1Unorm        B5G5R5A1_UNORM             R5G5B5A1_UNORM
 *
 * A THIRD reading -- the whole name from the high end, alpha included, so that 43 would
 * be A1R5G5B5_UNORM -- is the one a reader reaches for next and it is REFUTED, not
 * merely unchosen: moving the one-bit alpha from one end to the other shifts the three
 * five-bit fields by a bit, and re-rendering the measured screen through it puts
 * horizontal striping across every icon. The real screen has none.
 *
 * THE RIGHT COLUMN IS THE ONE TAKEN, and it was a boot that chose it. The left column
 * was an inference -- from Metal's packed 32-bit names, whose layouts other standards
 * fix, and from its array names -- and the inference was wrong. Apple's own compositor,
 * drawing its own home screen through these formats, is what said so: see
 * packed16_served below for the pixel and the arithmetic.
 *
 * The half that is decidable HERE is still measured, and still worth measuring, because
 * it is the half that catches a typo rather than a convention: `air_metal_test
 * --pixel-formats` writes a word with exactly one field set, draws a shader that samples
 * it, and reads which channel came back 1. 15 probes, 15 in the channel the table
 * claims. What that cannot see is which of the two columns Apple meant, because both
 * ends of it are ours -- and that is exactly the part the screen settled.
 */
#include "air_metal_formats.inc"

#include "air_metal_allocation.inc"

#include "air_metal_purgeable.inc"
#include "air_metal_texture.inc"
#include "air_metal_fence.inc"

int
rwa_texture_generate_mipmaps(struct rwa_texture *texture, char *why, size_t why_size)
{
   RWA_LOCKED;
   if (!texture_require_backing(texture, "generate mipmaps", why, why_size)) return -1;
   struct rwa_device *device = texture->device;
   struct pipe_resource *res = texture->res;
   /* util_gen_mipmap's base_level is the first level; each subsequent level is downsampled from it */
   unsigned base = texture->first_level, last = base + texture_levels(texture) - 1;

   if (last == base)
      return 0;
   /* The layers of an array are its array_size, and of a 3D texture its depth: what a blit
    * addresses as a layer is one per depth slice there.
    */
   unsigned layers = res->target == PIPE_TEXTURE_3D ? res->depth0 : res->array_size;

   if (layers == 0)
      layers = 1;
   if (!util_gen_mipmap(device->pipe, res, texture_format(texture), base, last, 0, layers - 1,
                        PIPE_TEX_FILTER_LINEAR)) {
      say(why, why_size, "%s would not downsample the %s texture of %u levels at %ux%u",
          rwa_device_name(device), texture->format_name, texture_levels(texture),
          texture->shape.width, texture->shape.height);
      return -1;
   }
   return 0;
}

#include "air_metal_texels.inc"

#include "air_metal_sampler.inc"
#include "air_metal_argbuf.inc"

#include "air_metal_rgb2p.inc"
#include "air_metal_yuv.inc"
#include "air_metal_bicubic.inc"
#include "air_metal_nircache.inc"
#include "air_metal_compute.inc"

#include "air_metal_bind.inc"
#include "air_metal_dispatch.inc"
#include "air_metal_reflect_vertex.inc"
#include "air_metal_vertex_cache.inc"
#include "air_metal_render_create.inc"
#include "air_metal_render_draw.inc"
#include "air_metal_copy.inc"
