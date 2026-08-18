#include "modules/api.h"

#include <jxl/encode.h>
#include <jxl/resizable_parallel_runner.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Mostly copying from darktable (and thus GPLv3), but also  o-jpg, o-exr and o-pfm.
// Cobbled together by me, with very limited C or programming knowledge (but not an LLM!).
// Currently very basic. And bad.

JxlEncoderStatus JxlAssert(JxlEncoderStatus code,
                           JxlEncoder *encoder,
                           int line)
{
  if(code != JXL_ENC_SUCCESS)
  {
    JxlEncoderError error = JxlEncoderGetError(encoder);
    fprintf(stderr, "[o-jxl] libjxl call failed with err %d (src/pipe/modules/o-jxl/main.c#L%d)\n", error, line);
  }
  return code;
}

void write_sink(
    dt_module_t            *module,
    void                   *buf,
    dt_write_sink_params_t *p)

{
  const char *basename = dt_module_param_string(module, 0);
  fprintf(stderr, "[o-jxl] writing '%s'\n", basename);
  const uint16_t *p16 = buf;
  
  const size_t width  = module->connector[0].roi.wd;
  const size_t height = module->connector[0].roi.ht;

  const dt_colour_primaries_t primaries =  module->img_param.colour_primaries;
  const dt_colour_trc_t trc = module->img_param.colour_trc;
  
  char filename[512];
  snprintf(filename, sizeof(filename), "%s.jxl", basename);



  JxlEncoder *encoder = JxlEncoderCreate(NULL);

  const unsigned num_threads = JxlResizableParallelRunnerSuggestThreads(width, height);
  void *runner = JxlResizableParallelRunnerCreate(NULL);
  JxlResizableParallelRunnerSetThreads(runner, num_threads);
  JxlAssert(JxlEncoderSetParallelRunner(encoder, JxlResizableParallelRunner, runner), encoder, __LINE__);

  // Automatically freed when we destroy the encoder
  JxlEncoderFrameSettings *frame_settings = JxlEncoderFrameSettingsCreate(encoder, NULL);

  // Set encoder basic info, just f16 for now
  JxlBasicInfo basic_info;
  JxlEncoderInitBasicInfo(&basic_info);
  basic_info.xsize = width;
  basic_info.ysize = height;
  basic_info.bits_per_sample = 16;
  basic_info.exponent_bits_per_sample = 5;

  const float quality = dt_module_param_float(module, 1)[0];
  // JXL natively uses ‘distance’ a [0:25] value. This aims to estimate a distance
  // roughly equivalent to what would be obtained with libjpeg-turbo with the same quality parameter.
  const float distance = JxlEncoderDistanceFromQuality(quality);
  JxlAssert(JxlEncoderSetFrameDistance(frame_settings, distance), encoder, __LINE__);
  if(quality == 100)
  {
    // HAVE NOT DONE LOSSLESS
  }

  // Don’t know how to create GUI sliders, so just setting the default effort of 7.
  JxlAssert(JxlEncoderFrameSettingsSetOption(frame_settings, JXL_ENC_FRAME_SETTING_EFFORT, 7), encoder, __LINE__);

  // Codestream level should be chosen automatically given the settings
  JxlAssert(JxlEncoderSetBasicInfo(encoder, &basic_info), encoder, __LINE__);



  // Also just hard coding for now
  JxlColorEncoding colour_encoding;

  colour_encoding.color_space = JXL_COLOR_SPACE_RGB;
  colour_encoding.white_point = JXL_WHITE_POINT_D65;

  JxlPrimaries nativePrimaries = 0;
  switch(primaries)
  {
    case s_colour_primaries_srgb:   nativePrimaries = JXL_PRIMARIES_SRGB;
                                    break;
    case s_colour_primaries_P3:     nativePrimaries = JXL_PRIMARIES_P3;
                                    break;
    case s_colour_primaries_2020:   nativePrimaries = JXL_PRIMARIES_2100;
                                    break;
    default:                        nativePrimaries = JXL_PRIMARIES_CUSTOM;
    // Then set these? Or ICC?      colour_encoding.primaries_red_xy   = double;
    //                              colour_encoding.primaries_green_xy = double;
    //                              colour_encoding.primaries_blue_xy  = double;
  }
  colour_encoding.primaries = nativePrimaries;

  JxlTransferFunction nativeTRC = 0;
  switch(trc)
  {
    case s_colour_trc_linear:       nativeTRC = JXL_TRANSFER_FUNCTION_LINEAR;
                                    break;
    case s_colour_trc_709:          nativeTRC = JXL_TRANSFER_FUNCTION_709;
                                    break;
    case s_colour_trc_srgb:         nativeTRC = JXL_TRANSFER_FUNCTION_SRGB;
                                    break;
    case s_colour_trc_PQ:           nativeTRC = JXL_TRANSFER_FUNCTION_PQ;
                                    break;
    case s_colour_trc_DCI:          nativeTRC = JXL_TRANSFER_FUNCTION_DCI;
                                    break;
    case s_colour_trc_HLG:          nativeTRC = JXL_TRANSFER_FUNCTION_HLG;
                                    break;
    case s_colour_trc_gamma:        nativeTRC = JXL_TRANSFER_FUNCTION_GAMMA;
                                    // Then set gamma value. But I think s_colour_trc_gamma is only 2.2.
                                    colour_encoding.gamma = 2.2;
                                    break;
    default:                        nativeTRC = JXL_TRANSFER_FUNCTION_UNKNOWN;
  }
  colour_encoding.transfer_function = nativeTRC;

  colour_encoding.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
  JxlAssert(JxlEncoderSetColorEncoding(encoder, &colour_encoding), encoder, __LINE__);



  JxlPixelFormat pixel_format = { 3, JXL_TYPE_FLOAT16, JXL_NATIVE_ENDIAN, 0 };

  // Fix pixel stride
  const size_t pixels_size = width * height * 3 * sizeof(uint16_t);
  uint16_t *pixels = malloc(pixels_size);
  if(!pixels)
    fprintf(stderr, "could not allocate output pixel buffer of size %zu", pixels_size);

  for(size_t y = 0; y < height; ++y)
  {
    for(size_t x = 0; x < width; ++x)
    {
      const uint16_t *in_pixel = p16 + 4 * ((y * width) + x);
      uint16_t *out_pixel = pixels + 3 * ((y * width) + x);

      out_pixel[0] = in_pixel[0];
      out_pixel[1] = in_pixel[1];
      out_pixel[2] = in_pixel[2];
    }
  }

  printf("pixels are at %p and are %zu long.\n", pixels, pixels_size);
  JxlAssert(JxlEncoderAddImageFrame(frame_settings, &pixel_format, pixels, pixels_size), encoder, __LINE__);

  // No more image frames nor metadata boxes to add
  JxlEncoderCloseInput(encoder);

  // Write the image codestream to a buffer, starting with a chunk of 64 KiB.
  // TODO: Can we better estimate what the optimal size of chunks is for this image?
  size_t chunk_size = 1 << 16;
  size_t out_len = chunk_size;
  printf("out_len = %zu\n", out_len);
  uint8_t *out_buf = malloc(out_len);
  printf("out_buf = %p\n", out_buf);
  if(!out_buf) printf("could not allocate codestream buffer of size %zu", out_len);
  uint8_t *out_cur = out_buf;
  size_t out_avail = out_len;

  JxlEncoderStatus out_status = JXL_ENC_NEED_MORE_OUTPUT;
  while(out_status == JXL_ENC_NEED_MORE_OUTPUT) {
    out_status = JxlEncoderProcessOutput(encoder, &out_cur, &out_avail);
    if(out_status == JXL_ENC_SUCCESS) printf("out_status = JXL_ENC_SUCCESS\n");
    if(out_status == JXL_ENC_ERROR) printf("out_status = JXL_ENC_ERROR\n");
    if(out_status == JXL_ENC_NEED_MORE_OUTPUT) printf("out_status = JXL_ENC_NEED_MORE_OUTPUT\n");
    if(out_status == JXL_ENC_NEED_MORE_OUTPUT) {
      printf("Ok?\n");
      const size_t offset = out_cur - out_buf;
      printf("offset = %zu\n", offset);
      if(chunk_size < 1 << 20)
        chunk_size *= 2;

      printf("out_buf = %p\n", out_buf);
      out_len += chunk_size;
      out_buf = realloc(out_buf, out_len);
      out_cur = out_buf + offset;
      out_avail = out_len - offset;

    }
  }

  // Update actual length of codestream written
  out_len = out_cur - out_buf;

  // Write codestream contents to file
  printf("out_len = %zu\n", out_len);
  FILE *out_file = fopen(filename, "wb");
  if(fwrite(out_buf, sizeof(uint8_t), out_len, out_file) != out_len)
    printf("could not write bytes to `%s'", filename);

  if(runner)
    JxlResizableParallelRunnerDestroy(runner);
  if(encoder)
    JxlEncoderDestroy(encoder);
  if(out_file)
    fclose(out_file);
  free(pixels);
  free(out_buf);
}
