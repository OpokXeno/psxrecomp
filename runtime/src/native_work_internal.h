#ifndef NATIVE_WORK_INTERNAL_H
#define NATIVE_WORK_INTERNAL_H

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "gpu.h"
#include "gpu_render.h"
#include "gpu_semantic_workload.h"
#include "gpu_sw_renderer.h"
#include "gpu_gl_renderer.h"
#include "native_vk_present.h"
#include "gpu_vram_region_set.h"
#include "psx_render_nclip.h"
#include "xg_render_motion.h"
#include "xg_render_native_mesh.h"
#include "xg_render_native_work.h"
#include "xg_render_semantic_presentation.h"
#include "latency_ring.h"
#include "debug_overlay.h"
#include "runtime_menu.h"
#include "wayland_presentation.h"

#include <limits.h>
#include <float.h>
#include "mod_texture_banks.h"
#include "hd_texture_runtime.h"
#include "frame_interpolation.h"
#include "host_osd.h"
#include "host_time.h"
#include "host_cpu.h"
#include "host_parallel.h"
#include "frame_pacing.h"
#include "psx_rewind.h"

#include "psx_sdl.h"
#include "native_phase_cadence.h"
#include "native_gpu_hazard.h"
#include "native_depth.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "png_write.h"   /* png_write_rgb — present_shot readback */

/* ---- shared types ---- */
#define VRAM_W 1024
#define VRAM_H 512
/* User-facing cap. x8 = 8192x4096 — inside every
 * modern driver's GL_MAX_TEXTURE_SIZE. The live-rebuild fallback in
 * gl_maybe_apply_scale still restores the previous scale if refused. */
#define GL_MAX_INTERNAL_SCALE 8

/* Dirty-rect unions, native VRAM coords, inclusive bounds. */
typedef struct { int x0, y0, x1, y1, set; } DirtyRect;

/* ---- Native asynchronous software compiler / main-thread presenter ---
 * The sealed callback authenticates only the immutable SourceCommit.  The
 * worker consumes its records through copy APIs and compiles private device
 * pixels and, at higher resolution, an owned GPU operation journal;
 * neither s_hr_fbo nor any mutable Native/compatibility present texture is a
 * Native source. The main owner services GPU journals and presents endpoints;
 * it owns every GL operation, including deferred work with no endpoint. */
/* Complete queued batches, one retained hold, and one compiling endpoint. */
#define GL_NATIVE_ENDPOINT_CAPACITY (XG_RENDER_PRESENTATION_BATCH_CAPACITY + 2u)
#define GL_NATIVE_FENCE_CAPACITY (GL_NATIVE_ENDPOINT_CAPACITY * 2u + 2u)

typedef enum GlNativeEndpointUploadState {
    GL_NATIVE_ENDPOINT_UPLOAD_NONE = 0,
    GL_NATIVE_ENDPOINT_UPLOAD_STAGED,
    GL_NATIVE_ENDPOINT_UPLOAD_IN_PROGRESS,
    GL_NATIVE_ENDPOINT_UPLOAD_READY,
    GL_NATIVE_ENDPOINT_UPLOAD_FAILED,
} GlNativeEndpointUploadState;

typedef enum GlNativeFenceKind {
    GL_NATIVE_FENCE_KIND_NONE = 0,
    GL_NATIVE_FENCE_KIND_SOFTWARE_READY,
    GL_NATIVE_FENCE_KIND_GL_COMPLETION,
} GlNativeFenceKind;

typedef enum GlNativeFenceState {
    GL_NATIVE_FENCE_STATE_FREE = 0,
    GL_NATIVE_FENCE_STATE_PENDING,
    GL_NATIVE_FENCE_STATE_READY,
} GlNativeFenceState;

#define GL_NATIVE_MOTION_PHASE_CAPACITY 15u
/* Backend-opaque image: color/target/mask hold a GL texture / framebuffer /
 * mask texture name (GLuint in the low 32 bits) for the GL executor; neutral
 * code only moves and compares them. */
typedef struct NativeImageSlot {
    uint64_t color, target, mask;
    uint32_t width, height;
    int mask_valid;
    uint32_t *pixels;
    uint64_t digest;
    GlNativeEndpointUploadState upload_state;
} NativeImageSlot;

typedef struct GlNativeEndpointSlot {
    /* The authored image: color/target(allocated/uploaded by the main-thread
     * presenter)/mask/width/height only. Its pixels, digest and upload_state
     * stay in staged_pixels, pixel_digest and upload_state below. */
    NativeImageSlot image;
    int mask_valid;
    uint32_t *staged_pixels;
    XgPresentationIdentity identity;
    GlRendererNativeEndpointMetadata metadata;
    uint64_t digest;
    uint64_t record_audit_digest;
    uint64_t pixel_digest;
    uint64_t backend_generation;
    uint64_t temporal_generation;
    uint32_t generation;
    uint32_t fence_refs;
    GlNativeEndpointUploadState upload_state;
    int in_use;
    int release_pending;
    int presenter_busy;
    NativeImageSlot phases[GL_NATIVE_MOTION_PHASE_CAPACITY];
} GlNativeEndpointSlot;

typedef struct GlNativeFenceSlot {
    void *sync; /* GLsync, opaque outside the GL executor */
    uint32_t generation;
    uint32_t endpoint_slot;
    uint32_t endpoint_generation;
    GlNativeFenceKind kind;
    GlNativeFenceState state;
} GlNativeFenceSlot;

typedef struct GlNativeResourceRecord {
    XgSemanticResourceRef reference;
    XgRenderResourceView view;
    int view_valid;
} GlNativeResourceRecord;

typedef struct GlNativeCompileAudit {
    XgRenderSourceCommitHeader header;
    uint32_t consumed_temporal_coverages;
    uint32_t consumed_temporal_publications;
    uint32_t applied_temporal_publications;
    uint64_t record_digest;
    uint64_t blocker_resource_id;
    uint64_t blocker_resource_generation;
    uint64_t rendered_draw_pixels;
    uint64_t endpoint_visible_pixels;
    uint64_t endpoint_pixel_digest;
    uint32_t blocker;
    uint32_t blocker_record_index;
    uint32_t consumed_passes;
    uint32_t consumed_draws;
    uint32_t consumed_resources;
    uint32_t consumed_surface_edges;
    uint32_t consumed_ui_nodes;
    uint32_t consumed_ui_glyph_runs;
    uint32_t consumed_ui_glyph_placements;
    uint32_t consumed_native_operations;
    uint32_t endpoint_storage_width;
    uint32_t endpoint_storage_height;
    uint32_t rendered_view_draws;
    uint32_t marked_view_draws;
    uint32_t view_raster_passes;
    uint32_t view_copies;
    uint32_t view_wave_rows;
    uint32_t view_wave_incomplete;
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
    int saw_textured_draw;
    int endpoint_was_movie;
    int endpoint_was_depth24;
    int endpoint_was_discrete;
    uint32_t temporal_status;
    uint32_t consumed_motion_resources, temporal_phase_count, temporal_interval_vblanks;
    uint32_t motion_evaluations, motion_projected_draws;
    /* Phase-loop cost (replay + hash per rendered phase): ns + delivered
     * phases, merged into phase_gen_* diag. Only successful/partial loops
     * accumulate (discarded work is pure waste, not generation cost). */
    uint64_t phase_gen_ns;
    uint32_t phase_gen_count;
    uint64_t temporal_interval_ns;
    int recipe_canonical_match, recipe_view_match, recipe_validation_performed;
    /* Native depth test (CPU reference raster): VIEW draws by effective stamp
     * (NONE/TEST/TEST_WRITE), and fragments tested/rejected/written/reset. */
    uint32_t depth_draws[3];
    uint64_t depth_tested_fragments, depth_rejected_fragments;
    uint64_t depth_written_fragments, depth_reset_fragments;
    uint32_t depth_key_min, depth_key_max; /* positive FP32 key bits; 0 = none (depth view range) */
    /* Only populated entries are read; keep capacity storage out of the reset. */
    XgSemanticPassRecord passes[XG_RENDER_SCENE_PASS_CAPACITY];
    XgSemanticDrawRecord draws[XG_RENDER_SCENE_DRAW_CAPACITY];
    GlNativeResourceRecord resources[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgRenderMotionRef motion_resources[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgSemanticResourceRef temporal_coverages[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgRenderTemporalPublication temporal_publications[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgSemanticSurfaceEdge edges[XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY];
    XgSemanticUiNodeRecord ui_nodes[XG_RENDER_SCENE_UI_NODE_CAPACITY];
    XgSemanticUiGlyphRunRecord
        ui_runs[XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY];
    XgSemanticUiGlyphPlacementRecord
        ui_placements[XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY];
} GlNativeCompileAudit;

typedef struct GlNativeGuestReferenceSlot {
    GlRendererNativeGuestReferenceEvent event;
    uint32_t *pixels;
    uint64_t sequence;
} GlNativeGuestReferenceSlot;

typedef struct GlNativePendingGuestReference {
    uint32_t *pixels;
    uint64_t guest_vblank_sequence;
    uint64_t guest_cycle;
    uint64_t pixel_digest;
    uint32_t width;
    uint32_t height;
    uint8_t depth24;
    uint8_t valid;
} GlNativePendingGuestReference;
#include "xg_render_array.h"
typedef struct GlNativeRecipePixels {
    uint32_t references;
    uint64_t write_serial;
    uint16_t words[VRAM_W * VRAM_H];
} GlNativeRecipePixels;
typedef struct GlNativeRecipeReads {
    GpuVramRegionSet words;
    uint16_t indices[VRAM_H * GPU_VRAM_REGION_WORDS_PER_ROW];
} GlNativeRecipeReads;
/* The material fields a batch may vary per triangle: which texels and how
 * they combine. Clip area, offsets, texture window, dither and mask bits
 * stay the command's (or recipe record's) own. */
typedef struct GlNativeTextureState {
    uint16_t tpage, page_x, page_y, clut_x, clut_y;
    uint8_t texture_depth, shading, textured, raw_texture, semi_transparent, blend_mode;
} GlNativeTextureState;
typedef struct GlNativeRecipeDraw {
    GpuRenderSemantic semantic; /* target-local coordinates; no packet pointers */
    GlNativeRecipePixels *textures; /* Immutable texels at this draw's execution. */
    XgRenderMotionDrawBinding motion;
    int32_t projection_offset_x, projection_offset_y;
    uint32_t motion_index;
    uint64_t temporal_component;
    uint32_t temporal_index;
    uint8_t enhanced;
    uint8_t temporal_departure; /* Phase-only, never part of endpoint/history. */
    uint8_t dither_x, dither_y;
    uint16_t view_origin_y;
    GpuRenderHdTexture hd_texture; /* page-relative, survives relocation */
    /* A MESH: semantic is its material template (no triangles); the shared
     * expansion holds them in guest coordinates, relocated by
     * projection_offset_x/y at replay exactly like semantic was. */
    struct GlNativeRecipeMesh *mesh;
} GlNativeRecipeDraw;
/* An expanded MESH operation: immutable once built, shared by the recipes
 * that replay it (counted on the compile owner, read by phase workers). */
typedef struct GlNativeRecipeMesh {
    uint32_t references, count;
    uint32_t capacity;
    /* Coalesced guest draws (native_recipe_coalesce): triangles already
     * relocated into the recipe target, each with its own texture state
     * (textures, capacity entries), grown in place while one recipe alone
     * holds them. A MESH operation's expansion is not relocated and uses
     * its record's texture state (textures NULL). */
    uint8_t relocated;
    GlNativeTextureState *textures;
    /* Coalesced runs may also cross objects: each triangle's own pose
     * (recipe motion index and part) and owner identity (capacity entries;
     * NULL: the record's). */
    struct GlNativeRecipeMeshPose *poses;
    XgRenderNativeMeshTriangle triangles[];
} GlNativeRecipeMesh;
typedef struct GlNativeRecipeMeshPose {
    uint64_t scene_id;
    uint32_t motion_index, part, producer_id;
    uint8_t identity_valid;
} GlNativeRecipeMeshPose;
typedef struct GlNativeRecipeCoverage {
    XgSemanticResourceRef reference;
    XgRenderTemporalCoverageView view;
    /* Identity-only adjacency receipt; no ownership of predecessor bytes. */
    XgSemanticResourceRef predecessor;
} GlNativeRecipeCoverage;
typedef struct GlNativeCoverageScope {
    GlNativeRecipeCoverage current, previous;
} GlNativeCoverageScope;
typedef struct GlNativeCoverageState {
    uint32_t references, count;
    uint32_t capacity;
    GlNativeCoverageScope *scopes;
} GlNativeCoverageState;
/* Field wave (FUN_800a4dac). The guest saves the bottom 48 rows at x=960,
 * redraws its frame 32 rows lower through rows 0..13 of a 20x17 mesh of 16x16
 * tiles, lifts the result 32 rows, and redraws the bottom from the saved rows
 * through rows 14..16. The displacement is separable: every row shares the
 * column boundaries x(c) = 16c + Ax*rsin(phase + c*fx) >> 12, every column
 * the row boundaries. Native keeps those transfers and tiles out of VIEW and
 * replays the same mesh as spans of the rendered VIEW plane, continuing the
 * game's column formula through the widescreen margins. */
#define NATIVE_WAVE_COLUMNS 20
#define NATIVE_WAVE_ROWS 17
#define NATIVE_WAVE_TILES (NATIVE_WAVE_COLUMNS * NATIVE_WAVE_ROWS)
#define NATIVE_WAVE_TRANSFERS 15
#define NATIVE_WAVE_MAX_BOUNDARIES 96
/* The guest links rows 13..0 then 16..14, each from column 19 down to 0: row
 * 14 column 0 is the last tile of a frame's mesh. */
#define NATIVE_WAVE_LAST_TILE (14u * NATIVE_WAVE_COLUMNS)
typedef struct GlNativeWaveTile {
    int16_t left, right, top, bottom; /* target-local x, VRAM rows */
    int16_t texture_x, texture_y;     /* VRAM texel of its top-left corner */
    int16_t clip_top, clip_bottom;    /* drawing area rows */
    uint8_t after_lift;
} GlNativeWaveTile;
typedef struct GlNativeWaveTransfer {
    uint16_t src_x, src_y, dst_x, dst_y, width, height;
} GlNativeWaveTransfer;
/* One target's wave transfers and tiles of the frame being drawn. */
typedef struct GlNativeWaveCapture {
    GlNativeWaveTile tiles[NATIVE_WAVE_TILES];
    uint8_t seen[NATIVE_WAVE_TILES];
    uint32_t count, transfer_count;
    GlNativeWaveTransfer transfers[NATIVE_WAVE_TRANSFERS];
    GlNativeWaveTransfer lift; /* valid when lifted */
    int lifted, invalid;
} GlNativeWaveCapture;
typedef struct GlNativeWaveRow {
    int16_t top, bottom, source_y, clip_top, clip_bottom; /* target-local rows */
    uint8_t after_lift; /* drawn after the lift rather than lifted by it */
    uint8_t present;    /* draws inside its drawing area */
} GlNativeWaveRow;
typedef struct GlNativeWaveGeometry {
    int16_t first_column; /* column of x[0]; negative inside the left margin */
    uint16_t boundary_count, row_count; /* rows[] is indexed by guest mesh row */
    int16_t x[NATIVE_WAVE_MAX_BOUNDARIES]; /* target-local boundary x */
    GlNativeWaveRow rows[NATIVE_WAVE_ROWS];
} GlNativeWaveGeometry;
/* Immutable once built; shared by the recipes that replay it. */
typedef struct GlNativeWaveWarp {
    uint32_t references;
    uint16_t view_width, offset; /* VIEW plane x = target-local x + offset */
    uint16_t origin_y;           /* VRAM row of target-local row 0 */
    GlNativeWaveTransfer lift;   /* applied to whole VIEW rows when lifted */
    int lifted;
    GlNativeWaveGeometry current, previous;
    int has_previous;
} GlNativeWaveWarp;

typedef struct GlNativeRecipe {
    uint32_t references;
    uint32_t count;
    uint32_t clear_color;
    /* Nonzero: the recipe starts from the target's captured contents (base
     * plane base_slot) instead of clear_color. */
    uint64_t base_serial;
    uint32_t base_slot;
    uint32_t texture_count;
    uint32_t motion_count;
    uint32_t motion_capacity;
    XgRenderMotionRef *motions;
    uint32_t coverage_count;
    uint32_t coverage_capacity;
    GlNativeRecipeCoverage *coverages;
    GlNativeCoverageState *publication;
    uint16_t width, height, view_width, view_height, offset, origin_x, origin_y;
    bool dithering_disabled;
    bool depth_test; /* Captured native_depth_test: replays keep a depth plane. */
    GlNativeRecipePixels *textures;
    /* Words depending on this replay's framebuffer, including COPY/draw aliases.
     * External rendered textures are otherwise immutable discrete inputs. */
    GpuVramRegionSet feedback;
    /* Recent feedback-region answers for texture page and CLUT rectangles,
     * which consecutive draws keep asking; every feedback write clears it. */
    struct { int16_t x, y, w, h; uint8_t result; } feedback_cache[4];
    uint32_t feedback_cache_count, feedback_cache_next;
    uint32_t draw_capacity;
    GlNativeRecipeDraw *draws;
    /* The Field wave runs before draw wave_at (after the last when equal). */
    GlNativeWaveWarp *wave;
    uint32_t wave_at;
} GlNativeRecipe;
typedef struct GlNativeMotionHistory {
    GlNativeRecipe *recipe;
    XgPresentationIdentity identity;
    XgSemanticSceneIdentity scene;
    XgSemanticDisplayState display;
    uint64_t pixel_digest;
    uint64_t reference_digest; /* Logical 1x scanout, not the GPU storage hash. */
    uint32_t storage_width;
    uint16_t crop_y;
    int valid;
} GlNativeMotionHistory;
typedef struct GlNativeMotionPrepared {
    GlNativeMotionHistory history;
    NativeImageSlot phases[GL_NATIVE_MOTION_PHASE_CAPACITY];
    uint32_t count;
    uint64_t interval_ns, previous_digest;
} GlNativeMotionPrepared;
typedef struct GlNativeMotionProjection {
    double screen[2][3][3], native[2][3][3]; /* native: x, y, view-Z deltas */
    XgRenderMotionProjectResult result;
} GlNativeMotionProjection;
typedef struct GlNativeMotionEntity {
    XgRenderMotionRef previous;
    const XgRenderMotionPose *previous_pose, *current_pose;
    XgRenderMotionEvaluation evaluation;
    const GlNativeMotionProjection *projections;
    uint64_t owner_scene;
    uint64_t owner_component;
    uint32_t owner_scope;
    uint32_t owner_producer;
    int enabled;
} GlNativeMotionEntity;
typedef struct GlNativeVertexSample {
    int64_t position[4], offset[2];
    int32_t view[3];
    int32_t depth; /* native_view_depth (Q12 view Z), zero when absent */
    uint16_t distance;
    uint8_t native, projective;
} GlNativeVertexSample;
typedef struct GlNativeVertexUse {
    uint64_t scene;
    uint64_t component;
    uint32_t scope;
    uint32_t group, vertex, draw, corner, mesh;
    GlNativeVertexSample sample;
    uint8_t frame;
} GlNativeVertexUse;
typedef struct GlNativeVertexMesh {
    uint64_t scene;
    uint64_t component;
    uint32_t scope;
    uint32_t producer, draw, parent;
    uint8_t bound, unbound, bad, enabled;
} GlNativeVertexMesh;
typedef struct GlNativeVertexPair {
    GlNativeVertexSample endpoints[2];
    uint32_t mesh;
    uint8_t valid;
    uint8_t anchor_recovered;
} GlNativeVertexPair;
typedef struct GlNativeVertexCache {
    GlNativeVertexPair *pairs;
    GlNativeVertexMesh *meshes;
    uint32_t (*map)[6];
    uint32_t *draw_mesh;
    int64_t (*deltas)[5]; /* canonical x/y, Native x/y, Native depth (Q12) */
    uint32_t pair_count, mesh_count;
} GlNativeVertexCache;
typedef struct GlNativeVertexDiagnostics {
    uint64_t vblank;
    uint64_t prepare_ns;
    uint32_t vertices, matched, missing, conflicts, active_meshes, discrete_meshes;
    uint32_t moved_vertices, phases;
} GlNativeVertexDiagnostics;
typedef struct GlNativeRecipeReplay {
    const GlNativeRecipe *recipe;
    uint32_t *pixels;
    uint16_t crop_y, height;
    int use_view, ok;
} GlNativeRecipeReplay;
typedef struct GlNativeRecipeStats {
    uint32_t temporal_status, motion_projected_draws;
} GlNativeRecipeStats;
typedef struct GlNativePhaseProducer {
    XgPresentationIdentity identity;
    uint32_t phase, producer, draws, moved_draws, moved_vertices;
} GlNativePhaseProducer;
typedef struct GlNativeRecipeStripe {
    const GlNativeRecipe *recipe;
    const GlNativeMotionEntity *entities;
    const GlNativeVertexCache *vertices;
    uint32_t phase, *pixels;
    uint16_t crop_y, height, begin, end;
    int use_view, ok;
    GlNativeRecipeStats stats;
} GlNativeRecipeStripe;
typedef struct GlNativeMotionRejectEvent {
    const char *reason;
    uint64_t entity_id, source_update;
    uint32_t draw_index, producer_id;
} GlNativeMotionRejectEvent;
#define GL_NATIVE_VIEW_TARGET_CAPACITY 64u
typedef struct GlNativeViewDomain {
    uint32_t *pixels; /* One extended framebuffer, addressed by physical VRAM Y. */
    uint16_t x, width;
    uint8_t initialized[VRAM_H];
} GlNativeViewDomain;
typedef struct GlNativeViewRaster {
    uint16_t x, y, width, height;
    uint32_t *pixels;
    uint32_t *depth; /* Optional depth plane indexed like pixels. */
} GlNativeViewRaster;
typedef struct GlNativeViewTarget {
    uint16_t x, y, width, height;
    int transition_snapshot; /* 1: certified capture; 2: pending CPU round trip. */
    uint64_t declaration;
    uint32_t *pixels;
    GlNativeWaveCapture wave;
    GlNativeRecipe *recipe;
    /* Draws of its recent recipes: the next recipe's initial reserve, even
     * when the previous one was dropped before the new frame began. */
    uint32_t recipe_draws;
    /* Covered the scanout since its recipe began: the next declaration of a
     * snapshot-based target starts a new frame. */
    int displayed;
} GlNativeViewTarget;
typedef struct GlNativeViewState {
    uint32_t domain_count;
    uint32_t count;
    int active;
    uint16_t width, reference_height, offset;
    uint16_t temporal_hz;
    uint64_t declaration_sequence;
    uint64_t owned; /* transaction-private DOMAIN allocations only */
    uint64_t write_serial;
    uint64_t word_versions[VRAM_H][GPU_VRAM_REGION_WORDS_PER_ROW];
    uint64_t word_page_versions[2][GPU_VRAM_REGION_WORDS_PER_ROW];
    GlNativeCoverageState *publication;
    GpuVramRegionSet raster_words;
    int reset_motion_history;
    /* Captured native_depth_test. Each DOMAIN allocation then holds its depth
     * plane after the pixels, so COW/retention/free cover both together. */
    int depth_test;
    uint64_t readback_rgb_digest;
    uint16_t readback_x, readback_y, readback_width, readback_height;
    int readback_capture;
    struct GlNativeGpuWork *gpu; /* Private compiler journal; never published. */
    /* The last source frame's wave mesh: the start of the next interval. */
    GlNativeWaveGeometry wave_previous;
    uint64_t wave_previous_vblank;
    GlNativeViewTarget targets[GL_NATIVE_VIEW_TARGET_CAPACITY];
    GlNativeViewDomain domains[GL_NATIVE_VIEW_TARGET_CAPACITY];
} GlNativeViewState;

#define GL_NATIVE_GPU_PHASE_BASE (GL_NATIVE_VIEW_TARGET_CAPACITY + 1u)
#define GL_NATIVE_GPU_PLANES (GL_NATIVE_GPU_PHASE_BASE + GL_NATIVE_MOTION_PHASE_CAPACITY)
typedef enum GlNativeGpuOp { NATIVE_GPU_SEED, NATIVE_GPU_DRAW, NATIVE_GPU_WORDS, NATIVE_GPU_SNAPSHOT, NATIVE_GPU_SPAN,
    NATIVE_GPU_WORDS_RESET, NATIVE_GPU_BASE, NATIVE_GPU_CANONICAL_READBACK, NATIVE_GPU_TEMPLATE,
    NATIVE_GPU_PROVENANCE_CLEAR,
    /* mesh_count SPAN rectangles (GlNativeGpuSpanRect at data) sharing one
     * plane, source and mask, in one draw; x/y/w/h bound them all. */
    NATIVE_GPU_SPANS } GlNativeGpuOp;
typedef struct GlNativeGpuSpanRect {
    float x, y, w, h; /* fractional edges rasterize by pixel centre */
    float sx, sy, sw, sh;
} GlNativeGpuSpanRect;
/* SPAN sources at or above this index sample a persistent recipe base plane
 * (one per VIEW target) instead of a same-work snapshot. */
#define GL_NATIVE_GPU_BASE_SOURCE GL_NATIVE_GPU_PLANES
/* One triangle of a mesh DRAW, with its own texture state. */
typedef struct GlNativeGpuMeshTriangle {
    XgRenderIrTriangle triangle;
    GlNativeTextureState texture;
} GlNativeGpuMeshTriangle;

typedef struct GlNativeGpuCommand {
    GlNativeGpuOp kind;
    uint32_t plane, source, data, color, mask;
    /* A mesh DRAW: mesh_count GlNativeGpuMeshTriangle at `data` in its
     * journal's data blob, under draw's material with each triangle's own
     * texture state; draw.primitive holds none. */
    uint32_t mesh_count;
    uint32_t texture_snapshot;
    uint32_t template_record;
    int x, y, w, h;
    float sx, sy, sw, sh;
    XgSemanticDrawRecord draw;
} GlNativeGpuCommand;

/* The per-triangle material flags vertex and raster setup read, without
 * materializing the whole material. */
typedef struct GlNativeTriangleFlags { uint8_t textured, gouraud, semi_transparent; } GlNativeTriangleFlags;
typedef struct GlNativeGpuGeometry GlNativeGpuGeometry;
typedef struct GlNativeGpuChunk {
    GlNativeGpuCommand *commands;
    uint8_t *data;
    uint32_t count, bytes;
    GlNativeGpuGeometry *geometry;
} GlNativeGpuChunk;
typedef struct GlNativePhaseTextures {
    uint16_t *words;
    uint32_t *layers;
    uint32_t count;
} GlNativePhaseTextures;
typedef struct GlNativePhaseTemplates {
    GlNativeGpuCommand *commands;
    uint8_t *data;
    GlNativeGpuGeometry *geometry;
    uint32_t *record_commands;
    uint8_t *phase_vertices_needed;
    uint32_t count;
} GlNativePhaseTemplates;

typedef struct GlNativeGpuTemplateCommand {
    uint32_t first, count, visible;
    unsigned depth;
    int left, top, right, bottom, scissor[4];
    uint64_t depth_tested, filters[2][2];
} GlNativeGpuTemplateCommand;
typedef struct GlNativeGpuTemplateParameters {
    GlNativeGpuTemplateCommand *commands;
    uint8_t *visible;
    uint32_t triangles;
} GlNativeGpuTemplateParameters;
typedef struct GlNativeWorkTiming {
    uint64_t begin_ns, compile_cpu_ns, queued_ns, dispatch_begin_ns, dispatch_end_ns;
    uint64_t ready_ns, hash_begin_ns, hash_end_ns, hash_cpu_ns, end_ns;
    uint64_t service_ns, service_cpu_ns, copy_ns;
    XgPresentationIdentity identity;
    uint64_t digest;
    uint64_t deadline_ns, previous_vblank, previous_cycle;
    uint64_t native_cpu_ns, phase_cpu_ns, phase_begin_ns, reference_digest;
    uint64_t phase_project_cpu_ns, phase_capture_cpu_ns, phase_geometry_cpu_ns;
    uint64_t phase_digests[GL_NATIVE_MOTION_PHASE_CAPACITY];
    uint64_t service_prepare_cpu_ns, service_draw_cpu_ns, service_scanout_cpu_ns, canonical_read_cpu_ns;
    uint64_t image_alloc_cpu_ns;
    uint32_t width, height, scale, commands, phases, gpu;
    uint32_t gpu_setup_triangles;
    uint32_t temporal_status, fresh, operations;
} GlNativeWorkTiming;
typedef struct GlNativeGpuWork {
    GlNativeWorkTiming timing;
    XgRenderSourceCommitHandle commit;
    XgPresentationIdentity identity;
    uint32_t scale, count, capacity, bytes, data_capacity;
    uint32_t render_width, render_height; /* exact host raster, zero: integer scale */
    float raster_scale[2]; /* physical pixels per virtual raster pixel */
    uint32_t raster_page_rows; /* guest rows between framebuffer origins */
    uint32_t raster_page_pixels; /* integer physical distance between those origins */
    uint32_t published_count;
    GlNativeGpuChunk phase_chunks[GL_NATIVE_MOTION_PHASE_CAPACITY];
    uint32_t chunk_count, published_chunk_count, chunk_cursor, phase_cursor, phase_command_count;
    uint64_t phase_bytes;
    const GlNativeGpuCommand *published_commands;
    const uint8_t *published_data;
    XgRenderRetiredBuffer *retired_buffers;
    uint16_t published_widths[GL_NATIVE_GPU_PLANES], published_heights[GL_NATIVE_GPU_PLANES];
    int sealed, cancel_requested, render_failed;
    int phase_stream_ready; /* Complete immutable base published; phase resources/chunks may still arrive. */
    int deferred; /* CPU-prepared successor: no GL publication before FIFO promotion. */
    const GlNativePhaseTextures *phase_textures;
    int owns_phase_textures;
    int phase_textures_uploaded;
    const GlNativePhaseTemplates *phase_templates;
    int owns_phase_templates, phase_templates_uploaded;
    GlNativeGpuTemplateParameters *template_parameters;
    int template_parameters_attempted;
    int capture_templates;
    int gpu_setup;
    const uint8_t *static_records;
    uint32_t template_record;
    uint32_t initial_words_offset;
    int canonical_gpu;
    uint32_t *canonical_pixels; /* GL-owned readback storage; never aliases the CPU canvas. */
    uint32_t *canonical_provenance; /* Exact fragment writes, from canonical attachment 1. */
    uint32_t canonical_requested, canonical_completed;
    GlNativeGpuCommand *commands;
    uint8_t *data;
    GlNativeGpuGeometry *geometry; /* Private phase worker preparation, moved with its journal. */
    uint16_t *words;
    uint16_t dirty_word_rows[VRAM_H]; /* One bit per 64-word canonical block. */
    uint16_t dirty_texture_pages[2]; /* Conservative union of the rows above. */
    /* Compiler-only synchronization of immutable replay snapshots. A texture
     * page is 64 words by 256 rows; CLUTs share the same word blocks. */
    const uint16_t *replay_page_sources[2][GPU_VRAM_REGION_WORDS_PER_ROW];
    const uint16_t *replay_block_sources[VRAM_H][GPU_VRAM_REGION_WORDS_PER_ROW];
    uint16_t widths[GL_NATIVE_GPU_PLANES], heights[GL_NATIVE_GPU_PLANES];
    uint8_t base_planes[GL_NATIVE_GPU_PHASE_BASE]; /* Planes retained by this exact CPU base. */
    struct GlNativeGpuExec *exec; /* GL objects; NULL until the executor's first slice. */
    uint32_t image_widths[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u], image_heights[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u]; /* scanout image sizes, set by the executor */
    uint32_t cursor;
    uint8_t *readback_pixels[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u];
    size_t readback_capacity[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u];
    uint64_t image_digests[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u];
    uint64_t geometry_draws, transfer_draws, visible_pixels;
    uint64_t filter_draws[2][2];
    uint32_t *reference_pixels; /* Optional logical-grid samples of the GPU image. */
    int row_planes[VRAM_H];
    uint16_t scanout_x, scanout_y, scanout_width, scanout_height, phase_crop_y, phase_crop_x;
    uint16_t scanout_canonical_width, scanout_offset;
    uint32_t phase_count;
    double wave_phase; /* A phase journal's place in its interval, (0, 1]. */
    int state, failed, fresh, scanout, depth24, disabled, scanout_unchanged;
    uint32_t fail_stage, fail_command, fail_kind, fail_gl_error; /* first failure: GL_RENDERER_NATIVE_GPU_FAILURE_* */
    int depth_test; /* VIEW/phase planes carry a depth attachment (never plane 0). */
    uint64_t submitted_ns, word_uploads, snapshot_commands;
    uint64_t destination_barriers, destination_copies;
    uint64_t skipped_barriers;       /* destination reads with no overlapping write */
    uint64_t scheduled_layers;       /* barrier-separated layers of scheduled DRAW runs */
    int depth_view;                  /* GL_NATIVE_DEPTH_VIEW_* captured at creation */
    int wireframe;                   /* debug wireframe captured at creation */
    int depth_view_log[2];           /* depth view range: log2 of min/max key, x256 */
    /* GPU depth telemetry: triangles depth-tested (their occlusion queries
     * live in the executor state). */
    uint64_t depth_tested_triangles;
} GlNativeGpuWork;
/* The GL executor as the compile and presenter side see it. Installed by
 * gl_renderer_native_init_services before the GPU thread starts. */
typedef struct NativeGpuBackend {
    int  (*plane_resident)(unsigned plane);              /* compile thread, no lock */
    /* Size of the work's plane on the executor; 0 for a plane it has not created. */
    void (*plane_extent)(const GlNativeGpuWork *work, unsigned plane, uint32_t *width, uint32_t *height);
    void (*apply_planes)(GlNativeGpuWork *work);         /* s_native_state_mutex held, work READY */
    void (*adopt_images)(GlNativeEndpointSlot *endpoint, GlNativeGpuWork *work,
                         uint32_t phase_count);          /* same lock, scanout work */
    XgRenderFenceStatus (*sync_poll)(void *sync);
    int  (*sync_wait_delete)(void *sync);
    int  (*upload_staged)(NativeImageSlot *image, int width, int height, const uint32_t *pixels);
    void (*image_destroy)(NativeImageSlot *image);
} NativeGpuBackend;
extern const NativeGpuBackend *g_native_backend;
/* Opt-in one-endpoint evidence. Own CPU buffers/recipe retains only; never a
 * render input, GL resource owner, deadline override, or extra source batch. */
typedef struct GlNativeMotionProbe {
    uint64_t target_vblank;
    XgPresentationIdentity identity, previous_identity, previous_image_identity;
    GlNativeRecipe *recipe, *previous_recipe;
    GlNativeVertexCache vertices;
    GlNativeMotionEntity *entities;
    GlNativeMotionProjection *projections;
    GlNativeGpuCommand *commands;
    uint32_t first_command, count, phase_count, scanout_height;
    uint32_t image_widths[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u], image_heights[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u];
    uint8_t *readback_pixels[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u];
    uint64_t image_digests[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u];
    uint8_t *previous_pixels;
    uint32_t previous_width, previous_height;
    uint64_t previous_digest;
} GlNativeMotionProbe;
/* Protected by s_native_state_mutex: queued -> dispatching -> submitted -> ready -> published.
 * The worker never executes GL or waits for the owner while inside compile. */
enum { NATIVE_GPU_QUEUED = 1, NATIVE_GPU_SUBMITTED, NATIVE_GPU_READY, NATIVE_GPU_PUBLISHED, NATIVE_GPU_CANCELLED, NATIVE_GPU_DISPATCHING, NATIVE_GPU_WAIT_INPUT };
/* Bounded worker diagnostics, inspectable without trapping every GPU packet. */
typedef struct GlNativeRecipeDropEvent {
    const char *caller;
    uint32_t line, draws, motions, textures;
    uint16_t x, y, width, height;
    const char *texture_reason;
    int texture_x, texture_y;
    GpuRenderMaterial material;
} GlNativeRecipeDropEvent;

typedef struct GlNativeGpuPending {
    GlNativeCompileAudit *audit;
    GlNativeViewState *views;
    GlNativeMotionPrepared motion;
    uint32_t *pixels, *vram;
    uint16_t *words;
    int needs_endpoint, endpoint_index, fence_index, fence_reserved;
} GlNativeGpuPending;
typedef struct GlNativePrefetch {
    GlNativeGpuPending prepared;
    GlNativeGpuWork *gpu;
    XgRenderSourceCommitHandle commit;
    XgPresentationIdentity base_identity;
    uint64_t cpu_ns;
    int leased;
} GlNativePrefetch;
#define GL_NATIVE_PREFETCH_CAPACITY 3u

typedef struct GlNativeAttributePlanes {
    double x, y;
    double plane[5][3]; /* UV, RGB: seed, logical d/dx, logical d/dy. */
    uint32_t dda[5][3]; /* Q12 seed and physical-pixel steps, modulo 256. */
    int integral;
    /* Inclusive sampled texel range {lo_u, lo_v, hi_u, hi_v} (gpu_uv.h). */
    int limits[4];
} GlNativeAttributePlanes;
enum { GL_NATIVE_DEPTH_KEEP = 0, GL_NATIVE_DEPTH_RESET = 1, GL_NATIVE_DEPTH_WRITE = 2 };
/* Debug presentation of the GPU planes (gl_renderer_set_native_depth_view). */
enum { GL_NATIVE_DEPTH_VIEW_OFF = 0, GL_NATIVE_DEPTH_VIEW_DEPTH = 1, GL_NATIVE_DEPTH_VIEW_POLICY = 2,
       GL_NATIVE_DEPTH_VIEW_DEPTH_GREY = 3 };
/* Policy colours of the policy view: NONE, TEST, TEST_WRITE, stamped but
 * without a usable depth plane on this triangle. */
enum { GL_NATIVE_DEPTH_SHOW_NONE = 0, GL_NATIVE_DEPTH_SHOW_TEST = 1,
       GL_NATIVE_DEPTH_SHOW_WRITE = 2, GL_NATIVE_DEPTH_SHOW_FALLBACK = 3 };
typedef struct GlNativeDepthPlane { float n0, a, b; int32_t ox, oy; } GlNativeDepthPlane;
/* write is the mode of an unblended fragment; semi marks per-texel blending,
 * whose blended fragments keep the stored key. */
typedef struct GlNativeDepthMode {
    int test, write, semi, bias, show;
    GlNativeDepthPlane plane;
} GlNativeDepthMode;

typedef struct GlNativeGpuDrawGeometry {
    GlNativeAttributePlanes attributes;
    GlNativeDepthMode depth;
    int scissor[4], left, top, right, bottom, visible;
} GlNativeGpuDrawGeometry;
typedef struct GlNativeGpuGeometryOffset {
    uint32_t vertex, primitive;
} GlNativeGpuGeometryOffset;
struct GlNativeGpuGeometry {
    float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS];
    GlNativeGpuGeometryOffset *offsets;
    GlNativeGpuDrawGeometry *primitives;
    uint32_t command_count;
    int gpu_setup;
};

/* ---- state and functions shared between translation units ---- */
extern SDL_Thread *s_native_gpu_thread;
extern SDL_cond *s_native_gpu_condition;
extern SDL_atomic_t s_native_gpu_run,s_native_gpu_status;
extern void (*s_native_gpu_notify)(void *);
extern void *s_native_gpu_notify_data;
extern int s_native_active;
extern int s_tex_filter;
extern int s_sprite_filter;
extern unsigned int s_native_phase_budget;
extern uint64_t s_native_phase_cost_per_draw[3];
extern SDL_mutex *s_native_state_mutex;
extern GlNativeEndpointSlot s_native_endpoints[GL_NATIVE_ENDPOINT_CAPACITY];
extern GlNativeFenceSlot s_native_fences[GL_NATIVE_FENCE_CAPACITY];
extern uint64_t s_native_backend_generation;
extern uint64_t s_native_temporal_generation;
extern uint32_t *s_native_native_vram;
extern uint16_t *s_native_native_words;
void native_canvas_spare_clear(void);
extern XgPresentationIdentity s_native_native_vram_identity;
extern GlNativeMotionHistory s_native_motion_history;
extern int s_native_recipe_audit_enabled;
extern int s_native_phase_hashes_enabled;
extern int s_native_record_audit_enabled;
extern int s_native_depth_stats_enabled;
extern int s_native_phase_stats_enabled;
extern uint32_t s_native_phase_producer_count;
extern volatile uint64_t s_native_coverage_acquires,s_native_coverage_releases,s_native_coverage_release_errors;
extern volatile uint64_t s_native_coverage_publications;
extern GlNativeViewState s_native_views;
extern SDL_atomic_t s_native_gpu_setup_ready;
extern uint32_t s_native_gpu_snapshot_capacity;
extern uint64_t s_native_work_timing_count;
extern uint64_t s_native_timing_origin;
uint64_t native_thread_cpu_ns(void);
void native_work_timing_dump(void);
void native_gpu_buffers_free(GlNativeGpuWork *work);
extern GlNativeMotionProbe s_native_motion_probe;
extern GlNativeGpuWork *s_native_gpu_work;
extern uint8_t *s_native_gpu_readback_spare[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u];
extern size_t s_native_gpu_readback_capacity[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u];
extern uint32_t s_native_gpu_scale;
extern int s_native_gpu_canonical;
extern uint64_t s_native_pending_present_sequence;
extern int s_native_pending_present_valid;
extern GlRendererNativeCompilerDiagnostics s_native_compiler_diag;
extern GlRendererNativePipelineDiagnostics s_native_pipeline_diag;
uint64_t pres_hash_bytes(const uint8_t *bytes, size_t size);
uint64_t pres_hash_pixels_visible(const void *pixels, size_t count, uint64_t *visible);
int dirty_intersects_wrapped(const DirtyRect *dirty, int x, int y, int w, int h);
extern SDL_atomic_t s_native_depth_view_option;
extern int s_native_gpu_depth_view_applied;
extern int s_native_depth_view_log_min,s_native_depth_view_log_max;
extern SDL_atomic_t s_native_wireframe_option;
extern int s_native_gpu_wireframe_applied;
uint64_t native_gpu_image_digest(const void *pixels, size_t size, uint64_t *visible);
int gl_renderer_scene_filter_strength(void);
int gl_renderer_anisotropy(void);
int gl_renderer_debug_mipmaps(void);
uint32_t native_next_generation(uint32_t generation);
void native_guest_reference_reset_locked(void);
int gl_renderer_native_guest_reference_enabled(void);
uint32_t native_endpoint_mismatch_mask( const XgRenderCompiledEndpoint *compiled, const GlNativeEndpointSlot *endpoint, uint32_t generation);
uint64_t native_pack_handle(uint32_t slot, uint32_t generation);
int native_unpack_handle(uint64_t handle, uint32_t capacity, uint32_t *slot, uint32_t *generation);
void native_gpu_fail_note(GlNativeGpuWork *work, uint32_t stage, uint32_t command, uint32_t kind, unsigned backend_code);
void native_gpu_fail_publish(const GlNativeGpuWork *work);
void native_endpoint_maybe_recycle_locked(uint32_t slot, uint32_t generation);
void native_publish_legacy_owner_locked(void);
void native_persistent_surfaces_clear(void);
int native_texture_window_coord(int coordinate, uint8_t mask, uint8_t offset);
void native_attribute_planes(const XgRenderIrTriangle *triangle, const double x[3], const double y[3], uint32_t scale, int gouraud, GlNativeAttributePlanes *out);
int32_t native_vertex_depth_q12(const XgRenderIrVertex *vertex);
GlNativeDepthMode native_depth_mode(const XgSemanticDrawRecord *draw, const XgRenderIrTriangle *triangle, const double x[3], const double y[3]);
void native_view_worker_shutdown(void);
void native_recipe_release(GlNativeRecipe *recipe);
void native_views_discard(GlNativeViewState *views, int all);
void native_recipe_reads_cache_clear(void);
void native_phase_pool_stop(void);
void native_motion_probe_discard(void);
uint32_t native_gpu_command_vertex_count(const GlNativeGpuCommand *command);
void native_gpu_prepare_draw(const GlNativeGpuWork *work, const GlNativeGpuCommand *command, const uint8_t *blob, uint32_t triangle, uint32_t width, uint32_t height, const float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS], GlNativeGpuDrawGeometry *out);
int native_gpu_build_vertex_slice(const GlNativeGpuWork *work, const GlNativeGpuCommand *commands, const uint8_t *blob, uint32_t begin, uint32_t end, float (**out_vertices)[GL_NATIVE_GPU_VERTEX_FLOATS], uint32_t *out_count);
void native_gpu_geometry_free(GlNativeGpuGeometry *geometry);
GlNativeGpuGeometry *native_gpu_geometry_prepare(const GlNativeGpuWork *work);
void native_prefetch_discard(void);
void native_gpu_pending_discard(void);
/* ---- backend-neutral Native presentation (native_presenter_select.c) ---- */
typedef struct NativePresentSel {
    uint32_t slot, generation, image_index;
    int upload_required, upload_result_recorded;
    uint32_t *staged_pixels;        /* owned until the backend's upload takes it, or abort restores it */
    NativeImageSlot image;          /* the selected image (colour/mask/size/state) */
    GlNativeEndpointSlot endpoint;  /* copy of the endpoint with the selected image's colour/target/digest */
} NativePresentSel;
NativeImageSlot native_endpoint_image(const GlNativeEndpointSlot *endpoint, uint32_t index);
void native_endpoint_store_image(GlNativeEndpointSlot *endpoint, uint32_t index, const NativeImageSlot *image);
float native_aa_halton(unsigned index, unsigned base);
/* Presenter thread. Validates the endpoint, takes presenter ownership and picks the
 * temporal image for alpha; false means nothing is held and the failure is counted. */
bool native_present_select(NativePresentSel *sel, const XgRenderCompiledEndpoint *compiled,
                           uint64_t alpha_numerator, uint64_t alpha_denominator,
                           uint64_t *out_completion_fence, int thread_ok);
/* After the backend tried the staged upload: records the result in the endpoint. Returns upload_ok. */
int native_present_upload_result(NativePresentSel *sel, int upload_ok);
/* The compose drew: releases presenter ownership and records the pending present. */
void native_present_commit(NativePresentSel *sel, uint64_t present_sequence);
/* The compose failed: restores the endpoint hold and counts the failure. */
void native_present_abort(NativePresentSel *sel, uint32_t blocker);
void native_letterbox_rect(int ox, int oy, int ow, int oh, int num, int den, int *x, int *y, int *w, int *h);
/* The letterboxed game rectangle of a window, with the host stretch override. */
void native_present_rect(const GlRendererNativeEndpointMetadata *metadata, int window_w, int window_h,
                         int stretch_num, int stretch_den, int *x, int *y, int *w, int *h);
/* The anti-aliasing mode actually applied to this endpoint (0 = plain) and whether FXAA runs nearest. */
int native_present_aa_mode(const GlRendererNativeEndpointMetadata *metadata, int mask_valid, int aa_setting, int *aa_nearest);
/* Fences, endpoint release and worker services shared by every Native backend. */
uint64_t native_register_fence(void *sync, GlNativeFenceKind kind, uint32_t endpoint_slot, uint32_t endpoint_generation);
XgRenderFenceStatus native_fence_status(uint64_t handle, void *user_data);
void native_discard_fence(uint64_t handle, void *user_data);
void native_release_endpoint(const XgRenderCompiledEndpoint *compiled, void *user_data);
int gl_renderer_native_neutral_begin(XgRenderWorkerServices *out_worker_services);
void gl_renderer_native_neutral_end(void);
void native_work_free(GlNativeGpuWork *work);
void gl_renderer_native_present_settings(int *aa_mode, int *aa_factor, int *stretch_num, int *stretch_den);
XgRenderCompileResult native_worker_compile(XgRenderSourceCommitHandle commit, XgRenderCompiledEndpoint *out_endpoint, uint64_t *out_compile_fence, void *user_data);

/* ---- inline helpers ---- */
static inline GlNativeTextureState native_texture_state(const XgRenderIrMaterialState *m) {
    return (GlNativeTextureState){m->tpage, m->texture_page_x, m->texture_page_y, m->clut_x, m->clut_y,
        (uint8_t)m->texture_depth, (uint8_t)m->shading, m->textured, m->raw_texture,
        m->semi_transparent, (uint8_t)m->blend_mode};
}

static inline void native_texture_state_apply(XgRenderIrMaterialState *m, const GlNativeTextureState *t) {
    m->tpage = t->tpage; m->texture_page_x = t->page_x; m->texture_page_y = t->page_y;
    m->clut_x = t->clut_x; m->clut_y = t->clut_y;
    m->texture_depth = (XgRenderIrTextureDepth)t->texture_depth;
    m->shading = (XgRenderIrShading)t->shading;
    m->textured = t->textured; m->raw_texture = t->raw_texture;
    m->semi_transparent = t->semi_transparent;
    m->blend_mode = (XgRenderIrBlendMode)t->blend_mode;
}

static inline int native_texture_state_equal(const GlNativeTextureState *a, const GlNativeTextureState *b) {
    return a->tpage == b->tpage && a->page_x == b->page_x && a->page_y == b->page_y &&
        a->clut_x == b->clut_x && a->clut_y == b->clut_y && a->texture_depth == b->texture_depth &&
        a->shading == b->shading && a->textured == b->textured && a->raw_texture == b->raw_texture &&
        a->semi_transparent == b->semi_transparent && a->blend_mode == b->blend_mode;
}

static inline uint32_t native_gpu_command_triangles(const GlNativeGpuCommand *command) {
    return command->mesh_count ? command->mesh_count : command->draw.primitive.triangle_count;
}

/* data: the blob of the journal (work, published slice or phase chunk) that
 * holds this command. */
static inline const XgRenderIrTriangle *native_gpu_command_triangle(
        const GlNativeGpuCommand *command, const uint8_t *data, uint32_t triangle) {
    return command->mesh_count
        ? &((const GlNativeGpuMeshTriangle *)(const void *)(data + command->data))[triangle].triangle
        : &command->draw.primitive.triangles[triangle];
}

static inline GlNativeTriangleFlags native_gpu_command_flags(const GlNativeGpuCommand *command,
                                                             const uint8_t *data, uint32_t triangle) {
    if (command->mesh_count) {
        const GlNativeTextureState *texture =
            &((const GlNativeGpuMeshTriangle *)(const void *)(data + command->data))[triangle].texture;
        return (GlNativeTriangleFlags){texture->textured != 0u,
            texture->shading == XG_RENDER_IR_SHADING_GOURAUD, texture->semi_transparent != 0u};
    }
    const XgRenderIrMaterialState *m = &command->draw.primitive.material;
    return (GlNativeTriangleFlags){m->textured != 0u, m->shading == XG_RENDER_IR_SHADING_GOURAUD,
        m->semi_transparent != 0u};
}

/* ---- pixel conversion (PS1 1555: bit15=mask, B[14:10] G[9:5] R[4:0]) ---- */
static inline uint32_t conv_1555_to_rgba8(uint16_t p) {
    uint32_t r = (p & 0x1F) << 3, g = ((p >> 5) & 0x1F) << 3, b = ((p >> 10) & 0x1F) << 3;
    uint32_t a = (p >> 15) & 1 ? 0xFF : 0;
    return r | (g << 8) | (b << 16) | (a << 24);   /* RGBA8 little-endian */
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline uint16_t native_native_texel(const uint16_t *words,
                                       const XgRenderIrMaterialState *material, int u, int v) {
    u = native_texture_window_coord(u, material->texture_window_mask_x, material->texture_window_offset_x);
    v = native_texture_window_coord(v, material->texture_window_mask_y, material->texture_window_offset_y);
    const uint32_t row = ((material->texture_page_y * 256u + (uint32_t)v) & (VRAM_H - 1)) * VRAM_W;
    const uint32_t page_x = material->texture_page_x * 64u;
    if (material->texture_depth == XG_RENDER_IR_TEXTURE_15_BIT)
        return words[row + ((page_x + (uint32_t)u) & (VRAM_W - 1))];
    const unsigned shift = material->texture_depth == XG_RENDER_IR_TEXTURE_4_BIT ? 2u : 1u;
    const uint16_t packed = words[row + ((page_x + ((uint32_t)u >> shift)) & (VRAM_W - 1))];
    const uint32_t index = shift == 2u ? (packed >> ((u & 3) * 4)) & 15u : (packed >> ((u & 1) * 8)) & 255u;
    return words[(material->clut_y & (VRAM_H - 1)) * VRAM_W + ((material->clut_x + index) & (VRAM_W - 1))];
}

/* Guest texture words always live on the 1x grid. VIEW planes retain the
 * selected presentation resolution independently. */
static inline uint32_t native_gpu_plane_scale(const GlNativeGpuWork *work, uint32_t plane) {
    return work->canonical_gpu && !plane ? 1u : work->scale;
}

/* Pure preparation shared by the serial endpoint path and parallel phase
 * workers. Attribute/depth planes use the original Q16 values in double,
 * while coverage uses exactly the floats uploaded to the vertex buffer. */
static inline int native_gpu_floor_position(float value) {
    const int whole=(int)value;
    return whole-(value<(float)whole);
}

static inline int native_gpu_ceil_position(float value) {
    const int whole=(int)value;
    return whole+(value>(float)whole);
}

#endif
