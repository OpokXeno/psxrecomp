#include "game_fps_meter.h"
#include <stdio.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)
static int near(double actual, double expected) { double d = actual - expected; return d >= -0.01 && d <= 0.01; }

static void run_field_hook_cadence(uint64_t hook_updates, double expected_game)
{
    GameFpsGp0Stats gp0 = {0};
    game_fps_meter_reset(0u, 1000u, &gp0, 0u);
    for (uint32_t index = 1u; index <= 60u; index++) {
        gp0.draw++;
        const uint64_t authored = hook_updates * index / 60u;
        const uint64_t now = (uint64_t)index * 1000u / 60u;
        game_fps_meter_record_vblank(now, &gp0, authored);
        game_fps_meter_record_present(now);
    }
    GameFpsSnapshot snapshot;
    game_fps_meter_snapshot(1000u, &snapshot);
    CHECK(near(snapshot.game_fps, expected_game) && near(snapshot.video_fps, 60.0),
          "field hooks authoritatively separate Game and Video FPS");
    CHECK(snapshot.game_fps_source == GAME_FPS_SOURCE_FIELD_HOOK,
          "certified field sample reports field_hook source");
}

static void test_gp0_fallback(void)
{
    GameFpsGp0Stats gp0 = {0};
    game_fps_meter_reset(0u, 1000u, &gp0, 0u);
    for (uint32_t index = 1u; index <= 60u; index++) {
        if ((index & 1u) == 0u) gp0.draw++;
        const uint64_t now = (uint64_t)index * 1000u / 60u;
        game_fps_meter_record_vblank(now, &gp0, 0u);
        game_fps_meter_record_present(now);
    }
    GameFpsSnapshot snapshot;
    game_fps_meter_snapshot(1000u, &snapshot);
    CHECK(near(snapshot.game_fps, 30.0) && near(snapshot.video_fps, 60.0),
          "GP0 fallback remains intact outside certified fields");
    CHECK(snapshot.game_fps_source == GAME_FPS_SOURCE_GP0,
          "fallback sample reports gp0 source");
}

int main(void)
{
    run_field_hook_cadence(30u, 30.0);
    run_field_hook_cadence(60u, 60.0);
    test_gp0_fallback();
    if (failures) return 1;
    puts("PASS: game/video FPS meter preserves field-hook authority and GP0 fallback");
    return 0;
}
