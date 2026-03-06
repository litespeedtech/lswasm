cmake_minimum_required(VERSION 3.16)
project(lswasm VERSION 1.0.0 LANGUAGES CXX)

# V8 ≥ 12.x requires C++20 (uses `requires` clauses in internal headers).
# Other runtimes work fine with C++17.
if(WASM_RUNTIME STREQUAL "v8")
  set(CMAKE_CXX_STANDARD 20)
else()
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WASM runtime selection: "wasmtime" (default), "v8", "wasmedge", "wamr", or "" (Null VM only).
set(WASM_RUNTIME "wasmtime" CACHE STRING "WASM runtime to use (wasmtime, v8, wasmedge, wamr, or empty for Null VM)")

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
# V8_ROOT: path to a V8 source tree with a completed wee8 build (e.g. ~/v8).
# Expected layout:
#   V8_ROOT/include/v8.h
#   V8_ROOT/src/wasm/c-api.h
#   V8_ROOT/third_party/wasm-api/wasm.hh
#   V8_ROOT/out/wee8/obj/libwee8.a
elseif(WASM_RUNTIME STREQUAL "v8")
  set(V8_ROOT "" CACHE PATH "Path to V8 source tree (with out/wee8/obj/libwee8.a)")

  if(V8_ROOT AND IS_DIRECTORY "${V8_ROOT}")
    # --- V8 from source tree ---
    # Determine build output directory (allow V8_BUILD_DIR override, default out/wee8).
    set(V8_BUILD_DIR "${V8_ROOT}/out/wee8" CACHE PATH "V8 build output directory")

    # Verify key files exist.
    if(NOT EXISTS "${V8_ROOT}/include/v8.h")
      message(FATAL_ERROR "V8_ROOT=${V8_ROOT} but include/v8.h not found.")
    endif()
    if(NOT EXISTS "${V8_BUILD_DIR}/obj/libwee8.a")
      message(FATAL_ERROR "libwee8.a not found at ${V8_BUILD_DIR}/obj/.\n"
        "Build it with: ninja -C ${V8_BUILD_DIR} wee8")
    endif()

    # proxy-wasm-cpp-host V8 backend includes:
    #   "include/v8.h"         → needs V8_ROOT on include path
    #   "src/wasm/c-api.h"     → needs V8_ROOT on include path
    #   "wasm-api/wasm.hh"     → needs V8_ROOT/third_party on include path
    #   "absl/strings/..."     → needs abseil (system or V8_ROOT/third_party/abseil-cpp)
    set(V8_INCLUDE_DIRS
      "${V8_ROOT}"
      "${V8_ROOT}/include"
      "${V8_ROOT}/third_party"
    )
    # Use V8's bundled abseil if available, else fall back to system.
    if(IS_DIRECTORY "${V8_ROOT}/third_party/abseil-cpp")
      list(APPEND V8_INCLUDE_DIRS "${V8_ROOT}/third_party/abseil-cpp")
    endif()
    # V8's generated headers (v8-gn.h, etc.).
    if(IS_DIRECTORY "${V8_BUILD_DIR}/gen/include")
      list(APPEND V8_INCLUDE_DIRS "${V8_BUILD_DIR}/gen/include")
    endif()

    include_directories(${V8_INCLUDE_DIRS})

    # ---- V8 libc++ ABI compatibility ----
    # V8 GN builds use Clang + a bundled libc++ by default (use_custom_libcxx=true).
    # To link against libwee8.a we MUST compile with the same libc++, otherwise
    # C++ symbol mangling differs (libc++ __Cr namespace vs libstdc++).
    #
    # This requires using V8's bundled Clang (or a compatible system Clang) — pass:
    #   -DCMAKE_C_COMPILER=<V8_ROOT>/third_party/llvm-build/Release+Asserts/bin/clang
    #   -DCMAKE_CXX_COMPILER=<V8_ROOT>/third_party/llvm-build/Release+Asserts/bin/clang++
    #
    # We auto-detect the libc++ headers, sysroot, and archives from the V8 tree.
    set(V8_LIBCXX_GENDIR "${V8_BUILD_DIR}/gen/third_party/libc++/src/include")
    set(V8_LIBCXXABI_DIR "${V8_ROOT}/third_party/libc++abi/src/include")
    set(V8_SYSROOT      "${V8_ROOT}/build/linux/debian_bullseye_amd64-sysroot")
    set(V8_LIBCXX_A     "${V8_BUILD_DIR}/obj/buildtools/third_party/libc++/libc++.a")
    set(V8_LIBCXXABI_A  "${V8_BUILD_DIR}/obj/buildtools/third_party/libc++abi/libc++abi.a")

    # Find clang_rt.builtins (needed to replace libgcc).
    file(GLOB V8_CLANG_RT_BUILTINS "${V8_ROOT}/third_party/llvm-build/Release+Asserts/lib/clang/*/lib/x86_64-unknown-linux-gnu/libclang_rt.builtins.a")

    set(V8_USE_BUNDLED_LIBCXX FALSE)
    if(IS_DIRECTORY "${V8_LIBCXX_GENDIR}" AND EXISTS "${V8_LIBCXX_A}" AND EXISTS "${V8_LIBCXXABI_A}")
      set(V8_USE_BUNDLED_LIBCXX TRUE)
      message(STATUS "  V8 bundled libc++ detected — will use for ABI compatibility")
    else()
      message(WARNING "V8 bundled libc++ not found. Linking may fail due to ABI mismatch.\n"
        "  Expected: ${V8_LIBCXX_A}\n"
        "  If V8 was built with use_custom_libcxx=false, GCC/libstdc++ should work.")
    endif()

    # Compile flags for V8 libc++ compatibility.
    # We use -nostdinc++ to suppress the system C++ headers and instead point
    # at V8's generated libc++ headers.  We do NOT use --sysroot because the
    # V8 sysroot targets an older glibc (Bullseye) and would conflict with
    # the host's dynamically linked libraries (OpenSSL, etc.).
    if(V8_USE_BUNDLED_LIBCXX)
      set(V8_CXX_FLAGS "-nostdinc++ -isystem${V8_LIBCXX_GENDIR} -isystem${V8_LIBCXXABI_DIR}")
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${V8_CXX_FLAGS}")
      message(STATUS "  V8 CXX flags: ${V8_CXX_FLAGS}")

      # V8's libc++ objects use LLVM .eh_frame format which GNU ld cannot parse.
      # Use LLVM's lld linker (bundled with V8's Clang) for compatibility.
      set(V8_LLD "${V8_ROOT}/third_party/llvm-build/Release+Asserts/bin/ld.lld")
      if(EXISTS "${V8_LLD}")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=${V8_LLD}")
        message(STATUS "  Using V8's lld linker: ${V8_LLD}")
      else()
        # Try system lld as fallback.
        find_program(LLD_PROGRAM ld.lld)
        if(LLD_PROGRAM)
          set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
          message(STATUS "  Using system lld linker")
        else()
          message(WARNING "lld not found. GNU ld may fail to link V8's libc++ objects (.eh_frame errors).")
        endif()
      endif()
    endif()

    # Extract compile defines from the ninja build so that our compilation
    # matches the ABI of the prebuilt libwee8.a.  We capture:
    #   -DV8_*        – V8 engine config
    #   -DCPPGC_*     – cppgc garbage-collector config
    #   -D_LIBCPP_*   – libc++ ABI / hardening config
    #   -D_LIBCXXABI_* – libc++abi config
    set(V8_COMPILE_DEFS "")
    set(_v8_ninja "${V8_BUILD_DIR}/obj/wee8.ninja")
    if(EXISTS "${_v8_ninja}")
      file(READ "${_v8_ninja}" _v8_ninja_content)
      # Match all relevant -D tokens (including those with =value).
      string(REGEX MATCHALL "-D(V8_[A-Za-z0-9_]+(=[^ ]*)?|CPPGC_[A-Za-z0-9_]+(=[^ ]*)?|_LIBCPP_[A-Za-z0-9_]+(=[^ ]*)?|_LIBCXXABI_[A-Za-z0-9_]+(=[^ ]*)?)" _v8_def_flags "${_v8_ninja_content}")
      foreach(_flag IN LISTS _v8_def_flags)
        # Strip leading -D
        string(REGEX REPLACE "^-D" "" _def "${_flag}")
        list(APPEND V8_COMPILE_DEFS "${_def}")
      endforeach()
      list(REMOVE_DUPLICATES V8_COMPILE_DEFS)
      message(STATUS "  V8 compile definitions (${CMAKE_CURRENT_LIST_LINE}): ${V8_COMPILE_DEFS}")
    else()
      # Minimal required set if ninja file is not available.
      set(V8_COMPILE_DEFS
        V8_ENABLE_WEBASSEMBLY
        V8_COMPRESS_POINTERS
        V8_COMPRESS_POINTERS_IN_SHARED_CAGE
        V8_31BIT_SMIS_ON_64BIT_ARCH
        V8_ENABLE_SANDBOX
      )
      message(STATUS "  Using default V8 compile definitions (wee8.ninja not found)")
    endif()

    # libwee8.a is the WASM C-API static archive (monolithic).
    set(V8_WEE8_LIB "${V8_BUILD_DIR}/obj/libwee8.a")
    # Also pick up support archives if they exist.
    set(V8_LIBBASE   "${V8_BUILD_DIR}/obj/libv8_libbase.a")
    set(V8_LIBPLATFORM "${V8_BUILD_DIR}/obj/libv8_libplatform.a")

    list(APPEND PROXY_WASM_SOURCES
      "${PROXY_WASM_HOST_DIR}/src/v8/v8.cc"
    )
    set(HAVE_RUNTIME TRUE)
    message(STATUS "Using V8 source tree at ${V8_ROOT}")
    message(STATUS "  wee8: ${V8_WEE8_LIB}")
  else()
    # --- System-installed V8 ---
    find_package(v8 QUIET)
    if(v8_FOUND)
      list(APPEND PROXY_WASM_SOURCES
        "${PROXY_WASM_HOST_DIR}/src/v8/v8.cc"
      )
      set(HAVE_RUNTIME TRUE)
      message(STATUS "V8 found via CMake config and enabled")
    else()
      message(FATAL_ERROR "WASM_RUNTIME=v8 but V8 not found.\n"
        "Set -DV8_ROOT=/path/to/v8 to point at a V8 source tree,\n"
        "or install V8 development libraries system-wide.")
    endif()
  endif()

# ---- WasmEdge ----
elseif(WASM_RUNTIME STREQUAL "wasmedge")
  # Try pkg-config first (standard WasmEdge install provides wasmedge.pc).
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(WASMEDGE QUIET wasmedge)
  endif()

  if(WASMEDGE_FOUND)
    include_directories(${WASMEDGE_INCLUDE_DIRS})
    link_directories(${WASMEDGE_LIBRARY_DIRS})
    list(APPEND PROXY_WASM_SOURCES
      "${PROXY_WASM_HOST_DIR}/src/wasmedge/wasmedge.cc"
    )
    set(HAVE_RUNTIME TRUE)
    message(STATUS "WasmEdge found via pkg-config and enabled")
  else()
    # Fallback: try find_path / find_library for manual installs.
    find_path(WASMEDGE_INCLUDE_DIR wasmedge/wasmedge.h
      PATHS /usr/local/include /usr/include
            "$ENV{HOME}/.wasmedge/include"
            "${CMAKE_SOURCE_DIR}/third_party/wasmedge/include")
    find_library(WASMEDGE_LIBRARY wasmedge
      PATHS /usr/local/lib /usr/lib /usr/lib64
            "$ENV{HOME}/.wasmedge/lib"
            "${CMAKE_SOURCE_DIR}/third_party/wasmedge/lib")

    if(WASMEDGE_INCLUDE_DIR AND WASMEDGE_LIBRARY)
      include_directories(${WASMEDGE_INCLUDE_DIR})
      get_filename_component(WASMEDGE_LIB_DIR "${WASMEDGE_LIBRARY}" DIRECTORY)
      link_directories(${WASMEDGE_LIB_DIR})
      list(APPEND PROXY_WASM_SOURCES
        "${PROXY_WASM_HOST_DIR}/src/wasmedge/wasmedge.cc"
      )
      set(HAVE_RUNTIME TRUE)
      message(STATUS "WasmEdge found at ${WASMEDGE_LIBRARY}")
    else()
      message(FATAL_ERROR "WASM_RUNTIME=wasmedge but WasmEdge not found.\n"
        "Install via: curl -sSf https://raw.githubusercontent.com/WasmEdge/WasmEdge/master/utils/install.sh | bash\n"
        "Or see: https://wasmedge.org/docs/start/install")
    endif()
  endif()

# ---- WAMR (WebAssembly Micro Runtime) ----
elseif(WASM_RUNTIME STREQUAL "wamr")
  # Try pkg-config first.
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(WAMR QUIET iwasm)
  endif()

  if(WAMR_FOUND)
    include_directories(${WAMR_INCLUDE_DIRS})
    link_directories(${WAMR_LIBRARY_DIRS})
    list(APPEND PROXY_WASM_SOURCES
      "${PROXY_WASM_HOST_DIR}/src/wamr/wamr.cc"
    )
    set(HAVE_RUNTIME TRUE)
    message(STATUS "WAMR found via pkg-config and enabled")
  else()
    # Fallback: try find_path / find_library for manual or source-built installs.
    find_path(WAMR_INCLUDE_DIR wasm_c_api.h
      PATHS /usr/local/include /usr/include
            "${CMAKE_SOURCE_DIR}/third_party/wamr/core/iwasm/include"
            "${CMAKE_SOURCE_DIR}/third_party/wasm-micro-runtime/core/iwasm/include")
    find_library(WAMR_LIBRARY NAMES iwasm vmlib
      PATHS /usr/local/lib /usr/lib /usr/lib64
            "${CMAKE_SOURCE_DIR}/third_party/wamr/build"
            "${CMAKE_SOURCE_DIR}/third_party/wasm-micro-runtime/build"
            "${CMAKE_SOURCE_DIR}/third_party/wasm-micro-runtime/product-mini/platforms/linux/build")

    if(WAMR_INCLUDE_DIR AND WAMR_LIBRARY)
      include_directories(${WAMR_INCLUDE_DIR})
      get_filename_component(WAMR_LIB_DIR "${WAMR_LIBRARY}" DIRECTORY)
      link_directories(${WAMR_LIB_DIR})
      list(APPEND PROXY_WASM_SOURCES
        "${PROXY_WASM_HOST_DIR}/src/wamr/wamr.cc"
      )
      set(HAVE_RUNTIME TRUE)
      message(STATUS "WAMR found at ${WAMR_LIBRARY}")
    else()
      message(FATAL_ERROR "WASM_RUNTIME=wamr but WAMR not found.\n"
        "Build from source: https://github.com/bytecodealliance/wasm-micro-runtime\n"
        "  cmake -Bbuild -DWAMR_BUILD_LIB_WASI_THREADS=1 && cmake --build build")
    endif()
  endif()

elseif(NOT WASM_RUNTIME STREQUAL "")
  message(FATAL_ERROR "Unknown WASM_RUNTIME value: '${WASM_RUNTIME}'. Use 'wasmtime', 'v8', 'wasmedge', 'wamr', or empty string.")
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

# Propagate the chosen runtime as a compile definition: WASM_RUNTIME_WASMTIME, WASM_RUNTIME_V8, WASM_RUNTIME_WASMEDGE, or WASM_RUNTIME_WAMR.
if(WASM_RUNTIME STREQUAL "wasmtime" AND HAVE_RUNTIME)
  target_link_libraries(proxy-wasm-host PUBLIC wasmtime)
  target_compile_definitions(proxy-wasm-host PUBLIC WASM_RUNTIME_WASMTIME)
elseif(WASM_RUNTIME STREQUAL "v8" AND HAVE_RUNTIME)
  if(V8_ROOT AND IS_DIRECTORY "${V8_ROOT}")
    # Link against the static wee8 archive from the V8 source build.
    target_link_libraries(proxy-wasm-host PUBLIC "${V8_WEE8_LIB}")
    if(EXISTS "${V8_LIBBASE}")
      target_link_libraries(proxy-wasm-host PUBLIC "${V8_LIBBASE}")
    endif()
    if(EXISTS "${V8_LIBPLATFORM}")
      target_link_libraries(proxy-wasm-host PUBLIC "${V8_LIBPLATFORM}")
    endif()
    # V8 links against ICU, zlib, partition_alloc static archives,
    # Rust .rlib libraries (for Temporal API), and clang_rt builtins.
    # Extract all link dependencies from the wee8.ninja build line.
    if(EXISTS "${_v8_ninja}")
      # Extract all .rlib and .a dependencies from the wee8.ninja file.
      # _v8_ninja_content was already read above for compile definitions.
      string(REGEX MATCHALL "[^ ]+\\.(rlib|a)" _v8_link_deps "${_v8_ninja_content}")
      set(V8_EXTRA_LIBS "")
      foreach(_dep IN LISTS _v8_link_deps)
        # Skip libwee8.a itself (already linked) and .o files.
        if(_dep MATCHES "libwee8\\.a$")
          continue()
        endif()
        # Resolve relative paths against V8_BUILD_DIR (ninja builddir).
        # All paths in wee8.ninja are relative to the top-level build dir.
        if(IS_ABSOLUTE "${_dep}")
          set(_abs "${_dep}")
        else()
          get_filename_component(_abs "${V8_BUILD_DIR}/${_dep}" ABSOLUTE)
        endif()
        if(EXISTS "${_abs}")
          list(APPEND V8_EXTRA_LIBS "${_abs}")
        endif()
      endforeach()
      list(REMOVE_DUPLICATES V8_EXTRA_LIBS)
      list(LENGTH V8_EXTRA_LIBS _v8_extra_count)
      message(STATUS "  V8 extra link dependencies: ${_v8_extra_count} archives")
      if(V8_EXTRA_LIBS)
        target_link_libraries(proxy-wasm-host PUBLIC ${V8_EXTRA_LIBS})
      endif()
    else()
      # Fallback: link known V8 dependencies manually.
      set(V8_ICU_I18N "${V8_BUILD_DIR}/obj/third_party/icu/libicui18n.a")
      set(V8_ICU_UC   "${V8_BUILD_DIR}/obj/third_party/icu/libicuuc.a")
      set(V8_ZLIB     "${V8_BUILD_DIR}/obj/third_party/zlib/libchrome_zlib.a")
      foreach(_v8_dep IN ITEMS V8_ICU_I18N V8_ICU_UC V8_ZLIB)
        if(EXISTS "${${_v8_dep}}")
          target_link_libraries(proxy-wasm-host PUBLIC "${${_v8_dep}}")
        endif()
      endforeach()
    endif()
    # Link V8's bundled libc++ and libc++abi for ABI compatibility.
    if(V8_USE_BUNDLED_LIBCXX)
      target_link_libraries(proxy-wasm-host PUBLIC "${V8_LIBCXX_A}" "${V8_LIBCXXABI_A}")
      if(V8_CLANG_RT_BUILTINS)
        target_link_libraries(proxy-wasm-host PUBLIC "${V8_CLANG_RT_BUILTINS}")
      endif()
    endif()
    # System libs V8 depends on.
    target_link_libraries(proxy-wasm-host PUBLIC dl)
    # Propagate V8's own compile definitions so internal headers match the built library.
    target_compile_definitions(proxy-wasm-host PUBLIC ${V8_COMPILE_DEFS})
  elseif(v8_FOUND)
    target_link_libraries(proxy-wasm-host PUBLIC v8)
  endif()
  target_compile_definitions(proxy-wasm-host PUBLIC WASM_RUNTIME_V8)
elseif(WASM_RUNTIME STREQUAL "wasmedge" AND HAVE_RUNTIME)
  if(WASMEDGE_FOUND)
    target_link_libraries(proxy-wasm-host PUBLIC ${WASMEDGE_LIBRARIES})
  else()
    target_link_libraries(proxy-wasm-host PUBLIC "${WASMEDGE_LIBRARY}")
  endif()
  target_compile_definitions(proxy-wasm-host PUBLIC WASM_RUNTIME_WASMEDGE)
elseif(WASM_RUNTIME STREQUAL "wamr" AND HAVE_RUNTIME)
  if(WAMR_FOUND)
    target_link_libraries(proxy-wasm-host PUBLIC ${WAMR_LIBRARIES})
  else()
    target_link_libraries(proxy-wasm-host PUBLIC "${WAMR_LIBRARY}")
  endif()
  target_compile_definitions(proxy-wasm-host PUBLIC WASM_RUNTIME_WAMR)
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
  # V8 libraries are already propagated via proxy-wasm-host (PUBLIC linkage).
  if(NOT V8_ROOT OR NOT IS_DIRECTORY "${V8_ROOT}")
    if(v8_FOUND)
      target_link_libraries(lswasm PRIVATE v8)
    endif()
  endif()
  target_compile_definitions(lswasm PRIVATE WASM_RUNTIME_V8)
elseif(WASM_RUNTIME STREQUAL "wasmedge" AND HAVE_RUNTIME)
  # WasmEdge libraries are already propagated via proxy-wasm-host (PUBLIC linkage).
  target_compile_definitions(lswasm PRIVATE WASM_RUNTIME_WASMEDGE)
elseif(WASM_RUNTIME STREQUAL "wamr" AND HAVE_RUNTIME)
  # WAMR libraries are already propagated via proxy-wasm-host (PUBLIC linkage).
  target_compile_definitions(lswasm PRIVATE WASM_RUNTIME_WAMR)
endif()

# Pass project version to C++ code.
target_compile_definitions(lswasm PRIVATE
  LSWASM_VERSION="${PROJECT_VERSION}"
  LSWASM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  LSWASM_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  LSWASM_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

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
