/*
 * dtpipe.h - Public API for libdtpipe
 *
 * libdtpipe is a standalone image processing pipeline extracted from darktable.
 * It provides raw image loading, non-destructive parameter-based editing,
 * and export to common formats.
 *
 * C89 compatible. All types are opaque handles to maintain ABI stability
 * and maximum FFI compatibility.
 *
 * Thread safety:
 *   - dtpipe_init() and dtpipe_cleanup() are NOT thread-safe. Call them
 *     once from the main thread before and after all other operations.
 *   - dt_image_t handles are NOT thread-safe. Do not share across threads.
 *   - dt_pipe_t handles are NOT thread-safe. Do not share across threads.
 *   - Multiple independent pipelines from different images may be used
 *     concurrently from separate threads, as long as each pipeline is only
 *     accessed from one thread at a time.
 */

#ifndef DTPIPE_H
#define DTPIPE_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Error codes
 * -------------------------------------------------------------------------
 * All functions that can fail return an int error code.
 * Zero (DTPIPE_OK) indicates success. Negative values indicate errors.
 * Getter functions that return a pointer return NULL on failure.
 */

#define DTPIPE_OK                  0   /* Success */
#define DTPIPE_ERR_GENERIC        -1   /* Unspecified error */
#define DTPIPE_ERR_INVALID_ARG    -2   /* NULL or invalid argument */
#define DTPIPE_ERR_NOT_FOUND      -3   /* File or module not found */
#define DTPIPE_ERR_IO             -4   /* File read/write failure */
#define DTPIPE_ERR_FORMAT         -5   /* Unsupported or corrupt file format */
#define DTPIPE_ERR_NO_MEMORY      -6   /* Memory allocation failure */
#define DTPIPE_ERR_ALREADY_INIT   -7   /* dtpipe_init() called more than once */
#define DTPIPE_ERR_NOT_INIT       -8   /* Library not initialized */
#define DTPIPE_ERR_MODULE         -9   /* Module operation failed */
#define DTPIPE_ERR_PARAM_TYPE    -10   /* Wrong type for parameter */
#define DTPIPE_ERR_RENDER        -11   /* Pipeline render failed */
#define DTPIPE_ERR_EXPORT        -12   /* Export encoding failed */

/* -------------------------------------------------------------------------
 * Opaque handle types
 * -------------------------------------------------------------------------
 * These are forward declarations only. The internal representation is not
 * part of the public ABI. Always allocate and free via the provided API.
 */

typedef struct dt_image_t          dt_image_t;
typedef struct dt_pipe_s           dt_pipe_t;
typedef struct dt_render_result_s  dt_render_result_t;

/* -------------------------------------------------------------------------
 * Render result
 * -------------------------------------------------------------------------
 * Returned by dtpipe_render() and dtpipe_render_region().
 * The pixel buffer is owned by the result handle; free with
 * dtpipe_free_render(). Do not free 'pixels' directly.
 *
 * Pixel format: 8-bit RGBA, interleaved (R G B A R G B A ...).
 * Row-major, top-to-bottom, left-to-right.
 * stride is the number of bytes per row (>= width * 4).
 */
struct dt_render_result_s {
    unsigned char *pixels; /* RGBA pixel data, row-major               */
    int            width;  /* Width of the rendered region in pixels    */
    int            height; /* Height of the rendered region in pixels   */
    int            stride; /* Bytes per row                             */
};

/* -------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------
 */

/*
 * dtpipe_init - Initialize the library.
 *
 * Must be called once before any other dtpipe function.
 * data_dir: path to the libdtpipe data directory containing LUTs, presets,
 *           color profiles, etc. Pass NULL to use the compiled-in default.
 *
 * Returns DTPIPE_OK on success, DTPIPE_ERR_ALREADY_INIT if called again
 * without an intervening dtpipe_cleanup(), or a negative error code.
 */
int dtpipe_init(const char *data_dir);

/*
 * dtpipe_cleanup - Shut down the library and release global resources.
 *
 * Call once when done. All dt_pipe_t and dt_image_t handles must be freed
 * before calling this function; behaviour is undefined otherwise.
 */
void dtpipe_cleanup(void);

/* -------------------------------------------------------------------------
 * Image loading
 * -------------------------------------------------------------------------
 */

/*
 * dtpipe_load_raw - Load a raw image file.
 *
 * path: absolute or relative path to the raw file.
 *       Supported formats are determined by the bundled rawspeed library
 *       (Canon CR2/CR3, Nikon NEF, Sony ARW, Fuji RAF, etc.).
 *
 * Returns a new dt_image_t handle on success, or NULL on failure.
 * The caller is responsible for freeing the handle with dtpipe_free_image().
 */
dt_image_t *dtpipe_load_raw(const char *path);

/*
 * dtpipe_free_image - Free an image handle.
 *
 * img may be NULL (no-op).
 * Do not use the handle after this call.
 */
void dtpipe_free_image(dt_image_t *img);

/*
 * dtpipe_get_width - Return the full (unscaled) image width in pixels.
 *
 * Returns -1 if img is NULL.
 */
int dtpipe_get_width(dt_image_t *img);

/*
 * dtpipe_get_height - Return the full (unscaled) image height in pixels.
 *
 * Returns -1 if img is NULL.
 */
int dtpipe_get_height(dt_image_t *img);

/*
 * dtpipe_get_camera_maker - Return the camera manufacturer string from EXIF.
 *
 * The returned pointer is valid for the lifetime of img. Do not free it.
 * Returns NULL if img is NULL or no maker information is available.
 */
const char *dtpipe_get_camera_maker(dt_image_t *img);

/*
 * dtpipe_get_camera_model - Return the camera model string from EXIF.
 *
 * The returned pointer is valid for the lifetime of img. Do not free it.
 * Returns NULL if img is NULL or no model information is available.
 */
const char *dtpipe_get_camera_model(dt_image_t *img);

/*
 * dtpipe_get_last_error - Return a human-readable description of the most
 * recent error from any dtpipe function, or an empty string if no error
 * has occurred. The returned pointer is valid until the next dtpipe call
 * on the same thread. Do not free it.
 */
const char *dtpipe_get_last_error(void);

/* -------------------------------------------------------------------------
 * Pipeline
 * -------------------------------------------------------------------------
 */

/*
 * dtpipe_create - Create a new processing pipeline for an image.
 *
 * img: a loaded image handle. The pipeline holds a reference to img;
 *      img must remain valid for the lifetime of the pipeline.
 *
 * The pipeline is initialized with darktable's default IOP order and each
 * module's default parameters. Use dtpipe_load_history() or dtpipe_load_xmp()
 * to apply a specific editing history.
 *
 * Returns a new dt_pipe_t handle, or NULL on failure.
 * Free with dtpipe_free().
 */
dt_pipe_t *dtpipe_create(dt_image_t *img);

/*
 * dtpipe_free - Free a pipeline handle.
 *
 * pipe may be NULL (no-op).
 * Do not use the handle after this call.
 */
void dtpipe_free(dt_pipe_t *pipe);

/* -------------------------------------------------------------------------
 * Parameters
 * -------------------------------------------------------------------------
 * Module names are the darktable IOP operation names, e.g. "exposure",
 * "colorin", "demosaic", "sharpen". Parameter names match the field names
 * in the corresponding dt_iop_*_params_t struct.
 *
 * All set/get functions return DTPIPE_OK on success or a negative error code.
 */

/*
 * dtpipe_set_param_float - Set a float parameter on a module.
 *
 * pipe:   pipeline handle
 * module: IOP operation name (e.g. "exposure")
 * param:  parameter field name (e.g. "exposure")
 * value:  new value
 *
 * Returns DTPIPE_OK, DTPIPE_ERR_NOT_FOUND if module/param not found,
 * DTPIPE_ERR_PARAM_TYPE if param is not a float, or DTPIPE_ERR_INVALID_ARG.
 */
int dtpipe_set_param_float(dt_pipe_t *pipe, const char *module,
                           const char *param, float value);

/*
 * dtpipe_set_param_int - Set an integer parameter on a module.
 *
 * Returns DTPIPE_OK, DTPIPE_ERR_NOT_FOUND, DTPIPE_ERR_PARAM_TYPE, or
 * DTPIPE_ERR_INVALID_ARG.
 */
int dtpipe_set_param_int(dt_pipe_t *pipe, const char *module,
                         const char *param, int value);

/*
 * dtpipe_get_param_float - Read a float parameter from a module.
 *
 * out: pointer to a float that receives the value on success. Must not be NULL.
 *
 * Returns DTPIPE_OK, DTPIPE_ERR_NOT_FOUND, DTPIPE_ERR_PARAM_TYPE, or
 * DTPIPE_ERR_INVALID_ARG.
 */
int dtpipe_get_param_float(dt_pipe_t *pipe, const char *module,
                           const char *param, float *out);

/*
 * dtpipe_enable_module - Enable or disable a module in the pipeline.
 *
 * enabled: non-zero to enable, zero to disable.
 *
 * Returns DTPIPE_OK or DTPIPE_ERR_NOT_FOUND.
 */
int dtpipe_enable_module(dt_pipe_t *pipe, const char *module, int enabled);

/* -------------------------------------------------------------------------
 * Rendering
 * -------------------------------------------------------------------------
 */

/*
 * dtpipe_render - Render the full image.
 *
 * pipe:  pipeline handle
 * scale: output scale factor relative to the full image dimensions.
 *        1.0 = full resolution, 0.5 = half resolution, etc.
 *        Must be > 0.0.
 *
 * Returns a new dt_render_result_t on success, or NULL on failure.
 * The caller must free the result with dtpipe_free_render().
 */
dt_render_result_t *dtpipe_render(dt_pipe_t *pipe, float scale);

/*
 * dtpipe_render_region - Render a rectangular crop of the image.
 *
 * x, y:  top-left corner of the region in full-resolution image coordinates.
 * w, h:  width and height of the region in full-resolution image coordinates.
 * scale: output scale factor applied to the region. 1.0 = 1:1 pixel mapping.
 *        Must be > 0.0.
 *
 * The output pixel dimensions will be (int)(w * scale) x (int)(h * scale).
 *
 * Returns a new dt_render_result_t on success, or NULL on failure.
 * The caller must free the result with dtpipe_free_render().
 */
dt_render_result_t *dtpipe_render_region(dt_pipe_t *pipe,
                                         int x, int y, int w, int h,
                                         float scale);

/*
 * dtpipe_free_render - Free a render result.
 *
 * result may be NULL (no-op).
 */
void dtpipe_free_render(dt_render_result_t *result);

/* -------------------------------------------------------------------------
 * Export
 * -------------------------------------------------------------------------
 * Export functions render at full resolution and write the output to disk.
 * All return DTPIPE_OK on success or a negative error code.
 */

/*
 * dtpipe_export_jpeg - Export to JPEG.
 *
 * path:    destination file path. Created or overwritten.
 * quality: JPEG quality, 1-100. 90 is a reasonable default.
 *
 * Returns DTPIPE_OK, DTPIPE_ERR_IO, DTPIPE_ERR_RENDER, or DTPIPE_ERR_EXPORT.
 */
int dtpipe_export_jpeg(dt_pipe_t *pipe, const char *path, int quality);

/*
 * dtpipe_export_png - Export to 16-bit PNG.
 *
 * path: destination file path. Created or overwritten.
 *
 * Returns DTPIPE_OK, DTPIPE_ERR_IO, DTPIPE_ERR_RENDER, or DTPIPE_ERR_EXPORT.
 */
int dtpipe_export_png(dt_pipe_t *pipe, const char *path);

/*
 * dtpipe_export_tiff - Export to TIFF.
 *
 * path: destination file path. Created or overwritten.
 * bits: bit depth per channel. Supported values: 8, 16, 32 (float).
 *
 * Returns DTPIPE_OK, DTPIPE_ERR_IO, DTPIPE_ERR_RENDER, DTPIPE_ERR_EXPORT,
 * or DTPIPE_ERR_INVALID_ARG if bits is not 8, 16, or 32.
 */
int dtpipe_export_tiff(dt_pipe_t *pipe, const char *path, int bits);

/* -------------------------------------------------------------------------
 * History serialization
 * -------------------------------------------------------------------------
 */

/*
 * dtpipe_serialize_history - Serialize the current pipeline state to JSON.
 *
 * Returns a newly allocated null-terminated JSON string on success.
 * The caller must free the string with free(3).
 * Returns NULL on failure (no memory, or library not initialized).
 *
 * The JSON format is the dtpipe history format (see docs/history-format.md).
 */
char *dtpipe_serialize_history(dt_pipe_t *pipe);

/*
 * dtpipe_load_history - Apply a serialized history to the pipeline.
 *
 * json: null-terminated JSON string in dtpipe history format.
 *
 * Replaces the current module parameters with those in json.
 * Returns DTPIPE_OK, DTPIPE_ERR_INVALID_ARG, DTPIPE_ERR_FORMAT, or
 * DTPIPE_ERR_MODULE.
 */
int dtpipe_load_history(dt_pipe_t *pipe, const char *json);

/*
 * dtpipe_load_xmp - Read editing history from a darktable XMP sidecar file.
 *
 * path: path to the .xmp file.
 *
 * Returns DTPIPE_OK, DTPIPE_ERR_NOT_FOUND, DTPIPE_ERR_IO, DTPIPE_ERR_FORMAT,
 * or DTPIPE_ERR_MODULE.
 */
int dtpipe_load_xmp(dt_pipe_t *pipe, const char *path);

/*
 * dtpipe_save_xmp - Write the current pipeline history to an XMP sidecar file.
 *
 * path: destination .xmp file path. Created or overwritten.
 *
 * Returns DTPIPE_OK, DTPIPE_ERR_IO, or DTPIPE_ERR_GENERIC.
 */
int dtpipe_save_xmp(dt_pipe_t *pipe, const char *path);

/* -------------------------------------------------------------------------
 * Module introspection
 * -------------------------------------------------------------------------
 */

/*
 * dtpipe_get_module_count - Return the number of available IOP modules.
 *
 * Returns the count, or -1 if the library is not initialized.
 */
int dtpipe_get_module_count(void);

/*
 * dtpipe_get_module_name - Return the operation name of a module by index.
 *
 * index: 0-based module index, must be < dtpipe_get_module_count().
 *
 * The returned pointer is valid until dtpipe_cleanup() is called.
 * Do not free it. Returns NULL if index is out of range.
 */
const char *dtpipe_get_module_name(int index);

#ifdef __cplusplus
}
#endif

#endif /* DTPIPE_H */
