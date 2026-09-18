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

add_executable(test_binary_little_endian tests/test_little_endian.cpp)
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
  src/record_tape.cpp
)

add_library(fexma::record_tract ALIAS fexma_record_tract)

target_include_directories(fexma_record_tract
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(fexma_record_tract PUBLIC cxx_std_20)

add_library(fexma_wal STATIC
  src/config.cpp
  src/persistence.cpp
  src/physical_wal_file.cpp
  src/reader.cpp
  src/recovery.cpp
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

function(fexma_wal_apply_warnings target_name)
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

fexma_wal_apply_warnings(fexma_record_tract)
fexma_wal_apply_warnings(fexma_wal)

add_executable(test_wal_reader tests/test_wal_reader.cpp)
target_link_libraries(test_wal_reader PRIVATE fexma::wal)
fexma_wal_apply_warnings(test_wal_reader)
add_test(NAME test_wal_reader COMMAND test_wal_reader)

add_executable(test_wal_recovery tests/test_wal_recovery.cpp)
target_link_libraries(test_wal_recovery PRIVATE fexma::wal)
target_include_directories(test_wal_recovery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
fexma_wal_apply_warnings(test_wal_recovery)
add_test(NAME test_wal_recovery COMMAND test_wal_recovery)

add_executable(test_record_tape tests/test_record_tape.cpp)
target_link_libraries(test_record_tape PRIVATE fexma::record_tract)
fexma_wal_apply_warnings(test_record_tape)
add_test(NAME test_record_tape COMMAND test_record_tape)

add_executable(test_wal_slider tests/test_wal_slider.cpp)
target_link_libraries(test_wal_slider PRIVATE fexma::record_tract)
fexma_wal_apply_warnings(test_wal_slider)
add_test(NAME test_wal_slider COMMAND test_wal_slider)

add_executable(test_wal_bare_pipeline tests/test_wal_bare_pipeline.cpp)
target_link_libraries(test_wal_bare_pipeline PRIVATE fexma::record_tract)
fexma_wal_apply_warnings(test_wal_bare_pipeline)
add_test(NAME test_wal_bare_pipeline COMMAND test_wal_bare_pipeline)

add_executable(test_wal_persistence_slider tests/test_wal_persistence_slider.cpp)
target_link_libraries(test_wal_persistence_slider PRIVATE fexma::wal)
target_include_directories(test_wal_persistence_slider PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
fexma_wal_apply_warnings(test_wal_persistence_slider)
add_test(NAME test_wal_persistence_slider COMMAND test_wal_persistence_slider)
