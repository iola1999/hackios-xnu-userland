/* Host test of the production encoder, Work ownership and copy executor against
 * libredwoodair's software device. The small command-buffer fixture drains the
 * recorded list; Apple's Metal queue and GPU fences need device validation. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../tools/mesa/air/air_metal.h"
#include "redwoodmetal_work_private.h"
#include "redwoodmetal_work.inc"

static char why[512];
static unsigned destroyed_buffers, buffer_calls, texture_calls;

/* The driver's own log: kept so a case can assert what it said, and echoed so a failure shows it. */
static char printed[512];
static void rw_print(const char *fmt, ...)
{
	va_list arguments;
	va_start(arguments, fmt);
	vsnprintf(printed, sizeof(printed), fmt, arguments);
	va_end(arguments);
	printf("metal_blit_test: driver: %s", printed);
}

/* The shape the stub texture reports, and the one its rwa_texture is created with. */
static const struct rwa_texture_shape test_shape = { .pixel_format = 80, .type = 3,
	.width = 16, .height = 12, .depth = 1, .array_length = 2,
	.mip_levels = 2, .sample_count = 1, .usage = 7 };

@interface CopyDevice : NSObject {
@public
	struct rwa_device *raw;
}
@end
@implementation CopyDevice
- (instancetype)init
{
	self = [super init];
	if (self) raw = rwa_device_open(why, sizeof(why));
	assert(raw);
	return self;
}
- (void)dealloc { rwa_device_close(raw); [super dealloc]; }
@end

@interface RedwoodMetalBuffer : NSObject {
	CopyDevice *_device;
@public
	struct rwa_buffer *raw;
}
- (instancetype)initWithDevice:(CopyDevice *)device;
- (id)device;
- (struct rwa_buffer *)mesaBuffer;
@end
@implementation RedwoodMetalBuffer
- (instancetype)initWithDevice:(CopyDevice *)device
{
	self = [super init];
	if (self) {
		_device = [device retain];
		raw = rwa_buffer_create(device->raw, 1024, why, sizeof(why));
		assert(raw);
	}
	return self;
}
- (id)device { return _device; }
- (struct rwa_buffer *)mesaBuffer { return raw; }
- (void)dealloc
{
	destroyed_buffers++;
	rwa_buffer_destroy(raw);
	[_device release];
	[super dealloc];
}
@end

@interface RedwoodMetalTexture : NSObject {
	CopyDevice *_device;
@public
	struct rwa_texture *raw;
}
- (instancetype)initWithDevice:(CopyDevice *)device;
- (id)device;
- (struct rwa_texture *)mesaTexture;
- (NSUInteger)width;
- (NSUInteger)height;
- (NSUInteger)mipmapLevelCount;
- (NSUInteger)pixelFormat;
@end
@implementation RedwoodMetalTexture
- (instancetype)initWithDevice:(CopyDevice *)device
{
	self = [super init];
	if (self) {
		_device = [device retain];
		raw = rwa_texture_create(device->raw, &test_shape, why, sizeof(why));
		assert(raw);
	}
	return self;
}
- (id)device { return _device; }
- (struct rwa_texture *)mesaTexture { return raw; }
- (NSUInteger)width { return test_shape.width; }
- (NSUInteger)height { return test_shape.height; }
- (NSUInteger)mipmapLevelCount { return test_shape.mip_levels; }
- (NSUInteger)pixelFormat { return test_shape.pixel_format; }
- (void)dealloc { rwa_texture_destroy(raw); [_device release]; [super dealloc]; }
@end

@interface RedwoodMetalCommandBuffer : NSObject {
	CopyDevice *_device;
@public
	NSMutableArray *work;
}
- (instancetype)initWithDevice:(CopyDevice *)device;
- (void)recordWork:(RedwoodMetalWork *)item;
- (id)device;
- (BOOL)drainCopies;
@end

#define _MTLCommandEncoder CopyTestCommandEncoder
@interface _MTLCommandEncoder : NSObject {
	id _owner;
}
- (instancetype)initWithCommandBuffer:(id)commandBuffer;
- (void)endEncoding;
@end
@implementation _MTLCommandEncoder
- (instancetype)initWithCommandBuffer:(id)commandBuffer
{
	self = [super init];
	if (self) _owner = [commandBuffer retain];
	return self;
}
- (void)endEncoding { [_owner release]; _owner = nil; }
- (void)dealloc { [_owner release]; [super dealloc]; }
@end

static struct {
	__typeof__(&rwa_buffer_copy) buffer_copy;
	__typeof__(&rwa_texture_copy) texture_copy;
	__typeof__(&rwa_texture_generate_mipmaps) texture_generate_mipmaps;
} mesa = { rwa_buffer_copy, rwa_texture_copy, rwa_texture_generate_mipmaps };
#include "redwoodmetal_blit.inc"
#undef _MTLCommandEncoder

@implementation RedwoodMetalCommandBuffer
- (instancetype)initWithDevice:(CopyDevice *)device
{
	self = [super init];
	if (self) {
		_device = [device retain];
		work = [[NSMutableArray alloc] init];
	}
	return self;
}
- (id)device { return _device; }
- (void)recordWork:(RedwoodMetalWork *)item { [item retainResources]; [work addObject:item]; }
- (BOOL)drainCopies
{
	for (RedwoodMetalWork *item in work) {
		buffer_calls += item->copyKind == RW_COPY_BUFFER && mesa.buffer_copy != NULL;
		texture_calls += item->copyKind == RW_COPY_TEXTURE && mesa.texture_copy != NULL;
		if (rw_run_copy(item, why, sizeof(why)) != 0) return NO;
	}
	return YES;
}
- (void)dealloc { [work release]; [_device release]; [super dealloc]; }
@end

static RedwoodMetalBlitEncoder *encoder(RedwoodMetalCommandBuffer *command)
{
	return [[[RedwoodMetalBlitEncoder alloc] initWithRedwoodCommandBuffer:command] autorelease];
}

static void ordering_and_lifetime(CopyDevice *device)
{
	RedwoodMetalBuffer *a = [[RedwoodMetalBuffer alloc] initWithDevice:device];
	RedwoodMetalBuffer *b = [[RedwoodMetalBuffer alloc] initWithDevice:device];
	RedwoodMetalBuffer *c = [[RedwoodMetalBuffer alloc] initWithDevice:device];
	for (unsigned i = 0; i < 1024; i++) ((uint8_t *)rwa_buffer_map(a->raw))[i] = i * 7 + i / 13;
	memset(rwa_buffer_map(b->raw), 0x22, 1024);
	memset(rwa_buffer_map(c->raw), 0x33, 1024);
	RedwoodMetalCommandBuffer *first = [[RedwoodMetalCommandBuffer alloc] initWithDevice:device];
	RedwoodMetalCommandBuffer *second = [[RedwoodMetalCommandBuffer alloc] initWithDevice:device];
	RedwoodMetalBlitEncoder *one = encoder(first), *two = encoder(second);
	[one copyFromBuffer:b sourceOffset:53 toBuffer:c destinationOffset:100 size:257];
	[two copyFromBuffer:a sourceOffset:29 toBuffer:b destinationOffset:53 size:257];
	[one endEncoding];
	[two endEncoding];
	assert(((uint8_t *)rwa_buffer_map(b->raw))[53] == 0x22);
	[a release];
	assert(destroyed_buffers == 0);
	assert([second drainCopies] && [first drainCopies]);
	for (unsigned i = 100; i < 357; i++) {
		unsigned source = i - 100 + 29;
		assert(((uint8_t *)rwa_buffer_map(c->raw))[i] == (uint8_t)(source * 7 + source / 13));
	}
	assert(((uint8_t *)rwa_buffer_map(c->raw))[99] == 0x33);
	assert(((uint8_t *)rwa_buffer_map(c->raw))[357] == 0x33);
	[second release];
	assert(destroyed_buffers == 1);
	[first release];
	RedwoodMetalCommandBuffer *chain = [[RedwoodMetalCommandBuffer alloc] initWithDevice:device];
	RedwoodMetalBlitEncoder *three = encoder(chain);
	[three copyFromBuffer:c sourceOffset:100 toBuffer:b destinationOffset:500 size:20];
	[three copyFromBuffer:b sourceOffset:500 toBuffer:c destinationOffset:600 size:20];
	[three endEncoding];
	assert([chain drainCopies]);
	assert(((uint8_t *)rwa_buffer_map(c->raw))[600] == (uint8_t)(29 * 7 + 29 / 13));
	BOOL raised = NO;
	@try { [three copyFromBuffer:b sourceOffset:0 toBuffer:c destinationOffset:0 size:1]; }
	@catch (NSException *exception) { (void)exception; raised = YES; }
	assert(raised);
	[chain release];
	[b release];
	[c release];
	assert(destroyed_buffers == 3);
}

static void refusals(CopyDevice *device)
{
	RedwoodMetalBuffer *b = [[RedwoodMetalBuffer alloc] initWithDevice:device];
	memset(rwa_buffer_map(b->raw), 0x19, 1024);
	((uint8_t *)rwa_buffer_map(b->raw))[0] = 0xaa;
	RedwoodMetalCommandBuffer *command = [[RedwoodMetalCommandBuffer alloc] initWithDevice:device];
	RedwoodMetalBlitEncoder *blit = encoder(command);
	[blit copyFromBuffer:nil sourceOffset:0 toBuffer:b destinationOffset:0 size:4];
	[blit copyFromBuffer:b sourceOffset:0 toBuffer:b destinationOffset:32 size:4];
	[blit endEncoding];
	assert([command->work count] == 2 && ![command drainCopies]);
	assert(((uint8_t *)rwa_buffer_map(b->raw))[32] == 0x19);
	[command release];
	command = [[RedwoodMetalCommandBuffer alloc] initWithDevice:device];
	blit = encoder(command);
	[blit copyFromBuffer:b sourceOffset:SIZE_MAX toBuffer:b destinationOffset:32 size:4];
	[blit endEncoding];
	assert(![command drainCopies]);
	mesa.buffer_copy = NULL;
	assert(![command drainCopies] && strstr(why, "does not implement"));
	mesa.buffer_copy = rwa_buffer_copy;
	[command release];
	[b release];
}

static void texture_encoding(CopyDevice *device)
{
	RedwoodMetalTexture *source = [[RedwoodMetalTexture alloc] initWithDevice:device];
	RedwoodMetalTexture *target = [[RedwoodMetalTexture alloc] initWithDevice:device];
	uint8_t input[12][80], output[12][96], clear[12][80] = { 0 };
	for (unsigned y = 0; y < 12; y++) {
		for (unsigned x = 0; x < 80; x++) input[y][x] = x + y * 19;
	}
	assert(rwa_texture_write(source->raw, 1, 1, NULL, NULL, input, 80, why, sizeof(why)) == 0);
	assert(rwa_texture_write(target->raw, 0, 0, NULL, NULL, clear, 80, why, sizeof(why)) == 0);
	RedwoodMetalCommandBuffer *command = [[RedwoodMetalCommandBuffer alloc] initWithDevice:device];
	RedwoodMetalBlitEncoder *blit = encoder(command);
	[blit copyFromTexture:source sourceSlice:1 sourceLevel:1 sourceOrigin:MTLOriginMake(2, 1, 0)
		sourceSize:MTLSizeMake(3, 4, 1) toTexture:target destinationSlice:0
		destinationLevel:0 destinationOrigin:MTLOriginMake(5, 3, 0)];
	[blit endEncoding];
	[source release];
	assert([command drainCopies]);
	memset(output, 0xcc, sizeof(output));
	assert(rwa_texture_read(target->raw, 0, 0, NULL, NULL, output, 96, why, sizeof(why)) == 0);
	for (unsigned y = 0; y < 12; y++) {
		for (unsigned x = 0; x < 96; x++) {
			uint8_t expected = x >= 64 ? 0xcc : 0;
			if (y >= 3 && y < 7 && x >= 20 && x < 32) expected = input[y - 2][x - 12];
			assert(output[y][x] == expected);
		}
	}
	[command release];
	command = [[RedwoodMetalCommandBuffer alloc] initWithDevice:device];
	blit = encoder(command);
	[blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(SIZE_MAX, 0, 0)
		sourceSize:MTLSizeMake(1, 1, 1) toTexture:target destinationSlice:0
		destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];
	assert(![command drainCopies] && strstr(why, "32-bit"));
	[command release];
	[target release];
}

/* Mipmap generation records one item, and that item downsamples level 0 into the levels above.
 *
 * The record is what this asserts first: an encoder that accepted the call and recorded nothing
 * would answer every lookup with the same silence as a working one, and the caller would sample
 * stale texels without a word. The second half is the content: level 1 stays as it was until the
 * work runs, and after it runs each texel is somewhere between the four level-0 texels it covers.
 * A filter that samples one corner instead of averaging would fall outside that range, and so
 * would a level left untouched at zero.
 *
 * A selector MTLBlitCommandEncoder declares and this driver does not implement is still forwarded,
 * so the process survives it; a selector no protocol declares still raises. */
static void mipmap_generation(CopyDevice *device)
{
	RedwoodMetalTexture *target = [[RedwoodMetalTexture alloc] initWithDevice:device];
	RedwoodMetalCommandBuffer *command = [[RedwoodMetalCommandBuffer alloc] initWithDevice:device];
	id<MTLBlitCommandEncoder> blit = (id<MTLBlitCommandEncoder>)encoder(command);
	const unsigned origin[3] = { 0, 0, 0 };
	const unsigned full[3] = { 16, 12, 1 };
	const unsigned half[3] = { 8, 6, 1 };
	unsigned char level0[16 * 12 * 4], level1[8 * 6 * 4];

	for (unsigned layer = 0; layer < 2; layer++) {
		for (unsigned y = 0; y < 12; y++) {
			for (unsigned x = 0; x < 16; x++) {
				unsigned char *texel = level0 + (y * 16 + x) * 4;

				texel[0] = (unsigned char)(x * 16);
				texel[1] = (unsigned char)(y * 20);
				texel[2] = (unsigned char)(layer != 0 ? 0x40 : 0x80);
				texel[3] = 0xff;
			}
		}
		assert(rwa_texture_write(target->raw, 0, layer, origin, full, level0, 0, why,
					 sizeof(why)) == 0);
	}

	[blit generateMipmapsForTexture:(id<MTLTexture>)target];
	[blit endEncoding];
	assert([command->work count] == 1);
	assert(((RedwoodMetalWork *)[command->work objectAtIndex:0])->copyKind == RW_COPY_MIPMAP);
	assert([command drainCopies]);

	for (unsigned layer = 0; layer < 2; layer++) {
		memset(level1, 0, sizeof(level1));
		assert(rwa_texture_read(target->raw, 1, layer, origin, half, level1, 0, why,
					sizeof(why)) == 0);
		for (unsigned y = 0; y < 6; y++) {
			for (unsigned x = 0; x < 8; x++) {
				const unsigned char *out = level1 + (y * 8 + x) * 4;

				for (unsigned channel = 0; channel < 4; channel++) {
					unsigned lowest = 255, highest = 0;

					for (unsigned dy = 0; dy < 2; dy++) {
						for (unsigned dx = 0; dx < 2; dx++) {
							unsigned value = level0[((y * 2 + dy) * 16 +
										 (x * 2 + dx)) * 4 + channel];

							lowest = value < lowest ? value : lowest;
							highest = value > highest ? value : highest;
						}
					}
					assert(out[channel] >= lowest && out[channel] <= highest);
				}
			}
		}
	}
	[command release];
	[target release];
}

static void unimplemented_selectors(CopyDevice *device)
{
	RedwoodMetalTexture *target = [[RedwoodMetalTexture alloc] initWithDevice:device];
	RedwoodMetalBuffer *buffer = [[RedwoodMetalBuffer alloc] initWithDevice:device];
	RedwoodMetalCommandBuffer *command = [[RedwoodMetalCommandBuffer alloc] initWithDevice:device];
	id<MTLBlitCommandEncoder> blit = (id<MTLBlitCommandEncoder>)encoder(command);
	blit = (id<MTLBlitCommandEncoder>)encoder(command);
	[blit fillBuffer:(id<MTLBuffer>)buffer range:NSMakeRange(0, 16) value:0x5a];
	assert(strstr(printed, "blit selector fillBuffer:range:value: is not implemented"));
	[blit endEncoding];
	assert([command->work count] == 0 && [command drainCopies]);
	assert(((uint8_t *)rwa_buffer_map(buffer->raw))[0] != 0x5a);
	[command release];

	command = [[RedwoodMetalCommandBuffer alloc] initWithDevice:device];
	blit = (id<MTLBlitCommandEncoder>)encoder(command);
	BOOL raised = NO;
	@try { [(id)blit performSelector:sel_registerName("redwoodSelectorNoProtocolDeclares")]; }
	@catch (NSException *exception) { raised = [exception.name isEqualToString:NSInvalidArgumentException]; }
	assert(raised);
	[blit endEncoding];
	[command release];
	[buffer release];
	[target release];
}

int main(void)
{
	setenv("RWA_SOFTWARE", "1", 1);
	@autoreleasepool {
		CopyDevice *device = [[CopyDevice alloc] init];
		ordering_and_lifetime(device);
		refusals(device);
		texture_encoding(device);
		mipmap_generation(device);
		unimplemented_selectors(device);
		[device release];
	}
	printf("metal_blit_test: software backend calls buffer=%u texture=%u; deferred copies, commit ordering, ownership, errors, mipmap generation and unimplemented selectors PASS\n", buffer_calls, texture_calls);
	return 0;
}
