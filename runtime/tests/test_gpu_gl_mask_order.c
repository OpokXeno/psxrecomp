#include "gpu.h"
#include "gpu_gl_renderer.h"
#include "gpu_render.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>

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

static void reset_gpu_for_case(void) {
    gpu_init();
    gpu_write_gp0(0xe3000000u);
    gpu_write_gp0(0xe407ffffu);
}

static int read_fbo_pixel(int x, int y, uint16_t *pixel) {
    return gl_renderer_fbo_peek(x, y, 1, 1, pixel);
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

int main(void) {
    SDL_Window *window;

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
        test_flat_batch_uses_submission_mask_check();
        test_textured_batch_uses_submission_mask_check();
    }

    gl_renderer_shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) return 1;
    puts("PASS: delayed OpenGL mask-check batches preserve GP0(E6) submission state");
    return 0;
}
