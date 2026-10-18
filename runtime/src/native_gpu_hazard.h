#ifndef NATIVE_GPU_HAZARD_H
#define NATIVE_GPU_HAZARD_H

#include <stdint.h>

/* Native GPU vertex: position (2), uv (2), color (4), perspective weight (1)
 * and padding. The weight is the normalized reciprocal view depth (1/z with
 * the nearest vertex at 1.0); zero selects the faithful affine path. */
#define GL_NATIVE_GPU_VERTEX_FLOATS 12u
#define GL_NATIVE_GPU_VERTEX_WEIGHT 8u

/* Writes since the last texture barrier, as exact pixel-space shapes (one
 * triangle or rectangle each). A uniform grid of small index lists finds
 * candidates; a full cell is saturated (any query touching it overlaps) and
 * shapes spanning many cells sit in a short linear list. Overflow of either
 * sets plane->dirty_all instead. */
#define NATIVE_HZ_SHAPES 4096u
#define NATIVE_HZ_CELL 64u
#define NATIVE_HZ_CELL_CAP 8u
#define NATIVE_HZ_FULL 255u
#define NATIVE_HZ_BIG 16u
#define NATIVE_HZ_BIG_CELLS 32u
#define NATIVE_HZ_MARGIN 0.0625f
typedef struct GlNativeGpuShape {
    float v[3][2], box[4], mag; /* pixels; box = x0,y0,x1,y1 */
    float p[3][2], q[3], off;   /* raw vertex data, triangles only */
    int tri;
    /* Immutable edge direction, length and own projection interval. Keep the
     * original division order so separating-axis decisions are unchanged. */
    double edge[3][5]; /* ex, ey, length, minimum, maximum */
} GlNativeGpuShape;
typedef struct GlNativeGpuHazards {
    uint32_t count, big_count, touched_count, columns, rows;
    uint16_t big[NATIVE_HZ_BIG];
    uint8_t *cell_n;
    uint16_t *cell_index;
    uint32_t *touched;
    GlNativeGpuShape shapes[NATIVE_HZ_SHAPES];
} GlNativeGpuHazards;

#define NATIVE_SCHED_CMDS 2048u
#define NATIVE_SCHED_PRIMS 65536u
#define NATIVE_SCHED_LAYERS 1024u
#define NATIVE_SCHED_NONE 0xFFFFu
#define NATIVE_SCHED_CELL 8u /* guest pixels */
typedef struct GlNativeSchedPrim { uint32_t shape; uint8_t count, reads, unknown; } GlNativeSchedPrim;
typedef struct NativeSchedState {
    struct GlNativeGpuDrawGeometry *geo; GlNativeSchedPrim *prim; uint16_t *layer, *shape_layer;
    GlNativeGpuShape *shape; uint32_t *stamp, *touched, *node_shape; int32_t *cell_head, *node_next;
    uint32_t geo_cap, prim_cap, layer_cap, shape_cap, shape_layer_cap, stamp_cap, cell_cap, node_cap, node_next_cap, query;
} NativeSchedState;
extern NativeSchedState s_sched;

void native_gpu_hazards_clear(GlNativeGpuHazards *hz);
int native_gpu_shape_tri(GlNativeGpuShape *s, const float *v0, const float *v1, const float *v2, float off, uint32_t scale);
void native_gpu_shape_rect(GlNativeGpuShape *s, int x0, int y0, int x1, int y1);
int native_gpu_shape_cells_sized(uint32_t width, uint32_t height, uint32_t columns, uint32_t rows, uint32_t size,
                                 const GlNativeGpuShape *s, int c[4]);
int native_gpu_hazard_hit(uint32_t width, uint32_t height, const GlNativeGpuHazards *hz, const GlNativeGpuShape *b, uint32_t scale);
void native_gpu_hazard_add(uint32_t width, uint32_t height, GlNativeGpuHazards **hzp, int *dirty_all, const GlNativeGpuShape *s);
uint32_t native_sched_query(uint32_t width, uint32_t height, uint32_t columns, uint32_t rows, uint32_t size,
                            const GlNativeGpuShape *s, uint32_t r, uint32_t scale, uint32_t best);
uint32_t native_sched_query_bounded(uint32_t width, uint32_t height, uint32_t columns, uint32_t rows, uint32_t size,
                            const GlNativeGpuShape *s, uint32_t r, uint32_t scale, uint32_t best, const uint16_t *cell_max);

#endif
