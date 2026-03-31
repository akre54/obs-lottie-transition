#include "lottie-transition.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/base.h>

OBS_DECLARE_MODULE()

/* Temp file path for browser HTML */
#define HTML_TEMP_PATH "/tmp/obs-lottie-transition.html"
OBS_MODULE_USE_DEFAULT_LOCALE("obs-lottie-transition", "en-US")

#define TAG "[lottie-transition] "

static void lt_update(void *data, obs_data_t *settings);

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

	/* Read bridge-core.js and bridge.js for inlining */
	char *bridge_core_path = obs_module_file("web/bridge-core.js");
	char *bridge_core_js = bridge_core_path ?
		os_quick_read_utf8_file(bridge_core_path) : NULL;
	bfree(bridge_core_path);
	if (!bridge_core_js) {
		blog(LOG_ERROR, TAG "Failed to read bridge-core.js");
		return false;
	}

	char *bridge_path = obs_module_file("web/bridge.js");
	char *bridge_js = bridge_path ? os_quick_read_utf8_file(bridge_path) : NULL;
	bfree(bridge_path);
	if (!bridge_js) {
		bfree(bridge_core_js);
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
	for (const char *p = bridge_js; *p; p++) {
		if (p[0] == '<' && p[1] == '/')
			{ dstr_cat(&bridge_escaped, "<\\/"); p++; }
		else
			dstr_catf(&bridge_escaped, "%c", *p);
	}
	bfree(bridge_core_js);
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
		lt->ep_browser_tex = gs_effect_get_param_by_name(lt->effect, "browser_tex");
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

	/* Destroy and recreate browser to get a fresh page load.
	 * The restart_when_active + active toggle doesn't reliably reload
	 * the page on subsequent transitions. Full recreate costs ~80ms
	 * of CEF cold-start (frames 1-4 are transparent) but is reliable. */
	obs_enter_graphics();
	destroy_browser_source(lt);
	obs_leave_graphics();
	create_browser_source(lt);
	blog(LOG_INFO, TAG "Recreated browser for transition");
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
	bool tr_ok = false;
	if (gs_texrender_begin(browser_tr, cx, cy)) {
		struct vec4 cc;
		vec4_zero(&cc);
		gs_clear(GS_CLEAR_COLOR, &cc, 0.0f, 0);
		obs_source_video_render(lt->browser);
		gs_texrender_end(browser_tr);
		tr_ok = true;
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

	/* If slot transforms exist, render scenes with transforms applied */
	gs_texture_t *tex_a = a;
	gs_texture_t *tex_b = b;

	if (lt->has_transforms) {
		if (!lt->texrender_a)
			lt->texrender_a = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!lt->texrender_b)
			lt->texrender_b = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

		render_scene_transformed(lt->texrender_a, lt->source, false,
					 &lt->transform_a, cx, cy);
		render_scene_transformed(lt->texrender_b, lt->source, true,
					 &lt->transform_b, cx, cy);

		gs_texture_t *ta = gs_texrender_get_texture(lt->texrender_a);
		gs_texture_t *tb = gs_texrender_get_texture(lt->texrender_b);
		if (ta) tex_a = ta;
		if (tb) tex_b = tb;

		if (lt->render_count <= 5) {
			blog(LOG_INFO, TAG "transforms: A pos=(%.0f,%.0f) scale=(%.2f,%.2f) "
			     "B pos=(%.0f,%.0f) scale=(%.2f,%.2f)",
			     lt->transform_a.pos_x, lt->transform_a.pos_y,
			     lt->transform_a.scale_x, lt->transform_a.scale_y,
			     lt->transform_b.pos_x, lt->transform_b.pos_y,
			     lt->transform_b.scale_x, lt->transform_b.scale_y);
		}
	}

	/* Composite using shader:
	 * browser_tex: R=matteA, G=matteB, B=unused, A=always 1 */
	gs_effect_set_texture(lt->ep_scene_a, tex_a);
	gs_effect_set_texture(lt->ep_scene_b, tex_b);
	gs_effect_set_texture(lt->ep_browser_tex, browser_texture);
	gs_effect_set_bool(lt->ep_invert_matte, lt->invert_matte);

	const char *tech_name = "MatteComposite";
	gs_technique_t *tech = gs_effect_get_technique(lt->effect, tech_name);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(browser_texture, 0, cx, cy);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);

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
