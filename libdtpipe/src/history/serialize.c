/*
 * history/serialize.c - History JSON serialization for libdtpipe
 *
 * Implements dtpipe_serialize_history() (public) and the internal helper
 * dtpipe_serialize_history_impl().
 *
 * The output JSON format is documented in docs/history-format.md.
 *
 * Design
 * ──────
 * We avoid a third-party JSON library. Instead we use a simple growable
 * byte buffer (_buf_t) and write JSON tokens directly. This keeps the
 * dependency count at zero and produces compact, predictable output.
 *
 * The serializer iterates over all module instances in the pipeline
 * (pipe->modules) in iop_order and for each module emits:
 *   - "enabled"
 *   - "version"  (from the module's so->version, or 0 if unavailable)
 *   - "params"   (all fields described in the params descriptor table)
 *
 * Only modules that have a descriptor table entry in params.c are
 * emitted with a non-empty "params" object. Modules without a table
 * (not yet described) are still emitted with enabled/version but with
 * an empty "params": {}.
 *
 * Special string fields (colorin profile names, colorout profile name)
 * are not yet wired — those are deferred to Phase 5+ color management
 * work. The serializer emits the numeric type/intent params only.
 */

#include "history/serialize.h"
#include "pipe/create.h"
#include "pipe/params.h"
#include "dtpipe_internal.h"
#include "common/iop_order.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Growable string buffer ──────────────────────────────────────────────── */

typedef struct _buf_t
{
  char  *data;
  size_t len;   /* bytes written, not counting the NUL */
  size_t cap;   /* allocated bytes including the NUL slot */
} _buf_t;

#define BUF_INIT_CAP 4096

static int _buf_init(_buf_t *b)
{
  b->data = (char *)malloc(BUF_INIT_CAP);
  if(!b->data) return 0;
  b->data[0] = '\0';
  b->len = 0;
  b->cap = BUF_INIT_CAP;
  return 1;
}

static void _buf_free(_buf_t *b)
{
  free(b->data);
  b->data = NULL;
  b->len  = 0;
  b->cap  = 0;
}

/* Ensure at least `need` more bytes fit (plus NUL). Returns 0 on OOM. */
static int _buf_reserve(_buf_t *b, size_t need)
{
  size_t required = b->len + need + 1;
  if(required <= b->cap) return 1;

  size_t new_cap = b->cap * 2;
  while(new_cap < required) new_cap *= 2;

  char *p = (char *)realloc(b->data, new_cap);
  if(!p) return 0;
  b->data = p;
  b->cap  = new_cap;
  return 1;
}

/* Append a C string. Returns 0 on OOM. */
static int _buf_puts(_buf_t *b, const char *s)
{
  size_t n = strlen(s);
  if(!_buf_reserve(b, n)) return 0;
  memcpy(b->data + b->len, s, n + 1); /* includes NUL */
  b->len += n;
  return 1;
}

/* Printf-style append. Returns 0 on OOM. */
static int _buf_printf(_buf_t *b, const char *fmt, ...)
{
  /* First attempt with a stack buffer to avoid double-pass in the common case */
  char tmp[128];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  if(n < 0) return 0;

  if((size_t)n < sizeof(tmp))
  {
    return _buf_puts(b, tmp);
  }

  /* Rare: output was truncated — allocate exact size */
  char *heap = (char *)malloc((size_t)n + 1);
  if(!heap) return 0;
  va_start(ap, fmt);
  vsnprintf(heap, (size_t)n + 1, fmt, ap);
  va_end(ap);
  int ok = _buf_puts(b, heap);
  free(heap);
  return ok;
}

/* ── JSON escaping ───────────────────────────────────────────────────────── */

/*
 * Append a JSON-escaped string value (including surrounding double-quotes).
 * We only need to escape control characters, backslash, and double-quote —
 * all other bytes are passed through unchanged (UTF-8 safe).
 */
static int _buf_put_json_string(_buf_t *b, const char *s)
{
  if(!_buf_puts(b, "\"")) return 0;
  for(; *s; s++)
  {
    unsigned char c = (unsigned char)*s;
    if(c == '"')       { if(!_buf_puts(b, "\\\"")) return 0; }
    else if(c == '\\') { if(!_buf_puts(b, "\\\\")) return 0; }
    else if(c == '\n') { if(!_buf_puts(b, "\\n"))  return 0; }
    else if(c == '\r') { if(!_buf_puts(b, "\\r"))  return 0; }
    else if(c == '\t') { if(!_buf_puts(b, "\\t"))  return 0; }
    else if(c < 0x20)
    {
      if(!_buf_printf(b, "\\u%04x", (unsigned)c)) return 0;
    }
    else
    {
      char ch[2] = { (char)c, '\0' };
      if(!_buf_puts(b, ch)) return 0;
    }
  }
  return _buf_puts(b, "\"");
}

/* ── Float formatting ────────────────────────────────────────────────────── */

/*
 * Format a float value for JSON output.
 *
 * Rules:
 *   - Use up to 8 significant digits (sufficient for float round-trip).
 *   - Strip trailing zeros after the decimal point, but always emit at
 *     least one decimal digit so the value is unambiguously a number.
 *   - Special values (NaN, Inf) become 0.0 with a warning (not valid JSON).
 */
static int _buf_put_float(_buf_t *b, float v)
{
  if(!isfinite(v))
  {
    fprintf(stderr, "[dtpipe/serialize] warning: non-finite float (%g) -> 0.0\n",
            (double)v);
    v = 0.0f;
  }

  /* Use %.8g which gives 8 significant digits and strips trailing zeros */
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%.8g", (double)v);

  /* Ensure there is a decimal point so the value isn't misread as integer */
  int has_dot_or_e = 0;
  for(int i = 0; tmp[i]; i++)
  {
    if(tmp[i] == '.' || tmp[i] == 'e' || tmp[i] == 'E')
    {
      has_dot_or_e = 1;
      break;
    }
  }
  if(!has_dot_or_e)
  {
    /* Append ".0" */
    size_t len = strlen(tmp);
    if(len + 2 < sizeof(tmp))
    {
      tmp[len]     = '.';
      tmp[len + 1] = '0';
      tmp[len + 2] = '\0';
    }
  }

  return _buf_puts(b, tmp);
}

/* ── Module serialization ────────────────────────────────────────────────── */

/*
 * Serialize all known params for a module.
 *
 * Returns 0 on OOM.  If the module has no params block or no descriptor
 * table, emits an empty object {}.
 */
static int _serialize_params(_buf_t *b,
                              const dt_iop_module_t *m,
                              const char *op)
{
  if(!_buf_puts(b, "\"params\": {")) return 0;

  /* No params block allocated — emit empty object */
  if(!m->params)
    return _buf_puts(b, "}");

  int count = dtpipe_param_count(op);
  if(count <= 0)
    return _buf_puts(b, "}");

  int first = 1;
  for(int i = 0; i < count; i++)
  {
    const dt_param_desc_t *d = dtpipe_get_param_desc(op, i);
    if(!d) continue;

    /* Bounds-check: offset + size must fit within params_size */
    if(m->params_size > 0 &&
       (int)(d->offset + d->size) > m->params_size)
    {
      fprintf(stderr,
              "[dtpipe/serialize] warning: param %s.%s out of params_size bounds\n",
              op, d->name);
      continue;
    }

    if(!first) { if(!_buf_puts(b, ", ")) return 0; }
    first = 0;

    /* Key */
    if(!_buf_put_json_string(b, d->name)) return 0;
    if(!_buf_puts(b, ": ")) return 0;

    /* Value */
    const uint8_t *src = (const uint8_t *)m->params + d->offset;
    switch(d->type)
    {
      case DT_PARAM_FLOAT:
      {
        float fv;
        memcpy(&fv, src, sizeof(float));
        if(!_buf_put_float(b, fv)) return 0;
        break;
      }
      case DT_PARAM_INT:
      {
        int32_t iv;
        memcpy(&iv, src, sizeof(int32_t));
        if(!_buf_printf(b, "%" PRId32, iv)) return 0;
        break;
      }
      case DT_PARAM_UINT32:
      {
        uint32_t uv;
        memcpy(&uv, src, sizeof(uint32_t));
        if(!_buf_printf(b, "%" PRIu32, uv)) return 0;
        break;
      }
      case DT_PARAM_BOOL:
      {
        int32_t bv;
        memcpy(&bv, src, sizeof(int32_t));
        if(!_buf_puts(b, bv ? "true" : "false")) return 0;
        break;
      }
      default:
        if(!_buf_puts(b, "null")) return 0;
        break;
    }
  }

  return _buf_puts(b, "}");
}

/*
 * Serialize one module. `indent` is a string of spaces for readability.
 * Returns 0 on OOM.
 */
static int _serialize_module(_buf_t *b, const dt_iop_module_t *m)
{
  const char *op = m->op;
  /* Module version is not exposed on dt_iop_module_so_t in the headless
     build; emit 0 as a placeholder. Phase 5+ can add a version field to
     the so struct when modules are compiled in. */
  int version = 0;

  if(!_buf_puts(b, "    {\n")) return 0;
  if(!_buf_printf(b, "      \"enabled\": %s,\n",
                  m->enabled ? "true" : "false")) return 0;
  if(!_buf_printf(b, "      \"version\": %d,\n", version)) return 0;
  if(!_buf_puts(b, "      ")) return 0;
  if(!_serialize_params(b, m, op)) return 0;
  if(!_buf_puts(b, "\n    }")) return 0;

  return 1;
}

/* ── Public entry point ──────────────────────────────────────────────────── */

char *dtpipe_serialize_history_impl(dt_pipe_t *pipe)
{
  if(!pipe) return NULL;

  _buf_t b;
  if(!_buf_init(&b)) return NULL;

  /* ── Header ─────────────────────────────────────────────────────────────── */
  if(!_buf_puts(&b,
      "{\n"
      "  \"version\": \"1.0\",\n"
      "  \"generator\": \"libdtpipe\",\n"))
    goto oom;

  /* ── Source block ────────────────────────────────────────────────────────── */
  {
    const char *maker = pipe->img
                        ? (pipe->img->camera_maker[0]
                           ? pipe->img->camera_maker : NULL)
                        : NULL;
    const char *model = pipe->img
                        ? (pipe->img->camera_model[0]
                           ? pipe->img->camera_model : NULL)
                        : NULL;
    const char *fname = pipe->img
                        ? (pipe->img->filename[0]
                           ? pipe->img->filename : NULL)
                        : NULL;

    if(fname || maker || model)
    {
      if(!_buf_puts(&b, "  \"source\": {\n")) goto oom;
      int first_src = 1;

      if(fname)
      {
        if(!first_src && !_buf_puts(&b, ",\n")) goto oom;
        first_src = 0;
        if(!_buf_puts(&b, "    \"filename\": ")) goto oom;
        if(!_buf_put_json_string(&b, fname)) goto oom;
      }
      if(maker || model)
      {
        if(!first_src && !_buf_puts(&b, ",\n")) goto oom;
        first_src = 0;
        /* Combine maker + model into "camera" */
        char camera[256] = "";
        if(maker && model)
          snprintf(camera, sizeof(camera), "%s %s", maker, model);
        else if(maker)
          snprintf(camera, sizeof(camera), "%s", maker);
        else
          snprintf(camera, sizeof(camera), "%s", model);

        if(!_buf_puts(&b, "    \"camera\": ")) goto oom;
        if(!_buf_put_json_string(&b, camera)) goto oom;
      }
      if(!_buf_puts(&b, "\n  },\n")) goto oom;
    }
  }

  /* ── Settings block ──────────────────────────────────────────────────────── */
  if(!_buf_puts(&b,
      "  \"settings\": {\n"
      "    \"iop_order\": \"v5.0\",\n"
      "    \"color_workflow\": \"scene-referred\"\n"
      "  },\n"))
    goto oom;

  /* ── Modules block ───────────────────────────────────────────────────────── */
  if(!_buf_puts(&b, "  \"modules\": {\n")) goto oom;

  {
    /* Walk module list in iop_order (already sorted at creation time).
       _module_node_t is defined in pipe/create.h which we include. */
    _module_node_t *mn = (_module_node_t *)pipe->modules;
    int first_mod = 1;

    for(; mn; mn = mn->next)
    {
      const dt_iop_module_t *m = &mn->module;
      const char *op = m->op;
      if(!op[0]) continue;

      if(!first_mod)
      {
        if(!_buf_puts(&b, ",\n")) goto oom;
      }
      first_mod = 0;

      /* Key: op name */
      if(!_buf_puts(&b, "    ")) goto oom;
      if(!_buf_put_json_string(&b, op)) goto oom;
      if(!_buf_puts(&b, ": ")) goto oom;

      /* Value: module object */
      if(!_serialize_module(&b, m)) goto oom;
    }
  }

  if(!_buf_puts(&b, "\n  },\n")) goto oom;

  /* ── Masks placeholder ───────────────────────────────────────────────────── */
  if(!_buf_puts(&b, "  \"masks\": {}\n}\n")) goto oom;

  return b.data; /* caller owns this */

oom:
  _buf_free(&b);
  return NULL;
}

/* ── Public API wrapper ──────────────────────────────────────────────────── */

char *dtpipe_serialize_history(dt_pipe_t *pipe)
{
  return dtpipe_serialize_history_impl(pipe);
}
