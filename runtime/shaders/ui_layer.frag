#version 450
//! gl_version 330
//! variant SCENE_RESOLVE NATIVE_SCENE_RESOLVE=1
#include "native_prelude.glsl"
PSX_SAMPLER2D(0, u_tex) PSX_VARYING(0) out vec4 frag;
#ifdef NATIVE_SCENE_RESOLVE
PSX_VARYING(0) in vec2 scene_uv;
layout(push_constant) uniform SceneResolve { int filtered; };
void main(){
 ivec2 size=textureSize(u_tex,0);
 ivec2 p=clamp(ivec2(scene_uv*vec2(size)),ivec2(0),size-1);
 vec4 c;
 if(filtered==0)c=texelFetch(u_tex,p,0);
 else{
  c=texture(u_tex,scene_uv);
  if(c.a>0.0)c=texelFetch(u_tex,p,0);
 }
 frag=vec4(c.rgb,1.0);
}
#else
void main(){ ivec2 size = textureSize(u_tex, 0);
  ivec2 p = ivec2(gl_FragCoord.xy);
   frag = (p.x < size.x && p.y < size.y) ? texelFetch(u_tex, p, 0) : vec4(0.0); }
#endif
