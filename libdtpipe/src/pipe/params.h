/*
 * params.h - Parameter descriptor tables for libdtpipe
 *
 * Internal header.  Do NOT include from public API files.
 *
 * Each IOP module's parameter struct is described by a table of
 * dt_param_desc_t entries.  The table maps param_name → (offset, type, size)
 * so that dtpipe_set_param_float / dtpipe_get_param_float / dtpipe_set_param_int
 * can read/write fields generically without knowing the concrete struct layout
 * at compile time.
 *
 * Design (Option B from the project plan):
 *   - Hand-written descriptors for Tier 1 + Tier 2 modules.
 *   - The descriptor array for a module is registered in _module_param_tables[]
 *     keyed by operation name.
 *   - dtpipe_lookup_param() searches the table and returns a dt_param_desc_t*.
 *
 * Adding a new module:
 *   1. Define a static dt_param_desc_t array named _params_<opname>[].
 *   2. Add an entry to _module_param_tables[] in params.c.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Supported parameter types ───────────────────────────────────────────── */

typedef enum dt_param_type_t
{
  DT_PARAM_FLOAT  = 0,
  DT_PARAM_INT    = 1,
  DT_PARAM_UINT32 = 2,
  DT_PARAM_BOOL   = 3,
} dt_param_type_t;

/* ── Single parameter descriptor ─────────────────────────────────────────── */

typedef struct dt_param_desc_t
{
  const char     *name;    /* field name as it appears in the params struct  */
  size_t          offset;  /* byte offset within the params struct           */
  dt_param_type_t type;    /* scalar type                                    */
  size_t          size;    /* sizeof the field (for bounds-check)            */
  float           min;     /* soft lower bound (warn, not error)             */
  float           max;     /* soft upper bound (warn, not error)             */
} dt_param_desc_t;

/* ── Module descriptor table entry ──────────────────────────────────────── */

typedef struct dt_module_param_table_t
{
  const char          *op;       /* IOP operation name                       */
  const dt_param_desc_t *params; /* pointer to descriptor array              */
  int                   count;   /* number of entries in params[]            */
} dt_module_param_table_t;

/* ── Public helpers ──────────────────────────────────────────────────────── */

/**
 * Look up a parameter descriptor for the given (op, param_name) pair.
 * Returns NULL if not found.
 */
const dt_param_desc_t *dtpipe_lookup_param(const char *op,
                                           const char *param_name);

/**
 * Return the number of parameter descriptors registered for an operation.
 * Returns -1 if the operation is not found.
 */
int dtpipe_param_count(const char *op);

/**
 * Return the i-th parameter descriptor for the given operation.
 * Returns NULL if the operation is not found or i is out of range.
 * Useful for iterating all parameters of a module (e.g. during serialization).
 */
const dt_param_desc_t *dtpipe_get_param_desc(const char *op, int i);

#ifdef __cplusplus
}
#endif
