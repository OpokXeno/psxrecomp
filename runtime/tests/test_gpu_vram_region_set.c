#include "gpu_vram_region_set.h"

#include <stdio.h>
#include <stdlib.h>

static void require(int condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "gpu_vram_region_set_test: %s\n", message);
    exit(1);
}

int main(void) {
    static GpuVramRegionSet set;
    GpuVramRect bounds;
    GpuVramRect rects[4];

    require(!gpu_vram_region_any(&set), "zero state must be clean");
    gpu_vram_region_mark_rect(&set, 63, 7, 65, 8);
    require(gpu_vram_region_intersects(&set, 63, 7, 63, 7),
            "first word edge was not marked");
    require(gpu_vram_region_intersects(&set, 65, 8, 65, 8),
            "second word edge was not marked");
    require(!gpu_vram_region_intersects(&set, 62, 7, 62, 8),
            "adjacent pixel was marked");
    gpu_vram_region_clear_rect(&set, 64, 7, 64, 8);
    require(!gpu_vram_region_intersects(&set, 64, 7, 64, 8),
            "regional clear retained the cleared column");
    require(gpu_vram_region_intersects(&set, 63, 7, 65, 8),
            "regional clear removed neighbouring pixels");

    gpu_vram_region_clear_all(&set);
    require(gpu_vram_split_transfer(1022, 510, 4, 4, rects) == 4,
            "two-axis wrap must split into four rectangles");
    require(rects[0].x == 1022 && rects[0].y == 510 &&
            rects[0].w == 2 && rects[0].h == 2,
            "primary wrapped rectangle is wrong");
    require(rects[3].x == 0 && rects[3].y == 0 &&
            rects[3].w == 2 && rects[3].h == 2,
            "corner wrapped rectangle is wrong");
    gpu_vram_region_mark_transfer(&set, 1022, 510, 4, 4);
    require(gpu_vram_region_intersects(&set, 0, 0, 1, 1),
            "wrapped corner was not marked");
    require(!gpu_vram_region_intersects(&set, 2, 0, 1021, 509),
            "wrapped transfer marked pixels outside its four rectangles");
    require(gpu_vram_region_bounds(&set, &bounds),
            "dirty bounds were not reported");
    require(bounds.x == 0 && bounds.y == 0 &&
            bounds.w == 1024 && bounds.h == 512,
            "wrapped edge bounds must span VRAM without filling the interior");

    gpu_vram_region_clear_transfer(&set, 1022, 510, 4, 4);
    require(!gpu_vram_region_any(&set),
            "wrapped regional clear did not restore clean state");

    gpu_vram_region_mark_rect(&set, -10, -10, 3, 2);
    require(gpu_vram_region_intersects(&set, 0, 0, 3, 2),
            "clipped mark lost in-range pixels");
    require(gpu_vram_region_bounds(&set, &bounds) &&
            bounds.x == 0 && bounds.y == 0 && bounds.w == 4 && bounds.h == 3,
            "clipped bounds are wrong");

    gpu_vram_region_clear_all(&set);
    require(gpu_vram_split_transfer(0, 0, 4096, 4096, rects) == 1 &&
            rects[0].w == 1024 && rects[0].h == 512,
            "oversized transfer must clamp to one full-VRAM rectangle");

    return 0;
}
