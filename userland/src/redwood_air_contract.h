/*
 * DRAFT (2026-10-25, mesa/task-22 idle prep) — not included by any TU yet, so it is zero bytes
 * to every build until its adoption batch lands. Adoption = the dedup the audit scheduled as the
 * general first step AFTER the device round (docs/analysis/linecount-audit-userland-2026-10.md
 * section 4): metal_drv_test.m, redwoodmetal.m and drawable_test.c drop their private copies of
 * what is below and include this header instead (a quoted include resolves beside the includer,
 * so build.sh needs no change for the adoption itself).
 *
 * The contract between libredwoodair.dylib (tools/mesa/air/, the producer) and the programs that
 * dlopen it. The authority is tools/mesa/air/air_metal.h — it is not available where these
 * programs compile, so each consumer re-declared what it calls, in three places, with different
 * subsets: three chances for one copy to silently fall behind, and the layout-sensitive ones
 * (struct rwa_texture_decl) would then misread every field after the point of the drift.
 *
 * The rule for the producer's owner: change air_metal.h first, then mirror it here. The asserts
 * below catch a size or field-position change on this side; the self-test TU in the comment at
 * the bottom catches everything the sizes cannot see (field order, enums, signatures) by
 * compiling BOTH headers into one TU and asserting they agree.
 */
#ifndef REDWOOD_AIR_CONTRACT_H
#define REDWOOD_AIR_CONTRACT_H

#include <stddef.h>

/* Define REDWOOD_AIR_CONTRACT_TESTING and include air_metal.h FIRST to compile the self-test TU
 * at the bottom: C has no compatible-but-not-identical redefinition in one translation unit
 * (clang rejects even a faithful copy of an enum), so in that mode the producer's definitions
 * are the ones in scope and the mirror below is skipped — which is exactly what the self-test
 * wants: everything still asserted here (the offsets, the enum values, the typedefs) then runs
 * against the producer's real types, and the standalone compile of this header keeps asserting
 * the same constants against the mirror, so drift on either side trips one of the two. */
#ifndef REDWOOD_AIR_CONTRACT_TESTING

/* The handles libredwoodair hands out and nobody outside it looks inside. Declared as pointers
 * only — anything that needs a field needs the full struct, and that belongs to one consumer's
 * private header (RedwoodMetalSPI.h per the audit), not to this shared face. */
struct rwa_device;
struct rwa_buffer;
struct rwa_texture;
struct rwa_sampler;
struct rwa_compute;
struct rwa_render;
struct rwa_depth_stencil;

/* What kind of element a shader's texture slot holds. Values are the contract, and the order is
 * part of it: rwa_texture_decl.element carries one. Mirrored from air_metal.h; the self-test TU
 * below checks the four values name for name. */
enum rwa_element {
	RWA_ELEMENT_UNKNOWN = 0,
	RWA_ELEMENT_FLOAT,
	RWA_ELEMENT_INT,
	RWA_ELEMENT_UINT,
};

/* One texture slot a shader declares, as rwa_module_textures reports it. Mirrored FIELD FOR
 * FIELD from air_metal.h 244 (its comments say what each field means; the words worth having
 * here are the sizes', since a wrong read here is silent): 6 unsigneds, then Apple's type name
 * copied inline — copied, not pointed at, because rwa_module_textures frees what it read before
 * it returns. metal_drv_test.m holds today's only copy of this struct (104-171); that copy was
 * verified identical to the producer's field by field on 25 October, which is exactly the
 * verification that gets forgotten the next time either side moves. */
struct rwa_texture_decl {
	unsigned index;
	unsigned type;
	unsigned type_known;
	unsigned usage;
	unsigned element;        /* enum rwa_element */
	unsigned element_bits;
	char metal_type[64];
};

#endif /* REDWOOD_AIR_CONTRACT_TESTING */

/* Layout pinned field by field, not just in total: a same-size reshuffle would otherwise pass
 * the size check and misread every field after the point of the change. The constants are the
 * contract and they are asserted against whichever definition is in scope — the mirror's in a
 * standalone compile, the producer's in the self-test TU below. */
_Static_assert(sizeof(struct rwa_texture_decl) == 88,
               "rwa_texture_decl is 88 bytes (air_metal.h 244, 25 October)");
_Static_assert(offsetof(struct rwa_texture_decl, index) == 0 &&
               offsetof(struct rwa_texture_decl, type) == 4 &&
               offsetof(struct rwa_texture_decl, type_known) == 8 &&
               offsetof(struct rwa_texture_decl, usage) == 12 &&
               offsetof(struct rwa_texture_decl, element) == 16 &&
               offsetof(struct rwa_texture_decl, element_bits) == 20 &&
               offsetof(struct rwa_texture_decl, metal_type) == 24,
               "rwa_texture_decl field order: six unsigneds then metal_type, or a copy reads fields off");
_Static_assert(RWA_ELEMENT_UNKNOWN == 0 && RWA_ELEMENT_FLOAT == 1 &&
               RWA_ELEMENT_INT == 2 && RWA_ELEMENT_UINT == 3,
               "rwa_element values are carried inside rwa_texture_decl.element across the wall");

/* The functions more than one consumer dlsyms, prototyped the way the library exports them
 * (air_metal.h line refs: device 62-66, texture 138-215, module_textures 276, encoded_members
 * 476). Each also has a typedef, because the consumers hold the dlsym'd entry points in
 * variables — the extern declaration is the authority (and the self-test TU's anchor), and the
 * _fn typedef is what an adopting consumer writes where it today spells the type out inline.
 * A consumer that calls something not listed here adds it the same way: prototype, typedef, and
 * the self-test TU keeps it honest. */
extern struct rwa_device *rwa_device_open(char *why, size_t why_size);
extern void rwa_device_close(struct rwa_device *device);
extern const char *rwa_device_name(struct rwa_device *device);
extern int rwa_texture_write(struct rwa_texture *texture, unsigned level, unsigned layer,
                             const unsigned origin[3], const unsigned size[3],
                             const void *bytes, size_t stride, char *why, size_t why_size);
extern int rwa_texture_read(struct rwa_texture *texture, unsigned level, unsigned layer,
                            const unsigned origin[3], const unsigned size[3],
                            void *bytes, size_t stride, char *why, size_t why_size);
extern void rwa_texture_destroy(struct rwa_texture *texture);
extern int rwa_module_textures(struct rwa_device *device, const void *bitcode, size_t size,
                               const char *entry, struct rwa_texture_decl *out, unsigned max,
                               unsigned *count, char *why, size_t why_size);
extern unsigned rwa_argbuf_encoded_members(const struct rwa_buffer *dest, size_t dest_offset);

typedef struct rwa_device *rwa_device_open_fn(char *, size_t);
typedef void rwa_device_close_fn(struct rwa_device *);
typedef const char *rwa_device_name_fn(struct rwa_device *);
typedef int rwa_texture_write_fn(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
                                 const unsigned[3], const void *, size_t, char *, size_t);
typedef int rwa_texture_read_fn(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
                                const unsigned[3], void *, size_t, char *, size_t);
typedef void rwa_texture_destroy_fn(struct rwa_texture *);
typedef int rwa_module_textures_fn(struct rwa_device *, const void *, size_t, const char *,
                                   struct rwa_texture_decl *, unsigned, unsigned *, char *,
                                   size_t);
typedef unsigned rwa_argbuf_encoded_members_fn(const struct rwa_buffer *, size_t);

/*
 * The self-test TU (host, zero device bytes; run it with the adoption batch and add it to the
 * host harness list check.sh takes). Both headers in one translation unit, with air_metal.h
 * FIRST so a duplicate definition is this file's fault to find — the typedef-to-the-same-
 * signature rule lets the typedefs coexist with the producer's declarations, and the lines
 * below then FAIL the compile the moment a mirror drifts: an assignment of every exported
 * function to its typedef'd pointer checks the full signature, and the asserts check layouts
 * and enum values from both sides of the wall:
 *
 *   cc -fsyntax-only -I tools/mesa/air -I userland/src -x c - <<'EOCHECK'
 *   #include "air_metal.h"
 *   #define REDWOOD_AIR_CONTRACT_TESTING
 *   #include "redwood_air_contract.h"
 *   rwa_texture_read_fn *c_read = &rwa_texture_read;
 *   rwa_texture_write_fn *c_write = &rwa_texture_write;
 *   rwa_device_open_fn *c_open = &rwa_device_open;
 *   rwa_device_close_fn *c_close = &rwa_device_close;
 *   rwa_device_name_fn *c_name = &rwa_device_name;
 *   rwa_texture_destroy_fn *c_destroy = &rwa_texture_destroy;
 *   rwa_module_textures_fn *c_modtex = &rwa_module_textures;
 *   rwa_argbuf_encoded_members_fn *c_members = &rwa_argbuf_encoded_members;
 *   EOCHECK
 *
 * (air_metal.h must compile standalone against nothing but the C standard library — it does
 * today; if that ever stops being true, this TU is where the fact becomes visible, which is
 * the other thing the self-test is for.)
 */
#endif /* REDWOOD_AIR_CONTRACT_H */
