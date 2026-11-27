#ifndef AIR_METAL_RENDER_H
#define AIR_METAL_RENDER_H

/* Render pipeline, depth/stencil, draw, and buffer-texture declarations, included by air_metal.h. */

/*
 * THE VERTEX ATTRIBUTE LAYOUT: MTLVertexDescriptor, and the one thing this contract was
 * missing when the phone's own window server first reached it. ONE of its render
 * pipeline specifications was refused by name, 80 times over 42 lives of backboardd --
 * "reads vertex attributes ([[stage_in]]), and this
 * contract carries no vertex attribute layout (rwa_render_shape has no formats, strides
 * or offsets)" -- because the shader's half of that match is all AIR carries
 * (docs/air.md 3.1: Apple's compiler flattens the stage-in struct and the format, the
 * offset, the stride and the buffer are nowhere in the module) and the API's half had
 * nowhere to arrive.
 *
 * It is TWO arrays because Metal's descriptor is two, and each is indexed by Metal's own
 * index rather than packed, because Metal's are:
 *
 *   - `attribute[n]` is MTLVertexAttributeDescriptor at `vertexDescriptor.attributes[n]`,
 *     where n is the shader's [[attribute(n)]]. The indices a real shader uses are not
 *     contiguous -- QuartzCore's std_vert0_lpf declares 0, 1 and 3 -- so a packed array
 *     with a count would need a second field to say which index each entry is, and that
 *     is a field to disagree with the entry.
 *   - `layout[k]` is MTLVertexBufferLayoutDescriptor at `vertexDescriptor.layouts[k]`,
 *     where k is the buffer index an attribute names and the index the stream is bound
 *     at (rwa_draw.vertex_streams).
 *
 * There is no count for either, for the same reason Metal has none: an attribute is
 * configured when its `format` is not MTLVertexFormatInvalid (0), which is exactly what
 * a freshly made MTLVertexDescriptor holds everywhere, and a layout is read only when an
 * attribute names its index. So a zeroed rwa_render_shape is a pipeline with no vertex
 * layout at all, which is every pipeline this library drew before now.
 *
 * In Gallium this becomes one `pipe_vertex_element` per attribute the shader FETCHES, in
 * driver_location order, plus one `pipe_vertex_buffer` per buffer index at the draw --
 * see rwa_draw.vertex_streams, and air_metal.c for why that order is the only one that
 * feeds the right attribute.
 */
struct rwa_vertex_attribute {
	unsigned format;         /* MTLVertexFormat; 0 (Invalid) is "not configured" */
	unsigned offset;         /* bytes from the start of the element, as Metal's is */
	unsigned buffer_index;   /* which stream it is fetched from: layout[] and
				  * rwa_draw.vertex_streams are indexed by this */
};

struct rwa_vertex_layout {
	unsigned stride;         /* bytes between one vertex's element and the next */
	unsigned step_function;  /* MTLVertexStepFunction: Constant 0, PerVertex 1,
				  * PerInstance 2, PerPatch 3, PerPatchControlPoint 4 */
	/* Metal's stepRate: with PerInstance, how many consecutive instances share one
	 * element, which is Gallium's instance_divisor exactly. ZERO IS READ AS ONE,
	 * because Metal's own default is 1 and a caller's struct filled in only as far
	 * as it cared about leaves this at 0 -- the same reading rwa_draw.instance_count
	 * gets, and for the same reason. With Constant there is no rate to honour (every
	 * vertex and every instance reads element 0) and this field is not read at all.
	 */
	unsigned step_rate;
};

/* Metal's own limits, from Apple's feature tables: 31 attributes and 31 buffer indices.
 * The modules corroborate the buffer half on this device -- every one of them carries
 * `air.max_device_buffers i32 31` -- and Gallium has room for both (PIPE_MAX_ATTRIBS is
 * 32 for elements and for buffers alike). What the SHADER side can use is narrower and
 * is the frontend's business: NIR has sixteen VERT_ATTRIB_GENERIC slots, so an
 * attribute index of 16 or more is refused when the module is translated, long before
 * one of these entries is read.
 */
#define RWA_MAX_VERTEX_ATTRIBUTES 31
#define RWA_MAX_VERTEX_BUFFERS    31

/*
 * THE BLEND STATE: MTLRenderPipelineColorAttachmentDescriptor's other half, and the
 * second thing this contract did not have at all.
 *
 * What the absence looked like. iOS's own window server composites the home screen
 * through this library, and CoreAnimation composites its layers with PREMULTIPLIED
 * ALPHA: a layer's texel holds colour already multiplied by its own coverage, and the
 * pipeline is asked for source-over -- source times one, plus destination times one
 * minus the source's alpha. With no blend state in the shape, every pipeline this
 * library made was
 *
 *     create_blend_state(pipe, &(struct pipe_blend_state){ .rt[0].colormask = RGBA })
 *
 * which is blending OFF: the source replaces the destination. A label layer whose
 * background is transparent black then lands as OPAQUE BLACK, which is exactly what the
 * phone showed -- every icon label in a black box, the Search pill a black rectangle,
 * the dock edged in dark red, the widgets flat white or black slabs. Not a refusal and
 * not a crash: a picture, wrong in a way only a photograph could report.
 *
 * WHAT IS CARRIED, and it is Metal's own eight properties per colour attachment, as
 * Metal's own numbers. The library maps each enumeration BY TABLE, refuses what it
 * cannot serve BY NAME, and derives nothing from a cast -- MTLBlendFactor and
 * pipe_blendfactor agree on exactly two of nineteen values, MTLColorWriteMask's bits run
 * in the OPPOSITE ORDER to Gallium's colormask, and air_metal.c has both tables with the
 * arithmetic of every disagreement written out.
 *
 * WHAT THE COMPOSITOR ACTUALLY ASKS FOR, measured and not assumed. CoreAnimation builds
 * every one of its pipelines in one function
 * (CA::OGL::MetalContext::create_pipeline_state, 0x188378090 of this device's
 * QuartzCore), out of a 55-entry table of blend modes at 0x18855c092, and what it sends
 * is readable instruction by instruction:
 *
 *     mode 3  "sov"  blendingEnabled YES, One / OneMinusSourceAlpha, Add, both halves,
 *                    and writeMask NEVER SENT -- so All, Apple's default
 *     mode 9  "dot"  Zero / OneMinusSourceAlpha      mode 12 "add"  One / One
 *     mode 15 "mul"  DestinationColor / Zero         mode 8  "din"  Zero / SourceAlpha
 *     mode 6  "sap"  DestinationAlpha / OneMinusSourceAlpha
 *     mode 13 "sbs"  One / One, ReverseSubtract      mode 16 "scn"  One / OneMinusSourceColor
 *
 * Of the 1079 pipelines in this device's own precompiled archive
 * (QuartzCore.framework/default.pipelinelib/pipelines.desc, whose names carry the mode),
 * 472 are source-over, 33 "dot", 31 "add", 19 "mul", 13 "din", 3 "sap", 2 "sbs", 1
 * "scn", 1 "ldg" -- and NOT ONE uses a factor this library refuses. The refusals below
 * cost the home screen nothing; they are there for the applications.
 *
 * AND 504 OF THOSE 1079 CARRY NO BLEND STATE AT ALL, which is the trap. Those layers
 * blend INSIDE THE FRAGMENT SHADER -- the mode is lifted into the fixed-function blender
 * only when it is one of the first 26 and some other conditions hold
 * (Pipeline::Spec::lift_blend_function, 0x18834859c), and the twenty-eight Photoshop-like
 * modes from "pld" onwards never are -- so their descriptor arrives UNTOUCHED, at
 * Apple's own defaults. Those defaults were read off
 * -[MTLRenderPipelineColorAttachmentDescriptorInternal init] (0x181a2e9a8), which writes
 * the word 0x78001080:
 *
 *     blendingEnabled NO, both operations Add, sourceRGB One, sourceAlpha One,
 *     destinationRGB Zero, destinationAlpha Zero, writeMask All (0xf)
 *
 * SO: THE SOURCE FACTOR OF A PIPELINE THAT DOES NOT BLEND IS ONE, AND ITS WRITE MASK IS
 * 0xF. A driver that inferred "this descriptor carries blending" from any field being
 * non-zero would turn the blender on for 504 of 1079 pipelines that have already blended
 * in their shader, and double-blend the home screen. `blendingEnabled` is the only field
 * that says, and when it is false this library reads NOTHING ELSE of the entry but the
 * write mask -- which is Metal's own rule.
 *
 * `color_blend_count` IS NOT DECORATION, and it is the one field here that is not
 * Metal's. It says how many entries of `color_blend[]` the caller filled in, and ZERO
 * MEANS NONE AT ALL -- blending off, all four channels written, which is the state every
 * draw this library ever made used and what a shape built before this field existed
 * still asks for. It has to exist because every field of a blend descriptor has a
 * meaningful zero and NOT ONE of those zeros is Metal's default, as the word above
 * shows:
 *
 *     blendingEnabled    false     -- which IS Metal's default, so zero is safe here
 *     writeMask          None (0)  -- Metal's default is All (0xf), and a zeroed entry
 *                                     would write NOTHING: a pipeline that draws
 *                                     nothing at all, and no refusal to say why
 *     the four factors    Zero     -- and Metal's defaults are One, One, Zero, Zero
 *     the two operations  Add      -- the one field whose zero IS the default
 *
 * So there is no value of `write_mask` that can mean "the caller never said", and the
 * project has already paid once for reading a meaningful zero as an absence: compare
 * function 8 is `never`, which is what Apple's own metal_stdlib writes into an inline
 * sampler, and a test for "not zero" refused 653 of 653 draws. The count is how this
 * contract tells None from never-said, and it is one number about the array rather than
 * a second opinion inside each entry.
 *
 * THE ENTRIES REALLY DO DIFFER PER ATTACHMENT on this device, so independent blending is
 * not a hypothetical: create_pipeline_state's own multiple-attachment loop sends
 * `setWriteMask: 0` to every attachment that is not the primary one and sends
 * `setBlendingEnabled:` to none of them, while the primary one gets the blend. That is
 * exactly the case Gallium's `independent_blend_enable` exists for, and exactly the case
 * where leaving it clear makes rt[0] govern them all.
 *
 * A caller that gives FEWER entries than `color_count` attachments is refused by name:
 * filling in attachment 0 and leaving attachment 1 to the zeros would turn the second
 * attachment's blending silently off, which is the failure this whole field exists to
 * end.
 *
 * It is in the SHAPE because that is where Metal puts it -- blend state is compiled into
 * the pipeline, not set on the encoder -- with ONE exception that is therefore refused:
 * the four factors that read Metal's BLEND COLOUR
 * (-setBlendColorRed:green:blue:alpha:) are encoder state, this contract has nowhere to
 * carry it, and the driver's own setter is a stub. See the refusal list at
 * rwa_render_create.
 */
struct rwa_color_blend {
	unsigned blending_enabled;               /* isBlendingEnabled */
	unsigned rgb_blend_operation;            /* MTLBlendOperation */
	unsigned alpha_blend_operation;          /* MTLBlendOperation */
	unsigned source_rgb_blend_factor;        /* MTLBlendFactor */
	unsigned destination_rgb_blend_factor;   /* MTLBlendFactor */
	unsigned source_alpha_blend_factor;      /* MTLBlendFactor */
	unsigned destination_alpha_blend_factor; /* MTLBlendFactor */
	/* MTLColorWriteMask, which is FOUR BITS IN THE OTHER ORDER FROM GALLIUM'S: Metal
	 * has Red at 1 << 3 and Alpha at 1 << 0 where PIPE_MASK_R is 0x1 and PIPE_MASK_A
	 * is 0x8. All (0xf) and None (0) are the same number either way, and so are
	 * green|blue and red|alpha, so twelve of the sixteen masks are exchanged by a
	 * cast and four are not -- which is why a test that only ever asks for All would
	 * pass with the bits backwards. Anything above 0xf is refused by name.
	 */
	unsigned write_mask;
};

struct rwa_render_shape {
	unsigned color_format[8];      /* MTLPixelFormat per colour attachment */
	unsigned color_count;
	unsigned depth_format;         /* MTLPixelFormat, or 0 for none */
	unsigned stencil_format;       /* the same format again, or 0: see above */
	unsigned sample_count;

	/*
	 * MTLRenderPipelineDescriptor.vertexDescriptor, appended for the reason the
	 * indexed fields of rwa_draw were appended: userland/src/annibalemetal.m holds a
	 * copy of this struct typed out by hand, and appending leaves every field that
	 * driver already fills at the offset it already reads. Both still have to be
	 * rebuilt together.
	 *
	 * It is in the SHAPE and not in the draw because that is where Metal puts it: the
	 * vertex descriptor is pipeline state, compiled once, and -setVertexBuffer: is
	 * encoder state. The two halves of one fetch therefore arrive in two calls, and
	 * what each is checked against is different -- the layout against the shader at
	 * creation, the buffers against the layout at the draw.
	 */
	struct rwa_vertex_attribute vertex_attribute[RWA_MAX_VERTEX_ATTRIBUTES];
	struct rwa_vertex_layout vertex_layout[RWA_MAX_VERTEX_BUFFERS];

	/*
	 * MTLRenderPipelineColorAttachmentDescriptor's blend state, one entry per colour
	 * attachment, and APPENDED -- 8 * 32 + 4 = 260 bytes at the END of this struct,
	 * with every field above it left at the offset it already had.
	 *
	 * That is not tidiness. userland/src/annibalemetal.m holds a copy of this struct
	 * typed out by hand, and this port has paid twice for a field inserted in the
	 * MIDDLE of a shared struct: once with a primitive type arriving as 1871292816
	 * (rwa_draw, see there), and once when struct air_resource grew by eight bytes
	 * and every resource after the first was read eight bytes early. Appending is
	 * what keeps the damage of a half-rebuilt pair to the new field alone. Both
	 * copies still have to be rebuilt together.
	 *
	 * `color_blend_count` of 0 is "this shape carries no blend state", which is
	 * blending off and all four channels written -- see struct rwa_color_blend for
	 * why a count is needed and a zeroed entry cannot stand in for one.
	 */
	struct rwa_color_blend color_blend[8];
	unsigned color_blend_count;
};

/*
 * ONE THING ABOUT THE FOUR COUNTS BELOW, now that an argument buffer's members are
 * bindings of their own: they count the Gallium slots the stage READS, members included,
 * because that is what they have always meant and what a mismatch would be refused
 * against. A member is not a slot the driver fills, though -- it is filled from the
 * encoder's record (rwa_argbuf_bind) -- so a driver that bound `fragment_textures`
 * textures of its own would be binding over the members' slots. What it binds directly
 * is what the application bound at Metal's own indices, and nothing else; the masks
 * below say which buffer indices are sets.
 */
struct rwa_render_info {
	unsigned vertex_buffers;       /* what each stage binds, so a driver can refuse a */
	unsigned fragment_buffers;     /* mismatch before it draws rather than read a slot */
	unsigned fragment_textures;    /* nothing was bound to */
	unsigned fragment_samplers;
	unsigned generated_vertex_stage;   /* 1 when this library supplied the vertex stage */

	/* 1 when the fragment shader stores to [[depth]], and then the depth buffer holds
	 * what the shader computed rather than the interpolated z. Reported rather than
	 * made a requirement, because Metal does not require a depth attachment for such a
	 * shader -- it drops the writes -- and a caller that measures a shader needs to
	 * know which of the two it is looking at. Apple's own texturedFragment2DDepth and
	 * texturedFragment3DDepth are the two in the corpus: each declares a colour output
	 * and stores nothing to it, so with no depth attachment they draw and produce
	 * nothing anyone can read.
	 */
	unsigned fragment_writes_depth;

	/*
	 * What the vertex descriptor turned into, which a driver needs for two things it
	 * cannot work out on its own: how many stream slots a draw has to fill, and what
	 * Metal's pipeline reflection would have to report.
	 *
	 * `vertex_attribute_count` is how many attributes the vertex stage FETCHES -- the
	 * number of pipe_vertex_elements built, which is the used attributes of
	 * air_shader_info and not the configured entries of the descriptor: an attribute
	 * the module marks air.arg_unused costs no fetch, and a configured attribute no
	 * module reads is ignored exactly as Metal ignores it.
	 *
	 * `vertex_attribute_index[i]` is the Metal [[attribute(n)]] of element i, so the
	 * mapping this library built can be read back and compared with the shader's own
	 * interface instead of being taken on trust. The order is driver_location order,
	 * which is the order AIR lists the attributes with the unused ones skipped -- for
	 * std_vert0_lpf (attributes 0, 1 unused, 3) the two entries are 0 and 3.
	 *
	 * `vertex_streams` is the number of slots rwa_draw.vertex_streams must cover
	 * (the highest buffer index any fetched attribute names, plus one) and
	 * `vertex_stream_mask` is which of them are actually fetched from. A slot in the
	 * count but not in the mask needs nothing bound: Gallium's set_vertex_buffers
	 * takes a count from slot 0 where Metal's -setVertexBuffer:atIndex: takes an
	 * index, so a descriptor that uses index 5 alone is still an array of six with
	 * five empty entries.
	 */
	unsigned vertex_attribute_count;
	unsigned vertex_attribute_index[16];
	unsigned vertex_streams;
	unsigned vertex_stream_mask;

	/* And which Metal buffer indices of each stage are ARGUMENT BUFFERS, one bit
	 * each. Metal's one table per stage holds three things now -- a vertex stream,
	 * a buffer argument and a set -- and only the shader and the vertex descriptor
	 * together say which an index is: the descriptor's layouts are
	 * vertex_stream_mask above, the module's sets are these, and what is in neither
	 * mask is an ordinary [[buffer(n)]] argument. A driver that cannot tell them
	 * apart would hand a set's buffer to a shader expecting a constant block.
	 *
	 * Appended for the reason the attribute fields above were appended, and the
	 * same warning applies: this struct is filled in by the library, so a driver
	 * built against the shorter one is handed more than it has room for. Both still
	 * have to be rebuilt together.
	 */
	unsigned vertex_argbuf_mask;
	unsigned fragment_argbuf_mask;

	/*
	 * WHAT THE BLEND STATE BECAME, which is here for the reason
	 * `vertex_attribute_index` is: it is the only way a caller can read back the
	 * mapping this library made instead of trusting it.
	 *
	 * `blend_enabled_mask` is one bit per colour attachment, set when that attachment
	 * ends up blending.
	 *
	 * `color_write_masks` is four bits per attachment, attachment i at bits 4*i, and
	 * they are GALLIUM's bits -- PIPE_MASK_R 0x1 .. PIPE_MASK_A 0x8 -- and NOT
	 * MTLColorWriteMask's, which run the other way round. That is the whole point of
	 * reporting it: for any mask but All, None, green|blue and red|alpha the number
	 * here MUST differ from the writeMask the caller passed, and a log line where the
	 * two agree on, say, 0xe is a log line saying the bits were never reversed.
	 *
	 * Appended, with the warning the two fields above carry: this struct is filled in
	 * BY the library, so a driver built against the shorter one is handed more than it
	 * has room for. Both still have to be rebuilt together.
	 */
	unsigned blend_enabled_mask;
	unsigned color_write_masks;
};

/*
 * MTLDepthStencilDescriptor, which is a device object in Metal and a created state here
 * for the same reason a sampler is: it is compiled once and bound at every draw, and a
 * compare function Metal does not have must be refused when the state is made rather
 * than at the draw that was going to use it.
 *
 * The fields are MTLDepthStencilDescriptor's own, and the two faces are its
 * frontFaceStencil and backFaceStencil. Nothing here is a "stencil enabled" flag,
 * because Metal has none: its defaults -- compare Always, every operation Keep -- ARE
 * the off state, and this library derives Gallium's `enabled` bit from them rather than
 * taking a flag a Metal driver has nothing to fill in with.
 *
 * `read_mask` and `write_mask` are Metal's uint32 masks of an eight-bit buffer; only the
 * low eight bits can select anything and that is what is used.
 */
struct rwa_stencil_face {
	unsigned compare_function;              /* MTLCompareFunction */
	unsigned stencil_fail_operation;        /* MTLStencilOperation */
	unsigned depth_fail_operation;          /* MTLStencilOperation */
	unsigned depth_stencil_pass_operation;  /* MTLStencilOperation */
	unsigned read_mask, write_mask;
};

struct rwa_depth_stencil_state {
	unsigned depth_compare_function;   /* MTLCompareFunction */
	unsigned depth_write_enabled;      /* isDepthWriteEnabled */
	struct rwa_stencil_face front, back;
};

struct rwa_depth_stencil *rwa_depth_stencil_create(struct rwa_device *device,
						   const struct rwa_depth_stencil_state *state,
						   char *why, size_t why_size);
void rwa_depth_stencil_destroy(struct rwa_depth_stencil *state);

/*
 * WHAT A VERTEX LAYOUT IS REFUSED FOR, each refusal naming what was asked for, and every
 * one of them is THIS LIBRARY'S refusal rather than freedreno's for a reason that was
 * read in freedreno's own source: fd6_vertex_state_create does
 * `assert(fmt != FMT6_NONE)` and then programs the register with whatever it has, and
 * A6XX_VFD_DECODE_INSTR_OFFSET() is a masking setter over a 12-bit field. A driver that
 * hands freedreno a layout it cannot fetch gets an assertion in a debug build and a
 * wrong fetch in a release one -- so the sentence has to be written here, where the
 * caller's own words are still in hand.
 *
 *   - an attribute of air_shader_info.vertex_attributes_read with no configured
 *     attribute in the descriptor, UNLESS the module gates it with a function-constant
 *     predicate. The fetch would read a VFD slot nothing was set up for; the gated case
 *     is legal and is the one exception, because the parameter exists whatever the
 *     predicate says and a pipeline whose constants make it false may configure nothing
 *     there (docs/air.md 3.1, and six of QuartzCore's 46 attributes are such).
 *   - a format whose converted type is not the type the shader declared: an integer
 *     format for a `float` member or a normalised one for a `uint` member. This is what
 *     air_vertex_attribute.base_type is for, and why the frontend keeps INT and UINT
 *     apart although `int4` and `uint4` are the same <4 x i32>.
 *   - MTLVertexFormatInt1010102Normalized, UInt1010102Normalized, and the two formats
 *     the iOS 16.5 SDK has not got at all (FloatRG11B10 = 54 and FloatRGB9E5 = 55):
 *     every one of those is several channels packed into one word, and the pixel format
 *     table in air_metal.c declines packed formats for the reason spelled out there --
 *     Metal's channel order and Gallium's are two conventions and a mistake between them
 *     does not fail, it swaps two channels of a picture that still looks like a picture.
 *     FloatRGB9E5 has a second reason that is not a convention: PIPE_FORMAT_R9G9B9E5_FLOAT
 *     is texture-only in fd6_format_table.c, so fd6_vertex_format() answers FMT6_NONE
 *     and the VFD cannot fetch it however the channels are ordered.
 *   - MTLVertexStepFunctionPerPatch and PerPatchControlPoint: tessellation, which has no
 *     pipe_vertex_element field and no stage in this port.
 *   - PerVertex (or Constant) with a step rate other than 1: Gallium has a divisor for
 *     instances and nothing to express a rate over vertices with.
 *   - an attribute offset or a layout stride above 4095 bytes. Metal allows more, and
 *     this is the refusal that matters most because the alternative is silent:
 *     A6XX_VFD_DECODE_INSTR_OFFSET__MASK is 0x0001ffe0 at shift 5, twelve bits, and the
 *     setter ANDs rather than asserts, so offset 4096 would fetch offset 0.
 *   - a buffer index that is also a [[buffer(n)]] the vertex module reads. Metal has one
 *     table behind -setVertexBuffer:offset:atIndex: and both the layouts and the stage's
 *     buffer arguments read out of it, so an index is either a stream or an argument and
 *     never both; an application that configured both wants two things in one slot.
 *   - a vertex descriptor on a pipeline whose vertex stage this library GENERATED. That
 *     stage draws the library's own quad out of the library's own buffer; a descriptor
 *     for it would describe memory the caller does not own.
 *
 * AND WHAT A BLEND STATE IS REFUSED FOR. Every one of these would otherwise DRAW
 * SOMETHING -- a picture with the wrong arithmetic in it -- which is the failure the
 * blend fields were added to end, so each is a sentence with the caller's own words in
 * it:
 *
 *   - MTLBlendFactorBlendColor, OneMinusBlendColor, BlendAlpha and OneMinusBlendAlpha
 *     (11-14). Gallium has all four (PIPE_BLENDFACTOR_CONST_COLOR and friends) and so
 *     does the hardware; what nothing here has is the COLOUR. Metal keeps it on the
 *     encoder, -setBlendColorRed:green:blue:alpha:, so it is not pipeline state and not
 *     in this shape -- and the driver's own setter is a stub that drops it. Honouring
 *     them would blend against the zero a Gallium context starts with, which is
 *     "multiply by nothing": the layer would vanish instead of failing. Serving them is
 *     one float[4] in rwa_draw and one call to set_blend_color, on the day something
 *     asks.
 *   - MTLBlendFactorSource1Color, OneMinusSource1Color, Source1Alpha and
 *     OneMinusSource1Alpha (15-18): dual-source blending, where the second factor comes
 *     from a SECOND fragment output at [[color(0), index(1)]]. Neither this contract nor
 *     this frontend carries a second output -- air_shader_info has one colour per target
 *     and nothing names an index -- so the factor would read whatever register happened
 *     to be there.
 *   - MTLBlendFactorSourceAlphaSaturated as the sourceAlphaBlendFactor or the
 *     destinationAlphaBlendFactor -- and only there. For the three colour channels the
 *     factor is min(As, 1 - Ad) in Metal and in GL alike and it is served; for the ALPHA
 *     channel GL defines it as 1 and Metal's one equation does not, and softpipe
 *     implements GL's ("multiply alpha by 1.0", sp_quad_blend.c). Serving the alpha half
 *     would mean choosing one of two readings of a number the NEXT composite reads.
 *   - MTLBlendOperationMin or Max with any factor other than MTLBlendFactorOne. Metal's
 *     equation multiplies by the factors and then takes the minimum; GL ignores the
 *     factors for these two operations entirely. One is the identity under both readings
 *     and is served, and it is the only form a maximum-blend is ever written in --
 *     anything else draws correctly on one back end and wrongly on the other, with
 *     nothing in the picture to say which.
 *   - a factor above 18 or an operation above 4: not a value of Metal's enumeration at
 *     all, and the number is printed, because that is what a half-rebuilt pair of
 *     structs looks like from this side.
 *   - a writeMask above 0xf. MTLColorWriteMask is four bits; anything else is a field
 *     nobody set or a struct read at the wrong offset.
 *   - blending enabled on a colour attachment whose MTLPixelFormat is a PURE INTEGER
 *     format (RGBA8Uint, R32Sint and the rest). Metal forbids it, GL and Vulkan forbid
 *     it, and there is no arithmetic to do: a blend equation over integers has no
 *     agreed meaning and freedreno would program the blender anyway.
 *   - blend state for fewer attachments than the shape has colour formats: see
 *     rwa_render_shape.color_blend_count.
 *   - colour attachments whose blend states DIFFER on a screen that cannot blend them
 *     independently (pipe_caps.indep_blend_enable / indep_blend_func). Gallium's rt[0]
 *     governs them all when independent blending is off, so the second attachment would
 *     silently get the first one's equation.
 */
struct rwa_render *rwa_render_create(struct rwa_device *device,
				     const void *vertex_bitcode, size_t vertex_size,
				     const char *vertex_entry,
				     const void *fragment_bitcode, size_t fragment_size,
				     const char *fragment_entry,
				     const struct rwa_render_shape *shape,
				     struct rwa_render_info *info, char *why, size_t why_size);

/* rwa_render_create with functions linked into the fragment stage. */
struct rwa_render *rwa_render_create_linked(struct rwa_device *device,
					    const void *vertex_bitcode, size_t vertex_size,
					    const char *vertex_entry,
					    const void *fragment_bitcode, size_t fragment_size,
					    const char *fragment_entry,
					    const struct rwa_linked_function *fragment_linked,
					    unsigned num_fragment_linked,
					    const struct rwa_render_shape *shape,
					    struct rwa_render_info *info, char *why, size_t why_size);
void rwa_render_destroy(struct rwa_render *render);

/*
 * One draw into one render pass, which is what MTLRenderPassDescriptor plus one
 * -drawPrimitives: amounts to. The colour attachments are textures made by this library.
 *
 * `clear_color`: NULL for Load; indexed RGBA for Clear (`discard` bits 8..15; no bits clears attachment 0).
 * `primitive` is MTLPrimitiveType as Metal defines it.
 *
 * The depth/stencil attachment and its clears are the same shape -- the texture or NULL, and
 * a pointer to a value or NULL for load -- because a pass clears depth and keeps stencil often
 * enough that one load action for both would be a lie about one of them. `depth_stencil` NULL
 * is Metal's state when an application has set none: the test always passes and nothing is
 * written, which is also what every draw this library has made until now did, there having
 * been no depth attachment to test against.
 *
 * `stencil_reference` is front and back, and it is in the DRAW and not in the state object
 * because that is where Metal puts it: -setStencilReferenceValue: is a method of the render
 * command encoder, not a property of MTLDepthStencilDescriptor.
 *
 * `depth_range` is MTLViewport's znear and zfar, the only part of a viewport this contract
 * carries, and it is here because without it a depth test has nothing to test: the vertex
 * stage this library generates puts all four corners at clip z = 0, and Metal's NDC z runs
 * from 0 at the near plane, so the window depth of that quad is exactly `znear`. Two draws
 * that differ only in `depth_range` are the same picture at two depths. NULL is { 0, 1 },
 * which is what every draw before this one used.
 *
 * Returns 0, or -1 with a sentence. A pass the GPU does not finish is a failure here, on the
 * same grounds as a dispatch: a picture that was never drawn must not read as a result.
 *
 * ---------------------------------------------------------------------------------------
 * INDEXED DRAWING, which is what every draw in a real application is.
 *
 * `index_buffer` NULL is -drawPrimitives:vertexStart:vertexCount:, the only form this
 * contract had, and everything below is then zero. A buffer is one of the three direct
 * -drawIndexedPrimitives: forms, and the fields are those calls' arguments one for one:
 *
 *   -drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:
 *                        [:instanceCount:[baseVertex:baseInstance:]]
 *
 * It is an `rwa_buffer` because Metal's index buffer is an `id <MTLBuffer>` and nothing
 * else: a driver has the application's buffer object in hand, and on the device that is
 * one this library made (-newBufferWithLength:options:). A fourth form takes the whole
 * argument list out of a buffer the GPU may write (MTLDrawIndexedPrimitivesIndirect-
 * Arguments) and is NOT here: there is nothing in this contract a driver could put those
 * five words into, and a draw whose count is read on the GPU cannot be checked on the
 * processor at all -- which is the half of this contract that keeps a wrong picture from
 * reading as a result. The driver declines that form by name.
 *
 * THE COUNT IS `vertex_count`, and there is deliberately no second count field. Metal
 * has two names for one role (`vertexCount` without an index buffer, `indexCount` with
 * one) and Gallium has one: `pipe_draw_start_count_bias.count` is however many elements
 * the draw walks, whichever kind they are. A second field would be a second thing to
 * disagree with the first, and this library's whole job at a draw is to refuse the
 * disagreements -- so an indexed draw of three indices sets vertex_count = 3, and
 * vertex_count = 0 is still "a draw of no vertices" and still refused.
 *
 * `vertex_start` must be ZERO for an indexed draw and is refused otherwise, because
 * Metal's indexed forms have no vertexStart: where in the index buffer to start is
 * `index_buffer_offset` and nothing else. The two are the same field on the Gallium side
 * and so cannot both be honoured -- and a caller that filled in both wants something
 * Metal cannot express.
 *
 * `index_buffer_offset` is in BYTES, as Metal's own argument is, and Apple's header
 * states the rule this library enforces by name: "indexBufferOffset must be a multiple
 * of the index size". That rule is not a formality here, it is the reason the conversion
 * exists at all: Gallium counts the start in ELEMENTS (both back ends do --
 * a6xx emits `first_indx = draw->start` against the resource's base, and softpipe hands
 * the draw module the resource's base with `eltSize`), so the byte offset becomes
 * offset / index size, exactly when Metal's own rule holds. An odd offset with
 * MTLIndexTypeUInt16 is that refusal's commonest case and it names the type.
 *
 * `index_type` is MTLIndexType, which has exactly two values -- UInt16 = 0, UInt32 = 1
 * (MTLStageInputOutputDescriptor.h) -- and anything else is refused by name rather than
 * read as one of them. There is no 8-bit index type in Metal, although both back ends
 * have one.
 *
 * `base_vertex` is added to every index before the vertex is fetched, and it is signed
 * because Metal says so in as many words: "baseVertex Offset for vertex_id. NOTE: this
 * can be negative". Gallium's `index_bias` is the same signed int in the same place, and
 * both back ends honour it (fd6_draw.cc's `index_start`, the draw module's `eltBias`).
 * With no index buffer it must be zero: Metal's non-indexed forms have no baseVertex,
 * and a non-zero one there is a caller inventing an argument.
 *
 * `instance_count` and `base_instance` belong in this same struct because that is where
 * Metal puts them -- one call carries indexCount, instanceCount, baseVertex and
 * baseInstance together, and there is no "set the instance count" method on the encoder
 * to carry them separately. ZERO IS READ AS ONE: every caller written before this field
 * existed leaves it zeroed, and Metal's non-instanced forms draw exactly one instance,
 * so the two readings agree. `base_instance` is Metal's "Offset for instance_id", which
 * is what the frontend already assumes of it -- air.instance_id translates to
 * instance_id + base_instance, because Metal's [[instance_id]] counts from the base and
 * GL's gl_InstanceID does not.
 *
 * What this library CHECKS before it draws, each refusal naming what was asked for: the
 * index type; the offset against the index size; that the indices asked for are inside
 * the buffer (offset + count * size <= its length), because both back ends answer an
 * index past the end with something that looks like a picture -- softpipe substitutes
 * index 0 ("if the index buffer would overflow we return index 0", draw_private.h) and
 * a6xx clamps against `max_indices` computed from the resource's width; and, when the
 * vertex stage was GENERATED, that every index lands on one of that quad's four corners
 * once base_vertex is added, which is the indexed form of the vertex_start check that
 * was already here. For a SUPPLIED vertex stage an index is a vertex id and there is
 * nothing to bound it against: the shader reads its own buffer with it, and reading past
 * that buffer is the application's business, exactly as it is in Metal.
 *
 * Primitive restart is not in this contract because it is not in Metal: there is no
 * restart index anywhere in MTLRenderCommandEncoder, so 0xFFFF is an ordinary index
 * here, and Gallium's `primitive_restart` stays off. A driver ported from GL would get
 * that backwards.
 */
struct rwa_vertex_stream {
	struct rwa_buffer *buffer;   /* NULL: nothing bound at this index */
	size_t offset;               /* bytes, Metal's own `offset:` argument */
};

struct rwa_draw {
	struct rwa_texture *const *color;
	unsigned color_count;
	const float *clear_color;      /* NULL, or attachment-indexed RGBA values */
	struct rwa_texture *depth;     /* the depth/stencil attachment, or NULL for none */
	const float *clear_depth;      /* NULL, or one float: MTLClearDepth */
	const unsigned *clear_stencil; /* NULL, or one value: MTLClearStencil */
	const struct rwa_depth_stencil *depth_stencil;   /* NULL: no test, no write */
	unsigned stencil_reference[2];                   /* front, back */
	const float *depth_range;      /* NULL, or { znear, zfar } */
	struct rwa_bindings vertex;
	struct rwa_bindings fragment;
	unsigned primitive;
	unsigned vertex_start, vertex_count;

	/* The indexed forms. Appended rather than placed beside `primitive` on purpose:
	 * userland/src/annibalemetal.m holds a copy of this struct typed out by hand, and
	 * appending leaves every field that driver already fills at the offset it already
	 * reads. Both still have to be rebuilt together -- a driver compiled against one
	 * shape of this struct calling a library built with the other read a primitive
	 * type back as 1871292816 -- and the refusals above are what turns that mismatch
	 * into a sentence instead of a picture: an index type of 1871292816, an offset no
	 * index size divides and a count no buffer holds are all refused by name.
	 */
	struct rwa_buffer *index_buffer;   /* NULL: the non-indexed -drawPrimitives: */
	unsigned index_type;               /* MTLIndexType: UInt16 = 0, UInt32 = 1 */
	size_t index_buffer_offset;        /* bytes, and a multiple of the index size */
	int base_vertex;                   /* added to every index; may be negative */
	unsigned instance_count;           /* 0 is read as one */
	unsigned base_instance;            /* Metal's offset for [[instance_id]] */

	/*
	 * THE VERTEX STREAMS: -setVertexBuffer:offset:atIndex:, indexed by that index,
	 * which is the index rwa_render_shape.vertex_layout is indexed by as well.
	 *
	 * These are the memory the vertex fetch reads, and they are in the DRAW because
	 * that is where Metal keeps them -- the descriptor is pipeline state and the
	 * buffer is encoder state, and the same pipeline draws two meshes out of two
	 * buffers.
	 *
	 * It is a second array beside `vertex.buffers` rather than the same one, and both
	 * come off the same table in the driver, because of two things that differ:
	 *
	 *   - the OFFSET. rwa_bindings has no room for one, and a stream's offset is not
	 *     the same thing as a buffer argument's: this one is which MESH to draw, it
	 *     is checked against the layout's stride and the draw's own vertex range, and
	 *     dropping it would draw the wrong mesh rather than fail. A buffer argument's
	 *     offset arrives in `*_buffer_table` below, where an argument buffer's does.
	 *   - WHAT IS CHECKED. A slot in `vertex.buffers` has to satisfy what the module
	 *     declares at [[buffer(n)]]; a slot here has to satisfy the layout. The two
	 *     sets must not overlap (rwa_render_create refuses it), so a driver may pass
	 *     its one table as both and this library takes each index from the side that
	 *     owns it.
	 *
	 * `vertex_stream_count` is how many slots this array covers, as Gallium's
	 * set_vertex_buffers counts them: from slot 0, empty entries and all. A slot the
	 * pipeline fetches from and this array leaves NULL is refused by name.
	 *
	 * What is CHECKED about a stream, besides its presence: that the last element the
	 * draw walks is inside the buffer -- offset + stride * (highest vertex index) +
	 * the format's own size, against the buffer's length less the stream offset.
	 * Reading past a vertex buffer is bounded rather than wild on both of these back
	 * ends (freedreno programs VFD_FETCH_SIZE from the resource, softpipe clamps in
	 * the draw module), so what it produces is a picture made partly of zeros, which
	 * is the kind of wrong answer this contract exists to turn into a sentence. A
	 * stream whose layout steps per INSTANCE is not checked that way and says so:
	 * which element an instance reads is the hardware's arithmetic over the base
	 * instance and the divisor, and a bound computed from the wrong formula would
	 * refuse draws that are perfectly legal.
	 */
	const struct rwa_vertex_stream *vertex_streams;
	unsigned vertex_stream_count;

	/*
	 * METAL'S BUFFER TABLE for each stage, at Metal's own indices and with Metal's
	 * own offsets, covering `*_buffer_table_count` slots from 0: an argument
	 * buffer's encoding offset and a buffer argument's own offset, which
	 * `vertex.buffers` and `fragment.buffers` have nowhere to put. struct
	 * rwa_buffer_bind says what each means and what is done with it.
	 *
	 * A driver fills these from the same table it fills `vertex.buffers` and
	 * `fragment.buffers` from -- the one Metal has -- and this library takes from
	 * each side what that side owns. A stream's offset is NOT here: a vertex stream
	 * is `vertex_streams` above, which has its own, and a buffer index is never both
	 * (rwa_render_create refuses a descriptor that claims an index the module reads).
	 *
	 * APPENDED, and that is the whole reason they are four fields here instead of
	 * two in rwa_bindings: `vertex` and `fragment` are embedded by value above, so a
	 * field added inside rwa_bindings moves `primitive` and everything after it for
	 * any driver built against the other shape of this header -- which is how a
	 * primitive type once arrived as 1871292816. These four move nothing.
	 *
	 * A set the pipeline declares with nothing bound at its index is refused by
	 * name, and so is one whose buffer holds no encoding: see rwa_buffer_bind.
	 */
	const struct rwa_buffer_bind *vertex_buffer_table;
	unsigned vertex_buffer_table_count;
	const struct rwa_buffer_bind *fragment_buffer_table;
	unsigned fragment_buffer_table_count;

	/* { originX, originY, width, height } in pixels, or NULL for the whole attachment,
	 * which is Metal's default and what every draw did before the rectangle was served.
	 * CoreAnimation renders a layer into an attachment larger than the layer and places
	 * it with this rectangle: with the rectangle ignored, the layer's quad covered the
	 * whole attachment and the content arrived at the wrong size and position.
	 * APPENDED for the reason the fields above are. */
	const float *viewport;

	/* { x, y, width, height } in pixels from the attachment's top left, or NULL for no
	 * clip. Metal's default is the whole attachment and CoreAnimation sets it to a layer's
	 * bounds; a draw that ignores it paints outside the layer the caller is filling.
	 * APPENDED for the reason above. */
	const unsigned *scissor;

	/* The resolveTexture of each colour attachment, or NULL where the attachment has
	 * none. Metal's MSAA resolve is not a draw: the pass renders into a multisample
	 * texture and the resolved one-sample result is written when the pass ends. There
	 * is no end-of-pass call here -- a Metal pass is a sequence of rwa_render_draw
	 * calls carrying the same attachments -- so the resolve happens at the end of
	 * every draw that names one. The multisample attachment keeps its contents, so a
	 * second resolve after the next draw leaves the same result, at the cost of
	 * resolving once per draw rather than once per pass.
	 *
	 * An entry must be a one-sample texture whose size and format match its colour
	 * attachment; a mismatch is refused by name at the draw. APPENDED for the reason
	 * `viewport` is.
	 */
	struct rwa_texture *const *resolve;

	/*
	 * The colour attachments the pass does not load (MTLLoadActionDontCare), bit i for
	 * attachment i, on the pass's first draw only and 0 on the others. Appended, like the
	 * fields above it, and filled by the same driver build. What the library does with it is
	 * a hint to the GPU driver that the attachment's contents can be left out of the tile
	 * memory; the picture is the same with it ignored.
	 */
	unsigned discard;
	/* { r, g, b, a } of -setBlendColorRed:green:blue:alpha:, or NULL for {0, 0, 0, 0}. */
	const float *blend_color;
};

int rwa_render_clear(struct rwa_device *, const struct rwa_draw *, char *, size_t);
int rwa_render_draw(struct rwa_render *render, const struct rwa_draw *draw,
		    char *why, size_t why_size);

/* A texture over a buffer the caller holds (-newTextureWithDescriptor:offset:bytesPerRow:), 2D with
 * one level and one sample. Its bytes are copied from the buffer when it is made and at every
 * rwa_buffer_textures_refresh; once it is drawn into, written as an image, copied into or written by
 * the processor, they go back at the next refresh or at rwa_buffer_textures_writeback. */
struct rwa_texture *rwa_texture_from_buffer(struct rwa_device *device,
					    const struct rwa_texture_shape *shape,
					    struct rwa_buffer *source, size_t offset,
					    unsigned row_bytes, char *why, size_t why_size);
void rwa_buffer_textures_refresh(struct rwa_device *device);
void rwa_buffer_textures_writeback(struct rwa_device *device);

/* A second texture over the same resource: Metal's -newTextureViewWithPixelFormat: and the
 * level range of -newTextureViewWithPixelFormat:textureType:levels:slices:. The view covers
 * num_levels levels from first_level, every slice, and reads and renders the memory as
 * mtl_format, whose texels must have the size of the texture's own. Level L of the view is
 * level first_level + L of the texture. Destroyed with rwa_texture_destroy, independently of
 * the texture it was made from.
 */
struct rwa_texture *rwa_texture_view(struct rwa_texture *texture, unsigned mtl_format,
				     unsigned first_level, unsigned num_levels,
				     char *why, size_t why_size);
void rwa_texture_set_swizzle(struct rwa_texture *texture, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void rwa_texture_set_view_slices(struct rwa_texture *texture, unsigned mtl_type,
				 unsigned first_slice, unsigned num_slices);

#endif /* AIR_METAL_RENDER_H */
