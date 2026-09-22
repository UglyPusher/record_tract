cmake_minimum_required(VERSION 3.16)

project(record_tract LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_library(fexma_binary INTERFACE)
add_library(fexma::binary ALIAS fexma_binary)

target_include_directories(fexma_binary
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(fexma_binary INTERFACE cxx_std_20)

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

add_library(fexma_record_tract STATIC
  src/record_tract/record_tape.cpp
)

add_library(fexma::record_tract ALIAS fexma_record_tract)

target_include_directories(fexma_record_tract
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(fexma_record_tract PUBLIC cxx_std_20)

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
fexma_apply_warnings(fexma_wal)

add_executable(example_basic_tract
  examples/basic_tract/demo_app.cpp
)
target_link_libraries(example_basic_tract PRIVATE fexma::record_tract)
fexma_apply_warnings(example_basic_tract)

add_executable(test_wal_reader tests/wal/test_reader.cpp)
target_link_libraries(test_wal_reader PRIVATE fexma::wal)
fexma_apply_warnings(test_wal_reader)
add_test(NAME test_wal_reader COMMAND test_wal_reader)

add_executable(test_wal_recovery tests/wal/test_recovery.cpp)
target_link_libraries(test_wal_recovery PRIVATE fexma::wal)
target_include_directories(test_wal_recovery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/wal)
fexma_apply_warnings(test_wal_recovery)
add_test(NAME test_wal_recovery COMMAND test_wal_recovery)

add_executable(test_record_tape tests/core/test_record_tape.cpp)
target_link_libraries(test_record_tape PRIVATE fexma::record_tract)
fexma_apply_warnings(test_record_tape)
add_test(NAME test_record_tape COMMAND test_record_tape)

add_executable(test_record_tape_set_tail_ref
  tests/core/test_record_tape_set_tail_ref.cpp
)
target_link_libraries(test_record_tape_set_tail_ref PRIVATE fexma::record_tract)
fexma_apply_warnings(test_record_tape_set_tail_ref)
add_test(NAME test_record_tape_set_tail_ref_open
  COMMAND test_record_tape_set_tail_ref open
)
add_test(NAME test_record_tape_set_tail_ref_closed
  COMMAND test_record_tape_set_tail_ref closed
)

add_executable(test_slider tests/core/test_slider.cpp)
target_link_libraries(test_slider PRIVATE fexma::record_tract)
fexma_apply_warnings(test_slider)
add_test(NAME test_slider COMMAND test_slider)

add_executable(test_bare_pipeline tests/core/test_bare_pipeline.cpp)
target_link_libraries(test_bare_pipeline PRIVATE fexma::record_tract)
fexma_apply_warnings(test_bare_pipeline)
add_test(NAME test_bare_pipeline COMMAND test_bare_pipeline)

add_executable(test_wal_persistence tests/wal/test_persistence.cpp)
target_link_libraries(test_wal_persistence PRIVATE fexma::wal)
target_include_directories(test_wal_persistence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/wal)
fexma_apply_warnings(test_wal_persistence)
add_test(NAME test_wal_persistence COMMAND test_wal_persistence)
