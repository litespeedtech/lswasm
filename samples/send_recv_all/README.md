# send_recv_all — Streaming Receive-All Proxy-Wasm Filter

A proxy-wasm filter written in C++ using the
[proxy-wasm-cpp-sdk](https://github.com/proxy-wasm/proxy-wasm-cpp-sdk)
that implements the
[proxy-wasm ABI v0.2.1](https://github.com/proxy-wasm/spec/tree/master/abi-versions/v0.2.1).
It can be loaded by `lswasm` via the `--module` flag.

## What it does

On every incoming request, the filter:

1. **`onRequestHeaders`** — Reads WASI environment variables and all request
   headers; formats them into a human-readable block.  Appends a
   `=== Request Body ===` title so the caller knows the body will follow.
   Returns `Continue` (not `StopIteration`) so the host delivers body data.

2. **`onRequestBody`** — Each time a body chunk arrives the filter calls
   `getBufferBytes(WasmBufferType::HttpRequestBody, …)` to fetch the chunk
   and appends it to an internal accumulator.  Returns
   `FilterDataStatus::StopIterationAndBuffer` until `end_of_stream` is `true`,
   at which point the complete local response (header section + body) is sent
   via `sendLocalResponse(200, …)`.

3. **`onResponseHeaders`** (for proxied requests) — Adds `X-Wasm-Filter` and
   `X-Powered-By` custom response headers.

### Why streaming matters

Each `onRequestBody` call handles exactly one chunk rather than accumulating the
entire request before processing begins.  This means:

* The host input buffer never needs to hold more than one chunk at a time.
* Memory usage inside the WASM module is bounded by `chunk_size + output_size`.
* Very large POST bodies are handled without exhausting either the input or the
  output buffer.

## Prerequisites

* **WASI SDK** (≥ 29) — provides `clang++`, `wasm-ld`, and a wasi-sysroot
  with C++ headers (libc++/libc++abi).
  Download from <https://github.com/WebAssembly/wasi-sdk/releases>.  For example:

```bash
cd third_party
wget https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-29/wasi-sdk-29.0-x86_64-linux.tar.gz
tar xf wasi-sdk-29.0-x86_64-linux.tar.gz
cd ..
```

* **proxy-wasm-cpp-sdk** — included as a git submodule at
  `third_party/proxy-wasm-cpp-sdk/`.  Make sure submodules are initialised:

```bash
git submodule update --init --recursive
```

## Building

### With CMake + toolchain file (recommended)

Assuming the path to the WASI SDK is (as above):
```bash
mkdir samples/send_recv_all/build
cmake -S samples/send_recv_all -B samples/send_recv_all/build \
    -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi-toolchain.cmake \
    -DWASI_SDK_PATH=third_party/wasi-sdk-29.0-x86_64-linux
cmake --build samples/send_recv_all/build
```

The resulting `send_recv_all.wasm` is written to `samples/send_recv_all/build/`.

You can also set the `WASI_SDK_PATH` environment variable instead of
passing it as a CMake cache variable.

#### Debug build

Pass `-DCMAKE_BUILD_TYPE=Debug` to produce an unstripped, unoptimised
WASM module suitable for debugging with Chrome DevTools or `wasm-gdb`:

```bash
cmake -S samples/send_recv_all -B samples/send_recv_all/build-debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi-toolchain.cmake \
  -DWASI_SDK_PATH=/path/to/wasi-sdk-29.0 \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build samples/send_recv_all/build-debug
```

### Without a toolchain file

The `CMakeLists.txt` includes a fallback that configures the WASI SDK
inline if no toolchain file is provided:

```bash
cmake -S samples/send_recv_all -B samples/send_recv_all/build \
  -DWASI_SDK_PATH=/path/to/wasi-sdk-29.0
cmake --build samples/send_recv_all/build
```

### Manual one-liner

```bash
WASI_SDK=/path/to/wasi-sdk-29.0
$WASI_SDK/bin/clang++ --target=wasm32-wasi -O2 -std=c++17 \
  --sysroot=$WASI_SDK/share/wasi-sysroot \
  -fvisibility=hidden -fno-exceptions -fno-rtti \
  -I third_party/proxy-wasm-cpp-sdk \
  third_party/proxy-wasm-cpp-sdk/proxy_wasm_intrinsics.cc \
  samples/send_recv_all/send_recv_all.cpp \
  -mexec-model=reactor \
  -Wl,--export=memory -Wl,--export=malloc -Wl,--export=free \
  -Wl,--export=proxy_abi_version_0_2_1 \
  -Wl,--export=proxy_on_context_create \
  -Wl,--export=proxy_on_vm_start \
  -Wl,--export=proxy_on_configure \
  -Wl,--export=proxy_on_request_headers \
  -Wl,--export=proxy_on_request_body \
  -Wl,--export=proxy_on_response_headers \
  -Wl,--export=proxy_on_done \
  -Wl,--export=proxy_on_log \
  -Wl,--export=proxy_on_delete \
  -Wl,--strip-all -Wl,--allow-undefined \
  -o samples/send_recv_all/send_recv_all.wasm
```

> **Note:** `-mexec-model=reactor` replaces the old `-Wl,--no-entry -Wl,--export-all`
> flags, producing a WASI reactor that only exports the symbols the host needs.
> The CMake build exports the full set of proxy-wasm ABI functions; the
> one-liner above exports only the ones this particular filter implements.

## Usage with lswasm

```bash
./build/lswasm --module samples/send_recv_all/send_recv_all.wasm
```

### GET request (no body)

```bash
curl --unix-socket /tmp/lswasm.sock http://localhost/
```

The response body will contain the environment variables, all request headers,
and a note that there is no body.

### POST request (with body)

```bash
curl --unix-socket /tmp/lswasm.sock \
     -X POST -d "Hello, streaming body!" \
     http://localhost/
```

The response body will contain the environment variables, request headers, and
then the `=== Request Body ===` section followed by the posted body text.

### Large body test

```bash
# Generate a 1 MB body and POST it.
dd if=/dev/urandom bs=1024 count=1024 2>/dev/null | \
  base64 | \
  curl --unix-socket /tmp/lswasm.sock \
       -X POST --data-binary @- \
       http://localhost/
```

The filter will stream the body in chunks without filling up the input or
output buffers.

## Sample output

```
=== Environment Variables ===

Environment variable count: 3

  PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
  HOME=/root
  LANG=C.UTF-8


=== Request Headers ===

Header count: 6

  :method: POST
  :path: /
  :scheme: http
  :authority: localhost
  Host: localhost
  Content-Length: 22

=== Request Body ===

Hello, streaming body!
```

## File overview

| File | Description |
|---|---|
| `send_recv_all.cpp` | C++ source using proxy-wasm-cpp-sdk (`RootContext` / `Context`) |
| `CMakeLists.txt` | CMake build file (WASI SDK toolchain, SDK include path) |
| `README.md` | This file |

> **Note:** `send_recv_all.wasm` is **not** checked into the repository.
> Build it from source using the instructions above.
