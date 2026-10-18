#ifndef NATIVE_VK_DEPTH_BOUNDS_H
#define NATIVE_VK_DEPTH_BOUNDS_H
#include <stdint.h>
#include <limits.h>

static inline int64_t nvk_depth_floor_div(int64_t n, int64_t d)
{
    return n >= 0 ? n / d : -((d - 1 - n) / d);
}
static inline int64_t nvk_depth_floor_shift(int64_t n, unsigned shift)
{
    return n >= 0 ? n >> shift : -(((-n) + ((INT64_C(1) << shift) - 1)) >> shift);
}

/* Conservative extrema of the shader's integer depth key over a physical
 * pixel rectangle. Reject intervals where the shader's intermediate int32
 * accumulator can wrap; those triangles retain the ordinary depth read. */
static inline int nvk_depth_interval(int32_t n0, int32_t a, int32_t b, int32_t ox, int32_t oy,
                                     uint32_t scale, int left, int top, int right, int bottom,
                                     uint32_t *low, uint32_t *high)
{
    if ((scale != 1u && scale != 2u && scale != 4u && scale != 8u) || left >= right || top >= bottom ||
        a < -(1 << 22) || a > (1 << 22) || b < -(1 << 22) || b > (1 << 22)) return 0;
    uint32_t minimum = UINT32_MAX, maximum = 0u;
    const unsigned shift = scale == 1u ? 0u : scale == 2u ? 1u : scale == 4u ? 2u : 3u;
    for (unsigned corner = 0u; corner < 4u; ++corner) {
        const int64_t dx = (corner & 1u ? right - 1 : left) - (int64_t)ox * scale;
        const int64_t dy = (corner & 2u ? bottom - 1 : top) - (int64_t)oy * scale;
        const int64_t lx = nvk_depth_floor_shift(dx, shift), ly = nvk_depth_floor_shift(dy, shift);
        const int64_t n = (int64_t)n0 + (int64_t)a * lx + (int64_t)b * ly;
        if (n < INT32_MIN || n > INT32_MAX) return 0;
        const int64_t numerator = n * scale + (int64_t)a * (dx - lx * scale) + (int64_t)b * (dy - ly * scale);
        int64_t key = nvk_depth_floor_shift(numerator, 8u + shift);
        if (key < 1) key = 1;
        if (key > UINT32_MAX / 2u) return 0;
        if ((uint32_t)key < minimum) minimum = (uint32_t)key;
        if ((uint32_t)key > maximum) maximum = (uint32_t)key;
    }
    *low = minimum; *high = maximum;
    return 1;
}
#endif
