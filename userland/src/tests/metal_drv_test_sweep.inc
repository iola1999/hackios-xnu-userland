/* Metal protocol selector sweep tests, included into metal_drv_test.m. */


/*
 * WHAT EVERY CLASS OF THE DRIVER OWES, COUNTED ON EVERY RUN.
 *
 * 10.17 counted this for the device object and found nothing owed; the objects below it
 * were never counted, and the first boot in which iOS's own daemons could reach them
 * (logs/130132-run.log, metal=1 with launchd alive) answered why that mattered: 88 aborts
 * in a hundred seconds, all of them -[AnnibaleMetalCommandQueue setGPUPriority:] and
 * -setCompletionQueue:, each followed by launchd restarting the daemon.
 *
 * Counting them is not simply "ask the runtime", for two reasons, and both of them are the
 * reason this code is longer than the one loop it replaces:
 *
 *  1. THE PROTOCOL MAY NOT BE IN THE PROCESS. MTLDevice and MTLDeviceSPI are declared in
 *     Metal.framework, so every process that asks Metal has them. MTLCommandQueueSPI,
 *     MTLCommandBufferSPI, MTLRenderCommandEncoderSPI, MTLComputeCommandEncoderSPI,
 *     MTLRenderPipelineStateSPI, MTLComputePipelineStateSPI and even the PUBLIC
 *     MTLCommandQueue and MTLRenderCommandEncoder are declared in AGXMetalA11.bundle --
 *     the GPU bundle of a phone whose GPU is Apple's, which never loads here. So
 *     objc_getProtocol returns NULL for them and `protocol_copyMethodDescriptionList` has
 *     nothing to copy. Their required methods are compiled in below, read out of this
 *     phone's own shared cache by tools/metal/protometh.py; where the runtime does have a
 *     protocol it is asked instead, and the two counts are compared out loud.
 *
 *  2. AN ENTRY IN A METHOD TABLE IS NOT BEHAVIOUR (docs/metal.md 10.1). Apple writes an
 *     abstract method as a real entry whose body is [self doesNotRecognizeSelector:_cmd];
 *     `respondsToSelector:` answers YES for it and the abort when it is called names the
 *     RECEIVER -- our class -- so in the log it is indistinguishable from a method that was
 *     never written. _MTLCommandEncoder has 27 such entries for the mesh and object shader
 *     stages alone. A count that only asked `respondsToSelector:` would have said zero
 *     owed while 28 aborts sat waiting, so each implementation found in the chain is also
 *     READ: an abstract body branches to the image's doesNotRecognizeSelector: stub, and
 *     that is what is looked for.
 */
struct rw_protocol {
	const char *name;
	const char *const *selectors;
	unsigned count;
};
struct rw_class {
	const char *name;
	const char *const *protocols;   /* the protocol and what it adopts, NULL-terminated */
};
#define RW_PROTOCOL(name, sym) { name, sym, sizeof(sym) / sizeof(sym[0]) }

/* The required methods of each protocol that is NOT in any process on this phone,
 * read out of AGXMetalA11.bundle in the device's own shared cache. Generated;
 * tools/metal/owes.py prints the same lists and says how they were taken. */
static const char *const spi_commandbufferspi[] = {   /* MTLCommandBufferSPI: 14 required */
	"addPurgedHeap:", "addPurgedResource:", "commitAndWaitUntilSubmittedWithDeadline:",
	"commitWithDeadline:", "deadlineProfile",
	"encodeDashboardFinalizeForResourceGroup:dashboard:value:forIndex:",
	"encodeDashboardFinalizeForResourceGroup:dashboard:values:indices:count:",
	"encodeDashboardTagForResourceGroup:", "isProfilingEnabled", "presentDrawable:options:",
	"profilingResults", "setLogs:", "setProfilingEnabled:", "userDictionary",
};
static const char *const spi_commandqueue[] = {   /* MTLCommandQueue: 7 required */
	"commandBuffer", "commandBufferWithDescriptor:", "commandBufferWithUnretainedReferences",
	"device", "insertDebugCaptureBoundary", "label", "setLabel:",
};
static const char *const spi_commandqueuespi[] = {   /* MTLCommandQueueSPI: 24 required */
	"backgroundTrackingPID", "commitQueue", "commitSynchronously", "completionQueue",
	"disableCrossQueueHazardTracking", "executionEnabled", "finish", "getBackgroundGPUPriority",
	"getGPUPriority", "isOpenGLQueue", "isProfilingEnabled", "maxCommandBufferCount", "qosLevel",
	"setBackgroundGPUPriority:", "setBackgroundGPUPriority:offset:", "setBackgroundTrackingPID:",
	"setCompletionQueue:", "setExecutionEnabled:", "setGPUPriority:", "setGPUPriority:offset:",
	"setProfilingEnabled:", "setSkipRender:", "setSubmissionQueue:", "skipRender",
};
static const char *const spi_computecommandencoder[] = {   /* MTLComputeCommandEncoder: 35 required */
	"dispatchThreadgroups:threadsPerThreadgroup:",
	"dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
	"dispatchThreads:threadsPerThreadgroup:", "dispatchType",
	"executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:",
	"executeCommandsInBuffer:withRange:", "memoryBarrierWithResources:count:",
	"memoryBarrierWithScope:", "sampleCountersInBuffer:atSampleIndex:withBarrier:",
	"setAccelerationStructure:atBufferIndex:", "setBuffer:offset:atIndex:",
	"setBufferOffset:atIndex:", "setBuffers:offsets:withRange:", "setBytes:length:atIndex:",
	"setComputePipelineState:", "setImageblockWidth:height:",
	"setIntersectionFunctionTable:atBufferIndex:",
	"setIntersectionFunctionTables:withBufferRange:", "setSamplerState:atIndex:",
	"setSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setSamplerStates:lodMinClamps:lodMaxClamps:withRange:", "setSamplerStates:withRange:",
	"setStageInRegion:", "setStageInRegionWithIndirectBuffer:indirectBufferOffset:",
	"setTexture:atIndex:", "setTextures:withRange:", "setThreadgroupMemoryLength:atIndex:",
	"setVisibleFunctionTable:atBufferIndex:", "setVisibleFunctionTables:withBufferRange:",
	"updateFence:", "useHeap:", "useHeaps:count:", "useResource:usage:",
	"useResources:count:usage:", "waitForFence:",
};
static const char *const spi_computecommandencoderspi[] = {   /* MTLComputeCommandEncoderSPI: 4 required */
	"newKernelDebugInfo", "setThreadgroupDistributionMode:",
	"setThreadgroupDistributionModeWithClusterGroupIndex:", "setThreadgroupPackingDisabled:",
};
static const char *const spi_computepipelinestatespi[] = {   /* MTLComputePipelineStateSPI: 7 required */
	"allocatedSize", "debugInstrumentationData", "getComputeKernelTelemetryID", "gpuHandle",
	"staticThreadgroupMemoryLength", "textureWriteRoundingMode", "uniqueIdentifier",
};
static const char *const spi_pipelinestatewithprogramaddresstable[] = {   /* MTLPipelineStateWithProgramAddressTable: 1 required */
	"pipelineBinaries",
};
static const char *const spi_rendercommandencoder[] = {   /* MTLRenderCommandEncoder: 125 required */
	"dispatchThreadsPerTile:",
	"drawIndexedPatches:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
	"drawIndexedPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:instanceCount:baseInstance:",
	"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",
	"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",
	"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
	"drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
	"drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawPatches:patchIndexBuffer:patchIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
	"drawPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:instanceCount:baseInstance:",
	"drawPrimitives:indirectBuffer:indirectBufferOffset:",
	"drawPrimitives:vertexStart:vertexCount:",
	"drawPrimitives:vertexStart:vertexCount:instanceCount:",
	"drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:",
	"executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:",
	"executeCommandsInBuffer:withRange:",
	"memoryBarrierWithResources:count:afterStages:beforeStages:",
	"memoryBarrierWithScope:afterStages:beforeStages:",
	"sampleCountersInBuffer:atSampleIndex:withBarrier:", "setBlendColorRed:green:blue:alpha:",
	"setColorStoreAction:atIndex:", "setColorStoreActionOptions:atIndex:", "setCullMode:",
	"setDepthBias:slopeScale:clamp:", "setDepthClipMode:", "setDepthStencilState:",
	"setDepthStoreAction:", "setDepthStoreActionOptions:",
	"setFragmentAccelerationStructure:atBufferIndex:", "setFragmentBuffer:offset:atIndex:",
	"setFragmentBufferOffset:atIndex:", "setFragmentBuffers:offsets:withRange:",
	"setFragmentBytes:length:atIndex:", "setFragmentIntersectionFunctionTable:atBufferIndex:",
	"setFragmentIntersectionFunctionTables:withBufferRange:", "setFragmentSamplerState:atIndex:",
	"setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setFragmentSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setFragmentSamplerStates:withRange:", "setFragmentTexture:atIndex:",
	"setFragmentTextures:withRange:", "setFragmentVisibleFunctionTable:atBufferIndex:",
	"setFragmentVisibleFunctionTables:withBufferRange:", "setFrontFacingWinding:",
	"setMeshBuffer:offset:atIndex:", "setMeshBufferOffset:atIndex:",
	"setMeshBuffers:offsets:withRange:", "setMeshBytes:length:atIndex:",
	"setMeshSamplerState:atIndex:", "setMeshSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setMeshSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setMeshSamplerStates:withRange:", "setMeshTexture:atIndex:", "setMeshTextures:withRange:",
	"setObjectBuffer:offset:atIndex:", "setObjectBufferOffset:atIndex:",
	"setObjectBuffers:offsets:withRange:", "setObjectBytes:length:atIndex:",
	"setObjectSamplerState:atIndex:", "setObjectSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setObjectSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setObjectSamplerStates:withRange:", "setObjectTexture:atIndex:",
	"setObjectTextures:withRange:", "setObjectThreadgroupMemoryLength:atIndex:",
	"setRenderPipelineState:", "setScissorRect:", "setScissorRects:count:",
	"setStencilFrontReferenceValue:backReferenceValue:", "setStencilReferenceValue:",
	"setStencilStoreAction:", "setStencilStoreActionOptions:",
	"setTessellationFactorBuffer:offset:instanceStride:", "setTessellationFactorScale:",
	"setThreadgroupMemoryLength:offset:atIndex:", "setTileAccelerationStructure:atBufferIndex:",
	"setTileBuffer:offset:atIndex:", "setTileBufferOffset:atIndex:",
	"setTileBuffers:offsets:withRange:", "setTileBytes:length:atIndex:",
	"setTileIntersectionFunctionTable:atBufferIndex:",
	"setTileIntersectionFunctionTables:withBufferRange:", "setTileSamplerState:atIndex:",
	"setTileSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setTileSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setTileSamplerStates:withRange:", "setTileTexture:atIndex:", "setTileTextures:withRange:",
	"setTileVisibleFunctionTable:atBufferIndex:",
	"setTileVisibleFunctionTables:withBufferRange:", "setTriangleFillMode:",
	"setVertexAccelerationStructure:atBufferIndex:", "setVertexAmplificationCount:viewMappings:",
	"setVertexBuffer:offset:atIndex:", "setVertexBufferOffset:atIndex:",
	"setVertexBuffers:offsets:withRange:", "setVertexBytes:length:atIndex:",
	"setVertexIntersectionFunctionTable:atBufferIndex:",
	"setVertexIntersectionFunctionTables:withBufferRange:", "setVertexSamplerState:atIndex:",
	"setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setVertexSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setVertexSamplerStates:withRange:", "setVertexTexture:atIndex:",
	"setVertexTextures:withRange:", "setVertexVisibleFunctionTable:atBufferIndex:",
	"setVertexVisibleFunctionTables:withBufferRange:", "setViewport:", "setViewports:count:",
	"setVisibilityResultMode:offset:", "textureBarrier", "tileHeight", "tileWidth",
	"updateFence:afterStages:", "useHeap:", "useHeap:stages:", "useHeaps:count:",
	"useHeaps:count:stages:", "useResource:usage:", "useResource:usage:stages:",
	"useResources:count:usage:", "useResources:count:usage:stages:",
	"waitForFence:beforeStages:",
};
static const char *const spi_rendercommandencoderspi[] = {   /* MTLRenderCommandEncoderSPI: 34 required */
	"addSplitHandler:",
	"drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"isMemorylessRender", "setDepthClipModeSPI:",
	"setFragmentTexture:atTextureIndex:samplerState:atSamplerIndex:", "setLineWidth:",
	"setMeshBuffer:offset:atIndex:", "setMeshBufferOffset:atIndex:",
	"setMeshBuffers:offsets:withRange:", "setMeshBytes:length:atIndex:",
	"setMeshSamplerState:atIndex:", "setMeshSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setMeshSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setMeshSamplerStates:withRange:", "setMeshTexture:atIndex:", "setMeshTextures:withRange:",
	"setMeshVisibleFunctionTable:atBufferIndex:",
	"setMeshVisibleFunctionTables:withBufferRange:", "setObjectBuffer:offset:atIndex:",
	"setObjectBufferOffset:atIndex:", "setObjectBuffers:offsets:withRange:",
	"setObjectBytes:length:atIndex:", "setObjectSamplerState:atIndex:",
	"setObjectSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setObjectSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setObjectSamplerStates:withRange:", "setObjectTexture:atIndex:",
	"setObjectTextures:withRange:", "setObjectThreadgroupMemoryLength:atIndex:",
	"setObjectVisibleFunctionTable:atBufferIndex:",
	"setObjectVisibleFunctionTables:withBufferRange:", "setVertexAmplificationMode:value:",
};
static const char *const spi_renderpipelinestatespi[] = {   /* MTLRenderPipelineStateSPI: 32 required */
	"allocatedSize", "fragmentDebugInstrumentationData", "fragmentFunctionHandleWithFunction:",
	"getFragmentShaderTelemetryID", "getVertexShaderTelemetryID", "gpuHandle",
	"maxTotalThreadsPerMeshThreadgroup", "maxTotalThreadsPerObjectThreadgroup",
	"meshDebugInstrumentationData", "meshFunctionHandleWithFunction:",
	"meshThreadExecutionWidth", "newFragmentIntersectionFunctionTableWithDescriptor:",
	"newFragmentShaderDebugInfo",
	"newRenderPipelineStateWithAdditionalBinaryFunctions:fragmentAdditionalBinaryFunctions:error:",
	"newTileIntersectionFunctionTableWithDescriptor:",
	"newTileRenderPipelineStateWithAdditionalBinaryFunctions:error:",
	"newVertexIntersectionFunctionTableWithDescriptor:", "newVertexShaderDebugInfo",
	"newVisibleFunctionTableFromFragmentStageWithDescriptor:",
	"newVisibleFunctionTableFromMeshStageWithDescriptor:",
	"newVisibleFunctionTableFromObjectStageWithDescriptor:",
	"newVisibleFunctionTableFromTileStageWithDescriptor:",
	"newVisibleFunctionTableFromVertexStageWithDescriptor:", "objectDebugInstrumentationData",
	"objectFunctionHandleWithFunction:", "objectThreadExecutionWidth",
	"textureWriteRoundingMode", "tileDebugInstrumentationData",
	"tileFunctionHandleWithFunction:", "uniqueIdentifier", "vertexDebugInstrumentationData",
	"vertexFunctionHandleWithFunction:",
};

static const struct rw_protocol rw_protocols[] = {
	RW_PROTOCOL("MTLCommandBufferSPI", spi_commandbufferspi),
	RW_PROTOCOL("MTLCommandQueue", spi_commandqueue),
	RW_PROTOCOL("MTLCommandQueueSPI", spi_commandqueuespi),
	RW_PROTOCOL("MTLComputeCommandEncoder", spi_computecommandencoder),
	RW_PROTOCOL("MTLComputeCommandEncoderSPI", spi_computecommandencoderspi),
	RW_PROTOCOL("MTLComputePipelineStateSPI", spi_computepipelinestatespi),
	RW_PROTOCOL("MTLPipelineStateWithProgramAddressTable", spi_pipelinestatewithprogramaddresstable),
	RW_PROTOCOL("MTLRenderCommandEncoder", spi_rendercommandencoder),
	RW_PROTOCOL("MTLRenderCommandEncoderSPI", spi_rendercommandencoderspi),
	RW_PROTOCOL("MTLRenderPipelineStateSPI", spi_renderpipelinestatespi),
};

static const char *const chain_device[] = {
	"MTLDeviceSPI", "MTLDevice", NULL,
};
static const char *const chain_commandqueue[] = {
	"MTLCommandQueueSPI", "MTLCommandQueue", NULL,
};
static const char *const chain_commandbuffer[] = {
	"MTLCommandBufferSPI", "MTLCommandBuffer", NULL,
};
static const char *const chain_renderencoder[] = {
	"MTLRenderCommandEncoderSPI", "MTLCommandEncoderSPI", "MTLCommandEncoder",
	"MTLRenderCommandEncoder", NULL,
};
static const char *const chain_computeencoder[] = {
	"MTLComputeCommandEncoderSPI", "MTLCommandEncoderSPI", "MTLCommandEncoder",
	"MTLComputeCommandEncoder", NULL,
};
static const char *const chain_renderpipeline[] = {
	"MTLRenderPipelineStateSPI", "MTLRenderPipelineState",
	"MTLPipelineStateWithProgramAddressTable", NULL,
};
static const char *const chain_computepipeline[] = {
	"MTLComputePipelineStateSPI", "MTLComputePipelineState",
	"MTLPipelineStateWithProgramAddressTable", NULL,
};
static const char *const chain_texture[] = {
	"MTLTextureSPI", "MTLTexture", "MTLResource", "MTLResourceSPI", NULL,
};
static const char *const chain_buffer[] = {
	"MTLBuffer", "MTLResource", "MTLResourceSPI", NULL,
};
static const char *const chain_sampler[] = {
	"MTLSamplerStateSPI", "MTLSamplerState", NULL,
};
static const char *const chain_depthstencilstate[] = {
	"MTLDepthStencilStateSPI", "MTLDepthStencilState", NULL,
};

static const struct rw_class rw_classes[] = {
	{ "AnnibaleMetalDevice", chain_device },
	{ "AnnibaleMetalCommandQueue", chain_commandqueue },
	{ "AnnibaleMetalCommandBuffer", chain_commandbuffer },
	{ "AnnibaleMetalRenderEncoder", chain_renderencoder },
	{ "AnnibaleMetalComputeEncoder", chain_computeencoder },
	{ "AnnibaleMetalRenderPipeline", chain_renderpipeline },
	{ "AnnibaleMetalComputePipeline", chain_computepipeline },
	{ "AnnibaleMetalTexture", chain_texture },
	{ "AnnibaleMetalBuffer", chain_buffer },
	{ "AnnibaleMetalSampler", chain_sampler },
	{ "AnnibaleMetalDepthStencilState", chain_depthstencilstate },
};

/* The target of the first branch out of this body, which for an abstract method of Apple's
 * is the __objc_stubs entry for doesNotRecognizeSelector:. B and BL are decoded in place --
 * imm26, signed, in units of four bytes. (A11 has no pointer authentication, so an IMP is
 * an address and can simply be read.) */
static uintptr_t
first_external_branch(IMP imp)
{
	const uint32_t *words = (const uint32_t *)(uintptr_t)imp;

	if (imp == NULL) {
		return 0;
	}
	for (int i = 0; i < 10; i++) {
		uint32_t word = words[i];

		if ((word >> 26) == 0x05 || (word >> 26) == 0x25) {   /* B, BL */
			intptr_t offset = (intptr_t)((int32_t)(word << 6) >> 6) * 4;
			uintptr_t target = (uintptr_t)(words + i) + offset;

			if (target < (uintptr_t)words || target > (uintptr_t)(words + 10)) {
				return target;
			}
		}
		if (word == 0xd65f03c0) {   /* ret: the body ended and called nothing out */
			break;
		}
	}
	return 0;
}

/*
 * The stub every abstract method of Apple's calls, learned rather than assumed: two methods
 * that are known to be abstract in this Metal are read, and their first outward branch has
 * to be the same address. If it is not, the check is switched off and says so -- a wrong
 * stub address would call every method in the chain abstract, which is the kind of number
 * that looks like a finding.
 *
 * One stub is enough because every base class this driver inherits from (_MTLDevice,
 * _MTLCommandQueue, _MTLCommandBuffer, _MTLCommandEncoder, _MTLResource,
 * _MTLObjectWithLabel, _MTLSamplerState, _MTLDepthStencilState, and the two pipeline
 * states) lives in Metal.framework, and __objc_stubs is per image. A base class from
 * another image -- IOGPU, say, if this driver ever inherits one of its classes -- would
 * have its own stub and would be missed here, which is why the two methods read are both
 * Metal's and why this says so.
 */
static uintptr_t
abstract_stub(void)
{
	static uintptr_t stub;
	static int looked;

	if (!looked) {
		/* -[_MTLDevice initWithAcceleratorPort:] is the abstract method this whole
		 * subsystem started from (10.1); -[_MTLCommandEncoder setMeshTexture:atIndex:]
		 * is one of the 27 mesh-stage entries the driver now overrides. */
		const char *classes[2] = { "_MTLDevice", "_MTLCommandEncoder" };
		const char *selectors[2] = { "initWithAcceleratorPort:",
					     "setMeshTexture:atIndex:" };
		uintptr_t found[2] = { 0, 0 };

		looked = 1;
		for (int i = 0; i < 2; i++) {
			Class cls = objc_getClass(classes[i]);
			Method method = cls != Nil ?
			    class_getInstanceMethod(cls, sel_getUid(selectors[i])) : NULL;

			if (method != NULL) {
				found[i] = first_external_branch(
				    method_getImplementation(method));
			}
		}
		if (found[0] != 0 && found[0] == found[1]) {
			stub = found[0];
			printf("drvtest: sweep: Apple's doesNotRecognizeSelector: stub is at "
			       "%#lx, agreed by both of the methods known to be abstract\n",
			       (unsigned long)stub);
		} else {
			printf("drvtest: sweep: the two methods known to be abstract branch to "
			       "%#lx and %#lx, so the stub could not be identified and no "
			       "method will be CALLED abstract below (the unanswered count is "
			       "unaffected)\n", (unsigned long)found[0], (unsigned long)found[1]);
		}
	}
	return stub;
}

/* Is what answers this selector one of Apple's abstract entries -- an abort wearing a
 * method table entry? */
static int
answer_is_abstract(Class cls, SEL selector)
{
	Method method = class_getInstanceMethod(cls, selector);
	uintptr_t stub = abstract_stub();

	return stub != 0 && method != NULL &&
	       first_external_branch(method_getImplementation(method)) == stub;
}

/* The required selectors of one protocol: the runtime's copy when this process has the
 * protocol, and otherwise the list read out of the cache. Returns 0 only when neither has
 * it, which is itself worth printing. */
static unsigned
required_selectors(const char *name, SEL *into, unsigned room, const char **source)
{
	Protocol *proto = objc_getProtocol(name);
	unsigned n = 0, taken = 0;

	*source = "runtime";
	if (proto != NULL) {
		struct objc_method_description *req =
		    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

		for (unsigned i = 0; i < n && taken < room; i++) {
			into[taken++] = req[i].name;
		}
		free(req);
		/* and if the cache said something different about the same protocol, say so:
		 * one of the two is then out of date, and it matters which */
		for (unsigned i = 0; i < sizeof(rw_protocols) / sizeof(rw_protocols[0]); i++) {
			if (strcmp(rw_protocols[i].name, name) == 0 &&
			    rw_protocols[i].count != n) {
				printf("drvtest: sweep: %s: the runtime requires %u methods and "
				       "the cache said %u; the runtime is used\n", name, n,
				       rw_protocols[i].count);
			}
		}
		return taken;
	}
	for (unsigned i = 0; i < sizeof(rw_protocols) / sizeof(rw_protocols[0]); i++) {
		if (strcmp(rw_protocols[i].name, name) != 0) {
			continue;
		}
		*source = "the cache";
		for (unsigned j = 0; j < rw_protocols[i].count && taken < room; j++) {
			into[taken++] = sel_getUid(rw_protocols[i].selectors[j]);
		}
		return taken;
	}
	*source = "nowhere";
	return 0;
}

/* One class, every protocol of it, counted. No instance is needed and none is made: what
 * `[object respondsToSelector:]` answers is what the class chain answers, so asking the
 * class cannot be defeated by a nil object or by a GPU that is not there. */
static void
sweep_class(const struct rw_class *entry)
{
	Class cls = objc_getClass(entry->name);
	SEL selectors[768];
	unsigned total = 0, unanswered = 0, abstract = 0, shown = 0;
	char which[256] = { 0 };

	if (cls == Nil) {
		printf("drvtest: sweep: %s IS NOT IN THIS PROCESS, so nothing of it was "
		       "counted\n", entry->name);
		return;
	}
	for (unsigned p = 0; entry->protocols[p] != NULL; p++) {
		const char *source = "";
		SEL one[768];
		unsigned n = required_selectors(entry->protocols[p], one,
						sizeof(one) / sizeof(one[0]), &source);

		if (n == 0) {
			printf("drvtest: sweep: %s: no list of %s exists in this process or in "
			       "the compiled-in tables, so its methods were NOT counted\n",
			       entry->name, entry->protocols[p]);
			continue;
		}
		snprintf(which + strlen(which), sizeof(which) - strlen(which), "%s%s(%u,%s)",
			 p == 0 ? "" : "+", entry->protocols[p], n, source);
		for (unsigned i = 0; i < n; i++) {
			int seen = 0;

			/* a selector required by two protocols of the same class is one
			 * method, and counting it twice would inflate every number here;
			 * SELs are interned, so comparing the pointers is comparing the names */
			for (unsigned j = 0; j < total; j++) {
				if (selectors[j] == one[i]) {
					seen = 1;
					break;
				}
			}
			if (seen || total >= sizeof(selectors) / sizeof(selectors[0])) {
				continue;
			}
			selectors[total++] = one[i];
			if (!class_respondsToSelector(cls, one[i])) {
				unanswered++;
				if (shown < 8) {
					printf("drvtest: sweep:   %s still owes %s\n",
					       entry->name, sel_getName(one[i]));
					shown++;
				}
			} else if (answer_is_abstract(cls, one[i])) {
				abstract++;
				if (shown < 8) {
					printf("drvtest: sweep:   %s answers %s with one of "
					       "Apple's abstract stubs, which aborts when it is "
					       "called\n", entry->name, sel_getName(one[i]));
					shown++;
				}
			}
		}
	}
	printf("drvtest: sweep: of %u methods %s requires, %u are unanswered and %u are "
	       "answered by an abstract stub of Apple's [%s]\n", total, entry->name,
	       unanswered, abstract, which);
}

/*
 * Every class the driver has, on every run. The numbers belong in the log of each boot
 * rather than in a measurement someone took once: the protocols are the system's, and the
 * system is what changes under us.
 */
static void
sweep_every_class(void)
{
	unsigned classes = sizeof(rw_classes) / sizeof(rw_classes[0]);

	printf("drvtest: sweep: %u classes, counted the way 10.17 counted the device: "
	       "unanswered means absent from the chain, and an entry whose body refuses is "
	       "answered\n", classes);
	for (unsigned i = 0; i < classes; i++) {
		sweep_class(&rw_classes[i]);
	}
}
