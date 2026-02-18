/*
 * create.h - Internal definition of dt_pipe_t (the public dt_pipe_s)
 *
 * The public dtpipe.h forward-declares `struct dt_pipe_s` as an opaque handle.
 * This header provides the actual struct layout, visible only to internal
 * translation units (create.c, render.c, params.c, export.c, …).
 *
 * Do NOT include this header from any file that is part of the public API
 * surface.  Consumers of libdtpipe only ever see dt_pipe_t*.
 */

#pragma once

#include "dtpipe.h"
#include "dtpipe_internal.h"
#include "pipe/pixelpipe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Module instance list node ───────────────────────────────────────────── */

/*
 * Each registered IOP (from darktable.iop) gets one dt_iop_module_t instance
 * per pipeline.  We keep them in a singly-linked list ordered by iop_order.
 */
typedef struct _module_node_t
{
  dt_iop_module_t       module;   /* owned; embedded (not heap-allocated) */
  struct _module_node_t *next;
} _module_node_t;

/* ── dt_pipe_t (== struct dt_pipe_s) ─────────────────────────────────────── */

struct dt_pipe_s
{
  /*
   * The underlying pixelpipe engine.
   * dt_dev_pixelpipe_t is the struct defined in dtpipe_internal.h that holds
   * the node list, backbuf, ROI machinery, etc.
   */
  dt_dev_pixelpipe_t  pipe;

  /* Source image (borrowed – the caller retains ownership). */
  dt_image_t         *img;

  /*
   * The per-pipeline module instances (dt_iop_module_t).
   * Created in dtpipe_create() and freed in dtpipe_free().
   * pipe.iop points into this list (as a void*).
   */
  _module_node_t     *modules;

  /*
   * Float-RGBA input buffer, allocated by dtpipe_render().
   * NULL until the first render; freed in dtpipe_free().
   */
  float              *input_buf;
  int                 input_width;
  int                 input_height;

  /*
   * Minimal develop object — provides module->dev for IOP modules that need
   * to read image metadata (e.g. rawprepare reads crop/black/white from
   * image_storage; temperature reads WB coefficients from chroma).
   * Populated from img at dtpipe_create() time.
   */
  dt_develop_t        dev;

  /*
   * Snapshot of pipe->pipe.dsc at creation time (image input format).
   * Restored at the start of every render so that format-changing modules
   * (rawprepare, demosaic) see a clean descriptor on each run.
   */
  dt_iop_buffer_dsc_t initial_dsc;
};

/* ── Helpers exposed to other internal TUs ───────────────────────────────── */

/**
 * Look up the dt_iop_module_t for a named operation in a pipeline.
 * Returns NULL if not found.
 */
dt_iop_module_t *dtpipe_find_module(dt_pipe_t *pipe, const char *op);

#ifdef __cplusplus
}
#endif
