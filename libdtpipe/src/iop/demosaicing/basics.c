/*
 * demosaicing/basics.c — pre-median filter, color smoothing, green
 * equilibration functions for libdtpipe demosaic IOP.
 *
 * Extracted from darktable src/iop/demosaicing/basics.c (GPLv3).
 * OpenCL code removed.  Textually #include'd by demosaic.c.
 *
 * Copyright (C) 2010-2025 darktable developers (GPLv3)
 */

#define SWAP(a, b)                                                             \
  {                                                                            \
    const float tmp = (b);                                                     \
    (b) = (a);                                                                 \
    (a) = tmp;                                                                 \
  }

DT_OMP_DECLARE_SIMD(aligned(in, out))
static void pre_median_b(float *out,
                         const float *const in,
                         const int width,
                         const int height,
                         const uint32_t filters,
                         const int num_passes,
                         const float threshold)
{
  dt_iop_image_copy_by_size(out, in, width, height, 1);

  // green:
  const int lim[5] = { 0, 1, 2, 1, 0 };
  for(int pass = 0; pass < num_passes; pass++)
  {
    DT_OMP_FOR()
    for(int row = 3; row < height - 3; row++)
    {
      float med[9];
      int col = 3;
      if(FC(row, col, filters) != 1 && FC(row, col, filters) != 3) col++;
      float *pixo = out + (size_t)width * row + col;
      const float *pixi = in + (size_t)width * row + col;
      for(; col < width - 3; col += 2)
      {
        int cnt = 0;
        for(int k = 0, i = 0; i < 5; i++)
        {
          for(int j = -lim[i]; j <= lim[i]; j += 2)
          {
            if(fabsf(pixi[width * (i - 2) + j] - pixi[0]) < threshold)
            {
              med[k++] = pixi[width * (i - 2) + j];
              cnt++;
            }
            else
              med[k++] = 64.0f + pixi[width * (i - 2) + j];
          }
        }
        for(int i = 0; i < 8; i++)
          for(int ii = i + 1; ii < 9; ii++)
            if(med[i] > med[ii]) SWAP(med[i], med[ii]);
        pixo[0] = fmaxf(0.0f, (cnt == 1 ? med[4] - 64.0f : med[(cnt - 1) / 2]));
        pixo += 2;
        pixi += 2;
      }
    }
  }
}

static void pre_median(float *out,
                       const float *const in,
                       const int width,
                       const int height,
                       const uint32_t filters,
                       const int num_passes,
                       const float threshold)
{
  pre_median_b(out, in, width, height, filters, num_passes, threshold);
}

#define SWAPmed(I, J)                                                          \
  if(med[I] > med[J]) SWAP(med[I], med[J])

static void color_smoothing(float *out,
                            const int width,
                            const int height,
                            const int num_passes)
{
  const int width4 = 4 * width;
  for(int pass = 0; pass < num_passes; pass++)
  {
    for(int c = 0; c < 3; c += 2)
    {
      {
        float *outp = out;
        for(int j = 0; j < height; j++)
          for(int i = 0; i < width; i++, outp += 4) outp[3] = outp[c];
      }
      DT_OMP_FOR()
      for(int j = 1; j < height - 1; j++)
      {
        float *outp = out + (size_t)4 * j * width + 4;
        for(int i = 1; i < width - 1; i++, outp += 4)
        {
          float med[9] = { outp[-width4 - 4 + 3] - outp[-width4 - 4 + 1],
                           outp[-width4 + 0 + 3] - outp[-width4 + 0 + 1],
                           outp[-width4 + 4 + 3] - outp[-width4 + 4 + 1],
                           outp[-4 + 3]           - outp[-4 + 1],
                           outp[+0 + 3]           - outp[+0 + 1],
                           outp[+4 + 3]           - outp[+4 + 1],
                           outp[+width4 - 4 + 3]  - outp[+width4 - 4 + 1],
                           outp[+width4 + 0 + 3]  - outp[+width4 + 0 + 1],
                           outp[+width4 + 4 + 3]  - outp[+width4 + 4 + 1] };
          SWAPmed(1, 2); SWAPmed(4, 5); SWAPmed(7, 8);
          SWAPmed(0, 1); SWAPmed(3, 4); SWAPmed(6, 7);
          SWAPmed(1, 2); SWAPmed(4, 5); SWAPmed(7, 8);
          SWAPmed(0, 3); SWAPmed(5, 8); SWAPmed(4, 7);
          SWAPmed(3, 6); SWAPmed(1, 4); SWAPmed(2, 5);
          SWAPmed(4, 7); SWAPmed(4, 2); SWAPmed(6, 4);
          SWAPmed(4, 2);
          outp[c] = fmaxf(med[4] + outp[1], 0.0f);
        }
      }
    }
  }
}
#undef SWAP

static void green_equilibration_lavg(float *out,
                                     const float *const in,
                                     const int width,
                                     const int height,
                                     const uint32_t filters,
                                     const float thr)
{
  const float maximum = 1.0f;

  int oj = 2, oi = 2;
  if(FC(oj, oi, filters) != 1) oj++;
  if(FC(oj, oi, filters) != 1) oi++;
  if(FC(oj, oi, filters) != 1) oj--;

  dt_iop_image_copy_by_size(out, in, width, height, 1);

  DT_OMP_FOR(collapse(2))
  for(size_t j = oj; j < (size_t)(height - 2); j += 2)
  {
    for(size_t i = oi; i < (size_t)(width - 2); i += 2)
    {
      const float o1_1 = in[(j - 1) * width + i - 1];
      const float o1_2 = in[(j - 1) * width + i + 1];
      const float o1_3 = in[(j + 1) * width + i - 1];
      const float o1_4 = in[(j + 1) * width + i + 1];
      const float o2_1 = in[(j - 2) * width + i];
      const float o2_2 = in[(j + 2) * width + i];
      const float o2_3 = in[j * width + i - 2];
      const float o2_4 = in[j * width + i + 2];

      const float m1 = (o1_1 + o1_2 + o1_3 + o1_4) / 4.0f;
      const float m2 = (o2_1 + o2_2 + o2_3 + o2_4) / 4.0f;

      if((m2 > 0.0f) && (m1 > 0.0f) && (m1 / m2 < maximum * 2.0f))
      {
        const float c1 = (fabsf(o1_1 - o1_2) + fabsf(o1_1 - o1_3) + fabsf(o1_1 - o1_4)
                        + fabsf(o1_2 - o1_3) + fabsf(o1_3 - o1_4) + fabsf(o1_2 - o1_4)) / 6.0f;
        const float c2 = (fabsf(o2_1 - o2_2) + fabsf(o2_1 - o2_3) + fabsf(o2_1 - o2_4)
                        + fabsf(o2_2 - o2_3) + fabsf(o2_3 - o2_4) + fabsf(o2_2 - o2_4)) / 6.0f;
        if((in[j * width + i] < maximum * 0.95f) && (c1 < maximum * thr) && (c2 < maximum * thr))
          out[j * width + i] = fmaxf(0.0f, in[j * width + i] * m1 / m2);
      }
    }
  }
}

static void green_equilibration_favg(float *out,
                                     const float *const in,
                                     const int width,
                                     const int height,
                                     const uint32_t filters)
{
  int oj = 0, oi = 0;
  double sum1 = 0.0, sum2 = 0.0, gr_ratio;

  if((FC(oj, oi, filters) & 1) != 1) oi++;
  const int g2_offset = oi ? -1 : 1;
  dt_iop_image_copy_by_size(out, in, width, height, 1);
  DT_OMP_FOR(reduction(+ : sum1, sum2) collapse(2))
  for(size_t j = oj; j < (size_t)(height - 1); j += 2)
    for(size_t i = oi; i < (size_t)(width - 1 - g2_offset); i += 2)
    {
      sum1 += in[j * width + i];
      sum2 += in[(j + 1) * width + i + g2_offset];
    }

  if(sum1 > 0.0 && sum2 > 0.0)
    gr_ratio = sum2 / sum1;
  else
    return;

  DT_OMP_FOR(collapse(2))
  for(int j = oj; j < (height - 1); j += 2)
    for(int i = oi; i < (width - 1 - g2_offset); i += 2)
      out[(size_t)j * width + i] = fmaxf(0.0f, in[(size_t)j * width + i] * gr_ratio);
}

/* A simple 3x3 box blur used as a demosaic mask placeholder.
 * In the full darktable this is used for the "demosaic_box3" debug display;
 * we keep it here to satisfy references in process() without OpenCL. */
static void demosaic_box3(float *out,
                          const float *const in,
                          const int width,
                          const int height,
                          const uint32_t filters,
                          const uint8_t (*const xtrans)[6])
{
  (void)filters; (void)xtrans;
  /* Simple copy — debug mode not needed in libdtpipe */
  dt_iop_image_copy_by_size(out, in, width, height, 1);
}
