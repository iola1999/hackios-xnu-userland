/* Texture format, shape validation, allocation and explicit linear import. */
static enum pipe_format
texture_format(const struct rwa_texture *t)
{
   return t->view_format != PIPE_FORMAT_NONE ? t->view_format : t->res->format;
}

/* Level L of the texture is level first_level + L of its resource. */
static unsigned
texture_levels(const struct rwa_texture *t)
{
   return t->num_levels ? t->num_levels : t->res->last_level + 1;
}

/* A caller's level of the texture as a level of its resource, refused past the texture's
 * last level. */
static bool
resource_level(const struct rwa_texture *t, unsigned *level, char *why, size_t why_size)
{
   if (t->chroma) {
      say(why, why_size, "combined NV12 byte access is unsupported; use individual planes");
      return false;
   }
   if (*level >= texture_levels(t)) {
      say(why, why_size, "level %u of a %s with %u", *level, t->format_name, texture_levels(t));
      return false;
   }
   *level += t->first_level;
   return true;
}

/* What a format this library declines is, so the refusal can say it instead of
 * printing a number. The ranges are Metal's own enumeration, grouped the way Metal
 * groups them, and each carries the reason it is not in the table above -- the reason
 * differs per family, and "unsupported" would hide which of them it is.
 */
static const char *
pixel_format_refusal(unsigned f)
{
   if (f == 0)
      return "MTLPixelFormatInvalid, which is what a descriptor nobody filled in holds";
   if (f >= 40 && f <= 43)
      return "one of Metal's 16-bit packed colour formats (B5G6R5, A1BGR5, ABGR4, BGR5A1), "
             "which this library serves by default and RWA_PACKED16=0 has taken out of the "
             "picture. Declining them costs the home screen its icons, so this is a "
             "measurement switch and not a setting";
   if (f == 44)
      return "MTLPixelFormatBGRA4Unorm, which Metal.framework's own enumeration carries at "
             "44 and this device's MTLPixelFormat.h does not declare at all: a format no "
             "public header names is one whose layout nothing here can read";
   if (f >= 160 && f <= 167)
      return "one of the PVRTC compressed formats, whose texels come in blocks this library "
             "cannot make";
   if (f >= 170 && f <= 183)
      return "one of the EAC or ETC2 compressed formats, whose texels come in blocks this "
             "library cannot make";
   if (f >= 222 && f <= 236)
      return "one of the ASTC HDR formats; this library serves the LDR and sRGB ASTC formats only";
   if (f == 240 || f == 241)
      return "one of Metal's packed 4:2:2 video formats (GBGR422, BGRG422), where one block "
             "holds two texels and a region in texels is not a region in bytes";
   /* The four depth and stencil formats iOS has are in the table above; these are the
    * rest of Metal's enumeration between them, and each is declined for its own reason
    * rather than for being a depth format.
    */
   if (f == 255)
      return "MTLPixelFormatDepth24Unorm_Stencil8, which Metal's own header marks "
             "API_UNAVAILABLE(ios) -- this device answers NO to "
             "-isDepth24Stencil8PixelFormatSupported, and a format the device says it "
             "does not have must not be allocatable through it";
   if (f == 261 || f == 262)
      return "one of the X*_Stencil8 formats, which is not a texture but a view of the "
             "stencil aspect of one made as Depth32Float_Stencil8: it comes from "
             "-newTextureViewWithPixelFormat:, which this library does not do, and "
             "rwa_depth_read asks for an aspect of the texture itself instead";
   if (f >= 250 && f <= 262)
      return "a depth or stencil format of Metal's enumeration that this library has no "
             "Gallium equivalent for by name";
   if (f >= 552 && f <= 555)
      return "one of Apple's extended-range XR formats, which has no Gallium equivalent at "
             "all";
   return "not a value of MTLPixelFormat this library knows: either a format newer than the "
          "iOS 16.5 SDK's or none at all";
}

/* Where an imported texture's memory comes from: an IOSurface by its ID, or (kms_handle) a buffer
 * object of this device, at an offset and row length. NULL below means "allocate". */
struct rwa_surface_import {
   int id;
   unsigned offset, row_bytes, kms_handle;
};

static struct rwa_texture *
texture_of_shape(struct rwa_device *device, const struct rwa_texture_shape *shape,
                 const struct rwa_surface_import *import, char *why, size_t why_size)
{
   if (!shape) {
      say(why, why_size, "a texture with no descriptor");
      return NULL;
   }

   if (rgb2p_format(shape->pixel_format)) {
      say(why, why_size, "RGB2P requires both explicit IOSurface planes");
      return NULL;
   }
   const struct rwa_pixel_format *fmt = pixel_format(shape->pixel_format);
   if (!fmt) {
      say(why, why_size, "pixel format %u is %s", shape->pixel_format,
          pixel_format_refusal(shape->pixel_format));
      return NULL;
   }

   /* Metal's descriptor defaults these to 1 and never to 0, so a zero is a caller's
    * struct that was filled in only as far as it cared about. Taken as one rather than
    * refused: a texture with no texels at all is still refused below, on the width,
    * which is the field no descriptor leaves out.
    */
   unsigned width = shape->width;
   unsigned height = shape->height ? shape->height : 1;
   unsigned depth = shape->depth ? shape->depth : 1;
   unsigned layers = shape->array_length ? shape->array_length : 1;
   unsigned mips = shape->mip_levels ? shape->mip_levels : 1;
   unsigned samples = shape->sample_count ? shape->sample_count : 1;

   const char *type_name = texture_type_name(shape->type);
   if (!type_name) {
      say(why, why_size, "texture type %u is not one of MTLTextureType's ten", shape->type);
      return NULL;
   }
   /* Multisample is a property of the TYPE in Metal and of the sample count in Gallium,
    * and the two have to be made to agree in both directions (below, and in
    * attachment_shape_fits for a pass). This is the Metal side of that.
    */
   bool multisample = shape->type == RWA_TYPE_2D_MULTISAMPLE ||
                      shape->type == RWA_TYPE_2D_MULTISAMPLE_ARRAY;

   /*
    * What an imported surface can be, and every one of these is a refusal rather than a
    * silent adjustment. The surface is row-major memory somebody else laid out, and all
    * freedreno is told about that layout is one pitch (fd_resource_from_handle sets
    * layout.pitch0 = handle->stride and slice 0's offset, and nothing else). So a mip
    * chain, an array, a cube or a second sample would need this side and the allocator to
    * have agreed on where the extra levels live -- and no such agreement exists. A
    * drawable is none of those things: -[CAMetalLayer nextDrawable] asks for a plain 2D
    * texture, mipmapped: NO, one slice (docs/metal.md 10.46).
    */
   if (import) {
      if (shape->type != RWA_TYPE_2D) {
         say(why, why_size, "a %s over an IOSurface: an imported surface is row-major "
                            "memory with one pitch, and only a MTLTextureType2D can be laid "
                            "over it without an agreement about where further levels live",
             type_name);
         return NULL;
      }
      if (mips != 1) {
         say(why, why_size, "%u mip levels over an IOSurface: the import is told one pitch "
                            "and nothing about smaller levels, so there is nowhere for them "
                            "to be", mips);
         return NULL;
      }
      if (layers != 1 || depth != 1) {
         say(why, why_size, "%u slices and a depth of %u over an IOSurface: the import is "
                            "one plane of one image", layers, depth);
         return NULL;
      }
      if (import->row_bytes == 0) {
         say(why, why_size, "an IOSurface with no bytes a row: the import takes the pitch "
                            "from the caller and cannot guess it");
         return NULL;
      }
   }

   enum pipe_texture_target target;
   unsigned array_size = 1;
   switch (shape->type) {
   case RWA_TYPE_1D:
   case RWA_TYPE_1D_ARRAY:
      if (height != 1 || depth != 1) {
         say(why, why_size, "a %s of %ux%ux%u: a 1D texture has one row and one slice",
             type_name, width, height, depth);
         return NULL;
      }
      target = shape->type == RWA_TYPE_1D ? PIPE_TEXTURE_1D : PIPE_TEXTURE_1D_ARRAY;
      array_size = shape->type == RWA_TYPE_1D ? 1 : layers;
      break;
   case RWA_TYPE_2D:
   case RWA_TYPE_2D_ARRAY:
      if (depth != 1) {
         say(why, why_size, "a %s with a depth of %u: a 2D texture has one slice, and an "
                            "array of them is arrayLength and not depth", type_name, depth);
         return NULL;
      }
      target = shape->type == RWA_TYPE_2D ? PIPE_TEXTURE_2D : PIPE_TEXTURE_2D_ARRAY;
      array_size = shape->type == RWA_TYPE_2D ? 1 : layers;
      break;
   case RWA_TYPE_CUBE:
   case RWA_TYPE_CUBE_ARRAY:
      if (width != height || depth != 1) {
         say(why, why_size, "a %s of %ux%ux%u: a cube's faces are square", type_name, width,
             height, depth);
         return NULL;
      }
      /* Gallium counts the six faces in array_size, so a cube array is six slices per
       * cube. Metal counts them in the type instead, which is why this is a
       * translation and not a copy.
       */
      target = shape->type == RWA_TYPE_CUBE ? PIPE_TEXTURE_CUBE : PIPE_TEXTURE_CUBE_ARRAY;
      array_size = 6 * (shape->type == RWA_TYPE_CUBE ? 1 : layers);
      break;
   case RWA_TYPE_3D:
      if (shape->array_length > 1) {
         say(why, why_size, "a %s with an arrayLength of %u: Metal has no array of 3D "
                            "textures", type_name, shape->array_length);
         return NULL;
      }
      target = PIPE_TEXTURE_3D;
      break;
   case RWA_TYPE_2D_MULTISAMPLE:
   case RWA_TYPE_2D_MULTISAMPLE_ARRAY:
      /*
       * Gallium has no multisample TARGET. A multisample texture is PIPE_TEXTURE_2D (or
       * _2D_ARRAY) with nr_samples above one, and every per-sample thing hangs off that
       * one number: freedreno multiplies the layout's bytes per texel by it
       * (fd_resource_layout_init, "layout->cpp *= fd_resource_nr_samples"), and a
       * framebuffer's sample count is read back off the attachment's resource rather
       * than taken from the framebuffer state (util_framebuffer_get_num_samples). So
       * the target is a plain 2D texture's and the sample count below is the whole of
       * the difference.
       */
      if (depth != 1) {
         say(why, why_size, "a %s with a depth of %u: a 2D texture has one slice, and an "
                            "array of them is arrayLength and not depth", type_name, depth);
         return NULL;
      }
      target = shape->type == RWA_TYPE_2D_MULTISAMPLE ? PIPE_TEXTURE_2D
                                                      : PIPE_TEXTURE_2D_ARRAY;
      array_size = shape->type == RWA_TYPE_2D_MULTISAMPLE ? 1 : layers;
      break;
   case RWA_TYPE_TEXTURE_BUFFER:
      /* The frontend refuses a texture buffer by name as well (docs/air.md section 5);
       * it is a buffer with a texture's interface, and rwa_buffer_create is where one
       * comes from.
       */
      say(why, why_size, "a %s: this is a buffer behind a texture's interface, which the "
                         "frontend refuses on the shader side too", type_name);
      return NULL;
   default:
      UNREACHABLE("texture_type_name admitted a type this switch does not");
   }

   if (width == 0) {
      say(why, why_size, "a %s that is no texels wide", type_name);
      return NULL;
   }
   /*
    * The sample count, by Metal's own rules about it -- and these are quoted rather than
    * deduced. The sentences are in Metal itself, in the __cstring of
    * /System/Library/Frameworks/Metal.framework/Metal (0x181b6915f and 0x181b690c5 in
    * this device's shared cache), not in MetalTools: they are what every device enforces
    * and not what the validation layer adds.
    *
    *   "MTLTextureDescriptor sampleCount must be > 1 for multisample textures."
    *   "MTLTextureDescriptor mipmapLevelCount must equal 1 for multisample textures."
    *
    * A one-sample multisample texture is the tempting thing to allow, because Gallium
    * would make one happily -- nr_samples 0 and 1 are the same resource -- and it would
    * let the six multisample fragment shaders of AppleMetalGLRenderer run with their
    * loop over samples executing once. It is refused because that is exactly a pass for
    * the wrong reason: with one sample the sample index can only ever be 0, so a shader
    * reading sample i and a shader ignoring i entirely print the same pixel, and the
    * descriptor is one Metal would have refused.
    */
   if (!multisample && samples != 1) {
      say(why, why_size, "a %s with a sampleCount of %u: only a multisample type has more "
                         "than one sample per texel", type_name, samples);
      return NULL;
   }
   if (multisample && samples < 2) {
      say(why, why_size, "a %s with a sampleCount of %u: Metal's own rule is "
                         "\"MTLTextureDescriptor sampleCount must be > 1 for multisample "
                         "textures\", and a texture of one sample is a MTLTextureType2D",
          type_name, shape->sample_count);
      return NULL;
   }
   if (multisample && mips != 1) {
      say(why, why_size, "a %s with a mipmapLevelCount of %u: Metal's own rule is "
                         "\"MTLTextureDescriptor mipmapLevelCount must equal 1 for "
                         "multisample textures\", and there is no filtered fetch from a "
                         "multisample texture for a smaller level to serve", type_name,
          mips);
      return NULL;
   }

   /* A mip chain no larger than the texture: with more levels than halvings the extra
    * ones are 1x1 of nothing, and a shader asking for level 7 of a 4x4 texture is a
    * shader that believes something else about the image it was given.
    */
   unsigned halvings = 1;
   for (unsigned e = MAX3(width, height, depth); e > 1; e >>= 1)
      halvings++;
   if (mips > halvings) {
      say(why, why_size, "%u mip levels of a %ux%ux%u texture, which has %u", mips, width,
          height, depth, halvings);
      return NULL;
   }

   /* MTLTextureUsage, which decides one thing here: whether the GPU is told this
    * texture can be stored to. MTLTextureUsageUnknown (zero) is Metal's "assume
    * anything", so it means both; a usage with neither read nor write in it
    * (PixelFormatView alone) still has to be allocatable and readable back, so it is
    * given the sampled binding.
    */
   unsigned known = RWA_USAGE_SHADER_READ | RWA_USAGE_SHADER_WRITE |
                    RWA_USAGE_RENDER_TARGET | RWA_USAGE_PIXEL_FORMAT_VIEW;
   if (shape->usage & ~known) {
      say(why, why_size, "usage bits 0x%x are not MTLTextureUsage's, and this library will "
                         "not guess what they allow", shape->usage & ~known);
      return NULL;
   }
   bool read = shape->usage == 0 || (shape->usage & RWA_USAGE_SHADER_READ);
   bool write = shape->usage == 0 || (shape->usage & RWA_USAGE_SHADER_WRITE);

   /* A depth or stencil format turns the same usage bits into a different binding, and
    * the difference is not a convention: Gallium has PIPE_BIND_DEPTH_STENCIL where it
    * has PIPE_BIND_RENDER_TARGET for colour, and freedreno answers for a Z format only
    * when asked about that one (a6xx/fd6_screen.cc hands the format to fd6_pipe2depth
    * under PIPE_BIND_DEPTH_STENCIL and nothing else). Asked as a render target, every
    * depth format would be declined with a sentence about the GPU that is really about
    * the question.
    *
    * MTLTextureUsageShaderWrite on one is refused rather than dropped: Metal has no
    * depth format a shader stores to and Gallium has no image binding for one, so the
    * texture could only be bound where it would not be written -- which is the shape of
    * failure this library exists to avoid. With usage 0, Metal's "assume anything", the
    * write is simply not assumed.
    */
   /* A multisample texture is the other kind a shader cannot store to, and both sides
    * say so. Metal's MSL has no writable multisample texture at all -- texture2d_ms
    * takes access::read and nothing else -- and freedreno refuses
    * PIPE_BIND_SHADER_IMAGE outright for any sample count above zero
    * (a6xx/fd6_screen.cc, "if (sample_count > 0) return false"), which is the check
    * that once held eighteen kernels on the wrong question (docs/metal.md 10.21). So an
    * explicit MTLTextureUsageShaderWrite is refused by name, and usage 0 -- Metal's
    * "assume anything" -- simply does not assume the write.
    */
   if (multisample && (shape->usage & RWA_USAGE_SHADER_WRITE)) {
      say(why, why_size, "a %s with MTLTextureUsageShaderWrite: Metal has no multisample "
                         "texture a shader stores to (texture2d_ms takes access::read and "
                         "nothing else) and neither has this GPU, which declines an image "
                         "binding for any sample count above one, so there is no binding "
                         "this could be given", type_name);
      return NULL;
   }
   if (multisample)
      write = false;

   bool depth_stencil = format_is_depth_stencil(fmt->pipe);
   if (depth_stencil && (shape->usage & RWA_USAGE_SHADER_WRITE)) {
      say(why, why_size, "a %s of %s with MTLTextureUsageShaderWrite: no shader stores to "
                         "a depth or stencil texture, in Metal or on this GPU, so there is "
                         "no binding this could be given", type_name, fmt->name);
      return NULL;
   }
   if (depth_stencil)
      write = false;

   /* A compressed texture is written by the processor or by a blit from a buffer and only
    * sampled: freedreno has no image or render target binding for a block format. */
   bool compressed = util_format_is_compressed(fmt->pipe);
   if (compressed && (shape->usage & (RWA_USAGE_SHADER_WRITE | RWA_USAGE_RENDER_TARGET))) {
      say(why, why_size, "a %s of %s with MTLTextureUsageShaderWrite or RenderTarget: a "
                         "compressed format can only be sampled", type_name, fmt->name);
      return NULL;
   }
   if (compressed)
      write = false;

   unsigned bind = (read ? PIPE_BIND_SAMPLER_VIEW : 0) |
                   (write ? PIPE_BIND_SHADER_IMAGE : 0) |
                   ((shape->usage & RWA_USAGE_RENDER_TARGET) || (depth_stencil && !shape->usage)
                       ? (depth_stencil ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET)
                       : 0);
   if (!bind) {
      bind = PIPE_BIND_SAMPLER_VIEW;
      read = true;
   }

   /* Gallium counts a texture that is not multisampled as zero samples, not one,
    * everywhere a sample count is asked for -- and the difference is not cosmetic here.
    * freedreno's fd6_screen_is_format_supported refuses PIPE_BIND_SHADER_IMAGE outright
    * when the sample count is above zero (a6xx/fd6_screen.cc: "if (sample_count > 0)
    * return false"), so asking with one -- which is what `samples` holds for every
    * texture this far, the multisample types having been refused above -- made the
    * Adreno decline RGBA32Uint, RGBA32Float, R32*, RGBA8Unorm and every other format as
    * a texture the shader writes. That was measured on the device: five candidates, all
    * five refused with this file's own sentence, and no image kernel could be served.
    * air_run never meets it because it never asks is_format_supported and creates its
    * resources with nr_samples left at zero (air_run.c, image_shapes). So a
    * non-multisample texture is zero samples to both the query and resource_create,
    * and a multisample one is its own count to both.
    */
   unsigned query_samples = samples > 1 ? samples : 0;

   /* Asked before it is allocated, so that a format the driver does not do for this
    * binding is a sentence naming both rather than a NULL from resource_create naming
    * neither. WHICH sample counts exist is the back end's answer and not a list here:
    * freedreno's a6xx takes 1, 2 and 4 and declines 8 (a6xx/fd6_screen.cc,
    * valid_sample_count, where 8 is commented out because it moves the LRZ width),
    * while softpipe declines every count above one for every format and binding there
    * is (softpipe/sp_screen.c: "if (sample_count > 1) return false", and
    * caps->fake_sw_msaa, which asks the frontend to pretend with one sample).
    */
   if (!device->screen->is_format_supported(device->screen, fmt->pipe, target,
                                            query_samples, query_samples, bind)) {
      char ms[48] = "";
      if (query_samples)
         snprintf(ms, sizeof(ms), " of %u samples per texel", query_samples);
      say(why, why_size, "%s cannot give %s as a %s%s%s%s", rwa_device_name(device),
          fmt->name, type_name, ms,
          write ? " the shader writes" : " the shader samples",
          !(bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL)) ? ""
             : depth_stencil ? " and tests depth in" : " and renders to");
      return NULL;
   }

   struct rwa_texture *texture = calloc(1, sizeof(*texture));
   if (!texture) {
      say(why, why_size, "out of memory making a texture");
      return NULL;
   }
   texture->device = device;
   texture->references = 1;
   texture->shape = *shape;
   texture->format_name = fmt->name;
   texture->layers = array_size;
   texture->shader_read = read;
   texture->shader_write = write;

   struct pipe_resource templ = {
      .target = target, .format = fmt->pipe,
      .width0 = width, .height0 = height, .depth0 = depth,
      .array_size = array_size, .last_level = mips - 1, .nr_samples = query_samples,
      .bind = bind, .usage = PIPE_USAGE_DEFAULT };

   if (import) {
      /*
       * Memory somebody else allocated, by the one route this port has: the ID goes where
       * Linux puts a dmabuf file descriptor, and freedreno's import on Darwin already
       * means exactly that (freedreno_bo.c, fd_bo_from_dmabuf_drm: "what comes in here is
       * an IOSurface, and the file descriptor its ID", and the kernel answers it as
       * kAnnibaleSurface). PIPE_BIND_SHARED is deliberately NOT added to the template:
       * fd_resource_from_handle sets is_shared on the resource itself, which is the flag
       * that matters, and a bind bit the is_format_supported query above was not made with
       * would be a question asked twice and answered once.
       *
       * MiniIOSurface exposes linear storage without UBWC metadata. Its global
       * 64-byte row alignment can be smaller than freedreno's format-specific pitch.
       */
      struct winsys_handle whandle = {
         .type = import->kms_handle ? WINSYS_HANDLE_TYPE_KMS : WINSYS_HANDLE_TYPE_FD,
         .handle = import->kms_handle ? import->kms_handle : (unsigned) import->id,
         .stride = import->row_bytes,
         .offset = import->offset,
         .modifier = DRM_FORMAT_MOD_LINEAR,
      };

      /* fdl6_layout_image's explicit linear layout aligns power-of-two texels
       * to max(64 bytes, 16 pixels). Three-component formats use the next
       * power-of-two texel size times 64. Keep this check paired with the
       * actual fdl layout tests; MiniIOSurface's global row mask is insufficient.
       * tools/mesa/fdl6-linear-import-pitch.patch lowers that for the two cases
       * it serves, the same way fd6_resource.cc does: an import only sampled needs a
       * whole number of texels (iconservicesagent's RGBA16Float icon surfaces have
       * rows of 992 and 1248 bytes), and a colour target 64 bytes, which freedreno
       * then renders in sysmem (SpringBoard renders into a 120-texel RGBA16Float
       * surface of 960 bytes a row).
       */
      unsigned texel = util_format_get_blocksize(fmt->pipe);
      bool sample_only = !(bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL |
                                   PIPE_BIND_SHADER_IMAGE));
      bool colour_target = !sample_only &&
                           !(bind & (PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_SHADER_IMAGE));
      unsigned pitch_align = sample_only ? texel
         : colour_target ? 64
         : util_is_power_of_two_or_zero(texel) ? MAX2(64, 16 * texel)
         : 64 * util_next_power_of_two(texel);
      if (import->row_bytes < width * texel) {
         say(why, why_size, "IOSurface %d has %u bytes a row, and %u texels of %u bytes "
                            "need at least %u: this GPU's import refuses a pitch narrower "
                            "than the image", import->id, import->row_bytes, width, texel,
             width * texel);
         free(texture);
         return NULL;
      }
      if (import->row_bytes % pitch_align != 0) {
         say(why, why_size, "IOSurface %d has %u bytes a row for %s (%u bytes per texel); "
                            "freedreno explicit linear import requires a multiple of %u "
                            "bytes (fdl6_layout_image)", import->id, import->row_bytes,
             fmt->name, texel, pitch_align);
         free(texture);
         return NULL;
      }

      /* null_sw_winsys has no external-memory import. Validate the supplied
       * layout first so host tests can distinguish malformed pitches.
       */
      if (device->software || !device->screen->resource_from_handle) {
         say(why, why_size, "%s imports no memory: IOSurface %d, %ux%u %s of %s, "
                            "%u bytes a row; pitch checks passed, but this backend "
                            "cannot import external storage", rwa_device_name(device),
             import->id, width, height, type_name, fmt->name, import->row_bytes);
         free(texture);
         return NULL;
      }

      texture->res = device->screen->resource_from_handle(device->screen, &templ, &whandle,
                                                          PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
      if (!texture->res) {
         say(why, why_size, "%s resource_from_handle failed for IOSurface %d: %ux%u %s "
                            "of %s, row bytes %u, offset %u, linear pitch alignment %u. "
                            "The driver returned no detailed failure reason; inspect "
                            "its layout, buffer import and kernel logs",
             rwa_device_name(device), import->id, width, height, type_name, fmt->name,
             import->row_bytes, import->offset, pitch_align);
         free(texture);
         return NULL;
      }
      return texture;
   }

   texture->res = device->screen->resource_create(device->screen, &templ);
   if (!texture->res) {
      say(why, why_size, "the GPU driver would not allocate a %ux%ux%u %s of %s. After a few "
                         "dozen allocations in one boot this driver starts refusing outright "
                         "(docs/air.md section 4)", width, height, depth, type_name,
          fmt->name);
      free(texture);
      return NULL;
   }
   return texture_start_zeroed(device, texture);
}

struct rwa_texture *
rwa_texture_create(struct rwa_device *device, const struct rwa_texture_shape *shape,
                   char *why, size_t why_size)
{
   RWA_LOCKED;
   return texture_of_shape(device, shape, NULL, why, why_size);
}

struct rwa_texture *
rwa_texture_from_surface(struct rwa_device *device, const struct rwa_texture_shape *shape,
                         int surface_id, unsigned offset, unsigned row_bytes, char *why,
                         size_t why_size)
{
   RWA_LOCKED;
   /*
    * A surface ID of zero is not a surface. IOSurfaceGetID answers 0 for one that was
    * never made shareable, and the kernel's table has nothing at 0 either, so this would
    * reach freedreno as an import of nothing and come back as the pitch sentence above --
    * which would name the wrong thing.
    */
   if (surface_id <= 0) {
      say(why, why_size, "IOSurface %d is not a surface: an ID of zero is what a surface "
                         "that was never made shareable answers, and there is nothing for "
                         "the kernel to look up", surface_id);
      return NULL;
   }
   const struct rwa_surface_import import = {
      .id = surface_id, .offset = offset, .row_bytes = row_bytes };

   return texture_of_shape(device, shape, &import, why, why_size);
}
