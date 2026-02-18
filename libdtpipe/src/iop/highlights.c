/*
 * highlights.c - Highlight reconstruction IOP for libdtpipe
 *
 * Extracted from darktable src/iop/highlights.c
 * Copyright (C) 2010-2025 darktable developers.
 * Adapted for libdtpipe: GUI, OpenCL, raster mask, and complex reconstruction
 * algorithms removed.  Only DT_IOP_HIGHLIGHTS_CLIP mode is implemented;
 * all other mode values fall through to clip.  This avoids dependencies on
 * box_filters, distance_transform, interpolation, noise_generator, and the
 * hlreconstruct/ helper files.
 *
 * All internal functions are static (Phase 8 convention for single dylib).
 *
 * Operates in IOP_CS_RAW (before demosaic).
 */

#include "dtpipe_internal.h"
#include "iop/iop_math.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── Mode enum (mirrors darktable's) ────────────────────────────────────── */

typedef enum dt_iop_highlights_mode_t
{
  DT_IOP_HIGHLIGHTS_CLIP     = 0,
  DT_IOP_HIGHLIGHTS_LCH      = 1,
  DT_IOP_HIGHLIGHTS_INPAINT  = 2,
  DT_IOP_HIGHLIGHTS_LAPLACIAN = 3,
  DT_IOP_HIGHLIGHTS_SEGMENTS = 4,
  DT_IOP_HIGHLIGHTS_OPPOSED  = 5,
} dt_iop_highlights_mode_t;

/* Magic clip factors per mode (from darktable source) */
static const float highlights_clip_magics[6] = {
  1.0f,   /* CLIP     */
  1.0f,   /* LCH      */
  0.987f, /* INPAINT  */
  0.995f, /* LAPLACIAN*/
  0.987f, /* SEGMENTS */
  0.987f, /* OPPOSED  */
};

/* ── Parameter / data structs (must match params.c descriptor layout) ────── */

typedef struct dt_iop_highlights_params_t
{
  int32_t mode;         /* reconstruction method     */
  float   blendL;       /* blend luminance (unused)  */
  float   blendC;       /* blend chroma   (unused)  */
  float   strength;     /* reconstruction strength   */
  float   clip;         /* clipping threshold        */
  float   noise_level;  /* noise level               */
  int32_t iterations;   /* guided laplacian iters    */
  int32_t scales;       /* wavelet scales            */
  float   candidating;  /* segmentation threshold    */
  float   combine;      /* combine segments          */
  int32_t recovery;     /* rebuild mode              */
  float   solid_color;  /* inpaint flat color        */
} dt_iop_highlights_params_t;

/* data is identical to params for highlights */
typedef dt_iop_highlights_params_t dt_iop_highlights_data_t;

/* ── process_clip: per-channel clamp in RAW colorspace ───────────────────── */

static void process_clip(dt_iop_module_t *self,
                          dt_dev_pixelpipe_iop_t *piece,
                          const void *const ivoid,
                          void *const ovoid,
                          const dt_iop_roi_t *const roi_in,
                          const dt_iop_roi_t *const roi_out,
                          const float clip)
{
  const float *const in  = (const float *const)ivoid;
  float *const       out = (float *const)ovoid;

  const uint32_t filters = piece->pipe->dsc.filters;
  const int ch = filters ? 1 : 4;

  if(ch == 4)
  {
    /* Non-mosaic (SRAW / linear raw): clamp all channels */
    const size_t msize = (size_t)roi_out->width * roi_out->height * ch;
    DT_OMP_FOR()
    for(size_t k = 0; k < msize; k++)
      out[k] = fminf(clip, in[k]);
  }
  else
  {
    /* Bayer / X-Trans: per-channel clamp with late-correction support */
    const uint8_t(*const xtrans)[6] =
        (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
    const gboolean is_xtrans = (filters == 9u);

    /* Apply late WB correction to per-channel clips if needed */
    const dt_dev_chroma_t *chr = NULL;
    if(self->dev) chr = &((dt_develop_t *)self->dev)->chroma;

    dt_aligned_pixel_t clips = { clip, clip, clip, clip };
    if(chr && chr->late_correction && chr->D65coeffs[0] > 0.0f)
    {
      for_each_channel(c)
        clips[c] *= chr->as_shot[c] / chr->D65coeffs[c];
    }

    DT_OMP_FOR()
    for(int row = 0; row < roi_out->height; row++)
    {
      for(int col = 0; col < roi_out->width; col++)
      {
        const size_t ox   = (size_t)row * roi_out->width + col;
        const int    irow = row + roi_out->y - roi_in->y;
        const int    icol = col + roi_out->x - roi_in->x;
        const size_t ix   = (size_t)irow * roi_in->width + icol;

        if((icol >= 0) && (irow >= 0)
           && (irow < roi_in->height) && (icol < roi_in->width))
        {
          const int c = is_xtrans
              ? FCxtrans(irow, icol, roi_in, xtrans)
              : FC(irow, icol, filters);
          out[ox] = fminf(in[ix], clips[c]);
        }
        else
          out[ox] = 0.0f;
      }
    }
  }
}

/* ── process() ───────────────────────────────────────────────────────────── */

static void process(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const void *const ivoid,
                     void *const ovoid,
                     const dt_iop_roi_t *const roi_in,
                     const dt_iop_roi_t *const roi_out)
{
  dt_dev_pixelpipe_t *pipe = piece->pipe;
  const uint32_t filters = pipe->dsc.filters;
  dt_iop_highlights_data_t *d = piece->data;

  const float clip = d->clip * dt_iop_get_processed_minimum(piece);

  /*
   * Phase A: only DT_IOP_HIGHLIGHTS_CLIP is implemented.
   * All other modes fall through to clip.  This avoids pulling in the
   * large hlreconstruct/ dependencies (opposed, laplacian, lch, segbased,
   * inpaint).  Those algorithms can be added in a future task.
   */
  if(filters == 0)
  {
    /* Non-mosaic raw (SRAW): simple clip */
    process_clip(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
    const float m = dt_iop_get_processed_minimum(piece);
    for_three_channels(k) pipe->dsc.processed_maximum[k] = m;
  }
  else
  {
    /* Bayer / X-Trans: per-channel clip */
    process_clip(self, piece, ivoid, ovoid, roi_in, roi_out, clip);

    /* Update processed_maximum for all non-laplacian / non-opposed modes */
    const float m = dt_iop_get_processed_maximum(piece);
    for_three_channels(k) pipe->dsc.processed_maximum[k] = m;
  }
}

/* ── commit_params() ─────────────────────────────────────────────────────── */

static void commit_params(dt_iop_module_t *self,
                           dt_iop_params_t *p1,
                           dt_dev_pixelpipe_t *pipe,
                           dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_highlights_params_t *p =
      (const dt_iop_highlights_params_t *)p1;
  dt_iop_highlights_data_t *d = piece->data;

  memcpy(d, p, sizeof(*p));

  /* For non-raw images always use clip */
  const dt_image_t *img = &pipe->image;
  if(!dt_image_is_rawprepare_supported(img))
    d->mode = DT_IOP_HIGHLIGHTS_CLIP;

  /* Phase A: always use clip — ignore all complex modes */
  d->mode = DT_IOP_HIGHLIGHTS_CLIP;
}

/* ── modify_roi_out() / modify_roi_in() ──────────────────────────────────── */

static void modify_roi_out(dt_iop_module_t *self,
                            dt_dev_pixelpipe_iop_t *piece,
                            dt_iop_roi_t *roi_out,
                            const dt_iop_roi_t *const roi_in)
{
  *roi_out   = *roi_in;
  roi_out->x = MAX(0, roi_in->x);
  roi_out->y = MAX(0, roi_in->y);
}

static void modify_roi_in(dt_iop_module_t *self,
                           dt_dev_pixelpipe_iop_t *piece,
                           const dt_iop_roi_t *const roi_out,
                           dt_iop_roi_t *roi_in)
{
  /* In clip mode the output ROI equals the input ROI — no extra border
     needed.  Complex reconstruction modes (opposed, laplacian) would
     require the full image here; leave that for a future task. */
  *roi_in = *roi_out;
}

/* ── init_pipe() / cleanup_pipe() ────────────────────────────────────────── */

static void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                       dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_highlights_data_t));
}

static void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

/* ── init() — default params ─────────────────────────────────────────────── */

static void init(dt_iop_module_t *self)
{
  dt_iop_highlights_params_t *d = self->default_params;
  if(!d) return;

  d->mode        = DT_IOP_HIGHLIGHTS_CLIP;
  d->blendL      = 1.0f;
  d->blendC      = 0.0f;
  d->strength    = 0.0f;
  d->clip        = 1.0f;
  d->noise_level = 0.0f;
  d->iterations  = 30;
  d->scales      = 6;   /* WAVELETS_7_SCALE */
  d->candidating = 0.4f;
  d->combine     = 2.0f;
  d->recovery    = 0;   /* DT_RECOVERY_MODE_OFF */
  d->solid_color = 0.0f;

  memcpy(self->params, d, sizeof(*d));
}

/* ── colorspace declarations ─────────────────────────────────────────────── */

static dt_iop_colorspace_type_t input_colorspace(dt_iop_module_t *self,
                                                   dt_dev_pixelpipe_t *pipe,
                                                   dt_dev_pixelpipe_iop_t *piece)
{
  /* Highlights operates on RAW data; fall back to RGB for non-raw input */
  if(pipe && pipe->dsc.filters == 0 && !dt_image_is_raw(&pipe->image))
    return IOP_CS_RGB;
  return IOP_CS_RAW;
}

static dt_iop_colorspace_type_t output_colorspace(dt_iop_module_t *self,
                                                    dt_dev_pixelpipe_t *pipe,
                                                    dt_dev_pixelpipe_iop_t *piece)
{
  return input_colorspace(self, pipe, piece);
}

/* ── Public init_global entry point ──────────────────────────────────────── */

void dt_iop_highlights_init_global(dt_iop_module_so_t *so)
{
  so->process_plain     = process;
  so->init              = init;
  so->init_pipe         = init_pipe;
  so->cleanup_pipe      = cleanup_pipe;
  so->commit_params     = commit_params;
  so->modify_roi_in     = modify_roi_in;
  so->modify_roi_out    = modify_roi_out;
  so->input_colorspace  = input_colorspace;
  so->output_colorspace = output_colorspace;
}
