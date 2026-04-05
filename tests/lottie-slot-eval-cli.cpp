#include <cstdio>
#include <cstdlib>

#include "lottie-slot-eval.hpp"

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <lottie-file> <frame>\n", argv[0]);
		return 2;
	}

	lt_slot_set slots;
	if (!lt_slot_set_load_file(argv[1], slots)) {
		fprintf(stderr, "failed to load slot file\n");
		return 1;
	}

	slot_transform slot_a{};
	slot_transform slot_b{};
	if (!lt_slot_set_evaluate_frame(slots, (float)atof(argv[2]), &slot_a, &slot_b)) {
		fprintf(stderr, "failed to evaluate slots\n");
		return 1;
	}

	printf("{\"slotA\":{\"pos_x\":%.6f,\"pos_y\":%.6f,\"scale_x\":%.6f,"
	       "\"scale_y\":%.6f,\"rotation\":%.6f,\"opacity\":%.6f},"
	       "\"slotB\":{\"pos_x\":%.6f,\"pos_y\":%.6f,\"scale_x\":%.6f,"
	       "\"scale_y\":%.6f,\"rotation\":%.6f,\"opacity\":%.6f}}\n",
	       slot_a.pos_x, slot_a.pos_y, slot_a.scale_x, slot_a.scale_y,
	       slot_a.rotation, slot_a.opacity, slot_b.pos_x, slot_b.pos_y,
	       slot_b.scale_x, slot_b.scale_y, slot_b.rotation, slot_b.opacity);
	return 0;
}
