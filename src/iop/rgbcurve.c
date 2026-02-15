/*
    This file is part of darktable,
    Copyright (C) 2019-2025 darktable developers.

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

#include "common/iop_profile.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/rgb_norms.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "libs/colorpicker.h"

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)
#define DT_IOP_RGBCURVE_RES 256
#define DT_IOP_RGBCURVE_MAXNODES MAX_ANCHORS
#define DT_IOP_RGBCURVE_MIN_X_DISTANCE 0.0025f
// max iccprofile file name length
// must be in synch with filename in dt_colorspaces_color_profile_t in colorspaces.h
#define DT_IOP_COLOR_ICC_LEN 512

DT_MODULE_INTROSPECTION(1, dt_iop_rgbcurve_params_t)

typedef enum rgbcurve_channel_t
{
  DT_IOP_RGBCURVE_R = 0,
  DT_IOP_RGBCURVE_G = 1,
  DT_IOP_RGBCURVE_B = 2,
  DT_IOP_RGBCURVE_MAX_CHANNELS = 3
} rgbcurve_channel_t;

typedef enum dt_iop_rgbcurve_autoscale_t
{
  DT_S_SCALE_AUTOMATIC_RGB = 0, // $DESCRIPTION: "RGB, linked channels"
  DT_S_SCALE_MANUAL_RGB = 1     // $DESCRIPTION: "RGB, independent channels"
} dt_iop_rgbcurve_autoscale_t;

typedef struct dt_iop_rgbcurve_node_t
{
  float x; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0
  float y; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0
} dt_iop_rgbcurve_node_t;

typedef struct dt_iop_rgbcurve_params_t
{
  dt_iop_rgbcurve_node_t curve_nodes[DT_IOP_RGBCURVE_MAX_CHANNELS]
                                    [DT_IOP_RGBCURVE_MAXNODES]; // actual nodes for each curve
  int curve_num_nodes[DT_IOP_RGBCURVE_MAX_CHANNELS]; // $DEFAULT: 2 number of nodes per curve
  int curve_type[DT_IOP_RGBCURVE_MAX_CHANNELS]; // $DEFAULT: MONOTONE_HERMITE (CATMULL_ROM, MONOTONE_HERMITE, CUBIC_SPLINE)
  dt_iop_rgbcurve_autoscale_t curve_autoscale;  // $DEFAULT: DT_S_SCALE_AUTOMATIC_RGB $DESCRIPTION: "mode"
  gboolean compensate_middle_grey; // $DEFAULT: 0  $DESCRIPTION: "compensate middle gray" scale the curve and histogram so middle gray is at .5
  dt_iop_rgb_norms_t preserve_colors; // $DEFAULT: DT_RGB_NORM_LUMINANCE $DESCRIPTION: "preserve colors"
} dt_iop_rgbcurve_params_t;


typedef struct dt_iop_rgbcurve_data_t
{
  float table[DT_IOP_RGBCURVE_MAX_CHANNELS][0x10000];      // precomputed look-up tables for tone curve
  dt_iop_rgbcurve_params_t params;
  dt_draw_curve_t *curve[DT_IOP_RGBCURVE_MAX_CHANNELS];    // curves for pipe piece and pixel processing
  float unbounded_coeffs[DT_IOP_RGBCURVE_MAX_CHANNELS][3]; // approximation for extrapolation
  gboolean curve_changed[DT_IOP_RGBCURVE_MAX_CHANNELS];    // curve type changed?
  dt_colorspaces_color_profile_type_t type_work; // working color profile
  char filename_work[DT_IOP_COLOR_ICC_LEN];
} dt_iop_rgbcurve_data_t;

typedef float (*_curve_table_ptr)[0x10000];
typedef float (*_coeffs_table_ptr)[3];

typedef struct dt_iop_rgbcurve_global_data_t
{
  int kernel_rgbcurve;
} dt_iop_rgbcurve_global_data_t;

const char *name()
{
  return _("rgb curve");
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_GRADING;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self, _("alter an image’s tones using curves in RGB color space"),
     _("corrective and creative"),
     _("linear, RGB, display-referred"),
     _("non-linear, RGB"),
     _("linear, RGB, display-referred"));
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_rgbcurve_params_t p;
  memset(&p, 0, sizeof(p));
  p.curve_num_nodes[DT_IOP_RGBCURVE_R] = 6;
  p.curve_num_nodes[DT_IOP_RGBCURVE_G] = 7;
  p.curve_num_nodes[DT_IOP_RGBCURVE_B] = 7;
  p.curve_type[DT_IOP_RGBCURVE_R] = CUBIC_SPLINE;
  p.curve_type[DT_IOP_RGBCURVE_G] = CUBIC_SPLINE;
  p.curve_type[DT_IOP_RGBCURVE_B] = CUBIC_SPLINE;
  p.curve_autoscale = DT_S_SCALE_AUTOMATIC_RGB;
  p.compensate_middle_grey = TRUE;
  p.preserve_colors = 1;

  float linear_ab[7] = { 0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0 };

  // linear a, b curves for presets
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_G][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_G][k].y = linear_ab[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_B][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_B][k].y = linear_ab[k];

  // More useful low contrast curve (based on Samsung NX -2 Contrast)
  p.curve_nodes[DT_IOP_RGBCURVE_R][0].x = 0.000000;
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].x = 0.003862;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].x = 0.076613;
  p.curve_nodes[DT_IOP_RGBCURVE_R][3].x = 0.169355;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].x = 0.774194;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].x = 1.000000;
  p.curve_nodes[DT_IOP_RGBCURVE_R][0].y = 0.000000;
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y = 0.007782;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y = 0.156182;
  p.curve_nodes[DT_IOP_RGBCURVE_R][3].y = 0.290352;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y = 0.773852;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y = 1.000000;
  dt_gui_presets_add_generic(_("contrast | compression"), self->op,
                             self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  p.curve_num_nodes[DT_IOP_RGBCURVE_R] = 7;
  float linear_L[7] = { 0.0, 0.08, 0.17, 0.50, 0.83, 0.92, 1.0 };

  // Linear - no contrast
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  dt_gui_presets_add_generic(_("linear (gamma 1.0)"), self->op,
                             self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Linear contrast
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.020;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.020;
  dt_gui_presets_add_generic(_("contrast | medium (linear)"), self->op,
                             self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.040;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.040;
  dt_gui_presets_add_generic(_("contrast | high (linear)"), self->op,
                             self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Gamma contrast
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.020;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.020;
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].x =
      powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].x, 2.2f);
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].y =
      powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].y, 2.2f);
  dt_gui_presets_add_generic(_("contrast | medium (gamma 2.2)"), self->op,
                             self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.040;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.040;
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].x =
      powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].x, 2.2f);
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].y =
      powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].y, 2.2f);
  dt_gui_presets_add_generic(_("contrast | high (gamma 2.2)"), self->op,
                             self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  /** for pure power-like functions, we need more nodes close to the bounds**/

  p.curve_type[DT_IOP_RGBCURVE_R] = MONOTONE_HERMITE;

  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];

  // Gamma 2.0 - no contrast
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = (linear_L[k] * linear_L[k]);
  dt_gui_presets_add_generic(_("non-contrast curve | gamma 2.0"), self->op,
                             self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Gamma 0.5 - no contrast
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = sqrtf(linear_L[k]);
  dt_gui_presets_add_generic(_("non-contrast curve | gamma 0.5"), self->op,
                             self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Log2 - no contrast
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = logf(linear_L[k] + 1.0f) / logf(2.0f);
  dt_gui_presets_add_generic(_("non-contrast curve | logarithm (base 2)"), self->op,
                             self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Exp2 - no contrast
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = powf(2.0f, linear_L[k]) - 1.0f;
  dt_gui_presets_add_generic(_("non-contrast curve | exponential (base 2)"), self->op,
                             self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_DISPLAY);
}


static inline int _add_node(dt_iop_rgbcurve_node_t *curve_nodes,
                            int *nodes,
                            float x,
                            float y)
{
  int selected = -1;
  if(curve_nodes[0].x > x)
    selected = 0;
  else
  {
    for(int k = 1; k < *nodes; k++)
    {
      if(curve_nodes[k].x > x)
      {
        selected = k;
        break;
      }
    }
  }
  if(selected == -1) selected = *nodes;
  for(int i = *nodes; i > selected; i--)
  {
    curve_nodes[i].x = curve_nodes[i - 1].x;
    curve_nodes[i].y = curve_nodes[i - 1].y;
  }
  // found a new point
  curve_nodes[selected].x = x;
  curve_nodes[selected].y = y;
  (*nodes)++;
  return selected;
}

static inline int _add_node_from_picker
  (dt_iop_rgbcurve_params_t *p,
   const float *const in,
   const float increment,
   const int ch,
   const dt_iop_order_iccprofile_info_t *const work_profile)
{
  float x = 0.f;
  float y = 0.f;
  float val = 0.f;

  if(p->curve_autoscale == DT_S_SCALE_AUTOMATIC_RGB)
    val = (work_profile)
           ? dt_ioppr_get_rgb_matrix_luminance(in,
                                               work_profile->matrix_in,
                                               work_profile->lut_in,
                                               work_profile->unbounded_coeffs_in,
                                               work_profile->lutsize,
                                               work_profile->nonlinearlut)
           : dt_camera_rgb_luminance(in);
  else
    val = in[ch];

  if(p->compensate_middle_grey && work_profile)
    y = x = dt_ioppr_compensate_middle_grey(val, work_profile);
  else
    y = x = val;

  x = CLIP(x - increment);
  y = CLIP(y + increment);

  return _add_node(p->curve_nodes[ch], &p->curve_num_nodes[ch], x, y);
}


#define RGBCURVE_DEFAULT_STEP (0.001f)


#undef RGBCURVE_DEFAULT_STEP


void change_image(dt_iop_module_t *self)
{
}


void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the pixelpipe
  dt_iop_rgbcurve_data_t *d = dt_alloc1_align_type(dt_iop_rgbcurve_data_t);
  const dt_iop_rgbcurve_params_t *const default_params = self->default_params;
  piece->data = (void *)d;
  memcpy(&d->params, default_params, sizeof(dt_iop_rgbcurve_params_t));

  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, default_params->curve_type[ch]);
    d->params.curve_num_nodes[ch] = default_params->curve_num_nodes[ch];
    d->params.curve_type[ch] = default_params->curve_type[ch];
    for(int k = 0; k < default_params->curve_num_nodes[ch]; k++)
      dt_draw_curve_add_point(d->curve[ch], default_params->curve_nodes[ch][k].x,
                                    default_params->curve_nodes[ch][k].y);
  }

  for(int k = 0; k < 0x10000; k++)
    d->table[DT_IOP_RGBCURVE_R][k] = k / 0x10000; // identity for r
  for(int k = 0; k < 0x10000; k++)
    d->table[DT_IOP_RGBCURVE_G][k] = k / 0x10000; // identity for g
  for(int k = 0; k < 0x10000; k++)
    d->table[DT_IOP_RGBCURVE_B][k] = k / 0x10000; // identity for b
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_rgbcurve_data_t *d = piece->data;
  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    dt_draw_curve_destroy(d->curve[ch]);
  dt_free_align(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  self->request_histogram |= (DT_REQUEST_ON | DT_REQUEST_EXPANDED);

  dt_iop_rgbcurve_params_t *d = self->default_params;

  d->curve_nodes[0][1].x = d->curve_nodes[0][1].y =
  d->curve_nodes[1][1].x = d->curve_nodes[1][1].y =
  d->curve_nodes[2][1].x = d->curve_nodes[2][1].y = 1.0;

  self->histogram_middle_grey = d->compensate_middle_grey;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 25; // rgbcurve.cl, from programs.conf
  dt_iop_rgbcurve_global_data_t *gd =dt_alloc1_align_type(dt_iop_rgbcurve_global_data_t);
  self->data = gd;

  gd->kernel_rgbcurve = dt_opencl_create_kernel(program, "rgbcurve");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_rgbcurve_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_rgbcurve);
  dt_free_align(self->data);
  self->data = NULL;
}

// called from process*(), takes care of changed curve type
static void _generate_curve_lut(dt_dev_pixelpipe_t *pipe,
                                dt_iop_rgbcurve_data_t *d)
{
  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_work_profile_info(pipe);

  DT_ALIGNED_ARRAY dt_iop_rgbcurve_node_t curve_nodes[3][DT_IOP_RGBCURVE_MAXNODES];

  if(work_profile)
  {
    if(d->type_work == work_profile->type && strcmp(d->filename_work,
                                                    work_profile->filename) == 0) return;
  }

  if(work_profile && d->params.compensate_middle_grey)
  {
    d->type_work = work_profile->type;
    g_strlcpy(d->filename_work, work_profile->filename, sizeof(d->filename_work));

    for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    {
      for(int k = 0; k < d->params.curve_num_nodes[ch]; k++)
      {
        curve_nodes[ch][k].x =
          dt_ioppr_uncompensate_middle_grey(d->params.curve_nodes[ch][k].x, work_profile);
        curve_nodes[ch][k].y =
          dt_ioppr_uncompensate_middle_grey(d->params.curve_nodes[ch][k].y, work_profile);
      }
    }
  }
  else
  {
    for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    {
      memcpy(curve_nodes[ch], d->params.curve_nodes[ch],
             sizeof(dt_iop_rgbcurve_node_t) * DT_IOP_RGBCURVE_MAXNODES);
    }
  }

  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
  {
    /* take care of possible change of curve type, number of nodes is explicitly set.
       We only need a new curve if it's type has changed for a different interpolation.
       If we change the curve we avoid a possible race condition between pixelpipes.
    */
    if(d->curve_changed[ch])
    {
      dt_draw_curve_t *oldcurve = d->curve[ch];
      d->curve[ch] = dt_draw_curve_new(0.0, 1.0, d->params.curve_type[ch]);
      d->curve_changed[ch] = FALSE;
      dt_draw_curve_destroy(oldcurve);
    }

    for(int k = 0; k < d->params.curve_num_nodes[ch]; k++)
      dt_draw_curve_set_point(d->curve[ch], k, curve_nodes[ch][k].x, curve_nodes[ch][k].y);
    d->curve[ch]->c.m_numAnchors = d->params.curve_num_nodes[ch];

    dt_draw_curve_calc_values(d->curve[ch], 0.0f, 1.0f, 0x10000, NULL, d->table[ch]);
  }

  // extrapolation for each curve (right hand side only):
  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
  {
    const float xm_L = curve_nodes[ch][d->params.curve_num_nodes[ch] - 1].x;
    const float x_L[4] = { 0.7f * xm_L, 0.8f * xm_L, 0.9f * xm_L, 1.0f * xm_L };
    const float y_L[4] = { d->table[ch][CLAMP((int)(x_L[0] * 0x10000ul), 0, 0xffff)],
                           d->table[ch][CLAMP((int)(x_L[1] * 0x10000ul), 0, 0xffff)],
                           d->table[ch][CLAMP((int)(x_L[2] * 0x10000ul), 0, 0xffff)],
                           d->table[ch][CLAMP((int)(x_L[3] * 0x10000ul), 0, 0xffff)] };
    dt_iop_estimate_exp(x_L, y_L, 4, d->unbounded_coeffs[ch]);
  }
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rgbcurve_data_t *d = piece->data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)p1;

  if(pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
  {
    piece->request_histogram |= DT_REQUEST_ON;
    self->histogram_middle_grey = p->compensate_middle_grey;
  }
  else
  {
    piece->request_histogram &= ~DT_REQUEST_ON;
  }

  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    d->curve_changed[ch] = d->params.curve_type[ch] != p->curve_type[ch];

  memcpy(&d->params, p, sizeof(dt_iop_rgbcurve_params_t));

  // working color profile
  d->type_work = DT_COLORSPACE_NONE;
  d->filename_work[0] = '\0';
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  dt_iop_rgbcurve_data_t *d = piece->data;
  dt_iop_rgbcurve_global_data_t *gd = self->global_data;

 _generate_curve_lut(piece->pipe, d);
  cl_int err = CL_SUCCESS;

  cl_mem dev_r = NULL;
  cl_mem dev_g = NULL;
  cl_mem dev_b = NULL;
  cl_mem dev_coeffs_r = NULL;
  cl_mem dev_coeffs_g = NULL;
  cl_mem dev_coeffs_b = NULL;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  const int use_work_profile = (work_profile == NULL) ? 0 : 1;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int autoscale = d->params.curve_autoscale;
  const int preserve_colors = d->params.preserve_colors;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid,
                                            &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto cleanup;

  err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  dev_r = dt_opencl_copy_host_to_device(devid, d->table[DT_IOP_RGBCURVE_R],
                                        256, 256, sizeof(float));
  if(dev_r == NULL) goto cleanup;

  dev_g = dt_opencl_copy_host_to_device(devid, d->table[DT_IOP_RGBCURVE_G],
                                        256, 256, sizeof(float));
  if(dev_g == NULL) goto cleanup;

  dev_b = dt_opencl_copy_host_to_device(devid, d->table[DT_IOP_RGBCURVE_B],
                                        256, 256, sizeof(float));
  if(dev_b == NULL) goto cleanup;

  dev_coeffs_r = dt_opencl_copy_host_to_device_constant
    (devid,
     sizeof(float) * DT_IOP_RGBCURVE_MAX_CHANNELS, d->unbounded_coeffs[0]);
  if(dev_coeffs_r == NULL) goto cleanup;

  dev_coeffs_g = dt_opencl_copy_host_to_device_constant
    (devid,
     sizeof(float) * DT_IOP_RGBCURVE_MAX_CHANNELS, d->unbounded_coeffs[1]);
  if(dev_coeffs_g == NULL) goto cleanup;

  dev_coeffs_b = dt_opencl_copy_host_to_device_constant
    (devid,
     sizeof(float) * DT_IOP_RGBCURVE_MAX_CHANNELS, d->unbounded_coeffs[2]);
  if(dev_coeffs_b == NULL) goto cleanup;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rgbcurve, width, height,
                                         CLARG(dev_in), CLARG(dev_out),
                                         CLARG(width), CLARG(height),
                                         CLARG(dev_r), CLARG(dev_g), CLARG(dev_b),
                                         CLARG(dev_coeffs_r),
                                         CLARG(dev_coeffs_g),
                                         CLARG(dev_coeffs_b),
                                         CLARG(autoscale),
                                         CLARG(preserve_colors),
                                         CLARG(dev_profile_info),
                                         CLARG(dev_profile_lut),
                                         CLARG(use_work_profile));

cleanup:
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs_r);
  dt_opencl_release_mem_object(dev_coeffs_g);
  dt_opencl_release_mem_object(dev_coeffs_b);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl,
                                     &dev_profile_info, &dev_profile_lut);
  return err;
}
#endif

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  const float *const restrict in = (float*)ivoid;
  float *const restrict out = (float*)ovoid;
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/,
                                        self, piece->colors,
                                        in, out, roi_in, roi_out))
    return; // image has been copied through to output and module's
            // trouble flag has been updated

  dt_iop_rgbcurve_data_t *const restrict d = piece->data;
  _generate_curve_lut(piece->pipe, d);

  const float xm_L = 1.0f / d->unbounded_coeffs[DT_IOP_RGBCURVE_R][0];
  const float xm_g = 1.0f / d->unbounded_coeffs[DT_IOP_RGBCURVE_G][0];
  const float xm_b = 1.0f / d->unbounded_coeffs[DT_IOP_RGBCURVE_B][0];

  const int width = roi_out->width;
  const int height = roi_out->height;
  const size_t npixels = (size_t)width * height;
  const int autoscale = d->params.curve_autoscale;
  const _curve_table_ptr restrict table = d->table;
  const _coeffs_table_ptr restrict unbounded_coeffs = d->unbounded_coeffs;

  DT_OMP_FOR()
  for(int y = 0; y < 4*npixels; y += 4)
  {
    if(autoscale == DT_S_SCALE_MANUAL_RGB)
    {
      out[y+0] = (in[y+0] < xm_L) ? table[DT_IOP_RGBCURVE_R][CLAMP((int)(in[y+0] * 0x10000ul), 0, 0xffff)]
                                  : dt_iop_eval_exp(unbounded_coeffs[DT_IOP_RGBCURVE_R], in[y+0]);
      out[y+1] = (in[y+1] < xm_g) ? table[DT_IOP_RGBCURVE_G][CLAMP((int)(in[y+1] * 0x10000ul), 0, 0xffff)]
                                  : dt_iop_eval_exp(unbounded_coeffs[DT_IOP_RGBCURVE_G], in[y+1]);
      out[y+2] = (in[y+2] < xm_b) ? table[DT_IOP_RGBCURVE_B][CLAMP((int)(in[y+2] * 0x10000ul), 0, 0xffff)]
                                  : dt_iop_eval_exp(unbounded_coeffs[DT_IOP_RGBCURVE_B], in[y+2]);
    }
    else if(autoscale == DT_S_SCALE_AUTOMATIC_RGB)
    {
      if(d->params.preserve_colors == DT_RGB_NORM_NONE)
      {
        for(int c = 0; c < 3; c++)
        {
          out[y+c] = (in[y+c] < xm_L)
            ? table[DT_IOP_RGBCURVE_R][CLAMP((int)(in[y+c] * 0x10000ul), 0, 0xffff)]
            : dt_iop_eval_exp(unbounded_coeffs[DT_IOP_RGBCURVE_R], in[y+c]);
        }
      }
      else
      {
        float ratio = 1.f;
        const float lum = dt_rgb_norm(in + y, d->params.preserve_colors, work_profile);
        if(lum > 0.f)
        {
          const float curve_lum = (lum < xm_L)
            ? table[DT_IOP_RGBCURVE_R][CLAMP((int)(lum * 0x10000ul), 0, 0xffff)]
            : dt_iop_eval_exp(unbounded_coeffs[DT_IOP_RGBCURVE_R], lum);
          ratio = curve_lum / lum;
        }
        for(size_t c = 0; c < 3; c++)
        {
          out[y+c] = (ratio * in[y+c]);
        }
      }
    }
    out[y+3] = in[y+3];
  }
}

#undef DT_GUI_CURVE_EDITOR_INSET
#undef DT_IOP_RGBCURVE_RES
#undef DT_IOP_RGBCURVE_MAXNODES
#undef DT_IOP_RGBCURVE_MIN_X_DISTANCE
#undef DT_IOP_COLOR_ICC_LEN

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
