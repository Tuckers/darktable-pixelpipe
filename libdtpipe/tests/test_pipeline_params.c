/*
 * test_pipeline_params.c
 *
 * Task 4.4 verification: exercise dtpipe_set_param_float(),
 * dtpipe_set_param_int(), dtpipe_get_param_float(), and
 * dtpipe_enable_module() via the public libdtpipe API.
 *
 * Tests:
 *   1. NULL argument guards return DTPIPE_ERR_INVALID_ARG.
 *   2. Setting a param on a non-existent module returns DTPIPE_ERR_NOT_FOUND.
 *   3. If a module IS registered (i.e. its so is in the IOP registry and
 *      its params block is allocated), set+get round-trips correctly.
 *   4. Type mismatch returns DTPIPE_ERR_PARAM_TYPE.
 *   5. dtpipe_enable_module() toggles the enabled flag.
 *
 * Note: in the current project phase, no IOP modules are compiled in
 * (the registry in init.c is empty).  Tests 3–5 therefore require a real
 * registered module.  Where no module is present, the tests confirm that
 * the correct error code is returned rather than crashing.  When modules
 * are eventually compiled in, re-run this test to exercise the full path.
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

#define CHECK(cond, msg)                                                        \
  do {                                                                          \
    if(!(cond)) {                                                               \
      fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg));         \
      g_failures++;                                                             \
    } else {                                                                    \
      printf("  OK  %s\n", (msg));                                             \
    }                                                                           \
  } while(0)

#define CHECK_EQ(got, expect, msg)                                              \
  do {                                                                          \
    int _g = (got), _e = (expect);                                              \
    if(_g != _e) {                                                              \
      fprintf(stderr, "FAIL [%s:%d] %s  (got %d, expected %d)\n",              \
              __FILE__, __LINE__, (msg), _g, _e);                               \
      g_failures++;                                                             \
    } else {                                                                    \
      printf("  OK  %s\n", (msg));                                             \
    }                                                                           \
  } while(0)

/* ── Test 1: NULL guards ──────────────────────────────────────────────────── */

static void test_null_guards(void)
{
  printf("\n--- Test 1: NULL argument guards ---\n");

  /* All functions should handle NULL pipe gracefully */
  CHECK_EQ(dtpipe_set_param_float(NULL, "exposure", "exposure", 1.0f),
           DTPIPE_ERR_INVALID_ARG,
           "set_param_float(NULL pipe) -> INVALID_ARG");

  CHECK_EQ(dtpipe_set_param_int(NULL, "exposure", "mode", 0),
           DTPIPE_ERR_INVALID_ARG,
           "set_param_int(NULL pipe) -> INVALID_ARG");

  float val = 0.0f;
  CHECK_EQ(dtpipe_get_param_float(NULL, "exposure", "exposure", &val),
           DTPIPE_ERR_INVALID_ARG,
           "get_param_float(NULL pipe) -> INVALID_ARG");

  CHECK_EQ(dtpipe_enable_module(NULL, "exposure", 1),
           DTPIPE_ERR_INVALID_ARG,
           "enable_module(NULL pipe) -> INVALID_ARG");
}

/* ── Test 2: module not found on real pipeline (registry empty) ───────────── */

static void test_module_not_found(dt_pipe_t *pipe)
{
  printf("\n--- Test 2: module not found (empty registry) ---\n");

  int n = dtpipe_get_module_count();
  printf("  info: %d module(s) registered\n", n);

  if(n == 0)
  {
    printf("  info: registry is empty — confirming NOT_FOUND returns\n");

    CHECK_EQ(dtpipe_set_param_float(pipe, "exposure", "exposure", 1.0f),
             DTPIPE_ERR_NOT_FOUND,
             "set_param_float on unregistered module -> NOT_FOUND");

    CHECK_EQ(dtpipe_set_param_int(pipe, "exposure", "mode", 0),
             DTPIPE_ERR_NOT_FOUND,
             "set_param_int on unregistered module -> NOT_FOUND");

    float v = -1.0f;
    CHECK_EQ(dtpipe_get_param_float(pipe, "exposure", "exposure", &v),
             DTPIPE_ERR_NOT_FOUND,
             "get_param_float on unregistered module -> NOT_FOUND");

    CHECK_EQ(dtpipe_enable_module(pipe, "exposure", 1),
             DTPIPE_ERR_NOT_FOUND,
             "enable_module on unregistered module -> NOT_FOUND");
  }
  else
  {
    printf("  info: registry has modules — skipping empty-registry checks\n");
  }
}

/* ── Test 3: NULL param/module name guards on real pipe ───────────────────── */

static void test_null_name_guards(dt_pipe_t *pipe)
{
  printf("\n--- Test 3: NULL name guards ---\n");

  CHECK_EQ(dtpipe_set_param_float(pipe, NULL, "exposure", 1.0f),
           DTPIPE_ERR_INVALID_ARG,
           "set_param_float(NULL module) -> INVALID_ARG");

  CHECK_EQ(dtpipe_set_param_float(pipe, "exposure", NULL, 1.0f),
           DTPIPE_ERR_INVALID_ARG,
           "set_param_float(NULL param) -> INVALID_ARG");

  float v = 0.0f;
  CHECK_EQ(dtpipe_get_param_float(pipe, NULL, "exposure", &v),
           DTPIPE_ERR_INVALID_ARG,
           "get_param_float(NULL module) -> INVALID_ARG");

  CHECK_EQ(dtpipe_get_param_float(pipe, "exposure", NULL, &v),
           DTPIPE_ERR_INVALID_ARG,
           "get_param_float(NULL param) -> INVALID_ARG");

  CHECK_EQ(dtpipe_get_param_float(pipe, "exposure", "exposure", NULL),
           DTPIPE_ERR_INVALID_ARG,
           "get_param_float(NULL out) -> INVALID_ARG");

  CHECK_EQ(dtpipe_enable_module(pipe, NULL, 1),
           DTPIPE_ERR_INVALID_ARG,
           "enable_module(NULL module) -> INVALID_ARG");
}

/* ── Test 4: set/get round-trip (only runs if module is registered) ─────── */

static void test_set_get_roundtrip(dt_pipe_t *pipe)
{
  printf("\n--- Test 4: set/get round-trip (requires exposure module) ---\n");

  /* If no modules are registered, confirm NOT_FOUND and skip */
  if(dtpipe_get_module_count() == 0)
  {
    printf("  skip: no modules registered\n");
    return;
  }

  /* Check if "exposure" module is present in the pipeline */
  int rc;
  float before = -99.0f;
  rc = dtpipe_get_param_float(pipe, "exposure", "exposure", &before);
  if(rc == DTPIPE_ERR_NOT_FOUND)
  {
    printf("  skip: exposure module not present in this pipeline\n");
    return;
  }
  CHECK_EQ(rc, DTPIPE_OK, "get initial exposure value");

  /* Set a new value */
  float new_val = 2.5f;
  CHECK_EQ(dtpipe_set_param_float(pipe, "exposure", "exposure", new_val),
           DTPIPE_OK, "set exposure to 2.5");

  /* Read it back */
  float after = 0.0f;
  CHECK_EQ(dtpipe_get_param_float(pipe, "exposure", "exposure", &after),
           DTPIPE_OK, "get exposure after set");
  CHECK(after == new_val, "exposure value round-trips correctly");

  /* Restore */
  dtpipe_set_param_float(pipe, "exposure", "exposure", before);

  /* Integer param: mode */
  CHECK_EQ(dtpipe_set_param_int(pipe, "exposure", "mode", 0),
           DTPIPE_OK, "set exposure.mode = 0 (manual)");

  /* Type mismatch: exposure is float, ask for int -> type error */
  CHECK_EQ(dtpipe_set_param_int(pipe, "exposure", "exposure", 1),
           DTPIPE_ERR_PARAM_TYPE,
           "set float param via set_param_int -> PARAM_TYPE");

  /* Type mismatch: mode is int, ask get_param_float -> type error */
  float dummy = 0.0f;
  CHECK_EQ(dtpipe_get_param_float(pipe, "exposure", "mode", &dummy),
           DTPIPE_ERR_PARAM_TYPE,
           "get int param via get_param_float -> PARAM_TYPE");

  /* Unknown param name */
  CHECK_EQ(dtpipe_set_param_float(pipe, "exposure", "nonexistent_field", 0.0f),
           DTPIPE_ERR_NOT_FOUND,
           "set unknown param name -> NOT_FOUND");
}

/* ── Test 5: enable / disable module ─────────────────────────────────────── */

static void test_enable_module(dt_pipe_t *pipe)
{
  printf("\n--- Test 5: enable/disable module ---\n");

  if(dtpipe_get_module_count() == 0)
  {
    printf("  skip: no modules registered\n");
    return;
  }

  /* Find any module that is present */
  const char *op = dtpipe_get_module_name(0);
  if(!op)
  {
    printf("  skip: module name lookup failed\n");
    return;
  }

  CHECK_EQ(dtpipe_enable_module(pipe, op, 0), DTPIPE_OK, "disable module");
  CHECK_EQ(dtpipe_enable_module(pipe, op, 1), DTPIPE_OK, "enable module");
  printf("  info: toggled module '%s'\n", op);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
  const char *raf_path = (argc > 1) ? argv[1]
    : "../../test-image/DSCF4379.RAF";

  printf("=== Task 4.4 verification: parameter access ===\n");

  /* Library init */
  int rc = dtpipe_init(NULL);
  if(rc != DTPIPE_OK && rc != DTPIPE_ERR_ALREADY_INIT)
  {
    fprintf(stderr, "dtpipe_init failed: %d\n", rc);
    return 1;
  }

  /* Test 1 requires no pipeline */
  test_null_guards();

  /* Load image and create pipeline for remaining tests */
  dt_image_t *img = dtpipe_load_raw(raf_path);
  if(!img)
  {
    fprintf(stderr, "  warning: could not load '%s' (%s)\n",
            raf_path, dtpipe_get_last_error());
    fprintf(stderr, "  Tests 2-5 require an image; running limited tests.\n");

    printf("\n=== Results: %d failure(s) ===\n", g_failures);
    dtpipe_cleanup();
    return g_failures ? 1 : 0;
  }

  dt_pipe_t *pipe = dtpipe_create(img);
  if(!pipe)
  {
    fprintf(stderr, "  dtpipe_create failed\n");
    dtpipe_free_image(img);
    dtpipe_cleanup();
    return 1;
  }

  test_module_not_found(pipe);
  test_null_name_guards(pipe);
  test_set_get_roundtrip(pipe);
  test_enable_module(pipe);

  dtpipe_free(pipe);
  dtpipe_free_image(img);
  dtpipe_cleanup();

  printf("\n=== Results: %d failure(s) ===\n", g_failures);
  return g_failures ? 1 : 0;
}
