#ifndef PSXRECOMP_GAME_FPS_METER_H
#define PSXRECOMP_GAME_FPS_METER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t draw, fill, copy, env;
} GameFpsGp0Stats;

typedef enum {
    GAME_FPS_SOURCE_GP0 = 0,
    GAME_FPS_SOURCE_FIELD_HOOK = 1,
} GameFpsSource;

typedef struct {
    double game_fps, video_fps, sample_seconds;
    uint64_t game_updates, video_presents;
    uint64_t total_game_updates, total_video_presents;
    GameFpsSource game_fps_source;
    uint8_t warmed, valid;
} GameFpsSnapshot;

void game_fps_meter_reset(uint64_t now, uint64_t frequency,
                          const GameFpsGp0Stats *initial_gp0,
                          uint64_t field_hook_updates);
void game_fps_meter_record_vblank(uint64_t now,
                                  const GameFpsGp0Stats *gp0,
                                  uint64_t field_hook_updates);
static inline const char *game_fps_source_name(GameFpsSource source)
{
    return source == GAME_FPS_SOURCE_FIELD_HOOK ? "field_hook" : "gp0";
}
void game_fps_meter_record_present(uint64_t now);
void game_fps_meter_snapshot(uint64_t now, GameFpsSnapshot *out);
int game_fps_meter_log_due(uint64_t now, uint64_t state_key,
                           GameFpsSnapshot *out);

#ifdef __cplusplus
}
#endif
#endif
