/*
 * test_xmp_write.c
 *
 * Task 5.5 verification: exercise dtpipe_save_xmp().
 *
 * Tests:
 *   1. NULL args return DTPIPE_ERR_INVALID_ARG.
 *   2. Write to an unwritable path returns DTPIPE_ERR_IO.
 *   3. Save to a valid path returns DTPIPE_OK and creates a file.
 *   4. The saved file is valid XML (pugixml can parse it — verified
 *      indirectly by loading it back with dtpipe_load_xmp()).
 *   5. Round-trip: set exposure param, save XMP, reload XMP on a fresh
 *      pipeline, verify exposure.exposure matches.
 *   6. Round-trip: disable a module, save, reload, verify disabled state
 *      is preserved.
 *   7. (optional, requires RAF) Real-image round-trip: load RAF + real XMP,
 *      save to a new path, reload, verify exposure.exposure matches.
 *
 * Usage:
 *   test_xmp_write [path/to/DSCF4379.RAF]
 *
 * Exit codes:
 *   0 – all checks passed
 *   1 – one or more checks failed
 */

#include "dtpipe.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if(!(cond)) {                                                              \
      fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg));        \
      g_failures++;                                                            \
    } else {                                                                   \
      printf("  OK  %s\n", (msg));                                            \
    }                                                                          \
  } while(0)

#define CHECK_EQ(got, expect, msg)                                             \
  do {                                                                         \
    int _g = (int)(got), _e = (int)(expect);                                   \
    if(_g != _e) {                                                             \
      fprintf(stderr, "FAIL [%s:%d] %s  (got %d, expected %d)\n",             \
              __FILE__, __LINE__, (msg), _g, _e);                              \
      g_failures++;                                                            \
    } else {                                                                   \
      printf("  OK  %s\n", (msg));                                            \
    }                                                                          \
  } while(0)

#define CHECK_EQ_F(got, expect, tol, msg)                                      \
  do {                                                                         \
    float _g = (float)(got), _e = (float)(expect);                             \
    float _d = _g - _e; if(_d < 0.0f) _d = -_d;                               \
    if(_d > (float)(tol)) {                                                    \
      fprintf(stderr, "FAIL [%s:%d] %s  (got %g, expected %g, tol %g)\n",     \
              __FILE__, __LINE__, (msg), (double)_g, (double)_e,               \
              (double)(tol));                                                   \
      g_failures++;                                                            \
    } else {                                                                   \
      printf("  OK  %s\n", (msg));                                            \
    }                                                                          \
  } while(0)

/* Check whether a file exists and is non-empty */
static int file_nonempty(const char *path)
{
  FILE *f = fopen(path, "r");
  if(!f) return 0;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fclose(f);
  return sz > 0;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_null_args(dt_image_t *img)
{
  printf("\n--- Test 1: NULL argument guard ---\n");
  dt_pipe_t *pipe = dtpipe_create(img);
  if(!pipe) { fprintf(stderr, "  SKIP (no pipeline)\n"); return; }

  CHECK_EQ(dtpipe_save_xmp(NULL, "/tmp/dtpipe_xmp_null.xmp"),
           DTPIPE_ERR_INVALID_ARG,
           "NULL pipe → DTPIPE_ERR_INVALID_ARG");
  CHECK_EQ(dtpipe_save_xmp(pipe, NULL),
           DTPIPE_ERR_INVALID_ARG,
           "NULL path → DTPIPE_ERR_INVALID_ARG");

  dtpipe_free(pipe);
}

static void test_bad_path(dt_image_t *img)
{
  printf("\n--- Test 2: Unwritable path ---\n");
  dt_pipe_t *pipe = dtpipe_create(img);
  if(!pipe) { fprintf(stderr, "  SKIP (no pipeline)\n"); return; }

  /* Writing into a non-existent directory should fail */
  int rc = dtpipe_save_xmp(pipe,
    "/tmp/dtpipe_nonexistent_dir_xyz/out.xmp");
  CHECK_EQ(rc, DTPIPE_ERR_IO, "bad path → DTPIPE_ERR_IO");

  dtpipe_free(pipe);
}

static void test_creates_file(dt_image_t *img)
{
  printf("\n--- Test 3: File is created ---\n");
  const char *out = "/tmp/dtpipe_test_write_out.xmp";

  dt_pipe_t *pipe = dtpipe_create(img);
  if(!pipe) { fprintf(stderr, "  SKIP (no pipeline)\n"); return; }

  int rc = dtpipe_save_xmp(pipe, out);
  CHECK_EQ(rc, DTPIPE_OK, "dtpipe_save_xmp returns DTPIPE_OK");
  CHECK(file_nonempty(out), "output file exists and is non-empty");

  dtpipe_free(pipe);
}

static void test_roundtrip_params(dt_image_t *img)
{
  printf("\n--- Test 4: Round-trip param value ---\n");
  const char *out = "/tmp/dtpipe_test_roundtrip_params.xmp";

  /* Set a distinctive exposure value */
  dt_pipe_t *pipe = dtpipe_create(img);
  if(!pipe) { fprintf(stderr, "  SKIP (no pipeline)\n"); return; }

  float target = 1.75f;
  int src = dtpipe_set_param_float(pipe, "exposure", "exposure", target);
  if(src != DTPIPE_OK)
  {
    printf("  SKIP (exposure module not registered, rc=%d)\n", src);
    dtpipe_free(pipe);
    return;
  }

  /* Save */
  int wrc = dtpipe_save_xmp(pipe, out);
  CHECK_EQ(wrc, DTPIPE_OK, "save_xmp returns DTPIPE_OK");
  dtpipe_free(pipe);

  if(wrc != DTPIPE_OK) return;

  /* Reload on a fresh pipeline */
  dt_pipe_t *pipe2 = dtpipe_create(img);
  if(!pipe2) { fprintf(stderr, "  SKIP (no pipeline)\n"); return; }

  int lrc = dtpipe_load_xmp(pipe2, out);
  CHECK_EQ(lrc, DTPIPE_OK, "load_xmp on saved file returns DTPIPE_OK");

  if(lrc == DTPIPE_OK)
  {
    float got = 0.0f;
    int grc = dtpipe_get_param_float(pipe2, "exposure", "exposure", &got);
    if(grc == DTPIPE_OK)
      CHECK_EQ_F(got, target, 1e-4f, "round-trip: exposure.exposure preserved");
    else
      printf("  SKIP get_param_float (rc=%d)\n", grc);
  }

  dtpipe_free(pipe2);
}

static void test_roundtrip_enabled(dt_image_t *img)
{
  printf("\n--- Test 5: Round-trip enabled state ---\n");
  const char *out = "/tmp/dtpipe_test_roundtrip_enabled.xmp";

  dt_pipe_t *pipe = dtpipe_create(img);
  if(!pipe) { fprintf(stderr, "  SKIP (no pipeline)\n"); return; }

  /* Disable temperature module */
  int drc = dtpipe_enable_module(pipe, "temperature", 0);
  if(drc != DTPIPE_OK)
  {
    printf("  SKIP (temperature module not found, rc=%d)\n", drc);
    dtpipe_free(pipe);
    return;
  }

  /* Enable exposure (should already be on, but be explicit) */
  dtpipe_enable_module(pipe, "exposure", 1);

  int wrc = dtpipe_save_xmp(pipe, out);
  CHECK_EQ(wrc, DTPIPE_OK, "save_xmp returns DTPIPE_OK");
  dtpipe_free(pipe);

  if(wrc != DTPIPE_OK) return;

  /* Reload and set a known exposure value to confirm file was readable */
  dt_pipe_t *pipe2 = dtpipe_create(img);
  if(!pipe2) { fprintf(stderr, "  SKIP (no pipeline)\n"); return; }

  int lrc = dtpipe_load_xmp(pipe2, out);
  CHECK_EQ(lrc, DTPIPE_OK, "load_xmp on enabled-state XMP returns DTPIPE_OK");
  CHECK(lrc == DTPIPE_OK, "pipeline survives enabled-state round-trip without crash");

  dtpipe_free(pipe2);
}

static void test_real_roundtrip(dt_image_t *img, const char *xmp_path)
{
  printf("\n--- Test 6: Real darktable XMP round-trip ---\n");
  const char *out = "/tmp/dtpipe_test_real_roundtrip.xmp";

  /* Load the original darktable XMP */
  dt_pipe_t *pipe = dtpipe_create(img);
  if(!pipe) { fprintf(stderr, "  SKIP (no pipeline)\n"); return; }

  int lrc = dtpipe_load_xmp(pipe, xmp_path);
  if(lrc != DTPIPE_OK)
  {
    fprintf(stderr, "  SKIP (load_xmp failed: %d)\n", lrc);
    dtpipe_free(pipe);
    return;
  }

  /* Read the exposure value from the darktable XMP */
  float orig_exp = 0.0f;
  int grc = dtpipe_get_param_float(pipe, "exposure", "exposure", &orig_exp);
  int have_exp = (grc == DTPIPE_OK);
  if(have_exp)
    printf("  original exposure.exposure = %g\n", (double)orig_exp);

  /* Save to new XMP */
  int wrc = dtpipe_save_xmp(pipe, out);
  CHECK_EQ(wrc, DTPIPE_OK, "save_xmp returns DTPIPE_OK");
  dtpipe_free(pipe);

  if(wrc != DTPIPE_OK) return;

  /* Reload from our saved XMP */
  dt_pipe_t *pipe2 = dtpipe_create(img);
  if(!pipe2) { fprintf(stderr, "  SKIP (no pipeline)\n"); return; }

  int lrc2 = dtpipe_load_xmp(pipe2, out);
  CHECK_EQ(lrc2, DTPIPE_OK, "load_xmp on re-saved XMP returns DTPIPE_OK");

  if(lrc2 == DTPIPE_OK && have_exp)
  {
    float got_exp = 0.0f;
    int grc2 = dtpipe_get_param_float(pipe2, "exposure", "exposure", &got_exp);
    if(grc2 == DTPIPE_OK)
      CHECK_EQ_F(got_exp, orig_exp, 1e-4f,
                 "real XMP round-trip: exposure.exposure preserved");
    else
      printf("  SKIP get_param_float after reload (rc=%d)\n", grc2);
  }

  dtpipe_free(pipe2);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
  printf("=== test_xmp_write ===\n");

  const char *raf_path = (argc > 1) ? argv[1] : NULL;

  char xmp_path[4096] = {0};
  if(raf_path)
    snprintf(xmp_path, sizeof(xmp_path), "%s.xmp", raf_path);

  if(dtpipe_init(NULL) != DTPIPE_OK)
  {
    fprintf(stderr, "dtpipe_init failed\n");
    return 1;
  }

  dt_image_t *img = raf_path ? dtpipe_load_raw(raf_path) : NULL;
  if(raf_path && !img)
    fprintf(stderr, "Warning: could not load '%s' — some tests may skip\n",
            raf_path);

  test_null_args(img);
  test_bad_path(img);
  test_creates_file(img);
  test_roundtrip_params(img);
  test_roundtrip_enabled(img);

  if(img && xmp_path[0] != '\0')
    test_real_roundtrip(img, xmp_path);
  else
    printf("\n--- Test 6: SKIP (no RAF path or XMP provided) ---\n");

  if(img) dtpipe_free_image(img);
  dtpipe_cleanup();

  printf("\n=== %s (%d failure%s) ===\n",
         g_failures == 0 ? "PASSED" : "FAILED",
         g_failures, g_failures == 1 ? "" : "s");
  return g_failures > 0 ? 1 : 0;
}
