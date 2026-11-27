/* Picture band measurement and display tests, included into metal_drv_test.m. */


/*
 * The two bands: Apple's own textured blit over the whole attachment, linear and nearest.
 *
 * This is where the first boot stopped (logs/101536-run.log): the mosaic's summary was the
 * last line of the process, and after it the kernel's idle heartbeat and nothing else for
 * four minutes. The GPU was not wedged -- the heartbeat went on reading sensors and the run
 * ended cleanly -- and the driver refuses a draw the GPU does not finish with a sentence
 * and a status (that is how Apple's two [[depth]] shaders are known to hang it,
 * logs/094810-run.log: "status 5, the GPU did not finish the draw"). So a silent stop is
 * NOT a hung dispatch, and the suspects are all on this side of the fence.
 *
 * Everything here is therefore built to name the step rather than to survive it:
 *
 *   - every step says its own name BEFORE it starts, flushed. On the way this stage takes,
 *     nothing printed unless something failed, so the stall had the whole stage to hide in;
 *   - between the mosaic and the textured pipeline there is now a PROBE, which is the
 *     cheapest way to cut the suspect space in half in one boot. It draws
 *     clearFragment01 -- the shader the mosaic has already drawn 64 times, with the
 *     generated vertex stage, no texture, no sampler and no vertex buffer -- into the
 *     band-sized attachments, and reads first one pixel and then the whole 1080x648 back.
 *     Three different things can stall in there and each has its own line, so the boot
 *     says which: making four 1080x648 BGRA8Unorm render targets, drawing into one, and
 *     reading one back. If the probe gets through, none of those is the cause and what is
 *     left is Apple's vertex stage, the sampled texture and the sampler;
 *   - nothing here stores to [[depth]], which is the one thing known to hang this GPU.
 *
 * And the mosaic is already on the panel before any of it runs, so a stall now costs the
 * bands and not the screenshot.
 */
static void
pic_bands(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	id<MTLTexture> bt[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *brp = nil;
	id<MTLRenderPipelineState> ps = nil;
	id<MTLTexture> grad = nil;
	id<MTLBuffer> vpos = nil, vuv = nil;
	id<MTLFunction> vfn = nil, ffn = nil;
	MTLTextureDescriptor *gd = nil;
	unsigned char *band = NULL;
	static float texels[PIC_TEX * PIC_TEX * 4];
	static const float quad[4][4] = {
		{ -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
	};
	static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };

	pic_step("the memory for one band on this side");
	band = malloc((size_t)PANEL_W * PIC_BAND_H * 4);
	if (band == NULL) {
		printf("drvtest: picture: no memory for a band of %u by %u\n", PANEL_W,
		       PIC_BAND_H);
		goto out;
	}

	pic_step("four 1080x648 BGRA8Unorm render targets for the two bands (the ladder has "
		 "just made and freed the same four)");
	if (!pic_targets(dev, bt, 4, PANEL_W, PIC_BAND_H, &brp)) {
		goto out;
	}
	pic_step("four attachments made");

	/* ---- Apple's own blit ------------------------------------------------------- */
	pic_step("Apple's two functions, texturedVertex2D and texturedFragment2DFloat");
	vfn = [library newFunctionWithName:@"texturedVertex2D"];
	ffn = [library newFunctionWithName:@"texturedFragment2DFloat"];
	printf("drvtest: picture: texturedVertex2D %s, texturedFragment2DFloat %s\n",
	       vfn != nil ? "is here" : "is MISSING", ffn != nil ? "is here" : "is MISSING");
	fflush(stdout);
	if (vfn == nil || ffn == nil) {
		goto out;
	}
	pic_step("the textured pipeline, with Apple's vertex stage");
	ps = pic_pipeline(dev, vfn, ffn, 4, "texturedVertex2D + texturedFragment2DFloat");
	if (ps == nil) {
		goto out;
	}
	pic_step("the textured pipeline is made");

	/* The texture Apple's blit samples: 8x8, RGBA32Float because the shader declares
	 * texture2d<float, sample>, every channel a level of pic_ramp so that what the
	 * sampler returns is exact in eight bits. */
	pic_step("the 8x8 RGBA32Float texture");
	gd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
								width:PIC_TEX
							       height:PIC_TEX
							    mipmapped:NO];
	[gd setUsage:MTLTextureUsageShaderRead];
	grad = [dev newTextureWithDescriptor:gd];
	if (grad == nil) {
		printf("drvtest: picture: the 8x8 texture could not be made\n");
		goto out;
	}
	for (unsigned y = 0; y < PIC_TEX; y++) {
		for (unsigned x = 0; x < PIC_TEX; x++) {
			unsigned char c[4];
			float *t = texels + (y * PIC_TEX + x) * 4;

			pic_texel_colour(x, y, c);
			t[0] = c[2] / 255.0f;
			t[1] = c[1] / 255.0f;
			t[2] = c[0] / 255.0f;
			t[3] = 1.0f;
		}
	}
	/* one call for the whole picture and not one per row, for draw_path's reason: a
	 * seed written at a non-zero region origin is a second thing being measured */
	pic_step("seeding that texture in one replaceRegion:");
	[grad replaceRegion:MTLRegionMake2D(0, 0, PIC_TEX, PIC_TEX)
		mipmapLevel:0
		  withBytes:texels
		bytesPerRow:PIC_TEX * 4 * sizeof(float)];
	/* and read it back, for draw_path's other reason: a draw that samples a texture
	 * nobody has looked at cannot tell a wrong sample from a wrong seed */
	pic_step("reading that texture back");
	{
		static float back[PIC_TEX * PIC_TEX * 4];

		memset(back, 0, sizeof(back));
		[grad getBytes:back
		   bytesPerRow:PIC_TEX * 4 * sizeof(float)
		    fromRegion:MTLRegionMake2D(0, 0, PIC_TEX, PIC_TEX)
		   mipmapLevel:0];
		printf("drvtest: picture: texel (0,0) reads back r g b %g %g %g and was seeded "
		       "%g %g %g\n", (double)back[0], (double)back[1], (double)back[2],
		       (double)texels[0], (double)texels[1], (double)texels[2]);
		fflush(stdout);
	}

	/* What texturedVertex2D reads: buffer 0 one float4 of clip-space position per
	 * vertex, buffer 1 one float2 of texture coordinate. The corners and the
	 * coordinates are air_metal_test's own, so this draw and the host's are one quad.
	 *
	 * Which way up it comes out is the viewport's, not this file's, and it has just
	 * changed: the library now puts clip y = +1 in memory row 0, which is Metal's rule
	 * (docs/metal.md 10.28, landed once 10.32 had fixed the flat varying that was
	 * hiding it). The corner at clip (-1, +1) therefore lands in row 0 and carries the
	 * coordinate v = 1, so the TOP of the band is texel row PIC_TEX-1 and the band is
	 * the texture mirrored vertically.
	 *
	 * This table is deliberately NOT flipped to cancel that. The four corners and the
	 * four coordinates are air_metal_test's own, so that this draw and the host's are
	 * one quad; and compensating in the coordinates would hide the very thing the new
	 * sign made visible, which is how a flat varying passed for a vertical flip for
	 * three boots. What flips is the EXPECTATION, in pic_count_bands.
	 *
	 * The picture says it for itself: texel (0,0) is green and texel (0,7) is blue, so
	 * the band must now come out BLUE at the top and GREEN at the bottom, and two named
	 * pixels below say it in numbers rather than in a count.
	 */
	pic_queue_state(queue, "before the two band draws");
	pic_step("the two vertex buffers Apple's vertex stage reads");
	vpos = [dev newBufferWithLength:64 options:0];
	vuv = [dev newBufferWithLength:64 options:0];
	if (vpos == nil || vuv == nil || [vpos contents] == NULL || [vuv contents] == NULL) {
		printf("drvtest: picture: no vertex buffers\n");
		goto out;
	}
	memcpy([vpos contents], quad, sizeof(quad));
	memcpy([vuv contents], uv, sizeof(uv));

	for (int pass = 0; pass < 2; pass++) {
		const char *what = pass == 0 ? "the linear band" : "the nearest band";
		id<MTLBuffer> vbuf[2] = { vpos, vuv };
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		pic_step(pass == 0 ? "the linear sampler" : "the nearest sampler");
		[sd setMinFilter:pass == 0 ? MTLSamplerMinMagFilterLinear
					   : MTLSamplerMinMagFilterNearest];
		[sd setMagFilter:pass == 0 ? MTLSamplerMinMagFilterLinear
					   : MTLSamplerMinMagFilterNearest];
		[sd setSAddressMode:MTLSamplerAddressModeClampToEdge];
		[sd setTAddressMode:MTLSamplerAddressModeClampToEdge];
		id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

		[sd release];
		if (samp == nil) {
			printf("drvtest: picture: no %s\n", what);
			continue;
		}
		pic_step(pass == 0 ? "drawing the linear band" : "drawing the nearest band");
		if (pic_draw(queue, brp, ps, vbuf, 2, nil, 0, grad, samp, what, true)) {
			pic_step(pass == 0 ? "the linear band drew; reading it back"
					   : "the nearest band drew; reading it back");
			[bt[0] getBytes:band
			    bytesPerRow:PANEL_W * 4
			     fromRegion:MTLRegionMake2D(0, 0, PANEL_W, PIC_BAND_H)
			    mipmapLevel:0];
			pic_blit(band, PANEL_W, PIC_BAND_H, 0,
				 pass == 0 ? PIC_LINEAR_Y : PIC_NEAREST_Y);
			/* on the screen at once, before the other band is even begun */
			pic_push(what);
		}
		[samp release];
	}
out:
	free(band);
	[vpos release];
	[vuv release];
	[grad release];
	[ps release];
	pic_release_targets(bt, 4, brp);
	[vfn release];
	[ffn release];
	pic_step("done");
}

/* What the two bands must hold, and a handful of pixels named one by one. */
static void
pic_count_bands(void)
{
	unsigned long bad_nearest = 0, bad_centres = 0;

	/*
	 * THE TEXEL ROW IS COUNTED FROM THE BOTTOM, and that is Metal's orientation rather
	 * than a workaround: the viewport puts clip y = +1 in memory row 0 (10.28), the
	 * corner that lands there carries v = 1, so band row r shows texel row
	 * PIC_TEX-1 - r/PIC_SY where it used to show r/PIC_SY. The quad and its
	 * coordinates are unchanged on purpose -- see the note in pic_bands.
	 *
	 * The x axis is untouched. Nothing in the viewport's y sign is a horizontal
	 * statement, which is also why the linear/nearest contrast at column 134 below
	 * reads exactly as it did before the sign went in.
	 */
	for (unsigned q = 0; q < PIC_BAND_H; q++) {
		for (unsigned p = 0; p < PANEL_W; p++) {
			unsigned char want[4];
			const unsigned char *g = pic_panel +
			    ((size_t)(PIC_NEAREST_Y + q) * PANEL_W + p) * 4;

			pic_texel_colour(p / PIC_SX, PIC_TEX - 1 - q / PIC_SY, want);
			if (g[0] != want[0] || g[1] != want[1] || g[2] != want[2]) {
				bad_nearest++;
			}
		}
	}
	for (unsigned j = 0; j < PIC_TEX; j++) {
		for (unsigned i = 0; i < PIC_TEX; i++) {
			unsigned px = PIC_SX * i + (PIC_SX - 1) / 2;
			/* texel row j is now at band row PIC_TEX-1-j */
			unsigned py = PIC_SY * (PIC_TEX - 1 - j) + (PIC_SY - 1) / 2;
			unsigned char want[4];
			const unsigned char *g = pic_panel +
			    ((size_t)(PIC_LINEAR_Y + py) * PANEL_W + px) * 4;

			pic_texel_colour(i, j, want);
			if (g[0] != want[0] || g[1] != want[1] || g[2] != want[2]) {
				if (bad_centres < 6) {
					printf("drvtest: picture: linear texel centre (%u,%u) "
					       "at panel (%u,%u) is r g b %u %u %u and its "
					       "texel is %u %u %u\n", i, j, px,
					       PIC_LINEAR_Y + py, g[2], g[1], g[0], want[2],
					       want[1], want[0]);
				}
				bad_centres++;
			}
		}
	}
	printf("drvtest: picture: nearest band: %lu pixels of %u are not the texel the "
	       "sampler must have returned\n", bad_nearest, PANEL_W * PIC_BAND_H);
	printf("drvtest: picture: linear band: %lu of %u texel centres are not their texel\n",
	       bad_centres, PIC_TEX * PIC_TEX);

	/* The one place the two bands must differ, and the whole of what says the sampler
	 * state reached the hardware: halfway between texel 0 and texel 1 along a row of
	 * texel centres. Nearest can only answer one of the two; linear must answer
	 * between them, and softpipe through this same library answers 18 there.
	 *
	 * Unmoved by the viewport's y sign, and that is the point: this reads the RED
	 * channel, which pic_texel_colour makes a function of x alone, so it is a horizontal
	 * statement and a vertical mirror cannot change it. If this pair ever moves with a
	 * y sign, the cause is not the y sign. */
	unsigned x = PIC_SX - 1, y = (PIC_SY - 1) / 2;
	const unsigned char *l = pic_panel + ((size_t)(PIC_LINEAR_Y + y) * PANEL_W + x) * 4;
	const unsigned char *n = pic_panel + ((size_t)(PIC_NEAREST_Y + y) * PANEL_W + x) * 4;

	printf("drvtest: picture: at column %u, row %u of each band: linear red %u, nearest "
	       "red %u (texel 0 is %u, texel 1 is %u; softpipe gives 18 and 0)\n", x, y,
	       l[2], n[2], pic_ramp[0], pic_ramp[1]);
	printf("drvtest: picture: the sampler %s\n",
	       l[2] > pic_ramp[0] && l[2] < pic_ramp[1] && n[2] == pic_ramp[0] ?
	       "FILTERS: the linear band is between two texels where the nearest one is on "
	       "one of them" :
	       "did not do that, and the two bands are not telling two things apart");

	/* And the orientation itself, as two named pixels rather than as a count: "0 of
	 * 699840" says the band matched its expectation, whatever that expectation was, and
	 * cannot say which way up anything is. With Metal's rule the top of the band is
	 * texel row PIC_TEX-1 and the bottom is texel row 0, so it is BLUE at the top and
	 * GREEN at the bottom; under the old sign it was the other way up. */
	{
		unsigned char top[4], bottom[4];
		const unsigned char *a = pic_panel +
		    ((size_t)(PIC_LINEAR_Y + (PIC_SY - 1) / 2) * PANEL_W +
		     (PIC_SX - 1) / 2) * 4;
		const unsigned char *b = pic_panel +
		    ((size_t)(PIC_LINEAR_Y + PIC_SY * (PIC_TEX - 1) + (PIC_SY - 1) / 2) *
		     PANEL_W + (PIC_SX - 1) / 2) * 4;

		pic_texel_colour(0, PIC_TEX - 1, top);
		pic_texel_colour(0, 0, bottom);
		printf("drvtest: picture: the band's top left is r g b %u %u %u (texel (0,%u), "
		       "must be %u %u %u) and its bottom left is %u %u %u (texel (0,0), must "
		       "be %u %u %u): clip y = +1 is memory row 0\n", a[2], a[1], a[0],
		       PIC_TEX - 1, top[2], top[1], top[0], b[2], b[1], b[0], bottom[2],
		       bottom[1], bottom[0]);
	}

	/* and the four corner squares of the mosaic, which has no filtering in it at all:
	 * what a person can check against the picture by eye */
	static const unsigned corner[4][2] = { { 0, 0 }, { 7, 0 }, { 0, 7 }, { 7, 7 } };

	for (int c = 0; c < 4; c++) {
		unsigned i = corner[c][0], j = corner[c][1];
		unsigned px = i * PIC_TILE_W + PIC_TILE_W / 2;
		unsigned py = j * PIC_TILE_H + PIC_TILE_H / 2;
		unsigned char want[4];
		const unsigned char *g = pic_panel + ((size_t)py * PANEL_W + px) * 4;

		pic_mosaic_colour(i, j, want);
		printf("drvtest: picture: clearFragment%02X at panel (%u,%u) is r g b %u %u %u "
		       "and must be %u %u %u: %s\n", 4 * (j * PIC_TILES + i) + 1, px, py,
		       g[2], g[1], g[0], want[2], want[1], want[0],
		       g[0] == want[0] && g[1] == want[1] && g[2] == want[2] ? "the same"
									    : "DIFFERENT");
	}
	fflush(stdout);
}

/*
 * And then keep repainting it, for as long as the caller asked.
 *
 * The repainting is not what puts the picture there -- pic_push already did that after
 * every stage, and the copy stays in the boot framebuffer by itself. It is for the boot
 * where a daemon of Apple's IS running and draws over the panel between one screenshot
 * and the next: five times a second beats a window server that swaps occasionally. With
 * JOBS="" nothing of this is needed and the first push would have been enough.
 */
static void
pic_show(int seconds)
{
	if (pic_connection == 0) {
		return;
	}
	/* "--picture 0" is still a picture: pic_push has already painted it once. */
	if (seconds < 1) {
		seconds = 1;
	}
	printf("drvtest: picture: repainting for %d seconds; the screenshot can be taken at "
	       "any time now and more than once\n", seconds);
	fflush(stdout);
	for (int i = 0; i < seconds * 5; i++) {
		struct pic_show_request show = {
			(uint64_t)(uintptr_t)pic_panel, PANEL_W, PANEL_H, PANEL_W * 4, 0
		};

		IOConnectCallStructMethod(pic_connection, 0x7f, &show, sizeof(show), NULL,
					  NULL);
		usleep(200000);
	}
	printf("drvtest: picture: done repainting after %d seconds\n", seconds);
	fflush(stdout);
}

static void
picture_path(id<MTLDevice> dev, const char *metallib, int seconds)
{
	NSData *data = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: picture: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	NSError *err = nil;
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: picture: newLibraryWithData: gave nil\n");
		return;
	}
	pic_panel = calloc(1, (size_t)PANEL_W * PANEL_H * 4);
	if (pic_panel == NULL) {
		printf("drvtest: picture: no memory for %ux%u pixels\n", PANEL_W, PANEL_H);
		return;
	}
	printf("drvtest: picture: %s; the panel is %ux%u, the mosaic %ux%u tiles of %ux%u, "
	       "each band %ux%u\n", metallib, PANEL_W, PANEL_H, PIC_TILES, PIC_TILES,
	       PIC_TILE_W, PIC_TILE_H, PANEL_W, PIC_BAND_H);
	fflush(stdout);

	id<MTLCommandQueue> queue = [dev newCommandQueue];

	/* The panel BEFORE anything is drawn, so that a stage which finishes can be shown
	 * the moment it finishes. The first boot of this mode drew the whole mosaic
	 * correctly and then stopped on its way into the bands, and because the only push
	 * was at the very end the screenshot caught the loader's splash: a verified picture
	 * existed in this process's memory and never reached the screen. */
	pic_open_panel();

	pic_mosaic(dev, library, queue);
	pic_push("the mosaic -- 64 of Apple's fragment shaders, drawn on the Adreno");
	printf("drvtest: picture: the mosaic is done; now the ladder, then the two bands\n");
	fflush(stdout);
	/* From here on a second thread says, every so often, that this process is alive and
	 * which step it is in -- so the next silence means something definite. */
	pic_watch();
	pic_ladder(dev, library, queue);
	pic_bands(dev, library, queue);
	pic_alive = 0;
	pic_count_bands();
	pic_show(seconds);
}
