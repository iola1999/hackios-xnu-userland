/* Dispatch, memory, queue, present, and verdict timing tests, included into metal_drv_test.m. */


/*
 * The compute round trip, which is the same question as the draw's asked of the other path.
 *
 * A dispatch goes through the same -commit and the same fence as a draw (annibalemetal.m's
 * commit loop calls rwa_dispatch for it), so the difference between this number and the 4x4
 * draw's is the difference between the two paths' own work and nothing else -- which is the
 * only way to say whether the per-submission cost lives in the render path or under both.
 *
 * The grid is 4x4x1 in one threadgroup, air_run's own and 10.15's, so the kernel being timed
 * is doing the work it was already checked doing. Sixteen buffers of 4 KB are bound for
 * compute_path's reason: a kernel sees only what it declared, binding more is harmless and
 * binding fewer makes it read a slot nothing was bound to. A kernel that declares a texture
 * is refused by the library with its reason, which is printed and then the next kernel tried;
 * four are tried and no more, because a mode that spends a boot looking for a kernel it can
 * dispatch has stopped measuring anything.
 */
static void
tm_dispatch(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	/*
	 * Twelve tries and not four, and textures bound as well as buffers, because the first
	 * run measured nothing here and said why: all four kernels it tried were refused with
	 * "writes texture 0 and 0 textures were bound" (logs/225409-run.log). That is a true
	 * sentence about this harness and nothing about the kernels, and AV1SW.metallib has
	 * eleven of them, so both limits were this test's own.
	 */
	enum { TM_DISPATCH_BUFFERS = 16, TM_DISPATCH_TRIES = 12 };
	struct tm_run commit = { 0 }, encode = { 0 }, wait = { 0 };
	id<MTLBuffer> bufs[TM_DISPATCH_BUFFERS] = { nil };
	id<MTLTexture> texs[TM_DISPATCH_BUFFERS] = { nil };
	id<MTLSamplerState> samp = nil;
	id<MTLComputePipelineState> ps = nil;
	NSString *chosen = nil;
	unsigned tried = 0, made = 0;

	for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
		bufs[i] = [dev newBufferWithLength:4096 options:0];
		if (bufs[i] == nil) {
			printf("drvtest: time: no buffer for the dispatch, so the compute round "
			       "trip is NOT MEASURED\n");
			goto out;
		}
		if ([bufs[i] contents] != NULL) {
			memset([bufs[i] contents], 0, 4096);
		}
	}
	/*
	 * Textures and a sampler for the kernels that declare them, 64x64 and in the first
	 * format this hardware will give as an image a shader writes. Sixteen of each, for the
	 * buffers' reason: a kernel sees only what it declared, so binding more is harmless and
	 * binding fewer leaves it reading a slot nothing was bound to. A kernel whose
	 * declaration wants another element type is still refused BY NAME, and that refusal is
	 * printed -- which is the answer either way.
	 */
	{
		static const MTLPixelFormat want[] = {
			MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA32Uint,
			MTLPixelFormatRGBA16Float, MTLPixelFormatR32Float,
			MTLPixelFormatRGBA8Unorm,
		};
		MTLTextureDescriptor *td = nil;

		for (size_t f = 0; f < sizeof(want) / sizeof(want[0]) && td == nil; f++) {
			MTLTextureDescriptor *try = [MTLTextureDescriptor
			    texture2DDescriptorWithPixelFormat:want[f] width:64 height:64
						     mipmapped:NO];

			[try setUsage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];
			id<MTLTexture> probe = [dev newTextureWithDescriptor:try];

			if (probe != nil) {
				[probe release];
				td = try;
			}
		}
		for (int i = 0; td != nil && i < TM_DISPATCH_BUFFERS; i++) {
			texs[i] = [dev newTextureWithDescriptor:td];
			made += texs[i] != nil;
		}
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		samp = [dev newSamplerStateWithDescriptor:sd];
		[sd release];
		printf("drvtest: time: for the dispatch: %u of %u textures (64x64, format %lu, "
		       "read and written) and a sampler %s\n", made, TM_DISPATCH_BUFFERS,
		       td != nil ? (unsigned long)[td pixelFormat] : 0UL,
		       samp != nil ? "made" : "NOT made");
		fflush(stdout);
	}
	/*
	 * A kernel that compiles AND whose first dispatch finishes. Both are needed: 10.16
	 * measured nine kernels of which eight finished, and the ninth would otherwise be
	 * timed as a very fast failure.
	 */
	for (NSString *name in [library functionNames]) {
		if (tried >= TM_DISPATCH_TRIES || ps != nil) {
			break;
		}
		id<MTLFunction> fn = [library newFunctionWithName:name];

		if (fn == nil || [fn functionType] != MTLFunctionTypeKernel) {
			[fn release];
			continue;
		}
		tried++;
		NSError *err = nil;
		id<MTLComputePipelineState> candidate =
		    [dev newComputePipelineStateWithFunction:fn error:&err];

		if (candidate == nil) {
			printf("drvtest: time: %s got no compute pipeline: %s\n",
			       [name UTF8String], err != nil ?
			       [[err localizedDescription] UTF8String] : "(no reason)");
			[fn release];
			continue;
		}
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

		/* A nil encoder would make the command buffer record nothing, and a command
		 * buffer that recorded nothing COMPLETES -- so without this check an empty
		 * dispatch would be timed as a fast one. */
		if (enc == nil) {
			printf("drvtest: time: no compute encoder for %s, so it is not timed\n",
			       [name UTF8String]);
			[candidate release];
			[fn release];
			continue;
		}
		[enc setComputePipelineState:candidate];
		for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
			[enc setBuffer:bufs[i] offset:0 atIndex:i];
			if (texs[i] != nil) {
				[enc setTexture:texs[i] atIndex:i];
			}
			if (samp != nil) {
				[enc setSamplerState:samp atIndex:i];
			}
		}
		[enc dispatchThreadgroups:MTLSizeMake(1, 1, 1)
		    threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		tm_submissions++;
		if ([cmd status] == RW_CB_COMPLETED) {
			/*
			 * And whether anything was written, because a dispatch has no pixel to
			 * compare: all sixteen buffers were zeroed above, so a kernel that
			 * leaves them zero either writes nothing or wrote zeros, and that is
			 * worth saying out loud beside its time rather than assuming.
			 */
			unsigned long changed = 0;

			for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
				const unsigned char *p = [bufs[i] contents];

				for (unsigned k = 0; p != NULL && k < 4096; k++) {
					changed += p[k] != 0;
				}
			}
			printf("drvtest: time: %s finished and %lu of %u bytes of the bound "
			       "buffers are no longer zero%s\n", [name UTF8String], changed,
			       4096 * TM_DISPATCH_BUFFERS,
			       changed == 0 ? " -- so its time below is of a dispatch that may "
					      "have written nothing" : "");
			ps = candidate;
			chosen = name;
		} else {
			printf("drvtest: time: %s compiled but its first dispatch ended with "
			       "status %lu%s%s, so it is not timed\n", [name UTF8String],
			       (unsigned long)[cmd status], [cmd error] != nil ? ": " : "",
			       [cmd error] != nil ?
			       [[[cmd error] localizedDescription] UTF8String] : "");
			[candidate release];
		}
		[fn release];
	}
	if (ps == nil) {
		printf("drvtest: time: no kernel of this library both compiled and finished in "
		       "%u tries, so the compute round trip is NOT MEASURED here -- run this "
		       "mode again over /System/Library/VideoDecoders/AV1SW.metallib, whose "
		       "eleven kernels 10.15 dispatched on this GPU\n", tried);
		fflush(stdout);
		goto out;
	}
	printf("drvtest: time: the dispatch below is %s, 4x4x1 threads in one threadgroup, 16 "
	       "buffers bound\n", [chosen UTF8String]);
	fflush(stdout);
	/*
	 * Its compile, timed here and not with the render pipelines, because a library may have
	 * kernels and no fragment function at all (AV1SW.metallib is eleven kernels) and the
	 * render group leaves early for such a library. The kernel is one that has already
	 * dispatched, so this is the compile cost of a shader known to work.
	 */
	{
		struct tm_run compile = { 0 };
		id<MTLFunction> again = [library newFunctionWithName:chosen];

		for (unsigned r = 0; again != nil && r < tm_repeats; r++) {
			@autoreleasepool {
				NSError *e = nil;
				uint64_t t0 = tm_now_ns();
				id<MTLComputePipelineState> p =
				    [dev newComputePipelineStateWithFunction:again error:&e];
				uint64_t t1 = tm_now_ns();

				if (p != nil) {
					tm_add(&compile, t1 - t0);
					[p release];
				}
			}
		}
		[again release];
		tm_say("a COMPUTE pipeline, warm, the same kernel again", &compile,
		       "a kernel's compile is its own size -- the name is printed above, and "
		       "comparing it with a fragment shader's compares two shaders and not "
		       "two paths");
	}
	struct tm_worked worked = { 0, 0, { 0 } };
	bool gpu_side = tm_gpu_worked(&worked);

	for (unsigned r = 0; r < tm_repeats; r++) {
		@autoreleasepool {
			uint64_t t0 = tm_now_ns();
			id<MTLCommandBuffer> cmd = [queue commandBuffer];
			id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

			if (enc == nil) {
				printf("drvtest: time: the encoder stopped being given at "
				       "repeat %u\n", r);
				break;
			}
			[enc setComputePipelineState:ps];
			for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
				[enc setBuffer:bufs[i] offset:0 atIndex:i];
				if (texs[i] != nil) {
					[enc setTexture:texs[i] atIndex:i];
				}
				if (samp != nil) {
					[enc setSamplerState:samp atIndex:i];
				}
			}
			[enc dispatchThreadgroups:MTLSizeMake(1, 1, 1)
			    threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
			[enc endEncoding];
			uint64_t t1 = tm_now_ns();

			[cmd commit];
			uint64_t t2 = tm_now_ns();

			[cmd waitUntilCompleted];
			uint64_t t3 = tm_now_ns();

			tm_submissions++;
			if ([cmd status] != RW_CB_COMPLETED) {
				printf("drvtest: time: the dispatch stopped finishing at repeat "
				       "%u (status %lu), so the numbers below cover only what "
				       "came before it\n", r, (unsigned long)[cmd status]);
				break;
			}
			tm_add(&encode, t1 - t0);
			tm_add(&commit, t2 - t1);
			tm_add(&wait, t3 - t2);
		}
	}
	tm_say("-commit, one 4x4x1 compute dispatch", &commit,
	       "this kernel is a big one -- its name is printed above, and a kernel's own work "
	       "is in this number as much as the round trip is");
	if (gpu_side && tm_gpu_worked(&worked)) {
		tm_say_gpu_side("the compute dispatch", &worked, tm_median(&commit),
				(unsigned long)commit.n);
	}
	tm_say("the encoding of that dispatch", &encode,
	       "it is anywhere near -commit: encoding only records here");
	tm_say("-waitUntilCompleted after that dispatch", &wait,
	       "it is not ~0 -- then the fence moved out of -commit on the compute path too");
	[ps release];
out:
	for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
		[bufs[i] release];
		[texs[i] release];
	}
	[samp release];
}

/*
 * What memory costs: a texture in, a texture out.
 *
 * -replaceRegion: is how a game's textures arrive, and -getBytes: is how every verifying mode
 * in this file reads an attachment. The second is not a game's cost at all, and is here so
 * that it can be subtracted from the picture mode's time instead of being mistaken for a
 * draw's. Both are reported in MB/s as well, which is the only form in which 256 bytes and
 * ten megabytes are comparable.
 */
static void
tm_memory(id<MTLDevice> dev, unsigned w, unsigned h)
{
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:w
							      height:h
							   mipmapped:NO];
	struct tm_run up = { 0 }, down = { 0 }, newbuf = { 0 };
	size_t bytes = (size_t)w * h * 4;
	unsigned char *host = malloc(bytes);
	unsigned repeats = tm_repeats > 8 ? 8 : tm_repeats;

	[td setUsage:MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];
	id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

	if (tex == nil || host == NULL) {
		printf("drvtest: time: no %ux%u texture or no %zu bytes on this side, so the "
		       "upload and the readback are NOT MEASURED\n", w, h, bytes);
		free(host);
		[tex release];
		fflush(stdout);
		return;
	}
	memset(host, 0x5a, bytes);
	/* Eight and not thirty-two: ten megabytes each way is the measurement, and the point
	 * of the repeats is only that one outlier cannot be it. */
	for (unsigned i = 0; i < repeats; i++) {
		uint64_t t0 = tm_now_ns();

		[tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
		       mipmapLevel:0
			 withBytes:host
		       bytesPerRow:(NSUInteger)w * 4];
		tm_add(&up, tm_now_ns() - t0);
	}
	for (unsigned i = 0; i < repeats; i++) {
		uint64_t t0 = tm_now_ns();

		[tex getBytes:host
		  bytesPerRow:(NSUInteger)w * 4
		   fromRegion:MTLRegionMake2D(0, 0, w, h)
		  mipmapLevel:0];
		tm_add(&down, tm_now_ns() - t0);
	}
	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			uint64_t t0 = tm_now_ns();
			id<MTLBuffer> b = [dev newBufferWithLength:65536
							   options:MTLResourceStorageModeShared];
			uint64_t t1 = tm_now_ns();

			if (b != nil) {
				tm_add(&newbuf, t1 - t0);
				[b release];
			}
		}
	}
	char line[160];

	snprintf(line, sizeof(line), "-replaceRegion: the whole %ux%u BGRA8", w, h);
	tm_say_rate(line, &up, bytes,
		    "the first sample is the odd one -- the pages of this side's buffer are "
		    "touched by the memset above, so they are not being faulted in here");
	snprintf(line, sizeof(line), "-getBytes: the whole %ux%u BGRA8", w, h);
	tm_say_rate(line, &down, bytes,
		    "a game is thought to pay this: it does not read attachments back, and "
		    "this line exists to be SUBTRACTED from the verifying modes");
	tm_say("-newBufferWithLength: 65536", &newbuf,
	       "the buffer is never touched, so this is the allocation and not the pages: a "
	       "first write to it would fault");
	free(host);
	[tex release];
}

/*
 * How many command buffers a second the queue sustains with nothing in them.
 *
 * This is the floor under every frame rate: a driver that cannot make and retire a command
 * buffer quickly cannot draw a frame quickly whatever the GPU does. It is also the one place
 * where a return of the 64-slot bug (10.33) would show up as a number -- each buffer takes a
 * slot of maxCommandBufferCount at -init and this driver gives it back at -commit, so if the
 * return ever goes away this loop stops dead at the 65th instead of slowing down, and the
 * last line of this mode is the deliberate test for that.
 */
static void
tm_queue(id<MTLCommandQueue> queue)
{
	struct tm_run empty = { 0 };
	unsigned n = tm_repeats * 8 < TM_MAX_SAMPLES ? tm_repeats * 8 : TM_MAX_SAMPLES;

	for (unsigned i = 0; i < n; i++) {
		@autoreleasepool {
			uint64_t t0 = tm_now_ns();
			id<MTLCommandBuffer> cmd = [queue commandBuffer];

			[cmd commit];
			tm_add(&empty, tm_now_ns() - t0);
		}
	}
	tm_say("an EMPTY command buffer, made and committed", &empty,
	       "the autorelease pool is doing the freeing here, so this is not the cost of a "
	       "command buffer that an application keeps a handler on");

	uint64_t median = tm_median(&empty);

	/*
	 * Only when the median is above the clock's own tick. The first run's median was 0 and
	 * 1 microsecond -- that is "below this clock's resolution", and turning it into
	 * "1 000 000 a second" would be quoting the quantum as a measurement.
	 */
	if (median > 2000) {
		printf("drvtest: time: empty command buffers: %.0f a second at the median, so "
		       "no frame can cost less than %.1f us of this driver's bookkeeping per "
		       "command buffer\n", 1000000000.0 / (double)median, median / 1000.0);
	} else {
		printf("drvtest: time: an empty command buffer is BELOW this clock's resolution "
		       "(median %.1f us against a %.1f us tick): all that can be said is that "
		       "it costs at most a microsecond or two, and the queue's bookkeeping is "
		       "not in any frame's way\n", median / 1000.0, 1.0);
	}
	fflush(stdout);
}

/*
 * What it costs to show a frame -- which is NOT Metal's, and is said so where it is printed.
 *
 * This port has no drawable: a finished attachment reaches the panel by being read back into
 * this process (-getBytes:) and handed to the kernel's kAnnibaleShow, which copies it row by
 * row into the boot framebuffer (iokit/Kernel/AnnibaleGPU.cpp). Both halves are measured
 * because a frame rate that leaves them out is a frame rate for a game nobody can see.
 */
static void
tm_present(id<MTLDevice> dev)
{
	struct tm_run show = { 0 }, read = { 0 };
	unsigned repeats = tm_repeats > 8 ? 8 : tm_repeats;
	size_t bytes = (size_t)PANEL_W * PANEL_H * 4;

	if (pic_panel == NULL) {
		pic_panel = calloc(1, bytes);
	}
	if (pic_panel == NULL) {
		printf("drvtest: time: no memory for the panel, so the present is NOT "
		       "MEASURED\n");
		fflush(stdout);
		return;
	}
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:PANEL_W
							      height:PANEL_H
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];
	id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

	if (tex != nil) {
		for (unsigned i = 0; i < repeats; i++) {
			uint64_t t0 = tm_now_ns();

			[tex getBytes:pic_panel
			  bytesPerRow:PANEL_W * 4
			   fromRegion:MTLRegionMake2D(0, 0, PANEL_W, PANEL_H)
			  mipmapLevel:0];
			tm_add(&read, tm_now_ns() - t0);
		}
		tm_say_rate("a frame OUT of Metal: -getBytes: 1080x2400", &read, bytes,
			    "the attachment was never drawn into, so this is a read of "
			    "untouched memory -- which is the right number for the COPY and "
			    "says nothing about a draw");
		[tex release];
	}
	if (pic_connection == 0) {
		printf("drvtest: time: no GPU service open, so the panel copy is NOT MEASURED "
		       "(that is the kernel's kAnnibaleShow, not Metal's)\n");
		fflush(stdout);
		return;
	}
	for (unsigned i = 0; i < repeats; i++) {
		struct pic_show_request req = {
			(uint64_t)(uintptr_t)pic_panel, PANEL_W, PANEL_H, PANEL_W * 4, 0
		};
		uint64_t t0 = tm_now_ns();

		IOConnectCallStructMethod(pic_connection, 0x7f, &req, sizeof(req), NULL, NULL);
		tm_add(&show, tm_now_ns() - t0);
	}
	tm_say_rate("the panel copy in the KERNEL (kAnnibaleShow), 1080x2400", &show, bytes,
		    "this is counted as Metal's: it is a row-by-row copy in the kernel and the "
		    "same call src/gl_test.c shows a triangle with");

	uint64_t frame = tm_median(&read) + tm_median(&show);

	tm_present_ns = frame;
	if (frame != 0) {
		printf("drvtest: time: showing one finished 1080x2400 frame costs %.1f us "
		       "before any drawing at all, which alone caps this port at %.1f frames a "
		       "second\n", frame / 1000.0, 1000000000.0 / (double)frame);
		fflush(stdout);
	}
}

/*
 * The 64-slot ladder, LAST and on purpose.
 *
 * -[_MTLCommandBuffer initWithQueue:retainedReferences:synchronousDebugMode:] waits on the
 * queue's semaphore (maxCommandBufferCount, 64 by default) and this driver returns the slot
 * at -commit (10.33). Sixty-four live, committed command buffers must therefore hold zero
 * slots and the sixty-fifth must not block.
 *
 * There is deliberately NO autorelease pool in this loop: with one, each buffer would be
 * deallocated at the end of its iteration and the slot would come back from -dealloc, so the
 * ladder would pass whether or not -commit returns it -- which is exactly the bug. Sixty-four
 * live command buffers holding no slot is the measurement.
 *
 * It is last because if the return ever regresses this blocks for ever, and then everything
 * else in this mode has already been printed and the log's final line names the cause.
 */
static void
tm_slots(id<MTLCommandQueue> queue)
{
	id<MTLCommandBuffer> live[64];
	struct tm_run sixtyfifth = { 0 };

	printf("drvtest: time: the slot test: 64 command buffers, committed and kept alive, "
	       "then the 65th. If this is the last line in the log, -commit stopped returning "
	       "the queue's semaphore slot and the 65th -commandBuffer is blocked for ever\n");
	fflush(stdout);
	for (unsigned i = 0; i < 64; i++) {
		live[i] = [[queue commandBuffer] retain];
		[live[i] commit];
	}
	/* The queue's own count, asked rather than inferred -- the same two selectors
	 * pic_queue_state uses, printed here so that every line of this mode reads alike. */
	NSUInteger (*ask)(id, SEL) = (NSUInteger (*)(id, SEL))objc_msgSend;
	SEL outstanding = sel_getUid("annibaleOutstandingCommandBuffers");
	SEL most = sel_getUid("maxCommandBufferCount");

	if ([queue respondsToSelector:outstanding] && [queue respondsToSelector:most]) {
		printf("drvtest: time: the queue takes %lu command buffers at a time and %lu of "
		       "the 64 alive still hold a slot (it must be 0: the slot goes back at "
		       "-commit)\n", (unsigned long)ask(queue, most),
		       (unsigned long)ask(queue, outstanding));
	} else {
		printf("drvtest: time: this queue does not say how many command buffers hold a "
		       "slot, so a block below would again have to be inferred\n");
	}
	fflush(stdout);
	uint64_t t0 = tm_now_ns();
	id<MTLCommandBuffer> next = [queue commandBuffer];
	uint64_t t1 = tm_now_ns();

	tm_add(&sixtyfifth, t1 - t0);
	[next commit];
	tm_say("the 65th -commandBuffer with 64 alive", &sixtyfifth,
	       "it is near the two-second mark rather than near the ordinary cost above -- "
	       "then the slot is being returned late instead of at -commit");
	for (unsigned i = 0; i < 64; i++) {
		[live[i] release];
	}
}

/*
 * WHAT THE NUMBERS MEAN FOR A GAME, with the arithmetic in the log rather than in somebody's
 * head afterwards.
 *
 * Every assumption is named on the line it is used, because each of them is generous to this
 * driver and a reader who does not know that will read these frame rates as a promise:
 *
 *   - ONE COMMAND BUFFER A FRAME. An application that makes several pays the command buffer's
 *     cost several times.
 *   - NO OVERLAP, which is not an assumption but this driver's behaviour: rwa_render_draw
 *     waits on its own fence inside -commit, so the processor and the GPU take turns and a
 *     frame is the SUM of its draws. A driver that pipelined would reach the maximum of the
 *     two instead of the sum, and that is the single biggest difference between these numbers
 *     and what the same hardware can do.
 *   - EVERY DRAW COVERS THE WHOLE PANEL in the upper bracket and sixteen pixels in the lower
 *     one. A real scene is between them and nearer the lower end, so the two brackets are
 *     printed and the middle is not invented.
 *   - THE PRESENT IS PAID ONCE A FRAME, and it is not Metal's: this port shows a frame by
 *     reading the attachment back and handing it to the kernel's row copy. A game that never
 *     shows anything saves it.
 *   - NOTHING ELSE RUNS. These were measured with the panel to ourselves; with iOS's own
 *     daemons alive the GPU and the one core doing all this are shared.
 *   - THE COMPILES ARE NOT IN THE FRAME. They are counted separately, as what a game pays
 *     before its first frame and again whenever it meets a shader it has not met.
 */
static void
tm_verdict(void)
{
	static const unsigned scene[] = { 1, 10, 50, 200, 1000 };

	printf("drvtest: time: ---- what this costs a game, from the medians above ----\n");
	if (tm_warm_pipeline_ns != 0) {
		printf("drvtest: time: a shader costs %.2f ms to compile and NOTHING caches it, "
		       "so a game with 200 pipelines pays %.1f s before its first frame and one "
		       "with 2000 pays %.0f s. There is no disk cache and no pipeline library on "
		       "this port (10.40: the file Metal looks for does not exist), so it pays "
		       "that again at every launch\n", tm_warm_pipeline_ns / 1000000.0,
		       200.0 * tm_warm_pipeline_ns / 1000000000.0,
		       2000.0 * tm_warm_pipeline_ns / 1000000000.0);
	} else {
		printf("drvtest: time: the compile cost was not measured in this run, so the "
		       "startup arithmetic is missing rather than estimated\n");
	}
	if (tm_present_ns != 0) {
		printf("drvtest: time: showing a frame costs %.2f ms before any drawing, which "
		       "by itself caps this port at %.1f frames a second\n",
		       tm_present_ns / 1000000.0, 1000000000.0 / (double)tm_present_ns);
	}
	if (tm_small_marginal_ns == 0 && tm_panel_marginal_ns == 0) {
		printf("drvtest: time: no per-draw median survived this run, so no frame rate "
		       "is computed here. That is the honest answer and not a zero\n");
		fflush(stdout);
		return;
	}
	printf("drvtest: time: a frame of N draws = the present + one command buffer + N x one "
	       "draw, with no overlap between processor and GPU because this driver fences "
	       "inside -commit. Two brackets: every draw 4x4 (the per-draw floor) and every "
	       "draw the whole 1080x2400 panel (the ceiling)\n");
	for (unsigned i = 0; i < sizeof(scene) / sizeof(scene[0]); i++) {
		double fixed = (double)tm_present_ns + (double)tm_buffer_ns;
		double small = fixed + (double)scene[i] * tm_small_marginal_ns;
		double big = fixed + (double)scene[i] * tm_panel_marginal_ns;

		printf("drvtest: time: %4u draws: %8.2f ms (%6.1f fps) if every draw is 4x4; "
		       "%10.2f ms (%6.2f fps) if every draw fills the panel\n", scene[i],
		       small / 1000000.0, small > 0 ? 1000000000.0 / small : 0.0,
		       big / 1000000.0, big > 0 ? 1000000000.0 / big : 0.0);
	}
	if (tm_panel_marginal_ns != 0 && tm_small_marginal_ns != 0) {
		/*
		 * The ratio of the two is a ratio and NOT a number of draws, which is how the
		 * first run of this printed "below about 13 draws a frame" -- a wrong sentence
		 * next to a right number. What the ratio says is how much of a draw is fixed:
		 * all of it at 4x4, a fraction of it at the panel, and the area at which the
		 * two are equal is what a reader actually wants.
		 */
		printf("drvtest: time: of one full-panel draw's %.2f ms, %.2f ms is also there at "
		       "4x4 (the submission, the fence and this driver's own work per draw) and "
		       "%.2f ms is the pixels: the fixed part is %.0f%% of a full-screen draw and "
		       "all of a small one, and the two are equal at about %.2f megapixels a "
		       "draw\n", tm_panel_marginal_ns / 1000000.0,
		       tm_small_marginal_ns / 1000000.0,
		       (double)(tm_panel_marginal_ns - tm_small_marginal_ns) / 1000000.0,
		       100.0 * tm_small_marginal_ns / (double)tm_panel_marginal_ns,
		       tm_panel_marginal_ns > tm_small_marginal_ns ?
		       2.4719 * tm_small_marginal_ns /
		       (double)(tm_panel_marginal_ns - tm_small_marginal_ns) : 0.0);
	}
	if (tm_panel_tex_ns != 0 && tm_panel_marginal_ns != 0) {
		printf("drvtest: time: Apple's textured blit over the panel costs %.2f ms "
		       "against %.2f ms for the shader that only writes a colour, so one "
		       "texture fetch per pixel over 2 592 000 pixels is worth %.2f ms\n",
		       tm_panel_tex_ns / 1000000.0, tm_panel_marginal_ns / 1000000.0,
		       (double)((int64_t)tm_panel_tex_ns - (int64_t)tm_panel_marginal_ns) /
		       1000000.0);
	}
	fflush(stdout);
}

/*
 * --time [REPEATS]: the mode itself.
 *
 * The order is deliberate: the clock first (so nothing else is read without it), then the
 * compiles (which a game pays before its first frame), then the draws from the smallest shape
 * in this file to the panel's own, then memory, then the queue, then the present, then the
 * slot test. Each group says what it is and what would make it wrong, and the last lines turn
 * the medians into a frame rate with the arithmetic shown, so a reader does not have to redo
 * it to disagree with it.
 */
static void
time_path(id<MTLDevice> dev, const char *metallib, unsigned repeats)
{
	NSData *data = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:metallib]];

	tm_mode_entered = tm_now_ns();
	tm_repeats = repeats < 4 ? 4 : (repeats > TM_MAX_SAMPLES ? TM_MAX_SAMPLES : repeats);
	printf("drvtest: time: %u repeats of each measurement; every line says what it is and "
	       "what would make it wrong. A median leads, the first sample is printed apart "
	       "because the first of anything here is a different measurement, and the max is "
	       "there so an outlier is seen and not smoothed\n", tm_repeats);
	printf("drvtest: time: this driver does the GPU work inside -commit and waits for the "
	       "fence there (10.13), so -commit is what an application waits on and "
	       "-waitUntilCompleted should measure ~0. Both are printed; if the second is not "
	       "~0, the split in every line below is wrong\n");
	printf("drvtest: time: the GPU's share is separated three ways, in order of how much "
	       "they can be trusted: by ASKING THE KERNEL for the core's own time per case "
	       "(request 0x7d, if this kernel has it -- the next line says whether it does), "
	       "by subtracting the 4x4 case from the big ones, and by the kernel's own "
	       "\"tl: q\" lines in this log with gpu_log=1\n");
	fflush(stdout);
	if (data == nil) {
		printf("drvtest: time: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	NSError *err = nil;
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: time: newLibraryWithData: gave nil\n");
		return;
	}
	id<MTLCommandQueue> queue = [dev newCommandQueue];

	if (queue == nil) {
		printf("drvtest: time: no command queue\n");
		return;
	}
	/* opened here and not at the end, because the present measurement needs it and
	 * because a mode that stops early should still have shown what it got that far */
	pic_open_panel();

	tm_clock_floor();
	tm_first_frame(dev, library, queue);
	tm_pipelines(dev, library);

	/* 4x4 is every earlier number in this file, 1080x648 is the picture's band (so these
	 * are comparable with logs/140340-run.log, where the same draw read "0 ms"), and
	 * 1080x2400 is the panel: what a frame is. Four targets at 4x4 and at the band, one at
	 * the panel, because the panel with four is 41 MB of attachment and the question this
	 * mode asks is what a frame costs, not what four frames cost. */
	tm_draw_case(dev, library, queue, 4, 4, 4, false);
	tm_draw_case(dev, library, queue, 4, PANEL_W, PIC_BAND_H, false);
	tm_draw_case(dev, library, queue, 1, PANEL_W, PANEL_H, false);
	tm_draw_case(dev, library, queue, 1, PANEL_W, PANEL_H, true);

	tm_dispatch(dev, library, queue);
	tm_memory(dev, PANEL_W, PANEL_H);
	tm_queue(queue);
	tm_present(dev);

	tm_verdict();
	printf("drvtest: time: the whole mode took %.1f s of wall clock, which is NOT a "
	       "measurement of anything -- it is here so that a boot can be given enough "
	       "seconds for it\n", (tm_now_ns() - tm_mode_entered) / 1000000000.0);
	printf("drvtest: time: this mode asked the GPU for %lu submissions in all; with "
	       "gpu_log=1 the kernel's \"tl: q <fence> took <us>\" lines in this same log are "
	       "the GPU's own side of them, and its five-second report prints the average and "
	       "a histogram -- but only in a window in which something was submitted, so a "
	       "burst shorter than five seconds leaves no such line\n", tm_submissions);
	fflush(stdout);
	tm_slots(queue);
	printf("drvtest: time: done\n");
	fflush(stdout);
}
