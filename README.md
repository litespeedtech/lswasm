# WASM HTTP Proxy Server

A C++ HTTP proxy server that can execute WebAssembly (WASM) filter modules using proxy-wasm-cpp-host, with support for **Wasmtime**, **V8**, and **WasmEdge** runtimes.

## Features

- Single-threaded, `epoll`-based HTTP server (Linux)
- TCP and **Unix domain socket** listeners
- WASM filter module loading and execution via proxy-wasm-cpp-host
- HTTP filter chain with short-circuit on local responses (`sendLocalResponse`)
- **Response header manipulation** from WASM modules via proxy-wasm ABI
- Support for Wasmtime, V8, and WasmEdge runtimes (selectable via `-DWASM_RUNTIME=`)
- Per-module environment variables (`--env KEY=VALUE`)
- Graceful shutdown with signal handling (SIGINT, SIGTERM)
- Modular CMake-based build system

## Architecture

```
├── src/
│   ├── main.cpp                # HTTP server (epoll loop, CLI, request handling)
│   ├── http_filter.h           # HTTP filter context (filter chain lifecycle)
│   ├── wasm_module_manager.h   # WASM module manager (load, execute, state)
│   ├── wasm_module_manager.cc  # WASM module manager implementation
│   ├── log.h                   # Debug logging macros (file-based, --debug flag)
│   └── hash_shim.cc            # Hash helper shim
├── samples/
│   ├── sample_filter.cpp       # Example WASM filter (C++ source)
│   ├── sample_filter.wasm      # Pre-compiled sample filter
│   ├── CMakeLists.txt          # Build rules for samples
│   └── README.md               # Sample documentation
├── third_party/
│   └── proxy-wasm-cpp-host/    # Git submodule
└── CMakeLists.txt              # Build configuration
```

### Submodule Integration

The `proxy-wasm-cpp-host` is included as a git submodule at `third_party/proxy-wasm-cpp-host/`. This provides:
- Core WASM module loading and execution
- Proxy-WASM ABI implementation
- Support for multiple WASM runtimes

## Prerequisites

### System Dependencies

```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y build-essential cmake git libssl-dev pkg-config

# macOS
brew install cmake openssl pkg-config
```

### WASM Runtimes

#### Wasmtime (Default)

```bash
# Install Wasmtime
curl https://wasmtime.dev/install.sh -sSf | bash

# Install development files
# Ubuntu/Debian
sudo apt-get install -y libwasmtime-dev

# Or build from source
git clone https://github.com/bytecodealliance/wasmtime.git
cd wasmtime
cargo install --path crates/cli
```

#### V8

> **Important:** The proxy-wasm-cpp-host V8 backend requires the **full V8
> source tree** with a completed build. The Node.js `libnode-dev` package is
> *not* sufficient because proxy-wasm-cpp-host uses internal V8 headers
> (e.g. `src/wasm/c-api.h`) that are not shipped with Node.js.

##### Building V8 from source

Follow the instructions here to install depot_tools, and getting the source tree: https://v8.dev/docs/source-code
In the v8 directory, run:
```
gn gen out/wee8 --args='
  is_debug=false 
  v8_symbol_level=1 
  is_component_build=false 
  v8_enable_i18n_support=false 
  v8_use_external_startup_data=false 
  v8_monolithic=true 
  target_cpu="x64"
  v8_enable_sandbox=false 
'
autoninja -C out/wee8 wee8
```
This is a time-consuming process.

> **Note:** Adjust arguments as needed for your specific platform (e.g., `"arm64"`).

This produces `out/wee8/obj/libwee8.a` (~120 MB, monolithic archive
containing V8, ICU, zlib, and all dependencies).

Refer to the [official V8 build guide](https://v8.dev/docs/build) for
platform-specific prerequisites and troubleshooting.

##### V8 toolchain requirements

V8 builds with its own bundled **Clang** compiler and **libc++** (with a custom
ABI namespace `__Cr`). To link against `libwee8.a`, lswasm **must** be compiled
with the same toolchain:

- **Compiler:** V8's bundled Clang at `<V8_ROOT>/third_party/llvm-build/Release+Asserts/bin/clang++`
- **Linker:** V8's bundled `lld` (auto-detected by CMake from the V8 tree)
- **C++ stdlib:** V8's bundled libc++ headers and static archives (auto-detected)
- **C++ standard:** C++20 (required by V8 ≥ 14.x headers; set automatically)

The CMake build system handles all of these automatically when `V8_ROOT` is
set — you only need to point `CMAKE_CXX_COMPILER` at V8's Clang.

##### V8 link dependencies

Modern V8 (≥ 14.7) uses Rust for the ECMAScript Temporal API. The CMake build
automatically extracts all required `.rlib` and `.a` dependencies from V8's
`wee8.ninja` build file, including:

- ICU (i18n, unicode), zlib, partition_alloc
- ~55 Rust `.rlib` archives (temporal_rs, icu_calendar, diplomat_runtime, etc.)
- Rust standard library sysroot archives
- `libclang_rt.builtins.a`

#### WasmEdge

WasmEdge is a lightweight, high-performance WebAssembly runtime optimized for
cloud-native, edge, and decentralized applications.

```bash
# Quick install (Linux/macOS)
curl -sSf https://raw.githubusercontent.com/WasmEdge/WasmEdge/master/utils/install.sh | bash

# Ubuntu/Debian — install development files
sudo apt-get install -y wasmedge

# Or build from source
git clone https://github.com/WasmEdge/WasmEdge.git
cd WasmEdge
cmake -Bbuild -GNinja -DCMAKE_BUILD_TYPE=Release .
cmake --build build
sudo cmake --install build
```

The WasmEdge C SDK headers (`wasmedge/wasmedge.h`) and shared library
(`libwasmedge.so`) must be installed in a standard system path or in
`third_party/wasmedge/`.

## Building

### 1. Clone with Submodules

```bash
git clone <project-url>
cd lswasm
git submodule update --init --recursive
```

### 2. Create Build Directory

```bash
mkdir build
cd build
```

### 3. Configure and Build

#### With Wasmtime (Default)

```bash
cmake .. \
  -DWASM_RUNTIME=wasmtime \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j$(nproc)
```

#### With V8 (from source tree)

```bash
# Point at V8's bundled Clang compiler and the V8 source tree.
V8_ROOT=~/v8

cmake .. \
  -DWASM_RUNTIME=v8 \
  -DV8_ROOT="$V8_ROOT" \
  -DCMAKE_CXX_COMPILER="$V8_ROOT/third_party/llvm-build/Release+Asserts/bin/clang++" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j$(nproc)
```

#### With WasmEdge

```bash
cmake .. \
  -DWASM_RUNTIME=wasmedge \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j$(nproc)
```

## Running

### Basic Usage

```bash
./lswasm
```

By default, lswasm listens on a Unix domain socket at `/tmp/lswasm.sock`.

### Custom TCP Port

```bash
./lswasm --port 9000
```

### Custom Unix Domain Socket Path

```bash
./lswasm --uds /var/run/lswasm.sock
```

When both `--port` and `--uds` are given, only `--uds` is used.

### Loading a WASM Filter Module

```bash
./lswasm --module samples/sample_filter.wasm
```

### Passing Environment Variables to WASM Modules

```bash
./lswasm --module samples/sample_filter.wasm --env MY_KEY=my_value --env ANOTHER=val
```

### Help

```bash
./lswasm --help
```

## Testing

By default, lswasm listens on a **Unix domain socket** at `/tmp/lswasm.sock`.
Use `--port` to switch to TCP mode for direct `curl` testing.

### Basic Health Check (TCP mode)

```bash
# Start with TCP listener:
./lswasm --port 8080

# In another terminal:
curl http://localhost:8080/

# Output:
# WASM HTTP Proxy Server
# Method: GET
# Path: /
# Version: HTTP/1.1
# Runtime: Wasmtime
```

### Via Unix Domain Socket (default mode)

```bash
# Start with the default UDS listener:
./lswasm

# In another terminal:
curl --unix-socket /tmp/lswasm.sock http://localhost/
```

### With netcat (TCP mode)

```bash
echo -e "GET / HTTP/1.1\r\n\r\n" | nc localhost 8080
```

## Build Configuration

The CMakeLists.txt provides the following options:

- `WASM_RUNTIME` (string) - WASM runtime to use: `wasmtime` (default), `v8`, `wasmedge`, or empty string for Null VM
- `V8_ROOT` (path) - Path to V8 source tree (required when `WASM_RUNTIME=v8`)
- `V8_BUILD_DIR` (path) - V8 build output directory (default: `V8_ROOT/out/wee8`)
- `CMAKE_BUILD_TYPE` - Release or Debug build

Build output will show the selected runtime:

```
=== WASM Proxy Build Configuration ===
WASM runtime: wasmtime
Runtime found: TRUE
=========================================
```

## Project Structure

```
lswasm/
├── CMakeLists.txt                      # Main build configuration
├── README.md                           # This file
├── .gitignore                          # Git ignore rules
├── .gitmodules                         # Git submodule configuration
├── src/
│   ├── main.cpp                        # HTTP server (epoll loop, CLI)
│   ├── http_filter.h                   # HTTP filter context & chain
│   ├── wasm_module_manager.h           # WASM module manager header
│   ├── wasm_module_manager.cc          # WASM module manager impl
│   ├── log.h                           # Debug logging macros
│   └── hash_shim.cc                    # Hash helper shim
├── samples/
│   ├── sample_filter.cpp               # Example WASM filter source
│   ├── sample_filter.wasm              # Pre-compiled sample filter
│   ├── sample_filter.explore.html      # Interactive filter explorer
│   ├── CMakeLists.txt                  # Sample build rules
│   └── README.md                       # Sample documentation
├── third_party/
│   └── proxy-wasm-cpp-host/           # WASM host library (submodule)
│       ├── include/proxy-wasm/        # Header files
│       ├── src/                       # Implementation files
│       └── ...
└── build/                              # Build output directory (gitignored)
    ├── lswasm                          # Compiled executable
    └── compile_commands.json           # For IDE support
```

## Development

### Reconfiguring After Runtime Installation

If you install a new WASM runtime after the initial build, reconfigure CMake:

```bash
cd build
cmake --fresh ..
cmake --build . -j$(nproc)
```

### Viewing Compiler Commands

Check `build/compile_commands.json` for detailed compiler configurations. This can be used by IDEs and tools like clangd for better editor support.

## Troubleshooting

### Runtime Not Found

```
-- Wasmtime not found - install via: cargo install wasmtime-cli or apt install libwasmtime-dev
```

**Solution**: Install the missing runtime using the commands in the Prerequisites section.

### CMake Not Found

```
cmake: command not found
```

**Solution**: Install CMake:
```bash
# Ubuntu/Debian
sudo apt-get install -y cmake

# macOS
brew install cmake
```

### Permission Denied on Port 8080

```
Failed to bind socket to port 8080
```

**Solution**: Use a port > 1024 or run with sudo:
```bash
./lswasm --port 8000
```

## TODO: Future Enhancements

- [x] WasmEdge runtime support
- [ ] Support for additional WASM runtimes (WAMR, etc.)
- [ ] TLS/HTTPS support
- [ ] Configuration file support (YAML/JSON)

## License

Copyright 2026 LiteSpeed Technologies, Inc.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.  

See http://www.gnu.org/licenses/.

## Contributing

Contributions are welcome! Please submit pull requests and issues to the project repository.

## References

- [proxy-wasm-cpp-host](https://github.com/proxy-wasm/proxy-wasm-cpp-host)
- [Wasmtime](https://docs.wasmtime.dev/)
- [V8](https://v8.dev/)
- [V8 Build Guide](https://v8.dev/docs/build)
