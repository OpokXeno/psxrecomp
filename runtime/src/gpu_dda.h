/* gpu_dda.h — the PS1 polygon interpolant plane (Beetle/Mednafen DrawTriangle).
 *
 * Hardware-exact U/V and Gouraud R/G/B for an integer-vertex triangle are not
 * the real-valued linear interpolation a GL/VK rasterizer computes. The GPU
 * works in unsigned Q24 accumulators:
 *
 *   delta_x = trunc(CALCIS(attr, y) * 4096 / denom) << 12      (u32)
 *   delta_y = trunc(CALCIS(x, attr) * 4096 / denom) << 12
 *   seed    = ((attr[core] << 12) + 0x800) << 12               (+1/2 unit)
 *   value(x, y) = (seed + delta_x * (x - x_core) + delta_y * (y - y_core)) >> 24
 *
 * where `core` is the vertex chosen before the Y sort (Beetle's asymmetric X
 * tie rules) and every multiply wraps modulo 2^32. The slope truncation and
 * the 1/2 seed mean the 8-bit result differs from floor/round of the exact
 * plane by one in a data-dependent minority of pixels; a rasterizer that
 * interpolates in floats picks a different texel/colour there.
 *
 * psx_dda_triangle() folds the core offset into an origin-relative plane so a
 * fragment at absolute VRAM pixel (x, y) evaluates
 *   (base + dx * (u32)x + dy * (u32)y) >> 24
 * with plain u32 arithmetic (identical in C and GLSL `uint`). */
#ifndef PSXRECOMP_GPU_DDA_H
#define PSXRECOMP_GPU_DDA_H

#include <stdint.h>

typedef struct PsxDdaPlane { uint32_t base, dx, dy; } PsxDdaPlane;

/* x/y: integer vertex positions with the draw offset applied. val: `count`
 * attribute channels (each 0..255 at the three vertices). Returns 0 for a
 * zero-area triangle (the GPU draws nothing). */
static inline int psx_dda_triangle(const int x[3], const int y[3],
                                   const int val[][3], unsigned count,
                                   PsxDdaPlane *out) {
    const int64_t denom = (int64_t)(x[1] - x[0]) * (y[2] - y[1]) -
                          (int64_t)(x[2] - x[1]) * (y[1] - y[0]);
    unsigned core;
    if (!denom) return 0;
    core = x[1] <= x[0] ? (x[2] <= x[1] ? 2u : 1u) : (x[2] < x[0] ? 2u : 0u);
    for (unsigned c = 0; c < count; ++c) {
        const int64_t nx = (int64_t)(val[c][1] - val[c][0]) * (y[2] - y[1]) -
                           (int64_t)(val[c][2] - val[c][1]) * (y[1] - y[0]);
        const int64_t ny = (int64_t)(x[1] - x[0]) * (val[c][2] - val[c][1]) -
                           (int64_t)(x[2] - x[1]) * (val[c][1] - val[c][0]);
        /* C division truncates toward zero, as the int32 original does. */
        const uint32_t dx = (uint32_t)(int32_t)(nx * 4096 / denom) << 12;
        const uint32_t dy = (uint32_t)(int32_t)(ny * 4096 / denom) << 12;
        const uint32_t seed = (((uint32_t)val[c][core] << 12) + 0x800u) << 12;
        out[c].dx = dx;
        out[c].dy = dy;
        out[c].base = seed - dx * (uint32_t)x[core] - dy * (uint32_t)y[core];
    }
    return 1;
}

/* ---- Lines (Beetle/Mednafen DrawLine) ------------------------------------
 * A line is NOT a Bresenham or a GL_LINES rasterization: k = max(|dx|,|dy|)
 * and exactly k+1 pixels are plotted (both endpoints), stepping a Q32 DDA that
 * starts half a pixel in (x is biased by -1024/2^32, y too when it decreases),
 * with per-step deltas rounded away from zero, after swapping so x0 <= x1.
 * Gouraud channels are Q12 with a 1/2 seed and int32-truncated per-step deltas.
 * The result is the ground truth both rasterizers must reproduce. */
typedef struct PsxLineWalker {
    uint64_t x, y;
    int64_t dx_dk, dy_dk;
    uint32_t r, g, b;
    int32_t dr_dk, dg_dk, db_dk;
    int32_t remaining;      /* pixels still to emit */
    int gouraud;
    int flat_r, flat_g, flat_b;
} PsxLineWalker;

static inline int64_t psx_line_divide(int64_t delta, int32_t dk) {
    delta = (int64_t)((uint64_t)delta << 32);
    if (delta < 0) delta -= dk - 1;
    if (delta > 0) delta += dk - 1;
    return delta / dk;
}

/* Endpoints with the draw offset applied; colours 0..255. */
static inline void psx_line_begin(PsxLineWalker *w, int x0, int y0, int r0, int g0, int b0,
                                  int x1, int y1, int r1, int g1, int b1, int gouraud) {
    int32_t dxa = x1 > x0 ? x1 - x0 : x0 - x1, dya = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t k = dxa > dya ? dxa : dya;
    if (x0 > x1 && k) {
        int t;
        t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t;
        t = r0; r0 = r1; r1 = t; t = g0; g0 = g1; g1 = t; t = b0; b0 = b1; b1 = t;
    }
    w->gouraud = gouraud;
    w->flat_r = r0; w->flat_g = g0; w->flat_b = b0;
    w->remaining = k + 1;
    if (!k) { w->dx_dk = w->dy_dk = 0; w->dr_dk = w->dg_dk = w->db_dk = 0; }
    else {
        w->dx_dk = psx_line_divide((int64_t)x1 - x0, k);
        w->dy_dk = psx_line_divide((int64_t)y1 - y0, k);
        w->dr_dk = (int32_t)((uint32_t)(r1 - r0) << 12) / k;
        w->dg_dk = (int32_t)((uint32_t)(g1 - g0) << 12) / k;
        w->db_dk = (int32_t)((uint32_t)(b1 - b0) << 12) / k;
    }
    w->x = (((uint64_t)(int64_t)x0 << 32) | (UINT64_C(1) << 31)) - 1024;
    w->y = ((uint64_t)(int64_t)y0 << 32) | (UINT64_C(1) << 31);
    if (w->dy_dk < 0) w->y -= 1024;
    w->r = ((uint32_t)r0 << 12) | (1u << 11);
    w->g = ((uint32_t)g0 << 12) | (1u << 11);
    w->b = ((uint32_t)b0 << 12) | (1u << 11);
}

/* Next pixel: x,y are the 11-bit wrapped hardware coordinates (clip test
 * against the draw area is the caller's). Returns 0 when the line is done. */
static inline int psx_line_next(PsxLineWalker *w, int *x, int *y, int *r, int *g, int *b) {
    if (w->remaining <= 0) return 0;
    *x = (int)((w->x >> 32) & 2047);
    *y = (int)((w->y >> 32) & 2047);
    if (w->gouraud) {
        *r = (int)((uint8_t)(w->r >> 12));
        *g = (int)((uint8_t)(w->g >> 12));
        *b = (int)((uint8_t)(w->b >> 12));
    } else { *r = w->flat_r; *g = w->flat_g; *b = w->flat_b; }
    w->x += (uint64_t)w->dx_dk;
    w->y += (uint64_t)w->dy_dk;
    w->r += (uint32_t)w->dr_dk; w->g += (uint32_t)w->dg_dk; w->b += (uint32_t)w->db_dk;
    --w->remaining;
    return 1;
}

#endif
