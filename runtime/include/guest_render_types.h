#ifndef PSX_GUEST_RENDER_TYPES_H
#define PSX_GUEST_RENDER_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef GUEST_RENDER_BRIDGE_SLOT_CAPACITY
#define GUEST_RENDER_BRIDGE_SLOT_CAPACITY 64u
#endif

#ifndef GUEST_RENDER_BRIDGE_BINDING_CAPACITY
#ifdef GUEST_RENDER_BRIDGE_TESTING
#ifndef GUEST_RENDER_BRIDGE_TEST_BINDING_CAPACITY
#define GUEST_RENDER_BRIDGE_TEST_BINDING_CAPACITY 4u
#endif
#define GUEST_RENDER_BRIDGE_BINDING_CAPACITY \
    GUEST_RENDER_BRIDGE_TEST_BINDING_CAPACITY
#else
#define GUEST_RENDER_BRIDGE_BINDING_CAPACITY 4096u
#endif
#endif

#ifndef GUEST_RENDER_BRIDGE_SCENE_EPOCH_LIMIT
#define GUEST_RENDER_BRIDGE_SCENE_EPOCH_LIMIT UINT64_MAX
#endif

#ifndef GUEST_RENDER_BRIDGE_STATE_SEQUENCE_LIMIT
#define GUEST_RENDER_BRIDGE_STATE_SEQUENCE_LIMIT UINT64_MAX
#endif

#ifndef GUEST_RENDER_BRIDGE_FALLBACK_COUNT_LIMIT
#define GUEST_RENDER_BRIDGE_FALLBACK_COUNT_LIMIT UINT64_MAX
#endif

typedef enum {
    GUEST_RENDER_OK = 0,
    GUEST_RENDER_INVALID_ARGUMENT,
    GUEST_RENDER_INVALID_TRANSITION,
    GUEST_RENDER_NO_COMPLETED_STATE,
    GUEST_RENDER_SLOT_CAPACITY_EXCEEDED,
    GUEST_RENDER_STALE_HANDLE,
    GUEST_RENDER_INVALID_PROVENANCE,
    GUEST_RENDER_COUNTER_EXHAUSTED,
    GUEST_RENDER_WRONG_THREAD,
    GUEST_RENDER_DUPLICATE_PACKET_ADDRESS,
    GUEST_RENDER_DUPLICATE_PRIMITIVE_INDEX,
    GUEST_RENDER_BINDING_CAPACITY_EXCEEDED,
    GUEST_RENDER_BINDING_NOT_FOUND,
} GuestRenderStatus;

typedef enum {
    GUEST_RENDER_TIMING_ORIGINAL = 0,
    GUEST_RENDER_TIMING_NATIVE_59_94,
} GuestRenderTimingMode;

typedef enum {
    GUEST_RENDER_RENDER_ORIGINAL = 0,
    GUEST_RENDER_RENDER_SHADOW,
    GUEST_RENDER_RENDER_NATIVE,
} GuestRenderRenderMode;

typedef enum {
    GUEST_RENDER_PRODUCER_NATIVE = 0,
    GUEST_RENDER_PRODUCER_SHADOW,
} GuestRenderProducerTier;

typedef enum {
    GUEST_RENDER_FALLBACK_NONE = 0,
    GUEST_RENDER_FALLBACK_FORCED_ORIGINAL,
    GUEST_RENDER_FALLBACK_INVALID_ARGUMENT,
    GUEST_RENDER_FALLBACK_SCENE_RESET,
    GUEST_RENDER_FALLBACK_NESTED_PRODUCER,
    GUEST_RENDER_FALLBACK_ACTIVE_PRODUCER,
    GUEST_RENDER_FALLBACK_WRONG_STATE,
    GUEST_RENDER_FALLBACK_STALE_HANDLE,
    GUEST_RENDER_FALLBACK_INVALID_PROVENANCE,
    GUEST_RENDER_FALLBACK_SLOT_CAPACITY,
    GUEST_RENDER_FALLBACK_COUNTER_EXHAUSTED,
    GUEST_RENDER_FALLBACK_WRONG_THREAD,
    GUEST_RENDER_FALLBACK_INVALID_PACKET_ADDRESS,
    GUEST_RENDER_FALLBACK_DUPLICATE_PACKET_ADDRESS,
    GUEST_RENDER_FALLBACK_DUPLICATE_PRIMITIVE_INDEX,
    GUEST_RENDER_FALLBACK_BINDING_CAPACITY,
    GUEST_RENDER_FALLBACK_PRESENTATION_GATE,
    GUEST_RENDER_FALLBACK_BACKEND_FAILURE,
} GuestRenderFallbackReason;

typedef struct {
    uint64_t scene_epoch;
    uint64_t state_sequence;
} GuestRenderVisualStateId;

typedef struct {
    GuestRenderTimingMode timing_mode;
    GuestRenderRenderMode render_mode;
} GuestRenderSceneConfig;

typedef struct {
    GuestRenderTimingMode requested_timing_mode;
    GuestRenderTimingMode effective_timing_mode;
    GuestRenderRenderMode requested_render_mode;
    GuestRenderRenderMode effective_render_mode;
} GuestRenderModes;

typedef struct {
    GuestRenderProducerTier tier;
    uint8_t reserved[16];
} GuestRenderProducerProvenance;

typedef struct {
    GuestRenderVisualStateId state_id;
    size_t slot_index;
} GuestRenderProducerHandle;

typedef struct {
    GuestRenderProducerHandle handle;
    GuestRenderProducerProvenance provenance;
    size_t binding_start;
    size_t binding_count;
} GuestRenderProducerSlot;

typedef struct {
    GuestRenderProducerHandle handle;
    uint32_t packet_address;
    uint32_t source_primitive_index;
} GuestRenderPacketBinding;

typedef struct {
    GuestRenderVisualStateId id;
    size_t slot_count;
    size_t binding_count;
} GuestRenderCompletedState;

typedef struct {
    GuestRenderModes modes;
    GuestRenderFallbackReason fallback_reason;
    GuestRenderFallbackReason last_fallback_reason;
    bool state_open;
    bool producer_open;
    size_t slot_count;
    size_t binding_count;
    uint64_t fallback_count;
    uint64_t scene_fallback_count_baseline;
    uint64_t scene_fallback_count_delta;
    bool fallback_count_overflowed;
} GuestRenderBridgeSnapshot;

#endif
