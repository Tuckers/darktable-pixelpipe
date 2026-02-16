cmake_minimum_required(VERSION 3.22)

project(dtpipe
  VERSION 0.1.0
  DESCRIPTION "Standalone darktable pixelpipe library"
  LANGUAGES C CXX
)

# Module path for custom Find*.cmake files
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Build options
option(DTPIPE_BUILD_TESTS   "Build test suite"               ON)
option(DTPIPE_BUILD_NODE    "Build Node.js addon"            OFF)
option(DTPIPE_ENABLE_OPENCL "Enable OpenCL GPU acceleration" ON)
option(DTPIPE_ENABLE_OPENMP "Enable OpenMP CPU parallelization" ON)

# ── Required dependencies ─────────────────────────────────────────────────────

find_package(LCMS2 REQUIRED)

# JPEG – prefer CMake built-in (3.12+)
find_package(JPEG REQUIRED)

# PNG – CMake built-in
find_package(PNG REQUIRED)

# TIFF – CMake built-in
find_package(TIFF REQUIRED)

# exiv2 ships its own CMake config since 0.27; exports Exiv2::exiv2lib
find_package(exiv2 QUIET CONFIG)
if(NOT exiv2_FOUND)
  # Fallback: pkg-config
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(EXIV2 REQUIRED exiv2)
    add_library(exiv2_imported INTERFACE IMPORTED)
    set_target_properties(exiv2_imported PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${EXIV2_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES      "${EXIV2_LINK_LIBRARIES}"
      INTERFACE_COMPILE_OPTIONS     "${EXIV2_CFLAGS_OTHER}"
    )
    add_library(Exiv2::exiv2lib ALIAS exiv2_imported)
  else()
    # Last resort: locate manually
    find_path(EXIV2_INCLUDE_DIR NAMES exiv2/exiv2.hpp
      HINTS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(EXIV2_LIBRARY NAMES exiv2
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(NOT EXIV2_INCLUDE_DIR OR NOT EXIV2_LIBRARY)
      message(FATAL_ERROR "exiv2 not found. Install with: brew install exiv2")
    endif()
    add_library(exiv2_imported INTERFACE IMPORTED)
    set_target_properties(exiv2_imported PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${EXIV2_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES      "${EXIV2_LIBRARY}"
    )
    add_library(Exiv2::exiv2lib ALIAS exiv2_imported)
  endif()
endif()

# pugixml ships its own CMake config
find_package(pugixml QUIET CONFIG)
if(NOT pugixml_FOUND)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PUGIXML REQUIRED pugixml)
    add_library(pugixmllib INTERFACE IMPORTED)
    set_target_properties(pugixmllib PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${PUGIXML_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES      "${PUGIXML_LINK_LIBRARIES}"
      INTERFACE_COMPILE_OPTIONS     "${PUGIXML_CFLAGS_OTHER}"
    )
    add_library(pugixml::pugixml ALIAS pugixmllib)
  else()
    find_path(PUGIXML_INCLUDE_DIR NAMES pugixml.hpp
      HINTS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(PUGIXML_LIBRARY NAMES pugixml
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(NOT PUGIXML_INCLUDE_DIR OR NOT PUGIXML_LIBRARY)
      message(FATAL_ERROR "pugixml not found. Install with: brew install pugixml")
    endif()
    add_library(pugixmllib INTERFACE IMPORTED)
    set_target_properties(pugixmllib PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${PUGIXML_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES      "${PUGIXML_LIBRARY}"
    )
    add_library(pugixml::pugixml ALIAS pugixmllib)
  endif()
endif()

# ── Optional dependencies ─────────────────────────────────────────────────────

# OpenCL
if(DTPIPE_ENABLE_OPENCL)
  find_package(OpenCL QUIET)
  if(OpenCL_FOUND)
    message(STATUS "OpenCL found: ${OpenCL_VERSION_STRING}")
    set(DTPIPE_HAVE_OPENCL TRUE)
  else()
    message(STATUS "OpenCL not found – GPU acceleration disabled")
    set(DTPIPE_HAVE_OPENCL FALSE)
  endif()
else()
  set(DTPIPE_HAVE_OPENCL FALSE)
endif()

# OpenMP
if(DTPIPE_ENABLE_OPENMP)
  find_package(OpenMP QUIET COMPONENTS C CXX)
  if(NOT OpenMP_C_FOUND)
    # Apple Clang doesn't ship OpenMP — try Homebrew libomp
    find_program(_BREW brew)
    if(_BREW)
      execute_process(COMMAND ${_BREW} --prefix libomp
                      OUTPUT_VARIABLE _LIBOMP_PREFIX
                      OUTPUT_STRIP_TRAILING_WHITESPACE
                      ERROR_QUIET)
    endif()
    if(_LIBOMP_PREFIX AND EXISTS "${_LIBOMP_PREFIX}/lib/libomp.dylib")
      message(STATUS "Using Homebrew libomp at ${_LIBOMP_PREFIX}")
      set(OpenMP_C_FLAGS        "-Xclang -fopenmp -I${_LIBOMP_PREFIX}/include" CACHE STRING "" FORCE)
      set(OpenMP_CXX_FLAGS      "-Xclang -fopenmp -I${_LIBOMP_PREFIX}/include" CACHE STRING "" FORCE)
      set(OpenMP_C_LIB_NAMES    "omp" CACHE STRING "" FORCE)
      set(OpenMP_CXX_LIB_NAMES  "omp" CACHE STRING "" FORCE)
      set(OpenMP_omp_LIBRARY     "${_LIBOMP_PREFIX}/lib/libomp.dylib" CACHE FILEPATH "" FORCE)
      find_package(OpenMP QUIET COMPONENTS C CXX)
    endif()
  endif()
  if(OpenMP_C_FOUND)
    message(STATUS "OpenMP found: ${OpenMP_C_VERSION}")
    set(DTPIPE_HAVE_OPENMP TRUE)
  else()
    message(STATUS "OpenMP not found – CPU parallelization disabled")
    set(DTPIPE_HAVE_OPENMP FALSE)
  endif()
else()
  set(DTPIPE_HAVE_OPENMP FALSE)
endif()

# ── rawspeed (RAW decoding library) ───────────────────────────────────────────

include(FetchContent)

# Configure rawspeed build options before fetching
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKING OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP ${DTPIPE_HAVE_OPENMP} CACHE BOOL "" FORCE)
set(BINARY_PACKAGE_BUILD ON CACHE BOOL "" FORCE)
set(RAWSPEED_ENABLE_LTO OFF CACHE BOOL "" FORCE)

# rawspeed only accepts: Debug, Release, ReleaseWithAsserts, Coverage, Sanitize, TSan, Fuzz
# Map our build type to a rawspeed-compatible one
string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_upper)
set(CMAKE_BUILD_TYPE_SAVE "${CMAKE_BUILD_TYPE}")
if(_build_upper STREQUAL "RELWITHDEBINFO" OR _build_upper STREQUAL "MINSIZEREL")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

FetchContent_Declare(
  rawspeed
  GIT_REPOSITORY https://github.com/darktable-org/rawspeed.git
  GIT_TAG        develop
  GIT_SHALLOW    TRUE
)

# Suppress warnings from rawspeed build
set(CMAKE_C_FLAGS_SAVE "${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS_SAVE "${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")

FetchContent_MakeAvailable(rawspeed)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_SAVE}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_SAVE}")

# Restore our original build type
set(CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE_SAVE}" CACHE STRING "" FORCE)

# Deploy rawspeed data files (cameras.xml) to build output
set(RAWSPEED_DATADIR "${rawspeed_SOURCE_DIR}/data")
set(DTPIPE_DATADIR "${CMAKE_BINARY_DIR}/share/dtpipe/rawspeed")
add_custom_target(rawspeed_data ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DTPIPE_DATADIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${RAWSPEED_DATADIR}/cameras.xml"
    "${RAWSPEED_DATADIR}/showcameras.xsl"
    "${DTPIPE_DATADIR}/"
  COMMENT "Deploying rawspeed camera data"
)

# ── Compiler / sanitizer setup ────────────────────────────────────────────────

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No build type selected, defaulting to RelWithDebInfo")
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Address + UBSan on Debug (GCC/Clang only)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(_san_flags -fsanitize=address,undefined -fno-omit-frame-pointer)
    add_compile_options(${_san_flags})
    add_link_options(${_san_flags})
    message(STATUS "Sanitizers enabled: address, undefined")
  endif()
endif()

# ── Subdirectories ────────────────────────────────────────────────────────────

add_subdirectory(src)

if(DTPIPE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DTPIPE_BUILD_NODE)
  add_subdirectory(node)
endif()
