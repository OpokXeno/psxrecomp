#version 450
//! gl_version 430
#include "native_prelude.glsl"
layout(std430,PSX_BINDING(0,0)) buffer Metadata { uint meta[]; };
#ifdef VULKAN
layout(push_constant) uniform CapturePush { vec4 view; ivec4 rect; uvec4 info; };
#else
uniform vec4 view; uniform ivec4 rect; uniform uvec4 info;
#endif
void main(){ivec2 p=ivec2(gl_FragCoord.xy)-rect.xy;
 if(any(lessThan(p,ivec2(0)))||any(greaterThanEqual(p,rect.zw)))discard;
 atomicAdd(meta[4u+uint(p.y*rect.z+p.x)],1u);}
