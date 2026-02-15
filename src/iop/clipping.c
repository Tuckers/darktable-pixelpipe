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

#include "common/debug.h"
#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(5, dt_iop_clipping_params_t)

/** flip H/V, rotate an image, then clip the buffer. */
typedef enum dt_iop_clipping_flags_t
{
  FLAG_FLIP_HORIZONTAL = 1 << 0,
  FLAG_FLIP_VERTICAL   = 1 << 1
} dt_iop_clipping_flags_t;

typedef struct dt_iop_clipping_aspect_t
{
  char *name;
  int d, n;
} dt_iop_clipping_aspect_t;

typedef struct dt_iop_clipping_params_t
{
  float angle; // $MIN: -180.0 $MAX: 180.0
  float cx;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "left"
  float cy;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "top"
  float cw;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "right"
  float ch;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "bottom"
  float k_h, k_v;
  float kxa;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2
  float kya;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2
  float kxb;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.8
  float kyb;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2
  float kxc;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.8
  float kyc;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.8
  float kxd;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2
  float kyd;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.8
  int k_type, k_sym;
  int k_apply;   // $DEFAULT: 0
  gboolean crop_auto; // $DEFAULT: TRUE $DESCRIPTION: "automatic cropping"
  int ratio_n;   // $DEFAULT: -1
  int ratio_d;   // $DEFAULT: -1
} dt_iop_clipping_params_t;

typedef enum _grab_region_t
{
  GRAB_CENTER       = 0,                                               // 0
  GRAB_LEFT         = 1 << 0,                                          // 1
  GRAB_TOP          = 1 << 1,                                          // 2
  GRAB_RIGHT        = 1 << 2,                                          // 4
  GRAB_BOTTOM       = 1 << 3,                                          // 8
  GRAB_TOP_LEFT     = GRAB_TOP | GRAB_LEFT,                            // 3
  GRAB_TOP_RIGHT    = GRAB_TOP | GRAB_RIGHT,                           // 6
  GRAB_BOTTOM_RIGHT = GRAB_BOTTOM | GRAB_RIGHT,                        // 12
  GRAB_BOTTOM_LEFT  = GRAB_BOTTOM | GRAB_LEFT,                         // 9
  GRAB_HORIZONTAL   = GRAB_LEFT | GRAB_RIGHT,                          // 5
  GRAB_VERTICAL     = GRAB_TOP | GRAB_BOTTOM,                          // 10
  GRAB_ALL          = GRAB_LEFT | GRAB_TOP | GRAB_RIGHT | GRAB_BOTTOM, // 15
  GRAB_NONE         = 1 << 4                                           // 16
} _grab_region_t;

/* calculate the aspect ratios for current image */

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_clipping_params_v5_t
  {
    float angle;
    float cx;
    float cy;
    float cw;
    float ch;
    float k_h, k_v;
    float kxa;
    float kya;
    float kxb;
    float kyb;
    float kxc;
    float kyc;
    float kxd;
    float kyd;
    int k_type, k_sym;
    int k_apply;
    gboolean crop_auto;
    int ratio_n;
    int ratio_d;
  } dt_iop_clipping_params_v5_t;

  if(old_version == 2)
  {
    union {
        float f;
        uint32_t u;
    } k;
    // old structure def
    typedef struct old_params_t
    {
      float angle, cx, cy, cw, ch, k_h, k_v;
    } old_params_t;

    const old_params_t *o = (old_params_t *)old_params;
    dt_iop_clipping_params_v5_t *n = malloc(sizeof(dt_iop_clipping_params_v5_t));

    k.f = o->k_h;
    int is_horizontal;
    if(k.u & 0x40000000u)
      is_horizontal = 1;
    else
      is_horizontal = 0;
    k.u &= ~0x40000000;
    if(is_horizontal)
    {
      n->k_h = k.f;
      n->k_v = 0.0f;
    }
    else
    {
      n->k_h = 0.0f;
      n->k_v = k.f;
    }

    n->angle = o->angle, n->cx = o->cx, n->cy = o->cy, n->cw = o->cw, n->ch = o->ch;
    n->kxa = n->kxd = 0.2f;
    n->kxc = n->kxb = 0.8f;
    n->kya = n->kyb = 0.2f;
    n->kyc = n->kyd = 0.8f;
    if(n->k_h == 0 && n->k_v == 0)
      n->k_type = 0;
    else
      n->k_type = 4;
    n->k_sym = 0;
    n->k_apply = 0;
    n->crop_auto = 1;

    // will be computed later, -2 here is used to detect uninitialized
    // value, -1 is already used for no clipping.
    n->ratio_d = n->ratio_n = -2;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_clipping_params_v5_t);
    *new_version = 5;
    return 0;
  }
  if(old_version == 3)
  {
    // old structure def
    typedef struct old_params_t
    {
      float angle, cx, cy, cw, ch, k_h, k_v;
    } old_params_t;

    const old_params_t *o = (old_params_t *)old_params;
    dt_iop_clipping_params_v5_t *n = malloc(sizeof(dt_iop_clipping_params_v5_t));

    n->angle = o->angle, n->cx = o->cx, n->cy = o->cy, n->cw = o->cw, n->ch = o->ch;
    n->k_h = o->k_h, n->k_v = o->k_v;
    n->kxa = n->kxd = 0.2f;
    n->kxc = n->kxb = 0.8f;
    n->kya = n->kyb = 0.2f;
    n->kyc = n->kyd = 0.8f;
    if(n->k_h == 0 && n->k_v == 0)
      n->k_type = 0;
    else
      n->k_type = 4;
    n->k_sym = 0;
    n->k_apply = 0;
    n->crop_auto = 1;

    // will be computed later, -2 here is used to detect uninitialized
    // value, -1 is already used for no clipping.
    n->ratio_d = n->ratio_n = -2;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_clipping_params_v5_t);
    *new_version = 5;
    return 0;
  }
  if(old_version == 4)
  {
    typedef struct old_params_t
    {
      float angle, cx, cy, cw, ch, k_h, k_v;
      float kxa, kya, kxb, kyb, kxc, kyc, kxd, kyd;
      int k_type, k_sym;
      int k_apply, crop_auto;
    } old_params_t;

    const old_params_t *o = (old_params_t *)old_params;
    dt_iop_clipping_params_v5_t *n = malloc(sizeof(dt_iop_clipping_params_v5_t));

    n->angle = o->angle, n->cx = o->cx, n->cy = o->cy, n->cw = o->cw, n->ch = o->ch;
    n->k_h = o->k_h, n->k_v = o->k_v;
    n->kxa = o->kxa, n->kxb = o->kxb, n->kxc = o->kxc, n->kxd = o->kxd;
    n->kya = o->kya, n->kyb = o->kyb, n->kyc = o->kyc, n->kyd = o->kyd;
    n->k_type = o->k_type;
    n->k_sym = o->k_sym;
    n->k_apply = o->k_apply;
    n->crop_auto = o->crop_auto;

    // will be computed later, -2 here is used to detect uninitialized
    // value, -1 is already used for no clipping.
    n->ratio_d = n->ratio_n = -2;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_clipping_params_v5_t);
    *new_version = 5;
    return 0;
  }

  return 1;
}


typedef struct dt_iop_clipping_data_t
{
  float angle;              // rotation angle
  float aspect;             // forced aspect ratio
  float m[4];               // rot/mirror matrix
  float inv_m[4];           // inverse of m (m^-1)
  float ki_h, k_h;          // keystone correction, ki and corrected k
  float ki_v, k_v;          // keystone correction, ki and corrected k
  float tx, ty;             // rotation center
  float cx, cy, cw, ch;     // crop window
  float cix, ciy;           // crop window on roi_out 1.0 scale
  uint32_t all_off;         // 1: v and h off, else one of them is used
  uint32_t flags;           // flipping flags
  uint32_t flip;            // flipped output buffer so more area would fit.

  dt_boundingbox_t k_space; // space for the "destination" rectangle of the keystone quadrilateral
  float kxa, kya, kxb, kyb, kxc, kyc, kxd,
      kyd; // point of the "source" quadrilatere (modified if keystone is not "full")
  float a, b, d, e, g, h; // value of the transformation matrix (c=f=0 && i=1)
  int k_apply;
  int crop_auto;
  float enlarge_x, enlarge_y;
} dt_iop_clipping_data_t;

typedef struct dt_iop_clipping_global_data_t
{
  int kernel_clip_rotate_bilinear;
  int kernel_clip_rotate_bicubic;
  int kernel_clip_rotate_lanczos2;
  int kernel_clip_rotate_lanczos3;
} dt_iop_clipping_global_data_t;


// helper to count corners in for loops:
static inline void get_corner(const float *aabb, const int i, float *p)
{
  for(int k = 0; k < 2; k++) p[k] = aabb[2 * ((i >> k) & 1) + k];
}

static inline void adjust_aabb(const float *p, float *aabb)
{
  aabb[0] = fminf(aabb[0], p[0]);
  aabb[1] = fminf(aabb[1], p[1]);
  aabb[2] = fmaxf(aabb[2], p[0]);
  aabb[3] = fmaxf(aabb[3], p[1]);
}

const char *deprecated_msg()
{
  return _("this module is deprecated. please use the crop, orientation and/or rotate and perspective modules instead.");
}

const char *name()
{
  return _("crop and rotate");
}

const char *aliases()
{
  return _("reframe|perspective|keystone|distortion");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("change the framing and correct the perspective"),
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
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_ALLOW_FAST_PIPE
         | IOP_FLAGS_GUIDES_SPECIAL_DRAW | IOP_FLAGS_GUIDES_WIDGET | IOP_FLAGS_DEPRECATED;
}

int operation_tags()
{
  return IOP_TAG_DISTORT | IOP_TAG_CROPPING;
}

int operation_tags_filter()
{
  // switch off watermark, it gets confused.
  return IOP_TAG_DECORATION | IOP_TAG_CROPPING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static void keystone_get_matrix(const dt_boundingbox_t k_space, float kxa, const float kxb, const float kxc, const float kxd, float kya,
                                const float kyb, const float kyc, const float kyd, float *a, float *b, float *d, float *e,
                                float *g, float *h)
{
  *a = -((kxb * (kyd * kyd - kyc * kyd) - kxc * kyd * kyd + kyb * (kxc * kyd - kxd * kyd) + kxd * kyc * kyd)
         * k_space[2])
       / (kxb * (kxc * kyd * kyd - kxd * kyc * kyd) + kyb * (kxd * kxd * kyc - kxc * kxd * kyd));
  *b = ((kxb * (kxd * kyd - kxd * kyc) - kxc * kxd * kyd + kxd * kxd * kyc + (kxc * kxd - kxd * kxd) * kyb)
        * k_space[2])
       / (kxb * (kxc * kyd * kyd - kxd * kyc * kyd) + kyb * (kxd * kxd * kyc - kxc * kxd * kyd));
  *d = (kyb * (kxb * (kyd * k_space[3] - kyc * k_space[3]) - kxc * kyd * k_space[3] + kxd * kyc * k_space[3])
        + kyb * kyb * (kxc * k_space[3] - kxd * k_space[3]))
       / (kxb * kyb * (-kxc * kyd - kxd * kyc) + kxb * kxb * kyc * kyd + kxc * kxd * kyb * kyb);
  *e = -(kxb * (kxd * kyc * k_space[3] - kxc * kyd * k_space[3])
         + kxb * kxb * (kyd * k_space[3] - kyc * k_space[3])
         + kxb * kyb * (kxc * k_space[3] - kxd * k_space[3]))
       / (kxb * kyb * (-kxc * kyd - kxd * kyc) + kxb * kxb * kyc * kyd + kxc * kxd * kyb * kyb);
  *g = -(kyb * (kxb * (2.0f * kxc * kyd * kyd - 2.0f * kxc * kyc * kyd) - kxc * kxc * kyd * kyd
                + 2.0f * kxc * kxd * kyc * kyd - kxd * kxd * kyc * kyc)
         + kxb * kxb * (kyc * kyc * kyd - kyc * kyd * kyd)
         + kyb * kyb * (-2.0f * kxc * kxd * kyd + kxc * kxc * kyd + kxd * kxd * kyc))
       / (kxb * kxb * (kxd * kyc * kyc * kyd - kxc * kyc * kyd * kyd)
          + kxb * kyb * (kxc * kxc * kyd * kyd - kxd * kxd * kyc * kyc)
          + kyb * kyb * (kxc * kxd * kxd * kyc - kxc * kxc * kxd * kyd));
  *h = (kxb * (-kxc * kxc * kyd * kyd + 2.0f * kxc * kxd * kyc * kyd - kxd * kxd * kyc * kyc)
        + kxb * kxb * (kxc * kyd * kyd - 2.0f * kxd * kyc * kyd + kxd * kyc * kyc)
        + kxb * (2.0f * kxd * kxd - 2.0f * kxc * kxd) * kyb * kyc
        + (kxc * kxc * kxd - kxc * kxd * kxd) * kyb * kyb)
       / (kxb * kxb * (kxd * kyc * kyc * kyd - kxc * kyc * kyd * kyd)
          + kxb * kyb * (kxc * kxc * kyd * kyd - kxd * kxd * kyc * kyc)
          + kyb * kyb * (kxc * kxd * kxd * kyc - kxc * kxc * kxd * kyd));
}

DT_OMP_DECLARE_SIMD()
static inline void keystone_backtransform(float *i, const dt_boundingbox_t k_space, const float a, const float b, const float d,
                                          const float e, const float g, const float h, const float kxa, const float kya)
{
  const float xx = i[0] - k_space[0];
  const float yy = i[1] - k_space[1];

  const float div = ((d * xx - a * yy) * h + (b * yy - e * xx) * g + a * e - b * d);

  i[0] = (e * xx - b * yy) / div + kxa;
  i[1] = -(d * xx - a * yy) / div + kya;
}

DT_OMP_DECLARE_SIMD()
static inline void keystone_transform(float *i, const dt_boundingbox_t k_space, const float a, const float b, const float d,
                                      const float e, const float g, const float h, const float kxa, const float kya)
{
  const float xx = i[0] - kxa;
  const float yy = i[1] - kya;

  const float div = g * xx + h * yy + 1;
  i[0] = (a * xx + b * yy) / div + k_space[0];
  i[1] = (d * xx + e * yy) / div + k_space[1];
}

DT_OMP_DECLARE_SIMD()
static inline void backtransform(float *x, float *o, const float *m, const float t_h, const float t_v)
{
  x[1] /= (1.0f + x[0] * t_h);
  x[0] /= (1.0f + x[1] * t_v);
  mul_mat_vec_2(m, x, o);
}

DT_OMP_DECLARE_SIMD()
static inline void inv_matrix(const float *m, float *inv_m)
{
  const float det = (m[0] * m[3]) - (m[1] * m[2]);
  inv_m[0] =  m[3] / det;
  inv_m[1] = -m[1] / det;
  inv_m[2] = -m[2] / det;
  inv_m[3] =  m[0] / det;
}

DT_OMP_DECLARE_SIMD()
static inline void transform(const float *x, float *o, const float *m, const float t_h, const float t_v)
{
  mul_mat_vec_2(m, x, o);
  o[1] *= (1.0f + o[0] * t_h);
  o[0] *= (1.0f + o[1] * t_v);
}

gboolean distort_transform(dt_iop_module_t *self,
                           dt_dev_pixelpipe_iop_t *piece,
                           float *const restrict points,
                           size_t points_count)
{
  // as dt_iop_roi_t contain int values and not floats, we can have some rounding errors
  // as a workaround, we use a factor for preview pipes
  float factor = 1.0f;
  if(piece->pipe->type & (DT_DEV_PIXELPIPE_PREVIEW | DT_DEV_PIXELPIPE_PREVIEW2)) factor = 100.0f;
  // we first need to be sure that all data values are computed
  // this is done in modify_roi_out fct, so we create tmp roi
  dt_iop_roi_t roi_out, roi_in;
  roi_in.width = piece->buf_in.width * factor;
  roi_in.height = piece->buf_in.height * factor;
  self->modify_roi_out(self, piece, &roi_out, &roi_in);

  const dt_iop_clipping_data_t *d = piece->data;

  const float rx = piece->buf_in.width;
  const float ry = piece->buf_in.height;

  const dt_boundingbox_t k_space = { d->k_space[0] * rx, d->k_space[1] * ry, d->k_space[2] * rx, d->k_space[3] * ry };
  const float kxa = d->kxa * rx, kxb = d->kxb * rx, kxc = d->kxc * rx, kxd = d->kxd * rx;
  const float kya = d->kya * ry, kyb = d->kyb * ry, kyc = d->kyc * ry, kyd = d->kyd * ry;
  float ma = 0, mb = 0, md = 0, me = 0, mg = 0, mh = 0;
  if(d->k_apply == 1)
    keystone_get_matrix(k_space, kxa, kxb, kxc, kxd, kya, kyb, kyc, kyd, &ma, &mb, &md, &me, &mg, &mh);

  DT_OMP_FOR(if(points_count > 100))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float pi[2], po[2];
    pi[0] = points[i];
    pi[1] = points[i + 1];

    if(d->k_apply == 1) keystone_transform(pi, k_space, ma, mb, md, me, mg, mh, kxa, kya);

    pi[0] -= d->tx / factor;
    pi[1] -= d->ty / factor;
    // transform this point using matrix m
    transform(pi, po, d->inv_m, d->k_h, d->k_v);

    if(d->flip)
    {
      po[1] += d->tx / factor;
      po[0] += d->ty / factor;
    }
    else
    {
      po[0] += d->tx / factor;
      po[1] += d->ty / factor;
    }

    points[i] = po[0] - (d->cix - d->enlarge_x) / factor;
    points[i + 1] = po[1] - (d->ciy - d->enlarge_y) / factor;
  }

  // revert side-effects of the previous call to modify_roi_out
  // TODO: this is just a quick hack. we need a major revamp of this module!
  if(factor != 1.0f)
  {
    roi_in.width = piece->buf_in.width;
    roi_in.height = piece->buf_in.height;
    self->modify_roi_out(self, piece, &roi_out, &roi_in);
  }

  return TRUE;
}
gboolean distort_backtransform(dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               float *const restrict points,
                               size_t points_count)
{
  // as dt_iop_roi_t contain int values and not floats, we can have some rounding errors
  // as a workaround, we use a factor for preview pipes
  float factor = 1.0f;
  if(piece->pipe->type & (DT_DEV_PIXELPIPE_PREVIEW | DT_DEV_PIXELPIPE_PREVIEW2)) factor = 100.0f;
  // we first need to be sure that all data values are computed
  // this is done in modify_roi_out fct, so we create tmp roi
  dt_iop_roi_t roi_out, roi_in;
  roi_in.width = piece->buf_in.width * factor;
  roi_in.height = piece->buf_in.height * factor;
  self->modify_roi_out(self, piece, &roi_out, &roi_in);

  const dt_iop_clipping_data_t *d = piece->data;

  const float rx = piece->buf_in.width;
  const float ry = piece->buf_in.height;

  const dt_boundingbox_t k_space = { d->k_space[0] * rx, d->k_space[1] * ry, d->k_space[2] * rx, d->k_space[3] * ry };
  const float kxa = d->kxa * rx, kxb = d->kxb * rx, kxc = d->kxc * rx, kxd = d->kxd * rx;
  const float kya = d->kya * ry, kyb = d->kyb * ry, kyc = d->kyc * ry, kyd = d->kyd * ry;
  float ma, mb, md, me, mg, mh;
  if(d->k_apply == 1)
    keystone_get_matrix(k_space, kxa, kxb, kxc, kxd, kya, kyb, kyc, kyd, &ma, &mb, &md, &me, &mg, &mh);

  DT_OMP_FOR_SIMD(if(points_count > 100) aligned(points:64) aligned(k_space:16))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float pi[2], po[2];
    pi[0] = -(d->enlarge_x - d->cix) / factor + points[i];
    pi[1] = -(d->enlarge_y - d->ciy) / factor + points[i + 1];

    // transform this point using matrix m
    if(d->flip)
    {
      pi[1] -= d->tx / factor;
      pi[0] -= d->ty / factor;
    }
    else
    {
      pi[0] -= d->tx / factor;
      pi[1] -= d->ty / factor;
    }

    backtransform(pi, po, d->m, d->k_h, d->k_v);

    po[0] += d->tx / factor;
    po[1] += d->ty / factor;
    if(d->k_apply == 1) keystone_backtransform(po, k_space, ma, mb, md, me, mg, mh, kxa, kya);

    points[i] = po[0];
    points[i + 1] = po[1];
  }

  // revert side-effects of the previous call to modify_roi_out
  // TODO: this is just a quick hack. we need a major revamp of this module!
  if(factor != 1.0f)
  {
    roi_in.width = piece->buf_in.width;
    roi_in.height = piece->buf_in.height;
    self->modify_roi_out(self, piece, &roi_out, &roi_in);
  }

  return TRUE;
}

void distort_mask(dt_iop_module_t *self,
                  dt_dev_pixelpipe_iop_t *piece,
                  const float *const in,
                  float *const out,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  const dt_iop_clipping_data_t *d = piece->data;

  // only crop, no rot fast and sharp path:
  if(!d->flags && d->angle == 0.0 && d->all_off && roi_in->width == roi_out->width &&
     roi_in->height == roi_out->height)
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
  }
  else
  {
    const dt_interpolation_t *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    const float rx = piece->buf_in.width * roi_in->scale;
    const float ry = piece->buf_in.height * roi_in->scale;
    const dt_boundingbox_t k_space =
      { d->k_space[0] * rx, d->k_space[1] * ry, d->k_space[2] * rx, d->k_space[3] * ry };
    const float kxa = d->kxa * rx, kxb = d->kxb * rx, kxc = d->kxc * rx, kxd = d->kxd * rx;
    const float kya = d->kya * ry, kyb = d->kyb * ry, kyc = d->kyc * ry, kyd = d->kyd * ry;
    float ma, mb, md, me, mg, mh;
    if(d->k_apply == 1)
      keystone_get_matrix(k_space, kxa, kxb, kxc, kxd, kya, kyb, kyc, kyd, &ma, &mb, &md, &me, &mg, &mh);

    DT_OMP_FOR(dt_omp_sharedconst(k_space))
    // (slow) point-by-point transformation.
    // TODO: optimize with scanlines and linear steps between?
    for(int j = 0; j < roi_out->height; j++)
    {
      float *_out = out + (size_t)j * roi_out->width;
      for(int i = 0; i < roi_out->width; i++)
      {
        float pi[2] = { 0.0f }, po[2] = { 0.0f };

        pi[0] = roi_out->x - roi_out->scale * d->enlarge_x + roi_out->scale * d->cix + i + 0.5f;
        pi[1] = roi_out->y - roi_out->scale * d->enlarge_y + roi_out->scale * d->ciy + j + 0.5f;

        // transform this point using matrix m
        if(d->flip)
        {
          pi[1] -= d->tx * roi_out->scale;
          pi[0] -= d->ty * roi_out->scale;
        }
        else
        {
          pi[0] -= d->tx * roi_out->scale;
          pi[1] -= d->ty * roi_out->scale;
        }
        pi[0] /= roi_out->scale;
        pi[1] /= roi_out->scale;
        backtransform(pi, po, d->m, d->k_h, d->k_v);
        po[0] *= roi_in->scale;
        po[1] *= roi_in->scale;
        po[0] += d->tx * roi_in->scale;
        po[1] += d->ty * roi_in->scale;
        if(d->k_apply == 1) keystone_backtransform(po, k_space, ma, mb, md, me, mg, mh, kxa, kya);
        po[0] -= roi_in->x + 0.5f;
        po[1] -= roi_in->y + 0.5f;

        _out[i] = CLIP(dt_interpolation_compute_sample(interpolation, in,
                                                       po[0], po[1],
                                                       roi_in->width, roi_in->height, 1, roi_in->width));
      }
    }
  }
}


void modify_roi_out(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in_orig)
{
  const dt_iop_roi_t roi_in_d = *roi_in_orig;
  const dt_iop_roi_t *roi_in = &roi_in_d;

  dt_iop_clipping_data_t *d = piece->data;

  // use whole-buffer roi information to create matrix and inverse.
  float rt[] = { cosf(d->angle), sinf(d->angle), -sinf(d->angle), cosf(d->angle) };
  if(d->angle == 0.0f)
  {
    rt[0] = rt[3] = 1.0f;
    rt[1] = rt[2] = 0.0f;
  }

  for(int k = 0; k < 4; k++) d->m[k] = rt[k];
  if(d->flags & FLAG_FLIP_HORIZONTAL)
  {
    d->m[0] = -rt[0];
    d->m[2] = -rt[2];
  }
  if(d->flags & FLAG_FLIP_VERTICAL)
  {
    d->m[1] = -rt[1];
    d->m[3] = -rt[3];
  }

  // now compute inverse of m
  inv_matrix(d->m, d->inv_m);

  if(d->k_apply == 0 && d->crop_auto == 1) // this is the old solution.
  {
    float inv_rt[4] = { 0.0f };
    inv_matrix(rt, inv_rt);

    *roi_out = *roi_in;
    // correct keystone correction factors by resolution of this buffer
    const float kc = 1.0f / fminf(roi_in->width, roi_in->height);
    d->k_h = d->ki_h * kc;
    d->k_v = d->ki_v * kc;

    float cropscale = -1.0f;
    // check portrait/landscape orientation, whichever fits more area:
    const float oaabb[4]
        = { -.5f * roi_in->width, -.5f * roi_in->height, .5f * roi_in->width, .5f * roi_in->height };
    for(int flip = 0; flip < 2; flip++)
    {
      const float roi_in_width = flip ? roi_in->height : roi_in->width;
      const float roi_in_height = flip ? roi_in->width : roi_in->height;
      float newcropscale = 1.0f;
      // fwd transform rotated points on corners and scale back inside roi_in bounds.
      float p[2], o[2];
      const float aabb[4] = { -.5f * roi_in_width, -.5f * roi_in_height, .5f * roi_in_width, .5f * roi_in_height };
      for(int c = 0; c < 4; c++)
      {
        get_corner(oaabb, c, p);
        transform(p, o, inv_rt, d->k_h, d->k_v);
        for(int k = 0; k < 2; k++)
          if(fabsf(o[k]) > 0.001f) newcropscale = fminf(newcropscale, aabb[(o[k] > 0 ? 2 : 0) + k] / o[k]);
      }
      if(newcropscale >= cropscale)
      {
        cropscale = newcropscale;
        // remember rotation center in whole-buffer coordinates:
        d->tx = roi_in->width * .5f;
        d->ty = roi_in->height * .5f;
        d->flip = flip;

        const float ach = d->ch - d->cy, acw = d->cw - d->cx;
        // rotate and clip to max extent
        if(flip)
        {
          roi_out->y = d->tx - (.5f - d->cy) * cropscale * roi_in->width;
          roi_out->x = d->ty - (.5f - d->cx) * cropscale * roi_in->height;
          roi_out->height = ach * cropscale * roi_in->width;
          roi_out->width = acw * cropscale * roi_in->height;
        }
        else
        {
          roi_out->x = d->tx - (.5f - d->cx) * cropscale * roi_in->width;
          roi_out->y = d->ty - (.5f - d->cy) * cropscale * roi_in->height;
          roi_out->width = acw * cropscale * roi_in->width;
          roi_out->height = ach * cropscale * roi_in->height;
        }
      }
    }
  }
  else
  {
    *roi_out = *roi_in;
    // set roi_out values with rotation and keystone
    // initial corners pos
    const dt_boundingbox_t corn_x = { 0.0f, roi_in->width, roi_in->width, 0.0f };
    const dt_boundingbox_t corn_y = { 0.0f, 0.0f, roi_in->height, roi_in->height };
    // destination corner points
    dt_boundingbox_t corn_out_x = { 0.0f };
    dt_boundingbox_t corn_out_y = { 0.0f };

    // we don't test image flip as autocrop is not completely ok...
    d->flip = 0;

    // we apply rotation and keystone to all those points
    float p[2], o[2];
    for(int c = 0; c < 4; c++)
    {
      // keystone
      o[0] = corn_x[c];
      o[1] = corn_y[c];
      if(d->k_apply == 1)
      {
        o[0] /= (float)roi_in->width;
        o[1] /= (float)roi_in->height;
        keystone_transform(o, d->k_space, d->a, d->b, d->d, d->e, d->g, d->h, d->kxa, d->kya);
        o[0] *= roi_in->width;
        o[1] *= roi_in->height;
      }
      // rotation
      p[0] = o[0] - .5f * roi_in->width;
      p[1] = o[1] - .5f * roi_in->height;
      transform(p, o, d->inv_m, d->k_h, d->k_v);
      o[0] += .5f * roi_in->width;
      o[1] += .5f * roi_in->height;

      // and we set the values
      corn_out_x[c] = o[0];
      corn_out_y[c] = o[1];
    }

    float new_x = min4f(corn_out_x);
    if(new_x + roi_in->width < 0) new_x = -roi_in->width;
    float new_y = min4f(corn_out_y);
    if(new_y + roi_in->height < 0) new_y = -roi_in->height;

    float new_sc_x = max4f(corn_out_x);
    if(new_sc_x > 2.0f * roi_in->width) new_sc_x = 2.0f * roi_in->width;
    float new_sc_y = max4f(corn_out_y);
    if(new_sc_y > 2.0f * roi_in->height) new_sc_y = 2.0f * roi_in->height;

    // be careful, we don't want too small area here !
    if(new_sc_x - new_x < roi_in->width / 8.0f)
    {
      float f = (new_sc_x + new_x) / 2.0f;
      if(f < roi_in->width / 16.0f) f = roi_in->width / 16.0f;
      if(f >= roi_in->width * 15.0f / 16.0f) f = roi_in->width * 15.0f / 16.0f - 1.0f;
      new_x = f - roi_in->width / 16.0f, new_sc_x = f + roi_in->width / 16.0f;
    }
    if(new_sc_y - new_y < roi_in->height / 8.0f)
    {
      float f = (new_sc_y + new_y) / 2.0f;
      if(f < roi_in->height / 16.0f) f = roi_in->height / 16.0f;
      if(f >= roi_in->height * 15.0f / 16.0f) f = roi_in->height * 15.0f / 16.0f - 1.0f;
      new_y = f - roi_in->height / 16.0f, new_sc_y = f + roi_in->height / 16.0f;
    }

    new_sc_y = new_sc_y - new_y;
    new_sc_x = new_sc_x - new_x;

    // now we apply the clipping
    new_x += d->cx * new_sc_x;
    new_y += d->cy * new_sc_y;
    new_sc_x *= d->cw - d->cx;
    new_sc_y *= d->ch - d->cy;

    d->enlarge_x = fmaxf(-new_x, 0.0f);
    roi_out->x = fmaxf(new_x, 0.0f);
    d->enlarge_y = fmaxf(-new_y, 0.0f);
    roi_out->y = fmaxf(new_y, 0.0f);

    roi_out->width = new_sc_x;
    roi_out->height = new_sc_y;
    d->tx = roi_in->width * .5f;
    d->ty = roi_in->height * .5f;
  }

  // sanity check.
  if(roi_out->x < 0) roi_out->x = 0;
  if(roi_out->y < 0) roi_out->y = 0;
  if(roi_out->width < 4 || roi_out->height < 4)
  {
    dt_print_pipe(DT_DEBUG_PIPE,
      "safety check", piece->pipe, self, DT_DEVICE_NONE, roi_in, roi_out);

    roi_out->x = roi_in->x;
    roi_out->y = roi_in->y;
    roi_out->width = roi_in->width;
    roi_out->height = roi_in->height;
    piece->enabled = FALSE;

    if(piece->pipe->type & DT_DEV_PIXELPIPE_FULL)
      dt_control_log
        (_("module '%s' has insane data so it is bypassed for now. you should disable it or change parameters\n"),
         self->name());
  }

  // save rotation crop on output buffer in world scale:
  d->cix = roi_out->x;
  d->ciy = roi_out->y;
}

void modify_roi_in(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  const dt_iop_clipping_data_t *d = piece->data;
  *roi_in = *roi_out;
  // modify_roi_out took care of bounds checking for us. we hopefully do not get requests outside the clipping
  // area.
  // transform aabb back to roi_in

  // this aabb is set off by cx/cy
  const float so = roi_out->scale;
  const float kw = piece->buf_in.width * so, kh = piece->buf_in.height * so;
  const float roi_out_x = roi_out->x - d->enlarge_x * so, roi_out_y = roi_out->y - d->enlarge_y * so;
  float p[2], o[2];
  const dt_boundingbox_t aabb = { roi_out_x + d->cix * so, roi_out_y + d->ciy * so, roi_out_x + d->cix * so + roi_out->width,
                  roi_out_y + d->ciy * so + roi_out->height };
  dt_boundingbox_t aabb_in = { FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
  for(int c = 0; c < 4; c++)
  {
    // get corner points of roi_out
    get_corner(aabb, c, p);

    // backtransform aabb using m
    if(d->flip)
    {
      p[1] -= d->tx * so;
      p[0] -= d->ty * so;
    }
    else
    {
      p[0] -= d->tx * so;
      p[1] -= d->ty * so;
    }
    p[0] *= 1.0 / so;
    p[1] *= 1.0 / so;
    backtransform(p, o, d->m, d->k_h, d->k_v);
    o[0] *= so;
    o[1] *= so;
    o[0] += d->tx * so;
    o[1] += d->ty * so;
    o[0] /= kw;
    o[1] /= kh;
    if(d->k_apply == 1)
      keystone_backtransform(o, d->k_space, d->a, d->b, d->d, d->e, d->g, d->h, d->kxa, d->kya);
    o[0] *= kw;
    o[1] *= kh;
    // transform to roi_in space, get aabb.
    adjust_aabb(o, aabb_in);
  }

  // adjust roi_in to minimally needed region
  roi_in->x = aabb_in[0] - 1;
  roi_in->y = aabb_in[1] - 1;
  roi_in->width = aabb_in[2] - aabb_in[0] + 2;
  roi_in->height = aabb_in[3] - aabb_in[1] + 2;

  if(d->angle == 0.0f && d->all_off)
  {
    // just crop: make sure everything is precise.
    roi_in->x = aabb_in[0];
    roi_in->y = aabb_in[1];
    roi_in->width = roi_out->width;
    roi_in->height = roi_out->height;
  }

  // sanity check.
  const float scwidth = piece->buf_in.width * so, scheight = piece->buf_in.height * so;
  roi_in->x = CLAMP(roi_in->x, 0, (int)floorf(scwidth));
  roi_in->y = CLAMP(roi_in->y, 0, (int)floorf(scheight));
  roi_in->width = CLAMP(roi_in->width, 1, (int)ceilf(scwidth) - roi_in->x);
  roi_in->height = CLAMP(roi_in->height, 1, (int)ceilf(scheight) - roi_in->y);
}

// 3rd (final) pass: you get this input region (may be different from what was requested above),
// do your best to fill the output region!
void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4/*need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // unsupported format, image has been copied to output and module's trouble flag set

  const dt_iop_clipping_data_t *d = piece->data;

  const int ch = 4;
  const int ch_width = ch * roi_in->width;

  // only crop, no rot fast and sharp path:
  if(!d->flags && d->angle == 0.0 && d->all_off && roi_in->width == roi_out->width
     && roi_in->height == roi_out->height)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
  }
  else
  {
    const dt_interpolation_t *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    const float rx = piece->buf_in.width * roi_in->scale;
    const float ry = piece->buf_in.height * roi_in->scale;
    const dt_boundingbox_t k_space =
      { d->k_space[0] * rx, d->k_space[1] * ry, d->k_space[2] * rx, d->k_space[3] * ry };
    const float kxa = d->kxa * rx, kxb = d->kxb * rx, kxc = d->kxc * rx, kxd = d->kxd * rx;
    const float kya = d->kya * ry, kyb = d->kyb * ry, kyc = d->kyc * ry, kyd = d->kyd * ry;
    float ma, mb, md, me, mg, mh;
    if(d->k_apply == 1)
      keystone_get_matrix(k_space, kxa, kxb, kxc, kxd, kya, kyb, kyc, kyd, &ma, &mb, &md, &me, &mg, &mh);

    DT_OMP_FOR(dt_omp_sharedconst(k_space))
    // (slow) point-by-point transformation.
    // TODO: optimize with scanlines and linear steps between?
    for(int j = 0; j < roi_out->height; j++)
    {
      float *out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;
      for(int i = 0; i < roi_out->width; i++)
      {
        float pi[2], po[2];

        pi[0] = roi_out->x - roi_out->scale * d->enlarge_x + roi_out->scale * d->cix + i + 0.5f;
        pi[1] = roi_out->y - roi_out->scale * d->enlarge_y + roi_out->scale * d->ciy + j + 0.5f;

        // transform this point using matrix m
        if(d->flip)
        {
          pi[1] -= d->tx * roi_out->scale;
          pi[0] -= d->ty * roi_out->scale;
        }
        else
        {
          pi[0] -= d->tx * roi_out->scale;
          pi[1] -= d->ty * roi_out->scale;
        }
        pi[0] /= roi_out->scale;
        pi[1] /= roi_out->scale;
        backtransform(pi, po, d->m, d->k_h, d->k_v);
        po[0] *= roi_in->scale;
        po[1] *= roi_in->scale;
        po[0] += d->tx * roi_in->scale;
        po[1] += d->ty * roi_in->scale;
        if(d->k_apply == 1) keystone_backtransform(po, k_space, ma, mb, md, me, mg, mh, kxa, kya);
        po[0] -= roi_in->x + 0.5f;
        po[1] -= roi_in->y + 0.5f;

        dt_interpolation_compute_pixel4c(interpolation, (float *)ivoid, out + ch*i, po[0], po[1], roi_in->width,
                                         roi_in->height, ch_width);
      }
    }
  }
}


#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_clipping_data_t *d = piece->data;
  const dt_iop_clipping_global_data_t *gd = self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  // only crop, no rot fast and sharp path:
  if(!d->flags && d->angle == 0.0 && d->all_off && roi_in->width == roi_out->width
     && roi_in->height == roi_out->height)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    int crkernel = -1;

    const dt_interpolation_t *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

    switch(interpolation->id)
    {
      case DT_INTERPOLATION_BILINEAR:
        crkernel = gd->kernel_clip_rotate_bilinear;
        break;
      case DT_INTERPOLATION_BICUBIC:
        crkernel = gd->kernel_clip_rotate_bicubic;
        break;
      case DT_INTERPOLATION_LANCZOS2:
        crkernel = gd->kernel_clip_rotate_lanczos2;
        break;
      case DT_INTERPOLATION_LANCZOS3:
        crkernel = gd->kernel_clip_rotate_lanczos3;
        break;
      default:
        return err;
    }

    const int roi[2] = { roi_in->x, roi_in->y };
    const float roo[2] = { roi_out->x - roi_out->scale * d->enlarge_x + roi_out->scale * d->cix,
                     roi_out->y - roi_out->scale * d->enlarge_y + roi_out->scale * d->ciy };
    const float t[2] = { d->tx, d->ty };
    const float k[2] = { d->k_h, d->k_v };
    const float m[4] = { d->m[0], d->m[1], d->m[2], d->m[3] };

    const float k_sizes[2] = { piece->buf_in.width * roi_in->scale, piece->buf_in.height * roi_in->scale };
    const dt_boundingbox_t k_space = { d->k_space[0] * k_sizes[0], d->k_space[1] * k_sizes[1],
                                       d->k_apply ? d->k_space[2] * k_sizes[0] : 0.0f,
                                       d->k_space[3] * k_sizes[1] };
    float ma, mb, md, me, mg, mh;
    keystone_get_matrix(k_space, d->kxa * k_sizes[0], d->kxb * k_sizes[0], d->kxc * k_sizes[0],
                        d->kxd * k_sizes[0], d->kya * k_sizes[1], d->kyb * k_sizes[1], d->kyc * k_sizes[1],
                        d->kyd * k_sizes[1], &ma, &mb, &md, &me, &mg, &mh);
    const float ka[2] = { d->kxa * k_sizes[0], d->kya * k_sizes[1] };
    const float maa[4] = { ma, mb, md, me };
    const float mbb[2] = { mg, mh };

    size_t sizes[3];

    sizes[0] = ROUNDUPDWD(width, devid);
    sizes[1] = ROUNDUPDHT(height, devid);
    sizes[2] = 1;
    dt_opencl_set_kernel_args(devid, crkernel, 0, CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height),
      CLARG(roi_in->width), CLARG(roi_in->height), CLARG(roi), CLARG(roo), CLARG(roi_in->scale), CLARG(roi_out->scale),
      CLARG(d->flip), CLARG(t), CLARG(k), CLARG(m), CLARG(k_space), CLARG(ka), CLARG(maa), CLARG(mbb));
    err = dt_opencl_enqueue_kernel_2d(devid, crkernel, sizes);
  }

error:
  return err;
}
#endif

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_clipping_global_data_t *gd = malloc(sizeof(dt_iop_clipping_global_data_t));
  self->data = gd;
  gd->kernel_clip_rotate_bilinear = dt_opencl_create_kernel(program, "clip_rotate_bilinear");
  gd->kernel_clip_rotate_bicubic = dt_opencl_create_kernel(program, "clip_rotate_bicubic");
  gd->kernel_clip_rotate_lanczos2 = dt_opencl_create_kernel(program, "clip_rotate_lanczos2");
  gd->kernel_clip_rotate_lanczos3 = dt_opencl_create_kernel(program, "clip_rotate_lanczos3");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  const dt_iop_clipping_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_clip_rotate_bilinear);
  dt_opencl_free_kernel(gd->kernel_clip_rotate_bicubic);
  dt_opencl_free_kernel(gd->kernel_clip_rotate_lanczos2);
  dt_opencl_free_kernel(gd->kernel_clip_rotate_lanczos3);
  free(self->data);
  self->data = NULL;
}


void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)p1;
  dt_iop_clipping_data_t *d = piece->data;

  // reset all values to be sure everything is initialized
  d->m[0] = d->m[3] = 1.0f;
  d->m[1] = d->m[2] = 0.0f;
  d->ki_h = d->ki_v = d->k_h = d->k_v = 0.0f;
  d->tx = d->ty = 0.0f;
  d->cix = d->ciy = 0.0f;
  d->kxa = d->kxd = d->kya = d->kyb = 0.0f;
  d->kxb = d->kxc = d->kyc = d->kyd = 0.6f;
  d->k_space[0] = d->k_space[1] = 0.2f;
  d->k_space[2] = d->k_space[3] = 0.6f;
  d->k_apply = 0;
  d->enlarge_x = d->enlarge_y = 0.0f;
  d->flip = 0;
  d->angle = deg2radf(p->angle);

  // image flip
  d->flags = (p->ch < 0 ? FLAG_FLIP_VERTICAL : 0) | (p->cw < 0 ? FLAG_FLIP_HORIZONTAL : 0);
  d->crop_auto = p->crop_auto;

  // keystones values computation
  if(p->k_type == 4)
  {
    // this is for old keystoning
    d->k_apply = 0;
    d->all_off = 1;
    if(fabsf(p->k_h) >= .0001) d->all_off = 0;
    if(p->k_h >= -1.0 && p->k_h <= 1.0)
      d->ki_h = p->k_h;
    else
      d->ki_h = 0.0f;
    if(fabsf(p->k_v) >= .0001) d->all_off = 0;
    if(p->k_v >= -1.0 && p->k_v <= 1.0)
      d->ki_v = p->k_v;
    else
      d->ki_v = 0.0f;
  }
  else if(p->k_type >= 0 && p->k_apply == 1)
  {
    // we reset old keystoning values
    d->ki_h = d->ki_v = 0;
    d->kxa = p->kxa;
    d->kxb = p->kxb;
    d->kxc = p->kxc;
    d->kxd = p->kxd;
    d->kya = p->kya;
    d->kyb = p->kyb;
    d->kyc = p->kyc;
    d->kyd = p->kyd;
    // we adjust the points if the keystoning is not in "full" mode
    if(p->k_type == 1) // we want horizontal points to be aligned
    {
      // line equations parameters
      const float a1 = (d->kxd - d->kxa) / (d->kyd - d->kya);
      const float b1 = d->kxa - a1 * d->kya;
      const float a2 = (d->kxc - d->kxb) / (d->kyc - d->kyb);
      const float b2 = d->kxb - a2 * d->kyb;

      if(d->kya > d->kyb)
      {
        // we move kya to the level of kyb
        d->kya = d->kyb;
        d->kxa = a1 * d->kya + b1;
      }
      else
      {
        // we move kyb to the level of kya
        d->kyb = d->kya;
        d->kxb = a2 * d->kyb + b2;
      }

      if(d->kyc > d->kyd)
      {
        // we move kyd to the level of kyc
        d->kyd = d->kyc;
        d->kxd = a1 * d->kyd + b1;
      }
      else
      {
        // we move kyc to the level of kyd
        d->kyc = d->kyd;
        d->kxc = a2 * d->kyc + b2;
      }
    }
    else if(p->k_type == 2) // we want vertical points to be aligned
    {
      // line equations parameters
      const float a1 = (d->kyb - d->kya) / (d->kxb - d->kxa);
      const float b1 = d->kya - a1 * d->kxa;
      const float a2 = (d->kyc - d->kyd) / (d->kxc - d->kxd);
      const float b2 = d->kyd - a2 * d->kxd;

      if(d->kxa > d->kxd)
      {
        // we move kxa to the level of kxd
        d->kxa = d->kxd;
        d->kya = a1 * d->kxa + b1;
      }
      else
      {
        // we move kyb to the level of kya
        d->kxd = d->kxa;
        d->kyd = a2 * d->kxd + b2;
      }

      if(d->kxc > d->kxb)
      {
        // we move kyd to the level of kyc
        d->kxb = d->kxc;
        d->kyb = a1 * d->kxb + b1;
      }
      else
      {
        // we move kyc to the level of kyd
        d->kxc = d->kxb;
        d->kyc = a2 * d->kxc + b2;
      }
    }
    d->k_space[0] = fabsf((d->kxa + d->kxd) / 2.0f);
    d->k_space[1] = fabsf((d->kya + d->kyb) / 2.0f);
    d->k_space[2] = fabsf((d->kxb + d->kxc) / 2.0f) - d->k_space[0];
    d->k_space[3] = fabsf((d->kyc + d->kyd) / 2.0f) - d->k_space[1];
    d->kxb = d->kxb - d->kxa;
    d->kxc = d->kxc - d->kxa;
    d->kxd = d->kxd - d->kxa;
    d->kyb = d->kyb - d->kya;
    d->kyc = d->kyc - d->kya;
    d->kyd = d->kyd - d->kya;
    keystone_get_matrix(d->k_space, d->kxa, d->kxb, d->kxc, d->kxd, d->kya, d->kyb, d->kyc, d->kyd, &d->a,
                        &d->b, &d->d, &d->e, &d->g, &d->h);

    d->k_apply = 1;
    d->all_off = 0;
    d->crop_auto = 0;
  }
  else
  {
    d->all_off = 1;
    d->k_apply = 0;
  }

  if(dt_iop_has_focus(self))
  {
    d->cx = 0.0f;
    d->cy = 0.0f;
    d->cw = 1.0f;
    d->ch = 1.0f;
  }
  else
  {
    d->cx = CLAMPF(p->cx, 0.0f, 0.9f);
    d->cy = CLAMPF(p->cy, 0.0f, 0.9f);
    d->cw = CLAMPF(fabsf(p->cw), 0.1f, 1.0f);
    d->ch = CLAMPF(fabsf(p->ch), 0.1f, 1.0f);
    // we show a error on stderr if we have clamped something
    if(d->cx != p->cx || d->cy != p->cy || d->cw != fabsf(p->cw) || d->ch != fabsf(p->ch))
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[crop&rotate] invalid crop data for %d : x=%0.04f y=%0.04f w=%0.04f h=%0.04f",
               pipe->image.id, p->cx, p->cy, p->cw, p->ch);
    }
  }
}


void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_clipping_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


void reload_defaults(dt_iop_module_t *self)
{
  const dt_image_t *img = &self->dev->image_storage;

  dt_iop_clipping_params_t *d = self->default_params;

  d->cx = img->usercrop[1];
  d->cy = img->usercrop[0];
  d->cw = img->usercrop[3];
  d->ch = img->usercrop[2];
}


static gint _aspect_ratio_cmp(const dt_iop_clipping_aspect_t *a, const dt_iop_clipping_aspect_t *b)
{
  // want most square at the end, and the most non-square at the beginning

  if((a->d == 0 || a->d == 1) && a->n == 0) return -1;

  const float ad = MAX(a->d, a->n);
  const float an = MIN(a->d, a->n);
  const float bd = MAX(b->d, b->n);
  const float bn = MIN(b->d, b->n);
  const float aratio = ad / an;
  const float bratio = bd / bn;

  if(aratio < bratio) return -1;

  const float prec = 0.0003f;
  if(fabsf(aratio - bratio) < prec) return 0;

  return 1;
}


static _grab_region_t get_grab(const float pzx, const float pzy, const dt_iop_clipping_gui_data_t *g, const float border,
                               const float wd, const float ht)
{
  _grab_region_t grab = GRAB_NONE;
  if(!(pzx < g->clip_x || pzx > g->clip_x + g->clip_w || pzy < g->clip_y || pzy > g->clip_y + g->clip_h))
  {
    // we are inside the crop box
    grab = GRAB_CENTER;
    if(pzx >= g->clip_x && pzx * wd < g->clip_x * wd + border) grab |= GRAB_LEFT; // left border
    if(pzy >= g->clip_y && pzy * ht < g->clip_y * ht + border) grab |= GRAB_TOP;  // top border
    if(pzx <= g->clip_x + g->clip_w && pzx * wd > (g->clip_w + g->clip_x) * wd - border)
      grab |= GRAB_RIGHT; // right border
    if(pzy <= g->clip_y + g->clip_h && pzy * ht > (g->clip_h + g->clip_y) * ht - border)
      grab |= GRAB_BOTTOM; // bottom border
  }
  return grab;
}

// draw symmetry signs

// draw guides and handles over the image

// determine the distance between the segment [(xa,ya)(xb,yb)] and the point (xc,yc)


GSList *mouse_actions(dt_iop_module_t *self)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG, 0, _("[%s on borders] crop"), self->name());
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG, GDK_SHIFT_MASK,
                                     _("[%s on borders] crop keeping ratio"), self->name());
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_RIGHT_DRAG, 0, _("[%s] define/rotate horizon"), self->name());
  return lm;
}

#undef PHI
#undef INVPHI

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
