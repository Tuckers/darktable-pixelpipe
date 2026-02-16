/*
 * history/serialize.h - History JSON serialization for libdtpipe
 *
 * Internal header.  Do NOT include from public API files.
 *
 * Implements dtpipe_serialize_history() declared in dtpipe.h.
 * The output format is specified in docs/history-format.md.
 *
 * No third-party JSON library is required; JSON is generated via
 * a simple growable string buffer written with fprintf-style helpers.
 */

#pragma once

#include "dtpipe.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dtpipe_serialize_history_impl - internal entry point.
 *
 * Same semantics as the public dtpipe_serialize_history():
 * returns a heap-allocated NUL-terminated JSON string on success.
 * The caller must free() it.
 * Returns NULL on allocation failure.
 */
char *dtpipe_serialize_history_impl(dt_pipe_t *pipe);

#ifdef __cplusplus
}
#endif
