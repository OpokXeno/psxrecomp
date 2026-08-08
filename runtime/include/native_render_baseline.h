#ifndef PSX_NATIVE_RENDER_BASELINE_H
#define PSX_NATIVE_RENDER_BASELINE_H

#include "guest_render_bridge.h"
#include "gpu_render.h"
#include "gte_attribution.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NATIVE_RENDER_BASELINE_SCHEMA_VERSION = 2,
    NATIVE_RENDER_BASELINE_OT_CAPACITY = 4096,
    NATIVE_RENDER_BASELINE_VBLANK_CAPACITY = 65536,
};

typedef enum {
    NATIVE_RENDER_BASELINE_NONE = 0,
    NATIVE_RENDER_BASELINE_INTERPRETER = 1,
    NATIVE_RENDER_BASELINE_NATIVE = 2,
} NativeRenderBaselineMode;

typedef enum {
    NATIVE_RENDER_BASELINE_OT_VALID = 0,
    NATIVE_RENDER_BASELINE_OT_INVALID = 1,
    NATIVE_RENDER_BASELINE_OT_CYCLIC = 2,
} NativeRenderBaselineOtStatus;

typedef enum {
    NATIVE_RENDER_BASELINE_COMPLETE = 0,
    NATIVE_RENDER_BASELINE_DISABLED = 1,
    NATIVE_RENDER_BASELINE_INVALID_CONFIG = 2,
    NATIVE_RENDER_BASELINE_MISSING_GAME_DIGEST = 3,
    NATIVE_RENDER_BASELINE_PRODUCER_ABSENT = 4,
    NATIVE_RENDER_BASELINE_INVALID_OT = 5,
    NATIVE_RENDER_BASELINE_CYCLIC_OT = 6,
    NATIVE_RENDER_BASELINE_OVERFLOW = 7,
    NATIVE_RENDER_BASELINE_UNSUPPORTED_DISPLAY = 8,
    NATIVE_RENDER_BASELINE_MISSING_CAMERA_DIGEST = 9,
    NATIVE_RENDER_BASELINE_INCOMPLETE_OBSERVATION = 10,
    NATIVE_RENDER_BASELINE_MISSING_VISUAL_STATE = 11,
    NATIVE_RENDER_BASELINE_GTE_OVERFLOW = 12,
    NATIVE_RENDER_BASELINE_VRAM_SERIAL_OVERFLOW = 13,
    NATIVE_RENDER_BASELINE_INCOMPLETE_FIELDS = 14,
} NativeRenderBaselineReason;

typedef enum {
    NATIVE_RENDER_BASELINE_FIELD_GAME_IDENTITY = UINT64_C(1) << 0,
    NATIVE_RENDER_BASELINE_FIELD_VISUAL_STATE = UINT64_C(1) << 1,
    NATIVE_RENDER_BASELINE_FIELD_RENDER_MODES = UINT64_C(1) << 2,
    NATIVE_RENDER_BASELINE_FIELD_PRODUCER_COUNTS = UINT64_C(1) << 3,
    NATIVE_RENDER_BASELINE_FIELD_GTE_COUNTS = UINT64_C(1) << 4,
    NATIVE_RENDER_BASELINE_FIELD_OT_DIGEST = UINT64_C(1) << 5,
    NATIVE_RENDER_BASELINE_FIELD_MATERIAL_DIGEST = UINT64_C(1) << 6,
    NATIVE_RENDER_BASELINE_FIELD_VRAM_SERIAL = UINT64_C(1) << 7,
    NATIVE_RENDER_BASELINE_FIELD_VRAM_DIGEST = UINT64_C(1) << 8,
    NATIVE_RENDER_BASELINE_FIELD_DISPLAY15_DIGEST = UINT64_C(1) << 9,
    NATIVE_RENDER_BASELINE_FIELD_HOST_FRAMEBUFFER_DIGEST = UINT64_C(1) << 10,
    NATIVE_RENDER_BASELINE_FIELD_ALL =
        (UINT64_C(1) << 11) - UINT64_C(1),
} NativeRenderBaselineField;

typedef struct {
    uint32_t authenticated_producer_address;
    uint32_t max_vblanks;
    uint64_t game_digest;
} NativeRenderBaselineConfig;

/* node_address/next_node_address retain the OT insertion link. final_ordinal
 * is the exact painter-order ordinal observed while walking that link chain. */
typedef struct {
    uint32_t node_address;
    uint32_t next_node_address;
    uint32_t packet_words;
    uint32_t final_ordinal;
} NativeRenderBaselineOtNode;

typedef enum {
    NATIVE_RENDER_BASELINE_MATERIAL_OT = 1,
    NATIVE_RENDER_BASELINE_MATERIAL_DMA = 2,
    NATIVE_RENDER_BASELINE_MATERIAL_MMIO = 3,
} NativeRenderBaselineMaterialProvenance;

/* Material observations bind the effective draw state to its real command
 * source. submission_ordinal is final command order for semantic journals and
 * the exact source-word ordinal for direct Original GP0 execution. */
typedef struct {
    GpuRenderMaterial material;
    NativeRenderBaselineMaterialProvenance provenance;
    uint32_t command_address;
    uint64_t source_word_ordinal;
    uint64_t container_ordinal;
    uint64_t submission_ordinal;
    uint64_t word_count;
} NativeRenderBaselineMaterialObservation;

typedef struct {
    uint32_t schema_version;
    int enabled;
    int complete;
    int overflow;
    int invalid_ot;
    int cyclic_ot;
    NativeRenderBaselineReason incomplete_reason;
    uint64_t field_completeness_mask;
    uint64_t required_field_mask;

    GuestRenderVisualStateId visual_state_id;
    GuestRenderRenderMode requested_render_mode;
    GuestRenderRenderMode effective_render_mode;
    GuestRenderFallbackReason fallback_reason;
    uint64_t fallback_count;
    uint64_t producer_count;
    uint64_t producer_binding_count;

    uint64_t interpreter_calls;
    uint64_t native_calls;
    uint64_t gte_total_count;
    uint64_t gte_inside_producer_count;
    uint64_t gte_outside_producer_count;
    uint64_t gte_tier_counts[GTE_ATTRIBUTION_TIER_COUNT];
    GteAttributionOverflowReason gte_overflow_reason;
    int gte_blocked;

    uint64_t ot_lists;
    uint64_t ot_nodes;
    uint64_t ot_words;
    uint64_t ot_digest;
    uint64_t topology_digest;
    uint64_t material_samples;
    uint64_t material_digest;

    uint64_t gp0_writes;
    uint64_t gp1_writes;
    uint64_t vram_mutations;
    uint64_t global_vram_mutation_serial;
    int global_vram_serial_overflowed;
    uint64_t vram_digest;
    uint64_t gpu_digest;

    uint64_t display_samples;
    uint64_t display15_digest;
    uint64_t display_digest;
    uint64_t host_framebuffer_samples;
    uint64_t host_framebuffer_digest;

    uint64_t audio_frames;
    uint64_t audio_events;
    uint64_t vblank_delta;
    uint64_t guest_cycle_delta;
    uint64_t cycles_per_vblank;
    uint64_t cycle_digest;
    uint64_t audio_digest;
    uint64_t game_digest;
    uint64_t camera_actor_digest;
    uint64_t normalized_digest;
} NativeRenderBaselineSnapshot;

extern int g_native_render_baseline_armed;

void native_render_baseline_reset(void);
int native_render_baseline_arm(const NativeRenderBaselineConfig *config);
void native_render_baseline_set_auto_finalize_vblanks(uint32_t vblanks);
void native_render_baseline_note_execution_impl(uint32_t address,
                                                 NativeRenderBaselineMode mode);
void native_render_baseline_note_camera_actor_digest(uint64_t digest);
void native_render_baseline_note_material(
    const NativeRenderBaselineMaterialObservation *observation);
/* digest must cover a top-down, tightly normalized host framebuffer image;
 * dimensions and format normalization are owned by the host capture point. */
void native_render_baseline_note_host_framebuffer_digest(uint64_t digest);
int native_render_baseline_host_framebuffer_capture_due(void);
void native_render_baseline_ot_begin(uint32_t list_address);
void native_render_baseline_ot_node(const NativeRenderBaselineOtNode *node);
void native_render_baseline_ot_end(NativeRenderBaselineOtStatus status);
void native_render_baseline_observe_vblank_impl(void);
int native_render_baseline_finalize(void);
void native_render_baseline_snapshot(NativeRenderBaselineSnapshot *out);

#ifdef PSX_NATIVE_RENDER_BASELINE_TEST
void native_render_baseline_test_seed_ot_totals(uint64_t lists, uint64_t nodes,
                                                 uint64_t words);
uint64_t native_render_baseline_test_normalized_digest(
    const NativeRenderBaselineSnapshot *snapshot);
#endif

static inline int native_render_baseline_is_armed(void) {
    return g_native_render_baseline_armed;
}

static inline void native_render_baseline_note_execution(
        uint32_t address, NativeRenderBaselineMode mode) {
    if (g_native_render_baseline_armed)
        native_render_baseline_note_execution_impl(address, mode);
}

static inline void native_render_baseline_observe_vblank(void) {
    if (g_native_render_baseline_armed)
        native_render_baseline_observe_vblank_impl();
}

#ifdef __cplusplus
}
#endif

#endif
