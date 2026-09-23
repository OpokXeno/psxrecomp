#ifndef PSXRECOMP_HD_TEXTURE_RUNTIME_H
#define PSXRECOMP_HD_TEXTURE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "gpu_render.h"
#include "hd_texture_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Process-wide HD texture replacement state: the active pack set and its VRAM
 * upload tracker. The launcher's texture-pack list selects the roots; the
 * gr_* facade feeds VRAM mutations; a hardware renderer asks for matches.
 * Everything here runs on the thread that drives the renderer. */

/* Replaces the active pack set (roots[0] has priority). count == 0 disables
 * replacement. Returns 0 and leaves replacement disabled on failure. */
int hd_texture_runtime_configure(const char* const* roots, size_t count,
                                 char* error, size_t error_capacity);
int hd_texture_runtime_active(void);
HdTexturePack* hd_texture_runtime_pack(void);
/* Changes whenever the pack set is replaced; renderers drop GPU copies of
 * replacement images from an older generation. */
uint32_t hd_texture_runtime_generation(void);

/* Live guest VRAM (1024x512 words) used for CLUT hashing at resolve time. */
void hd_texture_runtime_set_vram(const uint16_t* vram);

/* Decides whether a textured draw is replaced. lim is the draw's inclusive
 * sampled page-texel range {lo_u, lo_v, hi_u, hi_v}; window_* are the raw
 * GP0(E2h) fields. Must run in guest order with the mutation feed below.
 * Starts decoding the image when it matches. Returns out->valid. */
int hd_texture_runtime_resolve(uint16_t page_x, uint16_t page_y, uint8_t depth,
                               uint16_t clut_x, uint16_t clut_y,
                               const int lim[4],
                               uint8_t window_mask_x, uint8_t window_mask_y,
                               uint8_t window_offset_x, uint8_t window_offset_y,
                               GpuRenderHdTexture* out);
/* Same decision for a decoded GP0 semantic (page coordinates from its
 * material, sampled range from its triangles). */
int hd_texture_runtime_resolve_semantic(const GpuRenderSemantic* semantic,
                                        GpuRenderHdTexture* out);

/* VRAM mutation feed (native VRAM word coordinates). */
void hd_texture_runtime_on_upload(int x, int y, int w, int h,
                                  const uint16_t* pixels);
void hd_texture_runtime_on_copy(int src_x, int src_y, int dst_x, int dst_y,
                                int w, int h);
void hd_texture_runtime_on_fill(int x, int y, int w, int h);

/* Debug-server surface: live counters and an A/B switch. Disabling keeps
 * tracking (so re-enabling is immediate) but resolves nothing. */
typedef struct HdTextureRuntimeStats {
    int active;                 /* a pack set is configured */
    int enabled;                /* A/B switch */
    size_t pack_images;
    size_t tracked_uploads;     /* resident uploads the pack has images for */
    uint64_t uploads_seen;
    uint64_t resolves;          /* textured draws examined */
    uint64_t matches;           /* draws given a replacement */
    uint32_t recent_count;      /* valid entries of recent[] */
    uint64_t recent[16];        /* last distinct matched keys, texhash<<32|pal */
} HdTextureRuntimeStats;
void hd_texture_runtime_stats(HdTextureRuntimeStats* out);
void hd_texture_runtime_set_enabled(int enabled);

/* Savestate section payload. The tracker is host metadata, but without it a
 * restored VRAM would never match again until the game re-uploaded. */
uint32_t hd_texture_runtime_state_bytes(void);
void     hd_texture_runtime_state_write(uint8_t* out);
int      hd_texture_runtime_state_read(const uint8_t* data, uint32_t size);
void     hd_texture_runtime_state_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_HD_TEXTURE_RUNTIME_H */
