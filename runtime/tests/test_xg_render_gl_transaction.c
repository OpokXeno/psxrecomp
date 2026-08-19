#include "gpu.h"
#include "gpu_gl_renderer.h"
#include "gpu_render.h"
#include "guest_render_native_stream.h"

#include "psx_sdl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef PSX_GL_TRANSACTION_TESTING
#error "test_xg_render_gl_transaction.c requires PSX_GL_TRANSACTION_TESTING"
#endif

/* gpu.c's unrelated platform hooks are not reached by this focused path. */
int g_exec_phase;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint64_t s_frame_count;

void *debug_cpu_ptr(void) { return NULL; }
uint32_t debug_guest_ra(void) { return 0; }
uint32_t debug_guest_sp(void) { return 0; }
void event_ring_record_aux(void) {}
int gte_geometry_correction_enabled(void) { return 0; }
int gte_geometry_correction_lookup(void) { return 0; }
void gte_precision_load_word(void) {}
void gte_precision_tracking_set(void) {}
int mdec_recently_active(void) { return 0; }
uint8_t *memory_get_ram_ptr(void) { return NULL; }
void psx_fatal_halt(void) {}
void psx_irq_raise(void) {}
uint8_t psx_read_byte(void) { return 0; }
uint16_t psx_read_half(void) { return 0; }
uint32_t psx_read_word(void) { return 0; }
void psx_write_half(void) {}
void psx_write_word(void) {}
void text_xlate_vram_upload(void) {}
void latency_ring_mark(void) {}
const GpuRenderBackend *vk_backend_get(void) { return NULL; }
extern const GpuRenderBackend *gl_backend_get(void);

enum {
    TEST_X = 420,
    TEST_Y = 300,
    SAMPLE_X = TEST_X + 2,
    SAMPLE_Y = TEST_Y + 2,
    PRESENT_SAMPLE_X = 160,
    PRESENT_SAMPLE_Y = 120,
    BLACK_1555 = 0x0000,
    RED_1555 = 0x001f,
    WHITE_1555 = 0x7fff,
    DITHER_X = 460,
    DITHER_Y = 340,
    DITHER_REGION_W = 12,
    DITHER_REGION_H = 12,
};

typedef enum DitherCase {
    DITHER_CASE_FLAT = 0,
    DITHER_CASE_GOURAUD,
    DITHER_CASE_TEXTURED,
    DITHER_CASE_SEMITRANSPARENT,
    DITHER_CASE_COUNT
} DitherCase;

static const int dither_vertex_x[3] = { DITHER_X, DITHER_X + 10, DITHER_X };
static const int dither_vertex_y[3] = { DITHER_Y, DITHER_Y, DITHER_Y + 10 };
static const int dither_vertex_u[3] = { 32, 42, 32 };
static const int dither_vertex_v[3] = { 40, 40, 50 };
static const uint32_t dither_colors[DITHER_CASE_COUNT][3] = {
    { UINT32_C(0x13579b), UINT32_C(0x13579b), UINT32_C(0x13579b) },
    { UINT32_C(0x123456), UINT32_C(0x89abcd), UINT32_C(0x55c37a) },
    { UINT32_C(0x6b91d5), UINT32_C(0x6b91d5), UINT32_C(0x6b91d5) },
    { UINT32_C(0x9f7351), UINT32_C(0x9f7351), UINT32_C(0x9f7351) },
};

static int failures;
static SDL_Window *test_window;
static SDL_GLContext test_gl_context;

#ifdef PSX_DEBUG_OVERLAY
static uint64_t overlay_pre_swap_calls;
static unsigned int overlay_last_target;

void psx_debug_overlay_pre_swap_target(unsigned int framebuffer) {
    overlay_pre_swap_calls++;
    overlay_last_target = framebuffer;
}

void psx_debug_overlay_pre_swap(void) {
    psx_debug_overlay_pre_swap_target(0u);
}

static void reset_overlay_pre_swap_diag(void) {
    overlay_pre_swap_calls = 0u;
    overlay_last_target = 0u;
}
#else
static void reset_overlay_pre_swap_diag(void) {}
#endif

static void expect_true(int condition, const char *label) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static void expect_status(GpuRenderTransactionStatus actual,
                          GpuRenderTransactionStatus expected,
                          const char *label) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: expected status %d, got %d\n",
                label, (int)expected, (int)actual);
        failures++;
    }
}

static void expect_swap_status(GlRendererTransactionSwapStatus actual,
                               GlRendererTransactionSwapStatus expected,
                               const char *label) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: expected swap status %d, got %d\n",
                label, (int)expected, (int)actual);
        failures++;
    }
}

static void expect_pixel_at(int x, int y, uint16_t expected,
                            const char *label) {
    uint16_t actual = UINT16_C(0xffff);

    expect_true(gl_renderer_fbo_peek(x, y, 1, 1, &actual),
                 "transaction pixel is readable from the GL FBO");
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: expected 0x%04x, got 0x%04x\n",
                label, expected, actual);
        failures++;
    }
}

static void expect_pixel(uint16_t expected, const char *label) {
    expect_pixel_at(SAMPLE_X, SAMPLE_Y, expected, label);
}

static GpuRenderTransactionId transaction_id(uint64_t sequence) {
    GpuRenderTransactionId id = { UINT64_C(0x584700000000000e), sequence };

    return id;
}

static GpuRenderPresent canonical_present(void) {
    GpuRenderPresent present;

    memset(&present, 0, sizeof(present));
    present.path = GPU_RENDER_PRESENT_CANONICAL;
    present.display_x = 0;
    present.display_y = 0;
    present.display_width = 320;
    present.display_height = 240;
    present.surface_width = 1024u * (uint32_t)gr_scale();
    present.surface_height = 512u * (uint32_t)gr_scale();
    present.scale = (uint32_t)gr_scale();
    return present;
}

static GpuRenderSemantic flat_triangle(void) {
    GpuRenderSemantic semantic;
    static const int x[3] = { TEST_X - 3, TEST_X + 5, TEST_X - 3 };
    static const int y[3] = { TEST_Y + 2, TEST_Y + 2, TEST_Y + 10 };

    memset(&semantic, 0, sizeof(semantic));
    semantic.material.tpage = UINT16_C(0x0100);
    semantic.material.texture_depth = GPU_RENDER_TEXTURE_15_BIT;
    semantic.material.shading = GPU_RENDER_SHADING_FLAT;
    semantic.material.blend_mode = GPU_RENDER_BLEND_AVERAGE;
    semantic.material.draw_area_right = 1023u;
    semantic.material.draw_area_bottom = 511u;
    semantic.material.draw_offset_x = 3;
    semantic.material.draw_offset_y = -2;
    semantic.triangle_count = 1u;
    semantic.triangles[0].split_count = 1u;
    for (int i = 0; i < 3; i++) {
        GpuRenderSemanticVertex *vertex = &semantic.triangles[0].vertices[i];

        vertex->x = x[i] * INT32_C(65536);
        vertex->y = y[i] * INT32_C(65536);
        vertex->r = 255u;
        vertex->g = 255u;
        vertex->b = 255u;
    }
    return semantic;
}

static GpuRenderSemantic ordered_overlapping_triangles(void) {
    GpuRenderSemantic semantic = flat_triangle();

    semantic.material.semi_transparent = 1u;
    semantic.triangle_count = 2u;
    semantic.triangles[0].split_count = 2u;
    semantic.triangles[1] = semantic.triangles[0];
    semantic.triangles[1].split_index = 1u;
    for (int i = 0; i < 3; i++) {
        semantic.triangles[0].vertices[i].r = 255u;
        semantic.triangles[0].vertices[i].g = 0u;
        semantic.triangles[0].vertices[i].b = 0u;
        semantic.triangles[1].vertices[i].r = 0u;
        semantic.triangles[1].vertices[i].g = 0u;
        semantic.triangles[1].vertices[i].b = 255u;
    }
    return semantic;
}

static GpuRenderSemantic centered_flat_triangle(void) {
    GpuRenderSemantic semantic = flat_triangle();
    static const int x[3] = { 97, 217, 97 };
    static const int y[3] = { 102, 102, 222 };

    for (int i = 0; i < 3; i++) {
        semantic.triangles[0].vertices[i].x = x[i] * INT32_C(65536);
        semantic.triangles[0].vertices[i].y = y[i] * INT32_C(65536);
    }
    return semantic;
}

static uint32_t gp0_xy(int x, int y) {
    return (uint32_t)(x & 0x7ff) | ((uint32_t)(y & 0x7ff) << 16);
}

static uint32_t gp0_draw_offset(int x, int y) {
    return UINT32_C(0xe5000000) | (uint32_t)(x & 0x7ff) |
           ((uint32_t)(y & 0x7ff) << 11);
}

static void configure_dither_draw_state(DitherCase draw_case,
                                        int offset_x, int offset_y) {
    const int left = DITHER_X + offset_x + 1;
    const int top = DITHER_Y + offset_y + 1;
    const int right = DITHER_X + offset_x + 8;
    const int bottom = DITHER_Y + offset_y + 8;
    const unsigned int mask_set = draw_case == DITHER_CASE_TEXTURED ? 1u : 0u;
    const unsigned int mask_check =
        draw_case == DITHER_CASE_SEMITRANSPARENT ? 1u : 0u;

    gpu_write_gp0(UINT32_C(0xe1000300)); /* 15-bit page 0 + dither. */
    gpu_write_gp0(UINT32_C(0xe3000000) | (uint32_t)left |
                  ((uint32_t)top << 10));
    gpu_write_gp0(UINT32_C(0xe4000000) | (uint32_t)right |
                  ((uint32_t)bottom << 10));
    gpu_write_gp0(gp0_draw_offset(offset_x, offset_y));
    gpu_write_gp0(UINT32_C(0xe6000000) | mask_set | (mask_check << 1));
}

static void prepare_dither_vram(DitherCase draw_case,
                                int offset_x, int offset_y) {
    const uint16_t backdrop = draw_case == DITHER_CASE_SEMITRANSPARENT
        ? UINT16_C(0x14a5) : UINT16_C(0x0000);

    for (int y = 0; y < DITHER_REGION_H; y++) {
        for (int x = 0; x < DITHER_REGION_W; x++) {
            gr_vram_write(DITHER_X + offset_x + x,
                          DITHER_Y + offset_y + y, backdrop);
        }
    }
    if (draw_case == DITHER_CASE_SEMITRANSPARENT) {
        gr_vram_write(DITHER_X + offset_x + 3,
                      DITHER_Y + offset_y + 3,
                      (uint16_t)(backdrop | UINT16_C(0x8000)));
    }
    if (draw_case >= DITHER_CASE_TEXTURED) {
        const uint16_t stp = draw_case == DITHER_CASE_SEMITRANSPARENT
            ? UINT16_C(0x8000) : UINT16_C(0x0000);

        for (int v = 38; v <= 52; v++) {
            for (int u = 30; u <= 44; u++) {
                uint16_t texel = (uint16_t)((((u * 3 + v) & 31) << 0) |
                    (((u + v * 5) & 31) << 5) |
                    (((u * 7 + v * 3) & 31) << 10));
                if ((texel & UINT16_C(0x7fff)) == 0u) texel = 1u;
                gr_vram_write(u, v, (uint16_t)(texel | stp));
            }
        }
    }
    gl_renderer_flush_cpu_uploads();
}

static void submit_original_dither_triangle(DitherCase draw_case) {
    const uint32_t *color = dither_colors[draw_case];

    if (draw_case == DITHER_CASE_FLAT) {
        gpu_write_gp0(UINT32_C(0x20000000) | color[0]);
        for (int i = 0; i < 3; i++)
            gpu_write_gp0(gp0_xy(dither_vertex_x[i], dither_vertex_y[i]));
    } else if (draw_case == DITHER_CASE_GOURAUD) {
        gpu_write_gp0(UINT32_C(0x30000000) | color[0]);
        gpu_write_gp0(gp0_xy(dither_vertex_x[0], dither_vertex_y[0]));
        gpu_write_gp0(color[1]);
        gpu_write_gp0(gp0_xy(dither_vertex_x[1], dither_vertex_y[1]));
        gpu_write_gp0(color[2]);
        gpu_write_gp0(gp0_xy(dither_vertex_x[2], dither_vertex_y[2]));
    } else {
        const uint32_t opcode = draw_case == DITHER_CASE_SEMITRANSPARENT
            ? UINT32_C(0x26000000) : UINT32_C(0x24000000);

        gpu_write_gp0(opcode | color[0]);
        gpu_write_gp0(gp0_xy(dither_vertex_x[0], dither_vertex_y[0]));
        gpu_write_gp0((uint32_t)dither_vertex_u[0] |
                      ((uint32_t)dither_vertex_v[0] << 8));
        gpu_write_gp0(gp0_xy(dither_vertex_x[1], dither_vertex_y[1]));
        gpu_write_gp0((uint32_t)dither_vertex_u[1] |
                      ((uint32_t)dither_vertex_v[1] << 8) |
                      UINT32_C(0x01000000));
        gpu_write_gp0(gp0_xy(dither_vertex_x[2], dither_vertex_y[2]));
        gpu_write_gp0((uint32_t)dither_vertex_u[2] |
                      ((uint32_t)dither_vertex_v[2] << 8));
    }
}

static GpuRenderSemantic semantic_dither_triangle(DitherCase draw_case,
                                                   int offset_x,
                                                   int offset_y) {
    GpuRenderSemantic semantic;

    memset(&semantic, 0, sizeof(semantic));
    semantic.material.tpage = UINT16_C(0x0100);
    semantic.material.texture_depth = GPU_RENDER_TEXTURE_15_BIT;
    semantic.material.shading = draw_case == DITHER_CASE_GOURAUD
        ? GPU_RENDER_SHADING_GOURAUD : GPU_RENDER_SHADING_FLAT;
    semantic.material.textured = draw_case >= DITHER_CASE_TEXTURED;
    semantic.material.semi_transparent =
        draw_case == DITHER_CASE_SEMITRANSPARENT;
    semantic.material.blend_mode = GPU_RENDER_BLEND_AVERAGE;
    semantic.material.dither = 1u;
    semantic.material.mask_set = draw_case == DITHER_CASE_TEXTURED;
    semantic.material.mask_check =
        draw_case == DITHER_CASE_SEMITRANSPARENT;
    semantic.material.draw_area_left =
        (uint16_t)(DITHER_X + offset_x + 1);
    semantic.material.draw_area_top =
        (uint16_t)(DITHER_Y + offset_y + 1);
    semantic.material.draw_area_right =
        (uint16_t)(DITHER_X + offset_x + 8);
    semantic.material.draw_area_bottom =
        (uint16_t)(DITHER_Y + offset_y + 8);
    semantic.material.draw_offset_x = (int16_t)offset_x;
    semantic.material.draw_offset_y = (int16_t)offset_y;
    semantic.triangle_count = 1u;
    semantic.triangles[0].split_count = 1u;
    for (int i = 0; i < 3; i++) {
        GpuRenderSemanticVertex *vertex = &semantic.triangles[0].vertices[i];

        vertex->x = dither_vertex_x[i] * INT32_C(65536);
        vertex->y = dither_vertex_y[i] * INT32_C(65536);
        vertex->u = dither_vertex_u[i] * INT32_C(65536);
        vertex->v = dither_vertex_v[i] * INT32_C(65536);
        vertex->r = (uint8_t)(dither_colors[draw_case][i] & 0xffu);
        vertex->g = (uint8_t)((dither_colors[draw_case][i] >> 8) & 0xffu);
        vertex->b = (uint8_t)((dither_colors[draw_case][i] >> 16) & 0xffu);
    }
    return semantic;
}

static uint64_t dither_region_hash(const uint16_t *pixels) {
    uint64_t hash = UINT64_C(1469598103934665603);

    for (int i = 0; i < DITHER_REGION_W * DITHER_REGION_H; i++) {
        hash ^= pixels[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void prepare_zoom_vram(void) {
    for (int y = 0; y < 224; y++) {
        for (int x = 0; x < 64; x++) {
            uint16_t texel = (uint16_t)((((x + y) & 31) << 0) |
                (((x * 3 + y) & 31) << 5) |
                (((x + y * 5) & 31) << 10) | UINT16_C(0x8000));
            if ((texel & UINT16_C(0x7fff)) == 0u) texel |= 1u;
            gr_vram_write(704 + x, 256 + y, texel);
            gr_vram_write(x, y, UINT16_C(0x2108));
        }
    }
    gl_renderer_flush_cpu_uploads();
}

static void submit_original_zoom_quad(void) {
    gpu_write_gp0(UINT32_C(0xe100013b));
    gpu_write_gp0(UINT32_C(0xe3000000));
    gpu_write_gp0(UINT32_C(0xe4037c3f));
    gpu_write_gp0(UINT32_C(0xe5000000));
    gpu_write_gp0(UINT32_C(0xe6000000));
    gpu_write_gp0(UINT32_C(0x2e808080));
    gpu_write_gp0(gp0_xy(0, 0));
    gpu_write_gp0(UINT32_C(0x00000000));
    gpu_write_gp0(gp0_xy(64, 0));
    gpu_write_gp0(UINT32_C(0x013b0040));
    gpu_write_gp0(gp0_xy(0, 223));
    gpu_write_gp0(UINT32_C(0x0000df00));
    gpu_write_gp0(gp0_xy(64, 223));
    gpu_write_gp0(UINT32_C(0x0000df40));
}

static GpuRenderSemantic semantic_zoom_quad(void) {
    static const uint8_t split[2][3] = { { 0, 1, 2 }, { 2, 1, 3 } };
    static const int x[4] = { 0, 64, 0, 64 };
    static const int y[4] = { 0, 0, 223, 223 };
    static const int u[4] = { 0, 64, 0, 64 };
    static const int v[4] = { 0, 0, 223, 223 };
    GpuRenderSemantic semantic;

    memset(&semantic, 0, sizeof(semantic));
    semantic.material.tpage = UINT16_C(0x013b);
    semantic.material.texture_page_x = 11u;
    semantic.material.texture_page_y = 1u;
    semantic.material.texture_depth = GPU_RENDER_TEXTURE_15_BIT;
    semantic.material.blend_mode = GPU_RENDER_BLEND_ADD;
    semantic.material.shading = GPU_RENDER_SHADING_FLAT;
    semantic.material.textured = 1u;
    semantic.material.semi_transparent = 1u;
    semantic.material.draw_area_right = 319u;
    semantic.material.draw_area_bottom = 223u;
    semantic.triangle_count = 2u;
    for (int triangle = 0; triangle < 2; triangle++) {
        semantic.triangles[triangle].split_index = (uint8_t)triangle;
        semantic.triangles[triangle].split_count = 2u;
        for (int vertex_index = 0; vertex_index < 3; vertex_index++) {
            const int source = split[triangle][vertex_index];
            GpuRenderSemanticVertex *vertex =
                &semantic.triangles[triangle].vertices[vertex_index];
            vertex->x = x[source] * INT32_C(65536);
            vertex->y = y[source] * INT32_C(65536);
            vertex->u = u[source] * INT32_C(65536);
            vertex->v = v[source] * INT32_C(65536);
            vertex->r = vertex->g = vertex->b = 0x80u;
        }
    }
    return semantic;
}

static void test_immediate_zoom_matches_original_gp0(void) {
    uint16_t original[64 * 224];
    uint16_t native[64 * 224];
    GpuRenderSemantic semantic = semantic_zoom_quad();

    prepare_zoom_vram();
    submit_original_zoom_quad();
    expect_true(gl_renderer_fbo_peek(0, 0, 64, 224, original),
                "Original GP0 zoom result is readable");

    prepare_zoom_vram();
    expect_status(gr_stream_barrier(), GPU_RENDER_TRANSACTION_OK,
                  "zoom immediate ordering opens");
    expect_status(gr_draw_semantic_immediate(&semantic),
                  GPU_RENDER_TRANSACTION_OK, "zoom immediate semantic draws");
    expect_status(gr_stream_barrier(), GPU_RENDER_TRANSACTION_OK,
                  "zoom immediate ordering closes");
    expect_true(gl_renderer_fbo_peek(0, 0, 64, 224, native),
                "immediate semantic zoom result is readable");
    expect_true(memcmp(original, native, sizeof(original)) == 0,
                "immediate semantic tpage-Y1 ABR1 FT4 matches Original GP0");
}

static void set_test_pixel(uint16_t pixel) {
    gr_vram_write(SAMPLE_X, SAMPLE_Y, pixel);
    gl_renderer_flush_cpu_uploads();
    expect_pixel(pixel, "setup pixel reaches canonical GL VRAM");
}

static void test_begin_requires_context(void) {
    expect_status(gr_transaction_begin(transaction_id(1u), 1u),
                  GPU_RENDER_TRANSACTION_CONTEXT_LOST,
                  "begin fails closed before GL raster initialization");
}

static void test_native_fmv_surface_path(void) {
    const GpuRenderBackend *backend = gl_backend_get();
    const uint32_t pixel = UINT32_C(0xff204080);

    expect_true(backend != NULL && backend->present_native_cpu_frame != NULL,
                "OpenGL backend exposes an independent Native FMV callback");
    expect_true(gr_present_native_cpu_frame(&pixel, 1, 1, 0, 1, 0) != 0,
                "Native FMV callback presents through its separate surface");
}

static void test_ready_then_immediate_swap(void) {
    const GpuRenderTransactionId id = transaction_id(2u);
    GpuRenderSemantic semantic = flat_triangle();
    GpuRenderPresent present = canonical_present();
    GpuRenderTransactionStatus commit_status;
    GlRendererTransactionSwapStatus swap_status =
        GL_RENDERER_TRANSACTION_SWAP_NOT_READY;
    uint64_t presents_before;
    GlPresEvent event;

    set_test_pixel(BLACK_1555);
    gl_renderer_transaction_test_reset();
    reset_overlay_pre_swap_diag();
    presents_before = gl_renderer_pres_total();
    expect_status(gr_transaction_begin(id, 20u), GPU_RENDER_TRANSACTION_OK,
                  "commit case begins");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "commit case opens semantic ordering");
    expect_status(gr_draw_semantic(id, &semantic), GPU_RENDER_TRANSACTION_OK,
                  "flat semantic draws through GL");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "commit case closes semantic ordering");
    commit_status = gr_commit_validate(id, 20u, &present);
    if (commit_status == GPU_RENDER_TRANSACTION_READY)
        swap_status = gl_renderer_swap_ready_transaction();
    expect_status(commit_status, GPU_RENDER_TRANSACTION_READY,
                  "canonical transaction commits READY");
    expect_swap_status(swap_status, GL_RENDERER_TRANSACTION_SWAP_SUCCESS,
                       "READY is consumed only by the explicit swap API");
    {
        GlRendererTransactionTestDiag diag;

        gl_renderer_transaction_test_diag(&diag);
        expect_true(diag.pending_commit == 0 &&
                    diag.staging_compositions == 1u &&
                    diag.default_writes_before_final_blit == 0u &&
                    diag.final_blits == 1u && diag.swaps == 1u &&
                    diag.publications == 1u &&
                    diag.operations_after_final_validation == 0u,
                    "commit blits once, then immediate swap publishes once");
#ifdef PSX_DEBUG_OVERLAY
        expect_true(overlay_pre_swap_calls == 1u && overlay_last_target != 0u,
                    "commit targets the Debug overlay at staging exactly once");
#endif
    }
    expect_true(gl_renderer_pres_total() == presents_before + 1u,
                "present ring publishes only after the transactional swap");
    expect_true(gl_renderer_pres_get(presents_before, &event) &&
                event.path == GL_PRES_VRAM && event.dx == 0 && event.dy == 0 &&
                event.w == 320 && event.h == 240 && event.glerr == 0,
                "deferred ring entry describes the swapped staged frame");
    expect_pixel(WHITE_1555, "committed semantic changes the canonical pixel");
}

static void test_rollback_restores_pixel(void) {
    const GpuRenderTransactionId id = transaction_id(3u);
    GpuRenderSemantic semantic = flat_triangle();
    uint64_t presents_before;

    set_test_pixel(BLACK_1555);
    presents_before = gl_renderer_pres_total();
    expect_status(gr_transaction_begin(id, 30u), GPU_RENDER_TRANSACTION_OK,
                  "rollback case begins");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "rollback case opens semantic ordering");
    expect_status(gr_draw_semantic(id, &semantic), GPU_RENDER_TRANSACTION_OK,
                  "rollback case draws a semantic");
    expect_pixel(WHITE_1555, "transactional draw is visible before rollback");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "rollback succeeds");
    expect_true(gl_renderer_pres_total() == presents_before,
                "rollback never swaps");
    expect_pixel(BLACK_1555,
                 "rollback restores CPU raw VRAM and canonical GL mirrors");
}

static void test_split_triangle_order(void) {
    const GpuRenderTransactionId id = transaction_id(11u);
    GpuRenderSemantic semantic = ordered_overlapping_triangles();
    GpuRenderPresent present = canonical_present();

    set_test_pixel(BLACK_1555);
    expect_status(gr_transaction_begin(id, 110u), GPU_RENDER_TRANSACTION_OK,
                  "split-order case begins");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "split-order case opens semantic ordering");
    expect_status(gr_draw_semantic(id, &semantic), GPU_RENDER_TRANSACTION_OK,
                  "ordered split semantic draws");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "split-order case closes semantic ordering");
    expect_status(gr_commit_validate(id, 110u, &present),
                  GPU_RENDER_TRANSACTION_READY,
                  "split-order case commits");
    expect_swap_status(gl_renderer_swap_ready_transaction(),
                       GL_RENDERER_TRANSACTION_SWAP_SUCCESS,
                       "split-order READY transaction swaps explicitly");
    expect_pixel(UINT16_C(0x3c07),
                 "second blue split blends after the first red split");
}

static uint64_t begin_white_transaction(GpuRenderTransactionId id,
                                         uint64_t serial) {
    GpuRenderSemantic semantic = flat_triangle();
    uint64_t checkpoint_coh;

    expect_status(gr_transaction_begin(id, serial), GPU_RENDER_TRANSACTION_OK,
                  "fault case begins");
    checkpoint_coh = gl_renderer_coh_total();
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "fault case opens semantic ordering");
    expect_status(gr_draw_semantic(id, &semantic), GPU_RENDER_TRANSACTION_OK,
                  "fault case draws");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "fault case closes semantic ordering");
    return checkpoint_coh;
}

static GpuRenderDeferredCandidateToken capture_centered_candidate(
        GpuRenderTransactionId id, uint64_t serial) {
    GpuRenderSemantic semantic = centered_flat_triangle();
    GpuRenderDeferredCandidateToken token =
        GPU_RENDER_DEFERRED_CANDIDATE_NONE;

    gr_vram_write(PRESENT_SAMPLE_X, PRESENT_SAMPLE_Y, BLACK_1555);
    gl_renderer_flush_cpu_uploads();
    expect_status(gr_transaction_begin(id, serial), GPU_RENDER_TRANSACTION_OK,
                  "candidate source transaction begins");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "candidate source opens semantic ordering");
    expect_status(gr_draw_semantic(id, &semantic), GPU_RENDER_TRANSACTION_OK,
                  "candidate source draws a centered semantic");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "candidate source closes semantic ordering");
    expect_pixel_at(PRESENT_SAMPLE_X, PRESENT_SAMPLE_Y, WHITE_1555,
                    "candidate source is complete before capture");
    expect_status(gr_deferred_candidate_capture(id, &token),
                  GPU_RENDER_TRANSACTION_OK,
                  "private full-frame candidate captures");
    expect_true(token != GPU_RENDER_DEFERRED_CANDIDATE_NONE,
                "candidate capture returns an opaque nonzero token");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "candidate source transaction rolls back");
    expect_pixel_at(PRESENT_SAMPLE_X, PRESENT_SAMPLE_Y, BLACK_1555,
                    "source rollback restores canonical VRAM");
    return token;
}

static void test_deferred_candidate_commit(void) {
    const GpuRenderTransactionId id = transaction_id(270u);
    const uint64_t serial = 270u;
    GpuRenderDeferredCandidateToken token;
    GpuRenderPresent present = canonical_present();
    GlRendererTransactionTestDiag diag;
    GlPresEvent event;
    uint64_t presents_before;

    gl_renderer_transaction_test_reset();
    reset_overlay_pre_swap_diag();
    presents_before = gl_renderer_pres_total();
    token = capture_centered_candidate(id, serial);
    gl_renderer_transaction_test_diag(&diag);
    expect_true(diag.deferred_candidate_captures == 1u &&
                diag.deferred_candidate_discards == 0u &&
                diag.deferred_candidate_active == 1,
                "source rollback retains exactly one private candidate");

    expect_status(gr_deferred_transaction_begin(id, serial + 1u, token),
                  GPU_RENDER_TRANSACTION_OK,
                  "deferred transaction opens from the retained token");
    expect_status(gr_commit_validate(id, serial + 1u, &present),
                  GPU_RENDER_TRANSACTION_READY,
                  "deferred candidate composes to READY");
    expect_true(gl_renderer_pres_total() == presents_before,
                "deferred READY remains unpublished before swap");
    expect_swap_status(gl_renderer_swap_ready_transaction(),
                       GL_RENDERER_TRANSACTION_SWAP_SUCCESS,
                       "deferred READY swaps explicitly");
    expect_true(gl_renderer_pres_get(presents_before, &event) &&
                event.px_r >= 248u && event.px_g >= 248u &&
                event.px_b >= 248u && event.src_valid &&
                event.src_r == 0u && event.src_g == 0u && event.src_b == 0u,
                "staged frame uses the white candidate while canonical VRAM stays black");
    expect_pixel_at(PRESENT_SAMPLE_X, PRESENT_SAMPLE_Y, BLACK_1555,
                    "deferred swap does not reapply candidate pixels to VRAM");
    gl_renderer_transaction_test_diag(&diag);
    expect_true(diag.deferred_transaction_begins == 1u &&
                diag.deferred_candidate_discards == 1u &&
                diag.deferred_candidate_active == 0 &&
                diag.swaps == 1u && diag.publications == 1u,
                "successful deferred swap consumes and releases candidate once");
    expect_status(gr_deferred_candidate_discard(token),
                  GPU_RENDER_TRANSACTION_STATE_REJECTED,
                  "consumed candidate token cannot be reused");
}

static void test_deferred_candidate_discard_and_identity(void) {
    const GpuRenderTransactionId id = transaction_id(280u);
    const GpuRenderTransactionId other = transaction_id(281u);
    GpuRenderDeferredCandidateToken token;
    GlRendererTransactionTestDiag diag;

    gl_renderer_transaction_test_reset();
    token = capture_centered_candidate(id, 280u);
    expect_status(gr_deferred_transaction_begin(other, 281u, token),
                  GPU_RENDER_TRANSACTION_STATE_REJECTED,
                  "candidate rejects a different visual identity");
    gl_renderer_transaction_test_diag(&diag);
    expect_true(diag.deferred_candidate_active == 1 &&
                diag.deferred_transaction_begins == 0u,
                "identity rejection leaves the candidate privately owned");
    expect_status(gr_deferred_candidate_discard(token),
                  GPU_RENDER_TRANSACTION_OK,
                  "explicit invalidation discards the retained candidate");
    gl_renderer_transaction_test_diag(&diag);
    expect_true(diag.deferred_candidate_active == 0 &&
                diag.deferred_candidate_discards == 1u,
                "explicit invalidation releases exactly one candidate");
    expect_status(gr_deferred_transaction_begin(id, 281u, token),
                  GPU_RENDER_TRANSACTION_STATE_REJECTED,
                  "discarded token cannot open a deferred transaction");
}

static void test_deferred_commit_faults_release_candidate(void) {
    static const int phases[] = {
        GL_TRANSACTION_FAULT_POST_COMPOSITION,
        GL_TRANSACTION_FAULT_FINAL_VALIDATION,
        GL_TRANSACTION_FAULT_FINAL_BLIT,
    };

    for (size_t index = 0; index < sizeof(phases) / sizeof(phases[0]); index++) {
        const uint64_t serial = UINT64_C(290) + index;
        const GpuRenderTransactionId id = transaction_id(serial);
        GpuRenderDeferredCandidateToken token;
        GpuRenderPresent present = canonical_present();
        GlRendererTransactionTestDiag diag;
        uint64_t presents_before;

        gl_renderer_transaction_test_reset();
        presents_before = gl_renderer_pres_total();
        token = capture_centered_candidate(id, serial);
        expect_status(gr_deferred_transaction_begin(id, serial + 10u, token),
                      GPU_RENDER_TRANSACTION_OK,
                      "fault case opens deferred transaction");
        gl_renderer_transaction_test_inject_fault(phases[index]);
        expect_status(gr_commit_validate(id, serial + 10u, &present),
                      GPU_RENDER_TRANSACTION_BACKEND_ERROR,
                      "injected deferred commit phase fails before READY");
        expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                      "failed deferred commit remains rollbackable");
        gl_renderer_transaction_test_diag(&diag);
        expect_true(diag.phase_failures == 1u &&
                    diag.last_fault_phase == phases[index] &&
                    diag.deferred_candidate_captures == 1u &&
                    diag.deferred_transaction_begins == 1u &&
                    diag.deferred_candidate_discards == 1u &&
                    diag.deferred_candidate_active == 0 &&
                    diag.swaps == 0u && diag.publications == 0u &&
                    gl_renderer_pres_total() == presents_before,
                    "deferred commit fault releases candidate without publication");
        expect_pixel_at(PRESENT_SAMPLE_X, PRESENT_SAMPLE_Y, BLACK_1555,
                        "deferred fault rollback preserves canonical VRAM");
    }
}

static void test_commit_phase_failures_remain_rollbackable(void) {
    static const int phases[] = {
        GL_TRANSACTION_FAULT_POST_COMPOSITION,
        GL_TRANSACTION_FAULT_FINAL_VALIDATION,
        GL_TRANSACTION_FAULT_FINAL_BLIT,
    };

    for (size_t index = 0; index < sizeof(phases) / sizeof(phases[0]); index++) {
        const uint64_t serial = UINT64_C(200) + index;
        const GpuRenderTransactionId id = transaction_id(serial);
        GlRendererTransactionTestDiag diag;
        GpuRenderPresent present = canonical_present();
        uint64_t checkpoint_coh;
        uint64_t presents_before;

        set_test_pixel(BLACK_1555);
        gl_renderer_transaction_test_reset();
        reset_overlay_pre_swap_diag();
        presents_before = gl_renderer_pres_total();
        checkpoint_coh = begin_white_transaction(id, serial);
        gl_renderer_transaction_test_inject_fault(phases[index]);
        expect_status(gr_commit_validate(id, serial, &present),
                      GPU_RENDER_TRANSACTION_BACKEND_ERROR,
                      "injected commit phase fails before READY");

        gl_renderer_transaction_test_diag(&diag);
        expect_true(diag.pending_commit == 0 &&
                    diag.commits_ready == 0u &&
                    diag.staging_compositions == 1u &&
                    diag.default_writes_before_final_blit == 0u &&
                    diag.final_blits == 0u && diag.swaps == 0u &&
                    diag.publications == 0u && diag.phase_failures == 1u &&
                    diag.last_fault_phase == phases[index],
                    "injected commit phase fails before READY/swap/publication");
        expect_true(gl_renderer_pres_total() == presents_before,
                    "failed commit phase publishes no present record");
#ifdef PSX_DEBUG_OVERLAY
        expect_true(overlay_pre_swap_calls == 1u && overlay_last_target != 0u,
                    "failed commit attempt prepares the staged overlay once");
#endif
        expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                      "failed commit phase remains backend-rollbackable");
        expect_true(gl_renderer_coh_total() == checkpoint_coh,
                    "rollback rewinds failed commit coherency events");
        expect_pixel(BLACK_1555,
                     "rollback restores failed commit checkpoint VRAM");
        expect_true(gl_renderer_pres_total() == presents_before,
                    "rollback after commit fault remains unpublished");
    }
}

static void test_normal_present_does_not_consume_ready(void) {
    const GpuRenderTransactionId id = transaction_id(230u);
    GpuRenderPresent present = canonical_present();
    GlRendererTransactionTestDiag diag;
    uint64_t presents_before;

    set_test_pixel(BLACK_1555);
    gl_renderer_transaction_test_reset();
    reset_overlay_pre_swap_diag();
    presents_before = gl_renderer_pres_total();
    (void)begin_white_transaction(id, 230u);
    expect_status(gr_commit_validate(id, 230u, &present),
                  GPU_RENDER_TRANSACTION_READY,
                  "normal-present rejection case reaches READY");
    gl_renderer_transaction_test_diag(&diag);
    expect_true(diag.pending_commit == 1 &&
                diag.staging_compositions == 1u && diag.final_blits == 1u &&
                diag.swaps == 0u && diag.publications == 0u,
                "READY includes composition/final blit but remains unpublished");
    expect_true(gl_renderer_pres_total() == presents_before,
                "READY alone publishes no present record");
#ifdef PSX_DEBUG_OVERLAY
    expect_true(overlay_pre_swap_calls == 1u && overlay_last_target != 0u,
                "READY composition invokes only the staged overlay hook");
#endif

    gl_renderer_present_vram(0, 0, 319, 240, 0, 0);
    gl_renderer_transaction_test_diag(&diag);
    expect_true(diag.pending_commit == 1 && diag.final_blits == 1u &&
                diag.swaps == 0u && diag.publications == 0u &&
                gl_renderer_pres_total() == presents_before,
                "ordinary VRAM present rejects READY without consuming it");
    expect_swap_status(gl_renderer_swap_ready_transaction(),
                       GL_RENDERER_TRANSACTION_SWAP_SUCCESS,
                       "explicit swap still consumes READY after safe rejection");
    expect_true(gl_renderer_pres_total() == presents_before + 1u,
                "only the explicit swap publishes the retained READY frame");
    expect_pixel(WHITE_1555,
                 "explicitly swapped READY transaction remains canonical");
}

static void test_commit_context_loss_remains_rollbackable(void) {
    const GpuRenderTransactionId id = transaction_id(235u);
    GpuRenderPresent present = canonical_present();
    GlRendererTransactionTestDiag diag;
    uint64_t presents_before;

    set_test_pixel(BLACK_1555);
    gl_renderer_transaction_test_reset();
    presents_before = gl_renderer_pres_total();
    (void)begin_white_transaction(id, 235u);
    expect_true(SDL_GL_MakeCurrent(test_window, NULL) == 0,
                "test detaches the transaction GL context");
    expect_status(gr_commit_validate(id, 235u, &present),
                  GPU_RENDER_TRANSACTION_CONTEXT_LOST,
                  "commit fails closed when context ownership is lost");
    expect_true(SDL_GL_MakeCurrent(test_window, test_gl_context) == 0,
                "test restores the transaction GL context");
    gl_renderer_transaction_test_diag(&diag);
    expect_true(diag.pending_commit == 0 && diag.final_blits == 0u &&
                diag.swaps == 0u && diag.publications == 0u &&
                gl_renderer_pres_total() == presents_before,
                "context-loss commit has no default write or publication");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "context-loss commit remains rollbackable after ownership returns");
    expect_pixel(BLACK_1555,
                 "context-loss rollback restores the checkpoint");
}

static void test_swap_requires_ready(void) {
    const GpuRenderTransactionId id = transaction_id(238u);
    GlRendererTransactionTestDiag diag;
    uint64_t presents_before;

    gl_renderer_transaction_test_reset();
    presents_before = gl_renderer_pres_total();
    expect_swap_status(gl_renderer_swap_ready_transaction(),
                       GL_RENDERER_TRANSACTION_SWAP_NOT_READY,
                       "explicit swap rejects an absent transaction");
    expect_status(gr_transaction_begin(id, 238u), GPU_RENDER_TRANSACTION_OK,
                  "pre-READY swap case begins");
    expect_swap_status(gl_renderer_swap_ready_transaction(),
                       GL_RENDERER_TRANSACTION_SWAP_NOT_READY,
                       "explicit swap rejects an uncommitted transaction");
    gl_renderer_transaction_test_diag(&diag);
    expect_true(diag.swaps == 0u && diag.publications == 0u &&
                gl_renderer_pres_total() == presents_before,
                "NOT_READY performs no swap or publication");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "NOT_READY leaves the open checkpoint rollbackable");
}

static void test_noncanonical_present_routes_fail_closed(void) {
    uint32_t cpu_pixel = 0;

    for (int route = 0; route < 3; route++) {
        const uint64_t serial = UINT64_C(240) + (uint64_t)route;
        const GpuRenderTransactionId id = transaction_id(serial);
        GpuRenderPresent present = canonical_present();
        GlRendererTransactionTestDiag diag;
        uint64_t presents_before;

        set_test_pixel(BLACK_1555);
        gl_renderer_transaction_test_reset();
        presents_before = gl_renderer_pres_total();
        (void)begin_white_transaction(id, serial);
        expect_status(gr_commit_validate(id, serial, &present),
                      GPU_RENDER_TRANSACTION_READY,
                      "noncanonical-route case reaches READY");
        if (route == 0) {
            gl_renderer_present_blank();
        } else if (route == 1) {
            gl_renderer_present(&cpu_pixel, 1, 1, 0, 0, 0);
        } else {
            expect_true(gl_renderer_present_wide_fbo(0, 0, 240, 0) == 1,
                        "transactional wide rejection suppresses CPU fallback");
        }
        gl_renderer_transaction_test_diag(&diag);
        expect_true(diag.pending_commit == 1 && diag.final_blits == 1u &&
                     diag.swaps == 0u && diag.publications == 0u &&
                     gl_renderer_pres_total() == presents_before,
                    "blank/CPU/wide route cannot consume or publish READY");
        expect_swap_status(gl_renderer_swap_ready_transaction(),
                           GL_RENDERER_TRANSACTION_SWAP_SUCCESS,
                           "explicit swap consumes READY after route rejection");
        expect_true(gl_renderer_pres_total() == presents_before + 1u,
                    "rejected route publishes only through explicit swap");
        expect_pixel(WHITE_1555,
                     "explicit swap preserves the committed route frame");
    }
}

static void test_stale_serial_then_rollback(void) {
    const GpuRenderTransactionId id = transaction_id(4u);
    GpuRenderSemantic semantic = flat_triangle();
    GpuRenderPresent present = canonical_present();

    set_test_pixel(BLACK_1555);
    expect_status(gr_transaction_begin(id, 40u), GPU_RENDER_TRANSACTION_OK,
                  "stale-serial case begins");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "stale-serial case opens semantic ordering");
    expect_status(gr_draw_semantic(id, &semantic), GPU_RENDER_TRANSACTION_OK,
                  "stale-serial case draws");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "stale-serial case drains");
    expect_status(gr_commit_validate(id, 41u, &present),
                  GPU_RENDER_TRANSACTION_STATE_REJECTED,
                  "changed VRAM serial rejects commit");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "stale-serial transaction remains rollbackable");
    expect_pixel(BLACK_1555, "stale-serial rollback restores the old pixel");
}

static void test_unsupported_semantic_has_no_partial_draw(void) {
    GpuRenderTransactionId id = transaction_id(5u);
    GpuRenderSemantic semantic = flat_triangle();

    semantic.triangle_count = 2u;
    semantic.triangles[0].split_count = 2u;
    semantic.triangles[1] = semantic.triangles[0];
    semantic.triangles[1].split_index = 1u;
    semantic.triangles[1].vertices[0].x += 1;

    set_test_pixel(BLACK_1555);
    expect_status(gr_transaction_begin(id, 50u), GPU_RENDER_TRANSACTION_OK,
                  "unsupported-semantic case begins");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "unsupported-semantic case opens ordering");
    expect_status(gr_draw_semantic(id, &semantic),
                  GPU_RENDER_TRANSACTION_UNSUPPORTED,
                  "fractional split rejects the whole semantic");
    expect_pixel(BLACK_1555,
                 "preflight prevents the valid first split from drawing");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "unsupported semantic remains rollbackable");

    id = transaction_id(6u);
    semantic = flat_triangle();
    semantic.material.dither = 1u;
    expect_status(gr_transaction_begin(id, 60u), GPU_RENDER_TRANSACTION_OK,
                  "dither case begins");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "dither case opens ordering");
    expect_status(gr_draw_semantic(id, &semantic),
                  GPU_RENDER_TRANSACTION_OK,
                  "PS1 dithering is accepted by semantic preflight");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "dithered semantic remains rollbackable");
}

static void test_semantic_dither_matches_original_gp0(void) {
    static const int offsets[2][2] = { { 0, 0 }, { 3, -2 } };
    uint64_t hashes[DITHER_CASE_COUNT][2];
    uint64_t sequence = UINT64_C(300);

    for (int draw_case = 0; draw_case < DITHER_CASE_COUNT; draw_case++) {
        for (int offset_index = 0; offset_index < 2; offset_index++) {
            const int offset_x = offsets[offset_index][0];
            const int offset_y = offsets[offset_index][1];
            const GpuRenderTransactionId id = transaction_id(sequence);
            GpuRenderSemantic semantic = semantic_dither_triangle(
                (DitherCase)draw_case, offset_x, offset_y);
            uint16_t original[DITHER_REGION_W * DITHER_REGION_H];
            uint16_t semantic_pixels[DITHER_REGION_W * DITHER_REGION_H];
            int same;

            prepare_dither_vram((DitherCase)draw_case, offset_x, offset_y);
            configure_dither_draw_state((DitherCase)draw_case,
                                        offset_x, offset_y);
            submit_original_dither_triangle((DitherCase)draw_case);
            expect_true(gl_renderer_fbo_peek(
                            DITHER_X + offset_x, DITHER_Y + offset_y,
                            DITHER_REGION_W, DITHER_REGION_H, original),
                        "Original GP0 dither result is readable");

            prepare_dither_vram((DitherCase)draw_case, offset_x, offset_y);
            expect_status(gr_transaction_begin(id, sequence),
                          GPU_RENDER_TRANSACTION_OK,
                          "dither parity transaction begins");
            expect_status(gr_ordering_barrier(id),
                          GPU_RENDER_TRANSACTION_OK,
                          "dither parity opens semantic ordering");
            expect_status(gr_draw_semantic(id, &semantic),
                          GPU_RENDER_TRANSACTION_OK,
                          "dither parity semantic draws");
            expect_status(gr_ordering_barrier(id),
                          GPU_RENDER_TRANSACTION_OK,
                          "dither parity closes semantic ordering");
            expect_true(gl_renderer_fbo_peek(
                            DITHER_X + offset_x, DITHER_Y + offset_y,
                            DITHER_REGION_W, DITHER_REGION_H,
                            semantic_pixels),
                        "semantic dither result is readable");

            same = memcmp(original, semantic_pixels, sizeof(original)) == 0;
            if (!same) {
                int mismatch = 0;
                while (mismatch < DITHER_REGION_W * DITHER_REGION_H &&
                       original[mismatch] == semantic_pixels[mismatch])
                    mismatch++;
                fprintf(stderr,
                        "FAIL: dither parity case %d offset (%d,%d) at (%d,%d): Original 0x%04x semantic 0x%04x\n",
                        draw_case, offset_x, offset_y,
                        mismatch % DITHER_REGION_W,
                        mismatch / DITHER_REGION_W,
                        mismatch < DITHER_REGION_W * DITHER_REGION_H
                            ? original[mismatch] : 0u,
                        mismatch < DITHER_REGION_W * DITHER_REGION_H
                            ? semantic_pixels[mismatch] : 0u);
                failures++;
            }
            hashes[draw_case][offset_index] =
                dither_region_hash(semantic_pixels);
            expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                          "dither parity semantic rolls back");
            sequence++;
        }
    }

    expect_true(hashes[DITHER_CASE_FLAT][0] ==
                    hashes[DITHER_CASE_FLAT][1],
                "flat polygons quantize without applying the dither matrix");
    expect_true(hashes[DITHER_CASE_GOURAUD][0] !=
                    hashes[DITHER_CASE_GOURAUD][1],
                "Gouraud dither phase follows final draw-offset coordinates");
}

static void test_invalid_present_and_identity_gates(void) {
    GpuRenderTransactionId id = transaction_id(7u);
    const GpuRenderTransactionId other = transaction_id(8u);
    GpuRenderPresent present = canonical_present();

    expect_status(gr_transaction_begin(id, 70u), GPU_RENDER_TRANSACTION_OK,
                  "nested-begin case begins");
    expect_status(gr_transaction_begin(other, 70u),
                  GPU_RENDER_TRANSACTION_INVALID_TRANSITION,
                  "nested transaction fails closed");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "nested-begin rejection keeps the checkpoint rollbackable");

    id = transaction_id(7u);
    expect_status(gr_transaction_begin(id, 70u), GPU_RENDER_TRANSACTION_OK,
                  "identity-gate case begins");
    expect_status(gr_ordering_barrier(other),
                  GPU_RENDER_TRANSACTION_STATE_REJECTED,
                  "wrong transaction identity fails closed");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "original identity can still roll back");

    id = transaction_id(9u);
    expect_status(gr_transaction_begin(id, 90u), GPU_RENDER_TRANSACTION_OK,
                  "invalid-present case begins");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "invalid-present case has final barrier");
    present.path = GPU_RENDER_PRESENT_HIRES;
    expect_status(gr_commit_validate(id, 90u, &present),
                  GPU_RENDER_TRANSACTION_UNSUPPORTED,
                  "unstaged hires final composition is not claimed");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "invalid present remains rollbackable");

    id = transaction_id(12u);
    present = canonical_present();
    present.display_width = 0;
    expect_status(gr_transaction_begin(id, 120u), GPU_RENDER_TRANSACTION_OK,
                  "malformed-present case begins");
    expect_status(gr_ordering_barrier(id), GPU_RENDER_TRANSACTION_OK,
                  "malformed-present case has final barrier");
    expect_status(gr_commit_validate(id, 120u, &present),
                  GPU_RENDER_TRANSACTION_VALIDATION_FAILED,
                  "zero-width canonical present fails validation");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "malformed present remains rollbackable");
}

static void test_interpolation_history_gate(void) {
    const GpuRenderTransactionId id = transaction_id(10u);
    int enabled = 0, history = 0;

    gr_vram_write(0, 0, BLACK_1555);
    gl_renderer_flush_cpu_uploads();
    gl_renderer_set_interpolation(1, 120.0, 120.0, 0);
    gl_renderer_present_vram(0, 0, 320, 240, 0, 0);
    gl_renderer_interpolation_diag(&enabled, NULL, &history, NULL, NULL, NULL);
    expect_true(enabled == 1 && history > 0,
                "interpolation is active with retained history");
    expect_status(gr_transaction_begin(id, 100u),
                  GPU_RENDER_TRANSACTION_STATE_REJECTED,
                  "active interpolation/history rejects begin");

    gl_renderer_set_interpolation(0, 60.0, 60.0, 0);
    gl_renderer_interpolation_diag(&enabled, NULL, &history, NULL, NULL, NULL);
    expect_true(enabled == 0 && history == 0,
                "disabled interpolation has quiesced history");
    expect_status(gr_transaction_begin(id, 100u), GPU_RENDER_TRANSACTION_OK,
                  "quiesced interpolation permits begin");
    expect_status(gr_rollback(id), GPU_RENDER_TRANSACTION_OK,
                  "post-interpolation checkpoint rolls back");
}

static void write_sourced_gp0(uint32_t command_address,
                              const uint32_t *words,
                              size_t word_count) {
    for (size_t index = 0u; index < word_count; ++index) {
        const GpuRenderOracleSource source = {
            GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST,
            command_address + (uint32_t)index * 4u,
            command_address / 4u + index,
            (command_address - 4u) / 4u,
        };

        expect_true(gpu_native_submit_gp0_word(words[index], &source),
                    "Native GP0 packet word is accepted");
    }
}

static void test_native_stream_consumes_gp0_anchor(void) {
    const uint32_t command_address = UINT32_C(0x00102004);
    const uint32_t original_words[] = {
        UINT32_C(0x20ffffff),
        gp0_xy(TEST_X, TEST_Y),
        gp0_xy(TEST_X + 8, TEST_Y),
        gp0_xy(TEST_X, TEST_Y + 8),
    };
    const GpuRenderTransactionId id = transaction_id(300u);
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic;
    GuestRenderNativeStreamSnapshot snapshot;

    gr_vram_write(SAMPLE_X, SAMPLE_Y, BLACK_1555);
    gr_vram_write(SAMPLE_X + 100, SAMPLE_Y, BLACK_1555);
    gl_renderer_flush_cpu_uploads();
    gpu_write_gp0(UINT32_C(0xe3000000));
    gpu_write_gp0(UINT32_C(0xe407ffff));
    gpu_write_gp0(UINT32_C(0xe5000000));
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    original_words,
                    sizeof(original_words) / sizeof(original_words[0]),
                    &environment, &semantic) == 1,
                "GP0 anchor produces its authenticated Native semantic");
    semantic.material.draw_offset_x = 100;
    guest_render_native_stream_set_enabled(true);
    guest_render_native_stream_clear();
    expect_true(guest_render_native_stream_stage_exact(
                    id, command_address, &semantic) ==
                    GUEST_RENDER_NATIVE_STREAM_OK,
                "Native semantic binds to its exact GP0 command address");
    expect_true(guest_render_native_stream_activate_visual(id) ==
                    GUEST_RENDER_NATIVE_STREAM_OK,
                "Native visual state becomes authoritative atomically");
    {
        const GpuRenderOracleSource source = {
            GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST,
            command_address, command_address / 4u,
            (command_address - 4u) / 4u,
        };
        expect_true(gpu_native_preflight_reservation_begin(),
                    "Native DMA preflight reservation begins");
        expect_true(gpu_native_preflight_gp0_packet(
                        original_words,
                        sizeof(original_words) / sizeof(original_words[0]),
                        &source),
                    "authenticated GP0 binding resolves once during preflight");
        expect_true(gpu_native_preflight_reservation_seal(),
                    "Native DMA preflight reservation seals atomically");
    }
    write_sourced_gp0(command_address, original_words,
                      sizeof(original_words) / sizeof(original_words[0]));
    expect_pixel(WHITE_1555,
                 "Native semantic uses ordered GPU raster state at consumption");
    expect_pixel_at(SAMPLE_X + 100, SAMPLE_Y, BLACK_1555,
                    "stale staged draw offset is not used for Native raster");
    expect_true(guest_render_native_stream_snapshot(&snapshot) ==
                    GUEST_RENDER_NATIVE_STREAM_OK &&
                snapshot.staged_count == 0u &&
                snapshot.total_staged == 1u &&
                snapshot.total_consumed == 1u &&
                snapshot.total_visual_states == 1u,
                "Native stream consumes the binding exactly once");
    guest_render_native_stream_set_enabled(false);
}

static void test_canonical_framebuffer_digest(void) {
    uint64_t first = 0u;
    uint64_t second = 0u;
    uint64_t unchanged = 0u;

    gr_vram_write(0, 0, BLACK_1555);
    expect_true(gr_canonical_framebuffer_digest(0, 0, 1, 1, &first),
                "canonical framebuffer digest is available");
    gr_vram_write(0, 0, RED_1555);
    expect_true(gr_canonical_framebuffer_digest(0, 0, 1, 1, &second),
                "updated canonical framebuffer digest is available");
    expect_true(first != second,
                "canonical framebuffer digest binds RGBA8 pixels");
    gr_vram_write(1, 0, RED_1555);
    expect_true(gr_canonical_framebuffer_digest(0, 0, 1, 1, &unchanged),
                "canonical framebuffer region remains available");
    expect_true(second == unchanged,
                "canonical framebuffer digest excludes off-display pixels");
    expect_true(!gr_canonical_framebuffer_digest(-1, 0, 1, 1, &unchanged),
                "invalid canonical framebuffer region is rejected");
}

static void test_shutdown_with_pending_deferred_ready(void) {
    const GpuRenderTransactionId id = transaction_id(260u);
    GpuRenderPresent present = canonical_present();
    GlRendererTransactionTestDiag diag;
    GpuRenderDeferredCandidateToken token;

    gl_renderer_transaction_test_reset();
    token = capture_centered_candidate(id, 260u);
    expect_status(gr_deferred_transaction_begin(id, 261u, token),
                  GPU_RENDER_TRANSACTION_OK,
                  "shutdown case opens a deferred transaction");
    expect_status(gr_commit_validate(id, 261u, &present),
                   GPU_RENDER_TRANSACTION_READY,
                   "shutdown deferred case reaches READY");
    gl_renderer_transaction_test_diag(&diag);
    expect_true(diag.pending_commit == 1 &&
                diag.deferred_candidate_active == 1,
                "shutdown case retains a READY checkpoint and candidate");
}

int main(void) {
    const GpuRenderBackend *backend;
    SDL_Window *window;

#if !defined(PSX_SDL3)
    SDL_SetMainReady();
#endif
    SDL_setenv("PSX_GL_PRESENT_PROBE", "1", 1);
    expect_true(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL video initializes");
    if (failures) return 1;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    window = SDL_CreateWindow("xg_render_gl_transaction_test",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, 32, 32,
                              SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    expect_true(window != NULL, "hidden SDL OpenGL window is created");
    if (!window) {
        SDL_Quit();
        return 1;
    }

    backend = gl_backend_get();
    expect_true(backend && backend->transaction_begin &&
                backend->ordering_barrier && backend->draw_semantic &&
                backend->stream_barrier &&
                backend->draw_semantic_immediate &&
                backend->canonical_framebuffer_digest &&
                backend->commit_validate && backend->rollback &&
                backend->deferred_candidate_capture &&
                backend->deferred_candidate_discard &&
                backend->deferred_transaction_begin,
                "OpenGL backend exposes ordinary and deferred transaction callbacks");
    gr_set_backend(GR_BACKEND_OPENGL);
    gpu_init();
    gr_set_scale(2);
    gr_set_texture_filter(0);
    test_begin_requires_context();
    expect_true(gl_renderer_init_context(window),
                 "OpenGL raster pipeline initializes");
    test_window = window;
    test_gl_context = SDL_GL_GetCurrentContext();
    expect_true(gr_backend() == GR_BACKEND_OPENGL,
                "OpenGL backend remains selected");

    if (!failures) test_canonical_framebuffer_digest();
    if (!failures) test_native_fmv_surface_path();
    if (!failures) test_ready_then_immediate_swap();
    if (!failures) test_rollback_restores_pixel();
    if (!failures) test_split_triangle_order();
    if (!failures) test_commit_phase_failures_remain_rollbackable();
    if (!failures) test_normal_present_does_not_consume_ready();
    if (!failures) test_commit_context_loss_remains_rollbackable();
    if (!failures) test_swap_requires_ready();
    if (!failures) test_noncanonical_present_routes_fail_closed();
    if (!failures) test_stale_serial_then_rollback();
    if (!failures) test_unsupported_semantic_has_no_partial_draw();
    if (!failures) test_semantic_dither_matches_original_gp0();
    if (!failures) test_immediate_zoom_matches_original_gp0();
    if (!failures) test_native_stream_consumes_gp0_anchor();
    if (!failures) test_invalid_present_and_identity_gates();
    if (!failures) test_interpolation_history_gate();
    if (!failures) test_deferred_candidate_commit();
    if (!failures) test_deferred_candidate_discard_and_identity();
    if (!failures) test_deferred_commit_faults_release_candidate();
    if (!failures) test_shutdown_with_pending_deferred_ready();

    gl_renderer_shutdown();
    {
        GlRendererTransactionTestDiag diag;

        gl_renderer_transaction_test_diag(&diag);
        expect_true(diag.pending_commit == 0 &&
                    diag.deferred_candidate_active == 0 &&
                    diag.deferred_candidate_discards == 1u,
                    "shutdown restores READY state and releases its candidate");
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) return 1;
    puts("PASS: OpenGL semantic transactions commit and roll back atomically");
    return 0;
}
