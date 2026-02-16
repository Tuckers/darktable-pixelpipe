/*
 * dtpipe_internal.h - Minimal internal type definitions for libdtpipe
 *
 * This header provides the core structs, enums, and macros needed by extracted
 * IOP modules and the pixelpipe engine. It is intentionally self-contained:
 * no GLib, no GTK, no SQLite, no Lua — just C11 standard headers.
 *
 * The definitions here are derived from darktable's headers but stripped of:
 *   - Database identifiers (film_id, group_id, etc.)
 *   - GUI state (GtkWidget*, expander, buttons, bauhaus)
 *   - Histogram collection infrastructure
 *   - OpenCL back-buffer
 *   - Undo/redo machinery
 *
 * Naming convention: types are identical to darktable originals so that IOP
 * source files can be compiled against this header with minimal changes.
 */

#pragma once

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── OpenMP shims ─────────────────────────────────────────────────────────── */

#ifdef _OPENMP
#  include <omp.h>
#  define DT_OMP_PRAGMA(...) _Pragma(#__VA_ARGS__)
#  define DT_OMP_FOR(clauses) \
     _Pragma("omp parallel for default(firstprivate) schedule(static) " #clauses)
#  define DT_OMP_FOR_SIMD(clauses) \
     _Pragma("omp parallel for simd default(firstprivate) schedule(simd:static) " #clauses)
#  define DT_OMP_SIMD(clauses) _Pragma("omp simd " #clauses)
#  define DT_OMP_DECLARE_SIMD(clauses) _Pragma("omp declare simd " #clauses)
#  define dt_omp_firstprivate(...) firstprivate(__VA_ARGS__)
#  define dt_omp_nontemporal(...)
#  define dt_omp_sharedconst(...) shared(__VA_ARGS__)
static inline int dt_get_num_threads(void) {
  return (int)CLAMP(omp_get_num_procs(), 1, 64);
}
static inline int dt_get_thread_num(void) { return omp_get_thread_num(); }
#else
#  define DT_OMP_PRAGMA(...)
#  define DT_OMP_FOR(clauses)
#  define DT_OMP_FOR_SIMD(clauses)
#  define DT_OMP_SIMD(clauses)
#  define DT_OMP_DECLARE_SIMD(clauses)
#  define dt_omp_firstprivate(...)
#  define dt_omp_nontemporal(...)
#  define dt_omp_sharedconst(...)
static inline int dt_get_num_threads(void) { return 1; }
static inline int dt_get_thread_num(void)  { return 0; }
static inline int omp_get_max_threads(void) { return 1; }
static inline int omp_get_thread_num(void)  { return 0; }
#endif

/* ── Cache-line / alignment constants ────────────────────────────────────── */

#if defined(__APPLE__) && defined(__aarch64__)
#  define DT_CACHELINE_BYTES  128
#  define DT_CACHELINE_FLOATS  32
#  define DT_CACHELINE_PIXELS   8
#else
#  define DT_CACHELINE_BYTES   64
#  define DT_CACHELINE_FLOATS  16
#  define DT_CACHELINE_PIXELS   4
#endif

#define DT_ALIGNED_ARRAY __attribute__((aligned(DT_CACHELINE_BYTES)))
#define DT_ALIGNED_PIXEL __attribute__((aligned(16)))

/** Aligned 4-float pixel vector */
typedef DT_ALIGNED_PIXEL float dt_aligned_pixel_t[4];

/** 3×3 matrix padded to 4×4 for SIMD */
typedef float DT_ALIGNED_ARRAY dt_colormatrix_t[4][4];

#define DT_IS_ALIGNED(x)       __builtin_assume_aligned(x, DT_CACHELINE_BYTES)
#define DT_IS_ALIGNED_PIXEL(x) __builtin_assume_aligned(x, 16)

/* ── Per-channel SIMD loop macros ─────────────────────────────────────────── */

#ifdef DT_NO_VECTORIZATION
#  define DT_PIXEL_SIMD_CHANNELS 3
#else
#  define DT_PIXEL_SIMD_CHANNELS 4
#endif

#if defined(_OPENMP) && !defined(DT_NO_SIMD_HINTS)
#  define for_each_channel(_var, ...) \
     _Pragma("omp simd " #__VA_ARGS__) \
     for (size_t _var = 0; _var < DT_PIXEL_SIMD_CHANNELS; _var++)
#  define for_four_channels(_var, ...) \
     _Pragma("omp simd " #__VA_ARGS__) \
     for (size_t _var = 0; _var < 4; _var++)
#  define for_three_channels(_var, ...) \
     _Pragma("omp simd " #__VA_ARGS__) \
     for (size_t _var = 0; _var < 3; _var++)
#else
#  define for_each_channel(_var, ...)   for (size_t _var = 0; _var < DT_PIXEL_SIMD_CHANNELS; _var++)
#  define for_four_channels(_var, ...)  for (size_t _var = 0; _var < 4; _var++)
#  define for_three_channels(_var, ...) for (size_t _var = 0; _var < 3; _var++)
#endif

/* ── Module version / introspection ──────────────────────────────────────── */

#define DT_MODULE_VERSION 25

#ifdef _DEBUG
#  define DT_MODULE(MODVER)                      \
     int dt_module_dt_version(void) { return -DT_MODULE_VERSION; } \
     int dt_module_mod_version(void) { return  MODVER; }
#else
#  define DT_MODULE(MODVER)                      \
     int dt_module_dt_version(void) { return  DT_MODULE_VERSION; } \
     int dt_module_mod_version(void) { return  MODVER; }
#endif

/** Each IOP calls this macro at file scope to declare its version and params type. */
#define DT_MODULE_INTROSPECTION(MODVER, PARAMSTYPE) DT_MODULE(MODVER)

/* ── Basic scalar constants ───────────────────────────────────────────────── */

#ifndef PHI
#  define PHI    1.61803398874989479F
#endif
#ifndef INVPHI
#  define INVPHI 0.61803398874989479F
#endif

#define CLAMPS(A, L, H) ((A) > (L) ? ((A) < (H) ? (A) : (H)) : (L))

#define DT_MAX_FILENAME_LEN 256
#define DT_MAX_PATH_FOR_PARAMS 4096

#define DT_DEVICE_CPU -1
#define DT_DEVICE_NONE -2

/* ── ID typedefs ─────────────────────────────────────────────────────────── */

typedef int32_t dt_imgid_t;
typedef int32_t dt_filmid_t;
typedef int32_t dt_mask_id_t;
typedef uint64_t dt_hash_t;

#define NO_IMGID   0
#define NO_FILMID  0
#define NO_MASKID  0
#define INVALID_MASKID (-1)
#define BLEND_RASTER_ID 0
#define DT_INITHASH 5381
#define DT_INVALID_HASH 0

#define dt_is_valid_imgid(n)  ((n) > NO_IMGID)
#define dt_is_valid_filmid(n) ((n) > NO_FILMID)
#define dt_is_valid_maskid(n) ((n) > NO_MASKID)

/** Operation name: a 20-char string identifying the IOP (e.g. "exposure"). */
typedef char dt_dev_operation_t[20];

/* ── IOP order types ─────────────────────────────────────────────────────── */

typedef enum dt_iop_order_t
{
  DT_IOP_ORDER_CUSTOM  = 0,
  DT_IOP_ORDER_LEGACY  = 1,
  DT_IOP_ORDER_V30     = 2,
  DT_IOP_ORDER_V30_JPG = 3,
  DT_IOP_ORDER_V50     = 4,
  DT_IOP_ORDER_V50_JPG = 5,
  DT_IOP_ORDER_LAST    = 6
} dt_iop_order_t;

#define DT_DEFAULT_IOP_ORDER_RAW DT_IOP_ORDER_V50
#define DT_DEFAULT_IOP_ORDER_JPG DT_IOP_ORDER_V50_JPG

typedef struct dt_iop_order_entry_t
{
  union {
    double iop_order_f; /* legacy float order (migration only) */
    int    iop_order;   /* integer sort key (1-based, step 100) */
  } o;
  char    operation[20];
  int32_t instance;
  char    name[25];
} dt_iop_order_entry_t;

typedef struct dt_iop_order_rule_t
{
  char op_prev[20];
  char op_next[20];
} dt_iop_order_rule_t;

/* ── dt_iop_params_t ─────────────────────────────────────────────────────── */

#ifndef DT_IOP_PARAMS_T
#define DT_IOP_PARAMS_T
typedef void dt_iop_params_t;
#endif

/* ── Debug / print shims ─────────────────────────────────────────────────── */

typedef enum dt_debug_thread_t
{
  DT_DEBUG_ALWAYS  = 0,
  DT_DEBUG_CACHE   = 1 <<  0,
  DT_DEBUG_DEV     = 1 <<  2,
  DT_DEBUG_PERF    = 1 <<  4,
  DT_DEBUG_OPENCL  = 1 <<  7,
  DT_DEBUG_NAN     = 1 << 11,
  DT_DEBUG_MASKS   = 1 << 12,
  DT_DEBUG_PIPE    = 1 << 25,
  DT_DEBUG_ALL     = (int)0x7fffffff,
} dt_debug_thread_t;

/* Suppress all debug output in the standalone library */
#define dt_print(thread, ...)        do {} while(0)
#define dt_print_nts(thread, ...)    do {} while(0)
#define dt_print_pipe(thread, ...)   do {} while(0)

/* ── Mutex wrapper ───────────────────────────────────────────────────────── */

typedef struct { pthread_mutex_t m; } dt_pthread_mutex_t;

static inline void dt_pthread_mutex_init(dt_pthread_mutex_t *mtx) {
  pthread_mutex_init(&mtx->m, NULL);
}
static inline void dt_pthread_mutex_destroy(dt_pthread_mutex_t *mtx) {
  pthread_mutex_destroy(&mtx->m);
}
static inline void dt_pthread_mutex_lock(dt_pthread_mutex_t *mtx) {
  pthread_mutex_lock(&mtx->m);
}
static inline void dt_pthread_mutex_unlock(dt_pthread_mutex_t *mtx) {
  pthread_mutex_unlock(&mtx->m);
}

/* ── Atomic int (C11) ────────────────────────────────────────────────────── */

#ifdef __cplusplus
#  include <atomic>
   typedef std::atomic<int> dt_atomic_int;
   static inline void dt_atomic_set_int(dt_atomic_int *v, int x) { std::atomic_store(v, x); }
   static inline int  dt_atomic_get_int(dt_atomic_int *v)         { return std::atomic_load(v); }
#else
#  include <stdatomic.h>
   typedef atomic_int dt_atomic_int;
   static inline void dt_atomic_set_int(dt_atomic_int *v, int x) { atomic_store(v, x); }
   static inline int  dt_atomic_get_int(dt_atomic_int *v)         { return atomic_load(v); }
#endif

/* ── Aligned allocation helpers ─────────────────────────────────────────── */

static inline void *dt_alloc_aligned(size_t size) {
  void *buf = NULL;
#if defined(_WIN32)
  buf = _aligned_malloc(size, DT_CACHELINE_BYTES);
#else
  if (posix_memalign(&buf, DT_CACHELINE_BYTES, size) != 0) buf = NULL;
#endif
  return buf;
}

static inline void *dt_calloc_aligned(size_t size) {
  void *buf = dt_alloc_aligned(size);
  if (buf) memset(buf, 0, size);
  return buf;
}

static inline void dt_free_align(void *p) { free(p); }

static inline float *dt_alloc_align_float(size_t n) {
  return (float *)dt_alloc_aligned(n * sizeof(float));
}
static inline float *dt_calloc_align_float(size_t n) {
  return (float *)dt_calloc_aligned(n * sizeof(float));
}
static inline uint8_t *dt_alloc_align_uint8(size_t n) {
  return (uint8_t *)dt_alloc_aligned(n * sizeof(uint8_t));
}

#define dt_alloc_align_type(TYPE, count) \
  ((TYPE*)__builtin_assume_aligned(dt_alloc_aligned((count)*sizeof(TYPE)), DT_CACHELINE_BYTES))
#define dt_calloc_align_type(TYPE, count) \
  ((TYPE*)__builtin_assume_aligned(dt_calloc_aligned((count)*sizeof(TYPE)), DT_CACHELINE_BYTES))
#define dt_alloc1_align_type(TYPE) \
  ((TYPE*)__builtin_assume_aligned(dt_alloc_aligned(sizeof(TYPE)), DT_CACHELINE_BYTES))
#define dt_calloc1_align_type(TYPE) \
  ((TYPE*)__builtin_assume_aligned(dt_calloc_aligned(sizeof(TYPE)), DT_CACHELINE_BYTES))

/* Per-thread buffer helpers */
static inline void *dt_alloc_perthread(size_t n, size_t objsize, size_t *padded_size) {
  const size_t alloc_size  = n * objsize;
  const size_t cache_lines = (alloc_size + DT_CACHELINE_BYTES - 1) / DT_CACHELINE_BYTES;
  *padded_size = DT_CACHELINE_BYTES * cache_lines / objsize;
  return dt_alloc_aligned(DT_CACHELINE_BYTES * cache_lines * (size_t)dt_get_num_threads());
}
static inline float *dt_alloc_perthread_float(size_t n, size_t *padded_size) {
  return (float *)dt_alloc_perthread(n, sizeof(float), padded_size);
}
#define dt_get_perthread(buf, padsize) \
  DT_IS_ALIGNED((buf) + ((padsize) * (size_t)dt_get_thread_num()))

/* ── Hash helper ─────────────────────────────────────────────────────────── */

static inline dt_hash_t dt_hash(dt_hash_t hash, const void *data, size_t size) {
  const uint8_t *str = (const uint8_t *)data;
  for (size_t i = 0; i < size; i++)
    hash = ((hash << 5) + hash) ^ str[i];
  return hash;
}

/* ── Pixel copy helpers ──────────────────────────────────────────────────── */

static inline void copy_pixel(float *restrict out, const float *restrict in) {
  for_each_channel(k, aligned(in, out:16)) out[k] = in[k];
}

static inline void copy_pixel_nontemporal(float *restrict out, const float *restrict in) {
#if defined(__SSE__)
  #include <xmmintrin.h>
  _mm_stream_ps(out, *((__m128 *)in));
#else
  for_each_channel(k, aligned(in, out:16)) out[k] = in[k];
#endif
}

#define dt_omploop_sfence() do {} while(0)

/* ── Codepath / CPU feature detection ───────────────────────────────────── */

typedef struct dt_codepath_t {
  unsigned int _no_intrinsics : 1;
} dt_codepath_t;

/* ── dt_iop_colorspace_type_t ────────────────────────────────────────────── */

typedef enum dt_iop_colorspace_type_t
{
  IOP_CS_NONE   = -1,
  IOP_CS_RAW    =  0,
  IOP_CS_LAB    =  1,
  IOP_CS_RGB    =  2,
  IOP_CS_LCH    =  3,
  IOP_CS_HSL    =  4,
  IOP_CS_JZCZHZ =  5,
} dt_iop_colorspace_type_t;

/* ── dt_iop_color_intent_t ───────────────────────────────────────────────── */

typedef enum dt_iop_color_intent_t
{
  DT_INTENT_PERCEPTUAL             = 0,
  DT_INTENT_RELATIVE_COLORIMETRIC  = 1,
  DT_INTENT_SATURATION             = 2,
  DT_INTENT_ABSOLUTE_COLORIMETRIC  = 3,
  DT_INTENT_LAST
} dt_iop_color_intent_t;

/* ── dt_colorspaces_color_profile_type_t ─────────────────────────────────── */

typedef enum dt_colorspaces_color_profile_type_t
{
  DT_COLORSPACE_NONE             = -1,
  DT_COLORSPACE_FILE             =  0,
  DT_COLORSPACE_SRGB             =  1,
  DT_COLORSPACE_ADOBERGB         =  2,
  DT_COLORSPACE_LIN_REC709       =  3,
  DT_COLORSPACE_LIN_REC2020      =  4,
  DT_COLORSPACE_XYZ              =  5,
  DT_COLORSPACE_LAB              =  6,
  DT_COLORSPACE_INFRARED         =  7,
  DT_COLORSPACE_DISPLAY          =  8,
  DT_COLORSPACE_EMBEDDED_ICC     =  9,
  DT_COLORSPACE_EMBEDDED_MATRIX  = 10,
  DT_COLORSPACE_STANDARD_MATRIX  = 11,
  DT_COLORSPACE_ENHANCED_MATRIX  = 12,
  DT_COLORSPACE_VENDOR_MATRIX    = 13,
  DT_COLORSPACE_ALTERNATE_MATRIX = 14,
  DT_COLORSPACE_BRG              = 15,
  DT_COLORSPACE_EXPORT           = 16,
  DT_COLORSPACE_SOFTPROOF        = 17,
  DT_COLORSPACE_WORK             = 18,
  DT_COLORSPACE_DISPLAY2         = 19,
  DT_COLORSPACE_REC709           = 20,
  DT_COLORSPACE_PROPHOTO_RGB     = 21,
  DT_COLORSPACE_PQ_REC2020       = 22,
  DT_COLORSPACE_HLG_REC2020      = 23,
  DT_COLORSPACE_PQ_P3            = 24,
  DT_COLORSPACE_HLG_P3           = 25,
  DT_COLORSPACE_DISPLAY_P3       = 26,
  DT_COLORSPACE_LAST             = 27,
} dt_colorspaces_color_profile_type_t;

/* ── dt_iop_rgb_norms_t ──────────────────────────────────────────────────── */

typedef enum dt_iop_rgb_norms_t
{
  DT_RGB_NORM_NONE      = 0,
  DT_RGB_NORM_LUMINANCE = 1,
  DT_RGB_NORM_MAX       = 2,
  DT_RGB_NORM_AVERAGE   = 3,
  DT_RGB_NORM_SUM       = 4,
  DT_RGB_NORM_NORM      = 5,
  DT_RGB_NORM_POWER     = 6,
} dt_iop_rgb_norms_t;

/* ── dt_imageio_levels_t ─────────────────────────────────────────────────── */

typedef enum dt_imageio_levels_t
{
  IMAGEIO_INT8      = 0x0,
  IMAGEIO_INT12     = 0x1,
  IMAGEIO_INT16     = 0x2,
  IMAGEIO_INT32     = 0x3,
  IMAGEIO_FLOAT     = 0x4,
  IMAGEIO_BW        = 0x5,
  IMAGEIO_INT10     = 0x6,
  IMAGEIO_PREC_MASK = 0xFF,
} dt_imageio_levels_t;

/* ── dt_dev_pixelpipe_type_t ─────────────────────────────────────────────── */

typedef enum dt_dev_pixelpipe_type_t
{
  DT_DEV_PIXELPIPE_NONE      = 0,
  DT_DEV_PIXELPIPE_EXPORT    = 1 << 0,
  DT_DEV_PIXELPIPE_FULL      = 1 << 1,
  DT_DEV_PIXELPIPE_PREVIEW   = 1 << 2,
  DT_DEV_PIXELPIPE_THUMBNAIL = 1 << 3,
  DT_DEV_PIXELPIPE_PREVIEW2  = 1 << 4,
  DT_DEV_PIXELPIPE_SCREEN    = (1 << 2) | (1 << 1) | (1 << 4),
  DT_DEV_PIXELPIPE_ANY       = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4),
  DT_DEV_PIXELPIPE_FAST      = 1 << 8,
} dt_dev_pixelpipe_type_t;

/* ── dt_dev_pixelpipe_display_mask_t ─────────────────────────────────────── */

typedef enum dt_dev_pixelpipe_display_mask_t
{
  DT_DEV_PIXELPIPE_DISPLAY_NONE    = 0,
  DT_DEV_PIXELPIPE_DISPLAY_MASK    = 1 << 0,
  DT_DEV_PIXELPIPE_DISPLAY_CHANNEL = 1 << 1,
  DT_DEV_PIXELPIPE_DISPLAY_OUTPUT  = 1 << 2,
} dt_dev_pixelpipe_display_mask_t;

/* ── dt_dev_pixelpipe_change/status ──────────────────────────────────────── */

typedef enum dt_dev_pixelpipe_change_t
{
  DT_DEV_PIPE_UNCHANGED   = 0,
  DT_DEV_PIPE_TOP_CHANGED = 1 << 0,
  DT_DEV_PIPE_REMOVE      = 1 << 1,
  DT_DEV_PIPE_SYNCH       = 1 << 2,
  DT_DEV_PIPE_ZOOMED      = 1 << 3,
} dt_dev_pixelpipe_change_t;

typedef enum dt_dev_pixelpipe_status_t
{
  DT_DEV_PIXELPIPE_DIRTY   = 0,
  DT_DEV_PIXELPIPE_RUNNING = 1,
  DT_DEV_PIXELPIPE_VALID   = 2,
  DT_DEV_PIXELPIPE_INVALID = 3,
} dt_dev_pixelpipe_status_t;

typedef enum dt_dev_pixelpipe_stopper_t
{
  DT_DEV_PIXELPIPE_STOP_NO    = 0,
  DT_DEV_PIXELPIPE_STOP_NODES,
  DT_DEV_PIXELPIPE_STOP_HQ,
  DT_DEV_PIXELPIPE_STOP_LAST,
} dt_dev_pixelpipe_stopper_t;

/* ── dt_dev_request_flags_t ──────────────────────────────────────────────── */

typedef enum dt_dev_request_flags_t
{
  DT_REQUEST_NONE          = 0,
  DT_REQUEST_ON            = 1 << 0,
  DT_REQUEST_ONLY_IN_GUI   = 1 << 1,
  DT_REQUEST_EXPANDED      = 1 << 2,
} dt_dev_request_flags_t;

/* ── IOP group / flags / state ───────────────────────────────────────────── */

typedef enum dt_iop_group_t
{
  IOP_GROUP_NONE      = 0,
  IOP_GROUP_BASIC     = 1 << 0,
  IOP_GROUP_TONE      = 1 << 1,
  IOP_GROUP_COLOR     = 1 << 2,
  IOP_GROUP_CORRECT   = 1 << 3,
  IOP_GROUP_EFFECT    = 1 << 4,
  IOP_GROUP_TECHNICAL = 1 << 5,
  IOP_GROUP_GRADING   = 1 << 6,
  IOP_GROUP_EFFECTS   = 1 << 7,
} dt_iop_group_t;

typedef enum dt_iop_tags_t
{
  IOP_TAG_NONE       = 0,
  IOP_TAG_DISTORT    = 1 << 0,
  IOP_TAG_DECORATION = 1 << 1,
  IOP_TAG_CROPPING   = 1 << 2,
  IOP_TAG_GEOMETRY   = 1 << 3,
} dt_iop_tags_t;

typedef enum dt_iop_flags_t
{
  IOP_FLAGS_NONE                 = 0,
  IOP_FLAGS_INCLUDE_IN_STYLES    = 1 <<  0,
  IOP_FLAGS_SUPPORTS_BLENDING    = 1 <<  1,
  IOP_FLAGS_DEPRECATED           = 1 <<  2,
  IOP_FLAGS_ALLOW_TILING         = 1 <<  4,
  IOP_FLAGS_HIDDEN               = 1 <<  5,
  IOP_FLAGS_TILING_FULL_ROI      = 1 <<  6,
  IOP_FLAGS_ONE_INSTANCE         = 1 <<  7,
  IOP_FLAGS_PREVIEW_NON_OPENCL   = 1 <<  8,
  IOP_FLAGS_NO_HISTORY_STACK     = 1 <<  9,
  IOP_FLAGS_NO_MASKS             = 1 << 10,
  IOP_FLAGS_FENCE                = 1 << 11,
  IOP_FLAGS_ALLOW_FAST_PIPE      = 1 << 12,
  IOP_FLAGS_UNSAFE_COPY          = 1 << 13,
  IOP_FLAGS_EXPAND_ROI_IN        = 1 << 17,
  IOP_FLAGS_WRITE_DETAILS        = 1 << 18,
  IOP_FLAGS_WRITE_RASTER         = 1 << 19,
} dt_iop_flags_t;

typedef enum dt_iop_module_state_t
{
  IOP_STATE_HIDDEN   = 0,
  IOP_STATE_ACTIVE,
  IOP_STATE_FAVORITE,
  IOP_STATE_LAST,
} dt_iop_module_state_t;

/* Opaque per-module data types (content defined by each IOP) */
typedef void dt_iop_gui_data_t;
typedef void dt_iop_data_t;
typedef void dt_iop_global_data_t;
typedef void dt_iop_module_data_t;

/* ── dt_iop_roi_t ────────────────────────────────────────────────────────── */

typedef struct dt_iop_roi_t
{
  int   x, y;
  int   width, height;
  float scale;
} dt_iop_roi_t;

/* ── dt_iop_buffer_type_t / dt_iop_buffer_dsc_t ─────────────────────────── */

typedef enum dt_iop_buffer_type_t
{
  TYPE_UNKNOWN = 0,
  TYPE_FLOAT,
  TYPE_UINT16,
} dt_iop_buffer_type_t;

typedef struct dt_iop_buffer_dsc_t
{
  /** Number of channels: 1 (raw) or 4 (float RGBA). */
  unsigned int channels;
  /** Underlying data type. */
  dt_iop_buffer_type_t datatype;
  /** Bayer filter pattern (0 = no mosaic). */
  uint32_t filters;
  /** Fuji X-Trans filter array (used when filters == 9u). */
  uint8_t xtrans[6][6];

  struct {
    uint16_t raw_black_level;
    uint16_t raw_white_point;
  } rawprepare;

  struct {
    bool               enabled;
    dt_aligned_pixel_t coeffs;
  } temperature;

  /** Per-channel saturation maximum, propagated through the pipe. */
  dt_aligned_pixel_t processed_maximum;

  /** Colorspace tag (one of dt_iop_colorspace_type_t cast to int). */
  int cst;
} dt_iop_buffer_dsc_t;

/* ── dt_image_orientation_t ──────────────────────────────────────────────── */

typedef enum dt_image_orientation_t
{
  ORIENTATION_NULL    = -1,
  ORIENTATION_NONE    =  0,
  ORIENTATION_FLIP_Y  = 1 << 0,
  ORIENTATION_FLIP_X  = 1 << 1,
  ORIENTATION_SWAP_XY = 1 << 2,

  ORIENTATION_FLIP_HORIZONTALLY = ORIENTATION_FLIP_X,
  ORIENTATION_FLIP_VERTICALLY   = ORIENTATION_FLIP_Y,
  ORIENTATION_ROTATE_180_DEG    = ORIENTATION_FLIP_Y | ORIENTATION_FLIP_X,
  ORIENTATION_TRANSPOSE         = ORIENTATION_SWAP_XY,
  ORIENTATION_ROTATE_CCW_90_DEG = ORIENTATION_FLIP_X | ORIENTATION_SWAP_XY,
  ORIENTATION_ROTATE_CW_90_DEG  = ORIENTATION_FLIP_Y | ORIENTATION_SWAP_XY,
  ORIENTATION_TRANSVERSE        = ORIENTATION_FLIP_Y | ORIENTATION_FLIP_X | ORIENTATION_SWAP_XY,
} dt_image_orientation_t;

/* ── dt_image_colorspace_t ───────────────────────────────────────────────── */

typedef enum dt_image_colorspace_t
{
  DT_IMAGE_COLORSPACE_NONE,
  DT_IMAGE_COLORSPACE_SRGB,
  DT_IMAGE_COLORSPACE_ADOBE_RGB,
} dt_image_colorspace_t;

/* ── dt_image_t ──────────────────────────────────────────────────────────── */
/*
 * Image metadata.  Fields preserved:
 *   - dimensions, orientation
 *   - camera maker/model/lens strings
 *   - key EXIF scalars
 *   - buffer descriptor (bayer pattern, colour matrix, WB coeffs)
 *   - embedded ICC profile pointer
 *
 * Fields removed from the original:
 *   - database IDs (film_id, group_id, id) — not meaningful standalone
 *   - thumbnail / cache machinery
 *   - lens-correction correction tables (sensor-specific)
 *   - geolocation
 *   - GLib timestamps
 */
typedef struct dt_image_t
{
  /* EXIF basics */
  bool                    exif_inited;
  dt_image_orientation_t  orientation;
  float                   exif_exposure;
  float                   exif_exposure_bias;
  float                   exif_aperture;
  float                   exif_iso;
  float                   exif_focal_length;
  float                   exif_focus_distance;
  float                   exif_crop;
  float                   exif_highlight_preservation;
  char                    exif_maker[64];
  char                    exif_model[64];
  char                    exif_lens[128];
  char                    exif_whitebalance[64];
  char                    exif_flash[64];
  char                    exif_exposure_program[64];
  char                    exif_metering_mode[64];

  /* Camera make/model (may differ from EXIF for aliased bodies) */
  char camera_maker[64];
  char camera_model[64];
  char camera_alias[64];
  char camera_makermodel[128];

  /* File path */
  char filename[DT_MAX_FILENAME_LEN];

  /* Geometry */
  int32_t width, height;
  int32_t final_width, final_height;
  int32_t p_width, p_height;   /* updated by rawprepare */
  int32_t crop_x, crop_y;
  int32_t crop_right, crop_bottom;
  float   aspect_ratio;

  /* Image flags (DT_IMAGE_RAW, DT_IMAGE_LDR, DT_IMAGE_HDR, …) */
  int32_t flags;

  /* Pixel buffer descriptor (Bayer pattern, data type, channel count) */
  dt_iop_buffer_dsc_t buf_dsc;

  /* Colour science */
  float              d65_color_matrix[9]; /* 3×3 matrix from DNG */
  uint8_t           *profile;             /* embedded ICC blob (may be NULL) */
  uint32_t           profile_size;
  dt_image_colorspace_t colorspace;       /* sRGB / AdobeRGB hint from EXIF */

  /* Raw data metadata */
  uint16_t           raw_black_level;
  uint16_t           raw_black_level_separate[4];
  uint32_t           raw_white_point;
  uint32_t           fuji_rotation_pos;
  float              pixel_aspect_ratio;
  float              linear_response_limit;

  /* White balance coefficients */
  dt_aligned_pixel_t wb_coeffs;

  /* Adobe XYZ→CAM matrix */
  float              adobe_XYZ_to_CAM[4][3];

  /* User crop (normalised bounding box: x0,y0,x1,y1) */
  float              usercrop[4];
} dt_image_t;

/* Image flag bits */
typedef enum dt_image_flags_t
{
  DT_IMAGE_LDR                      =    32,
  DT_IMAGE_RAW                      =    64,
  DT_IMAGE_HDR                      =   128,
  DT_IMAGE_AUTO_PRESETS_APPLIED     =   512,
  DT_IMAGE_NO_LEGACY_PRESETS        =  1024,
  DT_IMAGE_MONOCHROME               = 32768,
  DT_IMAGE_MONOCHROME_WORKFLOW      = 1 << 20,
} dt_image_flags_t;

/* ── Forward declarations ────────────────────────────────────────────────── */

struct dt_iop_module_so_t;
struct dt_iop_module_t;
struct dt_dev_pixelpipe_t;
struct dt_dev_pixelpipe_iop_t;
struct dt_develop_tiling_t;
struct dt_iop_order_iccprofile_info_t;
struct dt_develop_blend_params_t;
struct dt_dev_pixelpipe_cache_t;

/* ── dt_develop_tiling_t ─────────────────────────────────────────────────── */

typedef struct dt_develop_tiling_t
{
  float  factor;      /* CPU memory multiplier (relative to in+out buffer) */
  float  factor_cl;   /* GPU memory multiplier */
  size_t maxbuf;      /* Maximum single CPU buffer bytes (0 = unlimited) */
  size_t maxbuf_cl;   /* Maximum single GPU buffer bytes (0 = unlimited) */
  size_t overhead;    /* Additional fixed overhead bytes */
  int    overlap;     /* Required overlap between tiles (pixels) */
  float  xalign;      /* Required tile-width  alignment (pixels, default 1) */
  float  yalign;      /* Required tile-height alignment (pixels, default 1) */
} dt_develop_tiling_t;

/* ── dt_develop_blend_params_t ───────────────────────────────────────────── */

/* Blend mode constants (subset used by the processing engine) */
#define DEVELOP_MASK_DISABLED  0
#define DEVELOP_MASK_ENABLED   1
#define DEVELOP_MASK_BOTH      3

#define DEVELOP_BLEND_CS_NONE       0
#define DEVELOP_BLEND_CS_RAW        1
#define DEVELOP_BLEND_CS_LAB        2
#define DEVELOP_BLEND_CS_RGB_DISPLAY 3
#define DEVELOP_BLEND_CS_RGB_SCENE  4

typedef struct dt_develop_blend_params_t
{
  uint32_t  mask_mode;      /* bitmask: DEVELOP_MASK_* */
  uint32_t  blend_mode;
  int       blend_cst;      /* DEVELOP_BLEND_CS_* */
  float     opacity;
  /* Additional fields present in the original; kept opaque here */
  uint8_t   _reserved[256];
} dt_develop_blend_params_t;

/* ── dt_iop_order_iccprofile_info_t ──────────────────────────────────────── */

typedef struct dt_iop_order_iccprofile_info_t
{
  dt_colorspaces_color_profile_type_t type;
  char    filename[DT_MAX_PATH_FOR_PARAMS];
  int     intent;
  /* matrix_in[3][4] and matrix_out[3][4] – 4×3 padded for SIMD */
  float   matrix_in[3][4];
  float   matrix_out[3][4];
  float   lut_in[3][1 << 16];   /* per-channel LUT (rarely populated) */
  float   lut_out[3][1 << 16];
  float   unbounded_coeffs_in[3][3];
  float   unbounded_coeffs_out[3][3];
  int     lut_size;
  int     nonlinear;
} dt_iop_order_iccprofile_info_t;

/* ── Histogram stats (used by module + pipe structs) ─────────────────────── */

typedef struct dt_dev_histogram_stats_t
{
  uint32_t bins_count;
  size_t   buf_size;
  uint32_t pixels;
  uint32_t ch;
} dt_dev_histogram_stats_t;

/* ── dt_dev_pixelpipe_iop_t ──────────────────────────────────────────────── */
/*
 * One node in the pixelpipe — the per-pipe instance of an IOP.
 */
typedef struct dt_dev_pixelpipe_iop_t
{
  struct dt_iop_module_t  *module; /* the IOP module instance */
  struct dt_dev_pixelpipe_t *pipe; /* owning pipeline */

  void *data;          /* per-pipe private data (allocated by module's init_pipe) */
  void *blendop_data;  /* per-pipe blending data */

  bool enabled;

  /* Geometry */
  float        iscale;
  int          iwidth, iheight;

  /* Cache hash of (params + enabled) */
  dt_hash_t    hash;

  int          bpc;    /* bits per channel; 32 = float */
  int          colors; /* channels per pixel */

  /* Theoretical full-buffer ROIs as passed through modify_roi_out */
  dt_iop_roi_t buf_in, buf_out;
  /* Actual ROIs used during processing */
  dt_iop_roi_t processed_roi_in, processed_roi_out;

  /* Disable process_cl / tiling temporarily from commit_params */
  bool process_cl_ready;
  bool process_tiling_ready;

  /* Buffer format descriptors */
  dt_iop_buffer_dsc_t dsc_in;
  dt_iop_buffer_dsc_t dsc_out;

  /* Raster mask table (maps mask id → float*).  NULL if unused. */
  void *raster_masks; /* GHashTable* in the original; opaque here */
} dt_dev_pixelpipe_iop_t;

/* ── dt_dev_pixelpipe_t ──────────────────────────────────────────────────── */
/*
 * The pixel processing pipeline.
 *
 * Removed vs. original:
 *   - dt_dev_pixelpipe_cache_t  (multi-entry zoom cache — complex GLib machinery)
 *   - uint8_t *backbuf          (GUI preview backbuffer)
 *   - histogram collection
 *   - GLib GList wrappers replaced with opaque void* (populated by init code)
 *   - detail-mask / scharr buffer (re-added if needed in Phase 3.5)
 */
typedef struct dt_dev_pixelpipe_t
{
  /* Input buffer (float RGBA, possibly down-scaled) */
  float *input;
  int    iwidth, iheight;
  float  iscale;

  /* Output dimensions after all modules */
  int processed_width, processed_height;

  /* Expected output format; may be updated by process*() */
  dt_iop_buffer_dsc_t dsc;

  /* ICC profile info for working / input / output spaces */
  struct dt_iop_order_iccprofile_info_t *work_profile_info;
  struct dt_iop_order_iccprofile_info_t *input_profile_info;
  struct dt_iop_order_iccprofile_info_t *output_profile_info;

  /* Ordered list of dt_dev_pixelpipe_iop_t nodes (void* = GList* internally) */
  void *nodes;  /* GList<dt_dev_pixelpipe_iop_t*> */

  /* State */
  dt_dev_pixelpipe_change_t changed;
  dt_dev_pixelpipe_status_t status;
  bool loading;
  bool input_changed;
  bool nocache;
  bool processing;
  bool opencl_enabled;
  bool opencl_error;
  bool tiling;
  bool bypass_blendif;
  bool store_all_raster_masks;

  dt_dev_pixelpipe_display_mask_t mask_display;

  /* Shutdown flag: 0 = running, non-zero = stop (see dt_dev_pixelpipe_stopper_t) */
  dt_atomic_int shutdown;

  int  input_timestamp;
  int  devid;  /* OpenCL device id, DT_DEVICE_CPU = -1 */

  dt_dev_pixelpipe_type_t type;

  /* Output bit depth / levels */
  dt_imageio_levels_t levels;

  /* Output ICC profile override */
  dt_colorspaces_color_profile_type_t icc_type;
  char                                icc_filename[DT_MAX_PATH_FOR_PARAMS];
  dt_iop_color_intent_t               icc_intent;

  /* Snapshot of image metadata when the pipeline was created */
  dt_image_t image;

  /* Ordered snapshot of module instances (void* = GList<dt_iop_module_t*>) */
  void *iop;
  void *iop_order_list;
  void *forms;  /* mask forms list */

  /* Synchronisation */
  dt_pthread_mutex_t mutex;
  dt_pthread_mutex_t backbuf_mutex;

  /* Final output buffer (filled by the last module) */
  uint8_t *backbuf;
  size_t   backbuf_size;
  int      backbuf_width, backbuf_height;

  /* Final pixel dimensions reported after all geometry transformations */
  int final_width, final_height;
} dt_dev_pixelpipe_t;

static inline bool dt_pipe_shutdown(dt_dev_pixelpipe_t *pipe) {
  return dt_atomic_get_int(&pipe->shutdown) != DT_DEV_PIXELPIPE_STOP_NO;
}

/* ── dt_iop_module_so_t ──────────────────────────────────────────────────── */
/*
 * The shared-object / static description of an IOP type.
 * One per operation (e.g. "exposure"), shared across all instances.
 *
 * Only the fields needed for headless processing are retained.
 */
typedef struct dt_iop_module_so_t
{
  dt_dev_operation_t op;

  /** Process function pointer set by the module at load time. */
  void (*process_plain)(struct dt_iop_module_t *self,
                        struct dt_dev_pixelpipe_iop_t *piece,
                        const void *const i,
                        void *const o,
                        const dt_iop_roi_t *const roi_in,
                        const dt_iop_roi_t *const roi_out);

  dt_iop_global_data_t *data;
  dt_iop_module_state_t state;

  bool have_introspection;
  bool pref_based_presets;

  /** Returns IOP flags (combination of dt_iop_flags_t). */
  int (*flags)(void);
  /** Returns IOP tags (combination of dt_iop_tags_t). */
  int (*operation_tags)(void);
} dt_iop_module_so_t;

/* Helper: check if a module's so matches a given op name */
static inline bool dt_iop_module_is(const struct dt_iop_module_so_t *so,
                                    const char *op)
{
  return so && (strncmp(so->op, op, sizeof(so->op)) == 0);
}

/* ── dt_iop_module_t ─────────────────────────────────────────────────────── */
/*
 * A single instance of an IOP in a development history stack.
 *
 * Removed vs. original:
 *   - All GtkWidget* fields
 *   - GUI-only colour picker fields
 *   - Histogram/request_mask display fields
 *   - Bauhaus widget list
 *   - Guides toggle/combo
 *
 * Retained: everything needed by process(), commit_params(), init_pipe(),
 * cleanup_pipe(), modify_roi_*(), legacy_params(), default_colorspace().
 */
typedef struct dt_iop_module_t
{
  /** String identifying this operation (e.g. "exposure"). */
  dt_dev_operation_t op;

  /** Position in the history stack (0-based, higher = later). */
  int32_t instance;

  /** Sort key; pipeline is ordered by ascending iop_order. */
  int iop_order;

  /** Is this module enabled? */
  bool enabled;
  bool default_enabled;

  /** Module parameters (size = params_size bytes). */
  dt_iop_params_t *params;
  dt_iop_params_t *default_params;
  int32_t          params_size;

  /** Per-pipeline private data (not per-instance). */
  dt_iop_global_data_t *global_data;

  /** Per-instance data (allocated in init()). */
  dt_iop_module_data_t *data;

  /** GUI data pointer — always NULL in headless mode. */
  dt_iop_gui_data_t *gui_data;
  dt_pthread_mutex_t gui_lock;

  /** Blending parameters. */
  struct dt_develop_blend_params_t *blend_params;
  struct dt_develop_blend_params_t *default_blendop_params;

  /** Reference to the static module descriptor. */
  dt_iop_module_so_t *so;

  /** Multi-instance support. */
  int  multi_priority;
  char multi_name[128];
  bool multi_name_hand_edited;

  /** CPU process function pointer (mirrors so->process_plain). */
  void (*process_plain)(struct dt_iop_module_t *self,
                        struct dt_dev_pixelpipe_iop_t *piece,
                        const void *const i,
                        void *const o,
                        const dt_iop_roi_t *const roi_in,
                        const dt_iop_roi_t *const roi_out);

  /** Full-buffer CPU process (same signature, alternative name used by some modules). */
  void (*process)(struct dt_iop_module_t *self,
                  struct dt_dev_pixelpipe_iop_t *piece,
                  const void *const i,
                  void *const o,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out);

  /** Tiled CPU process. */
  void (*process_tiling)(struct dt_iop_module_t *self,
                         struct dt_dev_pixelpipe_iop_t *piece,
                         const void *const i,
                         void *const o,
                         const dt_iop_roi_t *const roi_in,
                         const dt_iop_roi_t *const roi_out,
                         const int in_bpp);

  /** Compute output ROI from input ROI. */
  void (*modify_roi_out)(struct dt_iop_module_t *self,
                         struct dt_dev_pixelpipe_iop_t *piece,
                         dt_iop_roi_t *roi_out,
                         const dt_iop_roi_t *roi_in);

  /** Compute required input ROI from desired output ROI. */
  void (*modify_roi_in)(struct dt_iop_module_t *self,
                        struct dt_dev_pixelpipe_iop_t *piece,
                        const dt_iop_roi_t *roi_out,
                        dt_iop_roi_t *roi_in);

  /** Populate output buffer descriptor. */
  void (*output_format)(struct dt_iop_module_t *self,
                        struct dt_dev_pixelpipe_t *pipe,
                        struct dt_dev_pixelpipe_iop_t *piece,
                        struct dt_iop_buffer_dsc_t *dsc);

  /** Return required input colorspace. */
  int (*input_colorspace)(struct dt_iop_module_t *self,
                          struct dt_dev_pixelpipe_t *pipe,
                          struct dt_dev_pixelpipe_iop_t *piece);

  /** Return produced output colorspace. */
  int (*output_colorspace)(struct dt_iop_module_t *self,
                           struct dt_dev_pixelpipe_t *pipe,
                           struct dt_dev_pixelpipe_iop_t *piece);

  /** Return colorspace for blending (may differ from output). */
  int (*blend_colorspace)(struct dt_iop_module_t *self,
                          struct dt_dev_pixelpipe_t *pipe,
                          struct dt_dev_pixelpipe_iop_t *piece);

  /** Compute tiling requirements. */
  void (*tiling_callback)(struct dt_iop_module_t *self,
                          struct dt_dev_pixelpipe_iop_t *piece,
                          const dt_iop_roi_t *roi_in,
                          const dt_iop_roi_t *roi_out,
                          struct dt_develop_tiling_t *tiling);

  /** Returns IOP flags (combination of dt_iop_flags_t). */
  int (*flags)(void);

  /** Returns IOP tags (combination of dt_iop_tags_t). */
  int (*operation_tags)(void);

  /** Sort key for pipeline ordering (used by iop_order) */
  int position;

  bool have_introspection;

  /** Raster mask plumbing (opaque; used by blend code). */
  struct {
    struct { void *users; void *masks; } source; /* GHashTable* */
    struct { struct dt_iop_module_t *source; dt_mask_id_t id; } sink;
  } raster_mask;

  /** Back-pointer to the develop object (opaque in standalone mode). */
  void *dev; /* struct dt_develop_t* */

  /** Picked colour storage (filled during eval; ignored in headless). */
  dt_aligned_pixel_t picked_color,        picked_color_min,        picked_color_max;
  dt_aligned_pixel_t picked_output_color, picked_output_color_min, picked_output_color_max;

  /* Histogram (NULL in headless) */
  uint32_t *histogram;
  dt_dev_histogram_stats_t histogram_stats;
  uint32_t histogram_max[4];
  dt_iop_colorspace_type_t histogram_cst;
  bool histogram_middle_grey;

  /** Trouble flag; ignored in headless mode. */
  bool has_trouble;

  /** Whether the enable button should be hidden (UI hint only). */
  bool hide_enable_button;
} dt_iop_module_t;

/* ── Minimal darktable_t global state ────────────────────────────────────── */
/*
 * The full darktable has a massive global struct.  libdtpipe replaces it with
 * a tiny struct that holds only what the extracted IOPs actually touch.
 */
typedef struct darktable_t
{
  dt_codepath_t codepath;
  int32_t       num_openmp_threads;
  uint32_t      unmuted;  /* debug flags bitmask */
  void         *iop;      /* GList<dt_iop_module_so_t*> — populated by init */
  void         *iop_order_list;
  void         *color_profiles; /* struct dt_colorspaces_t* (opaque) */
  char         *datadir;
  char         *sharedir;
  char         *tmpdir;
  char         *configdir;
  char         *cachedir;
} darktable_t;

extern darktable_t darktable;

/* ── Convenience: module description helper ──────────────────────────────── */

/* IOPs call this to set their tooltip text; it's a no-op in headless mode. */
static inline const char **dt_iop_set_description(
  struct dt_iop_module_t *module,
  const char *main_text,
  const char *purpose,
  const char *input,
  const char *process,
  const char *output)
{
  (void)module; (void)main_text; (void)purpose;
  (void)input;  (void)process;   (void)output;
  return NULL;
}

/* ── Unreachable-codepath helper ─────────────────────────────────────────── */

static inline void dt_unreachable_codepath_with_caller(
  const char *desc, const char *file, int line, const char *func)
{
  (void)desc; (void)file; (void)line; (void)func;
  __builtin_unreachable();
}
#define dt_unreachable_codepath() \
  dt_unreachable_codepath_with_caller("unreachable", __FILE__, __LINE__, __FUNCTION__)
#define dt_unreachable_codepath_with_desc(D) \
  dt_unreachable_codepath_with_caller(D, __FILE__, __LINE__, __FUNCTION__)

/* ── Worker thread count ─────────────────────────────────────────────────── */

static inline size_t dt_get_num_procs(void) {
#ifdef _OPENMP
  return (size_t)(omp_get_num_procs() > 0 ? omp_get_num_procs() : 1);
#else
  return 1;
#endif
}

/* ── FMA helper ──────────────────────────────────────────────────────────── */

#ifdef FP_FAST_FMAF
#  define DT_FMA(x, y, z) fmaf(x, y, z)
#else
#  define DT_FMA(x, y, z) ((x) * (y) + (z))
#endif

/* ── CPU clone-target attribute ──────────────────────────────────────────── */

#if __has_attribute(target_clones) && !defined(_WIN32) && !defined(NATIVE_ARCH) \
    && !defined(__APPLE__) && defined(__GLIBC__)
#  if defined(__amd64__) || defined(__x86_64__)
#    define __DT_CLONE_TARGETS__ \
       __attribute__((target_clones("default","sse2","sse4.1","avx","avx2","fma4")))
#  else
#    define __DT_CLONE_TARGETS__
#  endif
#else
#  define __DT_CLONE_TARGETS__
#endif

/* ── Buffer descriptor helpers ───────────────────────────────────────────── */

/**
 * Return byte-per-pixel for a buffer descriptor.
 * float RGBA = 16 bytes, float mono = 4 bytes, uint16 RGBA = 8 bytes.
 */
static inline size_t dt_iop_buffer_dsc_to_bpp(const dt_iop_buffer_dsc_t *dsc)
{
  size_t elem;
  switch(dsc->datatype)
  {
    case TYPE_FLOAT:  elem = sizeof(float);    break;
    case TYPE_UINT16: elem = sizeof(uint16_t); break;
    default:          elem = sizeof(float);    break;
  }
  return elem * dsc->channels;
}

/* ── Buffer alignment check ──────────────────────────────────────────────── */

static inline bool dt_check_aligned(const void *p)
{
  return ((uintptr_t)p & (DT_CACHELINE_BYTES - 1)) == 0;
}

/* ── Image copy helper ───────────────────────────────────────────────────── */

/**
 * Copy nfloats floats from src to dst (aligned, SIMD-friendly).
 */
static inline void dt_iop_image_copy(float *restrict dst,
                                     const float *restrict src,
                                     const size_t nfloats)
{
  memcpy(dst, src, nfloats * sizeof(float));
}

static inline void dt_iop_image_copy_by_size(void *restrict dst,
                                             const void *restrict src,
                                             const int width,
                                             const int height,
                                             const int ch)
{
  memcpy(dst, src, (size_t)width * height * ch * sizeof(float));
}

/* ── Tiling memory-fit test ──────────────────────────────────────────────── */

/**
 * Returns true if the module+blending fits in host memory without tiling.
 */
static inline bool dt_tiling_piece_fits_host_memory(
  const struct dt_dev_pixelpipe_iop_t *piece,
  const size_t width, const size_t height, const size_t bpp,
  const float factor, const size_t overhead)
{
  (void)piece;
  /* Simple heuristic: factor * w * h * bpp + overhead < 80% of available RAM.
     We use a conservative 2 GB limit when we can't query the OS. */
  const size_t required = (size_t)(factor * (float)(width * height * bpp)) + overhead;
  const size_t limit = (size_t)2 * 1024 * 1024 * 1024; /* 2 GiB */
  return required <= limit;
}

/* ── Colorspace transform stub ───────────────────────────────────────────── */
/*
 * Full colorspace transforms require a complete ICC profile engine (Phase 4+).
 * For now: if in == out, this is a no-op.  Otherwise we log and leave data as-is.
 * IOPs that request a colorspace mismatch will receive unconverted data until
 * the real transform is wired up; for a CPU-only headless pipeline operating
 * entirely in one colorspace this is fine.
 */
static inline void dt_ioppr_transform_image_colorspace(
  const struct dt_iop_module_t *module,
  void *src, void *dst,
  const int width, const int height,
  const int cst_from, const int cst_to,
  int *converted_cst,
  const dt_iop_order_iccprofile_info_t *profile)
{
  (void)module; (void)profile;
  /* Passthrough stub: mark as converted to target space regardless.
     Full ICC-based transform is deferred to Phase 4 color management. */
  if(src != dst)
    memcpy(dst, src, (size_t)width * height * 4 * sizeof(float));
  if(converted_cst) *converted_cst = cst_to;
  (void)cst_from;
}

static inline const dt_iop_order_iccprofile_info_t *
dt_ioppr_get_pipe_work_profile_info(const struct dt_dev_pixelpipe_t *pipe)
{
  (void)pipe;
  return NULL;
}

static inline const dt_iop_order_iccprofile_info_t *
dt_ioppr_get_pipe_current_profile_info(const struct dt_iop_module_t *module,
                                       const struct dt_dev_pixelpipe_t *pipe)
{
  (void)module; (void)pipe;
  return NULL;
}

static inline bool dt_is_valid_colormatrix(float v)
{
  return !isnan(v) && !isinf(v);
}

/* ── Blending stubs ──────────────────────────────────────────────────────── */

static inline dt_iop_colorspace_type_t
dt_develop_blend_colorspace(const struct dt_dev_pixelpipe_iop_t *piece,
                             dt_iop_colorspace_type_t cst)
{
  (void)piece;
  return cst; /* passthrough: blend in whatever space the module outputs */
}

static inline void dt_develop_blend_process(
  struct dt_iop_module_t *module,
  struct dt_dev_pixelpipe_iop_t *piece,
  const void *input, void *output,
  const dt_iop_roi_t *roi_in,
  const dt_iop_roi_t *roi_out)
{
  /* No-op stub: full blending implementation is deferred. */
  (void)module; (void)piece; (void)input; (void)output;
  (void)roi_in; (void)roi_out;
}

/* Tiling requirements for blend operations */
static inline void tiling_callback_blendop(
  struct dt_iop_module_t *module,
  struct dt_dev_pixelpipe_iop_t *piece,
  const dt_iop_roi_t *roi_in,
  const dt_iop_roi_t *roi_out,
  dt_develop_tiling_t *tiling)
{
  (void)module; (void)piece; (void)roi_in; (void)roi_out;
  tiling->factor   = 2.0f;
  tiling->factor_cl = 2.0f;
  tiling->maxbuf   = 0;
  tiling->maxbuf_cl = 0;
  tiling->overhead = 0;
  tiling->overlap  = 0;
  tiling->xalign   = 1;
  tiling->yalign   = 1;
}

/* ── Clip-and-zoom ───────────────────────────────────────────────────────── */

/**
 * Scale a float-RGBA input buffer into an output buffer according to ROIs.
 * Simple bilinear implementation sufficient for headless export.
 */
static inline void dt_iop_clip_and_zoom(float *restrict out,
                                        const float *restrict in,
                                        const dt_iop_roi_t *const roi_out,
                                        const dt_iop_roi_t *const roi_in)
{
  const float scalex = (float)roi_in->width  / (float)roi_out->width;
  const float scaley = (float)roi_in->height / (float)roi_out->height;

  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    const float fy = (j + 0.5f) * scaley - 0.5f;
    const int   y0 = (int)fy;
    const int   y1 = y0 + 1;
    const float dy = fy - (float)y0;

    const int iy0 = CLAMPS(y0, 0, roi_in->height - 1);
    const int iy1 = CLAMPS(y1, 0, roi_in->height - 1);

    for(int i = 0; i < roi_out->width; i++)
    {
      const float fx = (i + 0.5f) * scalex - 0.5f;
      const int   x0 = (int)fx;
      const int   x1 = x0 + 1;
      const float dx = fx - (float)x0;

      const int ix0 = CLAMPS(x0, 0, roi_in->width - 1);
      const int ix1 = CLAMPS(x1, 0, roi_in->width - 1);

      const float *p00 = in + 4 * (iy0 * roi_in->width + ix0);
      const float *p01 = in + 4 * (iy0 * roi_in->width + ix1);
      const float *p10 = in + 4 * (iy1 * roi_in->width + ix0);
      const float *p11 = in + 4 * (iy1 * roi_in->width + ix1);

      float *o = out + 4 * (j * roi_out->width + i);
      for(int c = 0; c < 4; c++)
      {
        o[c] = (1.0f - dy) * ((1.0f - dx) * p00[c] + dx * p01[c])
                     + dy  * ((1.0f - dx) * p10[c] + dx * p11[c]);
      }
    }
  }
}

/* ── Output format helper ────────────────────────────────────────────────── */

/**
 * Determine the output buffer descriptor for a module/pipe/piece combination.
 * If the module provides output_format(), call it; otherwise pass dsc through.
 */
static inline void get_output_format(struct dt_iop_module_t *module,
                                     struct dt_dev_pixelpipe_t *pipe,
                                     struct dt_dev_pixelpipe_iop_t *piece,
                                     void *dev_unused,
                                     dt_iop_buffer_dsc_t *dsc)
{
  (void)dev_unused;
  if(module && module->output_format)
    module->output_format(module, pipe, piece, dsc);
  /* else leave dsc unchanged (propagated from upstream) */
}

/* ── Default ROI callbacks ───────────────────────────────────────────────── */

/**
 * Default modify_roi_out: output ROI equals input ROI (no geometry change).
 */
static inline void dt_iop_default_modify_roi_out(
  struct dt_iop_module_t *self,
  struct dt_dev_pixelpipe_iop_t *piece,
  dt_iop_roi_t *roi_out,
  const dt_iop_roi_t *roi_in)
{
  (void)self; (void)piece;
  *roi_out = *roi_in;
}

/**
 * Default modify_roi_in: input ROI equals output ROI (no geometry change).
 */
static inline void dt_iop_default_modify_roi_in(
  struct dt_iop_module_t *self,
  struct dt_dev_pixelpipe_iop_t *piece,
  const dt_iop_roi_t *roi_out,
  dt_iop_roi_t *roi_in)
{
  (void)self; (void)piece;
  *roi_in = *roi_out;
}

/**
 * Default tiling callback: minimal tiling requirements (factor = 2 for in+out).
 */
static inline void dt_iop_default_tiling_callback(
  struct dt_iop_module_t *self,
  struct dt_dev_pixelpipe_iop_t *piece,
  const dt_iop_roi_t *roi_in,
  const dt_iop_roi_t *roi_out,
  dt_develop_tiling_t *tiling)
{
  (void)self; (void)piece; (void)roi_in; (void)roi_out;
  tiling->factor   = 2.0f;
  tiling->factor_cl = 2.0f;
  tiling->maxbuf   = 0;
  tiling->maxbuf_cl = 0;
  tiling->overhead = 0;
  tiling->overlap  = 0;
  tiling->xalign   = 1;
  tiling->yalign   = 1;
}

/* ── Pipe cache stubs ────────────────────────────────────────────────────── */
/*
 * The original darktable uses a multi-entry LRU cache (pixelpipe_cache.c) to
 * avoid re-processing unchanged modules.  Implementing that cache requires
 * complex hash-chain management; for Phase 3.5 we use a trivial single-buffer
 * cache that always misses (correctness over performance).
 *
 * The full cache can be added in a later phase once correctness is verified.
 */

static inline dt_hash_t dt_dev_pixelpipe_cache_hash(const dt_iop_roi_t *roi,
                                                     const struct dt_dev_pixelpipe_t *pipe,
                                                     int pos)
{
  (void)roi; (void)pipe; (void)pos;
  return DT_INVALID_HASH; /* always miss */
}

static inline bool dt_dev_pixelpipe_cache_available(const struct dt_dev_pixelpipe_t *pipe,
                                                     dt_hash_t hash,
                                                     size_t bufsize)
{
  (void)pipe; (void)hash; (void)bufsize;
  return false;
}

static inline bool dt_dev_pixelpipe_cache_get(struct dt_dev_pixelpipe_t *pipe,
                                              dt_hash_t hash,
                                              size_t bufsize,
                                              void **buf,
                                              dt_iop_buffer_dsc_t **dsc,
                                              struct dt_iop_module_t *module,
                                              bool important)
{
  (void)hash; (void)module; (void)important;
  /* Allocate a fresh aligned buffer every call */
  if(*buf == NULL)
  {
    *buf = dt_alloc_aligned(bufsize);
    (void)pipe;
  }
  return (*buf != NULL);
}

static inline void dt_dev_pixelpipe_invalidate_cacheline(struct dt_dev_pixelpipe_t *pipe,
                                                          void *buf)
{
  (void)pipe; (void)buf;
}

static inline void dt_dev_pixelpipe_cache_invalidate_later(struct dt_dev_pixelpipe_t *pipe,
                                                            int stopper)
{
  (void)pipe; (void)stopper;
}

static inline void dt_dev_pixelpipe_cache_flush(struct dt_dev_pixelpipe_t *pipe)
{
  (void)pipe;
}

/* ── Performance timing stubs ────────────────────────────────────────────── */

typedef struct { double clock; double user; } dt_times_t;

static inline void dt_get_perf_times(dt_times_t *t) { (void)t; }
static inline void dt_show_times_f(const dt_times_t *t, const char *a,
                                    const char *b, ...) { (void)t; (void)a; (void)b; }

/* ── Module skip helper ──────────────────────────────────────────────────── */

static inline bool dt_iop_module_is_skipped(const void *dev,
                                             const struct dt_iop_module_t *mod)
{
  (void)dev; (void)mod;
  return false; /* no skip logic in headless mode */
}

#ifdef __cplusplus
} /* extern "C" */
#endif
