#ifndef PSX_GUEST_RENDER_TRANSACTION_H
#define PSX_GUEST_RENDER_TRANSACTION_H

#include "gpu_render.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY
#define GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY 4096u
#endif

#ifndef GUEST_RENDER_TRANSACTION_WORD_CAPACITY
#define GUEST_RENDER_TRANSACTION_WORD_CAPACITY 32768u
#endif

#ifndef GUEST_RENDER_TRANSACTION_BINDING_CAPACITY
#define GUEST_RENDER_TRANSACTION_BINDING_CAPACITY 4096u
#endif

#ifndef GUEST_RENDER_TRANSACTION_PENDING_CAPACITY
#define GUEST_RENDER_TRANSACTION_PENDING_CAPACITY 4096u
#endif

#define GUEST_RENDER_TRANSACTION_NO_COMMAND UINT64_MAX

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GuestRenderTransactionStatus {
    GUEST_RENDER_TRANSACTION_OK = 0,
    GUEST_RENDER_TRANSACTION_READY,
    GUEST_RENDER_TRANSACTION_ABORTED,
    GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT,
    GUEST_RENDER_TRANSACTION_INVALID_TRANSITION,
    GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED,
    GUEST_RENDER_TRANSACTION_JOURNAL_INCOMPLETE,
    GUEST_RENDER_TRANSACTION_INVALID_ORDER,
    GUEST_RENDER_TRANSACTION_INVALID_LINK,
    GUEST_RENDER_TRANSACTION_TARGET_NOT_FOUND,
    GUEST_RENDER_TRANSACTION_DUPLICATE_TARGET,
    GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID,
    GUEST_RENDER_TRANSACTION_STALE_VRAM_SERIAL,
    GUEST_RENDER_TRANSACTION_BACKEND_FAILURE,
    GUEST_RENDER_TRANSACTION_COMPATIBILITY_FAILURE,
    GUEST_RENDER_TRANSACTION_TARGET_SIDE_EFFECTS_FAILURE,
    GUEST_RENDER_TRANSACTION_BACKEND_PRESENT_FAILURE,
    GUEST_RENDER_TRANSACTION_CHECKPOINT_BEGIN_FAILURE,
    GUEST_RENDER_TRANSACTION_CHECKPOINT_ROLLBACK_FAILURE,
    GUEST_RENDER_TRANSACTION_CHECKPOINT_COMMIT_FAILURE,
    GUEST_RENDER_TRANSACTION_REPLAY_FAILURE,
    GUEST_RENDER_TRANSACTION_COUNTER_EXHAUSTED,
    GUEST_RENDER_TRANSACTION_BACKEND_ROLLBACK_FAILURE,
} GuestRenderTransactionStatus;

typedef enum GuestRenderTransactionPhase {
    GUEST_RENDER_TRANSACTION_IDLE = 0,
    GUEST_RENDER_TRANSACTION_ACTIVE,
    GUEST_RENDER_TRANSACTION_AWAITING_SWAP,
    GUEST_RENDER_TRANSACTION_ROLLED_BACK,
} GuestRenderTransactionPhase;

typedef enum GuestRenderTransactionSource {
    GUEST_RENDER_TRANSACTION_SOURCE_OT = 0,
    GUEST_RENDER_TRANSACTION_SOURCE_DMA,
    GUEST_RENDER_TRANSACTION_SOURCE_MMIO,
} GuestRenderTransactionSource;

/* Reasons identify either an observation trigger or a terminal internal
 * failure. For observation triggers the coordinator never performs the
 * operation; the caller invokes it exactly once after abort returns. */
typedef enum GuestRenderTransactionObservationReason {
    GUEST_RENDER_TRANSACTION_OBSERVATION_NONE = 0,
    GUEST_RENDER_TRANSACTION_OBSERVATION_GP1,
    GUEST_RENDER_TRANSACTION_OBSERVATION_GPUSTAT,
    GUEST_RENDER_TRANSACTION_OBSERVATION_GPUREAD,
    GUEST_RENDER_TRANSACTION_OBSERVATION_DMA2_GPU_TO_RAM_C0,
    GUEST_RENDER_TRANSACTION_OBSERVATION_DELAYED_COMPLETION,
    GUEST_RENDER_TRANSACTION_OBSERVATION_IRQ,
    GUEST_RENDER_TRANSACTION_OBSERVATION_LATE_MMIO,
    GUEST_RENDER_TRANSACTION_OBSERVATION_SECOND_LIST,
    GUEST_RENDER_TRANSACTION_OBSERVATION_LATE_COMMAND,
    GUEST_RENDER_TRANSACTION_OBSERVATION_VISUAL_ID_CHANGED,
    GUEST_RENDER_TRANSACTION_OBSERVATION_VRAM_SERIAL_CHANGED,
    GUEST_RENDER_TRANSACTION_OBSERVATION_COMPATIBILITY_FAILURE,
    GUEST_RENDER_TRANSACTION_OBSERVATION_TARGET_SIDE_EFFECTS_FAILURE,
    GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE,
    GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_PRESENT_FAILURE,
    GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_BEGIN_FAILURE,
    GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_ROLLBACK_FAILURE,
    GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_COMMIT_FAILURE,
    GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT,
    GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_ROLLBACK_FAILURE,
} GuestRenderTransactionObservationReason;

/* command_id, container_id, and list_id are opaque caller metadata.
 * container_id identifies the source packet/container that held the command;
 * predecessor_command_id and successor_command_id describe the exact final
 * submission order. */
typedef struct GuestRenderTransactionCommandMetadata {
    GuestRenderTransactionSource source;
    uint64_t list_id;
    uint64_t command_id;
    uint64_t container_id;
    uint64_t predecessor_command_id;
    uint64_t successor_command_id;
    size_t ordinal;
    size_t word_offset;
    size_t word_count;
} GuestRenderTransactionCommandMetadata;

typedef struct GuestRenderTransactionJournal {
    GpuRenderTransactionId visual_id;
    uint64_t vram_mutation_serial;
    uint64_t list_id;
    const GuestRenderTransactionCommandMetadata *commands;
    size_t command_count;
    const uint32_t *words;
    size_t word_count;
    bool complete;
} GuestRenderTransactionJournal;

/* Selectors deliberately receive metadata only. Original command words are
 * not present in this callback's type or in its argument. */
typedef bool (*GuestRenderTransactionSemanticSelector)(
    const GuestRenderTransactionCommandMetadata *metadata);

typedef struct GuestRenderTransactionSemanticBinding {
    GuestRenderTransactionSemanticSelector selector;
    GpuRenderSemantic semantic;
    /* Exactly one of has_exact_command_id or selector must be configured. */
    bool has_exact_command_id;
    uint64_t exact_command_id;
} GuestRenderTransactionSemanticBinding;

/* Compatibility is called once for every unbound command, in final order.
 * The words point into the coordinator's private immutable Original copy and
 * are valid only for the duration of the callback. The callback must consume
 * them through the transaction-compatible GPU command path, not by reissuing
 * the source DMA, MMIO, completion, or cycle effects. A draw returns its
 * effective material through out_material/out_has_material so publication can
 * remain commit-ordered; non-draw commands set out_has_material false. */
typedef bool (*GuestRenderTransactionCompatibilityCallback)(
    const GuestRenderTransactionCommandMetadata *metadata,
    const uint32_t *original_words,
    size_t original_word_count,
    GpuRenderMaterial *out_material,
    bool *out_has_material,
    void *user_data);

/* Called for each bound command after its ordering barrier and before its
 * semantic draw. It must run canonical parser, state, and oracle effects with
 * Original raster output suppressed. It must not recreate source DMA, MMIO,
 * completion, or guest-cycle effects. The immutable words are valid only for
 * the duration of the callback. */
typedef bool (*GuestRenderTransactionTargetSideEffectsCallback)(
    const GuestRenderTransactionCommandMetadata *metadata,
    const uint32_t *original_words,
    size_t original_word_count,
    void *user_data);

/* Called in final order only after the semantic transaction is swapped and
 * its checkpoint commits. Rolled-back draws never publish observations. The
 * metadata and material pointers are valid only during the callback. */
typedef void (*GuestRenderTransactionMaterialObservationCallback)(
    const GuestRenderTransactionCommandMetadata *metadata,
    const GpuRenderMaterial *material,
    void *user_data);

/* The checkpoint covers generic parser, state, and oracle mutations outside
 * the GPU backend transaction. begin returning true establishes one live
 * checkpoint. rollback restores it and consumes it; commit makes mutations
 * durable and consumes it. A false begin must leave runtime state unchanged
 * and no checkpoint to restore. A false commit must leave it rollback-capable.
 * A false rollback is terminal: the coordinator reports failure and never
 * invokes rollback again. */
typedef bool (*GuestRenderTransactionCheckpointCallback)(
    GpuRenderTransactionId visual_id,
    uint64_t vram_mutation_serial,
    void *user_data);

typedef struct GuestRenderTransactionReplayJournal {
    GpuRenderTransactionId visual_id;
    uint64_t vram_mutation_serial;
    uint64_t list_id;
    const GuestRenderTransactionCommandMetadata *commands;
    size_t command_count;
    const uint32_t *words;
    size_t word_count;
} GuestRenderTransactionReplayJournal;

/* Replay is one whole-journal call. It must feed the copied words directly to
 * the canonical GPU command path in listed order. It must not reissue DMA,
 * MMIO, delayed-completion, IRQ, or guest-cycle effects represented by source
 * metadata. The view and its pointers are valid only during the callback. */
typedef bool (*GuestRenderTransactionReplayCallback)(
    const GuestRenderTransactionReplayJournal *journal,
    void *user_data);

typedef struct GuestRenderTransactionRequest {
    const GuestRenderTransactionJournal *journal;
    const GuestRenderTransactionSemanticBinding *bindings;
    size_t binding_count;
    GpuRenderTransactionId current_visual_id;
    uint64_t current_vram_mutation_serial;
    GuestRenderTransactionCompatibilityCallback compatibility_callback;
    void *compatibility_user_data;
    /* Both callbacks are required whenever binding_count is nonzero. */
    GuestRenderTransactionTargetSideEffectsCallback
        target_side_effects_callback;
    void *target_side_effects_user_data;
    GuestRenderTransactionMaterialObservationCallback
        material_observation_callback;
    void *material_observation_user_data;
    GuestRenderTransactionCheckpointCallback begin_checkpoint;
    GuestRenderTransactionCheckpointCallback rollback_checkpoint;
    GuestRenderTransactionCheckpointCallback commit_checkpoint;
    void *checkpoint_user_data;
    GuestRenderTransactionReplayCallback replay_callback;
    void *replay_user_data;
} GuestRenderTransactionRequest;

typedef struct GuestRenderTransactionPendingExecuteRequest {
    const GuestRenderTransactionJournal *journal;
    GpuRenderTransactionId current_visual_id;
    uint64_t current_vram_mutation_serial;
    GuestRenderTransactionCompatibilityCallback compatibility_callback;
    void *compatibility_user_data;
    /* Both callbacks are required: execute_pending owns at least one binding. */
    GuestRenderTransactionTargetSideEffectsCallback
        target_side_effects_callback;
    void *target_side_effects_user_data;
    GuestRenderTransactionMaterialObservationCallback
        material_observation_callback;
    void *material_observation_user_data;
    GuestRenderTransactionCheckpointCallback begin_checkpoint;
    GuestRenderTransactionCheckpointCallback rollback_checkpoint;
    GuestRenderTransactionCheckpointCallback commit_checkpoint;
    void *checkpoint_user_data;
    GuestRenderTransactionReplayCallback replay_callback;
    void *replay_user_data;
} GuestRenderTransactionPendingExecuteRequest;

typedef struct GuestRenderTransactionPendingSnapshot {
    GpuRenderTransactionId visual_id;
    size_t binding_count;
} GuestRenderTransactionPendingSnapshot;

typedef struct GuestRenderTransactionDeferredSnapshot {
    GpuRenderTransactionId visual_id;
    uint64_t post_replay_vram_mutation_serial;
    size_t binding_count;
    bool sealed;
} GuestRenderTransactionDeferredSnapshot;

typedef struct GuestRenderTransactionSnapshot {
    GuestRenderTransactionPhase phase;
    GuestRenderTransactionObservationReason abort_reason;
    GuestRenderTransactionStatus last_status;
    GpuRenderTransactionStatus backend_status;
    GpuRenderTransactionStatus rollback_status;
    GpuRenderTransactionId active_visual_id;
    uint64_t active_vram_mutation_serial;
    size_t active_command_count;
    size_t active_binding_count;
    uint64_t published_transaction_count;
    uint64_t published_substitution_count;
} GuestRenderTransactionSnapshot;

const void *guest_render_transaction_process_owner(void);

/* Copies, validates, begins, and executes one complete journal. OK means the
 * backend transaction remains open and is waiting for present-time commit. */
GuestRenderTransactionStatus guest_render_transaction_execute(
    const GuestRenderTransactionRequest *request);

/* Stages a copied semantic without accepting journal words. Staging failures
 * clear the complete pending set so a partial or mixed set cannot survive. */
GuestRenderTransactionStatus guest_render_transaction_stage_exact(
    GpuRenderTransactionId visual_id,
    uint64_t exact_command_id,
    const GpuRenderSemantic *semantic);
GuestRenderTransactionStatus guest_render_transaction_pending_snapshot(
    GuestRenderTransactionPendingSnapshot *out_snapshot);
void guest_render_transaction_clear_pending(void);

/* Uses the staged exact bindings. A successful preflight transfers ownership
 * to the immutable active transaction and clears pending before backend begin.
 * INVALID_ARGUMENT, CAPACITY_EXCEEDED, JOURNAL_INCOMPLETE, INVALID_ORDER, and
 * INVALID_LINK preserve pending only when the journal and current visual IDs
 * still equal the staged ID. Every other preflight result, plus begin or
 * execution failure, clears pending and requires restaging. */
GuestRenderTransactionStatus guest_render_transaction_execute_pending(
    const GuestRenderTransactionPendingExecuteRequest *request);

/* Call immediately before an observation trigger. On ABORTED the backend has
 * been rolled back and the complete Original journal has been replayed; this
 * function has not performed the trigger itself. */
GuestRenderTransactionStatus
guest_render_transaction_abort_before_observation(
    GuestRenderTransactionObservationReason reason);
/* Replay callbacks report each Original draw material while the coordinator is
 * aborting. The coordinator publishes this fallback stream only when the
 * semantic/deferred candidate does not become authoritative. */
bool guest_render_transaction_note_replay_material(
    const GuestRenderTransactionCommandMetadata *metadata,
    const GpuRenderMaterial *material);
bool guest_render_transaction_replay_material_capture_active(void);

/* A GPUSTAT abort may leave one backend-owned full-frame candidate prepared.
 * The GPU observation guard seals it only after rollback/replay, immediately
 * before allowing the original GPUSTAT read. No packet words or adapter IR are
 * exposed through this descriptor. */
GuestRenderTransactionStatus guest_render_transaction_seal_deferred_retry(
    uint64_t post_replay_vram_mutation_serial);
GuestRenderTransactionStatus guest_render_transaction_deferred_snapshot(
    GuestRenderTransactionDeferredSnapshot *out_snapshot);
void guest_render_transaction_invalidate_deferred(void);
extern int g_guest_render_transaction_deferred_active;

/* Opens a second status-bearing transaction around the sealed backend
 * candidate. It begins a fresh canonical checkpoint and consumes the candidate
 * token; normal present/swap APIs finish or roll it back. */
GuestRenderTransactionStatus guest_render_transaction_begin_deferred(
    GpuRenderTransactionId current_visual_id,
    uint64_t current_vram_mutation_serial);

/* Revalidates identity and serial at present time, then calls
 * gr_commit_validate. READY requires the caller to perform its swap/present. */
GuestRenderTransactionStatus guest_render_transaction_present(
    GpuRenderTransactionId current_visual_id,
    uint64_t current_vram_mutation_serial,
    const GpuRenderPresent *present);

/* The only operation that publishes transaction and substitution counters. */
GuestRenderTransactionStatus guest_render_transaction_post_swap_success(void);

/* Call only after the backend presentation path has restored or suppressed a
 * failed staged swap. This publishes nothing and performs no GPU rollback or
 * Original replay. Pass checkpoint_already_restored only when the caller has
 * contractually restored this exact runtime checkpoint; otherwise the
 * coordinator invokes rollback_checkpoint once. */
GuestRenderTransactionStatus guest_render_transaction_post_swap_failure(
    bool checkpoint_already_restored);

GuestRenderTransactionStatus guest_render_transaction_snapshot(
    GuestRenderTransactionSnapshot *out_snapshot);
size_t guest_render_transaction_command_capacity(void);
size_t guest_render_transaction_word_capacity(void);
size_t guest_render_transaction_binding_capacity(void);
size_t guest_render_transaction_pending_capacity(void);
const char *guest_render_transaction_observation_reason_name(uint32_t reason);

#ifdef GUEST_RENDER_TRANSACTION_TESTING
void guest_render_transaction_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
