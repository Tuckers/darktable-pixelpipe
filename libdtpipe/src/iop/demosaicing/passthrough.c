/*
 * demosaicing/passthrough.c — passthrough demosaic modes for libdtpipe.
 *
 * Extracted from darktable src/iop/demosaicing/passthrough.c (GPLv3).
 * Textually #include'd by demosaic.c.
 *
 * Copyright (C) 2010-2024 darktable developers (GPLv3)
 */

static void passthrough_monochrome(float *out,
                                   const float *const in,
                                   const int width,
                                   const int height)
{
  DT_OMP_FOR(collapse(2))
  for(int j = 0; j < height; j++)
  {
    for(int i = 0; i < width; i++)
    {
      for(int c = 0; c < 3; c++)
        out[(size_t)4 * ((size_t)j * width + i) + c] = in[(size_t)j * width + i];
    }
  }
}

static void passthrough_color(float *out,
                              const float *const in,
                              const int width,
                              const int height,
                              const uint32_t filters,
                              const uint8_t (*const xtrans)[6])
{
  if(filters != 9u)
  {
    DT_OMP_FOR(collapse(2))
    for(int row = 0; row < height; row++)
    {
      for(int col = 0; col < width; col++)
      {
        const float val = in[(size_t)col + row * width];
        const size_t offset = (size_t)4 * ((size_t)row * width + col);
        const size_t ch = FC(row, col, filters);
        out[offset] = out[offset + 1] = out[offset + 2] = 0.0f;
        out[offset + ch] = val;
      }
    }
  }
  else
  {
    DT_OMP_FOR(collapse(2))
    for(int row = 0; row < height; row++)
    {
      for(int col = 0; col < width; col++)
      {
        const float val = in[(size_t)col + row * width];
        const size_t offset = (size_t)4 * ((size_t)row * width + col);
        const size_t ch = FCNxtrans(row, col, xtrans);
        out[offset] = out[offset + 1] = out[offset + 2] = 0.0f;
        out[offset + ch] = val;
      }
    }
  }
}
