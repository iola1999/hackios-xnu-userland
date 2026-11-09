static void
my_glUseProgram(void *context, GLuint program)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x12c8);
	if (eagl_tracing) {
		eagl_trace("glUseProgram(%u)\n", program);
	}
	e->program = program;
	glUseProgram(program);
	EAGL_LEAVE(e, 0x12c8);
}

static void
my_glActiveTexture(void *context, GLenum unit)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0xab8);
	if (eagl_tracing) {
		eagl_trace("glActiveTexture(%#x)\n", unit);
	}
	e->unit = unit - 0x84c0;
	glActiveTexture(unit);
	EAGL_LEAVE(e, 0xab8);
}

static void
my_glUniformMatrix4fv(void *context, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1368);
	if (eagl_tracing) {
		eagl_trace("glUniformMatrix4fv(%d, %d, %d, %p)\n", location, count, transpose, (const void *)value);
		eagl_trace_floats(value, 16);
	}
	e->matrix = value;
	glUniformMatrix4fv(location, count, transpose, value);
	EAGL_LEAVE(e, 0x1368);
}

static void
my_glVertexAttribPointer(void *context, GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
    const void *pointer)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1000);
	if (eagl_tracing) {
		eagl_trace("glVertexAttribPointer(%u, %d, %#x, %d, %d, %p)\n", index, size, type, normalized, stride, pointer);
	}
	if (index < 8) {
		e->arrays[index].pointer = pointer;
		e->arrays[index].stride = stride;
		e->arrays[index].size = size;
		e->arrays[index].type = type;
	}
	glVertexAttribPointer(index, size, type, normalized, stride, pointer);
	EAGL_LEAVE(e, 0x1000);
}

static void
my_glDrawArrays(void *context, GLenum mode, GLint first, GLsizei count)
{
	struct eagl *e = context;
	GLint enabled[4] = { 1, 1, 1, 1 };
	bool plain;

	EAGL_ENTER(e, 0x210);
	drawing(e);
	count_spans(e, mode, count);
	e->draws++;
	e->triangles += triangles_of(mode, count);
	if (eagl_tracing) {
		eagl_trace("glDrawArrays(%#x, %d, %d)\n", mode, first, count);
		trace_draw(e, 0, NULL, first, count);
	}
	if (trace_placing) {
		trace_placed_draw(e, mode, 0, NULL, first, count);
	}
	if (skipping != 0 && skip_draw(e)) {
		/* glskip=N: nothing is drawn, so that what this program puts there can be seen missing */
	} else if (group_draw_begin(e, enabled)) {
		glDrawArrays(mode, first, count);
		group_draw_end(e, enabled);
	} else {
		plain = plain_draw_begin(e, enabled);
		glDrawArrays(mode, first, count);
		if (plain) {
			plain_draw_end(e, enabled);
		}
	}
	if (trace_listed) {
		trace_placed_look(e);
	}
	EAGL_LEAVE(e, 0x210);
	if (eagl_tracing) {
		trace_pixels(e);
	}
}

static void
my_glDrawElements(void *context, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	struct eagl *e = context;
	GLint enabled[4] = { 1, 1, 1, 1 };
	bool plain;

	EAGL_ENTER(e, 0x220);
	drawing(e);
	count_spans(e, mode, count);
	e->draws++;
	e->triangles += triangles_of(mode, count);
	if (eagl_tracing) {
		eagl_trace("glDrawElements(%#x, %d, %#x, %p)\n", mode, count, type, indices);
		trace_draw(e, type, indices, 0, count);
	}
	if (trace_placing) {
		trace_placed_draw(e, mode, type, indices, 0, count);
	}
	if (skipping != 0 && skip_draw(e)) {
		/* glskip=N: see my_glDrawArrays */
	} else if (group_draw_begin(e, enabled)) {
		glDrawElements(mode, count, type, indices);
		group_draw_end(e, enabled);
	} else {
		plain = plain_draw_begin(e, enabled);
		glDrawElements(mode, count, type, indices);
		if (plain) {
			plain_draw_end(e, enabled);
		}
	}
	if (trace_listed) {
		trace_placed_look(e);
	}
	EAGL_LEAVE(e, 0x220);
	if (eagl_tracing) {
		trace_pixels(e);
	}
}

static void
my_glFinish(void *context)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x2d0);
	if (eagl_tracing) {
		eagl_trace("glFinish()\n");
	}
	clear_now(e);
	glFinish();
	frame_end(e, true);
	EAGL_LEAVE(e, 0x2d0);
}

static void
my_glFlush(void *context)
{
	struct eagl *e = context;
	uint64_t began;

	EAGL_ENTER(e, 0x2d8);
	if (eagl_tracing) {
		eagl_trace("glFlush()\n");
	}
	clear_now(e);
	began = now_us();
	glFlush();
	e->flushing += now_us() - began;
	frame_end(e, false);
	EAGL_LEAVE(e, 0x2d8);
}

/* APPLE_fence: wait for one object to be done with. Everything is done with after glFinish. */
static void
my_glFinishObjectAPPLE(void *context, GLenum object, GLint name)
{
	struct eagl *e = context;

	(void)object;
	(void)name;
	EAGL_ENTER(e, 0xeb8);
	clear_now(e);
	glFinish();
	frame_end(e, true);
	EAGL_LEAVE(e, 0xeb8);
}

static void
my_glDeleteTextures(void *context, GLsizei n, const GLuint *textures)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1e0);
	clear_now(e);
	for (GLsizei i = 0; i < n; i++) {
		image_forget(e, textures[i]);
		vblur_forget(e, textures[i]);
		group_forget(e, textures[i]);
		if (textures[i] == e->colour) {
			e->colour = 0;          /* (a texture that is deleted leaves the framebuffer bound) */
		}
	}
	glDeleteTextures(n, textures);
	EAGL_LEAVE(e, 0x1e0);
}

static const GLubyte *
my_glGetString(void *context, GLenum name)
{
	struct eagl *e = context;
	const GLubyte *string;

	EAGL_ENTER(e, 0x3b0);
	/* 0x8a1e is no name of OpenGL ES's; iOS's library answers it (the constructor of CA::OGL::GLESContext asks) */
	string = name == 0x8a1e ? (const GLubyte *)"" : glGetString(name);
	if (name == GL_EXTENSIONS && string != NULL && !fetch_offered) {
		static char without[8192];      /* (the list less that extension: see fetch_offered) */
		size_t used = 0;

		for (const char *word = (const char *)string; *word != 0;) {
			size_t length = strcspn(word, " ");

			if (strncmp(word, "GL_EXT_shader_framebuffer_fetch", 31) != 0 && used + length + 2 < sizeof(without)) {
				memcpy(without + used, word, length);
				used += length;
				without[used++] = ' ';
			}
			word += length + (word[length] == ' ');
		}
		without[used] = 0;
		string = (const GLubyte *)without;
	}
	/* The list of extensions is longer than a line of the log, and it was being cut at 900
	   characters -- which is where one looks to see whether the driver takes a compressed
	   texture of iOS's kind (ASTC). In pieces, then, so that none of it is lost. */
	if (name == GL_EXTENSIONS && string != NULL) {
		size_t length = strlen((const char *)string);

		say("annibale-gl: asked for string %#x, %zu characters:\n", name, length);
		for (size_t at = 0; at < length; at += 700) {
			say("annibale-gl:  %.700s\n", (const char *)string + at);
		}
	} else {
		say("annibale-gl: asked for string %#x: %.900s\n", name, string ? (const char *)string : "(none)");
	}
	EAGL_LEAVE(e, 0x3b0);
	return string;
}

static void
my_glGetIntegerv(void *context, GLenum name, GLint *values)
{
	struct eagl *e = context;
	static unsigned said;

	EAGL_ENTER(e, 0x348);
	glGetIntegerv(name, values);
	if (said < 40) {
		said++;
		say("annibale-gl: asked for number %#x: %d\n", name, values[0]);
	}
	EAGL_LEAVE(e, 0x348);
}

static void
my_glTexImage2D(void *context, GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void *pixels)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x970);
	if (level == 0) {
		note_format(pixels ? "filled" : "empty", (GLenum)internal, format, type, width, height);
	}
	if (eagl_tracing) {
		eagl_trace("glTexImage2D(%#x, %d, %#x, %d, %d, %d, %#x, %#x, %p)\n", target, level, internal, width, height, border,
		    format, type, pixels);
		if (pixels != NULL && type == 0x140b && format == GL_RGBA) {            /* half floats: the first and the last pixel */
			const uint16_t *half = pixels;

			eagl_trace("    half floats: %#x %#x %#x %#x ... %#x %#x %#x %#x\n", half[0], half[1], half[2], half[3],
			    half[4 * (width - 1)], half[4 * (width - 1) + 1], half[4 * (width - 1) + 2], half[4 * (width - 1) + 3]);
		}
	}
	clear_now(e);
	if (target == GL_TEXTURE_2D && e->unit < 8) {
		group_forget(e, e->bound[e->unit]);
	}
	uint64_t began = slow_calls_us != 0 ? now_us() : 0;
	glTexImage2D(target, level, (GLint)internal_format((GLenum)internal, format), width, height, border, format, type, pixels);
	if (began != 0 && now_us() - began >= slow_calls_us) {
		/* (with glslow=US: which pictures take long to hand over) */
		say("tl: slow-tex %dx%d level %d format %#x type %#x%s %llu at %llu\n", width, height, level, format, type,
		    pixels ? "" : " (empty)", now_us() - began, began);
	}
	EAGL_LEAVE(e, 0x970);
}

/* An upload into part of a texture: a group with a vibrant layer in it is made what it would have been first. */
static void
my_glTexSubImage2D(void *context, GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
    GLenum type, const void *pixels)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x9a0);
	if (eagl_tracing) {
		eagl_trace("glTexSubImage2D(%#x, %d, %d, %d, %d, %d, %#x, %#x, %p)\n", target, level, x, y, width, height, format, type,
		    pixels);
	}
	clear_now(e);
	if (target == GL_TEXTURE_2D && e->unit < 8 && e->groups != NULL) {
		group_read(e, e->bound[e->unit]);
		group_forget(e, e->bound[e->unit]);
	}
	glTexSubImage2D(target, level, x, y, width, height, format, type, pixels);
	EAGL_LEAVE(e, 0x9a0);
}

/* A copy of what is drawn into a texture: both made what they would have been first (see group_draw_begin()). */
static void
my_glCopyTexSubImage2D(void *context, GLenum target, GLint level, GLint x0, GLint y0, GLint x, GLint y, GLsizei width,
    GLsizei height)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1c8);
	if (eagl_tracing) {
		eagl_trace("glCopyTexSubImage2D(%#x, %d, %d, %d, %d, %d, %d, %d)\n", target, level, x0, y0, x, y, width, height);
	}
	clear_now(e);
	if (e->groups != NULL) {
		group_read(e, e->colour);
		if (target == GL_TEXTURE_2D && e->unit < 8) {
			group_read(e, e->bound[e->unit]);
			group_forget(e, e->bound[e->unit]);
		}
	}
	glCopyTexSubImage2D(target, level, x0, y0, x, y, width, height);
	EAGL_LEAVE(e, 0x1c8);
}

extern const void *_dyld_get_shared_cache_range(size_t *length);

/* Where the shared cache was built to be, before it was moved (as src/inject.c has it). */
#define CACHE_BASE 0x180000000UL

/* The callers of this call, as they would be in the cache as built: "ipsw a2s" names them. */
static void
say_callers(const char *what)
{
	uintptr_t callers[5], slide;
	const void *cache;
	size_t length = 0;
	char line[5 * 20 + 1] = "";
	size_t used = 0;

	cache = _dyld_get_shared_cache_range(&length);
	if (cache == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	callers[0] = (uintptr_t)__builtin_return_address(0);
	callers[1] = (uintptr_t)__builtin_return_address(1);
	callers[2] = (uintptr_t)__builtin_return_address(2);
	callers[3] = (uintptr_t)__builtin_return_address(3);
	callers[4] = (uintptr_t)__builtin_return_address(4);
	for (unsigned i = 0; i < sizeof(callers) / sizeof(callers[0]); i++) {
		if (callers[i] < slide + CACHE_BASE || callers[i] >= slide + CACHE_BASE + length) {
			used += (size_t)snprintf(line + used, sizeof(line) - used, " (not the cache)");
			continue;
		}
		used += (size_t)snprintf(line + used, sizeof(line) - used, " %#lx", callers[i] - slide);
	}
	say("annibale-gl: %s, asked for by%s\n", what, line);
}

/*
 * A coarse look at a picture that arrives as ASTC 4x4, without asking the GPU for anything: a block
 * of it that holds one colour for all its sixteen pixels ("a void extent") carries that colour in
 * its last eight bytes, two bytes a channel, and an encoder writes those wherever the picture is
 * flat. Walking a grid of blocks and printing the top byte of red, green and blue of each therefore
 * shows the picture where it is flat and "??" where it is not -- enough to tell what the picture is.
 */
static void
astc_thumbnail(GLenum internal, GLsizei width, GLsizei height, GLsizei size, const void *data)
{
	const uint8_t *bytes = data;
	unsigned across, down, flat = 0, seen = 0;

	if (internal != 0x93b0 || data == NULL || width <= 0 || height <= 0) {
		return;                         /* only the four-by-four kind, and only with its bytes */
	}
	across = ((unsigned)width + 3) / 4;
	down = ((unsigned)height + 3) / 4;
	if ((size_t)size < (size_t)across * down * 16) {
		return;
	}
	for (unsigned row = 0; row < 16; row++) {
		char line[24 * 7 + 1] = "";
		size_t used = 0;
		unsigned by = down * row / 16;

		for (unsigned column = 0; column < 24; column++) {
			unsigned bx = across * column / 24;
			const uint8_t *block = bytes + 16 * ((size_t)by * across + bx);
			unsigned mode = block[0] | ((unsigned)block[1] << 8);

			seen++;
			if ((mode & 0x1ff) == 0x1fc) {
				flat++;
				used += (size_t)snprintf(line + used, sizeof(line) - used, " %02x%02x%02x",
				    block[9], block[11], block[13]);
			} else {
				used += (size_t)snprintf(line + used, sizeof(line) - used, " ......");
			}
		}
		say("annibale-gl: astc %u x %u, row %u:%s\n", across, down, by, line);
	}
	say("annibale-gl: astc %u x %u: %u of the %u blocks looked at hold one colour\n", across, down, flat, seen);
}

static void
my_glCompressedTexImage2D(void *context, GLenum target, GLint level, GLenum internal, GLsizei width, GLsizei height,
    GLint border, GLsizei size, const void *data)
{
	struct eagl *e = context;

	static unsigned told;

	EAGL_ENTER(e, 0xbe8);
	if (level == 0) {
		note_format("compressed", internal, 0, 0, width, height);
	}
	if (told < 40) {
		(void)glGetError();             /* so that what is left behind is this call's own */
	}
	glCompressedTexImage2D(target, level, internal, width, height, border, size, data);
	/*
	 * A format the driver has not got is refused here, and then the texture stays undefined and
	 * is read as whatever the driver leaves in it -- magenta, with some. The first of these are
	 * written down with what the call left behind, because the errors of every call are only
	 * asked for over the first six hundred frames (see eagl_leave) and the pictures of the
	 * switcher arrive long after that.
	 */
	if (told < 40) {
		GLenum error = glGetError();
		char head[3 * 32 + 1] = "";

		/*
		 * The first blocks, as bytes. A block of ASTC says in its first eleven bits how it is
		 * laid out, and whether it is one of the kinds that only a decoder with the HDR part of
		 * the extension can read -- the driver here has GL_KHR_texture_compression_astc_ldr and
		 * not the HDR one, and an LDR decoder hands back magenta for a block it cannot read.
		 */
		for (unsigned i = 0; data != NULL && i < 32 && (GLsizei)i < size; i++) {
			snprintf(head + 3 * i, 4, " %02x", ((const uint8_t *)data)[i]);
		}
		told++;
		say("annibale-gl: a compressed picture into texture %u: internal format %#x, %d x %d, "
		    "level %d, %d bytes%s, it begins%s\n", e->bound[e->unit], internal, width, height,
		    level, size, error == 0 ? "" : error == 0x500 ? " -- refused, no such format" : " -- refused",
		    head);
		astc_thumbnail(internal, width, height, size, data);
		say_callers("that compressed picture");
	}
	EAGL_LEAVE(e, 0xbe8);
}

static void
my_glBindFramebuffer(void *context, GLenum target, GLuint framebuffer)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1508);
	if (eagl_tracing) {
		eagl_trace("glBindFramebuffer(%#x, %u)\n", target, framebuffer);
	}
	clear_now(e);                   /* (a clear put off is of what was drawn into until now) */
	glBindFramebuffer(target, framebuffer);
	if (target == GL_READ_FRAMEBUFFER) {
		EAGL_LEAVE(e, 0x1508);
		return;                 /* (not what is drawn into) */
	}
	/*
	 * What is drawn into from here on is asked of the library, not kept by the framebuffer's
	 * name: CoreAnimation makes a framebuffer for every frame and the names do not come back,
	 * so there is no end of them (a table of 4096 was past its end six minutes into a tour,
	 * and from then on no frame was known to have been drawn into anything, or waited for).
	 */
	e->framebuffer = framebuffer;
	e->colour = 0;
	if (framebuffer != 0) {
		GLint kind = 0, name = 0;

		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
		if (kind == GL_TEXTURE) {
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
			e->colour = (GLuint)name;
		}
	}
	EAGL_LEAVE(e, 0x1508);
}

static void
my_glFramebufferTexture2D(void *context, GLenum target, GLenum attachment, GLenum to, GLuint texture, GLint level)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1530);
	clear_now(e);
	if (attachment == GL_COLOR_ATTACHMENT0 && target != GL_READ_FRAMEBUFFER) {
		e->colour = texture;
	}
	if (eagl_tracing) {
		eagl_trace("glFramebufferTexture2D(%#x, %#x, %#x, %u, %d)\n", target, attachment, to, texture, level);
	}
	glFramebufferTexture2D(target, attachment, to, texture, level);
	EAGL_LEAVE(e, 0x1530);
}

static void
my_glDeleteFramebuffers(void *context, GLsizei n, const GLuint *framebuffers)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1510);
	clear_now(e);
	for (GLsizei i = 0; i < n; i++) {
		if (framebuffers[i] == e->framebuffer) {
			e->framebuffer = e->colour = 0;
		}
	}
	glDeleteFramebuffers(n, framebuffers);
	EAGL_LEAVE(e, 0x1510);
}

/* A private entry of iOS's library (CA::OGL::GLESContext::set_surface calls it); nobody else has it. */
static void
my_glFramebufferParameteriAPPLE(void *context, GLenum target, GLenum name, GLint value)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x2008);
	if (slot_calls[0x2008 / 8] <= 4) {
		say("annibale-gl: glFramebufferParameteriAPPLE(%#x, %#x, %d): nothing done\n", target, name, value);
	}
}

/* EXT_debug_marker: names for a debugger's list of calls. */
static void
my_glInsertEventMarkerEXT(void *context, GLsizei length, const GLchar *marker)
{
	(void)length;
	(void)marker;
	EAGL_ENTER(context, 0x1988);
}

static void
my_glPushGroupMarkerEXT(void *context, GLsizei length, const GLchar *marker)
{
	(void)length;
	(void)marker;
	EAGL_ENTER(context, 0x1990);
}

static void
my_glPopGroupMarkerEXT(void *context)
{
	EAGL_ENTER(context, 0x1998);
}

/*
 * CoreAnimation's shaders are in the first version of the shading language, with one thing of
 * Apple's own in them: sampler2DRect and texture2DRect(), a texture read at coordinates counted in
 * its pixels (what it shows of IOSurfaces is read so). The language has no such thing, and in
 * its first version no way to make one, since a shader cannot ask how big a texture is. In
 * version 3.00 it can. So every shader is handed on as a 3.00 one: the words of the first
 * version that 3.00 dropped are defined back, and texture2DRect() divides by textureSize().
 * (All of a program's shaders must be of one version, so all are treated alike.)
 */
/*
 * CoreAnimation's shaders say "lowp" and "mediump" for nearly everything, as shaders written for
 * Apple's GPUs do. On an Adreno mediump is a float of sixteen bits, eleven of them the number: a
 * place in a texture of 1024 rows worked out so is right to half a row, and the table CoreAnimation
 * looks a gradient's colours up in (32 x 1024, a cube of colour laid out flat; its fragment shader
 * with "c_st") then gives sixty-odd shades where it has thousands. The Weather widget's sky was
 * drawn in horizontal bands by that. Everything is computed in full precision instead (the two
 * words are defined away, in vertex shaders as well: a uniform must be the same in both).
 * Boot-arg glprecision=0: as the shaders say.
 */
static int full_precision = 1;
static const char all_highp[] =
    "#define lowp highp\n"
    "#define mediump highp\n";

static const char fragment_start[] =
    "#define varying in\n"
    "#define texture2D texture\n"
    "#define texture2DProj textureProj\n"
    "#define texture2DLod textureLod\n"
    "#define textureCube texture\n"
    "#define sampler2DRect sampler2D\n"
    "#define texture2DRect(s, c) texture(s, (c) / vec2(textureSize(s, 0)))\n"
    "highp vec4 annibale_data[1];\n"
    "#define gl_FragData annibale_data\n"
    "#define gl_FragColor annibale_data[0]\n";
/*
 * What a fragment shader of the first version writes to (gl_FragData), and with the extension a
 * GPU's driver offers and CoreAnimation then uses (EXT_shader_framebuffer_fetch) reads what is
 * there already from (gl_LastFragData), are in 3.00 one variable of the shader's own, declared
 * "out" or "inout". The shader's main() is given another name and called from one that reads
 * that variable before and writes it after.
 */
static const char fragment_plain[] =
    "layout(location = 0) out highp vec4 annibale_colour;\n"
    "#define main annibale_main\n"
    "#line 1\n";
static const char fragment_plain_end[] =
    "\n#undef main\n"
    "void main() { annibale_main(); annibale_colour = annibale_data[0]; }\n";
static const char fragment_fetch[] =
    "layout(location = 0) inout highp vec4 annibale_colour;\n"
    "highp vec4 annibale_last[1];\n"
    "#define gl_LastFragData annibale_last\n"
    "#define main annibale_main\n"
    "#line 1\n";
static const char fragment_fetch_end[] =
    "\n#undef main\n"
    "void main() { annibale_last[0] = annibale_colour; annibale_main(); annibale_colour = annibale_data[0]; }\n";
static const char vertex_start[] =
    "#define attribute in\n"
    "#define varying out\n"
    "#define texture2D texture\n"
    "#define texture2DLod textureLod\n"
    "#line 1\n";

static GLuint
my_glCreateShader(void *context, GLenum type)
{
	struct eagl *e = context;
	GLuint shader;

	EAGL_ENTER(e, 0x1298);
	shader = glCreateShader(type);
	e->fragment[shader % 256] = type == GL_FRAGMENT_SHADER;
	EAGL_LEAVE(e, 0x1298);
	return shader;
}

/*
 * CoreAnimation does not ask whether its shaders compiled. Here it is asked, and what the
 * compiler said is written down with the shader's text.
 */
static void
my_glShaderSource(void *context, GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
	struct eagl *e = context;
	bool fragment = e->fragment[shader % 256], fetch;
	const GLchar *parts[6] = { "#version 300 es\n", fragment ? fragment_start : vertex_start, "", NULL, "",
		                   full_precision ? all_highp : "" };
	size_t size = 1, extensions = 0;
	char *text, *line, *head;

	EAGL_ENTER(e, 0x12a0);
	for (GLsizei i = 0; i < count; i++) {
		size += lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
	}
	if ((text = calloc(1, 2 * size + 64)) == NULL) {
		glShaderSource(shader, count, strings, lengths);
		return;
	}
	for (GLsizei i = 0; i < count; i++) {
		strncat(text, strings[i], lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]));
	}
	/* "#extension" lines must stay ahead of everything that is not the preprocessor's: they move to the top */
	head = text + size;
	strcpy(head, parts[0]);
	for (line = text; (line = strstr(line, "#extension")) != NULL; line += 2) {
		size_t length = strcspn(line, "\n");

		strncat(head, line, length);
		strcat(head, "\n");
		line[0] = line[1] = '/';
		extensions++;
	}
	parts[0] = head;
	parts[3] = text;
	if (fragment) {
		fetch = strstr(text, "gl_LastFragData") != NULL;
		parts[2] = fetch ? fragment_fetch : fragment_plain;
		parts[4] = fetch ? fragment_fetch_end : fragment_plain_end;
	}
	free((void *)e->sources[shader % 256]);
	e->sources[shader % 256] = text;
	glShaderSource(shader, 6, (const GLchar *const[6]){ parts[0], parts[5], parts[1], parts[2], parts[3], parts[4] }, NULL);
	EAGL_LEAVE(e, 0x12a0);
}

static void
say_long(const char *what, const char *text)
{
	say("annibale-gl: %s:\n", what);
	for (size_t at = 0, length = strlen(text); at < length; at += 900) {
		write(2, text + at, length - at < 900 ? length - at : 900);
	}
	say("\n");
}

static void
my_glCompileShader(void *context, GLuint shader)
{
	struct eagl *e = context;
	GLint good = 0;
	char said[1024] = "";

	EAGL_ENTER(e, 0x12a8);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &good);
	e->shaders++;
	if (!good) {
		glGetShaderInfoLog(shader, sizeof(said), NULL, said);
		say("annibale-gl: A SHADER DOES NOT COMPILE (shader %u): %s\n", shader, said);
		if (e->sources[shader % 256] != NULL) {
			say_long("its text", e->sources[shader % 256]);
		}
	} else if ((e->shaders <= 2 || all_shaders) && e->sources[shader % 256] != NULL) {
		say("annibale-gl: shader %u:\n", shader);
		say_long(all_shaders ? "a shader" : "the first shaders, for the record", e->sources[shader % 256]);
	}
	EAGL_LEAVE(e, 0x12a8);
}

static void
my_glLinkProgram(void *context, GLuint program)
{
	struct eagl *e = context;
	GLint good = 0;
	char said[1024] = "";

	EAGL_ENTER(e, 0x12c0);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	e->programs++;
	if (!good) {
		glGetProgramInfoLog(program, sizeof(said), NULL, said);
		say("annibale-gl: A PROGRAM DOES NOT LINK (program %u): %s\n", program, said);
	}
	EAGL_LEAVE(e, 0x12c0);
}

/* iOS's library has one entry for deleting a program and deleting a shader. */
static void
my_glDeleteProgram(void *context, GLuint name)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1280);
	if (glIsProgram(name)) {
		glDeleteProgram(name);
	} else {
		glDeleteShader(name);
	}
	EAGL_LEAVE(e, 0x1280);
}

#define EAGL_GEN_TABLE
#include "eagl_gen.h"
#undef EAGL_GEN_TABLE

_Static_assert(EAGL_TABLE_SIZE <= SLOTS * 8, "the table has grown");

/* ---- EAGLContext ---------------------------------------------------------------------------------- */

static id
my_initWithAPI(id self, SEL selector, unsigned long api, id properties)
{
	struct eagl *e;
	unsigned place;

	(void)selector;
	(void)properties;
	for (place = 0; place < sizeof(contexts) / sizeof(contexts[0]) && contexts[place] != NULL; place++) {
	}
	if (api < 2 || api > 3 || place == sizeof(contexts) / sizeof(contexts[0]) || (e = calloc(1, sizeof(*e))) == NULL) {
		say("annibale-gl: no context of API %lu\n", api);
		return nil;
	}
	if ((e->gl = annibale_gl_create((int)api)) == NULL || !annibale_gl_make_current(e->gl)) {
		say("annibale-gl: %s gives no OpenGL ES %lu context\n", annibale_gl_library(), api);
		free(e);
		return nil;
	}
	current = e;
	e->object = self;
	e->api = api;
	e->table[0] = e;
	eagl_fill_table(e->table);
	contexts[place] = e;
	say("annibale-gl: an OpenGL ES %lu context for CoreAnimation, by %s: %s; %s; shaders %s\n", api, annibale_gl_library(),
	    (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
	    (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
	return self;
}

static unsigned long
my_API(id self, SEL selector)
{
	struct eagl *e = context_of(self);

	(void)selector;
	return e ? e->api : 0;
}

/*
 * -[EAGLContext sharegroup]. CoreAnimation never asks for it, but CoreImage does: a CIContext made
 * over an EAGLContext goes +[CIContext contextWithEAGLContext:] -> internalContextWithEAGLContext:
 * -> CI::GLContext::GLContext -> wrapEAGLContextCreate -> -[EAGLContext sharegroup], and iOS's own
 * accessor reads ivars of an EAGLContext that my_initWithAPI never filled in (it keeps its state in
 * `struct eagl` beside the object and does not call through to Apple's init). On the phone that
 * crashed in -[EAGLContext sharegroup]+4 touching address 0x8. Nothing here shares a context with
 * anybody, so the honest answer is none; what CoreImage then does is the next thing to find out.
 */
static id
my_sharegroup(id self, SEL selector)
{
	static int said;

	(void)self;
	(void)selector;
	if (!said) {
		said = 1;
		say("annibale-gl: -sharegroup asked for: none (nothing here shares a context)\n");
	}
	return nil;
}

static unsigned long
my_setParameter(id self, SEL selector, unsigned parameter, const int32_t *values)
{
	(void)self;
	(void)selector;
	say("annibale-gl: -setParameter:%#x to:%d: noted\n", parameter, values ? values[0] : 0);
	return 0;
}

static void *
my_getMacroContextPrivate(id self, SEL selector)
{
	struct eagl *e = context_of(self);

	(void)selector;
	return e ? e->table : NULL;
}

/* (the types are the method's own: the last three arrive on the stack, where sizes matter) */
static BOOL
my_texImageIOSurface(id self, SEL selector, IOSurfaceRef surface, unsigned long target, unsigned long internal,
    GLsizei width, GLsizei height, unsigned long format, unsigned long type, uint32_t plane, BOOL invert)
{
	static unsigned shown;
	struct eagl *e = context_of(self);
	struct image *image;
	GLint texture = 0;
	uint64_t began;

	(void)selector;
	if (e == NULL || surface == NULL) {
		return NO;
	}
	eagl_enter(e, 0);
	clear_now(e);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	if (target != GL_TEXTURE_2D || texture == 0 || (image = image_new(e, (GLuint)texture, surface)) == NULL) {
		say("annibale-gl: -texImageIOSurface: target %#lx, texture %d: not done\n", target, texture);
		return NO;
	}
	image->to = (GLenum)target;
	image->internal = (GLenum)internal;
	image->format = (GLenum)format;
	image->type = (GLenum)type;
	image->plane = plane;
	image->invert = invert;
	image->width = width;
	image->height = height;
	note_format(invert ? "IOSurface shown, inverted" : "IOSurface shown", image->internal, image->format, image->type, width,
	    height);
	if (shown++ < 24) {
		say("annibale-gl: -texImageIOSurface: surface %u ('%.4s' %zu x %zu, %zu planes) as texture %d: %d x %d, plane %u%s\n",
		    IOSurfaceGetID(surface), (const char *)&(uint32_t){ __builtin_bswap32(IOSurfaceGetPixelFormat(surface)) },
		    IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface), IOSurfaceGetPlaneCount(surface), texture, width,
		    height, plane, invert ? ", inverted" : "");
	}
	image->wrapped = IOSurfaceGetPixelFormat(surface) == 0;
	began = now_us();
	if (!surface_shared(e, image)) {
		surface_changed(e, image);              /* (what it adds up to now) */
		surface_to_texture(e, image);
	}
	e->attaching += now_us() - began;
	eagl_leave(e, 0x970);
	return YES;
}

/* (the "image" is the target, GL_TEXTURE_2D, and means the texture bound to it) */
static BOOL
my_attachImage(id self, SEL selector, unsigned long target, IOSurfaceRef surface, BOOL inverted)
{
	struct eagl *e = context_of(self);
	static unsigned attached;
	struct image *image;
	GLint texture = 0;
	uint32_t pixels;
	uint64_t began;

	(void)selector;
	if (e == NULL || surface == NULL) {
		return NO;
	}
	eagl_enter(e, 0);
	clear_now(e);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	pixels = IOSurfaceGetPixelFormat(surface);
	if (target != GL_TEXTURE_2D || texture == 0 || (pixels != 'BGRA' && pixels != 0) ||
	    IOSurfaceGetBytesPerElement(surface) != 4 || IOSurfaceGetPlaneCount(surface) != 0 ||
	    (image = image_new(e, (GLuint)texture, surface)) == NULL) {
		say("annibale-gl: -attachImage: target %#lx, texture %d, an IOSurface of '%.4s': not done\n", target, texture,
		    (const char *)&(uint32_t){ __builtin_bswap32(pixels) });
		return NO;
	}
	image->target = true;
	image->to = GL_TEXTURE_2D;
	image->invert = inverted;
	image->width = (GLsizei)IOSurfaceGetWidth(surface);
	image->height = (GLsizei)IOSurfaceGetHeight(surface);
	image->internal = image->format = GL_BGRA;
	image->type = GL_UNSIGNED_BYTE;
	/*
	 * A surface with no pixel format is memory CoreAnimation has made a surface around: a
	 * picture to be shown, as an application has drawn it. Those that come this way are mostly
	 * the signs applications draw to be tinted -- black, so nothing but opacity -- and their
	 * memory is opacity first, then blue, green, red (seen: the first byte of every four is all
	 * there is in the black ones, and the one sign with colours in it, blue in the software
	 * renderer's picture, comes out orange if red is taken to follow the opacity). Read
	 * blue-first, like the surfaces that are drawn into, the black ones have no opacity at all
	 * and are not seen. iOS's library has the order from the surface; here it is taken from
	 * there being no pixel format.
	 */
	image->wrapped = image->alpha_first = pixels == 0;
	note_format("IOSurface attached", GL_BGRA, GL_BGRA, GL_UNSIGNED_BYTE, image->width, image->height);
	/*
	 * What is attached is as often read as drawn into (a picture CoreAnimation has made a surface
	 * around, to be shown): a texture that is not the surface's memory starts with what the surface holds.
	 */
	began = now_us();
	if (surface_mirrored(e, image) || !surface_shared(e, image)) {
		surface_changed(e, image);              /* (what it adds up to now) */
		surface_to_texture(e, image);
	}
	e->attaching += now_us() - began;
	if (attached++ < 24) {
		say("annibale-gl: -attachImage: texture %d is IOSurface %u ('%.4s' %d x %d, %zu bytes a row)%s%s\n", texture,
		    IOSurfaceGetID(surface), (const char *)&(uint32_t){ __builtin_bswap32(pixels) }, image->width, image->height,
		    IOSurfaceGetBytesPerRow(surface), inverted ? ", drawn inverted" : "", image->shared ? ": its memory" :
		    image->mirror != 0 ? ": a texture, copied into its memory by the GPU" : ": a copy");
	}
	eagl_leave(e, 0x970);
	return YES;
}

static void
my_dealloc(id self, SEL selector)
{
	struct objc_super super = { self, class_getSuperclass(objc_getClass("EAGLContext")) };
	struct eagl *e = context_of(self);

	if (e != NULL) {
		say("annibale-gl: the context goes\n");
		summary(e);
		for (unsigned i = 0; i < sizeof(contexts) / sizeof(contexts[0]); i++) {
			if (contexts[i] == e) {
				contexts[i] = NULL;
			}
		}
		if (current == e) {
			annibale_gl_make_current(NULL);
			current = NULL;
		}
		annibale_gl_destroy(e->gl);
		free(e);
	}
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&super, selector);
}

/* Does the display's driver wait for the GPU before it shows a surface (iokit/Kernel/MiniIOMobileFramebuffer.cpp)? */
static bool
display_waits(void)
{
	io_service_t display = IOServiceGetMatchingService(0, IOServiceMatching("IOMobileFramebuffer"));
	CFTypeRef says;
	bool waits;

	if (display == 0) {
		return false;
	}
	says = IORegistryEntryCreateCFProperty(display, CFSTR("AnnibaleWaitsForGPU"), kCFAllocatorDefault, 0);
	waits = says == kCFBooleanTrue;
	if (says != NULL) {
		CFRelease(says);
	}
	IOObjectRelease(display);
	return waits;
}

__attribute__((constructor)) static void
install(void)
{
	static const struct {
		const char *name;
		IMP ours;
	} methods[] = {
		{ "initWithAPI:properties:", (IMP)my_initWithAPI },
		{ "API", (IMP)my_API },
		{ "sharegroup", (IMP)my_sharegroup },
		{ "setParameter:to:", (IMP)my_setParameter },
		{ "getMacroContextPrivate", (IMP)my_getMacroContextPrivate },
		{ "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:", (IMP)my_texImageIOSurface },
		{ "attachImage:toCoreSurface:invertedRender:", (IMP)my_attachImage },
		{ "dealloc", (IMP)my_dealloc },
	};
	Class class = objc_getClass("EAGLContext");
	unsigned replaced = 0;
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;

	/* the kernel's boot-args are where a run is told things */
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		if ((argument = strstr(arguments, "gltrace=")) != NULL) {
			trace_last = trace_first = (unsigned)strtoul(argument + 8, (char **)&argument, 10);
			if (*argument == '-') {
				trace_last = (unsigned)strtoul(argument + 1, NULL, 10);
			}
		}
		if ((argument = strstr(arguments, "gltrace_at=")) != NULL) {
			trace_at = now_us() + strtoull(argument + 11, NULL, 10) * 1000000;
		}
		if ((argument = strstr(arguments, "gltrace_draws=")) != NULL) {
			trace_draws = (unsigned)strtoul(argument + 14, NULL, 10);
		}
		if ((argument = strstr(arguments, "gltrace_function=")) != NULL) {
			trace_function = (int)strtoul(argument + 17, (char **)&argument, 10);
			trace_function_after = *argument == ',' ? (int)strtoul(argument + 1, NULL, 10) : 1;
		}
		if ((argument = strstr(arguments, "gltrace_frames=")) != NULL) {
			trace_frames = (unsigned)strtoul(argument + 15, NULL, 10);
		}
		trace_lit = strstr(arguments, "gltrace_lit=1") != NULL;
		trace_look = strstr(arguments, "gltrace_look=1") != NULL;
		trace_shaders = strstr(arguments, "gltrace_shaders=1") != NULL;
		function_numbers = strstr(arguments, "glnumbers=1") != NULL;
		trace_uniforms_on = strstr(arguments, "gltrace_uniforms=1") != NULL;
		if ((argument = strstr(arguments, "gltrace_corners=")) != NULL) {
			trace_corners = (GLsizei)strtol(argument + 16, NULL, 10);
		}
		fetch_offered = strstr(arguments, "glfetch=0") == NULL;
		group_vibrant_on = strstr(arguments, "glgroup=0") == NULL;
		if ((argument = strstr(arguments, "gltrace_place=")) != NULL) {
			char *end = (char *)argument + 14;

			for (unsigned i = 0; i < 4; i++) {
				trace_place[i] = strtof(end, &end);
				end += *end == ',';
			}
			trace_placed = 1;
		}
		all_shaders = strstr(arguments, "glshaders=1") != NULL;
		census_on = strstr(arguments, "glcensus=1") != NULL;
		frame_lines = strstr(arguments, "glframes=1") != NULL;
		latency_lines = strstr(arguments, "touch_lat=1") != NULL;
		if ((argument = strstr(arguments, "glslow=")) != NULL) {
			slow_calls_us = strtoull(argument + 7, NULL, 10);
		}
		full_precision = strstr(arguments, "glprecision=0") == NULL;
		vblur_on = strstr(arguments, "vblur=0") != NULL ? 0 : strstr(arguments, "vblur=2") != NULL ? 2 : 1;
		if ((argument = strstr(arguments, "glplain=")) != NULL) {
			plain_draws = (int)strtoul(argument + 8, NULL, 10);
		}
		if ((argument = strstr(arguments, "glskip=")) != NULL) {
			const char *at = argument + 7;

			while (skipping < sizeof(skipped) / sizeof(skipped[0])) {
				skipped[skipping++] = (GLuint)strtoul(at, NULL, 10);
				if ((at = strchr(at, ',')) == NULL) {
					break;
				}
				at++;
			}
		}
		if ((argument = strstr(arguments, "glshow=")) != NULL) {
			if (strchr(argument + 7, 'x') != NULL && strchr(argument + 7, 'x') < argument + 7 + strcspn(argument + 7, " ")) {
				shown_wanted[0] = (GLint)strtol(argument + 7, NULL, 10);
				shown_wanted[1] = (GLint)strtol(strchr(argument + 7, 'x') + 1, NULL, 10);
				shown_texture = 1;              /* (that there is one to show at all) */
			} else {
				shown_texture = (GLuint)strtoul(argument + 7, NULL, 10);
			}
			if ((argument = strstr(arguments, "glshow_part=")) != NULL) {
				shown_part[0] = (GLint)strtol(argument + 12, NULL, 10);
				shown_part[1] = strchr(argument + 12, ',') != NULL ?
				    (GLint)strtol(strchr(argument + 12, ',') + 1, NULL, 10) : shown_part[0];
			}
			if ((argument = strstr(arguments, "glshow_zoom=")) != NULL) {
				shown_zoom = (GLint)strtol(argument + 12, NULL, 10);
				shown_zoom = shown_zoom < 1 ? 1 : shown_zoom;
			}
		}
		share_surfaces = strstr(arguments, "glcopy=1") == NULL;
		blit_targets = strstr(arguments, "glblit=0") == NULL;
		sync_frames = strstr(arguments, "glsync=0") == NULL;
		if (strstr(arguments, "glwait=0") != NULL || (strstr(arguments, "glwait=1") == NULL && display_waits())) {
			wait_for_frames = 0;
		}
		say("annibale-gl: a frame is waited for %s\n", wait_for_frames ? "here, before it is handed to the display" :
		    "by the display's driver, not here");
		for (argument = arguments; (argument = strstr(argument, "glenv=")) != NULL; argument += 6) {
			char setting[128];
			size_t length = strcspn(argument + 6, " ");
			char *value;

			if (length < sizeof(setting)) {
				memcpy(setting, argument + 6, length);
				setting[length] = 0;
				if ((value = strchr(setting, '=')) != NULL) {
					*value = 0;
					setenv(setting, value + 1, 1);
					say("annibale-gl: in the environment: %s=%s\n", setting, value + 1);
				}
			}
		}
	}

	if (class == Nil) {
		say("annibale-gl: this program has no EAGLContext: nothing to do\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
		SEL selector = sel_registerName(methods[i].name);
		Method method = class_getInstanceMethod(class, selector);

		if (method == NULL) {
			say("annibale-gl: EAGLContext has no -%s\n", methods[i].name);
			continue;
		}
		say("annibale-gl: -[EAGLContext %s] (%s) replaced\n", methods[i].name, method_getTypeEncoding(method));
		if (method == class_getInstanceMethod(class_getSuperclass(class), selector)) {
			/* inherited: the class gets one of its own, the one it inherits is everybody's */
			class_addMethod(class, selector, methods[i].ours, method_getTypeEncoding(method));
		} else {
			method_setImplementation(method, methods[i].ours);
		}
		replaced++;
	}
	say("annibale-gl: %u of %zu methods of EAGLContext are ours; OpenGL ES by %s\n", replaced,
	    sizeof(methods) / sizeof(methods[0]), annibale_gl_library());
}
