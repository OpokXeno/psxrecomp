#ifndef PSX_GPU_GL_RENDERER_H
#define PSX_GPU_GL_RENDERER_H

/* GL backend context + present entry points, called from main.cpp's window
 * setup and present path when [video] renderer = "opengl".  The backend's
 * rasterization vtable is obtained separately via gl_backend_get()
 * (gpu_render.h).  SDL_Window is forward-declared (SDL typedefs it from this
 * same struct tag) so this header needs no SDL include. */

#include <stddef.h>
#include <stdint.h>

#include "gpu_render.h"

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

/* Create the GL context on a window made with SDL_WINDOW_OPENGL.
 * Returns 1 on success, 0 to fall back to the SDL_Renderer present path. */
int  gl_renderer_init_context(struct SDL_Window *win);

/* Set the GL swap interval / vsync mode (1=vsync, 0=immediate, -1=adaptive).
 * Safe before or after context creation; applies live when a context exists. */
void gl_renderer_set_swap_interval(int interval);
/* Semantic Native interpolation target. Accepted values: 0/default, 60, 120,
 * 240. Targets above 60 use immediate swaps plus main-context subframe pacing;
 * Wayland presentation feedback may phase-align that clock to physical retrace. */
int gl_renderer_set_native_interpolation_fps(int target_fps);
int gl_renderer_native_interpolation_fps(void);

/* Presentation-only frame interpolation. High-refresh sub-presents blend the
 * two most recent stable display images; guest simulation timing is unchanged. */
void gl_renderer_set_interpolation(int enabled, double host_hz, double target_hz,
                                   int blend_mode);
void gl_renderer_set_interpolation_suspended(int suspended);
void gl_renderer_interpolation_diag(int *enabled, int *suspended,
                                    int *history_frames,
                                    double *host_hz, double *target_hz,
                                    uint64_t *swaps);
/* Cumulative CPU-upload diagnostics: calls, rects, pixels, conversion ticks,
 * texture-upload ticks, FBO-draw ticks. Active only with PSX_RUNTIME_PERF_DIAG. */
void gl_renderer_runtime_diag(uint64_t out[6]);

/* Present an ARGB8888 image (BGRA byte order) as a letterboxed quad + swap.
 * Used for 24-bit (FMV) frames and the PSX_GL_FORCE_CPU_PRESENT diagnostic.
 * force_4_3 = pillarbox at native 4:3 even on a wide display aspect (FMVs
 * are authored 4:3 and get no GTE squash to compensate the stretch).
 * content_w: if 0 < content_w < src_w, only columns [0, content_w) are shown
 * (left-aligned in the letterbox; the rest stays cleared black). Used to hide
 * a trailing depth24 margin without changing CRTC width / stretching. */
void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h, int linear,
                         int force_4_3, int content_w);

/* Independent Native FMV surface. This path has its own upload surface and
 * draw operation and never calls gl_renderer_present(). */
int gl_renderer_present_native_cpu_frame(const uint32_t *pixels, int src_w,
                                         int src_h, int linear, int force_4_3,
                                         int content_w);

/* Clear to black + swap (display-disabled frame). */
void gl_renderer_present_blank(void);

/* §33: re-present the last Live frame captured before Swap (or from a VRAM
 * snapshot when interpolation owned the last present). Used during rollback
 * resim so the window keeps a wall-clock present cadence without reading
 * mid-resim VRAM. Returns 1 if a Swap happened, 0 if no hold is available. */
int gl_renderer_present_hold_last(void);

/* Sync the authoritative FBO down to CPU VRAM if the GPU side is ahead (else
 * a no-op). Screenshots and the debug server call this before reading CPU
 * VRAM. Do NOT use before 24-bit (FMV) scanout — a full readback can clobber
 * packed RGB888 MDEC bytes in the CPU mirror (use flush_cpu_uploads instead). */
void gl_renderer_sync_cpu(void);

/* Land pending CPU→VRAM uploads into the FBO without reading the FBO back.
 * Safe before 24-bit (FMV) CPU scanout of the mirror. */
void gl_renderer_flush_cpu_uploads(void);

/* Mark the whole display dirty, drop present-path latches, reset frame-
 * interpolation history, and force the next several SwapWindow calls even if
 * VRAM tiles match the last present. Call after savestate restore so a
 * reloaded identical frame still reaches the window (double/triple buffer). */
void gl_renderer_invalidate_present(void);

/* After savestate restore: push the CPU VRAM mirror into the GL FBO. Needed
 * when the load happened while GP1 depth24 was on — the normal path skips
 * framebuffer-sized uploads, which also skipped the full-VRAM boot_state
 * blit and left post-FMV menus without texture pages. */
void gl_renderer_restage_vram_after_savestate(void);

/* Netplay dual-raster: every GP0 also writes software VRAM @ 1× (snaps /
 * digests / GPUREAD authority) while the OpenGL hr FBO keeps settings-scale
 * SSAA for present-only. Never enables glReadPixels; CPU stays current. */
void gl_renderer_set_cpu_auth_dual(int on);
int  gl_renderer_cpu_auth_dual(void);

/* Post-savestate freeze probe: skip/swap/dirty-mark counters (GL present path).
 * take() returns deltas since the previous take/reset. Safe no-ops when GL is
 * inactive. rect_dirty tests the current present-tile dirty bits. */
void gl_renderer_present_probe_reset(void);
void gl_renderer_present_probe_take(uint64_t *skip_delta, uint64_t *swap_delta,
                                    uint64_t *dirty_mark_delta,
                                    int *force_remaining);
int  gl_renderer_present_rect_dirty(int disp_x, int disp_y, int w, int h);

/* THE present path for 15-bit frames: blit the display region straight from
 * the authoritative VRAM FBO into a letterboxed rect (no readback).
 * Deterministic — used for every 15-bit frame. linear = filter on scale.
 * force_4_3 pins to native 4:3 (15-bit MDEC FMV frames on a wide aspect).
 * A live transaction is rejected without being consumed; READY transactions
 * may be presented only by gl_renderer_swap_ready_transaction. */
int gl_renderer_present_vram(int disp_x, int disp_y, int w, int h, int linear,
                             int force_4_3);

typedef enum GlRendererTransactionSwapStatus {
    GL_RENDERER_TRANSACTION_SWAP_NOT_READY = 0,
    GL_RENDERER_TRANSACTION_SWAP_SUCCESS = 1,
} GlRendererTransactionSwapStatus;

/* Consume a transaction only after gr_commit_validate returned READY. On the
 * READY path the first SDL/GL/window operation is SDL_GL_SwapWindow; all
 * renderer publication and checkpoint disposal follow the call. SDL exposes
 * no swap result, so SUCCESS means the call returned under the context/window
 * ownership validated by commit. NOT_READY performs no SDL/GL/window operation
 * and leaves any open pre-READY checkpoint rollbackable. */
GlRendererTransactionSwapStatus gl_renderer_swap_ready_transaction(void);

/* Fail-closed recovery for the otherwise unreachable case where commit
 * returned READY but the explicit swap operation cannot consume it. Restores
 * and discards the backend checkpoint without swapping. Returns non-zero when
 * a READY transaction was consumed. */
int gl_renderer_cancel_ready_transaction(void);

/* GPU-direct native-wide present: blit the displayed buffer's wide FBO (key =
 * disp_x) straight to the window, no CPU readback. Returns 0 if no wide surface
 * exists for disp_x (caller falls back to the CPU readout path). */
int gl_renderer_present_wide_fbo(int disp_x, int disp_y, int disp_h, int linear);

/* Producer-driven Native widescreen. This is independent from gpu_ws_* and the
 * legacy wide mirror; only Native semantic draws populate these surfaces. */
int gl_renderer_configure_native_view(int enabled, int aspect_num,
                                      int aspect_den, int canonical_width,
                                      int canonical_height);
int gl_renderer_present_native_view(int disp_x, int disp_y, int disp_h,
                                    int linear);
int gl_renderer_native_view_width(void);

/* Native semantic-stream canonical presentation. Selects a host-only
 * midpoint/current FBO and swaps on the main GL context; it never starts the
 * legacy interpolation thread. */
int gl_renderer_present_native_midpoint(int disp_x, int disp_y, int w, int h,
                                        int linear, int force_4_3);

typedef enum GlRendererNativeMidpointCancelReason {
    GL_NATIVE_MIDPOINT_CANCEL_NONE = 0,
    GL_NATIVE_MIDPOINT_CANCEL_GENERIC,
    GL_NATIVE_MIDPOINT_CANCEL_WORKLOAD_RECORD,
    GL_NATIVE_MIDPOINT_CANCEL_REASON_COUNT,
} GlRendererNativeMidpointCancelReason;

typedef enum GlRendererNativeMidpointResetReason {
    GL_NATIVE_MIDPOINT_RESET_EXPLICIT = 0,
    GL_NATIVE_MIDPOINT_RESET_INITIALIZE,
    GL_NATIVE_MIDPOINT_RESET_SCALE_CHANGE,
    GL_NATIVE_MIDPOINT_RESET_FPS_CHANGE,
    GL_NATIVE_MIDPOINT_RESET_BLANK_PRESENT,
    GL_NATIVE_MIDPOINT_RESET_INVALIDATE_PRESENT,
    GL_NATIVE_MIDPOINT_RESET_SUSPENSION_CHANGE,
    GL_NATIVE_MIDPOINT_RESET_VIEW_FREE,
    GL_NATIVE_MIDPOINT_RESET_PENDING_CANONICAL_MISMATCH,
    GL_NATIVE_MIDPOINT_RESET_PENDING_VIEW_MISMATCH,
    GL_NATIVE_MIDPOINT_RESET_FRONTEND_HEADLESS,
    GL_NATIVE_MIDPOINT_RESET_FRONTEND_DEBUG_TURBO,
    GL_NATIVE_MIDPOINT_RESET_FRONTEND_TURBO_SKIP,
    GL_NATIVE_MIDPOINT_RESET_FRONTEND_LOAD_SKIP,
    GL_NATIVE_MIDPOINT_RESET_FRONTEND_FMV_SKIP,
    GL_NATIVE_MIDPOINT_RESET_FRONTEND_NETPLAY_SKIP,
    GL_NATIVE_MIDPOINT_RESET_FRONTEND_DEPTH24_HOLD,
    GL_NATIVE_MIDPOINT_RESET_FRONTEND_NON_NATIVE_WIDE,
    GL_NATIVE_MIDPOINT_RESET_FRONTEND_TRANSACTION,
    GL_NATIVE_MIDPOINT_RESET_FRONTEND_NON_NATIVE_STREAM,
    GL_NATIVE_MIDPOINT_RESET_FRONTEND_CPU_PRESENT,
    GL_NATIVE_MIDPOINT_RESET_REASON_COUNT,
} GlRendererNativeMidpointResetReason;

typedef struct GlRendererNativeMidpointDiagnostics {
    uint32_t target_fps;
    uint32_t phase_count;
    uint64_t begun_frames;
    uint64_t sealed_frames;
    uint64_t midpoint_presents;
    uint64_t current_presents;
    uint64_t midpoint_candidates;
    uint64_t midpoint_duplicate_empty_frames;
    uint64_t midpoint_duplicate_static_frames;
    uint64_t midpoint_eligible_without_duplicate_frames;
    uint64_t midpoint_ineligible_after_duplicate_frames;
    uint64_t midpoint_ineligible_without_duplicate_frames;
    uint64_t midpoint_candidate_pending_current;
    uint64_t midpoint_candidate_canonical_disabled;
    uint64_t midpoint_candidate_view_unseeded;
    uint64_t eligibility_complete_frames;
    uint64_t eligibility_partial_count_mismatch_frames;
    uint64_t eligibility_partial_incomplete_match_frames;
    uint64_t eligibility_no_previous_frames;
    uint64_t eligibility_overflow_frames;
    uint64_t eligibility_count_mismatch_frames;
    uint64_t eligibility_incomplete_match_frames;
    uint64_t eligibility_static_frames;
    uint64_t deferred_current_frames;
    uint64_t deferred_current_flushes;
    uint64_t retired_candidate_count;
    uint64_t retired_inserted_count;
    uint64_t retired_history_miss_count;
    uint64_t retired_capacity_miss_count;
    uint64_t retired_phase_failure_count;
    uint64_t retired_producer_history_recovery_count;
    uint64_t retired_world_model_candidate_count;
    uint64_t retired_world_model_inserted_count;
    uint64_t retired_world_model_history_miss_count;
    uint64_t retired_world_model_history_recovery_count;
    uint64_t retired_world_model_producer_context_recovery_count;
    uint64_t retired_world_model_class_context_recovery_count;
    uint64_t retired_terrain_unmatched_count;
    uint64_t retired_terrain_eligible_count;
    uint64_t retired_terrain_missing_current_geometry_count;
    uint64_t retired_terrain_missing_anchor_count;
    uint64_t retired_terrain_scene_mismatch_count;
    uint64_t retired_terrain_position_mode_mismatch_count;
    uint64_t retired_terrain_material_position_mismatch_count;
    uint64_t retired_terrain_anchor_overflow_count;
    uint64_t retired_terrain_candidate_count;
    uint64_t retired_terrain_inserted_count;
    uint64_t retired_terrain_history_miss_count;
    uint64_t retired_terrain_history_recovery_count;
    uint32_t first_retired_terrain_missing_primitive;
    uint32_t first_retired_terrain_missing_group;
    uint32_t first_retired_terrain_missing_vertex;
    uint32_t last_retired_phase_failure_producer;
    uint32_t last_retired_phase_failure_primitive;
    uint32_t last_retired_history_miss_producer;
    uint32_t last_retired_history_miss_primitive;
    uint64_t host_queue_flush_reasons[9];
    uint64_t reset_count;
    uint64_t reset_with_previous_count;
    uint64_t reset_with_pending_count;
    uint64_t reset_reason_counts[GL_NATIVE_MIDPOINT_RESET_REASON_COUNT];
    uint64_t reset_with_previous_reason_counts[
        GL_NATIVE_MIDPOINT_RESET_REASON_COUNT];
    uint32_t last_reset_reason;
    uint64_t pending_mismatch_slot_count;
    uint64_t pending_mismatch_x_count;
    uint64_t pending_mismatch_y_count;
    uint64_t pending_mismatch_width_count;
    uint64_t pending_mismatch_height_count;
    uint64_t pending_vertical_lag_count;
    int last_pending_slot;
    int last_pending_x;
    int last_pending_y;
    int last_pending_width;
    int last_pending_height;
    int last_present_slot;
    int last_present_x;
    int last_present_y;
    int last_present_width;
    int last_present_height;
    uint64_t cancelled_frames;
    uint64_t cancel_reason_counts[GL_NATIVE_MIDPOINT_CANCEL_REASON_COUNT];
    uint32_t last_cancel_reason;
    uint32_t last_cancel_status;
    uint64_t last_cancel_workload_current;
    uint64_t last_cancel_identity_scene;
    uint32_t last_cancel_identity_producer;
    uint32_t last_cancel_identity_primitive;
    int last_cancel_identity_valid;
    uint64_t last_cancel_existing_command_id;
    uint64_t last_cancel_current_command_id;
    uint64_t workload_epoch;
    uint64_t workload_recorded;
    uint64_t workload_total_matched;
    uint64_t workload_total_snapped;
    uint64_t workload_total_ambiguous;
    uint64_t workload_total_moved;
    uint64_t workload_total_unkeyed;
    uint64_t workload_total_exact_matches;
    uint64_t workload_total_exact_semitransparent_matches;
    uint64_t workload_total_source_geometry_matches;
    uint64_t workload_total_matched_vertices;
    uint64_t workload_total_position_changed_vertices;
    uint64_t workload_total_position_delta_fixed;
    uint64_t workload_max_semantic_position_delta_fixed;
    uint64_t workload_max_semantic_identity_scene;
    uint32_t workload_max_semantic_identity_producer;
    uint32_t workload_max_semantic_identity_primitive;
    int workload_max_semantic_identity_valid;
    uint64_t workload_total_unkeyed_moved_matches;
    uint64_t workload_total_unkeyed_motion_over_32px;
    uint64_t workload_total_unkeyed_motion_over_64px;
    uint64_t workload_total_unkeyed_motion_over_128px;
    uint64_t workload_total_unkeyed_motion_over_192px;
    uint64_t workload_total_unkeyed_motion_over_240px;
    uint64_t workload_max_keyed_semantic_position_delta_fixed;
    uint64_t workload_max_keyed_semantic_identity_scene;
    uint32_t workload_max_keyed_semantic_identity_producer;
    uint32_t workload_max_keyed_semantic_identity_primitive;
    uint64_t workload_total_keyed_moved_matches;
    uint64_t workload_total_keyed_motion_over_32px;
    uint64_t workload_total_keyed_motion_over_64px;
    uint64_t workload_total_keyed_motion_over_128px;
    uint64_t workload_total_keyed_motion_over_192px;
    uint64_t workload_total_keyed_motion_over_240px;
    uint64_t workload_total_midpoint_distinct_vertices;
    uint64_t workload_total_midpoint_collapsed_vertices;
    uint64_t workload_total_midpoint_formula_failures;
    uint64_t workload_total_projective_input_vertices;
    uint64_t workload_total_projective_valid_input_vertices;
    uint64_t workload_total_projective_phase_vertices;
    uint64_t temporal_candidate_count;
    uint64_t temporal_candidate_recorded_count;
    uint64_t temporal_candidate_visible_count;
    uint64_t temporal_candidate_record_failure_count;
    uint64_t temporal_candidate_duplicate_count;
    uint64_t temporal_candidate_identity_collision_count;
    uint64_t temporal_candidate_peak_workload_count;
    uint64_t temporal_candidate_first_failure_workload_count;
    uint32_t temporal_candidate_first_failure_status;
    uint32_t temporal_candidate_first_failure_producer;
    uint32_t temporal_candidate_first_failure_primitive;
    uint64_t workload_total_previous_unmatched;
    uint64_t workload_total_previous_unmatched_keyed;
    uint64_t workload_total_previous_unmatched_projective;
    uint64_t workload_total_retrospective_semitransparent_rejected;
    uint64_t workload_total_eligible_frames;
    uint64_t workload_total_rejected_no_previous_frames;
    uint64_t workload_total_rejected_overflow_frames;
    uint64_t workload_total_rejected_count_mismatch_frames;
    uint64_t workload_total_rejected_incomplete_match_frames;
    uint64_t workload_total_rejected_static_frames;
    uint64_t workload_total_partial_count_mismatch_frames;
    uint64_t workload_total_partial_incomplete_match_frames;
    uint64_t workload_current;
    uint64_t workload_previous;
    uint64_t workload_matched;
    uint64_t workload_snapped;
    uint64_t workload_ambiguous;
    uint64_t workload_moved;
    uint64_t workload_unkeyed;
    uint64_t workload_exact_matches;
    uint64_t workload_exact_semitransparent_matches;
    uint64_t workload_source_geometry_matches;
    uint64_t workload_matched_vertices;
    uint64_t workload_position_changed_vertices;
    uint64_t workload_position_delta_fixed;
    uint64_t workload_midpoint_distinct_vertices;
    uint64_t workload_midpoint_collapsed_vertices;
    uint64_t workload_midpoint_formula_failures;
    uint64_t presented_midpoint_matched_vertices;
    uint64_t presented_midpoint_position_changed_vertices;
    uint64_t presented_midpoint_distinct_vertices;
    uint64_t presented_midpoint_collapsed_vertices;
    uint64_t presented_midpoint_formula_failures;
    uint64_t presented_midpoint_position_delta_fixed;
    uint64_t workload_retrospective_candidates;
    uint64_t workload_retrospective_budget_exhausted;
    uint64_t workload_retrospective_semitransparent_rejected;
    uint64_t workload_last_previous;
    uint64_t workload_last_current;
    uint64_t workload_last_previous_unkeyed;
    uint64_t workload_last_current_unkeyed;
    uint64_t workload_last_matched;
    uint64_t workload_last_snapped;
    uint64_t workload_last_ambiguous;
    uint64_t workload_last_moved;
    uint64_t workload_last_exact_matches;
    uint64_t workload_last_exact_semitransparent_matches;
    uint64_t workload_last_previous_unmatched;
    uint64_t workload_last_previous_unmatched_keyed;
    uint64_t workload_last_previous_unmatched_projective;
    uint32_t workload_last_eligibility;
    int workload_last_previous_overflowed;
    int workload_last_current_overflowed;
    uint64_t nonsemantic_uploads;
    uint64_t nonsemantic_fills;
    uint64_t nonsemantic_margin_clears;
    uint64_t nonsemantic_copies;
    uint64_t gl_error_count;
    uint32_t last_gl_error;
    uint32_t last_gl_operation;
    int current_pending_present;
    int frame_open;
    int frame_valid;
    int suspended;
    int previous_usable;
} GlRendererNativeMidpointDiagnostics;

typedef struct GlRendererNativeWaveDiagnostics {
    uint64_t semantics;
    uint64_t starts;
    uint64_t completed;
    uint64_t target_resets;
    uint64_t row_resets;
    uint64_t invalid_row_resets;
    uint64_t matching_copies;
    uint64_t ready_copies;
    uint64_t partial_copies;
    uint64_t apply_successes;
    uint64_t apply_failures;
    uint64_t margin_clears;
    uint64_t presents;
    uint64_t ready_copies_by_page[2];
    uint64_t partial_copies_by_page[2];
    uint64_t margin_clears_by_page[2];
    uint64_t presents_by_page[2];
    uint64_t presents_with_wave_by_page[2];
    int32_t last_copy_dst_y;
    int32_t last_copy_packet_count;
    int32_t last_copy_row_count;
    int32_t last_present_y;
    int32_t current_packet_count;
    int32_t current_row_count;
    int32_t current_base_x;
    int32_t current_slot;
    int wave_valid_by_page[2];
    int current_recording;
    int current_ready;
} GlRendererNativeWaveDiagnostics;

typedef struct GlRendererSemanticProducerDiagnostics {
    uint32_t producer_id;
    uint64_t semantic_count;
    uint64_t midpoint_semantic_count;
    uint64_t primitive_count;
    uint64_t static_primitive_count;
    uint64_t fully_moving_primitive_count;
    uint64_t partially_moving_primitive_count;
    uint64_t matched_order_count;
    uint64_t previous_order_inversion_count;
    uint64_t max_previous_order_regression;
    uint64_t vertex_count;
    uint64_t duplicate_vertex_count;
    uint64_t exact_vertex_conflict_count;
    uint64_t raster_vertex_conflict_count;
    uint64_t retired_candidates;
    uint64_t retired_unmatched;
    uint64_t retired_missing_current_geometry;
    uint64_t retired_inserted;
    uint64_t retired_skipped_history;
    uint64_t retired_skipped_capacity;
    uint64_t max_midpoint_delta_fixed;
    uint32_t max_midpoint_primitive_id;
} GlRendererSemanticProducerDiagnostics;

typedef struct GlRendererSemanticProducerItemDiagnostics {
    uint64_t frame;
    uint64_t scene_id;
    uint32_t producer_id;
    uint32_t primitive_id;
    uint32_t identity_valid;
    uint32_t queue_order;
    int32_t base_x;
    int32_t slot;
    uint32_t current_order;
    uint32_t previous_order;
    uint32_t match_kind;
    uint32_t fallback_kind;
    uint32_t subprimitive_index;
    uint32_t topology;
    uint32_t screen_space_2d;
    uint32_t world_model;
    uint32_t tpage;
    uint32_t clut_x;
    uint32_t clut_y;
    int32_t draw_offset_x;
    int32_t draw_offset_y;
    uint32_t draw_area[4];
    uint32_t textured;
    uint32_t raw_texture;
    uint32_t semi_transparent;
    uint32_t moving_vertex_count;
    uint64_t midpoint_delta_fixed;
    int64_t current_area;
    int64_t midpoint_area;
    int raw_bounds[4];
    int uv_bounds[4];
    int current_bounds[4];
    int midpoint_bounds[4];
    int previous_order_valid;
} GlRendererSemanticProducerItemDiagnostics;

typedef enum GlRendererRetiredFailureReason {
    GL_RETIRED_FAILURE_MISSING_ANCHOR = 1,
    GL_RETIRED_FAILURE_SCENE_MISMATCH,
    GL_RETIRED_FAILURE_POSITION_MODE_MISMATCH,
    GL_RETIRED_FAILURE_MATERIAL_POSITION_MISMATCH,
    GL_RETIRED_FAILURE_ANCHOR_OVERFLOW,
    GL_RETIRED_FAILURE_HISTORY_MISS,
    GL_RETIRED_FAILURE_CAPACITY,
    GL_RETIRED_FAILURE_PHASE,
    GL_RETIRED_FAILURE_MIDPOINT_ZERO_AREA,
    GL_RETIRED_FAILURE_MIDPOINT_EXTENT_COLLAPSE,
    GL_RETIRED_FAILURE_MIDPOINT_WINDING_FLIP,
    GL_RETIRED_FAILURE_FRONT_ORDER_DISPLACEMENT,
    GL_RETIRED_FAILURE_MIDPOINT_VERTEX_CONFLICT,
    GL_RETIRED_FAILURE_MIDPOINT_FIXED_ZERO_AREA,
    GL_RETIRED_FAILURE_MIDPOINT_FIXED_WINDING_FLIP,
} GlRendererRetiredFailureReason;

typedef struct GlRendererRetiredFailureEvent {
    uint64_t frame;
    uint64_t scene_id;
    uint32_t reason;
    uint32_t producer_id;
    uint32_t primitive_id;
    uint32_t group_id;
    uint32_t vertex_id;
    uint32_t previous_order;
    uint32_t auxiliary;
    int64_t value_a;
    int64_t value_b;
    int32_t current_x[3];
    int32_t current_y[3];
    int32_t midpoint_x[3];
    int32_t midpoint_y[3];
    int32_t current_z[3];
    int32_t midpoint_z[3];
    int32_t current_edge_distance;
    int32_t midpoint_edge_distance;
    int32_t surface_width;
    int32_t base_x;
    int32_t slot;
} GlRendererRetiredFailureEvent;

enum {
    GL_NATIVE_MIDPOINT_GL_SEED_CANONICAL = 1,
    GL_NATIVE_MIDPOINT_GL_SEED_VIEW,
    GL_NATIVE_MIDPOINT_GL_MIRROR_RECTS,
    GL_NATIVE_MIDPOINT_GL_DRAW_CANONICAL,
    GL_NATIVE_MIDPOINT_GL_DRAW_VIEW,
    GL_NATIVE_MIDPOINT_GL_FILL_VIEW,
    GL_NATIVE_MIDPOINT_GL_COPY_CANONICAL,
    GL_NATIVE_MIDPOINT_GL_COPY_VIEW,
    GL_NATIVE_MIDPOINT_GL_SNAPSHOT_CURRENT,
    GL_NATIVE_MIDPOINT_GL_WAVE_COPY,
};

/* Native-view midpoint lifecycle. All operations execute on the main GL
 * context; suspension resets history and keeps FMV/depth24 cadence authored. */
int gl_renderer_native_midpoint_begin(void);
int gl_renderer_native_midpoint_seal(void);
void gl_renderer_native_midpoint_cancel(void);
void gl_renderer_native_midpoint_reset(void);
void gl_renderer_native_midpoint_reset_for_reason(
    GlRendererNativeMidpointResetReason reason);
void gl_renderer_native_midpoint_set_suspended(int suspended);
void gl_renderer_native_midpoint_diag(
    GlRendererNativeMidpointDiagnostics *out_diagnostics);
void gl_renderer_native_wave_diag(
    GlRendererNativeWaveDiagnostics *out_diagnostics);
void gl_renderer_semantic_producer_diag(
    uint32_t producer_id,
    GlRendererSemanticProducerDiagnostics *out_diagnostics);
size_t gl_renderer_semantic_producer_items(
    uint32_t producer_id, uint64_t frame, size_t offset,
    GlRendererSemanticProducerItemDiagnostics *out_items, size_t capacity,
    size_t *out_total, uint64_t *out_frame);
size_t gl_renderer_retired_failure_events(
    GlRendererRetiredFailureEvent *out_events, size_t capacity);
uint64_t gl_renderer_retired_failure_event_total(void);
uint64_t gl_renderer_retired_failure_event_overflow(void);
GpuRenderTransactionStatus gl_renderer_record_interpolation_anchors(
    const GpuRenderInterpolationVertexAnchor *anchors, size_t count);

/* Display aspect for the present letterbox (default 4:3). A wide aspect
 * stretches the 4:3 frame; pair with gte_set_display_aspect (cpu_state.h)
 * for the widescreen field-of-view hack. */
void gl_renderer_set_display_aspect(int num, int den);

/* Select full native-wide mirror rendering instead of the centre-splice fast
 * path. Textured edge expansion needs the complete mirror surface. */
void gl_renderer_set_wide_fast(int on);

void gl_renderer_shutdown(void);

/* Diagnostics (debug server): read GPU-side VRAM without touching the CPU
 * array; report coherency flags + dirty rects. fbo_peek returns 0 when the
 * GL pipeline is inactive. */
int  gl_renderer_fbo_peek(int x, int y, int w, int h, uint16_t *out);
int  gl_renderer_native_view_peek(int base_x, int x, int y,
                                   int w, int h, uint16_t *out);
int  gl_renderer_native_view_center_diff(uint32_t *count, int bbox[4],
                                          int samples[8][2],
                                          uint16_t samples_px[8][2]);
int  gl_renderer_native_view_phase_peek(int base_x, unsigned int phase,
                                        int x, int y, int w, int h,
                                        uint16_t *out);
void gl_renderer_diag(int *gpu_dirty, int pending[5], int pack[5]);

/* Always-on coherency event ring (debug server "gl_coh_ring"): every upload
 * flush, fill, copy, draw bbox, pack, full readback, present, and probe
 * perturbation, with rect + frame. An op that flushes internally records its
 * own event AFTER the FLUSH it caused (the event after a FLUSH = trigger). */
enum {
    GL_COH_FLUSH    = 1,   /* CPU->FBO upload flush (pending box)     */
    GL_COH_FILL     = 2,   /* GP0(02) fill rect                       */
    GL_COH_COPY_SRC = 3,   /* GP0(80) copy, source rect               */
    GL_COH_COPY     = 4,   /* GP0(80) copy, dest rect                 */
    GL_COH_DRAW     = 5,   /* drawn prim bbox (clipped to draw area)  */
    GL_COH_PACK     = 6,   /* hr FBO -> raw mirror pack (dirty box)   */
    GL_COH_ENSURE   = 7,   /* full FBO -> CPU VRAM readback           */
    GL_COH_PRESENT  = 8,   /* 15-bit present blit (display rect)      */
    GL_COH_UPLOAD   = 9,   /* bulk CPU->VRAM transfer_in dest rect    */
    GL_COH_PEEK     = 10,  /* gl_fbo_peek probe (perturbs: flushes)   */
    GL_COH_DIFF     = 11,  /* gl_vram_diff probe (perturbs: flushes)  */
};

typedef struct {
    uint32_t frame;
    uint8_t  kind;
    int16_t  x0, y0, x1, y1;   /* native VRAM coords, inclusive */
} GlCohEvent;

uint64_t gl_renderer_coh_total(void);
/* Fetch event by absolute sequence number; 0 if evicted or out of range. */
int gl_renderer_coh_get(uint64_t seq, GlCohEvent *out);

/* Always-on present ring (debug server "gl_present_ring"): EVERY SwapWindow —
 * including blank (display-disabled) and CPU-quad presents, which the coherency
 * ring does not record — with the path taken, source display rect, letterbox
 * dest rect, a glGetError sample, wall-clock ms, and a backbuffer pixel sampled
 * at the letterbox centre right before the swap. Each entry is marked completed
 * only after its exact SDL_GL_SwapWindow call returns. Native semantic swaps
 * may additionally carry an opt-in full composed-framebuffer hash and Wayland
 * presentation feedback proving whether the compositor displayed or discarded
 * that exact surface commit. */
enum {
    GL_PRES_VRAM  = 0,   /* 15-bit FBO blit present (gl_renderer_present_vram) */
    GL_PRES_WIDE  = 1,   /* native-wide FBO blit present                       */
    GL_PRES_CPU   = 2,   /* CPU-readout quad present (24-bit FMV / forced)     */
    GL_PRES_BLANK = 3,   /* display-disabled black present                     */
    GL_PRES_INTERP = 4,  /* host-refresh interpolation sub-present              */
    GL_PRES_NATIVE_CURRENT = 5,  /* native semantic stream, current FBO swap   */
    GL_PRES_NATIVE_MIDPOINT = 6, /* native semantic stream, midpoint FBO swap  */
};

typedef enum GlNativeMidpointDecision {
    GL_NATIVE_MIDPOINT_DECISION_UNKNOWN = 0,
    GL_NATIVE_MIDPOINT_DECISION_SELECTED,
    GL_NATIVE_MIDPOINT_DECISION_PENDING_CURRENT,
    GL_NATIVE_MIDPOINT_DECISION_SUSPENDED,
    GL_NATIVE_MIDPOINT_DECISION_NO_OPEN_FRAME,
    GL_NATIVE_MIDPOINT_DECISION_EMPTY_DUPLICATE,
    GL_NATIVE_MIDPOINT_DECISION_SEAL_CANCELLED,
    GL_NATIVE_MIDPOINT_DECISION_STATIC_DUPLICATE,
    GL_NATIVE_MIDPOINT_DECISION_ELIGIBLE_WITHOUT_DUPLICATE,
    GL_NATIVE_MIDPOINT_DECISION_INELIGIBLE_AFTER_DUPLICATE,
    GL_NATIVE_MIDPOINT_DECISION_INELIGIBLE_WITHOUT_DUPLICATE,
    GL_NATIVE_MIDPOINT_DECISION_CANONICAL_DISABLED,
    GL_NATIVE_MIDPOINT_DECISION_CANDIDATE_PENDING_CURRENT,
    GL_NATIVE_MIDPOINT_DECISION_VIEW_UNSEEDED,
    GL_NATIVE_MIDPOINT_DECISION_COUNT,
} GlNativeMidpointDecision;

#define GL_PRES_RING_CAPACITY 8192u

typedef struct {
    uint32_t frame;        /* s_frame_count at swap                        */
    uint32_t t_ms;         /* SDL_GetTicks() at swap                       */
    uint8_t  path;         /* GL_PRES_*                                    */
    uint8_t  px_r, px_g, px_b; /* backbuffer sample at letterbox centre    */
    uint16_t glerr;        /* glGetError() drained just before the swap    */
    int16_t  dx, dy, w, h; /* source display rect (native px; 0 for blank) */
    int16_t  lx, ly, lw, lh; /* letterbox dest rect (window px)            */
    uint8_t  src_r, src_g, src_b, src_valid; /* blit SOURCE (hr FBO) sample
                                              * at the display-rect centre  */
    uint8_t  swap_completed; /* set only after SDL_GL_SwapWindow returns   */
    uint8_t  phase_numerator;   /* 0 for current/non-semantic presents     */
    uint8_t  phase_denominator; /* 0 for current/non-semantic presents     */
    uint8_t  framebuffer_hash_valid;
    uint8_t  presentation_feedback; /* 0=pending/unavailable, 1=presented, 2=discarded */
    uint8_t  midpoint_decision;   /* GlNativeMidpointDecision */
    uint8_t  midpoint_eligibility; /* GpuSemanticWorkloadEligibility */
    uint64_t framebuffer_hash;
    uint64_t presentation_time_ns;
    uint64_t refresh_sequence;
    uint32_t refresh_ns;
    uint32_t presentation_flags;
    uint8_t  source_hash_valid;
    uint8_t  source_hash_reserved[7];
    uint64_t source_hash;
    uint8_t  geometry_hash_valid;
    uint8_t  geometry_hash_reserved[7];
    uint64_t geometry_hash;
    uint8_t  phase_surface_hash_valid;
    uint8_t  phase_surface_hash_reserved[7];
    uint64_t phase_surface_hash;
    uint8_t  phase_vram_hash_valid;
    uint8_t  phase_vram_hash_reserved[7];
    uint64_t phase_vram_hash;
    int16_t  scanout_dx, scanout_dy, scanout_w, scanout_h;
} GlPresEvent;

uint64_t gl_renderer_pres_total(void);
int gl_renderer_pres_get(uint64_t seq, GlPresEvent *out);

typedef struct GlRendererPresentationDiagnostics {
    uint64_t hash_requested;
    uint64_t hash_completed;
    uint64_t hash_dropped;
    uint64_t source_hash_requested;
    uint64_t source_hash_completed;
    uint64_t source_hash_dropped;
    uint64_t phase_surface_hash_requested;
    uint64_t phase_surface_hash_completed;
    uint64_t phase_surface_hash_dropped;
    uint64_t phase_vram_hash_requested;
    uint64_t phase_vram_hash_completed;
    uint64_t phase_vram_hash_dropped;
    uint64_t feedback_requested;
    uint64_t feedback_presented;
    uint64_t feedback_discarded;
    uint64_t feedback_pending;
    uint32_t presentation_clock_id;
    int hash_enabled;
    int wayland_window;
    int presentation_protocol_available;
} GlRendererPresentationDiagnostics;

void gl_renderer_presentation_diagnostics(
    GlRendererPresentationDiagnostics *out_diagnostics);

/* frame_perf: aggregate the per-frame GPU/CPU phase-timing ring (debug server
 * "frame_perf"). wide_filter: -1 = all frames, 0 = 4:3 present, 1 = native-wide.
 * Fills out[13]: [0]=count, [1]=total_ms avg, [2]=total_ms max, [3]=emu_cpu_ms avg
 * (frame minus the present call), [4]=present_wall_ms avg, [5]=scene_gpu_ms avg,
 * [6]=scene_gpu_ms max, [7]=present_gpu_ms avg, [8]=present_gpu_ms max,
 * [9]=scene primitives/frame avg (pre double-draw), [10]=mirror_gpu_ms avg (of
 * scene_gpu, the native-wide mirror passes; GL_TIMESTAMP pairs), [11]=mirror_gpu_ms
 * max, [12]=mirror passes/frame avg, [13]=CPU wall in flush_tex_batch avg,
 * [14]=CPU wall in glb_wide_* avg, [15]=batches/frame avg, [16]=wide target
 * sets/frame avg, [17]=wide FBO creations/frame avg. GPU phases are true
 * GL_TIME_ELAPSED times (CPU-overhead independent). Returns the count. */
int gl_renderer_perf_aggregate(int wide_filter, double out[18]);

/* Native-wide mirror ablation (perf attribution, debug cmd gl_ws_ablate):
 * 0 = normal, 1 = skip the whole mirror pass (incl. wide_clear), 2 = full mirror
 * state churn without the draw calls, 3 = mirror draws stay on the hr FBO (no
 * per-pass FBO rebind; diagnostic only — corrupts both surfaces' content). */
void gl_renderer_set_ws_ablate(int mode);
int  gl_renderer_get_ws_ablate(void);

/* Cumulative textured fraction of scene primitives since boot (flat vs textured
 * batching decision). Sets *out_tex_frac; returns total prim count. */
uint64_t gl_renderer_perf_prim_split(double *out_tex_frac);
/* Cumulative textured-batch diagnostics: total, then flushes caused by
 * isolation, blend-mode, mask, filter, backdrop-gate, texture-window, capacity. */
void gl_renderer_batch_diag(uint64_t out[8]);

#ifdef PSX_GL_TRANSACTION_TESTING
enum {
    GL_TRANSACTION_FAULT_NONE = 0,
    GL_TRANSACTION_FAULT_POST_COMPOSITION,
    GL_TRANSACTION_FAULT_FINAL_VALIDATION,
    GL_TRANSACTION_FAULT_FINAL_BLIT,
};

typedef struct GlRendererTransactionTestDiag {
    uint64_t commits_ready;
    uint64_t staging_compositions;
    uint64_t default_writes_before_final_blit;
    uint64_t final_blits;
    uint64_t swaps;
    uint64_t publications;
    uint64_t phase_failures;
    uint64_t forced_original_presents;
    uint64_t operations_after_final_validation;
    uint64_t deferred_candidate_captures;
    uint64_t deferred_candidate_discards;
    uint64_t deferred_transaction_begins;
    int pending_commit;
    int deferred_candidate_active;
    int last_fault_phase;
} GlRendererTransactionTestDiag;

void gl_renderer_transaction_test_reset(void);
void gl_renderer_transaction_test_inject_fault(int phase);
void gl_renderer_transaction_test_diag(GlRendererTransactionTestDiag *out);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_GL_RENDERER_H */
