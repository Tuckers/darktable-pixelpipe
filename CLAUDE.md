# darktable Pixelpipe Extraction — Project Summary

## Goal

Extract darktable's image processing pipeline into a standalone C library (`libdtpipe`), then wrap it with a Node.js addon for use in an Electron-based RAW photo editor.

**End result:**
- `libdtpipe` — headless C library for RAW processing
- `@app/dtpipe` — Node.js native addon wrapping the library
- Electron app with custom UI using the processing backend

---

## Repository Layout

```
libdtpipe/
├── CMakeLists.txt
├── include/
│   └── dtpipe.h              # Public API (opaque handles, C89-compatible)
├── src/
│   ├── CMakeLists.txt
│   ├── dtpipe_internal.h     # All internal types (dt_image_t, dt_dev_pixelpipe_t, …)
│   ├── init.c                # dtpipe_init(), dtpipe_cleanup(), IOP registry
│   ├── common/
│   │   ├── iop_order.h/.c    # IOP execution order lists (v5.0, etc.)
│   ├── pipe/
│   │   ├── pixelpipe.h/.c    # Core pipeline engine (processing loop, nodes)
│   │   ├── create.h/.c       # dtpipe_create(), dtpipe_free(), struct dt_pipe_s
│   │   ├── params.h          # dt_param_desc_t, descriptor table types
│   │   └── params.c          # dtpipe_set/get_param_float/int, dtpipe_enable_module
│   └── imageio/
│       └── load.cc           # dtpipe_load_raw() via rawspeed + exiv2
└── tests/
    ├── test_pipeline_process.c   # Task 3.5: pixelpipe engine tests
    ├── test_pipeline_create.c    # Task 4.3: public API create/free tests
    ├── test_pipeline_params.c    # Task 4.4: public API parameter access tests
    └── test_params_unit.c        # Task 4.4: internal descriptor + buffer round-trip
```

---

## Build

Two build trees exist:

| Directory | Type | Use |
|---|---|---|
| `libdtpipe/build` | ASan+UBSan | Development / CI |
| `libdtpipe/build-release` | Release | Fast manual testing |

```bash
# Configure release build
cmake -B libdtpipe/build-release -S libdtpipe -DCMAKE_BUILD_TYPE=Release -DDTPIPE_SANITIZE=OFF

# Build
cmake --build libdtpipe/build-release --target dtpipe

# Run a test (needs cameras.xml in share/)
DYLD_LIBRARY_PATH=src ./tests/test_pipeline_create ../../test-image/DSCF4379.RAF
```

The `share/` directory (containing `rawspeed/cameras.xml`) must be present next to the test binary. Copy it from the sanitized build if missing:
```bash
cp -r libdtpipe/build/share libdtpipe/build-release/
```

---

## Phase Progress

### Phase 0: Analysis & Discovery — ✅ Complete
- 0.1 Map IOP Dependencies
- 0.2 Identify Core vs Optional Modules
- 0.3 Map common/ Dependencies
- 0.4 Extract Parameter Schemas

### Phase 1: Build System Bootstrap — ✅ Complete
- 1.1 Create Project Scaffold
- 1.2 Write Root CMakeLists.txt
- 1.3 Integrate rawspeed (via FetchContent, static link)

### Phase 2: Strip GUI from IOPs — ✅ Complete
- 2.1 Create IOP Stripping Script
- 2.2 Strip Template Module (exposure.c)
- 2.3 Batch Strip Essential Modules (Tier 1)
- 2.4 Batch Strip Core Creative Modules (Tier 2)

### Phase 3: Core Infrastructure — ✅ Complete
- 3.1 Write Minimal darktable.h → `dtpipe_internal.h`
- 3.2 Implement dtpipe_init()
- 3.3 Port iop_order.c
- 3.4 Port pixelpipe_hb.c Part 1 (structures)
- 3.5 Port pixelpipe_hb.c Part 2 (processing loop)

### Phase 4: Public API Implementation — ✅ Complete
- 4.1 Define Public Header (`include/dtpipe.h`) — ✅ Complete
- 4.2 Implement Image Loading (`imageio/load.cc`) — ✅ Complete
- 4.3 Implement Pipeline Creation (`pipe/create.c`) — ✅ Complete
- 4.4 Implement Parameter Access (`pipe/params.h`, `pipe/params.c`) — ✅ Complete
- 4.5 Implement Render (`pipe/render.c`) — ✅ Complete
- 4.6 Implement Export (`imageio/export.cc`) — ✅ Complete

### Phase 5: History Serialization — ⬜ Not Started
- 5.1–5.5: JSON history format, XMP read/write

### Phase 6: Node.js Addon — ⬜ Not Started
- 6.1–6.2: N-API addon scaffold, wrapping image loading + pipeline

---

## Key Architectural Decisions

### `dtpipe_internal.h`
Single header providing all internal types needed by extracted IOP modules.
Replaces darktable's scattered headers with a self-contained, GLib-free equivalent.
No GTK, no SQLite, no Lua — just C11 standard headers + pthread.

### `dt_pipe_t` (struct dt_pipe_s)
Opaque public handle defined in `pipe/create.h`. Wraps:
- `dt_dev_pixelpipe_t pipe` — the internal engine
- `dt_image_t *img` — borrowed reference to source image
- `_module_node_t *modules` — owned ordered list of `dt_iop_module_t` instances
- `float *input_buf` — float-RGBA input, allocated lazily at render time

### IOP module registration
`init.c` maintains a `_so_node_t*` list in `darktable.iop`. Currently empty
(Phase 2 stripped modules are not yet compiled in — to be wired up as Phase 4
rendering is completed). Pipeline creation works correctly with zero modules
(passes input through unchanged).

### Default enabled modules
When no XMP/history is loaded, these modules are enabled by default:
`rawprepare`, `demosaic`, `colorin`, `exposure`, `colorout`

### GLib replacement
GList and related types are replaced throughout with plain C singly-linked lists.
The `void *` fields on `dt_dev_pixelpipe_t` (`iop`, `nodes`, `iop_order_list`)
store internal list head pointers, cast at use sites.

### Colorspace transforms
`dt_ioppr_transform_image_colorspace()` is a passthrough stub in
`dtpipe_internal.h`. Full ICC-based transforms are deferred to Phase 4+ color
management work.

### Pipeline cache
`dt_dev_pixelpipe_cache_*` functions are stubs that always return a cache miss.
A real LRU cache can be added later once correctness is verified.

### Parameter descriptor tables (`pipe/params.h/.c`)
Each IOP's params struct is described by a static `dt_param_desc_t[]` array
mapping field names to `(offsetof, type, size, soft min/max)`. The master
lookup table in `params.c` covers 8 modules (Tier 1 + key Tier 2):
`exposure`, `temperature`, `rawprepare`, `demosaic`, `colorin`, `colorout`,
`highlights`, `sharpen`. Offsets are computed via `offsetof()` on concrete
local struct definitions that mirror the canonical darktable params structs.
To add a new module: define a `_params_<op>[]` array and add a row to
`_module_param_tables[]`. The `dtpipe_lookup_param()` helper is also used
by the future history serializer (Phase 5).

---

## Test Image

`test-image/DSCF4379.RAF` — Fuji GFX 50R RAF file used for integration tests.
The GFX 50R uses a standard Bayer CFA sensor (not X-Trans).
Not committed to the repo (removed in commit `be32f35`). Must be present locally.

---

## IOP Module Tiers (from Phase 0 analysis)

**Tier 1 — Essential** (RAW → RGB pipeline):
`rawprepare`, `demosaic`, `colorin`, `colorout`, `temperature`, `exposure`

**Tier 2 — Core Creative** (most-used editing):
`highlights`, `shadhi`, `levels`, `curves`, `tonecurve`, `colorbalance`,
`colorbalancergb`, `filmic`, `filmicrgb`, `sigmoid`, `sharpen`, `denoiseprofile`,
`lens`, `rotate`, `crop`, `clipping`

**Tier 3 — Specialized** (nice to have, add later)

**Tier 4 — Skip** (deprecated or rarely used)
