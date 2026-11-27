/* Timing harness helpers and draw timing tests, included into metal_drv_test.m. */


/*
 * ==========================================================================================
 * --time: WHAT THIS DRIVER COSTS, ON THE PHONE, IN MICROSECONDS
 * ==========================================================================================
 *
 * Everything above this line answers "is it right". Nothing above it has ever answered "how
 * long" -- not a dispatch, not a draw, not a frame -- and the two questions fail differently:
 * a wrong draw says so in a pixel, a slow draw says nothing at all. The acceptance test for
 * this driver is a game, and a game is bought with time, so this mode measures the four
 * things a frame is actually made of and prints each with what would make it wrong.
 *
 * WHERE THE TIME GOES HERE, AND WHY IT IS NOT WHERE AN APPLICATION EXPECTS IT.
 *
 * -[AnnibaleMetalCommandBuffer commit] does the work (docs/metal.md 10.13). The encoder only
 * RECORDS; commit walks the recorded list calling rwa_render_draw / rwa_dispatch, and each of
 * those flushes the Gallium context and WAITS ON ITS OWN FENCE before returning
 * (tools/mesa/air/air_metal.c, finish_or_say_why with required = true). Three consequences,
 * and the mode is built around them:
 *
 *   1. encoding is nearly free and measures nothing an application cares about;
 *   2. -commit is the whole frame -- our translation, the submission, the GPU's execution and
 *      the wait for it -- one draw after another, with NO overlap between processor and GPU.
 *      So a frame's cost is the SUM of its draws and not the maximum of two pipelines;
 *   3. -waitUntilCompleted has nothing left to wait for and must measure ~0. That is not a
 *      hole to paper over: it is the check that the split is where this comment says it is,
 *      which is why it is measured and printed instead of assumed. A -waitUntilCompleted that
 *      is NOT ~0 means this comment is out of date and every other number here is mis-split.
 *
 * ONE DRAW IS ONE RENDER PASS IS ONE SUBMISSION. rwa_render_draw creates the surfaces, sets
 * the framebuffer, clears, draws, flushes and fences, then tears the framebuffer down again
 * (air_metal.c around 4124-4346). So eight Metal draws into one encoder are eight kernel
 * submissions and eight passes over the attachment, not one -- which is exactly why this mode
 * times one draw per command buffer AND eight, and prints the marginal cost of a draw
 * separately from the cost of a command buffer.
 *
 * WHAT IS MEASURED, AND WHY THESE AND NOT OTHERS
 *
 *   the clock itself      every number below is this clock's; printed first so that nothing
 *                         within a few times its floor is read as a measurement.
 *   a pipeline state      an AIR -> NIR -> ir3 compile inside -newRenderPipelineStateWith-
 *                         Descriptor:, with NO cache anywhere (air_metal.c: "One compiled
 *                         state, not a cache"). Measured cold (the process's first, which also
 *                         pays for whatever the compiler sets up once) and warm, the same
 *                         function again and then different functions -- because if the same
 *                         function is much cheaper the second time, something IS caching and
 *                         the warm number is not a compile.
 *   a draw                at three sizes: 4x4 (every earlier number in this file), 1080x648
 *                         (the picture's band, so this is comparable with logs/140340) and
 *                         1080x2400, the panel. A 4x4 quad says nothing about a frame: that is
 *                         the whole reason the big sizes are here.
 *   with and without the clear   the same attachments drawn through two render passes that
 *                         differ in ONE field, loadAction. Every draw in a pass whose
 *                         loadAction is Clear re-clears the attachment in this driver
 *                         (annibalemetal.m copies _clears into every recorded draw), and an
 *                         application clears once a frame, so the two have to be told apart
 *                         before a per-draw cost means anything.
 *   a texture upload      -replaceRegion: of the whole attachment-sized image, in MB/s: what
 *                         streaming a texture costs.
 *   a readback            -getBytes: of the same. No game does this; it is here because every
 *                         verifying mode in this file does it, so its cost has to be
 *                         attributable and NOT mistaken for the draw's.
 *   command buffers/s     how many the queue sustains with no work in them at all: the floor
 *                         under any frame rate, and the one number that would catch a
 *                         regression of the 64-slot semaphore bug (10.33) as a slowdown
 *                         rather than as a hang.
 *   the present           -getBytes: of the finished attachment plus the kernel's kAnnibaleShow
 *                         row copy. NOT Metal's, and said so where it is printed -- but a
 *                         frame that is never shown is not a frame, and this is the only way
 *                         this port has to show one, so a frame rate that leaves it out is a
 *                         frame rate for a game nobody can see.
 *
 * HOW OUR COST IS SEPARATED FROM THE GPU's, AND WHERE IT CANNOT BE
 *
 * Three ways, in order of how much they can be trusted. The first did not exist when this mode
 * was written and was added because the other two were not enough (docs/metal.md 10.43):
 *
 *   (0) ASK THE KERNEL. Request kAnnibaleGPUWorked (0x7d) on AnnibaleGPU answers
 *       annibale_gpu_worked()'s three numbers -- the microseconds the 3D core worked, on how
 *       many submissions, and the histogram -- taken off CP_ALWAYS_ON_COUNTER. Reading resets
 *       them, so each case is bracketed by two reads and gets the core's own time for its own
 *       draws. An older kernel answers kIOReturnUnsupported and the mode says in words that
 *       the GPU's side is not measured, rather than printing a zero.
 *
 * and the two that were there first, which still run because they need nothing of the kernel:
 *
 *   (a) BY SUBTRACTION, in this process, no boot argument needed. The 4x4 case and the
 *       1080x2400 case emit the same commands, bind the same state and make the same number
 *       of submissions; what differs is the number of pixels. So the 4x4 per-draw median is
 *       our cost plus the round trip, and the increase at the bigger sizes is the GPU's --
 *       its fill and the bandwidth of one pass over the attachment. This attributes nothing
 *       it cannot see: it says "fixed" and "proportional to area", not "ours" and "theirs".
 *   (b) BY THE KERNEL'S OWN COUNTER, which needs the boot argument gpu_log=1 and a log to
 *       read afterwards. bsd/dev/annibale_adreno.c times every submission off the GPU's
 *       always-on counter and writes "tl: q <fence> took <us>" per submission; AnnibaleGPU.cpp
 *       prints, every five seconds in which something was submitted, "the GPU worked N us on
 *       each of M" with a histogram. Those lines land in the same run log as these, so the
 *       reader subtracts. This mode cannot print them itself and does not pretend to: what it
 *       prints instead is HOW MANY submissions each case made, so the kernel's M can be
 *       matched to a case instead of guessed at.
 *
 * And one number that cannot be split at all, said so rather than attributed: the cold
 * pipeline. It contains the compiler's one-time setup and one compile and nothing separates
 * them from inside the process; what can be said is that the second compile no longer pays
 * whatever the first did, and the difference between the two is printed.
 */

enum {
	TM_MAX_SAMPLES = 256,   /* kept per measurement; more are counted and not kept */
	TM_BATCH = 8,           /* draws in the batched command buffer */
};

struct tm_run {
	unsigned n;             /* taken, which may exceed what was kept */
	uint64_t ns[TM_MAX_SAMPLES];
};

static unsigned tm_repeats = 32;
static uint64_t tm_main_entered;        /* the first statement of main() */
static uint64_t tm_mode_entered;        /* the first statement of time_path() */
static uint64_t tm_first_pixel;         /* the first draw of this mode that the GPU finished */
static uint64_t tm_cold_pipeline_ns;    /* the process's first compile, which happens once */
/*
 * The four medians the frame-rate arithmetic at the end is made of, kept here because they are
 * measured in four different groups and the arithmetic has to be done where all four are
 * known. Each is zero until its group has run, and the arithmetic says so rather than
 * substituting a guess.
 */
static uint64_t tm_small_marginal_ns;   /* a further 4x4 draw in the same command buffer */
static uint64_t tm_panel_marginal_ns;   /* a further 1080x2400 draw, trivial fragment shader */
static uint64_t tm_panel_tex_ns;        /* ... and with Apple's textured blit */
static uint64_t tm_buffer_ns;           /* the command buffer itself, once a frame */
static uint64_t tm_warm_pipeline_ns;    /* one compile, warm, a shader not seen before */
static uint64_t tm_present_ns;          /* -getBytes: the panel plus the kernel's copy */
static unsigned long tm_submissions;    /* draws this mode asked the GPU for, for the kernel's count */

/*
 * CLOCK_MONOTONIC and not pic_now_ms's timebase arithmetic, for one reason: pic_now_ms
 * divides down to milliseconds and almost everything measured here is smaller than its
 * resolution -- logs/140340-run.log says "the step before took 0 ms" for a whole 1080x648
 * textured draw, which is the exact shape of a measurement that is not one.
 */
static uint64_t
tm_now_ns(void)
{
	struct timespec t;

	if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
		return 0;
	}
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void
tm_add(struct tm_run *r, uint64_t ns)
{
	if (r->n < TM_MAX_SAMPLES) {
		r->ns[r->n] = ns;
	}
	r->n++;
}

/*
 * One measurement, said in full.
 *
 * The MEDIAN leads, because one slow sample -- a page fault, a migration to another core, the
 * kernel's own five-second report landing in the middle of a draw -- moves a mean and cannot
 * move the median of thirty-two. The MAX is printed beside it so an outlier is seen rather
 * than smoothed away, and the FIRST sample is printed on its own because in this mode the
 * first of anything is a different measurement from the rest: the first pipeline pays for the
 * compiler, the first draw with a pipeline pays for ir3's program link (ir3_cache_lookup
 * happens at draw time, not at pipeline creation), and the first touch of a buffer pays for
 * its pages.
 */
static void
tm_say(const char *what, const struct tm_run *r, const char *wrong)
{
	uint64_t s[TM_MAX_SAMPLES];
	unsigned n = r->n < TM_MAX_SAMPLES ? r->n : TM_MAX_SAMPLES;
	uint64_t total = 0;

	if (n == 0) {
		printf("drvtest: time: %-44s NOT MEASURED (no sample was taken)\n", what);
		fflush(stdout);
		return;
	}
	memcpy(s, r->ns, (size_t)n * sizeof(s[0]));
	for (unsigned i = 1; i < n; i++) {      /* insertion sort; n is at most 256 */
		uint64_t v = s[i];
		unsigned j = i;

		while (j > 0 && s[j - 1] > v) {
			s[j] = s[j - 1];
			j--;
		}
		s[j] = v;
	}
	for (unsigned i = 0; i < n; i++) {
		total += s[i];
	}
	printf("drvtest: time: %-44s n %3u  first %9.1f us  median %9.1f  min %9.1f  "
	       "max %9.1f  mean %9.1f\n", what, n, r->ns[0] / 1000.0, s[n / 2] / 1000.0,
	       s[0] / 1000.0, s[n - 1] / 1000.0, (double)total / 1000.0 / n);
	if (wrong != NULL) {
		printf("drvtest: time:    ^ wrong if: %s\n", wrong);
	}
	fflush(stdout);
}

/* The median of what was kept, for the arithmetic this mode does on its own numbers. */
static uint64_t
tm_median(const struct tm_run *r)
{
	uint64_t s[TM_MAX_SAMPLES];
	unsigned n = r->n < TM_MAX_SAMPLES ? r->n : TM_MAX_SAMPLES;

	if (n == 0) {
		return 0;
	}
	memcpy(s, r->ns, (size_t)n * sizeof(s[0]));
	for (unsigned i = 1; i < n; i++) {
		uint64_t v = s[i];
		unsigned j = i;

		while (j > 0 && s[j - 1] > v) {
			s[j] = s[j - 1];
			j--;
		}
		s[j] = v;
	}
	return s[n / 2];
}

/* The same, plus what it is in megabytes a second -- which is the only form in which a copy
 * of ten megabytes and a copy of two hundred bytes can be compared. */
static void
tm_say_rate(const char *what, const struct tm_run *r, size_t bytes, const char *wrong)
{
	uint64_t median = tm_median(r);

	tm_say(what, r, wrong);
	if (median != 0) {
		printf("drvtest: time:    ^ %zu bytes at the median: %.1f MB/s\n", bytes,
		       (double)bytes * 1000.0 / (double)median);
		fflush(stdout);
	}
}

/*
 * The floor under every number in this mode.
 *
 * Two back-to-back reads of the clock, a thousand times: the smallest non-zero difference is
 * the clock's own granularity, and the median difference is what one reading costs. A
 * measurement of a few times either is noise wearing a number's clothes, and this is printed
 * FIRST so that no one has to take the rest on trust.
 */
static void
tm_clock_floor(void)
{
	struct tm_run gap = { 0 };
	uint64_t smallest = ~0ull;

	for (unsigned i = 0; i < 1000; i++) {
		uint64_t a = tm_now_ns(), b = tm_now_ns();

		if (b > a) {
			tm_add(&gap, b - a);
			if (b - a < smallest) {
				smallest = b - a;
			}
		}
	}
	if (gap.n == 0) {
		printf("drvtest: time: the clock never moved between two reads: either "
		       "CLOCK_MONOTONIC is not implemented here or its resolution is coarser "
		       "than this loop, and NOTHING below can be believed\n");
		fflush(stdout);
		return;
	}
	/*
	 * Said this way after the first run said it the wrong way. The median of the NON-ZERO
	 * gaps is one quantum by construction -- every non-zero gap is one tick -- so printing
	 * it as "one reading costs that much" was a wrong sentence beside a right number
	 * (logs/225409-run.log: "the smallest step is 1000 ns and one reading costs about
	 * 1000 ns"). What the fraction of pairs that moved actually gives is the cost of a
	 * reading: if 44 of 1000 pairs crossed a 1000 ns boundary, a pair takes about
	 * 0.044 x 1000 ns.
	 */
	printf("drvtest: time: the clock: it ticks in steps of %llu ns, and %u of 1000 pairs of "
	       "back-to-back reads fell either side of a tick -- so one reading costs about "
	       "%llu ns and every single number below carries +/-%llu ns of quantisation, which "
	       "a median over %u repeats averages away but a lone sample does not\n",
	       (unsigned long long)smallest, gap.n,
	       (unsigned long long)(smallest * gap.n / 1000 / 2), (unsigned long long)smallest,
	       tm_repeats);
	fflush(stdout);
}

/*
 * One render pipeline, timed.
 *
 * The descriptor is built OUTSIDE the timed region on purpose: setting four pixel formats on
 * a Metal descriptor is Metal's own bookkeeping and has nothing to do with the compile, and
 * folding it in would quietly add a constant to every compile number here.
 *
 * A refusal is a number too -- how long the library took to decide -- so it is returned with
 * its reason instead of printed, and the caller keeps refusals out of the compiled ones'
 * statistics. A refused pipeline counted as a compile would make the driver look faster the
 * more shaders it cannot translate.
 */
static id<MTLRenderPipelineState>
tm_make_pipeline(id<MTLDevice> dev, id<MTLFunction> vertex, id<MTLFunction> fragment,
		 unsigned targets, uint64_t *ns, char *why, size_t size)
{
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	NSError *err = nil;

	[pd setVertexFunction:vertex];
	[pd setFragmentFunction:fragment];
	for (unsigned i = 0; i < targets; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatBGRA8Unorm];
	}
	uint64_t t0 = tm_now_ns();
	id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
									    error:&err];
	uint64_t t1 = tm_now_ns();

	*ns = t1 - t0;
	[pd release];
	if (ps == nil && why != NULL) {
		snprintf(why, size, "%s", err != nil ?
		    [[err localizedDescription] UTF8String] : "(no reason)");
	}
	return ps;
}

/*
 * THE GPU'S OWN SIDE, ASKED OF THE KERNEL INSTEAD OF READ OUT OF THE LOG.
 *
 * The first run of this mode could only subtract one case from another and point at the
 * kernel's `tl: q <fence> took <us>` lines for the rest -- and that turned out to be the
 * weakest part of it: those lines number every submission on the machine, this test is not the
 * only thing submitting, and aligning 1285 of them to four cases by eye is exactly the kind of
 * arithmetic that produces a confident wrong answer (docs/metal.md 10.43 shows the three
 * populations it did produce, and the ambiguity in their boundaries).
 *
 * So the kernel now hands the numbers over: request kAnnibaleGPUWorked (0x7d) on AnnibaleGPU
 * answers annibale_gpu_worked()'s three outputs -- the microseconds the 3D core itself worked,
 * on how many submissions, and how many of those were under 0.1, 0.25, 0.5, 1, 2 and 4 ms --
 * measured off CP_ALWAYS_ON_COUNTER at 19.2 ticks a microsecond (bsd/dev/annibale_adreno.c).
 * Reading it RESETS it, which is what makes bracketing a case possible: read once to zero,
 * draw, read again.
 *
 * Two things this must never do, and both are why it is written this way:
 *
 *   - IT MUST BE OPTIONAL. An older kernel answers 0x7d with "request 0x7d is not known here"
 *     and kIOReturnUnsupported, and a zero from that is indistinguishable from a GPU that did
 *     no work. So the first failure disables it for the run and says so in words; it never
 *     prints a zero that could be read as a measurement.
 *   - IT MUST SAY THAT IT TAKES THE NUMBERS AWAY. The kernel's own five-second report in
 *     AnnibaleGPU.cpp shares these counters, so a boot that runs this mode leaves gaps in those
 *     lines. That is expected rather than a fault, and it is printed once.
 */
struct tm_worked {
	uint64_t microseconds;
	uint32_t submissions;
	uint32_t by[7];
};

static int tm_worked_state;     /* 0 not tried, 1 answers, -1 this kernel has no such request */

static bool
tm_gpu_worked(struct tm_worked *out)
{
	struct tm_worked in = { 0, 0, { 0 } };
	size_t size = sizeof(in);

	if (tm_worked_state < 0 || pic_connection == 0) {
		return false;
	}
	kern_return_t kr = IOConnectCallStructMethod(pic_connection, 0x7d, &in, sizeof(in),
						     out, &size);

	if (kr != KERN_SUCCESS || size < sizeof(*out)) {
		/* The two answers worth telling apart are printed as the number, because they
		 * mean different things: kIOReturnUnsupported (0xe00002c7) is a kernel without
		 * this request, kIOReturnNotReady (0xe00002d8) is this kernel with the Adreno
		 * not started -- and in the second case nothing else here would be measuring
		 * either. */
		printf("drvtest: time: this kernel does not report GPU time: request 0x7d "
		       "answered %#x and gave %zu of %zu bytes (0xe00002c7 is a kernel without "
		       "the request, 0xe00002d8 is one whose GPU is not started). The GPU's own "
		       "share is NOT measured in this run -- it is only in the kernel's own "
		       "\"tl: q <fence> took <us>\" lines, if this boot has gpu_log=1\n", kr,
		       size, sizeof(*out));
		fflush(stdout);
		tm_worked_state = -1;
		return false;
	}
	if (tm_worked_state == 0) {
		printf("drvtest: time: this kernel reports GPU time (request 0x7d), so each case "
		       "below is bracketed by a read of it. Reading RESETS the kernel's counters, "
		       "which it shares with its own five-second \"the GPU worked N us\" report -- "
		       "so that report will have gaps in this boot, and that is this mode's doing "
		       "and not a fault\n");
		fflush(stdout);
		tm_worked_state = 1;
	}
	return true;
}

/*
 * What the kernel says the GPU did, beside what we measured ourselves.
 *
 * `ours_ns` is the per-draw median this case measured and `draws` how many draws it made, so
 * the two can be compared per draw rather than in bulk. The subtraction is printed only when
 * the kernel timed about as many submissions as this case made: if the counts disagree,
 * something else was submitting and the remainder is not ours -- which is said instead of
 * subtracted.
 */
static void
tm_say_gpu_side(const char *what, const struct tm_worked *w, uint64_t ours_ns,
		unsigned long draws)
{
	if (w->submissions == 0) {
		printf("drvtest: time: %s: the kernel timed NO submission of this case, so its "
		       "GPU share is unknown here (the core finishes before its times are added "
		       "up, which happens at the next submission -- so a case this short can "
		       "leave none)\n", what);
		fflush(stdout);
		return;
	}
	double per = (double)w->microseconds / w->submissions;

	printf("drvtest: time: %s: the kernel says the 3D core itself worked %.1f us on each "
	       "of %u submissions (%u under 0.1 ms, %u under 0.25, %u under 0.5, %u under 1, "
	       "%u under 2, %u under 4, %u longer)\n", what, per, w->submissions, w->by[0],
	       w->by[1], w->by[2], w->by[3], w->by[4], w->by[5], w->by[6]);
	if (draws != 0 && w->submissions >= draws - draws / 8 &&
	    w->submissions <= draws + draws / 8 && ours_ns != 0) {
		double ours = ours_ns / 1000.0;

		printf("drvtest: time: %s: THE SPLIT: %.1f us a draw measured here, %.1f us of "
		       "it the core's own, %.1f us (%.0f%%) this side of the ioctl -- our "
		       "translation, the submission and the wait. The second number is the "
		       "GPU's clock and the first is this processor's, so the remainder carries "
		       "both their errors\n", what, ours, per, ours - per,
		       ours > 0 ? 100.0 * (ours - per) / ours : 0.0);
	} else if (draws != 0) {
		printf("drvtest: time: %s: this case made %lu draws and the kernel timed %u "
		       "submissions, so they are NOT the same work and nothing is subtracted "
		       "here\n", what, draws, w->submissions);
	}
	fflush(stdout);
}

/*
 * The attachments, and the TWO passes over them that differ in one field.
 *
 * Both descriptors hold the same textures; `*clear` loads by clearing and `*load` keeps what
 * is there. That is the only difference between them, which is what makes the difference
 * between their timings the cost of the clear and nothing else. (In this driver the clear is
 * attachment 0's alone -- annibalemetal.m reads loadAction only for i == 0 -- so what the
 * difference prices is one full-attachment clear, however many targets are bound.)
 */
static bool
tm_targets(id<MTLDevice> dev, id<MTLTexture> *t, unsigned n, unsigned w, unsigned h,
	   MTLRenderPassDescriptor **clear, MTLRenderPassDescriptor **load)
{
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:w
							      height:h
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	*clear = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
	*load = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
	for (unsigned i = 0; i < n; i++) {
		t[i] = [dev newTextureWithDescriptor:td];
		if (t[i] == nil) {
			printf("drvtest: time: attachment %u of %u at %ux%u could not be made, "
			       "so this size is NOT MEASURED (that is %.1f MB of attachment "
			       "asked for)\n", i, n, w, h,
			       (double)n * w * h * 4 / 1048576.0);
			fflush(stdout);
			return false;
		}
		for (int which = 0; which < 2; which++) {
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[(which == 0 ? *clear : *load) colorAttachments]
				objectAtIndexedSubscript:i];

			[a setTexture:t[i]];
			[a setLoadAction:which == 0 ? MTLLoadActionClear : MTLLoadActionLoad];
			[a setClearColor:MTLClearColorMake(0, 0, 0, 1)];
			[a setStoreAction:MTLStoreActionStore];
		}
	}
	return true;
}

/* What one command buffer with `draws` draws in it cost, in four parts. */
struct tm_parts {
	uint64_t make;          /* -[queue commandBuffer] */
	uint64_t encode;        /* the encoder, the bindings, the draws, -endEncoding */
	uint64_t commit;        /* -commit: in THIS driver, all of the work and the fence */
	uint64_t wait;          /* -waitUntilCompleted: must be ~0 here, and that is the point */
};

/*
 * One command buffer, drawn and waited on, with each of the four parts timed separately.
 *
 * Returns false and says why when the GPU did not finish it: a draw that failed is instant,
 * and a fast number from a draw that did nothing is the one reading this mode must never
 * produce.
 */
static bool
tm_one_buffer(id<MTLCommandQueue> queue, MTLRenderPassDescriptor *rp,
	      id<MTLRenderPipelineState> ps, id<MTLBuffer> uniform, unsigned nuniform,
	      id<MTLBuffer> *vbuf, unsigned nvbuf, id<MTLTexture> tex, id<MTLSamplerState> samp,
	      unsigned draws, struct tm_parts *parts, const char *what)
{
	uint64_t t0 = tm_now_ns();
	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	uint64_t t1 = tm_now_ns();

	if (cmd == nil) {
		printf("drvtest: time: %s: no command buffer\n", what);
		fflush(stdout);
		return false;
	}
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	if (enc == nil) {
		printf("drvtest: time: %s: no render encoder\n", what);
		fflush(stdout);
		return false;
	}
	[enc setRenderPipelineState:ps];
	for (unsigned i = 0; i < nuniform; i++) {
		[enc setFragmentBuffer:uniform offset:0 atIndex:i];
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
	for (unsigned i = 0; i < draws; i++) {
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	}
	[enc endEncoding];
	uint64_t t2 = tm_now_ns();

	[cmd commit];
	uint64_t t3 = tm_now_ns();

	[cmd waitUntilCompleted];
	uint64_t t4 = tm_now_ns();

	tm_submissions += draws;
	if (tm_first_pixel == 0) {
		tm_first_pixel = t4;
	}
	parts->make = t1 - t0;
	parts->encode = t2 - t1;
	parts->commit = t3 - t2;
	parts->wait = t4 - t3;
	if ([cmd status] != RW_CB_COMPLETED) {
		printf("drvtest: time: %s: status %lu%s%s -- this draw did NOT happen, so its "
		       "time is not a draw's\n", what, (unsigned long)[cmd status],
		       [cmd error] != nil ? ": " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String]
					  : "");
		fflush(stdout);
		return false;
	}
	return true;
}

/*
 * THE COLD PATH, AND IT RUNS FIRST BECAUSE IT CANNOT BE REPEATED.
 *
 * One pipeline and one draw, in that order, before anything else in this mode has compiled
 * anything. This is the only measurement here that a process gets one sample of, and it is the
 * one a launching application pays: from the first statement of main() to a pixel the GPU has
 * actually written.
 *
 * What it does NOT include is said as plainly as what it does. The device, the Mesa screen and
 * the GPU context are opened in -initWithAcceleratorPort: (annibalemetal.m calls
 * rwa_device_open there), which main() did above, so they are inside the "before this mode"
 * part and not inside the compile. And main()'s part also contains this test's own sweep of
 * every class of the driver, which no application does -- so the breakdown is printed in three
 * pieces rather than as one number that would flatter or damn the driver depending on which
 * piece a reader assumed.
 */
static void
tm_first_frame(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	struct tm_run cold = { 0 }, first = { 0 };
	id<MTLTexture> t[1] = { nil };
	MTLRenderPassDescriptor *rp_clear = nil, *rp_load = nil;
	struct tm_parts parts = { 0, 0, 0, 0 };
	char why[512] = { 0 };
	uint64_t ns = 0;
	id<MTLFunction> fn = [library newFunctionWithName:@"clearFragment01"];

	if (fn == nil) {
		printf("drvtest: time: no clearFragment01 in this library, so the cold path is "
		       "NOT MEASURED here (every warm number below still stands)\n");
		fflush(stdout);
		return;
	}
	if (!tm_targets(dev, t, 1, 4, 4, &rp_clear, &rp_load)) {
		[fn release];
		return;
	}
	id<MTLRenderPipelineState> ps = tm_make_pipeline(dev, nil, fn, 1, &ns, why,
							 sizeof(why));

	tm_cold_pipeline_ns = ns;
	tm_add(&cold, ns);
	if (ps == nil) {
		printf("drvtest: time: the process's first pipeline was REFUSED after %.1f us: "
		       "%s\n", ns / 1000.0, why);
		fflush(stdout);
		goto out;
	}
	tm_say("a pipeline, COLD: the process's very first", &cold,
	       "something made a pipeline before this -- nothing above this mode does, so "
	       "that would mean main() changed");
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];

	if (uniform != nil && [uniform contents] != NULL) {
		memset([uniform contents], 0, 256);
		((float *)[uniform contents])[3] = 1.0f;
	}
	/* nuniform 0 for a buffer that could not be made: binding nil sixteen times would make
	 * the library refuse the draw for a reason that is this harness's and not the GPU's. */
	if (tm_one_buffer(queue, rp_clear, ps, uniform, uniform != nil ? 16 : 0, NULL, 0, nil,
			  nil, 1, &parts, "the cold draw") && tm_first_pixel != 0) {
		tm_add(&first, parts.commit);
		tm_say("-commit, COLD: the first draw of the process (4x4)", &first,
		       "it is near the warm 4x4 number far below -- then ir3 linked this "
		       "program at pipeline creation after all, and not at the draw");
		printf("drvtest: time: from the first statement of main() to the first pixel "
		       "this GPU wrote: %.1f ms = %.1f ms before this mode (the device, the "
		       "Mesa screen, the sweep of every class -- the sweep is this test's and "
		       "no application does it) + %.1f ms this mode's library and panel + %.1f "
		       "ms the cold compile + %.1f ms the cold draw\n",
		       (tm_first_pixel - tm_main_entered) / 1000000.0,
		       (tm_mode_entered - tm_main_entered) / 1000000.0,
		       (double)(tm_first_pixel - tm_mode_entered - tm_cold_pipeline_ns -
				parts.commit) / 1000000.0,
		       tm_cold_pipeline_ns / 1000000.0, parts.commit / 1000000.0);
		fflush(stdout);
	} else {
		printf("drvtest: time: the cold draw did not finish, so the distance from "
		       "main() to a first pixel is NOT MEASURED in this run\n");
		fflush(stdout);
	}
	[uniform release];
out:
	[ps release];
	[fn release];
	[t[0] release];
	[rp_clear release];
	[rp_load release];
}

/*
 * The compiles.
 *
 * Four questions, in this order, because each one is the control for the next:
 *
 *   cold            measured above, in tm_first_frame, because a process has exactly one cold
 *                   sample and spending it inside a loop would waste it. It carries the
 *                   compiler's one-time setup and one compile, and nothing inside this process
 *                   can separate them -- said here rather than attributed.
 *   warm, same      the same function again, over and over. If this is much cheaper than
 *                   cold, the difference is the one-time setup. If it is cheaper than "warm,
 *                   different" below, something is caching compiled code and this number is
 *                   not a compile at all -- air_metal.c says there is no cache
 *                   ("One compiled state, not a cache"), and this is how that claim is
 *                   checked rather than quoted.
 *   warm, different a different function each time, so no cache can be hit. THIS is the
 *                   number that prices a game's shader load.
 *   Apple's vertex  the same again with a real vertex stage (texturedVertex2D) instead of the
 *                   one this driver generates for a fragment-only pipeline: two modules
 *                   through the front end instead of one.
 */
static void
tm_pipelines(id<MTLDevice> dev, id<MTLLibrary> library)
{
	struct tm_run same = { 0 }, different = { 0 }, refused = { 0 }, apple = { 0 };
	char why[512] = { 0 };
	uint64_t ns = 0;
	id<MTLFunction> one = [library newFunctionWithName:@"clearFragment01"];
	NSArray<NSString *> *names = [library functionNames];

	/* clearFragment01 is the shader 10.33 drew 64 times on this GPU, so a pipeline made
	 * from it is a pipeline whose correctness is already known. A library without it still
	 * gets measured, with the first fragment function it does have. */
	if (one == nil) {
		for (NSString *name in names) {
			id<MTLFunction> fn = [library newFunctionWithName:name];

			if (fn != nil && [fn functionType] == MTLFunctionTypeFragment) {
				one = fn;
				printf("drvtest: time: this library has no clearFragment01, so "
				       "the pipeline timings use %s\n", [name UTF8String]);
				break;
			}
			[fn release];
		}
	}
	if (one == nil) {
		printf("drvtest: time: this library has no fragment function at all, so no "
		       "pipeline is timed\n");
		fflush(stdout);
		return;
	}
	id<MTLRenderPipelineState> ps = tm_make_pipeline(dev, nil, one, 1, &ns, why,
							 sizeof(why));

	if (ps == nil) {
		printf("drvtest: time: a pipeline was REFUSED after %.1f us: %s. Nothing below "
		       "this line is a compile's cost\n", ns / 1000.0, why);
		fflush(stdout);
		return;
	}
	[ps release];
	tm_add(&same, ns);

	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			ps = tm_make_pipeline(dev, nil, one, 1, &ns, why, sizeof(why));
			if (ps != nil) {
				tm_add(&same, ns);
				[ps release];
			}
		}
	}
	tm_say("a pipeline, warm, the SAME function again", &same,
	       "it is far below the next line -- then something caches compiled code and this "
	       "is a cache hit, not a compile");
	if (tm_cold_pipeline_ns != 0 && tm_median(&same) != 0) {
		printf("drvtest: time: the cold compile cost %.1f us and a warm one %.1f us at "
		       "the median, so whatever the compiler sets up once is worth %.1f us -- "
		       "paid by the first pipeline of a process and by no other\n",
		       tm_cold_pipeline_ns / 1000.0, tm_median(&same) / 1000.0,
		       ((double)tm_cold_pipeline_ns - (double)tm_median(&same)) / 1000.0);
		fflush(stdout);
	}

	unsigned taken = 0;

	for (NSString *name in names) {
		if (taken >= tm_repeats) {
			break;
		}
		@autoreleasepool {
			id<MTLFunction> fn = [library newFunctionWithName:name];

			if (fn == nil || [fn functionType] != MTLFunctionTypeFragment) {
				[fn release];
				continue;
			}
			id<MTLRenderPipelineState> p = tm_make_pipeline(dev, nil, fn, 1, &ns,
								       why, sizeof(why));

			if (p != nil) {
				tm_add(&different, ns);
				[p release];
				taken++;
			} else {
				tm_add(&refused, ns);
			}
			[fn release];
		}
	}
	tm_say("a pipeline, warm, a DIFFERENT function each time", &different,
	       "the functions are all alike -- these are this library's in file order, so a "
	       "library of one shape prices only that shape");
	tm_warm_pipeline_ns = tm_median(&different);
	tm_say("a pipeline the library REFUSED (how long to decide)", &refused,
	       "nothing was refused, in which case this line is absent and not zero");

	id<MTLFunction> vfn = [library newFunctionWithName:@"texturedVertex2D"];
	id<MTLFunction> ffn = [library newFunctionWithName:@"texturedFragment2DFloat"];

	if (vfn != nil && ffn != nil) {
		for (unsigned i = 0; i < tm_repeats; i++) {
			@autoreleasepool {
				id<MTLRenderPipelineState> p = tm_make_pipeline(dev, vfn, ffn, 1,
									       &ns, why,
									       sizeof(why));
				if (p != nil) {
					tm_add(&apple, ns);
					[p release];
				}
			}
		}
		tm_say("a pipeline with Apple's OWN vertex stage (2 modules)", &apple,
		       "it equals the generated-vertex line above -- then one of the two "
		       "modules is not being compiled");
	} else {
		printf("drvtest: time: no texturedVertex2D/texturedFragment2DFloat in this "
		       "library, so the two-module compile is not timed\n");
	}
	[one release];
	[vfn release];
	[ffn release];
	fflush(stdout);
}

/*
 * One size of draw, measured five ways.
 *
 * `targets` attachments of `w` by `h`, the pipeline from clearFragment01 (whose vertex stage
 * this driver generates, so the quad covers the whole attachment -- 10.33) or from Apple's
 * textured blit, and then:
 *
 *   the first draw          with a pipeline nothing has drawn with yet, on its own. ir3 links
 *                           its program at DRAW time (ir3_cache_lookup), not at pipeline
 *                           creation, so the first draw of a pipeline is a different number
 *                           from the rest and a median that swallowed it would hide a cost a
 *                           game pays once per material.
 *   one draw, clearing      the whole round trip as every mode in this file has done it.
 *   one draw, NOT clearing  the same attachments through a pass whose loadAction is Load.
 *   TM_BATCH draws, one buffer  the marginal cost of a draw, with the command buffer paid for
 *                           once. In this driver each of them is still its own submission and
 *                           its own pass, which is exactly what this is here to price.
 *   the four parts          -commandBuffer, the encoding, -commit and -waitUntilCompleted,
 *                           each on its own, from the same runs.
 *
 * and then ONE PIXEL is read back and compared with what the shader was told to write,
 * because a draw that silently did nothing is the fastest draw there is.
 */
static void
tm_draw_case(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
	     unsigned targets, unsigned w, unsigned h, bool textured)
{
	id<MTLTexture> t[8] = { nil };
	MTLRenderPassDescriptor *rp_clear = nil, *rp_load = nil;
	id<MTLRenderPipelineState> ps = nil;
	id<MTLFunction> vfn = nil, ffn = nil;
	id<MTLBuffer> uniform = nil, vpos = nil, vuv = nil;
	id<MTLTexture> small = nil;
	id<MTLSamplerState> samp = nil;
	char what[160], why[512] = { 0 };
	struct tm_run clear1 = { 0 }, load1 = { 0 }, loadN = { 0 }, make = { 0 },
		      encode = { 0 }, wait = { 0 }, firstdraw = { 0 };
	struct tm_parts parts = { 0, 0, 0, 0 };
	struct tm_worked worked = { 0, 0, { 0 } };
	bool gpu_side = false;
	uint64_t ns = 0;
	unsigned long before = tm_submissions;
	/* declared here and not where they are first wanted: every failure below leaves by
	 * `goto out`, and a jump over an initialisation is the kind of thing that compiles
	 * on one compiler and not the next */
	id<MTLBuffer> vbuf[2] = { nil, nil };
	unsigned nvbuf = 0, nuniform = 0;
	unsigned char px[4] = { 0, 0, 0, 0 };
	char label[192];
	/* the colour clearFragment01 is told to write, distinct from the clear (black) and from
	 * anything a wrong texel could be */
	static const unsigned char want[4] = { 109, 182, 73, 255 };   /* b g r a */
	static const float quad[4][4] = {
		{ -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
	};
	static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
	static float texels[PIC_TEX * PIC_TEX * 4];

	snprintf(what, sizeof(what), "%s %ux%u x%u", textured ? "Apple's blit at" : "a draw at",
		 w, h, targets);
	printf("drvtest: time: ---- %s (%.1f MB of attachment) ----\n", what,
	       (double)targets * w * h * 4 / 1048576.0);
	fflush(stdout);
	/* The functions first and the attachments after: a library without them (AV1SW's
	 * eleven kernels, say) would otherwise allocate 41 MB of render target before finding
	 * out it has nothing to draw with. */
	if (textured) {
		vfn = [library newFunctionWithName:@"texturedVertex2D"];
		ffn = [library newFunctionWithName:@"texturedFragment2DFloat"];
	} else {
		ffn = [library newFunctionWithName:@"clearFragment01"];
	}
	if (ffn == nil || (textured && vfn == nil)) {
		printf("drvtest: time: %s: the functions this case needs are not in this "
		       "library, so it is NOT MEASURED\n", what);
		goto out;
	}
	if (!tm_targets(dev, t, targets, w, h, &rp_clear, &rp_load)) {
		goto out;
	}
	ps = tm_make_pipeline(dev, vfn, ffn, targets, &ns, why, sizeof(why));
	if (ps == nil) {
		printf("drvtest: time: %s: no pipeline (%s), so this case is NOT MEASURED\n",
		       what, why);
		goto out;
	}
	if (textured) {
		/* Apple's blit reads a position and a texture coordinate per vertex and
		 * samples an 8x8 texture; the corners, the coordinates and the texels are the
		 * picture's own (pic_bands), so this draw and the one 10.33 photographed are
		 * the same draw at a different size. */
		MTLTextureDescriptor *gd =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
			MTLPixelFormatRGBA32Float width:PIC_TEX height:PIC_TEX mipmapped:NO];

		[gd setUsage:MTLTextureUsageShaderRead];
		small = [dev newTextureWithDescriptor:gd];
		vpos = [dev newBufferWithLength:64 options:0];
		vuv = [dev newBufferWithLength:64 options:0];
		if (small == nil || vpos == nil || vuv == nil || [vpos contents] == NULL ||
		    [vuv contents] == NULL) {
			printf("drvtest: time: %s: no texture or vertex buffers\n", what);
			goto out;
		}
		for (unsigned y = 0; y < PIC_TEX; y++) {
			for (unsigned x = 0; x < PIC_TEX; x++) {
				unsigned char c[4];
				float *v = texels + (y * PIC_TEX + x) * 4;

				pic_texel_colour(x, y, c);
				v[0] = c[2] / 255.0f;
				v[1] = c[1] / 255.0f;
				v[2] = c[0] / 255.0f;
				v[3] = 1.0f;
			}
		}
		[small replaceRegion:MTLRegionMake2D(0, 0, PIC_TEX, PIC_TEX)
			 mipmapLevel:0
			   withBytes:texels
			 bytesPerRow:PIC_TEX * 4 * sizeof(float)];
		memcpy([vpos contents], quad, sizeof(quad));
		memcpy([vuv contents], uv, sizeof(uv));
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		[sd setMinFilter:MTLSamplerMinMagFilterNearest];
		[sd setMagFilter:MTLSamplerMinMagFilterNearest];
		[sd setSAddressMode:MTLSamplerAddressModeClampToEdge];
		[sd setTAddressMode:MTLSamplerAddressModeClampToEdge];
		samp = [dev newSamplerStateWithDescriptor:sd];
		[sd release];
	} else {
		/* The constant buffer every clearFragment reads, bound at every index up to 16
		 * for the mosaic's reason: the shader declares index 14 and a driver that bound
		 * only 0 would leave it reading nothing. */
		uniform = [dev newBufferWithLength:256 options:0];
		if (uniform == nil || [uniform contents] == NULL) {
			printf("drvtest: time: %s: no constant buffer\n", what);
			goto out;
		}
		float *u = [uniform contents];

		memset(u, 0, 256);
		u[0] = want[2] / 255.0f;
		u[1] = want[1] / 255.0f;
		u[2] = want[0] / 255.0f;
		u[3] = 1.0f;
	}
	vbuf[0] = vpos;
	vbuf[1] = vuv;
	nvbuf = textured ? 2 : 0;
	nuniform = textured ? 0 : 16;

	/* The first draw with this pipeline, alone and not in any median. */
	if (!tm_one_buffer(queue, rp_clear, ps, uniform, nuniform, vbuf, nvbuf, small, samp, 1,
			   &parts, what)) {
		goto out;
	}
	tm_add(&firstdraw, parts.commit);
	tm_say("-commit, the FIRST draw with a fresh pipeline", &firstdraw,
	       "the pipeline had drawn before -- it is made in this function and used here "
	       "first, so that would mean this file changed");

	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			if (!tm_one_buffer(queue, rp_clear, ps, uniform, nuniform, vbuf, nvbuf,
					   small, samp, 1, &parts, what)) {
				goto out;
			}
			tm_add(&clear1, parts.commit);
			tm_add(&make, parts.make);
			tm_add(&encode, parts.encode);
			tm_add(&wait, parts.wait);
		}
	}
	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			if (!tm_one_buffer(queue, rp_load, ps, uniform, nuniform, vbuf, nvbuf,
					   small, samp, 1, &parts, what)) {
				goto out;
			}
			tm_add(&load1, parts.commit);
		}
	}
	/*
	 * The batched loop is the one the kernel's counter is bracketed around, and not the
	 * other two: every one of its draws is a submission of exactly the same work, with no
	 * clear in it, so "per submission" on the kernel's side and "per draw" on ours mean the
	 * same thing. Bracketing the whole case would mix three different draws into one
	 * average and make the subtraction meaningless.
	 */
	gpu_side = tm_gpu_worked(&worked);        /* reads to zero; what it returns here is the past */
	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			if (!tm_one_buffer(queue, rp_load, ps, uniform, nuniform, vbuf, nvbuf,
					   small, samp, TM_BATCH, &parts, what)) {
				goto out;
			}
			tm_add(&loadN, parts.commit);
		}
	}
	if (gpu_side) {
		gpu_side = tm_gpu_worked(&worked);
	}
	tm_say("-commit, 1 draw, the pass CLEARS", &clear1,
	       "the attachment is not actually cleared -- this driver reads loadAction only "
	       "for attachment 0, so with more targets this prices one clear and not four");
	tm_say("-commit, 1 draw, the pass KEEPS (no clear)", &load1,
	       "it is not below the clearing line -- then the clear costs nothing measurable "
	       "at this size, which is the expected answer at 4x4 and not at 1080x2400");
	snprintf(label, sizeof(label), "-commit, %u draws in ONE command buffer, no clear",
		 TM_BATCH);
	tm_say(label, &loadN,
	       "it is about equal to one draw -- then the draws are being merged somewhere, "
	       "which this driver does not do: each is its own submission");
	tm_say("-[queue commandBuffer] alone", &make,
	       "it grows through the run -- that is the queue's 64 slots filling, and the "
	       "last line of this mode is the test for it");
	tm_say("the encoding (encoder + bindings + draw + end)", &encode,
	       "it is anywhere near -commit -- in this driver encoding only records, so a "
	       "large number here means work moved and this comment is stale");
	tm_say("-waitUntilCompleted after that -commit", &wait,
	       "it is NOT ~0 -- then the fence is no longer paid inside -commit and every "
	       "split in this mode is wrong");

	uint64_t one = tm_median(&load1), many = tm_median(&loadN);

	if (gpu_side && many != 0) {
		tm_say_gpu_side(what, &worked, many / TM_BATCH,
				(unsigned long)tm_repeats * TM_BATCH);
	}

	if (one != 0 && many > one && TM_BATCH > 1) {
		double marginal = (double)(many - one) / (TM_BATCH - 1);

		printf("drvtest: time: %s: a draw on its own costs %.1f us and each further "
		       "draw in the same command buffer %.1f us, so the command buffer itself "
		       "is worth %.1f us\n", what, one / 1000.0, marginal / 1000.0,
		       (one - marginal) / 1000.0);
		printf("drvtest: time: %s: at %.1f us a draw, a 16.7 ms frame (60 Hz) holds "
		       "%.0f such draws and a 33.3 ms frame (30 Hz) holds %.0f\n", what,
		       marginal / 1000.0, 16700000.0 / marginal, 33300000.0 / marginal);
		if (w * h > 16) {
			/* Both, because the first run printed only the first and the two differ
			 * by the number of targets: 599 us per megapixel of PASS at 1080x648 x4
			 * is 150 us per megapixel actually written, and that is the figure
			 * comparable with the panel's single target. */
			printf("drvtest: time: %s: that is %.2f us per megapixel of the pass and "
			       "%.2f us per megapixel actually written (%u targets), fill and one "
			       "pass over it together; %.1f GB/s of attachment written\n", what,
			       marginal / 1000.0 / ((double)w * h / 1048576.0),
			       marginal / 1000.0 / ((double)w * h * targets / 1048576.0), targets,
			       (double)w * h * targets * 4.0 / marginal);
		}
		fflush(stdout);
		/* kept for the arithmetic at the end of the mode */
		if (w * h == 16 && !textured) {
			tm_small_marginal_ns = (uint64_t)marginal;
		}
		if (w == PANEL_W && h == PANEL_H) {
			if (textured) {
				tm_panel_tex_ns = (uint64_t)marginal;
			} else {
				tm_panel_marginal_ns = (uint64_t)marginal;
				tm_buffer_ns = one > (uint64_t)marginal ?
				    one - (uint64_t)marginal : 0;
			}
		}
	}

	/* And the pixel, because every number above would be better if the draw did nothing. */
	[t[0] getBytes:px
	   bytesPerRow:4
	    fromRegion:MTLRegionMake2D(0, 0, 1, 1)
	   mipmapLevel:0];
	if (textured) {
		/* The viewport puts clip y = +1 in memory row 0 (10.28), so pixel (0,0) is
		 * texel (0, PIC_TEX-1), and with a NEAREST sampler it is that texel exactly. */
		unsigned char expect[4];

		pic_texel_colour(0, PIC_TEX - 1, expect);
		printf("drvtest: time: %s: pixel (0,0) is r g b %u %u %u and texel "
		       "(0,%u) is %u %u %u -- %s\n", what, px[2], px[1], px[0], PIC_TEX - 1,
		       expect[2], expect[1], expect[0],
		       px[0] == expect[0] && px[1] == expect[1] && px[2] == expect[2] ?
		       "the same, so the timed draws drew" : "NOT THE SAME: the timings above "
		       "are of a draw that did something else");
	} else {
		printf("drvtest: time: %s: pixel (0,0) is r g b %u %u %u and its buffer named "
		       "%u %u %u -- %s\n", what, px[2], px[1], px[0], want[2], want[1], want[0],
		       px[0] == want[0] && px[1] == want[1] && px[2] == want[2] ?
		       "the same, so the timed draws drew" : "NOT THE SAME: the timings above "
		       "are of a draw that did something else");
	}
	printf("drvtest: time: %s: this case asked the GPU for %lu submissions (the kernel's "
	       "own count, with gpu_log=1, must agree)\n", what, tm_submissions - before);
	fflush(stdout);
out:
	[ps release];
	[vfn release];
	[ffn release];
	[uniform release];
	[vpos release];
	[vuv release];
	[small release];
	[samp release];
	for (unsigned i = 0; i < targets; i++) {
		[t[i] release];
	}
	[rp_clear release];
	[rp_load release];
}
