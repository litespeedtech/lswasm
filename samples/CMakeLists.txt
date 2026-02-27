cmake_minimum_required(VERSION 3.16)

# Force clang for wasm32-wasi cross-compilation.
# These must be set before project() so CMake uses them during compiler detection.
find_program(CLANG_C_COMPILER NAMES clang clang-18 clang-17 clang-16 clang-15 clang-14 REQUIRED)
set(CMAKE_C_COMPILER "${CLANG_C_COMPILER}" CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_C_COMPILER_TARGET "wasm32-wasi" CACHE STRING "" FORCE)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

# Skip CMake's test-compile step (the wasm linker may not be on PATH yet).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Locate the wasi-sysroot shipped by the distro or WASI SDK.
if(NOT DEFINED WASI_SYSROOT)
  if(DEFINED ENV{WASI_SYSROOT})
    set(WASI_SYSROOT "$ENV{WASI_SYSROOT}")
  elseif(EXISTS "/usr/share/wasi-sysroot")
    set(WASI_SYSROOT "/usr/share/wasi-sysroot")
  elseif(DEFINED ENV{WASI_SDK_PATH})
    set(WASI_SYSROOT "$ENV{WASI_SDK_PATH}/share/wasi-sysroot")
  endif()
endif()

# Locate wasm-ld (provided by lld / lld-<version>).
find_program(WASM_LD NAMES wasm-ld wasm-ld-18 wasm-ld-17 wasm-ld-16 wasm-ld-15 wasm-ld-14)
if(WASM_LD)
  message(STATUS "Found wasm-ld: ${WASM_LD}")
else()
  message(STATUS "wasm-ld not found – install lld (e.g. apt install lld-18)")
endif()

project(sample_filter_wasm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(sample_filter sample_filter.c)

# Compile flags for wasm32-wasi freestanding module.
target_compile_options(sample_filter PRIVATE
  --target=wasm32-wasi
  -O2
  -ffreestanding
  -fvisibility=hidden
)

if(WASI_SYSROOT)
  target_compile_options(sample_filter PRIVATE --sysroot=${WASI_SYSROOT})
  target_link_options(sample_filter PRIVATE --sysroot=${WASI_SYSROOT})
  message(STATUS "Using WASI sysroot: ${WASI_SYSROOT}")
endif()

# Link as a reactor (no _start entry point), export all symbols.
target_link_options(sample_filter PRIVATE
  --target=wasm32-wasi
  -nostdlib
  -Wl,--no-entry
  -Wl,--export-all
  -Wl,--strip-all
)

# If wasm-ld is found under a versioned name, tell clang to use it explicitly.
if(WASM_LD)
  target_link_options(sample_filter PRIVATE -fuse-ld=${WASM_LD})
endif()

# Produce a .wasm file instead of a native binary.
set_target_properties(sample_filter PROPERTIES SUFFIX ".wasm")

message(STATUS "Using C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "Configured sample_filter.wasm target (proxy-wasm 0.2.1 ABI)")
