/* Source-owned GPU-only geometry must reach Native scanout at 1x as at 4x.
 * Word-texture reuse must preserve actual GL bytes across patches and resets.
 * Exercise the real collector/compiler and GPU with a hidden GL context; no
 * game executable, disc, BIOS or controller input is used. */
#include "psx_sdl.h"

static int thread_creations_before_failure = -1;
static SDL_Thread *worker_test_create_thread(SDL_ThreadFunction function,
        const char *name, void *data) {
    if (thread_creations_before_failure == 0) return NULL;
    if (thread_creations_before_failure > 0) --thread_creations_before_failure;
    return SDL_CreateThread(function, name, data);
}
#undef SDL_CreateThread
#define SDL_CreateThread worker_test_create_thread
#include "../src/gpu_gl_renderer.c"
#undef SDL_CreateThread

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

static void check_scanout_resolve(int benchmark) {
    assert(native_gpu_scanout_init());
    const unsigned cases[][3] = {{1,1,0}, {1,4,0}, {4,1,0}, {4,4,0}, {4,4,2}};
    for (unsigned c=0; c<sizeof(cases)/sizeof(*cases); ++c) {
        const unsigned source_scale=cases[c][0], scale=cases[c][1], border=cases[c][2];
        GlNativeGpuPlane input={0}, output={0}, mask={0};
        assert(native_gpu_plane_size(&input,384*source_scale,256*source_scale));
        assert(native_gpu_plane_size(&output,(320+2*border)*scale,224*scale));
        assert(native_gpu_mask_size(&mask,output.width,output.height));
        uint32_t *pixels=malloc((size_t)input.width*input.height*4u);
        uint32_t *actual=malloc((size_t)output.width*output.height*4u);
        uint8_t *classes=malloc((size_t)output.width*output.height);
        assert(pixels && actual && classes);
        for (unsigned i=0;i<input.width*input.height;++i)
            pixels[i]=(i*UINT32_C(2654435761)&UINT32_C(0x00ffffff))|((i&255u)<<24u);
        glBindTexture(GL_TEXTURE_2D,input.texture);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,input.width,input.height,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        native_gpu_scanout_begin(&output,&mask,border!=0u);
        native_gpu_scanout_rect(&input,31,17,border,0,320,224,source_scale,scale);
        native_gpu_scanout_end();
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,output.framebuffer);
        glReadPixels(0,0,output.width,output.height,GL_RGBA,GL_UNSIGNED_BYTE,actual);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,mask.framebuffer);
        glReadPixels(0,0,output.width,output.height,GL_RED,GL_UNSIGNED_BYTE,classes);
        for (unsigned y=0;y<output.height;++y) for(unsigned x=0;x<output.width;++x) {
            uint32_t expected=UINT32_C(0xff000000);
            if (x>=border*scale && x<(320+border)*scale) {
                const unsigned sx=31*source_scale+((2*(x-border*scale)+1)*source_scale)/(2*scale);
                const unsigned sy=17*source_scale+((2*y+1)*source_scale)/(2*scale);
                expected=pixels[(size_t)sy*input.width+sx];
            }
            assert(actual[(size_t)y*output.width+x]==(expected|UINT32_C(0xff000000)));
            assert(classes[(size_t)y*output.width+x]==(expected>>24u));
        }
        glBindTexture(GL_TEXTURE_2D,mask.texture);
        GLint swizzle=0,format=0;
        glGetTexParameteriv(GL_TEXTURE_2D,0x8E45,&swizzle);
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&format);
        assert(swizzle==GL_RED && format==0x8229);
        assert(glGetError()==GL_NO_ERROR);
        printf("scanout resolve source=%ux output=%ux border=%u: exact RGB and all mask bytes\n",source_scale,scale,border);
        if (benchmark && source_scale==4 && scale==4 && !border) {
            GlNativeGpuPlane old_mask={0};
            assert(native_gpu_plane_size(&old_mask,output.width,output.height));
            for (unsigned trial=0;trial<3;++trial) for(unsigned optimized=0;optimized<2;++optimized) {
                GLuint query[2]; GLuint64 begin,end;
                p_glGenQueries(2,query);p_glQueryCounter(query[0],GL_TIMESTAMP);
                for(unsigned frame=0;frame<1000;++frame) {
                    if(optimized) {
                        native_gpu_scanout_begin(&output,&mask,0);
                        native_gpu_scanout_rect(&input,31,17,0,0,320,224,4,4);
                        native_gpu_scanout_end();
                    } else {
                        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,input.framebuffer);
                        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,output.framebuffer);
                        glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
                        p_glBlitFramebuffer(31*4,17*4,351*4,241*4,0,0,320*4,224*4,GL_COLOR_BUFFER_BIT,GL_NEAREST);
                        native_gpu_blit(&output,&old_mask,0,0,output.width,output.height);
                        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,output.framebuffer);
                        glColorMask(0,0,0,1);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glColorMask(1,1,1,1);
                    }
                }
                p_glQueryCounter(query[1],GL_TIMESTAMP);
                p_glGetQueryObjectui64v(query[0],GL_QUERY_RESULT,&begin);
                p_glGetQueryObjectui64v(query[1],GL_QUERY_RESULT,&end);
                printf("scanout benchmark trial=%u optimized=%u GPU_ns_per_image=%llu mask_bytes=%u\n",
                    trial,optimized,(unsigned long long)((end-begin)/1000),output.width*output.height*(optimized?1u:4u));
                p_glDeleteQueries(2,query);
            }
            native_gpu_plane_free(&old_mask);
        }
        native_gpu_plane_free(&input);native_gpu_plane_free(&output);native_gpu_plane_free(&mask);
        free(pixels);free(actual);free(classes);
    }
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

/* Compare the actual ordered raster shader with both parameter delivery paths.
 * Material/attribute records come from the real draw compiler, and the complete
 * RGBA and integer-depth images must remain byte-identical. */
static void check_raster_equivalence(unsigned scale,int gaps,int benchmark) {
    if (!s_native_gpu_interlock) return;
    enum { COUNT=64, WIDTH=320, HEIGHT=224 };
    const unsigned SCALE=scale;
    gl_renderer_set_anisotropy(16);
    GlNativeGpuWork *work=calloc(1,sizeof(*work));
    GlNativeGpuParameters *parameters=calloc(COUNT,sizeof(*parameters));
    uint8_t visible[COUNT]={0};
    uint32_t (*texels)[4]=calloc(COUNT*(NATIVE_TRIANGLE_TEXELS+NATIVE_SHARED_TEXELS),sizeof(*texels));
    assert(work && parameters && texels);
    uint16_t *words=malloc(VRAM_W*VRAM_H*sizeof(*words));assert(words);
    for(unsigned y=0;y<VRAM_H;++y)for(unsigned x=0;x<VRAM_W;++x)
        words[y*VRAM_W+x]=(x+y)%17==0?0:((x&31)|((y&31)<<5)|(((x+y)&31)<<10)|((x%3!=0)<<15));
    assert(native_gpu_sync_initial_words(words)>=0);free(words);
    work->scale=SCALE;work->depth_test=1;work->count=COUNT;
    work->commands=calloc(COUNT,sizeof(*work->commands));
    work->widths[1]=WIDTH;work->heights[1]=HEIGHT;
    assert(work->commands && native_gpu_plane_size(&work->planes[1],WIDTH*SCALE,HEIGHT*SCALE));
    assert(native_gpu_plane_depth(&work->planes[1]));
    for(unsigned i=0;i<COUNT;++i) {
        GpuRenderSemantic draw=triangle(i+1,32+i*3,240-i*2);
        draw.screen_space_2d=GPU_RENDER_SCREEN_SPACE_2D_NONE;
        draw.material.textured=i%2;draw.material.texture_depth=GPU_RENDER_TEXTURE_15_BIT;
        draw.material.shading=(i/4)%2?GPU_RENDER_SHADING_GOURAUD:GPU_RENDER_SHADING_FLAT;
        draw.material.raw_texture=(i/8)%2;draw.material.semi_transparent=(i/2)%2;draw.material.blend_mode=(i/4)%4;
        draw.material.mask_set=(i/8)%2;draw.material.mask_check=(i/16)%2;
        draw.depth_policy=GPU_RENDER_DEPTH_TEST_WRITE;draw.aa_exempt=i%2;
        for(unsigned v=0;v<3;++v) {
            GpuRenderSemanticVertex *vertex=&draw.triangles[0].vertices[v];
            vertex->x+=(int32_t)(i%4)*48*65536;vertex->y+=(int32_t)((i/4)%3)*40*65536;
            vertex->u=(v==1?(i%3==0?504:120):4)*65536;vertex->v=(v==2?100:4)*65536;
            if(i%5==0){vertex->x+=32768;vertex->y-=32768;}
            vertex->r=32+i*3+v*3;vertex->g=240-i*2-v*7;
            if(i%7==0){vertex->projective_position=1;vertex->projective_view_z=12+(i%7)+v;}
            vertex->native_view_depth=4096*(12+(i%7)+v);
        }
        if(i%2){GpuRenderSemanticVertex vertex=draw.triangles[0].vertices[0];
            draw.triangles[0].vertices[0]=draw.triangles[0].vertices[1];
            draw.triangles[0].vertices[1]=vertex;}
        work->commands[i].kind=NATIVE_GPU_DRAW;work->commands[i].plane=1;
        assert(native_materialize_native_draw(&draw,&work->commands[i].draw));
    }
    p_glUseProgram(s_native_gpu_program);p_glBindVertexArray(s_native_gpu_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER,s_native_gpu_vbo);
    float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS]=NULL;
    assert(native_gpu_upload_vertex_slice(work,work->commands,NULL,0,COUNT,&vertices));
    s_native_gpu_template_capture.parameters=parameters;s_native_gpu_template_capture.visible=visible;
    for(unsigned i=0;i<COUNT;++i)
        assert(native_gpu_render_command(work,&work->commands[i],NULL,NULL,NULL,i*3,NULL,vertices,NULL));
    memset(&s_native_gpu_template_capture,0,sizeof(s_native_gpu_template_capture));
    GLint first[COUNT];GLsizei counts[COUNT];unsigned draws=0;
    for(unsigned i=0;i<COUNT;++i) {
        uint32_t record[NATIVE_TRIANGLE_TEXELS][4],shared[NATIVE_SHARED_TEXELS][4];
        native_gpu_pack_parameters(&parameters[i],record,shared,i);
        memcpy(texels+i*NATIVE_TRIANGLE_TEXELS,record,sizeof(record));
        memcpy(texels+COUNT*NATIVE_TRIANGLE_TEXELS+i*NATIVE_SHARED_TEXELS,shared,sizeof(shared));
        if(visible[i] && (!gaps || i%3!=1)){first[draws]=i*3;counts[draws++]=3;}
    }
    assert(draws==(gaps?COUNT-21:COUNT));
    GLuint buffer,texture,programs[2];p_glGenBuffers(1,&buffer);glGenTextures(1,&texture);
    p_glBindBuffer(PSXGL_TEXTURE_BUFFER,buffer);
    p_glBufferData(PSXGL_TEXTURE_BUFFER,COUNT*(NATIVE_TRIANGLE_TEXELS+NATIVE_SHARED_TEXELS)*sizeof(*texels),texels,PSXGL_STREAM_DRAW);
    p_glActiveTexture(PSXGL_TEXTURE0+7);glBindTexture(PSXGL_TEXTURE_BUFFER,texture);
    p_glTexBuffer(PSXGL_TEXTURE_BUFFER,PSXGL_RGBA32UI,buffer);
    for(unsigned path=0;path<2;++path) {
        programs[path]=native_gpu_ordered_program(path==0);assert(programs[path]);
        p_glUseProgram(programs[path]);
        const char *samplers[]={"words","destination","source_image","destination_depth","source_depth","hd_image","mip_image","parameters","recipe_words"};
        for(unsigned u=0;u<9;++u)p_glUniform1i(p_glGetUniformLocation(programs[path],samplers[u]),u);
        p_glUniform1i(p_glGetUniformLocation(programs[path],"parameters_on"),1);
        p_glUniform1i(p_glGetUniformLocation(programs[path],"parameter_shared"),COUNT*NATIVE_TRIANGLE_TEXELS);
        float size[4];memcpy(size,parameters[0].value[NATIVE_PARAMETER_FLOAT4+NATIVE_UNIFORM_SIZE],sizeof(size));
        p_glUniform4f(p_glGetUniformLocation(programs[path],"u_size"),size[0],size[1],size[2],size[3]);
    }
    const size_t bytes=(size_t)WIDTH*HEIGHT*SCALE*SCALE*4;
    uint32_t *reference=malloc(bytes*2),*actual=malloc(bytes*2);assert(reference && actual);
    p_glActiveTexture(PSXGL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,s_native_gpu_words);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,work->planes[1].framebuffer);
    glViewport(0,0,WIDTH*SCALE,HEIGHT*SCALE);glDisable(GL_SCISSOR_TEST);
    p_glBindImageTexture(0,work->planes[1].texture,0,GL_FALSE,0,PSXGL_READ_WRITE,GL_RGBA8);
    p_glBindImageTexture(1,work->planes[1].depth,0,GL_FALSE,0,PSXGL_READ_WRITE,PSXGL_R32UI);
    const GLenum attachments[2]={PSXGL_COLOR_ATTACHMENT0,PSXGL_COLOR_ATTACHMENT1};p_glDrawBuffers(2,attachments);
    for(unsigned trial=0;trial<(benchmark?3u:1u);++trial)for(unsigned pass=0;pass<2;++pass) {
        const unsigned path=(pass+trial)%2,iterations=benchmark?2048:1;
        p_glUseProgram(programs[path]);glFinish();GLuint query[2];GLuint64 begin,end;
        p_glGenQueries(2,query);p_glQueryCounter(query[0],GL_TIMESTAMP);
        for(unsigned frame=0;frame<iterations;++frame) {
            p_glMemoryBarrier(PSXGL_SHADER_IMAGE_ACCESS_BARRIER_BIT|PSXGL_FRAMEBUFFER_BARRIER_BIT);
            glColorMask(1,1,1,1);glClearColor(0.1f,0.2f,0.3f,0);glClear(GL_COLOR_BUFFER_BIT);
            const GLuint far[4]={0};p_glColorMaski(1,1,1,1,1);p_glClearBufferuiv(PSXGL_COLOR,1,far);
            glColorMask(0,0,0,0);
            p_glMultiDrawArrays(GL_TRIANGLES,first,counts,draws);
        }
        p_glMemoryBarrier(PSXGL_SHADER_IMAGE_ACCESS_BARRIER_BIT|PSXGL_FRAMEBUFFER_BARRIER_BIT);
        p_glQueryCounter(query[1],GL_TIMESTAMP);p_glGetQueryObjectui64v(query[0],GL_QUERY_RESULT,&begin);p_glGetQueryObjectui64v(query[1],GL_QUERY_RESULT,&end);p_glDeleteQueries(2,query);
        uint32_t *pixels=path?actual:reference;
        glReadBuffer(PSXGL_COLOR_ATTACHMENT0);glReadPixels(0,0,WIDTH*SCALE,HEIGHT*SCALE,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        glReadBuffer(PSXGL_COLOR_ATTACHMENT1);glReadPixels(0,0,WIDTH*SCALE,HEIGHT*SCALE,PSXGL_RED_INTEGER,GL_UNSIGNED_INT,pixels+bytes/4);
        assert(glGetError()==GL_NO_ERROR);
        printf("raster scale=%u gaps=%d trial=%u optimized=%u GPU_ns_per_frame=%llu digest=%016llx\n",scale,gaps,trial,path,(unsigned long long)((end-begin)/iterations),(unsigned long long)pres_hash_bytes((const uint8_t *)pixels,bytes*2));
        if(path)assert(memcmp(reference,actual,bytes*2)==0);
    }
    assert(memcmp(reference,actual,bytes*2)==0);
    unsigned coloured=0,depth_written=0;
    for(size_t i=0;i<bytes/4;++i){coloured+=(reference[i]&0xffffffu)!=0;depth_written+=reference[bytes/4+i]!=0;}
    assert(coloured && depth_written);
    glReadBuffer(PSXGL_COLOR_ATTACHMENT0);glColorMask(1,1,1,1);p_glUseProgram(s_native_gpu_program);
    for(unsigned p=0;p<2;++p)p_glDeleteProgram(programs[p]);
    p_glDeleteBuffers(1,&buffer);glDeleteTextures(1,&texture);free(reference);free(actual);
    free(vertices);free(parameters);free(texels);native_gpu_free(work);
}

/* Opaque texels outside a published UI rectangle must not touch the game.
 * This exercises the actual presenter compositor rather than a bounds helper. */
/* Include CPU geometry, material packing, upload and compute completion in the
 * crossover measurement. A mesh is one command, not one triangle: using draw
 * record counts here would hide the expensive case the selector must handle. */
static void check_setup_batches(int benchmark) {
    if (!s_native_gpu_interlock || !SDL_AtomicGet(&s_native_gpu_setup_ready)) return;
    const uint32_t sizes[] = {1u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u};
    for (size_t n = 0u; n < sizeof(sizes) / sizeof(*sizes); ++n) {
        const uint32_t count = sizes[n];
        GlNativeGpuWork *work = calloc(1u, sizeof(*work));
        GlNativeGpuCommand command = {.kind = NATIVE_GPU_DRAW, .plane = 1u, .mesh_count = count};
        GlNativeGpuMeshTriangle *mesh = calloc(count, sizeof(*mesh));
        assert(work && mesh);
        GpuRenderSemantic draw = triangle(1u, 128u, 64u);
        draw.screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_NONE;
        draw.depth_policy = GPU_RENDER_DEPTH_TEST_WRITE;
        assert(native_materialize_native_draw(&draw, &command.draw));
        for (uint32_t t = 0u; t < count; ++t) {
            mesh[t].triangle = command.draw.primitive.triangles[0];
            mesh[t].texture.shading = t % 2u ? XG_RENDER_IR_SHADING_GOURAUD : XG_RENDER_IR_SHADING_FLAT;
            mesh[t].texture.semi_transparent = t % 3u == 0u;
            for (unsigned v = 0u; v < 3u; ++v) {
                mesh[t].triangle.vertices[v].x += (int32_t)(t % 7u) * 32768;
                mesh[t].triangle.vertices[v].y += (int32_t)((t + v) % 5u) * 8192;
                mesh[t].triangle.vertices[v].r = (uint8_t)(32u + t % 128u + v * 7u);
                mesh[t].triangle.vertices[v].g = (uint8_t)(240u - t % 128u - v * 13u);
                mesh[t].triangle.vertices[v].native_view_depth = (12 + v) * 4096;
            }
        }
        work->scale = 4u; work->depth_test = 1; work->count = 1u;
        work->commands = &command; work->data = (uint8_t *)mesh;
        work->widths[1] = 320u; work->heights[1] = 224u;
        assert(native_gpu_plane_size(&work->planes[1], 1280u, 896u));
        assert(native_gpu_plane_depth(&work->planes[1]));
        const size_t bytes = (size_t)count * (NATIVE_TRIANGLE_TEXELS + NATIVE_SHARED_TEXELS) * 16u;
        uint32_t (*reference)[4] = malloc(bytes); assert(reference);
        const unsigned repeats = benchmark ? 32u : 1u;
        for (unsigned trial = 0u; trial < (benchmark ? 3u : 1u); ++trial)
            for (unsigned path = 0u; path < 2u; ++path) {
                glFinish();
                const uint64_t begin = SDL_GetPerformanceCounter();
                for (unsigned repeat = 0u; repeat < repeats; ++repeat) {
                    work->gpu_setup = path;
                    GlNativePhaseTemplates cache = {.count = 1u, .commands = &command,
                        .data = (uint8_t *)mesh, .geometry = native_gpu_geometry_prepare(work)};
                    assert(cache.geometry);
                    work->phase_templates = &cache;
                    work->template_parameters_attempted = 0;
                    p_glUseProgram(s_native_gpu_program);
                    native_gpu_template_parameters_prepare(work, 1u);
                    assert(work->template_parameters);
                    if (!trial && !repeat) {
                        p_glMemoryBarrier(0x200 /* GL_BUFFER_UPDATE_BARRIER_BIT */);
                        p_glBindBuffer(PSXGL_TEXTURE_BUFFER, s_native_gpu_template_parameter_buffer);
                        const uint32_t (*actual)[4] = p_glMapBufferRange(PSXGL_TEXTURE_BUFFER, 0, bytes, PSXGL_MAP_READ_BIT);
                        assert(actual);
                        if (!path) memcpy(reference, actual, bytes);
                        else assert(memcmp(reference, actual, bytes) == 0);
                        assert(p_glUnmapBuffer(PSXGL_TEXTURE_BUFFER));
                    }
                    free(work->template_parameters->commands);
                    free(work->template_parameters->visible);
                    free(work->template_parameters); work->template_parameters = NULL;
                    native_gpu_geometry_free(cache.geometry);
                }
                glFinish();
                const double us = (double)(SDL_GetPerformanceCounter() - begin) * 1e6 /
                    (double)SDL_GetPerformanceFrequency() / repeats;
                if (benchmark) printf("setup triangles=%u trial=%u gpu=%u us=%.3f\n", count, trial, path, us);
            }
        free(reference); free(mesh);
        work->commands = NULL; work->data = NULL; work->phase_templates = NULL;
        native_gpu_free(work);
    }
    puts("triangle setup: CPU/GPU parameter buffers match, including subpixel positions and depth");
}

static void check_setup_selection(void) {
    const char *option = getenv("PSX_NATIVE_GPU_SETUP");
    char *saved = option ? strdup(option) : NULL;
    const int ready = SDL_AtomicGet(&s_native_gpu_setup_ready);
    SDL_AtomicSet(&s_native_gpu_setup_ready, 1);
    assert(SDL_setenv("PSX_NATIVE_GPU_SETUP", "", 1) == 0);
    assert(!native_gpu_setup_enabled(0u));
    assert(!native_gpu_setup_enabled(1u));
    assert(!native_gpu_setup_enabled(128u));
    assert(native_gpu_setup_enabled(129u));
    assert(native_gpu_setup_enabled(UINT64_MAX));
    assert(SDL_setenv("PSX_NATIVE_GPU_SETUP", "0", 1) == 0);
    assert(!native_gpu_setup_enabled(4097u));
    assert(SDL_setenv("PSX_NATIVE_GPU_SETUP", "1", 1) == 0);
    assert(native_gpu_setup_enabled(1u));
    SDL_AtomicSet(&s_native_gpu_setup_ready, 0);
    assert(!native_gpu_setup_enabled(4097u));
    SDL_AtomicSet(&s_native_gpu_setup_ready, ready);
    assert(SDL_setenv("PSX_NATIVE_GPU_SETUP", saved ? saved : "", 1) == 0);
    free(saved);
}

static void check_ui_clip(void) {
    uint32_t blue[32*32],actual[32*32];
    for(unsigned i=0;i<32*32;++i)blue[i]=UINT32_C(0xffff0000);
    GlUiLayer *layer=&s_ui_layers[0];
    layer->texture=make_tex(GL_RGBA8,32,32,GL_RGBA,GL_UNSIGNED_BYTE);
    layer->width=layer->height=32;assert(layer->texture);
    glBindTexture(GL_TEXTURE_2D,layer->texture);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,blue);
    s_ui_front=0;
    const int rectangles[][4]={{0,30,32,2},{3,5,9,7},{0,0,32,32}};
    for(unsigned c=0;c<3;++c) {
        layer->clip_x=rectangles[c][0];layer->clip_y=rectangles[c][1];
        layer->clip_width=rectangles[c][2];layer->clip_height=rectangles[c][3];
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);glDisable(GL_SCISSOR_TEST);
        glColorMask(1,1,1,1);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        native_ui_layer_composite();glReadBuffer(GL_BACK);
        glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,actual);
        for(int y=0;y<32;++y)for(int x=0;x<32;++x) {
            const int inside=x>=layer->clip_x && x<layer->clip_x+layer->clip_width &&
                y>=layer->clip_y && y<layer->clip_y+layer->clip_height;
            assert(actual[y*32+x]==(inside?UINT32_C(0xffff0000):UINT32_C(0xff0000ff)));
        }
        assert(glGetError()==GL_NO_ERROR);
    }
    if(layer->consumed)p_glDeleteSync(layer->consumed);
    glDeleteTextures(1,&layer->texture);memset(layer,0,sizeof(*layer));s_ui_front=-1;
    p_glDeleteProgram(s_ui_program);s_ui_program=0;
    p_glDeleteVertexArrays(1,&s_ui_vao);s_ui_vao=0;
    puts("UI composite: menu bar, popup rectangle and full-window bounds preserve outside pixels");
}

static void check_parallel_geometry(void) {
    const uint32_t triangles = 4097u;
    GlNativeGpuWork *work = calloc(1u, sizeof(*work));
    GlNativeGpuCommand *commands = calloc(5u, sizeof(*commands));
    GlNativeGpuMeshTriangle *mesh = calloc(triangles, sizeof(*mesh));
    assert(work && commands && mesh);
    work->scale = 2u; work->count = 5u; work->commands = commands;
    work->data = (uint8_t *)mesh;
    work->widths[0] = work->widths[1] = 128u;
    work->heights[0] = work->heights[1] = 97u;
    GpuRenderSemantic semantic = triangle(1u, 248u, 128u);
    assert(native_materialize_native_draw(&semantic, &commands[2].draw));
    for (uint32_t i = 0u; i < triangles; ++i) {
        mesh[i].triangle = commands[2].draw.primitive.triangles[0];
        mesh[i].triangle.vertices[0].x += (int32_t)(i % 7u) * 65536;
        mesh[i].texture.shading = i % 2u ? XG_RENDER_IR_SHADING_GOURAUD : XG_RENDER_IR_SHADING_FLAT;
        mesh[i].texture.semi_transparent = i % 3u == 0u;
    }
    commands[0] = (GlNativeGpuCommand){.kind = NATIVE_GPU_SEED, .plane = 0u, .w = 128, .h = 97};
    commands[1].kind = NATIVE_GPU_WORDS; /* Zero-sized prefix entry. */
    commands[2].kind = NATIVE_GPU_DRAW; commands[2].plane = 1u;
    commands[2].mesh_count = triangles;
    commands[3] = commands[2]; commands[3].mesh_count = 0u;
    commands[3].draw.topology = GPU_RENDER_SEMANTIC_LINES;
    commands[3].draw.line_count = 1u;
    commands[3].draw.lines[0].vertices[1].x = 64 * 65536;
    commands[3].draw.lines[0].vertices[1].y = 32 * 65536;
    commands[4] = (GlNativeGpuCommand){.kind = NATIVE_GPU_SPAN, .source = 0u,
        .plane = 1u, .w = 64, .h = 48, .sw = 64, .sh = 48};
    for (unsigned setup = 0u; setup < 2u; ++setup) {
        work->gpu_setup = setup;
        float (*reference)[GL_NATIVE_GPU_VERTEX_FLOATS] = NULL;
        uint32_t count;
        assert(native_gpu_build_vertex_slice(work, commands, work->data, 0u, 5u, &reference, &count));
        GlNativeGpuGeometry *geometry = native_gpu_geometry_prepare(work);
        assert(geometry && geometry->offsets[5].vertex == count);
        assert(memcmp(reference, geometry->vertices, (size_t)count * sizeof(*reference)) == 0);
        for (uint32_t c = 0u; c < 5u; ++c) {
            if (commands[c].kind != NATIVE_GPU_DRAW) continue;
            const uint32_t stride = commands[c].draw.topology == GPU_RENDER_SEMANTIC_LINES ? 6u : 3u;
            const GlNativeGpuGeometryOffset offset = geometry->offsets[c];
            const uint32_t primitives = geometry->offsets[c + 1u].primitive - offset.primitive;
            for (uint32_t t = 0u; t < primitives; ++t) {
                GlNativeGpuDrawGeometry expected = {0};
                native_gpu_prepare_draw(work, &commands[c], work->data, t, 128u, 97u,
                    reference + offset.vertex + t * stride, &expected);
                assert(memcmp(&expected, &geometry->primitives[offset.primitive + t], sizeof(expected)) == 0);
            }
        }
        native_gpu_geometry_free(geometry); free(reference);
    }
    work->widths[0] = work->heights[0] = 0u;
    assert(!native_gpu_geometry_prepare(work)); /* Invalid transfer uses owner fallback. */
    free(mesh); free(commands); free(work);
    puts("parallel geometry: one 4097-triangle mesh, lines and transfers match serial");
}

static void check_phase_vertex_ranges(void) {
    GlNativeMotionEndpoints endpoints = {.corner_count = 4099u, .range_count = 2u,
        .ranges = {{0u, 2000u}, {2007u, 4099u}}};
    endpoints.vertices = calloc(endpoints.corner_count, sizeof(*endpoints.vertices));
    endpoints.vertex_endpoints = calloc(endpoints.corner_count, sizeof(*endpoints.vertex_endpoints));
    GlNativeMotionEntity *entities = calloc(1u, sizeof(*entities));
    GlNativeMotionPhaseVertex *actual = malloc(endpoints.corner_count * sizeof(*actual));
    GlNativeMotionPhaseVertex *expected = malloc(endpoints.corner_count * sizeof(*expected));
    assert(endpoints.vertices && endpoints.vertex_endpoints && entities && actual && expected);
    memset(actual, 0x5a, endpoints.corner_count * sizeof(*actual));
    memset(expected, 0x5a, endpoints.corner_count * sizeof(*expected));
    GlNativePhaseVertexBatch serial = {.endpoints = &endpoints, .entities = entities,
        .phase_entities = entities, .out = expected, .offsets = {0u, 2000u, 4092u}};
    native_motion_phase_vertex_ranges(0u, 4092u, &serial);
    native_motion_phase_vertices(&endpoints, entities, entities, NULL, actual);
    assert(memcmp(actual, expected, endpoints.corner_count * sizeof(*actual)) == 0);
    free(actual); free(expected); free(entities);
    free(endpoints.vertices); free(endpoints.vertex_endpoints);
}

static void check_worker_scaling(void) {
    check_parallel_geometry();
    check_phase_vertex_ranges();
    const unsigned cpus = psx_host_available_cpu_count((unsigned)SDL_GetCPUCount());
    GlNativeRecipe recipe = {0};
    uint32_t offset = 0u;
    GlNativeMotionEndpoints endpoints = {.offsets = &offset};
    GlNativePhaseJob jobs[GL_NATIVE_MOTION_PHASE_CAPACITY] = {0};
    const uint32_t batches[] = {2u, 7u, GL_NATIVE_MOTION_PHASE_CAPACITY, 2u};
    unsigned peak = 0u;
    for (size_t b = 0u; b < sizeof(batches) / sizeof(*batches); ++b) {
        const uint32_t count = batches[b];
        for (uint32_t i = 0u; i < count; ++i) {
            endpoints.ranges[i][0] = endpoints.ranges[i][1] = UINT32_MAX;
            jobs[i] = (GlNativePhaseJob){.recipe = &recipe, .endpoints = &endpoints, .phase = i};
        }
        native_phase_run(jobs, count, NATIVE_PHASE_ENDPOINTS);
        const unsigned wanted = cpus < count ? cpus : count;
        if (wanted > peak) peak = wanted;
        assert(s_native_phase_pool.worker_count == peak);
        for (uint32_t i = 0u; i < count; ++i) {
            assert(jobs[i].completed);
            assert(endpoints.ranges[i][0] == 0u && endpoints.ranges[i][1] == 0u);
        }
    }
    native_phase_pool_stop();
    assert(!s_native_phase_pool.threads && !s_native_phase_pool.worker_count);
    thread_creations_before_failure = 0;
    assert(!native_phase_pool_start(7u));
    assert(!s_native_phase_pool.threads && !s_native_phase_pool.worker_count);
    native_phase_pool_stop();
    thread_creations_before_failure = 2;
    assert(native_phase_pool_start(7u));
    assert(s_native_phase_pool.worker_count == (cpus < 2u ? cpus : 2u));
    native_phase_run(jobs, 7u, NATIVE_PHASE_ENDPOINTS);
    for (uint32_t i = 0u; i < 7u; ++i) assert(jobs[i].completed);
    native_phase_pool_stop();
    thread_creations_before_failure = -1;

    /* Uneven, cropped stripes must reproduce ordered overlapping triangles. */
    GlNativeRecipeDraw records[2] = {0};
    records[0].semantic = triangle(1u, 248u, 0u);
    records[1].semantic = triangle(2u, 0u, 248u);
    records[1].semantic.material.semi_transparent = 1;
    recipe = (GlNativeRecipe){.width = 128u, .height = 97u,
        .clear_color = UINT32_C(0xff181008), .dithering_disabled = 1,
        .draws = records, .count = 2u};
    uint32_t *reference = NULL, *parallel = NULL;
    GlNativeRecipeStats stats = {0};
    assert(native_recipe_render_rows(&recipe, NULL, NULL, 0u, 0, &stats,
        5u, 89u, &reference, 0u, 89u, NULL, NULL, NULL, NULL, NULL));
    assert(native_recipe_render(&recipe, NULL, NULL, 0u, 0, NULL, 5u, 89u, &parallel));
    assert(memcmp(reference, parallel, (size_t)128u * 89u * sizeof(*reference)) == 0);
    assert(reference[32u * 128u + 32u] != recipe.clear_color);
    free(parallel); parallel = NULL;
    thread_creations_before_failure = 0;
    assert(native_recipe_render(&recipe, NULL, NULL, 0u, 0, NULL, 5u, 89u, &parallel));
    assert(memcmp(reference, parallel, (size_t)128u * 89u * sizeof(*reference)) == 0);
    thread_creations_before_failure = -1;
    free(reference); free(parallel);

    /* Canonical capture stays on the owner; reference bands own disjoint rows. */
    GlNativeCompileAudit *audit = calloc(1u, sizeof(*audit));
    GlNativeGpuWork *gpu = calloc(1u, sizeof(*gpu));
    uint32_t *pixels = calloc((size_t)128u * 97u, sizeof(*pixels));
    assert(audit && gpu && pixels);
    GlNativeResourceRecord resource = {0};
    resource.view.descriptor.width = 128u; resource.view.descriptor.height = 97u;
    GlNativeCpuSurface surface = {.resource = &resource, .pixels = pixels,
        .width = 128u, .height = 97u, .valid = 1};
    GlNativeCpuCompiler compiler = {.audit = audit, .gpu = gpu,
        .viewport_width = 128u, .viewport_height = 97u, .dithering_disabled = 1};
    XgSemanticDrawRecord draw;
    assert(native_materialize_native_draw(&records[0].semantic, &draw));
    assert(native_view_worker_draw(&compiler, &surface, &draw, 0u));
    assert(s_native_view_worker);
    const unsigned bands = cpus < 97u ? cpus : 97u;
    assert(s_native_view_worker->band_count == bands);
    assert(native_view_worker_finish(&compiler));
    assert(pixels[32u * 128u + 32u] != 0u);
    uint32_t *serial_pixels = calloc((size_t)128u * 97u, sizeof(*serial_pixels));
    assert(serial_pixels);
    GlNativeCpuSurface serial_surface = surface;
    serial_surface.pixels = serial_pixels;
    GlNativeCpuCompiler serial_compiler = compiler;
    serial_compiler.gpu = NULL;
    assert(native_render_draw(&serial_compiler, &serial_surface, &draw, 0u));
    assert(memcmp(pixels, serial_pixels, (size_t)128u * 97u * sizeof(*pixels)) == 0);
    /* Rebinding the retained workers must preserve their private audit owner. */
    assert(native_materialize_native_draw(&records[1].semantic, &draw));
    assert(native_view_worker_draw(&compiler, &surface, &draw, 1u));
    assert(native_view_worker_finish(&compiler));
    assert(native_render_draw(&serial_compiler, &serial_surface, &draw, 1u));
    assert(memcmp(pixels, serial_pixels, (size_t)128u * 97u * sizeof(*pixels)) == 0);
    native_view_worker_shutdown();
    native_gpu_free(gpu);
    free(audit); free(pixels); free(serial_pixels);
    printf("Native workers: %u CPUs, %u phase workers, %u raster bands; cropped stripes match serial\n",
        cpus, peak, bands);
}

static void check_motion_texture_identity(void) {
    GlNativeRecipeDraw before[2] = {0}, after[2] = {0};
    const uint32_t corners[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};
    for (uint32_t draw = 0u; draw < 2u; ++draw) {
        GpuRenderSemantic *s = &before[draw].semantic;
        *s = triangle(draw + 1u, 248u, 0u);
        s->screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_NONE;
        s->material.textured = 1u;
        s->interpolation_identity = (GpuRenderInterpolationIdentity){1u, 7u, draw + 100u, 1u};
        for (uint32_t v = 0u; v < 3u; ++v) {
            const uint32_t corner = corners[draw][v];
            GpuRenderSemanticVertex *p = &s->triangles[0].vertices[v];
            p->x = (16 + 32 * (corner & 1u)) * 65536;
            p->y = (16 + 32 * (corner >> 1u)) * 65536;
            p->native_view_x = p->x + 16384;
            p->native_view_y = p->y + 8192;
            p->native_view_position = 1u;
            p->interpolation_group_id = 7u;
            p->interpolation_vertex_id = corner;
            p->interpolation_vertex_identity_valid = 1u;
            /* Different faces can share source vertices across a UV seam. */
            p->u = (draw * 64u + corner * 8u) * 65536;
            p->v = corner * 8u * 65536;
        }
        after[draw] = before[draw];
        for (uint32_t v = 0u; v < 3u; ++v) {
            after[draw].semantic.triangles[0].vertices[v].x += 65536;
            after[draw].semantic.triangles[0].vertices[v].native_view_x += 32768;
        }
    }
    for (unsigned scenario = 0u; scenario < 8u; ++scenario) {
        GlNativeRecipeDraw prior[2] = {before[0], before[1]};
        GlNativeRecipeDraw now[2] = {after[0], after[1]};
        switch (scenario) {
        case 1u: prior[0] = before[1]; prior[1] = before[0]; break;
        case 2u: now[0] = after[1]; now[1] = after[0]; break;
        case 3u: now[0].semantic.triangles[0].vertices[0].u += 65536; break;
        case 4u: now[0].semantic.material.tpage ^= 1u; break;
        case 5u: now[0].semantic.interpolation_identity.primitive_id += 999u; break;
        case 6u: prior[1].semantic.interpolation_identity = prior[0].semantic.interpolation_identity; break;
        case 7u: prior[0].semantic.interpolation_identity.valid = 0u; break;
        }
        GlNativeRecipe previous = {.draws = prior, .count = 2u};
        GlNativeRecipe current = {.draws = now, .count = 2u};
        GlNativeVertexCache cache = {0};
        GlNativeVertexDiagnostics diag = {0};
        assert(native_motion_vertices(&previous, &current, 1u, &cache, &diag));
        assert(diag.matched == 4u && !diag.missing && !diag.conflicts);
        assert(diag.active_meshes == (scenario < 3u));
        assert(diag.discrete_meshes == (scenario >= 3u));
        if (scenario < 3u)
            for (uint32_t i = 0u; i < cache.pair_count; ++i) {
                assert(cache.deltas[i][0] == -32768);
                assert(cache.deltas[i][2] == -16384);
            }
        /* Selection must never rewrite the Native endpoint or round its XY. */
        for (uint32_t d = 0u; d < 2u; ++d)
            for (uint32_t v = 0u; v < 3u; ++v)
                assert((now[d].semantic.triangles[0].vertices[v].native_view_x & 65535) == 49152);
        native_motion_vertices_discard(&cache);
    }
    puts("motion textures: UV seams/OT reordering interpolate; cel changes and missing/ambiguous identities stay discrete");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--motion-only") == 0) {
        check_motion_texture_identity();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--workers-only") == 0) {
        assert(SDL_Init(0) == 0);
        check_worker_scaling();
        SDL_Quit();
        return 0;
    }
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
    check_scanout_resolve(argc==2 && strcmp(argv[1],"--scanout-benchmark")==0);
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
    for(unsigned scale=1;scale<=8;++scale)for(int gaps=0;gaps<2;++gaps)
        check_raster_equivalence(scale,gaps,scale==4 && argc==2 && strcmp(argv[1],"--raster-benchmark")==0);
    check_setup_selection();
    check_setup_batches(argc == 2 && strcmp(argv[1], "--setup-benchmark") == 0);
    check_motion_texture_identity();
    check_ui_clip();
    native_view_worker_shutdown();
    native_phase_pool_stop();
    SDL_DestroyCond(s_native_gpu_condition);
    SDL_DestroyMutex(s_native_state_mutex);
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
