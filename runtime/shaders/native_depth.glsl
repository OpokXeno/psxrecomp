#if __VERSION__ < 400 && defined(GL_ARB_gpu_shader5)
#extension GL_ARB_gpu_shader5 : enable
#endif
#if __VERSION__ >= 400 || defined(GL_ARB_gpu_shader5)
#define DEPTH_PRECISE precise
#else
#define DEPTH_PRECISE
#endif
// Positive FP32 depth, bit-preserved in R32UI images and capture nodes.
// Unsigned comparisons have the same ordering; 0 is far.
uint depth_biased(uint key,uint bias){
 float factor=bias!=0u?uintBitsToFloat(0x3f800000u+(0x00800000u>>bias)):1.0;
 DEPTH_PRECISE float tested=uintBitsToFloat(key)*factor;
 return floatBitsToUint(tested);
}
uint depth_threshold(uint stored){
 DEPTH_PRECISE float threshold=uintBitsToFloat(stored)*0.9990234375; // 1 - 2^-10
 return floatBitsToUint(threshold);
}

uint depth_evaluate(ivec4 bits,ivec2 delta){
 vec3 plane=intBitsToFloat(bits.xyz);
#if __VERSION__ >= 400 || defined(GL_ARB_gpu_shader5)
 precise float key=fma(plane.z,float(delta.y),fma(plane.y,float(delta.x),plane.x));
#else
 // GL 3.3 without ARB_gpu_shader5 retains FP32 depth with separate mul/add.
 float key=plane.z*float(delta.y)+(plane.y*float(delta.x)+plane.x);
#endif
 return floatBitsToUint(max(key,1.17549435e-38));
}

// Continuous host-grid sample; the integer entry point above remains exact
// for manual scales and the canonical PS1 raster.
uint depth_evaluate_at(ivec4 bits,vec2 delta){
 vec3 plane=intBitsToFloat(bits.xyz);
#if __VERSION__ >= 400 || defined(GL_ARB_gpu_shader5)
 precise float key=fma(plane.z,delta.y,fma(plane.y,delta.x,plane.x));
#else
 float key=plane.z*delta.y+(plane.y*delta.x+plane.x);
#endif
 return floatBitsToUint(max(key,1.17549435e-38));
}
