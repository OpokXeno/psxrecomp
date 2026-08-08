#include "native_render_baseline.h"

#include "guest_render_native_stream.h"
#include "native_render_baseline_runtime.h"

#include <limits.h>
#include <string.h>

#define FNV_OFFSET UINT64_C(1469598103934665603)
#define FNV_PRIME UINT64_C(1099511628211)
#define PSX_RAM_BYTES (2u * 1024u * 1024u)
#define PSX_RAM_WORD_MASK UINT32_C(0x001ffffc)
#define OT_TERMINATOR UINT32_C(0x00ffffff)

typedef struct {
    uint32_t producer_phys;
    uint32_t max_vblanks;
    uint32_t auto_finalize_vblanks;
    uint32_t current_ot_nodes;
    uint32_t expected_ot_node;
    GteAttributionSnapshot gte_at_arm;
    int ot_open;
    int failed;
} NativeRenderBaselineState;

int g_native_render_baseline_armed;
static NativeRenderBaselineSnapshot baseline;
static NativeRenderBaselineState state;

static void note_native_stream_material(
        uint64_t command_id, const GpuRenderMaterial *material) {
    NativeRenderBaselineMaterialObservation observation = { 0 };

    if (!material || command_id > UINT32_MAX) return;
    observation.material = *material;
    observation.provenance = NATIVE_RENDER_BASELINE_MATERIAL_OT;
    observation.command_address = (uint32_t)command_id;
    observation.source_word_ordinal = command_id;
    observation.container_ordinal = command_id;
    observation.submission_ordinal = baseline.material_samples;
    observation.word_count = 1u;
    native_render_baseline_note_material(&observation);
}

static uint64_t mix(uint64_t hash, uint64_t value) {
    return (hash ^ value) * FNV_PRIME;
}

static int add_u64(uint64_t *value, uint64_t addition) {
    if (*value > UINT64_MAX - addition) return 0;
    *value += addition;
    return 1;
}

static int normalize_ram_word(uint32_t address, uint32_t *out) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);

    if ((segment != 0u && segment != UINT32_C(0x80000000) &&
         segment != UINT32_C(0xa0000000)) ||
        physical >= PSX_RAM_BYTES || (physical & 3u) != 0u)
        return 0;
    *out = physical & PSX_RAM_WORD_MASK;
    return 1;
}

static int material_is_valid(const GpuRenderMaterial *material) {
    const uint16_t encoded_depth =
        material ? (uint16_t)((material->tpage >> 7u) & 3u) : 3u;

    return material && material->tpage <= UINT16_C(0x01ff) &&
           encoded_depth != 3u &&
           material->texture_page_x ==
               (material->tpage & UINT16_C(0x000f)) &&
           material->texture_page_y == ((material->tpage >> 4u) & 1u) &&
           material->blend_mode ==
               (GpuRenderBlendMode)((material->tpage >> 5u) & 3u) &&
           material->texture_depth == (GpuRenderTextureDepth)encoded_depth &&
           material->clut_x <= 1023u && (material->clut_x & 15u) == 0u &&
           material->clut_y <= 511u &&
           material->draw_area_left <= material->draw_area_right &&
           material->draw_area_top <= material->draw_area_bottom &&
           material->draw_area_right <= 1023u &&
           material->draw_area_bottom <= 1023u &&
           material->draw_offset_x >= -1024 && material->draw_offset_x <= 1023 &&
           material->draw_offset_y >= -1024 && material->draw_offset_y <= 1023 &&
           material->texture_window_mask_x <= 31u &&
           material->texture_window_mask_y <= 31u &&
           material->texture_window_offset_x <= 31u &&
           material->texture_window_offset_y <= 31u &&
           (material->shading == GPU_RENDER_SHADING_FLAT ||
            material->shading == GPU_RENDER_SHADING_GOURAUD) &&
           material->textured <= 1u && material->raw_texture <= 1u &&
           material->semi_transparent <= 1u && material->dither <= 1u &&
           material->mask_set <= 1u && material->mask_check <= 1u &&
           (!material->raw_texture || material->textured);
}

static uint64_t material_state_digest(const GpuRenderMaterial *material) {
    uint64_t digest = FNV_OFFSET;

#define MIX_MATERIAL(field) digest = mix(digest, (uint64_t)material->field)
    MIX_MATERIAL(tpage);
    MIX_MATERIAL(texture_page_x);
    MIX_MATERIAL(texture_page_y);
    MIX_MATERIAL(clut_x);
    MIX_MATERIAL(clut_y);
    MIX_MATERIAL(draw_area_left);
    MIX_MATERIAL(draw_area_top);
    MIX_MATERIAL(draw_area_right);
    MIX_MATERIAL(draw_area_bottom);
    digest = mix(digest, (uint64_t)(uint16_t)material->draw_offset_x);
    digest = mix(digest, (uint64_t)(uint16_t)material->draw_offset_y);
    MIX_MATERIAL(texture_depth);
    MIX_MATERIAL(texture_window_mask_x);
    MIX_MATERIAL(texture_window_mask_y);
    MIX_MATERIAL(texture_window_offset_x);
    MIX_MATERIAL(texture_window_offset_y);
    MIX_MATERIAL(shading);
    MIX_MATERIAL(textured);
    MIX_MATERIAL(raw_texture);
    MIX_MATERIAL(semi_transparent);
    MIX_MATERIAL(blend_mode);
    MIX_MATERIAL(dither);
    MIX_MATERIAL(mask_set);
    MIX_MATERIAL(mask_check);
#undef MIX_MATERIAL
    return digest;
}

static void fail_observation(NativeRenderBaselineReason reason) {
    baseline.complete = 0;
    baseline.incomplete_reason = reason;
    baseline.overflow |= reason == NATIVE_RENDER_BASELINE_OVERFLOW ||
                         reason == NATIVE_RENDER_BASELINE_GTE_OVERFLOW ||
                         reason == NATIVE_RENDER_BASELINE_VRAM_SERIAL_OVERFLOW;
    baseline.invalid_ot |= reason == NATIVE_RENDER_BASELINE_INVALID_OT;
    baseline.cyclic_ot |= reason == NATIVE_RENDER_BASELINE_CYCLIC_OT;
    state.failed = 1;
    state.producer_phys = 0;
    state.current_ot_nodes = 0;
    state.expected_ot_node = 0;
    state.ot_open = 0;
    g_native_render_baseline_armed = 0;
}

static int gte_summary(GteAttributionSnapshot *out) {
    const GteAttributionResult result =
        gte_attribution_snapshot(out, NULL, 0u, NULL, 0u);
    return result == GTE_ATTRIBUTION_OK ||
           result == GTE_ATTRIBUTION_INSUFFICIENT_CAPACITY;
}

static NativeRenderBaselineReason capture_guest_render_state(void) {
    GuestRenderBridgeSnapshot bridge = {0};
    GuestRenderBridgeSnapshot historical_bridge = {0};
    GuestRenderCompletedState completed = {0};
    GuestRenderStatus present_status;

    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        bridge.modes.requested_render_mode < GUEST_RENDER_RENDER_ORIGINAL ||
        bridge.modes.requested_render_mode > GUEST_RENDER_RENDER_NATIVE ||
        bridge.modes.effective_render_mode < GUEST_RENDER_RENDER_ORIGINAL ||
        bridge.modes.effective_render_mode > GUEST_RENDER_RENDER_NATIVE)
        return NATIVE_RENDER_BASELINE_MISSING_VISUAL_STATE;

    present_status = guest_render_bridge_present(&completed);
    if (present_status == GUEST_RENDER_NO_COMPLETED_STATE &&
        guest_render_bridge_last_completed(&historical_bridge, &completed) ==
            GUEST_RENDER_OK) {
        bridge = historical_bridge;
        present_status = GUEST_RENDER_OK;
    }

    if (present_status == GUEST_RENDER_OK) {
        if (completed.id.scene_epoch == 0u ||
            completed.slot_count != bridge.slot_count ||
            completed.binding_count != bridge.binding_count)
            return NATIVE_RENDER_BASELINE_MISSING_VISUAL_STATE;
        baseline.visual_state_id = completed.id;
    } else if (present_status != GUEST_RENDER_NO_COMPLETED_STATE ||
               bridge.modes.requested_render_mode !=
                   GUEST_RENDER_RENDER_ORIGINAL ||
               bridge.modes.effective_render_mode !=
                   GUEST_RENDER_RENDER_ORIGINAL ||
               bridge.state_open || bridge.producer_open ||
               baseline.visual_state_id.scene_epoch == 0u) {
        return NATIVE_RENDER_BASELINE_MISSING_VISUAL_STATE;
    }

    baseline.requested_render_mode = bridge.modes.requested_render_mode;
    baseline.effective_render_mode = bridge.modes.effective_render_mode;
    baseline.fallback_reason = bridge.fallback_reason;
    baseline.fallback_count = bridge.fallback_count;
    baseline.producer_count = present_status == GUEST_RENDER_OK
        ? (uint64_t)completed.slot_count : (uint64_t)bridge.slot_count;
    baseline.producer_binding_count = present_status == GUEST_RENDER_OK
        ? (uint64_t)completed.binding_count : (uint64_t)bridge.binding_count;
    baseline.field_completeness_mask |=
        NATIVE_RENDER_BASELINE_FIELD_VISUAL_STATE |
        NATIVE_RENDER_BASELINE_FIELD_RENDER_MODES |
        NATIVE_RENDER_BASELINE_FIELD_PRODUCER_COUNTS;
    return NATIVE_RENDER_BASELINE_COMPLETE;
}

static void note_original_visual_state(void) {
    GuestRenderBridgeSnapshot bridge = {0};

    if (baseline.visual_state_id.scene_epoch != 0u ||
        guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        bridge.state_open || bridge.producer_open ||
        bridge.modes.requested_render_mode != GUEST_RENDER_RENDER_ORIGINAL ||
        bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_ORIGINAL)
        return;

    /* Original has no bridge transaction. The first authenticated OT list is
     * the real visual submission boundary available on that route. */
    baseline.visual_state_id.scene_epoch = 1u;
    baseline.visual_state_id.state_sequence = baseline.ot_lists + 1u;
}

static NativeRenderBaselineReason capture_gte_counts(void) {
    GteAttributionSnapshot current = {0};
    size_t tier;

    if (!gte_summary(&current))
        return NATIVE_RENDER_BASELINE_INCOMPLETE_OBSERVATION;
    if (current.total_count < state.gte_at_arm.total_count ||
        current.inside_producer_count <
            state.gte_at_arm.inside_producer_count ||
        current.outside_producer_count <
            state.gte_at_arm.outside_producer_count)
        return NATIVE_RENDER_BASELINE_GTE_OVERFLOW;

    baseline.gte_total_count =
        current.total_count - state.gte_at_arm.total_count;
    baseline.gte_inside_producer_count =
        current.inside_producer_count -
        state.gte_at_arm.inside_producer_count;
    baseline.gte_outside_producer_count =
        current.outside_producer_count -
        state.gte_at_arm.outside_producer_count;
    for (tier = 0; tier < GTE_ATTRIBUTION_TIER_COUNT; ++tier) {
        if (current.tier_counts[tier] <
            state.gte_at_arm.tier_counts[tier])
            return NATIVE_RENDER_BASELINE_GTE_OVERFLOW;
        baseline.gte_tier_counts[tier] =
            current.tier_counts[tier] -
            state.gte_at_arm.tier_counts[tier];
    }
    baseline.gte_overflow_reason = current.overflow_reason;
    baseline.gte_blocked = current.blocked ? 1 : 0;
    if (current.blocked ||
        current.overflow_reason != GTE_ATTRIBUTION_OVERFLOW_NONE)
        return NATIVE_RENDER_BASELINE_GTE_OVERFLOW;
    baseline.field_completeness_mask |=
        NATIVE_RENDER_BASELINE_FIELD_GTE_COUNTS;
    return NATIVE_RENDER_BASELINE_COMPLETE;
}

static uint64_t normalized_digest(
        const NativeRenderBaselineSnapshot *snapshot) {
    uint64_t hash = FNV_OFFSET;
    size_t tier;

#define MIX_FIELD(field) hash = mix(hash, (uint64_t)snapshot->field)
    MIX_FIELD(schema_version);
    MIX_FIELD(enabled);
    MIX_FIELD(complete);
    MIX_FIELD(overflow);
    MIX_FIELD(invalid_ot);
    MIX_FIELD(cyclic_ot);
    MIX_FIELD(incomplete_reason);
    MIX_FIELD(field_completeness_mask);
    MIX_FIELD(required_field_mask);
    hash = mix(hash, snapshot->visual_state_id.scene_epoch);
    hash = mix(hash, snapshot->visual_state_id.state_sequence);
    MIX_FIELD(requested_render_mode);
    MIX_FIELD(effective_render_mode);
    MIX_FIELD(fallback_reason);
    MIX_FIELD(fallback_count);
    MIX_FIELD(producer_count);
    MIX_FIELD(producer_binding_count);
    MIX_FIELD(interpreter_calls);
    MIX_FIELD(native_calls);
    MIX_FIELD(gte_total_count);
    MIX_FIELD(gte_inside_producer_count);
    MIX_FIELD(gte_outside_producer_count);
    for (tier = 0; tier < GTE_ATTRIBUTION_TIER_COUNT; ++tier)
        hash = mix(hash, snapshot->gte_tier_counts[tier]);
    MIX_FIELD(gte_overflow_reason);
    MIX_FIELD(gte_blocked);
    MIX_FIELD(ot_lists);
    MIX_FIELD(ot_nodes);
    MIX_FIELD(ot_words);
    MIX_FIELD(ot_digest);
    MIX_FIELD(topology_digest);
    MIX_FIELD(material_samples);
    MIX_FIELD(material_digest);
    MIX_FIELD(gp0_writes);
    MIX_FIELD(gp1_writes);
    MIX_FIELD(vram_mutations);
    MIX_FIELD(global_vram_mutation_serial);
    MIX_FIELD(global_vram_serial_overflowed);
    MIX_FIELD(vram_digest);
    MIX_FIELD(gpu_digest);
    MIX_FIELD(display_samples);
    MIX_FIELD(display15_digest);
    MIX_FIELD(display_digest);
    MIX_FIELD(host_framebuffer_samples);
    MIX_FIELD(host_framebuffer_digest);
    MIX_FIELD(audio_frames);
    MIX_FIELD(audio_events);
    MIX_FIELD(vblank_delta);
    MIX_FIELD(guest_cycle_delta);
    MIX_FIELD(cycles_per_vblank);
    MIX_FIELD(cycle_digest);
    MIX_FIELD(audio_digest);
    MIX_FIELD(game_digest);
    MIX_FIELD(camera_actor_digest);
#undef MIX_FIELD
    return hash;
}

void native_render_baseline_reset(void) {
    memset(&baseline, 0, sizeof(baseline));
    memset(&state, 0, sizeof(state));
    baseline.schema_version = NATIVE_RENDER_BASELINE_SCHEMA_VERSION;
    baseline.required_field_mask = NATIVE_RENDER_BASELINE_FIELD_ALL;
    baseline.incomplete_reason = NATIVE_RENDER_BASELINE_DISABLED;
    baseline.ot_digest = FNV_OFFSET;
    baseline.topology_digest = FNV_OFFSET;
    baseline.material_digest = FNV_OFFSET;
    baseline.vram_digest = FNV_OFFSET;
    baseline.gpu_digest = FNV_OFFSET;
    baseline.display15_digest = FNV_OFFSET;
    baseline.display_digest = FNV_OFFSET;
    baseline.host_framebuffer_digest = FNV_OFFSET;
    baseline.cycle_digest = FNV_OFFSET;
    baseline.audio_digest = FNV_OFFSET;
    native_render_baseline_runtime_reset();
    guest_render_native_stream_set_material_observer(NULL);
    gte_attribution_set_enabled(false);
    g_native_render_baseline_armed = 0;
}

int native_render_baseline_arm(const NativeRenderBaselineConfig *config) {
    uint32_t producer_phys;

    native_render_baseline_reset();
    if (!config) {
        baseline.incomplete_reason = NATIVE_RENDER_BASELINE_INVALID_CONFIG;
        return 0;
    }
    producer_phys = config->authenticated_producer_address & UINT32_C(0x1fffffff);
    if (producer_phys == 0u ||
        (config->authenticated_producer_address & 3u) != 0u ||
        producer_phys >= PSX_RAM_BYTES || config->max_vblanks == 0u ||
        config->max_vblanks > NATIVE_RENDER_BASELINE_VBLANK_CAPACITY) {
        baseline.incomplete_reason = NATIVE_RENDER_BASELINE_INVALID_CONFIG;
        return 0;
    }
    if (config->game_digest == 0u) {
        baseline.incomplete_reason = NATIVE_RENDER_BASELINE_MISSING_GAME_DIGEST;
        return 0;
    }
    gte_attribution_reset();
    if (!gte_summary(&state.gte_at_arm) || state.gte_at_arm.blocked ||
        state.gte_at_arm.overflow_reason != GTE_ATTRIBUTION_OVERFLOW_NONE) {
        baseline.incomplete_reason = NATIVE_RENDER_BASELINE_GTE_OVERFLOW;
        baseline.overflow = 1;
        return 0;
    }

    baseline.enabled = 1;
    baseline.game_digest = config->game_digest;
    baseline.field_completeness_mask |=
        NATIVE_RENDER_BASELINE_FIELD_GAME_IDENTITY;
    baseline.incomplete_reason = NATIVE_RENDER_BASELINE_INCOMPLETE_OBSERVATION;
    state.producer_phys = producer_phys;
    state.max_vblanks = config->max_vblanks;
    native_render_baseline_runtime_arm();
    guest_render_native_stream_set_material_observer(note_native_stream_material);
    g_native_render_baseline_armed = 1;
    return 1;
}

void native_render_baseline_set_auto_finalize_vblanks(uint32_t vblanks) {
    if (g_native_render_baseline_armed && vblanks > 0u &&
        vblanks <= state.max_vblanks)
        state.auto_finalize_vblanks = vblanks;
}

void native_render_baseline_note_execution_impl(uint32_t address,
                                                 NativeRenderBaselineMode mode) {
    if ((address & UINT32_C(0x1fffffff)) != state.producer_phys) return;
    if (mode == NATIVE_RENDER_BASELINE_INTERPRETER) {
        if (!add_u64(&baseline.interpreter_calls, 1u))
            fail_observation(NATIVE_RENDER_BASELINE_OVERFLOW);
    } else if (mode == NATIVE_RENDER_BASELINE_NATIVE) {
        if (!add_u64(&baseline.native_calls, 1u))
            fail_observation(NATIVE_RENDER_BASELINE_OVERFLOW);
    }
}

void native_render_baseline_note_camera_actor_digest(uint64_t digest) {
    if (g_native_render_baseline_armed)
        baseline.camera_actor_digest = digest;
}

void native_render_baseline_note_material(
        const NativeRenderBaselineMaterialObservation *observation) {
    const GpuRenderMaterial *material;

    if (!g_native_render_baseline_armed) return;
    if (!observation || observation->word_count == 0u ||
        (observation->provenance != NATIVE_RENDER_BASELINE_MATERIAL_OT &&
         observation->provenance != NATIVE_RENDER_BASELINE_MATERIAL_DMA &&
         observation->provenance != NATIVE_RENDER_BASELINE_MATERIAL_MMIO) ||
        !material_is_valid(&observation->material)) {
        fail_observation(NATIVE_RENDER_BASELINE_INCOMPLETE_OBSERVATION);
        return;
    }
    if (!add_u64(&baseline.material_samples, 1u)) {
        fail_observation(NATIVE_RENDER_BASELINE_OVERFLOW);
        return;
    }
    material = &observation->material;
    baseline.material_digest += material_state_digest(material);
    baseline.field_completeness_mask |=
        NATIVE_RENDER_BASELINE_FIELD_MATERIAL_DIGEST;
}

void native_render_baseline_note_host_framebuffer_digest(uint64_t digest) {
    if (!g_native_render_baseline_armed) return;
    if (!add_u64(&baseline.host_framebuffer_samples, 1u)) {
        fail_observation(NATIVE_RENDER_BASELINE_OVERFLOW);
        return;
    }
    baseline.host_framebuffer_digest =
        mix(baseline.host_framebuffer_digest, digest);
    baseline.field_completeness_mask |=
        NATIVE_RENDER_BASELINE_FIELD_HOST_FRAMEBUFFER_DIGEST;
}

int native_render_baseline_host_framebuffer_capture_due(void) {
    if (!g_native_render_baseline_armed ||
        baseline.host_framebuffer_samples != 0u)
        return 0;
    return state.auto_finalize_vblanks == 0u ||
           baseline.vblank_delta + 1u >= state.auto_finalize_vblanks;
}

void native_render_baseline_ot_begin(uint32_t list_address) {
    uint32_t normalized;
    NativeRenderBaselineReason reason;

    if (!g_native_render_baseline_armed) return;
    if ((baseline.field_completeness_mask &
         NATIVE_RENDER_BASELINE_FIELD_VISUAL_STATE) == 0u) {
        note_original_visual_state();
        reason = capture_guest_render_state();
        if (reason != NATIVE_RENDER_BASELINE_COMPLETE &&
            reason != NATIVE_RENDER_BASELINE_MISSING_VISUAL_STATE)
            fail_observation(reason);
    }
    if (state.ot_open || !normalize_ram_word(list_address, &normalized)) {
        fail_observation(NATIVE_RENDER_BASELINE_INVALID_OT);
        return;
    }
    state.ot_open = 1;
    state.current_ot_nodes = 0;
    state.expected_ot_node = normalized;
    baseline.ot_digest = mix(baseline.ot_digest, UINT64_C(0x4f54424c));
    baseline.ot_digest = mix(baseline.ot_digest, normalized);
    baseline.ot_digest = mix(baseline.ot_digest, baseline.ot_lists);
    baseline.topology_digest = baseline.ot_digest;
}

void native_render_baseline_ot_node(const NativeRenderBaselineOtNode *node) {
    uint32_t normalized_node;
    uint32_t normalized_next = OT_TERMINATOR;
    uint64_t total_nodes;
    uint64_t total_words;

    if (!g_native_render_baseline_armed) return;
    if (!state.ot_open || !node || node->packet_words > 255u ||
        !normalize_ram_word(node->node_address, &normalized_node) ||
        normalized_node != state.expected_ot_node ||
        node->final_ordinal != state.current_ot_nodes ||
        (node->next_node_address != OT_TERMINATOR &&
         !normalize_ram_word(node->next_node_address, &normalized_next))) {
        fail_observation(NATIVE_RENDER_BASELINE_INVALID_OT);
        return;
    }
    total_nodes = baseline.ot_nodes;
    total_words = baseline.ot_words;
    if (state.current_ot_nodes >= NATIVE_RENDER_BASELINE_OT_CAPACITY ||
        !add_u64(&total_nodes, 1u) ||
        !add_u64(&total_words, node->packet_words)) {
        fail_observation(NATIVE_RENDER_BASELINE_OVERFLOW);
        return;
    }

    baseline.ot_nodes = total_nodes;
    baseline.ot_words = total_words;
    baseline.ot_digest = mix(baseline.ot_digest, normalized_node);
    baseline.ot_digest = mix(baseline.ot_digest, normalized_next);
    baseline.ot_digest = mix(baseline.ot_digest, node->packet_words);
    baseline.ot_digest = mix(baseline.ot_digest, node->final_ordinal);
    baseline.topology_digest = baseline.ot_digest;
    state.expected_ot_node = normalized_next;
    ++state.current_ot_nodes;
}

void native_render_baseline_ot_end(NativeRenderBaselineOtStatus status) {
    if (!g_native_render_baseline_armed) return;
    if (!state.ot_open) {
        fail_observation(NATIVE_RENDER_BASELINE_INVALID_OT);
        return;
    }
    if (status == NATIVE_RENDER_BASELINE_OT_VALID &&
        (state.current_ot_nodes == 0u ||
         state.expected_ot_node != OT_TERMINATOR))
        status = NATIVE_RENDER_BASELINE_OT_INVALID;

    state.ot_open = 0;
    state.current_ot_nodes = 0;
    state.expected_ot_node = 0;
    if (!add_u64(&baseline.ot_lists, 1u)) {
        fail_observation(NATIVE_RENDER_BASELINE_OVERFLOW);
        return;
    }
    baseline.ot_digest = mix(baseline.ot_digest, UINT64_C(0x4f54454e));
    baseline.ot_digest = mix(baseline.ot_digest, status);
    baseline.topology_digest = baseline.ot_digest;
    if (status == NATIVE_RENDER_BASELINE_OT_INVALID)
        fail_observation(NATIVE_RENDER_BASELINE_INVALID_OT);
    else if (status == NATIVE_RENDER_BASELINE_OT_CYCLIC)
        fail_observation(NATIVE_RENDER_BASELINE_CYCLIC_OT);
    else if (status != NATIVE_RENDER_BASELINE_OT_VALID)
        fail_observation(NATIVE_RENDER_BASELINE_INVALID_OT);
    else
        baseline.field_completeness_mask |=
            NATIVE_RENDER_BASELINE_FIELD_OT_DIGEST;
}

void native_render_baseline_observe_vblank_impl(void) {
    NativeRenderBaselineReason reason;

    if (!g_native_render_baseline_armed) return;
    if (baseline.vblank_delta >= state.max_vblanks) {
        fail_observation(NATIVE_RENDER_BASELINE_OVERFLOW);
        return;
    }
    reason = native_render_baseline_runtime_observe(&baseline);
    if (reason != NATIVE_RENDER_BASELINE_COMPLETE)
        fail_observation(reason);
    else if (state.auto_finalize_vblanks != 0u &&
             baseline.vblank_delta == state.auto_finalize_vblanks)
        (void)native_render_baseline_finalize();
}

int native_render_baseline_finalize(void) {
    NativeRenderBaselineReason reason;

    if (!baseline.enabled || state.failed) return 0;
    if (state.ot_open) {
        fail_observation(NATIVE_RENDER_BASELINE_INVALID_OT);
        return 0;
    }
    if (baseline.interpreter_calls == 0u && baseline.native_calls == 0u) {
        fail_observation(NATIVE_RENDER_BASELINE_PRODUCER_ABSENT);
        return 0;
    }
    if (baseline.ot_lists == 0u || baseline.vblank_delta == 0u ||
        baseline.display_samples == 0u) {
        fail_observation(NATIVE_RENDER_BASELINE_INCOMPLETE_OBSERVATION);
        return 0;
    }
    if (baseline.camera_actor_digest == 0u) {
        fail_observation(NATIVE_RENDER_BASELINE_MISSING_CAMERA_DIGEST);
        return 0;
    }
    if ((baseline.field_completeness_mask &
         NATIVE_RENDER_BASELINE_FIELD_VISUAL_STATE) == 0u) {
        reason = capture_guest_render_state();
        if (reason != NATIVE_RENDER_BASELINE_COMPLETE) {
            fail_observation(reason);
            return 0;
        }
    }
    reason = capture_gte_counts();
    if (reason != NATIVE_RENDER_BASELINE_COMPLETE) {
        fail_observation(reason);
        return 0;
    }
    if (baseline.field_completeness_mask != baseline.required_field_mask) {
        fail_observation(NATIVE_RENDER_BASELINE_INCOMPLETE_FIELDS);
        return 0;
    }

    baseline.cycles_per_vblank =
        baseline.guest_cycle_delta / baseline.vblank_delta;
    baseline.complete = 1;
    baseline.incomplete_reason = NATIVE_RENDER_BASELINE_COMPLETE;
    baseline.normalized_digest = normalized_digest(&baseline);
    state.producer_phys = 0;
    g_native_render_baseline_armed = 0;
    return 1;
}

void native_render_baseline_snapshot(NativeRenderBaselineSnapshot *out) {
    if (out) *out = baseline;
}

#ifdef PSX_NATIVE_RENDER_BASELINE_TEST
void native_render_baseline_test_seed_ot_totals(uint64_t lists, uint64_t nodes,
                                                 uint64_t words) {
    baseline.ot_lists = lists;
    baseline.ot_nodes = nodes;
    baseline.ot_words = words;
}

uint64_t native_render_baseline_test_normalized_digest(
        const NativeRenderBaselineSnapshot *snapshot) {
    return snapshot ? normalized_digest(snapshot) : 0u;
}
#endif
