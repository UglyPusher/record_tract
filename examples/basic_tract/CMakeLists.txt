# Standalone build for the basic demo using the local record_tract checkout.

cmake_minimum_required(VERSION 3.16)

project(record_tract_basic_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(
  ${CMAKE_CURRENT_LIST_DIR}/../..
  ${CMAKE_CURRENT_BINARY_DIR}/record_tract
  EXCLUDE_FROM_ALL
)

add_executable(example_basic_tract
  demo_app.cpp
)

target_link_libraries(example_basic_tract PRIVATE fexma::record_tract)

if(MSVC)
  target_compile_options(example_basic_tract PRIVATE
    /W4
    /permissive-
    /EHsc
  )
else()
  target_compile_options(example_basic_tract PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  )
endif()
