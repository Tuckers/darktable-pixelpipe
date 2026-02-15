#include "dtpipe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Placeholder initialization - will be fleshed out in Task 3.2 */

dtpipe_error_t dtpipe_init(void)
{
  /* TODO: initialize rawspeed, OpenCL (optional), color management */
  return DTPIPE_OK;
}

void dtpipe_cleanup(void)
{
  /* TODO: release global resources */
}

void dtpipe_free_buffer(void *buf)
{
  free(buf);
}

void dtpipe_free_string(char *str)
{
  free(str);
}
