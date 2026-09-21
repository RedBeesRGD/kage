#ifndef CG_OUTPUT_H
#define CG_OUTPUT_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>

#include "server.h"
#include "view.h"

struct cg_output {
	struct cg_server *server;
	struct wlr_output *wlr_output;
	/* In upscale mode this belongs to present_scene, not to server->scene. */
	struct wlr_scene_output *scene_output;

	/* Upscale mode only: a scene holding nothing but the virtual output's
	 * last frame, scaled up to this output. */
	struct wlr_scene *present_scene;
	struct wlr_scene_buffer *present_buffer;

	struct wl_listener commit;
	struct wl_listener request_state;
	struct wl_listener destroy;
	struct wl_listener frame;

	struct wl_list link; // cg_server::outputs
};

void handle_output_manager_apply(struct wl_listener *listener, void *data);
void handle_output_manager_test(struct wl_listener *listener, void *data);
void handle_output_layout_change(struct wl_listener *listener, void *data);
void handle_new_output(struct wl_listener *listener, void *data);
void output_set_window_title(struct cg_output *output, const char *title);

#endif
