/*
    This file is part of darktable,
    Copyright (C) 2009-2025 darktable developers.

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
#include <lcms2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/opencl.h"
#include "common/wb_presets.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"

// for Kelvin temperature and bogus WB
#include "common/colorspaces.h"
#include "external/cie_colorimetric_tables.c"

DT_MODULE_INTROSPECTION(4, dt_iop_temperature_params_t)

#define INITIALBLACKBODYTEMPERATURE 4000

#define DT_IOP_LOWEST_TEMPERATURE 1901
#define DT_IOP_HIGHEST_TEMPERATURE 25000

#define DT_IOP_LOWEST_TINT 0.135
#define DT_IOP_HIGHEST_TINT 2.326

#define DT_IOP_NUM_OF_STD_TEMP_PRESETS 5

// If you reorder presets combo, change this consts
#define DT_IOP_TEMP_UNKNOWN -1
#define DT_IOP_TEMP_AS_SHOT 0
#define DT_IOP_TEMP_SPOT 1
#define DT_IOP_TEMP_USER 2
#define DT_IOP_TEMP_D65 3
#define DT_IOP_TEMP_D65_LATE 4


typedef struct dt_iop_temperature_params_t
{
  float red;     // $MIN: 0.0 $MAX: 8.0
  float green;   // $MIN: 0.0 $MAX: 8.0
  float blue;    // $MIN: 0.0 $MAX: 8.0
  float various; // $MIN: 0.0 $MAX: 8.0
  int preset;
} dt_iop_temperature_params_t;


typedef struct dt_iop_temperature_data_t
{
  float coeffs[4];
  int preset;
} dt_iop_temperature_data_t;

typedef struct dt_iop_temperature_global_data_t
{
  int kernel_whitebalance_4f;
  int kernel_whitebalance_1f;
  int kernel_whitebalance_1f_xtrans;
} dt_iop_temperature_global_data_t;

typedef struct dt_iop_temperature_preset_data_t
{
  int no_ft_pos;
  int min_ft_pos;
  int max_ft_pos;
} dt_iop_temperature_preset_data_t;

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_temperature_params_v3_t
  {
    float red;
    float green;
    float blue;
    float various;
  } dt_iop_temperature_params_v3_t;

  typedef struct dt_iop_temperature_params_v4_t
  {
    float red;
    float green;
    float blue;
    float various;
    int preset;
  } dt_iop_temperature_params_v4_t;

  if(old_version == 2)
  {
    typedef struct dt_iop_temperature_params_v2_t
    {
      float temp_out;
      float coeffs[3];
    } dt_iop_temperature_params_v2_t;

    const dt_iop_temperature_params_v2_t *o = (dt_iop_temperature_params_v2_t *)old_params;
    dt_iop_temperature_params_v3_t *n = malloc(sizeof(dt_iop_temperature_params_v3_t));

    n->red = o->coeffs[0];
    n->green = o->coeffs[1];
    n->blue = o->coeffs[2];
    n->various = NAN;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_temperature_params_v3_t);
    *new_version = 3;
    return 0;
  }

  if(old_version == 3)
  {
    const dt_iop_temperature_params_v3_t *o = (dt_iop_temperature_params_v3_t *)old_params;
    dt_iop_temperature_params_v4_t *n = malloc(sizeof(dt_iop_temperature_params_v4_t));

    n->red = o->red;
    n->green = o->green;
    n->blue = o->blue;
    n->various = NAN;
    n->preset = DT_IOP_TEMP_UNKNOWN;
    *new_params = n;
    *new_params_size = sizeof(dt_iop_temperature_params_v4_t);
    *new_version = 4;
    return 0;
  }
  return 1;
}


static inline void _temp_array_from_params(double a[4],
                                           const dt_iop_temperature_params_t *p)
{
  float *coeffs = (float *)p;
  for_four_channels(c)
   a[c] = coeffs[c];
}

static gboolean _ignore_missing_wb(dt_image_t *img)
{
  // Ignore files that end with "-hdr.dng" since these are broken files we
  // generated without any proper WB tagged
  if(g_str_has_suffix(img->filename,"-hdr.dng"))
    return TRUE;

  // If we failed to read the image correctly, don't complain about WB
  if(img->load_status != DT_IMAGEIO_OK && img->load_status != DT_IMAGEIO_CACHE_FULL)
    return TRUE;

  static const char *const ignored_cameras[] = {
    "Canon PowerShot A610",
    "Canon PowerShot S3 IS",
    "Canon PowerShot A620",
    "Canon PowerShot A720 IS",
    "Canon PowerShot A630",
    "Canon PowerShot A640",
    "Canon PowerShot A650",
    "Canon PowerShot SX110 IS",
    "Mamiya ZD",
    "Canon EOS D2000C",
    "Kodak EOS DCS 1",
    "Kodak DCS560C",
    "Kodak DCS460D",
    "Nikon E5700",
    "Sony DSC-F828",
    "GITUP GIT2",
  };

  for(int i=0; i < sizeof(ignored_cameras)/sizeof(ignored_cameras[1]); i++)
    if(!strcmp(img->camera_makermodel, ignored_cameras[i]))
      return TRUE;

  return FALSE;
}


const char *name()
{
  return C_("modulename", "white balance");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self,
     _("scale raw RGB channels to balance white and help demosaicing"),
     _("corrective"),
     _("linear, raw, scene-referred"),
     _("linear, raw"),
     _("linear, raw, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_GRADING;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_UNSAFE_COPY;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  // This module may work in RAW or RGB (e.g. for TIFF files)
  // depending on the input The module does not change the color space
  // between the input and output, therefore implement it here
  if(piece && piece->dsc_in.cst != IOP_CS_RAW)
    return IOP_CS_RGB;
  return IOP_CS_RAW;
}

/*
 * Spectral power distribution functions
 * https://en.wikipedia.org/wiki/Spectral_power_distribution
 */
typedef double((*spd)(unsigned long int wavelength, double TempK));

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a Blackbody Radiator"
 * http://www.brucelindbloom.com/Eqn_Blackbody.html
 */
static double _spd_blackbody(unsigned long int wavelength, double TempK)
{
  // convert wavelength from nm to m
  const long double lambda = (double)wavelength * 1e-9;

/*
 * these 2 constants were computed using following Sage code:
 *
 * (from http://physics.nist.gov/cgi-bin/cuu/Value?h)
 * h = 6.62606957 * 10^-34 # Planck
 * c= 299792458 # speed of light in vacuum
 * k = 1.3806488 * 10^-23 # Boltzmann
 *
 * c_1 = 2 * pi * h * c^2
 * c_2 = h * c / k
 *
 * print 'c_1 = ', c_1, ' ~= ', RealField(128)(c_1)
 * print 'c_2 = ', c_2, ' ~= ', RealField(128)(c_2)
 */

#define c1 3.7417715246641281639549488324352159753e-16L
#define c2 0.014387769599838156481252937624049081933L

  return (double)(c1 / (powl(lambda, 5) * (expl(c2 / (lambda * TempK)) - 1.0L)));

#undef c2
#undef c1
}

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a CIE D-Illuminant"
 * http://www.brucelindbloom.com/Eqn_DIlluminant.html
 * and https://en.wikipedia.org/wiki/Standard_illuminant#Illuminant_series_D
 */
static double _spd_daylight(unsigned long int wavelength, double TempK)
{
  cmsCIExyY WhitePoint = { D65xyY.x, D65xyY.y, 1.0 };

  /*
   * Bruce Lindbloom, "TempK to xy"
   * http://www.brucelindbloom.com/Eqn_T_to_xy.html
   */
  cmsWhitePointFromTemp(&WhitePoint, TempK);

  const double M = (0.0241 + 0.2562 * WhitePoint.x - 0.7341 * WhitePoint.y),
               m1 = (-1.3515 - 1.7703 * WhitePoint.x + 5.9114 * WhitePoint.y) / M,
               m2 = (0.0300 - 31.4424 * WhitePoint.x + 30.0717 * WhitePoint.y) / M;

  const unsigned long int j
      = ((wavelength - cie_daylight_components[0].wavelength)
         / (cie_daylight_components[1].wavelength
            - cie_daylight_components[0].wavelength));

  return (cie_daylight_components[j].S[0] + m1 * cie_daylight_components[j].S[1]
          + m2 * cie_daylight_components[j].S[2]);
}

/*
 * Bruce Lindbloom, "Computing XYZ From Spectral Data (Emissive Case)"
 * http://www.brucelindbloom.com/Eqn_Spect_to_XYZ.html
 */
static cmsCIEXYZ _spectrum_to_XYZ(double TempK, spd I)
{
  cmsCIEXYZ Source = {.X = 0.0, .Y = 0.0, .Z = 0.0 };

  /*
   * Color matching functions
   * https://en.wikipedia.org/wiki/CIE_1931_color_space#Color_matching_functions
   */
  for(size_t i = 0; i < cie_1931_std_colorimetric_observer_count; i++)
  {
    const unsigned long int lambda =
      cie_1931_std_colorimetric_observer[0].wavelength
      + (cie_1931_std_colorimetric_observer[1].wavelength
         - cie_1931_std_colorimetric_observer[0].wavelength) * i;

    const double P = I(lambda, TempK);
    Source.X += P * cie_1931_std_colorimetric_observer[i].xyz.X;
    Source.Y += P * cie_1931_std_colorimetric_observer[i].xyz.Y;
    Source.Z += P * cie_1931_std_colorimetric_observer[i].xyz.Z;
  }

  // normalize so that each component is in [0.0, 1.0] range
  const double _max = fmax(fmax(Source.X, Source.Y), Source.Z);
  Source.X /= _max;
  Source.Y /= _max;
  Source.Z /= _max;

  return Source;
}

// TODO: temperature and tint cannot be disjoined! (here it assumes no tint)
static cmsCIEXYZ _temperature_to_XYZ(double TempK)
{
  if(TempK < DT_IOP_LOWEST_TEMPERATURE) TempK = DT_IOP_LOWEST_TEMPERATURE;
  if(TempK > DT_IOP_HIGHEST_TEMPERATURE) TempK = DT_IOP_HIGHEST_TEMPERATURE;

  if(TempK < INITIALBLACKBODYTEMPERATURE)
  {
    // if temperature is less than 4000K we use blackbody,
    // because there will be no Daylight reference below 4000K...
    return _spectrum_to_XYZ(TempK, _spd_blackbody);
  }
  else
  {
    return _spectrum_to_XYZ(TempK, _spd_daylight);
  }
}

static cmsCIEXYZ _temperature_tint_to_XYZ(double TempK, double tint)
{
  cmsCIEXYZ xyz = _temperature_to_XYZ(TempK);

  xyz.Y /= tint; // TODO: This is baaad!

  return xyz;
}

// binary search inversion
static void _XYZ_to_temperature(cmsCIEXYZ XYZ, float *TempK, float *tint)
{
  double maxtemp = DT_IOP_HIGHEST_TEMPERATURE, mintemp = DT_IOP_LOWEST_TEMPERATURE;
  cmsCIEXYZ _xyz;

  for(*TempK = (maxtemp + mintemp) / 2.0;
      (maxtemp - mintemp) > 1.0;
      *TempK = (maxtemp + mintemp) / 2.0)
  {
    _xyz = _temperature_to_XYZ(*TempK);
    if(_xyz.Z / _xyz.X > XYZ.Z / XYZ.X)
      maxtemp = *TempK;
    else
      mintemp = *TempK;
  }

  // TODO: Fix this to move orthogonally to planckian locus
  *tint = (_xyz.Y / _xyz.X) / (XYZ.Y / XYZ.X);


  if(*TempK < DT_IOP_LOWEST_TEMPERATURE) *TempK = DT_IOP_LOWEST_TEMPERATURE;
  if(*TempK > DT_IOP_HIGHEST_TEMPERATURE) *TempK = DT_IOP_HIGHEST_TEMPERATURE;
  if(*tint < DT_IOP_LOWEST_TINT) *tint = DT_IOP_LOWEST_TINT;
  if(*tint > DT_IOP_HIGHEST_TINT) *tint = DT_IOP_HIGHEST_TINT;
}


static cmsCIEXYZ _mul2xyz(dt_iop_module_t *self,
                         const dt_iop_temperature_params_t *p)
{

  double CAM[4];
  _temp_array_from_params(CAM, p);

  for(int k = 0; k < 4; k++)
    CAM[k] = CAM[k] > 0.0f ? 1.0 / CAM[k] : 0.0f;

  double XYZ[3];
  for(int k = 0; k < 3; k++)
  {
    XYZ[k] = 0.0;
    for(int i = 0; i < 4; i++)
    {
      XYZ[k] += g->CAM_to_XYZ[k][i] * CAM[i];
    }
  }

  return (cmsCIEXYZ){ XYZ[0], XYZ[1], XYZ[2] };
}

static void _mul2temp(dt_iop_module_t *self,
                     dt_iop_temperature_params_t *p,
                     float *TempK,
                     float *tint)
{
  _XYZ_to_temperature(_mul2xyz(self, p), TempK, tint);
}

DT_OMP_DECLARE_SIMD(aligned(inp,outp))
static inline void scaled_copy_4wide(float *const outp,
                                     const float *const inp,
                                     const float *const coeffs)
{
  // this needs to be in a separate function to make GCC8 vectorize it
  // at -O2 as well as -O3
  for_four_channels(c, aligned(inp, coeffs, outp))
    outp[c] = inp[c] * coeffs[c];
}

static inline void _publish_chroma(dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_temperature_data_t *const d = piece->data;
  dt_iop_module_t *self = piece->module;
  dt_dev_chroma_t *chr = &self->dev->chroma;

  piece->pipe->dsc.temperature.enabled = piece->enabled;
  for_four_channels(k)
  {
    piece->pipe->dsc.temperature.coeffs[k] = d->coeffs[k];
    piece->pipe->dsc.processed_maximum[k] =
      d->coeffs[k] * piece->pipe->dsc.processed_maximum[k];
    chr->wb_coeffs[k] = d->coeffs[k];
  }
  chr->late_correction = (d->preset == DT_IOP_TEMP_D65_LATE);
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const uint32_t filters = piece->pipe->dsc.filters;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  const dt_iop_temperature_data_t *const d = piece->data;

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;
  const float *const d_coeffs = d->coeffs;

  if(filters == 9u)
  { // xtrans float mosaiced
    DT_OMP_FOR()
    for(int j = 0; j < roi_out->height; j++)
    {
      const float DT_ALIGNED_PIXEL coeffs[3][4] =
      {
        { d_coeffs[FCxtrans(j, 0, roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 1, roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 2, roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 3, roi_out, xtrans)] },
        { d_coeffs[FCxtrans(j, 4, roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 5, roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 6, roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 7, roi_out, xtrans)] },
        { d_coeffs[FCxtrans(j, 8, roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 9, roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 10, roi_out, xtrans)],
          d_coeffs[FCxtrans(j, 11, roi_out, xtrans)] },
      };
      // process sensels four at a time (note that attempting to
      //ensure alignment for this main loop actually slowed things
      //down marginally)
      int i = 0;
      for(int coeff = 0; i + 4 < roi_out->width; i += 4, coeff = (coeff+1)%3)
      {
        const size_t p = (size_t)j * roi_out->width + i;
        for_four_channels(c) // in and out are NOT aligned when width is not a multiple of 4
          out[p+c] = in[p+c] * coeffs[coeff][c];
      }
      // process the leftover sensels
      for(; i < roi_out->width; i++)
      {
        const size_t p = (size_t)j * roi_out->width + i;
        out[p] = in[p] * d_coeffs[FCxtrans(j, i, roi_out, xtrans)];
      }
    }
  }
  else if(filters)
  { // bayer float mosaiced
    const int width = roi_out->width;
    DT_OMP_FOR()
    for(int j = 0; j < roi_out->height; j++)
    {
      int i = 0;

      const int alignment = 3 & (4 - ((j*width) & 3));
      const int offset_j = j + roi_out->y;

      // process the unaligned sensels at the start of the row (when
      // width is not a multiple of 4)
      for(; i < alignment; i++)
      {
        const size_t p = (size_t)j * width + i;
        out[p] = in[p] * d_coeffs[FC(offset_j, i + roi_out->x, filters)];
      }
      const dt_aligned_pixel_t coeffs =
        { d_coeffs[FC(offset_j, i + roi_out->x, filters)],
          d_coeffs[FC(offset_j, i + roi_out->x + 1,filters)],
          d_coeffs[FC(offset_j, i + roi_out->x + 2, filters)],
          d_coeffs[FC(offset_j, i + roi_out->x + 3, filters)] };

      // process sensels four at a time
      for(; i < width - 4; i += 4)
      {
        const size_t p = (size_t)j * width + i;
        scaled_copy_4wide(out + p,in + p, coeffs);
      }

      // process the leftover sensels
      for(; i < width; i++)
      {
        const size_t p = (size_t)j * width + i;
        out[p] = in[p] * d_coeffs[FC(offset_j, i + roi_out->x, filters)];
      }
    }
  }
  else
  { // non-mosaiced
    const size_t npixels = roi_out->width * (size_t)roi_out->height;

    DT_OMP_FOR()
    for(size_t k = 0; k < 4*npixels; k += 4)
    {
      for_each_channel(c,aligned(in,out))
      {
        out[k+c] = in[k+c] * d_coeffs[c];
      }
    }
  }

  _publish_chroma(piece);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_temperature_data_t *d = piece->data;
  const dt_iop_temperature_global_data_t *gd = self->global_data;
  dt_dev_pixelpipe_t *pipe = piece->pipe;

  const int devid = pipe->devid;
  const uint32_t filters = pipe->dsc.filters;
  cl_mem dev_coeffs = NULL;
  cl_mem dev_xtrans = NULL;
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  int kernel = -1;
  if(filters == 9u) kernel = gd->kernel_whitebalance_1f_xtrans;
  else if(filters)  kernel = gd->kernel_whitebalance_1f;
  else              kernel = gd->kernel_whitebalance_4f;

  if(filters == 9u)
  {
    dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(pipe->dsc.xtrans), pipe->dsc.xtrans);
    if(dev_xtrans == NULL) goto error;
  }

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 4, d->coeffs);
  if(dev_coeffs == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, roi_in->width, roi_in->height,
                                         CLARG(dev_in), CLARG(dev_out),
                                         CLARG(roi_in->width), CLARG(roi_in->height),
                                         CLARG(dev_coeffs), CLARG(filters),
                                         CLARG(roi_out->x), CLARG(roi_out->y), CLARG(dev_xtrans));
  if(err != CL_SUCCESS) goto error;

  _publish_chroma(piece);

error:
  dt_opencl_release_mem_object(dev_coeffs);
  dt_opencl_release_mem_object(dev_xtrans);
  return err;
}
#endif

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)p1;
  dt_iop_temperature_data_t *d = piece->data;
  float *tcoeffs = (float *)p;

  if(self->hide_enable_button)
    piece->enabled = FALSE;

  dt_dev_chroma_t *chr = &self->dev->chroma;

  if(self->hide_enable_button)
  {
    for_four_channels(k)
      chr->wb_coeffs[k] = 1.0f;
    return;
  }

  for_four_channels(k)
  {
    d->coeffs[k] = tcoeffs[k];
    chr->wb_coeffs[k] = piece->enabled ? d->coeffs[k] : 1.0f;
  }

  // 4Bayer images not implemented in OpenCL yet
  if(self->dev->image_storage.flags & DT_IMAGE_4BAYER)
    piece->process_cl_ready = FALSE;

  d->preset = p->preset;

  /* Make sure the chroma information stuff is valid
     If piece is disabled we always clear the trouble message and
     make sure chroma does know there is no temperature module.
  */
  chr->late_correction = p->preset == DT_IOP_TEMP_D65_LATE;
  chr->temperature = piece->enabled ? self : NULL;
  if(pipe->type & DT_DEV_PIXELPIPE_PREVIEW && !piece->enabled)
    dt_iop_set_module_trouble_message(self, NULL, NULL, NULL);
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_temperature_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


static gboolean _calculate_bogus_daylight_wb(dt_iop_module_t *self, double bwb[4])
{
  if(!dt_image_is_matrix_correction_supported(&self->dev->image_storage))
  {
    bwb[0] = 1.0;
    bwb[2] = 1.0;
    bwb[1] = 1.0;
    bwb[3] = 1.0;

    return FALSE;
  }

  double mul[4];
  if(dt_colorspaces_conversion_matrices_rgb
     (self->dev->image_storage.adobe_XYZ_to_CAM,
      NULL, NULL,
      self->dev->image_storage.d65_color_matrix, mul))
  {
    // normalize green:
    bwb[0] = mul[0] / mul[1];
    bwb[2] = mul[2] / mul[1];
    bwb[1] = 1.0;
    bwb[3] = mul[3] / mul[1];

    return FALSE;
  }

  return TRUE;
}

static void _prepare_matrices(dt_iop_module_t *self)
{

  // sRGB D65
  const double RGB_to_XYZ[3][4] = { { 0.4124564, 0.3575761, 0.1804375, 0 },
                                    { 0.2126729, 0.7151522, 0.0721750, 0 },
                                    { 0.0193339, 0.1191920, 0.9503041, 0 } };

  // sRGB D65
  const double XYZ_to_RGB[4][3] = { { 3.2404542, -1.5371385, -0.4985314 },
                                    { -0.9692660, 1.8760108, 0.0415560 },
                                    { 0.0556434, -0.2040259, 1.0572252 },
                                    { 0, 0, 0 } };

  if(!dt_image_is_raw(&self->dev->image_storage))
  {
    // let's just assume for now(TM) that if it is not raw, it is sRGB
    memcpy(g->XYZ_to_CAM, XYZ_to_RGB, sizeof(g->XYZ_to_CAM));
    memcpy(g->CAM_to_XYZ, RGB_to_XYZ, sizeof(g->CAM_to_XYZ));
    return;
  }

  if(!dt_colorspaces_conversion_matrices_xyz(self->dev->image_storage.adobe_XYZ_to_CAM,
                                              self->dev->image_storage.d65_color_matrix,
                                              g->XYZ_to_CAM, g->CAM_to_XYZ))
  {
    if(self->dev->image_storage.load_status == DT_IMAGEIO_OK)  // suppress spurious error messages
    {
      char *camera = self->dev->image_storage.camera_makermodel;
      dt_print(DT_DEBUG_ALWAYS, "[temperature] `%s' color matrix not found for image", camera);
      dt_control_log(_("`%s' color matrix not found for image"), camera);
    }
  }
}

static void _find_coeffs(dt_iop_module_t *self, double coeffs[4])
{
  const dt_image_t *img = &self->dev->image_storage;

  // the raw should provide wb coeffs:
  gboolean ok = TRUE;
  // Only check the first three values, the fourth is usually NAN for RGB
  const int num_coeffs = (img->flags & DT_IMAGE_4BAYER) ? 4 : 3;
  for(int k = 0; ok && k < num_coeffs; k++)
  {
    if(!dt_isnormal(img->wb_coeffs[k]) || img->wb_coeffs[k] == 0.0f)
      ok = FALSE;
  }
  if(ok)
  {
    for_four_channels(k)
      coeffs[k] = img->wb_coeffs[k];
    return;
  }

  double bwb[4];
  if(!_calculate_bogus_daylight_wb(self, bwb))
  {
    // found camera matrix and used it to calculate bogus daylight wb
    for_four_channels(c)
      coeffs[c] = bwb[c];
    return;
  }

  // no cam matrix??? try presets:
  for(int i = 0; i < dt_wb_presets_count(); i++)
  {
    const dt_wb_data *wbp = dt_wb_preset(i);

    if(!strcmp(wbp->make, img->camera_maker)
       && !strcmp(wbp->model, img->camera_model))
    {
      // just take the first preset we find for this camera
      for(int k = 0; k < 3; k++)
        coeffs[k] = wbp->channels[k];
      return;
    }
  }

  // did not find preset either?
  if(!_ignore_missing_wb(&(self->dev->image_storage)))
  {
    //  only display this if we have a sample, otherwise it is better to keep
    //  on screen the more important message about missing sample and the way
    //  to contribute.
    if(!img->camera_missing_sample)
      dt_control_log(_("failed to read camera white balance information from `%s'!"),
                     img->filename);
    dt_print(DT_DEBUG_ALWAYS,
             "[temperature] failed to read camera white balance information from `%s'!",
             img->filename);
  }

  // final security net: hardcoded default that fits most cams.
  coeffs[0] = 2.0;
  coeffs[1] = 1.0;
  coeffs[2] = 1.5;
  coeffs[3] = 1.0;
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_temperature_params_t *d = self->default_params;
  dt_iop_temperature_params_t *p = self->params;

  d->preset = dt_is_scene_referred() ? DT_IOP_TEMP_D65_LATE : DT_IOP_TEMP_AS_SHOT;

  float *dcoeffs = (float *)d;
  for_four_channels(k)
    dcoeffs[k] = 1.0f;

  // we might be called from presets update infrastructure => there is no image
  if(!self->dev || !dt_is_valid_imgid(self->dev->image_storage.id))
    return;

  const gboolean is_raw =
    dt_image_is_matrix_correction_supported(&self->dev->image_storage);
  const gboolean true_monochrome =
    dt_image_monochrome_flags(&self->dev->image_storage) & DT_IMAGE_MONOCHROME;

  gboolean another_cat_defined = FALSE;

  if(!dt_is_scene_referred())
  {
    another_cat_defined =
      dt_history_check_module_exists(self->dev->image_storage.id,
                                     "channelmixerrgb", TRUE);
  }

  const gboolean is_modern = dt_is_scene_referred() || another_cat_defined;

  self->default_enabled = FALSE;
  self->hide_enable_button = true_monochrome;

  // we want these data in all cases to keep them in dev->chroma
  double daylights[4] = {1.0, 1.0, 1.0, 1.0 };
  double as_shot[4] = {1.0, 1.0, 1.0, 1.0 };

  // to have at least something and definitely not crash
  _temp_array_from_params(daylights, d);

  if(!_calculate_bogus_daylight_wb(self, daylights))
  {
    // found camera matrix and used it to calculate bogus daylight wb
  }
  else
  {
    // if we didn't find anything for daylight wb, look for a wb
    // preset with appropriate name.  we're normalizing that to be D65
    for(int i = 0; i < dt_wb_presets_count(); i++)
    {
      const dt_wb_data *wbp = dt_wb_preset(i);

      if(!strcmp(wbp->make, self->dev->image_storage.camera_maker)
         && !strcmp(wbp->model, self->dev->image_storage.camera_model)
         && (!strcmp(wbp->name, "Daylight")  //??? PO
             || !strcmp(wbp->name, "DirectSunlight"))
         && wbp->tuning == 0)
      {
        for_four_channels(k)
          daylights[k] = wbp->channels[k];
        break;
      }
    }
  }

  // Store EXIF WB coeffs
  if(is_raw)
  {
    _find_coeffs(self, as_shot);
    as_shot[0] /= as_shot[1];
    as_shot[2] /= as_shot[1];
    as_shot[3] /= as_shot[1];
    as_shot[1] = 1.0;
  }

  dt_dev_chroma_t *chr = &self->dev->chroma;
  for_four_channels(k)
  {
    chr->as_shot[k] = as_shot[k];
    chr->D65coeffs[k] = daylights[k];
  }

  dt_print(DT_DEBUG_PARAMS,
    "[dt_iop_reload_defaults] scene=%s, modern=%s, CAT=%s. D65 %.3f %.3f %.3f, AS-SHOT %.3f %.3f %.3f",
    dt_is_scene_referred() ? "YES" : "NO",
    is_modern ? "YES" : "NO",
    another_cat_defined ? "YES" : "NO",
    daylights[0], daylights[1], daylights[2], as_shot[0], as_shot[1], as_shot[2]);

  d->preset = p->preset = DT_IOP_TEMP_AS_SHOT;

  // White balance module doesn't need to be enabled for true_monochrome raws (like
  // for leica monochrom cameras). prepare_matrices is a noop as well, as there
  // isn't a color matrix, so we can skip that as well.

  if(!true_monochrome)
  {
  }

  // remember daylight wb used for temperature/tint conversion,
  // assuming it corresponds to CIE daylight (D65)
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_temperature_global_data_t *gd = malloc(sizeof(dt_iop_temperature_global_data_t));
  self->data = gd;
  gd->kernel_whitebalance_4f = dt_opencl_create_kernel(program, "whitebalance_4f");
  gd->kernel_whitebalance_1f = dt_opencl_create_kernel(program, "whitebalance_1f");
  gd->kernel_whitebalance_1f_xtrans =
    dt_opencl_create_kernel(program, "whitebalance_1f_xtrans");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_temperature_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_whitebalance_4f);
  dt_opencl_free_kernel(gd->kernel_whitebalance_1f);
  dt_opencl_free_kernel(gd->kernel_whitebalance_1f_xtrans);
  free(self->data);
  self->data = NULL;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
