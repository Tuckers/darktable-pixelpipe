/*
 * export.c - Export functions for libdtpipe
 *
 * Implements:
 *   dtpipe_export_jpeg(pipe, path, quality)
 *   dtpipe_export_png(pipe, path)
 *   dtpipe_export_tiff(pipe, path, bits)
 *
 * All three functions render at full resolution (scale = 1.0) and write
 * the result to the requested path.  Metadata (EXIF) is embedded using
 * exiv2 after the file is written.
 *
 * Bit-depth strategy
 * ──────────────────
 *   JPEG  : 8-bit sRGB  (libjpeg)
 *   PNG   : 16-bit sRGB (libpng)
 *   TIFF  : 8, 16, or 32-bit float (libtiff)
 *
 * For JPEG and 8-bit TIFF/PNG, the pipeline's float-RGBA backbuf is
 * converted through the standard piecewise sRGB transfer function then
 * quantised to uint8 or uint16.
 *
 * For 16-bit PNG/TIFF the same gamma curve is applied but quantised
 * to uint16 (0–65535) for higher fidelity.
 *
 * For 32-bit TIFF the float values are written as-is (linear scene-referred).
 *
 * Colorspace note
 * ───────────────
 * The pipeline currently operates as a passthrough stub (no ICC transforms
 * wired in, Phase 4+).  Output is tagged as sRGB in all formats; actual
 * colour science accuracy comes when real ICC transforms are added.
 *
 * EXIF embedding
 * ──────────────
 * After writing the raster data, we use exiv2 to copy the source image's
 * EXIF block into the output file.  This is done by:
 *   1. Open the output file with exiv2.
 *   2. Set key EXIF tags from dt_image_t (make, model, exposure, etc.).
 *   3. Write/save.
 *
 * Exiv2 is a C++ library; this translation unit is therefore compiled as C++
 * (renamed export.cc in CMakeLists).  All public functions are extern "C".
 */

#ifdef __cplusplus
#  include <cerrno>
#  include <cmath>
#  include <cstdio>
#  include <cstdlib>
#  include <cstring>
#else
#  include <errno.h>
#  include <math.h>
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#endif

#include <jpeglib.h>
#include <png.h>
#include <tiffio.h>

/* exiv2 is C++ only; include only when compiling as C++ */
#ifdef __cplusplus
#  include <exiv2/exiv2.hpp>
#endif

#include "dtpipe.h"
#include "dtpipe_internal.h"
#include "pipe/create.h"
#include "pipe/pixelpipe.h"
#include "pipe/render.h"

/* ── sRGB gamma (identical to render.c) ──────────────────────────────────── */

static inline float _srgb_gamma(float x)
{
  if(x <= 0.0f)       return 0.0f;
  if(x >= 1.0f)       return 1.0f;
  if(x <= 0.0031308f) return x * 12.92f;
  return 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
}

static inline uint8_t _to_u8(float x)
{
  return (uint8_t)(_srgb_gamma(x) * 255.0f + 0.5f);
}

static inline uint16_t _to_u16(float x)
{
  return (uint16_t)(_srgb_gamma(x) * 65535.0f + 0.5f);
}

/* ── _run_pipeline ───────────────────────────────────────────────────────── */
/*
 * Run the pipeline at full resolution and return a pointer to the float-RGBA
 * backbuf.  Sets *out_w and *out_h to the rendered dimensions.
 *
 * Returns non-NULL on success; the returned pointer is owned by the pipeline
 * and is valid until the next render call or dtpipe_free().
 */
static const float *_run_pipeline(dt_pipe_t *pipe, int *out_w, int *out_h)
{
  /* Ensure float-RGBA input buffer exists (reuse render.c internal helper
     via the shared render.h declaration). */
  if(!dtpipe_ensure_input_buf(pipe))
    return NULL;

  /* Reset pipe->pipe.dsc to the initial image format before each render.
     Format-changing modules (rawprepare, demosaic) mutate pipe->pipe.dsc
     in-place; without this reset the export sees the post-pipeline format
     from the previous render as its input format. */
  pipe->pipe.dsc = pipe->initial_dsc;

  const int W = pipe->input_width;
  const int H = pipe->input_height;

  dt_dev_pixelpipe_set_input(&pipe->pipe,
                             pipe->input_buf,
                             W, H,
                             1.0f,   /* iscale: full resolution */
                             pipe->img);

  const bool err = dt_dev_pixelpipe_process(&pipe->pipe,
                                            0, 0,   /* origin */
                                            W, H,
                                            1.0f);  /* scale */
  if(err)
  {
    fprintf(stderr, "[dtpipe/export] pipeline render failed\n");
    return NULL;
  }

  if(!pipe->pipe.backbuf)
  {
    fprintf(stderr, "[dtpipe/export] pipeline produced no output\n");
    return NULL;
  }

  *out_w = pipe->pipe.backbuf_width;
  *out_h = pipe->pipe.backbuf_height;
  return (const float *)pipe->pipe.backbuf;
}

/* ── EXIF embedding via exiv2 ─────────────────────────────────────────────── */

#ifdef __cplusplus
static void _embed_exif(const char *path, const dt_image_t *img)
{
  if(!img) return;
  try
  {
    auto image = Exiv2::ImageFactory::open(path);
    if(!image.get()) return;
    image->readMetadata();

    Exiv2::ExifData &exif = image->exifData();

    if(img->exif_maker[0])
      exif["Exif.Image.Make"]  = std::string(img->exif_maker);
    if(img->exif_model[0])
      exif["Exif.Image.Model"] = std::string(img->exif_model);
    if(img->exif_lens[0])
      exif["Exif.Photo.LensModel"] = std::string(img->exif_lens);

    if(img->exif_exposure > 0.0f)
    {
      /* Store as rational: numerator=1, denominator=round(1/exposure) */
      const int denom = (int)(1.0f / img->exif_exposure + 0.5f);
      exif["Exif.Photo.ExposureTime"] = Exiv2::Rational(1, denom > 0 ? denom : 1);
    }

    if(img->exif_aperture > 0.0f)
      exif["Exif.Photo.FNumber"] =
        Exiv2::Rational((int)(img->exif_aperture * 10 + 0.5f), 10);

    if(img->exif_iso > 0.0f)
      exif["Exif.Photo.ISOSpeedRatings"] = (uint16_t)img->exif_iso;

    if(img->exif_focal_length > 0.0f)
      exif["Exif.Photo.FocalLength"] =
        Exiv2::Rational((int)(img->exif_focal_length * 10 + 0.5f), 10);

    image->writeMetadata();
  }
  catch(const Exiv2::Error &e)
  {
    fprintf(stderr, "[dtpipe/export] exiv2 error embedding EXIF: %s\n", e.what());
    /* Non-fatal: the file is still valid without EXIF */
  }
}
#else
static void _embed_exif(const char *path, const dt_image_t *img)
{
  (void)path; (void)img;
  /* EXIF embedding requires C++ (exiv2); skipped in C builds */
}
#endif

/* ── JPEG export ─────────────────────────────────────────────────────────── */

int dtpipe_export_jpeg(dt_pipe_t *pipe, const char *path, int quality)
{
  if(!pipe || !path) return DTPIPE_ERR_INVALID_ARG;
  if(quality < 1 || quality > 100) quality = 90;

  int W = 0, H = 0;
  const float *fbuf = _run_pipeline(pipe, &W, &H);
  if(!fbuf) return DTPIPE_ERR_RENDER;

  /* Allocate 8-bit RGB row (JPEG doesn't use alpha) */
  uint8_t *rowbuf = (uint8_t *)malloc((size_t)W * 3);
  if(!rowbuf) return DTPIPE_ERR_NO_MEMORY;

  FILE *fp = fopen(path, "wb");
  if(!fp)
  {
    free(rowbuf);
    fprintf(stderr, "[dtpipe/export] cannot open '%s': %s\n", path, strerror(errno));
    return DTPIPE_ERR_IO;
  }

  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr       jerr;

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, fp);

  cinfo.image_width      = (JDIMENSION)W;
  cinfo.image_height     = (JDIMENSION)H;
  cinfo.input_components = 3;
  cinfo.in_color_space   = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  /* Progressive JPEG for smaller files */
  jpeg_simple_progression(&cinfo);

  jpeg_start_compress(&cinfo, TRUE);

  for(int row = 0; row < H; row++)
  {
    const float *src = fbuf + (size_t)row * W * 4;
    for(int x = 0; x < W; x++)
    {
      rowbuf[3 * x + 0] = _to_u8(src[4 * x + 0]);
      rowbuf[3 * x + 1] = _to_u8(src[4 * x + 1]);
      rowbuf[3 * x + 2] = _to_u8(src[4 * x + 2]);
    }
    JSAMPROW row_ptr = rowbuf;
    jpeg_write_scanlines(&cinfo, &row_ptr, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  fclose(fp);
  free(rowbuf);

  _embed_exif(path, pipe->img);
  return DTPIPE_OK;
}

/* ── PNG export ──────────────────────────────────────────────────────────── */

int dtpipe_export_png(dt_pipe_t *pipe, const char *path)
{
  if(!pipe || !path) return DTPIPE_ERR_INVALID_ARG;

  int W = 0, H = 0;
  const float *fbuf = _run_pipeline(pipe, &W, &H);
  if(!fbuf) return DTPIPE_ERR_RENDER;

  FILE *fp = fopen(path, "wb");
  if(!fp)
  {
    fprintf(stderr, "[dtpipe/export] cannot open '%s': %s\n", path, strerror(errno));
    return DTPIPE_ERR_IO;
  }

  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                                NULL, NULL, NULL);
  if(!png_ptr) { fclose(fp); return DTPIPE_ERR_EXPORT; }

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if(!info_ptr)
  {
    png_destroy_write_struct(&png_ptr, NULL);
    fclose(fp);
    return DTPIPE_ERR_EXPORT;
  }

  if(setjmp(png_jmpbuf(png_ptr)))
  {
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);
    return DTPIPE_ERR_EXPORT;
  }

  png_init_io(png_ptr, fp);

  /* 16-bit RGB (no alpha — standard for photo export) */
  png_set_IHDR(png_ptr, info_ptr,
               (png_uint_32)W, (png_uint_32)H,
               16,                   /* bit depth */
               PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);

  /* sRGB chunk: perceptual rendering intent */
  png_set_sRGB_gAMA_and_cHRM(png_ptr, info_ptr, PNG_sRGB_INTENT_PERCEPTUAL);

  png_write_info(png_ptr, info_ptr);

  /* libpng 16-bit rows are big-endian */
  png_set_swap(png_ptr); /* swap to native-endian after setting up big-endian write */

  /* Allocate one row of uint16 RGB */
  uint16_t *rowbuf = (uint16_t *)malloc((size_t)W * 3 * sizeof(uint16_t));
  if(!rowbuf)
  {
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);
    return DTPIPE_ERR_NO_MEMORY;
  }

  for(int row = 0; row < H; row++)
  {
    const float *src = fbuf + (size_t)row * W * 4;
    for(int x = 0; x < W; x++)
    {
      rowbuf[3 * x + 0] = _to_u16(src[4 * x + 0]);
      rowbuf[3 * x + 1] = _to_u16(src[4 * x + 1]);
      rowbuf[3 * x + 2] = _to_u16(src[4 * x + 2]);
    }
    png_write_row(png_ptr, (png_bytep)rowbuf);
  }

  png_write_end(png_ptr, NULL);
  png_destroy_write_struct(&png_ptr, &info_ptr);
  free(rowbuf);
  fclose(fp);

  _embed_exif(path, pipe->img);
  return DTPIPE_OK;
}

/* ── TIFF export ─────────────────────────────────────────────────────────── */

int dtpipe_export_tiff(dt_pipe_t *pipe, const char *path, int bits)
{
  if(!pipe || !path) return DTPIPE_ERR_INVALID_ARG;
  if(bits != 8 && bits != 16 && bits != 32) return DTPIPE_ERR_INVALID_ARG;

  int W = 0, H = 0;
  const float *fbuf = _run_pipeline(pipe, &W, &H);
  if(!fbuf) return DTPIPE_ERR_RENDER;

  TIFF *tif = TIFFOpen(path, "w");
  if(!tif)
  {
    fprintf(stderr, "[dtpipe/export] cannot open '%s' for TIFF write\n", path);
    return DTPIPE_ERR_IO;
  }

  /* Basic TIFF tags */
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      (uint32_t)W);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     (uint32_t)H);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);
  TIFFSetField(tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP,    1);

  if(bits == 32)
  {
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 32);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT,  SAMPLEFORMAT_IEEEFP);
    /* LZW compression for float TIFF */
    TIFFSetField(tif, TIFFTAG_COMPRESSION,   COMPRESSION_LZW);
    TIFFSetField(tif, TIFFTAG_PREDICTOR,     PREDICTOR_FLOATINGPOINT);
  }
  else
  {
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT,  SAMPLEFORMAT_UINT);
    /* LZW + horizontal differencing predictor for good int compression */
    TIFFSetField(tif, TIFFTAG_COMPRESSION,   COMPRESSION_LZW);
    TIFFSetField(tif, TIFFTAG_PREDICTOR,     PREDICTOR_HORIZONTAL);
  }

  /* Write source metadata into TIFF tags */
  if(pipe->img)
  {
    if(pipe->img->exif_maker[0])
      TIFFSetField(tif, TIFFTAG_MAKE,  pipe->img->exif_maker);
    if(pipe->img->exif_model[0])
      TIFFSetField(tif, TIFFTAG_MODEL, pipe->img->exif_model);
  }

  const size_t row_bytes = (bits == 32) ? (size_t)W * 3 * sizeof(float)
                         : (bits == 16) ? (size_t)W * 3 * sizeof(uint16_t)
                                        : (size_t)W * 3 * sizeof(uint8_t);
  void *rowbuf = malloc(row_bytes);
  if(!rowbuf)
  {
    TIFFClose(tif);
    return DTPIPE_ERR_NO_MEMORY;
  }

  int write_ok = 1;
  for(int row = 0; row < H && write_ok; row++)
  {
    const float *src = fbuf + (size_t)row * W * 4;

    if(bits == 32)
    {
      float *dst = (float *)rowbuf;
      for(int x = 0; x < W; x++)
      {
        /* Clamp linear float to [0,1] – scene-linear, no gamma */
        dst[3 * x + 0] = CLAMPS(src[4 * x + 0], 0.0f, 1.0f);
        dst[3 * x + 1] = CLAMPS(src[4 * x + 1], 0.0f, 1.0f);
        dst[3 * x + 2] = CLAMPS(src[4 * x + 2], 0.0f, 1.0f);
      }
    }
    else if(bits == 16)
    {
      uint16_t *dst = (uint16_t *)rowbuf;
      for(int x = 0; x < W; x++)
      {
        dst[3 * x + 0] = _to_u16(src[4 * x + 0]);
        dst[3 * x + 1] = _to_u16(src[4 * x + 1]);
        dst[3 * x + 2] = _to_u16(src[4 * x + 2]);
      }
    }
    else /* bits == 8 */
    {
      uint8_t *dst = (uint8_t *)rowbuf;
      for(int x = 0; x < W; x++)
      {
        dst[3 * x + 0] = _to_u8(src[4 * x + 0]);
        dst[3 * x + 1] = _to_u8(src[4 * x + 1]);
        dst[3 * x + 2] = _to_u8(src[4 * x + 2]);
      }
    }

    if(TIFFWriteScanline(tif, rowbuf, (uint32_t)row, 0) < 0)
    {
      fprintf(stderr, "[dtpipe/export] TIFF write error at row %d\n", row);
      write_ok = 0;
    }
  }

  free(rowbuf);
  TIFFClose(tif);

  if(!write_ok) return DTPIPE_ERR_EXPORT;

  _embed_exif(path, pipe->img);
  return DTPIPE_OK;
}
