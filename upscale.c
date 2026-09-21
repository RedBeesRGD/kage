/*
 * Cage: A Wayland kiosk.
 *
 * Fixed-resolution virtual output, integer scaled onto the physical outputs.
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "output.h"
#include "server.h"
#include "upscale.h"
#include "view.h"

bool
upscale_parse_size(struct cg_upscale *upscale, const char *arg)
{
	char *end = NULL;

	long width = strtol(arg, &end, 10);
	if (end == arg || (*end != 'x' && *end != 'X') || width <= 0 || width > 16384) {
		return false;
	}

	const char *height_arg = end + 1;
	long height = strtol(height_arg, &end, 10);
	if (end == height_arg || *end != '\0' || height <= 0 || height > 16384) {
		return false;
	}

	upscale->width = (int) width;
	upscale->height = (int) height;
	upscale->enabled = true;
	return true;
}

bool
upscale_parse_max_scale(struct cg_upscale *upscale, const char *arg)
{
	char *end = NULL;
	long factor = strtol(arg, &end, 10);

	if (end == arg || *end != '\0' || factor < 1 || factor > 64) {
		return false;
	}

	upscale->max_scale = (int) factor;
	return true;
}

bool
upscale_parse_fit(struct cg_upscale *upscale, const char *arg)
{
	if (strcmp(arg, "exact") == 0) {
		upscale->fit = CG_UPSCALE_EXACT;
	} else if (strcmp(arg, "fit") == 0) {
		upscale->fit = CG_UPSCALE_FIT;
	} else if (strcmp(arg, "fill") == 0) {
		upscale->fit = CG_UPSCALE_FILL;
	} else {
		return false;
	}

	return true;
}

bool
upscale_parse_filter(struct cg_upscale *upscale, const char *arg)
{
	if (strcmp(arg, "auto") == 0) {
		upscale->filter = CG_UPSCALE_FILTER_AUTO;
	} else if (strcmp(arg, "nearest") == 0) {
		upscale->filter = CG_UPSCALE_FILTER_NEAREST;
	} else if (strcmp(arg, "bilinear") == 0) {
		upscale->filter = CG_UPSCALE_FILTER_BILINEAR;
	} else {
		return false;
	}

	return true;
}

/*
 * Left to itself, only a whole-number scale can be reproduced exactly, so that
 * is the only case where nearest-neighbour is the obvious answer; anything else
 * lands source pixels between destination pixels, where nearest turns a smooth
 * edge into a ragged one. That is a default rather than a rule, though - pixel
 * art stretched to fill the screen may well want nearest anyway - so -F
 * overrides it.
 */
static enum wlr_scale_filter_mode
upscale_filter(const struct cg_upscale *upscale)
{
	switch (upscale->filter) {
	case CG_UPSCALE_FILTER_NEAREST:
		return WLR_SCALE_FILTER_NEAREST;
	case CG_UPSCALE_FILTER_BILINEAR:
		return WLR_SCALE_FILTER_BILINEAR;
	case CG_UPSCALE_FILTER_AUTO:
	default:
		return upscale->fit == CG_UPSCALE_EXACT ? WLR_SCALE_FILTER_NEAREST : WLR_SCALE_FILTER_BILINEAR;
	}
}

static void
upscale_set_last_buffer(struct cg_upscale *upscale, struct wlr_buffer *buffer)
{
	if (upscale->last_buffer == buffer) {
		return;
	}

	if (upscale->last_buffer) {
		wlr_buffer_unlock(upscale->last_buffer);
	}

	upscale->last_buffer = buffer ? wlr_buffer_lock(buffer) : NULL;
}

/*
 * Every physical output presents the same virtual frame, so a single render of
 * the virtual output feeds all of them.
 */
static void
handle_virtual_output_commit(struct wl_listener *listener, void *data)
{
	struct cg_upscale *upscale = wl_container_of(listener, upscale, commit);
	struct wlr_output_event_commit *event = data;

	if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER) || event->state->buffer == NULL) {
		return;
	}

	upscale_set_last_buffer(upscale, event->state->buffer);

	static bool reported = false;
	if (!reported) {
		wlr_log(WLR_DEBUG, "upscale: virtual output committed a %dx%d buffer", event->state->buffer->width,
			event->state->buffer->height);
		reported = true;
	}

	struct cg_output *output;
	wl_list_for_each (output, &upscale->server->outputs, link) {
		if (output->present_buffer) {
			wlr_scene_buffer_set_buffer(output->present_buffer, event->state->buffer);
			/* Setting a buffer must not be allowed to lose this. */
			wlr_scene_buffer_set_filter_mode(output->present_buffer, upscale_filter(upscale));
		}
	}
}

static bool
has_enabled_sink(struct cg_server *server)
{
	struct cg_output *output;
	wl_list_for_each (output, &server->outputs, link) {
		if (output->wlr_output->enabled && output->scene_output) {
			return true;
		}
	}

	return false;
}

/*
 * The virtual output has something new to show.
 *
 * Physical outputs render and present in their own frame handlers, and a
 * physical output only gets another frame event once it commits. It only
 * commits when the virtual output has produced something, and the virtual
 * output is only rendered from a physical frame handler: left alone, the two
 * schedules deadlock as soon as the compositor goes idle, and the screen stays
 * black however hard the client draws. So waking the physical outputs here is
 * what keeps the loop alive.
 */
static void
handle_virtual_output_frame(struct wl_listener *listener, void *data)
{
	struct cg_upscale *upscale = wl_container_of(listener, upscale, frame);
	bool presented = false;

	struct cg_output *output;
	wl_list_for_each (output, &upscale->server->outputs, link) {
		if (output->wlr_output->enabled && output->scene_output) {
			wlr_output_schedule_frame(output->wlr_output);
			presented = true;
		}
	}

	if (presented) {
		return;
	}

	/* Nothing is presenting us, so drive the clients ourselves. */
	wlr_scene_output_commit(upscale->scene_output, NULL);

	struct timespec now = {0};
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(upscale->scene_output, &now);
}

static void
handle_virtual_output_destroy(struct wl_listener *listener, void *data)
{
	struct cg_upscale *upscale = wl_container_of(listener, upscale, destroy);

	wl_list_remove(&upscale->commit.link);
	wl_list_remove(&upscale->frame.link);
	wl_list_remove(&upscale->destroy.link);
	wl_list_init(&upscale->commit.link);
	wl_list_init(&upscale->frame.link);
	wl_list_init(&upscale->destroy.link);

	upscale_set_last_buffer(upscale, NULL);
	upscale->wlr_output = NULL;
	upscale->scene_output = NULL;
}

void
upscale_prepare_scene(struct cg_server *server)
{
	if (!server->upscale.enabled) {
		return;
	}

	/*
	 * Direct scanout would hand the client's own dmabuf straight to the
	 * presentation scene, and that is where pixel exactness quietly dies: a
	 * dmabuf that EGL reports as external_only imports as
	 * GL_TEXTURE_EXTERNAL_OES, and Mali's driver ignores sampler filter
	 * state on external images, so the nearest filter is discarded and the
	 * upscale comes out bilinear. Compositing into our own buffer instead
	 * costs one 1:1 copy of a small frame and keeps the sampling ours.
	 *
	 * The scene reads this when it is created, so this has to run first. It
	 * does not overwrite an existing setting, so exporting the variable as 0
	 * is still a way to get scanout back.
	 */
	const char *prev = getenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT");
	if (prev != NULL) {
		server->upscale.scanout_env = strdup(prev);
		server->upscale.scanout_env_set = server->upscale.scanout_env != NULL;
	}

	if (setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", true) != 0) {
		wlr_log_errno(WLR_ERROR, "Unable to disable direct scan-out");
	}
}

/*
 * Undo the above, for the presentation scenes only.
 *
 * The scene reads the variable once, when it is created, so scoping the
 * override to the client scene is a matter of restoring it before any
 * presentation scene exists. A physical output presenting the virtual frame
 * unscaled or at an integer multiple is exactly the case a scaling plane
 * handles for free, and the display controller is told which filter to use, so
 * the result is pixel exact without the GPU touching it.
 */
static void
upscale_restore_scanout(struct cg_upscale *upscale)
{
	static bool restored = false;

	if (restored) {
		return;
	}
	restored = true;

	if (upscale->scanout_env_set) {
		setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", upscale->scanout_env, true);
	} else {
		unsetenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT");
	}
}

bool
upscale_create_backend(struct cg_server *server)
{
	struct cg_upscale *upscale = &server->upscale;

	upscale->server = server;
	wl_list_init(&upscale->commit.link);
	wl_list_init(&upscale->frame.link);
	wl_list_init(&upscale->destroy.link);

	upscale->backend = wlr_headless_backend_create(wl_display_get_event_loop(server->wl_display));
	if (!upscale->backend) {
		wlr_log(WLR_ERROR, "Unable to create the headless backend for the virtual output");
		return false;
	}

	/* Adding it to the multi-backend before the renderer and allocator are
	 * created makes sure they are compatible with both backends. */
	if (!wlr_multi_backend_add(server->backend, upscale->backend)) {
		wlr_log(WLR_ERROR, "Unable to add the headless backend to the multi-backend");
		wlr_backend_destroy(upscale->backend);
		upscale->backend = NULL;
		return false;
	}

	return true;
}

bool
upscale_claim_output(struct cg_server *server, struct wlr_output *wlr_output)
{
	struct cg_upscale *upscale = &server->upscale;

	if (!upscale->enabled || upscale->backend == NULL || upscale->wlr_output != NULL) {
		return false;
	}
	if (wlr_output->backend != upscale->backend) {
		return false;
	}

	if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "Failed to initialize rendering for the virtual output");
		return true;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, upscale->width, upscale->height, 0);
	bool committed = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	if (!committed) {
		wlr_log(WLR_ERROR, "Failed to commit the virtual output");
		return true;
	}

	upscale->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	if (!upscale->scene_output) {
		wlr_log(WLR_ERROR, "Failed to allocate the scene output for the virtual output");
		return true;
	}

	/* This is the only output in the layout, so it is also the only
	 * wl_output clients ever see, and the only coordinate space the cursor
	 * and the views live in. */
	struct wlr_output_layout_output *layout_output = wlr_output_layout_add(server->output_layout, wlr_output, 0, 0);
	if (!layout_output) {
		wlr_log(WLR_ERROR, "Failed to add the virtual output to the output layout");
		return true;
	}
	wlr_scene_output_layout_add_output(server->scene_output_layout, layout_output, upscale->scene_output);

	upscale->wlr_output = wlr_output;

	upscale->commit.notify = handle_virtual_output_commit;
	wl_signal_add(&wlr_output->events.commit, &upscale->commit);
	upscale->frame.notify = handle_virtual_output_frame;
	wl_signal_add(&wlr_output->events.frame, &upscale->frame);
	upscale->destroy.notify = handle_virtual_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &upscale->destroy);

	wlr_log(WLR_INFO, "Clients see a single %dx%d output (%s)", upscale->width, upscale->height, wlr_output->name);
	return true;
}

bool
upscale_create_output(struct cg_server *server)
{
	struct cg_upscale *upscale = &server->upscale;

	/* wlr_headless_add_output() emits new_output synchronously, which is
	 * where upscale_claim_output() configures it. */
	struct wlr_output *wlr_output = wlr_headless_add_output(upscale->backend, upscale->width, upscale->height);
	if (!wlr_output) {
		wlr_log(WLR_ERROR, "Unable to create the virtual output");
		return false;
	}

	if (upscale->wlr_output != wlr_output) {
		wlr_log(WLR_ERROR, "Unable to configure the virtual output");
		return false;
	}

	return true;
}

bool
upscale_setup_sink(struct cg_output *output)
{
	struct cg_server *server = output->server;
	struct cg_upscale *upscale = &server->upscale;

	upscale_restore_scanout(upscale);

	output->present_scene = wlr_scene_create();
	if (!output->present_scene) {
		wlr_log(WLR_ERROR, "Failed to allocate the presentation scene");
		return false;
	}

	output->present_buffer = wlr_scene_buffer_create(&output->present_scene->tree, upscale->last_buffer);
	if (!output->present_buffer) {
		wlr_log(WLR_ERROR, "Failed to allocate the presentation buffer");
		return false;
	}

	wlr_scene_buffer_set_filter_mode(output->present_buffer, upscale_filter(upscale));

	output->scene_output = wlr_scene_output_create(output->present_scene, output->wlr_output);
	if (!output->scene_output) {
		wlr_log(WLR_ERROR, "Failed to allocate scene output");
		return false;
	}
	wlr_scene_output_set_position(output->scene_output, 0, 0);

	upscale_update_sink(output);
	return true;
}

void
upscale_update_sink(struct cg_output *output)
{
	struct cg_upscale *upscale = &output->server->upscale;

	if (!upscale->enabled || !output->present_buffer) {
		return;
	}

	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);
	if (width <= 0 || height <= 0) {
		return;
	}

	int dest_width, dest_height;
	const char *how;

	switch (upscale->fit) {
	case CG_UPSCALE_FILL:
		dest_width = width;
		dest_height = height;
		how = "stretched";
		break;
	case CG_UPSCALE_FIT: {
		/*
		 * The largest rectangle with the virtual output's aspect ratio.
		 * Comparing the cross products avoids floating point and tells
		 * us which axis runs out first.
		 */
		int64_t by_width = (int64_t) width * upscale->height;
		int64_t by_height = (int64_t) height * upscale->width;

		if (by_width <= by_height) {
			dest_width = width;
			dest_height = (int) (by_width / upscale->width);
		} else {
			dest_height = height;
			dest_width = (int) (by_height / upscale->height);
		}
		how = "fitted";
		break;
	}
	case CG_UPSCALE_EXACT:
	default: {
		/* The largest whole number of times the virtual output fits,
		 * capped so a very large panel cannot ask for more than we want
		 * to pay for. */
		int factor_x = width / upscale->width;
		int factor_y = height / upscale->height;
		int factor = factor_x < factor_y ? factor_x : factor_y;

		if (factor > upscale->max_scale) {
			factor = upscale->max_scale;
		}
		if (factor < 1) {
			factor = 1;
		}

		dest_width = upscale->width * factor;
		dest_height = upscale->height * factor;
		how = "exact";
		break;
	}
	}

	wlr_scene_buffer_set_filter_mode(output->present_buffer, upscale_filter(upscale));
	wlr_scene_buffer_set_dest_size(output->present_buffer, dest_width, dest_height);
	wlr_scene_node_set_position(&output->present_buffer->node, (width - dest_width) / 2,
				    (height - dest_height) / 2);

	wlr_log(WLR_INFO, "Output %s (%dx%d): %dx%d %s to %dx%d at %d,%d, %s", output->wlr_output->name, width, height,
		upscale->width, upscale->height, how, dest_width, dest_height, (width - dest_width) / 2,
		(height - dest_height) / 2,
		upscale_filter(upscale) == WLR_SCALE_FILTER_NEAREST ? "nearest" : "interpolated");
}

void
upscale_sink_gone(struct cg_server *server)
{
	struct cg_upscale *upscale = &server->upscale;

	if (!upscale->enabled || !upscale->wlr_output || has_enabled_sink(server)) {
		return;
	}

	/* Nothing presents the virtual output any more, so restart its own
	 * frame loop to keep clients rendering. */
	wlr_output_schedule_frame(upscale->wlr_output);
}

struct client_filter {
	enum wlr_scale_filter_mode filter;
	bool *reported;
};

static void
set_client_filter(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	const struct client_filter *ctx = data;

	wlr_scene_buffer_set_filter_mode(buffer, ctx->filter);

	if (ctx->reported != NULL && !*ctx->reported && buffer->buffer != NULL) {
		wlr_log(WLR_DEBUG, "upscale: client buffer %dx%d drawn as %dx%d", buffer->buffer->width,
			buffer->buffer->height, buffer->dst_width, buffer->dst_height);
	}
}

/*
 * The clients' own surfaces need the same filter as the presentation. A client
 * that scales inside the virtual output - a viewport, which is how SDL
 * implements its emulated fullscreen modes, or a buffer scale - is filtered
 * before we ever see it, and nothing at the end can undo that choice. Both
 * stages have to agree or the result is whichever one is worse.
 */
static void
pixelate_clients(struct cg_server *server)
{
	static bool reported = false;
	struct client_filter ctx = {
		.filter = upscale_filter(&server->upscale),
		.reported = &reported,
	};
	struct cg_view *view;

	wl_list_for_each (view, &server->views, link) {
		if (view->scene_tree) {
			wlr_scene_node_for_each_buffer(&view->scene_tree->node, set_client_filter, &ctx);
		}
	}
	reported = true;
}

void
upscale_render(struct cg_server *server)
{
	struct cg_upscale *upscale = &server->upscale;

	if (!upscale->enabled || !upscale->scene_output) {
		return;
	}

	pixelate_clients(server);

	/* A no-op when the virtual scene is undamaged, so calling this once per
	 * physical output per frame is cheap. */
	wlr_scene_output_commit(upscale->scene_output, NULL);
}

/*
 * Commit a presentation output, stating the filter.
 *
 * wlr_scene_output_commit() only expresses the filter when the frame is handed
 * to a plane, and never on the commit that sets the mode. A display controller
 * that can be told which filter to use should be told on every commit that
 * scales, whether or not that particular frame ends up scanned out, so build
 * the state here and say so.
 */
bool
upscale_commit_sink(struct cg_output *output)
{
	struct cg_server *server = output->server;
	bool ok = false;

	if (!server->upscale.enabled) {
		return wlr_scene_output_commit(output->scene_output, NULL);
	}

	if (!wlr_scene_output_needs_frame(output->scene_output)) {
		return true;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);

	if (!wlr_scene_output_build_state(output->scene_output, &state, NULL)) {
		goto out;
	}

	state.buffer_scale_filter = upscale_filter(&server->upscale);

	ok = wlr_output_commit_state(output->wlr_output, &state);

out:
	wlr_output_state_finish(&state);
	return ok;
}

void
upscale_send_frame_done(struct cg_server *server, struct timespec *now)
{
	struct cg_upscale *upscale = &server->upscale;

	if (!upscale->enabled || !upscale->scene_output) {
		return;
	}

	wlr_scene_output_send_frame_done(upscale->scene_output, now);
}

void
upscale_destroy(struct cg_server *server)
{
	struct cg_upscale *upscale = &server->upscale;

	if (!upscale->enabled) {
		return;
	}

	if (upscale->wlr_output) {
		wl_list_remove(&upscale->commit.link);
		wl_list_remove(&upscale->frame.link);
		wl_list_remove(&upscale->destroy.link);
		wl_list_init(&upscale->commit.link);
		wl_list_init(&upscale->frame.link);
		wl_list_init(&upscale->destroy.link);
		upscale->wlr_output = NULL;
		upscale->scene_output = NULL;
	}

	upscale_set_last_buffer(upscale, NULL);

	free(upscale->scanout_env);
	upscale->scanout_env = NULL;
	upscale->scanout_env_set = false;
}
