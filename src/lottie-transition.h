#ifndef LOTTIE_TRANSITION_H
#define LOTTIE_TRANSITION_H

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <util/threading.h>
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
	bool invert_matte;
	bool scripts_injected;
	bool lottie_data_injected;

	/* Thread safety */
	pthread_mutex_t mutex;
};

#endif
