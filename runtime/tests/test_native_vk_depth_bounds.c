#include "native_vk_depth_bounds.h"
#include <assert.h>
#include <stdio.h>

static uint32_t random_state = 0x4713259du;
static uint32_t next(void) { random_state ^= random_state << 13; random_state ^= random_state >> 17; return random_state ^= random_state << 5; }

static void check_precision_and_policy(void)
{
    /* The old integer reciprocal keys collapsed these distant surfaces. */
    const uint32_t near = native_depth_encode(1.0f / 60000.0f), far = native_depth_encode(1.0f / 60001.0f);
    assert((uint32_t)(268435456.0 / 60000.0) == (uint32_t)(268435456.0 / 60001.0));
    assert(near > far && far > 0u);
    assert(native_depth_decode(native_depth_encode(0.25f)) == 0.25f);
    assert(native_depth_threshold(0u) == 0u);
    assert(native_depth_threshold(UINT32_MAX) == UINT32_MAX);
    assert(native_depth_biased(near, 0) == near);
    assert(far >= native_depth_threshold(near)); /* Preserve the OT tie band. */
    assert(native_depth_encode(1.0f / 1100.0f) < native_depth_threshold(native_depth_encode(1.0f / 1000.0f)));
    for (unsigned bias = 1; bias <= 16; ++bias) {
        float expected = (1.0f / 60000.0f) * (1.0f + ldexpf(1.0f, -(int)bias));
        assert(native_depth_biased(near, bias) == native_depth_encode(expected));
    }
    assert(native_depth_decode(native_depth_evaluate(-1.0f, 0, 0, 0, 0)) == FLT_MIN);
}

int main(void)
{
    check_precision_and_policy();
    unsigned pixels = 0, intervals = 0;
    for (int s = 1; s <= 8; ++s) for (unsigned i = 0; i < 12000; ++i) {
        const float seed = ldexpf(1.0f + (float)(next() % 8192u) / 8192.0f, (int)(next() % 25u) - 16);
        const float a = seed * ((int)(next() % 8193u) - 4096) / 1024.0f;
        const float b = seed * ((int)(next() % 8193u) - 4096) / 1024.0f;
        int ox = (int)(next() % 1024) - 512, oy = (int)(next() % 1024) - 512;
        int left = ox * s + (int)(next() % 80) - 40, top = oy * s + (int)(next() % 80) - 40;
        int right = left + 1 + (int)(next() % 12), bottom = top + 1 + (int)(next() % 12);
        uint32_t low, high;
        assert(nvk_depth_interval(seed, a, b, ox, oy, (uint32_t)s, left, top, right, bottom, &low, &high));
        ++intervals;
        for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
            const float dx = a / s, dy = b / s;
            const float px = (float)(x - ox * s), py = (float)(y - oy * s);
            uint32_t key = native_depth_evaluate(seed, dx, dy, px, py);
            assert(key >= low && key <= high);
            /* Separate operations, as on a GL 3.3 implementation without FMA. */
            volatile float product = dx * px, sum = seed + product, product_y = dy * py;
            key = native_depth_encode(fmaxf(sum + product_y, FLT_MIN));
            assert(key >= low && key <= high);
            ++pixels;
        }
    }
    assert(intervals == 96000);
    uint32_t low, high;
    assert(!nvk_depth_interval(INFINITY, 0, 0, 0, 0, 4, 4, 0, 8, 4, &low, &high));
    assert(!nvk_depth_interval(1, NAN, 0, 0, 0, 4, 4, 0, 8, 4, &low, &high));
    assert(!nvk_depth_interval(1, 0, 0, 0, 0, 0, 4, 0, 8, 4, &low, &high));
    assert(!nvk_depth_interval(FLT_MAX, FLT_MAX, 0, 0, 0, 4, 4, 0, 8, 4, &low, &high));
    printf("PASS FP32 precision/policy, %u intervals, %u shader-depth samples (FMA and separate operations)\n", intervals, pixels);
}
