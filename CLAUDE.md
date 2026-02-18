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
node/
├── package.json              # @app/dtpipe, node-addon-api dep
├── binding.gyp               # Links against libdtpipe.dylib
├── src/
│   └── addon.cc              # N-API addon (empty scaffold → Task 6.2+)
├── lib/
│   └── index.js              # JS wrapper (re-exports native)
├── types/
│   └── index.d.ts            # TypeScript declarations
└── test/
    └── test.js               # Addon load verification

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
│   ├── history/
│   │   ├── serialize.h/.c    # dtpipe_serialize_history() → JSON string
│   │   ├── deserialize.h/.c  # dtpipe_load_history() from JSON
│   │   ├── xmp_read.h/.cc    # dtpipe_load_xmp() via pugixml
│   │   └── xmp_write.h/.cc   # dtpipe_save_xmp() via pugixml
│   └── imageio/
│       └── load.cc           # dtpipe_load_raw() via rawspeed + exiv2
└── tests/
    ├── test_pipeline_process.c   # Task 3.5: pixelpipe engine tests
    ├── test_pipeline_create.c    # Task 4.3: public API create/free tests
    ├── test_pipeline_params.c    # Task 4.4: public API parameter access tests
    ├── test_params_unit.c        # Task 4.4: internal descriptor + buffer round-trip
    ├── test_history_roundtrip.c  # Task 5.2: JSON serialize/deserialize round-trip
    ├── test_history_deserialize.c # Task 5.3: JSON deserialization tests
    ├── test_xmp_read.c           # Task 5.4: XMP reading tests
    ├── test_xmp_write.c          # Task 5.5: XMP writing tests
    ├── test_main.c               # Task 7.1: Unified test harness (all suites)
    ├── gen_reference.c           # Task 7.2: Reference render generator (run once)
    ├── benchmark_performance.c   # Performance benchmark (all pipeline stages)
    └── reference/                # Task 7.2: Ground-truth renders (3 presets × PNG+JSON + metadata.txt)
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

### Phase 5: History Serialization — ✅ Complete
- 5.1 Design History JSON Format (`docs/history-format.md`) — ✅ Complete
- 5.2 Implement History Serialization (`src/history/serialize.c`) — ✅ Complete
- 5.3 Implement History Deserialization (`src/history/deserialize.c`) — ✅ Complete
- 5.4 Implement XMP Reading (`src/history/xmp_read.cc`) — ✅ Complete
- 5.5 Implement XMP Writing (`src/history/xmp_write.cc`) — ✅ Complete

### Phase 6: Node.js Addon — ✅ Complete
- 6.1 Create Addon Scaffold (`node/` directory, binding.gyp, empty addon.cc) — ✅ Complete
- 6.2 Wrap Image Loading (`Image` class, `loadRaw()`) — ✅ Complete
- 6.3 Wrap Pipeline Operations (`Pipeline` class, `createPipeline()`) — ✅ Complete
- 6.4 Wrap Render (`Pipeline.render()`, `Pipeline.renderRegion()`, `RenderResult` class) — ✅ Complete
- 6.5 Wrap Export (`Pipeline.exportJpeg()`, `Pipeline.exportPng()`, `Pipeline.exportTiff()`, history/XMP methods) — ✅ Complete
- 6.6 TypeScript Declarations (`node/types/index.d.ts`) — ✅ Complete

### Phase 7: Testing — ✅ Complete
- 7.1 Create C Test Harness (`tests/test_main.c`) — ✅ Complete
- 7.2 Create Reference Renders — ✅ Complete
- 7.3 Implement Regression Tests — ✅ Complete
- 7.4 Write Node.js Tests — ✅ Complete

### Phase 8: Wire Up Real IOP Processing — 🔄 In Progress
- 8.1 Extend Pipeline Engine for Real Modules — ✅ Complete
- 8.2 Add GLib/Darktable Compatibility Layer — ✅ Complete
- 8.3 Port IOP Math and Utility Helpers — ✅ Complete
- 8.4 Port exposure.c (Template Module) — ✅ Complete
- 8.5 Port rawprepare.c — ✅ Complete
- 8.6 Port temperature.c — ✅ Complete
- 8.7 Port demosaic.c (Core Only) — ✅ Complete

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

### OpenMP on macOS
Apple Clang doesn't ship OpenMP. The CMake build auto-detects Homebrew's `libomp`
(`brew install libomp`) via `brew --prefix libomp` and sets `-Xclang -fopenmp`
flags. The `DT_OMP_FOR()` / `DT_OMP_FOR_SIMD()` macros use `DT_OMP_PRAGMA()`
(which stringifies via `_Pragma(#__VA_ARGS__)`) to produce a single string literal,
avoiding C++ `_Pragma` concatenation issues. `default(shared)` is used instead of
`default(firstprivate)` (which requires OpenMP 5.1, unsupported by Homebrew libomp).
Both `OpenMP::OpenMP_C` and `OpenMP::OpenMP_CXX` are linked so `.cc` files
including `dtpipe_internal.h` also see `_OPENMP` defined. After a clean
reconfigure, both `libdtpipe` and rawspeed build with OpenMP threading.

### GLib replacement
GList and related types are replaced throughout with plain C singly-linked lists.
The `void *` fields on `dt_dev_pixelpipe_t` (`iop`, `nodes`, `iop_order_list`)
store internal list head pointers, cast at use sites.

### Colorspace transforms
`dt_ioppr_transform_image_colorspace()` is a passthrough stub in
`dtpipe_internal.h`. Full ICC-based transforms are deferred to Phase 4+ color
management work.

### Pipeline base-case buffer descriptor (`pipe/pixelpipe.c`)
The `_process_rec()` base case (no more nodes) now explicitly sets the output
format descriptor to `{channels=4, datatype=TYPE_FLOAT, cst=IOP_CS_RGB}` before
computing `bpp` and `bufsize`. Without this, when enabled modules have no process
function, intermediate recursive levels pass zero-initialized `_input_format`
structs as `out_format`, leading to `bpp=0`, a zero-size `backbuf` allocation,
and a buffer overread in `_float_to_u8_rgba`.

### Pipeline cache
`dt_dev_pixelpipe_cache_*` functions are stubs that always return a cache miss.
A real LRU cache can be added later once correctness is verified.

### History deserializer (`history/deserialize.c`)
Minimal recursive-descent JSON parser — no third-party library. Two-phase:
Phase 1 walks the top-level document, dispatching the `"modules"` object.
Phase 2 applies each module's `"enabled"`, `"version"`, and `"params"` keys
directly to the live pipeline via `dtpipe_find_module()` + `memcpy` into the
params buffer (guided by descriptor offsets). Error policy: unknown modules
and unknown params warn and skip; malformed JSON returns `DTPIPE_ERR_FORMAT`;
missing `"version"` key is rejected. Tests in `tests/test_history_deserialize.c`.

### XMP writer (`history/xmp_write.cc`)
Uses pugixml to build and write darktable-compatible XMP sidecar files.
Structure: `x:xmpmeta` → `rdf:RDF` → `rdf:Description` (with `darktable:history_end`) → `darktable:history` → `rdf:Seq` of `rdf:li` entries.
Each `rdf:li` carries: `darktable:num`, `operation`, `enabled`, `modversion` (always 0), `params` (plain lowercase hex of raw params bytes), `multi_priority` (always 0), `multi_name` (always "").
Plain hex encoding is used (simpler than gz-base64; darktable reads both).
Tests in `tests/test_xmp_write.c`.

### XMP reader (`history/xmp_read.cc`)
Uses pugixml to parse darktable XMP sidecar files. Two params encodings exist:
- **Plain hex** — lowercase hex string of the raw packed struct bytes.
- **gz-encoded** — `gz` + 2 hex chars (informational, ignored) + standard base64 of a zlib-compressed stream. Decode: skip 4 chars, base64-decode remainder, `uncompress()`.
History entries are deduplicated per operation name by keeping the one with the highest `darktable:num` that is `< darktable:history_end`. Multi-instance modules (`multi_priority > 0`) are skipped. Once decoded, the raw struct bytes are applied field-by-field via the param descriptor table (same offsets as the darktable binary). Modules without a descriptor table get a raw `memcpy`. Tests in `tests/test_xmp_read.c`.

### Stub SO registrations (`init.c`)
The `_iop_registry[]` in `init.c` now contains stub entries for all 8 modules that have descriptor tables (`rawprepare`, `demosaic`, `colorin`, `colorout`, `exposure`, `temperature`, `highlights`, `sharpen`). These stubs set `process_plain = NULL` — the pixelpipe engine skips nodes with no process function and logs a warning. This allows `dtpipe_create()` to build module instances (and allocate params buffers) for all descriptor-table modules without requiring compiled IOP code. When real IOP source is compiled in, the stub entry is replaced with the actual `init_global` function that sets `process_plain`.

### Params buffer allocation (`pipe/create.c` + `pipe/params.c`)
`_build_module_list()` now calls `dtpipe_params_struct_size(op)` (added to `params.c`) after creating each module instance. If the op has a descriptor table, a zero-initialised params buffer of `max(offset + size)` bytes is allocated for both `m->params` and `m->default_params`. Modules without a descriptor table leave `m->params = NULL` and `params_size = 0` — `dtpipe_set_param_float` returns `DTPIPE_ERR_NOT_FOUND` for them.

### Render wrapping (`node/src/addon.cc` — Task 6.4)
`Pipeline.render(scale)` and `Pipeline.renderRegion(x,y,w,h,scale)` return `Promise<RenderResult>` using `Napi::AsyncWorker` subclasses (`RenderWorker`, `RenderRegionWorker`) that call `dtpipe_render()` / `dtpipe_render_region()` off the main thread. `RenderResult` exposes `buffer` (an `ArrayBuffer` with external data and a free-finalizer), `width`, `height`, and `dispose()`. The pixel data is packed tightly (width×4 bytes per row, no padding) into a malloc'd buffer copied from the render result, which is freed immediately after the copy. `Napi::SharedArrayBuffer` is not available as a C++ wrapper in node-addon-api v8; `ArrayBuffer` is used instead (also transferable via `postMessage`). Note: the first render call on a large RAW is slow (~10s for 50MP without OpenMP) because `dtpipe_ensure_input_buf` builds the full-resolution float-RGBA input buffer single-threaded.

### Export and history wrapping (`node/src/addon.cc` — Task 6.5)
`Pipeline.exportJpeg(path, quality?)`, `Pipeline.exportPng(path)`, and `Pipeline.exportTiff(path, bits?)` return `Promise<void>` via `ExportJpegWorker`, `ExportPngWorker`, `ExportTiffWorker` (`Napi::AsyncWorker` subclasses). Each captures its parameters in the constructor and calls the corresponding `dtpipe_export_*` function off the main thread. Default quality for JPEG is 90; default bits for TIFF is 16. Validation (quality 1–100, bits in {8,16,32}) is done synchronously before queuing. Note: full-resolution export of a large RAW (e.g. 57MP) is slow (~30–60s single-threaded) because the render step rebuilds the full float-RGBA buffer.

History/XMP methods are synchronous (fast): `serializeHistory()` returns a JSON string (caller-owned from `dtpipe_serialize_history`; freed after creating the Napi::String), `loadHistory(json)` applies a JSON history, `loadXmp(path)` reads a darktable XMP sidecar, `saveXmp(path)` writes one. All throw a JavaScript `Error` on failure.

### TypeScript declarations (`node/types/index.d.ts` — Task 6.6)
Full `export declare` declarations for `Image`, `Pipeline`, `RenderResult`, `loadRaw()`, and `createPipeline()`. Uses `ArrayBuffer` (not `SharedArrayBuffer`) for `RenderResult.buffer` to match the actual `Napi::ArrayBuffer` implementation. The `package.json` `"types"` field points to this file. Verified with `tsc --strict --noEmit` via a `tsconfig.json` with `paths: { "@app/dtpipe": ["types/index.d.ts"] }`.

### Unified test harness (`tests/test_main.c` — Task 7.1)
Single-binary test runner covering all six API surface areas: `test_init`, `test_load`, `test_pipeline`, `test_render`, `test_export`, `test_history`. Uses a minimal inline `CHECK`/`CHECK_EQ_INT`/`SKIP` macro framework — no third-party test library. 99 checks, 0 failures against the test RAF. Registered in CMake as `test_main` with a 300s timeout (export tests are slow at full resolution). Two important test design notes:
- `dtpipe_init()` uses `pthread_once` internally, so a second call returns `DTPIPE_OK` (not `DTPIPE_ERR_ALREADY_INIT`); the test accepts either.
- With all IOP modules having stub `process_plain=NULL`, rendered pixel alpha is 0 (raw sensor passthrough); the render tests verify dimensions and no-crash rather than exact pixel values.

### Regression tests (`tests/test_regression.c` — Task 7.3)
Three-preset regression harness. For each preset (a/b/c) it: loads the preset history from `tests/reference/<name>.json`, calls `dtpipe_export_png` to a temp file in `/tmp/`, then loads both the exported PNG and the reference PNG via libpng and compares pixel-by-pixel (8-bit RGBA, RGB channels only, alpha ignored). Max allowed per-channel diff is 1 (rounding), mean warn threshold is 0.5. On failure a 3×-amplified diff PNG is written to `/tmp/dtpipe_diff_<name>.png`. Registered as ctest `regression` with a 300s timeout; currently passes in ~12s (27 checks, 0 failures). Note: since all IOPs have stub `process_plain=NULL`, all three renders are currently identical (raw passthrough), so pixel diffs are exactly 0. Linked against `dtpipe PNG::PNG m`.

### Reference renders (`tests/reference/` — Task 7.2)
Three preset renders generated by `tests/gen_reference.c` (run once to establish ground truth):
- `preset_a` — exposure +1.0, sharpen enabled
- `preset_b` — exposure -0.5, sharpen enabled
- `preset_c` — exposure  0.0, sharpen disabled

Each preset produces a `<name>.png` (full-resolution via `dtpipe_export_png`) and a `<name>.json` (serialized history). A `metadata.txt` captures camera info, dimensions, render scale, and module list. Generator is built as `gen_reference` and run via `cmake --build . --target generate_reference`. Since all IOP modules have stub `process_plain=NULL`, all three PNGs are currently identical (raw sensor passthrough); they become differentiated once real IOP code is compiled in.

### Node.js TypeScript tests (`node/test/test.cts` — Task 7.4)
47-test suite covering all 6 addon API surface areas using `node:test` (built-in, Node ≥ 18) and `node:assert/strict`. Written as a `.cts` file (CommonJS TypeScript) so `require()` and `__dirname` are available at runtime without `import.meta`. Type-checked separately via `npm run typecheck` (uses `tsconfig.test.json` with `module: CommonJS`, `types: ["node"]`). Run via `npm run test:ts` which invokes `node --experimental-strip-types test/test.cts`. Key design notes:
- Explicit type annotations (`const assert: typeof import("node:assert/strict") = require(...)`) are required for TypeScript assertion-function narrowing (TS2775); `require(...) as T` is not sufficient.
- `before()`/`after()` hooks in each `describe` block set up and tear down a shared `img` + `pipe` pair, keeping test isolation without re-loading the RAF for every test.
- Export tests create a `mkdtemp` temp directory and remove it in `after()`.
- The RAF path resolves via `path.resolve(__dirname, "../../test-image/DSCF4379.RAF")` (two levels up from `node/test/`).

### Pipeline engine lifecycle support (`dtpipe_internal.h`, `pipe/create.c`, `pipe/pixelpipe.c` — Task 8.1)
`dt_iop_module_so_t` now carries all lifecycle and routing callbacks: `init`, `init_pipe`, `cleanup_pipe`, `commit_params`, `input_colorspace`, `output_colorspace`, `output_format`, `modify_roi_in`, `modify_roi_out`. These are populated by a module's `init_global()` registration function and copied into `dt_iop_module_t` instances by `_build_module_list()` in `create.c`.

Key engine changes:
- `_build_module_list()` copies all new function pointers from SO → module instance, then calls `so->init(module)` (if set) to populate default params.
- `dtpipe_create()` calls `module->init_pipe(module, pipe, piece)` (if set) after each `dt_dev_pixelpipe_add_node()`, allocating `piece->data`.
- `dt_dev_pixelpipe_cleanup_nodes()` calls `piece->module->cleanup_pipe(module, pipe, piece)` (if set) before freeing `piece->data` — real IOP cleanup runs first, then a safety `free()` handles any remaining pointer (e.g. stub modules that allocate nothing).
- `_process_rec()` calls `module->commit_params(module, module->params, pipe, piece)` (if set) immediately before `_process_on_cpu()`, ensuring `piece->data` reflects current user params before processing.

All 8 stub modules (NULL function pointers) continue working. All 13 tests pass after these changes.

### IOP math/utility header (`libdtpipe/src/iop/iop_math.h` — Task 8.3)
A self-contained header that all Tier 1 IOP source files include instead of darktable's scattered math headers. Ported from `src/develop/imageop_math.h`, `src/common/imagebuf.h`, `src/common/math.h`, and `src/common/dttypes.h`.

Key contents:
- **Bayer/X-Trans pattern** — `FC()`, `FCxtrans()`, `FCNxtrans()`, `fcol()`
- **Exponential fit** — `dt_iop_estimate_exp()`, `dt_iop_eval_exp()`
- **Fast math** — `dt_fast_expf()`, `dt_fast_mexp2f()`, `fastlog2()`, `fastlog()`, `dt_log2f()`, `Log2()`, `Log2Thres()`
- **Scalar helpers** — `sqf()`, `sqrf()`, `ceil_fast()`, `interpolatef()`, `feqf()`, angle conversion, `dt_fast_hypotf()`
- **Channel-array ops** — `max3f()`, `min3f()`, `max4f()`, `dt_iop_get_processed_maximum/minimum()`
- **Alpha copy** — `dt_iop_alpha_copy()`
- **3×3 matrix ops** — `mat3mulv()`, `mat3mul()`, `mat3SSEmul()`, `mul_mat_vec_2()`, `scalar_product()`, `dot_product()`
- **Aligned-pixel vector ops** — `euclidean_norm()`, `downscale_vector()`, `upscale_vector()`, `dt_vector_add/sub/mul/div/clip/clipneg()`, `dt_vector_channel_max()`
- **Kahan summation**, `scharr_gradient()`
- **Image buffer helpers** — `dt_iop_image_fill()`, `dt_iop_add_const()`, `dt_iop_image_mul_const()`, `dt_iop_image_alloc()`, `dt_iop_copy_image_roi()`, `dt_iop_alloc_image_buffers()` (variadic, supports all `DT_IMGSZ_*` flags), `dt_iop_have_required_input_format()`
- **Raw Bayer zoom helpers** — `dt_iop_clip_and_zoom_roi()`, `dt_iop_clip_and_zoom_mosaic_half_size_f()`, `dt_iop_clip_and_zoom_demosaic_half_size_f()`, `dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f()`
- **Tiling alias** — `default_tiling_callback()` → `dt_iop_default_tiling_callback()`
- **SIMD copy** — `dt_simd_memcpy()`
- **NaN/inf guards** — `dt_isnan()`, `dt_isinf()`, `dt_isfinite()`, `dt_isnormal()`

Design notes:
- `dt_iop_image_copy` and `dt_iop_image_copy_by_size` are already in `dtpipe_internal.h`; `iop_math.h` only adds a comment to avoid redefinition.
- `dt_iop_clip_and_zoom_roi` delegates to `dt_iop_clip_and_zoom` (already in `dtpipe_internal.h`).
- `default_tiling_callback` is an alias for `dt_iop_default_tiling_callback` (already in `dtpipe_internal.h`).
- Verified: `clang -fsyntax-only -x c -` compiles without errors. All 13 existing tests still pass.

### GLib/Darktable compatibility layer (`dtpipe_internal.h` — Task 8.2)
A self-contained compat section (guarded by `#ifndef GLIB_MAJOR_VERSION`) appended to `dtpipe_internal.h` provides everything Tier 1 IOPs need to compile without modification:
- GLib types: `gboolean`, `gint`, `guint`, `gchar`, `gfloat`, `gdouble`, `gpointer`; constants `TRUE`/`FALSE`; helpers `g_strlcpy`, `g_strlcat`, `g_snprintf`, `g_strdup`, `g_free`, `g_malloc0`; macros `MIN`, `MAX`, `CLAMP`
- Gettext no-ops: `_()`, `N_()`, `C_()`, `NC_()`
- Logging stubs: `dt_control_log()` (no-op), `dt_iop_set_module_trouble_message()` (prints to stderr if msg given)
- Image query helpers: `dt_image_is_raw()`, `dt_image_is_monochrome()`, `dt_image_is_rawprepare_supported()`, `dt_image_is_ldr()`, `dt_image_is_hdr()` — implemented using `img->flags`
- Config stubs: `dt_conf_get_string_const()`, `dt_conf_get_bool()`, `dt_conf_get_int()`, `dt_conf_get_float()` — all return harmless defaults
- `dt_iop_set_description_so()` — no-op stub for SO-level description calls
- GUI preset stubs: `dt_gui_presets_add_generic()`, `BUILTIN_PRESET()`
- `dt_dev_chroma_t` — WB/chromatic adaptation struct (D65coeffs, as_shot, wb_coeffs, adaptation, mode)
- `dt_develop_t` — minimal develop object (image_storage + chroma); `module->dev` is typed to `void*` pointing here
- `dt_is_scene_referred()` — always returns TRUE
- `DT_DEBUG_CONTROL_SIGNAL_RAISE` — no-op macro
- `dt_sse2_supported()` — always returns 0 (no SSE dispatch on macOS/ARM)

### Exposure IOP (`libdtpipe/src/iop/exposure.c` — Task 8.4)
First real IOP module ported into libdtpipe. Serves as the template for all subsequent Tier 1 IOP ports.

Key implementation decisions:
- Includes only `dtpipe_internal.h` and `iop/iop_math.h` — no darktable-internal headers.
- `dt_iop_exposure_params_t` field layout (int32_t mode, float black, float exposure, float deflicker_percentile, float deflicker_target_level, int32_t compensate_exposure_bias, int32_t compensate_hilite_pres) matches `_exposure_params_t` in `pipe/params.c` exactly, enabling correct memcpy-based history serialization.
- `init_presets()`, `reload_defaults()`, `init_global()`/`cleanup_global()` (OpenCL kernel setup), `process_cl()`, and all deflicker histogram logic removed. Deflicker is always disabled (`d->deflicker = 0`).
- `commit_params()` simplified: copies raw params with no exposure-bias or highlight-preservation compensation (these require EXIF wiring through `module->dev` — deferred to later tasks).
- `_process_common_setup()` computes `d->scale = 1/(exp2(-exposure) - black)` and `d->black` from the committed data struct.
- `process()` applies a SIMD-vectorized linear transform: `out[k] = (in[k] - black) * scale` across all `ch * npixels` elements, then scales `dsc.processed_maximum[]`.
- `dt_iop_exposure_init_global()` wires: `process_plain`, `init`, `init_pipe`, `cleanup_pipe`, `commit_params`, `input_colorspace` (IOP_CS_RGB), `output_colorspace` (IOP_CS_RGB). No `modify_roi_*` or `output_format` — exposure is a 1:1 pixel operation.
- Registered in `init.c` `_iop_registry[]` with `dt_iop_exposure_init_global`. The forward declaration is uncommented.
- Added to `src/CMakeLists.txt` DTPIPE_SOURCES under `# Phase 8: real IOP modules (Tier 1)`.

Verified: compiles cleanly, 13/13 tests pass, 99 checks, 0 failures. Exposure no longer appears in "has no process function" engine log.

### rawprepare IOP (`libdtpipe/src/iop/rawprepare.c` — Task 8.5)
First pipeline module ported — subtracts per-channel sensor black levels and normalises to [0, 1]. Operates on `IOP_CS_RAW` (single-channel Bayer uint16 or float). Implements `modify_roi_out`/`modify_roi_in` (sensor border crop) and `output_format` (writes `dsc->rawprepare` metadata).

Key implementation decisions:
- All internal functions (`process`, `commit_params`, `init_pipe`, `cleanup_pipe`, `init`, `output_format`, `modify_roi_in`, `modify_roi_out`) are `static` — required to avoid duplicate-symbol linker errors when multiple IOP `.c` files are compiled into the same `.dylib`. Same fix applied retroactively to `exposure.c`.
- `dt_rawspeed_crop_dcraw_filters` implemented as a file-local `_crop_dcraw_filters()` — rotates the 8-bit Bayer descriptor by `(cx & 1, cy & 1)` and replicates to all 4 bytes of the 32-bit filter word.
- GainMap support removed (requires `g_list_nth_data` / DNG infrastructure). `apply_gainmaps` is always FALSE.
- Database calls (`_image_set_rawcrops`, `dt_image_cache_get`) removed; `p_width`/`p_height` not updated (pipeline uses ROI extents directly).
- `init()` reads `module->dev->image_storage` for crop extents and raw black/white — requires `module->dev` to be set before `so->init()` is called.
- `dt_develop_t dev` added to `struct dt_pipe_s` (`pipe/create.h`) and populated from `img` in `dtpipe_create()` before `_build_module_list()` is called.
- `_build_module_list()` takes a new `dt_develop_t *dev` parameter; sets `m->dev = dev` before calling `so->init(m)`.
- `pipe->dev.chroma.as_shot[]` populated from `img->wb_coeffs[]` (used by temperature in Task 8.6).

Verified: 13/13 tests pass, 99 checks, 0 failures. `rawprepare` no longer in "no process function" log. Remaining stubs: `demosaic`, `colorin`, `colorout`.

### Temperature IOP (`libdtpipe/src/iop/temperature.c` — Task 8.6)
White balance module — applies per-channel multipliers to raw Bayer sensel data. Operates in `IOP_CS_RAW` (before demosaic).

Key implementation decisions:
- All internal functions are `static` (Phase 8 convention).
- No OpenCL, no lcms2/color-matrix temperature-tint conversion, no wb_presets database lookup — deferred to future tasks.
- `process()` keeps all three darktable code paths: X-Trans (`filters==9`), Bayer (`filters!=0`), non-mosaiced (`filters==0`). Uses `FC()` and `FCxtrans()` from `iop_math.h`.
- `init()` reads `dev->image_storage.wb_coeffs[]` if finite and non-zero, normalises so green == 1.0, falls back to neutral 1.0 coefficients otherwise.
- `commit_params()` copies four channel multipliers from params to `piece->data`, guards against zero coefficients (replaces with 1.0), and updates `dev->chroma.wb_coeffs[]` for downstream modules.
- `_publish_chroma()` writes applied coefficients back to `pipe->dsc.temperature` and `dev->chroma.wb_coeffs`, scales `dsc.processed_maximum[]`. Guards against NULL dev.
- `dt_dev_chroma_t` extended with `late_correction int` field in `dtpipe_internal.h` (needed to track D65-late preset state).
- `colorspace()` functions return `IOP_CS_RAW` by default; fall back to `IOP_CS_RGB` when piece's input is already non-raw (handles TIFF/JPEG input paths).
- Registered in `init.c` `_iop_registry[]` with `dt_iop_temperature_init_global`.

Verified: 13/13 tests pass, 0 failures. `temperature` no longer in "no process function" log. Remaining stubs: `demosaic`, `colorin`, `colorout`.

### Demosaic IOP (`libdtpipe/src/iop/demosaic.c` — Task 8.7)
Critical format-transition module: 1-channel Bayer RAW → 4-channel float RGBA. PPG algorithm only (Phase A).

Key implementation decisions:
- All internal functions are `static` (Phase 8 convention).
- Three helper files textually `#include`'d: `demosaicing/basics.c` (green equilibration, color smoothing, pre-median, box3), `demosaicing/ppg.c` (PPG interpolation), `demosaicing/passthrough.c` (mono/color passthrough).
- `output_format()` MUST set `dsc->channels=4, dsc->datatype=TYPE_FLOAT, dsc->cst=IOP_CS_RGB` — this is the critical 1→4 channel transition declaration.
- `_demosaic_full(roi_out)` returns `roi_out->scale > 0.5f`; fast half-size path used for preview scales.
- Fast path: `dt_iop_clip_and_zoom_demosaic_half_size_f` — averages 2×2 Bayer quads → 1 RGBA pixel; O(output_pixels), ~4× faster than PPG.
- Full PPG path: allocates intermediate full-res buffer, runs green equilibration, PPG, color smoothing.
- Compat macros added: `DT_IMAGE_4BAYER 0` (CYGM not supported), `dt_image_is_mono_sraw` delegates to `dt_image_is_monochrome`.

**Critical pipeline engine fixes required for demosaic (Task 8.7):**
1. **`pixelpipe.c` — output buffer sized after `output_format()`**: `bpp` and `bufsize` must be computed AFTER calling `module->output_format()`, not before. The old code computed bufsize from the incoming (1-ch) format, then allocated a 1-ch buffer that demosaic overwrote with 4-ch data → segfault. Fixed by moving `bpp`/`bufsize` computation to after `output_format()`.
2. **`pipe->pipe.dsc` reset before each render**: rawprepare mutates `pipe->pipe.dsc.filters` in-place; demosaic's `output_format()` sets `channels=4`. On the second render, `pipe->pipe.dsc` reflected the post-pipeline format (4-ch RGB) rather than the input format (1-ch RAW). Fixed by adding `pipe->initial_dsc` field to `struct dt_pipe_s` (saved at `dtpipe_create()` time) and restoring it at the start of both `_do_render()` in `render.c` and `_run_pipeline()` in `export.cc`.
3. **`render.c` — 1-channel input buffer for RAW**: `dtpipe_ensure_input_buf` must build a 1-channel float buffer for RAW images (not 4-channel RGBA) so rawprepare/demosaic receive proper Bayer data.
4. **`pixelpipe.c` base case — reads from `pipe->dsc`**: The `_process_rec()` base case now reflects the image's actual input format from `pipe->dsc` (1-ch RAW for raw images) rather than hardcoding 4-ch RGB.
5. **`create.c` — `pipe->pipe.dsc` initialization**: `dtpipe_create()` initializes `pipe->pipe.dsc` from image metadata before building module list: `channels=1, TYPE_FLOAT, IOP_CS_RAW, filters, xtrans` for raw; `channels=4, TYPE_FLOAT, IOP_CS_RGB` for non-raw.

Verified: 99/99 tests pass, 0 failures, 8.8s total. `demosaic` no longer in "no process function" log. Remaining stubs: `colorin`, `colorout`.

### Phase 8 IOP static-function convention
All IOP internal functions MUST be `static`. In darktable each IOP compiles to its own `.so`; duplicate symbol names across IOPs are harmless. In libdtpipe all IOPs link into a single shared library, so non-static `process`, `init`, etc. collide at link time. Rule: every function in `libdtpipe/src/iop/*.c` that is not the public `dt_iop_<name>_init_global()` entry point must be declared `static`.

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
