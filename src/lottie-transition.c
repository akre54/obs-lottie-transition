#include "lottie-transition.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/base.h>

OBS_DECLARE_MODULE()

/* Minimal base64 encoder */
static const char b64_table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(struct dstr *out, const uint8_t *data, size_t len)
{
	size_t out_len = 4 * ((len + 2) / 3);
	dstr_ensure_capacity(out, out->len + out_len + 1);

	size_t i;
	for (i = 0; i + 2 < len; i += 3) {
		uint32_t v = ((uint32_t)data[i] << 16) |
			     ((uint32_t)data[i+1] << 8) |
			     (uint32_t)data[i+2];
		dstr_catf(out, "%c%c%c%c",
			  b64_table[(v >> 18) & 0x3F],
			  b64_table[(v >> 12) & 0x3F],
			  b64_table[(v >> 6) & 0x3F],
			  b64_table[v & 0x3F]);
	}
	if (i < len) {
		uint32_t v = (uint32_t)data[i] << 16;
		if (i + 1 < len) v |= (uint32_t)data[i+1] << 8;
		dstr_catf(out, "%c%c", b64_table[(v >> 18) & 0x3F],
			  b64_table[(v >> 12) & 0x3F]);
		if (i + 1 < len)
			dstr_catf(out, "%c=", b64_table[(v >> 6) & 0x3F]);
		else
			dstr_cat(out, "==");
	}
}
OBS_MODULE_USE_DEFAULT_LOCALE("obs-lottie-transition", "en-US")

#define TAG "[lottie-transition] "

static void lt_update(void *data, obs_data_t *settings);

/* ------------------------------------------------------------------ */
/* Helper: build browser URL via temp file in /tmp                     */
/* ------------------------------------------------------------------ */

static void build_browser_url(struct dstr *url, struct lottie_transition *lt)
{
	/*
	 * CONFIRMED: data:text/html works for private browser sources,
	 * even at 400KB+. file:// and exec_browser_js do NOT work.
	 *
	 * Test: Does percent-encoding inside <script> work?
	 * Use a simple script with && (encoded as %26%26).
	 */
	/*
	 * CONFIRMED: data:text/html works with raw unencoded content.
	 * No percent-encoding needed — CEF handles &&, #, % etc. fine.
	 * Only need to escape </ as <\/ inside <script> tags.
	 */

	/* Read lottie.min.js */
	char *lottie_path = obs_module_file("web/lottie.min.js");
	char *lottie_js = lottie_path ? os_quick_read_utf8_file(lottie_path) : NULL;
	bfree(lottie_path);
	if (!lottie_js) {
		blog(LOG_ERROR, TAG "Failed to read lottie.min.js");
		return;
	}

	/* Read bridge.js */
	char *bridge_path = obs_module_file("web/bridge.js");
	char *bridge_js = bridge_path ? os_quick_read_utf8_file(bridge_path) : NULL;
	bfree(bridge_path);
	if (!bridge_js) {
		blog(LOG_ERROR, TAG "Failed to read bridge.js");
		bfree(lottie_js);
		return;
	}

	/* Escape </ as <\/ to prevent premature </script> closure */
	struct dstr lottie_escaped = {0};
	struct dstr bridge_escaped = {0};
	for (const char *p = lottie_js; *p; p++) {
		if (p[0] == '<' && p[1] == '/')
			{ dstr_cat(&lottie_escaped, "<\\/"); p++; }
		else
			dstr_catf(&lottie_escaped, "%c", *p);
	}
	bfree(lottie_js);

	for (const char *p = bridge_js; *p; p++) {
		if (p[0] == '<' && p[1] == '/')
			{ dstr_cat(&bridge_escaped, "<\\/"); p++; }
		else
			dstr_catf(&bridge_escaped, "%c", *p);
	}
	bfree(bridge_js);

	/* Read animation data */
	char *anim_json = NULL;
	if (lt->lottie_file && *lt->lottie_file)
		anim_json = os_quick_read_utf8_file(lt->lottie_file);

	/* Build HTML first */
	struct dstr html = {0};
	dstr_cat(&html,
		"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
		"<style>*{margin:0;padding:0;overflow:hidden;background:#000}"
		"canvas{display:block}</style></head><body>"
		"<canvas id='lottie-canvas'></canvas>");

	dstr_catf(&html,
		"<script>window._obsConfig={width:%u,height:%u,dataStripHeight:%d};</script>",
		lt->cx, lt->cy, DATA_STRIP_HEIGHT);

	dstr_catf(&html, "<script>%s</script>", lottie_escaped.array);

	if (anim_json)
		dstr_catf(&html, "<script>window._lottieData=%s;</script>", anim_json);

	dstr_catf(&html, "<script>%s</script>", bridge_escaped.array);
	dstr_cat(&html, "</body></html>");

	blog(LOG_INFO, TAG "HTML size: %lu bytes", (unsigned long)html.len);

	/* Write HTML to /tmp for debugging in regular browser */
	os_quick_write_utf8_file("/tmp/obs-lottie-debug.html",
				 html.array, html.len, false);
	blog(LOG_INFO, TAG "Wrote debug HTML to /tmp/obs-lottie-debug.html");

	/* Count </script in the HTML - should be exactly 4 (or 5 with anim) */
	int script_close_count = 0;
	for (const char *s = html.array; (s = strstr(s, "</script")); s++)
		script_close_count++;
	blog(LOG_INFO, TAG "Found %d </script tags in HTML", script_close_count);

	/* Check for unescaped </ inside script content */
	int slash_tag_count = 0;
	for (const char *s = html.array; (s = strstr(s, "</")); s++)
		slash_tag_count++;
	blog(LOG_INFO, TAG "Found %d </ sequences total in HTML", slash_tag_count);

	/* Use base64 encoding to avoid ALL character escaping issues */
	dstr_cat(url, "data:text/html;base64,");
	base64_encode(url, (const uint8_t *)html.array, html.len);

	blog(LOG_INFO, TAG "Data URL size: %lu bytes (lottie=%lu bridge=%lu anim=%s)",
	     (unsigned long)url->len,
	     (unsigned long)lottie_escaped.len,
	     (unsigned long)bridge_escaped.len,
	     anim_json ? "yes" : "no");

	dstr_free(&html);

	dstr_free(&lottie_escaped);
	dstr_free(&bridge_escaped);
	bfree(anim_json);
}

/* ------------------------------------------------------------------ */
/* Helper: create/destroy browser source                               */
/* ------------------------------------------------------------------ */

static void create_browser_source(struct lottie_transition *lt)
{
	if (lt->browser)
		return;

	struct dstr url = {0};
	build_browser_url(&url, lt);

	if (!url.array || !*url.array) {
		blog(LOG_ERROR, TAG "Failed to build browser URL");
		dstr_free(&url);
		return;
	}

	uint32_t browser_height = lt->cy * BROWSER_REGIONS + DATA_STRIP_HEIGHT;

	obs_data_t *browser_settings = obs_data_create();
	obs_data_set_string(browser_settings, "url", url.array);
	obs_data_set_int(browser_settings, "width", lt->cx);
	obs_data_set_int(browser_settings, "height", browser_height);
	obs_data_set_int(browser_settings, "fps", 60);
	obs_data_set_bool(browser_settings, "shutdown", false);
	obs_data_set_bool(browser_settings, "restart_when_active", false);

	blog(LOG_INFO, TAG "Creating browser: size=%ux%u",
	     lt->cx, browser_height);

	lt->browser = obs_source_create_private("browser_source",
						"lottie_transition_browser",
						browser_settings);

	obs_data_release(browser_settings);
	dstr_free(&url);

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
/* Helper: stage data strip from browser texture for next-frame read   */
/* ------------------------------------------------------------------ */

static void stage_data_strip(struct lottie_transition *lt)
{
	if (!lt->browser || !lt->stagesurf)
		return;

	uint32_t browser_height = lt->cy * BROWSER_REGIONS + DATA_STRIP_HEIGHT;
	uint32_t strip_y = lt->cy * BROWSER_REGIONS;

	gs_texrender_t *browser_tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (gs_texrender_begin(browser_tr, lt->cx, browser_height)) {
		struct vec4 cc;
		vec4_zero(&cc);
		gs_clear(GS_CLEAR_COLOR, &cc, 0.0f, 0);
		obs_source_video_render(lt->browser);
		gs_texrender_end(browser_tr);
	}

	gs_texture_t *browser_tex = gs_texrender_get_texture(browser_tr);
	if (!browser_tex) {
		gs_texrender_destroy(browser_tr);
		return;
	}

	gs_texrender_t *strip_tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (gs_texrender_begin(strip_tr, lt->cx, DATA_STRIP_HEIGHT)) {
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
					 0, strip_y,
					 lt->cx, DATA_STRIP_HEIGHT);

		gs_technique_end_pass(tech);
		gs_technique_end(tech);
		gs_texrender_end(strip_tr);
	}

	gs_texture_t *strip_tex = gs_texrender_get_texture(strip_tr);
	if (strip_tex) {
		gs_stage_texture(lt->stagesurf, strip_tex);
		lt->stage_ready = true;
	}

	gs_texrender_destroy(strip_tr);
	gs_texrender_destroy(browser_tr);
}

/* ------------------------------------------------------------------ */
/* Helper: render a scene with transforms into a texrender             */
/* ------------------------------------------------------------------ */

static void render_scene_transformed(gs_texrender_t *tr,
				     obs_source_t *transition,
				     bool scene_b,
				     const struct slot_transform *xform,
				     uint32_t cx, uint32_t cy)
{
	if (!gs_texrender_begin(tr, cx, cy))
		return;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	float center_x = (float)cx * 0.5f;
	float center_y = (float)cy * 0.5f;

	gs_matrix_push();
	gs_matrix_identity();

	float offset_x = xform->pos_x - center_x;
	float offset_y = xform->pos_y - center_y;

	gs_matrix_translate3f(center_x + offset_x, center_y + offset_y, 0.0f);
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f,
			  RAD(xform->rotation));
	gs_matrix_scale3f(xform->scale_x, xform->scale_y, 1.0f);
	gs_matrix_translate3f(-center_x, -center_y, 0.0f);

	if (scene_b)
		obs_transition_video_render_direct(transition,
						   OBS_TRANSITION_SOURCE_B);
	else
		obs_transition_video_render_direct(transition,
						   OBS_TRANSITION_SOURCE_A);

	gs_matrix_pop();
	gs_texrender_end(tr);
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
		lt->ep_matte_a = gs_effect_get_param_by_name(lt->effect, "matte_a");
		lt->ep_matte_b = gs_effect_get_param_by_name(lt->effect, "matte_b");
		lt->ep_overlay = gs_effect_get_param_by_name(lt->effect, "overlay");
		lt->ep_invert_matte = gs_effect_get_param_by_name(lt->effect, "invert_matte");
	}

	lt->anim_total_frames = 30.0f;
	lt->anim_frame_rate = 30.0f;

	lt_update(lt, settings);

	return lt;
}

static void lt_destroy(void *data)
{
	struct lottie_transition *lt = data;

	obs_enter_graphics();

	destroy_browser_source(lt);

	gs_texrender_destroy(lt->texrender_a);
	gs_texrender_destroy(lt->texrender_b);
	gs_stagesurface_destroy(lt->stagesurf);
	gs_effect_destroy(lt->effect);

	obs_leave_graphics();

	pthread_mutex_destroy(&lt->mutex);
	bfree(lt->lottie_file);
	bfree(lt);
}

static void lt_update(void *data, obs_data_t *settings)
{
	struct lottie_transition *lt = data;

	pthread_mutex_lock(&lt->mutex);

	const char *file = obs_data_get_string(settings, "lottie_file");
	bool file_changed = false;

	if (file && *file) {
		if (!lt->lottie_file || strcmp(lt->lottie_file, file) != 0) {
			bfree(lt->lottie_file);
			lt->lottie_file = bstrdup(file);
					file_changed = true;
		}
	}

	lt->invert_matte = obs_data_get_bool(settings, "invert_matte");

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		lt->cx = ovi.base_width;
		lt->cy = ovi.base_height;
	} else {
		lt->cx = 1920;
		lt->cy = 1080;
	}

	pthread_mutex_unlock(&lt->mutex);

	blog(LOG_INFO, TAG "lt_update: file_changed=%d file='%s' cx=%u cy=%u browser=%p",
	     (int)file_changed, lt->lottie_file ? lt->lottie_file : "(null)",
	     lt->cx, lt->cy, (void *)lt->browser);

	if (file_changed) {
		destroy_browser_source(lt);
		create_browser_source(lt);
	}
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

	return props;
}

static void lt_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "lottie_file", "");
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
	pthread_mutex_unlock(&lt->mutex);

	if (!lt->browser)
		create_browser_source(lt);
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
		blog(LOG_INFO, TAG "tick #%d  t=%.3f  browser=%ux%u",
		     lt->tick_count, lt->progress,
		     obs_source_get_width(lt->browser),
		     obs_source_get_height(lt->browser));
	}

	/* Animation is self-driving in bridge.js via requestAnimationFrame */

	obs_enter_graphics();
	ensure_stagesurf(lt);
	read_data_strip(lt);
	stage_data_strip(lt);
	obs_leave_graphics();
}

/* ------------------------------------------------------------------ */
/* Video render                                                        */
/* ------------------------------------------------------------------ */

static void lt_transition_video_callback(void *data, gs_texture_t *a,
					 gs_texture_t *b, float t,
					 uint32_t cx, uint32_t cy)
{
	struct lottie_transition *lt = data;

	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);

	lt->render_count++;

	if (!lt->browser) {
		if (lt->render_count <= 3)
			blog(LOG_INFO, TAG "render #%d: NO BROWSER", lt->render_count);
		return;
	}

	uint32_t bw = obs_source_get_width(lt->browser);
	uint32_t bh = obs_source_get_height(lt->browser);

	if (lt->render_count <= 5) {
		blog(LOG_INFO, TAG "render #%d  t=%.3f  cx=%u cy=%u  "
		     "browser=%p bw=%u bh=%u",
		     lt->render_count, t, cx, cy,
		     (void *)lt->browser, bw, bh);
	}

	/* Render browser into a texrender */
	uint32_t rw = bw ? bw : cx;
	uint32_t rh = bh ? bh : cy;

	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	bool tr_ok = false;
	if (gs_texrender_begin(tr, rw, rh)) {
		struct vec4 cc;
		vec4_zero(&cc);
		gs_clear(GS_CLEAR_COLOR, &cc, 0.0f, 0);
		obs_source_video_render(lt->browser);
		gs_texrender_end(tr);
		tr_ok = true;
	}

	gs_texture_t *tex = gs_texrender_get_texture(tr);

	if (lt->render_count <= 5) {
		blog(LOG_INFO, TAG "render #%d: texrender_ok=%d tex=%p",
		     lt->render_count, (int)tr_ok, (void *)tex);
	}

	/* Read back a few pixels to check if browser rendered anything */
	if (tex && lt->render_count <= 5) {
		gs_stagesurf_t *ss = gs_stagesurface_create(rw, rh, GS_RGBA);
		if (ss) {
			gs_stage_texture(ss, tex);
			uint8_t *sdata;
			uint32_t slinesize;
			if (gs_stagesurface_map(ss, &sdata, &slinesize)) {
				/* Read pixel at (10,10) and (cx/2, cy/2) */
				uint32_t p1_off = 10 * 4 + 10 * slinesize;
				uint32_t mid_x = rw / 2;
				uint32_t mid_y = rh / 2;
				uint32_t p2_off = mid_x * 4 + mid_y * slinesize;
				blog(LOG_INFO, TAG "render #%d: pixel(10,10)="
				     "RGBA(%u,%u,%u,%u) pixel(%u,%u)="
				     "RGBA(%u,%u,%u,%u)",
				     lt->render_count,
				     sdata[p1_off+0], sdata[p1_off+1],
				     sdata[p1_off+2], sdata[p1_off+3],
				     mid_x, mid_y,
				     sdata[p2_off+0], sdata[p2_off+1],
				     sdata[p2_off+2], sdata[p2_off+3]);
				gs_stagesurface_unmap(ss);
			} else {
				blog(LOG_INFO, TAG "render #%d: stagesurf map FAILED",
				     lt->render_count);
			}
			gs_stagesurface_destroy(ss);
		}
	}

	/* Draw browser texture to output */
	if (tex) {
		gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *img = gs_effect_get_param_by_name(eff, "image");
		gs_effect_set_texture(img, tex);

		gs_technique_t *tech = gs_effect_get_technique(eff, "Draw");
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);
		gs_draw_sprite(tex, 0, cx, cy);
		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	}

	gs_texrender_destroy(tr);
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
