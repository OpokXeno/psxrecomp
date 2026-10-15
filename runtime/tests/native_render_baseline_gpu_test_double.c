#include "native_render_baseline.h"
#include "mod_memory.h"
#include "hd_texture_runtime.h"
#include "xg_render_source_commit.h"

int g_native_render_baseline_armed;

void native_render_baseline_note_material(
        const NativeRenderBaselineMaterialObservation *observation) {
    (void)observation;
}

uint32_t psx_mod_gpu_dma_resolve_address(uint32_t address) {
    return psx_mod_gpu_dma_resolve_address_for(address, 0u);
}

/* Focused GPU tests never enter frontend vblank, netplay, IRQ-resume, or card
 * service paths. Keep those policy gates neutral without linking the frontend. */
int psx_get_in_exception(void) { return 0; }
int psx_netplay_active(void) { return 0; }
int sio_hold_present_for_card(void) { return 0; }
uint32_t psx_compiled_irq_resume_pc(void) { return 0u; }
uint32_t psx_last_irq_check_pc(void) { return 0u; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0u; }
void mod_runtime_on_vblank(void) {}
void sio_ape_card_unstick_pump(void) {}

uint32_t memory_get_ram_size(void) { return UINT32_C(0x00200000); }
bool xg_render_submission_temporal_binding(
        const GpuRenderSemantic *semantic, XgRenderTemporalBinding *out) {
    (void)semantic; (void)out;
    return false;
}

/* These renderer fixtures have neither an external texture pack nor UI
 * telemetry. Keep those services absent while testing the real GL owners. */
int hd_texture_runtime_active(void) { return 0; }
HdTexturePack *hd_texture_runtime_pack(void) { return NULL; }
uint32_t hd_texture_runtime_generation(void) { return 0u; }
int hd_texture_runtime_resolve(uint16_t px, uint16_t py, uint8_t depth,
        uint16_t cx, uint16_t cy, const int lim[4], uint8_t mx, uint8_t my,
        uint8_t ox, uint8_t oy, GpuRenderHdTexture *out) {
    (void)px; (void)py; (void)depth; (void)cx; (void)cy; (void)lim;
    (void)mx; (void)my; (void)ox; (void)oy; (void)out;
    return 0;
}
int hd_texture_pack_acquire_decoded(HdTexturePack *pack, uint32_t texture,
        uint32_t palette, HdTexturePixels *out) {
    (void)pack; (void)texture; (void)palette; (void)out;
    return HD_TEXTURE_LOOKUP_NONE;
}
int hd_texture_pack_request_decode(HdTexturePack *pack, uint32_t texture, uint32_t palette) {
    (void)pack; (void)texture; (void)palette;
    return HD_TEXTURE_LOOKUP_NONE;
}
void hd_texture_pack_forget_decoded(HdTexturePack *pack, uint32_t texture, uint32_t palette) {
    (void)pack; (void)texture; (void)palette;
}
void hd_texture_pixels_release(HdTexturePixels *pixels) { (void)pixels; }
void psx_debug_overlay_present_observe(void) {}
void psx_debug_overlay_post_swap(int swapped) { (void)swapped; }
