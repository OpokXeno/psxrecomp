#include "native_render_baseline.h"
#include "mod_memory.h"

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
