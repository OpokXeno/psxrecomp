/* gpu_native_canonical.c — API-free canonical VRAM owner for Native work.
 *
 * Native work journals every GP0 draw, CPU->VRAM upload, fill and copy, and
 * its compile thread publishes the guest-resolution VRAM words after each
 * work. Guest-visible VRAM therefore needs no GPU-API rasterizer: the
 * `native` facade backend below validates and defers draws/fills/copies to
 * the journal, keeps a CPU mirror (the `vram` array handed to gr_init), and
 * synchronises that mirror from the published words before any CPU read.
 * The OpenGL single-owner mode shares the validation, the deferral flag and
 * the sync with this backend. */

#include "gpu_native_canonical.h"
#include "gpu.h"
#include "native_work_internal.h"
#include "native_vk_presenter.h"
#include "crash_trace.h"
#include "psx_cycles.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const GpuRenderBackend *gl_backend_get(void);

static int s_deferred;
static uint16_t *s_vram;
static const GpuRenderBackend *s_host;
static int s_area[4] = { 0, 0, VRAM_W - 1, VRAM_H - 1 };
static GpuNativeCanonicalStats s_stats;

/* Structural validation of an accepted semantic; shared by the OpenGL
 * single-owner mode and the native backend. */
GpuRenderTransactionStatus gpu_native_canonical_validate_semantic(
        const GpuRenderSemantic *semantic, int midpoint_pass) {
    const GpuRenderMaterial *material;
    uint16_t encoded_depth;

    if (!semantic) return GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    if (semantic->screen_space_2d > GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE ||
        semantic->native_view_effect >
            GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID ||
        (semantic->native_view_effect == GPU_RENDER_NATIVE_VIEW_EFFECT_NONE &&
         semantic->native_view_effect_index != 0u) ||
        (semantic->native_view_effect ==
             GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID &&
         semantic->native_view_effect_index >= NATIVE_VIEW_WAVE_PACKET_COUNT))
        return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
    material = &semantic->material;
    switch (material->texture_depth) {
    case GPU_RENDER_TEXTURE_4_BIT:
    case GPU_RENDER_TEXTURE_8_BIT:
    case GPU_RENDER_TEXTURE_15_BIT:
        break;
    default:
        return GPU_RENDER_TRANSACTION_UNSUPPORTED;
    }
    switch (material->blend_mode) {
    case GPU_RENDER_BLEND_AVERAGE:
    case GPU_RENDER_BLEND_ADD:
    case GPU_RENDER_BLEND_SUBTRACT:
    case GPU_RENDER_BLEND_ADD_QUARTER:
        break;
    default:
        return GPU_RENDER_TRANSACTION_UNSUPPORTED;
    }
    switch (material->shading) {
    case GPU_RENDER_SHADING_FLAT:
    case GPU_RENDER_SHADING_GOURAUD:
        break;
    default:
        return GPU_RENDER_TRANSACTION_UNSUPPORTED;
    }

    encoded_depth = (uint16_t)((material->tpage >> 7) & 3u);
    if (material->tpage > UINT16_C(0x01ff) || encoded_depth == 3u ||
        material->texture_page_x != (material->tpage & UINT16_C(0x000f)) ||
        material->texture_page_y != ((material->tpage >> 4) & 1u) ||
        material->blend_mode !=
            (GpuRenderBlendMode)((material->tpage >> 5) & 3u) ||
        material->texture_depth != (GpuRenderTextureDepth)encoded_depth ||
        material->clut_x > 1023u || (material->clut_x & 15u) != 0u ||
        material->clut_y > 511u ||
        material->draw_area_left > material->draw_area_right ||
        material->draw_area_top > material->draw_area_bottom ||
        material->draw_area_right > 1023u ||
        material->draw_area_bottom > 1023u ||
        material->draw_offset_x < -1024 || material->draw_offset_x > 1023 ||
        material->draw_offset_y < -1024 || material->draw_offset_y > 1023 ||
        material->texture_window_mask_x > 31u ||
        material->texture_window_mask_y > 31u ||
        material->texture_window_offset_x > 31u ||
        material->texture_window_offset_y > 31u ||
        material->textured > 1u || material->raw_texture > 1u ||
        material->semi_transparent > 1u || material->dither > 1u ||
        material->mask_set > 1u ||
        material->mask_check > 1u ||
        (material->raw_texture && !material->textured))
        return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;

    if (semantic->topology == GPU_RENDER_SEMANTIC_LINES) {
        if (semantic->screen_space_2d || material->textured ||
            material->raw_texture ||
            semantic->triangle_count != 0u || semantic->line_count == 0u ||
            semantic->line_count > GPU_RENDER_SEMANTIC_LINE_CAPACITY)
            return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
        for (uint8_t line_index = 0u;
             line_index < semantic->line_count; line_index++) {
            for (int vertex_index = 0; vertex_index < 2; vertex_index++) {
                const GpuRenderSemanticVertex *vertex =
                    &semantic->lines[line_index].vertices[vertex_index];

                /* Only midpoint passes may interpolate positions between PSX
                 * pixels. Texture coordinates remain integer PSX texels. */
                if ((!midpoint_pass &&
                     (((uint32_t)vertex->x & UINT32_C(0xffff)) != 0u ||
                      ((uint32_t)vertex->y & UINT32_C(0xffff)) != 0u)) ||
                    ((uint32_t)vertex->u & UINT32_C(0xffff)) != 0u ||
                    ((uint32_t)vertex->v & UINT32_C(0xffff)) != 0u)
                    return GPU_RENDER_TRANSACTION_UNSUPPORTED;
                if (vertex->native_view_position > 1u ||
                    (!vertex->native_view_position &&
                     (vertex->native_view_x != 0 || vertex->native_view_y != 0)))
                    return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
                if (material->shading == GPU_RENDER_SHADING_FLAT &&
                    vertex_index != 0 &&
                    (vertex->r != semantic->lines[line_index].vertices[0].r ||
                     vertex->g != semantic->lines[line_index].vertices[0].g ||
                     vertex->b != semantic->lines[line_index].vertices[0].b))
                    return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
            }
        }
        return GPU_RENDER_TRANSACTION_OK;
    }
    if (semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->line_count != 0u || semantic->triangle_count == 0u ||
        semantic->triangle_count > GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY)
        return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;

    for (uint8_t triangle_index = 0;
         triangle_index < semantic->triangle_count; triangle_index++) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];

        if (triangle->split_index != triangle_index ||
            triangle->split_count != semantic->triangle_count)
            return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
        for (int vertex_index = 0; vertex_index < 3; vertex_index++) {
            const GpuRenderSemanticVertex *vertex =
                &triangle->vertices[vertex_index];

            /* Midpoint passes retain 16.16 positions, including half-pixels.
             * GP0 texture coordinates must still address whole texels. */
            if ((!midpoint_pass &&
                 (((uint32_t)vertex->x & UINT32_C(0xffff)) != 0u ||
                  ((uint32_t)vertex->y & UINT32_C(0xffff)) != 0u)) ||
                ((uint32_t)vertex->u & UINT32_C(0xffff)) != 0u ||
                ((uint32_t)vertex->v & UINT32_C(0xffff)) != 0u)
                return GPU_RENDER_TRANSACTION_UNSUPPORTED;
            if (vertex->native_view_position > 1u ||
                (semantic->screen_space_2d && vertex->native_view_position) ||
                (!vertex->native_view_position &&
                 (vertex->native_view_x != 0 || vertex->native_view_y != 0)))
                return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
            if (material->shading == GPU_RENDER_SHADING_FLAT &&
                vertex_index != 0 &&
                (vertex->r != triangle->vertices[0].r ||
                 vertex->g != triangle->vertices[0].g ||
                 vertex->b != triangle->vertices[0].b))
                return GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
        }
    }
    return GPU_RENDER_TRANSACTION_OK;
}

void gpu_native_canonical_defer(GpuNativeCanonicalOp op) {
    s_deferred = 1;
    switch (op) {
    case GPU_NATIVE_CANONICAL_DRAW:        s_stats.deferred_draws++; break;
    case GPU_NATIVE_CANONICAL_LEGACY_DRAW: s_stats.deferred_legacy_draws++; break;
    case GPU_NATIVE_CANONICAL_FILL:        s_stats.deferred_fills++; break;
    case GPU_NATIVE_CANONICAL_COPY:        s_stats.deferred_copies++; break;
    }
}

int gpu_native_canonical_deferred(void) { return s_deferred; }

void gpu_native_canonical_discard(void) { s_deferred = 0; }

/* Native replays accepted geometry/fills/copies into canonical VRAM. Only a
 * CPU read needs the mirror: finish the FIFO before exposing any of its words. */
int gpu_native_canonical_sync(uint16_t *vram) {
    uint64_t started;
    uint64_t elapsed;
    int valid;

    if (!s_deferred) return 1;
    started = SDL_GetTicksNS();
    if (!xg_render_native_work_drain(psx_get_cycle_count())) {
        psx_fatal_halt("Native canonical synchronization failed");
        return 0;
    }
    SDL_LockMutex(s_native_state_mutex);
    valid = s_native_native_words != NULL;
    if (valid) memcpy(vram, s_native_native_words, VRAM_W * VRAM_H * sizeof(*vram));
    SDL_UnlockMutex(s_native_state_mutex);
    if (!valid) {
        psx_fatal_halt("Native canonical words unavailable");
        return 0;
    }
    s_deferred = 0;
    elapsed = SDL_GetTicksNS() - started;
    s_stats.sync_count++;
    s_stats.sync_total_ns += elapsed;
    if (elapsed > s_stats.sync_max_ns) s_stats.sync_max_ns = elapsed;
    return 1;
}

/* The host has joined: no collector drain is possible, so keep the last
 * published words. */
int gpu_native_canonical_handoff(uint16_t *vram) {
    if (!s_deferred) return 0;
    if (s_native_native_words && vram)
        memcpy(vram, s_native_native_words, VRAM_W * VRAM_H * sizeof(*vram));
    s_deferred = 0;
    s_stats.handoffs++;
    return 1;
}

void gpu_native_canonical_stats(GpuNativeCanonicalStats *out) {
    *out = s_stats;
    out->active = gr_vram_owner_is_native();
    out->deferred = s_deferred;
}

/* ---- `native` facade backend ---------------------------------------------
 * Raster state setters (semi-transparency, mask bits, texture window, colour
 * modulation, dither, precise/perspective triangle, draw offset) configure a
 * rasterizer; Native work carries the full material in every journaled draw
 * and nothing here rasterizes, so they record nothing. Host configuration
 * (scale, texture filter) belongs to the presenting renderer and is
 * forwarded to it. */

static void nb_init(uint16_t *vram) {
    s_vram = vram;
    s_deferred = 0;
    sw_renderer_init(vram);
    if (s_host) s_host->init(vram);
}
/* Without a host renderer (Native on Vulkan, no OpenGL context) the facade
 * itself holds the scale and filter the menu asks for; Native work consumes
 * them from here. */
static int s_free_scale = 1, s_free_filter = 0;
static void nb_set_scale(int scale) {
    if (s_host) s_host->set_scale(scale);
    else s_free_scale = scale > 0 ? scale : 1;
}
static int nb_scale(void) { return s_host ? s_host->scale() : s_free_scale; }
static void nb_set_texture_filter(int bilinear) {
    if (s_host) s_host->set_texture_filter(bilinear);
    else s_free_filter = bilinear ? 1 : 0;
}
static int nb_texture_filter(void) {
    return s_host ? s_host->texture_filter() : s_free_filter;
}
static void nb_set_semi_transparency(int enabled, int mode) { (void)enabled; (void)mode; }
static void nb_set_mask_bits(int set_bit, int check_bit) { (void)set_bit; (void)check_bit; }
static void nb_set_texture_window(uint32_t raw) { (void)raw; }
static void nb_set_color_modulation(int r, int g, int b, int raw_texture) {
    (void)r; (void)g; (void)b; (void)raw_texture;
}
static void nb_set_dither(int enabled) { (void)enabled; }
static void nb_set_precise_triangle(int enabled, int32_t x0, int32_t y0,
                                    int32_t x1, int32_t y1,
                                    int32_t x2, int32_t y2) {
    (void)enabled; (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2;
}
static void nb_set_perspective_triangle(int enabled, float q0, float q1, float q2) {
    (void)enabled; (void)q0; (void)q1; (void)q2;
}
static void nb_set_draw_area(int x1, int y1, int x2, int y2) {
    s_area[0] = x1; s_area[1] = y1; s_area[2] = x2; s_area[3] = y2;
}
static void nb_get_draw_area(int *x1, int *y1, int *x2, int *y2) {
    *x1 = s_area[0]; *y1 = s_area[1]; *x2 = s_area[2]; *y2 = s_area[3];
}
static void nb_set_draw_offset(int x, int y) { (void)x; (void)y; }

/* Fills and copies, native or legacy route: the GP0 route already journaled
 * the operation (gpu.c emits the VRAM event). */
static void nb_fill_rect(int x, int y, int w, int h, uint16_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
    gpu_native_canonical_defer(GPU_NATIVE_CANONICAL_FILL);
}
static void nb_copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    (void)src_x; (void)src_y; (void)dst_x; (void)dst_y; (void)w; (void)h;
    gpu_native_canonical_defer(GPU_NATIVE_CANONICAL_COPY);
}

/* Per-primitive draws exist only on the legacy GP0 route, which Native work
 * reaches when the packet stream is disabled for a scene. gp0_execute_command
 * captured the primitive into the journal before executing it. */
static void nb_legacy_draw(void) {
    gpu_native_canonical_defer(GPU_NATIVE_CANONICAL_LEGACY_DRAW);
}
static void nb_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                                  uint16_t color) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2; (void)color;
    nb_legacy_draw();
}
static void nb_draw_gouraud_triangle(int x0, int y0, uint16_t c0,
                                     int x1, int y1, uint16_t c1,
                                     int x2, int y2, uint16_t c2) {
    (void)x0; (void)y0; (void)c0; (void)x1; (void)y1; (void)c1;
    (void)x2; (void)y2; (void)c2;
    nb_legacy_draw();
}
static void nb_draw_flat_triangle_rgb888(int x0, int y0, int x1, int y1,
                                         int x2, int y2, uint32_t color) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2; (void)color;
    nb_legacy_draw();
}
static void nb_draw_gouraud_triangle_rgb888(int x0, int y0, uint32_t c0,
                                            int x1, int y1, uint32_t c1,
                                            int x2, int y2, uint32_t c2) {
    (void)x0; (void)y0; (void)c0; (void)x1; (void)y1; (void)c1;
    (void)x2; (void)y2; (void)c2;
    nb_legacy_draw();
}
static void nb_draw_textured_triangle(int x0, int y0, int u0, int v0,
                                      int x1, int y1, int u1, int v1,
                                      int x2, int y2, int u2, int v2,
                                      uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage) {
    (void)x0; (void)y0; (void)u0; (void)v0; (void)x1; (void)y1; (void)u1; (void)v1;
    (void)x2; (void)y2; (void)u2; (void)v2;
    (void)clut_x; (void)clut_y; (void)texpage;
    nb_legacy_draw();
}
static void nb_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0,
                                             uint32_t color0,
                                             int x1, int y1, int u1, int v1,
                                             uint32_t color1,
                                             int x2, int y2, int u2, int v2,
                                             uint32_t color2,
                                             uint16_t clut_x, uint16_t clut_y,
                                             uint16_t texpage, int raw_texture) {
    (void)x0; (void)y0; (void)u0; (void)v0; (void)color0;
    (void)x1; (void)y1; (void)u1; (void)v1; (void)color1;
    (void)x2; (void)y2; (void)u2; (void)v2; (void)color2;
    (void)clut_x; (void)clut_y; (void)texpage; (void)raw_texture;
    nb_legacy_draw();
}
static void nb_draw_flat_rect(int x, int y, int w, int h, uint16_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
    nb_legacy_draw();
}
static void nb_draw_textured_rect(int x, int y, int w, int h, int u, int v,
                                  uint16_t clut_x, uint16_t clut_y,
                                  uint16_t texpage) {
    (void)x; (void)y; (void)w; (void)h; (void)u; (void)v;
    (void)clut_x; (void)clut_y; (void)texpage;
    nb_legacy_draw();
}
static void nb_draw_textured_rect_scaled(int x, int y, int w, int h,
                                         int u0, int v0, int u1, int v1,
                                         uint16_t clut_x, uint16_t clut_y,
                                         uint16_t texpage) {
    (void)x; (void)y; (void)w; (void)h; (void)u0; (void)v0; (void)u1; (void)v1;
    (void)clut_x; (void)clut_y; (void)texpage;
    nb_legacy_draw();
}
static void nb_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)color;
    nb_legacy_draw();
}
static void nb_draw_shaded_line(int x0, int y0, uint16_t c0,
                                int x1, int y1, uint16_t c1) {
    (void)x0; (void)y0; (void)c0; (void)x1; (void)y1; (void)c1;
    nb_legacy_draw();
}

/* The journal is the single ordering authority; there is no host surface
 * whose readiness a barrier could check. */
static GpuRenderTransactionStatus nb_stream_barrier(void) {
    return GPU_RENDER_TRANSACTION_OK;
}
static GpuRenderTransactionStatus nb_draw_semantic_immediate(
        const GpuRenderSemantic *semantic) {
    const GpuRenderTransactionStatus status =
        gpu_native_canonical_validate_semantic(semantic, 0);

    if (status != GPU_RENDER_TRANSACTION_OK) return status;
    gpu_native_canonical_defer(GPU_NATIVE_CANONICAL_DRAW);
    return GPU_RENDER_TRANSACTION_OK;
}
/* Native work compiles temporal phases and anchors from the journal. */
static GpuRenderTransactionStatus nb_draw_semantic_temporal_candidate(
        const GpuRenderSemantic *semantic, const GpuRenderTemporalCullPolicy *policy) {
    if (semantic == NULL || policy == NULL)
        return GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    s_stats.skipped_temporal_candidates++;
    return GPU_RENDER_TRANSACTION_OK;
}
static GpuRenderTransactionStatus nb_record_interpolation_anchors(
        const GpuRenderInterpolationVertexAnchor *anchors, size_t count) {
    if (anchors == NULL && count != 0u)
        return GPU_RENDER_TRANSACTION_INVALID_ARGUMENT;
    s_stats.skipped_anchor_vertices += count;
    return GPU_RENDER_TRANSACTION_OK;
}

static int nb_render_display(uint32_t *out, int pitch, int dx, int dy, int dw, int dh) {
    if (dw > 0 && dh > 0 && !gpu_native_canonical_sync(s_vram)) return 0;
    return sw_render_display(out, pitch, dx, dy, dw, dh);
}
static int nb_render_display_hires(uint32_t *out, int pitch, int dx, int dy, int dw, int dh) {
    if (dw > 0 && dh > 0 && !gpu_native_canonical_sync(s_vram)) return 0;
    return sw_render_display_hires(out, pitch, dx, dy, dw, dh);
}

/* The only writer is the text translation layer patching strips after an
 * upload; the upload event carried the unpatched pixels, so the journal
 * would never learn of the patch. */
static void nb_vram_write(int x, int y, uint16_t pixel) {
    (void)x; (void)y; (void)pixel;
    psx_fatal_halt("Native canonical VRAM point write is not journaled "
                   "(text vram_patch needs an upload event)");
}
static void nb_vram_prepare_read(int x, int y, int w, int h) {
    (void)x; (void)y;
    if (w > 0 && h > 0) (void)gpu_native_canonical_sync(s_vram);
}
static uint16_t nb_vram_read(int x, int y) {
    (void)gpu_native_canonical_sync(s_vram);
    return sw_vram_read(x, y);
}
/* The transfer was journaled by gpu.c; the mirror takes the same words. */
static void nb_vram_transfer_in(int x, int y, int w, int h, const uint16_t *data) {
    sw_vram_transfer_in(x, y, w, h, data);
    s_stats.transfers_in++;
}
static void nb_vram_transfer_out(int x, int y, int w, int h, uint16_t *data) {
    (void)gpu_native_canonical_sync(s_vram);
    sw_vram_transfer_out(x, y, w, h, data);
    s_stats.transfers_out++;
}

/* Presentation, the wide compositor, native-view margins and the legacy
 * transaction group are left NULL: the facade null-checks each of them, the
 * Native presenter composes through its own services, and the legacy
 * present body is skipped while Native is selected (main.cpp). */
static const GpuRenderBackend NATIVE_BACKEND = {
    .name = "native",
    .init = nb_init,
    .set_scale = nb_set_scale,
    .scale = nb_scale,
    .set_texture_filter = nb_set_texture_filter,
    .texture_filter = nb_texture_filter,
    .set_semi_transparency = nb_set_semi_transparency,
    .set_mask_bits = nb_set_mask_bits,
    .set_texture_window = nb_set_texture_window,
    .set_color_modulation = nb_set_color_modulation,
    .set_dither = nb_set_dither,
    .set_precise_triangle = nb_set_precise_triangle,
    .set_perspective_triangle = nb_set_perspective_triangle,
    .fill_rect = nb_fill_rect,
    .copy_rect = nb_copy_rect,
    .draw_flat_triangle = nb_draw_flat_triangle,
    .draw_gouraud_triangle = nb_draw_gouraud_triangle,
    .draw_flat_triangle_rgb888 = nb_draw_flat_triangle_rgb888,
    .draw_gouraud_triangle_rgb888 = nb_draw_gouraud_triangle_rgb888,
    .draw_textured_triangle = nb_draw_textured_triangle,
    .draw_shaded_textured_triangle = nb_draw_shaded_textured_triangle,
    .draw_flat_rect = nb_draw_flat_rect,
    .draw_textured_rect = nb_draw_textured_rect,
    .draw_textured_rect_scaled = nb_draw_textured_rect_scaled,
    .draw_line = nb_draw_line,
    .draw_shaded_line = nb_draw_shaded_line,
    .native_fill_rect = nb_fill_rect,
    .native_copy_rect = nb_copy_rect,
    .stream_barrier = nb_stream_barrier,
    .draw_semantic_immediate = nb_draw_semantic_immediate,
    .draw_semantic_temporal_candidate = nb_draw_semantic_temporal_candidate,
    .record_interpolation_anchors = nb_record_interpolation_anchors,
    .render_display = nb_render_display,
    .render_display_hires = nb_render_display_hires,
    .vram_write = nb_vram_write,
    .vram_prepare_read = nb_vram_prepare_read,
    .vram_read = nb_vram_read,
    .vram_transfer_in = nb_vram_transfer_in,
    .vram_transfer_out = nb_vram_transfer_out,
    .set_draw_area = nb_set_draw_area,
    .get_draw_area = nb_get_draw_area,
    .set_draw_offset = nb_set_draw_offset,
};

const GpuRenderBackend *native_canonical_backend_get(void) { return &NATIVE_BACKEND; }

/* Native work is the single valid owner of guest VRAM when the host renderer
 * is OpenGL running its Native service, nothing else owns VRAM
 * (netplay/visual-truth dual raster, recipe audit) and the work lane is up.
 * Opt-in until it replaces the host canonical path. */
int gpu_native_canonical_enter(void) {
    const char *option = getenv("PSX_NATIVE_CANONICAL_FACADE");
    /* Native on Vulkan has no host renderer to own VRAM: the facade is the
     * only owner, so it is not opt-in there. */
    const int no_host = native_vk_services_active();

    if (gr_vram_owner_is_native()) return 0;
    if (!no_host && (!option || option[0] != '1')) return 0;
    if (gr_backend() != (no_host ? GR_BACKEND_SOFTWARE : GR_BACKEND_OPENGL) || !s_native_active ||
        !xg_render_native_work_enabled() || gl_renderer_cpu_auth_dual() ||
        gl_renderer_native_guest_reference_enabled() ||
        s_native_recipe_audit_enabled)
        return 0;
    /* Pull any pixels the host canonical raster still owns into the mirror. */
    gr_vram_prepare_read(0, 0, VRAM_W, VRAM_H);
    s_vram = gpu_get_vram_ptr();
    s_host = no_host ? NULL : gl_backend_get();
    if (no_host) {
        s_free_scale = gr_scale();
        s_free_filter = gr_texture_filter();
    }
    gr_set_backend(GR_BACKEND_NATIVE);
    return 1;
}

/* Back to the host canonical path. The deferral flag is kept: the host's own
 * sync/handoff resolves it from the same Native words. */
void gpu_native_canonical_leave(void) {
    if (!gr_vram_owner_is_native()) return;
    gr_set_backend(gr_backend());
    s_host = NULL;
}

/* ---- PSX_NATIVE_CANONICAL_AUDIT ------------------------------------------ */

#define AUDIT_RING 256
#define AUDIT_FIRST 8

typedef struct AuditMismatch { uint16_t x, y, gl, native; } AuditMismatch;
typedef struct AuditEntry {
    uint64_t seq, vblank, cycle;
    uint64_t cmd_index;               /* COMMAND audits: ordinal of the GP0 command */
    uint32_t cmd_words[16];
    uint8_t cmd_count;
    GpuNativeDrawEnvironment cmd_env; /* state the command executed under */
    uint8_t reason, mask_only, shown;
    uint16_t x, y, w, h;
    uint32_t count;
    uint16_t min_x, min_y, max_x, max_y;
    AuditMismatch first[AUDIT_FIRST];
} AuditEntry;

static const char *const AUDIT_REASON_NAME[GPU_NATIVE_AUDIT_REASON_COUNT] = {
    "prepare_read", "vram_read", "transfer_out", "vblank", "command"
};

static atomic_flag s_audit_lock = ATOMIC_FLAG_INIT;
static AuditEntry s_audit_ring[AUDIT_RING];
static uint64_t s_audit_seq;          /* entries recorded (mismatching audits) */
static uint64_t s_audit_vblank;
static uint64_t s_audit_runs[GPU_NATIVE_AUDIT_REASON_COUNT];
static uint64_t s_audit_bad[GPU_NATIVE_AUDIT_REASON_COUNT];
static uint64_t s_audit_mask_only;
static uint64_t s_audit_pixels;
static uint64_t s_audit_unavailable;
static int s_audit_busy;
static int s_audit_vblank_reason;   /* gpu_native_canonical_audit_vblank is driving the read */
static int s_audit_cmd_active;      /* gpu_native_canonical_audit_command is driving the read */
static uint64_t s_audit_cmd_index;
static uint64_t s_audit_cmd_bad;
static uint32_t s_audit_cmd_words[16];
static int s_audit_cmd_count;
static GpuNativeDrawEnvironment s_audit_cmd_env;
static int s_audit_covered;           /* last prepare/transfer rect is clean */
static int s_audit_cov[4];

/* Held snapshot: 0 empty, 1 being filled, 2 held, 3 being dumped. */
static int s_snap_state;
static uint16_t *s_snap_gl, *s_snap_native;
static AuditEntry s_snap_meta;

static void audit_lock(void) { while (atomic_flag_test_and_set_explicit(&s_audit_lock, memory_order_acquire)) {} }
static void audit_unlock(void) { atomic_flag_clear_explicit(&s_audit_lock, memory_order_release); }

int gpu_native_canonical_audit_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("PSX_NATIVE_CANONICAL_AUDIT");
        mode = !e ? 0 : e[0] == '3' ? 3 : e[0] == '2' ? 2 : e[0] == '1' ? 1 : 0;
    }
    return mode;
}

void gpu_native_canonical_audit(GpuNativeCanonicalAuditReason reason,
                                int x, int y, int w, int h,
                                const uint16_t *gl_words) {
    AuditEntry e;
    int snapshot = 0;

    if (!gpu_native_canonical_audit_mode() || s_audit_busy || !gl_words ||
        w <= 0 || h <= 0 || gr_vram_owner_is_native())
        return;
    if (reason == GPU_NATIVE_AUDIT_VRAM_READ && s_audit_covered &&
        x >= s_audit_cov[0] && y >= s_audit_cov[1] &&
        x < s_audit_cov[0] + s_audit_cov[2] && y < s_audit_cov[1] + s_audit_cov[3])
        return;   /* inside a rect just audited clean: avoid per-pixel flood */
    if (!s_native_active || !xg_render_native_work_enabled() ||
        !s_native_state_mutex) {
        s_audit_unavailable++;
        return;
    }
    if (s_audit_vblank_reason) reason = GPU_NATIVE_AUDIT_VBLANK;
    if (s_audit_cmd_active) reason = GPU_NATIVE_AUDIT_COMMAND;
    s_audit_busy = 1;
    if (!xg_render_native_work_drain(psx_get_cycle_count())) {
        s_audit_unavailable++;
        s_audit_busy = 0;
        return;
    }
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    x &= VRAM_W - 1;
    y &= VRAM_H - 1;
    memset(&e, 0, sizeof(e));
    e.vblank = s_audit_vblank;
    e.cycle = psx_get_cycle_count();
    e.reason = (uint8_t)reason;
    if (gpu_native_canonical_audit_mode() >= 3 && s_audit_cmd_count) {
        e.cmd_index = s_audit_cmd_index;
        e.cmd_count = (uint8_t)s_audit_cmd_count;
        memcpy(e.cmd_words, s_audit_cmd_words, sizeof(e.cmd_words));
        e.cmd_env = s_audit_cmd_env;
    }
    e.x = (uint16_t)x; e.y = (uint16_t)y; e.w = (uint16_t)w; e.h = (uint16_t)h;
    e.min_x = VRAM_W; e.min_y = VRAM_H;
    SDL_LockMutex(s_native_state_mutex);
    if (!s_native_native_words) {
        SDL_UnlockMutex(s_native_state_mutex);
        s_audit_unavailable++;
        s_audit_busy = 0;
        return;
    }
    {
        uint32_t other = 0;
        for (int j = 0; j < h; j++) {
            const int py = (y + j) & (VRAM_H - 1);
            for (int i = 0; i < w; i++) {
                const int px = (x + i) & (VRAM_W - 1);
                const uint16_t g = gl_words[py * VRAM_W + px];
                const uint16_t n = s_native_native_words[py * VRAM_W + px];
                if (g == n) continue;
                if ((uint16_t)(g ^ n) != 0x8000u) other++;
                if (e.count < AUDIT_FIRST)
                    e.first[e.count] = (AuditMismatch){ (uint16_t)px, (uint16_t)py, g, n };
                e.count++;
                if (px < e.min_x) e.min_x = (uint16_t)px;
                if (py < e.min_y) e.min_y = (uint16_t)py;
                if (px > e.max_x) e.max_x = (uint16_t)px;
                if (py > e.max_y) e.max_y = (uint16_t)py;
            }
        }
        e.mask_only = e.count != 0u && other == 0u;
    }
    SDL_UnlockMutex(s_native_state_mutex);
    e.shown = e.count < AUDIT_FIRST ? (uint8_t)e.count : AUDIT_FIRST;

    audit_lock();
    s_audit_runs[reason]++;
    if (e.count) {
        s_audit_bad[reason]++;
        s_audit_pixels += e.count;
        if (e.mask_only) s_audit_mask_only++;
        /* Mode 3 keeps the root-cause command entries (and their snapshot)
         * from being displaced by the guest's later per-pixel readbacks. */
        if (gpu_native_canonical_audit_mode() < 3 || reason == GPU_NATIVE_AUDIT_COMMAND) {
            e.seq = ++s_audit_seq;
            s_audit_ring[(e.seq - 1u) % AUDIT_RING] = e;
            if (s_snap_state == 0) { s_snap_state = 1; snapshot = 1; }
        }
    }
    audit_unlock();
    s_audit_covered = 0;
    if (!e.count && (reason == GPU_NATIVE_AUDIT_PREPARE_READ ||
                     reason == GPU_NATIVE_AUDIT_TRANSFER_OUT) &&
        x + w <= VRAM_W && y + h <= VRAM_H) {
        s_audit_covered = 1;
        s_audit_cov[0] = x; s_audit_cov[1] = y; s_audit_cov[2] = w; s_audit_cov[3] = h;
    }

    if (snapshot) {
        /* Make the whole host mirror current (the rect read pulled only its
         * own region), then keep both full copies. */
        gr_vram_prepare_read(0, 0, VRAM_W, VRAM_H);
        if (!s_snap_gl) s_snap_gl = malloc(VRAM_W * VRAM_H * sizeof(uint16_t));
        if (!s_snap_native) s_snap_native = malloc(VRAM_W * VRAM_H * sizeof(uint16_t));
        if (s_snap_gl && s_snap_native) {
            memcpy(s_snap_gl, gl_words, VRAM_W * VRAM_H * sizeof(uint16_t));
            SDL_LockMutex(s_native_state_mutex);
            memcpy(s_snap_native, s_native_native_words,
                   VRAM_W * VRAM_H * sizeof(uint16_t));
            SDL_UnlockMutex(s_native_state_mutex);
            s_snap_meta = e;
        }
        audit_lock();
        s_snap_state = (s_snap_gl && s_snap_native) ? 2 : 0;
        audit_unlock();
        /* PSX_NATIVE_CANONICAL_AUDIT_DUMP=<path>: write the first snapshot
         * as <path>.gl.bin / <path>.native.bin without a debug client. */
        static int dumped_once;
        if (!dumped_once && s_snap_state == 2 &&
            getenv("PSX_NATIVE_CANONICAL_AUDIT_DUMP")) {
            dumped_once = 1;
            char *dumped = gpu_native_canonical_audit_dump(0,
                getenv("PSX_NATIVE_CANONICAL_AUDIT_DUMP"));
            free(dumped);
        }
    }
    s_audit_busy = 0;
}

static int audit_sext11(uint32_t v) { return (int)((int32_t)(v << 21) >> 21); }

/* Destination rect of one GP0 draw/fill/copy, before draw-area clipping for
 * fills/copies (those ignore it) and after for draws. 0 = nothing drawn. */
static int audit_command_rect(const uint32_t *w, int n,
                              const GpuNativeDrawEnvironment *env,
                              int *rx, int *ry, int *rw, int *rh) {
    const unsigned op = w[0] >> 24;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);

    if (op == 0x02u && n >= 3) {
        *rx = (int)(w[1] & 0x3f0u); *ry = (int)((w[1] >> 16) & 0x1ffu);
        *rw = (int)(((w[2] & 0x3ffu) + 0xfu) & ~0xfu);
        *rh = (int)((w[2] >> 16) & 0x1ffu);
        return *rw > 0 && *rh > 0;
    }
    if (op >= 0x80u && op <= 0x9fu && n >= 4) {
        *rx = (int)(w[2] & 0x3ffu); *ry = (int)((w[2] >> 16) & 0x1ffu);
        *rw = (int)((((w[3] & 0x3ffu) - 1u) & 0x3ffu) + 1u);
        *rh = (int)((((w[3] >> 16) - 1u) & 0x1ffu) + 1u);
        return 1;
    }
    if (op >= 0x20u && op <= 0x3fu) {
        const int verts = (op & 8u) ? 4 : 3, tex = (op & 4u) != 0, gour = (op & 0x10u) != 0;
        int idx = 1;
        for (int v = 0; v < verts && idx < n; v++) {
            if (v && gour) idx++;
            if (idx >= n) break;
            const int x = audit_sext11(w[idx]), y = audit_sext11(w[idx] >> 16);
            if (x < x0) x0 = x; if (x > x1) x1 = x;
            if (y < y0) y0 = y; if (y > y1) y1 = y;
            idx += 1 + (tex ? 1 : 0);
        }
    } else if (op >= 0x40u && op <= 0x5fu) {
        const int gour = (op & 0x10u) != 0;
        for (int idx = 1; idx < n; idx += gour ? 2 : 1) {
            if ((w[idx] & 0xf000f000u) == 0x50005000u) break;
            const int x = audit_sext11(w[idx]), y = audit_sext11(w[idx] >> 16);
            if (x < x0) x0 = x; if (x > x1) x1 = x;
            if (y < y0) y0 = y; if (y > y1) y1 = y;
        }
    } else if (op >= 0x60u && op <= 0x7fu && n >= 2) {
        const int tex = (op & 4u) != 0, size = (int)((op >> 3) & 3u);
        const int x = audit_sext11(w[1]), y = audit_sext11(w[1] >> 16);
        int sw = size == 1 ? 1 : size == 2 ? 8 : 16, sh = sw;
        if (size == 0) {
            const int si = 2 + (tex ? 1 : 0);
            if (si >= n) return 0;
            sw = (int)(w[si] & 0x3ffu); sh = (int)((w[si] >> 16) & 0x1ffu);
        }
        if (sw <= 0 || sh <= 0) return 0;
        x0 = x; x1 = x + sw - 1; y0 = y; y1 = y + sh - 1;
    } else return 0;
    if (x1 < x0 || y1 < y0) return 0;
    x0 += env->draw.offset_x; x1 += env->draw.offset_x;
    y0 += env->draw.offset_y; y1 += env->draw.offset_y;
    if (x0 < (int)env->draw.left) x0 = env->draw.left;
    if (y0 < (int)env->draw.top) y0 = env->draw.top;
    if (x1 > (int)env->draw.right) x1 = env->draw.right;
    if (y1 > (int)env->draw.bottom) y1 = env->draw.bottom;
    if (x1 < x0 || y1 < y0 || x1 >= VRAM_W + 1024 || y1 >= VRAM_H) {
        if (y1 >= VRAM_H) y1 = VRAM_H - 1;
        if (x1 < x0 || y1 < y0) return 0;
    }
    if (x1 >= VRAM_W) x1 = VRAM_W - 1;
    *rx = x0; *ry = y0; *rw = x1 - x0 + 1; *rh = y1 - y0 + 1;
    return 1;
}

#define AUDIT_CMD_MISMATCH_LIMIT 64

void gpu_native_canonical_audit_command(const uint32_t *words, int word_count,
                                        const void *environment) {
    const GpuNativeDrawEnvironment *env = environment;
    int x, y, w, h;

    if (gpu_native_canonical_audit_mode() < 3 || s_audit_busy || !words ||
        word_count < 1 || !env || gr_vram_owner_is_native())
        return;
    if (!audit_command_rect(words, word_count, env, &x, &y, &w, &h)) return;
    if (s_audit_cmd_bad >= AUDIT_CMD_MISMATCH_LIMIT) return;
    s_audit_cmd_index++;
    s_audit_cmd_count = word_count < 16 ? word_count : 16;
    memcpy(s_audit_cmd_words, words, (size_t)s_audit_cmd_count * sizeof(uint32_t));
    s_audit_cmd_env = *env;
    s_audit_cmd_active = 1;
    {
        const uint64_t before = s_audit_seq;
        gr_vram_prepare_read(x, y, w, h);
        if (s_audit_seq != before) s_audit_cmd_bad++;
    }
    s_audit_cmd_active = 0;
}

void gpu_native_canonical_audit_vblank(void) {
    if (!gpu_native_canonical_audit_mode()) return;
    s_audit_vblank++;
    if (gpu_native_canonical_audit_mode() >= 2 && !s_audit_busy) {
        /* The host's prepare_read audits with its own reason; relabel. */
        s_audit_vblank_reason = 1;
        gr_vram_prepare_read(0, 0, VRAM_W, VRAM_H);
        s_audit_vblank_reason = 0;
    }
}

char *gpu_native_canonical_audit_json(int id, int n) {
    AuditEntry *copy = malloc(AUDIT_RING * sizeof(*copy));
    uint64_t runs[GPU_NATIVE_AUDIT_REASON_COUNT], bad[GPU_NATIVE_AUDIT_REASON_COUNT];
    uint64_t seq, vb, mo, px, un;
    int have, snap;
    size_t cap, pos = 0;
    char *out;
    int count;

    if (!copy) return NULL;
    audit_lock();
    memcpy(copy, s_audit_ring, AUDIT_RING * sizeof(*copy));
    memcpy(runs, s_audit_runs, sizeof(runs));
    memcpy(bad, s_audit_bad, sizeof(bad));
    seq = s_audit_seq; vb = s_audit_vblank; mo = s_audit_mask_only;
    px = s_audit_pixels; un = s_audit_unavailable;
    snap = s_snap_state;
    audit_unlock();
    have = seq < AUDIT_RING ? (int)seq : AUDIT_RING;
    count = (n > 0 && n < have) ? n : have;
    cap = 1024 + (size_t)count * 1700;
    out = malloc(cap);
    if (!out) { free(copy); return NULL; }
    pos += snprintf(out + pos, cap - pos,
        "{\"id\":%d,\"ok\":true,\"mode\":%d,\"vblank\":%llu,\"mismatching_audits\":%llu,"
        "\"mask_only_audits\":%llu,\"mismatch_pixels\":%llu,\"unavailable\":%llu,"
        "\"snapshot_held\":%d,\"reasons\":{",
        id, gpu_native_canonical_audit_mode(), (unsigned long long)vb,
        (unsigned long long)seq, (unsigned long long)mo,
        (unsigned long long)px, (unsigned long long)un, snap == 2);
    for (int r = 0; r < GPU_NATIVE_AUDIT_REASON_COUNT; r++)
        pos += snprintf(out + pos, cap - pos,
            "%s\"%s\":{\"audits\":%llu,\"mismatching\":%llu}", r ? "," : "",
            AUDIT_REASON_NAME[r], (unsigned long long)runs[r],
            (unsigned long long)bad[r]);
    pos += snprintf(out + pos, cap - pos, "},\"entries\":[");
    for (int i = 0; i < count; i++) {
        const AuditEntry *e = &copy[(seq - (uint64_t)count + (uint64_t)i) % AUDIT_RING];
        pos += snprintf(out + pos, cap - pos,
            "%s{\"seq\":%llu,\"vblank\":%llu,\"cycle\":%llu,\"reason\":\"%s\","
            "\"rect\":[%u,%u,%u,%u],\"count\":%u,\"mask_only\":%s,"
            "\"bbox\":[%u,%u,%u,%u],\"cmd_index\":%llu,",
            i ? "," : "", (unsigned long long)e->seq,
            (unsigned long long)e->vblank, (unsigned long long)e->cycle,
            AUDIT_REASON_NAME[e->reason], e->x, e->y, e->w, e->h, e->count,
            e->mask_only ? "true" : "false",
            e->min_x, e->min_y, e->max_x, e->max_y,
            (unsigned long long)e->cmd_index);
        if (e->cmd_count) {
            const GpuDrawState *d = &e->cmd_env.draw;
            pos += snprintf(out + pos, cap - pos, "\"cmd\":[");
            for (int k = 0; k < e->cmd_count; k++)
                pos += snprintf(out + pos, cap - pos, "%s\"0x%08X\"", k ? "," : "", e->cmd_words[k]);
            pos += snprintf(out + pos, cap - pos,
                "],\"env\":{\"tpage\":%u,\"area\":[%u,%u,%u,%u],\"offset\":[%d,%d],"
                "\"twin\":[%u,%u,%u,%u],\"dither\":%u,\"mask_set\":%u,\"mask_check\":%u},",
                e->cmd_env.tpage, d->left, d->top, d->right, d->bottom,
                d->offset_x, d->offset_y, d->texture_window_mask_x, d->texture_window_mask_y,
                d->texture_window_offset_x, d->texture_window_offset_y,
                d->dither, d->mask_set, d->mask_check);
        }
        pos += snprintf(out + pos, cap - pos, "\"first\":[");
        for (int k = 0; k < e->shown; k++)
            pos += snprintf(out + pos, cap - pos,
                "%s{\"x\":%u,\"y\":%u,\"gl\":\"0x%04X\",\"native\":\"0x%04X\"}",
                k ? "," : "", e->first[k].x, e->first[k].y,
                e->first[k].gl, e->first[k].native);
        pos += snprintf(out + pos, cap - pos, "]}");
    }
    snprintf(out + pos, cap - pos, "]}");
    free(copy);
    return out;
}

char *gpu_native_canonical_audit_dump(int id, const char *path) {
    char name[1100];
    char *out = malloc(512);
    AuditEntry m;
    FILE *f;
    int ok = 1;

    if (!out) return NULL;
    audit_lock();
    if (s_snap_state != 2) {
        audit_unlock();
        snprintf(out, 512, "{\"id\":%d,\"ok\":false,\"error\":\"no snapshot held\"}", id);
        return out;
    }
    s_snap_state = 3;
    m = s_snap_meta;
    audit_unlock();
    for (int which = 0; which < 2 && ok; which++) {
        snprintf(name, sizeof(name), "%s.%s.bin", path, which ? "native" : "gl");
        f = fopen(name, "wb");
        ok = f && fwrite(which ? s_snap_native : s_snap_gl, sizeof(uint16_t),
                         VRAM_W * VRAM_H, f) == (size_t)VRAM_W * VRAM_H;
        if (f) ok = fclose(f) == 0 && ok;
    }
    audit_lock();
    s_snap_state = 0;
    audit_unlock();
    snprintf(out, 512,
        "{\"id\":%d,\"ok\":%s,\"seq\":%llu,\"vblank\":%llu,\"cycle\":%llu,"
        "\"reason\":\"%s\",\"rect\":[%u,%u,%u,%u],\"count\":%u,\"mask_only\":%s}",
        id, ok ? "true" : "false", (unsigned long long)m.seq,
        (unsigned long long)m.vblank, (unsigned long long)m.cycle,
        AUDIT_REASON_NAME[m.reason], m.x, m.y, m.w, m.h, m.count,
        m.mask_only ? "true" : "false");
    return out;
}
