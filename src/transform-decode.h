#ifndef TRANSFORM_DECODE_H
#define TRANSFORM_DECODE_H

#include <stdint.h>

struct slot_transform {
	float pos_x;
	float pos_y;
	float scale_x;
	float scale_y;
	float rotation;
	float opacity;
};

void slot_transform_identity(struct slot_transform *t);

void transform_decode_from_pixels(const uint8_t *data, uint32_t row_stride,
				  uint32_t width, struct slot_transform *slot_a,
				  struct slot_transform *slot_b);

#endif
