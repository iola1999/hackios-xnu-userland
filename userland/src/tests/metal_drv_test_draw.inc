/* Render draw, argument buffer, and depth tests, included into metal_drv_test.m. */


static void
draw_path(id<MTLDevice> dev, const char *metallib)
{
	static const unsigned expected[4] = { 2458616957u, 2930221209u, 3401825461u, 3873429713u };
	NSError *err = nil;
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: draw: newLibraryWithData: gave nil\n");
		return;
	}
	const char *shader = fragment_function;
	id<MTLFunction> fragment = [library newFunctionWithName:
	    [NSString stringWithUTF8String:shader]];

	if (fragment == nil) {
		printf("drvtest: draw: no %s in that library\n", shader);
		return;
	}
	/* Named in the log on purpose: tools/air/metalcheck.py can then produce the host
	 * reference itself, rather than the comparison resting on the four words written into
	 * this file. */
	printf("drvtest: draw: %s %s\n", metallib, shader);

	/* What a shader declares, from the compiler library the driver is already using. The
	 * sweep below binds per shader with it; this single draw keeps its one binding, whose
	 * reference is hand-checked. */
	load_texture_query(dev);

	/* Three attachments, because the shader writes target 2 and Metal numbers them from 0;
	 * the format is the one air_run drew into, so the comparison is of the same thing. */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:4
							     height:4
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[3] = { nil, nil, nil };
	/* With --samples above one the pass renders into these and resolves into `targets`,
	 * which stay the one-sample textures everything below reads back. Kept as a separate
	 * array so the readback code is the same at every sample count. */
	id<MTLTexture> msaa[3] = { nil, nil, nil };

	for (int i = 0; i < 3; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: draw: attachment %d could not be made\n", i);
			return;
		}
		if (draw_samples > 1) {
			MTLTextureDescriptor *md = [[td copy] autorelease];

			[md setTextureType:MTLTextureType2DMultisample];
			[md setSampleCount:draw_samples];
			[md setUsage:MTLTextureUsageRenderTarget];
			msaa[i] = [dev newTextureWithDescriptor:md];
			if (msaa[i] == nil) {
				printf("drvtest: draw: the %u-sample attachment %d could not "
				       "be made\n", draw_samples, i);
				return;
			}
		}
	}
	if (draw_samples > 1) {
		printf("drvtest: draw: %u samples, device answers -supportsSampleCount:%u "
		       "with %s\n", draw_samples, draw_samples,
		       [dev supportsSampleCount:draw_samples] ? "yes" : "no");
	}

	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

	[pd setFragmentFunction:fragment];
	id<MTLFunction> vertex = nil;

	if (vertex_function != NULL) {
		vertex = [library newFunctionWithName:
		    [NSString stringWithUTF8String:vertex_function]];
		if (vertex == nil) {
			printf("drvtest: draw: no %s in that library\n", vertex_function);
			return;
		}
		printf("drvtest: draw: vertex stage is Apple's own %s\n", vertex_function);
	}
	[pd setVertexFunction:vertex];   /* nil: the library generates the stage, as air_run does */
	for (int i = 0; i < 3; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatRGBA32Uint];
	}
	/* The pipeline's sample count has to agree with the attachments' -- Metal refuses a
	 * pipeline of one sample with a four-sample attachment, and so does the library. */
	[pd setSampleCount:draw_samples];
	err = nil;
	id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd
										  error:&err];

	[pd release];
	printf("drvtest: draw: newRenderPipelineStateWithDescriptor: gave %s%s%s\n",
	       pipeline != nil ? class_getName(object_getClass(pipeline)) : "nil",
	       err != nil ? ", error: " : "",
	       err != nil ? [[err localizedDescription] UTF8String] : "");
	if (pipeline == nil) {
		return;
	}

	/* The shader's constant buffer, filled the way air_run fills it, and bound at every
	 * index up to 16 because this one is declared at location 14 and a driver that bound
	 * only index 0 would leave it reading nothing. */
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];

	if (uniform != nil && [uniform contents] != NULL) {
		unsigned char *bytes = [uniform contents];

		for (int i = 0; i < 256; i++) {
			bytes[i] = (unsigned char)((i * 7 + 13) & 0xff);
		}
	}

	/*
	 * The vertex stage's own buffers. Two of them, because that is what the shaders declare,
	 * and the first version of this bound ONE buffer of corners at every index -- so
	 * texturedVertex2D, which reads positions from buffer 0 and texture coordinates from
	 * buffer 1, read positions as its coordinates and returned an honest answer to an input
	 * nobody meant (103 200 303 400 on the device, docs/metal.md 10.25). A harness that
	 * binds the same thing everywhere cannot be compared with anything.
	 *
	 * Positions go to index 0 and to index 14, because clearVertex declares its buffer at
	 * 14 and texturedVertex2D at 0; coordinates go to 1. The corners are the geometry the
	 * generated stage covers, so the two roads draw the same pixels.
	 */
	id<MTLBuffer> corners = [dev newBufferWithLength:256 options:0];
	id<MTLBuffer> coords = [dev newBufferWithLength:256 options:0];

	if (vertex_function != NULL && corners != nil && [corners contents] != NULL) {
		static const float quad[4][4] = {
			{ -1.0f, -1.0f, 0.0f, 1.0f },
			{  1.0f, -1.0f, 0.0f, 1.0f },
			{ -1.0f,  1.0f, 0.0f, 1.0f },
			{  1.0f,  1.0f, 0.0f, 1.0f },
		};

		memcpy([corners contents], quad, sizeof(quad));
	}
	if (vertex_function != NULL && coords != nil && [coords contents] != NULL) {
		/* the same corners in texture space, so the far corner of the picture reads the
		 * far texel -- which is the half of the comparison that a dropped varying cannot
		 * fake */
		static const float uv[4][2] = {
			{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f },
		};

		memcpy([coords contents], uv, sizeof(uv));
	}

	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

	for (int i = 0; i < 3; i++) {
		MTLRenderPassColorAttachmentDescriptor *a =
		    [[rp colorAttachments] objectAtIndexedSubscript:i];

		[a setTexture:targets[i]];
		[a setLoadAction:MTLLoadActionClear];
		[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
		[a setStoreAction:MTLStoreActionStore];
		/* Above one sample the attachment is the multisample texture and `targets[i]` is
		 * its resolve texture, which is where the picture is read from below. This is the
		 * shape CoreAnimation asks for: storeAction 2 with a resolveTexture. */
		if (draw_samples > 1) {
			[a setTexture:msaa[i]];
			[a setResolveTexture:targets[i]];
			[a setStoreAction:MTLStoreActionMultisampleResolve];
		}
	}

	/* Textures for the fragment stage, when the shader reads any. The first attempt at
	 * drawing with Apple's textured vertex shader never reached the draw at all: it was
	 * refused with "reads texture 0 and 0 textures were bound", which is a sentence about
	 * this harness. The texels are air_run's own, so the picture can be compared. */
	id<MTLTexture> dtex[4] = { nil, nil, nil, nil };
	id<MTLSamplerState> dsamp = nil;
	MTLTextureDescriptor *dtd = texture_descriptor(
	    slot_format[0] != 0 ? slot_format[0] : MTLPixelFormatRGBA32Uint,
	    MTLTextureUsageShaderRead);

	[dtd setWidth:4];
	[dtd setHeight:4];
	for (int i = 0; i < 4; i++) {
		dtex[i] = [dev newTextureWithDescriptor:dtd];
		if (dtex[i] == nil) {
			break;
		}
		unsigned stride_bytes = texel_bytes_of([dtd pixelFormat]);
		unsigned char all[4 * 4 * 16];

		for (unsigned y = 0; y < 4; y++) {
			for (unsigned x = 0; x < 4; x++) {
				float v[4] = { 100.0f + x + 1000.0f * i, 200.0f + y,
					       300.0f + x + y, 400.0f };

				write_texel(all + (y * 4 + x) * stride_bytes,
					    [dtd pixelFormat], v);
			}
		}
		/* One call for the whole picture, and not one per row.
		 *
		 * Per row it was four calls with a non-zero region origin, which made every
		 * measurement of this texture depend on the driver getting origin.y right --
		 * a thing this test is not about. It is also the second explanation of the
		 * vertical flip in docs/metal.md 10.26, which one pixel could not separate
		 * from the first: four rows landing in one place would leave every texel
		 * carrying the last row's y = 203, and pixel (0, 0) would then read
		 * 100 203 303 400 with no flip anywhere.
		 */
		if (seed_rows) {
			for (unsigned y = 0; y < 4; y++) {
				[dtex[i] replaceRegion:MTLRegionMake2D(0, y, 4, 1)
					  mipmapLevel:0
					    withBytes:all + y * 4 * stride_bytes
					  bytesPerRow:4 * stride_bytes];
			}
		} else {
			[dtex[i] replaceRegion:MTLRegionMake2D(0, 0, 4, 4)
				  mipmapLevel:0
				    withBytes:all
				  bytesPerRow:4 * stride_bytes];
		}
		/* and read it back, because a draw that samples a texture nobody has looked
		 * at cannot tell a wrong sample from a wrong seed
		 */
		if (i == 0) {
			unsigned char back[4 * 4 * 16];

			memset(back, 0, sizeof(back));
			[dtex[i] getBytes:back
			      bytesPerRow:4 * stride_bytes
			       fromRegion:MTLRegionMake2D(0, 0, 4, 4)
			      mipmapLevel:0];
			for (unsigned y = 0; y < 4; y++) {
				printf("drvtest: draw: seeded row %u reads back", y);
				for (unsigned x = 0; x < 4; x++) {
					float v[4] = { 0, 0, 0, 0 };

					read_texel(back + (y * 4 + x) * stride_bytes,
						   [dtd pixelFormat], v);
					printf(" %g/%g", v[0], v[1]);
				}
				printf("   (x/y of each texel; row %u must read y = %u)\n",
				       y, 200 + y);
			}
		}
	}
	MTLSamplerDescriptor *dsd = [[MTLSamplerDescriptor alloc] init];

	dsamp = [dev newSamplerStateWithDescriptor:dsd];
	[dsd release];
	printf("drvtest: draw: %d textures of format %lu for the fragment stage\n",
	       dtex[0] != nil ? (dtex[3] != nil ? 4 : 1) : 0, (unsigned long)[dtd pixelFormat]);

	id<MTLCommandQueue> queue = [dev newCommandQueue];
	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	printf("drvtest: draw: renderCommandEncoderWithDescriptor: gave %s\n",
	       enc != nil ? class_getName(object_getClass(enc)) : "nil");
	if (enc == nil) {
		return;
	}
	[enc setRenderPipelineState:pipeline];
	for (int i = 0; i < 16; i++) {
		[enc setFragmentBuffer:uniform offset:0 atIndex:i];
		/* what each index holds is what some shader declares there: 0 and 14 are
		 * positions, 1 is texture coordinates */
		[enc setVertexBuffer:i == 1 ? coords : corners offset:0 atIndex:i];
		if (i < 4 && dtex[i] != nil) {
			[enc setFragmentTexture:dtex[i] atIndex:i];
		}
		if (dsamp != nil) {
			[enc setFragmentSamplerState:dsamp atIndex:i];
		}
	}
	/* four vertices as a strip: the quad air_run draws over the whole attachment */
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[enc endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];
	printf("drvtest: draw: status %lu%s%s\n", (unsigned long)[cmd status],
	       [cmd error] != nil ? ", error: " : "",
	       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String] : "");
	if ([cmd status] != RW_CB_COMPLETED) {
		return;
	}

	/* What the render path still owes, counted on the device the same way the device's own
	 * protocols were: the encoder and the pipeline state are where the remaining work is,
	 * and a number beats an estimate. */
	for (int which = 0; which < 2; which++) {
		const char *pname = which == 0 ? "MTLRenderCommandEncoder" :
		    "MTLRenderPipelineState";
		id object = which == 0 ? (id)enc : (id)pipeline;
		Protocol *proto = objc_getProtocol(pname);
		unsigned n = 0, missing = 0, shown = 0;

		if (proto == NULL) {
			continue;
		}
		struct objc_method_description *req =
		    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

		for (unsigned i = 0; i < n; i++) {
			if ([object respondsToSelector:req[i].name]) {
				continue;
			}
			missing++;
			if (shown < 6) {
				printf("drvtest: draw:   %s still owes %s\n", pname,
				       sel_getName(req[i].name));
				shown++;
			}
		}
		free(req);
		printf("drvtest: draw: of %u methods %s requires, %u are unanswered\n", n, pname,
		       missing);
	}

	if (draw_all) {
		draw_every(dev, library, queue, rp, uniform, dtex, dsamp, targets);
		return;
	}

	unsigned pixel[4] = { 0 };

	[targets[2] getBytes:pixel
		 bytesPerRow:4 * 4 * 4
		  fromRegion:MTLRegionMake2D(0, 0, 1, 1)
		 mipmapLevel:0];
	/* All sixteen pixels, not one.
	 *
	 * One pixel is what made a flat varying look like a vertical flip for three boots:
	 * pixel (0,0) lies in the triangle whose provoking vertex carries the texture
	 * coordinate (0,1), so a FLAT varying puts texel (0,3) there -- exactly what a
	 * mirrored picture would put there. And pixels (3,3) and (0,3) read the same under
	 * flat and under smooth, so of the sixteen only fourteen can tell the two apart.
	 * docs/metal.md 10.32 is the measurement that followed from printing them all.
	 */
	{
		unsigned grid[4 * 4 * 4];

		memset(grid, 0, sizeof(grid));
		[targets[2] getBytes:grid
			 bytesPerRow:4 * 16
			  fromRegion:MTLRegionMake2D(0, 0, 4, 4)
			 mipmapLevel:0];
		for (unsigned gy = 0; gy < 4; gy++) {
			for (unsigned gx = 0; gx < 4; gx++) {
				const unsigned *t = grid + (gy * 4 + gx) * 4;

				printf("drvtest: draw: attachment 2 at (%u, %u) = %u %u %u %u\n",
				       gx, gy, t[0], t[1], t[2], t[3]);
			}
		}
	}
	printf("drvtest: draw: the pixel at (0, 0) of attachment 2 is %u %u %u %u\n",
	       pixel[0], pixel[1], pixel[2], pixel[3]);
	printf("drvtest: draw: softpipe computed                      %u %u %u %u\n",
	       expected[0], expected[1], expected[2], expected[3]);
	if (strcmp(shader, "clearFragment20") != 0) {
		/* The four words above are clearFragment20's; another shader has no constant
		 * here, and metalcheck.py produces its reference by running air_run. */
		printf("drvtest: draw: no constant for %s in this test; metalcheck compares it\n",
		       shader);
	} else {
		printf("drvtest: draw: %s\n",
		       memcmp(pixel, expected, sizeof(pixel)) == 0 ?
		       "THE SAME -- a Metal fragment shader drew on the Adreno" :
		       "DIFFERENT, and a different picture is a failure, not a variation");
	}
	/* The indexed draws go LAST, over this pipeline and these attachments, so that the
	 * picture above is their reference and the only thing that differs between it and
	 * them is the index buffer. */
	if (draw_index) {
		index_draws(dev, queue, rp, pipeline, uniform, corners, coords, dtex, dsamp,
			    targets);
	}
	fflush(stdout);
}

/*
 * --draw-depth: whether a depth test discards anything on the Adreno.
 *
 * Every draw this driver has made ran with no depth attachment and so with depth testing
 * off, which is not a choice anyone made -- it is an absence. This is the measurement that
 * closes it, and it is the same experiment air_metal_test runs on softpipe
 * (tools/mesa/air/air_metal_test.c, --draw-depth), so the two can be put side by side:
 *
 *   - a fragment shader that writes COLOUR, drawn four times. Three with
 *     MTLCompareFunctionLess and depth writes on, at depths 0.5, 0.75 and 0.25, and then
 *     once more at 0.75 with MTLCompareFunctionAlways. The quad is the same quad; what
 *     changes is the depth (through MTLViewport's znear, which is the only thing in Metal
 *     that moves this driver's quad in z) and the bytes of its constant buffer, so a draw
 *     that is not discarded leaves a colour the previous one did not. The colour after the
 *     far draw is the test: unchanged means discarded, and the Always control at that same
 *     depth afterwards says the compare function did it rather than the draw.
 *   - a fragment shader that writes DEPTH -- Apple's texturedFragment2DDepth and
 *     texturedFragment3DDepth -- where the buffer must come back holding what the shader
 *     computed. Those two sample a texture and store channel x to [[depth]], so their
 *     texture is seeded in [0, 1] instead of with air_run's pattern, whose 100.0 would
 *     clamp to the far plane and read exactly like the clear value.
 *
 * Which of the two runs is the pipeline's own answer (-writesDepth, out of the compiler
 * library's rwa_render_info) and not a flag here.
 */
static MTLPixelFormat depth_attachment_format = MTLPixelFormatDepth32Float;

/* 64 values in (0, 1), each exact as a float, so the one that comes back names the texel it
 * came from -- and none of them is 0 or 1, which a cleared or untouched buffer could hold. */
static float
depth_texel_value(unsigned x, unsigned y, unsigned z)
{
	return ((float)(x + 4 * y + 16 * z) + 0.5f) / 64.0f;
}

static void
fill_depth_texture(id<MTLTexture> tex)
{
	MTLPixelFormat pf = [tex pixelFormat];
	unsigned bytes = texel_bytes_of(pf);
	unsigned char row[DRAW_TEX * 16];

	for (NSUInteger z = 0; z < [tex depth]; z++) {
		for (NSUInteger y = 0; y < [tex height]; y++) {
			for (NSUInteger x = 0; x < [tex width]; x++) {
				float d = depth_texel_value((unsigned)x, (unsigned)y,
							    (unsigned)z);
				float v[4] = { d, d, d, d };

				write_texel(row + x * bytes, pf, v);
			}
			[tex replaceRegion:MTLRegionMake3D(0, y, z, [tex width], 1, 1)
			       mipmapLevel:0
				     slice:0
				 withBytes:row
			       bytesPerRow:[tex width] * bytes
			     bytesPerImage:0];
		}
	}
}

/* The depth buffer, through the driver's own aspect reader: -getBytes: has no aspect to
 * name and the compiler library refuses a depth texture through it by name, so this asks
 * for the depth aspect as floats. Returns false when the driver is older than this test. */
static bool
read_depth_buffer(id<MTLTexture> zt, float *depth, const char *what)
{
	SEL sel = sel_getUid("annibaleReadDepth:stencil:fromRegion:");

	if (![zt respondsToSelector:sel]) {
		printf("drvtest: depth: this texture does not answer "
		       "annibaleReadDepth:stencil:fromRegion:, so the driver is older than this "
		       "test\n");
		return false;
	}
	BOOL ok = ((BOOL (*)(id, SEL, float *, uint8_t *, MTLRegion))objc_msgSend)(
	    zt, sel, depth, NULL, MTLRegionMake2D(0, 0, DRAW_TEX, DRAW_TEX));

	if (!ok) {
		printf("drvtest: depth: %s: the depth could not be read back\n", what);
		return false;
	}
	bool uniform = true;

	for (unsigned i = 1; i < DRAW_TEX * DRAW_TEX; i++) {
		if (depth[i] != depth[0]) {
			uniform = false;
		}
	}
	printf("drvtest: depth: %s: depth at (0,0) = %.9g, and the 16 texels are %s\n", what,
	       depth[0], uniform ? "all that value" : "NOT all the same");
	return uniform;
}

/*
 * --argbuf: MTLArgumentEncoder on the device, and the buffer OFFSET that goes with it.
 *
 * Three measurements, and what each is worth is different, so they are printed apart:
 *
 *  1. THE FUNCTION PATH. -[MTLFunction newArgumentEncoderWithBufferIndex:] is a method of
 *     Metal's own class, and on this port Metal's own implementation cannot work: it asks
 *     the function for its reflection, which asks the device for -compiler, which is a
 *     two-instruction stub answering nil (0x181aac128), and then reports
 *     "bufferIndex %lu does not identify an argument buffer" and returns nil. The driver
 *     answers it instead, through a category, by reading the function's own bitcode. So
 *     what this measures is WHOSE answer came back: Apple's shaders declare no argument
 *     buffer at all (0 of 456 modules, docs/air.md 8.2), so the answer must still be nil
 *     -- but with the driver's own sentence, which names how many sets the function has.
 *     Metal's sentence and the driver's are told apart in the log by "this function
 *     declares".
 *  2. THE DESCRIPTOR PATH, which needs no shader: -[MTLDevice newArgumentEncoderWithArguments:]
 *     with a pointer, a texture and a sampler member. encodedLength must be 24 and
 *     alignment 8 -- 8 bytes per member, which is what Apple's own metadata says a set is
 *     (7 856 of Resident Evil 4's 7 858 sets; the two that differ are both in the one
 *     kernel that builds indirect command buffers and are refused before an encoder could
 *     be made). Then the three members are encoded and the compiler library is asked how
 *     many records it holds for that (buffer, offset), which is the whole of what an
 *     encoder does here.
 *  3. THE BUFFER OFFSET, which is not about argument buffers at all and is the reason this
 *     mode exists tonight: Apple's own clearFragment20 drawn twice, with its 48-byte
 *     constant block at offset 0 of one buffer and at OFFSET 1164 of another. 1164 is the
 *     offset the window server binds its own fragment buffer at, 55 times in one boot,
 *     and until this work every one of those bindings was refused. The two pictures must
 *     be the same four words, and the same four words the host reference gives
 *     (tools/air/metalcheck.py, and draw_path's own `expected`).
 *
 * What is NOT here, said plainly: a DRAW through an argument buffer. That needs a
 * .metallib holding a shader that declares one, and this port has no container writer --
 * Apple's libraries have no sets, and the game's are not something a job list can reach.
 * The draw-through-an-encoder differential is therefore the host's
 * (tools/air/argbufruncheck.sh, on softpipe), and what the device adds is the three above.
 */
static void
argbuf_path(id<MTLDevice> dev, const char *metallib)
{
	static const unsigned expected[4] = { 2458616957u, 2930221209u, 3401825461u,
					      3873429713u };
	NSError *err = nil;
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: argbuf: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: argbuf: newLibraryWithData: gave nil\n");
		return;
	}

	/* 1. the function path */
	id<MTLFunction> fragment = [library newFunctionWithName:
	    [NSString stringWithUTF8String:fragment_function]];

	if (fragment == nil) {
		printf("drvtest: argbuf: no %s in that library\n", fragment_function);
		return;
	}
	SEL withIndex = sel_getUid("newArgumentEncoderWithBufferIndex:");

	printf("drvtest: argbuf: %s answers newArgumentEncoderWithBufferIndex: %s\n",
	       fragment_function,
	       [fragment respondsToSelector:withIndex] ? "yes" : "NO (nothing implements it)");
	if ([fragment respondsToSelector:withIndex]) {
		id enc = ((id (*)(id, SEL, NSUInteger))objc_msgSend)(fragment, withIndex, 0);

		printf("drvtest: argbuf: an encoder for [[buffer(0)]] of %s is %s (Apple's "
		       "shaders declare no argument buffer, so nil is the right answer and "
		       "the LINE ABOVE from the driver is the measurement)\n",
		       fragment_function,
		       enc != nil ? class_getName(object_getClass(enc)) : "nil");
		[enc release];
	}

	/* 2. the descriptor path */
	Class descClass = objc_lookUpClass("MTLArgumentDescriptor");
	SEL newArgs = sel_getUid("newArgumentEncoderWithArguments:");

	if (descClass != NULL && [dev respondsToSelector:newArgs]) {
		static const NSUInteger type[3] = { 60 /* Pointer */, 58 /* Texture */,
						    59 /* Sampler */ };
		NSMutableArray *args = [NSMutableArray array];

		for (unsigned i = 0; i < 3; i++) {
			id d = [descClass argumentDescriptor];

			[d setIndex:i];
			[d setDataType:(MTLDataType)type[i]];
			[d setArrayLength:0];
			[args addObject:d];
		}
		id enc = ((id (*)(id, SEL, id))objc_msgSend)(dev, newArgs, args);

		if (enc == nil) {
			printf("drvtest: argbuf: newArgumentEncoderWithArguments: gave nil\n");
		} else {
			NSUInteger len = [enc encodedLength], align = [enc alignment];

			printf("drvtest: argbuf: %s: encodedLength %lu (must be 24: 8 bytes a "
			       "member), alignment %lu (must be 8)\n",
			       len == 24 && align == 8 ? "ok" : "WRONG",
			       (unsigned long)len, (unsigned long)align);

			/* and the records, which is what this encoder does instead of
			 * writing: the buffer's own bytes are never read by a draw. */
			id<MTLBuffer> dest = [dev newBufferWithLength:len > 0 ? len : 64
							     options:0];
			id<MTLBuffer> member = [dev newBufferWithLength:256 options:0];
			MTLTextureDescriptor *td = [MTLTextureDescriptor
			    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							 width:4 height:4 mipmapped:NO];
			[td setUsage:MTLTextureUsageShaderRead];
			id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
			MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
			id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

			[sd release];
			[enc setArgumentBuffer:dest offset:0];
			[enc setBuffer:member offset:0 atIndex:0];
			[enc setTexture:tex atIndex:1];
			[enc setSamplerState:samp atIndex:2];
			/* An id past the set's own members, which Metal would write a handle
			 * for and this driver refuses by name: the line comes from the
			 * driver, and what is checked here is that nothing crashed. */
			[enc setTexture:tex atIndex:7];

			unsigned recorded = argbuf_records(dest, 0);

			printf("drvtest: argbuf: %s: %u of 3 members recorded against that "
			       "buffer at offset 0\n", recorded == 3 ? "ok" : "WRONG",
			       recorded);
			[dest release];
			[member release];
			[tex release];
			[samp release];
			[enc release];
		}
	} else {
		printf("drvtest: argbuf: no MTLArgumentDescriptor class or no "
		       "newArgumentEncoderWithArguments: on this device\n");
	}

	/* 3. the buffer offset, which is the window server's own case */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:4 height:4
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[3] = { nil, nil, nil };

	for (int i = 0; i < 3; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: argbuf: attachment %d could not be made\n", i);
			return;
		}
	}
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

	[pd setFragmentFunction:fragment];
	[pd setVertexFunction:nil];
	for (int i = 0; i < 3; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatRGBA32Uint];
	}
	err = nil;
	id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd
										  error:&err];
	[pd release];
	if (pipeline == nil) {
		printf("drvtest: argbuf: no pipeline: %s\n",
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
		return;
	}

	/* The same 48 bytes in two places: at offset 0 of one buffer, and at offset 1164 of
	 * another whose other bytes are deliberately NOT that pattern -- so a driver that
	 * ignored the offset would read the wrong block and print different words, and one
	 * that refused the binding would print a refusal. */
	const NSUInteger at = 1164;
	id<MTLBuffer> flat = [dev newBufferWithLength:256 options:0];
	id<MTLBuffer> moved = [dev newBufferWithLength:4096 options:0];

	if (flat == nil || moved == nil || [flat contents] == NULL ||
	    [moved contents] == NULL) {
		printf("drvtest: argbuf: the two uniform buffers could not be made\n");
		return;
	}
	unsigned char *fb = [flat contents], *mb = [moved contents];

	for (NSUInteger i = 0; i < 256; i++) {
		fb[i] = (unsigned char)((i * 7 + 13) & 0xff);
	}
	memset(mb, 0xa5, 4096);
	for (NSUInteger i = 0; i < 256; i++) {
		mb[at + i] = fb[i];
	}

	id<MTLCommandQueue> queue = [dev newCommandQueue];
	struct { id<MTLBuffer> buffer; NSUInteger offset; const char *what; } run[2] = {
		{ flat, 0, "at offset 0" },
		{ moved, at, "at offset 1164, the window server's own" },
	};

	for (unsigned k = 0; k < 2; k++) {
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

		for (int i = 0; i < 3; i++) {
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[rp colorAttachments] objectAtIndexedSubscript:i];

			[a setTexture:targets[i]];
			[a setLoadAction:MTLLoadActionClear];
			[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
			[a setStoreAction:MTLStoreActionStore];
		}
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc =
		    [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			printf("drvtest: argbuf: no render encoder\n");
			return;
		}
		[enc setRenderPipelineState:pipeline];
		/* every index up to 16, as draw_path binds it: this shader's block is
		 * declared at [[buffer(14)]] and a driver that bound only index 0 would
		 * leave it reading nothing */
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:run[k].buffer offset:run[k].offset atIndex:i];
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];

		if ([cmd status] != RW_CB_COMPLETED) {
			printf("drvtest: argbuf: the draw %s did not complete: status %lu%s%s\n",
			       run[k].what, (unsigned long)[cmd status],
			       [cmd error] != nil ? ", " : "",
			       [cmd error] != nil
				   ? [[[cmd error] localizedDescription] UTF8String] : "");
			continue;
		}
		unsigned pixel[4] = { 0 };

		[targets[2] getBytes:pixel
			 bytesPerRow:4 * 4 * 4
			  fromRegion:MTLRegionMake2D(0, 0, 1, 1)
			 mipmapLevel:0];
		bool same = pixel[0] == expected[0] && pixel[1] == expected[1] &&
			    pixel[2] == expected[2] && pixel[3] == expected[3];

		printf("drvtest: argbuf: %s: the uniform block %s draws %u %u %u %u\n",
		       same ? "ok" : "WRONG", run[k].what, pixel[0], pixel[1], pixel[2],
		       pixel[3]);
	}
	printf("drvtest: argbuf: the four words above are the host's own reference "
	       "(tools/air/metalcheck.py): %u %u %u %u\n", expected[0], expected[1],
	       expected[2], expected[3]);
}

static void
depth_path(id<MTLDevice> dev, const char *metallib)
{
	NSError *err = nil;
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: depth: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: depth: newLibraryWithData: gave nil\n");
		return;
	}
	id<MTLFunction> fragment = [library newFunctionWithName:
	    [NSString stringWithUTF8String:fragment_function]];

	if (fragment == nil) {
		printf("drvtest: depth: no %s in that library\n", fragment_function);
		return;
	}
	printf("drvtest: depth: %s %s, depth attachment pixel format %lu\n", metallib,
	       fragment_function, (unsigned long)depth_attachment_format);
	load_texture_query(dev);

	/* Three colour attachments of air_run's own format, as the colour draw has: the
	 * shader this defaults to writes render target 2, and 0 and 1 have to exist for 2
	 * to be bound at all. */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:DRAW_TEX
							     height:DRAW_TEX
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[3] = { nil, nil, nil };

	for (int i = 0; i < 3; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: depth: colour attachment %d could not be made\n", i);
			return;
		}
	}

	/* The depth attachment: MTLTextureUsageRenderTarget and nothing else. The compiler
	 * library turns that usage into PIPE_BIND_DEPTH_STENCIL because the FORMAT is a
	 * depth one, which is the one place where the same Metal usage bit means two
	 * different Gallium bindings. */
	MTLTextureDescriptor *ztd =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth_attachment_format
							      width:DRAW_TEX
							     height:DRAW_TEX
							  mipmapped:NO];

	[ztd setUsage:MTLTextureUsageRenderTarget];

	id<MTLTexture> zt = [dev newTextureWithDescriptor:ztd];

	if (zt == nil) {
		printf("drvtest: depth: the depth attachment could not be made; the driver has "
		       "printed the compiler library's own reason\n");
		return;
	}
	printf("drvtest: depth: the depth attachment is a %s of pixel format %lu\n",
	       class_getName(object_getClass(zt)), (unsigned long)[zt pixelFormat]);

	/* And the proof that it is not readable as colour, which is the contract and not an
	 * accident: a caller asking for a depth texel the colour way is told which call to
	 * use. The driver prints the library's sentence. */
	{
		unsigned char ignored[DRAW_TEX * DRAW_TEX * 8];

		memset(ignored, 0, sizeof(ignored));
		printf("drvtest: depth: -getBytes: on the depth attachment must be refused:\n");
		[zt getBytes:ignored
		 bytesPerRow:DRAW_TEX * 8
		  fromRegion:MTLRegionMake2D(0, 0, DRAW_TEX, DRAW_TEX)
		 mipmapLevel:0];
	}

	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

	[pd setFragmentFunction:fragment];
	[pd setVertexFunction:nil];   /* the library generates the stage, as air_run does */
	for (int i = 0; i < 3; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatRGBA32Uint];
	}
	/* Both aspect properties, as an application with a combined format sets them: one
	 * texture behind the two, which is what the library requires of the pair. */
	[pd setDepthAttachmentPixelFormat:depth_attachment_format];
	if (depth_attachment_format == MTLPixelFormatDepth32Float_Stencil8) {
		[pd setStencilAttachmentPixelFormat:depth_attachment_format];
	}
	err = nil;
	id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd
										  error:&err];

	[pd release];
	printf("drvtest: depth: newRenderPipelineStateWithDescriptor: gave %s%s%s\n",
	       pipeline != nil ? class_getName(object_getClass(pipeline)) : "nil",
	       err != nil ? ", error: " : "",
	       err != nil ? [[err localizedDescription] UTF8String] : "");
	if (pipeline == nil) {
		return;
	}
	SEL writes = sel_getUid("writesDepth");
	bool shader_writes_depth =
	    [pipeline respondsToSelector:writes] &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(pipeline, writes);

	printf("drvtest: depth: this pipeline's fragment shader %s to [[depth]]\n",
	       shader_writes_depth ? "STORES" : "stores nothing");

	/*
	 * The two depth/stencil states, which is the first MTLDepthStencilState this driver
	 * has ever been asked for. What they answer about themselves is printed, because
	 * those four methods are the whole of <MTLDepthStencilStateSPI> beyond the public
	 * protocol and Apple's base class answers NO to all four -- so "writesDepth = YES"
	 * here is the override being read back, not a default.
	 */
	MTLDepthStencilDescriptor *dsd = [[MTLDepthStencilDescriptor alloc] init];

	[dsd setDepthCompareFunction:MTLCompareFunctionLess];
	[dsd setDepthWriteEnabled:YES];
	id<MTLDepthStencilState> less = [dev newDepthStencilStateWithDescriptor:dsd];

	[dsd setDepthCompareFunction:MTLCompareFunctionAlways];
	id<MTLDepthStencilState> always = [dev newDepthStencilStateWithDescriptor:dsd];

	[dsd release];
	if (less == nil || always == nil) {
		printf("drvtest: depth: newDepthStencilStateWithDescriptor: gave nil, so there "
		       "is no state to test with\n");
		return;
	}
	printf("drvtest: depth: newDepthStencilStateWithDescriptor: gave %s\n",
	       class_getName(object_getClass(less)));
	{
		static const char *const spi[4] = { "readsDepth", "writesDepth", "readsStencil",
						    "writesStencil" };
		unsigned missing = 0;

		printf("drvtest: depth: the Less state answers");
		for (unsigned i = 0; i < 4; i++) {
			SEL s = sel_getUid(spi[i]);

			if (![less respondsToSelector:s]) {
				missing++;
				printf(" %s=unanswered", spi[i]);
				continue;
			}
			printf(" %s=%s", spi[i],
			       ((BOOL (*)(id, SEL))objc_msgSend)(less, s) ? "YES" : "NO");
		}
		printf(" (%u unanswered)\n", missing);

		Protocol *proto = objc_getProtocol("MTLDepthStencilState");
		unsigned n = 0, unanswered = 0;

		if (proto != NULL) {
			struct objc_method_description *req =
			    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

			for (unsigned i = 0; i < n; i++) {
				if (![less respondsToSelector:req[i].name]) {
					printf("drvtest: depth:   MTLDepthStencilState still "
					       "owes %s\n", sel_getName(req[i].name));
					unanswered++;
				}
			}
			free(req);
		}
		printf("drvtest: depth: of %u methods MTLDepthStencilState requires, %u are "
		       "unanswered\n", n, unanswered);
	}

	/* The shader's constant buffer, bound at every index up to 16 because the one this
	 * defaults to is declared at 14 (draw_path says why). Its bytes are what change
	 * between steps, so a draw that was not discarded leaves a different colour. */
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];

	if (uniform == nil || [uniform contents] == NULL) {
		printf("drvtest: depth: no constant buffer\n");
		return;
	}

	/* The textures the shader declares, if any, made the shape each slot asks for --
	 * and then seeded in [0, 1] when the shader stores what it samples into [[depth]],
	 * because air_run's 100.0 would clamp to the far plane and read as the clear. */
	id<MTLTexture> dtex[RW_TEX_SLOTS] = { nil };
	id<MTLSamplerState> dsamp = nil;
	unsigned declared = 0;
	char texwhy[256] = "";

	if (!textures_for_shader(dev, fragment, dtex, RW_TEX_SLOTS, &declared, texwhy,
				 sizeof(texwhy))) {
		printf("drvtest: depth: %s: %s\n", fragment_function, texwhy);
		return;
	}
	bool declared_3d = false;

	for (unsigned i = 0; i < RW_TEX_SLOTS; i++) {
		if (dtex[i] == nil) {
			continue;
		}
		if ([dtex[i] textureType] == MTLTextureType3D) {
			declared_3d = true;
		}
		if (shader_writes_depth) {
			fill_depth_texture(dtex[i]);
		}
	}
	if (declared) {
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		dsamp = [dev newSamplerStateWithDescriptor:sd];
		[sd release];
		printf("drvtest: depth: %u texture slot%s declared, seeded with %s\n", declared,
		       declared == 1 ? "" : "s",
		       shader_writes_depth ? "(x + 4y + 16z + 0.5) / 64 in every channel"
					   : "air_run's texels");
	}

	id<MTLCommandQueue> queue = [dev newCommandQueue];

	/*
	 * One step: a render pass of its own, because a pass is where a load action lives
	 * and these steps differ in whether they clear. The first clears the colour and the
	 * depth; the rest load both, which is how the depth of the previous draw is still
	 * there to be tested against.
	 */
	struct step {
		const char *what;
		float z;
		bool pattern;
		unsigned char fill;
		bool clear;
		bool control;
		bool expect_draw;
	};
	static const struct step colour_steps[4] = {
		{ "the near draw at depth 0.5",            0.5f,  true,  0x00, true,  false, true },
		{ "the far draw at depth 0.75",            0.75f, false, 0xa5, false, false, false },
		{ "the nearer draw at depth 0.25",         0.25f, false, 0x5a, false, false, true },
		{ "the control at depth 0.75 with Always", 0.75f, false, 0x3c, false, true,  true },
	};
	static const struct step depth_steps[1] = {
		{ "the draw, with the test off and depth writes on", 0.5f, true, 0x00, true,
		  true, true },
	};
	const struct step *steps = shader_writes_depth ? depth_steps : colour_steps;
	unsigned nsteps = shader_writes_depth ? 1 : 4;
	unsigned pixel[4][4];
	float depth_at[4];
	bool uniform_depth = true;

	memset(pixel, 0, sizeof(pixel));
	memset(depth_at, 0, sizeof(depth_at));
	for (unsigned s = 0; s < nsteps; s++) {
		unsigned char *bytes = [uniform contents];

		for (int i = 0; i < 256; i++) {
			bytes[i] = steps[s].pattern ?
			    (unsigned char)((i * 7 + 13) & 0xff) : steps[s].fill;
		}

		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

		for (int i = 0; i < 3; i++) {
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[rp colorAttachments] objectAtIndexedSubscript:i];

			[a setTexture:targets[i]];
			[a setLoadAction:steps[s].clear ? MTLLoadActionClear
							: MTLLoadActionLoad];
			[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
			[a setStoreAction:MTLStoreActionStore];
		}
		MTLRenderPassDepthAttachmentDescriptor *za = [rp depthAttachment];

		[za setTexture:zt];
		[za setLoadAction:steps[s].clear ? MTLLoadActionClear : MTLLoadActionLoad];
		[za setClearDepth:1.0];
		[za setStoreAction:MTLStoreActionStore];

		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			printf("drvtest: depth: %s: no encoder\n", steps[s].what);
			return;
		}
		[enc setRenderPipelineState:pipeline];
		[enc setDepthStencilState:steps[s].control ? always : less];
		/* The quad's depth. MTLViewport's znear is the only thing in Metal that moves
		 * it: the generated vertex stage puts all four corners at clip z = 0, and
		 * Metal's NDC z starts at 0 on the near plane, so the window depth of what is
		 * drawn is znear exactly. The rectangle is the whole attachment, which is what
		 * the driver serves. */
		[enc setViewport:(MTLViewport){ 0.0, 0.0, DRAW_TEX, DRAW_TEX, steps[s].z, 1.0 }];
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:uniform offset:0 atIndex:i];
			if (i < RW_TEX_SLOTS && dtex[i] != nil) {
				[enc setFragmentTexture:dtex[i] atIndex:i];
			}
			if (dsamp != nil) {
				[enc setFragmentSamplerState:dsamp atIndex:i];
			}
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		printf("drvtest: depth: %s: status %lu%s%s\n", steps[s].what,
		       (unsigned long)[cmd status], [cmd error] != nil ? ", error: " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String] : "");
		if ([cmd status] != RW_CB_COMPLETED) {
			return;
		}

		[targets[2] getBytes:pixel[s]
			 bytesPerRow:DRAW_TEX * 4 * 4
			  fromRegion:MTLRegionMake2D(0, 0, 1, 1)
			 mipmapLevel:0];
		printf("drvtest: depth: %s: the pixel at (0, 0) of attachment 2 is %u %u %u %u\n",
		       steps[s].what, pixel[s][0], pixel[s][1], pixel[s][2], pixel[s][3]);

		float map[DRAW_TEX * DRAW_TEX];

		if (!read_depth_buffer(zt, map, steps[s].what)) {
			uniform_depth = false;
		}
		depth_at[s] = map[0];
	}

	/* The verdicts. Each says what would have to be true for it to read otherwise, and
	 * the tolerance is the format's: Depth32Float holds 0.25, 0.5 and 0.75 exactly, so
	 * there it is ==; Depth16Unorm holds none of them and gets one step of itself. */
	float tol = depth_attachment_format == MTLPixelFormatDepth16Unorm ?
	    1.0f / 65535.0f : 0.0f;

	if (shader_writes_depth) {
		float want = depth_texel_value(1, 1, declared_3d ? 1 : 0);
		float got = depth_at[0];
		float d = got - want;
		bool same = (d < 0.0f ? -d : d) <= tol;

		printf("drvtest: depth: the shader sampled texel (1, 1, %u) and stored %.9g to "
		       "[[depth]]; the buffer holds %.9g\n", declared_3d ? 1 : 0, want, got);
		printf("drvtest: depth: the quad's own depth was 0.5 and the clear was 1, so "
		       "neither could be mistaken for it\n");
		printf("drvtest: depth: %s\n", same && uniform_depth ?
		       "THE SAME -- a Metal fragment shader's [[depth]] reached the depth "
		       "buffer on the Adreno" :
		       "DIFFERENT, and a depth buffer that is nearly right is worse than none");
	} else {
		bool far_discarded = memcmp(pixel[1], pixel[0], sizeof(pixel[0])) == 0;
		bool nearer_drawn = memcmp(pixel[2], pixel[0], sizeof(pixel[0])) != 0;
		bool control_drawn = memcmp(pixel[3], pixel[2], sizeof(pixel[0])) != 0;
		static const float want[4] = { 0.5f, 0.5f, 0.25f, 0.75f };
		bool depths_right = true;

		for (unsigned s = 0; s < 4; s++) {
			float d = depth_at[s] - want[s];

			if ((d < 0.0f ? -d : d) > tol) {
				depths_right = false;
			}
		}
		printf("drvtest: depth: the far draw was %s, and its buffer held a different "
		       "fill, so an undiscarded one would have changed the pixel\n",
		       far_discarded ? "DISCARDED (the pixel did not change)"
				     : "NOT discarded -- the pixel changed");
		printf("drvtest: depth: the nearer draw %s\n",
		       nearer_drawn ? "PASSED (the pixel changed)"
				    : "did NOT pass, and it is in front of what was there");
		printf("drvtest: depth: the Always control at the same far depth %s\n",
		       control_drawn ? "DREW (so the compare function discarded the other one, "
				       "not the draw)"
				     : "did not draw either, so the fault is not the depth test");
		printf("drvtest: depth: depth after each step: %.9g %.9g %.9g %.9g (0.5, 0.5 "
		       "kept, 0.25, 0.75)\n", depth_at[0], depth_at[1], depth_at[2],
		       depth_at[3]);
		printf("drvtest: depth: %s\n",
		       far_discarded && nearer_drawn && control_drawn && depths_right &&
		       uniform_depth ?
		       "DEPTH TESTING DISCARDS ON THE ADRENO -- the compare function, the depth "
		       "write and the buffer all agree" :
		       "SOMETHING ABOVE DISAGREES, and a depth test that is nearly right is "
		       "worse than none");
	}
	fflush(stdout);
}
