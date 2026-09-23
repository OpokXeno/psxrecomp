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
#include "../../../native_renderer/include/xg_render_semantic_presentation.h"

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

/* Create the GL context on a window made with SDL_WINDOW_OPENGL.
 * Returns 1 on success, 0 to fall back to the SDL_Renderer present path. */
int  gl_renderer_init_context(struct SDL_Window *win);
/* Select a retained immutable bank for the next textured submission; zero
 * selects live VRAM. Emulation/GL owning thread only. Returns 0 if unavailable. */
int gl_renderer_select_texture_bank(uint16_t id);
int gl_renderer_texture_banks_supported(void);

/* Native OpenGL transport.  init_services must run on the thread which owns
 * s_ctx, while that context is current; it creates a shared presenter context
 * and fills callbacks suitable for
 * XgRenderWorkerServices/XgRenderPresenterServices.  capture_source only
 * authenticates the sealed SourceCommit header.  It performs no GL work and
 * captures no compatibility framebuffer.  The worker later consumes every
 * immutable commit record through the SourceCommit copy API and rasterizes to
 * owned CPU staging. Only the presenter uploads/draws that immutable endpoint.
 *
 * Hold uses the existing compose(endpoint, 1, 1) service repeatedly while the
 * core keeps the endpoint's original ownership reference. Each successful
 * compose returns a separate completion fence for the core to retire. The core
 * resolves each presentation attempt before the next compose and releases the
 * endpoint exactly once on replacement/invalidation, after retiring its fences.
 * Ready textures are reused; failed composition does not consume the endpoint.
 * Native work publishes up to seven complete source-motion phases, n/(N+1)
 * for n=1..N, plus the authored endpoint at 1/1. Equivalent exact fractions are
 * accepted. N comes from the nearest integer cadence ratio between the observed
 * source interval and captured display.temporal_hz, never the monitor rate.
 * temporal_hz=0 disables smoothing before any replay/evaluation/phase allocation.
 * All phase CPU buffers are complete before endpoint/fence publication; GL
 * textures/FBOs are lazily uploaded by the presenter, never by the CPU worker.
 * Upload state is published under the endpoint lock and failed uploads retain
 * staging for retry. Phases share the endpoint's ownership/fence lifetime.
 *
 * Core schedules increasing alpha using services.clock_ns (SDL_GetTicksNS) over
 * temporal_interval_ns. Use phases only with a retained previous pixel_digest
 * equal to temporal_previous_pixel_digest and matching epoch/scene/layout.
 * Otherwise present 1/1. Zero phases authorize no temporal deferral, artificial
 * deadline or wait for a future endpoint. Pixel equality is not motion state.
 * A captured rate transition resets history and expires retained phase images;
 * their authored endpoints can still be held. It does not reset canonical/VIEW.
 *
 * The presentation host must be joined before native_shutdown.  The general
 * gl_renderer_shutdown path also calls it, after which legacy main-thread
 * presentation may own SwapWindow again. */
int gl_renderer_native_init_services(
    XgRenderWorkerServices *out_worker_services,
    XgRenderPresenterServices *out_presenter_services);
int gl_renderer_native_capture_source(
    XgRenderSourceCommitHandle commit,
    const XgRenderSourceCommitHeader *sealed_header);
void gl_renderer_native_shutdown(void);
/* Main owner only, before the host pump and during yield/EOF waits. Performs
 * bounded renderer submission/fence service, never swaps or advances the guest.
 * -1: renderer failure; 0: no progress; 1: progress, notify the render worker.
 * Must also be called when no endpoint is presentable. */
int gl_renderer_native_service(void);
void gl_renderer_native_set_worker_notify(void (*notify)(void *), void *data);
void gl_renderer_native_stop_gpu_worker(void);
/* Cheap request query; does not enter GL or execute renderer work. */
int gl_renderer_native_service_pending(void);

/* Independent Native visual-truth path. When PSX_NATIVE_VISUAL_TRUTH=1 the
 * OpenGL backend also maintains its existing software 1x canonical mirror.
 * The VBlank seam snapshots that guest scanout before source publication; the
 * Native worker later compares it, by full presentation identity, with the
 * independently compiled endpoint. */
int gl_renderer_native_guest_reference_enabled(void);
int gl_renderer_native_capture_guest_reference(
    uint64_t guest_vblank_sequence, uint64_t guest_cycle);

enum {
    GL_RENDERER_NATIVE_ENDPOINT_FORMAT_RGBA8 = 1,
};

/* Endpoint width/height remain the canonical source display dimensions. Storage
 * width/height describe the actual immutable pixel buffer/GL texture and may be
 * wider for VIEW. Composition and pixel hashing use storage dimensions.
 * temporal_phase_count is 0..7. temporal_interval_ns is the guest-cycle
 * duration since the previous published visual endpoint; interval_vblanks is
 * diagnostic only. With zero phases both intervals and previous digest are zero.
 * No temporal scheduling delay is permitted without phases. pixel_digest identifies the current
 * complete RGBA8 image, not a phase. Digests do not encode display layout. */
typedef struct GlRendererNativeEndpointMetadata {
    uint16_t display_x;
    uint16_t display_y;
    uint16_t aspect_num;
    uint16_t aspect_den;
    uint32_t width;
    uint32_t height;
    uint32_t storage_width;
    uint32_t storage_height;
    uint32_t format;
    int depth24;
    int interlaced;
    int disabled;
    uint16_t temporal_hz;
    uint16_t render_scale;
    uint32_t temporal_phase_count;
    uint32_t temporal_interval_vblanks;
    uint64_t temporal_interval_ns;
    uint64_t pixel_digest;
    uint64_t temporal_previous_pixel_digest;
} GlRendererNativeEndpointMetadata;

int gl_renderer_native_endpoint_metadata(
    uint64_t opaque_handle,
    GlRendererNativeEndpointMetadata *out_metadata);

/* native_work uses a worker-owned 1024x512 VRAM, initially zero. A chunk applies
 * DRAW/UPLOAD/COPY/FILL privately and publishes atomically. Mutation-only chunks
 * return APPLIED with zero outputs; display boundaries produce independent full
 * endpoints and software-ready fences. Identical pixel/layout/epoch/scene
 * boundaries with equivalent retained temporal recipes return APPLIED after all
 * mutations commit; GPU work with commands is classified only after its actual
 * storage digest is ready, never from the CPU reference alone. They do not replace the
 * published visual history or interrupt an interval. Pool backpressure returns
 * WOULD_BLOCK without applying work. GPU submissions retain one private owned
 * transaction across retries; service never publishes device/VIEW state itself.
 * Disabled boundaries produce black and
 * still require valid scanout dimensions/aspect.
 *
 * Device memory survives presentation epoch/scene/artifact changes. The source
 * must drain before those transitions and send an explicit full-VRAM upload
 * with mask_set/check disabled on reset/restore. native_vram_identity names
 * the last applied epoch/sequence.
 * No guest/compatibility memory is read. Device texturing remains affine and
 * canonical. TARGET selects an explicit non-wrapping framebuffer rectangle;
 * it never changes the device pixels or material scissor. A VIEW is maintained
 * when its width matches native_width - 2*native_offset_x. Native positions and
 * 2D anchors are rasterized separately, never by stretching the device image.
 * Unknown draws remain centered. COPY uses declared target maps and may name
 * a translated destination only when it copies the complete declared source.
 * A full unmasked VRAM replacement resets VIEW declarations and wave scratch;
 * SOURCE must emit the restored TARGET before subsequent marked draws.
 * Layout changes keep declarations but reseed derived images from the owned
 * canonical center with zero margins. No prior scene is invented in margins.
 * A recipe starts at a full unmasked FILL or a proven full-target, untextured,
 * opaque uniform rectangle. The latter is retained as the first draw, including
 * deterministic dither and mask state; fades and partial scissors are not clears.
 * The bounded target registry has 64 entries. Wave margins use complete 20x17
 * cohorts and declared VIEW source rows; incomplete/unsupported cohorts leave
 * the actual draw/copy result intact and increment view_wave_incomplete instead
 * of blocking valid device work or inventing a fullscreen warp.
 *
 * The separate scene-builder lane retains its compiled-surface carry cache.
 * Unlike native_work FIFO, that cache cannot reconstruct omitted delta sources.
 * Movie/UI endpoints in that lane remain discrete.
 *
 * Complete recipes retain motion snapshot generations independently of source
 * commits, including across COPY/COW and visual-history retention. Texture
 * data is retained per draw, with at most 16 immutable snapshot versions
 * per recipe. Later writes outside the target do not invalidate past samples;
 * subsequent draws capture refreshed data. Provenance records actual canonical
 * fragment writes, not inclusive bounding boxes or transparent/masked pixels.
 * Fully copied targets inherit their source recipe and its draw-time snapshots.
 * Replay must match both canonical and VIEW planes. A phase evaluates each
 * entity's XgRenderMotionPose once, reuses it for every local-vertex binding and
 * for both planes, then projects. motion.c owns local TRS/quaternion slerp and
 * hierarchy/camera composition. Unbound source vertices may instead share an
 * exact scene/group/vertex pair and its projected displacement; primitive identity
 * is not required for that source-keyed path. No proximity matching or image
 * blending is used. Unknown draws remain authored/discrete.
 * An unbound draw in a bound entity's authored producer namespace, conflicting
 * entity/camera snapshots, incompatible lifecycle or CLIP_REQUIRED rejects the
 * affected entity/component before phase rendering. The real endpoint and FIFO
 * remain valid and unchanged.
 * Renderer phases only read immutable recipe texture/CLUT data and write new
 * buffers, never committed canonical or VIEW pixels.
 * Temporal publications are consumed at their before_operation FIFO gaps,
 * including trailing metadata-only gaps. A scope replaces its complete snapshot;
 * recipe/history retain exact retired resource refs independently of commits.
 * Explicit component IDs partition atomic meshes within their producer scope.
 * Visibility anchors may come only from the immediate compatible publication,
 * never a last-seen vertex cache. Empty/intermediate publications break stale A.
 *
 * render_scale > 1 routes a complete ordered operation journal, not just motion
 * recipes, to the owner-thread Native GPU service. The CPU device and reference VIEW
 * stay 1x. All raw texture inputs are copied at their execution boundaries; GPU
 * COPY reads frozen high-resolution source planes. Only private GPU planes are
 * modified before the worker atomically applies the completed transaction.
 * The independent CPU VIEW raster may run on its ordered reader. Canonical writes
 * intersecting its conservative texture/CLUT read set, transfers, target changes,
 * failure cleanup and publication drain that reader before mutating/freeing input.
 * Endpoint width/height remain guest dimensions; storage dimensions are physical.
 * GPU image digests hash actual RGBA storage through fenced PBOs. Logical reference
 * digests are separate and never stand in for those images. Optional guest
 * comparison samples the original logical grid from the actual GPU endpoint.
 * Movies with intrinsically packed 24-bit pixels keep their original storage. */
typedef enum GlRendererNativeCompileBlocker {
    GL_RENDERER_NATIVE_BLOCKER_NONE = 0,
    GL_RENDERER_NATIVE_BLOCKER_HEADER_COPY,
    GL_RENDERER_NATIVE_BLOCKER_HEADER_MISMATCH,
    GL_RENDERER_NATIVE_BLOCKER_INVALID_DISPLAY,
    GL_RENDERER_NATIVE_BLOCKER_PASS_COPY,
    GL_RENDERER_NATIVE_BLOCKER_DRAW_COPY,
    GL_RENDERER_NATIVE_BLOCKER_RESOURCE_COPY,
    GL_RENDERER_NATIVE_BLOCKER_RESOURCE_VIEW,
    GL_RENDERER_NATIVE_BLOCKER_RESOURCE_DIGEST,
    GL_RENDERER_NATIVE_BLOCKER_SURFACE_EDGE_COPY,
    GL_RENDERER_NATIVE_BLOCKER_UI_NODE_COPY,
    GL_RENDERER_NATIVE_BLOCKER_UI_GLYPH_RUN_COPY,
    GL_RENDERER_NATIVE_BLOCKER_UI_GLYPH_PLACEMENT_COPY,
    GL_RENDERER_NATIVE_BLOCKER_TARGET_SURFACE_DESCRIPTOR,
    GL_RENDERER_NATIVE_BLOCKER_TEXTURE_DESCRIPTOR,
    GL_RENDERER_NATIVE_BLOCKER_UI_RESOURCE_DESCRIPTOR,
    GL_RENDERER_NATIVE_BLOCKER_SURFACE_EDGE_DESCRIPTOR,
    GL_RENDERER_NATIVE_BLOCKER_MOVIE_LAYOUT,
    GL_RENDERER_NATIVE_BLOCKER_ENDPOINT_CAPACITY,
    GL_RENDERER_NATIVE_BLOCKER_WORKER_CONTEXT,
    GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE,
    GL_RENDERER_NATIVE_BLOCKER_FENCE_CAPACITY,
    GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
    GL_RENDERER_NATIVE_BLOCKER_PASS_DEPENDENCY,
    GL_RENDERER_NATIVE_BLOCKER_UNSUPPORTED_UI_NODE,
    GL_RENDERER_NATIVE_BLOCKER_NO_STORED_DISPLAY_SURFACE,
    GL_RENDERER_NATIVE_BLOCKER_SURFACE_HISTORY,
    GL_RENDERER_NATIVE_BLOCKER_NATIVE_OPERATION,
} GlRendererNativeCompileBlocker;

typedef enum GlRendererNativeTemporalStatus {
    GL_RENDERER_NATIVE_TEMPORAL_NOT_APPLICABLE = 0,
    GL_RENDERER_NATIVE_TEMPORAL_SOURCE_STATE_REQUIRED,
    GL_RENDERER_NATIVE_TEMPORAL_DISCRETE_DISPLAY,
    GL_RENDERER_NATIVE_TEMPORAL_NO_RECIPE,
    GL_RENDERER_NATIVE_TEMPORAL_REPLAY_FAILED,
    GL_RENDERER_NATIVE_TEMPORAL_REPLAY_MISMATCH,
    GL_RENDERER_NATIVE_TEMPORAL_READY,
    GL_RENDERER_NATIVE_TEMPORAL_DUPLICATE,
    GL_RENDERER_NATIVE_TEMPORAL_NO_HISTORY,
    GL_RENDERER_NATIVE_TEMPORAL_BINDING_INCOMPLETE,
    GL_RENDERER_NATIVE_TEMPORAL_POSE_INCOMPATIBLE,
    GL_RENDERER_NATIVE_TEMPORAL_CLIP_REQUIRED,
    GL_RENDERER_NATIVE_TEMPORAL_PROJECTION_INVALID,
    GL_RENDERER_NATIVE_TEMPORAL_ALLOCATION,
    GL_RENDERER_NATIVE_TEMPORAL_STATIC_POSE,
    GL_RENDERER_NATIVE_TEMPORAL_SMOOTHING_DISABLED,
    GL_RENDERER_NATIVE_TEMPORAL_SOURCE_CADENCE,
    GL_RENDERER_NATIVE_TEMPORAL_DEADLINE_EXPIRED,
    GL_RENDERER_NATIVE_TEMPORAL_WHOLE_ONLY,
    GL_RENDERER_NATIVE_TEMPORAL_STATUS_COUNT,
} GlRendererNativeTemporalStatus;

typedef struct GlRendererNativeLegacyOwnerDiagnostics {
    uint64_t canonical_draws;
    uint64_t canonical_fills;
    uint64_t canonical_copies;
    uint64_t skipped_view_draws;
    uint64_t skipped_temporal_candidates;
    uint64_t skipped_anchor_vertices;
    uint64_t legacy_host_raster_passes;
    uint32_t pending_host_draws;
    uint32_t legacy_view_surfaces;
    uint32_t configured_view_width;
    int active;
} GlRendererNativeLegacyOwnerDiagnostics;

typedef struct GlRendererNativeGpuDiagnostics {
    uint64_t submitted, completed, applied, cancelled;
    uint64_t geometry_draws, transfer_draws, commands, captured_bytes, readback_bytes;
    uint64_t service_ns;
    uint64_t submit_ns, finish_ns, service_max_ns, hash_ns;
    uint64_t fence_polls, fence_pending, fence_latency_ns, fence_latency_max_ns;
    uint64_t timed_work, gpu_render_ns, gpu_readback_ns, gpu_max_ns;
    uint64_t word_uploads, snapshot_commands;
    uint64_t destination_barriers, destination_copies;
    /* Destination reads that needed no barrier (no write overlapped them). */
    uint64_t skipped_barriers;
    /* Native depth test on the GPU: triangles depth-tested, and samples that
     * passed in runs of them (GL_SAMPLES_PASSED, at the render scale). */
    uint64_t depth_tested_triangles, depth_samples_passed;
    /* Reference is the CPU 1x scanout; image is the actual GPU RGBA storage. */
    uint64_t last_reference_digest, last_image_digest;
    XgPresentationIdentity last_image_identity;
    /* Last completed GPU image, not a pending settings request or a CPU movie. */
    uint32_t render_scale, storage_width, storage_height;
    /* 0 idle, 1 queued, 2 submitted, 3 ready, 4 published, 5 cancelled, 6 dispatching. */
    uint32_t pending_state;
} GlRendererNativeGpuDiagnostics;

typedef struct GlRendererNativeCompilerDiagnostics {
    uint64_t sealed_captures;
    uint64_t sealed_capture_rejections;
    uint64_t compile_attempts;
    uint64_t compiled_endpoints;
    uint64_t compile_failures;
    uint64_t last_commit_digest;
    uint64_t last_record_audit_digest;
    uint64_t blocker_resource_id;
    uint64_t blocker_resource_generation;
    uint64_t rendered_draw_pixels;
    uint64_t endpoint_visible_pixels;
    uint64_t last_endpoint_pixel_digest;
    XgPresentationIdentity last_capture_identity;
    XgPresentationIdentity last_compile_identity;
    uint32_t last_blocker;
    uint32_t blocker_record_index;
    uint32_t consumed_passes;
    uint32_t consumed_draws;
    uint32_t consumed_resources;
    uint32_t consumed_surface_edges;
    uint32_t consumed_ui_nodes;
    uint32_t consumed_ui_glyph_runs;
    uint32_t consumed_ui_glyph_placements;
    uint32_t rendered_passes;
    uint32_t rendered_draws;
    uint32_t rendered_surface_edges;
    uint32_t rendered_ui_nodes;
    uint32_t rendered_ui_glyphs;
    uint32_t pass_loads;
    uint32_t pass_clears;
    uint32_t pass_discards;
    uint32_t pass_stores;
    int all_records_consumed;
    int last_endpoint_was_movie;
    int last_endpoint_was_depth24;
    int last_endpoint_was_discrete;
    int last_endpoint_pixel_digest_valid;
    uint32_t last_endpoint_display_x;
    uint32_t last_endpoint_display_y;
    uint32_t last_endpoint_width;
    uint32_t last_endpoint_height;
    uint32_t last_endpoint_first_edge_kind;
    uint32_t consumed_native_operations;
    uint64_t applied_native_work;
    uint64_t compile_would_block;
    XgPresentationIdentity native_vram_identity;
    uint32_t rendered_view_draws;
    uint32_t marked_view_draws;
    uint32_t view_copies;
    uint32_t view_wave_rows;
    uint32_t view_wave_incomplete;
    uint32_t last_storage_width;
    uint32_t last_storage_height;
    uint32_t view_target_count;
    int32_t active_view_target;
    /* Last published native endpoint, not overwritten by duplicate boundaries. */
    uint64_t temporal_status_counts[GL_RENDERER_NATIVE_TEMPORAL_STATUS_COUNT];
    XgPresentationIdentity last_temporal_identity;
    uint32_t last_temporal_status;
    uint32_t last_temporal_phase_count;
    uint32_t last_temporal_interval_vblanks;
    uint64_t last_temporal_interval_ns;
    uint64_t duplicate_endpoints;
    uint64_t motion_evaluations;
    uint64_t motion_projected_draws;
    uint32_t consumed_motion_resources;
    int recipe_canonical_match;
    int recipe_view_match;
    /* Last published Native endpoint. Match flags are unknown (zero), not a
     * failed comparison, when this optional pixel audit was not performed. */
    int recipe_validation_performed;
    /* Cumulative committed Native-work counts; excludes diagnostic/phase replay. */
    uint64_t view_logical_draws;
    uint64_t view_physical_raster_passes;
    uint32_t view_domain_count;
    /* Cumulative phase-generation cost (replay + hash per rendered phase):
     * total ns, batches, and phases. avg ms/phase tells whether N phases
     * fit the tick deadline at high targets. */
    uint64_t phase_gen_ns_total;
    uint64_t phase_gen_batches;
    uint64_t phase_gen_phases;
    /* Native depth test. Last captured option state, and cumulative committed
     * VIEW draws per effective stamp (NONE/TEST/TEST_WRITE) plus CPU reference
     * raster fragments tested, rejected, written and reset to far. */
    int native_depth_test;
    uint64_t depth_draws[3];
    uint64_t depth_tested_fragments;
    uint64_t depth_rejected_fragments;
    uint64_t depth_written_fragments;
    uint64_t depth_reset_fragments;
    /* Main-owned GPU path, published under the diagnostic lock at present/read. */
    GlRendererNativeLegacyOwnerDiagnostics legacy_owner;
    GlRendererNativeGpuDiagnostics gpu;
} GlRendererNativeCompilerDiagnostics;

void gl_renderer_native_compiler_diagnostics(
    GlRendererNativeCompilerDiagnostics *out_diagnostics);
const char *gl_renderer_native_compile_blocker_name(uint32_t blocker);
const char *gl_renderer_native_temporal_status_name(uint32_t status);

typedef enum GlRendererNativePresentBlocker {
    GL_RENDERER_NATIVE_PRESENT_BLOCKER_NONE = 0,
    GL_RENDERER_NATIVE_PRESENT_BLOCKER_INVALID_ARGUMENT,
    GL_RENDERER_NATIVE_PRESENT_BLOCKER_ENDPOINT_MISMATCH,
    GL_RENDERER_NATIVE_PRESENT_BLOCKER_STAGING_MISSING,
    GL_RENDERER_NATIVE_PRESENT_BLOCKER_CONTEXT,
    GL_RENDERER_NATIVE_PRESENT_BLOCKER_UPLOAD,
    GL_RENDERER_NATIVE_PRESENT_BLOCKER_DRAWABLE,
    GL_RENDERER_NATIVE_PRESENT_BLOCKER_GL_DRAW,
    GL_RENDERER_NATIVE_PRESENT_BLOCKER_FENCE,
    GL_RENDERER_NATIVE_PRESENT_BLOCKER_SWAP_CONTEXT,
    GL_RENDERER_NATIVE_PRESENT_BLOCKER_SWAP_REJECTED,
} GlRendererNativePresentBlocker;

typedef struct GlRendererNativePipelineDiagnostics {
    uint64_t capture_attempts;
    uint64_t capture_successes;
    uint64_t capture_failures;
    uint64_t upload_attempts;
    uint64_t upload_successes;
    uint64_t upload_failures;
    uint64_t compose_attempts;
    uint64_t compose_successes;
    uint64_t compose_failures;
    uint64_t compose_retired_before_swap;
    uint64_t swap_attempts;
    uint64_t swap_successes;
    uint64_t swap_failures;
    uint64_t source_pixel_comparisons;
    uint64_t source_pixel_matches;
    uint64_t source_pixel_mismatches;
    uint64_t guest_reference_capture_attempts;
    uint64_t guest_reference_captures;
    uint64_t guest_reference_capture_failures;
    uint64_t guest_reference_bound_sources;
    uint64_t guest_reference_missing_sources;
    uint64_t guest_reference_dropped_sources;
    uint64_t guest_reference_comparisons;
    uint64_t guest_reference_matches;
    uint64_t guest_reference_mismatches;
    uint64_t last_commit_digest;
    uint64_t last_record_audit_digest;
    uint64_t last_endpoint_pixel_digest;
    uint64_t last_endpoint_handle;
    uint64_t last_backend_generation;
    uint64_t last_present_sequence;
    uint64_t last_compared_present_sequence;
    uint64_t last_compared_endpoint_pixel_digest;
    uint64_t last_compared_source_hash;
    uint64_t last_guest_reference_sequence;
    uint64_t last_guest_reference_digest;
    uint64_t last_guest_reference_endpoint_digest;
    uint64_t last_guest_reference_mismatch_pixels;
    XgPresentationIdentity last_identity;
    XgPresentationIdentity last_compared_identity;
    XgPresentationIdentity last_guest_reference_identity;
    uint32_t last_endpoint_mismatch_mask;
    uint32_t last_present_blocker;
    uint32_t last_width;
    uint32_t last_height;
    uint32_t last_format;
    uint32_t last_guest_reference_mismatch_mask;
    int last_source_pixel_comparison_valid;
    int last_source_pixel_match;
    int last_guest_reference_comparison_valid;
    int last_guest_reference_match;
    int pending_present;
} GlRendererNativePipelineDiagnostics;

void gl_renderer_native_pipeline_diagnostics(
    GlRendererNativePipelineDiagnostics *out_diagnostics);
const char *gl_renderer_native_present_blocker_name(uint32_t blocker);

enum {
    GL_NATIVE_GUEST_REFERENCE_MISMATCH_MISSING = 1u << 0,
    GL_NATIVE_GUEST_REFERENCE_MISMATCH_WIDTH = 1u << 1,
    GL_NATIVE_GUEST_REFERENCE_MISMATCH_HEIGHT = 1u << 2,
    GL_NATIVE_GUEST_REFERENCE_MISMATCH_DEPTH = 1u << 3,
    GL_NATIVE_GUEST_REFERENCE_MISMATCH_PIXELS = 1u << 4,
};

#define GL_NATIVE_GUEST_REFERENCE_RING_CAPACITY 256u
#define GL_NATIVE_GUEST_REFERENCE_FAILURE_CAPACITY 16u
#define GL_NATIVE_GUEST_REFERENCE_SAMPLE_CAPACITY 8u

typedef struct GlRendererNativeGuestReferenceEvent {
    XgPresentationIdentity identity;
    uint64_t semantic_digest;
    uint64_t record_audit_digest;
    uint64_t reference_pixel_digest;
    uint64_t endpoint_pixel_digest;
    uint64_t mismatch_pixel_count;
    uint64_t rendered_draw_pixels;
    uint64_t endpoint_visible_pixels;
    uint32_t scene_module;
    uint32_t authored_scene_id;
    uint32_t authored_submode;
    uint32_t source_interval_vblanks;
    uint32_t source_display_x;
    uint32_t source_display_y;
    uint32_t reference_width;
    uint32_t reference_height;
    uint32_t endpoint_width;
    uint32_t endpoint_height;
    uint32_t mismatch_mask;
    uint32_t mismatch_bounds[4];
    uint32_t consumed_records[6];
    uint32_t rendered_records[5];
    uint32_t pass_operations[4];
    uint16_t sample_x[GL_NATIVE_GUEST_REFERENCE_SAMPLE_CAPACITY];
    uint16_t sample_y[GL_NATIVE_GUEST_REFERENCE_SAMPLE_CAPACITY];
    uint32_t reference_samples[GL_NATIVE_GUEST_REFERENCE_SAMPLE_CAPACITY];
    uint32_t endpoint_samples[GL_NATIVE_GUEST_REFERENCE_SAMPLE_CAPACITY];
    uint8_t sample_count;
    uint8_t reference_valid;
    uint8_t identity_valid;
    uint8_t endpoint_valid;
    uint8_t comparison_valid;
    uint8_t matches_endpoint;
    uint8_t reference_depth24;
    uint8_t endpoint_depth24;
} GlRendererNativeGuestReferenceEvent;

uint64_t gl_renderer_native_guest_reference_total(void);
int gl_renderer_native_guest_reference_get(
    uint64_t sequence, GlRendererNativeGuestReferenceEvent *out_event);
uint64_t gl_renderer_native_guest_reference_failure_total(void);
int gl_renderer_native_guest_reference_failure_get(
    uint32_t index, uint64_t *out_sequence,
    GlRendererNativeGuestReferenceEvent *out_event);

/* Set the GL swap interval / vsync mode (1=vsync, 0=immediate, -1=adaptive).
 * Safe before or after context creation; applies live when a context exists. */
void gl_renderer_set_swap_interval(int interval);
/* Semantic Native interpolation target. Accepted values: 0/default, 60, 120,
 * 240. Targets above 60 use immediate swaps plus main-context subframe pacing;
 * Wayland presentation feedback may phase-align that clock to physical retrace. */
int gl_renderer_set_native_interpolation_fps(int target_fps);
int gl_renderer_native_interpolation_fps(void);
/* True requested target (pool denominator aside): 75/144/165 report
 * exactly; the plain getter above reports denominator*30. */
int gl_renderer_native_interpolation_target_fps(void);

/* Presentation-only temporal blending. High-refresh sub-presents blend the two
 * most recent stable display images on the owning render thread/context; this
 * does not generate motion vectors or true intermediate object positions. */
void gl_renderer_set_interpolation(int enabled, double host_hz, double target_hz,
                                   double source_hz, int blend_mode);
void gl_renderer_set_interpolation_suspended(int suspended);
int gl_renderer_interpolation_owns_cadence(void);
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

/* Bezel art shown in the letterbox/pillarbox margins. Takes RGBA8 pixels; the
 * caller owns them and may free them on return. Passing NULL clears it.
 * Returns 0 only if a texture could not be created. */
int  gl_renderer_set_bezel(const void *rgba, int w, int h);
int  gl_renderer_has_bezel(void);

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

/* FMV present reconstruction, settings.toml [video] fmv_filter. Takes the
 * config enum VIDEO_FMV_FILTER_* (0 nearest, 1 bilinear, 2 sharp, 3 bicubic).
 * Only consulted while video antialiasing is on; AA off is always nearest. */
void gl_renderer_set_fmv_filter(int cfg_value);
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
 * READY path the private owner validation and its immediately-contained swap
 * precede all renderer publication and checkpoint disposal. SDL exposes no
 * swap result, so SUCCESS means the call returned under the context/window
 * ownership validated by commit. NOT_READY leaves any open pre-READY
 * checkpoint rollbackable. */
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
    uint64_t retired_ineligible_missing_anchor_count;
    uint64_t retired_ineligible_producer_absent_count;
    uint64_t winding_component_snap_count;
    uint64_t winding_phase_only_reject_count;
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

/* Presentation-only stretch of the 4:3 view to num:den (0,0 = off). Pair with
 * gl_renderer_set_display_aspect(num, den); the Native presenter follows it
 * for everything except 24-bit (FMV) endpoints, which stay 4:3. */
void gl_renderer_set_display_stretch(int num, int den);

/* Scanline post-process (host display setting). on toggles the effect; strength
 * (0..1) is the depth of the dark gap between PS1 scanlines. Applied at the
 * native display-line pitch in the present/interpolation shaders, and faded in
 * with output scale so it never shimmers on a sub-2x window. gl_renderer_get_
 * scanlines returns the on flag and (via out-param) the current strength. */
void gl_renderer_set_scanlines(int on, float strength);
int  gl_renderer_get_scanlines(float *strength);

/* Presentation-only gamma adjustment. gamma = 1.0 is the identity; values
 * above 1.0 lift shadow detail and values below 1.0 darken it. The adjustment
 * is applied once to game content in the final GL presentation pass, including
 * temporal interpolation, but not to the bezel, host OSD, black margins, or an
 * already-composed hold-last image. Non-finite and out-of-range values are
 * clamped to a safe range. Safe to call before GL context creation. */
void  gl_renderer_set_post_gamma(float gamma);
float gl_renderer_get_post_gamma(void);

/* Select full native-wide mirror rendering instead of the centre-splice fast
 * path. Textured edge expansion needs the complete mirror surface. */
void gl_renderer_set_wide_fast(int on);
/* Native renderer per-pixel depth test for producer-classified 3D draws
 * (host presentation only). Always on at startup; only the debug overlay and
 * debug server switch it, and it is never persisted. Off is bit-identical to
 * pure ordering-table order. Captured into each Native work's display state,
 * so it applies from the next frame. */
void gl_renderer_set_native_depth_test(int on);
int  gl_renderer_native_depth_test(void);
/* Debug presentation of the Native GPU VIEW/phase planes: 0 = off, 1 = depth
 * keys on a colour ramp over the frame's key range (red near, blue far; black
 * = no certified surface), 3 = the same range in grey (near bright),
 * 2 = per-draw depth policy tint (grey NONE, yellow TEST, green TEST_WRITE,
 * red = stamped but without a usable depth plane). Presentation only; changing
 * it re-seeds the planes. Only the GPU (render scale > 1) path shows it. */
void gl_renderer_set_native_depth_view(int mode);
int  gl_renderer_native_depth_view(void);
/* Debug wireframe of the Native GPU VIEW/phase planes: every triangle drawn as
 * unlit one-pixel lines over black (guest pixels, fills and copies out of
 * guest VRAM become black). Presentation only; overrides the depth view and
 * re-seeds the planes when changed. Only the GPU path shows it. */
void gl_renderer_set_native_wireframe(int on);
int  gl_renderer_native_wireframe(void);

/* Bounded ring of Native motion (frame interpolation) entity rejections: why
 * a bound entity's phases fell back to discrete endpoints. Copies up to
 * capacity most recent events, oldest first; returns the count copied. */
typedef struct GlRendererNativeMotionRejectEvent {
    uint64_t sequence;       /* 0-based position in the lifetime event stream */
    const char *reason;      /* static string */
    uint64_t entity_id;
    uint64_t source_update;
    uint32_t draw_index;
    uint32_t producer_id;
} GlRendererNativeMotionRejectEvent;
#define GL_RENDERER_NATIVE_MOTION_REJECT_CAPACITY 32u
uint32_t gl_renderer_native_motion_rejects(GlRendererNativeMotionRejectEvent *out,
                                           uint32_t capacity, uint64_t *out_total);

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
 * flush, fill, copy, draw bbox, pack, regional readback, present, and probe
 * perturbation, with rect + frame. An op that flushes internally records its
 * own event AFTER the FLUSH it caused (the event after a FLUSH = trigger). */
enum {
    GL_COH_FLUSH    = 1,   /* CPU->FBO upload flush (pending box)     */
    GL_COH_FILL     = 2,   /* GP0(02) fill rect                       */
    GL_COH_COPY_SRC = 3,   /* GP0(80) copy, source rect               */
    GL_COH_COPY     = 4,   /* GP0(80) copy, dest rect                 */
    GL_COH_DRAW     = 5,   /* drawn prim bbox (clipped to draw area)  */
    GL_COH_PACK     = 6,   /* hr FBO -> raw mirror pack (dirty box)   */
    GL_COH_ENSURE   = 7,   /* regional FBO -> CPU VRAM readback       */
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
    uint8_t  swap_attempted;
    uint8_t  swap_failed;
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
    uint8_t  semantic_identity_valid;
    uint8_t  source_pixel_comparison_valid;
    uint8_t  source_pixel_match;
    uint8_t  native_upload_attempted;
    uint8_t  native_upload_succeeded;
    uint8_t  native_compose_succeeded;
    uint8_t  endpoint_pixel_digest_valid;
    uint8_t  native_retired_before_swap;
    uint8_t  native_state_reserved[7];
    uint64_t source_hash;
    XgPresentationIdentity semantic_identity;
    uint64_t semantic_digest;
    uint64_t record_audit_digest;
    uint64_t endpoint_pixel_digest;
    uint64_t endpoint_handle;
    uint64_t endpoint_backend_generation;
    uint32_t endpoint_format;
    uint32_t endpoint_mismatch_mask;
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
/* HD texture replacement: {native resident, native bytes, native failed,
 * legacy resident, legacy bytes, legacy failed, native draws replaced}. */
void gl_renderer_hd_texture_stats(uint64_t out[7]);

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
