# FindLCMS2.cmake - Find the LCMS2 (Little Color Management System) library
#
# This module defines:
#  LCMS2_FOUND        - True if LCMS2 was found
#  LCMS2_INCLUDE_DIRS - Include directories for LCMS2
#  LCMS2_LIBRARIES    - Libraries to link against
#  LCMS2_VERSION      - Version of LCMS2

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_LCMS2 QUIET lcms2)
endif()

find_path(LCMS2_INCLUDE_DIR
  NAMES lcms2.h
  HINTS ${PC_LCMS2_INCLUDEDIR} ${PC_LCMS2_INCLUDE_DIRS}
  PATH_SUFFIXES lcms2
)

find_library(LCMS2_LIBRARY
  NAMES lcms2 lcms2-2
  HINTS ${PC_LCMS2_LIBDIR} ${PC_LCMS2_LIBRARY_DIRS}
)

set(LCMS2_VERSION ${PC_LCMS2_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LCMS2
  REQUIRED_VARS LCMS2_LIBRARY LCMS2_INCLUDE_DIR
  VERSION_VAR LCMS2_VERSION
)

if(LCMS2_FOUND)
  set(LCMS2_LIBRARIES ${LCMS2_LIBRARY})
  set(LCMS2_INCLUDE_DIRS ${LCMS2_INCLUDE_DIR})
  if(NOT TARGET LCMS2::LCMS2)
    add_library(LCMS2::LCMS2 UNKNOWN IMPORTED)
    set_target_properties(LCMS2::LCMS2 PROPERTIES
      IMPORTED_LOCATION "${LCMS2_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LCMS2_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(LCMS2_INCLUDE_DIR LCMS2_LIBRARY)
