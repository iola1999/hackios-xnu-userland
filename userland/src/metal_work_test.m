/* Host test of the production work item's manual ownership. Build with:
 * xcrun clang -fno-objc-arc -Wall -Wextra -Werror -framework Foundation
 *   userland/src/metal_work_test.m -o /tmp/metal_work_test
 * No Metal device is used; GPU completion and fence behavior are outside this test. */
#include "redwoodmetal_work_private.h"
#include "redwoodmetal_work.inc"
#include <assert.h>
#include <stdio.h>

struct counts {
	unsigned retained, released, destroyed;
};

@interface WorkProbe : NSObject {
	struct counts *_counts;
}
- (instancetype)initWithCounts:(struct counts *)counts;
- (unsigned)probeValue;
@end

@implementation WorkProbe
- (instancetype)initWithCounts:(struct counts *)counts
{
	self = [super init];
	if (self) {
		_counts = counts;
	}
	return self;
}
- (id)retain { _counts->retained++; return [super retain]; }
- (oneway void)release { _counts->released++; [super release]; }
- (unsigned)probeValue { return 0x1234; }
- (void)dealloc { _counts->destroyed++; [super dealloc]; }
@end

enum {
	ColorSlots = sizeof(((RedwoodMetalWork *)0)->color) / sizeof(id),
	SlotCount = 5 + ColorSlots + 4 * RW_MAX_BUFFERS
};

static void
slots(RedwoodMetalWork *work, id **out)
{
	unsigned n = 0;
	out[n++] = (id *)&work->pipeline;
	out[n++] = (id *)&work->render;
	out[n++] = &work->depth;
	out[n++] = &work->depthStencilState;
	out[n++] = &work->indexBuffer;
	work->colorCount = ColorSlots;
	for (unsigned i = 0; i < ColorSlots; i++) {
		out[n++] = &work->color[i];
	}
	work->vertexBufferCount = work->count = work->textureCount = work->samplerCount = RW_MAX_BUFFERS;
	for (unsigned i = 0; i < RW_MAX_BUFFERS; i++) {
		out[n++] = &work->vertexBuffers[i];
	}
	for (unsigned i = 0; i < RW_MAX_BUFFERS; i++) {
		out[n++] = &work->buffers[i];
	}
	for (unsigned i = 0; i < RW_MAX_BUFFERS; i++) {
		out[n++] = &work->textures[i];
	}
	for (unsigned i = 0; i < RW_MAX_BUFFERS; i++) {
		out[n++] = &work->samplers[i];
	}
	assert(n == SlotCount);
}

static void
distinct_references(void)
{
	struct counts counts[SlotCount] = { 0 };
	id *fields[SlotCount];
	RedwoodMetalWork *work = [[RedwoodMetalWork alloc] init];
	slots(work, fields);
	@autoreleasepool {
		for (unsigned i = 0; i < SlotCount; i++) {
			*fields[i] = [[[WorkProbe alloc] initWithCounts:&counts[i]] autorelease];
		}
		[work retainResources];
		[work retainResources];
	}
	for (unsigned i = 0; i < SlotCount; i++) {
		assert(counts[i].retained == 1 && counts[i].released == 1);
		assert(counts[i].destroyed == 0 && [*fields[i] probeValue] == 0x1234);
	}
	[work release];
	for (unsigned i = 0; i < SlotCount; i++) {
		assert(counts[i].released == 2 && counts[i].destroyed == 1);
	}
}

static void
repeated_references(void)
{
	struct counts counts = { 0 };
	id *fields[SlotCount];
	RedwoodMetalWork *first = [[RedwoodMetalWork alloc] init];
	RedwoodMetalWork *second = [[RedwoodMetalWork alloc] init];
	WorkProbe *probe = [[WorkProbe alloc] initWithCounts:&counts];
	slots(first, fields);
	for (unsigned i = 0; i < SlotCount; i++) {
		*fields[i] = probe;
	}
	second->depth = probe;
	[first retainResources];
	[second retainResources];
	[probe release];
	assert(counts.retained == SlotCount + 1 && counts.destroyed == 0);
	[first release];
	assert(counts.released == SlotCount + 1 && counts.destroyed == 0);
	assert([second->depth probeValue] == 0x1234);
	[second release];
	assert(counts.released == SlotCount + 2 && counts.destroyed == 1);
}

static void
discarded_and_sparse(void)
{
	struct counts counts = { 0 };
	WorkProbe *probe = [[WorkProbe alloc] initWithCounts:&counts];
	RedwoodMetalWork *discarded = [[RedwoodMetalWork alloc] init];
	discarded->depth = probe;
	[discarded release];
	assert(counts.retained == 0 && counts.released == 0 && [probe probeValue] == 0x1234);
	RedwoodMetalWork *sparse = [[RedwoodMetalWork alloc] init];
	sparse->count = RW_MAX_BUFFERS;
	sparse->buffers[RW_MAX_BUFFERS - 1] = probe;
	[sparse retainResources];
	[probe release];
	assert(counts.destroyed == 0 && [sparse->buffers[RW_MAX_BUFFERS - 1] probeValue] == 0x1234);
	[sparse release];
	assert(counts.retained == 1 && counts.released == 2 && counts.destroyed == 1);
	RedwoodMetalWork *empty = [[RedwoodMetalWork alloc] init];
	[empty retainResources];
	[empty release];
}

int main(void)
{
	@autoreleasepool {
		distinct_references();
		repeated_references();
		discarded_and_sparse();
	}
	printf("metal_work_test: PASS, %u object slots, duplicate references, discarded and sparse work\n", SlotCount);
	return 0;
}
