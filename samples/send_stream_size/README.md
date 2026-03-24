# send_stream_size — Streaming size-based response generator

A proxy-wasm sample filter that generates a response of a caller-specified
size using the **lswasm streaming response API**.

The response size is extracted from the query string of the `:path:` request
pseudo-header.  The filter generates printable ASCII text in blocks of up to
32 KB and streams them to the client via `writeResponseChunk()`.  The output
is safe to display in a browser.

This is useful for **download/throughput benchmarking** — simulating
realistic large-response scenarios without requiring a backend or static
files of specific sizes.

## How it works

| Phase              | Action                                                     |
|--------------------|------------------------------------------------------------|
| `onRequestHeaders` | Reads `:path:`, extracts the numeric query string as the response size. Returns 400 if no valid size is found. |
| Streaming          | Sends `200 OK` with `Content-Type: text/plain` and `Content-Length`, then writes printable ASCII data in ≤ 32 KB chunks via `writeResponseChunk()` until the full size is sent. Calls `finishResponse()` to complete. |
| Fallback           | If streaming is not supported, falls back to `sendLocalResponse()` — works for small sizes but may hit buffer limits for large responses. |

## Prerequisites

* **WASI SDK** ≥ 29.0 — <https://github.com/WebAssembly/wasi-sdk/releases>
* **lswasm** built with WAMR or WasmEdge runtime
* **proxy-wasm-cpp-sdk** submodule checked out (`git submodule update --init`)

## Building

From the lswasm project root:

```bash
# Configure (adjust WASI_SDK_PATH for your installation)
cmake -S samples/send_stream_size -B samples/send_stream_size/build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi-toolchain.cmake \
  -DWASI_SDK_PATH=/opt/wasi-sdk-29.0

# Build
cmake --build samples/send_stream_size/build
```

This produces `samples/send_stream_size/build/send_stream_size.wasm`.

## Running

Start lswasm with the built filter:

```bash
./build-wamr/lswasm -m samples/send_stream_size/build/send_stream_size.wasm \
  -l 0.0.0.0:8080
```

## Testing

### Generate 1 KB response

```bash
curl -o /dev/null -w '%{size_download} bytes, %{time_total}s\n' \
  http://localhost:8080/test?1024
```

### Generate 1 MB response

```bash
curl -o /dev/null -w '%{size_download} bytes, %{time_total}s\n' \
  http://localhost:8080/test?1048576
```

### Generate 100 MB response

```bash
curl -o /dev/null -w '%{size_download} bytes, %{time_total}s\n' \
  http://localhost:8080/test?104857600
```

### Missing size (error case)

```bash
curl -v http://localhost:8080/test
```

Expected output:
```
< HTTP/1.1 400 Bad Request
...
400 Bad Request

Usage: GET /any/path?SIZE
  SIZE = number of bytes to generate (must be > 0)

Example: GET /html/test.wasm?1048576
  → streams 1 MB of random data
```

### Throughput benchmarking with `wrk`

```bash
# 10 threads, 100 connections, 30 seconds, requesting 64 KB per response
wrk -t10 -c100 -d30s http://localhost:8080/bench?65536
```

### Comparison with `send_recv_stream`

`send_recv_stream` echoes back the request body — useful for testing
upload/round-trip scenarios.  `send_stream_size` generates a response of
any requested size — useful for testing pure download throughput without
needing to send a large request body.

## URL format

```
GET /any/path?SIZE
```

Where `SIZE` is a positive integer specifying the number of bytes to
generate.  The path component before `?` is ignored.  The query string
is parsed up to the first `&` or `#`, so additional parameters can follow:

```
GET /test?1048576&foo=bar    → generates 1 MB
GET /bench?65536#section     → generates 64 KB
```

## API reference

The streaming API is exposed via `samples/include/lswasm_streaming.h`:

| Function                                    | Description                               |
|---------------------------------------------|-------------------------------------------|
| `lswasm::streaming::isSupported()`          | Returns `true` if the host has the API    |
| `lswasm::streaming::sendResponseHeaders()`  | Send HTTP status + headers                |
| `lswasm::streaming::writeResponseChunk()`   | Write a chunk of body data                |
| `lswasm::streaming::finishResponse()`       | Signal end of response                    |

See the [streaming response API plan](../../plans/streaming-response-api.md)
for full design documentation.
