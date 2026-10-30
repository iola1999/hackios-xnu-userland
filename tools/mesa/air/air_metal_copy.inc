/* Byte-preserving copies on the existing context; submission and fence behavior
 * match rwa_dispatch. Gallium may choose a CPU fallback, so hardware execution
 * must be checked separately from pixel correctness. */
int
rwa_buffer_copy(struct rwa_buffer *src, size_t src_offset,
                struct rwa_buffer *dst, size_t dst_offset, size_t size,
                char *why, size_t why_size)
{
   RWA_LOCKED;
   if (!src || !dst || src->device != dst->device) {
      say(why, why_size, "buffer copy requires two buffers from one device");
      return -1;
   }
   if (!size || src_offset > src->size || size > src->size - src_offset ||
       dst_offset > dst->size || size > dst->size - dst_offset ||
       src_offset > INT32_MAX || dst_offset > INT32_MAX ||
       size > (size_t)INT32_MAX - src_offset || size > (size_t)INT32_MAX - dst_offset) {
      say(why, why_size, "buffer copy range is empty, out of bounds or exceeds Gallium's signed coordinates");
      return -1;
   }
   if (src->encodings || dst->encodings) {
      say(why, why_size, "copying buffers with argument-buffer encodings is unsupported");
      return -1;
   }
   if (src->res == dst->res &&
       src_offset < dst_offset + size && dst_offset < src_offset + size) {
      say(why, why_size, "overlapping buffer copy ranges are unsupported");
      return -1;
   }
   struct pipe_context *pipe = src->device->pipe;
   if (!pipe->resource_copy_region) {
      say(why, why_size, "the driver has no resource copy operation");
      return -1;
   }
   struct pipe_box box = { .x = src_offset, .width = size, .height = 1, .depth = 1 };
   pipe->resource_copy_region(pipe, dst->res, 0, dst_offset, 0, 0, src->res, 0, &box);
   return finish_or_say_why(src->device, true, "buffer copy", why, why_size);
}

static bool
copy_texture_box(struct rwa_texture *texture, unsigned level, unsigned slice,
                 const unsigned origin[3], const unsigned size[3], struct pipe_box *box,
                 char *why, size_t why_size)
{
   const struct pipe_resource *res = texture->res;
   if ((res->target != PIPE_TEXTURE_2D && res->target != PIPE_TEXTURE_2D_ARRAY) ||
       res->nr_samples > 1 || format_is_depth_stencil(res->format) ||
       util_format_is_compressed(res->format)) {
      say(why, why_size, "texture copy supports single-sample 2D color textures without compression");
      return false;
   }
   if (!origin || !size || level > res->last_level || slice >= texture->layers ||
       origin[2] || size[2] != 1 || !size[0] || !size[1]) {
      say(why, why_size, "invalid texture copy level, slice or region");
      return false;
   }
   unsigned width = u_minify(res->width0, level), height = u_minify(res->height0, level);
   if (origin[0] > width || size[0] > width - origin[0] ||
       origin[1] > height || size[1] > height - origin[1] ||
       width > INT32_MAX || height > INT32_MAX || slice > INT32_MAX) {
      say(why, why_size, "texture copy region is out of bounds");
      return false;
   }
   *box = (struct pipe_box){ .x = origin[0], .y = origin[1], .z = slice,
                            .width = size[0], .height = size[1], .depth = 1 };
   return true;
}

int
rwa_texture_copy(struct rwa_texture *src, unsigned src_level, unsigned src_slice,
                 const unsigned src_origin[3], struct rwa_texture *dst,
                 unsigned dst_level, unsigned dst_slice, const unsigned dst_origin[3],
                 const unsigned size[3], char *why, size_t why_size)
{
   RWA_LOCKED;
   if (!src || !dst || src->device != dst->device) {
      say(why, why_size, "texture copy requires two textures from one device");
      return -1;
   }
   if (src->res->format != dst->res->format || src->shape.pixel_format != dst->shape.pixel_format) {
      say(why, why_size, "texture copy requires identical pixel formats");
      return -1;
   }
   struct pipe_box from, to;
   if (!resource_level(src, &src_level, why, why_size) ||
       !resource_level(dst, &dst_level, why, why_size) ||
       !copy_texture_box(src, src_level, src_slice, src_origin, size, &from, why, why_size) ||
       !copy_texture_box(dst, dst_level, dst_slice, dst_origin, size, &to, why, why_size))
      return -1;
   if (src->res == dst->res && src_level == dst_level && src_slice == dst_slice &&
       from.x < to.x + to.width && to.x < from.x + from.width &&
       from.y < to.y + to.height && to.y < from.y + from.height) {
      say(why, why_size, "overlapping texture copy regions are unsupported");
      return -1;
   }
   struct pipe_context *pipe = src->device->pipe;
   if (!pipe->resource_copy_region) {
      say(why, why_size, "the driver has no resource copy operation");
      return -1;
   }
   pipe->resource_copy_region(pipe, dst->res, dst_level, to.x, to.y, to.z,
                              src->res, src_level, &from);
   texture_written(dst);
   return finish_or_say_why(src->device, true, "texture copy", why, why_size);
}
