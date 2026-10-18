#include "hd_texture_cache.h"

#include <stdlib.h>
#include <string.h>

#include "hd_texture_runtime.h"

void hd_cache_clear(HdTextureCache *cache, const HdCacheOps *ops) {
    for (size_t i = 0; i < cache->count; ++i)
        if (cache->entries[i].handle) ops->destroy(ops->context, cache->entries[i].handle);
    cache->count = 0;
    cache->bytes = 0;
    cache->resident = cache->failed = 0;
}

static void hd_cache_evict(HdTextureCache *cache, uint64_t incoming, const HdCacheOps *ops) {
    while (cache->bytes + incoming > HD_CACHE_BUDGET) {
        size_t victim = SIZE_MAX;
        for (size_t i = 0; i < cache->count; ++i)
            if (cache->entries[i].handle &&
                (victim == SIZE_MAX ||
                 cache->entries[i].last_use < cache->entries[victim].last_use))
                victim = i;
        if (victim == SIZE_MAX) return;
        ops->destroy(ops->context, cache->entries[victim].handle);
        cache->bytes -= cache->entries[victim].bytes;
        --cache->resident;
        /* Order-preserving removal: the caller's newest entry stays last. */
        memmove(&cache->entries[victim], &cache->entries[victim + 1u],
                (cache->count - victim - 1u) * sizeof(cache->entries[0]));
        --cache->count;
    }
}

uint64_t hd_cache_lookup(HdTextureCache *cache, const GpuRenderHdTexture *hd, const HdCacheOps *ops) {
    HdTexturePack *pack = hd_texture_runtime_pack();
    if (!hd->valid || !pack) return 0;
    const uint32_t generation = hd_texture_runtime_generation();
    if (cache->generation != generation) {
        hd_cache_clear(cache, ops);
        cache->generation = generation;
    }
    const uint64_t key = ((uint64_t)hd->texture_hash << 32) | hd->palette_hash;
    for (size_t i = 0; i < cache->count; ++i) {
        HdCacheEntry *entry = &cache->entries[i];
        if (entry->key != key) continue;
        if (entry->failed) return 0;
        entry->last_use = ++cache->tick;
        return entry->handle;
    }
    HdTexturePixels pixels;
    const int state = hd_texture_pack_acquire_decoded(pack, hd->texture_hash,
                                                     hd->palette_hash, &pixels);
    if (state == HD_TEXTURE_LOOKUP_NONE) {
        (void)hd_texture_pack_request_decode(pack, hd->texture_hash,
                                             hd->palette_hash);
        return 0;
    }
    if (cache->count == cache->capacity) {
        const size_t capacity = cache->capacity ? cache->capacity * 2u : 64u;
        HdCacheEntry *entries = (HdCacheEntry *)realloc(cache->entries,
                                                        capacity * sizeof(*entries));
        if (!entries) {
            if (state == HD_TEXTURE_LOOKUP_FOUND) hd_texture_pixels_release(&pixels);
            return 0;
        }
        cache->entries = entries;
        cache->capacity = capacity;
    }
    HdCacheEntry *entry = &cache->entries[cache->count++];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->last_use = ++cache->tick;
    if (state != HD_TEXTURE_LOOKUP_FOUND) {
        entry->failed = 1;
        ++cache->failed;
        return 0;
    }
    const size_t bytes = (size_t)pixels.width * pixels.height * 4u;
    if (cache->scratch_capacity < bytes) {
        uint8_t *scratch = (uint8_t *)realloc(cache->scratch, bytes);
        if (!scratch) {
            hd_texture_pixels_release(&pixels);
            --cache->count;
            return 0;
        }
        cache->scratch = scratch;
        cache->scratch_capacity = bytes;
    }
    /* Pack alpha convention (0 = hole, 127 = STP set, 255 = opaque) to coverage:
     * alpha >= 64 is opaque; colour is premultiplied by coverage so linear/mip
     * filtering never bleeds a hole's colour into an edge. The shader divides
     * it back out. STP stays the guest texel's own bit. */
    for (uint32_t y = 0; y < pixels.height; ++y) {
        const uint8_t *in = pixels.rgba + (size_t)y * pixels.stride;
        uint8_t *out = cache->scratch + (size_t)y * pixels.width * 4u;
        for (uint32_t x = 0; x < pixels.width; ++x, in += 4, out += 4) {
            const int opaque = in[3] >= 64;
            out[0] = opaque ? in[0] : 0;
            out[1] = opaque ? in[1] : 0;
            out[2] = opaque ? in[2] : 0;
            out[3] = opaque ? 255 : 0;
        }
    }
    const uint32_t width = pixels.width, height = pixels.height;
    hd_texture_pixels_release(&pixels);
    hd_texture_pack_forget_decoded(pack, hd->texture_hash, hd->palette_hash);
    const uint64_t resident = (uint64_t)bytes * 4u / 3u; /* with mip chain */
    hd_cache_evict(cache, resident, ops);
    /* Eviction may have moved entries; this one is still the last. */
    entry = &cache->entries[cache->count - 1u];
    entry->handle = ops->upload(ops->context, width, height, cache->scratch);
    if (!entry->handle) {
        entry->failed = 1;
        ++cache->failed;
        return 0;
    }
    entry->bytes = resident;
    cache->bytes += resident;
    ++cache->resident;
    return entry->handle;
}
