cmake_minimum_required(VERSION 3.16)

project(
    record_tract
    VERSION 0.1.0
    LANGUAGES CXX
)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(RECORD_TRACT_IS_TOP_LEVEL ON)
else()
  set(RECORD_TRACT_IS_TOP_LEVEL OFF)
endif()

option(
  RECORD_TRACT_BUILD_WAL
  "Build record_tract WAL components"
  ${RECORD_TRACT_IS_TOP_LEVEL}
)

option(
  RECORD_TRACT_BUILD_EXAMPLES
  "Build record_tract examples"
  ${RECORD_TRACT_IS_TOP_LEVEL}
)

option(
  RECORD_TRACT_BUILD_TESTS
  "Build record_tract tests"
  ${RECORD_TRACT_IS_TOP_LEVEL}
)

if(RECORD_TRACT_BUILD_TESTS)
  include(CTest)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(fexma_binary INTERFACE)
add_library(fexma::binary ALIAS fexma_binary)

target_include_directories(fexma_binary
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(fexma_binary INTERFACE cxx_std_20)

add_library(fexma_record_tract STATIC
  src/record_tract/record_tape.cpp
)

add_library(fexma::record_tract ALIAS fexma_record_tract)

target_include_directories(fexma_record_tract
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(fexma_record_tract PUBLIC cxx_std_20)

function(fexma_apply_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE
      /W4
      /permissive-
      /EHsc
      /FS
      $<$<CONFIG:Release>:/O2>
    )
  else()
    target_compile_options(${target_name} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      $<$<CONFIG:Release>:-O3>
    )
  endif()
endfunction()

fexma_apply_warnings(fexma_record_tract)

if(RECORD_TRACT_BUILD_WAL)
  add_library(fexma_wal STATIC
    src/wal/config.cpp
    src/wal/persistence.cpp
    src/wal/physical_wal_file.cpp
    src/wal/reader.cpp
    src/wal/recovery.cpp
  )

  add_library(fexma::wal ALIAS fexma_wal)

  target_include_directories(fexma_wal
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
  )

  target_compile_features(fexma_wal PUBLIC cxx_std_20)
  target_link_libraries(fexma_wal
    PUBLIC
      fexma::record_tract
    PRIVATE
      fexma::binary
  )

  fexma_apply_warnings(fexma_wal)
endif()

if(RECORD_TRACT_BUILD_EXAMPLES)
  add_executable(example_basic_tract
    examples/basic_tract/demo_app.cpp
  )
  target_link_libraries(example_basic_tract PRIVATE fexma::record_tract)
  fexma_apply_warnings(example_basic_tract)
endif()

if(RECORD_TRACT_BUILD_TESTS AND BUILD_TESTING)
  add_executable(test_binary_little_endian tests/binary/test_little_endian.cpp)
  target_link_libraries(test_binary_little_endian PRIVATE fexma::binary)
  if(MSVC)
    target_compile_options(test_binary_little_endian PRIVATE
      /W4
      /permissive-
      /EHsc
    )
  else()
    target_compile_options(test_binary_little_endian PRIVATE
      -Wall
      -Wextra
      -Wpedantic
    )
  endif()
  add_test(NAME test_binary_little_endian COMMAND test_binary_little_endian)

  if(RECORD_TRACT_BUILD_WAL)
    add_executable(test_wal_reader tests/wal/test_reader.cpp)
    target_link_libraries(test_wal_reader PRIVATE fexma::wal)
    fexma_apply_warnings(test_wal_reader)
    add_test(NAME test_wal_reader COMMAND test_wal_reader)

    add_executable(test_wal_recovery tests/wal/test_recovery.cpp)
    target_link_libraries(test_wal_recovery PRIVATE fexma::wal)
    target_include_directories(test_wal_recovery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/wal)
    fexma_apply_warnings(test_wal_recovery)
    add_test(NAME test_wal_recovery COMMAND test_wal_recovery)
  endif()

  add_executable(test_record_tape tests/core/test_record_tape.cpp)
  target_link_libraries(test_record_tape PRIVATE fexma::record_tract)
  fexma_apply_warnings(test_record_tape)
  add_test(NAME test_record_tape COMMAND test_record_tape)

  add_executable(test_core_fail_fast
    tests/core/test_core_fail_fast.cpp
  )
  target_link_libraries(test_core_fail_fast PRIVATE fexma::record_tract)
  fexma_apply_warnings(test_core_fail_fast)
  add_test(NAME test_core_fail_fast_set_tail_ref_open
    COMMAND test_core_fail_fast set_tail_ref_open
  )
  add_test(NAME test_core_fail_fast_set_tail_ref_closed
    COMMAND test_core_fail_fast set_tail_ref_closed
  )
  add_test(NAME test_core_fail_fast_reopen_closed
    COMMAND test_core_fail_fast reopen_closed
  )
  add_test(NAME test_core_fail_fast_zero_read_count
    COMMAND test_core_fail_fast zero_read_count
  )
  add_test(NAME test_core_fail_fast_zero_publish_count
    COMMAND test_core_fail_fast zero_publish_count
  )
  add_test(NAME test_core_fail_fast_tail_ahead_of_head
    COMMAND test_core_fail_fast tail_ahead_of_head
  )
  add_test(NAME test_core_fail_fast_retained_range_exceeds_capacity
    COMMAND test_core_fail_fast retained_range_exceeds_capacity
  )
  add_test(NAME test_core_fail_fast_slider_upstream_regression
    COMMAND test_core_fail_fast slider_upstream_regression
  )
  add_test(NAME test_core_fail_fast_slider_view_unavailable
    COMMAND test_core_fail_fast slider_view_unavailable
  )

  add_executable(test_slider tests/core/test_slider.cpp)
  target_link_libraries(test_slider PRIVATE fexma::record_tract)
  fexma_apply_warnings(test_slider)
  add_test(NAME test_slider COMMAND test_slider)

  add_executable(test_bare_pipeline tests/core/test_bare_pipeline.cpp)
  target_link_libraries(test_bare_pipeline PRIVATE fexma::record_tract)
  fexma_apply_warnings(test_bare_pipeline)
  add_test(NAME test_bare_pipeline COMMAND test_bare_pipeline)

  if(RECORD_TRACT_BUILD_WAL)
    add_executable(test_wal_persistence tests/wal/test_persistence.cpp)
    target_link_libraries(test_wal_persistence PRIVATE fexma::wal)
    target_include_directories(test_wal_persistence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/wal)
    fexma_apply_warnings(test_wal_persistence)
    add_test(NAME test_wal_persistence COMMAND test_wal_persistence)
  endif()
endif()
