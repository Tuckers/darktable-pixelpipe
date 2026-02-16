/*
 * load.cc - RAW image loading via rawspeed
 *
 * Implements dtpipe_load_raw() and companion accessors.
 *
 * This is a C++ translation unit because rawspeed exposes a C++ API.
 * All functions are declared extern "C" so they are callable from C.
 *
 * Supported formats: anything rawspeed handles (CR2, NEF, ARW, RAF, ORF,
 * DNG, RW2, PEF, SRW, …).  CR3 is excluded (not supported by rawspeed).
 */

#include "RawSpeed-API.h"

#include "dtpipe.h"
#include "dtpipe_internal.h"

#include <exiv2/exiv2.hpp>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

using namespace rawspeed;

/* ── Thread-safety guard for rawspeed file reads ─────────────────────────── */
static std::mutex _read_mutex;

/* ── CameraMetaData singleton ────────────────────────────────────────────── */

static CameraMetaData *_meta = nullptr;
static std::once_flag  _meta_once;

static void _load_meta(void)
{
  std::call_once(_meta_once, []() {
    /*
     * cameras.xml is installed by the CMake build to:
     *   ${CMAKE_BINARY_DIR}/share/dtpipe/rawspeed/cameras.xml
     * At runtime the caller passes datadir via dtpipe_init(); darktable.datadir
     * is set from that.  Fall back to the build-tree path if datadir is unset.
     */
    std::string cam_path;
    if(darktable.datadir && darktable.datadir[0])
    {
      cam_path = std::string(darktable.datadir) + "/rawspeed/cameras.xml";
    }
    else
    {
      /* Best-effort: try the directory that contains the running library.
         For test builds we accept the build-tree location. */
      cam_path = DTPIPE_DATADIR "/rawspeed/cameras.xml";
    }

    try
    {
      _meta = new CameraMetaData(cam_path.c_str());
    }
    catch(const std::exception &exc)
    {
      fprintf(stderr, "[dtpipe/load] Failed to load cameras.xml from '%s': %s\n",
              cam_path.c_str(), exc.what());
      _meta = nullptr;
    }
  });
}

/*
 * dt_image_t is defined in dtpipe_internal.h.  It contains all metadata
 * fields plus pixels/pixels_size/bpp for the raw pixel buffer.
 * No separate wrapper struct is needed.
 */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Clamp an integer to [0, UINT16_MAX] */
static inline uint16_t _clamp_u16(int v)
{
  if(v < 0)         return 0;
  if(v > 0xffff)    return 0xffff;
  return (uint16_t)v;
}

/*
 * Read EXIF scalars that rawspeed doesn't expose (exposure, aperture, ISO,
 * focal length) using exiv2.
 */
static void _read_exif(dt_image_t *m, const char *path)
{
  try
  {
    auto image = Exiv2::ImageFactory::open(path);
    if(!image.get()) return;
    image->readMetadata();

    Exiv2::ExifData &exif = image->exifData();
    if(exif.empty()) return;

    auto get_float = [&](const char *key, float &out) {
      auto it = exif.findKey(Exiv2::ExifKey(key));
      if(it != exif.end())
        out = static_cast<float>(it->toFloat());
    };

    get_float("Exif.Photo.ExposureTime",     m->exif_exposure);
    get_float("Exif.Photo.ExposureBiasValue",m->exif_exposure_bias);
    get_float("Exif.Photo.FNumber",          m->exif_aperture);
    get_float("Exif.Photo.FocalLength",      m->exif_focal_length);

    /* ISO */
    {
      auto it = exif.findKey(Exiv2::ExifKey("Exif.Photo.ISOSpeedRatings"));
      if(it != exif.end())
        m->exif_iso = static_cast<float>(it->toInt64());
    }

    /* Orientation */
    {
      auto it = exif.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
      if(it != exif.end())
      {
        /* Map EXIF orientation values to dt_image_orientation_t */
        switch(it->toInt64())
        {
          case 1: m->orientation = ORIENTATION_NONE;             break;
          case 2: m->orientation = ORIENTATION_FLIP_HORIZONTALLY;break;
          case 3: m->orientation = ORIENTATION_ROTATE_180_DEG;   break;
          case 4: m->orientation = ORIENTATION_FLIP_VERTICALLY;  break;
          case 5: m->orientation = ORIENTATION_TRANSPOSE;        break;
          case 6: m->orientation = ORIENTATION_ROTATE_CW_90_DEG; break;
          case 7: m->orientation = ORIENTATION_TRANSVERSE;       break;
          case 8: m->orientation = ORIENTATION_ROTATE_CCW_90_DEG;break;
          default: break;
        }
      }
    }

    /* White balance label */
    {
      auto it = exif.findKey(Exiv2::ExifKey("Exif.Photo.WhiteBalance"));
      if(it != exif.end())
        snprintf(m->exif_whitebalance, sizeof(m->exif_whitebalance),
                 "%s", it->print(&exif).c_str());
    }

    /* Lens string */
    {
      auto it = exif.findKey(Exiv2::ExifKey("Exif.Photo.LensModel"));
      if(it == exif.end())
        it = exif.findKey(Exiv2::ExifKey("Exif.Photo.LensSpecification"));
      if(it != exif.end())
        snprintf(m->exif_lens, sizeof(m->exif_lens),
                 "%s", it->print(&exif).c_str());
    }

    /* EXIF make / model (rawspeed canonical strings are preferred but EXIF
       make/model fields are also populated for reference) */
    {
      auto it = exif.findKey(Exiv2::ExifKey("Exif.Image.Make"));
      if(it != exif.end())
        snprintf(m->exif_maker, sizeof(m->exif_maker),
                 "%s", it->print(&exif).c_str());
    }
    {
      auto it = exif.findKey(Exiv2::ExifKey("Exif.Image.Model"));
      if(it != exif.end())
        snprintf(m->exif_model, sizeof(m->exif_model),
                 "%s", it->print(&exif).c_str());
    }

    m->exif_inited = true;
  }
  catch(const std::exception &)
  {
    /* Non-fatal: EXIF read failure just means empty metadata */
  }
}

/* ── Public C API ────────────────────────────────────────────────────────── */

extern "C" {

/* forward-declare the error-string helper we may need */
static char _last_error[512] = "";

dt_image_t *dtpipe_load_raw(const char *path)
{
  if(!path || !path[0])
  {
    snprintf(_last_error, sizeof(_last_error), "dtpipe_load_raw: path is NULL or empty");
    return nullptr;
  }

  /* Ensure cameras.xml is loaded */
  _load_meta();

  /* Allocate the image struct */
  dt_image_t *img = (dt_image_t *)calloc(1, sizeof(dt_image_t));
  if(!img)
  {
    snprintf(_last_error, sizeof(_last_error), "dtpipe_load_raw: out of memory");
    return nullptr;
  }

  dt_image_t *m = img;

  /* Store filename */
  snprintf(m->filename, sizeof(m->filename), "%s", path);

  /* Read EXIF scalars first (rawspeed doesn't expose exposure/aperture/etc.) */
  _read_exif(m, path);

  try
  {
    /* 1. Read the file into a memory buffer (rawspeed wants a Buffer view) */
    FileReader reader(path);

    std::unique_ptr<std::vector<uint8_t,
      DefaultInitAllocatorAdaptor<uint8_t, AlignedAllocator<uint8_t, 16>>>>
        storage;
    Buffer buf;

    {
      std::lock_guard<std::mutex> lock(_read_mutex);
      auto result = reader.readFile();
      storage = std::move(result.first);
      buf     = result.second;
    }

    /* 2. Parse and decode */
    RawParser parser(buf);
    std::unique_ptr<RawDecoder> decoder = parser.getDecoder(_meta);
    if(!decoder)
    {
      snprintf(_last_error, sizeof(_last_error),
               "dtpipe_load_raw: unsupported format '%s'", path);
      free(img);
      return nullptr;
    }

    decoder->failOnUnknown = true;
    decoder->checkSupport(_meta);
    decoder->decodeRaw();
    decoder->decodeMetaData(_meta);

    RawImage r = decoder->mRaw;

    /* Release rawspeed storage now that decode is done */
    decoder.reset();
    storage.reset();

    /* 3. Extract metadata from the decoded RawImage */

    /* Camera strings (canonical from cameras.xml) */
    snprintf(m->camera_maker, sizeof(m->camera_maker),
             "%s", r->metadata.canonical_make.c_str());
    snprintf(m->camera_model, sizeof(m->camera_model),
             "%s", r->metadata.canonical_model.c_str());
    snprintf(m->camera_alias, sizeof(m->camera_alias),
             "%s", r->metadata.canonical_alias.c_str());
    snprintf(m->camera_makermodel, sizeof(m->camera_makermodel),
             "%s %s",
             r->metadata.canonical_make.c_str(),
             r->metadata.canonical_model.c_str());

    /* If EXIF maker/model were empty, fill from rawspeed */
    if(!m->exif_maker[0])
      snprintf(m->exif_maker, sizeof(m->exif_maker),
               "%s", r->metadata.make.c_str());
    if(!m->exif_model[0])
      snprintf(m->exif_model, sizeof(m->exif_model),
               "%s", r->metadata.model.c_str());

    /* Black/white levels */
    m->raw_black_level = (r->blackLevel >= 0)
                         ? _clamp_u16(r->blackLevel)
                         : 0;
    m->raw_white_point = r->whitePoint.has_value()
                         ? (uint32_t)*r->whitePoint
                         : (uint32_t)((1u << 16) - 1);

    /* Per-channel black levels */
    if(!r->blackLevelSeparate)
      r->calculateBlackAreas();

    if(r->blackLevelSeparate)
    {
      /* blackLevelSeparate: Optional<Array2DRef<int>>
         getAsArray1DRef():  Optional<Array1DRef<int>>  */
      const auto bl1d_opt = (*r->blackLevelSeparate).getAsArray1DRef();
      if(bl1d_opt.has_value())
      {
        const auto &bl = *bl1d_opt;
        for(int i = 0; i < 4; i++)
          m->raw_black_level_separate[i] = _clamp_u16(bl(i));
      }

      if(r->blackLevel < 0)
      {
        float black = 0.0f;
        for(int i = 0; i < 4; i++)
          black += (float)m->raw_black_level_separate[i];
        m->raw_black_level = _clamp_u16((int)roundf(black / 4.0f));
      }
    }

    /* White balance coefficients */
    if(r->metadata.wbCoeffs.has_value())
    {
      for(int i = 0; i < 4; i++)
        m->wb_coeffs[i] = (*r->metadata.wbCoeffs)[i];
    }

    /* Adobe XYZ→CAM color matrix */
    const int msize = (int)r->metadata.colorMatrix.size();
    for(int k = 0; k < 4; k++)
      for(int i = 0; i < 3; i++)
      {
        const int idx = k * 3 + i;
        m->adobe_XYZ_to_CAM[k][i] = (idx < msize)
            ? (float)r->metadata.colorMatrix[idx]
            : 0.0f;
      }

    /* Geometry */
    const iPoint2D uncropped = r->getUncroppedDim();
    const iPoint2D cropped   = r->dim;
    const iPoint2D cropTL    = r->getCropOffset();
    const iPoint2D cropBR    = uncropped - cropped - cropTL;

    m->width  = uncropped.x;
    m->height = uncropped.y;
    m->crop_x      = cropTL.x;
    m->crop_y      = cropTL.y;
    m->crop_right  = cropBR.x;
    m->crop_bottom = cropBR.y;
    m->p_width     = m->width  - m->crop_x - m->crop_right;
    m->p_height    = m->height - m->crop_y - m->crop_bottom;
    m->final_width  = m->p_width;
    m->final_height = m->p_height;

    m->fuji_rotation_pos   = r->metadata.fujiRotationPos;
    m->pixel_aspect_ratio  = (float)r->metadata.pixelAspectRatio;

    /* Buffer descriptor */
    if(r->getDataType() == RawImageType::F32)
    {
      m->flags |= DT_IMAGE_HDR;
      m->buf_dsc.datatype = TYPE_FLOAT;
      if(r->whitePoint.has_value() && *r->whitePoint == 0x3F800000)
        m->raw_white_point = 1;
      if(m->raw_white_point == 1)
        for(int k = 0; k < 4; k++)
          m->buf_dsc.processed_maximum[k] = 1.0f;
    }
    else
    {
      m->buf_dsc.datatype = TYPE_UINT16;
    }

    m->bpp = r->getBpp();

    if(r->isCFA)
    {
      m->buf_dsc.channels = 1;
      m->buf_dsc.filters  =
          ColorFilterArray::shiftDcrawFilter(
              r->cfa.getDcrawFilter(), cropTL.x, cropTL.y);

      if(m->buf_dsc.filters == 9u)
      {
        /* X-Trans: store 6×6 CFA layout */
        for(int j = 0; j < 6; j++)
          for(int i = 0; i < 6; i++)
            m->buf_dsc.xtrans[j][i] =
                (uint8_t)r->cfa.getColorAt(i % 6, j % 6);
      }

      m->flags &= ~DT_IMAGE_LDR;
      m->flags |=  DT_IMAGE_RAW;
    }
    else
    {
      /* sRAW / non-CFA */
      m->buf_dsc.channels = r->getCpp();
      m->buf_dsc.filters  = 0u;
      m->flags |= DT_IMAGE_LDR;
    }

    m->buf_dsc.cst = IOP_CS_RAW;

    /* 4. Copy the pixel buffer */
    const size_t row_bytes   = (size_t)r->pitch;
    const size_t total_bytes = row_bytes * (size_t)uncropped.y;

    m->pixels = malloc(total_bytes);
    if(!m->pixels)
    {
      snprintf(_last_error, sizeof(_last_error),
               "dtpipe_load_raw: out of memory for pixel buffer (%zu bytes)",
               total_bytes);
      free(img);
      return nullptr;
    }
    m->pixels_size = total_bytes;

    const size_t expected = (size_t)m->width * (size_t)m->height * m->bpp;
    if(expected == total_bytes)
    {
      /* Stride matches – single memcpy */
      memcpy(m->pixels,
             &r->getByteDataAsUncroppedArray2DRef()(0, 0),
             total_bytes);
    }
    else
    {
      /* Stride differs – copy row by row */
      const uint8_t *src =
          (const uint8_t *)&r->getByteDataAsUncroppedArray2DRef()(0, 0);
      uint8_t *dst = (uint8_t *)m->pixels;
      const size_t line_bytes = (size_t)m->width * m->bpp;
      for(int row = 0; row < uncropped.y; row++)
      {
        memcpy(dst + row * line_bytes, src + row * row_bytes, line_bytes);
      }
      /* Adjust stored size to reflect the tightly-packed copy */
      m->pixels_size = line_bytes * (size_t)uncropped.y;
    }

    _last_error[0] = '\0';
    return img;
  }
  catch(const rawspeed::RawspeedException &exc)
  {
    snprintf(_last_error, sizeof(_last_error),
             "dtpipe_load_raw: rawspeed error: %s", exc.what());
  }
  catch(const std::exception &exc)
  {
    snprintf(_last_error, sizeof(_last_error),
             "dtpipe_load_raw: error: %s", exc.what());
  }
  catch(...)
  {
    snprintf(_last_error, sizeof(_last_error),
             "dtpipe_load_raw: unknown error");
  }

  free(img);
  return nullptr;
}

void dtpipe_free_image(dt_image_t *img)
{
  if(!img) return;
  free(img->pixels);
  free(img);
}

int dtpipe_get_width(dt_image_t *img)
{
  if(!img) return -1;
  return img->width;
}

int dtpipe_get_height(dt_image_t *img)
{
  if(!img) return -1;
  return img->height;
}

const char *dtpipe_get_camera_maker(dt_image_t *img)
{
  if(!img) return nullptr;
  return img->camera_maker;
}

const char *dtpipe_get_camera_model(dt_image_t *img)
{
  if(!img) return nullptr;
  return img->camera_model;
}

const char *dtpipe_get_last_error(void)
{
  return _last_error;
}

/*
 * dtpipe_image_get_pixels - internal helper for pipeline code.
 *
 * Returns a pointer to the raw pixel buffer and fills *out_bpp with the
 * bytes-per-pixel of the buffer (2 = uint16, 4 = float).
 * Not part of the public API surface; declared in dtpipe_internal.h (Phase 4+).
 */
void *dtpipe_image_get_pixels(dt_image_t *img, uint32_t *out_bpp)
{
  if(!img) return nullptr;
  if(out_bpp) *out_bpp = img->bpp;
  return img->pixels;
}

} /* extern "C" */
