#include "gpu_render.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef GPU_RENDER_TRANSACTION_TESTING
#error "test_gpu_render_transaction.c requires GPU_RENDER_TRANSACTION_TESTING"
#endif

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

typedef struct TransactionFixture {
    GpuRenderTransactionStatus begin_status;
    GpuRenderTransactionStatus barrier_status;
    GpuRenderTransactionStatus draw_status;
    GpuRenderTransactionStatus commit_status;
    GpuRenderTransactionStatus rollback_status;
    GpuRenderTransactionId begin_id;
    GpuRenderTransactionId barrier_id;
    GpuRenderTransactionId draw_id;
    GpuRenderTransactionId commit_id;
    GpuRenderTransactionId rollback_id;
    uint64_t begin_serial;
    uint64_t commit_serial;
    const GpuRenderSemantic *semantic;
    const GpuRenderPresent *present;
    unsigned int begin_calls;
    unsigned int barrier_calls;
    unsigned int draw_calls;
    unsigned int commit_calls;
    unsigned int rollback_calls;
    char call_order[32];
    size_t call_count;
} TransactionFixture;

enum MockPrimitive {
    MOCK_FILL_RECT = 0,
    MOCK_COPY_RECT,
    MOCK_FLAT_TRIANGLE,
    MOCK_GOURAUD_TRIANGLE,
    MOCK_TEXTURED_TRIANGLE,
    MOCK_SHADED_TEXTURED_TRIANGLE,
    MOCK_FLAT_RECT,
    MOCK_TEXTURED_RECT,
    MOCK_TEXTURED_RECT_SCALED,
    MOCK_LINE,
    MOCK_SHADED_LINE,
    MOCK_PRIMITIVE_COUNT
};

typedef struct DispatchFixture {
    unsigned int primitive_calls[MOCK_PRIMITIVE_COUNT];
    unsigned int scale_calls;
    unsigned int filter_calls;
    unsigned int semi_calls;
    unsigned int mask_calls;
    unsigned int texture_window_calls;
    unsigned int modulation_calls;
    unsigned int draw_area_calls;
    unsigned int draw_offset_calls;
    unsigned int vram_write_calls;
    unsigned int vram_read_calls;
    unsigned int transfer_in_calls;
    unsigned int transfer_out_calls;
    int scale;
    int filter;
    int semi_enabled;
    int semi_mode;
    int mask_set;
    int mask_check;
    uint32_t texture_window;
    int modulation_r;
    int modulation_g;
    int modulation_b;
    int modulation_raw;
    int draw_area[4];
    int draw_offset[2];
    int vram_x;
    int vram_y;
    uint16_t vram_pixel;
    const uint16_t *transfer_in;
    uint16_t *transfer_out;
} DispatchFixture;

static TransactionFixture fixture;
static DispatchFixture dispatch_fixture;

const GpuRenderBackend *gl_backend_get(void) { return NULL; }
const GpuRenderBackend *vk_backend_get(void) { return NULL; }

static void note_call(char operation) {
    if (fixture.call_count < sizeof(fixture.call_order))
        fixture.call_order[fixture.call_count++] = operation;
}

static GpuRenderTransactionStatus backend_begin(
        GpuRenderTransactionId transaction_id,
        uint64_t vram_mutation_serial) {
    fixture.begin_calls++;
    fixture.begin_id = transaction_id;
    fixture.begin_serial = vram_mutation_serial;
    note_call('B');
    return fixture.begin_status;
}

static GpuRenderTransactionStatus backend_barrier(
        GpuRenderTransactionId transaction_id) {
    fixture.barrier_calls++;
    fixture.barrier_id = transaction_id;
    note_call('O');
    return fixture.barrier_status;
}

static GpuRenderTransactionStatus backend_draw(
        GpuRenderTransactionId transaction_id,
        const GpuRenderSemantic *semantic) {
    fixture.draw_calls++;
    fixture.draw_id = transaction_id;
    fixture.semantic = semantic;
    note_call('D');
    return fixture.draw_status;
}

static GpuRenderTransactionStatus backend_commit(
        GpuRenderTransactionId transaction_id,
        uint64_t current_vram_mutation_serial,
        const GpuRenderPresent *present) {
    fixture.commit_calls++;
    fixture.commit_id = transaction_id;
    fixture.commit_serial = current_vram_mutation_serial;
    fixture.present = present;
    note_call('C');
    return fixture.commit_status;
}

static GpuRenderTransactionStatus backend_rollback(
        GpuRenderTransactionId transaction_id) {
    fixture.rollback_calls++;
    fixture.rollback_id = transaction_id;
    note_call('R');
    return fixture.rollback_status;
}

static void backend_set_scale(int scale) {
    dispatch_fixture.scale_calls++;
    dispatch_fixture.scale = scale;
}

static void backend_set_texture_filter(int filter) {
    dispatch_fixture.filter_calls++;
    dispatch_fixture.filter = filter;
}

static void backend_set_semi_transparency(int enabled, int mode) {
    dispatch_fixture.semi_calls++;
    dispatch_fixture.semi_enabled = enabled;
    dispatch_fixture.semi_mode = mode;
}

static void backend_set_mask_bits(int set_bit, int check_bit) {
    dispatch_fixture.mask_calls++;
    dispatch_fixture.mask_set = set_bit;
    dispatch_fixture.mask_check = check_bit;
}

static void backend_set_texture_window(uint32_t raw) {
    dispatch_fixture.texture_window_calls++;
    dispatch_fixture.texture_window = raw;
}

static void backend_set_color_modulation(int r, int g, int b, int raw_texture) {
    dispatch_fixture.modulation_calls++;
    dispatch_fixture.modulation_r = r;
    dispatch_fixture.modulation_g = g;
    dispatch_fixture.modulation_b = b;
    dispatch_fixture.modulation_raw = raw_texture;
}

static void backend_fill_rect(int x, int y, int w, int h, uint16_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
    dispatch_fixture.primitive_calls[MOCK_FILL_RECT]++;
}

static void backend_copy_rect(int src_x, int src_y, int dst_x, int dst_y,
                              int w, int h) {
    (void)src_x; (void)src_y; (void)dst_x; (void)dst_y; (void)w; (void)h;
    dispatch_fixture.primitive_calls[MOCK_COPY_RECT]++;
}

static void backend_draw_flat_triangle(int x0, int y0, int x1, int y1,
                                       int x2, int y2, uint16_t color) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2; (void)color;
    dispatch_fixture.primitive_calls[MOCK_FLAT_TRIANGLE]++;
}

static void backend_draw_gouraud_triangle(int x0, int y0, uint16_t c0,
                                          int x1, int y1, uint16_t c1,
                                          int x2, int y2, uint16_t c2) {
    (void)x0; (void)y0; (void)c0; (void)x1; (void)y1; (void)c1;
    (void)x2; (void)y2; (void)c2;
    dispatch_fixture.primitive_calls[MOCK_GOURAUD_TRIANGLE]++;
}

static void backend_draw_textured_triangle(int x0, int y0, int u0, int v0,
                                           int x1, int y1, int u1, int v1,
                                           int x2, int y2, int u2, int v2,
                                           uint16_t clut_x, uint16_t clut_y,
                                           uint16_t texpage) {
    (void)x0; (void)y0; (void)u0; (void)v0; (void)x1; (void)y1;
    (void)u1; (void)v1; (void)x2; (void)y2; (void)u2; (void)v2;
    (void)clut_x; (void)clut_y; (void)texpage;
    dispatch_fixture.primitive_calls[MOCK_TEXTURED_TRIANGLE]++;
}

static void backend_draw_shaded_textured_triangle(
        int x0, int y0, int u0, int v0, uint32_t c0,
        int x1, int y1, int u1, int v1, uint32_t c1,
        int x2, int y2, int u2, int v2, uint32_t c2,
        uint16_t clut_x, uint16_t clut_y, uint16_t texpage, int raw_texture) {
    (void)x0; (void)y0; (void)u0; (void)v0; (void)c0;
    (void)x1; (void)y1; (void)u1; (void)v1; (void)c1;
    (void)x2; (void)y2; (void)u2; (void)v2; (void)c2;
    (void)clut_x; (void)clut_y; (void)texpage; (void)raw_texture;
    dispatch_fixture.primitive_calls[MOCK_SHADED_TEXTURED_TRIANGLE]++;
}

static void backend_draw_flat_rect(int x, int y, int w, int h, uint16_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
    dispatch_fixture.primitive_calls[MOCK_FLAT_RECT]++;
}

static void backend_draw_textured_rect(int x, int y, int w, int h, int u, int v,
                                       uint16_t clut_x, uint16_t clut_y,
                                       uint16_t texpage) {
    (void)x; (void)y; (void)w; (void)h; (void)u; (void)v;
    (void)clut_x; (void)clut_y; (void)texpage;
    dispatch_fixture.primitive_calls[MOCK_TEXTURED_RECT]++;
}

static void backend_draw_textured_rect_scaled(
        int x, int y, int w, int h, int u0, int v0, int u1, int v1,
        uint16_t clut_x, uint16_t clut_y, uint16_t texpage) {
    (void)x; (void)y; (void)w; (void)h; (void)u0; (void)v0;
    (void)u1; (void)v1; (void)clut_x; (void)clut_y; (void)texpage;
    dispatch_fixture.primitive_calls[MOCK_TEXTURED_RECT_SCALED]++;
}

static void backend_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)color;
    dispatch_fixture.primitive_calls[MOCK_LINE]++;
}

static void backend_draw_shaded_line(int x0, int y0, uint16_t c0,
                                     int x1, int y1, uint16_t c1) {
    (void)x0; (void)y0; (void)c0; (void)x1; (void)y1; (void)c1;
    dispatch_fixture.primitive_calls[MOCK_SHADED_LINE]++;
}

static void backend_vram_write(int x, int y, uint16_t pixel) {
    dispatch_fixture.vram_write_calls++;
    dispatch_fixture.vram_x = x;
    dispatch_fixture.vram_y = y;
    dispatch_fixture.vram_pixel = pixel;
}

static uint16_t backend_vram_read(int x, int y) {
    dispatch_fixture.vram_read_calls++;
    dispatch_fixture.vram_x = x;
    dispatch_fixture.vram_y = y;
    return UINT16_C(0x5aa5);
}

static void backend_vram_transfer_in(int x, int y, int w, int h,
                                     const uint16_t *data) {
    (void)x; (void)y; (void)w; (void)h;
    dispatch_fixture.transfer_in_calls++;
    dispatch_fixture.transfer_in = data;
}

static void backend_vram_transfer_out(int x, int y, int w, int h,
                                      uint16_t *data) {
    (void)x; (void)y; (void)w; (void)h;
    dispatch_fixture.transfer_out_calls++;
    dispatch_fixture.transfer_out = data;
}

static void backend_set_draw_area(int x1, int y1, int x2, int y2) {
    dispatch_fixture.draw_area_calls++;
    dispatch_fixture.draw_area[0] = x1;
    dispatch_fixture.draw_area[1] = y1;
    dispatch_fixture.draw_area[2] = x2;
    dispatch_fixture.draw_area[3] = y2;
}

static void backend_set_draw_offset(int x, int y) {
    dispatch_fixture.draw_offset_calls++;
    dispatch_fixture.draw_offset[0] = x;
    dispatch_fixture.draw_offset[1] = y;
}

static const GpuRenderBackend BACKEND = {
    .name = "transaction-test",
    .set_scale = backend_set_scale,
    .set_texture_filter = backend_set_texture_filter,
    .set_semi_transparency = backend_set_semi_transparency,
    .set_mask_bits = backend_set_mask_bits,
    .set_texture_window = backend_set_texture_window,
    .set_color_modulation = backend_set_color_modulation,
    .fill_rect = backend_fill_rect,
    .copy_rect = backend_copy_rect,
    .draw_flat_triangle = backend_draw_flat_triangle,
    .draw_gouraud_triangle = backend_draw_gouraud_triangle,
    .draw_textured_triangle = backend_draw_textured_triangle,
    .draw_shaded_textured_triangle = backend_draw_shaded_textured_triangle,
    .draw_flat_rect = backend_draw_flat_rect,
    .draw_textured_rect = backend_draw_textured_rect,
    .draw_textured_rect_scaled = backend_draw_textured_rect_scaled,
    .draw_line = backend_draw_line,
    .draw_shaded_line = backend_draw_shaded_line,
    .vram_write = backend_vram_write,
    .vram_read = backend_vram_read,
    .vram_transfer_in = backend_vram_transfer_in,
    .vram_transfer_out = backend_vram_transfer_out,
    .set_draw_area = backend_set_draw_area,
    .set_draw_offset = backend_set_draw_offset,
    .transaction_begin = backend_begin,
    .ordering_barrier = backend_barrier,
    .draw_semantic = backend_draw,
    .commit_validate = backend_commit,
    .rollback = backend_rollback,
};

static GpuRenderTransactionId state_id(uint64_t scene, uint64_t sequence) {
    GpuRenderTransactionId id = { scene, sequence };

    return id;
}

static GpuRenderSemantic semantic_fixture(void) {
    GpuRenderSemantic semantic = { 0 };
    size_t triangle;
    size_t vertex;

    semantic.material.tpage = UINT16_C(0x0153);
    semantic.material.texture_page_x = 3u;
    semantic.material.texture_page_y = 1u;
    semantic.material.clut_x = 1008u;
    semantic.material.clut_y = 511u;
    semantic.material.draw_area_left = 5u;
    semantic.material.draw_area_top = 6u;
    semantic.material.draw_area_right = 900u;
    semantic.material.draw_area_bottom = 400u;
    semantic.material.draw_offset_x = -17;
    semantic.material.draw_offset_y = 23;
    semantic.material.texture_depth = GPU_RENDER_TEXTURE_15_BIT;
    semantic.material.texture_window_mask_x = 1u;
    semantic.material.texture_window_mask_y = 2u;
    semantic.material.texture_window_offset_x = 3u;
    semantic.material.texture_window_offset_y = 4u;
    semantic.material.shading = GPU_RENDER_SHADING_GOURAUD;
    semantic.material.textured = 1u;
    semantic.material.raw_texture = 1u;
    semantic.material.semi_transparent = 1u;
    semantic.material.blend_mode = GPU_RENDER_BLEND_SUBTRACT;
    semantic.material.dither = 1u;
    semantic.material.mask_set = 1u;
    semantic.material.mask_check = 1u;
    semantic.triangle_count = GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY;
    for (triangle = 0u; triangle < GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY;
         ++triangle) {
        semantic.triangles[triangle].split_index = (uint8_t)triangle;
        semantic.triangles[triangle].split_count =
            GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY;
        for (vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *out =
                &semantic.triangles[triangle].vertices[vertex];
            const int32_t seed = (int32_t)(triangle * 3u + vertex + 1u);

            out->x = seed * INT32_C(-65536);
            out->y = seed * INT32_C(32768);
            out->u = seed * INT32_C(-4096);
            out->v = seed * INT32_C(8192);
            out->r = (uint8_t)(10 + seed);
            out->g = (uint8_t)(40 + seed);
            out->b = (uint8_t)(70 + seed);
        }
    }
    return semantic;
}

static GpuRenderPresent present_fixture(void) {
    GpuRenderPresent present = { 0 };

    present.path = GPU_RENDER_PRESENT_WIDE;
    present.display_x = -9;
    present.display_y = 17;
    present.display_width = 640;
    present.display_height = 240;
    present.surface_width = 2560u;
    present.surface_height = 960u;
    present.wide_base_x = 320;
    present.scale = 4u;
    present.linear_filter = 1u;
    present.force_4_3 = 1u;
    present.reserved[0] = 0x5au;
    present.reserved[1] = 0xa5u;
    return present;
}

static void reset_fixture(void) {
    memset(&fixture, 0, sizeof(fixture));
    memset(&dispatch_fixture, 0, sizeof(dispatch_fixture));
    fixture.begin_status = GPU_RENDER_TRANSACTION_OK;
    fixture.barrier_status = GPU_RENDER_TRANSACTION_OK;
    fixture.draw_status = GPU_RENDER_TRANSACTION_OK;
    fixture.commit_status = GPU_RENDER_TRANSACTION_READY;
    fixture.rollback_status = GPU_RENDER_TRANSACTION_OK;
    gr_test_inject_backend(&BACKEND);
}

static void dispatch_all_primitives(void) {
    gr_fill_rect(1, 2, 3, 4, UINT16_C(0x1111));
    gr_copy_rect(5, 6, 7, 8, 9, 10);
    gr_draw_flat_triangle(11, 12, 13, 14, 15, 16, UINT16_C(0x2222));
    gr_draw_gouraud_triangle(17, 18, UINT16_C(0x3333),
                             19, 20, UINT16_C(0x4444),
                             21, 22, UINT16_C(0x5555));
    gr_draw_textured_triangle(23, 24, 25, 26,
                              27, 28, 29, 30,
                              31, 32, 33, 34,
                              48u, 7u, UINT16_C(0x0102));
    gr_draw_shaded_textured_triangle(
        35, 36, 37, 38, UINT32_C(0x00112233),
        39, 40, 41, 42, UINT32_C(0x00445566),
        43, 44, 45, 46, UINT32_C(0x00778899),
        64u, 8u, UINT16_C(0x0153), 1);
    gr_draw_flat_rect(47, 48, 49, 50, UINT16_C(0x6666));
    gr_draw_textured_rect(51, 52, 53, 54, 55, 56,
                          80u, 9u, UINT16_C(0x0102));
    gr_draw_textured_rect_scaled(57, 58, 59, 60, 61, 62, 63, 64,
                                 96u, 10u, UINT16_C(0x0153));
    gr_draw_line(65, 66, 67, 68, UINT16_C(0x7777));
    gr_draw_shaded_line(69, 70, UINT16_C(0x8888),
                        71, 72, UINT16_C(0x9999));
}

static int primitive_counts_equal(unsigned int expected) {
    size_t index;

    for (index = 0u; index < MOCK_PRIMITIVE_COUNT; ++index) {
        if (dispatch_fixture.primitive_calls[index] != expected) return 0;
    }
    return 1;
}

static int test_draw_suppression_covers_every_primitive_and_nests(void) {
    reset_fixture();
    CHECK(!gr_draw_suppression_active());
    CHECK(gr_draw_suppression_begin() == GPU_RENDER_DRAW_SUPPRESSION_OK);
    CHECK(gr_draw_suppression_active());
    CHECK(gr_draw_suppression_begin() == GPU_RENDER_DRAW_SUPPRESSION_OK);
    dispatch_all_primitives();
    CHECK(primitive_counts_equal(0u));
    CHECK(gr_draw_suppression_end() == GPU_RENDER_DRAW_SUPPRESSION_OK);
    CHECK(gr_draw_suppression_active());
    dispatch_all_primitives();
    CHECK(primitive_counts_equal(0u));
    CHECK(gr_draw_suppression_end() == GPU_RENDER_DRAW_SUPPRESSION_OK);
    CHECK(!gr_draw_suppression_active());
    dispatch_all_primitives();
    CHECK(primitive_counts_equal(1u));
    return 1;
}

static int test_state_and_vram_calls_continue_while_suppressed(void) {
    uint16_t transfer_in[] = { UINT16_C(0x1234), UINT16_C(0x5678) };
    uint16_t transfer_out[2] = { 0 };

    reset_fixture();
    CHECK(gr_draw_suppression_begin() == GPU_RENDER_DRAW_SUPPRESSION_OK);
    gr_set_scale(3);
    gr_set_texture_filter(1);
    gr_set_semi_transparency(1, 2);
    gr_set_mask_bits(1, 0);
    gr_set_texture_window(UINT32_C(0x00abcdef));
    gr_set_color_modulation(10, 20, 30, 1);
    gr_set_draw_area(4, 5, 600, 400);
    gr_set_draw_offset(-17, 23);
    CHECK(dispatch_fixture.scale_calls == 1u && dispatch_fixture.scale == 3);
    CHECK(dispatch_fixture.filter_calls == 1u && dispatch_fixture.filter == 1);
    CHECK(dispatch_fixture.semi_calls == 1u &&
          dispatch_fixture.semi_enabled == 1 && dispatch_fixture.semi_mode == 2);
    CHECK(dispatch_fixture.mask_calls == 1u &&
          dispatch_fixture.mask_set == 1 && dispatch_fixture.mask_check == 0);
    CHECK(dispatch_fixture.texture_window_calls == 1u &&
          dispatch_fixture.texture_window == UINT32_C(0x00abcdef));
    CHECK(dispatch_fixture.modulation_calls == 1u &&
          dispatch_fixture.modulation_r == 10 &&
          dispatch_fixture.modulation_g == 20 &&
          dispatch_fixture.modulation_b == 30 &&
          dispatch_fixture.modulation_raw == 1);
    CHECK(dispatch_fixture.draw_area_calls == 1u &&
          dispatch_fixture.draw_area[0] == 4 &&
          dispatch_fixture.draw_area[1] == 5 &&
          dispatch_fixture.draw_area[2] == 600 &&
          dispatch_fixture.draw_area[3] == 400);
    CHECK(dispatch_fixture.draw_offset_calls == 1u &&
          dispatch_fixture.draw_offset[0] == -17 &&
          dispatch_fixture.draw_offset[1] == 23);

    gr_vram_write(73, 74, UINT16_C(0xa55a));
    CHECK(dispatch_fixture.vram_write_calls == 1u &&
          dispatch_fixture.vram_x == 73 && dispatch_fixture.vram_y == 74 &&
          dispatch_fixture.vram_pixel == UINT16_C(0xa55a));
    CHECK(gr_vram_read(75, 76) == UINT16_C(0x5aa5));
    CHECK(dispatch_fixture.vram_read_calls == 1u &&
          dispatch_fixture.vram_x == 75 && dispatch_fixture.vram_y == 76);
    gr_vram_transfer_in(77, 78, 2, 1, transfer_in);
    gr_vram_transfer_out(79, 80, 2, 1, transfer_out);
    CHECK(dispatch_fixture.transfer_in_calls == 1u &&
          dispatch_fixture.transfer_in == transfer_in);
    CHECK(dispatch_fixture.transfer_out_calls == 1u &&
          dispatch_fixture.transfer_out == transfer_out);
    CHECK(gr_draw_suppression_end() == GPU_RENDER_DRAW_SUPPRESSION_OK);
    return 1;
}

static int test_suppression_underflow_and_overflow_poison(void) {
    unsigned int depth;

    reset_fixture();
    CHECK(gr_draw_suppression_end() == GPU_RENDER_DRAW_SUPPRESSION_UNDERFLOW);
    CHECK(gr_draw_suppression_active());
    CHECK(gr_draw_suppression_begin() == GPU_RENDER_DRAW_SUPPRESSION_POISONED);
    CHECK(gr_draw_suppression_end() == GPU_RENDER_DRAW_SUPPRESSION_POISONED);
    dispatch_all_primitives();
    CHECK(primitive_counts_equal(0u));

    reset_fixture();
    for (depth = 0u; depth < GPU_RENDER_DRAW_SUPPRESSION_MAX_DEPTH; ++depth)
        CHECK(gr_draw_suppression_begin() == GPU_RENDER_DRAW_SUPPRESSION_OK);
    CHECK(gr_draw_suppression_begin() == GPU_RENDER_DRAW_SUPPRESSION_OVERFLOW);
    CHECK(gr_draw_suppression_active());
    CHECK(gr_draw_suppression_end() == GPU_RENDER_DRAW_SUPPRESSION_POISONED);
    dispatch_all_primitives();
    CHECK(primitive_counts_equal(0u));
    return 1;
}

static int test_suppression_reset_and_open_transaction_independence(void) {
    const GpuRenderTransactionId id = state_id(21u, 22u);
    GpuRenderSemantic semantic = semantic_fixture();

    reset_fixture();
    CHECK(gr_draw_suppression_begin() == GPU_RENDER_DRAW_SUPPRESSION_OK);
    CHECK(gr_transaction_begin(id, 23u) == GPU_RENDER_TRANSACTION_OK);
    gr_set_backend(GR_BACKEND_OPENGL);
    CHECK(gr_backend() == GR_BACKEND_SOFTWARE);
    CHECK(gr_draw_suppression_active());
    dispatch_all_primitives();
    CHECK(primitive_counts_equal(0u));
    CHECK(gr_ordering_barrier(id) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_draw_semantic(id, &semantic) == GPU_RENDER_TRANSACTION_OK);
    CHECK(fixture.draw_calls == 1u);
    CHECK(gr_ordering_barrier(id) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_draw_suppression_end() == GPU_RENDER_DRAW_SUPPRESSION_OK);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);

    CHECK(gr_draw_suppression_begin() == GPU_RENDER_DRAW_SUPPRESSION_OK);
    CHECK(gr_draw_suppression_active());
    gr_test_inject_backend(&BACKEND);
    CHECK(!gr_draw_suppression_active());
    dispatch_all_primitives();
    CHECK(primitive_counts_equal(1u));
    return 1;
}

static int test_unsupported_callbacks_fail_closed(void) {
    GpuRenderBackend incomplete = BACKEND;
    const GpuRenderTransactionId id = state_id(1u, 2u);

    reset_fixture();
    gr_test_inject_backend(NULL);
    CHECK(gr_transaction_begin(id, 3u) == GPU_RENDER_TRANSACTION_UNSUPPORTED);

    reset_fixture();
    incomplete.transaction_begin = NULL;
    gr_test_inject_backend(&incomplete);
    CHECK(gr_transaction_begin(id, 3u) == GPU_RENDER_TRANSACTION_UNSUPPORTED);
    CHECK(fixture.begin_calls == 0u);

    reset_fixture();
    incomplete = BACKEND;
    incomplete.ordering_barrier = NULL;
    gr_test_inject_backend(&incomplete);
    CHECK(gr_transaction_begin(id, 3u) == GPU_RENDER_TRANSACTION_UNSUPPORTED);
    CHECK(fixture.begin_calls == 0u);

    reset_fixture();
    incomplete = BACKEND;
    incomplete.draw_semantic = NULL;
    gr_test_inject_backend(&incomplete);
    CHECK(gr_transaction_begin(id, 3u) == GPU_RENDER_TRANSACTION_UNSUPPORTED);
    CHECK(fixture.begin_calls == 0u);

    reset_fixture();
    incomplete = BACKEND;
    incomplete.commit_validate = NULL;
    gr_test_inject_backend(&incomplete);
    CHECK(gr_transaction_begin(id, 3u) == GPU_RENDER_TRANSACTION_UNSUPPORTED);
    CHECK(fixture.begin_calls == 0u);

    reset_fixture();
    incomplete = BACKEND;
    incomplete.rollback = NULL;
    gr_test_inject_backend(&incomplete);
    CHECK(gr_transaction_begin(id, 3u) == GPU_RENDER_TRANSACTION_UNSUPPORTED);
    CHECK(fixture.begin_calls == 0u);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_INVALID_TRANSITION);
    return 1;
}

static int test_lifecycle_and_ordering(void) {
    const GpuRenderTransactionId id = state_id(4u, 5u);
    GpuRenderSemantic semantic = semantic_fixture();
    GpuRenderPresent present = present_fixture();

    reset_fixture();
    CHECK(gr_ordering_barrier(id) == GPU_RENDER_TRANSACTION_INVALID_TRANSITION);
    CHECK(gr_draw_semantic(id, &semantic) ==
          GPU_RENDER_TRANSACTION_INVALID_TRANSITION);
    CHECK(gr_commit_validate(id, 7u, &present) ==
          GPU_RENDER_TRANSACTION_INVALID_TRANSITION);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_INVALID_TRANSITION);

    reset_fixture();
    CHECK(gr_transaction_begin(id, 6u) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_transaction_begin(id, 6u) ==
          GPU_RENDER_TRANSACTION_INVALID_TRANSITION);
    CHECK(gr_ordering_barrier(id) ==
          GPU_RENDER_TRANSACTION_INVALID_TRANSITION);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);

    reset_fixture();
    CHECK(gr_transaction_begin(id, 6u) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_draw_semantic(id, &semantic) ==
          GPU_RENDER_TRANSACTION_ORDER_REJECTED);
    CHECK(fixture.draw_calls == 0u);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);

    reset_fixture();
    CHECK(gr_transaction_begin(id, 6u) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_ordering_barrier(id) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_draw_semantic(id, &semantic) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_commit_validate(id, 7u, &present) ==
          GPU_RENDER_TRANSACTION_ORDER_REJECTED);
    CHECK(fixture.commit_calls == 0u);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);

    reset_fixture();
    CHECK(gr_transaction_begin(id, 6u) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_ordering_barrier(id) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_draw_semantic(id, &semantic) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_ordering_barrier(id) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_commit_validate(id, 7u, &present) == GPU_RENDER_TRANSACTION_READY);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_INVALID_TRANSITION);
    return 1;
}

static int test_backend_statuses_and_argument_rejection(void) {
    const GpuRenderTransactionId id = state_id(8u, 9u);
    GpuRenderSemantic semantic = semantic_fixture();
    GpuRenderPresent present = present_fixture();

    reset_fixture();
    fixture.begin_status = GPU_RENDER_TRANSACTION_STATE_REJECTED;
    CHECK(gr_transaction_begin(id, 10u) ==
          GPU_RENDER_TRANSACTION_STATE_REJECTED);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_INVALID_TRANSITION);

    reset_fixture();
    CHECK(gr_transaction_begin(id, 10u) == GPU_RENDER_TRANSACTION_OK);
    fixture.barrier_status = GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    CHECK(gr_ordering_barrier(id) == GPU_RENDER_TRANSACTION_CONTEXT_LOST);
    CHECK(gr_draw_semantic(id, &semantic) ==
          GPU_RENDER_TRANSACTION_INVALID_TRANSITION);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);

    reset_fixture();
    CHECK(gr_transaction_begin(id, 10u) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_ordering_barrier(id) == GPU_RENDER_TRANSACTION_OK);
    fixture.draw_status = GPU_RENDER_TRANSACTION_VALIDATION_FAILED;
    CHECK(gr_draw_semantic(id, &semantic) ==
          GPU_RENDER_TRANSACTION_VALIDATION_FAILED);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);

    reset_fixture();
    CHECK(gr_transaction_begin(id, 10u) == GPU_RENDER_TRANSACTION_OK);
    fixture.commit_status = GPU_RENDER_TRANSACTION_STATE_REJECTED;
    CHECK(gr_commit_validate(id, 11u, &present) ==
          GPU_RENDER_TRANSACTION_STATE_REJECTED);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);

    reset_fixture();
    CHECK(gr_transaction_begin(id, 10u) == GPU_RENDER_TRANSACTION_OK);
    fixture.commit_status = GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    CHECK(gr_commit_validate(id, 11u, &present) ==
          GPU_RENDER_TRANSACTION_BACKEND_ERROR);
    fixture.rollback_status = GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_CONTEXT_LOST);
    fixture.rollback_status = GPU_RENDER_TRANSACTION_OK;
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);

    reset_fixture();
    CHECK(gr_transaction_begin(id, 10u) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_draw_semantic(id, NULL) ==
          GPU_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);

    reset_fixture();
    CHECK(gr_transaction_begin(id, 10u) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_commit_validate(id, 11u, NULL) ==
          GPU_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);

    reset_fixture();
    CHECK(gr_transaction_begin(id, 10u) == GPU_RENDER_TRANSACTION_OK);
    fixture.commit_status = GPU_RENDER_TRANSACTION_OK;
    CHECK(gr_commit_validate(id, 11u, &present) ==
          GPU_RENDER_TRANSACTION_BACKEND_ERROR);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);
    return 1;
}

static int test_cross_state_calls_are_rejected_before_dispatch(void) {
    const GpuRenderTransactionId first = state_id(12u, 13u);
    const GpuRenderTransactionId other = state_id(12u, 14u);
    GpuRenderSemantic semantic = semantic_fixture();
    GpuRenderPresent present = present_fixture();

    reset_fixture();
    CHECK(gr_transaction_begin(first, 15u) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_ordering_barrier(other) == GPU_RENDER_TRANSACTION_STATE_REJECTED);
    CHECK(fixture.barrier_calls == 0u);
    CHECK(gr_rollback(other) == GPU_RENDER_TRANSACTION_STATE_REJECTED);
    CHECK(fixture.rollback_calls == 0u);
    CHECK(gr_rollback(first) == GPU_RENDER_TRANSACTION_OK);

    reset_fixture();
    CHECK(gr_transaction_begin(first, 15u) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_ordering_barrier(first) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_draw_semantic(other, &semantic) ==
          GPU_RENDER_TRANSACTION_STATE_REJECTED);
    CHECK(fixture.draw_calls == 0u);
    CHECK(gr_rollback(first) == GPU_RENDER_TRANSACTION_OK);

    reset_fixture();
    CHECK(gr_transaction_begin(first, 15u) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_commit_validate(other, 16u, &present) ==
          GPU_RENDER_TRANSACTION_STATE_REJECTED);
    CHECK(fixture.commit_calls == 0u);
    CHECK(gr_rollback(first) == GPU_RENDER_TRANSACTION_OK);
    return 1;
}

static int test_exact_parameter_forwarding(void) {
    const GpuRenderTransactionId id =
        state_id(UINT64_C(0x1020304050607080),
                 UINT64_C(0x8877665544332211));
    const uint64_t begin_serial = UINT64_C(0xfedcba9876543210);
    const uint64_t commit_serial = UINT64_C(0x0123456789abcdef);
    GpuRenderSemantic semantic = semantic_fixture();
    GpuRenderPresent present = present_fixture();

    reset_fixture();
    CHECK(gr_transaction_begin(id, begin_serial) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_ordering_barrier(id) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_draw_semantic(id, &semantic) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_ordering_barrier(id) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_commit_validate(id, commit_serial, &present) ==
          GPU_RENDER_TRANSACTION_READY);
    CHECK(fixture.begin_calls == 1u);
    CHECK(fixture.barrier_calls == 2u);
    CHECK(fixture.draw_calls == 1u);
    CHECK(fixture.commit_calls == 1u);
    CHECK(fixture.rollback_calls == 0u);
    CHECK(fixture.begin_id.scene_epoch == id.scene_epoch);
    CHECK(fixture.begin_id.state_sequence == id.state_sequence);
    CHECK(fixture.barrier_id.scene_epoch == id.scene_epoch);
    CHECK(fixture.barrier_id.state_sequence == id.state_sequence);
    CHECK(fixture.draw_id.scene_epoch == id.scene_epoch);
    CHECK(fixture.draw_id.state_sequence == id.state_sequence);
    CHECK(fixture.commit_id.scene_epoch == id.scene_epoch);
    CHECK(fixture.commit_id.state_sequence == id.state_sequence);
    CHECK(fixture.begin_serial == begin_serial);
    CHECK(fixture.commit_serial == commit_serial);
    CHECK(fixture.semantic == &semantic);
    CHECK(fixture.present == &present);
    CHECK(fixture.call_count == 5u);
    CHECK(memcmp(fixture.call_order, "BODOC", 5u) == 0);

    reset_fixture();
    CHECK(gr_transaction_begin(id, begin_serial) == GPU_RENDER_TRANSACTION_OK);
    CHECK(gr_rollback(id) == GPU_RENDER_TRANSACTION_OK);
    CHECK(fixture.rollback_id.scene_epoch == id.scene_epoch);
    CHECK(fixture.rollback_id.state_sequence == id.state_sequence);
    CHECK(fixture.call_count == 2u);
    CHECK(memcmp(fixture.call_order, "BR", 2u) == 0);
    return 1;
}

int main(void) {
    if (!test_draw_suppression_covers_every_primitive_and_nests()) return 1;
    if (!test_state_and_vram_calls_continue_while_suppressed()) return 1;
    if (!test_suppression_underflow_and_overflow_poison()) return 1;
    if (!test_suppression_reset_and_open_transaction_independence()) return 1;
    if (!test_unsupported_callbacks_fail_closed()) return 1;
    if (!test_lifecycle_and_ordering()) return 1;
    if (!test_backend_statuses_and_argument_rejection()) return 1;
    if (!test_cross_state_calls_are_rejected_before_dispatch()) return 1;
    if (!test_exact_parameter_forwarding()) return 1;
    return 0;
}
