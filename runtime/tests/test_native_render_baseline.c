#include "audio_trace.h"
#include "gpu.h"
#include "native_render_baseline.h"
#include "spu.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t s_vram[1024u * 512u];
static GpuDisplayInfo s_display;
static AudioTraceStats s_audio;
static SpuDebugInfo s_spu;
static GuestRenderBridgeSnapshot s_bridge;
static GuestRenderCompletedState s_completed;
static uint64_t s_cycles;
static uint64_t s_gp0;
static uint64_t s_gp1;
static uint64_t s_fill;
static uint64_t s_draw;
static uint64_t s_copy;
static uint64_t s_vram_serial;
static int s_vram_serial_overflowed;
static int s_bridge_snapshot_available;
static int s_completed_state_available;

const uint16_t *gpu_get_vram(void) { return s_vram; }
void gpu_get_display_info(GpuDisplayInfo *out) { *out = s_display; }
uint64_t gpu_get_gp0_count(void) { return s_gp0; }
uint64_t gpu_get_gp1_count(void) { return s_gp1; }
uint64_t gpu_render_vram_mutation_serial(void) { return s_vram_serial; }
bool gpu_render_vram_mutation_overflowed(void) {
    return s_vram_serial_overflowed != 0;
}
void gpu_get_gp0_stats(uint64_t *nop, uint64_t *fill, uint64_t *draw,
                       uint64_t *env, uint64_t *copy) {
    *nop = 0;
    *fill = s_fill;
    *draw = s_draw;
    *env = 0;
    *copy = s_copy;
}
void audio_trace_get_stats(AudioTraceStats *out) { *out = s_audio; }
void spu_debug_info(SpuDebugInfo *out) { *out = s_spu; }
uint64_t psx_get_cycle_count(void) { return s_cycles; }
void gl_renderer_sync_cpu(void) {}
void vk_renderer_sync_cpu(void) {}

GuestRenderStatus guest_render_bridge_snapshot(
        GuestRenderBridgeSnapshot *out) {
    if (!s_bridge_snapshot_available || !out) return GUEST_RENDER_NO_COMPLETED_STATE;
    *out = s_bridge;
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_present(GuestRenderCompletedState *out) {
    if (!s_completed_state_available || !out) return GUEST_RENDER_NO_COMPLETED_STATE;
    *out = s_completed;
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_last_completed(
        GuestRenderBridgeSnapshot *out_snapshot,
        GuestRenderCompletedState *out_completed) {
    if (!s_completed_state_available || !out_snapshot || !out_completed)
        return GUEST_RENDER_NO_COMPLETED_STATE;
    *out_snapshot = s_bridge;
    *out_completed = s_completed;
    return GUEST_RENDER_OK;
}

static int check(int condition, const char *message) {
    if (condition) return 1;
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
}

static NativeRenderBaselineConfig valid_config(void) {
    NativeRenderBaselineConfig config;
    memset(&config, 0, sizeof(config));
    config.authenticated_producer_address = UINT32_C(0x80010000);
    config.game_digest = UINT64_C(0x1020304050607080);
    config.max_vblanks = 8u;
    return config;
}

static void reset_fixture(void) {
    memset(s_vram, 0, sizeof(s_vram));
    memset(&s_audio, 0, sizeof(s_audio));
    memset(&s_spu, 0, sizeof(s_spu));
    memset(&s_bridge, 0, sizeof(s_bridge));
    memset(&s_completed, 0, sizeof(s_completed));
    s_display.display_x = 1023u;
    s_display.display_y = 511u;
    s_display.width = 2u;
    s_display.height = 2u;
    s_display.depth24 = 0;
    s_display.disabled = 0;
    s_bridge.modes.requested_render_mode = GUEST_RENDER_RENDER_NATIVE;
    s_bridge.modes.effective_render_mode = GUEST_RENDER_RENDER_SHADOW;
    s_bridge.fallback_reason = GUEST_RENDER_FALLBACK_NONE;
    s_bridge.slot_count = 2u;
    s_bridge.binding_count = 3u;
    s_completed.id.scene_epoch = 7u;
    s_completed.id.state_sequence = 11u;
    s_completed.slot_count = s_bridge.slot_count;
    s_completed.binding_count = s_bridge.binding_count;
    s_bridge_snapshot_available = 1;
    s_completed_state_available = 1;
    s_cycles = s_gp0 = s_gp1 = s_fill = s_draw = s_copy = 0u;
    s_vram_serial = 0u;
    s_vram_serial_overflowed = 0;
    gte_attribution_reset();
    native_render_baseline_reset();
}

static int arm_fixture(void) {
    NativeRenderBaselineConfig config = valid_config();
    reset_fixture();
    return native_render_baseline_arm(&config);
}

static void note_valid_ot(uint32_t second_node) {
    const NativeRenderBaselineOtNode first = {
        UINT32_C(0x1000), second_node, 2u, 0u,
    };
    const NativeRenderBaselineOtNode second = {
        second_node, UINT32_C(0x00ffffff), 3u, 1u,
    };

    native_render_baseline_ot_begin(UINT32_C(0x1000));
    native_render_baseline_ot_node(&first);
    native_render_baseline_ot_node(&second);
    native_render_baseline_ot_end(NATIVE_RENDER_BASELINE_OT_VALID);
}

static void note_gte_fixture(void) {
    const GteAttributionSite outside = {
        UINT32_C(0x80020000), UINT32_C(0x80021000), true, true,
    };
    const GteAttributionSite inside = {
        UINT32_C(0x80022000), UINT32_C(0x80023000), true, true,
    };
    const GteAttributionProducerContext producer = {
        {7u, 11u}, 19u, GTE_ATTRIBUTION_TIER_WARM,
    };

    (void)gte_attribution_set_execution_tier(GTE_ATTRIBUTION_TIER_STATIC);
    (void)gte_attribution_record_execute(&outside);
    (void)gte_attribution_producer_begin(&producer);
    (void)gte_attribution_record_execute(&inside);
    (void)gte_attribution_producer_end();
}

static NativeRenderBaselineMaterialObservation material_observation(
        uint64_t ordinal) {
    NativeRenderBaselineMaterialObservation observation;

    memset(&observation, 0, sizeof(observation));
    observation.material.tpage = 0u;
    observation.material.texture_depth = GPU_RENDER_TEXTURE_4_BIT;
    observation.material.blend_mode = GPU_RENDER_BLEND_AVERAGE;
    observation.material.shading = GPU_RENDER_SHADING_FLAT;
    observation.material.draw_area_right = 1023u;
    observation.material.draw_area_bottom = 511u;
    observation.provenance = NATIVE_RENDER_BASELINE_MATERIAL_OT;
    observation.command_address = UINT32_C(0x1004);
    observation.source_word_ordinal = UINT64_C(0x401);
    observation.container_ordinal = UINT64_C(0x400);
    observation.submission_ordinal = ordinal;
    observation.word_count = 4u;
    return observation;
}

static void note_runtime_fixture(uint16_t mask_bit) {
    s_vram[511u * 1024u + 1023u] = (uint16_t)(0x001fu | mask_bit);
    s_vram[511u * 1024u] = 0x03e0u;
    s_vram[1023u] = 0x7c00u;
    s_vram[0] = 0x7fffu;
    s_gp0 = 9u;
    s_gp1 = 3u;
    s_fill = 1u;
    s_draw = 4u;
    s_copy = 2u;
    s_vram_serial = 7u;
    s_audio.tap_frames[AUDIO_TAP_SPU_OUT] = 735u;
    s_audio.tap_nonzero[AUDIO_TAP_SPU_OUT] = 700u;
    s_audio.tap_audible[AUDIO_TAP_SPU_OUT] = 600u;
    s_audio.events_total = 11u;
    s_spu.key_on_count = 2u;
    s_spu.active_mask = 3u;
    s_cycles = 564480u;
    native_render_baseline_observe_vblank();
}

static NativeRenderBaselineSnapshot run_baseline(
        uint32_t second_node, uint16_t mask_bit, int material, int host,
        const NativeRenderBaselineMaterialObservation *material_override) {
    NativeRenderBaselineSnapshot snapshot;
    NativeRenderBaselineConfig config = valid_config();

    reset_fixture();
    (void)native_render_baseline_arm(&config);
    native_render_baseline_note_camera_actor_digest(
        UINT64_C(0x8877665544332211));
    if (material) {
        const NativeRenderBaselineMaterialObservation default_observation =
            material_observation(0u);
        native_render_baseline_note_material(
            material_override ? material_override : &default_observation);
    }
    if (host)
        native_render_baseline_note_host_framebuffer_digest(
            UINT64_C(0x5555666677778888));
    native_render_baseline_note_execution(
        UINT32_C(0xa0010000), NATIVE_RENDER_BASELINE_NATIVE);
    note_valid_ot(second_node);
    note_gte_fixture();
    note_runtime_fixture(mask_bit);
    (void)native_render_baseline_finalize();
    native_render_baseline_snapshot(&snapshot);
    return snapshot;
}

static int test_disabled_and_invalid_config(void) {
    NativeRenderBaselineSnapshot snapshot;
    NativeRenderBaselineConfig config;
    int ok = 1;

    reset_fixture();
    {
        const NativeRenderBaselineMaterialObservation observation =
            material_observation(0u);
        native_render_baseline_note_material(&observation);
    }
    native_render_baseline_observe_vblank();
    native_render_baseline_snapshot(&snapshot);
    ok &= check(!snapshot.enabled && snapshot.material_samples == 0u &&
                    snapshot.incomplete_reason == NATIVE_RENDER_BASELINE_DISABLED,
                "disabled baseline is inert");

    config = valid_config();
    config.authenticated_producer_address = 0u;
    ok &= check(!native_render_baseline_arm(&config),
                "invalid producer config is rejected");
    config = valid_config();
    config.game_digest = 0u;
    ok &= check(!native_render_baseline_arm(&config),
                "missing game identity is rejected");
    return ok;
}

static int test_complete_contract_and_gte_snapshot(void) {
    const NativeRenderBaselineSnapshot snapshot =
        run_baseline(UINT32_C(0x1100), 0u, 1, 1, NULL);
    int ok = 1;

    ok &= check(snapshot.complete && !snapshot.overflow &&
                    snapshot.schema_version ==
                        NATIVE_RENDER_BASELINE_SCHEMA_VERSION,
                "closed baseline completes");
    ok &= check(snapshot.field_completeness_mask ==
                        NATIVE_RENDER_BASELINE_FIELD_ALL &&
                    snapshot.required_field_mask ==
                        NATIVE_RENDER_BASELINE_FIELD_ALL,
                "all required fields have a closed completeness mask");
    ok &= check(snapshot.visual_state_id.scene_epoch == 7u &&
                    snapshot.visual_state_id.state_sequence == 11u &&
                    snapshot.requested_render_mode ==
                        GUEST_RENDER_RENDER_NATIVE &&
                    snapshot.effective_render_mode ==
                        GUEST_RENDER_RENDER_SHADOW &&
                    snapshot.producer_count == 2u &&
                    snapshot.producer_binding_count == 3u,
                "bridge visual state, modes, and producer counts are captured");
    ok &= check(snapshot.gte_total_count == 2u &&
                    snapshot.gte_inside_producer_count == 1u &&
                    snapshot.gte_outside_producer_count == 1u &&
                    snapshot.gte_tier_counts[GTE_ATTRIBUTION_TIER_STATIC] == 1u &&
                    snapshot.gte_tier_counts[GTE_ATTRIBUTION_TIER_WARM] == 1u &&
                    !snapshot.gte_blocked,
                "GTE aggregate, provenance, and tier deltas are incorporated");
    ok &= check(snapshot.ot_lists == 1u && snapshot.ot_nodes == 2u &&
                    snapshot.ot_words == 5u && snapshot.ot_digest != 0u &&
                    snapshot.material_samples == 1u &&
                    snapshot.host_framebuffer_samples == 1u,
                "OT ordering and explicit digest inputs are captured");
    ok &= check(snapshot.global_vram_mutation_serial == 7u &&
                    snapshot.vram_mutations == 7u && snapshot.vram_digest != 0u &&
                    snapshot.display15_digest != 0u &&
                    snapshot.normalized_digest ==
                        native_render_baseline_test_normalized_digest(&snapshot),
                "VRAM, display15, and normalized digests are complete");
    return ok;
}

static int test_original_route_uses_observed_ot_identity(void) {
    NativeRenderBaselineSnapshot snapshot;
    NativeRenderBaselineConfig config = valid_config();
    int ok = 1;

    reset_fixture();
    s_bridge.modes.requested_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
    s_bridge.modes.effective_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
    s_completed_state_available = 0;
    (void)native_render_baseline_arm(&config);
    native_render_baseline_note_camera_actor_digest(
        UINT64_C(0x8877665544332211));
    {
        const NativeRenderBaselineMaterialObservation observation =
            material_observation(0u);
        native_render_baseline_note_material(&observation);
    }
    native_render_baseline_note_host_framebuffer_digest(
        UINT64_C(0x5555666677778888));
    native_render_baseline_note_execution(
        UINT32_C(0xa0010000), NATIVE_RENDER_BASELINE_INTERPRETER);
    note_valid_ot(UINT32_C(0x1100));
    note_gte_fixture();
    note_runtime_fixture(0u);
    (void)native_render_baseline_finalize();
    native_render_baseline_snapshot(&snapshot);
    ok &= check(snapshot.complete &&
                    snapshot.visual_state_id.scene_epoch == 1u &&
                    snapshot.visual_state_id.state_sequence == 1u &&
                    snapshot.requested_render_mode ==
                        GUEST_RENDER_RENDER_ORIGINAL &&
                    snapshot.effective_render_mode ==
                        GUEST_RENDER_RENDER_ORIGINAL,
                "Original route binds visual identity to the observed OT");
    return ok;
}

static int test_missing_explicit_inputs_fail_closed(void) {
    NativeRenderBaselineSnapshot snapshot =
        run_baseline(UINT32_C(0x1100), 0u, 0, 0, NULL);
    const uint64_t missing =
        NATIVE_RENDER_BASELINE_FIELD_MATERIAL_DIGEST |
        NATIVE_RENDER_BASELINE_FIELD_HOST_FRAMEBUFFER_DIGEST;
    NativeRenderBaselineConfig config = valid_config();
    int ok;

    ok = check(!snapshot.complete &&
                   snapshot.incomplete_reason ==
                       NATIVE_RENDER_BASELINE_INCOMPLETE_FIELDS &&
                   (snapshot.field_completeness_mask & missing) == 0u,
               "missing material/framebuffer evidence fails closed");

    reset_fixture();
    (void)native_render_baseline_arm(&config);
    {
        const NativeRenderBaselineMaterialObservation observation =
            material_observation(0u);
        native_render_baseline_note_material(&observation);
    }
    native_render_baseline_note_host_framebuffer_digest(2u);
    native_render_baseline_note_execution(
        config.authenticated_producer_address, NATIVE_RENDER_BASELINE_NATIVE);
    note_valid_ot(UINT32_C(0x1100));
    note_gte_fixture();
    note_runtime_fixture(0u);
    (void)native_render_baseline_finalize();
    native_render_baseline_snapshot(&snapshot);
    ok &= check(!snapshot.complete &&
                    snapshot.incomplete_reason ==
                        NATIVE_RENDER_BASELINE_MISSING_CAMERA_DIGEST,
                "missing camera/actor evidence fails closed");
    return ok;
}

static int test_host_framebuffer_capture_schedule(void) {
    NativeRenderBaselineConfig config = valid_config();
    int ok = 1;

    reset_fixture();
    (void)native_render_baseline_arm(&config);
    native_render_baseline_set_auto_finalize_vblanks(3u);
    ok &= check(!native_render_baseline_host_framebuffer_capture_due(),
                "framebuffer capture waits for the final frame");
    native_render_baseline_observe_vblank();
    ok &= check(!native_render_baseline_host_framebuffer_capture_due(),
                "framebuffer capture remains deferred before the final frame");
    native_render_baseline_observe_vblank();
    ok &= check(native_render_baseline_host_framebuffer_capture_due(),
                "framebuffer capture is due on the final frame");
    native_render_baseline_note_host_framebuffer_digest(2u);
    ok &= check(!native_render_baseline_host_framebuffer_capture_due(),
                "published framebuffer evidence satisfies the capture");

    reset_fixture();
    (void)native_render_baseline_arm(&config);
    native_render_baseline_set_auto_finalize_vblanks(0u);
    ok &= check(native_render_baseline_host_framebuffer_capture_due(),
                "explicit-finalize baselines capture the current frame");
    return ok;
}

static int test_ot_metadata_and_complete_vram_are_bound(void) {
    const NativeRenderBaselineSnapshot first =
        run_baseline(UINT32_C(0x1100), 0u, 1, 1, NULL);
    const NativeRenderBaselineSnapshot linked_differently =
        run_baseline(UINT32_C(0x1200), 0u, 1, 1, NULL);
    const NativeRenderBaselineSnapshot mask_changed =
        run_baseline(UINT32_C(0x1100), UINT16_C(0x8000), 1, 1, NULL);
    int ok = 1;

    ok &= check(first.ot_digest != linked_differently.ot_digest &&
                    first.normalized_digest !=
                        linked_differently.normalized_digest,
                "OT insertion links and final order affect the baseline");
    ok &= check(first.display15_digest == mask_changed.display15_digest &&
                    first.vram_digest != mask_changed.vram_digest &&
                    first.normalized_digest != mask_changed.normalized_digest,
                "display15 normalizes mask while complete VRAM retains it");
    return ok;
}

static int test_material_digest_is_route_independent(void) {
    NativeRenderBaselineMaterialObservation first_observation =
        material_observation(3u);
    NativeRenderBaselineMaterialObservation second_observation =
        first_observation;
    NativeRenderBaselineSnapshot first;
    NativeRenderBaselineSnapshot second;

    second_observation.provenance = NATIVE_RENDER_BASELINE_MATERIAL_DMA;
    second_observation.command_address += 16u;
    second_observation.source_word_ordinal += 4u;
    second_observation.container_ordinal += 8u;
    second_observation.submission_ordinal += 1u;
    second_observation.word_count += 2u;
    first = run_baseline(
        UINT32_C(0x1100), 0u, 1, 1, &first_observation);
    second = run_baseline(
        UINT32_C(0x1100), 0u, 1, 1, &second_observation);
    return check(first.material_samples == second.material_samples &&
                     first.material_digest == second.material_digest,
                 "material digest is independent of submission route");
}

static int test_normalized_digest_binds_contract_fields(void) {
    const NativeRenderBaselineSnapshot original =
        run_baseline(UINT32_C(0x1100), 0u, 1, 1, NULL);
    NativeRenderBaselineSnapshot changed;
    const uint64_t digest = original.normalized_digest;
    int ok = 1;

#define CHECK_BOUND(change, message) do { \
    changed = original; \
    change; \
    ok &= check(native_render_baseline_test_normalized_digest(&changed) != digest, \
                message); \
} while (0)
    CHECK_BOUND(changed.visual_state_id.state_sequence++,
                "visual state is digest-bound");
    CHECK_BOUND(changed.requested_render_mode = GUEST_RENDER_RENDER_ORIGINAL,
                "requested mode is digest-bound");
    CHECK_BOUND(changed.producer_count++, "producer counts are digest-bound");
    CHECK_BOUND(changed.gte_total_count++, "GTE counts are digest-bound");
    CHECK_BOUND(changed.ot_digest++, "OT digest is digest-bound");
    CHECK_BOUND(changed.material_digest++, "material digest is digest-bound");
    CHECK_BOUND(changed.global_vram_mutation_serial++,
                "global VRAM serial is digest-bound");
    CHECK_BOUND(changed.vram_digest++, "complete VRAM digest is digest-bound");
    CHECK_BOUND(changed.display15_digest++, "display15 digest is digest-bound");
    CHECK_BOUND(changed.host_framebuffer_digest++,
                "host framebuffer digest is digest-bound");
    CHECK_BOUND(changed.field_completeness_mask ^= 1u,
                "completeness mask is digest-bound");
#undef CHECK_BOUND
    return ok;
}

static int test_overflow_paths_fail_closed(void) {
    NativeRenderBaselineSnapshot snapshot;
    NativeRenderBaselineConfig config = valid_config();
    GteAttributionSite site = {
        UINT32_C(0x80030000), 0u, true, false,
    };
    int ok = 1;

    reset_fixture();
    (void)native_render_baseline_arm(&config);
    {
        const NativeRenderBaselineMaterialObservation observation =
            material_observation(0u);
        native_render_baseline_note_material(&observation);
    }
    native_render_baseline_note_host_framebuffer_digest(2u);
    native_render_baseline_note_camera_actor_digest(3u);
    native_render_baseline_note_execution(
        config.authenticated_producer_address, NATIVE_RENDER_BASELINE_NATIVE);
    note_valid_ot(UINT32_C(0x1100));
    s_vram_serial_overflowed = 1;
    native_render_baseline_observe_vblank();
    native_render_baseline_snapshot(&snapshot);
    ok &= check(snapshot.overflow &&
                    snapshot.incomplete_reason ==
                        NATIVE_RENDER_BASELINE_VRAM_SERIAL_OVERFLOW,
                "global VRAM serial overflow fails closed");

    reset_fixture();
    (void)native_render_baseline_arm(&config);
    {
        const NativeRenderBaselineMaterialObservation observation =
            material_observation(0u);
        native_render_baseline_note_material(&observation);
    }
    native_render_baseline_note_host_framebuffer_digest(2u);
    native_render_baseline_note_camera_actor_digest(3u);
    native_render_baseline_note_execution(
        config.authenticated_producer_address, NATIVE_RENDER_BASELINE_NATIVE);
    note_valid_ot(UINT32_C(0x1100));
    (void)gte_attribution_set_execution_tier(GTE_ATTRIBUTION_TIER_STATIC);
    for (unsigned index = 0u; index < 6u; ++index)
        (void)gte_attribution_record_execute(&site);
    note_runtime_fixture(0u);
    (void)native_render_baseline_finalize();
    native_render_baseline_snapshot(&snapshot);
    ok &= check(snapshot.overflow && snapshot.gte_blocked &&
                    snapshot.gte_overflow_reason ==
                        GTE_ATTRIBUTION_OVERFLOW_COUNTER &&
                    snapshot.incomplete_reason ==
                        NATIVE_RENDER_BASELINE_GTE_OVERFLOW,
                "GTE attribution overflow fails closed");
    return ok;
}

static int test_ot_validation_and_capacity(void) {
    NativeRenderBaselineSnapshot snapshot;
    NativeRenderBaselineConfig config = valid_config();
    int ok = 1;

    reset_fixture();
    (void)native_render_baseline_arm(&config);
    native_render_baseline_ot_begin(UINT32_C(0x1000));
    {
        const NativeRenderBaselineOtNode wrong = {
            UINT32_C(0x1100), UINT32_C(0x00ffffff), 1u, 0u,
        };
        native_render_baseline_ot_node(&wrong);
    }
    native_render_baseline_snapshot(&snapshot);
    ok &= check(snapshot.invalid_ot &&
                    snapshot.incomplete_reason ==
                        NATIVE_RENDER_BASELINE_INVALID_OT,
                "broken OT insertion link fails closed");

    reset_fixture();
    (void)native_render_baseline_arm(&config);
    native_render_baseline_test_seed_ot_totals(0u, UINT64_MAX, 0u);
    native_render_baseline_ot_begin(UINT32_C(0x1000));
    {
        const NativeRenderBaselineOtNode node = {
            UINT32_C(0x1000), UINT32_C(0x00ffffff), 1u, 0u,
        };
        native_render_baseline_ot_node(&node);
    }
    native_render_baseline_snapshot(&snapshot);
    ok &= check(snapshot.overflow && snapshot.ot_nodes == UINT64_MAX,
                "OT counter overflow is atomic and fail-closed");
    return ok;
}

int main(void) {
    int ok = 1;

    ok &= test_disabled_and_invalid_config();
    ok &= test_complete_contract_and_gte_snapshot();
    ok &= test_original_route_uses_observed_ot_identity();
    ok &= test_missing_explicit_inputs_fail_closed();
    ok &= test_host_framebuffer_capture_schedule();
    ok &= test_ot_metadata_and_complete_vram_are_bound();
    ok &= test_material_digest_is_route_independent();
    ok &= test_normalized_digest_binds_contract_fields();
    ok &= test_overflow_paths_fail_closed();
    ok &= test_ot_validation_and_capacity();
    return ok ? 0 : 1;
}
