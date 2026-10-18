// Scanline post-process, shared by the present and interpolation shaders.
// Darkens toward the gap between PS1 scanlines with a soft sinusoidal beam
// centred on each line. The pitch is u_scanline_lines (the number of native
// display lines across the source rect), so it tracks the PS1 line grid, not
// the internal SSAA scale or the window size. u_scanline_scale is output pixels
// per PS1 line; the gate fades the effect in from 1x to 2x so it never shimmers
// on a window too small to resolve one line as two rows (Nyquist). Computed
// against v_uv.y (position across the display region) so it is orientation- and
// scale-agnostic. Requires the PSX_SCANLINE_UNIFORMS to be declared.
vec3 psx_scanline(vec3 rgb, float vy){
  if (u_scanline == 0) return rgb;
  float gate = clamp(u_scanline_scale - 1.0, 0.0, 1.0);
  if (gate <= 0.0) return rgb;
  float p    = fract(vy * u_scanline_lines);
  float beam = sin(3.14159265 * p);
  float mult = 1.0 - (u_scanline_strength * gate) * (1.0 - beam);
  return rgb * mult;
}
