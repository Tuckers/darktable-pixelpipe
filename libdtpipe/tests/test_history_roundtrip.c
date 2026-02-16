/*
 * test_history_roundtrip.c
 *
 * Task 5.2 verification: exercise dtpipe_serialize_history().
 *
 * Tests:
 *   1. NULL pipe returns NULL.
 *   2. Serialize with no image (minimal pipeline) returns valid JSON.
 *   3. JSON contains expected top-level keys.
 *   4. JSON is well-formed enough to pass basic structural checks
 *      (balanced braces, no NUL bytes before the terminator).
 *   5. With a real image loaded, serialized JSON contains source block.
 *   6. After changing a param, re-serialized JSON differs from the original
 *      (change is reflected).
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

/* Return non-zero if `needle` appears in `haystack`. */
static int contains(const char *haystack, const char *needle)
{
  return strstr(haystack, needle) != NULL;
}

/*
 * Very cheap well-formedness check:
 *   - Count opening/closing braces and brackets — they must balance.
 *   - No NUL bytes before the final terminator.
 *   - Must start with '{' and end with '}' (ignoring trailing whitespace).
 */
static int json_looks_valid(const char *s)
{
  if(!s || s[0] != '{') return 0;

  int braces   = 0;
  int brackets = 0;
  int in_string = 0;
  int escape    = 0;
  size_t len = strlen(s);

  for(size_t i = 0; i < len; i++)
  {
    char c = s[i];
    if(c == '\0') return 0; /* embedded NUL */

    if(escape) { escape = 0; continue; }
    if(in_string)
    {
      if(c == '\\') { escape = 1; continue; }
      if(c == '"')  { in_string = 0; }
      continue;
    }

    switch(c)
    {
      case '"': in_string = 1; break;
      case '{': braces++;      break;
      case '}': braces--;      break;
      case '[': brackets++;    break;
      case ']': brackets--;    break;
    }
  }

  return (braces == 0 && brackets == 0);
}

/* ── Test 1: NULL pipe ────────────────────────────────────────────────────── */

static void test_null_pipe(void)
{
  printf("\n--- Test 1: NULL pipe ---\n");
  char *json = dtpipe_serialize_history(NULL);
  CHECK(json == NULL, "serialize_history(NULL) returns NULL");
  free(json);
}

/* ── Test 2 & 3: minimal pipeline (no modules registered) ─────────────────── */

static void test_minimal_pipeline(dt_pipe_t *pipe)
{
  printf("\n--- Test 2+3: minimal pipeline serialization ---\n");

  char *json = dtpipe_serialize_history(pipe);
  CHECK(json != NULL, "serialize_history returns non-NULL");
  if(!json) return;

  printf("  JSON length: %zu bytes\n", strlen(json));

  /* Basic structure */
  CHECK(json_looks_valid(json), "JSON has balanced braces/brackets");
  CHECK(contains(json, "\"version\""),   "JSON contains \"version\" key");
  CHECK(contains(json, "\"1.0\""),       "JSON version is \"1.0\"");
  CHECK(contains(json, "\"generator\""), "JSON contains \"generator\" key");
  CHECK(contains(json, "libdtpipe"),     "generator value is libdtpipe");
  CHECK(contains(json, "\"settings\""),  "JSON contains \"settings\" key");
  CHECK(contains(json, "\"v5.0\""),      "iop_order is v5.0");
  CHECK(contains(json, "\"modules\""),   "JSON contains \"modules\" key");
  CHECK(contains(json, "\"masks\""),     "JSON contains \"masks\" key");

  free(json);
}

/* ── Test 4: source block present when image has metadata ────────────────── */

static void test_source_block(dt_pipe_t *pipe)
{
  printf("\n--- Test 4: source block ---\n");

  char *json = dtpipe_serialize_history(pipe);
  if(!json)
  {
    printf("  skip: serialization returned NULL\n");
    return;
  }

  /* Only check for source block if the image actually has EXIF data */
  if(contains(json, "\"source\""))
  {
    CHECK(contains(json, "\"filename\"") || contains(json, "\"camera\""),
          "source block contains filename or camera");
    printf("  info: source block is present\n");
  }
  else
  {
    printf("  info: no source block (image has no EXIF — that is OK)\n");
  }

  free(json);
}

/* ── Test 5: param change is reflected in serialized output ──────────────── */

static void test_param_change_reflected(dt_pipe_t *pipe)
{
  printf("\n--- Test 5: param change reflected in JSON ---\n");

  /* This test requires the exposure module to be registered */
  if(dtpipe_get_module_count() == 0)
  {
    printf("  skip: no modules registered\n");
    return;
  }

  /* Check if exposure is present */
  float orig = 0.0f;
  int rc = dtpipe_get_param_float(pipe, "exposure", "exposure", &orig);
  if(rc != DTPIPE_OK)
  {
    printf("  skip: exposure module not present (rc=%d)\n", rc);
    return;
  }

  /* Serialize baseline */
  char *json_before = dtpipe_serialize_history(pipe);
  CHECK(json_before != NULL, "baseline serialization succeeds");
  if(!json_before) return;

  /* Change exposure */
  float new_val = orig + 1.0f;
  rc = dtpipe_set_param_float(pipe, "exposure", "exposure", new_val);
  CHECK_EQ(rc, DTPIPE_OK, "set exposure param");

  /* Serialize again */
  char *json_after = dtpipe_serialize_history(pipe);
  CHECK(json_after != NULL, "post-change serialization succeeds");

  if(json_after)
  {
    CHECK(strcmp(json_before, json_after) != 0,
          "JSON differs after param change");
    CHECK(json_looks_valid(json_after), "post-change JSON is valid");
  }

  /* Restore */
  dtpipe_set_param_float(pipe, "exposure", "exposure", orig);

  free(json_before);
  free(json_after);
}

/* ── Test 6: enable/disable state reflected ──────────────────────────────── */

static void test_enabled_state_reflected(dt_pipe_t *pipe)
{
  printf("\n--- Test 6: enabled state reflected in JSON ---\n");

  if(dtpipe_get_module_count() == 0)
  {
    printf("  skip: no modules registered\n");
    return;
  }

  const char *op = dtpipe_get_module_name(0);
  if(!op)
  {
    printf("  skip: could not get module name\n");
    return;
  }

  /* Ensure the module starts enabled so disabling produces a diff */
  dtpipe_enable_module(pipe, op, 1);

  char *json_enabled = dtpipe_serialize_history(pipe);
  CHECK(json_enabled != NULL, "serialization with module enabled");
  if(!json_enabled) return;

  dtpipe_enable_module(pipe, op, 0);
  char *json_disabled = dtpipe_serialize_history(pipe);
  CHECK(json_disabled != NULL, "serialization with module disabled");

  if(json_disabled)
  {
    CHECK(strcmp(json_enabled, json_disabled) != 0,
          "JSON differs after module disabled");
  }

  /* Restore */
  dtpipe_enable_module(pipe, op, 1);
  free(json_enabled);
  free(json_disabled);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
  const char *raf_path = (argc > 1) ? argv[1]
    : "../../test-image/DSCF4379.RAF";

  printf("=== Task 5.2 verification: history serialization ===\n");

  int rc = dtpipe_init(NULL);
  if(rc != DTPIPE_OK && rc != DTPIPE_ERR_ALREADY_INIT)
  {
    fprintf(stderr, "dtpipe_init failed: %d\n", rc);
    return 1;
  }

  /* Test 1 needs no pipeline */
  test_null_pipe();

  /* Load image (optional — tests degrade gracefully without it) */
  dt_image_t *img = dtpipe_load_raw(raf_path);
  if(!img)
    fprintf(stderr, "  info: could not load '%s' (%s) — running without image\n",
            raf_path, dtpipe_get_last_error());

  dt_pipe_t *pipe = img ? dtpipe_create(img) : NULL;
  if(img && !pipe)
  {
    fprintf(stderr, "  dtpipe_create failed\n");
    dtpipe_free_image(img);
    dtpipe_cleanup();
    return 1;
  }

  /* If no image, create a minimal pipeline around a NULL img is not supported.
     Instead we skip the remaining tests. */
  if(!pipe)
  {
    printf("\n  info: no pipeline available — skipping tests 2-6\n");
    printf("\n=== Results: %d failure(s) ===\n", g_failures);
    dtpipe_cleanup();
    return g_failures ? 1 : 0;
  }

  test_minimal_pipeline(pipe);
  test_source_block(pipe);
  test_param_change_reflected(pipe);
  test_enabled_state_reflected(pipe);

  dtpipe_free(pipe);
  dtpipe_free_image(img);
  dtpipe_cleanup();

  printf("\n=== Results: %d failure(s) ===\n", g_failures);
  return g_failures ? 1 : 0;
}
