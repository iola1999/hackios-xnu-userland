/* Vertex attribute layout tests, included into metal_drv_test.m. */


/*
 * --draw-attrib: the vertex attribute layout ON THE HARDWARE, through Metal's own API.
 *
 * docs/metal.md 10.44 put MTLVertexDescriptor into the contract and measured it on this
 * machine's softpipe: the offsets, the stride, the formats, the buffer index, the step
 * function and its rate each changed what came back, and the element order proved to be the
 * shader's driver_location order and not Metal's attribute order. What that measurement
 * could NOT touch is the only part made of hardware: the VFD -- Adreno's vertex fetch unit
 * -- which softpipe does not have at all. So three things in it were left as honest
 * negatives, and this mode is here to settle them on the phone:
 *
 *   - the SWAP. MTLVertexFormatUChar4Normalized_BGRA becomes PIPE_FORMAT_B8G8R8A8_UNORM, and
 *     nothing about the byte order is in the fetch's format field: freedreno puts it in
 *     VFD_DECODE_INSTR.swap, which fd6_vertex_swap() reads off the pipe_format. Measured by
 *     storing the coordinate so that it is only right if the swap happens: the first byte of
 *     the element is ZERO and the coordinate lives in bytes 2 and 1, which the BGRA order
 *     delivers as x and y. Without the swap the shader's x is that zero, every pixel samples
 *     column 0 of the texture, and the x gradient of the picture disappears.
 *   - the INSTANCED bit and STEP_RATE, which are two more fields of the same register
 *     (VFD_DECODE_INSTR.instanced and VFD_DECODE[k].STEP_RATE) and have never been
 *     programmed by this driver. Measured in the picture, because no colour attachment can
 *     show an instance count: the coordinate comes out of a SECOND stream that steps per
 *     instance, so all four vertices of one instance share one coordinate and every pixel
 *     holds ONE texel; per vertex instead, the four vertices get four coordinates and the
 *     picture is a gradient of four texels.
 *   - the PLACEHOLDER element for an attribute gated by a function constant, which 10.44
 *     proved accepted and not fetched: on the host the pipeline for such a module dies
 *     inside softpipe before anything can be drawn with it (the `_lph` variants on the
 *     16-bit narrowing of a `half4` attribute, the `_lpf` ones on `store_scratch`). Here ir3 compiles it, so the question becomes
 *     the one that matters: does the GPU come back. An unconfigured attribute that got NO
 *     element at all would leave an empty VFD slot, and an empty slot fetches from address 0
 *     -- which is the shape of the fault that cost this port a boot in 10.18.
 *
 * And it carries over the thing that makes the host measurement evidence rather than a
 * picture: THE DECOY. The module's own [[attribute(1)]] is air.arg_unused -- nothing reads
 * it -- and this mode configures it anyway, pointing at bytes that hold a different texture
 * coordinate. An element array built in Metal's attribute order, or over all three
 * attributes in order, feeds that layout to attribute 3's fetch: the geometry stays
 * perfectly right and every pixel comes back (100, 200, 300, 400). That is the only wrong
 * answer here that a coverage check could not see.
 *
 * The pipeline is Apple's own code on both sides and out of two libraries, which is a legal
 * Metal pipeline and the shape a real one has: QuartzCore's std_vert0_lpf (position is
 * `mvp_matrix * attribute(0)`, so an IDENTITY matrix in its [[buffer(2)]] makes the position
 * the attribute verbatim, and its [[attribute(3)]] `color` becomes the first varying) over
 * AppleMetalGLRenderer's texturedFragment2DUInt (samples texture 0 at that varying and
 * writes the texel to four RGBA32Uint targets). So the COVERAGE measures attribute 0 and
 * every WORD measures attribute 3: pixel (x, y) must hold the texel its own interpolated
 * coordinate lands in, (100 + x, 200 + y, 300 + x + y, 400), up to the vertical orientation
 * of 10.32 -- both orientations are counted and the one that matched is printed, because
 * which attribute fed the fetch has the same answer either way.
 *
 *   metal_drv_test --draw-attrib [--vertexlib PATH] [--vertex NAME] [--gated NAME]
 *                  [--fragment NAME] FRAGMENTLIB
 */

#define ATTR_N 8          /* elements in the stream */
#define ATTR_STREAM 5     /* Metal buffer index of the interleaved stream */
#define ATTR_INSTANCE 6   /* ... of the per-instance stream */
#define ATTR_OTHER 7      /* ... and of the same stream bound somewhere else */

static const char *attrib_vertex_lib =
    "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
static const char *attrib_gated_function = "fixed_vert_lpf_spc";

/*
 * The eight elements of the stream: the four corners of the full-screen quad with a texture
 * coordinate running the whole [0, 1] square, then four at half the distance from the origin
 * whose coordinate is the far corner. The second four exist for one step -- the same data
 * read at twice the stride, where the fetch walks 0, 2, 4, 6 and must produce a picture
 * nothing else here produces.
 */
static const float attrib_pos[ATTR_N][4] = {
	{ -1.0f, -1.0f, 0.0f, 1.0f }, {  1.0f, -1.0f, 0.0f, 1.0f },
	{ -1.0f,  1.0f, 0.0f, 1.0f }, {  1.0f,  1.0f, 0.0f, 1.0f },
	{ -0.5f, -0.5f, 0.0f, 1.0f }, {  0.5f, -0.5f, 0.0f, 1.0f },
	{ -0.5f,  0.5f, 0.0f, 1.0f }, {  0.5f,  0.5f, 0.0f, 1.0f },
};
static const float attrib_uv[ATTR_N][2] = {
	{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f },
	{ 1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 1.0f },
};

/* What the DECOY points at, in every element: the centre of texel (0, 0), so an
 * implementation that fed this layout to attribute 3's fetch comes back with
 * (100, 200, 300, 400) in all sixteen pixels -- one number, named in advance, and not a
 * plausible gradient. */
static const float attrib_decoy_uv[2] = { 0.125f, 0.125f };

/* The per-instance stream: four coordinates, two in opposite corners of the texture and two
 * in the other two. Instance 1's is the far corner, which is the texel the instanced step
 * must leave in every pixel; a stream that stepped per vertex would hand these four to the
 * four vertices of ONE instance and the picture would be a gradient instead. */
static const float attrib_instance_uv[4][2] = {
	{ 0.125f, 0.125f }, { 0.875f, 0.875f }, { 0.125f, 0.875f }, { 0.875f, 0.125f },
};

struct attrib_slot {
	NSUInteger format;   /* MTLVertexFormat */
	NSUInteger offset;
	NSUInteger buffer;
};

struct attrib_layout {
	const char *what;
	NSUInteger stride;
	struct attrib_slot pos, uv, decoy;
};

/* Metal's Short4Normalized is value / 32767, so 1.0 is 32767 exactly and the quad's corners
 * are exact; the half-size ones are not, and nothing here compares them. */
static int16_t
attrib_snorm16(float v)
{
	float s = v * 32767.0f;

	return (int16_t)(s < 0.0f ? s - 0.5f : s + 0.5f);
}

/* UChar4Normalized: 255 is 1.0 exactly, so a coordinate of 0 or 1 survives the encoding. */
static unsigned char
attrib_unorm8(float v)
{
	return (unsigned char)(v * 255.0f + 0.5f);
}

/*
 * The stream, written in whatever encoding the layout names. Always ATTR_N elements of the
 * layout's own stride, so a step that reads it at twice the stride has something defined to
 * read instead of running off the end (and being refused for it, which is a different
 * measurement and has its own step).
 *
 * The BGRA case is the swap measurement and is the only place here where the bytes are NOT
 * in the order the components are: byte 0 is zero, byte 1 is v and byte 2 is u, because
 * MTLVertexFormatUChar4Normalized_BGRA delivers (x, y, z, w) = (byte2, byte1, byte0, byte3).
 * If the swap is not programmed the shader's x is byte 0 -- the zero -- and the picture
 * loses its x gradient entirely.
 */
static void
attrib_fill(unsigned char *mem, size_t size, const struct attrib_layout *l)
{
	memset(mem, 0, size);
	for (unsigned i = 0; i < ATTR_N; i++) {
		unsigned char *e = mem + (size_t)i * l->stride;

		if (l->pos.format == MTLVertexFormatFloat4) {
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
		if (l->uv.format == MTLVertexFormatFloat4) {
			float v[4] = { attrib_uv[i][0], attrib_uv[i][1], 0.0f, 1.0f };

			memcpy(e + l->uv.offset, v, sizeof(v));
		} else if (l->uv.format == MTLVertexFormatUChar4Normalized_BGRA) {
			unsigned char v[4] = { 0, attrib_unorm8(attrib_uv[i][1]),
					       attrib_unorm8(attrib_uv[i][0]), 255 };

			memcpy(e + l->uv.offset, v, sizeof(v));
		} else {
			unsigned char v[4] = { attrib_unorm8(attrib_uv[i][0]),
					       attrib_unorm8(attrib_uv[i][1]), 0, 255 };

			memcpy(e + l->uv.offset, v, sizeof(v));
		}
		if (l->decoy.format == MTLVertexFormatFloat2) {
			float v[2] = { attrib_decoy_uv[0], attrib_decoy_uv[1] };

			memcpy(e + l->decoy.offset, v, sizeof(v));
		} else {
			/* 0.125 as a half: 2^-3 is exponent field 15 - 3 = 12 with a zero
			 * mantissa, which is 0x3000. Written out because this file has no
			 * float-to-half of its own and the value is a constant. */
			uint16_t v[2] = { 0x3000, 0x3000 };

			memcpy(e + l->decoy.offset, v, sizeof(v));
		}
	}
}

/*
 * The layout as Metal's own object. Both of its arrays are filled at Metal's own indices,
 * and attribute 1 -- the one the module marks air.arg_unused -- is configured on purpose:
 * that is the decoy, and a descriptor configuring an attribute no shader reads is legal and
 * ignored by Metal.
 */
static MTLVertexDescriptor *
attrib_descriptor(const struct attrib_layout *l, NSUInteger step_function,
		  NSUInteger step_rate, NSInteger instance_buffer)
{
	MTLVertexDescriptor *vd = [[MTLVertexDescriptor alloc] init];
	const struct attrib_slot *slot[3] = { &l->pos, &l->decoy, &l->uv };
	const unsigned at[3] = { 0, 1, 3 };

	for (unsigned i = 0; i < 3; i++) {
		MTLVertexAttributeDescriptor *a =
		    [[vd attributes] objectAtIndexedSubscript:at[i]];

		[a setFormat:(MTLVertexFormat)slot[i]->format];
		[a setOffset:slot[i]->offset];
		[a setBufferIndex:slot[i]->buffer];
	}
	MTLVertexBufferLayoutDescriptor *bl =
	    [[vd layouts] objectAtIndexedSubscript:l->pos.buffer];

	[bl setStride:l->stride];
	[bl setStepFunction:MTLVertexStepFunctionPerVertex];
	[bl setStepRate:1];
	if (instance_buffer >= 0) {
		MTLVertexBufferLayoutDescriptor *il =
		    [[vd layouts] objectAtIndexedSubscript:(NSUInteger)instance_buffer];

		[il setStride:8];        /* one float2 coordinate per element */
		[il setStepFunction:(MTLVertexStepFunction)step_function];
		[il setStepRate:step_rate];
	}
	return vd;
}

/* One pipeline over four RGBA32Uint attachments, which is what texturedFragment2DUInt
 * writes. The reason for a sentence rather than a nil is the whole of 10.44: every refusal
 * in this path is the compiler library's own and names what was asked for. */
static id<MTLRenderPipelineState>
attrib_pipeline(id<MTLDevice> dev, id<MTLFunction> vertex, id<MTLFunction> fragment,
		MTLVertexDescriptor *vd, const char *what, bool quiet)
{
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	NSError *err = nil;

	[pd setVertexFunction:vertex];
	[pd setFragmentFunction:fragment];
	[pd setVertexDescriptor:vd];
	for (unsigned i = 0; i < 4; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatRGBA32Uint];
	}
	id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
									    error:&err];

	[pd release];
	if (ps == nil && !quiet) {
		printf("drvtest: attrib: %s got no pipeline: %s\n", what,
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
	}
	if (ps == nil && quiet) {
		printf("drvtest: attrib:   %s: refused -- %s\n", what,
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
	}
	return ps;
}

/* Everything a draw of this mode binds, so that eight draws differ in the layout and in
 * nothing else. The vertex ARGUMENTS (the matrices) and the vertex STREAMS come off one
 * table, as Metal has one; the pipeline's own mask is what splits them in the driver. */
struct attrib_bind {
	id<MTLCommandQueue> queue;
	MTLRenderPassDescriptor *rp;
	id<MTLTexture> target;                /* the attachment read back */
	id<MTLBuffer> argument[8];            /* by Metal buffer index: the matrices */
	id<MTLBuffer> stream[8];              /* by Metal buffer index */
	NSUInteger stream_offset[8];
	id<MTLTexture> tex;
	id<MTLSamplerState> samp;
};

/*
 * One draw and the picture it left, into attachments cleared to zero by the pass. Returns
 * false and says where it stopped: this driver RECORDS the draw and performs it on -commit
 * (10.13), so "no encoder", "status" and "the GPU did not finish" are three different
 * answers and the log has to tell them apart.
 */
static bool
attrib_draw(const struct attrib_bind *b, id<MTLRenderPipelineState> ps,
	    NSUInteger instances, id<MTLBuffer> indices, NSUInteger index_count,
	    NSUInteger index_offset, unsigned grid[16][4], const char *what)
{
	id<MTLCommandBuffer> cmd = [b->queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:b->rp];

	if (enc == nil) {
		printf("drvtest: attrib: %s got no encoder\n", what);
		return false;
	}
	[enc setRenderPipelineState:ps];
	for (unsigned i = 0; i < 8; i++) {
		if (b->argument[i] != nil) {
			[enc setVertexBuffer:b->argument[i] offset:0 atIndex:i];
		}
		if (b->stream[i] != nil) {
			[enc setVertexBuffer:b->stream[i] offset:b->stream_offset[i] atIndex:i];
		}
	}
	[enc setFragmentTexture:b->tex atIndex:0];
	[enc setFragmentSamplerState:b->samp atIndex:0];
	if (indices != nil) {
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
				indexCount:index_count
				 indexType:MTLIndexTypeUInt16
			       indexBuffer:indices
			 indexBufferOffset:index_offset
			     instanceCount:instances
				baseVertex:0
			      baseInstance:0];
	} else {
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
			vertexStart:0
			vertexCount:4
		      instanceCount:instances];
	}
	[enc endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];
	if ([cmd status] != RW_CB_COMPLETED) {
		printf("drvtest: attrib: %s: status %lu%s%s\n", what, (unsigned long)[cmd status],
		       [cmd error] != nil ? " -- " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String] : "");
		return false;
	}
	memset(grid, 0, sizeof(unsigned[16][4]));
	[b->target getBytes:grid
		bytesPerRow:4 * 16
		 fromRegion:MTLRegionMake2D(0, 0, 4, 4)
		mipmapLevel:0];
	return true;
}

/*
 * What the picture says, counted rather than eyeballed. 10.32 established that the texture
 * comes back mirrored vertically against a naive reading of the coordinate, so BOTH
 * orientations are counted and the one that matched is printed: what this mode is about is
 * WHICH attribute fed the fetch, and that question has the same answer either way.
 */
struct attrib_verdict {
	unsigned x_gradient;    /* pixels whose first word is 100 + x */
	unsigned mirrored;      /* ... and whose second is 200 + (3 - y) */
	unsigned upright;       /* ... or 200 + y */
	unsigned decoy_texel;   /* pixels holding texel (0, 0): the decoy's own answer */
	unsigned column0;       /* pixels holding texel (0, y): what a dropped BGRA swap gives */
	unsigned uniform;       /* pixels equal to the pixel at (0, 0) */
	unsigned covered;
};

static void
attrib_judge(const unsigned grid[16][4], struct attrib_verdict *v)
{
	memset(v, 0, sizeof(*v));
	for (unsigned y = 0; y < 4; y++) {
		for (unsigned x = 0; x < 4; x++) {
			const unsigned *w = grid[y * 4 + x];

			v->covered += w[0] != 0;
			v->x_gradient += w[0] == 100 + x;
			v->mirrored += w[0] == 100 + x && w[1] == 200 + (3 - y);
			v->upright += w[0] == 100 + x && w[1] == 200 + y;
			v->decoy_texel += w[0] == 100 && w[1] == 200 && w[2] == 300 &&
					  w[3] == 400;
			v->column0 += w[0] == 100;
			v->uniform += w[0] == grid[0][0] && w[1] == grid[0][1] &&
				      w[2] == grid[0][2] && w[3] == grid[0][3];
		}
	}
}

static void
attrib_print(const char *what, const unsigned grid[16][4], const struct attrib_verdict *v)
{
	printf("drvtest: attrib: %s\n", what);
	printf("drvtest: attrib:   coverage ");
	for (unsigned y = 0; y < 4; y++) {
		for (unsigned x = 0; x < 4; x++) {
			printf("%c", index_at(grid, x, y) ? '#' : '.');
		}
		printf("%s", y < 3 ? "/" : "");
	}
	printf("  %u of 16; (0,0) = %u %u %u %u, (3,3) = %u %u %u %u\n", v->covered,
	       grid[0][0], grid[0][1], grid[0][2], grid[0][3],
	       grid[15][0], grid[15][1], grid[15][2], grid[15][3]);
	printf("drvtest: attrib:   %u of 16 hold texel x, %u hold (x, 3-y), %u hold (x, y), "
	       "%u hold texel (0,0), %u hold column 0, %u are the same as (0,0)\n",
	       v->x_gradient, v->mirrored, v->upright, v->decoy_texel, v->column0, v->uniform);
}

/* Whether a picture is the texel grid the layout describes: every pixel its own texel, in
 * one of the two vertical orientations, over the whole attachment. */
static bool
attrib_is_grid(const struct attrib_verdict *v)
{
	return v->covered == 16 && v->x_gradient == 16 && (v->mirrored == 16 || v->upright == 16);
}

static void
attrib_path(id<MTLDevice> dev, const char *fragment_lib)
{
	NSError *err = nil;
	/* Two libraries, two MTLLibrary objects, one pipeline -- which Metal allows and a real
	 * pipeline does: the attributes are QuartzCore's and the shader that turns their
	 * varying into a readable texel is the GL renderer's. */
	NSData *vdata = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:attrib_vertex_lib]];
	NSData *fdata = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:fragment_lib]];

	if (vdata == nil || fdata == nil) {
		printf("drvtest: attrib: cannot read %s\n", vdata == nil ? attrib_vertex_lib
								         : fragment_lib);
		return;
	}
	dispatch_data_t vdd = dispatch_data_create([vdata bytes], [vdata length], NULL,
						   DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	dispatch_data_t fdd = dispatch_data_create([fdata bytes], [fdata length], NULL,
						   DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> vlib = [dev newLibraryWithData:vdd error:&err];
	id<MTLLibrary> flib = [dev newLibraryWithData:fdd error:&err];

	if (vlib == nil || flib == nil) {
		printf("drvtest: attrib: newLibraryWithData: gave nil for %s\n",
		       vlib == nil ? attrib_vertex_lib : fragment_lib);
		return;
	}
	/* The vertex function's default is the module 10.44 measured on this machine, and the
	 * fragment function's is the only shader in reach that turns a varying into a word a
	 * pixel can be read as. --vertex and --fragment override either; the fragment default
	 * is spelled out rather than taken from `fragment_function`, whose own default
	 * (clearFragment20) writes a constant and would make every picture here identical. */
	const char *vname = vertex_function != NULL ? vertex_function : "std_vert0_lpf";
	const char *fname = (fragment_function != NULL &&
			     strcmp(fragment_function, "clearFragment20") != 0)
	    ? fragment_function : "texturedFragment2DUInt";
	id<MTLFunction> vertex = [vlib newFunctionWithName:
	    [NSString stringWithUTF8String:vname]];
	id<MTLFunction> fragment = [flib newFunctionWithName:
	    [NSString stringWithUTF8String:fname]];

	if (vertex == nil || fragment == nil) {
		printf("drvtest: attrib: no %s in %s\n", vertex == nil ? vname : fname,
		       vertex == nil ? attrib_vertex_lib : fragment_lib);
		return;
	}
	printf("drvtest: attrib: %s (%s) over %s (%s)\n", vname, attrib_vertex_lib, fname,
	       fragment_lib);

	/* Four attachments, because that shader writes four, and 4x4 as everywhere else in
	 * this program: a pixel here, a pixel out of air_metal_test and a pixel out of air_run
	 * are the same measurement. */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:4
							     height:4
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *rp = [[MTLRenderPassDescriptor renderPassDescriptor] retain];

	for (unsigned i = 0; i < 4; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: attrib: attachment %u could not be made\n", i);
			return;
		}
		MTLRenderPassColorAttachmentDescriptor *a =
		    [[rp colorAttachments] objectAtIndexedSubscript:i];

		[a setTexture:targets[i]];
		[a setLoadAction:MTLLoadActionClear];
		[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
		[a setStoreAction:MTLStoreActionStore];
	}

	/* The sampled texture, air_run's texels, and the sampler Metal gives by default:
	 * nearest, clamp to edge, normalised coordinates -- so the texel the coordinate falls
	 * in is the texel that comes back, with no weights to round. */
	MTLTextureDescriptor *std_ =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:4
							     height:4
							  mipmapped:NO];

	[std_ setUsage:MTLTextureUsageShaderRead];
	id<MTLTexture> tex = [dev newTextureWithDescriptor:std_];

	if (tex == nil) {
		printf("drvtest: attrib: the sampled texture could not be made\n");
		return;
	}
	/*
	 * Seeded in ONE call and read back before anything is drawn, which is draw_path's own
	 * rule and for its reason: per row it would be four calls with a non-zero region
	 * origin, and then every picture below would depend on the driver getting origin.y
	 * right -- a thing this mode is not about. And a draw that samples a texture nobody
	 * has looked at cannot tell a wrong sample from a wrong seed.
	 */
	{
		unsigned char all[4 * 4 * 16], back[4 * 4 * 16];
		unsigned stride_bytes = texel_bytes_of(MTLPixelFormatRGBA32Uint);

		for (unsigned y = 0; y < 4; y++) {
			for (unsigned x = 0; x < 4; x++) {
				float v[4] = { 100.0f + x, 200.0f + y, 300.0f + x + y, 400.0f };

				write_texel(all + (y * 4 + x) * stride_bytes,
					    MTLPixelFormatRGBA32Uint, v);
			}
		}
		[tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4)
		       mipmapLevel:0
			 withBytes:all
		       bytesPerRow:4 * stride_bytes];
		memset(back, 0, sizeof(back));
		[tex getBytes:back
		  bytesPerRow:4 * stride_bytes
		   fromRegion:MTLRegionMake2D(0, 0, 4, 4)
		  mipmapLevel:0];
		for (unsigned y = 0; y < 4; y++) {
			printf("drvtest: attrib: seeded row %u reads back", y);
			for (unsigned x = 0; x < 4; x++) {
				float v[4] = { 0, 0, 0, 0 };

				read_texel(back + (y * 4 + x) * stride_bytes,
					   MTLPixelFormatRGBA32Uint, v);
				printf(" %g/%g", (double)v[0], (double)v[1]);
			}
			printf("   (x/y of each texel; row %u must read y = %u)\n", y, 200 + y);
		}
	}
	MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
	id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

	[sd release];

	/*
	 * The vertex stage's own buffer arguments: mvp_matrix at [[buffer(2)]] gets the
	 * IDENTITY, which is what makes the position the attribute verbatim and the picture
	 * computable; texmat at [[buffer(3)]] is air.arg_unused in this module and is bound
	 * because the module declares it; [[buffer(4)]] is bound for the gated module, which
	 * declares a chroma_matrix there. The identity is the identity in either column order,
	 * which is the whole reason this harness can fill a matrix it did not define.
	 */
	struct attrib_bind b;

	memset(&b, 0, sizeof(b));
	b.queue = [dev newCommandQueue];
	b.rp = rp;
	b.target = targets[3];
	b.tex = tex;
	b.samp = samp;
	for (unsigned i = 2; i <= 4; i++) {
		b.argument[i] = [dev newBufferWithLength:64 options:0];
		if (b.argument[i] == nil || [b.argument[i] contents] == NULL) {
			printf("drvtest: attrib: buffer %u could not be made\n", i);
			return;
		}
		float *f = [b.argument[i] contents];

		memset(f, 0, 64);
		for (unsigned k = 0; k < 4; k++) {
			f[k * 4 + k] = 1.0f;
		}
	}

	/* The two shapes of the stream, and everything about them differs except the numbers
	 * they encode: the stride, all three offsets and all three formats. */
	static const struct attrib_layout wide = {
		"1. Float4 at offset 16 of a 48-byte element, stream at buffer index 5",
		48,
		{ MTLVertexFormatFloat4, 16, ATTR_STREAM },
		{ MTLVertexFormatFloat4, 0, ATTR_STREAM },
		{ MTLVertexFormatFloat2, 32, ATTR_STREAM },
	};
	static const struct attrib_layout narrow = {
		"2. Short4Normalized at offset 4 of a 16-byte element, the same four vertices",
		16,
		{ MTLVertexFormatShort4Normalized, 4, ATTR_STREAM },
		{ MTLVertexFormatUChar4Normalized, 0, ATTR_STREAM },
		{ MTLVertexFormatHalf2, 12, ATTR_STREAM },
	};

	id<MTLBuffer> wide_stream = [dev newBufferWithLength:ATTR_N * wide.stride options:0];
	id<MTLBuffer> narrow_stream = [dev newBufferWithLength:ATTR_N * narrow.stride options:0];
	id<MTLBuffer> bgra_stream = [dev newBufferWithLength:ATTR_N * narrow.stride options:0];
	id<MTLBuffer> instance_stream =
	    [dev newBufferWithLength:sizeof(attrib_instance_uv) options:0];
	id<MTLBuffer> indices = [dev newBufferWithLength:sizeof(index_list) options:0];

	if (wide_stream == nil || narrow_stream == nil || bgra_stream == nil ||
	    instance_stream == nil || indices == nil) {
		printf("drvtest: attrib: a vertex stream buffer could not be made\n");
		return;
	}
	struct attrib_layout bgra = narrow;

	bgra.what = "3. the coordinate as UChar4Normalized_BGRA: the SWAP";
	bgra.uv.format = MTLVertexFormatUChar4Normalized_BGRA;
	attrib_fill([wide_stream contents], [wide_stream length], &wide);
	attrib_fill([narrow_stream contents], [narrow_stream length], &narrow);
	attrib_fill([bgra_stream contents], [bgra_stream length], &bgra);
	memcpy([instance_stream contents], attrib_instance_uv, sizeof(attrib_instance_uv));
	memcpy([indices contents], index_list, sizeof(index_list));
	printf("drvtest: attrib: the stream is 8 elements: four corners of the quad with the "
	       "coordinate running the whole [0,1] square, then four at half the distance with "
	       "the coordinate (1,1). The module's UNUSED attribute 1 is configured too and "
	       "points at (%g, %g), the centre of texel (0,0)\n", (double)attrib_decoy_uv[0],
	       (double)attrib_decoy_uv[1]);

	/* 1. The wide layout, which is the reference every step below is compared with. */
	MTLVertexDescriptor *vd = attrib_descriptor(&wide, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> wide_ps = attrib_pipeline(dev, vertex, fragment, vd,
							     wide.what, false);

	[vd release];
	if (wide_ps == nil) {
		return;
	}
	/* What the compiler library built, read off the driver's own pipeline object rather
	 * than assumed: the elements are in the shader's driver_location order, so for
	 * std_vert0_lpf (attributes 0, 1 unused, 3) there must be TWO of them and they must be
	 * attribute 0 and attribute 3. A module of another shape is said so rather than
	 * measured against the wrong expectation. */
	unsigned elements = 0, mask = 0, slots = 0;
	SEL sel_count = sel_getUid("vertexElementCount");
	SEL sel_attr = sel_getUid("vertexElementAttribute:");
	SEL sel_mask = sel_getUid("vertexStreamMask");
	SEL sel_slots = sel_getUid("vertexStreamCount");

	if ([wide_ps respondsToSelector:sel_count] && [wide_ps respondsToSelector:sel_attr]) {
		elements = ((unsigned (*)(id, SEL))objc_msgSend)(wide_ps, sel_count);
		mask = ((unsigned (*)(id, SEL))objc_msgSend)(wide_ps, sel_mask);
		slots = ((unsigned (*)(id, SEL))objc_msgSend)(wide_ps, sel_slots);
		printf("drvtest: attrib: the pipeline holds %u vertex element%s, %u stream "
		       "slot%s, mask 0x%x; elements are", elements, elements == 1 ? "" : "s",
		       slots, slots == 1 ? "" : "s", mask);
		for (unsigned i = 0; i < elements; i++) {
			printf(" [%u]=attribute(%u)", i,
			       ((unsigned (*)(id, SEL, unsigned))objc_msgSend)(wide_ps, sel_attr,
									       i));
		}
		printf("\n");
	} else {
		printf("drvtest: attrib: this pipeline does not report its elements, so the "
		       "element ORDER is measured only by the decoy below\n");
	}

	unsigned g_wide[16][4], g_narrow[16][4], g_bgra[16][4], g_stride[16][4];
	unsigned g_other[16][4], g_idx[16][4], g_half[16][4], g_inst[16][4], g_rate[16][4];
	struct attrib_verdict v;
	bool ok = true;

	b.stream[ATTR_STREAM] = wide_stream;
	if (!attrib_draw(&b, wide_ps, 1, nil, 0, 0, g_wide, wide.what)) {
		return;
	}
	attrib_judge(g_wide, &v);
	attrib_print(wide.what, g_wide, &v);
	if (!attrib_is_grid(&v)) {
		printf("drvtest: attrib:   NOT the picture the layout describes%s\n",
		       v.decoy_texel == 16
			   ? ": every pixel holds texel (0,0), which is where the module's "
			     "UNUSED attribute 1 points -- the elements were not built in "
			     "driver_location order"
			   : "");
		ok = false;
	} else {
		printf("drvtest: attrib:   the coordinate of [[attribute(3)]] reached every "
		       "pixel and the quad of [[attribute(0)]] covered all 16, ON THE ADRENO: "
		       "the offsets, the stride and buffer index 5 are READ, and element 1 is "
		       "attribute 3 and not the unused attribute 1 (%s orientation, 10.32)\n",
		       v.mirrored == 16 ? "mirrored" : "upright");
	}

	/* 2. The same numbers in other formats at another stride and other offsets: equal to
	 *    the bit, or the formats are not what decoded the bytes. 0 and 255 read as a float
	 *    are 0.0 and 3.57e-43, and a Short4Normalized position read as Float4 is four
	 *    denormals -- a quad collapsed into the origin, covering nothing. */
	vd = attrib_descriptor(&narrow, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> narrow_ps = attrib_pipeline(dev, vertex, fragment, vd,
							       narrow.what, false);
	[vd release];
	if (narrow_ps == nil) {
		return;
	}
	b.stream[ATTR_STREAM] = narrow_stream;
	if (!attrib_draw(&b, narrow_ps, 1, nil, 0, 0, g_narrow, narrow.what)) {
		return;
	}
	attrib_judge(g_narrow, &v);
	attrib_print(narrow.what, g_narrow, &v);
	{
		unsigned d = index_diff(g_wide, g_narrow);

		printf("drvtest: attrib:   %u of 64 words differ from step 1: %s\n", d,
		       d == 0 ? "two encodings, two strides, three different offsets, ONE "
				"PICTURE -- the VFD decoded the bytes"
			      : "the same numbers in another format came back as another picture");
		ok = ok && d == 0;
	}

	/* 3. THE SWAP, which is the first of the three things softpipe could not answer. */
	vd = attrib_descriptor(&bgra, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> bgra_ps = attrib_pipeline(dev, vertex, fragment, vd,
							     bgra.what, false);
	[vd release];
	if (bgra_ps != nil) {
		b.stream[ATTR_STREAM] = bgra_stream;
		if (attrib_draw(&b, bgra_ps, 1, nil, 0, 0, g_bgra, bgra.what)) {
			unsigned d = index_diff(g_narrow, g_bgra);

			attrib_judge(g_bgra, &v);
			attrib_print(bgra.what, g_bgra, &v);
			printf("drvtest: attrib:   %u of 64 words differ from step 2, and the "
			       "coordinate's own byte 0 is ZERO: %s\n", d,
			       d == 0 ? "the BGRA order IS programmed -- VFD_DECODE_INSTR.swap "
					"delivers bytes 2 and 1 as x and y"
				      : v.column0 == 16
					    ? "every pixel holds column 0, which is what reading "
					      "the bytes in memory order gives: the swap was NOT "
					      "programmed"
					    : "neither picture, which is a third answer and wants "
					      "reading");
			ok = ok && d == 0;
		} else {
			ok = false;
		}
		[bgra_ps release];
	} else {
		ok = false;
	}

	/* 4. The stride, with a picture nothing else here produces: the narrow layout read at
	 *    32 bytes instead of 16 over a buffer of eight elements, so the fetch walks
	 *    elements 0, 2, 4 and 6 -- two corners of the full quad and two of the half-size
	 *    one. */
	struct attrib_layout stride2 = narrow;

	stride2.what = "4. the narrow layout at twice its stride: the fetch walks 0, 2, 4, 6";
	stride2.stride = 32;
	vd = attrib_descriptor(&stride2, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> stride_ps = attrib_pipeline(dev, vertex, fragment, vd,
							       stride2.what, false);
	[vd release];
	if (stride_ps != nil) {
		b.stream[ATTR_STREAM] = narrow_stream;
		if (attrib_draw(&b, stride_ps, 1, nil, 0, 0, g_stride, stride2.what)) {
			unsigned d = index_diff(g_narrow, g_stride);

			attrib_judge(g_stride, &v);
			attrib_print(stride2.what, g_stride, &v);
			printf("drvtest: attrib:   %u of 64 words differ from step 2: %s\n", d,
			       d ? "the stride decides which element a vertex reads"
				 : "A STRIDE OF 32 DREW WHAT A STRIDE OF 16 DREW, so the stride "
				   "is not read");
			ok = ok && d != 0;
		} else {
			ok = false;
		}
		[stride_ps release];
	} else {
		ok = false;
	}

	/* 5. The buffer index from the other side: the same layout fetched from index 7, with
	 *    the stream bound at 7 and nowhere else. A driver that bound every stream at slot 0
	 *    would fetch from an empty slot here and draw nothing. */
	struct attrib_layout other = narrow;

	other.what = "5. the narrow layout fetched from buffer index 7";
	other.pos.buffer = other.uv.buffer = other.decoy.buffer = ATTR_OTHER;
	vd = attrib_descriptor(&other, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> other_ps = attrib_pipeline(dev, vertex, fragment, vd,
							      other.what, false);
	[vd release];
	if (other_ps != nil) {
		b.stream[ATTR_STREAM] = nil;
		b.stream[ATTR_OTHER] = narrow_stream;
		if (attrib_draw(&b, other_ps, 1, nil, 0, 0, g_other, other.what)) {
			unsigned d = index_diff(g_narrow, g_other);

			printf("drvtest: attrib:   %s: %u of 64 words differ from step 2, %s\n",
			       other.what, d,
			       d == 0 ? "so Metal's buffer index is carried all the way down to "
					"VFD_FETCH[k]"
				      : "so the index moved the picture, which it must not");
			ok = ok && d == 0;
		} else {
			ok = false;
		}
		[other_ps release];
		b.stream[ATTR_OTHER] = nil;
		b.stream[ATTR_STREAM] = narrow_stream;
	} else {
		ok = false;
	}

	/* 6 and 7. The indexed forms over a real layout, which is what every draw in a real
	 *          application is -- and here an index is the ELEMENT THE FETCH READS, where in
	 *          10.35 it was the [[vertex_id]] a shader indexed its own buffer with. Four
	 *          indices must give step 2's picture; the last three must give one triangle of
	 *          it. */
	if (attrib_draw(&b, narrow_ps, 1, indices, 4, 0, g_idx,
			"6. -drawIndexedPrimitives: 4 indices [0 1 2 3] over the narrow layout")) {
		unsigned d = index_diff(g_narrow, g_idx);

		printf("drvtest: attrib:   6. indices [0 1 2 3]: %u of 64 words differ from step "
		       "2, %s\n", d,
		       d == 0 ? "so the index buffer selects the element the fetch reads"
			      : "so an index fed the fetch the wrong element");
		ok = ok && d == 0;
	} else {
		ok = false;
	}
	if (attrib_draw(&b, narrow_ps, 1, indices, 3, 4 * 2, g_half,
			"7. the same buffer's last three indices [1 2 3], UInt16 at byte 8")) {
		attrib_judge(g_half, &v);
		attrib_print("7. indices [1 2 3] at byte 8", g_half, &v);
		printf("drvtest: attrib:   %u of 64 words differ from step 6, covering %u of 16: "
		       "%s\n", index_diff(g_idx, g_half), v.covered,
		       v.covered > 0 && v.covered < 16
			   ? "one triangle of the quad out of three of its four elements"
			   : "NOT one triangle, which is what an offset that was not applied or "
			     "an index that was not read would give");
		ok = ok && v.covered > 0 && v.covered < 16;
	} else {
		ok = false;
	}

	/*
	 * 8 and 9. THE INSTANCED BIT and THE STEP RATE, the second thing softpipe could not
	 *          answer about the hardware. The coordinate comes out of a second stream that
	 *          steps per instance; two instances of this geometry cover the same pixels, so
	 *          the second is drawn over the first and every pixel must hold ONE texel --
	 *          instance 1's element, (0.875, 0.875), which is texel (3, 3) = 103 203 306
	 *          400. A stream that stepped per VERTEX would hand the four coordinates to the
	 *          four vertices of one instance and the picture would be a gradient. Then the
	 *          same at a step rate of 2, where both instances share element 0 and the
	 *          picture must be texel (0, 0) = 100 200 300 400 instead -- a different number
	 *          and not a different-looking picture.
	 */
	struct attrib_layout split = narrow;

	split.what = "8. the coordinate from a second stream stepping PER INSTANCE, 2 instances";
	split.uv.format = MTLVertexFormatFloat2;
	split.uv.offset = 0;
	split.uv.buffer = ATTR_INSTANCE;
	for (unsigned step = 0; step < 2; step++) {
		NSUInteger rate = step == 0 ? 1 : 2;
		const char *what = step == 0 ? split.what
		    : "9. the same at a step rate of 2: both instances share element 0";
		unsigned (*grid)[4] = step == 0 ? g_inst : g_rate;
		unsigned want_x = step == 0 ? 3 : 0, want_y = step == 0 ? 3 : 0;

		vd = attrib_descriptor(&split, MTLVertexStepFunctionPerInstance, rate,
				       ATTR_INSTANCE);
		id<MTLRenderPipelineState> ps = attrib_pipeline(dev, vertex, fragment, vd, what,
								false);
		[vd release];
		if (ps == nil) {
			ok = false;
			continue;
		}
		b.stream[ATTR_INSTANCE] = instance_stream;
		if (attrib_draw(&b, ps, 2, nil, 0, 0, grid, what)) {
			unsigned w0 = 100 + want_x, w1 = 200 + want_y;

			attrib_judge(grid, &v);
			attrib_print(what, grid, &v);
			bool flat = v.uniform == 16 && grid[0][0] == w0 && grid[0][1] == w1;

			printf("drvtest: attrib:   expecting %u %u %u %u in all 16 pixels (texel "
			       "(%u,%u)): %s\n", w0, w1, 300 + want_x + want_y, 400, want_x,
			       want_y,
			       flat ? (step == 0 ? "the stream stepped PER INSTANCE on the "
						   "hardware"
						 : "the step rate IS the instance divisor")
				    : v.uniform != 16
					  ? "the picture is a gradient, so that stream stepped "
					    "per VERTEX"
					  : "flat, but not that texel");
			ok = ok && flat;
		} else {
			ok = false;
		}
		[ps release];
		b.stream[ATTR_INSTANCE] = nil;
	}

	/*
	 * 10. THE PLACEHOLDER for an attribute gated by a function constant, which is the third
	 *     thing this machine could not answer: on softpipe the pipeline for such a module
	 *     dies in nir_to_tgsi before any draw. QuartzCore's fixed_vert_*_spc has five
	 *     attributes -- listed 0, 4, 1, 2, 3, so attribute 4 is at driver_location 1 -- and
	 *     three of them (point_size, texcoord0, texcoord1) are gated. This configures the
	 *     two that are NOT gated and leaves the three alone, which is what a pipeline whose
	 *     constants make the predicate false legally does.
	 *
	 *     What is claimed about it is exactly two things, and no more: the pipeline BUILDS,
	 *     and the draw COMES BACK. The picture is printed and nothing is concluded from it
	 *     -- no function constants are supplied here, so what the shader reads when the
	 *     predicate is false is the pipeline's business and not this test's. The claim that
	 *     matters is the second one: an unconfigured attribute with no element at all would
	 *     leave an empty VFD slot, and an empty slot fetches from address 0 (10.18).
	 */
	if (attrib_gated_function != NULL && *attrib_gated_function != '\0') {
		id<MTLFunction> gated = [vlib newFunctionWithName:
		    [NSString stringWithUTF8String:attrib_gated_function]];

		if (gated == nil) {
			printf("drvtest: attrib: 10. no %s in %s, so the function-constant gate "
			       "was not measured\n", attrib_gated_function, attrib_vertex_lib);
		} else {
			struct attrib_layout gate = narrow;

			gate.what = "10. a module whose attributes are gated by function "
				    "constants, with only the two ungated ones configured";
			gate.decoy.format = MTLVertexFormatInvalid;   /* attribute 1 is gated */
			vd = attrib_descriptor(&gate, MTLVertexStepFunctionPerVertex, 1, -1);
			id<MTLRenderPipelineState> ps = attrib_pipeline(dev, gated, fragment, vd,
									gate.what, false);
			[vd release];
			if (ps == nil) {
				printf("drvtest: attrib:   and that refusal is the measurement "
				       "failing: a gated attribute needs no configuration, so "
				       "this pipeline had to build\n");
				ok = false;
			} else {
				unsigned g_gate[16][4];

				b.stream[ATTR_STREAM] = narrow_stream;
				if (attrib_draw(&b, ps, 1, nil, 0, 0, g_gate, gate.what)) {
					attrib_judge(g_gate, &v);
					attrib_print(gate.what, g_gate, &v);
					printf("drvtest: attrib:   the pipeline built and the GPU "
					       "CAME BACK, so the placeholder element for a gated "
					       "attribute fetches memory this library owns and not "
					       "an empty VFD slot. What the pixels hold is NOT a "
					       "claim: no function constants were supplied, so the "
					       "predicate's value -- and what the shader reads "
					       "under it -- is the pipeline's and not this test's\n");
				} else {
					printf("drvtest: attrib:   the pipeline built and the draw "
					       "did NOT come back, which is what an empty VFD slot "
					       "looks like\n");
					ok = false;
				}
				[ps release];
			}
			[gated release];
		}
	}

	/*
	 * And three layouts that must be REFUSED on the device, by name, so the sentences are
	 * read in the log of the machine they are about rather than only on the host: the
	 * twelve-bit offset field, a packed format whose channel order against Gallium's is
	 * unmeasured, and the one buffer table Metal has behind -setVertexBuffer:atIndex:.
	 */
	printf("drvtest: attrib: and the layouts that must be refused, each by name:\n");
	unsigned refused = 0;
	for (unsigned i = 0; i < 3; i++) {
		struct attrib_layout bad = narrow;
		const char *what = i == 0 ? "an attribute at offset 4096"
		    : i == 1 ? "MTLVertexFormatUInt1010102Normalized, a packed word"
			     : "a stream at the buffer index the module reads mvp_matrix from";

		bad.what = what;
		if (i == 0) {
			bad.pos.offset = 4096;
		} else if (i == 1) {
			bad.pos.format = MTLVertexFormatUInt1010102Normalized;
		} else {
			bad.pos.buffer = bad.uv.buffer = bad.decoy.buffer = 2;
		}
		vd = attrib_descriptor(&bad, MTLVertexStepFunctionPerVertex, 1, -1);
		id<MTLRenderPipelineState> ps = attrib_pipeline(dev, vertex, fragment, vd, what,
								true);
		[vd release];
		if (ps != nil) {
			printf("drvtest: attrib:   %s: BUILT, and it should not have\n", what);
			[ps release];
			ok = false;
		} else {
			refused++;
		}
	}
	printf("drvtest: attrib: %u of 3 refused by name\n", refused);
	ok = ok && refused == 3;

	printf("drvtest: attrib: %s\n",
	       ok ? "THE VERTEX ATTRIBUTE LAYOUT IS READ ON THE ADRENO -- the offsets, the "
		    "stride, the formats, the BGRA swap, the buffer index, the index buffer, the "
		    "instanced bit and its step rate each changed what came back, and the element "
		    "order is the shader's driver_location order and not Metal's attribute order"
		  : "SOMETHING ABOVE DISAGREES, and a layout that is nearly right draws a picture "
		    "of the wrong vertices");

	[narrow_ps release];
	[wide_ps release];
	[indices release];
	[instance_stream release];
	[bgra_stream release];
	[narrow_stream release];
	[wide_stream release];
	for (unsigned i = 2; i <= 4; i++) {
		[b.argument[i] release];
	}
	[samp release];
	[tex release];
	for (unsigned i = 0; i < 4; i++) {
		[targets[i] release];
	}
	[rp release];
	fflush(stdout);
}
