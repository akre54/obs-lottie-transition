#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "transform-decode.h"

static int parse_byte(const char *text, uint8_t *out)
{
	char *end = NULL;
	long value = strtol(text, &end, 10);

	if (!text || *text == '\0' || !end || *end != '\0')
		return 0;
	if (value < 0 || value > 255)
		return 0;

	*out = (uint8_t)value;
	return 1;
}

int main(int argc, char **argv)
{
	uint8_t pixels[24];
	struct slot_transform slot_a;
	struct slot_transform slot_b;

	if (argc != 25) {
		fprintf(stderr, "usage: %s <24 byte values>\n", argv[0]);
		return 2;
	}

	for (int i = 0; i < 24; i++) {
		if (!parse_byte(argv[i + 1], &pixels[i])) {
			fprintf(stderr, "invalid byte at index %d: %s\n", i, argv[i + 1]);
			return 2;
		}
	}

	transform_decode_from_pixels(pixels, 24, 6, &slot_a, &slot_b);

	printf("{\"slotA\":{\"pos_x\":%.6f,\"pos_y\":%.6f,\"scale_x\":%.6f,"
	       "\"scale_y\":%.6f,\"rotation\":%.6f,\"opacity\":%.6f},"
	       "\"slotB\":{\"pos_x\":%.6f,\"pos_y\":%.6f,\"scale_x\":%.6f,"
	       "\"scale_y\":%.6f,\"rotation\":%.6f,\"opacity\":%.6f}}\n",
	       slot_a.pos_x, slot_a.pos_y, slot_a.scale_x,
	       slot_a.scale_y, slot_a.rotation, slot_a.opacity,
	       slot_b.pos_x, slot_b.pos_y, slot_b.scale_x,
	       slot_b.scale_y, slot_b.rotation, slot_b.opacity);

	return 0;
}
