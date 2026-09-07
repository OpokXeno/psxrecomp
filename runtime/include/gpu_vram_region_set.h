#ifndef PSXRECOMP_GPU_VRAM_REGION_SET_H
#define PSXRECOMP_GPU_VRAM_REGION_SET_H

#include <stdint.h>
#include <string.h>

#define GPU_VRAM_REGION_WIDTH 1024
#define GPU_VRAM_REGION_HEIGHT 512
#define GPU_VRAM_REGION_WORDS_PER_ROW (GPU_VRAM_REGION_WIDTH / 64)

typedef struct GpuVramRect {
    int x;
    int y;
    int w;
    int h;
} GpuVramRect;

typedef struct GpuVramRegionSet {
    uint64_t rows[GPU_VRAM_REGION_HEIGHT][GPU_VRAM_REGION_WORDS_PER_ROW];
    uint32_t nonzero_words;
} GpuVramRegionSet;

static inline uint64_t gpu_vram_region_word_mask(int first_bit, int last_bit) {
    const uint64_t low = UINT64_MAX << first_bit;
    const uint64_t high = last_bit == 63
        ? UINT64_MAX
        : (UINT64_C(1) << (last_bit + 1)) - UINT64_C(1);
    return low & high;
}

static inline void gpu_vram_region_clear_all(GpuVramRegionSet *set) {
    memset(set, 0, sizeof(*set));
}

static inline int gpu_vram_region_any(const GpuVramRegionSet *set) {
    return set->nonzero_words != 0;
}

static inline void gpu_vram_region_clip_rect(int *x0, int *y0,
                                              int *x1, int *y1) {
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 >= GPU_VRAM_REGION_WIDTH) *x1 = GPU_VRAM_REGION_WIDTH - 1;
    if (*y1 >= GPU_VRAM_REGION_HEIGHT) *y1 = GPU_VRAM_REGION_HEIGHT - 1;
}

static inline void gpu_vram_region_mark_rect(GpuVramRegionSet *set,
                                              int x0, int y0,
                                              int x1, int y1) {
    gpu_vram_region_clip_rect(&x0, &y0, &x1, &y1);
    if (x0 > x1 || y0 > y1) return;

    const int first_word = x0 >> 6;
    const int last_word = x1 >> 6;
    for (int y = y0; y <= y1; ++y) {
        for (int word = first_word; word <= last_word; ++word) {
            const int first_bit = word == first_word ? x0 & 63 : 0;
            const int last_bit = word == last_word ? x1 & 63 : 63;
            const uint64_t old_value = set->rows[y][word];
            set->rows[y][word] |= gpu_vram_region_word_mask(first_bit, last_bit);
            if (!old_value && set->rows[y][word]) ++set->nonzero_words;
        }
    }
}

static inline void gpu_vram_region_clear_rect(GpuVramRegionSet *set,
                                               int x0, int y0,
                                               int x1, int y1) {
    gpu_vram_region_clip_rect(&x0, &y0, &x1, &y1);
    if (x0 > x1 || y0 > y1) return;

    const int first_word = x0 >> 6;
    const int last_word = x1 >> 6;
    for (int y = y0; y <= y1; ++y) {
        for (int word = first_word; word <= last_word; ++word) {
            const int first_bit = word == first_word ? x0 & 63 : 0;
            const int last_bit = word == last_word ? x1 & 63 : 63;
            const uint64_t old_value = set->rows[y][word];
            set->rows[y][word] &=
                ~gpu_vram_region_word_mask(first_bit, last_bit);
            if (old_value && !set->rows[y][word]) --set->nonzero_words;
        }
    }
}

static inline int gpu_vram_region_intersects(const GpuVramRegionSet *set,
                                              int x0, int y0,
                                              int x1, int y1) {
    gpu_vram_region_clip_rect(&x0, &y0, &x1, &y1);
    if (!gpu_vram_region_any(set) || x0 > x1 || y0 > y1) return 0;

    const int first_word = x0 >> 6;
    const int last_word = x1 >> 6;
    for (int y = y0; y <= y1; ++y) {
        for (int word = first_word; word <= last_word; ++word) {
            const int first_bit = word == first_word ? x0 & 63 : 0;
            const int last_bit = word == last_word ? x1 & 63 : 63;
            if (set->rows[y][word] &
                gpu_vram_region_word_mask(first_bit, last_bit))
                return 1;
        }
    }
    return 0;
}

static inline int gpu_vram_region_bounds(const GpuVramRegionSet *set,
                                          GpuVramRect *bounds) {
    int x0 = GPU_VRAM_REGION_WIDTH;
    int y0 = GPU_VRAM_REGION_HEIGHT;
    int x1 = -1;
    int y1 = -1;
    if (!gpu_vram_region_any(set)) return 0;

    for (int y = 0; y < GPU_VRAM_REGION_HEIGHT; ++y) {
        for (int word = 0; word < GPU_VRAM_REGION_WORDS_PER_ROW; ++word) {
            uint64_t value = set->rows[y][word];
            int first;
            int last;
            if (!value) continue;
#if defined(_MSC_VER)
            first = 0;
            while (!(value & (UINT64_C(1) << first))) ++first;
            last = 63;
            while (!(value & (UINT64_C(1) << last))) --last;
#else
            first = __builtin_ctzll(value);
            last = 63 - __builtin_clzll(value);
#endif
            if (word * 64 + first < x0) x0 = word * 64 + first;
            if (word * 64 + last > x1) x1 = word * 64 + last;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    bounds->x = x0;
    bounds->y = y0;
    bounds->w = x1 - x0 + 1;
    bounds->h = y1 - y0 + 1;
    return 1;
}

static inline int gpu_vram_split_transfer(int x, int y, int w, int h,
                                           GpuVramRect rects[4]) {
    int count = 0;
    int first_width;
    int first_height;
    if (w <= 0 || h <= 0) return 0;
    if (w > GPU_VRAM_REGION_WIDTH) w = GPU_VRAM_REGION_WIDTH;
    if (h > GPU_VRAM_REGION_HEIGHT) h = GPU_VRAM_REGION_HEIGHT;
    x &= GPU_VRAM_REGION_WIDTH - 1;
    y &= GPU_VRAM_REGION_HEIGHT - 1;
    first_width = w < GPU_VRAM_REGION_WIDTH - x
        ? w : GPU_VRAM_REGION_WIDTH - x;
    first_height = h < GPU_VRAM_REGION_HEIGHT - y
        ? h : GPU_VRAM_REGION_HEIGHT - y;
    rects[count++] = (GpuVramRect){x, y, first_width, first_height};
    if (first_width < w)
        rects[count++] = (GpuVramRect){0, y, w - first_width, first_height};
    if (first_height < h)
        rects[count++] = (GpuVramRect){x, 0, first_width, h - first_height};
    if (first_width < w && first_height < h)
        rects[count++] = (GpuVramRect){0, 0, w - first_width,
                                      h - first_height};
    return count;
}

static inline void gpu_vram_region_mark_transfer(GpuVramRegionSet *set,
                                                  int x, int y,
                                                  int w, int h) {
    GpuVramRect rects[4];
    const int count = gpu_vram_split_transfer(x, y, w, h, rects);
    for (int i = 0; i < count; ++i)
        gpu_vram_region_mark_rect(set, rects[i].x, rects[i].y,
                                 rects[i].x + rects[i].w - 1,
                                 rects[i].y + rects[i].h - 1);
}

static inline void gpu_vram_region_clear_transfer(GpuVramRegionSet *set,
                                                   int x, int y,
                                                   int w, int h) {
    GpuVramRect rects[4];
    const int count = gpu_vram_split_transfer(x, y, w, h, rects);
    for (int i = 0; i < count; ++i)
        gpu_vram_region_clear_rect(set, rects[i].x, rects[i].y,
                                  rects[i].x + rects[i].w - 1,
                                  rects[i].y + rects[i].h - 1);
}

#endif
