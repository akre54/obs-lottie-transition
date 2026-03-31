#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "transform-decode.h"

static uint16_t encode_float(float value, float min, float max)
{
	float normalized = (value - min) / (max - min);

	if (normalized < 0.0f)
		normalized = 0.0f;
	if (normalized > 1.0f)
		normalized = 1.0f;

	return (uint16_t)lroundf(normalized * 65535.0f);
}

static void encode_slot(const struct slot_transform *slot, uint8_t *pixels)
{
	static const float mins[6] = {-4096.0f, -4096.0f, 0.0f,
				      0.0f, -360.0f, 0.0f};
	static const float maxs[6] = {4096.0f, 4096.0f, 10.0f,
				      10.0f, 360.0f, 1.0f};
	const float values[6] = {
		slot->pos_x, slot->pos_y, slot->scale_x,
		slot->scale_y, slot->rotation, slot->opacity,
	};

	for (int i = 0; i < 3; i++) {
		uint16_t first = encode_float(values[i * 2], mins[i * 2], maxs[i * 2]);
		uint16_t second = encode_float(values[i * 2 + 1],
					       mins[i * 2 + 1], maxs[i * 2 + 1]);
		uint8_t *pixel = pixels + i * 4;

		pixel[0] = (uint8_t)((first >> 8) & 0xFF);
		pixel[1] = (uint8_t)(first & 0xFF);
		pixel[2] = (uint8_t)((second >> 8) & 0xFF);
		pixel[3] = (uint8_t)(second & 0xFF);
	}
}

static int nearly_equal(float a, float b, float epsilon)
{
	return fabsf(a - b) <= epsilon;
}

static void test_identity_on_blank_strip(void)
{
	uint8_t pixels[24] = {0};
	struct slot_transform slot_a;
	struct slot_transform slot_b;

	transform_decode_from_pixels(pixels, 24, 6, &slot_a, &slot_b);

	assert(nearly_equal(slot_a.pos_x, 0.0f, 0.001f));
	assert(nearly_equal(slot_a.scale_x, 1.0f, 0.001f));
	assert(nearly_equal(slot_a.opacity, 1.0f, 0.001f));
	assert(nearly_equal(slot_b.pos_y, 0.0f, 0.001f));
	assert(nearly_equal(slot_b.scale_y, 1.0f, 0.001f));
	assert(nearly_equal(slot_b.opacity, 1.0f, 0.001f));
}

static void test_decode_known_values(void)
{
	uint8_t pixels[24] = {0};
	struct slot_transform input_a = {
		.pos_x = 128.5f,
		.pos_y = -72.25f,
		.scale_x = 1.25f,
		.scale_y = 0.75f,
		.rotation = 45.0f,
		.opacity = 0.6f,
	};
	struct slot_transform input_b = {
		.pos_x = -512.0f,
		.pos_y = 384.0f,
		.scale_x = 2.0f,
		.scale_y = 2.5f,
		.rotation = -90.0f,
		.opacity = 0.2f,
	};
	struct slot_transform output_a;
	struct slot_transform output_b;

	encode_slot(&input_a, pixels);
	encode_slot(&input_b, pixels + 12);

	transform_decode_from_pixels(pixels, 24, 6, &output_a, &output_b);

	assert(nearly_equal(output_a.pos_x, input_a.pos_x, 0.2f));
	assert(nearly_equal(output_a.pos_y, input_a.pos_y, 0.2f));
	assert(nearly_equal(output_a.scale_x, input_a.scale_x, 0.01f));
	assert(nearly_equal(output_a.scale_y, input_a.scale_y, 0.01f));
	assert(nearly_equal(output_a.rotation, input_a.rotation, 0.02f));
	assert(nearly_equal(output_a.opacity, input_a.opacity, 0.001f));

	assert(nearly_equal(output_b.pos_x, input_b.pos_x, 0.2f));
	assert(nearly_equal(output_b.pos_y, input_b.pos_y, 0.2f));
	assert(nearly_equal(output_b.scale_x, input_b.scale_x, 0.01f));
	assert(nearly_equal(output_b.scale_y, input_b.scale_y, 0.01f));
	assert(nearly_equal(output_b.rotation, input_b.rotation, 0.02f));
	assert(nearly_equal(output_b.opacity, input_b.opacity, 0.001f));
}

static void test_width_guard_returns_identity(void)
{
	uint8_t pixels[16];
	struct slot_transform slot_a;
	struct slot_transform slot_b;

	memset(pixels, 0xFF, sizeof(pixels));
	transform_decode_from_pixels(pixels, 16, 4, &slot_a, &slot_b);

	assert(nearly_equal(slot_a.scale_x, 1.0f, 0.001f));
	assert(nearly_equal(slot_b.scale_y, 1.0f, 0.001f));
}

int main(void)
{
	test_identity_on_blank_strip();
	test_decode_known_values();
	test_width_guard_returns_identity();
	return 0;
}
