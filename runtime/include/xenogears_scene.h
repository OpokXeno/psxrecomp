#ifndef PSXRECOMP_XENOGEARS_SCENE_H
#define PSXRECOMP_XENOGEARS_SCENE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t field_context, requested_module, active_module, module_pointer;
    uint16_t field_id, raw_field_id, masked_field_id, game_progress;
    uint8_t valid_field;
} XgScene;

void psx_xenogears_read_scene(XgScene *out);
void psx_xenogears_scene_reset(void);
void psx_xenogears_scene_vblank_boundary(int fmv_active);
uint32_t psx_xenogears_scene_generation(void);

#ifdef __cplusplus
}
#endif

#endif
