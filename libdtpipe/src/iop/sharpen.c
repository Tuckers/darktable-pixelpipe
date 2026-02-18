/*
 * sharpen.c - Unsharp Mask (USM) sharpening IOP for libdtpipe
 *
 * Extracted from darktable src/iop/sharpen.c
 * Copyright (C) 2009-2023 darktable developers.
 * Adapted for libdtpipe: GUI, OpenCL, and preset code removed.
 * All internal functions are static (Phase 8 convention for single dylib).
 *
 * Operates in IOP_CS_LAB — applies unsharp mask to the L channel only.
 */

#include "dtpipe_internal.h"
#include "iop/iop_math.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAXR 12

/* ── Parameter structs (must match params.c descriptor layout) ───────────── */

typedef struct dt_iop_sharpen_params_t
{
  float radius;    /* $MIN: 0.0 $MAX: 99.0 $DEFAULT: 2.0 */
  float amount;    /* $MIN: 0.0 $MAX:  2.0 $DEFAULT: 0.5 */
  float threshold; /* $MIN: 0.0 $MAX:100.0 $DEFAULT: 0.5 */
} dt_iop_sharpen_params_t;

typedef struct dt_iop_sharpen_data_t
{
  float radius, amount, threshold;
} dt_iop_sharpen_data_t;

/* ── Gaussian kernel builder ─────────────────────────────────────────────── */

static float *init_gaussian_kernel(const int rad, const size_t mat_size,
                                    const float sigma2)
{
  float *const mat = dt_calloc_align_float(mat_size);
  if(mat)
  {
    float weight = 0.0f;
    for(int l = -rad; l <= rad; l++)
      weight += mat[l + rad] = expf(-l * l / (2.f * sigma2));
    for(int l = -rad; l <= rad; l++)
      mat[l + rad] /= weight;
  }
  return mat;
}

/* ── process() ───────────────────────────────────────────────────────────── */

static void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                    const void *const ivoid, void *const ovoid,
                    const dt_iop_roi_t *const roi_in,
                    const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*full-color pixels*/, self,
                                         piece->colors, ivoid, ovoid,
                                         roi_in, roi_out))
    return;

  const dt_iop_sharpen_data_t *const data = piece->data;
  const int rad = MIN(MAXR,
      (int)ceilf(data->radius * roi_in->scale / piece->iscale));

  /* Very small images or zero radius: pass through unchanged */
  if(rad == 0 ||
     (roi_out->width < 2 * rad + 1 || roi_out->height < 2 * rad + 1))
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, 4);
    return;
  }

  float *restrict tmp;   /* one row per thread */
  size_t padded_size;
  if(!dt_iop_alloc_image_buffers(self, roi_in, roi_out,
                                  1 | DT_IMGSZ_WIDTH | DT_IMGSZ_PERTHREAD,
                                  &tmp, &padded_size, 0))
  {
    dt_iop_copy_image_roi(ovoid, ivoid, 4, roi_in, roi_out);
    return;
  }

  const int wd  = 2 * rad + 1;
  const int wd4 = (wd & 3) ? (wd >> 2) + 1 : wd >> 2;
  const size_t mat_size = (size_t)4 * wd4;
  const float sigma2 =
      (1.0f / (2.5f * 2.5f))
      * (data->radius * roi_in->scale / piece->iscale)
      * (data->radius * roi_in->scale / piece->iscale);

  float *const mat = init_gaussian_kernel(rad, mat_size, sigma2);
  if(!mat)
  {
    fprintf(stderr, "[sharpen] out of memory\n");
    dt_iop_copy_image_roi(ovoid, ivoid, 4, roi_in, roi_out);
    dt_free_align(tmp);
    return;
  }

  const float *const restrict in = (const float *)ivoid;
  const size_t width = roi_out->width;

  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    /* Top/bottom border rows: copy unchanged */
    if(j < rad || j >= roi_out->height - rad)
    {
      const float *const restrict row_in  = in + (size_t)4 * j * width;
      float *const restrict row_out =
          ((float *)ovoid) + (size_t)4 * j * width;
      memcpy(row_out, row_in, 4 * sizeof(float) * width);
      continue;
    }

    float *const restrict temp_buf = dt_get_perthread(tmp, padded_size);

    /* Vertical blur into temp_buf */
    const size_t start_row = j - rad;
    const size_t end_row   = j + rad;
    const size_t end_bulk  = width & ~(size_t)3;

    for(size_t i = 0; i < end_bulk; i += 4)
    {
      dt_aligned_pixel_t sum = { 0.0f };
      for(size_t k = start_row; k <= end_row; k++)
      {
        const size_t k_adj = k - start_row;
        for_four_channels(c, aligned(in))
          sum[c] += mat[k_adj] * in[4 * (k * width + i + c)];
      }
      float *const vblurred = temp_buf + i;
      for_four_channels(c, aligned(vblurred))
        vblurred[c] = sum[c];
    }
    for(size_t i = end_bulk; i < width; i++)
    {
      float sum = 0.0f;
      for(size_t k = start_row; k <= end_row; k++)
        sum += mat[k - start_row] * in[4 * (k * width + i)];
      temp_buf[i] = sum;
    }

    /* Horizontal blur + USM mix → output */
    float *const restrict row_out =
        ((float *)ovoid) + (size_t)4 * j * width;

    /* Copy left border pixels unchanged */
    for(int i = 0; i < rad; i++)
      copy_pixel(row_out + 4 * i, in + 4 * (j * width + i));

    const float threshold = data->threshold;
    const float amount    = data->amount;

    for(int i = rad; i < roi_out->width - rad; i++)
    {
      float sum = 0.0f;
      for(int k = i - rad; k <= i + rad; k++)
        sum += mat[k - (i - rad)] * temp_buf[k];

      const size_t index = 4 * (j * width + i);
      const float  diff  = in[index] - sum;
      const float  absd  = fabsf(diff);
      const float  detail =
          (absd > threshold) ? copysignf(MAX(absd - threshold, 0.0f), diff)
                             : 0.0f;
      row_out[4 * i]     = in[index] + detail * amount;
      row_out[4 * i + 1] = in[index + 1];
      row_out[4 * i + 2] = in[index + 2];
    }

    /* Copy right border pixels unchanged */
    for(int i = roi_out->width - rad; i < roi_out->width; i++)
      copy_pixel(row_out + 4 * i, in + 4 * (j * width + i));
  }

  dt_free_align(mat);
  dt_free_align(tmp);
}

/* ── commit_params() ─────────────────────────────────────────────────────── */

static void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1,
                           dt_dev_pixelpipe_t *pipe,
                           dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_sharpen_params_t *p = (const dt_iop_sharpen_params_t *)p1;
  dt_iop_sharpen_data_t *d = piece->data;

  /* Scale radius to 2.5 sigma so the kernel captures the full Gaussian */
  d->radius    = 2.5f * p->radius;
  d->amount    = p->amount;
  d->threshold = p->threshold;
}

/* ── init_pipe() / cleanup_pipe() ────────────────────────────────────────── */

static void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                       dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_sharpen_data_t));
}

static void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

/* ── init() — default params ─────────────────────────────────────────────── */

static void init(dt_iop_module_t *self)
{
  dt_iop_sharpen_params_t *d = self->default_params;
  if(!d) return;
  d->radius    = 2.0f;
  d->amount    = 0.5f;
  d->threshold = 0.5f;
  memcpy(self->params, d, sizeof(*d));
}

/* ── colorspace declarations ─────────────────────────────────────────────── */

static dt_iop_colorspace_type_t input_colorspace(dt_iop_module_t *self,
                                                   dt_dev_pixelpipe_t *pipe,
                                                   dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

static dt_iop_colorspace_type_t output_colorspace(dt_iop_module_t *self,
                                                    dt_dev_pixelpipe_t *pipe,
                                                    dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

/* ── Public init_global entry point ──────────────────────────────────────── */

void dt_iop_sharpen_init_global(dt_iop_module_so_t *so)
{
  so->process_plain      = process;
  so->init               = init;
  so->init_pipe          = init_pipe;
  so->cleanup_pipe       = cleanup_pipe;
  so->commit_params      = commit_params;
  so->input_colorspace   = input_colorspace;
  so->output_colorspace  = output_colorspace;
}

#undef MAXR
