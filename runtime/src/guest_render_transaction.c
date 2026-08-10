#include "guest_render_transaction.h"

#include <limits.h>
#include <string.h>

#if GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY == 0
#error "GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY must be nonzero"
#endif

#if GUEST_RENDER_TRANSACTION_WORD_CAPACITY == 0
#error "GUEST_RENDER_TRANSACTION_WORD_CAPACITY must be nonzero"
#endif

#if GUEST_RENDER_TRANSACTION_BINDING_CAPACITY == 0
#error "GUEST_RENDER_TRANSACTION_BINDING_CAPACITY must be nonzero"
#endif

#if GUEST_RENDER_TRANSACTION_PENDING_CAPACITY == 0
#error "GUEST_RENDER_TRANSACTION_PENDING_CAPACITY must be nonzero"
#endif

#if GUEST_RENDER_TRANSACTION_PENDING_CAPACITY > GUEST_RENDER_TRANSACTION_BINDING_CAPACITY
#error "pending capacity cannot exceed active binding capacity"
#endif

typedef enum InternalPhase {
    INTERNAL_IDLE = 0,
    INTERNAL_EXECUTING,
    INTERNAL_ACTIVE,
    INTERNAL_AWAITING_SWAP,
    INTERNAL_ABORTING,
    INTERNAL_ROLLED_BACK,
} InternalPhase;

typedef enum DeferredPhase {
    DEFERRED_NONE = 0,
    DEFERRED_PREPARED,
    DEFERRED_SEALED,
} DeferredPhase;

typedef struct GuestRenderPendingMaterialObservation {
    GuestRenderTransactionCommandMetadata command;
    GpuRenderMaterial material;
} GuestRenderPendingMaterialObservation;

typedef struct GuestRenderDeferredRetry {
    GpuRenderDeferredCandidateToken candidate_token;
    GpuRenderTransactionId visual_id;
    uint64_t post_replay_vram_mutation_serial;
    size_t binding_count;
    GuestRenderTransactionCheckpointCallback begin_checkpoint;
    GuestRenderTransactionCheckpointCallback rollback_checkpoint;
    GuestRenderTransactionCheckpointCallback commit_checkpoint;
    void *checkpoint_user_data;
    GuestRenderTransactionMaterialObservationCallback
        material_observation_callback;
    void *material_observation_user_data;
    GuestRenderPendingMaterialObservation
        material_observations[GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY];
    size_t material_observation_count;
    GuestRenderPendingMaterialObservation
        fallback_material_observations[
            GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY];
    size_t fallback_material_observation_count;
    DeferredPhase phase;
} GuestRenderDeferredRetry;

typedef struct GuestRenderTransactionCoordinator {
    GuestRenderTransactionCommandMetadata
        commands[GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY];
    uint32_t words[GUEST_RENDER_TRANSACTION_WORD_CAPACITY];
    GuestRenderTransactionSemanticBinding
        bindings[GUEST_RENDER_TRANSACTION_BINDING_CAPACITY];
    size_t command_binding[GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY];
    GpuRenderTransactionId visual_id;
    uint64_t vram_mutation_serial;
    uint64_t list_id;
    size_t command_count;
    size_t word_count;
    size_t binding_count;
    GuestRenderTransactionCompatibilityCallback compatibility_callback;
    void *compatibility_user_data;
    GuestRenderTransactionTargetSideEffectsCallback
        target_side_effects_callback;
    void *target_side_effects_user_data;
    GuestRenderTransactionMaterialObservationCallback
        material_observation_callback;
    void *material_observation_user_data;
    GuestRenderPendingMaterialObservation
        material_observations[GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY];
    size_t material_observation_count;
    GuestRenderPendingMaterialObservation
        fallback_material_observations[
            GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY];
    size_t fallback_material_observation_count;
    GuestRenderTransactionCheckpointCallback begin_checkpoint;
    GuestRenderTransactionCheckpointCallback rollback_checkpoint;
    GuestRenderTransactionCheckpointCallback commit_checkpoint;
    void *checkpoint_user_data;
    GuestRenderTransactionReplayCallback replay_callback;
    void *replay_user_data;
    InternalPhase phase;
    GuestRenderTransactionObservationReason abort_reason;
    GuestRenderTransactionStatus last_status;
    GpuRenderTransactionStatus backend_status;
    GpuRenderTransactionStatus rollback_status;
    uint64_t published_transaction_count;
    uint64_t published_substitution_count;
    GuestRenderTransactionSemanticBinding
        pending_bindings[GUEST_RENDER_TRANSACTION_PENDING_CAPACITY];
    GpuRenderTransactionId pending_visual_id;
    size_t pending_binding_count;
    bool replay_attempted;
    bool backend_open;
    bool checkpoint_open;
    bool active_deferred;
    GuestRenderDeferredRetry deferred_retry;
} GuestRenderTransactionCoordinator;

static GuestRenderTransactionCoordinator coordinator;
int g_guest_render_transaction_deferred_active;

static bool visual_ids_equal(GpuRenderTransactionId left,
                             GpuRenderTransactionId right) {
    return left.scene_epoch == right.scene_epoch &&
           left.state_sequence == right.state_sequence;
}

static bool source_is_valid(GuestRenderTransactionSource source) {
    return source == GUEST_RENDER_TRANSACTION_SOURCE_OT ||
           source == GUEST_RENDER_TRANSACTION_SOURCE_DMA ||
           source == GUEST_RENDER_TRANSACTION_SOURCE_MMIO;
}

static void publish_material_observations(
        GuestRenderTransactionMaterialObservationCallback callback,
        void *user_data,
        const GuestRenderPendingMaterialObservation *observations,
        size_t observation_count) {
    size_t index;

    if (!callback || !observations) return;
    for (index = 0u; index < observation_count; ++index)
        callback(&observations[index].command, &observations[index].material,
                 user_data);
}

static bool semantic_is_valid(const GpuRenderSemantic *semantic) {
    const GpuRenderMaterial *material = &semantic->material;
    uint16_t encoded_depth;
    size_t triangle_index;

    if (semantic->screen_space_2d > GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE ||
        (material->texture_depth != GPU_RENDER_TEXTURE_4_BIT &&
        material->texture_depth != GPU_RENDER_TEXTURE_8_BIT &&
        material->texture_depth != GPU_RENDER_TEXTURE_15_BIT))
        return false;
    if (material->blend_mode != GPU_RENDER_BLEND_AVERAGE &&
        material->blend_mode != GPU_RENDER_BLEND_ADD &&
        material->blend_mode != GPU_RENDER_BLEND_SUBTRACT &&
        material->blend_mode != GPU_RENDER_BLEND_ADD_QUARTER)
        return false;
    if (material->shading != GPU_RENDER_SHADING_FLAT &&
        material->shading != GPU_RENDER_SHADING_GOURAUD)
        return false;
    encoded_depth = (uint16_t)((material->tpage >> 7u) & 3u);
    if (material->tpage > UINT16_C(0x01ff) || encoded_depth == 3u ||
        material->texture_page_x != (material->tpage & UINT16_C(0x000f)) ||
        material->texture_page_y != ((material->tpage >> 4u) & 1u) ||
        material->blend_mode !=
            (GpuRenderBlendMode)((material->tpage >> 5u) & 3u) ||
        material->texture_depth != (GpuRenderTextureDepth)encoded_depth ||
        material->clut_x > 1023u || (material->clut_x & 15u) != 0u ||
        material->clut_y > 511u ||
        material->draw_area_left > material->draw_area_right ||
        material->draw_area_top > material->draw_area_bottom ||
        material->draw_area_right > 1023u ||
        material->draw_area_bottom > 1023u ||
        material->draw_offset_x < -1024 || material->draw_offset_x > 1023 ||
        material->draw_offset_y < -1024 || material->draw_offset_y > 1023 ||
        material->texture_window_mask_x > 31u ||
        material->texture_window_mask_y > 31u ||
        material->texture_window_offset_x > 31u ||
        material->texture_window_offset_y > 31u ||
        material->textured > 1u || material->raw_texture > 1u ||
        material->semi_transparent > 1u || material->dither > 1u ||
        material->mask_set > 1u || material->mask_check > 1u ||
        (material->raw_texture != 0u && material->textured == 0u))
        return false;
    if (semantic->topology == GPU_RENDER_SEMANTIC_LINES) {
        if (semantic->screen_space_2d || material->textured ||
            material->raw_texture ||
            semantic->triangle_count != 0u || semantic->line_count == 0u ||
            semantic->line_count > GPU_RENDER_SEMANTIC_LINE_CAPACITY)
            return false;
        for (size_t line_index = 0u;
             line_index < semantic->line_count; ++line_index) {
            for (size_t vertex_index = 0u; vertex_index < 2u; ++vertex_index) {
                const GpuRenderSemanticVertex *vertex =
                    &semantic->lines[line_index].vertices[vertex_index];

                if (((uint32_t)vertex->x & UINT32_C(0xffff)) != 0u ||
                    ((uint32_t)vertex->y & UINT32_C(0xffff)) != 0u ||
                    ((uint32_t)vertex->u & UINT32_C(0xffff)) != 0u ||
                    ((uint32_t)vertex->v & UINT32_C(0xffff)) != 0u)
                    return false;
                if (vertex->native_view_position > 1u ||
                    (!vertex->native_view_position &&
                     (vertex->native_view_x != 0 || vertex->native_view_y != 0)))
                    return false;
                if (material->shading == GPU_RENDER_SHADING_FLAT &&
                    vertex_index != 0u &&
                    (vertex->r != semantic->lines[line_index].vertices[0].r ||
                     vertex->g != semantic->lines[line_index].vertices[0].g ||
                     vertex->b != semantic->lines[line_index].vertices[0].b))
                    return false;
            }
        }
        return true;
    }
    if (semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->line_count != 0u || semantic->triangle_count == 0u ||
        semantic->triangle_count > GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY)
        return false;
    for (triangle_index = 0u;
         triangle_index < semantic->triangle_count;
         ++triangle_index) {
        if (semantic->triangles[triangle_index].split_index != triangle_index ||
            semantic->triangles[triangle_index].split_count !=
                semantic->triangle_count)
            return false;
        for (size_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const GpuRenderSemanticVertex *vertex =
                &semantic->triangles[triangle_index].vertices[vertex_index];

            if (vertex->native_view_position > 1u ||
                (semantic->screen_space_2d && vertex->native_view_position) ||
                (!vertex->native_view_position &&
                 (vertex->native_view_x != 0 || vertex->native_view_y != 0)))
                return false;
        }
        if (material->shading == GPU_RENDER_SHADING_FLAT) {
            size_t vertex_index;

            for (vertex_index = 1u; vertex_index < 3u; ++vertex_index) {
                if (semantic->triangles[triangle_index].vertices[vertex_index].r !=
                        semantic->triangles[triangle_index].vertices[0].r ||
                    semantic->triangles[triangle_index].vertices[vertex_index].g !=
                        semantic->triangles[triangle_index].vertices[0].g ||
                    semantic->triangles[triangle_index].vertices[vertex_index].b !=
                        semantic->triangles[triangle_index].vertices[0].b)
                    return false;
            }
        }
    }
    return true;
}

static bool phase_has_active_transaction(void) {
    return coordinator.phase == INTERNAL_EXECUTING ||
           coordinator.phase == INTERNAL_ACTIVE ||
           coordinator.phase == INTERNAL_AWAITING_SWAP ||
           coordinator.phase == INTERNAL_ABORTING;
}

static void clear_pending(void) {
    memset(coordinator.pending_bindings, 0,
           sizeof(coordinator.pending_bindings));
    memset(&coordinator.pending_visual_id, 0,
           sizeof(coordinator.pending_visual_id));
    coordinator.pending_binding_count = 0u;
}

static void clear_pending_for_visual(GpuRenderTransactionId visual_id) {
    if (coordinator.pending_binding_count != 0u &&
        visual_ids_equal(coordinator.pending_visual_id, visual_id))
        clear_pending();
}

static void clear_deferred_retry(void) {
    publish_material_observations(
        coordinator.deferred_retry.material_observation_callback,
        coordinator.deferred_retry.material_observation_user_data,
        coordinator.deferred_retry.fallback_material_observations,
        coordinator.deferred_retry.fallback_material_observation_count);
    if (coordinator.deferred_retry.candidate_token !=
        GPU_RENDER_DEFERRED_CANDIDATE_NONE)
        (void)gr_deferred_candidate_discard(
            coordinator.deferred_retry.candidate_token);
    memset(&coordinator.deferred_retry, 0,
           sizeof(coordinator.deferred_retry));
    g_guest_render_transaction_deferred_active = 0;
}

static void clear_private_transaction(void) {
    memset(coordinator.commands, 0, sizeof(coordinator.commands));
    memset(coordinator.words, 0, sizeof(coordinator.words));
    memset(coordinator.bindings, 0, sizeof(coordinator.bindings));
    memset(coordinator.command_binding, 0,
           sizeof(coordinator.command_binding));
    memset(&coordinator.visual_id, 0, sizeof(coordinator.visual_id));
    coordinator.vram_mutation_serial = 0u;
    coordinator.list_id = 0u;
    coordinator.command_count = 0u;
    coordinator.word_count = 0u;
    coordinator.binding_count = 0u;
    coordinator.compatibility_callback = NULL;
    coordinator.compatibility_user_data = NULL;
    coordinator.target_side_effects_callback = NULL;
    coordinator.target_side_effects_user_data = NULL;
    coordinator.material_observation_callback = NULL;
    coordinator.material_observation_user_data = NULL;
    memset(coordinator.material_observations, 0,
           sizeof(coordinator.material_observations));
    coordinator.material_observation_count = 0u;
    memset(coordinator.fallback_material_observations, 0,
           sizeof(coordinator.fallback_material_observations));
    coordinator.fallback_material_observation_count = 0u;
    coordinator.begin_checkpoint = NULL;
    coordinator.rollback_checkpoint = NULL;
    coordinator.commit_checkpoint = NULL;
    coordinator.checkpoint_user_data = NULL;
    coordinator.replay_callback = NULL;
    coordinator.replay_user_data = NULL;
    coordinator.replay_attempted = false;
    coordinator.backend_open = false;
    coordinator.checkpoint_open = false;
    coordinator.active_deferred = false;
}

static GuestRenderTransactionStatus fail_preflight(
        GuestRenderTransactionStatus status) {
    clear_private_transaction();
    coordinator.phase = INTERNAL_IDLE;
    coordinator.abort_reason = GUEST_RENDER_TRANSACTION_OBSERVATION_NONE;
    coordinator.last_status = status;
    coordinator.backend_status = GPU_RENDER_TRANSACTION_OK;
    coordinator.rollback_status = GPU_RENDER_TRANSACTION_OK;
    return status;
}

static GuestRenderTransactionStatus validate_journal(void) {
    size_t index;
    size_t next_word_offset = 0u;

    for (index = 0u; index < coordinator.command_count; ++index) {
        const GuestRenderTransactionCommandMetadata *command =
            &coordinator.commands[index];
        const uint64_t expected_predecessor =
            index == 0u ? GUEST_RENDER_TRANSACTION_NO_COMMAND :
                          coordinator.commands[index - 1u].command_id;
        const uint64_t expected_successor =
            index + 1u == coordinator.command_count ?
                GUEST_RENDER_TRANSACTION_NO_COMMAND :
                coordinator.commands[index + 1u].command_id;
        size_t other_index;

        if (!source_is_valid(command->source) ||
            command->list_id != coordinator.list_id ||
            command->command_id == GUEST_RENDER_TRANSACTION_NO_COMMAND)
            return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
        if (command->ordinal != index)
            return GUEST_RENDER_TRANSACTION_INVALID_ORDER;
        if (command->predecessor_command_id != expected_predecessor ||
            command->successor_command_id != expected_successor)
            return GUEST_RENDER_TRANSACTION_INVALID_LINK;
        if (command->word_count == 0u ||
            command->word_offset != next_word_offset ||
            command->word_count > coordinator.word_count - next_word_offset)
            return GUEST_RENDER_TRANSACTION_JOURNAL_INCOMPLETE;
        next_word_offset += command->word_count;
        for (other_index = 0u; other_index < index; ++other_index) {
            if (coordinator.commands[other_index].command_id ==
                command->command_id)
                return GUEST_RENDER_TRANSACTION_INVALID_LINK;
        }
    }
    if (next_word_offset != coordinator.word_count)
        return GUEST_RENDER_TRANSACTION_JOURNAL_INCOMPLETE;
    return GUEST_RENDER_TRANSACTION_OK;
}

static GuestRenderTransactionStatus resolve_bindings(bool allow_subset) {
    bool compatibility_needed = false;
    size_t matched_binding_count = 0u;
    size_t binding_index;
    size_t command_index;

    for (command_index = 0u;
         command_index < coordinator.command_count;
         ++command_index)
        coordinator.command_binding[command_index] = SIZE_MAX;

    for (binding_index = 0u;
         binding_index < coordinator.binding_count;
         ++binding_index) {
        const GuestRenderTransactionSemanticBinding *binding =
            &coordinator.bindings[binding_index];
        size_t target_index = SIZE_MAX;
        size_t match_count = 0u;

        const bool has_selector = binding->selector != NULL;

        if (binding->has_exact_command_id == has_selector ||
            (binding->has_exact_command_id &&
             binding->exact_command_id == GUEST_RENDER_TRANSACTION_NO_COMMAND) ||
            !semantic_is_valid(&binding->semantic))
            return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
        for (command_index = 0u;
             command_index < coordinator.command_count;
             ++command_index) {
            const GuestRenderTransactionCommandMetadata metadata =
                coordinator.commands[command_index];

            if ((binding->has_exact_command_id &&
                 metadata.command_id == binding->exact_command_id) ||
                (has_selector && binding->selector(&metadata))) {
                target_index = command_index;
                ++match_count;
            }
        }
        if (match_count == 0u && allow_subset)
            continue;
        if (match_count == 0u)
            return GUEST_RENDER_TRANSACTION_TARGET_NOT_FOUND;
        if (match_count != 1u)
            return GUEST_RENDER_TRANSACTION_DUPLICATE_TARGET;
        if (coordinator.command_binding[target_index] != SIZE_MAX)
            return GUEST_RENDER_TRANSACTION_DUPLICATE_TARGET;
        if (matched_binding_count != binding_index)
            coordinator.bindings[matched_binding_count] = *binding;
        coordinator.command_binding[target_index] = matched_binding_count;
        ++matched_binding_count;
    }
    if (allow_subset && matched_binding_count == 0u)
        return GUEST_RENDER_TRANSACTION_TARGET_NOT_FOUND;
    coordinator.binding_count = matched_binding_count;

    for (command_index = 0u;
         command_index < coordinator.command_count;
         ++command_index) {
        if (coordinator.command_binding[command_index] == SIZE_MAX) {
            compatibility_needed = true;
            break;
        }
    }
    if (compatibility_needed && !coordinator.compatibility_callback)
        return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    return GUEST_RENDER_TRANSACTION_OK;
}

static bool replay_original_once(void) {
    GuestRenderTransactionReplayJournal replay;

    if (coordinator.replay_attempted) return false;
    coordinator.replay_attempted = true;
    if (coordinator.active_deferred) return true;
    replay.visual_id = coordinator.visual_id;
    replay.vram_mutation_serial = coordinator.vram_mutation_serial;
    replay.list_id = coordinator.list_id;
    replay.commands = coordinator.commands;
    replay.command_count = coordinator.command_count;
    replay.words = coordinator.words;
    replay.word_count = coordinator.word_count;
    return coordinator.replay_callback(&replay, coordinator.replay_user_data);
}

bool guest_render_transaction_note_replay_material(
        const GuestRenderTransactionCommandMetadata *metadata,
        const GpuRenderMaterial *material) {
    GuestRenderPendingMaterialObservation *observation;

    if (coordinator.phase != INTERNAL_ABORTING ||
        !coordinator.replay_attempted || coordinator.active_deferred ||
        !metadata || !material ||
        coordinator.fallback_material_observation_count >=
            GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY)
        return false;
    observation = &coordinator.fallback_material_observations[
        coordinator.fallback_material_observation_count++];
    observation->command = *metadata;
    observation->material = *material;
    return true;
}

bool guest_render_transaction_replay_material_capture_active(void) {
    return coordinator.phase == INTERNAL_ABORTING &&
           coordinator.replay_attempted && !coordinator.active_deferred;
}

static bool rollback_checkpoint_once(void) {
    bool succeeded;

    if (!coordinator.checkpoint_open) return true;
    succeeded = coordinator.rollback_checkpoint(
        coordinator.visual_id, coordinator.vram_mutation_serial,
        coordinator.checkpoint_user_data);
    coordinator.checkpoint_open = false;
    return succeeded;
}

static GuestRenderTransactionStatus abort_and_replay(
        GuestRenderTransactionObservationReason reason,
        GuestRenderTransactionStatus status) {
    bool replay_succeeded;
    bool checkpoint_rollback_succeeded;
    bool candidate_captured = false;
    const bool prepare_deferred =
        reason == GUEST_RENDER_TRANSACTION_OBSERVATION_GPUSTAT &&
        !coordinator.active_deferred && coordinator.backend_open;
    const GpuRenderTransactionId aborted_visual_id = coordinator.visual_id;
    const size_t aborted_binding_count = coordinator.binding_count;
    const GuestRenderTransactionCheckpointCallback deferred_begin_checkpoint =
        coordinator.begin_checkpoint;
    const GuestRenderTransactionCheckpointCallback deferred_rollback_checkpoint =
        coordinator.rollback_checkpoint;
    const GuestRenderTransactionCheckpointCallback deferred_commit_checkpoint =
        coordinator.commit_checkpoint;
    const GuestRenderTransactionMaterialObservationCallback
        deferred_material_observation_callback =
            coordinator.material_observation_callback;
    void *const deferred_material_observation_user_data =
        coordinator.material_observation_user_data;
    GuestRenderPendingMaterialObservation deferred_material_observations[
        GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY];
    GuestRenderPendingMaterialObservation fallback_material_observations[
        GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY];
    const size_t deferred_material_observation_count = prepare_deferred
        ? coordinator.material_observation_count : 0u;
    size_t fallback_material_observation_count;
    void *const deferred_checkpoint_user_data =
        coordinator.checkpoint_user_data;
    GpuRenderDeferredCandidateToken candidate_token =
        GPU_RENDER_DEFERRED_CANDIDATE_NONE;

    if (deferred_material_observation_count != 0u)
        memcpy(deferred_material_observations,
               coordinator.material_observations,
               deferred_material_observation_count *
                   sizeof(deferred_material_observations[0]));

    if (prepare_deferred &&
        gr_deferred_candidate_capture(coordinator.visual_id,
                                      &candidate_token) ==
            GPU_RENDER_TRANSACTION_OK &&
        candidate_token != GPU_RENDER_DEFERRED_CANDIDATE_NONE)
        candidate_captured = true;

    coordinator.phase = INTERNAL_ABORTING;
    coordinator.abort_reason = reason;
    if (coordinator.backend_open) {
        coordinator.rollback_status = gr_rollback(coordinator.visual_id);
        coordinator.backend_open = false;
    } else {
        coordinator.rollback_status = GPU_RENDER_TRANSACTION_OK;
    }
    if (coordinator.rollback_status != GPU_RENDER_TRANSACTION_OK) {
        coordinator.checkpoint_open = false;
        clear_pending_for_visual(aborted_visual_id);
        if (candidate_captured)
            (void)gr_deferred_candidate_discard(candidate_token);
        clear_private_transaction();
        coordinator.phase = INTERNAL_ROLLED_BACK;
        coordinator.abort_reason =
            GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_ROLLBACK_FAILURE;
        coordinator.last_status =
            GUEST_RENDER_TRANSACTION_BACKEND_ROLLBACK_FAILURE;
        return coordinator.last_status;
    }
    checkpoint_rollback_succeeded = rollback_checkpoint_once();
    replay_succeeded = replay_original_once();
    fallback_material_observation_count =
        coordinator.fallback_material_observation_count;
    if (fallback_material_observation_count != 0u)
        memcpy(fallback_material_observations,
               coordinator.fallback_material_observations,
               fallback_material_observation_count *
                   sizeof(fallback_material_observations[0]));
    clear_pending_for_visual(aborted_visual_id);
    clear_private_transaction();
    coordinator.phase = INTERNAL_ROLLED_BACK;
    if (candidate_captured &&
        coordinator.rollback_status == GPU_RENDER_TRANSACTION_OK &&
        checkpoint_rollback_succeeded && replay_succeeded) {
        clear_deferred_retry();
        coordinator.deferred_retry.candidate_token = candidate_token;
        coordinator.deferred_retry.visual_id = aborted_visual_id;
        coordinator.deferred_retry.binding_count = aborted_binding_count;
        coordinator.deferred_retry.begin_checkpoint =
            deferred_begin_checkpoint;
        coordinator.deferred_retry.rollback_checkpoint =
            deferred_rollback_checkpoint;
        coordinator.deferred_retry.commit_checkpoint =
            deferred_commit_checkpoint;
        coordinator.deferred_retry.checkpoint_user_data =
            deferred_checkpoint_user_data;
        coordinator.deferred_retry.material_observation_callback =
            deferred_material_observation_callback;
        coordinator.deferred_retry.material_observation_user_data =
            deferred_material_observation_user_data;
        coordinator.deferred_retry.material_observation_count =
            deferred_material_observation_count;
        if (deferred_material_observation_count != 0u)
            memcpy(coordinator.deferred_retry.material_observations,
                   deferred_material_observations,
                   deferred_material_observation_count *
                       sizeof(deferred_material_observations[0]));
        coordinator.deferred_retry.fallback_material_observation_count =
            fallback_material_observation_count;
        if (fallback_material_observation_count != 0u)
            memcpy(coordinator.deferred_retry.fallback_material_observations,
                   fallback_material_observations,
                   fallback_material_observation_count *
                       sizeof(fallback_material_observations[0]));
        coordinator.deferred_retry.phase = DEFERRED_PREPARED;
        g_guest_render_transaction_deferred_active = 1;
    } else if (candidate_captured) {
        (void)gr_deferred_candidate_discard(candidate_token);
    }
    if (!candidate_captured &&
        coordinator.rollback_status == GPU_RENDER_TRANSACTION_OK &&
        checkpoint_rollback_succeeded && replay_succeeded) {
        publish_material_observations(
            deferred_material_observation_callback,
            deferred_material_observation_user_data,
            fallback_material_observations,
            fallback_material_observation_count);
    }
    if (!checkpoint_rollback_succeeded) {
        coordinator.abort_reason =
            GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_ROLLBACK_FAILURE;
        status = GUEST_RENDER_TRANSACTION_CHECKPOINT_ROLLBACK_FAILURE;
    } else if (!replay_succeeded) {
        status = GUEST_RENDER_TRANSACTION_REPLAY_FAILURE;
    }
    coordinator.last_status = status;
    return status;
}

static GuestRenderTransactionStatus execute_preflighted(void) {
    size_t command_index;

    if (!coordinator.begin_checkpoint(
            coordinator.visual_id, coordinator.vram_mutation_serial,
            coordinator.checkpoint_user_data)) {
        return abort_and_replay(
            GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_BEGIN_FAILURE,
            GUEST_RENDER_TRANSACTION_CHECKPOINT_BEGIN_FAILURE);
    }
    coordinator.checkpoint_open = true;
    coordinator.backend_status = gr_transaction_begin(
        coordinator.visual_id, coordinator.vram_mutation_serial);
    if (coordinator.backend_status != GPU_RENDER_TRANSACTION_OK) {
        return abort_and_replay(
            GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE,
            GUEST_RENDER_TRANSACTION_BACKEND_FAILURE);
    }
    coordinator.backend_open = true;

    for (command_index = 0u;
         command_index < coordinator.command_count;
         ++command_index) {
        const GuestRenderTransactionCommandMetadata *command =
            &coordinator.commands[command_index];
        const size_t binding_index =
            coordinator.command_binding[command_index];

        coordinator.backend_status =
            gr_ordering_barrier(coordinator.visual_id);
        if (coordinator.backend_status != GPU_RENDER_TRANSACTION_OK) {
            return abort_and_replay(
                GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE,
                GUEST_RENDER_TRANSACTION_BACKEND_FAILURE);
        }
        if (binding_index != SIZE_MAX) {
            if (!coordinator.target_side_effects_callback(
                    command,
                    &coordinator.words[command->word_offset],
                    command->word_count,
                    coordinator.target_side_effects_user_data)) {
                return abort_and_replay(
                    GUEST_RENDER_TRANSACTION_OBSERVATION_TARGET_SIDE_EFFECTS_FAILURE,
                    GUEST_RENDER_TRANSACTION_TARGET_SIDE_EFFECTS_FAILURE);
            }
            coordinator.backend_status = gr_draw_semantic(
                coordinator.visual_id,
                &coordinator.bindings[binding_index].semantic);
            if (coordinator.backend_status != GPU_RENDER_TRANSACTION_OK) {
                return abort_and_replay(
                    GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE,
                    GUEST_RENDER_TRANSACTION_BACKEND_FAILURE);
            }
            if (coordinator.material_observation_count >=
                GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY) {
                return abort_and_replay(
                    GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE,
                    GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED);
            }
            coordinator.material_observations[
                coordinator.material_observation_count].command = *command;
            coordinator.material_observations[
                coordinator.material_observation_count].material =
                    coordinator.bindings[binding_index].semantic.material;
            ++coordinator.material_observation_count;
        } else {
            GpuRenderMaterial material = {0};
            bool has_material = false;

            if (!coordinator.compatibility_callback(
                    command,
                    &coordinator.words[command->word_offset],
                    command->word_count, &material, &has_material,
                    coordinator.compatibility_user_data)) {
                return abort_and_replay(
                    GUEST_RENDER_TRANSACTION_OBSERVATION_COMPATIBILITY_FAILURE,
                    GUEST_RENDER_TRANSACTION_COMPATIBILITY_FAILURE);
            }
            if (has_material) {
                if (coordinator.material_observation_count >=
                    GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY) {
                    return abort_and_replay(
                        GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE,
                        GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED);
                }
                coordinator.material_observations[
                    coordinator.material_observation_count].command = *command;
                coordinator.material_observations[
                    coordinator.material_observation_count].material = material;
                ++coordinator.material_observation_count;
            }
        }
    }
    coordinator.backend_status = gr_ordering_barrier(coordinator.visual_id);
    if (coordinator.backend_status != GPU_RENDER_TRANSACTION_OK) {
        return abort_and_replay(
            GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE,
            GUEST_RENDER_TRANSACTION_BACKEND_FAILURE);
    }
    coordinator.phase = INTERNAL_ACTIVE;
    coordinator.last_status = GUEST_RENDER_TRANSACTION_OK;
    return GUEST_RENDER_TRANSACTION_OK;
}

static bool pending_retry_is_safe(
        GuestRenderTransactionStatus status,
        const GuestRenderTransactionPendingExecuteRequest *request) {
    if (!request || !request->journal ||
        coordinator.pending_binding_count == 0u ||
        !visual_ids_equal(request->journal->visual_id,
                          coordinator.pending_visual_id) ||
        !visual_ids_equal(request->current_visual_id,
                          coordinator.pending_visual_id))
        return false;
    switch (status) {
    case GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT:
    case GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED:
    case GUEST_RENDER_TRANSACTION_JOURNAL_INCOMPLETE:
    case GUEST_RENDER_TRANSACTION_INVALID_ORDER:
    case GUEST_RENDER_TRANSACTION_INVALID_LINK:
    case GUEST_RENDER_TRANSACTION_TARGET_NOT_FOUND:
        return true;
    default:
        return false;
    }
}

static GuestRenderTransactionStatus copy_and_preflight(
        const GuestRenderTransactionRequest *request,
        bool allow_binding_subset) {
    const GuestRenderTransactionJournal *journal;
    GuestRenderTransactionStatus status;

    if (!request || !request->journal)
        return fail_preflight(GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    journal = request->journal;
    if (!journal->commands || !journal->words || !request->bindings ||
        !request->target_side_effects_callback ||
        !request->material_observation_callback || !request->begin_checkpoint ||
        !request->rollback_checkpoint || !request->commit_checkpoint ||
        !request->replay_callback || journal->command_count == 0u ||
        journal->word_count == 0u || request->binding_count == 0u)
        return fail_preflight(GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT);
    if (journal->command_count > GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY ||
        journal->word_count > GUEST_RENDER_TRANSACTION_WORD_CAPACITY ||
        request->binding_count > GUEST_RENDER_TRANSACTION_BINDING_CAPACITY)
        return fail_preflight(GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED);
    if (!journal->complete)
        return fail_preflight(GUEST_RENDER_TRANSACTION_JOURNAL_INCOMPLETE);

    clear_private_transaction();
    coordinator.visual_id = journal->visual_id;
    coordinator.vram_mutation_serial = journal->vram_mutation_serial;
    coordinator.list_id = journal->list_id;
    coordinator.command_count = journal->command_count;
    coordinator.word_count = journal->word_count;
    coordinator.binding_count = request->binding_count;
    coordinator.compatibility_callback = request->compatibility_callback;
    coordinator.compatibility_user_data = request->compatibility_user_data;
    coordinator.target_side_effects_callback =
        request->target_side_effects_callback;
    coordinator.target_side_effects_user_data =
        request->target_side_effects_user_data;
    coordinator.material_observation_callback =
        request->material_observation_callback;
    coordinator.material_observation_user_data =
        request->material_observation_user_data;
    coordinator.begin_checkpoint = request->begin_checkpoint;
    coordinator.rollback_checkpoint = request->rollback_checkpoint;
    coordinator.commit_checkpoint = request->commit_checkpoint;
    coordinator.checkpoint_user_data = request->checkpoint_user_data;
    coordinator.replay_callback = request->replay_callback;
    coordinator.replay_user_data = request->replay_user_data;
    memcpy(coordinator.commands, journal->commands,
           journal->command_count * sizeof(coordinator.commands[0]));
    memcpy(coordinator.words, journal->words,
           journal->word_count * sizeof(coordinator.words[0]));
    memcpy(coordinator.bindings, request->bindings,
           request->binding_count * sizeof(coordinator.bindings[0]));

    status = validate_journal();
    if (status != GUEST_RENDER_TRANSACTION_OK) return fail_preflight(status);
    if (coordinator.visual_id.scene_epoch == 0u ||
        !visual_ids_equal(coordinator.visual_id, request->current_visual_id))
        return fail_preflight(GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID);
    if (coordinator.vram_mutation_serial !=
        request->current_vram_mutation_serial)
        return fail_preflight(GUEST_RENDER_TRANSACTION_STALE_VRAM_SERIAL);
    status = resolve_bindings(allow_binding_subset);
    if (status != GUEST_RENDER_TRANSACTION_OK) return fail_preflight(status);
    return GUEST_RENDER_TRANSACTION_OK;
}

const void *guest_render_transaction_process_owner(void) {
    return &coordinator;
}

GuestRenderTransactionStatus guest_render_transaction_execute(
        const GuestRenderTransactionRequest *request) {
    GuestRenderTransactionStatus status;

    clear_deferred_retry();
    if (coordinator.phase != INTERNAL_IDLE &&
        coordinator.phase != INTERNAL_ROLLED_BACK)
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    coordinator.phase = INTERNAL_EXECUTING;
    coordinator.abort_reason = GUEST_RENDER_TRANSACTION_OBSERVATION_NONE;
    coordinator.last_status = GUEST_RENDER_TRANSACTION_OK;
    coordinator.backend_status = GPU_RENDER_TRANSACTION_OK;
    coordinator.rollback_status = GPU_RENDER_TRANSACTION_OK;
    status = copy_and_preflight(request, false);
    if (status != GUEST_RENDER_TRANSACTION_OK) return status;
    return execute_preflighted();
}

GuestRenderTransactionStatus guest_render_transaction_stage_exact(
        GpuRenderTransactionId visual_id,
        uint64_t exact_command_id,
        const GpuRenderSemantic *semantic) {
    size_t index;
    GuestRenderTransactionSemanticBinding *binding;

    clear_deferred_retry();
    if (phase_has_active_transaction()) {
        clear_pending();
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    }
    if (visual_id.scene_epoch == 0u ||
        exact_command_id == GUEST_RENDER_TRANSACTION_NO_COMMAND ||
        !semantic || !semantic_is_valid(semantic)) {
        clear_pending();
        return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    }
    if (coordinator.pending_binding_count != 0u &&
        !visual_ids_equal(coordinator.pending_visual_id, visual_id)) {
        clear_pending();
        return GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID;
    }
    for (index = 0u; index < coordinator.pending_binding_count; ++index) {
        if (coordinator.pending_bindings[index].exact_command_id ==
            exact_command_id) {
            clear_pending();
            return GUEST_RENDER_TRANSACTION_DUPLICATE_TARGET;
        }
    }
    if (coordinator.pending_binding_count ==
        GUEST_RENDER_TRANSACTION_PENDING_CAPACITY) {
        clear_pending();
        return GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED;
    }
    if (coordinator.pending_binding_count == 0u)
        coordinator.pending_visual_id = visual_id;
    binding = &coordinator.pending_bindings[coordinator.pending_binding_count];
    memset(binding, 0, sizeof(*binding));
    binding->has_exact_command_id = true;
    binding->exact_command_id = exact_command_id;
    binding->semantic = *semantic;
    ++coordinator.pending_binding_count;
    return GUEST_RENDER_TRANSACTION_OK;
}

GuestRenderTransactionStatus guest_render_transaction_pending_snapshot(
        GuestRenderTransactionPendingSnapshot *out_snapshot) {
    if (!out_snapshot) return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    out_snapshot->visual_id = coordinator.pending_visual_id;
    out_snapshot->binding_count = coordinator.pending_binding_count;
    return GUEST_RENDER_TRANSACTION_OK;
}

void guest_render_transaction_clear_pending(void) {
    clear_pending();
}

GuestRenderTransactionStatus guest_render_transaction_execute_pending(
        const GuestRenderTransactionPendingExecuteRequest *request) {
    GuestRenderTransactionRequest active_request;
    GuestRenderTransactionStatus status;

    clear_deferred_retry();
    if (coordinator.phase != INTERNAL_IDLE &&
        coordinator.phase != INTERNAL_ROLLED_BACK)
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (coordinator.pending_binding_count == 0u)
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (!request || !request->journal) {
        clear_pending();
        return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    }
    if (!visual_ids_equal(request->journal->visual_id,
                          coordinator.pending_visual_id) ||
        !visual_ids_equal(request->current_visual_id,
                          coordinator.pending_visual_id)) {
        clear_pending();
        return GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID;
    }
    memset(&active_request, 0, sizeof(active_request));
    active_request.journal = request->journal;
    active_request.current_visual_id = request->current_visual_id;
    active_request.current_vram_mutation_serial =
        request->current_vram_mutation_serial;
    active_request.compatibility_callback =
        request->compatibility_callback;
    active_request.compatibility_user_data =
        request->compatibility_user_data;
    active_request.target_side_effects_callback =
        request->target_side_effects_callback;
    active_request.target_side_effects_user_data =
        request->target_side_effects_user_data;
    active_request.material_observation_callback =
        request->material_observation_callback;
    active_request.material_observation_user_data =
        request->material_observation_user_data;
    active_request.begin_checkpoint = request->begin_checkpoint;
    active_request.rollback_checkpoint = request->rollback_checkpoint;
    active_request.commit_checkpoint = request->commit_checkpoint;
    active_request.checkpoint_user_data = request->checkpoint_user_data;
    active_request.replay_callback = request->replay_callback;
    active_request.replay_user_data = request->replay_user_data;
    active_request.bindings = coordinator.pending_bindings;
    active_request.binding_count = coordinator.pending_binding_count;
    coordinator.phase = INTERNAL_EXECUTING;
    coordinator.abort_reason = GUEST_RENDER_TRANSACTION_OBSERVATION_NONE;
    coordinator.last_status = GUEST_RENDER_TRANSACTION_OK;
    coordinator.backend_status = GPU_RENDER_TRANSACTION_OK;
    coordinator.rollback_status = GPU_RENDER_TRANSACTION_OK;
    status = copy_and_preflight(&active_request, true);
    if (status != GUEST_RENDER_TRANSACTION_OK) {
        if (!pending_retry_is_safe(status, request)) clear_pending();
        return status;
    }
    clear_pending();
    return execute_preflighted();
}

GuestRenderTransactionStatus
guest_render_transaction_abort_before_observation(
        GuestRenderTransactionObservationReason reason) {
    if (coordinator.phase != INTERNAL_ACTIVE)
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (reason <= GUEST_RENDER_TRANSACTION_OBSERVATION_NONE ||
        reason > GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT)
        return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    return abort_and_replay(reason, GUEST_RENDER_TRANSACTION_ABORTED);
}

GuestRenderTransactionStatus guest_render_transaction_seal_deferred_retry(
        uint64_t post_replay_vram_mutation_serial) {
    if (coordinator.deferred_retry.phase != DEFERRED_PREPARED)
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (coordinator.phase != INTERNAL_ROLLED_BACK) {
        clear_deferred_retry();
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    }
    coordinator.deferred_retry.post_replay_vram_mutation_serial =
        post_replay_vram_mutation_serial;
    coordinator.deferred_retry.phase = DEFERRED_SEALED;
    g_guest_render_transaction_deferred_active = 1;
    return GUEST_RENDER_TRANSACTION_OK;
}

GuestRenderTransactionStatus guest_render_transaction_deferred_snapshot(
        GuestRenderTransactionDeferredSnapshot *out_snapshot) {
    if (!out_snapshot) return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    if (coordinator.deferred_retry.phase != DEFERRED_SEALED)
        return GUEST_RENDER_TRANSACTION_OK;
    out_snapshot->visual_id = coordinator.deferred_retry.visual_id;
    out_snapshot->post_replay_vram_mutation_serial =
        coordinator.deferred_retry.post_replay_vram_mutation_serial;
    out_snapshot->binding_count = coordinator.deferred_retry.binding_count;
    out_snapshot->sealed = true;
    return GUEST_RENDER_TRANSACTION_OK;
}

void guest_render_transaction_invalidate_deferred(void) {
    if (coordinator.deferred_retry.phase == DEFERRED_NONE) return;
    clear_deferred_retry();
}

GuestRenderTransactionStatus guest_render_transaction_begin_deferred(
        GpuRenderTransactionId current_visual_id,
        uint64_t current_vram_mutation_serial) {
    GuestRenderDeferredRetry retry;
    bool checkpoint_rollback_succeeded;

    if (coordinator.deferred_retry.phase != DEFERRED_SEALED)
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (coordinator.phase != INTERNAL_IDLE &&
        coordinator.phase != INTERNAL_ROLLED_BACK) {
        clear_deferred_retry();
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    }
    if (!visual_ids_equal(coordinator.deferred_retry.visual_id,
                          current_visual_id)) {
        clear_deferred_retry();
        coordinator.last_status = GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID;
        return coordinator.last_status;
    }
    if (coordinator.deferred_retry.post_replay_vram_mutation_serial !=
        current_vram_mutation_serial) {
        clear_deferred_retry();
        coordinator.last_status = GUEST_RENDER_TRANSACTION_STALE_VRAM_SERIAL;
        return coordinator.last_status;
    }

    retry = coordinator.deferred_retry;
    memset(&coordinator.deferred_retry, 0,
           sizeof(coordinator.deferred_retry));
    clear_private_transaction();
    coordinator.visual_id = retry.visual_id;
    coordinator.vram_mutation_serial =
        retry.post_replay_vram_mutation_serial;
    coordinator.binding_count = retry.binding_count;
    coordinator.begin_checkpoint = retry.begin_checkpoint;
    coordinator.rollback_checkpoint = retry.rollback_checkpoint;
    coordinator.commit_checkpoint = retry.commit_checkpoint;
    coordinator.checkpoint_user_data = retry.checkpoint_user_data;
    coordinator.material_observation_callback =
        retry.material_observation_callback;
    coordinator.material_observation_user_data =
        retry.material_observation_user_data;
    coordinator.material_observation_count =
        retry.material_observation_count;
    if (retry.material_observation_count != 0u)
        memcpy(coordinator.material_observations,
               retry.material_observations,
               retry.material_observation_count *
                   sizeof(coordinator.material_observations[0]));
    coordinator.fallback_material_observation_count =
        retry.fallback_material_observation_count;
    if (retry.fallback_material_observation_count != 0u)
        memcpy(coordinator.fallback_material_observations,
               retry.fallback_material_observations,
               retry.fallback_material_observation_count *
                   sizeof(coordinator.fallback_material_observations[0]));
    coordinator.active_deferred = true;
    coordinator.phase = INTERNAL_EXECUTING;
    coordinator.abort_reason = GUEST_RENDER_TRANSACTION_OBSERVATION_NONE;
    coordinator.last_status = GUEST_RENDER_TRANSACTION_OK;
    coordinator.backend_status = GPU_RENDER_TRANSACTION_OK;
    coordinator.rollback_status = GPU_RENDER_TRANSACTION_OK;

    if (!coordinator.begin_checkpoint ||
        !coordinator.rollback_checkpoint ||
        !coordinator.commit_checkpoint ||
        !coordinator.begin_checkpoint(
            coordinator.visual_id, coordinator.vram_mutation_serial,
            coordinator.checkpoint_user_data)) {
        (void)gr_deferred_candidate_discard(retry.candidate_token);
        publish_material_observations(
            retry.material_observation_callback,
            retry.material_observation_user_data,
            retry.fallback_material_observations,
            retry.fallback_material_observation_count);
        clear_private_transaction();
        coordinator.phase = INTERNAL_ROLLED_BACK;
        coordinator.abort_reason =
            GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_BEGIN_FAILURE;
        coordinator.last_status =
            GUEST_RENDER_TRANSACTION_CHECKPOINT_BEGIN_FAILURE;
        return coordinator.last_status;
    }
    coordinator.checkpoint_open = true;
    coordinator.backend_status = gr_deferred_transaction_begin(
        coordinator.visual_id, coordinator.vram_mutation_serial,
        retry.candidate_token);
    if (coordinator.backend_status != GPU_RENDER_TRANSACTION_OK) {
        checkpoint_rollback_succeeded = rollback_checkpoint_once();
        (void)gr_deferred_candidate_discard(retry.candidate_token);
        if (checkpoint_rollback_succeeded)
            publish_material_observations(
                retry.material_observation_callback,
                retry.material_observation_user_data,
                retry.fallback_material_observations,
                retry.fallback_material_observation_count);
        clear_private_transaction();
        coordinator.phase = INTERNAL_ROLLED_BACK;
        coordinator.abort_reason = checkpoint_rollback_succeeded
            ? GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE
            : GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_ROLLBACK_FAILURE;
        coordinator.last_status = checkpoint_rollback_succeeded
            ? GUEST_RENDER_TRANSACTION_BACKEND_FAILURE
            : GUEST_RENDER_TRANSACTION_CHECKPOINT_ROLLBACK_FAILURE;
        return coordinator.last_status;
    }
    coordinator.backend_open = true;
    coordinator.phase = INTERNAL_ACTIVE;
    return GUEST_RENDER_TRANSACTION_OK;
}

GuestRenderTransactionStatus guest_render_transaction_present(
        GpuRenderTransactionId current_visual_id,
        uint64_t current_vram_mutation_serial,
        const GpuRenderPresent *present) {
    if (coordinator.phase != INTERNAL_ACTIVE)
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (!present) return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    if (!visual_ids_equal(coordinator.visual_id, current_visual_id)) {
        return abort_and_replay(
            GUEST_RENDER_TRANSACTION_OBSERVATION_VISUAL_ID_CHANGED,
            GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID);
    }
    if (coordinator.vram_mutation_serial != current_vram_mutation_serial) {
        return abort_and_replay(
            GUEST_RENDER_TRANSACTION_OBSERVATION_VRAM_SERIAL_CHANGED,
            GUEST_RENDER_TRANSACTION_STALE_VRAM_SERIAL);
    }
    coordinator.backend_status = gr_commit_validate(
        coordinator.visual_id, current_vram_mutation_serial, present);
    if (coordinator.backend_status != GPU_RENDER_TRANSACTION_READY) {
        return abort_and_replay(
            GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE,
            GUEST_RENDER_TRANSACTION_BACKEND_FAILURE);
    }
    coordinator.backend_open = false;
    coordinator.phase = INTERNAL_AWAITING_SWAP;
    coordinator.last_status = GUEST_RENDER_TRANSACTION_READY;
    return GUEST_RENDER_TRANSACTION_READY;
}

GuestRenderTransactionStatus guest_render_transaction_post_swap_success(void) {
    GpuRenderTransactionId published_visual_id;
    bool checkpoint_rollback_succeeded;
    bool counters_exhausted;

    if (coordinator.phase != INTERNAL_AWAITING_SWAP)
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (!coordinator.commit_checkpoint(
            coordinator.visual_id, coordinator.vram_mutation_serial,
            coordinator.checkpoint_user_data)) {
        checkpoint_rollback_succeeded = rollback_checkpoint_once();
        published_visual_id = coordinator.visual_id;
        if (coordinator.active_deferred && checkpoint_rollback_succeeded)
            publish_material_observations(
                coordinator.material_observation_callback,
                coordinator.material_observation_user_data,
                coordinator.fallback_material_observations,
                coordinator.fallback_material_observation_count);
        clear_private_transaction();
        clear_pending_for_visual(published_visual_id);
        coordinator.phase = INTERNAL_ROLLED_BACK;
        coordinator.abort_reason = checkpoint_rollback_succeeded ?
            GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_COMMIT_FAILURE :
            GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_ROLLBACK_FAILURE;
        coordinator.last_status = checkpoint_rollback_succeeded ?
            GUEST_RENDER_TRANSACTION_CHECKPOINT_COMMIT_FAILURE :
            GUEST_RENDER_TRANSACTION_CHECKPOINT_ROLLBACK_FAILURE;
        coordinator.backend_status = GPU_RENDER_TRANSACTION_BACKEND_ERROR;
        coordinator.rollback_status = GPU_RENDER_TRANSACTION_OK;
        return coordinator.last_status;
    }
    coordinator.checkpoint_open = false;
    publish_material_observations(
        coordinator.material_observation_callback,
        coordinator.material_observation_user_data,
        coordinator.material_observations,
        coordinator.material_observation_count);
    counters_exhausted =
        coordinator.published_transaction_count == UINT64_MAX ||
        coordinator.binding_count >
            UINT64_MAX - coordinator.published_substitution_count;
    if (!counters_exhausted) {
        ++coordinator.published_transaction_count;
        coordinator.published_substitution_count += coordinator.binding_count;
    }
    published_visual_id = coordinator.visual_id;
    clear_private_transaction();
    clear_pending_for_visual(published_visual_id);
    coordinator.phase = INTERNAL_IDLE;
    coordinator.abort_reason = GUEST_RENDER_TRANSACTION_OBSERVATION_NONE;
    coordinator.last_status = counters_exhausted ?
        GUEST_RENDER_TRANSACTION_COUNTER_EXHAUSTED :
        GUEST_RENDER_TRANSACTION_OK;
    coordinator.backend_status = GPU_RENDER_TRANSACTION_OK;
    coordinator.rollback_status = GPU_RENDER_TRANSACTION_OK;
    return coordinator.last_status;
}

GuestRenderTransactionStatus guest_render_transaction_post_swap_failure(
        bool checkpoint_already_restored) {
    GpuRenderTransactionId failed_visual_id;
    bool checkpoint_rollback_succeeded = true;

    if (coordinator.phase != INTERNAL_AWAITING_SWAP)
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    if (checkpoint_already_restored) {
        coordinator.checkpoint_open = false;
    } else {
        checkpoint_rollback_succeeded = rollback_checkpoint_once();
    }
    failed_visual_id = coordinator.visual_id;
    if (coordinator.active_deferred && checkpoint_rollback_succeeded)
        publish_material_observations(
            coordinator.material_observation_callback,
            coordinator.material_observation_user_data,
            coordinator.fallback_material_observations,
            coordinator.fallback_material_observation_count);
    clear_private_transaction();
    clear_pending_for_visual(failed_visual_id);
    coordinator.phase = INTERNAL_ROLLED_BACK;
    coordinator.abort_reason = checkpoint_rollback_succeeded ?
        GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_PRESENT_FAILURE :
        GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_ROLLBACK_FAILURE;
    coordinator.last_status = checkpoint_rollback_succeeded ?
        GUEST_RENDER_TRANSACTION_BACKEND_PRESENT_FAILURE :
        GUEST_RENDER_TRANSACTION_CHECKPOINT_ROLLBACK_FAILURE;
    coordinator.backend_status = GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    coordinator.rollback_status = GPU_RENDER_TRANSACTION_OK;
    return coordinator.last_status;
}

GuestRenderTransactionStatus guest_render_transaction_snapshot(
        GuestRenderTransactionSnapshot *out_snapshot) {
    if (!out_snapshot) return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    switch (coordinator.phase) {
    case INTERNAL_EXECUTING:
    case INTERNAL_ACTIVE:
    case INTERNAL_ABORTING:
        out_snapshot->phase = GUEST_RENDER_TRANSACTION_ACTIVE;
        break;
    case INTERNAL_AWAITING_SWAP:
        out_snapshot->phase = GUEST_RENDER_TRANSACTION_AWAITING_SWAP;
        break;
    case INTERNAL_ROLLED_BACK:
        out_snapshot->phase = GUEST_RENDER_TRANSACTION_ROLLED_BACK;
        break;
    case INTERNAL_IDLE:
    default:
        out_snapshot->phase = GUEST_RENDER_TRANSACTION_IDLE;
        break;
    }
    out_snapshot->abort_reason = coordinator.abort_reason;
    out_snapshot->last_status = coordinator.last_status;
    out_snapshot->backend_status = coordinator.backend_status;
    out_snapshot->rollback_status = coordinator.rollback_status;
    out_snapshot->active_visual_id = coordinator.visual_id;
    out_snapshot->active_vram_mutation_serial =
        coordinator.vram_mutation_serial;
    out_snapshot->active_command_count = coordinator.command_count;
    out_snapshot->active_binding_count = coordinator.binding_count;
    out_snapshot->published_transaction_count =
        coordinator.published_transaction_count;
    out_snapshot->published_substitution_count =
        coordinator.published_substitution_count;
    return GUEST_RENDER_TRANSACTION_OK;
}

size_t guest_render_transaction_command_capacity(void) {
    return GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY;
}

size_t guest_render_transaction_word_capacity(void) {
    return GUEST_RENDER_TRANSACTION_WORD_CAPACITY;
}

size_t guest_render_transaction_binding_capacity(void) {
    return GUEST_RENDER_TRANSACTION_BINDING_CAPACITY;
}

size_t guest_render_transaction_pending_capacity(void) {
    return GUEST_RENDER_TRANSACTION_PENDING_CAPACITY;
}

const char *guest_render_transaction_observation_reason_name(uint32_t reason) {
    switch (reason) {
    case GUEST_RENDER_TRANSACTION_OBSERVATION_NONE: return "none";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_GP1: return "gp1";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_GPUSTAT: return "gpustat";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_GPUREAD: return "gpuread";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_DMA2_GPU_TO_RAM_C0:
        return "dma2_gpu_to_ram_c0";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_DELAYED_COMPLETION:
        return "delayed_completion";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_IRQ: return "irq";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_LATE_MMIO: return "late_mmio";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_SECOND_LIST:
        return "second_list";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_LATE_COMMAND:
        return "late_command";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_VISUAL_ID_CHANGED:
        return "visual_id_changed";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_VRAM_SERIAL_CHANGED:
        return "vram_serial_changed";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_COMPATIBILITY_FAILURE:
        return "compatibility_failure";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_TARGET_SIDE_EFFECTS_FAILURE:
        return "target_side_effects_failure";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_FAILURE:
        return "backend_failure";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_PRESENT_FAILURE:
        return "backend_present_failure";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_BEGIN_FAILURE:
        return "checkpoint_begin_failure";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_ROLLBACK_FAILURE:
        return "checkpoint_rollback_failure";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_CHECKPOINT_COMMIT_FAILURE:
        return "checkpoint_commit_failure";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT:
        return "caller_abort";
    case GUEST_RENDER_TRANSACTION_OBSERVATION_BACKEND_ROLLBACK_FAILURE:
        return "backend_rollback_failure";
    default: return "unknown";
    }
}

#ifdef GUEST_RENDER_TRANSACTION_TESTING
void guest_render_transaction_test_reset(void) {
    clear_deferred_retry();
    memset(&coordinator, 0, sizeof(coordinator));
}
#endif
