/*
 * temperature.c — darktable temperature (white balance) IOP, ported for libdtpipe
 *
 * Extracted from darktable src/iop/temperature.c (GPLv3).
 * GUI code, OpenCL, lcms2 color matrix conversion, wb_presets database,
 * reload_defaults() complexity, and deflicker removed.
 * Adapted to compile against dtpipe_internal.h instead of darktable headers.
 *
 * What this module does:
 *   - Multiplies each raw Bayer sensel by its per-channel white balance
 *     coefficient (red, green, blue, various/4th).
 *   - Operates in IOP_CS_RAW (before demosaic).
 *   - Stores applied coefficients in pipe->dsc.temperature and dev->chroma.
 *
 * Param struct layout MUST match _temperature_params_t in pipe/params.c:
 *   float red, green, blue, various   (per-channel WB multipliers [0, 8])
 *   int   preset                      (DT_IOP_TEMP_AS_SHOT=0, etc.)
 *
 * Simplifications vs. darktable:
 *   - No temperature/tint UI conversion (the cmsCIEXYZ / blackbody math).
 *   - No wb_presets database lookups.
 *   - init() reads as-shot WB coefficients directly from image metadata.
 *   - commit_params() copies coefficients from params → data (no hide_enable
 *     button logic, no process_cl_ready manipulation).
 *   - _publish_chroma() guards against NULL dev.
 *
 * Copyright (C) 2009-2025 darktable developers (GPLv3)
 */

#include "dtpipe_internal.h"
#include "iop/iop_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── Preset constants (mirrors darktable values) ──────────────────────────── */

#define DT_IOP_TEMP_UNKNOWN   -1
#define DT_IOP_TEMP_AS_SHOT    0
#define DT_IOP_TEMP_SPOT       1
#define DT_IOP_TEMP_USER       2
#define DT_IOP_TEMP_D65        3
#define DT_IOP_TEMP_D65_LATE   4

/* ── Parameter and data structs ─────────────────────────────────────────── */

/*
 * IMPORTANT: field order and types must exactly match _temperature_params_t in
 * pipe/params.c so that memcpy-based history load/save works correctly.
 */
typedef struct dt_iop_temperature_params_t
{
  float red;     /* R channel multiplier  [0.0, 8.0] */
  float green;   /* G channel multiplier  [0.0, 8.0] */
  float blue;    /* B channel multiplier  [0.0, 8.0] */
  float various; /* 4th channel multiplier[0.0, 8.0] */
  int   preset;  /* DT_IOP_TEMP_* constant           */
} dt_iop_temperature_params_t;

typedef struct dt_iop_temperature_data_t
{
  float coeffs[4]; /* committed per-channel WB multipliers */
  int   preset;
} dt_iop_temperature_data_t;

/* ── Helpers ────────────────────────────────────────────────────────────── */

/*
 * Write applied WB coefficients back to the pipeline descriptor and to
 * dev->chroma so downstream modules (colorin, filmic…) can read them.
 * Guards against NULL dev.
 */
static void _publish_chroma(dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_temperature_data_t *const d =
      (const dt_iop_temperature_data_t *)piece->data;
  dt_iop_module_t *self = piece->module;

  piece->pipe->dsc.temperature.enabled = piece->enabled;
  for(int k = 0; k < 4; k++)
  {
    piece->pipe->dsc.temperature.coeffs[k] = d->coeffs[k];
    piece->pipe->dsc.processed_maximum[k] *=
        (d->coeffs[k] > 0.0f ? d->coeffs[k] : 1.0f);
  }

  /* Update dev->chroma if we have a develop context */
  if(self->dev)
  {
    dt_develop_t *dev = (dt_develop_t *)self->dev;
    dev->chroma.late_correction = (d->preset == DT_IOP_TEMP_D65_LATE);
    for(int k = 0; k < 4; k++)
      dev->chroma.wb_coeffs[k] = piece->enabled ? d->coeffs[k] : 1.0f;
  }
}

/* ── Colorspace declarations ────────────────────────────────────────────── */

static dt_iop_colorspace_type_t input_colorspace(dt_iop_module_t *self,
                                                  dt_dev_pixelpipe_t *pipe,
                                                  dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe;
  /* Temperature works on RAW mosaic data; if the piece has already been
     converted (e.g. demosaic first in test), fall back to RGB. */
  if(piece && piece->dsc_in.cst != IOP_CS_RAW)
    return IOP_CS_RGB;
  return IOP_CS_RAW;
}

static dt_iop_colorspace_type_t output_colorspace(dt_iop_module_t *self,
                                                   dt_dev_pixelpipe_t *pipe,
                                                   dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe;
  if(piece && piece->dsc_in.cst != IOP_CS_RAW)
    return IOP_CS_RGB;
  return IOP_CS_RAW;
}

/* ── process ────────────────────────────────────────────────────────────── */

/*
 * Apply per-channel WB multipliers to the raw Bayer mosaic.
 *
 * Three code paths:
 *   1. X-Trans (filters == 9u) — FCxtrans() per sensel
 *   2. Bayer    (filters != 0) — FC() per sensel, SIMD-friendly 4-wide loop
 *   3. Non-mosaiced (RGB or RGBA, filters == 0) — apply coeffs[c] per channel
 */
static void process(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    const void *const ivoid,
                    void *const ovoid,
                    const dt_iop_roi_t *const roi_in,
                    const dt_iop_roi_t *const roi_out)
{
  (void)roi_in;
  const uint32_t filters = piece->pipe->dsc.filters;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  const dt_iop_temperature_data_t *const d =
      (const dt_iop_temperature_data_t *)piece->data;

  const float *const in  = (const float *const)ivoid;
  float *const       out = (float *const)ovoid;
  const float *const d_coeffs = d->coeffs;

  if(filters == 9u)
  {
    /* X-Trans float mosaiced */
    DT_OMP_FOR()
    for(int j = 0; j < roi_out->height; j++)
    {
      /* pre-compute per-group-of-4 coefficient lookup for this row */
      const float DT_ALIGNED_PIXEL coeffs[3][4] = {
        { d_coeffs[FCxtrans(j, 0,  roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 1,  roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 2,  roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 3,  roi_out, xtrans)] },
        { d_coeffs[FCxtrans(j, 4,  roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 5,  roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 6,  roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 7,  roi_out, xtrans)] },
        { d_coeffs[FCxtrans(j, 8,  roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 9,  roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 10, roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 11, roi_out, xtrans)] },
      };
      int i = 0;
      for(int coeff = 0; i + 4 <= roi_out->width; i += 4, coeff = (coeff + 1) % 3)
      {
        const size_t p = (size_t)j * roi_out->width + i;
        for(int c = 0; c < 4; c++)
          out[p + c] = in[p + c] * coeffs[coeff][c];
      }
      for(; i < roi_out->width; i++)
      {
        const size_t p = (size_t)j * roi_out->width + i;
        out[p] = in[p] * d_coeffs[FCxtrans(j, i, roi_out, xtrans)];
      }
    }
  }
  else if(filters)
  {
    /* Bayer float mosaiced */
    const int width = roi_out->width;
    DT_OMP_FOR()
    for(int j = 0; j < roi_out->height; j++)
    {
      const int offset_j = j + roi_out->y;
      int i = 0;

      /* Handle any unaligned prefix (when (j*width) is not a multiple of 4) */
      const int alignment = 3 & (4 - ((j * width) & 3));
      for(; i < alignment && i < width; i++)
      {
        const size_t p = (size_t)j * width + i;
        out[p] = in[p] * d_coeffs[FC(offset_j, i + roi_out->x, filters)];
      }

      /* 4-wide SIMD-friendly loop */
      if(i < width)
      {
        const float DT_ALIGNED_PIXEL c4[4] = {
          d_coeffs[FC(offset_j, i     + roi_out->x, filters)],
          d_coeffs[FC(offset_j, i + 1 + roi_out->x, filters)],
          d_coeffs[FC(offset_j, i + 2 + roi_out->x, filters)],
          d_coeffs[FC(offset_j, i + 3 + roi_out->x, filters)],
        };
        for(; i < width - 3; i += 4)
        {
          const size_t p = (size_t)j * width + i;
          out[p + 0] = in[p + 0] * c4[0];
          out[p + 1] = in[p + 1] * c4[1];
          out[p + 2] = in[p + 2] * c4[2];
          out[p + 3] = in[p + 3] * c4[3];
        }
      }

      /* Trailing sensels */
      for(; i < width; i++)
      {
        const size_t p = (size_t)j * width + i;
        out[p] = in[p] * d_coeffs[FC(offset_j, i + roi_out->x, filters)];
      }
    }
  }
  else
  {
    /* Non-mosaiced (RGBA or RGB) — apply per-channel coefficients */
    const size_t npixels = (size_t)roi_out->width * roi_out->height;
    DT_OMP_FOR()
    for(size_t k = 0; k < 4 * npixels; k += 4)
    {
      for(int c = 0; c < 4; c++)
        out[k + c] = in[k + c] * d_coeffs[c];
    }
  }

  _publish_chroma(piece);
}

/* ── commit_params ──────────────────────────────────────────────────────── */

/*
 * Copy user params (the four channel multipliers) into piece->data.
 *
 * Simplified from darktable:
 *   - No hide_enable_button logic.
 *   - No process_cl_ready manipulation.
 *   - Just copy coefficients and update dev->chroma.
 */
static void commit_params(dt_iop_module_t *self,
                          dt_iop_params_t *p1,
                          dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece)
{
  (void)pipe;
  const dt_iop_temperature_params_t *p = (const dt_iop_temperature_params_t *)p1;
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;

  /* The first four floats of params_t are the channel coefficients */
  d->coeffs[0] = (p->red     > 0.0f) ? p->red     : 1.0f;
  d->coeffs[1] = (p->green   > 0.0f) ? p->green   : 1.0f;
  d->coeffs[2] = (p->blue    > 0.0f) ? p->blue    : 1.0f;
  d->coeffs[3] = (p->various > 0.0f) ? p->various : 1.0f;
  d->preset    = p->preset;

  /* Update dev->chroma.wb_coeffs now (for modules that read it at
     commit time rather than at process time). */
  if(self->dev)
  {
    dt_develop_t *dev = (dt_develop_t *)self->dev;
    for(int k = 0; k < 4; k++)
      dev->chroma.wb_coeffs[k] = piece->enabled ? d->coeffs[k] : 1.0f;
    dev->chroma.late_correction = (p->preset == DT_IOP_TEMP_D65_LATE);
  }
}

/* ── init_pipe / cleanup_pipe ───────────────────────────────────────────── */

static void init_pipe(dt_iop_module_t *self,
                      dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe;
  piece->data = calloc(1, sizeof(dt_iop_temperature_data_t));
  /* Safe default: neutral coefficients */
  if(piece->data)
  {
    dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
    d->coeffs[0] = d->coeffs[1] = d->coeffs[2] = d->coeffs[3] = 1.0f;
    d->preset = DT_IOP_TEMP_AS_SHOT;
  }
}

static void cleanup_pipe(dt_iop_module_t *self,
                         dt_dev_pixelpipe_t *pipe,
                         dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe;
  free(piece->data);
  piece->data = NULL;
}

/* ── init ───────────────────────────────────────────────────────────────── */

/*
 * Set the module's default params.
 *
 * Strategy (simplified):
 *   - If dev->image_storage has valid as-shot WB coefficients, use them
 *     normalised to green == 1.0.
 *   - Otherwise fall back to neutral 1.0 / 1.0 / 1.0 / NaN (matching
 *     the darktable convention for the 4th coefficient).
 *
 * The complex temperature/tint → CAM coefficient conversion and the
 * wb_presets database lookup are deferred to a future task (they require
 * lcms2 camera-matrix data not yet wired up in libdtpipe).
 */
static void init(dt_iop_module_t *self)
{
  if(!self->params || self->params_size < sizeof(dt_iop_temperature_params_t))
    return;

  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  memset(p, 0, sizeof(*p));

  /* Default: neutral coefficients */
  p->red     = 1.0f;
  p->green   = 1.0f;
  p->blue    = 1.0f;
  p->various = 1.0f;
  p->preset  = DT_IOP_TEMP_AS_SHOT;

  /* Use as-shot WB from the loaded image if available */
  if(self->dev)
  {
    dt_develop_t *dev = (dt_develop_t *)self->dev;
    const dt_image_t *img = &dev->image_storage;
    const int is_raw = (img->flags & DT_IMAGE_RAW) != 0;

    if(is_raw)
    {
      /* Check all three primary coefficients are finite and non-zero */
      int ok = 1;
      for(int k = 0; k < 3; k++)
      {
        if(!dt_isfinite(img->wb_coeffs[k]) || img->wb_coeffs[k] == 0.0f)
        {
          ok = 0;
          break;
        }
      }

      if(ok && img->wb_coeffs[1] > 0.0f)
      {
        /* Normalise so that green == 1.0 */
        const float g_inv = 1.0f / img->wb_coeffs[1];
        p->red     = img->wb_coeffs[0] * g_inv;
        p->green   = 1.0f;
        p->blue    = img->wb_coeffs[2] * g_inv;
        p->various = dt_isfinite(img->wb_coeffs[3]) ? img->wb_coeffs[3] * g_inv : 1.0f;
        p->preset  = DT_IOP_TEMP_AS_SHOT;
      }
    }
  }

  /* Mirror into default_params */
  if(self->default_params &&
     self->default_params != (dt_iop_params_t *)self->params)
    memcpy(self->default_params, p, sizeof(*p));
}

/* ── Registration ───────────────────────────────────────────────────────── */

/*
 * Called from init.c during library initialisation.
 * Populates all function pointers on the SO struct.
 */
void dt_iop_temperature_init_global(dt_iop_module_so_t *so)
{
  so->process_plain    = process;
  so->init             = init;
  so->init_pipe        = init_pipe;
  so->cleanup_pipe     = cleanup_pipe;
  so->commit_params    = commit_params;
  so->input_colorspace = input_colorspace;
  so->output_colorspace= output_colorspace;
  /* modify_roi_in / modify_roi_out: not needed — temperature is 1:1 pixel   */
  /* output_format: not needed — temperature does not change buffer format    */
}
