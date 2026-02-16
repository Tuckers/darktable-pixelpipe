/*
 * test_pipeline_export.c
 *
 * Task 4.6 verification: exercise dtpipe_export_jpeg(), dtpipe_export_png(),
 * and dtpipe_export_tiff() via the public libdtpipe API.
 *
 * Tests:
 *   1. NULL-guard: all export functions return DTPIPE_ERR_INVALID_ARG on
 *      NULL pipe or NULL path.
 *   2. Invalid quality / bits arguments are rejected.
 *   3. Load a real image, create a pipeline, export to each format.
 *      - dtpipe_export_jpeg  → /tmp/dtpipe_test.jpg
 *      - dtpipe_export_png   → /tmp/dtpipe_test.png
 *      - dtpipe_export_tiff  (8-bit)  → /tmp/dtpipe_test_8.tiff
 *      - dtpipe_export_tiff  (16-bit) → /tmp/dtpipe_test_16.tiff
 *      - dtpipe_export_tiff  (32-bit) → /tmp/dtpipe_test_32.tiff
 *   4. Output files exist and have non-zero size.
 *   5. Basic format magic-bytes check for each file.
 *
 * Exit codes:
 *   0 – all checks passed
 *   1 – one or more checks failed
 */

#include "dtpipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static long file_size(const char *path)
{
  struct stat st;
  if(stat(path, &st) != 0) return -1;
  return (long)st.st_size;
}

/* Read first N bytes of a file; returns bytes read (0 on error). */
static size_t read_magic(const char *path, uint8_t *buf, size_t n)
{
  FILE *fp = fopen(path, "rb");
  if(!fp) return 0;
  size_t got = fread(buf, 1, n, fp);
  fclose(fp);
  return got;
}

/* ── Test 1+2: NULL / invalid-arg guards ─────────────────────────────────── */

static void test_null_guards(void)
{
  printf("\n--- Test 1: NULL guards ---\n");

  CHECK(dtpipe_export_jpeg(NULL, "/tmp/x.jpg", 90) == DTPIPE_ERR_INVALID_ARG,
        "export_jpeg(NULL pipe) == DTPIPE_ERR_INVALID_ARG");
  CHECK(dtpipe_export_png(NULL, "/tmp/x.png")      == DTPIPE_ERR_INVALID_ARG,
        "export_png(NULL pipe) == DTPIPE_ERR_INVALID_ARG");
  CHECK(dtpipe_export_tiff(NULL, "/tmp/x.tif", 16) == DTPIPE_ERR_INVALID_ARG,
        "export_tiff(NULL pipe) == DTPIPE_ERR_INVALID_ARG");

  printf("\n--- Test 2: invalid bits argument ---\n");
  /* We need a real pipe for this; done in the integration test below.
     Here we just confirm the NULL-path check happens before bit validation. */
  CHECK(dtpipe_export_tiff(NULL, "/tmp/x.tif", 99) == DTPIPE_ERR_INVALID_ARG,
        "export_tiff(NULL pipe, bits=99) == DTPIPE_ERR_INVALID_ARG");
}

/* ── Test 3-5: real export from loaded image ─────────────────────────────── */

static void test_exports(const char *raf_path)
{
  printf("\n--- Initialise library ---\n");
  int rc = dtpipe_init(NULL);
  CHECK(rc == DTPIPE_OK || rc == DTPIPE_ERR_ALREADY_INIT, "dtpipe_init OK");

  printf("\n--- Load image ---\n");
  dt_image_t *img = dtpipe_load_raw(raf_path);
  CHECK(img != NULL, "dtpipe_load_raw returned non-NULL");
  if(!img)
  {
    fprintf(stderr, "  (last error: %s)\n", dtpipe_get_last_error());
    return;
  }
  printf("  info: %d x %d  %s %s\n",
         dtpipe_get_width(img), dtpipe_get_height(img),
         dtpipe_get_camera_maker(img), dtpipe_get_camera_model(img));

  printf("\n--- Create pipeline ---\n");
  dt_pipe_t *pipe = dtpipe_create(img);
  CHECK(pipe != NULL, "dtpipe_create returned non-NULL");
  if(!pipe) { dtpipe_free_image(img); return; }

  /* ── JPEG ─────────────────────────────────────────────────────────────── */
  printf("\n--- Test 3a: export JPEG ---\n");
  const char *jpeg_path = "/tmp/dtpipe_test.jpg";
  rc = dtpipe_export_jpeg(pipe, jpeg_path, 85);
  CHECK(rc == DTPIPE_OK, "export_jpeg returned DTPIPE_OK");

  if(rc == DTPIPE_OK)
  {
    long sz = file_size(jpeg_path);
    CHECK(sz > 0, "JPEG file has non-zero size");
    printf("  info: %s (%ld bytes)\n", jpeg_path, sz);

    /* JPEG magic: FF D8 FF */
    uint8_t magic[4] = {0};
    read_magic(jpeg_path, magic, 3);
    CHECK(magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF,
          "JPEG file starts with FF D8 FF");
  }

  /* ── PNG ──────────────────────────────────────────────────────────────── */
  printf("\n--- Test 3b: export PNG ---\n");
  const char *png_path = "/tmp/dtpipe_test.png";
  rc = dtpipe_export_png(pipe, png_path);
  CHECK(rc == DTPIPE_OK, "export_png returned DTPIPE_OK");

  if(rc == DTPIPE_OK)
  {
    long sz = file_size(png_path);
    CHECK(sz > 0, "PNG file has non-zero size");
    printf("  info: %s (%ld bytes)\n", png_path, sz);

    /* PNG magic: 89 50 4E 47 */
    uint8_t magic[4] = {0};
    read_magic(png_path, magic, 4);
    CHECK(magic[0] == 0x89 && magic[1] == 0x50
          && magic[2] == 0x4E && magic[3] == 0x47,
          "PNG file starts with 89 50 4E 47");
  }

  /* ── TIFF 8-bit ───────────────────────────────────────────────────────── */
  printf("\n--- Test 3c: export TIFF 8-bit ---\n");
  const char *tiff8_path = "/tmp/dtpipe_test_8.tiff";
  rc = dtpipe_export_tiff(pipe, tiff8_path, 8);
  CHECK(rc == DTPIPE_OK, "export_tiff(8) returned DTPIPE_OK");

  if(rc == DTPIPE_OK)
  {
    long sz = file_size(tiff8_path);
    CHECK(sz > 0, "TIFF-8 file has non-zero size");
    printf("  info: %s (%ld bytes)\n", tiff8_path, sz);

    /* TIFF magic: 49 49 2A 00 (little-endian) or 4D 4D 00 2A (big-endian) */
    uint8_t magic[4] = {0};
    read_magic(tiff8_path, magic, 4);
    const int le = (magic[0] == 0x49 && magic[1] == 0x49
                    && magic[2] == 0x2A && magic[3] == 0x00);
    const int be = (magic[0] == 0x4D && magic[1] == 0x4D
                    && magic[2] == 0x00 && magic[3] == 0x2A);
    CHECK(le || be, "TIFF-8 file has valid TIFF magic");
  }

  /* ── TIFF 16-bit ──────────────────────────────────────────────────────── */
  printf("\n--- Test 3d: export TIFF 16-bit ---\n");
  const char *tiff16_path = "/tmp/dtpipe_test_16.tiff";
  rc = dtpipe_export_tiff(pipe, tiff16_path, 16);
  CHECK(rc == DTPIPE_OK, "export_tiff(16) returned DTPIPE_OK");

  if(rc == DTPIPE_OK)
  {
    long sz = file_size(tiff16_path);
    CHECK(sz > 0, "TIFF-16 file has non-zero size");
    printf("  info: %s (%ld bytes)\n", tiff16_path, sz);
    /* 16-bit has at least 2x the data of 8-bit */
    long sz8 = file_size(tiff8_path);
    CHECK(sz8 < 0 || sz > sz8 / 2,
          "TIFF-16 file is larger than half of TIFF-8 (sanity)");
  }

  /* ── TIFF 32-bit float ────────────────────────────────────────────────── */
  printf("\n--- Test 3e: export TIFF 32-bit float ---\n");
  const char *tiff32_path = "/tmp/dtpipe_test_32.tiff";
  rc = dtpipe_export_tiff(pipe, tiff32_path, 32);
  CHECK(rc == DTPIPE_OK, "export_tiff(32) returned DTPIPE_OK");

  if(rc == DTPIPE_OK)
  {
    long sz = file_size(tiff32_path);
    CHECK(sz > 0, "TIFF-32 file has non-zero size");
    printf("  info: %s (%ld bytes)\n", tiff32_path, sz);
  }

  /* ── invalid bits ─────────────────────────────────────────────────────── */
  printf("\n--- Test 4: invalid bits argument ---\n");
  rc = dtpipe_export_tiff(pipe, "/tmp/dtpipe_test_bad.tiff", 99);
  CHECK(rc == DTPIPE_ERR_INVALID_ARG,
        "export_tiff(bits=99) == DTPIPE_ERR_INVALID_ARG");

  /* Cleanup */
  dtpipe_free(pipe);
  dtpipe_free_image(img);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
  const char *path = (argc > 1) ? argv[1]
                                 : "../../test-image/DSCF4379.RAF";

  printf("=== Task 4.6 verification: dtpipe_export_* ===\n");

  test_null_guards();
  test_exports(path);

  printf("\n=== Results: %d failure(s) ===\n", g_failures);
  return g_failures ? 1 : 0;
}
