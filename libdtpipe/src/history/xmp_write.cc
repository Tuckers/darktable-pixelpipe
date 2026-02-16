/*
 * history/xmp_write.cc - XMP sidecar writing for libdtpipe
 *
 * Implements dtpipe_save_xmp() (public) and dtpipe_save_xmp_impl().
 *
 * Output format
 * ─────────────
 * Produces a darktable-compatible XMP sidecar that darktable can read back.
 * The structure mirrors what darktable writes:
 *
 *   <?xml version="1.0" encoding="UTF-8"?>
 *   <x:xmpmeta xmlns:x="adobe:ns:meta/">
 *    <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
 *     <rdf:Description rdf:about=""
 *         xmlns:darktable="http://darktable.sf.net/"
 *         darktable:history_end="N">
 *      <darktable:history>
 *       <rdf:Seq>
 *        <rdf:li
 *          darktable:num="0"
 *          darktable:operation="exposure"
 *          darktable:enabled="1"
 *          darktable:modversion="7"
 *          darktable:params="000000000000803f..."
 *          darktable:multi_priority="0"
 *          darktable:multi_name=""/>
 *        ...
 *       </rdf:Seq>
 *      </darktable:history>
 *     </rdf:Description>
 *    </rdf:RDF>
 *   </x:xmpmeta>
 *
 * Params encoding
 * ───────────────
 * We always use plain hex (lowercase hex string of the raw packed struct
 * bytes). This is simpler than gz-encoding and always readable by darktable.
 * darktable supports both encodings when reading.
 *
 * Multi-instance modules
 * ──────────────────────
 * libdtpipe does not yet support multi-instance modules. All modules are
 * written with multi_priority="0" and multi_name="".
 *
 * iop_order
 * ─────────
 * darktable also stores iop_order metadata. We write a minimal compatible
 * set: history entries only (no iop_order list attribute). darktable will
 * reconstruct order from the operation names on open.
 */

/*
 * Pull in <atomic> before dtpipe_internal.h to avoid the C++ standard
 * header-inside-extern-"C" issue (same pattern as xmp_read.cc).
 */
#include <atomic>

#include "history/xmp_write.h"
#include "pipe/create.h"   /* _module_node_t, dtpipe_find_module */
#include "pipe/params.h"   /* dtpipe_param_count, dtpipe_get_param_desc */
#include "dtpipe_internal.h"

#include <pugixml.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ── Hex encoding ────────────────────────────────────────────────────────── */

/*
 * Encode `len` bytes from `data` as a lowercase hex string into `out`.
 * out must be at least 2*len+1 bytes.
 */
static void _hex_encode(const uint8_t *data, size_t len, char *out)
{
  static const char hex[] = "0123456789abcdef";
  for(size_t i = 0; i < len; i++)
  {
    out[i * 2]     = hex[(data[i] >> 4) & 0xf];
    out[i * 2 + 1] = hex[data[i] & 0xf];
  }
  out[len * 2] = '\0';
}

/* ── Params → hex ────────────────────────────────────────────────────────── */

/*
 * Encode a module's params buffer as a hex string.
 * Returns a heap-allocated string that the caller must free(), or NULL on OOM.
 * Returns empty string "" (heap-allocated) if module has no params.
 */
static char *_encode_params(const dt_iop_module_t *m)
{
  if(!m->params || m->params_size <= 0)
  {
    char *empty = (char *)malloc(1);
    if(empty) empty[0] = '\0';
    return empty;
  }

  size_t hex_len = (size_t)m->params_size * 2 + 1;
  char *hex = (char *)malloc(hex_len);
  if(!hex) return NULL;

  _hex_encode((const uint8_t *)m->params, (size_t)m->params_size, hex);
  return hex;
}

/* ── Main implementation ─────────────────────────────────────────────────── */

int dtpipe_save_xmp_impl(dt_pipe_t *pipe, const char *path)
{
  if(!pipe || !path) return DTPIPE_ERR_INVALID_ARG;

  pugi::xml_document doc;

  /* XML declaration */
  pugi::xml_node decl = doc.append_child(pugi::node_declaration);
  decl.append_attribute("version")  = "1.0";
  decl.append_attribute("encoding") = "UTF-8";

  /* <x:xmpmeta> */
  pugi::xml_node xmpmeta = doc.append_child("x:xmpmeta");
  xmpmeta.append_attribute("xmlns:x") = "adobe:ns:meta/";

  /* <rdf:RDF> */
  pugi::xml_node rdf = xmpmeta.append_child("rdf:RDF");
  rdf.append_attribute("xmlns:rdf") =
    "http://www.w3.org/1999/02/22-rdf-syntax-ns#";

  /* <rdf:Description> */
  pugi::xml_node desc = rdf.append_child("rdf:Description");
  desc.append_attribute("rdf:about") = "";
  desc.append_attribute("xmlns:darktable") = "http://darktable.sf.net/";

  /* Count modules to set history_end */
  int history_end = 0;
  for(_module_node_t *mn = (_module_node_t *)pipe->modules; mn; mn = mn->next)
  {
    if(mn->module.op[0] != '\0')
      history_end++;
  }
  desc.append_attribute("darktable:history_end") = history_end;

  /* <darktable:history> */
  pugi::xml_node history = desc.append_child("darktable:history");

  /* <rdf:Seq> */
  pugi::xml_node seq = history.append_child("rdf:Seq");

  int num = 0;
  for(_module_node_t *mn = (_module_node_t *)pipe->modules; mn; mn = mn->next)
  {
    const dt_iop_module_t *m = &mn->module;
    if(m->op[0] == '\0') continue;

    char *params_hex = _encode_params(m);
    if(!params_hex) return DTPIPE_ERR_NO_MEMORY;

    pugi::xml_node li = seq.append_child("rdf:li");
    li.append_attribute("darktable:num")            = num;
    li.append_attribute("darktable:operation")      = m->op;
    li.append_attribute("darktable:enabled")        = m->enabled ? 1 : 0;
    li.append_attribute("darktable:modversion")     = 0; /* placeholder */
    li.append_attribute("darktable:params")         = params_hex;
    li.append_attribute("darktable:multi_priority") = 0;
    li.append_attribute("darktable:multi_name")     = "";

    free(params_hex);
    num++;
  }

  /* Save with indentation */
  bool ok = doc.save_file(path, "  ");
  if(!ok)
  {
    fprintf(stderr, "[dtpipe/xmp_write] failed to write '%s'\n", path);
    return DTPIPE_ERR_IO;
  }

  return DTPIPE_OK;
}

/* ── Public API wrapper ──────────────────────────────────────────────────── */

extern "C" int dtpipe_save_xmp(dt_pipe_t *pipe, const char *path)
{
  return dtpipe_save_xmp_impl(pipe, path);
}
