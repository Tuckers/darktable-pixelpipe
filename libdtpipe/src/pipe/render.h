/*
 * render.h - Internal render helpers shared between render.c and export.c
 */

#pragma once

#include "pipe/create.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ensure pipe->input_buf is allocated and populated from the raw pixel data.
 * Idempotent: if the buffer already exists it is returned as-is.
 * Returns true on success, false on allocation failure.
 */
bool dtpipe_ensure_input_buf(dt_pipe_t *pipe);

#ifdef __cplusplus
}
#endif
