// Shared by native_gpu.vert and native_gpu.frag: dialect prelude, interlock extension
// and the per-view uniform list both stages declare.
#include "native_prelude.glsl"
#ifdef NATIVE_GPU_INTERLOCK
#extension GL_ARB_fragment_shader_interlock : require
#define NATIVE_VIEW_UNIFORMS(U) U(vec4, u_size)
#else
#define NATIVE_VIEW_UNIFORMS(U) U(vec4, size)
#endif
