// develop.glsl: grain synthesis and film density development.

vec3 add_grain(ivec2 ipos, vec3 density)
{
  vec3 np = clamp((density + prep.grain.grain_dmin) * prep.grain.inv_density_range, 0.0, 1.0);
  vec3 p_dev_0 = clamp(np * prep.grain.inv_layer_fraction_0, 0.0, 1.0);
  vec3 p_dev_1 = clamp((np - prep.grain.layer_fraction_0) * prep.grain.inv_layer_fraction_1, 0.0, 1.0);
  vec3 p_dev_2 = clamp((np - prep.grain.layer_fraction_01) * prep.grain.inv_layer_fraction_2, 0.0, 1.0);
  vec3 var_0 = p_dev_0 * max(1.0 - p_dev_0 * prep.grain.uniformity, 0.0);
  vec3 var_1 = p_dev_1 * max(1.0 - p_dev_1 * prep.grain.uniformity, 0.0);
  vec3 var_2 = p_dev_2 * max(1.0 - p_dev_2 * prep.grain.uniformity, 0.0);
  vec3 var_comb = prep.grain.variance_weight_fast * var_0 + prep.grain.variance_weight_mid * var_1 + prep.grain.variance_weight_slow * var_2;
  if(all(lessThan(var_comb, vec3(1e-10)))) return density;
  vec3 std_comb = sqrt(max(var_comb, vec3(0.0)));
  vec2 pos = vec2(ipos) * prep.grain.lattice_scale + prep.grain.lattice_seed;
  vec2 p = floor(pos);
  vec2 f = fract(pos);
  ivec2 ip = ivec2(p);
  vec3 acc = vec3(0.0);
  float wsq = 0.0;
  int particles_per_cell = prep.grain.particles_per_cell;
  uint random_stream = prep.grain.random_stream;
  [[loop]] for (int y = -1; y <= 2; y++) {
    float dy_grid = float(y) - f.y;
    float dy_grid_sq = dy_grid * dy_grid;
    [[loop]] for (int x = -1; x <= 2; x++) {
      float dx_grid = float(x) - f.x;
      float dist_grid_sq = dx_grid * dx_grid + dy_grid_sq;
      float w_win = max(0.0, 1.0 - dist_grid_sq * 0.25);
      float window = w_win * w_win;
      if (window <= 0.0) continue;
      ivec2 cell = ip + ivec2(x, y);
      vec2 d_grid = vec2(dx_grid, dy_grid);
      vec3 cell_acc = vec3(0.0);
      float cell_wsq = 0.0;
      [[unroll]] for (int k = 0; k < 12; k++) {
        if (k >= particles_per_cell) break;
        uvec3 h = pcg3d(uvec3(uvec2(cell), random_stream ^ uint(k)));
        vec2 jitter = vec2(float(h.x & 0xFFFu), float((h.x >> 12u) & 0xFFFu)) * (1.0 / 4096.0) - 0.5;
        vec3 n = vec3(float(h.y & 0xFFu), float((h.y >> 8u) & 0xFFu), float((h.y >> 16u) & 0xFFu)) * (2.0 / 255.0) - 1.0;
        vec2 d = d_grid + jitter;
        float w_part = exp2(-dot(d, d) * prep.grain.noise_kernel_scale);
        cell_acc += n * w_part;
        cell_wsq += w_part * w_part;
      }
      acc += cell_acc * window;
      wsq += cell_wsq * (window * window);
    }
  }
  vec3 final_noise = acc * inversesqrt(max(wsq, 1e-6)) * std_comb;
  return max(density + final_noise, prep.grain.grain_dmin * -1.0);
}

vec3 develop_density(vec3 y, ivec2 ipos)
{
  vec3 density_cmy = eval_film_density(y);
  density_cmy = mix(density_cmy, vec3(0.0), isnan(density_cmy));
  [[branch]] if(params.grain > 0) density_cmy = add_grain(ipos, density_cmy);
  return density_cmy;
}

vec3 develop_film_nowarp(vec3 log_raw, ivec2 ipos)
{
  return develop_density(clamp(log_raw, vec3(-4.0), vec3(4.0)), ipos);
}

vec3 develop_film(vec3 log_raw, ivec2 ipos, sampler2D curvewarp)
{
  log_raw = clamp(log_raw, vec3(-4.0), vec3(4.0));
  vec3 y = log_raw;
  if (params.couplers > 0)
  {
    vec3 tcx = logexp_to_tc(log_raw);
    y = vec3(texture(curvewarp, vec2(tcx.r, 0.5)).r, texture(curvewarp, vec2(tcx.g, 0.5)).g, texture(curvewarp, vec2(tcx.b, 0.5)).b);
  }
  return develop_density(y, ipos);
}
