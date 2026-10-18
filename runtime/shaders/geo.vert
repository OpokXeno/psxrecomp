#version 450
/* Untextured geometry (flat / gouraud triangles, line pixel quads, flat rects).
 * Position is in PSX VRAM pixel space (0..1024, 0..512); map to NDC for the
 * 1024x512 VRAM image (resolution-independent, so the SxS supersample is just
 * a larger framebuffer). VRAM y increases downward; Vulkan clip y also points
 * down, so VRAM y=0 lands at image row 0 (top) with no flip.
 *
 * u_shift = half an HR pixel (0.5/S in native units): the rasterizer samples
 * coverage at pixel centers, the PS1 DDA at integer coords. The shift aligns
 * the two grids (matches the GL backend's GEO_VS). u_xoff/u_xhalf carry the
 * native-wide x translation / clip half-extent (0 / 512 canonical). The
 * a_dda* attributes are the hardware Gouraud plane (gpu_dda.h). */
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec4 in_col;
layout(location = 2) in uvec4 a_dda0;
layout(location = 3) in uvec4 a_dda1;
layout(location = 4) in uvec2 a_dda2;
layout(push_constant) uniform PC {
    float u_shift;
    float u_xoff;
    float u_xhalf;
    int u_dither;
    int u_scale;
    int u_dda_ok;
} pc;
layout(location = 0) noperspective out vec4 v_col;
layout(location = 1) noperspective out vec2 v_pos;
layout(location = 2) flat out uvec4 v_dda0;
layout(location = 3) flat out uvec4 v_dda1;
layout(location = 4) flat out uvec2 v_dda2;
void main() {
    v_col = in_col;
    v_pos = in_pos;
    v_dda0 = a_dda0; v_dda1 = a_dda1; v_dda2 = a_dda2;
    gl_Position = vec4((in_pos.x + pc.u_shift + pc.u_xoff) / pc.u_xhalf - 1.0,
                       (in_pos.y + pc.u_shift) / 256.0 - 1.0, 0.0, 1.0);
}
