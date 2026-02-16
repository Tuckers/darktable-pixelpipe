/*
 * history/xmp_read.cc - XMP sidecar reading for libdtpipe
 *
 * Implements dtpipe_load_xmp() (public) and dtpipe_load_xmp_impl().
 *
 * darktable XMP format
 * ────────────────────
 * History entries live in <darktable:history><rdf:Seq><rdf:li .../>...
 * Each <rdf:li> carries these attributes:
 *
 *   darktable:num           – history stack index (0-based)
 *   darktable:operation     – IOP name, e.g. "exposure"
 *   darktable:enabled       – "1" or "0"
 *   darktable:modversion    – params struct version integer
 *   darktable:params        – encoded params (see below)
 *   darktable:multi_priority – "0" for primary instance; >0 = extra instance
 *
 * history_end on the parent <rdf:Description> tells us how many entries
 * in the sequence are valid (entries with num >= history_end are stale).
 *
 * Params encoding
 * ───────────────
 * Two variants appear in the wild:
 *
 *   Plain hex  – lowercase hex string, e.g. "22f4d03f0000803f..."
 *                Used for simpler/smaller structs.
 *
 *   gz-encoded – "gz" + 2 hex digits (zlib header bytes that were
 *                stripped before base64) + standard base64.
 *                E.g. "gz48eJzjZhg..." where "48" is two hex digits
 *                and "eJzj..." is the base64 of the zlib-compressed data.
 *
 *                Decoding: prepend the 1-byte header (the two hex digits
 *                decoded), base64-decode the remainder, prepend the header
 *                byte, then zlib-decompress.
 *
 * Once decoded we have the raw params struct bytes in little-endian layout.
 * We use the param descriptor tables (params.h) to copy individual fields
 * into the pipeline module's params buffer at the correct offsets.
 *
 * Error policy
 * ────────────
 *   - File not found / unreadable → DTPIPE_ERR_NOT_FOUND / DTPIPE_ERR_IO
 *   - No darktable:history element → DTPIPE_ERR_FORMAT
 *   - Unknown operation name → warn, skip
 *   - Params blob shorter than descriptor offset → warn, skip field
 *   - Module version mismatch → warn, apply best-effort
 *   - multi_priority > 0 → warn, skip (multi-instance not yet supported)
 *   - gz decode / zlib error → warn, skip params (apply enabled state only)
 */

/*
 * Include <atomic> before dtpipe_internal.h so that the C++ standard header
 * is pulled in before dtpipe_internal.h's extern "C" block opens.
 * dtpipe_internal.h conditionally includes <atomic> when compiled as C++,
 * and including a C++ standard header inside extern "C" triggers errors on
 * modern libc++ (the <atomic> header uses C++ templates).
 */
#include <atomic>

#include "history/xmp_read.h"
#include "pipe/create.h"   /* dtpipe_find_module() */
#include "pipe/params.h"   /* dtpipe_lookup_param(), dtpipe_get_param_desc() */
#include "dtpipe_internal.h"

#include <pugixml.hpp>
#include <zlib.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ── Base64 decode ───────────────────────────────────────────────────────── */

static const int8_t _b64_table[256] = {
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* 0x00 */
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* 0x10 */
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, /* 0x20: '+','/' */
  52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1, /* 0x30: '0'-'9','=' */
  -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, /* 0x40: 'A'-'O' */
  15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1, /* 0x50: 'P'-'Z' */
  -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, /* 0x60: 'a'-'o' */
  41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, /* 0x70: 'p'-'z' */
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

/*
 * Decode base64 string into out[].
 * Returns number of bytes written, or -1 on error.
 * out_size must be >= ceil(strlen(b64) * 3/4).
 */
static int _base64_decode(const char *b64, uint8_t *out, size_t out_size)
{
  size_t len = strlen(b64);
  size_t pos = 0;

  for(size_t i = 0; i < len; )
  {
    /* Skip whitespace */
    while(i < len && (b64[i] == '\n' || b64[i] == '\r' ||
                      b64[i] == ' '  || b64[i] == '\t'))
      i++;
    if(i >= len) break;

    /* Collect up to 4 base64 chars */
    int8_t v[4] = {0, 0, 0, 0};
    int count = 0;
    bool done = false;
    for(int k = 0; k < 4 && i < len; k++, i++)
    {
      char c = b64[i];
      if(c == '=') { i++; done = true; break; } /* advance past '=' and stop */
      int8_t dv = _b64_table[(unsigned char)c];
      if(dv < 0)
      {
        fprintf(stderr, "[dtpipe/xmp_read] bad base64 char '%c'\n", c);
        return -1;
      }
      v[k] = dv;
      count = k + 1;
    }

    if(count >= 2)
    {
      if(pos >= out_size) return -1;
      out[pos++] = (uint8_t)((v[0] << 2) | (v[1] >> 4));
    }
    if(count >= 3)
    {
      if(pos >= out_size) return -1;
      out[pos++] = (uint8_t)((v[1] << 4) | (v[2] >> 2));
    }
    if(count >= 4)
    {
      if(pos >= out_size) return -1;
      out[pos++] = (uint8_t)((v[2] << 6) | v[3]);
    }
    if(done) break;
  }
  return (int)pos;
}

/* ── Params decoding ─────────────────────────────────────────────────────── */

static const size_t MAX_BLOB = 65536; /* 64 KiB — largest plausible params */

/*
 * Decode a darktable params string into blob[].
 * Handles both plain hex and gz-encoded variants.
 * Returns number of bytes decoded, or -1 on error.
 */
static int _decode_params(const char *params_str, uint8_t *blob, size_t blob_size)
{
  if(!params_str || params_str[0] == '\0') return 0;

  if(params_str[0] == 'g' && params_str[1] == 'z')
  {
    /*
     * gz-encoded: "gz" + 2 hex digits (informational, ignored) + base64
     *
     * darktable's format is:
     *   "gz" <2-char-hex-level> <base64-of-zlib-stream>
     *
     * The 2 hex chars are an artifact of the encoding (likely the zlib
     * compression level used), NOT a byte to prepend to the stream.
     * The base64 portion decodes directly to a valid zlib stream (starts
     * with the standard 0x78 zlib magic byte).
     */
    const char *b64 = params_str + 4; /* skip "gz" + 2 hex chars */

    size_t b64_len    = strlen(b64);
    size_t comp_max   = b64_len + 4;
    uint8_t *comp_buf = (uint8_t *)malloc(comp_max);
    if(!comp_buf) return -1;

    int b64_bytes = _base64_decode(b64, comp_buf, comp_max);
    if(b64_bytes < 0)
    {
      free(comp_buf);
      return -1;
    }

    uLongf dest_len = (uLongf)blob_size;
    int zret = uncompress(blob, &dest_len, comp_buf, (uLong)b64_bytes);
    free(comp_buf);

    if(zret != Z_OK)
    {
      fprintf(stderr, "[dtpipe/xmp_read] zlib error %d decompressing params\n", zret);
      return -1;
    }
    return (int)dest_len;
  }
  else
  {
    /* Plain hex: decode directly */
    size_t hex_len = strlen(params_str);
    if(hex_len & 1)
    {
      fprintf(stderr, "[dtpipe/xmp_read] odd-length hex string (%zu)\n", hex_len);
      return -1;
    }
    size_t byte_count = hex_len / 2;
    if(byte_count > blob_size) byte_count = blob_size;
    for(size_t i = 0; i < byte_count; i++)
    {
      unsigned int hi2, lo2;
      char hc2 = params_str[i * 2];
      char lc2 = params_str[i * 2 + 1];
      if     (hc2 >= '0' && hc2 <= '9') hi2 = (unsigned)(hc2 - '0');
      else if(hc2 >= 'a' && hc2 <= 'f') hi2 = (unsigned)(hc2 - 'a') + 10u;
      else if(hc2 >= 'A' && hc2 <= 'F') hi2 = (unsigned)(hc2 - 'A') + 10u;
      else { fprintf(stderr, "[dtpipe/xmp_read] bad hex char '%c'\n", hc2); return -1; }
      if     (lc2 >= '0' && lc2 <= '9') lo2 = (unsigned)(lc2 - '0');
      else if(lc2 >= 'a' && lc2 <= 'f') lo2 = (unsigned)(lc2 - 'a') + 10u;
      else if(lc2 >= 'A' && lc2 <= 'F') lo2 = (unsigned)(lc2 - 'A') + 10u;
      else { fprintf(stderr, "[dtpipe/xmp_read] bad hex char '%c'\n", lc2); return -1; }
      blob[i] = (uint8_t)((hi2 << 4) | lo2);
    }
    return (int)byte_count;
  }
}

/* ── Apply a single decoded history entry to the pipeline ────────────────── */

static void _apply_history_entry(dt_pipe_t *pipe,
                                  const char *op,
                                  int enabled,
                                  int modversion,
                                  const uint8_t *blob,
                                  int blob_len)
{
  dt_iop_module_t *mod = dtpipe_find_module(pipe, op);
  if(!mod)
  {
    /* Unknown to this pipeline — silently skip */
    return;
  }

  mod->enabled = enabled;

  /* Version mismatch check omitted: dt_iop_module_t carries no version field.
   * The XMP modversion is noted but not enforced — apply best-effort. */
  (void)modversion;

  if(!mod->params || mod->params_size <= 0 || blob_len <= 0)
    return;

  int count = dtpipe_param_count(op);
  if(count <= 0)
  {
    /* No descriptor table — raw copy up to min(blob_len, params_size) */
    int n = blob_len < mod->params_size ? blob_len : mod->params_size;
    memcpy(mod->params, blob, (size_t)n);
    return;
  }

  for(int i = 0; i < count; i++)
  {
    const dt_param_desc_t *d = dtpipe_get_param_desc(op, i);
    if(!d) break;

    if((int)(d->offset + d->size) > blob_len)
    {
      fprintf(stderr,
              "[dtpipe/xmp_read] warning: '%s.%s' offset+size=%zu > blob=%d — skip\n",
              op, d->name, d->offset + d->size, blob_len);
      continue;
    }
    if((int)(d->offset + d->size) > mod->params_size)
    {
      fprintf(stderr,
              "[dtpipe/xmp_read] warning: '%s.%s' offset+size=%zu > params_size=%d — skip\n",
              op, d->name, d->offset + d->size, mod->params_size);
      continue;
    }

    memcpy((uint8_t *)mod->params + d->offset,
           blob + d->offset,
           d->size);
  }
}

/* ── XMP traversal helper ────────────────────────────────────────────────── */

/* Find a node named "darktable:history" anywhere in the document. */
struct _HistoryFinder : pugi::xml_tree_walker
{
  pugi::xml_node result;
  bool for_each(pugi::xml_node &node) override
  {
    const char *n = node.name();
    if(strcmp(n, "darktable:history") == 0)
    {
      result = node;
      return false;
    }
    return true;
  }
};

/* ── Main implementation ─────────────────────────────────────────────────── */

int dtpipe_load_xmp_impl(dt_pipe_t *pipe, const char *path)
{
  if(!pipe || !path) return DTPIPE_ERR_INVALID_ARG;

  /* Load and parse */
  pugi::xml_document doc;
  pugi::xml_parse_result pr = doc.load_file(path);

  if(pr.status == pugi::status_file_not_found ||
     pr.status == pugi::status_io_error)
  {
    fprintf(stderr, "[dtpipe/xmp_read] cannot open '%s': %s\n",
            path, pr.description());
    return DTPIPE_ERR_NOT_FOUND;
  }
  if(!pr)
  {
    fprintf(stderr, "[dtpipe/xmp_read] XML parse error in '%s': %s\n",
            path, pr.description());
    return DTPIPE_ERR_FORMAT;
  }

  /* Find <darktable:history> */
  _HistoryFinder finder;
  doc.traverse(finder);
  pugi::xml_node history_node = finder.result;
  if(!history_node)
  {
    fprintf(stderr, "[dtpipe/xmp_read] no darktable:history in '%s'\n", path);
    return DTPIPE_ERR_FORMAT;
  }

  /* Read history_end from the nearest ancestor that has it */
  int history_end = INT_MAX;
  for(pugi::xml_node anc = history_node.parent(); anc; anc = anc.parent())
  {
    pugi::xml_attribute he = anc.attribute("darktable:history_end");
    if(he) { history_end = he.as_int(INT_MAX); break; }
  }

  /* Find <rdf:Seq> */
  pugi::xml_node seq = history_node.child("rdf:Seq");
  if(!seq) seq = history_node.child("Seq");
  if(!seq)
  {
    fprintf(stderr, "[dtpipe/xmp_read] no rdf:Seq in darktable:history\n");
    return DTPIPE_ERR_FORMAT;
  }

  /*
   * Collect history entries.  Per operation, keep only the entry with the
   * highest `num` that is still < history_end (= current active edit state).
   *
   * We store entries in a flat array and do a linear scan — there are at most
   * ~30 unique module names in a typical pipeline.
   */
  struct _Entry
  {
    char op[64];
    int  num;
    int  enabled;
    int  modversion;
    /* Params string pointer into pugixml's string pool — valid while doc lives */
    const char *params_str;
  };

  static const int MAX_OPS = 64;
  _Entry slots[MAX_OPS];
  int    slot_count = 0;

  auto find_slot = [&](const char *op) -> _Entry *
  {
    for(int i = 0; i < slot_count; i++)
      if(strcmp(slots[i].op, op) == 0) return &slots[i];
    if(slot_count < MAX_OPS)
    {
      _Entry &e = slots[slot_count++];
      strncpy(e.op, op, sizeof(e.op) - 1);
      e.op[sizeof(e.op) - 1] = '\0';
      e.num = -1;
      e.params_str = nullptr;
      return &e;
    }
    return nullptr;
  };

  for(pugi::xml_node li : seq.children("rdf:li"))
  {
    const char *op  = li.attribute("darktable:operation").value();
    if(!op || op[0] == '\0') continue;

    int num  = li.attribute("darktable:num").as_int(0);
    int mpri = li.attribute("darktable:multi_priority").as_int(0);

    if(num >= history_end) continue;

    if(mpri > 0)
    {
      fprintf(stderr,
              "[dtpipe/xmp_read] skipping multi-instance '%s' (priority %d)\n",
              op, mpri);
      continue;
    }

    _Entry *slot = find_slot(op);
    if(!slot)
    {
      fprintf(stderr, "[dtpipe/xmp_read] too many modules, skipping '%s'\n", op);
      continue;
    }
    if(num <= slot->num) continue; /* older entry */

    slot->num        = num;
    slot->enabled    = li.attribute("darktable:enabled").as_int(1);
    slot->modversion = li.attribute("darktable:modversion").as_int(0);
    slot->params_str = li.attribute("darktable:params").value();
  }

  /* Decode and apply */
  uint8_t *blob = (uint8_t *)malloc(MAX_BLOB);
  if(!blob) return DTPIPE_ERR_NO_MEMORY;

  for(int i = 0; i < slot_count; i++)
  {
    _Entry &e = slots[i];
    if(e.num < 0) continue;

    int blob_len = 0;
    if(e.params_str && e.params_str[0] != '\0')
    {
      blob_len = _decode_params(e.params_str, blob, MAX_BLOB);
      if(blob_len < 0)
      {
        fprintf(stderr,
                "[dtpipe/xmp_read] warning: failed to decode params for '%s' "
                "— applying enabled state only\n", e.op);
        blob_len = 0;
      }
    }

    _apply_history_entry(pipe, e.op, e.enabled, e.modversion, blob, blob_len);
  }

  free(blob);
  return DTPIPE_OK;
}

/* ── Public API wrapper ──────────────────────────────────────────────────── */

extern "C" int dtpipe_load_xmp(dt_pipe_t *pipe, const char *path)
{
  return dtpipe_load_xmp_impl(pipe, path);
}
