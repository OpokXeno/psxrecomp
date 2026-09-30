/* Exercise the real VIEW transfer/recognition code without a GL context.
 * Unreferenced renderer entry points are removed by section GC. */
#include "../src/gpu_gl_renderer.c"
#include <assert.h>

static uint16_t words[VRAM_W * VRAM_H];
static uint32_t pixels[VRAM_W * VRAM_H];
static uint32_t copied[320 * 240];
static uint16_t uploaded[324 * 240];

static void check_view(uint32_t width, uint32_t offset) {
    GlNativeViewState views = {.width = width, .offset = offset, .active = -1};
    GlNativeCompileAudit audit = {0};
    XgRenderNativeOperation copy = {.kind = XG_RENDER_NATIVE_OPERATION_COPY,
        .src_x = 0, .src_y = 0, .dst_x = 704, .dst_y = 256,
        .width = 320, .height = 224};
    int wave_source;
    uint64_t wave_destinations;
    const int frame = native_view_target(&views, 0, 0, 320, 224);
    assert(frame >= 0 && native_view_private(&views, (uint32_t)frame, pixels));
    /* A crop must preserve the full VIEW row, including its reveal margin. */
    for (uint32_t y = 0; y < 224; ++y)
        for (uint32_t x = 0; x < width; ++x)
            views.targets[frame].pixels[y * width + x] = 0x120000u + y * width + x;
    assert(native_view_transfer(&views, &copy, NULL, pixels, copied,
        &wave_source, &wave_destinations, &audit));
    int snapshot = native_view_cover(&views, views.count, 704, 256, 320, 224);
    assert(snapshot >= 0 && views.targets[snapshot].transition_snapshot);
    assert(memcmp(views.targets[frame].pixels, views.targets[snapshot].pixels,
        width * 224 * sizeof(uint32_t)) == 0);

    XgSemanticDrawRecord draw = {.topology = GPU_RENDER_SEMANTIC_TRIANGLES};
    draw.primitive.triangle_count = 1;
    draw.primitive.material.textured = true;
    draw.primitive.material.texture_depth = XG_RENDER_IR_TEXTURE_15_BIT;
    draw.primitive.material.texture_page_x = 15;
    draw.primitive.material.texture_page_y = 1;
    draw.primitive.triangles[0].vertices[1].u = 63 * 65536;
    draw.primitive.triangles[0].vertices[2].v = 223 * 65536;
    assert(native_transition_texture(&views, &draw) == snapshot);
    /* SDK scissors can cover all VRAM while the actual draw stays on screen. */
    draw.primitive.material.draw_area_right = VRAM_W - 1;
    draw.primitive.material.draw_area_bottom = VRAM_H - 1;
    draw.primitive.triangles[0].vertices[1].x = 320 * 65536;
    draw.primitive.triangles[0].vertices[2].y = 224 * 65536;
    assert(!native_transition_overwritten(&views.targets[snapshot], &draw));
    draw.primitive.material.draw_offset_x = 704;
    draw.primitive.material.draw_offset_y = 256;
    assert(native_transition_overwritten(&views.targets[snapshot], &draw));
    draw.primitive.material.draw_offset_x = 0;
    draw.primitive.material.draw_offset_y = 0;
    draw.primitive.material.texture_depth = XG_RENDER_IR_TEXTURE_8_BIT;
    assert(native_transition_texture(&views, &draw) < 0);
    draw.primitive.material.texture_depth = XG_RENDER_IR_TEXTURE_15_BIT;
    draw.primitive.material.texture_window_mask_x = 1;
    assert(native_transition_texture(&views, &draw) < 0);
    draw.primitive.material.texture_window_mask_x = 0;

    /* World-map fades capture 240 rows from a 224-row framebuffer, including
     * padding. Keep the wider alias selectable as well as the old crop. */
    copy.height = 240;
    assert(native_view_transfer(&views, &copy, NULL, pixels, copied,
        &wave_source, &wave_destinations, &audit));
    draw.primitive.triangles[0].vertices[2].v = 239 * 65536;
    snapshot = native_transition_texture(&views, &draw);
    assert(snapshot >= 0 && views.targets[snapshot].height == 240);

    for (uint32_t i = 0; i < VRAM_W * VRAM_H; ++i) words[i] = (uint16_t)(i & 0x7fff);
    GlNativeResourceRecord upload = {0};
    upload.view.bytes = uploaded;
    upload.view.descriptor.row_pitch = 324 * sizeof(uint16_t);
    XgRenderNativeOperation operation = {.kind = XG_RENDER_NATIVE_OPERATION_UPLOAD,
        .dst_x = 768, .dst_y = 256, .width = 64, .height = 224};
    XgRenderNativeOperation view_operation = operation;
    for (uint32_t y = 0; y < operation.height; ++y)
        for (uint32_t x = 0; x < operation.width; ++x)
            uploaded[y * 324 + x] = words[(256 + y) * VRAM_W + 768 + x] ^ 0x8000;
    assert(native_transition_upload(&views, &operation, &upload, words, &view_operation) == 1);
    assert(view_operation.kind == XG_RENDER_NATIVE_OPERATION_UPLOAD);
    uploaded[100 * 324 + 27] ^= 1;
    /* Actual GPUREAD words can differ from the Native canonical raster. A
     * receipt proves their STP-only return without relaxing RGB equality. */
    views.readback_capture = 1;
    views.readback_x = operation.dst_x; views.readback_y = operation.dst_y;
    views.readback_width = operation.width; views.readback_height = operation.height;
    uint64_t receipt = UINT64_C(1469598103934665603);
    for (uint32_t y = 0; y < operation.height; ++y)
        for (uint32_t x = 0; x < operation.width; ++x) {
            const uint16_t word = uploaded[y * 324 + x] & 0x7fff;
            receipt = (receipt ^ (word & 255)) * UINT64_C(1099511628211);
            receipt = (receipt ^ (word >> 8)) * UINT64_C(1099511628211);
        }
    views.readback_rgb_digest = receipt;
    assert(native_transition_upload(&views, &operation, &upload, words, &view_operation) == 1);
    views.readback_rgb_digest = receipt;
    uploaded[0] ^= 2;
    assert(native_transition_upload(&views, &operation, &upload, words, &view_operation) == 0);
    uploaded[0] ^= 2;
    views.readback_rgb_digest = 0;
    assert(native_transition_upload(&views, &operation, &upload, words, &view_operation) == 0);
    uploaded[100 * 324 + 27] ^= 1;
    operation.mask_check = 1;
    assert(native_transition_upload(&views, &operation, &upload, words, &view_operation) == 0);

    /* Battle's CPU round trip requires RGB equality with a declared framebuffer. */
    operation.mask_check = 0;
    operation.dst_x = 704;
    operation.width = 320;
    for (uint32_t y = 0; y < operation.height; ++y)
        for (uint32_t x = 0; x < operation.width; ++x)
            uploaded[y * 324 + x] = words[y * VRAM_W + x] | 0x8000;
    assert(native_transition_upload(&views, &operation, &upload, words, &view_operation) == 2);
    assert(operation.kind == XG_RENDER_NATIVE_OPERATION_UPLOAD);
    assert(view_operation.kind == XG_RENDER_NATIVE_OPERATION_COPY);
    assert(view_operation.src_x == 0 && view_operation.src_y == 0);
    uploaded[223 * 324 + 319] ^= 1;
    assert(native_transition_upload(&views, &operation, &upload, words, &view_operation) == 0);

    /* An unrelated copy to the same address retires the capture certificate. */
    copy.src_x = 384;
    assert(native_view_transfer(&views, &copy, NULL, pixels, copied,
        &wave_source, &wave_destinations, &audit));
    assert(native_transition_texture(&views, &draw) < 0);
    native_views_discard(&views, 1);
    assert(native_transition_upload(&views, &operation, &upload, words, &view_operation) == 0);
}

/* Field -> World Map uses the short display's 216-row MoveImage, followed
 * by three 128/128/64-pixel FT4 strips and a full-width fade mask. */
static void check_world_view(uint32_t width, uint32_t offset) {
    GlNativeViewState views = {.width = width, .offset = offset, .active = -1};
    GlNativeCompileAudit audit = {0};
    const int frame = native_view_target(&views, 0, 0, 320, 216);
    assert(frame >= 0 && native_view_private(&views, (uint32_t)frame, pixels));
    for (uint32_t y = 0; y < 216; ++y)
        for (uint32_t x = 0; x < width; ++x)
            views.targets[frame].pixels[y * width + x] = 0x340000u + y * width + x;
    XgRenderNativeOperation copy = {.kind = XG_RENDER_NATIVE_OPERATION_COPY,
        .src_x = 0, .src_y = 0, .dst_x = 704, .dst_y = 256,
        .width = 320, .height = 216};
    int wave_source;
    uint64_t wave_destinations;
    assert(native_view_transfer(&views, &copy, NULL, pixels, copied,
        &wave_source, &wave_destinations, &audit));
    const int snapshot = native_view_cover(&views, views.count, 704, 256, 320, 216);
    assert(snapshot >= 0 && views.targets[snapshot].transition_snapshot == 1);
    assert(memcmp(views.targets[frame].pixels, views.targets[snapshot].pixels,
        width * 216 * sizeof(uint32_t)) == 0);
    XgSemanticDrawRecord draw = {.topology = GPU_RENDER_SEMANTIC_TRIANGLES};
    draw.primitive.triangle_count = 1;
    draw.primitive.material.textured = true;
    draw.primitive.material.texture_depth = XG_RENDER_IR_TEXTURE_15_BIT;
    draw.primitive.material.texture_page_y = 1;
    for (uint32_t strip = 0; strip < 3; ++strip) {
        draw.primitive.material.texture_page_x = 11 + strip * 2;
        draw.primitive.triangles[0].vertices[1].u = (strip == 2 ? 63 : 127) * 65536;
        draw.primitive.triangles[0].vertices[2].v = 215 * 65536;
        assert(native_transition_texture(&views, &draw) == snapshot);
        /* Retail vertices extend below the display, with V == Y. Scissor
         * clipping keeps all actual samples inside the 216 captured rows. */
        draw.primitive.material.draw_area_bottom = 215;
        draw.primitive.triangles[0].vertices[2].y = 239 * 65536;
        draw.primitive.triangles[0].vertices[2].v = 239 * 65536;
        assert(native_transition_texture(&views, &draw) == snapshot);
        draw.primitive.material.draw_offset_y = 216;
        draw.primitive.material.draw_area_top = 216;
        draw.primitive.material.draw_area_bottom = 431;
        assert(native_transition_texture(&views, &draw) == snapshot);
        draw.primitive.triangles[0].vertices[2].v -= 65536;
        assert(native_transition_texture(&views, &draw) < 0);
        draw.primitive.triangles[0].vertices[2].v += 65536;
        draw.primitive.material.draw_area_bottom = 455;
        assert(native_transition_texture(&views, &draw) < 0);
        draw.primitive.material.draw_offset_y = 0;
        draw.primitive.material.draw_area_top = 0;
        draw.primitive.material.draw_area_bottom = 215;
        draw.primitive.triangles[0].vertices[2].v = 215 * 65536;
    }
    /* A subsequent texture upload must still invalidate this shorter capture. */
    XgRenderNativeOperation fill = {.kind = XG_RENDER_NATIVE_OPERATION_FILL,
        .dst_x = 704, .dst_y = 256, .width = 320, .height = 216};
    assert(native_view_transfer(&views, &fill, NULL, pixels, NULL,
        &wave_source, &wave_destinations, &audit));
    assert(native_transition_texture(&views, &draw) < 0);
    native_views_discard(&views, 1);
}

int main(void) {
    check_view(320, 0);
    check_view(426, 53);
    check_world_view(320, 0);
    check_world_view(426, 53);
    puts("Native transition captures: PASS (4:3 and 16:9)");
    return 0;
}
