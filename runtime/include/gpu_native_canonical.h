#ifndef PSX_GPU_NATIVE_CANONICAL_H
#define PSX_GPU_NATIVE_CANONICAL_H

/* Canonical guest VRAM under Native work.
 *
 * Native work journals every GP0 draw, CPU->VRAM upload, fill and copy and
 * its compile thread publishes the resulting guest-resolution VRAM words, so
 * no GPU API needs a second canonical rasterizer. This module holds what is
 * API-free and shared by the OpenGL single-owner mode and the `native`
 * facade backend: semantic validation, the deferral flag, and the
 * drain-and-copy synchronisation that makes the CPU VRAM mirror current.
 *
 * All entry points run on the guest thread, except gpu_native_canonical_stats
 * (advisory plain reads for the debug server). */

#include <stdint.h>
#include "gpu_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native-view wave grid: one packet per tile. */
#define NATIVE_VIEW_WAVE_COLUMNS 20
#define NATIVE_VIEW_WAVE_ROWS 17
#define NATIVE_VIEW_WAVE_PACKET_COUNT \
    (NATIVE_VIEW_WAVE_COLUMNS * NATIVE_VIEW_WAVE_ROWS)

typedef enum GpuNativeCanonicalOp {
    GPU_NATIVE_CANONICAL_DRAW = 0,     /* semantic draw (packet route) */
    GPU_NATIVE_CANONICAL_LEGACY_DRAW,  /* per-primitive draw (legacy GP0 route) */
    GPU_NATIVE_CANONICAL_FILL,
    GPU_NATIVE_CANONICAL_COPY
} GpuNativeCanonicalOp;

typedef struct GpuNativeCanonicalStats {
    int active;                        /* `native` facade backend selected */
    int deferred;                      /* CPU mirror stale versus Native words */
    uint64_t sync_count;
    uint64_t sync_total_ns;
    uint64_t sync_max_ns;
    uint64_t deferred_draws;
    uint64_t deferred_legacy_draws;
    uint64_t deferred_fills;
    uint64_t deferred_copies;
    uint64_t transfers_in;
    uint64_t transfers_out;
    uint64_t handoffs;
    uint64_t skipped_temporal_candidates;
    uint64_t skipped_anchor_vertices;
} GpuNativeCanonicalStats;

/* Structural validation of an accepted semantic. midpoint_pass allows 16.16
 * positions between PS1 pixels (host midpoint passes only). */
GpuRenderTransactionStatus gpu_native_canonical_validate_semantic(
    const GpuRenderSemantic *semantic, int midpoint_pass);

/* The journal now owns a guest-visible mutation the CPU mirror has not seen. */
void gpu_native_canonical_defer(GpuNativeCanonicalOp op);
int  gpu_native_canonical_deferred(void);
/* Forget a pending deferral (VRAM was replaced wholesale and re-journaled). */
void gpu_native_canonical_discard(void);

/* Drain the Native work queue and copy the published words into vram, then
 * clear the deferral. Returns 1 when vram is current (including nothing to
 * do); a failed drain or missing words is psx_fatal_halt. */
int gpu_native_canonical_sync(uint16_t *vram);

/* Native work is gone: copy the last published words into vram without
 * draining. Returns 1 when a deferral was resolved, so the caller can
 * propagate vram to its own copies. */
int gpu_native_canonical_handoff(uint16_t *vram);

void gpu_native_canonical_stats(GpuNativeCanonicalStats *out);

/* Select/leave the `native` facade backend on top of the running host
 * renderer. enter() returns 0 (host canonical path kept) unless Native work
 * is the single valid owner; see gpu_native_canonical.c. */
int  gpu_native_canonical_enter(void);
void gpu_native_canonical_leave(void);

/* ---- PSX_NATIVE_CANONICAL_AUDIT ------------------------------------------
 * Observation-only comparison of the host raster's guest-read words against
 * Native's published canonical words, for the default OpenGL mode where the
 * host raster owns guest VRAM. 0 = off, 1 = every guest VRAM read through
 * the facade, 2 = also the whole of VRAM once per guest vblank. Cached
 * getenv. The host calls audit() with its (already current) words after it
 * made the mirror current for the rect; the audit drains the Native queue,
 * compares under s_native_state_mutex and records a ring entry. Guest
 * thread only; the json/dump readers are safe from the debug server. */
typedef enum GpuNativeCanonicalAuditReason {
    GPU_NATIVE_AUDIT_PREPARE_READ = 0, /* gr_vram_prepare_read (GPUREAD start) */
    GPU_NATIVE_AUDIT_VRAM_READ,        /* gr_vram_read (mask check, peek) */
    GPU_NATIVE_AUDIT_TRANSFER_OUT,     /* gr_vram_transfer_out */
    GPU_NATIVE_AUDIT_VBLANK,           /* whole VRAM once per guest vblank */
    GPU_NATIVE_AUDIT_COMMAND,          /* mode 3: bbox after each GP0 draw/fill/copy */
    GPU_NATIVE_AUDIT_REASON_COUNT
} GpuNativeCanonicalAuditReason;

int  gpu_native_canonical_audit_mode(void);
void gpu_native_canonical_audit(GpuNativeCanonicalAuditReason reason,
                                int x, int y, int w, int h,
                                const uint16_t *gl_words);
/* Mode 3 (>=3 also implies the vblank audit): audit the destination rect of
 * one just-executed GP0 draw/fill/copy so the first diverging command is
 * identified with its raw words and environment. */
void gpu_native_canonical_audit_command(const uint32_t *words, int word_count,
                                        const void *environment);
/* gpu_vblank_tick: count the guest vblank; mode 2 audits all of VRAM. */
void gpu_native_canonical_audit_vblank(void);
/* malloc'd JSON line (caller frees); n = newest entries, <=0 all kept. */
char *gpu_native_canonical_audit_json(int id, int n);
/* Write <path>.gl.bin / <path>.native.bin (raw LE u16, 1024x512) from the
 * snapshot taken at the first mismatching audit since the last dump, release
 * it, and return a malloc'd JSON line with its seq/vblank/cycle/reason/rect
 * (ok:false when none was held or a write failed). Caller frees. */
char *gpu_native_canonical_audit_dump(int id, const char *path);

/* Backend table behind GR_BACKEND_NATIVE. */
const GpuRenderBackend *native_canonical_backend_get(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_NATIVE_CANONICAL_H */
