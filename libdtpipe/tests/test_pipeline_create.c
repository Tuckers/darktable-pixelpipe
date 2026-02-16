/*
 * test_pipeline_create.c
 *
 * Task 4.3 verification: exercise dtpipe_create() and dtpipe_free() via the
 * public libdtpipe API.
 *
 * Tests:
 *   1. dtpipe_create(NULL) returns NULL safely.
 *   2. Create a pipeline from a real image, verify handle is non-NULL.
 *   3. dtpipe_get_module_count() returns a non-negative value.
 *   4. Iterate module names — no crash, names are non-NULL strings.
 *   5. dtpipe_free(pipe) succeeds without crash.
 *   6. dtpipe_free(NULL) is a safe no-op.
 *
 * Exit codes:
 *   0 – all checks passed
 *   1 – one or more checks failed
 */

#include "dtpipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static int g_failures = 0;

#define CHECK(cond, msg)                                                      \
  do {                                                                        \
    if(!(cond)) {                                                             \
      fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg));       \
      g_failures++;                                                           \
    } else {                                                                  \
      printf("  OK  %s\n", (msg));                                           \
    }                                                                         \
  } while(0)

/* ── Test 1: NULL image guard ─────────────────────────────────────────────── */

static void test_create_null(void)
{
  printf("\n--- Test 1: dtpipe_create(NULL) is safe ---\n");
  dt_pipe_t *pipe = dtpipe_create(NULL);
  CHECK(pipe == NULL, "dtpipe_create(NULL) returns NULL");
}

/* ── Test 2 + 3 + 4 + 5: create pipeline from real image ─────────────────── */

static void test_create_from_image(const char *raf_path)
{
  printf("\n--- Test 2: load image ---\n");

  int rc = dtpipe_init(NULL);
  CHECK(rc == DTPIPE_OK || rc == DTPIPE_ERR_ALREADY_INIT, "dtpipe_init OK");

  dt_image_t *img = dtpipe_load_raw(raf_path);
  CHECK(img != NULL, "dtpipe_load_raw returned non-NULL");
  if(!img)
  {
    fprintf(stderr, "  (last error: %s)\n", dtpipe_get_last_error());
    return;
  }

  CHECK(dtpipe_get_width(img)  > 0, "image width > 0");
  CHECK(dtpipe_get_height(img) > 0, "image height > 0");

  printf("\n--- Test 3: dtpipe_create from image ---\n");

  dt_pipe_t *pipe = dtpipe_create(img);
  CHECK(pipe != NULL, "dtpipe_create returned non-NULL");

  if(!pipe)
  {
    dtpipe_free_image(img);
    return;
  }

  printf("\n--- Test 4: dtpipe_get_module_count ---\n");

  int count = dtpipe_get_module_count();
  CHECK(count >= 0, "dtpipe_get_module_count() >= 0");
  printf("  info: %d module(s) registered\n", count);

  printf("\n--- Test 5: iterate module names ---\n");

  for(int i = 0; i < count; i++)
  {
    const char *name = dtpipe_get_module_name(i);
    CHECK(name != NULL, "dtpipe_get_module_name returns non-NULL");
    if(name)
      CHECK(name[0] != '\0', "module name is non-empty");
  }

  /* Out-of-range index returns NULL */
  CHECK(dtpipe_get_module_name(count)    == NULL, "module name at count is NULL");
  CHECK(dtpipe_get_module_name(-1)       == NULL, "module name at -1 is NULL");

  printf("\n--- Test 6: dtpipe_free ---\n");

  dtpipe_free(pipe);
  printf("  OK  dtpipe_free did not crash\n");

  dtpipe_free_image(img);
  printf("  OK  dtpipe_free_image did not crash\n");

  printf("\n--- Test 7: dtpipe_free(NULL) is safe ---\n");
  dtpipe_free(NULL);
  printf("  OK  dtpipe_free(NULL) did not crash\n");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
  const char *raf_path = (argc > 1) ? argv[1]
    : "../../test-image/DSCF4379.RAF";

  printf("=== Task 4.3 verification: dtpipe_create / dtpipe_free ===\n");

  test_create_null();
  test_create_from_image(raf_path);

  printf("\n=== Results: %d failure(s) ===\n", g_failures);
  return g_failures ? 1 : 0;
}
