#include <cassert>
#include <cmath>

#include "lottie-slot-eval.hpp"

#ifndef LT_SOURCE_DIR
#define LT_SOURCE_DIR "."
#endif

static bool approx(float a, float b, float epsilon = 0.02f)
{
	return std::fabs(a - b) <= epsilon;
}

static void test_progress_and_hold_interpolation(void)
{
	lt_slot_set slots;
	slots.loaded = true;
	slots.start_frame = 0.0f;
	slots.end_frame = 11.0f;
	slots.slot_a.present = true;
	slots.slot_a.in_point = 0.0f;
	slots.slot_a.out_point = 10.0f;
	slots.slot_a.position.present = true;
	slots.slot_a.position.animated = true;
	slots.slot_a.position.keyframes = {
		{0.0f, {100.0f, 200.0f}, {100.0f, 200.0f}, {}, {}, {}, {}, true},
		{5.0f, {100.0f, 200.0f}, {400.0f, 500.0f}, {1.0f}, {1.0f}, {0.0f}, {0.0f}, false},
		{10.0f, {400.0f, 500.0f}, {}, {}, {}, {}, {}, false},
	};

	slot_transform a{};
	slot_transform b{};
	assert(lt_slot_set_evaluate_frame(slots, 3.0f, &a, &b));
	assert(approx(a.pos_x, 100.0f));
	assert(approx(a.pos_y, 200.0f));

	assert(lt_slot_set_evaluate_progress(slots, 1.0f, &a, &b));
	assert(approx(a.pos_x, 400.0f));
	assert(approx(a.pos_y, 500.0f));
}

static void test_out_of_range_slot_becomes_transparent(void)
{
	lt_slot_set slots;
	slots.loaded = true;
	slots.start_frame = 0.0f;
	slots.end_frame = 20.0f;
	slots.slot_a.present = true;
	slots.slot_a.in_point = 2.0f;
	slots.slot_a.out_point = 4.0f;

	slot_transform a{};
	slot_transform b{};
	assert(lt_slot_set_evaluate_frame(slots, 0.0f, &a, &b));
	assert(approx(a.opacity, 0.0f));
	assert(approx(a.scale_x, 1.0f));
	assert(approx(a.scale_y, 1.0f));
}

static void test_slide_and_mask_slots(void)
{
	lt_slot_set slots;
	assert(lt_slot_set_load_file(LT_SOURCE_DIR "/examples/slide-and-mask.json", slots));

	slot_transform a{};
	slot_transform b{};
	assert(lt_slot_set_evaluate_frame(slots, 0.0f, &a, &b));
	assert(approx(a.pos_x, 960.0f));
	assert(approx(b.pos_x, 2880.0f));
	assert(approx(a.scale_x, 1.0f));
	assert(approx(b.scale_x, 1.0f));

	assert(lt_slot_set_evaluate_frame(slots, 30.0f, &a, &b));
	assert(approx(a.pos_x, -960.0f));
	assert(approx(b.pos_x, 960.0f));
}

static void test_sliding_window_slots(void)
{
	lt_slot_set slots;
	assert(lt_slot_set_load_file(LT_SOURCE_DIR "/examples/sliding-window.json", slots));

	slot_transform a{};
	slot_transform b{};
	assert(lt_slot_set_evaluate_frame(slots, 0.0f, &a, &b));
	assert(approx(a.scale_x, 1.0f));
	assert(approx(a.scale_y, 1.0f));
	assert(approx(b.scale_x, 1.1f));
	assert(approx(b.scale_y, 1.1f));
	assert(approx(b.pos_x, 1200.0f));

	assert(lt_slot_set_evaluate_frame(slots, 45.0f, &a, &b));
	assert(approx(a.scale_x, 1.05f));
	assert(approx(a.scale_y, 1.05f));
	assert(approx(b.scale_x, 1.0f));
	assert(approx(b.scale_y, 1.0f));
	assert(approx(b.pos_x, 960.0f));
}

int main(void)
{
	test_progress_and_hold_interpolation();
	test_out_of_range_slot_becomes_transparent();
	test_slide_and_mask_slots();
	test_sliding_window_slots();
	return 0;
}
