/* Source-owned GPU-only geometry must reach Native scanout at 1x as at 4x.
 * Word-texture reuse must preserve actual GL bytes across patches and resets.
 * Exercise the real collector/compiler and GPU with a hidden GL context; no
 * game executable, disc, BIOS or controller input is used. */
#include "../src/gpu_gl_renderer.c"

#undef NDEBUG
#include <assert.h>

/* Count actual driver transfers, including those made by the real FIFO owner. */
static uint64_t word_upload_calls, word_upload_bytes;
static GLint last_word_rect[4];
void __real_glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei,
    GLenum, GLenum, const void *);
void __wrap_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
        GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) {
    if (format == GL_RED_INTEGER && type == GL_UNSIGNED_SHORT) {
        ++word_upload_calls;
        word_upload_bytes += (uint64_t)width * height * sizeof(uint16_t);
        last_word_rect[0] = x; last_word_rect[1] = y;
        last_word_rect[2] = width; last_word_rect[3] = height;
    }
    __real_glTexSubImage2D(target, level, x, y, width, height, format, type, pixels);
}

/* The real presenter/telemetry entry points are not driven by this fixture. */
uint64_t s_frame_count;
void latency_ring_mark(LatencyStage stage) { (void)stage; }
int gpu_screen_lut_snapshot(const uint8_t **out) { *out = NULL; return 0; }

/* This fixture has no external texture pack or guest wave parameters. */
HdTexturePack *hd_texture_runtime_pack(void) { return NULL; }
uint32_t hd_texture_runtime_generation(void) { return 0u; }
int hd_texture_pack_acquire_decoded(HdTexturePack *pack, uint32_t texture_hash,
        uint32_t palette_hash, HdTexturePixels *out) {
    (void)pack; (void)texture_hash; (void)palette_hash; (void)out;
    return HD_TEXTURE_LOOKUP_NONE;
}
int hd_texture_pack_request_decode(HdTexturePack *pack, uint32_t texture_hash,
        uint32_t palette_hash) {
    (void)pack; (void)texture_hash; (void)palette_hash;
    return HD_TEXTURE_LOOKUP_NONE;
}
void hd_texture_pack_forget_decoded(HdTexturePack *pack, uint32_t texture_hash,
        uint32_t palette_hash) {
    (void)pack; (void)texture_hash; (void)palette_hash;
}
void hd_texture_pixels_release(HdTexturePixels *pixels) { (void)pixels; }
bool gpu_native_field_wave_columns(const int16_t observed[21], int first,
        uint32_t count, int16_t *out) {
    (void)observed; (void)first; (void)count; (void)out;
    return false;
}

static GpuRenderSemantic triangle(uint64_t command_id, uint8_t red, uint8_t green) {
    GpuRenderSemantic draw = {
        .submission_command_id = command_id,
        .topology = GPU_RENDER_SEMANTIC_TRIANGLES,
        .triangle_count = 1u,
        .screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_STRETCH,
        .material = {.draw_area_right = 319u, .draw_area_bottom = 223u},
    };
    const int x[3] = {16, 96, 16}, y[3] = {16, 16, 96};
    draw.triangles[0].split_count = 1u;
    for (unsigned i = 0u; i < 3u; ++i) {
        draw.triangles[0].vertices[i].x = x[i] * 65536;
        draw.triangles[0].vertices[i].y = y[i] * 65536;
        draw.triangles[0].vertices[i].r = red;
        draw.triangles[0].vertices[i].g = green;
    }
    return draw;
}

static void check_word_texture(const uint16_t *expected) {
    uint16_t *actual = malloc(sizeof(s_native_gpu_upload_words));
    assert(actual);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_native_gpu_words);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, actual);
    assert(glGetError() == GL_NO_ERROR);
    assert(memcmp(actual, expected, sizeof(s_native_gpu_upload_words)) == 0);
    free(actual);
}

static void check_word_patch(uint16_t *baseline, bool reset) {
    GlNativeGpuWork *work = calloc(1u, sizeof(*work));
    const size_t size = sizeof(s_native_gpu_upload_words);
    uint16_t patch[64];
    for (unsigned i = 0u; i < 64u; ++i) patch[i] = (reset ? 0x4321u : 0x1234u) + i;
    assert(work);
    work->scale = 1u;
    work->words = malloc(size);
    assert(work->words);
    memcpy(work->words, baseline, size);
    assert(native_gpu_data(work, baseline, size, &work->initial_words_offset));
    GlNativeGpuCommand *command = native_gpu_command(work, NATIVE_GPU_WORDS);
    assert(command);
    command->x = VRAM_W - 64; command->y = VRAM_H - 1;
    command->w = 64; command->h = 1;
    assert(native_gpu_data(work, patch, sizeof(patch), &command->data));
    if (reset) assert(native_gpu_command(work, NATIVE_GPU_WORDS_RESET));
    const uint64_t calls = word_upload_calls;
    const uint64_t bytes = word_upload_bytes;
    native_gpu_publish_prefix(work, 1);
    const uint64_t deadline = SDL_GetTicksNS() + UINT64_C(5000000000);
    while (work->state != NATIVE_GPU_READY && SDL_GetTicksNS() < deadline) {
        assert(native_gpu_service() >= 0);
        SDL_Delay(1u);
    }
    assert(work->state == NATIVE_GPU_READY && !work->failed);
    /* Unchanged initial words caused no full upload; the actual FIFO patch
     * (or its reset) still uploaded and updated exactly the last 64 words. */
    assert(word_upload_calls == calls + 1u);
    assert(word_upload_bytes == bytes + sizeof(patch));
    if (!reset) memcpy(baseline + VRAM_W * VRAM_H - 64u, patch, sizeof(patch));
    check_word_texture(baseline);
    assert(native_gpu_sync_initial_words(baseline) == 0);
    assert(word_upload_calls == calls + 1u);
    s_native_gpu_work = NULL;
    native_gpu_free(work);
}

static void check_word_upload_cache(void) {
    uint16_t *words = calloc(1u, sizeof(s_native_gpu_upload_words));
    assert(words && native_gpu_program_init());
    assert(!s_native_gpu_upload_words_valid);
    s_native_mip_count = 1u;
    s_native_mips[0].dirty = 0u;
    const uint64_t calls = word_upload_calls;
    assert(native_gpu_sync_initial_words(words) == 1);
    assert(word_upload_calls == calls + 1u);
    assert(s_native_mips[0].dirty);
    check_word_texture(words);
    s_native_mips[0].dirty = 0u;
    for (unsigned i = 0u; i < 4u; ++i) assert(native_gpu_sync_initial_words(words) == 0);
    assert(word_upload_calls == calls + 1u);
    assert(!s_native_mips[0].dirty);
    words[0] = 0x001fu; words[VRAM_W * 200u + 500u] = 0x83e0u;
    words[VRAM_W * VRAM_H - 1u] = 0x7c00u;
    assert(native_gpu_sync_initial_words(words) == 1);
    assert(word_upload_calls == calls + 2u);
    assert(s_native_mips[0].dirty);
    s_native_mip_count = 0u;
    check_word_texture(words);
    check_word_patch(words, false);
    check_word_patch(words, true);
    memset(words, 0, sizeof(s_native_gpu_upload_words));
    assert(native_gpu_sync_initial_words(words) == 1);
    /* New texture storage cannot inherit the preceding all-zero cache hit. */
    p_glDeleteProgram(s_native_gpu_program);
    p_glDeleteVertexArrays(1, &s_native_gpu_vao);
    p_glDeleteBuffers(1, &s_native_gpu_vbo);
    glDeleteTextures(1, &s_native_gpu_words);
    glDeleteTextures(1, &s_native_gpu_input);
    s_native_gpu_program = s_native_gpu_vao = s_native_gpu_vbo = 0;
    s_native_gpu_words = s_native_gpu_input = 0;
    assert(native_gpu_program_init());
    assert(!s_native_gpu_upload_words_valid);
    const uint64_t recreated_calls = word_upload_calls;
    assert(native_gpu_sync_initial_words(words) == 1);
    assert(word_upload_calls == recreated_calls + 1u);
    check_word_texture(words);
    printf("word cache: first/changed/recreated uploads and FIFO patches/resets verified\n");
    free(words);
}

static void check_initial_word_rectangles(void) {
    const struct { uint32_t x, y, width, height; } cases[] = {
        {0u, 0u, 1u, 1u}, {1023u, 511u, 1u, 1u}, {63u, 10u, 2u, 1u},
        {95u, 10u, 2u, 2u}, {250u, 200u, 12u, 4u}, {500u, 300u, 16u, 1u},
        {512u, 300u, 16u, 1u},
        {704u, 256u, 320u, 224u}, {0u, 0u, 1024u, 512u}, {960u, 1u, 64u, 510u},
    };
    const size_t size = sizeof(s_native_gpu_upload_words);
    uint16_t *words = calloc(1u, size);
    uint8_t *unaligned = malloc(size + 1u);
    assert(words && unaligned);
    for (size_t c = 0u; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        memset(words, 0, size);
        (void)native_gpu_sync_initial_words(words);
        const uint64_t calls = word_upload_calls, bytes = word_upload_bytes;
        for (uint32_t y = cases[c].y; y < cases[c].y + cases[c].height; ++y)
            for (uint32_t x = cases[c].x; x < cases[c].x + cases[c].width; ++x)
                words[y * VRAM_W + x] = (uint16_t)(1u | (y * 31u + x * 79u));
        memcpy(unaligned + 1u, words, size);
        assert(native_gpu_sync_initial_words(unaligned + 1u) == 1);
        const uint32_t left = (cases[c].x / 64u) * 64u;
        const uint32_t width = ((cases[c].x + cases[c].width + 63u) / 64u) * 64u - left;
        const uint64_t transferred = (uint64_t)width * cases[c].height * sizeof(uint16_t);
        assert(word_upload_calls == calls + 1u);
        assert(word_upload_bytes == bytes + transferred);
        assert(last_word_rect[0] == (GLint)left && last_word_rect[1] == (GLint)cases[c].y);
        assert(last_word_rect[2] == (GLint)width && last_word_rect[3] == (GLint)cases[c].height);
        check_word_texture(words); /* Check all VRAM, including unchanged gaps. */
        GLint stride = -1;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &stride);
        assert(stride == 0);
        assert(native_gpu_sync_initial_words(unaligned + 1u) == 0);
        assert(word_upload_calls == calls + 1u);
        printf("initial words changed=%ux%u at=%u,%u transferred=%llu bytes (previously %zu)\n",
            cases[c].width, cases[c].height, cases[c].x, cases[c].y,
            (unsigned long long)transferred, size);
    }
    /* Different rows expand the left/right edges independently; interior
     * changes still copy across all untouched rows in the bounding rectangle. */
    memset(words, 0, size);
    (void)native_gpu_sync_initial_words(words);
    words[11u * VRAM_W + 800u] = 1u;
    words[20u * VRAM_W + 64u] = 2u;
    words[30u * VRAM_W + 950u] = 3u;
    words[100u * VRAM_W + 256u] = 4u;
    const uint64_t irregular_bytes = word_upload_bytes;
    assert(native_gpu_sync_initial_words(words) == 1);
    assert(last_word_rect[0] == 64 && last_word_rect[1] == 11);
    assert(last_word_rect[2] == 896 && last_word_rect[3] == 90);
    assert(word_upload_bytes == irregular_bytes + 896u * 90u * sizeof(uint16_t));
    check_word_texture(words);
    /* Two distant writes force full coverage, including all rows between them. */
    memset(words, 0, size);
    (void)native_gpu_sync_initial_words(words);
    words[0] = 1u; words[VRAM_W * VRAM_H - 1u] = 2u;
    const uint64_t bytes = word_upload_bytes;
    assert(native_gpu_sync_initial_words(words) == 1);
    assert(word_upload_bytes == bytes + size);
    check_word_texture(words);
    /* Initial snapshots retain their existing mip revalidation policy, even
     * with a partial transfer; include palette consumers and wrapped UV islands. */
    memset(words, 0, size);
    (void)native_gpu_sync_initial_words(words);
    s_native_mip_count = 4u;
    s_native_mips[0] = (GlNativeMipEntry){.depth = 2u, .limits = {0, 0, 31, 31}};
    s_native_mips[1] = (GlNativeMipEntry){.depth = 2u, .page_x = 11u,
        .page_y = 1u, .limits = {0, 0, 127, 100}};
    s_native_mips[2] = (GlNativeMipEntry){.depth = 0u, .clut_x = 704u,
        .clut_y = 300u, .limits = {0, 0, 31, 31}};
    s_native_mips[3] = (GlNativeMipEntry){.depth = 2u, .limits = {-2, -2, 1, 1}};
    words[300u * VRAM_W + 705u] = 0x001fu;
    assert(native_gpu_sync_initial_words(words) == 1);
    for (uint32_t i = 0u; i < s_native_mip_count; ++i) assert(s_native_mips[i].dirty);
    s_native_mip_count = 0u;
    check_word_texture(words);
    free(words); free(unaligned);
}

static int baseline_word_upload(const void *words) {
    /* Exact pre-optimization baseline: it already compared for mip invalidation,
     * but still copied and sent the full texture on every batch. */
    if (memcmp(s_native_gpu_upload_words, words, sizeof(s_native_gpu_upload_words)) != 0)
        native_gpu_mips_dirty_all();
    memcpy(s_native_gpu_upload_words, words, sizeof(s_native_gpu_upload_words));
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_native_gpu_words);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VRAM_W, VRAM_H,
        GL_RED_INTEGER, GL_UNSIGNED_SHORT, s_native_gpu_upload_words);
    return 1;
}

static void benchmark_word_uploads(void) {
    enum { iterations = 1500, trials = 5 };
    uint16_t *words = malloc(sizeof(s_native_gpu_upload_words));
    assert(words);
    for (size_t i = 0u; i < VRAM_W * VRAM_H; ++i) words[i] = (uint16_t)(i * 40503u);
    assert(native_gpu_sync_initial_words(words) == 1);
    printf("benchmark renderer: %s\n", glGetString(GL_RENDERER));
    /* No changes, one edge word, and distant corners requiring full coverage. */
    for (unsigned changing = 0u; changing < 3u; ++changing)
    for (unsigned trial = 0u; trial < trials; ++trial) {
        /* Alternate order; measure CPU and complete GL work inside each sample. */
        for (unsigned pass = 0u; pass < 2u; ++pass) {
            const bool optimized = (pass ^ (trial & 1u)) != 0u;
            int (*volatile upload)(const void *) = optimized
                ? native_gpu_sync_initial_words : baseline_word_upload;
            glFinish();
            const uint64_t calls = word_upload_calls, bytes = word_upload_bytes;
            const uint64_t start = SDL_GetTicksNS(), cpu_start = native_thread_cpu_ns();
            for (unsigned i = 0u; i < iterations; ++i) {
                if (changing) words[VRAM_W * VRAM_H - 1u] ^= 1u;
                if (changing == 2u) words[0] ^= 1u;
                (void)upload(words);
            }
            glFinish();
            const uint64_t cpu_ns = native_thread_cpu_ns() - cpu_start;
            const uint64_t elapsed = SDL_GetTicksNS() - start;
            assert(word_upload_calls - calls == (optimized && !changing ? 0u : iterations));
            const uint64_t expected_bytes = optimized && !changing ? 0u :
                (uint64_t)iterations * (optimized && changing == 1u ? 128u : sizeof(s_native_gpu_upload_words));
            assert(word_upload_bytes - bytes == expected_bytes);
            printf("changing=%u trial=%u optimized=%d iterations=%u wall_ns=%llu cpu_ns=%llu uploads=%llu bytes=%llu\n",
                changing, trial, optimized, iterations, (unsigned long long)elapsed,
                (unsigned long long)cpu_ns, (unsigned long long)(word_upload_calls - calls),
                (unsigned long long)(word_upload_bytes - bytes));
        }
    }
    free(words);
}

static void check_scale(uint32_t scale, bool depth24) {
    XgRenderSourceBuilder builder;
    XgRenderSourceCommitHandle commit;
    const XgPresentationIdentity identity = {.presentation_epoch = 1u,
        .source_sequence = 1u, .guest_vblank_sequence = 1u,
        .guest_cycle = 100u, .scene_generation = 1u};
    const XgSemanticSceneIdentity scene = {.disc_id = 1u,
        .executable_identity = 1u, .module = XG_SEMANTIC_MODULE_FIELD};
    const XgSemanticDisplayState display = {.width = 320u, .height = 224u,
        .aspect_num = 4u, .aspect_den = 3u, .native_width = 320u,
        .native_height = 240u, .render_scale = scale, .depth24 = depth24};
    assert(xg_render_source_commit_begin(&identity, &scene, &display, 1u,
        false, false, &builder) == XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_set_native_work(builder, true) == XG_RENDER_SOURCE_COMMIT_OK);
    XgRenderNativeOperation operation = {.kind = XG_RENDER_NATIVE_OPERATION_TARGET,
        .width = 320u, .height = 224u};
    assert(xg_render_source_commit_append_native_operation(builder, &operation) == XG_RENDER_SOURCE_COMMIT_OK);
    operation = (XgRenderNativeOperation){.kind = XG_RENDER_NATIVE_OPERATION_DRAW,
        .semantic = triangle(0x10000u, 0u, 255u)};
    assert(xg_render_source_commit_append_native_operation(builder, &operation) == XG_RENDER_SOURCE_COMMIT_OK);
    /* Host additions in the DMA aperture deliberately never enter CPU VRAM.
     * Red must cover the guest's green triangle in the presented GPU image. */
    operation.semantic = triangle(0x800004u, 255u, 0u);
    assert(xg_render_source_commit_append_native_operation(builder, &operation) == XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) == XG_RENDER_SOURCE_COMMIT_OK);
    GlNativeCompileAudit *audit = calloc(1u, sizeof(*audit));
    GlNativeViewState *views = calloc(1u, sizeof(*views));
    const GlNativeViewState base = {.active = -1};
    const XgPresentationIdentity base_identity = {0};
    uint32_t *vram = NULL, *pixels = NULL;
    uint16_t *words = NULL;
    assert(audit && views);
    assert(xg_render_source_commit_header_copy(commit, &audit->header) == XG_RENDER_SOURCE_COMMIT_OK);
    GlNativeGpuWork *work = native_gpu_work_create(commit, audit, &base, 0u, NULL, 1);
    assert(work);
    assert(native_compile_native_work(commit, audit, &vram, &words, &pixels,
        views, work, &base, NULL, NULL, &base_identity));
    const size_t pixel_count = (size_t)audit->endpoint_storage_width * audit->header.display.height;
    assert(audit->endpoint_pixel_digest == pres_hash_bytes((const uint8_t *)pixels, pixel_count * sizeof(*pixels)));
    uint64_t visible = 0u;
    for (size_t i = 0u; i < pixel_count; ++i) visible += (pixels[i] & UINT32_C(0x00ffffff)) != 0u;
    assert(audit->endpoint_visible_pixels == visible);
    assert(work->scale == scale);
    if (depth24) {
        assert(!work->scanout);
        /* A movie's RGB bytes still come from canonical words, at any scale. */
        const size_t byte_x = 32u * 3u;
        uint32_t expected = UINT32_C(0xff000000);
        for (unsigned channel = 0u; channel < 3u; ++channel) {
            const size_t x = byte_x + channel;
            expected |= ((uint32_t)(words[32u * VRAM_W + x / 2u] >>
                ((x & 1u) * 8u)) & 255u) << (channel * 8u);
        }
        assert(pixels[32u * 320u + 32u] == expected);
        printf("scale=%u depth24 CPU pixel=%08x\n", scale, expected);
    } else {
        /* The GPU-only addition must not alter the canonical guest words. */
        assert(words[32u * VRAM_W + 32u] == 0x03e0u);
        uint32_t sample = pixels[32u * 320u + 32u];
        if (work->scanout) {
            work->deferred = 0;
            native_gpu_publish_prefix(work, 1);
            const uint64_t deadline = SDL_GetTicksNS() + UINT64_C(5000000000);
            while (work->state != NATIVE_GPU_READY && SDL_GetTicksNS() < deadline) {
                assert(native_gpu_service() >= 0);
                SDL_Delay(1u);
            }
            assert(work->state == NATIVE_GPU_READY && !work->failed);
            assert(work->images[0].width == 320u * scale);
            assert(work->images[0].height == 224u * scale);
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, work->images[0].framebuffer);
            glReadPixels(32u * scale, 32u * scale, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &sample);
        }
        printf("scale=%u GPU scanout=%d host pixel=%08x guest word=%04x\n",
            scale, work->scanout, sample, words[32u * VRAM_W + 32u]);
        fflush(stdout);
        assert(sample == UINT32_C(0xff0000ff));
        assert(glGetError() == GL_NO_ERROR);
    }
    s_native_gpu_work = NULL;
    native_gpu_free(work);
    native_views_discard(views, 1);
    free(views); free(audit); free(vram); free(words); free(pixels);
    assert(xg_render_source_commit_retire(commit) == XG_RENDER_SOURCE_COMMIT_OK);
}

/* Field wave copies bypass VIEW transfers, but canonical VRAM must still
 * copy the current pre-write image. Cover overlapping lifts, saved blocks,
 * and a stale snapshot left by an earlier ordinary COPY at both scales. */
static void check_wave_copy(uint32_t scale, bool saved_block, bool stale_snapshot) {
    XgRenderSourceBuilder builder;
    XgRenderSourceCommitHandle commit;
    const XgPresentationIdentity identity = {.presentation_epoch = 1u,
        .source_sequence = 1u, .guest_vblank_sequence = 1u,
        .guest_cycle = 100u, .scene_generation = 1u};
    const XgSemanticSceneIdentity scene = {.disc_id = 1u,
        .executable_identity = 1u, .module = XG_SEMANTIC_MODULE_FIELD};
    const XgSemanticDisplayState display = {.width = 320u, .height = 224u,
        .aspect_num = 4u, .aspect_den = 3u, .native_width = 320u,
        .native_height = 240u, .render_scale = scale};
    assert(xg_render_source_commit_begin(&identity, &scene, &display, 1u,
        false, false, &builder) == XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_set_native_work(builder, true) == XG_RENDER_SOURCE_COMMIT_OK);
    XgRenderNativeOperation operation = {.kind = XG_RENDER_NATIVE_OPERATION_TARGET,
        .width = 320u, .height = 224u};
    assert(xg_render_source_commit_append_native_operation(builder, &operation) == XG_RENDER_SOURCE_COMMIT_OK);
    if (stale_snapshot) {
        operation = (XgRenderNativeOperation){.kind = XG_RENDER_NATIVE_OPERATION_COPY,
            .dst_x = 800u, .dst_y = 256u, .width = 16u, .height = 16u};
        assert(xg_render_source_commit_append_native_operation(builder, &operation) == XG_RENDER_SOURCE_COMMIT_OK);
    }
    operation = (XgRenderNativeOperation){.kind = XG_RENDER_NATIVE_OPERATION_FILL,
        .dst_y = 128u, .width = 320u, .height = 16u, .fill_color = 0x7c00u};
    assert(xg_render_source_commit_append_native_operation(builder, &operation) == XG_RENDER_SOURCE_COMMIT_OK);
    operation = (XgRenderNativeOperation){.kind = XG_RENDER_NATIVE_OPERATION_COPY,
        .src_y = 32u, .dst_x = saved_block ? 400u : 0u,
        .dst_y = saved_block ? 256u : 0u, .width = 320u, .height = 192u,
        .semantic = {.native_view_effect = GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID}};
    assert(xg_render_source_commit_append_native_operation(builder, &operation) == XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) == XG_RENDER_SOURCE_COMMIT_OK);

    uint32_t *initial_pixels = calloc(VRAM_W * VRAM_H, sizeof(*initial_pixels));
    uint16_t *initial_words = calloc(VRAM_W * VRAM_H, sizeof(*initial_words));
    GlNativeCompileAudit *audit = calloc(1u, sizeof(*audit));
    GlNativeViewState *views = calloc(1u, sizeof(*views));
    assert(initial_pixels && initial_words && audit && views);
    for (uint32_t y = 0u; y < 224u; ++y)
        for (uint32_t x = 0u; x < 320u; ++x) {
            const uint16_t word = 1u + (y * 101u + x * 7u) % 0x7bffu;
            initial_words[y * VRAM_W + x] = word;
            initial_pixels[y * VRAM_W + x] = conv_1555_to_rgba8(word);
        }
    const GlNativeViewState base = {.active = -1};
    const XgPresentationIdentity base_identity = {0};
    uint32_t *vram = NULL, *pixels = NULL;
    uint16_t *words = NULL;
    assert(xg_render_source_commit_header_copy(commit, &audit->header) == XG_RENDER_SOURCE_COMMIT_OK);
    GlNativeGpuWork *work = native_gpu_work_create(commit, audit, &base, 0u, NULL, 1);
    assert(work);
    assert(native_compile_native_work(commit, audit, &vram, &words, &pixels,
        views, work, &base, initial_pixels, initial_words, &base_identity));
    assert(views->count == 1u);
    assert(saved_block ? views->targets[0].wave.transfer_count == 1u :
        views->targets[0].wave.lifted == 1);
    work->deferred = 0;
    native_gpu_publish_prefix(work, 1);
    const uint64_t deadline = SDL_GetTicksNS() + UINT64_C(5000000000);
    while (work->state != NATIVE_GPU_READY && SDL_GetTicksNS() < deadline) {
        assert(native_gpu_service() >= 0);
        SDL_Delay(1u);
    }
    assert(work->state == NATIVE_GPU_READY && !work->failed);
    const uint32_t width = operation.width * scale, height = operation.height * scale;
    uint32_t *actual = malloc((size_t)width * height * sizeof(*actual));
    assert(actual);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, work->planes[0].framebuffer);
    glReadPixels(operation.dst_x * scale, operation.dst_y * scale, width, height,
        GL_RGBA, GL_UNSIGNED_BYTE, actual);
    assert(glGetError() == GL_NO_ERROR);
    for (uint32_t y = 0u; y < height; ++y)
        for (uint32_t x = 0u; x < width; ++x) {
            const uint32_t sy = operation.src_y + y / scale;
            const uint16_t expected = sy >= 128u && sy < 144u ? 0x7c00u :
                initial_words[sy * VRAM_W + x / scale];
            assert(words[(operation.dst_y + y / scale) * VRAM_W + operation.dst_x + x / scale] == expected);
            assert((actual[(size_t)y * width + x] & UINT32_C(0x00ffffff)) ==
                (conv_1555_to_rgba8(expected) & UINT32_C(0x00ffffff)));
        }
    printf("wave COPY scale=%u saved_block=%d stale_snapshot=%d: all GPU pixels match\n",
        scale, saved_block, stale_snapshot);
    s_native_gpu_work = NULL;
    native_gpu_free(work);
    native_views_discard(views, 1);
    free(actual); free(initial_pixels); free(initial_words);
    free(views); free(audit); free(vram); free(words); free(pixels);
    assert(xg_render_source_commit_retire(commit) == XG_RENDER_SOURCE_COMMIT_OK);
}

int main(int argc, char **argv) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL video initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window *window = SDL_CreateWindow("Native scale regression", 0, 0, 32, 32,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    assert(window);
    SDL_GLContext context = SDL_GL_CreateContext(window);
    assert(context && load_modern_gl());
    s_native_gpu_ctx = context;
    s_native_state_mutex = SDL_CreateMutex();
    s_native_gpu_condition = SDL_CreateCond();
    assert(s_native_state_mutex && s_native_gpu_condition);
    s_native_active = 1;
    check_word_upload_cache();
    check_initial_word_rectangles();
    check_scale(1u, false);
    check_scale(4u, false);
    check_scale(1u, true);
    check_scale(4u, true);
    for (uint32_t scale = 1u; scale <= 4u; scale += 3u)
        for (unsigned saved = 0u; saved < 2u; ++saved)
            for (unsigned stale = 0u; stale < 2u; ++stale)
                check_wave_copy(scale, saved != 0u, stale != 0u);
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) benchmark_word_uploads();
    SDL_DestroyCond(s_native_gpu_condition);
    SDL_DestroyMutex(s_native_state_mutex);
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
