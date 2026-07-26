#include "cpu_state.h"
#include "interrupts.h"

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

static uint8_t s_ram[2u * 1024u * 1024u];
static uint32_t s_handler_epc;
static uint32_t s_handler_gte_issue_count;
static uint32_t s_bios_saved_epc;
static uint32_t s_gte_issue_count;
static uint64_t s_gte_issue_cycle;
static uint64_t s_handler_gte_deadline;
static uint32_t s_handler_cause;

uint64_t g_irq_cop2_test_device_edge_cycle = UINT64_MAX;
uint32_t g_irq_cop2_test_device_edge_bit;

uint32_t i_stat;
uint32_t i_mask;
int g_psx_call_bail;
uint64_t s_frame_count;

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
