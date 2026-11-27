/* Copy/write ring diagnostics and SIGSEGV guard helpers, included into air_metal.c. */

/* Two rings of sixteen, printed only when this process dies on SIGSEGV. One records every copy of a
 * rectangle (`util_copy_box` calls), the other every rwa_texture_write entry. Both print with the
 * `tl: x` prefix and are told apart by the field count: six fields carry a resource, seven carry a
 * texture write. The prefix is `tl: x` because `tl: c` belongs to the touch driver
 * (xnu/files/bsd/dev/annibale_goodix.c) and `tl: w` to the kernel's 117-line counter.
 *
 * The fault is a memcpy reading at an address whose own bytes read as text or as a bit pattern: bad
 * access prints that address as x1, and the values seen so far include `backlight-` reversed,
 * `meContro`, `0x80000c000c0000` and `0x3e03000000000000`. A field that should hold a data pointer
 * holds something else.
 *
 * The seven-field line answers whether the caller handed that value in: rwa_texture_write records
 * `bytes` before it validates anything, so a `bytes` equal to the faulting copy's `src` puts the
 * fault outside this library.
 *
 * The six-field line answers which object the copy was for, because a copy can arrive from two
 * places in this build and only one of them is a texture write: u_default_texture_subdata fills a
 * texture from client memory, util_resource_copy_region copies one resource into another and is
 * reached from freedreno's blit fallbacks. `<res>` is the resource the copy reads from; when the
 * source is client memory there is no such resource, so it records the texture being filled.
 * Printing either line live is not an option: one frame of the desktop makes many copies and the
 * log ring holds about twenty seconds of tl: lines.
 */
#define RWA_RING 16

struct rwa_copy_record {
   const void *res;
   const void *buf;
   const void *src;
   unsigned level;
   unsigned box_y;
   unsigned rows;
   unsigned len;
   uint64_t at;   /* microseconds of mach_absolute_time, the clock the tl: lines carry */
};

struct rwa_write_record {
   const void *texture;
   unsigned level;
   unsigned layer;
   const void *bytes;
   size_t stride;
   unsigned width;
   unsigned height;
   uint64_t at;
};

static struct rwa_copy_record rwa_copy_ring[RWA_RING];
static unsigned rwa_copy_written;   /* monotonic count; a call's slot is its remainder */
static const void *rwa_last_copy_res;   /* the resource of the most recent recorded copy */
static struct rwa_write_record rwa_write_ring[RWA_RING];
static unsigned rwa_write_written;

/* Microseconds on the device's own clock, which is what every tl: line carries: the rings are read
 * against those lines, so they cannot be on a clock of their own.
 */
static uint64_t
device_us(void)
{
   static mach_timebase_info_data_t base;

   if (!base.denom)
      mach_timebase_info(&base);
   return mach_absolute_time() * base.numer / base.denom / 1000;
}

/* Every rwa_texture_write entry, before it validates anything: a call that carries a NULL or a
 * garbage `bytes` is the answer to one of the two questions this ring exists for, so the record
 * cannot sit behind the checks. `size` is the caller's array and is read only for its first two
 * entries, which every caller passes; a NULL array would be refused a few lines later anyway.
 */
static void
rwa_write_record_entry(const void *texture, unsigned level, unsigned layer, const void *bytes,
                       size_t stride, const unsigned *size)
{
   unsigned slot = __atomic_fetch_add(&rwa_write_written, 1, __ATOMIC_RELAXED) % RWA_RING;

   rwa_write_ring[slot] = (struct rwa_write_record){
      .texture = texture, .level = level, .layer = layer, .bytes = bytes, .stride = stride,
      .width = size ? size[0] : 0, .height = size ? size[1] : 0, .at = device_us(),
   };
}

/* Called from util_copy_rect, which is Mesa's file
 * (tools/mesa/rebase-26/0012-fd-shadow-guard-probes.patch) and holds no state of its own. Hidden so the
 * library's exported symbols stay the ones air_metal.h declares; the other translation unit in this
 * library resolves it at link time like any other internal call.
 */
__attribute__((visibility("hidden")))
void rwa_copy_record(const void *res, const void *buf, const void *src, unsigned level,
                     unsigned box_y, unsigned rows, unsigned len);

__attribute__((visibility("hidden")))
void
rwa_copy_record(const void *res, const void *buf, const void *src, unsigned level, unsigned box_y,
                unsigned rows, unsigned len)
{
   unsigned slot = __atomic_fetch_add(&rwa_copy_written, 1, __ATOMIC_RELAXED) % RWA_RING;

   rwa_copy_ring[slot] =
      (struct rwa_copy_record){ res, buf, src, level, box_y, rows, len, device_us() };
   rwa_last_copy_res = res;
}

/* One line when a region reaches texture_box with no texels in it, and nothing otherwise. The
 * refusal itself is older than this library's instrumentation: texture_box has always declined
 * these. The line says which texture and which region, for the case where the region is not one
 * the caller meant to write.
 */
static void
write_zero_region(const struct rwa_texture *texture, unsigned w, unsigned h, unsigned d)
{
   char line[128];
   int n = snprintf(line, sizeof line, "tl: y %p %u %u %u %llu\n", (const void *)texture, w, h, d,
                    (unsigned long long)device_us());

   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* One line when a transfer map is about to return an address built from a buffer object it cannot
 * read, and nothing otherwise. A mapped buffer has `bo->map` set for as long as the bo lives, so a
 * null there, a null bo, or an offset past the bo's size are the three ways the address a caller
 * receives stops being inside any buffer. The bo and its map are printed as well as the offset:
 * together they separate a stale struct from an offset that came out too large.
 */
__attribute__((visibility("hidden")))
void rwa_map_anomaly(const void *res, const void *bo, const void *bo_map, uint64_t offset,
                     uint64_t bo_size, unsigned level, unsigned box_y);

__attribute__((visibility("hidden")))
void
rwa_map_anomaly(const void *res, const void *bo, const void *bo_map, uint64_t offset,
                uint64_t bo_size, unsigned level, unsigned box_y)
{
   char line[192];
   int n = snprintf(line, sizeof line, "tl: z %p %p %p %llu %llu %u %u %llu\n", res, bo, bo_map,
                    (unsigned long long)offset, (unsigned long long)bo_size, level, box_y,
                    (unsigned long long)device_us());

   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* The two halves of the address a transfer map returns, printed only when the copy is about to read
 * a pointer that cannot be one: `bo_map` is the buffer's own mapping and `offset` is what the driver
 * added to it, so a huge offset and a stale map are told apart on the line. `rows` and `len` say
 * which copy this is, in the same units the tl: x line uses.
 *
 * The dst side has the same shape without rows and len: it exists to show that the far side of the
 * copy was inside its buffer at the moment the near side was not.
 */
__attribute__((visibility("hidden")))
void rwa_map_parts(const void *res, const void *bo, const void *bo_map, uint64_t offset,
                   unsigned rows, unsigned len);

__attribute__((visibility("hidden")))
void
rwa_map_parts(const void *res, const void *bo, const void *bo_map, uint64_t offset, unsigned rows,
              unsigned len)
{
   char line[192];
   int n = snprintf(line, sizeof line, "tl: z2 %p %p %p %llu %u %u %llu\n", res, bo, bo_map,
                    (unsigned long long)offset, rows, len,
                    (unsigned long long)device_us());

   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

__attribute__((visibility("hidden")))
void rwa_dst_parts(const void *res, const void *bo, const void *bo_map, uint64_t offset);

__attribute__((visibility("hidden")))
void
rwa_dst_parts(const void *res, const void *bo, const void *bo_map, uint64_t offset)
{
   char line[192];
   int n = snprintf(line, sizeof line, "tl: z3 %p %p %p %llu %llu\n", res, bo, bo_map,
                    (unsigned long long)offset, (unsigned long long)device_us());

   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* The rect-level line: util_copy_rect walks rows and advances the source by the stride, so a first
 * row that is a fine address says nothing about the row the copy dies on. Both ends are computed
 * here and the line is printed only when one of them cannot be an address.
 *
 * `res` is the resource of the most recent recorded copy, which is the copy this rect belongs to:
 * util_copy_box records before it hands the rects over. A rect that arrives from some other caller
 * carries whatever the last recorded copy was, which is why the line is a diagnostic and not a fact
 * about the rect on its own.
 */
__attribute__((visibility("hidden")))
void rwa_copy_rect_guard(const void *src, long stride, unsigned rows, unsigned len);

__attribute__((visibility("hidden")))
void
rwa_copy_rect_guard(const void *src, long stride, unsigned rows, unsigned len)
{
   uintptr_t first = (uintptr_t)src;
   uintptr_t last = (uintptr_t)((intptr_t)first + (intptr_t)(rows ? rows - 1 : 0) * (intptr_t)stride);
   char line[192];
   int n;

   if (!((first >> 48) | (last >> 48)))
      return;

   n = snprintf(line, sizeof line, "tl: z4 %p %p %zu %u %u %p %llu\n", rwa_last_copy_res, src,
                (size_t)(stride < 0 ? -stride : stride), rows, len, (const void *)last,
                (unsigned long long)device_us());
   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* The box-level line: the layer every copy entrance passes through, so it sees the source pointer
 * that the rect then walks. `src_res` is the resource the most recent recorded copy named, printed
 * as the pointer itself: if that value sits in the image's string table, the resource pointer was
 * read as text somewhere above this call; if it is an ordinary heap pointer while `src` is text,
 * the source address came from the mapping layer instead.
 */
__attribute__((visibility("hidden")))
void rwa_copy_box_guard(const void *src, long stride, unsigned x, unsigned y, unsigned w,
                        unsigned h);

__attribute__((visibility("hidden")))
void
rwa_copy_box_guard(const void *src, long stride, unsigned x, unsigned y, unsigned w, unsigned h)
{
   char line[192];
   int n;

   if (!((uintptr_t)src >> 48))
      return;

   n = snprintf(line, sizeof line, "tl: z5 %p %p %zu %u %u %u %u %llu\n", rwa_last_copy_res, src,
                (size_t)(stride < 0 ? -stride : stride), x, y, w, h,
                (unsigned long long)device_us());
   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* The address resource_transfer_map_unsync is about to hand a caller, when that address cannot be
 * one. `bo_map` and `offset` are the two halves; `bo_src` is the layout's surface offset for this
 * level, which separates "the backing object changed" (surface offset moves with the layout that
 * the swap brought in) from "only the stride or the box moved" (surface offset holds, the product
 * term does not). `level` and `box.y` are the inputs to that product.
 */
__attribute__((visibility("hidden")))
void rwa_map_unsync_guard(const void *res, const void *bo, const void *bo_map, uint64_t offset,
                          uint64_t bo_size, unsigned level, unsigned box_y, uint64_t bo_src);

__attribute__((visibility("hidden")))
void
rwa_map_unsync_guard(const void *res, const void *bo, const void *bo_map, uint64_t offset,
                     uint64_t bo_size, unsigned level, unsigned box_y, uint64_t bo_src)
{
   char line[224];
   int n = snprintf(line, sizeof line, "tl: z6 %p %p %p %llu %llu %u %u %llu %llu\n", res, bo,
                    bo_map, (unsigned long long)offset, (unsigned long long)bo_size, level, box_y,
                    (unsigned long long)bo_src, (unsigned long long)device_us());

   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* The other map path: a tiled resource is mapped through a staging resource, so the pointer a caller
 * receives comes from `fd_bo_map(staging_rsc->bo)` and never touches the offset arithmetic. Printed
 * only when that pointer cannot be an address, or when there is no buffer object behind it.
 */
__attribute__((visibility("hidden")))
void rwa_staging_guard(const void *staging_res, const void *bo, const void *bo_map,
                       uint64_t bo_size, const void *layout_resource);

__attribute__((visibility("hidden")))
void
rwa_staging_guard(const void *staging_res, const void *bo, const void *bo_map, uint64_t bo_size,
                  const void *layout_resource)
{
   char line[192];
   int n = snprintf(line, sizeof line, "tl: z7 %p %p %p %llu %p %llu\n", staging_res, bo, bo_map,
                    (unsigned long long)bo_size, layout_resource,
                    (unsigned long long)device_us());

   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* The pointer a copy uses, taken at the moment the map call returned it, next to the transfer that
 * produced it and the two fields the helper's own transfer holds. `map == staging` says the address
 * is the helper's staging buffer; `map == ptr` says it is the driver's map of the resource; neither
 * says the helper is not where this address came from.
 */
__attribute__((visibility("hidden")))
void rwa_map_guard(const void *map, const void *transfer, unsigned is_helper, const void *ptr,
                   const void *staging);

__attribute__((visibility("hidden")))
void
rwa_map_guard(const void *map, const void *transfer, unsigned is_helper, const void *ptr,
              const void *staging)
{
   char line[192];
   int n = snprintf(line, sizeof line, "tl: z8 %p %p %u %p %p %llu\n", map, transfer, is_helper,
                    ptr, staging, (unsigned long long)device_us());

   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* The pointer a caller handed to texture_subdata, checked at the layer that receives it: this path
 * copies from the caller's memory and never maps anything, so a bad value here came in through the
 * call rather than out of a buffer object.
 */
__attribute__((visibility("hidden")))
void rwa_subdata_guard(const void *data, const void *transfer, unsigned level, unsigned stride,
                       unsigned w, unsigned h);

__attribute__((visibility("hidden")))
void
rwa_subdata_guard(const void *data, const void *transfer, unsigned level, unsigned stride, unsigned w,
                  unsigned h)
{
   char line[192];
   int n = snprintf(line, sizeof line, "tl: z9 %p %p %u %u %u %u %llu\n", data, transfer, level,
                    stride, w, h, (unsigned long long)device_us());

   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* Who called the subdata path. `self` is the callee's own address, so `ret - self` is the offset of
 * the call site when both sit in one image; a `ret` far from `self` means the caller is in another
 * library, which is the case worth distinguishing rather than guessing at.
 */
__attribute__((visibility("hidden")))
void rwa_subdata_caller(const void *data, const void *ret, const void *self, const void *transfer);

__attribute__((visibility("hidden")))
void
rwa_subdata_caller(const void *data, const void *ret, const void *self, const void *transfer)
{
   char line[192];
   int n = snprintf(line, sizeof line, "tl: za %p %p %p %p %llu\n", data, ret, self, transfer,
                    (unsigned long long)device_us());

   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* The value the write is about to copy from, taken at the last point inside this library before the
 * driver sees it, next to the resource it goes into. Printed only when it cannot be an address, so
 * it lines up with the same test at the entry points on either side of this one.
 */
__attribute__((visibility("hidden")))
void rwa_texture_write_guard(const void *bytes, const void *res);

__attribute__((visibility("hidden")))
void
rwa_texture_write_guard(const void *bytes, const void *res)
{
   char line[160];
   int n = snprintf(line, sizeof line, "tl: zb %p %p %llu\n", bytes, res,
                    (unsigned long long)device_us());

   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* Who called this API. `ret - self` distinguishes the two upstreams that reach it: the driver's
 * -replaceRegion: sits in another image, while the buffer-backed upload path is inside this one.
 */
__attribute__((visibility("hidden")))
void rwa_texture_write_caller(const void *bytes, const void *ret, const void *self);

__attribute__((visibility("hidden")))
void
rwa_texture_write_caller(const void *bytes, const void *ret, const void *self)
{
   char line[160];
   int n = snprintf(line, sizeof line, "tl: zc %p %p %p %llu\n", bytes, ret, self,
                    (unsigned long long)device_us());

   if (n > 0)
      (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
}

/* The two rings on one timeline, oldest first, then the process dies the way it would have without
 * this handler: SIG_DFL and a re-raise, so the kernel still records `died on signal`. One timeline
 * because the order is the evidence -- a write entry precedes the copies it makes, so which write a
 * faulting copy belongs to is read off the sequence. Slots never written hold at 0 and are skipped;
 * they are always the tail of a ring that filled fewer than sixteen times.
 *
 * write() rather than printf: the faulting thread may hold stdio's lock, and a handler waiting for it
 * leaves the process alive with no dump.
 */
static void
rwa_rings_on_segv(int sig)
{
   unsigned wrote = __atomic_load_n(&rwa_write_written, __ATOMIC_RELAXED);
   unsigned copied = __atomic_load_n(&rwa_copy_written, __ATOMIC_RELAXED);
   unsigned w_first = wrote > RWA_RING ? wrote - RWA_RING : 0;
   unsigned r_first = copied > RWA_RING ? copied - RWA_RING : 0;
   unsigned w_left = wrote - w_first, r_left = copied - r_first;
   unsigned w = 0, r = 0;

   while (w < w_left || r < r_left) {
      const struct rwa_write_record *write_rec = &rwa_write_ring[(w_first + w) % RWA_RING];
      const struct rwa_copy_record *copy_rec = &rwa_copy_ring[(r_first + r) % RWA_RING];
      bool take_write = w < w_left &&
         (r == r_left || write_rec->at <= copy_rec->at);
      char line[160];
      int n;

      if (take_write) {
         n = snprintf(line, sizeof line, "tl: x %p %u %u %p %zu %u %u %llu\n",
                      write_rec->texture, write_rec->level, write_rec->layer, write_rec->bytes,
                      write_rec->stride, write_rec->width, write_rec->height,
                      (unsigned long long)write_rec->at);
         w++;
      } else {
         n = snprintf(line, sizeof line, "tl: x %p %p %p %u %u %u %u %llu\n", copy_rec->res,
                      copy_rec->buf, copy_rec->src, copy_rec->level, copy_rec->box_y,
                      copy_rec->rows, copy_rec->len, (unsigned long long)copy_rec->at);
         r++;
      }

      if (n > 0)
         (void) write(STDOUT_FILENO, line, MIN2(n, (int)sizeof line - 1));
   }

   signal(sig, SIG_DFL);
   raise(sig);
}

/* Once per process, at the first device: the rings belong to this library and a driver opens one
 * device.
 */
static void
rwa_rings_install(void)
{
   static bool installed;

   if (installed)
      return;
   installed = true;
   signal(SIGSEGV, rwa_rings_on_segv);
}

