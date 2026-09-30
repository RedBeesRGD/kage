/*
 * Cage: A Wayland kiosk.
 *
 * Compositor-wide post-processing with a user-supplied GLSL ES shader.
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

#include "output.h"
#include "server.h"
#include "shader.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SHADER_MAX_SIZE (1024 * 1024)
#define NOISE_SIZE 64
#define SCANLINE_SAMPLES 8
/* Short enough that consecutive frames still differ at mediump. */
#define TIME_WRAP 8.0
#define WOBBLE_SPEED M_PI

static const char vertex_source[] = "#version 100\n"
				    "attribute vec2 kage_position;\n"
				    "attribute vec2 kage_texcoord;\n"
				    "uniform vec2 kage_sourcesize;\n"
				    "varying vec2 uvs;\n"
				    "varying vec2 scanlinecoord;\n"
				    "void main()\n"
				    "{\n"
				    "	uvs = kage_texcoord;\n"
				    "	scanlinecoord = vec2(kage_texcoord.x, kage_texcoord.y * kage_sourcesize.y);\n"
				    "	gl_Position = vec4(kage_position, 0.0, 1.0);\n"
				    "}\n";

static const char copy_vertex_source[] = "#version 100\n"
					 "attribute vec2 kage_position;\n"
					 "attribute vec2 kage_texcoord;\n"
					 "varying vec2 texcoord;\n"
					 "void main()\n"
					 "{\n"
					 "	texcoord = kage_texcoord;\n"
					 "	gl_Position = vec4(kage_position, 0.0, 1.0);\n"
					 "}\n";

/* A 1:1 copy, sampled at texel centres, so it wants the precision. */
#define COPY_FRAGMENT_SOURCE(extension, sampler)                                                                       \
	"#version 100\n" extension "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"                                               \
	"precision highp float;\n"                                                                                     \
	"#else\n"                                                                                                      \
	"precision mediump float;\n"                                                                                   \
	"#endif\n"                                                                                                     \
	"uniform " sampler " tex;\n"                                                                                   \
	"varying vec2 texcoord;\n"                                                                                     \
	"void main()\n"                                                                                                \
	"{\n"                                                                                                          \
	"	gl_FragColor = vec4(texture2D(tex, texcoord).rgb, 1.0);\n"                                                   \
	"}\n"

enum copy_kind {
	COPY_2D,
	COPY_EXTERNAL,
	COPY_KINDS,
};

static const char *const copy_fragment_sources[COPY_KINDS] = {
	[COPY_2D] = COPY_FRAGMENT_SOURCE("", "sampler2D"),
	[COPY_EXTERNAL] =
		COPY_FRAGMENT_SOURCE("#extension GL_OES_EGL_image_external : require\n", "samplerExternalOES"),
};

struct shader_program {
	GLuint program;
	GLint position;
	GLint texcoord;
};

struct cg_shader_gl {
	struct wlr_renderer *renderer;
	struct wlr_egl *egl;

	struct shader_program main;
	struct {
		GLint sourcesize;
		GLint texturewidth;
		GLint time;
		GLint wobblephase;
		GLint noisescale;
		GLint noiseoffset;
	} uniforms;

	struct shader_program copy[COPY_KINDS];
	GLint copy_tex[COPY_KINDS];

	GLuint noise_texture;
	GLuint scanline_texture;

	struct timespec start;
	uint32_t random;
};

struct cg_shader_output {
	/* Without -r, the scene is composited here for the shader to read. */
	struct wlr_swapchain *swapchain;
	/* The most recent composite, locked. */
	struct wlr_buffer *source;

	/* An upright GL_TEXTURE_2D copy of the source, for when the source
	 * itself is not one. */
	GLuint copy_texture;
	GLuint copy_fbo;
	int copy_width, copy_height;
};

/* What is drawn, and where. */
struct frame {
	struct wlr_buffer *source;
	/* How the source is laid out relative to the upright picture: normal
	 * for the virtual output, the output's own transform for a scene
	 * composited for it. */
	enum wl_output_transform source_transform;
	/* Where the picture goes, in output-local logical coordinates. */
	struct wlr_box dest;
};

struct egl_saved {
	EGLDisplay display;
	EGLContext context;
	EGLSurface draw, read;
};

bool
shader_load(struct cg_shader *shader, const char *path)
{
	FILE *file = fopen(path, "rb");
	if (!file) {
		wlr_log(WLR_ERROR, "Unable to open shader %s: %s", path, strerror(errno));
		return false;
	}

	char *source = malloc(SHADER_MAX_SIZE + 1);
	if (!source) {
		fclose(file);
		return false;
	}

	size_t len = fread(source, 1, SHADER_MAX_SIZE + 1, file);
	bool failed = ferror(file);
	fclose(file);

	if (failed) {
		wlr_log(WLR_ERROR, "Unable to read shader %s", path);
		free(source);
		return false;
	}
	if (len > SHADER_MAX_SIZE) {
		wlr_log(WLR_ERROR, "Shader %s is larger than %d bytes", path, SHADER_MAX_SIZE);
		free(source);
		return false;
	}
	source[len] = '\0';

	free(shader->path);
	free(shader->source);
	shader->path = strdup(path);
	shader->source = source;
	shader->enabled = shader->path != NULL;
	return shader->enabled;
}

/*
 * Temporarily set an environment variable that wlroots only reads while
 * creating something, so the override does not leak into the client.
 */
struct env_override {
	const char *name;
	char *value;
	bool was_set;
};

static void
env_override(struct env_override *saved, const char *name, const char *value)
{
	const char *prev = getenv(name);

	saved->name = name;
	saved->value = prev ? strdup(prev) : NULL;
	saved->was_set = saved->value != NULL;

	setenv(name, value, true);
}

static void
env_restore(struct env_override *saved)
{
	if (saved->was_set) {
		setenv(saved->name, saved->value, true);
	} else {
		unsetenv(saved->name);
	}
	free(saved->value);
}

/* The shader is GLSL, so the renderer has to be GLES2 whatever the default. */
struct wlr_renderer *
shader_create_renderer(struct wlr_backend *backend)
{
	const char *requested = getenv("WLR_RENDERER");
	if (requested != NULL && strcmp(requested, "gles2") != 0) {
		wlr_log(WLR_ERROR, "WLR_RENDERER=%s cannot run a GLSL shader; unset it or use gles2", requested);
		return NULL;
	}

	struct env_override saved;
	env_override(&saved, "WLR_RENDERER", "gles2");
	struct wlr_renderer *renderer = wlr_renderer_autocreate(backend);
	env_restore(&saved);

	return renderer;
}

/*
 * Direct scan-out would hand a client's buffer straight to the display and
 * skip the shader, so the scene must always composite. It reads the variable
 * once, when it is created.
 */
struct wlr_scene *
shader_create_scene(void)
{
	struct env_override saved;
	env_override(&saved, "WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1");
	struct wlr_scene *scene = wlr_scene_create();
	env_restore(&saved);

	return scene;
}

static bool
gl_begin(struct cg_shader_gl *gl, struct egl_saved *saved)
{
	saved->display = eglGetCurrentDisplay();
	saved->context = eglGetCurrentContext();
	saved->draw = eglGetCurrentSurface(EGL_DRAW);
	saved->read = eglGetCurrentSurface(EGL_READ);

	if (!eglMakeCurrent(wlr_egl_get_display(gl->egl), EGL_NO_SURFACE, EGL_NO_SURFACE,
			    wlr_egl_get_context(gl->egl))) {
		wlr_log(WLR_ERROR, "shader: unable to make the EGL context current");
		return false;
	}

	return true;
}

static void
gl_end(struct cg_shader_gl *gl, const struct egl_saved *saved)
{
	if (saved->display == EGL_NO_DISPLAY) {
		eglMakeCurrent(wlr_egl_get_display(gl->egl), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	} else {
		eglMakeCurrent(saved->display, saved->draw, saved->read, saved->context);
	}
}

static GLuint
compile(GLenum type, const char *source, const char *name)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok != GL_TRUE) {
		char log[4096];
		GLsizei len = 0;
		glGetShaderInfoLog(shader, sizeof(log), &len, log);
		wlr_log(WLR_ERROR, "Failed to compile %s:\n%.*s", name, (int) len, log);
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

static bool
link_program(struct shader_program *out, const char *vertex_source, const char *fragment_source, const char *name)
{
	GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source, "the built-in vertex shader");
	if (!vertex) {
		return false;
	}

	GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source, name);
	if (!fragment) {
		glDeleteShader(vertex);
		return false;
	}

	GLuint program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);
	glDetachShader(program, vertex);
	glDetachShader(program, fragment);
	glDeleteShader(vertex);
	glDeleteShader(fragment);

	GLint ok = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (ok != GL_TRUE) {
		char log[4096];
		GLsizei len = 0;
		glGetProgramInfoLog(program, sizeof(log), &len, log);
		wlr_log(WLR_ERROR, "Failed to link %s:\n%.*s", name, (int) len, log);
		glDeleteProgram(program);
		return false;
	}

	out->program = program;
	out->position = glGetAttribLocation(program, "kage_position");
	out->texcoord = glGetAttribLocation(program, "kage_texcoord");
	return true;
}

static uint32_t
next_random(uint32_t *state)
{
	/* xorshift32; only ever used for noise. */
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static GLuint
create_texture(GLsizei width, GLsizei height, const uint8_t *pixels, GLint filter, GLint wrap)
{
	GLuint texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
	return texture;
}

static void
create_textures(struct cg_shader_gl *gl)
{
	static uint8_t noise[NOISE_SIZE * NOISE_SIZE * 4];
	for (size_t i = 0; i < sizeof(noise); i += 4) {
		uint32_t r = next_random(&gl->random);
		noise[i + 0] = r & 0xff;
		noise[i + 1] = (r >> 8) & 0xff;
		noise[i + 2] = (r >> 16) & 0xff;
		noise[i + 3] = 0xff;
	}
	gl->noise_texture = create_texture(NOISE_SIZE, NOISE_SIZE, noise, GL_NEAREST, GL_REPEAT);

	/*
	 * One scanline from top to bottom: dark at the edges, where it meets
	 * its neighbours, and clear in the middle. The shader repeats it once
	 * per picture row and interpolates between the samples.
	 */
	uint8_t scanline[SCANLINE_SAMPLES * 4];
	for (int i = 0; i < SCANLINE_SAMPLES; i++) {
		double dark = 1.0 - sin(M_PI * (i + 0.5) / SCANLINE_SAMPLES);
		uint8_t value = (uint8_t) lround(dark * 255.0);
		scanline[i * 4 + 0] = value;
		scanline[i * 4 + 1] = value;
		scanline[i * 4 + 2] = value;
		scanline[i * 4 + 3] = 0xff;
	}
	gl->scanline_texture = create_texture(1, SCANLINE_SAMPLES, scanline, GL_LINEAR, GL_REPEAT);
}

bool
shader_init(struct cg_server *server)
{
	struct cg_shader *shader = &server->shader;

	if (!wlr_renderer_is_gles2(server->renderer)) {
		wlr_log(WLR_ERROR, "A shader needs the GLES2 renderer");
		return false;
	}

	struct cg_shader_gl *gl = calloc(1, sizeof(*gl));
	if (!gl) {
		wlr_log(WLR_ERROR, "Failed to allocate the shader");
		return false;
	}
	gl->renderer = server->renderer;
	gl->egl = wlr_gles2_renderer_get_egl(server->renderer);
	shader->gl = gl;

	clock_gettime(CLOCK_MONOTONIC, &gl->start);
	gl->random = (uint32_t) gl->start.tv_nsec | 1;

	struct egl_saved saved;
	if (!gl_begin(gl, &saved)) {
		return false;
	}

	bool ok = link_program(&gl->main, vertex_source, shader->source, shader->path);
	if (ok && gl->main.position < 0) {
		/* Only possible if the fragment shader broke the link. */
		wlr_log(WLR_ERROR, "Shader %s left the built-in vertex shader without a position", shader->path);
		ok = false;
	}

	if (ok) {
		GLuint program = gl->main.program;
		gl->uniforms.sourcesize = glGetUniformLocation(program, "kage_sourcesize");
		gl->uniforms.texturewidth = glGetUniformLocation(program, "texturewidth");
		gl->uniforms.time = glGetUniformLocation(program, "time");
		gl->uniforms.wobblephase = glGetUniformLocation(program, "wobblephase");
		gl->uniforms.noisescale = glGetUniformLocation(program, "noisescale");
		gl->uniforms.noiseoffset = glGetUniformLocation(program, "noiseoffset");

		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "tex"), 0);
		glUniform1i(glGetUniformLocation(program, "noisetex"), 1);
		glUniform1i(glGetUniformLocation(program, "scanlinetex"), 2);
		glUseProgram(0);

		ok = link_program(&gl->copy[COPY_2D], copy_vertex_source, copy_fragment_sources[COPY_2D],
				  "the built-in copy shader");
	}

	/* Only needed for buffers the driver will not sample as GL_TEXTURE_2D,
	 * and only possible where they can be imported at all. */
	if (ok && wlr_gles2_renderer_check_ext(server->renderer, "GL_OES_EGL_image_external") &&
	    !link_program(&gl->copy[COPY_EXTERNAL], copy_vertex_source, copy_fragment_sources[COPY_EXTERNAL],
			  "the built-in external copy shader")) {
		wlr_log(WLR_INFO, "Buffers only importable as external images cannot be post-processed");
	}

	if (ok) {
		for (int i = 0; i < COPY_KINDS; i++) {
			if (gl->copy[i].program) {
				glUseProgram(gl->copy[i].program);
				glUniform1i(glGetUniformLocation(gl->copy[i].program, "tex"), 0);
			}
		}
		glUseProgram(0);

		create_textures(gl);
	}

	gl_end(gl, &saved);

	if (ok) {
		wlr_log(WLR_INFO, "Post-processing every frame with %s", shader->path);
	}
	return ok;
}

void
shader_finish(struct cg_shader *shader)
{
	struct cg_shader_gl *gl = shader->gl;

	if (gl) {
		struct egl_saved saved;
		if (gl_begin(gl, &saved)) {
			glDeleteProgram(gl->main.program);
			for (int i = 0; i < COPY_KINDS; i++) {
				glDeleteProgram(gl->copy[i].program);
			}
			glDeleteTextures(1, &gl->noise_texture);
			glDeleteTextures(1, &gl->scanline_texture);
			gl_end(gl, &saved);
		}
		free(gl);
		shader->gl = NULL;
	}

	free(shader->path);
	free(shader->source);
	shader->path = NULL;
	shader->source = NULL;
}

bool
shader_setup_output(struct cg_output *output)
{
	output->shader = calloc(1, sizeof(*output->shader));
	if (!output->shader) {
		wlr_log(WLR_ERROR, "Failed to allocate the shader state for output %s", output->wlr_output->name);
		return false;
	}

	/*
	 * A hardware cursor is drawn over whatever the shader produced, neither
	 * distorted with the picture nor where the picture shows it. Drawn in
	 * software, it becomes part of the composite the shader reads. With
	 * -r the cursor lives on the virtual output, which has no planes, so
	 * this is already the case.
	 */
	if (!output->server->upscale.enabled) {
		wlr_output_lock_software_cursors(output->wlr_output, true);
	}

	return true;
}

static void
set_source(struct cg_shader_output *state, struct wlr_buffer *buffer)
{
	if (state->source == buffer) {
		return;
	}

	if (state->source) {
		wlr_buffer_unlock(state->source);
	}

	state->source = buffer ? wlr_buffer_lock(buffer) : NULL;
}

void
shader_output_finish(struct cg_output *output)
{
	struct cg_shader_output *state = output->shader;
	struct cg_shader_gl *gl = output->server->shader.gl;

	if (!state) {
		return;
	}

	set_source(state, NULL);
	wlr_swapchain_destroy(state->swapchain);

	struct egl_saved saved;
	if (gl && (state->copy_fbo || state->copy_texture) && gl_begin(gl, &saved)) {
		glDeleteFramebuffers(1, &state->copy_fbo);
		glDeleteTextures(1, &state->copy_texture);
		gl_end(gl, &saved);
	}

	free(state);
	output->shader = NULL;
}

/*
 * Composite the scene for this output into our own buffer, rather than the
 * output's, for the shader to read. The composite is only redrawn when the
 * scene has changed; the shader runs every frame regardless.
 */
static struct wlr_buffer *
composite_scene(struct cg_output *output)
{
	struct cg_shader_output *state = output->shader;
	struct wlr_output *wlr_output = output->wlr_output;
	const struct wlr_drm_format *format = &wlr_output->swapchain->format;

	if (state->swapchain &&
	    (state->swapchain->width != wlr_output->width || state->swapchain->height != wlr_output->height ||
	     state->swapchain->format.format != format->format)) {
		set_source(state, NULL);
		wlr_swapchain_destroy(state->swapchain);
		state->swapchain = NULL;
	}

	if (!state->swapchain) {
		/* The output's own format, so it is known to be renderable. */
		state->swapchain =
			wlr_swapchain_create(output->server->allocator, wlr_output->width, wlr_output->height, format);
		if (!state->swapchain) {
			wlr_log(WLR_ERROR, "Failed to create the shader's swapchain for output %s", wlr_output->name);
			return NULL;
		}
	}

	if (state->source && !wlr_scene_output_needs_frame(output->scene_output)) {
		return state->source;
	}

	struct wlr_output_state scene_state;
	wlr_output_state_init(&scene_state);

	struct wlr_scene_output_state_options options = {
		.swapchain = state->swapchain,
	};
	if (wlr_scene_output_build_state(output->scene_output, &scene_state, &options) &&
	    (scene_state.committed & WLR_OUTPUT_STATE_BUFFER) && scene_state.buffer) {
		set_source(state, scene_state.buffer);
	}

	/* Anything else the scene put in here (damage, gamma, a timeline to
	 * wait on) described its own buffer, which is not what gets committed.
	 * Our draw is queued behind the composite on the same context. */
	wlr_output_state_finish(&scene_state);

	return state->source;
}

/* A corner of a (width, height) picture, laid out by transform, in the
 * [-1, 1] coordinates of a (buffer_width, buffer_height) framebuffer. */
static void
corner_to_ndc(enum wl_output_transform transform, int width, int height, int x, int y, int buffer_width,
	      int buffer_height, GLfloat out[2])
{
	struct wlr_box point = {.x = x, .y = y};
	struct wlr_box transformed;
	wlr_box_transform(&transformed, &point, wlr_output_transform_invert(transform), width, height);

	/* Row 0 of a framebuffer object is its first row in memory, which is
	 * the top of the picture. */
	out[0] = 2.0f * transformed.x / buffer_width - 1.0f;
	out[1] = 2.0f * transformed.y / buffer_height - 1.0f;
}

static void
draw_quad(const struct shader_program *program, const GLfloat positions[8], const GLfloat texcoords[8])
{
	glVertexAttribPointer(program->position, 2, GL_FLOAT, GL_FALSE, 0, positions);
	glEnableVertexAttribArray(program->position);
	if (program->texcoord >= 0) {
		glVertexAttribPointer(program->texcoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
		glEnableVertexAttribArray(program->texcoord);
	}

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(program->position);
	if (program->texcoord >= 0) {
		glDisableVertexAttribArray(program->texcoord);
	}
}

/* Picture corners, in triangle strip order. */
static const GLfloat corner_uvs[8] = {0, 0, 1, 0, 0, 1, 1, 1};

static bool
ensure_copy_target(struct cg_shader_output *state, int width, int height)
{
	if (state->copy_fbo && state->copy_width == width && state->copy_height == height) {
		return true;
	}

	if (!state->copy_texture) {
		glGenTextures(1, &state->copy_texture);
	}
	if (!state->copy_fbo) {
		glGenFramebuffers(1, &state->copy_fbo);
	}

	glBindTexture(GL_TEXTURE_2D, state->copy_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, state->copy_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, state->copy_texture, 0);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE) {
		wlr_log(WLR_ERROR, "shader: the %dx%d copy target is incomplete (0x%x)", width, height, status);
		state->copy_width = state->copy_height = 0;
		return false;
	}

	state->copy_width = width;
	state->copy_height = height;
	return true;
}

/*
 * Returns the upright picture as a GL_TEXTURE_2D the shader can sample,
 * copying the source into one if it is not already.
 */
static GLuint
picture_texture(struct cg_shader_gl *gl, struct cg_shader_output *state, const struct frame *frame,
		const struct wlr_gles2_texture_attribs *attribs, int width, int height)
{
	if (frame->source_transform == WL_OUTPUT_TRANSFORM_NORMAL && attribs->target == GL_TEXTURE_2D) {
		return attribs->tex;
	}

	enum copy_kind kind = attribs->target == GL_TEXTURE_2D ? COPY_2D : COPY_EXTERNAL;
	const struct shader_program *copy = &gl->copy[kind];
	if (!copy->program) {
		static bool reported = false;
		if (!reported) {
			wlr_log(WLR_ERROR, "shader: cannot sample a buffer of GL target 0x%x", attribs->target);
			reported = true;
		}
		return 0;
	}

	if (!ensure_copy_target(state, width, height)) {
		return 0;
	}

	/* Each picture corner, and where the source holds it. */
	GLfloat positions[8], texcoords[8];
	for (int i = 0; i < 4; i++) {
		int x = (int) corner_uvs[i * 2] * width;
		int y = (int) corner_uvs[i * 2 + 1] * height;
		positions[i * 2] = corner_uvs[i * 2] * 2.0f - 1.0f;
		positions[i * 2 + 1] = corner_uvs[i * 2 + 1] * 2.0f - 1.0f;

		GLfloat ndc[2];
		corner_to_ndc(frame->source_transform, width, height, x, y, frame->source->width, frame->source->height,
			      ndc);
		texcoords[i * 2] = (ndc[0] + 1.0f) / 2.0f;
		texcoords[i * 2 + 1] = (ndc[1] + 1.0f) / 2.0f;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, state->copy_fbo);
	glViewport(0, 0, width, height);
	glUseProgram(copy->program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(attribs->target, attribs->tex);
	glTexParameteri(attribs->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(attribs->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	draw_quad(copy, positions, texcoords);

	glBindTexture(attribs->target, 0);
	return state->copy_texture;
}

static bool
draw(struct cg_shader_gl *gl, struct cg_output *output, struct wlr_buffer *target, const struct frame *frame)
{
	struct cg_shader_output *state = output->shader;
	struct wlr_output *wlr_output = output->wlr_output;

	struct wlr_texture *source = NULL;
	struct wlr_gles2_texture_attribs attribs = {0};
	if (frame->source) {
		source = wlr_texture_from_buffer(gl->renderer, frame->source);
		if (!source) {
			wlr_log(WLR_ERROR, "shader: unable to import the frame for output %s", wlr_output->name);
			return false;
		}
		wlr_gles2_texture_get_attribs(source, &attribs);
	}

	struct egl_saved saved;
	if (!gl_begin(gl, &saved)) {
		wlr_texture_destroy(source);
		return false;
	}

	bool ok = false;
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(gl->renderer, target);
	if (!fbo) {
		wlr_log(WLR_ERROR, "shader: unable to render to output %s", wlr_output->name);
		goto out;
	}

	int width = 0, height = 0;
	GLuint picture = 0;
	if (source) {
		width = frame->source->width;
		height = frame->source->height;
		wlr_output_transform_coords(frame->source_transform, &width, &height);
		picture = picture_texture(gl, state, frame, &attribs, width, height);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, target->width, target->height);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	if (picture) {
		/* The output in logical pixels, oriented as it is seen. */
		int output_width = target->width;
		int output_height = target->height;
		wlr_output_transform_coords(wlr_output->transform, &output_width, &output_height);

		float scale = wlr_output->scale;
		struct wlr_box dest = {
			.x = (int) roundf(frame->dest.x * scale),
			.y = (int) roundf(frame->dest.y * scale),
			.width = (int) roundf(frame->dest.width * scale),
			.height = (int) roundf(frame->dest.height * scale),
		};

		GLfloat positions[8];
		for (int i = 0; i < 4; i++) {
			int x = dest.x + (int) corner_uvs[i * 2] * dest.width;
			int y = dest.y + (int) corner_uvs[i * 2 + 1] * dest.height;
			corner_to_ndc(wlr_output->transform, output_width, output_height, x, y, target->width,
				      target->height, &positions[i * 2]);
		}

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (double) (now.tv_sec - gl->start.tv_sec) + (now.tv_nsec - gl->start.tv_nsec) / 1e9;

		glUseProgram(gl->main.program);
		glUniform2f(gl->uniforms.sourcesize, (GLfloat) width, (GLfloat) height);
		glUniform1f(gl->uniforms.texturewidth, (GLfloat) width);
		glUniform1f(gl->uniforms.time, (GLfloat) (fmod(elapsed, TIME_WRAP) + 1.0));
		glUniform1f(gl->uniforms.wobblephase, (GLfloat) fmod(elapsed * WOBBLE_SPEED, 2.0 * M_PI));
		glUniform2f(gl->uniforms.noisescale, (GLfloat) dest.width / NOISE_SIZE,
			    (GLfloat) dest.height / NOISE_SIZE);
		glUniform2f(gl->uniforms.noiseoffset, (next_random(&gl->random) & 0xffff) / 65536.0f,
			    (next_random(&gl->random) & 0xffff) / 65536.0f);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, picture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, gl->noise_texture);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, gl->scanline_texture);

		draw_quad(&gl->main, positions, corner_uvs);

		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	/* Implicit sync, as the renderer does when it has no timeline. */
	glFlush();
	ok = true;

out:
	gl_end(gl, &saved);
	wlr_texture_destroy(source);
	return ok;
}

bool
shader_commit_output(struct cg_output *output)
{
	struct cg_server *server = output->server;
	struct cg_shader_gl *gl = server->shader.gl;
	struct wlr_output *wlr_output = output->wlr_output;
	bool ok = false;

	if (!gl || !output->shader) {
		return false;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);

	if (!wlr_output_configure_primary_swapchain(wlr_output, &state, &wlr_output->swapchain)) {
		goto out;
	}

	int width, height;
	wlr_output_effective_resolution(wlr_output, &width, &height);

	struct frame frame = {
		.dest = {.width = width, .height = height},
	};

	if (server->upscale.enabled) {
		/* Where upscale mode would have put the virtual output. */
		frame.source = server->upscale.last_buffer;
		frame.source_transform = WL_OUTPUT_TRANSFORM_NORMAL;
		if (output->present_buffer && output->present_buffer->dst_width > 0) {
			frame.dest = (struct wlr_box){
				.x = output->present_buffer->node.x,
				.y = output->present_buffer->node.y,
				.width = output->present_buffer->dst_width,
				.height = output->present_buffer->dst_height,
			};
		}
	} else {
		frame.source = composite_scene(output);
		frame.source_transform = wlr_output->transform;
	}

	struct wlr_buffer *target = wlr_swapchain_acquire(wlr_output->swapchain);
	if (!target) {
		goto out;
	}

	bool drawn = draw(gl, output, target, &frame);
	if (drawn) {
		wlr_output_state_set_buffer(&state, target);
	}
	wlr_buffer_unlock(target);

	/* Committing every frame keeps the frame events, and so the
	 * animation, coming whether or not anything else changed. */
	if (drawn) {
		ok = wlr_output_commit_state(wlr_output, &state);
	}

out:
	wlr_output_state_finish(&state);
	return ok;
}
