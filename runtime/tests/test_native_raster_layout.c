#include "native_raster_layout.h"
#include "native_resolution.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

int main(void) {
    XgSemanticDisplayState display = {
        .width = 320, .height = 216, .aspect_num = 16, .aspect_den = 9,
        .native_width = 426, .native_height = 240, .native_offset_x = 53,
        .render_scale = 1, .temporal_hz = 240,
    };
    const uint16_t origins[] = {0, 224, 0, 224, 256, 0};
    for (unsigned preset = 0; preset < NATIVE_RESOLUTION_PRESET_COUNT; ++preset) {
        int width, height;
        native_resolution_preset(preset, &width, &height);
        display.render_width = (uint16_t)width;
        display.render_height = (uint16_t)height;
        display.display_y = 0;
        GlNativeRasterLayout stored;
        assert(native_raster_layout_build(&display, 1, &stored));
        assert(stored.width == (uint32_t)width && stored.height == (uint32_t)height);
        assert(native_resolution_page_pixels(216, (uint32_t)height, 216) == (uint32_t)height);
        assert(native_resolution_page_pixels(224, (uint32_t)height, 216) >= (uint32_t)height);
        /* 720p/1440p have fractional row scales. Alternating the game's two
         * buffers must preserve the resident VIEW pixels and HD materials. */
        for (unsigned i = 0; i < sizeof(origins) / sizeof(origins[0]); ++i) {
            display.display_y = origins[i];
            GlNativeRasterLayout next;
            assert(native_raster_layout_build(&display, 1, &next));
            assert(native_raster_layout_equal(&stored, &next));
        }
        ++display.render_width;
        GlNativeRasterLayout resized;
        assert(native_raster_layout_build(&display, 1, &resized));
        assert(!native_raster_layout_equal(&stored, &resized));
        --display.render_width;
        ++display.height;
        assert(native_raster_layout_build(&display, 1, &resized));
        assert(!native_raster_layout_equal(&stored, &resized));
        --display.height;
        printf("PASS %dx%d: display-buffer flips preserve the raster layout\n", width, height);
    }
    return 0;
}
