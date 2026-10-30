#ifndef AIR_METAL_RESOURCE_PRIVATE_H
#define AIR_METAL_RESOURCE_PRIVATE_H

/* What the draws left bound in the context (air_metal_state.inc). Index 0 of the per-stage
 * arrays is the fragment stage, 1 the vertex stage. */
struct rwa_bound_state {
   struct pipe_framebuffer_state fb;
   bool fb_set, viewport_set, stencil_set, scissor_set;
   struct pipe_viewport_state viewport;
   struct pipe_stencil_ref stencil;
   struct pipe_scissor_state scissor;
   void *blend, *dsa, *rast, *vs, *fs, *velem;
   struct pipe_sampler_view *views[2][RWA_MAX_TEXTURES];
   void *samplers[2][RWA_MAX_TEXTURES];
   unsigned num_views[2], num_samplers[2];
};

struct rwa_device {
   struct pipe_screen *screen;
   struct pipe_context *pipe;

   /* NIR's options for this screen, asked once: the translator needs them to know what
    * the consumer can be handed (what it may leave for the backend to lower).
    */
   const struct nir_shader_compiler_options *nir_options;

   /* what the screen says about dispatching, read once (see rwa_device_open) */
   unsigned max_threads_per_group;
   unsigned thread_execution_width;

   /* One sampler for the slots nothing was bound to, made when a dispatch first needs
    * it. See rwa_dispatch: a kernel's read() is a texture fetch and ignores the
    * sampler, but softpipe dereferences the state of every unit a shader touches.
    */
   void *default_sampler;

   /* Whether this is the rasterizer in plain C rather than the Adreno. Kept because one
    * refusal has to name it: a software screen HAS a resource_from_handle, and it is the
    * winsys under it that imports nothing, so a failed import there would otherwise come
    * back wearing the GPU's sentence about pitch alignment -- which is not what went
    * wrong (rwa_texture_from_surface).
    */
   bool software;

   /* What this screen requires of a buffer offset, per binding, read once. Only an
    * argument buffer's member has an offset at all here (rwa_argbuf_encode_buffer), and
    * these are what it is checked against: a6xx wants 64 bytes for a UBO and 64 for an
    * SSBO, softpipe 16 and 4. Metal's own rule is looser than either, so an offset it
    * allows has to be refused by name rather than rounded -- Gallium's drivers program
    * the register with what they are given.
    */
   unsigned ubo_offset_align, ssbo_offset_align;

   /* Every buffer that holds an argument-buffer encoding, so that destroying a texture,
    * a sampler or a buffer can scrub it out of the records that name it. Short by
    * construction: a buffer joins only when something is encoded into it.
    */
   struct rwa_buffer *encoded;

   /* The fence of the last flush nobody has waited for yet (rwa_flush, rwa_finish).
    * Held here rather than returned to the caller so that a caller can submit work
    * without waiting for it and still fence on it later: a flush with nothing new to
    * send gives no fence, and a finish that took that for "nothing is outstanding"
    * would report a frame done while the GPU was still drawing it.
    */
   struct pipe_fence_handle *pending;

   /* The textures the caller laid over a buffer (rwa_texture_from_buffer). They are here
    * because their bytes live somewhere else until the processor copies them, and the copy
    * has to happen again before every batch of work that reads them.
    */
   struct rwa_buffer_texture *buffer_textures;

   struct rwa_bound_state bound;
};

/* One texture laid over a buffer, as -newTextureWithDescriptor:offset:bytesPerRow: asks for.
 * The texture keeps memory of its own and the bytes are copied into it; a Gallium resource
 * cannot be made to share another resource's memory.
 */
struct rwa_buffer_texture {
   struct rwa_buffer_texture *next;
   struct rwa_texture *texture;
   struct rwa_buffer *source;
   size_t offset;
   unsigned row_bytes;
   /* The texture holds bytes the buffer does not have yet: the GPU or the processor wrote
    * the texture since the last copy between them (texture_written). */
   bool written;
};

/*
 * WHAT AN MTLArgumentEncoder LEAVES BEHIND, which is a record and not bytes.
 *
 * Metal's encoder writes an 8-byte handle per member into the destination buffer; this
 * library binds by index and the frontend has already resolved every member to one, so
 * there is nothing to write and the handles would say nothing if there were (air_metal.h,
 * docs/air.md 8.2). So a -setTexture:atIndex: lands here: one slot per Metal `[[id(n)]]`,
 * in one encoding, in the DESTINATION BUFFER -- because Metal's object model allows one
 * buffer to hold several encodings at several offsets and to reuse each across many
 * draws, so neither the encoder nor the pipeline is a place a record could live.
 *
 * `destroyed` is the one thing here that Metal does not have. An argument buffer there
 * holds raw handles and keeping the resources alive is the application's business, which
 * is what -useResource: is for; a raw pointer in a record would be a read of freed memory
 * at the next draw instead. So the three destroy calls scrub themselves out of every
 * record that names them and leave this flag, and the draw then refuses BY NAME -- the
 * same answer, one frame earlier, with the resource's own number in it.
 */
struct rwa_argbuf_slot {
   bool set;                      /* this member was encoded at all */
   bool destroyed;                /* ... and the resource it named has been destroyed */
   unsigned kind;                 /* enum rwa_argbuf_kind: which call set it */
   struct rwa_buffer *buffer;     /* RWA_ARGBUF_BUFFER, with Metal's own offset */
   size_t offset;
   struct rwa_texture *texture;   /* RWA_ARGBUF_TEXTURE */
   struct rwa_sampler *sampler;   /* RWA_ARGBUF_SAMPLER */
};

/* One encoding: everything -setArgumentBuffer:offset: named as a destination, until the
 * buffer is destroyed. The slot array grows to the highest id encoded rather than being
 * RWA_MAX_ARGBUF_MEMBERS long, because an application re-encodes a two-member set
 * thousands of times in a frame and sixty-four slots of each would be memory nothing
 * reads. The list is walked linearly: a draw looks an offset up once per set, which is
 * cheap beside the draw itself, and an application with thousands of encodings in one
 * buffer would be the thing to measure before making this a table.
 */
struct rwa_argbuf_encoding {
   struct rwa_argbuf_encoding *next;
   size_t offset;
   unsigned slots;                /* how many the array holds */
   unsigned members;              /* how many of them have been set */
   struct rwa_argbuf_slot *slot;
};

struct rwa_buffer {
   struct rwa_device *device;
   struct pipe_resource *res;
   struct pipe_transfer *xfer;
   void *map;
   size_t size;
   /* The caller's own memory of a no-copy buffer (rwa_buffer_set_mirror), or NULL: what the
    * caller writes there is copied into map before textures over the buffer are refreshed, and
    * their results are written back into it. mirror_hash is the memory's hash when it was last
    * read or written here (mirror_hashed false: never read). */
   void *mirror;
   uint64_t mirror_hash;
   bool mirror_hashed;

   /* The encodings an MTLArgumentEncoder recorded into this buffer, and this buffer's
    * place in the device's list of buffers that hold any -- joined when the first member
    * is encoded, so a buffer that is only ever a buffer costs nothing.
    */
   struct rwa_argbuf_encoding *encodings;
   struct rwa_buffer *next_encoded;
   bool listed;
};

/* A texture, with the shape Metal asked for kept as it was given rather than as it was
 * translated: every sentence this file writes about a texture has to be one a Metal
 * programmer recognises, so a refusal names MTLPixelFormatRGBA32Float and not
 * R32G32B32A32_FLOAT.
 */
struct rwa_texture {
   struct rwa_device *device;
   struct pipe_resource *res;
   struct pipe_sampler_view *view;  /* made when a dispatch first samples it, then kept */
   struct rwa_texture_shape shape;
   const char *format_name;         /* the MTLPixelFormat, spelled out */
   unsigned layers;                 /* array slices, or the six faces of a cube */

   /* what MTLTextureUsage allowed, which is checked at every dispatch: a kernel that
    * writes a texture the application declared read-only is an application's mistake,
    * and Metal itself catches it -- so this library has to as well, or it is the
    * lenient one.
    */
   bool shader_read, shader_write;

   /* Set when the bytes came from a buffer the caller holds (rwa_texture_from_buffer). */
   struct rwa_buffer_texture *backing;

   /* The format a view samples and renders the shared resource as (rwa_texture_view);
    * PIPE_FORMAT_NONE for a texture that made its own resource. Read through
    * texture_format(). */
   enum pipe_format view_format;
   /* The run of the resource's levels a view covers; num_levels is 0 for a texture that
    * made its own resource. Read through texture_levels(). */
   unsigned first_level, num_levels;
};

/*
 * A colour or depth target made here starts at zero. freedreno takes a new resource's memory
 * from its cache of freed buffers and does not clear it, so a target can hold what an earlier
 * owner drew. render92 sampled a 96x128 target of SpringBoard's whose rows below the 96x97
 * scissor held part of an earlier icon's shadow before the commit's first draw, and RenderBox's
 * distance and blur passes read texels outside what they draw. Multisampled targets are left
 * as they are.
 * Called by texture_of_shape on the resource it has just made; returns the texture.
 */
static inline struct rwa_texture *
texture_start_zeroed(struct rwa_device *device, struct rwa_texture *texture)
{
   static const uint64_t zero[2];
   struct pipe_resource *res = texture->res;

   if (!(res->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL)) ||
       res->nr_samples > 1 || !device->pipe->clear_texture)
      return texture;
   for (unsigned level = 0; level <= res->last_level; level++) {
      struct pipe_box box;

      u_box_3d(0, 0, 0, u_minify(res->width0, level), u_minify(res->height0, level),
               util_num_layers(res, level), &box);
      device->pipe->clear_texture(device->pipe, res, level, &box, zero);
   }
   return texture;
}

#endif
