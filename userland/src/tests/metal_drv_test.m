/*
 * Does the driver's device object, built as an ordinary library, get through Metal?
 *
 * metal_stubdev proved the contract with classes assembled at runtime. This asks the same
 * question of the real thing: /annibale/lib/libannibalemetal.dylib, whose AnnibaleMetalDevice
 * is declared in source and linked against the private `_OBJC_CLASS_$__MTLDevice`. Whether
 * that symbol binds at all is the first thing this program measures -- if dyld cannot find
 * it, the library does not load and the driver has to go back to building its class at
 * runtime.
 *
 * Then the same three steps as before, and in the same order Metal uses them:
 *   step 4   [class isSubclassOfClass:_MTLDevice]
 *   step 5   [[class alloc] initWithAcceleratorPort:service]
 *   MTLAddDevice (0x181a1875c), then MTLCreateSystemDefaultDevice()
 *
 * Discovery is still bypassed, deliberately: no bundle, and the service still has neither
 * MetalPluginName nor MetalPluginClassName, because docs/metal.md section 7 measured what
 * happens when the whole system takes the Metal road with nothing behind it.
 *
 * With --bundle it loads the driver the way Metal's own discovery would -- through NSBundle,
 * out of /annibale/lib/AnnibaleMetal.bundle -- instead of dlopen'ing the dylib. That tests the
 * one part of discovery that cannot be reasoned about: whether a flat, unsigned bundle of
 * ours gives up its class to -[NSBundle classNamed:], whose identity test
 * ([NSBundle bundleForClass:cls] == self, 0x1811e0dac) is the thing most likely to refuse.
 * The service is still not given MetalPluginName/MetalPluginClassName, so Metal is not asked
 * to find anything: only the steps are walked, in its order.
 *
 * Run: /annibale/bin/metal_drv_test [--bundle] [LIBRARY [FUNCTION]]   (RC_JOBS=rc.jobs.metaldev)
 *
 * With no arguments it reads /System/Library/VideoDecoders/AV1SW.metallib and offers every
 * function in it to the compiler. With a FUNCTION it compiles and dispatches just that one,
 * which is how a kernel binding nothing but buffers can be picked: the driver has no
 * textures yet, and most of the system's shaders want them.
 *
 * With --picture [SECONDS] it draws the panel instead of a pixel: 64 of Apple's own
 * fragment shaders and Apple's own textured blit, at 1080x2400, put on the screen through
 * the kernel's GPU service so that `./xl screenshot` can take it. See picture_path below;
 * RC_JOBS=rc.jobs.metalpicture runs it.
 *
 * With --time [REPEATS] it measures what all of that COSTS -- a compile, a draw at the
 * panel's own size, a texture, a command buffer, a frame on the screen -- which no mode
 * above it has ever done. See time_path below; RC_JOBS=rc.jobs.metalperf runs it.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ucontext.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mach/mach_time.h>

#define MTL_CREATE_DEVICE_UNSLID 0x181a34b3cUL
#define MTL_ADD_DEVICE_UNSLID    0x181a1875cUL
extern id MTLCreateSystemDefaultDevice(void);

/*
 * One call of the compiler library's own contract, declared here as annibalemetal.m
 * declares the rest of it (tools/mesa/air/air_metal.h is in the Mesa tree and this
 * program must not grow a dependency on it): what textures a module declares.
 *
 * It is asked of the same /annibale/lib/libannibaleair.dylib the driver loaded -- dyld
 * uniques images by install_name, so this dlopen gets that image and not a second one --
 * and of the same `struct rwa_device` the driver holds, which AnnibaleMetalDevice hands
 * out through -mesaDevice. So the answer here is the answer the driver will judge a
 * binding by at the draw, and not a second opinion about the same shader.
 *
 * Why this program has to ask at all: it is in the position of an application that did
 * not write the shader and has no MTLTextureDescriptor to read the shape off. Before
 * this, --draw-all bound ONE kind and ONE format for a whole library, and 13 of the 271
 * fragment shaders of AppleMetalGLRenderer were refused by name -- they declare a
 * texture3d, a texture2d<int> or a multisample texture. That refusal was honest and it
 * was about this harness, which means 13 shaders went unmeasured for a reason that has
 * nothing to do with the GPU.
 */
struct rwa_device;

enum rwa_element {
	RWA_ELEMENT_UNKNOWN = 0,
	RWA_ELEMENT_FLOAT,
	RWA_ELEMENT_INT,
	RWA_ELEMENT_UINT,
};

struct rwa_texture_decl {
	unsigned index;
	unsigned type;           /* MTLTextureType */
	unsigned type_known;
	unsigned usage;          /* MTLTextureUsage */
	unsigned element;        /* enum rwa_element */
	unsigned element_bits;
	char metal_type[64];
};

static int (*rwa_module_textures)(struct rwa_device *, const void *, size_t,
				  const char *, struct rwa_texture_decl *, unsigned,
				  unsigned *, char *, size_t);
static struct rwa_device *air_device;

/*
 * How many members an MTLArgumentEncoder has recorded against (buffer, offset), asked of
 * the compiler library itself.
 *
 * It is the one thing about an encoder that cannot be seen from Metal's side: there is no
 * argument buffer to read back, because the frontend resolved every member of every set
 * to a binding and Metal's 8-byte handles mean nothing to a Gallium driver (docs/air.md
 * 8.2), so what an encoder leaves behind is a RECORD in the library and not bytes. This
 * reaches it the way -mesaDevice is reached above: the driver's own accessor for the
 * `struct rwa_buffer` behind an MTLBuffer, and then the library's own count.
 */
static unsigned (*rwa_argbuf_encoded_members)(const void *, size_t);

static unsigned
argbuf_records(id<MTLBuffer> buffer, size_t offset)
{
	SEL mesa = sel_getUid("mesaBuffer");

	if (rwa_argbuf_encoded_members == NULL || buffer == nil ||
	    ![buffer respondsToSelector:mesa]) {
		return 0;
	}
	const void *b = ((const void *(*)(id, SEL))objc_msgSend)(buffer, mesa);

	return b != NULL ? rwa_argbuf_encoded_members(b, offset) : 0;
}

/* Both halves of the query, or neither: a table filled from a device that is not the
 * driver's would describe a shader nobody is about to draw. Prints what is missing and
 * leaves the sweep to its old single binding, which is a smaller measurement and not a
 * wrong one. */
static void
load_texture_query(id<MTLDevice> dev)
{
	void *lib = dlopen("/annibale/lib/libannibaleair.dylib", RTLD_NOW);

	if (lib == NULL) {
		printf("drvtest: no compiler library to ask what a shader declares: %s\n",
		       dlerror());
		return;
	}
	*(void **)&rwa_module_textures = dlsym(lib, "rwa_module_textures");
	/* and the argument-buffer record count, which --argbuf asks for and nothing else
	 * needs: a library without it is older than that mode and says so there. */
	*(void **)&rwa_argbuf_encoded_members = dlsym(lib, "rwa_argbuf_encoded_members");
	if (rwa_module_textures == NULL) {
		printf("drvtest: that compiler library has no rwa_module_textures, so it is "
		       "older than this test\n");
		return;
	}
	SEL mesa = sel_getUid("mesaDevice");

	if (![dev respondsToSelector:mesa]) {
		printf("drvtest: this device object does not answer -mesaDevice, so there is "
		       "no screen to read a module with\n");
		rwa_module_textures = NULL;
		return;
	}
	air_device = ((struct rwa_device *(*)(id, SEL))objc_msgSend)(dev, mesa);
	if (air_device == NULL) {
		printf("drvtest: the device has no Mesa screen (a boot without gpu=1), so "
		       "nothing can be read out of a module\n");
		rwa_module_textures = NULL;
	}
}

/* One function's module, as -[_MTLFunction bitcodeData] hands it over: the 0x0B17C0DE
 * wrapper and all, which the compiler library accepts either way. nil when this
 * function does not answer the selector, which is the one thing that would make the
 * whole query moot. */
static NSData *
function_bitcode(id<MTLFunction> fn)
{
	SEL bitcode = sel_getUid("bitcodeData");

	if (fn == nil || ![fn respondsToSelector:bitcode]) {
		return nil;
	}
	NSData *data = ((NSData *(*)(id, SEL))objc_msgSend)(fn, bitcode);

	return data != nil && [data length] >= 4 ? data : nil;
}

/* Steps 3 and 4 of Metal's discovery, by hand, against a bundle of ours. Returns the class
 * Metal would have ended up with, or Nil with the reason printed. */
static Class
class_from_bundle(const char *path)
{
	NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:path]];

	if (bundle == nil) {
		printf("drvtest: NSBundle would not open %s\n", path);
		return Nil;
	}
	printf("drvtest: bundle %s, executable %s\n", path,
	       [bundle executablePath] != nil ? [[bundle executablePath] UTF8String] : "(none)");

	/* what step 4 does first, and the test that can refuse it */
	Class named = [bundle classNamed:@"AnnibaleMetalDevice"];

	printf("drvtest: [bundle classNamed:] gave %s\n",
	       named != Nil ? class_getName(named) : "nil -- the identity test refused");

	/* and the road Metal takes when that answers nil, which makes no such test */
	Class principal = [bundle principalClass];

	printf("drvtest: [bundle principalClass] gave %s\n",
	       principal != Nil ? class_getName(principal) : "nil");

	Class cls = named != Nil ? named : principal;
	if (cls == Nil) {
		return Nil;
	}
	if ([NSBundle bundleForClass:cls] == bundle) {
		printf("drvtest: and the runtime agrees the class came out of that bundle\n");
	}
	return cls;
}

/* One number over a run of bytes, computed the same way here and on the host, so that two
 * readers of the same module can be compared without carrying the module anywhere. */
static uint64_t
fnv1a(const unsigned char *bytes, size_t length)
{
	uint64_t hash = 0xcbf29ce484222325ULL;

	for (size_t i = 0; i < length; i++) {
		hash ^= bytes[i];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

/* Where the AIR of one function actually comes from.
 *
 * -[_MTLFunction bitcodeData] (0x181aef9e0) is @required on <MTLFunctionSPI> and returns the
 * function's module with LLVM's 0x0B17C0DE wrapper in front of it: Metal has already read
 * the container, found this function in it and kept the whole metallib mapped, so a driver
 * asks one declared selector and gets bytes. That is the junction with subsystem (1): the
 * frontend in docs/air.md reads exactly such a module.
 *
 * So this prints, for every function of the library, the wrapper's own idea of where the
 * module is and a checksum over it -- which tools/metallib.py computes over the same
 * function of the same file on the host. Equal numbers mean Metal and our own reader hand
 * the compiler the same bytes; different numbers would mean the frontend has been compiling
 * something other than what Metal would give it. */
static void
air_of_each_function(id<MTLLibrary> library)
{
	SEL bitcode = sel_getUid("bitcodeData");

	for (NSString *name in [library functionNames]) {
		id<MTLFunction> fn = [library newFunctionWithName:name];

		if (fn == nil) {
			printf("drvtest: air %-34s no function object\n", [name UTF8String]);
			continue;
		}
		if (![fn respondsToSelector:bitcode]) {
			printf("drvtest: air %-34s does not answer bitcodeData\n",
			       [name UTF8String]);
			continue;
		}
		NSData *data = ((NSData *(*)(id, SEL))objc_msgSend)(fn, bitcode);

		if (data == nil || [data length] < 20) {
			printf("drvtest: air %-34s bitcodeData gave %s\n", [name UTF8String],
			       data == nil ? "nil" : "less than a wrapper");
			continue;
		}
		const unsigned char *bytes = [data bytes];
		uint32_t magic = 0, offset = 0, size = 0;

		memcpy(&magic, bytes + 0, 4);
		memcpy(&offset, bytes + 8, 4);
		memcpy(&size, bytes + 12, 4);
		if (magic != 0x0b17c0deU || (size_t)offset + size > [data length]) {
			printf("drvtest: air %-34s wrapper %#x, offset %u, size %u of %lu -- "
			       "not what the format says\n", [name UTF8String], magic, offset,
			       size, (unsigned long)[data length]);
			continue;
		}
		printf("drvtest: air %-34s module %u bytes at %u of %lu, fnv1a %016llx\n",
		       [name UTF8String], size, offset, (unsigned long)[data length],
		       (unsigned long long)fnv1a(bytes + offset, size));
	}
	fflush(stdout);
}

/* The compute path, asked of the driver exactly as an application would ask.
 *
 * Every function of the library is offered to newComputePipelineStateWithFunction:, because
 * the interesting number is how many of them a driver on this port can compile at all, and
 * because a refusal carries the front end's own sentence about what it could not translate.
 * The first one that compiles and binds nothing but buffers is then dispatched for real:
 * queue, command buffer, encoder, commit, wait, and a look at the memory afterwards. */
/* What kind of texture to bind, and of what format, when nothing else says. A kernel
 * declaring `texture3d<half, sample>` is refused when a 2D texture is bound, and the refusal
 * names the kind -- which is useful once and useless twice, so --tex/--texfmt let the kind be
 * given on the command line after it has been read out of the shader's own air.texture
 * metadata on the machine.
 *
 * The draw sweep no longer needs that: rwa_module_textures answers what each shader declares
 * and --draw-all binds it per shader. These remain the compute path's binding and the draw
 * sweep's fallback for a slot no shader named, and --texslots below remains an override. */
static NSUInteger texture_kind = MTLTextureType2D;
static MTLPixelFormat texture_format = MTLPixelFormatRGBA32Float;

/*
 * And per slot, because a kernel can declare a different kind and element type in each:
 * one of the corpus's reads texture 0 as texture2d<uint> and texture 3 as texture2d<int>,
 * and a harness that binds one format everywhere is refused by name for the others. The
 * maps are filled from the shader's own air.texture entries, read on the machine; a slot
 * nobody named falls back to the single values above.
 *
 * Where --draw-all now reads the declaration itself, these are the deliberate mismatch: a
 * slot named here is bound what the command line says and not what the shader asked for,
 * which is the only way the driver's refusal can be tested at all.
 */
#define RW_TEX_SLOTS 16
static MTLPixelFormat slot_format[RW_TEX_SLOTS];
static NSUInteger slot_kind[RW_TEX_SLOTS];

static MTLPixelFormat
format_named(const char *name)
{
	static const struct { const char *name; MTLPixelFormat format; } formats[] = {
		{ "rgba8unorm", MTLPixelFormatRGBA8Unorm },
		{ "rgba16float", MTLPixelFormatRGBA16Float },
		{ "rgba16uint", MTLPixelFormatRGBA16Uint },
		{ "rgba16sint", MTLPixelFormatRGBA16Sint },
		{ "rgba32float", MTLPixelFormatRGBA32Float },
		{ "rgba32uint", MTLPixelFormatRGBA32Uint },
		{ "rgba32sint", MTLPixelFormatRGBA32Sint },
		{ "r32float", MTLPixelFormatR32Float },
		{ "r32uint", MTLPixelFormatR32Uint },
		/* the depth attachment formats iOS has, for --draw-depth. Depth24Unorm_Stencil8
		 * is absent because Metal's own header marks it API_UNAVAILABLE(ios) and the
		 * device answers NO to -isDepth24Stencil8PixelFormatSupported. */
		{ "depth16unorm", MTLPixelFormatDepth16Unorm },
		{ "depth32float", MTLPixelFormatDepth32Float },
		{ "depth32float_stencil8", MTLPixelFormatDepth32Float_Stencil8 },
		{ "stencil8", MTLPixelFormatStencil8 },
	};

	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (strcmp(name, formats[i].name) == 0) {
			return formats[i].format;
		}
	}
	printf("drvtest: unknown texture format \"%s\"\n", name);
	return 0;
}

static NSUInteger
kind_named(const char *name)
{
	return strcmp(name, "3d") == 0 ? MTLTextureType3D :
	    strcmp(name, "cube") == 0 ? MTLTextureTypeCube :
	    strcmp(name, "1d") == 0 ? MTLTextureType1D :
	    strcmp(name, "2darray") == 0 ? MTLTextureType2DArray : MTLTextureType2D;
}

/* "0=2d:rgba32uint,3=2d:rgba32sint" -- one entry per slot the shader declares. */
static void
parse_slots(const char *spec)
{
	char buf[512];

	snprintf(buf, sizeof(buf), "%s", spec);
	for (char *tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
		unsigned index = 0;
		char kind[16] = { 0 }, fmt[32] = { 0 };

		if (sscanf(tok, "%u=%15[^:]:%31s", &index, kind, fmt) != 3 ||
		    index >= RW_TEX_SLOTS) {
			printf("drvtest: cannot read the slot spec \"%s\"\n", tok);
			continue;
		}
		slot_kind[index] = kind_named(kind);
		slot_format[index] = format_named(fmt);
	}
}

/* One texel, in whatever the texture's format is. __fp16 does the half conversion with the
 * hardware's own rounding rather than a hand-rolled one, which is the sort of thing that is
 * wrong in the last bit and then blamed on the GPU. */
static void
write_texel(unsigned char *at, MTLPixelFormat format, const float v[4])
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm:
		for (int c = 0; c < 4; c++) {
			float x = v[c] / 1000.0f;   /* the values run to 1400; scale to [0,1] */

			at[c] = (unsigned char)(x < 0 ? 0 : x > 1 ? 255 : x * 255.0f + 0.5f);
		}
		return;
	case MTLPixelFormatRGBA16Float: {
		__fp16 h[4];

		for (int c = 0; c < 4; c++) {
			h[c] = (__fp16)v[c];
		}
		memcpy(at, h, sizeof(h));
		return;
	}
	case MTLPixelFormatRGBA16Uint: {
		unsigned short u[4];

		for (int c = 0; c < 4; c++) {
			u[c] = (unsigned short)v[c];
		}
		memcpy(at, u, sizeof(u));
		return;
	}
	case MTLPixelFormatRGBA16Sint: {
		short s[4];

		for (int c = 0; c < 4; c++) {
			s[c] = (short)v[c];
		}
		memcpy(at, s, sizeof(s));
		return;
	}
	case MTLPixelFormatRGBA32Uint: {
		unsigned u[4];

		for (int c = 0; c < 4; c++) {
			u[c] = (unsigned)v[c];
		}
		memcpy(at, u, sizeof(u));
		return;
	}
	/*
	 * An integer format takes the NUMBER and not the bits of the float that held it,
	 * which is the convention air_run and air_metal_test both write by
	 * (`written.i[k] = (int32_t)v[c]`, air_metal_test.c) -- and the only one under
	 * which a shader's answer here and its answer there are the same measurement.
	 * Writing 100.0f's bits into a texture2d<int> would have the shader read
	 * 1120403456, a number that looks like a compiler bug.
	 *
	 * RGBA32Sint fell into the four-float case below until a slot could ask for it by
	 * name, which is why the case is here now and not in the first version of this.
	 */
	case MTLPixelFormatRGBA32Sint: {
		int s[4];

		for (int c = 0; c < 4; c++) {
			s[c] = (int)v[c];
		}
		memcpy(at, s, sizeof(s));
		return;
	}
	case MTLPixelFormatR32Uint: {
		unsigned u = (unsigned)v[0];

		memcpy(at, &u, sizeof(u));
		return;
	}
	case MTLPixelFormatR32Float:
		memcpy(at, v, sizeof(float));
		return;
	default:                            /* RGBA32Float and anything else four-float wide */
		memcpy(at, v, 4 * sizeof(float));
		return;
	}
}

/* write_texel backwards, for the one case where the test has to say what landed in a texture
 * rather than what it put there. Only the formats write_texel makes are read: a format it
 * cannot write cannot be in a texture this test seeded.
 */
static void
read_texel(const unsigned char *at, MTLPixelFormat format, float v[4])
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm:
		for (int c = 0; c < 4; c++) {
			v[c] = at[c] / 255.0f * 1000.0f;   /* write_texel's scale, undone */
		}
		return;
	case MTLPixelFormatRGBA16Float: {
		__fp16 h[4];

		memcpy(h, at, sizeof(h));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)h[c];
		}
		return;
	}
	case MTLPixelFormatRGBA16Uint: {
		unsigned short u[4];

		memcpy(u, at, sizeof(u));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)u[c];
		}
		return;
	}
	case MTLPixelFormatRGBA16Sint: {
		short t[4];

		memcpy(t, at, sizeof(t));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)t[c];
		}
		return;
	}
	case MTLPixelFormatRGBA32Uint: {
		unsigned u[4];

		memcpy(u, at, sizeof(u));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)u[c];
		}
		return;
	}
	case MTLPixelFormatRGBA32Sint: {
		int t[4];

		memcpy(t, at, sizeof(t));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)t[c];
		}
		return;
	}
	case MTLPixelFormatR32Uint: {
		unsigned u;

		memcpy(&u, at, sizeof(u));
		v[0] = (float)u;
		return;
	}
	case MTLPixelFormatR32Float:
		memcpy(v, at, sizeof(float));
		return;
	default:
		memcpy(v, at, 4 * sizeof(float));
		return;
	}
}

/* How wide one texel of a format is, which is what a row's stride is counted in. It was
 * an expression inside the compute path, right for the three formats that path asked
 * for and wrong for R32Float (4 bytes read as 16) the moment a slot could name one. */
static unsigned
texel_bytes_of(MTLPixelFormat format)
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm:
	case MTLPixelFormatR32Float:
	case MTLPixelFormatR32Uint:
		return 4;
	case MTLPixelFormatRGBA16Float:
	case MTLPixelFormatRGBA16Uint:
	case MTLPixelFormatRGBA16Sint:
		return 8;
	default:                            /* the four-word RGBA32 formats */
		return 16;
	}
}

static MTLTextureDescriptor *
texture_descriptor(MTLPixelFormat format, MTLTextureUsage usage)
{
	MTLTextureDescriptor *td = [[[MTLTextureDescriptor alloc] init] autorelease];

	[td setPixelFormat:format];
	[td setTextureType:texture_kind];
	[td setWidth:64];
	[td setHeight:texture_kind == MTLTextureType1D ? 1 : 64];
	[td setDepth:texture_kind == MTLTextureType3D ? 16 : 1];
	[td setArrayLength:1];
	[td setMipmapLevelCount:1];
	[td setSampleCount:1];
	[td setUsage:usage];
	return td;
}

#include "metal_drv_test_compute.inc"
#include "metal_drv_test_attrib.inc"
#include "metal_drv_test_draw.inc"
#include "metal_drv_test_picture.inc"
#include "metal_drv_test_bands.inc"
#include "metal_drv_test_time.inc"
#include "metal_drv_test_time2.inc"
#include "metal_drv_test_sweep.inc"


int
main(int argc, char **argv)
{
	struct sigaction sa = { 0 };

	/* First, before anything: --time reports the distance from here to the first drawn
	 * pixel, which is what a launching application waits through. */
	tm_main_entered = tm_now_ns();
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = fault;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	const char *path = "/annibale/lib/libannibalemetal.dylib";
	const char *metallib = "/System/Library/VideoDecoders/AV1SW.metallib";
	const char *only = NULL;
	bool as_bundle = false;
	bool draw = false;
	bool picture = false;
	int picture_seconds = 60;
	bool timing = false;
	unsigned timing_repeats = 32;
	int next = 1;

	/*
	 * --sweep: ask Metal for the system's device and count what every class of the driver
	 * owes, then stop. Nothing is dlopen'd and MTLAddDevice is not called, so this is the
	 * road iOS's own daemons take -- which makes it the one form of this program that can
	 * be added to a boot with metal=1 and launchd alive (rc.jobs.metalprops) to put the
	 * numbers in that log beside whatever the daemons then do.
	 */
	if (argc > next && strcmp(argv[next], "--sweep") == 0) {
		id<MTLDevice> found = MTLCreateSystemDefaultDevice();

		printf("drvtest: sweep: MTLCreateSystemDefaultDevice() = %s\n",
		       found != nil ? class_getName(object_getClass(found)) : "nil");
		if (found == nil) {
			printf("drvtest: sweep: without a device from Metal the driver's "
			       "classes are not loaded; boot with metal=1 and run "
			       "metal_props on first\n");
			return 1;
		}
		sweep_every_class();
		return 0;
	}
	if (argc > next && strcmp(argv[next], "--bundle") == 0) {
		as_bundle = true;
		next++;
	}
	if (argc > next && strcmp(argv[next], "--draw") == 0) {
		draw = true;
		next++;
	}
	/* --samples N: the draw's attachments carry N samples, and above one the pass resolves
	 * into the one-sample texture it would otherwise have stored into directly. Its own
	 * flag rather than a third size for --draw, because what it changes is the attachment's
	 * type, and a driver that answered -supportsSampleCount: with one is exactly what it is
	 * there to catch -- the run says which answer the device gave before it draws. */
	if (argc > next && strcmp(argv[next], "--samples") == 0 && argc > next + 1) {
		draw_samples = (unsigned)strtoul(argv[next + 1], NULL, 10);
		if (draw_samples != 1 && draw_samples != 2 && draw_samples != 4) {
			printf("drvtest: --samples takes 1, 2 or 4 and was given %u\n",
			       draw_samples);
			return 2;
		}
		next += 2;
	}
	/* --draw-index implies --draw: the indexed steps are compared with the ordinary
	 * draw's own picture, so that draw has to happen first and in the same process. */
	if (argc > next && strcmp(argv[next], "--draw-index") == 0) {
		draw = true;
		draw_index = true;
		next++;
	}
	/* --draw-attrib does NOT imply --draw: it builds its own pipelines, with a vertex
	 * descriptor, and the picture it compares everything with is its own first step. */
	if (argc > next && strcmp(argv[next], "--draw-attrib") == 0) {
		draw_attrib = true;
		next++;
	}
	/* --argbuf: MTLArgumentEncoder, and the buffer offset that goes with it. Its own
	 * flag and not an option of --draw because two of its three measurements draw
	 * nothing at all (an encoder from a function, and one from descriptors), and the
	 * third draws the same shader twice -- so what it prints is a comparison and not a
	 * picture. argbuf_path says what each of the three is worth. */
	if (argc > next && strcmp(argv[next], "--argbuf") == 0) {
		argbuf = true;
		next++;
	}
	/*
	 * --vertexlib and --gated are taken out of the argument list wherever they stand,
	 * before anything else is read, because a flag's meaning must not depend on its
	 * neighbours. The job list writes
	 *     --vertexlib L --vertex V --gated G --fragment F
	 * and a loop that accepted only a contiguous run of these two stopped at --vertex
	 * and left --gated for nobody; the device reported "cannot read --gated" after a
	 * boot that had already spent its twenty minutes. Lifting them out here keeps the
	 * --vertex/--fragment pair loop below exactly as it was.
	 */
	for (int i = next; i + 1 < argc; ) {
		bool lib = strcmp(argv[i], "--vertexlib") == 0;

		if (!lib && strcmp(argv[i], "--gated") != 0) {
			i++;
			continue;
		}
		if (lib) {
			attrib_vertex_lib = argv[i + 1];
		} else {
			/* "none" leaves the gate unmeasured and says so, for a library that
			 * has no module of that shape. */
			attrib_gated_function = strcmp(argv[i + 1], "none") == 0
			    ? NULL : argv[i + 1];
		}
		for (int j = i; j + 2 < argc; j++) {
			argv[j] = argv[j + 2];
		}
		argc -= 2;
	}
	/* --picture [SECONDS]: the draw at the size of the panel, put on the panel. Its own
	 * flag and not a size for --draw, because what it produces is not a pixel to compare
	 * but a whole screen, and because it ends in a loop that repaints: a run is over when
	 * XL_SECS says so, and the screenshot is taken while this is still painting. */
	if (argc > next && strcmp(argv[next], "--picture") == 0) {
		picture = true;
		next++;
		if (argc > next && argv[next][0] >= '0' && argv[next][0] <= '9') {
			picture_seconds = (int)strtol(argv[next], NULL, 10);
			next++;
		}
	}
	/*
	 * --time [REPEATS]: what the driver costs rather than whether it is right. Its own
	 * flag and not a size for --draw, for the same reason --picture has one: what it
	 * produces is not a pixel to compare but a time, and a time has to be measured at
	 * the panel's real size over and over before one number of it means anything.
	 * REPEATS defaults to 32, which is enough that one outlier cannot be the
	 * measurement and small enough that four sizes of draw still fit in a boot.
	 */
	if (argc > next && strcmp(argv[next], "--time") == 0) {
		timing = true;
		next++;
		if (argc > next && argv[next][0] >= '0' && argv[next][0] <= '9') {
			timing_repeats = (unsigned)strtoul(argv[next], NULL, 10);
			next++;
		}
	}
	/* --draw-depth is the draw with a depth attachment under it, which no pass in this
	 * port has ever had. Its own flag because what it measures is not a pixel but
	 * whether a draw was discarded, and that takes four draws of one quad (depth_path
	 * says why each). An optional format name follows, by name and not by number for
	 * --texfmt's reason. */
	if (argc > next && strcmp(argv[next], "--draw-depth") == 0) {
		draw_depth = true;
		next++;
		if (argc > next && strncmp(argv[next], "--", 2) != 0 &&
		    strchr(argv[next], '/') == NULL) {
			MTLPixelFormat f = format_named(argv[next]);

			if (f == 0) {
				printf("drvtest: %s is not a pixel format this test names\n",
				       argv[next]);
				return 2;
			}
			depth_attachment_format = f;
			next++;
		}
	}
	if (argc > next && strcmp(argv[next], "--draw-all") == 0) {
		draw = true;
		draw_all = true;
		next++;
		if (argc > next + 1 && argv[next][0] >= '0' && argv[next][0] <= '9') {
			draw_from = (unsigned)strtoul(argv[next], NULL, 10);
			draw_count = (unsigned)strtoul(argv[next + 1], NULL, 10);
			next += 2;
		}
	}
	while (argc > next && (strcmp(argv[next], "--seed-rows") == 0 ||
			       strcmp(argv[next], "--no-texdecl") == 0)) {
		if (strcmp(argv[next], "--seed-rows") == 0) {
			seed_rows = true;
		} else {
			use_decl = false;
		}
		next++;
	}
	while (argc > next + 1 && (strcmp(argv[next], "--vertex") == 0 ||
				   strcmp(argv[next], "--fragment") == 0)) {
		if (strcmp(argv[next], "--vertex") == 0) {
			vertex_function = argv[next + 1];
		} else {
			fragment_function = argv[next + 1];
		}
		next += 2;
	}
	while (argc > next + 1 && strncmp(argv[next], "--tex", 5) == 0) {
		if (strcmp(argv[next], "--tex") == 0) {
			const char *kind = argv[next + 1];

			texture_kind = strcmp(kind, "3d") == 0 ? MTLTextureType3D :
			    strcmp(kind, "cube") == 0 ? MTLTextureTypeCube :
			    strcmp(kind, "1d") == 0 ? MTLTextureType1D :
			    strcmp(kind, "2darray") == 0 ? MTLTextureType2DArray :
			    MTLTextureType2D;
		} else if (strcmp(argv[next], "--texslots") == 0) {
			parse_slots(argv[next + 1]);
		} else if (strcmp(argv[next], "--texfmt") == 0) {
			/* By name, mapped with the SDK's own constants: a pixel format written
			 * as a number here would be a number copied out of a header by hand,
			 * and the one thing worse than a wrong format is a wrong format that
			 * looks deliberate. */
			MTLPixelFormat f = format_named(argv[next + 1]);

			if (f != 0) {
				texture_format = f;
			}
		}
		next += 2;
	}
	if (argc > next) {
		metallib = argv[next++];
	}
	if (argc > next) {
		only = argv[next++];
	}

	/*
	 * THE TWO ROADS, and taking the wrong one is an abort rather than a mistake.
	 *
	 * MTLCreateSystemDefaultDevice is a dispatch_sync of a block whose whole body is a
	 * count of Metal's device array: 0 leaves nil, 1 returns that one, and ANYTHING ELSE
	 * is MTLReportFailure("doesn't know what to do with more than one device") and
	 * abort(). MTLAddDevice appends to that same array. So in a boot where the registry
	 * switch is on, Metal's own discovery has already registered our device, and a
	 * program that then adds its own makes two and dies on the next probe -- which is
	 * exactly what happened to triangle_app in the boot of 22 October 05:50, the only
	 * process in the whole system that aborted (docs/metal.md 10.47).
	 *
	 * Apple's daemons never meet this because none of them calls MTLAddDevice; neither
	 * does a game. It is a hazard of our own test programs alone, and the fix is an
	 * order: ask first, and build a device only when Metal has none.
	 */
	id<MTLDevice> found = MTLCreateSystemDefaultDevice();

	printf("drvtest: before anything, MTLCreateSystemDefaultDevice() = %s\n",
	       found != nil ? class_getName(object_getClass(found)) : "nil");
	if (found != nil) {
		printf("drvtest: Metal's own discovery already has a device, so this process "
		       "adds NOTHING: a second one would abort the next probe\n");
	}

	Class cls = Nil;

	if (found == nil && as_bundle) {
		/* Only one of the two images may be loaded: both define the same classes, and
		 * the Objective-C runtime would keep one of them and not say which. */
		cls = class_from_bundle("/annibale/lib/AnnibaleMetal.bundle");
		if (cls == Nil) {
			return 1;
		}
	} else if (found == nil) {
		void *lib = dlopen(path, RTLD_NOW);

		if (lib == NULL) {
			printf("drvtest: dlopen(%s) failed: %s\n", path, dlerror());
			printf("drvtest: if that is the class symbol, the device class has to be "
			       "built at runtime after all\n");
			return 1;
		}
		printf("drvtest: %s loaded, so _OBJC_CLASS_$__MTLDevice binds\n", path);
		cls = objc_getClass("AnnibaleMetalDevice");
	}
	/* --samples above one turns the driver's answer on itself: the driver reads this variable
	 * in -supportsSampleCount: and answers four only when it is set, so a run that asks for
	 * four samples has to have asked for the device that gives them. Set here rather than
	 * taken from the environment because the boot's job list has no way to give this
	 * process one: init's run_command() builds its own envp and passes no other.
	 */
	if (draw_samples > 1) {
		setenv("ANNIBALE_MSAA", "1", 1);
	}
	Class base = objc_getClass("_MTLDevice");
	if (found != nil) {
		goto have_device;           /* steps 4 and 5 are Metal's own work in this boot */
	}
	if (cls == Nil || base == Nil) {
		printf("drvtest: AnnibaleMetalDevice %s, _MTLDevice %s\n",
		       cls != Nil ? "is here" : "is missing", base != Nil ? "is here" : "is missing");
		return 1;
	}
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");
	printf("drvtest: step 4 isSubclassOfClass:_MTLDevice %s; conforms to MTLDeviceSPI %s, "
	       "to MTLDevice %s\n",
	       [cls isSubclassOfClass:base] ? "PASSES" : "fails",
	       spi != NULL && [cls conformsToProtocol:spi] ? "yes" : "no",
	       [cls conformsToProtocol:@protocol(MTLDevice)] ? "yes" : "no");

	io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
	if (svc == IO_OBJECT_NULL) {
		printf("drvtest: no IOAcceleratorES service to hand it\n");
		return 1;
	}
	id dev = ((id (*)(id, SEL, io_service_t))objc_msgSend)(
	    [cls alloc], sel_getUid("initWithAcceleratorPort:"), svc);
	printf("drvtest: step 5 gave %s\n",
	       dev != nil ? class_getName(object_getClass(dev)) : "nil");
	if (dev == nil) {
		return 1;
	}

	uintptr_t slide = (uintptr_t)&MTLCreateSystemDefaultDevice - MTL_CREATE_DEVICE_UNSLID;
	void (*add_device)(id) = (void (*)(id))(slide + MTL_ADD_DEVICE_UNSLID);

	@try {
		add_device(dev);
		printf("drvtest: MTLAddDevice returned\n");
	} @catch (NSException *e) {
		printf("drvtest: MTLAddDevice threw %s: %s\n", [[e name] UTF8String],
		       [[e reason] UTF8String]);
		return 1;
	}

have_device:
	;
	id<MTLDevice> sys = found != nil ? found : MTLCreateSystemDefaultDevice();
	printf("drvtest: MTLCreateSystemDefaultDevice() = %s\n",
	       sys != nil ? class_getName(object_getClass(sys)) : "nil");
	if (sys == nil) {
		return 1;
	}
	printf("drvtest: it calls itself \"%s\"\n", [[sys name] UTF8String]);

	/* What the device still owes, counted by the runtime on the device rather than guessed
	 * from the cache: every method the two protocols require, asked of the real object. An
	 * unanswered one is not a gap in a table, it is an application aborting on
	 * "unrecognized selector", because the gates inside Metal are assertions. */
	for (int which = 0; which < 2; which++) {
		const char *pname = which == 0 ? "MTLDevice" : "MTLDeviceSPI";
		Protocol *proto = objc_getProtocol(pname);
		unsigned n = 0, missing = 0, shown = 0;

		if (proto == NULL) {
			continue;
		}
		struct objc_method_description *req =
		    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

		for (unsigned i = 0; i < n; i++) {
			if ([sys respondsToSelector:req[i].name]) {
				continue;
			}
			missing++;
			if (shown < 8) {
				printf("drvtest:   %s still owes %s\n", pname,
				       sel_getName(req[i].name));
				shown++;
			}
		}
		free(req);
		printf("drvtest: of %u methods %s requires, %u are unanswered (unanswered means "
		       "absent from the chain; an entry whose body refuses is not counted here)\n",
		       n, pname, missing);
	}

	/* and the same count for every other class of the driver, which is where the two
	 * methods the daemons named were missing from */
	sweep_every_class();

	/* The first resource. A buffer here is a GEM allocation of the kernel's own gpu
	 * driver, so this checks the whole path: the kernel gave memory, the GPU has an
	 * address for it, this processor can write it and read back what it wrote. */
	id<MTLBuffer> buf = [sys newBufferWithLength:4096 options:MTLResourceStorageModeShared];

	printf("drvtest: newBufferWithLength:options: gave %s\n",
	       buf != nil ? class_getName(object_getClass(buf)) : "nil");
	if (buf != nil) {
		unsigned char *p = [buf contents];

		printf("drvtest:   length %lu, contents %p, gpu address %s\n",
		       (unsigned long)[buf length], p,
		       [buf gpuAddress] != 0 ?
		       [[NSString stringWithFormat:@"%#llx",
			 (unsigned long long)[buf gpuAddress]] UTF8String] :
		       "not exposed (this one came from the compiler library)");
		if (p != NULL) {
			int same = 1;

			for (int i = 0; i < 4096; i++) {
				p[i] = (unsigned char)(i * 7 + 13);
			}
			for (int i = 0; i < 4096; i++) {
				if (p[i] != (unsigned char)(i * 7 + 13)) {
					same = 0;
					break;
				}
			}
			printf("drvtest:   wrote 4096 bytes and read back %s\n",
			       same ? "the same bytes" : "SOMETHING ELSE");
		}
		/* and what such a buffer still owes, asked of the runtime rather than guessed */
		unsigned n = 0;
		Protocol *bp = objc_getProtocol("MTLBuffer");
		struct objc_method_description *req =
		    bp != NULL ? protocol_copyMethodDescriptionList(bp, YES, YES, &n) : NULL;
		unsigned missing = 0;

		for (unsigned i = 0; i < n; i++) {
			if (![buf respondsToSelector:req[i].name]) {
				if (missing < 12) {
					printf("drvtest:   still owed: %s\n",
					       sel_getName(req[i].name));
				}
				missing++;
			}
		}
		free(req);
		printf("drvtest:   of %u methods MTLBuffer requires, %u are unanswered\n",
		       n, missing);
	}

	/* And what Metal can do with it: the container path needs no driver code at all, which
	 * is where subsystem (1) plugs in one level further down. */
	NSData *data = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:metallib]];
	if (data == nil) {
		printf("drvtest: cannot read %s\n", metallib);
		return 0;
	}
	printf("drvtest: %s, %lu bytes\n", metallib, (unsigned long)[data length]);
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	NSError *err = nil;
	id<MTLLibrary> mtllib = [sys newLibraryWithData:dd error:&err];

	printf("drvtest: newLibraryWithData: gave %s with %lu functions\n",
	       mtllib != nil ? class_getName(object_getClass(mtllib)) : "nil",
	       mtllib != nil ? (unsigned long)[[mtllib functionNames] count] : 0UL);
	if (mtllib == nil) {
		return 0;
	}
	id<MTLFunction> fn = [mtllib newFunctionWithName:[[mtllib functionNames] firstObject]];

	printf("drvtest: newFunctionWithName: gave %s\n",
	       fn != nil ? class_getName(object_getClass(fn)) : "nil");

	if (timing) {
		time_path(sys, metallib, timing_repeats);
		return 0;
	}
	if (picture) {
		picture_path(sys, metallib, picture_seconds);
		return 0;
	}
	if (draw_depth) {
		depth_path(sys, metallib);
		return 0;
	}
	if (draw_attrib) {
		attrib_path(sys, metallib);
		return 0;
	}
	if (argbuf) {
		/* the compiler library's own record count, by the same loader the texture
		 * query uses: one dlopen of the image the driver already holds */
		load_texture_query(sys);
		argbuf_path(sys, metallib);
		return 0;
	}
	if (draw) {
		draw_path(sys, metallib);
		return 0;
	}
	if (only == NULL) {
		air_of_each_function(mtllib);
	}
	compute_path(sys, mtllib, only);
	return 0;
}
