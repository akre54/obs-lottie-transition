#ifndef LOTTIE_TRANSITION_H
#define LOTTIE_TRANSITION_H

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <util/threading.h>
#include "lottie-backend.h"
#include "transform-decode.h"

#define DATA_STRIP_HEIGHT 2
#define BROWSER_REGIONS 3 /* matteA, matteB, overlay */

struct lottie_transition {
	obs_source_t *source;

	/* Browser source for rendering Lottie */
	obs_source_t *browser;

	/* Texrenders for scene A and B with transforms applied */
	gs_texrender_t *texrender_a;
	gs_texrender_t *texrender_b;

	/* Staging surface for async data strip readback */
	gs_stagesurf_t *stagesurf;
	bool stage_ready; /* previous frame's stage data is available */

	/* Decoded transform data (from previous frame, 1-frame latency) */
	struct slot_transform transform_a;
	struct slot_transform transform_b;
	bool has_transforms; /* data strip contained valid data */

	/* Shader effect for matte compositing */
	gs_effect_t *effect;
	gs_eparam_t *ep_scene_a;
	gs_eparam_t *ep_scene_b;
	gs_eparam_t *ep_browser_tex;
	gs_eparam_t *ep_invert_matte;
	gs_eparam_t *ep_has_matte_a;
	gs_eparam_t *ep_has_matte_b;
	gs_eparam_t *ep_scene_size;
	gs_eparam_t *ep_slot_a_pos_scale;
	gs_eparam_t *ep_slot_a_rot_opacity;
	gs_eparam_t *ep_slot_b_pos_scale;
	gs_eparam_t *ep_slot_b_rot_opacity;

	/* Transition state */
	bool active;
	float progress; /* 0.0 to 1.0 */
	int tick_count;
	int render_count;
	int total_ticks;

	/* Cached animation info */
	float anim_total_frames;
	float anim_frame_rate;

	/* Output dimensions */
	uint32_t cx;
	uint32_t cy;

	/* Properties */
	char *lottie_file;
	enum lt_backend_type requested_backend;
	enum lt_backend_type effective_backend;
	void *thorvg_backend;
	bool invert_matte;
	bool has_matte_a;
	bool has_matte_b;
	bool scripts_injected;
	bool lottie_data_injected;

	/* E2E harness telemetry */
	bool e2e_enabled;
	bool e2e_trace;
	bool e2e_capture_frames;
	bool e2e_perf;
	char *e2e_capture_dir;
	uint32_t e2e_sample_mask;
	int e2e_transition_index;
	uint8_t *e2e_prev_sample;
	size_t e2e_prev_sample_size;
	uint32_t e2e_prev_width;
	uint32_t e2e_prev_height;
	uint64_t perf_transition_start_ns;
	uint64_t perf_last_render_start_ns;
	uint64_t perf_render_gap_sum_ns;
	uint64_t perf_render_gap_max_ns;
	uint64_t perf_callback_sum_ns;
	uint64_t perf_callback_max_ns;
	uint64_t perf_backend_sum_ns;
	uint64_t perf_backend_max_ns;
	uint64_t perf_backend_pass_sum_ns;
	uint64_t perf_backend_pass_max_ns;
	uint64_t perf_backend_slot_sum_ns;
	uint64_t perf_backend_slot_max_ns;
	uint64_t perf_backend_pack_sum_ns;
	uint64_t perf_backend_pack_max_ns;
	uint64_t perf_backend_upload_sum_ns;
	uint64_t perf_backend_upload_max_ns;
	uint64_t perf_composite_sum_ns;
	uint64_t perf_composite_max_ns;
	uint32_t perf_render_gap_count;
	uint32_t perf_gap_over_20ms;
	uint32_t perf_gap_over_33ms;
	uint32_t perf_gap_over_50ms;

	/* Thread safety */
	pthread_mutex_t mutex;
};

#endif
