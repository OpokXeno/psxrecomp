#include "gpu.h"
#include "gpu_gl_renderer.h"
#include "gpu_render.h"
#include "gpu_semantic_workload.h"
#include "gte_native_provenance.h"
#include "memory.h"

#include "psx_sdl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* gpu.c's unrelated platform hooks are not reached by this focused GP0 path. */
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
uint32_t memory_get_ram_word_mask(void) {
    return g_psx_ram_mask & ~UINT32_C(3);
}
int gte_native_provenance_load(uint32_t address, uint32_t packed_sxy,
                               GteNativeVertexProvenance *out) {
    (void)address;
    (void)packed_sxy;
    (void)out;
    return 0;
}

enum {
    MASKED_BLACK_1555 = 0x8000,
    WHITE_1555 = 0x7fff,
    TEXTURE_PAGE_15BPP = 0x0100,
    FLAT_X = 300,
    FLAT_Y = 300,
    TEXTURED_X = 320,
    TEXTURED_Y = 300,
};

static int failures;

static void expect_true(int condition, const char *label) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static void expect_pixel(uint16_t actual, uint16_t expected, const char *label) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: expected 0x%04x, got 0x%04x\n",
                label, expected, actual);
        failures++;
    }
}

static uint32_t gp0_xy(int x, int y) {
    return (uint32_t)(x & 0x3ff) | ((uint32_t)(y & 0x1ff) << 16);
}

static void set_draw_band(int y) {
    gpu_write_gp0(UINT32_C(0xe3000000) | ((uint32_t)y << 10u));
    gpu_write_gp0(UINT32_C(0xe4000000) | UINT32_C(319) |
                  ((uint32_t)(y + 239) << 10u));
    gpu_write_gp0(UINT32_C(0xe5000000) |
                  (((uint32_t)y & UINT32_C(0x7ff)) << 11u));
}

static void set_display_band(int y) {
    gpu_write_gp1(UINT32_C(0x05000000) | ((uint32_t)y << 10u));
}

static void semantic_set_draw_band(GpuRenderSemantic *semantic, int y) {
    semantic->material.draw_area_top = (uint16_t)y;
    semantic->material.draw_area_bottom = (uint16_t)(y + 239);
    semantic->material.draw_offset_y = (int16_t)y;
}

static uint16_t argb8888_to_rgb555(uint32_t pixel) {
    return (uint16_t)(((pixel >> 3u) & 0x1fu) |
                      ((pixel >> 6u) & 0x03e0u) |
                      ((pixel >> 9u) & 0x7c00u));
}

static void reset_gpu_for_case(void) {
    gpu_init();
    gpu_write_gp0(0xe3000000u);
    gpu_write_gp0(0xe407ffffu);
}

static int read_fbo_pixel(int x, int y, uint16_t *pixel) {
    return gl_renderer_fbo_peek(x, y, 1, 1, pixel);
}

static void test_native_environment_tpage_latching(void) {
    GpuNativeDrawEnvironment environment = {0};
    uint32_t untextured_quad[5] = {
        0x28000000u, 0u, 0u, 0u, 0x01a00000u,
    };
    uint32_t textured_triangle[7] = {
        0x24000000u, 0u, 0u, 0u, 0x00550000u, 0u, 0u,
    };
    uint32_t shaded_textured_triangle[9] = {
        0x34000000u, 0u, 0u, 0u, 0u,
        0x00770000u, 0u, 0u, 0u,
    };

    environment.tpage = 0x0012u;
    gpu_native_environment_apply(untextured_quad, 5, &environment);
    expect_true(environment.tpage == 0x0012u,
                "untextured quad does not latch a tpage word");

    gpu_native_environment_apply(textured_triangle, 7, &environment);
    expect_true(environment.tpage == 0x0055u,
                "flat textured triangle latches word four tpage");

    gpu_native_environment_apply(shaded_textured_triangle, 9, &environment);
    expect_true(environment.tpage == 0x0077u,
                "Gouraud textured triangle latches word five tpage");
}

static void test_native_cull_view_is_independent_of_legacy_wide_mode(void) {
    gpu_ws_configure(4, 3, 0u, 0, 0);
    gpu_ws_configure_native_cull(1, 16, 9, 320, 240);
    expect_true(psx_ws_x_margin() == 53,
                "Native cull view matches the host's 426-wide edge margin");
    expect_true(psx_ws_depth_bound(0x0d80) == 0x1200,
                "Native cull view scales depth gates without enabling mode 2");
    expect_true(psx_ws_plane_nx(4096) == 3072,
                "Native cull view scales side-plane normals");
    expect_true(psx_ws_backdrop_x(0) == -53 &&
                    psx_ws_backdrop_x(160) == 160 &&
                    psx_ws_backdrop_x(320) == 373,
                "Native cull view transforms configured backdrop X stores");
    gpu_ws_configure_native_cull(0, 4, 3, 320, 240);
    expect_true(psx_ws_x_margin() == 0,
                "disabling Native culling restores the canonical margin");
    expect_true(psx_ws_backdrop_x(0) == 0 && psx_ws_backdrop_x(320) == 320,
                "disabling Native culling restores canonical backdrop X stores");
    expect_true(psx_ws_depth_bound(0x0d80) == 0x0d80,
                 "disabling Native culling restores the canonical depth gate");

    gpu_ws_set_temporal_cull_guard_pixels(8);
    expect_true(psx_ws_x_margin() == 8,
                "temporal cull guard expands the canonical view");
    gpu_ws_configure_native_cull(1, 16, 9, 320, 240);
    expect_true(psx_ws_x_margin() == 61,
                "temporal cull guard extends the Native wide margin");
    gpu_ws_set_temporal_cull_guard_pixels(0);
    expect_true(psx_ws_x_margin() == 53,
                "disabling temporal coverage restores the Native margin");
    gpu_ws_configure_native_cull(0, 4, 3, 320, 240);
}

static void test_semantic_guest_cull_policy(void) {
    const uint32_t addresses[] = {
        0x80010000u, 0x80010004u, 0x80010008u,
        0x8001000cu, 0x80010010u, 0x80010014u,
        0x80010018u, 0x8001001cu,
    };
    const uint8_t semantics[] = {
        PSX_WS_CULL_SEMANTIC_SCREEN_BIAS,
        PSX_WS_CULL_SEMANTIC_WORLD_RANGE,
        PSX_WS_CULL_SEMANTIC_LEFT_EDGE,
        PSX_WS_CULL_SEMANTIC_MASKED_SCREEN_X,
        PSX_WS_CULL_SEMANTIC_FRUSTUM_PLANE_X,
        PSX_WS_CULL_SEMANTIC_SIGNED_SCREEN_X,
        PSX_WS_CULL_SEMANTIC_DEPTH_BOUND,
        PSX_WS_CULL_SEMANTIC_XCLIP_BOUND,
    };

    gpu_ws_set_semantic_cull_sites(addresses, semantics, 8);
    gpu_ws_configure_native_cull(1, 16, 9, 320, 240);
    expect_true(psx_ws_semantic_cull_site(0xA0010000u) ==
                    PSX_WS_CULL_SEMANTIC_SCREEN_BIAS,
                "semantic cull lookup normalizes physical aliases");
    expect_true(psx_ws_guest_cull_screen_bias(0u, 0) == 53u,
                "semantic screen-bias policy uses the Native margin");
    expect_true(psx_ws_guest_cull_world_range(425u, 320) == 1 &&
                    psx_ws_guest_cull_world_range(426u, 320) == 0,
                "semantic world-range policy widens both sides");
    expect_true(psx_ws_guest_cull_left_edge(39u) == 0u - 39u - 53u,
                "semantic left-edge policy moves the reject edge");
    expect_true(psx_ws_guest_cull_masked_screen_x(0xffffu, 320u) == 1,
                "semantic masked-screen-X policy preserves wrapped left reveal");
    expect_true(psx_ws_guest_cull_frustum_plane_x(4096) == 3072,
                "semantic frustum-plane policy scales the side normal");
    expect_true(psx_ws_semantic_cull_site(0x80010014u) ==
                    PSX_WS_CULL_SEMANTIC_SIGNED_SCREEN_X &&
                    psx_ws_guest_cull_signed_screen_x(372, 0x140) == 1 &&
                    psx_ws_guest_cull_signed_screen_x(373, 0x140) == 0,
                "semantic signed-screen-X policy widens the right edge");
    expect_true(psx_ws_guest_cull_depth_signed(0x11ff, 0x0d80) == 1 &&
                    psx_ws_guest_cull_depth_unsigned(0x1200u, 0x0d80) == 0,
                "semantic depth-bound policy scales signed and unsigned gates");
    expect_true(psx_ws_guest_cull_xclip_bound(319u) == 0x7fffffffu,
                "semantic X-clip-bound policy disables the old edge reject");

    gpu_ws_configure_native_cull(0, 4, 3, 320, 240);
    expect_true(psx_ws_guest_cull_screen_bias(0u, 0) == 0u &&
                    psx_ws_guest_cull_world_range(320u, 320) == 0 &&
                    psx_ws_guest_cull_frustum_plane_x(4096) == 4096,
                "semantic guest-cull policy is identity at 4:3");
    gpu_ws_set_semantic_cull_sites(NULL, NULL, 0);
}

static void test_native_semantic_vertices_keep_raw_coordinates(void) {
    const GpuNativeDrawEnvironment environment = {
        {
            0u, 0u, 1023u, 511u, 3, -2,
            0u, 0u, 0u, 0u, 0u, 0u, 0u,
        },
        0u,
    };
    const uint32_t words[4] = {
        0x20ffffffu, gp0_xy(10, 20), gp0_xy(30, 40), gp0_xy(50, 60),
    };
    GpuRenderSemantic semantic = {0};
    const uint32_t rectangle[3] = {
        0x60ffffffu, gp0_xy(10, 20), gp0_xy(8, 8),
    };

    expect_true(gpu_native_semantic_from_gp0(words, 4, &environment, &semantic) == 1,
                "flat triangle converts to Native semantic geometry");
    expect_true(semantic.material.draw_offset_x == 3 &&
                    semantic.material.draw_offset_y == -2,
                "semantic material carries the draw offset");
    expect_true(semantic.triangles[0].vertices[0].x == 10 * 65536 &&
                    semantic.triangles[0].vertices[0].y == 20 * 65536,
                "semantic vertices retain raw GP0 coordinates");
    expect_true(!semantic.screen_space_2d,
                "polygon semantics remain unclassified 3D geometry");
    expect_true(gpu_native_semantic_from_gp0(
                    rectangle, 3, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d ==
                        GPU_RENDER_SCREEN_SPACE_2D_STRETCH,
                "rectangle primitives are classified as screen-space 2D");
}

static void test_native_portrait_uses_canonical_position(void) {
    const GpuNativeDrawEnvironment environment = {0};
    uint32_t portrait[9] = {
        0x2c808080u,
        gp0_xy(10, 20), 0x38c00000u,
        gp0_xy(74, 20), 0x009b0010u,
        gp0_xy(10, 116), 0x00000070u,
        gp0_xy(74, 116), 0x00000080u,
    };
    GpuRenderSemantic semantic = {0};

    expect_true(gpu_native_semantic_from_gp0(
                    portrait, 9, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "dialogue portrait stays with the canonical dialogue box");

    portrait[2] = 0x38000000u;
    expect_true(gpu_native_semantic_from_gp0(
                    portrait, 9, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "first portrait CLUT row stays in canonical screen space");

    portrait[2] = 0x3bc00000u;
    expect_true(gpu_native_semantic_from_gp0(
                    portrait, 9, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "last portrait CLUT row stays in canonical screen space");

    portrait[2] = 0x38c10000u;
    expect_true(gpu_native_semantic_from_gp0(
                    portrait, 9, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "non-portrait CLUT X does not classify a textured FT4 as screen-space");

    portrait[2] = 0x3c000000u;
    expect_true(gpu_native_semantic_from_gp0(
                    portrait, 9, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                 "out-of-range CLUT row does not classify a textured FT4 as screen-space");
}

static void test_dialogue_text_is_centered_with_its_box(void) {
    GpuNativeDrawEnvironment environment = {0};
    uint32_t box_fill[] = {
        0x62000000u, gp0_xy(21, 19), gp0_xy(129, 52),
    };
    uint32_t text_sprite[] = {
        0x65808080u, gp0_xy(28, 24), 0x7c000000u, gp0_xy(88, 13),
    };
    const uint32_t border_sprite[] = {
        0x66808080u, gp0_xy(20, 18), 0x3d100000u, gp0_xy(64, 8),
    };
    const uint32_t continue_sprite[] = {
        0x64808080u, gp0_xy(42, 64), 0x3d900000u, gp0_xy(12, 8),
    };
    GpuRenderSemantic semantic = {0};

    expect_true(gpu_native_semantic_from_gp0(
                    box_fill, 3, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "system text fill keeps its canonical width");

    box_fill[0] = 0x62406080u;
    box_fill[2] = gp0_xy(128, 56);
    expect_true(gpu_native_semantic_from_gp0(
                    box_fill, 3, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "field window body keeps its canonical width");

    environment.tpage = 0x001cu;
    expect_true(gpu_native_semantic_from_gp0(
                    text_sprite, 4, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "field dialogue text remains centered while its line sprite grows");

    text_sprite[3] = gp0_xy(92, 13);
    expect_true(gpu_native_semantic_from_gp0(
                    text_sprite, 4, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "revealing another field-dialogue glyph keeps the same mapping");

    environment.tpage = 0x001eu;
    expect_true(gpu_native_semantic_from_gp0(
                    text_sprite, 4, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "combat dialogue text remains centered with its polygon box");

    environment.tpage = 0x001du;
    expect_true(gpu_native_semantic_from_gp0(
                    text_sprite, 4, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "opening text remains centered while its line sprite grows");

    environment.tpage = 0x001fu;
    expect_true(gpu_native_semantic_from_gp0(
                    text_sprite, 4, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_STRETCH,
                "unrelated screen-space sprite keeps widescreen stretching");

    box_fill[0] = 0x62000000u;
    box_fill[2] = gp0_xy(128, 52);
    expect_true(gpu_native_semantic_from_gp0(
                    box_fill, 3, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_STRETCH,
                "unrelated translucent rectangle keeps widescreen stretching");

    environment.tpage = 0x005au;
    expect_true(gpu_native_semantic_from_gp0(
                    border_sprite, 4, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "field dialogue border sprite keeps its canonical position");

    environment.tpage = 0x001au;
    expect_true(gpu_native_semantic_from_gp0(
                    continue_sprite, 4, &environment, &semantic) == 1 &&
                    semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE,
                "dialogue continue indicator stays attached to the box");
}

static void test_untextured_native_semantic_latches_ordered_blend(void) {
    const uint32_t draw_mode[] = { 0xe1000040u };
    const uint32_t words[] = {
        0x22000000u, gp0_xy(10, 20), gp0_xy(30, 20), gp0_xy(10, 40),
    };
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_MMIO, 0x1000u, 0u, 0u,
    };
    const GpuNativeDrawEnvironment stale_environment = {
        {
            0u, 0u, 1023u, 511u, 0, 0,
            0u, 0u, 0u, 0u, 0u, 0u, 0u,
        },
        0u,
    };
    GpuRenderSemantic semantic = {0};
    uint16_t pixel = 0;

    expect_true(gpu_native_semantic_from_gp0(
                    words, 4, &stale_environment, &semantic) == 1,
                "untextured Native semantic starts from producer material");
    gr_vram_write(12, 22, WHITE_1555);
    gl_renderer_flush_cpu_uploads();
    expect_true(gpu_native_submit_gp0_packet(
                    draw_mode, 1, NULL, &source) == 1,
                "ordered GP0 draw mode applies before Native semantic");
    expect_true(gpu_native_submit_gp0_packet(
                    words, 4, &semantic, &source) == 1,
                "bound untextured Native semantic rasterizes");
    gl_renderer_flush_cpu_uploads();
    expect_true(read_fbo_pixel(12, 22, &pixel),
                "Native semantic blend result reads the OpenGL FBO");
    expect_pixel(pixel, WHITE_1555,
                 "ordered subtract blend preserves white under a black triangle");
}

static void test_unbound_gp0_packet_rasterizes_natively(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(340, 300), gp0_xy(348, 300), gp0_xy(340, 308),
    };
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST, 0x1804u, 0x601u, 0x600u,
    };
    uint16_t pixel = 0;

    reset_gpu_for_case();
    expect_true(gpu_native_submit_gp0_packet(
                    words, sizeof(words) / sizeof(words[0]), NULL, &source) == 1,
                "unbound GP0 packet translates on the OpenGL Native path");
    gl_renderer_flush_cpu_uploads();
    expect_true(read_fbo_pixel(341, 301, &pixel),
                "packet-derived Native triangle reads the OpenGL FBO");
    expect_pixel(pixel, WHITE_1555,
                 "packet-derived Native triangle rasterizes in OpenGL");
}

static void test_canonical_native_midpoint_policy(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 10), gp0_xy(18, 10), gp0_xy(10, 18),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after = {0};

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "canonical Native midpoint runs with the wide view disabled");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "canonical midpoint fixture builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 1u, 1u};
    gl_renderer_native_midpoint_diag(&before);
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK,
                "first canonical Native source frame rasterizes");
    expect_true(gl_renderer_present_native_midpoint(0, 0, 320, 240, 0, 1),
                "first canonical Native source frame presents directly");
    expect_true(gl_renderer_present_native_midpoint(0, 0, 320, 240, 0, 1),
                "empty duplicate VBlank presents current without spanning workload");
    gr_native_fill_rect(100, 100, 4, 4, 0);
    gr_native_copy_rect(100, 100, 104, 100, 4, 4);
    expect_true(gl_renderer_present_native_midpoint(0, 0, 320, 240, 0, 1),
                "discrete GP0-only duplicate VBlank preserves semantic history");
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
            semantic.triangles[triangle].vertices[vertex].x +=
                INT32_C(65536);
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK,
                "second canonical Native source frame rasterizes");
    expect_true(gl_renderer_present_native_midpoint(0, 0, 320, 240, 0, 1),
                "changed canonical Native source frame presents on its cadence slot");
    gl_renderer_native_midpoint_diag(&after);
    expect_true(after.previous_usable,
                "canonical path retains a usable previous frame");
    expect_true(after.midpoint_presents == before.midpoint_presents + 1u,
                "canonical path presents one changed-frame midpoint");
    expect_true(after.midpoint_candidates == before.midpoint_candidates + 1u &&
                    after.midpoint_duplicate_empty_frames ==
                        before.midpoint_duplicate_empty_frames + 2u,
                "canonical path records its duplicate-to-candidate cadence");
    expect_true(after.workload_total_eligible_frames ==
                        before.workload_total_eligible_frames + 1u &&
                    after.eligibility_complete_frames ==
                        before.eligibility_complete_frames + 1u &&
                    after.eligibility_no_previous_frames ==
                        before.eligibility_no_previous_frames + 1u &&
                    after.workload_total_rejected_no_previous_frames ==
                        before.workload_total_rejected_no_previous_frames + 1u,
                "canonical path reports complete-frame eligibility reasons");
    expect_true(after.current_presents == before.current_presents + 3u,
                "canonical path presents three current frames");
    expect_true(after.frame_open,
                "canonical path opens the next source frame");

    /* MDEC/depth24 callers discard semantic history. Their phase slots repeat
     * current pixels instead of leaking a prior interpolated surface. */
    gl_renderer_native_midpoint_set_suspended(1);
    semantic.triangles[0].vertices[0].x += 20 * INT32_C(65536);
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK,
                "suspended Native frame still rasterizes authoritative current output");
    expect_true(gl_renderer_present_native_midpoint(0, 0, 320, 240, 0, 1),
                "suspended Native frame presents current directly");
    gl_renderer_native_midpoint_diag(&after);
    expect_true(after.suspended && !after.previous_usable &&
                    after.midpoint_presents == before.midpoint_presents + 1u &&
                    after.current_presents == before.current_presents + 4u,
                "suspension keeps authored frames out of midpoint history");
    gl_renderer_native_midpoint_set_suspended(0);
    /* gpu_init resets GP0 state but intentionally preserves the live FBO; leave
     * the shared test renderer blank for the Native-view seed fixture below. */
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
}

static void test_canonical_native_partial_midpoint(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 30), gp0_xy(18, 30), gp0_xy(10, 38),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic previous_moving = {0};
    GpuRenderSemantic previous_retired;
    GpuRenderSemantic current_moving;
    GpuRenderSemantic current_born;
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after = {0};

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "partial midpoint runs with the wide view disabled");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &previous_moving) == 1,
                "partial midpoint fixture builds semantic geometry");
    previous_moving.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 9u, 1u, 1u};
    previous_retired = previous_moving;
    previous_retired.interpolation_identity.primitive_id = 2u;
    for (uint8_t triangle = 0u;
         triangle < previous_retired.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
            previous_retired.triangles[triangle].vertices[vertex].x +=
                50 * INT32_C(65536);
    gl_renderer_native_midpoint_diag(&before);
    expect_true(gr_draw_semantic_immediate(&previous_moving) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gr_draw_semantic_immediate(&previous_retired) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1) &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1),
                "partial midpoint fixture establishes prior and duplicate frames");

    gr_native_fill_rect(0, 0, 320, 240, 0);
    current_moving = previous_moving;
    for (uint8_t triangle = 0u;
         triangle < current_moving.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
            current_moving.triangles[triangle].vertices[vertex].x +=
                20 * INT32_C(65536);
    current_born = previous_moving;
    current_born.interpolation_identity.primitive_id = 3u;
    for (uint8_t triangle = 0u;
         triangle < current_born.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
            current_born.triangles[triangle].vertices[vertex].x +=
                100 * INT32_C(65536);
    expect_true(gr_draw_semantic_immediate(&current_moving) ==
                     GPU_RENDER_TRANSACTION_OK &&
                    gr_draw_semantic_immediate(&current_born) ==
                     GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1),
                "safe moving match presents beside a snapped born primitive");
    gl_renderer_native_midpoint_diag(&after);
    expect_true(after.midpoint_presents == before.midpoint_presents + 1u &&
                    after.midpoint_candidates ==
                        before.midpoint_candidates + 1u &&
                    after.workload_total_partial_incomplete_match_frames ==
                          before.workload_total_partial_incomplete_match_frames +
                              1u &&
                    after.eligibility_partial_incomplete_match_frames ==
                        before.eligibility_partial_incomplete_match_frames +
                            1u &&
                    after.workload_last_eligibility ==
                        GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_INCOMPLETE_MATCH &&
                    after.workload_last_matched == 1u &&
                    after.workload_last_snapped == 1u &&
                    after.workload_last_moved == 1u &&
                    after.retired_candidate_count ==
                        before.retired_candidate_count &&
                    after.current_pending_present,
                "partial transition interpolates its strict match without retired geometry");
    expect_true(after.presented_midpoint_matched_vertices ==
                        before.presented_midpoint_matched_vertices + 3u &&
                    after.presented_midpoint_position_changed_vertices ==
                        before.presented_midpoint_position_changed_vertices + 3u &&
                    after.presented_midpoint_formula_failures ==
                        before.presented_midpoint_formula_failures,
                "partial midpoint reports only its strict matched motion");
    expect_true(gl_renderer_present_native_midpoint(
                    0, 0, 320, 240, 0, 1),
                "partial midpoint drains its authoritative current frame");
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
}

static void test_canonical_native_partial_birth_midpoint(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 50), gp0_xy(18, 50), gp0_xy(10, 58),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic previous = {0};
    GpuRenderSemantic current;
    GpuRenderSemantic born;
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after = {0};

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "partial-birth midpoint uses the canonical view");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &previous) == 1,
                "partial-birth fixture builds semantic geometry");
    previous.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 9u, 41u, 1u};
    expect_true(gr_draw_semantic_immediate(&previous) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1) &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1),
                "partial-birth fixture establishes A,A history");

    current = previous;
    for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
        current.triangles[0].vertices[vertex].x += 20 * INT32_C(65536);
    born = current;
    born.interpolation_identity.primitive_id = 42u;
    for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
        born.triangles[0].vertices[vertex].x += 80 * INT32_C(65536);
    gl_renderer_native_midpoint_diag(&before);
    expect_true(gr_draw_semantic_immediate(&current) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gr_draw_semantic_immediate(&born) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1),
                "partial-birth transition presents its safe midpoint");
    gl_renderer_native_midpoint_diag(&after);
    expect_true(after.midpoint_presents == before.midpoint_presents + 1u &&
                    after.midpoint_candidates ==
                        before.midpoint_candidates + 1u &&
                    after.workload_last_eligibility ==
                        GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_COUNT_MISMATCH &&
                    after.workload_last_previous_unmatched == 0u &&
                    after.workload_last_matched == 1u &&
                    after.workload_last_snapped == 1u &&
                    after.workload_last_moved == 1u &&
                    after.retired_candidate_count ==
                        before.retired_candidate_count &&
                    after.presented_midpoint_position_changed_vertices ==
                        before.presented_midpoint_position_changed_vertices + 3u &&
                    after.current_pending_present,
                "partial-birth midpoint interpolates matches without retiring geometry");
    expect_true(gl_renderer_present_native_midpoint(
                    0, 0, 320, 240, 0, 1),
                "partial-birth midpoint drains its current endpoint");
}

static void make_retirable_native_triangle(
        GpuRenderSemantic *semantic, uint32_t primitive_id,
        uint32_t vertex_group) {
    semantic->interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 0x8009932cu,
                                         primitive_id, 1u};
    for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
        GpuRenderSemanticVertex *position =
            &semantic->triangles[0].vertices[vertex];

        position->native_view_x =
            position->x + 80 * INT32_C(65536);
        position->native_view_y = position->y;
        position->native_view_position = 1u;
        position->projective_view_x = 0;
        position->projective_view_y = 0;
        position->projective_view_z = 1024;
        position->projective_offset_x = position->x;
        position->projective_offset_y = position->y;
        position->projective_native_offset_x = 80 * INT32_C(65536);
        position->projective_native_offset_y = 0;
        position->projective_distance = 256u;
        position->projective_position = 1u;
        position->interpolation_group_id = vertex_group;
        position->interpolation_vertex_id = vertex;
        position->interpolation_vertex_identity_valid = 1u;
    }
}

static void shift_native_triangle_x(GpuRenderSemantic *semantic, int pixels) {
    const int32_t delta = pixels * INT32_C(65536);

    for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
        GpuRenderSemanticVertex *position =
            &semantic->triangles[0].vertices[vertex];

        position->x += delta;
        position->native_view_x += delta;
        position->projective_offset_x += delta;
    }
}

static void test_retired_history_mismatch_preserves_native_present(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 30), gp0_xy(18, 30), gp0_xy(10, 38),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic stale_moving = {0};
    GpuRenderSemantic stale_retired;
    GpuRenderSemantic previous_moving;
    GpuRenderSemantic previous_retired;
    GpuRenderSemantic current_moving;
    GpuRenderInterpolationVertexAnchor anchors[3];
    GlRendererSemanticProducerItemDiagnostics items[8];
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after = {0};
    size_t item_total = 0u;
    uint64_t item_frame = 0u;
    uint32_t current_queue_order = UINT32_MAX;
    uint32_t retired_queue_order = UINT32_MAX;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "retired history fixture configures Native view");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &stale_moving) == 1,
                "retired history fixture builds semantic geometry");
    make_retirable_native_triangle(
        &stale_moving, 100u, UINT32_C(0x63000000));
    stale_retired = stale_moving;
    stale_retired.interpolation_identity.primitive_id = 101u;
    shift_native_triangle_x(&stale_retired, 40);
    expect_true(gr_draw_semantic_immediate(&stale_moving) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gr_draw_semantic_immediate(&stale_retired) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "retired history fixture captures stale host history");

    previous_moving = stale_moving;
    previous_retired = stale_retired;
    previous_moving.interpolation_identity.primitive_id = 1u;
    previous_retired.interpolation_identity.primitive_id = 2u;
    expect_true(gr_draw_semantic_immediate(&previous_moving) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gr_draw_semantic_immediate(&previous_retired) ==
                    GPU_RENDER_TRANSACTION_OK,
                "retired history fixture records the next source frame");
    gr_fill_rect(900, 400, 1, 1, 0);
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "intervening GPU operation presents after flushing host queue");

    current_moving = previous_moving;
    shift_native_triangle_x(&current_moving, 20);
    for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
        GpuRenderSemanticVertex current_vertex =
            previous_retired.triangles[0].vertices[vertex];

        current_vertex.x += 8 * INT32_C(65536);
        current_vertex.native_view_x += 8 * INT32_C(65536);
        current_vertex.projective_offset_x += 8 * INT32_C(65536);
        anchors[vertex] = (GpuRenderInterpolationVertexAnchor){
            .scene_id = previous_retired.interpolation_identity.scene_id,
            .producer_id = previous_retired.interpolation_identity.producer_id,
            .material = previous_retired.material,
            .vertex = current_vertex,
        };
    }
    gl_renderer_native_midpoint_diag(&before);
    expect_true(gr_record_interpolation_anchors(anchors, 3u) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gr_draw_semantic_immediate(&current_moving) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "stale retired history preserves the current present");
    gl_renderer_native_midpoint_diag(&after);
    const size_t item_count = gl_renderer_semantic_producer_items(
        UINT32_C(0x8009932c), UINT64_MAX, 0u, items,
        sizeof(items) / sizeof(items[0]), &item_total, &item_frame);
    for (size_t index = 0u; index < item_count; ++index) {
        if (items[index].primitive_id == 1u)
            current_queue_order = items[index].queue_order;
        if (items[index].primitive_id == 2u)
            retired_queue_order = items[index].queue_order;
    }
    expect_true(after.cancelled_frames == before.cancelled_frames,
                 "optional retired geometry never cancels authoritative current frame");
    expect_true(item_total >= 2u && item_frame != UINT64_MAX &&
                    current_queue_order < retired_queue_order,
                "retired geometry preserves its prior painter-order boundary");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "retired history fixture disables Native view");
}

static void test_canonical_native_midpoint_cancellation(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 10), gp0_xy(18, 10), gp0_xy(10, 18),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after = {0};

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "cancellation fixture keeps the wide view disabled");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "cancellation fixture builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 2u, 1u};
    gl_renderer_native_midpoint_diag(&before);
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(0, 0, 320, 240, 0, 1),
                "cancellation fixture seals its first source frame");
    gl_renderer_native_midpoint_cancel();
    expect_true(gl_renderer_present_native_midpoint(0, 0, 320, 240, 0, 1),
                "cancelled source frame presents authoritative current output");
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
            semantic.triangles[triangle].vertices[vertex].x +=
                20 * INT32_C(65536);
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(0, 0, 320, 240, 0, 1),
                "post-cancel source frame presents directly");
    gl_renderer_native_midpoint_diag(&after);
    expect_true(after.cancelled_frames == before.cancelled_frames + 1u &&
                    after.cancel_reason_counts[
                        GL_NATIVE_MIDPOINT_CANCEL_GENERIC] ==
                        before.cancel_reason_counts[
                            GL_NATIVE_MIDPOINT_CANCEL_GENERIC] + 1u &&
                    after.last_cancel_reason ==
                        GL_NATIVE_MIDPOINT_CANCEL_GENERIC &&
                    after.midpoint_presents == before.midpoint_presents &&
                    after.current_presents == before.current_presents + 3u,
                "cancelled source cannot become the next midpoint match");
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
}

static void test_canonical_native_midpoint_duplicate_identity(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 10), gp0_xy(18, 10), gp0_xy(10, 18),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GpuRenderSemantic duplicate;
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after = {0};

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "duplicate identity fixture keeps the wide view disabled");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "duplicate identity fixture builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 3u, 1u};
    semantic.submission_command_id = UINT64_C(0x100);
    gl_renderer_native_midpoint_diag(&before);
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1) &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1),
                "duplicate identity fixture establishes a prior source frame");

    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
            semantic.triangles[triangle].vertices[vertex].x +=
                INT32_C(65536);
    duplicate = semantic;
    duplicate.submission_command_id = UINT64_C(0x200);
    for (uint8_t triangle = 0u; triangle < duplicate.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
            duplicate.triangles[triangle].vertices[vertex].x +=
                20 * INT32_C(65536);
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gr_draw_semantic_immediate(&duplicate) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1),
                "late duplicate identity keeps authoritative current rendering");

    expect_true(gl_renderer_present_native_midpoint(
                    0, 0, 320, 240, 0, 1),
                "post-conflict duplicate VBlank cannot restore stale history");
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
            semantic.triangles[triangle].vertices[vertex].x +=
                INT32_C(65536);
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1),
                "first post-conflict source frame presents current directly");
    gl_renderer_native_midpoint_diag(&after);
    expect_true(after.cancelled_frames == before.cancelled_frames + 1u &&
                    after.cancel_reason_counts[
                        GL_NATIVE_MIDPOINT_CANCEL_WORKLOAD_RECORD] ==
                        before.cancel_reason_counts[
                            GL_NATIVE_MIDPOINT_CANCEL_WORKLOAD_RECORD] + 1u &&
                    after.last_cancel_reason ==
                        GL_NATIVE_MIDPOINT_CANCEL_WORKLOAD_RECORD &&
                    after.last_cancel_status ==
                        GPU_SEMANTIC_WORKLOAD_CONFLICT &&
                    after.last_cancel_workload_current == 2u &&
                    after.last_cancel_identity_valid &&
                    after.last_cancel_identity_scene == 1u &&
                    after.last_cancel_identity_producer == 1u &&
                    after.last_cancel_identity_primitive == 3u &&
                    after.last_cancel_existing_command_id == UINT64_C(0x100) &&
                    after.last_cancel_current_command_id == UINT64_C(0x200) &&
                    after.midpoint_presents == before.midpoint_presents &&
                    after.current_presents == before.current_presents + 5u &&
                    after.previous_usable,
                "duplicate identity cancels the complete midpoint frame and rebuilds history from current");
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
}

static void test_temporal_candidate_history_only_participation(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 10), gp0_xy(18, 10), gp0_xy(10, 18),
    };
    const GpuRenderTemporalCullPolicy policy = {
        .flags = GPU_RENDER_TEMPORAL_FORCE_PHASES |
            GPU_RENDER_TEMPORAL_CULL_SCREEN,
        .screen_left = 1000 * INT32_C(65536),
        .screen_top = INT32_MIN,
        .screen_right_exclusive = 1001 * INT32_C(65536),
        .screen_bottom_exclusive = INT32_MAX,
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic previous = {0};
    GpuRenderSemantic hidden;
    GpuSemanticWorkloadDiagnostics workload = {0};
    GpuSemanticWorkloadMatchInfo match = {0};
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after = {0};

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "history-only candidate configures Native view");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &previous) == 1,
                "history-only candidate fixture builds semantic geometry");
    make_retirable_native_triangle(&previous, 200u, UINT32_C(0x64000000));
    expect_true(gr_draw_semantic_immediate(&previous) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "history-only candidate fixture seals its previous endpoint");

    hidden = previous;
    shift_native_triangle_x(&hidden, 20);
    gl_renderer_native_midpoint_diag(&before);
    expect_true(gr_draw_semantic_temporal_candidate(&hidden, &policy) ==
                    GPU_RENDER_TRANSACTION_OK,
                "fully culled temporal candidate advances semantic history");
    gpu_semantic_workload_diagnostics(&workload);
    expect_true(workload.current_count == 1u &&
                    workload.current_participating_count == 0u &&
                    workload.matched_count == 0u && workload.moved_count == 0u,
                "fully culled temporal endpoint does not alter visual eligibility");
    expect_true(gpu_semantic_workload_match_info(
                    &hidden.interpolation_identity, &match) ==
                    GPU_SEMANTIC_WORKLOAD_OK &&
                    match.participation ==
                        GPU_SEMANTIC_WORKLOAD_PARTICIPATION_HISTORY_ONLY,
                "fully culled temporal endpoint remains keyed history");
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "history-only temporal source does not create a visual frame");
    gl_renderer_native_midpoint_diag(&after);
    expect_true(after.temporal_candidate_count ==
                        before.temporal_candidate_count + 1u &&
                    after.temporal_candidate_recorded_count ==
                        before.temporal_candidate_recorded_count + 1u &&
                    after.temporal_candidate_visible_count ==
                        before.temporal_candidate_visible_count &&
                    after.temporal_candidate_record_failure_count ==
                        before.temporal_candidate_record_failure_count &&
                    after.workload_last_eligibility ==
                        before.workload_last_eligibility &&
                    after.sealed_frames == before.sealed_frames &&
                    after.midpoint_duplicate_empty_frames ==
                        before.midpoint_duplicate_empty_frames + 1u,
                "history-only candidate is recorded without sealing or phase presentation");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "history-only candidate fixture disables Native view");
}

static void test_temporal_candidate_duplicate_identity_fails_closed(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 10), gp0_xy(18, 10), gp0_xy(10, 18),
    };
    const GpuRenderTemporalCullPolicy policy = {
        .flags = GPU_RENDER_TEMPORAL_FORCE_PHASES,
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GpuRenderSemantic duplicate;
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after = {0};

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "temporal duplicate fixture configures Native view");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "temporal duplicate fixture builds semantic geometry");
    make_retirable_native_triangle(&semantic, 201u, UINT32_C(0x64000010));
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "temporal duplicate fixture seals its previous endpoint");

    shift_native_triangle_x(&semantic, 1);
    duplicate = semantic;
    shift_native_triangle_x(&duplicate, 20);
    gl_renderer_native_midpoint_diag(&before);
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gr_draw_semantic_temporal_candidate(&duplicate, &policy) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "authoritative and temporal duplicate preserve current rendering");
    gl_renderer_native_midpoint_diag(&after);
    expect_true(after.temporal_candidate_count ==
                        before.temporal_candidate_count + 1u &&
                    after.temporal_candidate_record_failure_count ==
                        before.temporal_candidate_record_failure_count + 1u &&
                    after.temporal_candidate_duplicate_count ==
                        before.temporal_candidate_duplicate_count + 1u &&
                    after.temporal_candidate_identity_collision_count ==
                        before.temporal_candidate_identity_collision_count + 1u &&
                    after.cancelled_frames == before.cancelled_frames + 1u,
                "authoritative and temporal duplicate remains fail-closed");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "temporal duplicate fixture disables Native view");
}

static void test_native_midpoint_reset_flushes_pending_view_draw(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 80), gp0_xy(18, 80), gp0_xy(10, 88),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GlRendererNativeMidpointDiagnostics diagnostics = {0};
    uint16_t pixel = 0;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "pending-reset Native view configures");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "pending-reset fixture builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 4u, 1u};
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK,
                "pending-reset fixture queues its Native draw");

    gl_renderer_native_midpoint_reset();

    expect_true(gl_renderer_native_view_peek(0, 64, 81, 1, 1, &pixel),
                "reset-flushed Native draw is readable");
    expect_pixel(pixel, WHITE_1555,
                 "midpoint reset materializes queued Native work before clearing state");
    gl_renderer_native_midpoint_diag(&diagnostics);
    expect_true(!diagnostics.frame_open && !diagnostics.frame_valid &&
                    !diagnostics.current_pending_present,
                "midpoint reset clears lifecycle state after materialization");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "pending-reset Native view disables independently");
}

static void test_native_full_width_copy_flushes_pending_view_draw(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 100), gp0_xy(18, 100), gp0_xy(10, 108),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after = {0};
    uint16_t source = 0;
    uint16_t destination = 0;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "full-width-copy Native view configures");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "full-width-copy fixture builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 5u, 1u};
    gl_renderer_native_midpoint_diag(&before);
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK,
                "full-width-copy fixture queues its Native source draw");

    gr_native_copy_rect(0, 96, 0, 116, 320, 16);

    expect_true(gl_renderer_native_view_peek(0, 64, 101, 1, 1, &source) &&
                    gl_renderer_native_view_peek(
                        0, 64, 121, 1, 1, &destination),
                "full-width Native copy source and destination are readable");
    expect_pixel(source, WHITE_1555,
                 "full-width Native copy retains the queued source draw");
    expect_pixel(destination, WHITE_1555,
                 "full-width Native copy observes queued draws in submission order");
    gl_renderer_native_midpoint_diag(&after);
    expect_true(after.host_queue_flush_reasons[5] ==
                    before.host_queue_flush_reasons[5] + 1u,
                "full-width Native copy records its pending-queue barrier");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "full-width-copy Native view disables independently");
}

static void test_native_midpoint_sequence_a_a_b_c(void) {
    /* If C arrives while B's current endpoint is pending, B must present first.
     * C then occupies the next phase/current pair as a fail-closed snap rather
     * than creating a second current debt and breaking the 30->60 cadence. */
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 140), gp0_xy(18, 140), gp0_xy(10, 148),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after_b = {0};
    GlRendererNativeMidpointDiagnostics after_c = {0};
    GlPresEvent present_event = {0};
    uint64_t presents_before;
    uint16_t b_pixel = 0;
    uint16_t c_pixel = 0;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "A,A,B,C Native view configures");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "A,A,B,C fixture builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 6u, 1u};
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &semantic.triangles[triangle].vertices[vertex];
            position->native_view_x = position->x + 100 * INT32_C(65536);
            position->native_view_y = position->y;
            position->native_view_position = 1u;
        }
    gl_renderer_native_midpoint_diag(&before);
    presents_before = gl_renderer_pres_total();

    gr_native_fill_rect(0, 0, 320, 240, 0);
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0) &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "A,A,B,C fixture presents A and its duplicate");

    gr_native_fill_rect(0, 0, 320, 240, 0);
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            semantic.triangles[triangle].vertices[vertex].x +=
                20 * INT32_C(65536);
            semantic.triangles[triangle].vertices[vertex].native_view_x +=
                20 * INT32_C(65536);
        }
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "A,A,B,C fixture presents the A-to-B midpoint");
    gl_renderer_native_midpoint_diag(&after_b);
    expect_true(after_b.midpoint_presents == before.midpoint_presents + 1u &&
                    after_b.current_presents == before.current_presents + 2u &&
                    after_b.current_pending_present,
                "B is saved as current after the A-to-B midpoint");
    expect_true(
        after_b.presented_midpoint_position_changed_vertices ==
                before.presented_midpoint_position_changed_vertices + 3u &&
            after_b.presented_midpoint_distinct_vertices ==
                before.presented_midpoint_distinct_vertices + 3u &&
            after_b.presented_midpoint_collapsed_vertices ==
                before.presented_midpoint_collapsed_vertices &&
            after_b.presented_midpoint_formula_failures ==
                before.presented_midpoint_formula_failures,
        "completed A-to-B midpoint contains three strict midpoint vertices");
    expect_true(gl_renderer_pres_get(presents_before, &present_event) &&
                    present_event.path == GL_PRES_NATIVE_CURRENT &&
                    present_event.swap_completed &&
                    gl_renderer_pres_get(presents_before + 1u, &present_event) &&
                    present_event.path == GL_PRES_NATIVE_CURRENT &&
                    present_event.swap_completed &&
                    gl_renderer_pres_get(presents_before + 2u, &present_event) &&
                    present_event.path == GL_PRES_NATIVE_MIDPOINT &&
                    present_event.swap_completed,
                "present ring distinguishes A,A current swaps from the A-to-B midpoint");

    gr_native_fill_rect(0, 0, 320, 240, 0);
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            semantic.triangles[triangle].vertices[vertex].x +=
                20 * INT32_C(65536);
            semantic.triangles[triangle].vertices[vertex].native_view_x +=
                20 * INT32_C(65536);
        }
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "C arrival presents saved B before materializing C");
    expect_true(gl_renderer_native_view_peek(0, 131, 141, 1, 1, &b_pixel) &&
                    gl_renderer_native_view_peek(
                        0, 151, 141, 1, 1, &c_pixel),
                "post-C Native current surface is readable");
    expect_pixel(b_pixel, 0,
                 "post-C current surface does not retain the cleared B position");
    expect_pixel(c_pixel, WHITE_1555,
                 "C materializes after saved B is selected for presentation");
    gl_renderer_native_midpoint_diag(&after_c);
    expect_true(after_c.midpoint_presents == before.midpoint_presents + 1u &&
                    after_c.current_presents == before.current_presents + 3u &&
                    after_c.current_pending_present,
                "A,A,B,C carries exactly one saved-current presentation debt");

    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "empty VBlank presents saved C after A,A,B,C");
    gl_renderer_native_midpoint_diag(&after_c);
    expect_true(after_c.current_presents == before.current_presents + 4u &&
                    !after_c.current_pending_present && after_c.frame_open,
                "saved C drains once and opens the next source frame");
    expect_true(gl_renderer_pres_get(presents_before + 3u, &present_event) &&
                    present_event.path == GL_PRES_NATIVE_CURRENT &&
                    present_event.swap_completed &&
                    gl_renderer_pres_get(presents_before + 4u, &present_event) &&
                    present_event.path == GL_PRES_NATIVE_CURRENT &&
                    present_event.swap_completed,
                "present ring classifies the saved B and C swaps as current");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "A,A,B,C Native view disables independently");
}

static void test_native_original_present_sequence(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 180), gp0_xy(18, 180), gp0_xy(10, 188),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after = {0};
    GlPresEvent event = {0};
    uint64_t sequence;

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "original Native target configures Native-wide surfaces");
    expect_true(gl_renderer_set_native_interpolation_fps(30) &&
                    gl_renderer_native_interpolation_fps() == 30,
                "original Native target configures 30 FPS");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "original Native target builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 40u, 1u};
    gl_renderer_native_midpoint_diag(&before);
    sequence = gl_renderer_pres_total();
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "original Native target presents current geometry");
    gl_renderer_native_midpoint_diag(&after);
    expect_true(gl_renderer_pres_get(sequence, &event) &&
                    event.path == GL_PRES_NATIVE_CURRENT &&
                    event.phase_numerator == 0u &&
                    event.phase_denominator == 0u &&
                    after.target_fps == 30u && after.phase_count == 0u &&
                    after.midpoint_presents == before.midpoint_presents &&
                    after.current_presents == before.current_presents + 1u &&
                    !after.frame_open && !after.current_pending_present,
                "original Native target bypasses interpolation phases");
    expect_true(gl_renderer_set_native_interpolation_fps(60),
                "original Native target restores 60 FPS");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "original Native target disables Native-wide surfaces");
}

static void test_native_rational_present_sequence(int target_fps,
                                                  unsigned int denominator) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 180), gp0_xy(18, 180), gp0_xy(10, 188),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GlPresEvent event = {0};
    const unsigned int subframes = denominator / 2u;
    uint64_t phase_sequences[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES] = {0};
    uint64_t current_sequence;
    uint64_t sequence;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_set_native_interpolation_fps(target_fps),
                "rational Native interpolation target configures");
    expect_true(gl_renderer_native_interpolation_fps() == target_fps,
                "rational Native interpolation target reports exactly");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "rational Native interpolation uses canonical view");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "rational Native interpolation builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 7u, 1u};

    current_sequence = gl_renderer_pres_total();
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1),
                "rational Native interpolation presents its first current-only VBlank");
    for (unsigned int subframe = 0u; subframe < subframes; ++subframe) {
        expect_true(gl_renderer_pres_get(current_sequence++, &event) &&
                        event.path == GL_PRES_NATIVE_CURRENT &&
                        event.phase_numerator == 0u &&
                        event.phase_denominator == 0u &&
                        event.swap_completed,
                    "current-only VBlank repeats current at the rational target cadence");
    }
    expect_true(current_sequence == gl_renderer_pres_total(),
                "first current-only VBlank emits exactly one target half-frame");

    current_sequence = gl_renderer_pres_total();
    expect_true(gl_renderer_present_native_midpoint(
                    0, 0, 320, 240, 0, 1),
                "rational Native interpolation presents its duplicate current-only VBlank");
    for (unsigned int subframe = 0u; subframe < subframes; ++subframe) {
        expect_true(gl_renderer_pres_get(current_sequence++, &event) &&
                        event.path == GL_PRES_NATIVE_CURRENT &&
                        event.phase_numerator == 0u &&
                        event.phase_denominator == 0u &&
                        event.swap_completed,
                    "duplicate current-only VBlank repeats current at the target cadence");
    }
    expect_true(current_sequence == gl_renderer_pres_total(),
                "duplicate current-only VBlank emits exactly one target half-frame");
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
            semantic.triangles[triangle].vertices[vertex].x +=
                80 * INT32_C(65536);

    sequence = gl_renderer_pres_total();
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1),
                "rational Native interpolation presents first phase half");
    for (unsigned int numerator = 1u;
         numerator <= denominator / 2u; ++numerator) {
        phase_sequences[numerator - 1u] = sequence;
        expect_true(gl_renderer_pres_get(sequence++, &event) &&
                        event.path == GL_PRES_NATIVE_MIDPOINT &&
                        event.phase_numerator == numerator &&
                        event.phase_denominator == denominator &&
                        event.swap_completed,
                    "first VBlank presents each rational phase in order");
    }
    expect_true(gl_renderer_present_native_midpoint(
                    0, 0, 320, 240, 0, 1),
                "rational Native interpolation presents second phase half");
    for (unsigned int numerator = denominator / 2u + 1u;
         numerator < denominator; ++numerator) {
        phase_sequences[numerator - 1u] = sequence;
        expect_true(gl_renderer_pres_get(sequence++, &event) &&
                        event.path == GL_PRES_NATIVE_MIDPOINT &&
                        event.phase_numerator == numerator &&
                        event.phase_denominator == denominator &&
                        event.swap_completed,
                    "second VBlank presents each rational phase in order");
    }
    expect_true(gl_renderer_pres_get(sequence++, &event) &&
                    event.path == GL_PRES_NATIVE_CURRENT &&
                    event.phase_numerator == 0u &&
                    event.phase_denominator == 0u &&
                    event.swap_completed,
                "second VBlank ends on current without stale phase metadata");
    expect_true(sequence == gl_renderer_pres_total(),
                "rational Native cadence emits no hidden extra swaps");
    {
        uint64_t previous_hash = 0u;
        uint64_t previous_source_hash = 0u;
        uint64_t previous_geometry_hash = 0u;
        uint64_t previous_phase_surface_hash = 0u;
        uint64_t previous_phase_vram_hash = 0u;
        for (unsigned int numerator = 1u;
             numerator < denominator; ++numerator) {
            expect_true(gl_renderer_pres_get(
                            phase_sequences[numerator - 1u], &event) &&
                            event.framebuffer_hash_valid &&
                            event.source_hash_valid &&
                            event.geometry_hash_valid &&
                            event.phase_surface_hash_valid &&
                            event.phase_vram_hash_valid,
                        "canonical Native phase has geometry, phase-VRAM, visible-source, and final hashes");
            if (event.framebuffer_hash_valid) {
                expect_true(numerator == 1u ||
                                event.framebuffer_hash != previous_hash,
                            "successive rational Native phase framebuffers are distinct");
                previous_hash = event.framebuffer_hash;
            }
            if (event.source_hash_valid) {
                expect_true(numerator == 1u ||
                                event.source_hash != previous_source_hash,
                            "successive rational Native phase sources are distinct");
                previous_source_hash = event.source_hash;
            }
            if (event.geometry_hash_valid) {
                expect_true(numerator == 1u ||
                                event.geometry_hash != previous_geometry_hash,
                            "successive canonical Native phase geometry is distinct");
                previous_geometry_hash = event.geometry_hash;
            }
            if (event.phase_surface_hash_valid) {
                expect_true(numerator == 1u || event.phase_surface_hash !=
                                previous_phase_surface_hash,
                            "successive canonical Native phase surfaces are distinct");
                previous_phase_surface_hash = event.phase_surface_hash;
            }
            if (event.phase_vram_hash_valid) {
                expect_true(numerator == 1u ||
                                event.phase_vram_hash != previous_phase_vram_hash,
                            "successive canonical Native phase VRAM images are distinct");
                previous_phase_vram_hash = event.phase_vram_hash;
            }
        }
    }
    expect_true(gl_renderer_set_native_interpolation_fps(60),
                "rational Native interpolation restores the 60 FPS target");
}

static void test_native_wide_rational_present_sequence(
        int target_fps, unsigned int denominator) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 180), gp0_xy(18, 180), gp0_xy(10, 188),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GlPresEvent event = {0};
    uint64_t phase_sequences[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES] = {0};
    uint64_t current_sequence;
    uint64_t sequence;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_set_native_interpolation_fps(target_fps),
                "Native-wide rational interpolation target configures");
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "Native-wide rational interpolation view configures");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "Native-wide rational interpolation builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 8u, 1u};
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &semantic.triangles[triangle].vertices[vertex];
            position->native_view_x = position->x + 100 * INT32_C(65536);
            position->native_view_y = position->y;
            position->native_view_position = 1u;
        }

    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0) &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "Native-wide rational interpolation establishes A,A history");
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            semantic.triangles[triangle].vertices[vertex].x +=
                80 * INT32_C(65536);
            semantic.triangles[triangle].vertices[vertex].native_view_x +=
                80 * INT32_C(65536);
        }

    sequence = gl_renderer_pres_total();
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "Native-wide rational interpolation presents its first phase half");
    for (unsigned int numerator = 1u;
         numerator <= denominator / 2u; ++numerator)
        phase_sequences[numerator - 1u] = sequence++;
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "Native-wide rational interpolation presents its second phase half");
    for (unsigned int numerator = denominator / 2u + 1u;
         numerator < denominator; ++numerator)
        phase_sequences[numerator - 1u] = sequence++;
    current_sequence = sequence++;
    expect_true(sequence == gl_renderer_pres_total(),
                "Native-wide rational cadence emits no hidden extra swaps");
    {
        uint64_t previous_hash = 0u;
        uint64_t previous_source_hash = 0u;
        uint64_t previous_geometry_hash = 0u;
        uint64_t previous_phase_surface_hash = 0u;
        uint64_t previous_phase_vram_hash = 0u;
        for (unsigned int numerator = 1u;
             numerator < denominator; ++numerator) {
            expect_true(gl_renderer_pres_get(
                            phase_sequences[numerator - 1u], &event) &&
                            event.path == GL_PRES_NATIVE_MIDPOINT &&
                            event.phase_numerator == numerator &&
                            event.phase_denominator == denominator &&
                            event.framebuffer_hash_valid &&
                            event.source_hash_valid &&
                            event.geometry_hash_valid &&
                            event.phase_surface_hash_valid &&
                            event.phase_vram_hash_valid,
                        "Native-wide phase has ordered geometry, phase-VRAM, visible-source, and final hashes");
            if (event.framebuffer_hash_valid) {
                expect_true(numerator == 1u ||
                                event.framebuffer_hash != previous_hash,
                            "successive Native-wide phase framebuffers are distinct");
                previous_hash = event.framebuffer_hash;
            }
            if (event.source_hash_valid) {
                expect_true(numerator == 1u ||
                                event.source_hash != previous_source_hash,
                            "successive Native-wide phase sources are distinct");
                previous_source_hash = event.source_hash;
            }
            if (event.geometry_hash_valid) {
                expect_true(numerator == 1u ||
                                event.geometry_hash != previous_geometry_hash,
                            "successive Native-wide phase geometry is distinct");
                previous_geometry_hash = event.geometry_hash;
            }
            if (event.phase_surface_hash_valid) {
                expect_true(numerator == 1u || event.phase_surface_hash !=
                                previous_phase_surface_hash,
                            "successive Native-wide phase surfaces are distinct");
                previous_phase_surface_hash = event.phase_surface_hash;
            }
            if (event.phase_vram_hash_valid) {
                expect_true(numerator == 1u ||
                                event.phase_vram_hash != previous_phase_vram_hash,
                            "successive Native-wide phase VRAM images are distinct");
                previous_phase_vram_hash = event.phase_vram_hash;
            }
        }
        expect_true(gl_renderer_pres_get(current_sequence, &event) &&
                        event.path == GL_PRES_NATIVE_CURRENT &&
                        event.geometry_hash_valid &&
                        event.geometry_hash != previous_geometry_hash,
                    "Native-wide current preserves distinct pending-frame geometry");
    }
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "Native-wide rational interpolation view disables");
    expect_true(gl_renderer_set_native_interpolation_fps(60),
                "Native-wide rational interpolation restores 60 FPS");
}

static void expect_vertical_phase_band(uint64_t sequence, const char *label) {
    static const unsigned int numerator[4] = {1u, 2u, 3u, 0u};
    static const unsigned int denominator[4] = {4u, 4u, 4u, 0u};
    static const int scanout_y[4] = {0, 0, 256, 256};

    for (unsigned int index = 0u; index < 4u; ++index) {
        GlPresEvent event = {0};

        expect_true(gl_renderer_pres_get(sequence + index, &event) &&
                        event.dy == 256 && event.h == 240 &&
                        event.scanout_dy == scanout_y[index] &&
                        event.scanout_h == 240 &&
                        event.phase_numerator == numerator[index] &&
                        event.phase_denominator == denominator[index],
                    label);
    }
}

static void test_canonical_native_vertical_double_buffer(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 180), gp0_xy(18, 180), gp0_xy(10, 188),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    uint64_t sequence;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_set_native_interpolation_fps(120),
                "canonical vertical-buffer interpolation target configures");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "canonical vertical-buffer view configures");
    set_draw_band(0);
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "canonical vertical-buffer fixture builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 9u, 1u};
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1) &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1),
                "canonical vertical-buffer fixture establishes A,A history");

    set_draw_band(256);
    semantic_set_draw_band(&semantic, 256);
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex)
            semantic.triangles[triangle].vertices[vertex].x +=
                80 * INT32_C(65536);
    sequence = gl_renderer_pres_total();
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_midpoint(
                        0, 0, 320, 240, 0, 1),
                "canonical vertical-buffer fixture presents B phase head");
    set_display_band(256);
    expect_true(gl_renderer_present_native_midpoint(
                        0, 256, 320, 240, 0, 1),
                "canonical vertical-buffer fixture presents B phase tail");
    expect_vertical_phase_band(
        sequence,
        "canonical B phases sample the rendered band before the GP1 flip");
    expect_true(gl_renderer_set_native_interpolation_fps(60),
                "canonical vertical-buffer interpolation restores 60 FPS");
}

static void test_native_wide_vertical_double_buffer(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 180), gp0_xy(18, 180), gp0_xy(10, 188),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    uint64_t sequence;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_set_native_interpolation_fps(120),
                "Native-wide vertical-buffer interpolation target configures");
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "Native-wide vertical-buffer view configures");
    set_draw_band(0);
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "Native-wide vertical-buffer fixture builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 10u, 1u};
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &semantic.triangles[triangle].vertices[vertex];
            position->native_view_x = position->x + 100 * INT32_C(65536);
            position->native_view_y = position->y;
            position->native_view_position = 1u;
        }
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0) &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "Native-wide vertical-buffer fixture establishes A,A history");

    set_draw_band(256);
    semantic_set_draw_band(&semantic, 256);
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            semantic.triangles[triangle].vertices[vertex].x +=
                80 * INT32_C(65536);
            semantic.triangles[triangle].vertices[vertex].native_view_x +=
                80 * INT32_C(65536);
        }
    sequence = gl_renderer_pres_total();
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "Native-wide vertical-buffer fixture presents B phase head");
    set_display_band(256);
    expect_true(gl_renderer_present_native_view(0, 256, 240, 0),
                "Native-wide vertical-buffer fixture presents B phase tail");
    expect_vertical_phase_band(
        sequence,
        "Native-wide B phases sample the rendered band before the GP1 flip");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "Native-wide vertical-buffer view disables");
    expect_true(gl_renderer_set_native_interpolation_fps(60),
                "Native-wide vertical-buffer interpolation restores 60 FPS");
}

static void test_native_wide_pending_current_precedes_vertical_flip(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 180), gp0_xy(18, 180), gp0_xy(10, 188),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GlPresEvent event = {0};
    GlRendererNativeMidpointDiagnostics before = {0};
    GlRendererNativeMidpointDiagnostics after = {0};
    uint64_t sequence;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "vertical-lag Native view configures");
    set_draw_band(0);
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "vertical-lag fixture builds semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 11u, 1u};
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &semantic.triangles[triangle].vertices[vertex];
            position->native_view_x = position->x + 100 * INT32_C(65536);
            position->native_view_y = position->y;
            position->native_view_position = 1u;
        }
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0) &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "vertical-lag fixture establishes A,A history");

    set_draw_band(256);
    semantic_set_draw_band(&semantic, 256);
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            semantic.triangles[triangle].vertices[vertex].x +=
                80 * INT32_C(65536);
            semantic.triangles[triangle].vertices[vertex].native_view_x +=
                80 * INT32_C(65536);
        }
    gl_renderer_native_midpoint_diag(&before);
    sequence = gl_renderer_pres_total();
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "vertical-lag fixture presents B midpoint before GP1 flip");
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "vertical-lag fixture presents saved B while scanout still names A");
    gl_renderer_native_midpoint_diag(&after);
    expect_true(after.midpoint_presents == before.midpoint_presents + 1u &&
                    after.current_presents == before.current_presents + 1u &&
                    after.reset_count == before.reset_count &&
                    after.pending_vertical_lag_count ==
                        before.pending_vertical_lag_count + 1u &&
                    after.previous_usable && !after.current_pending_present,
                "vertical scanout lag drains saved current without discarding semantic history");
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0) &&
                    gl_renderer_pres_get(sequence + 2u, &event) &&
                    event.path == GL_PRES_NATIVE_CURRENT &&
                    event.dy == 256 && event.scanout_dy == 0,
                "promoted current remains visible while GP1 still names the old band");
    set_display_band(256);
    expect_true(gl_renderer_present_native_view(0, 256, 240, 0) &&
                    gl_renderer_pres_get(sequence + 3u, &event) &&
                    event.path == GL_PRES_NATIVE_CURRENT &&
                    event.dy == 256 && event.scanout_dy == 256,
                "GP1 catch-up retires the promoted-band override without changing content");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "vertical-lag Native view disables");
}

static void test_native_wide_current_and_phase_share_subpixel_raster(void) {
    const uint32_t stationary_words[] = {
        0x28ffffffu,
        gp0_xy(20, 20), gp0_xy(28, 20), gp0_xy(20, 28), gp0_xy(28, 28),
    };
    const uint32_t moving_words[] = {
        0x28ffffffu,
        gp0_xy(180, 20), gp0_xy(188, 20), gp0_xy(180, 28), gp0_xy(188, 28),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic stationary = {0};
    GpuRenderSemantic moving = {0};
    uint16_t phase_pixels[24 * 24] = {0};
    uint16_t current_pixels[24 * 24] = {0};

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "subpixel-raster Native view configures");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    stationary_words,
                    sizeof(stationary_words) / sizeof(stationary_words[0]),
                    &environment, &stationary) == 1 &&
                    gpu_native_semantic_from_gp0(
                        moving_words,
                        sizeof(moving_words) / sizeof(moving_words[0]),
                        &environment, &moving) == 1,
                "subpixel-raster fixture builds both quads");
    stationary.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 12u, 1u};
    moving.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 13u, 1u};
    for (uint8_t triangle = 0u; triangle < stationary.triangle_count;
         ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &stationary.triangles[triangle].vertices[vertex];
            position->native_view_x = position->x +
                80 * INT32_C(65536) + 3 * INT32_C(16384);
            position->native_view_y = position->y + 3 * INT32_C(16384);
            position->native_view_position = 1u;
        }
    for (uint8_t triangle = 0u; triangle < moving.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &moving.triangles[triangle].vertices[vertex];
            position->native_view_x = position->x +
                80 * INT32_C(65536) + 3 * INT32_C(16384);
            position->native_view_y = position->y + 3 * INT32_C(16384);
            position->native_view_position = 1u;
        }
    expect_true(gr_draw_semantic_immediate(&stationary) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gr_draw_semantic_immediate(&moving) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0) &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "subpixel-raster fixture establishes A,A history");
    for (uint8_t triangle = 0u; triangle < moving.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            moving.triangles[triangle].vertices[vertex].x +=
                20 * INT32_C(65536);
            moving.triangles[triangle].vertices[vertex].native_view_x +=
                20 * INT32_C(65536);
        }
    expect_true(gr_draw_semantic_immediate(&stationary) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gr_draw_semantic_immediate(&moving) ==
                    GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_present_native_view(0, 0, 240, 0),
                "subpixel-raster fixture presents its moving midpoint");
    expect_true(gl_renderer_native_view_phase_peek(
                    0, 0u, 92, 12, 24, 24, phase_pixels) &&
                    gl_renderer_native_view_peek(
                        0, 92, 12, 24, 24, current_pixels),
                "subpixel-raster current and midpoint regions are readable");
    expect_true(memcmp(phase_pixels, current_pixels, sizeof(phase_pixels)) == 0,
                "stationary non-projective Native geometry keeps stable current and phase coverage");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "subpixel-raster Native view disables");
}

static void render_native_quad_raster_xy(
        int32_t fraction_x, int32_t fraction_y,
        int suspended, int projective,
        uint16_t current_pixels[24 * 24],
        uint16_t phase_pixels[24 * 24]) {
    const uint32_t words[] = {
        0x28ffffffu,
        gp0_xy(20, 20), gp0_xy(28, 20), gp0_xy(20, 28), gp0_xy(28, 28),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "suspended-raster Native view configures");
    gl_renderer_native_midpoint_set_suspended(suspended);
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]), &environment,
                    &semantic) == 1,
                "suspended-raster fixture builds its quad");
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &semantic.triangles[triangle].vertices[vertex];
            position->native_view_x =
                position->x + 80 * INT32_C(65536) + fraction_x;
            position->native_view_y = position->y + fraction_y;
            position->native_view_position = 1u;
            position->projective_position = projective ? 1u : 0u;
        }
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                     GPU_RENDER_TRANSACTION_OK &&
                    gl_renderer_native_view_peek(
                        0, 92, 0, 24, 24, current_pixels) &&
                    (phase_pixels == NULL ||
                     gl_renderer_native_view_phase_peek(
                          0, 0u, 92, 0, 24, 24, phase_pixels)),
                "suspended-raster Native current region is readable");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "suspended-raster Native view disables");
    gl_renderer_native_midpoint_set_suspended(0);
}

static void render_native_quad_raster(
        int32_t fraction, int suspended, int projective,
        uint16_t current_pixels[24 * 24],
        uint16_t phase_pixels[24 * 24]) {
    render_native_quad_raster_xy(
        fraction, fraction, suspended, projective,
        current_pixels, phase_pixels);
}

static void test_native_wide_suspended_current_uses_integer_raster(void) {
    uint16_t fractional_pixels[24 * 24] = {0};
    uint16_t integer_pixels[24 * 24] = {0};

    render_native_quad_raster(
        3 * INT32_C(16384), 1, 0, fractional_pixels, NULL);
    render_native_quad_raster(0, 1, 0, integer_pixels, NULL);
    expect_true(memcmp(fractional_pixels, integer_pixels,
                       sizeof(fractional_pixels)) == 0,
                "suspended Native current truncates fractional geometry to the authored integer raster");
}

static void test_native_wide_projective_geometry_uses_integer_raster(void) {
    uint16_t fractional_current[24 * 24] = {0};
    uint16_t fractional_phase[24 * 24] = {0};
    uint16_t integer_current[24 * 24] = {0};
    uint16_t integer_phase[24 * 24] = {0};

    render_native_quad_raster(
        3 * INT32_C(16384), 0, 1,
        fractional_current, fractional_phase);
    render_native_quad_raster(
        0, 0, 1, integer_current, integer_phase);
    expect_true(memcmp(fractional_current, integer_current,
                       sizeof(fractional_current)) == 0,
                "projective Native current preserves authored integer coverage");
    expect_true(memcmp(fractional_phase, integer_phase,
                       sizeof(fractional_phase)) == 0,
                "projective Native phase preserves authored integer coverage");
}

static void test_native_wide_phase_uses_integer_raster(void) {
    uint16_t fractional_current[24 * 24] = {0};
    uint16_t fractional_phase[24 * 24] = {0};
    uint16_t integer_current[24 * 24] = {0};
    uint16_t integer_phase[24 * 24] = {0};

    render_native_quad_raster(
        3 * INT32_C(16384), 0, 0,
        fractional_current, fractional_phase);
    render_native_quad_raster(
        0, 0, 0, integer_current, integer_phase);
    expect_true(memcmp(fractional_current, integer_current,
                       sizeof(fractional_current)) == 0,
                "non-projective Native current preserves integer coverage");
    expect_true(memcmp(fractional_phase, integer_phase,
                       sizeof(fractional_phase)) == 0,
                "non-projective Native phase preserves integer coverage");
}

static void test_native_wide_negative_fixed_positions_floor(void) {
    uint16_t fractional_pixels[24 * 24] = {0};
    uint16_t floored_pixels[24 * 24] = {0};

    render_native_quad_raster_xy(
        0, -20 * INT32_C(65536) - INT32_C(16384),
        1, 1, fractional_pixels, NULL);
    render_native_quad_raster_xy(
        0, -21 * INT32_C(65536),
        1, 1, floored_pixels, NULL);
    expect_true(memcmp(fractional_pixels, floored_pixels,
                       sizeof(fractional_pixels)) == 0,
                "negative Native 16.16 positions use PS1 floor rounding at the viewport edge");
}

static void test_native_textured_subpixel_seam_uses_raster_geometry(void) {
    static const int32_t x[4] = {
        90 * INT32_C(65536), 100 * INT32_C(65536),
        90 * INT32_C(65536) + INT32_C(16384),
        100 * INT32_C(65536),
    };
    static const int32_t y[4] = {
        50 * INT32_C(65536), 50 * INT32_C(65536),
        60 * INT32_C(65536), 60 * INT32_C(65536),
    };
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {1u, 3u, 2u}};
    static const int32_t u[2][3] = {
        {INT32_C(65536), 0, INT32_C(65536)},
        {0, 0, 0},
    };
    GpuRenderSemantic semantic = {0};
    uint16_t current_region[12 * 12] = {0};
    uint16_t phase_region[12 * 12] = {0};
    uint32_t covered = 0u;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gr_vram_write(0, 0, WHITE_1555);
    gl_renderer_flush_cpu_uploads();
    gr_set_texture_filter(1);
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "textured-subpixel seam Native view configures");

    semantic.material.tpage = TEXTURE_PAGE_15BPP;
    semantic.material.texture_depth = GPU_RENDER_TEXTURE_15_BIT;
    semantic.material.shading = GPU_RENDER_SHADING_FLAT;
    semantic.material.textured = 1u;
    semantic.material.raw_texture = 1u;
    semantic.material.draw_area_right = 319u;
    semantic.material.draw_area_bottom = 239u;
    semantic.triangle_count = 2u;
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 14u, 1u};
    for (uint8_t triangle = 0u; triangle < 2u; ++triangle) {
        semantic.triangles[triangle].split_index = triangle;
        semantic.triangles[triangle].split_count = 2u;
        for (uint8_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const uint8_t source = split[triangle][vertex_index];
            GpuRenderSemanticVertex *vertex =
                &semantic.triangles[triangle].vertices[vertex_index];

            vertex->x =
                (x[source] / INT32_C(65536) - 80) * INT32_C(65536);
            vertex->y = y[source];
            vertex->u = u[triangle][vertex_index];
            vertex->v = 0;
            vertex->r = vertex->g = vertex->b = 0x80u;
            vertex->native_view_x = x[source];
            vertex->native_view_y = y[source];
            vertex->native_view_position = 1u;
        }
    }

    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK,
                "fractional adjacent textured triangles rasterize");
    expect_true(gl_renderer_native_view_peek(
                    0, 89, 49, 12, 12, current_region) &&
                    gl_renderer_native_view_phase_peek(
                        0, 0u, 89, 49, 12, 12, phase_region),
                "fractional textured phase seam pixels are readable");
    expect_true(memcmp(current_region, phase_region,
                       sizeof(current_region)) == 0,
                "textured current and phase share identical integer edge coverage");
    for (uint32_t index = 0u; index < 12u * 12u; ++index)
        covered += phase_region[index] == WHITE_1555;
    expect_true(covered != 0u,
                "textured integer phase retains covered triangle pixels");

    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "textured-subpixel seam Native view disables");
    gr_set_texture_filter(0);
}

static void test_native_wide_current_present_sequence(int target_fps,
                                                      unsigned int denominator) {
    GlPresEvent event = {0};
    const unsigned int subframes = denominator / 2u;
    uint64_t sequence;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_set_native_interpolation_fps(target_fps),
                "Native-wide current cadence target configures");
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "Native-wide current cadence view configures");

    sequence = gl_renderer_pres_total();
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "Native-wide current-only VBlank presents");
    for (unsigned int subframe = 0u; subframe < subframes; ++subframe) {
        expect_true(gl_renderer_pres_get(sequence++, &event) &&
                        event.path == GL_PRES_NATIVE_CURRENT &&
                        event.phase_numerator == 0u &&
                        event.phase_denominator == 0u &&
                        event.swap_completed,
                    "Native-wide current-only VBlank repeats current at the target cadence");
    }
    expect_true(sequence == gl_renderer_pres_total(),
                "Native-wide current-only VBlank emits exactly one target half-frame");

    gl_renderer_native_midpoint_set_suspended(1);
    sequence = gl_renderer_pres_total();
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "suspended Native-wide current VBlank presents");
    expect_true(gl_renderer_pres_get(sequence++, &event) &&
                    event.path == GL_PRES_NATIVE_CURRENT &&
                    event.phase_numerator == 0u &&
                    event.phase_denominator == 0u &&
                    event.swap_completed &&
                    sequence == gl_renderer_pres_total(),
                "suspension preserves one authored current swap per VBlank");
    gl_renderer_native_midpoint_set_suspended(0);

    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "Native-wide current cadence view disables");
    expect_true(gl_renderer_set_native_interpolation_fps(60),
                "Native-wide current cadence restores the 60 FPS target");
}

static void test_native_view_uses_semantic_wide_positions(void) {
    const uint32_t clear[] = {
        0x02000000u, gp0_xy(100, 0), gp0_xy(32, 32),
    };
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 10), gp0_xy(18, 10), gp0_xy(10, 18),
    };
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST, 0x1904u, 0x641u, 0x640u,
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    GlRendererNativeMidpointDiagnostics midpoint_diag = {0};
    uint64_t midpoint_presents_before;
    uint64_t current_presents_before;
    uint16_t canonical = 0;
    uint16_t native = 0;
    uint16_t duplicated = 0;

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "producer-driven Native 16:9 surface configures");
    gl_renderer_native_midpoint_diag(&midpoint_diag);
    midpoint_presents_before = midpoint_diag.midpoint_presents;
    current_presents_before = midpoint_diag.current_presents;
    expect_true(gpu_native_submit_gp0_packet(
                    clear, sizeof(clear) / sizeof(clear[0]), NULL, &source) == 1,
                "Native restore test clears the canonical reseed source");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, 4, &environment, &semantic) == 1,
                "Native view test builds canonical semantic geometry");
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, 3u, 1u};
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle) {
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &semantic.triangles[triangle].vertices[vertex];
            position->native_view_x = position->x + 100 * INT32_C(65536);
            position->native_view_y = position->y;
            position->native_view_position = 1u;
        }
    }
    expect_true(gpu_native_submit_gp0_packet(
                    words, 4, &semantic, &source) == 1,
                "semantic draw reaches canonical and Native view surfaces");
    gl_renderer_flush_cpu_uploads();
    expect_true(read_fbo_pixel(11, 11, &canonical),
                "canonical semantic result reads the canonical FBO");
    expect_true(gl_renderer_native_view_peek(0, 111, 11, 1, 1, &native),
                "source-derived position reads the Native view FBO");
    expect_true(gl_renderer_native_view_peek(0, 64, 11, 1, 1, &duplicated),
                "centered canonical position reads the Native view FBO");
    expect_pixel(canonical, WHITE_1555,
                 "canonical geometry remains unchanged by Native widescreen");
    expect_pixel(native, WHITE_1555,
                 "Native view uses the source-derived wide position");
    expect_pixel(duplicated, 0,
                 "Native view does not duplicate the current canonical draw");
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "producer-driven Native surface presents directly through OpenGL");
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "Native surface presents again without an intervening packet");
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle)
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            semantic.triangles[triangle].vertices[vertex].x +=
                20 * INT32_C(65536);
            semantic.triangles[triangle].vertices[vertex].native_view_x +=
                20 * INT32_C(65536);
        }
    expect_true(gpu_native_submit_gp0_packet(
                    words, 4, &semantic, &source) == 1,
                "second structural frame records an online midpoint draw");
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "changed source VBlank presents the Native midpoint");
    gl_renderer_native_midpoint_diag(&midpoint_diag);
    expect_true(midpoint_diag.previous_usable,
                "Native midpoint retains a usable previous frame");
    expect_true(midpoint_diag.midpoint_presents ==
                    midpoint_presents_before + 1u,
                "Native midpoint presents one changed frame");
    expect_true(midpoint_diag.current_presents ==
                    current_presents_before + 2u,
                "Native midpoint presents two current frames");
    expect_true(midpoint_diag.frame_open &&
                    midpoint_diag.current_pending_present &&
                    midpoint_diag.deferred_current_frames != 0u &&
                    midpoint_diag.deferred_current_flushes != 0u,
                "Native midpoint snapshots the complete current frame for its duplicate VBlank");
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "duplicate VBlank rasterizes and presents the deferred current tail");
    expect_true(gl_renderer_native_view_peek(0, 132, 11, 1, 1, &native),
                "deferred current geometry is readable after the duplicate VBlank");
    expect_pixel(native, WHITE_1555,
                 "duplicate VBlank rasterizes the authoritative current position");
    gl_renderer_native_midpoint_diag(&midpoint_diag);
    expect_true(midpoint_diag.current_presents ==
                    current_presents_before + 3u &&
                    !midpoint_diag.current_pending_present &&
                    midpoint_diag.frame_open,
                "duplicate VBlank presents the saved current frame and opens the next source frame");
    gl_renderer_invalidate_present();
    expect_true(gl_renderer_present_native_view(0, 0, 240, 0),
                "invalidated Native surface reseeds before presentation");
    expect_true(gl_renderer_native_view_peek(0, 164, 11, 1, 1, &native),
                "reseeded Native surface remains readable");
    expect_pixel(native, 0,
                 "VRAM restore discards stale producer-expanded pixels");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "producer-driven Native view disables independently");
}

static void test_native_view_scales_screen_space_rectangles(void) {
    const uint32_t rectangle[] = {
        0x60ffffffu, gp0_xy(10, 30), gp0_xy(8, 8),
    };
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST, 0x1984u, 0x661u, 0x660u,
    };
    uint16_t canonical = 0;
    uint16_t scaled = 0;
    uint16_t centered = 0;

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "screen-space 2D Native view configures");
    expect_true(gpu_native_submit_gp0_packet(
                    rectangle, sizeof(rectangle) / sizeof(rectangle[0]),
                    NULL, &source) == 1,
                "screen-space rectangle reaches the Native semantic path");
    gl_renderer_flush_cpu_uploads();
    expect_true(read_fbo_pixel(11, 31, &canonical),
                "screen-space rectangle reads the canonical FBO");
    expect_true(gl_renderer_native_view_peek(0, 14, 31, 1, 1, &scaled),
                "screen-space rectangle reads its 16:9-scaled coordinate");
    expect_true(gl_renderer_native_view_peek(0, 64, 31, 1, 1, &centered),
                "screen-space rectangle reads the old centered coordinate");
    expect_pixel(canonical, WHITE_1555,
                 "screen-space scaling leaves canonical geometry unchanged");
    expect_pixel(scaled, WHITE_1555,
                 "screen-space rectangle scales horizontally to 16:9");
    expect_pixel(centered, 0,
                 "screen-space rectangle is not centered as 3D fallback");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                 "screen-space 2D Native view disables independently");
}

static void test_native_view_centers_animated_dialogue_window(void) {
    const uint32_t rectangle[] = {
        0x62406080u, gp0_xy(10, 100), gp0_xy(37, 23),
    };
    const uint32_t unrelated_rectangle[] = {
        0x62406080u, gp0_xy(10, 150), gp0_xy(37, 23),
    };
    const GpuRenderOracleSource dialogue_source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST,
        0x000c2778u, 0x000c2778u / 4u, 0x600u,
    };
    const GpuRenderOracleSource unrelated_source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST,
        0x000c2798u, 0x000c2798u / 4u, 0x600u,
    };
    uint16_t scaled = 0;
    uint16_t centered = 0;

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "animated dialogue Native view configures");
    expect_true(gpu_native_submit_gp0_packet(
                    rectangle, sizeof(rectangle) / sizeof(rectangle[0]),
                    NULL, &dialogue_source) == 1,
                "animated dialogue body reaches the Native semantic path");
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_native_view_peek(0, 14, 101, 1, 1, &scaled),
                "animated dialogue stretched coordinate is readable");
    expect_true(gl_renderer_native_view_peek(0, 64, 101, 1, 1, &centered),
                "animated dialogue centered coordinate is readable");
    expect_pixel(scaled, 0,
                 "animated dialogue body is not stretched to widescreen");
    expect_true(centered != 0,
                "animated dialogue body stays in the centered 4:3 plane");

    scaled = centered = 0;
    expect_true(gpu_native_submit_gp0_packet(
                    unrelated_rectangle,
                    sizeof(unrelated_rectangle) /
                        sizeof(unrelated_rectangle[0]),
                    NULL, &unrelated_source) == 1,
                "nearby unrelated rectangle reaches the Native semantic path");
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_native_view_peek(0, 14, 151, 1, 1, &scaled) &&
                    gl_renderer_native_view_peek(
                        0, 64, 151, 1, 1, &centered),
                "nearby unrelated rectangle coordinates are readable");
    expect_true(scaled != 0 && centered == 0,
                "only the static dialogue packet family stays centered");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "animated dialogue Native view disables independently");
}

static void test_native_view_preserves_screen_space_primitive_size(void) {
    const uint32_t left_quad[] = {
        0x28ffffffu,
        gp0_xy(10, 30), gp0_xy(74, 30), gp0_xy(10, 38), gp0_xy(74, 38),
    };
    const uint32_t right_quad[] = {
        0x28ffffffu,
        gp0_xy(246, 50), gp0_xy(310, 50), gp0_xy(246, 58), gp0_xy(310, 58),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    uint16_t outside_left = 0;
    uint16_t left = 0;
    uint16_t right = 0;
    uint16_t outside_right = 0;
    int translated_left;
    int translated_right;
    int native_width;

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "preserve-size Native view configures");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    left_quad, sizeof(left_quad) / sizeof(left_quad[0]),
                    &environment, &semantic) == 1,
                "left preserve-size test builds canonical semantic geometry");
    semantic.screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE;
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK,
                "left preserve-size semantic reaches canonical and Native views");
    gl_renderer_flush_cpu_uploads();

    native_width = gl_renderer_native_view_width();
    expect_true(native_width == 426,
                "Native 16:9 surface uses the symmetric even raster width");
    translated_left = 10 * native_width / 320;
    translated_right = translated_left + 64;
    expect_true(gl_renderer_native_view_peek(
                    0, translated_left - 1, 31, 1, 1, &outside_left) &&
                gl_renderer_native_view_peek(
                    0, translated_left, 31, 1, 1, &left) &&
                gl_renderer_native_view_peek(
                    0, translated_right - 1, 31, 1, 1, &right) &&
                gl_renderer_native_view_peek(
                    0, translated_right, 31, 1, 1, &outside_right),
                "preserve-size Native view pixels are readable");
    expect_pixel(outside_left, 0,
                 "preserve-size primitive leaves its left neighbor untouched");
    expect_pixel(left, WHITE_1555,
                 "left preserve-size primitive retains its stretched left edge");
    expect_pixel(right, WHITE_1555,
                 "preserve-size primitive retains its canonical width");
    expect_pixel(outside_right, 0,
                 "preserve-size primitive does not stretch past its right edge");

    expect_true(gpu_native_semantic_from_gp0(
                    right_quad, sizeof(right_quad) / sizeof(right_quad[0]),
                    &environment, &semantic) == 1,
                "right preserve-size test builds canonical semantic geometry");
    semantic.screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE;
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK,
                "right preserve-size semantic reaches canonical and Native views");
    gl_renderer_flush_cpu_uploads();

    translated_right = 310 * native_width / 320;
    translated_left = translated_right - 64;
    outside_left = left = right = outside_right = 0;
    expect_true(gl_renderer_native_view_peek(
                    0, translated_left - 1, 51, 1, 1, &outside_left) &&
                gl_renderer_native_view_peek(
                    0, translated_left, 51, 1, 1, &left) &&
                gl_renderer_native_view_peek(
                    0, translated_right - 1, 51, 1, 1, &right) &&
                gl_renderer_native_view_peek(
                    0, translated_right, 51, 1, 1, &outside_right),
                "right preserve-size Native view pixels are readable");
    expect_pixel(outside_left, 0,
                 "right preserve-size primitive starts at canonical width");
    expect_pixel(left, WHITE_1555,
                 "right preserve-size primitive retains its canonical width");
    expect_pixel(right, WHITE_1555,
                 "right preserve-size primitive retains its stretched right edge");
    expect_pixel(outside_right, 0,
                 "right preserve-size primitive leaves its right neighbor untouched");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "preserve-size Native view disables independently");
}

static void test_native_view_expands_fullscreen_fade(void) {
    const uint32_t opaque_quad[] = {
        0x28ffffffu,
        gp0_xy(0, 0), gp0_xy(320, 0), gp0_xy(0, 224), gp0_xy(320, 224),
    };
    const uint32_t fade_quad[] = {
        0x2a000000u,
        gp0_xy(0, 0), gp0_xy(320, 0), gp0_xy(0, 224), gp0_xy(320, 224),
    };
    const uint32_t clear[] = {
        0x02000000u, gp0_xy(0, 0), gp0_xy(320, 224),
    };
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST, 0x1c04u, 0x701u, 0x700u,
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    uint16_t center = 0;
    uint16_t margin = 0;
    uint16_t right_margin = 0;
    uint16_t rightmost_margin = 0;
    int native_width;

    reset_gpu_for_case();
    gpu_write_gp1(0x07000000u | 0x10u | (0xf0u << 10u));
    gpu_write_gp0(0xe3000000u);
    gpu_write_gp0(0xe4037d3eu);
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "320x224 fullscreen fade Native view configures");
    native_width = gl_renderer_native_view_width();
    expect_true(native_width == 426,
                "fullscreen fade uses the symmetric Native surface width");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    opaque_quad, 5, &environment, &semantic) == 1,
                "fullscreen fade fixture builds the opaque base quad");
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle) {
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &semantic.triangles[triangle].vertices[vertex];
            const int x = position->x / INT32_C(65536);
            position->native_view_x =
                (x == 0 ? 0 : native_width) * INT32_C(65536);
            position->native_view_y = position->y;
            position->native_view_position = 1u;
        }
    }
    expect_true(gpu_native_submit_gp0_packet(
                    opaque_quad, sizeof(opaque_quad) / sizeof(opaque_quad[0]),
                    &semantic, &source) == 1,
                "fullscreen fade fixture paints the Native-wide base");
    expect_true(gpu_native_submit_gp0_packet(
                    fade_quad, sizeof(fade_quad) / sizeof(fade_quad[0]),
                    NULL, &source) == 1,
                "Native fallback submits the semitransparent fullscreen fade");
    expect_true(gl_renderer_native_view_peek(0, 160, 120, 1, 1, &center),
                "fullscreen fade reads its centered Native pixel");
    expect_true(gl_renderer_native_view_peek(0, 2, 120, 1, 1, &margin),
                "fullscreen fade reads its revealed-margin pixel");
    expect_true(gl_renderer_native_view_peek(0, native_width - 2, 120, 1, 1,
                                             &right_margin),
                "fullscreen fade reads the final Native surface pixel");
    expect_true(gl_renderer_native_view_peek(0, native_width - 1, 120, 1, 1,
                                             &rightmost_margin),
                "fullscreen fade reads the rightmost 16:9 Native pixel");
    expect_pixel(margin, center,
                 "fullscreen fade covers the Native-wide revealed margin");
    expect_pixel(right_margin, center,
                 "fullscreen fade covers the right Native-wide margin");
    expect_pixel(rightmost_margin, center,
                 "fullscreen fade covers the rightmost Native-wide pixel");
    expect_true(gpu_native_submit_gp0_packet(
                    clear, sizeof(clear) / sizeof(clear[0]), NULL, &source) == 1,
                "fullscreen fade fixture clears its canonical test surface");
    gpu_write_gp1(0x07000000u | 0x10u | (0x100u << 10u));
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "fullscreen fade Native view disables independently");
}

static void test_native_view_expands_gouraud_and_textured_overlays(void) {
    const uint32_t gouraud_quad[] = {
        0x38ffffffu, gp0_xy(0, 0),
        0x00ffffffu, gp0_xy(320, 0),
        0x00ffffffu, gp0_xy(0, 224),
        0x00ffffffu, gp0_xy(320, 224),
    };
    const uint16_t tpage = UINT16_C(0x0101);
    const uint32_t textured_quad[] = {
        0x2dffffffu,
        gp0_xy(0, 0), 0u,
        gp0_xy(320, 0), (uint32_t)tpage << 16u | 1u,
        gp0_xy(0, 224), 0x00000100u,
        gp0_xy(320, 224), 0x00000101u,
    };
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST, 0x1c84u, 0x711u, 0x710u,
    };
    uint16_t left = 0;
    uint16_t right = 0;

    reset_gpu_for_case();
    gpu_write_gp0(0xe3000000u);
    gpu_write_gp0(0xe4037d3eu);
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "Gouraud fullscreen Native view configures");
    expect_true(gpu_native_submit_gp0_packet(
                    gouraud_quad,
                    sizeof(gouraud_quad) / sizeof(gouraud_quad[0]),
                    NULL, &source) == 1,
                "Gouraud fullscreen overlay reaches the Native semantic path");
    expect_true(gl_renderer_native_view_peek(0, 2, 100, 1, 1, &left) &&
                gl_renderer_native_view_peek(0, 425, 100, 1, 1, &right),
                "Gouraud fullscreen Native margins are readable");
    expect_pixel(left, WHITE_1555,
                 "Gouraud fullscreen overlay covers the left Native margin");
    expect_pixel(right, WHITE_1555,
                 "Gouraud fullscreen overlay covers the right Native margin");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "Gouraud fullscreen Native view resets");

    reset_gpu_for_case();
    gpu_write_gp0(0xe3000000u);
    gpu_write_gp0(0xe4037d3eu);
    gr_vram_write(64, 0, WHITE_1555);
    gr_vram_write(65, 0, WHITE_1555);
    gr_vram_write(64, 1, WHITE_1555);
    gr_vram_write(65, 1, WHITE_1555);
    gl_renderer_flush_cpu_uploads();
    gpu_ws_set_nw_backdrop(0);
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "textured fullscreen Native view configures");
    expect_true(gpu_native_submit_gp0_packet(
                    textured_quad,
                    sizeof(textured_quad) / sizeof(textured_quad[0]),
                    NULL, &source) == 1,
                "textured fullscreen overlay reaches the Native semantic path");
    left = right = 0;
    expect_true(gl_renderer_native_view_peek(0, 2, 100, 1, 1, &left) &&
                gl_renderer_native_view_peek(0, 425, 100, 1, 1, &right),
                "textured fullscreen Native margins are readable");
    expect_pixel(left, WHITE_1555,
                 "textured fullscreen overlay covers the left Native margin");
    expect_pixel(right, WHITE_1555,
                 "textured fullscreen overlay covers the right Native margin");
    gpu_ws_set_nw_backdrop(1);
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "textured fullscreen Native view disables independently");
}

static void test_native_view_flip_clears_only_retired_margins(void) {
    const uint32_t words[] = {
        0x20ffffffu,
        gp0_xy(10, 10), gp0_xy(18, 10), gp0_xy(10, 18),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    uint16_t margin = 0;
    uint16_t center = 0;

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "flip invalidation Native view configures");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, sizeof(words) / sizeof(words[0]),
                    &environment, &semantic) == 1,
                "flip invalidation builds semantic geometry");
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle) {
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &semantic.triangles[triangle].vertices[vertex];
            position->native_view_x = position->x;
            position->native_view_y = position->y;
            position->native_view_position = 1u;
        }
    }
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK,
                "flip invalidation paints canonical and Native surfaces");
    expect_true(gl_renderer_native_view_peek(0, 11, 11, 1, 1, &margin) &&
                gl_renderer_native_view_peek(0, 64, 11, 1, 1, &center),
                "flip invalidation setup pixels are readable");
    expect_pixel(margin, WHITE_1555,
                 "source-derived primitive initially paints the Native margin");
    expect_pixel(center, WHITE_1555,
                 "canonical seed remains visible in the Native center");

    gpu_write_gp1(0x05000000u | (256u << 10u));
    margin = center = 0;
    expect_true(gl_renderer_native_view_peek(0, 11, 11, 1, 1, &margin) &&
                gl_renderer_native_view_peek(0, 64, 11, 1, 1, &center),
                "retired Native band remains readable after the flip");
    expect_pixel(margin, 0,
                 "real framebuffer flip clears the retired synthetic margin");
    expect_pixel(center, WHITE_1555,
                 "real framebuffer flip preserves the retired canonical center");

    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle) {
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &semantic.triangles[triangle].vertices[vertex];
            position->y += 10 * INT32_C(65536);
            position->native_view_y += 10 * INT32_C(65536);
        }
    }
    expect_true(gr_draw_semantic_immediate(&semantic) ==
                    GPU_RENDER_TRANSACTION_OK,
                "retired band accepts the next frame's Native draw");
    gpu_write_gp1(0x05000000u | (256u << 10u));
    margin = 0;
    expect_true(gl_renderer_native_view_peek(0, 11, 21, 1, 1, &margin),
                "redundant-flip Native margin remains readable");
    expect_pixel(margin, WHITE_1555,
                 "redundant GP1 display start does not clear an active draw band");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "flip invalidation Native view disables independently");
}

static void test_native_view_uses_effective_draw_destination(void) {
    const uint32_t offset[] = { 0xe5000140u };
    const uint32_t quad[] = {
        0x28ffffffu,
        gp0_xy(0, 0), gp0_xy(320, 0), gp0_xy(0, 224), gp0_xy(320, 224),
    };
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST, 0x1d44u, 0x751u, 0x750u,
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    uint16_t center = 0;
    uint16_t left_margin = 0;
    uint16_t right_margin = 0;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "effective-destination Native view configures");
    expect_true(gpu_native_submit_gp0_packet(
                    offset, sizeof(offset) / sizeof(offset[0]), NULL, &source) == 1,
                "effective-destination offset applies before the fullscreen quad");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    quad, sizeof(quad) / sizeof(quad[0]), &environment,
                    &semantic) == 1,
                "effective-destination fixture builds a fullscreen semantic");
    expect_true(gpu_native_submit_gp0_packet(
                    quad, sizeof(quad) / sizeof(quad[0]), &semantic, &source) == 1,
                "fullscreen semantic renders into its offset framebuffer");
    expect_true(gl_renderer_present_native_view(320, 0, 240, 0),
                "Native presentation selects the offset framebuffer");
    expect_true(gl_renderer_native_view_peek(320, 160, 100, 1, 1, &center),
                "offset fullscreen semantic reads the Native center");
    expect_true(gl_renderer_native_view_peek(320, 2, 100, 1, 1,
                                             &left_margin),
                "offset fullscreen semantic reads the left Native margin");
    expect_true(gl_renderer_native_view_peek(320, 425, 100, 1, 1,
                                             &right_margin),
                "offset fullscreen semantic reads the right Native margin");
    expect_pixel(center, WHITE_1555,
                 "offset fullscreen semantic updates the Native center");
    expect_pixel(left_margin, WHITE_1555,
                 "offset fullscreen semantic updates the left Native margin");
    expect_pixel(right_margin, WHITE_1555,
                 "offset fullscreen semantic updates the right Native margin");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "effective-destination Native view disables independently");
}

static void test_native_view_expands_offset_fullscreen_filter(void) {
    const uint32_t draw_area_top[] = {
        0xe3000000u | (256u << 10u),
    };
    const uint32_t draw_area_bottom[] = {
        0xe4000000u | 319u | (479u << 10u),
    };
    const uint32_t draw_offset[] = {
        0xe5000000u | (256u << 11u),
    };
    const uint32_t draw_mode[] = { 0xe1000040u };
    const uint32_t opaque_quad[] = {
        0x28ffffffu,
        gp0_xy(0, 0), gp0_xy(320, 0), gp0_xy(0, 224), gp0_xy(320, 224),
    };
    const uint32_t filter[] = {
        0x6264a073u, gp0_xy(0, 0), gp0_xy(320, 224),
    };
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST, 0x1d64u, 0x759u, 0x758u,
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic = {0};
    uint16_t center = 0;
    uint16_t left_margin = 0;
    uint16_t right_margin = 0;
    int native_width;

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "offset-filter Native view configures");
    native_width = gl_renderer_native_view_width();
    expect_true(gpu_native_submit_gp0_packet(
                    draw_area_top, 1, NULL, &source) == 1 &&
                    gpu_native_submit_gp0_packet(
                        draw_area_bottom, 1, NULL, &source) == 1 &&
                    gpu_native_submit_gp0_packet(
                        draw_offset, 1, NULL, &source) == 1,
                "offset-filter draw target state applies in order");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    opaque_quad, sizeof(opaque_quad) / sizeof(opaque_quad[0]),
                    &environment, &semantic) == 1,
                "offset-filter fixture builds its opaque base");
    for (uint8_t triangle = 0u; triangle < semantic.triangle_count; ++triangle) {
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *position =
                &semantic.triangles[triangle].vertices[vertex];
            const int x = position->x / INT32_C(65536);

            position->native_view_x =
                (x == 0 ? 0 : native_width) * INT32_C(65536);
            position->native_view_y = position->y;
            position->native_view_position = 1u;
        }
    }
    expect_true(gpu_native_submit_gp0_packet(
                    opaque_quad, sizeof(opaque_quad) / sizeof(opaque_quad[0]),
                    &semantic, &source) == 1 &&
                    gpu_native_submit_gp0_packet(
                        draw_mode, 1, NULL, &source) == 1 &&
                    gpu_native_submit_gp0_packet(
                        filter, sizeof(filter) / sizeof(filter[0]),
                        NULL, &source) == 1,
                "offset fullscreen filter reaches the Native semantic path");
    expect_true(gl_renderer_native_view_peek(
                    0, 160, 356, 1, 1, &center) &&
                    gl_renderer_native_view_peek(
                        0, 2, 356, 1, 1, &left_margin) &&
                    gl_renderer_native_view_peek(
                        0, native_width - 1, 356, 1, 1, &right_margin),
                "offset fullscreen filter pixels are readable");
    expect_true(center != WHITE_1555,
                "offset fullscreen filter modifies the Native center");
    expect_pixel(left_margin, center,
                 "offset fullscreen filter covers the left Native margin");
    expect_pixel(right_margin, center,
                 "offset fullscreen filter covers the right Native margin");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "offset-filter Native view disables independently");
}

static void test_native_view_backdrop_right_edge_coverage(void) {
    const uint32_t textured_quad[] = {
        0x2dffffffu,
        gp0_xy(0, 100), 0u,
        gp0_xy(372, 100), (0x0100u << 16u) | 1u,
        gp0_xy(0, 200), 0x00000100u,
        gp0_xy(372, 200), (0x0100u << 16u) | 0x00000101u,
    };
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST, 0x1d04u, 0x741u, 0x740u,
    };
    uint16_t edge = 0;

    reset_gpu_for_case();
    gpu_write_gp0(0xe3000000u);
    gpu_write_gp0(0xe4037d3eu);
    gr_vram_write(0, 0, WHITE_1555);
    gr_vram_write(1, 0, WHITE_1555);
    gr_vram_write(0, 1, WHITE_1555);
    gr_vram_write(1, 1, WHITE_1555);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "textured backdrop Native view configures");
    expect_true(gpu_native_submit_gp0_packet(
                    textured_quad,
                    sizeof(textured_quad) / sizeof(textured_quad[0]),
                    NULL, &source) == 1,
                "textured backdrop reaches the Native edge path");
    expect_true(gl_renderer_native_view_peek(0, 425, 104, 1, 1, &edge),
                "textured backdrop reads the final Native edge pixel");
    expect_pixel(edge, WHITE_1555,
                 "textured backdrop covers the final Native edge pixel");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "textured backdrop Native view disables independently");
}

static void test_native_view_tracks_ordered_packet_mutations(void) {
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST, 0x1a04u, 0x681u, 0x680u,
    };
    const uint32_t seed_triangle[] = {
        0x20ffffffu,
        gp0_xy(10, 10), gp0_xy(18, 10), gp0_xy(10, 18),
    };
    const uint32_t fill[] = {
        0x020000ffu, gp0_xy(0, 32), gp0_xy(320, 16),
    };
    const uint32_t copy[] = {
        0x80000000u, gp0_xy(20, 60), gp0_xy(30, 60), gp0_xy(1, 1),
    };
    const uint32_t wrap_copy[] = {
        0x80000000u, gp0_xy(100, 100), gp0_xy(1023, 511), gp0_xy(2, 2),
    };
    const uint32_t line[] = {
        0x40ffffffu, gp0_xy(10, 70), gp0_xy(20, 70),
    };
    const uint32_t wrap_fill[] = {
        0x02ff0000u, gp0_xy(0, 510), gp0_xy(320, 4),
    };
    const uint32_t partial_area[] = {
        0xe3000000u | 40u | (80u << 10u),
        0xe4000000u | 60u | (100u << 10u),
    };
    const uint32_t clipped_triangle[] = {
        0x20ffffffu,
        gp0_xy(30, 85), gp0_xy(50, 85), gp0_xy(30, 95),
    };
    const uint32_t full_area[] = { 0xe3000000u, 0xe407ffffu };
    const uint16_t upload_green = UINT16_C(0x03e0);
    const uint16_t upload_blue = UINT16_C(0x7c00);
    const uint16_t wrap_copy_source[4] = {
        UINT16_C(0x001f), UINT16_C(0x03e0),
        UINT16_C(0x7c00), UINT16_C(0x7fff),
    };
    uint16_t pair[2] = {0, 0};
    uint16_t pixel = 0;

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "ordered-mutation Native surface configures");
    expect_true(gpu_native_submit_gp0_packet(
                    seed_triangle,
                    sizeof(seed_triangle) / sizeof(seed_triangle[0]),
                    NULL, &source) == 1,
                "ordered-mutation case seeds the Native surface");
    expect_true(gpu_native_submit_gp0_packet(
                    fill, sizeof(fill) / sizeof(fill[0]), NULL, &source) == 1,
                "Native packet fill executes without parser replay");
    expect_true(gl_renderer_native_view_peek(0, 5, 33, 1, 1, &pixel),
                "Native packet fill reads a revealed-margin pixel");
    expect_pixel(pixel, UINT16_C(0x001f),
                 "full framebuffer fill refreshes Native margins");
    expect_true(gl_renderer_native_view_peek(0, 5, 33, 2, 1, pair),
                "Native readback samples a multi-pixel scaled region");
    expect_pixel(pair[0], UINT16_C(0x001f),
                 "scaled Native readback preserves the first logical pixel");
    expect_pixel(pair[1], UINT16_C(0x001f),
                 "scaled Native readback preserves the second logical pixel");

    expect_true(gpu_native_submit_gp0_packet(
                    wrap_fill, sizeof(wrap_fill) / sizeof(wrap_fill[0]),
                    NULL, &source) == 1,
                "Native packet fill preserves wrapped Y semantics");
    expect_true(gl_renderer_native_view_peek(0, 5, 0, 1, 1, &pixel),
                "wrapped Native fill reads its top-row destination");
    expect_pixel(pixel, upload_blue,
                 "wrapped Native fill updates the wrapped rows");
    expect_true(gl_renderer_present_native_view(960, 0, 240, 0),
                "wrapped Native framebuffer base prepares for presentation");
    expect_true(gl_renderer_native_view_peek(960, 117, 0, 1, 1, &pixel),
                "wrapped Native seed reads canonical X zero");
    expect_pixel(pixel, upload_blue,
                 "wrapped Native seed imports canonical X zero");

    gr_vram_transfer_in(0, 40, 1, 1, &upload_green);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_native_view_peek(960, 117, 40, 1, 1, &pixel),
                "wrapped Native upload mirror reads canonical X zero");
    expect_pixel(pixel, upload_green,
                 "wrapped Native upload mirror updates the wrapped surface");

    gr_vram_transfer_in(10, 50, 1, 1, &upload_green);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_native_view_peek(0, 63, 50, 1, 1, &pixel),
                "post-seed CPU upload reads the centered Native coordinate");
    expect_pixel(pixel, upload_green,
                 "post-seed CPU upload updates the Native surface");

    gr_vram_transfer_in(20, 60, 1, 1, &upload_blue);
    gl_renderer_flush_cpu_uploads();
    expect_true(gpu_native_submit_gp0_packet(
                    copy, sizeof(copy) / sizeof(copy[0]), NULL, &source) == 1,
                "Native packet VRAM copy executes without parser replay");
    expect_true(gl_renderer_native_view_peek(0, 83, 60, 1, 1, &pixel),
                "Native packet copy reads the centered destination");
    expect_pixel(pixel, upload_blue,
                 "Native packet copy updates the Native surface in order");

    gr_vram_transfer_in(100, 100, 2, 2, wrap_copy_source);
    gl_renderer_flush_cpu_uploads();
    expect_true(gpu_native_submit_gp0_packet(
                    wrap_copy, sizeof(wrap_copy) / sizeof(wrap_copy[0]),
                    NULL, &source) == 1,
                "Native packet VRAM copy accepts wrapped X/Y destinations");
    expect_true(read_fbo_pixel(1023, 511, &pixel),
                "wrapped copy reads canonical bottom-right destination");
    expect_pixel(pixel, wrap_copy_source[0],
                 "wrapped copy preserves canonical bottom-right texel");
    expect_true(read_fbo_pixel(0, 0, &pixel),
                "wrapped copy reads canonical top-left destination");
    expect_pixel(pixel, wrap_copy_source[3],
                 "wrapped copy preserves canonical top-left texel");
    expect_true(gl_renderer_native_view_peek(960, 116, 0, 1, 1, &pixel),
                "wrapped copy reads the right Native surface top row");
    expect_pixel(pixel, wrap_copy_source[2],
                 "wrapped copy updates the right Native surface after Y wrap");
    expect_true(gl_renderer_native_view_peek(960, 117, 0, 1, 1, &pixel),
                "wrapped copy reads canonical X zero on the right Native surface");
    expect_pixel(pixel, wrap_copy_source[3],
                 "wrapped copy updates canonical X zero on the wrapped surface");
    expect_true(gl_renderer_native_view_peek(0, 53, 511, 1, 1, &pixel),
                "wrapped copy reads the left Native surface bottom row");
    expect_pixel(pixel, wrap_copy_source[1],
                 "wrapped copy updates the left Native surface after X wrap");

    expect_true(gpu_native_submit_gp0_packet(
                    line, sizeof(line) / sizeof(line[0]), NULL, &source) == 1,
                "Native packet line routes through semantic OpenGL raster");
    expect_true(gl_renderer_native_view_peek(0, 68, 70, 1, 1, &pixel),
                "Native packet line reads its centered wide coordinate");
    expect_pixel(pixel, WHITE_1555,
                 "Native packet line updates the Native surface");

    expect_true(gpu_native_submit_gp0_packet(
                    &partial_area[0], 1, NULL, &source) == 1 &&
                    gpu_native_submit_gp0_packet(
                        &partial_area[1], 1, NULL, &source) == 1,
                "Native partial draw area applies in packet order");
    expect_true(gpu_native_submit_gp0_packet(
                    clipped_triangle,
                    sizeof(clipped_triangle) / sizeof(clipped_triangle[0]),
                    NULL, &source) == 1,
                "Native triangle rasterizes under a partial draw area");
    expect_true(gl_renderer_native_view_peek(0, 88, 86, 1, 1, &pixel),
                "partial-area rejected pixel reads the Native surface");
    expect_pixel(pixel, 0,
                 "partial draw area clips the Native surface left edge");
    expect_true(gl_renderer_native_view_peek(0, 98, 86, 1, 1, &pixel),
                "partial-area accepted pixel reads the Native surface");
    expect_pixel(pixel, WHITE_1555,
                 "partial draw area retains Native pixels inside the clip");
    expect_true(gpu_native_submit_gp0_packet(
                    &full_area[0], 1, NULL, &source) == 1 &&
                    gpu_native_submit_gp0_packet(
                        &full_area[1], 1, NULL, &source) == 1,
                "Native full draw area restores after partial clipping");
    expect_true(gl_renderer_configure_native_view(0, 4, 3, 320, 240),
                "ordered-mutation Native surface disables independently");
}

static void test_flat_batch_uses_submission_mask_check(void) {
    uint16_t pixel = 0;

    /* Given: a masked destination pixel and GP0(E6) destination checking disabled. */
    reset_gpu_for_case();
    gr_vram_write(FLAT_X, FLAT_Y, MASKED_BLACK_1555);
    gl_renderer_flush_cpu_uploads();
    expect_true(read_fbo_pixel(FLAT_X, FLAT_Y, &pixel),
                "flat setup reads the OpenGL FBO");
    expect_pixel(pixel, MASKED_BLACK_1555,
                 "flat setup uploads the masked destination to OpenGL");
    gpu_write_gp0(0xe6000000u);

    /* When: a white flat dot is queued, then GP0(E6) enables destination checking. */
    gpu_write_gp0(0x68ffffffu);
    gpu_write_gp0(gp0_xy(FLAT_X, FLAT_Y));
    gpu_write_gp0(0xe6000002u);
    gl_renderer_flush_cpu_uploads();

    /* Then: E6 semantics keep the queued draw's check=off state, yielding 0x7fff. */
    expect_true(read_fbo_pixel(FLAT_X, FLAT_Y, &pixel),
                "flat result reads the OpenGL FBO");
    expect_pixel(pixel, WHITE_1555,
                 "flat batch preserves its GP0(E6) check=off submission state");
}

static void test_wide_fullscreen_flat_rect_blends_margins_once(void) {
    enum { WIDE_WIDTH = 426, WIDE_OFFSET = 53 };
    uint16_t center = 0;
    uint32_t *wide = NULL;
    int width = 0;
    int height = 0;

    reset_gpu_for_case();
    gr_fill_rect(0, 0, 1024, 512, 0);
    gl_renderer_flush_cpu_uploads();
    gr_wide_configure(WIDE_WIDTH, WIDE_OFFSET);
    gr_wide_set_target(0);
    gr_set_semi_transparency(1, 0);
    gr_draw_flat_rect(0, 0, 320, 224, WHITE_1555);
    gl_renderer_flush_cpu_uploads();
    expect_true(read_fbo_pixel(160, 100, &center),
                "fullscreen flat rect reads its canonical composition");

    wide = malloc((size_t)WIDE_WIDTH * 2u * 512u * 2u * sizeof(*wide));
    expect_true(wide != NULL, "wide fullscreen test allocates its readback buffer");
    if (wide != NULL) {
        expect_true(gr_wide_dump_full(wide, WIDE_WIDTH * 2 * 512 * 2,
                                      &width, &height, 0) > 0,
                    "fullscreen flat rect reads the wide compositor surface");
        expect_true(width == WIDE_WIDTH * 2 && height == 512 * 2,
                    "wide compositor dump reports the scaled dimensions");
        if (width == WIDE_WIDTH * 2 && height == 512 * 2) {
            expect_pixel(argb8888_to_rgb555(wide[(size_t)100 * 2u * width +
                                                   2u * 2u]),
                         center,
                         "fullscreen flat rect blends the left margin once");
            expect_pixel(argb8888_to_rgb555(
                             wide[(size_t)100 * 2u * width +
                                  (size_t)(WIDE_OFFSET + 160) * 2u]),
                         center,
                         "fullscreen flat rect blends the center once");
            expect_pixel(argb8888_to_rgb555(
                             wide[(size_t)100 * 2u * width +
                                  (size_t)(WIDE_WIDTH - 3) * 2u]),
                         center,
                         "fullscreen flat rect blends the right margin once");
        }
        free(wide);
    }
    gr_set_semi_transparency(0, 0);
    gr_wide_disable_target();
    gr_wide_configure(0, 0);
}

static void test_textured_batch_uses_submission_mask_check(void) {
    uint16_t pixel = 0;

    /* Given: a masked destination, an opaque 15-bit white texel, and E6 check=on. */
    reset_gpu_for_case();
    gpu_write_gp0(0xe6000000u);
    gr_vram_write(0, 0, WHITE_1555);
    gr_vram_write(TEXTURED_X, TEXTURED_Y, MASKED_BLACK_1555);
    gl_renderer_flush_cpu_uploads();
    expect_true(read_fbo_pixel(TEXTURED_X, TEXTURED_Y, &pixel),
                "textured setup reads the OpenGL FBO");
    expect_pixel(pixel, MASKED_BLACK_1555,
                 "textured setup uploads the masked destination to OpenGL");
    gpu_write_gp0(0xe1000000u | TEXTURE_PAGE_15BPP);
    gpu_write_gp0(0xe6000002u);

    /* When: a raw-textured dot is queued, then GP0(E6) disables destination checking. */
    gpu_write_gp0(0x6d000000u);
    gpu_write_gp0(gp0_xy(TEXTURED_X, TEXTURED_Y));
    gpu_write_gp0(0x00000000u);
    gpu_write_gp0(0xe6000000u);
    gl_renderer_flush_cpu_uploads();

    /* Then: E6 semantics reject the queued draw, preserving the masked 0x8000 pixel. */
    expect_true(read_fbo_pixel(TEXTURED_X, TEXTURED_Y, &pixel),
                "textured result reads the OpenGL FBO");
    expect_pixel(pixel, MASKED_BLACK_1555,
                 "textured batch preserves its GP0(E6) check=on submission state");
}

static void test_native_fmv_cpu_present_path(void) {
    static const uint32_t pixels[4] = {
        0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu,
    };

    expect_true(gr_present_cpu_frame(pixels, 2, 2, 0, 1, 0),
                "Native FMV CPU frame uses the OpenGL presentation backend");
}

int main(void) {
    SDL_Window *window;

    test_native_environment_tpage_latching();
    test_native_cull_view_is_independent_of_legacy_wide_mode();
    test_semantic_guest_cull_policy();
    test_native_semantic_vertices_keep_raw_coordinates();
    test_native_portrait_uses_canonical_position();
    test_dialogue_text_is_centered_with_its_box();
    if (failures) return 1;

#if !defined(PSX_SDL3)
    SDL_SetMainReady();
#endif
    SDL_setenv("PSX_GL_PRESENT_HASH", "1", 1);
    expect_true(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL video initializes");
    if (failures) return 1;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    window = SDL_CreateWindow("gpu_gl_mask_order_test", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, 640, 360,
                              SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    expect_true(window != NULL, "hidden SDL OpenGL window is created");
    if (!window) {
        SDL_Quit();
        return 1;
    }

    gr_set_backend(GR_BACKEND_OPENGL);
    gpu_init();
    gr_set_scale(2);
    gr_set_texture_filter(0);
    expect_true(gl_renderer_init_context(window), "OpenGL raster pipeline initializes");
    expect_true(gr_backend() == GR_BACKEND_OPENGL, "OpenGL backend remains selected");
    if (!failures) {
        test_untextured_native_semantic_latches_ordered_blend();
        test_unbound_gp0_packet_rasterizes_natively();
        test_canonical_native_midpoint_policy();
        test_canonical_native_partial_midpoint();
        test_canonical_native_partial_birth_midpoint();
        test_retired_history_mismatch_preserves_native_present();
        test_canonical_native_midpoint_cancellation();
        test_canonical_native_midpoint_duplicate_identity();
        test_temporal_candidate_history_only_participation();
        test_temporal_candidate_duplicate_identity_fails_closed();
        test_native_midpoint_reset_flushes_pending_view_draw();
        test_native_full_width_copy_flushes_pending_view_draw();
        test_native_midpoint_sequence_a_a_b_c();
        test_native_original_present_sequence();
        test_native_rational_present_sequence(120, 4u);
        test_native_rational_present_sequence(240, 8u);
        test_native_wide_rational_present_sequence(120, 4u);
        test_native_wide_rational_present_sequence(240, 8u);
        test_canonical_native_vertical_double_buffer();
        test_native_wide_vertical_double_buffer();
        test_native_wide_pending_current_precedes_vertical_flip();
        test_native_wide_current_and_phase_share_subpixel_raster();
        test_native_wide_suspended_current_uses_integer_raster();
        test_native_wide_projective_geometry_uses_integer_raster();
        test_native_wide_phase_uses_integer_raster();
        test_native_wide_negative_fixed_positions_floor();
        test_native_textured_subpixel_seam_uses_raster_geometry();
        test_native_wide_current_present_sequence(120, 4u);
        test_native_wide_current_present_sequence(240, 8u);
        test_native_view_uses_semantic_wide_positions();
        test_native_view_scales_screen_space_rectangles();
        test_native_view_centers_animated_dialogue_window();
        test_native_view_preserves_screen_space_primitive_size();
        test_native_view_expands_fullscreen_fade();
        test_native_view_expands_gouraud_and_textured_overlays();
        test_native_view_flip_clears_only_retired_margins();
        test_native_view_uses_effective_draw_destination();
        test_native_view_expands_offset_fullscreen_filter();
        test_native_view_backdrop_right_edge_coverage();
        test_native_view_tracks_ordered_packet_mutations();
        test_flat_batch_uses_submission_mask_check();
        test_wide_fullscreen_flat_rect_blends_margins_once();
        test_textured_batch_uses_submission_mask_check();
        test_native_fmv_cpu_present_path();
    }

    gl_renderer_shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) return 1;
    puts("PASS: delayed OpenGL mask-check batches preserve GP0(E6) submission state");
    return 0;
}
