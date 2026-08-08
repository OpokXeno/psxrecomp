#include "xenogears_field_hook.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static uint8_t ram[2u * 1024u * 1024u];
static uint32_t page_generation;
static uint32_t fake_crc = 0xBBB22575u;
static uint32_t crc_calls;
static uint32_t watch_calls;
static int failures;

#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t crc32_compute(const uint8_t *data, size_t size)
{
    CHECK(data == ram + 0x000758C0u && size == 0x60u,
          "CRC covers exact immutable code window");
    crc_calls++;
    return fake_crc;
}
void overlay_watch_set_range(uint32_t address, uint32_t size)
{
    CHECK(address == 0x000758C0u && size == 0x60u,
          "watch covers exact immutable code window");
    watch_calls++;
}
uint32_t overlay_watch_pagegen_sum(uint32_t address, uint32_t size)
{
    CHECK(address == 0x000758C0u && size == 0x60u,
          "page generation covers exact immutable code window");
    return page_generation;
}

static int32_t policy_step = 2;
int32_t psx_xenogears_timing_field_frame_step(
    const XgFieldFrameContext *context, int32_t original_step)
{
    CHECK(context->load_base == 0x8006E800u &&
          context->site_pc == 0x800758E4u &&
          context->instruction_word == 0x24630002u &&
          context->frame_token == 0x12345678u,
          "hook forwards exact resident policy context");
    CHECK(original_step == 2, "hook forwards semantic step two");
    return policy_step;
}
void psx_xenogears_timing_field_invalidate(void) {}

static void test_cache_and_invalidation(void)
{
    psx_xenogears_field_resident_invalidate();
    CHECK(psx_xenogears_field_frame_step(0x800758E4u, 0x24630002u, 2, 0x12345678u,
          XG_FIELD_TIER_COLD_INTERPRETER) == 2, "cold hook uses shared policy");
    CHECK(crc_calls == 1u && watch_calls == 1u, "first hook watches and hashes once");
    policy_step = 1;
    CHECK(psx_xenogears_field_frame_step(0x800758E4u, 0x24630002u, 2, 0x12345678u,
          XG_FIELD_TIER_WARM_NATIVE) == 1, "warm hook uses identical live identity policy");
    CHECK(crc_calls == 1u, "unchanged page generation reuses CRC");
    page_generation++;
    CHECK(psx_xenogears_field_frame_step(0x800758E4u, 0x24630002u, 2, 0x12345678u,
          XG_FIELD_TIER_COLD_INTERPRETER) == 1 && crc_calls == 2u,
          "page generation change refreshes CRC");
    psx_xenogears_field_resident_dma(0x00010000u, 0x100u);
    CHECK(psx_xenogears_field_frame_step(0x800758E4u, 0x24630002u, 2, 0x12345678u,
          XG_FIELD_TIER_COLD_INTERPRETER) == 1 && crc_calls == 2u,
          "non-overlap DMA retains cache");
    psx_xenogears_field_resident_dma(0x0007591Fu, 1u);
    CHECK(psx_xenogears_field_frame_step(0x800758E4u, 0x24630002u, 2, 0x12345678u,
          XG_FIELD_TIER_COLD_INTERPRETER) == 1 && crc_calls == 3u,
          "overlapping DMA invalidates cache");
    fake_crc ^= 1u;
    page_generation++;
    CHECK(psx_xenogears_field_frame_step(0x800758E4u, 0x24630002u, 2, 0x12345678u,
          XG_FIELD_TIER_WARM_NATIVE) == 2,
          "wrong replacement CRC fails closed");
}

int main(void)
{
    test_cache_and_invalidation();
    if (failures) return 1;
    puts("PASS: resident field hook caches live RAM identity safely");
    return 0;
}
