#include "lottie-slot-eval.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace {

enum class JsonType {
	Null,
	Bool,
	Number,
	String,
	Array,
	Object,
};

struct JsonValue {
	JsonType type = JsonType::Null;
	bool bool_value = false;
	double number_value = 0.0;
	std::string string_value;
	std::vector<JsonValue> array_value;
	std::map<std::string, JsonValue> object_value;

	bool is_object() const { return type == JsonType::Object; }
	bool is_array() const { return type == JsonType::Array; }
	bool is_number() const { return type == JsonType::Number; }
	bool is_bool() const { return type == JsonType::Bool; }
	bool is_string() const { return type == JsonType::String; }

	const JsonValue *find(const char *key) const
	{
		auto it = object_value.find(key);
		return it != object_value.end() ? &it->second : nullptr;
	}
};

class JsonParser {
public:
	explicit JsonParser(const std::string &input) : input(input) {}

	bool parse(JsonValue &out)
	{
		skip_ws();
		if (!parse_value(out))
			return false;
		skip_ws();
		return pos == input.size();
	}

private:
	const std::string &input;
	size_t pos = 0;

	void skip_ws()
	{
		while (pos < input.size() && std::isspace((unsigned char)input[pos]))
			pos++;
	}

	bool parse_value(JsonValue &out)
	{
		if (pos >= input.size())
			return false;

		switch (input[pos]) {
		case '{':
			return parse_object(out);
		case '[':
			return parse_array(out);
		case '"':
			out.type = JsonType::String;
			return parse_string(out.string_value);
		case 't':
			return parse_literal("true", out, true);
		case 'f':
			return parse_literal("false", out, false);
		case 'n':
			return parse_null(out);
		default:
			return parse_number(out);
		}
	}

	bool parse_object(JsonValue &out)
	{
		if (input[pos] != '{')
			return false;
		out.type = JsonType::Object;
		out.object_value.clear();
		pos++;
		skip_ws();
		if (pos < input.size() && input[pos] == '}') {
			pos++;
			return true;
		}

		while (pos < input.size()) {
			std::string key;
			JsonValue value;
			if (!parse_string(key))
				return false;
			skip_ws();
			if (pos >= input.size() || input[pos] != ':')
				return false;
			pos++;
			skip_ws();
			if (!parse_value(value))
				return false;
			out.object_value.emplace(std::move(key), std::move(value));
			skip_ws();
			if (pos >= input.size())
				return false;
			if (input[pos] == '}') {
				pos++;
				return true;
			}
			if (input[pos] != ',')
				return false;
			pos++;
			skip_ws();
		}
		return false;
	}

	bool parse_array(JsonValue &out)
	{
		if (input[pos] != '[')
			return false;
		out.type = JsonType::Array;
		out.array_value.clear();
		pos++;
		skip_ws();
		if (pos < input.size() && input[pos] == ']') {
			pos++;
			return true;
		}

		while (pos < input.size()) {
			JsonValue value;
			if (!parse_value(value))
				return false;
			out.array_value.emplace_back(std::move(value));
			skip_ws();
			if (pos >= input.size())
				return false;
			if (input[pos] == ']') {
				pos++;
				return true;
			}
			if (input[pos] != ',')
				return false;
			pos++;
			skip_ws();
		}
		return false;
	}

	bool parse_string(std::string &out)
	{
		if (pos >= input.size() || input[pos] != '"')
			return false;

		out.clear();
		pos++;
		while (pos < input.size()) {
			char ch = input[pos++];
			if (ch == '"')
				return true;
			if (ch == '\\') {
				if (pos >= input.size())
					return false;
				char esc = input[pos++];
				switch (esc) {
				case '"':
				case '\\':
				case '/':
					out.push_back(esc);
					break;
				case 'b':
					out.push_back('\b');
					break;
				case 'f':
					out.push_back('\f');
					break;
				case 'n':
					out.push_back('\n');
					break;
				case 'r':
					out.push_back('\r');
					break;
				case 't':
					out.push_back('\t');
					break;
				case 'u':
					if (pos + 4 > input.size())
						return false;
					out.push_back('?');
					pos += 4;
					break;
				default:
					return false;
				}
			} else {
				out.push_back(ch);
			}
		}
		return false;
	}

	bool parse_literal(const char *literal, JsonValue &out, bool value)
	{
		const size_t len = strlen(literal);
		if (input.compare(pos, len, literal) != 0)
			return false;
		pos += len;
		out.type = JsonType::Bool;
		out.bool_value = value;
		return true;
	}

	bool parse_null(JsonValue &out)
	{
		if (input.compare(pos, 4, "null") != 0)
			return false;
		pos += 4;
		out = JsonValue{};
		return true;
	}

	bool parse_number(JsonValue &out)
	{
		size_t start = pos;
		if (input[pos] == '-')
			pos++;
		while (pos < input.size() && std::isdigit((unsigned char)input[pos]))
			pos++;
		if (pos < input.size() && input[pos] == '.') {
			pos++;
			while (pos < input.size() && std::isdigit((unsigned char)input[pos]))
				pos++;
		}
		if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
			pos++;
			if (pos < input.size() && (input[pos] == '+' || input[pos] == '-'))
				pos++;
			while (pos < input.size() && std::isdigit((unsigned char)input[pos]))
				pos++;
		}
		if (start == pos)
			return false;
		out.type = JsonType::Number;
		out.number_value = std::strtod(input.c_str() + start, nullptr);
		return true;
	}
};

static bool load_text_file(const char *path, std::string &out)
{
	if (!path || !*path)
		return false;

	std::ifstream input(path, std::ios::in | std::ios::binary);
	if (!input)
		return false;

	std::ostringstream buffer;
	buffer << input.rdbuf();
	out = buffer.str();
	return !out.empty();
}

static float json_number(const JsonValue *value, float fallback = 0.0f)
{
	if (!value)
		return fallback;
	if (value->is_number())
		return (float)value->number_value;
	if (value->is_bool())
		return value->bool_value ? 1.0f : 0.0f;
	return fallback;
}

static std::vector<float> json_number_array(const JsonValue *value)
{
	std::vector<float> out;
	if (!value)
		return out;
	if (value->is_number()) {
		out.push_back((float)value->number_value);
		return out;
	}
	if (!value->is_array())
		return out;

	out.reserve(value->array_value.size());
	for (const auto &item : value->array_value) {
		if (item.is_number())
			out.push_back((float)item.number_value);
	}
	return out;
}

static float component_value(const std::vector<float> &values, size_t index, float fallback)
{
	if (values.empty())
		return fallback;
	if (index < values.size())
		return values[index];
	return values.back();
}

static float cubic_bezier(float p0, float p1, float p2, float p3, float t)
{
	const float omt = 1.0f - t;
	return omt * omt * omt * p0 + 3.0f * omt * omt * t * p1 +
	       3.0f * omt * t * t * p2 + t * t * t * p3;
}

static float cubic_bezier_ease(float x1, float y1, float x2, float y2, float x)
{
	float lower = 0.0f;
	float upper = 1.0f;
	float t = x;

	for (int i = 0; i < 14; i++) {
		float estimate = cubic_bezier(0.0f, x1, x2, 1.0f, t);
		if (fabsf(estimate - x) < 1e-4f)
			break;
		if (estimate < x)
			lower = t;
		else
			upper = t;
		t = (lower + upper) * 0.5f;
	}

	return cubic_bezier(0.0f, y1, y2, 1.0f, t);
}

static lt_slot_property parse_property(const JsonValue *value)
{
	lt_slot_property prop;
	if (!value || !value->is_object())
		return prop;

	const JsonValue *animated = value->find("a");
	const JsonValue *raw = value->find("k");
	if (!raw)
		return prop;

	prop.present = true;
	prop.animated = json_number(animated, 0.0f) != 0.0f;

	if (!prop.animated) {
		prop.value = json_number_array(raw);
		if (prop.value.empty() && raw->is_number())
			prop.value.push_back((float)raw->number_value);
		return prop;
	}

	if (!raw->is_array())
		return prop;

	for (const auto &entry : raw->array_value) {
		if (!entry.is_object())
			continue;

		lt_slot_keyframe key;
		key.time = json_number(entry.find("t"), 0.0f);
		key.start = json_number_array(entry.find("s"));
		key.end = json_number_array(entry.find("e"));
		key.hold = json_number(entry.find("h"), 0.0f) != 0.0f;

		const JsonValue *ease_in = entry.find("i");
		const JsonValue *ease_out = entry.find("o");
		if (ease_in && ease_in->is_object()) {
			key.in_x = json_number_array(ease_in->find("x"));
			key.in_y = json_number_array(ease_in->find("y"));
		}
		if (ease_out && ease_out->is_object()) {
			key.out_x = json_number_array(ease_out->find("x"));
			key.out_y = json_number_array(ease_out->find("y"));
		}

		if (!key.start.empty())
			prop.keyframes.emplace_back(std::move(key));
	}

	if (prop.keyframes.empty())
		prop.animated = false;

	return prop;
}

static lt_slot_track parse_track(const JsonValue &layer)
{
	lt_slot_track track;
	if (!layer.is_object())
		return track;

	const JsonValue *ks = layer.find("ks");
	if (!ks || !ks->is_object())
		return track;

	track.present = true;
	track.in_point = json_number(layer.find("ip"), 0.0f);
	track.out_point = json_number(layer.find("op"),
				      std::numeric_limits<float>::max());
	track.position = parse_property(ks->find("p"));
	track.scale = parse_property(ks->find("s"));
	track.rotation = parse_property(ks->find("r"));
	if (!track.rotation.present)
		track.rotation = parse_property(ks->find("rz"));
	track.opacity = parse_property(ks->find("o"));
	return track;
}

static bool parse_slot_set(const JsonValue &root, lt_slot_set &out)
{
	if (!root.is_object())
		return false;

	const JsonValue *layers = root.find("layers");
	if (!layers || !layers->is_array())
		return false;

	out = lt_slot_set{};
	out.start_frame = json_number(root.find("ip"), 0.0f);
	out.end_frame = json_number(root.find("op"), 0.0f);

	for (const auto &layer : layers->array_value) {
		if (!layer.is_object())
			continue;
		const JsonValue *name = layer.find("nm");
		if (!name || !name->is_string())
			continue;

		if (name->string_value == "[SlotA]")
			out.slot_a = parse_track(layer);
		else if (name->string_value == "[SlotB]")
			out.slot_b = parse_track(layer);
	}

	out.loaded = out.slot_a.present || out.slot_b.present;
	return true;
}

static std::vector<float> evaluate_property(const lt_slot_property &property, float frame,
					    const std::vector<float> &fallback)
{
	if (!property.present)
		return fallback;
	if (!property.animated || property.keyframes.empty())
		return property.value.empty() ? fallback : property.value;

	const auto &keys = property.keyframes;
	if (frame <= keys.front().time)
		return keys.front().start;

	for (size_t i = 0; i + 1 < keys.size(); i++) {
		const lt_slot_keyframe &current = keys[i];
		const lt_slot_keyframe &next = keys[i + 1];
		if (frame >= next.time)
			continue;

		if (current.hold || next.time <= current.time || current.end.empty())
			return current.start;

		float progress = (frame - current.time) / (next.time - current.time);
		progress = std::min(std::max(progress, 0.0f), 1.0f);

		std::vector<float> values;
		values.resize(std::max(current.start.size(), current.end.size()));
		for (size_t c = 0; c < values.size(); c++) {
			float eased = progress;
			float x1 = component_value(current.out_x, c, component_value(current.out_x, 0, 0.0f));
			float y1 = component_value(current.out_y, c, component_value(current.out_y, 0, 0.0f));
			float x2 = component_value(current.in_x, c, component_value(current.in_x, 0, 1.0f));
			float y2 = component_value(current.in_y, c, component_value(current.in_y, 0, 1.0f));
			eased = cubic_bezier_ease(x1, y1, x2, y2, progress);
			values[c] = component_value(current.start, c, 0.0f) +
				    (component_value(current.end, c,
						     component_value(next.start, c, 0.0f)) -
				     component_value(current.start, c, 0.0f)) *
					    eased;
		}
		return values;
	}

	return keys.back().start;
}

static void evaluate_track(const lt_slot_track &track, float frame,
			   struct slot_transform *out)
{
	slot_transform_identity(out);
	if (!track.present)
		return;

	if (frame < track.in_point || frame > track.out_point) {
		out->opacity = 0.0f;
		return;
	}

	const std::vector<float> position = evaluate_property(
		track.position, frame, {0.0f, 0.0f, 0.0f});
	const std::vector<float> scale = evaluate_property(
		track.scale, frame, {100.0f, 100.0f, 100.0f});
	const std::vector<float> rotation = evaluate_property(
		track.rotation, frame, {0.0f});
	const std::vector<float> opacity = evaluate_property(
		track.opacity, frame, {100.0f});

	out->pos_x = component_value(position, 0, 0.0f);
	out->pos_y = component_value(position, 1, 0.0f);
	out->scale_x = component_value(scale, 0, 100.0f) / 100.0f;
	out->scale_y = component_value(scale, 1, component_value(scale, 0, 100.0f)) / 100.0f;
	out->rotation = component_value(rotation, 0, 0.0f);
	out->opacity = component_value(opacity, 0, 100.0f) / 100.0f;
}

} // namespace

bool lt_slot_set_load_file(const char *path, lt_slot_set &out)
{
	std::string text;
	JsonValue root;
	JsonParser parser(text);

	if (!load_text_file(path, text))
		return false;

	JsonParser file_parser(text);
	if (!file_parser.parse(root))
		return false;

	return parse_slot_set(root, out);
}

bool lt_slot_set_evaluate_frame(const lt_slot_set &slots, float frame,
				struct slot_transform *slot_a,
				struct slot_transform *slot_b)
{
	if (!slots.loaded)
		return false;

	evaluate_track(slots.slot_a, frame, slot_a);
	evaluate_track(slots.slot_b, frame, slot_b);
	return true;
}

bool lt_slot_set_evaluate_progress(const lt_slot_set &slots, float progress,
				   struct slot_transform *slot_a,
				   struct slot_transform *slot_b)
{
	if (!slots.loaded)
		return false;

	progress = std::min(std::max(progress, 0.0f), 1.0f);
	const float total = std::max(slots.end_frame - slots.start_frame, 1.0f);
	const float frame = slots.start_frame + progress * std::max(total - 1.0f, 0.0f);
	return lt_slot_set_evaluate_frame(slots, frame, slot_a, slot_b);
}
