/*
 * render.c - Public render API for libdtpipe
 *
 * Implements:
 *   dtpipe_render()         - render the full image at a given scale
 *   dtpipe_render_region()  - render a sub-rectangle at a given scale
 *   dtpipe_free_render()    - free a dt_render_result_t
 *
 * Pipeline overview
 * ─────────────────
 * The raw pixel buffer in dt_image_t is a Bayer mosaic stored as uint16_t
 * (or float).  The pixelpipe engine works with float-RGBA (4 floats/pixel).
 *
 * Until the IOP modules (rawprepare, demosaic, …) are compiled in we bridge
 * the gap with a minimal unpack step:
 *   - Expand each raw sample to float in [0,1] (divide by white_point).
 *   - Place it in all four RGBA channels so the image is at least visible.
 *
 * Once the IOP modules are registered, rawprepare + demosaic will replace
 * this stub and produce a proper de-mosaiced float-RGBA buffer.
 *
 * After the pipeline runs, the float-RGBA result in pipe->backbuf is
 * converted to 8-bit sRGB RGBA for the caller.
 *
 * sRGB gamma encoding
 * ───────────────────
 * The standard piecewise sRGB transfer function is applied per channel:
 *   if (linear <= 0.0031308)  out = linear * 12.92
 *   else                      out = 1.055 * pow(linear, 1/2.4) - 0.055
 * Values are clamped to [0, 1] before encoding.
 */

#include "pipe/create.h"    /* dt_pipe_t, _module_node_t, dtpipe_find_module */
#include "pipe/pixelpipe.h" /* dt_dev_pixelpipe_process, dt_dev_pixelpipe_set_input */
#include "pipe/render.h"    /* dtpipe_ensure_input_buf declaration */
#include "dtpipe.h"
#include "dtpipe_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── sRGB gamma ──────────────────────────────────────────────────────────── */

static inline float _srgb_gamma(float x)
{
  if(x <= 0.0f)          return 0.0f;
  if(x >= 1.0f)          return 1.0f;
  if(x <= 0.0031308f)    return x * 12.92f;
  return 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
}

/* ── _ensure_input_buf ───────────────────────────────────────────────────── */
/*
 * Build (or reuse) a float-RGBA input buffer from the raw Bayer mosaic.
 *
 * If no IOP modules are registered the buffer is synthesised by a simple
 * uint16→float unpack so the pipeline has something to work with.
 * The buffer is cached in pipe->input_buf and reused on subsequent renders.
 *
 * Returns true on success, false on allocation failure.
 */
bool dtpipe_ensure_input_buf(dt_pipe_t *pipe)
{
  if(pipe->input_buf)
    return true; /* already built */

  dt_image_t *img = pipe->img;
  const int W = pipe->input_width;
  const int H = pipe->input_height;

  if(W <= 0 || H <= 0 || !img->pixels)
  {
    fprintf(stderr, "[dtpipe/render] image has no pixel data\n");
    return false;
  }

  /*
   * For RAW images the pipeline expects a 1-channel float Bayer buffer as
   * input (rawprepare subtracts black levels, demosaic interpolates to RGB).
   * For non-RAW (JPEG/TIFF) we use 4-channel float RGBA.
   *
   * Raw buffer formats:
   *   bpp == 2  →  uint16_t Bayer samples, normalise by raw_white_point
   *   bpp == 4  →  float Bayer samples (e.g. from some DNG decoders)
   */
  const gboolean is_raw = dt_image_is_raw(img);
  const int channels = is_raw ? 1 : 4;

  float *buf = dt_alloc_align_float((size_t)W * H * channels);
  if(!buf)
  {
    fprintf(stderr, "[dtpipe/render] out of memory allocating input buffer\n");
    return false;
  }

  if(is_raw)
  {
    /* 1-channel Bayer: copy directly into a float buffer (no colour replication) */
    const float wp = img->raw_white_point > 0
                       ? (float)img->raw_white_point
                       : 65535.0f;
    if(img->bpp == 2)
    {
      const uint16_t *src = (const uint16_t *)img->pixels;
      DT_OMP_FOR()
      for(int i = 0; i < W * H; i++)
        buf[i] = (float)src[i] / wp;
    }
    else /* float raw */
    {
      const float *src = (const float *)img->pixels;
      DT_OMP_FOR()
      for(int i = 0; i < W * H; i++)
        buf[i] = src[i];
    }
  }
  else
  {
    /* Non-RAW: replicate single channel to RGBA for passthrough */
    const float *src = (const float *)img->pixels;
    DT_OMP_FOR()
    for(int i = 0; i < W * H; i++)
    {
      const float v = src ? src[i] : 0.0f;
      buf[4 * i + 0] = v;
      buf[4 * i + 1] = v;
      buf[4 * i + 2] = v;
      buf[4 * i + 3] = 1.0f;
    }
  }

  pipe->input_buf = buf;
  return true;
}

/* ── _float_to_u8_rgba ───────────────────────────────────────────────────── */
/*
 * Convert a float-RGBA buffer (W×H×4 floats) to 8-bit sRGB RGBA.
 * Returns a freshly malloc'd uint8_t buffer, or NULL on OOM.
 */
static uint8_t *_float_to_u8_rgba(const float *src, int W, int H)
{
  uint8_t *dst = (uint8_t *)malloc((size_t)W * H * 4);
  if(!dst) return NULL;

  DT_OMP_FOR()
  for(int i = 0; i < W * H; i++)
  {
    dst[4 * i + 0] = (uint8_t)(_srgb_gamma(src[4 * i + 0]) * 255.0f + 0.5f);
    dst[4 * i + 1] = (uint8_t)(_srgb_gamma(src[4 * i + 1]) * 255.0f + 0.5f);
    dst[4 * i + 2] = (uint8_t)(_srgb_gamma(src[4 * i + 2]) * 255.0f + 0.5f);
    dst[4 * i + 3] = (uint8_t)(CLAMPS(src[4 * i + 3], 0.0f, 1.0f) * 255.0f + 0.5f);
  }

  return dst;
}

/* ── _do_render ──────────────────────────────────────────────────────────── */
/*
 * Internal helper: run the pipeline over the given ROI and return a
 * dt_render_result_t.  Called by both dtpipe_render() and
 * dtpipe_render_region().
 *
 * @param pipe       Public pipeline handle.
 * @param x, y       Top-left corner of the ROI in full-resolution coords.
 * @param w, h       Dimensions of the ROI in full-resolution pixels.
 * @param scale      Output scale (1.0 = 1:1 mapping of the ROI).
 */
static dt_render_result_t *_do_render(dt_pipe_t *pipe,
                                      int x, int y, int w, int h,
                                      float scale)
{
  if(!pipe || scale <= 0.0f)
  {
    fprintf(stderr, "[dtpipe/render] invalid arguments\n");
    return NULL;
  }

  /* Ensure float-RGBA input buffer exists */
  if(!dtpipe_ensure_input_buf(pipe))
    return NULL;

  /* Reset pipe->pipe.dsc to the initial image format before each render.
     Format-changing modules (rawprepare, demosaic) mutate pipe->pipe.dsc
     in-place during processing.  Without this reset, the second render
     would start with the post-pipeline format (4-ch RGB) as its input
     format, causing rawprepare to skip its branch and demosaic to receive
     the wrong descriptor. */
  pipe->pipe.dsc = pipe->initial_dsc;

  /* Attach input to the underlying pixelpipe */
  const int full_w = pipe->input_width;
  const int full_h = pipe->input_height;

  dt_dev_pixelpipe_set_input(&pipe->pipe,
                             pipe->input_buf,
                             full_w, full_h,
                             1.0f,      /* iscale: input is full resolution */
                             pipe->img);

  /* Output dimensions */
  const int out_w = (int)((float)w * scale);
  const int out_h = (int)((float)h * scale);

  if(out_w <= 0 || out_h <= 0)
  {
    fprintf(stderr, "[dtpipe/render] output dimensions are zero\n");
    return NULL;
  }

  /* Run the pipeline */
  const bool err = dt_dev_pixelpipe_process(&pipe->pipe,
                                            x, y, out_w, out_h, scale);
  if(err)
  {
    fprintf(stderr, "[dtpipe/render] pipeline processing failed\n");
    return NULL;
  }

  /* pipeline result is float-RGBA in pipe->pipe.backbuf */
  const float *fbuf = (const float *)pipe->pipe.backbuf;
  if(!fbuf)
  {
    fprintf(stderr, "[dtpipe/render] pipeline produced no output\n");
    return NULL;
  }

  /* Convert to 8-bit sRGB RGBA */
  uint8_t *pixels = _float_to_u8_rgba(fbuf, out_w, out_h);
  if(!pixels)
  {
    fprintf(stderr, "[dtpipe/render] out of memory converting to 8-bit\n");
    return NULL;
  }

  /* Wrap in result struct */
  dt_render_result_t *result =
    (dt_render_result_t *)malloc(sizeof(dt_render_result_t));
  if(!result)
  {
    free(pixels);
    return NULL;
  }

  result->pixels = pixels;
  result->width  = out_w;
  result->height = out_h;
  result->stride = out_w * 4; /* 4 bytes per pixel, no padding */

  return result;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

dt_render_result_t *dtpipe_render(dt_pipe_t *pipe, float scale)
{
  if(!pipe)
    return NULL;

  return _do_render(pipe,
                    0, 0,                          /* full image origin */
                    pipe->input_width,
                    pipe->input_height,
                    scale);
}

dt_render_result_t *dtpipe_render_region(dt_pipe_t *pipe,
                                         int x, int y, int w, int h,
                                         float scale)
{
  if(!pipe)
    return NULL;
  if(w <= 0 || h <= 0 || scale <= 0.0f)
    return NULL;

  return _do_render(pipe, x, y, w, h, scale);
}

void dtpipe_free_render(dt_render_result_t *result)
{
  if(!result)
    return;
  free(result->pixels);
  result->pixels = NULL;
  free(result);
}
