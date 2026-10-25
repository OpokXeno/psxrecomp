#version 450
//! gl_version 330
#include "../shaders/native_depth.glsl"
uniform ivec4 plane;
uniform ivec2 origin;
uniform uint stored;
uniform uint bias;
layout(location=0) out uvec4 result;
void main(){
 uint key=depth_evaluate(plane,ivec2(gl_FragCoord.xy)-origin);
 uint tested=depth_biased(key,bias),threshold=depth_threshold(stored);
 result=uvec4(key,tested,threshold,tested>=threshold?1u:0u);
}
