#ifndef PSX_GPU_SEMANTIC_WORKLOAD_H
#define PSX_GPU_SEMANTIC_WORKLOAD_H

#include "gpu_render.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPU_SEMANTIC_WORKLOAD_CAPACITY 4096u
#define GPU_SEMANTIC_INTERPOLATION_MAX_PHASES 7u

typedef enum GpuSemanticWorkloadStatus {
    GPU_SEMANTIC_WORKLOAD_OK = 0,
    GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT,
    GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION,
    GPU_SEMANTIC_WORKLOAD_CAPACITY_EXCEEDED,
    GPU_SEMANTIC_WORKLOAD_NOT_FOUND,
    GPU_SEMANTIC_WORKLOAD_CONFLICT,
} GpuSemanticWorkloadStatus;

typedef enum GpuSemanticWorkloadEligibility {
    GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_UNKNOWN = 0,
    GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_ELIGIBLE,
    GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_NO_PREVIOUS,
    GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_OVERFLOW,
    GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_COUNT_MISMATCH,
    GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_INCOMPLETE_MATCH,
    GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_STATIC,
    GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_COUNT_MISMATCH,
    GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_INCOMPLETE_MATCH,
} GpuSemanticWorkloadEligibility;

typedef enum GpuSemanticWorkloadMatchKind {
    GPU_SEMANTIC_WORKLOAD_MATCH_UNKNOWN = 0,
    GPU_SEMANTIC_WORKLOAD_MATCH_IDENTITY,
    GPU_SEMANTIC_WORKLOAD_MATCH_RETROSPECTIVE,
    GPU_SEMANTIC_WORKLOAD_MATCH_SOURCE_GEOMETRY,
    GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_NO_PREVIOUS,
    GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_NOT_FOUND,
    GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_AMBIGUOUS,
    GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_ALREADY_USED,
    GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_INCOMPATIBLE,
    GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_UNKEYED,
} GpuSemanticWorkloadMatchKind;

typedef enum GpuSemanticWorkloadParticipation {
    GPU_SEMANTIC_WORKLOAD_PARTICIPATION_UNKNOWN = 0,
    GPU_SEMANTIC_WORKLOAD_PARTICIPATION_AUTHORITATIVE_CURRENT,
    GPU_SEMANTIC_WORKLOAD_PARTICIPATION_TEMPORAL_PHASE,
    GPU_SEMANTIC_WORKLOAD_PARTICIPATION_HISTORY_ONLY,
} GpuSemanticWorkloadParticipation;

typedef struct GpuSemanticWorkloadMatchInfo {
    GpuSemanticWorkloadMatchKind kind;
    GpuSemanticWorkloadMatchKind fallback_kind;
    GpuSemanticWorkloadParticipation participation;
    size_t current_order;
    size_t previous_order;
    bool previous_order_valid;
} GpuSemanticWorkloadMatchInfo;

typedef struct GpuSemanticWorkloadMotionDiagnostics {
    uint64_t epoch;
    uint64_t source_frame;
    uint64_t sequence;
    GpuSemanticWorkloadMatchKind match_kind;
    GpuSemanticWorkloadMatchKind fallback_kind;
    size_t current_order;
    size_t previous_order;
    size_t position_changed_vertex_count;
    uint64_t position_delta_fixed;
    GpuRenderSemantic previous;
    GpuRenderSemantic current;
    GpuRenderSemantic midpoint;
    bool valid;
    bool previous_valid;
} GpuSemanticWorkloadMotionDiagnostics;

typedef struct GpuSemanticWorkloadDiagnostics {
    uint64_t epoch;
    uint64_t sealed_frames;
    uint64_t total_recorded;
    uint64_t total_dropped;
    uint64_t total_matched;
    uint64_t total_snapped;
    uint64_t total_ambiguous;
    uint64_t total_moved;
    uint64_t total_unkeyed;
    uint64_t total_exact_matches;
    uint64_t total_exact_semitransparent_matches;
    uint64_t total_source_geometry_matches;
    uint64_t total_matched_vertices;
    uint64_t total_position_changed_vertices;
    uint64_t total_position_delta_fixed;
    uint64_t max_semantic_position_delta_fixed;
    uint64_t max_semantic_identity_scene;
    uint32_t max_semantic_identity_producer;
    uint32_t max_semantic_identity_primitive;
    bool max_semantic_identity_valid;
    uint64_t total_unkeyed_moved_matches;
    uint64_t total_unkeyed_motion_over_32px;
    uint64_t total_unkeyed_motion_over_64px;
    uint64_t total_unkeyed_motion_over_128px;
    uint64_t total_unkeyed_motion_over_192px;
    uint64_t total_unkeyed_motion_over_240px;
    uint64_t max_keyed_semantic_position_delta_fixed;
    uint64_t max_keyed_semantic_identity_scene;
    uint32_t max_keyed_semantic_identity_producer;
    uint32_t max_keyed_semantic_identity_primitive;
    uint64_t total_keyed_moved_matches;
    uint64_t total_keyed_motion_over_32px;
    uint64_t total_keyed_motion_over_64px;
    uint64_t total_keyed_motion_over_128px;
    uint64_t total_keyed_motion_over_192px;
    uint64_t total_keyed_motion_over_240px;
    uint64_t total_midpoint_distinct_vertices;
    uint64_t total_midpoint_collapsed_vertices;
    uint64_t total_midpoint_formula_failures;
    uint64_t total_projective_input_vertices;
    uint64_t total_projective_valid_input_vertices;
    uint64_t total_projective_phase_vertices;
    uint64_t total_previous_unmatched;
    uint64_t total_previous_unmatched_keyed;
    uint64_t total_previous_unmatched_projective;
    uint64_t total_retrospective_semitransparent_rejected;
    uint64_t total_eligible_frames;
    uint64_t total_rejected_no_previous_frames;
    uint64_t total_rejected_overflow_frames;
    uint64_t total_rejected_count_mismatch_frames;
    uint64_t total_rejected_incomplete_match_frames;
    uint64_t total_rejected_static_frames;
    uint64_t total_partial_count_mismatch_frames;
    uint64_t total_partial_incomplete_match_frames;
    size_t current_count;
    size_t previous_count;
    size_t current_participating_count;
    size_t previous_participating_count;
    size_t matched_count;
    size_t snapped_count;
    size_t ambiguous_count;
    size_t moved_count;
    size_t unkeyed_count;
    size_t exact_match_count;
    size_t exact_semitransparent_match_count;
    size_t source_geometry_match_count;
    size_t matched_vertex_count;
    size_t position_changed_vertex_count;
    uint64_t position_delta_fixed;
    size_t midpoint_distinct_vertex_count;
    size_t midpoint_collapsed_vertex_count;
    size_t midpoint_formula_failure_count;
    uint64_t retrospective_candidates;
    size_t retrospective_budget_exhausted;
    size_t retrospective_semitransparent_rejected;
    size_t last_seal_previous_count;
    size_t last_seal_current_count;
    size_t last_seal_previous_participating_count;
    size_t last_seal_current_participating_count;
    size_t last_seal_previous_unkeyed_count;
    size_t last_seal_current_unkeyed_count;
    size_t last_seal_matched_count;
    size_t last_seal_snapped_count;
    size_t last_seal_ambiguous_count;
    size_t last_seal_moved_count;
    size_t last_seal_exact_match_count;
    size_t last_seal_exact_semitransparent_match_count;
    size_t last_seal_previous_unmatched_count;
    size_t last_seal_previous_unmatched_keyed_count;
    size_t last_seal_previous_unmatched_projective_count;
    GpuSemanticWorkloadEligibility last_seal_eligibility;
    bool building;
    bool current_overflowed;
    bool last_seal_previous_overflowed;
    bool last_seal_current_overflowed;
    bool previous_usable;
} GpuSemanticWorkloadDiagnostics;

/* The module owns fixed storage for one process-wide display workload. */
void gpu_semantic_workload_reset(void);
GpuSemanticWorkloadStatus gpu_semantic_workload_begin(void);
GpuSemanticWorkloadStatus gpu_semantic_workload_record(
    const GpuRenderSemantic *semantic, GpuRenderSemantic *out_midpoint);
GpuSemanticWorkloadStatus gpu_semantic_workload_record_endpoint(
    const GpuRenderSemantic *semantic);
/* Records one current semantic and returns every requested retrospective phase.
 * Phase i uses alpha=(i+1)/denominator; phase_count must be denominator-1. */
GpuSemanticWorkloadStatus gpu_semantic_workload_record_phases(
    const GpuRenderSemantic *semantic, unsigned int denominator,
    GpuRenderSemantic *out_phases, size_t phase_count);
/* Temporal endpoints are retained in source history but never join the
 * authoritative current surface. If every generated phase is culled, mark the
 * just-recorded endpoint history-only before recording another semantic. */
GpuSemanticWorkloadStatus gpu_semantic_workload_record_temporal_phases(
    const GpuRenderSemantic *semantic, unsigned int denominator,
    GpuRenderSemantic *out_phases, size_t phase_count);
GpuSemanticWorkloadStatus
gpu_semantic_workload_mark_last_temporal_history_only(void);
GpuSemanticWorkloadStatus gpu_semantic_workload_record_anchors(
    const GpuRenderInterpolationVertexAnchor *anchors, size_t count);
GpuSemanticWorkloadStatus gpu_semantic_workload_seal(void);
/* Drops only the open source frame. The last sealed frame remains available
 * for a later source frame, so an empty display VBlank cannot span frames. */
GpuSemanticWorkloadStatus gpu_semantic_workload_discard_current(void);

bool gpu_semantic_workload_current_frame_has_work(void);
size_t gpu_semantic_workload_current_count(void);
bool gpu_semantic_workload_previous_frame_usable(void);

/* Returns the sealed current item at alpha 0.5. Unmatched items are copied
 * unchanged; matched positions and colors are averaged. UV stays current. */
GpuSemanticWorkloadStatus gpu_semantic_workload_interpolated(
    size_t index, GpuRenderSemantic *out_semantic);
GpuSemanticWorkloadStatus gpu_semantic_workload_interpolated_phase(
    size_t index, unsigned int numerator, unsigned int denominator,
    GpuRenderSemantic *out_semantic);
GpuSemanticWorkloadStatus gpu_semantic_workload_previous_order(
    const GpuRenderInterpolationIdentity *identity, size_t *out_previous_order);
GpuSemanticWorkloadStatus gpu_semantic_workload_match_info(
    const GpuRenderInterpolationIdentity *identity,
    GpuSemanticWorkloadMatchInfo *out_match);
GpuSemanticWorkloadStatus gpu_semantic_workload_current(
    const GpuRenderInterpolationIdentity *identity,
    GpuRenderSemantic *out_semantic);
GpuSemanticWorkloadStatus gpu_semantic_workload_previous(
    const GpuRenderInterpolationIdentity *identity,
    GpuRenderSemantic *out_semantic);
GpuSemanticWorkloadStatus gpu_semantic_workload_last_motion(
    GpuSemanticWorkloadMotionDiagnostics *out_motion);
size_t gpu_semantic_workload_retired_count(void);
typedef struct GpuSemanticWorkloadRetiredDiagnostics {
    size_t unmatched;
    size_t eligible;
    size_t scene_mismatch;
    size_t missing_anchor;
    size_t position_mode_mismatch;
    size_t material_position_mismatch;
    size_t anchor_overflow;
    uint32_t first_missing_primitive_id;
    uint32_t first_missing_group_id;
    uint32_t first_missing_vertex_id;
} GpuSemanticWorkloadRetiredDiagnostics;
typedef enum GpuSemanticWorkloadRetiredIssueReason {
    GPU_SEMANTIC_RETIRED_ISSUE_MISSING_ANCHOR = 1,
    GPU_SEMANTIC_RETIRED_ISSUE_SCENE_MISMATCH,
    GPU_SEMANTIC_RETIRED_ISSUE_POSITION_MODE_MISMATCH,
    GPU_SEMANTIC_RETIRED_ISSUE_MATERIAL_POSITION_MISMATCH,
    GPU_SEMANTIC_RETIRED_ISSUE_ANCHOR_OVERFLOW,
} GpuSemanticWorkloadRetiredIssueReason;
typedef struct GpuSemanticWorkloadRetiredIssue {
    uint64_t scene_id;
    uint32_t producer_id;
    uint32_t primitive_id;
    uint32_t group_id;
    uint32_t vertex_id;
    uint32_t previous_order;
    uint32_t reason;
} GpuSemanticWorkloadRetiredIssue;
void gpu_semantic_workload_retired_diagnostics(
    uint32_t producer_id, GpuSemanticWorkloadRetiredDiagnostics *out_diagnostics);
size_t gpu_semantic_workload_retired_issues(
    GpuSemanticWorkloadRetiredIssue *out_issues, size_t capacity);
GpuSemanticWorkloadStatus gpu_semantic_workload_retired(
    size_t retired_index, GpuRenderSemantic *out_semantic,
    size_t *out_previous_order);
GpuSemanticWorkloadStatus gpu_semantic_workload_retired_phases(
    size_t retired_index, unsigned int denominator,
    GpuRenderSemantic *out_phases, size_t phase_count,
    size_t *out_previous_order);

void gpu_semantic_workload_diagnostics(
    GpuSemanticWorkloadDiagnostics *out_diagnostics);

#ifdef __cplusplus
}
#endif

#endif
