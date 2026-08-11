#include "gpu.h"
#include "gpu_gl_renderer.h"
#include "gpu_render.h"

#include <SDL.h>

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
    expect_true(psx_ws_x_margin() == 54,
                "Native cull view exposes the host's 16:9 edge margin");
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
    expect_true(psx_ws_guest_cull_screen_bias(0u, 0) == 54u,
                "semantic screen-bias policy uses the Native margin");
    expect_true(psx_ws_guest_cull_world_range(427u, 320) == 1 &&
                    psx_ws_guest_cull_world_range(428u, 320) == 0,
                "semantic world-range policy widens both sides");
    expect_true(psx_ws_guest_cull_left_edge(39u) == 0u - 39u - 54u,
                "semantic left-edge policy moves the reject edge");
    expect_true(psx_ws_guest_cull_masked_screen_x(0xffffu, 320u) == 1,
                "semantic masked-screen-X policy preserves wrapped left reveal");
    expect_true(psx_ws_guest_cull_frustum_plane_x(4096) == 3072,
                "semantic frustum-plane policy scales the side normal");
    expect_true(psx_ws_semantic_cull_site(0x80010014u) ==
                    PSX_WS_CULL_SEMANTIC_SIGNED_SCREEN_X &&
                    psx_ws_guest_cull_signed_screen_x(373, 0x140) == 1 &&
                    psx_ws_guest_cull_signed_screen_x(374, 0x140) == 0,
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
    uint16_t canonical = 0;
    uint16_t native = 0;
    uint16_t duplicated = 0;

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "producer-driven Native 16:9 surface configures");
    expect_true(gpu_native_submit_gp0_packet(
                    clear, sizeof(clear) / sizeof(clear[0]), NULL, &source) == 1,
                "Native restore test clears the canonical reseed source");
    gpu_native_environment_get(&environment);
    expect_true(gpu_native_semantic_from_gp0(
                    words, 4, &environment, &semantic) == 1,
                "Native view test builds canonical semantic geometry");
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

    reset_gpu_for_case();
    gpu_write_gp1(0x07000000u | 0x10u | (0xf0u << 10u));
    gpu_write_gp0(0xe3000000u);
    gpu_write_gp0(0xe4037d3eu);
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "320x224 fullscreen fade Native view configures");
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
                (x == 0 ? 0 : 426) * INT32_C(65536);
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
    expect_true(gl_renderer_native_view_peek(0, 425, 120, 1, 1,
                                             &right_margin),
                "fullscreen fade reads the final Native surface pixel");
    expect_true(gl_renderer_native_view_peek(0, 426, 120, 1, 1,
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

    reset_gpu_for_case();
    expect_true(gl_renderer_configure_native_view(1, 16, 9, 320, 240),
                "offset-filter Native view configures");
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
                (x == 0 ? 0 : 427) * INT32_C(65536);
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
                        0, 425, 356, 1, 1, &right_margin),
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

    SDL_SetMainReady();
    expect_true(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL video initializes");
    if (failures) return 1;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    window = SDL_CreateWindow("gpu_gl_mask_order_test", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, 32, 32,
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
        test_native_view_uses_semantic_wide_positions();
        test_native_view_scales_screen_space_rectangles();
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
