#include "native_gpu_hazard.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

NativeSchedState s_sched;

void native_gpu_hazards_clear(GlNativeGpuHazards *hz) {
    for (uint32_t i = 0; i < hz->touched_count; ++i) hz->cell_n[hz->touched[i]] = 0;
    hz->touched_count = hz->count = hz->big_count = 0;
}

int native_gpu_shape_tri(GlNativeGpuShape *s, const float *v0, const float *v1, const float *v2, float off, uint32_t scale) {
    const float *v[3] = {v0, v1, v2};
    for (int i = 0; i < 3; ++i) {
        s->p[i][0] = v[i][0]; s->p[i][1] = v[i][1]; s->q[i] = v[i][GL_NATIVE_GPU_VERTEX_WEIGHT];
        if (!(fabsf(s->p[i][0]) < 1e6f && fabsf(s->p[i][1]) < 1e6f)) return -1;
    }
    /* Repeated vertices rasterize nothing. Collinear distinct ones stay: float
     * rounding in the vertex shader may give them a sliver of area. */
    if (!memcmp(s->p[0], s->p[1], 8) || !memcmp(s->p[1], s->p[2], 8) || !memcmp(s->p[0], s->p[2], 8)) return 0;
    s->off = off; s->tri = 1; s->mag = 0.f;
    s->box[0] = s->box[1] = INFINITY; s->box[2] = s->box[3] = -INFINITY;
    for (int i = 0; i < 3; ++i)
        for (int a = 0; a < 2; ++a) {
            s->v[i][a] = (float)(((double)s->p[i][a] + off) * scale);
            if (s->v[i][a] < s->box[a]) s->box[a] = s->v[i][a];
            if (s->v[i][a] > s->box[a + 2]) s->box[a + 2] = s->v[i][a];
            if (fabsf(s->v[i][a]) > s->mag) s->mag = fabsf(s->v[i][a]);
        }
    for (int i = 0; i < 3; ++i) {
        double *e = s->edge[i];
        e[0] = (double)s->v[(i + 1) % 3][0] - s->v[i][0];
        e[1] = (double)s->v[(i + 1) % 3][1] - s->v[i][1];
        e[2] = sqrt(e[0] * e[0] + e[1] * e[1]);
        e[3] = INFINITY; e[4] = -INFINITY;
        if (!(e[2] > 0.0)) continue;
        for (int k = 0; k < 3; ++k) {
            const double d = (s->v[k][0] * -e[1] + s->v[k][1] * e[0]) / e[2];
            if (d < e[3]) e[3] = d;
            if (d > e[4]) e[4] = d;
        }
    }
    return 1;
}

void native_gpu_shape_rect(GlNativeGpuShape *s, int x0, int y0, int x1, int y1) {
    s->tri = 0;
    s->box[0] = (float)x0; s->box[1] = (float)y0; s->box[2] = (float)x1; s->box[3] = (float)y1;
    s->mag = fmaxf(fmaxf(fabsf(s->box[0]), fabsf(s->box[1])), fmaxf(fabsf(s->box[2]), fabsf(s->box[3])));
}

static void native_gpu_shape_points(const GlNativeGpuShape *s, float pt[4][2], int *n) {
    if (s->tri) { memcpy(pt, s->v, sizeof(s->v)); *n = 3; return; }
    pt[0][0] = pt[3][0] = s->box[0]; pt[1][0] = pt[2][0] = s->box[2];
    pt[0][1] = pt[1][1] = s->box[1]; pt[2][1] = pt[3][1] = s->box[3];
    *n = 4;
}

/* 1 when some edge normal of triangle t separates a and b by more than m. */
static int native_gpu_shape_separated(const GlNativeGpuShape *t, const GlNativeGpuShape *other, double m) {
    float points[4][2]; int count;
    native_gpu_shape_points(other, points, &count);
    for (int i = 0; i < 3; ++i) {
        const double *edge = t->edge[i];
        const double ex = edge[0], ey = edge[1], len = edge[2];
        if (!(len > 0.0)) continue;
        double lo = INFINITY, hi = -INFINITY;
        for (int k = 0; k < count; ++k) {
            const double d = (points[k][0] * -ey + points[k][1] * ex) / len;
            if (d < lo) lo = d;
            if (d > hi) hi = d;
        }
        if (edge[4] + m < lo || hi + m < edge[3]) return 1;
    }
    return 0;
}

/* Triangles t and o share vertex V (t->v[ti] == o->v[oi]). 1 when an edge of t
 * through V has t on one side and both other vertices of o farther than m on
 * the other: then the two closures intersect only at V. */
static int native_gpu_vertex_separated(const GlNativeGpuShape *t, int ti, const GlNativeGpuShape *o, int oi, double m) {
    const double vx = t->v[ti][0], vy = t->v[ti][1];
    for (int e = 1; e <= 2; ++e) {
        const int w = (ti + e) % 3, x = (ti + 3 - e) % 3;
        const double ex = t->v[w][0] - vx, ey = t->v[w][1] - vy, len = sqrt(ex * ex + ey * ey);
        if (!(len > 0.0)) continue;
        const double side = (ex * (t->v[x][1] - vy) - ey * (t->v[x][0] - vx)) / len;
        if (fabs(side) <= m) continue;
        int beyond = 1;
        for (int k = 0; k < 3 && beyond; ++k) {
            if (k == oi) continue;
            const double d = (ex * (o->v[k][1] - vy) - ey * (o->v[k][0] - vx)) / len;
            beyond = side > 0.0 ? d < -m : d > m;
        }
        if (beyond) return 1;
    }
    return 0;
}

/* 1 when some pixel centre (x + 0.5, y + 0.5) lies within m of both triangles:
 * each triangle grown by m is six half-planes, and each centre row of their
 * common box gives one x interval to test. Rows beyond the cap count as hits. */
static int native_gpu_tris_share_centre(const GlNativeGpuShape *a, const GlNativeGpuShape *b, double m) {
    double nx[6], ny[6], c[6];
    for (int s = 0; s < 2; ++s) {
        const GlNativeGpuShape *t = s ? b : a;
        const double area = ((double)t->v[1][0] - t->v[0][0]) * ((double)t->v[2][1] - t->v[0][1]) -
                            ((double)t->v[1][1] - t->v[0][1]) * ((double)t->v[2][0] - t->v[0][0]);
        const double sign = area < 0.0 ? -1.0 : 1.0;
        for (int i = 0; i < 3; ++i) {
            const double x0 = t->v[i][0], y0 = t->v[i][1];
            const double ex = t->edge[i][0], ey = t->edge[i][1], len = t->edge[i][2];
            const int k = s * 3 + i;
            if (!(len > 0.0)) { nx[k] = ny[k] = 0.0; c[k] = -1.0; continue; } /* always satisfied */
            /* inside: n . P >= c, with n the unit inward normal */
            nx[k] = -ey * sign / len; ny[k] = ex * sign / len;
            c[k] = nx[k] * x0 + ny[k] * y0 - m;
        }
    }
    const double y0 = fmax(a->box[1], b->box[1]) - m, y1 = fmin(a->box[3], b->box[3]) + m;
    const double r0 = ceil(y0 - 0.5), r1 = floor(y1 - 0.5);
    if (r1 - r0 > 1024.0) return 1;
    const double x0 = fmax(a->box[0], b->box[0]) - m, x1 = fmin(a->box[2], b->box[2]) + m;
    for (double r = r0; r <= r1; r += 1.0) {
        const double yc = r + 0.5;
        double lo = x0, hi = x1;
        for (int k = 0; k < 6 && lo <= hi; ++k) {
            const double rhs = c[k] - ny[k] * yc;
            if (nx[k] > 1e-12) { const double v = rhs / nx[k]; if (v > lo) lo = v; }
            else if (nx[k] < -1e-12) { const double v = rhs / nx[k]; if (v < hi) hi = v; }
            else if (rhs > 0.0) hi = lo - 1.0;
        }
        if (lo <= hi && ceil(lo - 0.5) <= floor(hi - 0.5)) return 1;
    }
    return 0;
}

/* Could the two shapes produce a fragment at the same pixel? Conservative:
 * 0 only when provably disjoint. margin covers vertex-shader float rounding
 * and rasterizer snapping, growing with coordinate magnitude. */
static int native_gpu_shape_overlap(const GlNativeGpuShape *a, const GlNativeGpuShape *b, uint32_t scale) {
    const double m = NATIVE_HZ_MARGIN + 1e-5 * fmaxf(a->mag, b->mag);
    if (a->box[0] > b->box[2] + m || b->box[0] > a->box[2] + m ||
        a->box[1] > b->box[3] + m || b->box[1] > a->box[3] + m) return 0;
    if (a->tri && b->tri && a->off == b->off) {
        /* Watertight rule: triangles sharing an edge with bitwise-identical
         * endpoints (and weights, so identical window coordinates) on strictly
         * opposite sides never both cover a pixel. */
        int ai[3], bi[3], shared = 0;
        for (int i = 0; i < 3 && shared < 3; ++i)
            for (int j = 0; j < 3; ++j)
                if (!memcmp(a->p[i], b->p[j], 8) && !memcmp(&a->q[i], &b->q[j], 4)) { ai[shared] = i; bi[shared++] = j; break; }
        if (shared == 2) {
            const float *s0 = a->p[ai[0]], *s1 = a->p[ai[1]];
            const float *ta = a->p[3 - ai[0] - ai[1]], *tb = b->p[3 - bi[0] - bi[1]];
            const double ex = (double)s1[0] - s0[0], ey = (double)s1[1] - s0[1], len = sqrt(ex * ex + ey * ey);
            if (len > 0.0) {
                const double da = (ex * ((double)ta[1] - s0[1]) - ey * ((double)ta[0] - s0[0])) / len * scale;
                const double db = (ex * ((double)tb[1] - s0[1]) - ey * ((double)tb[0] - s0[0])) / len * scale;
                if ((da > m && db < -m) || (da < -m && db > m)) return 0;
            }
            return 1;
        }
        /* One common vertex V: when a line through V along an edge of either
         * triangle keeps the other's remaining vertices strictly beyond it, the
         * closures meet only at V, which no fragment uses unless V lies on a
         * pixel centre. */
        if (shared == 1) {
            const double fx = a->v[ai[0]][0] - floor(a->v[ai[0]][0]), fy = a->v[ai[0]][1] - floor(a->v[ai[0]][1]);
            if ((fabs(fx - 0.5) > 1.0 / 128.0 || fabs(fy - 0.5) > 1.0 / 128.0) &&
                (native_gpu_vertex_separated(a, ai[0], b, bi[0], m) || native_gpu_vertex_separated(b, bi[0], a, ai[0], m)))
                return 0;
        }
    }
    if (a->tri && native_gpu_shape_separated(a, b, m)) return 0;
    if (b->tri && native_gpu_shape_separated(b, a, m)) return 0;
    if (a->tri && b->tri && !native_gpu_tris_share_centre(a, b, m)) return 0;
    return 1;
}

/* Grid cells of the margin-grown box inside the plane; 0 = no pixel centre. */
int native_gpu_shape_cells_sized(uint32_t width, uint32_t height, uint32_t columns, uint32_t rows, uint32_t size,
                                 const GlNativeGpuShape *s, int c[4]) {
    const float e = NATIVE_HZ_MARGIN + 1e-5f * s->mag;
    const float w = (float)width, h = (float)height;
    if (s->box[2] + e < 0.f || s->box[3] + e < 0.f || s->box[0] - e > w || s->box[1] - e > h) return 0;
    c[0] = (int)(fmaxf(s->box[0] - e, 0.f) / (float)size);
    c[1] = (int)(fmaxf(s->box[1] - e, 0.f) / (float)size);
    c[2] = (int)(fmaxf(s->box[2] + e, 0.f) / (float)size);
    c[3] = (int)(fmaxf(s->box[3] + e, 0.f) / (float)size);
    if (c[2] >= (int)columns) c[2] = (int)columns - 1;
    if (c[3] >= (int)rows) c[3] = (int)rows - 1;
    if (c[0] > c[2]) c[0] = c[2];
    if (c[1] > c[3]) c[1] = c[3];
    return 1;
}

static int native_gpu_shape_cells(uint32_t width, uint32_t height, uint32_t columns, uint32_t rows, const GlNativeGpuShape *s, int c[4]) {
    return native_gpu_shape_cells_sized(width, height, columns, rows, NATIVE_HZ_CELL, s, c);
}

int native_gpu_hazard_hit(uint32_t width, uint32_t height, const GlNativeGpuHazards *hz, const GlNativeGpuShape *b, uint32_t scale) {
    int c[4];
    if (!hz || !hz->count) return 0;
    if (!native_gpu_shape_cells(width, height, hz->columns, hz->rows, b, c)) return 0;
    if ((c[2] - c[0] + 1) * (c[3] - c[1] + 1) > (int)NATIVE_HZ_BIG_CELLS) {
        for (uint32_t i = 0; i < hz->count; ++i)
            if (native_gpu_shape_overlap(&hz->shapes[i], b, scale)) return 1;
        return 0;
    }
    for (int y = c[1]; y <= c[3]; ++y)
        for (int x = c[0]; x <= c[2]; ++x) {
            const uint32_t cell = (uint32_t)y * hz->columns + (uint32_t)x, n = hz->cell_n[cell];
            if (n == NATIVE_HZ_FULL) return 1;
            for (uint32_t k = 0; k < n; ++k)
                if (native_gpu_shape_overlap(&hz->shapes[hz->cell_index[cell * NATIVE_HZ_CELL_CAP + k]], b, scale)) return 1;
        }
    for (uint32_t k = 0; k < hz->big_count; ++k)
        if (native_gpu_shape_overlap(&hz->shapes[hz->big[k]], b, scale)) return 1;
    return 0;
}

/* Any failure to record makes every later read of the plane barrier. */
void native_gpu_hazard_add(uint32_t width, uint32_t height, GlNativeGpuHazards **hzp, int *dirty_all, const GlNativeGpuShape *s) {
    GlNativeGpuHazards *hz = *hzp;
    if (!hz) {
        const uint32_t columns = (width + NATIVE_HZ_CELL - 1u) / NATIVE_HZ_CELL;
        const uint32_t rows = (height + NATIVE_HZ_CELL - 1u) / NATIVE_HZ_CELL;
        hz = columns && rows ? calloc(1, sizeof(*hz)) : NULL;
        if (hz) {
            hz->columns = columns; hz->rows = rows;
            hz->cell_n = calloc((size_t)columns * rows, 1);
            hz->cell_index = malloc((size_t)columns * rows * NATIVE_HZ_CELL_CAP * sizeof(uint16_t));
            hz->touched = malloc((size_t)columns * rows * sizeof(uint32_t));
            *hzp = hz;
            if (!hz->cell_n || !hz->cell_index || !hz->touched) { *dirty_all = 1; return; }
        } else { *dirty_all = 1; return; }
    }
    int c[4];
    if (!native_gpu_shape_cells(width, height, hz->columns, hz->rows, s, c)) return;
    if (hz->count == NATIVE_HZ_SHAPES) { *dirty_all = 1; return; }
    const uint32_t index = hz->count;
    if ((c[2] - c[0] + 1) * (c[3] - c[1] + 1) > (int)NATIVE_HZ_BIG_CELLS) {
        if (hz->big_count == NATIVE_HZ_BIG) { *dirty_all = 1; return; }
        hz->big[hz->big_count++] = (uint16_t)index;
    } else
        for (int y = c[1]; y <= c[3]; ++y)
            for (int x = c[0]; x <= c[2]; ++x) {
                const uint32_t cell = (uint32_t)y * hz->columns + (uint32_t)x, n = hz->cell_n[cell];
                if (!n) hz->touched[hz->touched_count++] = cell;
                if (n == NATIVE_HZ_FULL) continue;
                if (n == NATIVE_HZ_CELL_CAP) hz->cell_n[cell] = NATIVE_HZ_FULL;
                else { hz->cell_index[cell * NATIVE_HZ_CELL_CAP + n] = (uint16_t)index; hz->cell_n[cell] = (uint8_t)(n + 1u); }
            }
    hz->shapes[hz->count++] = *s;
}

/* Highest layer(U) + r over earlier shapes overlapping s, or `best` if higher. */
uint32_t native_sched_query(uint32_t width, uint32_t height, uint32_t columns, uint32_t rows, uint32_t size,
                            const GlNativeGpuShape *s, uint32_t r, uint32_t scale, uint32_t best) {
    return native_sched_query_bounded(width, height, columns, rows, size, s, r, scale, best, NULL);
}

/* Per-cell maxima are upper bounds, independent of exact triangle overlap.
 * Once a query reaches that bound, every remaining candidate in the cell is
 * provably unable to raise it. The unbounded caller keeps the original path. */
uint32_t native_sched_query_bounded(uint32_t width, uint32_t height, uint32_t columns, uint32_t rows, uint32_t size,
                            const GlNativeGpuShape *s, uint32_t r, uint32_t scale, uint32_t best, const uint16_t *cell_max) {
    int c[4];
    if (!native_gpu_shape_cells_sized(width, height, columns, rows, size, s, c)) return best;
    const uint32_t stamp = ++s_sched.query;
    for (int y = c[1]; y <= c[3]; ++y)
        for (int x = c[0]; x <= c[2]; ++x) {
            const uint32_t cell = (uint32_t)y * columns + (uint32_t)x;
            if (s_sched.cell_head[cell] < 0) continue;
            const uint32_t limit = cell_max ? cell_max[cell] + r : UINT32_MAX;
            if (best >= limit) continue;
            for (int32_t n = s_sched.cell_head[cell]; n >= 0; n = s_sched.node_next[n]) {
                const uint32_t u = s_sched.node_shape[n];
                if (s_sched.stamp[u] == stamp) continue;
                s_sched.stamp[u] = stamp;
                if (s_sched.shape_layer[u] + r > best && native_gpu_shape_overlap(&s_sched.shape[u], s, scale)) best = s_sched.shape_layer[u] + r;
                if (best >= limit) break;
            }
        }
    return best;
}
