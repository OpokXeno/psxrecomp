#ifndef PSX_NATIVE_RASTER_LAYOUT_H
#define PSX_NATIVE_RASTER_LAYOUT_H

#include "xg_render_scene_snapshot.h"

typedef struct GlNativeRasterLayout {
    uint32_t width, height;
    float scale_x, scale_y;
    uint32_t page_rows;
    uint32_t page_pixels;
} GlNativeRasterLayout;

/* VIEW storage spans the guest VRAM rows shared by both display buffers.
 * Its mapping must remain fixed when display_y flips between those buffers;
 * only scanout selects a different rectangle of the same stored pixels. */
static inline int native_raster_layout_build(const XgSemanticDisplayState *display,
        uint32_t scale, GlNativeRasterLayout *out) {
    *out = (GlNativeRasterLayout){display->render_width, display->render_height, 1.f, 1.f};
    if (!out->width || !out->height || display->depth24) return 1;
    const int expanded = display->native_width && display->native_height &&
        display->native_width > 2u * display->native_offset_x &&
        display->width == display->native_width - 2u * display->native_offset_x &&
        (uint32_t)display->aspect_num * display->native_height >=
            (uint32_t)display->aspect_den * display->width;
    const uint32_t width = expanded ? display->native_width : display->width;
    if (!width || !display->height || !scale) return 0;
    out->scale_x = (float)out->width / (width * scale);
    out->scale_y = (float)out->height / (display->height * scale);
    return 1;
}

static inline int native_raster_layout_equal(const GlNativeRasterLayout *a,
        const GlNativeRasterLayout *b) {
    return a->width == b->width && a->height == b->height &&
        a->scale_x == b->scale_x && a->scale_y == b->scale_y &&
        a->page_rows == b->page_rows && a->page_pixels == b->page_pixels;
}

#endif
