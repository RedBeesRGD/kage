#ifndef CG_UPSCALE_H
#define CG_UPSCALE_H

#include <stdbool.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_scene.h>

struct cg_server;
struct cg_output;

/*
 * When enabled, clients never see the physical outputs. Instead, a single
 * headless output of exactly `width`x`height` is published, and the frame
 * rendered for it is presented on every physical output according to
 * `enum cg_upscale_fit`.
 */
enum cg_upscale_fit {
	/* Largest whole multiple that fits, nearest-neighbour, centred. Pixel
	 * exact, at the cost of leaving a border on most panels. */
	CG_UPSCALE_INTEGER,
	/* Largest rectangle that keeps the virtual output's aspect ratio,
	 * interpolated, centred. Fills one axis exactly. */
	CG_UPSCALE_FIT,
	/* Stretched to the whole output, interpolated. Fills the panel and
	 * distorts the aspect ratio when the two do not agree. */
	CG_UPSCALE_FILL,
};

struct cg_upscale {
	bool enabled;
	enum cg_upscale_fit fit;
	int width, height;
	/* Never scale by more than this, whatever the output can hold. */
	int max_scale;

	struct cg_server *server;
	struct wlr_backend *backend;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	/* Most recent render of the virtual output, locked. */
	struct wlr_buffer *last_buffer;

	/* WLR_SCENE_DISABLE_DIRECT_SCANOUT as it was before we touched it, so
	 * the presentation scenes can be created with the user's setting. */
	char *scanout_env;
	bool scanout_env_set;

	struct wl_listener commit;
	struct wl_listener frame;
	struct wl_listener destroy;
};

bool upscale_parse_size(struct cg_upscale *upscale, const char *arg);
bool upscale_parse_max_scale(struct cg_upscale *upscale, const char *arg);
bool upscale_parse_fit(struct cg_upscale *upscale, const char *arg);

/* Must be called before the scene is created. */
void upscale_prepare_scene(struct cg_server *server);

/* Must be called before the renderer and allocator are created. */
bool upscale_create_backend(struct cg_server *server);
/* Must be called after the backend has been started. */
bool upscale_create_output(struct cg_server *server);
void upscale_destroy(struct cg_server *server);

/* Returns true if this new output is the virtual output, and claims it. */
bool upscale_claim_output(struct cg_server *server, struct wlr_output *wlr_output);

bool upscale_setup_sink(struct cg_output *output);
void upscale_update_sink(struct cg_output *output);

/* Call when a physical output is disabled or destroyed. */
void upscale_sink_gone(struct cg_server *server);

void upscale_render(struct cg_server *server);
bool upscale_commit_sink(struct cg_output *output);
void upscale_send_frame_done(struct cg_server *server, struct timespec *now);

#endif
