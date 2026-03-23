cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# lsapi_raw — native LSAPI streaming echo application (benchmark baseline)
#
# This is a *native* binary (not WASM).  It links directly against the LSAPI
# library source (lsapilib.c) and serves the same streaming echo behaviour as
# the send_recv_stream WASM filter, making it suitable for measuring the raw
# LSAPI transport overhead without a WASM runtime in the path.
#
# Build from the lswasm project root:
#
#   cmake -S samples/lsapi_raw -B samples/lsapi_raw/build
#   cmake --build samples/lsapi_raw/build
#
# The resulting binary is:  samples/lsapi_raw/build/lsapi_raw
# ---------------------------------------------------------------------------

project(lsapi_raw CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release if no build type was specified.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Locate the lswasm source tree (two levels up from this sample directory).
# ---------------------------------------------------------------------------
set(LSWASM_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    CACHE PATH "Path to the lswasm src/ directory")

if(NOT EXISTS "${LSWASM_SRC_DIR}/lsapilib.c")
  message(FATAL_ERROR
    "lsapilib.c not found at ${LSWASM_SRC_DIR}/lsapilib.c.\n"
    "Make sure you are building from the lswasm project tree.")
endif()

# ---------------------------------------------------------------------------
# Build target
# ---------------------------------------------------------------------------
add_executable(lsapi_raw
  lsapi_raw.cpp
  ${LSWASM_SRC_DIR}/lsapilib.c
)

target_include_directories(lsapi_raw PRIVATE
  ${LSWASM_SRC_DIR}
)

# lsapilib.c uses dlopen/dlsym and pthread APIs.
find_package(Threads REQUIRED)
target_link_libraries(lsapi_raw PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

target_compile_options(lsapi_raw PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<CONFIG:Release>:-O2>
  $<$<CONFIG:RelWithDebInfo>:-O2 -g>
  $<$<CONFIG:Debug>:-O0 -g>
  $<$<CONFIG:MinSizeRel>:-Os>
)

message(STATUS "lswasm src dir: ${LSWASM_SRC_DIR}")
message(STATUS "Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "Configured lsapi_raw target (native LSAPI benchmark baseline)")
