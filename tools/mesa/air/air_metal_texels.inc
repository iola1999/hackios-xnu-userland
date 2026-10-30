/*
 * Texels between a texture and the caller's memory, included into air_metal.c: the region a
 * call names (texture_box), the caller's row stride, and the writes and reads through a
 * processor mapping (rwa_texture_write, rwa_texture_read, rwa_depth_read).
 */

/*
 * A region of a texture, as Gallium wants it, and everything that will not fit in one
 * refused.
 *
 * Two conventions meet here and only one of them is the obvious one: a slice of a 2D
 * array or a face of a cube goes in box.z, and a slice of a 1D ARRAY goes in box.y.
 * softpipe asserts exactly that (sp_texture.c, softpipe_transfer_map), so a 1D array
 * written through z lands on the wrong slice -- and in a release build, where that
 * assertion is compiled out, it lands there quietly.
 */
static bool
texture_box(const struct rwa_texture *texture, unsigned level, unsigned layer,
            const unsigned origin[3], const unsigned size[3], struct pipe_box *box,
            char *why, size_t why_size)
{
   const struct pipe_resource *res = texture->res;
   /* origin and size may both be left out, which is the whole level: a caller filling
    * a texture it has just made has nothing else to say, and -replaceRegion: with the
    * texture's own region is what it would write.
    */
   unsigned o[3] = { origin ? origin[0] : 0, origin ? origin[1] : 0, origin ? origin[2] : 0 };

   if (level > res->last_level) {
      say(why, why_size, "level %u of a texture with %u", level, res->last_level + 1);
      return false;
   }

   bool volume = res->target == PIPE_TEXTURE_3D;
   bool rows = res->target != PIPE_TEXTURE_1D && res->target != PIPE_TEXTURE_1D_ARRAY;
   unsigned w = u_minify(res->width0, level);
   unsigned h = rows ? u_minify(res->height0, level) : 1;
   unsigned d = volume ? u_minify(res->depth0, level) : 1;
   unsigned s[3] = { size ? size[0] : w, size ? size[1] : h, size ? size[2] : d };

   if (layer >= texture->layers) {
      say(why, why_size, "slice %u of a texture with %u", layer, texture->layers);
      return false;
   }
   if (!s[0] || !s[1] || !s[2]) {
      write_zero_region(texture, s[0], s[1], s[2]);
      say(why, why_size, "a region of %ux%ux%u texels has no texels in it", s[0], s[1], s[2]);
      return false;
   }
   if (o[0] + s[0] > w || o[1] + s[1] > h || o[2] + s[2] > d) {
      say(why, why_size, "a region of %ux%ux%u texels at (%u, %u, %u) of a level that is "
                         "%ux%ux%u", s[0], s[1], s[2], o[0], o[1], o[2], w, h, d);
      return false;
   }

   *box = (struct pipe_box){ .x = o[0], .width = s[0] };
   if (!rows) {
      box->y = layer;
      box->height = 1;
      box->z = 0;
      box->depth = 1;
   } else {
      box->y = o[1];
      box->height = s[1];
      box->z = volume ? o[2] : layer;
      box->depth = volume ? s[2] : 1;
   }
   return true;
}

/* How wide a row of the caller's memory is, with Metal's convention that zero means
 * tightly packed, and the check that it is at least a row: a stride shorter than the
 * region would make every row after the first read from inside the one before it,
 * which produces an image rather than an error.
 */
static bool
caller_stride(const struct rwa_texture *texture, const struct pipe_box *box, size_t stride,
              size_t *out, char *why, size_t why_size)
{
   unsigned texel = util_format_get_blocksize(texture->res->format);
   size_t least = (size_t)box->width * texel;
   if (stride == 0) {
      *out = least;
      return true;
   }
   if (stride < least) {
      say(why, why_size, "%zu bytes per row for %u texels of %s, which need %zu", stride,
          box->width, texture->format_name, least);
      return false;
   }
   *out = stride;
   return true;
}

int
rwa_texture_write(struct rwa_texture *texture, unsigned level, unsigned layer,
                  const unsigned origin[3], const unsigned size[3], const void *bytes,
                  size_t stride, char *why, size_t why_size)
{
   RWA_LOCKED;
   struct pipe_box box;
   size_t row;

   /* First statement, before the argument checks: a call that arrives with garbage in `bytes` is one
    * of the two answers the rings exist for (see the rings above rwa_device_open).
    */
   rwa_write_record_entry(texture, level, layer, bytes, stride, size);

   /* Round air-d: which of the two upstreams this write came from, printed only when `bytes`
    * cannot be an address. The driver's -replaceRegion: is another image; the buffer-backed
    * upload path is inside this one.
    */
   if (bytes && ((uintptr_t)bytes >> 48)) {
      void rwa_texture_write_caller(const void *bytes, const void *ret, const void *self);

      rwa_texture_write_caller(bytes, (const void *)__builtin_return_address(0),
                               (const void *)&rwa_texture_write);
   }

   if (!bytes) {
      say(why, why_size, "no bytes to write into a texture");
      return -1;
   }
   /* A depth or stencil texture is refused here and read through rwa_depth_read, and
    * the two go together on purpose: a texel of Depth32Float_Stencil8 holds two values
    * of two kinds, so a call whose only argument is "bytes" cannot say which of them
    * the caller means. Nothing in this path needs to write one -- a depth attachment is
    * filled by the pass's clear and by the draw -- so this declines rather than
    * inventing an aspect argument no caller has asked for.
    */
   if (format_is_depth_stencil(texture->res->format)) {
      say(why, why_size, "writing texels into a %s texture: a depth or stencil texel has "
                         "an aspect and this call has nowhere to name it. A depth "
                         "attachment is filled by the render pass's clearDepth and by what "
                         "the draw writes", texture->format_name);
      return -1;
   }
   /*
    * And a multisample texture is refused for a harder reason than an argument the call
    * lacks: there is no way to write one from the processor at all, on either side.
    *
    * Metal forbids it twice. A multisample texture cannot be MTLStorageModeShared on
    * this device -- Metal's own "MTLTextureDescriptor: Multisample textures cannot be
    * allocated with MTLStorageModeShared on this device" -- and -replaceRegion: writes
    * only a texture the processor can see, so the one call that would do it cannot be
    * given a texture to do it to.
    *
    * Gallium cannot either. freedreno lays a multisample texel out as nr_samples texels'
    * worth of bytes (fd_resource_layout_init: "layout->cpp *= fd_resource_nr_samples")
    * in an order that is the hardware's, so bytes copied through a map would land in no
    * defined sample -- and they would land SILENTLY, which is the shape of failure this
    * library exists to avoid.
    *
    * What fills a multisample texture is a render pass into it, which is also the only
    * thing that fills one in Metal: rwa_render_create with this sample count and
    * rwa_render_draw with this texture as an attachment.
    */
   if (texture->res->nr_samples > 1) {
      say(why, why_size, "writing texels into a %s of %u samples per texel: Metal writes "
                         "no multisample texture from the processor (one cannot be "
                         "MTLStorageModeShared on this device, and -replaceRegion: writes "
                         "only what the processor can see) and this GPU lays the samples of "
                         "a texel out in an order of its own, so these bytes would land in "
                         "no defined sample. A multisample texture is filled by rendering "
                         "into it", texture->format_name, texture->res->nr_samples);
      return -1;
   }
   if (!resource_level(texture, &level, why, why_size) ||
       !texture_box(texture, level, layer, origin, size, &box, why, why_size) ||
       !caller_stride(texture, &box, stride, &row, why, why_size))
      return -1;

   /* The slice stride is the region's own rows, because the contract has one stride: a
    * caller handing over a 3D region hands over a packed stack of its rows, which is
    * what -replaceRegion:...bytesPerRow:bytesPerImage: is given when bytesPerImage is
    * the obvious product.
    */
   {
      void rwa_texture_write_guard(const void *bytes, const void *res);

      if (bytes && ((uintptr_t)bytes >> 48))
         rwa_texture_write_guard(bytes, texture->res);
   }

   texture->device->pipe->texture_subdata(texture->device->pipe, texture->res, level,
                                          PIPE_MAP_WRITE, &box, bytes, row,
                                          row * box.height);
   texture_written(texture);
   return 0;
}

int
rwa_texture_read(struct rwa_texture *texture, unsigned level, unsigned layer,
                 const unsigned origin[3], const unsigned size[3], void *bytes,
                 size_t stride, char *why, size_t why_size)
{
   RWA_LOCKED;
   struct pipe_context *pipe = texture->device->pipe;
   struct pipe_box box;
   size_t row;

   if (!bytes) {
      say(why, why_size, "nowhere to read a texture into");
      return -1;
   }
   if (format_is_depth_stencil(texture->res->format)) {
      say(why, why_size, "reading texels out of a %s texture: it has %s, and the one answer "
                         "this call can give would be whichever aspect the caller did not "
                         "mean. rwa_depth_read takes a float per texel, a byte per texel, "
                         "or both", texture->format_name,
          format_has_depth(texture->res->format) && format_has_stencil(texture->res->format)
             ? "a depth and a stencil aspect"
             : format_has_depth(texture->res->format) ? "a depth aspect"
                                                      : "a stencil aspect");
      return -1;
   }
   /* A multisample texture has no one texel to give back: it holds nr_samples values
    * per texel and this call's answer is one of them, which is the same shape of
    * refusal as a depth texel's two aspects -- except that there is nowhere to add a
    * sample argument either, because Metal has no call that reads one sample of a
    * multisample texture to the processor (-getBytes: refuses one for the storage-mode
    * reason rwa_texture_write gives). The two things that read one in Metal are a
    * shader (read(coord, sample), which is what this library binds it for) and a
    * resolve into a one-sample texture, which this library has not got.
    */
   if (texture->res->nr_samples > 1) {
      say(why, why_size, "reading texels out of a %s of %u samples per texel: it holds %u "
                         "values per texel and the one answer this call can give would be "
                         "whichever sample the caller did not mean. Metal reads one with a "
                         "shader's read(coord, sample) or with a resolve, and neither is "
                         "this call", texture->format_name, texture->res->nr_samples,
          texture->res->nr_samples);
      return -1;
   }
   if (!resource_level(texture, &level, why, why_size) ||
       !texture_box(texture, level, layer, origin, size, &box, why, why_size) ||
       !caller_stride(texture, &box, stride, &row, why, why_size))
      return -1;

   struct pipe_transfer *xfer = NULL;
   const uint8_t *map = pipe->texture_map(pipe, texture->res, level, PIPE_MAP_READ, &box,
                                          &xfer);
   if (!map) {
      say(why, why_size, "the GPU driver would not map a %ux%u region of this texture for "
                         "the processor", box.width, box.height);
      return -1;
   }

   /* Row by row, because the driver's rows are as far apart as its own layout says and
    * the caller's are as far apart as the caller said. They are equal often enough
    * that a single memcpy would pass every test here and then tear on the phone, where
    * freedreno's pitch is aligned and softpipe's is not.
    */
   size_t texel_row = (size_t)box.width * util_format_get_blocksize(texture->res->format);
   uint8_t *dst = bytes;
   for (int z = 0; z < box.depth; z++)
      for (int y = 0; y < box.height; y++)
         memcpy(dst + ((size_t)z * box.height + y) * row,
                map + (size_t)z * xfer->layer_stride + (size_t)y * xfer->stride, texel_row);

   pipe->texture_unmap(pipe, xfer);
   return 0;
}

/*
 * What a depth/stencil texture holds, one aspect at a time (air_metal.h).
 *
 * The decoding is Mesa's own for the format and not arithmetic written here. That is
 * the same decision as the colour format table being identities only, and it is what
 * makes Depth16Unorm and Depth32Float comparable at all: both come back as a float in
 * [0, 1], 65535 and 1.0f being the same depth. util_format also knows the layouts this
 * library would otherwise have to guess -- freedreno keeps Z32_FLOAT_S8X24_UINT as two
 * resources and its transfer helper interleaves them for a map
 * (U_TRANSFER_HELPER_SEPARATE_Z32S8 in freedreno_resource.c), so what arrives here is
 * the packed format whichever back end it came from.
 */
int
rwa_depth_read(struct rwa_texture *texture, unsigned level, unsigned layer,
               const unsigned origin[3], const unsigned size[3], float *depth,
               uint8_t *stencil, char *why, size_t why_size)
{
   RWA_LOCKED;
   struct pipe_context *pipe = texture->device->pipe;
   enum pipe_format format = texture->res->format;
   struct pipe_box box;

   if (!depth && !stencil) {
      say(why, why_size, "a depth read that asks for neither aspect");
      return -1;
   }
   if (!format_is_depth_stencil(format)) {
      say(why, why_size, "%s is a colour format and has no depth or stencil aspect; its "
                         "texels come back through rwa_texture_read", texture->format_name);
      return -1;
   }
   /* An aspect the format has not got is refused by name rather than zeroed: a caller
    * asking for the stencil of a Depth32Float texture believes it allocated a different
    * texture, and a buffer of zeros would be evidence for that belief.
    */
   if (depth && !format_has_depth(format)) {
      say(why, why_size, "the depth of a %s texture, which has only a stencil aspect",
          texture->format_name);
      return -1;
   }
   if (stencil && !format_has_stencil(format)) {
      say(why, why_size, "the stencil of a %s texture, which has only a depth aspect",
          texture->format_name);
      return -1;
   }
   /* A multisample depth attachment is refused here for rwa_texture_read's reason and
    * not for this one's: the aspect is named, the sample is not, and there is no Metal
    * call that reads one sample of one to the processor.
    */
   if (texture->res->nr_samples > 1) {
      say(why, why_size, "the depth of a %s of %u samples per texel: this call gives one "
                         "value per texel and such a texture holds %u, with no argument "
                         "here to say which. A multisample depth attachment is read by a "
                         "shader or resolved, and this library resolves nothing",
          texture->format_name, texture->res->nr_samples, texture->res->nr_samples);
      return -1;
   }
   if (!resource_level(texture, &level, why, why_size) ||
       !texture_box(texture, level, layer, origin, size, &box, why, why_size))
      return -1;

   const struct util_format_unpack_description *unpack =
      util_format_unpack_description(format);
   if ((depth && !unpack->unpack_z_float) || (stencil && !unpack->unpack_s_8uint)) {
      say(why, why_size, "Mesa has no unpacker for the %s aspect of %s, and this library "
                         "will not take the bits apart itself",
          depth && !unpack->unpack_z_float ? "depth" : "stencil", texture->format_name);
      return -1;
   }

   struct pipe_transfer *xfer = NULL;
   const uint8_t *map = pipe->texture_map(pipe, texture->res, level, PIPE_MAP_READ, &box,
                                          &xfer);
   if (!map) {
      say(why, why_size, "the GPU driver would not map a %ux%u region of this %s texture "
                         "for the processor", box.width, box.height, texture->format_name);
      return -1;
   }

   /* Row by row and slice by slice, for rwa_texture_read's reason: the driver's rows are
    * as far apart as its layout says. The caller's are tightly packed, one value per
    * texel, which is what the contract promises and is why there is no stride argument.
    */
   for (int z = 0; z < box.depth; z++) {
      for (int y = 0; y < box.height; y++) {
         const uint8_t *row = map + (size_t)z * xfer->layer_stride + (size_t)y * xfer->stride;
         size_t at = ((size_t)z * box.height + y) * box.width;
         if (depth)
            unpack->unpack_z_float(depth + at, 0, row, 0, box.width, 1);
         if (stencil)
            unpack->unpack_s_8uint(stencil + at, 0, row, 0, box.width, 1);
      }
   }

   pipe->texture_unmap(pipe, xfer);
   return 0;
}

/*
 * A w x h sample of the texture's first level as 8-bit RGBA, nearest texel at the centre of each
 * cell, for a driver's diagnostic print (REDWOOD_THUMBS in the userland driver). Values are
 * clamped to [0, 1], except in a two-channel float texture, which holds signed values (RenderBox
 * accumulates path winding in one): there R and G are magnitudes and B carries the signs, 128
 * for a negative R and 64 for a negative G. A depth texture is sampled as RenderBox's depth
 * values, z times 2^32: R is the whole part times four (0 to 63), G the fraction, and B is 255
 * where the value is 64 or more. A stencil-only, multisample or compressed texture is refused
 * with -1.
 * Mapping for reading waits for the work that writes the texture, so a call between two draws
 * splits their batch.
 */
int rwa_texture_thumbnail(struct rwa_texture *texture, unsigned w, unsigned h, uint8_t *rgba);

int
rwa_texture_thumbnail(struct rwa_texture *texture, unsigned w, unsigned h, uint8_t *rgba)
{
   RWA_LOCKED;
   enum pipe_format format = texture ? texture_format(texture) : PIPE_FORMAT_NONE;
   unsigned level = 0;
   char why[64];

   bool depth = texture && format_is_depth_stencil(texture->res->format);
   if (!texture || !rgba || !w || !h || (depth && !format_has_depth(texture->res->format)) ||
       texture->res->nr_samples > 1 || util_format_is_compressed(format) ||
       !resource_level(texture, &level, why, sizeof(why)))
      return -1;
   const struct util_format_unpack_description *unpack =
      depth ? util_format_unpack_description(texture->res->format) : NULL;
   if (depth && !unpack->unpack_z_float)
      return -1;

   struct pipe_context *pipe = texture->device->pipe;
   unsigned width = u_minify(texture->res->width0, level);
   unsigned height = u_minify(texture->res->height0, level);
   struct pipe_box box;
   u_box_2d(0, 0, width, height, &box);
   struct pipe_transfer *xfer = NULL;
   const uint8_t *map = pipe->texture_map(pipe, texture->res, level, PIPE_MAP_READ, &box, &xfer);
   if (!map)
      return -1;

   unsigned cpp = util_format_get_blocksize(depth ? texture->res->format : format);
   bool signs = util_format_get_nr_components(format) == 2 && util_format_is_float(format);
   for (unsigned y = 0; y < h; y++) {
      for (unsigned x = 0; x < w; x++) {
         unsigned sx = (2 * x + 1) * width / (2 * w), sy = (2 * y + 1) * height / (2 * h);
         const uint8_t *at = map + (size_t)sy * xfer->stride + (size_t)sx * cpp;
         float texel[4] = { 0, 0, 0, 1 };
         if (depth) {
            float z;
            unpack->unpack_z_float(&z, 0, at, 0, 1, 1);
            double d = (double)z * 4294967296.0;
            texel[0] = (float)(MIN2(floor(d), 63.0) * 4.0 / 255.0);
            texel[1] = (float)(d - floor(d));
            texel[2] = d >= 64.0 ? 1.0f : 0.0f;
         } else {
            util_format_unpack_rgba(format, texel, at, 1);
         }
         if (signs) {
            texel[2] = ((texel[0] < 0 ? 128 : 0) | (texel[1] < 0 ? 64 : 0)) / 255.0f;
            texel[0] = fabsf(texel[0]);
            texel[1] = fabsf(texel[1]);
         }
         for (unsigned c = 0; c < 4; c++)
            rgba[(y * w + x) * 4 + c] = (uint8_t)(CLAMP(texel[c], 0.0f, 1.0f) * 255.0f + 0.5f);
      }
   }
   pipe->texture_unmap(pipe, xfer);
   return 0;
}
