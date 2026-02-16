enable_testing()

# ── Task 3.5 verification ─────────────────────────────────────────────────────

set(RAF_PATH "${CMAKE_SOURCE_DIR}/../test-image/DSCF4379.RAF")

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
