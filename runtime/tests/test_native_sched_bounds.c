#include "native_gpu_hazard.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Differential coverage of the optimized cell-list exit against the original
 * unbounded scan, including repeated cells, nonmonotone layers and best seeds. */
static uint32_t rng = 0x31c73a19u;
static uint32_t random_u32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(void)
{
    enum { SHAPES = 256, CELLS = 16 * 16, NODES = SHAPES * CELLS };
    uint16_t maxima[CELLS];
    s_sched.shape = calloc(SHAPES, sizeof(*s_sched.shape));
    s_sched.shape_layer = calloc(SHAPES, sizeof(*s_sched.shape_layer));
    s_sched.stamp = calloc(SHAPES, sizeof(*s_sched.stamp));
    s_sched.cell_head = malloc(CELLS * sizeof(*s_sched.cell_head));
    s_sched.node_shape = malloc(NODES * sizeof(*s_sched.node_shape));
    s_sched.node_next = malloc(NODES * sizeof(*s_sched.node_next));
    assert(s_sched.shape && s_sched.shape_layer && s_sched.stamp && s_sched.cell_head && s_sched.node_shape && s_sched.node_next);
    unsigned queries = 0;
    for (uint32_t scale = 1; scale <= 8; scale *= 2) for (unsigned round = 0; round < 100; ++round) {
        const uint32_t width = 128 * scale, cell_size = 8 * scale;
        for (unsigned c = 0; c < CELLS; ++c) s_sched.cell_head[c] = -1;
        memset(maxima, 0, sizeof(maxima));
        uint32_t nodes = 0;
        for (uint32_t i = 0; i < SHAPES; ++i) {
            GlNativeGpuShape *shape = &s_sched.shape[i];
            float v[3][GL_NATIVE_GPU_VERTEX_FLOATS] = {{0}};
            for (unsigned k = 0; k < 3; ++k) {
                v[k][0] = ((int)(random_u32() % 192) - 32) + (random_u32() % 16) / 16.f;
                v[k][1] = ((int)(random_u32() % 192) - 32) + (random_u32() % 16) / 16.f;
                if (round & 1u) v[k][GL_NATIVE_GPU_VERTEX_WEIGHT] = 0.1f + (random_u32() % 10) / 10.f;
            }
            if ((i & 3u) == 0) native_gpu_shape_rect(shape, (int)(v[0][0] * scale), (int)(v[0][1] * scale),
                (int)(v[0][0] * scale) + 12 * scale, (int)(v[0][1] * scale) + 10 * scale);
            else if (native_gpu_shape_tri(shape, v[0], v[1], v[2], 0.5f / scale - 1.f / 64.f, scale) <= 0) continue;
            for (uint32_t reads = 0; reads <= 1; ++reads) {
                uint32_t best = random_u32() % 128;
                uint32_t full = native_sched_query(width, width, 16, 16, cell_size, shape, reads, scale, best);
                uint32_t bounded = native_sched_query_bounded(width, width, 16, 16, cell_size, shape, reads, scale, best, maxima);
                assert(full == bounded);
                ++queries;
            }
            s_sched.shape_layer[i] = (uint16_t)(random_u32() % NATIVE_SCHED_LAYERS);
            int cells[4];
            if (!native_gpu_shape_cells_sized(width, width, 16, 16, cell_size, shape, cells)) continue;
            for (int y = cells[1]; y <= cells[3]; ++y) for (int x = cells[0]; x <= cells[2]; ++x) {
                uint32_t c = y * 16u + x;
                if (s_sched.shape_layer[i] > maxima[c]) maxima[c] = s_sched.shape_layer[i];
                assert(nodes < NODES);
                s_sched.node_shape[nodes] = i; s_sched.node_next[nodes] = s_sched.cell_head[c];
                s_sched.cell_head[c] = (int32_t)nodes++;
            }
        }
    }
    free(s_sched.shape); free(s_sched.shape_layer); free(s_sched.stamp);
    free(s_sched.cell_head); free(s_sched.node_shape); free(s_sched.node_next);
    printf("PASS %u bounded/unbounded dependency queries\n", queries);
    return 0;
}
