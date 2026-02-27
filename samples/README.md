# Sample Proxy-Wasm Filter

A minimal proxy-wasm filter written in C that implements the
[proxy-wasm ABI v0.2.1](https://github.com/proxy-wasm/spec/tree/master/abi-versions/v0.2.1).
It can be loaded by `lswasm` via the `--module` flag.

## What it does

The filter logs a message at each proxy-wasm lifecycle callback:

| Callback | Behaviour |
|---|---|
| `proxy_on_vm_start` | Logs `"sample_filter: proxy_on_vm_start"` |
| `proxy_on_configure` | Logs `"sample_filter: proxy_on_configure"` |
| `proxy_on_context_create` | Logs `"sample_filter: proxy_on_context_create"` |
| `proxy_on_request_headers` | Logs `"sample_filter: proxy_on_request_headers"`, returns **Continue** |
| `proxy_on_request_body` | Logs `"sample_filter: proxy_on_request_body"`, returns **Continue** |
| `proxy_on_response_headers` | Logs `"sample_filter: proxy_on_response_headers"`, returns **Continue** |
| `proxy_on_response_body` | Logs `"sample_filter: proxy_on_response_body"`, returns **Continue** |
| `proxy_on_done` | Logs `"sample_filter: proxy_on_done"` |

All other callbacks are implemented as no-ops that return the appropriate
"continue" status.

## Prerequisites

* **clang** (≥ 14) with wasm32 target support
* **lld** (matching clang version) — provides `wasm-ld`

On Ubuntu/Debian:

```bash
sudo apt-get install -y clang-18 lld-18
```

## Building

### With CMake (recommended)

```bash
cmake -S samples -B samples/build
cmake --build samples/build
```

The resulting `sample_filter.wasm` is written to `samples/build/`.

### Manual one-liner

```bash
clang --target=wasm32-wasi -O2 -ffreestanding -fvisibility=hidden \
  -nostdlib -Wl,--no-entry -Wl,--export-all -Wl,--strip-all \
  samples/sample_filter.c -o samples/sample_filter.wasm
```

## Usage with lswasm

```bash
./build/lswasm --module samples/sample_filter.wasm
```

Then send a request:

```bash
curl http://localhost:8080/
```

The server console will show the filter's log messages as the request is
processed through the proxy-wasm callback chain.

## File overview

| File | Description |
|---|---|
| `sample_filter.c` | C source implementing the proxy-wasm 0.2.1 ABI |
| `CMakeLists.txt` | CMake build file (forces clang, targets wasm32-wasi) |
| `sample_filter.wasm` | Pre-built WASM binary (checked in for convenience) |
