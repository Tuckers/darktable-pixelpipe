/*
 * test_pipeline_process.c
 *
 * Task 3.5 verification: exercise dt_dev_pixelpipe_process() through a
 * minimal (zero-module) pipeline fed with a synthetic float-RGBA input.
 *
 * A separate test (test_raf_load, Phase 4) will integrate rawspeed decoding.
 * Here we synthesise a small input buffer so the test has no file I/O
 * dependency and runs fast in CI.  A separate "smoke" test at the bottom
 * opens the real RAF to confirm rawspeed can at least open it.
 *
 * Exit codes:
 *   0 – all checks passed
 *   1 – a check failed
 */

#include "dtpipe_internal.h"
#include "pipe/pixelpipe.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

#ifndef MAX
#  define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    if(!(cond)) {                                                           \
      fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg));     \
      g_failures++;                                                         \
    } else {                                                                \
      printf("  OK  %s\n", (msg));                                         \
    }                                                                       \
  } while(0)

/* ── synthetic input ─────────────────────────────────────────────────────── */

/*
 * Build a W×H float-RGBA (4 floats/pixel) ramp image.
 * Red   = x / (W-1)
 * Green = y / (H-1)
 * Blue  = 0.5
 * Alpha = 1.0
 */
static float *make_ramp(int W, int H)
{
  float *buf = dt_alloc_align_float((size_t)W * H * 4);
  if(!buf) return NULL;
  for(int y = 0; y < H; y++)
  {
    for(int x = 0; x < W; x++)
    {
      float *p = buf + (y * W + x) * 4;
      p[0] = (W > 1) ? (float)x / (float)(W - 1) : 0.5f;
      p[1] = (H > 1) ? (float)y / (float)(H - 1) : 0.5f;
      p[2] = 0.5f;
      p[3] = 1.0f;
    }
  }
  return buf;
}

/* ── Test 1: empty pipeline pass-through (full-res 1:1) ─────────────────── */

static void test_empty_pipeline_full_res(void)
{
  printf("\n--- Test 1: empty pipeline, full-res 1:1 pass-through ---\n");

  const int W = 64, H = 48;

  float *input = make_ramp(W, H);
  CHECK(input != NULL, "allocate ramp input");
  if(!input) return;

  dt_image_t img = { 0 };
  img.width  = W;
  img.height = H;

  dt_dev_pixelpipe_t pipe = { 0 };
  CHECK(dt_dev_pixelpipe_init(&pipe), "dt_dev_pixelpipe_init");

  dt_dev_pixelpipe_set_input(&pipe, input, W, H, 1.0f, &img);

  /* No modules: the pipeline just imports the raw input. */

  bool err = dt_dev_pixelpipe_process(&pipe, 0, 0, W, H, 1.0f);
  CHECK(!err, "dt_dev_pixelpipe_process returned no error");

  CHECK(pipe.backbuf != NULL,         "backbuf is non-NULL");
  CHECK(pipe.backbuf_width  == W,     "backbuf width matches");
  CHECK(pipe.backbuf_height == H,     "backbuf height matches");

  /* With no modules the output is the pass-through of the input.
     The backbuf is a copy, so verify a pixel in the centre. */
  if(pipe.backbuf)
  {
    const float *out = (const float *)pipe.backbuf;
    const int cx = W / 2, cy = H / 2;
    const float *op = out + (cy * W + cx) * 4;
    const float *ip = input + (cy * W + cx) * 4;

    const float tol = 1e-5f;
    CHECK(fabsf(op[0] - ip[0]) < tol, "centre pixel R matches input");
    CHECK(fabsf(op[1] - ip[1]) < tol, "centre pixel G matches input");
    CHECK(fabsf(op[2] - ip[2]) < tol, "centre pixel B matches input");
  }

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_free_align(input);
}

/* ── Test 2: downscaled output (scale < 1) ──────────────────────────────── */

static void test_empty_pipeline_scaled(void)
{
  printf("\n--- Test 2: empty pipeline, 0.5× downscale ---\n");

  const int W = 64, H = 48;
  const float scale = 0.5f;
  const int OW = (int)(W * scale);
  const int OH = (int)(H * scale);

  float *input = make_ramp(W, H);
  CHECK(input != NULL, "allocate ramp input");
  if(!input) return;

  dt_image_t img = { 0 };
  img.width  = W;
  img.height = H;

  dt_dev_pixelpipe_t pipe = { 0 };
  CHECK(dt_dev_pixelpipe_init(&pipe), "dt_dev_pixelpipe_init");
  dt_dev_pixelpipe_set_input(&pipe, input, W, H, 1.0f, &img);

  bool err = dt_dev_pixelpipe_process(&pipe, 0, 0, OW, OH, scale);
  CHECK(!err, "dt_dev_pixelpipe_process (scaled) returned no error");
  CHECK(pipe.backbuf != NULL,          "backbuf is non-NULL");
  CHECK(pipe.backbuf_width  == OW,     "backbuf width = OW");
  CHECK(pipe.backbuf_height == OH,     "backbuf height = OH");

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_free_align(input);
}

/* ── Test 3: get_dimensions with no modules ──────────────────────────────── */

static void test_get_dimensions(void)
{
  printf("\n--- Test 3: get_dimensions, no modules ---\n");

  const int W = 100, H = 75;

  dt_image_t img = { 0 };
  img.width  = W;
  img.height = H;

  dt_dev_pixelpipe_t pipe = { 0 };
  CHECK(dt_dev_pixelpipe_init(&pipe), "dt_dev_pixelpipe_init");

  /* Allocate a dummy input so set_input doesn't crash */
  float *dummy = dt_alloc_align_float((size_t)W * H * 4);
  dt_dev_pixelpipe_set_input(&pipe, dummy, W, H, 1.0f, &img);

  int ow = 0, oh = 0;
  dt_dev_pixelpipe_get_dimensions(&pipe, W, H, &ow, &oh);

  /* With no modules output == input */
  CHECK(ow == W, "get_dimensions: output width = input width");
  CHECK(oh == H, "get_dimensions: output height = input height");

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_free_align(dummy);
}

/* ── Test 4: shutdown flag API works correctly ───────────────────────────── */
/*
 * dt_dev_pixelpipe_process() resets the shutdown flag at entry (the caller
 * signals shutdown from another thread *during* processing).  Verify:
 *   a) the flag starts clear after a successful process() call
 *   b) dt_pipe_shutdown() correctly reads the atomic flag
 */
static void test_shutdown(void)
{
  printf("\n--- Test 4: shutdown flag API ---\n");

  const int W = 32, H = 32;
  float *input = make_ramp(W, H);
  CHECK(input != NULL, "allocate input");
  if(!input) return;

  dt_image_t img = { .width = W, .height = H };
  dt_dev_pixelpipe_t pipe = { 0 };
  dt_dev_pixelpipe_init(&pipe);
  dt_dev_pixelpipe_set_input(&pipe, input, W, H, 1.0f, &img);

  /* Before processing, set the flag then confirm dt_pipe_shutdown detects it */
  dt_atomic_set_int(&pipe.shutdown, DT_DEV_PIXELPIPE_STOP_NODES);
  CHECK(dt_pipe_shutdown(&pipe), "dt_pipe_shutdown() returns true when flag is set");

  /* process() resets the flag on entry — confirm it succeeds cleanly */
  bool err = dt_dev_pixelpipe_process(&pipe, 0, 0, W, H, 1.0f);
  CHECK(!err, "process() succeeds (resets shutdown flag on entry)");
  CHECK(!dt_pipe_shutdown(&pipe), "shutdown flag is clear after successful process()");

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_free_align(input);
}

/* ── Test 5: RAF file is accessible ──────────────────────────────────────── */

static void test_raf_accessible(const char *raf_path)
{
  printf("\n--- Test 5: RAF file is accessible ---\n");

  FILE *f = fopen(raf_path, "rb");
  CHECK(f != NULL, "open RAF file for reading");
  if(f)
  {
    /* Read first 4 bytes and check for Fuji RAF magic: "FUJI" */
    unsigned char magic[4] = { 0 };
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    CHECK(n == 4, "read 4 bytes from RAF");
    int is_fuji = (magic[0]=='F' && magic[1]=='U' && magic[2]=='J' && magic[3]=='I');
    CHECK(is_fuji, "RAF magic bytes are 'FUJI'");
  }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
  const char *raf_path = (argc > 1) ? argv[1]
    : "../../test-image/DSCF4379.RAF";

  printf("=== Task 3.5 verification: dt_dev_pixelpipe_process ===\n");

  /* The pipeline uses darktable global state for debug flags only */
  memset(&darktable, 0, sizeof(darktable));

  test_empty_pipeline_full_res();
  test_empty_pipeline_scaled();
  test_get_dimensions();
  test_shutdown();
  test_raf_accessible(raf_path);

  printf("\n=== Results: %d failure(s) ===\n", g_failures);
  return g_failures ? 1 : 0;
}
