/*
 * test_regression.c
 *
 * Task 7.3: Image comparison regression tests.
 *
 * For each of the three reference presets (preset_a, preset_b, preset_c):
 *   1. Load the test RAW image.
 *   2. Apply the preset history from tests/reference/<name>.json.
 *   3. Render at scale 0.25 (matching gen_reference).
 *   4. Load the reference PNG from tests/reference/<name>.png.
 *   5. Compare pixel-by-pixel (8-bit RGBA after clamping the render result).
 *      - FAIL if max per-channel absolute difference > MAX_PIXEL_DIFF (1).
 *      - WARN if mean per-channel absolute difference > MEAN_PIXEL_DIFF_WARN (0.5).
 *   6. On failure, write a diff image to /tmp/dtpipe_diff_<name>.png for inspection.
 *
 * Usage:
 *   test_regression [path/to/image.RAF [reference_dir]]
 *
 *   reference_dir defaults to tests/reference relative to CWD
 *   (i.e. the build directory).
 *
 * Exit codes:
 *   0 – all presets matched
 *   1 – one or more presets failed or a fatal error occurred
 */

#include "dtpipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <errno.h>

#include <png.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Config
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RENDER_SCALE          0.25f
#define MAX_PIXEL_DIFF        1       /* max allowed per-channel absolute diff (0-255) */
#define MEAN_PIXEL_DIFF_WARN  0.5f    /* warn threshold for mean per-channel diff */

/* ═══════════════════════════════════════════════════════════════════════════
 * Minimal test framework
 * ═══════════════════════════════════════════════════════════════════════════ */

static int g_pass = 0;
static int g_fail = 0;
static int g_warn = 0;
static int g_skip = 0;

#define CHECK(cond, msg)                                                        \
  do {                                                                          \
    if(!(cond)) {                                                               \
      fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg));       \
      g_fail++;                                                                 \
    } else {                                                                    \
      printf("  OK   %s\n", (msg));                                            \
      g_pass++;                                                                 \
    }                                                                           \
  } while(0)

#define WARN(msg)                                                               \
  do {                                                                          \
    fprintf(stderr, "  WARN %s\n", (msg));                                      \
    g_warn++;                                                                   \
  } while(0)

#define SKIP(msg)                                                               \
  do {                                                                          \
    printf("  SKIP %s\n", (msg));                                              \
    g_skip++;                                                                   \
  } while(0)

/* ═══════════════════════════════════════════════════════════════════════════
 * PNG I/O helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  unsigned char *pixels; /* RGBA, row-major */
  int            width;
  int            height;
} rgba_image_t;

static void free_rgba_image(rgba_image_t *img)
{
  if(!img) return;
  free(img->pixels);
  img->pixels = NULL;
}

/* Load an 8-bit RGBA PNG. Returns 0 on success, -1 on failure. */
static int load_png(const char *path, rgba_image_t *out)
{
  FILE *fp = fopen(path, "rb");
  if(!fp)
  {
    fprintf(stderr, "    load_png: cannot open '%s': %s\n", path, strerror(errno));
    return -1;
  }

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png) { fclose(fp); return -1; }

  png_infop info = png_create_info_struct(png);
  if(!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return -1; }

  if(setjmp(png_jmpbuf(png)))
  {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return -1;
  }

  png_init_io(png, fp);
  png_read_info(png, info);

  int width      = (int)png_get_image_width(png, info);
  int height     = (int)png_get_image_height(png, info);
  int color_type = png_get_color_type(png, info);
  int bit_depth  = png_get_bit_depth(png, info);

  /* Normalize to 8-bit RGBA */
  if(bit_depth == 16)
    png_set_strip_16(png);
  if(color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if(png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if(color_type == PNG_COLOR_TYPE_RGB ||
     color_type == PNG_COLOR_TYPE_GRAY ||
     color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  if(color_type == PNG_COLOR_TYPE_GRAY ||
     color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);

  png_read_update_info(png, info);

  size_t stride  = (size_t)png_get_rowbytes(png, info);
  unsigned char *pixels = (unsigned char *)malloc(stride * (size_t)height);
  if(!pixels)
  {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return -1;
  }

  png_bytep *rows = (png_bytep *)malloc(sizeof(png_bytep) * (size_t)height);
  if(!rows)
  {
    free(pixels);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return -1;
  }

  for(int y = 0; y < height; y++)
    rows[y] = pixels + (size_t)y * stride;

  png_read_image(png, rows);
  free(rows);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);

  out->pixels = pixels;
  out->width  = width;
  out->height = height;
  return 0;
}

/* Write an 8-bit RGBA PNG. Returns 0 on success, -1 on failure. */
static int write_png(const char *path, const unsigned char *pixels, int width, int height)
{
  FILE *fp = fopen(path, "wb");
  if(!fp)
  {
    fprintf(stderr, "    write_png: cannot open '%s': %s\n", path, strerror(errno));
    return -1;
  }

  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png) { fclose(fp); return -1; }

  png_infop info = png_create_info_struct(png);
  if(!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }

  if(setjmp(png_jmpbuf(png)))
  {
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return -1;
  }

  png_init_io(png, fp);
  png_set_IHDR(png, info, (png_uint_32)width, (png_uint_32)height,
               8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);

  for(int y = 0; y < height; y++)
    png_write_row(png, (png_bytep)(pixels + (size_t)y * (size_t)width * 4));

  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  fclose(fp);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Image comparison
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  double mean_diff;   /* mean per-channel absolute difference (0-255 scale) */
  int    max_diff;    /* max per-channel absolute difference */
  long   diff_pixels; /* number of pixels with any difference */
} compare_stats_t;

/*
 * compare_images – compare two 8-bit RGBA images of the same dimensions.
 *
 * diff_out: if non-NULL, filled with a 3×amplified diff image (R/G/B channels
 *           show per-channel diff * 3, alpha = 255) for debug purposes.
 *
 * Returns 0 if images are within threshold, -1 if they differ beyond it.
 */
static int compare_images(const unsigned char *a, const unsigned char *b,
                           int width, int height,
                           compare_stats_t *stats,
                           unsigned char **diff_out)
{
  long   total_channels = (long)width * height * 3; /* only RGB, ignore alpha */
  double sum_diff       = 0.0;
  int    max_diff       = 0;
  long   diff_pixels    = 0;

  unsigned char *diff = NULL;
  if(diff_out)
  {
    diff = (unsigned char *)calloc((size_t)width * (size_t)height * 4, 1);
    *diff_out = diff;
  }

  for(int i = 0; i < width * height; i++)
  {
    int pixel_diff = 0;
    for(int c = 0; c < 3; c++) /* RGB only */
    {
      int d = (int)a[i * 4 + c] - (int)b[i * 4 + c];
      if(d < 0) d = -d;
      sum_diff += d;
      if(d > max_diff) max_diff = d;
      if(d > pixel_diff) pixel_diff = d;

      if(diff)
      {
        int amplified = d * 3;
        if(amplified > 255) amplified = 255;
        diff[i * 4 + c] = (unsigned char)amplified;
      }
    }
    if(pixel_diff > 0) diff_pixels++;
    if(diff) diff[i * 4 + 3] = 255;
  }

  stats->mean_diff   = (total_channels > 0) ? (sum_diff / total_channels) : 0.0;
  stats->max_diff    = max_diff;
  stats->diff_pixels = diff_pixels;

  return (max_diff > MAX_PIXEL_DIFF) ? -1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * File utilities
 * ═══════════════════════════════════════════════════════════════════════════ */

static int read_file(const char *path, char **out_buf)
{
  FILE *fp = fopen(path, "rb");
  if(!fp) return -1;
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  rewind(fp);
  if(sz <= 0) { fclose(fp); return -1; }

  char *buf = (char *)malloc((size_t)sz + 1);
  if(!buf) { fclose(fp); return -1; }
  if(fread(buf, 1, (size_t)sz, fp) != (size_t)sz)
  {
    free(buf); fclose(fp); return -1;
  }
  buf[sz] = '\0';
  fclose(fp);
  *out_buf = buf;
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Preset definition (mirrors gen_reference.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  const char *name;
  const char *description;
  float       exposure;
  int         sharpen_on;
} preset_t;

static const preset_t PRESETS[] = {
  { "preset_a", "exposure +1.0, sharpen enabled",   1.0f,  1 },
  { "preset_b", "exposure -0.5, sharpen enabled",  -0.5f,  1 },
  { "preset_c", "exposure  0.0, sharpen disabled",  0.0f,  0 },
};
static const int N_PRESETS = (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));

/* ═══════════════════════════════════════════════════════════════════════════
 * Per-preset regression test
 * ═══════════════════════════════════════════════════════════════════════════ */

static int test_preset(dt_image_t *img, const preset_t *p, const char *ref_dir)
{
  char json_path[1024], png_path[1024], diff_path[1024];
  snprintf(json_path, sizeof(json_path), "%s/%s.json", ref_dir, p->name);
  snprintf(png_path,  sizeof(png_path),  "%s/%s.png",  ref_dir, p->name);
  snprintf(diff_path, sizeof(diff_path), "/tmp/dtpipe_diff_%s.png", p->name);

  printf("\n── Preset: %s ('%s') ──\n", p->name, p->description);

  /* ── Check reference files exist ── */
  {
    struct stat st;
    if(stat(json_path, &st) != 0)
    {
      fprintf(stderr, "  SKIP: reference JSON not found: %s\n", json_path);
      g_skip++;
      return 0;
    }
    if(stat(png_path, &st) != 0)
    {
      fprintf(stderr, "  SKIP: reference PNG not found: %s\n", png_path);
      g_skip++;
      return 0;
    }
  }

  /* ── Create pipeline ── */
  dt_pipe_t *pipe = dtpipe_create(img);
  if(!pipe)
  {
    fprintf(stderr, "  FAIL: dtpipe_create: %s\n", dtpipe_get_last_error());
    g_fail++;
    return -1;
  }

  /* ── Load preset history from reference JSON ── */
  char *json_buf = NULL;
  if(read_file(json_path, &json_buf) != 0)
  {
    fprintf(stderr, "  FAIL: cannot read '%s'\n", json_path);
    g_fail++;
    dtpipe_free(pipe);
    return -1;
  }

  int rc = dtpipe_load_history(pipe, json_buf);
  free(json_buf);

  CHECK(rc == DTPIPE_OK, "load_history from reference JSON");
  if(rc != DTPIPE_OK)
  {
    fprintf(stderr, "    dtpipe_load_history rc=%d: %s\n", rc, dtpipe_get_last_error());
    dtpipe_free(pipe);
    return -1;
  }

  /* ── Verify loaded params match the preset ── */
  {
    float exp_val = 0.0f;
    int param_rc = dtpipe_get_param_float(pipe, "exposure", "exposure", &exp_val);
    if(param_rc == DTPIPE_OK)
    {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "exposure.exposure = %.2f (expected %.2f)", exp_val, p->exposure);
      int close = (fabsf(exp_val - p->exposure) < 0.001f);
      CHECK(close, msg);
    }
    else
    {
      SKIP("exposure param not available (stub modules)");
    }

    int sharpen_enabled = -1;
    int en_rc = dtpipe_is_module_enabled(pipe, "sharpen", &sharpen_enabled);
    if(en_rc == DTPIPE_OK)
    {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "sharpen enabled = %d (expected %d)", sharpen_enabled, p->sharpen_on);
      CHECK(sharpen_enabled == p->sharpen_on, msg);
    }
    else
    {
      SKIP("sharpen module not found (stub modules)");
    }
  }

  /* ── Export to temp PNG (matches what gen_reference does) ── */
  char tmp_path[1024];
  snprintf(tmp_path, sizeof(tmp_path), "/tmp/dtpipe_test_%s.png", p->name);
  printf("  exporting PNG to %s ...\n", tmp_path);

  int export_rc = dtpipe_export_png(pipe, tmp_path);
  CHECK(export_rc == DTPIPE_OK, "export_png to temp file");
  if(export_rc != DTPIPE_OK)
  {
    fprintf(stderr, "    dtpipe_export_png rc=%d: %s\n",
            export_rc, dtpipe_get_last_error());
    dtpipe_free(pipe);
    return -1;
  }

  dtpipe_free(pipe);
  pipe = NULL;

  /* ── Load exported PNG ── */
  rgba_image_t exported = { NULL, 0, 0 };
  int exp_load_rc = load_png(tmp_path, &exported);
  CHECK(exp_load_rc == 0, "load exported PNG");
  if(exp_load_rc != 0)
    return -1;

  printf("  exported: %d x %d\n", exported.width, exported.height);

  /* ── Load reference PNG ── */
  rgba_image_t ref = { NULL, 0, 0 };
  int load_rc = load_png(png_path, &ref);
  CHECK(load_rc == 0, "load reference PNG");
  if(load_rc != 0)
  {
    free_rgba_image(&exported);
    return -1;
  }

  /* ── Dimension check ── */
  {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "export width matches reference (%d vs %d)",
             exported.width, ref.width);
    CHECK(exported.width == ref.width, msg);
    snprintf(msg, sizeof(msg),
             "export height matches reference (%d vs %d)",
             exported.height, ref.height);
    CHECK(exported.height == ref.height, msg);
  }

  if(exported.width != ref.width || exported.height != ref.height)
  {
    fprintf(stderr, "  FAIL: dimension mismatch; skipping pixel comparison\n");
    free_rgba_image(&ref);
    free_rgba_image(&exported);
    return -1;
  }

  /* ── Pixel comparison ── */
  compare_stats_t stats;
  unsigned char *diff_pixels = NULL;
  int cmp_rc = compare_images(exported.pixels, ref.pixels,
                               exported.width, exported.height,
                               &stats, &diff_pixels);

  printf("  pixel stats: mean_diff=%.4f  max_diff=%d  diff_pixels=%ld / %d\n",
         stats.mean_diff, stats.max_diff, stats.diff_pixels,
         exported.width * exported.height);

  {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "max pixel diff %d <= %d", stats.max_diff, MAX_PIXEL_DIFF);
    CHECK(cmp_rc == 0, msg);
  }

  if(stats.mean_diff > MEAN_PIXEL_DIFF_WARN)
  {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "mean pixel diff %.4f > %.1f (warning threshold)",
             stats.mean_diff, (double)MEAN_PIXEL_DIFF_WARN);
    WARN(msg);
  }

  /* ── Write diff image on failure ── */
  if(cmp_rc != 0 && diff_pixels)
  {
    if(write_png(diff_path, diff_pixels, exported.width, exported.height) == 0)
      printf("  diff image saved to: %s\n", diff_path);
  }

  free(diff_pixels);
  free_rgba_image(&ref);
  free_rgba_image(&exported);

  return cmp_rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
  const char *raf_path = (argc > 1) ? argv[1] : "../../test-image/DSCF4379.RAF";
  const char *ref_dir  = (argc > 2) ? argv[2] : "tests/reference";

  printf("╔══════════════════════════════════════════════════╗\n");
  printf("║  libdtpipe regression tests (Task 7.3)           ║\n");
  printf("╚══════════════════════════════════════════════════╝\n");
  printf("RAW input    : %s\n", raf_path);
  printf("Reference dir: %s\n", ref_dir);
  printf("Render scale : %.2f\n", RENDER_SCALE);
  printf("Max diff     : %d\n", MAX_PIXEL_DIFF);
  printf("\n");

  /* ── Init ── */
  int rc = dtpipe_init(NULL);
  if(rc != DTPIPE_OK && rc != DTPIPE_ERR_ALREADY_INIT)
  {
    fprintf(stderr, "FATAL: dtpipe_init failed: rc=%d\n", rc);
    return 1;
  }
  printf("OK   dtpipe_init\n");

  /* ── Load image ── */
  dt_image_t *img = dtpipe_load_raw(raf_path);
  if(!img)
  {
    fprintf(stderr, "FATAL: dtpipe_load_raw('%s'): %s\n",
            raf_path, dtpipe_get_last_error());
    dtpipe_cleanup();
    return 1;
  }
  printf("OK   dtpipe_load_raw  %d x %d  %s %s\n",
         dtpipe_get_width(img), dtpipe_get_height(img),
         dtpipe_get_camera_maker(img) ? dtpipe_get_camera_maker(img) : "",
         dtpipe_get_camera_model(img) ? dtpipe_get_camera_model(img) : "");

  /* ── Run each preset ── */
  int failures = 0;
  for(int i = 0; i < N_PRESETS; i++)
  {
    if(test_preset(img, &PRESETS[i], ref_dir) != 0)
      failures++;
  }

  /* ── Cleanup ── */
  dtpipe_free_image(img);
  dtpipe_cleanup();

  /* ── Summary ── */
  printf("\n══ Summary ══\n");
  printf("  pass=%d  fail=%d  warn=%d  skip=%d\n",
         g_pass, g_fail, g_warn, g_skip);

  if(failures == 0 && g_fail == 0)
  {
    printf("\nPASSED – all regression tests matched reference renders.\n");
    return 0;
  }
  else
  {
    fprintf(stderr, "\nFAILED – %d failure(s). See diff images in /tmp/ for details.\n",
            g_fail);
    return 1;
  }
}
