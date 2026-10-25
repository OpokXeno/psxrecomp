#ifndef NATIVE_VK_DEPTH_BOUNDS_H
#define NATIVE_VK_DEPTH_BOUNDS_H
#include "native_depth.h"

/* Conservative extrema of the FP32 shader plane over a physical rectangle.
 * Use the signs of the slopes to select two extreme corners, evaluate in
 * double and widen for FP32 rounding (including separate mul/add on GL 3.3).
 * This CPU scheduling path needs neither per-corner fmaf nor nextafter calls.
 * An uncertain interval retains the ordinary per-fragment depth read. */
static inline int nvk_depth_interval(float seed, float a, float b, int32_t ox, int32_t oy,
                                     uint32_t scale, int left, int top, int right, int bottom,
                                     uint32_t *low, uint32_t *high)
{
    if (!scale || scale > 8u || left >= right || top >= bottom ||
        !isfinite(seed) || !isfinite(a) || !isfinite(b)) return 0;
    /* Round the physical gradients exactly as the uploaded FP32 parameters. */
    const double dx = (float)(a / (float)scale), dy = (float)(b / (float)scale);
    const double x0 = (int64_t)left - (int64_t)ox * scale, x1 = (int64_t)right - 1 - (int64_t)ox * scale;
    const double y0 = (int64_t)top - (int64_t)oy * scale, y1 = (int64_t)bottom - 1 - (int64_t)oy * scale;
    const double minimum = seed + dx * (dx >= 0.0 ? x0 : x1) + dy * (dy >= 0.0 ? y0 : y1);
    const double maximum = seed + dx * (dx >= 0.0 ? x1 : x0) + dy * (dy >= 0.0 ? y1 : y0);
    const double ax = fabs(x0) > fabs(x1) ? fabs(x0) : fabs(x1);
    const double ay = fabs(y0) > fabs(y1) ? fabs(y0) : fabs(y1);
    const double error = 8.0 * FLT_EPSILON * (fabs((double)seed) + fabs(dx) * ax + fabs(dy) * ay);
    const double lower = minimum - error, upper = maximum + error;
    if (upper >= FLT_MAX) return 0;
    uint32_t lo = native_depth_encode(lower > FLT_MIN ? (float)lower : FLT_MIN);
    uint32_t hi = native_depth_encode(upper > FLT_MIN ? (float)upper : FLT_MIN);
    /* Outward by one FP32 ULP, without libm calls in this scheduling loop. */
    if (lo > UINT32_C(0x00800000)) --lo;
    if (hi >= UINT32_C(0x7f7fffff)) return 0;
    *low = lo; *high = hi + 1u;
    return 1;
}
#endif
