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

- [x] **Status:** Complete
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

- [x] **Status:** Complete
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

- [x] **Status:** Complete
- **Input:** TypeScript declarations
- **Output:** `node/test/test.cts`

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

## Phase 8: Wire Up Real IOP Processing

> **Goal:** Make the pipeline actually process images. Currently all 8 registered IOPs have `process_plain = NULL` (stubs), so the pipeline produces raw sensor passthrough — no demosaicing, no color conversion, no exposure adjustment. This phase ports the GUI-stripped IOP source files from `src/iop/` into `libdtpipe/src/iop/`, adapts them to compile against `dtpipe_internal.h`, and wires them into the pipeline engine.

> **Important context:** Phase 2 stripped GUI code from the IOPs *in place* in `src/iop/`. Those stripped files still `#include` darktable-internal headers (`develop/imageop.h`, `common/opencl.h`, etc.) that don't exist in `libdtpipe/`. The work here is to adapt each IOP to compile standalone against `dtpipe_internal.h`, then register it with a real `process_plain` pointer so the pipeline engine calls it.

> **Scope:** Tier 1 modules only (the minimum viable RAW → RGB pipeline): `rawprepare`, `temperature`, `highlights`, `demosaic`, `exposure`, `colorin`, `colorout`, `sharpen`. These are the 8 modules already registered as stubs in `init.c`.

### Task 8.1: Extend Pipeline Engine for Real Modules

- [ ] **Status:** Not started
- **Input:** `libdtpipe/src/pipe/pixelpipe.c`, `libdtpipe/src/pipe/create.c`, `libdtpipe/src/dtpipe_internal.h`
- **Output:** Updated engine that calls `init_pipe()`, `commit_params()`, `cleanup_pipe()`, and supports colorspace transitions and `output_format()`

**Background — what's missing:**

The current pipeline engine cannot run real IOP modules because of these gaps:

1. **`piece->data` is never allocated** — Real IOPs store their runtime parameters in `piece->data` (allocated by `init_pipe()`, populated by `commit_params()`). Currently `piece->data` is always `NULL`, so any IOP that reads it will segfault.

2. **`commit_params()` is never called** — In darktable, `commit_params()` copies/transforms `module->params` into `piece->data` before each render. Without this call, even if `piece->data` were allocated, it would contain zeros instead of the user's parameter values.

3. **Function pointers missing from `dt_iop_module_so_t`** — The SO struct only has `process_plain`, `flags`, and `operation_tags`. It needs: `init_pipe`, `cleanup_pipe`, `commit_params`, `init` (for defaults), `input_colorspace`, `output_colorspace`, `output_format`, `modify_roi_in`, `modify_roi_out`.

4. **`module->dev` is NULL** — Several IOPs dereference `self->dev` for image metadata (exposure bias, white balance coefficients). A minimal `dt_develop_t` or equivalent must exist.

5. **Colorspace transforms are no-ops** — `dt_ioppr_transform_image_colorspace()` is an inline stub. For colorin/colorout to work, this needs a real lcms2-based implementation.

6. **`output_format()` is never called** — Demosaic changes buffer format from 1-channel Bayer to 4-channel RGBA. The engine calls `module->output_format()` if set, but this function pointer is never populated.

**Claude Code Prompt:**
```
The libdtpipe pipeline engine needs several additions to support real IOP modules.
Currently all 8 registered modules have process_plain = NULL (stubs). Before we
can wire up real process functions, the engine infrastructure must support:

1. Module lifecycle callbacks (init_pipe, cleanup_pipe, commit_params, init)
2. Colorspace declarations (input_colorspace, output_colorspace)  
3. Format transitions (output_format)
4. ROI modifications (modify_roi_in, modify_roi_out)

Make these changes:

A. Add function pointer fields to dt_iop_module_so_t in dtpipe_internal.h:
   - void (*init)(dt_iop_module_t *self);
   - void (*init_pipe)(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                       dt_dev_pixelpipe_iop_t *piece);
   - void (*cleanup_pipe)(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece);
   - void (*commit_params)(dt_iop_module_t *self, dt_iop_params_t *params,
                           dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
   - dt_iop_colorspace_type_t (*input_colorspace)(dt_iop_module_t *self,
                                dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
   - dt_iop_colorspace_type_t (*output_colorspace)(dt_iop_module_t *self,
                                dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
   - int (*output_format)(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece, dt_iop_buffer_dsc_t *dsc);
   - void (*modify_roi_in)(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                           const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in);
   - void (*modify_roi_out)(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                            dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in);

B. Update create.c _build_module_list() to copy these function pointers from
   so to module. Also call so->init(module) if set (to populate default params).

C. Update create.c dtpipe_create() to call init_pipe() for each module's piece
   after dt_dev_pixelpipe_add_node(). This allocates piece->data.

D. Update pixelpipe.c _process_rec() to call commit_params() for each enabled
   module before calling _process_on_cpu(). This populates piece->data from
   module->params.

E. Update pixelpipe.c to use module->input_colorspace() and 
   module->output_colorspace() when available (already partially there — just
   ensure the function pointers are called correctly).

F. Update pixelpipe.c to use module->output_format() when available, to handle
   demosaic's 1-channel → 4-channel transition.

G. Update dt_dev_pixelpipe_cleanup_nodes() to call cleanup_pipe() for each
   module's piece (to free piece->data).

H. Change the iop_registration_t typedef in init.c to use a more capable
   init function signature. The init_fn should receive dt_iop_module_so_t*
   and populate all relevant function pointers (process_plain, init_pipe,
   cleanup_pipe, commit_params, colorspace functions, etc.).

Read these files for the current state:
- libdtpipe/src/dtpipe_internal.h (types)
- libdtpipe/src/pipe/pixelpipe.c (engine)
- libdtpipe/src/pipe/create.c (module instantiation)
- libdtpipe/src/init.c (registration)

Ensure all existing tests still pass after these changes. The 8 stub modules
(with all function pointers NULL) should continue to work — the engine must
gracefully skip NULL function pointers.
```

**Verification:**
```bash
cmake --build libdtpipe/build-release -j8
cd libdtpipe/build-release && ctest --output-on-failure
```
All 9 existing tests pass. The engine now has the infrastructure for real modules but still runs stubs.

---

### Task 8.2: Add GLib/Darktable Compatibility Layer to dtpipe_internal.h

- [ ] **Status:** Not started
- **Input:** `libdtpipe/src/dtpipe_internal.h`, darktable source headers
- **Output:** Updated `dtpipe_internal.h` with compatibility typedefs, macros, and stub functions

**Background:**

The stripped IOPs still reference GLib types, darktable utility functions, and darktable macros that don't exist in `dtpipe_internal.h`. Rather than modifying every IOP source file, add a compatibility layer to `dtpipe_internal.h` that provides these. This is purely additive — no existing code changes.

**Claude Code Prompt:**
```
Add a compatibility layer to libdtpipe/src/dtpipe_internal.h for compiling
darktable IOP modules. These IOPs were GUI-stripped but still reference
darktable types, macros, and utility functions. Add the minimum needed to
compile Tier 1 IOPs (rawprepare, temperature, demosaic, exposure, colorin,
colorout, highlights, sharpen).

Add these IN the dtpipe_internal.h file (or a new dtpipe_compat.h included
by dtpipe_internal.h if it's cleaner):

1. GLib type replacements:
   - typedef int gboolean;
   - #define TRUE 1
   - #define FALSE 0
   - typedef int gint;
   - typedef unsigned int guint;
   - typedef char gchar;
   - static inline char *g_strlcpy(...) — use strlcpy or strncpy

2. Gettext no-ops:
   - #define _(s) (s)
   - #define N_(s) (s)
   - #define C_(ctx, s) (s)

3. Darktable logging stubs (no-op or fprintf):
   - static inline void dt_control_log(const char *fmt, ...) { }
   - #define dt_print(...) do { } while(0)
   - #define dt_print_pipe(...) do { } while(0)
   - static inline void dt_iop_set_module_trouble_message(
       dt_iop_module_t *self, const char *msg, const char *tooltip,
       const char *stderr_msg) { if(stderr_msg) fprintf(stderr, "%s\n", stderr_msg); }

4. Image query helpers (implement using dt_image_t fields):
   - static inline gboolean dt_image_is_raw(const dt_image_t *img)
     { return (img->flags & DT_IMAGE_RAW) != 0; }
   - static inline gboolean dt_image_is_monochrome(const dt_image_t *img)
     { return (img->flags & DT_IMAGE_MONOCHROME) != 0; }
   - static inline gboolean dt_image_is_rawprepare_supported(const dt_image_t *img)
     { return dt_image_is_raw(img); }
   - #define dt_is_valid_imgid(id) ((id) > 0)

5. Config stubs (return sensible defaults):
   - static inline const char *dt_conf_get_string_const(const char *key)
     { return ""; }
   - static inline gboolean dt_conf_get_bool(const char *key)
     { return FALSE; }
   - static inline int dt_conf_get_int(const char *key)
     { return 0; }

6. IOP description stub:
   - static inline void dt_iop_set_description(dt_iop_module_so_t *so,
       const char *main, const char *purpose, const char *input,
       const char *output) { }

7. GUI preset stubs:
   - static inline void dt_gui_presets_add_generic(const char *name,
       ...) { }   (variadic no-op)
   - #define BUILTIN_PRESET(...) do { } while(0)

8. dt_dev_chroma_t struct (needed by temperature, highlights):
   - typedef struct dt_dev_chroma_t {
       float D65coeffs[4];
       float as_shot[4];
       float wb_coeffs[4]; 
       int adaptation;
       int mode;
     } dt_dev_chroma_t;

9. Minimal dt_develop_t struct (pointed to by module->dev):
   - typedef struct dt_develop_t {
       dt_image_t image_storage;
       dt_dev_chroma_t chroma;
     } dt_develop_t;

10. Add DT_IMAGE_RAW, DT_IMAGE_MONOCHROME flag constants to the image flags
    enum if not already present.

11. Scene-referred check stub:
    - static inline gboolean dt_is_scene_referred(void) { return TRUE; }

12. Signal macros (no-op):
    - #define DT_DEBUG_CONTROL_SIGNAL_RAISE(...) do { } while(0)

Read the current dtpipe_internal.h first, then ADD these definitions.
Do NOT modify existing definitions — only add new ones. Ensure no duplicate
definitions. Keep the file organized with clear section comments.

Verify with: cmake --build libdtpipe/build-release -j8
(should still compile and all tests pass)
```

**Verification:** `cmake --build` succeeds, all existing tests pass, `grep` confirms all needed types/macros are defined.

---

### Task 8.3: Port IOP Math and Utility Helpers

- [ ] **Status:** Not started
- **Input:** `src/develop/imageop_math.h`, `src/common/imagebuf.h`, `src/common/math.h`
- **Output:** `libdtpipe/src/iop/iop_math.h` — self-contained header with ported math functions

**Background:**

Multiple IOPs call math/utility functions from darktable's `develop/imageop_math.h`, `common/imagebuf.h`, and `common/math.h`. These are mostly small inline functions. They should be extracted into a single self-contained header in libdtpipe.

**Claude Code Prompt:**
```
Create libdtpipe/src/iop/iop_math.h containing math and utility functions
needed by the Tier 1 IOPs. Port these from the darktable source tree.

Functions needed (extract from the darktable headers listed below):

From src/develop/imageop_math.h:
- FC(row, col, filters) macro — Bayer color filter pattern lookup
- FCxtrans(row, col, xtrans) macro — X-Trans pattern lookup
- dt_iop_eval_exp(coeff, x) — power-law evaluation: coeff[1] * powf(x * coeff[0], coeff[2])
- dt_iop_estimate_exp(x, y, num, coeff) — fit power-law to samples
- dt_iop_clip_and_zoom_roi() — crop/zoom a float buffer by ROI
- dt_iop_clip_and_zoom() — simpler crop/zoom

From src/common/imagebuf.h:
- dt_iop_image_copy_by_size() — memcpy with size calculation
- dt_iop_copy_image_roi() — copy with ROI
- dt_iop_image_fill() — fill buffer with constant value
- dt_iop_have_required_input_format() — validate input format, log warning
- dt_iop_alloc_image_buffers() — variadic buffer allocator (simplified)

From src/common/math.h:
- dt_fast_expf() — fast exponential approximation
- dt_log2f() — log base 2

From src/common/darktable.h or equivalent:
- dt_alloc_align_float() — aligned malloc (already in dtpipe_internal.h, just verify)
- dt_free_align() — aligned free (already in dtpipe_internal.h, just verify)
- dt_calloc_align_float() — calloc variant
- dt_alloc_perthread_float() / dt_get_perthread() — per-thread buffers (already in dtpipe_internal.h, verify)

From src/develop/tiling.h:
- dt_iop_default_tiling_callback() — default tiling params (already stubbed? verify)

Implementation requirements:
- Pure C, self-contained header (static inline where possible)
- Include only <math.h>, <string.h>, <stdlib.h>, <stdio.h>, <stdint.h>
- Include "dtpipe_internal.h" for types like dt_iop_roi_t, dt_iop_module_t
- No GLib, no GTK, no OpenCL
- Use DT_OMP_FOR() / DT_OMP_FOR_SIMD() macros for parallel loops (already defined in dtpipe_internal.h)

Read the source darktable headers to get the exact implementations:
- src/develop/imageop_math.h
- src/common/imagebuf.h  
- src/common/math.h

Then create a clean, self-contained iop_math.h. Test: the header should
compile when included after dtpipe_internal.h.
```

**Verification:** `echo '#include "dtpipe_internal.h"\n#include "iop/iop_math.h"' | gcc -fsyntax-only -x c -` compiles without errors.

---

### Task 8.4: Port exposure.c (Template Module)

- [ ] **Status:** Not started
- **Input:** `src/iop/exposure.c` (GUI-stripped), `libdtpipe/src/iop/iop_math.h`
- **Output:** `libdtpipe/src/iop/exposure.c` — compiles and processes pixels

**Background:**

Exposure is the simplest Tier 1 IOP — its `process()` function is a single linear transform: `out[k] = (in[k] - black) * scale`. It has minimal external dependencies and serves as the template for porting all other IOPs.

**Claude Code Prompt:**
```
Port the exposure IOP from src/iop/exposure.c into libdtpipe/src/iop/exposure.c.

The source file has already been GUI-stripped (no GTK, no bauhaus). Your job
is to adapt it to compile against dtpipe_internal.h instead of darktable's
scattered headers.

Step-by-step:

1. Copy src/iop/exposure.c to libdtpipe/src/iop/exposure.c

2. Replace all #include directives with:
   #include "dtpipe_internal.h"
   #include "iop/iop_math.h"
   #include <math.h>
   #include <string.h>
   #include <stdlib.h>

3. Remove or stub these items:
   - Remove init_presets() if present (GUI presets)
   - Remove reload_defaults() (references self->dev->image_storage which
     may not be populated — use static defaults instead)
   - Remove any dt_opencl_* code and the entire process_cl() function
   - Remove #ifdef HAVE_OPENCL blocks entirely
   - Remove dt_control_log() calls (or they'll hit the no-op stub)
   - Remove deflicker mode logic from commit_params() (it requires raw
     histogram infrastructure we don't have). Set d->deflicker = 0 always.

4. Keep these functions exactly (adapt only includes/types):
   - process() — the pixel loop: out[k] = (in[k] - black) * scale
   - _process_common_setup() — computes d->scale and d->black from params
   - commit_params() — copies params into piece->data (simplified: no
     exposure bias compensation, no highlight preservation compensation,
     no deflicker — just copy the params and let _process_common_setup
     handle the math)
   - init_pipe() — calloc(1, sizeof(dt_iop_exposure_data_t))
   - cleanup_pipe() — free(piece->data)
   - init() — set default params (exposure=0.0, black=0.0, mode=MANUAL)

5. Add an init_global function that registers all function pointers:
   void dt_iop_exposure_init_global(dt_iop_module_so_t *so)
   {
     so->process_plain = process;
     so->init = init;
     so->init_pipe = init_pipe;
     so->cleanup_pipe = cleanup_pipe;
     so->commit_params = commit_params;
     so->input_colorspace = input_colorspace;  // returns IOP_CS_RGB
     so->output_colorspace = output_colorspace; // returns IOP_CS_RGB
   }

6. Define dt_iop_exposure_params_t and dt_iop_exposure_data_t locally in
   the file (they must match the layout in pipe/params.c exactly):
   
   typedef struct {
     int mode;        // 0 = manual
     float black;
     float exposure;
     float deflicker_percentile;
     float deflicker_target_level;
     int compensate_exposure_bias;   // was gboolean
     int compensate_hilite_pres;     // was gboolean
   } dt_iop_exposure_params_t;

   typedef struct {
     dt_iop_exposure_params_t params;
     int deflicker;
     float black;
     float scale;
   } dt_iop_exposure_data_t;

7. Add this file to libdtpipe/src/CMakeLists.txt DTPIPE_SOURCES.

8. Update the "exposure" entry in init.c _iop_registry[] to point to
   dt_iop_exposure_init_global instead of NULL.

9. Build and test:
   cmake --build libdtpipe/build-release -j8
   cd libdtpipe/build-release && ctest --output-on-failure

Read these files:
- src/iop/exposure.c (the GUI-stripped source to port)
- libdtpipe/src/dtpipe_internal.h (target type definitions)
- libdtpipe/src/pipe/params.c (exposure descriptor table — params must match)
- libdtpipe/src/init.c (registration)
- libdtpipe/src/CMakeLists.txt (build)
```

**Verification:**
```bash
cmake --build libdtpipe/build-release -j8  # compiles without errors
cd libdtpipe/build-release && ctest --output-on-failure  # all tests pass
```
Additionally, a rendered image should now show exposure adjustment when exposure param is changed (no longer raw passthrough for exposure).

---

### Task 8.5: Port rawprepare.c

- [ ] **Status:** Not started
- **Input:** `src/iop/rawprepare.c` (GUI-stripped)
- **Output:** `libdtpipe/src/iop/rawprepare.c`

**Background:**

rawprepare is the first module in the pipeline. It subtracts the sensor black level and normalizes to [0, 1] range. It operates on single-channel (Bayer/X-Trans) data. Uses `FC()` macro from `iop_math.h`.

**Claude Code Prompt:**
```
Port src/iop/rawprepare.c into libdtpipe/src/iop/rawprepare.c following the
same pattern established by exposure.c (Task 8.4).

Key differences from exposure:
- Operates on IOP_CS_RAW (single-channel Bayer data), not IOP_CS_RGB
- Uses the FC() macro to determine which color channel each pixel belongs to
- Applies per-channel black subtraction and white balance scaling
- Must set piece->pipe->dsc.processed_maximum[] after processing

Steps:
1. Copy src/iop/rawprepare.c to libdtpipe/src/iop/rawprepare.c
2. Replace includes with dtpipe_internal.h + iop/iop_math.h
3. Remove OpenCL code (process_cl, #ifdef HAVE_OPENCL blocks)
4. Remove reload_defaults() (references image cache, EXIF black levels)
5. Keep: process(), commit_params(), init_pipe(), cleanup_pipe(), init()
6. Keep: modify_roi_in(), modify_roi_out() (rawprepare adjusts ROI for
   sensor crop — this is important for correct buffer dimensions)
7. Keep: output_format() if present (rawprepare may set format metadata)
8. Define params and data structs locally (must match params.c descriptors)
9. Write dt_iop_rawprepare_init_global() to register all function pointers
10. Add to CMakeLists.txt and update init.c registry entry

Important: rawprepare accesses self->dev->image_storage for the raw black/white
point values. The minimal dt_develop_t (added in Task 8.2) must be populated
with these values. Check how create.c sets up module->dev.

Note: rawprepare also calls dt_rawspeed_crop_dcraw_filters() to adjust the
CFA filter pattern for sensor crop. This function must be stubbed or ported.
It lives in src/common/rawspeed_glue.c. A simplified version that returns the
unmodified filters value may suffice initially.

Read: src/iop/rawprepare.c, src/common/rawspeed_glue.c (for crop_dcraw_filters)
```

**Verification:** Compiles, tests pass, rawprepare correctly normalizes the raw data (black subtraction + scaling).

---

### Task 8.6: Port temperature.c

- [ ] **Status:** Not started
- **Input:** `src/iop/temperature.c` (GUI-stripped)
- **Output:** `libdtpipe/src/iop/temperature.c`

**Background:**

Temperature (white balance) applies per-channel multipliers to the raw Bayer data. It operates in IOP_CS_RAW. The process function is simple (per-pixel channel multiply), but `commit_params()` is complex — it converts color temperature / tint into per-channel coefficients using the camera's color matrix.

**Claude Code Prompt:**
```
Port src/iop/temperature.c into libdtpipe/src/iop/temperature.c.

Key characteristics:
- Operates on IOP_CS_RAW (before demosaic)
- process() is simple: multiply each pixel by its channel's coefficient
  using the FC() macro to determine which coefficient to apply
- commit_params() converts temperature/tint to per-channel multipliers
  using camera color matrices from EXIF data and dt_dev_chroma_t
- Uses self->dev->chroma for white balance coefficients

Simplification strategy:
- Keep process() as-is (simple per-pixel multiply)
- Simplify commit_params() to use the as-shot white balance coefficients
  from dev->chroma.as_shot[] as defaults, and allow override via params
- Remove the complex temperature-to-coefficient conversion that requires
  color matrices (can be added later)
- Remove preset logic and reload_defaults()
- Remove OpenCL code

The init() function should set default params to use as-shot WB (the camera's
auto white balance from EXIF). This means commit_params() can just copy
coefficients from dev->chroma into piece->data.

Important: create.c must populate module->dev with a dt_develop_t* that has
chroma.as_shot[] filled from the loaded image's EXIF WB data. Check how
dtpipe_load_raw() populates dt_image_t and trace how WB coefficients flow.

Read: src/iop/temperature.c (the process function and commit_params)
```

**Verification:** Compiles, tests pass, white balance is applied (neutral gray targets should appear neutral).

---

### Task 8.7: Port demosaic.c (Core Only)

- [ ] **Status:** Not started
- **Input:** `src/iop/demosaic.c` (GUI-stripped), `src/iop/demosaicing/` subdirectory
- **Output:** `libdtpipe/src/iop/demosaic.c`, `libdtpipe/src/iop/demosaicing/` files

**Background:**

Demosaic is the most complex Tier 1 module. It interpolates the single-channel Bayer (or X-Trans) sensor data into full-color RGBA. The main file `#include`s implementation files from `demosaicing/` for different algorithms. This is the critical format-transition module: input is 1-channel raw, output is 4-channel float RGBA.

**Claude Code Prompt:**
```
Port the demosaic IOP into libdtpipe. This is the most complex Tier 1 module.

The demosaic module:
- Input: 1-channel Bayer raw data (IOP_CS_RAW)
- Output: 4-channel float RGBA (IOP_CS_RGB)
- Uses output_format() to declare the format transition
- Contains multiple demosaicing algorithms in src/iop/demosaicing/

Strategy — port incrementally:

Phase A: Port with PPG algorithm only (simplest Bayer demosaicing)
1. Copy src/iop/demosaic.c to libdtpipe/src/iop/demosaic.c
2. Copy src/iop/demosaicing/basics.c to libdtpipe/src/iop/demosaicing/basics.c
   (contains the PPG implementation and passthrough_monochrome)
3. Replace includes with dtpipe_internal.h + iop/iop_math.h
4. Remove OpenCL code entirely
5. Remove X-Trans code paths initially (#ifdef out or remove)
6. Remove algorithms we don't need yet: amaze, lmmse, rcd, vng, dual, capture
   (just keep ppg from basics.c)
7. In process(), default to PPG for all Bayer demosaicing requests
8. Keep output_format() — this is CRITICAL: it must set dsc->channels = 4
   and dsc->datatype = TYPE_FLOAT to signal the format transition
9. Keep modify_roi_in() — demosaic needs border pixels for interpolation

Phase B (future): Add RCD, VNG, AMaZE algorithms

The key functions to keep:
- process() — dispatches to the selected algorithm
- output_format() — sets channels=4 (the 1→4 channel transition)
- commit_params() — selects algorithm, computes parameters  
- init_pipe() / cleanup_pipe()
- modify_roi_in() — requests extra border pixels from upstream
- input_colorspace() → IOP_CS_RAW
- output_colorspace() → IOP_CS_RGB

Define dt_iop_demosaic_params_t and dt_iop_demosaic_data_t locally.
Must match the descriptor table in params.c.

Critical implementation note: The PPG algorithm in basics.c uses the FC()
macro extensively. Ensure iop_math.h provides this. PPG also needs
dt_iop_clip_and_zoom_demosaic_half_size() for scaled processing — port this
or use a simpler fallback at full resolution initially.

Read:
- src/iop/demosaic.c (main dispatcher)
- src/iop/demosaicing/basics.c (PPG implementation)
- src/develop/imageop_math.h (FC macro, clip_and_zoom variants)
```

**Verification:** Compiles, tests pass. Rendered output should now show recognizable image content (actual colors instead of raw Bayer pattern). This is the biggest visual milestone — the output transitions from a gray/green mosaic to a real photograph.

---

### Task 8.8: Port colorin.c and colorout.c with lcms2

- [ ] **Status:** Not started
- **Input:** `src/iop/colorin.c`, `src/iop/colorout.c` (GUI-stripped), `src/common/colorspaces.h`
- **Output:** `libdtpipe/src/iop/colorin.c`, `libdtpipe/src/iop/colorout.c`, `libdtpipe/src/common/colorspaces.c`

**Background:**

colorin converts from camera RGB to the pipeline working colorspace (linear Rec2020 or Lab). colorout converts from the working colorspace to the output profile (sRGB). Both rely heavily on lcms2 (Little CMS) for ICC profile transforms. This is the hardest task in Phase 8.

**Claude Code Prompt:**
```
Port colorin.c and colorout.c into libdtpipe with lcms2 color management.

This is the most complex integration task. These modules:
- Use lcms2 (Little CMS 2) for ICC profile-based color transforms
- Reference dt_colorspaces_get_profile() and related functions from
  src/common/colorspaces.h which manage a profile cache
- Transform between camera RGB, linear Rec2020, Lab, and sRGB

Strategy — implement a minimal colorspace management layer:

1. Create libdtpipe/src/common/colorspaces.h and colorspaces.c:
   - Link against lcms2 (add find_package(LCMS2) or pkg_check_modules to CMake)
   - Implement a minimal profile manager:
     a. dt_colorspaces_get_profile(type, filename, intent) — returns a cached
        cmsHPROFILE. Start with just 3 built-in profiles:
        - DT_COLORSPACE_SRGB → cmsCreate_sRGBProfile()
        - DT_COLORSPACE_LIN_REC2020 → build from primaries
        - DT_COLORSPACE_LIN_REC709 → build from primaries
     b. dt_colorspaces_get_matrix_from_input_profile() — extract 3x3 matrix
     c. dt_colorspaces_get_matrix_from_output_profile() — extract 3x3 matrix
   - Port the essential inline color conversion functions from
     src/common/colorspaces_inline_conversions.h:
     a. dt_XYZ_to_Lab() / dt_Lab_to_XYZ()
     b. dt_XYZ_to_Rec709_D65() / dt_Rec709_D65_to_XYZ()
     c. dt_linearRGB_to_XYZ() / dt_XYZ_to_linearRGB() (with matrix)
     d. dt_apply_transposed_color_matrix() — 3x3 matrix multiply on pixel

2. Port colorin.c:
   - Replace includes with dtpipe_internal.h + common/colorspaces.h
   - Remove OpenCL code
   - Simplify: for initial implementation, use the matrix path only
     (not the full lcms2 transform path). Colorin's fast path extracts
     a 3x3 matrix from the input profile and applies it per-pixel.
   - Keep process(), commit_params(), init_pipe(), cleanup_pipe()
   - input_colorspace() → IOP_CS_RGB (linear camera RGB)
   - output_colorspace() → IOP_CS_LAB (or IOP_CS_RGB depending on config)

3. Port colorout.c:
   - Similar approach — matrix-based sRGB output
   - input_colorspace() → IOP_CS_LAB (or IOP_CS_RGB)
   - output_colorspace() → IOP_CS_RGB

4. Implement dt_ioppr_transform_image_colorspace() as a real function
   (replace the inline stub in dtpipe_internal.h):
   - Use the lcms2 transform objects cached in the module's global_data
   - Or use the 3x3 matrix path for speed

5. Add lcms2 to CMakeLists.txt:
   find_package(PkgConfig REQUIRED)
   pkg_check_modules(LCMS2 REQUIRED lcms2)
   target_link_libraries(dtpipe PRIVATE ${LCMS2_LIBRARIES})
   target_include_directories(dtpipe PRIVATE ${LCMS2_INCLUDE_DIRS})

Read:
- src/iop/colorin.c (especially the matrix fast path in process())
- src/iop/colorout.c
- src/common/colorspaces.h (the profile type enum and function signatures)
- src/common/colorspaces_inline_conversions.h (XYZ/Lab/RGB conversion math)
```

**Verification:** Compiles with lcms2 linked. Tests pass. Rendered output should now show proper colors — correct white balance, correct gamma curve, sRGB output. Before this task, colors will look wrong (linear light, wrong primaries). After this task, the output should be a recognizable photograph with correct-looking colors.

---

### Task 8.9: Port highlights.c and sharpen.c

- [ ] **Status:** Not started
- **Input:** `src/iop/highlights.c`, `src/iop/sharpen.c` (GUI-stripped)
- **Output:** `libdtpipe/src/iop/highlights.c`, `libdtpipe/src/iop/sharpen.c`

**Background:**

These are the two Tier 2 modules already registered as stubs. Highlights reconstructs blown-out highlights in the raw data (before demosaic). Sharpen applies unsharp mask in Lab colorspace (after colorin).

**Claude Code Prompt:**
```
Port highlights.c and sharpen.c into libdtpipe.

Port sharpen.c first (simpler):
1. Copy src/iop/sharpen.c to libdtpipe/src/iop/sharpen.c
2. Replace includes
3. Remove OpenCL code, init_presets()
4. Keep process() — classic unsharp mask on L channel in Lab space
5. Keep commit_params(), init_pipe(), cleanup_pipe()
6. input_colorspace() → IOP_CS_LAB
7. output_colorspace() → IOP_CS_LAB
8. Stub dt_iop_have_required_input_format() if not in iop_math.h
9. Register via dt_iop_sharpen_init_global()

Port highlights.c (complex):
1. Copy src/iop/highlights.c to libdtpipe/src/iop/highlights.c
2. Copy src/iop/hlreconstruct/ directory to libdtpipe/src/iop/hlreconstruct/
   (these .c files are textually #include'd, not separately compiled)
3. Replace includes
4. Remove ALL OpenCL code from highlights.c AND from hlreconstruct/ files
   (opposed.c has process_opposed_cl, laplacian.c has process_laplacian_bayer_cl)
5. Remove GUI remnants: the 'g' variable references at lines ~911 and ~990
   (these are gui_data_t* references that the stripping script missed)
6. Stub or remove: dt_mipmap_cache_get_matching_size(),
   dt_mipmap_cache_get_min_mip_from_pref(), dt_conf_get_string_const()
   (these are used to decide mask resolution — use sensible defaults)
7. Stub: dt_iop_piece_is_raster_mask_used() → return FALSE
   dt_iop_piece_set_raster() / dt_iop_piece_clear_raster() → no-op
8. Port or stub: dt_gaussian_fast_blur() (from common/box_filters.h),
   dt_box_mean() (from common/box_filters.h — needed by hlreconstruct)
9. Port or stub: dt_image_distance_transform(), dt_noise_generator_simd(),
   dt_interpolation_new/resample functions
10. For initial port: support only DT_IOP_HIGHLIGHTS_CLIP mode (simple
    per-pixel clamp). Stub the complex reconstruction algorithms to fall
    through to clip mode. This avoids needing box_filters, interpolation,
    noise_generator, distance_transform dependencies.
11. input_colorspace() → IOP_CS_RAW
12. output_colorspace() → IOP_CS_RAW
13. Register via dt_iop_highlights_init_global()

Add both to CMakeLists.txt and update init.c registry entries.

Read:
- src/iop/sharpen.c
- src/iop/highlights.c  
- src/iop/hlreconstruct/ files (to understand what can be stubbed)
```

**Verification:** Compiles, tests pass. Sharpen should produce visible sharpening effect (compare render with sharpen enabled vs disabled). Highlights in clip mode should prevent pure-white blowouts in overexposed areas.

---

### Task 8.10: Wire Up module->dev and White Balance Coefficients

- [ ] **Status:** Not started
- **Input:** `libdtpipe/src/pipe/create.c`, `libdtpipe/src/imageio/load.cc`
- **Output:** Updated create.c that populates `module->dev` with image metadata

**Background:**

Several IOPs access `self->dev->image_storage` for EXIF metadata and `self->dev->chroma` for white balance coefficients. Currently `module->dev` is NULL. This task creates a `dt_develop_t` instance in the pipeline and populates it from the loaded image.

**Claude Code Prompt:**
```
Wire up module->dev so that IOPs can access image metadata and white balance
coefficients.

Changes needed:

1. In pipe/create.c, add a dt_develop_t field to struct dt_pipe_s:
   dt_develop_t dev;   // minimal develop struct for IOP access

2. In dtpipe_create(), after loading the image, populate pipe->dev:
   - Copy img->* fields into pipe->dev.image_storage (the embedded dt_image_t)
   - Copy white balance coefficients from the image into pipe->dev.chroma:
     - as_shot[]: from img->wb_coeffs (the camera's auto WB from EXIF)
     - D65coeffs[]: from img->d65_color_matrix or compute from color matrix
   - Set pipe->dev.chroma.adaptation = 0 (as-shot)

3. In _build_module_list(), set m->dev = &pipe->dev for each module instance.

4. Check what fields of dt_image_t are populated by dtpipe_load_raw() in
   imageio/load.cc:
   - width, height (yes)
   - camera_maker, camera_model (yes)
   - exif_exposure_bias (check — needed by exposure's commit_params)
   - wb_coeffs / camera_matrix (check — needed by temperature)
   - flags including DT_IMAGE_RAW (check)
   - filters / xtrans (check — CFA pattern needed by rawprepare, demosaic)

5. If any fields are missing from load.cc, add them. Rawspeed provides:
   - Black levels per channel → img->raw_black_level, raw_black_level_separate[]
   - White point → img->raw_white_point
   - CFA pattern → img->buf_dsc.filters (32-bit Bayer pattern code)
   - White balance multipliers → from EXIF via exiv2

Read:
- libdtpipe/src/pipe/create.c (current pipeline creation)
- libdtpipe/src/imageio/load.cc (what image metadata is extracted)
- libdtpipe/src/dtpipe_internal.h (dt_image_t struct, dt_develop_t)
- src/iop/temperature.c (how it reads dev->chroma)
- src/iop/rawprepare.c (how it reads image black/white levels)
```

**Verification:** No IOPs segfault on `self->dev` access. Temperature module reads correct WB coefficients from the camera.

---

### Task 8.11: Update Tests and Regenerate Reference Renders

- [ ] **Status:** Not started
- **Input:** All ported IOPs from Tasks 8.4–8.10
- **Output:** Updated test suite, new reference renders

**Background:**

With real IOPs processing pixels, the test suite needs updating. The current reference renders are raw passthrough (all identical). The regression tests need new ground-truth images, and the unified test harness needs additional checks for actual pixel processing.

**Claude Code Prompt:**
```
Update the test suite for real IOP processing.

1. Update tests/test_main.c:
   - Add checks that rendered output is NOT identical to raw input
     (pixels should be transformed by the pipeline)
   - Add checks that exposure parameter changes produce visibly different
     output (e.g., exposure=+1.0 should produce brighter pixels than 0.0)
   - Add checks that enabling/disabling sharpen changes the output
   - Verify that the pipeline processes in the correct order
     (rawprepare → temperature → highlights → demosaic → colorin →
      exposure → colorout → sharpen)

2. Regenerate reference renders:
   - Run gen_reference to create new ground-truth PNGs with real processing
   - The three presets should now produce visibly different images:
     * preset_a: exposure +1.0 (brighter)
     * preset_b: exposure -0.5 (darker)  
     * preset_c: exposure 0.0, sharpen disabled
   - Verify the images look correct (not black, not white, recognizable photo)

3. Update tests/test_regression.c if needed:
   - The pixel comparison thresholds may need adjustment for real processing
   - With real IOPs, small floating-point variations are expected across
     platforms (different SSE/NEON paths, different libm implementations)
   - Consider increasing the per-channel tolerance from 1 to 2

4. Run the full test suite:
   cmake --build libdtpipe/build-release -j8
   cd libdtpipe/build-release && ctest --output-on-failure

5. Run the Node.js tests:
   cd node && npm test

Verify all tests pass. The key visual verification is that exported PNGs
from gen_reference show a recognizable photograph (the Fuji GFX 50R test
image) with correct colors, proper exposure, and visible sharpening when
enabled.
```

**Verification:** All C tests pass (9/9). All Node.js tests pass. Reference renders show a recognizable photograph with correct colors. The three presets produce visibly different images (different exposure levels).

---

## Phase 9: Repository Cleanup

> **Goal:** Create a clean, standalone repository containing only the libdtpipe library, Node.js addon, and supporting files. Remove all original darktable source code, build artifacts, and analysis files that are no longer needed.

### Task 9.1: Create Clean Repository

- [ ] **Status:** Not started
- **Input:** Current repository with all phases complete
- **Output:** New clean git repository

**Claude Code Prompt:**
```
Create a clean repository for the libdtpipe project. The current repo is a
fork of the full darktable source tree and contains ~1,890 original darktable
files that should not be shipped.

Steps:

1. Create a new directory: ~/Documents/dev/libdtpipe/

2. Copy these directories/files (the actual project output):
   - libdtpipe/ → libdtpipe/  (the C library — source, tests, cmake, data)
   - node/ → node/  (the Node.js addon)
   - docs/ → docs/  (design documents)
   - test-image/ → test-image/  (if present locally — .gitignore it)

3. Do NOT copy (original darktable / analysis / build artifacts):
   - src/ (original darktable source tree)
   - cmake/, data/, doc/, packaging/, po/, schemas/, tools/ (darktable files)
   - .ci/, .github/, .githooks/ (darktable CI)
   - analysis/ (Phase 0 output — useful but not needed in final repo)
   - scripts/ (strip_iop.py — already used, not needed)
   - build-check/ (build artifacts)
   - include/ (duplicate of libdtpipe/include/)
   - Top-level darktable files: CMakeLists.txt, build.sh, ConfigureChecks.cmake,
     DefineOptions.cmake, RELEASE_NOTES.md, CONTRIBUTING.md
   - darktable-extraction-project-plan.md (planning document)
   - libdtpipe/build/ and libdtpipe/build-release/ (build artifacts)
   - node/build/ and node/node_modules/ (build artifacts)

4. Create a proper .gitignore:
   build*/
   node_modules/
   *.dylib
   *.so
   *.node
   *.o
   test-image/
   .DS_Store

5. Write a new README.md for the project:
   - Project description (standalone RAW processing library)
   - Build instructions (cmake, dependencies)
   - Usage examples (C API, Node.js addon)
   - Supported cameras (via rawspeed)
   - License information

6. Create a clean CLAUDE.md with only the information relevant to the
   standalone project (remove references to darktable source tree, stripping
   scripts, analysis, Phase 0-2 details).

7. Initialize git repo:
   cd ~/Documents/dev/libdtpipe
   git init
   git add .
   git commit -m "Initial commit: libdtpipe standalone RAW processing library

   Extracted from darktable's pixel processing pipeline.
   - libdtpipe: C library for headless RAW→RGB processing
   - node/: Node.js native addon (@app/dtpipe)
   - 8 IOP modules: rawprepare, temperature, highlights, demosaic,
     exposure, colorin, colorout, sharpen
   - Full test suite with regression tests
   - XMP sidecar read/write compatibility with darktable"

Verify the new repo builds and all tests pass from a clean state:
   cd ~/Documents/dev/libdtpipe
   cmake -B libdtpipe/build -S libdtpipe -DCMAKE_BUILD_TYPE=Release
   cmake --build libdtpipe/build -j8
   cd libdtpipe/build && ctest --output-on-failure
```

**Verification:** New repo builds from scratch, all tests pass, no references to original darktable source tree, `git log` shows a single clean initial commit.

---

### Task 9.2: Verify Clean Build from Scratch

- [ ] **Status:** Not started
- **Input:** Clean repository from Task 9.1
- **Output:** Verified build on clean system

**Claude Code Prompt:**
```
Verify the clean libdtpipe repository builds and works from scratch.

1. Remove any cached build directories:
   rm -rf libdtpipe/build libdtpipe/build-release node/build node/node_modules

2. List all external dependencies required and verify they're installed:
   - CMake >= 3.16
   - C11 compiler (clang or gcc)
   - C++17 compiler (for rawspeed, exiv2, pugixml)
   - rawspeed (fetched via FetchContent — no install needed)
   - exiv2 (brew install exiv2)
   - pugixml (brew install pugixml)
   - lcms2 (brew install little-cms2)
   - libjpeg (brew install jpeg)
   - libpng (brew install libpng)
   - libtiff (brew install libtiff)
   - zlib (system)
   - OpenMP (brew install libomp — optional but recommended)

3. Configure and build from scratch:
   cmake -B libdtpipe/build -S libdtpipe -DCMAKE_BUILD_TYPE=Release
   cmake --build libdtpipe/build -j8

4. Run all C tests:
   cd libdtpipe/build && ctest --output-on-failure --timeout 300

5. Build and test Node.js addon:
   cd node && npm install && npm run build && npm test

6. Verify no dangling references to the old darktable source tree:
   grep -r "src/iop/" libdtpipe/ node/ --include='*.c' --include='*.cc' --include='*.h'
   grep -r "src/common/" libdtpipe/ node/ --include='*.c' --include='*.cc' --include='*.h'
   grep -r "src/develop/" libdtpipe/ node/ --include='*.c' --include='*.cc' --include='*.h'
   (These should return zero results)

7. Verify no GLib runtime dependency:
   otool -L libdtpipe/build/src/libdtpipe.dylib | grep -i glib
   (Should return nothing)

Report any issues found.
```

**Verification:** Clean build succeeds from scratch, all tests pass, no dangling references to darktable source tree, no GLib runtime dependency.

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
| 17 | 8.1-8.3 | Engine extensions + compat layer + math helpers |
| 18 | 8.4-8.5 | Port exposure + rawprepare (first real processing) |
| 19 | 8.6-8.7 | Port temperature + demosaic (visible image output) |
| 20 | 8.8 | Port colorin/colorout with lcms2 (correct colors) |
| 21 | 8.9-8.10 | Port highlights/sharpen + wire up module->dev |
| 22 | 8.11 | Update tests + regenerate references |
| 23 | 9.1-9.2 | Clean repository + verify build |

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

### Phase 8: Wire Up Real IOP Processing
- [ ] 8.1 Extend Pipeline Engine for Real Modules
- [ ] 8.2 Add GLib/Darktable Compatibility Layer
- [ ] 8.3 Port IOP Math and Utility Helpers
- [ ] 8.4 Port exposure.c (Template Module)
- [ ] 8.5 Port rawprepare.c
- [ ] 8.6 Port temperature.c
- [ ] 8.7 Port demosaic.c (Core Only)
- [ ] 8.8 Port colorin.c and colorout.c with lcms2
- [ ] 8.9 Port highlights.c and sharpen.c
- [ ] 8.10 Wire Up module->dev and White Balance Coefficients
- [ ] 8.11 Update Tests and Regenerate Reference Renders

### Phase 9: Repository Cleanup
- [ ] 9.1 Create Clean Repository
- [ ] 9.2 Verify Clean Build from Scratch
```
