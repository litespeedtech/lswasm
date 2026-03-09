cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
# This file expects to be configured with the wasm32-wasi toolchain file:
#
#   cmake -S samples -B samples/build \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi-toolchain.cmake \
#     -DWASI_SDK_PATH=/path/to/wasi-sdk-29.0
#
# The toolchain file sets the compiler, sysroot, and target triple.
# If you prefer not to use a toolchain file you can also pass
# -DWASI_SDK_PATH=<path> directly and the fallback block below will
# configure the same settings inline.
# ---------------------------------------------------------------------------

# Fallback: if no toolchain file was used, configure the WASI SDK inline.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic" OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "wasm32")
  # Clear any stale CMAKE_TOOLCHAIN_FILE so CMake doesn't look for a
  # non-existent file during project() when re-configuring without --fresh.
  unset(CMAKE_TOOLCHAIN_FILE CACHE)
  set(WASI_SDK_PATH "" CACHE PATH "Path to the WASI SDK installation (e.g. /opt/wasi-sdk-29.0)")

  if(NOT WASI_SDK_PATH)
    if(DEFINED ENV{WASI_SDK_PATH})
      set(WASI_SDK_PATH "$ENV{WASI_SDK_PATH}")
    endif()
  endif()

  if(NOT WASI_SDK_PATH OR NOT EXISTS "${WASI_SDK_PATH}/bin/clang++")
    message(FATAL_ERROR
      "WASI SDK not found.  Either use the toolchain file:\n"
      "  cmake -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi-toolchain.cmake\n"
      "or set -DWASI_SDK_PATH=<path> (or export WASI_SDK_PATH).\n"
      "Download from: https://github.com/WebAssembly/wasi-sdk/releases")
  endif()

  set(WASI_SYSROOT "${WASI_SDK_PATH}/share/wasi-sysroot")
  set(CMAKE_CXX_COMPILER "${WASI_SDK_PATH}/bin/clang++" CACHE FILEPATH "CXX compiler" FORCE)
  set(CMAKE_CXX_COMPILER_TARGET "wasm32-wasi" CACHE STRING "" FORCE)
  set(CMAKE_SYSROOT "${WASI_SYSROOT}" CACHE PATH "" FORCE)
  set(CMAKE_SYSTEM_NAME Generic)
  set(CMAKE_SYSTEM_PROCESSOR wasm32)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
endif()

# Default to Release if no build type was specified.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# proxy-wasm-cpp-sdk — header-only API plus one .cc runtime glue file.
# ---------------------------------------------------------------------------
set(PROXY_WASM_CPP_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/proxy-wasm-cpp-sdk"
    CACHE PATH "Path to the proxy-wasm-cpp-sdk directory")

if(NOT EXISTS "${PROXY_WASM_CPP_SDK}/proxy_wasm_intrinsics.h")
  message(FATAL_ERROR
    "proxy-wasm-cpp-sdk not found at ${PROXY_WASM_CPP_SDK}.\n"
    "Make sure the git submodule is initialised:\n"
    "  git submodule update --init --recursive")
endif()

project(sample_filter_wasm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Build target — sample_filter.wasm
# ---------------------------------------------------------------------------
add_executable(sample_filter
  sample_filter.cpp
  ${PROXY_WASM_CPP_SDK}/proxy_wasm_intrinsics.cc
)

target_include_directories(sample_filter PRIVATE
  ${PROXY_WASM_CPP_SDK}
)

# Compile flags for wasm32-wasi module.
# The target triple and sysroot are handled by the toolchain / CMAKE_SYSROOT;
# only WASM-specific flags are listed here.
target_compile_options(sample_filter PRIVATE
  -fvisibility=hidden
  -fno-exceptions
  -fno-rtti
  $<$<CONFIG:Release>:-O2>
  $<$<CONFIG:RelWithDebInfo>:-O2 -g>
  $<$<CONFIG:Debug>:-O0 -g>
  $<$<CONFIG:MinSizeRel>:-Os>
)

# The proxy-wasm ABI functions that the host calls into.
# -mexec-model=reactor builds a WASI reactor (no _start); the toolchain
# automatically exports _initialize which runs __wasm_call_ctors.
set(PROXY_WASM_EXPORTS
  memory
  malloc
  free
  proxy_abi_version_0_2_1
  proxy_on_context_create
  proxy_on_vm_start
  proxy_validate_configuration
  proxy_on_configure
  proxy_on_tick
  proxy_on_request_headers
  proxy_on_request_metadata
  proxy_on_request_body
  proxy_on_request_trailers
  proxy_on_response_headers
  proxy_on_response_metadata
  proxy_on_response_body
  proxy_on_response_trailers
  proxy_on_done
  proxy_on_log
  proxy_on_delete
  proxy_on_new_connection
  proxy_on_downstream_data
  proxy_on_upstream_data
  proxy_on_downstream_connection_close
  proxy_on_upstream_connection_close
  proxy_on_queue_ready
  proxy_on_grpc_receive_initial_metadata
  proxy_on_grpc_receive_trailing_metadata
  proxy_on_grpc_receive
  proxy_on_grpc_close
  proxy_on_http_call_response
  proxy_on_foreign_function
)

# Build export flags list.
set(WASM_EXPORT_FLAGS "")
foreach(_sym IN LISTS PROXY_WASM_EXPORTS)
  list(APPEND WASM_EXPORT_FLAGS "-Wl,--export=${_sym}")
endforeach()

# Link as a WASI reactor — only the symbols listed above are exported.
# --allow-undefined permits unresolved host-provided imports (proxy_log, etc.)
# that will be satisfied at runtime by the proxy-wasm host.
target_link_options(sample_filter PRIVATE
  -mexec-model=reactor
  -Wl,--allow-undefined
  $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-Wl,--strip-all>
  ${WASM_EXPORT_FLAGS}
)

# Produce a .wasm file instead of a native binary.
set_target_properties(sample_filter PROPERTIES SUFFIX ".wasm")

message(STATUS "WASI SDK:           ${WASI_SDK_PATH}")
message(STATUS "CMAKE_SYSROOT:      ${CMAKE_SYSROOT}")
message(STATUS "proxy-wasm-cpp-sdk: ${PROXY_WASM_CPP_SDK}")
message(STATUS "CXX compiler:       ${CMAKE_CXX_COMPILER}")
message(STATUS "Build type:         ${CMAKE_BUILD_TYPE}")
message(STATUS "Configured sample_filter.wasm target (proxy-wasm 0.2.1 ABI, SDK-based)")
