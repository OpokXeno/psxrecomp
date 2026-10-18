#include "native_vk_depth_bounds.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t random_state = 0x4713259du;
static uint32_t next(void) { random_state ^= random_state << 13; random_state ^= random_state >> 17; return random_state ^= random_state << 5; }
static uint32_t shader_key(int32_t n0, int32_t a, int32_t b, int32_t ox, int32_t oy, int s, int x, int y)
{
    int dx = x - ox * s, dy = y - oy * s;
    int lx = dx >= 0 ? dx / s : -((s - 1 - dx) / s), ly = dy >= 0 ? dy / s : -((s - 1 - dy) / s);
    uint32_t bits = (uint32_t)n0 + (uint32_t)a * (uint32_t)lx + (uint32_t)b * (uint32_t)ly;
    int32_t n; memcpy(&n, &bits, 4);
    int key = (n >> 8) + ((n & 255) * s + a * (dx - lx * s) + b * (dy - ly * s) + s * 134217728) / (256 * s) - 524288;
    return key > 0 ? (uint32_t)key : 1u;
}
int main(void)
{
    unsigned pixels = 0, intervals = 0;
    for (int s = 1; s <= 8; s *= 2) for (unsigned i = 0; i < 12000; ++i) {
        int32_t n0 = (int32_t)next(), a = (int32_t)(next() % 8388609u) - 4194304, b = (int32_t)(next() % 8388609u) - 4194304;
        int ox = (int)(next() % 1024) - 512, oy = (int)(next() % 1024) - 512;
        int left = ox * s + (int)(next() % 80) - 40, top = oy * s + (int)(next() % 80) - 40;
        int right = left + 1 + (int)(next() % 12), bottom = top + 1 + (int)(next() % 12);
        uint32_t low, high;
        if (!nvk_depth_interval(n0, a, b, ox, oy, (uint32_t)s, left, top, right, bottom, &low, &high)) continue;
        ++intervals;
        for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
            uint32_t key = shader_key(n0, a, b, ox, oy, s, x, y);
            assert(key >= low && key <= high);
            ++pixels;
        }
    }
    assert(intervals > 40000);
    uint32_t low, high;
    assert(!nvk_depth_interval(INT32_MAX, 4194304, 0, 0, 0, 4, 4, 0, 8, 4, &low, &high));
    printf("PASS %u intervals, %u shader-depth samples\n", intervals, pixels);
}
