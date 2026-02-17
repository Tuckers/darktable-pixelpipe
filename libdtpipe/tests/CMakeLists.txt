enable_testing()

set(RAF_PATH "${CMAKE_SOURCE_DIR}/../test-image/DSCF4379.RAF")
set(REFERENCE_DIR "${CMAKE_SOURCE_DIR}/tests/reference")

# ── Task 7.1: Unified test harness ────────────────────────────────────────────

add_executable(test_main
  test_main.c
)

target_link_libraries(test_main PRIVATE dtpipe m)

target_include_directories(test_main PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h (public API only)
)

add_test(
  NAME    test_main
  COMMAND test_main "${RAF_PATH}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_tests_properties(test_main PROPERTIES TIMEOUT 300)

# ── Task 7.2: Reference render generator ─────────────────────────────────────
#
# NOT a ctest test — run once manually (or via `cmake --build . --target generate_reference`)
# to establish the ground-truth renders used by Task 7.3 regression tests.
#
#   cmake --build libdtpipe/build-release --target generate_reference
#
# Output: libdtpipe/tests/reference/{preset_a,preset_b,preset_c}.{png,json} + metadata.txt

add_executable(gen_reference
  gen_reference.c
)

target_link_libraries(gen_reference PRIVATE dtpipe m)

target_include_directories(gen_reference PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h (public API only)
)

add_custom_target(generate_reference
  COMMAND gen_reference "${RAF_PATH}" "${REFERENCE_DIR}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  DEPENDS gen_reference
  COMMENT "Generating reference renders to ${REFERENCE_DIR}"
)

# ── Task 7.3: Regression tests ───────────────────────────────────────────────
#
# Renders each reference preset at scale 0.25 and compares pixel-by-pixel
# against the PNG ground truth in tests/reference/.
#
#   cmake --build libdtpipe/build-release && ctest -R regression

add_executable(test_regression
  test_regression.c
)

target_link_libraries(test_regression PRIVATE dtpipe PNG::PNG m)

target_include_directories(test_regression PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h (public API only)
)

add_test(
  NAME    regression
  COMMAND test_regression "${RAF_PATH}" "${REFERENCE_DIR}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_tests_properties(regression PROPERTIES TIMEOUT 300)

# ── Performance benchmark ──────────────────────────────────────────────────────

add_executable(benchmark_performance
  benchmark_performance.c
)

target_link_libraries(benchmark_performance PRIVATE dtpipe m)

target_include_directories(benchmark_performance PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h
  ${CMAKE_SOURCE_DIR}/src        # dtpipe_internal.h (thread count)
)

if(DTPIPE_HAVE_OPENMP)
  target_link_libraries(benchmark_performance PRIVATE OpenMP::OpenMP_C)
endif()

add_test(
  NAME    benchmark_performance
  COMMAND benchmark_performance "${RAF_PATH}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_tests_properties(benchmark_performance PROPERTIES TIMEOUT 120)

# ── Task 5.5 verification ─────────────────────────────────────────────────────

add_executable(test_xmp_write
  test_xmp_write.c
)

target_link_libraries(test_xmp_write PRIVATE dtpipe m)

target_include_directories(test_xmp_write PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h (public API only)
)

add_test(
  NAME    xmp_write
  COMMAND test_xmp_write "${RAF_PATH}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Task 5.4 verification ─────────────────────────────────────────────────────

add_executable(test_xmp_read
  test_xmp_read.c
)

target_link_libraries(test_xmp_read PRIVATE dtpipe m)

target_include_directories(test_xmp_read PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h (public API only)
)

add_test(
  NAME    xmp_read
  COMMAND test_xmp_read "${RAF_PATH}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Task 5.3 verification ─────────────────────────────────────────────────────

add_executable(test_history_deserialize
  test_history_deserialize.c
)

target_link_libraries(test_history_deserialize PRIVATE dtpipe)

target_include_directories(test_history_deserialize PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h (public API only)
)

add_test(
  NAME    history_deserialize
  COMMAND test_history_deserialize "${RAF_PATH}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Task 5.2 verification ─────────────────────────────────────────────────────

add_executable(test_history_roundtrip
  test_history_roundtrip.c
)

target_link_libraries(test_history_roundtrip PRIVATE dtpipe)

target_include_directories(test_history_roundtrip PRIVATE
  ${CMAKE_SOURCE_DIR}/include    # dtpipe.h (public API only)
)

add_test(
  NAME    history_roundtrip
  COMMAND test_history_roundtrip "${RAF_PATH}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)


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
