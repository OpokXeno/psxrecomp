#include "xenogears_timing.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RAM_MASK 0x1FFFFFu

static uint8_t ram[RAM_MASK + 1u];
static int failures;

#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

uint8_t psx_read_byte(uint32_t address) { return ram[address & RAM_MASK]; }

static void put_u16(uint32_t address, uint16_t value)
{
    ram[address & RAM_MASK] = (uint8_t)value;
    ram[(address + 1u) & RAM_MASK] = (uint8_t)(value >> 8);
}

static void put_u32(uint32_t address, uint32_t value)
{
    put_u16(address, (uint16_t)value);
    put_u16(address + 2u, (uint16_t)(value >> 16));
}

static void setup(NativeFpsMode mode)
{
    const NativeFpsStartupPolicy policy = {
        mode, mode == NATIVE_FPS_MODE_NATIVE_59_94
            ? NATIVE_FPS_STARTUP_EXPLICIT : NATIVE_FPS_STARTUP_ORIGINAL_DEFAULT,
        0
    };
    memset(ram, 0, sizeof(ram));
    psx_xenogears_timing_reset(XG_TIMING_REASON_BOOT);
    psx_xenogears_timing_set_startup_policy(&policy);
    psx_xenogears_timing_reset(XG_TIMING_REASON_BOOT);
}

static void seed_live_field(uint32_t context, uint16_t field_id)
{
    put_u32(0x800B0078u, context);
    put_u32(0x80018088u, 0u);
    put_u32(0x800592C0u, 0xFFFFFFFFu);
    put_u32(0x800592BCu, 0u);
    put_u16(0x8006F94Eu, field_id);
    put_u16(0x8006EF64u, 7u);
}

static XgFieldFrameContext exact_context(XgFieldExecutionTier tier)
{
    XgFieldFrameContext context = {
        0x8006E800u, 0xBBB22575u, 0x800758E4u, 0x24630002u, 10u, 77u, tier
    };
    return context;
}

static void test_live_scene_arms_candidate(void)
{
    XgTimingScene scene;
    setup(NATIVE_FPS_MODE_NATIVE_59_94);
    seed_live_field(0x80119728u, 5u);
    psx_xenogears_timing_read_scene(&scene);
    CHECK(scene.valid_field, "live tuple is a valid field candidate");
    CHECK(scene.raw_field_id == 5u && scene.masked_field_id == 5u &&
          scene.game_progress == 7u,
          "scene preserves authenticated raw and masked field state with GameProgress");
    psx_xenogears_timing_vblank_boundary(0);
    CHECK(psx_xenogears_timing_route() == XG_TIMING_ROUTE_FIELD_DEV &&
          psx_xenogears_timing_effective_mode() == NATIVE_FPS_MODE_ORIGINAL,
          "candidate waits for exact resident helper");
    seed_live_field(0x80119729u, 5u);
    psx_xenogears_timing_read_scene(&scene);
    CHECK(!scene.valid_field, "unaligned context fails closed");
    seed_live_field(0x80119728u, 0x0400u);
    psx_xenogears_timing_read_scene(&scene);
    CHECK(!scene.valid_field, "invalid field ID fails closed");
}

static void test_resident_policy_table(void)
{
    XgFieldFrameContext exact = exact_context(XG_FIELD_TIER_COLD_INTERPRETER);
    setup(NATIVE_FPS_MODE_ORIGINAL);
    seed_live_field(0x80119728u, 5u);
    psx_xenogears_timing_vblank_boundary(0);
    CHECK(psx_xenogears_timing_field_frame_step(&exact, 2) == 2 &&
          psx_xenogears_timing_field_frame_step(&exact, 2) == 2 &&
          psx_xenogears_timing_field_authored_updates() == 1u &&
          !psx_xenogears_timing_field_route_active(),
          "Original counts one exact authored frame and keeps step two");

    setup(NATIVE_FPS_MODE_NATIVE_59_94);
    seed_live_field(0x80119728u, 5u);
    psx_xenogears_timing_vblank_boundary(0);
    XgFieldFrameContext invalid[] = { exact, exact, exact, exact, exact, exact };
    invalid[0].load_base ^= 4u;
    invalid[1].logical_identity ^= 1u;
    invalid[2].site_pc += 4u;
    invalid[3].instruction_word ^= 1u;
    invalid[4].tier = (XgFieldExecutionTier)9;
    for (size_t index = 0; index < 5u; index++)
        CHECK(psx_xenogears_timing_field_frame_step(&invalid[index], 2) == 2,
              "invalid resident policy input fails closed");
    CHECK(psx_xenogears_timing_field_frame_step(&invalid[5], 3) == 3,
          "wrong original step fails closed");
    CHECK(psx_xenogears_timing_field_authored_updates() == 0u,
          "invalid inputs do not mark authored updates");
    CHECK(psx_xenogears_timing_field_frame_step(&exact, 2) == 1 &&
          psx_xenogears_timing_field_frame_step(&exact, 2) == 1 &&
          psx_xenogears_timing_effective_mode() == NATIVE_FPS_MODE_NATIVE_59_94 &&
          psx_xenogears_timing_field_helper_activations() == 1u &&
          psx_xenogears_timing_field_authored_updates() == 1u,
          "same-token Native polls activate but count once");
    exact.guest_vblank++;
    CHECK(psx_xenogears_timing_field_frame_step(&exact, 2) == 1 &&
          psx_xenogears_timing_field_helper_activations() == 1u &&
          psx_xenogears_timing_field_authored_updates() == 1u,
          "same frame token stays deduplicated across a guest VBlank");
    exact.frame_token++;
    CHECK(psx_xenogears_timing_field_frame_step(&exact, 2) == 1 &&
          psx_xenogears_timing_field_helper_activations() == 2u &&
          psx_xenogears_timing_field_authored_updates() == 2u,
          "next frame token permits exactly one next count");
    psx_xenogears_timing_field_invalidate();
    CHECK(psx_xenogears_timing_effective_mode() == NATIVE_FPS_MODE_ORIGINAL,
          "resident lifecycle invalidation disables Native");
}

int main(void)
{
    test_live_scene_arms_candidate();
    test_resident_policy_table();
    if (failures) return 1;
    puts("PASS: resident field frame policy is exact and fail-closed");
    return 0;
}
