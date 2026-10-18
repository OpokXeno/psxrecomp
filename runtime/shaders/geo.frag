#version 450
/* Untextured fragment: the literal PS1 colour endpoint (hardware Gouraud plane
 * when the primitive carries one, else the interpolated vertex colour),
 * dithered and quantized to 5 bits per channel. Ported verbatim from the GL
 * backend's GEO_FS. Mask bit (alpha) is carried so the stencil-mirror pass
 * reads it; for opaque draws alpha=1. */
#include "psx_raster.glsl"
layout(location = 0) noperspective in vec4 v_col;
layout(location = 1) noperspective in vec2 v_pos;
layout(location = 2) flat in uvec4 v_dda0;
layout(location = 3) flat in uvec4 v_dda1;
layout(location = 4) flat in uvec2 v_dda2;
layout(push_constant) uniform PC {
    float u_shift;
    float u_xoff;
    float u_xhalf;
    int u_dither;
    int u_scale;
    int u_dda_ok;
} pc;
layout(location = 0) out vec4 o_col;
void main() {
    ivec3 color8 = ivec3(clamp(v_col.rgb * 255.0 + 0.5, 0.0, 255.0));
    if (pc.u_dda_ok != 0 && v_dda2.y != 0u) {
        ivec2 p = ivec2(floor(v_pos));
        uvec2 px = uvec2(uint(p.x), uint(p.y));
        color8 = ivec3(int(psx_plane(v_dda0.x, v_dda0.y, v_dda0.z, px)),
                       int(psx_plane(v_dda0.w, v_dda1.x, v_dda1.y, px)),
                       int(psx_plane(v_dda1.z, v_dda1.w, v_dda2.x, px)));
    }
    o_col = vec4(psx_expand5(psx_quantize(color8, pc.u_dither, pc.u_scale)), v_col.a);
}
