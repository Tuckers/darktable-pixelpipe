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

static void _commit_box(dt_iop_module_t *self,
                        dt_iop_crop_gui_data_t *g,
                        dt_iop_crop_params_t *p,
                        const gboolean enforce_history)
{
  if(darktable.gui->reset)
    return;
  if(self->dev->preview_pipe->status != DT_DEV_PIXELPIPE_VALID)
    return;

  g->cropping = GRAB_CENTER;
  const dt_boundingbox_t old = { p->cx, p->cy, p->cw, p->ch };
  const float eps = 1e-6f; // threshold to avoid rounding errors
  if(!self->enabled)
  {
    // first time crop, if any data is stored in p, it's obsolete:
    p->cx = p->cy = 0.0f;
    p->cw = p->ch = 1.0f;
  }

  // we want value in iop space
  dt_dev_pixelpipe_t *fpipe = self->dev->full.pipe;
  const float wd = fpipe->processed_width;
  const float ht = fpipe->processed_height;
  dt_boundingbox_t points = { g->clip_x * wd,
                              g->clip_y * ht,
                             (g->clip_x + g->clip_w) * wd,
                             (g->clip_y + g->clip_h) * ht };
  if(dt_dev_distort_backtransform_plus(self->dev, fpipe, self->iop_order,
                                       DT_DEV_TRANSFORM_DIR_FORW_EXCL, points, 2))
  {
    dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, fpipe, self);
    if(piece)
    {
      if(piece->buf_out.width < 1 || piece->buf_out.height < 1)
        return;

      // Do we need to align on a given ratio?
      // excludes original image (portrait and landscape) and free
      if(p->ratio_d != 0 && p->ratio_n != 0)
      {
        const gboolean landscape = piece->buf_out.width >= piece->buf_out.height;
        const gboolean flipped = p->ratio_d < 0 ;
        const float rd = MAX(1, abs(p->ratio_d));
        const float rn = MAX(1, p->ratio_n);
        const float aspect = flipped  ? rn / rd : rd / rn;
        float width = points[2] - points[0];
        float height = points[3] - points[1];
        if(width > height)  height = landscape ? width / aspect : width * aspect;
        else                width  = landscape ? height * aspect : height / aspect;

        int align_w = flipped == landscape ? p->ratio_n : p->ratio_d;
        int align_h = flipped == landscape ? p->ratio_d : p->ratio_n;
        const gboolean exact = _reduce_aligners(&align_w, &align_h);
        const int dw = exact ? (int)width  % align_w : 0;
        const int dh = exact ? (int)height % align_h : 0;
        points[0] += (float)dw / 2.0f;
        points[1] += (float)dh / 2.0f;
        points[2] = points[0] + width - dw;
        points[3] = points[1] + height - dh;

        dt_print(DT_DEBUG_PIPE, "[commit crop] %s %s aspect=%.2f rn=%d rd=%+d aw=%d ah=%d dw=%d dh=%d size: %.3f x %.3f --> %.3f x %.3f",
            landscape ? "landscape" : "portrait",
            exact ? "exact" : "as is",
            aspect, p->ratio_n, p->ratio_d, align_w, align_h, dw, dh, width, height, points[2] - points[0], points[3] - points[1]);
      }

      // use possibly aspect corrected data and verify that the crop area stays in the image area
      p->cx = CLAMPF(points[0] / (float)piece->buf_out.width,   0.0f, 1.0f - MIN_CROP_SIZE);
      p->cy = CLAMPF(points[1] / (float)piece->buf_out.height,  0.0f, 1.0f - MIN_CROP_SIZE);
      p->cw = CLAMPF(points[2] / (float)piece->buf_out.width,   MIN_CROP_SIZE, 1.0f);
      p->ch = CLAMPF(points[3] / (float)piece->buf_out.height,  MIN_CROP_SIZE, 1.0f);

      if(enforce_history
         || !feqf(p->cx, old[0], eps)
         || !feqf(p->cy, old[1], eps)
         || !feqf(p->cw, old[2], eps)
         || !feqf(p->ch, old[3], eps))
        dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }
}


gboolean distort_transform(dt_iop_module_t *self,
                           dt_dev_pixelpipe_iop_t *piece,
                           float *const restrict points,
                           size_t points_count)
{
  dt_iop_crop_data_t *d = piece->data;

  const float crop_top = piece->buf_in.height * d->cy;
  const float crop_left = piece->buf_in.width * d->cx;

  // nothing to be done if parameters are set to neutral values (no top/left border)
  if(crop_top <= 0.0f && crop_left <= 0.0f) return TRUE;

  float *const pts = DT_IS_ALIGNED(points);

  DT_OMP_FOR(if(points_count > 100))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    pts[i] -= crop_left;
    pts[i + 1] -= crop_top;
  }

  return TRUE;
}

gboolean distort_backtransform(dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               float *const restrict points,
                               size_t points_count)
{
  dt_iop_crop_data_t *d = piece->data;

  const float crop_top = piece->buf_in.height * d->cy;
  const float crop_left = piece->buf_in.width * d->cx;

  // nothing to be done if parameters are set to neutral values (no top/left border)
  if(crop_top <= 0.0f && crop_left <= 0.0f) return TRUE;

  float *const pts = DT_IS_ALIGNED(points);

  DT_OMP_FOR(if(points_count > 100))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    pts[i] += crop_left;
    pts[i + 1] += crop_top;
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
  dt_iop_copy_image_roi(out, in, 1, roi_in, roi_out);
}

void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  dt_iop_crop_data_t *d = piece->data;

  const float px = (float)roi_in->width * d->cx;
  const float py = (float)roi_in->height * d->cy;
  float odx = (float)roi_in->width * (d->cw - d->cx);
  float ody = (float)roi_in->height * (d->ch - d->cy);
  // write and ensure sane data
  roi_out->x = MAX(0, (int)px);
  roi_out->y = MAX(0, (int)py);
  roi_out->width = MAX(4, (int)odx);
  roi_out->height = MAX(4, (int)ody);

  const gboolean exporting = piece->pipe->type & (DT_DEV_PIXELPIPE_EXPORT | DT_DEV_PIXELPIPE_THUMBNAIL);
  const gboolean aligned = d->ratio_d != 0 && d->ratio_n != 0;
  if(!exporting || !aligned)
    return;

  odx = floorf(odx);
  ody = floorf(ody);
  // For exporting pipelines we do some extra work to ensure exact dimensions
  // if the aspect has been toggled it's presented here as negative
  const float aspect = d->aspect < 0.0f ? fabsf(1.0f / d->aspect) : d->aspect;
  const gboolean keep_aspect = aspect > 1e-5;
  const gboolean landscape = roi_in->width >= roi_in->height;

  float width = odx;
  float height = ody;
  // so lets possibly enforce the ratio using the larger side as reference
  if(keep_aspect)
  {
    if(odx > ody) height = floorf(landscape ? odx / aspect : odx * aspect);
    else          width  = floorf(landscape ? ody * aspect : ody / aspect);
  }

  int align_w = roi_out->width >= roi_out->height ? d->ratio_d : d->ratio_n;
  int align_h = roi_out->width >= roi_out->height ? d->ratio_n : d->ratio_d;
  const gboolean exact = _reduce_aligners(&align_w, &align_h);
  const int dw = exact ? (roi_out->width  % align_w) : 0;
  const int dh = exact ? (roi_out->height % align_h) : 0;
  roi_out->x += dw / 2;
  roi_out->y += dh / 2;
  roi_out->width = MAX(4, roi_out->width - dw);
  roi_out->height = MAX(4, roi_out->height - dh);
  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE,
    "crop aspects", piece->pipe, self, DT_DEVICE_NONE, roi_in, NULL,
    " %s%s%sAspect=%.3f. odx: %.1f ody: %.1f --> width: %.1f height: %.1f aligners=%d %d corr=%d %d",
    d->aspect < 0.0f ? "toggled " : "",
    keep_aspect ? "fixed " : "",
    landscape ? "landscape " : "portrait ",
    aspect, odx, ody, width, height,
    align_w, align_h, dw, dh);
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  dt_iop_crop_data_t *d = piece->data;
  *roi_in = *roi_out;

  const float iw = piece->buf_in.width * roi_out->scale;
  const float ih = piece->buf_in.height * roi_out->scale;

  roi_in->x += iw * d->cx;
  roi_in->y += ih * d->cy;

  roi_in->x = CLAMP(roi_in->x, 0, (int)floorf(iw));
  roi_in->y = CLAMP(roi_in->y, 0, (int)floorf(ih));
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_copy_image_roi(ovoid, ivoid, 4, roi_in, roi_out);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { roi_out->width, roi_out->height, 1 };
  return dt_opencl_enqueue_copy_image(piece->pipe->devid, dev_in, dev_out,
                                            origin, origin, region);
}
#endif

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_crop_params_t *p = (dt_iop_crop_params_t *)p1;
  dt_iop_crop_data_t *d = piece->data;

  if(dt_iop_has_focus(self) && (pipe->type & DT_DEV_PIXELPIPE_BASIC))
  {
    d->cx = 0.0f;
    d->cy = 0.0f;
    d->cw = 1.0f;
    d->ch = 1.0f;
    d->aspect = 0.0f;
  }
  else
  {
    d->cx = CLAMPF(p->cx, 0.0f, 1.f - MIN_CROP_SIZE);
    d->cy = CLAMPF(p->cy, 0.0f, 1.f - MIN_CROP_SIZE);
    d->cw = CLAMPF(p->cw, MIN_CROP_SIZE, 1.0f);
    d->ch = CLAMPF(p->ch, MIN_CROP_SIZE, 1.0f);

    const int rd = p->ratio_d;
    const int rn = p->ratio_n;

    d->aspect = 0.0f;           // freehand
    if(rn == 0 && abs(rd) == 1) // original image ratio
    {
      const float pratio = dt_image_get_sensor_ratio(&self->dev->image_storage);
      d->aspect = rd > 0 ? pratio : -pratio;
    }
    else if(rn == 0) { }
    else                        // defined ratio
      d->aspect = (float)rd / (float)rn;
  }
  d->ratio_n = p->ratio_n;
  d->ratio_d = p->ratio_d;
  dt_print(DT_DEBUG_PARAMS, "[crop] commit ratio_d=%d ratio_n=%d", d->ratio_d, d->ratio_n);
}


void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_crop_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


static void _aspect_apply(dt_iop_module_t *self, _grab_region_t grab)
{

  int piwd, piht;
  dt_dev_get_processed_size(&darktable.develop->full, &piwd, &piht);
  const double iwd = piwd;
  const double iht = piht;

  // enforce aspect ratio.
  double aspect = _aspect_ratio_get(self, g->aspect_presets);

  // since one rarely changes between portrait and landscape by cropping,
  // long side of the crop box should match the long side of the image.
  if(iwd < iht && aspect != 0.0)
    aspect = 1.0 / aspect;

  if(aspect > 0.0)
  {
    // if only one side changed, force aspect by two adjacent in equal parts
    // 1 2 4 8 : x y w h
    double clip_x = MAX(iwd * g->clip_x / iwd, 0.0f);
    double clip_y = MAX(iht * g->clip_y / iht, 0.0f);
    double clip_w = MIN(iwd * g->clip_w / iwd, 1.0f);
    double clip_h = MIN(iht * g->clip_h / iht, 1.0f);

    // if we only modified one dim, respectively, we wanted these values:
    const double target_h = iwd * g->clip_w / (iht * aspect);
    const double target_w = iht * g->clip_h * aspect / iwd;
    // i.e. target_w/h = w/target_h = aspect
    // first fix aspect ratio:

    // corners: move two adjacent
    if(grab == GRAB_TOP_LEFT)
    {
      // move x y
      clip_x = clip_x + clip_w - (target_w + clip_w) * .5;
      clip_y = clip_y + clip_h - (target_h + clip_h) * .5;
      clip_w = (target_w + clip_w) * .5;
      clip_h = (target_h + clip_h) * .5;
    }
    else if(grab == GRAB_TOP_RIGHT) // move y w
    {
      clip_y = clip_y + clip_h - (target_h + clip_h) * .5;
      clip_w = (target_w + clip_w) * .5;
      clip_h = (target_h + clip_h) * .5;
    }
    else if(grab == GRAB_BOTTOM_RIGHT) // move w h
    {
      clip_w = (target_w + clip_w) * .5;
      clip_h = (target_h + clip_h) * .5;
    }
    else if(grab == GRAB_BOTTOM_LEFT) // move h x
    {
      clip_h = (target_h + clip_h) * .5;
      clip_x = clip_x + clip_w - (target_w + clip_w) * .5;
      clip_w = (target_w + clip_w) * .5;
    }
    else if(grab & GRAB_HORIZONTAL) // dragged either x or w (1 4)
    {
      // change h and move y, h equally
      const double off = target_h - clip_h;
      clip_h = clip_h + off;
      clip_y = clip_y - .5 * off;
    }
    else if(grab & GRAB_VERTICAL) // dragged either y or h (2 8)
    {
      // change w and move x, w equally
      const double off = target_w - clip_w;
      clip_w = clip_w + off;
      clip_x = clip_x - .5 * off;
    }
    // now fix outside boxes:
    if(clip_x < g->clip_max_x)
    {
      const double prev_clip_h = clip_h;
      clip_h *= (clip_w + clip_x - g->clip_max_x) / clip_w;
      clip_w = clip_w + clip_x - g->clip_max_x;
      clip_x = g->clip_max_x;
      if(grab & GRAB_TOP) clip_y += prev_clip_h - clip_h;
    }
    if(clip_y < g->clip_max_y)
    {
      const double prev_clip_w = clip_w;
      clip_w *= (clip_h + clip_y - g->clip_max_y) / clip_h;
      clip_h = clip_h + clip_y - g->clip_max_y;
      clip_y = g->clip_max_y;
      if(grab & GRAB_LEFT) clip_x += prev_clip_w - clip_w;
    }
    if(clip_x + clip_w > g->clip_max_x + g->clip_max_w)
    {
      const double prev_clip_h = clip_h;
      clip_h *= (g->clip_max_x + g->clip_max_w - clip_x) / clip_w;
      clip_w = g->clip_max_x + g->clip_max_w - clip_x;
      if(grab & GRAB_TOP) clip_y += prev_clip_h - clip_h;
    }
    if(clip_y + clip_h > g->clip_max_y + g->clip_max_h)
    {
      const double prev_clip_w = clip_w;
      clip_w *= (g->clip_max_y + g->clip_max_h - clip_y) / clip_h;
      clip_h = g->clip_max_y + g->clip_max_h - clip_y;
      if(grab & GRAB_LEFT) clip_x += prev_clip_w - clip_w;
    }
  }
}

void reload_defaults(dt_iop_module_t *self)
{
  const dt_image_t *img = &self->dev->image_storage;

  dt_iop_crop_params_t *dp = self->default_params;

  dp->cx = img->usercrop[1];
  dp->cy = img->usercrop[0];
  dp->cw = img->usercrop[3];
  dp->ch = img->usercrop[2];
  dp->ratio_n = dp->ratio_d = -1;
}


static gint _aspect_ratio_cmp(const dt_iop_crop_aspect_t *a,
                              const dt_iop_crop_aspect_t *b)
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


static _grab_region_t _gui_get_grab(float pzx,
                                    float pzy,
                                    dt_iop_crop_gui_data_t *g,
                                    const float border,
                                    const float wd,
                                    const float ht)
{
  _grab_region_t grab = GRAB_NONE;
  if(!(pzx < g->clip_x
       || pzx > g->clip_x + g->clip_w
       || pzy < g->clip_y
       || pzy > g->clip_y + g->clip_h))
  {
    // we are inside the crop box
    grab = GRAB_CENTER;

    float h_border = border / wd;
    float v_border = border / ht;
    if(!(g->clip_x || g->clip_y || g->clip_w != 1.0f || g->clip_h != 1.0f))
      h_border = v_border = 0.45;

    if(pzx >= g->clip_x && pzx < g->clip_x + h_border && pzx - g->clip_x < 0.5 * g->clip_w)
      grab |= GRAB_LEFT; // left border
    else if(pzx <= g->clip_x + g->clip_w && pzx > (g->clip_w + g->clip_x) - h_border)
      grab |= GRAB_RIGHT; // right border

    if(pzy >= g->clip_y && pzy < g->clip_y + v_border && pzy - g->clip_y < 0.5 * g->clip_h)
      grab |= GRAB_TOP;  // top border
    else if(pzy <= g->clip_y + g->clip_h && pzy > (g->clip_h + g->clip_y) - v_border)
      grab |= GRAB_BOTTOM; // bottom border
  }
  return grab;
}

// draw guides and handles over the image


int button_released(dt_iop_module_t *self,
                    const float x,
                    const float y,
                    const int which,
                    const uint32_t state,
                    const float zoom_scale)
{
  dt_iop_crop_params_t *p = self->params;
  // we don't do anything if the image is not ready
  if(!g->preview_ready) return 0;

  /* reset internal ui states*/

  dt_control_change_cursor(GDK_LEFT_PTR);

  // we save the crop into the params now so params are kept in synch with gui settings
  _commit_box(self, g, p, FALSE);
  return 1;
}


GSList *mouse_actions(dt_iop_module_t *self)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG, 0,
                                     _("[%s on borders] crop"), self->name());
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG, GDK_SHIFT_MASK,
                                     _("[%s on borders] crop keeping ratio"), self->name());
  return lm;
}

// #undef PHI
// #undef INVPHI

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
