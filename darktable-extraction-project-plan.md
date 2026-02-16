# darktable Pixelpipe Extraction Project Plan

## Project Overview

**Goal:** Extract darktable's image processing pipeline (pixelpipe) into a standalone C library, then wrap it with a Node.js addon for use in an Electron-based RAW photo editor.

**Estimated Timeline:** 12-16 weeks to MVP

**End Result:**
- `libdtpipe` — A headless C library for RAW processing
- `@app/dtpipe` — Node.js native addon wrapping the library
- Electron app with custom UI using the processing backend

---

## Phase 0: Analysis & Discovery

> **Goal:** Understand the codebase before cutting it apart.

### Task 0.1: Map IOP Dependencies

- [x] **Status:** Complete
- **Input:** darktable `src/iop/` directory
- **Output:** `analysis/iop-dependencies.json`

**Claude Code Prompt:**
```
Analyze all .c files in the darktable src/iop/ directory. For each file, extract and output as JSON:
- filename
- includes (both system and local)
- external function calls (functions not defined in the file)
- any GTK/GUI functions used (functions starting with gtk_, dt_bauhaus_, gui_)
- the params struct definition if present

Output as a JSON array of objects.
```

**Verification:** JSON file loads, contains entries for all ~60 IOP modules.

---

### Task 0.2: Identify Core vs Optional Modules

- [x] **Status:** Complete
- **Input:** IOP dependency analysis + personal usage patterns
- **Output:** `analysis/module-priority.md`

**Claude Code Prompt:**
```
Given this list of darktable IOP modules, categorize them into tiers:

Tier 1 - Essential (pipeline won't work without these):
- Modules required for basic RAW → RGB conversion

Tier 2 - Core Creative (the 15-20 most-used editing modules):
- Exposure, contrast, color grading, sharpening, noise reduction, etc.

Tier 3 - Specialized (nice to have):
- Modules for specific use cases

Tier 4 - Skip (rarely used or deprecated):
- Old modules superseded by newer ones

For each module, note:
- What it does
- Whether it has OpenCL support
- Any unusual dependencies

[Paste list of modules from 0.1 output]
```

**Verification:** All modules categorized, Tier 1 has ~6-8 modules, Tier 2 has 15-20.

---

### Task 0.3: Map common/ Dependencies

- [x] **Status:** Complete
- **Input:** darktable `src/common/` directory
- **Output:** `analysis/common-dependencies.json`

**Claude Code Prompt:**
```
Analyze all .c and .h files in darktable src/common/. For each file, determine:

1. Does it have GTK/GUI dependencies? (includes gtk/, cairo/, pango/, or calls gtk_* functions)
2. Does it have database dependencies? (includes sqlite, calls dt_database_*)
3. What is its primary purpose?

Categorize each file as:
- KEEP: Pure computation, no GUI/DB deps
- MODIFY: Has deps that can be stubbed out
- SKIP: Fundamentally tied to GUI/DB

Output as JSON with reasoning for each file.
```

**Verification:** JSON file loads, provides clear guidance for each file in common/.

---

### Task 0.4: Extract Parameter Schemas

- [x] **Status:** Complete
- **Input:** Tier 1 and Tier 2 IOP source files
- **Output:** `schemas/` directory with JSON schema per module

**Claude Code Prompt:**
```
For the darktable IOP module in [filename], extract the parameter schema:

1. Find the dt_iop_[name]_params_t struct
2. For each field, determine:
   - name
   - C type
   - corresponding JSON type (number, integer, boolean, string, array)
   - default value (from dt_iop_[name]_params_t's initialization or DT_MODULE_INTROSPECTION)
   - min/max range if determinable from code comments or UI bounds
   - description from code comments if available

Output as a JSON Schema document.

[Paste the .c file contents]
```

**Verification:** Run for exposure.c first as template, then batch remaining modules.

---

## Phase 1: Build System Bootstrap

> **Goal:** Create project structure and get an empty library compiling.

### Task 1.1: Create Project Scaffold

- [x] **Status:** Complete
- **Input:** None
- **Output:** Directory structure

**Claude Code Prompt:**
```
Create the following directory structure for the libdtpipe project:

libdtpipe/
├── CMakeLists.txt           # Root CMake file
├── cmake/
│   └── FindLCMS2.cmake      # Custom find modules if needed
├── include/
│   └── dtpipe.h             # Public API header
├── src/
│   ├── CMakeLists.txt       # Source CMake file
│   ├── init.c               # Library initialization
│   ├── pipe/
│   │   └── .gitkeep
│   ├── iop/
│   │   └── .gitkeep
│   ├── common/
│   │   └── .gitkeep
│   └── imageio/
│       └── .gitkeep
├── data/
│   ├── kernels/
│   │   └── .gitkeep
│   └── color/
│       └── .gitkeep
├── tests/
│   └── CMakeLists.txt
├── node/                    # Node.js addon (later)
│   └── .gitkeep
├── .gitignore
└── README.md

Create placeholder files and a basic .gitignore for C/CMake projects.
```

**Verification:** Directory structure exists, git init works.

---

### Task 1.2: Write Root CMakeLists.txt

- [x] **Status:** Complete
- **Input:** Dependency list
- **Output:** `CMakeLists.txt`

**Claude Code Prompt:**
```
Write a CMakeLists.txt for the libdtpipe project with:

Required dependencies:
- lcms2 (color management)
- libjpeg
- libpng  
- libtiff
- exiv2 (metadata)
- pugixml (XML parsing)

Optional dependencies:
- OpenCL (GPU acceleration)
- OpenMP (CPU parallelization)

The project should:
- Require CMake 3.16+
- Use C11 standard
- Build a shared library called 'dtpipe'
- Set up proper include directories
- Create a 'dtpipe' CMake target that other projects can link against
- Support both Debug and Release builds
- On Debug, enable sanitizers (address, undefined)

Include standard CMake best practices (target_include_directories, generator expressions, etc.)
```

**Verification:** `cmake -B build` succeeds (even if library is empty).

---

### Task 1.3: Integrate rawspeed

- [x] **Status:** Complete
- **Input:** rawspeed repository
- **Output:** CMake integration

**Claude Code Prompt:**
```
Add rawspeed as a dependency to the libdtpipe CMake project.

rawspeed is at: https://github.com/darktable-org/rawspeed

Options to consider:
1. FetchContent (downloads at configure time)
2. ExternalProject (downloads at build time)
3. Git submodule + add_subdirectory

Choose the most appropriate method and implement it. rawspeed should:
- Build as part of our project
- Export its include directories to our targets
- Link statically into libdtpipe

Note any rawspeed-specific CMake variables that need to be set.
```

**Verification:** rawspeed downloads and builds, can `#include <RawSpeed-API.h>`.

---

## Phase 2: Strip GUI from IOPs

> **Goal:** Create clean, GUI-free versions of all processing modules.

### Task 2.1: Create IOP Stripping Script

- [x] **Status:** Complete
- **Input:** None
- **Output:** `scripts/strip_iop.py`

**Claude Code Prompt:**
```
Write a Python script that processes a darktable IOP .c file and removes GUI-related code.

The script should:

1. Remove these includes:
   - "bauhaus/bauhaus.h"
   - "dtgtk/*.h"  
   - "gui/*.h"
   - <gtk/gtk.h>
   - Any include with "gui" in the path

2. Remove these functions entirely (including their bodies):
   - gui_init()
   - gui_cleanup()
   - gui_update()
   - gui_changed()
   - gui_reset()
   - gui_focus()
   - Any function starting with gui_
   - Any static functions only called by gui_* functions

3. Keep these functions:
   - process()
   - process_cl()
   - init()
   - cleanup()
   - commit_params()
   - init_pipe()
   - cleanup_pipe()
   - modify_roi_in()
   - modify_roi_out()
   - distort_transform()
   - distort_backtransform()
   - DT_MODULE_INTROSPECTION macro

4. Remove:
   - dt_iop_*_gui_data_t struct definitions
   - References to self->gui_data
   - GTK widget creation/manipulation

5. Preserve:
   - All processing logic
   - OpenCL kernel loading in init()
   - Parameter structs (dt_iop_*_params_t)
   - Global data structs (dt_iop_*_global_data_t)

Usage: python strip_iop.py input.c output.c

The script should be conservative—when uncertain, keep the code and add a TODO comment.
```

**Verification:** Run on a simple IOP, output compiles (with stub headers).

---

### Task 2.2: Strip Template Module (exposure.c)

- [x] **Status:** Complete
- **Input:** darktable `src/iop/exposure.c`
- **Output:** `src/iop/exposure.c` (stripped)

**Claude Code Prompt:**
```
I have a Python script that strips GUI code from darktable IOPs. I've run it on exposure.c.

Here's the output:
[paste stripped file]

Review this stripped module and:
1. Verify process() and process_cl() are intact and complete
2. Check that all necessary includes remain
3. Identify any remaining GUI references that slipped through
4. Fix any obvious issues
5. Add a header comment noting this was extracted from darktable

Also create a minimal test that:
- Includes the stripped file
- Verifies the structs are defined
- Verifies function signatures match expected API
```

**Verification:** Stripped exposure.c compiles with stub headers.

---

### Task 2.3: Batch Strip Essential Modules (Tier 1)

- [x] **Status:** Complete
- **Input:** Tier 1 module list
- **Output:** Stripped source files

**Tier 1 Modules:**
- [x] rawprepare.c (945 -> 807 lines; removed 3 GUI functions)
- [x] temperature.c (2250 -> 935 lines; removed 21 GUI functions)
- [x] highlights.c (1375 -> 1093 lines; removed 6 GUI functions)
- [x] demosaic.c (1876 -> 1504 lines; removed 10 GUI functions)
- [x] colorin.c (2093 -> 1667 lines; removed 5 GUI functions + manual fix for update_profile_list)
- [x] colorout.c (878 -> 754 lines; removed 6 GUI functions)

**Claude Code Prompt:**
```
Strip GUI code from these essential darktable IOP modules:

[paste each file, one at a time]

For each module:
1. Apply the stripping logic from strip_iop.py
2. Verify the process() function is complete
3. Note any unusual dependencies or issues
4. Output the cleaned source

These are Tier 1 modules—the pipeline cannot function without them.
```

**Verification:** All Tier 1 modules compile with stub headers.

---

### Task 2.4: Batch Strip Core Creative Modules (Tier 2)

- [x] **Status:** Complete
- **Input:** Tier 2 module list
- **Output:** Stripped source files

**Tier 2 Modules (suggested—customize to your workflow):**
- [x] basecurve.c (2184 -> 1683 lines; removed 12 GUI functions)
- [x] filmic.c (1648 -> 900 lines; removed 26 GUI functions)
- [x] filmicrgb.c (4755 -> 3025 lines; removed 17 GUI functions)
- [x] sigmoid.c (991 -> 867 lines; removed 3 GUI functions)
- [x] colorbalancergb.c (2068 -> 1248 lines; removed 13 GUI functions)
- [ ] toneequalizer.c (not present in source tree)
- [x] sharpen.c (447 -> 420 lines; removed 1 GUI function)
- [x] bilat.c (506 -> 361 lines; removed 3 GUI functions)
- [x] denoiseprofile.c (3968 -> 3086 lines; removed 15 GUI functions)
- [ ] lens.c (not present in source tree)
- [x] crop.c (1939 -> 814 lines; removed 20 GUI functions)
- [x] flip.c (631 -> 509 lines; removed 8 GUI functions)
- [x] clipping.c (3349 -> 1355 lines; removed 25 GUI functions)
- [x] graduatednd.c (1119 -> 734 lines; removed 11 GUI functions)
- [x] vignette.c (1105 -> 634 lines; removed 7 GUI functions)
- [x] channelmixer.c (761 -> 640 lines; removed 6 GUI functions)
- [x] channelmixerrgb.c (4747 -> 2072 lines; removed 42 GUI functions)
- [x] levels.c (999 -> 447 lines; removed 14 GUI functions)
- [x] rgbcurve.c (1953 -> 693 lines; removed 21 GUI functions)

**Claude Code Prompt:**
```
Strip GUI code from this batch of darktable IOP modules:

[paste 3-5 files per prompt]

For each module:
1. Apply stripping logic
2. Flag any modules with complex GUI interactions (color pickers, drawn masks, etc.)
3. Note OpenCL availability
4. Output cleaned source
```

**Verification:** All 17 present Tier 2 modules scanned clean of GUI tokens. Manual fixes applied to 4 modules where strip_iop.py missed residual GUI functions:
- basecurve.c: removed `dt_iop_basecurve_motion_notify` and `_move_point_internal` (mouse canvas handlers with non-`gui_` prefix)
- crop.c: removed `_commit_box`, `_gui_get_grab`, `button_released`, `mouse_actions`
- clipping.c: removed `get_grab`, `mouse_actions`
- channelmixerrgb.c: removed `_extract_patches` and its `extraction_result_t` typedef (color checker patch extraction using gui_data struct)

Note: Full compilation verification requires stub headers from Phase 3.

---

## Phase 3: Core Infrastructure

> **Goal:** Create the foundation that IOPs plug into.

### Task 3.1: Write Minimal darktable.h

- [x] **Status:** Complete
- **Input:** Original darktable headers
- **Output:** `libdtpipe/src/dtpipe_internal.h`

**Claude Code Prompt:**
```
Create a minimal internal header for libdtpipe that defines core structs.

Based on darktable's headers, extract and simplify:

1. dt_image_t - Image metadata and buffer info
   - Keep: dimensions, buffer pointers, camera info, orientation, color profile
   - Remove: database IDs, thumbnail flags, UI state

2. dt_iop_module_t - Module instance
   - Keep: name, params, enabled, process function pointers
   - Remove: GUI data, widget pointers, expander state

3. dt_dev_pixelpipe_t - Pipeline state
   - Keep: type (full/preview), scale, ROI, module list
   - Remove: GTK backbuffer, histogram data, UI callbacks

4. dt_iop_params_t - Base params type
   - Keep as opaque void* with size

5. dt_iop_roi_t - Region of interest
   - Keep: x, y, width, height, scale

6. Function pointer typedefs for IOP callbacks:
   - dt_iop_process_f
   - dt_iop_process_cl_f  
   - dt_iop_commit_params_f
   - etc.

Make it self-contained—no dependencies on original darktable headers.
Use C11 standard types (stdint.h, stdbool.h).
```

**Verification:** Header compiles standalone, IOPs can include it.

---

### Task 3.2: Implement dtpipe_init()

- [x] **Status:** Complete
- **Input:** Initialization requirements
- **Output:** `src/init.c`

**Claude Code Prompt:**
```
Implement the library initialization for libdtpipe.

dtpipe_init(const char *data_dir) should:

1. Store data directory path (for kernels, color profiles, etc.)

2. Initialize color management:
   - Set up lcms2 context
   - Load built-in sRGB profile
   - Prepare for loading ICC profiles from data_dir/color/

3. Initialize OpenCL (if available):
   - Detect devices
   - Compile kernels from data_dir/kernels/
   - Set up command queues
   - If OpenCL fails, continue with CPU-only mode

4. Discover and load IOP modules:
   - For now, use static registration (call init function for each compiled-in module)
   - Later could support dynamic loading from .so files
   - Build module list sorted by default iop_order

5. Set up global state structure (dt_t or similar)

dtpipe_cleanup() should release all resources in reverse order.

Handle errors gracefully—return error codes, don't crash.
Use thread-safe initialization (call_once or similar).
```

**Verification:** Init/cleanup runs without leaks (valgrind).

---

### Task 3.3: Port iop_order.c

- [x] **Status:** Complete
- **Input:** darktable `src/common/iop_order.c`
- **Output:** `src/common/iop_order.c` (ported)

**Claude Code Prompt:**
```
Port darktable's iop_order.c to libdtpipe.

The IOP order system determines module execution sequence in the pipeline.

Changes needed:
1. Remove database integration:
   - No reading/writing order to SQLite
   - Order comes from: built-in defaults, or JSON history file

2. Keep order logic:
   - v5.0 order as default (DT_IOP_ORDER_V50)
   - Support for custom orders
   - Module position lookup functions

3. Simplify to essentials:
   - dt_ioppr_get_iop_order_list_version() - get built-in order
   - dt_ioppr_get_iop_order() - get position for module
   - dt_ioppr_set_iop_order() - set custom order

4. Remove:
   - All dt_database_* calls
   - Image ID parameters (we don't have a DB)
   - Preset system integration

5. Add:
   - Function to load order from JSON
   - Function to serialize order to JSON

Input: [paste original iop_order.c and iop_order.h]
```

**Verification:** Can retrieve module order, order matches darktable defaults.

---

### Task 3.4: Port pixelpipe_hb.c (Part 1 - Structures)

- [x] **Status:** Complete
- **Input:** darktable `src/develop/pixelpipe_hb.c`
- **Output:** `src/pipe/pixelpipe.c` (partial)

**Claude Code Prompt:**
```
Begin porting darktable's pixelpipe_hb.c to libdtpipe.

Part 1: Core structures and initialization.

Extract and adapt:
1. dt_dev_pixelpipe_t structure
   - Keep: type, iop list, iwidth/iheight, processed_*, opencl state
   - Remove: backbuf (GTK), histogram, forms, GUI-related fields

2. dt_dev_pixelpipe_iop_t structure  
   - Keep: module pointer, params, enabled, roi_in/roi_out
   - Remove: GUI blending mode widgets

3. Pipeline initialization:
   - dt_dev_pixelpipe_init() 
   - dt_dev_pixelpipe_cleanup()
   - dt_dev_pixelpipe_set_input() - set input image

4. Pipeline types we need:
   - DT_DEV_PIXELPIPE_FULL - full resolution export
   - DT_DEV_PIXELPIPE_PREVIEW - fast preview at reduced size
   - Remove: THUMBNAIL, PREVIEW2, etc.

Make it compile standalone. Processing comes in Part 2.

Input: [paste relevant sections of pixelpipe_hb.c and headers]
```

**Verification:** Pipeline struct compiles, init/cleanup work.

---

### Task 3.5: Port pixelpipe_hb.c (Part 2 - Processing)

- [x] **Status:** Complete & Verified
- **Input:** pixelpipe_hb.c processing functions
- **Output:** `src/pipe/pixelpipe.c` (complete)

**Claude Code Prompt:**
```
Continue porting pixelpipe_hb.c.

Part 2: The actual processing loop.

Extract and adapt:
1. dt_dev_pixelpipe_process() - main entry point
   - Keep: ROI calculation, module iteration, buffer management
   - Remove: histogram calculation, GTK idle callbacks

2. dt_dev_pixelpipe_process_rec() - recursive processing
   - Keep: cache lookup, tiling dispatch, OpenCL/CPU branching
   - Simplify: remove some cache complexity initially

3. Buffer management:
   - Keep allocation/deallocation logic
   - Keep alignment requirements (for SIMD/OpenCL)

4. OpenCL dispatch:
   - Keep: device selection, memory transfers, kernel execution
   - Keep: fallback to CPU on OpenCL failure

5. Tiling:
   - Keep: dt_dev_pixelpipe_process_tiled() for large images
   - This handles images too big for GPU memory

Remove:
- All signal emissions (DT_SIGNAL_*)
- Progress callbacks to GUI
- Backbuffer updates

Input: [paste processing sections of pixelpipe_hb.c]
```

**Verification:** Can process a simple test image through minimal pipeline.

**To run the test:**
```bash
cmake -S libdtpipe -B build-check -DCMAKE_BUILD_TYPE=Debug
cmake --build build-check
cd build-check && ctest --output-on-failure
```

---

## Phase 4: Public API Implementation

> **Goal:** Create the clean C API that Node.js will call.

### Task 4.1: Define Public Header

- [x] **Status:** Complete & verified
- **Input:** API specification
- **Output:** `include/dtpipe.h`

**Claude Code Prompt:**
```
Implement the public API header for libdtpipe.

Requirements:
- C89 compatible (for maximum FFI compatibility)
- Opaque handle types (pointers to forward-declared structs)
- No internal types exposed
- Comprehensive documentation comments
- Error codes enumeration
- Thread-safety notes

API surface:

// Lifecycle
int dtpipe_init(const char *data_dir);
void dtpipe_cleanup(void);

// Image loading
dt_image_t *dtpipe_load_raw(const char *path);
void dtpipe_free_image(dt_image_t *img);
int dtpipe_get_width(dt_image_t *img);
int dtpipe_get_height(dt_image_t *img);
const char *dtpipe_get_camera_maker(dt_image_t *img);
const char *dtpipe_get_camera_model(dt_image_t *img);

// Pipeline
dt_pipe_t *dtpipe_create(dt_image_t *img);
void dtpipe_free(dt_pipe_t *pipe);

// Parameters
int dtpipe_set_param_float(dt_pipe_t *pipe, const char *module, const char *param, float value);
int dtpipe_set_param_int(dt_pipe_t *pipe, const char *module, const char *param, int value);
int dtpipe_get_param_float(dt_pipe_t *pipe, const char *module, const char *param, float *out);
int dtpipe_enable_module(dt_pipe_t *pipe, const char *module, int enabled);

// Rendering
dt_render_result_t *dtpipe_render(dt_pipe_t *pipe, float scale);
dt_render_result_t *dtpipe_render_region(dt_pipe_t *pipe, int x, int y, int w, int h, float scale);
void dtpipe_free_render(dt_render_result_t *result);

// Export
int dtpipe_export_jpeg(dt_pipe_t *pipe, const char *path, int quality);
int dtpipe_export_png(dt_pipe_t *pipe, const char *path);
int dtpipe_export_tiff(dt_pipe_t *pipe, const char *path, int bits);

// History
char *dtpipe_serialize_history(dt_pipe_t *pipe);
int dtpipe_load_history(dt_pipe_t *pipe, const char *json);
int dtpipe_load_xmp(dt_pipe_t *pipe, const char *path);
int dtpipe_save_xmp(dt_pipe_t *pipe, const char *path);

// Module introspection
int dtpipe_get_module_count(void);
const char *dtpipe_get_module_name(int index);
```

**Verification:** Header compiles, no internal dependencies leak through.

---

### Task 4.2: Implement Image Loading

- [x] **Status:** Complete
- **Input:** rawspeed integration
- **Output:** `src/imageio/load.cc`

**Claude Code Prompt:**
```
Implement RAW image loading using rawspeed.

dtpipe_load_raw(const char *path) should:

1. Use rawspeed to decode the file:
   - Create RawParser for the file
   - Get RawDecoder
   - Call decode() to get RawImage

2. Extract metadata:
   - Dimensions (width, height)
   - Camera make/model
   - ISO, shutter speed, aperture (if available)
   - White balance coefficients
   - Color matrix (if available)

3. Handle the raw buffer:
   - Get pointer to decoded data
   - Note CFA pattern (RGGB, etc.)
   - Note bit depth

4. Create and return dt_image_t:
   - Allocate struct
   - Copy metadata
   - Store buffer pointer (or copy buffer)
   - Set up for pipeline consumption

Error handling:
- Return NULL on failure
- Set error message retrievable via dtpipe_get_last_error()

Support common RAW formats: CR2, CR3, NEF, ARW, RAF, ORF, DNG
```

**Verification:** Load a test RAW, verify dimensions and metadata.

---

### Task 4.3: Implement Pipeline Creation

- [x] **Status:** Complete
- **Input:** Pipeline infrastructure
- **Output:** `src/pipe/create.c`

**Claude Code Prompt:**
```
Implement pipeline creation.

dtpipe_create(dt_image_t *img) should:

1. Allocate dt_pipe_t structure

2. Link to source image:
   - Store image pointer
   - Set input dimensions

3. Initialize module list:
   - Get default IOP order (v5.0)
   - For each registered module in order:
     - Create dt_dev_pixelpipe_iop_t instance
     - Call module's init_pipe()
     - Set default parameters
     - Set default enabled state

4. Set up defaults for common modules:
   - Enable: rawprepare, demosaic, colorin, colorout
   - Enable: exposure (with neutral settings)
   - Enable: filmic or sigmoid (user preference?)
   - Disable: most creative modules by default

5. Initialize OpenCL context for this pipe (if available)

dtpipe_free(dt_pipe_t *pipe) should:
1. For each module instance:
   - Call cleanup_pipe()
   - Free params
2. Release OpenCL resources
3. Free structure
```

**Verification:** Create pipeline for test image, iterate modules.

---

### Task 4.4: Implement Parameter Access

- [x] **Status:** Complete
- **Input:** Module param schemas
- **Output:** `src/pipe/params.c`

**Claude Code Prompt:**
```
Implement parameter get/set functions.

Need to handle:
1. Finding module by name in pipeline
2. Finding parameter by name in module's params struct
3. Type checking (float vs int vs other)
4. Bounds validation (optional, warn on out-of-range)

Implementation approach:
- Each IOP module needs a parameter descriptor table
- Table maps: param_name -> offset, type, size
- Use this for generic get/set

dtpipe_set_param_float(pipe, "exposure", "exposure", 1.5f) should:
1. Find "exposure" module in pipe->iop_list
2. Look up "exposure" param in module's param descriptors
3. Verify type is float
4. Write value to (module->params + offset)
5. Mark module as needing reprocess

For this to work, each IOP needs introspection data.
Option A: Generate from DT_MODULE_INTROSPECTION macros
Option B: Hand-write descriptors for each module
Option C: Use JSON schema files from Phase 0

Implement Option B for now (simplest), design for Option A later.

Start with exposure module as template:
- exposure (float): EV adjustment
- black (float): black point
```

**Verification:** Set exposure param, verify value stored correctly.

---

### Task 4.5: Implement Render

- [x] **Status:** Complete
- **Input:** Pipeline processing code
- **Output:** `src/pipe/render.c`

**Claude Code Prompt:**
```
Implement the render function.

dtpipe_render(dt_pipe_t *pipe, float scale) should:

1. Calculate output dimensions:
   - output_width = input_width * scale
   - output_height = input_height * scale

2. Set up ROI:
   - Full image at requested scale

3. Call pipeline processing:
   - dt_dev_pixelpipe_process() or equivalent
   - This runs all enabled modules in order

4. Convert output to RGBA:
   - Pipeline outputs float RGB
   - Convert to 8-bit RGBA for display
   - Apply gamma/sRGB transform if not already done

5. Return result:
   - Allocate dt_render_result_t
   - Set data pointer, dimensions, stride
   - Caller must free with dtpipe_free_render()

dtpipe_render_region() is similar but:
- Only process specified rectangle
- Used for viewport rendering (process what's visible)

Performance considerations:
- Cache intermediate results where possible
- Invalidate cache when params change
- For scale < 1.0, use optimized demosaic
```

**Verification:** Render test image at scale 0.25, verify output dimensions and basic correctness.

---

### Task 4.6: Implement Export

- [x] **Status:** Complete
- **Input:** Format libraries
- **Output:** `src/imageio/export.c`

**Claude Code Prompt:**
```
Implement export functions.

All exports should:
1. Render at full resolution (scale = 1.0)
2. Convert to appropriate color space/bit depth
3. Write to file using format library
4. Embed metadata (EXIF, XMP)

dtpipe_export_jpeg(pipe, path, quality):
- Render full res
- Convert to 8-bit sRGB
- Write using libjpeg
- Quality: 0-100
- Embed EXIF from source image

dtpipe_export_png(pipe, path):
- Render full res
- Convert to 16-bit sRGB
- Write using libpng
- Include ICC profile

dtpipe_export_tiff(pipe, path, bits):
- Render full res
- Support 8 or 16 bit
- Write using libtiff
- Uncompressed or LZW compression
- Include ICC profile
- Embed EXIF

Use exiv2 for metadata embedding.
Return 0 on success, error code on failure.
```

**Verification:** Export test image to each format, verify files open correctly.

---

## Phase 5: History Serialization

> **Goal:** Save and restore edit state.

### Task 5.1: Design History JSON Format

- [x] **Status:** Complete
- **Input:** Module schemas, XMP format reference
- **Output:** `docs/history-format.md`

**Claude Code Prompt:**
```
Design a JSON format for storing darktable-compatible edit history.

Requirements:
- Human-readable and editable
- Supports all module parameters
- Supports module ordering
- Supports masks (future)
- Can round-trip to/from XMP (mostly)

Proposed structure:

{
  "version": "1.0",
  "generator": "libdtpipe",
  
  "source": {
    "filename": "IMG_1234.CR2",
    "hash": "sha256:...",  // optional, for verification
    "camera": "Canon EOS R5"
  },
  
  "settings": {
    "iop_order": "v5.0",  // or "custom" with explicit order
    "color_workflow": "scene-referred"
  },
  
  "modules": {
    "exposure": {
      "enabled": true,
      "version": 6,
      "params": {
        "exposure": 1.5,
        "black": 0.002,
        "compensate_exposure_bias": true
      }
    },
    "filmic": {
      "enabled": true,
      "version": 5,
      "params": {
        "white_point_source": 4.0,
        "black_point_source": -8.0,
        // ... all params
      }
    }
    // ... more modules
  },
  
  "custom_order": [  // only if iop_order is "custom"
    "rawprepare",
    "exposure",
    // ...
  ],
  
  "masks": {
    // future: mask definitions
  }
}

Document each field, versioning strategy, and migration rules.
```

**Verification:** Document reviewed, covers all use cases.

---

### Task 5.2: Implement History Serialization

- [x] **Status:** Complete
- **Input:** JSON format spec
- **Output:** `src/history/serialize.c`

**Claude Code Prompt:**
```
Implement history serialization to JSON.

dtpipe_serialize_history(dt_pipe_t *pipe) should:

1. Create JSON root object
2. Add version and metadata
3. For each module in pipeline:
   - Add entry with enabled state
   - Serialize all params using introspection
   - Use param descriptors to determine types
4. Add custom order if applicable
5. Convert to string, return (caller frees)

Use cJSON library for JSON manipulation.

Implementation:
- Create helper: serialize_module(cJSON *parent, dt_iop_module_t *module)
- Handle each param type: float, int, bool, float[3], etc.
- Format floats with sufficient precision (6 decimal places)
```

**Verification:** Serialize test pipeline, verify JSON is valid and complete.

---

### Task 5.3: Implement History Deserialization

- [x] **Status:** Complete
- **Input:** JSON format spec
- **Output:** `src/history/deserialize.c`

**Claude Code Prompt:**
```
Implement history loading from JSON.

dtpipe_load_history(dt_pipe_t *pipe, const char *json) should:

1. Parse JSON string
2. Verify version compatibility
3. For each module in JSON:
   - Find module in pipeline
   - Set enabled state
   - Deserialize params
4. Apply custom order if present
5. Return 0 on success, error code on failure

Error handling:
- Unknown modules: warn and skip
- Wrong param types: warn and use default
- Missing params: use defaults
- Version mismatches: attempt migration

Create helper: deserialize_module(cJSON *obj, dt_iop_module_t *module)
```

**Verification:** Round-trip: serialize -> deserialize -> serialize, compare JSON.

---

### Task 5.4: Implement XMP Reading

- [x] **Status:** Complete
- **Input:** darktable XMP format
- **Output:** `src/history/xmp_read.cc`, `src/history/xmp_read.h`

**Claude Code Prompt:**
```
Implement reading darktable XMP sidecar files.

dtpipe_load_xmp(dt_pipe_t *pipe, const char *path) should:

1. Parse XMP file using pugixml
2. Find darktable namespace (usually "darktable")
3. Extract history stack:
   - Each history entry has: operation, enabled, params (blob)
   - Params are stored as hex-encoded binary
4. Decode each history entry:
   - Convert hex to bytes
   - Interpret according to module version
5. Apply to pipeline

darktable XMP structure (simplified):
<x:xmpmeta>
  <rdf:RDF>
    <rdf:Description>
      <darktable:history>
        <rdf:Seq>
          <rdf:li>
            <darktable:operation>exposure</darktable:operation>
            <darktable:enabled>1</darktable:enabled>
            <darktable:params>base64blob</darktable:params>
            <darktable:modversion>6</darktable:modversion>
          </rdf:li>
        </rdf:Seq>
      </darktable:history>
    </rdf:Description>
  </rdf:RDF>
</x:xmpmeta>

Handle version differences—darktable modules evolve over time.
```

**Verification:** Load XMP created by darktable, verify params match.

---

### Task 5.5: Implement XMP Writing

- [x] **Status:** Complete
- **Input:** darktable XMP format
- **Output:** `src/history/xmp_write.h`, `src/history/xmp_write.cc`
- **Tests:** `tests/test_xmp_write.c`

**Implementation:**
- Uses pugixml to build a darktable-compatible XMP document
- Writes `x:xmpmeta` → `rdf:RDF` → `rdf:Description` → `darktable:history` → `rdf:Seq` structure
- Each module emitted as `rdf:li` with `darktable:num/operation/enabled/modversion/params/multi_priority/multi_name` attributes
- Params encoded as plain lowercase hex (always readable by darktable)
- `darktable:history_end` set to module count
- `modversion` emitted as 0 (placeholder; same policy as serialize.c)
- Error handling: NULL args → `DTPIPE_ERR_INVALID_ARG`, write failure → `DTPIPE_ERR_IO`

**Verification:** All tests pass in release build. Confirmed:
- NULL arg guards work
- Bad path returns `DTPIPE_ERR_IO`
- File is created and non-empty
- Real-image XMP round-trip: save then load back returns `DTPIPE_OK`

---

## Phase 6: Node.js Addon

> **Goal:** Make libdtpipe callable from JavaScript.

### Task 6.1: Create Addon Scaffold

- [x] **Status:** Complete
- **Input:** None
- **Output:** `node/` directory structure

**Claude Code Prompt:**
```
Create a Node.js native addon project for libdtpipe.

Structure:
node/
├── package.json
├── binding.gyp
├── src/
│   └── addon.cc      # Main addon code
├── lib/
│   └── index.js      # JS wrapper
├── types/
│   └── index.d.ts    # TypeScript declarations
└── test/
    └── test.js

package.json:
- name: @app/dtpipe
- main: lib/index.js
- types: types/index.d.ts
- scripts: build, test
- dependencies: node-addon-api

binding.gyp:
- Link against libdtpipe
- Set include paths
- Set library paths
- Support macOS, Linux, Windows (later)

Use node-addon-api (N-API) for ABI stability across Node versions.
```

**Verification:** `npm run build` succeeds (empty addon).

---

### Task 6.2: Wrap Image Loading

- [x] **Status:** Complete
- **Input:** dtpipe.h
- **Output:** `node/src/addon.cc` (partial)

**Claude Code Prompt:**
```
Add image loading to the Node.js addon.

Implement:
1. loadRaw(path: string): Image
2. Image class with:
   - width: number (getter)
   - height: number (getter)
   - cameraMaker: string (getter)
   - cameraModel: string (getter)
   - dispose(): void

Use N-API to:
- Create Image class wrapping dt_image_t*
- Store native pointer in instance
- Clean up in destructor and dispose()

Example usage:
```javascript
const { loadRaw } = require('@app/dtpipe');
const img = loadRaw('/path/to/image.cr2');
console.log(`${img.width}x${img.height}`);
console.log(`Camera: ${img.cameraMaker} ${img.cameraModel}`);
img.dispose();
```

Error handling:
- Throw JavaScript Error on load failure
- Include error message from library
```

**Verification:** Load test RAW from Node.js, log metadata.

---

### Task 6.3: Wrap Pipeline Operations

- [x] **Status:** Complete
- **Input:** dtpipe.h
- **Output:** `node/src/addon.cc` (extended)

**Claude Code Prompt:**
```
Add pipeline operations to the Node.js addon.

Implement:
1. createPipeline(image: Image): Pipeline
2. Pipeline class with:
   - setParam(module: string, param: string, value: number): void
   - getParam(module: string, param: string): number
   - enableModule(module: string, enabled: boolean): void
   - isModuleEnabled(module: string): boolean
   - dispose(): void

For slow operations, consider using AsyncWorker:
- Actually, param set/get is fast, keep synchronous
- Pipeline creation might be slow, consider async

Example usage:
```javascript
const { loadRaw, createPipeline } = require('@app/dtpipe');
const img = loadRaw('/path/to/image.cr2');
const pipe = createPipeline(img);
pipe.setParam('exposure', 'exposure', 1.5);
pipe.enableModule('filmic', true);
// ... later
pipe.dispose();
img.dispose();
```
```

**Verification:** Create pipeline, set params, verify with get.

---

### Task 6.4: Wrap Render with SharedArrayBuffer

- [x] **Status:** Complete
- **Input:** dtpipe.h
- **Output:** `node/src/addon.cc` (extended)

**Claude Code Prompt:**
```
Add render function to the Node.js addon.

Implement:
Pipeline.render(scale: number): RenderResult
Pipeline.renderRegion(x, y, width, height, scale): RenderResult

RenderResult:
- buffer: SharedArrayBuffer (RGBA data)
- width: number
- height: number
- dispose(): void

Using SharedArrayBuffer allows:
- Zero-copy transfer to renderer process
- Direct use in WebGL textures
- Efficient display updates

Implementation:
1. Call dtpipe_render()
2. Create SharedArrayBuffer of correct size
3. Copy RGBA data into it (or use it directly if possible)
4. Return RenderResult object

Use AsyncWorker since render is slow:
```javascript
const result = await pipe.render(0.25);  // Returns Promise
// Send to renderer process
ipcMain.handle('render', async () => ({
  buffer: result.buffer,  // SharedArrayBuffer transfers efficiently
  width: result.width,
  height: result.height
}));
```
```

**Verification:** Render test image, verify buffer dimensions and basic pixel values.

---

### Task 6.5: Wrap Export Functions

- [x] **Status:** Complete
- **Input:** dtpipe.h
- **Output:** `node/src/addon.cc` (extended)

**Claude Code Prompt:**
```
Add export functions to the Node.js addon.

Implement as async (these are slow):
- Pipeline.exportJpeg(path: string, quality?: number): Promise<void>
- Pipeline.exportPng(path: string): Promise<void>
- Pipeline.exportTiff(path: string, bits?: number): Promise<void>

Use AsyncWorker:
1. Capture parameters in constructor
2. Do export in Execute() (off main thread)
3. Resolve/reject promise in OnOK()/OnError()

Example usage:
```javascript
const pipe = createPipeline(img);
pipe.setParam('exposure', 'exposure', 1.5);

// Export runs in background
await pipe.exportJpeg('/output/photo.jpg', 95);
console.log('Export complete!');
```
```

**Verification:** Export test image to each format, verify files.

---

### Task 6.6: Write TypeScript Declarations

- [x] **Status:** Complete
- **Input:** addon.cc
- **Output:** `node/types/index.d.ts`

**Claude Code Prompt:**
```
Write TypeScript declarations for the dtpipe addon.

Based on the implemented API:

```typescript
declare module '@app/dtpipe' {
  export function init(dataDir: string): void;
  export function cleanup(): void;
  
  export function loadRaw(path: string): Image;
  export function createPipeline(image: Image): Pipeline;
  
  export function getModuleCount(): number;
  export function getModuleName(index: number): string;
  
  export class Image {
    readonly width: number;
    readonly height: number;
    readonly cameraMaker: string;
    readonly cameraModel: string;
    dispose(): void;
  }
  
  export class Pipeline {
    setParam(module: string, param: string, value: number): void;
    getParam(module: string, param: string): number;
    enableModule(module: string, enabled: boolean): void;
    isModuleEnabled(module: string): boolean;
    
    render(scale: number): Promise<RenderResult>;
    renderRegion(x: number, y: number, width: number, height: number, scale: number): Promise<RenderResult>;
    
    exportJpeg(path: string, quality?: number): Promise<void>;
    exportPng(path: string): Promise<void>;
    exportTiff(path: string, bits?: number): Promise<void>;
    
    serializeHistory(): string;
    loadHistory(json: string): void;
    loadXmp(path: string): void;
    saveXmp(path: string): void;
    
    dispose(): void;
  }
  
  export interface RenderResult {
    readonly buffer: SharedArrayBuffer;
    readonly width: number;
    readonly height: number;
    dispose(): void;
  }
}
```

Include JSDoc comments for all exports.
```

**Verification:** TypeScript compiles against declarations, IDE shows completions.

---

## Phase 7: Testing

> **Goal:** Ensure correctness and catch regressions.

### Task 7.1: Create C Test Harness

- [x] **Status:** Complete
- **Input:** dtpipe.h
- **Output:** `tests/test_main.c`

**Claude Code Prompt:**
```
Create a C test harness for libdtpipe.

Use a simple test framework (greatest.h, minunit, or similar single-header).

Test suites:
1. test_init: init/cleanup cycles, double init handling
2. test_load: load RAW, verify metadata, load invalid file
3. test_pipeline: create, set params, verify params
4. test_render: render at various scales, verify dimensions
5. test_export: export to each format, verify files exist
6. test_history: serialize/deserialize round-trip

Each test should:
- Set up preconditions
- Call function
- Verify postconditions
- Clean up

Include test images in tests/data/:
- A small RAW file (or create synthetic test data)

Make tests runnable via: ./build/tests/test_main
```

**Verification:** Tests pass, valgrind shows no leaks.

---

### Task 7.2: Create Reference Renders

- [ ] **Status:** Not Started
- **Input:** Test RAW files
- **Output:** `tests/reference/` directory

**Claude Code Prompt:**
```
Create a script to generate reference renders for regression testing.

Script should:
1. Load each test RAW
2. Apply a standard set of edits:
   - Preset A: exposure +1, filmic defaults
   - Preset B: exposure -0.5, high contrast
   - Preset C: specific module combination
3. Render at scale 0.25
4. Save as PNG to tests/reference/

File naming: {raw_name}_{preset}.png

Also save the JSON history for each preset.

This creates ground truth for regression tests.
Run once to establish baseline, re-run when intentionally changing processing.
```

**Verification:** Reference images exist and look correct.

---

### Task 7.3: Implement Regression Tests

- [ ] **Status:** Not Started
- **Input:** Reference renders
- **Output:** `tests/test_regression.c`

**Claude Code Prompt:**
```
Implement image comparison regression tests.

test_regression should:
1. Load test RAW
2. Apply preset history
3. Render
4. Compare against reference image
5. Allow small differences (floating point variance)

Comparison algorithm:
- Convert both images to float
- Compute per-pixel absolute difference
- Compute mean/max difference
- Fail if max > threshold (e.g., 1/255)
- Warn if mean > smaller threshold

Helper functions:
- load_png(path) -> float buffer
- compare_images(a, b, threshold) -> pass/fail + stats

On failure:
- Save difference image for debugging
- Report which pixels differ most
```

**Verification:** Tests pass with reference images, fail when processing changed.

---

### Task 7.4: Write Node.js Tests

- [ ] **Status:** Not Started
- **Input:** TypeScript declarations
- **Output:** `node/test/test.ts`

**Claude Code Prompt:**
```
Write Jest tests for the Node.js addon.

Test categories:

1. Module loading
   - Can require('@app/dtpipe')
   - init() succeeds with valid data dir
   - init() fails with invalid data dir

2. Image loading
   - loadRaw() returns Image
   - Image has correct dimensions
   - loadRaw() throws on invalid file
   - dispose() works, double dispose doesn't crash

3. Pipeline operations
   - createPipeline() returns Pipeline
   - setParam/getParam round-trip
   - enableModule/isModuleEnabled
   - dispose() works

4. Rendering
   - render() returns SharedArrayBuffer
   - Correct dimensions at various scales
   - renderRegion() returns correct size

5. Export
   - exportJpeg creates file
   - exportPng creates file
   - exportTiff creates file

6. History
   - serializeHistory() returns valid JSON
   - loadHistory() applies settings
   - Round-trip preserves settings

Use async/await for async tests.
Set up/tear down test fixtures properly.
```

**Verification:** `npm test` passes all tests.

---

## Session Planning

Suggested work sessions (1-3 hours each):

| Session | Tasks | Goal |
|---------|-------|------|
| 1 | 0.1-0.4 | Complete analysis phase |
| 2 | 1.1-1.3 | Build system working |
| 3 | 2.1-2.2 | IOP stripping template |
| 4 | 2.3 | Tier 1 modules stripped |
| 5 | 2.4 | Tier 2 modules stripped |
| 6 | 3.1-3.2 | Core headers and init |
| 7 | 3.3 | IOP order ported |
| 8 | 3.4-3.5 | Pixelpipe ported |
| 9 | 4.1-4.2 | API header and loading |
| 10 | 4.3-4.4 | Pipeline and params |
| 11 | 4.5-4.6 | Render and export |
| 12 | 5.1-5.3 | JSON history |
| 13 | 5.4-5.5 | XMP support |
| 14 | 6.1-6.3 | Node addon basics |
| 15 | 6.4-6.6 | Node render and types |
| 16 | 7.1-7.4 | Testing |

---

## Appendix: Claude Code Tips

### Effective Prompting

**DO:**
- Paste relevant source code directly in prompts
- Include header files for context
- Specify exact function signatures
- Give examples of expected output
- Ask for one thing at a time

**DON'T:**
- Ask to "port the pixelpipe" (too vague)
- Assume Claude remembers previous sessions
- Skip providing type definitions
- Expect perfect output first try

### Debugging with Claude Code

When code doesn't compile:
```
This code fails to compile with:
[paste error]

The relevant types are:
[paste struct definitions]

Fix the issue.
```

When tests fail:
```
This test fails:
[paste test]

Expected: [expected]
Got: [actual]

The function being tested:
[paste function]

Find the bug.
```

### Iterating on Output

First pass:
```
Implement X...
```

Refinement:
```
The previous implementation has this issue: [issue]

Here's the current code:
[paste code]

Fix [specific problem].
```

Keep chat context if possible—Claude works better with history.

---

## Tracking Progress

Copy this to track completion:

```markdown
## Progress Tracker

### Phase 0: Analysis
- [ ] 0.1 Map IOP Dependencies
- [ ] 0.2 Identify Core Modules  
- [ ] 0.3 Map common/ Dependencies
- [ ] 0.4 Extract Param Schemas

### Phase 1: Build System
- [ ] 1.1 Create Scaffold
- [ ] 1.2 Write CMakeLists
- [ ] 1.3 Integrate rawspeed

### Phase 2: Strip IOPs
- [ ] 2.1 Create Stripping Script
- [ ] 2.2 Strip Template (exposure)
- [ ] 2.3 Strip Tier 1 Modules
- [ ] 2.4 Strip Tier 2 Modules

### Phase 3: Core Infrastructure
- [ ] 3.1 Write darktable.h
- [ ] 3.2 Implement init()
- [ ] 3.3 Port iop_order.c
- [ ] 3.4 Port pixelpipe (structures)
- [ ] 3.5 Port pixelpipe (processing)

### Phase 4: Public API
- [ ] 4.1 Define Header
- [ ] 4.2 Implement Loading
- [ ] 4.3 Implement Pipeline
- [ ] 4.4 Implement Params
- [ ] 4.5 Implement Render
- [ ] 4.6 Implement Export

### Phase 5: History
- [ ] 5.1 Design JSON Format
- [ ] 5.2 Implement Serialize
- [ ] 5.3 Implement Deserialize
- [ ] 5.4 Implement XMP Read
- [ ] 5.5 Implement XMP Write

### Phase 6: Node Addon
- [ ] 6.1 Create Scaffold
- [ ] 6.2 Wrap Loading
- [ ] 6.3 Wrap Pipeline
- [ ] 6.4 Wrap Render
- [x] 6.5 Wrap Export
- [ ] 6.6 TypeScript Types

### Phase 7: Testing
- [ ] 7.1 C Test Harness
- [ ] 7.2 Reference Renders
- [ ] 7.3 Regression Tests
- [ ] 7.4 Node.js Tests
```
