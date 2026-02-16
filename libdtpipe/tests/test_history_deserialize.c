/*
 * test_history_deserialize.c
 *
 * Task 5.3 verification: exercise dtpipe_load_history() and the
 * serialize → deserialize → serialize round-trip.
 *
 * Tests:
 *   1. NULL args return DTPIPE_ERR_INVALID_ARG.
 *   2. Missing "version" key returns DTPIPE_ERR_FORMAT.
 *   3. Unsupported major version returns DTPIPE_ERR_FORMAT.
 *   4. Malformed JSON returns DTPIPE_ERR_FORMAT.
 *   5. Empty modules object succeeds (no-op).
 *   6. Unknown module is warned and skipped (succeeds).
 *   7. Param round-trip: set exposure param, serialize, load into fresh
 *      pipeline, read back — values must match.
 *   8. Enable/disable round-trip: disable a module, serialize, reload,
 *      check module is disabled.
 *   9. Full serialize → load_history → serialize: second JSON equals first
 *      (idempotent, when modules are registered).
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

#define CHECK_EQ_F(got, expect, msg)                                            \
  do {                                                                          \
    float _g = (got), _e = (expect);                                            \
    float _diff = _g - _e; if(_diff < 0) _diff = -_diff;                       \
    if(_diff > 1e-4f) {                                                         \
      fprintf(stderr, "FAIL [%s:%d] %s  (got %g, expected %g)\n",              \
              __FILE__, __LINE__, (msg), (double)_g, (double)_e);               \
      g_failures++;                                                             \
    } else {                                                                    \
      printf("  OK  %s\n", (msg));                                             \
    }                                                                           \
  } while(0)

/* ── Test 1: NULL argument guard ─────────────────────────────────────────── */

static void test_null_args(void)
{
  printf("\n--- Test 1: NULL argument guard ---\n");

  /* Load a real pipeline just to test the NULL json arg */
  dt_pipe_t *pipe = dtpipe_create(NULL);
  /* dtpipe_create(NULL) may or may not succeed depending on implementation.
     We only test the NULL-json path if we have a pipe. */
  if(pipe)
  {
    int rc = dtpipe_load_history(pipe, NULL);
    CHECK_EQ(rc, DTPIPE_ERR_INVALID_ARG, "load_history(pipe, NULL) = INVALID_ARG");
    dtpipe_free(pipe);
  }
  else
  {
    printf("  skip: dtpipe_create(NULL) unavailable\n");
  }

  int rc2 = dtpipe_load_history(NULL, "{}");
  CHECK_EQ(rc2, DTPIPE_ERR_INVALID_ARG, "load_history(NULL, json) = INVALID_ARG");
}

/* ── Test 2: missing version key ─────────────────────────────────────────── */

static void test_missing_version(dt_pipe_t *pipe)
{
  printf("\n--- Test 2: missing 'version' key ---\n");
  const char *json = "{ \"modules\": {} }";
  int rc = dtpipe_load_history(pipe, json);
  CHECK_EQ(rc, DTPIPE_ERR_FORMAT, "missing version returns ERR_FORMAT");
}

/* ── Test 3: unsupported major version ───────────────────────────────────── */

static void test_bad_version(dt_pipe_t *pipe)
{
  printf("\n--- Test 3: unsupported major version ---\n");
  const char *json = "{ \"version\": \"99.0\", \"modules\": {} }";
  int rc = dtpipe_load_history(pipe, json);
  CHECK_EQ(rc, DTPIPE_ERR_FORMAT, "version 99.0 returns ERR_FORMAT");
}

/* ── Test 4: malformed JSON ───────────────────────────────────────────────── */

static void test_malformed_json(dt_pipe_t *pipe)
{
  printf("\n--- Test 4: malformed JSON ---\n");
  /* Missing closing brace */
  const char *json = "{ \"version\": \"1.0\", \"modules\": {";
  int rc = dtpipe_load_history(pipe, json);
  CHECK_EQ(rc, DTPIPE_ERR_FORMAT, "truncated JSON returns ERR_FORMAT");

  /* Completely invalid */
  const char *json2 = "not json at all";
  int rc2 = dtpipe_load_history(pipe, json2);
  CHECK_EQ(rc2, DTPIPE_ERR_FORMAT, "non-JSON string returns ERR_FORMAT");
}

/* ── Test 5: empty modules object ────────────────────────────────────────── */

static void test_empty_modules(dt_pipe_t *pipe)
{
  printf("\n--- Test 5: empty modules object ---\n");
  const char *json =
    "{ \"version\": \"1.0\", \"generator\": \"test\","
    "  \"modules\": {} }";
  int rc = dtpipe_load_history(pipe, json);
  CHECK_EQ(rc, DTPIPE_OK, "empty modules object succeeds");
}

/* ── Test 6: unknown module is skipped ───────────────────────────────────── */

static void test_unknown_module(dt_pipe_t *pipe)
{
  printf("\n--- Test 6: unknown module is skipped ---\n");
  const char *json =
    "{ \"version\": \"1.0\","
    "  \"modules\": {"
    "    \"nonexistent_module_xyz\": {"
    "      \"enabled\": true,"
    "      \"version\": 1,"
    "      \"params\": { \"foo\": 1.0 }"
    "    }"
    "  }"
    "}";
  int rc = dtpipe_load_history(pipe, json);
  CHECK_EQ(rc, DTPIPE_OK, "unknown module is skipped (succeeds)");
}

/* ── Test 7: param round-trip (requires modules registered) ─────────────── */

static void test_param_roundtrip(dt_image_t *img)
{
  printf("\n--- Test 7: param round-trip ---\n");

  if(dtpipe_get_module_count() == 0)
  {
    printf("  skip: no modules registered\n");
    return;
  }

  /* Create pipeline A, set exposure, serialize */
  dt_pipe_t *pipeA = dtpipe_create(img);
  if(!pipeA) { printf("  skip: dtpipe_create failed\n"); return; }

  /* Check exposure module is available */
  float orig = 0.0f;
  if(dtpipe_get_param_float(pipeA, "exposure", "exposure", &orig) != DTPIPE_OK)
  {
    printf("  skip: exposure module not present\n");
    dtpipe_free(pipeA);
    return;
  }

  float target = orig + 1.5f;
  CHECK_EQ(dtpipe_set_param_float(pipeA, "exposure", "exposure", target),
           DTPIPE_OK, "set exposure param on pipeA");

  char *json = dtpipe_serialize_history(pipeA);
  CHECK(json != NULL, "serialize pipeA");
  dtpipe_free(pipeA);
  if(!json) return;

  /* Create pipeline B (fresh defaults), load the serialized history */
  dt_pipe_t *pipeB = dtpipe_create(img);
  if(!pipeB)
  {
    printf("  skip: second dtpipe_create failed\n");
    free(json);
    return;
  }

  int rc = dtpipe_load_history(pipeB, json);
  CHECK_EQ(rc, DTPIPE_OK, "load_history into pipeB");

  float readback = 0.0f;
  CHECK_EQ(dtpipe_get_param_float(pipeB, "exposure", "exposure", &readback),
           DTPIPE_OK, "get_param_float after load_history");
  CHECK_EQ_F(readback, target, "exposure round-trips correctly");

  dtpipe_free(pipeB);
  free(json);
}

/* ── Test 8: enable/disable round-trip ───────────────────────────────────── */

static void test_enable_roundtrip(dt_image_t *img)
{
  printf("\n--- Test 8: enable/disable round-trip ---\n");

  if(dtpipe_get_module_count() == 0)
  {
    printf("  skip: no modules registered\n");
    return;
  }

  const char *op = dtpipe_get_module_name(0);
  if(!op) { printf("  skip: no module names\n"); return; }

  /* Pipeline A: disable first module, serialize */
  dt_pipe_t *pipeA = dtpipe_create(img);
  if(!pipeA) { printf("  skip: dtpipe_create failed\n"); return; }

  dtpipe_enable_module(pipeA, op, 0);
  char *json = dtpipe_serialize_history(pipeA);
  CHECK(json != NULL, "serialize with module disabled");
  dtpipe_free(pipeA);
  if(!json) return;

  /* Pipeline B: load history, check module is disabled */
  dt_pipe_t *pipeB = dtpipe_create(img);
  if(!pipeB)
  {
    printf("  skip: second dtpipe_create failed\n");
    free(json);
    return;
  }

  int rc = dtpipe_load_history(pipeB, json);
  CHECK_EQ(rc, DTPIPE_OK, "load_history with disabled module");

  /* Re-enable and check: after loading, module should be disabled.
     We use enable_module(pipe, op, 0) idempotently to verify state by
     comparing JSON before/after. */
  char *json2 = dtpipe_serialize_history(pipeB);
  CHECK(json2 != NULL, "re-serialize pipeB after load");
  if(json2)
    CHECK(strcmp(json, json2) == 0,
          "re-serialized JSON matches original (idempotent)");

  free(json);
  free(json2);
  dtpipe_free(pipeB);
}

/* ── Test 9: full serialize → load → serialize idempotence ───────────────── */

static void test_full_roundtrip(dt_image_t *img)
{
  printf("\n--- Test 9: full serialize → load → serialize idempotence ---\n");

  if(dtpipe_get_module_count() == 0)
  {
    printf("  skip: no modules registered\n");
    return;
  }

  dt_pipe_t *pipeA = dtpipe_create(img);
  if(!pipeA) { printf("  skip: dtpipe_create failed\n"); return; }

  /* Make a non-default change so there's something interesting to round-trip */
  dtpipe_set_param_float(pipeA, "exposure", "exposure", 0.33f);

  char *json1 = dtpipe_serialize_history(pipeA);
  CHECK(json1 != NULL, "first serialization");
  dtpipe_free(pipeA);
  if(!json1) return;

  /* Load into pipeB */
  dt_pipe_t *pipeB = dtpipe_create(img);
  if(!pipeB) { free(json1); printf("  skip\n"); return; }

  int rc = dtpipe_load_history(pipeB, json1);
  CHECK_EQ(rc, DTPIPE_OK, "load_history from first JSON");

  char *json2 = dtpipe_serialize_history(pipeB);
  CHECK(json2 != NULL, "second serialization");
  if(json2)
    CHECK(strcmp(json1, json2) == 0,
          "second JSON equals first (round-trip is idempotent)");

  dtpipe_free(pipeB);
  free(json1);
  free(json2);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
  const char *raf_path = (argc > 1) ? argv[1]
    : "../../test-image/DSCF4379.RAF";

  printf("=== Task 5.3 verification: history deserialization ===\n");

  int rc = dtpipe_init(NULL);
  if(rc != DTPIPE_OK && rc != DTPIPE_ERR_ALREADY_INIT)
  {
    fprintf(stderr, "dtpipe_init failed: %d\n", rc);
    return 1;
  }

  /* Test 1 is independent of image/pipeline */
  test_null_args();

  /* Load image (optional) */
  dt_image_t *img = dtpipe_load_raw(raf_path);
  if(!img)
    fprintf(stderr, "  info: could not load '%s' (%s) — running limited tests\n",
            raf_path, dtpipe_get_last_error());

  /* Tests 2-6 need only a pipeline (img may be NULL for create-less tests) */
  dt_pipe_t *pipe = img ? dtpipe_create(img) : NULL;

  if(pipe)
  {
    test_missing_version(pipe);
    test_bad_version(pipe);
    test_malformed_json(pipe);
    test_empty_modules(pipe);
    test_unknown_module(pipe);
    dtpipe_free(pipe);
  }
  else
  {
    printf("\n  info: no pipeline — skipping tests 2-6\n");
  }

  /* Tests 7-9 need an image for full round-trip */
  if(img)
  {
    test_param_roundtrip(img);
    test_enable_roundtrip(img);
    test_full_roundtrip(img);
  }
  else
  {
    printf("\n  info: no image — skipping tests 7-9\n");
  }

  if(img) dtpipe_free_image(img);
  dtpipe_cleanup();

  printf("\n=== Results: %d failure(s) ===\n", g_failures);
  return g_failures ? 1 : 0;
}
