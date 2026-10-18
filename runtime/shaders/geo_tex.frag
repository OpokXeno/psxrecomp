#version 450
#include "psx_raster.glsl"
/* Textured fragment: sample raw 1555 VRAM (integer), CLUT-decode per depth,
 * texture window, optional bilinear, texel-0 cutout discard, STP-split discard,
 * PS1 *2-around-0x80 colour modulation. Output alpha = bit15 of the written
 * pixel. Ported verbatim from the GL backend's TEX_FS (usampler2D u_vram = the
 * native R16_UINT raw VRAM mirror). */
layout(location = 0) noperspective in vec2  v_uv;
layout(location = 1) noperspective in vec4  v_col;
layout(location = 2) flat in ivec2 v_tpage;   /* texture page base, VRAM px */
layout(location = 3) flat in ivec2 v_clut;    /* CLUT base, VRAM px */
layout(location = 4) flat in int   v_depth;   /* 0=4bit 1=8bit 2=15bit */
layout(location = 5) flat in int   v_raw;     /* 1 = no colour modulation */
layout(location = 6) flat in ivec4 v_limits;  /* prim uv bounds (inclusive) */
layout(location = 7) smooth in vec2 v_uv_p;   /* perspective-correct UV */
layout(location = 8) flat   in int  v_persp;  /* 1 = use v_uv_p, else affine */
layout(location = 9)  noperspective in vec2 v_pos;
layout(location = 10) flat in uvec4 v_dda0;
layout(location = 11) flat in uvec4 v_dda1;
layout(location = 12) flat in uvec4 v_dda2;
layout(location = 13) flat in uvec4 v_dda3;

layout(set = 0, binding = 0) uniform usampler2D u_vram;

layout(push_constant) uniform PC {
    float u_shift;
    float u_xoff;
    float u_xhalf;
    int   u_semipass;   /* 0=all texels, 1=STP=0 only, 2=STP=1 only */
    int   u_maskset;    /* GP0(E6h) set-mask: OR bit15 into output */
    int   u_filter;     /* 1 = bilinear */
    ivec4 u_twin;       /* texture window: mask_x, mask_y, off_x, off_y */
    int   u_dither;     /* effective per-primitive GP0(E1) dither */
    int   u_scale;      /* HR samples per native VRAM pixel */
    int   u_dda_ok;     /* 1 = the planes describe this pass's pixel grid */
} pc;

layout(location = 0) out vec4 frag;

int vram_at(int x, int y) {
    return int(texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r);
}
int fetch_texel(int u, int v) {
    u &= 255; v &= 255;
    if ((pc.u_twin.x | pc.u_twin.y) != 0) {
        u = (u & ~(pc.u_twin.x * 8)) | ((pc.u_twin.z & pc.u_twin.x) * 8);
        v = (v & ~(pc.u_twin.y * 8)) | ((pc.u_twin.w & pc.u_twin.y) * 8);
    } else {
        u = clamp(u, v_limits.x, v_limits.z);
        v = clamp(v, v_limits.y, v_limits.w);
    }
    if (v_depth == 0) {
        int px = vram_at(v_tpage.x + (u >> 2), v_tpage.y + v);
        return vram_at(v_clut.x + ((px >> ((u & 3) * 4)) & 0xF), v_clut.y);
    } else if (v_depth == 1) {
        int px = vram_at(v_tpage.x + (u >> 1), v_tpage.y + v);
        return vram_at(v_clut.x + ((px >> ((u & 1) * 8)) & 0xFF), v_clut.y);
    }
    return vram_at(v_tpage.x + u, v_tpage.y + v);
}
ivec3 col5i(int raw) {
    return ivec3(raw & 31, (raw >> 5) & 31, (raw >> 10) & 31);
}
vec3 col5(int raw) {
    return vec3(col5i(raw)) / 31.0;
}
void main() {
    int stp;
    vec3 rgb = vec3(0.0);
    ivec3 nearest5 = ivec3(0);
    /* v_persp is 0 for every prim unless [video] perspective_texturing is on
     * AND this prim's packet carried full GTE projection provenance, so the
     * default is the PS1's affine (noperspective) mapping. */
    vec2 uv = (v_persp != 0) ? v_uv_p : v_uv;
    /* Hardware U/V/R/G/B planes (gpu_dda.h), exact per VRAM pixel. The bilinear
     * enhancement keeps float interpolation. */
    bool dda = v_dda3.w != 0u && pc.u_dda_ok != 0 && pc.u_filter == 0;
    ivec2 dda_uv = ivec2(0);
    ivec3 dda_col = ivec3(0);
    if (dda) {
        ivec2 p = ivec2(floor(v_pos));
        uvec2 px = uvec2(uint(p.x), uint(p.y));
        dda_uv = ivec2(int(psx_plane(v_dda0.x, v_dda0.y, v_dda0.z, px)),
                       int(psx_plane(v_dda0.w, v_dda1.x, v_dda1.y, px)));
        dda_col = ivec3(int(psx_plane(v_dda1.z, v_dda1.w, v_dda2.x, px)),
                        int(psx_plane(v_dda2.y, v_dda2.z, v_dda2.w, px)),
                        int(psx_plane(v_dda3.x, v_dda3.y, v_dda3.z, px)));
    }
    if (pc.u_filter == 0) {
        int raw = dda ? fetch_texel(dda_uv.x, dda_uv.y)
                      : fetch_texel(int(floor(uv.x)), int(floor(uv.y)));
        if (raw == 0) discard;
        nearest5 = col5i(raw);
        stp = (raw >> 15) & 1;
    } else {
        /* Bilinear, Beetle-PSX formulation: the NEAREST texel is the base
         * (cutout + STP authority), the neighbours lie toward the sub-texel
         * offset and clamp to v_limits. Transparent neighbours are ignored and
         * the colour is renormalised, but a transparent base stays cut out.
         * v_uv is aligned for PS1 top-left point sampling; recenter the
         * bilinear footprint so 1x rect tiles sample their own texels. */
        uv += vec2(pc.u_shift);
        int iu = int(floor(uv.x)), iv = int(floor(uv.y));
        float fx = uv.x - float(iu) - 0.5, fy = uv.y - float(iv) - 0.5;
        int sx = fx < 0.0 ? -1 : 1, sy = fy < 0.0 ? -1 : 1;
        fx = abs(fx); fy = abs(fy);
        int c00 = fetch_texel(iu, iv);
        if (c00 == 0) discard;
        int c10 = fetch_texel(iu + sx, iv);
        int c01 = fetch_texel(iu, iv + sy);
        int c11 = fetch_texel(iu + sx, iv + sy);
        float w00 = (c00 == 0 ? 0.0 : 1.0) * (1.0 - fx) * (1.0 - fy);
        float w10 = (c10 == 0 ? 0.0 : 1.0) * fx * (1.0 - fy);
        float w01 = (c01 == 0 ? 0.0 : 1.0) * (1.0 - fx) * fy;
        float w11 = (c11 == 0 ? 0.0 : 1.0) * fx * fy;
        float opac = w00 + w10 + w01 + w11;
        rgb = (col5(c00) * w00 + col5(c10) * w10 + col5(c01) * w01 + col5(c11) * w11) / opac;
        float stpf = (float((c00 >> 15) & 1) * w00 + float((c10 >> 15) & 1) * w10
                    + float((c01 >> 15) & 1) * w01 + float((c11 >> 15) & 1) * w11) / opac;
        stp = stpf >= 0.5 ? 1 : 0;
    }
    if (pc.u_semipass == 1 && stp == 1) discard;
    if (pc.u_semipass == 2 && stp == 0) discard;
    ivec3 color5;
    if (pc.u_filter == 0) {
        if (v_raw != 0) {
            color5 = nearest5;
        } else {
            ivec3 color8 = dda ? dda_col
                               : ivec3(clamp(v_col.rgb * 255.0 + 0.5, 0.0, 255.0));
            color5 = psx_quantize((nearest5 * color8) >> 4, pc.u_dither, pc.u_scale);
        }
    } else {
        if (v_raw == 0) rgb = clamp(rgb * v_col.rgb * 2.0, 0.0, 1.0);
        ivec3 color8 = ivec3(clamp(rgb * 255.0 + 0.5, 0.0, 255.0));
        color5 = psx_quantize(color8, v_raw == 0 ? pc.u_dither : 0, pc.u_scale);
    }
    frag = vec4(psx_expand5(color5), (stp == 1 || pc.u_maskset == 1) ? 1.0 : 0.0);
}
