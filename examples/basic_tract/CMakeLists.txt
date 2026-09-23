# Standalone consumer build for the basic demo. It fetches a pinned revision
# from GitHub and links the application only against fexma::record_tract.

cmake_minimum_required(VERSION 3.16)

project(record_tract_basic_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
  record_tract_source
  GIT_REPOSITORY https://github.com/UglyPusher/record_tract.git
  GIT_TAG da6a2459234ad0cf9f2044984b0716534f10241f
)

FetchContent_GetProperties(record_tract_source)
if(NOT record_tract_source_POPULATED)
  FetchContent_Populate(record_tract_source)
  add_subdirectory(
    ${record_tract_source_SOURCE_DIR}
    ${record_tract_source_BINARY_DIR}
    EXCLUDE_FROM_ALL
  )
endif()

add_executable(example_basic_tract_from_github
  demo_app.cpp
)

target_link_libraries(example_basic_tract
    PRIVATE fexma::record_tract
)

if(MSVC)
  target_compile_options(example_basic_tract_from_github PRIVATE
    /W4
    /permissive-
    /EHsc
  )
else()
  target_compile_options(example_basic_tract_from_github PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  )
endif()
