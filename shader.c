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

static char *
read_source(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "Unable to open shader '%s': %s\n", path, strerror(errno));
		return NULL;
	}

	char *source = malloc(SHADER_MAX_SOURCE + 1);
	if (!source) {
		fclose(file);
		return NULL;
	}

	size_t len = fread(source, 1, SHADER_MAX_SOURCE + 1, file);
	bool read_error = ferror(file);
	fclose(file);

	if (read_error) {
		fprintf(stderr, "Unable to read shader '%s'\n", path);
		free(source);
		return NULL;
	}
	if (len > SHADER_MAX_SOURCE) {
		fprintf(stderr, "Shader '%s' is larger than %d bytes\n", path, SHADER_MAX_SOURCE);
		free(source);
		return NULL;
	}
	if (len == 0) {
		fprintf(stderr, "Shader '%s' is empty\n", path);
		free(source);
		return NULL;
	}
	source[len] = '\0';

	return source;
}

bool
shader_parse_file(struct cg_shader *shader, const char *path)
{
	char *source = read_source(path);
	if (!source) {
		return false;
	}

	free(shader->source);
	free(shader->path);
	shader->source = source;
	shader->path = strdup(path);
	shader->enabled = true;
	return true;
}

bool
shader_parse_vertex_file(struct cg_shader *shader, const char *path)
{
	char *source = read_source(path);
	if (!source) {
		return false;
	}

	free(shader->vertex_source);
	free(shader->vertex_path);
	shader->vertex_source = source;
	shader->vertex_path = strdup(path);
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
	free(shader->vertex_source);
	free(shader->vertex_path);
	shader->source = NULL;
	shader->path = NULL;
	shader->vertex_source = NULL;
	shader->vertex_path = NULL;
}

#if WLR_HAS_GLES2_RENDERER

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <wayland-server-core.h>
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
#define UNIT_NEAREST 3

/* Nearest-filtered views kept at once; the virtual output rotates through
 * only a few buffers. */
#define NEAREST_VIEWS 8

#define NOISE_SIZE 64
#define ATTRIB_POSITION 0

/* Radians per second. */
#define WOBBLE_SPEED 4.0
#define TWO_PI 6.28318530717958647692

/* Keep the clock small enough for mediump arithmetic to stay useful. */
#define TIME_WRAP 3600.0

/*
 * The frame is drawn as a mesh of MESH_CELL_X by MESH_CELL_Y virtual pixel
 * cells, so a vertex stage can work out anything that varies smoothly across
 * the frame - a curvature, a wobble - once per vertex instead of once per
 * pixel. Across cells this size, a CRT curvature interpolates to within about
 * a hundredth of a pixel, and a horizontal wobble with a period of a few dozen
 * rows to within a few hundredths.
 */
#define MESH_CELL_X 8
#define MESH_CELL_Y 4
/* Vertices per side, so that the vertex count fits 16-bit indices. */
#define MESH_MAX_VERTICES 255

/*
 * The vertex stage used without -V. Provides the interface the fragment shader
 * is written against: `uvs` runs from 0,0 at the top left of the virtual
 * output to 1,1 at the bottom right, and `scanlinecoord` repeats the scanline
 * texture `scanlinerepeat` times down the frame. The mesh covers the whole
 * target, so gl_FragCoord is one virtual pixel per fragment.
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

struct cg_shader_gl;

/*
 * A second GL texture over the same DMA-BUF as a virtual output frame,
 * filtered nearest. GLES2 keeps the filter on the texture object, and the
 * renderer has only one object per buffer, so a shader that wants both a
 * linear and a nearest view of the frame needs its own import.
 */
struct nearest_view {
	struct cg_shader_gl *gl;
	struct wlr_buffer *buffer;
	EGLImageKHR image;
	GLuint tex;
	struct wl_listener destroy;
};

struct cg_shader_gl {
	EGLDisplay display;
	EGLContext context;

	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
	bool has_modifiers;
	struct nearest_view views[NEAREST_VIEWS];
	int next_view;

	GLuint program;
	GLuint noise_tex;
	GLuint scanline_tex;

	GLuint mesh_vbo;
	GLuint mesh_ibo;
	GLsizei mesh_indices;
	int mesh_width, mesh_height;

	GLint tex;
	GLint noisetex;
	GLint scanlinetex;
	GLint texnearest;
	GLint texturewidth;
	GLint texturesize;
	GLint texelsize;
	GLint time;
	GLint wobblephase;
	GLint noisescale;
	GLint noiseoffset;
	GLint scanlinerepeat;

	/* Whether each driven uniform was fixed with -U instead. */
	bool fixed_texturewidth;
	bool fixed_texturesize;
	bool fixed_texelsize;
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

static bool
has_extension(const char *list, const char *name)
{
	size_t len = strlen(name);
	for (const char *p = list; p && (p = strstr(p, name)) != NULL; p += len) {
		if ((p == list || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
			return true;
		}
	}
	return false;
}

/* Must be called with the renderer's context current. */
static bool
nearest_init(struct cg_shader_gl *gl)
{
	const char *egl_exts = eglQueryString(gl->display, EGL_EXTENSIONS);
	const char *gl_exts = (const char *) glGetString(GL_EXTENSIONS);

	if (!has_extension(egl_exts, "EGL_EXT_image_dma_buf_import") || !has_extension(gl_exts, "GL_OES_EGL_image")) {
		wlr_log(WLR_ERROR, "shader: texnearest needs EGL_EXT_image_dma_buf_import and GL_OES_EGL_image");
		return false;
	}

	gl->create_image = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
	gl->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
	gl->image_target_texture =
		(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	gl->has_modifiers = has_extension(egl_exts, "EGL_EXT_image_dma_buf_import_modifiers");

	if (!gl->create_image || !gl->destroy_image || !gl->image_target_texture) {
		wlr_log(WLR_ERROR, "shader: unable to load the EGLImage entry points for texnearest");
		return false;
	}

	return true;
}

/* Must be called with the renderer's context current. */
static void
nearest_release(struct nearest_view *view)
{
	if (!view->buffer) {
		return;
	}

	glDeleteTextures(1, &view->tex);
	view->gl->destroy_image(view->gl->display, view->image);
	wl_list_remove(&view->destroy.link);
	view->buffer = NULL;
	view->image = EGL_NO_IMAGE_KHR;
	view->tex = 0;
}

static void
handle_nearest_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct nearest_view *view = wl_container_of(listener, view, destroy);

	struct egl_saved saved;
	if (egl_enter(view->gl, &saved)) {
		nearest_release(view);
		egl_leave(view->gl, &saved);
	} else {
		/* Leak the GL objects rather than keep a dangling listener. */
		wl_list_remove(&view->destroy.link);
		view->buffer = NULL;
	}
}

/* Must be called with the renderer's context current. */
static GLuint
nearest_get(struct cg_shader_gl *gl, struct wlr_buffer *buffer)
{
	for (int i = 0; i < NEAREST_VIEWS; i++) {
		if (gl->views[i].buffer == buffer) {
			return gl->views[i].tex;
		}
	}

	struct wlr_dmabuf_attributes dmabuf;
	if (!wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		return 0;
	}
	if (dmabuf.modifier != DRM_FORMAT_MOD_INVALID && dmabuf.modifier != DRM_FORMAT_MOD_LINEAR &&
	    !gl->has_modifiers) {
		wlr_log(WLR_ERROR, "shader: EGL cannot import the frame's modifier for texnearest");
		return 0;
	}

	static const EGLint plane_attribs[WLR_DMABUF_MAX_PLANES][5] = {
		{EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
		 EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
		{EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
		 EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
		{EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
		 EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
		{EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
		 EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
	};

	EGLint attribs[64];
	int n = 0;
	attribs[n++] = EGL_WIDTH;
	attribs[n++] = dmabuf.width;
	attribs[n++] = EGL_HEIGHT;
	attribs[n++] = dmabuf.height;
	attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
	attribs[n++] = (EGLint) dmabuf.format;
	for (int i = 0; i < dmabuf.n_planes && i < WLR_DMABUF_MAX_PLANES; i++) {
		attribs[n++] = plane_attribs[i][0];
		attribs[n++] = dmabuf.fd[i];
		attribs[n++] = plane_attribs[i][1];
		attribs[n++] = (EGLint) dmabuf.offset[i];
		attribs[n++] = plane_attribs[i][2];
		attribs[n++] = (EGLint) dmabuf.stride[i];
		if (gl->has_modifiers && dmabuf.modifier != DRM_FORMAT_MOD_INVALID) {
			attribs[n++] = plane_attribs[i][3];
			attribs[n++] = (EGLint) (dmabuf.modifier & 0xffffffff);
			attribs[n++] = plane_attribs[i][4];
			attribs[n++] = (EGLint) (dmabuf.modifier >> 32);
		}
	}
	attribs[n++] = EGL_IMAGE_PRESERVED_KHR;
	attribs[n++] = EGL_TRUE;
	attribs[n++] = EGL_NONE;

	EGLImageKHR image = gl->create_image(gl->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
	if (image == EGL_NO_IMAGE_KHR) {
		wlr_log(WLR_ERROR, "shader: unable to import the frame for texnearest");
		return 0;
	}

	while (glGetError() != GL_NO_ERROR) {
	}

	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	gl->image_target_texture(GL_TEXTURE_2D, image);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	if (glGetError() != GL_NO_ERROR) {
		wlr_log(WLR_ERROR, "shader: the frame cannot be sampled as a 2D texture for texnearest");
		glDeleteTextures(1, &tex);
		gl->destroy_image(gl->display, image);
		return 0;
	}

	struct nearest_view *view = NULL;
	for (int i = 0; i < NEAREST_VIEWS && !view; i++) {
		if (!gl->views[i].buffer) {
			view = &gl->views[i];
		}
	}
	if (!view) {
		view = &gl->views[gl->next_view];
		gl->next_view = (gl->next_view + 1) % NEAREST_VIEWS;
		nearest_release(view);
	}

	view->gl = gl;
	view->buffer = buffer;
	view->image = image;
	view->tex = tex;
	view->destroy.notify = handle_nearest_buffer_destroy;
	wl_signal_add(&buffer->events.destroy, &view->destroy);

	return tex;
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
link_program(const struct cg_shader *shader)
{
	const char *path = shader->path;
	const char *fragment_source = shader->source;

	GLuint vertex = shader->vertex_source
				? compile_stage(GL_VERTEX_SHADER, shader->vertex_source, shader->vertex_path)
				: compile_stage(GL_VERTEX_SHADER, vertex_source, "the built-in vertex stage");
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

	gl->program = link_program(shader);
	if (!gl->program) {
		goto out;
	}

	gl->tex = glGetUniformLocation(gl->program, "tex");
	gl->noisetex = glGetUniformLocation(gl->program, "noisetex");
	gl->scanlinetex = glGetUniformLocation(gl->program, "scanlinetex");
	gl->texnearest = glGetUniformLocation(gl->program, "texnearest");
	gl->texturewidth = glGetUniformLocation(gl->program, "texturewidth");
	gl->texturesize = glGetUniformLocation(gl->program, "texturesize");
	gl->texelsize = glGetUniformLocation(gl->program, "texelsize");
	gl->time = glGetUniformLocation(gl->program, "time");
	gl->wobblephase = glGetUniformLocation(gl->program, "wobblephase");
	gl->noisescale = glGetUniformLocation(gl->program, "noisescale");
	gl->noiseoffset = glGetUniformLocation(gl->program, "noiseoffset");
	gl->scanlinerepeat = glGetUniformLocation(gl->program, "scanlinerepeat");

	gl->fixed_texturewidth = is_fixed(shader, "texturewidth");
	gl->fixed_texturesize = is_fixed(shader, "texturesize");
	gl->fixed_texelsize = is_fixed(shader, "texelsize");
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
	if (gl->texnearest >= 0) {
		glUniform1i(gl->texnearest, UNIT_NEAREST);
	}
	ok = set_fixed_uniforms(shader);
	glUseProgram(0);

	if (ok && gl->texnearest >= 0) {
		ok = nearest_init(gl);
	}

	if (!ok) {
		goto out;
	}

	shader->animated = (gl->time >= 0 && !gl->fixed_time) || (gl->wobblephase >= 0 && !gl->fixed_wobblephase) ||
			   (gl->noiseoffset >= 0 && !gl->fixed_noiseoffset);
	clock_gettime(CLOCK_MONOTONIC, &shader->start);

	wlr_log(WLR_INFO, "shader: loaded %s%s%s%s", shader->path, shader->vertex_path ? " with " : "",
		shader->vertex_path ? shader->vertex_path : "", shader->animated ? ", animated" : "");

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

static int
mesh_vertices(int pixels, int cell)
{
	int cells = (pixels + cell - 1) / cell;
	if (cells < 1) {
		cells = 1;
	}
	if (cells > MESH_MAX_VERTICES - 1) {
		cells = MESH_MAX_VERTICES - 1;
	}
	return cells + 1;
}

/* Must be called with the renderer's context current. */
static bool
ensure_mesh(struct cg_shader_gl *gl, int width, int height)
{
	if (gl->mesh_vbo && gl->mesh_width == width && gl->mesh_height == height) {
		return true;
	}

	int columns = mesh_vertices(width, MESH_CELL_X);
	int rows = mesh_vertices(height, MESH_CELL_Y);
	size_t vertex_count = (size_t) columns * rows;
	size_t index_count = (size_t) (columns - 1) * (rows - 1) * 6;

	GLfloat *vertices = malloc(vertex_count * 2 * sizeof(*vertices));
	GLushort *indices = malloc(index_count * sizeof(*indices));
	if (!vertices || !indices) {
		free(vertices);
		free(indices);
		return false;
	}

	GLfloat *v = vertices;
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < columns; x++) {
			*v++ = (GLfloat) x / (GLfloat) (columns - 1);
			*v++ = (GLfloat) y / (GLfloat) (rows - 1);
		}
	}

	GLushort *i = indices;
	for (int y = 0; y < rows - 1; y++) {
		for (int x = 0; x < columns - 1; x++) {
			GLushort corner = (GLushort) (y * columns + x);
			*i++ = corner;
			*i++ = (GLushort) (corner + 1);
			*i++ = (GLushort) (corner + columns);
			*i++ = (GLushort) (corner + 1);
			*i++ = (GLushort) (corner + columns + 1);
			*i++ = (GLushort) (corner + columns);
		}
	}

	if (!gl->mesh_vbo) {
		glGenBuffers(1, &gl->mesh_vbo);
		glGenBuffers(1, &gl->mesh_ibo);
	}

	glBindBuffer(GL_ARRAY_BUFFER, gl->mesh_vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (vertex_count * 2 * sizeof(*vertices)), vertices, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->mesh_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (index_count * sizeof(*indices)), indices, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	free(vertices);
	free(indices);

	gl->mesh_indices = (GLsizei) index_count;
	gl->mesh_width = width;
	gl->mesh_height = height;

	wlr_log(WLR_DEBUG, "shader: %dx%d mesh for %dx%d", columns, rows, width, height);
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
	if (gl->texturesize >= 0 && !gl->fixed_texturesize) {
		glUniform2f(gl->texturesize, (GLfloat) width, (GLfloat) height);
	}
	if (gl->texelsize >= 0 && !gl->fixed_texelsize) {
		glUniform2f(gl->texelsize, 1.0f / (GLfloat) width, 1.0f / (GLfloat) height);
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

	bool drawn = true;
	if (gl->texnearest >= 0) {
		GLuint nearest = nearest_get(gl, source);
		glActiveTexture(GL_TEXTURE0 + UNIT_NEAREST);
		glBindTexture(GL_TEXTURE_2D, nearest);
		glActiveTexture(GL_TEXTURE0 + UNIT_SOURCE);
		drawn = nearest != 0;
	}

	set_driven_uniforms(shader, source->width, source->height);

	drawn = drawn && ensure_mesh(gl, source->width, source->height);
	if (drawn) {
		glBindBuffer(GL_ARRAY_BUFFER, gl->mesh_vbo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->mesh_ibo);
		glVertexAttribPointer(ATTRIB_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
		glEnableVertexAttribArray(ATTRIB_POSITION);
		glDrawElements(GL_TRIANGLES, gl->mesh_indices, GL_UNSIGNED_SHORT, NULL);
		glDisableVertexAttribArray(ATTRIB_POSITION);
		/* The renderer draws from client memory. */
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	/* Leave the renderer the state it assumes. */
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0 + UNIT_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0 + UNIT_NOISE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0 + UNIT_SCANLINE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glUseProgram(0);

	bool ok = wlr_render_pass_submit(pass) && drawn;
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
			if (gl->mesh_vbo) {
				glDeleteBuffers(1, &gl->mesh_vbo);
				glDeleteBuffers(1, &gl->mesh_ibo);
			}
			for (int i = 0; i < NEAREST_VIEWS; i++) {
				nearest_release(&gl->views[i]);
			}
			egl_leave(gl, &saved);
		}
		/* Without the context, leak the GL objects but never a listener. */
		for (int i = 0; i < NEAREST_VIEWS; i++) {
			if (gl->views[i].buffer) {
				wl_list_remove(&gl->views[i].destroy.link);
				gl->views[i].buffer = NULL;
			}
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
