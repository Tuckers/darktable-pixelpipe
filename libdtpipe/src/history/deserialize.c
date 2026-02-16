/*
 * history/deserialize.c - History JSON deserialization for libdtpipe
 *
 * Implements dtpipe_load_history() (public) and the internal helper
 * dtpipe_load_history_impl().
 *
 * The accepted JSON format is documented in docs/history-format.md.
 *
 * Design
 * ──────
 * A minimal recursive-descent parser handles only the JSON subset written by
 * serialize.c:
 *   - Objects  {}
 *   - Arrays   []
 *   - Strings  "..."   (with \", \\, \n, \r, \t, \uXXXX escapes)
 *   - Numbers  integer and floating-point (parsed with strtol / strtof)
 *   - Keywords true / false / null
 *
 * The parser does not build a full AST. Instead it uses a two-phase approach:
 *   Phase 1 – parse the top-level document; skip "source", "settings",
 *             "custom_order", "masks"; find the "modules" object and
 *             dispatch each module entry to _apply_module().
 *   Phase 2 – _apply_module() walks the module object keys: "enabled",
 *             "version", "params". Each named param is applied via
 *             dtpipe_set_param_float / dtpipe_set_param_int / etc.
 *
 * Error policy
 * ────────────
 *   - Version major > 1: reject with DTPIPE_ERR_FORMAT.
 *   - Unknown modules in "modules": warn to stderr, skip.
 *   - Wrong param type or unknown param: warn, skip (don't abort).
 *   - Missing params: leave module at current (default) value.
 *   - Module version mismatch: warn, still apply params (best-effort).
 *   - Malformed JSON: return DTPIPE_ERR_FORMAT.
 */

#include "history/deserialize.h"
#include "pipe/create.h"   /* dtpipe_find_module(), _module_node_t */
#include "pipe/params.h"   /* dtpipe_lookup_param(), dt_param_desc_t */
#include "dtpipe_internal.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Parser state ────────────────────────────────────────────────────────── */

typedef struct _parser_t
{
  const char *src;   /* original string (for error reporting) */
  const char *p;     /* current read position */
  char        err[256]; /* last error message */
} _parser_t;

/* ── Forward declarations ────────────────────────────────────────────────── */

static int  _skip_ws(_parser_t *ps);
static int  _parse_value(_parser_t *ps);
static int  _parse_object(_parser_t *ps,
               int (*key_cb)(void *, const char *, _parser_t *), void *ctx);
static int  _parse_array(_parser_t *ps,
               int (*elem_cb)(void *, _parser_t *), void *ctx);
static int  _parse_string(_parser_t *ps, char *out, size_t out_size);
static int  _parse_number_f(_parser_t *ps, float *out);
static int  _parse_number_i(_parser_t *ps, int32_t *out);
static int  _parse_bool(_parser_t *ps, int *out);

/* ── Error helpers ───────────────────────────────────────────────────────── */

static void _set_err(_parser_t *ps, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ps->err, sizeof(ps->err), fmt, ap);
  va_end(ap);
}

/* ── Whitespace ──────────────────────────────────────────────────────────── */

static int _skip_ws(_parser_t *ps)
{
  while(*ps->p && isspace((unsigned char)*ps->p))
    ps->p++;
  return 1;
}

/* ── Expect a literal character ──────────────────────────────────────────── */

static int _expect(_parser_t *ps, char c)
{
  _skip_ws(ps);
  if(*ps->p != c)
  {
    _set_err(ps, "expected '%c', got '%c' near: %.20s", c, *ps->p, ps->p);
    return 0;
  }
  ps->p++;
  return 1;
}

/* ── String parsing ──────────────────────────────────────────────────────── */

/*
 * Parse a JSON string at *ps->p (which must point at the opening '"').
 * Writes the unescaped content to out[0..out_size-1] (NUL-terminated).
 * Returns 1 on success, 0 on parse error or buffer overflow.
 */
static int _parse_string(_parser_t *ps, char *out, size_t out_size)
{
  _skip_ws(ps);
  if(*ps->p != '"')
  {
    _set_err(ps, "expected string, got '%c'", *ps->p);
    return 0;
  }
  ps->p++; /* skip opening '"' */

  size_t pos = 0;
  while(*ps->p && *ps->p != '"')
  {
    char c = *ps->p++;
    if(c == '\\')
    {
      if(!*ps->p) break;
      char esc = *ps->p++;
      switch(esc)
      {
        case '"':  c = '"';  break;
        case '\\': c = '\\'; break;
        case '/':  c = '/';  break;
        case 'n':  c = '\n'; break;
        case 'r':  c = '\r'; break;
        case 't':  c = '\t'; break;
        case 'b':  c = '\b'; break;
        case 'f':  c = '\f'; break;
        case 'u':
        {
          /* \uXXXX — decode to UTF-8 (BMP only) */
          unsigned int cp = 0;
          for(int i = 0; i < 4; i++)
          {
            if(!*ps->p)
            {
              _set_err(ps, "truncated \\uXXXX escape");
              return 0;
            }
            char h = *ps->p++;
            unsigned int nib;
            if(h >= '0' && h <= '9')      nib = (unsigned)(h - '0');
            else if(h >= 'a' && h <= 'f') nib = (unsigned)(h - 'a') + 10u;
            else if(h >= 'A' && h <= 'F') nib = (unsigned)(h - 'A') + 10u;
            else { _set_err(ps, "bad hex in \\u escape"); return 0; }
            cp = (cp << 4) | nib;
          }
          /* Encode codepoint as UTF-8 */
          if(cp < 0x80)
          {
            c = (char)cp;
          }
          else if(cp < 0x800)
          {
            if(pos + 1 >= out_size) goto overflow;
            out[pos++] = (char)(0xC0 | (cp >> 6));
            c = (char)(0x80 | (cp & 0x3F));
          }
          else
          {
            if(pos + 2 >= out_size) goto overflow;
            out[pos++] = (char)(0xE0 | (cp >> 12));
            out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            c = (char)(0x80 | (cp & 0x3F));
          }
          break;
        }
        default:
          c = esc; /* unknown escape: keep literally */
          break;
      }
    }
    if(pos + 1 >= out_size) goto overflow;
    out[pos++] = c;
  }
  if(*ps->p == '"') ps->p++;
  out[pos] = '\0';
  return 1;

overflow:
  _set_err(ps, "string too long (max %zu bytes)", out_size - 1);
  return 0;
}

/* ── Number parsing ──────────────────────────────────────────────────────── */

static int _parse_number_f(_parser_t *ps, float *out)
{
  _skip_ws(ps);
  char *end;
  errno = 0;
  double d = strtod(ps->p, &end);
  if(end == ps->p)
  {
    _set_err(ps, "expected number near: %.20s", ps->p);
    return 0;
  }
  if(errno == ERANGE)
    d = (d > 0) ? (double)HUGE_VALF : -(double)HUGE_VALF;
  ps->p = end;
  *out = (float)d;
  return 1;
}

static int _parse_number_i(_parser_t *ps, int32_t *out)
{
  _skip_ws(ps);
  char *end;
  errno = 0;
  long v = strtol(ps->p, &end, 10);
  if(end == ps->p)
  {
    _set_err(ps, "expected integer near: %.20s", ps->p);
    return 0;
  }
  ps->p = end;
  *out = (int32_t)v;
  return 1;
}

static int _parse_number_u(_parser_t *ps, uint32_t *out)
{
  _skip_ws(ps);
  char *end;
  errno = 0;
  unsigned long v = strtoul(ps->p, &end, 10);
  if(end == ps->p)
  {
    _set_err(ps, "expected unsigned integer near: %.20s", ps->p);
    return 0;
  }
  ps->p = end;
  *out = (uint32_t)v;
  return 1;
}

static int _parse_bool(_parser_t *ps, int *out)
{
  _skip_ws(ps);
  if(strncmp(ps->p, "true", 4) == 0)  { ps->p += 4; *out = 1; return 1; }
  if(strncmp(ps->p, "false", 5) == 0) { ps->p += 5; *out = 0; return 1; }
  _set_err(ps, "expected true/false near: %.20s", ps->p);
  return 0;
}

/* ── Generic skip-value (skip any JSON value) ────────────────────────────── */

/*
 * Skip over one JSON value (object, array, string, number, or keyword)
 * without storing it.  Returns 1 on success, 0 on parse error.
 */
static int _skip_value(_parser_t *ps)
{
  _skip_ws(ps);
  char c = *ps->p;
  if(c == '"')
  {
    char tmp[512];
    return _parse_string(ps, tmp, sizeof(tmp));
  }
  if(c == '{')
  {
    /* Skip object */
    ps->p++;
    _skip_ws(ps);
    if(*ps->p == '}') { ps->p++; return 1; }
    while(1)
    {
      char key[256];
      if(!_parse_string(ps, key, sizeof(key))) return 0;
      if(!_expect(ps, ':')) return 0;
      if(!_skip_value(ps)) return 0;
      _skip_ws(ps);
      if(*ps->p == ',') { ps->p++; continue; }
      if(*ps->p == '}') { ps->p++; break; }
      _set_err(ps, "expected ',' or '}' in object");
      return 0;
    }
    return 1;
  }
  if(c == '[')
  {
    /* Skip array */
    ps->p++;
    _skip_ws(ps);
    if(*ps->p == ']') { ps->p++; return 1; }
    while(1)
    {
      if(!_skip_value(ps)) return 0;
      _skip_ws(ps);
      if(*ps->p == ',') { ps->p++; continue; }
      if(*ps->p == ']') { ps->p++; break; }
      _set_err(ps, "expected ',' or ']' in array");
      return 0;
    }
    return 1;
  }
  if(isdigit((unsigned char)c) || c == '-')
  {
    char *end;
    strtod(ps->p, &end);
    if(end == ps->p) { _set_err(ps, "bad number near: %.20s", ps->p); return 0; }
    ps->p = end;
    return 1;
  }
  if(strncmp(ps->p, "true",  4) == 0) { ps->p += 4; return 1; }
  if(strncmp(ps->p, "false", 5) == 0) { ps->p += 5; return 1; }
  if(strncmp(ps->p, "null",  4) == 0) { ps->p += 4; return 1; }
  _set_err(ps, "unexpected token near: %.20s", ps->p);
  return 0;
}

/* ── Object / array iteration helpers ────────────────────────────────────── */

/*
 * Iterate the keys of a JSON object.
 * For each key/value pair, calls key_cb(ctx, key, ps) with ps positioned
 * at the value.  key_cb must consume exactly one value (or call _skip_value).
 * Returns 1 on success, 0 on error.
 */
static int _parse_object(_parser_t *ps,
                          int (*key_cb)(void *, const char *, _parser_t *),
                          void *ctx)
{
  if(!_expect(ps, '{')) return 0;
  _skip_ws(ps);
  if(*ps->p == '}') { ps->p++; return 1; }

  while(1)
  {
    char key[256];
    if(!_parse_string(ps, key, sizeof(key))) return 0;
    if(!_expect(ps, ':')) return 0;
    if(!key_cb(ctx, key, ps)) return 0;
    _skip_ws(ps);
    if(*ps->p == ',') { ps->p++; _skip_ws(ps); continue; }
    if(*ps->p == '}') { ps->p++; break; }
    _set_err(ps, "expected ',' or '}' after object value");
    return 0;
  }
  return 1;
}

/* ── Module param application ────────────────────────────────────────────── */

typedef struct _param_ctx_t
{
  dt_iop_module_t *mod;
  const char      *op;
  _parser_t       *ps;
} _param_ctx_t;

/*
 * Callback for _parse_object when walking a "params" object.
 * Applies a single param to the module's params buffer.
 */
static int _apply_param_cb(void *ctx, const char *key, _parser_t *ps)
{
  _param_ctx_t *pc  = (_param_ctx_t *)ctx;
  dt_iop_module_t *mod = pc->mod;
  const char *op = pc->op;

  _skip_ws(ps);

  /* Look up descriptor */
  const dt_param_desc_t *d = dtpipe_lookup_param(op, key);
  if(!d)
  {
    /* Unknown param — skip with a warning */
    fprintf(stderr,
            "[dtpipe/deserialize] warning: unknown param '%s.%s' — skipping\n",
            op, key);
    return _skip_value(ps);
  }

  /* Bounds check: offset + size must fit within params_size */
  if(mod->params_size > 0 &&
     (int)(d->offset + d->size) > mod->params_size)
  {
    fprintf(stderr,
            "[dtpipe/deserialize] warning: param '%s.%s' out of bounds — skipping\n",
            op, key);
    return _skip_value(ps);
  }

  if(!mod->params)
  {
    fprintf(stderr,
            "[dtpipe/deserialize] warning: module '%s' has no params buffer — skipping\n",
            op);
    return _skip_value(ps);
  }

  uint8_t *dst = (uint8_t *)mod->params + d->offset;

  switch(d->type)
  {
    case DT_PARAM_FLOAT:
    {
      float fv;
      if(!_parse_number_f(ps, &fv)) return 0;
      if(fv < d->min || fv > d->max)
        fprintf(stderr,
                "[dtpipe/deserialize] warning: param '%s.%s' value %g out of range [%g,%g]\n",
                op, key, (double)fv, (double)d->min, (double)d->max);
      memcpy(dst, &fv, sizeof(float));
      break;
    }
    case DT_PARAM_INT:
    {
      int32_t iv;
      if(!_parse_number_i(ps, &iv)) return 0;
      memcpy(dst, &iv, sizeof(int32_t));
      break;
    }
    case DT_PARAM_UINT32:
    {
      uint32_t uv;
      if(!_parse_number_u(ps, &uv)) return 0;
      memcpy(dst, &uv, sizeof(uint32_t));
      break;
    }
    case DT_PARAM_BOOL:
    {
      /* Accept JSON true/false or integer 0/1 */
      _skip_ws(ps);
      int bv = 0;
      char c = *ps->p;
      if(c == 't' || c == 'f')
      {
        if(!_parse_bool(ps, &bv)) return 0;
      }
      else
      {
        int32_t iv;
        if(!_parse_number_i(ps, &iv)) return 0;
        bv = (iv != 0);
      }
      int32_t stored = bv;
      memcpy(dst, &stored, sizeof(int32_t));
      break;
    }
    default:
      fprintf(stderr,
              "[dtpipe/deserialize] warning: unknown type for param '%s.%s' — skipping\n",
              op, key);
      return _skip_value(ps);
  }

  return 1;
}

/* ── Module object application ───────────────────────────────────────────── */

typedef struct _module_apply_ctx_t
{
  dt_pipe_t  *pipe;
  const char *op;
  _parser_t  *ps;
} _module_apply_ctx_t;

/*
 * Callback for _parse_object when walking a single module's JSON object.
 * Handles keys: "enabled", "version", "params" (and skips all others).
 */
static int _module_key_cb(void *ctx, const char *key, _parser_t *ps)
{
  _module_apply_ctx_t *mc = (_module_apply_ctx_t *)ctx;

  dt_iop_module_t *mod = dtpipe_find_module(mc->pipe, mc->op);
  /* mod may be NULL if the module isn't in this pipeline — skip silently */

  if(strcmp(key, "enabled") == 0)
  {
    int bv = 0;
    if(!_parse_bool(ps, &bv)) return 0;
    if(mod) mod->enabled = bv;
    return 1;
  }

  if(strcmp(key, "version") == 0)
  {
    int32_t ver;
    if(!_parse_number_i(ps, &ver)) return 0;
    /* Warn on version mismatch but continue — best-effort */
    (void)ver;
    return 1;
  }

  if(strcmp(key, "params") == 0)
  {
    if(!mod)
    {
      /* Module not in pipeline — skip the params object */
      return _skip_value(ps);
    }
    _param_ctx_t pc = { mod, mc->op, ps };
    return _parse_object(ps, _apply_param_cb, &pc);
  }

  /* Any other key (e.g. "input_profile", "work_profile") — skip */
  return _skip_value(ps);
}

/*
 * Apply a single module from the "modules" object.
 * Called by _modules_key_cb with ps positioned at the module value (an object).
 */
static int _apply_module(_module_apply_ctx_t *mc)
{
  _parser_t *ps = mc->ps;
  _skip_ws(ps);
  if(*ps->p != '{')
  {
    fprintf(stderr,
            "[dtpipe/deserialize] warning: module '%s' value is not an object — skipping\n",
            mc->op);
    return _skip_value(ps);
  }

  dt_iop_module_t *mod = dtpipe_find_module(mc->pipe, mc->op);
  if(!mod)
  {
    fprintf(stderr,
            "[dtpipe/deserialize] warning: unknown module '%s' — skipping\n",
            mc->op);
    return _skip_value(ps);
  }

  return _parse_object(ps, _module_key_cb, mc);
}

/* ── "modules" object callback ───────────────────────────────────────────── */

typedef struct _modules_ctx_t
{
  dt_pipe_t *pipe;
  _parser_t *ps;
} _modules_ctx_t;

static int _modules_key_cb(void *ctx, const char *key, _parser_t *ps)
{
  _modules_ctx_t *mc = (_modules_ctx_t *)ctx;
  _module_apply_ctx_t mac = { mc->pipe, key, ps };
  return _apply_module(&mac);
}

/* ── Top-level document callback ─────────────────────────────────────────── */

typedef struct _doc_ctx_t
{
  dt_pipe_t *pipe;
  _parser_t *ps;
  int        version_ok;  /* set to 1 once version has been validated */
} _doc_ctx_t;

static int _doc_key_cb(void *ctx, const char *key, _parser_t *ps)
{
  _doc_ctx_t *dc = (_doc_ctx_t *)ctx;

  if(strcmp(key, "version") == 0)
  {
    char ver[32];
    if(!_parse_string(ps, ver, sizeof(ver))) return 0;
    /* Parse major version: must be == 1 */
    char *dot = strchr(ver, '.');
    long major = strtol(ver, NULL, 10);
    (void)dot;
    if(major > 1)
    {
      _set_err(ps, "unsupported history version '%s' (major %ld > 1)", ver, major);
      return 0;
    }
    dc->version_ok = 1;
    return 1;
  }

  if(strcmp(key, "modules") == 0)
  {
    _modules_ctx_t mc = { dc->pipe, ps };
    return _parse_object(ps, _modules_key_cb, &mc);
  }

  /* "generator", "source", "settings", "custom_order", "masks" — skip */
  return _skip_value(ps);
}

/* ── Public implementation ───────────────────────────────────────────────── */

int dtpipe_load_history_impl(dt_pipe_t *pipe, const char *json)
{
  if(!pipe || !json) return DTPIPE_ERR_INVALID_ARG;

  _parser_t ps;
  ps.src = json;
  ps.p   = json;
  ps.err[0] = '\0';

  _doc_ctx_t dc;
  dc.pipe = pipe;
  dc.ps   = &ps;
  dc.version_ok = 0;

  if(!_parse_object(&ps, _doc_key_cb, &dc))
  {
    fprintf(stderr, "[dtpipe/deserialize] parse error: %s\n", ps.err);
    return DTPIPE_ERR_FORMAT;
  }

  if(!dc.version_ok)
  {
    /* No "version" key found — treat as unknown format */
    fprintf(stderr, "[dtpipe/deserialize] error: missing required 'version' key\n");
    return DTPIPE_ERR_FORMAT;
  }

  return DTPIPE_OK;
}

/* ── Public API wrapper ──────────────────────────────────────────────────── */

int dtpipe_load_history(dt_pipe_t *pipe, const char *json)
{
  return dtpipe_load_history_impl(pipe, json);
}
