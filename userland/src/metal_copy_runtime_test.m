/* Runtime probe of Metal copy objects, including Apple's private queue/encoder
 * bases. Instantiate the candidate device directly; --software selects softpipe.
 * Pixel checks cover copying only. GPU execution needs device submission counters. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char phase[128] = "arguments";
static unsigned copy_calls, committed_buffers;

static void
stage(const char *name)
{
	snprintf(phase, sizeof(phase), "%s", name);
	printf("metalcopy26: stage=%s\n", phase);
}

static BOOL
fail(const char *format, ...)
{
	va_list args;
	printf("metalcopy26: FAIL stage=%s: ", phase);
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	putchar('\n');
	return NO;
}

static double
milliseconds(void)
{
	struct timespec now = { 0 };
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

static BOOL
compare(const uint8_t *actual, const uint8_t *expected, size_t size, const char *name)
{
	size_t bad = 0, first = 0;
	for (size_t i = 0; i < size; i++) {
		if (actual[i] != expected[i]) {
			if (!bad) first = i;
			bad++;
		}
	}
	if (bad) return fail("%s: %zu/%zu bytes differ; first=%zu actual=%02x expected=%02x",
		name, bad, size, first, actual[first], expected[first]);
	printf("metalcopy26: %s verified=%zu bytes\n", name, size);
	return YES;
}

static BOOL
submit(id<MTLCommandBuffer> command)
{
	stage("commit");
	double begin = milliseconds();
	[command commit];
	committed_buffers++;
	double submitted = milliseconds();
	printf("metalcopy26: commit status=%lu error=%s elapsed_ms=%.3f\n",
		(unsigned long)[command status], [[command error] description].UTF8String ?: "-",
		submitted - begin);
	if ([command status] == MTLCommandBufferStatusError || [command error] != nil)
		return fail("command failed during commit");
	stage("waitUntilCompleted");
	[command waitUntilCompleted];
	printf("metalcopy26: wait status=%lu error=%s elapsed_ms=%.3f\n",
		(unsigned long)[command status], [[command error] description].UTF8String ?: "-",
		milliseconds() - submitted);
	if ([command status] != MTLCommandBufferStatusCompleted || [command error] != nil)
		return fail("command did not complete successfully");
	return YES;
}

static BOOL
buffer_copy(id<MTLDevice> device, id<MTLCommandQueue> queue)
{
	id<MTLBuffer> source = nil, middle = nil, target = nil;
	id<MTLCommandBuffer> command = nil;
	id<MTLBlitCommandEncoder> encoder = nil;
	BOOL ended = NO;
	@try {
		stage("buffer allocation");
		source = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
		middle = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
		target = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
		if (!source || !middle || !target || ![source contents] || ![middle contents] || ![target contents])
			return fail("buffer allocation or CPU mapping returned nil");
		uint8_t pattern[1024], expected_middle[1024], expected_target[1024];
		for (unsigned i = 0; i < sizeof(pattern); i++) pattern[i] = i * 7 + i / 13;
		memcpy([source contents], pattern, sizeof(pattern));
		memset([middle contents], 0xa5, sizeof(pattern));
		memset([target contents], 0x3c, sizeof(pattern));
		memset(expected_middle, 0xa5, sizeof(expected_middle));
		memset(expected_target, 0x3c, sizeof(expected_target));
		stage("buffer commandBuffer and blitCommandEncoder");
		command = [[queue commandBuffer] retain];
		encoder = [[command blitCommandEncoder] retain];
		if (!command || !encoder || [command commandQueue] != queue || [encoder device] != device)
			return fail("command/encoder creation or device association failed");
		printf("metalcopy26: command=%s encoder=%s\n", object_getClassName(command), object_getClassName(encoder));
		[encoder copyFromBuffer:source sourceOffset:29 toBuffer:middle destinationOffset:53 size:257];
		[encoder copyFromBuffer:middle sourceOffset:53 toBuffer:target destinationOffset:101 size:257];
		copy_calls += 2;
		ended = YES;
		[encoder endEncoding];
		[source release]; source = nil;
		stage("buffer before commit");
		if (!compare([middle contents], expected_middle, sizeof(expected_middle), "middle before commit") ||
		    !compare([target contents], expected_target, sizeof(expected_target), "target before commit")) return NO;
		if (!submit(command)) return NO;
		stage("buffer readback");
		memcpy(expected_middle + 53, pattern + 29, 257);
		memcpy(expected_target + 101, pattern + 29, 257);
		return compare([middle contents], expected_middle, sizeof(expected_middle), "buffer copy 29->53") &&
			compare([target contents], expected_target, sizeof(expected_target), "dependent copy 53->101");
	} @finally {
		if (encoder && !ended) [encoder endEncoding];
		[encoder release];
		[command release];
		[source release];
		[middle release];
		[target release];
	}
}

static BOOL
texture_copy(id<MTLDevice> device, id<MTLCommandQueue> queue, MTLPixelFormat format, unsigned texel)
{
	enum { Width = 19, Height = 11, MaxWritePitch = Width * 8 + 16, MaxReadPitch = Width * 8 + 40 };
	const size_t write_pitch = Width * texel + 16, read_pitch = Width * texel + 40;
	uint8_t input[Height * MaxWritePitch] = { 0 }, clear[Height * MaxWritePitch] = { 0 };
	uint8_t output[Height * MaxReadPitch], expected[Height * MaxReadPitch];
	id<MTLTexture> source = nil, target = nil;
	id<MTLCommandBuffer> command = nil;
	id<MTLBlitCommandEncoder> encoder = nil;
	BOOL ended = NO;
	@try {
		stage("texture allocation");
		printf("metalcopy26: pixel_format=%lu size=%ux%u write_pitch=%zu read_pitch=%zu\n",
			(unsigned long)format, Width, Height, write_pitch, read_pitch);
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
			width:Width height:Height mipmapped:NO];
		descriptor.storageMode = MTLStorageModeShared;
		descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
		source = [device newTextureWithDescriptor:descriptor];
		target = [device newTextureWithDescriptor:descriptor];
		if (!source || !target) return fail("texture allocation returned nil");
		for (unsigned y = 0; y < Height; y++) {
			for (unsigned x = 0; x < Width * texel; x++) input[y * write_pitch + x] = x * 3 + y * 17;
			if (format == MTLPixelFormatRGBA16Float) {
				for (unsigned x = 0; x < Width * 4; x++) {
					uint16_t finite_half = 0x3000 + y * 37 + x * 3;
					memcpy(input + y * write_pitch + x * 2, &finite_half, 2);
				}
			}
		}
		MTLRegion whole = MTLRegionMake2D(0, 0, Width, Height);
		stage("texture upload");
		[source replaceRegion:whole mipmapLevel:0 withBytes:input bytesPerRow:write_pitch];
		[target replaceRegion:whole mipmapLevel:0 withBytes:clear bytesPerRow:write_pitch];
		stage("texture commandBuffer and blitCommandEncoder");
		command = [[queue commandBuffer] retain];
		encoder = [[command blitCommandEncoder] retain];
		if (!command || !encoder || [command commandQueue] != queue || [encoder device] != device)
			return fail("texture command/encoder creation or device association failed");
		[encoder copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(2, 3, 0)
			sourceSize:MTLSizeMake(7, 5, 1) toTexture:target destinationSlice:0
			destinationLevel:0 destinationOrigin:MTLOriginMake(5, 1, 0)];
		copy_calls++;
		ended = YES;
		[encoder endEncoding];
		[source release]; source = nil;
		memset(expected, 0xcc, Height * read_pitch);
		for (unsigned y = 0; y < Height; y++) memset(expected + y * read_pitch, 0, Width * texel);
		memset(output, 0xcc, Height * read_pitch);
		stage("texture before commit");
		[target getBytes:output bytesPerRow:read_pitch fromRegion:whole mipmapLevel:0];
		if (!compare(output, expected, Height * read_pitch, "texture before commit")) return NO;
		if (!submit(command)) return NO;
		stage("texture readback");
		for (unsigned y = 0; y < 5; y++)
			memcpy(expected + (y + 1) * read_pitch + 5 * texel,
				input + (y + 3) * write_pitch + 2 * texel, 7 * texel);
		memset(output, 0xcc, Height * read_pitch);
		[target getBytes:output bytesPerRow:read_pitch fromRegion:whole mipmapLevel:0];
		return compare(output, expected, Height * read_pitch, "texture region and row padding");
	} @finally {
		if (encoder && !ended) [encoder endEncoding];
		[encoder release];
		[command release];
		[source release];
		[target release];
	}
}

int main(int argc, char **argv)
{
	BOOL software = NO;
	const char *library = "/redwood/lib/libredwoodmetal.dylib";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--software")) software = YES;
		else if (!strcmp(argv[i], "--library") && i + 1 < argc) library = argv[++i];
		else {
			fprintf(stderr, "usage: %s [--software] [--library METAL_DYLIB]\n", argv[0]);
			return 2;
		}
	}
	int console = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (console >= 0) {
		dup2(console, STDOUT_FILENO);
		dup2(console, STDERR_FILENO);
		if (console > STDERR_FILENO) close(console);
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	if (software) setenv("RWA_SOFTWARE", "1", 1);
	else unsetenv("RWA_SOFTWARE");
	void *plugin = NULL, *backend = NULL;
	int result = 1;
	@autoreleasepool {
		id<MTLDevice> device = nil;
		id<MTLCommandQueue> queue = nil;
		@try {
			NSData *version_data = [NSData dataWithContentsOfFile:@"/System/Library/CoreServices/SystemVersion.plist"];
			NSDictionary *version = version_data ? [NSPropertyListSerialization propertyListWithData:version_data
				options:NSPropertyListImmutable format:NULL error:NULL] : nil;
			printf("metalcopy26: build=%s mode=%s library=%s\n",
				[version[@"ProductBuildVersion"] UTF8String] ?: "unknown", software ? "software" : "hardware", library);
			stage("dlopen Metal candidate");
			plugin = dlopen(library, RTLD_NOW | RTLD_LOCAL);
			if (!plugin) { fail("dlopen: %s", dlerror()); goto finished; }
			Class cls = objc_getClass("RedwoodMetalDevice");
			SEL init = sel_registerName("initWithAcceleratorPort:");
			if (!cls || ![cls instancesRespondToSelector:init]) {
				fail("RedwoodMetalDevice initializer is unavailable"); goto finished;
			}
			stage("initWithAcceleratorPort:0");
			device = ((id (*)(id, SEL, uint32_t))objc_msgSend)([cls alloc], init, 0);
			if (!device) { fail("device initializer returned nil"); goto finished; }
			SEL raw_device_selector = sel_registerName("mesaDevice");
			if (![device respondsToSelector:raw_device_selector]) { fail("mesaDevice is unavailable"); goto finished; }
			void *raw_device = ((void *(*)(id, SEL))objc_msgSend)(device, raw_device_selector);
			if (!raw_device) { fail("the candidate has no Mesa device"); goto finished; }
			backend = dlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW | RTLD_NOLOAD);
			const char *(*backend_name)(void *) = backend ? dlsym(backend, "rwa_device_name") : NULL;
			if (!backend_name) { fail("cannot identify the loaded Mesa backend"); goto finished; }
			const char *name = backend_name(raw_device);
			if (!name || (!!strstr(name, "softpipe") != !!software)) {
				fail("requested mode does not match backend %s", name ?: "null"); goto finished;
			}
			printf("metalcopy26: device_class=%s backend=%s _MTLDevice_size=%zu\n",
				object_getClassName(device), name, class_getInstanceSize(objc_getClass("_MTLDevice")));
			stage("newCommandQueue");
			queue = [device newCommandQueue];
			if (!queue || [queue device] != device) { fail("queue creation or device association failed"); goto finished; }
			printf("metalcopy26: queue_class=%s\n", object_getClassName(queue));
			if (!buffer_copy(device, queue) || !texture_copy(device, queue, MTLPixelFormatBGRA8Unorm, 4) ||
			    !texture_copy(device, queue, MTLPixelFormatRGBA16Float, 8)) goto finished;
			if (copy_calls != 4 || committed_buffers != 3) { fail("incomplete copy coverage"); goto finished; }
			result = 0;
		finished:;
		} @catch (NSException *exception) {
			fail("exception %s: %s", exception.name.UTF8String, exception.reason.UTF8String);
		} @finally {
			[queue release];
			[device release];
		}
	}
	if (backend) dlclose(backend);
	if (plugin) dlclose(plugin);
	printf("metalcopy26: %s mode=%s copy_calls=%u committed_buffers=%u\n",
		result ? "FAIL" : "PASS", software ? "software" : "hardware", copy_calls, committed_buffers);
	return result;
}
