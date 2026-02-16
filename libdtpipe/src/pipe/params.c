/*
 * params.c - Parameter get/set implementation for libdtpipe
 *
 * Implements the public dtpipe_set_param_float(), dtpipe_set_param_int(),
 * dtpipe_get_param_float(), and dtpipe_enable_module() API functions.
 *
 * Architecture (Option B: hand-written descriptors)
 * ──────────────────────────────────────────────────
 * Each IOP module's params struct is described by a static table of
 * dt_param_desc_t entries (name, byte offset, type, size, soft min/max).
 *
 * The descriptor tables mirror the canonical darktable dt_iop_*_params_t
 * structs precisely — field order, sizes, and offsets must match the actual
 * compiled IOP code when those modules are eventually integrated.
 *
 * Currently covered modules (Tier 1 + key Tier 2):
 *   exposure, temperature, rawprepare, demosaic,
 *   colorin, colorout, highlights, sharpen
 *
 * To add a new module:
 *   1. Define a static dt_param_desc_t _params_<op>[] array below.
 *   2. Add a { "<op>", _params_<op>, ARRAY_LEN(_params_<op>) } entry to
 *      _module_param_tables[].
 *
 * Important: the byte offsets here must match the actual compiled structs.
 * They are derived from the darktable source and the Phase 0 JSON schemas.
 * Verified against darktable 5.0 source (git tag 5.0.0).
 */

#include "pipe/params.h"
#include "pipe/create.h"
#include "dtpipe.h"
#include "dtpipe_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Convenience macro ───────────────────────────────────────────────────── */

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/*
 * PARAM_FLOAT / PARAM_INT helpers.
 *
 * Usage: PARAM_FLOAT(struct_type, field_name, min, max)
 *
 * These compute the offset at compile time via offsetof() on a locally-
 * defined concrete struct so that the offsets are always accurate — even if
 * the struct definition later moves into a shared header.
 */
#define PARAM_F(st, field, lo, hi) \
  { #field, offsetof(st, field), DT_PARAM_FLOAT, sizeof(float),   (lo), (hi) }
#define PARAM_I(st, field, lo, hi) \
  { #field, offsetof(st, field), DT_PARAM_INT,   sizeof(int32_t), (lo), (hi) }
#define PARAM_U(st, field, lo, hi) \
  { #field, offsetof(st, field), DT_PARAM_UINT32,sizeof(uint32_t),(lo), (hi) }
#define PARAM_B(st, field) \
  { #field, offsetof(st, field), DT_PARAM_BOOL,  sizeof(int32_t),  0.0f, 1.0f }

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: exposure  (version 7)
 * darktable src/iop/exposure.c  dt_iop_exposure_params_t
 * ══════════════════════════════════════════════════════════════════════════*/

typedef enum _exposure_mode_t {
  EXPOSURE_MODE_MANUAL    = 0,
  EXPOSURE_MODE_DEFLICKER = 1,
} _exposure_mode_t;

typedef struct _exposure_params_t {
  int32_t mode;                    /* dt_exposure_mode_t */
  float   black;                   /* black point correction     [-1, 1]  */
  float   exposure;                /* EV adjustment              [-18,18] */
  float   deflicker_percentile;    /* deflicker percentile       [0,100]  */
  float   deflicker_target_level;  /* deflicker target EV        [-18,18] */
  int32_t compensate_exposure_bias;/* bool                                */
  int32_t compensate_hilite_pres;  /* bool                                */
} _exposure_params_t;

static const dt_param_desc_t _params_exposure[] = {
  PARAM_I(_exposure_params_t, mode,                    0.0f,   1.0f),
  PARAM_F(_exposure_params_t, black,                  -1.0f,   1.0f),
  PARAM_F(_exposure_params_t, exposure,              -18.0f,  18.0f),
  PARAM_F(_exposure_params_t, deflicker_percentile,    0.0f, 100.0f),
  PARAM_F(_exposure_params_t, deflicker_target_level,-18.0f,  18.0f),
  PARAM_B(_exposure_params_t, compensate_exposure_bias),
  PARAM_B(_exposure_params_t, compensate_hilite_pres),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: temperature  (version 4)
 * darktable src/iop/temperature.c  dt_iop_temperature_params_t
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct _temperature_params_t {
  float   red;     /* R multiplier [0, 8] */
  float   green;   /* G multiplier [0, 8] */
  float   blue;    /* B multiplier [0, 8] */
  float   various; /* 4th channel  [0, 8] */
  int32_t preset;  /* WB preset            */
} _temperature_params_t;

static const dt_param_desc_t _params_temperature[] = {
  PARAM_F(_temperature_params_t, red,     0.0f, 8.0f),
  PARAM_F(_temperature_params_t, green,   0.0f, 8.0f),
  PARAM_F(_temperature_params_t, blue,    0.0f, 8.0f),
  PARAM_F(_temperature_params_t, various, 0.0f, 8.0f),
  PARAM_I(_temperature_params_t, preset, -1.0f, 4.0f),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: rawprepare  (version 2)
 * darktable src/iop/rawprepare.c  dt_iop_rawprepare_params_t
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct _rawprepare_params_t {
  int32_t  left;                      /* crop left   */
  int32_t  top;                       /* crop top    */
  int32_t  right;                     /* crop right  */
  int32_t  bottom;                    /* crop bottom */
  uint16_t raw_black_level_separate[4]; /* per-CFA black level */
  uint32_t raw_white_point;           /* white point */
  int32_t  flat_field;                /* flat field mode */
} _rawprepare_params_t;

static const dt_param_desc_t _params_rawprepare[] = {
  PARAM_I(_rawprepare_params_t, left,            0.0f, 65535.0f),
  PARAM_I(_rawprepare_params_t, top,             0.0f, 65535.0f),
  PARAM_I(_rawprepare_params_t, right,           0.0f, 65535.0f),
  PARAM_I(_rawprepare_params_t, bottom,          0.0f, 65535.0f),
  PARAM_U(_rawprepare_params_t, raw_white_point, 0.0f, 65535.0f),
  PARAM_I(_rawprepare_params_t, flat_field,      0.0f,     1.0f),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: demosaic  (version 6)
 * darktable src/iop/demosaic.c  dt_iop_demosaic_params_t
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct _demosaic_params_t {
  int32_t green_eq;          /* green equalisation method   */
  float   median_thrs;       /* median filter threshold     */
  int32_t color_smoothing;   /* smoothing iterations        */
  int32_t demosaicing_method;/* algorithm enum              */
  int32_t lmmse_refine;      /* LMMSE refinement level      */
  float   dual_thrs;         /* dual demosaic threshold     */
  float   cs_radius;         /* capture sharpen radius      */
  float   cs_thrs;           /* capture sharpen threshold   */
  float   cs_boost;          /* capture sharpen boost       */
  int32_t cs_iter;           /* capture sharpen iterations  */
  float   cs_center;         /* capture sharpen center      */
  int32_t cs_enabled;        /* capture sharpening on/off   */
} _demosaic_params_t;

static const dt_param_desc_t _params_demosaic[] = {
  PARAM_I(_demosaic_params_t, green_eq,           0.0f,    3.0f),
  PARAM_F(_demosaic_params_t, median_thrs,        0.0f,    1.0f),
  PARAM_I(_demosaic_params_t, color_smoothing,    0.0f,    5.0f),
  PARAM_I(_demosaic_params_t, demosaicing_method, 0.0f, 3074.0f),
  PARAM_I(_demosaic_params_t, lmmse_refine,       0.0f,    4.0f),
  PARAM_F(_demosaic_params_t, dual_thrs,          0.0f,    1.0f),
  PARAM_F(_demosaic_params_t, cs_radius,          0.0f,    1.5f),
  PARAM_F(_demosaic_params_t, cs_thrs,            0.0f,    1.0f),
  PARAM_F(_demosaic_params_t, cs_boost,           0.0f,    1.5f),
  PARAM_I(_demosaic_params_t, cs_iter,            1.0f,   25.0f),
  PARAM_F(_demosaic_params_t, cs_center,          0.0f,    1.0f),
  PARAM_B(_demosaic_params_t, cs_enabled),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: colorin  (version 7)
 * darktable src/iop/colorin.c  dt_iop_colorin_params_t
 * Filenames are fixed-size char arrays (512 bytes each).
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct _colorin_params_t {
  int32_t type;               /* input profile type enum   */
  char    filename[512];      /* ICC filename              */
  int32_t intent;             /* rendering intent          */
  int32_t normalize;          /* gamut clipping method     */
  int32_t blue_mapping;       /* legacy blue mapping bool  */
  int32_t type_work;          /* working profile type      */
  char    filename_work[512]; /* working profile filename  */
} _colorin_params_t;

static const dt_param_desc_t _params_colorin[] = {
  PARAM_I(_colorin_params_t, type,         0.0f, 27.0f),
  PARAM_I(_colorin_params_t, intent,       0.0f,  3.0f),
  PARAM_I(_colorin_params_t, normalize,    0.0f,  4.0f),
  PARAM_B(_colorin_params_t, blue_mapping),
  PARAM_I(_colorin_params_t, type_work,    0.0f, 27.0f),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: colorout  (version 5)
 * darktable src/iop/colorout.c  dt_iop_colorout_params_t
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct _colorout_params_t {
  int32_t type;          /* output profile type enum */
  char    filename[512]; /* ICC filename             */
  int32_t intent;        /* rendering intent         */
} _colorout_params_t;

static const dt_param_desc_t _params_colorout[] = {
  PARAM_I(_colorout_params_t, type,   0.0f, 27.0f),
  PARAM_I(_colorout_params_t, intent, 0.0f,  3.0f),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: highlights  (version 4)
 * darktable src/iop/highlights.c  dt_iop_highlights_params_t
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct _highlights_params_t {
  int32_t mode;         /* reconstruction method     */
  float   blendL;       /* blend luminance (unused)  */
  float   blendC;       /* blend chroma (unused)     */
  float   strength;     /* reconstruction strength   */
  float   clip;         /* clipping threshold        */
  float   noise_level;  /* noise level               */
  int32_t iterations;   /* guided laplacian iters    */
  int32_t scales;       /* wavelet scales            */
  float   candidating;  /* segmentation threshold    */
  float   combine;      /* combine segments          */
  int32_t recovery;     /* rebuild mode              */
  float   solid_color;  /* inpaint flat color        */
} _highlights_params_t;

static const dt_param_desc_t _params_highlights[] = {
  PARAM_I(_highlights_params_t, mode,         0.0f,   5.0f),
  PARAM_F(_highlights_params_t, blendL,       0.0f,   1.0f),
  PARAM_F(_highlights_params_t, blendC,       0.0f,   1.0f),
  PARAM_F(_highlights_params_t, strength,     0.0f,   1.0f),
  PARAM_F(_highlights_params_t, clip,         0.0f,   2.0f),
  PARAM_F(_highlights_params_t, noise_level,  0.0f,   0.5f),
  PARAM_I(_highlights_params_t, iterations,   1.0f, 256.0f),
  PARAM_I(_highlights_params_t, scales,       0.0f,  11.0f),
  PARAM_F(_highlights_params_t, candidating,  0.0f,   1.0f),
  PARAM_F(_highlights_params_t, combine,      0.0f,   8.0f),
  PARAM_I(_highlights_params_t, recovery,     0.0f,   6.0f),
  PARAM_F(_highlights_params_t, solid_color,  0.0f,   1.0f),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: sharpen  (version 1)
 * darktable src/iop/sharpen.c  dt_iop_sharpen_params_t
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct _sharpen_params_t {
  float radius;     /* blur radius      [0, 99]  */
  float amount;     /* sharpening amount[0,  2]  */
  float threshold;  /* noise threshold  [0,100]  */
} _sharpen_params_t;

static const dt_param_desc_t _params_sharpen[] = {
  PARAM_F(_sharpen_params_t, radius,     0.0f,  99.0f),
  PARAM_F(_sharpen_params_t, amount,     0.0f,   2.0f),
  PARAM_F(_sharpen_params_t, threshold,  0.0f, 100.0f),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Master lookup table
 * ══════════════════════════════════════════════════════════════════════════*/

static const dt_module_param_table_t _module_param_tables[] = {
  { "exposure",    _params_exposure,    ARRAY_LEN(_params_exposure)    },
  { "temperature", _params_temperature, ARRAY_LEN(_params_temperature) },
  { "rawprepare",  _params_rawprepare,  ARRAY_LEN(_params_rawprepare)  },
  { "demosaic",    _params_demosaic,    ARRAY_LEN(_params_demosaic)    },
  { "colorin",     _params_colorin,     ARRAY_LEN(_params_colorin)     },
  { "colorout",    _params_colorout,    ARRAY_LEN(_params_colorout)    },
  { "highlights",  _params_highlights,  ARRAY_LEN(_params_highlights)  },
  { "sharpen",     _params_sharpen,     ARRAY_LEN(_params_sharpen)     },
};

static const int _module_param_tables_count =
  ARRAY_LEN(_module_param_tables);

/* ── dtpipe_lookup_param ─────────────────────────────────────────────────── */

const dt_param_desc_t *dtpipe_lookup_param(const char *op,
                                           const char *param_name)
{
  if(!op || !param_name)
    return NULL;

  for(int i = 0; i < _module_param_tables_count; i++)
  {
    if(strncmp(_module_param_tables[i].op, op, 20) != 0)
      continue;

    const dt_module_param_table_t *tbl = &_module_param_tables[i];
    for(int j = 0; j < tbl->count; j++)
    {
      if(strcmp(tbl->params[j].name, param_name) == 0)
        return &tbl->params[j];
    }
    /* Module found but param not in table */
    return NULL;
  }
  return NULL;
}

/* ── dtpipe_param_count ──────────────────────────────────────────────────── */

int dtpipe_param_count(const char *op)
{
  if(!op)
    return -1;

  for(int i = 0; i < _module_param_tables_count; i++)
  {
    if(strncmp(_module_param_tables[i].op, op, 20) == 0)
      return _module_param_tables[i].count;
  }
  return -1;
}

/* ── dtpipe_get_param_desc ───────────────────────────────────────────────── */

const dt_param_desc_t *dtpipe_get_param_desc(const char *op, int i)
{
  if(!op || i < 0)
    return NULL;

  for(int t = 0; t < _module_param_tables_count; t++)
  {
    if(strncmp(_module_param_tables[t].op, op, 20) == 0)
    {
      if(i >= _module_param_tables[t].count)
        return NULL;
      return &_module_param_tables[t].params[i];
    }
  }
  return NULL;
}

/* ── dtpipe_params_struct_size ───────────────────────────────────────────── */

size_t dtpipe_params_struct_size(const char *op)
{
  if(!op)
    return 0;

  for(int t = 0; t < _module_param_tables_count; t++)
  {
    if(strncmp(_module_param_tables[t].op, op, 20) != 0)
      continue;

    size_t max_end = 0;
    const dt_module_param_table_t *tbl = &_module_param_tables[t];
    for(int i = 0; i < tbl->count; i++)
    {
      size_t end = tbl->params[i].offset + tbl->params[i].size;
      if(end > max_end)
        max_end = end;
    }
    return max_end;
  }
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API implementation
 * ══════════════════════════════════════════════════════════════════════════*/

/* ── dtpipe_set_param_float ──────────────────────────────────────────────── */

int dtpipe_set_param_float(dt_pipe_t *pipe, const char *module_name,
                           const char *param, float value)
{
  if(!pipe || !module_name || !param)
    return DTPIPE_ERR_INVALID_ARG;

  dt_iop_module_t *m = dtpipe_find_module(pipe, module_name);
  if(!m)
    return DTPIPE_ERR_NOT_FOUND;

  if(!m->params)
    return DTPIPE_ERR_NOT_FOUND;

  const dt_param_desc_t *desc = dtpipe_lookup_param(module_name, param);
  if(!desc)
    return DTPIPE_ERR_NOT_FOUND;

  if(desc->type != DT_PARAM_FLOAT)
    return DTPIPE_ERR_PARAM_TYPE;

  /* Soft bounds warning */
  if(value < desc->min || value > desc->max)
    fprintf(stderr,
            "[dtpipe/params] warning: %s.%s = %g is outside [%g, %g]\n",
            module_name, param, (double)value,
            (double)desc->min, (double)desc->max);

  float *dst = (float *)((uint8_t *)m->params + desc->offset);
  *dst = value;
  return DTPIPE_OK;
}

/* ── dtpipe_set_param_int ────────────────────────────────────────────────── */

int dtpipe_set_param_int(dt_pipe_t *pipe, const char *module_name,
                         const char *param, int value)
{
  if(!pipe || !module_name || !param)
    return DTPIPE_ERR_INVALID_ARG;

  dt_iop_module_t *m = dtpipe_find_module(pipe, module_name);
  if(!m)
    return DTPIPE_ERR_NOT_FOUND;

  if(!m->params)
    return DTPIPE_ERR_NOT_FOUND;

  const dt_param_desc_t *desc = dtpipe_lookup_param(module_name, param);
  if(!desc)
    return DTPIPE_ERR_NOT_FOUND;

  if(desc->type != DT_PARAM_INT &&
     desc->type != DT_PARAM_UINT32 &&
     desc->type != DT_PARAM_BOOL)
    return DTPIPE_ERR_PARAM_TYPE;

  /* Soft bounds warning */
  if((float)value < desc->min || (float)value > desc->max)
    fprintf(stderr,
            "[dtpipe/params] warning: %s.%s = %d is outside [%g, %g]\n",
            module_name, param, value,
            (double)desc->min, (double)desc->max);

  if(desc->type == DT_PARAM_UINT32)
  {
    uint32_t *dst = (uint32_t *)((uint8_t *)m->params + desc->offset);
    *dst = (uint32_t)value;
  }
  else
  {
    int32_t *dst = (int32_t *)((uint8_t *)m->params + desc->offset);
    *dst = (int32_t)value;
  }
  return DTPIPE_OK;
}

/* ── dtpipe_get_param_float ──────────────────────────────────────────────── */

int dtpipe_get_param_float(dt_pipe_t *pipe, const char *module_name,
                           const char *param, float *out)
{
  if(!pipe || !module_name || !param || !out)
    return DTPIPE_ERR_INVALID_ARG;

  dt_iop_module_t *m = dtpipe_find_module(pipe, module_name);
  if(!m)
    return DTPIPE_ERR_NOT_FOUND;

  if(!m->params)
    return DTPIPE_ERR_NOT_FOUND;

  const dt_param_desc_t *desc = dtpipe_lookup_param(module_name, param);
  if(!desc)
    return DTPIPE_ERR_NOT_FOUND;

  if(desc->type != DT_PARAM_FLOAT)
    return DTPIPE_ERR_PARAM_TYPE;

  const float *src = (const float *)((const uint8_t *)m->params + desc->offset);
  *out = *src;
  return DTPIPE_OK;
}

/* ── dtpipe_enable_module ────────────────────────────────────────────────── */

int dtpipe_enable_module(dt_pipe_t *pipe, const char *module_name, int enabled)
{
  if(!pipe || !module_name)
    return DTPIPE_ERR_INVALID_ARG;

  dt_iop_module_t *m = dtpipe_find_module(pipe, module_name);
  if(!m)
    return DTPIPE_ERR_NOT_FOUND;

  m->enabled = (enabled != 0);
  return DTPIPE_OK;
}

/* ── dtpipe_is_module_enabled ───────────────────────────────────────────── */

int dtpipe_is_module_enabled(dt_pipe_t *pipe, const char *module_name, int *out)
{
  if(!pipe || !module_name || !out)
    return DTPIPE_ERR_INVALID_ARG;

  dt_iop_module_t *m = dtpipe_find_module(pipe, module_name);
  if(!m)
    return DTPIPE_ERR_NOT_FOUND;

  *out = m->enabled ? 1 : 0;
  return DTPIPE_OK;
}
