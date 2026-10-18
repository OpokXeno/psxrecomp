// Hardware raster math shared by the legacy geometry programs (geo.frag,
// geo_tex.frag): the PS1 ordered-dither matrix and the 5-bit quantizer, and the
// Q24 interpolant plane evaluation of gpu_dda.h.
//
// One literal PS1 color endpoint for both geometry programs. Inputs are in the
// GPU's pre-quantization integer domain: RGB888 for untextured shading, and
// (texel5 * modulation8) >> 4 for modulated textures. gl_FragCoord is the final
// VRAM destination, so the matrix phase includes the GP0(E5) draw offset exactly.
const int PSX_DITHER[16] = int[16](
    -4, 0, -3, 1,  2, -2, 3, -1,  -3, 1, -4, 0,  3, -1, 2, -2);

ivec3 psx_quantize(ivec3 color, int dither_on, int scale) {
    ivec2 p = ivec2(gl_FragCoord.xy) / scale;
    int d = dither_on != 0 ? PSX_DITHER[((p.y & 3) << 2) | (p.x & 3)] : 0;
    return clamp((color + ivec3(d)) >> 3, ivec3(0), ivec3(31));
}

vec3 psx_expand5(ivec3 color5) { return vec3(color5 << 3) / 255.0; }

// Plane value at the VRAM pixel containing the fragment. v_pos is the vertex
// position in VRAM px, which the u_shift grid maps to (pixel + 1/64) at the
// fragment, so floor() recovers the integer pixel.
uint psx_plane(uint base, uint dx, uint dy, uvec2 pixel) {
    return (base + dx * pixel.x + dy * pixel.y) >> 24;
}
