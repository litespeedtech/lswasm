# cmake/wasm32-wasi-toolchain.cmake — CMake toolchain file for wasm32-wasi.
#
# Usage:
#   cmake -S samples -B samples/build \
#     -DCMAKE_TOOLCHAIN_FILE=<path>/cmake/wasm32-wasi-toolchain.cmake \
#     -DWASI_SDK_PATH=/path/to/wasi-sdk-29.0
#
# The WASI SDK bundles clang, wasm-ld, and a wasi-sysroot with C++ headers
# (libc++/libc++abi).  Download from:
#   https://github.com/WebAssembly/wasi-sdk/releases

# ---------------------------------------------------------------------------
# WASI SDK discovery
# ---------------------------------------------------------------------------
if(NOT WASI_SDK_PATH)
  if(DEFINED ENV{WASI_SDK_PATH})
    set(WASI_SDK_PATH "$ENV{WASI_SDK_PATH}" CACHE PATH
        "Path to the WASI SDK installation (e.g. /opt/wasi-sdk-29.0)" FORCE)
  endif()
endif()

if(NOT WASI_SDK_PATH OR NOT EXISTS "${WASI_SDK_PATH}/bin/clang++")
  message(FATAL_ERROR
    "WASI SDK not found.  Set -DWASI_SDK_PATH=<path> or export WASI_SDK_PATH.\n"
    "The SDK is required for C++ standard library support (libc++/libc++abi).\n"
    "Download from: https://github.com/WebAssembly/wasi-sdk/releases")
endif()

# Ensure WASI_SDK_PATH is forwarded to CMake's internal try_compile calls
# (compiler ABI detection, etc.) so this toolchain file can resolve it there.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES WASI_SDK_PATH)

# ---------------------------------------------------------------------------
# Cross-compilation settings
# ---------------------------------------------------------------------------
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

set(WASI_SYSROOT "${WASI_SDK_PATH}/share/wasi-sysroot")

set(CMAKE_C_COMPILER   "${WASI_SDK_PATH}/bin/clang"   CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${WASI_SDK_PATH}/bin/clang++" CACHE FILEPATH "CXX compiler" FORCE)
set(CMAKE_C_COMPILER_TARGET   "wasm32-wasi" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER_TARGET "wasm32-wasi" CACHE STRING "" FORCE)
set(CMAKE_SYSROOT "${WASI_SYSROOT}" CACHE PATH "" FORCE)

# Skip CMake's test-compile step (the wasm target cannot produce executables
# for the host).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
