# lswasm

**Version 1.0.0** · [Changelog](CHANGES.md)

A C++ HTTP proxy server that can execute WebAssembly (WASM) filter modules using proxy-wasm-cpp-host, with support for **Wasmtime**, **V8**, **WasmEdge**, and **WAMR** runtimes.

## Features

- **Multi-threaded** `epoll`-based HTTP server (Linux) with configurable worker thread pool (`--workers N`)
- Thread-local WASM VM cloning via proxy-wasm-cpp-host's `getOrCreateThreadLocalPlugin()` — each worker thread gets its own VM instance
- TCP and **Unix domain socket** listeners
- WASM filter module loading and execution via proxy-wasm-cpp-host
- HTTP filter chain with short-circuit on local responses (`sendLocalResponse`)
- **Response header manipulation** from WASM modules via proxy-wasm ABI
- **Streaming response API** — WASM modules can send chunked/streaming HTTP responses via foreign functions (`lswasm_send_response_headers`, `lswasm_write_response_chunk`, `lswasm_finish_response`)
- Support for Wasmtime, V8, WasmEdge, and WAMR runtimes (selectable via `-DWASM_RUNTIME=`)
- Per-module environment variables (`--env KEY=VALUE`)
- Reader-writer locked metrics (atomic counters/gauges) and reader-writer locked module registry
- Thread-safe logging
- Graceful shutdown with signal handling (SIGINT, SIGTERM) and ordered thread pool drain
- Modular CMake-based build system

## Architecture

```
lswasm/
├── CMakeLists.txt                  # Main build configuration
├── README.md                       # This file
├── .gitmodules                     # Git submodule configuration
├── install.sh                      # Install lswasm as a systemd user service
├── upgrade.sh                      # Automated upgrade (pull, build, restart)
├── uninstall.sh                    # Remove service and installed binary
├── src/
│   ├── main.cpp                    # HTTP server (epoll loop, CLI, thread pool dispatch)
│   ├── http_filter.h               # HTTP filter context (per-request WASM scopes)
│   ├── connection_io.h             # Worker ↔ epoll bridge for streaming I/O
│   ├── http_utils.h                # HTTP utility functions (header serialization, etc.)
│   ├── wasm_module_manager.h       # WASM module manager (thread-local VM cloning)
│   ├── wasm_module_manager.cc      # WASM module manager implementation
│   ├── thread_pool.h               # Fixed-size worker thread pool
│   ├── log.h                       # Thread-safe debug logging (file-based, --debug flag)
│   └── hash_shim.cc                # Hash helper shim
├── samples/
│   ├── include/
│   │   └── lswasm_streaming.h      # SDK-side convenience header for streaming API
│   ├── sample_filter/
│   │   ├── sample_filter.cpp       # Example WASM filter (C++ source)
│   │   ├── CMakeLists.txt          # Build rules for sample_filter
│   │   └── README.md               # Sample documentation
│   ├── send_recv_all/
│   │   ├── send_recv_all.cpp       # Buffered send/receive sample (C++ source)
│   │   ├── CMakeLists.txt          # Build rules for send_recv_all
│   │   └── README.md               # Sample documentation
│   └── send_recv_stream/
│       ├── send_recv_stream.cpp    # Streaming response sample (C++ source)
│       ├── CMakeLists.txt          # Build rules for send_recv_stream
│       └── README.md               # Sample documentation
├── cmake/
│   └── wasm32-wasi-toolchain.cmake # Toolchain file for building WASM modules
├── third_party/
│   ├── proxy-wasm-cpp-host/        # WASM host library (git submodule)
│   ├── proxy-wasm-cpp-sdk/         # WASM SDK (git submodule)
│   └── proxy-wasm-spec/            # WASM spec (git submodule)
└── build*/                         # Build output directories (gitignored)
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
sudo apt-get install -y build-essential cmake git libssl-dev pkg-config cargo

# macOS
brew install cmake openssl pkg-config rust
```

### WASM Runtimes

#### Wasmtime (Default)

CMake first tries `find_package(wasmtime)` for a system-installed library.
If that fails it looks for a source build in `third_party/wasmtime-src/`.

**Option A — System install (Ubuntu/Debian):**

```bash
sudo apt-get install -y libwasmtime-dev
```

**Option B — Build from source into `third_party/`:**

```bash
git clone https://github.com/bytecodealliance/wasmtime.git third_party/wasmtime-src
cd third_party/wasmtime-src
cargo build --release -p wasmtime-c-api
cd ../..
```

This produces the C API headers under
`third_party/wasmtime-src/crates/c-api/include/` and the library at
`third_party/wasmtime-src/target/release/libwasmtime.a`, which CMake
detects automatically.

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

CMake first tries pkg-config, then searches system paths, `~/.wasmedge/`,
and `third_party/wasmedge/`.

**Option A — Quick install (Linux/macOS):**

```bash
curl -sSf https://raw.githubusercontent.com/WasmEdge/WasmEdge/master/utils/install.sh | bash
```

**Option B — System install (Ubuntu/Debian):**

```bash
sudo apt-get install -y wasmedge
```

**Option C — Build from source into `third_party/`:**

```bash
git clone https://github.com/WasmEdge/WasmEdge.git third_party/wasmedge-src
cd third_party/wasmedge-src
cmake -Bbuild -GNinja -DCMAKE_BUILD_TYPE=Release .
cmake --build build
cmake --install build --prefix ../../third_party/wasmedge
cd ../..
```

This places headers in `third_party/wasmedge/include/` and the library
in `third_party/wasmedge/lib/`, which CMake detects automatically.

#### WAMR (WebAssembly Micro Runtime)

CMake first tries pkg-config, then searches system paths and
`third_party/wamr/` or `third_party/wasm-micro-runtime/`.

> **Important:** WAMR must be built with `-DWAMR_BUILD_LIBC_WASI=0`.
> proxy-wasm-cpp-host supplies its own WASI function stubs; WAMR's
> built-in WASI implementation conflicts with them and causes
> `_initialize` to trap with `unreachable`.

**Option A — System install:**

```bash
git clone https://github.com/bytecodealliance/wasm-micro-runtime.git
cd wasm-micro-runtime/product-mini/platforms/linux
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DWAMR_BUILD_LIBC_WASI=0
cmake --build . -j$(nproc)
sudo cmake --install .
```

**Option B — Build from source into `third_party/`:**

```bash
git clone https://github.com/bytecodealliance/wasm-micro-runtime.git \
  third_party/wasm-micro-runtime
cd third_party/wasm-micro-runtime/product-mini/platforms/linux
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DWAMR_BUILD_LIBC_WASI=0
cmake --build . -j$(nproc)
cd ../../../../../..
```

This places the headers in
`third_party/wasm-micro-runtime/core/iwasm/include/` and the library at
`third_party/wasm-micro-runtime/product-mini/platforms/linux/build/`,
which CMake detects automatically.

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

#### With WAMR

```bash
cmake .. \
  -DWASM_RUNTIME=wamr \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j$(nproc)
```

## Upgrading

To upgrade an existing lswasm installation to a newer version:

### Automated Upgrade (Recommended)

If lswasm was installed via `install.sh`, the `upgrade.sh` script automates
the entire process — pull, submodule update, clean rebuild, stop service,
copy binary, and restart:

```bash
cd lswasm
./upgrade.sh
```

Pass CMake options with `--cmake-args`:

```bash
./upgrade.sh --cmake-args "-DWASM_RUNTIME=wamr -DCMAKE_BUILD_TYPE=Release"
```

| Flag | Description |
|------|-------------|
| `--build-dir <path>` | Build directory (default: `build`) |
| `--cmake-args <args>` | Additional CMake configure arguments (quoted string, e.g., `"-DWASM_RUNTIME=wamr -DCMAKE_BUILD_TYPE=Release"`) |
| `--no-clean` | Incremental build instead of clean rebuild |
| `--no-pull` | Skip `git pull` (use local source as-is) |
| `--service-name <name>` | Override the systemd unit name |

### Re-installing with install.sh

You can also re-run `install.sh` with the same arguments as the original
install.  If an existing binary is detected at the install location, the
script automatically stops the running service before copying the new binary
and restarts it afterward.

### Manual Upgrade

#### 1. Check the Changelog

Review [CHANGES.md](CHANGES.md) for breaking changes, new features, and
migration notes before upgrading.

#### 2. Pull Latest Changes

```bash
cd lswasm
git pull
```

#### 3. Update Submodules

The `proxy-wasm-cpp-host` submodule may have been updated. Always sync and
update submodules after pulling:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

#### 4. Rebuild

A clean rebuild is recommended after upgrading, especially when submodules
or build configuration have changed:

```bash
# Remove the old build directory and start fresh
rm -rf build
mkdir build
cd build

# Configure (use the same options as your original build)
cmake .. \
  -DWASM_RUNTIME=wasmtime \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j$(nproc)
```

If you prefer an in-place reconfigure instead of a full clean build:

```bash
cd build
cmake --fresh ..
cmake --build . -j$(nproc)
```

> **Tip:** If the WASM runtime version on your system has changed (e.g. a
> Wasmtime or V8 upgrade), you should always do a clean rebuild to avoid
> link errors from stale cached artefacts.

## Running

### Basic Usage

```bash
./lswasm --module samples/sample_filter/sample_filter.wasm
```

The `--module` flag is **required** — lswasm will exit with an error if no
WASM filter module is specified.

By default, lswasm listens on a Unix domain socket at `/tmp/lswasm.sock`
using `std::thread::hardware_concurrency()` worker threads (or 4 if
detection fails).

### Custom Worker Count

```bash
# Use 8 worker threads
./lswasm --module filter.wasm --workers 8
```

### Custom TCP Port

```bash
./lswasm --module filter.wasm --port 9000
```

### Custom Unix Domain Socket Path

```bash
./lswasm --module filter.wasm --uds /var/run/lswasm.sock
```

When both `--port` and `--uds` are given, only `--uds` is used.

### Passing Environment Variables to WASM Modules

```bash
./lswasm --module samples/sample_filter/sample_filter.wasm --env MY_KEY=my_value --env ANOTHER=val
```

### Help

```bash
./lswasm --help
```

### Command-Line Reference

| Option | Argument | Description |
|--------|----------|-------------|
| `--port` | `PORT` | Listen on a TCP port instead of a Unix domain socket |
| `--uds` | `PATH` | Listen on a Unix domain socket (default: `/tmp/lswasm.sock`) |
| `--sock-perm` | `MODE` | Set UDS file permissions in octal (default: `0666`) |
| `--module` | `PATH` | **(required)** Load a WASM filter module |
| `--env` | `KEY=VALUE` | Set an environment variable for WASM modules (repeatable) |
| `--workers` | `N` | Number of worker threads (default: `hardware_concurrency()` or 4) |
| `--body-pacifier` | — | Include a diagnostic body in HTTP responses (request info, runtime, filters) |
| `--debug` | — | Enable debug logging to `/tmp/lswasm.log` |
| `--version` | — | Print version number and exit |
| `--help` | — | Show usage information and exit |

When both `--port` and `--uds` are given, only `--uds` is used.
By default (no `--port`), lswasm listens on the UDS path.

## Installing as a Service

lswasm ships with `install.sh` and `uninstall.sh` scripts that set up (or
tear down) a **systemd user service** so lswasm starts automatically when
you log in.

### Install

```bash
./install.sh \
  --bin ./build/lswasm \
  --install-dir /opt/lswasm \
  -- --module /etc/lswasm/filter.wasm --uds /run/lswasm.sock --debug
```

| Flag | Required | Description |
|------|----------|-------------|
| `--bin <path>` | Yes | Path to the compiled lswasm binary |
| `--install-dir <path>` | Yes | Directory where the binary will be copied |
| `--service-name <name>` | No | systemd unit name (default: `lswasm.service`) |
| `-- <args …>` | No | All arguments after `--` are forwarded to lswasm's `ExecStart` |

If `--module` or `--uds` are not present in the forwarded arguments, the
script prompts interactively.

After a successful install:

```bash
systemctl --user status lswasm.service
journalctl --user -u lswasm.service -f
```

### Uninstall

```bash
./uninstall.sh
```

Or with a custom service name:

```bash
./uninstall.sh --service-name my-lswasm.service
```

The uninstall script:
1. Stops and disables the systemd user service.
2. Removes the unit file and reloads systemd.
3. Deletes the installed binary.
4. Removes the install directory if it is empty.

## Starting and Stopping the Service

### systemd User Service

If you installed lswasm via [`install.sh`](install.sh), it runs as a **systemd user service** that starts automatically on login.

**Start** the service:

```bash
systemctl --user start lswasm.service
```

**Stop** the service:

```bash
systemctl --user stop lswasm.service
```

**Restart** the service (e.g. after updating a WASM module or changing configuration):

```bash
systemctl --user restart lswasm.service
```

**Check status:**

```bash
systemctl --user status lswasm.service
```

**View logs:**

```bash
journalctl --user -u lswasm.service -f
```

**Disable** the service from starting on login:

```bash
systemctl --user disable lswasm.service
```

**Re-enable** it:

```bash
systemctl --user enable lswasm.service
```

> **Note:** If you used a custom `--service-name` during installation, replace
> `lswasm.service` with that name in the commands above.

### Standalone (without systemd)

When running lswasm directly from the command line, stop it with
**Ctrl+C** (sends `SIGINT`) or by sending `SIGTERM`:

```bash
# Start in the foreground:
./lswasm --module samples/sample_filter/sample_filter.wasm --port 8080

# Stop with Ctrl+C, or from another terminal:
kill $(pidof lswasm)
```

lswasm handles both `SIGINT` and `SIGTERM` for graceful shutdown — it stops
accepting new connections, drains the worker thread pool (waiting for
in-flight requests to complete), cleans up the Unix domain socket (if used),
destroys WASM module state, and exits.

## Testing

By default, lswasm listens on a **Unix domain socket** at `/tmp/lswasm.sock`.
Use `--port` to switch to TCP mode for direct `curl` testing.

### Basic Health Check

```bash
# Start with the default UDS listener:
./lswasm --module samples/sample_filter/sample_filter.wasm

# In another terminal:
curl --unix-socket /tmp/lswasm.sock http://localhost/
```

### Basic Health Check (TCP mode)

```bash
# Start with TCP listener:
./lswasm --module samples/sample_filter/sample_filter.wasm --port 8080

# In another terminal:
curl http://localhost:8080/

# Output:
# WASM HTTP Proxy Server
# Method: GET
# Path: /
# Version: HTTP/1.1
# Runtime: Wasmtime
```

### With netcat (TCP mode)

```bash
echo -e "GET / HTTP/1.1\r\n\r\n" | nc localhost 8080
```

## Build Configuration

The CMakeLists.txt provides the following options:

- `WASM_RUNTIME` (string) - WASM runtime to use: `wasmtime` (default), `v8`, `wasmedge`, `wamr`, or empty string for Null VM
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

## Configuring LiteSpeed

To configure LiteSpeed (Enterprise or OpenLiteSpeed) there are many ways to do it.  Note that lswasm runs as a separate HTTP server and LiteSpeed will operate as a proxy to it.  The instructions below assume:

- An overall configuration.  This will work with OpenLiteSpeed and in LiteSpeed Enterprise in non-Apache mode.  In Apache mode, you will want to setup rewrite files to it.
- You are just testing it out and thus will use the sample application (from above).
- You will copy the sample application to the root of your server's Virtual Host directory ($LSWS_HOME/DEFAULT/html/ for LiteSpeed Enterprise's default Virtual Host or $LSWS_HOME/Example/html/ for OpenLiteSpeed's default Virtual Host).

Many users will configure it for processing a particular directory on an existing listener, in which case you'd setup a Virtual Host context for it.  Or a particular port, in which case you'd configure a listener and a VirtualHost context.

Navigate to: **Web Admin > Configuration > External App > Add**

- Type = `Web Server`.  Press the **Next** button
- Name = `wasm`.  A sample name that you can remember.
- Address = `uds://tmp/lswasm.sock`.  The default UDS address.  If during the install you configured it to be installed elsewhere or with TCP you should specify the alternate location or http://<address>:<port>.
- Max Connections = 20.  Specifies the maximum number of concurrent connections that can be established between the server and an external application.
- Connection Keepalive Timeout = `60`.  Specifies the maximum time in seconds to keep an idle persistent connection open.
- Initial Request Timeout (secs) = `60`.  Specifies the maximum time in seconds the server will wait for the external application to respond to the first request over a newly established connection. 
- Retry Timeout (secs) = `60`.  Specifies the period of time that the server waits before retrying an external application that had a prior communication problem.

Press the **Save** button to save your settings.  Press the **Script Handler** tab to setup an extension handler.

- Suffixes = `wasm`.  This will trigger on any file with a .wasm extension.
- Handler Type = `Web Server`.  This is the type of handler that lswasm runs as.
- Handler Name = `wasm`.  Enter the name from the External App above.

Press the **Save** button to save your settings.  Perform a **Graceful Restart** to apply the settings.

In the default case, in a browser with the default settings: `http://localhost:8088/sample_filter.wasm` will display the output of the settings.

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

## Streaming Response API

lswasm extends the proxy-wasm ABI with three **foreign functions** that let a
WASM filter stream HTTP responses incrementally instead of buffering the
entire body in a single `sendLocalResponse()` call.  This is useful for large
payloads, server-sent events, or any scenario where constant memory usage is
important.

### Foreign Functions

| Function | Argument | Description |
|----------|----------|-------------|
| `lswasm_send_response_headers` | 4-byte status code + marshalled header pairs | Begin a streaming response with the given HTTP status and headers |
| `lswasm_write_response_chunk` | Raw body bytes | Write a chunk of response body data to the client |
| `lswasm_finish_response` | *(none)* | Signal end-of-response — no more chunks may be written |

These are invoked via `proxy_call_foreign_function()` from the proxy-wasm
SDK.

### C++ Convenience Header

Include `lswasm_streaming.h` (located at `samples/include/`) in your filter
to get typed wrappers instead of calling `proxy_call_foreign_function()`
directly:

```cpp
#include "lswasm_streaming.h"

// In your stream context's onRequestHeaders or onRequestBody:

// 1. Send response headers (starts the streaming response).
lswasm::streaming::sendResponseHeaders(200, {
    {"content-type", "text/plain"},
    {"x-custom",     "value"},
});

// 2. Write body chunks as they become available.
lswasm::streaming::writeResponseChunk(data, len);

// 3. Finish the response.
lswasm::streaming::finishResponse();
```

### Lifecycle Rules

1. **`sendResponseHeaders`** must be called exactly once, before any chunks.
2. **`writeResponseChunk`** may be called zero or more times.
3. **`finishResponse`** must be called exactly once to close the response.
4. Calling these out of order returns `WasmResult::BadArgument`.

### Detecting Host Support

If your filter must run on hosts that may not support the streaming API, call
`lswasm::streaming::isSupported()` *before* `sendResponseHeaders()`.  It
returns `false` on hosts that don't register the foreign functions (the call
returns `WasmResult::NotFound`), letting you fall back to
`sendLocalResponse()`.

### Samples

- **`samples/send_recv_stream/`** — Streaming echo filter that writes each
  request body chunk back to the client as it arrives, maintaining constant
  memory usage regardless of body size.
- **`samples/send_recv_all/`** — Buffered send/receive filter that
  accumulates the entire request body and responds with a single
  `sendLocalResponse()`.  Good for small payloads.

See each sample's `README.md` for build and usage instructions.

## Troubleshooting

### Enabling lswasm logging

lswasm logging can be enabled by either entering the `--debug` command line option to the executable or creating the trigger file `/tmp/lswasm.dolog`.  The log is written to /tmp/lswasm.log.  Any problems you have in lswasm should start with enabling logging and examining the log.

### Verifying lswasm is working

The instructions above include a [Basic Health Check](#basic-health-check).  Before relying on LiteSpeed's configuration you should verify the service is performing correctly with the basic health check.  If it is not, then enable lswasm logging and check the logs for errors.

### Service output

If you are running lswasm as a service, the important messages will be written to the system log files.  In most modern Linux systems, these can be examined with journalctl searching for lswasm.

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
- [WasmEdge](https://wasmedge.org/)
- [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime)
