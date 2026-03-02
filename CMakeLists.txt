cmake_minimum_required(VERSION 3.16)
project(lswasm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WASM runtime selection: "wasmtime" (default), "v8", or "" (Null VM only).
set(WASM_RUNTIME "wasmtime" CACHE STRING "WASM runtime to use (wasmtime, v8, or empty for Null VM)")

# Find required packages
find_package(OpenSSL REQUIRED)

# Proxy-wasm-cpp-host configuration
set(PROXY_WASM_HOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/proxy-wasm-cpp-host")
set(PROXY_WASM_SPEC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/proxy-wasm-spec")
set(PROXY_WASM_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/proxy-wasm-cpp-sdk")

# Collect proxy-wasm sources (explicitly list to avoid abseil dependency on hash.cc)
set(PROXY_WASM_SOURCES
  "${PROXY_WASM_HOST_DIR}/src/bytecode_util.cc"
  "${PROXY_WASM_HOST_DIR}/src/context.cc"
  "${PROXY_WASM_HOST_DIR}/src/exports.cc"
  "${PROXY_WASM_HOST_DIR}/src/pairs_util.cc"
  "${PROXY_WASM_HOST_DIR}/src/shared_data.cc"
  "${PROXY_WASM_HOST_DIR}/src/shared_queue.cc"
  "${PROXY_WASM_HOST_DIR}/src/signature_util.cc"
  "${PROXY_WASM_HOST_DIR}/src/vm_id_handle.cc"
  "${PROXY_WASM_HOST_DIR}/src/wasm.cc"
)

# Runtime detection flag
set(HAVE_RUNTIME FALSE)

# ---- Wasmtime ----
if(WASM_RUNTIME STREQUAL "wasmtime")
  # Try to detect a system-installed wasmtime C library (via CMake config)
  find_package(wasmtime QUIET)
  if(wasmtime_FOUND)
    list(APPEND PROXY_WASM_SOURCES
      "${PROXY_WASM_HOST_DIR}/src/wasmtime/wasmtime.cc"
    )
    set(HAVE_RUNTIME TRUE)
    message(STATUS "Wasmtime found and enabled")
  else()
    # Fallback: maybe user built wasmtime from source in third_party
    set(WASMTIME_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/wasmtime-src")
    find_path(WASMTIME_INCLUDE_DIR wasmtime.h PATHS "${WASMTIME_SRC_DIR}/crates/c-api/include")
    find_library(WASMTIME_LIBRARY wasmtime PATHS "${WASMTIME_SRC_DIR}/target/release")
    if(WASMTIME_INCLUDE_DIR AND WASMTIME_LIBRARY)
      include_directories(${WASMTIME_INCLUDE_DIR})
      include_directories("${WASMTIME_SRC_DIR}")
      # include generated headers (conf.h) produced during wasmtime-c-api build
      file(GLOB WASMTIME_GENERATED_INCLUDE_DIR
           "${WASMTIME_SRC_DIR}/target/release/build/wasmtime-c-api-*/out/include")
      if(WASMTIME_GENERATED_INCLUDE_DIR)
        include_directories(${WASMTIME_GENERATED_INCLUDE_DIR})
      endif()
      link_directories("${WASMTIME_SRC_DIR}/target/release")
      list(APPEND PROXY_WASM_SOURCES
        "${PROXY_WASM_HOST_DIR}/src/wasmtime/wasmtime.cc"
      )
      set(HAVE_RUNTIME TRUE)
      message(STATUS "Using locally built Wasmtime library from ${WASMTIME_SRC_DIR}")
    else()
      message(FATAL_ERROR "WASM_RUNTIME=wasmtime but Wasmtime not found.\n"
        "Install via: cargo install wasmtime-cli or apt install libwasmtime-dev")
    endif()
  endif()

# ---- V8 ----
elseif(WASM_RUNTIME STREQUAL "v8")
  find_package(v8 QUIET)
  if(v8_FOUND)
    list(APPEND PROXY_WASM_SOURCES
      "${PROXY_WASM_HOST_DIR}/src/v8/v8.cc"
    )
    set(HAVE_RUNTIME TRUE)
    message(STATUS "V8 found via CMake config and enabled")
  else()
    # Fallback: try to find V8 via header/library search.
    find_path(V8_INCLUDE_DIR v8.h
      PATHS /usr/include /usr/local/include /usr/include/node /usr/include/nodejs/deps/v8/include)
    find_library(V8_LIBRARY v8
      PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)
    if(V8_INCLUDE_DIR AND V8_LIBRARY)
      include_directories(${V8_INCLUDE_DIR})
      # proxy-wasm-cpp-host V8 backend uses #include "include/v8-*.h", so we also
      # need the parent of the "include/" directory containing V8 headers.
      # For Node.js layout: /usr/include/nodejs/deps/v8/include/ → add /usr/include/nodejs/deps/v8/
      find_path(V8_PARENT_DIR include/v8-initialization.h
        PATHS /usr/include/nodejs/deps/v8 /usr/local/include)
      if(V8_PARENT_DIR)
        include_directories(${V8_PARENT_DIR})
      endif()
      list(APPEND PROXY_WASM_SOURCES
        "${PROXY_WASM_HOST_DIR}/src/v8/v8.cc"
      )
      set(HAVE_RUNTIME TRUE)
      message(STATUS "V8 found at ${V8_INCLUDE_DIR} / ${V8_LIBRARY}")
    else()
      message(FATAL_ERROR "WASM_RUNTIME=v8 but V8 not found.\n"
        "Install the V8 development libraries for your platform.")
    endif()
  endif()

elseif(NOT WASM_RUNTIME STREQUAL "")
  message(FATAL_ERROR "Unknown WASM_RUNTIME value: '${WASM_RUNTIME}'. Use 'wasmtime', 'v8', or empty string.")
endif()

# Add null VM support (always available)
list(APPEND PROXY_WASM_SOURCES
  "${PROXY_WASM_HOST_DIR}/src/null/null.cc"
  "${PROXY_WASM_HOST_DIR}/src/null/null_vm.cc"
  "${PROXY_WASM_HOST_DIR}/src/null/null_plugin.cc"
)

# Create proxy-wasm library
add_library(proxy-wasm-host STATIC ${PROXY_WASM_SOURCES})

# Include directories for proxy-wasm library
target_include_directories(proxy-wasm-host PUBLIC
  "${PROXY_WASM_HOST_DIR}/include"
  "${PROXY_WASM_SDK_DIR}"
)

# Private includes for source files
target_include_directories(proxy-wasm-host PRIVATE
  "${PROXY_WASM_HOST_DIR}"
  "${PROXY_WASM_HOST_DIR}/src"
)

# Link dependencies - only OpenSSL for now
target_link_libraries(proxy-wasm-host 
  PUBLIC OpenSSL::SSL OpenSSL::Crypto
)

# Propagate the chosen runtime as a compile definition: WASM_RUNTIME_WASMTIME or WASM_RUNTIME_V8.
if(WASM_RUNTIME STREQUAL "wasmtime" AND HAVE_RUNTIME)
  target_link_libraries(proxy-wasm-host PUBLIC wasmtime)
  target_compile_definitions(proxy-wasm-host PUBLIC WASM_RUNTIME_WASMTIME)
elseif(WASM_RUNTIME STREQUAL "v8" AND HAVE_RUNTIME)
  if(v8_FOUND)
    target_link_libraries(proxy-wasm-host PUBLIC v8)
  else()
    target_link_libraries(proxy-wasm-host PUBLIC ${V8_LIBRARY})
    # V8 typically also needs libv8_libplatform and libv8_libbase.
    find_library(V8_PLATFORM_LIBRARY v8_libplatform PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)
    find_library(V8_BASE_LIBRARY v8_libbase PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)
    if(V8_PLATFORM_LIBRARY)
      target_link_libraries(proxy-wasm-host PUBLIC ${V8_PLATFORM_LIBRARY})
    endif()
    if(V8_BASE_LIBRARY)
      target_link_libraries(proxy-wasm-host PUBLIC ${V8_BASE_LIBRARY})
    endif()
  endif()
  target_compile_definitions(proxy-wasm-host PUBLIC WASM_RUNTIME_V8)
endif()

# Main executable - HTTP proxy server with proxy-wasm filter support
add_executable(lswasm
  src/main.cpp
  src/wasm_module_manager.cc
  src/hash_shim.cc
)

target_include_directories(lswasm PRIVATE 
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${PROXY_WASM_HOST_DIR}/include"
  "${PROXY_WASM_HOST_DIR}"
  "${PROXY_WASM_HOST_DIR}/src"
  "${PROXY_WASM_SDK_DIR}"
)

target_link_libraries(lswasm PRIVATE 
  proxy-wasm-host
  OpenSSL::SSL 
  OpenSSL::Crypto
  pthread
)

if(WASM_RUNTIME STREQUAL "wasmtime" AND HAVE_RUNTIME)
  target_link_libraries(lswasm PRIVATE wasmtime)
  target_compile_definitions(lswasm PRIVATE WASM_RUNTIME_WASMTIME)
elseif(WASM_RUNTIME STREQUAL "v8" AND HAVE_RUNTIME)
  if(v8_FOUND)
    target_link_libraries(lswasm PRIVATE v8)
  else()
    target_link_libraries(lswasm PRIVATE ${V8_LIBRARY})
    if(V8_PLATFORM_LIBRARY)
      target_link_libraries(lswasm PRIVATE ${V8_PLATFORM_LIBRARY})
    endif()
    if(V8_BASE_LIBRARY)
      target_link_libraries(lswasm PRIVATE ${V8_BASE_LIBRARY})
    endif()
  endif()
  target_compile_definitions(lswasm PRIVATE WASM_RUNTIME_V8)
endif()

# Compilation flags
if(UNIX)
  if(APPLE)
    target_compile_options(proxy-wasm-host PRIVATE -fPIC)
    target_compile_options(lswasm PRIVATE -fPIC)
  endif()
endif()

# Print summary
message(STATUS "=== WASM Proxy Build Configuration ===")
message(STATUS "WASM runtime: ${WASM_RUNTIME}")
message(STATUS "Runtime found: ${HAVE_RUNTIME}")
message(STATUS "Submodules:")
message(STATUS "  - proxy-wasm-cpp-host: ${PROXY_WASM_HOST_DIR}")
message(STATUS "  - proxy-wasm-cpp-sdk: ${PROXY_WASM_SDK_DIR}")
message(STATUS "  - proxy-wasm-spec: ${PROXY_WASM_SPEC_DIR}")
message(STATUS "ABI headers from SDK: proxy_wasm_common.h, proxy_wasm_enums.h")
message(STATUS "=========================================)")
