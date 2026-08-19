#include "guest_render_transaction.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef GUEST_RENDER_TRANSACTION_TESTING
#error "test_guest_render_transaction.c requires test support"
#endif

#if GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY != 4
#error "tests require command capacity 4"
#endif

#if GUEST_RENDER_TRANSACTION_WORD_CAPACITY != 16
#error "tests require word capacity 16"
#endif

#if GUEST_RENDER_TRANSACTION_BINDING_CAPACITY != 2
#error "tests require binding capacity 2"
#endif

#if GUEST_RENDER_TRANSACTION_PENDING_CAPACITY != 2
#error "tests require pending capacity 2"
#endif

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

uint64_t s_frame_count;

typedef enum EventKind {
    EVENT_CHECKPOINT_BEGIN = 1,
    EVENT_BEGIN,
    EVENT_BARRIER,
    EVENT_COMPATIBILITY,
    EVENT_TARGET_SIDE_EFFECTS,
    EVENT_DRAW,
    EVENT_COMMIT,
    EVENT_ROLLBACK,
    EVENT_CHECKPOINT_ROLLBACK,
    EVENT_CHECKPOINT_COMMIT,
    EVENT_REPLAY,
} EventKind;

typedef struct Event {
    EventKind kind;
    uint64_t value;
} Event;

typedef struct Fixture {
    Event events[64];
    size_t event_count;
    size_t begin_count;
    size_t barrier_count;
    size_t draw_count;
    size_t commit_count;
    size_t rollback_count;
    size_t deferred_capture_count;
    size_t deferred_discard_count;
    size_t deferred_begin_count;
    size_t checkpoint_begin_count;
    size_t checkpoint_rollback_count;
    size_t checkpoint_commit_count;
    size_t compatibility_count;
    size_t target_side_effects_count;
    size_t material_observation_count;
    size_t replay_count;
    size_t selector_count;
    size_t trigger_count;
    size_t source_side_effect_count;
    bool compatibility_argument_error;
    size_t fail_barrier_call;
    size_t fail_draw_call;
    uint64_t fail_compatibility_command;
    uint64_t fail_target_side_effects_command;
    bool fail_begin;
    bool fail_commit;
    bool fail_rollback;
    bool fail_replay;
    bool fail_checkpoint_begin;
    bool fail_checkpoint_rollback;
    bool fail_checkpoint_commit;
    bool report_replay_material;
    GpuRenderTransactionId begin_id;
    uint64_t begin_serial;
    GpuRenderSemantic drawn[4];
    GuestRenderTransactionCommandMetadata replayed_commands[4];
    uint32_t replayed_words[16];
    size_t replayed_command_count;
    size_t replayed_word_count;
    uint64_t compatibility_command_ids[4];
    uint64_t target_side_effects_command_id;
    uint64_t compatibility_material_command_id;
    uint64_t modeled_bound_target_command_id;
    uint32_t target_side_effects_words[16];
    size_t target_side_effects_word_count;
    size_t original_target_raster_count;
    bool target_side_effects_argument_error;
    bool material_observation_argument_error;
    uint64_t material_observation_command_id;
    GpuRenderMaterial observed_material;
    uint64_t material_observation_command_ids[4];
    GpuRenderMaterial observed_materials[4];
    int runtime_state;
    int checkpoint_saved_state;
    int runtime_state_at_replay;
    int committed_runtime_state;
    bool checkpoint_live;
    bool runtime_state_committed;
    bool deferred_candidate_active;
    bool deferred_transaction_active;
    GpuRenderDeferredCandidateToken deferred_candidate_token;
} Fixture;

typedef struct JournalFixture {
    GuestRenderTransactionCommandMetadata commands[5];
    uint32_t words[17];
    GuestRenderTransactionJournal journal;
    GuestRenderTransactionSemanticBinding bindings[3];
    GuestRenderTransactionRequest request;
} JournalFixture;

static Fixture fixture;
static uint64_t selected_command_id = 22u;

static void record_event(EventKind kind, uint64_t value) {
    if (fixture.event_count < ARRAY_COUNT(fixture.events)) {
        fixture.events[fixture.event_count].kind = kind;
        fixture.events[fixture.event_count].value = value;
        ++fixture.event_count;
    }
}

GpuRenderTransactionStatus gr_transaction_begin(
        GpuRenderTransactionId transaction_id,
        uint64_t vram_mutation_serial) {
    ++fixture.begin_count;
    fixture.begin_id = transaction_id;
    fixture.begin_serial = vram_mutation_serial;
    record_event(EVENT_BEGIN, vram_mutation_serial);
    return fixture.fail_begin ? GPU_RENDER_TRANSACTION_BACKEND_ERROR :
                                GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_ordering_barrier(
        GpuRenderTransactionId transaction_id) {
    (void)transaction_id;
    ++fixture.barrier_count;
    record_event(EVENT_BARRIER, fixture.barrier_count);
    return fixture.fail_barrier_call == fixture.barrier_count ?
        GPU_RENDER_TRANSACTION_CONTEXT_LOST : GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_draw_semantic(
        GpuRenderTransactionId transaction_id,
        const GpuRenderSemantic *semantic) {
    (void)transaction_id;
    ++fixture.draw_count;
    record_event(EVENT_DRAW, (uint64_t)(uint32_t)
                 semantic->triangles[0].vertices[0].x);
    if (fixture.draw_count <= ARRAY_COUNT(fixture.drawn))
        fixture.drawn[fixture.draw_count - 1u] = *semantic;
    return fixture.fail_draw_call == fixture.draw_count ?
        GPU_RENDER_TRANSACTION_BACKEND_ERROR : GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_commit_validate(
        GpuRenderTransactionId transaction_id,
        uint64_t current_vram_mutation_serial,
        const GpuRenderPresent *present) {
    (void)transaction_id;
    (void)present;
    ++fixture.commit_count;
    record_event(EVENT_COMMIT, current_vram_mutation_serial);
    return fixture.fail_commit ? GPU_RENDER_TRANSACTION_VALIDATION_FAILED :
                                 GPU_RENDER_TRANSACTION_READY;
}

GpuRenderTransactionStatus gr_rollback(
        GpuRenderTransactionId transaction_id) {
    ++fixture.rollback_count;
    record_event(EVENT_ROLLBACK, transaction_id.state_sequence);
    if (fixture.deferred_transaction_active) {
        fixture.deferred_transaction_active = false;
        fixture.deferred_candidate_active = false;
        fixture.deferred_candidate_token =
            GPU_RENDER_DEFERRED_CANDIDATE_NONE;
        ++fixture.deferred_discard_count;
    }
    return fixture.fail_rollback ? GPU_RENDER_TRANSACTION_CONTEXT_LOST :
                                   GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_deferred_candidate_capture(
        GpuRenderTransactionId transaction_id,
        GpuRenderDeferredCandidateToken *out_candidate_token) {
    (void)transaction_id;
    if (!out_candidate_token || fixture.deferred_candidate_active)
        return GPU_RENDER_TRANSACTION_INVALID_TRANSITION;
    ++fixture.deferred_capture_count;
    fixture.deferred_candidate_token = UINT64_C(0xd3f3);
    fixture.deferred_candidate_active = true;
    *out_candidate_token = fixture.deferred_candidate_token;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_deferred_candidate_discard(
        GpuRenderDeferredCandidateToken candidate_token) {
    if (!fixture.deferred_candidate_active ||
        candidate_token != fixture.deferred_candidate_token)
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    ++fixture.deferred_discard_count;
    fixture.deferred_candidate_active = false;
    fixture.deferred_candidate_token =
        GPU_RENDER_DEFERRED_CANDIDATE_NONE;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_deferred_transaction_begin(
        GpuRenderTransactionId transaction_id,
        uint64_t vram_mutation_serial,
        GpuRenderDeferredCandidateToken candidate_token) {
    if (!fixture.deferred_candidate_active ||
        candidate_token != fixture.deferred_candidate_token)
        return GPU_RENDER_TRANSACTION_STATE_REJECTED;
    ++fixture.deferred_begin_count;
    fixture.deferred_transaction_active = true;
    fixture.begin_id = transaction_id;
    fixture.begin_serial = vram_mutation_serial;
    return fixture.fail_begin ? GPU_RENDER_TRANSACTION_BACKEND_ERROR
                              : GPU_RENDER_TRANSACTION_OK;
}

static bool select_command(
        const GuestRenderTransactionCommandMetadata *metadata) {
    ++fixture.selector_count;
    return metadata->command_id == selected_command_id;
}

static bool select_every_command(
        const GuestRenderTransactionCommandMetadata *metadata) {
    ++fixture.selector_count;
    return metadata->list_id == 91u;
}

static bool compatibility_command(
        const GuestRenderTransactionCommandMetadata *metadata,
        const uint32_t *original_words,
        size_t original_word_count,
        GpuRenderMaterial *out_material,
        bool *out_has_material,
        void *user_data) {
    (void)user_data;
    if (!original_words || original_word_count != metadata->word_count ||
        !out_material || !out_has_material) {
        fixture.compatibility_argument_error = true;
        return false;
    }
    memset(out_material, 0, sizeof(*out_material));
    *out_has_material =
        metadata->command_id == fixture.compatibility_material_command_id;
    ++fixture.compatibility_count;
    if (metadata->command_id == fixture.modeled_bound_target_command_id)
        ++fixture.original_target_raster_count;
    fixture.runtime_state += 10;
    if (fixture.compatibility_count <=
        ARRAY_COUNT(fixture.compatibility_command_ids)) {
        fixture.compatibility_command_ids[fixture.compatibility_count - 1u] =
            metadata->command_id;
    }
    record_event(EVENT_COMPATIBILITY, metadata->command_id);
    return metadata->command_id != fixture.fail_compatibility_command;
}

static bool target_side_effects(
        const GuestRenderTransactionCommandMetadata *metadata,
        const uint32_t *original_words,
        size_t original_word_count,
        void *user_data) {
    (void)user_data;
    if (!original_words || original_word_count != metadata->word_count ||
        original_word_count > ARRAY_COUNT(fixture.target_side_effects_words)) {
        fixture.target_side_effects_argument_error = true;
        return false;
    }
    ++fixture.target_side_effects_count;
    fixture.runtime_state += 100;
    fixture.target_side_effects_command_id = metadata->command_id;
    fixture.target_side_effects_word_count = original_word_count;
    memcpy(fixture.target_side_effects_words, original_words,
           original_word_count * sizeof(original_words[0]));
    record_event(EVENT_TARGET_SIDE_EFFECTS, metadata->command_id);
    return metadata->command_id != fixture.fail_target_side_effects_command;
}

static void observe_material(
        const GuestRenderTransactionCommandMetadata *metadata,
        const GpuRenderMaterial *material, void *user_data) {
    (void)user_data;
    if (!metadata || !material) {
        fixture.material_observation_argument_error = true;
        return;
    }
    if (fixture.material_observation_count <
        ARRAY_COUNT(fixture.material_observation_command_ids)) {
        fixture.material_observation_command_ids[
            fixture.material_observation_count] = metadata->command_id;
        fixture.observed_materials[fixture.material_observation_count] =
            *material;
    }
    ++fixture.material_observation_count;
    fixture.material_observation_command_id = metadata->command_id;
    fixture.observed_material = *material;
}

static bool begin_checkpoint(
        GpuRenderTransactionId visual_id,
        uint64_t vram_mutation_serial,
        void *user_data) {
    (void)visual_id;
    (void)vram_mutation_serial;
    (void)user_data;
    ++fixture.checkpoint_begin_count;
    record_event(EVENT_CHECKPOINT_BEGIN, (uint64_t)fixture.runtime_state);
    if (fixture.fail_checkpoint_begin) return false;
    fixture.checkpoint_saved_state = fixture.runtime_state;
    fixture.checkpoint_live = true;
    return true;
}

static bool rollback_checkpoint(
        GpuRenderTransactionId visual_id,
        uint64_t vram_mutation_serial,
        void *user_data) {
    (void)visual_id;
    (void)vram_mutation_serial;
    (void)user_data;
    ++fixture.checkpoint_rollback_count;
    record_event(EVENT_CHECKPOINT_ROLLBACK,
                 (uint64_t)fixture.checkpoint_saved_state);
    if (fixture.fail_checkpoint_rollback) return false;
    fixture.runtime_state = fixture.checkpoint_saved_state;
    fixture.checkpoint_live = false;
    return true;
}

static bool commit_checkpoint(
        GpuRenderTransactionId visual_id,
        uint64_t vram_mutation_serial,
        void *user_data) {
    (void)visual_id;
    (void)vram_mutation_serial;
    (void)user_data;
    ++fixture.checkpoint_commit_count;
    record_event(EVENT_CHECKPOINT_COMMIT, (uint64_t)fixture.runtime_state);
    if (fixture.fail_checkpoint_commit) return false;
    fixture.committed_runtime_state = fixture.runtime_state;
    fixture.runtime_state_committed = true;
    fixture.checkpoint_live = false;
    return true;
}

static bool replay_original(
        const GuestRenderTransactionReplayJournal *journal,
        void *user_data) {
    GpuRenderMaterial material;

    (void)user_data;
    ++fixture.replay_count;
    fixture.runtime_state_at_replay = fixture.runtime_state;
    fixture.runtime_state += 1000;
    record_event(EVENT_REPLAY, journal->command_count);
    fixture.replayed_command_count = journal->command_count;
    fixture.replayed_word_count = journal->word_count;
    if (journal->command_count <= ARRAY_COUNT(fixture.replayed_commands)) {
        memcpy(fixture.replayed_commands, journal->commands,
               journal->command_count * sizeof(journal->commands[0]));
    }
    if (journal->word_count <= ARRAY_COUNT(fixture.replayed_words)) {
        memcpy(fixture.replayed_words, journal->words,
               journal->word_count * sizeof(journal->words[0]));
    }
    if (fixture.report_replay_material) {
        memset(&material, 0, sizeof(material));
        for (size_t index = 0u; index < journal->command_count; ++index) {
            if (!guest_render_transaction_note_replay_material(
                    &journal->commands[index], &material))
                return false;
        }
    }
    return !fixture.fail_replay;
}

static GpuRenderSemantic make_semantic(int32_t x) {
    GpuRenderSemantic semantic;

    memset(&semantic, 0, sizeof(semantic));
    semantic.triangle_count = 1u;
    semantic.triangles[0].split_index = 0u;
    semantic.triangles[0].split_count = 1u;
    semantic.triangles[0].vertices[0].x = x;
    return semantic;
}

static GpuRenderPresent make_present(void) {
    GpuRenderPresent present;

    memset(&present, 0, sizeof(present));
    present.path = GPU_RENDER_PRESENT_HIRES;
    present.display_width = 640;
    present.display_height = 480;
    present.surface_width = 640u;
    present.surface_height = 480u;
    present.scale = 2u;
    return present;
}

static GpuRenderTransactionId make_visual_id(void) {
    const GpuRenderTransactionId id = { 7u, 3u };

    return id;
}

static GuestRenderTransactionCommandMetadata make_command(
        GuestRenderTransactionSource source,
        uint64_t command_id,
        uint64_t predecessor,
        uint64_t successor,
        size_t ordinal,
        size_t word_offset,
        size_t word_count) {
    GuestRenderTransactionCommandMetadata command;

    memset(&command, 0, sizeof(command));
    command.source = source;
    command.list_id = 91u;
    command.command_id = command_id;
    command.predecessor_command_id = predecessor;
    command.successor_command_id = successor;
    command.ordinal = ordinal;
    command.word_offset = word_offset;
    command.word_count = word_count;
    return command;
}

static void make_journal_fixture(JournalFixture *data) {
    memset(data, 0, sizeof(*data));
    data->commands[0] = make_command(
        GUEST_RENDER_TRANSACTION_SOURCE_OT, 11u,
        GUEST_RENDER_TRANSACTION_NO_COMMAND, 22u, 0u, 0u, 2u);
    data->commands[1] = make_command(
        GUEST_RENDER_TRANSACTION_SOURCE_DMA, 22u, 11u, 33u, 1u, 2u, 3u);
    data->commands[2] = make_command(
        GUEST_RENDER_TRANSACTION_SOURCE_MMIO, 33u, 22u,
        GUEST_RENDER_TRANSACTION_NO_COMMAND, 2u, 5u, 1u);
    data->words[0] = UINT32_C(0x01020304);
    data->words[1] = UINT32_C(0xa1a2a3a4);
    data->words[2] = UINT32_C(0x11121314);
    data->words[3] = UINT32_C(0x21222324);
    data->words[4] = UINT32_C(0x31323334);
    data->words[5] = UINT32_C(0xf1f2f3f4);
    data->journal.visual_id = make_visual_id();
    data->journal.vram_mutation_serial = 55u;
    data->journal.list_id = 91u;
    data->journal.commands = data->commands;
    data->journal.command_count = 3u;
    data->journal.words = data->words;
    data->journal.word_count = 6u;
    data->journal.complete = true;
    selected_command_id = 22u;
    data->bindings[0].selector = select_command;
    data->bindings[0].semantic = make_semantic(1234);
    data->request.journal = &data->journal;
    data->request.bindings = data->bindings;
    data->request.binding_count = 1u;
    data->request.current_visual_id = make_visual_id();
    data->request.current_vram_mutation_serial = 55u;
    data->request.compatibility_callback = compatibility_command;
    data->request.target_side_effects_callback = target_side_effects;
    data->request.material_observation_callback = observe_material;
    data->request.begin_checkpoint = begin_checkpoint;
    data->request.rollback_checkpoint = rollback_checkpoint;
    data->request.commit_checkpoint = commit_checkpoint;
    data->request.replay_callback = replay_original;
}

static GuestRenderTransactionPendingExecuteRequest make_pending_request(
        const JournalFixture *data) {
    GuestRenderTransactionPendingExecuteRequest request;

    memset(&request, 0, sizeof(request));
    request.journal = &data->journal;
    request.current_visual_id = data->request.current_visual_id;
    request.current_vram_mutation_serial =
        data->request.current_vram_mutation_serial;
    request.compatibility_callback = data->request.compatibility_callback;
    request.compatibility_user_data = data->request.compatibility_user_data;
    request.target_side_effects_callback =
        data->request.target_side_effects_callback;
    request.target_side_effects_user_data =
        data->request.target_side_effects_user_data;
    request.material_observation_callback =
        data->request.material_observation_callback;
    request.material_observation_user_data =
        data->request.material_observation_user_data;
    request.begin_checkpoint = data->request.begin_checkpoint;
    request.rollback_checkpoint = data->request.rollback_checkpoint;
    request.commit_checkpoint = data->request.commit_checkpoint;
    request.checkpoint_user_data = data->request.checkpoint_user_data;
    request.replay_callback = data->request.replay_callback;
    request.replay_user_data = data->request.replay_user_data;
    return request;
}

static void reset_fixture(void) {
    guest_render_transaction_test_reset();
    memset(&fixture, 0, sizeof(fixture));
}

static int snapshot(GuestRenderTransactionSnapshot *out_snapshot) {
    return guest_render_transaction_snapshot(out_snapshot) ==
           GUEST_RENDER_TRANSACTION_OK;
}

static int pending_snapshot(
        GuestRenderTransactionPendingSnapshot *out_snapshot) {
    return guest_render_transaction_pending_snapshot(out_snapshot) ==
           GUEST_RENDER_TRANSACTION_OK;
}

static int test_happy_slot_replacement_and_publication(void) {
    static const Event expected[] = {
        { EVENT_CHECKPOINT_BEGIN, 0u },
        { EVENT_BEGIN, 55u },
        { EVENT_BARRIER, 1u },
        { EVENT_COMPATIBILITY, 11u },
        { EVENT_BARRIER, 2u },
        { EVENT_TARGET_SIDE_EFFECTS, 22u },
        { EVENT_DRAW, 1234u },
        { EVENT_BARRIER, 3u },
        { EVENT_COMPATIBILITY, 33u },
        { EVENT_BARRIER, 4u },
        { EVENT_COMMIT, 55u },
    };
    JournalFixture data;
    make_journal_fixture(&data);
    GuestRenderTransactionSnapshot state;
    GpuRenderPresent present = make_present();

    reset_fixture();
    fixture.modeled_bound_target_command_id = 22u;
    fixture.compatibility_material_command_id = 11u;
    CHECK(guest_render_transaction_process_owner() != NULL);
    CHECK(guest_render_transaction_process_owner() ==
          guest_render_transaction_process_owner());
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(fixture.selector_count == 3u);
    CHECK(fixture.compatibility_count == 2u);
    CHECK(fixture.compatibility_command_ids[0] == 11u);
    CHECK(fixture.compatibility_command_ids[1] == 33u);
    CHECK(fixture.target_side_effects_count == 1u);
    CHECK(fixture.target_side_effects_command_id == 22u);
    CHECK(fixture.target_side_effects_word_count == 3u);
    CHECK(fixture.target_side_effects_words[0] == UINT32_C(0x11121314));
    CHECK(fixture.target_side_effects_words[1] == UINT32_C(0x21222324));
    CHECK(fixture.target_side_effects_words[2] == UINT32_C(0x31323334));
    CHECK(!fixture.target_side_effects_argument_error);
    CHECK(fixture.original_target_raster_count == 0u);
    CHECK(!fixture.compatibility_argument_error);
    CHECK(fixture.draw_count == 1u);
    CHECK(fixture.drawn[0].triangles[0].vertices[0].x == 1234);
    CHECK(fixture.material_observation_count == 0u);
    CHECK(fixture.begin_id.scene_epoch == make_visual_id().scene_epoch);
    CHECK(fixture.begin_id.state_sequence == make_visual_id().state_sequence);
    CHECK(fixture.begin_serial == 55u);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_ACTIVE);
    CHECK(state.active_visual_id.scene_epoch == make_visual_id().scene_epoch);
    CHECK(state.active_visual_id.state_sequence ==
          make_visual_id().state_sequence);
    CHECK(state.active_vram_mutation_serial == 55u);
    CHECK(state.published_transaction_count == 0u);
    CHECK(state.published_substitution_count == 0u);
    CHECK(fixture.checkpoint_commit_count == 0u);
    CHECK(!fixture.runtime_state_committed);
    CHECK(guest_render_transaction_present(make_visual_id(), 55u, &present) ==
          GUEST_RENDER_TRANSACTION_READY);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_AWAITING_SWAP);
    CHECK(state.published_transaction_count == 0u);
    CHECK(state.published_substitution_count == 0u);
    CHECK(fixture.event_count == ARRAY_COUNT(expected));
    CHECK(memcmp(fixture.events, expected, sizeof(expected)) == 0);
    CHECK(guest_render_transaction_post_swap_success() ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(fixture.checkpoint_commit_count == 1u);
    CHECK(fixture.checkpoint_rollback_count == 0u);
    CHECK(fixture.material_observation_count == 2u);
    CHECK(fixture.material_observation_command_id == 22u);
    CHECK(fixture.material_observation_command_ids[0] == 11u);
    CHECK(fixture.material_observation_command_ids[1] == 22u);
    CHECK(!fixture.material_observation_argument_error);
    CHECK(memcmp(&fixture.observed_materials[1],
                 &data.bindings[0].semantic.material,
                 sizeof(fixture.observed_materials[1])) == 0);
    CHECK(fixture.runtime_state_committed);
    CHECK(fixture.committed_runtime_state == fixture.runtime_state);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_IDLE);
    CHECK(state.published_transaction_count == 1u);
    CHECK(state.published_substitution_count == 1u);
    CHECK(guest_render_transaction_post_swap_success() ==
          GUEST_RENDER_TRANSACTION_INVALID_TRANSITION);
    CHECK(fixture.rollback_count == 0u && fixture.replay_count == 0u);
    return 0;
}

static int test_private_byte_identical_adjacent_payload_replay(void) {
    static const uint32_t expected_words[] = {
        UINT32_C(0x01020304), UINT32_C(0xa1a2a3a4),
        UINT32_C(0x11121314), UINT32_C(0x21222324),
        UINT32_C(0x31323334), UINT32_C(0xf1f2f3f4),
    };
    JournalFixture data;
    make_journal_fixture(&data);

    reset_fixture();
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    memset(data.words, 0x5a, sizeof(data.words));
    memset(data.commands, 0xa5, sizeof(data.commands));
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_LATE_COMMAND) ==
          GUEST_RENDER_TRANSACTION_ABORTED);
    CHECK(fixture.material_observation_count == 0u);
    CHECK(fixture.rollback_count == 1u);
    CHECK(fixture.replay_count == 1u);
    CHECK(fixture.replayed_command_count == 3u);
    CHECK(fixture.replayed_word_count == ARRAY_COUNT(expected_words));
    CHECK(memcmp(fixture.replayed_words, expected_words,
                 sizeof(expected_words)) == 0);
    CHECK(fixture.replayed_commands[0].word_offset == 0u);
    CHECK(fixture.replayed_commands[0].word_count == 2u);
    CHECK(fixture.replayed_commands[1].word_offset == 2u);
    CHECK(fixture.replayed_commands[1].word_count == 3u);
    CHECK(fixture.replayed_commands[2].word_offset == 5u);
    CHECK(fixture.replayed_commands[2].word_count == 1u);
    CHECK(fixture.replayed_commands[0].source ==
          GUEST_RENDER_TRANSACTION_SOURCE_OT);
    CHECK(fixture.replayed_commands[1].source ==
          GUEST_RENDER_TRANSACTION_SOURCE_DMA);
    CHECK(fixture.replayed_commands[2].source ==
          GUEST_RENDER_TRANSACTION_SOURCE_MMIO);
    CHECK(fixture.source_side_effect_count == 0u);
    CHECK(fixture.event_count >= 2u);
    CHECK(fixture.events[fixture.event_count - 3u].kind == EVENT_ROLLBACK);
    CHECK(fixture.events[fixture.event_count - 2u].kind ==
          EVENT_CHECKPOINT_ROLLBACK);
    CHECK(fixture.events[fixture.event_count - 1u].kind == EVENT_REPLAY);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_LATE_COMMAND) ==
          GUEST_RENDER_TRANSACTION_INVALID_TRANSITION);
    CHECK(fixture.replay_count == 1u);
    return 0;
}

static int test_preflight_links_targets_and_completeness(void) {
    JournalFixture data;

    reset_fixture();
    fixture.modeled_bound_target_command_id = 11u;
    make_journal_fixture(&data);
    data.commands[1].predecessor_command_id =
        GUEST_RENDER_TRANSACTION_NO_COMMAND;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_INVALID_LINK);
    CHECK(fixture.begin_count == 0u && fixture.selector_count == 0u);
    CHECK(fixture.replay_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    data.bindings[1] = data.bindings[0];
    data.bindings[1].semantic = make_semantic(5678);
    data.request.binding_count = 2u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_DUPLICATE_TARGET);
    CHECK(fixture.begin_count == 0u && fixture.replay_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    data.commands[1].ordinal = 2u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_INVALID_ORDER);
    CHECK(fixture.begin_count == 0u && fixture.selector_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    selected_command_id = 99u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_TARGET_NOT_FOUND);
    CHECK(fixture.begin_count == 0u && fixture.selector_count == 3u);

    reset_fixture();
    make_journal_fixture(&data);
    data.bindings[0].selector = select_every_command;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_DUPLICATE_TARGET);
    CHECK(fixture.begin_count == 0u && fixture.selector_count == 3u);

    reset_fixture();
    make_journal_fixture(&data);
    data.journal.complete = false;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_JOURNAL_INCOMPLETE);
    CHECK(fixture.begin_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    data.commands[1].word_offset = 3u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_JOURNAL_INCOMPLETE);
    CHECK(fixture.begin_count == 0u);
    return 0;
}

static int test_exact_binding_matching_contract(void) {
    JournalFixture data;

    reset_fixture();
    make_journal_fixture(&data);
    data.bindings[0].selector = NULL;
    data.bindings[0].has_exact_command_id = true;
    data.bindings[0].exact_command_id = 22u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(fixture.selector_count == 0u);
    CHECK(fixture.draw_count == 1u);
    CHECK(fixture.drawn[0].triangles[0].vertices[0].x == 1234);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);

    reset_fixture();
    make_journal_fixture(&data);
    data.bindings[0].selector = NULL;
    data.bindings[0].has_exact_command_id = true;
    data.bindings[0].exact_command_id = 99u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_TARGET_NOT_FOUND);
    CHECK(fixture.begin_count == 0u && fixture.selector_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    data.bindings[0].selector = NULL;
    data.bindings[0].has_exact_command_id = true;
    data.bindings[0].exact_command_id = 22u;
    data.bindings[1] = data.bindings[0];
    data.bindings[1].semantic = make_semantic(5678);
    data.request.binding_count = 2u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_DUPLICATE_TARGET);
    CHECK(fixture.begin_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    data.bindings[0].has_exact_command_id = true;
    data.bindings[0].exact_command_id = 22u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(fixture.begin_count == 0u && fixture.selector_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    data.bindings[0].selector = NULL;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(fixture.begin_count == 0u);
    return 0;
}

static int test_target_side_effects_and_adjacent_compatibility_order(void) {
    static const Event expected[] = {
        { EVENT_CHECKPOINT_BEGIN, 0u },
        { EVENT_BEGIN, 55u },
        { EVENT_BARRIER, 1u },
        { EVENT_TARGET_SIDE_EFFECTS, 11u },
        { EVENT_DRAW, 1234u },
        { EVENT_BARRIER, 2u },
        { EVENT_COMPATIBILITY, 22u },
        { EVENT_BARRIER, 3u },
        { EVENT_COMPATIBILITY, 33u },
        { EVENT_BARRIER, 4u },
    };
    JournalFixture data;

    reset_fixture();
    make_journal_fixture(&data);
    data.bindings[0].selector = NULL;
    data.bindings[0].has_exact_command_id = true;
    data.bindings[0].exact_command_id = 11u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(fixture.selector_count == 0u);
    CHECK(fixture.target_side_effects_count == 1u);
    CHECK(fixture.target_side_effects_command_id == 11u);
    CHECK(fixture.target_side_effects_word_count == 2u);
    CHECK(fixture.target_side_effects_words[0] == UINT32_C(0x01020304));
    CHECK(fixture.target_side_effects_words[1] == UINT32_C(0xa1a2a3a4));
    CHECK(fixture.compatibility_count == 2u);
    CHECK(fixture.compatibility_command_ids[0] == 22u);
    CHECK(fixture.compatibility_command_ids[1] == 33u);
    CHECK(fixture.original_target_raster_count == 0u);
    CHECK(fixture.event_count == ARRAY_COUNT(expected));
    CHECK(memcmp(fixture.events, expected, sizeof(expected)) == 0);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);

    reset_fixture();
    make_journal_fixture(&data);
    data.request.target_side_effects_callback = NULL;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(fixture.begin_count == 0u);
    CHECK(fixture.selector_count == 0u);
    CHECK(fixture.target_side_effects_count == 0u);
    return 0;
}

static int test_target_side_effects_failure_rolls_back_and_replays(void) {
    JournalFixture data;
    make_journal_fixture(&data);
    GuestRenderTransactionSnapshot state;

    reset_fixture();
    fixture.modeled_bound_target_command_id = 22u;
    fixture.runtime_state = 7;
    fixture.fail_target_side_effects_command = 22u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_TARGET_SIDE_EFFECTS_FAILURE);
    CHECK(fixture.target_side_effects_count == 1u);
    CHECK(fixture.draw_count == 0u);
    CHECK(fixture.original_target_raster_count == 0u);
    CHECK(fixture.rollback_count == 1u);
    CHECK(fixture.checkpoint_rollback_count == 1u);
    CHECK(fixture.replay_count == 1u);
    CHECK(fixture.material_observation_count == 0u);
    CHECK(fixture.checkpoint_saved_state == 7);
    CHECK(fixture.runtime_state_at_replay == 7);
    CHECK(fixture.runtime_state == 1007);
    CHECK(fixture.event_count >= 4u);
    CHECK(fixture.events[fixture.event_count - 4u].kind ==
          EVENT_TARGET_SIDE_EFFECTS);
    CHECK(fixture.events[fixture.event_count - 3u].kind == EVENT_ROLLBACK);
    CHECK(fixture.events[fixture.event_count - 2u].kind ==
          EVENT_CHECKPOINT_ROLLBACK);
    CHECK(fixture.events[fixture.event_count - 1u].kind == EVENT_REPLAY);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_ROLLED_BACK);
    CHECK(state.abort_reason ==
          GUEST_RENDER_TRANSACTION_OBSERVATION_TARGET_SIDE_EFFECTS_FAILURE);
    CHECK(state.last_status ==
          GUEST_RENDER_TRANSACTION_TARGET_SIDE_EFFECTS_FAILURE);
    CHECK(state.active_command_count == 0u);
    CHECK(state.active_binding_count == 0u);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_INVALID_TRANSITION);
    CHECK(fixture.checkpoint_rollback_count == 1u);
    return 0;
}

static int test_preflight_stale_state_serial_and_capacities(void) {
    JournalFixture data;

    reset_fixture();
    make_journal_fixture(&data);
    data.request.current_visual_id.state_sequence += 1u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID);
    CHECK(fixture.begin_count == 0u && fixture.replay_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    data.request.current_vram_mutation_serial += 1u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_STALE_VRAM_SERIAL);
    CHECK(fixture.begin_count == 0u && fixture.replay_count == 0u);

    CHECK(guest_render_transaction_command_capacity() == 4u);
    CHECK(guest_render_transaction_word_capacity() == 16u);
    CHECK(guest_render_transaction_binding_capacity() == 2u);
    CHECK(guest_render_transaction_pending_capacity() == 2u);

    reset_fixture();
    make_journal_fixture(&data);
    data.journal.command_count = 5u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED);

    reset_fixture();
    make_journal_fixture(&data);
    data.journal.word_count = 17u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED);

    reset_fixture();
    make_journal_fixture(&data);
    data.request.binding_count = 3u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED);
    CHECK(fixture.begin_count == 0u && fixture.replay_count == 0u);
    return 0;
}

static int test_late_command_aborts_before_trigger(void) {
    JournalFixture data;
    make_journal_fixture(&data);
    GuestRenderTransactionSnapshot state;

    reset_fixture();
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(fixture.trigger_count == 0u);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_LATE_COMMAND) ==
          GUEST_RENDER_TRANSACTION_ABORTED);
    CHECK(fixture.trigger_count == 0u);
    ++fixture.trigger_count;
    CHECK(fixture.trigger_count == 1u);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_ROLLED_BACK);
    CHECK(state.abort_reason ==
          GUEST_RENDER_TRANSACTION_OBSERVATION_LATE_COMMAND);
    CHECK(state.published_transaction_count == 0u);
    CHECK(state.published_substitution_count == 0u);
    CHECK(fixture.material_observation_count == 0u);
    return 0;
}

static int test_pending_staging_validation_and_capacity(void) {
    GpuRenderTransactionId id = make_visual_id();
    GpuRenderTransactionId other_id = make_visual_id();
    GpuRenderSemantic semantic = make_semantic(7001);
    GpuRenderSemantic invalid_semantic;
    GuestRenderTransactionPendingSnapshot pending;

    reset_fixture();
    CHECK(pending_snapshot(&pending));
    CHECK(pending.binding_count == 0u);
    CHECK(pending.visual_id.scene_epoch == 0u);
    CHECK(pending.visual_id.state_sequence == 0u);
    id.scene_epoch = 0u;
    CHECK(guest_render_transaction_stage_exact(id, 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);

    reset_fixture();
    id = make_visual_id();
    CHECK(guest_render_transaction_stage_exact(
              id, GUEST_RENDER_TRANSACTION_NO_COMMAND, &semantic) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    memset(&invalid_semantic, 0, sizeof(invalid_semantic));
    CHECK(guest_render_transaction_stage_exact(id, 22u, &invalid_semantic) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    invalid_semantic = make_semantic(7002);
    invalid_semantic.material.blend_mode = (GpuRenderBlendMode)99;
    CHECK(guest_render_transaction_stage_exact(id, 22u, &invalid_semantic) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);

    reset_fixture();
    CHECK(guest_render_transaction_stage_exact(id, 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending_snapshot(&pending));
    CHECK(pending.binding_count == 1u);
    CHECK(pending.visual_id.scene_epoch == id.scene_epoch);
    CHECK(pending.visual_id.state_sequence == id.state_sequence);
    CHECK(guest_render_transaction_stage_exact(id, 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_DUPLICATE_TARGET);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);

    reset_fixture();
    CHECK(guest_render_transaction_stage_exact(id, 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    ++other_id.state_sequence;
    CHECK(guest_render_transaction_stage_exact(other_id, 33u, &semantic) ==
          GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);

    reset_fixture();
    CHECK(guest_render_transaction_stage_exact(id, 11u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_stage_exact(id, 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_stage_exact(id, 33u, &semantic) ==
          GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);

    CHECK(guest_render_transaction_stage_exact(id, 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    guest_render_transaction_clear_pending();
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);
    CHECK(guest_render_transaction_pending_snapshot(NULL) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    return 0;
}

static int test_pending_execute_lifecycle_and_no_word_channel(void) {
    JournalFixture data;
    GuestRenderTransactionPendingExecuteRequest request;
    GuestRenderTransactionPendingSnapshot pending;
    GuestRenderTransactionSnapshot state;
    GpuRenderSemantic semantic = make_semantic(4321);
    GpuRenderSemantic unmatched_semantic = make_semantic(8765);

    reset_fixture();
    make_journal_fixture(&data);
    request = make_pending_request(&data);
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    semantic.triangles[0].vertices[0].x = 9999;
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(fixture.selector_count == 0u);
    CHECK(fixture.draw_count == 1u);
    CHECK(fixture.drawn[0].triangles[0].vertices[0].x == 4321);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_ROLLED_BACK);
    CHECK(state.active_command_count == 0u);
    CHECK(state.active_binding_count == 0u);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);

    reset_fixture();
    semantic = make_semantic(4321);
    make_journal_fixture(&data);
    request = make_pending_request(&data);
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 99u, &unmatched_semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(fixture.draw_count == 1u);
    CHECK(fixture.drawn[0].triangles[0].vertices[0].x == 4321);
    CHECK(snapshot(&state));
    CHECK(state.active_binding_count == 1u);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);

    reset_fixture();
    make_journal_fixture(&data);
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 11u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 33u, &semantic) ==
          GUEST_RENDER_TRANSACTION_INVALID_TRANSITION);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);
    return 0;
}

static int test_pending_retry_and_unsafe_clear_rules(void) {
    JournalFixture data;
    GuestRenderTransactionPendingExecuteRequest request;
    GuestRenderTransactionPendingSnapshot pending;
    GpuRenderSemantic semantic = make_semantic(8080);

    reset_fixture();
    make_journal_fixture(&data);
    request = make_pending_request(&data);
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    data.journal.complete = false;
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_JOURNAL_INCOMPLETE);
    CHECK(fixture.begin_count == 0u);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 1u);
    data.journal.complete = true;
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);

    reset_fixture();
    make_journal_fixture(&data);
    request = make_pending_request(&data);
    request.replay_callback = NULL;
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 1u);
    request.replay_callback = replay_original;
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);

    reset_fixture();
    make_journal_fixture(&data);
    request = make_pending_request(&data);
    request.target_side_effects_callback = NULL;
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(fixture.begin_count == 0u);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 1u);
    request.target_side_effects_callback = target_side_effects;
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);

    reset_fixture();
    make_journal_fixture(&data);
    request = make_pending_request(&data);
    request.rollback_checkpoint = NULL;
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(fixture.checkpoint_begin_count == 0u);
    CHECK(fixture.begin_count == 0u);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 1u);
    request.rollback_checkpoint = rollback_checkpoint;
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);

    reset_fixture();
    make_journal_fixture(&data);
    request = make_pending_request(&data);
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 99u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_TARGET_NOT_FOUND);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 1u);
    data.commands[1].successor_command_id = 99u;
    data.commands[2].command_id = 99u;
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);

    reset_fixture();
    make_journal_fixture(&data);
    request = make_pending_request(&data);
    request.current_vram_mutation_serial = 56u;
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_STALE_VRAM_SERIAL);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    request = make_pending_request(&data);
    ++request.current_visual_id.state_sequence;
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID);
    CHECK(fixture.begin_count == 0u);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    request = make_pending_request(&data);
    fixture.fail_begin = true;
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 22u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute_pending(&request) ==
          GUEST_RENDER_TRANSACTION_BACKEND_FAILURE);
    CHECK(fixture.begin_count == 1u);
    CHECK(fixture.rollback_count == 0u);
    CHECK(fixture.checkpoint_rollback_count == 1u);
    CHECK(fixture.replay_count == 1u);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);
    return 0;
}

static int test_abort_and_post_swap_clear_matching_pending(void) {
    JournalFixture data;
    GuestRenderTransactionPendingSnapshot pending;
    GuestRenderTransactionSnapshot state;
    GpuRenderSemantic semantic = make_semantic(6060);
    GpuRenderPresent present = make_present();

    reset_fixture();
    make_journal_fixture(&data);
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 11u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);
    CHECK(snapshot(&state));
    CHECK(state.active_command_count == 0u);
    CHECK(state.active_binding_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 11u, &semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_present(make_visual_id(), 55u, &present) ==
          GUEST_RENDER_TRANSACTION_READY);
    CHECK(guest_render_transaction_post_swap_success() ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_IDLE);
    CHECK(state.active_command_count == 0u);
    CHECK(state.active_binding_count == 0u);
    return 0;
}

static int test_checkpoint_failures_are_atomic_and_unique(void) {
    JournalFixture data;
    GuestRenderTransactionSnapshot state;
    GpuRenderPresent present = make_present();

    reset_fixture();
    make_journal_fixture(&data);
    data.request.begin_checkpoint = NULL;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(fixture.checkpoint_begin_count == 0u);
    CHECK(fixture.begin_count == 0u);
    CHECK(fixture.replay_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.runtime_state = 3;
    fixture.fail_checkpoint_begin = true;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_CHECKPOINT_BEGIN_FAILURE);
    CHECK(fixture.checkpoint_begin_count == 1u);
    CHECK(fixture.begin_count == 0u);
    CHECK(fixture.rollback_count == 0u);
    CHECK(fixture.checkpoint_rollback_count == 0u);
    CHECK(fixture.replay_count == 1u);
    CHECK(fixture.runtime_state_at_replay == 3);
    CHECK(snapshot(&state));
    CHECK(state.abort_reason ==
          GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_BEGIN_FAILURE);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.runtime_state = 4;
    fixture.fail_target_side_effects_command = 22u;
    fixture.fail_checkpoint_rollback = true;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_CHECKPOINT_ROLLBACK_FAILURE);
    CHECK(fixture.rollback_count == 1u);
    CHECK(fixture.checkpoint_rollback_count == 1u);
    CHECK(fixture.replay_count == 1u);
    CHECK(fixture.runtime_state_at_replay != 4);
    CHECK(snapshot(&state));
    CHECK(state.abort_reason ==
          GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_ROLLBACK_FAILURE);
    CHECK(state.last_status ==
          GUEST_RENDER_TRANSACTION_CHECKPOINT_ROLLBACK_FAILURE);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.runtime_state = 6;
    fixture.fail_checkpoint_commit = true;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_present(make_visual_id(), 55u, &present) ==
          GUEST_RENDER_TRANSACTION_READY);
    CHECK(fixture.runtime_state != 6);
    CHECK(guest_render_transaction_post_swap_success() ==
          GUEST_RENDER_TRANSACTION_CHECKPOINT_COMMIT_FAILURE);
    CHECK(fixture.checkpoint_commit_count == 1u);
    CHECK(fixture.checkpoint_rollback_count == 1u);
    CHECK(fixture.runtime_state == 6);
    CHECK(fixture.rollback_count == 0u);
    CHECK(fixture.replay_count == 0u);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_ROLLED_BACK);
    CHECK(state.abort_reason ==
          GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_COMMIT_FAILURE);
    CHECK(state.last_status ==
          GUEST_RENDER_TRANSACTION_CHECKPOINT_COMMIT_FAILURE);
    CHECK(state.published_transaction_count == 0u);
    CHECK(state.published_substitution_count == 0u);
    CHECK(fixture.material_observation_count == 0u);
    CHECK(state.active_command_count == 0u);
    CHECK(state.active_binding_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.runtime_state = 9;
    fixture.fail_target_side_effects_command = 22u;
    fixture.fail_rollback = true;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_BACKEND_ROLLBACK_FAILURE);
    CHECK(fixture.rollback_count == 1u);
    CHECK(fixture.checkpoint_rollback_count == 0u);
    CHECK(fixture.replay_count == 0u);
    CHECK(fixture.runtime_state_at_replay == 0);
    CHECK(snapshot(&state));
    CHECK(state.abort_reason ==
          GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_ROLLBACK_FAILURE);
    CHECK(state.rollback_status == GPU_RENDER_TRANSACTION_CONTEXT_LOST);
    return 0;
}

static int test_post_swap_failure_cleans_without_rollback_or_replay(void) {
    JournalFixture data;
    make_journal_fixture(&data);
    GuestRenderTransactionPendingSnapshot pending;
    GuestRenderTransactionSnapshot state;
    GpuRenderSemantic pending_semantic = make_semantic(5151);
    GpuRenderPresent present = make_present();

    reset_fixture();
    fixture.runtime_state = 5;
    CHECK(guest_render_transaction_stage_exact(
              make_visual_id(), 11u, &pending_semantic) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(snapshot(&state));
    CHECK(state.active_visual_id.scene_epoch == make_visual_id().scene_epoch);
    CHECK(state.active_visual_id.state_sequence ==
          make_visual_id().state_sequence);
    CHECK(state.active_vram_mutation_serial == 55u);
    CHECK(guest_render_transaction_present(make_visual_id(), 55u, &present) ==
          GUEST_RENDER_TRANSACTION_READY);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_AWAITING_SWAP);
    CHECK(state.active_visual_id.scene_epoch == make_visual_id().scene_epoch);
    CHECK(state.active_vram_mutation_serial == 55u);
    CHECK(fixture.runtime_state != 5);
    CHECK(guest_render_transaction_post_swap_failure(false) ==
          GUEST_RENDER_TRANSACTION_BACKEND_PRESENT_FAILURE);
    CHECK(fixture.rollback_count == 0u);
    CHECK(fixture.checkpoint_rollback_count == 1u);
    CHECK(fixture.replay_count == 0u);
    CHECK(fixture.material_observation_count == 0u);
    CHECK(fixture.runtime_state == 5);
    CHECK(pending_snapshot(&pending) && pending.binding_count == 0u);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_ROLLED_BACK);
    CHECK(state.abort_reason ==
          GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_PRESENT_FAILURE);
    CHECK(state.last_status ==
          GUEST_RENDER_TRANSACTION_BACKEND_PRESENT_FAILURE);
    CHECK(state.backend_status == GPU_RENDER_TRANSACTION_BACKEND_ERROR);
    CHECK(state.active_visual_id.scene_epoch == 0u);
    CHECK(state.active_visual_id.state_sequence == 0u);
    CHECK(state.active_vram_mutation_serial == 0u);
    CHECK(state.active_command_count == 0u);
    CHECK(state.active_binding_count == 0u);
    CHECK(state.published_transaction_count == 0u);
    CHECK(state.published_substitution_count == 0u);
    CHECK(guest_render_transaction_post_swap_failure(false) ==
          GUEST_RENDER_TRANSACTION_INVALID_TRANSITION);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.runtime_state = 9;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_present(make_visual_id(), 55u, &present) ==
          GUEST_RENDER_TRANSACTION_READY);
    fixture.runtime_state = fixture.checkpoint_saved_state;
    fixture.checkpoint_live = false;
    CHECK(guest_render_transaction_post_swap_failure(true) ==
          GUEST_RENDER_TRANSACTION_BACKEND_PRESENT_FAILURE);
    CHECK(fixture.checkpoint_rollback_count == 0u);
    CHECK(fixture.rollback_count == 0u);
    CHECK(fixture.replay_count == 0u);
    CHECK(fixture.runtime_state == 9);

    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);
    return 0;
}

static int test_every_required_observation_reason_is_unique(void) {
    static const GuestRenderTransactionObservationReason reasons[] = {
        GUEST_RENDER_TRANSACTION_OBSERVATION_GP1,
        GUEST_RENDER_TRANSACTION_OBSERVATION_GPUSTAT,
        GUEST_RENDER_TRANSACTION_OBSERVATION_GPUREAD,
        GUEST_RENDER_TRANSACTION_OBSERVATION_DMA2_GPU_TO_RAM_C0,
        GUEST_RENDER_TRANSACTION_OBSERVATION_DELAYED_COMPLETION,
        GUEST_RENDER_TRANSACTION_OBSERVATION_IRQ,
        GUEST_RENDER_TRANSACTION_OBSERVATION_LATE_MMIO,
        GUEST_RENDER_TRANSACTION_OBSERVATION_SECOND_LIST,
        GUEST_RENDER_TRANSACTION_OBSERVATION_TARGET_SIDE_EFFECTS_FAILURE,
        GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE,
        GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_PRESENT_FAILURE,
        GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_BEGIN_FAILURE,
        GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_ROLLBACK_FAILURE,
        GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_COMMIT_FAILURE,
    };
    size_t index;
    size_t other_index;

    for (index = 0u; index < ARRAY_COUNT(reasons); ++index) {
        JournalFixture data;
        make_journal_fixture(&data);
        GuestRenderTransactionSnapshot state;
        const char *name;

        for (other_index = 0u; other_index < index; ++other_index)
            CHECK(reasons[index] != reasons[other_index]);
        name = guest_render_transaction_observation_reason_name(reasons[index]);
        CHECK(name != NULL && strcmp(name, "unknown") != 0);
        reset_fixture();
        CHECK(guest_render_transaction_execute(&data.request) ==
              GUEST_RENDER_TRANSACTION_OK);
        CHECK(guest_render_transaction_abort_before_observation(reasons[index]) ==
              GUEST_RENDER_TRANSACTION_ABORTED);
        CHECK(fixture.rollback_count == 1u && fixture.replay_count == 1u);
        CHECK(snapshot(&state));
        CHECK(state.abort_reason == reasons[index]);
        CHECK(state.published_transaction_count == 0u);
        CHECK(state.published_substitution_count == 0u);
    }
    CHECK(strcmp(guest_render_transaction_observation_reason_name(UINT32_MAX),
                 "unknown") == 0);
    return 0;
}

static int test_compatibility_and_backend_failures_replay_once(void) {
    JournalFixture data;
    GuestRenderTransactionSnapshot state;
    GpuRenderPresent present = make_present();

    reset_fixture();
    make_journal_fixture(&data);
    fixture.fail_compatibility_command = 33u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_COMPATIBILITY_FAILURE);
    CHECK(fixture.draw_count == 1u);
    CHECK(fixture.rollback_count == 1u && fixture.replay_count == 1u);
    CHECK(snapshot(&state));
    CHECK(state.abort_reason ==
          GUEST_RENDER_TRANSACTION_OBSERVATION_COMPATIBILITY_FAILURE);
    CHECK(state.published_substitution_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.fail_begin = true;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_BACKEND_FAILURE);
    CHECK(fixture.begin_count == 1u);
    CHECK(fixture.rollback_count == 0u && fixture.replay_count == 1u);
    CHECK(fixture.checkpoint_rollback_count == 1u);
    CHECK(fixture.event_count == 4u);
    CHECK(fixture.events[0].kind == EVENT_CHECKPOINT_BEGIN);
    CHECK(fixture.events[1].kind == EVENT_BEGIN);
    CHECK(fixture.events[2].kind == EVENT_CHECKPOINT_ROLLBACK);
    CHECK(fixture.events[3].kind == EVENT_REPLAY);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.fail_draw_call = 1u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_BACKEND_FAILURE);
    CHECK(fixture.rollback_count == 1u && fixture.replay_count == 1u);
    CHECK(snapshot(&state));
    CHECK(state.published_transaction_count == 0u);
    CHECK(state.published_substitution_count == 0u);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.fail_barrier_call = 2u;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_BACKEND_FAILURE);
    CHECK(fixture.rollback_count == 1u && fixture.replay_count == 1u);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.fail_commit = true;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_present(make_visual_id(), 55u, &present) ==
          GUEST_RENDER_TRANSACTION_BACKEND_FAILURE);
    CHECK(fixture.commit_count == 1u);
    CHECK(fixture.rollback_count == 1u && fixture.replay_count == 1u);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_GP1) ==
          GUEST_RENDER_TRANSACTION_INVALID_TRANSITION);
    CHECK(fixture.replay_count == 1u);
    return 0;
}

static int test_present_revalidates_state_and_serial(void) {
    JournalFixture data;
    GpuRenderTransactionId stale_id;
    GpuRenderPresent present = make_present();
    GuestRenderTransactionSnapshot state;

    reset_fixture();
    make_journal_fixture(&data);
    fixture.report_replay_material = true;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    stale_id = make_visual_id();
    ++stale_id.state_sequence;
    CHECK(guest_render_transaction_present(stale_id, 55u, &present) ==
          GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID);
    CHECK(fixture.commit_count == 0u);
    CHECK(fixture.rollback_count == 1u && fixture.replay_count == 1u);
    CHECK(fixture.material_observation_count == 3u);
    CHECK(snapshot(&state));
    CHECK(state.abort_reason ==
          GUEST_RENDER_TRANSACTION_OBSERVATION_VISUAL_ID_CHANGED);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.report_replay_material = true;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_present(make_visual_id(), 56u, &present) ==
          GUEST_RENDER_TRANSACTION_STALE_VRAM_SERIAL);
    CHECK(fixture.commit_count == 0u);
    CHECK(fixture.rollback_count == 1u && fixture.replay_count == 1u);
    CHECK(snapshot(&state));
    CHECK(state.abort_reason ==
          GUEST_RENDER_TRANSACTION_OBSERVATION_VRAM_SERIAL_CHANGED);
    CHECK(state.published_transaction_count == 0u);
    CHECK(state.published_substitution_count == 0u);
    return 0;
}

static int test_gpustat_abort_deferred_retry_lifecycle(void) {
    JournalFixture data;
    GuestRenderTransactionDeferredSnapshot deferred;
    GuestRenderTransactionSnapshot state;
    GpuRenderPresent present = make_present();
    GpuRenderTransactionId stale_id;

    reset_fixture();
    make_journal_fixture(&data);
    fixture.report_replay_material = true;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_GPUSTAT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);
    CHECK(fixture.deferred_capture_count == 1u);
    CHECK(fixture.rollback_count == 1u);
    CHECK(fixture.checkpoint_rollback_count == 1u);
    CHECK(fixture.replay_count == 1u);
    CHECK(fixture.material_observation_count == 0u);
    CHECK(fixture.runtime_state == 1000);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_ROLLED_BACK);
    CHECK(state.abort_reason == GUEST_RENDER_TRANSACTION_OBSERVATION_GPUSTAT);
    CHECK(state.published_transaction_count == 0u);
    CHECK(state.published_substitution_count == 0u);
    CHECK(guest_render_transaction_deferred_snapshot(&deferred) ==
          GUEST_RENDER_TRANSACTION_OK && !deferred.sealed);

    CHECK(guest_render_transaction_seal_deferred_retry(77u) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_deferred_snapshot(&deferred) ==
          GUEST_RENDER_TRANSACTION_OK && deferred.sealed);
    CHECK(deferred.visual_id.scene_epoch == make_visual_id().scene_epoch);
    CHECK(deferred.visual_id.state_sequence == make_visual_id().state_sequence);
    CHECK(deferred.post_replay_vram_mutation_serial == 77u);
    CHECK(deferred.binding_count == 1u);
    CHECK(guest_render_transaction_seal_deferred_retry(77u) ==
          GUEST_RENDER_TRANSACTION_INVALID_TRANSITION);

    CHECK(guest_render_transaction_begin_deferred(make_visual_id(), 77u) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(fixture.deferred_begin_count == 1u);
    CHECK(fixture.checkpoint_begin_count == 2u);
    CHECK(fixture.replay_count == 1u);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_ACTIVE);
    CHECK(state.active_binding_count == 1u);
    CHECK(state.active_command_count == 0u);
    CHECK(state.published_transaction_count == 0u);
    CHECK(state.published_substitution_count == 0u);
    CHECK(fixture.material_observation_count == 0u);
    CHECK(guest_render_transaction_present(make_visual_id(), 77u, &present) ==
          GUEST_RENDER_TRANSACTION_READY);
    fixture.deferred_transaction_active = false;
    fixture.deferred_candidate_active = false;
    fixture.deferred_candidate_token = GPU_RENDER_DEFERRED_CANDIDATE_NONE;
    ++fixture.deferred_discard_count;
    CHECK(guest_render_transaction_post_swap_success() ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(snapshot(&state));
    CHECK(state.phase == GUEST_RENDER_TRANSACTION_IDLE);
    CHECK(state.published_transaction_count == 1u);
    CHECK(state.published_substitution_count == 1u);
    CHECK(fixture.checkpoint_commit_count == 1u);
    CHECK(fixture.material_observation_count == 1u);
    CHECK(fixture.material_observation_command_id == 22u);
    CHECK(fixture.replay_count == 1u);
    CHECK(!fixture.deferred_candidate_active);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.report_replay_material = true;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_GPUSTAT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);
    CHECK(guest_render_transaction_seal_deferred_retry(88u) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_begin_deferred(make_visual_id(), 89u) ==
          GUEST_RENDER_TRANSACTION_STALE_VRAM_SERIAL);
    CHECK(fixture.material_observation_count == 3u);
    CHECK(fixture.deferred_discard_count == 1u);
    CHECK(!fixture.deferred_candidate_active);

    reset_fixture();
    make_journal_fixture(&data);
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_GPUSTAT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);
    CHECK(guest_render_transaction_seal_deferred_retry(99u) ==
          GUEST_RENDER_TRANSACTION_OK);
    stale_id = make_visual_id();
    ++stale_id.state_sequence;
    CHECK(guest_render_transaction_begin_deferred(stale_id, 99u) ==
          GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID);
    CHECK(fixture.deferred_discard_count == 1u);

    reset_fixture();
    make_journal_fixture(&data);
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_GPUSTAT) ==
          GUEST_RENDER_TRANSACTION_ABORTED);
    CHECK(guest_render_transaction_seal_deferred_retry(111u) ==
          GUEST_RENDER_TRANSACTION_OK);
    guest_render_transaction_invalidate_deferred();
    CHECK(guest_render_transaction_deferred_snapshot(&deferred) ==
          GUEST_RENDER_TRANSACTION_OK && !deferred.sealed);
    CHECK(fixture.deferred_discard_count == 1u);
    CHECK(guest_render_transaction_begin_deferred(make_visual_id(), 111u) ==
          GUEST_RENDER_TRANSACTION_INVALID_TRANSITION);
    return 0;
}

static int test_invalid_arguments_and_replay_failure(void) {
    JournalFixture data;

    reset_fixture();
    CHECK(guest_render_transaction_execute(NULL) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(guest_render_transaction_snapshot(NULL) ==
          GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_NONE) ==
          GUEST_RENDER_TRANSACTION_INVALID_TRANSITION);

    reset_fixture();
    make_journal_fixture(&data);
    fixture.fail_replay = true;
    CHECK(guest_render_transaction_execute(&data.request) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(guest_render_transaction_abort_before_observation(
              GUEST_RENDER_TRANSACTION_OBSERVATION_GP1) ==
          GUEST_RENDER_TRANSACTION_REPLAY_FAILURE);
    CHECK(fixture.replay_count == 1u);
    return 0;
}

int main(void) {
    int result;

    result = test_happy_slot_replacement_and_publication();
    if (result != 0) return result;
    result = test_private_byte_identical_adjacent_payload_replay();
    if (result != 0) return result;
    result = test_preflight_links_targets_and_completeness();
    if (result != 0) return result;
    result = test_exact_binding_matching_contract();
    if (result != 0) return result;
    result = test_target_side_effects_and_adjacent_compatibility_order();
    if (result != 0) return result;
    result = test_target_side_effects_failure_rolls_back_and_replays();
    if (result != 0) return result;
    result = test_preflight_stale_state_serial_and_capacities();
    if (result != 0) return result;
    result = test_late_command_aborts_before_trigger();
    if (result != 0) return result;
    result = test_pending_staging_validation_and_capacity();
    if (result != 0) return result;
    result = test_pending_execute_lifecycle_and_no_word_channel();
    if (result != 0) return result;
    result = test_pending_retry_and_unsafe_clear_rules();
    if (result != 0) return result;
    result = test_abort_and_post_swap_clear_matching_pending();
    if (result != 0) return result;
    result = test_checkpoint_failures_are_atomic_and_unique();
    if (result != 0) return result;
    result = test_post_swap_failure_cleans_without_rollback_or_replay();
    if (result != 0) return result;
    result = test_every_required_observation_reason_is_unique();
    if (result != 0) return result;
    result = test_compatibility_and_backend_failures_replay_once();
    if (result != 0) return result;
    result = test_present_revalidates_state_and_serial();
    if (result != 0) return result;
    result = test_gpustat_abort_deferred_retry_lifecycle();
    if (result != 0) return result;
    result = test_invalid_arguments_and_replay_failure();
    if (result != 0) return result;
    return 0;
}
