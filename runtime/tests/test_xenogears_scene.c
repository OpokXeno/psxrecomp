#include "xenogears_scene.h"

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

static void seed_live_field(uint32_t context, uint16_t field_id)
{
    put_u32(0x800B0078u, context);
    put_u32(0x80018088u, 0u);
    put_u32(0x800592C0u, 0xFFFFFFFFu);
    put_u32(0x800592BCu, 0u);
    put_u16(0x8006F94Eu, field_id);
    put_u16(0x8006EF64u, 7u);
}

static void test_scene_read_and_generation(void)
{
    XgScene scene;

    memset(ram, 0, sizeof(ram));
    psx_xenogears_scene_reset();
    seed_live_field(0x80119728u, 5u);
    psx_xenogears_read_scene(&scene);
    CHECK(scene.valid_field, "live tuple is a valid field candidate");
    CHECK(scene.raw_field_id == 5u && scene.masked_field_id == 5u &&
          scene.game_progress == 7u,
          "scene preserves field state with GameProgress");
    psx_xenogears_scene_vblank_boundary(0);
    CHECK(psx_xenogears_scene_generation() == 1u,
          "first scene advances the generation");
    psx_xenogears_scene_vblank_boundary(0);
    CHECK(psx_xenogears_scene_generation() == 1u,
          "same scene does not advance the generation");
    seed_live_field(0x8011972Cu, 6u);
    psx_xenogears_scene_vblank_boundary(0);
    CHECK(psx_xenogears_scene_generation() == 2u,
          "field transition advances the generation");
    seed_live_field(0x80119729u, 5u);
    psx_xenogears_read_scene(&scene);
    CHECK(!scene.valid_field, "unaligned context fails closed");
    seed_live_field(0x80119728u, 0x0400u);
    psx_xenogears_read_scene(&scene);
    CHECK(!scene.valid_field, "invalid field ID fails closed");
}

int main(void)
{
    test_scene_read_and_generation();
    if (failures) return 1;
    puts("PASS: Xenogears scene tracking is independent of render timing");
    return 0;
}
