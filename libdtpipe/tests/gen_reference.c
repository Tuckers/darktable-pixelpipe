/*
 * gen_reference.c
 *
 * Task 7.2: Generate reference renders for regression testing.
 *
 * This program loads the test RAW image, applies three standard presets,
 * renders each at scale 0.25, and saves the results to tests/reference/.
 * It also saves the JSON history for each preset so Task 7.3 can replay them.
 *
 * Presets
 * -------
 *   preset_a  – exposure +1.0  (bright/overexposed look)
 *   preset_b  – exposure -0.5  (darker, high-contrast look)
 *   preset_c  – exposure  0.0, sharpen disabled  (neutral baseline)
 *
 * Output files (in <output_dir>/)
 * --------------------------------
 *   preset_a.png   preset_a.json
 *   preset_b.png   preset_b.json
 *   preset_c.png   preset_c.json
 *   metadata.txt   – image dimensions, camera info, pipeline module list
 *
 * Usage
 * -----
 *   gen_reference [path/to/image.RAF [output_dir]]
 *
 *   output_dir defaults to tests/reference/ relative to the binary's working
 *   directory (i.e. the build directory).  The directory is created if it
 *   does not exist.
 *
 * Exit codes
 *   0 – all reference files written successfully
 *   1 – fatal error (image load failure, render failure, etc.)
 */

#include "dtpipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

/* ── helpers ──────────────────────────────────────────────────────────────── */

static int make_dir(const char *path)
{
  struct stat st;
  if(stat(path, &st) == 0)
  {
    if(S_ISDIR(st.st_mode)) return 0; /* already exists */
    fprintf(stderr, "ERROR: '%s' exists but is not a directory\n", path);
    return -1;
  }
  if(mkdir(path, 0755) != 0)
  {
    fprintf(stderr, "ERROR: cannot create '%s': %s\n", path, strerror(errno));
    return -1;
  }
  return 0;
}

/* Build "<dir>/<name>" safely into buf[bufsz]. Returns buf. */
static char *join_path(char *buf, size_t bufsz, const char *dir, const char *name)
{
  snprintf(buf, bufsz, "%s/%s", dir, name);
  return buf;
}

static int write_string_file(const char *path, const char *content)
{
  FILE *fp = fopen(path, "w");
  if(!fp)
  {
    fprintf(stderr, "ERROR: cannot open '%s' for writing: %s\n", path, strerror(errno));
    return -1;
  }
  fputs(content, fp);
  fclose(fp);
  return 0;
}

/* ── preset definition ────────────────────────────────────────────────────── */

typedef struct {
  const char *name;
  const char *description;
  float       exposure;       /* exposure.exposure  (float) */
  int         sharpen_on;     /* 1 = enabled, 0 = disabled */
} preset_t;

static const preset_t PRESETS[] = {
  { "preset_a", "exposure +1.0, sharpen enabled",   1.0f,  1 },
  { "preset_b", "exposure -0.5, sharpen enabled",  -0.5f,  1 },
  { "preset_c", "exposure  0.0, sharpen disabled",  0.0f,  0 },
};
static const int N_PRESETS = (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));

#define RENDER_SCALE 0.25f

/* ── apply a preset to a freshly-created pipeline ─────────────────────────── */

static int apply_preset(dt_pipe_t *pipe, const preset_t *p)
{
  int rc;

  /* Exposure */
  rc = dtpipe_set_param_float(pipe, "exposure", "exposure", p->exposure);
  if(rc != DTPIPE_OK)
  {
    /* exposure module may not be in the pipeline — warn but continue */
    fprintf(stderr, "  warn: set exposure.exposure -> %.2f: rc=%d (%s)\n",
            p->exposure, rc, dtpipe_get_last_error());
  }

  /* Sharpen toggle */
  rc = dtpipe_enable_module(pipe, "sharpen", p->sharpen_on);
  if(rc != DTPIPE_OK)
  {
    fprintf(stderr, "  warn: enable_module(sharpen, %d): rc=%d (%s)\n",
            p->sharpen_on, rc, dtpipe_get_last_error());
  }

  return DTPIPE_OK; /* continue even if individual params are missing */
}

/* ── render one preset and write PNG + JSON ───────────────────────────────── */

static int render_preset(dt_image_t *img, const preset_t *p, const char *out_dir)
{
  char png_path[1024], json_path[1024];
  join_path(png_path,  sizeof(png_path),  out_dir, p->name);
  strncat(png_path,  ".png",  sizeof(png_path)  - strlen(png_path)  - 1);
  join_path(json_path, sizeof(json_path), out_dir, p->name);
  strncat(json_path, ".json", sizeof(json_path) - strlen(json_path) - 1);

  printf("  preset '%s': %s\n", p->name, p->description);

  /* Create a fresh pipeline for each preset so they are independent */
  dt_pipe_t *pipe = dtpipe_create(img);
  if(!pipe)
  {
    fprintf(stderr, "  ERROR: dtpipe_create failed: %s\n", dtpipe_get_last_error());
    return -1;
  }

  /* Apply the preset parameters */
  apply_preset(pipe, p);

  /* Render at RENDER_SCALE */
  printf("    rendering at scale %.2f ...\n", RENDER_SCALE);
  dt_render_result_t *r = dtpipe_render(pipe, RENDER_SCALE);
  if(!r)
  {
    fprintf(stderr, "  ERROR: dtpipe_render failed: %s\n", dtpipe_get_last_error());
    dtpipe_free(pipe);
    return -1;
  }
  printf("    render dimensions: %d x %d\n", r->width, r->height);
  dtpipe_free_render(r);

  /* Export PNG (full pipeline render encoded to PNG file) */
  printf("    exporting PNG -> %s\n", png_path);
  int rc = dtpipe_export_png(pipe, png_path);
  if(rc != DTPIPE_OK)
  {
    fprintf(stderr, "  ERROR: dtpipe_export_png failed (rc=%d): %s\n",
            rc, dtpipe_get_last_error());
    dtpipe_free(pipe);
    return -1;
  }

  /* Verify the PNG was written */
  struct stat st;
  if(stat(png_path, &st) != 0 || st.st_size == 0)
  {
    fprintf(stderr, "  ERROR: PNG file not found or empty: %s\n", png_path);
    dtpipe_free(pipe);
    return -1;
  }
  printf("    PNG: %ld bytes\n", (long)st.st_size);

  /* Serialize history to JSON */
  char *json = dtpipe_serialize_history(pipe);
  if(!json)
  {
    fprintf(stderr, "  ERROR: dtpipe_serialize_history failed: %s\n",
            dtpipe_get_last_error());
    dtpipe_free(pipe);
    return -1;
  }

  printf("    saving JSON -> %s\n", json_path);
  int write_rc = write_string_file(json_path, json);
  free(json);

  dtpipe_free(pipe);

  if(write_rc != 0) return -1;

  printf("    OK\n");
  return 0;
}

/* ── write metadata.txt ───────────────────────────────────────────────────── */

static void write_metadata(dt_image_t *img, const char *out_dir)
{
  char meta_path[1024];
  join_path(meta_path, sizeof(meta_path), out_dir, "metadata.txt");

  FILE *fp = fopen(meta_path, "w");
  if(!fp)
  {
    fprintf(stderr, "  warn: cannot write metadata.txt: %s\n", strerror(errno));
    return;
  }

  fprintf(fp, "# libdtpipe reference render metadata\n");
  fprintf(fp, "# Generated by gen_reference (Task 7.2)\n");
  fprintf(fp, "\n");

  const char *maker = dtpipe_get_camera_maker(img);
  const char *model = dtpipe_get_camera_model(img);
  fprintf(fp, "camera_maker: %s\n", maker ? maker : "(unknown)");
  fprintf(fp, "camera_model: %s\n", model ? model : "(unknown)");
  fprintf(fp, "full_width:   %d\n", dtpipe_get_width(img));
  fprintf(fp, "full_height:  %d\n", dtpipe_get_height(img));
  fprintf(fp, "render_scale: %.2f\n", RENDER_SCALE);

  int n = dtpipe_get_module_count();
  fprintf(fp, "module_count: %d\n", n);
  fprintf(fp, "modules:");
  for(int i = 0; i < n; i++)
  {
    const char *name = dtpipe_get_module_name(i);
    fprintf(fp, " %s", name ? name : "?");
  }
  fprintf(fp, "\n");

  fprintf(fp, "\n# Presets\n");
  for(int i = 0; i < N_PRESETS; i++)
    fprintf(fp, "%s: %s\n", PRESETS[i].name, PRESETS[i].description);

  fclose(fp);
  printf("  metadata -> %s\n", meta_path);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
  const char *raf_path  = (argc > 1) ? argv[1] : "../../test-image/DSCF4379.RAF";
  const char *out_dir   = (argc > 2) ? argv[2] : "tests/reference";

  printf("╔══════════════════════════════════════════════════╗\n");
  printf("║  libdtpipe reference render generator (7.2)      ║\n");
  printf("╚══════════════════════════════════════════════════╝\n");
  printf("RAW input : %s\n", raf_path);
  printf("Output dir: %s\n", out_dir);
  printf("\n");

  /* Init */
  int rc = dtpipe_init(NULL);
  if(rc != DTPIPE_OK && rc != DTPIPE_ERR_ALREADY_INIT)
  {
    fprintf(stderr, "FATAL: dtpipe_init failed: %d\n", rc);
    return 1;
  }

  /* Load image */
  printf("Loading RAW ...\n");
  dt_image_t *img = dtpipe_load_raw(raf_path);
  if(!img)
  {
    fprintf(stderr, "FATAL: cannot load '%s': %s\n", raf_path, dtpipe_get_last_error());
    dtpipe_cleanup();
    return 1;
  }
  printf("Loaded: %d x %d  %s %s\n",
         dtpipe_get_width(img), dtpipe_get_height(img),
         dtpipe_get_camera_maker(img) ? dtpipe_get_camera_maker(img) : "",
         dtpipe_get_camera_model(img) ? dtpipe_get_camera_model(img) : "");
  printf("\n");

  /* Create output directory */
  if(make_dir(out_dir) != 0)
  {
    dtpipe_free_image(img);
    dtpipe_cleanup();
    return 1;
  }

  /* Write metadata */
  write_metadata(img, out_dir);
  printf("\n");

  /* Generate each preset */
  int failures = 0;
  for(int i = 0; i < N_PRESETS; i++)
  {
    if(render_preset(img, &PRESETS[i], out_dir) != 0)
    {
      fprintf(stderr, "ERROR: preset '%s' failed\n", PRESETS[i].name);
      failures++;
    }
    printf("\n");
  }

  /* Cleanup */
  dtpipe_free_image(img);
  dtpipe_cleanup();

  /* Summary */
  if(failures == 0)
  {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  All %d preset(s) written to %s\n", N_PRESETS, out_dir);
    printf("╚══════════════════════════════════════════════════╝\n");
    return 0;
  }
  else
  {
    fprintf(stderr, "╔══════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  %d preset(s) FAILED                              ║\n", failures);
    fprintf(stderr, "╚══════════════════════════════════════════════════╝\n");
    return 1;
  }
}
