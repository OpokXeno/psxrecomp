/* Window/presentation tests run without a game session, menus or netplay.
 * Rasterization, texture banks and Native resources use production code. */
#include "host_time.h"
#include "psx_rewind.h"
#include "psx_savestate_menu.h"
#include "frame_pacing.h"
#include <stdint.h>

uint64_t psx_get_cycle_count(void) { return 0; }
int psx_present_vsync_owns_cadence(void) { return 0; }
uint32_t psx_read_word(uint32_t address);
uint32_t psx_mod_read_word(uint32_t address) { return psx_read_word(address); }
uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t size, uint32_t alignment) {
    (void)size;
    (void)alignment;
    return 0;
}
int psx_rewind_needs_present(void) { return 0; }
int psx_savestate_menu_needs_present(void) { return 0; }
float psx_rewind_slide(void) { return 0.0f; }
int psx_rewind_overlay_image(const uint32_t **pixels, int *w, int *h) {
    if (pixels) *pixels = 0;
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
}
int psx_savestate_menu_overlay_image(const uint32_t **pixels, int *w, int *h) {
    return psx_rewind_overlay_image(pixels, w, h);
}
int present_shot_take(char *path, int size) {
    if (path && size > 0) path[0] = 0;
    return 0;
}
void present_shot_done(int ok) { (void)ok; }
