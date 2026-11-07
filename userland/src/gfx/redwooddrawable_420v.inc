/*
 * redwooddrawable_420v.inc: Dual-plane NV12 ('420v' / format 500) IOSurface texture
 * binding and BT.601 YUV->RGB compute conversion pipeline for video playback.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

static id (*orig_dev_new_texture_surf)(id self, SEL _cmd, id desc, void *surf, NSUInteger plane);
static id s_ycbcr_pso = nil;
static id s_ycbcr_sampler = nil;
static id s_ycbcr_queue = nil;

static void *(*iosurf_get_base_address_of_plane)(void *, size_t);
static kern_return_t (*iosurf_lock)(void *, uint32_t, uint32_t *);
static kern_return_t (*iosurf_unlock)(void *, uint32_t, uint32_t *);

static void
rwd_lookup_iosurface_lock_symbols(void)
{
	static int looked_lock;
	if (!looked_lock) {
		*(void **)&iosurf_get_base_address_of_plane = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddressOfPlane");
		*(void **)&iosurf_lock = dlsym(RTLD_DEFAULT, "IOSurfaceLock");
		*(void **)&iosurf_unlock = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");
		looked_lock = 1;
	}
}

static void
rwd_init_compute_pipeline(id device)
{
	if (s_ycbcr_pso != nil && s_ycbcr_sampler != nil && s_ycbcr_queue != nil) {
		return;
	}
	@autoreleasepool {
		NSError *err = nil;
		Class strCls = objc_getClass("NSString");
		id src = strCls ? ((id (*)(Class, SEL, const char *))objc_msgSend)(strCls, sel_registerName("stringWithUTF8String:"), rwd_ycbcr_conversion_shader_source()) : nil;
		Class optCls = objc_getClass("MTLCompileOptions");
		id opts = optCls ? ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(optCls, sel_registerName("alloc")), sel_registerName("init")) : nil;

		id lib = [device newLibraryWithSource:src options:opts error:&err];
		if (opts != nil) {
			((void (*)(id, SEL))objc_msgSend)(opts, sel_registerName("release"));
		}

		if (lib != nil) {
			SEL newFnSel = sel_registerName("newFunctionWithName:");
			id fn = strCls ? ((id (*)(id, SEL, id))objc_msgSend)(lib, newFnSel, ((id (*)(Class, SEL, const char *))objc_msgSend)(strCls, sel_registerName("stringWithUTF8String:"), "ycbcr_420v_to_rgb")) : nil;
			if (fn != nil) {
				s_ycbcr_pso = [device newComputePipelineStateWithFunction:fn error:&err];
				((void (*)(id, SEL))objc_msgSend)(fn, sel_registerName("release"));
			}
			((void (*)(id, SEL))objc_msgSend)(lib, sel_registerName("release"));
		}

		if (s_ycbcr_pso == nil) {
			fprintf(stderr, "redwooddrawable: YCbCr compute pipeline error: %s\n",
				err ? [[err localizedDescription] UTF8String] : "failed to compile");
		}

		Class sdescCls = objc_getClass("MTLSamplerDescriptor");
		if (sdescCls != Nil) {
			id sdesc = ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(sdescCls, sel_registerName("alloc")), sel_registerName("init"));
			if (sdesc != nil) {
				((void (*)(id, SEL, NSUInteger))objc_msgSend)(sdesc, sel_registerName("setMinFilter:"), 1 /* MTLSamplerMinMagFilterLinear */);
				((void (*)(id, SEL, NSUInteger))objc_msgSend)(sdesc, sel_registerName("setMagFilter:"), 1 /* MTLSamplerMinMagFilterLinear */);
				((void (*)(id, SEL, NSUInteger))objc_msgSend)(sdesc, sel_registerName("setSAddressMode:"), 0 /* MTLSamplerAddressModeClampToEdge */);
				((void (*)(id, SEL, NSUInteger))objc_msgSend)(sdesc, sel_registerName("setTAddressMode:"), 0 /* MTLSamplerAddressModeClampToEdge */);
				s_ycbcr_sampler = [device newSamplerStateWithDescriptor:sdesc];
				((void (*)(id, SEL))objc_msgSend)(sdesc, sel_registerName("release"));
			}
		}

		s_ycbcr_queue = [device newCommandQueue];
	}
}

static id
rwd_420v_convert_compute(id device, id y_tex, id uv_tex, id dest_tex, NSUInteger width, NSUInteger height)
{
	rwd_init_compute_pipeline(device);
	if (s_ycbcr_pso == nil || s_ycbcr_sampler == nil || s_ycbcr_queue == nil) {
		return nil;
	}

	id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)[s_ycbcr_queue commandBuffer];
	if (cb == nil) {
		return nil;
	}

	id<MTLComputeCommandEncoder> enc = (id<MTLComputeCommandEncoder>)[cb computeCommandEncoder];
	if (enc == nil) {
		return nil;
	}

	[enc setComputePipelineState:s_ycbcr_pso];
	[enc setTexture:y_tex atIndex:0];
	[enc setTexture:uv_tex atIndex:1];
	[enc setTexture:dest_tex atIndex:2];
	[enc setSamplerState:s_ycbcr_sampler atIndex:0];

	MTLSize threadgroupSize = MTLSizeMake(16, 16, 1);
	MTLSize threadgroups = MTLSizeMake((width + 15) / 16, (height + 15) / 16, 1);
	[enc dispatchThreadgroups:threadgroups threadsPerThreadgroup:threadgroupSize];
	[enc endEncoding];

	[cb commit];
	[cb waitUntilCompleted];
	return dest_tex;
}

static id
rwd_420v_convert_software(void *iosurface, id dest_tex, NSUInteger width, NSUInteger height)
{
	rwd_lookup_iosurface_lock_symbols();
	if (iosurf_lock == NULL || iosurf_unlock == NULL || iosurf_get_base_address_of_plane == NULL) {
		return nil;
	}
	if (iosurf_lock(iosurface, 1 /* kIOSurfaceLockReadOnly */, NULL) != 0) {
		return nil;
	}

	const uint8_t *y_plane = (const uint8_t *)iosurf_get_base_address_of_plane(iosurface, 0);
	const uint8_t *uv_plane = (const uint8_t *)iosurf_get_base_address_of_plane(iosurface, 1);
	size_t y_stride = iosurf_get_bytes_per_row_of_plane ? iosurf_get_bytes_per_row_of_plane(iosurface, 0) : width;
	size_t uv_stride = iosurf_get_bytes_per_row_of_plane ? iosurf_get_bytes_per_row_of_plane(iosurface, 1) : width;

	if (y_plane != NULL && uv_plane != NULL) {
		size_t rgb_stride = width * 4;
		uint32_t *rgb_buf = (uint32_t *)malloc(rgb_stride * height);
		if (rgb_buf != NULL) {
			rwd_convert_ycbcr_420v_to_rgb(y_plane, (unsigned)y_stride,
						      uv_plane, (unsigned)uv_stride,
						      rgb_buf, (unsigned)rgb_stride,
						      (unsigned)width, (unsigned)height, 0);

			MTLRegion region = MTLRegionMake2D(0, 0, width, height);
			[dest_tex replaceRegion:region mipmapLevel:0 withBytes:rgb_buf bytesPerRow:rgb_stride];
			free(rgb_buf);
		}
	}
	iosurf_unlock(iosurface, 1, NULL);
	return dest_tex;
}

static id
rwd_hooked_newTextureWithDescriptor_iosurface_plane(id self, SEL _cmd, id desc, void *surf, NSUInteger plane)
{
	NSUInteger pixel_format = (NSUInteger)[desc pixelFormat];

	/*
	 * Intercept format 500 (MTLPixelFormatYCBCR8_420_2P) imports:
	 * CoreAnimation / backboardd or Safari requests a texture for a dual-plane '420v'
	 * IOSurface. We import Plane 0 (Y, R8Unorm) and Plane 1 (UV, RG8Unorm),
	 * allocate a destination BGRA8Unorm texture, and perform BT.601 YUV->RGB
	 * conversion via Metal compute shader (or software fallback), returning the
	 * resulting single-plane RGB texture.
	 */
	if (pixel_format == 500 && surf != NULL && plane == 0) {
		NSUInteger width = [desc width];
		NSUInteger height = [desc height];

		/* 1. Plane 0 (Y): R8Unorm (format 10) */
		id desc0 = [desc copy];
		[desc0 setPixelFormat:10];
		id y_tex = orig_dev_new_texture_surf ? orig_dev_new_texture_surf(self, _cmd, desc0, surf, 0) : nil;
		[desc0 release];

		/* 2. Plane 1 (UV): RG8Unorm (format 30) */
		id desc1 = [desc copy];
		[desc1 setPixelFormat:30];
		[desc1 setWidth:(width + 1) / 2];
		[desc1 setHeight:(height + 1) / 2];
		id uv_tex = orig_dev_new_texture_surf ? orig_dev_new_texture_surf(self, _cmd, desc1, surf, 1) : nil;
		[desc1 release];

		if (y_tex != nil && uv_tex != nil) {
			/* 3. Destination texture: BGRA8Unorm (format 80) */
			id dest_desc = [desc copy];
			[dest_desc setPixelFormat:80];
			[dest_desc setUsage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget];
			id dest_tex = [self newTextureWithDescriptor:dest_desc];
			[dest_desc release];

			if (dest_tex != nil) {
				id res = rwd_420v_convert_compute(self, y_tex, uv_tex, dest_tex, width, height);
				if (res == nil) {
					res = rwd_420v_convert_software(surf, dest_tex, width, height);
				}
				if (res != nil) {
					return res;
				}
			}
		}
	}

	if (orig_dev_new_texture_surf) {
		return orig_dev_new_texture_surf(self, _cmd, desc, surf, plane);
	}
	return nil;
}

static void
rwd_install_420v_hooks(void)
{
	static int installed_420v;
	if (installed_420v) {
		return;
	}
	Class devClass = objc_getClass("RedwoodMetalDevice");
	if (devClass != Nil) {
		SEL sel = sel_registerName("newTextureWithDescriptor:iosurface:plane:");
		Method m = class_getInstanceMethod(devClass, sel);
		if (m != NULL && orig_dev_new_texture_surf == NULL) {
			orig_dev_new_texture_surf = (id (*)(id, SEL, id, void *, NSUInteger))method_getImplementation(m);
			method_setImplementation(m, (IMP)rwd_hooked_newTextureWithDescriptor_iosurface_plane);
			installed_420v = 1;
			printf("redwooddrawable: installed 420v YCbCr two-plane conversion hook on RedwoodMetalDevice\n");
		}
	}
}
