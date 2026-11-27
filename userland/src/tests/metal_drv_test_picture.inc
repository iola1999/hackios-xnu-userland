/* Fault handler, draw_every, and picture mosaic/ladder tests, included into metal_drv_test.m. */


/*
 * Where it died, in terms of the image it died in.
 *
 * A program that faults on this port leaves nothing behind -- the kernel prints "Failed to
 * send exception EXC_CORPSE_NOTIFY" and that is all -- so the fault has to print itself. The
 * addresses that matter here are not in the shared cache but inside
 * /annibale/lib/libannibaleair.dylib, so the slide trick the other probes use (docs/metal.md
 * section 6) is no help: dladdr gives the image and the offset within it, which
 * llvm-addr2line on the machine turns back into a line.
 */
static void
fault(int sig, siginfo_t *info, void *uap)
{
	ucontext_t *uc = uap;
	uintptr_t pc = 0, lr = 0, fp = 0;

	if (uc != NULL && uc->uc_mcontext != NULL) {
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
		lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
		fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
	}
	printf("drvtest: SIGNAL %d at %p\n", sig, info != NULL ? info->si_addr : NULL);
	for (int i = 0; i < 10; i++) {
		uintptr_t at = i == 0 ? pc : i == 1 ? lr : 0;
		Dl_info d;

		if (i >= 2) {
			if (fp <= 0x1000) {
				break;
			}
			at = ((uintptr_t *)fp)[1];
			fp = ((uintptr_t *)fp)[0];
			if (at == 0) {
				break;
			}
		}
		if (dladdr((void *)at, &d) != 0 && d.dli_fname != NULL) {
			const char *slash = strrchr(d.dli_fname, '/');

			printf("drvtest:   %-2s %s+%#lx  (%s)\n", i == 0 ? "pc" : i == 1 ? "lr" : "",
			       slash != NULL ? slash + 1 : d.dli_fname,
			       at - (uintptr_t)d.dli_fbase,
			       d.dli_sname != NULL ? d.dli_sname : "no symbol");
		} else {
			printf("drvtest:   %-2s %#lx (in no image)\n",
			       i == 0 ? "pc" : i == 1 ? "lr" : "", at);
		}
	}
	fflush(stdout);
	_exit(sig);
}

/*
 * Every fragment shader of a library, drawn, one line each.
 *
 * One shader drawn is an example; 255 of them have a reference each (docs/air.md: air_run
 * draws every fragment shader of the corpus and the pixel was compared), so drawing them all
 * turns an example into coverage. One process does the whole library because the loader keeps
 * only the first 256 KB and the last 1 MB of a boot's log: 271 processes would lose the middle
 * of their own table, and a count read off a cut log is a lower bound pretending to be a
 * number.
 *
 * The line is deliberately terse and machine-readable -- name, status, four words -- because
 * the comparison belongs to tools/air/metalcheck.py, which produces each reference by running
 * air_run rather than trusting anything written here.
 *
 * Each shader is bound the textures IT declares, asked of the compiler library through
 * rwa_module_textures. The run before this one bound a single kind and format for the whole
 * library and 13 of Apple's 271 fragment shaders were refused by name for declaring a
 * texture3d, a texture2d<int> or a multisample texture: a true sentence about this harness
 * and nothing about those shaders. A slot no shader named keeps the single binding, so the
 * 256 shaders that declare no texture are bound what they were bound before and their pixels
 * stay comparable with that run's.
 */
static void
draw_every(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
	   MTLRenderPassDescriptor *rp, id<MTLBuffer> uniform, id<MTLTexture> *dtex,
	   id<MTLSamplerState> dsamp, id<MTLTexture> *targets)
{
	unsigned drew = 0, refused = 0, nopipe = 0, notex = 0, asked = 0;

	unsigned seen = 0;

	for (NSString *name in [library functionNames]) {
		id<MTLFunction> fn = [library newFunctionWithName:name];
		NSError *err = nil;

		if (fn == nil || [fn functionType] != MTLFunctionTypeFragment) {
			continue;
		}
		if (seen++ < draw_from || drew + refused + nopipe + notex >= draw_count) {
			continue;
		}
		/*
		 * What THIS shader declares, made for it, before anything is bound. The slots
		 * it does not name keep the sweep's own dtex[], so the 256 shaders that
		 * declare no texture at all are bound exactly what they were bound before and
		 * their numbers stay comparable with the previous run's.
		 */
		id<MTLTexture> own[RW_TEX_SLOTS] = { nil };
		unsigned ndecl = 0;
		char whynot[256] = "";

		if (use_decl && !textures_for_shader(dev, fn, own, RW_TEX_SLOTS, &ndecl, whynot,
						     sizeof(whynot))) {
			notex++;
			printf("drvtest: drawall %-34s no texture: %s\n", [name UTF8String],
			       whynot);
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		if (ndecl > 0) {
			asked++;
		}
		/* The name BEFORE the work and not only in the row after it.
		 *
		 * The first sweep with per-shader textures stopped dead at its third shader
		 * (logs/070755-run.log): two multisample refusals, then one line from the
		 * driver about a generated vertex stage, then nothing for the rest of the
		 * boot. Everything about which shader that was had to be inferred, and
		 * -[_MTLLibrary functionNames] is not in container order, so it could not be
		 * inferred from the index either. A line here costs one line per shader and
		 * makes the next hang name itself.
		 */
		printf("drvtest: drawall %-34s about to draw, %u declared texture%s\n",
		       [name UTF8String], ndecl, ndecl == 1 ? "" : "s");
		fflush(stdout);
		MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

		[pd setFragmentFunction:fn];
		[pd setVertexFunction:nil];
		for (int i = 0; i < 3; i++) {
			[[[pd colorAttachments] objectAtIndexedSubscript:i]
			    setPixelFormat:MTLPixelFormatRGBA32Uint];
		}
		id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
										   error:&err];
		[pd release];
		if (ps == nil) {
			nopipe++;
			printf("drvtest: drawall %-34s no pipeline: %s\n", [name UTF8String],
			       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			[ps release];
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		[enc setRenderPipelineState:ps];
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:uniform offset:0 atIndex:i];
			/* this shader's own texture where it declared one, and the sweep's
			 * single binding where it did not: a slot nobody named is a slot
			 * nothing reads, and leaving the old binding there keeps the 256
			 * shaders that declare none bound exactly as they were */
			if (i < RW_TEX_SLOTS && own[i] != nil) {
				[enc setFragmentTexture:own[i] atIndex:i];
			} else if (i < 4 && dtex[i] != nil) {
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

		if ([cmd status] != RW_CB_COMPLETED) {
			refused++;
			printf("drvtest: drawall %-34s status %lu %s\n", [name UTF8String],
			       (unsigned long)[cmd status],
			       [cmd error] != nil ?
			       [[[cmd error] localizedDescription] UTF8String] : "");
			[ps release];
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		/* All three attachments, because each shader writes the targets IT declares:
		 * some write target 0, some 1, some 2, and reading only one of them made 50 of
		 * 64 shaders uncomparable with "air_run printed no pixel for render target 2".
		 * Which target to compare is then the reference's business, not the harness's. */
		drew++;
		printf("drvtest: drawall %-34s status %d", [name UTF8String], RW_CB_COMPLETED);
		for (int t = 0; t < 3; t++) {
			unsigned pixel[4] = { 0 };

			[targets[t] getBytes:pixel
				bytesPerRow:4 * 4 * 4
				 fromRegion:MTLRegionMake2D(0, 0, 1, 1)
				mipmapLevel:0];
			printf(" t%d %u %u %u %u", t, pixel[0], pixel[1], pixel[2], pixel[3]);
		}
		/* What was bound, on the same line as the pixel: a row of this table is only
		 * a measurement if what it was measured against is in it, and metalcheck.py
		 * has to know which shape air_run should hold to produce the reference. */
		if (ndecl > 0) {
			printf(" tex");
			for (unsigned k = 0; k < RW_TEX_SLOTS; k++) {
				if (own[k] != nil) {
					printf(" %u=%lu:%lu", k,
					       (unsigned long)[own[k] textureType],
					       (unsigned long)[own[k] pixelFormat]);
				}
			}
		}
		printf("\n");
		[ps release];
		release_slots(own, RW_TEX_SLOTS);
	}
	printf("drvtest: drawall: %u drew, %u refused at the draw, %u got no pipeline, "
	       "%u could not be given the textures they declare (of the %u fragment shaders "
	       "from index %u; %u declared a texture and were bound what they declared)\n",
	       drew, refused, nopipe, notex, seen, draw_from, asked);
	fflush(stdout);
}

/*
 * --picture: the same shaders, at the size of the phone's panel, where a person can look
 * at them.
 *
 * Everything this driver has drawn so far went into a 4x4 off-screen attachment and came
 * back as four numbers. That is a measurement and it is not a picture, and they are not
 * the same claim: 4x4 says nothing about an attachment a megapixel wide, about a varying
 * that has to interpolate over a thousand pixels instead of four, or about a sampler
 * asked for a texel between two texels rather than the one a coordinate lands on.
 *
 * What is drawn, in three bands that together are exactly the panel (1080x2400 -- the
 * size the kernel prints at boot: "display 1125x2436 on a panel of 1080x2400"):
 *
 *   rows 0..1103      64 of Apple's own clearFragment shaders, one per square of an 8x8
 *                     mosaic of 135x138 tiles. The number in the name is a mask of two
 *                     bits per render target -- 0 none, 1 float4, 2 int4, 3 uint4 -- so
 *                     the 64 numbered 4k+1 are exactly those that write a float4 to
 *                     target 0, and each reads that float4 out of the constant buffer at
 *                     index 14 (its ClearFragmentInput is { float4, int4, uint4 } and
 *                     every float4 output is the first field). Each square is given a
 *                     colour of its own in that buffer, so a square is one shader's
 *                     answer and the mosaic is 64 of them side by side.
 *   rows 1104..1751   Apple's texturedVertex2D + texturedFragment2DFloat -- the pipeline
 *                     AppleMetalGLRenderer blits a texture with -- sampling an 8x8
 *                     texture over the whole 1080x648 attachment through a LINEAR
 *                     sampler. Every pixel of it is an interpolated texture coordinate
 *                     and a filtered texel, which is the one thing a 4x4 draw over a
 *                     constant coordinate cannot show at all (docs/metal.md 10.32).
 *   rows 1752..2399   the same draw with a NEAREST sampler. Blocky where the band above
 *                     is smooth, and that difference is the sampler state reaching the
 *                     hardware; it is also the band in which every pixel is exactly one
 *                     texel, so every pixel of it can be counted.
 *
 * The pixels reach the panel through the kernel's own GPU service -- kAnnibaleShow,
 * iokit/Kernel/AnnibaleGPU.cpp, the selector src/gl_test.c already puts its triangle on the
 * screen with -- and that is what `./xl screenshot` reads. Said plainly because it
 * matters: THE PRESENTATION IS A COPY IN THE KERNEL AND IS NOT METAL'S. What is claimed
 * is that every one of the 2 592 000 pixels handed over was computed by this driver's
 * Metal pipelines on the Adreno. Nothing here is drawn by the processor -- no border, no
 * label, no background -- so that the claim needs no exceptions.
 *
 * And the numbers beside it, because "it looks right" is not a measurement:
 *
 *   - the mosaic: every pixel of every tile against the colour that tile's buffer named,
 *     and every one of the 256 (shader, render target) pairs against what the shader's
 *     own name says it writes -- a target the name does not list must still hold the
 *     clear, which is the half a chart of colours alone would not say. Integer targets
 *     are not checked and are counted as not checked;
 *   - the nearest band: every pixel against the texel the sampler must have returned for
 *     it, floor(x / 135) and floor(y / 81);
 *   - the linear band: the 64 pixels that fall exactly on a texel centre. The band is 8
 *     texels over 1080 columns and 8 over 648 rows, so the scales are 135 and 81, both
 *     ODD -- and that is why a pixel centre can coincide with a texel centre at all
 *     (pixel p is texel centre i when p = s*i + (s-1)/2), which leaves the filter no
 *     weight to round there. Chosen, not noticed.
 *
 * All of it was drawn on this machine first, through the same library on softpipe, so
 * every count below has a reference and not an expectation.
 */
enum {
	/* The panel, not the display. A swap through IOMobileFramebuffer is scaled from the
	 * 1125x2436 user space sees down to this; kAnnibaleShow is not scaled at all -- it is
	 * a row-by-row copy cut to the panel -- so this is the size to draw. */
	PANEL_W = 1080, PANEL_H = 2400,
	PIC_TILES = 8,
	PIC_TILE_W = PANEL_W / PIC_TILES,               /* 135 */
	PIC_TILE_H = 138,                               /* so eight rows of tiles are 1104, */
	PIC_MOSAIC_H = PIC_TILES * PIC_TILE_H,          /* and 1104 + 648 + 648 is 2400 */
	PIC_BAND_H = (PANEL_H - PIC_MOSAIC_H) / 2,      /* exactly: no row of the panel is */
	PIC_LINEAR_Y = PIC_MOSAIC_H,                    /* left for the processor to fill */
	PIC_NEAREST_Y = PIC_MOSAIC_H + PIC_BAND_H,
	PIC_TEX = 8,
	PIC_SX = PANEL_W / PIC_TEX,                     /* 135, odd */
	PIC_SY = PIC_BAND_H / PIC_TEX,                  /* 81, odd */
};

/* Eight levels of a channel, each an exact byte. A texel written as k/255 comes back as
 * exactly k through an 8-bit unorm attachment, so no check here needs a tolerance and a
 * difference of one is a difference. */
static const unsigned char pic_ramp[PIC_TILES] = { 0, 36, 73, 109, 146, 182, 219, 255 };

/*
 * The colour of mosaic tile (i, j) and of texel (x, y), in the panel's own order: blue
 * first. The four corners of each are four different primaries on purpose -- a channel
 * swapped anywhere between the shader and the panel, or a picture mirrored, moves which
 * corner is which, and that is seen before it is counted.
 */
static void
pic_mosaic_colour(unsigned i, unsigned j, unsigned char bgra[4])
{
	bgra[0] = pic_ramp[PIC_TILES - 1 - i];
	bgra[1] = pic_ramp[j];
	bgra[2] = pic_ramp[i];
	bgra[3] = 255;
}

static void
pic_texel_colour(unsigned x, unsigned y, unsigned char bgra[4])
{
	bgra[0] = pic_ramp[y];
	bgra[1] = pic_ramp[PIC_TILES - 1 - y];
	bgra[2] = pic_ramp[x];
	bgra[3] = 255;
}

/* The panel's pixels as this process holds them, and the request the kernel takes them in
 * (iokit/Kernel/AnnibaleGPU.cpp, struct annibale_show). Declared here and not included,
 * because this program has no header of the kernel's -- exactly as src/gl_test.c does. */
static unsigned char *pic_panel;

struct pic_show_request {
	uint64_t pixels;
	uint32_t width, height, row_bytes, pad;
};

/*
 * The connection to that service, opened BEFORE anything is drawn and used after every
 * stage that finishes, rather than once at the end.
 *
 * That order is not tidiness, it is what the first boot cost (logs/101536-run.log): the
 * mosaic drew and counted perfectly on the Adreno and then the process stopped dead on
 * its way into the bands, so nothing ever reached the panel and the screenshot caught the
 * loader's own splash. A stage that finished is a picture; it belongs on the screen the
 * moment it is finished, and a stall after it should cost the bands and not the
 * screenshot. The copy persists by itself -- MiniIOMobileFramebufferShow writes the boot
 * framebuffer and, with JOBS="" , nothing else touches it -- so one push is enough to be
 * photographed even if the process never reaches the repainting loop at the end.
 */
static io_connect_t pic_connection;

static void
pic_open_panel(void)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("AnnibaleGPU"));

	if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &pic_connection) != 0) {
		pic_connection = 0;
		printf("drvtest: picture: no GPU service in this kernel to put the picture on "
		       "the panel with; the counts below still stand but nothing will be "
		       "seen\n");
	} else {
		printf("drvtest: picture: the panel is open; every stage goes on the screen as "
		       "it is finished\n");
	}
	fflush(stdout);
}

static void
pic_push(const char *what)
{
	struct pic_show_request show = {
		(uint64_t)(uintptr_t)pic_panel, PANEL_W, PANEL_H, PANEL_W * 4, 0
	};

	if (pic_connection == 0 || pic_panel == NULL) {
		return;
	}
	kern_return_t r = IOConnectCallStructMethod(pic_connection, 0x7f, &show, sizeof(show),
						    NULL, NULL);

	printf("drvtest: picture: ON THE PANEL (%#x): %s. A screenshot taken now has it\n", r,
	       what);
	fflush(stdout);
}

static uint64_t
pic_now_ms(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000000;
}

/*
 * Every step of the band stage says its own name before it starts, and how long the step
 * before it took.
 *
 * The first boot of this mode had NO line at all between the mosaic's summary and the
 * kernel's idle heartbeat, because on this path nothing prints unless something fails
 * (logs/101536-run.log) -- so the stall had the whole stage to hide in. The second boot,
 * with these lines, put it between two statements: "the probe: asking for an encoder" was
 * the last thing printed. The timestamp is here because that boot still could not say
 * whether the step was stuck or merely slow, and those are different bugs.
 */
static char pic_where[192] = "before anything";
static volatile uint64_t pic_since_ms;
static volatile int pic_alive = 1;

static void
pic_step(const char *what)
{
	uint64_t now = pic_now_ms(), took = pic_since_ms ? now - pic_since_ms : 0;

	printf("drvtest: picture: band step: %s   (the step before took %llu ms)\n", what,
	       (unsigned long long)took);
	fflush(stdout);
	snprintf(pic_where, sizeof(pic_where), "%s", what);
	pic_since_ms = now;
}

/*
 * And a thread that says the process is still alive and still in that step.
 *
 * Silence has meant three different things so far -- blocked, killed, and simply not
 * printing -- and nothing in the log told them apart. This separates two of the three: as
 * long as these lines keep coming, the process is alive and the step named in them has not
 * returned; when they stop, the process is gone. It writes with write(2) and not printf on
 * purpose: if the main thread is stuck holding stdio's lock, a printf here would block too
 * and the one case this is for would be the one case it could not report.
 *
 * It backs off by doubling -- 2, 4, 8, 16 ... seconds -- because the loader keeps the TAIL
 * of a long log, and a line a second for four minutes would push the measurements that
 * matter out of it.
 */
static void *
pic_heartbeat(void *unused)
{
	uint64_t next = 2000, watching = 0;

	(void)unused;
	for (;;) {
		char line[320];
		uint64_t in;
		int n;

		usleep(250000);
		if (!pic_alive) {
			return NULL;
		}
		if (pic_since_ms != watching) {
			watching = pic_since_ms;   /* a new step: count from it, not from the */
			next = 2000;               /* first slow one */
		}
		in = pic_now_ms() - pic_since_ms;
		if (in < next) {
			continue;
		}
		next *= 2;
		n = snprintf(line, sizeof(line), "drvtest: picture: STILL IN \"%s\" after "
			     "%llu ms -- this process is alive and that step has not "
			     "returned\n", pic_where, (unsigned long long)in);
		if (n > 0) {
			ssize_t written = write(1, line, (size_t)n);

			(void)written;
		}
	}
}

/*
 * What the QUEUE says about itself, rather than what this file infers.
 *
 * The second ladder named the stall as -[MTLCommandQueue commandBuffer] on the 65th
 * command buffer of the process, and 64 was Metal's own maxCommandBufferCount: every
 * _MTLCommandBuffer waits on the queue's semaphore in its designated initialiser and owes
 * it a signal, and this driver never paid (annibalemetal.m). That was an inference from a
 * number that happened to be 64. These two lines are the measurement instead: the queue
 * reports how many buffers it takes at a time and how many of this process's still hold a
 * slot, and it says OUT LOUD when the next one will block -- before it does.
 */
static unsigned long
pic_queue_state(id<MTLCommandQueue> queue, const char *when)
{
	NSUInteger (*ask)(id, SEL) = (NSUInteger (*)(id, SEL))objc_msgSend;
	SEL outstanding = sel_getUid("annibaleOutstandingCommandBuffers");
	SEL made = sel_getUid("annibaleCommandBuffersMade");
	SEL most = sel_getUid("maxCommandBufferCount");

	if (![queue respondsToSelector:outstanding] || ![queue respondsToSelector:most]) {
		printf("drvtest: picture: %s: this queue does not say how many command buffers "
		       "hold a slot, so the next block would again have to be inferred\n", when);
		fflush(stdout);
		return ~0UL;
	}
	unsigned long o = (unsigned long)ask(queue, outstanding);
	unsigned long m = (unsigned long)ask(queue, most);
	unsigned long n = [queue respondsToSelector:made] ?
	    (unsigned long)ask(queue, made) : 0;

	printf("drvtest: picture: %s: the queue takes %lu command buffers at a time, %lu "
	       "made so far, %lu still holding a slot%s\n", when, m, n, o,
	       m != 0 && o >= m ? " -- THE NEXT -commandBuffer WILL BLOCK FOR EVER" : "");
	fflush(stdout);
	return o;
}

/* The backing-off clock starts again at every step, so "STILL IN" counts from the step it
 * names and not from the first slow one. */
static void
pic_watch(void)
{
	pthread_t thread;

	pic_since_ms = pic_now_ms();
	if (pthread_create(&thread, NULL, pic_heartbeat, NULL) == 0) {
		pthread_detach(thread);
		printf("drvtest: picture: a second thread will say every so often that this "
		       "process is alive and which step it is in\n");
	} else {
		printf("drvtest: picture: no second thread, so silence below will again mean "
		       "either blocked or killed\n");
	}
	fflush(stdout);
}

/*
 * Four attachments, all BGRA8Unorm and all one size: the mosaic's shaders write up to
 * four render targets and Metal requires a pass's formats to be the pipeline's, so four
 * it is for every pipeline here. BGRA8Unorm because that is the panel's own order, so
 * what -getBytes: hands back is what the panel takes, with nothing swapped in between.
 */
static bool
pic_targets(id<MTLDevice> dev, id<MTLTexture> *t, unsigned n, unsigned w, unsigned h,
	    MTLRenderPassDescriptor **rp)
{
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:w
							      height:h
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	*rp = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
	for (unsigned i = 0; i < n; i++) {
		t[i] = [dev newTextureWithDescriptor:td];
		if (t[i] == nil) {
			printf("drvtest: picture: attachment %u of %ux%u could not be made\n",
			       i, w, h);
			return false;
		}
		MTLRenderPassColorAttachmentDescriptor *a =
		    [[*rp colorAttachments] objectAtIndexedSubscript:i];

		[a setTexture:t[i]];
		[a setLoadAction:MTLLoadActionClear];
		/* opaque black: a tile nothing drew is black on the panel and black in the
		 * count, and the two then agree about which tile it was */
		[a setClearColor:MTLClearColorMake(0, 0, 0, 1)];
		[a setStoreAction:MTLStoreActionStore];
	}
	return true;
}

static void
pic_release_targets(id<MTLTexture> *t, unsigned n, MTLRenderPassDescriptor *rp)
{
	for (unsigned i = 0; i < n; i++) {
		[t[i] release];
		t[i] = nil;
	}
	[rp release];
}

static id<MTLRenderPipelineState>
pic_pipeline(id<MTLDevice> dev, id<MTLFunction> vertex, id<MTLFunction> fragment,
	     unsigned n, const char *what)
{
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	NSError *err = nil;

	[pd setVertexFunction:vertex];
	[pd setFragmentFunction:fragment];
	for (unsigned i = 0; i < n; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatBGRA8Unorm];
	}
	id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
									    error:&err];
	[pd release];
	if (ps == nil) {
		printf("drvtest: picture: %s got no pipeline: %s\n", what,
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
	}
	return ps;
}

/*
 * One draw into the pass, waited on. True when the GPU finished it.
 *
 * `trace` says every part of it out loud, which the band stage needs and the mosaic's 64
 * draws do not: this driver's command buffer RECORDS the draw and performs it on -commit
 * (10.13), so encoding, committing and waiting are three different places to stop and the
 * log has to tell them apart. The mosaic passes false so that 64 working draws do not bury
 * the one that does not.
 */
static bool
pic_draw(id<MTLCommandQueue> queue, MTLRenderPassDescriptor *rp,
	 id<MTLRenderPipelineState> ps, id<MTLBuffer> *vbuf, unsigned nvbuf,
	 id<MTLBuffer> fbuf, unsigned nfbuf, id<MTLTexture> tex, id<MTLSamplerState> samp,
	 const char *what, bool trace)
{
	char line[192];

	/* The second boot stopped between these two calls and the one line covered both, so
	 * they are two lines now. Neither of them has anything in it proportional to the
	 * attachment's area -- -commandBuffer allocates an object and an NSMutableArray, and
	 * the encoder's init reads four properties per attachment (annibalemetal.m) -- which
	 * is exactly why the two have to be told apart before anything else is believed. */
	if (trace) {
		snprintf(line, sizeof(line), "%s: asking the queue for a command buffer", what);
		pic_step(line);
	}
	id<MTLCommandBuffer> cmd = [queue commandBuffer];

	if (trace) {
		snprintf(line, sizeof(line), "%s: got the command buffer; asking IT for a "
			 "render encoder", what);
		pic_step(line);
	}
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	if (trace) {
		snprintf(line, sizeof(line), "%s: got the encoder", what);
		pic_step(line);
	}

	if (enc == nil) {
		printf("drvtest: picture: %s got no encoder\n", what);
		return false;
	}
	[enc setRenderPipelineState:ps];
	for (unsigned i = 0; i < nfbuf; i++) {
		[enc setFragmentBuffer:fbuf offset:0 atIndex:i];
	}
	for (unsigned i = 0; i < nvbuf; i++) {
		[enc setVertexBuffer:vbuf[i] offset:0 atIndex:i];
	}
	if (tex != nil) {
		[enc setFragmentTexture:tex atIndex:0];
	}
	if (samp != nil) {
		[enc setFragmentSamplerState:samp atIndex:0];
	}
	if (trace) {
		snprintf(line, sizeof(line), "%s: bound; encoding the quad", what);
		pic_step(line);
	}
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[enc endEncoding];
	if (trace) {
		snprintf(line, sizeof(line), "%s: encoded; committing (this driver does the "
			 "work here, not at the wait)", what);
		pic_step(line);
	}
	[cmd commit];
	if (trace) {
		snprintf(line, sizeof(line), "%s: committed; waiting", what);
		pic_step(line);
	}
	[cmd waitUntilCompleted];
	if ([cmd status] != RW_CB_COMPLETED) {
		printf("drvtest: picture: %s: status %lu%s%s\n", what,
		       (unsigned long)[cmd status], [cmd error] != nil ? ", " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String]
					  : "");
		return false;
	}
	return true;
}

/* One rectangle of an attachment into the panel's buffer at (x0, y0). */
static void
pic_blit(const unsigned char *from, unsigned w, unsigned h, unsigned x0, unsigned y0)
{
	for (unsigned y = 0; y < h; y++) {
		memcpy(pic_panel + ((size_t)(y0 + y) * PANEL_W + x0) * 4,
		       from + (size_t)y * w * 4, (size_t)w * 4);
	}
}

/* The mosaic: 64 of Apple's clearFragment shaders, each into its own square. */
static void
pic_mosaic(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	id<MTLTexture> mt[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *mrp = nil;

	if (!pic_targets(dev, mt, 4, PIC_TILE_W, PIC_TILE_H, &mrp)) {
		pic_release_targets(mt, 4, mrp);
		return;
	}
	/* The constant buffer every clearFragment reads: declared at index 14, bound at
	 * every index up to 16 for draw_path's reason -- a driver that bound only index 0
	 * would leave the shader reading nothing. Its first float4 is the square's colour
	 * and is rewritten before each draw, which is safe because each draw is waited on
	 * before the next is encoded. */
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];
	unsigned char *tile = malloc((size_t)PIC_TILE_W * PIC_TILE_H * 4);

	if (uniform == nil || [uniform contents] == NULL || tile == NULL) {
		printf("drvtest: picture: no constant buffer or no memory for a tile\n");
		pic_release_targets(mt, 4, mrp);
		free(tile);
		return;
	}
	unsigned drew = 0, nopipe = 0, nodraw = 0;
	unsigned long rt_colour = 0, rt_clear = 0, rt_skipped = 0, rt_wrong = 0;
	unsigned long bad_mosaic = 0;

	for (unsigned k = 0; k < PIC_TILES * PIC_TILES; k++) {
		unsigned nn = 4 * k + 1, i = k % PIC_TILES, j = k / PIC_TILES;
		char name[32];

		snprintf(name, sizeof(name), "clearFragment%02X", nn);
		id<MTLFunction> fn = [library newFunctionWithName:
		    [NSString stringWithUTF8String:name]];

		if (fn == nil) {
			printf("drvtest: picture: no %s in that library\n", name);
			nopipe++;
			continue;
		}
		id<MTLRenderPipelineState> ps = pic_pipeline(dev, nil, fn, 4, name);

		if (ps == nil) {
			nopipe++;
			[fn release];
			continue;
		}
		unsigned char want[4];

		pic_mosaic_colour(i, j, want);
		float *u = [uniform contents];

		/* zeroed first: the int4 and uint4 fields of ClearFragmentInput are read by
		 * the integer targets this check skips, and memory nobody set would make
		 * "not checked" mean two things at once */
		memset(u, 0, 256);
		u[0] = want[2] / 255.0f;        /* the shader's float4 is r, g, b, a */
		u[1] = want[1] / 255.0f;
		u[2] = want[0] / 255.0f;
		u[3] = 1.0f;
		if (!pic_draw(queue, mrp, ps, NULL, 0, uniform, 16, nil, nil, name, false)) {
			nodraw++;
			[ps release];
			[fn release];
			continue;
		}
		drew++;
		/* Each of this shader's four targets against what its own name says it
		 * writes. A target the name does not list must still hold the clear. */
		for (unsigned t = 0; t < 4; t++) {
			unsigned kind = (nn >> (2 * t)) & 3;
			unsigned char expect[4] = { 0, 0, 0, 255 };
			unsigned long bad = 0;

			if (kind == 2 || kind == 3) {
				rt_skipped++;
				continue;
			}
			if (kind == 1) {
				memcpy(expect, want, 4);
			}
			[mt[t] getBytes:tile
			    bytesPerRow:PIC_TILE_W * 4
			     fromRegion:MTLRegionMake2D(0, 0, PIC_TILE_W, PIC_TILE_H)
			    mipmapLevel:0];
			for (size_t q = 0; q < (size_t)PIC_TILE_W * PIC_TILE_H; q++) {
				if (tile[q * 4] != expect[0] || tile[q * 4 + 1] != expect[1] ||
				    tile[q * 4 + 2] != expect[2]) {
					bad++;
				}
			}
			if (bad != 0) {
				rt_wrong++;
				if (rt_wrong <= 6) {
					printf("drvtest: picture: %s target %u (%s): %lu of "
					       "%u pixels wrong\n", name, t,
					       kind != 0 ? "its own colour"
							 : "not written, so the clear",
					       bad, PIC_TILE_W * PIC_TILE_H);
				}
			} else if (kind != 0) {
				rt_colour++;
			} else {
				rt_clear++;
			}
			if (t == 0) {
				bad_mosaic += bad;
				pic_blit(tile, PIC_TILE_W, PIC_TILE_H, i * PIC_TILE_W,
					 j * PIC_TILE_H);
			}
		}
		[ps release];
		[fn release];
	}
	free(tile);
	pic_release_targets(mt, 4, mrp);
	printf("drvtest: picture: mosaic: %u of %u shaders drew (%u got no pipeline, %u did "
	       "not finish)\n", drew, PIC_TILES * PIC_TILES, nopipe, nodraw);
	printf("drvtest: picture: mosaic: %lu pixels of %u are not the colour their square's "
	       "buffer named\n", bad_mosaic,
	       PIC_TILES * PIC_TILES * PIC_TILE_W * PIC_TILE_H);
	printf("drvtest: picture: mosaic: of %u (shader, render target) pairs, %lu held the "
	       "colour, %lu stayed at the clear, %lu are integer targets and not checked, "
	       "%lu are WRONG\n", 4 * PIC_TILES * PIC_TILES, rt_colour, rt_clear, rt_skipped,
	       rt_wrong);
	fflush(stdout);
}

/*
 * One named experiment: N attachments of W by H, a pipeline for clearFragment01 -- the
 * shader the mosaic has already drawn 64 times on this GPU -- an encoder, a draw and a
 * readback, every one of them saying its own name first.
 *
 * Four of these run in a ladder before Apple's blit, and between them they name the axis
 * the stall is on. The second boot (logs/101536-run.log's successor) stopped between
 * -commandBuffer and -renderCommandEncoderWithDescriptor: on four 1080x648 attachments,
 * and reading the driver afterwards found NOTHING in either call proportional to the
 * attachment's area: -commandBuffer allocates an object and an NSMutableArray, and the
 * encoder's init reads loadAction, storeAction, resolveTexture and clearColor per
 * attachment and nothing else. So the obvious story -- "a megapixel attachment is too
 * big" -- is not supported by the code, and the ladder is built to find out what is true
 * instead of assuming it:
 *
 *   again-small   four 135x138, the mosaic's own shape, once more. If THIS stalls, size
 *                 has nothing to do with it: what changed is that it is the 65th command
 *                 buffer of the process, with 64 encoders, 64 pipelines and 64 command
 *                 buffers behind it that nothing has freed (this file has no autorelease
 *                 pool, deliberately: the mosaic's objects are small and the leak is
 *                 bounded).
 *   one-big       ONE 1080x648. Size with no count.
 *   four-medium   four 540x324 -- 2.8 MB of attachment against four-big's 11.2 MB, and
 *                 the same count. Count with a quarter of the area.
 *   four-big      four 1080x648, the one that stalled.
 *
 * The first rung that does not finish is the answer, and the heartbeat thread says
 * whether it is stuck or merely slow while it is happening.
 */
static bool
pic_probe(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
	  unsigned n, unsigned w, unsigned h, bool full, const char *name)
{
	/* the colour, three different levels of pic_ramp so that a channel in the wrong
	 * place is seen, each an exact byte so the count needs no tolerance */
	const unsigned char want[4] = { 109, 182, 73, 255 };   /* b, g, r, a */
	id<MTLTexture> t[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *rp = nil;
	id<MTLFunction> fn = nil;
	id<MTLRenderPipelineState> ps = nil;
	id<MTLBuffer> u = nil;
	unsigned char *back = NULL;
	bool ok = false;
	char line[192];

	snprintf(line, sizeof(line), "probe %s: %u attachment%s of %ux%u (%lu KB of "
		 "attachment)", name, n, n == 1 ? "" : "s", w, h,
		 (unsigned long)((size_t)n * w * h * 4 / 1024));
	pic_step(line);
	if (!pic_targets(dev, t, n, w, h, &rp)) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the attachments are made", name);
	pic_step(line);

	fn = [library newFunctionWithName:@"clearFragment01"];
	if (fn == nil) {
		printf("drvtest: picture: probe %s: no clearFragment01\n", name);
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the pipeline", name);
	pic_step(line);
	ps = pic_pipeline(dev, nil, fn, n, name);
	if (ps == nil) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the constant buffer", name);
	pic_step(line);
	u = [dev newBufferWithLength:256 options:0];
	if (u == nil || [u contents] == NULL) {
		printf("drvtest: picture: probe %s: no constant buffer\n", name);
		goto done;
	}
	{
		float *f = [u contents];

		memset(f, 0, 256);
		f[0] = want[2] / 255.0f;
		f[1] = want[1] / 255.0f;
		f[2] = want[0] / 255.0f;
		f[3] = 1.0f;
	}
	snprintf(line, sizeof(line), "probe %s, before it asks for a command buffer", name);
	pic_queue_state(queue, line);
	snprintf(line, sizeof(line), "probe %s", name);
	if (!pic_draw(queue, rp, ps, NULL, 0, u, 16, nil, nil, line, true)) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: drew; reading ONE pixel back", name);
	pic_step(line);
	{
		unsigned char one[4] = { 0, 0, 0, 0 };

		[t[0] getBytes:one
		    bytesPerRow:4
		     fromRegion:MTLRegionMake2D(0, 0, 1, 1)
		    mipmapLevel:0];
		printf("drvtest: picture: probe %s: pixel (0,0) is r g b %u %u %u and must be "
		       "%u %u %u\n", name, one[2], one[1], one[0], want[2], want[1], want[0]);
		fflush(stdout);
		ok = one[0] == want[0] && one[1] == want[1] && one[2] == want[2];
	}
	if (!full) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the whole %ux%u back in one getBytes:", name,
		 w, h);
	pic_step(line);
	back = malloc((size_t)w * h * 4);
	if (back == NULL) {
		printf("drvtest: picture: probe %s: no memory to read %ux%u into\n", name, w, h);
		goto done;
	}
	memset(back, 0, (size_t)w * h * 4);
	[t[0] getBytes:back
	    bytesPerRow:w * 4
	     fromRegion:MTLRegionMake2D(0, 0, w, h)
	    mipmapLevel:0];
	{
		unsigned long bad = 0;

		for (size_t q = 0; q < (size_t)w * h; q++) {
			if (back[q * 4] != want[0] || back[q * 4 + 1] != want[1] ||
			    back[q * 4 + 2] != want[2]) {
				bad++;
			}
		}
		printf("drvtest: picture: probe %s: %lu pixels of %u are not that colour, so a "
		       "%ux%u render target draws and reads back %s\n", name, bad, w * h, w, h,
		       bad == 0 ? "WHOLE" : "WRONG");
		fflush(stdout);
		ok = ok && bad == 0;
	}
done:
	free(back);
	[u release];
	[ps release];
	[fn release];
	pic_release_targets(t, n, rp);
	snprintf(line, sizeof(line), "probe %s: finished, and its resources are freed", name);
	pic_step(line);
	return ok;
}

/*
 * The ladder, in the order that makes the first failure mean the most.
 *
 * Each rung frees everything it made before the next begins, so a rung is about its own
 * shape and not about what the rungs before it are still holding.
 */
static void
pic_ladder(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	pic_step("THE LADDER: four experiments that separate count from size. The first one "
		 "that does not finish is the answer");
	/*
	 * And the verdict on the bug the picture found, named so that a later change cannot
	 * quietly lose it: the mosaic has just made 64 command buffers and this test keeps
	 * every one of them alive ON PURPOSE -- there is no autorelease pool round its loop,
	 * and there must not be one. With a pool, each buffer would be deallocated at the
	 * end of its iteration and the slot would come back from -dealloc, so this would
	 * pass whether or not -commit returns it, which is the whole failure the driver had.
	 * Sixty-four live command buffers holding no slot is the measurement.
	 */
	unsigned long held = pic_queue_state(queue, "after the mosaic's 64 command buffers");

	if (held != ~0UL) {
		printf("drvtest: picture: the mosaic made 64 command buffers and %lu of them "
		       "still hold a queue slot: %s\n", held,
		       held == 0 ? "the slot IS given back at -commit, so the next command "
				   "buffer will not block" :
				   "the slot is NOT given back, and the next command buffer "
				   "will block for ever (this is the driver bug the picture "
				   "found)");
		fflush(stdout);
	}
	pic_probe(dev, library, queue, 4, PIC_TILE_W, PIC_TILE_H, false, "again-small");
	pic_probe(dev, library, queue, 1, PANEL_W, PIC_BAND_H, true, "one-big");
	pic_probe(dev, library, queue, 4, PANEL_W / 2, PIC_BAND_H / 2, false, "four-medium");
	pic_probe(dev, library, queue, 4, PANEL_W, PIC_BAND_H, true, "four-big");
	pic_queue_state(queue, "after the ladder");
	pic_step("THE LADDER: all four rungs finished");
}
