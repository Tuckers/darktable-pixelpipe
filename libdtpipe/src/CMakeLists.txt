set(DTPIPE_SOURCES
  init.c
  common/iop_order.c
  pipe/pixelpipe.c
)

add_library(dtpipe SHARED ${DTPIPE_SOURCES})

# ── Include directories ───────────────────────────────────────────────────────

target_include_directories(dtpipe
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/pipe
)

# ── Language standard ─────────────────────────────────────────────────────────

target_compile_features(dtpipe PUBLIC c_std_11)

# ── Required dependency linkage ───────────────────────────────────────────────

target_link_libraries(dtpipe
  PRIVATE
    rawspeed
    LCMS2::LCMS2
    JPEG::JPEG
    PNG::PNG
    TIFF::TIFF
    Exiv2::exiv2lib
    pugixml::pugixml
)

# ── Optional dependency linkage ───────────────────────────────────────────────

if(DTPIPE_HAVE_OPENCL)
  target_link_libraries(dtpipe PRIVATE OpenCL::OpenCL)
  target_compile_definitions(dtpipe PRIVATE DTPIPE_HAVE_OPENCL=1)
endif()

if(DTPIPE_HAVE_OPENMP)
  target_link_libraries(dtpipe PRIVATE OpenMP::OpenMP_C)
  target_compile_definitions(dtpipe PRIVATE DTPIPE_HAVE_OPENMP=1)
endif()

# ── Library metadata ──────────────────────────────────────────────────────────

set_target_properties(dtpipe PROPERTIES
  VERSION   ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  PUBLIC_HEADER "${CMAKE_SOURCE_DIR}/include/dtpipe.h"
)

# ── Install rules ─────────────────────────────────────────────────────────────

install(TARGETS dtpipe
  LIBRARY       DESTINATION lib
  ARCHIVE       DESTINATION lib
  PUBLIC_HEADER DESTINATION include
)
