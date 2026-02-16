enable_testing()

set(RAF_PATH "${CMAKE_SOURCE_DIR}/../test-image/DSCF4379.RAF")

# ── Task 4.6 verification ─────────────────────────────────────────────────────

add_executable(test_pipeline_export
  test_pipeline_export.c
)

target_link_libraries(test_pipeline_export PRIVATE dtpipe)

target_include_directories(test_pipeline_export PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h (public API only)
)

add_test(
  NAME    pipeline_export
  COMMAND test_pipeline_export "${RAF_PATH}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Task 4.5 verification ─────────────────────────────────────────────────────

add_executable(test_pipeline_render
  test_pipeline_render.c
)

target_link_libraries(test_pipeline_render PRIVATE dtpipe m)

target_include_directories(test_pipeline_render PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h (public API only)
)

add_test(
  NAME    pipeline_render
  COMMAND test_pipeline_render "${RAF_PATH}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Task 4.4 verification ─────────────────────────────────────────────────────

# Internal unit test: descriptor lookup + manual buffer round-trip
# (no IOP registry required)
add_executable(test_params_unit
  test_params_unit.c
)

target_link_libraries(test_params_unit PRIVATE dtpipe)

target_include_directories(test_params_unit PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h
  ${CMAKE_SOURCE_DIR}/src        # dtpipe_internal.h
  ${CMAKE_SOURCE_DIR}/src/pipe   # create.h, params.h
)

add_test(
  NAME    params_unit
  COMMAND test_params_unit
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Public API integration test (requires image; round-trip skipped if
# module registry is empty)
add_executable(test_pipeline_params
  test_pipeline_params.c
)

target_link_libraries(test_pipeline_params PRIVATE dtpipe)

target_include_directories(test_pipeline_params PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h (public API only)
)

add_test(
  NAME    pipeline_params
  COMMAND test_pipeline_params "${RAF_PATH}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Task 4.3 verification ─────────────────────────────────────────────────────

add_executable(test_pipeline_create
  test_pipeline_create.c
)

target_link_libraries(test_pipeline_create PRIVATE dtpipe)

target_include_directories(test_pipeline_create PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h (public API only)
)

add_test(
  NAME    pipeline_create
  COMMAND test_pipeline_create "${RAF_PATH}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Task 3.5 verification ─────────────────────────────────────────────────────

add_executable(test_pipeline_process
  test_pipeline_process.c
)

target_link_libraries(test_pipeline_process PRIVATE dtpipe m)

target_include_directories(test_pipeline_process PRIVATE
  ${CMAKE_SOURCE_DIR}/src        # dtpipe_internal.h
  ${CMAKE_SOURCE_DIR}/src/pipe   # pixelpipe.h
)

add_test(
  NAME    pipeline_process
  COMMAND test_pipeline_process "${RAF_PATH}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
