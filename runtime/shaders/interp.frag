#version 450
//! gl_version 330
#include "native_prelude.glsl"
#include "scanline_uniforms.glsl"
// Frame interpolation between two presented frames; the vertex stage is present.vert.
#define INTERP_DRAW_UNIFORMS(U) \
    U(float, u_alpha) U(int, u_blend_mode) U(float, u_gamma) \
    PSX_SCANLINE_UNIFORMS(U)
PSX_VARYING(0) in vec2 v_uv; PSX_SAMPLER2D(0, u_prev) PSX_SAMPLER2D(1, u_curr)
PSX_UNIFORMS(InterpDraw, 1, INTERP_DRAW_UNIFORMS)
PSX_VARYING(0) out vec4 frag;
#include "scanline.glsl"
void main(){
  vec4 prev=texture(u_prev,v_uv), curr=texture(u_curr,v_uv);
  float alpha=u_alpha;
  if(u_blend_mode==1){
    vec3 d=abs(prev.rgb-curr.rgb);
    float change=max(max(d.r,d.g),d.b);
    float safe_blend=1.0-smoothstep(0.08,0.20,change);
    alpha=mix(step(0.5,u_alpha),u_alpha,safe_blend);
  }
  vec4 c=mix(prev,curr,alpha);
  c.rgb=psx_scanline(c.rgb, v_uv.y);
  if (u_gamma > 0.0 && u_gamma != 1.0) c.rgb=pow(max(c.rgb, vec3(0.0)), vec3(1.0/u_gamma));
  frag=c;
}
