#pragma once

#include <stdint.h>

/* Hardware UNR reciprocal approximation used by RTPS/RTPT. Keeping this
 * header C-compatible lets native producers and temporal reprojection use the
 * same arithmetic as the GTE core. */
static uint8_t psx_gte_divide_table[0x101];
static int psx_gte_divide_table_ready;

static inline void psx_gte_initialize_divide_table(void) {
    uint32_t divisor;

    if (psx_gte_divide_table_ready) return;
    for (divisor = 0x8000u; divisor < 0x10000u; divisor += 0x80u) {
        uint32_t approximation = 512u;

        for (unsigned int iteration = 1u; iteration < 5u; ++iteration)
            approximation =
                (approximation *
                 (1024u * 512u - ((divisor >> 7u) * approximation))) >>
                18u;
        psx_gte_divide_table[(divisor >> 7u) & 0xffu] =
            (uint8_t)(((approximation + 1u) >> 1u) - 0x101u);
    }
    psx_gte_divide_table[0x100] = psx_gte_divide_table[0xff];
    psx_gte_divide_table_ready = 1;
}

static inline unsigned int psx_gte_clz16(uint16_t value) {
    unsigned int count = 0u;

    for (int bit = 15; bit >= 0; --bit) {
        if ((value & (uint16_t)(1u << bit)) != 0u) break;
        ++count;
    }
    return count;
}

static inline int32_t psx_gte_reciprocal(uint16_t divisor) {
    const int32_t x = 0x101 + psx_gte_divide_table[
        (((divisor & 0x7fffu) + 0x40u) >> 7u)];
    const int32_t first = ((int32_t)divisor * -x + 0x80) >> 8;

    return (x * (131072 + first) + 0x80) >> 8;
}

static inline int32_t psx_gte_divide(
        uint16_t projection_distance, uint16_t depth,
        int *out_overflow) {
    unsigned int shift;
    uint32_t dividend;
    uint32_t divisor;
    uint32_t result;

    psx_gte_initialize_divide_table();
    if ((uint32_t)depth * 2u <= (uint32_t)projection_distance) {
        if (out_overflow != 0) *out_overflow = 1;
        return 0x1ffff;
    }
    if (out_overflow != 0) *out_overflow = 0;
    shift = psx_gte_clz16(depth);
    dividend = (uint32_t)projection_distance << shift;
    divisor = (uint32_t)depth << shift;
    result = (uint32_t)(((uint64_t)dividend *
                         (uint32_t)psx_gte_reciprocal(
                             (uint16_t)(divisor | 0x8000u)) +
                         32768u) >>
                        16u);
    if (result > 0x1ffffu) result = 0x1ffffu;
    return (int32_t)result;
}
