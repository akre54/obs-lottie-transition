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

bool lt_thorvg_runtime_available(void);
struct lt_thorvg *lt_thorvg_create(const char *lottie_file, uint32_t cx, uint32_t cy);
void lt_thorvg_destroy(struct lt_thorvg *backend);
gs_texture_t *lt_thorvg_render(struct lt_thorvg *backend, float progress);
bool lt_thorvg_get_slot_transforms(struct lt_thorvg *backend,
				   struct slot_transform *slot_a,
				   struct slot_transform *slot_b);

#ifdef __cplusplus
}
#endif

#endif
