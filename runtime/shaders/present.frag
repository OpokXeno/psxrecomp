#version 450
//! gl_version 330
//! variant NATIVE_GENERIC NATIVE_PRESENT_SCENE_MASK=1
//! variant NATIVE_SPECIALIZED NATIVE_PRESENT_SPECIALIZED=1 NATIVE_PRESENT_SCENE_MASK=1
//! variant SMAA_EDGES NATIVE_SMAA_EDGES=1
//! variant SMAA_CACHED NATIVE_SMAA_CACHED=1 NATIVE_PRESENT_SCENE_MASK=1
//! variant SMAA_CACHED_SPECIALIZED NATIVE_SMAA_CACHED=1 NATIVE_PRESENT_SPECIALIZED=1 NATIVE_PRESENT_SCENE_MASK=1
#include "present_common.glsl"
#include "scanline_uniforms.glsl"
// Present sampling. u_sharp==0 is the historical behaviour: sample straight at
// v_uv, so the texture's own filter (NEAREST or LINEAR) decides everything.
//
// u_sharp==1 selects SHARP-BILINEAR, for upscaling a low-res source (FMV) to a
// much larger window. Plain GL_LINEAR blends across the whole texel and turns a
// 320x192 movie to mush; plain GL_NEAREST keeps it crisp but blocky and makes
// the non-integer scale factor beat (some source pixels land 3 window pixels
// wide, their neighbours 4). Sharp-bilinear keeps each texel flat across its
// interior and confines the linear ramp to a ONE-OUTPUT-PIXEL-wide band at the
// texel boundary: crisp like nearest, but without the uneven pixel widths.
//
// u_sharp_scale is output pixels per texel. At <=1 (downscale) the band covers
// the whole texel and this degrades to plain bilinear, which is what you want
// there. The result is clamped to u_uv_rect so the half-texel edge inset the
// caller applied still holds - that inset is what keeps LINEAR from bleeding
// the border texel into the image (the old reason FMV was pinned to NEAREST).
#define PRESENT_DRAW_UNIFORMS(U) \
    U(vec2, u_tex_size) U(vec2, u_sharp_scale) U(vec2, u_aa_drawable) U(vec2, u_aa_jitter) \
    U(int, u_screenlut_on) U(int, u_sharp) U(float, u_gamma) \
    U(int, u_aa_mode) U(int, u_aa_factor) U(int, u_aa_history_valid) \
    U(int, u_aa_mask_valid) U(int, u_aa_nearest) \
    PSX_SCANLINE_UNIFORMS(U)
PSX_VARYING(0) in vec2 v_uv; PSX_SAMPLER2D(0, u_tex) PSX_VARYING(0) out vec4 frag;
PSX_SAMPLER2D(1, u_screenlut)
PSX_UNIFORMS(PresentRect, 0, PRESENT_RECT_UNIFORMS)
PSX_UNIFORMS(PresentDraw, 1, PRESENT_DRAW_UNIFORMS)
#ifdef NATIVE_PRESENT_SPECIALIZED
/* Native presentation fixes the spatial mode for a whole draw. Compile out
 * unrelated AA/history/sharp-upscaling paths instead of carrying their
 * branches and temporary registers through every output pixel. */
layout(constant_id=0) const int present_aa_mode=0;
layout(constant_id=1) const int present_aa_factor=16;
layout(constant_id=2) const int present_mask_valid=0;
layout(constant_id=3) const int present_nearest=0;
layout(constant_id=4) const int present_lut=0;
#define u_aa_mode present_aa_mode
#define u_aa_factor present_aa_factor
#define u_aa_mask_valid present_mask_valid
#define u_aa_nearest present_nearest
#define u_screenlut_on present_lut
#define u_sharp 0
#define u_gamma 0.0
#define u_scanline 0
#endif
PSX_SAMPLER2D(2, u_aa_history) PSX_SAMPLER2D(3, u_aa_mask)
float aa_luma(vec3 c){return dot(c,vec3(0.299,0.587,0.114));}
bool aa_exempt(vec2 uv){float a=texture(u_aa_mask,uv).a;
 return (a>0.16&&a<0.5)||a>0.83;}
bool aa_guard(vec2 uv){if(u_aa_mask_valid==0)return false;
 vec2 d=fwidth(v_uv);
 return aa_exempt(uv)||aa_exempt(uv+vec2(d.x,0))||aa_exempt(uv-vec2(d.x,0))||
 aa_exempt(uv+vec2(0,d.y))||aa_exempt(uv-vec2(0,d.y));}
vec4 aa_spatial(vec2 uv){
/* At native 1x, FXAA operates on the nearest-expanded window image. Its
 * edge neighbourhood is one output pixel, not one enlarged guest texel;
 * flat interiors therefore keep their exact texture colours. */
 vec2 pixel=u_aa_nearest!=0?fwidth(v_uv):1.0/vec2(textureSize(u_tex,0));
#ifdef NATIVE_SMAA_CACHED
 if(u_aa_mode==2){
  ivec2 p=ivec2(gl_FragCoord.xy),lo=ivec2(u_sharp_scale),hi=lo+ivec2(u_tex_size)-1;
  vec4 edge_data=texelFetch(u_aa_history,p,0);vec3 c=texture(u_tex,uv).rgb;
  if(edge_data.a==0.0)return vec4(c,1);
  bool horizontal=edge_data.r>edge_data.g;
  ivec2 axis=horizontal?ivec2(1,0):ivec2(0,1);
  float threshold=edge_data.b*0.5,left=0.0,right=0.0;
  for(int i=1;i<=32;++i){if(i>u_aa_factor*2)break;
   vec2 e=texelFetch(u_aa_history,clamp(p-axis*i,lo,hi),0).rg;
   if((horizontal?e.r:e.g)<threshold)break;left=float(i);}
  if(left==0.0)return vec4(c,1);
  for(int i=1;i<=32;++i){if(i>u_aa_factor*2)break;
   vec2 e=texelFetch(u_aa_history,clamp(p+axis*i,lo,hi),0).rg;
   if((horizontal?e.r:e.g)<threshold)break;right=float(i);}
  if(right==0.0)return vec4(c,1);
  float weight=clamp(0.5*min(left,right)/(left+right+1.0),0.0,0.45);
  vec2 step_uv=1.0/u_tex_size;
  vec2 across=horizontal?vec2(0,step_uv.y):vec2(step_uv.x,0);
  return vec4(mix(c,texture(u_tex,uv+across*edge_data.a).rgb,weight),1);
 }
#endif
/* The Native raster has destination feedback and mask-bit semantics: a GL
 * multisample FBO cannot be sampled by the next draw. Resolve ordered
 * raster samples instead (fully shaded, unlike coverage-only HW MSAA). */
 if(u_aa_mode==4 || u_aa_mode==5){
  int count=u_aa_factor;
  int grid=count>=16?4:count>=8?(u_aa_mode==5?3:4):count>=2?2:1;
  vec4 sum=vec4(0.0); vec2 foot=fwidth(uv);
  for(int i=0;i<count;++i){
   int columns=grid,rows=(count+grid-1)/grid;
   int j=count==2&&i==1?3:count==8&&u_aa_mode==5&&i>=4?i+1:i;
   vec2 p=(vec2(j%columns,j/columns)+0.5)/vec2(columns,rows)-0.5;
   sum+=texture(u_tex,uv+p*foot);}
  return sum/float(count);
 }
 if(u_aa_mode!=1 && u_aa_mode!=2)return texture(u_tex,uv);
 vec3 c=texture(u_tex,uv).rgb; float l=aa_luma(c);
 float n=aa_luma(texture(u_tex,uv+vec2(0,pixel.y)).rgb);
 float s=aa_luma(texture(u_tex,uv-vec2(0,pixel.y)).rgb);
 float e=aa_luma(texture(u_tex,uv+vec2(pixel.x,0)).rgb);
 float w=aa_luma(texture(u_tex,uv-vec2(pixel.x,0)).rgb);
 float contrast=max(max(abs(l-n),abs(l-s)),max(abs(l-e),abs(l-w)));
 if(contrast<max(0.025,0.125*max(max(n,s),max(e,w))))return vec4(c,1);
 if(u_aa_mode==1){
  float nw=aa_luma(texture(u_tex,uv+pixel*vec2(-1,1)).rgb);
  float ne=aa_luma(texture(u_tex,uv+pixel).rgb);
  float sw=aa_luma(texture(u_tex,uv-pixel).rgb);
  float se=aa_luma(texture(u_tex,uv+pixel*vec2(1,-1)).rgb);
  vec2 dir=vec2(-((nw+ne)-(sw+se)),(nw+sw)-(ne+se));
  float reduce=max((nw+ne+sw+se)*0.0078125,0.0078125);
  float span=float(u_aa_factor)*2.0;
  dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+reduce),vec2(-span),vec2(span))*pixel;
  vec3 a=0.5*(texture(u_tex,uv+dir/6.0).rgb+texture(u_tex,uv-dir/6.0).rgb);
  vec3 b=0.5*a+0.25*(texture(u_tex,uv+dir*0.5).rgb+texture(u_tex,uv-dir*0.5).rgb);
  float lo=min(l,min(min(n,s),min(e,w))),hi=max(l,max(max(n,s),max(e,w)));
  float lb=aa_luma(b);return vec4(lb<lo||lb>hi?a:b,1);
 }
/* Morphological edge detection, directional search and blend weights.
 * An analytic 1x area weight is used instead of a precomputed area LUT. */
 bool horizontal=abs(n-s)>abs(e-w);
 vec2 axis=horizontal?vec2(pixel.x,0):vec2(0,pixel.y);
 vec2 across=horizontal?vec2(0,pixel.y):vec2(pixel.x,0);
 float edge=max(abs(l-(horizontal?n:w)),abs(l-(horizontal?s:e)));
 /* A zero-length side makes the final blend weight exactly zero. Probe
  * both immediate neighbours before searching either long edge, so an
  * isolated end never pays for the opposite search or the blend sample. */
  if(u_aa_factor<1)return vec4(c,1);
  vec2 first_left=uv-axis,first_right=uv+axis;
  if(abs(aa_luma(texture(u_tex,first_left+across).rgb)-aa_luma(texture(u_tex,first_left-across).rgb))<edge*0.5)return vec4(c,1);
  if(abs(aa_luma(texture(u_tex,first_right+across).rgb)-aa_luma(texture(u_tex,first_right-across).rgb))<edge*0.5)return vec4(c,1);
  float left=1.0,right=1.0;
  for(int i=2;i<=32;++i){if(i>u_aa_factor*2)break;
  vec2 p=uv-axis*float(i);
  if(abs(aa_luma(texture(u_tex,p+across).rgb)-aa_luma(texture(u_tex,p-across).rgb))<edge*0.5)break;
  left=float(i);
 }
  for(int i=2;i<=32;++i){if(i>u_aa_factor*2)break;
  vec2 p=uv+axis*float(i);
  if(abs(aa_luma(texture(u_tex,p+across).rgb)-aa_luma(texture(u_tex,p-across).rgb))<edge*0.5)break;
  right=float(i);
 }
 float weight=clamp(0.5*min(left,right)/(left+right+1.0),0.0,0.45);
 vec2 shift=across*(horizontal?(abs(l-n)>abs(l-s)?1.0:-1.0):(abs(l-e)>abs(l-w)?1.0:-1.0));
 return vec4(mix(c,texture(u_tex,uv+shift).rgb,weight),1);
}
/* Catmull-Rom bicubic via 9 bilinear taps. Sharper than plain bilinear at
 * the same smoothness, with mild overshoot that reads as edge definition.
 * The present texture holds exactly the source rect and wraps CLAMP_TO_EDGE,
 * so the +/-2 texel footprint clamps to real edge pixels, never garbage. */
vec4 bicubic(vec2 uv){
  vec2 sp = uv * u_tex_size;
  vec2 t1 = floor(sp - 0.5) + 0.5;
  vec2 f  = sp - t1;
  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  vec2 w3 = f * f * (-0.5 + 0.5 * f);
  vec2 w12 = w1 + w2;
  vec2 t0 = (t1 - 1.0) / u_tex_size;
  vec2 t3 = (t1 + 2.0) / u_tex_size;
  vec2 t12 = (t1 + w2 / w12) / u_tex_size;
  vec4 r = vec4(0.0);
  r += texture(u_tex, vec2(t0.x , t0.y )) * (w0.x  * w0.y );
  r += texture(u_tex, vec2(t12.x, t0.y )) * (w12.x * w0.y );
  r += texture(u_tex, vec2(t3.x , t0.y )) * (w3.x  * w0.y );
  r += texture(u_tex, vec2(t0.x , t12.y)) * (w0.x  * w12.y);
  r += texture(u_tex, vec2(t12.x, t12.y)) * (w12.x * w12.y);
  r += texture(u_tex, vec2(t3.x , t12.y)) * (w3.x  * w12.y);
  r += texture(u_tex, vec2(t0.x , t3.y )) * (w0.x  * w3.y );
  r += texture(u_tex, vec2(t12.x, t3.y )) * (w12.x * w3.y );
  r += texture(u_tex, vec2(t3.x , t3.y )) * (w3.x  * w3.y );
  return r;
}
#include "scanline.glsl"
#ifdef NATIVE_SMAA_EDGES
/* Cache colour-edge measurements once per output pixel. Searches in the
 * resolve pass read compact edge data instead of repeatedly filtering and
 * evaluating source RGB. All modes retain the original UI exclusion mask. */
void main(){
 vec2 pixel=fwidth(v_uv);vec3 c=texture(u_tex,v_uv).rgb;float l=aa_luma(c);
 float n=aa_luma(texture(u_tex,v_uv+vec2(0,pixel.y)).rgb);
 float s=aa_luma(texture(u_tex,v_uv-vec2(0,pixel.y)).rgb);
 float e=aa_luma(texture(u_tex,v_uv+vec2(pixel.x,0)).rgb);
 float w=aa_luma(texture(u_tex,v_uv-vec2(pixel.x,0)).rgb);
 float vertical=abs(n-s),horizontal=abs(e-w);
 bool along_x=vertical>horizontal;
 float edge=max(abs(l-(along_x?n:w)),abs(l-(along_x?s:e)));
 float contrast=max(max(abs(l-n),abs(l-s)),max(abs(l-e),abs(l-w)));
 float direction=along_x?(abs(l-n)>abs(l-s)?1.0:-1.0):(abs(l-e)>abs(l-w)?1.0:-1.0);
 if(contrast<max(0.025,0.125*max(max(n,s),max(e,w))))direction=0.0;
 if(aa_guard(v_uv))direction=2.0;
 frag=vec4(vertical,horizontal,edge,direction);
}
#else
void main(){
  vec2 uv = v_uv;
#ifdef NATIVE_SMAA_CACHED
  bool protected_pixel=u_aa_mode==2?texelFetch(u_aa_history,ivec2(gl_FragCoord.xy),0).a==2.0:u_aa_mode!=0&&aa_guard(v_uv);
#else
  bool protected_pixel=u_aa_mode!=0&&aa_guard(v_uv);
#endif
  if(u_aa_mode==3&&!protected_pixel)uv+=u_aa_jitter*fwidth(v_uv);
  vec4 c;
  if(protected_pixel){ivec2 q=ivec2(clamp(v_uv,vec2(0),vec2(0.999999))*vec2(textureSize(u_tex,0)));
    c=texelFetch(u_tex,q,0);}
  else if (u_sharp == 2) { c = bicubic(uv); }
  else {
    if (u_sharp == 1) {
      vec2 scale = max(u_sharp_scale, vec2(1.0));
      vec2 texel = uv * u_tex_size;
      vec2 tf    = floor(texel);
      vec2 cd    = (texel - tf) - 0.5;
      vec2 band  = 0.5 - 0.5 / scale;
      vec2 f     = (cd - clamp(cd, -band, band)) * scale + 0.5;
      uv = (tf + f) / u_tex_size;
      vec2 lo = min(u_uv_rect.xy, u_uv_rect.zw);
      vec2 hi = max(u_uv_rect.xy, u_uv_rect.zw);
      uv = clamp(uv, lo, hi);
    }
    c = aa_spatial(uv);
  }
  if (u_screenlut_on == 1) {
    ivec3 q = ivec3(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5) >> 3;
    int idx = (q.b << 10) | (q.g << 5) | q.r;
    c.rgb = texelFetch(u_screenlut, ivec2(idx & 255, idx >> 8), 0).rgb;
  }
  c.rgb = psx_scanline(c.rgb, v_uv.y);
  if (u_gamma > 0.0 && u_gamma != 1.0) c.rgb = pow(max(c.rgb, vec3(0.0)), vec3(1.0 / u_gamma));
  if(u_aa_mode==3 && !protected_pixel && u_aa_history_valid!=0){
    vec2 h_uv=gl_FragCoord.xy/u_aa_drawable;
    vec3 old=texture(u_aa_history,h_uv).rgb;
    vec3 diff=abs(old-c.rgb); float change=max(diff.r,max(diff.g,diff.b));
    float keep=1.0-1.0/float(u_aa_factor);
    c.rgb=mix(c.rgb,old,keep*(1.0-smoothstep(0.025,0.15,change)));
  }
  frag = c;
#ifdef NATIVE_PRESENT_SCENE_MASK
  /* The final window resolve uses this classification to keep guest UI
   * nearest-sampled while scaling antialiased 3D colours smoothly. */
  frag.a=protected_pixel?1.0:0.0;
#endif
}
#endif
