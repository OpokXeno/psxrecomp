#include "dma.h"
#include "gpu.h"
#include "gpu_render.h"
#include "guest_render_transaction.h"
#include "native_render_baseline.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef GPU_RENDER_TRANSACTION_TESTING
#error "test_xg_render_slot_substitution.c requires GPU transaction support"
#endif
#ifndef GUEST_RENDER_TRANSACTION_TESTING
#error "test_xg_render_slot_substitution.c requires coordinator test support"
#endif

extern void psx_write_word(uint32_t address, uint32_t value);
extern uint32_t psx_read_word(uint32_t address);
extern uint32_t i_stat;
extern uint64_t psx_cycle_count;
extern uint64_t test_irq_raise_calls;
extern uint64_t test_baseline_begin_calls;
extern uint64_t test_baseline_node_calls;
extern uint64_t test_baseline_word_count;
extern uint64_t test_baseline_end_calls;
extern NativeRenderBaselineOtStatus test_baseline_end_status;

enum {
    DMA2_MADR = 0x1f8010a0u,
    DMA2_BCR = 0x1f8010a4u,
    DMA2_CHCR = 0x1f8010a8u,
    DMA_DPCR = 0x1f8010f0u,
    DMA_DICR = 0x1f8010f4u,
    DMA2_ENABLE = 1u << 11,
    DMA2_IRQ_ENABLE = 1u << 18,
    DMA_MASTER_IRQ_ENABLE = 1u << 23,
    DMA2_IRQ_FLAG = 1u << 26,
    DMA2_LINKED = 0x01000401u,
    DMA2_BLOCK_TO_GPU = 0x01000201u,
    DMA2_BLOCK_TO_RAM = 0x01000200u,
    PACKET_WORDS = 4u,
};

typedef struct BackendFixture {
    char events[64];
    size_t event_count;
    uint32_t original_rasters;
    uint32_t semantic_rasters;
    uint32_t rollback_calls;
    uint32_t deferred_capture_calls;
    uint32_t deferred_discard_calls;
    uint32_t deferred_begin_calls;
    GpuRenderDeferredCandidateToken deferred_token;
    int deferred_candidate_active;
    int deferred_transaction_active;
    int fail_semantic;
    int fail_rollback;
} BackendFixture;

static BackendFixture fixture;
static uint16_t *backend_vram;
static uint64_t gp0_base;

static int require(int condition) { return condition; }
#define CHECK(expression) do { if (!require(expression)) return 0; } while (0)

static void record_event(char event) {
    if (fixture.event_count < sizeof(fixture.events) - 1u) {
        fixture.events[fixture.event_count++] = event;
        fixture.events[fixture.event_count] = '\0';
    }
}

static void backend_init(uint16_t *pixels) { backend_vram = pixels; }
static void backend_set_semi(int enabled, int mode) {
    (void)enabled;
    (void)mode;
}
static void backend_set_draw_area(int x1, int y1, int x2, int y2) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
}
static void backend_set_draw_offset(int x, int y) {
    (void)x;
    (void)y;
}
static void backend_draw_flat_triangle(int x0, int y0, int x1, int y1,
                                       int x2, int y2, uint16_t color) {
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)color;
    fixture.original_rasters++;
    record_event('C');
}
static void backend_draw_line(int x0, int y0, int x1, int y1,
                              uint16_t color) {
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)color;
    fixture.original_rasters++;
    record_event('C');
}
static uint16_t backend_vram_read(int x, int y) {
    return backend_vram[(uint32_t)(y & 511) * 1024u + (uint32_t)(x & 1023)];
}
static void backend_vram_transfer_in(int x, int y, int w, int h,
                                     const uint16_t *data) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)data;
}
static GpuRenderTransactionStatus backend_transaction_begin(
        GpuRenderTransactionId id, uint64_t serial) {
    (void)id;
    (void)serial;
    return GPU_RENDER_TRANSACTION_OK;
}
static GpuRenderTransactionStatus backend_ordering_barrier(
        GpuRenderTransactionId id) {
    (void)id;
    return GPU_RENDER_TRANSACTION_OK;
}
static GpuRenderTransactionStatus backend_draw_semantic(
        GpuRenderTransactionId id, const GpuRenderSemantic *semantic) {
    (void)id;
    (void)semantic;
    fixture.semantic_rasters++;
    record_event('T');
    return fixture.fail_semantic ? GPU_RENDER_TRANSACTION_BACKEND_ERROR :
                                   GPU_RENDER_TRANSACTION_OK;
}
static GpuRenderTransactionStatus backend_commit(
        GpuRenderTransactionId id, uint64_t serial,
        const GpuRenderPresent *present) {
    (void)id;
    (void)serial;
    (void)present;
    return GPU_RENDER_TRANSACTION_READY;
}
static GpuRenderTransactionStatus backend_rollback(
        GpuRenderTransactionId id) {
    (void)id;
    fixture.rollback_calls++;
    record_event('R');
    if (fixture.deferred_transaction_active) {
        fixture.deferred_transaction_active = 0;
        fixture.deferred_candidate_active = 0;
        fixture.deferred_token = GPU_RENDER_DEFERRED_CANDIDATE_NONE;
        fixture.deferred_discard_calls++;
    }
    return fixture.fail_rollback ? GPU_RENDER_TRANSACTION_BACKEND_ERROR :
                                   GPU_RENDER_TRANSACTION_OK;
}

static GpuRenderTransactionStatus backend_deferred_capture(
        GpuRenderTransactionId id,
        GpuRenderDeferredCandidateToken *out_token) {
    (void)id;
    if (!out_token || fixture.deferred_candidate_active)
        return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    fixture.deferred_token = UINT64_C(0x5847d3f3);
    fixture.deferred_candidate_active = 1;
    fixture.deferred_capture_calls++;
    *out_token = fixture.deferred_token;
    return GPU_RENDER_TRANSACTION_OK;
}

static GpuRenderTransactionStatus backend_deferred_discard(
        GpuRenderDeferredCandidateToken token) {
    if (!fixture.deferred_candidate_active || token != fixture.deferred_token)
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    fixture.deferred_candidate_active = 0;
    fixture.deferred_token = GPU_RENDER_DEFERRED_CANDIDATE_NONE;
    fixture.deferred_discard_calls++;
    return GPU_RENDER_TRANSACTION_OK;
}

static GpuRenderTransactionStatus backend_deferred_begin(
        GpuRenderTransactionId id, uint64_t serial,
        GpuRenderDeferredCandidateToken token) {
    (void)id;
    (void)serial;
    if (!fixture.deferred_candidate_active || token != fixture.deferred_token)
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    fixture.deferred_transaction_active = 1;
    fixture.deferred_begin_calls++;
    return GPU_RENDER_TRANSACTION_OK;
}

static int backend_swap_deferred(void) {
    if (!fixture.deferred_transaction_active ||
        !fixture.deferred_candidate_active)
        return 0;
    fixture.deferred_transaction_active = 0;
    fixture.deferred_candidate_active = 0;
    fixture.deferred_token = GPU_RENDER_DEFERRED_CANDIDATE_NONE;
    fixture.deferred_discard_calls++;
    return 1;
}

static const GpuRenderBackend TEST_BACKEND = {
    .name = "slot-substitution-test",
    .init = backend_init,
    .set_semi_transparency = backend_set_semi,
    .draw_flat_triangle = backend_draw_flat_triangle,
    .draw_line = backend_draw_line,
    .vram_read = backend_vram_read,
    .vram_transfer_in = backend_vram_transfer_in,
    .set_draw_area = backend_set_draw_area,
    .set_draw_offset = backend_set_draw_offset,
    .transaction_begin = backend_transaction_begin,
    .ordering_barrier = backend_ordering_barrier,
    .draw_semantic = backend_draw_semantic,
    .commit_validate = backend_commit,
    .rollback = backend_rollback,
    .deferred_candidate_capture = backend_deferred_capture,
    .deferred_candidate_discard = backend_deferred_discard,
    .deferred_transaction_begin = backend_deferred_begin,
};

static uint32_t xy(uint32_t x, uint32_t y) { return x | (y << 16); }

static GpuRenderSemantic valid_semantic(void) {
    GpuRenderSemantic semantic = {0};
    semantic.material.texture_depth = GPU_RENDER_TEXTURE_4_BIT;
    semantic.material.blend_mode = GPU_RENDER_BLEND_AVERAGE;
    semantic.material.shading = GPU_RENDER_SHADING_FLAT;
    semantic.triangle_count = 1u;
    semantic.triangles[0].split_count = 1u;
    return semantic;
}

static void write_triangle(uint32_t address, uint32_t bias) {
    psx_write_word(address + 0u, UINT32_C(0x20ffffff));
    psx_write_word(address + 4u, xy(8u + bias, 8u));
    psx_write_word(address + 8u, xy(12u + bias, 8u));
    psx_write_word(address + 12u, xy(8u + bias, 12u));
}

static void write_node(uint32_t header_address, uint32_t next,
                       uint32_t word_count) {
    psx_write_word(header_address, (word_count << 24) | (next & 0x00ffffffu));
}

static void build_substitution_list(uint32_t start, uint32_t *target_command,
                                     uint32_t *end_address) {
    uint32_t first = start;
    uint32_t target = first + 4u + PACKET_WORDS * 4u;
    uint32_t third = target + 4u + (PACKET_WORDS + 1u) * 4u;

    write_node(first, target, PACKET_WORDS);
    write_triangle(first + 4u, 0u);
    write_node(target, third, PACKET_WORDS + 1u);
    psx_write_word(target + 4u, UINT32_C(0xe5003805));
    write_triangle(target + 8u, 16u);
    write_node(third, 0x00ffffffu, PACKET_WORDS);
    write_triangle(third + 4u, 32u);
    *target_command = target + 8u;
    *end_address = third + 4u + PACKET_WORDS * 4u;
}

static void build_multi_command_node(uint32_t start,
                                     uint32_t *target_command) {
    write_node(start, 0x00ffffffu, 9u);
    write_triangle(start + 4u, 0u);
    psx_write_word(start + 20u, UINT32_C(0xe5003805));
    write_triangle(start + 24u, 16u);
    *target_command = start + 4u;
}

static void build_variable_command_node(uint32_t start,
                                        uint32_t *target_command) {
    write_node(start, 0x00ffffffu, 13u);
    psx_write_word(start + 4u, UINT32_C(0xa0000000));
    psx_write_word(start + 8u, xy(100u, 100u));
    psx_write_word(start + 12u, xy(2u, 1u));
    psx_write_word(start + 16u, UINT32_C(0x22221111));
    psx_write_word(start + 20u, UINT32_C(0xe5003805));
    psx_write_word(start + 24u, UINT32_C(0x48ffffff));
    psx_write_word(start + 28u, xy(8u, 8u));
    psx_write_word(start + 32u, xy(12u, 8u));
    psx_write_word(start + 36u, UINT32_C(0x50005000));
    write_triangle(start + 40u, 16u);
    *target_command = start + 40u;
}

static void build_incomplete_polyline_node(uint32_t start) {
    write_node(start, 0x00ffffffu, 7u);
    write_triangle(start + 4u, 0u);
    psx_write_word(start + 20u, UINT32_C(0x48ffffff));
    psx_write_word(start + 24u, xy(8u, 8u));
    psx_write_word(start + 28u, xy(12u, 8u));
}

static void build_multi_triangle_node(uint32_t start, uint32_t count) {
    write_node(start, 0x00ffffffu, count * PACKET_WORDS);
    for (uint32_t i = 0u; i < count; ++i)
        write_triangle(start + 4u + i * PACKET_WORDS * 4u, i * 4u);
}

static void build_triangle_list(uint32_t start, uint32_t count) {
    uint32_t address = start;
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t next = i + 1u == count ? 0x00ffffffu :
            address + 4u + PACKET_WORDS * 4u;
        write_node(address, next, PACKET_WORDS);
        write_triangle(address + 4u, i * 4u);
        address = next;
    }
}

static void reset_runtime(void) {
    guest_render_transaction_test_reset();
    memset(&fixture, 0, sizeof(fixture));
    gr_test_inject_backend(&TEST_BACKEND);
    gpu_init();
    gpu_write_gp0(UINT32_C(0xe3000000));
    gpu_write_gp0(UINT32_C(0xe407ffff));
    gp0_base = gpu_get_gp0_count();
    dma_init();
    i_stat = 0u;
    psx_cycle_count = 0u;
    test_irq_raise_calls = 0u;
    g_native_render_baseline_armed = 0;
    test_baseline_begin_calls = 0u;
    test_baseline_node_calls = 0u;
    test_baseline_word_count = 0u;
    test_baseline_end_calls = 0u;
    test_baseline_end_status = NATIVE_RENDER_BASELINE_OT_INVALID;
}

static int stage_target(GpuRenderTransactionId id, uint32_t target_command) {
    GpuRenderSemantic semantic = valid_semantic();
    return guest_render_transaction_stage_exact(
               id, target_command, &semantic) ==
           GUEST_RENDER_TRANSACTION_OK;
}

static void start_linked_dma(uint32_t start) {
    dma_write(DMA_DPCR, DMA2_ENABLE);
    dma_write(DMA2_MADR, start);
    dma_write(DMA2_BCR, 0u);
    dma_write(DMA2_CHCR, DMA2_LINKED);
}

static uint32_t trace_count(uint32_t kind) {
    const DMATraceEntry *entries;
    uint64_t total = dma_debug_get_trace(&entries);
    uint32_t count = 0u;
    uint64_t begin = total > DMA_TRACE_CAP ? total - DMA_TRACE_CAP : 0u;
    for (uint64_t i = begin; i < total; ++i) {
        const DMATraceEntry *entry = &entries[i % DMA_TRACE_CAP];
        if (entry->kind == kind && entry->channel == 2u) ++count;
    }
    return count;
}

static int snapshot_reason(GuestRenderTransactionPhase phase,
                           GuestRenderTransactionObservationReason reason) {
    GuestRenderTransactionSnapshot snapshot;
    return guest_render_transaction_snapshot(&snapshot) ==
               GUEST_RENDER_TRANSACTION_OK &&
           snapshot.phase == phase && snapshot.abort_reason == reason;
}

static int test_multi_command_node_exact_substitution(void) {
    const GpuRenderTransactionId id = {1u, 8u};
    const uint32_t start = 0x0800u;
    GuestRenderTransactionSnapshot transaction;
    GpuRenderOracleEvent first = {0};
    GpuRenderOracleEvent environment = {0};
    GpuRenderOracleEvent second = {0};
    GpuDrawArea draw_area;
    uint32_t target;
    uint64_t draw_count;
    uint64_t env_count;
    uint64_t unused;
    uint64_t draw_before;
    uint64_t env_before;
    uint32_t e5_before;

    reset_runtime();
    gpu_get_gp0_stats(&unused, &unused, &draw_before, &env_before, &unused);
    e5_before = gpu_render_transaction_test_opcode_count(0xe5u);
    build_multi_command_node(start, &target);
    CHECK(stage_target(id, target));
    CHECK(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    start_linked_dma(start);

    CHECK(strcmp(fixture.events, "TC") == 0);
    CHECK(fixture.original_rasters == 1u && fixture.semantic_rasters == 1u);
    CHECK(gpu_get_gp0_count() - gp0_base == 9u);
    gpu_get_gp0_stats(&unused, &unused, &draw_count, &env_count, &unused);
    CHECK(draw_count - draw_before == 2u && env_count - env_before == 1u);
    CHECK(gpu_render_transaction_test_opcode_count(0xe5u) - e5_before == 1u);
    gpu_get_draw_area(&draw_area);
    CHECK(draw_area.offset_x == 5 && draw_area.offset_y == 7);
    CHECK(guest_render_transaction_snapshot(&transaction) ==
              GUEST_RENDER_TRANSACTION_OK &&
          transaction.phase == GUEST_RENDER_TRANSACTION_ACTIVE &&
          transaction.active_command_count == 3u &&
          transaction.active_binding_count == 1u);
    CHECK(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_OK);
    CHECK(gpu_render_oracle_capture_read_event(0u, &first) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          gpu_render_oracle_capture_read_event(1u, &environment) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          gpu_render_oracle_capture_read_event(2u, &second) ==
              GPU_RENDER_ORACLE_RESULT_OK);
    CHECK(first.source.first_word == (start + 4u) / 4u &&
          first.source.last_word == (start + 16u) / 4u &&
          environment.source.first_word == (start + 20u) / 4u &&
          second.source.first_word == (start + 24u) / 4u &&
          second.source.last_word == (start + 36u) / 4u);
    CHECK(first.source.first_container == start / 4u &&
          environment.source.first_container == start / 4u &&
          second.source.first_container == start / 4u);

    dma_advance(10u);
    CHECK(strcmp(fixture.events, "TCRCC") == 0);
    CHECK(fixture.original_rasters == 3u && fixture.semantic_rasters == 1u);
    CHECK(gpu_get_gp0_count() - gp0_base == 9u);
    CHECK(snapshot_reason(GUEST_RENDER_TRANSACTION_ROLLED_BACK,
                          GUEST_RENDER_TRANSACTION_OBSERVATION_DELAYED_COMPLETION));
    return 1;
}

static int test_variable_commands_preserve_side_effects(void) {
    const GpuRenderTransactionId id = {1u, 9u};
    const uint32_t start = 0x0c00u;
    GuestRenderTransactionSnapshot transaction;
    GpuDrawArea draw_area;
    uint32_t target;

    reset_runtime();
    build_variable_command_node(start, &target);
    CHECK(stage_target(id, target));
    start_linked_dma(start);

    CHECK(strcmp(fixture.events, "CT") == 0);
    CHECK(fixture.original_rasters == 1u && fixture.semantic_rasters == 1u);
    CHECK(gpu_get_gp0_count() - gp0_base == 13u);
    CHECK(gpu_vram_peek(100, 100) == UINT16_C(0x1111));
    CHECK(gpu_vram_peek(101, 100) == UINT16_C(0x2222));
    gpu_get_draw_area(&draw_area);
    CHECK(draw_area.offset_x == 5 && draw_area.offset_y == 7);
    CHECK(guest_render_transaction_snapshot(&transaction) ==
              GUEST_RENDER_TRANSACTION_OK &&
          transaction.phase == GUEST_RENDER_TRANSACTION_ACTIVE &&
          transaction.active_command_count == 4u &&
          transaction.active_binding_count == 1u);

    dma_advance(14u);
    CHECK(strcmp(fixture.events, "CTRCC") == 0);
    CHECK(fixture.original_rasters == 3u && fixture.semantic_rasters == 1u);
    CHECK(gpu_get_gp0_count() - gp0_base == 13u);
    CHECK(gpu_vram_peek(100, 100) == UINT16_C(0x1111));
    CHECK(gpu_vram_peek(101, 100) == UINT16_C(0x2222));
    return 1;
}

static int test_interleaved_substitution_and_delayed_completion(void) {
    const GpuRenderTransactionId id = {1u, 10u};
    const uint32_t start = 0x1000u;
    uint32_t target;
    uint32_t end;
    uint32_t before[16];
    DMADebugState dma_state;
    GuestRenderTransactionSnapshot transaction;
    GpuDrawArea draw_area;
    uint64_t draw_count;
    uint64_t env_count;
    uint64_t unused;
    uint64_t draw_before;
    uint64_t env_before;
    uint64_t serial_before;
    uint32_t e5_before;

    reset_runtime();
    gpu_get_gp0_stats(&unused, &unused, &draw_before, &env_before, &unused);
    serial_before = gpu_render_vram_mutation_serial();
    e5_before = gpu_render_transaction_test_opcode_count(0xe5u);
    build_substitution_list(start, &target, &end);
    CHECK(end - start == sizeof(before));
    for (size_t i = 0u; i < 16u; ++i)
        before[i] = psx_read_word(start + (uint32_t)i * 4u);
    CHECK(stage_target(id, target));
    g_native_render_baseline_armed = 1;
    dma_write(DMA_DICR, DMA2_IRQ_ENABLE | DMA_MASTER_IRQ_ENABLE);
    start_linked_dma(start);

    CHECK(strcmp(fixture.events, "CTC") == 0);
    CHECK(fixture.original_rasters == 2u && fixture.semantic_rasters == 1u);
    CHECK(gpu_get_gp0_count() - gp0_base == 13u);
    gpu_get_gp0_stats(&unused, &unused, &draw_count, &env_count, &unused);
    CHECK(draw_count - draw_before == 3u && env_count - env_before == 1u);
    CHECK(gpu_render_transaction_test_opcode_count(0xe5u) - e5_before == 1u);
    gpu_get_draw_area(&draw_area);
    CHECK(draw_area.offset_x == 5 && draw_area.offset_y == 7);
    for (size_t i = 0u; i < 16u; ++i)
        CHECK(psx_read_word(start + (uint32_t)i * 4u) == before[i]);
    CHECK(guest_render_transaction_snapshot(&transaction) ==
              GUEST_RENDER_TRANSACTION_OK &&
          transaction.phase == GUEST_RENDER_TRANSACTION_ACTIVE &&
          transaction.active_visual_id.scene_epoch == id.scene_epoch &&
          transaction.active_visual_id.state_sequence == id.state_sequence &&
          transaction.active_vram_mutation_serial == serial_before &&
          transaction.active_command_count == 4u &&
          transaction.active_binding_count == 1u);
    dma_debug_get_state(&dma_state);
    CHECK(dma_state.channels[2].madr == 0x00ffffffu);
    CHECK(dma_state.channels[2].remaining_words == 16u);
    CHECK(dma_state.channels[2].cycles_accum == 16u);
    CHECK((dma_state.channels[2].chcr & (1u << 24)) != 0u);
    CHECK(trace_count('S') == 1u && trace_count('C') == 0u);
    CHECK(test_baseline_begin_calls == 1u &&
          test_baseline_node_calls == 3u &&
          test_baseline_word_count == 13u &&
          test_baseline_end_calls == 1u &&
          test_baseline_end_status == NATIVE_RENDER_BASELINE_OT_VALID);

    dma_advance(16u);
    CHECK(strcmp(fixture.events, "CTCRCCC") == 0);
    CHECK(fixture.rollback_calls == 1u);
    CHECK(fixture.original_rasters == 5u && fixture.semantic_rasters == 1u);
    CHECK(gpu_get_gp0_count() - gp0_base == 13u);
    CHECK(snapshot_reason(GUEST_RENDER_TRANSACTION_ROLLED_BACK,
                          GUEST_RENDER_TRANSACTION_OBSERVATION_DELAYED_COMPLETION));
    dma_debug_get_state(&dma_state);
    CHECK((dma_state.channels[2].chcr & (1u << 24)) == 0u);
    CHECK(dma_state.channels[2].remaining_words == 0u);
    CHECK((dma_get_dicr() & DMA2_IRQ_FLAG) != 0u);
    CHECK(trace_count('S') == 1u && trace_count('C') == 1u);
    CHECK(test_irq_raise_calls == 1u && psx_cycle_count == 0u);
    CHECK(test_baseline_begin_calls == 1u &&
          test_baseline_node_calls == 3u &&
          test_baseline_end_calls == 1u);
    dma_advance(16u);
    CHECK(trace_count('C') == 1u && test_irq_raise_calls == 1u);
    return 1;
}

static int test_second_list_guard(void) {
    const GpuRenderTransactionId id = {2u, 20u};
    uint32_t target;
    uint32_t end;

    reset_runtime();
    build_substitution_list(0x1400u, &target, &end);
    (void)end;
    build_triangle_list(0x1800u, 1u);
    CHECK(stage_target(id, target));
    start_linked_dma(0x1400u);
    start_linked_dma(0x1800u);
    CHECK(strcmp(fixture.events, "CTCRCCCC") == 0);
    CHECK(fixture.rollback_calls == 1u);
    CHECK(snapshot_reason(GUEST_RENDER_TRANSACTION_ROLLED_BACK,
                          GUEST_RENDER_TRANSACTION_OBSERVATION_SECOND_LIST));
    CHECK(trace_count('S') == 2u && trace_count('C') == 0u);
    return 1;
}

static int test_pending_target_survives_unrelated_list(void) {
    const GpuRenderTransactionId id = {2u, 21u};
    uint32_t target;
    uint32_t end;
    GuestRenderTransactionPendingSnapshot pending;
    GuestRenderTransactionSnapshot transaction;

    reset_runtime();
    build_triangle_list(0x1600u, 1u);
    build_substitution_list(0x1800u, &target, &end);
    (void)end;
    CHECK(stage_target(id, target));
    start_linked_dma(0x1600u);
    CHECK(strcmp(fixture.events, "C") == 0);
    CHECK(fixture.original_rasters == 1u && fixture.semantic_rasters == 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
              GUEST_RENDER_TRANSACTION_OK && pending.binding_count == 1u);
    CHECK(guest_render_transaction_snapshot(&transaction) ==
              GUEST_RENDER_TRANSACTION_OK &&
          transaction.phase == GUEST_RENDER_TRANSACTION_IDLE);

    start_linked_dma(0x1800u);
    CHECK(strcmp(fixture.events, "CCTC") == 0);
    CHECK(fixture.original_rasters == 3u && fixture.semantic_rasters == 1u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
              GUEST_RENDER_TRANSACTION_OK && pending.binding_count == 0u);
    CHECK(guest_render_transaction_snapshot(&transaction) ==
              GUEST_RENDER_TRANSACTION_OK &&
          transaction.phase == GUEST_RENDER_TRANSACTION_ACTIVE &&
          transaction.active_binding_count == 1u);
    return 1;
}

static int test_execution_failure_and_failed_guard_do_not_duplicate(void) {
    const GpuRenderTransactionId execution_id = {2u, 21u};
    const GpuRenderTransactionId guard_id = {2u, 22u};
    uint32_t target;
    uint32_t end;
    DMADebugState dma_state;

    reset_runtime();
    build_substitution_list(0x1900u, &target, &end);
    (void)end;
    CHECK(stage_target(execution_id, target));
    fixture.fail_semantic = 1;
    start_linked_dma(0x1900u);
    CHECK(strcmp(fixture.events, "CTRCCC") == 0);
    CHECK(fixture.rollback_calls == 1u && fixture.original_rasters == 4u);
    CHECK(gpu_get_gp0_count() - gp0_base == 13u);
    CHECK(snapshot_reason(GUEST_RENDER_TRANSACTION_ROLLED_BACK,
                          GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE));
    dma_advance(16u);
    CHECK(strcmp(fixture.events, "CTRCCC") == 0);
    CHECK(trace_count('C') == 1u);

    reset_runtime();
    build_substitution_list(0x1a00u, &target, &end);
    CHECK(stage_target(guard_id, target));
    start_linked_dma(0x1a00u);
    fixture.fail_rollback = 1;
    dma_advance(16u);
    CHECK(strcmp(fixture.events, "CTCR") == 0);
    CHECK(fixture.rollback_calls == 1u);
    CHECK(trace_count('C') == 0u);
    dma_debug_get_state(&dma_state);
    CHECK((dma_state.channels[2].chcr & (1u << 24)) != 0u);
    CHECK(dma_state.channels[2].remaining_words == 16u &&
          dma_state.channels[2].cycles_accum == 16u);
    dma_advance(16u);
    CHECK(trace_count('C') == 0u && fixture.rollback_calls == 1u);
    return 1;
}

static int test_block_and_c0_guards(void) {
    const GpuRenderTransactionId block_id = {3u, 30u};
    const GpuRenderTransactionId c0_id = {3u, 31u};
    uint32_t target;
    uint32_t end;

    reset_runtime();
    build_substitution_list(0x1c00u, &target, &end);
    (void)end;
    write_triangle(0x2000u, 0u);
    CHECK(stage_target(block_id, target));
    start_linked_dma(0x1c00u);
    dma_write(DMA2_MADR, 0x2000u);
    dma_write(DMA2_BCR, PACKET_WORDS | (1u << 16));
    dma_write(DMA2_CHCR, DMA2_BLOCK_TO_GPU);
    CHECK(snapshot_reason(GUEST_RENDER_TRANSACTION_ROLLED_BACK,
                          GUEST_RENDER_TRANSACTION_OBSERVATION_LATE_COMMAND));
    CHECK(strcmp(fixture.events, "CTCRCCCC") == 0);

    reset_runtime();
    build_substitution_list(0x2400u, &target, &end);
    CHECK(stage_target(c0_id, target));
    start_linked_dma(0x2400u);
    psx_write_word(0x2800u, UINT32_C(0xdeadbeef));
    dma_write(DMA2_MADR, 0x2800u);
    dma_write(DMA2_BCR, 1u | (1u << 16));
    dma_write(DMA2_CHCR, DMA2_BLOCK_TO_RAM);
    CHECK(snapshot_reason(
        GUEST_RENDER_TRANSACTION_ROLLED_BACK,
        GUEST_RENDER_TRANSACTION_OBSERVATION_DMA2_GPU_TO_RAM_C0));
    CHECK(strcmp(fixture.events, "CTCRCCC") == 0);
    CHECK(psx_read_word(0x2800u) == 0u);
    CHECK(trace_count('S') == 2u);
    return 1;
}

static int test_pending_block_substitution(void) {
    const GpuRenderTransactionId id = {3u, 32u};
    const uint32_t start = 0x2c00u;
    GpuRenderSemantic semantic = valid_semantic();
    GuestRenderTransactionSnapshot transaction;
    GuestRenderTransactionPendingSnapshot pending;
    DMADebugState dma_state;

    reset_runtime();
    write_triangle(start, 0u);
    write_triangle(start + 16u, 16u);
    write_triangle(start + 32u, 32u);
    CHECK(guest_render_transaction_stage_exact(
              id, start + 16u, &semantic) == GUEST_RENDER_TRANSACTION_OK);
    dma_write(DMA_DPCR, DMA2_ENABLE);
    dma_write(DMA2_MADR, start);
    dma_write(DMA2_BCR, 12u | (1u << 16));
    dma_write(DMA2_CHCR, DMA2_BLOCK_TO_GPU);

    CHECK(strcmp(fixture.events, "CTC") == 0);
    CHECK(fixture.original_rasters == 2u && fixture.semantic_rasters == 1u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
              GUEST_RENDER_TRANSACTION_OK && pending.binding_count == 0u);
    CHECK(guest_render_transaction_snapshot(&transaction) ==
              GUEST_RENDER_TRANSACTION_OK &&
          transaction.phase == GUEST_RENDER_TRANSACTION_ACTIVE &&
          transaction.active_binding_count == 1u);
    dma_debug_get_state(&dma_state);
    CHECK(dma_state.channels[2].madr == start + 48u);
    return 1;
}

static int test_gpustat_abort_and_deferred_present(void) {
    const GpuRenderTransactionId id = {4u, 39u};
    uint32_t target;
    uint32_t end;
    GuestRenderTransactionDeferredSnapshot deferred;
    GuestRenderTransactionSnapshot transaction;
    GpuRenderPresent present = {0};
    uint64_t serial;

    reset_runtime();
    build_substitution_list(0x3000u, &target, &end);
    (void)end;
    CHECK(stage_target(id, target));
    start_linked_dma(0x3000u);
    CHECK(strcmp(fixture.events, "CTC") == 0);
    CHECK(fixture.original_rasters == 2u && fixture.semantic_rasters == 1u);

    CHECK(gpu_render_transaction_test_gpustat_poll_count() == 0u);
    (void)gpu_read_gpustat();
    CHECK(strcmp(fixture.events, "CTCRCCC") == 0);
    CHECK(fixture.original_rasters == 5u && fixture.semantic_rasters == 1u);
    CHECK(fixture.rollback_calls == 1u);
    CHECK(fixture.deferred_capture_calls == 1u);
    CHECK(fixture.deferred_candidate_active);
    CHECK(gpu_render_transaction_test_gpustat_poll_count() == 1u);
    CHECK(guest_render_transaction_snapshot(&transaction) ==
              GUEST_RENDER_TRANSACTION_OK &&
          transaction.phase == GUEST_RENDER_TRANSACTION_ROLLED_BACK &&
          transaction.abort_reason ==
              GUEST_RENDER_TRANSACTION_OBSERVATION_GPUSTAT &&
          transaction.published_transaction_count == 0u &&
          transaction.published_substitution_count == 0u);
    CHECK(guest_render_transaction_deferred_snapshot(&deferred) ==
              GUEST_RENDER_TRANSACTION_OK && deferred.sealed &&
          deferred.binding_count == 1u);

    (void)gpu_read_gpustat();
    CHECK(gpu_render_transaction_test_gpustat_poll_count() == 2u);
    CHECK(fixture.rollback_calls == 1u && fixture.deferred_capture_calls == 1u);
    CHECK(guest_render_transaction_deferred_snapshot(&deferred) ==
              GUEST_RENDER_TRANSACTION_OK && deferred.sealed);

    gpu_write_gp1(UINT32_C(0x05000000));
    CHECK(guest_render_transaction_deferred_snapshot(&deferred) ==
              GUEST_RENDER_TRANSACTION_OK && deferred.sealed);
    CHECK(fixture.deferred_candidate_active &&
          fixture.deferred_discard_calls == 0u);

    serial = gpu_render_vram_mutation_serial();
    CHECK(serial == deferred.post_replay_vram_mutation_serial);
    CHECK(guest_render_transaction_begin_deferred(id, serial) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(fixture.deferred_begin_calls == 1u);
    CHECK(guest_render_transaction_snapshot(&transaction) ==
              GUEST_RENDER_TRANSACTION_OK &&
          transaction.phase == GUEST_RENDER_TRANSACTION_ACTIVE &&
          transaction.active_command_count == 0u &&
          transaction.active_binding_count == 1u &&
          transaction.published_transaction_count == 0u);
    CHECK(guest_render_transaction_present(id, serial, &present) ==
          GUEST_RENDER_TRANSACTION_READY);
    CHECK(backend_swap_deferred());
    CHECK(guest_render_transaction_post_swap_success() ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_snapshot(&transaction) ==
              GUEST_RENDER_TRANSACTION_OK &&
          transaction.phase == GUEST_RENDER_TRANSACTION_IDLE &&
          transaction.published_transaction_count == 1u &&
          transaction.published_substitution_count == 1u);
    CHECK(!fixture.deferred_candidate_active &&
          fixture.deferred_discard_calls == 1u);

    reset_runtime();
    build_substitution_list(0x3400u, &target, &end);
    CHECK(stage_target(id, target));
    start_linked_dma(0x3400u);
    (void)gpu_read_gpustat();
    CHECK(guest_render_transaction_deferred_snapshot(&deferred) ==
              GUEST_RENDER_TRANSACTION_OK && deferred.sealed);
    gpu_write_gp0(0u);
    CHECK(guest_render_transaction_deferred_snapshot(&deferred) ==
              GUEST_RENDER_TRANSACTION_OK && !deferred.sealed);
    CHECK(!fixture.deferred_candidate_active &&
          fixture.deferred_discard_calls == 1u);
    return 1;
}

static int test_capacity_and_cycle_fallback_original(void) {
    const GpuRenderTransactionId capacity_id = {4u, 40u};
    const GpuRenderTransactionId cycle_id = {4u, 41u};
    const GpuRenderTransactionId malformed_id = {4u, 42u};
    GuestRenderTransactionPendingSnapshot pending;
    GuestRenderTransactionSnapshot transaction;
    DMADebugState dma_state;

    reset_runtime();
    build_multi_triangle_node(0x3000u, 5u);
    CHECK(stage_target(capacity_id, 0x3004u));
    start_linked_dma(0x3000u);
    CHECK(strcmp(fixture.events, "CCCCC") == 0);
    CHECK(fixture.original_rasters == 5u && fixture.semantic_rasters == 0u);
    CHECK(gpu_get_gp0_count() - gp0_base == 20u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
              GUEST_RENDER_TRANSACTION_OK && pending.binding_count == 0u);
    CHECK(guest_render_transaction_snapshot(&transaction) ==
              GUEST_RENDER_TRANSACTION_OK &&
          transaction.phase == GUEST_RENDER_TRANSACTION_IDLE);
    dma_debug_get_state(&dma_state);
    CHECK(dma_state.channels[2].madr == 0x00ffffffu &&
          dma_state.channels[2].remaining_words == 21u);
    CHECK(trace_count('S') == 1u);

    reset_runtime();
    build_incomplete_polyline_node(0x3400u);
    CHECK(stage_target(malformed_id, 0x3404u));
    start_linked_dma(0x3400u);
    CHECK(strcmp(fixture.events, "CC") == 0);
    CHECK(fixture.original_rasters == 2u && fixture.semantic_rasters == 0u);
    CHECK(gpu_get_gp0_count() - gp0_base == 7u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
              GUEST_RENDER_TRANSACTION_OK && pending.binding_count == 0u);
    CHECK(guest_render_transaction_snapshot(&transaction) ==
              GUEST_RENDER_TRANSACTION_OK &&
          transaction.phase == GUEST_RENDER_TRANSACTION_IDLE);
    dma_debug_get_state(&dma_state);
    CHECK(dma_state.channels[2].madr == 0x00ffffffu &&
          dma_state.channels[2].remaining_words == 8u);

    reset_runtime();
    write_node(0x3800u, 0x3820u, PACKET_WORDS);
    write_triangle(0x3804u, 0u);
    write_node(0x3820u, 0x3800u, PACKET_WORDS);
    write_triangle(0x3824u, 8u);
    CHECK(stage_target(cycle_id, 0x3804u));
    start_linked_dma(0x3800u);
    CHECK(fixture.original_rasters == 9u && fixture.semantic_rasters == 0u);
    CHECK(gpu_get_gp0_count() - gp0_base == 36u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
              GUEST_RENDER_TRANSACTION_OK && pending.binding_count == 0u);
    dma_debug_get_state(&dma_state);
    CHECK(dma_state.channels[2].madr == 0x3820u);
    CHECK(dma_state.channels[2].remaining_words == 45u);
    CHECK(trace_count('S') == 1u);

    reset_runtime();
    write_node(0x3c00u, 0x4001u, PACKET_WORDS);
    write_triangle(0x3c04u, 0u);
    write_node(0x4000u, 0x00ffffffu, PACKET_WORDS);
    write_triangle(0x4004u, 8u);
    CHECK(stage_target(cycle_id, 0x3c04u));
    start_linked_dma(0x3c00u);
    CHECK(strcmp(fixture.events, "CC") == 0);
    CHECK(fixture.semantic_rasters == 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
              GUEST_RENDER_TRANSACTION_OK && pending.binding_count == 0u);
    dma_debug_get_state(&dma_state);
    CHECK(dma_state.channels[2].madr == 0x00ffffffu &&
          dma_state.channels[2].remaining_words == 10u);

    reset_runtime();
    write_node(0x4400u, 0x4414u, PACKET_WORDS);
    write_triangle(0x4404u, 0u);
    for (uint32_t i = 0u; i < 8u; ++i) {
        uint32_t header = 0x4414u + i * 4u;
        uint32_t next = i == 7u ? 0x00ffffffu : header + 4u;
        write_node(header, next, 0u);
    }
    CHECK(stage_target(cycle_id, 0x4404u));
    start_linked_dma(0x4400u);
    CHECK(strcmp(fixture.events, "C") == 0 &&
          fixture.semantic_rasters == 0u);
    dma_debug_get_state(&dma_state);
    CHECK(dma_state.channels[2].madr == 0x00ffffffu &&
          dma_state.channels[2].remaining_words == 13u);
    return 1;
}

int main(void) {
    if (!test_variable_commands_preserve_side_effects()) return 1;
    if (!test_interleaved_substitution_and_delayed_completion()) return 2;
    if (!test_second_list_guard()) return 3;
    if (!test_pending_target_survives_unrelated_list()) return 4;
    if (!test_execution_failure_and_failed_guard_do_not_duplicate()) return 5;
    if (!test_block_and_c0_guards()) return 6;
    if (!test_pending_block_substitution()) return 7;
    if (!test_gpustat_abort_and_deferred_present()) return 8;
    if (!test_capacity_and_cycle_fallback_original()) return 9;
    if (!test_multi_command_node_exact_substitution()) return 10;
    return 0;
}
