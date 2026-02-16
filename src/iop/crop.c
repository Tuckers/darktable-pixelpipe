/*
    This file is part of darktable,
    Copyright (C) 2021-2025 darktable developers.

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

#include "common/debug.h"
#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(3, dt_iop_crop_params_t)

#define MIN_CROP_SIZE 0.01f /* minimum crop width/height as fraction of image size */

/** flip guides H/V */
typedef enum dt_iop_crop_flip_t
{
  FLAG_FLIP_HORIZONTAL = 1 << 0,
  FLAG_FLIP_VERTICAL = 1 << 1
} dt_iop_crop_flip_t;

typedef struct dt_iop_crop_aspect_t
{
  char *name;
  int d, n;
} dt_iop_crop_aspect_t;

typedef struct dt_iop_crop_params_t
{
  float cx;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "left"
  float cy;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "top"
  float cw;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "right"
  float ch;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "bottom"
  int ratio_n; // $DEFAULT: -1
  int ratio_d; // $DEFAULT: -1
} dt_iop_crop_params_t;

typedef enum _grab_region_t
{
  GRAB_CENTER = 0,                                            // 0
  GRAB_LEFT = 1 << 0,                                         // 1
  GRAB_TOP = 1 << 1,                                          // 2
  GRAB_RIGHT = 1 << 2,                                        // 4
  GRAB_BOTTOM = 1 << 3,                                       // 8
  GRAB_TOP_LEFT = GRAB_TOP | GRAB_LEFT,                       // 3
  GRAB_TOP_RIGHT = GRAB_TOP | GRAB_RIGHT,                     // 6
  GRAB_BOTTOM_RIGHT = GRAB_BOTTOM | GRAB_RIGHT,               // 12
  GRAB_BOTTOM_LEFT = GRAB_BOTTOM | GRAB_LEFT,                 // 9
  GRAB_HORIZONTAL = GRAB_LEFT | GRAB_RIGHT,                   // 5
  GRAB_VERTICAL = GRAB_TOP | GRAB_BOTTOM,                     // 10
  GRAB_ALL = GRAB_LEFT | GRAB_TOP | GRAB_RIGHT | GRAB_BOTTOM, // 15
  GRAB_NONE = 1 << 4                                          // 16
} _grab_region_t;


typedef struct dt_iop_crop_data_t
{
  float aspect;         // forced aspect ratio
  float cx, cy, cw, ch; // crop window
  int ratio_n;
  int ratio_d;
} dt_iop_crop_data_t;

const char *name()
{
  return _("crop");
}

const char *aliases()
{
  return _("reframe|distortion");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("change the framing"),
                                _("corrective or creative"),
                                _("linear, RGB, scene-referred"),
                                _("geometric, RGB"),
                                _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI
    | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_ALLOW_FAST_PIPE
    | IOP_FLAGS_GUIDES_SPECIAL_DRAW | IOP_FLAGS_GUIDES_WIDGET | IOP_FLAGS_CROP_EXPOSER;
}

int operation_tags()
{
  return IOP_TAG_DISTORT | IOP_TAG_CROPPING;
}

int operation_tags_filter()
{
  // switch off watermark, it gets confused.
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
  typedef struct dt_iop_crop_params_v1_t
  {
    float cx;
    float cy;
    float cw;
    float ch;
    int ratio_n;
    int ratio_d;
  } dt_iop_crop_params_v1_t;

  typedef struct dt_iop_crop_params_v2_t
  {
    float cx;
    float cy;
    float cw;
    float ch;
    int ratio_n;
    int ratio_d;
    gboolean aligned;
  } dt_iop_crop_params_v2_t;

  typedef struct dt_iop_crop_params_v3_t
  {
    float cx;
    float cy;
    float cw;
    float ch;
    int ratio_n;
    int ratio_d;
  } dt_iop_crop_params_v3_t;

  if(old_version == 1)
  {
    const dt_iop_crop_params_v1_t *o = (dt_iop_crop_params_v1_t *)old_params;
    dt_iop_crop_params_v2_t *n = malloc(sizeof(dt_iop_crop_params_v2_t));
    memcpy(n, o, sizeof(dt_iop_crop_params_v1_t));
    n->aligned = FALSE;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_crop_params_v2_t);
    *new_version = 2;
    return 0;
  }
  else if(old_version == 2)
  {
    // recover from wrong params, see #19919
    const dt_iop_crop_params_v2_t *o = (dt_iop_crop_params_v2_t *)old_params;
    dt_iop_crop_params_t *n = malloc(sizeof(dt_iop_crop_params_v3_t));
    memcpy(n, o, sizeof(dt_iop_crop_params_v3_t));

    // Let's check for bad square crops because of bad edits with original image ratio
    if(self && self->dev && abs(n->ratio_d) == 1 && n->ratio_n == 0)
    {
      const float pwd = MAX(1.0f, self->dev->image_storage.p_width);
      const float pht = MAX(1.0f, self->dev->image_storage.p_height);
      const gboolean safe = pwd > 4.0f && pht > 4.0f;
      const float ratio = safe ? pwd / pht : 1.0f;

      const float landscape = (self->dev->image_storage.orientation & ORIENTATION_SWAP_XY) == 0;
      const float wd = landscape ? pwd : pht;
      const float ht = landscape ? pht : pwd;

      const float px = n->cx * wd;
      const float py = n->cy * ht;
      const float dx = (n->cw - n->cx) * wd;
      const float dy = (n->ch - n->cy) * ht;
      float new_dx = dx;
      float new_dy = dy;

      const gboolean correct = feqf(ratio, dx / dy, 0.01f) || feqf(ratio, dy / dx, 0.01f);
      const gboolean quadratic = feqf(dx, dy, 1.0f);
      const gboolean flipped = n->ratio_d < 0;
      if(!correct && safe)
      {
        if(landscape)
        {
          if(flipped)
          {
            new_dx = dy / ratio;
            n->cw = (new_dx + px) / wd;
          }
          else
          {
            new_dy = dx / ratio;
            n->ch = (new_dy + py) / ht;
          }
        }
        else
        { // portrait
          if(flipped)
          {
            new_dx = dy * ratio;
            n->cw = (new_dx +px) / wd;
          }
          else
          {
            new_dx = dy / ratio;
            n->cw = (new_dx + px) / wd;
          }
        }
        dt_print(DT_DEBUG_ALWAYS,
                 "WARNING: BAD CROP in [crop legacacy_params 2->3] ID=%d %s%s %s%s topleft=%d/%d %dx%d --> %dx%d (ratio=%.3f image %dx%d)",
                 self->dev->image_storage.id,
                 quadratic ? "quadratic " : "",
                 landscape ? "landscape" : "portrait",
                 flipped ? "flipped" : "unflipped",
                 o->aligned ? " aligned-mode" : "",
                 (int)px, (int)py, (int)dx, (int)dy, (int)new_dx, (int)new_dy,
                 ratio, (int)wd, (int)ht);
      }
      else
        dt_print(DT_DEBUG_PARAMS,
                 "[crop legacacy_params 2->3] 'original image' ratio was ok");
    }
    else
      dt_print(DT_DEBUG_PARAMS,
               "[crop legacy_params 2->3] unchanged ratio_d=%d ratio_n=%d",
               n->ratio_d, n->ratio_n);

    *new_params = n;
    *new_params_size = sizeof(dt_iop_crop_params_t);
    *new_version = 3;
    return 0;
  }

  return 1;
}

static gboolean _reduce_aligners(int *ialign_w, int *ialign_h)
{
  int align_w = MAX(1, abs(*ialign_w));
  int align_h = MAX(1, abs(*ialign_h));
  for(int i = 7; i > 1; i--)
  {
    while(align_w % i == 0 && align_h % i == 0)
    {
      align_w /= i;
      align_h /= i;
    }
  }
  *ialign_w = align_w;
  *ialign_h = align_h;
  return align_w <= 16
      && align_h <= 16
      && (align_w > 1 || align_h > 1);
}

// #undef PHI
// #undef INVPHI

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
