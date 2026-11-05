/*
 * An IOSurface of CoreAnimation's, as a texture of this driver.
 *
 * WHY THIS EXISTS, in one measurement. Showing a frame on this port costs 15.46 ms
 * (docs/metal.md 10.43): -getBytes: pulls a finished attachment into the process (11.96 ms,
 * 867 MB/s) and kRedwoodShow pushes it into the kernel (3.50 ms, 2962 MB/s). That is a
 * ceiling of 64.7 frames a second with nothing drawn, and about twelve of those
 * milliseconds exist for one reason only: there is nowhere else to put the picture. A
 * drawable is that somewhere else -- memory the display controller already scans out -- and
 * both copies go away, not get faster.
 *
 * WHAT COREANIMATION ACTUALLY ASKS FOR, read out of this phone's own cache (every address
 * below is in docs/metal.md 10.46 with what it was read from). -[CAMetalLayer nextDrawable]
 * (0x18829de60) ends in get_unused_drawable (0x18837cff8), which makes the surface itself
 * with CA::Render::create_iosurface_with_pixel_format (0x18833cfa8) and then calls
 * allocate_drawable_texture (0x1884b3470). That function sends a device exactly one message:
 *
 *     -newTextureWithDescriptor:iosurface:plane:   (0x1885c3060)
 *
 * with a plain MTLTextureDescriptor it has just built, the surface, and plane 0. Everything
 * else on that path is Apple's or CoreAnimation's: the descriptor's private setters
 * (-setRotation:, -setIsDrawable:, -setFramebufferOnly:, -setUsage:, -setResourceIndex:) are
 * all MTLTextureDescriptorInternal's own, and presentation never comes near a device at all
 * -- register_drawable (0x18838692c) hands the surface to the layer's CAImageQueue with
 * CAImageQueueRegisterIOSurfaceBuffer and -[CAMetalDrawable present] inserts the buffer id
 * into that queue. So the whole of what this port owes a drawable is the one method.
 *
 * AND THAT METHOD HAS FOUR OTHER CALLERS, which the first boot of this file found out the
 * hard way. A scan of QuartzCore's __text for BL to the selector stub finds five sites; the
 * four that are not allocate_drawable_texture are the window server's Metal compositor:
 * CA::OGL::MetalContext::set_destination_iosurface (0x188302550), ::update_surface
 * (0x1883163e0), ::get_surface (0x18833e724) and ::create_surface_from_iosurface
 * (0x18835e55c). They composite whatever surface a client gave the window server, so they
 * ask for formats no CAMetalLayer may hold -- backboardd asked 55 times in one boot for
 * MTLPixelFormat 500, which is MTLPixelFormatYCBCR8_420_2P. Three of the four pass plane 0
 * unconditionally; ::update_surface passes a plane variable (csel x4 at 0x1883163d8), so the
 * compositor does per-plane imports when it gets that far. This file therefore serves "a
 * texture over an IOSurface" for whoever asks, and refuses what one linear plane cannot be.
 *
 * HOW THE MEMORY GETS HERE. Four layers, and all four now exist:
 *
 *   the kernel     kRedwoodSurface (0x7e) turns an IOSurface into a GPU buffer and reports
 *                  its size, offset, row length and geometry (RedwoodGPU.cpp:821)
 *   the DRM shim   drmPrimeFDToHandle(fd, surface_id, &handle) -- the ID goes where Linux
 *                  puts a dmabuf descriptor (tools/mesa/redwood-drm/redwood_drm.c:189)
 *   freedreno      fd_bo_from_dmabuf_drm takes it ("what comes in here is an IOSurface, and
 *                  the file descriptor its ID"), and fd_resource_from_handle lays a resource
 *                  over it with one pitch
 *   the contract   rwa_texture_from_surface (tools/mesa/air/air_metal.h)
 *
 * The last of those is new, and it is still looked up rather than linked: an image built
 * before it existed has a library without the symbol, and then this file refuses BY NAME
 * instead of returning a bare nil. That distinction is not tidiness -- get_unused_drawable
 * does NOT test the texture it gets back (no branch between the call at 0x18837d220 and the
 * store at 0x18837d224), so a nil here becomes a CAMetalDrawable whose -texture is nil, and
 * the application finds out later, somewhere else, as an assertion about a nil colour
 * attachment. A sentence at the moment of the refusal names what was missing.
 *
 * WHO OWNS THE BUFFER, because this was wrong once. The buffer the kernel makes for a
 * surface belongs to freedreno's table from the moment it imports it, and
 * rwa_texture_destroy gives it back. So this file only DESCRIBES the surface (which
 * allocates nothing) and leaves the taking to the library. Taking it here as well would be a
 * second buffer over the same memory on a second user client -- a waste rather than a fault,
 * but closing it would then look like closing the library's.
 */

#include <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#import <mach-o/dyld.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "redwood_drawable.h"

/*
 * The two requests this file needs, by the numbers the kernel answers them on
 * (xnu/files/iokit/Kernel/RedwoodGPU.cpp:78-95): kRedwoodSurface is this port's own and
 * kDrmGemClose is Linux's. Repeated here rather than shared for the reason the structs are
 * repeated, and it is two lines.
 */
#define RWD_REQUEST_SURFACE   0x7e
#define RWD_REQUEST_GEM_CLOSE 0x09

/* A sentence into a caller's buffer, once, with no dependency on the caller having one. */
static void
rwd_say(char *why, size_t why_size, const char *format, ...)
{
	va_list arguments;

	if (why == NULL || why_size == 0) {
		return;
	}
	va_start(arguments, format);
	vsnprintf(why, why_size, format, arguments);
	va_end(arguments);
}

int
rwd_gpu_open(io_connect_t *connection, char *why, size_t why_size)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodGPU"));
	kern_return_t result;

	if (connection == NULL) {
		rwd_say(why, why_size, "rwd_gpu_open was given nowhere to put the connection");
		return -1;
	}
	*connection = 0;
	if (service == IO_OBJECT_NULL) {
		rwd_say(why, why_size, "no RedwoodGPU service: this kernel was booted without "
		    "gpu=1, so there is no GPU to give a surface to");
		return -1;
	}
	result = IOServiceOpen(service, mach_task_self(), 0, connection);
	IOObjectRelease(service);
	if (result != KERN_SUCCESS) {
		*connection = 0;
		rwd_say(why, why_size, "RedwoodGPU refused a user client: %#x", result);
		return -1;
	}
	return 0;
}

void
rwd_gpu_close(io_connect_t connection)
{
	if (connection != 0) {
		IOServiceClose(connection);
	}
}

/*
 * The connection a drawable's buffer lives on, opened once and never closed.
 *
 * This is not a convenience, it is the buffer's lifetime. A buffer made by kRedwoodSurface
 * belongs to the user client that asked for it, and IOSurfaceRootUserClient's and
 * RedwoodGPUUserClient's clientClose free what their client held -- so a connection closed
 * while a texture still refers to its buffer takes the texture's memory with it. One
 * connection for the process, kept for as long as the process has textures, is the only
 * lifetime that is right, and it costs one Mach port.
 *
 * 0 when there is no GPU service, which is a kernel booted without gpu=1 and the ordinary
 * case rather than a fault; the sentence is printed once, by whoever asked first.
 */
static io_connect_t
rwd_shared_connection(void)
{
	static io_connect_t connection;
	static int opened;

	if (!opened) {
		char why[256] = { 0 };

		opened = 1;
		if (rwd_gpu_open(&connection, why, sizeof(why)) != 0) {
			connection = 0;
			printf("redwooddrawable: %s\n", why);
		}
	}
	return connection;
}

/*
 * One kRedwoodSurface call. The kernel takes the struct in and hands the same struct back
 * filled (RedwoodGPU.cpp:1283 and the common copy at the end of externalMethod), so in and
 * out are one buffer of one size, and asking for fewer bytes back than were sent loses the
 * tail of the answer -- which is the one way to misuse this request.
 */
static int
rwd_surface_call(io_connect_t connection, struct rwd_surface *surface, const char *what,
		 char *why, size_t why_size)
{
	struct rwd_surface reply;
	size_t size = sizeof(reply);
	kern_return_t result;

	if (connection == 0) {
		rwd_say(why, why_size, "no connection to RedwoodGPU to %s a surface with", what);
		return -1;
	}
	result = IOConnectCallStructMethod(connection, RWD_REQUEST_SURFACE, surface,
	    sizeof(*surface), &reply, &size);
	if (result != KERN_SUCCESS) {
		/*
		 * Two of these answers mean something nameable and the rest do not, so only
		 * those two are translated. kIOReturnNotFound (0xe00002f0) is a surface ID the
		 * kernel's table does not hold -- a surface from another process that was never
		 * made shareable, or one already released. kIOReturnUnsupported (0xe00002c7) has
		 * exactly one cause here (RedwoodGPU.cpp:830): the surface was built around
		 * memory the process already had, which is neither whole pages nor in rows the
		 * GPU takes.
		 */
		rwd_say(why, why_size, "the kernel would not %s surface %u: %#x%s", what,
		    surface->surface, result,
		    result == kIOReturnUnsupported
			? " (that surface wraps memory of the process's own; the GPU cannot "
			  "take such memory as a buffer)"
			: result == kIOReturnNotFound
			    ? " (no surface of that ID in the kernel's table)"
			    : "");
		return -1;
	}
	if (size < sizeof(reply)) {
		rwd_say(why, why_size, "the kernel answered %zu bytes about surface %u, not %zu",
		    size, surface->surface, sizeof(reply));
		return -1;
	}
	*surface = reply;
	return 0;
}

int
rwd_surface_describe(io_connect_t connection, uint32_t surfaceID, struct rwd_surface *surface,
		     char *why, size_t why_size)
{
	if (surface == NULL) {
		rwd_say(why, why_size, "rwd_surface_describe was given nowhere to answer into");
		return -1;
	}
	memset(surface, 0, sizeof(*surface));
	surface->surface = surfaceID;
	surface->handle = RWD_DESCRIBE;
	return rwd_surface_call(connection, surface, "describe", why, why_size);
}

int
rwd_surface_take(io_connect_t connection, uint32_t surfaceID, struct rwd_surface *surface,
		 char *why, size_t why_size)
{
	if (surface == NULL) {
		rwd_say(why, why_size, "rwd_surface_take was given nowhere to answer into");
		return -1;
	}
	memset(surface, 0, sizeof(*surface));
	surface->surface = surfaceID;
	surface->handle = 0;
	if (rwd_surface_call(connection, surface, "take", why, why_size) != 0) {
		return -1;
	}
	if (surface->handle == 0) {
		rwd_say(why, why_size, "the kernel took surface %u and gave back buffer 0, "
		    "which is no buffer", surfaceID);
		return -1;
	}
	return 0;
}

int
rwd_surface_give_back(io_connect_t connection, uint32_t handle, char *why, size_t why_size)
{
	struct {
		uint32_t handle, pad;
	} close = { handle, 0 };
	kern_return_t result;

	if (connection == 0 || handle == 0) {
		return 0;                 /* nothing was taken, so nothing is owed */
	}
	result = IOConnectCallStructMethod(connection, RWD_REQUEST_GEM_CLOSE, &close,
	    sizeof(close), NULL, NULL);
	if (result != KERN_SUCCESS) {
		rwd_say(why, why_size, "the kernel would not take buffer %u back: %#x", handle,
		    result);
		return -1;
	}
	return 0;
}

static uint32_t (*iosurf_get_id)(void *);
static size_t (*iosurf_get_plane_count)(void *);
static size_t (*iosurf_get_width_of_plane)(void *, size_t);
static size_t (*iosurf_get_height_of_plane)(void *, size_t);
static size_t (*iosurf_get_bytes_per_row_of_plane)(void *, size_t);
static size_t (*iosurf_get_element_offset_of_plane)(void *, size_t);

static void
rwd_lookup_iosurface_symbols(void)
{
	static int looked;

	if (!looked) {
		/*
		 * RTLD_DEFAULT and not a link: this driver is loaded as a bundle into whatever
		 * process asks Metal, and a hard dependency on IOSurface.framework would fail
		 * that load rather than fail this one call. A process that has a CAMetalLayer
		 * has the framework loaded already -- CAMetalDrawable's own private structure
		 * holds an `__IOSurface *` -- so the symbol is there exactly when it is wanted.
		 */
		*(void **)&iosurf_get_id = dlsym(RTLD_DEFAULT, "IOSurfaceGetID");
		*(void **)&iosurf_get_plane_count = dlsym(RTLD_DEFAULT, "IOSurfaceGetPlaneCount");
		*(void **)&iosurf_get_width_of_plane = dlsym(RTLD_DEFAULT, "IOSurfaceGetWidthOfPlane");
		*(void **)&iosurf_get_height_of_plane = dlsym(RTLD_DEFAULT, "IOSurfaceGetHeightOfPlane");
		*(void **)&iosurf_get_bytes_per_row_of_plane = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRowOfPlane");
		*(void **)&iosurf_get_element_offset_of_plane = dlsym(RTLD_DEFAULT, "IOSurfaceGetElementOffsetOfPlane");
		looked = 1;
		if (iosurf_get_id == NULL) {
			printf("redwooddrawable: this process has no IOSurfaceGetID, so a "
			       "surface cannot be named to the kernel\n");
		}
	}
}

uint32_t
rwd_surface_id(void *iosurface)
{
	if (iosurface == NULL) {
		return 0;
	}
	rwd_lookup_iosurface_symbols();
	return iosurf_get_id != NULL ? iosurf_get_id(iosurface) : 0;
}

int
rwd_surface_fits_adreno(const struct rwd_surface *surface, unsigned bytes_per_pixel,
			char *why, size_t why_size)
{
	/*
	 * a6xx's minimum pitchalign is 6 -- fd_resource_from_handle sets
	 * MAX2(fdl_cpp_shift + log2(gmem_align_w), 6) and gmem_align_w is 16 for this
	 * generation, which gives 2 + 4 = 6 for a four-byte pixel -- so a row has to be a
	 * multiple of 64 bytes. Named as the number rather than the shift, because the number
	 * is what the kernel's MINISURF_ROW_ALIGN also is, and the agreement between the two
	 * trees is the whole reason this is expected to pass.
	 */
	const unsigned align = 64;
	uint64_t least;

	if (surface == NULL || bytes_per_pixel == 0) {
		rwd_say(why, why_size, "rwd_surface_fits_adreno needs a surface and a pixel size");
		return -1;
	}
	if (surface->planes > 1 && bytes_per_pixel > 2) {
		rwd_say(why, why_size, "surface %u has %u planes; a drawable's texture is one "
		    "plane and this driver imports no others", surface->surface, surface->planes);
		return -1;
	}
	if (surface->planes > 2) {
		rwd_say(why, why_size, "surface %u has %u planes; this driver imports up to 2 planes",
		    surface->surface, surface->planes);
		return -1;
	}
	least = (uint64_t)surface->width * bytes_per_pixel;
	if (surface->rowBytes < least) {
		rwd_say(why, why_size, "surface %u has %u bytes a row, and %ux%u pixels of %u "
		    "bytes need at least %llu", surface->surface, surface->rowBytes,
		    surface->width, surface->height, bytes_per_pixel,
		    (unsigned long long)least);
		return -1;
	}
	if (surface->rowBytes % align != 0) {
		rwd_say(why, why_size, "surface %u has %u bytes a row, which is not a multiple "
		    "of %u; freedreno refuses such an import (fd_resource_from_handle), and the "
		    "kernel's own surfaces are aligned to %u (MINISURF_ROW_ALIGN)",
		    surface->surface, surface->rowBytes, align, align);
		return -1;
	}
	if (surface->size < (uint64_t)surface->rowBytes * surface->height + surface->offset) {
		rwd_say(why, why_size, "surface %u is %llu bytes, and %u rows of %u bytes from "
		    "offset %u need %llu", surface->surface, (unsigned long long)surface->size,
		    surface->height, surface->rowBytes, surface->offset,
		    (unsigned long long)((uint64_t)surface->rowBytes * surface->height +
		    surface->offset));
		return -1;
	}
	return 0;
}

/*
 * Metal's pixel format, CoreAnimation's FourCC and the size of a pixel, in one table.
 *
 * The single-plane entries are read out of -[CAMetalLayer setPixelFormat:] (0x1882a3d38),
 * which is where that correspondence is decided for a drawable: it accepts 10/11, 70/71,
 * 80/81, 90, 94, 115 and 550-555, raises an NSException on anything else, and composes the
 * surface's FourCC from the number.
 *
 * But a drawable is NOT the only caller, and assuming it was cost a wrong sentence in a real
 * boot (Metal notes 10.46, "format 500"). Five call sites in QuartzCore send
 * -newTextureWithDescriptor:iosurface:plane:, and four of them are the window server's Metal
 * compositor rather than the drawable path: CA::OGL::MetalContext::set_destination_iosurface
 * (0x188302550), ::update_surface (0x1883163e0), ::get_surface (0x18833e724) and
 * ::create_surface_from_iosurface (0x18835e55c). Those composite whatever surface a client
 * handed the window server, so they ask for formats no CAMetalLayer may hold -- and the
 * authority for what this import can serve is therefore NOT CAMetalLayer's list but what one
 * linear plane can be. Hence the refusals below say what they are refusing.
 *
 * Two families are in the table and then refused, each for its own measured reason:
 *
 *   the XR formats (550-555, and 94)   the kernel substitutes 'w30r', 'l10r', 'w30R' and
 *       'b3a8' with 'BGRA' of the same geometry (MiniIOSurface.cpp:466-479), so a ten-bit
 *       surface asked for comes back eight-bit and a texture made to the asked-for format
 *       would compute wrong colours into it rather than fail;
 *   the YCbCr formats (500-629)        two planes, and nothing on this port has ever carried
 *       a second plane. See rwd_format_of_metal for the three things that would be needed.
 */
struct rwd_format {
	unsigned metal;
	uint32_t fourcc;
	unsigned bytes_per_pixel;
	int substituted;      /* the kernel turns this FourCC into something else */
	const char *name;
};

static const struct rwd_format rwd_formats[] = {
	{  10, 'A008', 1, 0, "MTLPixelFormatR8Unorm" },
	{  11, 'A008', 1, 0, "MTLPixelFormatR8Unorm_sRGB" },
	{  30, 'RG08', 2, 0, "MTLPixelFormatRG8Unorm" },
	{  31, 'RG08', 2, 0, "MTLPixelFormatRG8Unorm_sRGB" },
	{  70, 'RGBA', 4, 0, "MTLPixelFormatRGBA8Unorm" },
	{  71, 'RGBA', 4, 0, "MTLPixelFormatRGBA8Unorm_sRGB" },
	{  80, 'BGRA', 4, 0, "MTLPixelFormatBGRA8Unorm" },
	{  81, 'BGRA', 4, 0, "MTLPixelFormatBGRA8Unorm_sRGB" },
	{  90, 'R10k', 4, 0, "a ten-bit RGB format" },
	{  94, 'l10r', 4, 1, "a ten-bit RGB format" },
	{ 115, 'RGhA', 8, 0, "MTLPixelFormatRGBA16Float" },
	{ 500, '420v', 1, 0, "MTLPixelFormatYCBCR8_420_2P" },
	{ 550, 'b3a8', 4, 1, "a ten-bit XR format" },
	{ 551, 'b3a8', 4, 1, "a ten-bit XR format" },
	{ 552, 'w40a', 4, 1, "a ten-bit XR format" },
	{ 553, 'w40a', 4, 1, "a ten-bit XR format" },
	{ 554, 'w30r', 4, 1, "a ten-bit XR format" },
	{ 555, 'w30r', 4, 1, "a ten-bit XR format" },
};

int
rwd_format_of_metal(unsigned metal_pixel_format, uint32_t *fourcc, unsigned *bytes_per_pixel,
		    char *why, size_t why_size)
{
	if (metal_pixel_format == 500) {
		if (fourcc == NULL && bytes_per_pixel == NULL) {
			rwd_say(why, why_size, "pixel format 500 is MTLPixelFormatYCBCR8_420_2P: "
			    "YCbCr in TWO PLANES, and this import serves one. Missing: per-plane "
			    "offset and row length from the kernel (kRedwoodSurface reports only "
			    "the count, RedwoodGPU.cpp:849), a Gallium format for a biplanar Metal "
			    "texture, and a YCbCr-to-RGB conversion. Refused whole rather than "
			    "served as its luma plane, which would composite grey and look like a "
			    "bug elsewhere");
			return -1;
		}
		if (fourcc != NULL) {
			*fourcc = '420v';
		}
		if (bytes_per_pixel != NULL) {
			*bytes_per_pixel = 1;
		}
		return 0;
	}
	for (size_t i = 0; i < sizeof(rwd_formats) / sizeof(rwd_formats[0]); i++) {
		const struct rwd_format *f = &rwd_formats[i];

		if (f->metal != metal_pixel_format) {
			continue;
		}
		if (f->substituted) {
			rwd_say(why, why_size, "pixel format %u is %s, whose surface this "
			    "kernel makes as ordinary BGRA of the same geometry "
			    "(MiniIOSurface.cpp substitutes '%c%c%c%c'); a texture of the asked-"
			    "for format over such a surface would write wrong colours rather "
			    "than fail, so it is refused", metal_pixel_format, f->name,
			    (char)(f->fourcc >> 24), (char)(f->fourcc >> 16),
			    (char)(f->fourcc >> 8), (char)f->fourcc);
			return -1;
		}
		if (fourcc != NULL) {
			*fourcc = f->fourcc;
		}
		if (bytes_per_pixel != NULL) {
			*bytes_per_pixel = f->bytes_per_pixel;
		}
		return 0;
	}
	/*
	 * The YCbCr family, named rather than counted. Metal handles 500 to 629 as one block --
	 * `sub x8, x20, #0x1f4; cmp x8, #0x81` at 0x181a1f614 in the function behind
	 * MTLPixelFormatGetName -- and the case for offset 0 of its jump table stores the name
	 * at 0x181b643bf, which __TEXT,__cstring spells MTLPixelFormatYCBCR8_420_2P. So 500 is
	 * eight-bit YCbCr, 4:2:0, TWO PLANES, and the window server really does ask for it: 55
	 * times in one boot, all for one surface, out of backboardd.
	 */
	if (metal_pixel_format > 500 && metal_pixel_format <= 629) {
		/* Kept short enough to fit a 512-byte buffer whole: a refusal that is cut off
		 * mid-reason is a refusal nobody can act on, which is the failure this file
		 * exists to avoid. The three things it needs are in docs/metal.md 10.46. */
		rwd_say(why, why_size, "pixel format %u is %s: YCbCr in TWO PLANES, and this "
		    "import serves one. Missing: per-plane offset and row length from the kernel "
		    "(kRedwoodSurface reports only the count, RedwoodGPU.cpp:849), a Gallium "
		    "format for a biplanar Metal texture, and a YCbCr-to-RGB conversion. Refused "
		    "whole rather than served as its luma plane, which would composite grey and "
		    "look like a bug elsewhere", metal_pixel_format,
		    "one of Metal's private YCbCr formats (500-629)");
		return -1;
	}
	/*
	 * And the catch-all, which deliberately does NOT say "so it did not come from a
	 * drawable" any more. It did not, and saying so was true and useless: four of the five
	 * callers of this method are not the drawable path at all (see the table above), so the
	 * interesting fact is what this import can lay over a linear surface, not which caller
	 * it was.
	 */
	/*
	 * The list of what IS served is built from the table rather than written out beside it.
	 * That is not neatness: a sentence listing numbers next to a table of numbers drifts the
	 * moment one of them changes, and this very sentence had 90 in the wrong half of it
	 * before the table was read again.
	 */
	char served[160] = "", refused[160] = "";
	size_t sn = 0, rn = 0;

	for (size_t i = 0; i < sizeof(rwd_formats) / sizeof(rwd_formats[0]); i++) {
		char *into = rwd_formats[i].substituted ? refused : served;
		size_t *at = rwd_formats[i].substituted ? &rn : &sn;
		int wrote = snprintf(into + *at, (rwd_formats[i].substituted ? sizeof(refused)
								            : sizeof(served)) - *at,
		    "%s%u", *at ? ", " : "", rwd_formats[i].metal);

		if (wrote > 0) {
			*at += (size_t)wrote;
		}
	}
	rwd_say(why, why_size, "pixel format %u is not one this import can lay over a single "
	    "linear plane. It serves %s; it refuses %s because this kernel substitutes BGRA for "
	    "those FourCCs, and 500-629 because they are two planes", metal_pixel_format, served,
	    refused);
	return -1;
}

/*
 * The import call of the compiler library, if it has one.
 *
 * It is looked up rather than linked for the same reason src/redwoodmetal.m dlopens the
 * library at all, and with RTLD_NOLOAD so that this file never loads a second copy: dyld
 * uniques images by install_name, so a handle asked for this way is the one the driver
 * already has, or nothing when the driver never got one.
 *
 * The signature is tools/mesa/air/air_metal.h's, field for field: the shape the caller wants,
 * the surface by its ID (which is where freedreno's import takes it --
 * drmPrimeFDToHandle(fd, surface, &handle) in tools/mesa/redwood-drm/redwood_drm.c:189), and
 * the row length and offset the kernel reported, because fd_resource_from_handle takes those
 * from the caller and does not ask the kernel again.
 */
typedef struct rwa_texture *(*rwd_import_call)(struct rwa_device *device,
					       const struct rwd_texture_shape *shape,
					       int surface_id, unsigned offset,
					       unsigned row_bytes, char *why, size_t why_size);

static rwd_import_call
rwd_import(void)
{
	static rwd_import_call import;
	static int looked;

	if (!looked) {
		void *library = dlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW | RTLD_NOLOAD);

		looked = 1;
		if (library != NULL) {
			*(void **)&import = dlsym(library, "rwa_texture_from_surface");
		}
		printf("redwooddrawable: the compiler library %s, and %s "
		       "rwa_texture_from_surface\n",
		    library != NULL ? "is loaded in this process" : "is not loaded here",
		    import != NULL ? "has" : "has NOT");
	}
	return import;
}

int
rwd_import_available(void)
{
	return rwd_import() != NULL;
}

struct rwa_texture *
rwd_texture_for_surface(struct rwa_device *mesa_device, void *iosurface, unsigned long plane,
			const struct rwd_texture_shape *want, struct rwd_surface *took,
			char *why, size_t why_size)
{
	struct rwd_surface surface;
	io_connect_t connection;
	rwd_import_call import;
	struct rwa_texture *texture;
	unsigned bytes_per_pixel = 0;
	uint32_t fourcc = 0, id;
	int is_ycbcr_biplanar = 0;

	if (took != NULL) {
		memset(took, 0, sizeof(*took));
	}
	if (mesa_device == NULL || want == NULL) {
		rwd_say(why, why_size, "a drawable's texture needs the compiler library's device "
		    "and a shape to make it in");
		return NULL;
	}
	if (plane > 1) {
		rwd_say(why, why_size, "plane %lu of a surface was asked for; this driver "
		    "imports plane 0 and plane 1 only", plane);
		return NULL;
	}
	id = rwd_surface_id(iosurface);
	if (id == 0) {
		rwd_say(why, why_size, "that surface has no ID, so there is nothing for the "
		    "kernel to look up (a surface is given one when it is made shareable)");
		return NULL;
	}
	if (rwd_format_of_metal(want->pixel_format, &fourcc, &bytes_per_pixel, why,
	    why_size) != 0) {
		return NULL;
	}
	connection = rwd_shared_connection();
	if (connection == 0) {
		rwd_say(why, why_size, "no connection to RedwoodGPU: this kernel was booted "
		    "without gpu=1, so a surface's memory cannot be given to the GPU");
		return NULL;
	}
	/*
	 * Describe before taking. Describing allocates nothing, so every disagreement below is
	 * found without a buffer having to be given back -- and the disagreements are the
	 * point: the descriptor was built by CoreAnimation from the layer's drawableSize and
	 * pixelFormat, and the surface was made by CoreAnimation from the same two, but this
	 * driver has seen neither of those steps and must not assume they agreed.
	 */
	if (rwd_surface_describe(connection, id, &surface, why, why_size) != 0) {
		return NULL;
	}
	is_ycbcr_biplanar = (want->pixel_format == 500 || surface.pixelFormat == '420v' || surface.pixelFormat == '420f');
	if (plane != 0 && !is_ycbcr_biplanar) {
		rwd_say(why, why_size, "plane %lu of a surface was asked for; single-plane formats "
		    "import plane 0 only", plane);
		return NULL;
	}
	if (!is_ycbcr_biplanar && (surface.width != want->width || surface.height != want->height)) {
		rwd_say(why, why_size, "surface %u is %ux%u and the descriptor asks for %ux%u",
		    id, surface.width, surface.height, want->width, want->height);
		return NULL;
	}
	if (surface.pixelFormat != fourcc) {
		if (is_ycbcr_biplanar && (surface.pixelFormat == '420v' || surface.pixelFormat == '420f')) {
			/* Accepted bi-planar YCbCr FourCC */
		} else {
			rwd_say(why, why_size, "surface %u is '%c%c%c%c' and Metal format %u is "
			    "'%c%c%c%c'", id, (char)(surface.pixelFormat >> 24),
			    (char)(surface.pixelFormat >> 16), (char)(surface.pixelFormat >> 8),
			    (char)surface.pixelFormat, want->pixel_format, (char)(fourcc >> 24),
			    (char)(fourcc >> 16), (char)(fourcc >> 8), (char)fourcc);
			return NULL;
		}
	}
	if (rwd_surface_fits_adreno(&surface, bytes_per_pixel, why, why_size) != 0) {
		return NULL;
	}
	/*
	 * Everything that could be checked has been. The one thing left is the call that does
	 * not exist, and it is refused here with the whole of what was learned, because this
	 * sentence is what the next boot will have to go on.
	 */
	import = rwd_import();
	if (import == NULL) {
		rwd_say(why, why_size, "surface %u fits (%ux%u '%c%c%c%c', %u bytes a row, "
		    "%llu bytes from offset %u) and the kernel would hand its memory over as a "
		    "GPU buffer, but the compiler library in this image has no "
		    "rwa_texture_from_surface: that image was built before the call existed, and "
		    "without it there is no drawable", id, surface.width, surface.height,
		    (char)(surface.pixelFormat >> 24), (char)(surface.pixelFormat >> 16),
		    (char)(surface.pixelFormat >> 8), (char)surface.pixelFormat,
		    surface.rowBytes, (unsigned long long)surface.size, surface.offset);
		return NULL;
	}

	struct rwd_texture_shape import_shape = *want;
	unsigned req_offset = surface.offset;
	unsigned req_row_bytes = surface.rowBytes;

	if (is_ycbcr_biplanar) {
		if (plane == 0) {
			/* Y plane: Luma R8Unorm */
			import_shape.pixel_format = 10;
			if (iosurface != NULL && iosurf_get_plane_count != NULL && iosurf_get_plane_count(iosurface) >= 1) {
				if (iosurf_get_width_of_plane != NULL)
					import_shape.width = (unsigned)iosurf_get_width_of_plane(iosurface, 0);
				if (iosurf_get_height_of_plane != NULL)
					import_shape.height = (unsigned)iosurf_get_height_of_plane(iosurface, 0);
				if (iosurf_get_bytes_per_row_of_plane != NULL)
					req_row_bytes = (unsigned)iosurf_get_bytes_per_row_of_plane(iosurface, 0);
				if (iosurf_get_element_offset_of_plane != NULL)
					req_offset = (unsigned)iosurf_get_element_offset_of_plane(iosurface, 0);
			} else {
				import_shape.width = surface.width;
				import_shape.height = surface.height;
				req_row_bytes = surface.rowBytes;
				req_offset = surface.offset;
			}
		} else {
			/* UV plane: Chroma RG8Unorm */
			import_shape.pixel_format = 30;
			if (iosurface != NULL && iosurf_get_plane_count != NULL && iosurf_get_plane_count(iosurface) >= 2) {
				if (iosurf_get_width_of_plane != NULL)
					import_shape.width = (unsigned)iosurf_get_width_of_plane(iosurface, 1);
				if (iosurf_get_height_of_plane != NULL)
					import_shape.height = (unsigned)iosurf_get_height_of_plane(iosurface, 1);
				if (iosurf_get_bytes_per_row_of_plane != NULL)
					req_row_bytes = (unsigned)iosurf_get_bytes_per_row_of_plane(iosurface, 1);
				if (iosurf_get_element_offset_of_plane != NULL)
					req_offset = (unsigned)iosurf_get_element_offset_of_plane(iosurface, 1);
			} else {
				import_shape.width = (surface.width + 1) / 2;
				import_shape.height = (surface.height + 1) / 2;
				req_row_bytes = surface.rowBytes;
				req_offset = surface.offset + (surface.rowBytes * surface.height);
				req_offset = (req_offset + 63) & ~63u;
			}
		}
	}

	texture = import(mesa_device, &import_shape, (int)id, req_offset, req_row_bytes, why,
	    why_size);
	if (texture == NULL) {
		return NULL;    /* the library wrote the sentence; it names the GPU's own reason */
	}
	if (took != NULL) {
		*took = surface;    /* what the surface turned out to be, for whoever logs it */
	}
	return texture;
}

/*
 * Dual-plane 420v YCbCr (NV12) to RGB Metal conversion shader.
 * Converts Y (Plane 0, R8Unorm) and UV (Plane 1, RG8Unorm) into RGB.
 * Implements ITU-R BT.601 video-range conversion standard for iOS.
 */
static const char * const rwd_ycbcr_shader_source =
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"\n"
	"kernel void ycbcr_420v_to_rgb(\n"
	"    texture2d<float, access::sample> y_tex   [[texture(0)]],\n"
	"    texture2d<float, access::sample> uv_tex  [[texture(1)]],\n"
	"    texture2d<float, access::write>  out_tex [[texture(2)]],\n"
	"    sampler s                                [[sampler(0)]],\n"
	"    uint2 gid                                [[thread_position_in_grid]])\n"
	"{\n"
	"    if (gid.x >= out_tex.get_width() || gid.y >= out_tex.get_height()) {\n"
	"        return;\n"
	"    }\n"
	"    float2 coord = (float2(gid) + 0.5f) / float2(out_tex.get_width(), out_tex.get_height());\n"
	"    float y = y_tex.sample(s, coord).r;\n"
	"    float2 uv = uv_tex.sample(s, coord).rg;\n"
	"    float y_adj = (y - 16.0f / 255.0f) * 1.164383f;\n"
	"    float cb = uv.x - 128.0f / 255.0f;\n"
	"    float cr = uv.y - 128.0f / 255.0f;\n"
	"    float r = y_adj + 1.596027f * cr;\n"
	"    float g = y_adj - 0.391762f * cb - 0.812968f * cr;\n"
	"    float b = y_adj + 2.017232f * cb;\n"
	"    out_tex.write(float4(clamp(float3(r, g, b), 0.0f, 1.0f), 1.0f), gid);\n"
	"}\n"
	"\n"
	"fragment float4 ycbcr_420v_fragment(\n"
	"    float2 texCoord                         [[stage_in]],\n"
	"    texture2d<float, access::sample> y_tex  [[texture(0)]],\n"
	"    texture2d<float, access::sample> uv_tex [[texture(1)]],\n"
	"    sampler s                               [[sampler(0)]])\n"
	"{\n"
	"    float y = y_tex.sample(s, texCoord).r;\n"
	"    float2 uv = uv_tex.sample(s, texCoord).rg;\n"
	"    float y_adj = (y - 16.0f / 255.0f) * 1.164383f;\n"
	"    float cb = uv.x - 128.0f / 255.0f;\n"
	"    float cr = uv.y - 128.0f / 255.0f;\n"
	"    float r = y_adj + 1.596027f * cr;\n"
	"    float g = y_adj - 0.391762f * cb - 0.812968f * cr;\n"
	"    float b = y_adj + 2.017232f * cb;\n"
	"    return float4(clamp(float3(r, g, b), 0.0f, 1.0f), 1.0f);\n"
	"}\n";

const char *
rwd_ycbcr_conversion_shader_source(void)
{
	return rwd_ycbcr_shader_source;
}

void
rwd_convert_ycbcr_420v_to_rgb(const uint8_t *y_plane, unsigned y_stride,
			      const uint8_t *uv_plane, unsigned uv_stride,
			      uint32_t *rgb_out, unsigned rgb_stride,
			      unsigned width, unsigned height, int full_range)
{
	if (y_plane == NULL || uv_plane == NULL || rgb_out == NULL) {
		return;
	}
	for (unsigned y = 0; y < height; y++) {
		const uint8_t *y_row = y_plane + y * y_stride;
		const uint8_t *uv_row = uv_plane + (y / 2) * uv_stride;
		uint32_t *out_row = rgb_out + y * (rgb_stride / sizeof(uint32_t));

		for (unsigned x = 0; x < width; x++) {
			int y_val = y_row[x];
			int cb_val = uv_row[(x / 2) * 2];
			int cr_val = uv_row[(x / 2) * 2 + 1];

			int r, g, b;
			if (full_range) {
				int c = y_val;
				int d = cb_val - 128;
				int e = cr_val - 128;
				r = (c * 256 + 359 * e) / 256;
				g = (c * 256 - 88 * d - 183 * e) / 256;
				b = (c * 256 + 454 * d) / 256;
			} else {
				int c = y_val - 16;
				int d = cb_val - 128;
				int e = cr_val - 128;
				r = (298 * c + 409 * e + 128) / 256;
				g = (298 * c - 100 * d - 208 * e + 128) / 256;
				b = (298 * c + 516 * d + 128) / 256;
			}
			r = r < 0 ? 0 : (r > 255 ? 255 : r);
			g = g < 0 ? 0 : (g > 255 ? 255 : g);
			b = b < 0 ? 0 : (b > 255 ? 255 : b);

			/* 0xAARRGGBB in memory (BGRA order on little-endian) */
			out_row[x] = (uint32_t)(0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
		}
	}
}

/*
 * Enable SWVPDecodersAlwaysEnabled for VP9 software decoding in WebKit.
 * Hooks -[WKPreferences init] to enable internal debug feature for libvpx VP9 decoding,
 * registers WebKit VP9 decoder if libwebrtc is loaded, and configures defaults.
 */
static IMP orig_wkpref_init;
static int vp9_hook_installed;

static id
rwd_swizzled_wkpref_init(id self, SEL _cmd)
{
	self = ((id (*)(id, SEL))orig_wkpref_init)(self, _cmd);
	if (self != nil) {
		Class cls = object_getClass(self);
		SEL featuresSel = sel_registerName("_internalDebugFeatures");
		if ([cls respondsToSelector:featuresSel]) {
			NSArray *features = ((NSArray * (*)(id, SEL))objc_msgSend)(cls, featuresSel);
			if (features != nil && [features isKindOfClass:[NSArray class]]) {
				SEL keySel = sel_registerName("key");
				SEL setEnabledSel = sel_registerName("_setEnabled:forInternalDebugFeature:");
				for (id feature in features) {
					if ([feature respondsToSelector:keySel]) {
						NSString *key = ((NSString * (*)(id, SEL))objc_msgSend)(feature, keySel);
						if ([key isEqualToString:@"SWVPDecodersAlwaysEnabled"]) {
							if ([self respondsToSelector:setEnabledSel]) {
								((void (*)(id, SEL, BOOL, id))objc_msgSend)(self, setEnabledSel, YES, feature);
								printf("redwooddrawable: enabled SWVPDecodersAlwaysEnabled on WKPreferences %p\n", (void *)self);
							}
							break;
						}
					}
				}
			}
		}
	}
	return self;
}

void
rwd_enable_webkit_vp9(void)
{
	if (vp9_hook_installed) {
		return;
	}
	Class wkPrefClass = objc_getClass("WKPreferences");
	if (wkPrefClass != Nil) {
		SEL initSel = sel_registerName("init");
		Method m = class_getInstanceMethod(wkPrefClass, initSel);
		if (m != NULL) {
			orig_wkpref_init = method_getImplementation(m);
			method_setImplementation(m, (IMP)rwd_swizzled_wkpref_init);
			vp9_hook_installed = 1;
			printf("redwooddrawable: installed WKPreferences init hook for SWVPDecodersAlwaysEnabled\n");
		}
	}
	/* Also invoke libwebrtc registerWebKitVP9Decoder directly if loaded in this process */
	void (*reg_vp9)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "_ZN6webrtc26registerWebKitVP9DecoderEv");
	if (reg_vp9 != NULL) {
		reg_vp9();
		printf("redwooddrawable: registered libwebrtc VP9 decoder\n");
	}
	/* Set user defaults keys */
	NSUserDefaults *defs = [NSUserDefaults standardUserDefaults];
	if (defs != nil) {
		[defs setBool:YES forKey:@"SWVPDecodersAlwaysEnabled"];
		[defs setBool:YES forKey:@"WebKitSWVPDecodersAlwaysEnabled"];
	}
}

static void
rwd_dyld_image_added(const struct mach_header *mh, intptr_t vmaddr_slide)
{
	(void)mh;
	(void)vmaddr_slide;
	if (!vp9_hook_installed) {
		rwd_enable_webkit_vp9();
	}
}

__attribute__((constructor))
static void
rwd_module_init(void)
{
	rwd_enable_webkit_vp9();
	_dyld_register_func_for_add_image(rwd_dyld_image_added);
}
