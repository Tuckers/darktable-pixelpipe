# libdtpipe History JSON Format

Version: 1.0  
Status: Specification

---

## Overview

The libdtpipe history format is a JSON document that captures the complete
non-destructive editing state of a pipeline: which modules are active, what
parameters each module holds, and the processing order. It is the primary
interchange format used by `dtpipe_serialize_history()` and
`dtpipe_load_history()`, and it is the target representation when reading or
writing darktable XMP sidecars (Tasks 5.4–5.5).

Design goals:

- Human-readable and hand-editable in any text editor.
- Self-contained — no external lookup tables required to interpret it.
- Round-trippable to/from darktable XMP sidecars (with documented exceptions).
- Extensible via versioning without breaking existing readers.
- Compact enough to embed in other JSON documents (e.g. export manifests).

---

## Top-level Structure

```json
{
  "version": "1.0",
  "generator": "libdtpipe",

  "source": { ... },
  "settings": { ... },
  "modules": { ... },
  "custom_order": [ ... ],
  "masks": { }
}
```

All top-level keys are optional to a reader except `version`. An absent key
is treated as its default value (see each section below).

---

## `version`

**Type:** string  
**Required:** yes  
**Example:** `"1.0"`

The format version. Currently the only defined value is `"1.0"`.

A reader MUST reject documents whose major version number (the integer before
the first `.`) is greater than the major version the reader was built against.
A reader SHOULD accept documents with a higher minor version and ignore unknown
keys.

Migration rules are described in the [Versioning](#versioning) section.

---

## `generator`

**Type:** string  
**Required:** no  
**Example:** `"libdtpipe"`, `"darktable 5.0"`

Informational. The application that wrote this document. Not interpreted by
readers; useful for debugging.

---

## `source`

**Type:** object  
**Required:** no

Metadata about the source image. Used to detect when a history file is applied
to the wrong image (e.g. different camera body) and for display in the UI.

```json
"source": {
  "filename": "DSCF4379.RAF",
  "hash":     "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
  "camera":   "Fujifilm GFX 50R"
}
```

| Field      | Type   | Description |
|------------|--------|-------------|
| `filename` | string | Basename of the raw file (no path). |
| `hash`     | string | `"sha256:<hex>"` digest of the raw file bytes. Optional — omit if unavailable. Readers must not require it. |
| `camera`   | string | `"<maker> <model>"` from EXIF, for informational display only. |

---

## `settings`

**Type:** object  
**Required:** no

Global pipeline settings that are not tied to any single module.

```json
"settings": {
  "iop_order":       "v5.0",
  "color_workflow":  "scene-referred"
}
```

| Field            | Type   | Values | Default | Description |
|------------------|--------|--------|---------|-------------|
| `iop_order`      | string | `"v1.0"` – `"v5.0"`, `"custom"` | `"v5.0"` | Which built-in IOP ordering to use, or `"custom"` if `custom_order` is present. |
| `color_workflow` | string | `"scene-referred"`, `"display-referred"` | `"scene-referred"` | Informational annotation. Affects default module selection suggestions in the UI but not pipeline behaviour directly. |

### `iop_order` values

The string values map directly to the `dt_iop_order_t` enum in `iop_order.h`:

| String     | Enum constant          | Notes |
|------------|------------------------|-------|
| `"v1.0"`   | `DT_IOP_ORDER_LEGACY`  | darktable ≤ 2.x legacy order |
| `"v3.0"`   | `DT_IOP_ORDER_V30`     | darktable 3.x |
| `"v5.0"`   | `DT_IOP_ORDER_V50`     | darktable 5.x (current default) |
| `"custom"` | `DT_IOP_ORDER_CUSTOM`  | Explicit order provided in `custom_order` |

When reading a history without a `settings` block the reader MUST assume
`"v5.0"` ordering. When writing, the serializer MUST always emit the
`settings.iop_order` field.

---

## `modules`

**Type:** object (map of op-name → module object)  
**Required:** no (empty modules map = all modules at their defaults)

The `modules` object maps each IOP operation name (e.g. `"exposure"`) to its
editing state. Only modules whose parameters differ from their compiled-in
defaults OR whose enabled state differs from the pipeline default need to be
present. A reader applies only the modules listed; all other modules remain at
their defaults.

```json
"modules": {
  "exposure": {
    "enabled": true,
    "version": 7,
    "params": {
      "mode":                      0,
      "black":                     0.002,
      "exposure":                  1.5,
      "deflicker_percentile":      50.0,
      "deflicker_target_level":    -4.0,
      "compensate_exposure_bias":  false,
      "compensate_hilite_pres":    false
    }
  },
  "temperature": {
    "enabled": true,
    "version": 4,
    "params": {
      "red":     2.1,
      "green":   1.0,
      "blue":    1.6,
      "various": 1.0,
      "preset":  0
    }
  }
}
```

### Module object fields

| Field     | Type    | Required | Description |
|-----------|---------|----------|-------------|
| `enabled` | boolean | no       | Whether the module is active in the pipeline. Defaults to the pipeline default for that module (see `pipe/create.c`). |
| `version` | integer | no       | Params struct version. Used to detect stale presets from an older darktable version. Readers SHOULD warn and skip if the version does not match the compiled-in module version; they MUST NOT silently apply mismatched params. |
| `params`  | object  | no       | Map of parameter name → value. Only parameters listed are changed; omitted parameters stay at their defaults. |

### Parameter value encoding

| `dt_param_type_t` | JSON type | Notes |
|--------------------|-----------|-------|
| `DT_PARAM_FLOAT`   | number    | IEEE 754 single precision. Serialized as a JSON number; readers use `strtof()`. Special values (`NaN`, `Inf`) are not allowed. |
| `DT_PARAM_INT`     | number    | 32-bit signed integer. Must be a JSON integer (no decimal point). |
| `DT_PARAM_UINT32`  | number    | 32-bit unsigned integer. Must be a JSON integer ≥ 0. |
| `DT_PARAM_BOOL`    | boolean   | JSON `true` / `false`. When read from XMP the integer value `1`/`0` is also accepted. |

Parameters that are `char[]` fields in the params struct (e.g. `colorin.filename`,
`colorout.filename`) are NOT currently exposed through the JSON params map
because they are set by the color management subsystem, not by the user. They
are represented as string fields at the top level of the module object:

```json
"colorin": {
  "enabled": true,
  "version": 7,
  "input_profile": "embedded",
  "work_profile":  "linear_rec2020_rgb",
  "params": {
    "type":          0,
    "intent":        0,
    "normalize":     0,
    "blue_mapping":  false,
    "type_work":     0
  }
}
```

| String field      | Module    | Description |
|-------------------|-----------|-------------|
| `input_profile`   | `colorin` | ICC profile name or `"embedded"` to use the camera's embedded profile. |
| `work_profile`    | `colorin` | Working color space name (e.g. `"linear_rec2020_rgb"`). |
| `output_profile`  | `colorout`| Output ICC profile name (e.g. `"sRGB"`). |

### Covered modules

The following modules have parameter descriptor tables in `params.c` and can
be fully round-tripped through the JSON history:

| Module        | Version | Tier |
|---------------|---------|------|
| `rawprepare`  | 2       | 1    |
| `temperature` | 4       | 1    |
| `highlights`  | 4       | 1    |
| `demosaic`    | 6       | 1    |
| `exposure`    | 7       | 1    |
| `colorin`     | 7       | 1    |
| `colorout`    | 5       | 1    |
| `sharpen`     | 1       | 2    |

Modules not in this table are passed through at their defaults. A module
object for an unknown module name is stored verbatim by the serializer and
emitted unchanged on re-serialization, so round-trip fidelity is preserved
for future modules even before their descriptor tables are implemented.

---

## `custom_order`

**Type:** array of strings  
**Required:** only when `settings.iop_order == "custom"`

When present, specifies the explicit pipeline processing order as an ordered
list of operation names. The pipeline processes modules from index 0 to the
last index.

```json
"custom_order": [
  "rawprepare",
  "temperature",
  "highlights",
  "demosaic",
  "colorin",
  "exposure",
  "sharpen",
  "colorout"
]
```

Rules:

- Every module that appears in `modules` with `"enabled": true` SHOULD appear
  in `custom_order`. Modules not listed are appended at the end in their
  default v5.0 position.
- Duplicate entries are an error; readers MUST reject such documents.
- When `settings.iop_order` is any value other than `"custom"`, this field is
  ignored by readers (it may still be present, e.g. as a comment aide).

---

## `masks`

**Type:** object  
**Required:** no  
**Status:** Reserved for Phase 5+ mask support. Currently must be an empty
object `{}` or absent. Readers MUST ignore unknown keys within `masks`.

---

## Complete Example

```json
{
  "version":   "1.0",
  "generator": "libdtpipe",

  "source": {
    "filename": "DSCF4379.RAF",
    "camera":   "Fujifilm GFX 50R"
  },

  "settings": {
    "iop_order":      "v5.0",
    "color_workflow": "scene-referred"
  },

  "modules": {
    "rawprepare": {
      "enabled": true,
      "version": 2,
      "params": {
        "left":              0,
        "top":               0,
        "right":             0,
        "bottom":            0,
        "raw_white_point":   16383,
        "flat_field":        0
      }
    },
    "temperature": {
      "enabled": true,
      "version": 4,
      "params": {
        "red":     2.1,
        "green":   1.0,
        "blue":    1.6,
        "various": 1.0,
        "preset":  0
      }
    },
    "highlights": {
      "enabled": true,
      "version": 4,
      "params": {
        "mode":        0,
        "blendL":      1.0,
        "blendC":      0.0,
        "strength":    1.0,
        "clip":        1.0,
        "noise_level": 0.0,
        "iterations":  1,
        "scales":      5,
        "candidating": 0.5,
        "combine":     1.0,
        "recovery":    0,
        "solid_color": 0.0
      }
    },
    "demosaic": {
      "enabled": true,
      "version": 6,
      "params": {
        "green_eq":           0,
        "median_thrs":        0.0,
        "color_smoothing":    0,
        "demosaicing_method": 0,
        "lmmse_refine":       1,
        "dual_thrs":          0.2,
        "cs_radius":          0.5,
        "cs_thrs":            0.5,
        "cs_boost":           0.0,
        "cs_iter":            1,
        "cs_center":          0.5,
        "cs_enabled":         false
      }
    },
    "exposure": {
      "enabled": true,
      "version": 7,
      "params": {
        "mode":                     0,
        "black":                    0.002,
        "exposure":                 0.75,
        "deflicker_percentile":     50.0,
        "deflicker_target_level":   -4.0,
        "compensate_exposure_bias": false,
        "compensate_hilite_pres":   false
      }
    },
    "colorin": {
      "enabled": true,
      "version": 7,
      "input_profile": "embedded",
      "work_profile":  "linear_rec2020_rgb",
      "params": {
        "type":         0,
        "intent":       0,
        "normalize":    0,
        "blue_mapping": false,
        "type_work":    0
      }
    },
    "colorout": {
      "enabled": true,
      "version": 5,
      "output_profile": "sRGB",
      "params": {
        "type":   1,
        "intent": 0
      }
    }
  },

  "masks": {}
}
```

---

## XMP Round-trip Notes

darktable stores history in XMP sidecar files under the `Xmp.darktable.*`
namespace. The mapping to this JSON format is mostly 1-to-1, with these
caveats:

| Aspect | XMP representation | JSON representation | Loss? |
|--------|-------------------|---------------------|-------|
| Module params | Base64-encoded binary blob (the raw params struct) | Named fields via descriptor table | Params not in descriptor table are lost on XMP→JSON→XMP round-trip |
| IOP order | Comma-separated string in `Xmp.darktable.iop_order` | `settings.iop_order` + optional `custom_order` | None |
| Module enabled state | `Xmp.darktable.history_modversion`, `enabled` flag | `modules.<op>.enabled` | None |
| Masks | `Xmp.darktable.masks_*` namespace | `masks` (reserved, empty) | Masks are dropped until Phase 5+ |
| Multi-instance modules | op name + `_<n>` suffix in XMP | Not yet supported | Second+ instances are dropped |

Task 5.4 (XMP reading) and 5.5 (XMP writing) will handle the binary blob
decoding/encoding. The descriptor tables in `params.c` provide the struct
layout needed to decode blobs field-by-field.

---

## Versioning

### Format version

The `version` string follows `<major>.<minor>` semantics:

- **Major bump** — breaking change. Old readers MUST reject new-major documents.
  Example: removing or renaming a required field.
- **Minor bump** — additive change. Old readers MUST ignore unknown fields.
  Example: adding the `masks` section.

### Module param version

The per-module `version` integer maps to the darktable `version()` return
value in each IOP module's C source. When a params struct changes layout
between darktable releases the version integer is incremented. The serializer
writes the current compiled-in version; the deserializer rejects a mismatched
version with `DTPIPE_ERR_FORMAT`.

Migration functions (one per version increment per module) may be registered
in a future `params_migrate.c` — the infrastructure is reserved but not
implemented in Phase 5.

---

## Implementation Notes

- JSON is generated without a third-party library. The serializer in
  `src/history/serialize.c` (Task 5.2) writes JSON directly via `fprintf()`
  calls. This avoids a dependency on cJSON/jansson/yyjson while keeping the
  output size small.
- Parsing is handled by a minimal recursive-descent parser in
  `src/history/deserialize.c` (Task 5.3). It handles only the subset of JSON
  defined above; it does not support JSON comments (standard JSON does not
  either).
- Both the serializer and deserializer are tested by
  `tests/test_history_roundtrip.c` (added in Task 5.2).
