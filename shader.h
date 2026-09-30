#ifndef CG_SHADER_H
#define CG_SHADER_H

#include <stdbool.h>
#include <time.h>

struct wlr_allocator;
struct wlr_buffer;
struct wlr_renderer;
struct wlr_swapchain;

/*
 * A full-screen GLSL ES pass applied to every frame of the virtual output
 * (-r), at the virtual output's own resolution. The result replaces the
 * frame the presentation scenes would otherwise show, and is scaled onto the
 * physical outputs exactly as that frame would have been: same size, same
 * format, same modifier, same scaling plane, same filter.
 */

#define CG_SHADER_MAX_UNIFORMS 32

struct cg_shader_uniform {
	char *name;
	int count; /* Components, 1 to 4. */
	float value[4];
};

struct cg_shader_gl;

struct cg_shader {
	bool enabled;
	char *path;
	char *source;
	/* -V: replaces the built-in vertex stage. */
	char *vertex_path;
	char *vertex_source;

	/* Set with -U. A uniform set here is never driven by the compositor. */
	struct cg_shader_uniform uniforms[CG_SHADER_MAX_UNIFORMS];
	int uniforms_len;

	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct cg_shader_gl *gl;

	/* Output buffers, created on the first frame to match the virtual
	 * output's buffers exactly. */
	struct wlr_swapchain *swapchain;

	/* The shader reads a uniform that changes on its own, so frames have to
	 * be re-shaded even when the clients draw nothing new. */
	bool animated;
	struct timespec start;
};

bool shader_parse_file(struct cg_shader *shader, const char *path);
bool shader_parse_vertex_file(struct cg_shader *shader, const char *path);
bool shader_parse_uniform(struct cg_shader *shader, const char *arg);

/* Compiles and links the program. Needs the GLES2 renderer. */
bool shader_init(struct cg_shader *shader, struct wlr_renderer *renderer, struct wlr_allocator *allocator);
void shader_finish(struct cg_shader *shader);

/*
 * Shades `source` into a new buffer of the same size, format and modifier.
 * Returns that buffer locked, or NULL if this frame could not be shaded.
 */
struct wlr_buffer *shader_apply(struct cg_shader *shader, struct wlr_buffer *source);

#endif
