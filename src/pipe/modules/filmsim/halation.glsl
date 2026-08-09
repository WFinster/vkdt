// halation.glsl: combining blurred exposure bounces.

vec3 combine_halation(ivec2 ipos, sampler2D exposure,
    sampler2D hal0, sampler2D hal1, sampler2D hal2, sampler2D hal3, sampler2D hal4, sampler2D hal5)
{
  ivec2 out_sz = imageSize(img_out);
  vec2 tc = (ipos + 0.5) / vec2(out_sz);
  vec3 raw = max(fetch_upsampled(exposure, ipos, tc), vec3(0.0));
  int n_bounces = prep.halation.n_bounces;
  float rho = prep.halation.rho, w = 1.0;
  vec3 hal = w * max(fetch_bilinear(hal0, ipos, tc), vec3(0.0)); w *= rho;
  if(n_bounces > 1) { hal += w * max(fetch_bilinear(hal1, ipos, tc), vec3(0.0)); w *= rho; }
  if(n_bounces > 2) { hal += w * max(fetch_bilinear(hal2, ipos, tc), vec3(0.0)); w *= rho; }
  if(n_bounces > 3) { hal += w * max(fetch_bilinear(hal3, ipos, tc), vec3(0.0)); w *= rho; }
  if(n_bounces > 4) { hal += w * max(fetch_bilinear(hal4, ipos, tc), vec3(0.0)); w *= rho; }
  if(n_bounces > 5) hal += w * max(fetch_bilinear(hal5, ipos, tc), vec3(0.0));
  hal *= prep.halation.inv_weight_sum;
  const float midtone_falloff = 0.01, midtone_gain = 3.0;
  float x = max(0.0, (hal.r + hal.g + hal.b) * prep.halation.inv_strength_sum);
  vec3 hs = prep.halation.hs_base * exp2(-midtone_gain * log2_e * params.halation_midtones /
      (1e-3 + midtone_falloff * x * x));
  return log2(max((raw + hs * hal) / (1.0 + hs), vec3(1e-10))) * log10_2;
}
