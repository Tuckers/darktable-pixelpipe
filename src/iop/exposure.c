/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
/* NOTE: This file has been extracted from darktable for the pixelpipe
 * extraction project. GUI-related code has been removed using
 * scripts/strip_iop.py. Only image processing logic, parameter structs,
 * and pipeline functions are retained.
 */


#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/histogram.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"
#include "iop/iop_api.h"

#define exposure2white(x) exp2f(-(x))
#define white2exposure(x) -dt_log2f(fmaxf(1e-20f, x))

DT_MODULE_INTROSPECTION(7, dt_iop_exposure_params_t)

typedef enum dt_iop_exposure_mode_t
{
  EXPOSURE_MODE_MANUAL,   // $DESCRIPTION: "manual"
  EXPOSURE_MODE_DEFLICKER // $DESCRIPTION: "automatic"
} dt_iop_exposure_mode_t;

typedef enum dt_spot_mode_t
{
  DT_SPOT_MODE_CORRECT = 0,
  DT_SPOT_MODE_MEASURE = 1,
  DT_SPOT_MODE_LAST
} dt_spot_mode_t;

// uint16_t pixel can have any value in range [0, 65535], thus, there is
// 65536 possible values.
#define DEFLICKER_BINS_COUNT (UINT16_MAX + 1)

typedef struct dt_iop_exposure_params_t
{
  dt_iop_exposure_mode_t mode;      // $DEFAULT: EXPOSURE_MODE_MANUAL
  float black;                      // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "black level correction"
  float exposure;                   // $MIN: -18.0 $MAX: 18.0 $DEFAULT: 0.0
  float deflicker_percentile;       // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0 $DESCRIPTION: "percentile"
  float deflicker_target_level;     // $MIN: -18.0 $MAX: 18.0 $DEFAULT: -4.0 $DESCRIPTION: "target level"
  gboolean compensate_exposure_bias;// $DEFAULT: FALSE $DESCRIPTION: "compensate exposure bias"
  gboolean compensate_hilite_pres;  // $DEFAULT: TRUE $DESCRIPTION: "compensate highlight preservation"
} dt_iop_exposure_params_t;


typedef struct dt_iop_exposure_data_t
{
  dt_iop_exposure_params_t params;
  int deflicker;
  float black;
  float scale;
} dt_iop_exposure_data_t;

typedef struct dt_iop_exposure_global_data_t
{
  int kernel_exposure;
} dt_iop_exposure_global_data_t;

#define EXPOSURE_CORRECTION_UNDEFINED (-FLT_MAX)

const char *name()
{
  return _("exposure");
}

const char** description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self,
     _("redo the exposure of the shot as if you were still in-camera\n"
       "using a color-safe brightening similar to increasing ISO setting"),
     _("corrective and creative"),
     _("linear, RGB, scene-referred"),
     _("linear, RGB"),
     _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}


int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_exposure_params_v6_t
  {
    dt_iop_exposure_mode_t mode;
    float black;
    float exposure;
    float deflicker_percentile;
    float deflicker_target_level;
    gboolean compensate_exposure_bias;
  } dt_iop_exposure_params_v6_t;

  typedef struct dt_iop_exposure_params_v7_t
  {
    dt_iop_exposure_mode_t mode;
    float black;
    float exposure;
    float deflicker_percentile;
    float deflicker_target_level;
    gboolean compensate_exposure_bias;
    gboolean compensate_hilite_pres;
  } dt_iop_exposure_params_v7_t;

  if(old_version == 2)
  {
    typedef struct dt_iop_exposure_params_v2_t
    {
      float black, exposure, gain;
    } dt_iop_exposure_params_v2_t;

    const dt_iop_exposure_params_v2_t *o = (dt_iop_exposure_params_v2_t *)old_params;
    dt_iop_exposure_params_v6_t *n = malloc(sizeof(dt_iop_exposure_params_v6_t));

    n->mode = EXPOSURE_MODE_MANUAL;
    n->black = o->black;
    n->exposure = o->exposure;
    n->compensate_exposure_bias = FALSE;
    n->deflicker_percentile = 50.0f;
    n->deflicker_target_level = -4.0f;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_exposure_params_v6_t);
    *new_version = 6;
    return 0;
  }
  if(old_version == 3)
  {
    typedef struct dt_iop_exposure_params_v3_t
    {
      float black, exposure;
      gboolean deflicker;
      float deflicker_percentile, deflicker_target_level;
    } dt_iop_exposure_params_v3_t;

    const dt_iop_exposure_params_v3_t *o = (dt_iop_exposure_params_v3_t *)old_params;
    dt_iop_exposure_params_v6_t *n = malloc(sizeof(dt_iop_exposure_params_v6_t));

    n->mode = o->deflicker ? EXPOSURE_MODE_DEFLICKER : EXPOSURE_MODE_MANUAL;
    n->black = o->black;
    n->exposure = o->exposure;
    n->deflicker_percentile = o->deflicker_percentile;
    n->deflicker_target_level = o->deflicker_target_level;
    n->compensate_exposure_bias = FALSE;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_exposure_params_v6_t);
    *new_version = 6;
    return 0;
  }
  if(old_version == 4)
  {
    typedef enum dt_iop_exposure_deflicker_histogram_source_t {
      DEFLICKER_HISTOGRAM_SOURCE_THUMBNAIL,
      DEFLICKER_HISTOGRAM_SOURCE_SOURCEFILE
    } dt_iop_exposure_deflicker_histogram_source_t;

    typedef struct dt_iop_exposure_params_v4_t
    {
      dt_iop_exposure_mode_t mode;
      float black;
      float exposure;
      float deflicker_percentile, deflicker_target_level;
      dt_iop_exposure_deflicker_histogram_source_t deflicker_histogram_source;
    } dt_iop_exposure_params_v4_t;

    const dt_iop_exposure_params_v4_t *o = (dt_iop_exposure_params_v4_t *)old_params;
    dt_iop_exposure_params_v6_t *n = malloc(sizeof(dt_iop_exposure_params_v6_t));

    n->mode = o->mode;
    n->black = o->black;
    n->exposure = o->exposure;
    n->deflicker_percentile = o->deflicker_percentile;
    n->deflicker_target_level = o->deflicker_target_level;
    // deflicker_histogram_source is dropped. this does change output,
    // but deflicker still was not publicly released at that point
    n->compensate_exposure_bias = FALSE;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_exposure_params_v6_t);
    *new_version = 6;
    return 0;
  }
  if(old_version == 5)
  {
    typedef struct dt_iop_exposure_params_v5_t
    {
      dt_iop_exposure_mode_t mode;
      float black;
      float exposure;
      float deflicker_percentile, deflicker_target_level;
    } dt_iop_exposure_params_v5_t;

    const dt_iop_exposure_params_v5_t *o = (dt_iop_exposure_params_v5_t *)old_params;
    dt_iop_exposure_params_v6_t *n = malloc(sizeof(dt_iop_exposure_params_v6_t));

    n->mode = o->mode;
    n->black = o->black;
    n->exposure = o->exposure;
    n->deflicker_percentile = o->deflicker_percentile;
    n->deflicker_target_level = o->deflicker_target_level;
    n->compensate_exposure_bias = FALSE;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_exposure_params_v6_t);
    *new_version = 6;
    return 0;
  }
  if(old_version == 6)
  {
    const dt_iop_exposure_params_v6_t *o = (dt_iop_exposure_params_v6_t *)old_params;
    dt_iop_exposure_params_v7_t *n = malloc(sizeof(dt_iop_exposure_params_v7_t));

    n->mode = o->mode;
    n->black = o->black;
    n->exposure = o->exposure;
    n->deflicker_percentile = o->deflicker_percentile;
    n->deflicker_target_level = o->deflicker_target_level;
    n->compensate_exposure_bias = o->compensate_exposure_bias;
    n->compensate_hilite_pres = FALSE;	// module did not compensate h.p. before version 7

    *new_params = n;
    *new_params_size = sizeof(dt_iop_exposure_params_v7_t);
    *new_version = 7;
    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  self->pref_based_presets = TRUE;

  dt_gui_presets_add_generic
    (_("magic lantern defaults"), self->op,
     self->version(),
     &(dt_iop_exposure_params_t){.mode = EXPOSURE_MODE_DEFLICKER,
                                 .black = 0.0f,
                                 .exposure = 0.0f,
                                 .deflicker_percentile = 50.0f,
                                 .deflicker_target_level = -4.0f,
                                 .compensate_exposure_bias = FALSE,
                                 .compensate_hilite_pres = FALSE },
     sizeof(dt_iop_exposure_params_t), TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  const gboolean is_scene_referred = dt_is_scene_referred();

  if(is_scene_referred)
  {
    // For scene-referred workflow, since filmic doesn't brighten as base curve does,
    // we need an initial exposure boost. This preset has the same value as what is
    // auto-applied (see reload_default below) for scene-referred workflow.
    dt_gui_presets_add_generic
      (_("scene-referred default"), self->op, self->version(),
       NULL, 0,
       TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

    dt_gui_presets_update_format(BUILTIN_PRESET("scene-referred default"), self->op,
                                 self->version(), FOR_RAW);

    dt_gui_presets_update_autoapply(BUILTIN_PRESET("scene-referred default"),
                                    self->op, self->version(), TRUE);
  }
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *d = self->default_params;

  const gboolean scene_raw =
     dt_image_is_rawprepare_supported(&self->dev->image_storage)
     && dt_is_scene_referred();

  d->mode = EXPOSURE_MODE_MANUAL;

  if(scene_raw && self->multi_priority == 0)
  {
    const gboolean mono = dt_image_is_monochrome(&self->dev->image_storage);
    d->exposure = mono ? 0.0f : 0.7f;
    d->black =    mono ? 0.0f : -0.000244140625f;
    d->compensate_exposure_bias = TRUE;
  }
  else
  {
    d->exposure = 0.0f;
    d->black = 0.0f;
    d->compensate_exposure_bias = FALSE;
  }
  // the new default is to compensate for highlight preservation mode,
  // but ONLY if we're the first instance (to avoid multiple application)
  d->compensate_hilite_pres = dt_iop_is_first_instance(self->dev->iop, self);
}

static void _deflicker_prepare_histogram(dt_iop_module_t *self,
                                         uint32_t **histogram,
                                         dt_dev_histogram_stats_t *histogram_stats)
{
  const dt_image_t *img = dt_image_cache_get(self->dev->image_storage.id, 'r');
  dt_image_t image = *img;
  dt_image_cache_read_release(img);

  if(!img || image.buf_dsc.channels != 1 || image.buf_dsc.datatype != TYPE_UINT16) return;

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(&buf, self->dev->image_storage.id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  if(!buf.buf)
  {
    dt_control_log(_("failed to get raw buffer from image `%s'"), image.filename);
    dt_mipmap_cache_release(&buf);
    return;
  }

  dt_dev_histogram_collection_params_t histogram_params = { 0 };

  dt_histogram_roi_t histogram_roi = {.width = image.width,
                                      .height = image.height,

                                      // FIXME: get those from rawprepare IOP somehow !!!
                                      .crop_x = image.crop_x,
                                      .crop_y = image.crop_y,
                                      .crop_right = image.crop_right,
                                      .crop_bottom = image.crop_bottom };

  histogram_params.roi = &histogram_roi;
  histogram_params.bins_count = DEFLICKER_BINS_COUNT;

  dt_histogram_helper(&histogram_params, histogram_stats, IOP_CS_RAW, IOP_CS_NONE,
                      buf.buf, histogram, NULL, FALSE, NULL);

  dt_mipmap_cache_release(&buf);
}

/* input: 0 - 65535 (valid range: from black level to white level) */
/* output: -16 ... 0 */
static double _raw_to_ev(const uint32_t raw,
                         const uint32_t black_level,
                         const uint32_t white_level)
{
  const uint32_t raw_max = white_level - black_level;

  // we are working on data without black clipping,
  // so we can get values which are lower than the black level !!!
  const int64_t raw_val = MAX((int64_t)raw - (int64_t)black_level, 1);

  const double raw_ev = -log2(raw_max) + log2(raw_val);

  return raw_ev;
}

static void _compute_correction(dt_iop_module_t *self,
                                dt_iop_exposure_params_t *p,
                                dt_dev_pixelpipe_t *pipe,
                                const uint32_t *const histogram,
                                const dt_dev_histogram_stats_t *const histogram_stats,
                                float *correction)
{
  *correction = EXPOSURE_CORRECTION_UNDEFINED;

  if(histogram == NULL) return;

  const double thr
      = CLAMP(((double)histogram_stats->pixels * (double)p->deflicker_percentile
               / (double)100.0), 0.0, (double)histogram_stats->pixels);

  size_t n = 0;
  uint32_t raw = 0;

  for(size_t i = 0; i < histogram_stats->bins_count; i++)
  {
    n += histogram[i];

    if((double)n >= thr)
    {
      raw = i;
      break;
    }
  }

  const double ev
      = _raw_to_ev(raw, (uint32_t)pipe->dsc.rawprepare.raw_black_level,
                   pipe->dsc.rawprepare.raw_white_point);

  *correction = p->deflicker_target_level - ev;
}


static void _process_common_setup(dt_iop_module_t *self,
                                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_data_t *d = piece->data;

  d->black = d->params.black;
  float exposure = d->params.exposure;

  if(d->deflicker)
  {
    uint32_t *histogram = NULL;
    dt_dev_histogram_stats_t histogram_stats;
    _deflicker_prepare_histogram(self, &histogram, &histogram_stats);
    _compute_correction(self, &d->params, piece->pipe, histogram,
                      &histogram_stats, &exposure);
    dt_free_align(histogram);

    // second, show computed correction in UI.
  }

  const float white = exposure2white(exposure);
  d->scale = 1.0 / (white - d->black);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_exposure_data_t *d = piece->data;
  dt_iop_exposure_global_data_t *gd = self->global_data;

  _process_common_setup(self, piece);

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_exposure, width, height,
                                         CLARG(dev_in), CLARG(dev_out),
                                         CLARG(width), CLARG(height),
                                         CLARG((d->black)), CLARG((d->scale)));
  if(err != CL_SUCCESS) goto error;
  for(int k = 0; k < 3; k++) piece->pipe->dsc.processed_maximum[k] *= d->scale;

error:
  return err;
}
#endif

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const i,
             void *const o,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_exposure_data_t *const d = piece->data;

  _process_common_setup(self, piece);

  const int ch = piece->colors;

  const float *const restrict in = (float*)i;
  float *const restrict out = (float*)o;
  const float black = d->black;
  const float scale = d->scale;
  const size_t npixels = (size_t)roi_out->width * roi_out->height;
  DT_OMP_FOR_SIMD(aligned(in, out : 64))
  for(size_t k = 0; k < ch * npixels; k++)
  {
    out[k] = (in[k] - black) * scale;
  }
  for(int k = 0; k < 3; k++)
    piece->pipe->dsc.processed_maximum[k] *= d->scale;
}


static float _get_exposure_bias(const dt_iop_module_t *self)
{
  float bias = 0.0f;

  // just check that pointers exist and are initialized
  if(self->dev && self->dev->image_storage.exif_exposure_bias)
    bias = self->dev->image_storage.exif_exposure_bias;

  // sanity checks, don't trust exif tags too much
  if(bias != DT_EXIF_TAG_UNINITIALIZED)
    return CLAMP(bias, -5.0f, 5.0f);
  else
    return 0.0f;
}

static float _get_highlight_bias(const dt_iop_module_t *self)
{
  float bias = 0.0f;

  // Nikon: Exif.Nikon3.Colorspace==4  --> +2 EV
  // Fuji:  Exif.Fujifilm.DevelopmentDynamicRange
  //             100 --> no comp
  //             200 --> +1 EV
  //             400 --> +2 EV

  if(self->dev && self->dev->image_storage.exif_highlight_preservation > 0.0f)
    bias = self->dev->image_storage.exif_highlight_preservation;

  // sanity checks, don't trust exif tags too much
  if(bias != DT_EXIF_TAG_UNINITIALIZED)
    return CLAMP(bias, -1.0f, 4.0f);
  else
    return 0.0f;
}


void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)p1;
  dt_iop_exposure_data_t *d = piece->data;

  d->params.black = p->black;
  d->params.exposure = p->exposure;
  d->params.deflicker_percentile = p->deflicker_percentile;
  d->params.deflicker_target_level = p->deflicker_target_level;

  // If exposure bias compensation has been required, add it on top of
  // user exposure correction
  if(p->compensate_exposure_bias)
    d->params.exposure -= _get_exposure_bias(self);

  // If highlight preservation compensation has been required, add it on top of
  // the previous compensation values
//  d->params.compensate_hilite_pres = p->compensate_hilite_pres;
  if(p->compensate_hilite_pres)
    d->params.exposure += _get_highlight_bias(self);

  d->deflicker = 0;


  if(p->mode == EXPOSURE_MODE_DEFLICKER
     && dt_image_is_raw(&self->dev->image_storage)
     && self->dev->image_storage.buf_dsc.channels == 1
     && self->dev->image_storage.buf_dsc.datatype == TYPE_UINT16)
  {
    d->deflicker = 1;
  }
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1,sizeof(dt_iop_exposure_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // from programs.conf: basic.cl
  dt_iop_exposure_global_data_t *gd = calloc(1,sizeof(dt_iop_exposure_global_data_t));
  self->data = gd;
  gd->kernel_exposure = dt_opencl_create_kernel(program, "exposure");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_exposure_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_exposure);
  free(self->data);
  self->data = NULL;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
