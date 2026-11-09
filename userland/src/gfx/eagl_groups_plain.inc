/* One of our programs, made: CoreAnimation's own vertex arrays go in at the places it has them. */
static GLuint
plain_program(const char *fragment, const char *what)
{
	GLuint program = glCreateProgram(), shader;
	const GLchar *text;
	GLint good = 0;

	shader = glCreateShader(GL_VERTEX_SHADER);
	text = plain_vertex;
	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glAttachShader(program, shader);
	shader = glCreateShader(GL_FRAGMENT_SHADER);
	text = fragment;
	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glAttachShader(program, shader);
	glBindAttribLocation(program, 0, "vertex_position");
	glBindAttribLocation(program, 1, "vertex_color");
	glBindAttribLocation(program, 2, "vertex_texcoord0");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	say("annibale-gl: a program of ours for the draws CoreAnimation makes with none (%s): %s\n", what, good ? "made" : "NOT MADE");
	return program;
}

/*
 * A census (boot-arg glcensus=1) of what CoreAnimation's OpenGL ES renderer has no shader for and
 * meets all the same: every draw it makes with no program, by function, and every time it asks
 * for a function and is told there is none (it then draws the thing another way, or not at
 * all). Written down: each case the first time it is met, with what it is made of -- where on
 * which framebuffer, the textures, the blending, the numbers CoreAnimation has for the function
 * (a filter's inputs) and how the draw is made here; every ten seconds, what there was of each
 * in those seconds (which is what tells the screens of a scenario apart); and with that the
 * count of each since the start.
 */
enum { CENSUS_NOT_DRAWN, CENSUS_PLAIN, CENSUS_TABLE, CENSUS_VIBRANT, CENSUS_MASKING, CENSUS_BOX, CENSUS_BLUR4, CENSUS_LEVEL,
       CENSUS_VBLUR, CENSUS_KINDS };
static const char *const census_kinds[CENSUS_KINDS] = {
	"NOT DRAWN (no program, and no texture, or nothing of ours to draw it with, or it would read what it writes)", "drawn as the picture it is given (colour times texture)",
	"drawn through a table (ours, for the home indicator)", "drawn with our program for a colour matrix over what is behind",
	"drawn with our program for a colour matrix over premultiplied pixels",
	"drawn with our program that makes a picture eight times smaller",
	"drawn with our program that averages four samples around a point (a shadow's blur)",
	"made into a level of ours, half the size of the one before (a blur whose radius varies)",
	"drawn with our program that reads the levels by the mask (a blur whose radius varies)",
};
static int census_on;
static struct census {
	unsigned draws[CENSUS_KINDS], lately[CENSUS_KINDS];
	unsigned refused, refused_lately;
	uint8_t said;                   /* a bit for each kind met */
} census[90];
static uint64_t census_told;

static const char *
census_name(unsigned function)
{
	static const char names[] = ANNIBALE_CA_FUNCTIONS;
	static char name[4];

	memcpy(name, function < 90 ? names + 4 * function : "?  ", 3);
	return name;
}

/* From inject.c: CoreAnimation asked whether the function it is about to use exists, and was told no. */
static uint16_t frame_refused[90], frame_noprog[90][CENSUS_KINDS];     /* (glframes=1) this frame's, by function */

void
annibale_gl_function_refused(unsigned function)
{
	if (frame_lines && function < 90) {
		frame_refused[function]++;
	}
	if (census_on && function < 90) {
		census[function].refused++;
		census[function].refused_lately++;
	}
}

static void
census_draw(struct eagl *e, unsigned function, unsigned kind)
{
	char textures[200] = "", numbers[200] = "";
	GLint source = 0, destination = 0, viewport[4] = { 0, 0, 0, 0 };
	float low[2] = { 0, 0 }, high[2] = { 0, 0 };
	size_t used = 0;

	if (frame_lines && function < 90) {
		frame_noprog[function][kind]++;
	}
	if (!census_on || function >= 90) {
		return;
	}
	census[function].draws[kind]++;
	census[function].lately[kind]++;
	if (census[function].said & (1u << kind)) {
		return;
	}
	census[function].said |= (uint8_t)(1u << kind);
	for (unsigned unit = 0; unit < 4; unit++) {
		GLint width = 0, height = 0, format = 0;

		if (e->bound[unit] == 0) {
			continue;
		}
		glActiveTexture(GL_TEXTURE0 + unit);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
		used += (size_t)snprintf(textures + used, sizeof(textures) - used, " [%u] %d x %d (%#x)", unit, width, height,
		    (unsigned)format);
	}
	glActiveTexture(GL_TEXTURE0 + e->unit);
	for (unsigned i = 0; i < 4 && e->arrays[0].pointer != NULL && e->arrays[0].type == GL_FLOAT && e->arrays[0].size >= 2; i++) {
		float place[2];

		memcpy(place, (const uint8_t *)e->arrays[0].pointer + i * (size_t)(e->arrays[0].stride ? e->arrays[0].stride :
		    4 * e->arrays[0].size), sizeof(place));
		for (unsigned c = 0; c < 2; c++) {
			low[c] = i == 0 || place[c] < low[c] ? place[c] : low[c];
			high[c] = i == 0 || place[c] > high[c] ? place[c] : high[c];
		}
	}
	used = 0;
	for (unsigned i = 0; i < function_given[function] && used < sizeof(numbers) - 16; i++) {
		used += (size_t)snprintf(numbers + used, sizeof(numbers) - used, " %.3g", (double)function_values[function][i]);
	}
	glGetIntegerv(GL_BLEND_SRC_RGB, &source);
	glGetIntegerv(GL_BLEND_DST_RGB, &destination);
	glGetIntegerv(GL_VIEWPORT, viewport);
	say("annibale-gl: census: function %u (\"%s\") with no program, %s: first at %g,%g to %g,%g of framebuffer %u (%d x %d); "
	    "textures%s; blending %s (%#x, %#x); its numbers:%s\n", function, census_name(function), census_kinds[kind], (double)low[0],
	    (double)low[1], (double)high[0], (double)high[1], e->framebuffer, viewport[2], viewport[3], textures[0] ? textures : " none",
	    glIsEnabled(GL_BLEND) ? "on" : "off", (unsigned)source, (unsigned)destination, numbers[0] ? numbers : " none");
}

/* Every ten seconds: what there was since the last time, and in all. */
/*
 * (glframes=1) after a frame's line, when it is not what the frame before had: the functions
 * CoreAnimation was refused in it, its draws with no program by function and kind (census_kinds[]),
 * and what was done with groups that hold a vibrant layer (group_draw_begin()).
 */
static void
frame_functions(struct eagl *e)
{
	static char said[600];
	char text[600] = "";
	size_t used = 0;

	for (unsigned f = 0; f < 90 && used < sizeof(text) - 40; f++) {
		if (frame_refused[f] != 0) {
			used += (size_t)snprintf(text + used, sizeof(text) - used, " refused %u %s x%u;", f, census_name(f), frame_refused[f]);
		}
		for (unsigned k = 0; k < CENSUS_KINDS; k++) {
			if (frame_noprog[f][k] != 0 && used < sizeof(text) - 40) {
				used += (size_t)snprintf(text + used, sizeof(text) - used, " %u %s kind %u x%u;", f, census_name(f), k,
				    frame_noprog[f][k]);
			}
		}
	}
	if (group_covered + group_laid + group_flattened != 0 && used < sizeof(text) - 60) {
		used += (size_t)snprintf(text + used, sizeof(text) - used, " groups: %u covered, %u laid, %u flattened;", group_covered,
		    group_laid, group_flattened);
	}
	group_covered = group_laid = group_flattened = 0;
	if (used != 0 && strcmp(text, said) != 0) {
		say("annibale-gl: f %u functions:%s\n", e->frames, text);
	}
	memcpy(said, text, used + 1);
	bzero(frame_refused, sizeof(frame_refused));
	bzero(frame_noprog, sizeof(frame_noprog));
}

static void
census_report(void)
{
	char lately[900] = "", all[900] = "";
	size_t used_lately = 0, used_all = 0;
	uint64_t now = now_us();

	if (!census_on || now - census_told < 10000000) {
		return;
	}
	census_told = now;
	for (unsigned f = 0; f < 90; f++) {
		struct census *c = &census[f];
		unsigned drawn = 0, drawn_lately = 0;

		for (unsigned k = 0; k < CENSUS_KINDS; k++) {
			drawn += c->draws[k];
			drawn_lately += c->lately[k];
			c->lately[k] = 0;
		}
		if ((drawn_lately != 0 || c->refused_lately != 0) && used_lately < sizeof(lately) - 40) {
			used_lately += (size_t)snprintf(lately + used_lately, sizeof(lately) - used_lately, " %u %s x%u", f, census_name(f),
			    drawn_lately);
			if (c->refused_lately != 0) {
				used_lately += (size_t)snprintf(lately + used_lately, sizeof(lately) - used_lately, " (refused x%u)", c->refused_lately);
			}
		}
		if ((drawn != 0 || c->refused != 0) && used_all < sizeof(all) - 60) {
			used_all += (size_t)snprintf(all + used_all, sizeof(all) - used_all, " %u %s x%u", f, census_name(f), drawn);
			if (c->draws[CENSUS_NOT_DRAWN] != 0) {
				used_all += (size_t)snprintf(all + used_all, sizeof(all) - used_all, " (x%u not drawn)", c->draws[CENSUS_NOT_DRAWN]);
			}
			if (c->refused != 0) {
				used_all += (size_t)snprintf(all + used_all, sizeof(all) - used_all, " (refused x%u)", c->refused);
			}
		}
		c->refused_lately = 0;
	}
	if (lately[0] != 0) {
		say("annibale-gl: census: in the last ten seconds:%s\n", lately);
		say("annibale-gl: census: since the start:%s\n", all);
	}
}

/* A draw of "vrb" (function 39) made with our program over the chain: true if it can be. */
static bool
vblur_draw_begin(struct eagl *e, struct vblur_chain *chain, GLint enabled[4])
{
	static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	static unsigned told;
	int levels = chain->made < 5 ? chain->made : 5;

	if (e->vblur == 0) {
		e->vblur = vblur_program(vblur_draw_vertex, vblur_fragment, "reading the levels by the mask");
		if (e->vblur == 0) {
			vblur_on = 0;                   /* (better the stand-in than nothing) */
			return false;
		}
		glUseProgram(e->vblur);
		glUniform1i(glGetUniformLocation(e->vblur, "level0"), 0);
		glUniform1i(glGetUniformLocation(e->vblur, "mask"), 1);
		for (int k = 1; k < 5; k++) {
			char name[8] = "levelN";

			name[5] = (char)('0' + k);
			glUniform1i(glGetUniformLocation(e->vblur, name), k + 1);
		}
		e->vblur_matrix = glGetUniformLocation(e->vblur, "vertex_matrix");
		e->vblur_radius = glGetUniformLocation(e->vblur, "radius");
		e->vblur_levels = glGetUniformLocation(e->vblur, "levels");
		e->vblur_normalize = glGetUniformLocation(e->vblur, "normalize_edges");
		e->vblur_paint = glGetUniformLocation(e->vblur, "paint");
	}
	glUseProgram(e->vblur);
	glUniformMatrix4fv(e->vblur_matrix, 1, 0, e->matrix != NULL ? e->matrix : identity);
	glUniform1f(e->vblur_radius, function_values[FUNCTION_VARIABLE_BLUR][0]);
	glUniform1i(e->vblur_levels, levels);
	glUniform1f(e->vblur_normalize, function_values[FUNCTION_VARIABLE_BLUR][1]);
	glUniform1i(e->vblur_paint, vblur_on == 2);
	for (int k = 1; k < 5; k++) {
		glActiveTexture(GL_TEXTURE0 + k + 1);
		glBindTexture(GL_TEXTURE_2D, k < chain->made ? chain->level[k] : e->bound[0]);
	}
	glActiveTexture(GL_TEXTURE0 + e->unit);
	e->vblur_bound = true;
	for (GLuint i = 0; i < 4; i++) {
		glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
		if (!enabled[i]) {
			glEnableVertexAttribArray(i);
		}
	}
	if (told++ < 4) {
		const uint8_t *at = e->arrays[3].pointer;
		float first[2] = { 0, 0 };

		if (at != NULL && e->arrays[3].type == GL_FLOAT) {
			memcpy(first, at, sizeof(first));
		}
		say("annibale-gl: a blur whose radius varies, read by its mask: %d levels of texture %u (%d x %d), radius %g, numbers %g %g "
		    "%g; the mask's coordinates at the first corner %g %g\n", levels, chain->picture, chain->width, chain->height,
		    (double)function_values[FUNCTION_VARIABLE_BLUR][0], (double)function_values[FUNCTION_VARIABLE_BLUR][1],
		    (double)function_values[FUNCTION_VARIABLE_BLUR][2], (double)function_values[FUNCTION_VARIABLE_BLUR][3],
		    (double)first[0], (double)first[1]);
	}
	if (vblur_on == 2 && told <= 2) {
		/* the mask itself, for the log: a pixel in every twelve, blue green red opacity */
		uint8_t column[4 * 512];
		GLint height = 0;
		char said[1024];
		size_t used = 0;

		glActiveTexture(GL_TEXTURE0 + 1);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glActiveTexture(GL_TEXTURE0 + e->unit);
		if (height > 0 && height <= 512) {
			if (e->reader == 0) {
				glGenFramebuffers(1, &e->reader);
			}
			glBindFramebuffer(GL_FRAMEBUFFER, e->reader);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->bound[1], 0);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, 1, height, GL_BGRA, GL_UNSIGNED_BYTE, column);
			glPixelStorei(GL_PACK_ALIGNMENT, 4);
			glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
			for (GLint y = 0; y < height && used < sizeof(said) - 32; y += 12) {
				used += (size_t)snprintf(said + used, sizeof(said) - used, " %d:%u,%u,%u,%u", y, column[4 * y], column[4 * y + 1],
				    column[4 * y + 2], column[4 * y + 3]);
			}
			say("annibale-gl: the mask of the blur whose radius varies (texture %u, 1 x %d), from its first row:%s (error %#x)\n",
			    e->bound[1], height, said, glGetError());
		}
	}
	return true;
}

/*
 * A vibrant layer inside a group. While a layer of Control Center fades in (its opacity below 1),
 * CoreAnimation draws it and what is in it into a picture of its own, a group, and lays that over
 * the screen at the layer's opacity; at 1 it draws them straight onto the screen. The round
 * buttons of the connectivity module and the "Not Playing" title are vibrant: function 45 ("vcm",
 * our program for a colour matrix over what is behind, which reads the picture it draws into),
 * and in a group what is behind them is the group's picture, cleared to nothing: the colour matrix
 * of nothing is nothing, and they were missing from the screen until the fade was over, the
 * buttons' discs and the title coming in at once some half a second after the rest.
 *
 * So a vibrant draw into a group that holds nothing yet keeps there what it covers (its layer's
 * opacity times its mask), and the matrix with it; when that group is laid over the screen (or
 * over another picture with something in it), our program puts the matrix over what is there
 * by that coverage times the group's opacity -- which is what the group would have given had it
 * been started from what is behind it, and what the draw at full opacity gives. A group that
 * holds a vibrant layer's coverage and is then drawn into, or read any other way, is first made
 * what it would have been (the colour matrix of nothing, by the coverage). A group laid into
 * another group that holds nothing yet passes its coverage on. Boot-arg glgroup=0: as before.
 */

static const char group_cover_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform highp vec4 texmat;\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "void main() {\n"
    "  result = color * texture(texture0, texcoord * texmat.xy + texmat.zw);\n"
    "}\n";
static const char group_over_fragment[] =
    "#version 300 es\n"
    "#extension GL_EXT_shader_framebuffer_fetch : require\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform highp vec4 texmat;\n"
    "uniform mediump vec4 colours[5];\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "layout(location = 0) inout highp vec4 result;\n"
    "void main() {\n"
    "  mediump vec4 layer = color * texture(texture0, texcoord * texmat.xy + texmat.zw);\n"
    "  mediump vec4 behind = result, s = behind;\n"
    "  s.rgb = s.rgb / max(s.a, .005);\n"
    "  s = s.r * colours[0] + s.g * colours[1] + s.b * colours[2] + s.a * colours[3] + colours[4];\n"
    "  s.a = clamp(s.a, 0., 1.);\n"
    "  s.rgb = clamp(s.rgb * s.a, 0., 1.);\n"
    "  result = mix(behind, s, layer.a);\n"
    "}\n";
static const char group_flat_fragment[] =
    "#version 300 es\n"
    "#extension GL_EXT_shader_framebuffer_fetch : require\n"
    "uniform mediump vec4 flat_colour;\n"
    "layout(location = 0) inout highp vec4 result;\n"
    "void main() {\n"
    "  result = flat_colour * result.a;\n"
    "}\n";

#ifndef GL_BLEND_SRC_ALPHA
#define GL_BLEND_SRC_ALPHA              0x80cb
#define GL_BLEND_DST_ALPHA              0x80ca
#endif
#define GL_ONE                          1
#define GL_ONE_MINUS_SRC_ALPHA          0x0303

static GLfloat
clamp01(GLfloat value)
{
	return value < 0 ? 0 : value > 1 ? 1 : value;
}

/* The state of a texture of CoreAnimation's own (not one that stands for an IOSurface), or NULL. */
static struct group_texture *
group_of(struct eagl *e, GLuint texture)
{
	if (!group_vibrant_on || texture == 0 || texture >= GROUP_TEXTURES || image_of(e, texture) != NULL) {
		return NULL;
	}
	if (e->groups == NULL && (e->groups = calloc(GROUP_TEXTURES, sizeof(*e->groups))) == NULL) {
		return NULL;
	}
	return &e->groups[texture];
}

/* Our programs for it, made the first time. */
static bool
group_programs(struct eagl *e)
{
	GLint good = 0, good2 = 0, good3 = 0;

	if (e->group_cover != 0 || e->group_failed) {
		return !e->group_failed;
	}
	e->group_cover = plain_program(group_cover_fragment, "a vibrant layer's coverage, in a group");
	e->group_over = plain_program(group_over_fragment, "a group's vibrant layer over what is behind it");
	e->group_flat = plain_program(group_flat_fragment, "a group's vibrant layer as it would have been");
	glGetProgramiv(e->group_cover, GL_LINK_STATUS, &good);
	glGetProgramiv(e->group_over, GL_LINK_STATUS, &good2);
	glGetProgramiv(e->group_flat, GL_LINK_STATUS, &good3);
	e->group_failed = !good || !good2 || !good3;
	e->group_cover_matrix = glGetUniformLocation(e->group_cover, "vertex_matrix");
	e->group_cover_texmat = glGetUniformLocation(e->group_cover, "texmat");
	e->group_over_matrix = glGetUniformLocation(e->group_over, "vertex_matrix");
	e->group_over_texmat = glGetUniformLocation(e->group_over, "texmat");
	e->group_over_colours = glGetUniformLocation(e->group_over, "colours");
	e->group_flat_matrix = glGetUniformLocation(e->group_flat, "vertex_matrix");
	e->group_flat_colour = glGetUniformLocation(e->group_flat, "flat_colour");
	return !e->group_failed;
}

/*
 * A group that holds a vibrant layer's coverage made what CoreAnimation drew into it: the colour
 * matrix of nothing (its numbers added, opacity and all), by the coverage. Drawn through a
 * framebuffer of ours, the whole texture; everything of CoreAnimation's put back after.
 */
static void
group_flatten(struct eagl *e, GLuint texture, struct group_texture *g)
{
	static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	static const GLfloat corners[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLint view[4], box[4], width = 0, height = 0, enabled[3];
	GLboolean scissored, blending;
	GLfloat flat[4], alpha = clamp01(g->colours[19]);

	g->state = GROUP_NONE;
	group_flattened++;
	flat[3] = alpha;
	for (int i = 0; i < 3; i++) {
		flat[i] = clamp01(g->colours[16 + i] * alpha);
	}
	glGetIntegerv(GL_VIEWPORT, view);
	glGetIntegerv(GL_SCISSOR_BOX, box);
	scissored = glIsEnabled(GL_SCISSOR_TEST);
	blending = glIsEnabled(GL_BLEND);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	glBindTexture(GL_TEXTURE_2D, e->bound[0]);
	glActiveTexture(GL_TEXTURE0 + e->unit);
	if (e->reader == 0) {
		glGenFramebuffers(1, &e->reader);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->reader);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	if (flat[3] == 0) {
		GLfloat colour[4];

		/* (nothing by any coverage is nothing: the numbers add no opacity, as they do not in Control Center) */
		glGetFloatv(GL_COLOR_CLEAR_VALUE, colour);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glClearColor(colour[0], colour[1], colour[2], colour[3]);
	} else if (group_programs(e)) {
		glDisable(GL_BLEND);
		glUseProgram(e->group_flat);
		glUniformMatrix4fv(e->group_flat_matrix, 1, 0, identity);
		glUniform4fv(e->group_flat_colour, 1, flat);
		for (GLuint i = 0; i < 3; i++) {
			glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
		}
		glDisableVertexAttribArray(1);
		glDisableVertexAttribArray(2);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, corners);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glVertexAttribPointer(0, e->arrays[0].size, e->arrays[0].type, 0, e->arrays[0].stride, e->arrays[0].pointer);
		for (GLuint i = 0; i < 3; i++) {
			(enabled[i] ? glEnableVertexAttribArray : glDisableVertexAttribArray)(i);
		}
		glUseProgram(e->program);
		(blending ? glEnable : glDisable)(GL_BLEND);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
	glViewport(view[0], view[1], view[2], view[3]);
	glScissor(box[0], box[1], box[2], box[3]);
	(scissored ? glEnable : glDisable)(GL_SCISSOR_TEST);
}

/* A texture's contents are made anew (an upload, a copy into it): whatever it held is gone. */
static void
group_forget(struct eagl *e, GLuint texture)
{
	struct group_texture *g = e->groups != NULL && texture < GROUP_TEXTURES ? &e->groups[texture] : NULL;

	if (g != NULL) {
		g->state = GROUP_NONE;
	}
}

/* A texture is about to be read other than by a draw that lays a group over a picture: it is made what it would have been. */
static void
group_read(struct eagl *e, GLuint texture)
{
	struct group_texture *g = e->groups != NULL && texture != 0 && texture < GROUP_TEXTURES ? &e->groups[texture] : NULL;

	if (g != NULL && g->state == GROUP_VIBRANT) {
		group_flatten(e, texture, g);
	}
}

/* CoreAnimation's own uniform of its program in use, by name (a matrix of 16 numbers, or a vector of 4); false if it has none. */
static bool
group_uniform(struct eagl *e, const char *name, GLfloat *values)
{
	GLint location = glGetUniformLocation(e->program, name);

	if (location < 0) {
		return false;
	}
	glGetUniformfv(e->program, location, values);
	return true;
}

/*
 * Before a draw: is it one of the above, and if so, set up for it (the draw itself is CoreAnimation's,
 * with its arrays). Returns 0 for a draw left as it is, 1 for one of ours (group_draw_end() after it).
 */
static int
group_draw_begin(struct eagl *e, GLint enabled[4])
{
	static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	static uint8_t said[90];
	unsigned function = ca_context != NULL ? *(*(uint8_t **)((char *)ca_context + 0x10) + 0x10) : 90;
	struct group_texture *into = group_of(e, e->colour), *from = NULL;
	const GLfloat *colours = NULL;
	GLfloat matrix[16], texmat[4] = { 1, 1, 0, 0 };
	bool cover;

	if (!group_vibrant_on || e->colour == 0 || e->framebuffer == 0) {
		return 0;
	}
	if (e->program == 0 && function == FUNCTION_VIBRANT_MATRIX && fetch_offered && function_given[function] == 20 &&
	    e->bound[0] != 0 && e->bound[0] != e->colour && into != NULL && (into->state == GROUP_FRESH ||
	    (into->state == GROUP_VIBRANT && memcmp(into->colours, function_values[function], sizeof(into->colours)) == 0))) {
		/* a vibrant layer drawn into a group with nothing in it: its coverage */
		GLint width = 1, height = 1;

		glActiveTexture(GL_TEXTURE0);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glActiveTexture(GL_TEXTURE0 + e->unit);
		texmat[0] = 1.f / (GLfloat)(width > 0 ? width : 1);
		texmat[1] = 1.f / (GLfloat)(height > 0 ? height : 1);
		memcpy(matrix, e->matrix != NULL ? e->matrix : identity, sizeof(matrix));
		colours = function_values[function];
		cover = true;
	} else if (e->program != 0 && e->bound[0] != 0 && e->bound[0] != e->colour && (from = group_of(e, e->bound[0])) != NULL &&
	    from->state == GROUP_VIBRANT) {
		/* a group that holds a vibrant layer laid over something */
		bool laid = function == 1 || function == 3;    /* ("img", "cim": the picture as it is, by the colour) */

		if (function < 90 && !said[function]) {
			said[function] = 1;
			say("annibale-gl: a group that holds a vibrant layer is drawn with CoreAnimation's function %u (\"%s\"): %s\n",
			    function, census_name(function), laid ? "the layer over what is behind it" : "made what it would have been");
		}
		if (!laid || !group_uniform(e, "vertex_matrix", matrix)) {
			group_flatten(e, e->bound[0], from);
			from = NULL;
		} else {
			if (!group_uniform(e, "texmat0", texmat)) {
				texmat[0] = texmat[1] = 1;
			}
			colours = from->colours;
			cover = into != NULL && (into->state == GROUP_FRESH ||
			    (into->state == GROUP_VIBRANT && memcmp(into->colours, colours, sizeof(into->colours)) == 0));
		}
	}
	if (colours == NULL || !group_programs(e)) {
		/* not one of ours: what it reads is what CoreAnimation drew, and what it draws into is no longer a
		 * vibrant layer's coverage, nor empty */
		clear_now(e);
		for (unsigned unit = 0; unit < 4 && e->groups != NULL; unit++) {
			group_read(e, e->bound[unit]);
		}
		if (into != NULL) {
			if (into->state == GROUP_VIBRANT) {
				group_flatten(e, e->colour, into);
			}
			into->state = GROUP_NONE;
		}
		return 0;
	}
	clear_now(e);
	if (cover) {
		glUseProgram(e->group_cover);
		glUniformMatrix4fv(e->group_cover_matrix, 1, 0, matrix);
		glUniform4fv(e->group_cover_texmat, 1, texmat);
		e->group_blend = glIsEnabled(GL_BLEND);
		glGetIntegerv(GL_BLEND_SRC_RGB, &e->group_blend_func[0]);
		glGetIntegerv(GL_BLEND_DST_RGB, &e->group_blend_func[1]);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &e->group_blend_func[2]);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &e->group_blend_func[3]);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		if (into->state != GROUP_VIBRANT) {
			memcpy(into->colours, colours, sizeof(into->colours));
		}
		into->state = GROUP_VIBRANT;
		group_covered++;
	} else {
		glUseProgram(e->group_over);
		glUniformMatrix4fv(e->group_over_matrix, 1, 0, matrix);
		glUniform4fv(e->group_over_texmat, 1, texmat);
		glUniform4fv(e->group_over_colours, 5, colours);
		e->group_blend = glIsEnabled(GL_BLEND);
		glGetIntegerv(GL_BLEND_SRC_RGB, &e->group_blend_func[0]);
		glGetIntegerv(GL_BLEND_DST_RGB, &e->group_blend_func[1]);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &e->group_blend_func[2]);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &e->group_blend_func[3]);
		glDisable(GL_BLEND);            /* (the program blends) */
		if (into != NULL) {
			if (into->state == GROUP_VIBRANT) {
				group_flatten(e, e->colour, into);
			}
			into->state = GROUP_NONE;
		}
		group_laid++;
	}
	for (GLuint i = 0; i < 3; i++) {
		glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
		if (!enabled[i]) {
			glEnableVertexAttribArray(i);
		}
	}
	return 1;
}

static void
group_draw_end(struct eagl *e, const GLint enabled[4])
{
	for (GLuint i = 0; i < 3; i++) {
		if (!enabled[i]) {
			glDisableVertexAttribArray(i);
		}
	}
	(e->group_blend ? glEnable : glDisable)(GL_BLEND);
	glBlendFuncSeparate((GLenum)e->group_blend_func[0], (GLenum)e->group_blend_func[1], (GLenum)e->group_blend_func[2],
	    (GLenum)e->group_blend_func[3]);
	glUseProgram(e->program);
}

static bool
plain_draw_begin(struct eagl *e, GLint enabled[4])
{
	static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	static uint8_t said[90];
	unsigned function = ca_context != NULL ? *(*(uint8_t **)((char *)ca_context + 0x10) + 0x10) : 90;      /* (90: not known yet) */
	GLint rows = 0;
	GLuint program;

	if (e->program == 0) {
		e->line.vrd += function == FUNCTION_VARIABLE_DOWNSAMPLE;
		e->line.vrb += function == FUNCTION_VARIABLE_BLUR;
	}

	if (e->unclear != 0 && reads_what_it_writes(e)) {
		e->unclear = 0;
	}
	clear_now(e);
	if (e->program != 0) {
		return false;
	}
	if (vblur_on && reads_what_it_writes(e) && function_given[FUNCTION_VARIABLE_DOWNSAMPLE] != 0) {
		static unsigned told;

		vblur_level(e, (int)function_values[FUNCTION_VARIABLE_DOWNSAMPLE][0]);
		census_draw(e, function, CENSUS_LEVEL);
		if (told++ < 8) {
			say("annibale-gl: a blur whose radius varies: level %d of texture %u made of level %d\n",
			    (int)function_values[FUNCTION_VARIABLE_DOWNSAMPLE][0] + 1, e->bound[0],
			    (int)function_values[FUNCTION_VARIABLE_DOWNSAMPLE][0]);
		}
		return false;
	}
	/*
	 * Nor is any other draw with no program that would read the very texture it draws into. The
	 * window server's snapshots end with one: function 61 ("edr", by its numbers the display's
	 * headroom, infinite: nothing to change) over the whole picture, with that picture as its
	 * texture. Drawn as the picture it is given, it read what the GPU had not yet written out of
	 * its own memory -- nothing -- and wrote that over everything: every launch image SpringBoard
	 * keeps came out black, and with it the zoom that opens an application the first time.
	 */
	if (!plain_draws || (plain_draws == 1 && e->bound[0] == 0) || reads_what_it_writes(e) ||
	    (e->colour != 0 && e->colour == e->bound[0])) {
		census_draw(e, function, CENSUS_NOT_DRAWN);
		return false;
	}
	if (function < 90 && !said[function]) {
		said[function] = 1;
		say("annibale-gl: a draw with no program is CoreAnimation's function %u (\"%s\")\n", function, census_name(function));
	}
	if ((int)function == trace_function && --trace_function_after == 0) {
		trace_function_met = 1;
	}
	e->blended = 0;
	if (e->bound[0] != 0 && plain_draws != 3 && function < 90 && ((function == FUNCTION_VIBRANT_MATRIX && fetch_offered) ||
	    function == FUNCTION_MATRIX_PREMULTIPLIED) && function_given[function] == 20) {
		bool vibrant = function == FUNCTION_VIBRANT_MATRIX;

		if (vibrant && e->vibrant == 0) {
			GLint good = 0;

			e->vibrant = plain_program(vibrant_fragment, "a colour matrix over what is behind");
			glGetProgramiv(e->vibrant, GL_LINK_STATUS, &good);
			e->no_vibrant = !good;                  /* (a library without the extension: Mesa's software one) */
			e->vibrant_matrix = glGetUniformLocation(e->vibrant, "vertex_matrix");
			e->vibrant_colours = glGetUniformLocation(e->vibrant, "colours");
		} else if (!vibrant && e->masking == 0) {
			e->masking = plain_program(masking_fragment, "a colour matrix over premultiplied pixels");
			e->masking_matrix = glGetUniformLocation(e->masking, "vertex_matrix");
			e->masking_colours = glGetUniformLocation(e->masking, "colours");
		}
		if (vibrant && e->no_vibrant) {
			census_draw(e, function, CENSUS_NOT_DRAWN);     /* better nothing than the mask as a picture */
			return false;
		}
		census_draw(e, function, vibrant ? CENSUS_VIBRANT : CENSUS_MASKING);
		program = vibrant ? e->vibrant : e->masking;
		glUseProgram(program);
		glUniformMatrix4fv(vibrant ? e->vibrant_matrix : e->masking_matrix, 1, 0, e->matrix != NULL ? e->matrix : identity);
		glUniform4fv(vibrant ? e->vibrant_colours : e->masking_colours, 5, function_values[function]);
		if (vibrant && glIsEnabled(GL_BLEND)) {
			e->blended = 1;                 /* the program has blended already */
			glDisable(GL_BLEND);
		}
		glGetVertexAttribiv(3, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[3]);
		for (GLuint i = 0; i < 3; i++) {
			glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
			if (!enabled[i]) {
				glEnableVertexAttribArray(i);
			}
		}
		return true;
	}
	if (vblur_on && e->bound[0] != 0 && e->bound[1] != 0 && function == FUNCTION_VARIABLE_BLUR && function_given[function] >= 2) {
		struct vblur_chain *chain = vblur_chain_of(e, e->bound[0], false);

		if (chain != NULL && chain->made > 0 && vblur_draw_begin(e, chain, enabled)) {
			census_draw(e, function, CENSUS_VBLUR);
			return true;
		}
	}
	if (e->bound[0] != 0 && plain_draws != 3 && function == FUNCTION_SHADOW_BLUR && function_given[function] == 8) {
		if (e->blur4 == 0) {
			e->blur4 = plain_program(blur4_fragment, "four samples around a point");
			e->blur4_matrix = glGetUniformLocation(e->blur4, "vertex_matrix");
			e->blur4_places = glGetUniformLocation(e->blur4, "places");
		}
		census_draw(e, function, CENSUS_BLUR4);
		glUseProgram(e->blur4);
		glUniformMatrix4fv(e->blur4_matrix, 1, 0, e->matrix != NULL ? e->matrix : identity);
		glUniform2fv(e->blur4_places, 4, function_values[function]);
		glGetVertexAttribiv(3, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[3]);
		for (GLuint i = 0; i < 3; i++) {
			glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
			if (!enabled[i]) {
				glEnableVertexAttribArray(i);
			}
		}
		return true;
	}
	if (e->bound[0] != 0 && plain_draws != 3 && function == FUNCTION_DOWNSAMPLE_8 && function_given[function] == 8) {
		if (e->box == 0) {
			e->box = plain_program(box_fragment, "eight times smaller, sixteen samples");
			e->box_matrix = glGetUniformLocation(e->box, "vertex_matrix");
			e->box_places = glGetUniformLocation(e->box, "places");
		}
		census_draw(e, function, CENSUS_BOX);
		glUseProgram(e->box);
		glUniformMatrix4fv(e->box_matrix, 1, 0, e->matrix != NULL ? e->matrix : identity);
		glUniform2fv(e->box_places, 4, function_values[function]);
		glGetVertexAttribiv(3, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[3]);
		for (GLuint i = 0; i < 3; i++) {
			glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
			if (!enabled[i]) {
				glEnableVertexAttribArray(i);
			}
		}
		return true;
	}
	if (e->bound[0] != 0 && e->bound[1] != 0) {
		glActiveTexture(GL_TEXTURE0 + 1);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &rows);
		glActiveTexture(GL_TEXTURE0 + e->unit);
	}
	if (rows == 1) {
		if (e->lookup == 0) {
			e->lookup = plain_program(table_fragment, "through a table");
			e->lookup_matrix = glGetUniformLocation(e->lookup, "vertex_matrix");
			glUseProgram(e->lookup);
			glUniform1i(glGetUniformLocation(e->lookup, "table"), 1);
		}
		program = e->lookup;
	} else {
		if (e->plain == 0) {
			e->plain = plain_program(plain_fragment, "as they are");
			e->plain_matrix = glGetUniformLocation(e->plain, "vertex_matrix");
			e->plain_textured = glGetUniformLocation(e->plain, "textured");
		}
		program = e->plain;
	}
	census_draw(e, function, rows == 1 ? CENSUS_TABLE : CENSUS_PLAIN);
	glUseProgram(program);
	glUniformMatrix4fv(program == e->plain ? e->plain_matrix : e->lookup_matrix, 1, 0, e->matrix != NULL ? e->matrix : identity);
	if (program == e->plain) {
		glUniform1f(e->plain_textured, e->bound[0] != 0 ? 1 : 0);
	}
	glGetVertexAttribiv(3, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[3]);
	for (GLuint i = 0; i < 3; i++) {
		glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
		if (!enabled[i]) {
			glEnableVertexAttribArray(i);
		}
	}
	if (eagl_tracing || e->plain_said < 4) {
		const GLfloat *position = e->arrays[0].pointer, *where = e->arrays[2].pointer;

		e->plain_said++;
		say("annibale-gl: a draw with no program, into framebuffer %u: texture %u on the first unit%s; arrays %d %d %d; first "
		    "corner %g %g, texture coordinates %g %g\n", e->framebuffer, e->bound[0], rows == 1 ? ", a table on the second" : "",
		    enabled[0], enabled[1], enabled[2], position ? (double)position[0] : 0, position ? (double)position[1] : 0,
		    where ? (double)where[0] : 0, where ? (double)where[1] : 0);
	}
	return true;
}

static void
plain_draw_end(struct eagl *e, const GLint enabled[4])
{
	for (GLuint i = 0; i < 4; i++) {
		if (!enabled[i]) {
			glDisableVertexAttribArray(i);
		}
	}
	if (e->vblur_bound) {
		/* the textures of units 2 to 5 back as CoreAnimation had them */
		for (unsigned k = 2; k < 6; k++) {
			glActiveTexture(GL_TEXTURE0 + k);
			glBindTexture(GL_TEXTURE_2D, e->bound[k]);
		}
		glActiveTexture(GL_TEXTURE0 + e->unit);
		e->vblur_bound = false;
	}
	glUseProgram(0);
	if (e->blended) {
		glEnable(GL_BLEND);
		e->blended = 0;
	}
}
