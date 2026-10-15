#ifndef PSX_NATIVE_PHASE_CADENCE_H
#define PSX_NATIVE_PHASE_CADENCE_H

#include <stdint.h>

/* Enough authored samples for every requested presentation tick. Rounding
 * 75/60 to one sample, or 75/30 to two, silently limits unique images to 60
 * even while the swap clock runs at 75. Allow 1% source-clock noise around
 * exact ratios (59.94 vs 60) without allocating an unnecessary extra phase. */
static inline uint32_t native_phase_denominator(
        uint64_t interval_ns, uint32_t hz, uint32_t capacity) {
    const uint64_t second = UINT64_C(1000000000);
    if (!hz || !capacity) return 1u;
    if (interval_ns >= (uint64_t)capacity * second / hz) return capacity;
    const uint64_t samples = interval_ns * hz;
    uint32_t denominator = (uint32_t)(samples / second);
    if (samples % second > (uint64_t)denominator * UINT64_C(10000000))
        ++denominator;
    return denominator ? denominator : 1u;
}

#endif
