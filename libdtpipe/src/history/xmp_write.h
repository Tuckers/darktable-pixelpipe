/*
 * history/xmp_write.h - XMP sidecar writing for libdtpipe
 *
 * Internal header.  Do NOT include from public API files.
 *
 * Implements dtpipe_save_xmp() declared in dtpipe.h.
 * Writes a darktable-compatible XMP sidecar file from the current pipeline
 * module state so that darktable can re-open and apply the same edits.
 *
 * Requires pugixml (already linked via CMakeLists.txt).
 */

#pragma once

#include "dtpipe.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dtpipe_save_xmp_impl - internal entry point.
 *
 * Same semantics as the public dtpipe_save_xmp():
 *   - Serializes the enabled state and params for each pipeline module.
 *   - Writes a darktable-compatible XMP file to path.
 * Returns DTPIPE_OK on success, or a negative error code.
 */
int dtpipe_save_xmp_impl(dt_pipe_t *pipe, const char *path);

#ifdef __cplusplus
}
#endif
