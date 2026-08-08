#ifndef PSXRECOMP_XENOGEARS_TIMING_H
#define PSXRECOMP_XENOGEARS_TIMING_H

#include <stdint.h>
#include "native_fps_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { XG_TIMING_ROUTE_NONE = 0, XG_TIMING_ROUTE_FIELD_DEV = 1 } XgTimingRoute;
typedef enum {
    XG_TIMING_REASON_BOOT = 0, XG_TIMING_REASON_ORIGINAL_MODE,
    XG_TIMING_REASON_WAITING_FIELD, XG_TIMING_REASON_FIELD_DEV,
    XG_TIMING_REASON_SCENE_TRANSITION, XG_TIMING_REASON_FMV_BOUNDARY,
    XG_TIMING_REASON_SAVESTATE_LOAD, XG_TIMING_REASON_REQUEST_FROZEN,
} XgTimingReason;
typedef struct {
    uint32_t field_context, requested_module, active_module, module_pointer;
    uint16_t field_id, raw_field_id, masked_field_id, game_progress;
    uint8_t valid_field;
} XgTimingScene;
typedef enum { XG_FIELD_TICK_ORIGINAL = 0, XG_FIELD_TICK_LOGICAL = 1, XG_FIELD_TICK_VISUAL = 2 } XgFieldTickPhase;
typedef enum { XG_FIELD_TIER_COLD_INTERPRETER = 0, XG_FIELD_TIER_WARM_NATIVE = 1 } XgFieldExecutionTier;
typedef struct {
    uint32_t load_base, logical_identity, site_pc, instruction_word;
    uint64_t guest_vblank;
    uint32_t frame_token;
    XgFieldExecutionTier tier;
} XgFieldFrameContext;

void psx_xenogears_timing_read_scene(XgTimingScene *out);
int psx_xenogears_timing_set_startup_policy(const NativeFpsStartupPolicy *policy);
void psx_xenogears_timing_reset(XgTimingReason reason);
void psx_xenogears_timing_on_savestate_loaded(void);
void psx_xenogears_timing_vblank_boundary(int fmv_active);
NativeFpsMode psx_xenogears_timing_effective_mode(void);
NativeFpsMode psx_xenogears_timing_requested_mode(void);
XgTimingRoute psx_xenogears_timing_route(void);
XgTimingReason psx_xenogears_timing_reason(void);
const char *psx_xenogears_timing_route_name(void);
const char *psx_xenogears_timing_reason_name(void);
uint32_t psx_xenogears_timing_scene_generation(void);
uint64_t psx_xenogears_timing_accumulator(void);
int32_t psx_xenogears_timing_field_frame_step(const XgFieldFrameContext *context,
                                               int32_t original_step);
int psx_xenogears_timing_field_route_active(void);
uint64_t psx_xenogears_timing_field_helper_activations(void);
uint64_t psx_xenogears_timing_field_authored_updates(void);
void psx_xenogears_timing_field_invalidate(void);

uint32_t psx_xenogears_timing_field_delta_numerator(void);
uint32_t psx_xenogears_timing_field_delta_denominator(void);
uint32_t psx_xenogears_timing_field_accumulator_step(void);
int psx_xenogears_timing_field_logical_tick(void);
XgFieldTickPhase psx_xenogears_timing_field_tick_phase(void);
const char *psx_xenogears_timing_field_tick_phase_name(void);
int32_t psx_xenogears_timing_field_integrate_velocity(uint32_t, uint32_t, int32_t, int32_t, int32_t);

#ifdef __cplusplus
}
#endif
#endif
