/* Compute, texture setup, and indexed draw tests, included into metal_drv_test.m. */


static void
compute_path(id<MTLDevice> dev, id<MTLLibrary> library, const char *only)
{
	NSArray<NSString *> *names = only != NULL ?
	    @[ [NSString stringWithUTF8String:only] ] : [library functionNames];
	unsigned compiled = 0, refused = 0;
	id<MTLComputePipelineState> first = nil;
	NSString *firstName = nil;

	for (NSString *name in names) {
		id<MTLFunction> fn = [library newFunctionWithName:name];
		NSError *err = nil;
		id<MTLComputePipelineState> ps = fn != nil ?
		    [dev newComputePipelineStateWithFunction:fn error:&err] : nil;

		if (ps == nil) {
			refused++;
			printf("drvtest: compute %-32s refused: %s\n", [name UTF8String],
			       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
			continue;
		}
		compiled++;
		printf("drvtest: compute %-32s compiled: %lu threads per group, width %lu, "
		       "%lu bytes of threadgroup memory\n", [name UTF8String],
		       (unsigned long)[ps maxTotalThreadsPerThreadgroup],
		       (unsigned long)[ps threadExecutionWidth],
		       (unsigned long)[ps staticThreadgroupMemoryLength]);
		if (first == nil) {
			first = [ps retain];
			firstName = [name retain];
		}
	}
	printf("drvtest: of %lu functions, %u compiled and %u were refused\n",
	       (unsigned long)[names count], compiled, refused);
	if (first == nil) {
		return;
	}

	/* and one real dispatch, through the queue and the command buffer */
	id<MTLCommandQueue> queue = [dev newCommandQueue];

	printf("drvtest: newCommandQueue gave %s\n",
	       queue != nil ? class_getName(object_getClass(queue)) : "nil");
	if (queue == nil) {
		return;
	}
	/* Eight buffers, all bound. How many a kernel reads is its own business -- the corpus's
	 * compute kernels take between one and five -- and binding fewer than it declares would
	 * make it read a slot nothing was bound to. Metal's own rule is that a shader sees only
	 * what it declared, so binding more is harmless. */
	enum { RW_TEST_BUFFERS = 16 };
	id<MTLBuffer> bufs[RW_TEST_BUFFERS];

	for (int i = 0; i < RW_TEST_BUFFERS; i++) {
		bufs[i] = [dev newBufferWithLength:4096 options:0];
		if (bufs[i] == nil) {
			printf("drvtest: buffer %d could not be made\n", i);
			return;
		}
		if ([bufs[i] contents] != NULL) {
			memset([bufs[i] contents], 0, 4096);
		}
	}
	/* Textures and a sampler, for the kernels that declare them. Eight of each, the same
	 * reasoning as the buffers: a shader sees only what it declared, and binding fewer than
	 * it declares makes it read a slot nothing was bound to. 64x64 RGBA8 is a shape the
	 * corpus's kernels work in; a format the compiler library cannot translate is refused by
	 * name, which is the answer this test wants either way. */
	id<MTLTexture> texs[RW_TEST_BUFFERS] = { nil };
	id<MTLSamplerState> sampler = nil;
	int textures_made = 0;

	/*
	 * Which format to ask for is not a free choice, and the first run of this taught it:
	 * asking for RGBA8Unorm that the shader writes was refused by name -- "Adreno 7c+ Gen 3
	 * cannot give MTLPixelFormatRGBA8Unorm as a MTLTextureType2D the shader writes" -- and
	 * with no texture made every texture-reading kernel then failed with "reads texture 0
	 * and 0 textures were bound", which is a true sentence about the harness and says
	 * nothing about the kernel.
	 *
	 * So the candidates are tried in order and the first that the hardware will give as a
	 * writable image wins. A kernel whose declaration wants another element type is still
	 * refused, and that refusal names what it wanted -- which is the measurement, because
	 * this test is in the position of an application that did not write the shader.
	 */
	const MTLPixelFormat candidates[] = {
		texture_format, MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA32Uint,
		MTLPixelFormatRGBA16Float, MTLPixelFormatR32Float, MTLPixelFormatRGBA8Unorm,
	};
	MTLTextureDescriptor *td = nil;

	/* Read and write first, then read alone. The first run of this found that NOTHING is
	 * given as a texture a shader writes -- all five candidates refused -- and with no
	 * texture made every reading kernel failed with "reads texture 0 and 0 textures were
	 * bound", which is a sentence about this harness and not about any kernel. Falling back
	 * to read-only measures the eight that only read; the one that writes is then refused
	 * by the library, which is the honest state of things until that question is settled. */
	static const MTLTextureUsage usages[] = {
		MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite,
		MTLTextureUsageShaderRead,
	};

	for (size_t u = 0; u < sizeof(usages) / sizeof(usages[0]) && td == nil; u++) {
		for (size_t f = 0; f < sizeof(candidates) / sizeof(candidates[0]) && td == nil;
		     f++) {
			MTLTextureDescriptor *try = texture_descriptor(candidates[f], usages[u]);
			id<MTLTexture> probe;

			probe = [dev newTextureWithDescriptor:try];
			if (probe != nil) {
				[probe release];
				td = try;
				printf("drvtest: textures are format %lu, usage %lu (the first "
				       "this hardware gives)\n", (unsigned long)candidates[f],
				       (unsigned long)usages[u]);
			}
		}
	}
	if (td == nil) {
		printf("drvtest: no candidate pixel format can be made at all here\n");
	}
	for (int i = 0; td != nil && i < RW_TEST_BUFFERS; i++) {
		MTLTextureDescriptor *own = td;

		/* A slot the shader named gets what it asked for; the rest get what the probe
		 * above found the hardware will give. */
		if (i < RW_TEX_SLOTS && slot_format[i] != 0) {
			NSUInteger keep = texture_kind;

			texture_kind = slot_kind[i];
			own = texture_descriptor(slot_format[i], [td usage]);
			texture_kind = keep;
		}
		texs[i] = [dev newTextureWithDescriptor:own];
		if (texs[i] == nil) {
			printf("drvtest: slot %d (format %lu) was refused\n", i,
			       (unsigned long)[own pixelFormat]);
			break;
		}
		textures_made++;
		/*
		 * The texels are air_run's own (`texel_value`, air_run.c:92):
		 *
		 *     r = 100 + x + 1000 * slot,  g = 200 + y,  b = 300 + x + y + z,  a = 400
		 *
		 * and they are copied rather than invented for two reasons. The thousand per slot
		 * is deliberate there -- it makes a shader that reads the wrong slot return a
		 * visibly wrong texel instead of a plausible one, which air_run's comment records
		 * as the third mistake of that shape it met. And using the same values is what
		 * lets a texture-reading kernel's result be compared between softpipe here and
		 * the Adreno at all: the same shader over different texels is not a comparison.
		 */
		static unsigned char row[64 * 16];
		MTLPixelFormat pf = [own pixelFormat];
		unsigned texel_bytes = texel_bytes_of(pf);
		NSUInteger stride = 64 * texel_bytes;

		for (NSUInteger slice = 0; slice < [texs[i] arrayLength] *
		     (texture_kind == MTLTextureTypeCube ? 6 : 1); slice++) {
			for (NSUInteger z = 0; z < [texs[i] depth]; z++) {
				for (unsigned y = 0; y < 64; y++) {
					for (unsigned x = 0; x < 64; x++) {
						float v[4];

						v[0] = 100.0f + x + 1000.0f * i;
						v[1] = 200.0f + y;
						v[2] = 300.0f + x + y + (float)z;
						v[3] = 400.0f;
						write_texel(row + x * texel_bytes, pf, v);
					}
					[texs[i] replaceRegion:MTLRegionMake3D(0, y, z, 64, 1, 1)
						   mipmapLevel:0
							 slice:slice
						     withBytes:row
						   bytesPerRow:stride
						 bytesPerImage:0];
				}
			}
		}
	}
	MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

	sampler = [dev newSamplerStateWithDescriptor:sd];
	[sd release];
	printf("drvtest: %d of %d textures made, sampler %s\n", textures_made, RW_TEST_BUFFERS,
	       sampler != nil ? class_getName(object_getClass(sampler)) : "nil");

	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

	printf("drvtest: commandBuffer %s, computeCommandEncoder %s\n",
	       cmd != nil ? class_getName(object_getClass(cmd)) : "nil",
	       enc != nil ? class_getName(object_getClass(enc)) : "nil");
	if (cmd == nil || enc == nil) {
		return;
	}
	[enc setComputePipelineState:first];
	for (int i = 0; i < RW_TEST_BUFFERS; i++) {
		[enc setBuffer:bufs[i] offset:0 atIndex:i];
		if (texs[i] != nil) {
			[enc setTexture:texs[i] atIndex:i];
		}
		if (sampler != nil) {
			[enc setSamplerState:sampler atIndex:i];
		}
	}
	/* The same grid air_run uses -- 4x4x1 threads in one threadgroup -- so that the words
	 * this prints can be compared with what softpipe computes for the same kernel on the
	 * same (zero) input. A different thread count is a different computation for any kernel
	 * that writes per-thread, and then "the values agree" would mean nothing. */
	[enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
	[enc endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];
	printf("drvtest: dispatched %s: status %lu%s\n", [firstName UTF8String],
	       (unsigned long)[cmd status],
	       [cmd error] != nil ? [[NSString stringWithFormat:@", error: %@",
				      [[cmd error] localizedDescription]] UTF8String] : "");
	int written = 0;

	for (int which = 0; which < RW_TEST_BUFFERS; which++) {
		const unsigned char *bytes = [bufs[which] contents];
		int nonzero = 0;

		if (bytes == NULL) {
			continue;
		}
		for (int i = 0; i < 4096; i++) {
			if (bytes[i] != 0) {
				nonzero++;
			}
		}
		written += nonzero;
		if (nonzero == 0) {
			continue;   /* only the buffers that changed are worth a line */
		}
		printf("drvtest: buffer %d: %d non-zero bytes of 4096, first words %08x %08x "
		       "%08x %08x\n", which, nonzero, ((const unsigned *)bytes)[0],
		       ((const unsigned *)bytes)[1], ((const unsigned *)bytes)[2],
		       ((const unsigned *)bytes)[3]);
	}
	if (written == 0) {
		printf("drvtest: nothing was written to either buffer, so nothing is proven: this "
		       "kernel writes its buffer unconditionally\n");
	}
}

/*
 * One triangle, with one of Apple's own fragment shaders.
 *
 * `clearFragment20` out of AppleMetalGLRenderer's library writes render target 2 (int4) and
 * reads a 48-byte constant buffer at index 14. It is one of the 255 fragment shaders air_run
 * has drawn and compared pixel by pixel (docs/air.md), so there is a reference to compare
 * against -- and these four words ARE that reference, computed by air_run on softpipe with
 * the same (i * 7 + 13) & 0xff fill this function uses:
 *
 *     air_run: the pixel at (0, 0) of render target 2 came back as:
 *       2458616957 2930221209 3401825461 3873429713
 *     air_run: drew 4x4 into r32g32b32a32_uint, 3 render targets
 *
 * They are written down here rather than computed, which is a real weakness: if the shader's
 * meaning ever changes the constant stays and the test lies. tools/air/metalcheck.py is being
 * written to take the comparison off hardcoded numbers; until it does, the provenance above is
 * the only thing making this honest.
 */
/* Apple's own vertex shader, or the library's generated stage. `clearVertex` reads four
 * float4 corners out of a constant buffer at index 14 and indexes them by air.vertex_id --
 * no vertex attributes, which is why it can be served at all: the contract carries no
 * attribute layout, and all three of the corpus's vertex shaders turn out to read a buffer
 * instead (docs/metal.md 10.24). The corners written below are the ones the generated stage
 * covers, so the two roads draw the same pixels and a difference is a shader difference. */
static const char *vertex_function;
static const char *fragment_function = "clearFragment20";
static bool draw_all;
/* --samples N: how many samples the draw's attachments carry, one by default. Four makes the
 * pass a multisample one whose result is read out of the resolve texture, which is the only
 * way to tell a driver that resolves from one that stores the multisample attachment and
 * leaves the texture the application means to read as it was. The picture is the same four
 * words either way, so the comparison is between two runs of this one mode. */
static unsigned draw_samples = 1;
/* --draw-depth, which is the draw with a depth attachment under it and so with the first
 * depth test this port has ever run (depth_path). */
static bool draw_depth;
/* --draw-index, which is the draw that goes through an index buffer -- what every drawing
 * call in a real application is, and what this driver declined until now. It runs AFTER the
 * ordinary draw and over the same pipeline and attachments (index_draws), because the
 * measurement is a comparison: the same three indices drawn from two places in one buffer
 * must put the triangle in two different halves of the attachment. */
static bool draw_index;
/* --draw-attrib: the vertex attribute layout through Metal's own MTLVertexDescriptor, on the
 * hardware. Its own flag and not an option of --draw because what it draws is a different
 * pipeline out of two libraries, and because its verdicts are compared with each other and
 * not with the one picture --draw makes. */
static bool draw_attrib;
/* --argbuf: the argument encoder and the buffer offset, on the device. */
static bool argbuf;
/*
 * Which slice of the library to draw. Not a convenience: the loader keeps the first 256 KB
 * of a boot's log and the last 1 MB, dropping the middle, and the boot's own noise fills
 * most of that first 256 KB -- so about seventy lines of sweep survive per boot. Two runs
 * stopped at exactly 64 shaders and the limit looked like the driver's until the line number
 * gave it away: 2572 of 11480. A range turns that into five honest boots instead of one
 * number that is quietly a quarter of the answer.
 */
static unsigned draw_from, draw_count = 60;
/* --seed-rows writes the draw's 4x4 texture one row at a time, at a non-zero region origin,
 * which is how this test used to do it always. It is a flag now because that made every
 * measurement of that texture depend on the driver's origin.y, and because four rows landing
 * in one place is the second explanation of the flip in docs/metal.md 10.26.
 */
/* Metal's own MTLCommandBufferStatus: NotEnqueued 0, Enqueued 1, Committed 2, Scheduled 3,
 * Completed 4, Error 5. This test used to compare against the literal 6, which is what the
 * driver answered until it was corrected to Metal's numbering -- and 6 is not a status at
 * all, so an application comparing with MTLCommandBufferStatusCompleted saw completion the
 * moment -commit returned, before the GPU fence was paid. The driver prints its numbering
 * once per process; a log without that line was written by the old one.
 */
#define RW_CB_COMPLETED 4

static bool seed_rows;
/* --no-texdecl makes the sweep bind its single texture set to every shader, as it did
 * before each shader was given what it declares. It exists because the first sweep with
 * per-shader textures stopped at its third shader, and a sweep that cannot finish measures
 * nothing: with this, the 256 shaders that declare no texture are measurable again while
 * the 15 that do are measured one process at a time (userland/rc.jobs.metaltex).
 */
static bool use_decl = true;

/* 4x4, and 4x4x4 for a 3D slot, because that is air_run's TEX_SIZE and the shape it
 * draws against (air_run.c: a 2D texture 4x4, a 3D one 4x4x4). A texture of another
 * size here would make the device's pixel and the host's reference two different
 * measurements with one name. */
enum { DRAW_TEX = 4 };

/* A descriptor for one fragment-stage texture of the kind a slot asks for. Separate from
 * texture_descriptor() above, which is the compute path's 64x64 and reads the one global
 * kind: here the kind is per slot and comes from the shader. */
static MTLTextureDescriptor *
draw_texture_descriptor(NSUInteger type, MTLPixelFormat format, MTLTextureUsage usage)
{
	MTLTextureDescriptor *td = [[[MTLTextureDescriptor alloc] init] autorelease];

	[td setPixelFormat:format];
	[td setTextureType:type];
	[td setWidth:DRAW_TEX];
	[td setHeight:type == MTLTextureType1D ? 1 : DRAW_TEX];
	[td setDepth:type == MTLTextureType3D ? DRAW_TEX : 1];
	[td setArrayLength:1];
	[td setMipmapLevelCount:1];
	[td setSampleCount:1];
	[td setUsage:usage];
	return td;
}

/*
 * air_run's texels into one texture, every slice and every depth of it:
 *
 *     r = 100 + x + 1000 * slot,  g = 200 + y,  b = 300 + x + y + z,  a = 400
 *
 * The thousand per slot is why a shader that reads the wrong slot comes back visibly
 * wrong instead of plausibly right, and using air_run's own numbers is what lets the
 * pixel this draw produces be compared with the one softpipe produces at all.
 */
static void
fill_draw_texture(id<MTLTexture> tex, unsigned slot)
{
	MTLPixelFormat pf = [tex pixelFormat];
	unsigned bytes = texel_bytes_of(pf);
	unsigned char row[DRAW_TEX * 16];
	NSUInteger slices = [tex textureType] == MTLTextureTypeCube ? 6 : [tex arrayLength];

	for (NSUInteger slice = 0; slice < slices; slice++) {
		for (NSUInteger z = 0; z < [tex depth]; z++) {
			for (NSUInteger y = 0; y < [tex height]; y++) {
				for (NSUInteger x = 0; x < [tex width]; x++) {
					float v[4] = { 100.0f + x + 1000.0f * slot,
						       200.0f + y, 300.0f + x + y + z, 400.0f };

					write_texel(row + x * bytes, pf, v);
				}
				[tex replaceRegion:MTLRegionMake3D(0, y, z, [tex width], 1, 1)
				       mipmapLevel:0
					     slice:slice
					 withBytes:row
				       bytesPerRow:[tex width] * bytes
				     bytesPerImage:0];
			}
		}
	}
}

/*
 * The pixel format to bind for a declared element type: the 32-bit RGBA format of the
 * declared class, always.
 *
 * The width is deliberately not followed. What a binding is refused for is the CLASS --
 * "reads texture 0 as a texture2d<uint> and MTLPixelFormatRGBA32Float is a
 * floating-point or normalised format" -- and the three 32-bit formats are the ones
 * air_run and air_metal_test bind, so a pixel measured here and a pixel computed there
 * are the same measurement. Every texel value in the pattern (100 to 1400) is exact in
 * half and fits in a ushort, so a shader declaring `half` or `ushort` reads the same
 * numbers out of a 32-bit texture as out of a 16-bit one. --texslots still asks for a
 * 16-bit format by name when the point is the format rather than the shader.
 */
static MTLPixelFormat
format_for_element(unsigned element)
{
	switch (element) {
	case RWA_ELEMENT_INT:
		return MTLPixelFormatRGBA32Sint;
	case RWA_ELEMENT_UINT:
		return MTLPixelFormatRGBA32Uint;
	case RWA_ELEMENT_FLOAT:
		return MTLPixelFormatRGBA32Float;
	default:
		/* Nothing states an element type, so the slot is bound what this sweep bound
		 * every slot before it could ask -- draw_path's own dtex[] format. Not
		 * texture_format, which is the COMPUTE path's default: a pixel that changed
		 * because a fallback moved would look like a shader that changed. */
		return MTLPixelFormatRGBA32Uint;
	}
}

static void
release_slots(id<MTLTexture> *tex, unsigned slots)
{
	for (unsigned i = 0; i < slots; i++) {
		[tex[i] release];
		tex[i] = nil;
	}
}

/*
 * The textures one fragment shader declares: made, filled with air_run's texels, and
 * left in out[slot] for the slots it named. False and a sentence when any of that
 * cannot be done, and then the sentence IS the measurement for that shader -- "it
 * declares texture 0 as texture2d_ms<float, read> and no MTLTextureType 4 ... could be
 * made" is a limit of the driver (it binds no multisample texture), where the old
 * "declares texture 0 as a texture2d_ms and the texture bound there is a 2d" was a
 * limit of this harness.
 *
 * Without the query (an older compiler library, or a boot with no GPU) it does nothing
 * and says so by leaving every slot nil: the caller's single --texslots binding then
 * stands, which is exactly the behaviour this sweep had before.
 */
static bool
textures_for_shader(id<MTLDevice> dev, id<MTLFunction> fn, id<MTLTexture> *out,
		    unsigned slots, unsigned *declared, char *why, size_t why_size)
{
	*declared = 0;
	if (rwa_module_textures == NULL) {
		return true;
	}
	NSData *bc = function_bitcode(fn);

	if (bc == nil) {
		snprintf(why, why_size, "it does not answer bitcodeData, so there is no "
			 "module to read a declaration out of");
		return false;
	}
	struct rwa_texture_decl decl[RW_TEX_SLOTS];
	unsigned n = 0;
	char reason[256] = "";

	if (rwa_module_textures(air_device, [bc bytes], [bc length], NULL, decl,
				RW_TEX_SLOTS, &n, reason, sizeof(reason)) != 0) {
		snprintf(why, why_size, "%s", reason);
		return false;
	}
	*declared = n;
	for (unsigned k = 0; k < n; k++) {
		unsigned slot = decl[k].index;

		if (slot >= slots) {
			snprintf(why, why_size, "it declares texture %u and this harness binds "
				 "%u slots", slot, slots);
			return false;
		}
		/* --texslots stays an override, deliberately: binding the wrong thing on
		 * purpose is how the driver's refusal gets tested, and that can only be on
		 * purpose if a person can still ask for it. */
		bool overridden = slot_format[slot] != 0;

		/* A slot the module describes in no way at all -- a computed binding with no
		 * instruction and no air.arg_type_name -- is left to the caller's own
		 * binding rather than replaced by a guess of the same shape. No shader of the
		 * corpus is in this case; making a texture for it anyway would change what
		 * such a shader is bound without anything having asked.
		 */
		if (!overridden && !decl[k].type_known &&
		    decl[k].element == RWA_ELEMENT_UNKNOWN) {
			continue;
		}
		MTLPixelFormat format = overridden ? slot_format[slot]
						   : format_for_element(decl[k].element);
		NSUInteger type = overridden ? slot_kind[slot]
			: (decl[k].type_known ? (NSUInteger)decl[k].type : texture_kind);
		MTLTextureDescriptor *td = draw_texture_descriptor(type, format,
		    (MTLTextureUsage)decl[k].usage);

		out[slot] = [dev newTextureWithDescriptor:td];
		if (out[slot] == nil) {
			/* The driver has already printed the compiler library's own sentence
			 * about what it would not make; this one says which slot of which
			 * shader asked, which the library cannot know. */
			snprintf(why, why_size, "it declares texture %u as %s and no "
				 "MTLTextureType %lu of MTLPixelFormat %lu could be made for "
				 "it", slot,
				 decl[k].metal_type[0] != '\0' ? decl[k].metal_type :
				 "a texture whose metadata carries no type name",
				 (unsigned long)type, (unsigned long)format);
			return false;
		}
		fill_draw_texture(out[slot], slot);
	}
	return true;
}

static void draw_every(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
		       MTLRenderPassDescriptor *rp, id<MTLBuffer> uniform, id<MTLTexture> *dtex,
		       id<MTLSamplerState> dsamp, id<MTLTexture> *targets);

/*
 * The indexed draws, run after the ordinary one and over its pipeline, attachments and
 * bindings -- so that the only thing that differs between the reference picture and these
 * is the index buffer. docs/metal.md 10.35 is the experiment and air_metal_test
 * --draw-index is its reference implementation on softpipe, step for step.
 *
 * The index buffer holds 0 1 2 3 1 2 3. Three indices out of it make one triangle of the
 * quad: 0,1,2 is the half of the attachment below the diagonal, whose right angle is the
 * pixel at (0, 0), and 1,2,3 is the other half, whose right angle is (3, 3). Those two
 * draws differ in NOTHING but indexBufferOffset, so a driver that ignored the index buffer
 * or the offset would draw the first triangle twice and (3, 3) would come back as the clear
 * colour both times. The four pixels on the diagonal have their centres exactly on the
 * shared edge and belong to whichever triangle the rasteriser's fill rule gives them, so
 * the covered count is printed and nothing is concluded from it -- the two right angles are
 * the verdict.
 */
static const uint16_t index_list[7] = { 0, 1, 2, 3, 1, 2, 3 };

/* Whether pixel (x, y) of a 4x4 readback was drawn at all. The clear colour is zero and
 * clearFragment20's first word is 2458616957, so a non-zero first word is a covered pixel
 * -- which the caller's own four words have already confirmed before this is used. */
static bool
index_at(const unsigned grid[16][4], unsigned x, unsigned y)
{
	return grid[y * 4 + x][0] != 0;
}

/* How many of the 64 words of two pictures differ: the whole attachment and not one pixel,
 * because two triangles that differ only along the diagonal differ in four pixels. */
static unsigned
index_diff(const unsigned a[16][4], const unsigned b[16][4])
{
	unsigned n = 0;

	for (unsigned i = 0; i < 16; i++) {
		for (unsigned c = 0; c < 4; c++) {
			n += a[i][c] != b[i][c];
		}
	}
	return n;
}

/* One picture, as four rows of four, with the two corners the verdicts are made of. */
static void
index_print(unsigned n, const char *what, const unsigned grid[16][4])
{
	unsigned covered = 0;

	printf("drvtest: index: %u. %s: ", n, what);
	for (unsigned y = 0; y < 4; y++) {
		for (unsigned x = 0; x < 4; x++) {
			printf("%c", index_at(grid, x, y) ? '#' : '.');
			covered += index_at(grid, x, y);
		}
		printf("%s", y < 3 ? "/" : "");
	}
	printf("  %u of 16 pixels, (0,0) %s, (3,3) %s\n", covered,
	       index_at(grid, 0, 0) ? "drawn" : "clear",
	       index_at(grid, 3, 3) ? "drawn" : "clear");
}

static void
index_draws(id<MTLDevice> dev, id<MTLCommandQueue> queue, MTLRenderPassDescriptor *rp,
	    id<MTLRenderPipelineState> pipeline, id<MTLBuffer> uniform,
	    id<MTLBuffer> corners, id<MTLBuffer> coords, id<MTLTexture> *dtex,
	    id<MTLSamplerState> dsamp, id<MTLTexture> *targets)
{
	/* Step 0 is the draw the caller has just made and whose pixels are still on the
	 * attachment: the non-indexed strip of four. Everything below is compared with it or
	 * with another step of it. */
	static const struct {
		const char *what;
		bool wide;                /* MTLIndexTypeUInt32 rather than UInt16 */
		NSUInteger count, offset;
		NSInteger baseVertex;
		bool must_fail;
	} step[] = {
		{ "the indexed strip of 4 [0 1 2 3]",             false, 4, 0,     0, false },
		{ "indices [0 1 2]: the half below the diagonal", false, 3, 0,     0, false },
		{ "indices [1 2 3]: the half above it",           false, 3, 4 * 2, 0, false },
		{ "indices [0 1 2] with baseVertex 1",            false, 3, 0,     1, false },
		{ "indices [0 1 2] as UInt32",                    true,  3, 0,     0, false },
		{ "indices [1 2 3] as UInt32",                    true,  3, 4 * 4, 0, false },
		{ "UInt16 at an odd offset, which must be refused",   false, 3, 1, 0, true },
		{ "8 indices of a buffer that holds 7, refused too",  false, 8, 0, 0, true },
	};
	enum { STEPS = sizeof(step) / sizeof(step[0]) };
	unsigned grid[STEPS + 1][16][4];
	uint32_t wide[7];
	unsigned drew = 0, refused = 0;

	memset(grid, 0, sizeof(grid));
	[targets[2] getBytes:grid[0]
		 bytesPerRow:4 * 16
		  fromRegion:MTLRegionMake2D(0, 0, 4, 4)
		 mipmapLevel:0];
	for (unsigned i = 0; i < 7; i++) {
		wide[i] = index_list[i];
	}
	id<MTLBuffer> ib16 = [dev newBufferWithBytes:index_list length:sizeof(index_list)
					     options:0];
	id<MTLBuffer> ib32 = [dev newBufferWithBytes:wide length:sizeof(wide) options:0];

	if (ib16 == nil || ib32 == nil) {
		printf("drvtest: index: no index buffer: %s\n",
		       ib16 == nil ? "newBufferWithBytes: gave nil" : "the UInt32 one is nil");
		return;
	}
	printf("drvtest: index: the index buffer is a %s of %lu bytes, holding "
	       "0 1 2 3 1 2 3\n", class_getName(object_getClass(ib16)),
	       (unsigned long)[ib16 length]);
	printf("drvtest: index: step 0 is the non-indexed strip of 4 already drawn above\n");
	index_print(0, "the non-indexed strip of 4", grid[0]);

	for (unsigned s = 0; s < STEPS; s++) {
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			printf("drvtest: index: %s: no encoder\n", step[s].what);
			return;
		}
		[enc setRenderPipelineState:pipeline];
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:uniform offset:0 atIndex:i];
			[enc setVertexBuffer:i == 1 ? coords : corners offset:0 atIndex:i];
			if (i < 4 && dtex[i] != nil) {
				[enc setFragmentTexture:dtex[i] atIndex:i];
			}
			if (dsamp != nil) {
				[enc setFragmentSamplerState:dsamp atIndex:i];
			}
		}
		/* The longest form for every step, because the shorter two ARE this one with
		 * instanceCount 1, baseVertex 0 and baseInstance 0 -- Metal's own header says
		 * as much -- and one call site is one thing to read. */
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
				indexCount:step[s].count
				 indexType:step[s].wide ? MTLIndexTypeUInt32
							: MTLIndexTypeUInt16
			       indexBuffer:step[s].wide ? ib32 : ib16
			 indexBufferOffset:step[s].offset
			     instanceCount:1
				baseVertex:step[s].baseVertex
			      baseInstance:0];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		if ([cmd status] != RW_CB_COMPLETED) {
			printf("drvtest: index: %u. %s: status %lu%s%s\n", s + 1, step[s].what,
			       (unsigned long)[cmd status], [cmd error] != nil ? " -- " : "",
			       [cmd error] != nil
				   ? [[[cmd error] localizedDescription] UTF8String] : "");
			refused += step[s].must_fail;
			if (!step[s].must_fail) {
				printf("drvtest: index:    and that step was meant to draw\n");
			}
			continue;
		}
		if (step[s].must_fail) {
			printf("drvtest: index: %u. %s: DREW, and it should not have\n", s + 1,
			       step[s].what);
			continue;
		}
		drew++;
		[targets[2] getBytes:grid[s + 1]
			 bytesPerRow:4 * 16
			  fromRegion:MTLRegionMake2D(0, 0, 4, 4)
			 mipmapLevel:0];
		index_print(s + 1, step[s].what, grid[s + 1]);
	}

	/*
	 * And the comparisons, which are the measurement. Each names what the other answer
	 * would have meant, so a wrong run does not need this file to be read to be
	 * understood.
	 */
	printf("drvtest: index: the indexed strip of 4 differs from the non-indexed one in "
	       "%u of 64 words -- the CONTROL, which a draw ignoring the index buffer passes "
	       "as well\n", index_diff(grid[0], grid[1]));
	printf("drvtest: index: the two halves differ in %u of 64 words, (0,0) is %s in the "
	       "first and %s in the second, (3,3) is %s and %s: %s\n",
	       index_diff(grid[2], grid[3]),
	       index_at(grid[2], 0, 0) ? "drawn" : "clear",
	       index_at(grid[3], 0, 0) ? "drawn" : "clear",
	       index_at(grid[2], 3, 3) ? "drawn" : "clear",
	       index_at(grid[3], 3, 3) ? "drawn" : "clear",
	       (index_at(grid[2], 0, 0) && !index_at(grid[2], 3, 3) &&
		index_at(grid[3], 3, 3) && !index_at(grid[3], 0, 0))
		   ? "THE INDEX BUFFER AND THE OFFSET ARE READ ON THE ADRENO"
		   : "a right angle came back on the wrong side, which is what an ignored "
		     "index buffer or offset gives: the first triangle twice");
	printf("drvtest: index: baseVertex 1 over [0 1 2] differs from [1 2 3] in %u of 64 "
	       "words and from [0 1 2] in %u: %s\n", index_diff(grid[4], grid[3]),
	       index_diff(grid[4], grid[2]),
	       index_diff(grid[4], grid[3]) == 0
		   ? "baseVertex is added to every index"
		   : index_diff(grid[4], grid[2]) == 0 ? "baseVertex was DROPPED"
						       : "neither picture, which wants reading");
	printf("drvtest: index: the UInt32 halves differ from the UInt16 ones in %u and %u of "
	       "64 words: %s\n", index_diff(grid[5], grid[2]), index_diff(grid[6], grid[3]),
	       index_diff(grid[5], grid[2]) == 0 && index_diff(grid[6], grid[3]) == 0
		   ? "the index type is honoured"
		   : "a 32-bit index read two bytes at a time is 0, 0, 1 -- a triangle of no "
		     "area, which covers nothing");
	printf("drvtest: index: %u of 6 drew and %u of 2 were refused by name\n", drew,
	       refused);
	fflush(stdout);
}
