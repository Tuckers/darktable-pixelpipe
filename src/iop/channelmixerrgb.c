/*
  This file is part of darktable,
  Copyright (C) 2010-2025 darktable developers.

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

/** Note :
 * we use finite-math-only and fast-math because divisions by zero are manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "finite-math-only", "fp-contract=fast", "fast-math", \
                      "tree-vectorize", "no-math-errno")
#endif

// #define AI_ACTIVATED
/* AI feature not good enough so disabled for now
   If enabled there must be $DESCRIPTION: entries in illuminants.h for bauhaus
*/

#include "chart/common.h"
#include "common/chromatic_adaptation.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/colorchecker.h"
#include "common/opencl.h"
#include "common/illuminants.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/dttypes.h"
#include "develop/imageop_math.h"
#include "develop/openmp_maths.h"
#include "iop/iop_api.h"
#include "gaussian_elimination.h"

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

DT_MODULE_INTROSPECTION(3, dt_iop_channelmixer_rgb_params_t)

#define CHANNEL_SIZE 4
#define INVERSE_SQRT_3 0.5773502691896258f
#define COLOR_MIN -2.0
#define COLOR_MAX 2.0
#define ILLUM_X_MAX 360.0
#define ILLUM_Y_MAX 300.0
#define LIGHTNESS_MAX 100.0
#define HUE_MAX 360.0
#define CHROMA_MAX 128.0
#define TEMP_MIN 1667.
#define TEMP_MAX 25000.

typedef enum dt_iop_channelmixer_rgb_version_t
{
  CHANNELMIXERRGB_V_1 = 0, // $DESCRIPTION: "version 1 (2020)"
  CHANNELMIXERRGB_V_2 = 1, // $DESCRIPTION: "version 2 (2021)"
  CHANNELMIXERRGB_V_3 = 2, // $DESCRIPTION: "version 3 (Apr 2021)"
} dt_iop_channelmixer_rgb_version_t;

typedef struct dt_iop_channelmixer_rgb_params_t
{
  /* params of v1 and v2 */
  float red[CHANNEL_SIZE];         // $MIN: COLOR_MIN $MAX: COLOR_MAX
  float green[CHANNEL_SIZE];       // $MIN: COLOR_MIN $MAX: COLOR_MAX
  float blue[CHANNEL_SIZE];        // $MIN: COLOR_MIN $MAX: COLOR_MAX
  float saturation[CHANNEL_SIZE];  // $MIN: -2.0 $MAX: 2.0
  float lightness[CHANNEL_SIZE];   // $MIN: -2.0 $MAX: 2.0
  float grey[CHANNEL_SIZE];        // $MIN: -2.0 $MAX: 2.0
  gboolean normalize_R, normalize_G, normalize_B, normalize_sat, normalize_light, normalize_grey; // $DESCRIPTION: "normalize channels"
  dt_illuminant_t illuminant;      // $DEFAULT: DT_ILLUMINANT_D
  dt_illuminant_fluo_t illum_fluo; // $DEFAULT: DT_ILLUMINANT_FLUO_F3 $DESCRIPTION: "F source"
  dt_illuminant_led_t illum_led;   // $DEFAULT: DT_ILLUMINANT_LED_B5 $DESCRIPTION: "LED source"
  dt_adaptation_t adaptation;      // $DEFAULT: DT_ADAPTATION_CAT16
  float x, y;                      // $DEFAULT: 0.333
  float temperature;               // $MIN: TEMP_MIN $MAX: TEMP_MAX $DEFAULT: 5003.
  float gamut;                     // $MIN: 0.0 $MAX: 12.0 $DEFAULT: 1.0 $DESCRIPTION: "gamut compression"
  gboolean clip;                   // $DEFAULT: TRUE $DESCRIPTION: "clip negative RGB from gamut"

  /* params of v3 */
  dt_iop_channelmixer_rgb_version_t version; // $DEFAULT: CHANNELMIXERRGB_V_3 $DESCRIPTION: "saturation algorithm"

  /* always add new params after this so we can import legacy params with memcpy on the common part of the struct */

} dt_iop_channelmixer_rgb_params_t;


typedef enum dt_solving_strategy_t
{
  DT_SOLVE_OPTIMIZE_NONE = 0,
  DT_SOLVE_OPTIMIZE_LOW_SAT = 1,
  DT_SOLVE_OPTIMIZE_HIGH_SAT = 2,
  DT_SOLVE_OPTIMIZE_SKIN = 3,
  DT_SOLVE_OPTIMIZE_FOLIAGE = 4,
  DT_SOLVE_OPTIMIZE_SKY = 5,
  DT_SOLVE_OPTIMIZE_AVG_DELTA_E = 6,
  DT_SOLVE_OPTIMIZE_MAX_DELTA_E = 7,
} dt_solving_strategy_t;


typedef enum dt_spot_mode_t
{
  DT_SPOT_MODE_CORRECT = 0,
  DT_SPOT_MODE_MEASURE = 1,
  DT_SPOT_MODE_LAST
} dt_spot_mode_t;


typedef struct dt_iop_channelmixer_rbg_data_t
{
  dt_colormatrix_t MIX;
  float DT_ALIGNED_PIXEL saturation[CHANNEL_SIZE];
  float DT_ALIGNED_PIXEL lightness[CHANNEL_SIZE];
  float DT_ALIGNED_PIXEL grey[CHANNEL_SIZE];
  dt_aligned_pixel_t illuminant; // XYZ coordinates of illuminant
  float p, gamut;
  gboolean apply_grey;
  gboolean clip;
  dt_adaptation_t adaptation;
  dt_illuminant_t illuminant_type;
  dt_iop_channelmixer_rgb_version_t version;
} dt_iop_channelmixer_rbg_data_t;

typedef struct dt_iop_channelmixer_rgb_global_data_t
{
  int kernel_channelmixer_rgb_xyz;
  int kernel_channelmixer_rgb_cat16;
  int kernel_channelmixer_rgb_bradford_full;
  int kernel_channelmixer_rgb_bradford_linear;
  int kernel_channelmixer_rgb_rgb;
} dt_iop_channelmixer_rgb_global_data_t;


const char *name()
{
  return _("color calibration");
}

const char *aliases()
{
  return _("channel mixer|white balance|monochrome");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("perform color space corrections\n"
                                        "such as white balance, channels mixing\n"
                                        "and conversions to monochrome emulating film"),
                                      _("corrective or creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB or XYZ"),
                                      _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR;
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
  typedef struct dt_iop_channelmixer_rgb_params_v3_t
  {
    /* params of v1 and v2 */
    float red[CHANNEL_SIZE];
    float green[CHANNEL_SIZE];
    float blue[CHANNEL_SIZE];
    float saturation[CHANNEL_SIZE];
    float lightness[CHANNEL_SIZE];
    float grey[CHANNEL_SIZE];
    gboolean normalize_R, normalize_G, normalize_B, normalize_sat, normalize_light, normalize_grey;
    dt_illuminant_t illuminant;
    dt_illuminant_fluo_t illum_fluo;
    dt_illuminant_led_t illum_led;
    dt_adaptation_t adaptation;
    float x, y;
    float temperature;
    float gamut;
    gboolean clip;

    /* params of v3 */
    dt_iop_channelmixer_rgb_version_t version;

    /* always add new params after this so we can import legacy params with memcpy on the common part of the struct */

  } dt_iop_channelmixer_rgb_params_v3_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_channelmixer_rgb_params_v1_t
    {
      float red[CHANNEL_SIZE];
      float green[CHANNEL_SIZE];
      float blue[CHANNEL_SIZE];
      float saturation[CHANNEL_SIZE];
      float lightness[CHANNEL_SIZE];
      float grey[CHANNEL_SIZE];
      gboolean normalize_R, normalize_G, normalize_B, normalize_sat, normalize_light, normalize_grey;
      dt_illuminant_t illuminant;
      dt_illuminant_fluo_t illum_fluo;
      dt_illuminant_led_t illum_led;
      dt_adaptation_t adaptation;
      float x, y;
      float temperature;
      float gamut;
      gboolean clip;
    } dt_iop_channelmixer_rgb_params_v1_t;

    const dt_iop_channelmixer_rgb_params_v1_t *o = old_params;
    dt_iop_channelmixer_rgb_params_v3_t *n =
      malloc(sizeof(dt_iop_channelmixer_rgb_params_v3_t));

    // V1 and V2 use the same param structure but the normalize_grey
    // param had no effect since commit_params forced normalization no
    // matter what. So we re-import the params and force the param to
    // TRUE to keep edits.
    memcpy(n, o, sizeof(dt_iop_channelmixer_rgb_params_v1_t));

    n->normalize_grey = TRUE;

    // V2 and V3 use the same param structure but these :

    // swap the saturation parameters for R and B to put them in natural order
    const float R = n->saturation[0];
    const float B = n->saturation[2];
    n->saturation[0] = B;
    n->saturation[2] = R;

    // say that these params were created with legacy code
    n->version = CHANNELMIXERRGB_V_1;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_channelmixer_rgb_params_v3_t);
    *new_version = 3;
    return 0;
  }
  if(old_version == 2)
  {
    typedef struct dt_iop_channelmixer_rgb_params_v2_t
    {
      float red[CHANNEL_SIZE];
      float green[CHANNEL_SIZE];
      float blue[CHANNEL_SIZE];
      float saturation[CHANNEL_SIZE];
      float lightness[CHANNEL_SIZE];
      float grey[CHANNEL_SIZE];
      gboolean normalize_R, normalize_G, normalize_B, normalize_sat, normalize_light, normalize_grey;
      dt_illuminant_t illuminant;
      dt_illuminant_fluo_t illum_fluo;
      dt_illuminant_led_t illum_led;
      dt_adaptation_t adaptation;
      float x, y;
      float temperature;
      float gamut;
      gboolean clip;
    } dt_iop_channelmixer_rgb_params_v2_t;

    const dt_iop_channelmixer_rgb_params_v2_t *o = old_params;
    dt_iop_channelmixer_rgb_params_v3_t *n =
      malloc(sizeof(dt_iop_channelmixer_rgb_params_v3_t));

    memcpy(n, o, sizeof(dt_iop_channelmixer_rgb_params_v2_t));

    // swap the saturation parameters for R and B to put them in natural order
    const float R = n->saturation[0];
    const float B = n->saturation[2];
    n->saturation[0] = B;
    n->saturation[2] = R;

    // say that these params were created with legacy code
    n->version = CHANNELMIXERRGB_V_1;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_channelmixer_rgb_params_v3_t);
    *new_version = 3;
    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  // auto-applied scene-referred default
  self->pref_based_presets = TRUE;

  const gboolean is_scene_referred = dt_is_scene_referred();

  if(is_scene_referred)
  {
    dt_gui_presets_add_generic
      (_("scene-referred default"), self->op, self->version(),
       NULL, 0,
       TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

    dt_gui_presets_update_format(BUILTIN_PRESET("scene-referred default"), self->op,
                                 self->version(), FOR_MATRIX);

    dt_gui_presets_update_autoapply(BUILTIN_PRESET("scene-referred default"),
                                    self->op, self->version(), TRUE);
  }

  // others

  dt_iop_channelmixer_rgb_params_t p;
  memset(&p, 0, sizeof(p));

  p.version = CHANNELMIXERRGB_V_3;

  // bypass adaptation
  p.illuminant = DT_ILLUMINANT_PIPE;
  p.adaptation = DT_ADAPTATION_XYZ;

  // set everything to no-op
  p.gamut = 0.f;
  p.clip = FALSE;
  p.illum_fluo = DT_ILLUMINANT_FLUO_F3;
  p.illum_led = DT_ILLUMINANT_LED_B5;
  p.temperature = 5003.f;
  illuminant_to_xy(DT_ILLUMINANT_PIPE, NULL, NULL, &p.x, &p.y, p.temperature,
                   DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);

  p.red[0] = 1.f;
  p.red[1] = 0.f;
  p.red[2] = 0.f;
  p.green[0] = 0.f;
  p.green[1] = 1.f;
  p.green[2] = 0.f;
  p.blue[0] = 0.f;
  p.blue[1] = 0.f;
  p.blue[2] = 1.f;

  p.saturation[0] = 0.f;
  p.saturation[1] = 0.f;
  p.saturation[2] = 0.f;
  p.lightness[0] = 0.f;
  p.lightness[1] = 0.f;
  p.lightness[2] = 0.f;
  p.grey[0] = 0.f;
  p.grey[1] = 0.f;
  p.grey[2] = 0.f;

  p.normalize_R = FALSE;
  p.normalize_G = FALSE;
  p.normalize_B = FALSE;
  p.normalize_sat = FALSE;
  p.normalize_light = FALSE;
  p.normalize_grey = TRUE;

  // Create B&W presets
  p.clip = TRUE;
  p.grey[0] = 0.f;
  p.grey[1] = 1.f;
  p.grey[2] = 0.f;

  dt_gui_presets_add_generic(_("monochrome | luminance-based"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // film emulations

  /* These emulations are built using spectral sensitivities provided
  * by film manufacturers for tungsten light, corrected in spectral
  * domain for D50 illuminant, and integrated in spectral space
  * against CIE 2° 1931 XYZ color matching functions in the Python lib
  * Colour, with the following code :
  *
    import colour
    import numpy as np

    XYZ = np.zeros((3))

    for l in range(360, 830):
        XYZ += film_CMF[l]
          * colour.colorimetry.STANDARD_OBSERVERS_CMFS
              ['CIE 1931 2 Degree Standard Observer'][l]
                / colour.ILLUMINANTS_SDS['A'][l] * colour.ILLUMINANTS_SDS['D50'][l]

    XYZ / np.sum(XYZ)
  *
  * The film CMF is visually approximated from the graph. It is still
  * more accurate than bullshit factors in legacy channel mixer that
  * don't even say in which RGB space they are supposed to be applied.
  */

  // ILFORD HP5 +
  // https://www.ilfordphoto.com/amfile/file/download/file/1903/product/695/
  p.grey[0] = 0.25304098f;
  p.grey[1] = 0.25958747f;
  p.grey[2] = 0.48737156f;

  dt_gui_presets_add_generic(_("monochrome | ILFORD HP5+"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // ILFORD Delta 100
  // https://www.ilfordphoto.com/amfile/file/download/file/3/product/681/
  p.grey[0] = 0.24552374f;
  p.grey[1] = 0.25366007f;
  p.grey[2] = 0.50081619f;

  dt_gui_presets_add_generic(_("monochrome | ILFORD DELTA 100"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // ILFORD Delta 400 and 3200 - they have the same curve
  // https://www.ilfordphoto.com/amfile/file/download/file/1915/product/685/
  // https://www.ilfordphoto.com/amfile/file/download/file/1913/product/683/
  p.grey[0] = 0.24376712f;
  p.grey[1] = 0.23613559f;
  p.grey[2] = 0.52009729f;

  dt_gui_presets_add_generic(_("monochrome | ILFORD DELTA 400 - 3200"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // ILFORD FP4+
  // https://www.ilfordphoto.com/amfile/file/download/file/1919/product/690/
  p.grey[0] = 0.24149085f;
  p.grey[1] = 0.22149272f;
  p.grey[2] = 0.53701643f;

  dt_gui_presets_add_generic(_("monochrome | ILFORD FP4+"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // Fuji Acros 100
  // https://dacnard.wordpress.com/2013/02/15/the-real-shades-of-gray-bw-film-is-a-matter-of-heart-pt-1/
  p.grey[0] = 0.333f;
  p.grey[1] = 0.313f;
  p.grey[2] = 0.353f;

  dt_gui_presets_add_generic(_("monochrome | Fuji Acros 100"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // Kodak ?
  // can't find spectral sensitivity curves and the illuminant under which they are produced,
  // so ¯\_(ツ)_/¯

  // basic channel-mixer
  p.adaptation = DT_ADAPTATION_RGB; // bypass adaptation
  p.grey[0] = 0.f;
  p.grey[1] = 0.f;
  p.grey[2] = 0.f;
  p.normalize_R = TRUE;
  p.normalize_G = TRUE;
  p.normalize_B = TRUE;
  p.normalize_grey = FALSE;
  p.clip = FALSE;
  dt_gui_presets_add_generic(_("basic channel mixer"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // swap G-B
  p.red[0] = 1.f;
  p.red[1] = 0.f;
  p.red[2] = 0.f;
  p.green[0] = 0.f;
  p.green[1] = 0.f;
  p.green[2] = 1.f;
  p.blue[0] = 0.f;
  p.blue[1] = 1.f;
  p.blue[2] = 0.f;
  dt_gui_presets_add_generic(_("channel swap | swap G and B"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // swap G-R
  p.red[0] = 0.f;
  p.red[1] = 1.f;
  p.red[2] = 0.f;
  p.green[0] = 1.f;
  p.green[1] = 0.f;
  p.green[2] = 0.f;
  p.blue[0] = 0.f;
  p.blue[1] = 0.f;
  p.blue[2] = 1.f;
  dt_gui_presets_add_generic(_("channel swap | swap G and R"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // swap R-B
  p.red[0] = 0.f;
  p.red[1] = 0.f;
  p.red[2] = 1.f;
  p.green[0] = 0.f;
  p.green[1] = 1.f;
  p.green[2] = 0.f;
  p.blue[0] = 1.f;
  p.blue[1] = 0.f;
  p.blue[2] = 0.f;
  dt_gui_presets_add_generic(_("channel swap | swap R and B"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
}

static gboolean _dev_is_D65_chroma(const dt_develop_t *dev)
{
  const dt_dev_chroma_t *chr = &dev->chroma;
  return chr->late_correction
    ? dt_dev_equal_chroma(chr->wb_coeffs, chr->as_shot)
    : dt_dev_equal_chroma(chr->wb_coeffs, chr->D65coeffs);
}


static gboolean _get_white_balance_coeff(const dt_iop_module_t *self,
                                         dt_aligned_pixel_t custom_wb)
{
  const dt_dev_chroma_t *chr = &self->dev->chroma;

  // Init output with a no-op
  for_four_channels(k)
    custom_wb[k] = 1.0f;

  if(!dt_image_is_matrix_correction_supported(&self->dev->image_storage))
    return TRUE;

  // If we use D65 there are unchanged corrections
  if(_dev_is_D65_chroma(self->dev))
    return FALSE;

  const gboolean valid_chroma =
    chr->D65coeffs[0] > 0.0 && chr->D65coeffs[1] > 0.0 && chr->D65coeffs[2] > 0.0;

  const gboolean changed_chroma =
    chr->wb_coeffs[0] > 1.0f || chr->wb_coeffs[1] > 1.0f || chr->wb_coeffs[2] > 1.0f;

  // Otherwise - for example because the user made a correct preset, find the
  // WB adaptation ratio
  if(valid_chroma && changed_chroma)
  {
    for_four_channels(k)
      custom_wb[k] = (float)chr->D65coeffs[k] / chr->wb_coeffs[k];
  }
  return FALSE;
}


DT_OMP_DECLARE_SIMD(aligned(input, output:16) uniform(compression, clip))
static inline void _gamut_mapping(const dt_aligned_pixel_t input,
                                  const float compression,
                                  const gboolean clip,
                                  dt_aligned_pixel_t output)
{
  // Get the sum XYZ
  const float sum = input[0] + input[1] + input[2];
  const float Y = input[1];

  // use chromaticity coordinates of reference white for sum == 0
  dt_aligned_pixel_t xyY = {  sum > 0.0f ? input[0] / sum : D50xyY.x,
                              sum > 0.0f ? input[1] / sum : D50xyY.y,
                              Y,
                              0.0f };

  // Convert to uvY
  dt_aligned_pixel_t uvY;
  dt_xyY_to_uvY(xyY, uvY);

  // Get the chromaticity difference with white point uv
  const float D50[2] DT_ALIGNED_PIXEL = { 0.20915914598542354f, 0.488075320769787f };
  const float delta[2] DT_ALIGNED_PIXEL = { D50[0] - uvY[0], D50[1] - uvY[1] };
  const float Delta = Y * (sqf(delta[0]) + sqf(delta[1]));

  // Compress chromaticity (move toward white point)
  const float correction = (compression == 0.0f) ? 0.f : powf(Delta, compression);
  for(size_t c = 0; c < 2; c++)
  {
    // Ensure the correction does not bring our uyY vector the other side of D50
    // that would switch to the opposite color, so we clip at D50
    // correction * delta[c] + uvY[c]
    const float tmp = DT_FMA(correction, delta[c], uvY[c]);
    uvY[c] = (uvY[c] > D50[c])  ? fmaxf(tmp, D50[c])
                                : fminf(tmp, D50[c]);
  }

  // Convert back to xyY
  dt_uvY_to_xyY(uvY, xyY);

  // Clip upon request
  if(clip) for(size_t c = 0; c < 2; c++) xyY[c] = fmaxf(xyY[c], 0.0f);

  // Check sanity of y
  // since we later divide by y, it can't be zero
  xyY[1] = fmaxf(xyY[1], NORM_MIN);

  // Check sanity of x and y :
  // since Z = Y (1 - x - y) / y, if x + y >= 1, Z will be negative
  const float scale = xyY[0] + xyY[1];
  const int sanitize = (scale >= 1.f);
  for(size_t c = 0; c < 2; c++) xyY[c] = (sanitize) ? xyY[c] / scale : xyY[c];

  // Convert back to XYZ
  dt_xyY_to_XYZ(xyY, output);
}


DT_OMP_DECLARE_SIMD(aligned(input, output, saturation, lightness:16) uniform(saturation, lightness))
static inline void _luma_chroma(const dt_aligned_pixel_t input,
                                const dt_aligned_pixel_t saturation,
                                const dt_aligned_pixel_t lightness,
                                dt_aligned_pixel_t output,
                                const dt_iop_channelmixer_rgb_version_t version)
{
  // Compute euclidean norm
  float norm = euclidean_norm(input);
  const float avg = fmaxf((input[0] + input[1] + input[2]) / 3.0f, NORM_MIN);

  if(norm > 0.f && avg > 0.f)
  {
    // Compute flat lightness adjustment
    const float mix = scalar_product(input, lightness);

    // Compensate the norm to get color ratios (R, G, B) = (1, 1, 1)
    // for grey (colorless) pixels.
    if(version == CHANNELMIXERRGB_V_3) norm *= INVERSE_SQRT_3;

    // Ratios
    for_three_channels(c)
      output[c] = input[c] / norm;

    // Compute ratios and a flat colorfulness adjustment for the whole pixel
    float coeff_ratio = 0.f;

    if(version == CHANNELMIXERRGB_V_1)
    {
      for_three_channels(c)
        coeff_ratio += sqf(1.0f - output[c]) * saturation[c];
    }
    else
      coeff_ratio = scalar_product(output, saturation) / 3.f;

    // Adjust the RGB ratios with the pixel correction
    for_three_channels(c)
    {
      // if the ratio was already invalid (negative), we accept the
      // result to be invalid too otherwise bright saturated blues end
      // up solid black
      const float min_ratio = (output[c] < 0.0f) ? output[c] : 0.0f;
      const float output_inverse = 1.0f - output[c];
      output[c] = fmaxf(DT_FMA(output_inverse, coeff_ratio, output[c]),
                        min_ratio); // output_inverse  * coeff_ratio + output
    }

    // The above interpolation between original pixel ratios and
    // (1, 1, 1) might change the norm of the ratios. Compensate for that.
    if(version == CHANNELMIXERRGB_V_3) norm /= euclidean_norm(output) * INVERSE_SQRT_3;

    // Apply colorfulness adjustment channel-wise and repack with
    // lightness to get LMS back
    norm *= fmaxf(1.f + mix / avg, 0.f);
    for_three_channels(c)
      output[c] *= norm;
  }
  else
  {
    // we have black, 0 stays 0, no luminance = no color
    for_three_channels(c)
      output[c] = input[c];
  }
}

DT_OMP_DECLARE_SIMD(aligned(in, out, XYZ_to_RGB, RGB_to_XYZ, MIX : 64) aligned(illuminant, saturation, lightness, grey:16))
static inline void _loop_switch(const float *const restrict in,
                                float *const restrict out,
                                const size_t width,
                                const size_t height,
                                const size_t ch,
                                const dt_colormatrix_t XYZ_to_RGB,
                                const dt_colormatrix_t RGB_to_XYZ,
                                const dt_colormatrix_t MIX,
                                const dt_aligned_pixel_t illuminant,
                                const dt_aligned_pixel_t saturation,
                                const dt_aligned_pixel_t lightness,
                                const dt_aligned_pixel_t grey,
                                const float p,
                                const float gamut,
                                const gboolean clip,
                                const gboolean apply_grey,
                                const dt_adaptation_t kind,
                                const dt_iop_channelmixer_rgb_version_t version)
{
  dt_colormatrix_t RGB_to_LMS = { { 0.0f, 0.0f, 0.0f, 0.0f } };
  dt_colormatrix_t MIX_to_XYZ = { { 0.0f, 0.0f, 0.0f, 0.0f } };
  switch (kind)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    case DT_ADAPTATION_LINEAR_BRADFORD:
      make_RGB_to_Bradford_LMS(RGB_to_XYZ, RGB_to_LMS);
      make_Bradford_LMS_to_XYZ(MIX, MIX_to_XYZ);
      break;
    case DT_ADAPTATION_CAT16:
      make_RGB_to_CAT16_LMS(RGB_to_XYZ, RGB_to_LMS);
      make_CAT16_LMS_to_XYZ(MIX, MIX_to_XYZ);
      break;
    case DT_ADAPTATION_XYZ:
      dt_colormatrix_copy(RGB_to_LMS, RGB_to_XYZ);
      dt_colormatrix_copy(MIX_to_XYZ, MIX);
      break;
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
      // RGB_to_LMS not applied, since we are not adapting WB
      dt_colormatrix_mul(MIX_to_XYZ, RGB_to_XYZ, MIX);
      break;
  }
  const float minval = clip ? 0.0f : -FLT_MAX;
  const dt_aligned_pixel_t min_value = { minval, minval, minval, minval };

  dt_colormatrix_t RGB_to_XYZ_trans;
  dt_colormatrix_transpose(RGB_to_XYZ_trans, RGB_to_XYZ);
  dt_colormatrix_t RGB_to_LMS_trans;
  dt_colormatrix_transpose(RGB_to_LMS_trans, RGB_to_LMS);
  dt_colormatrix_t MIX_to_XYZ_trans;
  dt_colormatrix_transpose(MIX_to_XYZ_trans, MIX_to_XYZ);
  dt_colormatrix_t XYZ_to_RGB_trans;
  dt_colormatrix_transpose(XYZ_to_RGB_trans, XYZ_to_RGB);

  DT_OMP_FOR()
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    // intermediate temp buffers
    dt_aligned_pixel_t temp_one;
    dt_aligned_pixel_t temp_two;

    dt_vector_max_nan(temp_two, &in[k], min_value);

    /* WE START IN PIPELINE RGB */

    switch(kind)
    {
      case DT_ADAPTATION_FULL_BRADFORD:
      {
        // Convert from RGB to XYZ
        dt_apply_transposed_color_matrix(temp_two, RGB_to_XYZ_trans, temp_one);
        const float Y = temp_one[1];

        // Convert to LMS
        convert_XYZ_to_bradford_LMS(temp_one, temp_two);
        // Do white balance
        downscale_vector(temp_two, Y);
        bradford_adapt_D50(temp_two, illuminant, p, TRUE, temp_one);
        upscale_vector(temp_one, Y);
        copy_pixel(temp_two, temp_one);
        break;
      }
      case DT_ADAPTATION_LINEAR_BRADFORD:
      {
        // Convert from RGB to XYZ to LMS
        dt_apply_transposed_color_matrix(temp_two, RGB_to_LMS_trans, temp_one);

        // Do white balance
        bradford_adapt_D50(temp_one, illuminant, p, FALSE, temp_two);
        break;
      }
      case DT_ADAPTATION_CAT16:
      {
        // Convert from RGB to XYZ
        dt_apply_transposed_color_matrix(temp_two, RGB_to_LMS_trans, temp_one);

        // Do white balance
        // force full-adaptation
        CAT16_adapt_D50(temp_one, illuminant, 1.0f, TRUE, temp_two);
        break;
      }
      case DT_ADAPTATION_XYZ:
      {
        // Convert from RGB to XYZ
        dt_apply_transposed_color_matrix(temp_two, RGB_to_XYZ_trans, temp_one);

        // Do white balance in XYZ
        XYZ_adapt_D50(temp_one, illuminant, temp_two);
        break;
      }
      case DT_ADAPTATION_RGB:
      case DT_ADAPTATION_LAST:
      default:
      {
        // No white balance.
        for_four_channels(c)
          temp_one[c] = 0.0f; //keep compiler happy by ensuring that always initialized
      }
    }

    // Compute the 3D mix - this is a rotation + homothety of the vector base
    dt_apply_transposed_color_matrix(temp_two, MIX_to_XYZ_trans, temp_one);

    /* FROM HERE WE ARE MANDATORILY IN XYZ - DATA IS IN temp_one */

    // Gamut mapping happens in XYZ space no matter what, only 0->1 values are defined
    // for this
    if(clip)
      dt_vector_clipneg_nan(temp_one);
    _gamut_mapping(temp_one, gamut, clip, temp_two);

    // convert to LMS, XYZ or pipeline RGB
    switch(kind)
    {
      case DT_ADAPTATION_FULL_BRADFORD:
      case DT_ADAPTATION_LINEAR_BRADFORD:
      case DT_ADAPTATION_CAT16:
      case DT_ADAPTATION_XYZ:
      {
        convert_any_XYZ_to_LMS(temp_two, temp_one, kind);
        break;
      }
      case DT_ADAPTATION_RGB:
      case DT_ADAPTATION_LAST:
      default:
      {
        // Convert from XYZ to RGB
        dt_apply_transposed_color_matrix(temp_two, XYZ_to_RGB_trans, temp_one);
        break;
      }
    }

    /* FROM HERE WE ARE IN LMS, XYZ OR PIPELINE RGB depending on user
       param - DATA IS IN temp_one */

    // Clip in LMS
    if(clip)
      dt_vector_clipneg_nan(temp_one);

    // Apply lightness / saturation adjustment
    _luma_chroma(temp_one, saturation, lightness, temp_two, version);

    // Clip in LMS
    if(clip)
      dt_vector_clipneg_nan(temp_two);

    // Save
    if(apply_grey)
    {
      // Turn LMS, XYZ or pipeline RGB into monochrome
      const float grey_mix = fmaxf(scalar_product(temp_two, grey), 0.0f);
      temp_two[0] = temp_two[1] = temp_two[2] = grey_mix;
    }
    else
    {
      // Convert back to XYZ
      switch(kind)
      {
        case DT_ADAPTATION_FULL_BRADFORD:
        case DT_ADAPTATION_LINEAR_BRADFORD:
        case DT_ADAPTATION_CAT16:
        case DT_ADAPTATION_XYZ:
        {
          convert_any_LMS_to_XYZ(temp_two, temp_one, kind);
          break;
        }
        case DT_ADAPTATION_RGB:
        case DT_ADAPTATION_LAST:
        default:
        {
          // Convert from RBG to XYZ
          dt_apply_transposed_color_matrix(temp_two, RGB_to_XYZ_trans, temp_one);
          break;
        }
      }

      /* FROM HERE WE ARE MANDATORILY IN XYZ - DATA IS IN temp_one */

      // Clip in XYZ
      if(clip)
        dt_vector_clipneg_nan(temp_one);

      // Convert back to RGB
      dt_apply_transposed_color_matrix(temp_one, XYZ_to_RGB_trans, temp_two);

      if(clip)
        dt_vector_clipneg_nan(temp_two);
    }

    temp_two[3] = in[k + 3]; // alpha mask
    copy_pixel_nontemporal(&out[k], temp_two);
  }
}

// util to shift pixel index without headache
#define SHF(ii, jj, c) ((i + ii) * width + j + jj) * ch + c
#define OFF 4


#ifdef AI_ACTIVATED

#if defined(__GNUC__) && defined(_WIN32)
  // On Windows there is a rounding issue making the image full
  // black. For a discussion about the issue and tested solutions see
  // PR #12382).
  #pragma GCC push_options
  #pragma GCC optimize ("-fno-finite-math-only")
#endif

static inline void _auto_detect_WB(const float *const restrict in,
                                   float *const restrict temp,
                                   dt_illuminant_t illuminant,
                                   const size_t width,
                                   const size_t height,
                                   const size_t ch,
                                   const dt_colormatrix_t RGB_to_XYZ,
                                   dt_aligned_pixel_t xyz)
{
   /* Detect the chromaticity of the illuminant based on the grey edges hypothesis.
      So we compute a laplacian filter and get the weighted average of its chromaticities

      Inspired by :
      A Fast White Balance Algorithm Based on Pixel Greyness, Ba Thai·Guang Deng·Robert Ross
      https://www.researchgate.net/profile/Ba_Son_Thai/publication/308692177_A_Fast_White_Balance_Algorithm_Based_on_Pixel_Greyness/

      Edge-Based Color Constancy, Joost van de Weijer, Theo Gevers, Arjan Gijsenij
      https://hal.inria.fr/inria-00548686/document
    */
    const float D50[2] = { D50xyY.x, D50xyY.y };
// Convert RGB to xy
  DT_OMP_FOR(collapse(2))
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = (i * width + j) * ch;
      dt_aligned_pixel_t RGB;
      dt_aligned_pixel_t XYZ;

      // Clip negatives
      for_each_channel(c,aligned(in))
        RGB[c] = fmaxf(in[index + c], 0.0f);

      // Convert to XYZ
      dot_product(RGB, RGB_to_XYZ, XYZ);

      // Convert to xyY
      const float sum = fmaxf(XYZ[0] + XYZ[1] + XYZ[2], NORM_MIN);
      XYZ[0] /= sum;   // x
      XYZ[2] = XYZ[1]; // Y
      XYZ[1] /= sum;   // y

      // Shift the chromaticity plane so the D50 point (target) becomes the origin
      const float norm = dt_fast_hypotf(D50[0], D50[1]);

      temp[index    ] = (XYZ[0] - D50[0]) / norm;
      temp[index + 1] = (XYZ[1] - D50[1]) / norm;
      temp[index + 2] =  XYZ[2];
    }

  float elements = 0.f;
  dt_aligned_pixel_t xyY = { 0.f };

  if(illuminant == DT_ILLUMINANT_DETECT_SURFACES)
  {
    DT_OMP_FOR(reduction(+:xyY, elements))
    for(size_t i = 2 * OFF; i < height - 4 * OFF; i += OFF)
      for(size_t j = 2 * OFF; j < width - 4 * OFF; j += OFF)
      {
        float DT_ALIGNED_PIXEL central_average[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          // B-spline local average / blur
          central_average[c] = (temp[SHF(-OFF, -OFF, c)]
                                + 2.f * temp[SHF(-OFF, 0, c)]
                                + temp[SHF(-OFF, +OFF, c)]
                                + 2.f * temp[SHF(   0, -OFF, c)]
                                + 4.f * temp[SHF(   0, 0, c)]
                                + 2.f * temp[SHF(   0, +OFF, c)]
                                + temp[SHF(+OFF, -OFF, c)]
                                + 2.f * temp[SHF(+OFF, 0, c)]
                                + temp[SHF(+OFF, +OFF, c)]) / 16.0f;
          central_average[c] = fmaxf(central_average[c], 0.0f);
        }

        dt_aligned_pixel_t var = { 0.f };

        // compute patch-wise variance
        // If variance = 0, we are on a flat surface and want to discard that patch.
        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          var[c] = (  sqf(temp[SHF(-OFF, -OFF, c)] - central_average[c])
                    + sqf(temp[SHF(-OFF,    0, c)] - central_average[c])
                    + sqf(temp[SHF(-OFF, +OFF, c)] - central_average[c])
                    + sqf(temp[SHF(0,    -OFF, c)] - central_average[c])
                    + sqf(temp[SHF(0,       0, c)] - central_average[c])
                    + sqf(temp[SHF(0,    +OFF, c)] - central_average[c])
                    + sqf(temp[SHF(+OFF, -OFF, c)] - central_average[c])
                    + sqf(temp[SHF(+OFF,    0, c)] - central_average[c])
                    + sqf(temp[SHF(+OFF, +OFF, c)] - central_average[c])
                    ) / 9.0f;
        }

        // Compute the patch-wise chroma covariance.
        // If covariance = 0, chroma channels are not correlated and we either have noise or chromatic aberrations.
        // Both ways, we want to discard that patch from the chroma average.
        var[2] = ((temp[SHF(-OFF, -OFF, 0)] - central_average[0]) * (temp[SHF(-OFF, -OFF, 1)] - central_average[1]) +
                  (temp[SHF(-OFF,    0, 0)] - central_average[0]) * (temp[SHF(-OFF,    0, 1)] - central_average[1]) +
                  (temp[SHF(-OFF, +OFF, 0)] - central_average[0]) * (temp[SHF(-OFF, +OFF, 1)] - central_average[1]) +
                  (temp[SHF(   0, -OFF, 0)] - central_average[0]) * (temp[SHF(   0, -OFF, 1)] - central_average[1]) +
                  (temp[SHF(   0,    0, 0)] - central_average[0]) * (temp[SHF(   0,    0, 1)] - central_average[1]) +
                  (temp[SHF(   0, +OFF, 0)] - central_average[0]) * (temp[SHF(   0, +OFF, 1)] - central_average[1]) +
                  (temp[SHF(+OFF, -OFF, 0)] - central_average[0]) * (temp[SHF(+OFF, -OFF, 1)] - central_average[1]) +
                  (temp[SHF(+OFF,    0, 0)] - central_average[0]) * (temp[SHF(+OFF,    0, 1)] - central_average[1]) +
                  (temp[SHF(+OFF, +OFF, 0)] - central_average[0]) * (temp[SHF(+OFF, +OFF, 1)] - central_average[1])
          ) / 9.0f;

        // Compute the Minkowski p-norm for regularization
        const float p = 8.f;
        const float p_norm
            = powf(powf(fabsf(central_average[0]), p)
                   + powf(fabsf(central_average[1]), p), 1.f / p) + NORM_MIN;
        const float weight = var[0] * var[1] * var[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++) xyY[c] += central_average[c] * weight / p_norm;
        elements += weight / p_norm;
      }
  }
  else if(illuminant == DT_ILLUMINANT_DETECT_EDGES)
  {
    DT_OMP_FOR(reduction(+:xyY, elements))
    for(size_t i = 2 * OFF; i < height - 4 * OFF; i += OFF)
      for(size_t j = 2 * OFF; j < width - 4 * OFF; j += OFF)
      {
        float DT_ALIGNED_PIXEL dd[2];
        float DT_ALIGNED_PIXEL central_average[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          // B-spline local average / blur
          central_average[c] = (temp[SHF(-OFF, -OFF, c)]
                                + 2.f * temp[SHF(-OFF, 0, c)]
                                + temp[SHF(-OFF, +OFF, c)]
                                + 2.f * temp[SHF(   0, -OFF, c)]
                                + 4.f * temp[SHF(   0, 0, c)]
                                + 2.f * temp[SHF(   0, +OFF, c)]
                                + temp[SHF(+OFF, -OFF, c)]
                                + 2.f * temp[SHF(+OFF, 0, c)]
                                + temp[SHF(+OFF, +OFF, c)]) / 16.0f;

          // image - blur = laplacian = edges
          dd[c] = temp[SHF(0, 0, c)] - central_average[c];
        }

        // Compute the Minkowski p-norm for regularization
        const float p = 8.f;
        const float p_norm = powf(powf(fabsf(dd[0]), p)
                                  + powf(fabsf(dd[1]), p), 1.f / p) + NORM_MIN;

#pragma unroll
        for(size_t c = 0; c < 2; c++) xyY[c] -= dd[c] / p_norm;
        elements += 1.f;
      }
  }

  const float norm_D50 = dt_fast_hypotf(D50[0], D50[1]);

  for(size_t c = 0; c < 2; c++)
    xyz[c] = norm_D50 * (xyY[c] / elements) + D50[c];
}

#if defined(__GNUC__) && defined(_WIN32)
  #pragma GCC pop_options
#endif

#endif // AI_ACTIVATED

static void _declare_cat_on_pipe(dt_iop_module_t *self, const gboolean preset)
{
  // Avertise in dev->chroma that we are doing chromatic adaptation here
  // preset = TRUE allows to capture the CAT a priori at init time
  const dt_iop_channelmixer_rgb_params_t *p = self->params;
  if(!g) return;

  dt_dev_chroma_t *chr = &self->dev->chroma;
  const dt_iop_module_t *origcat = chr->adaptation;

  if(preset
    || (self->enabled
        && !g->is_blending
        && !(p->adaptation == DT_ADAPTATION_RGB || p->illuminant == DT_ILLUMINANT_PIPE)))
  {
    // We do CAT here so we need to register this instance as CAT-handler.
    if(chr->adaptation == NULL)
    {
      // We are the first to try to register, let's go !
      chr->adaptation = self;
    }
    else if(chr->adaptation == self)
    {
    }
    else
    {
      // Another instance already registered.
      // If we are lower in the pipe than it, register in its place.
      if(dt_iop_is_first_instance(self->dev->iop, self))
        chr->adaptation = self;
    }
  }

  if(origcat != chr->adaptation)
    dt_print(DT_DEBUG_PIPE, "changed CAT for %s%s from %p to %p",
      self->op, dt_iop_get_instance_id(self), origcat, chr->adaptation);
}


static void _check_if_close_to_daylight(const float x,
                                        const float y,
                                        float *temperature,
                                        dt_illuminant_t *illuminant,
                                        dt_adaptation_t *adaptation)
{
  /* Check if a chromaticity x, y is close to daylight within 2.5 % error margin.
   * If so, we enable the daylight GUI for better ergonomics
   * Otherwise, we default to direct x, y control for better accuracy
   *
   * Note : The use of CCT is discouraged if dE > 5 % in CIE 1960 Yuv space
   *        reference : https://onlinelibrary.wiley.com/doi/abs/10.1002/9780470175637.ch3
   */

  // Get the correlated color temperature (CCT)
  float t = xy_to_CCT(x, y);

  // xy_to_CCT is valid only in 3000 - 25000 K. We need another model below
  if(t < 3000.f && t > 1667.f)
    t = CCT_reverse_lookup(x, y);

  if(temperature)
    *temperature = t;

  // Convert to CIE 1960 Yuv space
  const float xy_ref[2] = { x, y };
  float uv_ref[2];
  xy_to_uv(xy_ref, uv_ref);

  float xy_test[2] = { 0.f };
  float uv_test[2];

  // Compute the test chromaticity from the daylight model
  illuminant_to_xy(DT_ILLUMINANT_D, NULL, NULL, &xy_test[0], &xy_test[1], t,
                   DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);
  xy_to_uv(xy_test, uv_test);

  // Compute the error between the reference illuminant and the test
  // illuminant derivated from the CCT with daylight model
  const float delta_daylight = dt_fast_hypotf(uv_test[0] - uv_ref[0], uv_test[1] - uv_ref[1]);

  // Compute the test chromaticity from the blackbody model
  illuminant_to_xy(DT_ILLUMINANT_BB, NULL, NULL, &xy_test[0], &xy_test[1], t,
                   DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);
  xy_to_uv(xy_test, uv_test);

  // Compute the error between the reference illuminant and the test
  // illuminant derivated from the CCT with black body model
  const float delta_bb = dt_fast_hypotf(uv_test[0] - uv_ref[0], uv_test[1] - uv_ref[1]);

  // Check the error between original and test chromaticity
  if(delta_bb < 0.005f || delta_daylight < 0.005f)
  {
    if(illuminant)
    {
      if(delta_bb < delta_daylight)
        *illuminant = DT_ILLUMINANT_BB;
      else
        *illuminant = DT_ILLUMINANT_D;
    }
  }
  else
  {
    // error is too big to use a CCT-based model, we fall back to a
    // custom/freestyle chroma selection for the illuminant
    if(illuminant) *illuminant = DT_ILLUMINANT_CUSTOM;
  }

  // CAT16 is more accurate no matter the illuminant
  if(adaptation) *adaptation = DT_ADAPTATION_CAT16;
}

static inline void _compute_patches_delta_E(const float *const restrict patches,
                                            const dt_color_checker_t *const checker,
                                            float *const restrict delta_E,
                                            float *const restrict avg_delta_E,
                                            float *const restrict max_delta_E)
{
  // Compute the delta E

  float dE = 0.f;
  float max_dE = 0.f;

  for(size_t k = 0; k < checker->patches; k++)
  {
    // Convert to Lab
    dt_aligned_pixel_t Lab_test;
    dt_aligned_pixel_t XYZ_test;

    // If exposure was normalized, denormalized it before
    for(size_t c = 0; c < 4; c++) XYZ_test[c] = patches[k * 4 + c];
    dt_XYZ_to_Lab(XYZ_test, Lab_test);

    const float *const restrict Lab_ref = checker->values[k].Lab;

    // Compute delta E 2000 to make your computer heat
    // ref: https://en.wikipedia.org/wiki/Color_difference#CIEDE2000
    // note : it will only be luck if I didn't mess-up the computation somewhere
    const float DL = Lab_ref[0] - Lab_test[0];
    const float L_avg = (Lab_ref[0] + Lab_test[0]) / 2.f;
    const float C_ref = dt_fast_hypotf(Lab_ref[1], Lab_ref[2]);
    const float C_test = dt_fast_hypotf(Lab_test[1], Lab_test[2]);
    const float C_avg = (C_ref + C_test) / 2.f;
    float C_avg_7 = C_avg * C_avg; // C_avg²
    C_avg_7 *= C_avg_7;            // C_avg⁴
    C_avg_7 *= C_avg_7;            // C_avg⁸
    C_avg_7 /= C_avg;              // C_avg⁷
    // 25⁷ = 6103515625
    const float C_avg_7_ratio_sqrt = sqrtf(C_avg_7 / (C_avg_7 + 6103515625.f));
    const float a_ref_prime = Lab_ref[1] * (1.f + 0.5f * (1.f - C_avg_7_ratio_sqrt));
    const float a_test_prime = Lab_test[1] * (1.f + 0.5f * (1.f - C_avg_7_ratio_sqrt));
    const float C_ref_prime = dt_fast_hypotf(a_ref_prime, Lab_ref[2]);
    const float C_test_prime = dt_fast_hypotf(a_test_prime, Lab_test[2]);
    const float DC_prime = C_ref_prime - C_test_prime;
    const float C_avg_prime = (C_ref_prime + C_test_prime) / 2.f;
    float h_ref_prime = atan2f(Lab_ref[2], a_ref_prime);
    float h_test_prime = atan2f(Lab_test[2], a_test_prime);

    // Comply with recommendations, h = 0° where C = 0 by convention
    if(C_ref_prime == 0.f) h_ref_prime = 0.f;
    if(C_test_prime == 0.f) h_test_prime = 0.f;

    // Get the hue angles from [-pi ; pi] back to [0 ; 2 pi],
    // again, to comply with specifications
    if(h_ref_prime < 0.f) h_ref_prime = 2.f * M_PI_F - h_ref_prime;
    if(h_test_prime < 0.f) h_test_prime = 2.f * M_PI_F - h_test_prime;

    // Convert to degrees, again to comply with specs
    h_ref_prime = rad2degf(h_ref_prime);
    h_test_prime = rad2degf(h_test_prime);

    float Dh_prime = h_test_prime - h_ref_prime;
    float Dh_prime_abs = fabsf(Dh_prime);
    if(C_test_prime == 0.f || C_ref_prime == 0.f)
      Dh_prime = 0.f;
    else if(Dh_prime_abs <= 180.f)
      ;
    else if(Dh_prime_abs > 180.f && (h_test_prime <= h_ref_prime))
      Dh_prime += 360.f;
    else if(Dh_prime_abs > 180.f && (h_test_prime > h_ref_prime))
      Dh_prime -= 360.f;

    // update abs(Dh_prime) for later
    Dh_prime_abs = fabsf(Dh_prime);

    const float DH_prime =
      2.f * sqrtf(C_test_prime * C_ref_prime) * sinf(deg2radf(Dh_prime) / 2.f);

    float H_avg_prime = h_ref_prime + h_test_prime;
    if(C_test_prime == 0.f || C_ref_prime == 0.f)
      ;
    else if(Dh_prime_abs <= 180.f)
      H_avg_prime /= 2.f;
    else if(Dh_prime_abs > 180.f && (H_avg_prime < 360.f))
      H_avg_prime = (H_avg_prime + 360.f) / 2.f;
    else if(Dh_prime_abs > 180.f && (H_avg_prime >= 360.f))
      H_avg_prime = (H_avg_prime - 360.f) / 2.f;

    const float T = 1.f
                    - 0.17f * cosf(deg2radf(H_avg_prime - 30))
                    + 0.24f * cosf(2.f * deg2radf(H_avg_prime))
                    + 0.32f * cosf(3.f * deg2radf(H_avg_prime) + deg2radf(6.f))
                    - 0.20f * cosf(4.f * deg2radf(H_avg_prime) - deg2radf(63.f));

    const float S_L = 1.f + (0.015f * sqf(L_avg - 50.f)) / sqrtf(20.f + sqf(L_avg - 50.f));
    const float S_C = 1.f + 0.045f * C_avg_prime;
    const float S_H = 1.f + 0.015f * C_avg_prime * T;
    const float R_T = -2.f * C_avg_7_ratio_sqrt
                      * sinf(deg2radf(60.f) * expf(-sqf((H_avg_prime - 275.f) / 25.f)));

    // roll the drum, here goes the Delta E, finally…
    const float DE = sqrtf(sqf(DL / S_L) + sqf(DC_prime / S_C) + sqf(DH_prime / S_H)
                           + R_T * (DC_prime / S_C) * (DH_prime / S_H));

    // Delta E 1976 for reference :
    //float DE = sqrtf(sqf(Lab_test[0] - Lab_ref[0]) + sqf(Lab_test[1] - Lab_ref[1]) + sqf(Lab_test[2] - Lab_ref[2]));

    //fprintf(stdout, "patch %s : Lab ref \t= \t%.3f \t%.3f \t%.3f \n", checker->values[k].name, Lab_ref[0], Lab_ref[1], Lab_ref[2]);
    //fprintf(stdout, "patch %s : Lab mes \t= \t%.3f \t%.3f \t%.3f \n", checker->values[k].name, Lab_test[0], Lab_test[1], Lab_test[2]);
    //fprintf(stdout, "patch %s : dE mes \t= \t%.3f \n", checker->values[k].name, DE);

    delta_E[k] = DE;
    dE += DE / (float)checker->patches;
    if(DE > max_dE) max_dE = DE;
  }

  *avg_delta_E = dE;
  *max_delta_E = max_dE;
}

#define GET_WEIGHT                                                \
      float hue = atan2f(reference[2], reference[1]);             \
      const float chroma = hypotf(reference[2], reference[1]);    \
      float delta_hue = hue - ref_hue;                            \
      if(chroma == 0.f)                                           \
        delta_hue = 0.f;                                          \
      else if(fabsf(delta_hue) <= M_PI_F)                         \
        ;                                                         \
      else if(fabsf(delta_hue) > M_PI_F && (hue <= ref_hue))      \
        delta_hue += 2.f * M_PI_F;                                \
      else if(fabsf(delta_hue) > M_PI_F && (hue > ref_hue))       \
        delta_hue -= 2.f * M_PI_F;                                \
      w = sqrtf(expf(-sqf(delta_hue) / 2.f));


typedef struct {
  float black;
  float exposure;
} extraction_result_t;

static const extraction_result_t _extract_patches(const float *const restrict in,
                                                  const dt_iop_roi_t *const roi_in,
                                                  dt_iop_channelmixer_rgb_gui_data_t *g,
                                                  const dt_colormatrix_t RGB_to_XYZ,
                                                  const dt_colormatrix_t XYZ_to_CAM,
                                                  float *const restrict patches,
                                                  const gboolean normalize_exposure)
{
  const size_t width = roi_in->width;
  const size_t height = roi_in->height;
  const float radius_x =
    g->checker->radius * hypotf(1.f, g->checker->ratio) * g->safety_margin;
  const float radius_y = radius_x / g->checker->ratio;

  if(g->delta_E_in == NULL)
    g->delta_E_in = dt_alloc_align_float(g->checker->patches);

  /* Get the average color over each patch */
  for(size_t k = 0; k < g->checker->patches; k++)
  {
    // center of the patch in the ideal reference
    const point_t center = { g->checker->values[k].x, g->checker->values[k].y };

    // corners of the patch in the ideal reference
    const point_t corners[4] = { {center.x - radius_x, center.y - radius_y},
                                 {center.x + radius_x, center.y - radius_y},
                                 {center.x + radius_x, center.y + radius_y},
                                 {center.x - radius_x, center.y + radius_y} };

    // apply patch coordinates transform depending on perspective
    point_t new_corners[4];
    // find the bounding box of the patch at the same time
    size_t x_min = width - 1;
    size_t x_max = 0;
    size_t y_min = height - 1;
    size_t y_max = 0;
    for(size_t c = 0; c < 4; c++)
    {
      new_corners[c] = apply_homography(corners[c], g->homography);
      x_min = fminf(new_corners[c].x, x_min);
      x_max = fmaxf(new_corners[c].x, x_max);
      y_min = fminf(new_corners[c].y, y_min);
      y_max = fmaxf(new_corners[c].y, y_max);
    }

    x_min = CLAMP((size_t)floorf(x_min), 0, width - 1);
    x_max = CLAMP((size_t)ceilf(x_max), 0, width - 1);
    y_min = CLAMP((size_t)floorf(y_min), 0, height - 1);
    y_max = CLAMP((size_t)ceilf(y_max), 0, height - 1);

    // Get the average color on the patch
    patches[k * 4] = patches[k * 4 + 1] = patches[k * 4 + 2] = patches[k * 4 + 3] = 0.f;
    size_t num_elem = 0;

    // Loop through the rectangular bounding box
    for(size_t j = y_min; j < y_max; j++)
      for(size_t i = x_min; i < x_max; i++)
      {
        // Check if this pixel lies inside the sampling area and sample if it does
        point_t current_point = { i + 0.5f, j + 0.5f };
        current_point = apply_homography(current_point, g->inverse_homography);
        current_point.x -= center.x;
        current_point.y -= center.y;

        if(current_point.x < radius_x && current_point.x > -radius_x &&
           current_point.y < radius_y && current_point.y > -radius_y)
        {
          for_three_channels(c)
          {
            patches[k * 4 + c] += in[(j * width + i) * 4 + c];

            // Debug : inpaint a black square in the preview to ensure the coordanites of
            // overlay drawings and actual pixel processing match
            // out[(j * width + i) * 4 + c] = 0.f;
          }
          num_elem++;
        }
      }

    for_three_channels(c)
      patches[k * 4 + c] /= (float)num_elem;

    // Convert to XYZ
    dt_aligned_pixel_t XYZ = { 0 };
    dot_product(patches + k * 4, RGB_to_XYZ, XYZ);
    for(size_t c = 0; c < 3; c++) patches[k * 4 + c] = XYZ[c];
  }

  // find reference white patch
  dt_aligned_pixel_t XYZ_white_ref;
  dt_Lab_to_XYZ(g->checker->values[g->checker->white].Lab, XYZ_white_ref);
  const float white_ref_norm = euclidean_norm(XYZ_white_ref);

  // find test white patch
  dt_aligned_pixel_t XYZ_white_test;
  for_three_channels(c)
    XYZ_white_test[c] = patches[g->checker->white * 4 + c];
  const float white_test_norm = euclidean_norm(XYZ_white_test);

  /* match global exposure */
  // white exposure depends on camera settings and raw white point,
  // we want our profile to be independent from that
  float exposure = white_ref_norm / white_test_norm;

  /* Exposure compensation */
  // Ensure the relative luminance of the test patch (compared to white patch)
  // is the same as the relative luminance of the reference patch.
  // This compensate for lighting fall-off and unevenness
  if(normalize_exposure)
  {
    for(size_t k = 0; k < g->checker->patches; k++)
    {
      float *const sample = patches + k * 4;

      dt_aligned_pixel_t XYZ_ref;
      dt_Lab_to_XYZ(g->checker->values[k].Lab, XYZ_ref);

      const float sample_norm = euclidean_norm(sample);
      const float ref_norm = euclidean_norm(XYZ_ref);

      const float relative_luminance_test = sample_norm / white_test_norm;
      const float relative_luminance_ref = ref_norm / white_ref_norm;

      const float luma_correction = relative_luminance_ref / relative_luminance_test;
      for_three_channels(c)
        sample[c] *= luma_correction * exposure;
    }
  }

  // black point is evaluated by rawspeed on each picture using the dark pixels
  // we want our profile to be also independent from its discrepancies
  // so we convert back the patches to camera RGB space and search the best fit of
  // RGB_ref = exposure * (RGB_test - offset) for offset.
  float black = 0.f;
  const float user_exposure = exp2f(dt_dev_exposure_get_exposure(darktable.develop));
  const float user_black = dt_dev_exposure_get_black(darktable.develop);

  if(XYZ_to_CAM)
  {
    float mean_ref = 0.f;
    float mean_test = 0.f;

    for(size_t k = 0; k < g->checker->patches; k++)
    {
      dt_aligned_pixel_t XYZ_ref, RGB_ref;
      dt_aligned_pixel_t XYZ_test, RGB_test;

      for_three_channels(c)
        XYZ_test[c] = patches[k * 4 + c];
      dt_Lab_to_XYZ(g->checker->values[k].Lab, XYZ_ref);

      dot_product(XYZ_test, XYZ_to_CAM, RGB_test);
      dot_product(XYZ_ref, XYZ_to_CAM, RGB_ref);

      // Undo exposure module settings
      for_three_channels(c)
        RGB_test[c] = RGB_test[c] / user_exposure / exposure + user_black;

      // From now on, we have all the reference and test data in camera RGB space
      // where exposure and black level are applied

      for_three_channels(c)
      {
        mean_test += RGB_test[c];
        mean_ref += RGB_ref[c];
      }
    }
    mean_test /= 3.f * g->checker->patches;
    mean_ref /= 3.f * g->checker->patches;

    float variance = 0.f;
    float covariance = 0.f;

    for(size_t k = 0; k < g->checker->patches; k++)
    {
      dt_aligned_pixel_t XYZ_ref, RGB_ref;
      dt_aligned_pixel_t XYZ_test, RGB_test;

      for_three_channels(c) XYZ_test[c] = patches[k * 4 + c];
      dt_Lab_to_XYZ(g->checker->values[k].Lab, XYZ_ref);

      dot_product(XYZ_test, XYZ_to_CAM, RGB_test);
      dot_product(XYZ_ref, XYZ_to_CAM, RGB_ref);

      // Undo exposure module settings
      for_three_channels(c)
      {
        RGB_test[c] = RGB_test[c] / user_exposure / exposure + user_black;
      }

      for_three_channels(c)
      {
        variance += sqf(RGB_test[c] - mean_test);
        covariance += (RGB_ref[c] - mean_ref) * (RGB_test[c] - mean_ref);
      }
    }
    variance /= 3.f * g->checker->patches;
    covariance /= 3.f * g->checker->patches;

    // Here, we solve the least-squares problem RGB_ref = exposure * RGB_test + offset
    // using :
    //   exposure = covariance(RGB_test, RGB_ref) / variance(RGB_test)
    //   offset = mean(RGB_ref) - exposure * mean(RGB_test)
    exposure = covariance / variance;
    black = mean_ref - exposure * mean_test;
  }

  // the exposure module applies output  = (input - offset) * exposure
  // but we compute output = input * exposure + offset
  // so, rescale offset to adapt our offset to exposure module GUI
  black /= -exposure;

  const extraction_result_t result = { black, exposure };
  return result;
}


void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const restrict ivoid,
             void *const restrict ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_channelmixer_rbg_data_t *data = piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  const dt_iop_order_iccprofile_info_t *const input_profile =
    dt_ioppr_get_pipe_input_profile_info(piece->pipe);

  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/,
                                        self, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's
            // trouble flag has been updated

  if(piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    _declare_cat_on_pipe(self, FALSE);

  dt_colormatrix_t RGB_to_XYZ;
  dt_colormatrix_t XYZ_to_RGB;
  dt_colormatrix_t XYZ_to_CAM;

  // repack the matrices as flat AVX2-compliant matrice
  if(work_profile)
  {
    // work profile can't be fetched in commit_params since it is not yet initialised
    memcpy(RGB_to_XYZ, work_profile->matrix_in, sizeof(RGB_to_XYZ));
    memcpy(XYZ_to_RGB, work_profile->matrix_out, sizeof(XYZ_to_RGB));
    memcpy(XYZ_to_CAM, input_profile->matrix_out, sizeof(XYZ_to_CAM));
  }

  assert(piece->colors == 4);
  const size_t ch = 4;

  const float *const restrict in = (const float *const restrict)ivoid;
  float *const restrict out = (float *const restrict)ovoid;

  // auto-detect WB upon request

  if(data->illuminant_type == DT_ILLUMINANT_CAMERA)
  {
    // The camera illuminant is a behaviour rather than a preset of
    // values: it uses whatever is in the RAW EXIF. But it depends on
    // what temperature.c is doing and needs to be updated
    // accordingly, to give a consistent result.  We initialise the
    // CAT defaults using the temperature coeffs at startup, but if
    // temperature is changed later, we get no notification of the
    // change here, so we can't update the defaults.  So we need to
    // re-run the detection at runtime…
    float x, y;
    dt_aligned_pixel_t custom_wb;
    _get_white_balance_coeff(self, custom_wb);

    if(find_temperature_from_raw_coeffs(&(self->dev->image_storage), custom_wb, &(x), &(y)))
    {
      // Convert illuminant from xyY to XYZ
      dt_aligned_pixel_t XYZ;
      illuminant_xy_to_XYZ(x, y, XYZ);

      // Convert illuminant from XYZ to Bradford modified LMS
      convert_any_XYZ_to_LMS(XYZ, data->illuminant, data->adaptation);
      data->illuminant[3] = 0.f;
    }
    else
    {
      // just use whatever was defined in commit_params hoping the defaults work…
    }
  }

  // force loop unswitching in a controlled way
  switch(data->adaptation)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    {
      _loop_switch(in, out, roi_out->width, roi_out->height, ch,
                   XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                   data->illuminant, data->saturation, data->lightness, data->grey,
                   data->p, data->gamut, data->clip, data->apply_grey,
                   DT_ADAPTATION_FULL_BRADFORD, data->version);
      break;
    }
    case DT_ADAPTATION_LINEAR_BRADFORD:
    {
      _loop_switch(in, out, roi_out->width, roi_out->height, ch,
                   XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                   data->illuminant, data->saturation, data->lightness, data->grey,
                   data->p, data->gamut, data->clip, data->apply_grey,
                   DT_ADAPTATION_LINEAR_BRADFORD, data->version);
      break;
    }
    case DT_ADAPTATION_CAT16:
    {
      _loop_switch(in, out, roi_out->width, roi_out->height, ch,
                   XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                   data->illuminant, data->saturation, data->lightness, data->grey,
                   data->p, data->gamut, data->clip, data->apply_grey,
                   DT_ADAPTATION_CAT16, data->version);
      break;
    }
    case DT_ADAPTATION_XYZ:
    {
      _loop_switch(in, out, roi_out->width, roi_out->height, ch,
                   XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                   data->illuminant, data->saturation, data->lightness, data->grey,
                   data->p, data->gamut, data->clip, data->apply_grey,
                   DT_ADAPTATION_XYZ, data->version);
      break;
    }
    case DT_ADAPTATION_RGB:
    {
      _loop_switch(in, out, roi_out->width, roi_out->height, ch,
                   XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                   data->illuminant, data->saturation, data->lightness, data->grey,
                   data->p, data->gamut, data->clip, data->apply_grey,
                   DT_ADAPTATION_RGB, data->version);
      break;
    }
    case DT_ADAPTATION_LAST:
    default:
    {
      break;
    }
  }

  // run dE validation at output
}

#if HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               const cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_channelmixer_rbg_data_t *const d = piece->data;
  const dt_iop_channelmixer_rgb_global_data_t *const gd = self->global_data;

  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);

  if(piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    _declare_cat_on_pipe(self, FALSE);

  if(d->illuminant_type == DT_ILLUMINANT_CAMERA)
  {
    // The camera illuminant is a behaviour rather than a preset of
    // values: it uses whatever is in the RAW EXIF. But it depends on
    // what temperature.c is doing and needs to be updated
    // accordingly, to give a consistent result.  We initialise the
    // CAT defaults using the temperature coeffs at startup, but if
    // temperature is changed later, we get no notification of the
    // change here, so we can't update the defaults.  So we need to
    // re-run the detection at runtime…
    float x, y;
    dt_aligned_pixel_t custom_wb;
    _get_white_balance_coeff(self, custom_wb);

    if(find_temperature_from_raw_coeffs(&(self->dev->image_storage), custom_wb, &(x), &(y)))
    {
      // Convert illuminant from xyY to XYZ
      dt_aligned_pixel_t XYZ;
      illuminant_xy_to_XYZ(x, y, XYZ);

      // Convert illuminant from XYZ to Bradford modified LMS
      convert_any_XYZ_to_LMS(XYZ, d->illuminant, d->adaptation);
      d->illuminant[3] = 0.f;
    }
  }

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  if(piece->colors != 4)
  {
    dt_control_log(_("channelmixerrgb works only on RGB input"));
    return err;
  }

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const cl_mem input_matrix_cl = dt_opencl_copy_host_to_device_constant
    (devid, 12 * sizeof(float), (float*)work_profile->matrix_in);
  const cl_mem output_matrix_cl = dt_opencl_copy_host_to_device_constant
    (devid, 12 * sizeof(float), (float*)work_profile->matrix_out);
  const cl_mem MIX_cl = dt_opencl_copy_host_to_device_constant
    (devid, 12 * sizeof(float), d->MIX);

  if(input_matrix_cl == NULL || output_matrix_cl == NULL || MIX_cl == NULL)
    goto error;
  // select the right kernel for the current LMS space
  int kernel = gd->kernel_channelmixer_rgb_rgb;

  switch(d->adaptation)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    {
      kernel = gd->kernel_channelmixer_rgb_bradford_full;
      break;
    }
    case DT_ADAPTATION_LINEAR_BRADFORD:
    {
      kernel = gd->kernel_channelmixer_rgb_bradford_linear;
      break;
    }
    case DT_ADAPTATION_CAT16:
    {
      kernel = gd->kernel_channelmixer_rgb_cat16;
      break;
    }
    case DT_ADAPTATION_XYZ:
    {
      kernel = gd->kernel_channelmixer_rgb_xyz;
      break;
     }
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
    {
      kernel = gd->kernel_channelmixer_rgb_rgb;
      break;
    }
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
          CLARG(dev_in), CLARG(dev_out),
          CLARG(width), CLARG(height),
          CLARG(input_matrix_cl), CLARG(output_matrix_cl),
          CLARG(MIX_cl), CLARG(d->illuminant), CLARG(d->saturation),
          CLARG(d->lightness), CLARG(d->grey),
          CLARG(d->p), CLARG(d->gamut), CLARG(d->clip), CLARG(d->apply_grey),
          CLARG(d->version));

error:
  dt_opencl_release_mem_object(input_matrix_cl);
  dt_opencl_release_mem_object(output_matrix_cl);
  dt_opencl_release_mem_object(MIX_cl);
  return err;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 32; // extended.cl in programs.conf
  dt_iop_channelmixer_rgb_global_data_t *gd = malloc(sizeof(dt_iop_channelmixer_rgb_global_data_t));

  self->data = gd;
  gd->kernel_channelmixer_rgb_cat16 =
    dt_opencl_create_kernel(program, "channelmixerrgb_CAT16");
  gd->kernel_channelmixer_rgb_bradford_full =
    dt_opencl_create_kernel(program, "channelmixerrgb_bradford_full");
  gd->kernel_channelmixer_rgb_bradford_linear =
    dt_opencl_create_kernel(program, "channelmixerrgb_bradford_linear");
  gd->kernel_channelmixer_rgb_xyz =
    dt_opencl_create_kernel(program, "channelmixerrgb_XYZ");
  gd->kernel_channelmixer_rgb_rgb =
    dt_opencl_create_kernel(program, "channelmixerrgb_RGB");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  const dt_iop_channelmixer_rgb_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_channelmixer_rgb_cat16);
  dt_opencl_free_kernel(gd->kernel_channelmixer_rgb_bradford_full);
  dt_opencl_free_kernel(gd->kernel_channelmixer_rgb_bradford_linear);
  dt_opencl_free_kernel(gd->kernel_channelmixer_rgb_xyz);
  dt_opencl_free_kernel(gd->kernel_channelmixer_rgb_rgb);
  free(self->data);
  self->data = NULL;
}
#endif


#ifdef AI_ACTIVATED
#endif


void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)p1;
  dt_iop_channelmixer_rbg_data_t *d = piece->data;

  d->version = p->version;

  float norm_R = 1.0f;
  if(p->normalize_R)
    norm_R = p->red[0] + p->red[1] + p->red[2];

  float norm_G = 1.0f;
  if(p->normalize_G)
    norm_G = p->green[0] + p->green[1] + p->green[2];

  float norm_B = 1.0f;
  if(p->normalize_B)
    norm_B = p->blue[0] + p->blue[1] + p->blue[2];

  float norm_sat = 0.0f;
  if(p->normalize_sat)
    norm_sat = (p->saturation[0] + p->saturation[1] + p->saturation[2]) / 3.f;

  float norm_light = 0.0f;
  if(p->normalize_light)
    norm_light = (p->lightness[0] + p->lightness[1] + p->lightness[2]) / 3.f;

  float norm_grey = p->grey[0] + p->grey[1] + p->grey[2];
  d->apply_grey = (p->grey[0] != 0.f) || (p->grey[1] != 0.f) || (p->grey[2] != 0.f);
  if(!p->normalize_grey || norm_grey == 0.f)
    norm_grey = 1.f;

  for(int i = 0; i < 3; i++)
  {
    d->MIX[0][i] = p->red[i] / norm_R;
    d->MIX[1][i] = p->green[i] / norm_G;
    d->MIX[2][i] = p->blue[i] / norm_B;
    d->saturation[i] = -p->saturation[i] + norm_sat;
    d->lightness[i] = p->lightness[i] - norm_light;
    // = NaN if(norm_grey == 0.f) but we don't care since (d->apply_grey == FALSE)
    d->grey[i] = p->grey[i] / norm_grey;
  }

  if(p->version == CHANNELMIXERRGB_V_1)
  {
    // for the v1 saturation algo, the effect of R and B coeffs is reversed
    d->saturation[0] = -p->saturation[2] + norm_sat;
    d->saturation[2] = -p->saturation[0] + norm_sat;
  }

  // just in case compiler feels clever and uses SSE 4×1 dot product
  d->saturation[CHANNEL_SIZE - 1] = 0.0f;
  d->lightness[CHANNEL_SIZE - 1] = 0.0f;
  d->grey[CHANNEL_SIZE - 1] = 0.0f;

  d->adaptation = p->adaptation;
  d->clip = p->clip;
  d->gamut = (p->gamut == 0.f) ? p->gamut : 1.f / p->gamut;

  // find x y coordinates of illuminant for CIE 1931 2° observer
  float x = p->x;
  float y = p->y;
  dt_aligned_pixel_t custom_wb;
  _get_white_balance_coeff(self, custom_wb);
  illuminant_to_xy(p->illuminant, &(self->dev->image_storage),
                   custom_wb, &x, &y, p->temperature, p->illum_fluo, p->illum_led);

  // if illuminant is set as camera, x and y are set on-the-fly at
  // commit time, so we need to set adaptation too
  if(p->illuminant == DT_ILLUMINANT_CAMERA)
    _check_if_close_to_daylight(x, y, NULL, NULL, &(d->adaptation));

  d->illuminant_type = p->illuminant;

  // Convert illuminant from xyY to XYZ
  dt_aligned_pixel_t XYZ;
  illuminant_xy_to_XYZ(x, y, XYZ);

  // Convert illuminant from XYZ to Bradford modified LMS
  convert_any_XYZ_to_LMS(XYZ, d->illuminant, d->adaptation);
  d->illuminant[3] = 0.f;

  const gboolean preview = piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW;
  const gboolean run_profile = preview && g && g->run_profile;
  const gboolean run_validation = preview && g && g->run_validation;

  const char *ill_desc = dt_introspection_get_enum_name(get_f("illuminant"), d->illuminant_type);
  dt_print(DT_DEBUG_PARAMS,
    "[commit color calibration]%s%s  temp=%i  xy=%.4f %.4f - XYZ=%.4f %.4f %.4f - LMS=%.4f %.4f %.4f  %s",
     run_profile ? " [profile]" : "",
     run_validation ? " [validation]" : "",
     (int)p->temperature, x, y, XYZ[0], XYZ[1], XYZ[2],
     d->illuminant[0], d->illuminant[1], d->illuminant[2],
     ill_desc ? ill_desc : "DT_ILLUMINANT_UNDEFINED");

  // blue compensation for Bradford transform = (test illuminant blue
  // / reference illuminant blue)^0.0834 reference illuminant is
  // hard-set D50 for darktable's pipeline test illuminant is user
  // params
  d->p = powf(0.818155f / d->illuminant[2], 0.0834f);

  // Disable OpenCL path if we are in any kind of diagnose mode (only CPU has this)

  // if this module has some mask applied we assume it's safe so give no warning
  const dt_develop_blend_params_t *b = (const dt_develop_blend_params_t *)piece->blendop_data;
  const dt_develop_mask_mode_t mask_mode = b ? b->mask_mode : DEVELOP_MASK_DISABLED;
  const gboolean is_blending = (mask_mode & DEVELOP_MASK_ENABLED) && (mask_mode >= DEVELOP_MASK_MASK);
  if(g) g->is_blending = is_blending;
}


/**
 * DOCUMENTATION
 *
 * The illuminant is stored in params as a set of x and y coordinates,
 * describing its chrominance in xyY color space.  xyY is a normalized
 * XYZ space, derivated from the retina cone sensors. By definition,
 * for an illuminant, Y = 1, so we only really care about (x, y).
 *
 * Using (x, y) is a robust and interoperable way to describe an
 * illuminant, since it is all the actual pixel code needs to perform
 * the chromatic adaptation. This (x, y) can be computed in many
 * different ways or taken from databases, and possibly from other
 * software, so storing only the result let us room to improve the
 * computation in the future, without losing compatibility with older
 * versions.
 *
 * However, it's not a great GUI since x and y are not perceptually
 * scaled. So the `g->illum_x` and `g->illum_y` actually display
 * respectively hue and chroma, in LCh color space, which is designed
 * for illuminants and preceptually spaced. This gives UI controls
 * which effect feels more even to the user.
 *
 * But that makes things a bit tricky, API-wise, since a set of (x, y)
 * depends on a set of (hue, chroma), so they always need to be
 * handled together, but also because the back-and-forth computations
 * Lch <-> xyY need to be done anytime we read or write from/to params
 * from/to GUI.
 *
 * Also, the R, G, B sliders have a background color gradient that
 * shows the actual R, G, B sensors used by the selected chromatic
 * adaptation. Each chromatic adaptation method uses a different RGB
 * space, called LMS in the literature (but it's only a
 * special-purpose RGB space for all we care here), which primaries
 * are projected to sRGB colors, to be displayed in the GUI, so users
 * may get a feeling of what colors they will get.
 **/


void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc1_align_type(dt_iop_channelmixer_rbg_data_t);
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_dev_reset_chroma(self->dev);
  dt_free_align(piece->data);
  piece->data = NULL;
}


void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  dt_iop_channelmixer_rgb_params_t *d = self->default_params;
  d->red[0] = d->green[1] = d->blue[2] = 1.0;
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_params_t *d = self->default_params;

  d->x = self->get_f("x")->Float.Default;
  d->y = self->get_f("y")->Float.Default;
  d->temperature = self->get_f("temperature")->Float.Default;
  d->illuminant = self->get_f("illuminant")->Enum.Default;
  d->adaptation = self->get_f("adaptation")->Enum.Default;

  const gboolean is_workflow_none = dt_conf_is_equal("plugins/darkroom/workflow", "none");
  const gboolean is_modern = dt_is_scene_referred() || is_workflow_none;

  // note that if there is already an instance of this module with an
  // adaptation set we default to RGB (none) in this instance.
  // try to register the CAT here
  _declare_cat_on_pipe(self, is_modern);
  const dt_image_t *img = &self->dev->image_storage;

  // check if we could register
  const gboolean CAT_already_applied =
    (self->dev->chroma.adaptation != NULL)       // CAT exists
    && (self->dev->chroma.adaptation != self); // and it is not us

  self->default_enabled = FALSE;

  if(CAT_already_applied || dt_image_is_monochrome(img))
  {
    // simple channel mixer
    d->illuminant = DT_ILLUMINANT_PIPE;
    d->adaptation = DT_ADAPTATION_RGB;
  }
  else
  {
    d->adaptation = DT_ADAPTATION_CAT16;

    dt_aligned_pixel_t custom_wb;
    if(!_get_white_balance_coeff(self, custom_wb))
    {
      if(find_temperature_from_raw_coeffs(img, custom_wb, &(d->x), &(d->y)))
        d->illuminant = DT_ILLUMINANT_CAMERA;
      _check_if_close_to_daylight(d->x, d->y,
                                  &(d->temperature), &(d->illuminant), &(d->adaptation));
    }
  }

}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
