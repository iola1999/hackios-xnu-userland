/*
 * What a Metal driver needs from Mesa, and nothing else.
 *
 * Subsystem (1) made an AIR front end inside Mesa; subsystem (2) is making the Metal object
 * graph in userland/src/annibalemetal.m. They meet here. The measurement that decided the
 * shape of this header is in docs/metal.md 10.12: Metal hands a driver one function's LLVM
 * module through -[_MTLFunction bitcodeData], and those bytes are -- checked over all eleven
 * functions of AV1SW.metallib -- exactly what this front end already reads. So the contract
 * is "AIR in, a pipeline out", and nothing in it mentions NIR, Gallium or ir3: the Metal
 * side must not grow a dependency on Mesa's headers, and this side must stay free to change
 * how it compiles.
 *
 * air_run (tools/mesa/air/air_run.c) does all of this already as a test program -- opens a
 * freedreno screen, translates a module, builds a compute state and launches a grid on the
 * phone's Adreno. This is that, as a library, so a driver can do it in process.
 *
 * Every call that can fail says why in a caller's buffer rather than returning a code to be
 * looked up: a driver has to put something in an NSError, and "the GPU did not finish" and
 * "the shader binds a texture, which is not implemented" are different sentences.
 */

#ifndef AIR_METAL_H
#define AIR_METAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rwa_device;
struct rwa_buffer;
struct rwa_texture;
struct rwa_sampler;
struct rwa_compute;

/* What Metal must be told about a compiled compute pipeline. The first three are protocol
 * requirements of MTLComputePipelineState that Metal's own base class answers with nothing
 * (docs/metal.md 10.8), so they have to come from whoever compiled the shader. */
struct rwa_compute_info {
	unsigned max_threads_per_group;    /* -maxTotalThreadsPerThreadgroup */
	unsigned thread_execution_width;   /* -threadExecutionWidth */
	unsigned static_shared_bytes;      /* -staticThreadgroupMemoryLength */
	unsigned buffers;                  /* how many buffer bindings the shader reads */
	unsigned textures;                 /* and textures, and samplers: a driver that cannot */
	unsigned samplers;                 /* bind them yet needs to know before it dispatches */

	/* Which of those buffer indices are ARGUMENT BUFFERS rather than buffer
	 * arguments, one bit each (`air.indirect_buffer`, see below). A driver needs
	 * this before it dispatches for the reason it needs vertex_stream_mask before
	 * it draws: Metal has ONE buffer table and an index in it is either a set or an
	 * argument, and which it is nobody but the shader knows. Appended, so every
	 * field a driver already fills keeps its offset.
	 */
	unsigned argbuf_mask;

	/* Nonzero for a kernel any threadgroup size that divides the grid serves alike
	 * (air_shader_info.group_free). Appended like argbuf_mask. */
	unsigned group_free;
};

/* The device. One per Metal device object; it owns the screen and one context.
 * Returns NULL and fills `why` when there is no GPU to open -- which on this port is the
 * ordinary case when the kernel was booted without gpu=1. */
struct rwa_device *rwa_device_open(char *why, size_t why_size);
void rwa_device_close(struct rwa_device *device);

/* What the driver should call the GPU, as Mesa knows it ("Adreno 7c+ Gen 3"). Never NULL. */
const char *rwa_device_name(struct rwa_device *device);

#include "air_metal_copy.h"

/*
 * AIR in, a compiled pipeline out.
 *
 * `bitcode`/`size` is one function's module as Metal hands it over: the 20-byte 0x0B17C0DE
 * wrapper may be in front of it and is accepted either way. `entry` names the function
 * inside the module; it may be NULL when the module holds exactly one.
 *
 * On failure returns NULL and writes a sentence into `why` -- and the sentences matter, as
 * docs/air.md says: 135 of the corpus's modules are refused for reasons a driver should
 * repeat to the application rather than swallow.
 */
struct rwa_compute *rwa_compute_create(struct rwa_device *device, const void *bitcode,
				       size_t size, const char *entry,
				       struct rwa_compute_info *info, char *why, size_t why_size);
void rwa_compute_destroy(struct rwa_compute *compute);

/*
 * A texture. The shape comes from Metal's own descriptor, in Metal's own terms, because that
 * is what a driver has in its hand when newTextureWithDescriptor: is called; translating it
 * into Gallium's terms is this library's job, and a format it cannot translate must be
 * refused BY NAME rather than approximated -- docs/air.md is full of reasons to distrust a
 * pixel format that was almost right.
 *
 * `pixel_format` is MTLPixelFormat as Metal defines it. `type` is MTLTextureType. `usage` is
 * MTLTextureUsage, and the only bit that changes what the GPU is told is whether the shader
 * writes the texture.
 *
 * A DEPTH TEXTURE is made here and nowhere else, and it is a texture in exactly these
 * terms: one whose `pixel_format` is one of Metal's depth or stencil formats
 * (Depth16Unorm, Depth32Float, Stencil8, Depth32Float_Stencil8). Three things follow from
 * the format alone, and they are the whole of what makes it different:
 *
 *   - MTLTextureUsageRenderTarget on it means the depth/stencil attachment of a render
 *     pass and not a colour one, because Gallium has two bindings where Metal has one
 *     usage bit, and the two are not interchangeable: freedreno answers for a Z format
 *     only when asked about PIPE_BIND_DEPTH_STENCIL (a6xx/fd6_screen.cc, fd6_pipe2depth).
 *   - MTLTextureUsageShaderWrite on it is refused by name. Metal has no depth format a
 *     shader stores to, and Gallium has no image binding for one either, so a texture
 *     made that way could only be bound to something that would not write it.
 *   - its texels do not come back through rwa_texture_read, which refuses it by name and
 *     says so: a texel of Depth32Float_Stencil8 holds two values of two kinds, and the
 *     one answer rwa_texture_read is able to give would be whichever of them the caller
 *     did not ask for. rwa_depth_read, below, asks for an aspect.
 *
 * Everything else about it is a texture's: the width, the mip chain, the region of a
 * write, MTLTextureUsageShaderRead for a shader that samples it (which is what Metal's
 * depth2d<float> parameter is, and what Apple's own texturedFragment2DDepth declares).
 */
struct rwa_texture_shape {
	unsigned pixel_format;
	unsigned type;
	unsigned width, height, depth;
	unsigned array_length;
	unsigned mip_levels;
	unsigned sample_count;
	unsigned usage;
};

struct rwa_texture *rwa_texture_create(struct rwa_device *device,
				       const struct rwa_texture_shape *shape,
				       char *why, size_t why_size);
void rwa_texture_destroy(struct rwa_texture *texture);

/*
 * The same texture, over memory somebody else allocated: an IOSurface of CoreAnimation's.
 *
 * This is what a drawable is (docs/metal.md 10.46). -[CAMetalLayer nextDrawable] makes an
 * IOSurface, builds an ordinary MTLTextureDescriptor and sends a device exactly one message,
 * -newTextureWithDescriptor:iosurface:plane:, and the texture that comes back has to BE that
 * surface's memory rather than a copy of it -- because the point of a drawable is that the
 * display controller already scans out what the pass wrote. Without this call the driver
 * reads the finished attachment back to the processor and hands it to the kernel, which is
 * measured at 15.46 ms a frame and caps the whole port at 64.7 frames a second with nothing
 * drawn (docs/metal.md 10.43).
 *
 * `surface_id` is the IOSurface's ID, and it goes where Linux puts a dmabuf file descriptor:
 * the DRM shim of this port takes the ID there already (tools/mesa/annibale-drm: the kernel's
 * kAnnibaleSurface answers it), so nothing new is asked of the layers underneath. `offset`
 * and `row_bytes` are the kernel's own answer about that surface and are NOT asked again
 * here, because freedreno takes both from the caller.
 *
 * Narrower than rwa_texture_create on purpose, and every narrowing is a refusal by name: a
 * plain 2D texture, one mip level, one sample, one slice. That is what a drawable is and all
 * that an imported row-major surface can be; a mip chain or an array would need a layout
 * agreement with whoever allocated the memory, and there is none.
 *
 * Ownership, which is the part that is easy to get wrong twice: the buffer the kernel makes
 * for that surface belongs to FREEDRENO's table of buffers from here on, and
 * rwa_texture_destroy gives it back. A caller that also takes the surface itself gets a
 * second buffer over the same memory on its own connection, which is a waste rather than a
 * fault -- but it must not then close freedreno's.
 *
 * NULL and a sentence on anything. A screen that imports nothing says so by name, which is
 * what the software screen on a build machine does.
 */
struct rwa_texture *rwa_texture_from_surface(struct rwa_device *device,
					     const struct rwa_texture_shape *shape,
					     int surface_id, unsigned offset,
					     unsigned row_bytes, char *why, size_t why_size);

/*
 * -generateMipmapsForTexture: downsamples levels 1..last_level from level 0, with the
 * library's own blit shaders. Level 0 has to have been written by the time this runs, which
 * is why the Metal side records it as a work item in the command buffer rather than calling
 * it while the blit encoder is open.
 *
 * 0, or -1 and a sentence. A texture of one level is a no-op, not an error: Metal allows the
 * call and the texture has nothing above level 0 to fill.
 */
int rwa_texture_generate_mipmaps(struct rwa_texture *texture, char *why, size_t why_size);

/*
 * Pixels in and out, which is -replaceRegion:... and -getBytes:... on the Metal side. The
 * region is given in texels and `stride` in bytes per row of the caller's memory; a stride
 * of 0 means tightly packed. 0, or -1 and a sentence.
 */
int rwa_texture_write(struct rwa_texture *texture, unsigned level, unsigned layer,
		      const unsigned origin[3], const unsigned size[3], const void *bytes,
		      size_t stride, char *why, size_t why_size);
int rwa_texture_read(struct rwa_texture *texture, unsigned level, unsigned layer,
		     const unsigned origin[3], const unsigned size[3], void *bytes,
		     size_t stride, char *why, size_t why_size);

/*
 * What a depth/stencil texture holds, by aspect, which is the reading rwa_texture_read
 * cannot give.
 *
 * `depth` takes one float per texel and `stencil` one byte per texel, both tightly
 * packed in the region's own row-major order, and either may be NULL. An aspect the
 * format does not have is refused BY NAME rather than filled with something: asking the
 * stencil of a Depth32Float texture is a caller that believes it allocated a different
 * texture, and a buffer of zeros would confirm that belief.
 *
 * The depth comes back as a float in [0, 1] whatever the format stores -- Depth16Unorm's
 * 65535 is 1.0 here -- because that is the number a depth test was made of and the only
 * one two formats can be compared in. The decoding is Mesa's own for the format
 * (util_format's unpack_z_float / unpack_s_8uint), not arithmetic written here, for the
 * reason the colour formats are a table of identities: a depth format packed by hand is
 * a wrong number that still looks like a depth.
 *
 * 0, or -1 and a sentence.
 */
int rwa_depth_read(struct rwa_texture *texture, unsigned level, unsigned layer,
		   const unsigned origin[3], const unsigned size[3], float *depth,
		   uint8_t *stencil, char *why, size_t why_size);

/*
 * What textures a module declares, slot by slot, in Metal's own terms.
 *
 * Everything above takes a texture's shape FROM the caller, because a driver has the
 * application's MTLTextureDescriptor in hand and never has to ask. Two callers do have
 * to ask, and neither has a descriptor to read it from:
 *
 *   - Metal's own pipeline reflection. -newRenderPipelineStateWithDescriptor:options:
 *     reflection: has to report MTLArgument.textureType and .textureDataType for every
 *     binding, and only whoever read the shader knows them.
 *   - a test standing in for an application. userland/src/metal_drv_test.m --draw-all
 *     sweeps every fragment function of a .metallib, and with one kind and one format
 *     bound for the whole library 13 of Apple's 271 were refused -- by this library, and
 *     rightly: they declare a texture3d, a texture2d<int> or a multisample texture where
 *     a texture2d<float> was bound. Thirteen shaders went unmeasured for a reason that
 *     has nothing to do with the hardware.
 *
 * What comes back is read off the same slots rwa_dispatch and rwa_render_draw judge a
 * binding by, through the same two helpers, so the two cannot drift: a caller that binds
 * what this reports is not refused for the shape, and a caller that binds something else
 * is refused BY NAME -- which is what keeps a deliberate mismatch testable.
 */
enum rwa_element {
	RWA_ELEMENT_UNKNOWN = 0,  /* the module states none: any format is accepted */
	RWA_ELEMENT_FLOAT,        /* float, half: a floating-point or normalised format */
	RWA_ELEMENT_INT,          /* int, short: a signed integer format */
	RWA_ELEMENT_UINT,         /* uint, ushort: an unsigned integer format */
};

struct rwa_texture_decl {
	unsigned index;          /* Metal's [[texture(n)]] */
	unsigned type;           /* MTLTextureType, as rwa_texture_shape.type takes it */
	unsigned type_known;     /* 0 when the shader indexes this slot with a computed
				  * value and no instruction declared a kind: `type` says
				  * nothing then, and any texture is accepted */
	unsigned usage;          /* the MTLTextureUsage bits the shader needs. Honoured and
				  * not advisory: a texture made without ShaderWrite is
				  * refused for a shader that writes it */
	unsigned element;        /* enum rwa_element */
	unsigned element_bits;   /* 32 for float/int/uint, 16 for half/short/ushort; 0 when
				  * Apple's metadata carries no type name for the slot and
				  * the element type came from the instructions, which say
				  * the class and not the width */
	char metal_type[64];     /* air.arg_type_name as Apple wrote it, e.g.
				  * "texture2d<half, write>"; "" when the module has none.
				  * Copied, not pointed at: this call frees everything it
				  * read before it returns */
};

/*
 * Fills `out` with one entry per texture slot the module declares, in slot order, and
 * writes how many into *count. Returns 0, or -1 with a sentence -- including when there
 * are more than `max` or when a slot is past the sixteen this library binds, because a
 * caller that quietly bound the first few would be dispatching a shader with a slot
 * nothing was bound to, which is the one outcome worse than an error.
 *
 * `bitcode`/`size`/`entry` are rwa_compute_create's, wrapper or not, and every stage is
 * accepted: a fragment shader's textures are as much a declaration as a kernel's. The
 * module is translated to answer, so a module this library cannot translate is refused
 * here with the same sentence rwa_compute_create would give.
 */
int rwa_module_textures(struct rwa_device *device, const void *bitcode, size_t size,
			const char *entry, struct rwa_texture_decl *out, unsigned max,
			unsigned *count, char *why, size_t why_size);

/*
 * A sampler, as Metal describes one. These fields are deliberately the same ones the front
 * end already decodes out of a shader's `constexpr sampler` word (docs/air.md section 3.3),
 * whose behaviour was checked against Apple's own rules in 15 cases: a driver that builds a
 * sampler from MTLSamplerDescriptor and a shader that declares its own must end up at the
 * same place, and keeping one vocabulary is what makes that checkable.
 *
 * The address modes are Metal's MTLSamplerAddressMode, the filters MTLSamplerMinMagFilter,
 * the mip filter MTLSamplerMipFilter and the compare function MTLCompareFunction.
 *
 * NOTHING SAYS HERE WHETHER A COMPARISON HAPPENS, and that is Metal's own design, not an
 * omission: sample() and sample_compare() are different instructions over one sampler,
 * so the descriptor only says WHICH comparison and the shader says whether. The library
 * builds both Gallium states a sampler needs for that -- Gallium puts the decision in
 * the sampler -- and binds the comparing one into exactly the slots the translated
 * shader samples with a comparison. A driver has nothing to do for it, and this struct
 * did not have to grow for it either.
 *
 * compare_function is MTLCompareFunction's eight and is not the shader word's nine: that
 * word has a `none` MTLCompareFunction has no name for, with `never` at 8 and in a
 * different order throughout. The translation is a table in air_metal.c's
 * sampler_state_from_air, with the three sources it was read from.
 */
struct rwa_sampler_state {
	unsigned address[3];       /* s, t, r */
	unsigned min_filter, mag_filter, mip_filter;
	unsigned compare_function;         /* MTLCompareFunction: 0 Never .. 7 Always */
	unsigned max_anisotropy;
	unsigned normalized_coordinates;   /* 0: in pixels, as Metal's unnormalized sampler */
	float lod_min, lod_max;
};

struct rwa_sampler *rwa_sampler_create(struct rwa_device *device,
				       const struct rwa_sampler_state *state,
				       char *why, size_t why_size);
void rwa_sampler_destroy(struct rwa_sampler *sampler);

/*
 * ---------------------------------------------------------------------------------------
 * ARGUMENT BUFFERS: MTLArgumentEncoder, and the one thing to read before the rest.
 *
 * THERE IS NO ARGUMENT BUFFER TO WRITE. Metal's encoder writes an 8-byte handle per
 * member into a plain buffer -- 8 bytes per member is what the metadata says, a
 * two-member set declaring `air.arg_type_size 16` and a ten-member set 80 -- and those
 * handles are an Apple GPU's descriptors. They mean nothing to a Gallium driver, which
 * binds by index; and this frontend has already resolved every member of every set to a
 * binding while translating (docs/air.md 8.2), so the handles are not needed either.
 *
 * So the encoder cannot be a writer here. It is a RECORDER: -setBuffer:offset:atIndex:,
 * -setTexture:atIndex: and -setSamplerState:atIndex: record (resource, Metal id) against
 * (destination buffer, destination offset), and at the draw or dispatch this library
 * binds each member of each set the pipeline's shaders declare to the Gallium slot the
 * frontend gave it. THE BUFFER'S CONTENTS ARE NEVER READ.
 *
 * Three consequences, each of which is a decision rather than a side effect:
 *
 *   - The records live WITH THE DESTINATION BUFFER and are keyed by the offset, because
 *     that is what Metal's own object model allows: one buffer holds several encodings at
 *     several offsets, and each is reused across many draws. A record keyed by the
 *     encoder would be wrong twice over -- an encoder is reset onto another buffer and
 *     another offset and keeps encoding, and an application may destroy it and draw with
 *     what it encoded for the rest of the frame.
 *   - An encoding is not cleared by -setArgumentBuffer:offset:, and that is Metal's
 *     behaviour and not a shortcut: the bytes an earlier encoding wrote into that buffer
 *     at that offset are still there, so a second encoding that sets three of five
 *     members leaves the other two as they were. A record that vanished would refuse a
 *     draw Metal would have made.
 *   - An argument buffer the application did NOT fill through an encoder is REFUSED BY
 *     NAME, and this is the most important refusal in this header. A buffer written by
 *     the processor, by a compute kernel or by a blit holds handles this library cannot
 *     interpret, and nothing distinguishes it from a correctly encoded one except the
 *     presence of a record. There is no guess available that is not "draw over whatever
 *     resource index 0 happens to be".
 *
 * `useResource:`/`useHeap:` are therefore no-ops on this driver, and saying so out loud
 * is better than implementing nothing in silence: every member is bound explicitly, so
 * there is no residency for the application to arrange.
 */

/* RE4's largest set has 43 members (docs/air.md 8.2) and Apple's own limit on an
 * argument buffer's entries is higher than this; what this number bounds is the record,
 * and a set with more members than this is refused by name rather than recorded in part.
 */
#define RWA_MAX_ARGBUF_MEMBERS 64

enum rwa_argbuf_kind {
	RWA_ARGBUF_BUFFER = 0,     /* `constant` or `device`: which it is, the shader decides */
	RWA_ARGBUF_TEXTURE = 1,
	RWA_ARGBUF_SAMPLER = 2,
};

/*
 * One member of one set, as the module declares it, in both namings -- and it takes two
 * because neither can be derived from the other:
 *
 *   - `id` is Metal's `[[id(n)]]`, which is what the APPLICATION passes to
 *     -setTexture:atIndex: and what this library's record is keyed by. It is per set, so
 *     set 0's member 0 and set 1's member 0 both say 0.
 *   - `index` is the flat per-kind binding the FRONTEND allocated, which is what the GPU
 *     is programmed with. A driver never has to use it: it is here so that the mapping
 *     can be read back and checked rather than trusted.
 */
struct rwa_argbuf_member {
	unsigned id;
	unsigned kind;             /* enum rwa_argbuf_kind */
	unsigned index;
	unsigned write;            /* 1 when the shader writes this member */
	char name[48];             /* air.arg_name, as Apple's compiler wrote it; "" if none */
};

/*
 * A set, and the two numbers an application allocates its buffer from.
 *
 * `encoded_length` and `alignment` are -[MTLArgumentEncoder encodedLength] and
 * -alignment, and they must be APPLE'S OWN NUMBERS rather than this library's idea of
 * what it needs: the application calls -newBufferWithLength:encodedLength and then
 * encodes at multiples of it, so a number of ours that was too small would have the
 * application overlapping its own encodings, and one too large would waste memory in a
 * way that is invisible until something runs out. They are 8 bytes per member the set
 * DECLARES -- used or not, which is why the member list below is never compacted -- and
 * 8 bytes, which is what `air.arg_type_size` and `air.arg_type_align_size` say in the
 * metadata of every one of Resident Evil 4's 7 857 sets (docs/air.md 8.2.1).
 */
struct rwa_argbuf_layout {
	unsigned metal_index;      /* the [[buffer(n)]] the SET is bound at */
	unsigned encoded_length;
	unsigned alignment;
	unsigned member_count;
	struct rwa_argbuf_member members[RWA_MAX_ARGBUF_MEMBERS];
};

/*
 * What argument buffers a module declares, set by set, in Metal's own terms:
 * rwa_module_textures' shape, for rwa_module_textures' reason. A driver asked for
 * -[MTLFunction newArgumentEncoderWithBufferIndex:] has a function's bitcode and nothing
 * else -- no pipeline yet, and no descriptor to read the layout from -- and the numbers
 * it must answer with are in the module.
 *
 * Returns 0 and fills `out` with the sets in the order the module declares them, or -1
 * and a sentence. A module with no argument buffer is 0 sets and not an error.
 *
 * REFUSED BY NAME here rather than at the draw, because each would otherwise be a
 * length the application allocates against: a set with more members than this library
 * records; two sets at one Metal buffer index; a member whose id is past the number of
 * members the set declares (Apple's encodedLength is over the highest id, and a set with
 * a gap in its ids would be allocated too short); and anything the module cannot be
 * translated at all for, with the sentence rwa_compute_create would give.
 */
int rwa_module_argbufs(struct rwa_device *device, const void *bitcode, size_t size,
		       const char *entry, struct rwa_argbuf_layout *out, unsigned max,
		       unsigned *count, char *why, size_t why_size);

/*
 * THE ENCODER, which is these three calls and no object: an encoder's whole state is
 * (destination buffer, destination offset), which the caller passes to each one, so
 * -setArgumentBuffer:offset: is a driver-side assignment and nothing happens here until
 * a member is set.
 *
 * `id` is Metal's `[[id(n)]]` as the application gave it. The KIND is the call, as it is
 * in Metal, and a member set with the wrong one is refused at the draw by name -- not
 * here, where the set this buffer is being encoded for is not known: one buffer may be
 * encoded for a set declared by a shader this library has never seen.
 *
 * `offset` on a buffer member is Metal's own `offset:` and is honoured, by the same rule
 * a DIRECT buffer argument's offset follows -- struct rwa_buffer_bind has it, and the
 * short of it is that a `constant` binding is served at any offset and a `device` one at
 * an offset this GPU cannot fetch from is refused by name. It is the ordinary case of an
 * application with one allocation and many blocks in it, and it is checked at the draw
 * rather than here, where the set this buffer is being encoded for is not known.
 *
 * 0, or -1 and a sentence: a destination that is not a buffer, an id past
 * RWA_MAX_ARGBUF_MEMBERS, a resource of another device, or an offset past the end of the
 * buffer being encoded.
 *
 * The records hold the resources, and keeping them alive while the encoding is drawn
 * with is the application's business, exactly as it is in Metal -- an argument buffer
 * there holds raw handles, which is what -useResource: exists for. What this library
 * does guarantee is that a resource destroyed after it was encoded is a REFUSAL at the
 * next draw and not a read of freed memory: rwa_buffer_destroy, rwa_texture_destroy and
 * rwa_sampler_destroy scrub the member they were encoded into and the draw then names
 * it.
 */
int rwa_argbuf_encode_buffer(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
			     struct rwa_buffer *buffer, size_t offset, char *why,
			     size_t why_size);
int rwa_argbuf_encode_texture(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
			      struct rwa_texture *texture, char *why, size_t why_size);
int rwa_argbuf_encode_sampler(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
			      struct rwa_sampler *sampler, char *why, size_t why_size);

/*
 * How many members are recorded at (dest, dest_offset), which is what -[MTLArgumentEncoder
 * constantDataAtIndex:] and a test need and what the refusal at the draw turns on: zero
 * is "this buffer was never encoded", and nothing else about a buffer says that. Not part
 * of any Metal protocol.
 */
unsigned rwa_argbuf_encoded_members(const struct rwa_buffer *dest, size_t dest_offset);

/*
 * METAL'S BUFFER TABLE FOR ONE STAGE: what is bound at each index AND AT WHAT OFFSET.
 *
 * `rwa_bindings.buffers` says which buffer is at index n; this says where in it, which
 * rwa_bindings has no room for. One array covers both of the things an offset means,
 * because Metal has one table and one -setFragmentBuffer:offset:atIndex: behind both:
 *
 *   - for an ARGUMENT BUFFER, the offset is WHICH ENCODING in that buffer the draw is to
 *     read: one allocation holds many sets at multiples of encodedLength, and the records
 *     this library binds from are keyed by it.
 *   - for a BUFFER ARGUMENT, it is which block of a buffer the shader's `constant` or
 *     `device` pointer starts at: the ordinary case of an application that packs several
 *     uniform blocks into one allocation and moves the offset per draw. The window server
 *     of this port does exactly that -- 55 bindings of one boot arrived as
 *     "fragment buffer 2 at offset 1164" -- and until this field existed each of them was
 *     a refused binding.
 *
 * What is done with a buffer argument's offset, which is not the same on both bindings
 * because Gallium's two bindings are not the same:
 *
 *   - a `constant` member or argument (a UBO) is ALWAYS served. An offset the GPU can
 *     fetch from directly is passed as the binding's own offset and nothing is copied;
 *     one it cannot -- a6xx wants multiples of 64 and softpipe 16, and Metal allows any
 *     multiple of 4 -- is bound as a `user_buffer`, which both back ends handle by
 *     uploading the bytes themselves (freedreno's upload_user_buffer aligns to 64,
 *     softpipe wraps the pointer). What is copied is `air.buffer_size` where the metadata
 *     carries one, which is all the shader can read, and the rest of the buffer
 *     otherwise.
 *   - a `device` member or argument (an SSBO) at an offset the GPU cannot fetch from is
 *     REFUSED BY NAME, and the reason is not symmetry: the shader may WRITE it, and the
 *     copy that serves a UBO would take the writes with it when it was thrown away.
 *     Gallium has no user_buffer on a shader buffer for the same reason.
 *
 * It is a SECOND array beside `rwa_bindings.buffers` rather than a field in it, and the
 * reason is this project's own scar tissue: `rwa_bindings` is embedded BY VALUE in
 * rwa_draw, twice, so a field added to it moves `primitive`, `vertex_start` and every
 * index field after them for a driver built against the other header -- which is exactly
 * how a primitive type came back as 1871292816 once. Nothing in rwa_bindings moves for
 * this feature.
 *
 * A driver fills this for every index it binds a buffer at, and `rwa_bindings.buffers`
 * exactly as before. Where both name a buffer they must name the SAME one: two tables
 * that disagree about index n are a driver filling one of them from the wrong place, and
 * that is refused by name rather than resolved in favour of either.
 */
struct rwa_buffer_bind {
	struct rwa_buffer *buffer;   /* NULL: nothing bound at this index */
	size_t offset;               /* Metal's own `offset:` */
};

/*
 * What a dispatch is given. Each array is bound at indices 0..count-1, which is the
 * convention the front end already uses for a kernel's arguments.
 *
 * A shader that declares a texture the caller did not bind must not be dispatched: it would
 * read a slot nothing was bound to, and the answer would look like a result. This library
 * refuses that, by name, which is why rwa_compute_info reports the counts a shader wants.
 */
struct rwa_bindings {
	struct rwa_buffer *const *buffers;
	unsigned buffer_count;
	struct rwa_texture *const *textures;
	unsigned texture_count;
	struct rwa_sampler *const *samplers;
	unsigned sampler_count;
};

/*
 * One dispatch. `groups` is the grid in threadgroups and `group_size` the threads in one of
 * them, both as Metal counts them.
 *
 * Returns 0, or -1 with a sentence in `why`. A dispatch that the GPU does not finish is a
 * failure here and says so: before it waited on a fence, air_run printed its own fill pattern
 * as the result of 21 of 47 kernels in one run (docs/air.md section 5).
 */
int rwa_dispatch(struct rwa_compute *compute, const struct rwa_bindings *bindings,
		 const unsigned groups[3], const unsigned group_size[3],
		 char *why, size_t why_size);

/*
 * The same dispatch, with the kernel's BUFFER TABLE: `table` is indexed by Metal's own
 * buffer index, as rwa_bindings.buffers is, and covers `table_count` slots from 0. It is
 * what carries an argument buffer's encoding offset and a buffer argument's own offset --
 * see struct rwa_buffer_bind, which has the whole of what each means.
 *
 * rwa_dispatch is this with no table at all: every offset is then zero, and a kernel that
 * declares an argument buffer is refused through it by name.
 *
 * A second entry point, because rwa_bindings is embedded by value in rwa_draw and two more
 * fields would move half of it for a driver built against the other header (rwa_buffer_bind);
 * a new symbol is absent in an older library, which the driver's dlsym treats as too old.
 */
int rwa_dispatch_buffers(struct rwa_compute *compute, const struct rwa_bindings *bindings,
			 const struct rwa_buffer_bind *table, unsigned table_count,
			 const unsigned groups[3], const unsigned group_size[3],
			 char *why, size_t why_size);

/*
 * rwa_dispatch_buffers with the length Metal's -setThreadgroupMemoryLength:atIndex: gave
 * for threadgroup buffer 0, in bytes. A kernel that declares such a buffer is refused when
 * the length is 0; for any other kernel the length is ignored. A new symbol for the reason
 * rwa_dispatch_buffers gives; rwa_dispatch_nowait leaves the wait to a later rwa_flush's fence.
 */
int rwa_dispatch_threadgroup(struct rwa_compute *compute, const struct rwa_bindings *bindings,
			     const struct rwa_buffer_bind *table, unsigned table_count,
			     const unsigned groups[3], const unsigned group_size[3],
			     unsigned threadgroup_bytes, char *why, size_t why_size);
int rwa_dispatch_nowait(struct rwa_compute *compute, const struct rwa_bindings *bindings,
			const struct rwa_buffer_bind *table, unsigned table_count, const unsigned groups[3],
			const unsigned group_size[3], unsigned threadgroup_bytes, char *why, size_t why_size);

/*
 * One part of a -dispatchThreads: whose threadgroup size does not divide the grid
 * (struct air_grid_split in nir_air.h has what the fields mean): `groups` and `group_size`
 * are this part's, and the kernel sees its threads where they sit in the whole grid.
 * `imageblock_size` as rwa_dispatch_nowait_imageblock (NULL or {0, 0}: none set); `wait` as
 * rwa_dispatch_threadgroup (true) or rwa_dispatch_nowait (false).
 */
struct rwa_grid_split {
	unsigned base_group[3], nominal[3], grid_threads[3], grid_groups[3];
};
int rwa_dispatch_split(struct rwa_compute *compute, const struct rwa_bindings *bindings,
		       const struct rwa_buffer_bind *table, unsigned table_count,
		       const unsigned groups[3], const unsigned group_size[3],
		       unsigned threadgroup_bytes, const unsigned imageblock_size[2],
		       const struct rwa_grid_split *split, bool wait, char *why, size_t why_size);

/* Wait for everything dispatched so far, with a real fence. 0, or -1 and a sentence. */
int rwa_finish(struct rwa_device *device, char *why, size_t why_size);

/* Hand over everything recorded so far, without waiting: what a Metal -commit does. The
 * fence is kept on the device, and the next rwa_finish waits for it. 0, or -1. */
int rwa_flush(struct rwa_device *device, char *why, size_t why_size);

/* The fence of the last rwa_flush, referenced (NULL: nothing handed over yet); rwa_fence_wait
 * waits for it without the library's lock, 0 or -1 after the timeout, from any thread. */
struct rwa_fence;
struct rwa_fence *rwa_fence_take(struct rwa_device *device);
int rwa_fence_wait(struct rwa_device *device, struct rwa_fence *fence);
void rwa_fence_release(struct rwa_device *device, struct rwa_fence *fence);

/*
 * ---------------------------------------------------------------------------------------
 * Drawing.
 *
 * The same shape of contract as compute, and for the same reason: AIR in, a pipeline out,
 * and nothing of NIR or Gallium in the caller's view.
 *
 * One thing here is not symmetrical, and it is deliberate. `vertex_bitcode` may be NULL,
 * and then this library supplies a pass-through vertex stage covering the whole attachment.
 * That is not a convenience: it is the road that already has evidence behind it. air_run
 * draws every fragment shader of the corpus that way -- a generated quad vertex shader plus
 * the real fragment shader -- and 255 of them have been drawn and compared pixel by pixel
 * (docs/air.md). A driver asked for a pipeline whose vertex function is one of Apple's own
 * three vertex shaders passes both modules instead, and then nothing is generated.
 */
struct rwa_render;
struct rwa_depth_stencil;

/*
 * Depth and stencil, which are ONE attachment of two aspects and not two attachments,
 * and both sides of the translation force that:
 *
 *   - Metal keeps two descriptors, MTLRenderPassDescriptor.depthAttachment and
 *     .stencilAttachment, and they are two views of one resource: for
 *     MTLPixelFormatDepth32Float_Stencil8 an application sets THE SAME texture on both,
 *     because there is one texture. The two pipeline properties are the same shape --
 *     depthAttachmentPixelFormat and stencilAttachmentPixelFormat, both set to the
 *     combined format. (Read off the cache: both attachment descriptors are backed by
 *     one MTLRenderPassAttachmentDescriptorPrivate that holds clearColor, clearDepth AND
 *     clearStencil together, and both -setTexture: make the same
 *     conformsToProtocol:MTLTextureImplementation check as the colour one --
 *     0x181a4245c and 0x181a40e54.)
 *   - Gallium has one `zsbuf` in a framebuffer and one resource behind it, and
 *     PIPE_FORMAT_Z24_UNORM_S8_UINT or PIPE_FORMAT_Z32_FLOAT_S8X24_UINT is one format
 *     with two aspects, cleared through two bits of one mask.
 *
 * So `depth_format` and `stencil_format` are both here, as the pipeline descriptor has
 * them, and this library requires them to name one texture: equal, or one of them zero.
 * A pipeline that gives two different formats is refused by name, because what it is
 * asking for -- a separate depth surface and stencil surface -- is a thing neither
 * Metal's own descriptor nor a Gallium framebuffer can hold, and guessing which of the
 * two to honour would silently drop the other.
 */
#include "air_metal_render.h"

#ifdef __cplusplus
}
#endif

#endif /* AIR_METAL_H */
