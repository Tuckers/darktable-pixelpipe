/*
 * exposure.c — darktable exposure IOP, ported for libdtpipe
 *
 * Extracted from darktable src/iop/exposure.c (GPLv3).
 * GUI code, OpenCL, deflicker histogram, and reload_defaults() removed.
 * Adapted to compile against dtpipe_internal.h instead of darktable headers.
 *
 * Struct layout of dt_iop_exposure_params_t MUST match the descriptor table
 * in libdtpipe/src/pipe/params.c (_exposure_params_t):
 *
 *   int32_t mode                     (EXPOSURE_MODE_MANUAL=0, DEFLICKER=1)
 *   float   black                    black point correction [-1, 1]
 *   float   exposure                 EV adjustment          [-18, 18]
 *   float   deflicker_percentile     [0, 100]
 *   float   deflicker_target_level   [-18, 18]
 *   int32_t compensate_exposure_bias bool
 *   int32_t compensate_hilite_pres   bool
 *
 * Copyright (C) 2009-2024 darktable developers (GPLv3)
 */

#include "dtpipe_internal.h"
#include "iop/iop_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── fast log2 already in iop_math.h, but define the white/exposure helpers */

#define exposure2white(x) exp2f(-(x))

/* ── Parameter and data structs ─────────────────────────────────────────── */

/*
 * IMPORTANT: field order and types must exactly match _exposure_params_t in
 * pipe/params.c so that memcpy-based history load/save works correctly.
 */
typedef struct dt_iop_exposure_params_t
{
  int32_t mode;                    /* 0=MANUAL, 1=DEFLICKER                 */
  float   black;                   /* black point correction  [-1.0, 1.0]   */
  float   exposure;                /* EV adjustment           [-18.0, 18.0] */
  float   deflicker_percentile;    /* percentile              [0.0, 100.0]  */
  float   deflicker_target_level;  /* deflicker target EV     [-18.0, 18.0] */
  int32_t compensate_exposure_bias;/* bool                                  */
  int32_t compensate_hilite_pres;  /* bool                                  */
} dt_iop_exposure_params_t;

typedef struct dt_iop_exposure_data_t
{
  dt_iop_exposure_params_t params; /* snapshot of module->params at commit  */
  int   deflicker;                 /* 1 if deflicker mode active (always 0) */
  float black;                     /* computed black point                  */
  float scale;                     /* computed scale = 1/(white - black)    */
} dt_iop_exposure_data_t;

/* ── _process_common_setup ──────────────────────────────────────────────── */

/*
 * Compute d->black and d->scale from the committed params.
 * deflicker is not supported (always disabled); exposure is used directly.
 */
static void _process_common_setup(dt_iop_module_t *self,
                                  dt_dev_pixelpipe_iop_t *piece)
{
  (void)self;
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;

  d->black = d->params.black;
  const float white = exposure2white(d->params.exposure);
  d->scale = 1.0f / (white - d->black);
}

/* ── process ────────────────────────────────────────────────────────────── */

static void process(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    const void *const i,
                    void *const o,
                    const dt_iop_roi_t *const roi_in,
                    const dt_iop_roi_t *const roi_out)
{
  _process_common_setup(self, piece);

  const dt_iop_exposure_data_t *const d =
      (const dt_iop_exposure_data_t *)piece->data;

  const int ch = piece->colors;
  const float *const restrict in  = (const float *)i;
  float *const restrict       out = (float *)o;
  const float black = d->black;
  const float scale = d->scale;
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  DT_OMP_FOR_SIMD(aligned(in, out : 64))
  for(size_t k = 0; k < (size_t)ch * npixels; k++)
    out[k] = (in[k] - black) * scale;

  for(int k = 0; k < 3; k++)
    piece->pipe->dsc.processed_maximum[k] *= d->scale;
}

/* ── colorspace declarations ────────────────────────────────────────────── */

static dt_iop_colorspace_type_t input_colorspace(dt_iop_module_t *self,
                                                  dt_dev_pixelpipe_t *pipe,
                                                  dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe; (void)piece;
  return IOP_CS_RGB;
}

static dt_iop_colorspace_type_t output_colorspace(dt_iop_module_t *self,
                                                   dt_dev_pixelpipe_t *pipe,
                                                   dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe; (void)piece;
  return IOP_CS_RGB;
}

/* ── commit_params ──────────────────────────────────────────────────────── */

/*
 * Copy user params into piece->data. Simplified from darktable:
 * - No deflicker (always set d->deflicker = 0)
 * - No exposure-bias compensation (would need EXIF data wired through dev)
 * - No highlight-preservation compensation
 * These can be added later when module->dev is fully populated.
 */
static void commit_params(dt_iop_module_t *self,
                          dt_iop_params_t *p1,
                          dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece)
{
  (void)pipe;
  const dt_iop_exposure_params_t *p = (const dt_iop_exposure_params_t *)p1;
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;

  d->params.black                   = p->black;
  d->params.exposure                = p->exposure;
  d->params.deflicker_percentile    = p->deflicker_percentile;
  d->params.deflicker_target_level  = p->deflicker_target_level;
  d->deflicker                      = 0; /* deflicker not supported */
}

/* ── init_pipe / cleanup_pipe ───────────────────────────────────────────── */

static void init_pipe(dt_iop_module_t *self,
                      dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe;
  piece->data = calloc(1, sizeof(dt_iop_exposure_data_t));
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
 * Set the module's default params. These are used when no XMP/history has
 * been loaded. Neutral defaults: no exposure adjustment, no black shift.
 */
static void init(dt_iop_module_t *self)
{
  if(!self->params || self->params_size < sizeof(dt_iop_exposure_params_t))
    return;

  dt_iop_exposure_params_t *d = (dt_iop_exposure_params_t *)self->params;
  memset(d, 0, sizeof(*d));
  d->mode                    = 0;    /* EXPOSURE_MODE_MANUAL */
  d->black                   = 0.0f;
  d->exposure                = 0.0f;
  d->deflicker_percentile    = 50.0f;
  d->deflicker_target_level  = -4.0f;
  d->compensate_exposure_bias= 0;
  d->compensate_hilite_pres  = 0;

  /* Also initialise default_params if it points to a separate buffer */
  if(self->default_params &&
     self->default_params != (dt_iop_params_t *)self->params)
    memcpy(self->default_params, d, sizeof(*d));
}

/* ── Registration ───────────────────────────────────────────────────────── */

/*
 * Called from init.c during library initialisation.
 * Populates all function pointers on the SO struct.
 */
void dt_iop_exposure_init_global(dt_iop_module_so_t *so)
{
  so->process_plain    = process;
  so->init             = init;
  so->init_pipe        = init_pipe;
  so->cleanup_pipe     = cleanup_pipe;
  so->commit_params    = commit_params;
  so->input_colorspace = input_colorspace;
  so->output_colorspace= output_colorspace;
  /* modify_roi_in / modify_roi_out: not needed — exposure is a 1:1 pixel op */
  /* output_format: not needed — exposure does not change buffer format       */
}
