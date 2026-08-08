#include "audio_trace.h"
#include "card_data_writes.h"
#include "cdrom.h"
#include "crash_trace.h"
#include "data_shards.h"
#include "debug_server.h"
#include "dirty_ram_interp.h"
#include "event_ring.h"
#include "fntrace.h"
#include "cpu_state.h"
#include "gpu_render.h"
#include "guest_render_bridge.h"
#include "interrupts.h"
#include "latency_ring.h"
#include "lockstep.h"
#include "mdec.h"
#include "native_render_baseline.h"
#include "overlay_loader.h"
#include "parity_trace.h"
#include "psx_bios_image.h"
#include "psx_cycles.h"
#include "sio.h"
#include "spu.h"
#include "text_xlate.h"
#include "timers.h"

#include <stdint.h>

int g_exec_phase;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint64_t s_frame_count;
int g_ls_mode;
int g_ls_suppress_record;
volatile int g_ds_recording;
uint64_t psx_cycle_count;
void (*g_overlay_flush_pending_cycles)(void);
CPUState *debug_cpu_ptr;
uint64_t test_mmio_sync_calls;
uint64_t test_irq_raise_calls;
uint64_t test_baseline_begin_calls;
uint64_t test_baseline_node_calls;
uint64_t test_baseline_word_count;
uint64_t test_baseline_end_calls;
NativeRenderBaselineOtStatus test_baseline_end_status;
uint32_t g_dirty_ram_exec_pc_counts[DIRTY_RAM_EXEC_WORD_COUNT];

void guest_render_bridge_force_original(GuestRenderFallbackReason reason) {
    (void)reason;
}

void psx_xg_render_auth_capture_clear_tile(CPUState *cpu) { (void)cpu; }
void psx_xg_render_auth_capture_tile_write(
        CPUState *cpu, uint32_t command_address, uint32_t writer_pc,
        uint8_t color) {
    (void)cpu;
    (void)command_address;
    (void)writer_pc;
    (void)color;
}
void psx_xg_render_auth_capture_model_ft3_link(CPUState *cpu) { (void)cpu; }

#ifdef GPU_ATOMIC_CHECKPOINT_TESTING
void psx_xg_render_auth_note_code_write(uint64_t previous_generation,
                                        uint64_t next_generation,
                                        uint32_t address, uint32_t size) {
    (void)previous_generation;
    (void)next_generation;
    (void)address;
    (void)size;
}
#endif

uint32_t debug_guest_ra(void) { return 0u; }
uint32_t debug_guest_sp(void) { return 0u; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind;
    (void)detail;
    (void)aux;
}
void gte_precision_tracking_set(int enabled) { (void)enabled; }
void gte_precision_invalidate_word(uint32_t addr) { (void)addr; }
int gte_geometry_correction_enabled(void) { return 0; }
int gte_geometry_correction_lookup(uint32_t packed, int32_t *x16, int32_t *y16) {
    (void)packed;
    (void)x16;
    (void)y16;
    return 0;
}
int gte_precision_load_word(uint32_t addr, uint32_t packed, int32_t *x16,
                            int32_t *y16, uint16_t *z) {
    (void)addr;
    (void)packed;
    (void)x16;
    (void)y16;
    (void)z;
    return 0;
}
int mdec_recently_active(uint32_t within_frames) {
    (void)within_frames;
    return 0;
}
int psx_get_in_exception(void) { return 0; }
void psx_devices_mmio_sync(void) { test_mmio_sync_calls++; }
void psx_irq_raise(uint32_t bit, uint32_t detail) {
    (void)bit;
    (void)detail;
    test_irq_raise_calls++;
}
void psx_fatal_halt(const char *reason) { (void)reason; }
void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width) {
    (void)addr;
    (void)val;
    (void)width;
}
void debug_server_trace_mmio_read(uint32_t addr, uint32_t val, uint8_t width) {
    (void)addr;
    (void)val;
    (void)width;
}
void timers_write(uint32_t addr, uint32_t value) {
    (void)addr;
    (void)value;
}
uint32_t timers_read(uint32_t addr) {
    (void)addr;
    return 0u;
}
void sio_write(uint32_t addr, uint32_t value) {
    (void)addr;
    (void)value;
}
uint32_t sio_read(uint32_t addr) {
    (void)addr;
    return 0u;
}
void cdrom_write(uint32_t addr, uint32_t value) {
    (void)addr;
    (void)value;
}
uint32_t cdrom_read(uint32_t addr) {
    (void)addr;
    return 0u;
}
void mdec_write(uint32_t addr, uint32_t value) {
    (void)addr;
    (void)value;
}
uint32_t mdec_read(uint32_t addr) {
    (void)addr;
    return 0u;
}
void spu_write(uint32_t addr, uint32_t value) {
    (void)addr;
    (void)value;
}
uint32_t spu_read(uint32_t addr) {
    (void)addr;
    return 0u;
}
void ds_note_read(uint32_t addr, uint32_t size) {
    (void)addr;
    (void)size;
}
void ds_note_write(uint32_t addr, uint32_t size) {
    (void)addr;
    (void)size;
}
void ds_note_dma_write(void) {}
uint32_t ls_read_hook(uint32_t addr, int size, uint32_t real_val) {
    (void)addr;
    (void)size;
    (void)real_val;
    return 0u;
}
void ls_write_hook(uint32_t addr, int size, uint32_t val) {
    (void)addr;
    (void)size;
    (void)val;
}
void text_xlate_vram_upload(int x, int y, int w, int h) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
void latency_ring_mark(LatencyStage stage) { (void)stage; }
void native_render_baseline_ot_begin(uint32_t list_address) {
    (void)list_address;
    test_baseline_begin_calls++;
}
void native_render_baseline_ot_node(const NativeRenderBaselineOtNode *node) {
    test_baseline_node_calls++;
    if (node) test_baseline_word_count += node->packet_words;
}
void native_render_baseline_ot_end(NativeRenderBaselineOtStatus status) {
    test_baseline_end_calls++;
    test_baseline_end_status = status;
}
void native_render_baseline_note_material(
        const NativeRenderBaselineMaterialObservation *observation) {
    (void)observation;
}
const GpuRenderBackend *vk_backend_get(void) { return 0; }
const GpuRenderBackend *gl_backend_get(void) { return 0; }
int g_native_render_baseline_armed;
void cdrom_debug_snapshot(CDROMDebugState *out) { (void)out; }
uint32_t cdrom_dma_sector_word_count(void) { return 0u; }
void mdec_debug_dma_in_start(uint32_t addr, uint32_t words) {
    (void)addr;
    (void)words;
}
void mdec_debug_dma_out_start(uint32_t addr, uint32_t words) {
    (void)addr;
    (void)words;
}
void mdec_debug_dma_in_end(uint32_t addr, uint32_t words) {
    (void)addr;
    (void)words;
}
void mdec_debug_dma_out_end(uint32_t addr, uint32_t words) {
    (void)addr;
    (void)words;
}
void mdec_dma_write_word(uint32_t value) { (void)value; }
uint32_t mdec_dma_read_word(void) { return 0u; }
uint32_t mdec_dma_write_words(const uint32_t *src, uint32_t max_words) {
    (void)src;
    return max_words;
}
int mdec_dma_write_ready(void) { return 0; }
int mdec_dma_read_ready(void) { return 0; }
int cdrom_get_setloc_lba(void) { return -1; }
uint32_t cdrom_dma_read(void) { return 0u; }
int cdrom_dma_ready(void) { return 0; }
void overlay_capture_on_dma(uint32_t load_addr, uint32_t size,
                            const uint8_t *bytes) {
    (void)load_addr;
    (void)size;
    (void)bytes;
}
void overlay_capture_before_dma(uint32_t load_addr, uint32_t size) {
    (void)load_addr;
    (void)size;
}
void spu_dma_write(uint32_t word) { (void)word; }
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) {
    (void)kind;
    (void)a;
    (void)b;
}
uint32_t psx_bios_kernel_body_count;
const PsxKernelBody *psx_bios_kernel_bodies;
uint32_t g_dirty_ram_exec_page_bitmap[16];
uint32_t g_dirty_ram_exec_pc_bitmap[(2u * 1024u * 1024u / 4u + 31u) / 32u];
uint32_t g_dirty_ram_dispatch_pc_bitmap[(2u * 1024u * 1024u / 4u + 31u) / 32u];
void overlay_loader_note_code_write(void) {}
void overlay_loader_active_write_check(uint32_t phys, uint32_t size) {
    (void)phys;
    (void)size;
}
void sio_tick(int cycles) { (void)cycles; }
int g_ram_read_watch_active;
void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t val) {
    (void)phys;
    (void)val;
}
int fntrace_is_game_started(void) { return 0; }
void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                    uint32_t new_val, uint8_t width) {
    (void)phys;
    (void)old_val;
    (void)new_val;
    (void)width;
}
void parity_trace_note_write(uint32_t addr, uint32_t width, uint32_t writer_pc) {
    (void)addr;
    (void)width;
    (void)writer_pc;
}
int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width) {
    (void)phys;
    (void)value;
    (void)width;
    return 0;
}
int g_ws_bd_stretch_on;
int g_ws_bd_stretch_pct;
