/*
 * create.c - Pipeline creation and destruction for libdtpipe
 *
 * Implements dtpipe_create() and dtpipe_free() (public API).
 *
 * Design notes
 * ────────────
 * A dt_pipe_t wraps a dt_dev_pixelpipe_t (the internal pixelpipe engine) plus
 * a set of per-pipeline dt_iop_module_t instances — one per registered IOP,
 * ordered by the darktable v5.0 iop_order list.
 *
 * Because no IOP modules are compiled in during the early phases of the
 * project (the registry in init.c is intentionally empty until Phase 2
 * modules are added), this file works correctly with zero modules: it
 * creates an empty pipeline that will simply pass the input through.
 *
 * The raw pixel buffer stored in dt_image_t is a Bayer mosaic
 * (uint16 or float) that cannot be used directly as float-RGBA input to the
 * pixelpipe.  The actual float-RGBA input buffer is therefore created lazily
 * at render time by the rawprepare / demosaic modules, or — before those are
 * compiled in — by a minimal unpack helper in render.c.  dtpipe_create() sets
 * pipe->input_buf = NULL and leaves the demosaic step to the render path.
 *
 * Default enabled state
 * ─────────────────────
 * When no XMP/history has been loaded, the pipeline defaults to:
 *   Enabled  : rawprepare, demosaic, colorin, colorout, exposure
 *   Disabled : everything else (creative modules off by default)
 *
 * The caller can change individual module states with dtpipe_enable_module().
 */

#include "pipe/create.h"
#include "pipe/params.h"
#include "common/iop_order.h"
#include "dtpipe.h"
#include "dtpipe_internal.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Modules that are enabled by default ──────────────────────────────────── */

static const char *const _default_enabled[] = {
  "rawprepare",
  "demosaic",
  "colorin",
  "exposure",
  "colorout",
  NULL
};

static bool _is_default_enabled(const char *op)
{
  for(int i = 0; _default_enabled[i]; i++)
    if(strncmp(op, _default_enabled[i], 20) == 0)
      return true;
  return false;
}

/* ── SO node list types (mirrors init.c) ─────────────────────────────────── */

/*
 * darktable.iop is a void* that points to the head of a singly-linked list
 * of _so_node_t nodes (defined in init.c).  We redeclare the layout here so
 * we can walk it without exposing it in a shared header.
 */
typedef struct _so_node_t
{
  dt_iop_module_so_t  *so;
  struct _so_node_t   *next;
} _so_node_t;

/* ── _build_module_list ───────────────────────────────────────────────────── */

/*
 * Walk darktable.iop and the v5.0 iop_order list to build an ordered list of
 * dt_iop_module_t instances for this pipeline.
 *
 * Strategy:
 *   1. Get the canonical v5.0 order list (dt_iop_order_list_t).
 *   2. For each entry in that order list, find the matching so in darktable.iop.
 *   3. Allocate a _module_node_t, fill in the dt_iop_module_t fields, and
 *      assign iop_order from the order list.
 *   4. Modules in darktable.iop that are NOT in the order list get iop_order
 *      INT_MAX so they sort last and are skipped by the pipeline engine.
 *
 * Returns the head of the module list, or NULL if darktable.iop is empty or
 * memory allocation fails.
 */
static _module_node_t *_build_module_list(dt_develop_t *dev)
{
  /* Get the canonical v5.0 order list */
  dt_iop_order_list_t order_list =
      dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_V50);

  _module_node_t *head = NULL;
  _module_node_t *tail = NULL;

  /*
   * Walk the order list.  For each entry, search darktable.iop for a
   * matching so.  This gives us modules in pipeline order.
   */
  for(dt_iop_order_list_node_t *oe = order_list; oe; oe = oe->next)
  {
    const char *op = oe->entry.operation;

    /* Find matching so in darktable.iop */
    dt_iop_module_so_t *so = NULL;
    for(_so_node_t *sn = (_so_node_t *)darktable.iop; sn; sn = sn->next)
    {
      if(strncmp(sn->so->op, op, sizeof(sn->so->op)) == 0)
      {
        so = sn->so;
        break;
      }
    }

    /* Skip order entries that have no compiled-in module */
    if(!so)
      continue;

    /* Allocate the module instance node */
    _module_node_t *node = (_module_node_t *)calloc(1, sizeof(_module_node_t));
    if(!node)
    {
      fprintf(stderr, "[dtpipe/create] out of memory allocating module '%s'\n",
              op);
      /* Continue: partial list is better than nothing */
      continue;
    }

    dt_iop_module_t *m = &node->module;

    /* Fill identity fields */
    strncpy(m->op, op, sizeof(m->op) - 1);
    m->so          = so;
    m->iop_order   = oe->entry.o.iop_order;
    m->instance    = oe->entry.instance;
    m->multi_priority = 0;
    strncpy(m->multi_name, op, sizeof(m->multi_name) - 1);

    /* Mirror process function pointers from the so */
    m->process_plain = so->process_plain;

    /* Mirror flags/operation_tags from so */
    m->flags           = so->flags;
    m->operation_tags  = so->operation_tags;

    /* Mirror lifecycle callbacks from the so */
    m->init_pipe      = so->init_pipe;
    m->cleanup_pipe   = so->cleanup_pipe;
    m->commit_params  = so->commit_params;

    /* Mirror colorspace/format/ROI callbacks from the so */
    m->input_colorspace  = so->input_colorspace;
    m->output_colorspace = so->output_colorspace;
    m->output_format     = so->output_format;
    m->modify_roi_in     = so->modify_roi_in;
    m->modify_roi_out    = so->modify_roi_out;

    /* Default enabled state */
    m->default_enabled = _is_default_enabled(op);
    m->enabled         = m->default_enabled;

    /* Allocate zero-initialised params buffer sized from descriptor table.
     * Modules without a descriptor table get no buffer (params stays NULL). */
    size_t psz = dtpipe_params_struct_size(op);
    if(psz > 0)
    {
      m->params         = (dt_iop_params_t *)calloc(1, psz);
      m->default_params = (dt_iop_params_t *)calloc(1, psz);
      m->params_size    = (int32_t)psz;
    }

    /* Set module->dev so that init() can read image metadata (crop extents,
     * black/white levels, WB coefficients, etc.). */
    m->dev = (void *)dev;

    /* Call so->init() if available — populates default params and any
     * per-instance data.  Must run after params buffer is allocated. */
    if(so->init)
      so->init(m);

    /* Initialise the gui_lock even though we never use it (code may lock it) */
    dt_pthread_mutex_init(&m->gui_lock);

    /* Append to the ordered list */
    if(!head)
    {
      head = node;
      tail = node;
    }
    else
    {
      tail->next = node;
      tail       = node;
    }
  }

  dt_ioppr_iop_order_list_free(order_list);
  return head;
}

/* ── _free_module_list ────────────────────────────────────────────────────── */

static void _free_module_list(_module_node_t *head)
{
  _module_node_t *node = head;
  while(node)
  {
    _module_node_t *next = node->next;
    dt_iop_module_t *m   = &node->module;

    /* Free owned parameter blocks */
    free(m->params);
    m->params = NULL;
    free(m->default_params);
    m->default_params = NULL;

    /* Free blending parameter blocks */
    free(m->blend_params);
    m->blend_params = NULL;
    free(m->default_blendop_params);
    m->default_blendop_params = NULL;

    /* Free per-instance data (allocated by init() – not used in headless) */
    free(m->data);
    m->data = NULL;

    dt_pthread_mutex_destroy(&m->gui_lock);

    free(node);
    node = next;
  }
}

/* ── _cleanup_pipe_nodes ─────────────────────────────────────────────────── */

/*
 * Walk the pixelpipe nodes and call each module's cleanup function for the
 * per-pipe data (piece->data).  This is separate from the module instance
 * cleanup above.
 *
 * In the current stub state, modules don't provide a cleanup_pipe() function
 * pointer (it lives on dt_iop_module_so_t; we can add it when needed).
 * The call to dt_dev_pixelpipe_cleanup_nodes() handles freeing piece->data
 * for now.
 */
static void _cleanup_pipe_nodes(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cleanup_nodes(pipe);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

dt_pipe_t *dtpipe_create(dt_image_t *img)
{
  if(!img)
  {
    fprintf(stderr, "[dtpipe_create] img is NULL\n");
    return NULL;
  }

  /* Allocate the wrapper struct */
  dt_pipe_t *pipe = (dt_pipe_t *)calloc(1, sizeof(dt_pipe_t));
  if(!pipe)
  {
    fprintf(stderr, "[dtpipe_create] out of memory\n");
    return NULL;
  }

  /* Store image reference (borrowed) */
  pipe->img = img;

  /* Populate the minimal develop object so IOP modules can access image
   * metadata through module->dev (e.g. rawprepare reads crop/black/white,
   * temperature reads WB coefficients). */
  memset(&pipe->dev, 0, sizeof(pipe->dev));
  pipe->dev.image_storage = *img; /* shallow copy; pixel buffer is NOT owned */
  /* wb_coeffs default: neutral 1.0 for all channels */
  pipe->dev.chroma.wb_coeffs[0] = 1.0f;
  pipe->dev.chroma.wb_coeffs[1] = 1.0f;
  pipe->dev.chroma.wb_coeffs[2] = 1.0f;
  pipe->dev.chroma.wb_coeffs[3] = 1.0f;
  /* Populate as-shot WB from the image's raw WB coefficients if available */
  for(int i = 0; i < 4; i++)
  {
    pipe->dev.chroma.as_shot[i] = img->wb_coeffs[i] > 0.0f ? img->wb_coeffs[i] : 1.0f;
    pipe->dev.chroma.wb_coeffs[i] = pipe->dev.chroma.as_shot[i];
    pipe->dev.chroma.D65coeffs[i] = 1.0f;
  }

  /* Initialise the underlying pixelpipe engine */
  if(!dt_dev_pixelpipe_init(&pipe->pipe))
  {
    fprintf(stderr, "[dtpipe_create] pixelpipe init failed\n");
    free(pipe);
    return NULL;
  }

  /* Record input dimensions (full-resolution; scaled-down input is set at
     render time after demosaicing) */
  pipe->input_width  = img->final_width  > 0 ? img->final_width  : img->width;
  pipe->input_height = img->final_height > 0 ? img->final_height : img->height;

  /* Copy image metadata snapshot into the pixelpipe */
  pipe->pipe.image = *img; /* shallow copy — pixels pointer is NOT owned here */

  /* Build the ordered module instance list.  Pass &pipe->dev so that
   * module->dev is set before so->init() is called (rawprepare reads
   * crop/black/white from dev->image_storage in its init()). */
  pipe->modules = _build_module_list(&pipe->dev);

  /*
   * Point pipe.iop at the module list so the node-building code can iterate
   * it.  The pixelpipe engine uses pipe.iop as a void* and casts it to the
   * same _module_node_t* layout it expects (the layout matches _mod_node_t in
   * pixelpipe.c because both embed dt_iop_module_t* as the first field of a
   * singly-linked list node — but here the module is embedded, not a pointer).
   *
   * NOTE: dt_dev_pixelpipe_reset_nodes() in pixelpipe.c uses a local
   * _mod_node_t that holds a dt_iop_module_t *, not an embedded struct.
   * We therefore do NOT call reset_nodes() here; instead we build the
   * pipeline nodes directly below using dt_dev_pixelpipe_add_node().
   */

  /* Build pixelpipe nodes from the module instance list */
  for(_module_node_t *mn = pipe->modules; mn; mn = mn->next)
  {
    dt_dev_pixelpipe_iop_t *piece =
        dt_dev_pixelpipe_add_node(&pipe->pipe, &mn->module);
    if(!piece)
    {
      fprintf(stderr,
              "[dtpipe_create] failed to add node for module '%s'\n",
              mn->module.op);
      /* Non-fatal: continue building remaining nodes */
      continue;
    }

    /* Call init_pipe() to allocate piece->data for this module. */
    if(mn->module.init_pipe)
      mn->module.init_pipe(&mn->module, &pipe->pipe, piece);
  }

  /* Set pipe.iop for any code that walks it (future use) */
  pipe->pipe.iop = (void *)pipe->modules;

  /* ── Initialize pipe->dsc from image metadata ─────────────────────────
   * The pipeline's buffer descriptor (dsc) must reflect the actual input
   * format before any module runs.  For raw images this is 1-channel Bayer;
   * the demosaic module's output_format() will update it to 4-channel RGB.
   * Without this, the base case in _process_rec() produces wrong buffer
   * sizes for rawprepare and demosaic.
   */
  const gboolean is_raw = dt_image_is_raw(img);
  if(is_raw)
  {
    pipe->pipe.dsc.channels = 1;
    pipe->pipe.dsc.datatype = TYPE_FLOAT;
    pipe->pipe.dsc.cst      = IOP_CS_RAW;
    pipe->pipe.dsc.filters  = img->buf_dsc.filters;
    for(int k = 0; k < 6; k++)
      for(int j = 0; j < 6; j++)
        pipe->pipe.dsc.xtrans[k][j] = img->buf_dsc.xtrans[k][j];
    /* Seed processed_maximum from image white point */
    const float wp = img->raw_white_point > 0 ? (float)img->raw_white_point : 65535.0f;
    for(int k = 0; k < 3; k++)
      pipe->pipe.dsc.processed_maximum[k] = 1.0f / wp; /* rawprepare normalises to [0,1] */
  }
  else
  {
    pipe->pipe.dsc.channels = 4;
    pipe->pipe.dsc.datatype = TYPE_FLOAT;
    pipe->pipe.dsc.cst      = IOP_CS_RGB;
    for(int k = 0; k < 3; k++)
      pipe->pipe.dsc.processed_maximum[k] = 1.0f;
  }

  /* Save the initial dsc so each render can reset pipe->pipe.dsc before
     processing.  Format-changing modules (rawprepare, demosaic) mutate
     pipe->pipe.dsc in-place; without a reset the second render sees the
     post-demosaic format (4-ch RGB) as the input format. */
  pipe->initial_dsc = pipe->pipe.dsc;

  return pipe;
}

void dtpipe_free(dt_pipe_t *pipe)
{
  if(!pipe)
    return;

  /* Shutdown any in-progress processing */
  dt_atomic_set_int(&pipe->pipe.shutdown, DT_DEV_PIXELPIPE_STOP_NODES);

  /* Free pixelpipe nodes (calls module cleanup_pipe where available) */
  _cleanup_pipe_nodes(&pipe->pipe);

  /* Release the pixelpipe engine resources (mutex, backbuf, etc.) */
  dt_dev_pixelpipe_cleanup(&pipe->pipe);

  /* Free the input buffer if we allocated one */
  free(pipe->input_buf);
  pipe->input_buf = NULL;

  /* Free all per-pipeline module instances */
  _free_module_list(pipe->modules);
  pipe->modules = NULL;

  free(pipe);
}

/* ── dtpipe_find_module ───────────────────────────────────────────────────── */

dt_iop_module_t *dtpipe_find_module(dt_pipe_t *pipe, const char *op)
{
  if(!pipe || !op)
    return NULL;

  for(_module_node_t *mn = pipe->modules; mn; mn = mn->next)
  {
    if(strncmp(mn->module.op, op, sizeof(mn->module.op)) == 0)
      return &mn->module;
  }
  return NULL;
}

/* ── dtpipe_get_module_count / dtpipe_get_module_name ────────────────────── */

int dtpipe_get_module_count(void)
{
  int count = 0;
  for(_so_node_t *sn = (_so_node_t *)darktable.iop; sn; sn = sn->next)
    count++;
  return count;
}

const char *dtpipe_get_module_name(int index)
{
  if(index < 0)
    return NULL;
  int i = 0;
  for(_so_node_t *sn = (_so_node_t *)darktable.iop; sn; sn = sn->next, i++)
  {
    if(i == index)
      return sn->so->op;
  }
  return NULL;
}
