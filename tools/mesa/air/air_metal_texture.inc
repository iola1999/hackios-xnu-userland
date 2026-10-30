/*
 * Textures over a caller's buffer, and views of a texture, included into air_metal.c.
 *
 * A buffer texture keeps its own resource. The buffer's bytes are copied into it when it is
 * made and before each batch of work (rwa_buffer_textures_refresh); once the texture has been
 * written, its bytes are copied back into the buffer instead, at the next refresh or when the
 * caller has waited for the work (rwa_buffer_textures_writeback). A buffer the caller made over
 * its own memory (-newBufferWithBytesNoCopy:, which this library holds as a copy) has that
 * memory as its mirror: what the caller writes there reaches the copy before the textures are
 * refreshed (buffer_mirror_pull), and the results go into both.
 *
 * A view shares the texture's resource and differs in the pixel format it is sampled, written
 * and rendered as.
 */

/* The memory a no-copy buffer was made over. The driver copies it into the buffer once, at
 * creation, and the caller keeps reading and writing it afterwards; NULL turns the mirror off. */
void rwa_buffer_set_mirror(struct rwa_buffer *buffer, void *mirror);

/*
 * The caller's memory is read with vm_read_overwrite, a page at a time, never by load: a caller
 * may free it while the buffer is still alive (backboardd does), and in render53 a plain read
 * of such memory killed backboardd. An unmapped page fails the call and the read is given up.
 */
static bool
mirror_read(const char *from, size_t size, uint64_t *hash, char *into)
{
   static uint64_t page[2048];
   uint64_t h = 0xcbf29ce484222325ull;

   for (size_t done = 0; done < size;) {
      vm_size_t n = MIN2(sizeof(page), size - done), got = 0;
      if (vm_read_overwrite(mach_task_self(), (vm_address_t)(uintptr_t)(from + done), n,
                            (vm_address_t)(uintptr_t)page, &got) != KERN_SUCCESS || got != n)
         return false;
      if (n % 8)
         memset((char *)page + n, 0, 8 - n % 8);
      for (size_t i = 0; i < DIV_ROUND_UP(n, 8); i++)
         h = (h ^ page[i]) * 0x100000001b3ull;
      if (into)
         memcpy(into + done, page, n);
      done += n;
   }
   *hash = h;
   return true;
}

/* Copy what the caller wrote to its memory since this library last read or wrote it. The
 * caller may also write the buffer through -contents, which is this library's copy, so the
 * copy is replaced only when the caller's memory changed. */
static void
buffer_mirror_pull(struct rwa_buffer *buffer)
{
   static unsigned said;
   uint64_t h;

   if (!buffer->mirror || !buffer->map || !mirror_read(buffer->mirror, buffer->size, &h, NULL))
      return;
   if (buffer->mirror_hashed && h == buffer->mirror_hash)
      return;
   if (buffer->mirror_hashed && !mirror_read(buffer->mirror, buffer->size, &h, buffer->map))
      return;
   if (buffer->mirror_hashed && said < 32)
      printf("rwa mirror %u: the caller wrote the memory of %zu-byte buffer %p\n", said++,
             buffer->size, (void *)buffer);
   buffer->mirror_hash = h;
   buffer->mirror_hashed = true;
}

void
rwa_buffer_set_mirror(struct rwa_buffer *buffer, void *mirror)
{
   RWA_LOCKED;
   if (!buffer)
      return;
   buffer->mirror = mirror;
   buffer->mirror_hashed = false;
   buffer_mirror_pull(buffer);   /* the hash of what the driver has just copied */
}

/* Where the texture's rows lie in the buffer, or NULL when they do not fit in it. */
static char *
buffer_texture_bytes(struct rwa_buffer_texture *backing, char *why, size_t why_size)
{
   struct rwa_buffer *source = backing->source;
   unsigned height = backing->texture->res->height0;
   size_t row = (size_t)backing->row_bytes;

   if (!source->map) {
      say(why, why_size, "the buffer this texture was laid over is not mapped");
      return NULL;
   }
   if (backing->offset > source->size || row * height > source->size - backing->offset) {
      say(why, why_size, "%zu bytes a row over %u rows at offset %zu run past the %zu-byte "
                         "buffer this texture was laid over", row, height, backing->offset,
          source->size);
      return NULL;
   }
   return (char *)source->map + backing->offset;
}

/* Copy the caller's bytes into the texture, row by row at the caller's stride. */
static int
buffer_texture_upload(struct rwa_buffer_texture *backing, char *why, size_t why_size)
{
   const char *bytes = buffer_texture_bytes(backing, why, why_size);

   if (!bytes || rwa_texture_write(backing->texture, 0, 0, NULL, NULL, bytes,
                                   backing->row_bytes, why, why_size) != 0)
      return -1;
   backing->written = false;   /* the texture now holds the buffer's bytes */
   return 0;
}

/* Copy the texture back into the caller's buffer. Mapping the texture for reading waits for
 * the work that wrote it. */
static int
buffer_texture_download(struct rwa_buffer_texture *backing, char *why, size_t why_size)
{
   char *bytes = buffer_texture_bytes(backing, why, why_size);

   if (bytes)
      buffer_mirror_pull(backing->source);   /* the caller's other writes, before the results */
   if (!bytes || rwa_texture_read(backing->texture, 0, 0, NULL, NULL, bytes,
                                  backing->row_bytes, why, why_size) != 0)
      return -1;
   size_t span = (size_t)backing->row_bytes * backing->texture->res->height0;
   if (backing->source->mirror) {
      memcpy((char *)backing->source->mirror + backing->offset, bytes, span);
      backing->source->mirror_hashed = false;
      buffer_mirror_pull(backing->source);
   }
   /* REDWOOD_THUMBS diagnostic: what the first writebacks carried, and where to */
   static unsigned said;
   if (said < 64 && getenv("REDWOOD_THUMBS")) {
      size_t nonzero = 0;
      for (size_t i = 0; i < span; i++)
         nonzero += bytes[i] != 0;
      printf("rwa writeback %u: %ux%u %s, %u bytes a row at offset %zu of buffer %p, mirror %p, "
             "%zu of %zu bytes nonzero\n", said++, backing->texture->res->width0,
             backing->texture->res->height0, backing->texture->format_name, backing->row_bytes,
             backing->offset, (void *)backing->source, backing->source->mirror, nonzero, span);
   }
   backing->written = false;
   return 0;
}

/*
 * A texture over a buffer the caller already holds, which is Metal's
 * -newTextureWithDescriptor:offset:bytesPerRow:.
 *
 * Gallium has no way to lay one resource over another's memory, so this keeps a texture
 * with memory of its own and copies the caller's bytes into it. The copy happens here and
 * again in rwa_buffer_textures_refresh, before each batch of work: the buffer is written by
 * the processor between calls, and nothing tells this library when. A texture that has been
 * rendered into or otherwise written goes the other way, into the buffer, because a caller
 * reads the result there: IconRendering renders an icon into such a texture, waits for the
 * command buffer and reads the buffer's bytes.
 */
/*
 * The texture laid over the buffer's own memory: an import of the buffer's buffer object at the
 * offset and row length the caller gave, so the GPU reads and writes the bytes the caller
 * reads and writes, and nothing is copied either way. The A8XX reads a linear texture at a row
 * pitch rounded up to 64 bytes and from a 64-byte aligned start, which is also what
 * minimumLinearTextureAlignmentForPixelFormat: tells callers; anything else, and a buffer
 * whose memory is the caller's (a mirror), keeps the copy below. NULL: no alias, and why
 * does not matter to the caller, which copies instead.
 */
static struct rwa_texture *
buffer_texture_alias(struct rwa_device *device, const struct rwa_texture_shape *shape,
                     struct rwa_buffer *source, size_t offset, unsigned row_bytes)
{
   static unsigned said;
   char why[256] = { 0 };
   struct winsys_handle handle = { .type = WINSYS_HANDLE_TYPE_KMS };
   struct rwa_texture *texture = NULL;

   if (getenv("RWA_BUFFER_TEXTURE_COPY"))
      say(why, sizeof(why), "RWA_BUFFER_TEXTURE_COPY is set");
   else if (source->mirror)
      say(why, sizeof(why), "the buffer's memory is the caller's (a no-copy buffer)");
   else if (offset % 64 != 0 || row_bytes % 64 != 0 || offset > UINT_MAX)
      say(why, sizeof(why), "offset or row length is not a multiple of 64 bytes");
   else if (device->software || !source->res || !device->screen->resource_get_handle ||
            !device->screen->resource_get_handle(device->screen, device->pipe, source->res,
                                                 &handle, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE))
      say(why, sizeof(why), "the buffer object has no handle to import");
   else {
      const struct rwa_surface_import import = {
         .offset = (unsigned)offset, .row_bytes = row_bytes, .kms_handle = handle.handle };
      texture = texture_of_shape(device, shape, &import, why, sizeof(why));
   }

   if (said < 16) {
      said++;
      printf("rwa alias: %ux%u format %u, %u bytes a row at offset %zu of buffer %p: %s\n",
             shape->width, shape->height, shape->pixel_format, row_bytes, offset,
             (void *)source, texture ? "laid over the buffer" : why);
   }
   return texture;
}

struct rwa_texture *
rwa_texture_from_buffer(struct rwa_device *device, const struct rwa_texture_shape *shape,
                        struct rwa_buffer *source, size_t offset, unsigned row_bytes,
                        char *why, size_t why_size)
{
   RWA_LOCKED;
   if (!device || !source) {
      say(why, why_size, "a texture over a buffer needs both a device and the buffer");
      return NULL;
   }
   if (!shape || shape->type != RWA_TYPE_2D || shape->mip_levels > 1 ||
       shape->array_length > 1 || shape->sample_count > 1) {
      say(why, why_size, "a texture over a buffer is a single-sample 2D texture with one "
                         "level; what was asked for has %u level(s), %u slice(s) and %u "
                         "sample(s) of type %u", shape ? shape->mip_levels : 0,
          shape ? shape->array_length : 0, shape ? shape->sample_count : 0,
          shape ? shape->type : 0);
      return NULL;
   }
   struct rwa_texture *texture = buffer_texture_alias(device, shape, source, offset, row_bytes);
   if (texture)
      return texture;
   texture = texture_of_shape(device, shape, NULL, why, why_size);
   if (!texture)
      return NULL;

   struct rwa_buffer_texture *backing = calloc(1, sizeof(*backing));
   if (!backing) {
      say(why, why_size, "out of memory recording a texture over a buffer");
      rwa_texture_destroy(texture);
      return NULL;
   }
   backing->texture = texture;
   backing->source = source;
   backing->offset = offset;
   backing->row_bytes = row_bytes;
   backing->next = device->buffer_textures;
   device->buffer_textures = backing;
   texture->backing = backing;

   buffer_mirror_pull(source);
   if (buffer_texture_upload(backing, why, why_size) != 0) {
      rwa_texture_destroy(texture);
      return NULL;
   }
   return texture;
}

struct rwa_texture *
rwa_texture_view(struct rwa_texture *texture, unsigned mtl_format, unsigned first_level,
                 unsigned num_levels, char *why, size_t why_size)
{
   RWA_LOCKED;
   if (!texture) {
      say(why, why_size, "a view of no texture");
      return NULL;
   }
   if (num_levels == 0 || first_level + num_levels > texture_levels(texture)) {
      say(why, why_size, "a view of levels %u+%u of a %s with %u", first_level, num_levels,
          texture->format_name, texture_levels(texture));
      return NULL;
   }
   const struct rwa_pixel_format *fmt = pixel_format(mtl_format);
   if (!fmt) {
      say(why, why_size, "a view of a %s as pixel format %u, which is %s",
          texture->format_name, mtl_format, pixel_format_refusal(mtl_format));
      return NULL;
   }
   enum pipe_format from = texture_format(texture);
   if (util_format_get_blocksize(fmt->pipe) != util_format_get_blocksize(from) ||
       util_format_get_blockwidth(fmt->pipe) != util_format_get_blockwidth(from) ||
       util_format_get_blockheight(fmt->pipe) != util_format_get_blockheight(from) ||
       format_is_depth_stencil(fmt->pipe) != format_is_depth_stencil(from)) {
      say(why, why_size, "a view of a %s as %s: the texels are not the same kind and size",
          texture->format_name, fmt->name);
      return NULL;
   }
   struct pipe_resource *res = texture->res;
   struct pipe_screen *screen = texture->device->screen;
   unsigned bind = res->bind & (PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET);
   if (bind && !screen->is_format_supported(screen, fmt->pipe, res->target, res->nr_samples,
                                            res->nr_samples, bind)) {
      say(why, why_size, "%s cannot sample or render a view of a %s as %s",
          rwa_device_name(texture->device), texture->format_name, fmt->name);
      return NULL;
   }

   struct rwa_texture *view = calloc(1, sizeof(*view));
   if (!view) {
      say(why, why_size, "out of memory making a texture view");
      return NULL;
   }
   view->device = texture->device;
   pipe_resource_reference(&view->res, res);
   view->shape = texture->shape;
   view->shape.pixel_format = mtl_format;
   view->shape.width = u_minify(texture->shape.width, first_level);
   /* zero height or depth is the caller's "not used", kept as it is */
   if (texture->shape.height)
      view->shape.height = u_minify(texture->shape.height, first_level);
   if (texture->shape.depth)
      view->shape.depth = u_minify(texture->shape.depth, first_level);
   view->shape.mip_levels = num_levels;
   view->first_level = texture->first_level + first_level;
   view->num_levels = num_levels;
   view->format_name = fmt->name;
   view->layers = texture->layers;
   view->shader_read = texture->shader_read;
   view->shader_write = texture->shader_write;
   view->view_format = fmt->pipe;
   return view;
}

/* The textures laid over this buffer keep a borrowed pointer to it and read its `map` and
 * `size` before every batch of work (buffer_texture_upload). An entry left behind would
 * therefore read this struct's memory after it is freed and copy from whatever reused
 * that chunk. Its texture loses the backing record, because the entry and the record are
 * the same allocation. Marked before unlinking: anything that sees the entry in between
 * finds a null source instead of a dangling one.
 */
static void
buffer_textures_forget(struct rwa_buffer *buffer)
{
   struct rwa_buffer_texture **at = &buffer->device->buffer_textures;

   while (*at) {
      struct rwa_buffer_texture *backing = *at;

      if (backing->source != buffer) {
         at = &backing->next;
         continue;
      }
      backing->source = NULL;
      *at = backing->next;
      if (backing->texture)
         backing->texture->backing = NULL;
      free(backing);
   }
}

void
rwa_buffer_textures_refresh(struct rwa_device *device)
{
   RWA_LOCKED;
   for (struct rwa_buffer_texture *b = device->buffer_textures; b; b = b->next)
      if (b->source)
         buffer_mirror_pull(b->source);
   for (struct rwa_buffer_texture *b = device->buffer_textures; b; b = b->next) {
      char why[256] = { 0 };

      /* Backstop for the removal in rwa_buffer_destroy, not a replacement for it: an entry
       * whose source was unlinked elsewhere is skipped rather than read.
       */
      if (!b->source)
         continue;

      /* A refresh that fails is not reported to a caller that is not making this call; the
       * draw that reads the texture reports what the bytes are, and the copy that failed
       * said why once when the texture was made. A written texture holds the newer bytes,
       * so they go to the buffer rather than being overwritten from it.
       */
      if (b->written)
         (void)buffer_texture_download(b, why, sizeof(why));
      else
         (void)buffer_texture_upload(b, why, sizeof(why));
   }
}

void
rwa_buffer_textures_writeback(struct rwa_device *device)
{
   RWA_LOCKED;
   for (struct rwa_buffer_texture *b = device->buffer_textures; b; b = b->next) {
      char why[256] = { 0 };

      if (b->source && b->written)
         (void)buffer_texture_download(b, why, sizeof(why));
   }
}
