#ifndef PSXRECOMP_XENOGEARS_FIELD_HOOK_H
#define PSXRECOMP_XENOGEARS_FIELD_HOOK_H
#include <stdint.h>
#include "xenogears_timing.h"
#ifdef __cplusplus
extern "C" {
#endif
int32_t psx_xenogears_field_frame_step(uint32_t site_pc, uint32_t instruction_word,
                                       int32_t original_step, uint32_t frame_token,
                                       uint32_t tier);
void psx_xenogears_field_resident_dma(uint32_t load_addr, uint32_t size);
void psx_xenogears_field_resident_invalidate(void);
#ifdef __cplusplus
}
#endif
#endif
