// Shared by native_gpu.vert and native_gpu.frag: dialect prelude, interlock extension
// and the per-view uniform list both stages declare.
#include "native_prelude.glsl"
#ifdef NATIVE_GPU_INTERLOCK
#extension GL_ARB_fragment_shader_interlock : require
#define NATIVE_VIEW_UNIFORMS(U) U(vec4, u_size) U(vec4, u_raster) U(vec4, u_source_raster) U(vec4, u_raster_pages)
#else
#define NATIVE_VIEW_UNIFORMS(U) U(vec4, size) U(vec4, raster) U(vec4, source_raster) U(vec4, raster_pages)
#endif
