#ifndef PSX_HD_TEXTURE_CACHE_H
#define PSX_HD_TEXTURE_CACHE_H

/* hd_texture_cache.c — the residency policy of GPU copies of HD texture pack
 * images, shared by every renderer (OpenGL legacy and Native, Vulkan Native).
 *
 * One cache per GPU context, each touched only by its owning thread. Images
 * arrive decoded from the pack's worker threads; a draw whose image is not
 * resident yet samples guest VRAM this frame and picks the image up once it
 * lands. The policy owns lookup, the pack's alpha convention (alpha >= 64 is
 * opaque, colour premultiplied by coverage), the LRU eviction under a byte
 * budget and invalidation when the pack changes. The backend only creates and
 * destroys the texture behind an opaque non-zero handle. */

#include <stddef.h>
#include <stdint.h>

#include "gpu_render.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HD_CACHE_BUDGET ((uint64_t)1536u * 1024u * 1024u)

typedef struct HdCacheEntry {
    uint64_t key;
    uint64_t handle;
    uint64_t bytes;
    uint64_t last_use;
    uint8_t failed;
} HdCacheEntry;

typedef struct HdTextureCache {
    HdCacheEntry *entries;
    size_t count, capacity;
    uint64_t bytes, tick;
    uint64_t resident, failed; /* scalar mirrors for cross-thread diagnostics */
    uint32_t generation;
    uint8_t *scratch;
    size_t scratch_capacity;
} HdTextureCache;

typedef struct HdCacheOps {
    /* Creates the texture (with its mip chain) from premultiplied RGBA8.
     * Returns a non-zero handle, or 0 on failure. */
    uint64_t (*upload)(void *context, uint32_t width, uint32_t height, const uint8_t *rgba);
    void (*destroy)(void *context, uint64_t handle);
    void *context;
} HdCacheOps;

void hd_cache_clear(HdTextureCache *cache, const HdCacheOps *ops);
/* Handle of the draw's replacement, uploading it on first availability. 0
 * while the image is decoding, missing or unusable. */
uint64_t hd_cache_lookup(HdTextureCache *cache, const GpuRenderHdTexture *hd, const HdCacheOps *ops);

#ifdef __cplusplus
}
#endif

#endif
