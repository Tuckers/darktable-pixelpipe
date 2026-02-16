/*
 * benchmark_performance.c
 *
 * Performance benchmark for libdtpipe. Times each discrete pipeline stage
 * and prints a structured summary with millisecond precision and pixel
 * throughput where applicable.
 *
 * Stages timed:
 *   1. Raw decode           — dtpipe_load_raw()
 *   2. Render 0.25x (cold)  — first render, includes input buffer build
 *   3. Render 0.25x (warm)  — second render, input buffer cached
 *   4. Render 1.0x           — full resolution render
 *   5. Render region 1024²  — dtpipe_render_region() crop
 *   6. Export JPEG           — dtpipe_export_jpeg() quality 90
 *   7. Export PNG            — dtpipe_export_png() 16-bit
 *   8. Export TIFF           — dtpipe_export_tiff() 16-bit
 *
 * Usage:
 *   benchmark_performance [path/to/image.RAF]
 *
 * Always exits 0 (informational). Prints warnings if stages fail.
 */

#include "dtpipe.h"
#include "dtpipe_internal.h" /* dt_get_num_threads() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Timing helper ───────────────────────────────────────────────────────── */

static double now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

/* ── Output helpers ──────────────────────────────────────────────────────── */

static void print_row(const char *label, double ms, double mpx)
{
  if(mpx > 0.0)
    printf("  %-30s %9.1f    %7.1f\n", label, ms, mpx);
  else
    printf("  %-30s %9.1f        -\n", label, ms);
}

static void print_skip(const char *label)
{
  printf("  %-30s   SKIPPED\n", label);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
  const char *path = (argc > 1) ? argv[1]
                                 : "../../test-image/DSCF4379.RAF";

  printf("\n=== libdtpipe Performance Benchmark ===\n\n");

  /* ── Library init ──────────────────────────────────────────────────── */
  int rc = dtpipe_init(NULL);
  if(rc != DTPIPE_OK && rc != DTPIPE_ERR_ALREADY_INIT)
  {
    fprintf(stderr, "dtpipe_init failed: %d\n", rc);
    return 0;
  }

  /* ── 1. Raw decode ─────────────────────────────────────────────────── */
  double t0 = now_ms();
  dt_image_t *img = dtpipe_load_raw(path);
  double t_load = now_ms() - t0;

  if(!img)
  {
    fprintf(stderr, "Failed to load image: %s\n", dtpipe_get_last_error());
    fprintf(stderr, "  (path: %s)\n", path);
    dtpipe_cleanup();
    return 0;
  }

  const int full_w = dtpipe_get_width(img);
  const int full_h = dtpipe_get_height(img);
  const double mpx = (double)full_w * full_h / 1.0e6;

  printf("Image: %s\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
  printf("Size:  %d x %d (%.1f Mpx)\n", full_w, full_h, mpx);

#ifdef _OPENMP
  printf("Threads: %d (OpenMP enabled)\n", dt_get_num_threads());
#else
  printf("Threads: 1 (OpenMP disabled)\n");
#endif

  printf("\n  %-30s %9s    %7s\n", "Stage", "Time (ms)", "Mpx/s");
  printf("  ──────────────────────────────────────────────────────\n");

  /* Print raw decode result */
  print_row("Raw decode (load_raw)", t_load, mpx / (t_load / 1000.0));

  /* ── Create pipeline ───────────────────────────────────────────────── */
  dt_pipe_t *pipe = dtpipe_create(img);
  if(!pipe)
  {
    fprintf(stderr, "dtpipe_create failed\n");
    dtpipe_free_image(img);
    dtpipe_cleanup();
    return 0;
  }

  /* ── 2. Render 0.25x (cold — includes input buffer build) ─────────── */
  t0 = now_ms();
  dt_render_result_t *r = dtpipe_render(pipe, 0.25f);
  double t_render_cold = now_ms() - t0;

  if(r)
  {
    double out_mpx = (double)r->width * r->height / 1.0e6;
    print_row("Render 0.25x (cold)", t_render_cold, mpx / (t_render_cold / 1000.0));
    dtpipe_free_render(r);
  }
  else
  {
    print_skip("Render 0.25x (cold)");
  }

  /* ── 3. Render 0.25x (warm — input buffer cached) ─────────────────── */
  t0 = now_ms();
  r = dtpipe_render(pipe, 0.25f);
  double t_render_warm = now_ms() - t0;

  if(r)
  {
    print_row("Render 0.25x (warm)", t_render_warm, mpx / (t_render_warm / 1000.0));
    dtpipe_free_render(r);
  }
  else
  {
    print_skip("Render 0.25x (warm)");
  }

  /* ── 4. Render 1.0x (full resolution) ──────────────────────────────── */
  t0 = now_ms();
  r = dtpipe_render(pipe, 1.0f);
  double t_render_full = now_ms() - t0;

  if(r)
  {
    print_row("Render 1.0x (full res)", t_render_full, mpx / (t_render_full / 1000.0));
    dtpipe_free_render(r);
  }
  else
  {
    print_skip("Render 1.0x (full res)");
  }

  /* ── 5. Render region 1024x1024 ────────────────────────────────────── */
  const int rw = 1024, rh = 1024;
  const int rx = (full_w - rw) / 2;
  const int ry = (full_h - rh) / 2;
  const double region_mpx = (double)rw * rh / 1.0e6;

  t0 = now_ms();
  r = dtpipe_render_region(pipe, rx, ry, rw, rh, 1.0f);
  double t_region = now_ms() - t0;

  if(r)
  {
    print_row("Render region 1024x1024", t_region, region_mpx / (t_region / 1000.0));
    dtpipe_free_render(r);
  }
  else
  {
    print_skip("Render region 1024x1024");
  }

  /* ── 6. Export JPEG ────────────────────────────────────────────────── */
  const char *jpeg_path = "/tmp/dtpipe_bench_export.jpg";

  t0 = now_ms();
  rc = dtpipe_export_jpeg(pipe, jpeg_path, 90);
  double t_jpeg = now_ms() - t0;

  if(rc == DTPIPE_OK)
  {
    print_row("Export JPEG (q90)", t_jpeg, -1.0);
    unlink(jpeg_path);
  }
  else
  {
    print_skip("Export JPEG (q90)");
  }

  /* ── 7. Export PNG ─────────────────────────────────────────────────── */
  const char *png_path = "/tmp/dtpipe_bench_export.png";

  t0 = now_ms();
  rc = dtpipe_export_png(pipe, png_path);
  double t_png = now_ms() - t0;

  if(rc == DTPIPE_OK)
  {
    print_row("Export PNG (16-bit)", t_png, -1.0);
    unlink(png_path);
  }
  else
  {
    print_skip("Export PNG (16-bit)");
  }

  /* ── 8. Export TIFF ────────────────────────────────────────────────── */
  const char *tiff_path = "/tmp/dtpipe_bench_export.tif";

  t0 = now_ms();
  rc = dtpipe_export_tiff(pipe, tiff_path, 16);
  double t_tiff = now_ms() - t0;

  if(rc == DTPIPE_OK)
  {
    print_row("Export TIFF (16-bit)", t_tiff, -1.0);
    unlink(tiff_path);
  }
  else
  {
    print_skip("Export TIFF (16-bit)");
  }

  printf("  ──────────────────────────────────────────────────────\n\n");

  /* Cleanup */
  dtpipe_free(pipe);
  dtpipe_free_image(img);
  dtpipe_cleanup();

  return 0;
}
