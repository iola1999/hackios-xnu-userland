/* Frame rendering, drawable surface, and readback tally, included into triangle_app.m. */

/* ------------------------------------------------------------------ reading the frame back */

/* Whether the pixel centre of (x, y) is inside the triangle through clip (-1,-1), (1,-1)
 * and (-1,1). The comparison is in integers, so it is the same answer the rasteriser's own
 * arithmetic gives and not a float that could land either way. */
static int
rwt_covered(unsigned x, unsigned y, unsigned width, unsigned height)
{
	return (unsigned long long)(2 * x + 1) * height <
	       (unsigned long long)(2 * y + 1) * width;
}

/* A pixel centre exactly ON the edge, which would make the pixel's colour the fill rule's
 * business rather than this file's. Counted so that a geometry where it happens says so
 * instead of showing up as pixels of the wrong colour. */
static int
rwt_on_edge(unsigned x, unsigned y, unsigned width, unsigned height)
{
	return (unsigned long long)(2 * x + 1) * height ==
	       (unsigned long long)(2 * y + 1) * width;
}

struct rwt_tally {
	unsigned long triangle, clear, neither, on_edge;
	unsigned long want_triangle, want_clear;
	unsigned long wrong;
	unsigned char first_wrong[4];
	unsigned first_wrong_x, first_wrong_y;
	int have_first_wrong;
};

/*
 * Every pixel of one view of the frame against the rule. `rows` is where row 0 starts and
 * `stride` is how far apart the rows are in THAT view -- which is the whole reason this is
 * a function and not a loop written twice: the IOSurface's stride is the kernel's
 * (aligned to 64) and -getBytes: is given this program's own, and a mismatch between them
 * is one of the ways an import can be wrong.
 */
static void
rwt_tally(const unsigned char *rows, size_t stride, unsigned width, unsigned height,
	  struct rwt_tally *tally)
{
	memset(tally, 0, sizeof(*tally));
	for (unsigned y = 0; y < height; y++) {
		const unsigned char *row = rows + (size_t)y * stride;

		for (unsigned x = 0; x < width; x++) {
			const unsigned char *pixel = row + (size_t)x * 4;
			int want = rwt_covered(x, y, width, height);
			const unsigned char *expect = want ? rwt_triangle_bgra
							   : rwt_clear_bgra;
			int is_triangle = pixel[0] == rwt_triangle_bgra[0] &&
					  pixel[1] == rwt_triangle_bgra[1] &&
					  pixel[2] == rwt_triangle_bgra[2];
			int is_clear = pixel[0] == rwt_clear_bgra[0] &&
				       pixel[1] == rwt_clear_bgra[1] &&
				       pixel[2] == rwt_clear_bgra[2];

			if (rwt_on_edge(x, y, width, height)) {
				tally->on_edge++;
			}
			if (is_triangle) {
				tally->triangle++;
			} else if (is_clear) {
				tally->clear++;
			} else {
				tally->neither++;
			}
			if (want) {
				tally->want_triangle++;
			} else {
				tally->want_clear++;
			}
			if (pixel[0] != expect[0] || pixel[1] != expect[1] ||
			    pixel[2] != expect[2]) {
				tally->wrong++;
				if (!tally->have_first_wrong) {
					tally->have_first_wrong = 1;
					tally->first_wrong_x = x;
					tally->first_wrong_y = y;
					memcpy(tally->first_wrong, pixel, 4);
				}
			}
		}
	}
}

static void
rwt_say_tally(const char *view, const struct rwt_tally *tally, unsigned width,
	      unsigned height)
{
	rwt_say("triangle: %s: of %lu pixels, %lu are orange, %lu are blue and %lu are "
	    "NEITHER; the rule says %lu orange and %lu blue, and %lu pixel centres sit on "
	    "the edge", view, (unsigned long)width * height, tally->triangle, tally->clear,
	    tally->neither, tally->want_triangle, tally->want_clear, tally->on_edge);
	if (tally->wrong == 0) {
		rwt_say("triangle: %s: 0 of %lu pixels are not the colour the rule says",
		    view, (unsigned long)width * height);
	} else {
		rwt_say("triangle: %s: WRONG: %lu of %lu pixels are not the colour the rule "
		    "says; the first is (%u,%u), which must be %s and reads b g r %u %u %u",
		    view, tally->wrong, (unsigned long)width * height, tally->first_wrong_x,
		    tally->first_wrong_y,
		    rwt_covered(tally->first_wrong_x, tally->first_wrong_y, width, height)
			? "orange" : "blue",
		    tally->first_wrong[0], tally->first_wrong[1], tally->first_wrong[2]);
	}
}

/*
 * Six pixels by name, printed as numbers next to what they must be. The count above says
 * whether the picture is right; these say WHAT IT IS when it is not, which a count cannot:
 * two of them straddle the hypotenuse eight hundredths of the panel apart, so a triangle
 * that is the wrong half, or mirrored, or shifted, moves a NAMED pixel and the log shows
 * which way.
 */
static unsigned
rwt_say_named_pixels(const char *view, const unsigned char *rows, size_t stride,
		     unsigned width, unsigned height, int loud)
{
	static const double points[6][2] = {
		{ 0.20, 0.80 }, { 0.80, 0.20 }, { 0.45, 0.55 },
		{ 0.55, 0.45 }, { 0.10, 0.95 }, { 0.95, 0.10 },
	};
	unsigned wrong = 0;

	for (unsigned i = 0; i < 6; i++) {
		unsigned x = (unsigned)(points[i][0] * width);
		unsigned y = (unsigned)(points[i][1] * height);
		const unsigned char *pixel;
		int want;
		const unsigned char *expect;

		if (x >= width) {
			x = width - 1;
		}
		if (y >= height) {
			y = height - 1;
		}
		pixel = rows + (size_t)y * stride + (size_t)x * 4;
		want = rwt_covered(x, y, width, height);
		expect = want ? rwt_triangle_bgra : rwt_clear_bgra;
		int agrees = pixel[0] == expect[0] && pixel[1] == expect[1] &&
			     pixel[2] == expect[2];

		if (!agrees) {
			wrong++;
		}
		/* Said in full on the frames that are counted in full, and after that only
		 * when it disagrees: six lines a frame for six hundred frames would bury the
		 * one frame that mattered under four thousand that did not. */
		if (loud || !agrees) {
			rwt_say("triangle: %s: pixel (%u,%u) must be %s (b g r %u %u %u) and is "
			    "%u %u %u: %s", view, x, y, want ? "orange" : "blue", expect[0],
			    expect[1], expect[2], pixel[0], pixel[1], pixel[2],
			    agrees ? "AGREES" : "WRONG");
		}
	}
	return wrong;
}

/* ----------------------------------------------------- the IOSurface behind the drawable */

/*
 * The drawable's own IOSurface, found through CAMetalDrawable's one ivar and validated
 * before it is believed.
 *
 * The driver's texture answers nil to -iosurface (annibalemetal.m), so there is no way to
 * ask Metal for it; CoreAnimation keeps it in the private structure of the drawable. The
 * search below is over the ivars of the drawable's class, narrowed to the one whose type
 * encoding names _CAMetalDrawablePrivate, so nothing of another shape is ever
 * dereferenced -- and the structure is only trusted after the texture and the device it
 * holds turn out to be the two this program already has.
 */
static IOSurfaceRef
rwt_surface_of_drawable(id drawable, id texture, id device)
{
	unsigned count = 0;
	Ivar *ivars = class_copyIvarList(object_getClass(drawable), &count);
	IOSurfaceRef surface = NULL;

	for (unsigned i = 0; i < count && surface == NULL; i++) {
		const char *type = ivar_getTypeEncoding(ivars[i]);
		char *private;

		if (type == NULL || strstr(type, "_CAMetalDrawablePrivate") == NULL) {
			continue;
		}
		private = *(char **)((char *)drawable + ivar_getOffset(ivars[i]));
		if (private == NULL) {
			continue;
		}
		if (*(id *)(private + RWT_PRIV_TEXTURE) != texture) {
			rwt_say("triangle: surface: %s holds a structure whose texture at "
			    "+%#x is not this drawable's, so its layout has moved and the "
			    "surface is not read from it", ivar_getName(ivars[i]),
			    RWT_PRIV_TEXTURE);
			continue;
		}
		if (*(id *)(private + RWT_PRIV_DEVICE) != device) {
			rwt_say("triangle: surface: %s holds the right texture at +%#x and "
			    "the wrong device at +%#x, so the layout is half right and "
			    "nothing is read from it", ivar_getName(ivars[i]),
			    RWT_PRIV_TEXTURE, RWT_PRIV_DEVICE);
			continue;
		}
		surface = *(IOSurfaceRef *)(private + RWT_PRIV_SURFACE);
		/* once: the structure is the same structure every frame, and the only reason
		 * to say it at all is that the offsets were validated rather than assumed */
		static int said;

		if (!said) {
			said = 1;
			rwt_say("triangle: surface: through ivar %s, whose structure holds this "
			    "drawable's own texture and device where 10.46 says they are",
			    ivar_getName(ivars[i]));
		}
	}
	free(ivars);
	if (surface == NULL) {
		static int said_not;

		if (!said_not) {
			said_not = 1;
			rwt_say("triangle: NOT MEASURED: the drawable's IOSurface could not be "
			    "reached, so the frame is read back through -getBytes: alone");
		}
	}
	return surface;
}

/* --------------------------------------------------------------------------- one frame */

struct rwt_frame_state {
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLRenderPipelineState> pipeline;
	id<MTLBuffer> uniform;
	CAMetalLayer *layer;
	unsigned frame;
	unsigned long failures;
	int present;                    /* 0 in --layer mode until the very last frame */
	unsigned char *scratch;         /* for -getBytes:, one whole frame */
	size_t scratch_bytes;
};

/*
 * Memory for the -getBytes: view of a frame, taken from the size the DRAWABLE turned out to
 * be and not from the size this file guessed.
 *
 * Lazily, and that is the point: in the application the layer has no drawableSize until
 * nextDrawable has worked one out ([layer size] x [layer contentsScale], docs/metal.md
 * 10.46), so a buffer allocated before the first frame would be allocated against a number
 * nobody had measured -- and a buffer too small is a view silently not read.
 *
 * Absent is not a failure: the surface view stands on its own, and the log says which views
 * were read.
 */
static void
rwt_want_scratch(struct rwt_frame_state *state, unsigned width, unsigned height)
{
	size_t want = (size_t)width * height * 4;

	if (state->scratch != NULL && state->scratch_bytes >= want) {
		return;
	}
	free(state->scratch);
	state->scratch = malloc(want);
	state->scratch_bytes = state->scratch != NULL ? want : 0;
	if (state->scratch == NULL) {
		rwt_say("triangle: NOT MEASURED: no memory for a %ux%u copy of the frame, so "
		    "-getBytes: is not asked", width, height);
	}
}

/*
 * One frame, end to end, and every part of it said out loud on the first few.
 *
 * The order is Metal's and the reading is deliberately BEFORE the present: the surface
 * belongs to this process until it is handed to the image queue, so reading it first
 * measures what the GPU wrote and not what a compositor may have done with it afterwards.
 */
static void
rwt_frame(struct rwt_frame_state *state)
{
	int loud = state->frame < RWT_COUNTED_FRAMES;

	state->frame++;
	@autoreleasepool {
		if (loud) {
			rwt_step("-[CAMetalLayer nextDrawable] (the first caller this driver's "
				 "-newTextureWithDescriptor:iosurface:plane: has ever had)");
		}
		id<CAMetalDrawable> drawable = [state->layer nextDrawable];

		if (drawable == nil) {
			state->failures++;
			rwt_say("triangle: frame %u: nextDrawable gave NIL. Either the driver "
			    "refused the texture -- look for \"annibalemetal: no drawable:\" "
			    "just above -- or all %lu drawables of the pool are still in "
			    "flight, which means the presents are not completing",
			    state->frame,
			    (unsigned long)[state->layer maximumDrawableCount]);
			return;
		}
		id<MTLTexture> texture = [drawable texture];

		if (texture == nil) {
			state->failures++;
			rwt_say("triangle: frame %u: FAILED: the drawable is %s and its "
			    "-texture is nil, which is what CoreAnimation does with a nil "
			    "from the device: it does not test it (docs/metal.md 10.46)",
			    state->frame, class_getName(object_getClass(drawable)));
			return;
		}
		unsigned width = (unsigned)[texture width];
		unsigned height = (unsigned)[texture height];

		if (loud) {
			rwt_say("triangle: frame %u: drawable %s, texture %s, %ux%u, pixel "
			    "format %lu, usage %lu, isDrawable %s", state->frame,
			    class_getName(object_getClass(drawable)),
			    class_getName(object_getClass(texture)), width, height,
			    (unsigned long)[texture pixelFormat],
			    (unsigned long)[texture usage],
			    [texture respondsToSelector:sel_getUid("isDrawable")] &&
			    ((BOOL (*)(id, SEL))objc_msgSend)(texture,
				sel_getUid("isDrawable")) ? "YES" : "NO");
		}
		if (width == 0 || height == 0) {
			state->failures++;
			rwt_say("triangle: frame %u: FAILED: the drawable's texture is %ux%u",
			    state->frame, width, height);
			return;
		}
		rwt_want_scratch(state, width, height);

		/* The surface, found before anything is drawn, so that a frame which draws
		 * and cannot be read back is told from a frame that never drew. */
		IOSurfaceRef surface = rwt_surface_of_drawable(drawable, texture,
		    state->device);
		size_t surface_stride = 0;
		const unsigned char *surface_rows = NULL;

		if (surface != NULL) {
			surface_stride = IOSurfaceGetBytesPerRow(surface);
			surface_rows = IOSurfaceGetBaseAddress(surface);
			if (loud) {
				rwt_say("triangle: frame %u: its IOSurface is id %u, %zux%zu, "
				    "%zu bytes a row, %zu bytes allocated, base %p",
				    state->frame, (unsigned)IOSurfaceGetID(surface),
				    IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
				    surface_stride, IOSurfaceGetAllocSize(surface),
				    (void *)surface_rows);
			}
			/* The one thing that would make reading it a wild read rather than a
			 * measurement, checked rather than assumed. */
			if (surface_rows == NULL ||
			    IOSurfaceGetWidth(surface) != width ||
			    IOSurfaceGetHeight(surface) != height ||
			    surface_stride < (size_t)width * 4 ||
			    IOSurfaceGetAllocSize(surface) < surface_stride * height) {
				rwt_say("triangle: NOT MEASURED: that surface does not describe "
				    "the texture (%ux%u, %zu bytes a row, %zu allocated), so it "
				    "is not read", width, height, surface_stride,
				    surface != NULL ? IOSurfaceGetAllocSize(surface) : 0);
				surface_rows = NULL;
			}
		}

		/* ---- the pass ---- */
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor
		    renderPassDescriptor];
		MTLRenderPassColorAttachmentDescriptor *attachment =
		    [[pass colorAttachments] objectAtIndexedSubscript:0];

		[attachment setTexture:texture];
		[attachment setLoadAction:MTLLoadActionClear];
		[attachment setClearColor:MTLClearColorMake(rwt_clear_bgra[2] / 255.0,
		    rwt_clear_bgra[1] / 255.0, rwt_clear_bgra[0] / 255.0, 1.0)];
		[attachment setStoreAction:MTLStoreActionStore];

		if (loud) {
			rwt_step("asking the queue for a command buffer");
		}
		id<MTLCommandBuffer> command = [state->queue commandBuffer];

		if (command == nil) {
			state->failures++;
			rwt_say("triangle: frame %u: FAILED: the queue gave no command buffer",
			    state->frame);
			return;
		}
		if (loud) {
			rwt_step("asking it for a render encoder over the drawable");
		}
		id<MTLRenderCommandEncoder> encoder =
		    [command renderCommandEncoderWithDescriptor:pass];

		if (encoder == nil) {
			state->failures++;
			rwt_say("triangle: frame %u: FAILED: no render encoder over the "
			    "drawable's texture", state->frame);
			return;
		}
		[encoder setRenderPipelineState:state->pipeline];
		/* at every index up to 16: the shader's buffer is declared at 14, and a
		 * driver that bound only index 0 would leave it reading nothing */
		for (unsigned i = 0; i < 16; i++) {
			[encoder setFragmentBuffer:state->uniform offset:0 atIndex:i];
		}
		if (loud) {
			rwt_step("encoding THREE vertices of the generated quad, which is one "
				 "triangle");
		}
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[encoder endEncoding];

		/*
		 * presentDrawable: BEFORE commit, because this driver does its work in
		 * -commit and Apple's -[_MTLCommandBuffer presentDrawable:] puts the present
		 * on a scheduled handler which -commit then delivers (docs/metal.md 10.46).
		 */
		if (state->present) {
			if (loud) {
				rwt_step("-[MTLCommandBuffer presentDrawable:] (Apple's own, "
					 "which hangs the present on a scheduled handler)");
			}
			[command presentDrawable:drawable];
		}
		if (loud) {
			rwt_step("commit (this driver draws HERE, not at the wait)");
		}
		[command commit];
		if (loud) {
			rwt_step("waitUntilCompleted");
		}
		[command waitUntilCompleted];

		NSUInteger status = [command status];

		if (status != RW_CB_COMPLETED) {
			state->failures++;
			rwt_say("triangle: frame %u: FAILED: command buffer status %lu%s%s",
			    state->frame, (unsigned long)status,
			    [command error] != nil ? ": " : "",
			    [command error] != nil
				? [[[command error] localizedDescription] UTF8String] : "");
			return;
		}
		if (loud) {
			rwt_say("triangle: frame %u: the GPU finished the pass (status %lu)",
			    state->frame, (unsigned long)status);
		}

		/* ---- what is in it, through two different views of the same memory ---- */
		unsigned named_wrong = 0;

		if (surface_rows != NULL) {
			uint32_t seed = 0;
			IOReturn locked = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, &seed);

			if (loud) {
				rwt_say("triangle: frame %u: IOSurfaceLock(read only) = %#x",
				    state->frame, locked);
			}
			named_wrong += rwt_say_named_pixels("surface check", surface_rows,
			    surface_stride, width, height, loud);
			if (loud) {
				struct rwt_tally tally;

				rwt_tally(surface_rows, surface_stride, width, height, &tally);
				rwt_say_tally("surface count", &tally, width, height);
				state->failures += tally.wrong != 0;
			}
			if (locked == kIOReturnSuccess) {
				IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, &seed);
			}
		}
		if (state->scratch != NULL &&
		    state->scratch_bytes >= (size_t)width * height * 4) {
			if (loud) {
				rwt_step("-getBytes: the whole frame, the GPU driver's own view "
					 "of the same resource");
			}
			memset(state->scratch, 0xa5, (size_t)width * height * 4);
			[texture getBytes:state->scratch
			      bytesPerRow:(size_t)width * 4
			       fromRegion:MTLRegionMake2D(0, 0, width, height)
			      mipmapLevel:0];
			named_wrong += rwt_say_named_pixels("getBytes check", state->scratch,
			    (size_t)width * 4, width, height, loud);
			if (loud) {
				struct rwt_tally tally;

				rwt_tally(state->scratch, (size_t)width * 4, width, height,
				    &tally);
				rwt_say_tally("getBytes count", &tally, width, height);
				state->failures += tally.wrong != 0;
			}
		}
		state->failures += named_wrong;
		/* Every twentieth frame, and every frame that went wrong: a frame is the same
		 * picture as the one before it, so what a per-frame line is actually for is the
		 * frame NUMBER -- proof that nextDrawable is still answering, which is what
		 * stops if the presents never complete. */
		if (loud || named_wrong != 0 || state->frame % 20 == 0) {
			rwt_say("triangle: frame %u: %u of the named pixels disagree; %lu "
			    "failures so far", state->frame, named_wrong, state->failures);
		}
	}
}

