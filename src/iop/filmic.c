/*
   This file is part of darktable,
   Copyright (C) 2018-2025 darktable developers.

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

#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "develop/openmp_maths.h"
#include "iop/iop_api.h"


#include "develop/imageop.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)


DT_MODULE_INTROSPECTION(3, dt_iop_filmic_params_t)

/**
 * DOCUMENTATION
 *
 * This code ports :
 * 1. Troy Sobotka's filmic curves for Blender (and other softs)
 *      https://github.com/sobotka/OpenAgX/blob/master/lib/agx_colour.py
 * 2. ACES camera logarithmic encoding
 *        https://github.com/ampas/aces-dev/blob/master/transforms/ctl/utilities/ACESutil.Lin_to_Log2_param.ctl
 *
 * The ACES log implementation is taken from the profile_gamma.c IOP
 * where it works in camera RGB space. Here, it works on an arbitrary RGB
 * space. ProPhotoRGB has been chosen for its wide gamut coverage and
 * for conveniency because it's already in darktable's libs. Any other
 * RGB working space could work. This chouice could (should) also be
 * exposed to the user.
 *
 * The filmic curves are tonecurves intended to simulate the luminance
 * transfer function of film with "S" curves. These could be reproduced in
 * the tonecurve.c IOP, however what we offer here is a parametric
 * interface useful to remap accurately and promptly the middle grey
 * to any arbitrary value chosen accordingly to the destination space.
 *
 * The combined use of both define a modern way to deal with large
 * dynamic range photographs by remapping the values with a comprehensive
 * interface avoiding many of the back and forth adjustments darktable
 * is prone to enforce.
 *
 * */

typedef struct dt_iop_filmic_params_t
{
  float grey_point_source;
  float black_point_source;
  float white_point_source;
  float security_factor;
  float grey_point_target;
  float black_point_target;
  float white_point_target;
  float output_power;
  float latitude_stops;
  float contrast;
  float saturation;
  float global_saturation;
  float balance;
  int interpolator;
  int preserve_color;
} dt_iop_filmic_params_t;


typedef struct dt_iop_filmic_data_t
{
  float table[0x10000];      // precomputed look-up table
  float table_temp[0x10000]; // precomputed look-up for the optimized interpolation
  float grad_2[0x10000];
  float max_grad;
  float grey_source;
  float black_source;
  float dynamic_range;
  float saturation;
  float global_saturation;
  float output_power;
  float contrast;
  int preserve_color;
  float latitude_min;
  float latitude_max;
} dt_iop_filmic_data_t;

typedef struct dt_iop_filmic_nodes_t
{
  int nodes;
  float y[5];
  float x[5];
} dt_iop_filmic_nodes_t;

typedef struct dt_iop_filmic_global_data_t
{
  int kernel_filmic;
  int kernel_filmic_log;
} dt_iop_filmic_global_data_t;


const char *name()
{
  return _("filmic");
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_DEPRECATED;
}

const char *deprecated_msg()
{
  return _("this module is deprecated. better use filmic rgb module instead.");
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_filmic_params_v3_t
  {
    float grey_point_source;
    float black_point_source;
    float white_point_source;
    float security_factor;
    float grey_point_target;
    float black_point_target;
    float white_point_target;
    float output_power;
    float latitude_stops;
    float contrast;
    float saturation;
    float global_saturation;
    float balance;
    int interpolator;
    int preserve_color;
  } dt_iop_filmic_params_v3_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_filmic_params_v1_t
    {
      float grey_point_source;
      float black_point_source;
      float white_point_source;
      float security_factor;
      float grey_point_target;
      float black_point_target;
      float white_point_target;
      float output_power;
      float latitude_stops;
      float contrast;
      float saturation;
      float balance;
      int interpolator;
    } dt_iop_filmic_params_v1_t;

    const dt_iop_filmic_params_v1_t *o = (dt_iop_filmic_params_v1_t *)old_params;
    dt_iop_filmic_params_v3_t *n = malloc(sizeof(dt_iop_filmic_params_v3_t));

    n->grey_point_source = o->grey_point_source;
    n->white_point_source = o->white_point_source;
    n->black_point_source = o->black_point_source;
    n->security_factor = o->security_factor;
    n->grey_point_target = o->grey_point_target;
    n->black_point_target = o->black_point_target;
    n->white_point_target = o->white_point_target;
    n->output_power = o->output_power;
    n->latitude_stops = o->latitude_stops;
    n->contrast = o->contrast;
    n->saturation = o->saturation;
    n->balance = o->balance;
    n->interpolator = o->interpolator;
    n->preserve_color = 0;
    n->global_saturation = 100;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_filmic_params_v3_t);
    *new_version = 3;
    return 0;
  }

  if(old_version == 2)
  {
    typedef struct dt_iop_filmic_params_v2_t
    {
      float grey_point_source;
      float black_point_source;
      float white_point_source;
      float security_factor;
      float grey_point_target;
      float black_point_target;
      float white_point_target;
      float output_power;
      float latitude_stops;
      float contrast;
      float saturation;
      float balance;
      int interpolator;
      int preserve_color;
    } dt_iop_filmic_params_v2_t;

    const dt_iop_filmic_params_v2_t *o = (dt_iop_filmic_params_v2_t *)old_params;
    dt_iop_filmic_params_v3_t *n = malloc(sizeof(dt_iop_filmic_params_v3_t));

    n->grey_point_source = o->grey_point_source;
    n->white_point_source = o->white_point_source;
    n->black_point_source = o->black_point_source;
    n->security_factor = o->security_factor;
    n->grey_point_target = o->grey_point_target;
    n->black_point_target = o->black_point_target;
    n->white_point_target = o->white_point_target;
    n->output_power = o->output_power;
    n->latitude_stops = o->latitude_stops;
    n->contrast = o->contrast;
    n->saturation = o->saturation;
    n->balance = o->balance;
    n->interpolator = o->interpolator;
    n->preserve_color = o->preserve_color;
    n->global_saturation = 100;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_filmic_params_v3_t);
    *new_version = 3;
    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_filmic_params_t p;
  memset(&p, 0, sizeof(p));

  // Fine-tune settings, no use here
  p.interpolator = CUBIC_SPLINE;

  // Output - standard display, gamma 2.2
  p.output_power = 2.2f;
  p.white_point_target = 100.0f;
  p.black_point_target = 0.0f;
  p.grey_point_target = 18.0f;

  // Input - standard raw picture
  p.security_factor = 0.0f;
  p.contrast = 1.618f;
  p.preserve_color = 1;
  p.balance = -12.0f;
  p.saturation = 60.0f;
  p.global_saturation = 70.0f;

  // Presets low-key
  p.grey_point_source = 25.4f;
  p.latitude_stops = 2.25f;
  p.white_point_source = 1.95f;
  p.black_point_source = -7.05f;
  dt_gui_presets_add_generic(_("09 EV (low-key)"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Presets indoors
  p.grey_point_source = 18.0f;
  p.latitude_stops = 2.75f;
  p.white_point_source = 2.45f;
  p.black_point_source = -7.55f;
  dt_gui_presets_add_generic(_("10 EV (indoors)"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Presets dim-outdoors
  p.grey_point_source = 12.77f;
  p.latitude_stops = 3.0f;
  p.white_point_source = 2.95f;
  p.black_point_source = -8.05f;
  dt_gui_presets_add_generic(_("11 EV (dim outdoors)"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Presets outdoors
  p.grey_point_source = 9.0f;
  p.latitude_stops = 3.5f;
  p.white_point_source = 3.45f;
  p.black_point_source = -8.55f;
  dt_gui_presets_add_generic(_("12 EV (outdoors)"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Presets outdoors
  p.grey_point_source = 6.38f;
  p.latitude_stops = 3.75f;
  p.white_point_source = 3.95f;
  p.black_point_source = -9.05f;
  dt_gui_presets_add_generic(_("13 EV (bright outdoors)"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Presets backlighting
  p.grey_point_source = 4.5f;
  p.latitude_stops = 4.25f;
  p.white_point_source = 4.45f;
  p.black_point_source = -9.55f;
  dt_gui_presets_add_generic(_("14 EV (backlighting)"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Presets sunset
  p.grey_point_source = 3.19f;
  p.latitude_stops = 4.50f;
  p.white_point_source = 4.95f;
  p.black_point_source = -10.05f;
  dt_gui_presets_add_generic(_("15 EV (sunset)"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Presets HDR
  p.grey_point_source = 2.25f;
  p.latitude_stops = 5.0f;
  p.white_point_source = 5.45f;
  p.black_point_source = -10.55f;
  dt_gui_presets_add_generic(_("16 EV (HDR)"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Presets HDR+
  p.grey_point_source = 1.125f;
  p.latitude_stops = 6.0f;
  p.white_point_source = 6.45f;
  p.black_point_source = -11.55f;
  dt_gui_presets_add_generic(_("18 EV (HDR++)"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);
}

// we need to move the actual processing of each pixel into a separate
// function to help the optimizer.  This will get inlined, but actually
// having the code inside the loop costs us 30% extra time with GCC10.
static void _process_pixel(const dt_aligned_pixel_t in,
                           dt_aligned_pixel_t out,
                           const float grey_source,
                           const float black_source,
                           const float inv_dynamic_range,
                           const dt_aligned_pixel_t output_power,
                           const float saturation,
                           const float EPS,
                           const int desaturate,
                           const int preserve_color,
                           const dt_iop_filmic_data_t *const data)
{
    dt_aligned_pixel_t XYZ;
    dt_Lab_to_XYZ(in, XYZ);

    dt_aligned_pixel_t input_rgb;
    dt_XYZ_to_prophotorgb(XYZ, input_rgb);

    float concavity, luma;

    // Global desaturation
    if(desaturate)
    {
      luma = XYZ[1];

      for_each_channel(c)
      {
        input_rgb[c] = luma + saturation * (input_rgb[c] - luma);
      }
    }

    dt_aligned_pixel_t rgb;
    if(preserve_color)
    {
      dt_aligned_pixel_t ratios;
      float max = dt_vector_channel_max(input_rgb);

      // Save the ratios
      for_each_channel(c)
        ratios[c] = input_rgb[c] / max;

      // Log tone-mapping
      max = max / grey_source;
      max = (max > EPS) ? (fastlog2(max) - black_source) * inv_dynamic_range : EPS;
      max = CLAMP(max, 0.0f, 1.0f);

      // Filmic S curve on the max RGB
      size_t index = CLAMP(max * 0x10000ul, 0, 0xffff);
      max = data->table[index];
      concavity = data->grad_2[index];

      // Re-apply ratios
      for_each_channel(c)
        rgb[c] = ratios[c] * max;

      luma = max;
    }
    else
    {
      size_t DT_ALIGNED_ARRAY index[4];

      for_each_channel(c)
        input_rgb[c] /= grey_source;
      dt_aligned_pixel_t log_rgb;
      dt_vector_log2(input_rgb, log_rgb);
      for_each_channel(c)
      {
        // Log tone-mapping on RGB
        rgb[c] = (input_rgb[c] > EPS) ? (log_rgb[c] - black_source) * inv_dynamic_range : EPS;
      }
      for_each_channel(c)
      {
        rgb[c] = CLAMP(rgb[c], 0.0f, 1.0f);
        // Store the index of the LUT
        index[c] = CLAMP(rgb[c] * 0x10000ul, 0, 0xffff);
      }

      // Concavity
      const float XYZ_luma = dt_prophotorgb_to_XYZ_luma(rgb);
      concavity = data->grad_2[(int)CLAMP(XYZ_luma * 0x10000ul, 0, 0xffff)];

      // Filmic S curve
      for_each_channel(c)
        rgb[c] = data->table[index[c]];

      luma = dt_prophotorgb_to_XYZ_luma(rgb);
    }

    // Desaturate on the non-linear parts of the curve
    for_each_channel(c)
    {
      // Desaturate on the non-linear parts of the curve
      rgb[c] = luma + concavity * (rgb[c] - luma);
      rgb[c] = CLAMP(rgb[c], 0.0f, 1.0f);
    }
    dt_aligned_pixel_t output_rgb;
    dt_vector_powf(rgb, output_power, output_rgb);
    // transform the result back to Lab
    // sRGB -> XYZ -> Lab
    dt_aligned_pixel_t res;
    dt_prophotorgb_to_Lab(output_rgb, res);
    copy_pixel_nontemporal(out, res);
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

  dt_iop_filmic_data_t *const data = piece->data;

  /** The log2(x) -> -INF when x -> 0
  * thus very low values (noise) will get even lower, resulting in noise negative amplification,
  * which leads to pepper noise in shadows. To avoid that, we need to clip values that are noise for sure.
  * Using 16 bits RAW data, the black value (known by rawspeed for every manufacturer) could be used as a threshold.
  * However, at this point of the pixelpipe, the RAW levels have already been corrected and everything can happen with black levels
  * in the exposure module. So we define the threshold as the first non-null 16 bit integer
  */
  const float EPS = powf(2.0f, -16);
  const int preserve_color = data->preserve_color;

  // If saturation == 100, we have a no-op. Disable the op then.
  const int desaturate = (data->global_saturation == 100.0f) ? FALSE : TRUE;
  const float saturation = data->global_saturation / 100.0f;

  const float *const restrict in = (float*)ivoid;
  float *const restrict out = (float*)ovoid;
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  const float grey_source = data->grey_source;
  const float black_source = data->black_source;
  const float inv_dynamic_range = 1.0f / data->dynamic_range;
  const dt_aligned_pixel_t output_power = {
    data->output_power, data->output_power,
    data->output_power, data->output_power
  };

  DT_OMP_FOR()
  for(size_t k = 0; k < (size_t)4 * npixels; k += 4)
  {
    _process_pixel(in + k, out +k, grey_source, black_source, inv_dynamic_range, output_power,
                   saturation, EPS, desaturate, preserve_color, data);
  }
  dt_omploop_sfence();
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_filmic_data_t *d = piece->data;
  dt_iop_filmic_global_data_t *gd = self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;


  cl_mem dev_table = NULL;
  cl_mem diff_table = NULL;

  dev_table = dt_opencl_copy_host_to_device(devid, d->table, 256, 256, sizeof(float));
  if(dev_table == NULL) goto error;

  diff_table = dt_opencl_copy_host_to_device(devid, d->grad_2, 256, 256, sizeof(float));
  if(diff_table == NULL) goto error;

  const float dynamic_range = d->dynamic_range;
  const float shadows_range = d->black_source;
  const float grey = d->grey_source;
  const float contrast = d->contrast;
  const float power = d->output_power;
  const int preserve_color = d->preserve_color;
  const float saturation = d->global_saturation / 100.0f;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_filmic, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(dynamic_range), CLARG(shadows_range),
    CLARG(grey), CLARG(dev_table), CLARG(diff_table), CLARG(contrast), CLARG(power), CLARG(preserve_color),
    CLARG(saturation));

error:
  dt_opencl_release_mem_object(dev_table);
  dt_opencl_release_mem_object(diff_table);
  return err;
}
#endif


void compute_curve_lut(dt_iop_filmic_params_t *p, float *table, float *table_temp, int res,
  dt_iop_filmic_data_t *d, dt_iop_filmic_nodes_t *nodes_data)
{
  dt_draw_curve_t *curve;

  const float white_source = p->white_point_source;
  const float black_source = p->black_point_source;
  const float dynamic_range = white_source - black_source;

  // luminance after log encoding
  const float black_log = 0.0f; // assumes user set log as in the autotuner
  const float grey_log = fabsf(p->black_point_source) / dynamic_range;
  const float white_log = 1.0f; // assumes user set log as in the autotuner

  // target luminance desired after filmic curve
  const float black_display = CLAMP(p->black_point_target, 0.0f, p->grey_point_target) / 100.0f; // in %
  const float grey_display = powf(CLAMP(p->grey_point_target, p->black_point_target, p->white_point_target) / 100.0f, 1.0f / (p->output_power));
  const float white_display = CLAMP(p->white_point_target, p->grey_point_target, 100.0f)  / 100.0f; // in %

  const float latitude = CLAMP(p->latitude_stops, 0.01f, dynamic_range * 0.99f);
  const float balance = CLAMP(p->balance, -50.0f, 50.0f) / 100.0f; // in %

  const float contrast = p->contrast;

  // nodes for mapping from log encoding to desired target luminance
  // X coordinates
  float toe_log = grey_log - latitude/dynamic_range * fabsf(black_source/dynamic_range);
  float shoulder_log = grey_log + latitude/dynamic_range * white_source/dynamic_range;


  // interception
  float linear_intercept = grey_display - (contrast * grey_log);

  // y coordinates
  float toe_display = (toe_log * contrast + linear_intercept);
  float shoulder_display = (shoulder_log * contrast + linear_intercept);

  // Apply the highlights/shadows balance as a shift along the contrast slope
  const float norm = powf(powf(contrast, 2.0f) + 1.0f, 0.5f);

  // negative values drag to the left and compress the shadows, on the UI negative is the inverse
  const float coeff = -(dynamic_range - latitude) / dynamic_range * balance;

  toe_display += coeff * contrast /norm;
  shoulder_display += coeff * contrast /norm;
  toe_log += coeff /norm;
  shoulder_log += coeff /norm;

  // Sanitize pass 1
  toe_log = CLAMP(toe_log, 0.0f, grey_log);
  shoulder_log = CLAMP(shoulder_log, grey_log, 1.0f);
  toe_display = CLAMP(toe_display, black_display, grey_display);
  shoulder_display = CLAMP(shoulder_display, grey_display, white_display);

  /**
   * Now we have 3 segments :
   *  - x = [0.0 ; toe_log], curved part
   *  - x = [toe_log ; grey_log ; shoulder_log], linear part
   *  - x = [shoulder_log ; 1.0] curved part
   *
   * BUT : in case some nodes overlap, we need to remove them to avoid
   * degenerating of the curve
  **/

  // sanitize pass 2
  int TOE_LOST = FALSE;
  int SHOULDER_LOST = FALSE;

  if((toe_log == grey_log && toe_display == grey_display) || (toe_log == 0.0f && toe_display  == black_display))
  {
    TOE_LOST = TRUE;
  }
  if((shoulder_log == grey_log && shoulder_display == grey_display) || (shoulder_log == 1.0f && shoulder_display == white_display))
  {
    SHOULDER_LOST = TRUE;
  }

  // Build the curve from the nodes

  if(SHOULDER_LOST && !TOE_LOST)
  {
    // shoulder only broke - we remove it
    nodes_data->nodes = 4;
    nodes_data->x[0] = black_log;
    nodes_data->x[1] = toe_log;
    nodes_data->x[2] = grey_log;
    nodes_data->x[3] = white_log;

    nodes_data->y[0] = black_display;
    nodes_data->y[1] = toe_display;
    nodes_data->y[2] = grey_display;
    nodes_data->y[3] = white_display;

    if(d)
    {
      d->latitude_min = toe_log;
      d->latitude_max = white_log;
    }

    //dt_control_log(_("filmic curve using 4 nodes - highlights lost"));

  }
  else if(TOE_LOST && !SHOULDER_LOST)
  {
    // toe only broke - we remove it
    nodes_data->nodes = 4;

    nodes_data->x[0] = black_log;
    nodes_data->x[1] = grey_log;
    nodes_data->x[2] = shoulder_log;
    nodes_data->x[3] = white_log;

    nodes_data->y[0] = black_display;
    nodes_data->y[1] = grey_display;
    nodes_data->y[2] = shoulder_display;
    nodes_data->y[3] = white_display;

    if(d)
    {
      d->latitude_min = black_log;
      d->latitude_max = shoulder_log;
    }

    //dt_control_log(_("filmic curve using 4 nodes - shadows lost"));

  }
  else if(TOE_LOST && SHOULDER_LOST)
  {
    // toe and shoulder both broke - we remove them
    nodes_data->nodes = 3;

    nodes_data->x[0] = black_log;
    nodes_data->x[1] = grey_log;
    nodes_data->x[2] = white_log;

    nodes_data->y[0] = black_display;
    nodes_data->y[1] = grey_display;
    nodes_data->y[2] = white_display;

    if(d)
    {
      d->latitude_min = black_log;
      d->latitude_max = white_log;
    }

    //dt_control_log(_("filmic curve using 3 nodes - highlights & shadows lost"));

  }
  else
  {
    // everything OK
    nodes_data->nodes = 4;

    nodes_data->x[0] = black_log;
    nodes_data->x[1] = toe_log;
    //nodes_data->x[2] = grey_log,
    nodes_data->x[2] = shoulder_log;
    nodes_data->x[3] = white_log;

    nodes_data->y[0] = black_display;
    nodes_data->y[1] = toe_display;
    //nodes_data->y[2] = grey_display,
    nodes_data->y[2] = shoulder_display;
    nodes_data->y[3] = white_display;

    if(d)
    {
      d->latitude_min = toe_log;
      d->latitude_max = shoulder_log;
    }

    //dt_control_log(_("filmic curve using 5 nodes - everything alright"));
  }

  if(p->interpolator != 3)
  {
    // Compute the interpolation

    // Catch bad interpolators exceptions (errors in saved params)
    int interpolator = CUBIC_SPLINE;
    if(p->interpolator > CUBIC_SPLINE && p->interpolator <= MONOTONE_HERMITE) interpolator = p->interpolator;

    curve = dt_draw_curve_new(0.0, 1.0, interpolator);
    for(int k = 0; k < nodes_data->nodes; k++) (void)dt_draw_curve_add_point(curve, nodes_data->x[k], nodes_data->y[k]);

    // Compute the LUT
    dt_draw_curve_calc_values(curve, 0.0f, 1.0f, res, NULL, table);
    dt_draw_curve_destroy(curve);

  }
  else
  {
    // Compute the monotonic interpolation
    curve = dt_draw_curve_new(0.0, 1.0, MONOTONE_HERMITE);
    for(int k = 0; k < nodes_data->nodes; k++) (void)dt_draw_curve_add_point(curve, nodes_data->x[k], nodes_data->y[k]);
    dt_draw_curve_calc_values(curve, 0.0f, 1.0f, res, NULL, table_temp);
    dt_draw_curve_destroy(curve);

    // Compute the cubic spline interpolation
    curve = dt_draw_curve_new(0.0, 1.0, CUBIC_SPLINE);
    for(int k = 0; k < nodes_data->nodes; k++) (void)dt_draw_curve_add_point(curve, nodes_data->x[k], nodes_data->y[k]);
    dt_draw_curve_calc_values(curve, 0.0f, 1.0f, res, NULL, table);
    dt_draw_curve_destroy(curve);

    // Average both LUT
    DT_OMP_FOR()
    for(int k = 0; k < res; k++) table[k] = (table[k] + table_temp[k]) / 2.0f;
  }

}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)p1;
  dt_iop_filmic_data_t *d = piece->data;

  d->preserve_color = p->preserve_color;

  // source luminance - Used only in the log encoding
  const float white_source = p->white_point_source;
  const float grey_source = p->grey_point_source / 100.0f; // in %
  const float black_source = p->black_point_source;
  const float dynamic_range = white_source - black_source;

  // luminance after log encoding
  const float grey_log = fabsf(p->black_point_source) / dynamic_range;

  // target luminance desired after filmic curve
  const float grey_display = powf(p->grey_point_target / 100.0f, 1.0f / (p->output_power));

  float contrast = p->contrast;
  if(contrast < grey_display / grey_log)
  {
    // We need grey_display - (contrast * grey_log) <= 0.0
    contrast = 1.0001f * grey_display / grey_log;
  }

  // commitproducts with no low-pass filter, you will increase the contrast of nois
  d->dynamic_range = dynamic_range;
  d->black_source = black_source;
  d->grey_source = grey_source;
  d->output_power = p->output_power;
  d->saturation = p->saturation;
  d->global_saturation = p->global_saturation;
  d->contrast = contrast;

  // compute the curves and their LUT
  dt_iop_filmic_nodes_t *nodes_data = malloc(sizeof(dt_iop_filmic_nodes_t));
  compute_curve_lut(p, d->table, d->table_temp, 0x10000, d, nodes_data);
  free(nodes_data);
  nodes_data = NULL;

  // Build a window function based on the log.
  // This will be used to selectively desaturate the non-linear parts
  // to avoid over-saturation in the toe and shoulder.

  const float latitude = d->latitude_max - d->latitude_min;
  const float center = (d->latitude_max + d->latitude_min)/2.0f;
  const float saturation = d->saturation / 100.0f;
  const float sigma = saturation * saturation * latitude * latitude;

  DT_OMP_FOR()
  for(int k = 0; k < 65536; k++)
  {
    const float x = ((float)k) / 65536.0f;
    if(sigma != 0.0f)
    {
      d->grad_2[k] = expf(-0.5f * (center - x) * (center - x) / sigma);
    }
    else
    {
      d->grad_2[k] = 0.0f;
    }
  }

}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_filmic_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_filmic_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_filmic_params_t));
  self->default_enabled = FALSE;
  self->params_size = sizeof(dt_iop_filmic_params_t);
  self->gui_data = NULL;

  *(dt_iop_filmic_params_t *)self->default_params
    = (dt_iop_filmic_params_t){
                                 .grey_point_source   = 18, // source grey
                                 .black_point_source  = -8.65,  // source black
                                 .white_point_source  = 2.45,  // source white
                                 .security_factor     = 0.0,  // security factor
                                 .grey_point_target   = 18.0, // target grey
                                 .black_point_target  = 0.0,  // target black
                                 .white_point_target  = 100.0,  // target white
                                 .output_power        = 2.2,  // target power (~ gamma)
                                 .latitude_stops      = 2.0,  // intent latitude
                                 .contrast            = 1.5,  // intent contrast
                                 .saturation          = 100.0,   // intent saturation
                                 .global_saturation   = 100.0,
                                 .balance             = 0.0, // balance shadows/highlights
                                 .interpolator        = CUBIC_SPLINE, //interpolator
                                 .preserve_color      = 0, // run the saturated variant
                              };
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 22; // filmic.cl, from programs.conf
  dt_iop_filmic_global_data_t *gd = malloc(sizeof(dt_iop_filmic_global_data_t));

  self->data = gd;
  gd->kernel_filmic = dt_opencl_create_kernel(program, "filmic");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_filmic_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_filmic);
  free(self->data);
  self->data = NULL;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
