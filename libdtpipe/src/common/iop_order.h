/*
 * iop_order.h - IOP execution order for libdtpipe
 *
 * Ported from darktable src/common/iop_order.h.
 *
 * Changes from original:
 *   - No GLib (GList, gboolean, etc.) — uses a simple singly-linked list
 *   - No database integration (SQLite removed)
 *   - dt_develop_t / dt_iop_module_t dependencies removed from public API
 *   - Added JSON load/save helpers (replaces DB-backed storage)
 *
 * The iop_order list is an ordered sequence of dt_iop_order_entry_t nodes.
 * Each node carries:
 *   operation   – the module name (e.g. "exposure")
 *   instance    – the multi-instance index (0 for base instance)
 *   iop_order   – integer sort key; pipeline is processed in ascending order
 */

#pragma once

#include "dtpipe_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── dt_iop_order_list_t ─────────────────────────────────────────────────── */

/**
 * A node in an iop-order singly-linked list.
 * Use dt_ioppr_* functions to create, query, and free these lists.
 */
typedef struct dt_iop_order_list_node_t
{
  dt_iop_order_entry_t           entry;
  struct dt_iop_order_list_node_t *next;
} dt_iop_order_list_node_t;

/** Opaque handle for an iop-order list (pointer to first node). */
typedef dt_iop_order_list_node_t *dt_iop_order_list_t;

/* ── Built-in order version tables ──────────────────────────────────────── */

/** Return the name string for a dt_iop_order_t value. */
const char *dt_iop_order_string(dt_iop_order_t order);

/**
 * Return a newly-allocated iop-order list for one of the built-in versions.
 * Caller must free with dt_ioppr_iop_order_list_free().
 * Returns NULL if version is out of range.
 */
dt_iop_order_list_t dt_ioppr_get_iop_order_list_version(dt_iop_order_t version);

/** Detect which built-in version (or DT_IOP_ORDER_CUSTOM) a list matches. */
dt_iop_order_t dt_ioppr_get_iop_order_list_kind(dt_iop_order_list_t list);

/** Free a list returned by dt_ioppr_get_iop_order_list_version() or the
 *  deserialise functions. */
void dt_ioppr_iop_order_list_free(dt_iop_order_list_t list);

/** Deep-copy a list. Caller must free the result. */
dt_iop_order_list_t dt_ioppr_iop_order_copy_deep(dt_iop_order_list_t list);

/* ── Entry lookup ────────────────────────────────────────────────────────── */

/**
 * Return the dt_iop_order_entry_t* for (op_name, multi_priority) in list.
 * Pass multi_priority == -1 to match any instance.
 * Returns NULL if not found.
 * The pointer is into the list node — do not free it separately.
 */
const dt_iop_order_entry_t *dt_ioppr_get_iop_order_entry(dt_iop_order_list_t list,
                                                          const char *op_name,
                                                          int multi_priority);

/**
 * Return the integer iop_order for (op_name, multi_priority).
 * Returns INT_MAX if not found.
 */
int dt_ioppr_get_iop_order(dt_iop_order_list_t list,
                           const char *op_name,
                           int multi_priority);

/**
 * Return the highest iop_order among all entries with operation == op_name.
 * Returns INT_MIN if not found.
 */
int dt_ioppr_get_iop_order_last(dt_iop_order_list_t list,
                                const char *op_name);

/**
 * Returns true if (operation, multi_priority) appears before base_operation
 * in the list (i.e. has a lower iop_order).
 */
bool dt_ioppr_is_iop_before(dt_iop_order_list_t list,
                             const char *base_operation,
                             const char *operation,
                             int multi_priority);

/* ── Sorting helpers ────────────────────────────────────────────────────── */

/** Sort a dt_iop_order_list_t in ascending iop_order order. Returns new head. */
dt_iop_order_list_t dt_ioppr_sort_iop_order_list(dt_iop_order_list_t list);

/* ── Text serialisation (same wire format as darktable) ──────────────────── */

/**
 * Serialise list to a heap-allocated NUL-terminated string of the form:
 *   "op1,inst1,op2,inst2,...,opN,instN"
 * Caller must free with free().
 * Returns NULL on error.
 */
char *dt_ioppr_serialize_text_iop_order_list(dt_iop_order_list_t list);

/**
 * Deserialise a string produced by dt_ioppr_serialize_text_iop_order_list().
 * Returns a newly-allocated list, or NULL on error.
 * Caller must free with dt_ioppr_iop_order_list_free().
 */
dt_iop_order_list_t dt_ioppr_deserialize_text_iop_order_list(const char *buf);

/**
 * Binary serialise (same format as darktable preset blobs).
 * *size is set to the number of bytes written.
 * Caller must free the returned buffer with free().
 * Returns NULL on error.
 */
void *dt_ioppr_serialize_iop_order_list(dt_iop_order_list_t list, size_t *size);

/**
 * Binary deserialise from a buffer of `size` bytes.
 * Returns a newly-allocated list, or NULL on error.
 */
dt_iop_order_list_t dt_ioppr_deserialize_iop_order_list(const char *buf, size_t size);

/* ── JSON I/O ────────────────────────────────────────────────────────────── */

/**
 * Write the iop-order list to a JSON file.
 * Format:
 *   { "version": <int>, "order": [ {"op":"<name>","instance":<int>}, ... ] }
 * Returns true on success.
 */
bool dt_ioppr_write_iop_order_json(dt_iop_order_list_t list,
                                   dt_iop_order_t kind,
                                   const char *path);

/**
 * Read an iop-order list from a JSON file written by dt_ioppr_write_iop_order_json().
 * *kind_out is set to the version field if non-NULL.
 * Returns a newly-allocated list, or NULL on error.
 */
dt_iop_order_list_t dt_ioppr_read_iop_order_json(const char *path,
                                                  dt_iop_order_t *kind_out);

/* ── Rules ───────────────────────────────────────────────────────────────── */

/**
 * A node in the rules singly-linked list.
 */
typedef struct dt_iop_order_rules_node_t
{
  dt_iop_order_rule_t            rule;
  struct dt_iop_order_rules_node_t *next;
} dt_iop_order_rules_node_t;

typedef dt_iop_order_rules_node_t *dt_iop_order_rules_t;

/**
 * Return the list of hard ordering rules (e.g. rawprepare must precede invert).
 * Caller must free with dt_ioppr_iop_order_rules_free().
 */
dt_iop_order_rules_t dt_ioppr_get_iop_order_rules(void);

/** Free a rules list. */
void dt_ioppr_iop_order_rules_free(dt_iop_order_rules_t rules);

#ifdef __cplusplus
}
#endif
