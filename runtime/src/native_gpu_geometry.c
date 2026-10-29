#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "gpu.h"
#include "gpu_render.h"
#include "gpu_semantic_workload.h"
#include "gpu_sw_renderer.h"
#include "gpu_gl_renderer.h"
#include "native_vk_present.h"
#include "gpu_vram_region_set.h"
#include "psx_render_nclip.h"
#include "xg_render_motion.h"
#include "xg_render_native_mesh.h"
#include "xg_render_native_work.h"
#include "xg_render_semantic_presentation.h"
#include "latency_ring.h"
#include "debug_overlay.h"
#include "runtime_menu.h"
#include "wayland_presentation.h"

#include <limits.h>
#include <float.h>
#include "mod_texture_banks.h"
#include "hd_texture_runtime.h"
#include "frame_interpolation.h"
#include "host_osd.h"
#include "host_time.h"
#include "host_cpu.h"
#include "host_parallel.h"
#include "frame_pacing.h"
#include "psx_rewind.h"

#include "psx_sdl.h"
#include "native_phase_cadence.h"
#include "native_gpu_hazard.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "png_write.h"   /* png_write_rgb — present_shot readback */
#include "native_work_internal.h"

/* Perspective weights for one native GPU triangle. Returns 1 and fills
 * weight[3] only when every vertex carries a valid projective position
 * (flag set, view z > 0), mirroring the CPU rasterizer's per-triangle
 * validation; otherwise the triangle stays affine. The weights are scaled so
 * the nearest vertex is 1.0, which keeps the homogeneous w the vertex shader
 * derives (w = 1/weight = z/zMin) in a well-conditioned range. */
static int native_gpu_triangle_weights(const XgRenderIrTriangle *triangle,
                                       float weight[3]) {
    int32_t nearest = 0;
    unsigned vertex;
    weight[0] = weight[1] = weight[2] = 0.0f;
    if (triangle == NULL) return 0;
    for (vertex = 0u; vertex < 3u; ++vertex) {
        const XgRenderIrVertex *current = &triangle->vertices[vertex];
        if (!current->projective_position || current->projective_view_z <= 0)
            return 0;
        if (nearest == 0 || current->projective_view_z < nearest)
            nearest = current->projective_view_z;
    }
    if (nearest == 0) return 0;
    for (vertex = 0u; vertex < 3u; ++vertex)
        weight[vertex] = (float)nearest /
            (float)triangle->vertices[vertex].projective_view_z;
    return 1;
}

static void native_gpu_draw_vertex_data(const GlNativeGpuCommand *command,
                                       const uint8_t *blob, uint32_t triangle,
                                       float data[6][GL_NATIVE_GPU_VERTEX_FLOATS]) {
    const XgSemanticDrawRecord *draw = &command->draw;
    const XgRenderIrMaterialState *m = &draw->primitive.material; /* clip/offsets: the command's */
    const GlNativeTriangleFlags flags = native_gpu_command_flags(command, blob, triangle);
    const int lines = draw->topology == GPU_RENDER_SEMANTIC_LINES;
    const XgRenderIrTriangle *tri = lines ? NULL : native_gpu_command_triangle(command, blob, triangle);
    float weight[3];
    /* Guest VRAM retains PS1 affine sampling. Perspective correction belongs
     * to presentation planes and must not feed back into guest texture words. */
    const int perspective = command->plane != 0u && !lines && flags.textured &&
        native_gpu_triangle_weights(tri, weight);
    memset(data, 0, (lines ? 6u : 3u) * GL_NATIVE_GPU_VERTEX_FLOATS * sizeof(float));
    for (unsigned v = 0; v < (lines ? 2u : 3u); ++v) {
        const XgRenderIrVertex *iv = lines ? NULL : &tri->vertices[v];
        const GpuRenderSemanticVertex *lv = lines ? &draw->lines[triangle].vertices[v] : NULL;
        const unsigned c = !flags.gouraud ? 0u : v;
        data[v][0] = (float)(lines ? lv->x : iv->x) / 65536.f + m->draw_offset_x;
        data[v][1] = (float)(lines ? lv->y : iv->y) / 65536.f + m->draw_offset_y - command->y;
        data[v][2] = lines ? 0.f : (float)iv->u / 65536.f;
        data[v][3] = lines ? 0.f : (float)iv->v / 65536.f;
        data[v][4] = lines ? draw->lines[triangle].vertices[c].r : tri->vertices[c].r;
        data[v][5] = lines ? draw->lines[triangle].vertices[c].g : tri->vertices[c].g;
        data[v][6] = lines ? draw->lines[triangle].vertices[c].b : tri->vertices[c].b;
        if (perspective) data[v][GL_NATIVE_GPU_VERTEX_WEIGHT] = weight[v];
    }
}

static void native_gpu_line_vertex_data(float data[6][GL_NATIVE_GPU_VERTEX_FLOATS]) {
    /* Unit-width geometry including both authored endpoints. */
    float a[8], b[8]; memcpy(a,data[0],sizeof(a));memcpy(b,data[1],sizeof(b));
    const float dx=b[0]-a[0],dy=b[1]-a[1],length=hypotf(dx,dy);
    const float ux=length>0.f?dx/length:1.f,uy=length>0.f?dy/length:0.f;
    const float extension=length>0.f?0.5f/length:0.f;
    if (!length) memcpy(b,a,sizeof(b));
    for(unsigned v=0;v<6u;++v) {
        const int end=v==1u||v==4u||v==5u;
        const float side=v==0u||v==1u||v==4u?-0.5f:0.5f;
        memcpy(data[v],end?b:a,sizeof(a));
        data[v][0]+=ux*(end?0.5f:-0.5f)-uy*side;
        data[v][1]+=uy*(end?0.5f:-0.5f)+ux*side;
        for(unsigned c=4u;c<7u;++c)data[v][c]+=(b[c]-a[c])*(end?extension:-extension);
    }
}

uint32_t native_gpu_command_vertex_count(const GlNativeGpuCommand *command) {
    if (command->kind == NATIVE_GPU_DRAW)
        return command->draw.topology == GPU_RENDER_SEMANTIC_LINES
            ? command->draw.line_count * 6u : native_gpu_command_triangles(command) * 3u;
    if (command->kind == NATIVE_GPU_SPANS) return command->mesh_count * 6u;
    return command->kind == NATIVE_GPU_SEED || command->kind == NATIVE_GPU_SPAN ? 6u : 0u;
}

/* TCP native_draw_probe: once armed, the prepared triangles of the first
 * endpoint work (kind 0) and the first phase work (kind 1) that follow, so
 * one sprite's setup can be compared between both paths. */
#define NATIVE_DRAW_PROBE_CAPACITY 2048u
typedef struct NativeDrawProbeRecord {
    uint32_t plane, command_y, textured, integral, continuous;
    float vertex[3][4], weight[3];
    double origin[2], u[3], v[3];
    int bounds[4];
} NativeDrawProbeRecord;
typedef struct NativeDrawProbeKind {
    const GlNativeGpuWork *work;
    uint32_t count, render_width, render_height, phase_crop_x, phase_crop_y, scanout_x, scanout_offset;
    float raster_scale[2];
    NativeDrawProbeRecord records[NATIVE_DRAW_PROBE_CAPACITY];
} NativeDrawProbeKind;
static SDL_SpinLock s_draw_probe_lock;
static int s_draw_probe_armed;
static NativeDrawProbeKind s_draw_probe[2];

void native_draw_probe_arm(void) {
    SDL_AtomicLock(&s_draw_probe_lock);
    memset(s_draw_probe, 0, sizeof(s_draw_probe));
    s_draw_probe_armed = 1;
    SDL_AtomicUnlock(&s_draw_probe_lock);
}

/* JSON for kind's header and records [offset, offset + count); 0 on overflow. */
size_t native_draw_probe_json(unsigned kind, uint32_t offset, uint32_t count, char *buffer, size_t capacity) {
    if (kind > 1u || !buffer || !capacity) return 0u;
    SDL_AtomicLock(&s_draw_probe_lock);
    const NativeDrawProbeKind *k = &s_draw_probe[kind];
    size_t used = (size_t)snprintf(buffer, capacity,
        "\"captured\":%d,\"total\":%u,\"render\":[%u,%u],\"raster_scale\":[%.9g,%.9g],"
        "\"phase_crop\":[%u,%u],\"scanout_x\":%u,\"scanout_offset\":%u,\"records\":[",
        k->work != NULL, k->count, k->render_width, k->render_height, k->raster_scale[0], k->raster_scale[1],
        k->phase_crop_x, k->phase_crop_y, k->scanout_x, k->scanout_offset);
    for (uint32_t i = offset; i < k->count && i < offset + count && used < capacity; ++i) {
        const NativeDrawProbeRecord *r = &k->records[i];
        used += (size_t)snprintf(buffer + used, capacity - used,
            "%s{\"plane\":%u,\"y\":%u,\"tex\":%u,\"integral\":%u,\"continuous\":%u,"
            "\"xy\":[%.6g,%.6g,%.6g,%.6g,%.6g,%.6g],\"uv\":[%.6g,%.6g,%.6g,%.6g,%.6g,%.6g],"
            "\"weight\":[%.9g,%.9g,%.9g],\"origin\":[%.9g,%.9g],\"u\":[%.9g,%.9g,%.9g],\"v\":[%.9g,%.9g,%.9g],\"bounds\":[%d,%d,%d,%d]}",
            i > offset ? "," : "", r->plane, r->command_y, r->textured, r->integral, r->continuous,
            r->vertex[0][0], r->vertex[0][1], r->vertex[1][0], r->vertex[1][1], r->vertex[2][0], r->vertex[2][1],
            r->vertex[0][2], r->vertex[0][3], r->vertex[1][2], r->vertex[1][3], r->vertex[2][2], r->vertex[2][3],
            r->weight[0], r->weight[1], r->weight[2], r->origin[0], r->origin[1], r->u[0], r->u[1], r->u[2], r->v[0], r->v[1], r->v[2],
            r->bounds[0], r->bounds[1], r->bounds[2], r->bounds[3]);
    }
    if (used < capacity) used += (size_t)snprintf(buffer + used, capacity - used, "]");
    SDL_AtomicUnlock(&s_draw_probe_lock);
    return used < capacity ? used : 0u;
}

static void native_draw_probe_record(const GlNativeGpuWork *work, const GlNativeGpuCommand *command,
        const float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS], const GlNativeTriangleFlags *flags,
        const GlNativeGpuDrawGeometry *out) {
    /* The phase template capture is not a phase: its draws reach phases as TEMPLATE. */
    if (work->capture_templates) return;
    SDL_AtomicLock(&s_draw_probe_lock);
    NativeDrawProbeKind *k = &s_draw_probe[work->wave_phase > 0.0 ? 1u : 0u];
    if (s_draw_probe_armed && !k->work) {
        k->work = work; k->render_width = work->render_width; k->render_height = work->render_height;
        k->phase_crop_x = work->phase_crop_x; k->phase_crop_y = work->phase_crop_y;
        k->scanout_x = work->scanout_x; k->scanout_offset = work->scanout_offset;
        k->raster_scale[0] = work->raster_scale[0]; k->raster_scale[1] = work->raster_scale[1];
    }
    if (s_draw_probe_armed && k->work == work && k->count < NATIVE_DRAW_PROBE_CAPACITY) {
        NativeDrawProbeRecord *r = &k->records[k->count++];
        r->plane = command->plane; r->command_y = command->y; r->textured = flags->textured;
        r->integral = (uint32_t)out->attributes.integral; r->continuous = (uint32_t)out->attributes.continuous;
        for (unsigned v = 0u; v < 3u; ++v) {
            memcpy(r->vertex[v], vertices[v], sizeof(r->vertex[v]));
            r->weight[v] = vertices[v][GL_NATIVE_GPU_VERTEX_WEIGHT];
        }
        r->origin[0] = out->attributes.x; r->origin[1] = out->attributes.y;
        memcpy(r->u, out->attributes.plane[0], sizeof(r->u));
        memcpy(r->v, out->attributes.plane[1], sizeof(r->v));
        r->bounds[0] = out->left; r->bounds[1] = out->top; r->bounds[2] = out->right; r->bounds[3] = out->bottom;
    }
    SDL_AtomicUnlock(&s_draw_probe_lock);
}

void native_gpu_prepare_draw(const GlNativeGpuWork *work,
        const GlNativeGpuCommand *command, const uint8_t *blob, uint32_t triangle,
        uint32_t width, uint32_t height,
        const float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS],
        GlNativeGpuDrawGeometry *out) {
    const XgSemanticDrawRecord *draw = &command->draw;
    const XgRenderIrMaterialState *m = &draw->primitive.material; /* clip/offsets: the command's */
    const GlNativeTriangleFlags flags = native_gpu_command_flags(command, blob, triangle);
    const int lines = draw->topology == GPU_RENDER_SEMANTIC_LINES;
    const int wire = work->wireframe && command->plane != 0u;
    float line_data[6][GL_NATIVE_GPU_VERTEX_FLOATS];
    double x[3], y[3];
    int left = m->draw_area_left, top = (int)m->draw_area_top - command->y;
    int right = m->draw_area_right, bottom = (int)m->draw_area_bottom - command->y;
    out->visible = 0;
    if (left < 0) left = 0; if (top < 0) top = 0;
    if (right >= (int)width) right = (int)width - 1;
    if (bottom >= (int)height) bottom = (int)height - 1;
    if (left > right || top > bottom) return;
    out->scissor[0] = left; out->scissor[1] = top;
    out->scissor[2] = right - left + 1; out->scissor[3] = bottom - top + 1;
    const XgRenderIrTriangle *tri = lines ? NULL : native_gpu_command_triangle(command, blob, triangle);
    if (lines) {
        /* Coverage uses authored endpoints, before unit-width expansion. */
        native_gpu_draw_vertex_data(command, blob, triangle, line_data);
        vertices = line_data;
    } else for (unsigned v = 0u; v < 3u; ++v) {
        const XgRenderIrVertex *iv = &tri->vertices[v];
        x[v] = iv->x / 65536.0 + m->draw_offset_x;
        y[v] = iv->y / 65536.0 + m->draw_offset_y - command->y;
    }
    if (lines && command->plane == 0u &&
        (abs(native_gpu_floor_position(vertices[1][0]) - native_gpu_floor_position(vertices[0][0])) >= VRAM_W ||
         abs(native_gpu_floor_position(vertices[1][1]) - native_gpu_floor_position(vertices[0][1])) >= VRAM_H)) return;
    float min_x = vertices[0][0], max_x = min_x, min_y = vertices[0][1], max_y = min_y;
    for (unsigned v = 1u; v < (lines ? 2u : 3u); ++v) {
        if(vertices[v][0]<min_x)min_x=vertices[v][0];if(vertices[v][0]>max_x)max_x=vertices[v][0];
        if(vertices[v][1]<min_y)min_y=vertices[v][1];if(vertices[v][1]>max_y)max_y=vertices[v][1];
    }
    /* The uploaded positions originate in Q16 int32 vertices plus bounded
     * integer VRAM offsets. They are finite and well inside int range, so
     * truncation/correction gives exact floor/ceil without per-triangle libm. */
    if (left < native_gpu_floor_position(min_x) - 1) left = native_gpu_floor_position(min_x) - 1;
    if (right > native_gpu_ceil_position(max_x) + 1) right = native_gpu_ceil_position(max_x) + 1;
    if (top < native_gpu_floor_position(min_y) - 1) top = native_gpu_floor_position(min_y) - 1;
    if (bottom > native_gpu_ceil_position(max_y) + 1) bottom = native_gpu_ceil_position(max_y) + 1;
    if (left > right || top > bottom) return;
    const int gpu_setup=!lines && work->gpu_setup;
    if(gpu_setup)memset(&out->attributes,0,sizeof(out->attributes));
    if (!lines && !gpu_setup) native_attribute_planes(tri,
        x, y, work->render_width && command->plane ? 0u : native_gpu_plane_scale(work, command->plane), flags.gouraud, &out->attributes);
    out->depth = (GlNativeDepthMode){0, GL_NATIVE_DEPTH_RESET, flags.semi_transparent ? 1 : 0,
        0, GL_NATIVE_DEPTH_SHOW_NONE, {0}};
    if (wire) out->depth.semi = 0;
    else if (!lines && command->plane != 0u &&
        (work->depth_test || work->depth_view != GL_NATIVE_DEPTH_VIEW_OFF)) {
        if(gpu_setup) {
            const unsigned policy=draw->primitive.depth_policy;
            if(!draw->screen_space_2d && !draw->native_view_effect && policy && policy<=GPU_RENDER_DEPTH_TEST_WRITE) {
                out->depth.test=1;
                out->depth.write=policy==GPU_RENDER_DEPTH_TEST_WRITE?GL_NATIVE_DEPTH_WRITE:GL_NATIVE_DEPTH_RESET;
                out->depth.bias=draw->primitive.depth_bias<=16u?draw->primitive.depth_bias:0;
            }
        } else out->depth = native_depth_mode(draw, tri, x, y);
        out->depth.semi = flags.semi_transparent; /* this triangle's own */
    }
    out->left = left; out->top = top; out->right = right; out->bottom = bottom;
    out->visible = 1;
    if (s_draw_probe_armed && !lines) native_draw_probe_record(work, command, vertices, &flags, out);
}

int native_gpu_build_vertex_slice(const GlNativeGpuWork *work,
        const GlNativeGpuCommand *commands, const uint8_t *blob, uint32_t begin, uint32_t end,
        float (**out_vertices)[GL_NATIVE_GPU_VERTEX_FLOATS], uint32_t *out_count) {
    *out_vertices = NULL;
    *out_count = 0u;
    size_t count = 0u;
    for (uint32_t i = begin; i < end; ++i) {
        const uint32_t n = native_gpu_command_vertex_count(&commands[i]);
        if (n > (size_t)INT_MAX - count) return 0;
        count += n;
    }
    *out_count = (uint32_t)count;
    if (!count) return 1;
    if (count > SIZE_MAX / (GL_NATIVE_GPU_VERTEX_FLOATS * sizeof(float))) return 0;
    float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS] = malloc(count * sizeof(*vertices));
    if (!vertices) return 0;
    size_t cursor = 0u;
    for (uint32_t i = begin; i < end; ++i) {
        const GlNativeGpuCommand *command = &commands[i];
        const uint32_t n = native_gpu_command_vertex_count(command);
        if (!n) continue;
        if (command->kind == NATIVE_GPU_DRAW) {
            const int lines = command->draw.topology == GPU_RENDER_SEMANTIC_LINES;
            const uint32_t stride = lines ? 6u : 3u;
            for (uint32_t t = 0; t < n / stride; ++t) {
                float data[6][GL_NATIVE_GPU_VERTEX_FLOATS];
                native_gpu_draw_vertex_data(command, blob, t, data);
                if (lines) native_gpu_line_vertex_data(data);
                memcpy(vertices + cursor, data, stride * sizeof(*vertices));
                cursor += stride;
            }
        } else if (command->kind == NATIVE_GPU_SPANS) {
            uint32_t extent_width = 0u, extent_height = 0u;
            if (g_native_backend) g_native_backend->plane_extent(work, command->source, &extent_width, &extent_height);
            const uint32_t source_scale = native_gpu_plane_scale(work, command->source);
            const uint32_t width = extent_width ? extent_width : work->widths[command->source]*source_scale;
            const uint32_t height = extent_height ? extent_height : work->heights[command->source]*source_scale;
            if (!width || !height) { free(vertices); return 0; }
            const GlNativeGpuSpanRect *rects = (const GlNativeGpuSpanRect *)(blob + command->data);
            for (uint32_t r = 0u; r < command->mesh_count; ++r) {
                const GlNativeGpuSpanRect *rect = &rects[r];
                const float u0=rect->sx*source_scale/width, v0=rect->sy*source_scale/height;
                const float u1=(rect->sx+rect->sw)*source_scale/width, v1=(rect->sy+rect->sh)*source_scale/height;
                const float x=rect->x,y=rect->y,rx=x+rect->w,b=y+rect->h;
                const float data[6][GL_NATIVE_GPU_VERTEX_FLOATS]={{x,y,u0,v0},{rx,y,u1,v0},{x,b,u0,v1},{x,b,u0,v1},{rx,y,u1,v0},{rx,b,u1,v1}};
                memcpy(vertices + cursor, data, sizeof(data));
                cursor += 6u;
            }
        } else {
            float u0=0.f,v0=0.f,u1=1.f,v1=1.f;
            if (command->kind != NATIVE_GPU_SEED && command->source != UINT32_MAX) {
                /* A recipe base has its destination phase raster's size. */
                const uint32_t source_plane = command->source >= GL_NATIVE_GPU_BASE_SOURCE
                    ? command->plane : command->source;
                uint32_t extent_width = 0u, extent_height = 0u;
                if (g_native_backend) g_native_backend->plane_extent(work, source_plane, &extent_width, &extent_height);
                const uint32_t source_scale = native_gpu_plane_scale(work, source_plane);
                /* Deferred CPU journals have captured dimensions but no GL
                 * objects yet. They produce the same scaled UV denominator. */
                const uint32_t width = extent_width ? extent_width : work->widths[source_plane]*source_scale;
                const uint32_t height = extent_height ? extent_height : work->heights[source_plane]*source_scale;
                if (!width || !height) { free(vertices); return 0; }
                u0=(float)(command->sx*source_scale)/width;
                v0=(float)(command->sy*source_scale)/height;
                u1=(float)((command->sx+command->sw)*source_scale)/width;
                v1=(float)((command->sy+command->sh)*source_scale)/height;
            }
            const float x=command->x,y=command->y,r=x+command->w,b=y+command->h;
            const float data[6][GL_NATIVE_GPU_VERTEX_FLOATS]={{x,y,u0,v0},{r,y,u1,v0},{x,b,u0,v1},{x,b,u0,v1},{r,y,u1,v0},{r,b,u1,v1}};
            memcpy(vertices + cursor, data, sizeof(data));
            cursor += 6u;
        }
    }
    *out_vertices = vertices;
    return 1;
}

void native_gpu_geometry_free(GlNativeGpuGeometry *geometry) {
    if (!geometry) return;
    free(geometry->vertices); free(geometry->offsets); free(geometry->primitives); free(geometry);
}

typedef struct GlNativeGeometryBatch {
    const GlNativeGpuWork *work;
    GlNativeGpuGeometry *geometry;
    const uint32_t *units;
    SDL_atomic_t failed;
} GlNativeGeometryBatch;

static void native_gpu_geometry_ranges(uint32_t begin, uint32_t end, void *context) {
    GlNativeGeometryBatch *batch = context;
    const GlNativeGpuWork *work = batch->work;
    GlNativeGpuGeometry *geometry = batch->geometry;
    uint32_t lo = 0u, hi = work->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (batch->units[mid + 1u] <= begin) lo = mid + 1u;
        else hi = mid;
    }
    for (uint32_t i = lo; i < work->count && batch->units[i] < end; ++i) {
        const GlNativeGpuCommand *command = &work->commands[i];
        const GlNativeGpuGeometryOffset offset = geometry->offsets[i];
        if (command->kind == NATIVE_GPU_DRAW) {
            const int lines = command->draw.topology == GPU_RENDER_SEMANTIC_LINES;
            const uint32_t stride = lines ? 6u : 3u;
            const uint32_t first = begin > batch->units[i] ? begin - batch->units[i] : 0u;
            const uint32_t last = (end < batch->units[i + 1u] ? end : batch->units[i + 1u]) - batch->units[i];
            for (uint32_t t = first; t < last; ++t) {
                float data[6][GL_NATIVE_GPU_VERTEX_FLOATS];
                native_gpu_draw_vertex_data(command, work->data, t, data);
                if (lines) native_gpu_line_vertex_data(data);
                float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS] = geometry->vertices + offset.vertex + t * stride;
                memcpy(vertices, data, stride * sizeof(*vertices));
                native_gpu_prepare_draw(work, command, work->data, t, work->widths[command->plane],
                    work->heights[command->plane], vertices, &geometry->primitives[offset.primitive + t]);
            }
        } else if (batch->units[i] != batch->units[i + 1u]) {
            float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS] = NULL;
            uint32_t count = 0u;
            if (!native_gpu_build_vertex_slice(work, work->commands, work->data, i, i + 1u, &vertices, &count))
                SDL_AtomicSet(&batch->failed, 1);
            else if (count) memcpy(geometry->vertices + offset.vertex, vertices, (size_t)count * sizeof(*vertices));
            free(vertices);
        }
    }
}

GlNativeGpuGeometry *native_gpu_geometry_prepare(const GlNativeGpuWork *work) {
    GlNativeGpuGeometry *geometry = calloc(1u, sizeof(*geometry));
    uint32_t *units = NULL;
    if (!geometry || !work->scale) goto failed;
    geometry->command_count = work->count;
    geometry->gpu_setup=work->gpu_setup;
    geometry->offsets = malloc(((size_t)work->count + 1u) * sizeof(*geometry->offsets));
    units = malloc(((size_t)work->count + 1u) * sizeof(*units));
    if (!geometry->offsets || !units) goto failed;
    uint32_t vertex = 0u, primitive = 0u, unit = 0u;
    for (uint32_t i = 0u; i < work->count; ++i) {
        const GlNativeGpuCommand *command = &work->commands[i];
        const uint32_t count = native_gpu_command_vertex_count(command);
        if (count > (uint32_t)INT_MAX - vertex) goto failed;
        geometry->offsets[i] = (GlNativeGpuGeometryOffset){vertex, primitive};
        units[i] = unit;
        vertex += count;
        if (command->kind == NATIVE_GPU_DRAW) {
            const uint32_t draws = command->draw.topology == GPU_RENDER_SEMANTIC_LINES
                ? command->draw.line_count : native_gpu_command_triangles(command);
            primitive += draws;
            unit += draws;
        } else if (count) ++unit;
    }
    geometry->offsets[work->count] = (GlNativeGpuGeometryOffset){vertex, primitive};
    units[work->count] = unit;
    if ((size_t)vertex > SIZE_MAX / sizeof(*geometry->vertices)) goto failed;
    geometry->vertices = vertex ? malloc((size_t)vertex * sizeof(*geometry->vertices)) : NULL;
    geometry->primitives = primitive ? calloc(primitive, sizeof(*geometry->primitives)) : NULL;
    if ((vertex && !geometry->vertices) || (primitive && !geometry->primitives)) goto failed;
    GlNativeGeometryBatch batch = {.work = work, .geometry = geometry, .units = units};
    SDL_AtomicSet(&batch.failed, 0);
    /* Primitive ranges can split a single large mesh and a single phase.
     * Workers fill immutable upload slots; GL/FIFO order stays on its owner. */
    psx_parallel_for(unit, 32u, native_gpu_geometry_ranges, &batch);
    if (SDL_AtomicGet(&batch.failed)) goto failed;
    free(units);
    return geometry;
failed:
    free(units);
    /* The existing owner preparation remains available on allocation failure. */
    native_gpu_geometry_free(geometry);
    return NULL;
}
