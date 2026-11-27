/* ENG-09 dual-end Metal material regression suite (6 categories).
 * Runs on Apple's native Metal device by default, or selects the port's driver via --driver. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { W = 16, H = 16 };
static const MTLScissorRect InnerScissor = { 4, 4, 8, 8 };

static void
deadline(int sig)
{
	(void)sig;
	static const char msg[] = "material_regress: 30-second deadline exceeded\n";
	(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
	_exit(124);
}

static uint16_t
f32_to_f16(float f)
{
	uint32_t x;
	memcpy(&x, &f, sizeof(x));
	uint32_t sign = (x >> 16) & 0x8000u;
	int exp = (int)((x >> 23) & 0xffu) - 127 + 15;
	uint32_t mant = x & 0x7fffffu;
	if (exp <= 0) return (uint16_t)sign;
	if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
	return (uint16_t)(sign | ((uint32_t)exp << 10) | ((mant + 0x1000u) >> 13));
}

static unsigned
bpp_for_format(MTLPixelFormat fmt)
{
	switch (fmt) {
	case MTLPixelFormatA8Unorm:
	case MTLPixelFormatR8Unorm:
		return 1;
	case MTLPixelFormatRGBA16Float:
		return 8;
	default:
		return 4;
	}
}

static const char *
format_tag(MTLPixelFormat fmt)
{
	switch (fmt) {
	case MTLPixelFormatBGRA8Unorm: return "bgra8";
	case MTLPixelFormatBGRA8Unorm_sRGB: return "bgra8_srgb";
	case MTLPixelFormatRGBA8Unorm_sRGB: return "rgba8_srgb";
	case MTLPixelFormatRGBA16Float: return "rgba16f";
	case MTLPixelFormatBGR10A2Unorm: return "bgr10a2";
	default: return "unknown";
	}
}

static id<MTLTexture>
make_tex(id<MTLDevice> dev, MTLPixelFormat fmt, MTLTextureUsage usage)
{
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
	                                                                             width:W
	                                                                            height:H
	                                                                         mipmapped:NO];
	d.storageMode = MTLStorageModeShared;
	d.usage = usage;
	return [[dev newTextureWithDescriptor:d] autorelease];
}

static void
fill_bgra_pattern(id<MTLTexture> tex, int variant)
{
	uint8_t buf[W * H * 4];
	for (unsigned y = 0; y < H; y++) {
		for (unsigned x = 0; x < W; x++) {
			uint8_t *p = buf + (y * W + x) * 4;
			BOOL checker = ((x >> 1) ^ (y >> 1)) & 1;
			if (variant == 0) {
				/* Premultiplied RGBA gradient + checkerboard + high-contrast edge. */
				uint8_t a = (x == 0 || y == 0) ? 0 : (x == W - 1 ? 255 : (uint8_t)(48 + y * 13));
				uint8_t r = checker ? (uint8_t)(220 * a / 255) : (uint8_t)((x * 16) * a / 255);
				uint8_t g = (uint8_t)((y * 16) * a / 255);
				uint8_t b = checker ? (uint8_t)(40 * a / 255) : (uint8_t)(200 * a / 255);
				p[0] = b; p[1] = g; p[2] = r; p[3] = a;
			} else {
				/* Backdrop with varying alpha for dst-alpha and blend tests. */
				p[0] = (uint8_t)(30 + x * 12);
				p[1] = (uint8_t)(200 - y * 10);
				p[2] = checker ? 180 : 60;
				p[3] = (uint8_t)(64 + x * 12);
			}
		}
	}
	[tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:buf bytesPerRow:W * 4];
}

static void
fill_fp16_pattern(id<MTLTexture> tex)
{
	uint16_t buf[W * H * 4];
	for (unsigned y = 0; y < H; y++) {
		for (unsigned x = 0; x < W; x++) {
			uint16_t *p = buf + (y * W + x) * 4;
			BOOL hi = ((x + y) & 3) == 0;
			p[0] = f32_to_f16(hi ? 0.95f : (float)x / 15.0f);
			p[1] = f32_to_f16((float)y / 15.0f);
			p[2] = f32_to_f16(0.25f + 0.5f * (float)((x ^ y) & 7) / 7.0f);
			p[3] = f32_to_f16(1.0f);
		}
	}
	[tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:buf bytesPerRow:W * 8];
}

static void
fill_sdf_pattern(id<MTLTexture> tex)
{
	uint8_t buf[W * H];
	for (unsigned y = 0; y < H; y++) {
		for (unsigned x = 0; x < W; x++) {
			float dx = ((float)x + 0.5f) / (float)W - 0.5f;
			float dy = ((float)y + 0.5f) / (float)H - 0.5f;
			float dist = 0.5f + (0.32f - sqrtf(dx * dx + dy * dy)) * 2.0f;
			if (dist < 0.0f) dist = 0.0f;
			if (dist > 1.0f) dist = 1.0f;
			buf[y * W + x] = (uint8_t)lroundf(dist * 255.0f);
		}
	}
	[tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:buf bytesPerRow:W];
}

static id<MTLRenderPipelineState>
make_pipe(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *frag,
          MTLPixelFormat fmt0, MTLPixelFormat fmt1,
          void (^configure)(MTLRenderPipelineDescriptor *))
{
	id<MTLFunction> vf = [[lib newFunctionWithName:@"mr_vertex"] autorelease];
	id<MTLFunction> ff = [[lib newFunctionWithName:frag] autorelease];
	if (!vf || !ff) {
		fprintf(stderr, "material_regress: missing shader %s\n", frag.UTF8String);
		return nil;
	}
	MTLRenderPipelineDescriptor *d = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
	d.vertexFunction = vf;
	d.fragmentFunction = ff;
	d.rasterSampleCount = 1;
	d.colorAttachments[0].pixelFormat = fmt0;
	d.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
	if (fmt1 != MTLPixelFormatInvalid) {
		d.colorAttachments[1].pixelFormat = fmt1;
		d.colorAttachments[1].writeMask = MTLColorWriteMaskAll;
	}
	if (configure) configure(d);
	NSError *err = nil;
	id<MTLRenderPipelineState> p = [[dev newRenderPipelineStateWithDescriptor:d error:&err] autorelease];
	if (!p || err) {
		fprintf(stderr, "material_regress: pipeline %s failed: %s\n", frag.UTF8String,
		        err.description.UTF8String ?: "nil");
		return nil;
	}
	return p;
}

static int
commit_and_wait(id<MTLCommandBuffer> cb, const char *label)
{
	[cb commit];
	[cb waitUntilCompleted];
	if (cb.status != MTLCommandBufferStatusCompleted || cb.error) {
		fprintf(stderr, "material_regress: %s failed status=%lu err=%s\n", label,
		        (unsigned long)cb.status, cb.error.description.UTF8String ?: "none");
		return -1;
	}
	return 0;
}

static int
dump_tex(id<MTLTexture> tex, NSString *outDir, NSString *name,
         const MTLScissorRect *rect, NSMutableArray *manifest)
{
	MTLPixelFormat fmt = tex.pixelFormat;
	unsigned bpp = bpp_for_format(fmt);
	size_t bytes = (size_t)W * H * bpp;
	uint8_t *buf = (uint8_t *)calloc(1, bytes);
	if (!buf) return -1;
	[tex getBytes:buf bytesPerRow:W * bpp fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
	NSString *file = [NSString stringWithFormat:@"%@.bin", name];
	BOOL ok = [[NSData dataWithBytes:buf length:bytes]
	    writeToFile:[outDir stringByAppendingPathComponent:file] atomically:YES];
	free(buf);
	if (!ok) {
		fprintf(stderr, "material_regress: cannot write %s\n", file.UTF8String);
		return -1;
	}
	MTLScissorRect r = rect ? *rect : (MTLScissorRect){ 0, 0, W, H };
	[manifest addObject:@{
		@"name": name,
		@"file": file,
		@"format": @(format_tag(fmt)),
		@"pixel_format": @((unsigned)fmt),
		@"width": @(W),
		@"height": @(H),
		@"bpp": @(bpp),
		@"check_rect": @[ @(r.x), @(r.y), @(r.width), @(r.height) ],
	}];
	printf("DUMP %s fmt=%s(%u) rect=%lu,%lu,%lu,%lu\n", name.UTF8String, format_tag(fmt),
	       (unsigned)fmt, (unsigned long)r.x, (unsigned long)r.y,
	       (unsigned long)r.width, (unsigned long)r.height);
	return 0;
}

static int
run_suite(id<MTLDevice> dev, id<MTLLibrary> lib, id<MTLCommandQueue> q,
          NSString *outDir, NSMutableArray *manifest)
{
	MTLSamplerDescriptor *sd = [[[MTLSamplerDescriptor alloc] init] autorelease];
	sd.minFilter = MTLSamplerMinMagFilterLinear;
	sd.magFilter = MTLSamplerMinMagFilterLinear;
	sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	id<MTLSamplerState> samp = [[dev newSamplerStateWithDescriptor:sd] autorelease];
	if (!samp) return 2;

	id<MTLTexture> srcBGRA = make_tex(dev, MTLPixelFormatBGRA8Unorm, MTLTextureUsageShaderRead);
	id<MTLTexture> srcFP16 = make_tex(dev, MTLPixelFormatRGBA16Float, MTLTextureUsageShaderRead);
	id<MTLTexture> sdfA8 = make_tex(dev, MTLPixelFormatA8Unorm, MTLTextureUsageShaderRead);
	id<MTLTexture> sdfR8 = make_tex(dev, MTLPixelFormatR8Unorm, MTLTextureUsageShaderRead);
	if (!srcBGRA || !srcFP16 || !sdfA8 || !sdfR8) return 2;
	fill_bgra_pattern(srcBGRA, 0);
	fill_fp16_pattern(srcFP16);
	fill_sdf_pattern(sdfA8);
	fill_sdf_pattern(sdfR8);

	MTLTextureUsage rtUsage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

	/* Category 1: Liquid Glass multi-attachment framebuffer fetch. */
	{
		id<MTLTexture> c0 = make_tex(dev, MTLPixelFormatBGRA8Unorm, rtUsage);
		id<MTLTexture> c1 = make_tex(dev, MTLPixelFormatRGBA16Float, rtUsage);
		fill_bgra_pattern(c0, 1);
		fill_fp16_pattern(c1);
		id<MTLRenderPipelineState> p = make_pipe(dev, lib, @"mr_glass_fbfetch",
		    MTLPixelFormatBGRA8Unorm, MTLPixelFormatRGBA16Float,
		    ^(MTLRenderPipelineDescriptor *d) {
			    d.colorAttachments[0].blendingEnabled = YES;
			    d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
			    d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			    d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
			    d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		    });
		if (!p) return 2;
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = c0;
		rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		rp.colorAttachments[1].texture = c1;
		rp.colorAttachments[1].loadAction = MTLLoadActionLoad;
		rp.colorAttachments[1].storeAction = MTLStoreActionStore;
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		[enc setRenderPipelineState:p];
		[enc setFragmentTexture:srcBGRA atIndex:0];
		[enc setFragmentSamplerState:samp atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		if (commit_and_wait(cb, "glass_fbfetch") != 0) return 2;
		if (dump_tex(c0, outDir, @"glass_fbfetch_c0", NULL, manifest) != 0) return 2;
		if (dump_tex(c1, outDir, @"glass_fbfetch_c1", NULL, manifest) != 0) return 2;
	}

	/* Category 2: Half-precision Gaussian blur (RGBA16Float). */
	{
		id<MTLTexture> dst = make_tex(dev, MTLPixelFormatRGBA16Float, rtUsage);
		id<MTLRenderPipelineState> p = make_pipe(dev, lib, @"mr_blur_fp16",
		    MTLPixelFormatRGBA16Float, MTLPixelFormatInvalid, nil);
		if (!p) return 2;
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = dst;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		[enc setRenderPipelineState:p];
		[enc setFragmentTexture:srcFP16 atIndex:0];
		[enc setFragmentSamplerState:samp atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		if (commit_and_wait(cb, "blur_fp16") != 0) return 2;
		if (dump_tex(dst, outDir, @"blur_fp16", NULL, manifest) != 0) return 2;
	}

	/* Category 3: Compositing filter & blend state matrix (5 subcases). */
	struct blend_case {
		const char *name;
		const char *frag;
		MTLBlendFactor srgb, drgb, sa, da;
		MTLBlendOperation rgbo, ao;
		MTLColorWriteMask mask;
		BOOL setBlendColor;
	};
	const struct blend_case blends[] = {
		{ "blend_src_alpha", "mr_blend_src",
		  MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
		  MTLBlendFactorOne, MTLBlendFactorOneMinusSourceAlpha,
		  MTLBlendOperationAdd, MTLBlendOperationAdd, MTLColorWriteMaskAll, NO },
		{ "blend_dst_alpha", "mr_blend_src",
		  MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
		  MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
		  MTLBlendOperationAdd, MTLBlendOperationAdd, MTLColorWriteMaskAll, NO },
		{ "blend_const_color", "mr_blend_src",
		  MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor,
		  MTLBlendFactorBlendAlpha, MTLBlendFactorOneMinusBlendAlpha,
		  MTLBlendOperationAdd, MTLBlendOperationAdd, MTLColorWriteMaskAll, YES },
		{ "blend_minmax_mask", "mr_blend_src",
		  MTLBlendFactorOne, MTLBlendFactorOne,
		  MTLBlendFactorOne, MTLBlendFactorOne,
		  MTLBlendOperationMin, MTLBlendOperationMax,
		  MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue, NO },
		{ "blend_dual_src", "mr_blend_dual",
		  MTLBlendFactorSource1Color, MTLBlendFactorOneMinusSource1Alpha,
		  MTLBlendFactorOne, MTLBlendFactorOneMinusSource1Alpha,
		  MTLBlendOperationAdd, MTLBlendOperationAdd, MTLColorWriteMaskAll, NO },
	};
	for (unsigned i = 0; i < sizeof(blends) / sizeof(blends[0]); i++) {
		const struct blend_case bc = blends[i];
		id<MTLTexture> dst = make_tex(dev, MTLPixelFormatBGRA8Unorm, rtUsage);
		fill_bgra_pattern(dst, 1);
		id<MTLRenderPipelineState> p = make_pipe(dev, lib, @(bc.frag),
		    MTLPixelFormatBGRA8Unorm, MTLPixelFormatInvalid,
		    ^(MTLRenderPipelineDescriptor *d) {
			    d.colorAttachments[0].blendingEnabled = YES;
			    d.colorAttachments[0].sourceRGBBlendFactor = bc.srgb;
			    d.colorAttachments[0].destinationRGBBlendFactor = bc.drgb;
			    d.colorAttachments[0].sourceAlphaBlendFactor = bc.sa;
			    d.colorAttachments[0].destinationAlphaBlendFactor = bc.da;
			    d.colorAttachments[0].rgbBlendOperation = bc.rgbo;
			    d.colorAttachments[0].alphaBlendOperation = bc.ao;
			    d.colorAttachments[0].writeMask = bc.mask;
		    });
		if (!p) return 2;
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = dst;
		rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		[enc setRenderPipelineState:p];
		if (bc.setBlendColor) {
			[enc setBlendColorRed:0.25f green:0.5f blue:0.75f alpha:0.5f];
		}
		[enc setFragmentTexture:srcBGRA atIndex:0];
		[enc setFragmentSamplerState:samp atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		if (commit_and_wait(cb, bc.name) != 0) return 2;
		if (dump_tex(dst, outDir, @(bc.name), NULL, manifest) != 0) return 2;
	}

	/* Category 4: SDF text / single-channel mask AA (A8 -> BGRA8, R8 -> BGRA8_sRGB). */
	struct {
		const char *name;
		const char *frag;
		id<MTLTexture> glyph;
		MTLPixelFormat dstFmt;
	} sdfs[] = {
		{ "sdf_a8_bgra", "mr_sdf_a8", sdfA8, MTLPixelFormatBGRA8Unorm },
		{ "sdf_r8_srgb", "mr_sdf_r8", sdfR8, MTLPixelFormatBGRA8Unorm_sRGB },
	};
	for (unsigned i = 0; i < 2; i++) {
		id<MTLTexture> dst = make_tex(dev, sdfs[i].dstFmt, rtUsage);
		id<MTLRenderPipelineState> p = make_pipe(dev, lib, @(sdfs[i].frag),
		    sdfs[i].dstFmt, MTLPixelFormatInvalid,
		    ^(MTLRenderPipelineDescriptor *d) {
			    d.colorAttachments[0].blendingEnabled = YES;
			    d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
			    d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			    d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
			    d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		    });
		if (!p) return 2;
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = dst;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].clearColor = MTLClearColorMake(0.12, 0.14, 0.18, 1.0);
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		[enc setRenderPipelineState:p];
		[enc setFragmentTexture:sdfs[i].glyph atIndex:0];
		[enc setFragmentSamplerState:samp atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		if (commit_and_wait(cb, sdfs[i].name) != 0) return 2;
		if (dump_tex(dst, outDir, @(sdfs[i].name), NULL, manifest) != 0) return 2;
	}

	/* Category 5: High-precision & sRGB format contracts (81, 71, 115, 94). */
	struct {
		const char *name;
		MTLPixelFormat fmt;
	} fmts[] = {
		{ "fmt_bgra8_srgb", MTLPixelFormatBGRA8Unorm_sRGB },
		{ "fmt_rgba8_srgb", MTLPixelFormatRGBA8Unorm_sRGB },
		{ "fmt_rgba16f", MTLPixelFormatRGBA16Float },
		{ "fmt_bgr10a2", MTLPixelFormatBGR10A2Unorm },
	};
	for (unsigned i = 0; i < 4; i++) {
		id<MTLTexture> dst = make_tex(dev, fmts[i].fmt, rtUsage);
		id<MTLRenderPipelineState> p = make_pipe(dev, lib, @"mr_format_grad",
		    fmts[i].fmt, MTLPixelFormatInvalid,
		    ^(MTLRenderPipelineDescriptor *d) {
			    d.colorAttachments[0].blendingEnabled = YES;
			    d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
			    d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			    d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
			    d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		    });
		if (!p) return 2;
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = dst;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2, 0.3, 1.0);
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		[enc setRenderPipelineState:p];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		if (commit_and_wait(cb, fmts[i].name) != 0) return 2;
		if (dump_tex(dst, outDir, @(fmts[i].name), NULL, manifest) != 0) return 2;
	}

	/* Category 6: Multi-pass DontCare and writeMask=0 readback (0009/0010 A/B). */
	for (int variant = 0; variant < 2; variant++) {
		BOOL isDontCare = (variant == 1);
		id<MTLTexture> c0 = make_tex(dev, MTLPixelFormatBGRA8Unorm, rtUsage);
		id<MTLTexture> c1 = make_tex(dev, MTLPixelFormatRGBA16Float, rtUsage);
		id<MTLTexture> rb = make_tex(dev, MTLPixelFormatBGRA8Unorm, rtUsage);
		id<MTLRenderPipelineState> pSeed = make_pipe(dev, lib, @"mr_mp_seed",
		    MTLPixelFormatBGRA8Unorm, MTLPixelFormatRGBA16Float, nil);
		id<MTLRenderPipelineState> pTouch = make_pipe(dev, lib, @"mr_mp_touch",
		    MTLPixelFormatBGRA8Unorm, MTLPixelFormatRGBA16Float,
		    ^(MTLRenderPipelineDescriptor *d) {
			    if (!isDontCare) {
				    d.colorAttachments[1].writeMask = MTLColorWriteMaskNone;
			    }
		    });
		id<MTLRenderPipelineState> pRead = make_pipe(dev, lib, @"mr_mp_readback",
		    MTLPixelFormatBGRA8Unorm, MTLPixelFormatInvalid, nil);
		if (!pSeed || !pTouch || !pRead) return 2;

		id<MTLCommandBuffer> cb = [q commandBuffer];
		/* Pass 1: seed c0 and c1. */
		MTLRenderPassDescriptor *rp1 = [MTLRenderPassDescriptor renderPassDescriptor];
		rp1.colorAttachments[0].texture = c0;
		rp1.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp1.colorAttachments[0].storeAction = MTLStoreActionStore;
		rp1.colorAttachments[1].texture = c1;
		rp1.colorAttachments[1].loadAction = MTLLoadActionClear;
		rp1.colorAttachments[1].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> e1 = [cb renderCommandEncoderWithDescriptor:rp1];
		[e1 setRenderPipelineState:pSeed];
		[e1 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[e1 endEncoding];

		/* Pass 2: either Load + writeMask=0 on c1, or DontCare on c1 with scissor. */
		MTLRenderPassDescriptor *rp2 = [MTLRenderPassDescriptor renderPassDescriptor];
		rp2.colorAttachments[0].texture = c0;
		rp2.colorAttachments[0].loadAction = MTLLoadActionLoad;
		rp2.colorAttachments[0].storeAction = MTLStoreActionStore;
		rp2.colorAttachments[1].texture = c1;
		rp2.colorAttachments[1].loadAction = isDontCare ? MTLLoadActionDontCare : MTLLoadActionLoad;
		rp2.colorAttachments[1].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> e2 = [cb renderCommandEncoderWithDescriptor:rp2];
		[e2 setRenderPipelineState:pTouch];
		[e2 setScissorRect:InnerScissor];
		[e2 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[e2 endEncoding];

		/* Pass 3: read back c1 into rb. */
		MTLRenderPassDescriptor *rp3 = [MTLRenderPassDescriptor renderPassDescriptor];
		rp3.colorAttachments[0].texture = rb;
		rp3.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp3.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> e3 = [cb renderCommandEncoderWithDescriptor:rp3];
		[e3 setRenderPipelineState:pRead];
		[e3 setFragmentTexture:c1 atIndex:0];
		[e3 setFragmentSamplerState:samp atIndex:0];
		[e3 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[e3 endEncoding];

		if (commit_and_wait(cb, isDontCare ? "mp_dontcare" : "mp_mask0") != 0) return 2;
		if (dump_tex(rb, outDir,
		             isDontCare ? @"mp_dontcare_readback" : @"mp_mask0_readback",
		             isDontCare ? &InnerScissor : NULL, manifest) != 0) {
			return 2;
		}
	}

	/* Category 7 (G3): ir3 fsat(bitcast) (32-bit + 16-bit) & relative constant MOV (alias.rt). */
	{
		struct {
			uint32_t xor_bits[4], xor_mask[4];
			uint32_t add_base[4], add_delta[4];
			uint16_t half_bits[4], half_mask[4];
		} fsat_in;
		const float vals[4] = { -0.5f, 0.5f, 1.5f, 0.25f };
		const uint32_t xmask[4] = { 0x13579bdfu, 0x2468ace0u, 0x5a5a5a5au, 0xa5a5a5a5u };
		const uint16_t hmask[4] = { 0x1234u, 0x2345u, 0x3456u, 0x4567u };
		for (unsigned i = 0; i < 4; i++) {
			uint32_t fb = 0;
			memcpy(&fb, &vals[i], sizeof(fb));
			fsat_in.xor_mask[i] = xmask[i];
			fsat_in.xor_bits[i] = fb ^ xmask[i];
			fsat_in.add_delta[i] = 0x10000u * (i + 1u);
			fsat_in.add_base[i] = fb - fsat_in.add_delta[i];
			uint16_t hb = f32_to_f16(vals[i]);
			fsat_in.half_mask[i] = hmask[i];
			fsat_in.half_bits[i] = (uint16_t)(hb ^ hmask[i]);
		}
		id<MTLBuffer> ubuf = [[dev newBufferWithBytes:&fsat_in length:sizeof(fsat_in)
		                                      options:MTLResourceStorageModeShared] autorelease];
		id<MTLTexture> dst = make_tex(dev, MTLPixelFormatRGBA16Float, rtUsage);
		id<MTLRenderPipelineState> p = make_pipe(dev, lib, @"mr_g3_fsat",
		    MTLPixelFormatRGBA16Float, MTLPixelFormatInvalid, nil);
		if (!ubuf || !dst || !p) return 2;
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = dst;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		[enc setRenderPipelineState:p];
		[enc setFragmentBuffer:ubuf offset:0 atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		if (commit_and_wait(cb, "g3_fsat_bitcast") != 0) return 2;
		if (dump_tex(dst, outDir, @"g3_fsat_bitcast", NULL, manifest) != 0) return 2;
	}
	{
		float tbl[8][4];
		for (unsigned i = 0; i < 8; i++) {
			tbl[i][0] = 0.1f * (float)(i + 1u);
			tbl[i][1] = 0.9f - 0.1f * (float)i;
			tbl[i][2] = 0.25f + 0.08f * (float)i;
			tbl[i][3] = 1.0f;
		}
		id<MTLBuffer> cbuf = [[dev newBufferWithBytes:tbl length:sizeof(tbl)
		                                      options:MTLResourceStorageModeShared] autorelease];
		id<MTLTexture> dst = make_tex(dev, MTLPixelFormatRGBA16Float, rtUsage);
		id<MTLRenderPipelineState> p = make_pipe(dev, lib, @"mr_g3_alias_rel",
		    MTLPixelFormatRGBA16Float, MTLPixelFormatInvalid, nil);
		if (!cbuf || !dst || !p) return 2;
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = dst;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		[enc setRenderPipelineState:p];
		[enc setFragmentBuffer:cbuf offset:0 atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		if (commit_and_wait(cb, "g3_alias_rel_const") != 0) return 2;
		if (dump_tex(dst, outDir, @"g3_alias_rel_const", NULL, manifest) != 0) return 2;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	const char *metallib = NULL, *driver = NULL, *output_path = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--metallib") && i + 1 < argc) metallib = argv[++i];
		else if (!strcmp(argv[i], "--driver") && i + 1 < argc) driver = argv[++i];
		else if (!strcmp(argv[i], "--output") && i + 1 < argc) output_path = argv[++i];
		else {
			fprintf(stderr, "usage: %s --metallib FILE --output DIR [--driver DYLIB]\n", argv[0]);
			return 2;
		}
	}
	if (!metallib || !output_path) {
		fprintf(stderr, "usage: %s --metallib FILE --output DIR [--driver DYLIB]\n", argv[0]);
		return 2;
	}
	signal(SIGALRM, deadline);
	alarm(30);
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	@autoreleasepool {
		@try {
			id<MTLDevice> dev = nil;
			if (driver) {
				unsetenv("RWA_SOFTWARE");
				if (!dlopen(driver, RTLD_NOW | RTLD_LOCAL)) {
					fprintf(stderr, "material_regress: dlopen: %s\n", dlerror());
					return 2;
				}
				Class cls = objc_getClass("AnnibaleMetalDevice");
				SEL init = sel_registerName("initWithAcceleratorPort:");
				if (!cls || ![cls instancesRespondToSelector:init]) return 2;
				dev = [((id (*)(id, SEL, uint32_t))objc_msgSend)([cls alloc], init, 0) autorelease];
			} else {
				dev = [MTLCreateSystemDefaultDevice() autorelease];
			}
			if (!dev) return 2;
			printf("material_regress: device=%s class=%s\n",
			       dev.name.UTF8String, object_getClassName(dev));

			NSString *outDir = [NSString stringWithUTF8String:output_path];
			if (![[NSFileManager defaultManager] createDirectoryAtPath:outDir
			                               withIntermediateDirectories:YES
			                                                attributes:nil
			                                                     error:NULL]) {
				return 2;
			}
			NSData *data = [NSData dataWithContentsOfFile:@(metallib)];
			if (!data.length) return 2;
			dispatch_data_t wrapped = dispatch_data_create(data.bytes, data.length, NULL,
			    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
			NSError *err = nil;
			id<MTLLibrary> lib = [[dev newLibraryWithData:wrapped error:&err] autorelease];
			dispatch_release(wrapped);
			if (!lib || err) {
				fprintf(stderr, "material_regress: metallib: %s\n",
				        err.description.UTF8String ?: "nil");
				return 2;
			}
			id<MTLCommandQueue> q = [[dev newCommandQueue] autorelease];
			if (!q) return 2;

			NSMutableArray *manifest = [NSMutableArray array];
			int rc = run_suite(dev, lib, q, outDir, manifest);
			if (rc != 0) return rc;

			NSDictionary *doc = @{
				@"device": dev.name ?: @"unknown",
				@"class": @(object_getClassName(dev)),
				@"cases": manifest,
			};
			NSData *json = [NSJSONSerialization dataWithJSONObject:doc
			                                               options:NSJSONWritingPrettyPrinted
			                                                 error:&err];
			if (!json || ![json writeToFile:[outDir stringByAppendingPathComponent:@"cases.json"]
			                     atomically:YES]) {
				return 2;
			}
			printf("material_regress: cases=%lu ok\n", (unsigned long)manifest.count);
			return 0;
		} @catch (NSException *ex) {
			fprintf(stderr, "material_regress: exception %s: %s\n",
			        ex.name.UTF8String, ex.reason.UTF8String);
			return 2;
		}
	}
}
