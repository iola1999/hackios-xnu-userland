/* Something is about to be drawn into the framebuffer that is bound. */
static void
drawing(struct eagl *e)
{
	struct image *image;

	frame_begin(e);
	if (e->framebuffer != 0 && e->colour != 0) {
		for (unsigned i = 0; i < VBLUR_CHAINS; i++) {
			if (e->chains[i].picture == e->colour) {
				e->chains[i].made = 0;
			}
		}
	}
	if (e->framebuffer != 0 && e->colour != 0 && (image = image_of(e, e->colour)) != NULL && image->target) {
		image->drawn = image->painted = true;
		if (frame_lines && image->width >= 1000 && image->height >= 2000) {
			/* the display's picture (a surface of its size): where this frame first draws into it */
			e->line.into_display++;
			if (!e->line.taken) {
				e->line.taken = true;
				e->line.boxed = glIsEnabled(GL_SCISSOR_TEST);
				glGetIntegerv(GL_SCISSOR_BOX, e->line.box);
				e->line.surface = IOSurfaceGetID(image->surface);
			}
		}
	}
}

/* ---- the entries written by hand ---------------------------------------------------------------- */

static void
my_glBindTexture(void *context, GLenum target, GLuint texture)
{
	struct eagl *e = context;
	struct image *image;

	EAGL_ENTER(e, 0x30);
	if (eagl_tracing) {
		eagl_trace("glBindTexture(%#x, %u)\n", target, texture);
	}
	glBindTexture(target, texture);
	if (target == GL_TEXTURE_2D && e->unit < 8) {
		e->bound[e->unit] = texture;
	}
	if (texture != 0 && e->image_count != 0 && (image = image_of(e, texture)) != NULL && !image->shared &&
	    (!image->target ? IOSurfaceGetSeed(image->surface) != image->seed || surface_changed(e, image) :
	    image->wrapped && !image->painted && surface_changed(e, image))) {
		surface_to_texture(e, image);
	}
	EAGL_LEAVE(e, 0x30);
}

static void
my_glClear(void *context, GLbitfield mask)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x58);
	drawing(e);
	e->clears++;
	if (eagl_tracing) {
		eagl_trace("glClear(%#x)\n", mask);
	}
	clear_now(e);
	if ((mask & GL_COLOR_BUFFER_BIT) && e->colour != 0) {
		struct group_texture *g = group_of(e, e->colour);

		if (g != NULL) {
			g->state = GROUP_FRESH;         /* (a group about to be drawn: see group_draw_begin()) */
		}
	}
	if (e->colour != 0) {
		/* what is about to be cleared may be what the next draw reads: see reads_what_it_writes() */
		e->unclear = mask;
		e->unclear_boxed = glIsEnabled(GL_SCISSOR_TEST);
		glGetIntegerv(GL_SCISSOR_BOX, e->unclear_box);
		glGetFloatv(GL_COLOR_CLEAR_VALUE, e->unclear_colour);
	} else {
		glClear(mask);
	}
	EAGL_LEAVE(e, 0x58);
}

/* While calls are written down: what a draw has left in the middle of what it drew into, and near its top. */
#define GL_VIEWPORT             0x0ba2

static void
trace_pixels(struct eagl *e)
{
	GLint view[4] = { 0, 0, 0, 0 };
	uint8_t middle[4] = { 0, 0, 0, 0 }, low[4] = { 0, 0, 0, 0 }, *all;
	unsigned lit = 0, left = ~0u, bottom = ~0u, right = 0, top = 0;

	glGetIntegerv(GL_VIEWPORT, view);
	glReadPixels(view[0] + view[2] / 2, view[1] + view[3] / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, middle);
	glReadPixels(view[0] + view[2] / 2, view[1] + view[3] / 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, low);
	/* ... and how much of the picture is anything but black, and where */
	if (trace_lit && view[2] > 0 && view[3] > 0 && (all = malloc((size_t)view[2] * (size_t)view[3] * 4)) != NULL) {
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(view[0], view[1], view[2], view[3], GL_RGBA, GL_UNSIGNED_BYTE, all);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		for (unsigned y = 0; y < (unsigned)view[3]; y++) {
			const uint8_t *row = all + (size_t)y * (size_t)view[2] * 4;

			for (unsigned x = 0; x < (unsigned)view[2]; x++) {
				if (row[4 * x] | row[4 * x + 1] | row[4 * x + 2]) {
					lit++;
					left = x < left ? x : left;
					right = x > right ? x : right;
					bottom = y < bottom ? y : bottom;
					top = y > top ? y : top;
				}
			}
		}
		free(all);
	}
	eagl_trace("    framebuffer %u (%d x %d): middle %u %u %u %u, an eighth up %u %u %u %u (red green blue alpha); %u pixels "
	    "not black, within %d,%d to %d,%d%s\n", e->framebuffer, view[2], view[3], middle[0], middle[1], middle[2], middle[3],
	    low[0], low[1], low[2], low[3], lit, lit ? (int)left : 0, lit ? (int)bottom : 0, (int)right, (int)top,
	    glGetError() ? " (not readable)" : "");
}

#define GL_TEXTURE_INTERNAL_FORMAT      0x1003
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE0                     0x84c0
#define GL_UNSIGNED_SHORT               0x1403
#define GL_FLOAT                        0x1406
#define GL_HALF_FLOAT                   0x140b

static float half_float(uint16_t half);

#define GL_ACTIVE_UNIFORMS              0x8b86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH    0x8b87
#define GL_FLOAT_VEC2                   0x8b50
#define GL_FLOAT_VEC3                   0x8b51
#define GL_FLOAT_VEC4                   0x8b52
#define GL_FLOAT_MAT2                   0x8b5a
#define GL_FLOAT_MAT3                   0x8b5b
#define GL_FLOAT_MAT4                   0x8b5c

/* How many numbers a uniform of this type is made of (0: not one of floats). */
static unsigned
uniform_floats(GLenum type)
{
	switch (type) {
	case GL_FLOAT:          return 1;
	case GL_FLOAT_VEC2:     return 2;
	case GL_FLOAT_VEC3:     return 3;
	case GL_FLOAT_VEC4:     return 4;
	case GL_FLOAT_MAT2:     return 4;
	case GL_FLOAT_MAT3:     return 9;
	case GL_FLOAT_MAT4:     return 16;
	default:                return 0;
	}
}

/*
 * gltrace_uniforms=1: what the program of a traced draw was given, by the names its shaders use
 * (coord_arg, texmat0, ...). CoreAnimation sets them by number, and a number says nothing; this
 * asks the program itself what it has and what is in it.
 */
static void
trace_uniforms(struct eagl *e)
{
	GLint count = 0, longest = 0;

	glGetProgramiv(e->program, GL_ACTIVE_UNIFORMS, &count);
	glGetProgramiv(e->program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &longest);
	for (GLint i = 0; i < count; i++) {
		char name[128] = "", line[400] = "";
		GLint size = 0, place;
		GLenum type = 0;
		unsigned floats;
		size_t used = 0;

		if (longest >= (GLint)sizeof(name)) {
			break;
		}
		glGetActiveUniform(e->program, (GLuint)i, (GLsizei)sizeof(name), NULL, &size, &type, name);
		place = glGetUniformLocation(e->program, name);
		if (place < 0) {
			continue;
		}
		if ((floats = uniform_floats(type)) == 0) {
			GLint whole[4] = { 0 };

			glGetUniformiv(e->program, place, whole);
			eagl_trace("      %s (%d) = %d\n", name, place, whole[0]);
			continue;
		}
		for (GLint k = 0; k < size && k < 8 && used < sizeof(line) - 80; k++) {
			GLfloat values[16] = { 0 };

			glGetUniformfv(e->program, place + k, values);
			if (size > 1) {
				used += (size_t)snprintf(line + used, sizeof(line) - used, " [%d]", k);
			}
			for (unsigned c = 0; c < floats; c++) {
				used += (size_t)snprintf(line + used, sizeof(line) - used, " %.6g", (double)values[c]);
			}
		}
		eagl_trace("      %s (%d) =%s\n", name, place, line);
	}
}

/*
 * gltrace_shaders=1: the text of the shaders the program of a traced draw is made of, the first
 * time that program draws. A program's number says nothing by itself, and CoreAnimation's own
 * shaders are what tells what a draw means; the text is the one my_glShaderSource() kept.
 */
static void
trace_program(struct eagl *e)
{
	static GLuint told[64];
	static unsigned telling;
	GLuint shaders[8] = { 0 };
	GLsizei count = 0;

	for (unsigned i = 0; i < telling; i++) {
		if (told[i] == e->program) {
			return;
		}
	}
	if (telling < sizeof(told) / sizeof(told[0])) {
		told[telling++] = e->program;
	}
	glGetAttachedShaders(e->program, (GLsizei)(sizeof(shaders) / sizeof(shaders[0])), &count, shaders);
	for (GLsizei i = 0; i < count; i++) {
		const char *text = e->sources[shaders[i] % 256];
		size_t length = text != NULL ? strlen(text) : 0;

		eagl_trace("    program %u, shader %u, %zu characters:\n", e->program, shaders[i], length);
		for (size_t at = 0; at < length; at += 900) {
			write(2, text + at, length - at < 900 ? length - at : 900);
		}
		if (length > 0 && text[length - 1] != '\n') {
			write(2, "\n", 1);
		}
	}
}

/*
 * What a draw that is being written down is made of: the texture on the first unit, and the
 * first corners as CoreAnimation has them in its arrays (place, colour, place in the texture).
 */
static void
trace_draw(struct eagl *e, GLenum type, const void *indices, GLint first, GLsizei count)
{
	char textures[160] = "";
	size_t used = 0;

	if (trace_shaders) {
		trace_program(e);
	}
	if (trace_uniforms_on) {
		trace_uniforms(e);
	}

	for (unsigned unit = 0; unit < 4; unit++) {
		GLint width = 0, height = 0, format = 0, wrap_s = 0, wrap_t = 0, filter = 0;

		if (e->bound[unit] == 0) {
			continue;
		}
		glActiveTexture(GL_TEXTURE0 + unit);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
		/* ... and how it is read past its edges and between its pixels, which is where a picture
		   taken from the edge of another goes wrong */
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap_s);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrap_t);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &filter);
		used += (size_t)snprintf(textures + used, sizeof(textures) - used, " [%u] %u (%d x %d, %#x, wrap %#x/%#x, mag %#x)",
		    unit, e->bound[unit], width, height, (unsigned)format, (unsigned)wrap_s, (unsigned)wrap_t, (unsigned)filter);
	}
	glActiveTexture(GL_TEXTURE0 + e->unit);
	eagl_trace("    program %u, framebuffer %u (into texture %u), textures%s\n", e->program, e->framebuffer, e->colour, textures);
	for (GLsizei i = 0; i < count && i < trace_corners; i++) {
		unsigned vertex = indices == NULL ? (unsigned)(first + i) : type == GL_UNSIGNED_SHORT ?
		    ((const uint16_t *)indices)[i] : type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i] :
		    ((const uint32_t *)indices)[i];
		char line[200] = "";
		size_t used = 0;

		for (unsigned a = 0; a < 3 && used < sizeof(line) - 60; a++) {
			const uint8_t *at = e->arrays[a].pointer;
			GLint size = e->arrays[a].size > 4 ? 4 : e->arrays[a].size;

			if (at == NULL || size <= 0) {
				continue;
			}
			at += (size_t)vertex * (size_t)(e->arrays[a].stride ? e->arrays[a].stride :
			    size * (e->arrays[a].type == GL_FLOAT ? 4 : e->arrays[a].type == GL_HALF_FLOAT ? 2 : 1));
			used += (size_t)snprintf(line + used, sizeof(line) - used, " [%u]", a);
			for (GLint c = 0; c < size; c++) {
				if (e->arrays[a].type == GL_FLOAT) {
					float value;

					memcpy(&value, at + 4 * c, 4);
					used += (size_t)snprintf(line + used, sizeof(line) - used, " %.5g", (double)value);
				} else if (e->arrays[a].type == GL_HALF_FLOAT) {
					/* (a layer's colour comes this way: four half floats, not four bytes) */
					uint16_t half;

					memcpy(&half, at + 2 * c, 2);
					used += (size_t)snprintf(line + used, sizeof(line) - used, " %.4g",
					    (double)half_float(half));
				} else {
					used += (size_t)snprintf(line + used, sizeof(line) - used, " %u", at[c]);
				}
			}
		}
		eagl_trace("    corner %u:%s\n", vertex, line);
	}
}

#define GL_BLEND                        0x0be2
#define GL_BLEND_DST_RGB                0x80c8
#define GL_BLEND_SRC_RGB                0x80c9

static float
half_float(uint16_t half)
{
	int exponent = half >> 10 & 31;
	float value = exponent == 0 ? (float)(half & 1023) / 1024 / 16384 :
	    (1 + (float)(half & 1023) / 1024) * (exponent >= 15 ? (float)(1u << (exponent - 15)) : 1 / (float)(1u << (15 - exponent)));

	return half & 0x8000 ? -value : value;
}

/* gltrace_place: a line about a draw, if any of its first corners is inside the rectangle (or the rectangle inside them). */
static void
trace_placed_draw(struct eagl *e, GLenum mode, GLenum type, const void *indices, GLint first, GLsizei count)
{
	float low[2] = { 1e9f, 1e9f }, high[2] = { -1e9f, -1e9f }, colour[4] = { 0, 0, 0, 0 }, where[2] = { 0, 0 };
	GLint blend[2] = { 0, 0 }, scissor[4] = { 0, 0, 0, 0 };
	char textures[160] = "";
	size_t used = 0;

	if (trace_shaders) {
		trace_program(e);               /* (gltrace_shaders=1: the text of each program as it first draws here) */
	}
	if (trace_uniforms_on) {
		trace_uniforms(e);              /* (gltrace_uniforms=1: and what it was given) */
	}

	if (e->arrays[0].pointer == NULL || e->arrays[0].type != GL_FLOAT) {
		return;
	}
	for (GLsizei i = 0; i < count && i < 96; i++) {
		unsigned vertex = indices == NULL ? (unsigned)(first + i) : type == GL_UNSIGNED_SHORT ?
		    ((const uint16_t *)indices)[i] : type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i] :
		    ((const uint32_t *)indices)[i];
		const uint8_t *at = (const uint8_t *)e->arrays[0].pointer + (size_t)vertex * (size_t)e->arrays[0].stride;
		float place[2];

		memcpy(place, at, sizeof(place));
		for (unsigned axis = 0; axis < 2; axis++) {
			low[axis] = place[axis] < low[axis] ? place[axis] : low[axis];
			high[axis] = place[axis] > high[axis] ? place[axis] : high[axis];
		}
		if (i == 0) {
			if (e->arrays[1].pointer != NULL && e->arrays[1].type == GL_HALF_FLOAT) {
				uint16_t halves[4];

				memcpy(halves, (const uint8_t *)e->arrays[1].pointer + (size_t)vertex * (size_t)e->arrays[1].stride, 8);
				for (unsigned part = 0; part < 4; part++) {
					colour[part] = half_float(halves[part]);
				}
			}
			if (e->arrays[2].pointer != NULL && e->arrays[2].type == GL_FLOAT) {
				memcpy(where, (const uint8_t *)e->arrays[2].pointer + (size_t)vertex * (size_t)e->arrays[2].stride, 8);
			}
		}
	}
	if (low[0] > trace_place[2] || high[0] < trace_place[0] || low[1] > trace_place[3] || high[1] < trace_place[1]) {
		return;
	}
	trace_listed = 1;
	trace_middle[0] = (low[0] + high[0]) / 2;
	trace_middle[1] = (low[1] + high[1]) / 2;
	for (unsigned unit = 0; unit < 4; unit++) {
		GLint width = 0, height = 0, format = 0;
		struct image *image;

		if (e->bound[unit] == 0) {
			continue;
		}
		glActiveTexture(GL_TEXTURE0 + unit);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
		image = image_of(e, e->bound[unit]);
		used += (size_t)snprintf(textures + used, sizeof(textures) - used, " [%u] %u (%d x %d, %#x%s)", unit, e->bound[unit],
		    width, height, (unsigned)format, image == NULL ? "" : image->shared ? ", a surface's memory" :
		    image->wrapped ? ", a copy of wrapped memory" : ", a copy of a surface");
	}
	glActiveTexture(GL_TEXTURE0 + e->unit);
	glGetIntegerv(GL_BLEND_SRC_RGB, &blend[0]);
	glGetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
	glGetIntegerv(GL_SCISSOR_BOX, scissor);
	say("gl-at: frame %u draw %u into %u (texture %u): %s of %d from %g,%g to %g,%g; program %u; textures%s; colour %g %g %g "
	    "%g; in the texture at %g,%g; blend %s %#x %#x; scissor %s %d,%d %dx%d\n", e->frames + 1, e->draws, e->framebuffer,
	    e->colour, mode == GL_TRIANGLES ? "triangles" : mode == 1 ? "lines" : "strips", count, (double)low[0], (double)low[1],
	    (double)high[0], (double)high[1], e->program, textures, (double)colour[0], (double)colour[1], (double)colour[2],
	    (double)colour[3], (double)where[0], (double)where[1], glIsEnabled(GL_BLEND) ? "on" : "off", blend[0], blend[1],
	    glIsEnabled(GL_SCISSOR_TEST) ? "on" : "off", scissor[0], scissor[1], scissor[2], scissor[3]);
}

/* gltrace_look: the pixel a listed draw has left in the middle of itself (the matrix is CoreAnimation's, the last it gave). */
static void
trace_placed_look(struct eagl *e)
{
	static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	const GLfloat *m = e->matrix != NULL ? e->matrix : identity;
	float x = m[0] * trace_middle[0] + m[4] * trace_middle[1] + m[12];
	float y = m[1] * trace_middle[0] + m[5] * trace_middle[1] + m[13];
	float w = m[3] * trace_middle[0] + m[7] * trace_middle[1] + m[15];
	GLint view[4] = { 0, 0, 0, 0 };
	uint8_t pixel[4] = { 0, 0, 0, 0 };
	int px, py;

	trace_listed = 0;
	if (!trace_look || w == 0) {
		return;
	}
	glGetIntegerv(GL_VIEWPORT, view);
	px = view[0] + (int)((x / w + 1) / 2 * (float)view[2]);
	py = view[1] + (int)((y / w + 1) / 2 * (float)view[3]);
	glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	say("gl-at:     it left %u %u %u %u (red green blue alpha) at %d,%d of the framebuffer%s\n", pixel[0], pixel[1], pixel[2],
	    pixel[3], px, py, glGetError() ? " (not readable)" : "");
}

static unsigned
triangles_of(GLenum mode, GLsizei count)
{
	return mode == GL_TRIANGLES ? (unsigned)count / 3 : (mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) &&
	       count > 2 ? (unsigned)count - 2 : 0;
}

/*
 * CoreAnimation's OpenGL ES renderer draws the last of its ninety "functions" (0x59, "cst" in the
 * table its Metal renderer names its shaders by; no textures of its own by that table) with no
 * program at all: glUseProgram(0), and then the draw. On iOS's library that may have meant
 * something; in OpenGL ES it draws nothing, and what goes that way is missing from the picture:
 * the lock screen's wallpaper, and the clock that is that wallpaper seen through its digits.
 * Here such a draw is made with a program of ours, the plainest there could be: CoreAnimation's
 * own vertex arrays (position 0, colour 1, texture coordinates 2, the latter in the texture's
 * pixels), the matrix it gives every program of its own (always from one place of its: that
 * place is read), and the colour times the texture on the first unit. With that the lock screen
 * has its wallpaper and its clock, and the home screen is as it was.
 * Only draws with a texture bound are treated so. CoreAnimation also makes draws with no program
 * and no texture, over the whole of a surface, several a frame: given the colour alone
 * (glplain=2) they paint the screen white, so whatever they are for, it is not that; they are
 * left to draw nothing, as before. What the function is on the Metal side was not found out.
 */
#define GL_VERTEX_SHADER                0x8b31
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED  0x8622

static const char plain_vertex[] =
    "#version 300 es\n"
    "uniform highp mat4 vertex_matrix;\n"
    "in highp vec4 vertex_position;\n"
    "in lowp vec4 vertex_color;\n"
    "in highp vec2 vertex_texcoord0;\n"
    "out lowp vec4 color;\n"
    "out highp vec2 texcoord;\n"
    "void main() {\n"
    "  gl_Position = vertex_matrix * vertex_position;\n"
    "  color = vertex_color;\n"
    "  texcoord = vertex_texcoord0;\n"
    "}\n";
static const char plain_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform lowp float textured;\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "void main() {\n"
    "  result = textured > 0.5 ? color * texture(texture0, texcoord / vec2(textureSize(texture0, 0))) : color;\n"
    "}\n";

/*
 * One kind of them has a second texture, on the second unit: a table, one row of pixels (seen:
 * 256 of two parts, brightness and opacity). What is drawn so is the home indicator, the bar at
 * the bottom of the screen that is light over dark pictures and dark over light ones. It is a
 * filter of CoreAnimation's own, "homeAffordanceBase", which its OpenGL ES renderer has no
 * program for: it binds what is behind the bar and the table, and draws with none; the filter's
 * three numbers (inputAddWhite, inputAmount, inputOverlayOpacity) it hands to no one. What the
 * filter does is CoreAnimation's software renderer's to say, and this is the same arithmetic:
 *   the table's entry for the brightness of what is behind, plus the white added, times the
 *   amount; that over what is behind; and the result laid over itself in "overlay" mode at the
 *   overlay's opacity.
 * The three numbers are the ones with which this gives the software renderer's bar on the
 * phone, 153 of 255 over black and 63 over white: 0.284, 1 and 0.31. (MaterialKit's defaults
 * for its "thin" bar would be 0.373, 0.31 and 0.4, and give neither.)
 */
static const char table_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform lowp sampler2D table;\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "const mediump float white = .284, amount = 1., overlay = .31;\n"
    "void main() {\n"
    "  mediump vec4 behind = texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
    "  mediump vec4 entry = texture(table, vec2(dot(behind.rgb, vec3(.2125, .7154, .0721)), .5));\n"
    "  mediump vec4 bar = clamp(entry + vec4(vec3(white), 1.), 0., 1.) * amount;\n"
    "  mediump vec4 under = behind * (1. - bar.a) + bar;\n"
    "  mediump vec4 over = under * overlay;\n"
    "  mediump vec3 dark = 2. * under.rgb * over.rgb;\n"
    "  mediump vec3 light = over.a * under.a - 2. * (under.a - under.rgb) * (over.a - over.rgb);\n"
    "  mediump vec3 both = mix(dark, light, step(under.a * .5, under.rgb));\n"
    "  mediump float opacity = over.a + under.a * (1. - over.a);\n"
    "  result = vec4(clamp(both + over.rgb * (1. - under.a) + under.rgb * (1. - over.a), 0., opacity), opacity);\n"
    "}\n";

/*
 * Two more are told apart by the "function" itself, which CoreAnimation's context has in a known
 * place while it draws (and which our library in the window server hands over with the context:
 * annibale_gl_function_values() below, called from inject.c). Both take a colour matrix, four
 * columns and what is added, the numbers function_values[] keeps.
 *
 *   "vcm" (45), the filter vibrantColorMatrix: the layer is not drawn, it says how much of the
 *   matrix is applied to what is behind it. The lock screen has two: a gradient over the whole
 *   screen, opaque red at the top and clear at the bottom, under which the wallpaper is toned
 *   down for the clock to be read; and the clock itself, through whose digits the wallpaper
 *   comes out nearly white. Drawn as a picture, the first was a red veil over the wallpaper.
 *   What the filter does is CoreAnimation's software renderer's to say (tex_vibrant_color_matrix:
 *   the matrix over what is there, tex_color_matrix_4x4_bias, and tex_mix by the layer's alpha);
 *   the matrix arithmetic is also in the OpenGL ES renderer's own shaders, colorMatrix4x4Bias.
 *   What is there is read with EXT_shader_framebuffer_fetch.
 *
 *   "c4p" (20), the filter colorMatrix told that its input is premultiplied
 *   (inputPremultipliedValues): the matrix over the layer's pixels as they are. The clock's
 *   digits pass through one that keeps nothing but red as alpha ("colorToAlphaMask") before they
 *   are that mask; drawn as they were, they stayed white.
 */
#define FUNCTION_MATRIX_PREMULTIPLIED   20
#define FUNCTION_DOWNSAMPLE_8           28
#define FUNCTION_VARIABLE_DOWNSAMPLE    38
#define FUNCTION_VARIABLE_BLUR          39
#define FUNCTION_SHADOW_BLUR            40
#define FUNCTION_VIBRANT_MATRIX         45

static const char vibrant_fragment[] =
    "#version 300 es\n"
    "#extension GL_EXT_shader_framebuffer_fetch : require\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform mediump vec4 colours[5];\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "layout(location = 0) inout highp vec4 result;\n"
    "void main() {\n"
    "  mediump vec4 layer = color * texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
    "  mediump vec4 behind = result, s = behind;\n"
    "  s.rgb = s.rgb / max(s.a, .005);\n"
    "  s = s.r * colours[0] + s.g * colours[1] + s.b * colours[2] + s.a * colours[3] + colours[4];\n"
    "  s.a = clamp(s.a, 0., 1.);\n"
    "  s.rgb = clamp(s.rgb * s.a, 0., 1.);\n"
    "  result = mix(behind, s, layer.a);\n"
    "}\n";
static const char masking_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform mediump vec4 colours[5];\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "void main() {\n"
    "  mediump vec4 s = color * texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
    "  result = clamp(s.r * colours[0] + s.g * colours[1] + s.b * colours[2] + s.a * colours[3] + colours[4], 0., 1.);\n"
    "}\n";

/*
 *   "ds8" (28): a picture made eight times smaller in one go, the first step of a blur of what is
 *   behind a layer (capture_backdrop()). Its numbers are four places, one and three pixels of the
 *   picture away each way: with the four of them mirrored in both directions, sixteen samples of
 *   four pixels each, they are the sixty-four pixels that become one. (CoreAnimation's own shaders
 *   for four and for six samples are of that kind; for this one the OpenGL ES renderer has none.)
 *   Drawn as it was given, one sample in the middle, four pixels of the sixty-four made the one:
 *   what is blurred after that shimmers when it moves.
 *   The colour of the corners is not the picture's to take: CoreAnimation's own shaders for four
 *   and for six samples do not touch it, and it is the layer's opacity, which it applies where the
 *   layer is laid on the screen. Multiplied in here, what was captured came out at the opacity of
 *   the layer that asked for it -- 0.45 for the translucent minus on a jiggling icon -- and
 *   everything made of it after, the blur, the curve of brightness and the colour matrix, worked
 *   on a picture too dark: the minus came out stained with what was behind it instead of the
 *   light grey it is on a phone.
 */
/*
 *   "sb3" (40): four samples around a point, averaged -- the same four places as "ds4" is given,
 *   one pixel out each way, with no weights of its own. It is asked for where a shadow is laid
 *   over what is behind it (the cards of the switcher of running applications) and in the home
 *   screen's edit mode. CoreAnimation's OpenGL ES renderer has no shader for it and, unlike the
 *   ones it never asks about, it asks and is told no -- and then what needs it is not drawn at
 *   all, so the shadow is missing. This waits for a way to hand CoreAnimation a program where it
 *   looks for one: answering that the renderer has the function (src/inject.c, glyes=40) only
 *   kills the window server, and no draw with no program ever arrives here (see there).
 */
static const char blur4_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform highp vec2 places[4];\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "void main() {\n"
    "  highp vec2 middle = texcoord / vec2(textureSize(texture0, 0));\n"
    "  result = .25 * (texture(texture0, middle + places[0]) + texture(texture0, middle + places[1]) +\n"
    "      texture(texture0, middle + places[2]) + texture(texture0, middle + places[3]));\n"
    "}\n";

static const char box_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform highp vec2 places[4];\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "void main() {\n"
    "  highp vec2 middle = texcoord / vec2(textureSize(texture0, 0));\n"
    "  highp vec4 sum = vec4(0.);\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    sum += texture(texture0, middle + places[i]) + texture(texture0, middle - places[i]);\n"
    "    sum += texture(texture0, middle + places[i] * vec2(1., -1.)) + texture(texture0, middle + places[i] * vec2(-1., 1.));\n"
    "  }\n"
    "  result = sum * .0625;\n"
    "}\n";

static int function_numbers;            /* boot-arg glnumbers=1 */

/* Four of a function's numbers at a time, for the line above (one of a few buffers in turn). */
static const char *
number_text(const float *values, size_t from, size_t count)
{
	static char texts[5][80];
	static unsigned which;
	char *text = texts[which++ % (sizeof(texts) / sizeof(texts[0]))];

	if (from >= count) {
		return "";
	}
	snprintf(text, sizeof(texts[0]), " [%g %g %g %g]", (double)values[from],
	    from + 1 < count ? (double)values[from + 1] : 0.0, from + 2 < count ? (double)values[from + 2] : 0.0,
	    from + 3 < count ? (double)values[from + 3] : 0.0);
	return text;
}

static void *ca_context;                /* CoreAnimation's GLContext */
static GLfloat function_values[90][20];
static uint8_t function_given[90];

/* From inject.c, at every GLContext::function_uniform(): the numbers a function is given, which no shader of CoreAnimation's takes. */
void
annibale_gl_function_values(void *context, unsigned function, unsigned index, size_t components, size_t count, const float *values)
{
	static uint8_t said[90];

	ca_context = context;
	if (function >= 90 || index != 0 || components * count > 20) {
		return;
	}
	/* (boot-arg glnumbers=1: every set of numbers a function is given that is not the one before,
	   up to a dozen of them -- the same function serves many a layer, with a matrix of its own) */
	if (!said[function] || (function_numbers && said[function] < 12 &&
	    memcmp(function_values[function], values, components * count * sizeof(values[0])) != 0)) {
		said[function]++;
		say("annibale-gl: CoreAnimation gives its function %u numbers: %zu of %zu:%s%s%s%s%s%s%s%s%s%s\n", function, count,
		    components, number_text(values, 0, components * count), number_text(values, 4, components * count),
		    number_text(values, 8, components * count), number_text(values, 12, components * count),
		    number_text(values, 16, components * count), "", "", "", "", "");
	}
	memcpy(function_values[function], values, components * count * sizeof(values[0]));
	function_given[function] = (uint8_t)(components * count);
}

/* boot-arg glplain=0: leave such draws as they are; 2: also those without a texture; 3: the two with a colour matrix drawn plainly too */
static int plain_draws = 1;

/*
 * The blur whose radius varies over the picture (the filter variableBlur with a mask: the strip
 * under the status bar in Search, through which what scrolls up fades into a blur) is made of two
 * functions, "vrd" (38) and "vrb" (39): the first, four times over, is to make of a picture its
 * smaller copies, each from the one before, and the second reads among them by the mask. In the
 * Metal renderer the copies are the levels of one texture. The OpenGL ES renderer has neither
 * shader, and nothing that says which level is drawn into: what it does, four times, is to clear
 * the texture and draw into it with that same texture as the source. What that leaves is the
 * driver's to decide (here now the picture, now nothing: the strip came out black after one
 * route to Search and right after another).
 * Here the levels are textures of our own, each half the size of the one before, one chain of
 * them for each picture blurred so: "vrd", whose one number is the level it reads, makes level
 * L + 1 of level L instead of drawing the picture into itself (vblur_level()), and "vrb" reads
 * the chain by the mask (vblur_draw_begin()). Boot-arg vblur=0 leaves it as it was before: no
 * level made, the picture not drawn from itself nor cleared for it, and "vrb" copying it out
 * plainly -- the strip unblurred. That stand-in had a fault of its own: it showed whatever the
 * picture last held, and once in a few dozen times in Search that was the wallpaper half dimmed
 * as it had been in the middle of Search's coming in, until something drew the strip again.
 *
 * Which function a draw is can be asked of CoreAnimation's context only while it draws (between
 * frames the place it is read from may be gone: the window server died of being asked at a
 * glClear). So every clear of a texture is put off, with the scissor and the colour it was to be
 * made with, until the next draw says what it is: that function, and the clear is dropped with
 * the draw; anything else, and it is made first (as it is before anything else that could tell
 * the difference: another framebuffer, another attachment, a texture deleted or filled, the end
 * of a frame). Only the clears of a texture that was also bound to be read used to be put off,
 * but CoreAnimation clears a surface as it makes it the target (GLESContext::set_surface: the
 * framebuffer, the viewport, the clear), before it binds the textures of the draw that follows:
 * the first of the four clears found the picture of the step before on the first unit, was
 * made, and left the strip black -- now in the first Search of a run, now from the second on,
 * by what had been drawn before.
 */
/*
 * Boot-arg vblur=0: the stand-in. vblur=2, for checking: the strip painted by its mask instead of
 * blurred, red where the radius is whole, blue where there is none, and the mask's pixels in the log.
 */
static int vblur_on = 1;

/*
 * Halving: a pixel of the level made is the average of the sixteen of the level it is made of
 * around it, by weight (four samples, each the GPU's own average of four pixels), which is what
 * makes a chain of such levels look blurred rather than coarse when it is read back enlarged.
 */
static const char vblur_vertex[] =
    "#version 300 es\n"
    "void main() {\n"
    "  vec2 corner = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
    "  gl_Position = vec4(corner, 0.0, 1.0);\n"
    "}\n";
static const char halve_fragment[] =
    "#version 300 es\n"
    "uniform highp sampler2D source;\n"
    "uniform highp vec2 texel;\n"
    "out highp vec4 result;\n"
    "void main() {\n"
    "  highp vec2 at = gl_FragCoord.xy * 2.0 * texel;\n"
    "  result = 0.25 * (texture(source, at + texel * vec2(-1.0, -1.0)) + texture(source, at + texel * vec2(1.0, -1.0)) +\n"
    "      texture(source, at + texel * vec2(-1.0, 1.0)) + texture(source, at + texel * vec2(1.0, 1.0)));\n"
    "}\n";
/*
 * Reading the chain: the mask (its opacity, on the second unit, at CoreAnimation's second texture
 * coordinates) says how much of the radius (the function's first number, in pixels of the
 * picture) is wanted at each pixel; a radius of r is read at level log2(r), between two levels
 * mixed. The mask's coordinates run past its ends (above the strip's top, below its bottom) and
 * the texture repeats: unheld, the last row of the strip read the mask's first, the whole radius,
 * and came out a line blurred darker than what it lay on. They are held to the mask's ends.
 * With the function's second number set (inputNormalizeEdges, it seems), what the blur drew in
 * from outside the picture -- nothing, transparent -- is made up for by dividing by the opacity,
 * as the edges of an ordinary blur are.
 * The colour's opacity is the blurring layer's (Spotlight's strip fades so while the gesture that
 * dismisses Search is under way), and CoreAnimation puts what this draws on the screen as it is,
 * without blending: so the opacity is how much of the blur is shown over the picture itself, not a
 * darkening. Multiplied in, the strip went dark with it -- to a quarter, in four frames, at each
 * start of that gesture -- and stayed dark whenever the gesture ended between two of the window
 * server's renderings of the strip: it reuses the last one until what is behind the strip changes
 * (the dark strip in Search, rc.jobs.strip).
 */
static const char vblur_draw_vertex[] =
    "#version 300 es\n"
    "uniform highp mat4 vertex_matrix;\n"
    "in highp vec4 vertex_position;\n"
    "in lowp vec4 vertex_color;\n"
    "in highp vec2 vertex_texcoord0;\n"
    "in highp vec2 vertex_texcoord1;\n"
    "out lowp vec4 color;\n"
    "out highp vec2 texcoord;\n"
    "out highp vec2 maskcoord;\n"
    "void main() {\n"
    "  gl_Position = vertex_matrix * vertex_position;\n"
    "  color = vertex_color;\n"
    "  texcoord = vertex_texcoord0;\n"
    "  maskcoord = vertex_texcoord1;\n"
    "}\n";
static const char vblur_fragment[] =
    "#version 300 es\n"
    "uniform highp sampler2D level0;\n"
    "uniform highp sampler2D mask;\n"
    "uniform highp sampler2D level1;\n"
    "uniform highp sampler2D level2;\n"
    "uniform highp sampler2D level3;\n"
    "uniform highp sampler2D level4;\n"
    "uniform highp float radius;\n"
    "uniform int levels;\n"
    "uniform highp float normalize_edges;\n"
    "uniform int paint;\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "in highp vec2 maskcoord;\n"
    "out highp vec4 result;\n"
    "highp vec4 level(int k, highp vec2 at) {\n"
    "  if (k <= 0) return texture(level0, at);\n"
    "  if (k == 1) return texture(level1, at);\n"
    "  if (k == 2) return texture(level2, at);\n"
    "  if (k == 3) return texture(level3, at);\n"
    "  return texture(level4, at);\n"
    "}\n"
    "void main() {\n"
    "  highp vec2 at = texcoord / vec2(textureSize(level0, 0));\n"
    "  highp vec2 mask_size = vec2(textureSize(mask, 0));\n"
    "  highp vec2 m_at = maskcoord.x > 1.5 || maskcoord.y > 1.5 ? maskcoord / mask_size : maskcoord;\n"
    "  m_at = clamp(m_at, 0.5 / mask_size, 1.0 - 0.5 / mask_size);\n"
    "  highp float m = texture(mask, m_at).a;\n"
    "  highp float lod = clamp(log2(max(m * radius, 1.0)), 0.0, float(levels - 1));\n"
    "  int k = int(floor(lod));\n"
    "  highp vec4 c = mix(level(k, at), level(min(k + 1, levels - 1), at), lod - float(k));\n"
    "  if (normalize_edges > 0.5) {\n"
    "    c = c.a > 0.004 ? vec4(c.rgb / c.a, 1.0) : vec4(0.0);\n"
    "    highp vec4 sharp = texture(level0, at);\n"
    "    sharp = sharp.a > 0.004 ? vec4(sharp.rgb / sharp.a, 1.0) : c;\n"
    "    result = vec4(mix(sharp.rgb, c.rgb, color.a), 1.0);\n"
    "  } else {\n"
    "    result = color * c;\n"
    "  }\n"
    "  if (paint != 0) {\n"
    "    highp float t = m;\n"
    "    result = vec4(t, 0.25 * c.g, 1.0 - t, 1.0);\n"
    "  }\n"
    "}\n";

static GLuint
vblur_program(const char *vertex, const char *fragment, const char *what)
{
	GLuint program = glCreateProgram(), shader;
	const GLchar *text;
	GLint good = 0;
	char said[512] = "";

	shader = glCreateShader(GL_VERTEX_SHADER);
	text = vertex;
	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glAttachShader(program, shader);
	shader = glCreateShader(GL_FRAGMENT_SHADER);
	text = fragment;
	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &good);
	if (!good) {
		glGetShaderInfoLog(shader, sizeof(said), NULL, said);
	}
	glAttachShader(program, shader);
	glBindAttribLocation(program, 0, "vertex_position");
	glBindAttribLocation(program, 1, "vertex_color");
	glBindAttribLocation(program, 2, "vertex_texcoord0");
	glBindAttribLocation(program, 3, "vertex_texcoord1");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	say("annibale-gl: a program of ours for the blur whose radius varies (%s): %s %s\n", what, good ? "made" : "NOT MADE", said);
	return good ? program : 0;
}

static struct vblur_chain *
vblur_chain_of(struct eagl *e, GLuint picture, bool make)
{
	struct vblur_chain *oldest = &e->chains[0];

	for (unsigned i = 0; i < VBLUR_CHAINS; i++) {
		if (e->chains[i].picture == picture && picture != 0) {
			return &e->chains[i];
		}
		if (e->chains[i].used < oldest->used) {
			oldest = &e->chains[i];
		}
	}
	if (!make) {
		return NULL;
	}
	/* (the one least lately used gives its textures up; they are made again at the sizes wanted) */
	oldest->picture = picture;
	oldest->made = 0;
	oldest->width = oldest->height = 0;
	return oldest;
}

static void
vblur_forget(struct eagl *e, GLuint picture)
{
	struct vblur_chain *chain = vblur_chain_of(e, picture, false);

	if (chain != NULL) {
		chain->picture = 0;
		chain->made = 0;
	}
}

/*
 * A draw of "vrd" (function 38) that reads level `from`: level from + 1 is made of it, in a
 * texture of ours. Every bit of state touched is put back as CoreAnimation had it.
 */
static void
vblur_level(struct eagl *e, int from)
{
	GLuint picture = e->bound[0];
	struct vblur_chain *chain;
	GLint view[4], box[4], width = 0, height = 0, to = from + 1;
	GLboolean scissored, blended;
	GLuint source;

	if (from < 0 || to >= VBLUR_LEVELS || picture == 0) {
		return;
	}
	if (e->halve == 0) {
		e->halve = vblur_program(vblur_vertex, halve_fragment, "halving");
		if (e->halve != 0) {
			glUseProgram(e->halve);
			glUniform1i(glGetUniformLocation(e->halve, "source"), 0);
			e->halve_texel = glGetUniformLocation(e->halve, "texel");
			glUseProgram(0);
		}
	}
	if (e->halve == 0) {
		return;
	}
	glActiveTexture(GL_TEXTURE0);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	chain = vblur_chain_of(e, picture, true);
	chain->used = e->frames + 1;
	if (chain->width != width || chain->height != height) {
		chain->width = width;
		chain->height = height;
		chain->made = 1;
		for (int k = 1; k < VBLUR_LEVELS; k++) {
			chain->level_width[k] = 0;
		}
	}
	if (from == 0) {
		chain->made = 1;                        /* (the picture is new: the levels made of the one before are not of it) */
	}
	if (from >= chain->made) {
		glActiveTexture(GL_TEXTURE0 + e->unit);
		return;                                 /* (the level it is to be made of is not there) */
	}
	if (chain->level_width[to] == 0) {
		GLsizei w = chain->width, h = chain->height;

		for (int k = 0; k < to; k++) {
			w = w / 2 > 1 ? w / 2 : 1;
			h = h / 2 > 1 ? h / 2 : 1;
		}
		if (chain->level[to] == 0) {
			glGenTextures(1, &chain->level[to]);
			glGenFramebuffers(1, &chain->framebuffer[to]);
		}
		glBindTexture(GL_TEXTURE_2D, chain->level[to]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, 0x2801, 0x2601);         /* min filter: linear */
		glTexParameteri(GL_TEXTURE_2D, 0x2800, 0x2601);         /* mag filter: linear */
		glTexParameteri(GL_TEXTURE_2D, 0x2802, 0x812f);         /* clamp to edge */
		glTexParameteri(GL_TEXTURE_2D, 0x2803, 0x812f);
		glBindFramebuffer(GL_FRAMEBUFFER, chain->framebuffer[to]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, chain->level[to], 0);
		chain->level_width[to] = w;
		chain->level_height[to] = h;
	}
	source = from == 0 ? picture : chain->level[from];
	glGetIntegerv(GL_VIEWPORT, view);
	glGetIntegerv(GL_SCISSOR_BOX, box);
	scissored = glIsEnabled(GL_SCISSOR_TEST);
	blended = glIsEnabled(GL_BLEND);
	glBindFramebuffer(GL_FRAMEBUFFER, chain->framebuffer[to]);
	glViewport(0, 0, chain->level_width[to], chain->level_height[to]);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glUseProgram(e->halve);
	glBindTexture(GL_TEXTURE_2D, source);
	glUniform2f(e->halve_texel, 1.0f / (float)(from == 0 ? chain->width : chain->level_width[from]),
	    1.0f / (float)(from == 0 ? chain->height : chain->level_height[from]));
	glDrawArrays(GL_TRIANGLES, 0, 3);
	chain->made = to + 1;
	/* back as it was */
	glBindTexture(GL_TEXTURE_2D, picture);
	glUseProgram(0);
	(blended ? glEnable : glDisable)(GL_BLEND);
	(scissored ? glEnable : glDisable)(GL_SCISSOR_TEST);
	glScissor(box[0], box[1], box[2], box[3]);
	glViewport(view[0], view[1], view[2], view[3]);
	glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
	glActiveTexture(GL_TEXTURE0 + e->unit);
}

static bool
reads_what_it_writes(struct eagl *e)
{
	return e->program == 0 && e->colour != 0 && e->colour == e->bound[0] && ca_context != NULL &&
	       *(*(uint8_t **)((char *)ca_context + 0x10) + 0x10) == FUNCTION_VARIABLE_DOWNSAMPLE;
}

static void
clear_now(struct eagl *e)
{
	GLint box[4] = { 0, 0, 0, 0 };
	GLfloat colour[4] = { 0, 0, 0, 0 };
	GLboolean color_mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
	GLboolean depth_mask = GL_TRUE;
	GLboolean boxed;

	if (e->unclear == 0) {
		return;
	}
	boxed = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_SCISSOR_BOX, box);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, colour);
	glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	(e->unclear_boxed ? glEnable : glDisable)(GL_SCISSOR_TEST);
	glScissor(e->unclear_box[0], e->unclear_box[1], e->unclear_box[2], e->unclear_box[3]);
	glClearColor(e->unclear_colour[0], e->unclear_colour[1], e->unclear_colour[2], e->unclear_colour[3]);
	glClear(e->unclear);
	e->unclear = 0;
	glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
	glDepthMask(depth_mask);
	(boxed ? glEnable : glDisable)(GL_SCISSOR_TEST);
	glScissor(box[0], box[1], box[2], box[3]);
	glClearColor(colour[0], colour[1], colour[2], colour[3]);
}
