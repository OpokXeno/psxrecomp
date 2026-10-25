#ifndef NATIVE_DEPTH_H
#define NATIVE_DEPTH_H

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* Positive FP32 reciprocal view Z. Keep its bits in the existing R32UI
 * images, CPU planes and capture nodes: no integer quantization or extra
 * storage, and unsigned comparisons preserve the order of positive floats.
 * Zero is far; the canonical plane's unrelated 0/1 provenance is unchanged. */
static inline uint32_t native_depth_encode(float depth) {
    uint32_t bits;
    memcpy(&bits, &depth, sizeof(bits));
    return bits;
}

static inline float native_depth_decode(uint32_t bits) {
    float depth;
    memcpy(&depth, &bits, sizeof(depth));
    return depth;
}

static inline uint32_t native_depth_biased(uint32_t key, unsigned bias) {
    const float factor = bias ? native_depth_decode(UINT32_C(0x3f800000) + (UINT32_C(0x00800000) >> bias)) : 1.0f;
    return native_depth_encode(native_depth_decode(key) * factor);
}

static inline uint32_t native_depth_threshold(uint32_t stored) {
    /* UINT32_MAX is the conservative bounds cache's unknown sentinel. */
    return stored == UINT32_MAX ? stored : native_depth_encode(native_depth_decode(stored) * (1.0f - 0.0009765625f));
}

/* Same explicit rounding as depth_key() in GLSL. Gradients are prepared per
 * physical pixel once per draw; no divides or scale branches per fragment. */
static inline uint32_t native_depth_evaluate(float seed, float dx, float dy, float x, float y) {
    return native_depth_encode(fmaxf(fmaf(dy, y, fmaf(dx, x, seed)), FLT_MIN));
}

#endif
