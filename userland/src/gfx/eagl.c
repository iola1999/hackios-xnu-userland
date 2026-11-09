/*
 * libannibale-gl.dylib: an OpenGL ES implementation behind iOS's EAGLContext, inside the window
 * server (backboardd; libannibale.dylib loads this when the image has it: build.sh GL=1).
 *
 * CoreAnimation's render server draws with Metal, or else with OpenGL ES, or else with its own
 * software rasterizer (CA::WindowServer::EAGLServer::renderer()). For OpenGL ES it makes an
 * EAGLContext of API 3 and asks six things of it, all of which are replaced here:
 *
 *   -initWithAPI:properties:   ours makes a context of the OpenGL ES library behind us
 *   -API                       3
 *   -setParameter:to:          noted
 *   -getMacroContextPrivate    a table of functions, called as entry(table[0], arguments...) at
 *                              the offsets of iOS's own OpenGLES (src/eagl_slots.txt); ours is
 *                              made by mkeagl.py: each entry counts the call and passes it on,
 *                              except the ones written by hand below (my_gl...)
 *   -texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:
 *                              the texture bound to `target` is to show what the IOSurface holds
 *   -attachImage:toCoreSurface:invertedRender:
 *                              the texture bound to the target ("image") is to BE the IOSurface:
 *                              what is drawn into it (it becomes a framebuffer's colour
 *                              attachment) is the screen; or it is a picture to be shown
 *
 * A GPU's driver does the last two by using the IOSurface's memory as the texture's, and so it
 * is done here where the library can (annibale_gl_surface_texture(); surface_shared()): nothing
 * is copied, and a frame's end waits for the GPU. Where it cannot (a software driver, a surface
 * the GPU cannot address, a picture to be turned over) the texture is an ordinary one, filled
 * from the IOSurface when that has changed (its seed) and read back into it when a frame ends
 * (glFlush, glFinish), with nothing but OpenGL ES calls. Either way this file does not know
 * what draws: the library is reached through its gl* functions and the functions of annibale_gl.h.
 *
 * Where CoreAnimation relies on iOS's library being what it is, this stands in between:
 * blue-first pixels into a texture called GL_RGBA (internal_format()), shaders that read
 * textures at coordinates in pixels (my_glShaderSource()), and draws made with no program
 * (plain_draw_begin()).
 *
 * What CoreAnimation asks of OpenGL ES is written down as it goes (the log lines "annibale-gl:"):
 * strings and limits it reads, texture formats and sizes, calls and triangles per frame. More
 * with the kernel's boot-args: gltrace=FIRST-LAST writes down every call of those frames (lines
 * "gl:"; with each draw what it is made of -- the texture, the first corners -- and after it a
 * pixel or two of the framebuffer; gltrace_lit=1: and how much of the framebuffer is not black);
 * gltrace_at=SECONDS does so for the first frame that long after the window server started,
 * gltrace_draws=N for the first frame after one of N draws, gltrace_function=N[,K] for the frame
 * after the Kth draw CoreAnimation makes of its function N with no program, gltrace_frames=N for
 * N frames from any of them on; gltrace_place=X0,Y0,X1,Y1 writes down, of those frames, only the draws whose corners
 * reach into that rectangle, a line each (lines "gl-at:"; writing everything down is slow, and a
 * window server that is slow draws other frames), gltrace_look=1 adds the pixel each of them
 * left in the middle of itself; gltrace_shaders=1 writes down the text of the shaders of every
 * program a traced draw uses, the first time it draws, gltrace_uniforms=1 what every traced draw
 * gave its program, by the names the shaders use, gltrace_corners=N how many of a draw's corners
 * to write down (twelve by default); glcensus=1 keeps count of the draws CoreAnimation makes with no
 * program and of the functions it asks for in vain (see census_draw()); glshaders=1 writes down
 * the text of every shader, glshow=TEXTURE (or WxH, with glshow_part=W,H and glshow_zoom=N) puts that texture over the corner of the picture, glplain=0 leaves the
 * draws without a program alone, glskip=N[,N...] leaves out the draws of those programs (see skip_draw()), glcopy=1 copies every IOSurface as if the library could not
 * share one, glwait=1 waits for the GPU at the end of every frame and glwait=0 never (see
 * wait_for_frames), glfetch=0 keeps EXT_shader_framebuffer_fetch from CoreAnimation (see
 * fetch_offered), glgroup=0 leaves vibrant layers inside groups as they were (see group_draw_begin()), glprecision=0 leaves the shaders' "lowp" and "mediump" as they are (see all_highp), glenv=NAME=VALUE puts that into the environment for the library to find
 * (its own switches: FD_MESA_DEBUG=sysmem, say), vblur=0 blurs nothing whose radius varies and
 * vblur=2 paints such blurs by their masks instead (see vblur_on), glframes=1 writes a line for
 * every frame (see frame_end()).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "annibale_gl.h"

/* ---- OpenGL ES: the types, the few names used here, the library's functions --------------------- */

typedef unsigned int GLenum, GLbitfield, GLuint;
typedef int GLint, GLsizei, GLfixed, GLclampx;
typedef unsigned char GLboolean, GLubyte;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned short GLushort, GLhalf;
typedef float GLfloat, GLclampf;
typedef double GLdouble, GLclampd;
typedef char GLchar;
typedef void GLvoid;
typedef long GLintptr, GLsizeiptr;
typedef int64_t GLint64;
typedef uint64_t GLuint64;
typedef struct __GLsync *GLsync;
typedef void *GLeglImageOES;
typedef void (*GLDEBUGPROC)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
typedef GLDEBUGPROC GLDEBUGPROCKHR;

#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_TRIANGLE_FAN                 0x0006
#define GL_UNPACK_ROW_LENGTH            0x0cf2
#define GL_UNPACK_ALIGNMENT             0x0cf5
#define GL_PACK_ROW_LENGTH              0x0d02
#define GL_PACK_ALIGNMENT               0x0d05
#define GL_TEXTURE_2D                   0x0de1
#define GL_UNSIGNED_BYTE                0x1401
#define GL_RGBA                         0x1908
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_EXTENSIONS                   0x1f03
#define GL_TEXTURE_BINDING_2D           0x8069
#define GL_BGRA                         0x80e1
#define GL_INFO_LOG_LENGTH              0x8b84
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_SHADING_LANGUAGE_VERSION     0x8b8c
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_TEXTURE                      0x1702
#define GL_READ_FRAMEBUFFER             0x8ca8
#define GL_DRAW_FRAMEBUFFER             0x8ca9
#define GL_READ_FRAMEBUFFER_BINDING     0x8caa
#define GL_DRAW_FRAMEBUFFER_BINDING     0x8ca6
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_NEAREST                      0x2600
#define GL_LINES                        0x0001
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8cd0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8cd1
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_FRAMEBUFFER                  0x8d40
#define GL_SYNC_GPU_COMMANDS_COMPLETE   0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT      0x00000001
#define GL_TRUE                         1
#define GL_DEPTH_WRITEMASK              0x0b72
#define GL_COLOR_WRITEMASK              0x0c23

#define VBLUR_LEVELS    8               /* levels of a blur whose radius varies, the picture itself included */
#define VBLUR_CHAINS    6               /* ... such blurs kept track of at a time */

struct eagl;
static int eagl_tracing;                /* this frame's calls are written down (boot-arg gltrace=FIRST-LAST) */
static int frame_lines;                 /* boot-arg glframes=1: a line for every frame (see frame_end()) */
static int latency_lines;               /* boot-arg touch_lat=1: when every frame began and ended (see frame_end()) */
static uint64_t slow_calls_us;          /* boot-arg glslow=US: a line for every call that takes longer (eagl_leave()) */
static uint64_t call_began_us;          /* (when the call in progress began) */
static uint64_t now_us(void);
static void eagl_trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void eagl_trace_floats(const float *values, int count);
static void eagl_enter(struct eagl *e, unsigned offset);
static void eagl_leave(struct eagl *e, unsigned offset);
static long eagl_missing(void *context, unsigned offset);
#define EAGL_ENTER(context, offset)     eagl_enter(context, offset)
#define EAGL_LEAVE(context, offset)     eagl_leave(context, offset)

#define EAGL_GEN_DECLARATIONS
#include "eagl_gen.h"
#undef EAGL_GEN_DECLARATIONS

static void
say(const char *format, ...)
{
	char line[1024];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

static void
eagl_trace(const char *format, ...)
{
	char line[512] = "gl: ";
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line + 4, sizeof(line) - 4, format, ap) + 4;
	va_end(ap);
	write(2, line, n > (int)sizeof(line) - 1 ? sizeof(line) - 1 : (size_t)n);
}

static void
eagl_trace_floats(const float *values, int count)
{
	char line[512] = "gl:    ";
	int n = 7;

	for (int i = 0; i < count && i < 16 && n < 480; i++) {
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %g", (double)values[i]);
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
}

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000;
}

/* ---- a context -------------------------------------------------------------------------------- */

#define SLOTS           0x440           /* entries of the table: more than iOS's has (checked below) */

/* (a vibrant layer inside a group: see group_draw_begin()) */
#define GROUP_TEXTURES  8192
enum { GROUP_NONE, GROUP_FRESH, GROUP_VIBRANT };
struct group_texture {
	uint8_t state;                  /* GROUP_FRESH: cleared, nothing drawn in it since; GROUP_VIBRANT: a vibrant layer's coverage */
	GLfloat colours[20];            /* ... and that layer's colour matrix */
};
static int group_vibrant_on = 1;        /* boot-arg glgroup=0 */
static unsigned group_covered, group_laid, group_flattened;    /* (glframes=1) this frame's */

/* A texture that stands for an IOSurface. */
struct image {
	GLuint texture;
	IOSurfaceRef surface;
	bool target;                    /* drawn into (attachImage) rather than shown (texImageIOSurface) */
	bool invert, drawn;
	bool shared;                    /* the texture is the surface's memory: nothing to copy */
	GLuint mirror;                  /* a texture that is the surface's memory, which the GPU copies the frame into (blit_targets) */
	bool wrapped;                   /* the surface is memory of somebody's with a surface made around it */
	bool alpha_first;               /* its pixels are opacity, blue, green, red in memory: see my_attachImage() */
	bool painted;                   /* something has been drawn into it (it is a target in fact, not only in name) */
	unsigned looked;                /* ... the frame in which that memory was last looked at, */
	uint64_t sum;                   /* ... and what it added up to */
	GLenum to, internal, format, type;
	uint32_t plane, seed;
	GLsizei width, height;
};

struct eagl {
	void *table[SLOTS];             /* [0] is the context itself: what every entry gets as its first argument */
	id object;                      /* the EAGLContext */
	struct annibale_gl *gl;
	unsigned long api;
	struct image images[256];
	unsigned image_count;
	GLuint framebuffer;             /* the one bound */
	GLuint colour;                  /* ... and the texture that is its colour attachment (0: none, or no texture) */
	GLuint reader;                  /* a framebuffer of ours, to read a texture back */
	GLuint writer;                  /* ... and one to copy a frame into its surface with (blit_targets) */
	GLuint program;                 /* the one in use, as CoreAnimation has it */
	GLuint plain, lookup;           /* programs of ours for when that is none (plain_draw_begin()) */
	GLint plain_matrix, plain_textured, lookup_matrix;
	GLuint vibrant, masking;        /* two more: what a colour matrix does to what is behind, and to the layer */
	int blended;                    /* blending is off for such a draw, to be put back after it */
	bool no_vibrant;                /* the first of the two could not be made */
	GLint vibrant_matrix, vibrant_colours, masking_matrix, masking_colours;
	GLbitfield unclear;             /* a glClear put off until the next draw (clear_now()), and what it was to be made with */
	GLint unclear_box[4];
	GLfloat unclear_colour[4];
	GLboolean unclear_boxed;
	struct group_texture *groups;   /* CoreAnimation's textures: which are groups with a vibrant layer in them (group_draw_begin()) */
	GLuint group_cover, group_over, group_flat;     /* ... and our programs for them */
	bool group_failed;
	GLint group_cover_matrix, group_cover_texmat, group_over_matrix, group_over_texmat, group_over_colours;
	GLint group_flat_matrix, group_flat_colour;
	GLboolean group_blend;          /* the blending a draw of those had, to be put back after it */
	GLint group_blend_func[4];
	GLuint box;                     /* and one that makes a picture eight times smaller */
	GLint box_matrix, box_places;
	GLuint blur4;                   /* and for the four samples of a shadow's blur (blur4_fragment) */
	GLint blur4_matrix, blur4_places;
	GLuint halve, vblur;            /* and two for the blur whose radius varies (vblur_level(), vblur_draw_begin()) */
	GLint halve_texel, vblur_matrix, vblur_radius, vblur_levels, vblur_normalize, vblur_paint;
	struct vblur_chain {
		GLuint picture;         /* CoreAnimation's texture, the first level (0: unused) */
		GLsizei width, height;
		GLuint level[VBLUR_LEVELS], framebuffer[VBLUR_LEVELS];  /* ours, from the second on (index 1 up) */
		GLsizei level_width[VBLUR_LEVELS], level_height[VBLUR_LEVELS];
		int made;               /* how many levels are made of the present picture, the first included */
		unsigned used;          /* the frame it was last used in */
	} chains[VBLUR_CHAINS];
	bool vblur_bound;               /* a draw of ours has textures of its own on units 2 to 5 */
	unsigned unit;                  /* the texture unit being spoken of */
	GLuint bound[8];                /* the 2D texture bound to each of the first units */
	const GLfloat *matrix;          /* where CoreAnimation keeps the matrix it gives its programs */
	struct {
		const void *pointer;
		GLsizei stride;
		GLint size;
		GLenum type;
	} arrays[8];                    /* the vertex arrays, as given */
	bool in_frame;
	struct {                        /* what the line a frame gets with glframes=1 says (frame_end()) */
		bool taken, boxed;      /* the first draw or clear into the display's picture seen; scissored */
		GLint box[4];
		uint32_t surface;       /* that picture's IOSurface */
		unsigned vrd, vrb, into_display;
		unsigned solid_spans, blended_spans;    /* line draws into it with blending off and on (glyphs' spans) */
		unsigned solid_ends, blended_ends;      /* ... and their lines' ends */
		unsigned other_spans, other_ends;       /* line draws into other targets */
	} line;
	/* a frame's worth */
	unsigned calls, draws, triangles, uploads, clears, last_draws;
	uint64_t upload_bytes, began, attaching, flushing;
	/* everything so far */
	unsigned frames, shaders, programs, errors, shared, refused;
	uint64_t all_calls, all_draws, all_triangles, readback_us, wait_us, attach_us, flush_us, frame_us;
	unsigned most_calls, most_triangles;
	const char *sources[256];       /* the text of the shaders being made, by name (modulo this) */
	bool fragment[256];             /* ... and which of the two kinds each is */
	unsigned plain_said;
};

static unsigned trace_first, trace_last;        /* the frames whose calls are written down */
static uint64_t trace_at;                       /* ... or the first frame from this time on (now_us()) */
static unsigned trace_draws;                    /* ... or the first frame after one of so many draws */
static unsigned trace_frames = 1, trace_left;   /* ... and how many frames from there on */
static int trace_lit;                           /* boot-arg gltrace_lit=1: count the lit pixels after every traced draw (slow) */
static int trace_function = -1, trace_function_after = 1;       /* boot-arg gltrace_function=N[,K]: the frames after the Kth draw ... */
static int trace_function_met;                                  /* ... CoreAnimation makes of its function N with no program */
static float trace_place[4];                    /* boot-arg gltrace_place=X0,Y0,X1,Y1: of those frames, only the draws that touch this */
static int trace_placed, trace_placing;         /* ... is set; ... and this frame is one of them */
static int trace_look;                          /* boot-arg gltrace_look=1: ... and what each of them left in the middle of itself */
static int trace_shaders;                       /* boot-arg gltrace_shaders=1: the text of the shaders of every program that draws */
static int trace_uniforms_on;                   /* boot-arg gltrace_uniforms=1: and what every traced draw gave its program */
static GLsizei trace_corners = 12;              /* boot-arg gltrace_corners=N: how many corners of a traced draw to write down */
static float trace_middle[2];                   /* ... which is here, as CoreAnimation counts */
static int trace_listed;                        /* the draw at hand is one of them */
static int all_shaders;                         /* boot-arg glshaders=1: the text of every shader, not of the first two */
static struct group_texture *group_of(struct eagl *e, GLuint texture);
static __thread struct eagl *current;
static struct eagl *contexts[8];
static uint32_t slot_calls[SLOTS];
static int check_errors = 1;            /* ask the library for an error after every call (the first frames) */
static int share_surfaces = 1;          /* boot-arg glcopy=1: 0 */
/*
 * A frame is not drawn into its surface's memory but into a texture of the library's own, and the
 * GPU copies it into the surface at the frame's end (one blit; the display's driver waits for that
 * submission, as it waited for the frame's last draw). Drawn into the surface's memory directly,
 * frames came out with parts of them missing, every other frame for a while -- Control Center's
 * modules, the home screen's icons -- where CoreAnimation reads the picture it is drawing to draw
 * over it (EXT_shader_framebuffer_fetch, the colour matrix over what is behind: our vibrant program);
 * a texture of the library's own does not show that, as a copy by the processor did not (glcopy=1).
 * Boot-arg glblit=0: the surface's memory, as before.
 */
static int blit_targets = 1;
/*
 * A frame that is handed to the display must have been drawn. The display's driver waits for
 * the GPU itself before it puts a surface on the panel, on a thread of its own, and says so in
 * the registry ("AnnibaleWaitsForGPU"); the window server is then at its next frame meanwhile.
 * With a driver that does not, the frame is waited for here (glFinish). Boot-arg glwait=1:
 * here in any case; glwait=0: nowhere, to see what the waiting costs (and frames half drawn).
 */
static int wait_for_frames = 1;
static int sync_frames = 1;             /* boot-arg glsync=0 (a test): a frame's end does not push what Mesa holds back (frame_end()) */
/*
 * EXT_shader_framebuffer_fetch -- a fragment shader reads the pixel it is about to write -- is
 * what CoreAnimation draws a number of things with where the library offers it: the shadows
 * under the home screen's labels, when they are of the kind that darkens only what is light,
 * are quads that read the picture and write it back, and a colour matrix over what is there
 * (the Search field's text) is one draw instead of a copy and a draw. Without the extension
 * CoreAnimation does the same with what OpenGL ES always has, at four times the submissions
 * for a home screen. (Mesa's driver for this GPU drew the first triangle of each such quad
 * with pixels of another tile, fragments of other icons under the labels, until it was given a
 * flush before the first draw that reads the picture after one that does not:
 * tools/mesa/freedreno-darwin.patch, fd6_draw.cc.) Boot-arg glfetch=0 keeps the extension from
 * CoreAnimation, to see the difference.
 */
static int fetch_offered = 1;

#define GL_SCISSOR_TEST                 0x0c11
#define GL_SCISSOR_BOX                  0x0c10
#define GL_COLOR_CLEAR_VALUE            0x0c22

static const char *eagl_slot_name(unsigned offset);
static void census_report(void);
static void clear_now(struct eagl *e);

static void
eagl_enter(struct eagl *e, unsigned offset)
{
	if (current != e) {
		if (!annibale_gl_make_current(e->gl)) {
			say("annibale-gl: the context cannot be made this thread's\n");
		}
		current = e;
	}
	slot_calls[offset / 8]++;
	e->calls++;
	if (slow_calls_us != 0) {
		call_began_us = now_us();
	}
}

static void
eagl_leave(struct eagl *e, unsigned offset)
{
	GLenum error;

	if (check_errors && offset != 0x338 && (error = glGetError()) != 0) {
		if (e->errors++ < 60) {
			say("annibale-gl: %s: error %#x\n", eagl_slot_name(offset), error);
		}
	}
	if (slow_calls_us != 0) {
		uint64_t now = now_us();

		/* "tl: slow NAME US at US, frame N": a call that took long, and when it ended (tools/frames.py) */
		if (now - call_began_us >= slow_calls_us) {
			say("tl: slow %s %llu at %llu, frame %u%s\n", eagl_slot_name(offset), now - call_began_us, now, e->frames + 1,
			    e->in_frame ? "" : " (before its first draw)");
		}
	}
}

/* An entry the library has no function for: say so, once an entry. */
static long
eagl_missing(void *context, unsigned offset)
{
	struct eagl *e = context;

	eagl_enter(e, offset);
	if (slot_calls[offset / 8] == 1) {
		say("annibale-gl: CALLED AND NOT THERE: entry %#x, %s\n", offset, eagl_slot_name(offset));
	}
	return 0;
}

static struct eagl *
context_of(id object)
{
	for (unsigned i = 0; i < sizeof(contexts) / sizeof(contexts[0]); i++) {
		if (contexts[i] != NULL && contexts[i]->object == object) {
			return contexts[i];
		}
	}
	return NULL;
}

/* ---- what CoreAnimation asks for: kept for the summary ------------------------------------------- */

struct format_use {
	GLenum internal, format, type;
	unsigned count, widest, tallest;
	const char *how;
};
static struct format_use formats[48];
static unsigned widest_texture, tallest_texture;

static void
note_format(const char *how, GLenum internal, GLenum format, GLenum type, GLsizei width, GLsizei height)
{
	struct format_use *use = NULL;

	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (formats[i].count == 0 || (formats[i].how == how && formats[i].internal == internal &&
		    formats[i].format == format && formats[i].type == type)) {
			use = &formats[i];
			break;
		}
	}
	if ((unsigned)width > widest_texture) {
		widest_texture = (unsigned)width;
	}
	if ((unsigned)height > tallest_texture) {
		tallest_texture = (unsigned)height;
	}
	if (use == NULL) {
		return;
	}
	if (use->count++ == 0) {
		use->how = how;
		use->internal = internal;
		use->format = format;
		use->type = type;
		say("annibale-gl: a new kind of texture: %s, internal format %#x, format %#x, type %#x, %d x %d\n", how, internal,
		    format, type, width, height);
	}
	if ((unsigned)width > use->widest) {
		use->widest = (unsigned)width;
	}
	if ((unsigned)height > use->tallest) {
		use->tallest = (unsigned)height;
	}
}

static void
summary(struct eagl *e)
{
	unsigned frames = e->frames ? e->frames : 1;

	say("annibale-gl: after %u frames: %llu calls, %llu draws and %llu triangles a frame on average; the most in one: "
	    "%u calls, %u triangles; %llu us a frame, of which %llu handing the commands on, %llu waiting for the picture "
	    "to be drawn and %llu copying it out, and before each %llu making textures of IOSurfaces; %u IOSurfaces were "
	    "the textures' memory, %u had to be copied; %u shaders, %u programs; biggest texture %u x %u; %u errors\n",
	    e->frames, e->all_calls / frames, e->all_draws / frames, e->all_triangles / frames, e->most_calls,
	    e->most_triangles, e->frame_us / frames, e->flush_us / frames, e->wait_us / frames, e->readback_us / frames,
	    e->attach_us / frames, e->shared, e->refused, e->shaders, e->programs, widest_texture, tallest_texture, e->errors);
	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]) && formats[i].count; i++) {
		say("annibale-gl:   textures: %s %#x/%#x/%#x: %u times, up to %u x %u\n", formats[i].how, formats[i].internal,
		    formats[i].format, formats[i].type, formats[i].count, formats[i].widest, formats[i].tallest);
	}
	for (unsigned offset = 8; offset < SLOTS * 8; offset += 8) {
		if (slot_calls[offset / 8]) {
			say("annibale-gl:   %-28s %u\n", eagl_slot_name(offset), slot_calls[offset / 8]);
		}
	}
}

/* ---- IOSurfaces ----------------------------------------------------------------------------------- */

static struct image *
image_of(struct eagl *e, GLuint texture)
{
	for (unsigned i = 0; i < e->image_count; i++) {
		if (e->images[i].texture == texture) {
			return &e->images[i];
		}
	}
	return NULL;
}

static struct image *
image_new(struct eagl *e, GLuint texture, IOSurfaceRef surface)
{
	struct image *image = image_of(e, texture);

	if (image == NULL) {
		if (e->image_count == sizeof(e->images) / sizeof(e->images[0])) {
			say("annibale-gl: more textures with IOSurfaces than there is room for\n");
			return NULL;
		}
		image = &e->images[e->image_count++];
	} else {
		if (image->mirror != 0) {
			glDeleteTextures(1, &image->mirror);
		}
		CFRelease(image->surface);
	}
	memset(image, 0, sizeof(*image));
	image->texture = texture;
	image->surface = (IOSurfaceRef)CFRetain(surface);
	return image;
}

static void
image_forget(struct eagl *e, GLuint texture)
{
	struct image *image = image_of(e, texture);

	if (image != NULL) {
		if (image->mirror != 0) {
			glDeleteTextures(1, &image->mirror);
		}
		CFRelease(image->surface);
		*image = e->images[--e->image_count];
	}
}

/* Apple's OpenGL ES takes blue-first pixels into a texture called GL_RGBA; everybody else's calls the texture so too. */
static GLenum
internal_format(GLenum internal, GLenum format)
{
	return format == GL_BGRA && internal == GL_RGBA ? GL_BGRA : internal;
}

static unsigned
bytes_a_pixel(GLenum format, GLenum type)
{
	unsigned parts = format == GL_RGBA || format == GL_BGRA ? 4 : format == 0x1907 ? 3 :        /* RGB */
	    format == 0x8227 || format == 0x190a ? 2 : 1;                                           /* RG, luminance + alpha */

	switch (type) {
	case GL_UNSIGNED_BYTE:
		return parts;
	case 0x140b: case 0x8d61: case 0x1403:          /* half floats, 16 bits */
		return 2 * parts;
	case 0x1406:                                    /* floats */
		return 4 * parts;
	case 0x8368:                                    /* 2:10:10:10 in a word */
		return 4;
	default:
		return 2;                               /* the packed 16-bit kinds */
	}
}

/*
 * For a surface drawn into (blit_targets): a second texture that is the surface's memory, for the
 * frame to be copied into by the GPU; the texture CoreAnimation draws into stays the library's own.
 * The texture that is bound is bound again after.
 */
static bool
surface_mirrored(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	GLint bound = 0;

	if (!blit_targets || !share_surfaces || !image->target || image->alpha_first ||
	    image->format != GL_BGRA || image->type != GL_UNSIGNED_BYTE || image->plane != 0 ||
	    (size_t)image->width != IOSurfaceGetWidth(surface) || (size_t)image->height != IOSurfaceGetHeight(surface)) {
		return false;
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	glGenTextures(1, &image->mirror);
	glBindTexture(GL_TEXTURE_2D, image->mirror);
	if (!annibale_gl_surface_texture(e->gl, IOSurfaceGetID(surface))) {
		glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
		glDeleteTextures(1, &image->mirror);
		image->mirror = 0;
		return false;
	}
	glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	e->shared++;
	return true;
}

/*
 * Make a texture, which is bound, of its IOSurface's own memory, if that is what is asked for
 * (the whole of a surface of blue-first pixels, the same way up) and the library can do it.
 */
static bool
surface_shared(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;

	if (!share_surfaces || image->invert || image->alpha_first || image->format != GL_BGRA || image->type != GL_UNSIGNED_BYTE ||
	    image->plane != 0 || (size_t)image->width != IOSurfaceGetWidth(surface) ||
	    (size_t)image->height != IOSurfaceGetHeight(surface) ||
	    !annibale_gl_surface_texture(e->gl, IOSurfaceGetID(surface))) {
		e->refused++;
		return false;
	}
	image->shared = true;
	e->shared++;
	return true;
}

/*
 * A surface that is shown goes on being drawn into after the texture has been made of it -- a
 * sign an application draws for one of its layers, a picture CoreAnimation has made a surface
 * around -- and not everybody who draws says so: often no seed moves, and the texture is not
 * made again. A GPU that has the memory itself shows what is there. A copy has to notice: once
 * a frame, when the texture is about to be drawn with, the memory is added up, and copied again
 * if the sum has changed.
 */
static bool
surface_changed(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	size_t size = IOSurfaceGetBytesPerRow(surface) * (size_t)(image->height - 1) +
	    (size_t)image->width * bytes_a_pixel(image->format, image->type);
	const uint8_t *base = IOSurfaceGetBaseAddress(surface);
	uint64_t sum = size, word;

	if (image->looked == e->frames + 1 || base == NULL || IOSurfaceGetPlaneCount(surface) > 0) {
		return false;
	}
	image->looked = e->frames + 1;
	for (size_t at = 0; at + 8 <= size; at += 8) {
		memcpy(&word, base + at, 8);
		sum = (sum ^ word) * 0x9e3779b97f4a7c15ull;
		sum ^= sum >> 29;
	}
	if (sum == image->sum) {
		return false;
	}
	image->sum = sum;
	return true;
}

/* Fill a texture, which is bound, from its IOSurface, unless it is the surface. */
static void
surface_to_texture(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	bool planar = IOSurfaceGetPlaneCount(surface) > 0;
	size_t row = planar ? IOSurfaceGetBytesPerRowOfPlane(surface, image->plane) : IOSurfaceGetBytesPerRow(surface);
	unsigned pixel = bytes_a_pixel(image->format, image->type);
	GLint row_length = 0, alignment = 4;
	uint8_t *base, *flipped = NULL;
	bool alpha_first = image->alpha_first && !planar && pixel == 4;

	if (image->shared) {
		return;
	}
	if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != 0) {
		say("annibale-gl: an IOSurface cannot be locked\n");
		return;
	}
	base = planar ? IOSurfaceGetBaseAddressOfPlane(surface, image->plane) : IOSurfaceGetBaseAddress(surface);
	if ((image->invert || alpha_first) && (flipped = calloc(row, (size_t)image->height)) != NULL) {
		for (GLsizei y = 0; y < image->height; y++) {
			const uint8_t *from = base + row * (size_t)(image->invert ? image->height - 1 - y : y);
			uint8_t *to = flipped + row * (size_t)y;
			size_t used = (size_t)image->width * pixel;     /* (the last row may end where its pixels do) */

			if (!alpha_first) {
				memcpy(to, from, used < row ? used : row);
				continue;
			}
			/* opacity, blue, green, red in memory: into the order OpenGL ES was told */
			for (size_t at = 0; at + 4 <= used && at + 4 <= row; at += 4) {
				to[at] = from[at + (image->format == GL_BGRA ? 1 : 3)];
				to[at + 1] = from[at + 2];
				to[at + 2] = from[at + (image->format == GL_BGRA ? 3 : 1)];
				to[at + 3] = from[at];
			}
		}
		base = flipped;
	}
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(row / pixel));
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(image->to, 0, (GLint)internal_format(image->internal, image->format), image->width, image->height, 0,
	    image->format, image->type, base);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
	glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
	if (trace_placing) {
		size_t set[4] = { 0, 0, 0, 0 }, size = row * (size_t)(image->height - 1) + (size_t)image->width * pixel;

		for (size_t at = 0; at < size; at++) {
			set[at & 3] += base[at] != 0;
		}
		say("gl-at: frame %u: texture %u filled from surface %u (pixel format %#x, %d x %d, %zu bytes a row, %zu a pixel by the "
		    "surface, %u by the texture: internal format %#x, format %#x, type %#x): of %zu bytes as they go to the texture, "
		    "not zero: %zu %zu %zu %zu (first, second, third, fourth of every four)\n", e->frames + 1, image->texture,
		    IOSurfaceGetID(surface),
		    (unsigned)IOSurfaceGetPixelFormat(surface), image->width, image->height, row,
		    IOSurfaceGetBytesPerElement(surface), pixel, image->internal, image->format, image->type, size, set[0], set[1],
		    set[2], set[3]);
		if (image->height == 1 && pixel == 2 && image->width >= 16) {
			char entries[200] = "";
			size_t used = 0;

			for (int i = 0; i < 16; i++) {
				size_t at = (size_t)(i * (image->width - 1) / 15) * 2;

				used += (size_t)snprintf(entries + used, sizeof(entries) - used, " %u/%u", base[at], base[at + 1]);
			}
			say("gl-at:     a table; sixteen of its entries, first to last:%s\n", entries);
		}
	}
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	free(flipped);
	image->seed = IOSurfaceGetSeed(surface);
	e->uploads++;
	e->upload_bytes += row * (size_t)image->height;
}

/* What has been drawn into a texture, into its IOSurface, unless it is there (frame_end() waits for it). */
static void
target_to_surface(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	size_t row = IOSurfaceGetBytesPerRow(surface);
	uint8_t *base;

	image->drawn = false;
	if (image->mirror != 0) {
		GLint read = 0, draw = 0;

		/* the frame into the surface's memory, by the GPU (blit_targets); upside down if it is drawn so */
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
		if (e->reader == 0) {
			glGenFramebuffers(1, &e->reader);
		}
		if (e->writer == 0) {
			glGenFramebuffers(1, &e->writer);
		}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, e->reader);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->texture, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->writer);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->mirror, 0);
		glBlitFramebuffer(0, 0, image->width, image->height, 0, image->invert ? image->height : 0, image->width,
		    image->invert ? 0 : image->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
		if (IOSurfaceLock(surface, 0, NULL) == 0) {
			IOSurfaceUnlock(surface, 0, NULL);
		}
		return;
	}
	if (image->shared) {
		/* whoever looks at the surface's seed is to see that it has changed */
		if (IOSurfaceLock(surface, 0, NULL) == 0) {
			IOSurfaceUnlock(surface, 0, NULL);
		}
		return;
	}
	if (e->reader == 0) {
		glGenFramebuffers(1, &e->reader);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->reader);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->texture, 0);
	if (IOSurfaceLock(surface, 0, NULL) == 0) {
		base = IOSurfaceGetBaseAddress(surface);
		glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(row / 4));
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		if (!image->invert) {
			glReadPixels(0, 0, image->width, image->height, GL_BGRA, GL_UNSIGNED_BYTE, base);
		} else {
			for (GLsizei y = 0; y < image->height; y++) {
				glReadPixels(0, y, image->width, 1, GL_BGRA, GL_UNSIGNED_BYTE, base + row * (size_t)(image->height - 1 - y));
			}
		}
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		IOSurfaceUnlock(surface, 0, NULL);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
}

/*
 * For looking at what is drawn on the way to the picture (boot-arg glshow=TEXTURE): that texture's
 * pixels are put over the top left corner of the picture as it goes out. glshow=WxH takes, instead
 * of a name that is a new one every run, whichever texture is of that size; glshow_part=W,H only
 * that much of its corner and glshow_zoom=N every pixel of it as a square of N, for a texture
 * whose few pixels in a corner are what matters (a material's backdrop, say).
 */
#define GL_TEXTURE_WIDTH        0x1000
#define GL_TEXTURE_HEIGHT       0x1001

static GLuint shown_texture;
/*
 * Boot-arg glskip=N[,N...]: the draws of those programs are left out of the picture, which says
 * what in it is their work. A program's number is the same from run to run, because CoreAnimation
 * builds its programs in the same order every time (the trace prints it: "program 111").
 */
static GLuint skipped[8];
static unsigned skipping;

static bool
skip_draw(struct eagl *e)
{
	for (unsigned i = 0; i < skipping; i++) {
		if (skipped[i] == e->program) {
			static GLuint told[8];
			static unsigned telling;

			for (unsigned k = 0; k < telling; k++) {
				if (told[k] == e->program) {
					return true;
				}
			}
			if (telling < sizeof(told) / sizeof(told[0])) {
				told[telling++] = e->program;
			}
			say("annibale-gl: the draws of program %u are left out\n", e->program);
			return true;
		}
	}
	return false;
}

static GLint shown_wanted[2];                   /* boot-arg glshow=WxH: the size to look for instead */
static GLint shown_part[2];                     /* boot-arg glshow_part=W,H: that much of its corner */
static GLint shown_zoom = 1;                    /* boot-arg glshow_zoom=N */

static GLuint shown_told;                       /* (the one written down as being shown) */

/* What sizes the textures are, when the one asked for is not among them. */
static void
say_sizes(void)
{
	GLint bound = 0;
	char line[600] = "";
	size_t used = 0;

	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	for (GLuint name = 1; name < 512 && used < sizeof(line) - 24; name++) {
		GLint width = 0, height = 0;

		if (!glIsTexture(name)) {
			continue;
		}
		glBindTexture(GL_TEXTURE_2D, name);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		used += (size_t)snprintf(line + used, sizeof(line) - used, " %u:%dx%d", name, width, height);
	}
	glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	say("annibale-gl: glshow: the textures there are:%s\n", line);
}

/* glshow=WxH: the first texture of that size there is (names are handed out afresh every run). */
static GLuint
texture_of_size(GLint want_width, GLint want_height)
{
	GLint bound = 0;
	GLuint found = 0;

	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	for (GLuint name = 1; name < 512 && found == 0; name++) {
		GLint width = 0, height = 0;

		if (!glIsTexture(name)) {
			continue;
		}
		glBindTexture(GL_TEXTURE_2D, name);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		/* within a few pixels: the size of a backdrop's texture is a pixel or two different every run */
		if (width - want_width <= 8 && want_width - width <= 8 && height - want_height <= 8 && want_height - height <= 8) {
			found = name;
		}
	}
	glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	return found;
}

static void
show_texture(struct eagl *e, struct image *image)
{
	GLint width = 0, height = 0, bound = 0;
	size_t row = IOSurfaceGetBytesPerRow(image->surface);
	uint8_t *pixels, *base;
	GLuint texture = shown_texture;

	if (shown_wanted[0] > 0 && (texture = texture_of_size(shown_wanted[0], shown_wanted[1])) == 0) {
		static unsigned said;

		if (said++ == 0) {
			say("annibale-gl: glshow: there is no texture of %d x %d\n", shown_wanted[0], shown_wanted[1]);
			say_sizes();
		}
		return;
	}
	if (shown_told != texture) {
		shown_told = texture;
		say("annibale-gl: glshow: the texture %u, %d of it by %d, every pixel a square of %d\n", texture,
		    shown_part[0] > 0 ? shown_part[0] : -1, shown_part[1] > 0 ? shown_part[1] : -1, shown_zoom);
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	glBindTexture(GL_TEXTURE_2D, texture);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	if (shown_part[0] > 0) {
		width = shown_part[0] < width ? shown_part[0] : width;
		height = shown_part[1] < height ? shown_part[1] : height;
	}
	if (width <= 0 || height <= 0 || (pixels = malloc((size_t)width * (size_t)height * 4)) == NULL) {
		return;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->reader);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
	if (glGetError() == 0 && IOSurfaceLock(image->surface, 0, NULL) == 0) {
		base = IOSurfaceGetBaseAddress(image->surface);
		for (GLint y = 0; y < height * shown_zoom && y < image->height; y++) {
			const uint8_t *from = pixels + (size_t)(y / shown_zoom) * (size_t)width * 4;
			uint8_t *to = base + row * (size_t)y;

			for (GLint x = 0; x < width * shown_zoom && x < image->width; x++) {
				memcpy(to + 4 * (size_t)x, from + 4 * (size_t)(x / shown_zoom), 4);
			}
		}
		IOSurfaceUnlock(image->surface, 0, NULL);
	}
	free(pixels);
}

/* Before the first drawing of a frame: the textures whose IOSurfaces somebody has written to since. */
static void
frame_begin(struct eagl *e)
{
	GLint bound = 0;
	bool rebound = false;

	if (e->in_frame) {
		return;
	}
	e->in_frame = true;
	e->began = now_us();
	eagl_tracing = e->frames + 1 >= trace_first && e->frames + 1 <= trace_last;
	if ((trace_at != 0 && e->began >= trace_at) || (trace_draws != 0 && e->last_draws == trace_draws) || trace_function_met) {
		trace_left = trace_frames;      /* (once) */
		trace_at = 0;
		trace_draws = 0;
		trace_function_met = 0;
		trace_function = -1;
	}
	trace_placing = 0;
	if (trace_left != 0) {
		trace_left--;
		eagl_tracing = !trace_placed;
		trace_placing = trace_placed;
	} else if (eagl_tracing && trace_placed) {
		/* (gltrace=FIRST-LAST with gltrace_place: a line a draw for those frames too) */
		eagl_tracing = 0;
		trace_placing = 1;
	}
	if (eagl_tracing) {
		eagl_trace("---- frame %u (framebuffer %u)\n", e->frames + 1, e->framebuffer);
	}
	for (unsigned i = 0; i < e->image_count; i++) {
		struct image *image = &e->images[i];

		if (image->target || image->shared || IOSurfaceGetSeed(image->surface) == image->seed) {
			continue;
		}
		if (!rebound) {
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
			rebound = true;
		}
		glBindTexture(image->to, image->texture);
		surface_to_texture(e, image);
	}
	if (rebound) {
		glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	}
}

/*
 * glFlush, glFinish: what was drawn must be where the display takes it from. `finished`: the
 * library has been waited for already.
 */
static void frame_functions(struct eagl *e);
static void
frame_end(struct eagl *e, bool finished)
{
	static unsigned unfollowed;
	uint64_t before, waited, after;
	unsigned copied = 0, in_place = 0;

	clear_now(e);
	before = now_us();
	for (unsigned i = 0; i < e->image_count; i++) {
		if (e->images[i].target && e->images[i].drawn && e->images[i].mirror != 0) {
			target_to_surface(e, &e->images[i]);    /* (the GPU's copy: before what follows sends it out) */
			if (shown_texture != 0) {
				show_texture(e, &e->images[i]);
			}
			in_place++;
			glFlush();                              /* (also after a glFinish: the copy is new since) */
		}
		if (e->images[i].target && e->images[i].drawn && e->images[i].shared) {
			in_place++;
		}
	}
	if (in_place != 0 && !finished && wait_for_frames) {
		glFinish();             /* the display's driver reads the surface as soon as it is told of it */
	} else if (in_place != 0 && !finished && sync_frames) {
		/*
		 * The display's driver waits for the last submission that named the surface -- and Mesa's
		 * freedreno may still hold a batch back after glFlush, to merge it with the next (submit
		 * merging, for small submissions). A fence that is asked to flush and waited for not at all
		 * sends everything held back to the kernel now; the GPU is not waited for. (Taken once for
		 * the cause of frames with parts missing; that was drawing into the surface itself, see
		 * blit_targets.)
		 */
		GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

		if (sync != NULL) {
			(void)glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
			glDeleteSync(sync);
		}
	}
	waited = now_us();
	for (unsigned i = 0; i < e->image_count; i++) {
		if (e->images[i].target && e->images[i].drawn) {
			copied += !e->images[i].shared;
			target_to_surface(e, &e->images[i]);
			if (shown_texture != 0) {
				show_texture(e, &e->images[i]);
			}
		}
	}
	if (!e->in_frame) {
		return;
	}
	after = now_us();
	e->in_frame = false;
	e->frames++;
	if (latency_lines) {
		/* "tl: g FRAME began US ended US DRAWS": microseconds of mach_absolute_time (tools/latency.py) */
		say("tl: g %u began %llu ended %llu %u\n", e->frames, e->began, after, e->draws);
	}
	if (frame_lines) {
		/*
		 * One line a frame, for matching with a screen recording (the kernel's rec_ring, whose log
		 * says when it began in the same time): when the frame ended, in the kernel's time; what was
		 * drawn; into which of the display's surfaces and, of the first draw or clear into it, the
		 * scissor box -- CoreAnimation draws only what has changed, inside that box, and leaves the
		 * rest of the surface as it was --; and the draws of the blur whose radius varies.
		 */
		say("annibale-gl: f %u at %llu.%03llu s: %u draws, %u clears; display surface %u, %u draws into it, box %s %d,%d %dx%d; "
		    "vrd %u vrb %u; %u in place, %u copied; spans %u solid %u blended, ends %u %u; other %u %u\n", e->frames,
		    after / 1000000, after / 1000 % 1000, e->draws, e->clears, e->line.surface, e->line.into_display, e->line.boxed ? "on" : "off", e->line.box[0], e->line.box[1],
		    e->line.box[2], e->line.box[3], e->line.vrd, e->line.vrb, in_place, copied, e->line.solid_spans, e->line.blended_spans,
		    e->line.solid_ends, e->line.blended_ends, e->line.other_spans, e->line.other_ends);
		bzero(&e->line, sizeof(e->line));
		frame_functions(e);
	}
	if (in_place == 0 && copied == 0 && e->draws != 0 && unfollowed < 8) {
		/* (whatever it was drawn into is shown as it is, drawn or not: this is to be seen in the log) */
		unfollowed++;
		say("annibale-gl: frame %u: %u draws, and none into an IOSurface that is known here (framebuffer %u, texture %u)\n",
		    e->frames, e->draws, e->framebuffer, e->colour);
	}
	e->all_calls += e->calls;
	e->all_draws += e->draws;
	e->all_triangles += e->triangles;
	e->wait_us += waited - before;
	e->readback_us += after - waited;
	e->attach_us += e->attaching;
	e->flush_us += e->flushing;
	e->frame_us += after - e->began;
	if (e->calls > e->most_calls) {
		e->most_calls = e->calls;
	}
	if (e->triangles > e->most_triangles) {
		e->most_triangles = e->triangles;
	}
	if (e->frames <= 40 || e->frames % 20 == 0) {
		say("annibale-gl: frame %u: %u GL calls, %u draws, %u triangles, %u clears; %u textures refilled from IOSurfaces "
		    "(%llu KB); %u picture%s copied out, %u drawn in place; %llu us (%llu handing on, %llu waiting, %llu copying "
		    "out), %llu before it for the IOSurfaces\n", e->frames, e->calls, e->draws, e->triangles, e->clears, e->uploads,
		    e->upload_bytes >> 10, copied, copied == 1 ? "" : "s", in_place, after - e->began, e->flushing, waited - before,
		    after - waited, e->attaching);
	}
	if (e->frames == 30 || e->frames % 200 == 0) {
		summary(e);
	}
	census_report();
	if (e->frames == 600 && e->errors == 0) {
		check_errors = 0;
	}
	e->last_draws = e->draws;
	e->calls = e->draws = e->triangles = e->uploads = e->clears = 0;
	e->upload_bytes = e->attaching = e->flushing = 0;
}

#ifndef GL_BLEND
#define GL_BLEND                        0x0be2
#endif
/* (glframes=1) a draw of lines, into the display's picture or another: CoreAnimation fills glyphs span by span */
static void
count_spans(struct eagl *e, GLenum mode, GLsizei count)
{
	struct image *image;

	if (!frame_lines || mode != GL_LINES) {
		return;
	}
	if (e->framebuffer == 0 || e->colour == 0 || (image = image_of(e, e->colour)) == NULL || !image->target ||
	    image->width < 1000 || image->height < 2000) {
		e->line.other_spans++;
		e->line.other_ends += (unsigned)count;
		return;
	}
	if (glIsEnabled(GL_BLEND)) {
		e->line.blended_spans++;
		e->line.blended_ends += (unsigned)count;
	} else {
		e->line.solid_spans++;
		e->line.solid_ends += (unsigned)count;
	}
}

#include "eagl_shaders_vblur.inc"
#include "eagl_groups_plain.inc"
#include "eagl_entries_context.inc"
