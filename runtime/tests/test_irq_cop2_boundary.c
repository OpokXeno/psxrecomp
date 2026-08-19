#include "cpu_state.h"
#include "interrupts.h"
#include "overlay_api.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/dirty_ram_interp.c"

enum {
    COP0_SR = 12,
    COP0_CAUSE = 13,
    COP0_EPC = 14,
    TEST_IRQ = IRQ_VBLANK,
    CAUSE_IP0 = 1u << 8,
    CAUSE_IP2 = 1u << 10,
};

static const uint32_t kCop2Pc = 0x80012000u;
static const uint32_t kNclipInsn = 0x4A000006u;
static const uint32_t kStaleMac0 = 0x13579BDFu;
static const uint32_t kExpectedMac0 = 19u;
static const uint32_t kVariantSwc2Pc = 0x80076858u;
static const uint32_t kVariantSwc2Insn = 0xE8590000u;
static const uint32_t kVariantCallPc = 0x800769C8u;
static const uint32_t kVariantCallInsn = 0x0C0129EFu;
static const uint32_t kVariantReadPc = 0x800769E4u;
static const uint32_t kVariantReadInsn = 0x8C630100u;
static const uint32_t kVariantBucketPc = 0x800769ECu;
static const uint32_t kVariantBucketInsn = 0x00621007u;
static const uint32_t kVariantWritePc = 0x80076A08u;
static const uint32_t kVariantWriteInsn = 0xAE040020u;
static const uint32_t kModelPostSwc2Pc = 0x8004A1ACu;
static const uint32_t kModelPostSwc2Insn = 0xE8B60000u;
static const uint32_t kSourcePreHook = PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE;
static const uint32_t kSourceCommitHook = PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT;

void psx_irq_set_cause_ptr(uint32_t *cause);

uint8_t g_irq_cop2_test_ram[2u * 1024u * 1024u];
#define s_ram g_irq_cop2_test_ram
static uint32_t s_handler_epc;
static uint32_t s_handler_gte_issue_count;
static uint32_t s_bios_saved_epc;
static uint32_t s_gte_issue_count;
static uint64_t s_gte_issue_cycle;
static uint64_t s_handler_gte_deadline;
static uint32_t s_handler_cause;

extern uint32_t g_xg_render_auth_cold_hook_count;
extern uint32_t g_xg_render_auth_cold_hook_kind;
extern uint32_t g_xg_render_auth_cold_hook_pc;
extern uint32_t g_xg_render_auth_cold_hook_words[8];
extern uint32_t g_xg_render_auth_cold_hook_delays[8];
extern uint32_t g_xg_render_auth_cold_hook_kinds[8];
extern uint32_t g_xg_render_auth_cold_hook_pcs[8];
extern uint32_t g_xg_render_auth_source_operations[8];
extern uint32_t g_xg_render_auth_source_widths[8];
extern uint32_t g_xg_render_native_cutover_post_pc;
extern uint32_t g_xg_render_native_cutover_call_count;
extern uint32_t g_xg_render_native_cutover_observed_word;

uint64_t g_irq_cop2_test_device_edge_cycle = UINT64_MAX;
uint32_t g_irq_cop2_test_device_edge_bit;

uint32_t i_stat;
uint32_t i_mask;
int g_psx_call_bail;
uint64_t s_frame_count;
uint32_t *g_psx_cyc_local_acc;
int g_overlay_capture_private_execution_enabled;

void overlay_capture_private_note_execution(uint32_t pc) { (void)pc; }

void psx_pgxp_load(CPUState *cpu, uint32_t insn, uint32_t address,
                   uint32_t value) {
    (void)cpu;
    (void)insn;
    (void)address;
    (void)value;
}

void psx_pgxp_store(CPUState *cpu, uint32_t insn, uint32_t address,
                    uint32_t value) {
    (void)cpu;
    (void)insn;
    (void)address;
    (void)value;
}

void psx_pgxp_alu(CPUState *cpu, uint32_t insn, uint32_t result,
                  uint32_t source1, uint32_t source2) {
    (void)cpu;
    (void)insn;
    (void)result;
    (void)source1;
    (void)source2;
}

void psx_pgxp_muldiv(CPUState *cpu, uint32_t insn, uint32_t hi,
                     uint32_t lo, uint32_t source1, uint32_t source2) {
    (void)cpu;
    (void)insn;
    (void)hi;
    (void)lo;
    (void)source1;
    (void)source2;
}

void psx_pgxp_cop2(CPUState *cpu, uint32_t insn, uint32_t value,
                   uint32_t address) {
    (void)cpu;
    (void)insn;
    (void)value;
    (void)address;
}

void pgxp_gte_push_sxy(int32_t x16, int32_t y16, uint16_t sz3,
                       uint32_t packed) {
    (void)x16;
    (void)y16;
    (void)sz3;
    (void)packed;
}

int pgxp_get_gte_sxy(uint32_t index, int32_t *x16, int32_t *y16) {
    (void)index;
    (void)x16;
    (void)y16;
    return 0;
}

void pgxp_gte_reg_written(int reg, uint32_t value) {
    (void)reg;
    (void)value;
}

void pgxp_store_gte_reg(uint32_t address, uint8_t reg) {
    (void)address;
    (void)reg;
}

int gpu_ws_precise_nclip_enabled(void) { return 0; }

PsxWsCullSemantic psx_ws_semantic_cull_site(uint32_t pc) {
    (void)pc;
    return PSX_WS_CULL_SEMANTIC_NONE;
}

int psx_ws_cull_keep_site(uint32_t pc, uint32_t insn, uint32_t vanilla,
                          uint32_t *out) {
    (void)pc;
    (void)insn;
    (void)vanilla;
    (void)out;
    return 0;
}

int psx_ws_angle_site(uint32_t pc, uint32_t insn, uint32_t *out) {
    (void)pc;
    (void)insn;
    (void)out;
    return 0;
}

int psx_ws_aspect_cone_site(CPUState *cpu, uint32_t pc, uint32_t insn,
                            uint32_t vanilla, uint32_t *out) {
    (void)cpu;
    (void)pc;
    (void)insn;
    (void)vanilla;
    (void)out;
    return 0;
}

uint32_t psx_ws_guest_cull_screen_bias(uint32_t value, int32_t immediate) {
    return value + (uint32_t)immediate;
}

int psx_ws_guest_cull_world_range(uint32_t value, int32_t immediate) {
    return value < (uint32_t)immediate;
}

uint32_t psx_ws_guest_cull_left_edge(uint32_t bound) { return 0u - bound; }

int psx_ws_guest_cull_masked_screen_x(uint32_t x, uint32_t bound) {
    const uint32_t signed_bound =
        (uint32_t)(int32_t)(int16_t)(uint16_t)bound;
    return (x & 0xffffu) < signed_bound;
}

int32_t psx_ws_guest_cull_frustum_plane_x(int32_t nx) { return nx; }

int psx_ws_guest_cull_signed_screen_x(int32_t value, int32_t immediate) {
    return value < immediate;
}

int psx_ws_guest_cull_depth_signed(int32_t value, int32_t immediate) {
    return value < immediate;
}

int psx_ws_guest_cull_depth_unsigned(uint32_t value, int32_t immediate) {
    return value < (uint32_t)immediate;
}

uint32_t psx_ws_guest_cull_xclip_bound(uint32_t vanilla) { return vanilla; }
int psx_ws_activation_margin(void) { return 0; }
int psx_ws_is_cull_slti_lower_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_cull_slti_lower(uint32_t value, uint32_t immediate) {
    return (int32_t)value < (int32_t)(int16_t)(uint16_t)immediate;
}

uint32_t psx_xg_render_auth_cold_instruction_flags(
    uint32_t pc, uint32_t instruction) {
    (void)pc;
    (void)instruction;
    return 0u;
}

void psx_post_load_grace_tick(void) {}
int psx_netplay_active(void) { return 0; }
int psx_selfcheck_enabled(void) { return 0; }
int psx_scheduler_top_level_resume_active(void) { return 0; }
void gpu_vblank_flush_present(void) {}
void sio_ape_card_unstick_pump(void) {}

void psx_netplay_poll_snap(CPUState *cpu, uint32_t resume_pc) {
    (void)cpu;
    (void)resume_pc;
}

void psx_selfcheck_poll(CPUState *cpu, uint32_t resume_pc) {
    (void)cpu;
    (void)resume_pc;
}

void psx_rewind_poll(CPUState *cpu, uint32_t resume_pc) {
    (void)cpu;
    (void)resume_pc;
}

static uint32_t test_read_word(uint32_t address) {
    uint32_t physical = address & 0x1FFFFFFFu;
    uint32_t value = 0;
    if (physical + sizeof(value) <= sizeof(s_ram))
        memcpy(&value, &s_ram[physical], sizeof(value));
    return value;
}

static void test_write_word(uint32_t address, uint32_t value) {
    uint32_t physical = address & 0x1FFFFFFFu;
    if (physical + sizeof(value) <= sizeof(s_ram))
        memcpy(&s_ram[physical], &value, sizeof(value));
}

static uint16_t test_read_half(uint32_t address) {
    return (uint16_t)test_read_word(address);
}

static void test_write_half(uint32_t address, uint16_t value) {
    test_write_word(address, value);
}

static uint8_t test_read_byte(uint32_t address) {
    return (uint8_t)test_read_word(address);
}

static void test_write_byte(uint32_t address, uint8_t value) {
    test_write_word(address, value);
}

uint32_t psx_read_word(uint32_t address) { return test_read_word(address); }
int psx_fetch_instruction_word_raw(uint32_t address, uint32_t *instruction) {
    if ((address & 3u) != 0u || instruction == NULL) return 0;
    *instruction = test_read_word(address);
    return 1;
}

void psx_dispatch(CPUState *cpu, uint32_t target) {
    (void)target;
    s_handler_epc = cpu->cop0[COP0_EPC];
    s_handler_cause = cpu->cop0[COP0_CAUSE];
    s_handler_gte_issue_count = s_gte_issue_count;
    s_handler_gte_deadline = cpu->gte_ts_done;
    uint32_t instruction = cpu->read_word(s_handler_epc);
    s_bios_saved_epc = s_handler_epc;
    if ((instruction & 0xFE000000u) == 0x4A000000u)
        s_bios_saved_epc += 4u;
    i_stat &= ~(1u << TEST_IRQ);
    cpu->pc = s_bios_saved_epc;
    g_exc_escape_reason = PSX_EXC_ESCAPE_RFE_RETURN;
}

void psx_gte_set(CPUState *cpu, uint32_t latency) {
    if (cpu->gte_ts_done > psx_cycle_count)
        psx_advance_cycles((uint32_t)(cpu->gte_ts_done - psx_cycle_count));
    s_gte_issue_count++;
    s_gte_issue_cycle = psx_cycle_count;
    cpu->gte_ts_done = psx_cycle_count + latency;
}

uint32_t psx_gte_cmd_latency(uint32_t command) {
    (void)command;
    return 7u;
}

void psx_gte_read(CPUState *cpu, uint32_t target_register) {
    (void)cpu;
    (void)target_register;
}

static void seed_gte(CPUState *cpu) {
    cpu->gte_data[12] = 0x00000000u;
    cpu->gte_data[13] = 0x00010004u;
    cpu->gte_data[14] = 0x00050001u;
    cpu->gte_data[24] = kStaleMac0;
    cpu->ld_which_t = 0x20u;
    cpu->read_fudge = 0x20u;
}

static void prepare_irq_case(CPUState *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->read_word = test_read_word;
    cpu->write_word = test_write_word;
    cpu->read_half = test_read_half;
    cpu->write_half = test_write_half;
    cpu->read_byte = test_read_byte;
    cpu->write_byte = test_write_byte;
    seed_gte(cpu);
    test_write_word(kCop2Pc, kNclipInsn);

    interrupts_init();
    psx_irq_set_cause_ptr(&cpu->cop0[COP0_CAUSE]);
    psx_cycle_count = 0;
    s_gte_issue_count = 0;
    s_gte_issue_cycle = UINT64_MAX;
    s_handler_gte_deadline = 0;
    s_handler_cause = 0;
    g_irq_cop2_test_device_edge_cycle = UINT64_MAX;
    g_irq_cop2_test_device_edge_bit = 0;
    psx_next_service_cycle = UINT64_MAX;
    i_stat = 1u << TEST_IRQ;
    i_mask = 1u << TEST_IRQ;
    cpu->cop0[COP0_SR] = 0x00400401u;
    cpu->pc = kCop2Pc;
}

static int test_compiled_return_emits_auth_hook(void) {
    CPUState cpu = {0};
    uint32_t next_pc = 0;

    g_xg_render_auth_cold_hook_count = 0u;
    g_xg_render_auth_cold_hook_kind = 0u;
    g_xg_render_auth_cold_hook_pc = 0u;
    (void)psx_check_interrupts_at(&cpu, kCop2Pc + 8u);
    cpu.pc = 0x80001234u;

    if (dirty_ram_finish_call_return(&cpu, kCop2Pc + 8u, &next_pc) != 0) {
        fprintf(stderr, "HARNESS FAIL: compiled return seam reported failure\n");
        return 0;
    }
    if (g_xg_render_auth_cold_hook_count != 1u ||
        g_xg_render_auth_cold_hook_kind != PSX_XG_RENDER_AUTH_HOOK_RETURN ||
        g_xg_render_auth_cold_hook_pc != kCop2Pc + 8u ||
        next_pc != kCop2Pc + 8u || cpu.pc != 0x80001234u) {
        fprintf(stderr,
                "FAIL: return hook count=%u kind=%u pc=%08X next=%08X cpu=%08X\n",
                g_xg_render_auth_cold_hook_count,
                g_xg_render_auth_cold_hook_kind,
                g_xg_render_auth_cold_hook_pc,
                next_pc,
                cpu.pc);
        return 0;
    }
    return 1;
}

static void reset_source_observations(void) {
    g_xg_render_auth_cold_hook_count = 0u;
    memset(g_xg_render_auth_cold_hook_words, 0,
           sizeof(g_xg_render_auth_cold_hook_words));
    memset(g_xg_render_auth_cold_hook_delays, 0,
           sizeof(g_xg_render_auth_cold_hook_delays));
    memset(g_xg_render_auth_cold_hook_kinds, 0,
           sizeof(g_xg_render_auth_cold_hook_kinds));
    memset(g_xg_render_auth_cold_hook_pcs, 0,
           sizeof(g_xg_render_auth_cold_hook_pcs));
    memset(g_xg_render_auth_source_operations, 0,
           sizeof(g_xg_render_auth_source_operations));
    memset(g_xg_render_auth_source_widths, 0,
           sizeof(g_xg_render_auth_source_widths));
}

static void prepare_source_cpu(CPUState *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    g_psx_ram = s_ram;
    cpu->read_word = test_read_word;
    cpu->write_word = test_write_word;
    cpu->read_half = test_read_half;
    cpu->write_half = test_write_half;
    cpu->read_byte = test_read_byte;
    cpu->write_byte = test_write_byte;
}

static int test_runtime_operations_emit_exact_source_observation_pairs(void) {
    CPUState cpu = {0};
    uint32_t next_pc = 0u;
    const uint32_t read_address = 0x80010100u;
    const uint32_t write_address = 0x80010220u;
    const uint32_t swc2_address = 0x80010300u;
    const uint32_t bucket_result = 0xC0000000u;

    memset(s_ram, 0, sizeof(s_ram));
    prepare_source_cpu(&cpu);
    reset_source_observations();

    cpu.gpr[3] = 0x80010000u;
    test_write_word(read_address, 0x80020000u);
    if (exec_one_fetched_observed(&cpu, kVariantReadPc & 0x1fffffffu,
                                  kVariantReadInsn,
                                  PSX_XG_RENDER_COLD_SOURCE, &next_pc) != 0 ||
        cpu.gpr[3] != 0x80020000u) {
        fprintf(stderr, "HARNESS FAIL: selected read did not execute\n");
        return 0;
    }

    cpu.gpr[16] = 0x80010200u;
    cpu.gpr[4] = 0x12345678u;
    if (exec_one_fetched_observed(&cpu, kVariantWritePc, kVariantWriteInsn,
                                  PSX_XG_RENDER_COLD_SOURCE, &next_pc) != 0 ||
        test_read_word(write_address) != 0x12345678u) {
        fprintf(stderr, "HARNESS FAIL: selected write did not execute\n");
        return 0;
    }

    cpu.gpr[2] = swc2_address;
    cpu.gte_data[25] = 0x11223344u;
    if (exec_one_fetched_observed(&cpu, kVariantSwc2Pc, kVariantSwc2Insn,
                                  PSX_XG_RENDER_COLD_SOURCE, &next_pc) != 0 ||
        test_read_word(swc2_address) != gte_read_data(&cpu, 25u)) {
        fprintf(stderr, "HARNESS FAIL: selected SWC2 did not execute\n");
        return 0;
    }

    cpu.gpr[2] = 0x80000000u;
    cpu.gpr[3] = 1u;
    if (exec_one_fetched_observed(&cpu, kVariantBucketPc, kVariantBucketInsn,
                                  PSX_XG_RENDER_COLD_SOURCE, &next_pc) != 0 ||
        cpu.gpr[2] != bucket_result) {
        fprintf(stderr, "HARNESS FAIL: selected bucket instruction did not execute\n");
        return 0;
    }

    cpu.gpr[3] = 0x80020000u;
    if (exec_one_fetched_unobserved(&cpu, kVariantReadPc + 0x100u,
                                    kVariantReadInsn, &next_pc) != 0) {
        fprintf(stderr, "HARNESS FAIL: foreign read did not execute\n");
        return 0;
    }

    const uint32_t expected_pcs[] = {
        kVariantReadPc, kVariantReadPc,
        kVariantWritePc, kVariantWritePc,
        kVariantSwc2Pc, kVariantSwc2Pc,
        kVariantBucketPc, kVariantBucketPc,
    };
    const uint32_t expected_instructions[] = {
        kVariantReadInsn, kVariantReadInsn,
        kVariantWriteInsn, kVariantWriteInsn,
        kVariantSwc2Insn, kVariantSwc2Insn,
        kVariantBucketInsn, kVariantBucketInsn,
    };
    const uint32_t expected_auxiliaries[] = {
        read_address, read_address,
        write_address, write_address,
        swc2_address, swc2_address,
        0u, bucket_result,
    };
    const uint32_t expected_operations[] = {
        PSX_XG_RENDER_SOURCE_OPERATION_READ,
        PSX_XG_RENDER_SOURCE_OPERATION_READ,
        PSX_XG_RENDER_SOURCE_OPERATION_WRITE,
        PSX_XG_RENDER_SOURCE_OPERATION_WRITE,
        PSX_XG_RENDER_SOURCE_OPERATION_SWC2,
        PSX_XG_RENDER_SOURCE_OPERATION_SWC2,
        PSX_XG_RENDER_SOURCE_OPERATION_BUCKET,
        PSX_XG_RENDER_SOURCE_OPERATION_BUCKET,
    };
    const uint32_t expected_widths[] = {4u, 4u, 4u, 4u, 4u, 4u, 0u, 0u};
    if (g_xg_render_auth_cold_hook_count != 8u) {
        fprintf(stderr, "FAIL: selected operations emitted %u source hooks, expected 8\n",
                g_xg_render_auth_cold_hook_count);
        return 0;
    }
    for (uint32_t index = 0u; index < 8u; ++index) {
        const uint32_t expected_hook =
            (index & 1u) == 0u ? kSourcePreHook : kSourceCommitHook;
        if (g_xg_render_auth_cold_hook_kinds[index] != expected_hook ||
            g_xg_render_auth_cold_hook_pcs[index] != expected_pcs[index] ||
            g_xg_render_auth_cold_hook_words[index] != expected_instructions[index] ||
            g_xg_render_auth_cold_hook_delays[index] != expected_auxiliaries[index] ||
            g_xg_render_auth_source_operations[index] != expected_operations[index] ||
            g_xg_render_auth_source_widths[index] != expected_widths[index]) {
            fprintf(stderr, "FAIL: source event %u metadata mismatch\n", index);
            return 0;
        }
    }
    return 1;
}

static int test_runtime_call_preserves_pre_capture_delay_commit_order(void) {
    CPUState cpu = {0};
    uint32_t next_pc = 0u;
    const int saved_precise_mode = g_precise_mode;

    prepare_source_cpu(&cpu);
    reset_source_observations();
    test_write_word(kVariantCallPc + 4u, 0u);
    g_precise_mode = 1;
    const int transferred = exec_one_fetched_observed(
        &cpu, kVariantCallPc, kVariantCallInsn,
        PSX_XG_RENDER_COLD_SOURCE | PSX_XG_RENDER_COLD_CAPTURE, &next_pc);
    g_precise_mode = saved_precise_mode;

    if (transferred != 1 || cpu.gpr[31] != kVariantCallPc + 8u ||
        g_xg_render_auth_cold_hook_count != 3u ||
        g_xg_render_auth_cold_hook_kinds[0] != kSourcePreHook ||
        g_xg_render_auth_cold_hook_kinds[1] !=
            PSX_XG_RENDER_AUTH_HOOK_CAPTURE ||
        g_xg_render_auth_cold_hook_kinds[2] != kSourceCommitHook ||
        g_xg_render_auth_cold_hook_pcs[0] != kVariantCallPc ||
        g_xg_render_auth_cold_hook_pcs[1] != kVariantCallPc ||
        g_xg_render_auth_cold_hook_pcs[2] != kVariantCallPc ||
        g_xg_render_auth_cold_hook_delays[0] != 0u ||
        g_xg_render_auth_cold_hook_delays[2] != 0u ||
        g_xg_render_auth_source_operations[0] !=
            PSX_XG_RENDER_SOURCE_OPERATION_CALL ||
        g_xg_render_auth_source_operations[2] !=
            PSX_XG_RENDER_SOURCE_OPERATION_CALL) {
        fprintf(stderr, "FAIL: selected call did not preserve PRE/capture/delay/COMMIT ordering\n");
        return 0;
    }
    return 1;
}

static int test_observe_after_sees_store_and_skips_lockstep_replay(void) {
    CPUState cpu = {0};
    uint32_t next_pc = 0u;
    const uint32_t packet_word = 0x80010400u;

    memset(s_ram, 0, sizeof(s_ram));
    prepare_source_cpu(&cpu);
    cpu.gpr[5] = packet_word;
    cpu.gte_data[22] = UINT32_C(0x11223344);
    g_xg_render_native_cutover_post_pc = kModelPostSwc2Pc;
    g_xg_render_native_cutover_call_count = 0u;
    g_xg_render_native_cutover_observed_word = 0u;
    if (exec_one_fetched_observed(
            &cpu, kModelPostSwc2Pc, kModelPostSwc2Insn,
            PSX_XG_RENDER_COLD_NATIVE_POST, &next_pc) != 0 ||
        test_read_word(packet_word) != UINT32_C(0x11223344) ||
        g_xg_render_native_cutover_call_count != 1u ||
        g_xg_render_native_cutover_observed_word != UINT32_C(0x11223344)) {
        fprintf(stderr, "FAIL: observe-after did not see the committed SWC2 store\n");
        return 0;
    }

    test_write_word(kModelPostSwc2Pc, kModelPostSwc2Insn);
    test_write_word(packet_word, 0u);
    cpu.gte_data[22] = UINT32_C(0x55667788);
    g_ls_replay_active = 1;
    const int replay_result = exec_one(&cpu, kModelPostSwc2Pc, &next_pc);
    g_ls_replay_active = 0;
    if (replay_result != 0 ||
        test_read_word(packet_word) != UINT32_C(0x55667788) ||
        g_xg_render_native_cutover_call_count != 1u) {
        fprintf(stderr, "FAIL: lockstep replay repeated observe-after\n");
        return 0;
    }
    g_xg_render_native_cutover_post_pc = 0u;
    return 1;
}

int main(void) {
    /* Given: NCLIP inputs with determinant 19 and a distinct stale MAC0. */
    CPUState control = {0};
    seed_gte(&control);
    gte_execute(&control, kNclipInsn & 0x01FFFFFFu);
    if (control.gte_data[24] != kExpectedMac0) {
        fprintf(stderr, "HARNESS FAIL: NCLIP control expected MAC0=%u got=%u\n",
                kExpectedMac0, control.gte_data[24]);
        return 2;
    }

    CPUState cpu = {0};
    prepare_irq_case(&cpu);

    /* When: the real site-1 dirty boundary accepts a pending interrupt. */
    int interrupted = dirty_ram_pump_boundary(&cpu, kCop2Pc, 1);
    psx_gte_read(&cpu, 2u);
    uint32_t observed_mac0 = gte_read_data(&cpu, 24u);

    /* Then: the handler sees P, saves P+4, and NCLIP has issued once. */
    if (interrupted != 1 || s_handler_epc != kCop2Pc ||
        s_bios_saved_epc != kCop2Pc + 4u || cpu.pc != kCop2Pc + 4u) {
        fprintf(stderr,
                "HARNESS FAIL: interrupted=%d handler_epc=%08X saved_epc=%08X pc=%08X\n",
                interrupted, s_handler_epc, s_bios_saved_epc, cpu.pc);
        return 2;
    }
    if (observed_mac0 != kExpectedMac0 || s_handler_gte_issue_count != 1u ||
        s_gte_issue_count != 1u || s_gte_issue_cycle != 1u ||
        s_handler_gte_deadline != 8u) {
        fprintf(stderr,
                "FAIL: IRQ accepted with EPC=%08X and BIOS resumed at %08X "
                "without exactly one NCLIP before handler/MAC0 read: "
                "expected=%u got=%u handler_issues=%u total_issues=%u "
                "issue_cycle=%llu handler_deadline=%llu\n",
                s_handler_epc, cpu.pc, kExpectedMac0, observed_mac0,
                s_handler_gte_issue_count, s_gte_issue_count,
                (unsigned long long)s_gte_issue_cycle,
                (unsigned long long)s_handler_gte_deadline);
        return 1;
    }

    if (!test_compiled_return_emits_auth_hook()) return 1;
    if (!test_runtime_operations_emit_exact_source_observation_pairs()) return 1;
    if (!test_runtime_call_preserves_pre_capture_delay_commit_order()) return 1;
    if (!test_observe_after_sees_store_and_skips_lockstep_replay()) return 1;

    prepare_irq_case(&cpu);
    int fell_through = 0;
    int redirected = psx_check_interrupts_at(&cpu, kCop2Pc);
    if (!redirected) {
        fell_through = 1;
        gte_execute(&cpu, kNclipInsn & 0x01FFFFFFu);
    }
    if (redirected != 1 || fell_through != 0 || cpu.pc != kCop2Pc + 4u ||
        s_handler_epc != kCop2Pc || s_handler_gte_issue_count != 1u ||
        s_gte_issue_count != 1u || cpu.gte_data[24] != kExpectedMac0 ||
        s_gte_issue_cycle != 1u || s_handler_gte_deadline != 8u) {
        fprintf(stderr,
                "FAIL: compiled check did not preserve EPC+4 redirect without "
                "fallthrough: redirected=%d fell_through=%d pc=%08X "
                "handler_epc=%08X handler_issues=%u total_issues=%u\n",
                redirected, fell_through, cpu.pc, s_handler_epc,
                s_handler_gte_issue_count, s_gte_issue_count);
        return 1;
    }

    prepare_irq_case(&cpu);
    i_stat = 0;
    cpu.cop0[COP0_SR] |= CAUSE_IP0;
    cpu.cop0[COP0_CAUSE] = CAUSE_IP0;
    cpu.gte_ts_done = 10u;
    g_irq_cop2_test_device_edge_cycle = 5u;
    g_irq_cop2_test_device_edge_bit = TEST_IRQ;
    psx_next_service_cycle = g_irq_cop2_test_device_edge_cycle;

    redirected = psx_check_interrupts_at(&cpu, kCop2Pc);
    observed_mac0 = gte_read_data(&cpu, 24u);
    if (redirected != 1 || s_handler_epc != kCop2Pc ||
        s_bios_saved_epc != kCop2Pc + 4u || cpu.pc != kCop2Pc + 4u ||
        observed_mac0 != kExpectedMac0 || s_handler_gte_issue_count != 1u ||
        s_gte_issue_count != 1u || s_gte_issue_cycle != 10u ||
        s_handler_gte_deadline != 17u || (s_handler_cause & CAUSE_IP0) == 0u ||
        (s_handler_cause & CAUSE_IP2) == 0u) {
        fprintf(stderr,
                "FAIL: busy-GTE software IRQ did not expose matured hardware IP2: "
                "redirected=%d epc=%08X saved_epc=%08X pc=%08X mac0=%u "
                "handler_issues=%u total_issues=%u issue_cycle=%llu "
                "handler_deadline=%llu cause=%08X\n",
                redirected, s_handler_epc, s_bios_saved_epc, cpu.pc,
                observed_mac0, s_handler_gte_issue_count, s_gte_issue_count,
                (unsigned long long)s_gte_issue_cycle,
                (unsigned long long)s_handler_gte_deadline, s_handler_cause);
        return 1;
    }

    puts("PASS: IRQ-visible COP2 EPC issued NCLIP exactly once, redirected to EPC+4, and refreshed Cause.IP2 after a busy-GTE stall");
    return 0;
}
