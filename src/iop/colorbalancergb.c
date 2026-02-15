/*
    This file is part of darktable,
    Copyright (C) 2020-2025 darktable developers.

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

// our includes go first:
#include "common/exif.h"
#include "common/dttypes.h"
#include "common/chromatic_adaptation.h"
#include "common/darktable_ucs_22_helpers.h"
#include "common/gamut_mapping.h"
#include "common/opencl.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/openmp_maths.h"
#include "iop/iop_api.h"

#include <stdlib.h>
#define STEPS 92         // so we test 92×92×92 combinations of RGB in [0; 1] to build the gamut LUT

// Filmlight Yrg puts red at 330°, while usual HSL wheels put it at 360/0°
// so shift in GUI only it to not confuse people. User params are always degrees,
// pixel params are always radians.
#define ANGLE_SHIFT -30.f
#define CONVENTIONAL_DEG_TO_YRG_RAD(x) (deg2radf(x + ANGLE_SHIFT))
#define YRG_RAD_TO_CONVENTIONAL_DEG(x) (rad2degf(x) - ANGLE_SHIFT)

DT_MODULE_INTROSPECTION(5, dt_iop_colorbalancergb_params_t)

typedef enum dt_iop_colorbalancrgb_saturation_t
{
  DT_COLORBALANCE_SATURATION_JZAZBZ = 0, // $DESCRIPTION: "JzAzBz (2021)"
  DT_COLORBALANCE_SATURATION_DTUCS = 1   // $DESCRIPTION: "darktable UCS (2022)"
} dt_iop_colorbalancrgb_saturation_t;

typedef struct dt_iop_colorbalancergb_params_t
{
  /* params of v1 */
  float shadows_Y;             // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float shadows_C;             // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float shadows_H;             // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float midtones_Y;            // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float midtones_C;            // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float midtones_H;            // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float highlights_Y;          // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float highlights_C;          // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float highlights_H;          // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float global_Y;              // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float global_C;              // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float global_H;              // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float shadows_weight;        // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "shadows fall-off"
  float white_fulcrum;         // $MIN: -16.0 $MAX: 16.0 $DEFAULT: 0.0 $DESCRIPTION: "white fulcrum"
  float highlights_weight;     // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "highlights fall-off"
  float chroma_shadows;        // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
  float chroma_highlights;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float chroma_global;         // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "global chroma"
  float chroma_midtones;       // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
  float saturation_global;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "global saturation"
  float saturation_highlights; // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float saturation_midtones;   // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
  float saturation_shadows;    // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
  float hue_angle;             // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "hue shift"

  /* params of v2 */
  float brilliance_global;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "global brilliance"
  float brilliance_highlights; // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float brilliance_midtones;   // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "mid-tones"
  float brilliance_shadows;    // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"

  /* params of v3 */
  float mask_grey_fulcrum;     // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.1845 $DESCRIPTION: "mask middle-gray fulcrum"

  /* params of v4 */
  float vibrance;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0 $DESCRIPTION: "global vibrance"
  float grey_fulcrum;     // $MIN:  0.0 $MAX: 1.0 $DEFAULT: 0.1845 $DESCRIPTION: "contrast gray fulcrum"
  float contrast;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0. $DESCRIPTION: "contrast"

  /* params of v5 */
  dt_iop_colorbalancrgb_saturation_t saturation_formula; // $DEFAULT: 1 $DESCRIPTION: "saturation formula"

  /* add future params after this so the legacy params import can use a blind memcpy */
} dt_iop_colorbalancergb_params_t;


typedef enum dt_iop_colorbalancergb_mask_data_t
{
  MASK_SHADOWS = 0,
  MASK_MIDTONES = 1,
  MASK_HIGHLIGHTS = 2,
  MASK_NONE
} dt_iop_colorbalancergb_mask_data_t;


typedef struct dt_iop_colorbalancergb_data_t
{
  float global[4];
  float shadows[4];
  float highlights[4];
  float midtones[4];
  float midtones_Y;
  float chroma_global, chroma[4], vibrance, contrast;
  float saturation_global, saturation[4];
  float brilliance_global, brilliance[4];
  float hue_angle;
  float shadows_weight, highlights_weight, midtones_weight, mask_grey_fulcrum;
  float white_fulcrum, grey_fulcrum;
  float *gamut_LUT;
  float max_chroma;
  dt_aligned_pixel_t checker_color_1, checker_color_2;
  dt_iop_colorbalancrgb_saturation_t saturation_formula;
  size_t checker_size;
  gboolean lut_inited;
  struct dt_iop_order_iccprofile_info_t *work_profile;
} dt_iop_colorbalancergb_data_t;

typedef struct dt_iop_colorbalance_global_data_t
{
  int kernel_colorbalance_rgb;
} dt_iop_colorbalancergb_global_data_t;


const char *name()
{
  return _("color balance rgb");
}

const char *aliases()
{
  return _("offset power slope|cdl|color grading|contrast|chroma_highlights|hue|vibrance|saturation");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("color grading tools using alpha masks to separate\n"
                                        "shadows, mid-tones and highlights"),
                                      _("corrective or creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
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
  typedef struct dt_iop_colorbalancergb_params_v5_t
  {
    /* params of v1 */
    float shadows_Y;
    float shadows_C;
    float shadows_H;
    float midtones_Y;
    float midtones_C;
    float midtones_H;
    float highlights_Y;
    float highlights_C;
    float highlights_H;
    float global_Y;
    float global_C;
    float global_H;
    float shadows_weight;
    float white_fulcrum;
    float highlights_weight;
    float chroma_shadows;
    float chroma_highlights;
    float chroma_global;
    float chroma_midtones;
    float saturation_global;
    float saturation_highlights;
    float saturation_midtones;
    float saturation_shadows;
    float hue_angle;

    /* params of v2 */
    float brilliance_global;
    float brilliance_highlights;
    float brilliance_midtones;
    float brilliance_shadows;

    /* params of v3 */
    float mask_grey_fulcrum;

    /* params of v4 */
    float vibrance;
    float grey_fulcrum;
    float contrast;

    /* params of v5 */
    dt_iop_colorbalancrgb_saturation_t saturation_formula;

    /* add future params after this so the legacy params import can use a blind memcpy */
  } dt_iop_colorbalancergb_params_v5_t;

  const dt_iop_colorbalancergb_params_v5_t default_v5 =
    { 0.0f,  0.0f,  0.0f,  0.0f,    0.0f,
      0.0f,  0.0f,  0.0f,  0.0f,    0.0f,
      0.0f,  0.0f,  1.0f,  0.0f,    1.0f,
      0.0f,  0.0f,  0.0f,  0.0f,    0.0f,
      0.0f,  0.0f,  0.0f,  0.0f,    0.0f,
      0.0f,  0.0f,  0.0f,  0.1845f, 0.0f,
      0.0f,  0.0f,  1
    };

  if(old_version == 1)
  {
    typedef struct dt_iop_colorbalancergb_params_v1_t
    {
      float shadows_Y;
      float shadows_C;
      float shadows_H;
      float midtones_Y;
      float midtones_C;
      float midtones_H;
      float highlights_Y;
      float highlights_C;
      float highlights_H;
      float global_Y;
      float global_C;
      float global_H;
      float shadows_weight;
      float white_fulcrum;
      float highlights_weight;
      float chroma_shadows;
      float chroma_highlights;
      float chroma_global;
      float chroma_midtones;
      float saturation_global;
      float saturation_highlights;
      float saturation_midtones;
      float saturation_shadows;
      float hue_angle;
    } dt_iop_colorbalancergb_params_v1_t;

    const dt_iop_colorbalancergb_params_v1_t *o = old_params;
    dt_iop_colorbalancergb_params_v5_t *n =
      malloc(sizeof(dt_iop_colorbalancergb_params_v5_t));

    // Init params with defaults
    memcpy(n, &default_v5, sizeof(dt_iop_colorbalancergb_params_v5_t));

    // Copy the common part of the params struct
    memcpy(n, o, sizeof(dt_iop_colorbalancergb_params_v1_t));

    n->saturation_global /= 100.f;
    n->mask_grey_fulcrum = 0.1845f;
    n->vibrance = 0.f;
    n->grey_fulcrum = 0.1845f;
    n->contrast = 0.f;
    n->saturation_formula = DT_COLORBALANCE_SATURATION_JZAZBZ;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_colorbalancergb_params_v5_t);
    *new_version = 5;
    return 0;
  }

  if(old_version == 2)
  {
    typedef struct dt_iop_colorbalancergb_params_v2_t
    {
      /* params of v1 */
      float shadows_Y;
      float shadows_C;
      float shadows_H;
      float midtones_Y;
      float midtones_C;
      float midtones_H;
      float highlights_Y;
      float highlights_C;
      float highlights_H;
      float global_Y;
      float global_C;
      float global_H;
      float shadows_weight;
      float white_fulcrum;
      float highlights_weight;
      float chroma_shadows;
      float chroma_highlights;
      float chroma_global;
      float chroma_midtones;
      float saturation_global;
      float saturation_highlights;
      float saturation_midtones;
      float saturation_shadows;
      float hue_angle;

      /* params of v2 */
      float brilliance_global;
      float brilliance_highlights;
      float brilliance_midtones;
      float brilliance_shadows;
    } dt_iop_colorbalancergb_params_v2_t;

    const dt_iop_colorbalancergb_params_v2_t *o = old_params;
    dt_iop_colorbalancergb_params_v5_t *n =
      malloc(sizeof(dt_iop_colorbalancergb_params_v5_t));

    // Init params with defaults
    memcpy(n, &default_v5, sizeof(dt_iop_colorbalancergb_params_v5_t));

    // Copy the common part of the params struct
    memcpy(n, o, sizeof(dt_iop_colorbalancergb_params_v2_t));

    n->mask_grey_fulcrum = 0.1845f;
    n->vibrance = 0.f;
    n->grey_fulcrum = 0.1845f;
    n->contrast = 0.f;
    n->saturation_formula = DT_COLORBALANCE_SATURATION_JZAZBZ;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_colorbalancergb_params_v5_t);
    *new_version = 5;
    return 0;
  }
  if(old_version == 3)
  {
    typedef struct dt_iop_colorbalancergb_params_v3_t
    {
      /* params of v1 */
      float shadows_Y;
      float shadows_C;
      float shadows_H;
      float midtones_Y;
      float midtones_C;
      float midtones_H;
      float highlights_Y;
      float highlights_C;
      float highlights_H;
      float global_Y;
      float global_C;
      float global_H;
      float shadows_weight;
      float white_fulcrum;
      float highlights_weight;
      float chroma_shadows;
      float chroma_highlights;
      float chroma_global;
      float chroma_midtones;
      float saturation_global;
      float saturation_highlights;
      float saturation_midtones;
      float saturation_shadows;
      float hue_angle;

      /* params of v2 */
      float brilliance_global;
      float brilliance_highlights;
      float brilliance_midtones;
      float brilliance_shadows;

      /* params of v3 */
      float mask_grey_fulcrum;
    } dt_iop_colorbalancergb_params_v3_t;

    const dt_iop_colorbalancergb_params_v3_t *o = old_params;
    dt_iop_colorbalancergb_params_v5_t *n =
      malloc(sizeof(dt_iop_colorbalancergb_params_v5_t));

    // Init params with defaults
    memcpy(n, &default_v5, sizeof(dt_iop_colorbalancergb_params_v5_t));

    // Copy the common part of the params struct
    memcpy(n, o, sizeof(dt_iop_colorbalancergb_params_v3_t));

    n->vibrance = 0.f;
    n->grey_fulcrum = 0.1845f;
    n->contrast = 0.f;
    n->saturation_formula = DT_COLORBALANCE_SATURATION_JZAZBZ;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_colorbalancergb_params_v5_t);
    *new_version = 5;
    return 0;
  }
  if(old_version == 4)
  {
    typedef struct dt_iop_colorbalancergb_params_v4_t
    {
      /* params of v1 */
      float shadows_Y;
      float shadows_C;
      float shadows_H;
      float midtones_Y;
      float midtones_C;
      float midtones_H;
      float highlights_Y;
      float highlights_C;
      float highlights_H;
      float global_Y;
      float global_C;
      float global_H;
      float shadows_weight;
      float white_fulcrum;
      float highlights_weight;
      float chroma_shadows;
      float chroma_highlights;
      float chroma_global;
      float chroma_midtones;
      float saturation_global;
      float saturation_highlights;
      float saturation_midtones;
      float saturation_shadows;
      float hue_angle;

      /* params of v2 */
      float brilliance_global;
      float brilliance_highlights;
      float brilliance_midtones;
      float brilliance_shadows;

      /* params of v3 */
      float mask_grey_fulcrum;

      /* params of v4 */
      float vibrance;
      float grey_fulcrum;
      float contrast;
    } dt_iop_colorbalancergb_params_v4_t;

    const dt_iop_colorbalancergb_params_v4_t *o = old_params;
    dt_iop_colorbalancergb_params_v5_t *n =
      malloc(sizeof(dt_iop_colorbalancergb_params_v5_t));

    // Init params with defaults
    memcpy(n, &default_v5, sizeof(dt_iop_colorbalancergb_params_v5_t));

    // Copy the common part of the params struct
    memcpy(n, o, sizeof(dt_iop_colorbalancergb_params_v4_t));

    n->saturation_formula = DT_COLORBALANCE_SATURATION_JZAZBZ;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_colorbalancergb_params_v5_t);
    *new_version = 5;
    return 0;
  }

  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  // Note : all the elements of the params structure are scalar floats,
  // so we can just init them all to 0.f in batch
  // Then, only 4 params have to be manually inited to non-zero values
  dt_iop_colorbalancergb_params_t p = { 0.f };
  p.shadows_weight = 1.f;        // DEFAULT: 1.0 DESCRIPTION: "shadows fall-off"
  p.highlights_weight = 1.f;     // DEFAULT: 1.0 DESCRIPTION: "highlights fall-off"
  p.mask_grey_fulcrum = 0.1845f; // DEFAULT: 0.1845 DESCRIPTION: "mask middle-gray fulcrum"
  p.grey_fulcrum = 0.1845f;      // DEFAULT: 0.1845 DESCRIPTION: "contrast gray fulcrum"
  p.saturation_formula = DT_COLORBALANCE_SATURATION_JZAZBZ;

  // preset
  p.chroma_global = 0.2f;
  p.saturation_shadows = 0.1f;
  p.saturation_midtones = 0.05f;
  p.saturation_highlights = -0.05f;

  dt_gui_presets_add_generic(_("basic colorfulness | legacy"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  p.saturation_formula = DT_COLORBALANCE_SATURATION_DTUCS;
  p.chroma_global = 0.f;

  p.saturation_global = 0.2f;
  p.saturation_shadows = 0.30f;
  p.saturation_midtones = 0.f;
  p.saturation_highlights = -0.5f;
  dt_gui_presets_add_generic(_("basic colorfulness | natural skin"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  p.saturation_global = 0.2f;
  p.saturation_shadows = 0.5f;
  p.saturation_midtones = 0.f;
  p.saturation_highlights = -0.25f;
  dt_gui_presets_add_generic(_("basic colorfulness | vibrant colors"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  p.saturation_global = 0.2f;
  p.saturation_shadows = 0.25f;
  p.saturation_midtones = 0.f;
  p.saturation_highlights = -0.25f;
  dt_gui_presets_add_generic(_("basic colorfulness | standard"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
}


DT_OMP_DECLARE_SIMD(aligned(output, output_comp: 16) uniform(shadows_weight, midtones_weight, highlights_weight))
static inline void opacity_masks(const float x,
                                 const float shadows_weight, const float highlights_weight,
                                 const float midtones_weight, const float mask_grey_fulcrum,
                                 dt_aligned_pixel_t output, dt_aligned_pixel_t output_comp)
{
  const float x_offset = (x - mask_grey_fulcrum);
  const float x_offset_norm = x_offset / mask_grey_fulcrum;
  const float alpha = 1.f / (1.f + expf(x_offset_norm * shadows_weight));    // opacity of shadows
  const float beta = 1.f / (1.f + expf(-x_offset_norm * highlights_weight)); // opacity of highlights
  const float alpha_comp = 1.f - alpha;
  const float beta_comp = 1.f - beta;
  const float gamma = expf(-sqf(x_offset) * midtones_weight / 4.f) * sqf(alpha_comp) * sqf(beta_comp) * 8.f; // opacity of midtones
  const float gamma_comp = 1.f - gamma;

  output[0] = alpha;
  output[1] = gamma;
  output[2] = beta;
  output[3] = 0.f;

  if(output_comp)
  {
    output_comp[0] = alpha_comp;
    output_comp[1] = gamma_comp;
    output_comp[2] = beta_comp;
    output_comp[3] = 0.f;
  }
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorbalancergb_data_t *d = piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return; // no point


  // work profile can't be fetched in commit_params since it is not yet initialised
  // work_profile->matrix_in === RGB_to_XYZ
  // work_profile->matrix_out === XYZ_to_RGB

  // Premultiply the input matrices

  /* What we do here is equivalent to :

    // go to CIE 1931 XYZ 2° D50
    dot_product(RGB, RGB_to_XYZ, XYZ_D50); // matrice product

    // chroma adapt D50 to D65
    XYZ_D50_to_65(XYZ_D50, XYZ_D65);       // matrice product

    // go to CIE 2006 LMS
    XYZ_to_LMS(XYZ_D65, LMS);              // matrice product

  * so we pre-multiply the 3 conversion matrices and operate only one matrix product
  */
  dt_colormatrix_t input_matrix = { { 0.0f } };
  dt_colormatrix_t output_matrix = { { 0.0f } };

  dt_colormatrix_mul(output_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in); // output_matrix used as temp buffer
  dt_colormatrix_mul(input_matrix, XYZ_D65_to_LMS_2006_D65, output_matrix);
  dt_colormatrix_t input_matrix_trans;
  dt_colormatrix_transpose(input_matrix_trans, input_matrix);

  // Premultiply the output matrix

  /* What we do here is equivalent to :
    XYZ_D65_to_50(XYZ_D65, XYZ_D50);           // matrix product
    dot_product(XYZ_D50, XYZ_to_RGB, pix_out); // matrix product
  */

  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);
  dt_colormatrix_t output_matrix_trans;
  dt_colormatrix_transpose(output_matrix_trans, output_matrix);

  const float *const restrict in = DT_IS_ALIGNED(((const float *const restrict)ivoid));
  float *const restrict out = DT_IS_ALIGNED(((float *const restrict)ovoid));
  const float *const restrict gamut_LUT = DT_IS_ALIGNED(((const float *const restrict)d->gamut_LUT));

  const float *const restrict global = DT_IS_ALIGNED_PIXEL((const float *const restrict)d->global);
  const float *const restrict highlights = DT_IS_ALIGNED_PIXEL((const float *const restrict)d->highlights);
  const float *const restrict shadows = DT_IS_ALIGNED_PIXEL((const float *const restrict)d->shadows);
  const float *const restrict midtones = DT_IS_ALIGNED_PIXEL((const float *const restrict)d->midtones);

  const float *const restrict chroma = DT_IS_ALIGNED_PIXEL((const float *const restrict)d->chroma);
  const float *const restrict saturation = DT_IS_ALIGNED_PIXEL((const float *const restrict)d->saturation);
  const float *const restrict brilliance = DT_IS_ALIGNED_PIXEL((const float *const restrict)d->brilliance);

  const gint mask_display
      = ((piece->pipe->type & DT_DEV_PIXELPIPE_FULL) && self->dev->gui_attached
         && g && g->mask_display);

  // pixel size of the checker background
  const size_t checker_1 = (mask_display) ? DT_PIXEL_APPLY_DPI(d->checker_size) : 0;
  const size_t checker_2 = 2 * checker_1;

  const float L_white = Y_to_dt_UCS_L_star(d->white_fulcrum);

  const float DT_ALIGNED_ARRAY hue_rotation_matrix[2][2] = {
    { cosf(d->hue_angle), -sinf(d->hue_angle) },
    { sinf(d->hue_angle),  cosf(d->hue_angle) },
  };

  const size_t npixels = (size_t)roi_out->height * roi_out->width;
  const size_t out_width = roi_out->width;

  DT_OMP_FOR()
  for(size_t k  = 0; k < 4 * npixels; k += 4)
  {
    // clip pipeline RGB
    dt_aligned_pixel_t RGB;
    copy_pixel(RGB, in + k);
    dt_vector_clipneg(RGB);

    // go to CIE 2006 LMS D65
    dt_aligned_pixel_t LMS;
    dt_apply_transposed_color_matrix(RGB, input_matrix_trans, LMS);

    /* The previous line is equivalent to :
      // go to CIE 1931 XYZ 2° D50
      dot_product(RGB, RGB_to_XYZ, XYZ_D50); // matrice product

      // chroma adapt D50 to D65
      XYZ_D50_to_65(XYZ_D50, XYZ_D65); // matrice product

      // go to CIE 2006 LMS
      XYZ_to_LMS(XYZ_D65, LMS); // matrice product
    */

    // go to Filmlight Yrg
    dt_aligned_pixel_t Yrg = { 0.f };
    LMS_to_Yrg(LMS, Yrg);

    // go to Ych
    dt_aligned_pixel_t Ych = { 0.f };
    Yrg_to_Ych(Yrg, Ych);

    // Sanitize input : no negative luminance
    Ych[0] = MAX(Ych[0], 0.f);

    // Opacities for luma masks
    dt_aligned_pixel_t opacities;
    dt_aligned_pixel_t opacities_comp;
    opacity_masks(powf(Ych[0], 0.4101205819200422f), // center middle grey in 50 %
                  d->shadows_weight, d->highlights_weight, d->midtones_weight,
                  d->mask_grey_fulcrum, opacities, opacities_comp);

    // Hue shift - do it now because we need the gamut limit at output hue right after
    // The hue rotation is implemented as a matrix multiplication.
    const float cos_h = Ych[2];
    const float sin_h = Ych[3];
    Ych[2] = hue_rotation_matrix[0][0] * cos_h + hue_rotation_matrix[0][1] * sin_h;
    Ych[3] = hue_rotation_matrix[1][0] * cos_h + hue_rotation_matrix[1][1] * sin_h;

    // Linear chroma : distance to achromatic at constant luminance in scene-referred
    const float chroma_boost = d->chroma_global + scalar_product(opacities, chroma);
    const float vibrance = d->vibrance * (1.0f - powf(Ych[1], fabsf(d->vibrance)));
    const float chroma_factor = MAX(1.f + chroma_boost + vibrance, 0.f);
    Ych[1] *= chroma_factor;

    // clip chroma at constant hue and Y if needed
    gamut_check_Yrg(Ych);

    // go to Yrg for real
    Ych_to_Yrg(Ych, Yrg);

    // Go to LMS
    Yrg_to_LMS(Yrg, LMS);

    // Go to Filmlight RGB
    LMS_to_gradingRGB(LMS, RGB);

    // Color balance
    for_four_channels(c, aligned(RGB, global))
    {
      // global : offset
      RGB[c] += global[c];
    }
    for_four_channels(c, aligned(RGB, opacities, opacities_comp, shadows, midtones, highlights:16))
    {
      //  highlights, shadows : 2 slopes with masking
      RGB[c] *= opacities_comp[2] * (opacities_comp[0] + opacities[0] * shadows[c]) + opacities[2] * highlights[c];
      // factorization of : (RGB[c] * (1.f - alpha) + RGB[c] * d->shadows[c] * alpha) * (1.f - beta)  + RGB[c] * d->highlights[c] * beta;
    }
    dt_aligned_pixel_t sign;
    for_each_channel(c)
      sign[c] = (RGB[c] < 0.f) ? -1.f : 1.f;
    dt_aligned_pixel_t abs_RGB;
    for_each_channel(c)
      abs_RGB[c] = fabsf(RGB[c]);
    dt_aligned_pixel_t scaled_RGB;
    for_each_channel(c)
      scaled_RGB[c] = abs_RGB[c] /d->white_fulcrum;
    dt_vector_powf(scaled_RGB, midtones, RGB);
    for_each_channel(c)
      RGB[c] = RGB[c] * sign[c] * d->white_fulcrum;

    // for the non-linear ops we need to go in Yrg again because RGB doesn't preserve color
    gradingRGB_to_LMS(RGB, LMS);
    LMS_to_Yrg(LMS, Yrg);

    // Y midtones power (gamma)
    Yrg[0] = powf(MAX(Yrg[0] / d->white_fulcrum, 0.f), d->midtones_Y) * d->white_fulcrum;

    // Y fulcrumed contrast
    Yrg[0] = d->grey_fulcrum * powf(Yrg[0] / d->grey_fulcrum, d->contrast);

    Yrg_to_LMS(Yrg, LMS);
    dt_aligned_pixel_t XYZ_D65 = { 0.f };
    LMS_to_XYZ(LMS, XYZ_D65);

    // Perceptual color adjustments
    if(d->saturation_formula == DT_COLORBALANCE_SATURATION_JZAZBZ)
    {
      dt_aligned_pixel_t Jab = { 0.f };
      dt_XYZ_2_JzAzBz(XYZ_D65, Jab);

      // Convert to JCh
      float JC[2] = { Jab[0], dt_fast_hypotf(Jab[1], Jab[2]) };   // brightness/chroma vector
      const float h = atan2f(Jab[2], Jab[1]);  // hue : (a, b) angle

      // Project JC onto S, the saturation eigenvector, with orthogonal vector O.
      // Note : O should be = (C * cosf(T) - J * sinf(T)) = 0 since S is the eigenvector,
      // so we add the chroma projected along the orthogonal axis to get some control value
      const float T = atan2f(JC[1], JC[0]); // angle of the eigenvector over the hue plane
      const float sin_T = sinf(T);
      const float cos_T = cosf(T);
      const float DT_ALIGNED_PIXEL M_rot_dir[2][2] = { {  cos_T,  sin_T },
                                                      { -sin_T,  cos_T } };
      const float DT_ALIGNED_PIXEL M_rot_inv[2][2] = { {  cos_T, -sin_T },
                                                      {  sin_T,  cos_T } };
      float SO[2];

      // brilliance & Saturation : mix of chroma and luminance
      const float boosts[2] = { 1.f + d->brilliance_global + scalar_product(opacities, brilliance),     // move in S direction
                                d->saturation_global + scalar_product(opacities, saturation) }; // move in O direction

      SO[0] = JC[0] * M_rot_dir[0][0] + JC[1] * M_rot_dir[0][1];
      SO[1] = SO[0] * MIN(MAX(T * boosts[1], -T), M_PI_F / 2.f - T);
      SO[0] = MAX(SO[0] * boosts[0], 0.f);

      // Project back to JCh, that is rotate back of -T angle
      JC[0] = MAX(SO[0] * M_rot_inv[0][0] + SO[1] * M_rot_inv[0][1], 0.f);
      JC[1] = MAX(SO[0] * M_rot_inv[1][0] + SO[1] * M_rot_inv[1][1], 0.f);

      // Gamut mapping
      const float out_max_sat_h = lookup_gamut(gamut_LUT, h);
      // if JC[0] == 0.f, the saturation / luminance ratio is infinite - assign the largest practical value we have
      const float sat = (JC[0] > 0.f) ? soft_clip(JC[1] / JC[0], 0.8f * out_max_sat_h, out_max_sat_h)
                                      : out_max_sat_h;
      const float max_C_at_sat = JC[0] * sat;
      // if sat == 0.f, the chroma is zero - assign the original luminance because there's no need to gamut map
      const float max_J_at_sat = (sat > 0.f) ? JC[1] / sat : JC[0];
      JC[0] = (JC[0] + max_J_at_sat) / 2.f;
      JC[1] = (JC[1] + max_C_at_sat) / 2.f;

      // Gamut-clip in Jch at constant hue and lightness,
      // e.g. find the max chroma available at current hue that doesn't
      // yield negative L'M'S' values, which will need to be clipped during conversion
      const float cos_H = cosf(h);
      const float sin_H = sinf(h);

      const float d0 = 1.6295499532821566e-11f;
      const float dd = -0.56f;
      float Iz = JC[0] + d0;
      Iz /= (1.f + dd - dd * Iz);
      Iz = MAX(Iz, 0.f);

      static const dt_colormatrix_t AI_trans
          = { {  1.0f,                 1.0f,                                1.0f, 0.0f },
              {  0.1386050432715393f, -0.1386050432715393f, -0.0960192420263190f, 0.0f },
              {  0.0580473161561189f, -0.0580473161561189f, -0.8118918960560390f, 0.0f } };

      // Do a test conversion to L'M'S'
      const dt_aligned_pixel_t IzAzBz = { Iz, JC[1] * cos_H, JC[1] * sin_H, 0.f };
      dt_apply_transposed_color_matrix(IzAzBz, AI_trans, LMS);

      // Clip chroma
      float max_C = JC[1];
      if(LMS[0] < 0.f)
        max_C = MIN(-Iz / (AI_trans[1][0] * cos_H + AI_trans[2][0] * sin_H), max_C);

      if(LMS[1] < 0.f)
        max_C = MIN(-Iz / (AI_trans[1][1] * cos_H + AI_trans[2][1] * sin_H), max_C);

      if(LMS[2] < 0.f)
        max_C = MIN(-Iz / (AI_trans[1][2] * cos_H + AI_trans[2][2] * sin_H), max_C);

      // Project back to JzAzBz for real
      Jab[0] = JC[0];
      Jab[1] = max_C * cos_H;
      Jab[2] = max_C * sin_H;

      dt_JzAzBz_2_XYZ(Jab, XYZ_D65);
    }
    else
    {
      dt_aligned_pixel_t xyY, JCH, HCB;
      dt_D65_XYZ_to_xyY(XYZ_D65, xyY);
      xyY_to_dt_UCS_JCH(xyY, L_white, JCH);
      dt_UCS_JCH_to_HCB(JCH, HCB);

      const float radius = dt_fast_hypotf(HCB[1], HCB[2]);
      const float sin_T = (radius > 0.f) ? HCB[1] / radius : 0.f;
      const float cos_T = (radius > 0.f) ? HCB[2] / radius : 0.f;
      const float DT_ALIGNED_PIXEL M_rot_inv[2][2] = { { cos_T,  sin_T }, { -sin_T, cos_T } };
      // This would be the full matrice of direct rotation if we didn't need only its last row
      //const float DT_ALIGNED_PIXEL M_rot_dir[2][2] = { { cos_T, -sin_T }, {  sin_T, cos_T } };

      const float P = MAX(FLT_MIN, HCB[1]); // as HCB[1] is at least zero we don't fiddle with sign
      const float W = sin_T * HCB[1] + cos_T * HCB[2];

      float a = MAX(1.f + d->saturation_global + scalar_product(opacities, saturation), 0.f);
      const float b = MAX(1.f + d->brilliance_global + scalar_product(opacities, brilliance), 0.f);

      const float max_a = dt_fast_hypotf(P, W) / P;
      a = soft_clip(a, 0.5f * max_a, max_a);

      const float P_prime = (a - 1.f) * P;
      const float W_prime = sqrtf(sqf(P) * (1.f - sqf(a)) + sqf(W)) * b;

      HCB[1] = MAX(M_rot_inv[0][0] * P_prime + M_rot_inv[0][1] * W_prime, 0.f);
      HCB[2] = MAX(M_rot_inv[1][0] * P_prime + M_rot_inv[1][1] * W_prime, 0.f);

      dt_UCS_HCB_to_JCH(HCB, JCH);

      // Gamut mapping
      const float max_colorfulness = lookup_gamut(gamut_LUT, JCH[2]); // WARNING : this is M²
      const float max_chroma = (15.932993652962535f * powf(JCH[0] * L_white, 0.6523997524738018f)
                                * powf(max_colorfulness, 0.6007557017508491f) / L_white);
      const dt_aligned_pixel_t JCH_gamut_boundary = { JCH[0], max_chroma, JCH[2], 0.f };
      dt_aligned_pixel_t HSB_gamut_boundary;
      dt_UCS_JCH_to_HSB(JCH_gamut_boundary, HSB_gamut_boundary);

      // Clip saturation at constant brightness
      dt_aligned_pixel_t HSB = { HCB[0], (HCB[2] > 0.f) ? HCB[1] / HCB[2] : 0.f, HCB[2], 0.f };
      HSB[1] = soft_clip(HSB[1], 0.8f * HSB_gamut_boundary[1], HSB_gamut_boundary[1]);

      dt_UCS_HSB_to_JCH(HSB, JCH);
      dt_UCS_JCH_to_xyY(JCH, L_white, xyY);
      dt_xyY_to_XYZ(xyY, XYZ_D65);
    }

    // Project back to D50 pipeline RGB
    dt_aligned_pixel_t pix_out;
    dt_apply_transposed_color_matrix(XYZ_D65, output_matrix_trans, pix_out);

    /* The previous line is equivalent to :
      XYZ_D65_to_50(XYZ_D65, XYZ_D50);           // matrix product
      dot_product(XYZ_D50, XYZ_to_RGB, pix_out); // matrix product
    */

    if(mask_display)
    {
      // draw checkerboard
      dt_aligned_pixel_t color;
      const size_t i = (k / 4) / out_width;
      const size_t j = (k / 4) % out_width;
      if(i % checker_1 < i % checker_2)
      {
        if(j % checker_1 < j % checker_2)
          copy_pixel(color, d->checker_color_2);
        else
          copy_pixel(color, d->checker_color_1);
      }
      else
      {
        if(j % checker_1 < j % checker_2)
          copy_pixel(color, d->checker_color_1);
        else
          copy_pixel(color, d->checker_color_2);
      }

      float opacity = opacities[g->mask_type];
      const float opacity_comp = 1.0f - opacity;

      dt_vector_clipneg(pix_out);
      for_four_channels(c, aligned(pix_out, color:16))
        pix_out[c] = opacity_comp * color[c] + opacity * pix_out[c];
      pix_out[3] = 1.0f; // alpha is opaque, we need to preview it
    }
    else
    {
      dt_vector_clipneg(pix_out);
    }
    copy_pixel_nontemporal(out + k, pix_out);
  }
  dt_omploop_sfence();	// ensure all nontemporal writes complete before we use them
}


#if HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorbalancergb_data_t *const d = piece->data;
  const dt_iop_colorbalancergb_global_data_t *const gd = self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  if(piece->colors != 4)
  {
    dt_control_log(_("colorbalance works only on RGB input"));
    return err;
  }

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  // Get working color profile
  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return err; // no point

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  cl_mem input_matrix_cl = NULL;
  cl_mem output_matrix_cl = NULL;
  cl_mem gamut_LUT_cl = NULL;
  cl_mem hue_rotation_matrix_cl = NULL;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  // repack the matrices as flat AVX2-compliant matrice
  // work profile can't be fetched in commit_params since it is not yet initialised
  // work_profile->matrix_in === RGB_to_XYZ
  // work_profile->matrix_out === XYZ_to_RGB

  // Premultiply the input matrices

  /* What we do here is equivalent to :

    // go to CIE 1931 XYZ 2° D50
    dot_product(RGB, RGB_to_XYZ, XYZ_D50); // matrice product

    // chroma adapt D50 to D65
    XYZ_D50_to_65(XYZ_D50, XYZ_D65);       // matrice product

    // go to CIE 2006 LMS
    XYZ_to_LMS(XYZ_D65, LMS);              // matrice product

  * so we pre-multiply the 3 conversion matrices and operate only one matrix product
  */
  dt_colormatrix_t input_matrix;
  dt_colormatrix_t output_matrix;

  dt_colormatrix_mul(output_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in); // output_matrix used as temp buffer
  dt_colormatrix_mul(input_matrix, XYZ_D65_to_LMS_2006_D65, output_matrix);

  // Premultiply the output matrix

  /* What we do here is equivalent to :
    XYZ_D65_to_50(XYZ_D65, XYZ_D50);           // matrix product
    dot_product(XYZ_D50, XYZ_to_RGB, pix_out); // matrix product
  */

  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

  input_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), input_matrix);
  output_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), output_matrix);
  gamut_LUT_cl = dt_opencl_copy_host_to_device_constant(devid, LUT_ELEM * sizeof(float), d->gamut_LUT);

  // Size of the checker
  const gint mask_display
      = ((piece->pipe->type & DT_DEV_PIXELPIPE_FULL) && self->dev->gui_attached
         && g && g->mask_display);
  const int checker_1 = (mask_display) ? DT_PIXEL_APPLY_DPI(d->checker_size) : 0;
  const int checker_2 = 2 * checker_1;
  const int mask_type = (mask_display) ? g->mask_type : 0;

  const float L_white = Y_to_dt_UCS_L_star(d->white_fulcrum);

  float hue_rotation_matrix[4]
    = { cosf(d->hue_angle), -sinf(d->hue_angle), sinf(d->hue_angle), cosf(d->hue_angle) };
  hue_rotation_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), hue_rotation_matrix);

  if(input_matrix_cl == NULL || output_matrix_cl == NULL || gamut_LUT_cl == NULL || hue_rotation_matrix_cl == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_colorbalance_rgb, width, height,
    CLARG(dev_in), CLARG(dev_out),
    CLARG(width), CLARG(height), CLARG(dev_profile_info), CLARG(input_matrix_cl), CLARG(output_matrix_cl),
    CLARG(gamut_LUT_cl), CLARG(d->shadows_weight), CLARG(d->highlights_weight), CLARG(d->midtones_weight),
    CLARG(d->mask_grey_fulcrum), CLARG(d->hue_angle), CLARG(d->chroma_global), CLARG(d->chroma), CLARG(d->vibrance),
    CLARG(d->global), CLARG(d->shadows), CLARG(d->highlights), CLARG(d->midtones), CLARG(d->white_fulcrum),
    CLARG(d->midtones_Y), CLARG(d->grey_fulcrum), CLARG(d->contrast), CLARG(d->brilliance_global),
    CLARG(d->brilliance), CLARG(d->saturation_global), CLARG(d->saturation), CLARG(mask_display), CLARG(mask_type),
    CLARG(checker_1), CLARG(checker_2), CLARG(d->checker_color_1), CLARG(d->checker_color_2), CLARG(L_white),
    CLARG(d->saturation_formula), CLARG(hue_rotation_matrix_cl));

error:
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_opencl_release_mem_object(input_matrix_cl);
  dt_opencl_release_mem_object(output_matrix_cl);
  dt_opencl_release_mem_object(gamut_LUT_cl);
  dt_opencl_release_mem_object(hue_rotation_matrix_cl);
  return err;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 8; // extended.cl in programs.conf
  dt_iop_colorbalancergb_global_data_t *gd = malloc(sizeof(dt_iop_colorbalancergb_global_data_t));

  self->data = gd;
  gd->kernel_colorbalance_rgb = dt_opencl_create_kernel(program, "colorbalancergb");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  const dt_iop_colorbalancergb_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_colorbalance_rgb);
  free(self->data);
  self->data = NULL;
}
#endif

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalancergb_data_t *d = piece->data;
  const dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)p1;

  d->checker_color_1[0] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/red"), 0.f, 1.f);
  d->checker_color_1[1] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/green"), 0.f, 1.f);
  d->checker_color_1[2] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/blue"), 0.f, 1.f);
  d->checker_color_1[3] = 1.f;

  d->checker_color_2[0] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/red"), 0.f, 1.f);
  d->checker_color_2[1] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/green"), 0.f, 1.f);
  d->checker_color_2[2] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/blue"), 0.f, 1.f);
  d->checker_color_2[3] = 1.f;

  d->checker_size = MAX(dt_conf_get_int("plugins/darkroom/colorbalancergb/checker/size"), 2);

  d->vibrance = p->vibrance;
  d->contrast = 1.0f + p->contrast; // that limits the user param range to [-1, 1], but it seems enough
  d->grey_fulcrum = p->grey_fulcrum;

  d->chroma_global = p->chroma_global;
  d->chroma[0] = p->chroma_shadows;
  d->chroma[1] = p->chroma_midtones;
  d->chroma[2] = p->chroma_highlights;
  d->chroma[3] = 0.f;

  d->saturation_global = p->saturation_global;
  d->saturation[0] = p->saturation_shadows;
  d->saturation[1] = p->saturation_midtones;
  d->saturation[2] = p->saturation_highlights;
  d->saturation[3] = 0.f;

  d->brilliance_global = p->brilliance_global;
  d->brilliance[0] = p->brilliance_shadows;
  d->brilliance[1] = p->brilliance_midtones;
  d->brilliance[2] = p->brilliance_highlights;
  d->brilliance[3] = 0.f;

  d->hue_angle = deg2radf(p->hue_angle);

  // measure the grading RGB of a pure white
  const dt_aligned_pixel_t Ych_norm = { 1.f, 0.f, 1.f, 0.f };
  dt_aligned_pixel_t RGB_norm = { 0.f };
  Ych_to_gradingRGB(Ych_norm, RGB_norm);
  dt_aligned_pixel_t Ych;

  // global
  {
    make_Ych(1.f, p->global_C, CONVENTIONAL_DEG_TO_YRG_RAD(p->global_H), Ych);
    Ych_to_gradingRGB(Ych, d->global);
    for(size_t c = 0; c < 4; c++) d->global[c] = (d->global[c] - RGB_norm[c]) + RGB_norm[c] * p->global_Y;
  }

  // shadows
  {
    make_Ych(1.f, p->shadows_C, CONVENTIONAL_DEG_TO_YRG_RAD(p->shadows_H), Ych);
    Ych_to_gradingRGB(Ych, d->shadows);
    for(size_t c = 0; c < 4; c++) d->shadows[c] = 1.f + (d->shadows[c] - RGB_norm[c]) + p->shadows_Y;
    d->shadows_weight = 2.f + p->shadows_weight * 2.f;
  }

  // highlights
  {
    make_Ych(1.f, p->highlights_C, CONVENTIONAL_DEG_TO_YRG_RAD(p->highlights_H), Ych);
    Ych_to_gradingRGB(Ych, d->highlights);
    for(size_t c = 0; c < 4; c++) d->highlights[c] = 1.f + (d->highlights[c] - RGB_norm[c]) + p->highlights_Y;
    d->highlights_weight = 2.f + p->highlights_weight * 2.f;
  }

  // midtones
  {
    make_Ych(1.f, p->midtones_C, CONVENTIONAL_DEG_TO_YRG_RAD(p->midtones_H), Ych);
    Ych_to_gradingRGB(Ych, d->midtones);
    for(size_t c = 0; c < 4; c++) d->midtones[c] = 1.f / (1.f + (d->midtones[c] - RGB_norm[c]));
    d->midtones_Y = 1.f / (1.f + p->midtones_Y);
    d->white_fulcrum = exp2f(p->white_fulcrum);
    d->midtones_weight = sqf(d->shadows_weight) * sqf(d->highlights_weight) /
      (sqf(d->shadows_weight) + sqf(d->highlights_weight));
    d->mask_grey_fulcrum = powf(p->mask_grey_fulcrum, 0.4101205819200422f);
  }

  if(p->saturation_formula != d->saturation_formula) d->lut_inited = FALSE;
  d->saturation_formula = p->saturation_formula;

  // Check if the RGB working profile has changed in pipe
  // WARNING: this function is not triggered upon working profile change,
  // so the gamut boundaries are wrong until we change some param in this module
  dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return;
  if(work_profile != d->work_profile)
  {
    d->lut_inited = FALSE;
    d->work_profile = work_profile;
  }

  // find the maximum chroma allowed by the current working gamut in conjunction to hue
  // this will be used to prevent users to mess up their images by pushing chroma out of gamut
  if(!d->lut_inited)
  {
    // Premultiply both matrices to go from D50 pipeline RGB to D65 XYZ in a single matrix dot product
    // instead of D50 pipeline to D50 XYZ (work_profile->matrix_in) and then D50 XYZ to D65 XYZ
    dt_colormatrix_t input_matrix;
    dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
    float *gamut_LUT = d->gamut_LUT;
    // make RGB values vary between [0; 1] in working space, convert to Ych and get the max(c(h)))
    if(p->saturation_formula == DT_COLORBALANCE_SATURATION_JZAZBZ)
    {
      float *const restrict sampler = dt_calloc_align_float(LUT_ELEM);
      DT_OMP_FOR(reduction(max: sampler[:LUT_ELEM]) collapse(3))
      for(size_t r = 0; r < STEPS; r++)
        for(size_t g = 0; g < STEPS; g++)
          for(size_t b = 0; b < STEPS; b++)
          {
            const dt_aligned_pixel_t rgb = { (float)r / (float)(STEPS - 1), (float)g / (float)(STEPS - 1),
                                            (float)b / (float)(STEPS - 1), 0.f };
            dt_aligned_pixel_t XYZ = { 0.f };
            float saturation = 0.f;
            float hue = 0.f;

            dot_product(rgb, input_matrix, XYZ); // Go from D50 pipeline RGB to D65 XYZ in one step

            dt_aligned_pixel_t Jab, Jch;
            dt_XYZ_2_JzAzBz(XYZ, Jab);           // this one expects D65 XYZ

            Jch[0] = Jab[0];
            Jch[1] = dt_fast_hypotf(Jab[2], Jab[1]);
            Jch[2] = atan2f(Jab[2], Jab[1]);

            saturation = (Jch[0] > 0.f) ? Jch[1] / Jch[0] : 0.f;
            hue = Jch[2];

            int index = roundf((LUT_ELEM - 1) * (hue + M_PI_F) / (2.f * M_PI_F));
            index += (index < 0) ? LUT_ELEM : 0;
            index -= (index >= LUT_ELEM) ? LUT_ELEM : 0;
            sampler[index] = fmaxf(sampler[index], saturation);
          }

     // anti-aliasing on the LUT (simple 5-taps 1D box average)
      for(size_t k = 2; k < LUT_ELEM - 2; k++)
        d->gamut_LUT[k] = (sampler[k - 2] + sampler[k - 1] + sampler[k] + sampler[k + 1] + sampler[k + 2]) / 5.f;

      // handle bounds
      d->gamut_LUT[0] = (sampler[LUT_ELEM - 2] + sampler[LUT_ELEM - 1] + sampler[0] + sampler[1] + sampler[2]) / 5.f;
      d->gamut_LUT[1] = (sampler[LUT_ELEM - 1] + sampler[0] + sampler[1] + sampler[2] + sampler[3]) / 5.f;
      d->gamut_LUT[LUT_ELEM - 1] = (sampler[LUT_ELEM - 3] + sampler[LUT_ELEM - 2] + sampler[LUT_ELEM - 1] + sampler[0] + sampler[1]) / 5.f;
      d->gamut_LUT[LUT_ELEM - 2] = (sampler[LUT_ELEM - 4] + sampler[LUT_ELEM - 3] + sampler[LUT_ELEM - 2] + sampler[LUT_ELEM - 1] + sampler[0]) / 5.f;
      dt_free_align(sampler);
    }
    else if(p->saturation_formula == DT_COLORBALANCE_SATURATION_DTUCS)
      dt_UCS_22_build_gamut_LUT(input_matrix, gamut_LUT);

    d->lut_inited = TRUE;
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc1_align_type(dt_iop_colorbalancergb_data_t);
  dt_iop_colorbalancergb_data_t *d = piece->data;
  d->gamut_LUT = dt_alloc_align_float(LUT_ELEM);
  d->lut_inited = FALSE;
  d->work_profile = NULL;
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_colorbalancergb_data_t *d = piece->data;
  if(d->gamut_LUT) dt_free_align(d->gamut_LUT);
  dt_free_align(piece->data);
  piece->data = NULL;
}

void pipe_RGB_to_Ych(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_aligned_pixel_t RGB,
                     dt_aligned_pixel_t Ych)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, pipe);
  if(work_profile == NULL) return; // no point

  dt_aligned_pixel_t XYZ_D50 = { 0.f };
  dt_aligned_pixel_t XYZ_D65 = { 0.f };

  dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50, work_profile->matrix_in_transposed, work_profile->lut_in,
                             work_profile->unbounded_coeffs_in, work_profile->lutsize,
                             work_profile->nonlinearlut);
  XYZ_D50_to_D65(XYZ_D50, XYZ_D65);
  XYZ_to_Ych(XYZ_D65, Ych);
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
