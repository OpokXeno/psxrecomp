// Virtual raster coordinates keep guest transfers and material state intact.
// Rasterization itself runs on the physical host pixel grid. Zero-initialized
// loose OpenGL uniforms retain the legacy identity mapping.
#ifdef NATIVE_GPU_INTERLOCK
#define NATIVE_RASTER u_raster
#define NATIVE_SOURCE_RASTER u_source_raster
#define NATIVE_RASTER_PAGES u_raster_pages
#else
#define NATIVE_RASTER raster
#define NATIVE_SOURCE_RASTER source_raster
#define NATIVE_RASTER_PAGES raster_pages
#endif
vec4 native_raster_transform(){
 return NATIVE_RASTER.x>0.0&&NATIVE_RASTER.y>0.0?NATIVE_RASTER:vec4(1.0,1.0,0.0,0.0);
}
vec2 native_raster_position(vec2 p){
 vec4 r=native_raster_transform();
 return (p-r.zw-vec2(0.5))*r.xy+vec2(0.5)+vec2(0.0,NATIVE_RASTER_PAGES.x);
}
vec2 native_raster_delta(vec2 p,vec2 origin){
 vec4 r=native_raster_transform();
 return (p-r.zw-vec2(0.5))*r.xy-(origin-vec2(0.0,NATIVE_RASTER_PAGES.x));
}
vec2 native_source_uv(vec2 uv){
 vec4 r=NATIVE_SOURCE_RASTER;
 if(r.x<=0.0||r.y<=0.0)return uv;
 vec2 mapped=uv*r.xy+r.zw;
 mapped.y+=floor(uv.y*NATIVE_RASTER_PAGES.z)*NATIVE_RASTER_PAGES.w;
 return mapped;
}
vec2 native_transfer_uv(vec2 uv,vec2 extent){
 vec2 mapped=native_source_uv(uv);
 // Point copies sample texel centres. Resolve boundary ties consistently
 // when the interpolator evaluates the same rectangle at another origin.
 if(NATIVE_RASTER_PAGES.y>0.0||NATIVE_RASTER_PAGES.z>0.0)
  mapped=(floor(mapped*extent+vec2(1.0/256.0))+vec2(0.5))/extent;
 return mapped;
}
