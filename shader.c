/*
 * Cage: A Wayland kiosk.
 *
 * A GLSL ES post-processing pass over the virtual output, at its own
 * resolution, ahead of the upscale.
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/config.h>
#include <wlr/util/log.h>

#include "shader.h"

/* Refuse anything this large: it is not a fragment shader. */
#define SHADER_MAX_SOURCE (1024 * 1024)

bool
shader_parse_file(struct cg_shader *shader, const char *path)
{
	FILE *file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "Unable to open shader '%s': %s\n", path, strerror(errno));
		return false;
	}

	char *source = malloc(SHADER_MAX_SOURCE + 1);
	if (!source) {
		fclose(file);
		return false;
	}

	size_t len = fread(source, 1, SHADER_MAX_SOURCE + 1, file);
	bool read_error = ferror(file);
	fclose(file);

	if (read_error) {
		fprintf(stderr, "Unable to read shader '%s'\n", path);
		free(source);
		return false;
	}
	if (len > SHADER_MAX_SOURCE) {
		fprintf(stderr, "Shader '%s' is larger than %d bytes\n", path, SHADER_MAX_SOURCE);
		free(source);
		return false;
	}
	if (len == 0) {
		fprintf(stderr, "Shader '%s' is empty\n", path);
		free(source);
		return false;
	}
	source[len] = '\0';

	free(shader->source);
	free(shader->path);
	shader->source = source;
	shader->path = strdup(path);
	shader->enabled = true;
	return true;
}

static bool
valid_identifier(const char *name, size_t len)
{
	if (len == 0 || !(isalpha((unsigned char) name[0]) || name[0] == '_')) {
		return false;
	}

	for (size_t i = 1; i < len; i++) {
		if (!(isalnum((unsigned char) name[i]) || name[i] == '_')) {
			return false;
		}
	}

	return true;
}

bool
shader_parse_uniform(struct cg_shader *shader, const char *arg)
{
	const char *eq = strchr(arg, '=');
	if (!eq || !valid_identifier(arg, (size_t) (eq - arg))) {
		fprintf(stderr, "Invalid uniform '%s', expected NAME=VALUE[,VALUE...]\n", arg);
		return false;
	}

	struct cg_shader_uniform uniform = {0};
	const char *cursor = eq + 1;

	for (;;) {
		if (uniform.count == 4) {
			fprintf(stderr, "Uniform '%s' has more than four components\n", arg);
			return false;
		}

		char *end = NULL;
		errno = 0;
		float value = strtof(cursor, &end);
		if (end == cursor || errno != 0 || !isfinite(value)) {
			fprintf(stderr, "Invalid value in uniform '%s'\n", arg);
			return false;
		}
		uniform.value[uniform.count++] = value;

		if (*end == '\0') {
			break;
		}
		if (*end != ',') {
			fprintf(stderr, "Invalid value in uniform '%s'\n", arg);
			return false;
		}
		cursor = end + 1;
	}

	uniform.name = strndup(arg, (size_t) (eq - arg));
	if (!uniform.name) {
		return false;
	}

	/* A later -U for the same name wins. */
	for (int i = 0; i < shader->uniforms_len; i++) {
		if (strcmp(shader->uniforms[i].name, uniform.name) == 0) {
			free(shader->uniforms[i].name);
			shader->uniforms[i] = uniform;
			return true;
		}
	}

	if (shader->uniforms_len == CG_SHADER_MAX_UNIFORMS) {
		fprintf(stderr, "Too many uniforms, at most %d can be set\n", CG_SHADER_MAX_UNIFORMS);
		free(uniform.name);
		return false;
	}

	shader->uniforms[shader->uniforms_len++] = uniform;
	return true;
}

static void
shader_free_options(struct cg_shader *shader)
{
	for (int i = 0; i < shader->uniforms_len; i++) {
		free(shader->uniforms[i].name);
		shader->uniforms[i].name = NULL;
	}
	shader->uniforms_len = 0;

	free(shader->source);
	free(shader->path);
	shader->source = NULL;
	shader->path = NULL;
}

#if WLR_HAS_GLES2_RENDERER

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>

/* Texture units, fixed for the life of the program. */
#define UNIT_SOURCE 0
#define UNIT_NOISE 1
#define UNIT_SCANLINE 2

#define NOISE_SIZE 64
#define ATTRIB_POSITION 0

/* Radians per second. */
#define WOBBLE_SPEED 4.0
#define TWO_PI 6.28318530717958647692

/* Keep the clock small enough for mediump arithmetic to stay useful. */
#define TIME_WRAP 3600.0

/*
 * Provides the interface the fragment shader is written against: `uvs` runs
 * from 0,0 at the top left of the virtual output to 1,1 at the bottom right,
 * and `scanlinecoord` repeats the scanline texture `scanlinerepeat` times down
 * the frame. The quad covers the whole target, so gl_FragCoord is one virtual
 * pixel per fragment.
 */
static const char vertex_source[] = "#version 100\n"
				    "attribute vec2 cg_position;\n"
				    "uniform float scanlinerepeat;\n"
				    "varying highp vec2 uvs;\n"
				    "varying highp vec2 scanlinecoord;\n"
				    "void main()\n"
				    "{\n"
				    "	uvs = cg_position;\n"
				    "	scanlinecoord = vec2(cg_position.x, cg_position.y * scanlinerepeat);\n"
				    "	gl_Position = vec4(cg_position * 2.0 - 1.0, 0.0, 1.0);\n"
				    "}\n";

struct cg_shader_gl {
	EGLDisplay display;
	EGLContext context;

	GLuint program;
	GLuint noise_tex;
	GLuint scanline_tex;

	GLint tex;
	GLint noisetex;
	GLint scanlinetex;
	GLint texturewidth;
	GLint time;
	GLint wobblephase;
	GLint noisescale;
	GLint noiseoffset;
	GLint scanlinerepeat;

	/* Whether each driven uniform was fixed with -U instead. */
	bool fixed_texturewidth;
	bool fixed_time;
	bool fixed_wobblephase;
	bool fixed_noisescale;
	bool fixed_noiseoffset;
	bool fixed_scanlinerepeat;

	bool reported;
	bool reported_busy;
};

struct egl_saved {
	EGLDisplay display;
	EGLContext context;
	EGLSurface draw, read;
};

static bool
egl_enter(struct cg_shader_gl *gl, struct egl_saved *saved)
{
	saved->display = eglGetCurrentDisplay();
	saved->context = eglGetCurrentContext();
	saved->draw = eglGetCurrentSurface(EGL_DRAW);
	saved->read = eglGetCurrentSurface(EGL_READ);

	if (!eglMakeCurrent(gl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, gl->context)) {
		wlr_log(WLR_ERROR, "shader: unable to make the renderer's EGL context current");
		return false;
	}
	return true;
}

static void
egl_leave(struct cg_shader_gl *gl, const struct egl_saved *saved)
{
	if (saved->display == EGL_NO_DISPLAY) {
		eglMakeCurrent(gl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	} else {
		eglMakeCurrent(saved->display, saved->draw, saved->read, saved->context);
	}
}

static GLuint
compile_stage(GLenum type, const char *source, const char *what)
{
	GLuint stage = glCreateShader(type);
	if (!stage) {
		return 0;
	}

	glShaderSource(stage, 1, &source, NULL);
	glCompileShader(stage);

	GLint ok = GL_FALSE;
	glGetShaderiv(stage, GL_COMPILE_STATUS, &ok);
	if (ok != GL_TRUE) {
		char log[4096] = {0};
		glGetShaderInfoLog(stage, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "shader: failed to compile %s:\n%s", what, log);
		glDeleteShader(stage);
		return 0;
	}

	return stage;
}

static GLuint
link_program(const char *fragment_source, const char *path)
{
	GLuint vertex = compile_stage(GL_VERTEX_SHADER, vertex_source, "the vertex stage");
	if (!vertex) {
		return 0;
	}

	GLuint fragment = compile_stage(GL_FRAGMENT_SHADER, fragment_source, path);
	if (!fragment) {
		glDeleteShader(vertex);
		return 0;
	}

	GLuint program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glBindAttribLocation(program, ATTRIB_POSITION, "cg_position");
	glLinkProgram(program);

	glDetachShader(program, vertex);
	glDetachShader(program, fragment);
	glDeleteShader(vertex);
	glDeleteShader(fragment);

	GLint ok = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (ok != GL_TRUE) {
		char log[4096] = {0};
		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "shader: failed to link %s:\n%s", path, log);
		glDeleteProgram(program);
		return 0;
	}

	return program;
}

static GLuint
create_texture(GLsizei width, GLsizei height, const uint8_t *pixels)
{
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_2D, 0);
	return tex;
}

static bool
is_fixed(const struct cg_shader *shader, const char *name)
{
	for (int i = 0; i < shader->uniforms_len; i++) {
		if (strcmp(shader->uniforms[i].name, name) == 0) {
			return true;
		}
	}
	return false;
}

/* Sets the -U values, once: they live in the program object from then on. */
static bool
set_fixed_uniforms(struct cg_shader *shader)
{
	struct cg_shader_gl *gl = shader->gl;

	for (int i = 0; i < shader->uniforms_len; i++) {
		const struct cg_shader_uniform *uniform = &shader->uniforms[i];
		GLint loc = glGetUniformLocation(gl->program, uniform->name);
		if (loc < 0) {
			wlr_log(WLR_INFO, "shader: uniform '%s' is not used by %s, ignoring it", uniform->name,
				shader->path);
			continue;
		}

		while (glGetError() != GL_NO_ERROR) {
		}

		switch (uniform->count) {
		case 1:
			glUniform1fv(loc, 1, uniform->value);
			break;
		case 2:
			glUniform2fv(loc, 1, uniform->value);
			break;
		case 3:
			glUniform3fv(loc, 1, uniform->value);
			break;
		default:
			glUniform4fv(loc, 1, uniform->value);
			break;
		}

		if (glGetError() != GL_NO_ERROR) {
			wlr_log(WLR_ERROR, "shader: uniform '%s' is not a float or vector of %d components",
				uniform->name, uniform->count);
			return false;
		}
	}

	return true;
}

bool
shader_init(struct cg_shader *shader, struct wlr_renderer *renderer, struct wlr_allocator *allocator)
{
	if (!shader->enabled) {
		return true;
	}

	if (!wlr_renderer_is_gles2(renderer)) {
		wlr_log(WLR_ERROR, "-S needs the GLES2 renderer; unset WLR_RENDERER or set it to gles2");
		return false;
	}

	struct cg_shader_gl *gl = calloc(1, sizeof(*gl));
	if (!gl) {
		return false;
	}

	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
	gl->display = wlr_egl_get_display(egl);
	gl->context = wlr_egl_get_context(egl);

	shader->gl = gl;
	shader->renderer = renderer;
	shader->allocator = allocator;

	struct egl_saved saved;
	if (!egl_enter(gl, &saved)) {
		return false;
	}

	bool ok = false;

	gl->program = link_program(shader->source, shader->path);
	if (!gl->program) {
		goto out;
	}

	gl->tex = glGetUniformLocation(gl->program, "tex");
	gl->noisetex = glGetUniformLocation(gl->program, "noisetex");
	gl->scanlinetex = glGetUniformLocation(gl->program, "scanlinetex");
	gl->texturewidth = glGetUniformLocation(gl->program, "texturewidth");
	gl->time = glGetUniformLocation(gl->program, "time");
	gl->wobblephase = glGetUniformLocation(gl->program, "wobblephase");
	gl->noisescale = glGetUniformLocation(gl->program, "noisescale");
	gl->noiseoffset = glGetUniformLocation(gl->program, "noiseoffset");
	gl->scanlinerepeat = glGetUniformLocation(gl->program, "scanlinerepeat");

	gl->fixed_texturewidth = is_fixed(shader, "texturewidth");
	gl->fixed_time = is_fixed(shader, "time");
	gl->fixed_wobblephase = is_fixed(shader, "wobblephase");
	gl->fixed_noisescale = is_fixed(shader, "noisescale");
	gl->fixed_noiseoffset = is_fixed(shader, "noiseoffset");
	gl->fixed_scanlinerepeat = is_fixed(shader, "scanlinerepeat");

	if (gl->tex < 0) {
		wlr_log(WLR_ERROR, "shader: %s never samples 'tex', the frame it is meant to process", shader->path);
		goto out;
	}

	/* A fresh random pattern for the shaders that cannot afford their own. */
	srand((unsigned) time(NULL));
	uint8_t noise[NOISE_SIZE * NOISE_SIZE];
	for (size_t i = 0; i < sizeof(noise); i++) {
		noise[i] = (uint8_t) (rand() & 0xff);
	}
	gl->noise_tex = create_texture(NOISE_SIZE, NOISE_SIZE, noise);

	/* Two rows per repeat, the second one dark: with the default repeat of
	 * half the frame height, every other virtual row is a scanline. */
	static const uint8_t scanline[2] = {0x00, 0xff};
	gl->scanline_tex = create_texture(1, 2, scanline);

	glUseProgram(gl->program);
	glUniform1i(gl->tex, UNIT_SOURCE);
	if (gl->noisetex >= 0) {
		glUniform1i(gl->noisetex, UNIT_NOISE);
	}
	if (gl->scanlinetex >= 0) {
		glUniform1i(gl->scanlinetex, UNIT_SCANLINE);
	}
	ok = set_fixed_uniforms(shader);
	glUseProgram(0);

	if (!ok) {
		goto out;
	}

	shader->animated = (gl->time >= 0 && !gl->fixed_time) || (gl->wobblephase >= 0 && !gl->fixed_wobblephase) ||
			   (gl->noiseoffset >= 0 && !gl->fixed_noiseoffset);
	clock_gettime(CLOCK_MONOTONIC, &shader->start);

	wlr_log(WLR_INFO, "shader: loaded %s%s", shader->path, shader->animated ? ", animated" : "");

out:
	egl_leave(gl, &saved);
	return ok;
}

/*
 * The shaded frame has to be exactly as acceptable to the display controller
 * as the unshaded one, or the scaling plane refuses it and -r stops Cage. So
 * rather than let the allocator pick, allocate the very format and modifier
 * the virtual output's own buffer ended up with.
 */
static bool
ensure_swapchain(struct cg_shader *shader, struct wlr_buffer *source)
{
	struct wlr_dmabuf_attributes dmabuf;
	if (!wlr_buffer_get_dmabuf(source, &dmabuf)) {
		wlr_log(WLR_ERROR, "shader: the virtual output's frame is not a DMA-BUF");
		return false;
	}

	struct wlr_swapchain *swapchain = shader->swapchain;
	if (swapchain && swapchain->width == source->width && swapchain->height == source->height &&
	    swapchain->format.format == dmabuf.format && swapchain->format.len == 1 &&
	    swapchain->format.modifiers[0] == dmabuf.modifier) {
		return true;
	}

	uint64_t modifier = dmabuf.modifier;
	struct wlr_drm_format format = {
		.format = dmabuf.format,
		.len = 1,
		.capacity = 1,
		.modifiers = &modifier,
	};

	struct wlr_swapchain *created = wlr_swapchain_create(shader->allocator, source->width, source->height, &format);
	if (!created) {
		wlr_log(WLR_ERROR, "shader: unable to create %dx%d buffers (format 0x%08x, modifier 0x%016llx)",
			source->width, source->height, (unsigned) dmabuf.format, (unsigned long long) dmabuf.modifier);
		return false;
	}

	wlr_swapchain_destroy(shader->swapchain);
	shader->swapchain = created;

	wlr_log(WLR_INFO, "shader: shading at %dx%d (format 0x%08x, modifier 0x%016llx)", source->width, source->height,
		(unsigned) dmabuf.format, (unsigned long long) dmabuf.modifier);
	return true;
}

static double
elapsed_seconds(const struct cg_shader *shader)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double) (now.tv_sec - shader->start.tv_sec) + (double) (now.tv_nsec - shader->start.tv_nsec) / 1e9;
}

static void
set_driven_uniforms(struct cg_shader *shader, int width, int height)
{
	struct cg_shader_gl *gl = shader->gl;
	double elapsed = elapsed_seconds(shader);

	if (gl->texturewidth >= 0 && !gl->fixed_texturewidth) {
		glUniform1f(gl->texturewidth, (GLfloat) width);
	}
	if (gl->time >= 0 && !gl->fixed_time) {
		glUniform1f(gl->time, (GLfloat) fmod(elapsed, TIME_WRAP));
	}
	if (gl->wobblephase >= 0 && !gl->fixed_wobblephase) {
		glUniform1f(gl->wobblephase, (GLfloat) fmod(elapsed * WOBBLE_SPEED, TWO_PI));
	}
	if (gl->noisescale >= 0 && !gl->fixed_noisescale) {
		/* One noise texel per virtual pixel. */
		glUniform2f(gl->noisescale, (GLfloat) width / NOISE_SIZE, (GLfloat) height / NOISE_SIZE);
	}
	if (gl->noiseoffset >= 0 && !gl->fixed_noiseoffset) {
		glUniform2f(gl->noiseoffset, (GLfloat) rand() / (GLfloat) RAND_MAX,
			    (GLfloat) rand() / (GLfloat) RAND_MAX);
	}
	if (gl->scanlinerepeat >= 0 && !gl->fixed_scanlinerepeat) {
		glUniform1f(gl->scanlinerepeat, (GLfloat) height / 2.0f);
	}
}

struct wlr_buffer *
shader_apply(struct cg_shader *shader, struct wlr_buffer *source)
{
	struct cg_shader_gl *gl = shader->gl;

	if (!gl || !gl->program || !source) {
		return NULL;
	}

	if (!ensure_swapchain(shader, source)) {
		return NULL;
	}

	struct wlr_buffer *target = wlr_swapchain_acquire(shader->swapchain);
	if (!target) {
		if (!gl->reported_busy) {
			wlr_log(WLR_ERROR, "shader: every output buffer is still in use, skipping a frame");
			gl->reported_busy = true;
		}
		return NULL;
	}

	struct wlr_texture *texture = wlr_texture_from_buffer(shader->renderer, source);
	if (!texture) {
		wlr_log(WLR_ERROR, "shader: unable to sample the virtual output's frame");
		wlr_buffer_unlock(target);
		return NULL;
	}

	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(texture, &attribs);
	if (attribs.target != GL_TEXTURE_2D) {
		/* sampler2D cannot read it, and Mali would ignore our filter. */
		wlr_log(WLR_ERROR, "shader: the virtual output's frame can only be sampled as an external image");
		wlr_texture_destroy(texture);
		wlr_buffer_unlock(target);
		return NULL;
	}

	/* Makes the renderer's context current, binds the target's FBO and sets
	 * the viewport to the whole of it. Submitting flushes exactly as the
	 * renderer would, so the frame reaches the display the same way. */
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(shader->renderer, target, NULL);
	if (!pass) {
		wlr_log(WLR_ERROR, "shader: unable to render into the output buffer");
		wlr_texture_destroy(texture);
		wlr_buffer_unlock(target);
		return NULL;
	}

	static const GLfloat quad[] = {
		0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
	};

	glDisable(GL_BLEND);
	glUseProgram(gl->program);

	glActiveTexture(GL_TEXTURE0 + UNIT_SCANLINE);
	glBindTexture(GL_TEXTURE_2D, gl->scanline_tex);
	glActiveTexture(GL_TEXTURE0 + UNIT_NOISE);
	glBindTexture(GL_TEXTURE_2D, gl->noise_tex);
	glActiveTexture(GL_TEXTURE0 + UNIT_SOURCE);
	glBindTexture(GL_TEXTURE_2D, attribs.tex);
	/* Interpolated within the virtual output, where the shader expects it;
	 * this has nothing to do with the upscale's filter. */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	set_driven_uniforms(shader, source->width, source->height);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glVertexAttribPointer(ATTRIB_POSITION, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(ATTRIB_POSITION);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(ATTRIB_POSITION);

	/* Leave the renderer the state it assumes. */
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0 + UNIT_NOISE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0 + UNIT_SCANLINE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glUseProgram(0);

	bool ok = wlr_render_pass_submit(pass);
	wlr_texture_destroy(texture);

	if (!ok) {
		wlr_log(WLR_ERROR, "shader: unable to submit the shaded frame");
		wlr_buffer_unlock(target);
		return NULL;
	}

	if (!gl->reported) {
		wlr_log(WLR_DEBUG, "shader: first %dx%d frame shaded", target->width, target->height);
		gl->reported = true;
	}
	gl->reported_busy = false;

	return target;
}

void
shader_finish(struct cg_shader *shader)
{
	wlr_swapchain_destroy(shader->swapchain);
	shader->swapchain = NULL;

	struct cg_shader_gl *gl = shader->gl;
	if (gl) {
		struct egl_saved saved;
		if (egl_enter(gl, &saved)) {
			if (gl->program) {
				glDeleteProgram(gl->program);
			}
			if (gl->noise_tex) {
				glDeleteTextures(1, &gl->noise_tex);
			}
			if (gl->scanline_tex) {
				glDeleteTextures(1, &gl->scanline_tex);
			}
			egl_leave(gl, &saved);
		}
		free(gl);
		shader->gl = NULL;
	}

	shader_free_options(shader);
	shader->enabled = false;
}

#else /* !WLR_HAS_GLES2_RENDERER */

bool
shader_init(struct cg_shader *shader, struct wlr_renderer *renderer, struct wlr_allocator *allocator)
{
	if (!shader->enabled) {
		return true;
	}

	wlr_log(WLR_ERROR, "-S needs wlroots built with the GLES2 renderer");
	return false;
}

struct wlr_buffer *
shader_apply(struct cg_shader *shader, struct wlr_buffer *source)
{
	return NULL;
}

void
shader_finish(struct cg_shader *shader)
{
	shader_free_options(shader);
	shader->enabled = false;
}

#endif
