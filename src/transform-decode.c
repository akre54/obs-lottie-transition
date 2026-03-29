#include "transform-decode.h"
#include <string.h>

/* Normalization ranges for each float field (order matches pixel layout) */
static const float range_min[6] = {-4096.0f, -4096.0f, 0.0f, 0.0f, -360.0f, 0.0f};
static const float range_max[6] = {4096.0f, 4096.0f, 10.0f, 10.0f, 360.0f, 1.0f};


void slot_transform_identity(struct slot_transform *t)
{
	t->pos_x = 0.0f;
	t->pos_y = 0.0f;
	t->scale_x = 1.0f;
	t->scale_y = 1.0f;
	t->rotation = 0.0f;
	t->opacity = 1.0f;
}

/*
 * Decode a single uint16 from two bytes (high, low) to a float within
 * the given [min, max] range.
 */
static float decode_float(uint8_t high, uint8_t low, float min, float max)
{
	uint16_t raw = ((uint16_t)high << 8) | (uint16_t)low;
	float t = (float)raw / 65535.0f;
	float val = min + t * (max - min);

	/* Clamp to range */
	if (val < min)
		val = min;
	if (val > max)
		val = max;

	return val;
}

/*
 * Decode 6 floats from 3 consecutive RGBA pixels starting at `px`.
 * Each pixel carries 2 floats: R,G = float1 (high,low), B,A = float2 (high,low).
 */
static void decode_slot(const uint8_t *px, struct slot_transform *slot)
{
	float vals[6];

	for (int i = 0; i < 3; i++) {
		const uint8_t *p = px + i * 4;
		vals[i * 2] = decode_float(p[0], p[1], range_min[i * 2],
					   range_max[i * 2]);
		vals[i * 2 + 1] = decode_float(p[2], p[3], range_min[i * 2 + 1],
						range_max[i * 2 + 1]);
	}

	slot->pos_x = vals[0];
	slot->pos_y = vals[1];
	slot->scale_x = vals[2];
	slot->scale_y = vals[3];
	slot->rotation = vals[4];
	slot->opacity = vals[5];
}

/*
 * Check if the data strip looks valid. We require at least 6 pixels wide
 * (3 per slot) and non-null data. Returns 1 if valid, 0 otherwise.
 */
static int strip_looks_valid(const uint8_t *data, uint32_t width)
{
	if (!data || width < 6)
		return 0;

	/* Check that the pixel data is not all zeros (garbage/blank frame) */
	const uint8_t *strip = data;
	int nonzero = 0;
	for (uint32_t i = 0; i < 6 * 4; i++) {
		if (strip[i] != 0) {
			nonzero = 1;
			break;
		}
	}

	return nonzero;
}

void transform_decode_from_pixels(const uint8_t *data, uint32_t row_stride,
				  uint32_t width, struct slot_transform *slot_a,
				  struct slot_transform *slot_b)
{
	(void)row_stride;

	if (!strip_looks_valid(data, width)) {
		slot_transform_identity(slot_a);
		slot_transform_identity(slot_b);
		return;
	}

	/* Slot A: pixels 0-2 (byte offset 0) */
	decode_slot(data, slot_a);

	/* Slot B: pixels 3-5 (byte offset 12) */
	decode_slot(data + 3 * 4, slot_b);
}
