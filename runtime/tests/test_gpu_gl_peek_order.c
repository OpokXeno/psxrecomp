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
    BLACK_1555 = 0x0000,
    WHITE_1555 = 0x7fff,
    DOT_X = 300,
    DOT_Y = 300,
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

static void test_fbo_peek_lands_prior_gp0_primitives(void) {
    uint16_t pixel = 0;

    /* Given: an uploaded black destination with GP0(E6) destination checking disabled. */
    reset_gpu_for_case();
    gpu_write_gp0(0xe6000000u);
    gr_vram_write(DOT_X, DOT_Y, BLACK_1555);
    gl_renderer_flush_cpu_uploads();
    expect_true(gl_renderer_fbo_peek(DOT_X, DOT_Y, 1, 1, &pixel),
                "setup reads the OpenGL FBO");
    expect_pixel(pixel, BLACK_1555, "setup uploads the black destination to OpenGL");

    /* When: a white GP0 flat dot is submitted without an explicit batch flush. */
    gpu_write_gp0(0x68ffffffu);
    gpu_write_gp0(gp0_xy(DOT_X, DOT_Y));

    /* Then: forensic FBO readback observes the preceding primitive as white 1555. */
    expect_true(gl_renderer_fbo_peek(DOT_X, DOT_Y, 1, 1, &pixel),
                "forensic readback reads the OpenGL FBO");
    expect_pixel(pixel, WHITE_1555,
                 "FBO peek lands all GP0 work before reading the destination");
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
    window = SDL_CreateWindow("gpu_gl_peek_order_test", SDL_WINDOWPOS_UNDEFINED,
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
    if (!failures)
        test_fbo_peek_lands_prior_gp0_primitives();

    gl_renderer_shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) return 1;
    puts("PASS: OpenGL FBO peek preserves GP0 primitive ordering");
    return 0;
}
