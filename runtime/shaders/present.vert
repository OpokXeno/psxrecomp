#version 450
//! gl_version 330
#include "present_common.glsl"
PSX_VARYING(0) out vec2 v_uv;
PSX_UNIFORMS(PresentRect, 0, PRESENT_RECT_UNIFORMS)
void main(){ vec2 p = vec2((VERTEX_ID<<1)&2, VERTEX_ID&2);
  v_uv = vec2(mix(u_uv_rect.x,u_uv_rect.z,p.x),
              mix(u_uv_rect.y,u_uv_rect.w,1.0-p.y));
  gl_Position = vec4(p*2.0-1.0,0.0,1.0); }
