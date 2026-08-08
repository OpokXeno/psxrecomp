#include "gpu.h"
#include "gpu_render.h"
#include "guest_render_transaction.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef GPU_RENDER_TRANSACTION_TESTING
#error "test_gpu_atomic_checkpoint.c requires GPU render transaction support"
#endif
#ifndef GUEST_RENDER_TRANSACTION_TESTING
#error "test_gpu_atomic_checkpoint.c requires coordinator test support"
#endif

static int require(int condition) { return condition; }
#define CHECK(expression) do { if (!require(expression)) return 0; } while (0)

extern void psx_write_word(uint32_t address, uint32_t value);
extern uint32_t gpu_snapshot_bytes(void);
extern void gpu_snapshot_write(uint8_t *bytes);
extern uint16_t *gpu_get_vram_ptr(void);
extern int gpu_get_c0_count(void);
extern uint64_t g_guest_store_count;
extern uint64_t g_mmio_access_count;
extern uint64_t test_mmio_sync_calls;

enum {
    GP0_MMIO = 0x1f801810u,
    TEST_WIRE_CAPACITY = 512u,
};

typedef struct CounterState {
    uint64_t gp0;
    uint64_t gp1;
    uint64_t nop;
    uint64_t fill;
    uint64_t draw;
    uint64_t env;
    uint64_t copy;
} CounterState;

typedef struct TransactionFixture {
    GuestRenderTransactionCommandMetadata command;
    GuestRenderTransactionJournal journal;
    GuestRenderTransactionSemanticBinding binding;
    GpuRenderOracleSource source;
    CounterState before;
    uint32_t opcode_before;
    uint64_t ring_before;
    uint64_t census_before;
    uint64_t oracle_events_before;
    unsigned int backend_rollback_calls;
    unsigned int replay_calls;
    int replay_saw_restored_state;
} TransactionFixture;

static TransactionFixture transaction_fixture;
static uint16_t *backend_vram;

static CounterState counters(void) {
    CounterState state = {0};

    state.gp0 = gpu_get_gp0_count();
    state.gp1 = gpu_get_gp1_count();
    gpu_get_gp0_stats(&state.nop, &state.fill, &state.draw, &state.env,
                      &state.copy);
    return state;
}

static int counters_equal(CounterState left, CounterState right) {
    return memcmp(&left, &right, sizeof(left)) == 0;
}

static void set_source(GpuRenderOracleSourceKind kind, uint32_t address,
                       uint64_t word, uint64_t container) {
    GpuRenderOracleSource source = {kind, address, word, container};
    gpu_set_gp0_source(&source);
}

static void feed_words(const uint32_t *words, size_t count,
                       const GpuRenderOracleSource *source) {
    size_t index;

    for (index = 0u; index < count; ++index) {
        GpuRenderOracleSource word_source = *source;
        word_source.word_address += (uint32_t)(index * 4u);
        word_source.word_ordinal += index;
        gpu_set_gp0_source(&word_source);
        gpu_write_gp0(words[index]);
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
static void backend_set_draw_offset(int x, int y) { (void)x; (void)y; }
static void backend_draw_flat_triangle(int x0, int y0, int x1, int y1,
                                       int x2, int y2, uint16_t color) {
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)color;
}
static uint16_t backend_vram_read(int x, int y) {
    return backend_vram[(uint32_t)(y & 511) * 1024u + (uint32_t)(x & 1023)];
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
    return GPU_RENDER_TRANSACTION_OK;
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
    transaction_fixture.backend_rollback_calls++;
    return GPU_RENDER_TRANSACTION_OK;
}

static const GpuRenderBackend TEST_BACKEND = {
    .name = "atomic-checkpoint-test",
    .init = backend_init,
    .set_semi_transparency = backend_set_semi,
    .draw_flat_triangle = backend_draw_flat_triangle,
    .vram_read = backend_vram_read,
    .set_draw_area = backend_set_draw_area,
    .set_draw_offset = backend_set_draw_offset,
    .transaction_begin = backend_transaction_begin,
    .ordering_barrier = backend_ordering_barrier,
    .draw_semantic = backend_draw_semantic,
    .commit_validate = backend_commit,
    .rollback = backend_rollback,
};

static void reset_gpu(void) {
    memset(&transaction_fixture, 0, sizeof(transaction_fixture));
    guest_render_transaction_test_reset();
    gr_test_inject_backend(&TEST_BACKEND);
    gpu_init();
}

static GpuRenderSemantic valid_semantic(void) {
    GpuRenderSemantic semantic = {0};

    semantic.material.texture_depth = GPU_RENDER_TEXTURE_4_BIT;
    semantic.material.blend_mode = GPU_RENDER_BLEND_AVERAGE;
    semantic.material.shading = GPU_RENDER_SHADING_FLAT;
    semantic.triangle_count = 1u;
    semantic.triangles[0].split_count = 1u;
    return semantic;
}

static bool target_side_effects(
        const GuestRenderTransactionCommandMetadata *metadata,
        const uint32_t *words, size_t word_count, void *user_data) {
    TransactionFixture *fixture = (TransactionFixture *)user_data;

    (void)metadata;
    if (gr_draw_suppression_begin() != GPU_RENDER_DRAW_SUPPRESSION_OK)
        return false;
    feed_words(words, word_count, &fixture->source);
    return gr_draw_suppression_end() == GPU_RENDER_DRAW_SUPPRESSION_OK;
}

static void observe_material(
        const GuestRenderTransactionCommandMetadata *metadata,
        const GpuRenderMaterial *material, void *user_data) {
    (void)metadata;
    (void)material;
    (void)user_data;
}

static bool replay_original(const GuestRenderTransactionReplayJournal *journal,
                            void *user_data) {
    TransactionFixture *fixture = (TransactionFixture *)user_data;
    GpuRenderOracleSnapshot oracle;
    CounterState current = counters();

    fixture->replay_calls++;
    fixture->replay_saw_restored_state =
        fixture->backend_rollback_calls == 1u &&
        !gpu_render_transaction_test_checkpoint_open() &&
        counters_equal(current, fixture->before) &&
        gpu_render_transaction_test_opcode_count(
            (uint8_t)(journal->words[0] >> 24)) == fixture->opcode_before &&
        gpu_gp0_ring_total() == fixture->ring_before &&
        gpu_ws_census_seq() == fixture->census_before &&
        gpu_render_oracle_capture_snapshot(&oracle) ==
            GPU_RENDER_ORACLE_RESULT_OK &&
        oracle.event_count == fixture->oracle_events_before;
    feed_words(journal->words, journal->word_count, &fixture->source);
    return true;
}

static int begin_transaction(const uint32_t *words, size_t word_count,
                             GpuRenderOracleSource source) {
    static uint64_t sequence = 1u;
    GuestRenderTransactionRequest request = {0};
    GpuRenderOracleSnapshot oracle;
    GpuRenderSemantic semantic = valid_semantic();
    GpuRenderTransactionId id = {1u, sequence++};

    transaction_fixture.command.source = GUEST_RENDER_TRANSACTION_SOURCE_MMIO;
    transaction_fixture.command.list_id = id.state_sequence;
    transaction_fixture.command.command_id = id.state_sequence + 100u;
    transaction_fixture.command.predecessor_command_id =
        GUEST_RENDER_TRANSACTION_NO_COMMAND;
    transaction_fixture.command.successor_command_id =
        GUEST_RENDER_TRANSACTION_NO_COMMAND;
    transaction_fixture.command.word_count = word_count;
    transaction_fixture.journal.visual_id = id;
    transaction_fixture.journal.vram_mutation_serial = 77u;
    transaction_fixture.journal.list_id = transaction_fixture.command.list_id;
    transaction_fixture.journal.commands = &transaction_fixture.command;
    transaction_fixture.journal.command_count = 1u;
    transaction_fixture.journal.words = words;
    transaction_fixture.journal.word_count = word_count;
    transaction_fixture.journal.complete = true;
    transaction_fixture.binding.has_exact_command_id = true;
    transaction_fixture.binding.exact_command_id =
        transaction_fixture.command.command_id;
    transaction_fixture.binding.semantic = semantic;
    transaction_fixture.source = source;
    transaction_fixture.before = counters();
    transaction_fixture.opcode_before =
        gpu_render_transaction_test_opcode_count((uint8_t)(words[0] >> 24));
    transaction_fixture.ring_before = gpu_gp0_ring_total();
    transaction_fixture.census_before = gpu_ws_census_seq();
    if (gpu_render_oracle_capture_snapshot(&oracle) !=
        GPU_RENDER_ORACLE_RESULT_OK)
        return 0;
    transaction_fixture.oracle_events_before = oracle.event_count;

    request.journal = &transaction_fixture.journal;
    request.bindings = &transaction_fixture.binding;
    request.binding_count = 1u;
    request.current_visual_id = id;
    request.current_vram_mutation_serial = 77u;
    request.target_side_effects_callback = target_side_effects;
    request.target_side_effects_user_data = &transaction_fixture;
    request.material_observation_callback = observe_material;
    request.begin_checkpoint = gpu_render_transaction_checkpoint_begin;
    request.rollback_checkpoint = gpu_render_transaction_checkpoint_rollback;
    request.commit_checkpoint = gpu_render_transaction_checkpoint_commit;
    request.replay_callback = replay_original;
    request.replay_user_data = &transaction_fixture;
    return guest_render_transaction_execute(&request) ==
               GUEST_RENDER_TRANSACTION_OK &&
           gpu_render_transaction_test_checkpoint_open();
}

static int abort_reason_is(GuestRenderTransactionObservationReason reason) {
    GuestRenderTransactionSnapshot snapshot;

    return guest_render_transaction_snapshot(&snapshot) ==
               GUEST_RENDER_TRANSACTION_OK &&
           snapshot.phase == GUEST_RENDER_TRANSACTION_ROLLED_BACK &&
           snapshot.abort_reason == reason;
}

static int test_parser_counters_oracle_and_cursors_restore(void) {
    const uint32_t command[4] = {
        UINT32_C(0x20ffffff), UINT32_C(0x00080008),
        UINT32_C(0x00080010), UINT32_C(0x00100008),
    };
    const GpuRenderTransactionId id = {9u, 10u};
    uint8_t before_wire[TEST_WIRE_CAPACITY];
    uint8_t restored_wire[TEST_WIRE_CAPACITY];
    uint32_t wire_bytes;
    CounterState before;
    uint32_t opcode_before;
    uint64_t ring_before;
    uint64_t census_before;
    uint64_t serial_before;
    uint64_t serial_after_speculative;
    GpuRenderOracleSnapshot oracle;
    GpuRenderOracleSource restored_source;
    GpuRenderOracleEvent event;

    reset_gpu();
    CHECK(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x1000u, 100u, 33u);
    gpu_write_gp0(command[0]);
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x1004u, 101u, 33u);
    gpu_write_gp0(command[1]);
    wire_bytes = gpu_snapshot_bytes();
    CHECK(wire_bytes <= TEST_WIRE_CAPACITY);
    gpu_snapshot_write(before_wire);
    before = counters();
    opcode_before = gpu_render_transaction_test_opcode_count(0x20u);
    ring_before = gpu_gp0_ring_total();
    census_before = gpu_ws_census_seq();
    serial_before = gpu_render_vram_mutation_serial();
    CHECK(gpu_render_transaction_checkpoint_begin(id, 11u, NULL));
    CHECK(!gpu_render_transaction_checkpoint_begin(id, 11u, NULL));
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x2000u, 900u, 44u);
    gpu_write_gp0(command[2]);
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x2004u, 901u, 44u);
    gpu_write_gp0(command[3]);
    serial_after_speculative = gpu_render_vram_mutation_serial();
    CHECK(serial_after_speculative == serial_before + 1u);
    CHECK(gpu_gp0_ring_total() == ring_before + 1u);
    CHECK(gpu_render_transaction_checkpoint_rollback(id, 11u, NULL));
    CHECK(gpu_render_vram_mutation_serial() == serial_after_speculative);
    CHECK(!gpu_render_transaction_test_checkpoint_open());
    gpu_snapshot_write(restored_wire);
    CHECK(memcmp(before_wire, restored_wire, wire_bytes) == 0);
    CHECK(counters_equal(counters(), before));
    CHECK(gpu_render_transaction_test_opcode_count(0x20u) == opcode_before);
    CHECK(gpu_gp0_ring_total() == ring_before);
    CHECK(gpu_ws_census_seq() == census_before);
    CHECK(gpu_render_oracle_capture_snapshot(&oracle) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          oracle.event_count == 0u);
    gpu_render_transaction_test_source(&restored_source);
    CHECK(restored_source.kind == GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK &&
          restored_source.word_address == 0x1004u &&
          restored_source.word_ordinal == 101u &&
          restored_source.container_ordinal == 33u);

    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x1008u, 102u, 33u);
    gpu_write_gp0(command[2]);
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x100cu, 103u, 33u);
    gpu_write_gp0(command[3]);
    CHECK(gpu_render_vram_mutation_serial() == serial_after_speculative + 1u);
    CHECK(gpu_render_oracle_capture_snapshot(&oracle) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          oracle.event_count == 1u);
    CHECK(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_OK);
    CHECK(gpu_render_oracle_capture_read_event(0u, &event) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          event.source.first_word == 100u && event.source.last_word == 103u &&
          event.source.first_container == 33u &&
          event.source.last_container == 33u && !event.source.discontinuous);
    CHECK(gpu_get_gp0_count() == before.gp0 + 2u);
    CHECK(gpu_render_transaction_test_opcode_count(0x20u) ==
          opcode_before + 1u);
    CHECK(gpu_gp0_ring_total() == ring_before + 1u);
    CHECK(gpu_ws_census_seq() == census_before + 1u);
    return 1;
}

static int test_commit_discards_without_restoring(void) {
    const GpuRenderTransactionId id = {12u, 13u};
    const GpuRenderTransactionId other = {12u, 14u};
    const uint32_t draw_offset = UINT32_C(0xe5003805);
    GpuDrawArea area;

    reset_gpu();
    CHECK(gpu_render_transaction_checkpoint_begin(id, 15u, NULL));
    gpu_write_gp0(draw_offset);
    CHECK(!gpu_render_transaction_checkpoint_commit(other, 15u, NULL));
    CHECK(gpu_render_transaction_test_checkpoint_open());
    CHECK(gpu_render_transaction_checkpoint_commit(id, 15u, NULL));
    CHECK(!gpu_render_transaction_test_checkpoint_open());
    gpu_get_draw_area(&area);
    CHECK(area.offset_x == 5 && area.offset_y == 7);
    CHECK(gpu_render_transaction_checkpoint_begin(other, 16u, NULL));
    gpu_write_gp0(UINT32_C(0xe5000000));
    CHECK(gpu_render_transaction_checkpoint_rollback(other, 16u, NULL));
    gpu_get_draw_area(&area);
    CHECK(area.offset_x == 5 && area.offset_y == 7);
    return 1;
}

static int test_successful_swap_commits_checkpoint(void) {
    const uint32_t original[] = {UINT32_C(0xe5003805)};
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x2800u, 650u, 70u,
    };
    GpuRenderPresent present = {0};
    GuestRenderTransactionSnapshot snapshot;
    GpuDrawArea area;

    reset_gpu();
    CHECK(begin_transaction(original, 1u, source));
    CHECK(guest_render_transaction_present(
              transaction_fixture.journal.visual_id,
              transaction_fixture.journal.vram_mutation_serial,
              &present) == GUEST_RENDER_TRANSACTION_READY);
    CHECK(gpu_render_transaction_test_checkpoint_open());
    CHECK(guest_render_transaction_post_swap_success() ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(!gpu_render_transaction_test_checkpoint_open());
    CHECK(transaction_fixture.replay_calls == 0u &&
          transaction_fixture.backend_rollback_calls == 0u);
    gpu_get_draw_area(&area);
    CHECK(area.offset_x == 5 && area.offset_y == 7);
    CHECK(gpu_get_gp0_count() == transaction_fixture.before.gp0 + 1u);
    CHECK(guest_render_transaction_snapshot(&snapshot) ==
              GUEST_RENDER_TRANSACTION_OK &&
          snapshot.phase == GUEST_RENDER_TRANSACTION_IDLE &&
          snapshot.published_transaction_count == 1u &&
          snapshot.published_substitution_count == 1u);
    return 1;
}

static int test_gp1_guard_replays_before_trigger_once(void) {
    const uint32_t original[] = {UINT32_C(0xe5003805)};
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST, 0x3000u, 700u, 80u,
    };
    GpuRenderOracleSnapshot oracle;
    GpuRenderOracleEvent first;
    GpuRenderOracleEvent second;

    reset_gpu();
    CHECK(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    CHECK(begin_transaction(original, 1u, source));
    gpu_write_gp1(UINT32_C(0x10000005));
    CHECK(transaction_fixture.replay_calls == 1u &&
          transaction_fixture.replay_saw_restored_state);
    CHECK(gpu_get_gp0_count() == transaction_fixture.before.gp0 + 1u);
    CHECK(gpu_get_gp1_count() == transaction_fixture.before.gp1 + 1u);
    CHECK(gpu_read_gpuread() == UINT32_C(0x00003805));
    CHECK(abort_reason_is(GUEST_RENDER_TRANSACTION_OBSERVATION_GP1));
    CHECK(gpu_render_oracle_capture_snapshot(&oracle) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          oracle.event_count == 2u);
    CHECK(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_OK);
    CHECK(gpu_render_oracle_capture_read_event(0u, &first) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          gpu_render_oracle_capture_read_event(1u, &second) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          first.kind == GPU_RENDER_ORACLE_EVENT_GP0_COMMAND &&
          first.source.first_word == 700u &&
          second.kind == GPU_RENDER_ORACLE_EVENT_GP1 &&
          first.sequence < second.sequence);
    return 1;
}

static int test_gpustat_guard_polls_once_after_replay(void) {
    const uint32_t original[] = {UINT32_C(0xe5000801)};
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x4000u, 800u, 90u,
    };
    uint32_t polls;

    reset_gpu();
    polls = gpu_render_transaction_test_gpustat_poll_count();
    CHECK(begin_transaction(original, 1u, source));
    (void)gpu_read_gpustat();
    CHECK(transaction_fixture.replay_calls == 1u &&
          transaction_fixture.replay_saw_restored_state);
    CHECK(gpu_render_transaction_test_gpustat_poll_count() == polls + 1u);
    CHECK(gpu_get_gp0_count() == transaction_fixture.before.gp0 + 1u);
    CHECK(abort_reason_is(GUEST_RENDER_TRANSACTION_OBSERVATION_GPUSTAT));
    return 1;
}

static int test_gpuread_guard_restores_transfer_before_read(void) {
    const uint32_t original[] = {
        UINT32_C(0xc0000000), UINT32_C(0x0014000a), UINT32_C(0x00010003),
    };
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x5000u, 900u, 100u,
    };
    uint16_t *vram;
    uint32_t first_word;
    uint32_t second_word;
    int c0_before;
    GpuRenderOracleSnapshot oracle;
    GpuRenderOracleEvent transfer_end;

    reset_gpu();
    vram = gpu_get_vram_ptr();
    vram[20u * 1024u + 10u] = UINT16_C(0x1111);
    vram[20u * 1024u + 11u] = UINT16_C(0x2222);
    vram[20u * 1024u + 12u] = UINT16_C(0x3333);
    c0_before = gpu_get_c0_count();
    CHECK(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    CHECK(begin_transaction(original, 3u, source));
    first_word = gpu_read_gpuread();
    CHECK(first_word == UINT32_C(0x22221111));
    CHECK(transaction_fixture.replay_calls == 1u &&
          transaction_fixture.replay_saw_restored_state);
    CHECK(gpu_get_c0_count() == c0_before + 1);
    CHECK(gpu_get_gp0_count() == transaction_fixture.before.gp0 + 3u);
    CHECK(abort_reason_is(GUEST_RENDER_TRANSACTION_OBSERVATION_GPUREAD));
    second_word = gpu_read_gpuread();
    CHECK(second_word == UINT32_C(0x00003333));
    CHECK(gpu_render_oracle_capture_snapshot(&oracle) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          oracle.event_count == 2u);
    CHECK(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_OK);
    CHECK(gpu_render_oracle_capture_read_event(1u, &transfer_end) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          transfer_end.kind == GPU_RENDER_ORACLE_EVENT_TRANSFER_END &&
          transfer_end.transfer.observed_words == 2u);
    return 1;
}

static int test_mmio_gp0_guard_has_one_store_and_sync(void) {
    const uint32_t original[] = {UINT32_C(0xe5002805)};
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x6000u, 1000u, 110u,
    };
    uint64_t stores;
    uint64_t mmio;
    uint64_t syncs;
    GpuDrawArea area;
    GpuRenderOracleSnapshot oracle;
    GpuRenderOracleEvent trigger;
    GpuRenderOracleEvent next;

    reset_gpu();
    CHECK(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    CHECK(begin_transaction(original, 1u, source));
    stores = g_guest_store_count;
    mmio = g_mmio_access_count;
    syncs = test_mmio_sync_calls;
    psx_write_word(GP0_MMIO, UINT32_C(0xe5004809));
    CHECK(g_guest_store_count == stores + 1u);
    CHECK(g_mmio_access_count == mmio + 1u);
    CHECK(test_mmio_sync_calls == syncs + 1u);
    CHECK(transaction_fixture.replay_calls == 1u &&
          transaction_fixture.replay_saw_restored_state);
    CHECK(gpu_get_gp0_count() == transaction_fixture.before.gp0 + 2u);
    CHECK(gpu_render_transaction_test_opcode_count(0xe5u) ==
          transaction_fixture.opcode_before + 2u);
    CHECK(gpu_gp0_ring_total() == transaction_fixture.ring_before + 2u);
    gpu_get_draw_area(&area);
    CHECK(area.offset_x == 9 && area.offset_y == 9);
    CHECK(abort_reason_is(GUEST_RENDER_TRANSACTION_OBSERVATION_LATE_MMIO));
    CHECK(gpu_render_oracle_capture_snapshot(&oracle) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          oracle.event_count == 2u);
    psx_write_word(GP0_MMIO, UINT32_C(0xe500500a));
    CHECK(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_OK);
    CHECK(gpu_render_oracle_capture_read_event(1u, &trigger) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          trigger.source.kind_mask == GPU_RENDER_ORACLE_SOURCE_MASK_MMIO);
    CHECK(gpu_render_oracle_capture_read_event(2u, &next) ==
              GPU_RENDER_ORACLE_RESULT_OK &&
          next.source.kind_mask == GPU_RENDER_ORACLE_SOURCE_MASK_MMIO &&
          next.source.first_word == trigger.source.first_word + 1u);
    return 1;
}

static int test_original_material_capture_is_scoped(void) {
    const uint32_t draw[] = {
        UINT32_C(0x20ffffff), UINT32_C(0x00080008),
        UINT32_C(0x00080010), UINT32_C(0x00100008),
    };
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x7000u, 1100u, 120u,
    };
    GpuRenderMaterial material;
    bool observed = false;

    reset_gpu();
    CHECK(gpu_render_material_capture_begin());
    CHECK(!gpu_render_material_capture_begin());
    feed_words(draw, 4u, &source);
    CHECK(gpu_render_material_capture_end(&material, &observed));
    CHECK(observed);
    CHECK(material.shading == GPU_RENDER_SHADING_FLAT);
    CHECK(material.textured == 0u);
    CHECK(material.draw_area_right == 0u);
    CHECK(material.draw_area_bottom == 0u);

    CHECK(gpu_render_material_capture_begin());
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x7010u, 1104u, 120u);
    gpu_write_gp0(UINT32_C(0xe1000000));
    CHECK(gpu_render_material_capture_end(&material, &observed));
    CHECK(!observed);

    CHECK(gpu_render_material_capture_begin());
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x7020u, 1108u, 120u);
    gpu_write_gp0(UINT32_C(0x48ffffff));
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x7024u, 1109u, 120u);
    gpu_write_gp0(UINT32_C(0x00080008));
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x7028u, 1110u, 120u);
    gpu_write_gp0(UINT32_C(0x55555555));
    CHECK(gpu_render_material_capture_end(&material, &observed));
    CHECK(observed && material.shading == GPU_RENDER_SHADING_FLAT);

    CHECK(gpu_render_material_capture_begin());
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x7030u, 1112u, 120u);
    gpu_write_gp0(UINT32_C(0x58ffffff));
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x7034u, 1113u, 120u);
    gpu_write_gp0(UINT32_C(0x00080008));
    set_source(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 0x7038u, 1114u, 120u);
    gpu_write_gp0(UINT32_C(0x55555555));
    CHECK(gpu_render_material_capture_end(&material, &observed));
    CHECK(observed && material.shading == GPU_RENDER_SHADING_GOURAUD);
    return 1;
}

int main(void) {
    if (!test_parser_counters_oracle_and_cursors_restore()) return 1;
    if (!test_commit_discards_without_restoring()) return 2;
    if (!test_successful_swap_commits_checkpoint()) return 3;
    if (!test_gp1_guard_replays_before_trigger_once()) return 4;
    if (!test_gpustat_guard_polls_once_after_replay()) return 5;
    if (!test_gpuread_guard_restores_transfer_before_read()) return 6;
    if (!test_mmio_gp0_guard_has_one_store_and_sync()) return 7;
    if (!test_original_material_capture_is_scoped()) return 8;
    return 0;
}
