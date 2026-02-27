# WASM HTTP Proxy Server

A C++ HTTP proxy server that can execute WebAssembly (WASM) filter modules using proxy-wasm-cpp-host, with support for both **Wasmer** and **Wasmtime** runtimes.

## Features

- Single-threaded, `epoll`-based HTTP server (Linux)
- TCP and **Unix domain socket** listeners
- WASM filter module loading and execution via proxy-wasm-cpp-host
- HTTP filter chain with short-circuit on local responses (`sendLocalResponse`)
- Support for both Wasmer and Wasmtime runtimes
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
│   └── hash_shim.cc            # Hash helper shim
├── samples/
│   ├── sample_filter.c         # Example WASM filter (C source)
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

#### Wasmtime (Recommended)

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

#### Wasmer

```bash
# Install Wasmer
curl https://get.wasmer.io -sSfL | sh

# And development files
# Ubuntu/Debian
sudo apt-get install -y libwasmer-dev

# Or from source
git clone https://github.com/wasmerio/wasmer.git
cd wasmer
make install
```

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

#### With Both Runtimes (Default)

```bash
cmake .. \
  -DENABLE_WASMER=ON \
  -DENABLE_WASMTIME=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j$(nproc)
```

#### Wasmtime Only

```bash
cmake .. \
  -DENABLE_WASMER=OFF \
  -DENABLE_WASMTIME=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j$(nproc)
```

#### Wasmer Only

```bash
cmake .. \
  -DENABLE_WASMER=ON \
  -DENABLE_WASMTIME=OFF \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j$(nproc)
```

#### Minimal Build (Null VM - No WASM Runtime)

```bash
cmake .. \
  -DENABLE_WASMER=OFF \
  -DENABLE_WASMTIME=OFF \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j$(nproc)
```

## Running

### Basic Usage

```bash
./lswasm
```

Server will listen on `http://localhost:8080`

### Custom Port

```bash
./lswasm --port 9000
```

### Unix Domain Socket

```bash
./lswasm --uds /tmp/lswasm.sock
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

### Basic Health Check

```bash
curl http://localhost:8080/

# Output:
# WASM HTTP Proxy Server
# Method: GET
# Path: /
# Version: HTTP/1.1
# Runtime: Wasmtime
```

### With netcat

```bash
echo -e "GET / HTTP/1.1\r\n\r\n" | nc localhost 8080
```

## Build Configuration

The CMakeLists.txt provides the following options:

- `ENABLE_WASMER` (ON/OFF) - Enable Wasmer runtime support
- `ENABLE_WASMTIME` (ON/OFF) - Enable Wasmtime runtime support
- `CMAKE_BUILD_TYPE` - Release or Debug build

Build output will show which runtimes are enabled:

```
=== WASM Proxy Build Configuration ===
Wasmer support: ON
Wasmtime support: ON
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
│   └── hash_shim.cc                    # Hash helper shim
├── samples/
│   ├── sample_filter.c                 # Example WASM filter source
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

- [x] Load and execute actual WASM modules from files
- [x] WASM module lifecycle management (load, unload, reload)
- [ ] HTTP path routing to different WASM modules
- [ ] Response header manipulation from WASM modules
- [ ] Support for additional WASM runtimes (WasmEdge, WAMR)
- [ ] TLS/HTTPS support
- [ ] Configuration file support (YAML/JSON)
- [ ] Metrics and logging

## License

[Add your license here]

## Contributing

Contributions are welcome! Please submit pull requests and issues to the project repository.

## References

- [proxy-wasm-cpp-host](https://github.com/proxy-wasm/proxy-wasm-cpp-host)
- [Wasmtime](https://docs.wasmtime.dev/)
- [Wasmer](https://docs.wasmer.io/)
