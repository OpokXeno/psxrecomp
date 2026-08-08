#ifndef PSX_GPU_GL_RENDERER_H
#define PSX_GPU_GL_RENDERER_H

/* GL backend context + present entry points, called from main.cpp's window
 * setup and present path when [video] renderer = "opengl".  The backend's
 * rasterization vtable is obtained separately via gl_backend_get()
 * (gpu_render.h).  SDL_Window is forward-declared (SDL typedefs it from this
 * same struct tag) so this header needs no SDL include. */

#include <stdint.h>

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

/* Presentation-only frame interpolation. High-refresh sub-presents blend the
 * two most recent stable display images; guest simulation timing is unchanged. */
void gl_renderer_set_interpolation(int enabled, double host_hz, double target_hz);
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
 * at the letterbox centre right before the swap (the ground truth for "did this
 * swap present black"). */
enum {
    GL_PRES_VRAM  = 0,   /* 15-bit FBO blit present (gl_renderer_present_vram) */
    GL_PRES_WIDE  = 1,   /* native-wide FBO blit present                       */
    GL_PRES_CPU   = 2,   /* CPU-readout quad present (24-bit FMV / forced)     */
    GL_PRES_BLANK = 3,   /* display-disabled black present                     */
    GL_PRES_INTERP = 4,  /* host-refresh interpolation sub-present              */
};

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
} GlPresEvent;

uint64_t gl_renderer_pres_total(void);
int gl_renderer_pres_get(uint64_t seq, GlPresEvent *out);

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
