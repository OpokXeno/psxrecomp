#include "cpu_state.h"
#include "debug_server.h"
#include "psx_fiber.h"
#include "psx_scheduler.h"

#include <setjmp.h>
#include <stdint.h>

uint64_t psx_cycle_count;
uint64_t psx_next_service_cycle = UINT64_MAX;
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;
int g_psx_cyc_bb_defer;
int psx_in_device_service;
int g_event_step_conservative;
int g_idle_skip_enabled;
int g_psx_dispatch_depth;
int g_call_unit_depth;
int g_dma_exec_depth;
uint64_t g_psx_bail_anomaly;
jmp_buf g_scheduler_jmpbuf;
psx_sched_escape_t g_sched_escape;

extern uint32_t i_stat;
extern uint64_t g_irq_cop2_test_device_edge_cycle;
extern uint32_t g_irq_cop2_test_device_edge_bit;

uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
void psx_devices_service_to_now(void) {
    if (psx_cycle_count >= g_irq_cop2_test_device_edge_cycle) {
        i_stat |= 1u << g_irq_cop2_test_device_edge_bit;
        g_irq_cop2_test_device_edge_cycle = UINT64_MAX;
    }
    psx_next_service_cycle = UINT64_MAX;
}
void psx_advance_cycles_slow(uint32_t cycles) {
    psx_cycle_count += cycles;
    if (psx_next_service_cycle == 0u || psx_cycle_count >= psx_next_service_cycle)
        psx_devices_service_to_now();
}
void psx_icache_fetch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; }
int psx_idle_skip_is_enabled(void) { return 0; }
void psx_idle_note_check(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }

void event_ring_record(uint16_t kind, uint8_t detail) {
    (void)kind;
    (void)detail;
}

void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t auxiliary) {
    (void)kind;
    (void)detail;
    (void)auxiliary;
}

void device_trace_note(uint32_t source, uint32_t detail) {
    (void)source;
    (void)detail;
}

uint32_t sio_get_seq(void) { return 0; }
int sio_card_protocol_active(void) { return 0; }
void gpu_vblank_tick(void) {}
void savestate_poll(CPUState *cpu, uint32_t resume_pc) {
    (void)cpu;
    (void)resume_pc;
}

void debug_server_poll(void) {}
void debug_server_log_restore_event(uint32_t kind, uint32_t pc, uint32_t jump) {
    (void)kind;
    (void)pc;
    (void)jump;
}

void debug_server_log_thread_event(uint32_t kind, CPUState *cpu,
                                   uint32_t current_tcb, uint32_t target_tcb,
                                   uint32_t target_pc) {
    (void)kind;
    (void)cpu;
    (void)current_tcb;
    (void)target_tcb;
    (void)target_pc;
}

int psx_hle_scheduler_enabled(void) { return 0; }
uint32_t psx_sched_current_tcb(CPUState *cpu) { (void)cpu; return 0; }
void psx_sched_save_context(CPUState *cpu, uint32_t tcb, uint32_t pc) {
    (void)cpu;
    (void)tcb;
    (void)pc;
}

void psx_sched_set_current_tcb(CPUState *cpu, uint32_t tcb) {
    (void)cpu;
    (void)tcb;
}

int psx_is_dispatchable(uint32_t pc) { return pc != 0; }
int overlay_loader_shadow_native_thread_switch_bail(void) { return 0; }
int dma_cdrom_transfer_active(void) { return 0; }
psx_fiber_t psx_fiber_current(void) { return NULL; }
int gpu_ws_present_native_43(void) { return 0; }
void psx_ws_note_gte_project(int count) { (void)count; }
