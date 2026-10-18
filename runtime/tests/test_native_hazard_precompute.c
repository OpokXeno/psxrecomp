#include "../src/native_gpu_hazard.c"
#include <assert.h>
#include <stdio.h>

/* Original pairwise SAT calculation: compare decisions at ordinary and
 * near-touching margins, including collinear triangles and large coordinates. */
static int reference_separated(const GlNativeGpuShape *t, const GlNativeGpuShape *other, double margin)
{
    float a[4][2], b[4][2]; int na, nb;
    native_gpu_shape_points(t, a, &na); native_gpu_shape_points(other, b, &nb);
    for (int i = 0; i < 3; ++i) {
        const double ex = (double)t->v[(i+1)%3][0] - t->v[i][0];
        const double ey = (double)t->v[(i+1)%3][1] - t->v[i][1];
        const double length = sqrt(ex*ex + ey*ey);
        if (!(length > 0.0)) continue;
        double lo_a = INFINITY, hi_a = -INFINITY, lo_b = INFINITY, hi_b = -INFINITY;
        for (int k = 0; k < na; ++k) {
            const double d = (a[k][0] * -ey + a[k][1] * ex) / length;
            if (d < lo_a) lo_a = d;
            if (d > hi_a) hi_a = d;
        }
        for (int k = 0; k < nb; ++k) {
            const double d = (b[k][0] * -ey + b[k][1] * ex) / length;
            if (d < lo_b) lo_b = d;
            if (d > hi_b) hi_b = d;
        }
        if (hi_a + margin < lo_b || hi_b + margin < lo_a) return 1;
    }
    return 0;
}

static uint32_t rng = 0x77a3291u;
static uint32_t random_u32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(void)
{
    unsigned comparisons = 0u;
    for (unsigned scale = 1u; scale <= 8u; scale *= 2u)
        for (unsigned iteration = 0u; iteration < 20000u; ++iteration) {
            float vertices[2][3][GL_NATIVE_GPU_VERTEX_FLOATS] = {{{0}}};
            GlNativeGpuShape a, b;
            const float origin = iteration % 7u == 0u ? 900000.f : -100.f;
            for (unsigned s = 0u; s < 2u; ++s)
                for (unsigned v = 0u; v < 3u; ++v) {
                    vertices[s][v][0] = origin + (random_u32() % 8192u) / 16.f;
                    vertices[s][v][1] = origin + (random_u32() % 8192u) / 16.f;
                }
            if (iteration % 5u == 0u) {
                memcpy(vertices[1][0], vertices[0][0], sizeof(vertices[0][0]));
                memcpy(vertices[1][1], vertices[0][1], sizeof(vertices[0][1]));
            }
            if (iteration % 11u == 0u)
                for (unsigned v = 0u; v < 3u; ++v) vertices[0][v][1] = origin;
            const float offset = 0.5f / scale - 1.f / 64.f;
            if (native_gpu_shape_tri(&a, vertices[0][0], vertices[0][1], vertices[0][2], offset, scale) != 1) continue;
            if (iteration % 3u == 0u) native_gpu_shape_rect(&b, (int)origin, (int)origin, (int)origin+256, (int)origin+128);
            else if (native_gpu_shape_tri(&b, vertices[1][0], vertices[1][1], vertices[1][2], offset, scale) != 1) continue;
            const double margins[] = { 0.0, 0.0625, 0.5, 1.0, NATIVE_HZ_MARGIN + 1e-5 * fmaxf(a.mag,b.mag) };
            for (unsigned m = 0u; m < sizeof(margins)/sizeof(margins[0]); ++m) {
                assert(reference_separated(&a,&b,margins[m]) == native_gpu_shape_separated(&a,&b,margins[m]));
                ++comparisons;
                if (b.tri) {
                    assert(reference_separated(&b,&a,margins[m]) == native_gpu_shape_separated(&b,&a,margins[m]));
                    ++comparisons;
                }
            }
        }
    printf("Precomputed separating axes: %u equivalent comparisons\n", comparisons);
    return 0;
}
