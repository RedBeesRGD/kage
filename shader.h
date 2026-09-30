#ifndef CG_SHADER_H
#define CG_SHADER_H

#include "config.h"

#include <stdbool.h>

struct cg_server;
struct cg_output;
struct wlr_backend;
struct wlr_renderer;
struct wlr_scene;

/*
 * When enabled, every frame is post-processed by a user-supplied GLSL ES 1.00
 * fragment shader on its way to the physical outputs. With -r, the shader
 * reads the virtual output and draws it into the physical output's box, so it
 * does the scaling itself. Without, the scene is composited offscreen for each
 * output and the shader draws that across the whole output.
 *
 * The fragment shader is linked against a built-in vertex shader, and sees:
 *
 *   varying vec2 uvs;              picture coordinates, (0,0) top left
 *   varying vec2 scanlinecoord;    x as uvs.x, y counting picture rows
 *   uniform sampler2D tex;         the picture, bilinear, clamped
 *   uniform sampler2D noisetex;    64x64 of random texels, repeating
 *   uniform sampler2D scanlinetex; one scanline's profile in .r, repeating
 *   uniform float texturewidth;    the picture's width in pixels
 *   uniform float time;            seconds, wrapping into [1, 9)
 *   uniform float wobblephase;     radians, advancing at pi per second
 *   uniform vec2 noisescale;       output pixels per noisetex texel
 *   uniform vec2 noiseoffset;      random each frame, in [0, 1)
 *
 * Any of these may be left undeclared. Other uniforms stay at zero.
 */
struct cg_shader {
	bool enabled;
	char *path;
	/* Read when the option is parsed, so that a bad path fails early. */
	char *source;
	/* GL objects, once the renderer exists. */
	struct cg_shader_gl *gl;
};

#if CAGE_HAS_SHADER

bool shader_load(struct cg_shader *shader, const char *path);

/* Use these in place of wlr_renderer_autocreate() and wlr_scene_create(). */
struct wlr_renderer *shader_create_renderer(struct wlr_backend *backend);
struct wlr_scene *shader_create_scene(void);

/* Must be called after the renderer has been created. */
bool shader_init(struct cg_server *server);
void shader_finish(struct cg_shader *shader);

bool shader_setup_output(struct cg_output *output);
/* Replaces the output's usual commit for as long as the shader is enabled. */
bool shader_commit_output(struct cg_output *output);
void shader_output_finish(struct cg_output *output);

#else

#include <wlr/util/log.h>

static inline bool
shader_load(struct cg_shader *shader, const char *path)
{
	wlr_log(WLR_ERROR, "Cannot use %s: Cage was built without the GLES2 renderer", path);
	return false;
}

static inline struct wlr_renderer *
shader_create_renderer(struct wlr_backend *backend)
{
	return NULL;
}

static inline struct wlr_scene *
shader_create_scene(void)
{
	return NULL;
}

static inline bool
shader_init(struct cg_server *server)
{
	return false;
}

static inline void
shader_finish(struct cg_shader *shader)
{
}

static inline bool
shader_setup_output(struct cg_output *output)
{
	return false;
}

static inline bool
shader_commit_output(struct cg_output *output)
{
	return false;
}

static inline void
shader_output_finish(struct cg_output *output)
{
}

#endif

#endif
