/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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
#include <stdlib.h>
#include <string.h>

#include "common/math.h"
#include "common/opencl.h"
#include "common/tea.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "iop/iop_api.h"
#include <inttypes.h>

DT_MODULE_INTROSPECTION(4, dt_iop_vignette_params_t)

typedef enum dt_iop_dither_t
{
  DITHER_OFF = 0,  // $DESCRIPTION: "off"
  DITHER_8BIT = 1, // $DESCRIPTION: "8-bit output"
  DITHER_16BIT = 2 // $DESCRIPTION: "16-bit output"
} dt_iop_dither_t;

typedef struct dt_iop_dvector_2d_t
{
  double x;
  double y;
} dt_iop_dvector_2d_t;

typedef struct dt_iop_fvector_2d_t
{
  float x; // $MIN: -1.0 $MAX: 1.0 $DESCRIPTION: "horizontal center"
  float y; // $MIN: -1.0 $MAX: 1.0 $DESCRIPTION: "vertical center"
} dt_iop_vector_2d_t;

typedef struct dt_iop_vignette_params_t
{
  float scale;               // $MIN: 0.0 $MAX: 200.0 $DEFAULT: 80.0 $DESCRIPTION: "fall-off start" Inner radius, percent of largest image dimension
  float falloff_scale;       // $MIN: 0.0 $MAX: 200.0 $DEFAULT: 50.0 $DESCRIPTION: "fall-off radius" 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: -0.5 -1 - 1 Strength of brightness reduction
  float saturation;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: -0.5 -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;        // $DEFAULT: FALSE $DESCRIPTION: "automatic ratio"
  float whratio;             // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "width/height ratio" 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;               // $MIN: 0.0 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "shape"
  dt_iop_dither_t dithering; // $DEFAULT: DITHER_OFF if and how to perform dithering
  gboolean unbound;          // $DEFAULT: TRUE whether the values should be clipped
} dt_iop_vignette_params_t;


typedef struct dt_iop_vignette_data_t
{
  float scale;
  float falloff_scale;
  float brightness;
  float saturation;
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;
  float whratio;
  float shape;
  int dithering;
  gboolean unbound;
} dt_iop_vignette_data_t;

typedef struct dt_iop_vignette_global_data_t
{
  int kernel_vignette;
} dt_iop_vignette_global_data_t;


const char *name()
{
  return _("vignetting");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("simulate a lens fall-off close to edges"),
                                      _("creative"),
                                      _("non-linear, RGB, display-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING
         | IOP_FLAGS_TILING_FULL_ROI;
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

int operation_tags()
{
  return IOP_TAG_DECORATION;
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
  typedef struct dt_iop_vignette_params_v4_t
  {
    float scale;
    float falloff_scale;
    float brightness;
    float saturation;
    dt_iop_vector_2d_t center;
    gboolean autoratio;
    float whratio;
    float shape;
    dt_iop_dither_t dithering;
    gboolean unbound;
  } dt_iop_vignette_params_v4_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_vignette_params_v1_t
    {
      double scale;
      double falloff_scale;

      double strength;
      double uniformity;
      double bsratio;
      gboolean invert_falloff;
      gboolean invert_saturation;
      dt_iop_dvector_2d_t center;
    } dt_iop_vignette_params_v1_t;

    const dt_iop_vignette_params_v1_t *old = old_params;
    dt_iop_vignette_params_v4_t *new = malloc(sizeof(dt_iop_vignette_params_v4_t));
    new->scale = old->scale;
    new->falloff_scale = old->falloff_scale;
    new->brightness = -(1.0 - MAX(old->bsratio, 0.0)) * old->strength / 100.0;
    new->saturation = -(1.0 + MIN(old->bsratio, 0.0)) * old->strength / 100.0;
    if(old->invert_saturation) new->saturation *= -2.0; // Double effect for increasing saturation
    if(old->invert_falloff) new->brightness = -new->brightness;
    new->center.x = old->center.x;
    new->center.y = old->center.y;
    new->autoratio = TRUE;
    new->whratio = 1.0;
    new->shape = 1.0;
    new->dithering = DITHER_OFF;
    new->unbound = FALSE;

    *new_params = new;
    *new_params_size = sizeof(dt_iop_vignette_params_v4_t);
    *new_version = 4;
    return 0;
  }
  if(old_version == 2)
  {
    typedef struct dt_iop_vignette_params_v2_t
    {
      float scale;
      float falloff_scale;
      float brightness;
      float saturation;
      dt_iop_vector_2d_t center;
      gboolean autoratio;
      float whratio;
    float shape;
    } dt_iop_vignette_params_v2_t;

    const dt_iop_vignette_params_v2_t *old = old_params;
    dt_iop_vignette_params_v4_t *new = malloc(sizeof(dt_iop_vignette_params_v4_t));
    new->scale = old->scale;
    new->falloff_scale = old->falloff_scale;
    new->brightness = old->brightness;
    new->saturation = old->saturation;
    new->center.x = old->center.x;
    new->center.y = old->center.y;
    new->autoratio = old->autoratio;
    new->whratio = old->whratio;
    new->shape = old->shape;
    new->dithering = DITHER_OFF;
    new->unbound = FALSE;

    *new_params = new;
    *new_params_size = sizeof(dt_iop_vignette_params_v4_t);
    *new_version = 4;
    return 0;
  }
  if(old_version == 3)
  {
    typedef struct dt_iop_vignette_params_v3_t
    {
      float scale;
      float falloff_scale;
      float brightness;
      float saturation;
      dt_iop_vector_2d_t center;
      gboolean autoratio;
      float whratio;
      float shape;
      int dithering;
    } dt_iop_vignette_params_v3_t;

    const dt_iop_vignette_params_v3_t *old = old_params;
    dt_iop_vignette_params_v4_t *new = malloc(sizeof(dt_iop_vignette_params_v4_t));
    new->scale = old->scale;
    new->falloff_scale = old->falloff_scale;
    new->brightness = old->brightness;
    new->saturation = old->saturation;
    new->center.x = old->center.x;
    new->center.y = old->center.y;
    new->autoratio = old->autoratio;
    new->whratio = old->whratio;
    new->shape = old->shape;
    new->dithering = old->dithering;
    new->unbound = FALSE;

    *new_params = new;
    *new_params_size = sizeof(dt_iop_vignette_params_v4_t);
    *new_version = 4;
    return 0;
  }

  return 1;
}


// FIXME: For portrait images the overlay is a bit off. The
// coordinates in mouse_moved seem to be ok though.

// FIXME: Pumping of the opposite direction when changing
// width/height. See two FIXMEs further down.

int button_pressed(dt_iop_module_t *self,
                   const float x,
                   const float y,
                   const double pressure,
                   const int which,
                   const int type,
                   const uint32_t state,
                   const float zoom_scale)
{
  if(which == GDK_BUTTON_PRIMARY) return 1;
  return 0;
}

int button_released(dt_iop_module_t *self,
                    const float x,
                    const float y,
                    const int which,
                    const uint32_t state,
                    const float zoom_scale)
{
  if(which == GDK_BUTTON_PRIMARY) return 1;
  return 0;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/,
                                        self, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return;

  const dt_iop_vignette_data_t *data = piece->data;
  const dt_iop_roi_t *buf_in = &piece->buf_in;
  const gboolean unbound = data->unbound;

  /* Center coordinates of buf_in, these should not consider buf_in->{x,y}! */
  const dt_iop_vector_2d_t buf_center = { buf_in->width * .5f, buf_in->height * .5f };
  /* Center coordinates of vignette center */
  const dt_iop_vector_2d_t vignette_center =
    { buf_center.x + data->center.x * buf_in->width / 2.0,
      buf_center.y + data->center.y * buf_in->height / 2.0 };
  /* Coordinates of vignette_center in terms of roi_in */
  const dt_iop_vector_2d_t roi_center
      = { vignette_center.x * roi_in->scale - roi_in->x,
          vignette_center.y * roi_in->scale - roi_in->y };
  float xscale;
  float yscale;

  /* w/h ratio follows piece dimensions */
  if(data->autoratio)
  {
    xscale = 2.0f / (buf_in->width * roi_out->scale);
    yscale = 2.0f / (buf_in->height * roi_out->scale);
  }
  else /* specified w/h ratio, scale proportional to longest side */
  {
    const float basis = 2.0f / (MAX(buf_in->height, buf_in->width) * roi_out->scale);
    // w/h ratio from 0-1 use as-is
    if(data->whratio <= 1.0f)
    {
      yscale = basis;
      xscale = yscale / data->whratio;
    }
    // w/h ratio from 1-2 interpret as 1-inf
    // that is, the h/w ratio + 1
    else
    {
      xscale = basis;
      yscale = xscale / (2.0f - data->whratio);
    }
  }
  const float dscale = data->scale / 100.0f;
  // A minimum falloff is used, based on the image size, to smooth out aliasing artifacts
  const float min_falloff = 100.0 / MIN(buf_in->width, buf_in->height);
  const float fscale = MAX(data->falloff_scale, min_falloff) / 100.0f;
  const float shape = MAX(data->shape, 0.001f);
  const float exp1 = 2.0f / shape;
  const float exp2 = shape / 2.0f;
  // Pre-scale the center offset
  const dt_iop_vector_2d_t roi_center_scaled = { roi_center.x * xscale,
                                                 roi_center.y * yscale };

  float dither = 0.0f;

  switch(data->dithering)
  {
    case DITHER_8BIT:
      dither = 1.0f / 256;
      break;
    case DITHER_16BIT:
      dither = 1.0f / 65536;
      break;
    case DITHER_OFF:
    default:
      dither = 0.0f;
  }

  unsigned int *const tea_states = alloc_tea_states(dt_get_num_threads());
  const float brightness = data->brightness;
  const float saturation = data->saturation;

  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    const size_t k = (size_t)4 * roi_out->width * j;
    const float *in = (const float *)ivoid + k;
    float *out = (float *)ovoid + k;
    unsigned int *tea_state = get_tea_state(tea_states,dt_get_thread_num());
    tea_state[0] = j * roi_out->height;
    for(int i = 0; i < roi_out->width; i++)
    {
      // current pixel coord translated to local coord
      const dt_iop_vector_2d_t pv
          = { fabsf(i * xscale - roi_center_scaled.x), fabsf(j * yscale - roi_center_scaled.y) };

      // Calculate the pixel weight in vignette. Length from center to pv:
      const float cplen = powf(powf(pv.x, exp1) + powf(pv.y, exp1), exp2);
      float weight = 0.f;
      float dith = 0.0f;

      // pixel is outside the inner vignette circle, lets calculate weight of vignette
      if(cplen >= dscale)
      {
        weight = ((cplen - dscale) / fscale);
        if(weight >= 1.0f)
          weight = 1.0f;
        else if(weight <= 0.0f)
          weight = 0.0f;
        else if(dither != 0.0f)
        {
          // only bother computing the random number if dithering is enabled
          weight = 0.5f - cosf((float)M_PI * weight) / 2.0f;
          encrypt_tea(tea_state);
          dith = dither * tpdf(tea_state[0]);
        }
      }

      // Let's apply weighted effect on brightness and desaturation
      dt_aligned_pixel_t col;
      copy_pixel(col, in + 4*i);
      if(weight > 0.0f)
      {
        // Then apply falloff vignette
        if(brightness < 0.0f)
        {
          const float falloff = (1.0f + (weight * brightness));
          for_each_channel(c)
            col[c] = col[c] * falloff + dith;
        }
        else
        {
          const float falloff = (weight * brightness);
          for_each_channel(c)
            col[c] = col[c] + falloff + dith;
        }
        for_each_channel(c)
          col[c] = unbound ? col[c] : CLIP(col[c]);

        // apply saturation
        const float mv = (col[0] + col[1] + col[2]) / 3.0f;
        const float wss = weight * saturation;
        for_each_channel(c)
        {
          col[c] = col[c] - ((mv - col[c]) * wss);
          col[c] = unbound ? col[c] : CLIP(col[c]);
        }
      }
      copy_pixel_nontemporal(out + 4*i, col) ;
    }
  }

  free_tea_states(tea_states);
}


#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_vignette_data_t *data = piece->data;
  dt_iop_vignette_global_data_t *gd = self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;

  const dt_iop_roi_t *buf_in = &piece->buf_in;

  /* Center coordinates of buf_in, these should not consider buf_in->{x,y}! */
  const dt_iop_vector_2d_t buf_center = { buf_in->width * .5f, buf_in->height * .5f };
  /* Center coordinates of vignette center */
  const dt_iop_vector_2d_t vignette_center =
    { buf_center.x + data->center.x * buf_in->width / 2.0,
      buf_center.y + data->center.y * buf_in->height / 2.0 };
  /* Coordinates of vignette_center in terms of roi_in */
  const dt_iop_vector_2d_t roi_center =
    { vignette_center.x * roi_in->scale - roi_in->x,
      vignette_center.y * roi_in->scale - roi_in->y };
  float xscale;
  float yscale;

  /* w/h ratio follows piece dimensions */
  if(data->autoratio)
  {
    xscale = 2.0 / (buf_in->width * roi_out->scale);
    yscale = 2.0 / (buf_in->height * roi_out->scale);
  }
  else /* specified w/h ratio, scale proportional to longest side */
  {
    const float basis = 2.0 / (MAX(buf_in->height, buf_in->width) * roi_out->scale);
    // w/h ratio from 0-1 use as-is
    if(data->whratio <= 1.0)
    {
      yscale = basis;
      xscale = yscale / data->whratio;
    }
    // w/h ratio from 1-2 interpret as 1-inf
    // that is, the h/w ratio + 1
    else
    {
      xscale = basis;
      yscale = xscale / (2.0 - data->whratio);
    }
  }
  const float dscale = data->scale / 100.0;
  // A minimum falloff is used, based on the image size, to smooth out aliasing artifacts
  const float min_falloff = 100.0 / MIN(buf_in->width, buf_in->height);
  const float fscale = MAX(data->falloff_scale, min_falloff) / 100.0;
  const float shape = MAX(data->shape, 0.001);
  const float exp1 = 2.0 / shape;
  const float exp2 = shape / 2.0;
  // Pre-scale the center offset
  const dt_iop_vector_2d_t roi_center_scaled = { roi_center.x * xscale,
                                                 roi_center.y * yscale };

  float dither = 0.0f;

  switch(data->dithering)
  {
    case DITHER_8BIT:
      dither = 1.0f / 256;
      break;
    case DITHER_16BIT:
      dither = 1.0f / 65536;
      break;
    case DITHER_OFF:
    default:
      dither = 0.0f;
  }

  float scale[2] = { xscale, yscale };
  float roi_center_scaled_f[2] = { roi_center_scaled.x, roi_center_scaled.y };
  float expt[2] = { exp1, exp2 };
  const float brightness = data->brightness;
  const float saturation = data->saturation;
  const int unbound = data->unbound;

  return dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_vignette, width, height,
                                          CLARG(dev_in), CLARG(dev_out),
                                          CLARG(width), CLARG(height),
                                          CLARG(scale),
                                          CLARG(roi_center_scaled_f),
                                          CLARG(expt),
                                          CLARG(dscale), CLARG(fscale),
                                          CLARG(brightness),
                                          CLARG(saturation),
                                          CLARG(dither),
    CLARG(unbound));
}
#endif


void init_global(dt_iop_module_so_t *self)
{
  const int program = 8; // extended.cl from programs.conf
  dt_iop_vignette_global_data_t *gd = malloc(sizeof(dt_iop_vignette_global_data_t));
  self->data = gd;
  gd->kernel_vignette = dt_opencl_create_kernel(program, "vignette");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_vignette_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_vignette);
  free(self->data);
  self->data = NULL;
}


void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)p1;
  dt_iop_vignette_data_t *d = piece->data;
  d->scale = p->scale;
  d->falloff_scale = p->falloff_scale;
  d->brightness = p->brightness;
  d->saturation = p->saturation;
  d->center = p->center;
  d->autoratio = p->autoratio;
  d->whratio = p->whratio;
  d->shape = p->shape;
  d->dithering = p->dithering;
  d->unbound = p->unbound;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_database_start_transaction(darktable.db);
  dt_iop_vignette_params_t p;
  p.scale = 40.0f;
  p.falloff_scale = 100.0f;
  p.brightness = -1.0f;
  p.saturation = 0.5f;
  p.center.x = 0.0f;
  p.center.y = 0.0f;
  p.autoratio = FALSE;
  p.whratio = 1.0f;
  p.shape = 1.0f;
  p.dithering = 0;
  p.unbound = TRUE;
  dt_gui_presets_add_generic(_("lomo"), self->op,
                             self->version(), &p, sizeof(p), TRUE,
                             DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_database_release_transaction(darktable.db);
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_vignette_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


GSList *mouse_actions(dt_iop_module_t *self)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_format
    (lm, DT_MOUSE_ACTION_LEFT_DRAG, 0,
     _("[%s on node] change vignette/feather size"), self->name());
  lm = dt_mouse_action_create_format
    (lm, DT_MOUSE_ACTION_LEFT_DRAG, GDK_CONTROL_MASK,
     _("[%s on node] change vignette/feather size keeping ratio"), self->name());
  lm = dt_mouse_action_create_format
    (lm, DT_MOUSE_ACTION_LEFT_DRAG, GDK_CONTROL_MASK,
     _("[%s on center] move vignette"), self->name());
  return lm;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
