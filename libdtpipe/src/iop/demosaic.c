/*
 * demosaic.c — darktable demosaic IOP, ported for libdtpipe
 *
 * Extracted from darktable src/iop/demosaic.c (GPLv3).
 * GUI code, OpenCL, capture sharpening, dual-demosaic, xtrans,
 * lmmse, amaze, rcd, vng, and reload_defaults() removed.
 * Only Bayer PPG + passthrough modes are compiled in (Phase A).
 * Adapted to compile against dtpipe_internal.h.
 *
 * Critical role: this is the 1-channel RAW → 4-channel float RGBA
 * format-transition module.  output_format() MUST set channels=4.
 *
 * Param struct layout MUST match _demosaic_params_t in pipe/params.c:
 *   int32_t green_eq            green equalisation method   (0=off)
 *   float   median_thrs         median filter threshold     (0.0)
 *   int32_t color_smoothing     smoothing iterations        (0=off)
 *   int32_t demosaicing_method  algorithm enum              (0=PPG)
 *   int32_t lmmse_refine        LMMSE refinement level      (1)
 *   float   dual_thrs           dual demosaic threshold     (0.2)
 *   float   cs_radius           capture sharpen radius      (0.0)
 *   float   cs_thrs             capture sharpen threshold   (0.4)
 *   float   cs_boost            capture sharpen boost       (0.0)
 *   int32_t cs_iter             capture sharpen iterations  (8)
 *   float   cs_center           capture sharpen center      (0.0)
 *   int32_t cs_enabled          capture sharpening on/off  (0)
 *
 * Copyright (C) 2010-2025 darktable developers (GPLv3)
 */

#include "dtpipe_internal.h"
#include "iop/iop_math.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* DT_IMAGE_4BAYER (CYGM sensors) not defined in our compat layer — always 0 */
#ifndef DT_IMAGE_4BAYER
#  define DT_IMAGE_4BAYER 0
#endif

/* dt_image_is_mono_sraw: sRAW monochrome from Sony/Fuji — treat same as monochrome */
static inline gboolean _dt_image_is_mono_sraw(const dt_image_t *img)
{
  return dt_image_is_monochrome(img);
}
#define dt_image_is_mono_sraw(img) _dt_image_is_mono_sraw(img)

/* ── Enumerations ───────────────────────────────────────────────────────── */

#define DT_DEMOSAIC_XTRANS 1024
#define DT_DEMOSAIC_DUAL   2048

typedef enum dt_iop_demosaic_method_t
{
  DT_IOP_DEMOSAIC_PPG                   = 0,
  DT_IOP_DEMOSAIC_AMAZE                 = 1,
  DT_IOP_DEMOSAIC_VNG4                  = 2,
  DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME = 3,
  DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR     = 4,
  DT_IOP_DEMOSAIC_RCD                   = 5,
  DT_IOP_DEMOSAIC_LMMSE                 = 6,
  DT_IOP_DEMOSAIC_MONO                  = 7,
  DT_IOP_DEMOSAIC_RCD_DUAL   = DT_DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_RCD,
  DT_IOP_DEMOSAIC_AMAZE_DUAL = DT_DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_AMAZE,
  DT_IOP_DEMOSAIC_VNG        = DT_DEMOSAIC_XTRANS | 0,
  DT_IOP_DEMOSAIC_MARKESTEIJN   = DT_DEMOSAIC_XTRANS | 1,
  DT_IOP_DEMOSAIC_MARKESTEIJN_3 = DT_DEMOSAIC_XTRANS | 2,
  DT_IOP_DEMOSAIC_PASSTHR_MONOX  = DT_DEMOSAIC_XTRANS | 3,
  DT_IOP_DEMOSAIC_FDC             = DT_DEMOSAIC_XTRANS | 4,
  DT_IOP_DEMOSAIC_PASSTHR_COLORX  = DT_DEMOSAIC_XTRANS | 5,
  DT_IOP_DEMOSAIC_MARKEST3_DUAL   = DT_DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_MARKESTEIJN_3,
} dt_iop_demosaic_method_t;

typedef enum dt_iop_demosaic_greeneq_t
{
  DT_IOP_GREEN_EQ_NO    = 0,
  DT_IOP_GREEN_EQ_LOCAL = 1,
  DT_IOP_GREEN_EQ_FULL  = 2,
  DT_IOP_GREEN_EQ_BOTH  = 3,
} dt_iop_demosaic_greeneq_t;

typedef enum dt_iop_demosaic_smooth_t
{
  DT_DEMOSAIC_SMOOTH_OFF = 0,
  DT_DEMOSAIC_SMOOTH_1   = 1,
  DT_DEMOSAIC_SMOOTH_2   = 2,
  DT_DEMOSAIC_SMOOTH_3   = 3,
  DT_DEMOSAIC_SMOOTH_4   = 4,
  DT_DEMOSAIC_SMOOTH_5   = 5,
} dt_iop_demosaic_smooth_t;

/* ── Parameter and data structs ─────────────────────────────────────────── */

/*
 * IMPORTANT: field order and types must exactly match _demosaic_params_t in
 * pipe/params.c so that memcpy-based history load/save works correctly.
 */
typedef struct dt_iop_demosaic_params_t
{
  int32_t green_eq;            /* green equalisation method   */
  float   median_thrs;         /* median filter threshold     */
  int32_t color_smoothing;     /* smoothing iterations        */
  int32_t demosaicing_method;  /* algorithm enum              */
  int32_t lmmse_refine;        /* LMMSE refinement level      */
  float   dual_thrs;           /* dual demosaic threshold     */
  float   cs_radius;           /* capture sharpen radius      */
  float   cs_thrs;             /* capture sharpen threshold   */
  float   cs_boost;            /* capture sharpen boost       */
  int32_t cs_iter;             /* capture sharpen iterations  */
  float   cs_center;           /* capture sharpen center      */
  int32_t cs_enabled;          /* capture sharpening on/off   */
} dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_data_t
{
  dt_iop_demosaic_greeneq_t  green_eq;
  dt_iop_demosaic_smooth_t   color_smoothing;
  uint32_t                   demosaicing_method;
  float                      median_thrs;
  float                      dual_thrs;
  gboolean                   cs_enabled;
} dt_iop_demosaic_data_t;

/* ── Crop Bayer filter helper (same as rawprepare's _crop_dcraw_filters) ── */

static uint32_t _crop_dcraw_filters(uint32_t filters, int cx, int cy)
{
  if(filters == 0 || filters == 9u) return filters;
  /* 8-bit Bayer descriptor byte, repeated to fill 32 bits */
  uint8_t fq = (uint8_t)(filters & 0xff);
  /* shift the 2×2 tile: swap bits within the byte for x offset,
     rotate 4-bit halves for y offset */
  if(cx & 1) fq = (uint8_t)(((fq & 0x55) << 1) | ((fq & 0xaa) >> 1));
  if(cy & 1) fq = (uint8_t)(((fq & 0x0f) << 4) | ((fq & 0xf0) >> 4));
  return (uint32_t)fq | ((uint32_t)fq << 8) | ((uint32_t)fq << 16) | ((uint32_t)fq << 24);
}

/* ── Include textual demosaicing implementations ─────────────────────────── */

#include "iop/demosaicing/basics.c"
#include "iop/demosaicing/passthrough.c"
#include "iop/demosaicing/ppg.c"

/* ── ROI helpers ─────────────────────────────────────────────────────────── */

static inline int _snap_to_cfa(const int p, const uint32_t filters)
{
  const int snap = !filters ? 1 : filters != 9u ? 2 : 3;
  return (p / snap) * snap;
}

static void modify_roi_out(dt_iop_module_t *self,
                           dt_dev_pixelpipe_iop_t *piece,
                           dt_iop_roi_t *roi_out,
                           const dt_iop_roi_t *const roi_in)
{
  (void)self; (void)piece;
  *roi_out = *roi_in;
  roi_out->x = 0;
  roi_out->y = 0;
}

/* Returns true if we should run full PPG (scale > 0.5), false for fast half-size path */
static gboolean _demosaic_full(const dt_iop_roi_t *const roi_out)
{
  return roi_out->scale > 0.5f;
}

static void modify_roi_in(dt_iop_module_t *self,
                          dt_dev_pixelpipe_iop_t *piece,
                          const dt_iop_roi_t *roi_out,
                          dt_iop_roi_t *roi_in)
{
  (void)self;
  *roi_in = *roi_out;
  const uint32_t filters = piece->pipe->dsc.filters;

  if(_demosaic_full(roi_out))
  {
    /* Full PPG: request full-resolution input, snapped to CFA grid */
    roi_in->x = MAX(0, _snap_to_cfa(roi_in->x / roi_out->scale, filters));
    roi_in->y = MAX(0, _snap_to_cfa(roi_in->y / roi_out->scale, filters));
    roi_in->width  = MAX(8, roi_in->width  / roi_out->scale);
    roi_in->height = MAX(8, roi_in->height / roi_out->scale);
    roi_in->scale  = 1.0f;
  }
  else
  {
    /* Fast half-size path: request 2× upscaled region from input (each
       2×2 Bayer quad becomes one output pixel), snapped to CFA alignment */
    roi_in->x = MAX(0, _snap_to_cfa((int)(roi_in->x / roi_out->scale), filters));
    roi_in->y = MAX(0, _snap_to_cfa((int)(roi_in->y / roi_out->scale), filters));
    roi_in->width  = MAX(8, (int)(roi_in->width  / roi_out->scale));
    roi_in->height = MAX(8, (int)(roi_in->height / roi_out->scale));
    roi_in->scale  = 1.0f;
  }
}

/* ── output_format — declares the 1→4 channel format transition ─────────── */

static void output_format(dt_iop_module_t *self,
                          dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece,
                          dt_iop_buffer_dsc_t *dsc)
{
  (void)self; (void)pipe; (void)piece;
  /* CRITICAL: demosaic converts 1-channel raw to 4-channel float RGBA */
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dsc->cst      = IOP_CS_RGB;
}

/* ── process ─────────────────────────────────────────────────────────────── */

static void process(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *const piece,
                    const void *const i,
                    void *const o,
                    const dt_iop_roi_t *const roi_in,
                    const dt_iop_roi_t *const roi_out)
{
  const dt_image_t *img = (const dt_image_t *)self->dev;  /* dev→image_storage set in dev */
  dt_dev_pixelpipe_t *const pipe = piece->pipe;
  const dt_iop_demosaic_data_t *d = (const dt_iop_demosaic_data_t *)piece->data;

  /* Resolve image_storage from dev (dev is typed void* → dt_develop_t*) */
  dt_develop_t *dev = (dt_develop_t *)self->dev;
  img = &dev->image_storage;

  /* Build adjusted xtrans table for current ROI position */
  uint8_t xtrans[6][6];
  for(int ii = 0; ii < 6; ++ii)
    for(int jj = 0; jj < 6; ++jj)
      xtrans[jj][ii] = pipe->dsc.xtrans[(jj + roi_in->y) % 6][(ii + roi_in->x) % 6];

  const uint32_t filters = _crop_dcraw_filters(pipe->dsc.filters, roi_in->x, roi_in->y);

  const gboolean is_xtrans = (filters == 9u);
  const gboolean is_4bayer = (img->flags & DT_IMAGE_4BAYER) != 0;
  const gboolean true_monochrome = dt_image_is_mono_sraw(img);

  int method = (int)(d->demosaicing_method) & ~DT_DEMOSAIC_DUAL;

  /* Safety: fall back to PPG for small tiles where PPG can't run anyway */
  const int width  = roi_in->width;
  const int height = roi_in->height;
  if((width < 16 || height < 16)
     && method != DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
     && method != DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
    method = DT_IOP_DEMOSAIC_PPG;

  /* Alias xtrans passthrough to Bayer passthrough when on non-xtrans sensor */
  if(method == DT_IOP_DEMOSAIC_PASSTHR_MONOX)
    method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  if(method == DT_IOP_DEMOSAIC_PASSTHR_COLORX)
    method = DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;

  /* Monochrome images: copy straight through */
  if(true_monochrome)
    method = DT_IOP_DEMOSAIC_MONO;

  /* For xtrans sensors we don't have a port yet — fall back to passthrough */
  if(is_xtrans && method != DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
               && method != DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR
               && method != DT_IOP_DEMOSAIC_MONO)
  {
    fprintf(stderr, "[demosaic] X-Trans sensor detected; falling back to passthrough_color\n");
    method = DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;
  }

  const gboolean fullscale = _demosaic_full(roi_out);

  /* Fast path: scale <= 0.5 — use half-size Bayer averaging directly into
     the output buffer.  Each 2×2 Bayer quad averages to one RGBA pixel.
     This is ~4× faster than full PPG and adequate for preview rendering. */
  if(!fullscale)
  {
    if(method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
      dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f(
          (float *)o, (const float *)i, roi_out, roi_in, roi_out->width, width);
    else if(method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
      passthrough_color((float *)o, (const float *)i, roi_out->width, roi_out->height, filters,
                        (const uint8_t (*)[6])xtrans);
    else if(!is_xtrans)
      dt_iop_clip_and_zoom_demosaic_half_size_f(
          (float *)o, (const float *)i, roi_out, roi_in, roi_out->width, width, filters);
    else
      dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f(
          (float *)o, (const float *)i, roi_out, roi_in, roi_out->width, width);

    const float procmax = dt_iop_get_processed_maximum(piece);
    for(int k = 0; k < 3; k++)
      pipe->dsc.processed_maximum[k] = procmax;
    return;
  }

  /* Full PPG path (scale > 0.5): process full-resolution input */

  /* direct=TRUE when roi_in and roi_out have the same pixel dimensions */
  const gboolean direct = (roi_out->width == width && roi_out->height == height
                           && feqf(roi_in->scale, roi_out->scale, 1e-8f));

  /* Allocate intermediate buffer if we need to scale output */
  float *out = direct ? (float *)o : dt_iop_image_alloc(width, height, 4);
  if(!out)
  {
    fprintf(stderr, "[demosaic] can't allocate output buffer\n");
    return;
  }

  /* Optional green equilibration */
  float *in = (float *)i;
  float *green_in = NULL;
  const gboolean no_masking = (pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_NONE);
  const gboolean greens = (!is_xtrans && !is_4bayer && !true_monochrome)
                          && (d->green_eq != DT_IOP_GREEN_EQ_NO)
                          && no_masking;
  if(greens)
  {
    const float threshold = 0.0001f * img->exif_iso;
    green_in = dt_iop_image_alloc(width, height, 1);
    if(green_in)
    {
      in = green_in;
      float *aux = NULL;
      switch(d->green_eq)
      {
        case DT_IOP_GREEN_EQ_FULL:
          green_equilibration_favg(in, (float *)i, width, height, filters);
          break;
        case DT_IOP_GREEN_EQ_LOCAL:
          green_equilibration_lavg(in, (float *)i, width, height, filters, threshold);
          break;
        case DT_IOP_GREEN_EQ_BOTH:
          aux = dt_iop_image_alloc(width, height, 1);
          if(aux)
          {
            green_equilibration_favg(aux, (float *)i, width, height, filters);
            green_equilibration_lavg(in, aux, width, height, filters, threshold);
            dt_free_align(aux);
          }
          break;
        default:
          break;
      }
    }
    else
      fprintf(stderr, "[demosaic] can't allocate green equilibration buffer\n");
  }

  /* Dispatch to the chosen algorithm */
  if(method == DT_IOP_DEMOSAIC_MONO)
    dt_iop_image_copy_by_size(out, in, width, height, 4);
  else if(method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
    passthrough_monochrome(out, in, width, height);
  else if(method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
    passthrough_color(out, in, width, height, filters, (const uint8_t (*)[6])xtrans);
  else
    demosaic_ppg(out, in, width, height, filters, d->median_thrs);

  dt_free_align(green_in);

  /* Optional color smoothing */
  if(d->color_smoothing != DT_DEMOSAIC_SMOOTH_OFF && no_masking)
    color_smoothing(out, width, height, (int)d->color_smoothing);

  /* Scale output if roi_out dimensions differ from roi_in */
  if(!direct)
  {
    dt_iop_clip_and_zoom_roi((float *)o, out, roi_out, roi_in);
    dt_free_align(out);
  }

  /* Update processed_maximum[] */
  const float procmax = dt_iop_get_processed_maximum(piece);
  for(int k = 0; k < 3; k++)
    pipe->dsc.processed_maximum[k] = procmax;
}

/* ── commit_params ─────────────────────────────────────────────────────── */

static void commit_params(dt_iop_module_t *self,
                          dt_iop_params_t *params,
                          dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_demosaic_params_t *const p = (const dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = (dt_iop_demosaic_data_t *)piece->data;

  /* Disable for non-raw images */
  dt_develop_t *dev = (dt_develop_t *)self->dev;
  const dt_image_t *img = &dev->image_storage;
  const gboolean true_monochrome = dt_image_is_mono_sraw(img);
  if(!(dt_image_is_raw(img) || true_monochrome))
  {
    piece->enabled = FALSE;
    return;
  }

  d->green_eq         = (dt_iop_demosaic_greeneq_t)p->green_eq;
  d->color_smoothing  = (dt_iop_demosaic_smooth_t)p->color_smoothing;
  d->median_thrs      = p->median_thrs;
  d->dual_thrs        = p->dual_thrs;
  d->cs_enabled       = (gboolean)p->cs_enabled;

  dt_iop_demosaic_method_t use_method = (dt_iop_demosaic_method_t)p->demosaicing_method;
  const gboolean xmethod  = (use_method & DT_DEMOSAIC_XTRANS) != 0;
  const gboolean is_dual  = (use_method & DT_DEMOSAIC_DUAL) != 0;
  const gboolean bayer4   = (img->flags & DT_IMAGE_4BAYER) != 0;
  const gboolean xtrans   = (img->buf_dsc.filters == 9u);
  const gboolean bayer    = !bayer4 && !xtrans && !true_monochrome;
  const gboolean passing  = (use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
                              || use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR);

  /* Correct xtrans/bayer mismatches */
  if(bayer && xmethod)
    use_method = is_dual ? DT_IOP_DEMOSAIC_RCD_DUAL : DT_IOP_DEMOSAIC_RCD;
  if(xtrans && !xmethod)
    use_method = is_dual ? DT_IOP_DEMOSAIC_MARKEST3_DUAL : DT_IOP_DEMOSAIC_MARKESTEIJN;
  if(bayer4 && !passing)
    use_method = DT_IOP_DEMOSAIC_VNG4;
  if(true_monochrome)
    use_method = DT_IOP_DEMOSAIC_MONO;

  if(use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  if(use_method == DT_IOP_DEMOSAIC_PASSTHR_COLORX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;

  /* Median only meaningful for PPG */
  if(use_method != DT_IOP_DEMOSAIC_PPG)
    d->median_thrs = 0.0f;

  if(passing || bayer4 || true_monochrome)
  {
    d->green_eq       = DT_IOP_GREEN_EQ_NO;
    d->color_smoothing = DT_DEMOSAIC_SMOOTH_OFF;
  }

  if(use_method & DT_DEMOSAIC_DUAL)
    d->color_smoothing = DT_DEMOSAIC_SMOOTH_OFF;

  d->demosaicing_method = (uint32_t)use_method;
}

/* ── init / init_pipe / cleanup_pipe ─────────────────────────────────────── */

static void init(dt_iop_module_t *self)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  dt_iop_demosaic_params_t *dp = (dt_iop_demosaic_params_t *)self->default_params;

  /* Detect sensor type from image metadata */
  dt_iop_demosaic_method_t method = DT_IOP_DEMOSAIC_PPG;
  if(self->dev)
  {
    dt_develop_t *dev = (dt_develop_t *)self->dev;
    const dt_image_t *img = &dev->image_storage;
    if(dt_image_is_monochrome(img))
    {
      method = dt_image_is_mono_sraw(img)
               ? DT_IOP_DEMOSAIC_MONO
               : DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
    }
    else if(img->buf_dsc.filters == 9u)
      method = DT_IOP_DEMOSAIC_MARKESTEIJN;
    else if(img->flags & DT_IMAGE_4BAYER)
      method = DT_IOP_DEMOSAIC_VNG4;
    else
      method = DT_IOP_DEMOSAIC_PPG;
  }

  /* Write into params buffers */
  if(p)
  {
    memset(p, 0, sizeof(*p));
    p->demosaicing_method = (int32_t)method;
    p->median_thrs    = 0.0f;
    p->dual_thrs      = 0.2f;
    p->cs_thrs        = 0.4f;
    p->cs_iter        = 8;
    p->lmmse_refine   = 1;
  }
  if(dp)
  {
    memset(dp, 0, sizeof(*dp));
    dp->demosaicing_method = (int32_t)method;
    dp->median_thrs    = 0.0f;
    dp->dual_thrs      = 0.2f;
    dp->cs_thrs        = 0.4f;
    dp->cs_iter        = 8;
    dp->lmmse_refine   = 1;
  }
}

static void init_pipe(dt_iop_module_t *self,
                      dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe;
  piece->data = calloc(1, sizeof(dt_iop_demosaic_data_t));
}

static void cleanup_pipe(dt_iop_module_t *self,
                         dt_dev_pixelpipe_t *pipe,
                         dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe;
  free(piece->data);
  piece->data = NULL;
}

/* ── Colorspace declarations ─────────────────────────────────────────────── */

static dt_iop_colorspace_type_t input_colorspace(dt_iop_module_t *self,
                                                  dt_dev_pixelpipe_t *pipe,
                                                  dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe; (void)piece;
  return IOP_CS_RAW;
}

static dt_iop_colorspace_type_t output_colorspace_fn(dt_iop_module_t *self,
                                                      dt_dev_pixelpipe_t *pipe,
                                                      dt_dev_pixelpipe_iop_t *piece)
{
  (void)self; (void)pipe; (void)piece;
  return IOP_CS_RGB;
}

/* ── Public registration entry point ─────────────────────────────────────── */

void dt_iop_demosaic_init_global(dt_iop_module_so_t *so)
{
  so->process_plain    = process;
  so->init             = init;
  so->init_pipe        = init_pipe;
  so->cleanup_pipe     = cleanup_pipe;
  so->commit_params    = commit_params;
  so->input_colorspace = input_colorspace;
  so->output_colorspace = output_colorspace_fn;
  so->output_format    = output_format;
  so->modify_roi_in    = modify_roi_in;
  so->modify_roi_out   = modify_roi_out;
}
