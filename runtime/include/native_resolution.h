#ifndef PSX_NATIVE_RESOLUTION_H
#define PSX_NATIVE_RESOLUTION_H

#include <stdint.h>

enum { NATIVE_RESOLUTION_PRESET_COUNT = 5 };

/* Round framebuffer strides with integers: a 216-row page at 480p is
 * exactly 480 rows, even when the float ratio rounds slightly upward. */
static inline uint32_t native_resolution_page_pixels(uint32_t rows,
        uint32_t height, uint32_t logical_height) {
    return logical_height ? (uint32_t)(((uint64_t)rows * height + logical_height - 1u) / logical_height) : 0u;
}

static inline void native_resolution_preset(unsigned index, int *width, int *height) {
    static const int sizes[NATIVE_RESOLUTION_PRESET_COUNT][2] = {
        {640, 480}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}
    };
    if (index >= NATIVE_RESOLUTION_PRESET_COUNT) index = 2;
    *width = sizes[index][0];
    *height = sizes[index][1];
}

/* Map a logical edge to an actual pixel boundary. Using the same rounded
 * boundary for adjacent spans prevents gaps at fractional render scales. */
static inline uint32_t native_resolution_edge(uint32_t edge,
        uint32_t logical_extent, uint32_t physical_extent) {
    return logical_extent ? (uint32_t)((uint64_t)edge * physical_extent / logical_extent) : 0u;
}

#endif
