#ifndef LOTTIE_SLOT_EVAL_HPP
#define LOTTIE_SLOT_EVAL_HPP

#include <string>
#include <vector>

extern "C" {
#include "transform-decode.h"
}

struct lt_slot_keyframe {
	float time = 0.0f;
	std::vector<float> start;
	std::vector<float> end;
	std::vector<float> in_x;
	std::vector<float> in_y;
	std::vector<float> out_x;
	std::vector<float> out_y;
	bool hold = false;
};

struct lt_slot_property {
	bool present = false;
	bool animated = false;
	std::vector<float> value;
	std::vector<lt_slot_keyframe> keyframes;
};

struct lt_slot_track {
	bool present = false;
	float in_point = 0.0f;
	float out_point = 0.0f;
	lt_slot_property position;
	lt_slot_property scale;
	lt_slot_property rotation;
	lt_slot_property opacity;
};

struct lt_slot_set {
	bool loaded = false;
	float start_frame = 0.0f;
	float end_frame = 0.0f;
	lt_slot_track slot_a;
	lt_slot_track slot_b;
};

bool lt_slot_set_load_file(const char *path, lt_slot_set &out);
bool lt_slot_set_evaluate_frame(const lt_slot_set &slots, float frame,
				struct slot_transform *slot_a,
				struct slot_transform *slot_b);
bool lt_slot_set_evaluate_progress(const lt_slot_set &slots, float progress,
				   struct slot_transform *slot_a,
				   struct slot_transform *slot_b);

#endif
