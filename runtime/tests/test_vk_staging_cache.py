#!/usr/bin/env python3
from pathlib import Path
import re

source = (Path(__file__).parents[1] / "src" / "gpu_vk_renderer.c").read_text()
make = re.search(
    r"static int make_staging\([^;]*?\) \{.*?\n\}", source, re.DOTALL
)
release = re.search(r"static void staging_release\(.*?\n\}", source, re.DOTALL)
destroy = re.search(r"static void staging_destroy\(.*?\n\}", source, re.DOTALL)
assert make and release and destroy
assert "entry->size >= bytes" in make.group(0)
assert "entry->busy = 1" in make.group(0)
assert "p_vkMapMemory" in make.group(0)
assert "busy = 0" in release.group(0)
assert "p_vkUnmapMemory" in destroy.group(0)
assert source.count("p_vkUnmapMemory(s_dev, rmem)") == 0
assert source.count("p_vkUnmapMemory(s_dev, umem)") == 0

planner = re.search(
    r"static int vk_vram_copy_segments\(.*?^\}", source,
    flags=re.DOTALL | re.MULTILINE,
)
move = re.search(
    r"static void vkb_copy_rect\(.*?^\}", source,
    flags=re.DOTALL | re.MULTILINE,
)
blit = re.search(
    r"static void blit_region\(.*?^\}", source,
    flags=re.DOTALL | re.MULTILINE,
)
assert planner, "Vulkan MoveImage segment planner not found"
assert move, "Vulkan MoveImage implementation not found"
assert blit, "Vulkan blit_region implementation not found"
planner_body = planner.group(0)
move_body = move.group(0)
blit_body = blit.group(0)
assert "gpu_vram_split_transfer" in planner_body
assert "logical_x" in planner_body and "logical_y" in planner_body
assert "vk_vram_copy_segments(sx, sy" in move_body
assert "vk_vram_copy_segments(dx, dy" in move_body
assert "VkImageCopy copies[4]" in move_body
assert "(uint32_t)source_count, copies" in move_body
assert "for (int i = 0; i < destination_count; ++i)" in move_body
assert "blit_region(r->x, r->y, r->w, r->h" in move_body
assert move_body.index("p_vkCmdCopyImage") < move_body.index(
    "for (int i = 0; i < destination_count; ++i)"
), "destination writes begin before the complete overlap-safe source snapshot"
assert "(destination[i].logical_x - r->x) * S" in move_body
assert "(destination[i].logical_y - r->y) * S" in move_body
assert "if (sx + w > VRAM_W)" not in move_body
assert "if (dx + w > VRAM_W)" not in move_body
assert "s_vram[(dy + row)" not in move_body, (
    "overlapping MoveImage must not update the CPU mirror from a mutable source"
)
assert "gpu_vram_region_mark_rect(&s_gpu_dirty" in blit_body, (
    "MoveImage destinations are not marked stale in the CPU mirror"
)


def segments(x, y, w, h):
    """Reference the four-way logical/physical split used by Vulkan."""
    w = min(max(w, 0), 1024)
    h = min(max(h, 0), 512)
    if not w or not h:
        return []
    x &= 1023
    y &= 511
    widths = [(x, min(w, 1024 - x), 0)]
    if widths[0][1] < w:
        widths.append((0, w - widths[0][1], widths[0][1]))
    heights = [(y, min(h, 512 - y), 0)]
    if heights[0][1] < h:
        heights.append((0, h - heights[0][1], heights[0][1]))
    return [
        (px, py, pw, ph, logical_x, logical_y)
        for py, ph, logical_y in heights
        for px, pw, logical_x in widths
    ]


# Source and destination use the same exact planner. Pin each endpoint wrapping
# independently in X and Y, plus the reported bottom-right source regression.
source_x = segments(1023, 20, 2, 2)
source_y = segments(20, 511, 2, 2)
source_corner = segments(1023, 511, 2, 2)
destination_x = segments(1023, 300, 2, 2)
destination_y = segments(300, 511, 2, 2)
destination_corner = segments(1023, 511, 2, 2)
assert source_x == [
    (1023, 20, 1, 2, 0, 0), (0, 20, 1, 2, 1, 0)
]
assert source_y == [
    (20, 511, 2, 1, 0, 0), (20, 0, 2, 1, 0, 1)
]
assert source_corner == [
    (1023, 511, 1, 1, 0, 0), (0, 511, 1, 1, 1, 0),
    (1023, 0, 1, 1, 0, 1), (0, 0, 1, 1, 1, 1),
]
assert destination_x == [
    (1023, 300, 1, 2, 0, 0), (0, 300, 1, 2, 1, 0)
]
assert destination_y == [
    (300, 511, 2, 1, 0, 0), (300, 0, 2, 1, 0, 1)
]
assert destination_corner == source_corner
assert segments(1022, 510, 4, 4) == [
    (1022, 510, 2, 2, 0, 0), (0, 510, 2, 2, 2, 0),
    (1022, 0, 2, 2, 0, 2), (0, 0, 2, 2, 2, 2),
]

# Overlapping destinations still source every piece from immutable logical
# scratch. These are the exact shader offsets before multiplication by scale.
overlap_destination = segments(1023, 511, 2, 2)
assert [(lx - x, ly - y) for x, y, _w, _h, lx, ly in overlap_destination] == [
    (-1023, -511), (1, -511), (-1023, 1), (1, 1)
]

print("Vulkan staging-cache and MoveImage wrap tests passed")
