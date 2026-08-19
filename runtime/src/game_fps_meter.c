#include "game_fps_meter.h"

#include "psx_sdl.h"
#include <string.h>

typedef struct {
    uint64_t frequency, window_start, productive_gp0, field_hook_updates;
    uint64_t window_game_updates, window_field_updates, window_video_presents;
    uint64_t total_game_updates, total_video_presents, total_field_updates;
    uint64_t last_log_time, last_log_state;
    GameFpsSnapshot published;
    uint8_t warmed, have_log_state, have_published;
} GameFpsMeter;

static GameFpsMeter s_meter;
static SDL_SpinLock s_meter_lock;

static uint64_t productive_total(const GameFpsGp0Stats *gp0)
{
    return gp0 ? gp0->draw + gp0->fill + gp0->copy : 0u;
}

static void snapshot_locked(uint64_t now, GameFpsSnapshot *out)
{
    const uint64_t elapsed = now >= s_meter.window_start ? now - s_meter.window_start : 0u;
    const int valid = s_meter.frequency != 0u && elapsed >= s_meter.frequency;
    const double seconds = s_meter.frequency != 0u
        ? (double)elapsed / (double)s_meter.frequency : 0.0;
    memset(out, 0, sizeof(*out));
    out->sample_seconds = seconds;
    out->game_fps_source = s_meter.window_field_updates != 0u
        ? GAME_FPS_SOURCE_FIELD_HOOK : GAME_FPS_SOURCE_GP0;
    out->game_updates = out->game_fps_source == GAME_FPS_SOURCE_FIELD_HOOK
        ? s_meter.window_field_updates : s_meter.window_game_updates;
    out->video_presents = s_meter.window_video_presents;
    out->total_game_updates = out->game_fps_source == GAME_FPS_SOURCE_FIELD_HOOK
        ? s_meter.total_field_updates : s_meter.total_game_updates;
    out->total_video_presents = s_meter.total_video_presents;
    out->warmed = s_meter.warmed;
    out->valid = (uint8_t)valid;
    if (seconds > 0.0) {
        out->game_fps = (double)out->game_updates / seconds;
        out->video_fps = (double)out->video_presents / seconds;
    }
    if (!out->valid && s_meter.have_published) {
        *out = s_meter.published;
        out->total_game_updates = out->game_fps_source == GAME_FPS_SOURCE_FIELD_HOOK
            ? s_meter.total_field_updates : s_meter.total_game_updates;
        out->total_video_presents = s_meter.total_video_presents;
    }
}

void game_fps_meter_reset(uint64_t now, uint64_t frequency,
                          const GameFpsGp0Stats *initial_gp0,
                          uint64_t field_hook_updates)
{
    SDL_AtomicLock(&s_meter_lock);
    memset(&s_meter, 0, sizeof(s_meter));
    s_meter.frequency = frequency;
    s_meter.window_start = now;
    s_meter.productive_gp0 = productive_total(initial_gp0);
    s_meter.field_hook_updates = field_hook_updates;
    s_meter.warmed = 1u;
    SDL_AtomicUnlock(&s_meter_lock);
}

void game_fps_meter_record_vblank(uint64_t now, const GameFpsGp0Stats *gp0,
                                  uint64_t field_hook_updates)
{
    const uint64_t productive = productive_total(gp0);
    SDL_AtomicLock(&s_meter_lock);
    if (!s_meter.warmed) {
        s_meter.window_start = now;
        s_meter.productive_gp0 = productive;
        s_meter.warmed = 1u;
    } else if (productive > s_meter.productive_gp0) {
        s_meter.window_game_updates++;
        s_meter.total_game_updates++;
        s_meter.productive_gp0 = productive;
    } else if (productive < s_meter.productive_gp0) {
        s_meter.productive_gp0 = productive;
    }
    if (field_hook_updates > s_meter.field_hook_updates) {
        const uint64_t updates = field_hook_updates - s_meter.field_hook_updates;
        s_meter.window_field_updates += updates;
        s_meter.total_field_updates += updates;
    }
    s_meter.field_hook_updates = field_hook_updates;
    SDL_AtomicUnlock(&s_meter_lock);
}

void game_fps_meter_record_present(uint64_t now)
{
    (void)now;
    SDL_AtomicLock(&s_meter_lock);
    if (s_meter.warmed) {
        s_meter.window_video_presents++;
        s_meter.total_video_presents++;
    }
    SDL_AtomicUnlock(&s_meter_lock);
}

void game_fps_meter_snapshot(uint64_t now, GameFpsSnapshot *out)
{
    if (!out) return;
    SDL_AtomicLock(&s_meter_lock);
    snapshot_locked(now, out);
    SDL_AtomicUnlock(&s_meter_lock);
}

int game_fps_meter_log_due(uint64_t now, uint64_t state_key, GameFpsSnapshot *out)
{
    int due = 0, periodic = 0;
    GameFpsSnapshot local_snapshot;
    GameFpsSnapshot *snapshot = out ? out : &local_snapshot;
    SDL_AtomicLock(&s_meter_lock);
    if (!s_meter.have_log_state || s_meter.last_log_state != state_key) {
        s_meter.last_log_state = state_key;
        s_meter.have_log_state = 1u;
        s_meter.last_log_time = now;
        due = 1;
    } else if (s_meter.frequency != 0u && now >= s_meter.last_log_time &&
               now - s_meter.last_log_time >= s_meter.frequency) {
        s_meter.last_log_time = now;
        due = periodic = 1;
    }
    snapshot_locked(now, snapshot);
    if (periodic) {
        s_meter.published = *snapshot;
        s_meter.have_published = 1u;
        s_meter.window_start = now;
        s_meter.window_game_updates = 0u;
        s_meter.window_field_updates = 0u;
        s_meter.window_video_presents = 0u;
    }
    SDL_AtomicUnlock(&s_meter_lock);
    return due;
}
