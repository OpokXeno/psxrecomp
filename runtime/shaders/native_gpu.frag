#version 450
//! gl_version 330
//! gl_variant INTERLOCK 420 NATIVE_GPU_INTERLOCK=1
//! gl_variant INTERLOCK_GENERIC_RASTER 420 NATIVE_GPU_INTERLOCK=1 NATIVE_GPU_GENERIC_RASTER=1
//! variant INTERLOCK NATIVE_GPU_INTERLOCK=1
//! variant INTERLOCK_GENERIC_RASTER NATIVE_GPU_INTERLOCK=1 NATIVE_GPU_GENERIC_RASTER=1
//! variant SPECIALIZED NATIVE_GPU_SPECIALIZED=1
//! variant CORE_BATCH NATIVE_GPU_CORE_BATCH=1
//! variant CORE_BATCH_INDEX_SPECIALIZED NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_SPECIALIZED=1 NATIVE_GPU_RECORD_BUFFER=1
//! variant CORE_BATCH_SPECIALIZED NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_SPECIALIZED=1
//! variant CORE_BATCH_FOOTPRINT NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_VERTEX_FOOTPRINT=1
//! variant CORE_BATCH_FOOTPRINT_SPECIALIZED NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_SPECIALIZED=1 NATIVE_GPU_VERTEX_FOOTPRINT=1
//! variant CAPTURE NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_CAPTURE=1
//! variant CAPTURE_SPECIALIZED NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_CAPTURE=1 NATIVE_GPU_SPECIALIZED=1
//! variant CAPTURE_LINKED NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_CAPTURE=1 NATIVE_GPU_LINKED_CAPTURE=1
//! variant CAPTURE_LINKED_SPECIALIZED NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_CAPTURE=1 NATIVE_GPU_LINKED_CAPTURE=1 NATIVE_GPU_SPECIALIZED=1
//! variant CAPTURE_LINKED_FOOTPRINT NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_CAPTURE=1 NATIVE_GPU_LINKED_CAPTURE=1 NATIVE_GPU_VERTEX_FOOTPRINT=1
//! variant CAPTURE_LINKED_FOOTPRINT_SPECIALIZED NATIVE_GPU_CORE_BATCH=1 NATIVE_GPU_CAPTURE=1 NATIVE_GPU_LINKED_CAPTURE=1 NATIVE_GPU_VERTEX_FOOTPRINT=1 NATIVE_GPU_SPECIALIZED=1
#ifdef NATIVE_GPU_LINKED_CAPTURE
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_KHR_shader_subgroup_shuffle : require
#endif
#include "native_gpu_common.glsl"
#include "native_depth.glsl"

#if defined(VULKAN) && !defined(NATIVE_GPU_INTERLOCK)
#define NATIVE_DECODE_UNIFORM(U) U(int, decoded_on)
#else
#define NATIVE_DECODE_UNIFORM(U)
#define decoded_on 0
#endif

#ifdef NATIVE_GPU_CORE_BATCH
/* Preserve the loose UBO layout, but source triangle-local values from flat
 * vertex outputs. Material state remains in the ordinary uniform block. */
#define sample_lim unused_sample_lim
#define depth_plane unused_depth_plane
#define attribute_origin unused_attribute_origin
#define uv_gradient unused_uv_gradient
#define depth_origin unused_depth_origin
#define attribute_plane unused_attribute_plane
#define attribute_dda unused_attribute_dda
#define page unused_page
#define window unused_window
#define hd_map unused_hd_map
#define hd_lim unused_hd_lim
#endif

/* Per-draw uniforms. The interlock variant reads them through u_-prefixed names
 * into globals (load_parameters) because batched draws take the same values from
 * the per-triangle parameter records instead. Inclusive sampled texel range of the
 * triangle (gpu_uv.h limits) is sample_lim. depth_state = (test, mode of an
 * unblended fragment 1=far 2=key 3=copy source, per-texel keep, bias);
 * depth_plane = (FP32 seed, physical dx, physical dy, integer scale), with
 * float bits transported in xyz. The origin is in physical pixels.
 * depth_view = (0 off / 1 depth colour / 2 policy / 3 depth grey,
 * policy colour, log2 key range x256). HD texture replacement: hd_map = (page->
 * upload texel offset u/v, upload size in texels), hd_lim = the draw's sampled
 * page-texel rectangle. */
#ifdef NATIVE_GPU_INTERLOCK
#define NATIVE_DRAW_UNIFORMS(U) \
    U(ivec4, hd_map) U(ivec4, hd_lim) U(ivec4, mip_rect) \
    U(ivec4, u_sample_lim) U(ivec4, u_state) U(ivec4, u_flags) U(ivec4, u_page) U(ivec4, u_window) \
    U(vec4, u_uv_gradient) \
    U(ivec4, u_depth_state) U(ivec4, u_depth_plane) U(ivec4, u_depth_view) \
    U(vec4, u_constant_color) U(vec4, u_attribute_origin) \
    U(vec4, u_attribute_plane[5]) U(ivec4, u_attribute_dda[5]) \
    U(ivec2, u_origin) U(ivec2, u_depth_origin) \
    U(int, hd_on) U(int, mip_on) \
    U(int, parameters_on) U(int, parameter_base) U(int, parameter_shared) \
    U(int, u_depth) U(int, u_aa_exempt) U(int, u_texture_filter) U(int, u_anisotropy) \
    U(float, u_filter_strength)
#else
#define NATIVE_DRAW_UNIFORMS(U) \
    U(ivec4, hd_map) U(ivec4, hd_lim) U(ivec4, mip_rect) \
    U(ivec4, sample_lim) U(ivec4, state) U(ivec4, flags) U(ivec4, page) U(ivec4, window) \
    U(vec4, uv_gradient) \
    U(ivec4, depth_state) U(ivec4, depth_plane) U(ivec4, depth_view) \
    U(vec4, constant_color) U(vec4, attribute_origin) \
    U(vec4, attribute_plane[5]) U(ivec4, attribute_dda[5]) \
    U(ivec2, origin) U(ivec2, depth_origin) \
    U(int, hd_on) U(int, mip_on) \
    U(int, u_depth) U(int, aa_exempt) U(int, texture_filter) U(int, anisotropy) \
    U(float, filter_strength) NATIVE_DECODE_UNIFORM(U)
#endif
PSX_UNIFORMS(NativeView, 0, NATIVE_VIEW_UNIFORMS)
PSX_UNIFORMS(NativeDraw, 1, NATIVE_DRAW_UNIFORMS)
#include "native_raster.glsl"

#ifdef NATIVE_GPU_CORE_BATCH
#undef sample_lim
#undef depth_plane
#undef attribute_origin
#undef uv_gradient
#undef depth_origin
#undef attribute_plane
#undef attribute_dda
#undef page
#undef window
#undef hd_map
#undef hd_lim
#ifdef NATIVE_GPU_RECORD_BUFFER
PSX_VARYING(4) flat in int triangle_record_index;
PSX_USAMPLERBUFFER(7, triangle_records)
uvec4 triangle_data[12];
#else
PSX_VARYING(4) flat in uvec4 triangle_data[12];
#endif
#define sample_lim ivec4(triangle_data[0])
#define depth_plane ivec4(triangle_data[1])
#define attribute_origin uintBitsToFloat(triangle_data[2])
#define uv_gradient uintBitsToFloat(triangle_data[3])
#define depth_origin ivec2(triangle_data[4].xy)
#define page ivec4(triangle_data[10])
#define window ivec4(triangle_data[11])
#define hd_map ivec4(triangle_data[3])
#define hd_lim ivec4(triangle_data[5].w,triangle_data[6].w,triangle_data[7].w,triangle_data[8].w)
vec4 attribute_plane[5]; ivec4 attribute_dda[5];
#if defined(VULKAN) && !defined(NATIVE_GPU_CAPTURE)
layout(std430,set=0,binding=12) readonly buffer DrawDamage { uint damage_words[]; };
void reject_unchanged_tile(){
 uint offset=triangle_data[4].w;if(offset==0u)return;--offset;
 ivec2 p=ivec2(gl_FragCoord.xy);
 ivec4 clip=ivec4(damage_words[offset+1u],damage_words[offset+2u],damage_words[offset+3u],damage_words[offset+4u]);
 if(any(lessThan(p,clip.xy))||any(greaterThanEqual(p,clip.zw)))discard;
 uint geometry=damage_words[offset],shift=geometry>>16u;
 uint tile=(uint(p.y)>>shift)*(geometry&65535u)+(uint(p.x)>>shift);
 if((damage_words[offset+5u+(tile>>5u)]&(1u<<(tile&31u)))==0u)discard;
}
#endif
#endif
#ifdef NATIVE_GPU_VERTEX_FOOTPRINT
PSX_VARYING(16) flat in vec4 triangle_footprint;
PSX_VARYING(17) flat in int triangle_taps;
#endif

#ifdef NATIVE_GPU_SPECIALIZED
/* Vulkan pipelines specialize material control flow, retaining the same
 * per-triangle arithmetic, texture taps and integer destination semantics. */
layout(constant_id=0) const int material_textured=0;
layout(constant_id=1) const int material_raw=0;
layout(constant_id=2) const int material_semi=0;
layout(constant_id=3) const int material_blend=0;
layout(constant_id=4) const int material_mask_set=0;
layout(constant_id=5) const int material_mask_check=0;
layout(constant_id=6) const int material_dither=0;
layout(constant_id=7) const int material_transfer=0;
layout(constant_id=8) const int material_hd=0;
layout(constant_id=9) const int material_mip=0;
layout(constant_id=10) const int material_filter=0;
layout(constant_id=11) const int material_aniso=0;
layout(constant_id=12) const int material_test=0;
layout(constant_id=13) const int material_keep=0;
layout(constant_id=14) const int material_tex_depth=0;
layout(constant_id=15) const int material_snapshot=0;
layout(constant_id=16) const float material_attribute=0.0;
layout(constant_id=17) const float material_flat=0.0;
layout(constant_id=18) const float material_scale=1.0;
layout(constant_id=19) const int material_depth_scale=1;
layout(constant_id=20) const int material_depth_write=0;
layout(constant_id=21) const int material_depth_bias=0;
layout(constant_id=22) const int material_decoded=0;
layout(constant_id=23) const int material_early_reject=0;
layout(constant_id=24) const int material_aniso_gather=0;
layout(constant_id=25) const int material_damage=0;
layout(constant_id=26) const int material_hardware_aniso=0;
#define decoded_on material_decoded
#define state ivec4(material_textured,material_raw,material_semi,material_blend)
#define flags ivec4(material_mask_set,material_mask_check,material_dither,material_transfer)
#define texture_filter material_filter
#define anisotropy material_aniso
#define hd_on material_hd
#define mip_on material_mip
#define depth_state ivec4(material_test,material_depth_write,material_keep,material_depth_bias)
#ifdef NATIVE_GPU_CORE_BATCH
#undef depth_plane
#undef attribute_origin
#define depth_plane ivec4(ivec3(triangle_data[1].xyz),material_depth_scale)
#define attribute_origin vec4(uintBitsToFloat(triangle_data[2].xy),material_attribute,material_flat)
#else
#define depth_plane ivec4(depth_plane.xyz,material_depth_scale)
#define attribute_origin vec4(attribute_origin.xy,material_attribute,material_flat)
#endif
#define size vec4(size.xyz,material_scale)
#define depth_view ivec4(0)
#define u_depth ((u_depth&~3)|material_tex_depth)
#define NATIVE_WORD_SNAPSHOT (material_snapshot!=0)
#else
#define NATIVE_WORD_SNAPSHOT (word_snapshot!=0)
#endif

PSX_USAMPLER2D(0, words)
#if defined(VULKAN) && !defined(NATIVE_GPU_INTERLOCK)
PSX_USAMPLER2D(11, decoded_words)
#if defined(VULKAN) && !defined(NATIVE_GPU_INTERLOCK)
layout(set=0,binding=15) uniform usampler2DArray decoded_layers;
layout(set=0,binding=14) uniform sampler2DArray decoded_filtered;
#endif
#endif
PSX_USAMPLER2D(4, source_depth)
PSX_SAMPLER2D(2, source_image)
/* The interlock variant reads the destination through storage images instead. */
#if !defined(NATIVE_GPU_CAPTURE) && (!defined(VULKAN) || !defined(NATIVE_GPU_INTERLOCK))
NATIVE_DEST_DECLS
#endif
PSX_USAMPLERBUFFER(8, recipe_words) int word_snapshot=0;
PSX_SAMPLER2D(5, hd_image)
PSX_SAMPLER2D(6, mip_image)
#ifdef NATIVE_GPU_INTERLOCK
layout(pixel_interlock_ordered) in;
layout(rgba8,PSX_BINDING(0,9)) coherent uniform image2D destination_image;
layout(r32ui,PSX_BINDING(1,10)) coherent uniform uimage2D destination_depth_image;
PSX_USAMPLERBUFFER(7, parameters) PSX_VARYING(4) flat in int parameter_index;
ivec4 sample_lim=ivec4(0),state=ivec4(0),flags=ivec4(0),page=ivec4(0),window=ivec4(0); int depth=0,aa_exempt=0,texture_filter=0,anisotropy=0; ivec2 origin=ivec2(0);
float filter_strength=0.0; vec4 uv_gradient=vec4(0.0); bool pixel_written;
#else
int depth=0;
#endif
#ifdef NATIVE_GPU_INTERLOCK
ivec4 depth_state=ivec4(0),depth_plane=ivec4(0),depth_view=ivec4(0); ivec2 depth_origin=ivec2(0);
#endif
#ifdef NATIVE_GPU_INTERLOCK
vec4 size=vec4(0.0),constant_color=vec4(0.0);
#endif
PSX_VARYING(0) noperspective in vec2 t; PSX_VARYING(2) smooth in vec2 t_p; PSX_VARYING(3) flat in int persp; PSX_VARYING(1) noperspective in vec4 color;
#ifdef VULKAN
#ifdef NATIVE_GPU_CORE_BATCH
/* Wireframe always uses the loose pipeline; keep the shared helper valid. */
const vec3 wire_barycentric=vec3(0.0);
#else
PSX_VARYING(5) noperspective in vec3 wire_barycentric;
#endif
#endif
#if defined(VULKAN) && (defined(NATIVE_GPU_INTERLOCK) || defined(NATIVE_GPU_CAPTURE))
/* The attachment-less interlock pass has no outputs: the results go to the storage images. */
vec4 result; uvec4 depth_result;
#else
layout(location=0) out vec4 result; layout(location=1) out uvec4 depth_result;
#endif
uint depth_key(){
 vec2 pos=native_raster_position(gl_FragCoord.xy);
 ivec2 delta=ivec2(pos)-depth_origin;
 return NATIVE_RASTER.x>0.0?depth_evaluate_at(depth_plane,native_raster_delta(gl_FragCoord.xy,vec2(depth_origin))):depth_evaluate(depth_plane,delta);
}
/* Depth views: black = no certified surface; otherwise t over the frame's
 * written key range (1 = near). Mode 1 maps t on a Turbo ramp (red near,
 * blue far), mode 3 on grey (near bright, far dark). */
/* lw is the screen-space width of one contour step (fwidth, taken by the
 * caller in uniform control flow): contours stay about one pixel wide. */
vec3 depth_gray(uint k,float lw){if(k==0u)return vec3(0.0);
 float l=log2(uintBitsToFloat(k)); float lo=float(depth_view.z)/256.0,hi=float(depth_view.w)/256.0;
 float t=hi>lo+0.01?clamp((l-lo)/(hi-lo),0.0,1.0):0.5;
 float contour=fract(l*16.0)<min(lw,0.5)?0.5:1.0;
 if(depth_view.x==3)return vec3(0.12+0.88*t)*contour;
 vec4 v4=vec4(1.0,t,t*t,t*t*t); vec2 v2=v4.zw*v4.z;
 vec3 c=vec3(dot(v4,vec4(0.13572138,4.61539260,-42.66032258,132.13108234))+dot(v2,vec2(-152.94239396,59.28637943)),
  dot(v4,vec4(0.09140261,2.19418839,4.84296658,-14.18503333))+dot(v2,vec2(4.27729857,2.82956604)),
  dot(v4,vec4(0.10667330,12.64194608,-60.58204836,110.36276771))+dot(v2,vec2(-89.90310912,27.34824973)));
 return clamp(c,0.0,1.0)*contour;}
#ifdef NATIVE_GPU_INTERLOCK
vec4 attribute_origin=vec4(0.0),attribute_plane[5]; ivec4 attribute_dda[5];
void load_parameters(){
 if(parameters_on==0){state=u_state;flags=u_flags;page=u_page;window=u_window;sample_lim=u_sample_lim;
 depth=u_depth&3;word_snapshot=u_depth>>2;origin=u_origin;aa_exempt=u_aa_exempt;texture_filter=u_texture_filter;filter_strength=u_filter_strength;
 anisotropy=u_anisotropy;uv_gradient=u_uv_gradient;size=u_size;constant_color=u_constant_color;
 depth_state=u_depth_state;depth_plane=u_depth_plane;depth_view=u_depth_view;depth_origin=u_depth_origin;
 attribute_origin=u_attribute_origin;for(int a=0;a<5;++a){attribute_plane[a]=u_attribute_plane[a];attribute_dda[a]=u_attribute_dda[a];}return;}
/* native_gpu_batch_draw: 14 texels per triangle, then 6 per shared record. */
 int b=(parameter_index-parameter_base)*14;
 state=ivec4(texelFetch(parameters,b));
#ifndef NATIVE_GPU_GENERIC_RASTER
if(state.x!=0){
#endif
 page=ivec4(texelFetch(parameters,b+1));sample_lim=ivec4(texelFetch(parameters,b+3));
 uv_gradient=uintBitsToFloat(texelFetch(parameters,b+6));
#ifndef NATIVE_GPU_GENERIC_RASTER
}
#endif
 depth_state=ivec4(texelFetch(parameters,b+2));
 depth_plane=ivec4(texelFetch(parameters,b+4));attribute_origin=uintBitsToFloat(texelFetch(parameters,b+5));
 ivec4 scalars=ivec4(texelFetch(parameters,b+7));aa_exempt=scalars.x;texture_filter=scalars.y;anisotropy=scalars.z;
 uvec4 own=texelFetch(parameters,b+8);depth=int(own.x&3u);word_snapshot=int(own.x>>2u);depth_origin=ivec2(own.yz);
#ifdef NATIVE_GPU_GENERIC_RASTER
 for(int a=0;a<5;++a){uvec4 v=texelFetch(parameters,b+9+a);attribute_dda[a]=ivec4(v);attribute_plane[a]=uintBitsToFloat(v);}
#endif
 int s=parameter_shared+int(own.w)*6;
 flags=ivec4(texelFetch(parameters,s));window=ivec4(texelFetch(parameters,s+1));
 depth_view=ivec4(texelFetch(parameters,s+2));size=uintBitsToFloat(texelFetch(parameters,s+3));
 constant_color=uintBitsToFloat(texelFetch(parameters,s+4));
 uvec4 misc=texelFetch(parameters,s+5);origin=ivec2(misc.yz);filter_strength=uintBitsToFloat(misc.w);
}
vec4 destination_color(){return imageLoad(destination_image,ivec2(gl_FragCoord.xy));}
uint destination_key(){return imageLoad(destination_depth_image,ivec2(gl_FragCoord.xy)).r;}
#define REJECT_PIXEL {pixel_written=false;return;}
#else
#ifdef NATIVE_GPU_CAPTURE
vec4 destination_color(){return vec4(0.0);}
uint destination_key(){return 0u;}
#else
vec4 destination_color(){return DEST_COLOR();}
uint destination_key(){return DEST_DEPTH().r;}
#endif
#define REJECT_PIXEL discard
#endif
float attribute_sample(int a){
#if defined(NATIVE_GPU_INTERLOCK) && !defined(NATIVE_GPU_GENERIC_RASTER)
 if(parameters_on!=0){uvec4 v=texelFetch(parameters,(parameter_index-parameter_base)*14+9+a);
  if(attribute_origin.z==1.0){ivec2 delta=ivec2(floor(native_raster_position(gl_FragCoord.xy)))-ivec2(attribute_origin.xy*size.w);
   uint q=v.x+v.y*uint(delta.x)+v.z*uint(delta.y);return float(q&1048575u)/4096.0;}
  vec3 p=uintBitsToFloat(v.xyz);vec2 delta=native_raster_delta(gl_FragCoord.xy,attribute_origin.xy*size.w)/size.w;
  return p.x+dot(p.yz,delta);}
#endif
 if(attribute_origin.z==1.0){
  ivec2 delta=ivec2(floor(native_raster_position(gl_FragCoord.xy)))-ivec2(attribute_origin.xy*size.w);
  uvec3 p=uvec3(attribute_dda[a].xyz); uint q=p.x+p.y*uint(delta.x)+p.z*uint(delta.y);
  return float(q&1048575u)/4096.0;
 }
 vec2 delta=native_raster_delta(gl_FragCoord.xy,attribute_origin.xy*size.w)/size.w;
 return attribute_plane[a].x+dot(attribute_plane[a].yz,delta);
}
float attribute_at(int a){return floor(attribute_sample(a));}
int word_at(ivec2 p){p&=ivec2(1023,511);return NATIVE_WORD_SNAPSHOT?
 int(texelFetch(recipe_words,(word_snapshot-1)*524288+p.y*1024+p.x).r):int(texelFetch(words,p,0).r);}
ivec2 guest_coordinates(ivec2 q){q&=255; if((window.x|window.y)==0)q=clamp(q,sample_lim.xy,sample_lim.zw);
 q=(q&~(window.xy*8))|((window.zw&window.xy)*8);
 return q;}
int guest_texel(ivec2 q){q=guest_coordinates(q);
#if defined(VULKAN) && !defined(NATIVE_GPU_INTERLOCK)
 if(decoded_on==2)return int(texelFetch(decoded_layers,ivec3(q,page.x),0).r);
 if(decoded_on!=0)return int(texelFetch(decoded_words,q,0).r);
#endif
 int shift=depth==0?2:depth==1?1:0; int w=word_at(page.xy+ivec2(q.x>>shift,q.y));
 if(depth<2){int index=depth==0?(w>>((q.x&3)*4))&15:(w>>((q.x&1)*8))&255; w=word_at(page.zw+ivec2(index,0));}
 return w;}
/* Extra samples integrate the elongated texel-space footprint along its
 * principal axis. Each sample honours the same atlas clamp, CLUT lookup and
 * transparent-texel renormalization as the ordinary bilinear footprint. */
vec4 anisotropic_tap(vec2 uv){ivec2 base=ivec2(floor(uv));
 vec2 f=fract(uv)-0.5; ivec2 dir=ivec2(f.x<0.0?-1:1,f.y<0.0?-1:1); f=abs(f);
 int a=guest_texel(base),b=guest_texel(base+ivec2(dir.x,0));
 int c=guest_texel(base+ivec2(0,dir.y)),d=guest_texel(base+dir);
 vec4 w=vec4((1.0-f.x)*(1.0-f.y),f.x*(1.0-f.y),(1.0-f.x)*f.y,f.x*f.y);
 w*=vec4(float(a!=0),float(b!=0),float(c!=0),float(d!=0));
 float total=dot(w,vec4(1.0)); if(total==0.0)return vec4(0.0);
 vec3 rgb=vec3(ivec3(a,a>>5,a>>10)&31)*w.x+vec3(ivec3(b,b>>5,b>>10)&31)*w.y+
  vec3(ivec3(c,c>>5,c>>10)&31)*w.z+vec3(ivec3(d,d>>5,d>>10)&31)*w.w;
 return vec4(rgb/total,1.0);}
/* Decoded mip texels carry premultiplied coverage so transparent palette
 * entries never darken opaque art. Guest nearest remains the STP/cutout
 * authority; this function supplies presentation RGB only. */
vec4 mip_color(vec2 uv,float lod,int point){
 vec2 local=(uv-vec2(mip_rect.xy))/vec2(mip_rect.zw); vec4 h;
 if(point!=0){int max_level=int(floor(log2(float(max(mip_rect.z,mip_rect.w)))));
  int level=int(clamp(floor(lod+0.5),0.0,float(max_level)));
  ivec2 shape=textureSize(mip_image,level);
  ivec2 p=clamp(ivec2(floor(local*vec2(shape))),ivec2(0),shape-ivec2(1));
  h=texelFetch(mip_image,p,level);}
 else h=textureLod(mip_image,local,lod);
 if(h.a<=0.00001)return vec4(0.0); return vec4(h.rgb*(31.0/h.a),h.a);}
/* An HD texel can cover a texel that is transparent in the guest image
 * (its anti-aliased edge overhangs the original silhouette). Such a texel
 * has no STP of its own; forcing 0 drew it opaque, and the dark colours a
 * semi-transparent HUD piece blends with showed as black edge pixels. Take
 * the STP of the nearest opaque guest texel inside the sampled rectangle;
 * with none, follow the material's semi-transparency. */
int hd_fringe_stp(ivec2 q){
 const ivec2 o[8]=ivec2[8](ivec2(1,0),ivec2(-1,0),ivec2(0,1),ivec2(0,-1),ivec2(1,1),ivec2(-1,1),ivec2(1,-1),ivec2(-1,-1));
 for(int i=0;i<8;++i){ivec2 n=q+o[i]; if((window.x|window.y)==0)n=clamp(n,hd_lim.xy,hd_lim.zw);
  int w=guest_texel(n); if(w!=0)return (w>>15)&1;}
 return state.z!=0?1:0;}
ivec3 source_c=ivec3(0); int source_mask=0,source_blend=0; bool source_hd=false; vec3 source_hf=vec3(0.0);
#if defined(VULKAN) && defined(NATIVE_GPU_SPECIALIZED) && !defined(NATIVE_GPU_INTERLOCK) && !defined(NATIVE_GPU_CAPTURE)
bool destination_checked=false;
vec4 checked_color=vec4(0.0);
uint checked_key=0u,checked_stored=0u;
void check_destination(int blending){
 if(flags.y!=0||blending!=0)checked_color=destination_color();
 if(flags.y!=0&&checked_color.a>0.5)discard;
 if(depth_state.x==1||(depth_state.z!=0&&blending!=0))checked_stored=destination_key();
 if(depth_state.x!=0){checked_key=depth_key();uint tested=depth_biased(checked_key,uint(depth_state.w));
  if(depth_state.x==1&&tested<depth_threshold(checked_stored))discard;}
 destination_checked=true;
}
#endif
void shade_source(){
/* HD coordinates and their derivatives come first, in uniform control
 * flow: every later discard would leave the texture LOD undefined. The
 * sample point is the texel-space position of the fragment centre, like
 * the bilinear VRAM path (size.z is the rasterizer's grid shift). */
 vec2 hd_uv=vec2(0.0),hd_dx=vec2(0.0),hd_dy=vec2(0.0);
 if(hd_on!=0){vec2 hp=(persp!=0?t_p:t)+vec2(size.z); vec2 fl=floor(hp);
  ivec2 hq=ivec2(fl)&255; hq=(hq&~(window.xy*8))|((window.zw&window.xy)*8);
  if(hd_on==2){hd_uv=native_source_uv((hp+vec2(hd_map.xy))/vec2(hd_map.zw));}
  else {vec2 k=vec2(textureSize(hd_image,0))/vec2(hd_map.zw);
  vec2 tx=clamp(vec2(hq+hd_map.xy)+(hp-fl),vec2(hd_lim.xy+hd_map.xy)+0.5/k,vec2(hd_lim.zw+hd_map.xy)+1.0-0.5/k);
  hd_uv=tx/vec2(hd_map.zw);}}
 hd_dx=dFdx(hd_uv); hd_dy=dFdy(hd_uv);
/* Derivatives must precede every per-fragment discard. Affine gradients
 * come from the unwrapped attribute plane, not the modulo-256 DDA value. */
 vec2 ax=vec2(0.0),ay=vec2(0.0);
  if(anisotropy>1||mip_on!=0){ax=persp!=0?dFdx(t_p):uv_gradient.xy;
   ay=persp!=0?dFdy(t_p):uv_gradient.zw;}
#if defined(VULKAN) && defined(NATIVE_GPU_CORE_BATCH) && !defined(NATIVE_GPU_CAPTURE)
 /* Preserve derivatives for all lanes, then skip unchanged tiles before
  * palette lookup and anisotropic filtering. No duplicate region draws. */
#ifdef NATIVE_GPU_SPECIALIZED
 if(material_damage!=0)reject_unchanged_tile();
#else
 reject_unchanged_tile();
#endif
#endif
  if(flags.w!=0){
#ifdef VULKAN
/* One framebuffer pixel along each triangle edge. Evaluate derivatives
 * before rejection, outside the interlock. No optional rasterizer features
 * or wider line support are needed, including on the Vulkan 1.1 core path. */
   if(flags.w==3){vec3 dx=dFdx(wire_barycentric),dy=dFdy(wire_barycentric);
    vec3 width=max(sqrt(dx*dx+dy*dy),vec3(0.000001));
    if(all(greaterThan(wire_barycentric,width)))REJECT_PIXEL;}
#endif
   vec2 transfer_uv=native_transfer_uv(t,vec2(textureSize(source_image,0)));
   result=flags.w==2?texture(source_image,transfer_uv):constant_color;
  if(flags.z!=0)result.a=result.a>0.5?2.0/3.0:0.0;
  if(flags.x!=0)result.a=result.a>0.83||result.a>0.16&&result.a<0.5?1.0:2.0/3.0;
  depth_result=uvec4(depth_state.y==3?texture(source_depth,transfer_uv).r:0u);
  return;}
 vec3 rgb=vec3(128.0);
#ifndef NATIVE_GPU_GENERIC_RASTER
if(state.x==0||state.y==0){
#endif
#ifdef NATIVE_GPU_GENERIC_RASTER
 rgb=attribute_origin.z==0.0?floor(color.rgb+0.5):vec3(attribute_at(2),attribute_at(3),attribute_at(4));
#else
 rgb=attribute_origin.z==0.0||attribute_origin.w!=0.0?floor(color.rgb+0.5):vec3(attribute_at(2),attribute_at(3),attribute_at(4));
#endif
#ifndef NATIVE_GPU_GENERIC_RASTER
}
#endif
 ivec3 c=ivec3(clamp(rgb,0.0,255.0)); int mask=flags.x,blend=state.z;
 const int d[16]=int[16](-4,0,-3,1,2,-2,3,-1,-3,1,-4,0,3,-1,2,-2);
 ivec2 dp=ivec2(floor(native_raster_position(gl_FragCoord.xy)/size.w))+origin; int bias=flags.z!=0?d[(dp.y&3)*4+(dp.x&3)]:0;
 bool hd=false; vec3 hf=vec3(0.0);
 if(state.x!=0){
#ifdef NATIVE_GPU_GENERIC_RASTER
vec2 uvs=persp!=0?t_p:vec2(attribute_at(0),attribute_at(1));
#else
vec2 affine_uv=persp!=0?vec2(0.0):vec2(attribute_sample(0),attribute_sample(1));
 vec2 uvs=persp!=0?t_p:floor(affine_uv);
#endif
  ivec2 q=ivec2(mod(uvs,256.0))&255; int w=guest_texel(q);
/* Replacement: coverage and colour from the HD image (premultiplied by
 * coverage at upload), STP from the guest texel, full 8-bit precision. */
  if(hd_on!=0){vec4 h=textureGrad(hd_image,hd_uv,hd_dx,hd_dy); if(hd_on==1&&h.a<0.5)REJECT_PIXEL; hd=true;
   hf=hd_on==2?h.rgb:h.rgb/h.a; if(state.y==0)hf=clamp(hf*vec3(c)/128.0,0.0,1.0);
    int stp=w!=0?(w>>15)&1:hd_fringe_stp(ivec2(mod(uvs,256.0))&255); mask|=stp; blend&=stp;
#if defined(VULKAN) && defined(NATIVE_GPU_SPECIALIZED) && !defined(NATIVE_GPU_INTERLOCK)
    #ifndef NATIVE_GPU_CAPTURE
    if(material_early_reject!=0)check_destination(blend);
    #endif
#endif
  }else{
   if(w==0)REJECT_PIXEL; ivec3 tex=ivec3(w,w>>5,w>>10)&31; int stp=(w>>15)&1; mask|=stp; blend&=stp;
#if defined(VULKAN) && defined(NATIVE_GPU_SPECIALIZED) && !defined(NATIVE_GPU_INTERLOCK)
   /* Coverage is known and derivatives were already evaluated. Avoid
    * destination reads for transparent texels, and anisotropic work for
    * fragments that the exact depth/mask policy will reject. */
   #ifndef NATIVE_GPU_CAPTURE
   if(material_early_reject!=0)check_destination(blend);
   #endif
#endif
  if(texture_filter!=0||anisotropy>1||mip_on!=0){
/* For affine draws, base and weights use the same PS1 attribute accumulator.
 * Interpolating t separately can place its floor on the other side of a
 * texel boundary (especially for mirrored UVs), sending the filter toward
 * the wrong neighbour while coverage and STP still use attribute_at. */
#ifdef NATIVE_GPU_GENERIC_RASTER
   vec2 filter_uv=persp!=0?t_p+vec2(size.z):vec2(attribute_sample(0),attribute_sample(1));
#else
vec2 filter_uv=persp!=0?t_p+vec2(size.z):affine_uv;
#endif
   vec3 filtered=vec3(tex);
   if(texture_filter!=0){vec2 f=fract(filter_uv)-0.5;
    ivec2 step_dir=ivec2(f.x<0.0?-1:1,f.y<0.0?-1:1); f=abs(f);
    int w10=guest_texel(q+ivec2(step_dir.x,0)),w01=guest_texel(q+ivec2(0,step_dir.y));
    int w11=guest_texel(q+step_dir);
    vec4 weights=vec4((1.0-f.x)*(1.0-f.y),f.x*(1.0-f.y),(1.0-f.x)*f.y,f.x*f.y);
    weights*=vec4(1.0,float(w10!=0),float(w01!=0),float(w11!=0));
    weights/=dot(weights,vec4(1.0));
    filtered=vec3(tex)*weights.x+vec3(ivec3(w10,w10>>5,w10>>10)&31)*weights.y+
     vec3(ivec3(w01,w01>>5,w01>>10)&31)*weights.z+vec3(ivec3(w11,w11>>5,w11>>10)&31)*weights.w;}
/* Singular values of the UV Jacobian identify the principal footprint
 * even when both screen-space gradients point in nearly the same direction. */
   if(anisotropy>1||mip_on!=0){float xx=dot(ax,ax),yy=dot(ay,ay),xy=dot(ax,ay);
/* A magnified footprint cannot need anisotropic taps or a mip level.
 * The largest eigenvalue is at most the trace, so this rejects only
 * cases whose old path selected one tap and no mip. */
#ifndef NATIVE_GPU_GENERIC_RASTER
#ifdef NATIVE_GPU_VERTEX_FOOTPRINT
if(persp==0||xx+yy>0.99){
#else
if(xx+yy>0.99){
#endif
#endif
    float major2,minor2;int n;
#ifdef NATIVE_GPU_VERTEX_FOOTPRINT
    if(persp==0){major2=triangle_footprint.z;minor2=triangle_footprint.w;n=anisotropy>1?min(anisotropy,triangle_taps):1;}
    else
#endif
    {major2=0.5*(xx+yy+sqrt((xx-yy)*(xx-yy)+4.0*xy*xy));
     minor2=max(xx+yy-major2,1.0);
     n=anisotropy>1?min(anisotropy,int(min(16.0,ceil(sqrt(major2/minor2))))):1;}
    bool use_mip=mip_on!=0&&major2>1.0;
    if(n>1){vec2 span;
#ifdef NATIVE_GPU_VERTEX_FOOTPRINT
     if(persp==0)span=triangle_footprint.xy;else
#endif
     {vec2 dir=abs(xy)>0.000001?normalize(vec2(xy,major2-xx)):
       (xx>=yy?vec2(1.0,0.0):vec2(0.0,1.0));span=ax*dir.x+ay*dir.y;}
      bool hardware_sampled=false;
#if defined(VULKAN) && defined(NATIVE_GPU_SPECIALIZED) && !defined(NATIVE_GPU_INTERLOCK)
      if(material_hardware_aniso!=0){if(decoded_on!=0){
       /* Keep manual clamping/window rules at atlas boundaries. The normal
        * anisotropic sampler is used only for an interior footprint; cutout
        * and STP were already taken from the unfiltered integer guest word. */
       vec2 radius=0.5*(abs(ax)+abs(ay))+vec2(2.0);
       vec2 low=max(vec2(sample_lim.xy),vec2(0.0)),high=min(vec2(sample_lim.zw),vec2(255.0));
       if((window.x|window.y)==0&&mip_on==0&&all(greaterThanEqual(filter_uv-radius,low))&&all(lessThanEqual(filter_uv+radius,high))){
        vec4 h=decoded_on==2?textureGrad(decoded_filtered,vec3(filter_uv/256.0,float(page.x)),ax/256.0,ay/256.0)
            :textureGrad(mip_image,filter_uv/256.0,ax/256.0,ay/256.0);
        if(h.a>0.0)filtered=h.rgb*(255.0/h.a);
        hardware_sampled=true;
       }
      }}
#endif
      if(!hardware_sampled){vec3 sum=vec3(0.0); float valid=0.0;
#if defined(VULKAN) && defined(NATIVE_GPU_SPECIALIZED) && !defined(NATIVE_GPU_INTERLOCK)
      ivec2 cached_tile=ivec2(-1);uvec4 cached_texels=uvec4(0);
#endif
     for(int i=0;i<16;++i){if(i>=n)break;
      vec2 uv=filter_uv+((float(i)+0.5)/float(n)-0.5)*span;
      if(use_mip){vec4 tap=mip_color(uv,log2(sqrt(minor2)),texture_filter==0?1:0);
       sum+=tap.rgb*tap.a; valid+=tap.a;}
      else if(texture_filter!=0){vec4 tap=anisotropic_tap(uv);
       sum+=tap.rgb; valid+=tap.a;}
       else{int tap;
#if defined(VULKAN) && defined(NATIVE_GPU_SPECIALIZED) && !defined(NATIVE_GPU_INTERLOCK)
        if(material_aniso_gather!=0){
         if(decoded_on!=0){
          ivec2 sample_q=guest_coordinates(ivec2(floor(uv))),tile=sample_q>>1;
          if(any(notEqual(tile,cached_tile))){
           cached_tile=tile;
           cached_texels=decoded_on==2?textureGather(decoded_layers,vec3((vec2(tile*2)+vec2(1.0))/256.0,float(page.x)),0)
               :textureGather(decoded_words,(vec2(tile*2)+vec2(1.0))/256.0,0);
          }
          int x=sample_q.x&1,y=sample_q.y&1;
          tap=int(cached_texels[y==0?3-x:x]);
         }else tap=guest_texel(ivec2(floor(uv)));
        }else
#endif
        tap=guest_texel(ivec2(floor(uv)));
        if(tap!=0){sum+=vec3(ivec3(tap,tap>>5,tap>>10)&31); valid+=1.0;}}}
      if(valid>0.0)filtered=sum/valid;
      }
    }else if(use_mip){vec4 tap=mip_color(filter_uv,log2(sqrt(major2)),texture_filter==0?1:0);
     if(tap.a>0.0)filtered=tap.rgb;}
#ifndef NATIVE_GPU_GENERIC_RASTER
}
#endif
   }
   if(texture_filter!=0&&filter_strength<1.0)filtered=mix(vec3(tex),filtered,filter_strength);
   vec3 scaled=state.y!=0?filtered*8.0:floor(filtered*vec3(c)/16.0);
   c=clamp(ivec3(floor((scaled+float(state.y!=0?0:bias))/8.0)),ivec3(0),ivec3(31));
  }else c=state.y!=0?tex:clamp((((tex*c)>>4)+bias)>>3,ivec3(0),ivec3(31));}
 }else {
#if defined(VULKAN) && defined(NATIVE_GPU_SPECIALIZED) && !defined(NATIVE_GPU_INTERLOCK)
  #ifndef NATIVE_GPU_CAPTURE
  if(material_early_reject!=0)check_destination(blend);
  #endif
#endif
  c=clamp((c+bias)>>3,ivec3(0),ivec3(31));}
 source_c=c;source_mask=mask;source_blend=blend;source_hd=hd;source_hf=hf;}
void commit_pixel(){
 vec4 old=vec4(0);uint key=0u,stored=0u;
#if defined(VULKAN) && defined(NATIVE_GPU_SPECIALIZED) && !defined(NATIVE_GPU_INTERLOCK) && !defined(NATIVE_GPU_CAPTURE)
 if(destination_checked){old=checked_color;key=checked_key;stored=checked_stored;}else
#endif
 {
 if(flags.y!=0 || (flags.w==0 && state.z!=0)) old=destination_color();
 if(flags.y!=0 && old.a>0.5) REJECT_PIXEL;
 if(depth_state.x==1||depth_state.z!=0) stored=destination_key();
 if(depth_state.x!=0){key=depth_key(); uint tested=depth_biased(key,uint(depth_state.w));
  if(depth_state.x==1&&tested<depth_threshold(stored)) REJECT_PIXEL;}
 }
 if(flags.w!=0){if(depth_view.x==1||depth_view.x==3)
  result.rgb=depth_gray(depth_result.r,fwidth(log2(max(uintBitsToFloat(depth_result.r),1.17549435e-38))*16.0));return;}
 ivec3 c=source_c;int mask=source_mask,blend=source_blend;bool hd=source_hd;vec3 hf=source_hf;
 if(hd){if(blend!=0){vec3 b=old.rgb;
  if(state.w==0)hf=(b+hf)*0.5; else if(state.w==1)hf=b+hf; else if(state.w==2)hf=b-hf; else hf=b+hf*0.25;}
  result=vec4(clamp(hf,0.0,1.0),float(mask*2+aa_exempt)/3.0);
 }else{
 if(blend!=0){ivec3 b=ivec3(floor(old.rgb*255.0+0.5))>>3;
  if(state.w==0)c=(b+c)/2; else if(state.w==1)c=b+c; else if(state.w==2)c=b-c; else c=b+c/4;}
 c=clamp(c,ivec3(0),ivec3(31)); result=vec4(vec3((c<<3)|(c>>2))/255.0,float(mask*2+aa_exempt)/3.0);}
/* Blended fragments keep the stored key; opaque ones (including opaque
 * texels of a semi-transparent material) write the key or far. */
 depth_result=uvec4(depth_state.y==4?1u:blend!=0?stored:(depth_state.y==2?key:0u));
 if(depth_view.x==1||depth_view.x==3){float lw=fwidth(log2(max(uintBitsToFloat(depth_result.r),1.17549435e-38))*16.0);
  result.rgb=blend!=0?old.rgb:depth_gray(depth_result.r,lw);}
 else if(depth_view.x==2){vec3 tint=depth_view.y==1?vec3(1.0,0.85,0.1):depth_view.y==2?vec3(0.1,0.9,0.2):
  depth_view.y==3?vec3(1.0,0.15,0.15):vec3(dot(result.rgb,vec3(0.3,0.59,0.11)));
  result.rgb=depth_view.y==0?tint*0.6:mix(result.rgb,tint,0.55);}}
#ifdef NATIVE_GPU_CAPTURE
layout(std430,set=0,binding=12) buffer CaptureMetadata { uint capture_meta[]; };
layout(std430,set=0,binding=13) writeonly buffer CaptureFragments { uvec4 capture_fragments[]; };
layout(push_constant) uniform CaptureRect {
 layout(offset=4) int capture_x; int capture_y; int capture_width; int capture_height;
};
void capture_pixel(){
#ifndef NATIVE_GPU_LINKED_CAPTURE
 if(gl_HelperInvocation)discard;
#endif
 if(source_hd||flags.w!=0){atomicOr(capture_meta[0],1u);discard;}
 ivec2 p=ivec2(gl_FragCoord.xy)-ivec2(capture_x,capture_y);
 if(any(lessThan(p,ivec2(0)))||p.x>=capture_width||p.y>=capture_height)discard;
 uint pixels=uint(capture_width*capture_height),pixel=uint(p.y*capture_width+p.x);
#ifdef NATIVE_GPU_LINKED_CAPTURE
 bool live=!gl_HelperInvocation;uint slot=0u;
  if(live)slot=atomicAdd(capture_meta[4u+pixels+pixel],1u);
  if(slot>=128u){atomicOr(capture_meta[0],2u);discard;}
 uint node;
 if(triangle_data[8].w!=0u){
  if(!live)discard;
  ivec2 local=ivec2(gl_FragCoord.xy)-ivec2(triangle_data[5].w,triangle_data[6].w);
  if(any(lessThan(local,ivec2(0)))||uint(local.x)>=triangle_data[7].w||uint(local.y)>=triangle_data[9].w){
   atomicOr(capture_meta[0],512u);discard;
  }
  if(triangle_data[4].w>=capture_meta[2]){atomicOr(capture_meta[0],4u);discard;}
  node=triangle_data[4].w+uint(local.y)*triangle_data[7].w+uint(local.x);
 }else{
 /* One allocator atomic per live subgroup. Explicitly exclude helpers and
  * broadcast from a live lane: helper-side atomics cannot allocate nodes. */
 uvec4 ballot_mask=subgroupBallot(live);uint base=0u;
 if(subgroupBallotBitCount(ballot_mask)==0u)discard;
 uint leader=subgroupBallotFindLSB(ballot_mask);
 if(gl_SubgroupInvocationID==leader)base=atomicAdd(capture_meta[1],subgroupBallotBitCount(ballot_mask));
 base=subgroupShuffle(base,leader);
 if(!live)discard;
  node=base+subgroupBallotExclusiveBitCount(ballot_mask);
 }
 if(node>=capture_meta[2]){atomicOr(capture_meta[0],4u);discard;}
 uint next=atomicExchange(capture_meta[4u+pixel],node);
#else
 uint slot=atomicAdd(capture_meta[4u+pixels+pixel],1u);
 uint offset=capture_meta[4u+2u*pixels+pixel]+capture_meta[4u+3u*pixels+(pixel>>8u)];
 if(slot>=128u||slot>=capture_meta[4u+pixel]||offset+slot>=capture_meta[2]){atomicOr(capture_meta[0],1u);discard;}
#endif
 uint key=depth_state.x!=0?depth_key():0u;
 uint tested=depth_biased(key,uint(depth_state.w));
 uint bits=uint(source_c.r)|(uint(source_c.g)<<5u)|(uint(source_c.b)<<10u)|
  (uint(source_mask)<<15u)|(uint(source_blend)<<16u)|(uint(aa_exempt)<<17u)|
  (uint(state.w)<<18u)|(uint(flags.y)<<20u)|(uint(depth_state.x)<<21u)|
  (uint(depth_state.y)<<22u)|(uint(min(depth_state.x+depth_state.z,1))<<25u);
#ifdef NATIVE_GPU_LINKED_CAPTURE
 /* Depth bias is clamped to 0..16 by geometry preparation. Keep the full
  * FP32 key bits and reconstruct the biased key in resolve. */
 capture_fragments[node]=uvec4(triangle_data[4].z,key,bits|(uint(depth_state.w)<<26u),next);
#else
 capture_fragments[offset+slot]=uvec4(triangle_data[4].z,key,tested,bits);
#endif
}
#endif
void main(){result=vec4(0.0);depth_result=uvec4(0);
#ifdef NATIVE_GPU_RECORD_BUFFER
 for(int i=0;i<12;++i)triangle_data[i]=texelFetch(triangle_records,triangle_record_index+i);
#endif
#ifdef NATIVE_GPU_CORE_BATCH
 for(int a=0;a<5;++a){attribute_plane[a]=uintBitsToFloat(triangle_data[5+a]);attribute_dda[a]=ivec4(triangle_data[5+a]);}
#endif
#ifdef NATIVE_GPU_CAPTURE
  if(capture_meta[0]!=0u)discard;
#ifdef NATIVE_GPU_LINKED_CAPTURE
  /* Damage rectangles are clipped to the configured tiles. Reject untouched tiles
   * before texture filtering, keeping a triangle in a single material batch. */
  if(capture_meta[3]!=0u){
   uint geometry=capture_meta[3];uvec2 tile=uvec2(gl_FragCoord.xy)>>(geometry>>16u);
   uint bit=tile.y*(geometry&65535u)+tile.x;
   uint at=4u+2u*uint(capture_width*capture_height)+(bit>>5u);
   if((capture_meta[at]&(1u<<(bit&31u)))==0u)discard;
  }
#endif
  depth=u_depth&3;word_snapshot=u_depth>>2;shade_source();capture_pixel();
#elif defined(NATIVE_GPU_INTERLOCK)
 load_parameters();pixel_written=true;shade_source();
 beginInvocationInterlockARB();
 if(pixel_written)commit_pixel();
 if(pixel_written){imageStore(destination_image,ivec2(gl_FragCoord.xy),result);
  if(any(notEqual(depth_state,ivec4(0))))imageStore(destination_depth_image,ivec2(gl_FragCoord.xy),depth_result);}
 endInvocationInterlockARB();
 if(!pixel_written)discard;
#else
 depth=u_depth&3;word_snapshot=u_depth>>2;shade_source();commit_pixel();
#endif
}
