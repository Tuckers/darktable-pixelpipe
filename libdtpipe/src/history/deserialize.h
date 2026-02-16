/*
 * history/deserialize.h - History JSON deserialization for libdtpipe
 *
 * Internal header.  Do NOT include from public API files.
 *
 * Implements dtpipe_load_history() declared in dtpipe.h.
 * The expected input format is specified in docs/history-format.md.
 *
 * No third-party JSON library is required; JSON is parsed via a minimal
 * recursive-descent parser that handles only the subset of JSON produced
 * by serialize.c.
 */

#pragma once

#include "dtpipe.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dtpipe_load_history_impl - internal entry point.
 *
 * Same semantics as the public dtpipe_load_history():
 *   - Parses the JSON string.
 *   - Applies enabled states and params to the pipeline modules.
 * Returns DTPIPE_OK on success, or a negative error code.
 */
int dtpipe_load_history_impl(dt_pipe_t *pipe, const char *json);

#ifdef __cplusplus
}
#endif
