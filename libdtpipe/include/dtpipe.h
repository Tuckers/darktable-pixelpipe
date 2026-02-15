#pragma once

/* dtpipe.h - Public API for libdtpipe
 *
 * libdtpipe is a standalone pixelpipe library extracted from darktable.
 * It provides raw image processing using darktable's IOP (image operation)
 * pipeline without requiring the darktable GUI or database.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Opaque handle types */
typedef struct dtpipe_image_t dtpipe_image_t;
typedef struct dtpipe_pipeline_t dtpipe_pipeline_t;

/* Error codes */
typedef enum {
  DTPIPE_OK             = 0,
  DTPIPE_ERROR_GENERIC  = -1,
  DTPIPE_ERROR_IO       = -2,
  DTPIPE_ERROR_MEMORY   = -3,
  DTPIPE_ERROR_INVALID  = -4,
} dtpipe_error_t;

/* Pixel formats for output */
typedef enum {
  DTPIPE_FORMAT_FLOAT32_RGB  = 0,  /* 3 floats per pixel, 0.0-1.0 */
  DTPIPE_FORMAT_UINT8_RGB    = 1,  /* 3 bytes per pixel, sRGB */
  DTPIPE_FORMAT_UINT16_RGB   = 2,  /* 3 uint16 per pixel, sRGB */
} dtpipe_format_t;

/* Image metadata */
typedef struct {
  int      width;
  int      height;
  char     camera_maker[64];
  char     camera_model[64];
  float    exif_iso;
  float    exif_exposure;
  float    exif_aperture;
  float    exif_focal_length;
} dtpipe_image_info_t;

/* Render result */
typedef struct {
  void            *data;      /* Caller must free with dtpipe_free_buffer() */
  size_t           size;      /* Total bytes in data */
  int              width;
  int              height;
  dtpipe_format_t  format;
} dtpipe_render_result_t;

/*
 * Library lifecycle
 */

/** Initialize the library. Must be called once before any other function. */
dtpipe_error_t dtpipe_init(void);

/** Shut down the library and release global resources. */
void dtpipe_cleanup(void);

/*
 * Image loading
 */

/** Load a raw image file. Returns NULL on failure. */
dtpipe_image_t *dtpipe_image_load(const char *path);

/** Get metadata for a loaded image. */
dtpipe_error_t dtpipe_image_get_info(const dtpipe_image_t *img,
                                      dtpipe_image_info_t  *info);

/** Free a loaded image. */
void dtpipe_image_free(dtpipe_image_t *img);

/*
 * Pipeline
 */

/** Create a processing pipeline for an image.
 *  history_json may be NULL for default settings. */
dtpipe_pipeline_t *dtpipe_pipeline_create(dtpipe_image_t *img,
                                           const char     *history_json);

/** Get the number of IOPs in the pipeline. */
int dtpipe_pipeline_iop_count(const dtpipe_pipeline_t *pipe);

/** Get IOP name by index. */
const char *dtpipe_pipeline_iop_name(const dtpipe_pipeline_t *pipe, int index);

/** Get IOP parameters as JSON string. Caller must free with dtpipe_free_string(). */
char *dtpipe_pipeline_iop_get_params(const dtpipe_pipeline_t *pipe, int index);

/** Set IOP parameters from JSON string. */
dtpipe_error_t dtpipe_pipeline_iop_set_params(dtpipe_pipeline_t *pipe,
                                               int                index,
                                               const char        *params_json);

/** Free the pipeline. */
void dtpipe_pipeline_free(dtpipe_pipeline_t *pipe);

/*
 * Rendering
 */

/** Render the pipeline to a buffer.
 *  scale: 1.0 = full resolution, 0.5 = half resolution, etc. */
dtpipe_error_t dtpipe_render(dtpipe_pipeline_t      *pipe,
                              float                   scale,
                              dtpipe_format_t         format,
                              dtpipe_render_result_t *result);

/*
 * History serialization
 */

/** Export pipeline history as JSON string. Caller must free with dtpipe_free_string(). */
char *dtpipe_history_to_json(const dtpipe_pipeline_t *pipe);

/** Import pipeline history from JSON string. */
dtpipe_error_t dtpipe_history_from_json(dtpipe_pipeline_t *pipe,
                                         const char        *json);

/** Read XMP sidecar and apply to pipeline. */
dtpipe_error_t dtpipe_xmp_read(dtpipe_pipeline_t *pipe, const char *xmp_path);

/** Write XMP sidecar from pipeline history. */
dtpipe_error_t dtpipe_xmp_write(const dtpipe_pipeline_t *pipe,
                                 const char              *xmp_path);

/*
 * Memory helpers
 */

/** Free a buffer returned by dtpipe_render(). */
void dtpipe_free_buffer(void *buf);

/** Free a string returned by dtpipe functions. */
void dtpipe_free_string(char *str);

#ifdef __cplusplus
}
#endif
