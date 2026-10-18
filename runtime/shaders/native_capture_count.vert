#version 450
//! gl_version 430
#include "native_prelude.glsl"
layout(location=0) in vec2 p;
layout(location=3) in float q;
#ifdef VULKAN
layout(push_constant) uniform CapturePush { vec4 view; ivec4 rect; uvec4 info; };
#else
uniform vec4 view; uniform ivec4 rect; uniform uvec4 info;
#endif
void main(){float w=q>0.0?1.0/q:1.0;
 gl_Position=vec4(((p+view.z)/view.xy*2.0-1.0)*w,0.0,w);}
