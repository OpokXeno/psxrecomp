#ifndef PSX_GPU_RENDER_H
#define PSX_GPU_RENDER_H

/* Renderer facade.  gpu.c (command processing) and main.cpp (present) call
 * these gr_* entry points instead of a specific backend.  The facade
 * dispatches to a selected backend: the software rasterizer (gpu_sw_renderer.c,
 * the default + fallback) or a hardware OpenGL backend (gpu_gl_renderer.c).
 *
 * The gr_* signatures mirror the software renderer's interface exactly, so a
 * backend is just a table of the same functions.  Select the backend with
 * gr_set_backend() BEFORE gr_init(); gr_backend() reports the EFFECTIVE backend
 * (a requested backend that fails to initialize falls back to software). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GR_BACKEND_SOFTWARE = 0,
    GR_BACKEND_OPENGL   = 1,
    GR_BACKEND_VULKAN   = 2
} GrBackend;

enum {
    GPU_RENDER_FIXED_FRACTION_BITS = 16,
    GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY = 2,
    GPU_RENDER_SEMANTIC_LINE_CAPACITY = 2,
    GPU_RENDER_DRAW_SUPPRESSION_MAX_DEPTH = 64
};

typedef int32_t GpuRenderFixed16_16;

typedef enum GpuRenderTransactionStatus {
    GPU_RENDER_TRANSACTION_OK = 0,
    GPU_RENDER_TRANSACTION_READY,
    GPU_RENDER_TRANSACTION_UNSUPPORTED,
    GPU_RENDER_TRANSACTION_INVALID_ARGUMENT,
    GPU_RENDER_TRANSACTION_INVALID_TRANSITION,
    GPU_RENDER_TRANSACTION_ORDER_REJECTED,
    GPU_RENDER_TRANSACTION_STATE_REJECTED,
    GPU_RENDER_TRANSACTION_VALIDATION_FAILED,
    GPU_RENDER_TRANSACTION_BACKEND_ERROR,
    GPU_RENDER_TRANSACTION_CONTEXT_LOST
} GpuRenderTransactionStatus;

typedef enum GpuRenderDrawSuppressionStatus {
    GPU_RENDER_DRAW_SUPPRESSION_OK = 0,
    GPU_RENDER_DRAW_SUPPRESSION_UNDERFLOW,
    GPU_RENDER_DRAW_SUPPRESSION_OVERFLOW,
    GPU_RENDER_DRAW_SUPPRESSION_POISONED
} GpuRenderDrawSuppressionStatus;

typedef enum GpuRenderTextureDepth {
    GPU_RENDER_TEXTURE_4_BIT = 0,
    GPU_RENDER_TEXTURE_8_BIT,
    GPU_RENDER_TEXTURE_15_BIT
} GpuRenderTextureDepth;

typedef enum GpuRenderBlendMode {
    GPU_RENDER_BLEND_AVERAGE = 0,
    GPU_RENDER_BLEND_ADD,
    GPU_RENDER_BLEND_SUBTRACT,
    GPU_RENDER_BLEND_ADD_QUARTER
} GpuRenderBlendMode;

typedef enum GpuRenderShading {
    GPU_RENDER_SHADING_FLAT = 0,
    GPU_RENDER_SHADING_GOURAUD
} GpuRenderShading;

typedef enum GpuRenderPresentPath {
    GPU_RENDER_PRESENT_CANONICAL = 0,
    GPU_RENDER_PRESENT_HIRES,
    GPU_RENDER_PRESENT_WIDE
} GpuRenderPresentPath;

/* A visual-state identity is opaque to the renderer. The facade only uses it
 * to reject calls that do not belong to the currently open transaction. */
typedef struct GpuRenderTransactionId {
    uint64_t scene_epoch;
    uint64_t state_sequence;
} GpuRenderTransactionId;

typedef uint64_t GpuRenderDeferredCandidateToken;

#define GPU_RENDER_DEFERRED_CANDIDATE_NONE UINT64_C(0)

typedef struct GpuRenderInterpolationIdentity {
    /* Also namespaces address-free retrospective matching when valid is zero. */
    uint64_t scene_id;
    uint32_t producer_id;
    uint32_t primitive_id;
    uint8_t valid;
} GpuRenderInterpolationIdentity;

typedef struct GpuRenderMaterial {
    uint16_t tpage;
    uint16_t texture_page_x;
    uint16_t texture_page_y;
    uint16_t clut_x;
    uint16_t clut_y;
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    GpuRenderTextureDepth texture_depth;
    uint8_t texture_window_mask_x;
    uint8_t texture_window_mask_y;
    uint8_t texture_window_offset_x;
    uint8_t texture_window_offset_y;
    GpuRenderShading shading;
    uint8_t textured;
    uint8_t raw_texture;
    uint8_t semi_transparent;
    GpuRenderBlendMode blend_mode;
    uint8_t dither;
    uint8_t mask_set;
    uint8_t mask_check;
} GpuRenderMaterial;

typedef struct GpuRenderSemanticVertex {
    GpuRenderFixed16_16 x;
    GpuRenderFixed16_16 y;
    GpuRenderFixed16_16 u;
    GpuRenderFixed16_16 v;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    /* Optional producer-derived position for a host Native view. Canonical
     * coordinates remain authoritative for guest VRAM and packet comparison. */
    GpuRenderFixed16_16 native_view_x;
    GpuRenderFixed16_16 native_view_y;
    uint8_t native_view_position;
    /* Optional pre-divide projection payload for temporal reprojection. */
    int32_t projective_view_x;
    int32_t projective_view_y;
    int32_t projective_view_z;
    GpuRenderFixed16_16 projective_offset_x;
    GpuRenderFixed16_16 projective_offset_y;
    GpuRenderFixed16_16 projective_native_offset_x;
    GpuRenderFixed16_16 projective_native_offset_y;
    uint16_t projective_distance;
    uint8_t projective_position;
    /* Scalar temporal depth for screen-space geometry that must not be
     * projectively reprojected (for example billboards). */
    int32_t temporal_depth;
    uint8_t temporal_depth_valid;
    /* Optional source-mesh identity. Vertices shared by separate primitives
     * use one temporal position even when primitive appearance snaps. */
    uint32_t interpolation_group_id;
    uint32_t interpolation_vertex_id;
    uint8_t interpolation_vertex_identity_valid;
} GpuRenderSemanticVertex;

typedef struct GpuRenderInterpolationVertexAnchor {
    uint64_t scene_id;
    uint32_t producer_id;
    GpuRenderMaterial material;
    GpuRenderSemanticVertex vertex;
} GpuRenderInterpolationVertexAnchor;

typedef struct GpuRenderSemanticTriangle {
    uint8_t split_index;
    uint8_t split_count;
    GpuRenderSemanticVertex vertices[3];
} GpuRenderSemanticTriangle;

typedef enum GpuRenderSemanticTopology {
    GPU_RENDER_SEMANTIC_TRIANGLES = 0,
    GPU_RENDER_SEMANTIC_LINES = 1,
} GpuRenderSemanticTopology;

typedef enum GpuRenderScreenSpace2dMode {
    GPU_RENDER_SCREEN_SPACE_2D_NONE = 0,
    GPU_RENDER_SCREEN_SPACE_2D_STRETCH = 1,
    GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE = 2,
} GpuRenderScreenSpace2dMode;

typedef enum GpuRenderNativeViewEffect {
    GPU_RENDER_NATIVE_VIEW_EFFECT_NONE = 0,
    GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID = 1,
} GpuRenderNativeViewEffect;

typedef struct GpuRenderSemanticLine {
    GpuRenderSemanticVertex vertices[2];
} GpuRenderSemanticLine;

/* Backend-neutral native primitive. The fixed capacity and 16.16 coordinates
 * directly cover a triangle or a quad split into two ordered triangles. A
 * producer may additionally provide a source-derived Native-view position. */
typedef struct GpuRenderSemantic {
    GpuRenderMaterial material;
    GpuRenderSemanticTopology topology;
    /* Canonical screen-space geometry mapped into the host aspect. Preserve-size
     * maps the nearest outer horizontal edge while retaining canonical dimensions.
     * Both modes are mutually exclusive with native_view_position vertices. */
    uint8_t screen_space_2d;
    /* Source-authenticated host effect. Canonical geometry and guest VRAM remain
     * unchanged; the Native renderer extends the effect into revealed margins. */
    uint8_t native_view_effect;
    /* Producer-local packet index for effects whose topology is stable while
     * linked-list order and per-frame geometry are not. */
    uint16_t native_view_effect_index;
    uint8_t triangle_count;
    GpuRenderSemanticTriangle triangles[GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY];
    uint8_t line_count;
    GpuRenderSemanticLine lines[GPU_RENDER_SEMANTIC_LINE_CAPACITY];
    /* Stable producer identity used only for retrospective host interpolation.
     * Packet addresses and draw ordinals are deliberately not identities: PSX
     * games routinely double-buffer packet arenas and reorder OT contents. */
    GpuRenderInterpolationIdentity interpolation_identity;
    /* Submission provenance for diagnostics only. Matching deliberately ignores
     * this double-buffered packet address. */
    uint64_t submission_command_id;
} GpuRenderSemantic;

typedef enum GpuRenderTemporalCullFlags {
    GPU_RENDER_TEMPORAL_CULL_PROJECTIVE = 1u << 0,
    GPU_RENDER_TEMPORAL_CULL_SCREEN = 1u << 1,
    GPU_RENDER_TEMPORAL_CULL_FRONT_FACE = 1u << 2,
    GPU_RENDER_TEMPORAL_CULL_DEPTH = 1u << 3,
    GPU_RENDER_TEMPORAL_FORCE_PHASES = 1u << 31,
} GpuRenderTemporalCullFlags;

typedef enum GpuRenderTemporalDepthMode {
    GPU_RENDER_TEMPORAL_DEPTH_NONE = 0,
    GPU_RENDER_TEMPORAL_DEPTH_MINIMUM,
    GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
    GPU_RENDER_TEMPORAL_DEPTH_AVERAGE,
    GPU_RENDER_TEMPORAL_DEPTH_LAST_VERTEX,
} GpuRenderTemporalDepthMode;

typedef enum GpuRenderTemporalFrontFace {
    GPU_RENDER_TEMPORAL_FRONT_NONE = 0,
    GPU_RENDER_TEMPORAL_FRONT_POSITIVE,
    GPU_RENDER_TEMPORAL_FRONT_NEGATIVE,
} GpuRenderTemporalFrontFace;

/* Visibility contract for geometry omitted by an authored endpoint. Bounds and
 * depth limits are inclusive/exclusive, matching the guest's usual cull tests.
 * The backend evaluates this contract independently for every generated phase;
 * it never adds the candidate to the authoritative current-frame surface. */
typedef struct GpuRenderTemporalCullPolicy {
    uint32_t flags;
    GpuRenderFixed16_16 screen_left;
    GpuRenderFixed16_16 screen_top;
    GpuRenderFixed16_16 screen_right_exclusive;
    GpuRenderFixed16_16 screen_bottom_exclusive;
    int32_t depth_min_inclusive;
    int32_t depth_max_exclusive;
    GpuRenderTemporalDepthMode depth_mode;
    GpuRenderTemporalFrontFace front_face;
    uint8_t ordering_depth_shift;
    uint8_t reserved;
} GpuRenderTemporalCullPolicy;

/* Describes the final composition without exposing a backend surface handle.
 * The backend owns the transaction's offscreen surface and its storage. */
typedef struct GpuRenderPresent {
    GpuRenderPresentPath path;
    int32_t display_x;
    int32_t display_y;
    int32_t display_width;
    int32_t display_height;
    uint32_t surface_width;
    uint32_t surface_height;
    int32_t wide_base_x;
    uint32_t scale;
    uint8_t linear_filter;
    uint8_t force_4_3;
    uint8_t reserved[2];
} GpuRenderPresent;

void      gr_set_backend(GrBackend backend);  /* call before gr_init() */
GrBackend gr_backend(void);                   /* effective backend after init */

/* Lifecycle / global state */
void gr_init(uint16_t *vram);
void gr_set_scale(int scale);
int  gr_scale(void);
void gr_set_texture_filter(int bilinear);
int  gr_texture_filter(void);

/* Per-primitive draw state */
void gr_set_semi_transparency(int enabled, int mode);
void gr_set_mask_bits(int set_bit, int check_bit);
void gr_set_texture_window(uint32_t raw);
void gr_set_color_modulation(int r, int g, int b, int raw_texture);
void gr_set_dither(int enabled);

/* Sub-pixel vertex override for the NEXT triangle ([video] geometry_correction).
 * x/y are signed 16.16 screen coordinates carrying the projection fraction the
 * GTE discarded; the integer positions still passed to gr_draw_*_triangle
 * remain authoritative for anything that must stay faithful (dirty-rect
 * tracking, the software path's native VRAM write). enabled == 0 clears the
 * override. Backends that do not implement it ignore the call, so the integer
 * path is what they draw. The override is consumed by the next triangle. */
void gr_set_precise_triangle(int enabled,
                             int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1,
                             int32_t x2, int32_t y2);
/* Perspective-correct UV weights for the NEXT triangle ([video]
 * perspective_texturing). q[i] is the normalized 1/z homogeneous weight at
 * vertex i. enabled == 0 restores the PS1's affine UV interpolation. */
void gr_set_perspective_triangle(int enabled, float q0, float q1, float q2);

/* Primitives */
/* Nest-safe suppression for canonical parser raster side effects. Underflow or
 * overflow poisons the scope and keeps draws suppressed for fail-closed safety. */
GpuRenderDrawSuppressionStatus gr_draw_suppression_begin(void);
GpuRenderDrawSuppressionStatus gr_draw_suppression_end(void);
int gr_draw_suppression_active(void);

void gr_fill_rect(int x, int y, int w, int h, uint16_t color);
void gr_copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h);
void gr_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                           uint16_t color);
void gr_draw_gouraud_triangle(int x0, int y0, uint16_t c0,
                              int x1, int y1, uint16_t c1,
                              int x2, int y2, uint16_t c2);
/* RGB888 polygon entry points preserve the GP0/semantic color precision until
 * the backend's PS1 dither + 15-bit quantization stage. Backends without an
 * exact high-precision path fall back to the legacy RGB555 callbacks. */
void gr_draw_flat_triangle_rgb888(int x0, int y0, int x1, int y1,
                                  int x2, int y2, uint32_t color);
void gr_draw_gouraud_triangle_rgb888(int x0, int y0, uint32_t c0,
                                     int x1, int y1, uint32_t c1,
                                     int x2, int y2, uint32_t c2);
void gr_draw_textured_triangle(int x0, int y0, int u0, int v0,
                               int x1, int y1, int u1, int v1,
                               int x2, int y2, int u2, int v2,
                               uint16_t clut_x, uint16_t clut_y,
                               uint16_t texpage);
void gr_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0,
                                      uint32_t color0,
                                      int x1, int y1, int u1, int v1,
                                      uint32_t color1,
                                      int x2, int y2, int u2, int v2,
                                      uint32_t color2,
                                      uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage, int raw_texture);
void gr_draw_flat_rect(int x, int y, int w, int h, uint16_t color);
void gr_draw_textured_rect(int x, int y, int w, int h,
                           int u, int v,
                           uint16_t clut_x, uint16_t clut_y,
                           uint16_t texpage);
void gr_draw_textured_rect_scaled(int x, int y, int w, int h,
                                  int u0, int v0, int u1, int v1,
                                  uint16_t clut_x, uint16_t clut_y,
                                  uint16_t texpage);
void gr_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
void gr_draw_shaded_line(int x0, int y0, uint16_t c0,
                         int x1, int y1, uint16_t c1);

/* Direct Native backend entry points for operations that do not use the
 * semantic triangle stream. These deliberately bypass the canonical parser
 * suppression scope and never invoke the GP0 parser again. */
void gr_native_fill_rect(int x, int y, int w, int h, uint16_t color);
void gr_native_copy_rect(int src_x, int src_y, int dst_x, int dst_y,
                         int w, int h);
void gr_native_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
void gr_native_draw_shaded_line(int x0, int y0, uint16_t c0,
                                int x1, int y1, uint16_t c1);

/* Authoritative Native stream operations. Unlike the legacy transaction API,
 * these mutate the active backend surface immediately in GP0 ingress order. */
GpuRenderTransactionStatus gr_stream_barrier(void);
GpuRenderTransactionStatus gr_draw_semantic_immediate(
    const GpuRenderSemantic *semantic);
GpuRenderTransactionStatus gr_draw_semantic_temporal_candidate(
    const GpuRenderSemantic *semantic,
    const GpuRenderTemporalCullPolicy *policy);
GpuRenderTransactionStatus gr_record_interpolation_anchors(
    const GpuRenderInterpolationVertexAnchor *anchors, size_t count);

/* Display readout (present path) */
int gr_render_display(uint32_t *out_pixels, int out_pitch,
                      int disp_x, int disp_y, int disp_w, int disp_h);
int gr_render_display_hires(uint32_t *out_pixels, int out_pitch,
                            int disp_x, int disp_y, int disp_w, int disp_h);
/* Native presentation paths. The CPU frame path is used for packed RGB888
 * MDEC output; it uploads the already-decoded frame without bypassing the
 * guest MDEC/DMA/VRAM state. */
int gr_present_vram(int disp_x, int disp_y, int disp_w, int disp_h,
                    int linear, int force_4_3);
int gr_present_cpu_frame(const uint32_t *pixels, int src_w, int src_h,
                         int linear, int force_4_3, int content_w);
/* Independent Native FMV presentation. This is deliberately a separate
 * backend operation, not a wrapper around the legacy CPU present path. */
int gr_present_native_cpu_frame(const uint32_t *pixels, int src_w, int src_h,
                                int linear, int force_4_3, int content_w);
/* Hashes the displayed rectangle of the active backend's canonical RGBA8
 * render source in top-down order. Returns zero when no authoritative host
 * surface exists or the rectangle is invalid. */
int gr_canonical_framebuffer_digest(int display_x, int display_y,
                                    int display_width, int display_height,
                                    uint64_t *out_digest);

/* VRAM transfers */
void gr_vram_write(int x, int y, uint16_t pixel);
uint16_t gr_vram_read(int x, int y);
void gr_vram_transfer_in(int x, int y, int w, int h, const uint16_t *data);
void gr_vram_transfer_out(int x, int y, int w, int h, uint16_t *data);

/* Draw area / offset */
void gr_set_draw_area(int x1, int y1, int x2, int y2);
void gr_get_draw_area(int *x1, int *y1, int *x2, int *y2);
void gr_set_draw_offset(int x, int y);

/* Native-wide compositor. gr_wide_supported() reports whether the active
 * backend implements it; if not, the others are no-ops and the caller keeps
 * the canonical present path. */
int  gr_wide_supported(void);
void gr_wide_configure(int wide_w, int offset);
void gr_wide_set_target(int base_x);
void gr_wide_disable_target(void);
void gr_wide_clear(int base_x, int y, int h, uint16_t color);
/* sides: bit 0 = left synthetic margin, bit 1 = right. */
void gr_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides);
/* Producer-driven Native view: clear only the synthetic side columns of a
 * retiring display band. Backends without a Native view ignore this call. */
void gr_native_view_clear_margins(int base_x, int y, int h, uint16_t color);
int  gr_wide_dump_full(uint32_t *out, int cap_pixels, int *ow, int *oh, int base_x);
int  gr_render_wide_display(uint32_t *out, int pitch, int base_x,
                            int disp_y, int disp_h);

/* Atomic semantic-render transaction. A successful commit returns READY, not
 * OK. Once READY is returned, the caller must immediately perform the backend
 * presentation operation. Any other open-transaction failure requires
 * gr_rollback() with the original transaction id. A barrier is required before
 * the first semantic draw and after the last semantic draw before commit. */
GpuRenderTransactionStatus gr_transaction_begin(
    GpuRenderTransactionId transaction_id,
    uint64_t vram_mutation_serial);
GpuRenderTransactionStatus gr_ordering_barrier(
    GpuRenderTransactionId transaction_id);
GpuRenderTransactionStatus gr_draw_semantic(
    GpuRenderTransactionId transaction_id,
    const GpuRenderSemantic *semantic);
GpuRenderTransactionStatus gr_commit_validate(
    GpuRenderTransactionId transaction_id,
    uint64_t current_vram_mutation_serial,
    const GpuRenderPresent *present);
GpuRenderTransactionStatus gr_rollback(
    GpuRenderTransactionId transaction_id);

/* A deferred candidate is a backend-owned, full-frame color surface captured
 * from an open transaction before strict guest-observation rollback. The token
 * is opaque, process-local, and single-use. Only a distinct deferred
 * transaction may consume it; software and Vulkan leave these unsupported. */
GpuRenderTransactionStatus gr_deferred_candidate_capture(
    GpuRenderTransactionId transaction_id,
    GpuRenderDeferredCandidateToken *out_candidate_token);
GpuRenderTransactionStatus gr_deferred_candidate_discard(
    GpuRenderDeferredCandidateToken candidate_token);
GpuRenderTransactionStatus gr_deferred_transaction_begin(
    GpuRenderTransactionId transaction_id,
    uint64_t vram_mutation_serial,
    GpuRenderDeferredCandidateToken candidate_token);

/* ---- Backend vtable -----------------------------------------------------
 * A backend supplies the same set of functions.  gpu_gl_renderer.c (Phase 2)
 * provides gl_backend_get(); until it does, requesting OpenGL falls back to
 * software.  Members mirror the gr_* / sw_* signatures 1:1. */
typedef struct GpuRenderBackend {
    const char *name;
    void (*init)(uint16_t *vram);
    void (*set_scale)(int scale);
    int  (*scale)(void);
    void (*set_texture_filter)(int bilinear);
    int  (*texture_filter)(void);
    void (*set_semi_transparency)(int enabled, int mode);
    void (*set_mask_bits)(int set_bit, int check_bit);
    void (*set_texture_window)(uint32_t raw);
    void (*set_color_modulation)(int r, int g, int b, int raw_texture);
    void (*set_dither)(int enabled);
    /* Optional (NULL = unsupported, facade no-ops): sub-pixel vertex override
     * and perspective UV weights for the next triangle. See gr_* above. */
    void (*set_precise_triangle)(int enabled,
                                 int32_t x0, int32_t y0,
                                 int32_t x1, int32_t y1,
                                 int32_t x2, int32_t y2);
    void (*set_perspective_triangle)(int enabled, float q0, float q1, float q2);
    void (*fill_rect)(int x, int y, int w, int h, uint16_t color);
    void (*copy_rect)(int src_x, int src_y, int dst_x, int dst_y, int w, int h);
    void (*draw_flat_triangle)(int x0, int y0, int x1, int y1, int x2, int y2,
                               uint16_t color);
    void (*draw_gouraud_triangle)(int x0, int y0, uint16_t c0,
                                  int x1, int y1, uint16_t c1,
                                  int x2, int y2, uint16_t c2);
    void (*draw_flat_triangle_rgb888)(int x0, int y0, int x1, int y1,
                                      int x2, int y2, uint32_t color);
    void (*draw_gouraud_triangle_rgb888)(int x0, int y0, uint32_t c0,
                                         int x1, int y1, uint32_t c1,
                                         int x2, int y2, uint32_t c2);
    void (*draw_textured_triangle)(int x0, int y0, int u0, int v0,
                                   int x1, int y1, int u1, int v1,
                                   int x2, int y2, int u2, int v2,
                                   uint16_t clut_x, uint16_t clut_y,
                                   uint16_t texpage);
    void (*draw_shaded_textured_triangle)(int x0, int y0, int u0, int v0,
                                          uint32_t color0,
                                          int x1, int y1, int u1, int v1,
                                          uint32_t color1,
                                          int x2, int y2, int u2, int v2,
                                          uint32_t color2,
                                          uint16_t clut_x, uint16_t clut_y,
                                          uint16_t texpage, int raw_texture);
    void (*draw_flat_rect)(int x, int y, int w, int h, uint16_t color);
    void (*draw_textured_rect)(int x, int y, int w, int h, int u, int v,
                               uint16_t clut_x, uint16_t clut_y,
                               uint16_t texpage);
    void (*draw_textured_rect_scaled)(int x, int y, int w, int h,
                                      int u0, int v0, int u1, int v1,
                                      uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage);
    void (*draw_line)(int x0, int y0, int x1, int y1, uint16_t color);
    void (*draw_shaded_line)(int x0, int y0, uint16_t c0,
                             int x1, int y1, uint16_t c1);
    void (*native_fill_rect)(int x, int y, int w, int h, uint16_t color);
    void (*native_copy_rect)(int src_x, int src_y, int dst_x, int dst_y,
                             int w, int h);
    GpuRenderTransactionStatus (*stream_barrier)(void);
    GpuRenderTransactionStatus (*draw_semantic_immediate)(
        const GpuRenderSemantic *semantic);
    GpuRenderTransactionStatus (*draw_semantic_temporal_candidate)(
        const GpuRenderSemantic *semantic,
        const GpuRenderTemporalCullPolicy *policy);
    GpuRenderTransactionStatus (*record_interpolation_anchors)(
        const GpuRenderInterpolationVertexAnchor *anchors, size_t count);
    int  (*render_display)(uint32_t *out, int pitch,
                           int dx, int dy, int dw, int dh);
    int  (*render_display_hires)(uint32_t *out, int pitch,
                                  int dx, int dy, int dw, int dh);
    int  (*present_vram)(int dx, int dy, int dw, int dh,
                         int linear, int force_4_3);
    int  (*present_cpu_frame)(const uint32_t *pixels, int src_w, int src_h,
                              int linear, int force_4_3, int content_w);
    int  (*present_native_cpu_frame)(const uint32_t *pixels, int src_w,
                                     int src_h, int linear, int force_4_3,
                                     int content_w);
    int  (*canonical_framebuffer_digest)(int display_x, int display_y,
                                         int display_width,
                                         int display_height,
                                         uint64_t *out_digest);
    void (*vram_write)(int x, int y, uint16_t pixel);
    uint16_t (*vram_read)(int x, int y);
    void (*vram_transfer_in)(int x, int y, int w, int h, const uint16_t *data);
    void (*vram_transfer_out)(int x, int y, int w, int h, uint16_t *data);
    void (*set_draw_area)(int x1, int y1, int x2, int y2);
    void (*get_draw_area)(int *x1, int *y1, int *x2, int *y2);
    void (*set_draw_offset)(int x, int y);
    /* Native-wide compositor (optional; NULL on backends without it — the
     * facade then reports gr_wide_supported() == 0 and the caller keeps the
     * canonical present). */
    void (*wide_configure)(int wide_w, int offset);
    void (*wide_set_target)(int base_x);
    void (*wide_disable_target)(void);
    void (*wide_clear)(int base_x, int y, int h, uint16_t color);
    void (*wide_clear_margins)(int base_x, int y, int h, uint16_t color, int sides);
    void (*native_view_clear_margins)(int base_x, int y, int h,
                                      uint16_t color);
    int  (*render_wide_display)(uint32_t *out, int pitch, int base_x,
                                int disp_y, int disp_h);
    /* Dump the ENTIRE wide compositor surface for base_x (all double-buffer
     * bands + both margins), g_wide_w x VRAM_H. Debug/inspection only; returns
     * pixel count, writes width/height to ow/oh. NULL if unsupported. */
    int  (*wide_dump_full)(uint32_t *out, int cap_pixels, int *ow, int *oh,
                           int base_x);
    /* The first five callbacks form the base transaction group. */
    GpuRenderTransactionStatus (*transaction_begin)(
        GpuRenderTransactionId transaction_id,
        uint64_t vram_mutation_serial);
    GpuRenderTransactionStatus (*ordering_barrier)(
        GpuRenderTransactionId transaction_id);
    GpuRenderTransactionStatus (*draw_semantic)(
        GpuRenderTransactionId transaction_id,
        const GpuRenderSemantic *semantic);
    GpuRenderTransactionStatus (*commit_validate)(
        GpuRenderTransactionId transaction_id,
        uint64_t current_vram_mutation_serial,
        const GpuRenderPresent *present);
    GpuRenderTransactionStatus (*rollback)(
        GpuRenderTransactionId transaction_id);
    /* Optional deferred-presentation group. All three must be present. */
    GpuRenderTransactionStatus (*deferred_candidate_capture)(
        GpuRenderTransactionId transaction_id,
        GpuRenderDeferredCandidateToken *out_candidate_token);
    GpuRenderTransactionStatus (*deferred_candidate_discard)(
        GpuRenderDeferredCandidateToken candidate_token);
    GpuRenderTransactionStatus (*deferred_transaction_begin)(
        GpuRenderTransactionId transaction_id,
        uint64_t vram_mutation_serial,
        GpuRenderDeferredCandidateToken candidate_token);
} GpuRenderBackend;

#ifdef GPU_RENDER_TRANSACTION_TESTING
/* Replaces the active backend and clears facade transaction state. Tests only. */
void gr_test_inject_backend(const GpuRenderBackend *backend);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_RENDER_H */
