#include "guest_render_bridge.h"

#include <SDL.h>

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

#ifndef GUEST_RENDER_BRIDGE_INITIAL_CLAIM_ROUNDS
#define GUEST_RENDER_BRIDGE_INITIAL_CLAIM_ROUNDS 2000u
#endif

static GuestRenderSceneConfig scene_config(int timing_mode, int render_mode) {
    GuestRenderSceneConfig config = { 0 };

    config.timing_mode = timing_mode;
    config.render_mode = render_mode;
    return config;
}

static GuestRenderProducerProvenance native_provenance(void) {
    GuestRenderProducerProvenance provenance = { 0 };

    provenance.tier = GUEST_RENDER_PRODUCER_NATIVE;
    return provenance;
}

static int start_scene(const GuestRenderSceneConfig *config,
                       GuestRenderVisualStateId *out_id) {
    return guest_render_bridge_begin_scene(config) == GUEST_RENDER_OK &&
           guest_render_bridge_begin_state(out_id) == GUEST_RENDER_OK;
}

static int read_snapshot(GuestRenderBridgeSnapshot *out_snapshot) {
    return guest_render_bridge_snapshot(out_snapshot) == GUEST_RENDER_OK;
}

static int test_singleton_owner_and_first_id(void) {
    const void *owner;
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_ORIGINAL,
                                                 GUEST_RENDER_RENDER_ORIGINAL);
    GuestRenderVisualStateId id = { 0 };

    guest_render_bridge_test_reset();
    owner = guest_render_bridge_process_owner();
    CHECK(owner != NULL);
    CHECK(guest_render_bridge_process_owner() == owner);
    CHECK(start_scene(&config, &id));
    CHECK(id.scene_epoch == 1u);
    CHECK(id.state_sequence == 0u);
    CHECK(guest_render_bridge_id_equal(id, id));
    CHECK(guest_render_bridge_finalize_state(id, NULL) ==
          GUEST_RENDER_INVALID_ARGUMENT);
    return 1;
}

static int test_scene_state_uniqueness_and_repeated_present(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_NATIVE_59_94,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderProducerProvenance provenance = native_provenance();
    GuestRenderVisualStateId first = { 0 };
    GuestRenderVisualStateId second = { 0 };
    GuestRenderVisualStateId next_scene = { 0 };
    GuestRenderProducerHandle first_handle = { 0 };
    GuestRenderProducerSlot first_slot = { 0 };
    GuestRenderProducerSlot fetched_slot = { 0 };
    GuestRenderCompletedState completed = { 0 };
    GuestRenderCompletedState present_once = { 0 };
    GuestRenderCompletedState present_twice = { 0 };

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &first));
    CHECK(first.scene_epoch == 1u && first.state_sequence == 0u);
    CHECK(guest_render_bridge_producer_begin(first, &provenance, &first_handle) ==
          GUEST_RENDER_OK);
    CHECK(first_handle.state_id.scene_epoch == first.scene_epoch);
    CHECK(first_handle.state_id.state_sequence == first.state_sequence);
    CHECK(first_handle.slot_index == 0u);
    provenance.tier = GUEST_RENDER_PRODUCER_SHADOW;
    CHECK(guest_render_bridge_producer_end(first_handle, &first_slot) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_id_equal(first_slot.handle.state_id, first));
    CHECK(first_slot.handle.slot_index == first_handle.slot_index);
    CHECK(first_slot.provenance.tier == GUEST_RENDER_PRODUCER_NATIVE);
    CHECK(guest_render_bridge_get_slot(first_handle, &fetched_slot) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_id_equal(fetched_slot.handle.state_id, first));
    CHECK(fetched_slot.handle.slot_index == first_handle.slot_index);
    CHECK(fetched_slot.provenance.tier == GUEST_RENDER_PRODUCER_NATIVE);
    CHECK(guest_render_bridge_finalize_state(first, &completed) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_id_equal(completed.id, first));
    CHECK(completed.slot_count == 1u);
    CHECK(guest_render_bridge_present(&present_once) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_present(&present_twice) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_id_equal(present_once.id, first));
    CHECK(guest_render_bridge_id_equal(present_twice.id, first));
    CHECK(present_once.slot_count == 1u && present_twice.slot_count == 1u);

    CHECK(guest_render_bridge_begin_state(&second) == GUEST_RENDER_OK);
    CHECK(!guest_render_bridge_id_equal(first, second));
    CHECK(second.scene_epoch == first.scene_epoch);
    CHECK(second.state_sequence == first.state_sequence + 1u);
    CHECK(guest_render_bridge_finalize_state(second, &completed) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_begin_state(&next_scene) == GUEST_RENDER_OK);
    CHECK(next_scene.scene_epoch == first.scene_epoch + 1u);
    CHECK(next_scene.state_sequence == 0u);
    CHECK(!guest_render_bridge_id_equal(first, next_scene));
    CHECK(!guest_render_bridge_id_equal(second, next_scene));
    CHECK(guest_render_bridge_finalize_state(next_scene, &completed) ==
          GUEST_RENDER_OK);
    return 1;
}

static int test_modes_are_copied_and_effective_modes_fall_back_independently(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_NATIVE_59_94,
                                                 GUEST_RENDER_RENDER_SHADOW);
    GuestRenderBridgeSnapshot snapshot = { 0 };
    GuestRenderModes modes = { 0 };

    guest_render_bridge_test_reset();
    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    config.timing_mode = GUEST_RENDER_TIMING_ORIGINAL;
    config.render_mode = GUEST_RENDER_RENDER_NATIVE;
    CHECK(read_snapshot(&snapshot));
    modes = snapshot.modes;
    CHECK(modes.requested_timing_mode == GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(modes.effective_timing_mode == GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(modes.requested_render_mode == GUEST_RENDER_RENDER_SHADOW);
    CHECK(modes.effective_render_mode == GUEST_RENDER_RENDER_SHADOW);
    guest_render_bridge_force_original(GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.modes.requested_timing_mode ==
          GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.modes.requested_render_mode == GUEST_RENDER_RENDER_SHADOW);
    CHECK(snapshot.modes.effective_timing_mode ==
          GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_ORIGINAL);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    CHECK(snapshot.last_fallback_reason ==
           GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    CHECK(snapshot.fallback_count == 1u);
    CHECK(snapshot.scene_fallback_count_baseline == 0u);
    CHECK(snapshot.scene_fallback_count_delta == 1u);
    CHECK(!snapshot.fallback_count_overflowed);

    guest_render_bridge_test_reset();
    config = scene_config(GUEST_RENDER_TIMING_ORIGINAL, GUEST_RENDER_RENDER_NATIVE);
    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.modes.requested_timing_mode == GUEST_RENDER_TIMING_ORIGINAL);
    CHECK(snapshot.modes.effective_timing_mode == GUEST_RENDER_TIMING_ORIGINAL);
    CHECK(snapshot.modes.requested_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    guest_render_bridge_force_original(GUEST_RENDER_FALLBACK_BACKEND_FAILURE);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.modes.requested_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_BACKEND_FAILURE);
    return 1;
}

static int test_fallback_telemetry_tracks_scene_and_lifetime_state(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_NATIVE_59_94,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderBridgeSnapshot snapshot = { 0 };

    guest_render_bridge_test_reset();
    guest_render_bridge_test_set_fallback_count_limit(2u);
    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    guest_render_bridge_force_original(GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.fallback_count == 1u);
    CHECK(snapshot.scene_fallback_count_baseline == 0u);
    CHECK(snapshot.scene_fallback_count_delta == 1u);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    CHECK(snapshot.last_fallback_reason ==
           GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    CHECK(!snapshot.fallback_count_overflowed);

    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_count == 1u);
    CHECK(snapshot.scene_fallback_count_baseline == 1u);
    CHECK(snapshot.scene_fallback_count_delta == 0u);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_NONE);
    CHECK(snapshot.last_fallback_reason ==
           GUEST_RENDER_FALLBACK_PRESENTATION_GATE);

    guest_render_bridge_force_original(GUEST_RENDER_FALLBACK_BACKEND_FAILURE);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.fallback_count == 2u);
    CHECK(snapshot.scene_fallback_count_baseline == 1u);
    CHECK(snapshot.scene_fallback_count_delta == 1u);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_BACKEND_FAILURE);
    CHECK(snapshot.last_fallback_reason ==
          GUEST_RENDER_FALLBACK_BACKEND_FAILURE);

    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    guest_render_bridge_force_original(GUEST_RENDER_FALLBACK_INVALID_ARGUMENT);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_count == 2u);
    CHECK(snapshot.scene_fallback_count_baseline == 2u);
    CHECK(snapshot.scene_fallback_count_delta == 0u);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED);
    CHECK(snapshot.last_fallback_reason ==
          GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED);
    CHECK(snapshot.fallback_count_overflowed);
    CHECK(guest_render_bridge_begin_scene(&config) ==
          GUEST_RENDER_COUNTER_EXHAUSTED);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_count_overflowed);
    return 1;
}

static int test_sequential_producers_no_nesting_and_finalize_order(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_ORIGINAL,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderProducerProvenance provenance = native_provenance();
    GuestRenderVisualStateId id = { 0 };
    GuestRenderProducerHandle first = { 0 };
    GuestRenderProducerHandle second = { 0 };
    GuestRenderProducerSlot slot = { 0 };
    GuestRenderCompletedState completed = { 0 };
    GuestRenderBridgeSnapshot snapshot = { 0 };

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &first) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_producer_end(first, &slot) == GUEST_RENDER_OK);
    CHECK(slot.handle.slot_index == 0u);
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &second) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_producer_end(second, &slot) == GUEST_RENDER_OK);
    CHECK(slot.handle.slot_index == 1u);
    CHECK(guest_render_bridge_finalize_state(id, &completed) == GUEST_RENDER_OK);
    CHECK(completed.slot_count == 2u);

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &first) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &second) ==
          GUEST_RENDER_INVALID_TRANSITION);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.producer_open);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_NESTED_PRODUCER);

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &first) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_finalize_state(id, &completed) ==
          GUEST_RENDER_INVALID_TRANSITION);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.producer_open);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_ACTIVE_PRODUCER);
    return 1;
}

static int test_packet_bindings_span_lookup_and_repeated_present(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_NATIVE_59_94,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderProducerProvenance provenance = native_provenance();
    GuestRenderVisualStateId id = { 0 };
    GuestRenderProducerHandle first_handle = { 0 };
    GuestRenderProducerHandle second_handle = { 0 };
    GuestRenderProducerSlot first_slot = { 0 };
    GuestRenderProducerSlot second_slot = { 0 };
    GuestRenderPacketBinding binding = { 0 };
    GuestRenderCompletedState completed = { 0 };
    GuestRenderCompletedState first_present = { 0 };
    GuestRenderCompletedState second_present = { 0 };
    GuestRenderBridgeSnapshot snapshot = { 0 };

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &first_handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(first_handle, UINT32_C(0x80000100), 7u) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(first_handle, UINT32_C(0xa0000104), 8u) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_producer_end(first_handle, &first_slot) ==
          GUEST_RENDER_OK);
    CHECK(first_slot.binding_start == 0u);
    CHECK(first_slot.binding_count == 2u);

    provenance.tier = GUEST_RENDER_PRODUCER_SHADOW;
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &second_handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(second_handle, UINT32_C(0x00000108), 7u) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_producer_end(second_handle, &second_slot) ==
          GUEST_RENDER_OK);
    CHECK(second_slot.binding_start == 2u);
    CHECK(second_slot.binding_count == 1u);
    CHECK(guest_render_bridge_finalize_state(id, &completed) == GUEST_RENDER_OK);
    CHECK(completed.slot_count == 2u);
    CHECK(completed.binding_count == 3u);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.binding_count == 3u);

    CHECK(guest_render_bridge_get_completed_binding(completed, 0u, &binding) ==
          GUEST_RENDER_OK);
    CHECK(binding.packet_address == UINT32_C(0x00000100));
    CHECK(binding.source_primitive_index == 7u);
    CHECK(binding.handle.slot_index == first_handle.slot_index);
    CHECK(guest_render_bridge_get_completed_binding(completed, 1u, &binding) ==
          GUEST_RENDER_OK);
    CHECK(binding.packet_address == UINT32_C(0x00000104));
    CHECK(binding.source_primitive_index == 8u);
    CHECK(guest_render_bridge_get_completed_binding(completed, 2u, &binding) ==
          GUEST_RENDER_OK);
    CHECK(binding.packet_address == UINT32_C(0x00000108));
    CHECK(binding.source_primitive_index == 7u);
    CHECK(binding.handle.slot_index == second_handle.slot_index);
    CHECK(guest_render_bridge_get_completed_binding(completed, 3u, &binding) ==
          GUEST_RENDER_BINDING_NOT_FOUND);
    CHECK(guest_render_bridge_find_completed_binding(
              completed, UINT32_C(0xa0000100), &binding) == GUEST_RENDER_OK);
    CHECK(binding.packet_address == UINT32_C(0x00000100));
    CHECK(guest_render_bridge_find_completed_binding(
              completed, UINT32_C(0x0000010c), &binding) ==
          GUEST_RENDER_BINDING_NOT_FOUND);

    CHECK(guest_render_bridge_present(&first_present) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_find_completed_binding(
              first_present, UINT32_C(0x80000104), &binding) == GUEST_RENDER_OK);
    CHECK(binding.source_primitive_index == 8u);
    CHECK(guest_render_bridge_present(&second_present) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_get_completed_binding(second_present, 2u, &binding) ==
          GUEST_RENDER_OK);
    CHECK(binding.handle.slot_index == second_handle.slot_index);
    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_find_completed_binding(
              second_present, UINT32_C(0x00000108), &binding) ==
          GUEST_RENDER_NO_COMPLETED_STATE);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.binding_count == 0u);
    return 1;
}

static int test_packet_binding_duplicates_fail_closed(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_NATIVE_59_94,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderProducerProvenance provenance = native_provenance();
    GuestRenderVisualStateId id = { 0 };
    GuestRenderProducerHandle handle = { 0 };
    GuestRenderBridgeSnapshot snapshot = { 0 };

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(handle, UINT32_C(0x80000200), 0u) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(handle, UINT32_C(0xa0000200), 1u) ==
          GUEST_RENDER_DUPLICATE_PACKET_ADDRESS);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.binding_count == 0u);
    CHECK(snapshot.modes.effective_timing_mode == GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason ==
          GUEST_RENDER_FALLBACK_DUPLICATE_PACKET_ADDRESS);

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(handle, UINT32_C(0x00000200), 3u) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(handle, UINT32_C(0x00000204), 3u) ==
          GUEST_RENDER_DUPLICATE_PRIMITIVE_INDEX);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.binding_count == 0u);
    CHECK(snapshot.modes.effective_timing_mode == GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason ==
          GUEST_RENDER_FALLBACK_DUPLICATE_PRIMITIVE_INDEX);

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(handle, UINT32_C(0x80000202), 0u) ==
          GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.binding_count == 0u);
    CHECK(snapshot.modes.effective_timing_mode == GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_INVALID_PACKET_ADDRESS);
    return 1;
}

static int test_packet_binding_capacity_and_stale_handles_fail_closed(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_NATIVE_59_94,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderProducerProvenance provenance = native_provenance();
    GuestRenderVisualStateId id = { 0 };
    GuestRenderProducerHandle handle = { 0 };
    GuestRenderProducerHandle stale = { 0 };
    GuestRenderProducerSlot slot = { 0 };
    GuestRenderBridgeSnapshot snapshot = { 0 };
    const size_t capacity = guest_render_bridge_binding_capacity();

    guest_render_bridge_test_reset();
    CHECK(capacity == GUEST_RENDER_BRIDGE_TEST_BINDING_CAPACITY);
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    for (size_t index = 0u; index < capacity; ++index) {
        CHECK(guest_render_bridge_bind_packet(
                  handle, UINT32_C(0x00000400) + (uint32_t)(index * 4u),
                  (uint32_t)index) == GUEST_RENDER_OK);
    }
    CHECK(guest_render_bridge_bind_packet(
              handle, UINT32_C(0x00000400) + (uint32_t)(capacity * 4u),
              (uint32_t)capacity) == GUEST_RENDER_BINDING_CAPACITY_EXCEEDED);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.binding_count == 0u);
    CHECK(snapshot.modes.effective_timing_mode == GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_BINDING_CAPACITY);

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(handle, UINT32_C(0x00000500), 0u) ==
          GUEST_RENDER_OK);
    stale = handle;
    stale.slot_index += 1u;
    CHECK(guest_render_bridge_bind_packet(stale, UINT32_C(0x00000504), 1u) ==
          GUEST_RENDER_STALE_HANDLE);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.binding_count == 0u);
    CHECK(snapshot.modes.effective_timing_mode == GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_STALE_HANDLE);

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(handle, UINT32_C(0x00000600), 0u) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_producer_end(handle, &slot) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(handle, UINT32_C(0x00000604), 1u) ==
          GUEST_RENDER_STALE_HANDLE);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.binding_count == 0u);
    CHECK(snapshot.modes.effective_timing_mode == GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_STALE_HANDLE);
    return 1;
}

static int test_scene_reset_clears_completed_state_and_open_state_falls_back(void) {
    GuestRenderSceneConfig native_config = scene_config(GUEST_RENDER_TIMING_NATIVE_59_94,
                                                        GUEST_RENDER_RENDER_NATIVE);
    GuestRenderSceneConfig original_config = scene_config(GUEST_RENDER_TIMING_ORIGINAL,
                                                          GUEST_RENDER_RENDER_ORIGINAL);
    GuestRenderVisualStateId id = { 0 };
    GuestRenderVisualStateId after_reset = { 0 };
    GuestRenderCompletedState completed = { 0 };
    GuestRenderBridgeSnapshot snapshot = { 0 };

    guest_render_bridge_test_reset();
    CHECK(start_scene(&native_config, &id));
    CHECK(guest_render_bridge_finalize_state(id, &completed) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_begin_scene(&original_config) == GUEST_RENDER_OK);
    CHECK(read_snapshot(&snapshot));
    CHECK(!snapshot.state_open);
    CHECK(!snapshot.producer_open);
    CHECK(snapshot.slot_count == 0u);
    CHECK(guest_render_bridge_present(&completed) ==
          GUEST_RENDER_NO_COMPLETED_STATE);
    CHECK(snapshot.modes.requested_timing_mode == GUEST_RENDER_TIMING_ORIGINAL);
    CHECK(snapshot.modes.requested_render_mode == GUEST_RENDER_RENDER_ORIGINAL);

    guest_render_bridge_test_reset();
    CHECK(start_scene(&native_config, &id));
    CHECK(guest_render_bridge_begin_scene(&native_config) == GUEST_RENDER_OK);
    CHECK(read_snapshot(&snapshot));
    CHECK(!snapshot.state_open);
    CHECK(snapshot.slot_count == 0u);
    CHECK(snapshot.modes.effective_timing_mode ==
          GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_SCENE_RESET);
    guest_render_bridge_force_original(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_SCENE_RESET);
    CHECK(guest_render_bridge_begin_state(&after_reset) == GUEST_RENDER_OK);
    CHECK(after_reset.scene_epoch == id.scene_epoch + 1u);
    CHECK(after_reset.state_sequence == 0u);
    CHECK(guest_render_bridge_finalize_state(after_reset, &completed) ==
          GUEST_RENDER_OK);
    return 1;
}

static int test_scene_reset_and_abort_clear_packet_bindings(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_NATIVE_59_94,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderProducerProvenance provenance = native_provenance();
    GuestRenderVisualStateId id = { 0 };
    GuestRenderProducerHandle handle = { 0 };
    GuestRenderProducerSlot slot = { 0 };
    GuestRenderPacketBinding binding = { 0 };
    GuestRenderCompletedState completed = { 0 };
    GuestRenderBridgeSnapshot snapshot = { 0 };

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(handle, UINT32_C(0x00000700), 0u) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.binding_count == 0u);
    CHECK(snapshot.modes.effective_timing_mode ==
          GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_SCENE_RESET);

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(handle, UINT32_C(0x00000704), 0u) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_producer_end(handle, &slot) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_finalize_state(id, &completed) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_get_completed_binding(completed, 0u, &binding) ==
          GUEST_RENDER_OK);
    guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.binding_count == 0u);
    CHECK(snapshot.modes.effective_timing_mode ==
          GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_NONE);
    CHECK(guest_render_bridge_get_completed_binding(completed, 0u, &binding) ==
          GUEST_RENDER_NO_COMPLETED_STATE);
    return 1;
}

static int test_abort_scene_clears_all_state_and_requires_new_scene(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_NATIVE_59_94,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderProducerProvenance provenance = native_provenance();
    GuestRenderVisualStateId id = { 0 };
    GuestRenderProducerHandle handle = { 0 };
    GuestRenderCompletedState completed = { 0 };
    GuestRenderBridgeSnapshot snapshot = { 0 };
    const void *owner;

    guest_render_bridge_test_reset();
    owner = guest_render_bridge_process_owner();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
    CHECK(guest_render_bridge_process_owner() == owner);
    CHECK(read_snapshot(&snapshot));
    CHECK(!snapshot.state_open);
    CHECK(!snapshot.producer_open);
    CHECK(snapshot.slot_count == 0u);
    CHECK(snapshot.modes.effective_timing_mode ==
          GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_NONE);
    CHECK(guest_render_bridge_present(&completed) ==
          GUEST_RENDER_NO_COMPLETED_STATE);
    CHECK(guest_render_bridge_begin_state(&id) ==
          GUEST_RENDER_INVALID_TRANSITION);
    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_begin_state(&id) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_finalize_state(id, &completed) == GUEST_RENDER_OK);
    guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_SCENE_RESET);
    CHECK(guest_render_bridge_present(&completed) ==
          GUEST_RENDER_NO_COMPLETED_STATE);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    guest_render_bridge_reset_scene();
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    return 1;
}

static int test_slot_capacity_clears_native_slots(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_ORIGINAL,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderProducerProvenance provenance = native_provenance();
    GuestRenderVisualStateId id = { 0 };
    GuestRenderProducerHandle handle = { 0 };
    GuestRenderProducerSlot slot = { 0 };
    GuestRenderBridgeSnapshot snapshot = { 0 };
    const size_t capacity = guest_render_bridge_slot_capacity();

    guest_render_bridge_test_reset();
    CHECK(capacity > 0u);
    CHECK(start_scene(&config, &id));
    for (size_t index = 0u; index < capacity; ++index) {
        CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
              GUEST_RENDER_OK);
        CHECK(handle.slot_index == index);
        CHECK(guest_render_bridge_producer_end(handle, &slot) == GUEST_RENDER_OK);
    }
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_SLOT_CAPACITY_EXCEEDED);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.slot_count == 0u);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_SLOT_CAPACITY);
    return 1;
}

static int test_wrong_id_stale_handle_and_invalid_provenance_fail_closed(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_ORIGINAL,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderProducerProvenance provenance = native_provenance();
    GuestRenderVisualStateId id = { 0 };
    GuestRenderVisualStateId wrong_id = { 0 };
    GuestRenderVisualStateId next_id = { 0 };
    GuestRenderProducerHandle handle = { 0 };
    GuestRenderProducerSlot slot = { 0 };
    GuestRenderBridgeSnapshot snapshot = { 0 };
    GuestRenderCompletedState completed = { 0 };

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    wrong_id = id;
    wrong_id.state_sequence++;
    CHECK(guest_render_bridge_producer_begin(wrong_id, &provenance, &handle) ==
          GUEST_RENDER_INVALID_TRANSITION);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.slot_count == 0u);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_WRONG_STATE);

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_producer_end(handle, &slot) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_finalize_state(id, &completed) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_begin_state(&next_id) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_get_slot(handle, &slot) == GUEST_RENDER_STALE_HANDLE);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.slot_count == 0u);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_STALE_HANDLE);

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    provenance.tier = 99;
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_INVALID_PROVENANCE);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.slot_count == 0u);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_INVALID_PROVENANCE);

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    provenance = native_provenance();
    provenance.reserved[0] = 1u;
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_INVALID_PROVENANCE);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.slot_count == 0u);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_INVALID_PROVENANCE);
    return 1;
}

static int test_counter_exhaustion_never_wraps(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_ORIGINAL,
                                                 GUEST_RENDER_RENDER_ORIGINAL);
    GuestRenderVisualStateId first = { 0 };
    GuestRenderVisualStateId second = { 0 };
    GuestRenderCompletedState completed = { 0 };
    GuestRenderBridgeSnapshot snapshot = { 0 };

    guest_render_bridge_test_reset();
    guest_render_bridge_test_set_counter_limits(1u, 1u);
    CHECK(start_scene(&config, &first));
    CHECK(first.scene_epoch == 1u && first.state_sequence == 0u);
    CHECK(guest_render_bridge_finalize_state(first, &completed) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_begin_state(&second) == GUEST_RENDER_OK);
    CHECK(second.scene_epoch == 1u && second.state_sequence == 1u);
    CHECK(guest_render_bridge_finalize_state(second, &completed) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_begin_state(&second) ==
          GUEST_RENDER_COUNTER_EXHAUSTED);
    CHECK(read_snapshot(&snapshot));
    CHECK(!snapshot.state_open);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_ORIGINAL);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED);

    guest_render_bridge_test_reset();
    guest_render_bridge_test_set_counter_limits(1u, 0u);
    CHECK(start_scene(&config, &first));
    CHECK(guest_render_bridge_finalize_state(first, &completed) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_begin_scene(&config) ==
          GUEST_RENDER_COUNTER_EXHAUSTED);
    CHECK(read_snapshot(&snapshot));
    CHECK(!snapshot.state_open);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED);
    return 1;
}

static int test_null_arguments_are_rejected(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_ORIGINAL,
                                                 GUEST_RENDER_RENDER_ORIGINAL);
    GuestRenderProducerProvenance provenance = native_provenance();
    GuestRenderVisualStateId id = { 0 };
    GuestRenderProducerHandle handle = { 0 };
    GuestRenderProducerSlot slot = { 0 };
    GuestRenderPacketBinding binding = { 0 };
    GuestRenderCompletedState completed = { 0 };

    guest_render_bridge_test_reset();
    CHECK(guest_render_bridge_begin_scene(NULL) == GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_begin_state(NULL) == GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(guest_render_bridge_begin_state(&id) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_producer_begin(id, NULL, &handle) ==
          GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(guest_render_bridge_producer_begin(id, &provenance, NULL) ==
          GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_producer_end(handle, NULL) ==
          GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(guest_render_bridge_producer_end(handle, &slot) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_finalize_state(id, NULL) ==
          GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(guest_render_bridge_finalize_state(id, &completed) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_present(NULL) == GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(guest_render_bridge_snapshot(NULL) == GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(guest_render_bridge_get_slot(handle, NULL) ==
          GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(guest_render_bridge_get_completed_binding(completed, 0u, NULL) ==
          GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(guest_render_bridge_find_completed_binding(
              completed, UINT32_C(0x00000100), NULL) ==
          GUEST_RENDER_INVALID_ARGUMENT);
    CHECK(guest_render_bridge_find_completed_binding(
              completed, UINT32_C(0x00000102), &binding) ==
          GUEST_RENDER_INVALID_ARGUMENT);
    return 1;
}

typedef struct WrongThreadCall {
    int result;
    GuestRenderVisualStateId id;
} WrongThreadCall;

static int wrong_thread_begin_state(void *userdata) {
    WrongThreadCall *call = userdata;

    call->result = guest_render_bridge_begin_state(&call->id);
    return 0;
}

static int test_wrong_thread_poison_does_not_create_a_state(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_NATIVE_59_94,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderBridgeSnapshot before = { 0 };
    GuestRenderBridgeSnapshot after = { 0 };
    GuestRenderVisualStateId id = { 0 };
    GuestRenderCompletedState completed = { 0 };
    WrongThreadCall call = { 0 };
    SDL_Thread *thread;
    int thread_result = -1;

    guest_render_bridge_test_reset();
    CHECK(guest_render_bridge_process_owner() != NULL);
    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    CHECK(read_snapshot(&before));
    thread = SDL_CreateThread(wrong_thread_begin_state, "guest-render-bridge", &call);
    CHECK(thread != NULL);
    SDL_WaitThread(thread, &thread_result);
    CHECK(thread_result == 0);
    CHECK(call.result == GUEST_RENDER_WRONG_THREAD);
    CHECK(read_snapshot(&after));
    CHECK(!after.state_open);
    CHECK(!after.producer_open);
    CHECK(after.slot_count == 0u);
    CHECK(after.modes.requested_timing_mode == before.modes.requested_timing_mode);
    CHECK(after.modes.requested_render_mode == before.modes.requested_render_mode);
    CHECK(after.modes.effective_timing_mode ==
          GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(after.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(after.fallback_reason == GUEST_RENDER_FALLBACK_WRONG_THREAD);
    CHECK(guest_render_bridge_begin_state(&id) == GUEST_RENDER_OK);
    CHECK(id.scene_epoch == 1u && id.state_sequence == 0u);
    CHECK(guest_render_bridge_finalize_state(id, &completed) == GUEST_RENDER_OK);
    return 1;
}

typedef struct WrongThreadBindingCall {
    GuestRenderProducerHandle handle;
    int result;
} WrongThreadBindingCall;

static int wrong_thread_bind_packet(void *userdata) {
    WrongThreadBindingCall *call = userdata;

    call->result = guest_render_bridge_bind_packet(
        call->handle, UINT32_C(0x00000804), 1u);
    return 0;
}

static int test_wrong_thread_poison_clears_packet_bindings(void) {
    GuestRenderSceneConfig config = scene_config(GUEST_RENDER_TIMING_NATIVE_59_94,
                                                 GUEST_RENDER_RENDER_NATIVE);
    GuestRenderProducerProvenance provenance = native_provenance();
    GuestRenderVisualStateId id = { 0 };
    GuestRenderProducerHandle handle = { 0 };
    GuestRenderBridgeSnapshot snapshot = { 0 };
    WrongThreadBindingCall call = { 0 };
    SDL_Thread *thread;
    int thread_result = -1;

    guest_render_bridge_test_reset();
    CHECK(start_scene(&config, &id));
    CHECK(guest_render_bridge_producer_begin(id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    CHECK(guest_render_bridge_bind_packet(handle, UINT32_C(0x00000800), 0u) ==
          GUEST_RENDER_OK);
    call.handle = handle;
    thread = SDL_CreateThread(wrong_thread_bind_packet,
                              "guest-render-binding-thread", &call);
    CHECK(thread != NULL);
    SDL_WaitThread(thread, &thread_result);
    CHECK(thread_result == 0);
    CHECK(call.result == GUEST_RENDER_WRONG_THREAD);
    CHECK(read_snapshot(&snapshot));
    CHECK(snapshot.binding_count == 0u);
    CHECK(snapshot.producer_open);
    CHECK(snapshot.modes.effective_timing_mode ==
          GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.fallback_reason == GUEST_RENDER_FALLBACK_WRONG_THREAD);
    return 1;
}

typedef struct InitialClaimRace {
    SDL_mutex *mutex;
    SDL_cond *cond;
    GuestRenderSceneConfig config;
    unsigned int generation;
    unsigned int ready;
    unsigned int finished;
    unsigned int completed;
    atomic_bool stop;
    atomic_int status[2];
    atomic_int effective_timing[2];
    atomic_int effective_render[2];
    atomic_int fallback_reason[2];
} InitialClaimRace;

typedef struct InitialClaimWorker {
    InitialClaimRace *race;
    unsigned int index;
} InitialClaimWorker;

static int initial_claim_worker(void *userdata) {
    InitialClaimWorker *worker = userdata;
    InitialClaimRace *race = worker->race;
    unsigned int observed_generation = 0u;

    SDL_LockMutex(race->mutex);
    race->ready += 1u;
    SDL_CondBroadcast(race->cond);
    for (;;) {
        GuestRenderStatus status;

        while (!atomic_load_explicit(&race->stop, memory_order_acquire) &&
               race->generation == observed_generation) {
            SDL_CondWait(race->cond, race->mutex);
        }
        if (atomic_load_explicit(&race->stop, memory_order_acquire)) break;
        observed_generation = race->generation;
        SDL_UnlockMutex(race->mutex);
        status = guest_render_bridge_begin_scene(&race->config);
        atomic_store_explicit(&race->status[worker->index], status,
                              memory_order_relaxed);
        SDL_LockMutex(race->mutex);
        race->finished += 1u;
        SDL_CondBroadcast(race->cond);
        while (!atomic_load_explicit(&race->stop, memory_order_acquire) &&
               race->finished != 2u) {
            SDL_CondWait(race->cond, race->mutex);
        }
        if (atomic_load_explicit(&race->stop, memory_order_acquire)) break;
        if (status == GUEST_RENDER_OK) {
            GuestRenderBridgeSnapshot snapshot = { 0 };

            SDL_UnlockMutex(race->mutex);
            if (guest_render_bridge_snapshot(&snapshot) != GUEST_RENDER_OK) return 1;
            atomic_store_explicit(&race->effective_timing[worker->index],
                                  snapshot.modes.effective_timing_mode,
                                  memory_order_relaxed);
            atomic_store_explicit(&race->effective_render[worker->index],
                                  snapshot.modes.effective_render_mode,
                                  memory_order_relaxed);
            atomic_store_explicit(&race->fallback_reason[worker->index],
                                  snapshot.fallback_reason, memory_order_relaxed);
            SDL_LockMutex(race->mutex);
        }
        race->completed += 1u;
        SDL_CondBroadcast(race->cond);
    }
    SDL_UnlockMutex(race->mutex);
    return 0;
}

static int test_simultaneous_initial_owner_claim_preserves_poison(void) {
    InitialClaimRace race = {
        .config = { GUEST_RENDER_TIMING_NATIVE_59_94, GUEST_RENDER_RENDER_NATIVE },
    };
    InitialClaimWorker workers[] = {
        { &race, 0u },
        { &race, 1u },
    };
    SDL_Thread *threads[2] = { NULL, NULL };
    int thread_results[2] = { -1, -1 };
    int result = 1;

    race.mutex = SDL_CreateMutex();
    race.cond = SDL_CreateCond();
    if (race.mutex == NULL || race.cond == NULL) {
        if (race.cond != NULL) SDL_DestroyCond(race.cond);
        if (race.mutex != NULL) SDL_DestroyMutex(race.mutex);
        return 0;
    }
    threads[0] = SDL_CreateThread(initial_claim_worker, "guest-render-owner-a",
                                  &workers[0]);
    threads[1] = SDL_CreateThread(initial_claim_worker, "guest-render-owner-b",
                                  &workers[1]);
    if (!threads[0] || !threads[1]) result = 0;
    SDL_LockMutex(race.mutex);
    while (result && race.ready != 2u) SDL_CondWait(race.cond, race.mutex);
    SDL_UnlockMutex(race.mutex);
    for (unsigned int round = 1u;
         result && round <= GUEST_RENDER_BRIDGE_INITIAL_CLAIM_ROUNDS;
         ++round) {
        unsigned int owner_index;
        GuestRenderStatus first_status;
        GuestRenderStatus second_status;

        guest_render_bridge_test_reset();
        SDL_LockMutex(race.mutex);
        race.finished = 0u;
        race.completed = 0u;
        race.generation = round;
        SDL_CondBroadcast(race.cond);
        while (race.completed != 2u) SDL_CondWait(race.cond, race.mutex);
        first_status = atomic_load_explicit(&race.status[0], memory_order_relaxed);
        second_status = atomic_load_explicit(&race.status[1], memory_order_relaxed);
        SDL_UnlockMutex(race.mutex);
        if (first_status == GUEST_RENDER_OK && second_status == GUEST_RENDER_WRONG_THREAD)
            owner_index = 0u;
        else if (second_status == GUEST_RENDER_OK &&
                 first_status == GUEST_RENDER_WRONG_THREAD)
            owner_index = 1u;
        else {
            result = 0;
            break;
        }
        if (atomic_load_explicit(&race.effective_timing[owner_index],
                                 memory_order_relaxed) !=
                GUEST_RENDER_TIMING_NATIVE_59_94 ||
            atomic_load_explicit(&race.effective_render[owner_index],
                                 memory_order_relaxed) !=
                GUEST_RENDER_RENDER_ORIGINAL ||
            atomic_load_explicit(&race.fallback_reason[owner_index],
                                 memory_order_relaxed) !=
                GUEST_RENDER_FALLBACK_WRONG_THREAD) {
            result = 0;
            break;
        }
    }
    SDL_LockMutex(race.mutex);
    atomic_store_explicit(&race.stop, true, memory_order_release);
    race.generation += 1u;
    SDL_CondBroadcast(race.cond);
    SDL_UnlockMutex(race.mutex);
    for (size_t index = 0u; index < sizeof(threads) / sizeof(threads[0]); ++index) {
        if (threads[index]) SDL_WaitThread(threads[index], &thread_results[index]);
        if (thread_results[index] != 0) result = 0;
    }
    SDL_DestroyCond(race.cond);
    SDL_DestroyMutex(race.mutex);
    return result;
}

static int test_fallback_reason_names_are_stable(void) {
    static const struct {
        int reason;
        const char *name;
    } reasons[] = {
        { GUEST_RENDER_FALLBACK_NONE, "none" },
        { GUEST_RENDER_FALLBACK_FORCED_ORIGINAL, "forced_original" },
        { GUEST_RENDER_FALLBACK_INVALID_ARGUMENT, "invalid_argument" },
        { GUEST_RENDER_FALLBACK_SCENE_RESET, "scene_reset" },
        { GUEST_RENDER_FALLBACK_NESTED_PRODUCER, "nested_producer" },
        { GUEST_RENDER_FALLBACK_ACTIVE_PRODUCER, "active_producer" },
        { GUEST_RENDER_FALLBACK_WRONG_STATE, "wrong_state" },
        { GUEST_RENDER_FALLBACK_STALE_HANDLE, "stale_handle" },
        { GUEST_RENDER_FALLBACK_INVALID_PROVENANCE, "invalid_provenance" },
        { GUEST_RENDER_FALLBACK_SLOT_CAPACITY, "slot_capacity" },
        { GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED, "counter_exhausted" },
        { GUEST_RENDER_FALLBACK_WRONG_THREAD, "wrong_thread" },
        { GUEST_RENDER_FALLBACK_INVALID_PACKET_ADDRESS,
          "invalid_packet_address" },
        { GUEST_RENDER_FALLBACK_DUPLICATE_PACKET_ADDRESS,
          "duplicate_packet_address" },
        { GUEST_RENDER_FALLBACK_DUPLICATE_PRIMITIVE_INDEX,
          "duplicate_primitive_index" },
        { GUEST_RENDER_FALLBACK_BINDING_CAPACITY, "binding_capacity" },
        { GUEST_RENDER_FALLBACK_PRESENTATION_GATE, "presentation_gate" },
        { GUEST_RENDER_FALLBACK_BACKEND_FAILURE, "backend_failure" },
    };

    for (size_t index = 0u; index < sizeof(reasons) / sizeof(reasons[0]); ++index) {
        const char *name = guest_render_bridge_fallback_reason_name(reasons[index].reason);

        CHECK(name != NULL);
        CHECK(strcmp(name, reasons[index].name) == 0);
    }
    CHECK(strcmp(guest_render_bridge_fallback_reason_name(UINT32_MAX), "unknown") == 0);
    return 1;
}

int main(void) {
    if (!test_singleton_owner_and_first_id()) return 1;
    if (!test_scene_state_uniqueness_and_repeated_present()) return 1;
    if (!test_modes_are_copied_and_effective_modes_fall_back_independently()) return 1;
    if (!test_fallback_telemetry_tracks_scene_and_lifetime_state()) return 1;
    if (!test_sequential_producers_no_nesting_and_finalize_order()) return 1;
    if (!test_packet_bindings_span_lookup_and_repeated_present()) return 1;
    if (!test_packet_binding_duplicates_fail_closed()) return 1;
    if (!test_packet_binding_capacity_and_stale_handles_fail_closed()) return 1;
    if (!test_scene_reset_clears_completed_state_and_open_state_falls_back()) return 1;
    if (!test_scene_reset_and_abort_clear_packet_bindings()) return 1;
    if (!test_abort_scene_clears_all_state_and_requires_new_scene()) return 1;
    if (!test_slot_capacity_clears_native_slots()) return 1;
    if (!test_wrong_id_stale_handle_and_invalid_provenance_fail_closed()) return 1;
    if (!test_counter_exhaustion_never_wraps()) return 1;
    if (!test_null_arguments_are_rejected()) return 1;
    if (!test_wrong_thread_poison_does_not_create_a_state()) return 1;
    if (!test_wrong_thread_poison_clears_packet_bindings()) return 1;
    if (!test_simultaneous_initial_owner_claim_preserves_poison()) return 1;
    if (!test_fallback_reason_names_are_stable()) return 1;
    return 0;
}
