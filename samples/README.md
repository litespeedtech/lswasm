# Sample Proxy-Wasm Filter

A proxy-wasm filter written in C++ using the
[proxy-wasm-cpp-sdk](https://github.com/proxy-wasm/proxy-wasm-cpp-sdk)
that implements the
[proxy-wasm ABI v0.2.1](https://github.com/proxy-wasm/spec/tree/master/abi-versions/v0.2.1).
It can be loaded by `lswasm` via the `--module` flag.

## What it does

On every incoming request, the filter:

1. Reads WASI environment variables and request headers.
2. Sends a **local response** (HTTP 200) whose body lists both.
3. On response headers (for proxied requests), adds `X-Wasm-Filter`
   and `X-Powered-By` custom headers.

Each lifecycle callback also logs a message via `LOG_INFO()`.

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
mkdir samples/build
cmake -S samples -B samples/build \
    -DCMAKE_TOOLCHAIN_FILE=$HOME/lswasm/cmake/wasm32-wasi-toolchain.cmake \
    -DWASI_SDK_PATH=third_party/wasi-sdk-29.0-x86_64-linux
cmake --build samples/build
```

The resulting `sample_filter.wasm` is written to `samples/build/`.

You can also set the `WASI_SDK_PATH` environment variable instead of
passing it as a CMake cache variable.

#### Debug build

Pass `-DCMAKE_BUILD_TYPE=Debug` to produce an unstripped, unoptimised
WASM module suitable for debugging with Chrome DevTools or `wasm-gdb`:

```bash
cmake -S samples -B samples/build-debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi-toolchain.cmake \
  -DWASI_SDK_PATH=/path/to/wasi-sdk-29.0 \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build samples/build-debug
```

### Without a toolchain file

The `CMakeLists.txt` includes a fallback that configures the WASI SDK
inline if no toolchain file is provided:

```bash
cmake -S samples -B samples/build \
  -DWASI_SDK_PATH=/path/to/wasi-sdk-29.0
cmake --build samples/build
```

### Manual one-liner

```bash
WASI_SDK=/path/to/wasi-sdk-29.0
$WASI_SDK/bin/clang++ --target=wasm32-wasi -O2 -std=c++17 \
  --sysroot=$WASI_SDK/share/wasi-sysroot \
  -fvisibility=hidden -fno-exceptions -fno-rtti \
  -I third_party/proxy-wasm-cpp-sdk \
  third_party/proxy-wasm-cpp-sdk/proxy_wasm_intrinsics.cc \
  samples/sample_filter.cpp \
  -mexec-model=reactor \
  -Wl,--export=memory -Wl,--export=malloc -Wl,--export=free \
  -Wl,--export=proxy_abi_version_0_2_1 \
  -Wl,--export=proxy_on_context_create \
  -Wl,--export=proxy_on_vm_start \
  -Wl,--export=proxy_on_configure \
  -Wl,--export=proxy_on_request_headers \
  -Wl,--export=proxy_on_response_headers \
  -Wl,--export=proxy_on_request_body \
  -Wl,--export=proxy_on_done \
  -Wl,--export=proxy_on_log \
  -Wl,--export=proxy_on_delete \
  -Wl,--strip-all -Wl,--allow-undefined \
  -o samples/sample_filter.wasm
```

> **Note:** `-mexec-model=reactor` replaces the old `-Wl,--no-entry -Wl,--export-all`
> flags, producing a WASI reactor that only exports the symbols the host
> needs.  The CMake build exports the full set of proxy-wasm ABI functions;
> the one-liner above exports only the ones this particular filter implements.

## Usage with lswasm

```bash
./build/lswasm --module samples/sample_filter.wasm
```

Then send a request (via the default Unix domain socket):

```bash
curl --unix-socket /tmp/lswasm.sock http://localhost/
```

The server console will show the filter's log messages as the request is
processed through the proxy-wasm callback chain, and the response body
will contain environment variables and request headers.

## File overview

| File | Description |
|---|---|
| `sample_filter.cpp` | C++ source using proxy-wasm-cpp-sdk (`RootContext` / `Context`) |
| `CMakeLists.txt` | CMake build file (WASI SDK toolchain, SDK include path) |
| `README.md` | This file |

> **Note:** `sample_filter.wasm` is **not** checked into the repository.
> Build it from source using the instructions above.
