#include "lottie-thorvg.h"
#include "lottie-backend.h"
#include "lottie-slot-eval.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <obs-data.h>
#include <util/base.h>
}

#include <thorvg.h>

struct thorvg_pass {
	tvg::Animation *animation = nullptr;
	tvg::SwCanvas *canvas = nullptr;
	std::vector<uint32_t> pixels;
	bool loaded = false;
};

struct lt_thorvg {
	uint32_t width = 0;
	uint32_t height = 0;
	gs_texture_t *texture = nullptr;
	std::vector<uint8_t> rgba;
	lt_slot_set slots;
	slot_transform slot_a;
	slot_transform slot_b;
	bool has_slots = false;
	thorvg_pass matte_a;
	thorvg_pass matte_b;
	thorvg_pass overlay;
};

static std::mutex g_thorvg_mutex;
static uint32_t g_thorvg_refs = 0;

static bool name_equals(const char *a, const char *b)
{
	return a && b && strcmp(a, b) == 0;
}

static bool is_reserved_layer(const char *name)
{
	return name_equals(name, "[MatteA]") || name_equals(name, "[MatteB]") ||
	       name_equals(name, "[SlotA]") || name_equals(name, "[SlotB]");
}

static obs_data_t *clone_json_data(obs_data_t *src)
{
	const char *json = obs_data_get_json(src);
	return json ? obs_data_create_from_json(json) : nullptr;
}

static obs_data_t *filter_json_layers(obs_data_t *src, enum lt_backend_type backend,
				      const char *pass_name)
{
	obs_data_t *copy = clone_json_data(src);
	if (!copy)
		return nullptr;

	obs_data_array_t *layers = obs_data_get_array(src, "layers");
	if (!layers)
		return copy;

	obs_data_array_t *filtered = obs_data_array_create();
	const size_t count = obs_data_array_count(layers);

	for (size_t i = 0; i < count; i++) {
		obs_data_t *layer = obs_data_array_item(layers, i);
		const char *name = obs_data_get_string(layer, "nm");
		bool keep = false;

		if (backend == LT_BACKEND_BROWSER) {
			if (strcmp(pass_name, "matteA") == 0)
				keep = name_equals(name, "[MatteA]") || name_equals(name, "[SlotA]") ||
				       name_equals(name, "[SlotB]");
			else if (strcmp(pass_name, "matteB") == 0)
				keep = name_equals(name, "[MatteB]");
			else if (strcmp(pass_name, "overlay") == 0)
				keep = !is_reserved_layer(name);
		} else {
			if (strcmp(pass_name, "matteA") == 0)
				keep = name_equals(name, "[MatteA]") ||
				       name_equals(name, "[SlotA]") ||
				       name_equals(name, "[SlotB]");
			else if (strcmp(pass_name, "matteB") == 0)
				keep = name_equals(name, "[MatteB]");
			else if (strcmp(pass_name, "overlay") == 0)
				keep = !is_reserved_layer(name);
		}

		if (keep)
			obs_data_array_push_back(filtered, layer);

		obs_data_release(layer);
	}

	obs_data_set_array(copy, "layers", filtered);
	obs_data_array_release(filtered);
	obs_data_array_release(layers);
	return copy;
}

static std::string dirname_from_path(const char *path)
{
	if (!path || !*path)
		return std::string();

	const char *slash = strrchr(path, '/');
	if (!slash)
		return std::string();

	return std::string(path, (size_t)(slash - path));
}

static bool init_thorvg_runtime(void)
{
	std::lock_guard<std::mutex> lock(g_thorvg_mutex);
	if (g_thorvg_refs == 0) {
		if (tvg::Initializer::init(0) != tvg::Result::Success)
			return false;
	}
	g_thorvg_refs++;
	return true;
}

static void term_thorvg_runtime(void)
{
	std::lock_guard<std::mutex> lock(g_thorvg_mutex);
	if (g_thorvg_refs == 0)
		return;
	g_thorvg_refs--;
	if (g_thorvg_refs == 0)
		tvg::Initializer::term();
}

static bool load_pass(thorvg_pass &pass, obs_data_t *json, const std::string &rpath,
		      uint32_t width, uint32_t height)
{
	if (!json)
		return false;

	const char *text = obs_data_get_json(json);
	if (!text || !*text)
		return false;

	pass.animation = tvg::Animation::gen();
	if (!pass.animation)
		return false;

	tvg::Picture *picture = pass.animation->picture();
	if (!picture)
		return false;

	const uint32_t size = (uint32_t)strlen(text);
	if (picture->load(text, size, "lottie+json", rpath.empty() ? nullptr : rpath.c_str(), true) !=
	    tvg::Result::Success)
		return false;

	picture->size((float)width, (float)height);

	pass.canvas = tvg::SwCanvas::gen();
	if (!pass.canvas)
		return false;

	pass.pixels.assign((size_t)width * (size_t)height, 0);
	if (pass.canvas->target(pass.pixels.data(), width, width, height,
				tvg::ColorSpace::ARGB8888) != tvg::Result::Success)
		return false;

	if (pass.canvas->add(picture) != tvg::Result::Success)
		return false;

	pass.loaded = true;
	return true;
}

static void destroy_pass(thorvg_pass &pass)
{
	if (pass.canvas) {
		pass.canvas->sync();
		delete pass.canvas;
		pass.canvas = nullptr;
	}

	if (pass.animation) {
		delete pass.animation;
		pass.animation = nullptr;
	}

	pass.pixels.clear();
	pass.loaded = false;
}

static void render_pass(thorvg_pass &pass, float progress)
{
	if (!pass.loaded)
		return;

	const float total = std::max(pass.animation->totalFrame(), 1.0f);
	float frame = progress * (total - 1.0f);
	frame = std::min(std::max(frame, 0.0f), std::max(total - 1.0f, 0.0f));

	pass.animation->frame(frame);
	pass.canvas->update();
	pass.canvas->draw(true);
	pass.canvas->sync();
}

static inline uint8_t pixel_a(uint32_t px)
{
	return (uint8_t)((px >> 24) & 0xFF);
}

static inline uint8_t pixel_r(uint32_t px)
{
	return (uint8_t)((px >> 16) & 0xFF);
}

static inline uint8_t pixel_g(uint32_t px)
{
	return (uint8_t)((px >> 8) & 0xFF);
}

static inline uint8_t pixel_b(uint32_t px)
{
	return (uint8_t)(px & 0xFF);
}

static inline uint8_t luma_from_argb(uint32_t px)
{
	const float r = (float)pixel_r(px);
	const float g = (float)pixel_g(px);
	const float b = (float)pixel_b(px);
	return (uint8_t)std::lround(r * 0.299f + g * 0.587f + b * 0.114f);
}

extern "C" bool lt_thorvg_runtime_available(void)
{
	return true;
}

extern "C" struct lt_thorvg *lt_thorvg_create(const char *lottie_file, uint32_t cx,
					      uint32_t cy)
{
	if (!lottie_file || !*lottie_file || cx == 0 || cy == 0)
		return nullptr;

	if (!init_thorvg_runtime())
		return nullptr;

	obs_data_t *root = obs_data_create_from_json_file(lottie_file);
	if (!root) {
		term_thorvg_runtime();
		return nullptr;
	}

	auto *backend = new lt_thorvg;
	backend->width = cx;
	backend->height = cy;
	backend->rgba.assign((size_t)cx * (size_t)cy * 4, 0);
	slot_transform_identity(&backend->slot_a);
	slot_transform_identity(&backend->slot_b);
	backend->has_slots = lt_slot_set_load_file(lottie_file, backend->slots);

	const std::string rpath = dirname_from_path(lottie_file);

	obs_data_t *matte_a = filter_json_layers(root, LT_BACKEND_THORVG, "matteA");
	obs_data_t *matte_b = filter_json_layers(root, LT_BACKEND_THORVG, "matteB");
	obs_data_t *overlay = filter_json_layers(root, LT_BACKEND_THORVG, "overlay");

	bool ok = load_pass(backend->matte_a, matte_a, rpath, cx, cy);
	ok = load_pass(backend->matte_b, matte_b, rpath, cx, cy) || ok;
	ok = load_pass(backend->overlay, overlay, rpath, cx, cy) || ok;

	if (matte_a)
		obs_data_release(matte_a);
	if (matte_b)
		obs_data_release(matte_b);
	if (overlay)
		obs_data_release(overlay);
	obs_data_release(root);

	if (!ok) {
		lt_thorvg_destroy(backend);
		return nullptr;
	}

	return backend;
}

extern "C" void lt_thorvg_destroy(struct lt_thorvg *backend)
{
	if (!backend)
		return;

	if (backend->texture) {
		gs_texture_destroy(backend->texture);
		backend->texture = nullptr;
	}

	destroy_pass(backend->overlay);
	destroy_pass(backend->matte_b);
	destroy_pass(backend->matte_a);
	delete backend;
	term_thorvg_runtime();
}

extern "C" gs_texture_t *lt_thorvg_render(struct lt_thorvg *backend, float progress)
{
	if (!backend)
		return nullptr;

	progress = std::min(std::max(progress, 0.0f), 1.0f);
	render_pass(backend->matte_a, progress);
	render_pass(backend->matte_b, progress);
	render_pass(backend->overlay, progress);
	backend->has_slots =
		lt_slot_set_evaluate_progress(backend->slots, progress, &backend->slot_a,
					      &backend->slot_b);

	const size_t pixels = (size_t)backend->width * (size_t)backend->height;
	for (size_t i = 0; i < pixels; i++) {
		const uint32_t a = backend->matte_a.loaded ? backend->matte_a.pixels[i] : 0;
		const uint32_t b = backend->matte_b.loaded ? backend->matte_b.pixels[i] : 0;
		const uint32_t o = backend->overlay.loaded ? backend->overlay.pixels[i] : 0;
		const size_t idx = i * 4;

		backend->rgba[idx + 0] = backend->matte_a.loaded ? luma_from_argb(a) : 255;
		backend->rgba[idx + 1] = backend->matte_b.loaded ? luma_from_argb(b) : 0;
		backend->rgba[idx + 2] = backend->overlay.loaded ? pixel_a(o) : 0;
		backend->rgba[idx + 3] = 255;
	}

	if (!backend->texture) {
		const uint8_t *data = backend->rgba.data();
		backend->texture = gs_texture_create(backend->width, backend->height, GS_RGBA, 1,
						     &data, GS_DYNAMIC);
	} else {
		gs_texture_set_image(backend->texture, backend->rgba.data(),
				     backend->width * 4, false);
	}

	return backend->texture;
}

extern "C" bool lt_thorvg_get_slot_transforms(struct lt_thorvg *backend,
					      struct slot_transform *slot_a,
					      struct slot_transform *slot_b)
{
	if (!backend || !slot_a || !slot_b || !backend->has_slots)
		return false;

	*slot_a = backend->slot_a;
	*slot_b = backend->slot_b;
	return true;
}
