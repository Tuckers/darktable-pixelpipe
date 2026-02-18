/*
 * iop_math.h - Self-contained math and utility functions for libdtpipe IOPs
 *
 * Ported from:
 *   src/develop/imageop_math.h  (FC, FCxtrans, dt_iop_eval_exp, dt_iop_estimate_exp,
 *                                dt_iop_alpha_copy)
 *   src/common/imagebuf.h       (dt_iop_image_copy_by_size, dt_iop_copy_image_roi,
 *                                dt_iop_image_fill, dt_iop_have_required_input_format,
 *                                dt_iop_alloc_image_buffers)
 *   src/common/math.h           (dt_fast_expf, dt_log2f, fastlog2, fastlog,
 *                                mat3mulv, mat3mul, mat3SSEmul, scalar_product,
 *                                dot_product, sqf, sqrf, euclidean_norm, CLIP,
 *                                CLAMPF, interpolatef, feqf, deg2radf, dt_fast_hypotf,
 *                                dt_vector_*, max3f, min3f)
 *   src/common/dttypes.h        (max3f, min3f)
 *
 * Task 8.3: Port IOP Math and Utility Helpers
 *
 * Requirements:
 *   - Pure C, self-contained header (static inline where possible)
 *   - Includes only <math.h>, <string.h>, <stdlib.h>, <stdio.h>, <stdint.h>
 *   - Include "dtpipe_internal.h" for types
 *   - No GLib, no GTK, no OpenCL
 */

#pragma once

#include "dtpipe_internal.h"
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Scalar constants ─────────────────────────────────────────────────────── */

#ifndef M_LN10
#  define M_LN10 2.30258509299404568402
#endif
#ifndef M_PI
#  define M_PI   3.14159265358979323846
#endif
#ifndef M_PI_F
#  define M_PI_F 3.14159265358979323846f
#endif

#define DT_M_LN2f (0.6931471805599453f)

#define LUT_ELEM 512
#define NORM_MIN 1.52587890625e-05f  /* 2^(-16) */

/* select speed vs accuracy tradeoff for polynomial approximations */
#define EXP_POLY_DEGREE 4
#define LOG_POLY_DEGREE 5

/* ── Clip / clamp helpers ─────────────────────────────────────────────────── */

/* CLIP: clamp to [0,1], NaN-safe (NaN compares false → result is 0) */
#ifndef CLIP
#  define CLIP(x) (((x) >= 0) ? ((x) <= 1 ? (x) : 1) : 0)
#endif

/* CLAMPF: clamp to [mn,mx], NaN-safe */
#ifndef CLAMPF
#  define CLAMPF(a, mn, mx) ((a) >= (mn) ? ((a) <= (mx) ? (a) : (mx)) : (mn))
#endif

/* LCLIP: clamp luminance to [0,100] */
#ifndef LCLIP
#  define LCLIP(x) ((x) < 0 ? 0.0 : ((x) > 100.0 ? 100.0 : (x)))
#endif

/* ── Channel color constants ─────────────────────────────────────────────── */

#ifndef RED
#  define RED   0
#  define GREEN 1
#  define BLUE  2
#  define ALPHA 3
#endif

/* ── Bayer / X-Trans pattern helpers ─────────────────────────────────────── */

/**
 * FC(row, col, filters) — return Bayer color index (0=R, 1=G, 2=B, 3=G2)
 * for the given row/column position in a Bayer sensor.
 * filters encodes the 2×2 repeating pattern.
 */
static inline int FC(const size_t row, const size_t col, const uint32_t filters)
{
  return (filters >> (((row << 1 & 14) + (col & 1)) << 1)) & 3;
}

/**
 * FCxtrans — return X-Trans color index for the given row/column.
 * If roi is provided, the pixel's sensor position includes the ROI offset.
 * +600 offset ensures non-negative indices when roi->y/x can be negative.
 */
static inline int FCxtrans(const int row, const int col,
                           const dt_iop_roi_t *const roi,
                           const uint8_t (*const xtrans)[6])
{
  int irow = row + 600;
  int icol = col + 600;
  if(roi)
  {
    irow += roi->y;
    icol += roi->x;
  }
  return xtrans[irow % 6][icol % 6];
}

/**
 * FCNxtrans — X-Trans color index without ROI offset.
 */
static inline int FCNxtrans(const int row, const int col,
                            const uint8_t (*const xtrans)[6])
{
  return xtrans[(row + 600) % 6][(col + 600) % 6];
}

/**
 * fcol — unified Bayer/X-Trans lookup.
 * Uses X-Trans when filters == 9, Bayer otherwise.
 */
DT_OMP_DECLARE_SIMD()
static inline int fcol(const int row, const int col,
                       const uint32_t filters,
                       const uint8_t (*const xtrans)[6])
{
  if(filters == 9)
    return FCNxtrans(row, col, xtrans);
  else
    return FC(row, col, filters);
}

/* ── Exponential fit helpers ─────────────────────────────────────────────── */

/**
 * dt_iop_estimate_exp — fit power-law f(x) = coeff[1] * (x * coeff[0])^coeff[2]
 * to the given (x, y) sample pairs (ordered by ascending x).
 */
static inline void dt_iop_estimate_exp(const float *const x,
                                       const float *const y,
                                       const int num,
                                       float *coeff)
{
  const float x0 = x[num - 1], y0 = y[num - 1];
  float g = 0.0f;
  int cnt = 0;
  for(int k = 0; k < num - 1; k++)
  {
    const float yy = y[k] / y0, xx = x[k] / x0;
    if(yy > 0.0f && xx > 0.0f)
    {
      g += logf(yy) / logf(xx);
      cnt++;
    }
  }
  g = cnt ? g / (float)cnt : 1.0f;
  coeff[0] = 1.0f / x0;
  coeff[1] = y0;
  coeff[2] = g;
}

/**
 * dt_iop_eval_exp — evaluate the power-law fit: coeff[1] * (x * coeff[0])^coeff[2]
 */
DT_OMP_DECLARE_SIMD()
static inline float dt_iop_eval_exp(const float *const coeff, const float x)
{
  return coeff[1] * powf(x * coeff[0], coeff[2]);
}

/* ── Fast math approximations ─────────────────────────────────────────────── */

union dt_float_int { float f; int k; };

/**
 * dt_fast_expf — fast approximation of expf(), accurate for x in [-100, 0].
 */
DT_OMP_DECLARE_SIMD()
static inline float dt_fast_expf(const float x)
{
  const int i1 = 0x3f800000u;
  const int i2 = 0x402DF854u;
  const int k0 = i1 + (int)(x * (i2 - i1));
  union dt_float_int u;
  u.k = k0 > 0 ? k0 : 0;
  return u.f;
}

/**
 * dt_fast_mexp2f — fast approximation of 2^-x for 0 < x < 126.
 */
static inline float dt_fast_mexp2f(const float x)
{
  const int i1 = 0x3f800000;
  const int i2 = 0x3f000000;
  const int k0 = i1 + (int)(x * (i2 - i1));
  union dt_float_int k;
  k.k = k0 >= 0x800000 ? k0 : 0;
  return k.f;
}

/**
 * fast_mexp2f — legacy version (slightly less accurate), kept for compatibility.
 */
static inline float fast_mexp2f(const float x)
{
  const float i1 = (float)0x3f800000u;
  const float i2 = (float)0x3f000000u;
  const float k0 = i1 + x * (i2 - i1);
  union dt_float_int k;
  k.k = k0 >= (float)0x800000u ? (int)k0 : 0;
  return k.f;
}

/**
 * fastlog2 — fast log base-2 approximation.
 */
static inline float fastlog2(const float x)
{
  const union { float f; uint32_t i; } vx = { x };
  const union { uint32_t i; float f; } mx = { (vx.i & 0x007FFFFF) | 0x3f000000 };
  float y = (float)vx.i;
  y *= 1.1920928955078125e-7f;
  return y - 124.22551499f
           - 1.498030302f  * mx.f
           - 1.72587999f   / (0.3520887068f + mx.f);
}

/**
 * fastlog — fast natural log approximation.
 */
static inline float fastlog(const float x)
{
  return DT_M_LN2f * fastlog2(x);
}

/**
 * dt_log2f — portable log base-2 (uses glibc log2f when available).
 */
DT_OMP_DECLARE_SIMD()
static inline float dt_log2f(const float f)
{
  return logf(f) / DT_M_LN2f;
}

/**
 * Log2 / Log2Thres — convenience log2 with zero/threshold guard.
 */
static inline float Log2(const float x)
{
  return (x > 0.0f) ? (logf(x) / DT_M_LN2f) : x;
}

static inline float Log2Thres(const float x, const float Thres)
{
  return logf(x > Thres ? x : Thres) / DT_M_LN2f;
}

/* ── Scalar math helpers ─────────────────────────────────────────────────── */

DT_OMP_DECLARE_SIMD()
static inline float sqf(const float x)
{
  return x * x;
}

static inline float sqrf(const float a)
{
  return a * a;
}

/** ceil_fast — fast float ceil without libc dependency */
static inline float ceil_fast(const float x)
{
  if(x <= 0.f)
    return (float)(int)x;
  else
    return -((float)(int)-x) + 1.f;
}

/** interpolatef — linear blend: a*b + (1-a)*c */
static inline float interpolatef(const float a, const float b, const float c)
{
  return a * (b - c) + c;
}

/** feqf — float equality within epsilon */
static inline int feqf(const float v1, const float v2, const float eps)
{
  return (fabsf(v1 - v2) < eps);
}

/** angle conversion */
static inline float deg2radf(const float deg)  { return deg * M_PI_F / 180.f; }
static inline float rad2degf(const float r)    { return r / M_PI_F * 180.f; }
static inline double deg2rad(const double deg) { return deg * M_PI  / 180.0; }
static inline double rad2deg(const double r)   { return r  / M_PI   * 180.0; }

/** dt_fast_hypotf — fast hypot (assumes no overflow/NaN) */
DT_OMP_DECLARE_SIMD()
static inline float dt_fast_hypotf(const float x, const float y)
{
  return sqrtf(x * x + y * y);
}

/* ── Channel-array max/min helpers ───────────────────────────────────────── */

static inline float max3f(const float *array)
{
  return fmaxf(fmaxf(array[0], array[1]), array[2]);
}

static inline float min3f(const float *array)
{
  return fminf(fminf(array[0], array[1]), array[2]);
}

static inline float max4f(const float *array)
{
  return fmaxf(fmaxf(array[0], array[1]), fmaxf(array[2], array[3]));
}

/* ── Processed maximum helpers (used by rawprepare / demosaic) ───────────── */

static inline float dt_iop_get_processed_maximum(dt_dev_pixelpipe_iop_t *piece)
{
  return fmaxf(1.0f, max3f(piece->pipe->dsc.processed_maximum));
}

static inline float dt_iop_get_processed_minimum(dt_dev_pixelpipe_iop_t *piece)
{
  return fmaxf(1.0f, min3f(piece->pipe->dsc.processed_maximum));
}

/* ── Alpha channel copy ──────────────────────────────────────────────────── */

/**
 * dt_iop_alpha_copy — copy alpha channel 1:1 from input to output RGBA buffers.
 */
DT_OMP_DECLARE_SIMD(uniform(width, height) aligned(ivoid, ovoid:64))
static inline void dt_iop_alpha_copy(const void *const ivoid,
                                     void *const ovoid,
                                     const size_t width,
                                     const size_t height)
{
  const float *const restrict in  = (const float *)ivoid;
  float *const       restrict out = (float *)ovoid;
  DT_OMP_FOR()
  for(size_t k = 3; k < width * height * 4; k += 4)
    out[k] = in[k];
}

/* ── 3×3 matrix operations ───────────────────────────────────────────────── */

/**
 * mat3mulv — multiply 3×3 matrix (row-major flat) by 3-vector.
 * dest must differ from v.
 */
DT_OMP_DECLARE_SIMD()
static inline void mat3mulv(float *const restrict dest,
                            const float *const mat,
                            const float *const restrict v)
{
  for(int k = 0; k < 3; k++)
  {
    float x = 0.0f;
    for(int i = 0; i < 3; i++)
      x += mat[3 * k + i] * v[i];
    dest[k] = x;
  }
}

/**
 * mat3mul — multiply two 3×3 matrices (row-major flat).
 * dest = m1 * m2.  dest must differ from m1 and m2.
 */
DT_OMP_DECLARE_SIMD()
static inline void mat3mul(float *const restrict dest,
                           const float *const restrict m1,
                           const float *const restrict m2)
{
  for(int k = 0; k < 3; k++)
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++)
        x += m1[3 * k + j] * m2[3 * j + i];
      dest[3 * k + i] = x;
    }
}

/**
 * mat3SSEmul — multiply two padded (4×4) 3×3 matrices.
 * dest = m1 * m2.  dest must differ from m1 and m2.
 */
DT_OMP_DECLARE_SIMD()
static inline void mat3SSEmul(dt_colormatrix_t dest,
                              const dt_colormatrix_t m1,
                              const dt_colormatrix_t m2)
{
  for(int k = 0; k < 3; k++)
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++)
        x += m1[k][j] * m2[j][i];
      dest[k][i] = x;
    }
}

/**
 * mul_mat_vec_2 — multiply a 2×2 matrix by a 2-vector.
 */
DT_OMP_DECLARE_SIMD()
static inline void mul_mat_vec_2(const float *m, const float *p, float *o)
{
  o[0] = p[0] * m[0] + p[1] * m[1];
  o[1] = p[0] * m[2] + p[1] * m[3];
}

/* ── Aligned-pixel vector operations ─────────────────────────────────────── */

/**
 * scalar_product — 3-element dot product of two 4-element aligned pixels.
 */
DT_OMP_DECLARE_SIMD(uniform(v_2) aligned(v_1, v_2:16))
static inline float scalar_product(const dt_aligned_pixel_t v_1,
                                   const dt_aligned_pixel_t v_2)
{
  float acc = 0.f;
  DT_OMP_SIMD(aligned(v_1, v_2:16) reduction(+:acc))
  for(size_t c = 0; c < 3; c++)
    acc += v_1[c] * v_2[c];
  return acc;
}

/**
 * dot_product — apply 3×4 colormatrix M to 4-element pixel v_in → v_out.
 */
DT_OMP_DECLARE_SIMD(uniform(M) aligned(M:64) aligned(v_in, v_out:16))
static inline void dot_product(const dt_aligned_pixel_t v_in,
                               const dt_colormatrix_t M,
                               dt_aligned_pixel_t v_out)
{
  DT_OMP_SIMD(aligned(M:64) aligned(v_in, v_out:16))
  for(size_t i = 0; i < 3; ++i)
    v_out[i] = scalar_product(v_in, M[i]);
}

/**
 * euclidean_norm — length of a 3-element pixel vector (at least NORM_MIN).
 */
DT_OMP_DECLARE_SIMD(aligned(vector:16))
static inline float euclidean_norm(const dt_aligned_pixel_t vector)
{
  return fmaxf(sqrtf(sqf(vector[0]) + sqf(vector[1]) + sqf(vector[2])), NORM_MIN);
}

DT_OMP_DECLARE_SIMD(aligned(vector:16))
static inline void downscale_vector(dt_aligned_pixel_t vector, const float scaling)
{
  const int valid = (scaling > NORM_MIN);
  for(size_t c = 0; c < 3; c++)
    vector[c] = valid ? vector[c] / (scaling + NORM_MIN) : vector[c] / NORM_MIN;
}

DT_OMP_DECLARE_SIMD(aligned(vector:16))
static inline void upscale_vector(dt_aligned_pixel_t vector, const float scaling)
{
  const int valid = (scaling > NORM_MIN);
  for(size_t c = 0; c < 3; c++)
    vector[c] = valid ? vector[c] * (scaling + NORM_MIN) : vector[c] * NORM_MIN;
}

static inline void dt_vector_add(dt_aligned_pixel_t sum,
                                 const dt_aligned_pixel_t v1,
                                 const dt_aligned_pixel_t v2)
{
  for_four_channels(c, aligned(sum, v1, v2)) sum[c] = v1[c] + v2[c];
}

static inline void dt_vector_sub(dt_aligned_pixel_t diff,
                                 const dt_aligned_pixel_t v1,
                                 const dt_aligned_pixel_t v2)
{
  for_four_channels(c, aligned(diff, v1, v2)) diff[c] = v1[c] - v2[c];
}

static inline void dt_vector_mul(dt_aligned_pixel_t result,
                                 const dt_aligned_pixel_t v1,
                                 const dt_aligned_pixel_t v2)
{
  for_four_channels(c, aligned(result, v1, v2)) result[c] = v1[c] * v2[c];
}

static inline void dt_vector_mul1(dt_aligned_pixel_t result,
                                  const dt_aligned_pixel_t in,
                                  const float scale)
{
  for_four_channels(c, aligned(result, in)) result[c] = in[c] * scale;
}

static inline void dt_vector_div(dt_aligned_pixel_t result,
                                 const dt_aligned_pixel_t v1,
                                 const dt_aligned_pixel_t v2)
{
  for_four_channels(c, aligned(result, v1, v2)) result[c] = v1[c] / v2[c];
}

static inline void dt_vector_div1(dt_aligned_pixel_t result,
                                  const dt_aligned_pixel_t in,
                                  const float divisor)
{
  for_four_channels(c, aligned(result, in)) result[c] = in[c] / divisor;
}

static inline void dt_vector_clip(dt_aligned_pixel_t values)
{
  static const dt_aligned_pixel_t zero = { 0.0f, 0.0f, 0.0f, 0.0f };
  static const dt_aligned_pixel_t one  = { 1.0f, 1.0f, 1.0f, 1.0f };
  for_four_channels(c, aligned(values))
  {
    values[c] = values[c] > zero[c] ? values[c] : zero[c];
    values[c] = values[c] < one[c]  ? values[c] : one[c];
  }
}

static inline void dt_vector_clipneg(dt_aligned_pixel_t values)
{
  for_four_channels(c, aligned(values))
    values[c] = values[c] > 0.0f ? values[c] : 0.0f;
}

static inline float dt_vector_channel_max(const dt_aligned_pixel_t pixel)
{
  return fmaxf(fmaxf(pixel[0], pixel[1]), pixel[2]);
}

/* ── Kahan summation ─────────────────────────────────────────────────────── */

DT_OMP_DECLARE_SIMD()
static inline float Kahan_sum(const float m, float *const restrict c, const float add)
{
  const float t1 = add - (*c);
  const float t2 = m + t1;
  *c = (t2 - m) - t1;
  return t2;
}

/* ── Scharr gradient ─────────────────────────────────────────────────────── */

static inline float scharr_gradient(const float *p, const int w)
{
  const float gx = 47.0f / 255.0f * (p[-w-1] - p[-w+1] + p[w-1] - p[w+1])
                 + 162.0f / 255.0f * (p[-1] - p[1]);
  const float gy = 47.0f / 255.0f * (p[-w-1] - p[w-1] + p[-w+1] - p[w+1])
                 + 162.0f / 255.0f * (p[-w] - p[w]);
  return sqrtf(sqrf(gx) + sqrf(gy));
}

/* ── Image buffer helpers ─────────────────────────────────────────────────── */

/* dt_iop_image_copy and dt_iop_image_copy_by_size are already defined in
 * dtpipe_internal.h.  No redefinition here — just a reminder comment. */

/**
 * dt_iop_copy_image_roi — copy from src with roi_in into dst at roi_out.
 * If roi_out is larger than roi_in, the extra area is zero-filled.
 * If roi_out is smaller, only the overlapping region is copied.
 */
static inline void dt_iop_copy_image_roi(float *const restrict out,
                                         const float *const restrict in,
                                         const size_t ch,
                                         const dt_iop_roi_t *const restrict roi_in,
                                         const dt_iop_roi_t *const restrict roi_out)
{
  /* Fast path: same size and position */
  if(roi_in->width  == roi_out->width  &&
     roi_in->height == roi_out->height &&
     roi_in->x      == roi_out->x      &&
     roi_in->y      == roi_out->y)
  {
    memcpy(out, in, (size_t)roi_out->width * roi_out->height * ch * sizeof(float));
    return;
  }

  const int copy_w = roi_out->width  < roi_in->width  ? roi_out->width  : roi_in->width;
  const int copy_h = roi_out->height < roi_in->height ? roi_out->height : roi_in->height;

  if(roi_out->width != copy_w || roi_out->height != copy_h)
    memset(out, 0, (size_t)roi_out->width * roi_out->height * ch * sizeof(float));

  for(int j = 0; j < copy_h; j++)
  {
    memcpy(out + (size_t)j * roi_out->width * ch,
           in  + (size_t)j * roi_in->width  * ch,
           (size_t)copy_w * ch * sizeof(float));
  }
}

/**
 * dt_iop_image_fill — fill width*height*ch floats with fill_value.
 */
static inline void dt_iop_image_fill(float *const buf,
                                     const float fill_value,
                                     const size_t width,
                                     const size_t height,
                                     const size_t ch)
{
  const size_t n = width * height * ch;
  DT_OMP_FOR()
  for(size_t i = 0; i < n; i++)
    buf[i] = fill_value;
}

/**
 * dt_iop_image_add_const — add add_value to every element.
 */
static inline void dt_iop_image_add_const(float *const buf,
                                          const float add_value,
                                          const size_t width,
                                          const size_t height,
                                          const size_t ch)
{
  const size_t n = width * height * ch;
  DT_OMP_FOR()
  for(size_t i = 0; i < n; i++)
    buf[i] += add_value;
}

/**
 * dt_iop_image_mul_const — multiply every element by mul_value.
 */
static inline void dt_iop_image_mul_const(float *const buf,
                                          const float mul_value,
                                          const size_t width,
                                          const size_t height,
                                          const size_t ch)
{
  const size_t n = width * height * ch;
  DT_OMP_FOR()
  for(size_t i = 0; i < n; i++)
    buf[i] *= mul_value;
}

/**
 * dt_iop_image_alloc — allocate aligned float buffer for width*height*ch pixels.
 * Caller must free with dt_free_align().
 */
static inline float *dt_iop_image_alloc(const size_t width,
                                        const size_t height,
                                        const size_t ch)
{
  return dt_alloc_align_float(width * height * ch);
}

/* ── DT_IMGSZ flags for dt_iop_alloc_image_buffers ─────────────────────── */

#define DT_IMGSZ_CH_MASK    0x000FFFF
#define DT_IMGSZ_ROI_MASK   0x0100000
#define DT_IMGSZ_OUTPUT     0x0000000
#define DT_IMGSZ_INPUT      0x0100000
#define DT_IMGSZ_PERTHREAD  0x0200000
#define DT_IMGSZ_CLEARBUF   0x0400000
#define DT_IMGSZ_DIM_MASK   0x00F0000
#define DT_IMGSZ_FULL       0x0000000
#define DT_IMGSZ_HEIGHT     0x0010000
#define DT_IMGSZ_WIDTH      0x0020000
#define DT_IMGSZ_LONGEST    0x0030000

/**
 * dt_iop_alloc_image_buffers — allocate one or more image buffers.
 *
 * Variable arguments: SIZE, PTR-to-floatPTR [, PTR-to-size_t], ...  sentinel: 0
 *
 * SIZE low bits are number of channels per pixel; high bits are DT_IMGSZ_* flags.
 * On failure: frees all already-allocated buffers, sets module trouble, returns FALSE.
 *
 * Simplified version: DT_IMGSZ_PERTHREAD is supported (padded_size pointer expected).
 * DT_IMGSZ_CLEARBUF zeroes the allocation.
 */
static inline int dt_iop_alloc_image_buffers(dt_iop_module_t *const module,
                                             const dt_iop_roi_t *const roi_in,
                                             const dt_iop_roi_t *const roi_out,
                                             ...)
{
  /* Collect all allocations first so we can free them all on failure */
  float **ptrs[32];
  size_t  sizes[32];
  int     n = 0;
  int     ok = 1;

  va_list ap;
  va_start(ap, roi_out);

  while(ok)
  {
    const unsigned flags = va_arg(ap, unsigned);
    if(flags == 0) break;

    float **ptr = va_arg(ap, float **);
    size_t *padded = NULL;
    if(flags & DT_IMGSZ_PERTHREAD)
      padded = va_arg(ap, size_t *);

    const unsigned ch  = flags & DT_IMGSZ_CH_MASK;
    const int use_in   = (flags & DT_IMGSZ_ROI_MASK) == DT_IMGSZ_INPUT;
    const dt_iop_roi_t *roi = use_in ? roi_in : roi_out;

    size_t w = (size_t)roi->width;
    size_t h = (size_t)roi->height;
    const unsigned dim = flags & DT_IMGSZ_DIM_MASK;
    if(dim == DT_IMGSZ_HEIGHT)       { w = 1; }
    else if(dim == DT_IMGSZ_WIDTH)   { h = 1; }
    else if(dim == DT_IMGSZ_LONGEST) { w = 1; h = w > h ? w : h; }

    const int zeroed = (flags & DT_IMGSZ_CLEARBUF) != 0;
    float *buf;

    if(flags & DT_IMGSZ_PERTHREAD)
    {
      size_t ps = 0;
      buf = dt_alloc_perthread_float(w * h * ch, &ps);
      if(padded) *padded = ps;
      if(zeroed && buf)
        memset(buf, 0, ps * (size_t)dt_get_num_threads() * sizeof(float));
    }
    else
    {
      buf = zeroed ? dt_calloc_align_float(w * h * ch)
                   : dt_alloc_align_float(w * h * ch);
    }

    *ptr = buf;
    if(!buf) { ok = 0; break; }

    if(n < 32) { ptrs[n] = ptr; sizes[n] = 0; n++; }
  }
  va_end(ap);

  if(!ok)
  {
    for(int i = 0; i < n; i++)
    {
      dt_free_align(*ptrs[i]);
      *ptrs[i] = NULL;
    }
    if(module)
    {
      fprintf(stderr, "dtpipe [%s]: dt_iop_alloc_image_buffers: out of memory\n",
              module->op);
    }
    return 0; /* FALSE */
  }
  (void)sizes;
  return 1; /* TRUE */
}

/**
 * dt_iop_have_required_input_format — validate input channel count.
 *
 * If the pipe's actual channel count != required_ch, copy input to output
 * and return FALSE (0) so the caller can skip processing.
 * Logs a warning to stderr.
 */
static inline int dt_iop_have_required_input_format(const int required_ch,
                                                    dt_iop_module_t *const module,
                                                    const int actual_pipe_ch,
                                                    const void *const restrict ivoid,
                                                    void *const restrict ovoid,
                                                    const dt_iop_roi_t *const roi_in,
                                                    const dt_iop_roi_t *const roi_out)
{
  if(actual_pipe_ch == required_ch)
    return 1; /* TRUE: have correct format */

  /* Wrong format: copy input → output unchanged and warn */
  if(ovoid && ivoid)
    memcpy(ovoid, ivoid,
           (size_t)roi_out->width * roi_out->height *
           (size_t)actual_pipe_ch * sizeof(float));

  if(module)
  {
    fprintf(stderr,
            "dtpipe [%s]: dt_iop_have_required_input_format: "
            "expected %d channels, got %d — skipping\n",
            module->op, required_ch, actual_pipe_ch);
  }
  return 0; /* FALSE */
}

/* ── Clip-and-zoom for raw Bayer buffers ──────────────────────────────────── */

/**
 * dt_iop_clip_and_zoom_roi — crop/zoom float-RGBA input to output according to ROIs.
 * Uses bilinear interpolation.  Wrapper around dt_iop_clip_and_zoom (already in
 * dtpipe_internal.h) with the argument name matching the original signature.
 */
static inline void dt_iop_clip_and_zoom_roi(float *out, const float *const in,
                                            const dt_iop_roi_t *const roi_out,
                                            const dt_iop_roi_t *const roi_in)
{
  /* Delegate to the implementation already in dtpipe_internal.h */
  dt_iop_clip_and_zoom(out, in, roi_out, roi_in);
}

/**
 * dt_iop_clip_and_zoom_mosaic_half_size_f — downsample float Bayer by factor 2.
 * Takes every other 2x2 block and averages the two green pixels.
 * Used by demosaic at scale < 1 to avoid the full demosaic pass.
 */
static inline void
dt_iop_clip_and_zoom_mosaic_half_size_f(float *const out,
                                        const float *const in,
                                        const dt_iop_roi_t *const roi_out,
                                        const dt_iop_roi_t *const roi_in,
                                        const int32_t out_stride,
                                        const int32_t in_stride,
                                        const uint32_t filters)
{
  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    const int y0 = (int)((j + roi_out->y) * 2 - roi_in->y);
    for(int i = 0; i < roi_out->width; i++)
    {
      const int x0 = (int)((i + roi_out->x) * 2 - roi_in->x);
      /* clamp to valid sensor range */
      const int iy0 = CLAMPS(y0,     0, roi_in->height - 1);
      const int iy1 = CLAMPS(y0 + 1, 0, roi_in->height - 1);
      const int ix0 = CLAMPS(x0,     0, roi_in->width  - 1);
      const int ix1 = CLAMPS(x0 + 1, 0, roi_in->width  - 1);

      float col[4] = { 0.f, 0.f, 0.f, 0.f };
      float cnt[4] = { 0.f, 0.f, 0.f, 0.f };

      const float *p00 = in + (size_t)iy0 * in_stride + ix0;
      const float *p01 = in + (size_t)iy0 * in_stride + ix1;
      const float *p10 = in + (size_t)iy1 * in_stride + ix0;
      const float *p11 = in + (size_t)iy1 * in_stride + ix1;

      int c;
      c = FC(iy0, ix0, filters); col[c] += p00[0]; cnt[c] += 1.f;
      c = FC(iy0, ix1, filters); col[c] += p01[0]; cnt[c] += 1.f;
      c = FC(iy1, ix0, filters); col[c] += p10[0]; cnt[c] += 1.f;
      c = FC(iy1, ix1, filters); col[c] += p11[0]; cnt[c] += 1.f;

      float *o = out + (size_t)j * out_stride + i;
      /* Store a single-channel value (average of the Bayer block) */
      float sum = 0.f, n = 0.f;
      for(int k = 0; k < 4; k++) { if(cnt[k] > 0.f) { sum += col[k]; n += cnt[k]; } }
      o[0] = n > 0.f ? sum / n : 0.f;
    }
  }
}

/**
 * dt_iop_clip_and_zoom_demosaic_half_size_f — downscale single-channel float
 * Bayer buffer to half-size 4-channel RGBA output.
 * Each output pixel covers a 2x2 Bayer block: RGGB → RGBA.
 */
static inline void
dt_iop_clip_and_zoom_demosaic_half_size_f(float *out,
                                          const float *const in,
                                          const dt_iop_roi_t *const roi_out,
                                          const dt_iop_roi_t *const roi_in,
                                          const int32_t out_stride,
                                          const int32_t in_stride,
                                          const uint32_t filters)
{
  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    const int y0 = (int)((j + roi_out->y) * 2 - roi_in->y);
    for(int i = 0; i < roi_out->width; i++)
    {
      const int x0 = (int)((i + roi_out->x) * 2 - roi_in->x);
      const int iy0 = CLAMPS(y0,     0, roi_in->height - 1);
      const int iy1 = CLAMPS(y0 + 1, 0, roi_in->height - 1);
      const int ix0 = CLAMPS(x0,     0, roi_in->width  - 1);
      const int ix1 = CLAMPS(x0 + 1, 0, roi_in->width  - 1);

      float col[4] = { 0.f, 0.f, 0.f, 0.f };
      float cnt[4] = { 0.f, 0.f, 0.f, 0.f };

      const float *p00 = in + (size_t)iy0 * in_stride + ix0;
      const float *p01 = in + (size_t)iy0 * in_stride + ix1;
      const float *p10 = in + (size_t)iy1 * in_stride + ix0;
      const float *p11 = in + (size_t)iy1 * in_stride + ix1;

      int c;
      c = FC(iy0, ix0, filters); col[c] += p00[0]; cnt[c] += 1.f;
      c = FC(iy0, ix1, filters); col[c] += p01[0]; cnt[c] += 1.f;
      c = FC(iy1, ix0, filters); col[c] += p10[0]; cnt[c] += 1.f;
      c = FC(iy1, ix1, filters); col[c] += p11[0]; cnt[c] += 1.f;

      float *o = out + (size_t)j * out_stride * 4 + i * 4;
      for(int k = 0; k < 4; k++)
        o[k] = cnt[k] > 0.f ? col[k] / cnt[k] : 0.f;
    }
  }
}

/**
 * dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f — passthrough for
 * monochrome (single-channel greyscale) sensors: copy-average to RGBA.
 */
static inline void
dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f(
  float *out,
  const float *const in,
  const dt_iop_roi_t *const roi_out,
  const dt_iop_roi_t *const roi_in,
  const int32_t out_stride,
  const int32_t in_stride)
{
  const float scalex = (float)roi_in->width  / (float)roi_out->width;
  const float scaley = (float)roi_in->height / (float)roi_out->height;

  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    const int iy = CLAMPS((int)(j * scaley), 0, roi_in->height - 1);
    for(int i = 0; i < roi_out->width; i++)
    {
      const int ix = CLAMPS((int)(i * scalex), 0, roi_in->width - 1);
      const float v = in[(size_t)iy * in_stride + ix];
      float *o = out + ((size_t)j * out_stride + i) * 4;
      o[0] = o[1] = o[2] = v;
      o[3] = 0.f;
    }
  }
}

/* ── Tiling stub ─────────────────────────────────────────────────────────── */

/**
 * default_tiling_callback — already in dtpipe_internal.h as
 * dt_iop_default_tiling_callback().  Provide a plain alias so IOP
 * source files that call default_tiling_callback() by that name compile.
 */
static inline void default_tiling_callback(dt_iop_module_t *self,
                                           dt_dev_pixelpipe_iop_t *piece,
                                           const dt_iop_roi_t *roi_in,
                                           const dt_iop_roi_t *roi_out,
                                           dt_develop_tiling_t *tiling)
{
  dt_iop_default_tiling_callback(self, piece, roi_in, roi_out, tiling);
}

/* ── simd_memcpy alias ───────────────────────────────────────────────────── */

/**
 * dt_simd_memcpy — parallel vectorized memcpy on aligned contiguous buffers.
 */
static inline void dt_simd_memcpy(const float *const restrict in,
                                  float *const restrict out,
                                  const size_t num_elem)
{
  DT_OMP_FOR_SIMD(aligned(in, out:64))
  for(size_t k = 0; k < num_elem; k++)
    out[k] = in[k];
}

/* ── isnan/isinf wrappers (finite-math-safe) ─────────────────────────────── */

static inline int dt_isnan(const float val)    { return isnan(val); }
static inline int dt_isinf(const float val)    { return isinf(val); }
static inline int dt_isfinite(const float val) { return isfinite(val); }
static inline int dt_isnormal(const float val) { return isnormal(val); }
