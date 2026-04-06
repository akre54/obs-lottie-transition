#include "lottie-thorvg.h"
#include "lottie-backend.h"
#include "lottie-slot-eval.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

extern "C" {
#include <util/platform.h>
}

#include <thorvg.h>

struct thorvg_pass {
	tvg::Animation *animation = nullptr;
	tvg::SwCanvas *canvas = nullptr;
	std::vector<uint32_t> pixels;
	gs_texture_t *texture = nullptr;
	bool loaded = false;
};

struct lt_thorvg {
	uint32_t width = 0;
	uint32_t height = 0;
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

struct filtered_pass_json {
	std::string json;
	bool has_layers = false;
};

static bool read_text_file(const char *path, std::string &out)
{
	if (!path || !*path)
		return false;

	std::ifstream file(path, std::ios::binary);
	if (!file)
		return false;

	out.assign(std::istreambuf_iterator<char>(file),
		   std::istreambuf_iterator<char>());
	return !out.empty();
}

static size_t skip_json_ws(const std::string &text, size_t pos)
{
	while (pos < text.size() &&
	       std::isspace(static_cast<unsigned char>(text[pos]))) {
		pos++;
	}
	return pos;
}

static bool find_matching_bracket(const std::string &text, size_t open_pos,
				  char open_ch, char close_ch, size_t *close_pos)
{
	bool in_string = false;
	bool escaped = false;
	int depth = 0;

	for (size_t i = open_pos; i < text.size(); i++) {
		char ch = text[i];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		if (ch == '"') {
			in_string = true;
			continue;
		}
		if (ch == open_ch) {
			depth++;
		} else if (ch == close_ch) {
			depth--;
			if (depth == 0) {
				*close_pos = i;
				return true;
			}
		}
	}

	return false;
}

static bool find_root_layers_array(const std::string &text, size_t *array_begin,
				   size_t *array_end)
{
	bool in_string = false;
	bool escaped = false;
	int object_depth = 0;
	int array_depth = 0;

	for (size_t i = 0; i < text.size(); i++) {
		char ch = text[i];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		if (ch == '"') {
			const size_t string_start = i;
			in_string = true;
			size_t j = i + 1;
			bool local_escaped = false;
			for (; j < text.size(); j++) {
				char inner = text[j];
				if (local_escaped) {
					local_escaped = false;
				} else if (inner == '\\') {
					local_escaped = true;
				} else if (inner == '"') {
					break;
				}
			}
			if (j >= text.size())
				return false;

			if (object_depth == 1 && array_depth == 0 &&
			    text.compare(string_start, j - string_start + 1,
					 "\"layers\"") == 0) {
				size_t colon = skip_json_ws(text, j + 1);
				if (colon >= text.size() || text[colon] != ':')
					return false;
				size_t array_pos = skip_json_ws(text, colon + 1);
				if (array_pos >= text.size() || text[array_pos] != '[')
					return false;
				*array_begin = array_pos;
				return find_matching_bracket(text, array_pos, '[', ']',
							     array_end);
			}

			i = j;
			in_string = false;
			continue;
		}

		if (ch == '{')
			object_depth++;
		else if (ch == '}')
			object_depth--;
		else if (ch == '[')
			array_depth++;
		else if (ch == ']')
			array_depth--;
	}

	return false;
}

static bool should_keep_layer(const char *name, enum lt_backend_type backend,
			      const char *pass_name)
{
	if (backend == LT_BACKEND_BROWSER) {
		if (strcmp(pass_name, "matteA") == 0)
			return name_equals(name, "[MatteA]") ||
			       name_equals(name, "[SlotA]") ||
			       name_equals(name, "[SlotB]");
		if (strcmp(pass_name, "matteB") == 0)
			return name_equals(name, "[MatteB]");
		if (strcmp(pass_name, "overlay") == 0)
			return !is_reserved_layer(name);
		return false;
	}

	if (strcmp(pass_name, "matteA") == 0)
		return name_equals(name, "[MatteA]");
	if (strcmp(pass_name, "matteB") == 0)
		return name_equals(name, "[MatteB]");
	if (strcmp(pass_name, "overlay") == 0)
		return !is_reserved_layer(name);
	return false;
}

static std::vector<std::string> split_top_level_items(const std::string &array_body)
{
	std::vector<std::string> items;
	bool in_string = false;
	bool escaped = false;
	int object_depth = 0;
	int array_depth = 0;
	size_t item_start = std::string::npos;

	for (size_t i = 0; i < array_body.size(); i++) {
		char ch = array_body[i];
		if (item_start == std::string::npos) {
			if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',')
				continue;
			item_start = i;
		}

		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		if (ch == '"') {
			in_string = true;
			continue;
		}
		if (ch == '{')
			object_depth++;
		else if (ch == '}')
			object_depth--;
		else if (ch == '[')
			array_depth++;
		else if (ch == ']')
			array_depth--;

		if (object_depth == 0 && array_depth == 0 && ch == ',') {
			items.emplace_back(array_body.substr(item_start, i - item_start));
			item_start = std::string::npos;
		}
	}

	if (item_start != std::string::npos)
		items.emplace_back(array_body.substr(item_start));

	return items;
}

static const char *extract_layer_name(const std::string &item, std::string &name_out)
{
	static const std::regex name_regex(
		"\"nm\"\\s*:\\s*\"((?:\\\\.|[^\\\\\"])*)\"");
	std::smatch match;
	if (!std::regex_search(item, match, name_regex) || match.size() < 2)
		return nullptr;

	name_out = match[1].str();
	return name_out.c_str();
}

static filtered_pass_json filter_json_layers(const std::string &src,
					     enum lt_backend_type backend,
					     const char *pass_name)
{
	filtered_pass_json out;
	size_t array_begin = 0;
	size_t array_end = 0;
	if (!find_root_layers_array(src, &array_begin, &array_end))
		return out;

	const std::string body =
		src.substr(array_begin + 1, array_end - array_begin - 1);
	const std::vector<std::string> items = split_top_level_items(body);
	std::string filtered_layers = "[";
	bool first = true;

	for (const std::string &item : items) {
		std::string name_storage;
		const char *name = extract_layer_name(item, name_storage);
		if (!should_keep_layer(name, backend, pass_name))
			continue;

		out.has_layers = true;
		if (!first)
			filtered_layers += ",";
		filtered_layers += item;
		first = false;
	}

	filtered_layers += "]";
	out.json = src.substr(0, array_begin) + filtered_layers +
		   src.substr(array_end + 1);
	return out;
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

static bool load_pass(thorvg_pass &pass, const std::string &json_text,
		      const std::string &rpath,
		      uint32_t width, uint32_t height)
{
	if (json_text.empty())
		return false;

	pass.animation = tvg::Animation::gen();
	if (!pass.animation)
		return false;

	tvg::Picture *picture = pass.animation->picture();
	if (!picture)
		return false;

	const uint32_t size = (uint32_t)json_text.size();
	if (picture->load(json_text.c_str(), size, "lottie+json",
			  rpath.empty() ? nullptr : rpath.c_str(), true) !=
	    tvg::Result::Success)
		return false;

	picture->size((float)width, (float)height);

	pass.canvas = tvg::SwCanvas::gen();
	if (!pass.canvas)
		return false;

	pass.pixels.assign((size_t)width * (size_t)height, 0);
	if (pass.canvas->target(pass.pixels.data(), width, width, height,
				tvg::ColorSpace::ARGB8888S) != tvg::Result::Success)
		return false;

	if (pass.canvas->add(picture) != tvg::Result::Success)
		return false;

	pass.loaded = true;
	return true;
}

static void destroy_pass(thorvg_pass &pass)
{
	if (pass.texture) {
		gs_texture_destroy(pass.texture);
		pass.texture = nullptr;
	}

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

static bool update_pass_texture(thorvg_pass &pass, uint32_t width, uint32_t height)
{
	if (!pass.loaded)
		return false;

	if (!pass.texture) {
		const uint8_t *data =
			reinterpret_cast<const uint8_t *>(pass.pixels.data());
		pass.texture = gs_texture_create(width, height, GS_BGRA, 1, &data,
						 GS_DYNAMIC);
	}

	if (!pass.texture)
		return false;

	uint8_t *mapped = nullptr;
	uint32_t linesize = 0;
	const uint8_t *src =
		reinterpret_cast<const uint8_t *>(pass.pixels.data());

	if (gs_texture_map(pass.texture, &mapped, &linesize)) {
		for (uint32_t y = 0; y < height; y++) {
			memcpy(mapped + (size_t)y * linesize,
			       src + (size_t)y * width * sizeof(uint32_t),
			       (size_t)width * sizeof(uint32_t));
		}
		gs_texture_unmap(pass.texture);
	} else {
		gs_texture_set_image(pass.texture, src, width * sizeof(uint32_t),
				     false);
	}

	return true;
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

	std::string root;
	if (!read_text_file(lottie_file, root)) {
		term_thorvg_runtime();
		return nullptr;
	}

	auto *backend = new lt_thorvg;
	backend->width = cx;
	backend->height = cy;
	slot_transform_identity(&backend->slot_a);
	slot_transform_identity(&backend->slot_b);
	backend->has_slots = lt_slot_set_load_file(lottie_file, backend->slots);

	const std::string rpath = dirname_from_path(lottie_file);
	filtered_pass_json matte_a =
		filter_json_layers(root, LT_BACKEND_THORVG, "matteA");
	filtered_pass_json matte_b =
		filter_json_layers(root, LT_BACKEND_THORVG, "matteB");
	filtered_pass_json overlay =
		filter_json_layers(root, LT_BACKEND_THORVG, "overlay");

	bool ok = false;
	if (matte_a.has_layers)
		ok = load_pass(backend->matte_a, matte_a.json, rpath, cx, cy) || ok;
	if (matte_b.has_layers)
		ok = load_pass(backend->matte_b, matte_b.json, rpath, cx, cy) || ok;
	if (overlay.has_layers)
		ok = load_pass(backend->overlay, overlay.json, rpath, cx, cy) || ok;

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

	destroy_pass(backend->overlay);
	destroy_pass(backend->matte_b);
	destroy_pass(backend->matte_a);
	delete backend;
	term_thorvg_runtime();
}

extern "C" gs_texture_t *lt_thorvg_render(struct lt_thorvg *backend, float progress,
					  struct lt_thorvg_render_stats *stats)
{
	if (!backend)
		return nullptr;

	if (stats)
		*stats = {};

	const uint64_t total_start_ns = os_gettime_ns();
	progress = std::min(std::max(progress, 0.0f), 1.0f);
	const uint64_t pass_start_ns = os_gettime_ns();
	render_pass(backend->matte_a, progress);
	render_pass(backend->matte_b, progress);
	render_pass(backend->overlay, progress);
	const uint64_t pass_end_ns = os_gettime_ns();

	backend->has_slots =
		lt_slot_set_evaluate_progress(backend->slots, progress, &backend->slot_a,
					      &backend->slot_b);
	const uint64_t slot_end_ns = os_gettime_ns();
	const uint64_t pack_end_ns = slot_end_ns;

	update_pass_texture(backend->matte_a, backend->width, backend->height);
	update_pass_texture(backend->matte_b, backend->width, backend->height);
	const uint64_t upload_end_ns = os_gettime_ns();

	if (stats) {
		stats->pass_ns = pass_end_ns - pass_start_ns;
		stats->slot_eval_ns = slot_end_ns - pass_end_ns;
		stats->pack_ns = pack_end_ns - slot_end_ns;
		stats->upload_ns = upload_end_ns - pack_end_ns;
		stats->total_ns = upload_end_ns - total_start_ns;
	}

	return backend->matte_a.texture ? backend->matte_a.texture :
	       (backend->matte_b.texture ? backend->matte_b.texture : nullptr);
}

extern "C" gs_texture_t *lt_thorvg_get_matte_a_texture(struct lt_thorvg *backend)
{
	return backend ? backend->matte_a.texture : nullptr;
}

extern "C" gs_texture_t *lt_thorvg_get_matte_b_texture(struct lt_thorvg *backend)
{
	return backend ? backend->matte_b.texture : nullptr;
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
