#version 450
//! gl_version 330
#include "native_prelude.glsl"
// Resolve RGB and its per-pixel AA class together. The old scanout copied
// RGBA twice, then rewrote alpha across the entire image. R8 keeps the exact
// class byte with one quarter of the mask storage; its alpha swizzle preserves
// the presenter's existing sampler contract. All objects belong to the GPU.
// Vertex stage: fullscreen.vert.
#define SCANOUT_DRAW_UNIFORMS(U) U(vec4, mapping)
PSX_SAMPLER2D(0, image)
PSX_UNIFORMS(ScanoutDraw, 0, SCANOUT_DRAW_UNIFORMS)
layout(location=0)out vec4 color;layout(location=1)out float mask;
void main(){ivec2 p=ivec2(floor(gl_FragCoord.xy*mapping.zw+mapping.xy));
vec4 c=texelFetch(image,p,0);color=vec4(c.rgb,1.0);mask=c.a;}
