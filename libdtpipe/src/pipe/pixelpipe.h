/*
 * pixelpipe.h - Internal pixelpipe API for libdtpipe
 *
 * Ported from darktable src/develop/pixelpipe_hb.h
 *
 * Stripped of:
 *   - GUI / GTK back-buffer concerns
 *   - Histogram collection
 *   - OpenCL buffer management (deferred to Part 2)
 *   - dt_develop_t coupling (replaced by standalone init)
 */

#pragma once

#include "dtpipe_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Pipeline lifecycle ──────────────────────────────────────────────────── */

/**
 * Initialise a pipeline of the given type.
 * Must be called before any other pixelpipe operation.
 * Returns true on success.
 */
bool dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe,
                                  int32_t width, int32_t height,
                                  int bpp, bool use_downscale);

bool dt_dev_pixelpipe_init_preview(dt_dev_pixelpipe_t *pipe);

bool dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe);

/** Free all resources held by pipe.  Does NOT free the pipe pointer itself. */
void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe);

/* ── Input configuration ─────────────────────────────────────────────────── */

/**
 * Attach a (possibly scaled-down) float-RGBA input buffer to the pipeline.
 * `buf` must remain valid until the pipeline is cleaned up or a new input
 * is set.  The pipeline does NOT take ownership.
 *
 * `iscale` is the ratio  full_resolution / input_resolution  (>= 1.0).
 * `image`  is the image metadata snapshot; a shallow copy is stored.
 */
void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe,
                                float *buf,
                                int width, int height,
                                float iscale,
                                const dt_image_t *image);

/* ── IOP node management ─────────────────────────────────────────────────── */

/**
 * Append a node for `module` to the pipeline's node list.
 * Allocates and zero-fills a dt_dev_pixelpipe_iop_t.
 * Returns the new node pointer, or NULL on allocation failure.
 */
dt_dev_pixelpipe_iop_t *dt_dev_pixelpipe_add_node(dt_dev_pixelpipe_t *pipe,
                                                   dt_iop_module_t *module);

/**
 * Free and remove all nodes from the pipeline.
 * Calls each module's cleanup_pipe() if the function pointer is set.
 */
void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe);

/**
 * Rebuild the node list from `pipe->iop` (a _so_node_t-style list of
 * dt_iop_module_t*).  Called when the module list changes.
 */
void dt_dev_pixelpipe_reset_nodes(dt_dev_pixelpipe_t *pipe);

/* ── Status helpers ──────────────────────────────────────────────────────── */

/** Mark the pipeline as needing a full rebuild. */
static inline void dt_dev_pixelpipe_dirty(dt_dev_pixelpipe_t *pipe)
{
  pipe->status = DT_DEV_PIXELPIPE_DIRTY;
}

/* ── Processing ──────────────────────────────────────────────────────────── */

/**
 * Process the pipeline over a region of interest.
 *
 * @param pipe          The pipeline (must have input set via set_input()).
 * @param x, y          Top-left corner of the output ROI (pipeline coordinates).
 * @param width, height Output dimensions in pixels.
 * @param scale         Output scale relative to full resolution (1.0 = full res).
 *
 * On success the float-RGBA result is stored in pipe->backbuf.
 * Returns false on success, true on error or early shutdown.
 */
bool dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe,
                              int x, int y,
                              int width, int height,
                              float scale);

/**
 * Compute the output dimensions of the pipeline for a given input size.
 * Useful to size the output buffer before calling process().
 *
 * Walks the node list applying modify_roi_out() on each enabled module.
 */
void dt_dev_pixelpipe_get_dimensions(dt_dev_pixelpipe_t *pipe,
                                     int width_in, int height_in,
                                     int *width_out, int *height_out);

#ifdef __cplusplus
}
#endif
