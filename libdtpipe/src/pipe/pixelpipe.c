/*
 * pixelpipe.c - Pipeline structures and initialisation for libdtpipe
 *
 * Ported from darktable src/develop/pixelpipe_hb.c
 * This is Part 1 (structures + init/cleanup).  Processing is in Part 2.
 *
 * Changes from original:
 *   - GLib/GList replaced with a plain singly-linked list (_pipe_node_t)
 *   - GUI back-buffer and histogram removed
 *   - OpenCL support stubbed (device enumeration deferred to Part 2)
 *   - dt_develop_t coupling removed; pipeline is fully standalone
 */

#include "pipe/pixelpipe.h"
#include "dtpipe_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MAX
#  define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#  define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ── Internal node list ──────────────────────────────────────────────────── */

/*
 * The pipeline stores its IOP nodes as a singly-linked list.
 * pipe->nodes is cast to _pipe_node_t* at the boundary of this file.
 * All other code treats pipe->nodes as void*.
 */
typedef struct _pipe_node_t
{
  dt_dev_pixelpipe_iop_t  piece;
  struct _pipe_node_t    *next;
} _pipe_node_t;

/* ── Common init helper ──────────────────────────────────────────────────── */

static bool _pixelpipe_init_base(dt_dev_pixelpipe_t *pipe,
                                 dt_dev_pixelpipe_type_t type)
{
  memset(pipe, 0, sizeof(*pipe));

  pipe->type   = type;
  pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  pipe->devid  = DT_DEVICE_CPU;

  dt_atomic_set_int(&pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NO);

  /* Default output: linear Rec. 709 / sRGB, perceptual rendering intent */
  pipe->icc_type   = DT_COLORSPACE_SRGB;
  pipe->icc_intent = DT_INTENT_PERCEPTUAL;

  dt_pthread_mutex_init(&pipe->mutex);
  dt_pthread_mutex_init(&pipe->backbuf_mutex);

  return true;
}

/* ── Public init functions ───────────────────────────────────────────────── */

bool dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe)
{
  return _pixelpipe_init_base(pipe, DT_DEV_PIXELPIPE_FULL);
}

bool dt_dev_pixelpipe_init_preview(dt_dev_pixelpipe_t *pipe)
{
  bool ok = _pixelpipe_init_base(pipe, DT_DEV_PIXELPIPE_PREVIEW);
  if (ok)
    pipe->type = (dt_dev_pixelpipe_type_t)(DT_DEV_PIXELPIPE_PREVIEW
                                           | DT_DEV_PIXELPIPE_FAST);
  return ok;
}

bool dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe,
                                  int32_t width, int32_t height,
                                  int bpp, bool use_downscale)
{
  (void)width; (void)height; (void)bpp; (void)use_downscale;
  bool ok = _pixelpipe_init_base(pipe, DT_DEV_PIXELPIPE_EXPORT);
  return ok;
}

/* ── Cleanup ─────────────────────────────────────────────────────────────── */

void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe)
{
  _pipe_node_t *node = (_pipe_node_t *)pipe->nodes;
  while (node)
  {
    _pipe_node_t *next = node->next;

    dt_dev_pixelpipe_iop_t *piece = &node->piece;

    /* Free per-pipe module data if the module provided a cleanup function. */
    /* cleanup_pipe() is added in Part 2 when module function tables exist. */

    /* Free blending data */
    free(piece->blendop_data);
    piece->blendop_data = NULL;

    /* Free per-pipe private data */
    free(piece->data);
    piece->data = NULL;

    free(node);
    node = next;
  }
  pipe->nodes = NULL;
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  dt_pthread_mutex_lock(&pipe->mutex);

  dt_dev_pixelpipe_cleanup_nodes(pipe);

  /* Release back-buffer if any */
  free(pipe->backbuf);
  pipe->backbuf      = NULL;
  pipe->backbuf_size = 0;

  /* Release input buffer — we do NOT own it (set_input is borrow-only) */
  pipe->input = NULL;

  dt_pthread_mutex_unlock(&pipe->mutex);

  dt_pthread_mutex_destroy(&pipe->mutex);
  dt_pthread_mutex_destroy(&pipe->backbuf_mutex);
}

/* ── Input ───────────────────────────────────────────────────────────────── */

void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe,
                                float *buf,
                                int width, int height,
                                float iscale,
                                const dt_image_t *image)
{
  pipe->input   = buf;
  pipe->iwidth  = width;
  pipe->iheight = height;
  pipe->iscale  = iscale;

  if (image)
    pipe->image = *image; /* shallow copy of metadata */

  pipe->input_changed = true;
  pipe->status        = DT_DEV_PIXELPIPE_DIRTY;
}

/* ── Node management ─────────────────────────────────────────────────────── */

dt_dev_pixelpipe_iop_t *dt_dev_pixelpipe_add_node(dt_dev_pixelpipe_t *pipe,
                                                   dt_iop_module_t *module)
{
  _pipe_node_t *node = (_pipe_node_t *)calloc(1, sizeof(_pipe_node_t));
  if (!node)
  {
    fprintf(stderr, "[pixelpipe] failed to allocate node for module '%s'\n",
            module ? module->op : "(null)");
    return NULL;
  }

  dt_dev_pixelpipe_iop_t *piece = &node->piece;
  piece->module = module;
  piece->pipe   = pipe;
  piece->enabled = module ? module->enabled : false;

  /* Initialise ROI scales to 1:1 */
  piece->iscale = pipe->iscale > 0.0f ? pipe->iscale : 1.0f;

  /* Append to the end of the list so nodes are in insertion order. */
  if (!pipe->nodes)
  {
    pipe->nodes = node;
  }
  else
  {
    _pipe_node_t *tail = (_pipe_node_t *)pipe->nodes;
    while (tail->next)
      tail = tail->next;
    tail->next = node;
  }

  return piece;
}

void dt_dev_pixelpipe_reset_nodes(dt_dev_pixelpipe_t *pipe)
{
  /* Free existing nodes first */
  dt_dev_pixelpipe_cleanup_nodes(pipe);

  /*
   * pipe->iop is a _so_node_t* list of dt_iop_module_t* in iop_order.
   * We walk it and create one pipeline node per module.
   *
   * The list type matches the _so_node_t used in init.c; we replicate the
   * inline struct here to avoid a cross-file dependency on the private type.
   */
  typedef struct _mod_node_t {
    dt_iop_module_t    *module;
    struct _mod_node_t *next;
  } _mod_node_t;

  _mod_node_t *mod = (_mod_node_t *)pipe->iop;
  while (mod)
  {
    dt_dev_pixelpipe_add_node(pipe, mod->module);
    mod = mod->next;
  }
}

/* ── Part 2: Processing ──────────────────────────────────────────────────── */

/*
 * Processing loop ported from darktable src/develop/pixelpipe_hb.c.
 *
 * Stripped of:
 *   - dt_develop_t coupling (replaced by pipe-only API)
 *   - OpenCL paths (CPU-only; can be added later behind DTPIPE_HAVE_OPENCL)
 *   - Histogram collection and colorpicker callbacks
 *   - GUI signal emissions (DT_CONTROL_SIGNAL_RAISE)
 *   - Backbuffer management for screen rendering
 *   - Benchmark/dump-pfm instrumentation
 *   - Fast-pipe GUI module group checks
 *   - Multi-pipe cache hinting (dt_dev_pixelpipe_important_cacheline)
 *
 * Retained:
 *   - ROI propagation (modify_roi_in / modify_roi_out)
 *   - Colorspace transforms around module process()
 *   - Tiling dispatch (process_tiling when image is too large)
 *   - Blending (no-op stub today; wired for future Phase 4+ blend)
 *   - Shutdown / abort checking throughout
 */

/* ── dt_pixelpipe_flow_t (local) ─────────────────────────────────────────── */

typedef enum _dt_pixelpipe_flow_t
{
  PIXELPIPE_FLOW_NONE                  = 0,
  PIXELPIPE_FLOW_PROCESSED_ON_CPU      = 1 << 3,
  PIXELPIPE_FLOW_PROCESSED_WITH_TILING = 1 << 5,
  PIXELPIPE_FLOW_BLENDED_ON_CPU        = 1 << 6,
} _dt_pixelpipe_flow_t;

/* ── _skip_piece ─────────────────────────────────────────────────────────── */

static inline bool _skip_piece(const dt_dev_pixelpipe_iop_t *piece)
{
  if(!piece->enabled)
    return true;
  if(piece->module && piece->module->iop_order == INT_MAX)
    return true;
  return false;
}

/* ── _transform_for_blend ────────────────────────────────────────────────── */
/*
 * Returns true if the blending step needs a colorspace transform.
 * Declared early so _process_on_cpu can call it.
 */
static inline bool _transform_for_blend(const dt_iop_module_t *const self,
                                         const dt_dev_pixelpipe_iop_t *const piece)
{
  const dt_develop_blend_params_t *const d = piece->blendop_data;
  if(d)
  {
    if((self->flags ? (self->flags() & IOP_FLAGS_SUPPORTS_BLENDING) : 0)
       && (d->mask_mode != DEVELOP_MASK_DISABLED))
    {
      return true;
    }
  }
  return false;
}

/* ── _process_on_cpu ─────────────────────────────────────────────────────── */
/*
 * Execute a single IOP node on the CPU.
 *
 * Handles:
 *   1. Colorspace transform of input to module's required input space
 *   2. Tiling vs. full-buffer dispatch
 *   3. Colorspace transform for blending (if blending is active)
 *   4. Blending
 *
 * Returns true if the pipeline should stop (shutdown or error).
 */
static bool _process_on_cpu(dt_dev_pixelpipe_t *pipe,
                             float *input,
                             dt_iop_buffer_dsc_t *input_format,
                             const dt_iop_roi_t *roi_in,
                             void **output,
                             dt_iop_buffer_dsc_t **out_format,
                             const dt_iop_roi_t *roi_out,
                             dt_iop_module_t *module,
                             dt_dev_pixelpipe_iop_t *piece,
                             dt_develop_tiling_t *tiling)
{
  if(dt_pipe_shutdown(pipe))
    return true;

  /* Alignment sanity check */
  if(!dt_check_aligned(input) || !dt_check_aligned(*output))
  {
    fprintf(stderr, "[pixelpipe] non-aligned buffers for module '%s' – aborting\n",
            module ? module->op : "(null)");
    return false; /* treat as non-fatal to avoid infinite restart loops */
  }

  /* Fetch working profile (NULL for RAW – no colour conversion) */
  const dt_iop_order_iccprofile_info_t *const work_profile =
    (input_format->cst != IOP_CS_RAW)
      ? dt_ioppr_get_pipe_work_profile_info(pipe)
      : NULL;

  const int cst_from = input_format->cst;
  const int cst_to   = module->input_colorspace
                         ? module->input_colorspace(module, pipe, piece)
                         : cst_from;
  const int cst_out  = module->output_colorspace
                         ? module->output_colorspace(module, pipe, piece)
                         : cst_to;

  /* Transform input buffer into the module's required colorspace */
  if(cst_from != cst_to)
  {
    dt_ioppr_transform_image_colorspace(module, input, input,
                                        roi_in->width, roi_in->height,
                                        cst_from, cst_to, &input_format->cst,
                                        work_profile);
  }

  if(dt_pipe_shutdown(pipe))
    return true;

  /* Sizes for tiling decision */
  const size_t in_bpp  = dt_iop_buffer_dsc_to_bpp(input_format);
  const size_t out_bpp = dt_iop_buffer_dsc_to_bpp(*out_format);
  const size_t m_bpp   = MAX(in_bpp, out_bpp);
  const size_t m_width  = (size_t)MAX(roi_in->width,  roi_out->width);
  const size_t m_height = (size_t)MAX(roi_in->height, roi_out->height);

  const bool fitting = dt_tiling_piece_fits_host_memory(
    piece, m_width, m_height, m_bpp, tiling->factor, tiling->overhead);

  /* Dispatch: tiled or full-buffer */
  if(!fitting && piece->process_tiling_ready && module->process_tiling)
  {
    module->process_tiling(module, piece, input, *output, roi_in, roi_out,
                           (int)in_bpp);
  }
  else
  {
    /* Prefer process() then fall back to process_plain() */
    if(module->process)
      module->process(module, piece, input, *output, roi_in, roi_out);
    else if(module->process_plain)
      module->process_plain(module, piece, input, *output, roi_in, roi_out);
    else
    {
      fprintf(stderr, "[pixelpipe] module '%s' has no process function\n",
              module->op);
      return false;
    }
  }

  if(dt_pipe_shutdown(pipe))
    return true;

  /* Save output colorspace */
  if(module->output_colorspace)
    pipe->dsc.cst = (dt_iop_colorspace_type_t)cst_out;

  /* Transform for blending if needed */
  const dt_iop_colorspace_type_t blend_cst =
    dt_develop_blend_colorspace(piece, pipe->dsc.cst);

  if(_transform_for_blend(module, piece))
  {
    dt_ioppr_transform_image_colorspace(module, input, input,
                                        roi_in->width, roi_in->height,
                                        input_format->cst, (int)blend_cst,
                                        &input_format->cst,
                                        work_profile);
    dt_ioppr_transform_image_colorspace(module, *output, *output,
                                        roi_out->width, roi_out->height,
                                        (int)pipe->dsc.cst, (int)blend_cst,
                                        (int *)&pipe->dsc.cst,
                                        work_profile);
  }

  if(dt_pipe_shutdown(pipe))
    return true;

  /* Apply blending (no-op stub until Phase 4+ blend is wired up) */
  dt_develop_blend_process(module, piece, input, *output, roi_in, roi_out);

  return dt_pipe_shutdown(pipe);
}

/* ── _process_rec ────────────────────────────────────────────────────────── */
/*
 * Recursive processing helper.  Walks the node list backward (from the last
 * module toward the first), then processes modules on the way back up.
 *
 * Parameters
 *   output      – address of the output buffer pointer (filled on return)
 *   out_format  – address of the output buffer descriptor pointer
 *   roi_out     – desired output region of interest
 *   node        – the current (last) node in our forward list being processed
 *                 (NULL means we're at the very beginning: import input)
 *   pos         – 0-based position index (used for cache hashing)
 *
 * Returns true on error / shutdown.
 */
static bool _process_rec(dt_dev_pixelpipe_t *pipe,
                         void **output,
                         dt_iop_buffer_dsc_t **out_format,
                         const dt_iop_roi_t *roi_out,
                         _pipe_node_t *node,
                         int pos)
{
  if(dt_pipe_shutdown(pipe))
    return true;

  dt_iop_roi_t roi_in = *roi_out;

  /* ── Base case: no more nodes → import raw input buffer ──────────────── */
  if(!node)
  {
    if(dt_pipe_shutdown(pipe))
      return true;

    const size_t bpp     = dt_iop_buffer_dsc_to_bpp(*out_format);
    const size_t bufsize = (size_t)bpp * roi_out->width * roi_out->height;

    /* Full 1:1, unscaled pass-through */
    if(roi_out->scale == 1.0f
       && roi_out->x == 0 && roi_out->y == 0
       && pipe->iwidth  == roi_out->width
       && pipe->iheight == roi_out->height
       && dt_check_aligned(pipe->input))
    {
      *output = pipe->input;
    }
    else
    {
      /* Allocate an output buffer and fill it */
      if(!dt_dev_pixelpipe_cache_get(pipe, DT_INVALID_HASH, bufsize,
                                     output, out_format, NULL, false))
        return true;

      if(roi_out->scale == 1.0f)
      {
        /* 1:1 copy with ROI offset */
        const int in_x = MAX(roi_out->x, 0);
        const int in_y = MAX(roi_out->y, 0);
        const size_t cp_width = bpp * (size_t)MAX(0,
          MIN(roi_out->width, pipe->iwidth - in_x));
        const size_t o_width = bpp * (size_t)roi_out->width;

        DT_OMP_FOR()
        for(int row = 0; row < roi_out->height; row++)
        {
          uint8_t *out_row = (uint8_t *)*output + bpp * (size_t)row * roi_out->width;
          if((row + in_y) < pipe->iheight)
          {
            memcpy(out_row,
                   (uint8_t *)pipe->input
                     + bpp * (size_t)(in_x + (in_y + row) * pipe->iwidth),
                   cp_width);
            if(cp_width < o_width)
              memset(out_row + cp_width, 0, o_width - cp_width);
          }
          else
          {
            memset(out_row, 0, o_width);
          }
        }
      }
      else
      {
        /* Scale: clip-and-zoom */
        roi_in.x      /= roi_out->scale;
        roi_in.y      /= roi_out->scale;
        roi_in.width   = pipe->iwidth;
        roi_in.height  = pipe->iheight;
        roi_in.scale   = 1.0f;

        if(bpp == 4 * sizeof(float) && dt_check_aligned(pipe->input))
          dt_iop_clip_and_zoom((float *)*output, pipe->input, roi_out, &roi_in);
        else
          memset(*output, 0, bufsize);
      }
    }

    return dt_pipe_shutdown(pipe);
  }

  /* ── Recursive case: process this node ───────────────────────────────── */

  dt_dev_pixelpipe_iop_t *piece  = &node->piece;
  dt_iop_module_t        *module = piece->module;

  /* Skip disabled / sentinel modules: recurse with this node's predecessor. */
  if(_skip_piece(piece))
  {
    /* Find the predecessor of this node */
    _pipe_node_t *prev_skip = NULL;
    {
      _pipe_node_t *cur = (_pipe_node_t *)pipe->nodes;
      while(cur && cur->next != node)
        cur = cur->next;
      prev_skip = cur; /* NULL when node is the head */
    }
    return _process_rec(pipe, output, out_format, roi_out, prev_skip, pos - 1);
  }

  /* Determine output format */
  const size_t bpp     = dt_iop_buffer_dsc_to_bpp(*out_format);
  const size_t bufsize = (size_t)bpp * roi_out->width * roi_out->height;

  /* Check shutdown / cache */
  if(dt_pipe_shutdown(pipe))
    return true;

  /* Compute the ROI that this module requires from its predecessor */
  if(module->modify_roi_in)
    module->modify_roi_in(module, piece, roi_out, &roi_in);

  piece->processed_roi_in  = roi_in;
  piece->processed_roi_out = *roi_out;

  /* Recurse to obtain input */
  void *input        = NULL;
  dt_iop_buffer_dsc_t _input_format = { 0 };
  dt_iop_buffer_dsc_t *input_format  = &_input_format;

  /* Find the predecessor node in the forward list */
  _pipe_node_t *prev = NULL;
  {
    _pipe_node_t *cur = (_pipe_node_t *)pipe->nodes;
    while(cur && cur->next != node)
      cur = cur->next;
    prev = cur; /* NULL if node is the head */
  }

  if(_process_rec(pipe, &input, &input_format, &roi_in, prev, pos - 1))
    return true;

  const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);

  /* Propagate buffer descriptor through the piece */
  piece->dsc_out = piece->dsc_in = *input_format;

  if(module->output_format)
    module->output_format(module, pipe, piece, &piece->dsc_out);

  **out_format = pipe->dsc = piece->dsc_out;

  /* Allocate output buffer */
  if(dt_pipe_shutdown(pipe))
    return true;

  if(!dt_dev_pixelpipe_cache_get(pipe, DT_INVALID_HASH, bufsize,
                                  output, out_format, module, false))
    return true;

  if(dt_pipe_shutdown(pipe))
    return true;

  /* ── Mask-display bypass ─────────────────────────────────────────────── */
  /* If the pipe is in mask-display mode and this module does not distort,
     skip it and just copy the input buffer through. */
  if(pipe->mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE
     && !(module->operation_tags
          ? (module->operation_tags() & IOP_TAG_DISTORT) : 0)
     && (in_bpp == dt_iop_buffer_dsc_to_bpp(*out_format))
     && memcmp(&roi_in, roi_out, sizeof(dt_iop_roi_t)) == 0)
  {
    **out_format = pipe->dsc = piece->dsc_out = piece->dsc_in;
    dt_iop_image_copy_by_size(*output, input,
                              roi_out->width, roi_out->height,
                              (int)(dt_iop_buffer_dsc_to_bpp(*out_format)
                                    / sizeof(float)));
    return false;
  }

  /* ── Tiling requirements ─────────────────────────────────────────────── */
  dt_develop_tiling_t tiling = { 0 };
  /* Use 0 as "not set" sentinel for factor_cl/maxbuf_cl since they are
     float and size_t respectively – the callback sets them if non-zero. */

  if(module->tiling_callback)
    module->tiling_callback(module, piece, &roi_in, roi_out, &tiling);
  else
    dt_iop_default_tiling_callback(module, piece, &roi_in, roi_out, &tiling);

  if(tiling.factor_cl == 0.0f) tiling.factor_cl = tiling.factor;
  if(tiling.maxbuf_cl == 0)    tiling.maxbuf_cl  = tiling.maxbuf;

  /* Aggregate blendop tiling requirements */
  if(piece->blendop_data
     && ((dt_develop_blend_params_t *)piece->blendop_data)->mask_mode
          != DEVELOP_MASK_DISABLED)
  {
    dt_develop_tiling_t tiling_blend = { 0 };
    tiling_callback_blendop(module, piece, &roi_in, roi_out, &tiling_blend);
    tiling.factor    = MAX(tiling.factor,    tiling_blend.factor);
    tiling.factor_cl = MAX(tiling.factor_cl, tiling_blend.factor);
    tiling.maxbuf    = MAX(tiling.maxbuf,    tiling_blend.maxbuf);
    tiling.overhead  = MAX(tiling.overhead,  tiling_blend.overhead);
    tiling.overlap   = MAX(tiling.overlap,   tiling_blend.overlap);
  }

  piece->module->position = pos;

  /* ── CPU processing ──────────────────────────────────────────────────── */
  if(_process_on_cpu(pipe, (float *)input, input_format, &roi_in,
                     output, out_format, roi_out,
                     module, piece, &tiling))
    return true;

  /* Persist final format into the piece */
  **out_format = piece->dsc_out = pipe->dsc;

  return dt_pipe_shutdown(pipe);
}

/* ── dt_dev_pixelpipe_process ────────────────────────────────────────────── */

/**
 * Main entry point: process the entire pipeline over the given ROI.
 *
 * @param pipe    Pipeline to process.
 * @param x, y   Top-left origin of the output ROI (in pipeline coordinates).
 * @param width, height  Output dimensions.
 * @param scale  Ratio of output to full-resolution pixels (1.0 = full res).
 *
 * On success the result is in pipe->backbuf (float RGBA, aligned).
 * Returns false on success, true on error or shutdown.
 */
bool dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe,
                              int x, int y,
                              int width, int height,
                              float scale)
{
  pipe->processing = true;
  dt_atomic_set_int(&pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NO);

  /* Set up the requested output ROI */
  dt_iop_roi_t roi = { x, y, width, height, scale };
  pipe->final_width  = width;
  pipe->final_height = height;

  /* Reset transient pipeline state */
  pipe->mask_display  = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  pipe->bypass_blendif = false;
  pipe->opencl_error   = false;

  void *buf = NULL;
  dt_iop_buffer_dsc_t _out_format = { 0 };
  dt_iop_buffer_dsc_t *out_format  = &_out_format;

  /* Initial output format: float RGBA (4 channels) */
  _out_format.channels = 4;
  _out_format.datatype = TYPE_FLOAT;
  _out_format.cst      = IOP_CS_RGB;

  /* Walk list to find the last (tail) node */
  _pipe_node_t *tail = (_pipe_node_t *)pipe->nodes;
  int pos = 0;
  if(tail)
  {
    while(tail->next)
    {
      tail = tail->next;
      pos++;
    }
  }

  /* Run the recursive processing engine */
  const bool err = _process_rec(pipe, &buf, &out_format, &roi, tail, pos);

  if(err)
  {
    pipe->processing = false;
    return true;
  }

  /* Store result in backbuf */
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);

  const size_t bpp     = dt_iop_buffer_dsc_to_bpp(out_format);
  const size_t newsize = (size_t)width * height * bpp;

  if(pipe->backbuf == NULL || pipe->backbuf_size != newsize)
  {
    free(pipe->backbuf);
    pipe->backbuf = dt_alloc_aligned(newsize);
    pipe->backbuf_size = newsize;
  }

  if(pipe->backbuf && buf)
  {
    memcpy(pipe->backbuf, buf, newsize);
    /* If buf was the raw input pointer (the 1:1 pass-through case), don't
       free it – the caller owns it.  Otherwise free the allocated buffer. */
    if(buf != pipe->input)
      dt_free_align(buf);
  }

  pipe->backbuf_width  = width;
  pipe->backbuf_height = height;
  pipe->dsc            = *out_format;

  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);

  pipe->status     = DT_DEV_PIXELPIPE_VALID;
  pipe->processing = false;
  return false;
}

/* ── dt_dev_pixelpipe_get_dimensions ─────────────────────────────────────── */

/**
 * Compute the output dimensions of the pipeline for a given input size.
 * Walks the node list forward, calling modify_roi_out() on each active module.
 */
void dt_dev_pixelpipe_get_dimensions(dt_dev_pixelpipe_t *pipe,
                                     int width_in, int height_in,
                                     int *width_out, int *height_out)
{
  dt_pthread_mutex_lock(&pipe->mutex);

  dt_iop_roi_t roi_in  = { 0, 0, width_in, height_in, 1.0f };
  dt_iop_roi_t roi_out = roi_in;

  _pipe_node_t *node = (_pipe_node_t *)pipe->nodes;
  while(node)
  {
    dt_dev_pixelpipe_iop_t *piece  = &node->piece;
    dt_iop_module_t        *module = piece->module;

    piece->buf_in = roi_in;

    if(!_skip_piece(piece))
    {
      if(module->modify_roi_out)
        module->modify_roi_out(module, piece, &roi_out, &roi_in);
      else
        dt_iop_default_modify_roi_out(module, piece, &roi_out, &roi_in);
    }
    else
    {
      roi_out = roi_in;
    }

    piece->buf_out = roi_out;
    roi_in = roi_out;
    node = node->next;
  }

  *width_out  = roi_out.width;
  *height_out = roi_out.height;

  dt_pthread_mutex_unlock(&pipe->mutex);
}
