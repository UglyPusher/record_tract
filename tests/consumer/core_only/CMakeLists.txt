cmake_minimum_required(VERSION 3.16)

project(record_tract_core_only_consumer LANGUAGES CXX)

if(NOT RECORD_TRACT_SOURCE_DIR)
  message(FATAL_ERROR "RECORD_TRACT_SOURCE_DIR must name the record_tract checkout")
endif()

include(FetchContent)
FetchContent_Declare(
  record_tract
  SOURCE_DIR "${RECORD_TRACT_SOURCE_DIR}"
)
FetchContent_MakeAvailable(record_tract)

add_executable(core_only_consumer main.cpp)
target_link_libraries(core_only_consumer PRIVATE fexma::record_tract)
