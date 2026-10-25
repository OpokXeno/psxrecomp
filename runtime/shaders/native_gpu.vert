#version 450
//! gl_version 330
//! gl_variant INTERLOCK 420 NATIVE_GPU_INTERLOCK=1
//! gl_variant INTERLOCK_GENERIC_RASTER 420 NATIVE_GPU_INTERLOCK=1 NATIVE_GPU_GENERIC_RASTER=1
//! variant INTERLOCK NATIVE_GPU_INTERLOCK=1
//! variant CORE_BATCH NATIVE_GPU_CORE_BATCH=1
//! variant CORE_BATCH_INDEX NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_RECORD_BUFFER=1
//! variant CORE_BATCH_FOOTPRINT NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_VERTEX_FOOTPRINT=1
#include "native_gpu_common.glsl"
layout(location=0) in vec2 p; layout(location=1) in vec2 uv; layout(location=2) in vec4 c;
layout(location=3) in float q;   /* reciprocal-z weight; 0 = affine */
PSX_UNIFORMS(NativeView, 0, NATIVE_VIEW_UNIFORMS)
#include "native_raster.glsl"
#ifdef NATIVE_GPU_INTERLOCK
#define size u_size
#endif
PSX_VARYING(0) noperspective out vec2 t; PSX_VARYING(1) noperspective out vec4 color;
/* t_p is a smooth varying: setting the homogeneous w from the weight makes
 * the rasterizer divide it by 1/w, which is exactly the perspective-correct
 * interpolation V = sum(li*Vi*wi)/sum(li*wi) with wi proportional to view z. */
PSX_VARYING(2) smooth out vec2 t_p; PSX_VARYING(3) flat out int persp;
#if defined(VULKAN) && !defined(NATIVE_GPU_CORE_BATCH)
/* Debug wireframe uses triangle-edge distances, not fillModeNonSolid or a
 * geometry shader. Native vertex ranges always start on a triangle boundary. */
PSX_VARYING(5) noperspective out vec3 wire_barycentric;
#endif
#ifdef NATIVE_GPU_INTERLOCK
PSX_VARYING(4) flat out int parameter_index;
#endif
#ifdef NATIVE_GPU_CORE_BATCH
layout(constant_id=0) const bool first_vertex_records=false;
PSX_USAMPLERBUFFER(7, triangle_records)
layout(push_constant) uniform BatchOffset { uint triangle_record_base; };
#ifdef NATIVE_GPU_RECORD_BUFFER
PSX_VARYING(4) flat out int triangle_record_index;
#else
PSX_VARYING(4) flat out uvec4 triangle_data[12];
#endif
#endif
#ifdef NATIVE_GPU_VERTEX_FOOTPRINT
PSX_VARYING(16) flat out vec4 triangle_footprint;
PSX_VARYING(17) flat out int triangle_taps;
#endif
void main(){
#ifdef NATIVE_GPU_CORE_BATCH
int record=int(triangle_record_base)+(VERTEX_ID/3)*12;
#ifdef NATIVE_GPU_RECORD_BUFFER
triangle_record_index=record;
#else
/* Vulkan triangle lists take flat values from the first vertex. The
 * other two vertices only need position/UV/colour; their triangle records
 * cannot be consumed by a fragment. */
if(!first_vertex_records||VERTEX_ID%3==0){
 for(int i=0;i<12;++i)triangle_data[i]=texelFetch(triangle_records,record+i);
}else{
 for(int i=0;i<12;++i)triangle_data[i]=uvec4(0);
}
#endif
#endif
#ifdef NATIVE_GPU_VERTEX_FOOTPRINT
vec4 gradient=uintBitsToFloat(triangle_data[3]);
vec2 ax=gradient.xy,ay=gradient.zw;
float xx=dot(ax,ax),yy=dot(ay,ay),xy=dot(ax,ay);
float major2=0.5*(xx+yy+sqrt((xx-yy)*(xx-yy)+4.0*xy*xy));
float minor2=max(xx+yy-major2,1.0);
triangle_taps=int(min(16.0,ceil(sqrt(major2/minor2))));
vec2 span=vec2(0.0);
if(triangle_taps>1){vec2 dir=abs(xy)>0.000001?normalize(vec2(xy,major2-xx)):
    (xx>=yy?vec2(1.0,0.0):vec2(0.0,1.0));span=ax*dir.x+ay*dir.y;}
triangle_footprint=vec4(span,major2,minor2);
#endif
#if defined(VULKAN) && !defined(NATIVE_GPU_CORE_BATCH)
wire_barycentric=vec3(VERTEX_ID%3==0?1.0:0.0,VERTEX_ID%3==1?1.0:0.0,VERTEX_ID%3==2?1.0:0.0);
#endif
#ifdef NATIVE_GPU_INTERLOCK
parameter_index=VERTEX_ID/3;
#endif
t=uv; t_p=uv; color=c; persp=q>0.0?1:0;
  float w=q>0.0?1.0/q:1.0;
/* Scaling both x/y and w leaves the NDC position unchanged, so coverage is
 * identical to the affine path; only the varying interpolation changes. */
  vec2 grid_shift=vec2(size.z);
  if(size.z!=0.0)grid_shift+=0.5/size.w*(native_raster_transform().xy-vec2(1.0));
  vec2 local=p-vec2(0.0,NATIVE_RASTER_PAGES.x);
  gl_Position=vec4(((local+grid_shift)/size.xy*2.0-1.0)*w,0.0,w);}
