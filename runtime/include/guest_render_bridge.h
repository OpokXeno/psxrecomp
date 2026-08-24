#ifndef PSX_GUEST_RENDER_BRIDGE_H
#define PSX_GUEST_RENDER_BRIDGE_H

#include "guest_render_types.h"

#ifdef __cplusplus
extern "C" {
#endif

const void *guest_render_bridge_process_owner(void);
bool guest_render_bridge_id_equal(GuestRenderVisualStateId left,
                                  GuestRenderVisualStateId right);
GuestRenderStatus guest_render_bridge_begin_scene(
        const GuestRenderSceneConfig *config);
GuestRenderStatus guest_render_bridge_begin_state(GuestRenderVisualStateId *out_id);
GuestRenderStatus guest_render_bridge_producer_begin(
        GuestRenderVisualStateId state_id,
        const GuestRenderProducerProvenance *provenance,
        GuestRenderProducerHandle *out_handle);
GuestRenderStatus guest_render_bridge_producer_end(
        GuestRenderProducerHandle handle, GuestRenderProducerSlot *out_slot);
GuestRenderStatus guest_render_bridge_bind_packet(
        GuestRenderProducerHandle handle, uint32_t packet_address,
        uint32_t source_primitive_index);
GuestRenderStatus guest_render_bridge_get_slot(
        GuestRenderProducerHandle handle, GuestRenderProducerSlot *out_slot);
GuestRenderStatus guest_render_bridge_finalize_state(
        GuestRenderVisualStateId state_id,
        GuestRenderCompletedState *out_completed);
GuestRenderStatus guest_render_bridge_present(GuestRenderCompletedState *out_completed);
/* Returns the most recently finalized state even after the current scene has
 * been reset. It is observation-only; present() retains current-scene rules. */
GuestRenderStatus guest_render_bridge_last_completed(
        GuestRenderBridgeSnapshot *out_snapshot,
        GuestRenderCompletedState *out_completed);
GuestRenderStatus guest_render_bridge_get_completed_binding(
        GuestRenderCompletedState completed, size_t binding_index,
        GuestRenderPacketBinding *out_binding);
GuestRenderStatus guest_render_bridge_find_completed_binding(
        GuestRenderCompletedState completed, uint32_t packet_address,
        GuestRenderPacketBinding *out_binding);
GuestRenderStatus guest_render_bridge_snapshot(GuestRenderBridgeSnapshot *out_snapshot);
void guest_render_bridge_abort_scene(GuestRenderFallbackReason reason);
void guest_render_bridge_reset_scene(void);
void guest_render_bridge_force_original(GuestRenderFallbackReason reason);
size_t guest_render_bridge_slot_capacity(void);
size_t guest_render_bridge_binding_capacity(void);
const char *guest_render_bridge_fallback_reason_name(uint32_t reason);

#ifdef GUEST_RENDER_BRIDGE_TESTING
void guest_render_bridge_test_reset(void);
void guest_render_bridge_test_set_counter_limits(uint64_t scene_epoch_limit,
                                                 uint64_t state_sequence_limit);
void guest_render_bridge_test_set_fallback_count_limit(uint64_t fallback_count_limit);
#endif

#ifdef __cplusplus
}
#endif

#endif
