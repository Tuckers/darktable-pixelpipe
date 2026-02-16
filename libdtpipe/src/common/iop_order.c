/*
 * iop_order.c - IOP execution order for libdtpipe
 *
 * Ported from darktable src/common/iop_order.c.
 *
 * Changes from original:
 *   - All SQLite / database calls removed
 *   - GLib (GList, gboolean, g_strlcpy, …) replaced with standard C
 *   - dt_develop_t / dt_iop_module_t dependencies removed
 *   - JSON I/O added (replaces DB storage) using pugixml-independent hand-written
 *     parser to avoid a C++ dependency in this translation unit
 *   - Binary and text serialisers preserved verbatim (same wire format)
 */

#include "common/iop_order.h"
#include "dtpipe_internal.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Safe strncpy that always NUL-terminates (like g_strlcpy). */
static void _strlcpy(char *dst, const char *src, size_t n)
{
  if(n == 0) return;
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

/* Count nodes in a list. */
static int _list_length(dt_iop_order_list_t list)
{
  int n = 0;
  for(dt_iop_order_list_node_t *l = list; l; l = l->next)
    n++;
  return n;
}

/* Allocate a new list node (entry is zeroed except for fields set by caller). */
static dt_iop_order_list_node_t *_new_node(const char *operation, int instance)
{
  dt_iop_order_list_node_t *n = calloc(1, sizeof(*n));
  if(!n) return NULL;
  _strlcpy(n->entry.operation, operation, sizeof(n->entry.operation));
  n->entry.instance     = instance;
  n->entry.o.iop_order  = 0;
  n->next               = NULL;
  return n;
}

/* Append a node to a list, returning the (possibly new) head. */
static dt_iop_order_list_node_t *_list_append(dt_iop_order_list_node_t *head,
                                               dt_iop_order_list_node_t *node)
{
  if(!head) return node;
  dt_iop_order_list_node_t *l = head;
  while(l->next) l = l->next;
  l->next = node;
  return head;
}

/* ── iop_order assignment ────────────────────────────────────────────────── */

/*
 * Assign integer iop_order values starting at 100, stepping by 100.
 * This leaves gaps so that multi-instances can be inserted between
 * existing entries without clashing.
 */
static void _reset_iop_order(dt_iop_order_list_t list)
{
  int order = 100;
  for(dt_iop_order_list_node_t *l = list; l; l = l->next)
  {
    l->entry.o.iop_order = order;
    order += 100;
  }
}

/* ── Built-in order tables (copied verbatim from darktable) ──────────────── */

/* Sentinel: entry with empty operation name marks end of table. */
#define DT_IOP_ORDER_TABLE_END { { 0.0f }, "", 0 }

static const dt_iop_order_entry_t _legacy_order[] = {
  { { 1.0f }, "rawprepare",    0},
  { { 2.0f }, "invert",        0},
  { { 3.0f }, "temperature",   0},
  { { 3.1f }, "rasterfile",    0},
  { { 4.0f }, "highlights",    0},
  { { 5.0f }, "cacorrect",     0},
  { { 6.0f }, "hotpixels",     0},
  { { 7.0f }, "rawdenoise",    0},
  { { 8.0f }, "demosaic",      0},
  { { 9.0f }, "mask_manager",  0},
  { {10.0f }, "denoiseprofile",0},
  { {11.0f }, "tonemap",       0},
  { {12.0f }, "exposure",      0},
  { {13.0f }, "spots",         0},
  { {14.0f }, "retouch",       0},
  { {15.0f }, "lens",          0},
  { {15.5f }, "cacorrectrgb",  0},
  { {16.0f }, "ashift",        0},
  { {17.0f }, "liquify",       0},
  { {18.0f }, "rotatepixels",  0},
  { {19.0f }, "scalepixels",   0},
  { {20.0f }, "flip",          0},
  { {20.5f }, "enlargecanvas", 0},
  { {21.0f }, "clipping",      0},
  { {21.5f }, "toneequal",     0},
  { {21.7f }, "crop",          0},
  { {21.9f }, "overlay",       0},
  { {22.0f }, "graduatednd",   0},
  { {23.0f }, "basecurve",     0},
  { {24.0f }, "bilateral",     0},
  { {25.0f }, "profile_gamma", 0},
  { {26.0f }, "hazeremoval",   0},
  { {27.0f }, "colorin",       0},
  { {27.5f }, "channelmixerrgb",0},
  { {27.5f }, "diffuse",       0},
  { {27.5f }, "censorize",     0},
  { {27.5f }, "negadoctor",    0},
  { {27.5f }, "blurs",         0},
  { {27.5f }, "basicadj",      0},
  { {27.5f }, "primaries",     0},
  { {28.0f }, "colorreconstruct",0},
  { {29.0f }, "colorchecker",  0},
  { {30.0f }, "defringe",      0},
  { {31.0f }, "equalizer",     0},
  { {32.0f }, "vibrance",      0},
  { {33.0f }, "colorbalance",  0},
  { {33.2f }, "colorequal",    0},
  { {33.5f }, "colorbalancergb",0},
  { {34.0f }, "colorize",      0},
  { {35.0f }, "colortransfer", 0},
  { {36.0f }, "colormapping",  0},
  { {37.0f }, "bloom",         0},
  { {38.0f }, "nlmeans",       0},
  { {39.0f }, "globaltonemap", 0},
  { {40.0f }, "shadhi",        0},
  { {41.0f }, "atrous",        0},
  { {42.0f }, "bilat",         0},
  { {43.0f }, "colorzones",    0},
  { {44.0f }, "lowlight",      0},
  { {45.0f }, "monochrome",    0},
  { {45.3f }, "sigmoid",       0},
  { {45.5f }, "agx",           0},
  { {46.0f }, "filmic",        0},
  { {46.5f }, "filmicrgb",     0},
  { {47.0f }, "colisa",        0},
  { {48.0f }, "zonesystem",    0},
  { {49.0f }, "tonecurve",     0},
  { {50.0f }, "levels",        0},
  { {50.2f }, "rgblevels",     0},
  { {50.5f }, "rgbcurve",      0},
  { {51.0f }, "relight",       0},
  { {52.0f }, "colorcorrection",0},
  { {53.0f }, "sharpen",       0},
  { {54.0f }, "lowpass",       0},
  { {55.0f }, "highpass",      0},
  { {56.0f }, "grain",         0},
  { {56.5f }, "lut3d",         0},
  { {57.0f }, "colorcontrast", 0},
  { {58.0f }, "colorout",      0},
  { {59.0f }, "channelmixer",  0},
  { {60.0f }, "soften",        0},
  { {61.0f }, "vignette",      0},
  { {62.0f }, "splittoning",   0},
  { {63.0f }, "velvia",        0},
  { {64.0f }, "clahe",         0},
  { {65.0f }, "finalscale",    0},
  { {66.0f }, "overexposed",   0},
  { {67.0f }, "rawoverexposed",0},
  { {67.5f }, "dither",        0},
  { {68.0f }, "borders",       0},
  { {69.0f }, "watermark",     0},
  { {71.0f }, "gamma",         0},
  DT_IOP_ORDER_TABLE_END
};

static const dt_iop_order_entry_t _v30_order[] = {
  { { 1.0f }, "rawprepare",    0},
  { { 2.0f }, "invert",        0},
  { { 3.0f }, "temperature",   0},
  { { 3.1f }, "rasterfile",    0},
  { { 4.0f }, "highlights",    0},
  { { 5.0f }, "cacorrect",     0},
  { { 6.0f }, "hotpixels",     0},
  { { 7.0f }, "rawdenoise",    0},
  { { 8.0f }, "demosaic",      0},
  { { 9.0f }, "denoiseprofile",0},
  { {10.0f }, "bilateral",     0},
  { {11.0f }, "rotatepixels",  0},
  { {12.0f }, "scalepixels",   0},
  { {13.0f }, "lens",          0},
  { {13.5f }, "cacorrectrgb",  0},
  { {14.0f }, "hazeremoval",   0},
  { {15.0f }, "ashift",        0},
  { {16.0f }, "flip",          0},
  { {16.5f }, "enlargecanvas", 0},
  { {16.7f }, "overlay",       0},
  { {17.0f }, "clipping",      0},
  { {18.0f }, "liquify",       0},
  { {19.0f }, "spots",         0},
  { {20.0f }, "retouch",       0},
  { {21.0f }, "exposure",      0},
  { {22.0f }, "mask_manager",  0},
  { {23.0f }, "tonemap",       0},
  { {24.0f }, "toneequal",     0},
  { {24.5f }, "crop",          0},
  { {25.0f }, "graduatednd",   0},
  { {26.0f }, "profile_gamma", 0},
  { {27.0f }, "equalizer",     0},
  { {28.0f }, "colorin",       0},
  { {28.5f }, "channelmixerrgb",0},
  { {28.5f }, "diffuse",       0},
  { {28.5f }, "censorize",     0},
  { {28.5f }, "negadoctor",    0},
  { {28.5f }, "blurs",         0},
  { {28.5f }, "primaries",     0},
  { {29.0f }, "nlmeans",       0},
  { {30.0f }, "colorchecker",  0},
  { {31.0f }, "defringe",      0},
  { {32.0f }, "atrous",        0},
  { {33.0f }, "lowpass",       0},
  { {34.0f }, "highpass",      0},
  { {35.0f }, "sharpen",       0},
  { {37.0f }, "colortransfer", 0},
  { {38.0f }, "colormapping",  0},
  { {39.0f }, "channelmixer",  0},
  { {40.0f }, "basicadj",      0},
  { {41.0f }, "colorbalance",  0},
  { {41.2f }, "colorequal",    0},
  { {41.5f }, "colorbalancergb",0},
  { {42.0f }, "rgbcurve",      0},
  { {43.0f }, "rgblevels",     0},
  { {44.0f }, "basecurve",     0},
  { {45.0f }, "filmic",        0},
  { {45.3f }, "sigmoid",       0},
  { {45.5f }, "agx",           0},
  { {46.0f }, "filmicrgb",     0},
  { {36.0f }, "lut3d",         0},
  { {47.0f }, "colisa",        0},
  { {48.0f }, "tonecurve",     0},
  { {49.0f }, "levels",        0},
  { {50.0f }, "shadhi",        0},
  { {51.0f }, "zonesystem",    0},
  { {52.0f }, "globaltonemap", 0},
  { {53.0f }, "relight",       0},
  { {54.0f }, "bilat",         0},
  { {55.0f }, "colorcorrection",0},
  { {56.0f }, "colorcontrast", 0},
  { {57.0f }, "velvia",        0},
  { {58.0f }, "vibrance",      0},
  { {60.0f }, "colorzones",    0},
  { {61.0f }, "bloom",         0},
  { {62.0f }, "colorize",      0},
  { {63.0f }, "lowlight",      0},
  { {64.0f }, "monochrome",    0},
  { {65.0f }, "grain",         0},
  { {66.0f }, "soften",        0},
  { {67.0f }, "splittoning",   0},
  { {68.0f }, "vignette",      0},
  { {69.0f }, "colorreconstruct",0},
  { {70.0f }, "colorout",      0},
  { {71.0f }, "clahe",         0},
  { {72.0f }, "finalscale",    0},
  { {73.0f }, "overexposed",   0},
  { {74.0f }, "rawoverexposed",0},
  { {75.0f }, "dither",        0},
  { {76.0f }, "borders",       0},
  { {77.0f }, "watermark",     0},
  { {78.0f }, "gamma",         0},
  DT_IOP_ORDER_TABLE_END
};

/* v5.0 RAW — same structure as v3.0 but finalscale moved before colorout */
static const dt_iop_order_entry_t _v50_order[] = {
  { { 1.0f }, "rawprepare",    0},
  { { 2.0f }, "invert",        0},
  { { 3.0f }, "temperature",   0},
  { { 3.1f }, "rasterfile",    0},
  { { 4.0f }, "highlights",    0},
  { { 5.0f }, "cacorrect",     0},
  { { 6.0f }, "hotpixels",     0},
  { { 7.0f }, "rawdenoise",    0},
  { { 8.0f }, "demosaic",      0},
  { { 9.0f }, "denoiseprofile",0},
  { {10.0f }, "bilateral",     0},
  { {11.0f }, "rotatepixels",  0},
  { {12.0f }, "scalepixels",   0},
  { {13.0f }, "lens",          0},
  { {13.5f }, "cacorrectrgb",  0},
  { {14.0f }, "hazeremoval",   0},
  { {15.0f }, "ashift",        0},
  { {16.0f }, "flip",          0},
  { {16.5f }, "enlargecanvas", 0},
  { {16.7f }, "overlay",       0},
  { {17.0f }, "clipping",      0},
  { {18.0f }, "liquify",       0},
  { {19.0f }, "spots",         0},
  { {20.0f }, "retouch",       0},
  { {21.0f }, "exposure",      0},
  { {22.0f }, "mask_manager",  0},
  { {23.0f }, "tonemap",       0},
  { {24.0f }, "toneequal",     0},
  { {24.5f }, "crop",          0},
  { {25.0f }, "graduatednd",   0},
  { {26.0f }, "profile_gamma", 0},
  { {27.0f }, "equalizer",     0},
  { {28.0f }, "colorin",       0},
  { {28.5f }, "channelmixerrgb",0},
  { {28.5f }, "diffuse",       0},
  { {28.5f }, "censorize",     0},
  { {28.5f }, "negadoctor",    0},
  { {28.5f }, "blurs",         0},
  { {28.5f }, "primaries",     0},
  { {29.0f }, "nlmeans",       0},
  { {30.0f }, "colorchecker",  0},
  { {31.0f }, "defringe",      0},
  { {32.0f }, "atrous",        0},
  { {33.0f }, "lowpass",       0},
  { {34.0f }, "highpass",      0},
  { {35.0f }, "sharpen",       0},
  { {37.0f }, "colortransfer", 0},
  { {38.0f }, "colormapping",  0},
  { {39.0f }, "channelmixer",  0},
  { {40.0f }, "basicadj",      0},
  { {41.0f }, "colorbalance",  0},
  { {41.2f }, "colorequal",    0},
  { {41.5f }, "colorbalancergb",0},
  { {42.0f }, "rgbcurve",      0},
  { {43.0f }, "rgblevels",     0},
  { {44.0f }, "basecurve",     0},
  { {45.0f }, "filmic",        0},
  { {45.3f }, "sigmoid",       0},
  { {45.5f }, "agx",           0},
  { {46.0f }, "filmicrgb",     0},
  { {36.0f }, "lut3d",         0},
  { {47.0f }, "colisa",        0},
  { {48.0f }, "tonecurve",     0},
  { {49.0f }, "levels",        0},
  { {50.0f }, "shadhi",        0},
  { {51.0f }, "zonesystem",    0},
  { {52.0f }, "globaltonemap", 0},
  { {53.0f }, "relight",       0},
  { {54.0f }, "bilat",         0},
  { {55.0f }, "colorcorrection",0},
  { {56.0f }, "colorcontrast", 0},
  { {57.0f }, "velvia",        0},
  { {58.0f }, "vibrance",      0},
  { {60.0f }, "colorzones",    0},
  { {61.0f }, "bloom",         0},
  { {62.0f }, "colorize",      0},
  { {63.0f }, "lowlight",      0},
  { {64.0f }, "monochrome",    0},
  { {65.0f }, "grain",         0},
  { {66.0f }, "soften",        0},
  { {67.0f }, "splittoning",   0},
  { {68.0f }, "vignette",      0},
  { {69.0f }, "colorreconstruct",0},
  { {69.4f }, "finalscale",    0},
  { {70.0f }, "colorout",      0},
  { {71.0f }, "clahe",         0},
  { {73.0f }, "overexposed",   0},
  { {74.0f }, "rawoverexposed",0},
  { {75.0f }, "dither",        0},
  { {76.0f }, "borders",       0},
  { {77.0f }, "watermark",     0},
  { {78.0f }, "gamma",         0},
  DT_IOP_ORDER_TABLE_END
};

/* v3.0 JPEG — non-linear-input variant */
static const dt_iop_order_entry_t _v30_jpg_order[] = {
  { { 1.0f }, "rawprepare",    0},
  { { 2.0f }, "invert",        0},
  { { 3.0f }, "temperature",   0},
  { { 3.1f }, "rasterfile",    0},
  { { 4.0f }, "highlights",    0},
  { { 5.0f }, "cacorrect",     0},
  { { 6.0f }, "hotpixels",     0},
  { { 7.0f }, "rawdenoise",    0},
  { { 8.0f }, "demosaic",      0},
  { {28.0f }, "colorin",       0},
  { {28.0f }, "denoiseprofile",0},
  { {28.0f }, "bilateral",     0},
  { {28.0f }, "rotatepixels",  0},
  { {28.0f }, "scalepixels",   0},
  { {28.0f }, "lens",          0},
  { {28.0f }, "cacorrectrgb",  0},
  { {28.0f }, "hazeremoval",   0},
  { {28.0f }, "ashift",        0},
  { {28.0f }, "flip",          0},
  { {28.0f }, "enlargecanvas", 0},
  { {28.0f }, "overlay",       0},
  { {28.0f }, "clipping",      0},
  { {28.0f }, "liquify",       0},
  { {28.0f }, "spots",         0},
  { {28.0f }, "retouch",       0},
  { {28.0f }, "exposure",      0},
  { {28.0f }, "mask_manager",  0},
  { {28.0f }, "tonemap",       0},
  { {28.0f }, "toneequal",     0},
  { {28.0f }, "crop",          0},
  { {28.0f }, "graduatednd",   0},
  { {28.0f }, "profile_gamma", 0},
  { {28.0f }, "equalizer",     0},
  { {28.5f }, "channelmixerrgb",0},
  { {28.5f }, "diffuse",       0},
  { {28.5f }, "censorize",     0},
  { {28.5f }, "negadoctor",    0},
  { {28.5f }, "blurs",         0},
  { {28.5f }, "primaries",     0},
  { {29.0f }, "nlmeans",       0},
  { {30.0f }, "colorchecker",  0},
  { {31.0f }, "defringe",      0},
  { {32.0f }, "atrous",        0},
  { {33.0f }, "lowpass",       0},
  { {34.0f }, "highpass",      0},
  { {35.0f }, "sharpen",       0},
  { {37.0f }, "colortransfer", 0},
  { {38.0f }, "colormapping",  0},
  { {39.0f }, "channelmixer",  0},
  { {40.0f }, "basicadj",      0},
  { {41.0f }, "colorbalance",  0},
  { {41.2f }, "colorequal",    0},
  { {41.5f }, "colorbalancergb",0},
  { {42.0f }, "rgbcurve",      0},
  { {43.0f }, "rgblevels",     0},
  { {44.0f }, "basecurve",     0},
  { {45.0f }, "filmic",        0},
  { {45.3f }, "sigmoid",       0},
  { {45.5f }, "agx",           0},
  { {46.0f }, "filmicrgb",     0},
  { {36.0f }, "lut3d",         0},
  { {47.0f }, "colisa",        0},
  { {48.0f }, "tonecurve",     0},
  { {49.0f }, "levels",        0},
  { {50.0f }, "shadhi",        0},
  { {51.0f }, "zonesystem",    0},
  { {52.0f }, "globaltonemap", 0},
  { {53.0f }, "relight",       0},
  { {54.0f }, "bilat",         0},
  { {55.0f }, "colorcorrection",0},
  { {56.0f }, "colorcontrast", 0},
  { {57.0f }, "velvia",        0},
  { {58.0f }, "vibrance",      0},
  { {60.0f }, "colorzones",    0},
  { {61.0f }, "bloom",         0},
  { {62.0f }, "colorize",      0},
  { {63.0f }, "lowlight",      0},
  { {64.0f }, "monochrome",    0},
  { {65.0f }, "grain",         0},
  { {66.0f }, "soften",        0},
  { {67.0f }, "splittoning",   0},
  { {68.0f }, "vignette",      0},
  { {69.0f }, "colorreconstruct",0},
  { {70.0f }, "colorout",      0},
  { {71.0f }, "clahe",         0},
  { {72.0f }, "finalscale",    0},
  { {73.0f }, "overexposed",   0},
  { {74.0f }, "rawoverexposed",0},
  { {75.0f }, "dither",        0},
  { {76.0f }, "borders",       0},
  { {77.0f }, "watermark",     0},
  { {78.0f }, "gamma",         0},
  DT_IOP_ORDER_TABLE_END
};

/* v5.0 JPEG — non-linear-input, finalscale before colorout */
static const dt_iop_order_entry_t _v50_jpg_order[] = {
  { { 1.0f }, "rawprepare",    0},
  { { 2.0f }, "invert",        0},
  { { 3.0f }, "temperature",   0},
  { { 3.1f }, "rasterfile",    0},
  { { 4.0f }, "highlights",    0},
  { { 5.0f }, "cacorrect",     0},
  { { 6.0f }, "hotpixels",     0},
  { { 7.0f }, "rawdenoise",    0},
  { { 8.0f }, "demosaic",      0},
  { {28.0f }, "colorin",       0},
  { {28.0f }, "denoiseprofile",0},
  { {28.0f }, "bilateral",     0},
  { {28.0f }, "rotatepixels",  0},
  { {28.0f }, "scalepixels",   0},
  { {28.0f }, "lens",          0},
  { {28.0f }, "cacorrectrgb",  0},
  { {28.0f }, "hazeremoval",   0},
  { {28.0f }, "ashift",        0},
  { {28.0f }, "flip",          0},
  { {28.0f }, "enlargecanvas", 0},
  { {28.0f }, "overlay",       0},
  { {28.0f }, "clipping",      0},
  { {28.0f }, "liquify",       0},
  { {28.0f }, "spots",         0},
  { {28.0f }, "retouch",       0},
  { {28.0f }, "exposure",      0},
  { {28.0f }, "mask_manager",  0},
  { {28.0f }, "tonemap",       0},
  { {28.0f }, "toneequal",     0},
  { {28.0f }, "crop",          0},
  { {28.0f }, "graduatednd",   0},
  { {28.0f }, "profile_gamma", 0},
  { {28.0f }, "equalizer",     0},
  { {28.5f }, "channelmixerrgb",0},
  { {28.5f }, "diffuse",       0},
  { {28.5f }, "censorize",     0},
  { {28.5f }, "negadoctor",    0},
  { {28.5f }, "blurs",         0},
  { {28.5f }, "primaries",     0},
  { {29.0f }, "nlmeans",       0},
  { {30.0f }, "colorchecker",  0},
  { {31.0f }, "defringe",      0},
  { {32.0f }, "atrous",        0},
  { {33.0f }, "lowpass",       0},
  { {34.0f }, "highpass",      0},
  { {35.0f }, "sharpen",       0},
  { {37.0f }, "colortransfer", 0},
  { {38.0f }, "colormapping",  0},
  { {39.0f }, "channelmixer",  0},
  { {40.0f }, "basicadj",      0},
  { {41.0f }, "colorbalance",  0},
  { {41.2f }, "colorequal",    0},
  { {41.5f }, "colorbalancergb",0},
  { {42.0f }, "rgbcurve",      0},
  { {43.0f }, "rgblevels",     0},
  { {44.0f }, "basecurve",     0},
  { {45.0f }, "filmic",        0},
  { {45.3f }, "sigmoid",       0},
  { {45.5f }, "agx",           0},
  { {46.0f }, "filmicrgb",     0},
  { {36.0f }, "lut3d",         0},
  { {47.0f }, "colisa",        0},
  { {48.0f }, "tonecurve",     0},
  { {49.0f }, "levels",        0},
  { {50.0f }, "shadhi",        0},
  { {51.0f }, "zonesystem",    0},
  { {52.0f }, "globaltonemap", 0},
  { {53.0f }, "relight",       0},
  { {54.0f }, "bilat",         0},
  { {55.0f }, "colorcorrection",0},
  { {56.0f }, "colorcontrast", 0},
  { {57.0f }, "velvia",        0},
  { {58.0f }, "vibrance",      0},
  { {60.0f }, "colorzones",    0},
  { {61.0f }, "bloom",         0},
  { {62.0f }, "colorize",      0},
  { {63.0f }, "lowlight",      0},
  { {64.0f }, "monochrome",    0},
  { {65.0f }, "grain",         0},
  { {66.0f }, "soften",        0},
  { {67.0f }, "splittoning",   0},
  { {68.0f }, "vignette",      0},
  { {69.0f }, "colorreconstruct",0},
  { {69.5f }, "finalscale",    0},
  { {70.0f }, "colorout",      0},
  { {71.0f }, "clahe",         0},
  { {73.0f }, "overexposed",   0},
  { {74.0f }, "rawoverexposed",0},
  { {75.0f }, "dither",        0},
  { {76.0f }, "borders",       0},
  { {77.0f }, "watermark",     0},
  { {78.0f }, "gamma",         0},
  DT_IOP_ORDER_TABLE_END
};

/* Index into tables by dt_iop_order_t (DT_IOP_ORDER_CUSTOM = 0 → NULL) */
static const dt_iop_order_entry_t *const _iop_order_tables[DT_IOP_ORDER_LAST] = {
  NULL,              /* DT_IOP_ORDER_CUSTOM  */
  _legacy_order,     /* DT_IOP_ORDER_LEGACY  */
  _v30_order,        /* DT_IOP_ORDER_V30     */
  _v30_jpg_order,    /* DT_IOP_ORDER_V30_JPG */
  _v50_order,        /* DT_IOP_ORDER_V50     */
  _v50_jpg_order,    /* DT_IOP_ORDER_V50_JPG */
};

static const char *const _iop_order_names[DT_IOP_ORDER_LAST] = {
  "custom",
  "legacy",
  "v3.0 RAW",
  "v3.0 JPEG",
  "v5.0 RAW",
  "v5.0 JPEG",
};

/* ── Public: order version names ─────────────────────────────────────────── */

const char *dt_iop_order_string(dt_iop_order_t order)
{
  if(order < 0 || order >= DT_IOP_ORDER_LAST)
    return "???";
  return _iop_order_names[order];
}

/* ── Convert a static table to a heap-allocated linked list ──────────────── */

static dt_iop_order_list_t _table_to_list(const dt_iop_order_entry_t entries[])
{
  dt_iop_order_list_t head = NULL;

  for(int k = 0; entries[k].operation[0]; k++)
  {
    dt_iop_order_list_node_t *node = _new_node(entries[k].operation, entries[k].instance);
    if(!node)
    {
      dt_ioppr_iop_order_list_free(head);
      return NULL;
    }
    /* preserve the original float order for potential legacy migration */
    node->entry.o.iop_order_f = entries[k].o.iop_order_f;
    head = _list_append(head, node);
  }

  _reset_iop_order(head);
  return head;
}

/* ── Public: built-in version lists ─────────────────────────────────────── */

dt_iop_order_list_t dt_ioppr_get_iop_order_list_version(dt_iop_order_t version)
{
  if(version >= DT_IOP_ORDER_LEGACY && version < DT_IOP_ORDER_LAST)
    return _table_to_list(_iop_order_tables[version]);
  return NULL;
}

void dt_ioppr_iop_order_list_free(dt_iop_order_list_t list)
{
  dt_iop_order_list_node_t *l = list;
  while(l)
  {
    dt_iop_order_list_node_t *next = l->next;
    free(l);
    l = next;
  }
}

dt_iop_order_list_t dt_ioppr_iop_order_copy_deep(dt_iop_order_list_t list)
{
  dt_iop_order_list_t head = NULL;

  for(dt_iop_order_list_node_t *l = list; l; l = l->next)
  {
    dt_iop_order_list_node_t *node = calloc(1, sizeof(*node));
    if(!node)
    {
      dt_ioppr_iop_order_list_free(head);
      return NULL;
    }
    node->entry = l->entry;
    head = _list_append(head, node);
  }

  return head;
}

/* ── Public: kind detection ──────────────────────────────────────────────── */

/*
 * Check whether a list matches a static table, ignoring multi-instances
 * of the same module (same as darktable's _check_iop_list_equal).
 */
static bool _check_iop_list_equal(dt_iop_order_list_t list,
                                   const dt_iop_order_entry_t table[])
{
  int k = 0;
  dt_iop_order_list_node_t *l = list;

  while(l)
  {
    if(strcmp(table[k].operation, l->entry.operation) != 0)
      return false;

    /* skip consecutive nodes with the same operation (multi-instances) */
    while(l->next && strcmp(l->next->entry.operation, table[k].operation) == 0)
      l = l->next;

    k++;
    l = l->next;
  }

  return true;
}

dt_iop_order_t dt_ioppr_get_iop_order_list_kind(dt_iop_order_list_t list)
{
  for(dt_iop_order_t v = DT_IOP_ORDER_LEGACY; v < DT_IOP_ORDER_LAST; v++)
  {
    if(_check_iop_list_equal(list, _iop_order_tables[v]))
      return v;
  }
  return DT_IOP_ORDER_CUSTOM;
}

/* ── Public: entry lookup ────────────────────────────────────────────────── */

const dt_iop_order_entry_t *dt_ioppr_get_iop_order_entry(dt_iop_order_list_t list,
                                                          const char *op_name,
                                                          int multi_priority)
{
  for(dt_iop_order_list_node_t *l = list; l; l = l->next)
  {
    if(strcmp(l->entry.operation, op_name) == 0
       && (l->entry.instance == multi_priority || multi_priority == -1))
      return &l->entry;
  }
  return NULL;
}

int dt_ioppr_get_iop_order(dt_iop_order_list_t list,
                           const char *op_name,
                           int multi_priority)
{
  const dt_iop_order_entry_t *e =
    dt_ioppr_get_iop_order_entry(list, op_name, multi_priority);
  if(e)
    return e->o.iop_order;

  fprintf(stderr, "[iop_order] cannot get iop_order for %s instance %d\n",
          op_name, multi_priority);
  return INT_MAX;
}

int dt_ioppr_get_iop_order_last(dt_iop_order_list_t list,
                                const char *op_name)
{
  int iop_order = INT_MIN;
  for(dt_iop_order_list_node_t *l = list; l; l = l->next)
  {
    if(strcmp(l->entry.operation, op_name) == 0)
    {
      if(l->entry.o.iop_order > iop_order)
        iop_order = l->entry.o.iop_order;
    }
  }
  return iop_order;
}

bool dt_ioppr_is_iop_before(dt_iop_order_list_t list,
                             const char *base_operation,
                             const char *operation,
                             int multi_priority)
{
  const int base_order = dt_ioppr_get_iop_order(list, base_operation, -1);
  const int op_order   = dt_ioppr_get_iop_order(list, operation, multi_priority);
  return op_order < base_order;
}

/* ── Public: sort ────────────────────────────────────────────────────────── */

/*
 * Merge-sort a singly-linked list by iop_order.
 * O(n log n), stable.
 */
static dt_iop_order_list_node_t *_merge_sorted(dt_iop_order_list_node_t *a,
                                                dt_iop_order_list_node_t *b)
{
  if(!a) return b;
  if(!b) return a;

  if(a->entry.o.iop_order <= b->entry.o.iop_order)
  {
    a->next = _merge_sorted(a->next, b);
    return a;
  }
  else
  {
    b->next = _merge_sorted(a, b->next);
    return b;
  }
}

static dt_iop_order_list_node_t *_split_list(dt_iop_order_list_node_t *head)
{
  /* Find the midpoint using slow/fast pointers. */
  dt_iop_order_list_node_t *slow = head;
  dt_iop_order_list_node_t *fast = head->next;

  while(fast && fast->next)
  {
    slow = slow->next;
    fast = fast->next->next;
  }

  dt_iop_order_list_node_t *second = slow->next;
  slow->next = NULL;
  return second;
}

static dt_iop_order_list_node_t *_merge_sort(dt_iop_order_list_node_t *head)
{
  if(!head || !head->next)
    return head;

  dt_iop_order_list_node_t *second = _split_list(head);
  head   = _merge_sort(head);
  second = _merge_sort(second);
  return _merge_sorted(head, second);
}

dt_iop_order_list_t dt_ioppr_sort_iop_order_list(dt_iop_order_list_t list)
{
  return _merge_sort(list);
}

/* ── Public: text serialisation ──────────────────────────────────────────── */

char *dt_ioppr_serialize_text_iop_order_list(dt_iop_order_list_t list)
{
  if(!list) return NULL;

  /* Two-pass: compute required buffer size, then fill. */
  size_t total = 0;
  for(dt_iop_order_list_node_t *l = list; l; l = l->next)
    total += strlen(l->entry.operation) + 12 + 1; /* op + comma + int + comma */

  char *text = malloc(total + 1);
  if(!text) return NULL;

  text[0] = '\0';
  char *p = text;
  const dt_iop_order_list_node_t *last = list;
  while(last->next) last = last->next;

  for(dt_iop_order_list_node_t *l = list; l; l = l->next)
  {
    int written = snprintf(p, total - (size_t)(p - text) + 1,
                           "%s,%d%s",
                           l->entry.operation,
                           l->entry.instance,
                           (l == last) ? "" : ",");
    if(written < 0)
    {
      free(text);
      return NULL;
    }
    p += written;
  }

  return text;
}

/* ── Sanity check (same as darktable) ────────────────────────────────────── */

static bool _ioppr_sanity_check(dt_iop_order_list_t list)
{
  if(!list) return false;

  /* First entry must be rawprepare */
  if(strcmp(list->entry.operation, "rawprepare") != 0)
    return false;

  /* Last entry must be gamma */
  const dt_iop_order_list_node_t *last = list;
  while(last->next) last = last->next;
  if(strcmp(last->entry.operation, "gamma") != 0)
    return false;

  return true;
}

dt_iop_order_list_t dt_ioppr_deserialize_text_iop_order_list(const char *buf)
{
  if(!buf || !*buf) return NULL;

  dt_iop_order_list_t head = NULL;

  /* Parse comma-separated tokens: op,instance,op,instance,... */
  char *copy = strdup(buf);
  if(!copy) return NULL;

  char *saveptr = NULL;
  char *tok = strtok_r(copy, ",", &saveptr);

  while(tok)
  {
    /* First token: operation name */
    char op[20];
    _strlcpy(op, tok, sizeof(op));

    /* Second token: instance number */
    tok = strtok_r(NULL, ",", &saveptr);
    if(!tok) goto error;

    int inst = 0;
    sscanf(tok, "%d", &inst);

    dt_iop_order_list_node_t *node = _new_node(op, inst);
    if(!node) goto error;

    head = _list_append(head, node);

    tok = strtok_r(NULL, ",", &saveptr);
  }

  free(copy);

  _reset_iop_order(head);

  if(!_ioppr_sanity_check(head))
    goto error_after_free;

  return head;

error:
  free(copy);
error_after_free:
  fprintf(stderr, "[iop_order] corrupted iop order list: '%.80s'\n", buf);
  dt_ioppr_iop_order_list_free(head);
  return NULL;
}

/* ── Public: binary serialisation ───────────────────────────────────────── */

void *dt_ioppr_serialize_iop_order_list(dt_iop_order_list_t list, size_t *size)
{
  if(!list || !size) return NULL;

  *size = 0;
  for(dt_iop_order_list_node_t *l = list; l; l = l->next)
    *size += strlen(l->entry.operation) + sizeof(int32_t) * 2;

  if(*size == 0) return NULL;

  char *params = malloc(*size);
  if(!params) return NULL;

  int pos = 0;
  for(dt_iop_order_list_node_t *l = list; l; l = l->next)
  {
    const int32_t len = (int32_t)strlen(l->entry.operation);
    memcpy(params + pos, &len, sizeof(int32_t));
    pos += sizeof(int32_t);

    memcpy(params + pos, l->entry.operation, len);
    pos += len;

    const int32_t inst = l->entry.instance;
    memcpy(params + pos, &inst, sizeof(int32_t));
    pos += sizeof(int32_t);
  }

  return params;
}

dt_iop_order_list_t dt_ioppr_deserialize_iop_order_list(const char *buf, size_t size)
{
  if(!buf || size == 0) return NULL;

  dt_iop_order_list_t head = NULL;
  const char *p = buf;

  while(size > 0)
  {
    if(size < sizeof(int32_t)) goto error;

    const int32_t len = *(const int32_t *)p;
    p    += sizeof(int32_t);
    size -= sizeof(int32_t);

    if(len < 0 || len > 20 || (size_t)len > size) goto error;

    char op[21];
    memcpy(op, p, (size_t)len);
    op[len] = '\0';
    p    += len;
    size -= (size_t)len;

    if(size < sizeof(int32_t)) goto error;

    const int32_t inst = *(const int32_t *)p;
    p    += sizeof(int32_t);
    size -= sizeof(int32_t);

    if(inst < 0 || inst > 1000) goto error;

    dt_iop_order_list_node_t *node = _new_node(op, inst);
    if(!node) goto error;

    head = _list_append(head, node);
  }

  _reset_iop_order(head);
  return head;

error:
  fprintf(stderr, "[iop_order] corrupted binary iop order list\n");
  dt_ioppr_iop_order_list_free(head);
  return NULL;
}

/* ── Public: JSON I/O ────────────────────────────────────────────────────── */

bool dt_ioppr_write_iop_order_json(dt_iop_order_list_t list,
                                   dt_iop_order_t kind,
                                   const char *path)
{
  if(!list || !path) return false;

  FILE *f = fopen(path, "w");
  if(!f)
  {
    fprintf(stderr, "[iop_order] cannot open '%s' for writing\n", path);
    return false;
  }

  fprintf(f, "{\n  \"version\": %d,\n  \"order\": [\n", (int)kind);

  for(dt_iop_order_list_node_t *l = list; l; l = l->next)
  {
    fprintf(f, "    { \"op\": \"%s\", \"instance\": %d }%s\n",
            l->entry.operation,
            l->entry.instance,
            l->next ? "," : "");
  }

  fprintf(f, "  ]\n}\n");
  fclose(f);
  return true;
}

/* Minimal hand-written JSON parser — handles only the format written above. */
static char *_json_skip_ws(char *p)
{
  while(p && *p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
    p++;
  return p;
}

static char *_json_expect(char *p, char c)
{
  p = _json_skip_ws(p);
  if(!p || *p != c) return NULL;
  return p + 1;
}

/* Read a JSON string value into buf (up to bufsize-1 chars). Returns pointer
 * after the closing quote, or NULL on error. */
static char *_json_read_string(char *p, char *buf, size_t bufsize)
{
  p = _json_skip_ws(p);
  if(!p || *p != '"') return NULL;
  p++;
  size_t i = 0;
  while(*p && *p != '"')
  {
    if(*p == '\\')
    {
      p++;
      if(!*p) return NULL;
    }
    if(i < bufsize - 1)
      buf[i++] = *p;
    p++;
  }
  buf[i] = '\0';
  if(*p != '"') return NULL;
  return p + 1;
}

/* Read a JSON integer. Returns pointer after last digit, or NULL on error. */
static char *_json_read_int(char *p, int *out)
{
  p = _json_skip_ws(p);
  if(!p) return NULL;
  char *end = NULL;
  long v = strtol(p, &end, 10);
  if(end == p) return NULL;
  *out = (int)v;
  return end;
}

dt_iop_order_list_t dt_ioppr_read_iop_order_json(const char *path,
                                                  dt_iop_order_t *kind_out)
{
  if(!path) return NULL;

  FILE *f = fopen(path, "r");
  if(!f)
  {
    fprintf(stderr, "[iop_order] cannot open '%s' for reading\n", path);
    return NULL;
  }

  /* Read whole file into memory. */
  fseek(f, 0, SEEK_END);
  long fsz = ftell(f);
  fseek(f, 0, SEEK_SET);

  if(fsz <= 0 || fsz > 1024 * 1024)
  {
    fprintf(stderr, "[iop_order] JSON file '%s' size %ld is invalid\n", path, fsz);
    fclose(f);
    return NULL;
  }

  char *buf = malloc((size_t)fsz + 1);
  if(!buf) { fclose(f); return NULL; }
  size_t nr = fread(buf, 1, (size_t)fsz, f);
  fclose(f);
  buf[nr] = '\0';

  dt_iop_order_list_t head = NULL;
  int version = (int)DT_IOP_ORDER_V50;

  char *p = buf;

  /* { */
  p = _json_expect(p, '{');
  if(!p) goto error;

  /* "version": N */
  p = _json_skip_ws(p);
  char key[64];
  p = _json_read_string(p, key, sizeof(key));
  if(!p || strcmp(key, "version") != 0) goto error;
  p = _json_expect(p, ':');
  if(!p) goto error;
  p = _json_read_int(p, &version);
  if(!p) goto error;

  /* , */
  p = _json_expect(p, ',');
  if(!p) goto error;

  /* "order": [ */
  p = _json_skip_ws(p);
  p = _json_read_string(p, key, sizeof(key));
  if(!p || strcmp(key, "order") != 0) goto error;
  p = _json_expect(p, ':');
  if(!p) goto error;
  p = _json_expect(p, '[');
  if(!p) goto error;

  /* array elements */
  p = _json_skip_ws(p);
  while(p && *p == '{')
  {
    p++; /* skip { */

    /* "op": "<name>" */
    p = _json_skip_ws(p);
    p = _json_read_string(p, key, sizeof(key));
    if(!p || strcmp(key, "op") != 0) goto error;
    p = _json_expect(p, ':');
    if(!p) goto error;
    char op[20];
    p = _json_read_string(p, op, sizeof(op));
    if(!p) goto error;

    /* , */
    p = _json_expect(p, ',');
    if(!p) goto error;

    /* "instance": N */
    p = _json_skip_ws(p);
    p = _json_read_string(p, key, sizeof(key));
    if(!p || strcmp(key, "instance") != 0) goto error;
    p = _json_expect(p, ':');
    if(!p) goto error;
    int inst = 0;
    p = _json_read_int(p, &inst);
    if(!p) goto error;

    p = _json_expect(p, '}');
    if(!p) goto error;

    dt_iop_order_list_node_t *node = _new_node(op, inst);
    if(!node) goto error;
    head = _list_append(head, node);

    /* optional comma between elements */
    p = _json_skip_ws(p);
    if(p && *p == ',')
      p++;
    p = _json_skip_ws(p);
  }

  /* ] } */
  p = _json_expect(p, ']');
  if(!p) goto error;

  free(buf);

  _reset_iop_order(head);

  if(kind_out)
    *kind_out = (dt_iop_order_t)version;

  return head;

error:
  fprintf(stderr, "[iop_order] failed to parse JSON file '%s'\n", path);
  free(buf);
  dt_ioppr_iop_order_list_free(head);
  return NULL;
}

/* ── Public: order rules ─────────────────────────────────────────────────── */

void dt_ioppr_iop_order_rules_free(dt_iop_order_rules_t rules)
{
  dt_iop_order_rules_node_t *r = rules;
  while(r)
  {
    dt_iop_order_rules_node_t *next = r->next;
    free(r);
    r = next;
  }
}

dt_iop_order_rules_t dt_ioppr_get_iop_order_rules(void)
{
  static const dt_iop_order_rule_t rule_table[] = {
    { "rawprepare",  "invert"         },
    { "invert",      "temperature"    },
    { "temperature", "highlights"     },
    { "highlights",  "cacorrect"      },
    { "cacorrect",   "hotpixels"      },
    { "hotpixels",   "rawdenoise"     },
    { "rawdenoise",  "demosaic"       },
    { "demosaic",    "colorin"        },
    { "colorin",     "colorout"       },
    { "colorout",    "gamma"          },
    { "flip",        "crop"           },
    { "flip",        "clipping"       },
    { "ashift",      "clipping"       },
    { "colorin",     "channelmixerrgb"},
    { "\0",          "\0"             }  /* sentinel */
  };

  dt_iop_order_rules_t head = NULL;
  dt_iop_order_rules_node_t *tail = NULL;

  for(int i = 0; rule_table[i].op_prev[0]; i++)
  {
    dt_iop_order_rules_node_t *node = calloc(1, sizeof(*node));
    if(!node) break;

    node->rule = rule_table[i];
    node->next = NULL;

    if(!head)
    {
      head = node;
      tail = node;
    }
    else
    {
      tail->next = node;
      tail = node;
    }
  }

  return head;
}
