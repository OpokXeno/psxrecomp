// Uniforms of the scanline post-process, shared by present.frag and interp.frag
// (see scanline.glsl). The function lives in its own file because it is included
// between the shaders' other functions, in that order, as it always was.
#define PSX_SCANLINE_UNIFORMS(U) \
    U(int, u_scanline) U(float, u_scanline_strength) \
    U(float, u_scanline_lines) U(float, u_scanline_scale)
