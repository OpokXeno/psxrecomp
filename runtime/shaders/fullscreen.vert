#version 450
//! gl_version 330
//! variant SCENE_RESOLVE NATIVE_SCENE_RESOLVE=1
#include "native_prelude.glsl"
#ifdef NATIVE_SCENE_RESOLVE
PSX_VARYING(0) out vec2 scene_uv;
#endif
// Fullscreen triangle: scanout resolve and UI layer composite.
void main(){ vec2 p = vec2((VERTEX_ID<<1)&2, VERTEX_ID&2);
#ifdef NATIVE_SCENE_RESOLVE
  scene_uv=p;
#endif
  gl_Position = vec4(p*2.0-1.0,0.0,1.0); }
