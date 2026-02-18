/*
 * init.c - libdtpipe library initialization and cleanup
 *
 * Implements dtpipe_init() and dtpipe_cleanup().
 *
 * Responsibilities:
 *   1. Store the data directory path (kernels, color profiles, etc.)
 *   2. Initialize color management via lcms2
 *   3. Optionally initialize OpenCL (graceful degradation to CPU-only)
 *   4. Statically register IOP modules and sort by iop_order
 *   5. Set up the darktable_t global struct
 *
 * Thread safety: dtpipe_init() uses call_once so it is safe to call from
 * multiple threads, but only the first call does real work.
 */

#include "dtpipe.h"
#include "dtpipe_internal.h"

#include <lcms2.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Global state ────────────────────────────────────────────────────────── */

/** The single global darktable_t instance (declared extern in dtpipe_internal.h). */
darktable_t darktable;

/* Internal color-management state */
static cmsContext _lcms_ctx      = NULL;
static cmsHPROFILE _srgb_profile = NULL;

/* call_once guard (pthread_once – C11 threads.h not available on macOS) */
static pthread_once_t _init_once = PTHREAD_ONCE_INIT;

/* Result stored by the once-initializer so concurrent callers can retrieve it */
static int _init_result = DTPIPE_OK;

/* Whether init has been called at all (distinct from once_flag) */
static atomic_int _initialized = 0;

/* ── Color management helpers ────────────────────────────────────────────── */

/* lcms2 error handler – redirect to stderr instead of aborting */
static void _lcms_error_handler(cmsContext ctx, cmsUInt32Number code,
                                 const char *text)
{
  (void)ctx;
  (void)code;
  fprintf(stderr, "[dtpipe/lcms2] error %u: %s\n", (unsigned)code, text);
}

static int _init_color_management(void)
{
  _lcms_ctx = cmsCreateContext(NULL, NULL);
  if (!_lcms_ctx)
  {
    fprintf(stderr, "[dtpipe] Failed to create lcms2 context\n");
    return DTPIPE_ERR_GENERIC;
  }

  cmsSetLogErrorHandlerTHR(_lcms_ctx, _lcms_error_handler);

  /* Built-in sRGB profile – always available without any data files */
  _srgb_profile = cmsCreate_sRGBProfileTHR(_lcms_ctx);
  if (!_srgb_profile)
  {
    fprintf(stderr, "[dtpipe] Failed to create sRGB profile\n");
    cmsDeleteContext(_lcms_ctx);
    _lcms_ctx = NULL;
    return DTPIPE_ERR_GENERIC;
  }

  return DTPIPE_OK;
}

static void _cleanup_color_management(void)
{
  if (_srgb_profile)
  {
    cmsCloseProfile(_srgb_profile);
    _srgb_profile = NULL;
  }
  if (_lcms_ctx)
  {
    cmsDeleteContext(_lcms_ctx);
    _lcms_ctx = NULL;
  }
}

/* ── OpenCL helpers ──────────────────────────────────────────────────────── */

#ifdef DTPIPE_HAVE_OPENCL
#ifdef __APPLE__
#  include <OpenCL/cl.h>
#else
#  include <CL/cl.h>
#endif

static bool _init_opencl(void)
{
  cl_uint num_platforms = 0;
  cl_int  err = clGetPlatformIDs(0, NULL, &num_platforms);
  if (err != CL_SUCCESS || num_platforms == 0)
  {
    fprintf(stderr, "[dtpipe/opencl] No OpenCL platforms found – using CPU\n");
    return false;
  }

  /* For now just detect; full device setup is deferred to Phase 3.5. */
  fprintf(stderr, "[dtpipe/opencl] %u platform(s) found (full init deferred)\n",
          (unsigned)num_platforms);
  return true;
}
#endif /* DTPIPE_HAVE_OPENCL */

/* ── IOP module registration ─────────────────────────────────────────────── */

/*
 * Each compiled-in IOP is forward-declared here and registered below.
 *
 * Convention (mirrors darktable):
 *   void dt_iop_<name>_init_global(dt_iop_module_so_t *module)
 *
 * This section is intentionally empty until Phase 2 modules are compiled in.
 * Add entries as IOPs are ported (Tasks 2.2 – 2.4).
 */

/* --- begin IOP forward declarations (added as modules are ported) ------- */
extern void dt_iop_exposure_init_global(dt_iop_module_so_t *module);
extern void dt_iop_rawprepare_init_global(dt_iop_module_so_t *module);
extern void dt_iop_temperature_init_global(dt_iop_module_so_t *module);
extern void dt_iop_demosaic_init_global(dt_iop_module_so_t *module);
/* --- end IOP forward declarations --------------------------------------- */

typedef void (*iop_init_global_fn_t)(dt_iop_module_so_t *);

typedef struct
{
  const char          *op;
  iop_init_global_fn_t init_fn;
} iop_registration_t;

/*
 * Stub registrations for modules that have descriptor tables in params.c.
 * These allow pipeline creation and param get/set to work even before the
 * actual IOP process functions are compiled in.  The process_plain pointer
 * is left NULL; the pipeline engine skips nodes with a NULL process pointer.
 */
static const iop_registration_t _iop_registry[] = {
  { "rawprepare",  dt_iop_rawprepare_init_global }, /* Task 8.5: real process */
  { "demosaic",    dt_iop_demosaic_init_global }, /* Task 8.7: PPG + passthrough */
  { "colorin",     NULL },
  { "exposure",    dt_iop_exposure_init_global },   /* Task 8.4: real process */
  { "colorout",    NULL },
  { "temperature", dt_iop_temperature_init_global }, /* Task 8.6: real process */
  { "highlights",  NULL },
  { "sharpen",     NULL },
};

static const int _iop_registry_len =
    (int)(sizeof(_iop_registry) / sizeof(_iop_registry[0]));

/* Simple singly-linked list used to emulate GList<dt_iop_module_so_t*> */
typedef struct _so_node_t
{
  dt_iop_module_so_t   *so;
  struct _so_node_t    *next;
} _so_node_t;

static _so_node_t *_iop_so_head = NULL;

static void _register_iop_modules(void)
{
  for (int i = 0; i < _iop_registry_len; i++)
  {
    dt_iop_module_so_t *so =
        (dt_iop_module_so_t *)calloc(1, sizeof(dt_iop_module_so_t));
    if (!so) continue;

    strncpy(so->op, _iop_registry[i].op, sizeof(so->op) - 1);
    so->state = IOP_STATE_ACTIVE;

    if (_iop_registry[i].init_fn)
      _iop_registry[i].init_fn(so);

    /* Prepend to list – ordering is finalised below */
    _so_node_t *node = (_so_node_t *)malloc(sizeof(_so_node_t));
    if (!node) { free(so); continue; }
    node->so   = so;
    node->next = _iop_so_head;
    _iop_so_head = node;
  }

  /*
   * darktable.iop is typed as void* to avoid a GList dependency.
   * For now we store our own linked list head; Phase 3.3 (iop_order.c)
   * will sort this list by the canonical iop_order values.
   */
  darktable.iop = (void *)_iop_so_head;
}

static void _unregister_iop_modules(void)
{
  _so_node_t *node = _iop_so_head;
  while (node)
  {
    _so_node_t *next = node->next;
    free(node->so);
    free(node);
    node = next;
  }
  _iop_so_head = NULL;
  darktable.iop = NULL;
}

/* ── CPU feature detection ───────────────────────────────────────────────── */

static void _detect_codepath(void)
{
  /* On Apple Silicon all SIMD is always available; just clear the flag. */
  darktable.codepath._no_intrinsics = 0;
}

/* ── Main init / cleanup (run exactly once via call_once) ────────────────── */

static char *_init_data_dir = NULL; /* data_dir passed to dtpipe_init() */

static void _do_init(void)
{
  memset(&darktable, 0, sizeof(darktable));

  /* Store data directory */
  if(_init_data_dir && _init_data_dir[0])
  {
    darktable.datadir = strdup(_init_data_dir);
  }

  /* CPU feature detection */
  _detect_codepath();

  /* Thread count */
#ifdef _OPENMP
  darktable.num_openmp_threads = omp_get_max_threads();
#else
  darktable.num_openmp_threads = 1;
#endif

  /* Color management */
  int rc = _init_color_management();
  if (rc != DTPIPE_OK)
  {
    _init_result = rc;
    return;
  }

  /* OpenCL (optional) */
#ifdef DTPIPE_HAVE_OPENCL
  (void)_init_opencl(); /* failure is non-fatal */
#endif

  /* IOP module registration */
  _register_iop_modules();

  _init_result = DTPIPE_OK;
  atomic_store(&_initialized, 1);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int dtpipe_init(const char *data_dir)
{
  /* Store data_dir before calling _do_init (which runs at most once) */
  if(data_dir && data_dir[0] && !_init_data_dir)
    _init_data_dir = strdup(data_dir);

  pthread_once(&_init_once, _do_init);
  return _init_result;
}

void dtpipe_cleanup(void)
{
  if (!atomic_load(&_initialized))
    return;

  atomic_store(&_initialized, 0);

  /* Release IOP modules (reverse of init) */
  _unregister_iop_modules();

  /* Release color management */
  _cleanup_color_management();

  /* Free data dir strings */
  free(darktable.datadir);
  darktable.datadir = NULL;
  free(_init_data_dir);
  _init_data_dir = NULL;

  memset(&darktable, 0, sizeof(darktable));
}

void dtpipe_free_buffer(void *buf)
{
  free(buf);
}

void dtpipe_free_string(char *str)
{
  free(str);
}
