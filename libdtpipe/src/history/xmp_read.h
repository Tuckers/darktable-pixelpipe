/*
 * history/xmp_read.h - XMP sidecar reading for libdtpipe
 *
 * Internal header.  Do NOT include from public API files.
 *
 * Implements dtpipe_load_xmp() declared in dtpipe.h.
 * Reads a darktable XMP sidecar file and applies its history stack to a
 * live pipeline.
 *
 * Requires pugixml (already linked via CMakeLists.txt).
 */

#pragma once

#include "dtpipe.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dtpipe_load_xmp_impl - internal entry point.
 *
 * Same semantics as the public dtpipe_load_xmp():
 *   - Parses the XMP file.
 *   - Applies enabled states and params to the pipeline modules.
 * Returns DTPIPE_OK on success, or a negative error code.
 */
int dtpipe_load_xmp_impl(dt_pipe_t *pipe, const char *path);

#ifdef __cplusplus
}
#endif
