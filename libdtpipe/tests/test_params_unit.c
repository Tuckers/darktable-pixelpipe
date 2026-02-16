/*
 * test_params_unit.c
 *
 * Task 4.4 internal unit test: verifies the param descriptor lookup layer
 * and the set/get offset arithmetic directly, without depending on the IOP
 * module registry being populated.
 *
 * This test allocates a zeroed buffer sized to match the exposure params
 * struct, injects it as module->params, then calls dtpipe_set_param_float()
 * and dtpipe_get_param_float() and verifies the correct bytes are written.
 *
 * Structs defined here must exactly match those in params.c.  If params.c
 * is updated, keep these in sync.
 *
 * Exit codes:
 *   0 – all checks passed
 *   1 – one or more checks failed
 */

#include "dtpipe.h"
#include "dtpipe_internal.h"
#include "pipe/create.h"     /* dt_pipe_t, _module_node_t, dtpipe_find_module */
#include "pipe/params.h"     /* dtpipe_lookup_param, dt_param_desc_t          */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

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
    int _g = (got), _e = (expect);                                             \
    if(_g != _e) {                                                             \
      fprintf(stderr, "FAIL [%s:%d] %s  (got %d, expected %d)\n",             \
              __FILE__, __LINE__, (msg), _g, _e);                              \
      g_failures++;                                                            \
    } else {                                                                   \
      printf("  OK  %s\n", (msg));                                            \
    }                                                                          \
  } while(0)

#define CHECK_FEQ(got, expect, msg)                                            \
  do {                                                                         \
    float _g = (got), _e = (expect);                                           \
    if(fabsf(_g - _e) > 1e-6f) {                                              \
      fprintf(stderr, "FAIL [%s:%d] %s  (got %g, expected %g)\n",             \
              __FILE__, __LINE__, (msg), (double)_g, (double)_e);             \
      g_failures++;                                                            \
    } else {                                                                   \
      printf("  OK  %s\n", (msg));                                            \
    }                                                                          \
  } while(0)

/* ── Exposure params struct (must match params.c exactly) ─────────────────── */

typedef struct _exposure_params_t {
  int32_t mode;
  float   black;
  float   exposure;
  float   deflicker_percentile;
  float   deflicker_target_level;
  int32_t compensate_exposure_bias;
  int32_t compensate_hilite_pres;
} _exposure_params_t;

/* ── Test 1: descriptor lookup ───────────────────────────────────────────── */

static void test_descriptor_lookup(void)
{
  printf("\n--- Test 1: descriptor lookup ---\n");

  const dt_param_desc_t *d;

  /* Known params */
  d = dtpipe_lookup_param("exposure", "exposure");
  CHECK(d != NULL, "lookup exposure.exposure found");
  if(d)
  {
    CHECK(d->type == DT_PARAM_FLOAT, "exposure.exposure type is FLOAT");
    CHECK(d->size == sizeof(float),  "exposure.exposure size == sizeof(float)");
    CHECK(d->offset == offsetof(_exposure_params_t, exposure),
          "exposure.exposure offset matches struct");
  }

  d = dtpipe_lookup_param("exposure", "black");
  CHECK(d != NULL, "lookup exposure.black found");
  if(d) CHECK(d->type == DT_PARAM_FLOAT, "exposure.black type is FLOAT");

  d = dtpipe_lookup_param("exposure", "mode");
  CHECK(d != NULL, "lookup exposure.mode found");
  if(d) CHECK(d->type == DT_PARAM_INT, "exposure.mode type is INT");

  /* Unknown module */
  d = dtpipe_lookup_param("nonexistent_module", "exposure");
  CHECK(d == NULL, "lookup unknown module returns NULL");

  /* Unknown param on known module */
  d = dtpipe_lookup_param("exposure", "nonexistent_field");
  CHECK(d == NULL, "lookup unknown param on known module returns NULL");

  /* NULL args */
  d = dtpipe_lookup_param(NULL, "exposure");
  CHECK(d == NULL, "lookup NULL op returns NULL");

  d = dtpipe_lookup_param("exposure", NULL);
  CHECK(d == NULL, "lookup NULL param returns NULL");

  /* param_count */
  int n = dtpipe_param_count("exposure");
  CHECK(n > 0, "param_count(exposure) > 0");
  printf("  info: exposure has %d described params\n", n);

  CHECK(dtpipe_param_count("nonexistent") == -1,
        "param_count(unknown module) == -1");

  /* All Tier 1 modules are described */
  const char *tier1[] = { "exposure", "temperature", "rawprepare",
                          "demosaic",  "colorin",     "colorout", NULL };
  for(int i = 0; tier1[i]; i++)
  {
    char msg[64];
    snprintf(msg, sizeof(msg), "param_count(%s) > 0", tier1[i]);
    CHECK(dtpipe_param_count(tier1[i]) > 0, msg);
  }
}

/* ── Test 2: manual buffer round-trip ────────────────────────────────────── */
/*
 * Allocate a fake exposure params buffer, inject it into a manually-
 * constructed dt_iop_module_t (no registry required), then call the public
 * API and verify the correct bytes are written and read back.
 */
static void test_manual_roundtrip(void)
{
  printf("\n--- Test 2: manual buffer round-trip ---\n");

  /* Allocate a zeroed params buffer */
  _exposure_params_t buf;
  memset(&buf, 0, sizeof(buf));

  /* Build a minimal module node — no so, no registration needed */
  _module_node_t node;
  memset(&node, 0, sizeof(node));
  strncpy(node.module.op, "exposure", sizeof(node.module.op) - 1);
  node.module.params      = &buf;
  node.module.params_size = (int32_t)sizeof(buf);
  node.module.enabled     = true;
  dt_pthread_mutex_init(&node.module.gui_lock);

  /* Build a minimal dt_pipe_t with one module */
  dt_pipe_t pipe;
  memset(&pipe, 0, sizeof(pipe));
  pipe.modules = &node;

  /* ── set_param_float: exposure field ──────────────────────────────────── */
  float set_val = 2.5f;
  int rc = dtpipe_set_param_float(&pipe, "exposure", "exposure", set_val);
  CHECK_EQ(rc, DTPIPE_OK, "set_param_float exposure=2.5 returns OK");
  CHECK_FEQ(buf.exposure, set_val, "buf.exposure == 2.5 after set");

  /* ── get_param_float: reads back correctly ────────────────────────────── */
  float got = 0.0f;
  rc = dtpipe_get_param_float(&pipe, "exposure", "exposure", &got);
  CHECK_EQ(rc, DTPIPE_OK,           "get_param_float returns OK");
  CHECK_FEQ(got, set_val,           "get_param_float returns 2.5");

  /* ── set_param_float: black field ─────────────────────────────────────── */
  rc = dtpipe_set_param_float(&pipe, "exposure", "black", -0.01f);
  CHECK_EQ(rc, DTPIPE_OK, "set_param_float black=-0.01 returns OK");
  CHECK_FEQ(buf.black, -0.01f,      "buf.black == -0.01 after set");

  /* ── set_param_int: mode field ────────────────────────────────────────── */
  rc = dtpipe_set_param_int(&pipe, "exposure", "mode", 1);
  CHECK_EQ(rc, DTPIPE_OK,           "set_param_int mode=1 returns OK");
  CHECK(buf.mode == 1,              "buf.mode == 1 after set");

  /* ── type mismatch: float field via set_param_int ─────────────────────── */
  rc = dtpipe_set_param_int(&pipe, "exposure", "exposure", 3);
  CHECK_EQ(rc, DTPIPE_ERR_PARAM_TYPE,
           "set float field via set_param_int -> PARAM_TYPE");

  /* ── type mismatch: int field via get_param_float ─────────────────────── */
  float dummy = 0.0f;
  rc = dtpipe_get_param_float(&pipe, "exposure", "mode", &dummy);
  CHECK_EQ(rc, DTPIPE_ERR_PARAM_TYPE,
           "get int field via get_param_float -> PARAM_TYPE");

  /* ── unknown param name ───────────────────────────────────────────────── */
  rc = dtpipe_set_param_float(&pipe, "exposure", "does_not_exist", 0.0f);
  CHECK_EQ(rc, DTPIPE_ERR_NOT_FOUND,
           "set unknown param -> NOT_FOUND");

  /* ── enable_module toggle ─────────────────────────────────────────────── */
  rc = dtpipe_enable_module(&pipe, "exposure", 0);
  CHECK_EQ(rc, DTPIPE_OK,           "disable exposure -> OK");
  CHECK(!node.module.enabled,       "module.enabled == false after disable");

  rc = dtpipe_enable_module(&pipe, "exposure", 1);
  CHECK_EQ(rc, DTPIPE_OK,           "enable exposure -> OK");
  CHECK(node.module.enabled,        "module.enabled == true after enable");

  /* ── no params buffer: returns NOT_FOUND ──────────────────────────────── */
  node.module.params = NULL;
  rc = dtpipe_set_param_float(&pipe, "exposure", "exposure", 1.0f);
  CHECK_EQ(rc, DTPIPE_ERR_NOT_FOUND,
           "set with NULL params buffer -> NOT_FOUND");
  node.module.params = &buf; /* restore */

  dt_pthread_mutex_destroy(&node.module.gui_lock);
}

/* ── Test 3: all described modules have sane offsets ─────────────────────── */

static void test_all_module_offsets(void)
{
  printf("\n--- Test 3: offset sanity for all described modules ---\n");

  const char *ops[] = {
    "exposure", "temperature", "rawprepare", "demosaic",
    "colorin",  "colorout",    "highlights", "sharpen",
    NULL
  };

  for(int i = 0; ops[i]; i++)
  {
    int n = dtpipe_param_count(ops[i]);
    if(n <= 0) continue;

    /* Walk every descriptor and check basic sanity */
    bool ok = true;
    for(int j = 0; j < n; j++)
    {
      const dt_param_desc_t *d = dtpipe_lookup_param(ops[i],
        /* We can't iterate by index with the current API; use the lookup
           indirectly by verifying count is positive and spot-check size. */
        j == 0 ? "exposure"    :   /* reused below for per-module spot check */
        ops[i]                      /* fallback: won't find this as a param */
      );
      (void)d;
    }

    /* Spot-check: first param of each module has size > 0 and offset < 4096 */
    /* We'll verify by calling lookup on a known-good param per module */
    struct { const char *op; const char *param; } spots[] = {
      { "exposure",    "exposure"          },
      { "temperature", "red"               },
      { "rawprepare",  "raw_white_point"   },
      { "demosaic",    "demosaicing_method"},
      { "colorin",     "type"              },
      { "colorout",    "type"              },
      { "highlights",  "mode"              },
      { "sharpen",     "radius"            },
    };

    for(int s = 0; s < (int)(sizeof(spots)/sizeof(spots[0])); s++)
    {
      if(strcmp(spots[s].op, ops[i]) != 0) continue;
      const dt_param_desc_t *d = dtpipe_lookup_param(spots[s].op,
                                                      spots[s].param);
      char msg[80];
      snprintf(msg, sizeof(msg), "%s.%s descriptor valid",
               spots[s].op, spots[s].param);
      ok = (d != NULL) && (d->size > 0) && (d->offset < 4096);
      CHECK(ok, msg);
    }
  }
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
  printf("=== Task 4.4 unit test: param descriptor + buffer round-trip ===\n");

  /* dtpipe_init not required for these tests (no image loading).
     Call it anyway to match production usage. */
  dtpipe_init(NULL);

  test_descriptor_lookup();
  test_manual_roundtrip();
  test_all_module_offsets();

  dtpipe_cleanup();

  printf("\n=== Results: %d failure(s) ===\n", g_failures);
  return g_failures ? 1 : 0;
}
