cmake_minimum_required(VERSION 3.16)
project(lswasm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options for runtime selection
option(ENABLE_WASMER "Enable Wasmer runtime support" ON)
option(ENABLE_WASMTIME "Enable Wasmtime runtime support" ON)

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

# Detection flags
set(HAVE_WASMTIME FALSE)
set(HAVE_WASMER FALSE)

# Add Wasmtime support if enabled
if(ENABLE_WASMTIME)
  # Try to detect a system-installed wasmtime C library (via CMake config)
  find_package(wasmtime QUIET)
  if(wasmtime_FOUND)
    list(APPEND PROXY_WASM_SOURCES
      "${PROXY_WASM_HOST_DIR}/src/wasmtime/wasmtime.cc"
    )
    set(HAVE_WASMTIME TRUE)
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
      set(HAVE_WASMTIME TRUE)
      message(STATUS "Using locally built Wasmtime library from ${WASMTIME_SRC_DIR}")
    else()
      message(STATUS "Wasmtime not found - install via: cargo install wasmtime-cli or apt install libwasmtime-dev")
    endif()
  endif()
endif()

# Add Wasmer support if enabled
if(ENABLE_WASMER)
  find_package(wasmer QUIET)
  if(wasmer_FOUND)
    set(HAVE_WASMER TRUE)
    message(STATUS "Wasmer found and enabled")
  else()
    message(STATUS "Wasmer not found - install via: curl https://get.wasmer.io -sSfL | sh")
  endif()
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

if(HAVE_WASMTIME)
  target_link_libraries(proxy-wasm-host PUBLIC wasmtime)
  target_compile_definitions(proxy-wasm-host PUBLIC ENABLE_WASMTIME)
endif()

if(HAVE_WASMER)
  target_link_libraries(proxy-wasm-host PUBLIC wasmer)
  target_compile_definitions(proxy-wasm-host PUBLIC ENABLE_WASMER)
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

if(HAVE_WASMTIME)
  target_link_libraries(lswasm PRIVATE wasmtime)
  target_compile_definitions(lswasm PRIVATE ENABLE_WASMTIME)
endif()

if(HAVE_WASMER)
  target_link_libraries(lswasm PRIVATE wasmer)
  target_compile_definitions(lswasm PRIVATE ENABLE_WASMER)
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
message(STATUS "Wasmer support: ${HAVE_WASMER}")
message(STATUS "Wasmtime support: ${HAVE_WASMTIME}")
message(STATUS "Submodules:")
message(STATUS "  - proxy-wasm-cpp-host: ${PROXY_WASM_HOST_DIR}")
message(STATUS "  - proxy-wasm-cpp-sdk: ${PROXY_WASM_SDK_DIR}")
message(STATUS "  - proxy-wasm-spec: ${PROXY_WASM_SPEC_DIR}")
message(STATUS "ABI headers from SDK: proxy_wasm_common.h, proxy_wasm_enums.h")
message(STATUS "=========================================)")
