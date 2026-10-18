#ifndef PSX_RUNTIME_MENU_DRAW_BOUNDS_H
#define PSX_RUNTIME_MENU_DRAW_BOUNDS_H

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

/* ClipRect limits a draw; it is not its occupied area. In particular ImGui's
 * menu-bar background can retain a full-viewport clip while its triangles
 * occupy only the top rows. Include AA fringe vertices and a physical-pixel
 * rounding margin. Unknown callbacks retain conservative full-window replay. */
static inline bool runtime_menu_draw_bounds(const ImDrawData *data,
        float &left, float &top, float &right, float &bottom) {
    if (!data) return false;
    const ImVec2 offset = data->DisplayPos, scale = data->FramebufferScale;
    if (!(scale.x > 0.f && scale.y > 0.f) || !std::isfinite(scale.x) || !std::isfinite(scale.y)) return true;
    for (int l = 0; l < data->CmdListsCount; ++l) {
        const ImDrawList &list = *data->CmdLists[l];
        for (const ImDrawCmd &command : list.CmdBuffer) {
            if (command.UserCallback) {
                if (command.UserCallback != ImDrawCallback_ResetRenderState) return true;
                continue;
            }
            if (!command.ElemCount) continue;
            const uint64_t end = (uint64_t)command.IdxOffset + command.ElemCount;
            if (end > (uint64_t)list.IdxBuffer.Size) return true;
            float x0 = std::numeric_limits<float>::infinity(), y0 = x0;
            float x1 = -x0, y1 = -y0;
            for (uint64_t i = command.IdxOffset; i < end; ++i) {
                const uint64_t vertex = (uint64_t)command.VtxOffset + list.IdxBuffer[(int)i];
                if (vertex >= (uint64_t)list.VtxBuffer.Size) return true;
                const ImVec2 p = list.VtxBuffer[(int)vertex].pos;
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) return true;
                x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
                x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
            }
            x0 = std::max(x0, command.ClipRect.x); y0 = std::max(y0, command.ClipRect.y);
            x1 = std::min(x1, command.ClipRect.z); y1 = std::min(y1, command.ClipRect.w);
            if (x0 >= x1 || y0 >= y1) continue;
            left = std::min(left, (x0 - offset.x) * scale.x - 1.f);
            top = std::min(top, (y0 - offset.y) * scale.y - 1.f);
            right = std::max(right, (x1 - offset.x) * scale.x + 1.f);
            bottom = std::max(bottom, (y1 - offset.y) * scale.y + 1.f);
        }
    }
    return false;
}

/* A closed, font-only menu bar has no mutable external textures or callbacks.
 * Its exact draw inputs can therefore reuse a published UI image. */
static inline bool runtime_menu_draw_signature(const ImDrawData *data, ImTextureID font,
        int width, int height, std::vector<unsigned char> &out) {
    out.clear();
    if (!data || data->CmdListsCount != 1) return false;
    const ImDrawList &list = *data->CmdLists[0];
    for (const ImDrawCmd &command : list.CmdBuffer)
        if (command.UserCallback || command.GetTexID() != font) return false;
    const auto append = [&out](const void *data, size_t size) {
        if (!size) return;
        const auto *bytes = static_cast<const unsigned char *>(data);
        out.insert(out.end(), bytes, bytes + size);
    };
    append(&width,sizeof(width)); append(&height,sizeof(height));
    append(&data->DisplayPos,sizeof(data->DisplayPos));
    append(&data->DisplaySize,sizeof(data->DisplaySize));
    append(&data->FramebufferScale,sizeof(data->FramebufferScale));
    append(&list.VtxBuffer.Size,sizeof(list.VtxBuffer.Size));
    append(list.VtxBuffer.Data,(size_t)list.VtxBuffer.Size*sizeof(ImDrawVert));
    append(&list.IdxBuffer.Size,sizeof(list.IdxBuffer.Size));
    append(list.IdxBuffer.Data,(size_t)list.IdxBuffer.Size*sizeof(ImDrawIdx));
    append(&list.CmdBuffer.Size,sizeof(list.CmdBuffer.Size));
    for (const ImDrawCmd &command : list.CmdBuffer) {
        const ImTextureID texture=command.GetTexID();
        append(&texture,sizeof(texture));append(&command.ClipRect,sizeof(command.ClipRect));
        append(&command.VtxOffset,sizeof(command.VtxOffset));append(&command.IdxOffset,sizeof(command.IdxOffset));
        append(&command.ElemCount,sizeof(command.ElemCount));
    }
    return true;
}

#endif
