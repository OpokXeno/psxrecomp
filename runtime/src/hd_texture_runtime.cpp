#include "hd_texture_runtime.h"

#include "gpu_uv.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

/* Decoded RGBA waits here only until the renderer turns it into a GPU
 * texture and forgets it, so this bounds in-flight decodes, not the pack. */
constexpr size_t kDecodeBudget = size_t{768} * 1024 * 1024;

HdTexturePack* g_pack = nullptr;
int g_enabled = 1;
uint64_t g_uploads_seen = 0, g_resolves = 0, g_matches = 0;
uint64_t g_recent[16];
uint32_t g_recent_count = 0;
const uint16_t* g_vram = nullptr;
uint32_t g_generation = 0;
std::vector<uint8_t> g_state_blob;

/* Sampled range of one window axis: the window replaces the masked bits of
 * every coordinate with the offset's, which can scatter a contiguous range. */
void window_axis(int lo, int hi, unsigned mask, unsigned offset,
                 int* out_lo, int* out_hi) {
    if (mask == 0) {
        *out_lo = lo;
        *out_hi = hi;
        return;
    }
    const int keep = ~(int)(mask * 8u);
    const int set = (int)((offset & mask) * 8u);
    int first = 255, last = 0;
    for (int value = lo; value <= hi; ++value) {
        const int sampled = ((value & keep) | set) & 255;
        if (sampled < first) first = sampled;
        if (sampled > last) last = sampled;
    }
    *out_lo = first;
    *out_hi = last;
}

} // namespace

extern "C" {

void hd_texture_runtime_set_vram(const uint16_t* vram) {
    g_vram = vram;
}

int hd_texture_runtime_resolve(uint16_t page_x, uint16_t page_y, uint8_t depth,
                               uint16_t clut_x, uint16_t clut_y,
                               const int lim[4],
                               uint8_t window_mask_x, uint8_t window_mask_y,
                               uint8_t window_offset_x, uint8_t window_offset_y,
                               GpuRenderHdTexture* out) {
    *out = GpuRenderHdTexture{};
    if (!g_pack || !g_vram || !g_enabled || depth > HD_TEXTURE_DEPTH_16BPP)
        return 0;
    ++g_resolves;
    int lo_u, hi_u, lo_v, hi_v;
    window_axis(lim[0], lim[2], window_mask_x, window_offset_x, &lo_u, &hi_u);
    window_axis(lim[1], lim[3], window_mask_y, window_offset_y, &lo_v, &hi_v);
    HdTextureDrawQuery query{};
    query.page_x = page_x;
    query.page_y = page_y;
    query.depth = depth;
    query.u_first = (uint8_t)lo_u;
    query.u_last = (uint8_t)hi_u;
    query.v_first = (uint8_t)lo_v;
    query.v_last = (uint8_t)hi_v;
    query.clut_x = clut_x;
    query.clut_y = clut_y;
    query.vram = g_vram;
    query.vram_word_count = size_t{1024} * 512;
    HdTextureMatch match;
    if (hd_texture_pack_match(g_pack, &query, &match) != HD_TEXTURE_LOOKUP_FOUND)
        return 0;
    const int texels_per_word = depth == HD_TEXTURE_DEPTH_4BPP ? 4 :
                                depth == HD_TEXTURE_DEPTH_8BPP ? 2 : 1;
    out->texture_hash = match.entry.texture_hash;
    out->palette_hash = match.entry.palette_hash;
    out->texel_offset_u =
        texels_per_word * ((int)match.source_word_x - lo_u / texels_per_word);
    out->texel_offset_v = (int)match.source_y - lo_v;
    out->texel_width = (uint16_t)(match.upload_width_words * texels_per_word);
    out->texel_height = match.upload_height;
    out->lim[0] = (uint8_t)lo_u;
    out->lim[1] = (uint8_t)lo_v;
    out->lim[2] = (uint8_t)hi_u;
    out->lim[3] = (uint8_t)hi_v;
    out->valid = 1;
    ++g_matches;
    const uint64_t key = (uint64_t{out->texture_hash} << 32) | out->palette_hash;
    bool seen = false;
    for (uint32_t i = 0; i < g_recent_count; ++i) seen |= g_recent[i] == key;
    if (!seen) {
        if (g_recent_count < 16) ++g_recent_count;
        for (uint32_t i = g_recent_count - 1; i > 0; --i) g_recent[i] = g_recent[i - 1];
        g_recent[0] = key;
    }
    (void)hd_texture_pack_request_decode(g_pack, out->texture_hash,
                                         out->palette_hash);
    return 1;
}

int hd_texture_runtime_resolve_semantic(const GpuRenderSemantic* semantic,
                                        GpuRenderHdTexture* out) {
    *out = GpuRenderHdTexture{};
    if (!g_pack || !semantic || !semantic->material.textured ||
        semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->triangle_count == 0)
        return 0;
    int lim[4] = {255, 255, 0, 0};
    for (unsigned t = 0; t < semantic->triangle_count; ++t) {
        float xs[3], ys[3];
        int us[3], vs[3], tri[4];
        for (unsigned v = 0; v < 3; ++v) {
            const GpuRenderSemanticVertex& vertex =
                semantic->triangles[t].vertices[v];
            xs[v] = (float)vertex.x / 65536.0f;
            ys[v] = (float)vertex.y / 65536.0f;
            us[v] = vertex.u >> 16;
            vs[v] = vertex.v >> 16;
        }
        psx_uv_tri_limits_f32(xs, ys, us, vs, tri);
        for (int axis = 0; axis < 2; ++axis) {
            if (tri[axis] < lim[axis]) lim[axis] = tri[axis];
            if (tri[axis + 2] > lim[axis + 2]) lim[axis + 2] = tri[axis + 2];
        }
    }
    const GpuRenderMaterial& material = semantic->material;
    return hd_texture_runtime_resolve(
        (uint16_t)(material.texture_page_x * 64u),
        (uint16_t)(material.texture_page_y * 256u),
        (uint8_t)material.texture_depth, material.clut_x, material.clut_y, lim,
        material.texture_window_mask_x, material.texture_window_mask_y,
        material.texture_window_offset_x, material.texture_window_offset_y,
        out);
}

int hd_texture_runtime_configure(const char* const* roots, size_t count,
                                 char* error, size_t error_capacity) {
    if (error && error_capacity) error[0] = '\0';
    hd_texture_pack_destroy(g_pack);
    g_pack = nullptr;
    ++g_generation;
    if (count == 0) return 1;
    HdTexturePack* pack = nullptr;
    if (!hd_texture_pack_create_multi(roots, count, &pack, error,
                                      error_capacity))
        return 0;
    hd_texture_pack_set_decode_budget(pack, kDecodeBudget);
    g_pack = pack;
    return 1;
}

int hd_texture_runtime_active(void) {
    return g_pack != nullptr;
}

HdTexturePack* hd_texture_runtime_pack(void) {
    return g_pack;
}

uint32_t hd_texture_runtime_generation(void) {
    return g_generation;
}

void hd_texture_runtime_on_upload(int x, int y, int w, int h,
                                  const uint16_t* pixels) {
    if (!g_pack || w <= 0 || h <= 0 || !pixels) return;
    if (w >= 1024 && h >= 512) {
        /* Whole-VRAM rewrite: a savestate or reset, never a game texture. */
        hd_texture_pack_reset_tracking(g_pack);
        return;
    }
    ++g_uploads_seen;
    uint32_t hash = 0;
    if (hd_texture_pack_track_upload(g_pack, (uint16_t)x, (uint16_t)y,
                                     (uint16_t)w, (uint16_t)h, pixels,
                                     (size_t)w * (size_t)h, &hash))
        hd_texture_pack_prefetch(g_pack, hash);
}

void hd_texture_runtime_on_copy(int src_x, int src_y, int dst_x, int dst_y,
                                int w, int h) {
    if (!g_pack || w <= 0 || h <= 0) return;
    hd_texture_pack_track_copy(g_pack, (uint16_t)src_x, (uint16_t)src_y,
                               (uint16_t)dst_x, (uint16_t)dst_y,
                               (uint16_t)w, (uint16_t)h);
}

void hd_texture_runtime_on_fill(int x, int y, int w, int h) {
    if (!g_pack || w <= 0 || h <= 0) return;
    hd_texture_pack_invalidate(g_pack, (uint16_t)x, (uint16_t)y,
                               (uint16_t)w, (uint16_t)h);
}

void hd_texture_runtime_stats(HdTextureRuntimeStats* out) {
    *out = HdTextureRuntimeStats{};
    out->active = g_pack != nullptr;
    out->enabled = g_enabled;
    if (g_pack) {
        HdTexturePackInfo info;
        hd_texture_pack_get_info(g_pack, &info);
        out->pack_images = info.replacement_file_count;
        out->tracked_uploads = hd_texture_pack_tracking_upload_count(g_pack);
    }
    out->uploads_seen = g_uploads_seen;
    out->resolves = g_resolves;
    out->matches = g_matches;
    out->recent_count = g_recent_count;
    for (uint32_t i = 0; i < g_recent_count; ++i) out->recent[i] = g_recent[i];
}

void hd_texture_runtime_set_enabled(int enabled) {
    g_enabled = enabled != 0;
}

uint32_t hd_texture_runtime_state_bytes(void) {
    uint8_t* data = nullptr;
    size_t size = 0;
    g_state_blob.clear();
    if (!hd_texture_pack_tracking_state_save(g_pack, &data, &size)) {
        /* The wire has hard caps; an over-full tracker is saved empty rather
         * than failing the whole savestate. */
        if (!hd_texture_pack_tracking_state_save(nullptr, &data, &size))
            return 0;
    }
    g_state_blob.assign(data, data + size);
    std::free(data);
    return (uint32_t)g_state_blob.size();
}

void hd_texture_runtime_state_write(uint8_t* out) {
    if (out && !g_state_blob.empty())
        std::memcpy(out, g_state_blob.data(), g_state_blob.size());
}

void hd_texture_runtime_state_reset(void) {
    hd_texture_pack_reset_tracking(g_pack);
}

int hd_texture_runtime_state_read(const uint8_t* data, uint32_t size) {
    if (!hd_texture_pack_tracking_state_check(data, size)) return 0;
    return hd_texture_pack_tracking_state_load(g_pack, data, size);
}

} // extern "C"
