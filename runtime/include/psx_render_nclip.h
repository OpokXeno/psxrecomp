#ifndef PSX_RENDER_NCLIP_H
#define PSX_RENDER_NCLIP_H

#include <stdint.h>
#include <limits.h>

/* Presentation-side orientation using the same Q16 positions as the raster.
 * Keep the hardware area when its sign agrees. A fractional positive area must
 * not round to zero and disappear. No floating-point tolerance is involved. */
static inline int32_t psx_render_nclip(int32_t hardware_area,
                                      const int32_t x[3], const int32_t y[3]) {
    const int64_t ax = (int64_t)x[1] - x[0], ay = (int64_t)y[1] - y[0];
    const int64_t bx = (int64_t)x[2] - x[0], by = (int64_t)y[2] - y[0];
    /* Two signed products and their difference fit int64 inside this domain. */
    if (ax < -INT32_MAX || ax > INT32_MAX || ay < -INT32_MAX || ay > INT32_MAX ||
        bx < -INT32_MAX || bx > INT32_MAX || by < -INT32_MAX || by > INT32_MAX)
        return hardware_area;
    const int64_t area = ax * by - ay * bx;
    if (!area) return 0;
    if (area > 0) return hardware_area > 0 ? hardware_area : 1;
    return hardware_area < 0 ? hardware_area : -1;
}

#endif
