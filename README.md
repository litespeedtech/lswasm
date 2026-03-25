# lswasm

**Version 1.0.0** · [Changelog](CHANGES.md)

A C++ agent that executes WebAssembly (WASM) filter modules via
[proxy-wasm-cpp-host](https://github.com/proxy-wasm/proxy-wasm-cpp-host),
with support for **Wasmtime**, **V8**, **WasmEdge**, and **WAMR** runtimes.  It is designed with LiteSpeed Server products in mind (LiteSpeed Enterprise and OpenLiteSpeed).

---

## Table of contents

- [Features](#features)
- [Architecture](#architecture)
- [Prerequisites](#prerequisites)
  - [System dependencies](#system-dependencies)
  - [WASM runtimes](#wasm-runtimes)
- [Building](#building)
- [Running](#running)
  - [Command-line reference](#command-line-reference)
  - [LSAPI mode (default)](#lsapi-transport-mode)
  - [Standalone LSPROXY mode](#standalone-lsproxy-mode)
- [Installing / upgrading / uninstalling](#installing-the-binary)
- [Configuring LiteSpeed](#configuring-litespeed)
- [Streaming response API](#streaming-response-api)
- [Performance](#performance)
  - [WAMR AOT precompilation](#wamr-aot-precompilation)
  - [WasmEdge AOT precompilation](#wasmedge-aot-precompilation)
- [Testing](#testing)
- [Development](#development)
- [Troubleshooting](#troubleshooting)
- [License](#license)
- [Contributing](#contributing)
- [References](#references)

---

## Features

| Category | Description |
|----------|-------------|
| **LSAPI transport** | Default mode for LiteSpeed / OpenLiteSpeed integration.  It can be started by LiteSpeed or stand-alone. |
| **Standalone LSPROXY** | UDS/TCP listener activated with `--lsproxy` |
| **Thread pool** | Configurable worker threads (`--workers N`) |
| **Multiple runtimes** | Wasmtime, V8, WasmEdge, WAMR — selectable at build time (`-DWASM_RUNTIME=`) |
| **Per-module env vars** | `--env KEY=VALUE` (repeatable) |
| **Concurrency** | Reader-writer locked metrics and module registry; thread-local WASM VM cloning via `getOrCreateThreadLocalPlugin()` |
| **Header manipulation** | WASM modules can modify response headers via the proxy-wasm ABI |
| **Streaming responses** | Foreign functions (`lswasm_send_response_headers`, `lswasm_write_response_chunk`, `lswasm_finish_response`) over both HTTP and LSAPI transports |
| **CMake build** | Modular CMake-based build system with per-runtime detection |

---

## Architecture

```
lswasm/
├── CMakeLists.txt                  # Main build configuration
├── README.md                       # This file
├── CHANGES.md                      # Changelog
├── .gitmodules                     # Git submodule configuration
├── install.sh                      # Install the lswasm binary and save metadata
├── upgrade.sh                      # Automated upgrade (pull, build, replace binary)
├── uninstall.sh                    # Remove installed binary and saved metadata
│
├── src/
│   ├── main.cpp                    # HTTP server (epoll loop, CLI, thread pool dispatch)
│   ├── http_filter.h               # HTTP filter context (per-request WASM scopes)
│   ├── connection_io.h             # Worker ↔ epoll bridge for streaming I/O
│   ├── http_utils.h                # HTTP utility functions (header serialization, etc.)
│   ├── response_sink.h             # Transport-abstract response interface
│   ├── http_response_sink.h        # ResponseSink for epoll/HTTP (chunked transfer)
│   ├── lsapi_response_sink.h       # ResponseSink for LSAPI transport
│   ├── wasm_module_manager.h       # WASM module manager (thread-local VM cloning)
│   ├── wasm_module_manager.cc      # WASM module manager implementation
│   ├── thread_pool.h               # Fixed-size worker thread pool
│   ├── log.h                       # Thread-safe debug logging (file-based, --debug flag)
│   ├── hash_shim.cc                # Hash helper shim
│   ├── lsapidef.h                  # LSAPI protocol definitions (C)
│   ├── lsapilib.h                  # LSAPI library header (C)
│   └── lsapilib.c                  # LSAPI library implementation (C)
│
├── samples/
│   ├── include/
│   │   └── lswasm_streaming.h      # SDK-side convenience header for streaming API
│   ├── lsapi_raw/                  # Native LSAPI prefork benchmark baseline
│   ├── sample_filter/              # Basic WASM filter example
│   ├── send_recv_all/              # Buffered send/receive sample
│   ├── send_recv_stream/           # Streaming echo sample
│   └── send_stream_size/           # Streaming size-based response generator
│
├── cmake/
│   └── wasm32-wasi-toolchain.cmake # Toolchain file for building WASM modules
│
├── third_party/
│   ├── proxy-wasm-cpp-host/        # WASM host library (git submodule)
│   ├── proxy-wasm-cpp-sdk/         # WASM SDK (git submodule)
│   └── proxy-wasm-spec/            # WASM spec (git submodule)
│
└── build*/                         # Build output directories (gitignored)
```

---

## Prerequisites

### System dependencies

#### Ubuntu / Debian

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git libssl-dev pkg-config cargo
```

#### Red Hat / AlmaLinux / Rocky Linux

```bash
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y cmake git openssl-devel pkg-config cargo
```

#### macOS

```bash
brew install cmake openssl pkg-config rust
```

### WASM runtimes

Choose **one** runtime to build against.  The runtime is selected at CMake
configure time with `-DWASM_RUNTIME=<name>`.

#### Wasmtime (default)

CMake first tries `find_package(wasmtime)` for a system-installed library.
If that fails it looks for a source build in `third_party/wasmtime-src/`.

<details>
<summary><strong>Option A — System install (Ubuntu/Debian)</strong></summary>

```bash
sudo apt-get install -y libwasmtime-dev
```
</details>

<details>
<summary><strong>Option B — Build from source into <code>third_party/</code></strong></summary>

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
</details>

#### V8

> **Important:** The proxy-wasm-cpp-host V8 backend requires the **full V8
> source tree** with a completed build. The Node.js `libnode-dev` package is
> *not* sufficient because proxy-wasm-cpp-host uses internal V8 headers
> (e.g. `src/wasm/c-api.h`) that are not shipped with Node.js.

<details>
<summary><strong>Building V8 from source</strong></summary>

Follow the [V8 source code guide](https://v8.dev/docs/source-code) to install
`depot_tools` and fetch the source tree. Then in the `v8` directory:

```bash
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

> Adjust `target_cpu` for your platform (e.g. `"arm64"`).

This produces `out/wee8/obj/libwee8.a` (~120 MB monolithic archive containing
V8, ICU, zlib, and all dependencies).

See the [official V8 build guide](https://v8.dev/docs/build) for
platform-specific prerequisites and troubleshooting.
</details>

<details>
<summary><strong>V8 toolchain requirements</strong></summary>

V8 builds with its own bundled **Clang** and **libc++** (custom ABI namespace
`__Cr`). To link against `libwee8.a`, lswasm **must** use the same toolchain:

| Component | Path |
|-----------|------|
| Compiler | `<V8_ROOT>/third_party/llvm-build/Release+Asserts/bin/clang++` |
| Linker | V8's bundled `lld` (auto-detected by CMake) |
| C++ stdlib | V8's bundled libc++ headers and static archives (auto-detected) |
| C++ standard | C++20 (required by V8 ≥ 14.x; set automatically) |

CMake handles all of this when `V8_ROOT` is set — just point
`CMAKE_CXX_COMPILER` at V8's Clang.
</details>

<details>
<summary><strong>V8 link dependencies</strong></summary>

Modern V8 (≥ 14.7) uses Rust for the ECMAScript Temporal API. CMake
automatically extracts all required `.rlib` and `.a` dependencies from V8's
`wee8.ninja` build file, including:

- ICU (i18n, unicode), zlib, partition\_alloc
- ~55 Rust `.rlib` archives (temporal\_rs, icu\_calendar, diplomat\_runtime, etc.)
- Rust standard library sysroot archives
- `libclang_rt.builtins.a`
</details>

#### WasmEdge

WasmEdge is a lightweight, high-performance WebAssembly runtime optimized for
cloud-native, edge, and decentralized applications.

CMake first tries pkg-config, then searches system paths, `~/.wasmedge/`, and
`third_party/wasmedge/`.

<details>
<summary><strong>Option A — Quick install (Linux/macOS)</strong></summary>

```bash
curl -sSf https://raw.githubusercontent.com/WasmEdge/WasmEdge/master/utils/install.sh | bash
```
</details>

<details>
<summary><strong>Option B — System install (Ubuntu/Debian)</strong></summary>

```bash
sudo apt-get install -y wasmedge
```
</details>

<details>
<summary><strong>Option C — Build from source into <code>third_party/</code></strong></summary>

```bash
git clone https://github.com/WasmEdge/WasmEdge.git third_party/wasmedge-src
cd third_party/wasmedge-src
cmake -Bbuild -GNinja -DCMAKE_BUILD_TYPE=Release .
cmake --build build
cmake --install build --prefix ../../third_party/wasmedge
cd ../..
```

Headers go to `third_party/wasmedge/include/` and the library to
`third_party/wasmedge/lib/`; CMake detects both automatically.
</details>

#### WAMR (WebAssembly Micro Runtime)

CMake first tries pkg-config, then searches system paths and
`third_party/wamr/` or `third_party/wasm-micro-runtime/`.

> **Important:** WAMR must be built with `-DWAMR_BUILD_LIBC_WASI=0`.
> proxy-wasm-cpp-host supplies its own WASI function stubs; WAMR's built-in
> WASI implementation conflicts with them and causes `_initialize` to trap
> with `unreachable`.

<details>
<summary><strong>Option A — System install</strong></summary>

```bash
git clone https://github.com/bytecodealliance/wasm-micro-runtime.git
cd wasm-micro-runtime/product-mini/platforms/linux
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DWAMR_BUILD_LIBC_WASI=0
cmake --build . -j$(nproc)
sudo cmake --install .
```
</details>

<details>
<summary><strong>Option B — Build from source into <code>third_party/</code></strong></summary>

```bash
git clone https://github.com/bytecodealliance/wasm-micro-runtime.git \
  third_party/wasm-micro-runtime
cd third_party/wasm-micro-runtime/product-mini/platforms/linux
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DWAMR_BUILD_LIBC_WASI=0
cmake --build . -j$(nproc)
cd ../../../../../..
```

Headers land in `third_party/wasm-micro-runtime/core/iwasm/include/` and the
library at `third_party/wasm-micro-runtime/product-mini/platforms/linux/build/`;
CMake detects both automatically.
</details>

---

## Building

### 1. Clone with submodules

```bash
git clone https://github.com/litespeedtech/lswasm.git
cd lswasm
git submodule update --init --recursive
```

### 2. Create a build directory

```bash
mkdir build && cd build
```

### 3. Configure and build

Pick the runtime that matches the one you installed above.

#### Wasmtime (default)

```bash
cmake .. -DWASM_RUNTIME=wasmtime -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

#### V8

```bash
V8_ROOT=~/v8

cmake .. \
  -DWASM_RUNTIME=v8 \
  -DV8_ROOT="$V8_ROOT" \
  -DCMAKE_CXX_COMPILER="$V8_ROOT/third_party/llvm-build/Release+Asserts/bin/clang++" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j$(nproc)
```

#### WasmEdge

```bash
cmake .. -DWASM_RUNTIME=wasmedge -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

#### WAMR

```bash
cmake .. -DWASM_RUNTIME=wamr -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### Build configuration variables

| Variable | Type | Description |
|----------|------|-------------|
| `WASM_RUNTIME` | string | `wasmtime` (default), `v8`, `wasmedge`, `wamr`, or `""` (Null VM) |
| `V8_ROOT` | path | Path to V8 source tree (required when `WASM_RUNTIME=v8`) |
| `V8_BUILD_DIR` | path | V8 build output directory (default: `V8_ROOT/out/wee8`) |
| `CMAKE_BUILD_TYPE` | string | `Release` or `Debug` |

A successful configure prints:

```
=== WASM Proxy Build Configuration ===
WASM runtime: wasmtime
Runtime found: TRUE
=========================================
```

---

## Running

### Command-line reference

| Option | Argument | Description |
|--------|----------|-------------|
| `--module` | `<path>` | **(required)** Path to the WASM filter module |
| `--lsapi-addr` | `<addr>` | Bind LSAPI to a specific address (e.g. `127.0.0.1:8000` or `/tmp/lswasm.sock`); LSAPI only |
| `--lsproxy` | — | Switch from default LSAPI mode to standalone LSPROXY mode |
| `--port` | `<port>` | TCP port for standalone LSPROXY mode (instead of UDS) |
| `--uds` | `<path>` | Unix domain socket path for LSPROXY mode (default: `/tmp/lswasm.sock`) |
| `--sock-perm` | `<mode>` | UDS file permissions in octal (default: `0666`); LSPROXY only |
| `--env` | `<key>=<value>` | Environment variable for WASM modules (repeatable) |
| `--workers` | `<n>` | Worker thread count (default: `hardware_concurrency()` or 4) |
| `--body-pacifier` | — | Include a diagnostic body in generated responses |
| `--debug` | — | Enable debug logging to `/tmp/lswasm.log` |
| `--version` | — | Print version and exit |
| `--help` | — | Show usage and exit |

> When both `--port` and `--uds` are given, only `--uds` is used.
> `--port`, `--uds`, and `--sock-perm` all require `--lsproxy`.
> `--lsapi-addr` requires LSAPI mode (cannot be combined with `--lsproxy`).

> lswasm needs to be run as a non-root user.

### Basic usage

```bash
# Minimal invocation (LSAPI mode):
./lswasm --module samples/sample_filter/sample_filter.wasm

# LSAPI mode with explicit listening address:
./lswasm --module filter.wasm --lsapi-addr 127.0.0.1:8000

# Standalone LSPROXY with default UDS:
./lswasm --module filter.wasm --lsproxy

# Standalone LSPROXY on TCP port 9000 with 8 workers:
./lswasm --module filter.wasm --lsproxy --port 9000 --workers 8

# Custom UDS path:
./lswasm --module filter.wasm --lsproxy --uds /var/run/lswasm.sock

# Pass env vars to the WASM module:
./lswasm --module filter.wasm --env MY_KEY=my_value --env ANOTHER=val
```

### LSAPI transport mode

LSAPI mode is the **default** and is the recommended communications mode when using a LiteSpeed server.

By default, the LSAPI listening socket is inherited from the parent process
(e.g. LiteSpeed) and that is the recommended method.  Use `--lsapi-addr` to create a listening socket on an
explicit address instead.  The address can be a TCP `host:port` pair or a
Unix domain socket path:

```bash
# TCP listener
./lswasm --module filter.wasm --lsapi-addr 127.0.0.1:8000

# UDS listener
./lswasm --module filter.wasm --lsapi-addr /tmp/lswasm_lsapi.sock
```

### Standalone LSPROXY mode

Use `--lsproxy` to switch to the standalone UDS/TCP listener.  In this mode
lswasm exposes its own socket endpoint and can be used as a web-server proxy
target.

- `--port`, `--uds`, and `--sock-perm` apply only in this mode.
- When both `--port` and `--uds` are given, only `--uds` is used.

---

## Installing the binary

lswasm ships with three lifecycle scripts:

| Script | Purpose |
|--------|---------|
| [`install.sh`](install.sh) | Copy the binary and save metadata for upgrades |
| [`upgrade.sh`](upgrade.sh) | Pull, rebuild, and replace the installed binary |
| [`uninstall.sh`](uninstall.sh) | Remove the installed binary and metadata |

> These scripts manage only the binary — they do not create or manage a system
> service or the required module.

### Install

```bash
./install.sh --bin ./build/lswasm --install-dir /usr/local/lsws/fcgi-bin
```

| Flag | Required | Description |
|------|----------|-------------|
| `--bin` `<path>` | Yes | Path to the compiled `lswasm` binary |
| `--install-dir` `<path>` | Yes | Destination directory for the binary |

After installing, point LiteSpeed/OpenLiteSpeed at the installed binary.  In the
common LSAPI deployment model the web server launches lswasm on demand:

```bash
/usr/local/lsws/fcgi-bin/lswasm --module /usr/local/lsws/fcgi-bin/sample_filter.wasm
```

For standalone mode, run with `--lsproxy` instead.

### Upgrade

```bash
./upgrade.sh
```

The upgrade script reads the install state from
`~/.local/state/lswasm/install-state.env` (written by `install.sh`), pulls
the latest source, rebuilds, and replaces the installed binary.

| Flag | Description |
|------|-------------|
| `--build-dir` `<path>` | Build directory (default: `build`) |
| `--cmake-args` `<args>` | Additional CMake configure arguments |
| `--no-clean` | Incremental build instead of clean rebuild |
| `--no-pull` | Skip `git pull` (use local source as-is) |

### Uninstall

```bash
./uninstall.sh
```

The uninstall script:
1. Deletes the installed binary.
2. Removes the install directory if it is empty.
3. Removes the saved install metadata.

---

## Configuring LiteSpeed

lswasm supports two integration models:

| Model | How it works |
|-------|-------------|
| **LSAPI** (default) | lswasm runs as an LSAPI application process and speaks the LSAPI protocol directly. |
| **LSPROXY** (`--lsproxy`) | lswasm runs as a separate server; LiteSpeed proxies requests to it over UDS/TCP. |

The instructions below describe the **LSAPI** setup.  For
LSPROXY mode, configure lswasm as a Web Server instead of a
LSAPI target (and add `--lsproxy`).  You will also need to create a service or other external method to pre-load it.

### Assumptions

- OpenLiteSpeed or LiteSpeed Enterprise in non-Apache mode.
- Using the sample filter for testing.
- Both `lswasm` and the sample filter `sample_filter.wasm` have been copied to the `$SERVER_ROOT/fcgi-bin/` directory, typically `/usr/local/lsws/fcgi-bin`.

> Many users will configure the filter for a particular directory (Virtual Host context), often the user's or application's home directory.

### Steps

Navigate to **Web Admin > Configuration > External App > Add**:

- **Type** = `LSAPI App` → press **Next**.
- **Name** = `wasm` (or any memorable name).
- **Address** = `uds://tmp/lswasm.sock` (adjust if you used a different path
   or TCP).  This must be unique for each external app.
- **Max Connections** = `1`.
- **Environment** = `LSAPI_CHILDREN=20`.
- **Initial Request Timeout** = `60`.
- **Retry Timeout** = `0`.
- **Connection Keepalive Timeout** = `60`.
- **Start By Server** = `Yes (Through CGI Daemon)`
- **Command** = `$SERVER_ROOT/fcgi-bin/lswasm --module $SERVER_ROOT/fcgi-bin/sample_filter.wasm` 
- **Instances** = `1`.
- **Run On Startup** = `Yes (Detached Mode)`.

Press **Save**, then navigate to **Configuration > Script Handler**:

1. **Suffixes** = `wasm`.
2. **Handler Type** = `LiteSpeed SAPI`.
3. **Handler Name** = `wasm` (the name from the External App above).

Press **Save**, then perform a **Graceful Restart** to apply.

To run a test you will need to create a `.wasm` file in the default vhost directory.  A simple way to do this would be to use the `touch` command:

- **LiteSpeed Enterprise:** Create a test file, then verify with curl:
    ```bash
    touch /usr/local/lsws/DEFAULT/html/test.wasm
    curl http://127.0.0.1:8088/test.wasm
    ```
- **OpenLiteSpeed:** Create a test file, then verify with curl:
    ```bash
    touch /usr/local/lsws/Example/html/test.wasm
    curl http://127.0.0.1:8088/html/test.wasm
    ```


---

## Streaming response API

lswasm extends the proxy-wasm ABI with three **foreign functions** that let a
WASM filter stream HTTP responses incrementally instead of buffering the entire
body in a single `sendLocalResponse()` call.  This is useful for large payloads,
server-sent events, or any scenario requiring constant memory usage.

### Foreign functions

| Function | Argument | Description |
|----------|----------|-------------|
| `lswasm_send_response_headers` | 4-byte `uint32_t` status + marshalled header pairs | Begin a streaming response |
| `lswasm_write_response_chunk` | Raw body bytes | Write a body chunk |
| `lswasm_finish_response` | *(none)* | Signal end-of-response |

These are invoked via `proxy_call_foreign_function()` from the proxy-wasm SDK.

On the HTTP transport, streaming responses use **HTTP/1.1 chunked transfer
encoding**.  The server normalizes headers automatically:

- `Content-Length` is removed.
- `Transfer-Encoding: chunked` is added if not already present.
- Conflicting `Transfer-Encoding` values are rejected.

### C++ convenience header

Include [`lswasm_streaming.h`](samples/include/lswasm_streaming.h) in your
filter for typed wrappers:

```cpp
#include "lswasm_streaming.h"

// 1. Send response headers (starts the streaming response).
lswasm::streaming::sendResponseHeaders(200, {
    {"Content-Type",  "text/plain"},
    {"X-Wasm-Filter", "example/active"},
});

// 2. Write body chunks as they become available.
lswasm::streaming::writeResponseChunk(data, len);

// 3. Finish the response.
lswasm::streaming::finishResponse();
```

### Lifecycle rules

1. `sendResponseHeaders` — call **exactly once**, before any chunks.
2. `writeResponseChunk` — call **zero or more** times after headers.
3. `finishResponse` — call **exactly once** to complete the response.
4. Calling out of order returns `WasmResult::BadArgument`.
5. Once a streaming response starts it becomes the terminal response path;
   the filter must finish it rather than switching to `sendLocalResponse()`.

### Detecting host support

If your filter may run on hosts without the streaming API, call
`lswasm::streaming::isSupported()` *before* `sendResponseHeaders()`.  The
host returns `WasmResult::BadArgument` for a zero-argument probe call;
unsupported hosts return `WasmResult::NotFound`, letting you fall back to
`sendLocalResponse()`.

### Samples

| Sample | Description |
|--------|-------------|
| [`samples/lsapi_raw/`](samples/lsapi_raw/) | Native LSAPI prefork baseline — mirrors the size-generator and streaming-echo workloads without a WASM runtime |
| [`samples/send_recv_stream/`](samples/send_recv_stream/) | Streaming echo filter — writes each request body chunk back as it arrives |
| [`samples/send_recv_all/`](samples/send_recv_all/) | Buffered filter — accumulates the body and responds with `sendLocalResponse()` |
| [`samples/send_stream_size/`](samples/send_stream_size/) | Streaming size generator — returns a caller-specified number of bytes for download and throughput benchmarking |

See each sample's `README.md` for build and usage instructions.

---

## Performance

### WAMR AOT precompilation

By default, WAMR executes WASM modules in **interpreter mode**, which can be
significantly slower than native code.  For production deployments,
**Ahead-of-Time (AOT) compilation** eliminates interpreter overhead by
converting WASM bytecode to native machine code at build time.

> **Note:** This section applies only to the **WAMR** runtime.  Wasmtime and V8
> use JIT compilation natively and do not require a separate AOT step.  For
> **WasmEdge** AOT, see the [WasmEdge AOT section](#wasmedge-aot-precompilation)
> below.  The embedded AOT custom sections are safely ignored by runtimes that
> don't recognize them.

#### 1. Build the WAMR AOT compiler (`wamrc`)

`wamrc` ships with the WAMR source tree and requires LLVM to build:

```bash
# Install LLVM (Ubuntu/Debian)
sudo apt-get install -y llvm-18-dev libclang-18-dev lld-18

# Build wamrc
cd third_party/wasm-micro-runtime/wamr-compiler
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build . -j$(nproc)
```

This produces `third_party/wasm-micro-runtime/wamr-compiler/build/wamrc`.

#### 2. AOT-compile your WASM module

```bash
# Compile to native code for the current platform
./third_party/wasm-micro-runtime/wamr-compiler/build/wamrc \
  --opt-level=3 \
  --output=my_filter.aot \
  my_filter.wasm
```

Common `wamrc` flags:

| Flag | Description |
|------|-------------|
| `--opt-level=N` | Optimization level (0–3; default 3) |
| `--target=TRIPLE` | Target triple (e.g. `x86_64`); defaults to host |
| `--cpu=NAME` | Target CPU (e.g. `generic`, `znver2`, `skylake`) |
| `--output=FILE` | Output `.aot` file path |

#### 3. Embed the AOT code in the WASM file

The AOT binary must be embedded as a WebAssembly custom section named
`"wamr-aot"` inside the original `.wasm` file.  A helper script is provided:

```bash
python3 tools/append_aot_to_wasm.py my_filter.wasm my_filter.aot my_filter_aot.wasm
```

This produces a single `.wasm` file containing both the original bytecode
(for portability) and the native AOT code (for WAMR performance).  The
resulting file works with **all** runtimes — non-WAMR runtimes simply ignore
the custom section.

#### 4. Deploy the AOT-enabled WASM file

Replace your deployed `.wasm` module with the AOT-embedded version:

```bash
cp my_filter_aot.wasm /usr/local/lsws/fcgi-bin/my_filter.wasm
```

No changes to the lswasm command line or LiteSpeed configuration are needed —
lswasm automatically detects and uses the `"wamr-aot"` custom section at
startup when running with the WAMR runtime.

#### Quick reference (end-to-end)

```bash
# Build (one-time)
wamrc --opt-level=3 --output=filter.aot filter.wasm

# Embed AOT in WASM
python3 tools/append_aot_to_wasm.py filter.wasm filter.aot filter_aot.wasm

# Deploy
cp filter_aot.wasm /usr/local/lsws/fcgi-bin/filter.wasm
```

### WasmEdge AOT precompilation

By default, WasmEdge executes WASM modules in **interpreter mode**.  For
production deployments, `wasmedgec` compiles WASM to native code in a
"universal WASM" format that WasmEdge loads and executes natively.

Unlike WAMR (which produces a raw `.aot` binary that must be embedded as a
custom section), WasmEdge's `wasmedgec` produces a **valid `.wasm` file** with
native code embedded alongside the original bytecode.  This means the output
can be used directly with `--module` — no extra embedding step is needed.

#### 1. AOT-compile with `wasmedgec`

`wasmedgec` ships with any standard WasmEdge installation:

```bash
# Install WasmEdge (if not already installed)
curl -sSf https://raw.githubusercontent.com/WasmEdge/WasmEdge/master/utils/install.sh | bash

# AOT-compile to "universal WASM" format
~/.wasmedge/bin/wasmedgec my_filter.wasm my_filter_aot.wasm
```

The output `my_filter_aot.wasm` is a valid WASM file with native code
embedded — WasmEdge will detect and use it automatically.

#### 2. Deploy

Simply pass the AOT-compiled file to lswasm:

```bash
cp my_filter_aot.wasm /usr/local/lsws/fcgi-bin/my_filter.wasm
```

That's it — no custom section embedding required.

#### Quick reference (end-to-end)

```bash
# AOT compile (one-time)
wasmedgec filter.wasm filter_aot.wasm

# Deploy
cp filter_aot.wasm /usr/local/lsws/fcgi-bin/filter.wasm
```

<details>
<summary>Advanced: Cross-runtime WASM files with both WAMR and WasmEdge AOT</summary>

For deployments where the same `.wasm` file must work optimally with multiple
runtimes, lswasm also supports embedding the WasmEdge universal WASM as a
`"wasmedge-aot"` custom section.  When running with the WasmEdge runtime,
lswasm extracts this section automatically.

```bash
# Build WAMR AOT
wamrc --opt-level=3 --output=filter.aot filter.wasm

# Build WasmEdge AOT
wasmedgec filter.wasm filter_wasmedge_aot.wasm

# Embed WAMR AOT section
python3 tools/append_aot_to_wasm.py filter.wasm filter.aot filter_with_wamr.wasm

# Embed WasmEdge AOT section
python3 tools/append_aot_to_wasm.py filter_with_wamr.wasm filter_wasmedge_aot.wasm filter_both.wasm \
    --section-name wasmedge-aot
```

The resulting `filter_both.wasm` works with all runtimes — WAMR uses its
`"wamr-aot"` section, WasmEdge uses its `"wasmedge-aot"` section, and
Wasmtime/V8 ignore both and JIT-compile normally.
</details>

### Runtime AOT/JIT comparison

| Runtime | Execution mode | Precompilation required? |
|---------|---------------|--------------------------|
| **Wasmtime** | JIT (Cranelift) | No — native speed at startup |
| **V8** | JIT (TurboFan) | No — native speed after warmup |
| **WAMR** | Interpreter (default) or AOT | **Yes** — use `wamrc` + embed `"wamr-aot"` section |
| **WasmEdge** | Interpreter (default) or AOT | **Yes** — use `wasmedgec` (output is directly usable) |

---

## Testing

By default (LSPROXY mode), lswasm listens on a **Unix domain socket** at
`/tmp/lswasm.sock`.  Use `--port` to switch to TCP mode for direct `curl`
testing.

### UDS health check

```bash
# Terminal 1:
./lswasm --module samples/sample_filter/sample_filter.wasm --lsproxy

# Terminal 2:
curl --unix-socket /tmp/lswasm.sock http://localhost/
```

### TCP health check

```bash
# Terminal 1:
./lswasm --module samples/sample_filter/sample_filter.wasm --lsproxy --port 8080

# Terminal 2:
curl http://localhost:8080/
```

Expected output:

```
=== Environment Variables ===

Environment variable count: 0

(no environment variables set)


=== Request Headers ===

Header count: 7

  :method: GET
  :path: /
  :scheme: http
  :authority: localhost
  Host: localhost
  User-Agent: curl/8.5.0
  Accept: */*
```

### Raw request with `netcat`

```bash
echo -e "GET / HTTP/1.1\r\n\r\n" | nc localhost 8080
```

---

## Development

### Reconfiguring after runtime installation

If you install a new WASM runtime after the initial build, reconfigure CMake:

```bash
cd build
cmake --fresh ..
cmake --build . -j$(nproc)
```

### Viewing compiler commands

`build/compile_commands.json` contains detailed compiler invocations for use by
IDEs and tools like `clangd`.

---

## Troubleshooting

### Enabling debug logging

Debug logging is activated in either of two ways:

- Pass `--debug` on the command line used to start lswasm.
- Create the trigger file `/tmp/lswasm.dolog`.

Logs are written to `/tmp/lswasm.log`.  Start troubleshooting by enabling
logging and examining this file.

### Verifying `lswasm` is working

Use the [health checks](#testing) above before relying on the LiteSpeed
configuration.  If they fail, enable debug logging and check the logs.

### LiteSpeed output

Check the LiteSpeed error logs.  If running in LiteSpeed mode, these will be `error.log` and `stderr.log` in `/usr/local/lsws/logs`.  If you are running in Apache mode, these are typically in the `/var/log/apache2` directory.

### Runtime not found

```
-- Wasmtime not found - install via: cargo install wasmtime-cli or apt install libwasmtime-dev
```

**Fix:** Install the missing runtime using the commands in the
[Prerequisites](#wasm-runtimes) section.

### CMake not found

```
cmake: command not found
```

**Fix:**

```bash
# Ubuntu/Debian
sudo apt-get install -y cmake

# macOS
brew install cmake
```

### Permission denied on bind

```
Failed to bind socket to port 8080
```

**Fix:** Use a port above 1024:

```bash
./lswasm --lsproxy --port 8000
```

---

## License

Copyright 2026 LiteSpeed Technologies, Inc.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

See <http://www.gnu.org/licenses/>.

---

## Contributing

Contributions are welcome! Please submit pull requests and issues to the
[project repository](https://github.com/litespeedtech/lswasm).

---

## References

- [proxy-wasm-cpp-host](https://github.com/proxy-wasm/proxy-wasm-cpp-host)
- [proxy-wasm-cpp-sdk](https://github.com/proxy-wasm/proxy-wasm-cpp-sdk)
- [Wasmtime](https://docs.wasmtime.dev/)
- [V8](https://v8.dev/)
- [V8 Build Guide](https://v8.dev/docs/build)
- [WasmEdge](https://wasmedge.org/)
- [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime)
