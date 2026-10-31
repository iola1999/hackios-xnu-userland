/* Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/* Included by air_metal.c where this block was defined. */

/*
 * -------------------------------------------------------------------------------------
 * Depth and stencil state: MTLDepthStencilDescriptor in Gallium's terms.
 *
 * A created object, as it is in Metal (-newDepthStencilStateWithDescriptor:), and for
 * the reason a sampler is one here: the Gallium state is compiled once and bound at
 * every draw, and a compare function Metal has no name for must be refused where the
 * caller's own struct is still in hand.
 */

/* The state it was built from is not kept, as a sampler's is not: a refusal about a
 * depth/stencil state happens while it is being created, where the caller's struct is
 * still in hand, and what Metal asks of the state object afterwards (-readsDepth and
 * its three neighbours) is answered by the driver from the descriptor it still holds.
 */
struct rwa_depth_stencil {
   struct rwa_device *device;
   void *cso;
};

/* MTLCompareFunction, spelled out so a refusal names what the caller passed. */
static const char *
compare_function_name(unsigned f)
{
   static const char *const names[] = {
      "MTLCompareFunctionNever", "MTLCompareFunctionLess", "MTLCompareFunctionEqual",
      "MTLCompareFunctionLessEqual", "MTLCompareFunctionGreater",
      "MTLCompareFunctionNotEqual", "MTLCompareFunctionGreaterEqual",
      "MTLCompareFunctionAlways",
   };
   return f < ARRAY_SIZE(names) ? names[f] : NULL;
}

/*
 * MTLCompareFunction as PIPE_FUNC_x.
 *
 * The two enumerations happen to run in the same order, so this table is the identity
 * -- and it is written out for exactly that reason. A cast would be a bet that two
 * enumerations nobody here controls stay aligned; the table is a statement that they
 * are, checkable by reading eight lines. The sampler path's compare function is passed
 * through as a number today (sampler_state_from_metal) and nothing reads it, which is
 * why that one was not worth a table and this one is: a depth test reads it every
 * fragment.
 */
static bool
compare_function_from_metal(unsigned f, unsigned *out, const char *what, char *why,
                            size_t why_size)
{
   static const unsigned func[] = {
      PIPE_FUNC_NEVER, PIPE_FUNC_LESS, PIPE_FUNC_EQUAL, PIPE_FUNC_LEQUAL,
      PIPE_FUNC_GREATER, PIPE_FUNC_NOTEQUAL, PIPE_FUNC_GEQUAL, PIPE_FUNC_ALWAYS,
   };
   if (f >= ARRAY_SIZE(func)) {
      say(why, why_size, "%s is %u, which is not one of MTLCompareFunction's eight", what, f);
      return false;
   }
   *out = func[f];
   return true;
}

/*
 * MTLStencilOperation as PIPE_STENCIL_OP_x, and this one is NOT the identity: Metal
 * counts IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap, and
 * Gallium counts INCR, DECR, INCR_WRAP, DECR_WRAP, INVERT. Invert is 5 in one and 7 in
 * the other, and the two wrapping operations sit where Metal keeps Invert. A cast would
 * turn "invert the stencil" into "increment it and wrap", which is a picture and not a
 * failure.
 */
static bool
stencil_op_from_metal(unsigned op, unsigned *out, const char *what, char *why,
                      size_t why_size)
{
   static const unsigned ops[] = {
      PIPE_STENCIL_OP_KEEP,        /* MTLStencilOperationKeep */
      PIPE_STENCIL_OP_ZERO,        /* ...Zero */
      PIPE_STENCIL_OP_REPLACE,     /* ...Replace */
      PIPE_STENCIL_OP_INCR,        /* ...IncrementClamp */
      PIPE_STENCIL_OP_DECR,        /* ...DecrementClamp */
      PIPE_STENCIL_OP_INVERT,      /* ...Invert -- 5 here, PIPE's 7 */
      PIPE_STENCIL_OP_INCR_WRAP,   /* ...IncrementWrap */
      PIPE_STENCIL_OP_DECR_WRAP,   /* ...DecrementWrap */
   };
   if (op >= ARRAY_SIZE(ops)) {
      say(why, why_size, "%s is %u, which is not one of MTLStencilOperation's eight", what,
          op);
      return false;
   }
   *out = ops[op];
   return true;
}

/* Whether a face of MTLStencilDescriptor asks for anything at all. Metal has no
 * "stencil enabled" flag and Gallium needs one, so it is derived from what Metal's own
 * defaults are: compare Always with every operation Keep tests nothing and writes
 * nothing, which is exactly Metal's documented "effectively skips the test". Deriving
 * it is the only honest option -- a flag in the contract would be a field the Metal
 * driver has nothing to fill it in from.
 */
static bool
stencil_face_does_anything(const struct rwa_stencil_face *f)
{
   return f->compare_function != 7 /* MTLCompareFunctionAlways */ ||
          f->stencil_fail_operation != 0 || f->depth_fail_operation != 0 ||
          f->depth_stencil_pass_operation != 0;
}

static bool
stencil_face_from_metal(const struct rwa_stencil_face *in, struct pipe_stencil_state *out,
                        const char *face, char *why, size_t why_size)
{
   char what[64];
   unsigned func, fail, zfail, zpass;

   snprintf(what, sizeof(what), "the %s face's stencilCompareFunction", face);
   if (!compare_function_from_metal(in->compare_function, &func, what, why, why_size))
      return false;
   snprintf(what, sizeof(what), "the %s face's stencilFailureOperation", face);
   if (!stencil_op_from_metal(in->stencil_fail_operation, &fail, what, why, why_size))
      return false;
   snprintf(what, sizeof(what), "the %s face's depthFailureOperation", face);
   if (!stencil_op_from_metal(in->depth_fail_operation, &zfail, what, why, why_size))
      return false;
   snprintf(what, sizeof(what), "the %s face's depthStencilPassOperation", face);
   if (!stencil_op_from_metal(in->depth_stencil_pass_operation, &zpass, what, why, why_size))
      return false;

   out->func = func;
   out->fail_op = fail;
   out->zfail_op = zfail;
   out->zpass_op = zpass;
   /* Metal's masks are uint32 over an eight-bit buffer, and its own default is
    * 0xFFFFFFFF; Gallium's fields are the eight bits that can select anything. So the
    * high bits are dropped and not refused -- refusing Metal's own default would refuse
    * every descriptor an application makes.
    */
   out->valuemask = in->read_mask & 0xff;
   out->writemask = in->write_mask & 0xff;
   return true;
}

struct rwa_depth_stencil *
rwa_depth_stencil_create(struct rwa_device *device,
                         const struct rwa_depth_stencil_state *state, char *why,
                         size_t why_size)
{
   RWA_LOCKED;
   if (!state) {
      say(why, why_size, "a depth/stencil state with no descriptor");
      return NULL;
   }

   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   unsigned func;
   if (!compare_function_from_metal(state->depth_compare_function, &func,
                                    "depthCompareFunction", why, why_size))
      return NULL;

   /* depth_enabled is on whenever a state exists, and the compare function carries the
    * whole meaning. Metal has no enable bit: MTLCompareFunctionAlways with no write IS
    * its off state, and PIPE_FUNC_ALWAYS with depth_writemask 0 is the same thing to
    * both drivers here. One fewer derived flag is one fewer place for the two to
    * disagree.
    */
   dsa.depth_enabled = 1;
   dsa.depth_func = func;
   dsa.depth_writemask = state->depth_write_enabled ? 1 : 0;

   if (!stencil_face_from_metal(&state->front, &dsa.stencil[0], "front", why, why_size) ||
       !stencil_face_from_metal(&state->back, &dsa.stencil[1], "back", why, why_size))
      return NULL;

   /* Gallium's two enable bits are not symmetrical and that is the trap here:
    * stencil[0].enabled means "stencil testing at all" and stencil[1].enabled means
    * "the back face has its own state" (p_state.h says so in as many words). Metal
    * always carries two faces, so the second bit is on only when they differ --
    * otherwise a driver would emit two-sided state for a descriptor that asked for one
    * behaviour, which is not wrong but is not what was asked either.
    */
   dsa.stencil[0].enabled = stencil_face_does_anything(&state->front) ||
                            stencil_face_does_anything(&state->back);
   dsa.stencil[1].enabled = dsa.stencil[0].enabled &&
                            memcmp(&state->front, &state->back, sizeof(state->front)) != 0;

   struct rwa_depth_stencil *ds = calloc(1, sizeof(*ds));
   if (!ds) {
      say(why, why_size, "out of memory making a depth/stencil state");
      return NULL;
   }
   ds->device = device;
   ds->cso = device->pipe->create_depth_stencil_alpha_state(device->pipe, &dsa);
   if (!ds->cso) {
      say(why, why_size, "%s would not make a depth/stencil state of %s%s",
          rwa_device_name(device), compare_function_name(state->depth_compare_function),
          state->depth_write_enabled ? " writing depth" : " not writing depth");
      free(ds);
      return NULL;
   }
   return ds;
}

void
rwa_depth_stencil_destroy(struct rwa_depth_stencil *state)
{
   RWA_LOCKED;
   if (!state)
      return;
   /* Unbound before deleted, unlike a sampler and like a pipeline's own states: a draw
    * binds this one and leaves it bound, so the context is holding exactly this pointer
    * when the caller lets it go. softpipe's delete is a plain FREE
    * (sp_state_derived.c's bound pointer is not cleared for it), so deleting it bound
    * leaves the context pointing at freed memory until something else is bound over it
    * -- the same trap delete_compute_state is about.
    */
   state->device->pipe->bind_depth_stencil_alpha_state(state->device->pipe, NULL);
   state_forget(state->device);
   state->device->pipe->delete_depth_stencil_alpha_state(state->device->pipe, state->cso);
   free(state);
}
