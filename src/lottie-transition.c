#include "lottie-transition.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/base.h>
#include "lottie-thorvg.h"

OBS_DECLARE_MODULE()

/* Temp file path for browser HTML */
#define HTML_TEMP_PATH "/tmp/obs-lottie-transition.html"
OBS_MODULE_USE_DEFAULT_LOCALE("obs-lottie-transition", "en-US")

#define TAG "[lottie-transition] "

static void lt_update(void *data, obs_data_t *settings);
static void create_render_backend(struct lottie_transition *lt);
static void destroy_render_backend(struct lottie_transition *lt);
static void draw_texture_direct(gs_texture_t *texture, uint32_t cx, uint32_t cy);
static void draw_matte_composite(struct lottie_transition *lt, gs_texture_t *scene_a,
				 gs_texture_t *scene_b, gs_texture_t *matte,
				 uint32_t cx, uint32_t cy);

static void lt_detect_matte_layers(struct lottie_transition *lt)
{
	lt->has_matte_a = false;
	lt->has_matte_b = false;

	if (!lt->lottie_file || !*lt->lottie_file)
		return;

	obs_data_t *root = obs_data_create_from_json_file(lt->lottie_file);
	if (!root)
		return;

	obs_data_array_t *layers = obs_data_get_array(root, "layers");
	if (!layers) {
		obs_data_release(root);
		return;
	}

	const size_t count = obs_data_array_count(layers);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *layer = obs_data_array_item(layers, i);
		const char *name = obs_data_get_string(layer, "nm");
		if (name && strcmp(name, "[MatteA]") == 0)
			lt->has_matte_a = true;
		else if (name && strcmp(name, "[MatteB]") == 0)
			lt->has_matte_b = true;
		obs_data_release(layer);
	}

	obs_data_array_release(layers);
	obs_data_release(root);
}

static bool lt_env_truthy(const char *name)
{
	const char *value = getenv(name);
	if (!value || !*value)
		return false;
	return strcmp(value, "0") != 0 && strcasecmp(value, "false") != 0 &&
	       strcasecmp(value, "no") != 0;
}

static void lt_json_escape(struct dstr *out, const char *value)
{
	const unsigned char *p = (const unsigned char *)(value ? value : "");

	for (; *p; p++) {
		switch (*p) {
		case '\\':
			dstr_cat(out, "\\\\");
			break;
		case '"':
			dstr_cat(out, "\\\"");
			break;
		case '\n':
			dstr_cat(out, "\\n");
			break;
		case '\r':
			dstr_cat(out, "\\r");
			break;
		case '\t':
			dstr_cat(out, "\\t");
			break;
		default:
			if (*p < 0x20)
				dstr_catf(out, "\\u%04x", (unsigned int)*p);
			else
				dstr_catf(out, "%c", *p);
		}
	}
}

static void lt_e2e_write_json_line(struct lottie_transition *lt, const char *event,
				       const char *extra_json)
{
	if (!lt->e2e_enabled || !lt->e2e_trace || !lt->e2e_capture_dir)
		return;

	struct dstr path = {0};
	dstr_catf(&path, "%s/plugin-events.jsonl", lt->e2e_capture_dir);
	FILE *file = fopen(path.array, "ab");
	if (!file) {
		dstr_free(&path);
		return;
	}

	struct dstr line = {0};
	struct dstr escaped_file = {0};
	struct dstr escaped_requested = {0};
	struct dstr escaped_effective = {0};

	lt_json_escape(&escaped_file, lt->lottie_file ? lt->lottie_file : "");
	lt_json_escape(&escaped_requested,
		       lt_backend_name(lt->requested_backend));
	lt_json_escape(&escaped_effective,
		       lt_backend_name(lt->effective_backend));

	dstr_catf(&line,
		  "{\"ts_ns\":%" PRIu64
		  ",\"event\":\"%s\",\"transition_index\":%d,"
		  "\"render_count\":%d,\"tick_count\":%d,\"progress\":%.6f,"
		  "\"backend_requested\":\"%s\",\"backend_effective\":\"%s\","
		  "\"lottie_file\":\"%s\"",
		  os_gettime_ns(), event, lt->e2e_transition_index,
		  lt->render_count, lt->tick_count, lt->progress,
		  escaped_requested.array, escaped_effective.array,
		  escaped_file.array);
	if (extra_json && *extra_json)
		dstr_catf(&line, ",%s", extra_json);
	dstr_cat(&line, "}\n");

	fwrite(line.array, 1, line.len, file);
	fclose(file);

	dstr_free(&escaped_effective);
	dstr_free(&escaped_requested);
	dstr_free(&escaped_file);
	dstr_free(&line);
	dstr_free(&path);
}

static void lt_e2e_init(struct lottie_transition *lt)
{
	const char *capture_dir = getenv("LT_E2E_CAPTURE_DIR");

	if (!capture_dir || !*capture_dir)
		return;

	lt->e2e_enabled = true;
	lt->e2e_trace = lt_env_truthy("LT_E2E_TRACE");
	lt->e2e_capture_frames = lt_env_truthy("LT_E2E_CAPTURE_FRAMES");
	lt->e2e_capture_dir = bstrdup(capture_dir);

	os_mkdirs(lt->e2e_capture_dir);
	if (lt->e2e_capture_frames) {
		struct dstr frames_dir = {0};
		dstr_catf(&frames_dir, "%s/frames", lt->e2e_capture_dir);
		os_mkdirs(frames_dir.array);
		dstr_free(&frames_dir);
	}

	lt_e2e_write_json_line(lt, "e2e_init", "\"capture_ready\":true");
}

static void lt_e2e_reset_transition_state(struct lottie_transition *lt)
{
	lt->e2e_sample_mask = 0;
	bfree(lt->e2e_prev_sample);
	lt->e2e_prev_sample = NULL;
	lt->e2e_prev_sample_size = 0;
	lt->e2e_prev_width = 0;
	lt->e2e_prev_height = 0;
}

static int lt_e2e_bucket_for_progress(float t)
{
	static const int buckets[] = {0, 25, 50, 75, 100};
	const float tolerance = 0.08f;
	int best_bucket = -1;
	float best_distance = 1.0f;

	for (size_t i = 0; i < sizeof(buckets) / sizeof(buckets[0]); i++) {
		float bucket_t = (float)buckets[i] / 100.0f;
		float distance = fabsf(t - bucket_t);
		if (distance <= tolerance && distance < best_distance) {
			best_distance = distance;
			best_bucket = buckets[i];
		}
	}

	return best_bucket;
}

static bool lt_e2e_should_sample(struct lottie_transition *lt, float t, int *bucket_percent)
{
	int bucket = lt_e2e_bucket_for_progress(t);
	uint32_t bit;

	if (!lt->e2e_enabled || bucket < 0)
		return false;

	bit = 1u << (unsigned int)(bucket / 25);
	if ((lt->e2e_sample_mask & bit) != 0)
		return false;

	lt->e2e_sample_mask |= bit;
	if (bucket_percent)
		*bucket_percent = bucket;
	return true;
}

static bool lt_write_ppm_file(const char *path, const uint8_t *pixels,
			      uint32_t width, uint32_t height)
{
	FILE *file = fopen(path, "wb");
	if (!file)
		return false;

	fprintf(file, "P6\n%u %u\n255\n", width, height);
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			const uint8_t *pixel = pixels + ((size_t)y * width + x) * 4;
			fwrite(pixel, 1, 3, file);
		}
	}

	fclose(file);
	return true;
}

static void lt_append_sample_json(struct dstr *extra, const char *name,
				       const uint8_t *pixels, uint32_t width,
				       uint32_t height, float u, float v)
{
	if (!extra || !pixels || width == 0 || height == 0)
		return;

	uint32_t x = (uint32_t)lroundf(u * (float)(width - 1));
	uint32_t y = (uint32_t)lroundf(v * (float)(height - 1));
	size_t offset = ((size_t)y * width + x) * 4;

	dstr_catf(extra,
		  ",\"sample_%s\":{\"x\":%u,\"y\":%u,\"r\":%u,\"g\":%u,\"b\":%u,\"a\":%u}",
		  name, x, y, (unsigned int)pixels[offset + 0],
		  (unsigned int)pixels[offset + 1],
		  (unsigned int)pixels[offset + 2],
		  (unsigned int)pixels[offset + 3]);
}

static void lt_e2e_capture_sample(struct lottie_transition *lt, gs_texture_t *texture,
				  uint32_t cx, uint32_t cy, int bucket_percent)
{
	if (!lt->e2e_enabled || !texture || !lt->e2e_capture_dir)
		return;

	gs_stagesurf_t *surface = gs_stagesurface_create(cx, cy, GS_RGBA);
	if (!surface)
		return;

	gs_stage_texture(surface, texture);

	uint8_t *mapped = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(surface, &mapped, &linesize)) {
		gs_stagesurface_destroy(surface);
		return;
	}

	size_t pixel_count = (size_t)cx * cy;
	size_t rgba_size = pixel_count * 4;
	uint8_t *copy = bzalloc(rgba_size);
	double sum_r = 0.0;
	double sum_g = 0.0;
	double sum_b = 0.0;
	double sum_a = 0.0;
	size_t nonblack_count = 0;
	size_t nonzero_alpha_count = 0;
	double delta_sum = 0.0;
	bool has_prev = lt->e2e_prev_sample &&
			lt->e2e_prev_sample_size == rgba_size &&
			lt->e2e_prev_width == cx &&
			lt->e2e_prev_height == cy;

	for (uint32_t y = 0; y < cy; y++) {
		const uint8_t *src_row = mapped + (size_t)y * linesize;
		uint8_t *dst_row = copy + (size_t)y * cx * 4;

		for (uint32_t x = 0; x < cx; x++) {
			size_t offset = (size_t)x * 4;
			size_t absolute = ((size_t)y * cx + x) * 4;
			uint8_t r = src_row[offset + 0];
			uint8_t g = src_row[offset + 1];
			uint8_t b = src_row[offset + 2];
			uint8_t a = src_row[offset + 3];

			dst_row[offset + 0] = r;
			dst_row[offset + 1] = g;
			dst_row[offset + 2] = b;
			dst_row[offset + 3] = a;

			sum_r += r;
			sum_g += g;
			sum_b += b;
			sum_a += a;
			if ((int)r + (int)g + (int)b > 24)
				nonblack_count++;
			if (a > 0)
				nonzero_alpha_count++;
			if (has_prev) {
				delta_sum += fabs((double)r - lt->e2e_prev_sample[absolute + 0]);
				delta_sum += fabs((double)g - lt->e2e_prev_sample[absolute + 1]);
				delta_sum += fabs((double)b - lt->e2e_prev_sample[absolute + 2]);
			}
		}
	}

	gs_stagesurface_unmap(surface);
	gs_stagesurface_destroy(surface);

	double mean_r = sum_r / (double)pixel_count;
	double mean_g = sum_g / (double)pixel_count;
	double mean_b = sum_b / (double)pixel_count;
	double mean_a = sum_a / (double)pixel_count;
	double nonblack_ratio = (double)nonblack_count / (double)pixel_count;
	double nonzero_alpha_ratio = (double)nonzero_alpha_count / (double)pixel_count;
	double mean_abs_rgb_delta = has_prev
		? delta_sum / ((double)pixel_count * 3.0)
		: 0.0;

	struct dstr frame_rel = {0};
	if (lt->e2e_capture_frames) {
		struct dstr frame_path = {0};
		dstr_catf(&frame_rel, "frames/trigger-%02d-p%03d.ppm",
			  lt->e2e_transition_index, bucket_percent);
		dstr_catf(&frame_path, "%s/%s", lt->e2e_capture_dir, frame_rel.array);
		lt_write_ppm_file(frame_path.array, copy, cx, cy);
		dstr_free(&frame_path);
	}

	bfree(lt->e2e_prev_sample);
	lt->e2e_prev_sample = copy;
	lt->e2e_prev_sample_size = rgba_size;
	lt->e2e_prev_width = cx;
	lt->e2e_prev_height = cy;

	struct dstr extra = {0};
	dstr_catf(&extra,
		  "\"bucket_percent\":%d,\"width\":%u,\"height\":%u,"
		  "\"mean_r\":%.4f,\"mean_g\":%.4f,\"mean_b\":%.4f,\"mean_a\":%.4f,"
		  "\"nonblack_ratio\":%.6f,\"nonzero_alpha_ratio\":%.6f,"
		  "\"mean_abs_rgb_delta\":%.6f",
		  bucket_percent, cx, cy, mean_r, mean_g, mean_b, mean_a,
		  nonblack_ratio, nonzero_alpha_ratio, mean_abs_rgb_delta);
	if (lt->has_transforms) {
		dstr_catf(&extra,
			  ",\"slot_a\":{\"pos_x\":%.4f,\"pos_y\":%.4f,\"scale_x\":%.4f,"
			  "\"scale_y\":%.4f,\"rotation\":%.4f,\"opacity\":%.4f},"
			  "\"slot_b\":{\"pos_x\":%.4f,\"pos_y\":%.4f,\"scale_x\":%.4f,"
			  "\"scale_y\":%.4f,\"rotation\":%.4f,\"opacity\":%.4f}",
			  lt->transform_a.pos_x, lt->transform_a.pos_y,
			  lt->transform_a.scale_x, lt->transform_a.scale_y,
			  lt->transform_a.rotation, lt->transform_a.opacity,
			  lt->transform_b.pos_x, lt->transform_b.pos_y,
			  lt->transform_b.scale_x, lt->transform_b.scale_y,
			  lt->transform_b.rotation, lt->transform_b.opacity);
	}
	if (frame_rel.len) {
		struct dstr escaped = {0};
		lt_json_escape(&escaped, frame_rel.array);
		dstr_catf(&extra, ",\"frame_path\":\"%s\"", escaped.array);
		dstr_free(&escaped);
	}
	lt_append_sample_json(&extra, "center", copy, cx, cy, 0.50f, 0.50f);
	lt_append_sample_json(&extra, "left_mid", copy, cx, cy, 0.25f, 0.50f);
	lt_append_sample_json(&extra, "right_mid", copy, cx, cy, 0.75f, 0.50f);
	lt_append_sample_json(&extra, "edge_25", copy, cx, cy, 0.25f, 0.25f);
	lt_append_sample_json(&extra, "edge_75", copy, cx, cy, 0.75f, 0.75f);

	lt_e2e_write_json_line(lt, "render_sample", extra.array);
	dstr_free(&extra);
	dstr_free(&frame_rel);
}

static void draw_texture_direct(gs_texture_t *texture, uint32_t cx, uint32_t cy)
{
	gs_effect_t *effect;
	gs_eparam_t *image;
	gs_technique_t *tech;

	if (!texture)
		return;

	effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	tech = gs_effect_get_technique(effect, "Draw");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(texture, 0, cx, cy);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

struct lt_texture_metrics {
	double mean_r;
	double mean_g;
	double mean_b;
	double mean_a;
	double nonblack_ratio;
	double nonzero_alpha_ratio;
};

static bool lt_collect_texture_metrics(gs_texture_t *texture, uint32_t cx, uint32_t cy,
				       struct lt_texture_metrics *metrics)
{
	if (!texture || !metrics)
		return false;

	gs_stagesurf_t *surface = gs_stagesurface_create(cx, cy, GS_RGBA);
	if (!surface)
		return false;

	gs_stage_texture(surface, texture);

	uint8_t *mapped = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(surface, &mapped, &linesize)) {
		gs_stagesurface_destroy(surface);
		return false;
	}

	size_t pixel_count = (size_t)cx * cy;
	double sum_r = 0.0;
	double sum_g = 0.0;
	double sum_b = 0.0;
	double sum_a = 0.0;
	size_t nonblack_count = 0;
	size_t nonzero_alpha_count = 0;

	for (uint32_t y = 0; y < cy; y++) {
		const uint8_t *src_row = mapped + (size_t)y * linesize;
		for (uint32_t x = 0; x < cx; x++) {
			size_t offset = (size_t)x * 4;
			uint8_t r = src_row[offset + 0];
			uint8_t g = src_row[offset + 1];
			uint8_t b = src_row[offset + 2];
			uint8_t a = src_row[offset + 3];
			sum_r += r;
			sum_g += g;
			sum_b += b;
			sum_a += a;
			if ((int)r + (int)g + (int)b > 24)
				nonblack_count++;
			if (a > 0)
				nonzero_alpha_count++;
		}
	}

	gs_stagesurface_unmap(surface);
	gs_stagesurface_destroy(surface);

	metrics->mean_r = sum_r / (double)pixel_count;
	metrics->mean_g = sum_g / (double)pixel_count;
	metrics->mean_b = sum_b / (double)pixel_count;
	metrics->mean_a = sum_a / (double)pixel_count;
	metrics->nonblack_ratio = (double)nonblack_count / (double)pixel_count;
	metrics->nonzero_alpha_ratio = (double)nonzero_alpha_count / (double)pixel_count;
	return true;
}

static void draw_matte_composite(struct lottie_transition *lt, gs_texture_t *scene_a,
				 gs_texture_t *scene_b, gs_texture_t *matte,
				 uint32_t cx, uint32_t cy)
{
	struct slot_transform slot_a = lt->transform_a;
	struct slot_transform slot_b = lt->transform_b;
	struct vec2 scene_size;
	struct vec4 slot_a_pos_scale;
	struct vec2 slot_a_rot_opacity;
	struct vec4 slot_b_pos_scale;
	struct vec2 slot_b_rot_opacity;

	if (!lt->has_transforms) {
		slot_transform_identity(&slot_a);
		slot_transform_identity(&slot_b);
		slot_a.pos_x = (float)cx * 0.5f;
		slot_a.pos_y = (float)cy * 0.5f;
		slot_b.pos_x = (float)cx * 0.5f;
		slot_b.pos_y = (float)cy * 0.5f;
	}

	gs_effect_set_texture(lt->ep_scene_a, scene_a);
	gs_effect_set_texture(lt->ep_scene_b, scene_b);
	gs_effect_set_texture(lt->ep_browser_tex, matte);
	gs_effect_set_bool(lt->ep_invert_matte, lt->invert_matte);
	gs_effect_set_bool(lt->ep_has_matte_a, lt->has_matte_a);
	gs_effect_set_bool(lt->ep_has_matte_b, lt->has_matte_b);
	vec2_set(&scene_size, (float)cx, (float)cy);
	vec4_set(&slot_a_pos_scale, slot_a.pos_x, slot_a.pos_y,
		 slot_a.scale_x, slot_a.scale_y);
	vec2_set(&slot_a_rot_opacity, slot_a.rotation, slot_a.opacity);
	vec4_set(&slot_b_pos_scale, slot_b.pos_x, slot_b.pos_y,
		 slot_b.scale_x, slot_b.scale_y);
	vec2_set(&slot_b_rot_opacity, slot_b.rotation, slot_b.opacity);
	gs_effect_set_vec2(lt->ep_scene_size, &scene_size);
	gs_effect_set_vec4(lt->ep_slot_a_pos_scale, &slot_a_pos_scale);
	gs_effect_set_vec2(lt->ep_slot_a_rot_opacity, &slot_a_rot_opacity);
	gs_effect_set_vec4(lt->ep_slot_b_pos_scale, &slot_b_pos_scale);
	gs_effect_set_vec2(lt->ep_slot_b_rot_opacity, &slot_b_rot_opacity);

	gs_technique_t *tech = gs_effect_get_technique(lt->effect, "MatteComposite");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(matte, 0, cx, cy);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

static void render_and_optionally_capture_composite(struct lottie_transition *lt,
						    gs_texture_t *scene_a,
						    gs_texture_t *scene_b,
						    gs_texture_t *matte,
						    uint32_t cx, uint32_t cy,
						    float t)
{
	if (!lt->e2e_enabled || !matte) {
		draw_matte_composite(lt, scene_a, scene_b, matte, cx, cy);
		return;
	}

	int sample_bucket = -1;
	if (!lt_e2e_should_sample(lt, t, &sample_bucket)) {
		draw_matte_composite(lt, scene_a, scene_b, matte, cx, cy);
		return;
	}

	gs_texrender_t *capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!capture) {
		draw_matte_composite(lt, scene_a, scene_b, matte, cx, cy);
		return;
	}

	if (!gs_texrender_begin(capture, cx, cy)) {
		gs_texrender_destroy(capture);
		draw_matte_composite(lt, scene_a, scene_b, matte, cx, cy);
		return;
	}

	if (lt->e2e_trace) {
		struct lt_texture_metrics scene_a_metrics = {0};
		struct lt_texture_metrics scene_b_metrics = {0};
		struct lt_texture_metrics matte_metrics = {0};
		bool got_a = lt_collect_texture_metrics(scene_a, cx, cy, &scene_a_metrics);
		bool got_b = lt_collect_texture_metrics(scene_b, cx, cy, &scene_b_metrics);
		bool got_m = lt_collect_texture_metrics(matte, cx, cy, &matte_metrics);
		if (got_a || got_b || got_m) {
			struct dstr extra = {0};
			dstr_catf(&extra, "\"bucket_percent\":%d", sample_bucket);
			if (got_a) {
				dstr_catf(&extra,
					  ",\"scene_a_metrics\":{\"mean_r\":%.4f,\"mean_g\":%.4f,"
					  "\"mean_b\":%.4f,\"mean_a\":%.4f,\"nonblack_ratio\":%.6f}",
					  scene_a_metrics.mean_r, scene_a_metrics.mean_g,
					  scene_a_metrics.mean_b, scene_a_metrics.mean_a,
					  scene_a_metrics.nonblack_ratio);
			}
			if (got_b) {
				dstr_catf(&extra,
					  ",\"scene_b_metrics\":{\"mean_r\":%.4f,\"mean_g\":%.4f,"
					  "\"mean_b\":%.4f,\"mean_a\":%.4f,\"nonblack_ratio\":%.6f}",
					  scene_b_metrics.mean_r, scene_b_metrics.mean_g,
					  scene_b_metrics.mean_b, scene_b_metrics.mean_a,
					  scene_b_metrics.nonblack_ratio);
			}
			if (got_m) {
				dstr_catf(&extra,
					  ",\"matte_metrics\":{\"mean_r\":%.4f,\"mean_g\":%.4f,"
					  "\"mean_b\":%.4f,\"mean_a\":%.4f,\"nonblack_ratio\":%.6f}",
					  matte_metrics.mean_r, matte_metrics.mean_g,
					  matte_metrics.mean_b, matte_metrics.mean_a,
					  matte_metrics.nonblack_ratio);
			}
			lt_e2e_write_json_line(lt, "input_sample", extra.array);
			dstr_free(&extra);
		}
	}

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	draw_matte_composite(lt, scene_a, scene_b, matte, cx, cy);
	gs_texrender_end(capture);

	gs_texture_t *capture_texture = gs_texrender_get_texture(capture);
	if (capture_texture) {
		lt_e2e_capture_sample(lt, capture_texture, cx, cy, sample_bucket);
		draw_texture_direct(capture_texture, cx, cy);
	} else {
		draw_matte_composite(lt, scene_a, scene_b, matte, cx, cy);
	}

	gs_texrender_destroy(capture);
}

/* ------------------------------------------------------------------ */
/* Helper: build HTML file for browser source (written to temp file)   */
/* ------------------------------------------------------------------ */

static bool build_html_file(struct lottie_transition *lt)
{
	/*
	 * Strategy: lottie.min.js (305KB) loaded via external <script src>,
	 * everything else inlined. Large inline JS (300KB+) causes CEF to
	 * not execute JS. Bridge.js is only ~8KB so safe for inline.
	 * The is_local_file setting uses http://absolute/ scheme internally,
	 * so relative src paths resolve against the HTML file's directory.
	 */

	/* Copy lottie.min.js to /tmp (external, too large to inline) */
	char *lottie_path = obs_module_file("web/lottie.min.js");
	if (!lottie_path) {
		blog(LOG_ERROR, TAG "Failed to find lottie.min.js");
		return false;
	}
	char *lottie_js = os_quick_read_utf8_file(lottie_path);
	bfree(lottie_path);
	if (!lottie_js) {
		blog(LOG_ERROR, TAG "Failed to read lottie.min.js");
		return false;
	}
	os_quick_write_utf8_file("/tmp/obs-lottie-lottie.min.js",
				 lottie_js, strlen(lottie_js), false);
	bfree(lottie_js);

	/* Read bridge-core.js, backend-plan.js and bridge.js for inlining.
	 * Order matters in the browser: backend-plan.js depends on BridgeCore. */
	char *bridge_core_path = obs_module_file("web/bridge-core.js");
	char *bridge_core_js = bridge_core_path ?
		os_quick_read_utf8_file(bridge_core_path) : NULL;
	bfree(bridge_core_path);
	if (!bridge_core_js) {
		blog(LOG_ERROR, TAG "Failed to read bridge-core.js");
		return false;
	}

	char *backend_plan_path = obs_module_file("web/backend-plan.js");
	char *backend_plan_js = backend_plan_path ?
		os_quick_read_utf8_file(backend_plan_path) : NULL;
	bfree(backend_plan_path);
	if (!backend_plan_js) {
		bfree(bridge_core_js);
		blog(LOG_ERROR, TAG "Failed to read backend-plan.js");
		return false;
	}

	char *bridge_path = obs_module_file("web/bridge.js");
	char *bridge_js = bridge_path ? os_quick_read_utf8_file(bridge_path) : NULL;
	bfree(bridge_path);
	if (!bridge_js) {
		bfree(bridge_core_js);
		bfree(backend_plan_js);
		blog(LOG_ERROR, TAG "Failed to read bridge.js");
		return false;
	}

	/* Escape </ as <\/ to prevent premature </script> closure */
	struct dstr bridge_escaped = {0};
	for (const char *p = bridge_core_js; *p; p++) {
		if (p[0] == '<' && p[1] == '/')
			{ dstr_cat(&bridge_escaped, "<\\/"); p++; }
		else
			dstr_catf(&bridge_escaped, "%c", *p);
	}
	bfree(bridge_core_js);
	for (const char *p = backend_plan_js; *p; p++) {
		if (p[0] == '<' && p[1] == '/')
			{ dstr_cat(&bridge_escaped, "<\\/"); p++; }
		else
			dstr_catf(&bridge_escaped, "%c", *p);
	}
	bfree(backend_plan_js);
	for (const char *p = bridge_js; *p; p++) {
		if (p[0] == '<' && p[1] == '/')
			{ dstr_cat(&bridge_escaped, "<\\/"); p++; }
		else
			dstr_catf(&bridge_escaped, "%c", *p);
	}
	bfree(bridge_js);

	/* Read animation JSON */
	char *anim_json = NULL;
	if (lt->lottie_file && *lt->lottie_file)
		anim_json = os_quick_read_utf8_file(lt->lottie_file);

	/* Build HTML: config inline, lottie external (async), anim inline, bridge inline.
	 * lottie.min.js loaded with async=false defer pattern to not block parsing
	 * but still execute before DOMContentLoaded. Bridge.js waits for lottie. */
	struct dstr html = {0};
	dstr_cat(&html,
		"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
		"<style>*{margin:0;padding:0;overflow:hidden;background:#f00}"
		"canvas{display:block}</style></head><body>"
		"<canvas id='lottie-canvas'></canvas>");

	/* Inline diagnostic: paint canvas lime immediately to verify JS runs */
	dstr_catf(&html,
		"<script>"
		"try{"
		"var _c=document.getElementById('lottie-canvas');"
		"_c.width=%u;_c.height=%u;"
		"var _x=_c.getContext('2d');"
		"_x.fillStyle='lime';"
		"_x.fillRect(0,0,%u,%u);"
		"console.log('[diag] Inline JS executed, painted lime');"
		"}catch(e){console.log('[diag] ERROR:'+e);}"
		"</script>",
		lt->cx, lt->cy,
		lt->cx, lt->cy);

	dstr_catf(&html,
		"<script>window._obsConfig={width:%u,height:%u,dataStripHeight:%d};</script>",
		lt->cx, lt->cy, DATA_STRIP_HEIGHT);

	/* Load lottie.min.js dynamically — blocking <script src> kills
	 * subsequent inline scripts in CEF private browser sources.
	 * Bridge.js will wait for lottie to be available. */
	if (anim_json)
		dstr_catf(&html, "<script>window._lottieData=%s;</script>", anim_json);

	dstr_cat(&html,
		"<script>"
		"var _s=document.createElement('script');"
		"_s.src='obs-lottie-lottie.min.js';"
		"_s.onload=function(){"
		  "window._lottieReady=true;"
		  "if(window._onLottieReady) window._onLottieReady();"
		"};"
		"_s.onerror=function(){"
		  "var _c=document.getElementById('lottie-canvas');"
		  "var _x=_c.getContext('2d');"
		  "_x.fillStyle='rgb(255,0,128)';"
		  "_x.fillRect(0,0,_c.width,_c.height);"
		"};"
		"document.head.appendChild(_s);"
		"</script>");

	dstr_catf(&html, "<script>%s</script>", bridge_escaped.array);
	dstr_cat(&html, "</body></html>");

	dstr_free(&bridge_escaped);

	blog(LOG_INFO, TAG "HTML size: %lu bytes (lottie external, anim=%s)",
	     (unsigned long)html.len, anim_json ? "yes" : "no");

	os_quick_write_utf8_file(HTML_TEMP_PATH, html.array, html.len, false);
	blog(LOG_INFO, TAG "Wrote HTML to %s", HTML_TEMP_PATH);

	dstr_free(&html);
	bfree(anim_json);
	return true;
}

/* ------------------------------------------------------------------ */
/* Helper: create/destroy browser source                               */
/* ------------------------------------------------------------------ */

static void create_browser_source(struct lottie_transition *lt)
{
	if (lt->browser)
		return;

	if (!build_html_file(lt)) {
		blog(LOG_ERROR, TAG "Failed to build HTML file");
		return;
	}

	obs_data_t *browser_settings = obs_data_create();
	obs_data_set_bool(browser_settings, "is_local_file", true);
	obs_data_set_string(browser_settings, "local_file", HTML_TEMP_PATH);
	obs_data_set_int(browser_settings, "width", lt->cx);
	obs_data_set_int(browser_settings, "height", lt->cy);
	obs_data_set_int(browser_settings, "fps", 60);
	obs_data_set_bool(browser_settings, "shutdown", false);
	obs_data_set_bool(browser_settings, "restart_when_active", false);

	blog(LOG_INFO, TAG "Creating browser (is_local_file): size=%ux%u",
	     lt->cx, lt->cy);

	lt->browser = obs_source_create_private("browser_source",
						"lottie_transition_browser",
						browser_settings);

	obs_data_release(browser_settings);

	if (!lt->browser) {
		blog(LOG_ERROR, TAG "Failed to create browser source!");
		return;
	}

	obs_source_inc_showing(lt->browser);
	obs_source_inc_active(lt->browser);


	blog(LOG_INFO, TAG "Browser created and activated, active=%d showing=%d",
	     (int)obs_source_active(lt->browser),
	     (int)obs_source_showing(lt->browser));
}

static void destroy_browser_source(struct lottie_transition *lt)
{
	if (lt->browser) {
		obs_source_dec_active(lt->browser);
		obs_source_dec_showing(lt->browser);
		obs_source_release(lt->browser);
		lt->browser = NULL;
	}
}

static void create_thorvg_backend(struct lottie_transition *lt)
{
	if (lt->thorvg_backend)
		return;

	lt->thorvg_backend = lt_thorvg_create(lt->lottie_file, lt->cx, lt->cy);
	if (!lt->thorvg_backend) {
		blog(LOG_ERROR, TAG "Failed to create ThorVG backend");
		return;
	}

	blog(LOG_INFO, TAG "Created ThorVG backend: size=%ux%u", lt->cx, lt->cy);
}

static void destroy_thorvg_backend(struct lottie_transition *lt)
{
	if (lt->thorvg_backend) {
		lt_thorvg_destroy(lt->thorvg_backend);
		lt->thorvg_backend = NULL;
	}
}

static void create_render_backend(struct lottie_transition *lt)
{
	switch (lt->effective_backend) {
	case LT_BACKEND_THORVG:
		create_thorvg_backend(lt);
		break;
	case LT_BACKEND_BROWSER:
	default:
		create_browser_source(lt);
		break;
	}
}

static void destroy_render_backend(struct lottie_transition *lt)
{
	switch (lt->effective_backend) {
	case LT_BACKEND_THORVG:
		destroy_thorvg_backend(lt);
		break;
	case LT_BACKEND_BROWSER:
	default:
		destroy_browser_source(lt);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Helper: GPU resource management                                     */
/* ------------------------------------------------------------------ */

static void ensure_stagesurf(struct lottie_transition *lt)
{
	if (!lt->stagesurf)
		lt->stagesurf = gs_stagesurface_create(lt->cx,
						       DATA_STRIP_HEIGHT,
						       GS_RGBA);
}

/* ------------------------------------------------------------------ */
/* Helper: read data strip from staging surface                        */
/* ------------------------------------------------------------------ */

static void read_data_strip(struct lottie_transition *lt)
{
	if (!lt->stagesurf || !lt->stage_ready)
		return;

	uint8_t *data;
	uint32_t linesize;

	if (gs_stagesurface_map(lt->stagesurf, &data, &linesize)) {
		transform_decode_from_pixels(data, linesize, lt->cx,
					     &lt->transform_a,
					     &lt->transform_b);
		lt->has_transforms = true;
		gs_stagesurface_unmap(lt->stagesurf);
	}

	lt->stage_ready = false;
}

/* ------------------------------------------------------------------ */
/* Helper: decode transforms from browser texture bottom row           */
/* ------------------------------------------------------------------ */

static void decode_transforms_from_texture(struct lottie_transition *lt,
					   gs_texture_t *browser_tex,
					   uint32_t cx, uint32_t cy)
{
	if (!browser_tex)
		return;

	/* Extract the bottom row of the browser texture */
	gs_texrender_t *strip_tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!gs_texrender_begin(strip_tr, cx, 1)) {
		gs_texrender_destroy(strip_tr);
		return;
	}

	struct vec4 cc;
	vec4_zero(&cc);
	gs_clear(GS_CLEAR_COLOR, &cc, 0.0f, 0);

	gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *img = gs_effect_get_param_by_name(eff, "image");
	gs_effect_set_texture(img, browser_tex);

	gs_technique_t *tech = gs_effect_get_technique(eff, "Draw");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite_subregion(browser_tex, 0, 0, cy - 1, cx, 1);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
	gs_texrender_end(strip_tr);

	gs_texture_t *strip_tex = gs_texrender_get_texture(strip_tr);
	if (!strip_tex) {
		gs_texrender_destroy(strip_tr);
		return;
	}

	/* Synchronous readback of just 1 row */
	gs_stagesurf_t *ss = gs_stagesurface_create(cx, 1, GS_RGBA);
	if (ss) {
		gs_stage_texture(ss, strip_tex);
		uint8_t *data;
		uint32_t linesize;
		if (gs_stagesurface_map(ss, &data, &linesize)) {
			/* Check for magic marker at pixel 6: 0xCAFEBABE */
			if (cx >= 7) {
				uint8_t *magic = data + 6 * 4;
				if (magic[0] == 0xCA && magic[1] == 0xFE &&
				    magic[2] == 0xBA && magic[3] == 0xBE) {
					transform_decode_from_pixels(
						data, linesize, cx,
						&lt->transform_a,
						&lt->transform_b);
					lt->has_transforms = true;
				}
			}
			gs_stagesurface_unmap(ss);
		}
		gs_stagesurface_destroy(ss);
	}

	gs_texrender_destroy(strip_tr);
}

/* ------------------------------------------------------------------ */
/* Helper: render browser and extract regions                          */
/* ------------------------------------------------------------------ */

static gs_texrender_t *render_browser_full(struct lottie_transition *lt)
{
	if (!lt->browser)
		return NULL;

	uint32_t browser_h = lt->cy * BROWSER_REGIONS + DATA_STRIP_HEIGHT;

	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (gs_texrender_begin(tr, lt->cx, browser_h)) {
		struct vec4 cc;
		vec4_zero(&cc);
		gs_clear(GS_CLEAR_COLOR, &cc, 0.0f, 0);
		obs_source_video_render(lt->browser);
		gs_texrender_end(tr);
	}
	return tr;
}

static gs_texture_t *extract_browser_region(struct lottie_transition *lt,
					    gs_texrender_t *dest,
					    gs_texture_t *browser_tex,
					    uint32_t region_index)
{
	if (!browser_tex)
		return NULL;

	if (!gs_texrender_begin(dest, lt->cx, lt->cy))
		return NULL;

	struct vec4 cc;
	vec4_zero(&cc);
	gs_clear(GS_CLEAR_COLOR, &cc, 0.0f, 0);

	gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *img = gs_effect_get_param_by_name(eff, "image");
	gs_effect_set_texture(img, browser_tex);

	gs_technique_t *tech = gs_effect_get_technique(eff, "Draw");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	gs_draw_sprite_subregion(browser_tex, 0,
				 0, region_index * lt->cy,
				 lt->cx, lt->cy);

	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	gs_texrender_end(dest);

	return gs_texrender_get_texture(dest);
}

/* ------------------------------------------------------------------ */
/* OBS source_info callbacks                                           */
/* ------------------------------------------------------------------ */

static const char *lt_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("LottieTransition");
}

static void *lt_create(obs_data_t *settings, obs_source_t *source)
{
	blog(LOG_INFO, TAG "lt_create called");

	struct lottie_transition *lt = bzalloc(sizeof(*lt));
	lt->source = source;

	pthread_mutex_init(&lt->mutex, NULL);
	slot_transform_identity(&lt->transform_a);
	slot_transform_identity(&lt->transform_b);
	lt_e2e_init(lt);

	char *effect_path = obs_module_file("lottie_transition.effect");
	if (effect_path) {
		obs_enter_graphics();
		char *errors = NULL;
		lt->effect = gs_effect_create_from_file(effect_path, &errors);
		obs_leave_graphics();
		if (lt->effect) {
			blog(LOG_INFO, TAG "Loaded effect OK");
		} else {
			blog(LOG_ERROR, TAG "Failed to load effect: %s",
			     errors ? errors : "(null)");
		}
		bfree(errors);
		bfree(effect_path);
	}

	if (lt->effect) {
		lt->ep_scene_a = gs_effect_get_param_by_name(lt->effect, "scene_a");
		lt->ep_scene_b = gs_effect_get_param_by_name(lt->effect, "scene_b");
		lt->ep_browser_tex = gs_effect_get_param_by_name(lt->effect, "browser_tex");
		lt->ep_invert_matte = gs_effect_get_param_by_name(lt->effect, "invert_matte");
		lt->ep_has_matte_a = gs_effect_get_param_by_name(lt->effect, "has_matte_a");
		lt->ep_has_matte_b = gs_effect_get_param_by_name(lt->effect, "has_matte_b");
		lt->ep_scene_size = gs_effect_get_param_by_name(lt->effect, "scene_size");
		lt->ep_slot_a_pos_scale =
			gs_effect_get_param_by_name(lt->effect, "slot_a_pos_scale");
		lt->ep_slot_a_rot_opacity =
			gs_effect_get_param_by_name(lt->effect, "slot_a_rot_opacity");
		lt->ep_slot_b_pos_scale =
			gs_effect_get_param_by_name(lt->effect, "slot_b_pos_scale");
		lt->ep_slot_b_rot_opacity =
			gs_effect_get_param_by_name(lt->effect, "slot_b_rot_opacity");
	}

	lt->anim_total_frames = 30.0f;
	lt->anim_frame_rate = 30.0f;
	lt->requested_backend = LT_BACKEND_BROWSER;
	lt->effective_backend = lt_backend_resolve(lt->requested_backend);

	lt_update(lt, settings);
	lt_e2e_write_json_line(lt, "create", "\"source_created\":true");

	return lt;
}

static void lt_destroy(void *data)
{
	struct lottie_transition *lt = data;

	obs_enter_graphics();

	destroy_render_backend(lt);

	gs_texrender_destroy(lt->texrender_a);
	gs_texrender_destroy(lt->texrender_b);
	gs_stagesurface_destroy(lt->stagesurf);
	gs_effect_destroy(lt->effect);

	obs_leave_graphics();

	pthread_mutex_destroy(&lt->mutex);
	lt_e2e_write_json_line(lt, "destroy", "\"source_destroyed\":true");
	bfree(lt->e2e_prev_sample);
	bfree(lt->e2e_capture_dir);
	bfree(lt->lottie_file);
	bfree(lt);
}

static void lt_update(void *data, obs_data_t *settings)
{
	struct lottie_transition *lt = data;

	pthread_mutex_lock(&lt->mutex);

	const char *file = obs_data_get_string(settings, "lottie_file");
	bool file_changed = false;
	bool backend_changed = false;

	if (file && *file) {
		if (!lt->lottie_file || strcmp(lt->lottie_file, file) != 0) {
			bfree(lt->lottie_file);
			lt->lottie_file = bstrdup(file);
					file_changed = true;
		}
	}

	lt->invert_matte = obs_data_get_bool(settings, "invert_matte");

	enum lt_backend_type requested_backend =
		lt_backend_parse(obs_data_get_string(settings, "renderer_backend"));
	if (lt->requested_backend != requested_backend) {
		lt->requested_backend = requested_backend;
		backend_changed = true;
	}
	lt->effective_backend = lt_backend_resolve(lt->requested_backend);

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		lt->cx = ovi.base_width;
		lt->cy = ovi.base_height;
	} else {
		lt->cx = 1920;
		lt->cy = 1080;
	}

	pthread_mutex_unlock(&lt->mutex);

	if (lt_backend_is_fallback(lt->requested_backend, lt->effective_backend)) {
		blog(LOG_WARNING, TAG "Requested backend '%s' is not available in this "
		     "build, falling back to '%s'",
		     lt_backend_name(lt->requested_backend),
		     lt_backend_name(lt->effective_backend));
	}

	blog(LOG_INFO, TAG "lt_update: file_changed=%d backend_changed=%d "
	     "backend=%s requested=%s file='%s' cx=%u cy=%u browser=%p",
	     (int)file_changed, (int)backend_changed,
	     lt_backend_name(lt->effective_backend),
	     lt_backend_name(lt->requested_backend),
	     lt->lottie_file ? lt->lottie_file : "(null)",
	     lt->cx, lt->cy, (void *)lt->browser);

	if (file_changed || backend_changed) {
		lt_detect_matte_layers(lt);
		destroy_render_backend(lt);
		create_render_backend(lt);
	}

	struct dstr extra = {0};
	dstr_catf(&extra,
		  "\"file_changed\":%s,\"backend_changed\":%s,"
		  "\"cx\":%u,\"cy\":%u,\"browser_active\":%s",
		  file_changed ? "true" : "false",
		  backend_changed ? "true" : "false",
		  lt->cx, lt->cy, lt->browser ? "true" : "false");
	lt_e2e_write_json_line(lt, "update", extra.array);
	dstr_free(&extra);
}

static obs_properties_t *lt_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, "lottie_file",
				obs_module_text("LottieFile"),
				OBS_PATH_FILE,
				"Lottie JSON (*.json)", NULL);

	obs_properties_add_bool(props, "invert_matte",
				obs_module_text("InvertMatte"));

	obs_property_t *backend = obs_properties_add_list(
		props, "renderer_backend",
		"Renderer Backend",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(backend, "ThorVG Native", "thorvg");
	obs_property_list_add_string(backend, "Browser (CEF / lottie-web)", "browser");

	return props;
}

static void lt_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "lottie_file", "");
	obs_data_set_default_string(settings, "renderer_backend", "thorvg");
	obs_data_set_default_bool(settings, "invert_matte", false);
}

/* ------------------------------------------------------------------ */
/* Transition callbacks                                                */
/* ------------------------------------------------------------------ */

static void lt_transition_start(void *data)
{
	struct lottie_transition *lt = data;
	blog(LOG_INFO, TAG "=== TRANSITION START ===");

	pthread_mutex_lock(&lt->mutex);
	lt->active = true;
	lt->progress = 0.0f;
	lt->tick_count = 0;
	lt->render_count = 0;
	lt->has_transforms = false;
	slot_transform_identity(&lt->transform_a);
	slot_transform_identity(&lt->transform_b);
	lt->e2e_transition_index++;
	lt_e2e_reset_transition_state(lt);
	pthread_mutex_unlock(&lt->mutex);

	lt_e2e_write_json_line(lt, "transition_start", "\"active\":true");

	/* CEF needs a full recreate to guarantee a fresh page load.
	 * Native backends are progress-driven and should not be restarted here. */
	if (lt_backend_recreate_on_transition_start(lt->effective_backend)) {
		obs_enter_graphics();
		destroy_render_backend(lt);
		obs_leave_graphics();
		create_render_backend(lt);
		blog(LOG_INFO, TAG "Recreated backend for transition (%s)",
		     lt_backend_name(lt->effective_backend));
	}
}

static void lt_transition_stop(void *data)
{
	struct lottie_transition *lt = data;
	blog(LOG_INFO, TAG "=== TRANSITION STOP === (ticks=%d renders=%d)",
	     lt->tick_count, lt->render_count);

	pthread_mutex_lock(&lt->mutex);
	lt->active = false;
	lt->progress = 0.0f;
	lt->has_transforms = false;
	slot_transform_identity(&lt->transform_a);
	slot_transform_identity(&lt->transform_b);
	pthread_mutex_unlock(&lt->mutex);
	lt_e2e_write_json_line(lt, "transition_stop", "\"active\":false");

	/* exec_browser_js doesn't work for private sources, animation is self-driving */
}

static void lt_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct lottie_transition *lt = data;

	if (!lt->active)
		return;

	lt->tick_count++;
	lt->progress = obs_transition_get_time(lt->source);

	if (lt->tick_count <= 3) {
		if (lt->effective_backend == LT_BACKEND_BROWSER) {
			blog(LOG_INFO, TAG "tick #%d  t=%.3f  browser=%ux%u",
			     lt->tick_count, lt->progress,
			     obs_source_get_width(lt->browser),
			     obs_source_get_height(lt->browser));
		} else {
			blog(LOG_INFO, TAG "tick #%d  t=%.3f  backend=%s",
			     lt->tick_count, lt->progress,
			     lt_backend_name(lt->effective_backend));
		}
	}

	/* Animation is self-driving in bridge.js via requestAnimationFrame */

	/* Data strip / slot transforms disabled for now — channel-packing
	 * approach puts everything in a single 1080p texture. */
}

/* ------------------------------------------------------------------ */
/* Video render                                                        */
/* ------------------------------------------------------------------ */

static void lt_transition_video_callback(void *data, gs_texture_t *a,
					 gs_texture_t *b, float t,
					 uint32_t cx, uint32_t cy)
{
	struct lottie_transition *lt = data;
	lt->render_count++;

	if (lt->effective_backend == LT_BACKEND_THORVG) {
		gs_texture_t *thorvg_texture = lt_thorvg_render(lt->thorvg_backend, t);
		if (!thorvg_texture || !a || !b || !lt->effect) {
			if (lt->render_count <= 3)
				blog(LOG_INFO, TAG "render #%d: NO THORVG/EFFECT", lt->render_count);
			return;
		}

		if (lt_thorvg_get_slot_transforms(lt->thorvg_backend, &lt->transform_a,
						  &lt->transform_b)) {
			lt->has_transforms = true;
		} else {
			lt->has_transforms = false;
			slot_transform_identity(&lt->transform_a);
			slot_transform_identity(&lt->transform_b);
		}

		render_and_optionally_capture_composite(lt, a, b,
							thorvg_texture, cx, cy, t);
		return;
	}

	if (!lt->browser || !lt->effect) {
		if (lt->render_count <= 3)
			blog(LOG_INFO, TAG "render #%d: NO BROWSER/EFFECT", lt->render_count);
		return;
	}

	uint32_t bw = obs_source_get_width(lt->browser);
	uint32_t bh = obs_source_get_height(lt->browser);

	if (lt->render_count <= 10) {
		blog(LOG_INFO, TAG "render #%d  t=%.3f  cx=%u cy=%u  bw=%u bh=%u",
		     lt->render_count, t, cx, cy, bw, bh);
	}

	/* Render browser source into a texrender */
	gs_texrender_t *browser_tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (gs_texrender_begin(browser_tr, cx, cy)) {
		struct vec4 cc;
		vec4_zero(&cc);
		gs_clear(GS_CLEAR_COLOR, &cc, 0.0f, 0);
		obs_source_video_render(lt->browser);
		gs_texrender_end(browser_tr);
	}

	gs_texture_t *browser_texture = gs_texrender_get_texture(browser_tr);

	/* Diagnostic readback — log channel-packed values throughout transition */
	if (browser_texture && lt->render_count <= 50) {
		gs_stagesurf_t *ss = gs_stagesurface_create(cx, cy, GS_RGBA);
		if (ss) {
			gs_stage_texture(ss, browser_texture);
			uint8_t *sdata;
			uint32_t slinesize;
			if (gs_stagesurface_map(ss, &sdata, &slinesize)) {
				/* Sample at 25% from edge and center */
				uint32_t eidx = (cx/4)*4 + (cy/4)*slinesize;
				uint32_t cidx = (cx/2)*4 + (cy/2)*slinesize;
				blog(LOG_INFO, TAG "render #%d t=%.3f: "
				     "edge(25%%): R=%u G=%u B=%u  "
				     "center: R=%u G=%u B=%u",
				     lt->render_count, t,
				     sdata[eidx], sdata[eidx+1], sdata[eidx+2],
				     sdata[cidx], sdata[cidx+1], sdata[cidx+2]);
				gs_stagesurface_unmap(ss);
			}
			gs_stagesurface_destroy(ss);
		}
	}

	if (!browser_texture || !a || !b) {
		gs_texrender_destroy(browser_tr);
		return;
	}

	/* Decode slot transforms from bottom row of browser texture */
	decode_transforms_from_texture(lt, browser_texture, cx, cy);

	if (lt->render_count <= 5 && lt->has_transforms) {
		blog(LOG_INFO, TAG "transforms: A pos=(%.0f,%.0f) scale=(%.2f,%.2f) "
		     "B pos=(%.0f,%.0f) scale=(%.2f,%.2f)",
		     lt->transform_a.pos_x, lt->transform_a.pos_y,
		     lt->transform_a.scale_x, lt->transform_a.scale_y,
		     lt->transform_b.pos_x, lt->transform_b.pos_y,
		     lt->transform_b.scale_x, lt->transform_b.scale_y);
	}

	/* Composite using shader:
	 * browser_tex: R=matteA, G=matteB, B=unused, A=always 1 */
	render_and_optionally_capture_composite(lt, a, b,
						browser_texture, cx, cy, t);

	gs_texrender_destroy(browser_tr);
}

static void lt_video_render(void *data, gs_effect_t *effect)
{
	struct lottie_transition *lt = data;
	UNUSED_PARAMETER(effect);
	obs_transition_video_render(lt->source, lt_transition_video_callback);
}

/* ------------------------------------------------------------------ */
/* Audio render — simple crossfade                                     */
/* ------------------------------------------------------------------ */

static float lt_audio_mix_a(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return 1.0f - t;
}

static float lt_audio_mix_b(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return t;
}

static bool lt_audio_render(void *data, uint64_t *ts_out,
			    struct obs_source_audio_mix *audio,
			    uint32_t mixers, size_t channels,
			    size_t sample_rate)
{
	struct lottie_transition *lt = data;
	return obs_transition_audio_render(lt->source, ts_out, audio, mixers,
					   channels, sample_rate,
					   lt_audio_mix_a, lt_audio_mix_b);
}

/* ------------------------------------------------------------------ */
/* OBS source info registration                                        */
/* ------------------------------------------------------------------ */

static struct obs_source_info lottie_transition_info = {
	.id = "lottie_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = lt_get_name,
	.create = lt_create,
	.destroy = lt_destroy,
	.update = lt_update,
	.get_properties = lt_get_properties,
	.get_defaults = lt_get_defaults,
	.video_render = lt_video_render,
	.video_tick = lt_video_tick,
	.audio_render = lt_audio_render,
	.transition_start = lt_transition_start,
	.transition_stop = lt_transition_stop,
};

bool obs_module_load(void)
{
	blog(LOG_INFO, TAG "Plugin loaded");
	obs_register_source(&lottie_transition_info);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, TAG "Plugin unloaded");
}
