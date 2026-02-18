/*
 * rawprepare.c — darktable rawprepare IOP, ported for libdtpipe
 *
 * Extracted from darktable src/iop/rawprepare.c (GPLv3).
 * GUI code, OpenCL, GainMap, database calls, image-cache writes,
 * and reload_defaults() removed. Adapted to compile against
 * dtpipe_internal.h instead of darktable headers.
 *
 * What this module does:
 *   - Subtracts per-channel black levels from the raw Bayer sensor data.
 *   - Divides by (white_point - black) to normalise to [0, 1].
 *   - Adjusts the filter pattern offset for left/top sensor crop.
 *   - Trims the left/top/right/bottom sensor border pixels.
 *
 * Param struct layout MUST match _rawprepare_params_t in pipe/params.c:
 *   int32_t  left, top, right, bottom       (crop edges)
 *   uint16_t raw_black_level_separate[4]    (per-CFA black level)
 *   uint32_t raw_white_point                (white point)
 *   int32_t  flat_field                     (0=off, 1=embedded GainMap)
 *
 * Copyright (C) 2015-2025 darktable developers (GPLv3)
 */

#include "dtpipe_internal.h"
#include "iop/iop_math.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Parameter and data structs ─────────────────────────────────────────── */

/*
 * IMPORTANT: field order and types must exactly match _rawprepare_params_t in
 * pipe/params.c so that memcpy-based history load/save works correctly.
 */
typedef struct dt_iop_rawprepare_params_t
{
  int32_t  left;                       /* crop left   (pixels)  */
  int32_t  top;                        /* crop top    (pixels)  */
  int32_t  right;                      /* crop right  (pixels)  */
  int32_t  bottom;                     /* crop bottom (pixels)  */
  uint16_t raw_black_level_separate[4];/* per-CFA black level   */
  uint32_t raw_white_point;            /* white point           */
  int32_t  flat_field;                 /* 0=off, 1=embedded GainMap */
} dt_iop_rawprepare_params_t;

typedef struct dt_iop_rawprepare_data_t
{
  int32_t left, top, right, bottom;
  float sub[4];    /* per-channel black subtraction values */
  float div[4];    /* per-channel divisor (white - black) */

  /* cached for dt_iop_buffer_dsc_t::rawprepare */
  struct {
    uint16_t raw_black_level;
    uint16_t raw_white_point;
  } rawprepare;

  /* GainMap support removed — always FALSE */
} dt_iop_rawprepare_data_t;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/*
 * dt_rawspeed_crop_dcraw_filters: adjust the CFA filter pattern integer for a
 * crop offset. Returns the filter pattern shifted by (cx, cy) pixels.
 *
 * In the full darktable this is implemented in src/common/rawspeed_glue.c.
 * For a simple crop the adjustment is purely a bit-shift of the 32-bit Bayer
 * pattern quadrant. We implement it here so rawprepare.c compiles standalone.
 *
 * The 32-bit filter integer encodes a 2×2 or 4×4 Bayer tile; rotating by
 * (cx & 1, cy & 1) shifts the starting quadrant.
 */
static uint32_t _crop_dcraw_filters(uint32_t filters, int cx, int cy)
{
  if(filters == 0 || filters == 9u)
    return filters; /* X-Trans or no mosaic — not affected by crop */

  /* For a standard Bayer 2×2 pattern the effective rotation is:
   * rotate by (cx & 1) columns and (cy & 1) rows within the 2×2 tile.
   * Each column rotation shifts 2 bits; each row rotation shifts (4+cx&1) bits.
   * This is what darktable's sensordata.cpp does for RGGB/BGGR/etc. */
  const int shift = 2 * ((cy & 1) * 2 + (cx & 1));
  /* Rotate the 8-bit Bayer descriptor; the descriptor is replicated in all
   * 4 bytes of the 32-bit word in darktable's encoding. */
  uint8_t b = (uint8_t)(filters & 0xFF);
  b = (uint8_t)((b >> shift) | (b << (8 - shift)));
  /* Replicate to all 4 bytes (darktable packs the same byte 4× for RGGB). */
  return (uint32_t)b | ((uint32_t)b << 8) | ((uint32_t)b << 16) | ((uint32_t)b << 24);
}

/* Compute the properly scaled crop inset given the pipeline scale */
static int _compute_proper_crop(dt_dev_pixelpipe_iop_t *piece,
                                const dt_iop_roi_t *const roi_in,
                                const int value)
{
  const float scale = roi_in->scale / piece->iscale;
  return (int)roundf((float)value * scale);
}

/* ── process ─────────────────────────────────────────────────────────────── */

static void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawprepare_data_t *const d = piece->data;

  const int csx = _compute_proper_crop(piece, roi_in, d->left);
  const int csy = _compute_proper_crop(piece, roi_in, d->top);

  float *const out = (float *const)ovoid;

  if(piece->pipe->dsc.filters && piece->dsc_in.channels == 1
     && piece->dsc_in.datatype == TYPE_UINT16)
  {
    /* Raw uint16 Bayer mosaic: subtract black, normalise */
    const uint16_t *const in = (const uint16_t *const)ivoid;

    DT_OMP_FOR_SIMD(collapse(2))
    for(int j = 0; j < roi_out->height; j++)
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        const size_t pin  = (size_t)(roi_in->width * (j + csy) + csx) + i;
        const size_t pout = (size_t)j * roi_out->width + i;

        /* Determine the Bayer channel quadrant for this pixel */
        const int id = (((j + roi_out->y + d->top) & 1) << 1)
                     | ((i + roi_out->x + d->left) & 1);
        out[pout] = (in[pin] - d->sub[id]) / d->div[id];
      }
    }

    /* Adjust the CFA filter pattern for the crop offset */
    piece->pipe->dsc.filters =
        _crop_dcraw_filters(self->dev
                            ? ((dt_develop_t *)self->dev)->image_storage.buf_dsc.filters
                            : piece->pipe->image.buf_dsc.filters,
                            csx, csy);

    /* Adjust the X-Trans filter pattern for the crop offset */
    if(piece->pipe->dsc.filters == 9u)
    {
      for(int row = 0; row < 6; row++)
        for(int col = 0; col < 6; col++)
          piece->pipe->dsc.xtrans[row][col] =
              piece->pipe->image.buf_dsc.xtrans[(row + csy) % 6][(col + csx) % 6];
    }
  }
  else if(piece->pipe->dsc.filters && piece->dsc_in.channels == 1
          && piece->dsc_in.datatype == TYPE_FLOAT)
  {
    /* Raw float mosaic (HDR DNG) */
    const float *const in = (const float *const)ivoid;

    DT_OMP_FOR_SIMD(collapse(2))
    for(int j = 0; j < roi_out->height; j++)
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        const size_t pin  = (size_t)(roi_in->width * (j + csy) + csx) + i;
        const size_t pout = (size_t)j * roi_out->width + i;

        const int id = (((j + roi_out->y + d->top) & 1) << 1)
                     | ((i + roi_out->x + d->left) & 1);
        out[pout] = (in[pin] - d->sub[id]) / d->div[id];
      }
    }

    piece->pipe->dsc.filters =
        _crop_dcraw_filters(self->dev
                            ? ((dt_develop_t *)self->dev)->image_storage.buf_dsc.filters
                            : piece->pipe->image.buf_dsc.filters,
                            csx, csy);

    if(piece->pipe->dsc.filters == 9u)
    {
      for(int row = 0; row < 6; row++)
        for(int col = 0; col < 6; col++)
          piece->pipe->dsc.xtrans[row][col] =
              piece->pipe->image.buf_dsc.xtrans[(row + csy) % 6][(col + csx) % 6];
    }
  }
  else
  {
    /* Pre-downsampled multi-channel buffer (non-raw or pre-demosaiced) */
    const float *const in = (const float *const)ivoid;
    const int ch = piece->colors;

    DT_OMP_FOR_SIMD(collapse(3))
    for(int j = 0; j < roi_out->height; j++)
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        for(int c = 0; c < ch; c++)
        {
          const size_t pin  = (size_t)ch * (roi_in->width * (j + csy) + csx + i) + c;
          const size_t pout = (size_t)ch * (j * roi_out->width + i) + c;
          out[pout] = (in[pin] - d->sub[c]) / d->div[c];
        }
      }
    }
  }

  /* After rawprepare the pipeline maximum is [0, 1] by definition */
  for(int k = 0; k < 4; k++)
    piece->pipe->dsc.processed_maximum[k] = 1.0f;
}

/* ── output_format ───────────────────────────────────────────────────────── */

/*
 * rawprepare does not change the channel count or data type, but it does
 * write the rawprepare sub-struct of the buffer descriptor.
 */
static void output_format(dt_iop_module_t *self,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece,
                   dt_iop_buffer_dsc_t *dsc)
{
  (void)self; (void)pipe;

  /* Default: propagate existing format */
  /* (default_output_format would do: dsc stays as-is) */

  if(piece->data)
  {
    const dt_iop_rawprepare_data_t *d = piece->data;
    dsc->rawprepare.raw_black_level = d->rawprepare.raw_black_level;
    dsc->rawprepare.raw_white_point = d->rawprepare.raw_white_point;
  }
}

/* ── modify_roi_out ──────────────────────────────────────────────────────── */

/*
 * rawprepare crops sensor borders: output is smaller than input by the crop
 * amounts. We're not scaling, just cropping.
 */
static void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  (void)self;
  *roi_out = *roi_in;

  const dt_iop_rawprepare_data_t *d = piece->data;
  if(!d) return;

  roi_out->x = 0;
  roi_out->y = 0;

  const float scale = roi_in->scale / piece->iscale;
  roi_out->width  -= (int)roundf((float)(d->left + d->right)  * scale);
  roi_out->height -= (int)roundf((float)(d->top  + d->bottom) * scale);

  /* Ensure sane minimum */
  if(roi_out->width  < 1) roi_out->width  = 1;
  if(roi_out->height < 1) roi_out->height = 1;
}

/* ── modify_roi_in ───────────────────────────────────────────────────────── */

static void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  (void)self;
  *roi_in = *roi_out;

  const dt_iop_rawprepare_data_t *d = piece->data;
  if(!d) return;

  const float scale = roi_in->scale / piece->iscale;
  roi_in->width  += (int)roundf((float)(d->left + d->right)  * scale);
  roi_in->height += (int)roundf((float)(d->top  + d->bottom) * scale);
}

/* ── commit_params ───────────────────────────────────────────────────────── */

static void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_rawprepare_params_t *const p =
      (const dt_iop_rawprepare_params_t *)params;
  dt_iop_rawprepare_data_t *d = piece->data;

  d->left   = p->left;
  d->top    = p->top;
  d->right  = p->right;
  d->bottom = p->bottom;

  if(pipe->dsc.filters)
  {
    /* Bayer or X-Trans data: normalise by (white - black_per_channel) */
    const float white = (float)p->raw_white_point;

    for(int i = 0; i < 4; i++)
    {
      d->sub[i] = (float)p->raw_black_level_separate[i];
      d->div[i] = (white - d->sub[i]);
      /* Guard against degenerate divisor */
      if(d->div[i] < 1.0f) d->div[i] = 1.0f;
    }
  }
  else
  {
    /* Pre-demosaiced or LDR data: normalise by 65535 */
    const float normalizer =
      ((pipe->image.flags & DT_IMAGE_HDR) == DT_IMAGE_HDR)
      ? 1.0f
      : (float)UINT16_MAX;

    const float white = (float)p->raw_white_point / normalizer;
    for(int i = 0; i < 4; i++)
    {
      d->sub[i] = (float)p->raw_black_level_separate[i] / normalizer;
      d->div[i] = (white - d->sub[i]);
      if(d->div[i] < 1e-6f) d->div[i] = 1.0f;
    }
  }

  /* Cache the averaged black level and white point for the dsc */
  float black_sum = 0.0f;
  for(int i = 0; i < 4; i++) black_sum += (float)p->raw_black_level_separate[i];
  d->rawprepare.raw_black_level = (uint16_t)roundf(black_sum / 4.0f);
  d->rawprepare.raw_white_point = (uint16_t)MIN(p->raw_white_point, 65535u);

  /* GainMaps not supported in libdtpipe — always disabled */

  /* Disable rawprepare for non-raw images (normalised float input) */
  if(!dt_image_is_rawprepare_supported(&pipe->image))
    piece->enabled = FALSE;

  (void)self;
}

/* ── init_pipe / cleanup_pipe ────────────────────────────────────────────── */

static void init_pipe(dt_iop_module_t *self,
                      dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe;
  piece->data = calloc(1, sizeof(dt_iop_rawprepare_data_t));
}

static void cleanup_pipe(dt_iop_module_t *self,
                         dt_dev_pixelpipe_t *pipe,
                         dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe;
  free(piece->data);
  piece->data = NULL;
}

/* ── init ────────────────────────────────────────────────────────────────── */

/*
 * Set default params from the image metadata stored in module->dev.
 * In libdtpipe, module->dev is a dt_develop_t* (set by create.c).
 * Falls back to neutral defaults (no crop, black=0, white=65535) if dev
 * is NULL or if image metadata is unavailable.
 */
static void init(dt_iop_module_t *self)
{
  if(!self->params || self->params_size < (int32_t)sizeof(dt_iop_rawprepare_params_t))
    return;

  dt_iop_rawprepare_params_t *d = (dt_iop_rawprepare_params_t *)self->params;
  memset(d, 0, sizeof(*d));

  if(self->dev)
  {
    /* Use actual image metadata (populated by dtpipe_create) */
    const dt_image_t *img = &((dt_develop_t *)self->dev)->image_storage;
    d->left   = img->crop_x;
    d->top    = img->crop_y;
    d->right  = img->crop_right;
    d->bottom = img->crop_bottom;
    for(int i = 0; i < 4; i++)
      d->raw_black_level_separate[i] = img->raw_black_level_separate[i];
    d->raw_white_point = (uint32_t)img->raw_white_point;
  }
  else
  {
    /* Neutral passthrough defaults */
    d->left   = 0;
    d->top    = 0;
    d->right  = 0;
    d->bottom = 0;
    for(int i = 0; i < 4; i++)
      d->raw_black_level_separate[i] = 0;
    d->raw_white_point = UINT16_MAX;
  }

  d->flat_field = 0; /* FLAT_FIELD_OFF */

  /* Sync default_params if it is a separate buffer */
  if(self->default_params &&
     self->default_params != (dt_iop_params_t *)self->params)
    memcpy(self->default_params, d, sizeof(*d));
}

/* ── input/output colorspace ─────────────────────────────────────────────── */

static int input_colorspace(dt_iop_module_t *self,
                             dt_dev_pixelpipe_t *pipe,
                             dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)piece;
  return (pipe && !dt_image_is_raw(&pipe->image)) ? IOP_CS_RGB : IOP_CS_RAW;
}

static int output_colorspace(dt_iop_module_t *self,
                              dt_dev_pixelpipe_t *pipe,
                              dt_dev_pixelpipe_iop_t *piece)
{
  return input_colorspace(self, pipe, piece);
}

/* ── Registration ────────────────────────────────────────────────────────── */

/*
 * Called from init.c during library initialisation.
 * Populates all function pointers on the SO struct.
 */
void dt_iop_rawprepare_init_global(dt_iop_module_so_t *so)
{
  so->process_plain    = process;
  so->init             = init;
  so->init_pipe        = init_pipe;
  so->cleanup_pipe     = cleanup_pipe;
  so->commit_params    = commit_params;
  so->input_colorspace = input_colorspace;
  so->output_colorspace= output_colorspace;
  so->output_format    = output_format;
  so->modify_roi_in    = modify_roi_in;
  so->modify_roi_out   = modify_roi_out;
}
