#include "cpu_state.h"
#include "debug_server.h"
#include "native_render_baseline.h"
#include "psx_fiber.h"
#include "psx_scheduler.h"
#include "xg_render_auth_runtime.h"

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
int g_native_render_baseline_armed;
int g_psx_icache_active;
int g_psx_load_delay;
volatile int g_ds_recording;
uint32_t g_psx_icache_tv[1024];
uint8_t *g_psx_ram;
uint64_t g_psx_cycle_fast_limit = UINT64_MAX;
uint64_t g_psx_bail_first;
uint64_t g_psx_bail_resolved;
uint64_t g_dispatch_static_hits;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint32_t g_dirty_ram_code_gen;
uint32_t g_ws_backdrop_lo;
uint32_t g_ws_backdrop_hi;
int g_ws_bd_from_interp;

extern uint8_t g_irq_cop2_test_ram[2u * 1024u * 1024u];

void native_render_baseline_observe_vblank_impl(void) {}

extern uint32_t i_stat;
extern uint64_t g_irq_cop2_test_device_edge_cycle;
extern uint32_t g_irq_cop2_test_device_edge_bit;
uint32_t g_xg_render_auth_cold_hook_count;
uint32_t g_xg_render_auth_cold_hook_kind;
uint32_t g_xg_render_auth_cold_hook_pc;
uint32_t g_xg_render_native_cutover_post_pc;
uint32_t g_xg_render_native_cutover_call_count;
uint32_t g_xg_render_native_cutover_observed_word;
uint32_t g_xg_render_auth_cold_hook_words[8];
uint32_t g_xg_render_auth_cold_hook_delays[8];
uint32_t g_xg_render_auth_cold_hook_kinds[8];
uint32_t g_xg_render_auth_cold_hook_pcs[8];
uint32_t g_xg_render_auth_source_operations[8];
uint32_t g_xg_render_auth_source_widths[8];
bool g_psx_xg_render_auth_cold_enabled = true;
static PsxXgRenderSourceSiteMetadata s_source_metadata;

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
void psx_icache_fetch_miss(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; }
uint8_t *memory_get_ram_ptr(void) { return g_irq_cop2_test_ram; }
int psx_load_delay_enabled(void) { return 0; }
uint32_t psx_cyc_load_word_slow(CPUState *cpu, uint32_t address,
                                uint32_t rt, uint32_t reg_mask) {
    (void)rt;
    (void)reg_mask;
    return cpu->read_word(address);
}
uint16_t psx_cyc_load_half_slow(CPUState *cpu, uint32_t address,
                                uint32_t rt, uint32_t reg_mask) {
    (void)rt;
    (void)reg_mask;
    return cpu->read_half(address);
}
uint8_t psx_cyc_load_byte(CPUState *cpu, uint32_t address,
                          uint32_t rt, uint32_t reg_mask) {
    (void)rt;
    (void)reg_mask;
    return cpu->read_byte(address);
}
uint32_t psx_cyc_lwc2_read(CPUState *cpu, uint32_t address) {
    return cpu->read_word(address);
}
int psx_idle_skip_is_enabled(void) { return 0; }
void psx_idle_note_check(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
void psx_bail_record(uint32_t site_ra, uint32_t site_sp,
                     uint32_t got_ra, uint32_t got_sp) {
    (void)site_ra;
    (void)site_sp;
    (void)got_ra;
    (void)got_sp;
}
int dirty_ram_is_dirty(uint32_t physical) { (void)physical; return 0; }
void psx_dispatch_call(CPUState *cpu, uint32_t address, uint32_t return_address) {
    (void)address;
    cpu->pc = return_address;
}
int overlay_loader_call_native(CPUState *cpu, uint32_t address) {
    (void)cpu;
    (void)address;
    return 0;
}
int psx_syscall(CPUState *cpu, uint32_t code) { (void)cpu; (void)code; return 0; }
void psx_break(CPUState *cpu, uint32_t code, uint32_t pc) {
    (void)cpu;
    (void)code;
    (void)pc;
}
void psx_muldiv_stall(CPUState *cpu) { (void)cpu; }
void psx_muldiv_set(CPUState *cpu, uint32_t latency) { (void)cpu; (void)latency; }
uint32_t psx_mult_latency_s(uint32_t value) { (void)value; return 1u; }
uint32_t psx_mult_latency_u(uint32_t value) { (void)value; return 1u; }
void psx_gte_stall(CPUState *cpu) { (void)cpu; }

int psx_ws_func_has_screen_cull(const uint32_t *words, int count) {
    (void)words;
    (void)count;
    return 0;
}
int psx_ws_cull_bltz_at(const uint32_t *words, int count, int index) {
    (void)words;
    (void)count;
    (void)index;
    return 0;
}
int psx_ws_backdrop_preload(void) { return 0; }
uint32_t psx_ws_backdrop_value(uint32_t original, int is_end, int window_cols) {
    (void)is_end;
    (void)window_cols;
    return original;
}
void psx_ws_backdrop_ring_note(uint32_t pc, int kind, int window_cols,
                               uint32_t original, uint32_t final_value,
                               int extent, int camera_x, int count,
                               uint32_t base, uint32_t display_list) {
    (void)pc;
    (void)kind;
    (void)window_cols;
    (void)original;
    (void)final_value;
    (void)extent;
    (void)camera_x;
    (void)count;
    (void)base;
    (void)display_list;
}
int psx_ws_is_cull_negsub_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_x_margin(void) { return 0; }
int psx_ws_auto_cull_on(void) { return 0; }
int psx_ws_cull_bltz(uint32_t value) { return (int32_t)value < 0; }
int psx_ws_is_cull_bias_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_depth_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_slti_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_w_imm(uint32_t immediate) { (void)immediate; return 0; }
int psx_ws_is_cull_vxrange_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_range_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_signed_x_bound_site(uint32_t pc, uint32_t instruction) {
    (void)pc;
    (void)instruction;
    return 0;
}
int psx_ws_is_cull_plane_nx_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_xclip_load_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_backdrop_site(uint32_t pc) { (void)pc; return 0; }
int32_t psx_ws_depth_bound(int32_t value) { return value; }
int32_t psx_ws_plane_nx(int32_t value) { return value; }
uint32_t psx_ws_xclip_bound(uint32_t value) { return value; }
int32_t psx_ws_player_x_bound(int32_t value) { return value; }
int psx_ws_cull_slti(uint32_t value, uint32_t immediate) {
    return (int32_t)value < (int32_t)immediate;
}
int psx_ws_cull_sltiu(uint32_t value, uint32_t immediate) {
    return value < immediate;
}
int psx_ws_cull_vxrange(uint32_t value, uint32_t immediate) {
    return value < immediate;
}
int psx_ws_backdrop_x(int value) { return value; }
int32_t psx_xenogears_field_frame_step(uint32_t pc, uint32_t instruction,
                                       int32_t vanilla_step,
                                       uint32_t field_state, uint32_t enabled) {
    (void)pc;
    (void)instruction;
    (void)field_state;
    (void)enabled;
    return vanilla_step;
}

void psx_xg_render_auth_cold_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                                  uint32_t instruction_word,
                                  uint32_t delay_slot_word) {
    (void)cpu;
    if (g_xg_render_auth_cold_hook_count < 8u) {
        const uint32_t index = g_xg_render_auth_cold_hook_count;
        g_xg_render_auth_cold_hook_kinds[index] = hook;
        g_xg_render_auth_cold_hook_pcs[index] = pc;
        g_xg_render_auth_cold_hook_words[index] = instruction_word;
        g_xg_render_auth_cold_hook_delays[index] = delay_slot_word;
    }
    g_xg_render_auth_cold_hook_count++;
    g_xg_render_auth_cold_hook_kind = hook;
    g_xg_render_auth_cold_hook_pc = pc;
}

bool psx_xg_render_auth_cold_hook_relevant(uint32_t hook, uint32_t pc,
                                           uint32_t instruction_word) {
    (void)hook;
    (void)pc;
    (void)instruction_word;
    return true;
}

bool psx_xg_render_auth_cold_source_pc_relevant(uint32_t pc) {
    const uint32_t physical = pc & UINT32_C(0x1fffffff);
    return physical == UINT32_C(0x00076858) ||
           physical == UINT32_C(0x000769c8) ||
           physical == UINT32_C(0x000769e4) ||
           physical == UINT32_C(0x000769ec) ||
           physical == UINT32_C(0x00076a08);
}

bool psx_xg_render_auth_native_ft4_bypass(
    CPUState *cpu, uint32_t pc, uint32_t instruction) {
    (void)instruction;
    if (pc == g_xg_render_native_cutover_post_pc) {
        ++g_xg_render_native_cutover_call_count;
        g_xg_render_native_cutover_observed_word =
            cpu->read_word(cpu->gpr[5]);
    }
    return false;
}

bool psx_xg_render_auth_native_cutover_pc_relevant(uint32_t pc) {
    (void)pc;
    return false;
}
bool psx_xg_render_auth_overlay_cutover_relevant(
    uint32_t pc, uint32_t instruction_word) {
    (void)pc;
    (void)instruction_word;
    return false;
}

bool psx_xg_render_auth_native_cutover_post_pc_relevant(uint32_t pc) {
    return pc == g_xg_render_native_cutover_post_pc;
}

bool psx_xg_render_auth_source_site_lookup(
    uint32_t pc, uint32_t instruction,
    PsxXgRenderSourceSiteMetadata *out_metadata) {
    PsxXgRenderSourceSiteMetadata metadata;

    if (pc == 0x80076858u && instruction == 0xe8590000u) {
        metadata = (PsxXgRenderSourceSiteMetadata){
            PSX_XG_RENDER_SOURCE_OPERATION_SWC2,
            PSX_XG_RENDER_SOURCE_AUXILIARY_EFFECTIVE_ADDRESS, 4u,
        };
    } else if (pc == 0x800769c8u && instruction == 0x0c0129efu) {
        metadata = (PsxXgRenderSourceSiteMetadata){
            PSX_XG_RENDER_SOURCE_OPERATION_CALL,
            PSX_XG_RENDER_SOURCE_AUXILIARY_NONE, 0u,
        };
    } else if (pc == 0x800769e4u && instruction == 0x8c630100u) {
        metadata = (PsxXgRenderSourceSiteMetadata){
            PSX_XG_RENDER_SOURCE_OPERATION_READ,
            PSX_XG_RENDER_SOURCE_AUXILIARY_EFFECTIVE_ADDRESS, 4u,
        };
    } else if (pc == 0x800769ecu && instruction == 0x00621007u) {
        metadata = (PsxXgRenderSourceSiteMetadata){
            PSX_XG_RENDER_SOURCE_OPERATION_BUCKET,
            PSX_XG_RENDER_SOURCE_AUXILIARY_RESULT_REGISTER, 0u,
        };
    } else if (pc == 0x80076a08u && instruction == 0xae040020u) {
        metadata = (PsxXgRenderSourceSiteMetadata){
            PSX_XG_RENDER_SOURCE_OPERATION_WRITE,
            PSX_XG_RENDER_SOURCE_AUXILIARY_EFFECTIVE_ADDRESS, 4u,
        };
    } else {
        return false;
    }
    s_source_metadata = metadata;
    *out_metadata = metadata;
    return true;
}

bool psx_xg_render_auth_cold_source_observe(
    PsxXgRenderSourceStage stage, uint32_t pc, uint32_t instruction,
    uint32_t auxiliary) {
    const uint32_t hook = stage == PSX_XG_RENDER_SOURCE_STAGE_PRE
        ? PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE
        : PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT;
    const uint32_t index = g_xg_render_auth_cold_hook_count;

    psx_xg_render_auth_cold_hook(NULL, hook, pc, instruction, auxiliary);
    if (index < 8u) {
        g_xg_render_auth_source_operations[index] = s_source_metadata.operation;
        g_xg_render_auth_source_widths[index] = s_source_metadata.width;
    }
    return true;
}

bool psx_xg_render_auth_cold_source_observe_cpu(
    CPUState *cpu, PsxXgRenderSourceStage stage, uint32_t pc,
    uint32_t instruction, uint32_t auxiliary) {
    (void)cpu;
    return psx_xg_render_auth_cold_source_observe(
        stage, pc, instruction, auxiliary);
}

bool psx_xg_render_auth_resident_ft4_observe(
    CPUState *cpu, uint32_t stage, uint32_t pc, uint32_t instruction) {
    (void)cpu;
    (void)stage;
    (void)pc;
    (void)instruction;
    return true;
}

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
