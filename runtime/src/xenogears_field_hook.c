#include "xenogears_field_hook.h"
#include "crc32.h"

#include <stddef.h>

enum {
    XG_RESIDENT_BASE = 0x8006E800u,
    XG_IDENTITY_PHYS = 0x000758C0u,
    XG_IDENTITY_SIZE = 0x60u,
    XG_IDENTITY_END = 0x00075920u,
    XG_IDENTITY_CRC = 0xBBB22575u,
};

extern uint8_t *memory_get_ram_ptr(void);
extern void overlay_watch_set_range(uint32_t phys, uint32_t len);
extern uint32_t overlay_watch_pagegen_sum(uint32_t phys, uint32_t len);

static uint32_t s_crc;
static uint32_t s_generation;
static uint8_t s_cached;

static uint32_t resident_crc(void)
{
    const uint32_t generation = overlay_watch_pagegen_sum(XG_IDENTITY_PHYS, XG_IDENTITY_SIZE);
    if (!s_cached || generation != s_generation) {
        overlay_watch_set_range(XG_IDENTITY_PHYS, XG_IDENTITY_SIZE);
        s_crc = crc32_compute(memory_get_ram_ptr() + XG_IDENTITY_PHYS, XG_IDENTITY_SIZE);
        s_generation = overlay_watch_pagegen_sum(XG_IDENTITY_PHYS, XG_IDENTITY_SIZE);
        s_cached = 1u;
    }
    return s_crc;
}

int32_t psx_xenogears_field_frame_step(uint32_t site_pc, uint32_t instruction_word,
                                       int32_t original_step, uint32_t frame_token,
                                       uint32_t tier)
{
    XgFieldFrameContext context;
    if (site_pc != 0x800758E4u || instruction_word != 0x24630002u ||
        original_step != 2 || (tier != XG_FIELD_TIER_COLD_INTERPRETER &&
        tier != XG_FIELD_TIER_WARM_NATIVE)) return original_step;
    context.load_base = XG_RESIDENT_BASE;
    context.logical_identity = resident_crc();
    context.site_pc = site_pc;
    context.instruction_word = instruction_word;
    context.guest_vblank = 0u;
    context.frame_token = frame_token;
    context.tier = (XgFieldExecutionTier)tier;
    if (context.logical_identity != XG_IDENTITY_CRC) return original_step;
    return psx_xenogears_timing_field_frame_step(&context, original_step);
}

void psx_xenogears_field_resident_invalidate(void)
{
    s_cached = 0u;
    psx_xenogears_timing_field_invalidate();
}

void psx_xenogears_field_resident_dma(uint32_t load_addr, uint32_t size)
{
    const uint32_t lo = load_addr & 0x1FFFFFFFu;
    const uint64_t hi = (uint64_t)lo + size;
    if (size != 0u && hi > XG_IDENTITY_PHYS && lo < XG_IDENTITY_END)
        psx_xenogears_field_resident_invalidate();
}
