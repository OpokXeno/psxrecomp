#include "guest_render_bridge.h"

#include "psx_sdl.h"

#include <stdatomic.h>
#include <string.h>

#if GUEST_RENDER_BRIDGE_SLOT_CAPACITY == 0
#error "GUEST_RENDER_BRIDGE_SLOT_CAPACITY must be nonzero"
#endif

#if GUEST_RENDER_BRIDGE_BINDING_CAPACITY == 0
#error "GUEST_RENDER_BRIDGE_BINDING_CAPACITY must be nonzero"
#endif

#define GUEST_RENDER_BRIDGE_BINDING_INDEX_CAPACITY \
    (GUEST_RENDER_BRIDGE_BINDING_CAPACITY * 2u)

typedef struct {
    uint32_t key;
    uint32_t generation;
    uint32_t binding_index;
} GuestRenderBindingIndexEntry;

typedef struct {
    uint64_t scene_epoch;
    uint64_t next_state_sequence;
    uint64_t scene_epoch_limit;
    uint64_t state_sequence_limit;
    uint64_t fallback_count_limit;
    uint_fast64_t observed_wrong_thread_poison;
    GuestRenderModes modes;
    GuestRenderFallbackReason fallback_reason;
    GuestRenderFallbackReason last_fallback_reason;
    GuestRenderVisualStateId open_id;
    GuestRenderProducerHandle active_handle;
    GuestRenderProducerProvenance active_provenance;
    GuestRenderCompletedState completed;
    GuestRenderBridgeSnapshot last_completed_snapshot;
    GuestRenderCompletedState last_completed;
    GuestRenderProducerSlot slots[GUEST_RENDER_BRIDGE_SLOT_CAPACITY];
    GuestRenderPacketBinding bindings[GUEST_RENDER_BRIDGE_BINDING_CAPACITY];
    GuestRenderBindingIndexEntry packet_index[
        GUEST_RENDER_BRIDGE_BINDING_INDEX_CAPACITY];
    GuestRenderBindingIndexEntry primitive_index[
        GUEST_RENDER_BRIDGE_BINDING_INDEX_CAPACITY];
    bool slot_complete[GUEST_RENDER_BRIDGE_SLOT_CAPACITY];
    size_t slot_count;
    size_t binding_count;
    size_t active_binding_start;
    uint32_t packet_index_generation;
    uint32_t primitive_index_generation;
    uint64_t fallback_count;
    uint64_t scene_fallback_count_baseline;
    bool scene_active;
    bool state_open;
    bool producer_open;
    bool scene_epoch_exhausted;
    bool state_sequence_exhausted;
    bool fallback_count_overflowed;
    bool completed_valid;
    bool last_completed_valid;
} GuestRenderBridge;

static GuestRenderBridge bridge = {
    .scene_epoch_limit = GUEST_RENDER_BRIDGE_SCENE_EPOCH_LIMIT,
    .state_sequence_limit = GUEST_RENDER_BRIDGE_STATE_SEQUENCE_LIMIT,
    .fallback_count_limit = GUEST_RENDER_BRIDGE_FALLBACK_COUNT_LIMIT,
};
static _Atomic uintptr_t owner_thread;
static atomic_uint_fast64_t wrong_thread_poison;

static size_t binding_index_hash(uint32_t key) {
    key ^= key >> 16u;
    key *= UINT32_C(0x7feb352d);
    key ^= key >> 15u;
    key *= UINT32_C(0x846ca68b);
    key ^= key >> 16u;
    return (size_t)key % GUEST_RENDER_BRIDGE_BINDING_INDEX_CAPACITY;
}

static void binding_index_advance(
        GuestRenderBindingIndexEntry *entries, uint32_t *generation) {
    ++*generation;
    if (*generation != 0u) return;
    memset(entries, 0,
           GUEST_RENDER_BRIDGE_BINDING_INDEX_CAPACITY * sizeof(*entries));
    *generation = 1u;
}

static GuestRenderBindingIndexEntry *binding_index_find(
        GuestRenderBindingIndexEntry *entries, uint32_t generation,
        uint32_t key) {
    const size_t start = binding_index_hash(key);

    for (size_t probe = 0u;
         probe < GUEST_RENDER_BRIDGE_BINDING_INDEX_CAPACITY; ++probe) {
        GuestRenderBindingIndexEntry *entry = &entries[
            (start + probe) % GUEST_RENDER_BRIDGE_BINDING_INDEX_CAPACITY];

        if (entry->generation != generation) return NULL;
        if (entry->key == key) return entry;
    }
    return NULL;
}

static GuestRenderBindingIndexEntry *binding_index_insert(
        GuestRenderBindingIndexEntry *entries, uint32_t generation,
        uint32_t key) {
    const size_t start = binding_index_hash(key);

    for (size_t probe = 0u;
         probe < GUEST_RENDER_BRIDGE_BINDING_INDEX_CAPACITY; ++probe) {
        GuestRenderBindingIndexEntry *entry = &entries[
            (start + probe) % GUEST_RENDER_BRIDGE_BINDING_INDEX_CAPACITY];

        if (entry->generation == generation) continue;
        entry->key = key;
        entry->generation = generation;
        return entry;
    }
    return NULL;
}

static bool id_is_equal(GuestRenderVisualStateId left,
                        GuestRenderVisualStateId right) {
    return left.scene_epoch == right.scene_epoch &&
           left.state_sequence == right.state_sequence;
}

static bool handle_is_equal(GuestRenderProducerHandle left,
                            GuestRenderProducerHandle right) {
    return id_is_equal(left.state_id, right.state_id) &&
           left.slot_index == right.slot_index;
}

static bool is_wrong_thread(void) {
    const uintptr_t owner = atomic_load_explicit(&owner_thread, memory_order_acquire);

    if (owner == 0u || owner == (uintptr_t)SDL_ThreadID()) return false;
    atomic_fetch_add_explicit(&wrong_thread_poison, 1u, memory_order_release);
    return true;
}

static GuestRenderStatus claim_scene_owner(void) {
    const uintptr_t current = (uintptr_t)SDL_ThreadID();
    uintptr_t expected = 0u;

    if (atomic_compare_exchange_strong_explicit(&owner_thread, &expected, current,
                                                memory_order_acq_rel,
                                                memory_order_acquire) ||
        expected == current)
        return GUEST_RENDER_OK;
    atomic_fetch_add_explicit(&wrong_thread_poison, 1u, memory_order_release);
    return GUEST_RENDER_WRONG_THREAD;
}

static void clear_bindings(void) {
    size_t index;

    binding_index_advance(bridge.packet_index,
                          &bridge.packet_index_generation);
    binding_index_advance(bridge.primitive_index,
                          &bridge.primitive_index_generation);
    bridge.binding_count = 0u;
    bridge.active_binding_start = 0u;
    bridge.completed_valid = false;
    memset(&bridge.completed, 0, sizeof(bridge.completed));
    for (index = 0u; index < GUEST_RENDER_BRIDGE_SLOT_CAPACITY; ++index) {
        bridge.slots[index].binding_start = 0u;
        bridge.slots[index].binding_count = 0u;
    }
}

static void clear_slots(void) {
    memset(bridge.slots, 0, sizeof(bridge.slots));
    memset(bridge.slot_complete, 0, sizeof(bridge.slot_complete));
    bridge.slot_count = 0u;
    clear_bindings();
}

static void set_fallback(GuestRenderFallbackReason reason) {
    if (bridge.fallback_reason == GUEST_RENDER_FALLBACK_NONE) {
        if (bridge.fallback_count >= bridge.fallback_count_limit) {
            bridge.fallback_count_overflowed = true;
            bridge.fallback_reason = GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED;
            bridge.last_fallback_reason =
                GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED;
            return;
        }
        bridge.fallback_reason = reason;
        bridge.last_fallback_reason = reason;
        ++bridge.fallback_count;
    }
}

static void demote_render(GuestRenderFallbackReason reason) {
    if (bridge.modes.requested_render_mode == GUEST_RENDER_RENDER_NATIVE) {
        if (reason != GUEST_RENDER_FALLBACK_FORCED_ORIGINAL)
            set_fallback(reason);
        return;
    }
    bridge.modes.effective_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
    set_fallback(reason);
}

static void observe_wrong_thread(void) {
    const uint_fast64_t poison =
        atomic_load_explicit(&wrong_thread_poison, memory_order_acquire);

    if (poison != bridge.observed_wrong_thread_poison) {
        bridge.observed_wrong_thread_poison = poison;
        clear_bindings();
        demote_render(GUEST_RENDER_FALLBACK_WRONG_THREAD);
    }
}

static bool timing_mode_is_valid(GuestRenderTimingMode mode) {
    return mode == GUEST_RENDER_TIMING_ORIGINAL ||
           mode == GUEST_RENDER_TIMING_NATIVE_59_94;
}

static bool render_mode_is_valid(GuestRenderRenderMode mode) {
    return mode == GUEST_RENDER_RENDER_ORIGINAL ||
           mode == GUEST_RENDER_RENDER_SHADOW ||
           mode == GUEST_RENDER_RENDER_NATIVE;
}

static bool provenance_is_valid(const GuestRenderProducerProvenance *provenance) {
    size_t index;

    if (provenance->tier != GUEST_RENDER_PRODUCER_NATIVE &&
        provenance->tier != GUEST_RENDER_PRODUCER_SHADOW)
        return false;
    for (index = 0u; index < sizeof(provenance->reserved); ++index) {
        if (provenance->reserved[index] != 0u) return false;
    }
    return true;
}

static bool normalize_packet_address(uint32_t address, uint32_t *out_address) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);

    if ((segment != 0u && segment != UINT32_C(0x80000000) &&
         segment != UINT32_C(0xa0000000)) ||
        physical > UINT32_C(0x001ffffc) || (physical & UINT32_C(3)) != 0u)
        return false;
    *out_address = physical;
    return true;
}

static bool completed_is_current(GuestRenderCompletedState completed) {
    return bridge.completed_valid && id_is_equal(completed.id, bridge.completed.id) &&
           completed.slot_count == bridge.completed.slot_count &&
           completed.binding_count == bridge.completed.binding_count;
}

const void *guest_render_bridge_process_owner(void) {
    return &bridge;
}

bool guest_render_bridge_id_equal(GuestRenderVisualStateId left,
                                  GuestRenderVisualStateId right) {
    return id_is_equal(left, right);
}

GuestRenderStatus guest_render_bridge_begin_scene(
        const GuestRenderSceneConfig *config) {
    bool reset_open_state;
    GuestRenderStatus owner_status;

    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    if (!config || !timing_mode_is_valid(config->timing_mode) ||
        !render_mode_is_valid(config->render_mode))
        return GUEST_RENDER_INVALID_ARGUMENT;
    owner_status = claim_scene_owner();
    if (owner_status != GUEST_RENDER_OK) return owner_status;
    reset_open_state = bridge.state_open || bridge.producer_open;
    if (bridge.scene_epoch_exhausted || bridge.scene_epoch_limit == 0u ||
        bridge.fallback_count_overflowed) {
        demote_render(GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED);
        return GUEST_RENDER_COUNTER_EXHAUSTED;
    }

    ++bridge.scene_epoch;
    bridge.scene_epoch_exhausted = bridge.scene_epoch == bridge.scene_epoch_limit;
    bridge.next_state_sequence = 0u;
    bridge.state_sequence_exhausted = false;
    bridge.modes.requested_timing_mode = config->timing_mode;
    bridge.modes.effective_timing_mode = config->timing_mode;
    bridge.modes.requested_render_mode = config->render_mode;
    bridge.modes.effective_render_mode = config->render_mode;
    bridge.fallback_reason = GUEST_RENDER_FALLBACK_NONE;
    bridge.scene_fallback_count_baseline = bridge.fallback_count;
    bridge.scene_active = true;
    bridge.state_open = false;
    bridge.producer_open = false;
    bridge.completed_valid = false;
    clear_slots();
    if (reset_open_state) demote_render(GUEST_RENDER_FALLBACK_SCENE_RESET);
    observe_wrong_thread();
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_begin_state(GuestRenderVisualStateId *out_id) {
    GuestRenderVisualStateId id;

    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    if (!out_id) return GUEST_RENDER_INVALID_ARGUMENT;
    observe_wrong_thread();
    if (!bridge.scene_active || bridge.state_open) {
        demote_render(GUEST_RENDER_FALLBACK_WRONG_STATE);
        return GUEST_RENDER_INVALID_TRANSITION;
    }
    if (bridge.state_sequence_exhausted) {
        demote_render(GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED);
        return GUEST_RENDER_COUNTER_EXHAUSTED;
    }

    id.scene_epoch = bridge.scene_epoch;
    id.state_sequence = bridge.next_state_sequence;
    bridge.state_sequence_exhausted =
        bridge.next_state_sequence == bridge.state_sequence_limit;
    if (!bridge.state_sequence_exhausted) ++bridge.next_state_sequence;
    bridge.open_id = id;
    bridge.state_open = true;
    bridge.producer_open = false;
    clear_slots();
    *out_id = id;
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_producer_begin(
        GuestRenderVisualStateId state_id,
        const GuestRenderProducerProvenance *provenance,
        GuestRenderProducerHandle *out_handle) {
    GuestRenderProducerHandle handle;

    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    if (!provenance || !out_handle) return GUEST_RENDER_INVALID_ARGUMENT;
    observe_wrong_thread();
    if (!bridge.state_open || state_id.scene_epoch == 0u ||
        !id_is_equal(state_id, bridge.open_id)) {
        demote_render(GUEST_RENDER_FALLBACK_WRONG_STATE);
        return GUEST_RENDER_INVALID_TRANSITION;
    }
    if (!provenance_is_valid(provenance)) {
        clear_slots();
        demote_render(GUEST_RENDER_FALLBACK_INVALID_PROVENANCE);
        return GUEST_RENDER_INVALID_PROVENANCE;
    }
    if (bridge.producer_open) {
        clear_slots();
        demote_render(GUEST_RENDER_FALLBACK_NESTED_PRODUCER);
        return GUEST_RENDER_INVALID_TRANSITION;
    }
    if (bridge.slot_count == GUEST_RENDER_BRIDGE_SLOT_CAPACITY) {
        clear_slots();
        demote_render(GUEST_RENDER_FALLBACK_SLOT_CAPACITY);
        return GUEST_RENDER_SLOT_CAPACITY_EXCEEDED;
    }

    handle.state_id = state_id;
    handle.slot_index = bridge.slot_count;
    ++bridge.slot_count;
    bridge.active_handle = handle;
    bridge.active_provenance = *provenance;
    bridge.active_binding_start = bridge.binding_count;
    binding_index_advance(bridge.primitive_index,
                          &bridge.primitive_index_generation);
    bridge.producer_open = true;
    *out_handle = handle;
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_producer_end(
        GuestRenderProducerHandle handle, GuestRenderProducerSlot *out_slot) {
    size_t slot_index;

    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    if (!out_slot) return GUEST_RENDER_INVALID_ARGUMENT;
    observe_wrong_thread();
    if (!bridge.producer_open || !handle_is_equal(handle, bridge.active_handle)) {
        clear_slots();
        demote_render(GUEST_RENDER_FALLBACK_STALE_HANDLE);
        return GUEST_RENDER_STALE_HANDLE;
    }

    slot_index = bridge.active_handle.slot_index;
    if (bridge.slot_count <= slot_index) bridge.slot_count = slot_index + 1u;
    bridge.slots[slot_index].handle = bridge.active_handle;
    bridge.slots[slot_index].provenance = bridge.active_provenance;
    bridge.slots[slot_index].binding_start = bridge.active_binding_start;
    bridge.slots[slot_index].binding_count =
        bridge.binding_count - bridge.active_binding_start;
    bridge.slot_complete[slot_index] = true;
    bridge.producer_open = false;
    *out_slot = bridge.slots[slot_index];
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_bind_packet(
        GuestRenderProducerHandle handle, uint32_t packet_address,
        uint32_t source_primitive_index) {
    GuestRenderPacketBinding *binding;
    GuestRenderBindingIndexEntry *packet_entry;
    GuestRenderBindingIndexEntry *primitive_entry;
    uint32_t normalized_address;

    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    observe_wrong_thread();
    if (!bridge.producer_open || !handle_is_equal(handle, bridge.active_handle)) {
        clear_bindings();
        demote_render(GUEST_RENDER_FALLBACK_STALE_HANDLE);
        return GUEST_RENDER_STALE_HANDLE;
    }
    if (!normalize_packet_address(packet_address, &normalized_address)) {
        clear_bindings();
        demote_render(GUEST_RENDER_FALLBACK_INVALID_PACKET_ADDRESS);
        return GUEST_RENDER_INVALID_ARGUMENT;
    }
    if (binding_index_find(bridge.packet_index,
                           bridge.packet_index_generation,
                           normalized_address) != NULL) {
        clear_bindings();
        demote_render(GUEST_RENDER_FALLBACK_DUPLICATE_PACKET_ADDRESS);
        return GUEST_RENDER_DUPLICATE_PACKET_ADDRESS;
    }
    if (binding_index_find(bridge.primitive_index,
                           bridge.primitive_index_generation,
                           source_primitive_index) != NULL) {
        clear_bindings();
        demote_render(GUEST_RENDER_FALLBACK_DUPLICATE_PRIMITIVE_INDEX);
        return GUEST_RENDER_DUPLICATE_PRIMITIVE_INDEX;
    }
    if (bridge.binding_count == GUEST_RENDER_BRIDGE_BINDING_CAPACITY) {
        clear_bindings();
        demote_render(GUEST_RENDER_FALLBACK_BINDING_CAPACITY);
        return GUEST_RENDER_BINDING_CAPACITY_EXCEEDED;
    }

    packet_entry = binding_index_insert(
        bridge.packet_index, bridge.packet_index_generation,
        normalized_address);
    primitive_entry = binding_index_insert(
        bridge.primitive_index, bridge.primitive_index_generation,
        source_primitive_index);
    if (packet_entry == NULL || primitive_entry == NULL) {
        clear_bindings();
        demote_render(GUEST_RENDER_FALLBACK_BINDING_CAPACITY);
        return GUEST_RENDER_BINDING_CAPACITY_EXCEEDED;
    }
    packet_entry->binding_index = (uint32_t)bridge.binding_count;
    binding = &bridge.bindings[bridge.binding_count++];
    binding->handle = bridge.active_handle;
    binding->packet_address = normalized_address;
    binding->source_primitive_index = source_primitive_index;
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_get_slot(
        GuestRenderProducerHandle handle, GuestRenderProducerSlot *out_slot) {
    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    if (!out_slot) return GUEST_RENDER_INVALID_ARGUMENT;
    observe_wrong_thread();
    if (!bridge.state_open || !id_is_equal(handle.state_id, bridge.open_id) ||
        handle.slot_index >= bridge.slot_count ||
        !bridge.slot_complete[handle.slot_index] ||
        !handle_is_equal(handle, bridge.slots[handle.slot_index].handle)) {
        clear_slots();
        demote_render(GUEST_RENDER_FALLBACK_STALE_HANDLE);
        return GUEST_RENDER_STALE_HANDLE;
    }
    *out_slot = bridge.slots[handle.slot_index];
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_finalize_state(
        GuestRenderVisualStateId state_id,
        GuestRenderCompletedState *out_completed) {
    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    if (!out_completed) return GUEST_RENDER_INVALID_ARGUMENT;
    observe_wrong_thread();
    if (!bridge.state_open || !id_is_equal(state_id, bridge.open_id)) {
        demote_render(GUEST_RENDER_FALLBACK_WRONG_STATE);
        return GUEST_RENDER_INVALID_TRANSITION;
    }
    if (bridge.producer_open) {
        demote_render(GUEST_RENDER_FALLBACK_ACTIVE_PRODUCER);
        return GUEST_RENDER_INVALID_TRANSITION;
    }

    bridge.completed.id = bridge.open_id;
    bridge.completed.slot_count = bridge.slot_count;
    bridge.completed.binding_count = bridge.binding_count;
    bridge.completed_valid = true;
    bridge.last_completed_snapshot.modes = bridge.modes;
    bridge.last_completed_snapshot.fallback_reason = bridge.fallback_reason;
    bridge.last_completed_snapshot.last_fallback_reason =
        bridge.last_fallback_reason;
    bridge.last_completed_snapshot.state_open = false;
    bridge.last_completed_snapshot.producer_open = false;
    bridge.last_completed_snapshot.slot_count = bridge.slot_count;
    bridge.last_completed_snapshot.binding_count = bridge.binding_count;
    bridge.last_completed_snapshot.fallback_count = bridge.fallback_count;
    bridge.last_completed_snapshot.scene_fallback_count_baseline =
        bridge.scene_fallback_count_baseline;
    bridge.last_completed_snapshot.scene_fallback_count_delta =
        bridge.fallback_count - bridge.scene_fallback_count_baseline;
    bridge.last_completed_snapshot.fallback_count_overflowed =
        bridge.fallback_count_overflowed;
    bridge.last_completed = bridge.completed;
    bridge.last_completed_valid = true;
    bridge.state_open = false;
    *out_completed = bridge.completed;
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_present(GuestRenderCompletedState *out_completed) {
    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    if (!out_completed) return GUEST_RENDER_INVALID_ARGUMENT;
    observe_wrong_thread();
    if (!bridge.completed_valid) return GUEST_RENDER_NO_COMPLETED_STATE;
    *out_completed = bridge.completed;
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_last_completed(
        GuestRenderBridgeSnapshot *out_snapshot,
        GuestRenderCompletedState *out_completed) {
    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    if (!out_snapshot || !out_completed) return GUEST_RENDER_INVALID_ARGUMENT;
    observe_wrong_thread();
    if (!bridge.last_completed_valid)
        return GUEST_RENDER_NO_COMPLETED_STATE;
    *out_snapshot = bridge.last_completed_snapshot;
    *out_completed = bridge.last_completed;
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_get_completed_binding(
        GuestRenderCompletedState completed, size_t binding_index,
        GuestRenderPacketBinding *out_binding) {
    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    if (!out_binding) return GUEST_RENDER_INVALID_ARGUMENT;
    observe_wrong_thread();
    if (!completed_is_current(completed)) return GUEST_RENDER_NO_COMPLETED_STATE;
    if (binding_index >= bridge.binding_count) return GUEST_RENDER_BINDING_NOT_FOUND;
    *out_binding = bridge.bindings[binding_index];
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_find_completed_binding(
        GuestRenderCompletedState completed, uint32_t packet_address,
        GuestRenderPacketBinding *out_binding) {
    uint32_t normalized_address;
    GuestRenderBindingIndexEntry *entry;

    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    if (!out_binding || !normalize_packet_address(packet_address, &normalized_address))
        return GUEST_RENDER_INVALID_ARGUMENT;
    observe_wrong_thread();
    if (!completed_is_current(completed)) return GUEST_RENDER_NO_COMPLETED_STATE;
    entry = binding_index_find(bridge.packet_index,
                               bridge.packet_index_generation,
                               normalized_address);
    if (entry == NULL || entry->binding_index >= bridge.binding_count)
        return GUEST_RENDER_BINDING_NOT_FOUND;
    *out_binding = bridge.bindings[entry->binding_index];
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_snapshot(GuestRenderBridgeSnapshot *out_snapshot) {
    if (is_wrong_thread()) return GUEST_RENDER_WRONG_THREAD;
    if (!out_snapshot) return GUEST_RENDER_INVALID_ARGUMENT;
    observe_wrong_thread();
    out_snapshot->modes = bridge.modes;
    out_snapshot->fallback_reason = bridge.fallback_reason;
    out_snapshot->last_fallback_reason = bridge.last_fallback_reason;
    out_snapshot->state_open = bridge.state_open;
    out_snapshot->producer_open = bridge.producer_open;
    out_snapshot->slot_count = bridge.slot_count;
    out_snapshot->binding_count = bridge.binding_count;
    out_snapshot->fallback_count = bridge.fallback_count;
    out_snapshot->scene_fallback_count_baseline =
        bridge.scene_fallback_count_baseline;
    out_snapshot->scene_fallback_count_delta =
        bridge.fallback_count - bridge.scene_fallback_count_baseline;
    out_snapshot->fallback_count_overflowed = bridge.fallback_count_overflowed;
    return GUEST_RENDER_OK;
}

void guest_render_bridge_abort_scene(GuestRenderFallbackReason reason) {
    if (is_wrong_thread()) return;
    observe_wrong_thread();
    if (reason == GUEST_RENDER_FALLBACK_NONE)
        reason = GUEST_RENDER_FALLBACK_FORCED_ORIGINAL;
    if (reason > GUEST_RENDER_FALLBACK_BACKEND_FAILURE)
        reason = GUEST_RENDER_FALLBACK_INVALID_ARGUMENT;
    demote_render(reason);
    memset(&bridge.open_id, 0, sizeof(bridge.open_id));
    memset(&bridge.active_handle, 0, sizeof(bridge.active_handle));
    memset(&bridge.active_provenance, 0, sizeof(bridge.active_provenance));
    memset(&bridge.completed, 0, sizeof(bridge.completed));
    bridge.scene_active = false;
    bridge.state_open = false;
    bridge.producer_open = false;
    bridge.completed_valid = false;
    clear_slots();
}

void guest_render_bridge_reset_scene(void) {
    if (is_wrong_thread()) return;
    observe_wrong_thread();
    memset(&bridge.open_id, 0, sizeof(bridge.open_id));
    memset(&bridge.active_handle, 0, sizeof(bridge.active_handle));
    memset(&bridge.active_provenance, 0, sizeof(bridge.active_provenance));
    memset(&bridge.completed, 0, sizeof(bridge.completed));
    if (bridge.modes.requested_render_mode == GUEST_RENDER_RENDER_NATIVE) {
        bridge.modes.effective_timing_mode =
            bridge.modes.requested_timing_mode;
        bridge.modes.effective_render_mode = GUEST_RENDER_RENDER_NATIVE;
    } else {
        bridge.modes.effective_timing_mode = GUEST_RENDER_TIMING_ORIGINAL;
        bridge.modes.effective_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
    }
    bridge.fallback_reason = GUEST_RENDER_FALLBACK_NONE;
    bridge.scene_active = false;
    bridge.state_open = false;
    bridge.producer_open = false;
    bridge.completed_valid = false;
    clear_slots();
}

void guest_render_bridge_force_original(GuestRenderFallbackReason reason) {
    if (is_wrong_thread()) return;
    observe_wrong_thread();
    if (reason == GUEST_RENDER_FALLBACK_NONE)
        reason = GUEST_RENDER_FALLBACK_FORCED_ORIGINAL;
    if (reason > GUEST_RENDER_FALLBACK_BACKEND_FAILURE)
        reason = GUEST_RENDER_FALLBACK_INVALID_ARGUMENT;
    demote_render(reason);
}

size_t guest_render_bridge_slot_capacity(void) {
    return GUEST_RENDER_BRIDGE_SLOT_CAPACITY;
}

size_t guest_render_bridge_binding_capacity(void) {
    return GUEST_RENDER_BRIDGE_BINDING_CAPACITY;
}

const char *guest_render_bridge_fallback_reason_name(uint32_t reason) {
    switch (reason) {
    case GUEST_RENDER_FALLBACK_NONE: return "none";
    case GUEST_RENDER_FALLBACK_FORCED_ORIGINAL: return "forced_original";
    case GUEST_RENDER_FALLBACK_INVALID_ARGUMENT: return "invalid_argument";
    case GUEST_RENDER_FALLBACK_SCENE_RESET: return "scene_reset";
    case GUEST_RENDER_FALLBACK_NESTED_PRODUCER: return "nested_producer";
    case GUEST_RENDER_FALLBACK_ACTIVE_PRODUCER: return "active_producer";
    case GUEST_RENDER_FALLBACK_WRONG_STATE: return "wrong_state";
    case GUEST_RENDER_FALLBACK_STALE_HANDLE: return "stale_handle";
    case GUEST_RENDER_FALLBACK_INVALID_PROVENANCE: return "invalid_provenance";
    case GUEST_RENDER_FALLBACK_SLOT_CAPACITY: return "slot_capacity";
    case GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED: return "counter_exhausted";
    case GUEST_RENDER_FALLBACK_WRONG_THREAD: return "wrong_thread";
    case GUEST_RENDER_FALLBACK_INVALID_PACKET_ADDRESS:
        return "invalid_packet_address";
    case GUEST_RENDER_FALLBACK_DUPLICATE_PACKET_ADDRESS:
        return "duplicate_packet_address";
    case GUEST_RENDER_FALLBACK_DUPLICATE_PRIMITIVE_INDEX:
        return "duplicate_primitive_index";
    case GUEST_RENDER_FALLBACK_BINDING_CAPACITY: return "binding_capacity";
    case GUEST_RENDER_FALLBACK_PRESENTATION_GATE:
        return "presentation_gate";
    case GUEST_RENDER_FALLBACK_BACKEND_FAILURE: return "backend_failure";
    default: return "unknown";
    }
}

#ifdef GUEST_RENDER_BRIDGE_TESTING
void guest_render_bridge_test_reset(void) {
    memset(&bridge, 0, sizeof(bridge));
    bridge.scene_epoch_limit = GUEST_RENDER_BRIDGE_SCENE_EPOCH_LIMIT;
    bridge.state_sequence_limit = GUEST_RENDER_BRIDGE_STATE_SEQUENCE_LIMIT;
    bridge.fallback_count_limit = GUEST_RENDER_BRIDGE_FALLBACK_COUNT_LIMIT;
    atomic_store_explicit(&owner_thread, 0u, memory_order_release);
    atomic_store_explicit(&wrong_thread_poison, 0u, memory_order_release);
}

void guest_render_bridge_test_set_counter_limits(uint64_t scene_epoch_limit,
                                                 uint64_t state_sequence_limit) {
    bridge.scene_epoch_limit = scene_epoch_limit;
    bridge.state_sequence_limit = state_sequence_limit;
}

void guest_render_bridge_test_set_fallback_count_limit(
        uint64_t fallback_count_limit) {
    bridge.fallback_count_limit = fallback_count_limit;
}
#endif
