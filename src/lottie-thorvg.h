#ifndef LOTTIE_THORVG_H
#define LOTTIE_THORVG_H

#include <stdbool.h>
#include <stdint.h>

#include <graphics/graphics.h>

#ifdef __cplusplus
extern "C" {
#endif

struct slot_transform;
struct lt_thorvg;
struct lt_thorvg_render_stats {
	uint64_t pass_ns;
	uint64_t slot_eval_ns;
	uint64_t pack_ns;
	uint64_t upload_ns;
	uint64_t total_ns;
};

bool lt_thorvg_runtime_available(void);
struct lt_thorvg *lt_thorvg_create(const char *lottie_file, uint32_t cx, uint32_t cy);
void lt_thorvg_destroy(struct lt_thorvg *backend);
gs_texture_t *lt_thorvg_render(struct lt_thorvg *backend, float progress,
			       struct lt_thorvg_render_stats *stats);
gs_texture_t *lt_thorvg_get_matte_a_texture(struct lt_thorvg *backend);
gs_texture_t *lt_thorvg_get_matte_b_texture(struct lt_thorvg *backend);
bool lt_thorvg_get_slot_transforms(struct lt_thorvg *backend,
				   struct slot_transform *slot_a,
				   struct slot_transform *slot_b);

#ifdef __cplusplus
}
#endif

#endif
