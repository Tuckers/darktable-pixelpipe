/*
    This file is part of darktable,
    Copyright (C) 2011-2024 darktable developers.

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
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/colorspaces.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/openmp_maths.h"
#include "iop/iop_api.h"
#include "libs/colorpicker.h"

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(5)
// special marker value for uninitialized (and thus invalid) levels.  Use this in preference
// to NAN so that we can enable optimizations from -ffinite-math-only.
#define DT_LEVELS_UNINIT (-FLT_MAX)

DT_MODULE_INTROSPECTION(2, dt_iop_levels_params_t)

//static void dt_iop_levels_mode_callback(GtkWidget *combo, gpointer user_data);
//static void dt_iop_levels_percentiles_callback(GtkWidget *slider, gpointer user_data);

typedef enum dt_iop_levels_mode_t
{
  LEVELS_MODE_MANUAL,   // $DESCRIPTION: "manual"
  LEVELS_MODE_AUTOMATIC // $DESCRIPTION: "automatic"
} dt_iop_levels_mode_t;

typedef struct dt_iop_levels_params_t
{
  dt_iop_levels_mode_t mode; // $DEFAULT: LEVELS_MODE_MANUAL
  float black; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0
  float gray;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0
  float white; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0
  float levels[3];
} dt_iop_levels_params_t;


typedef struct dt_iop_levels_data_t
{
  dt_iop_levels_mode_t mode;
  float percentiles[3];
  float levels[3];
  float in_inv_gamma;
  float lut[0x10000];
} dt_iop_levels_data_t;

typedef struct dt_iop_levels_global_data_t
{
  int kernel_levels;
} dt_iop_levels_global_data_t;


const char *deprecated_msg()
{
  return _("this module is deprecated. please use the RGB levels module instead.");
}

const char *name()
{
  return _("levels");
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_GRADING;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_DEPRECATED;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("adjust black, white and mid-gray points"),
                                      _("creative"),
                                      _("linear or non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_levels_params_v2_t
  {
    dt_iop_levels_mode_t mode;
    float black;
    float gray;
    float white;
    float levels[3];
  } dt_iop_levels_params_v2_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_levels_params_v1_t
    {
      float levels[3];
      int levels_preset;
    } dt_iop_levels_params_v1_t;

    const dt_iop_levels_params_v1_t *o = (dt_iop_levels_params_v1_t *)old_params;
    dt_iop_levels_params_v2_t *n = malloc(sizeof(dt_iop_levels_params_v2_t));

    n->mode = LEVELS_MODE_MANUAL;
    n->black = 0.0f;
    n->gray = 50.0f;
    n->white = 100.0f;
    n->levels[0] = o->levels[0];
    n->levels[1] = o->levels[1];
    n->levels[2] = o->levels[2];

    *new_params = n;
    *new_params_size = sizeof(dt_iop_levels_params_v2_t);
    *new_version = 2;
    return 0;
  }
  return 1;
}


static void dt_iop_levels_compute_levels_automatic(dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_levels_data_t *d = piece->data;

  uint32_t total = piece->histogram_stats.pixels;

  dt_aligned_pixel_t thr;
  for(int k = 0; k < 3; k++)
  {
    thr[k] = (float)total * d->percentiles[k] / 100.0f;
    d->levels[k] = DT_LEVELS_UNINIT;
  }

  if(piece->histogram == NULL) return;

  // find min and max levels
  size_t n = 0;
  for(uint32_t i = 0; i < piece->histogram_stats.bins_count; i++)
  {
    n += piece->histogram[4 * i];

    for(int k = 0; k < 3; k++)
    {
      if(d->levels[k] == DT_LEVELS_UNINIT && (n >= thr[k]))
      {
        d->levels[k] = (float)i / (float)(piece->histogram_stats.bins_count - 1);
      }
    }
  }
  // for numerical reasons sometimes the threshold is sharp but in float and n is size_t.
  // in this case we want to make sure we don't keep the marker that it is uninitialized:
  if(d->levels[2] == DT_LEVELS_UNINIT)
    d->levels[2] = 1.0f;

  // compute middle level from min and max levels
  float center = d->percentiles[1] / 100.0f;
  if(d->levels[0] != DT_LEVELS_UNINIT && d->levels[2] != DT_LEVELS_UNINIT)
    d->levels[1] = (1.0f - center) * d->levels[0] + center * d->levels[2];
}

static void compute_lut(dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_levels_data_t *d = piece->data;

  // Building the lut for values in the [0,1] range
  float delta = (d->levels[2] - d->levels[0]) / 2.0f;
  float mid = d->levels[0] + delta;
  float tmp = (d->levels[1] - mid) / delta;
  d->in_inv_gamma = pow(10, tmp);

  for(unsigned int i = 0; i < 0x10000; i++)
  {
    float percentage = (float)i / (float)0x10000ul;
    d->lut[i] = 100.0f * powf(percentage, d->in_inv_gamma);
  }
}


/*
 * WARNING: unlike commit_params, which is thread safe wrt gui thread and
 * pipes, this function lives in the pipeline thread, and NOT thread safe!
 */
static void commit_params_late(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_levels_data_t *d = piece->data;

  if(d->mode == LEVELS_MODE_AUTOMATIC)
  {

    if((piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
       || d->levels[0] == DT_LEVELS_UNINIT || d->levels[1] == DT_LEVELS_UNINIT
       || d->levels[2] == DT_LEVELS_UNINIT)
    {
      dt_iop_levels_compute_levels_automatic(piece);
      compute_lut(piece);
    }

  }
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return;
  const dt_iop_levels_data_t *const d = piece->data;

  if(d->mode == LEVELS_MODE_AUTOMATIC)
  {
    commit_params_late(self, piece);
  }

  const float *const restrict in = (float*)ivoid;
  float *const restrict out = (float*)ovoid;
  const size_t npixels = (size_t)roi_out->width * roi_out->height;
  const float level_black = d->levels[0];
  const float level_range = d->levels[2] - d->levels[0];
  const float inv_gamma = d->in_inv_gamma;
  const float *lut = d->lut;

  DT_OMP_FOR()
  for(int i = 0; i < 4 * npixels; i += 4)
  {
    const float L_in = in[i] / 100.0f;
    float L_out;
    if(L_in <= level_black)
    {
      // Anything below the lower threshold just clips to zero
      L_out = 0.0f;
    }
    else
    {
      const float percentage = (L_in - level_black) / level_range;
      // Within the expected input range we can use the lookup table, else we need to compute from scratch
      L_out = percentage < 1.0f ? lut[(int)(percentage * 0x10000ul)] : 100.0f * powf(percentage, inv_gamma);
    }

    // Preserving contrast
    const float denom = (in[i] > 0.01f) ? in[i] : 0.01f;
    out[i] = L_out;
    out[i+1] = in[i+1] * L_out / denom;
    out[i+2] = in[i+2] * L_out / denom;
  }
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_levels_data_t *d = piece->data;
  dt_iop_levels_global_data_t *gd = self->global_data;

  if(d->mode == LEVELS_MODE_AUTOMATIC)
  {
    commit_params_late(self, piece);
  }

  cl_mem dev_lut = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  dev_lut = dt_opencl_copy_host_to_device(devid, d->lut, 256, 256, sizeof(float));
  if(dev_lut == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_levels, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(dev_lut), CLARG(d->levels[0]),
    CLARG(d->levels[2]), CLARG(d->in_inv_gamma));

error:
  dt_opencl_release_mem_object(dev_lut);
  return err;
}
#endif

// void init_presets (dt_iop_module_so_t *self)
//{
//  dt_iop_levels_params_t p;
//  p.levels_preset = 0;
//
//  p.levels[0] = 0;
//  p.levels[1] = 0.5;
//  p.levels[2] = 1;
//  dt_gui_presets_add_generic(_("unmodified"), self->op, self->version(), &p, sizeof(p), 1);
//}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_levels_data_t *d = piece->data;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)p1;

  if(pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    piece->request_histogram |= DT_REQUEST_ON;
  else
    piece->request_histogram &= ~DT_REQUEST_ON;

  piece->request_histogram |= DT_REQUEST_ONLY_IN_GUI;

  piece->histogram_params.bins_count = 256;

  if(p->mode == LEVELS_MODE_AUTOMATIC)
  {
    d->mode = LEVELS_MODE_AUTOMATIC;

    piece->request_histogram |= DT_REQUEST_ON;
    self->request_histogram &= ~DT_REQUEST_ON;

    if(!self->dev->gui_attached) piece->request_histogram &= ~DT_REQUEST_ONLY_IN_GUI;

    piece->histogram_params.bins_count = 16384;

    /*
     * in principle, we do not need/want histogram in FULL pipe
     * because we will use histogram from preview pipe there,
     * but it might happen that for some reasons we do not have
     * histogram of preview pipe yet - e.g. on first pipe run
     * (just after setting mode to automatic)
     */

    d->percentiles[0] = p->black;
    d->percentiles[1] = p->gray;
    d->percentiles[2] = p->white;

    d->levels[0] = DT_LEVELS_UNINIT;
    d->levels[1] = DT_LEVELS_UNINIT;
    d->levels[2] = DT_LEVELS_UNINIT;

    // commit_params_late() will compute LUT later
  }
  else
  {
    d->mode = LEVELS_MODE_MANUAL;

    self->request_histogram |= DT_REQUEST_ON;

    d->levels[0] = p->levels[0];
    d->levels[1] = p->levels[1];
    d->levels[2] = p->levels[2];
    compute_lut(piece);
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_levels_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  free(piece->data);
  piece->data = NULL;
}


void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  self->request_histogram |= DT_REQUEST_ON;

  dt_iop_levels_params_t *d = self->default_params;

  d->levels[0] = 0.0f;
  d->levels[1] = 0.5f;
  d->levels[2] = 1.0f;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_levels_global_data_t *gd = malloc(sizeof(dt_iop_levels_global_data_t));
  self->data = gd;
  gd->kernel_levels = dt_opencl_create_kernel(program, "levels");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_levels_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_levels);
  free(self->data);
  self->data = NULL;
}


/**
 * Move handler_move to new_pos, storing the value in handles,
 * while keeping new_pos within a valid range
 * and preserving the ratio between the three handles.
 *
 * @param self Pointer to this module to be able to access gui_data
 * @param handle_move Handle to move
 * @param new_pow New position (0..1)
 * @param levels Pointer to dt_iop_levels_params->levels.
 * @param drag_start_percentage Ratio between handle 1, 2 and 3.
 *
 * @return TRUE if the marker were given a new position. FALSE otherwise.
 */


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
