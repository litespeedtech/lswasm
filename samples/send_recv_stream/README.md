# send_recv_stream — Streaming Echo WASM Filter

A proxy-wasm sample filter that receives an HTTP request and echoes it back
using the **lswasm streaming response API** (`lswasm_send_response_headers`,
`lswasm_write_response_chunk`, `lswasm_finish_response`).

Unlike `send_recv_all` (which buffers the entire response body and calls
`sendLocalResponse()` once), this filter writes each chunk of body data to
the client as soon as it arrives — keeping memory usage constant regardless
of body size.

## How it works

| Phase              | Action                                                     |
|--------------------|------------------------------------------------------------|
| `onRequestHeaders` | Probes for streaming API support; builds env-var + header preamble. If no body is expected, sends the complete response immediately. |
| `onRequestBody`    | On first chunk: streams response headers + preamble. Each subsequent chunk is echoed via `writeResponseChunk()`. On `end_of_stream`: calls `finishResponse()`. |
| Fallback           | If streaming is not supported (e.g. on an older lswasm build), the filter falls back to `sendLocalResponse()` — identical to `send_recv_all`. |

## Prerequisites

* **WASI SDK** ≥ 29.0 — <https://github.com/WebAssembly/wasi-sdk/releases>
* **lswasm** built with WAMR or WasmEdge runtime
* **proxy-wasm-cpp-sdk** submodule checked out (`git submodule update --init`)

## Building

From the lswasm project root:

```bash
# Configure (adjust WASI_SDK_PATH for your installation)
cmake -S samples/send_recv_stream -B samples/send_recv_stream/build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi-toolchain.cmake \
  -DWASI_SDK_PATH=/opt/wasi-sdk-29.0

# Build
cmake --build samples/send_recv_stream/build
```

This produces `samples/send_recv_stream/build/send_recv_stream.wasm`.

## Running

Start lswasm with the built filter:

```bash
./build-wamr/lswasm -m samples/send_recv_stream/build/send_recv_stream.wasm \
  -l 0.0.0.0:8080
```

## Testing

### Small request (no body)

```bash
curl -v http://localhost:8080/
```

Expected output:
```
=== Environment Variables ===
  ...
=== Request Headers ===
  :method: GET
  :path: /
  ...
=== Request Body ===
(no body)
```

### Large request (streaming body)

```bash
# Generate 10 MB of data and POST it
dd if=/dev/urandom bs=1M count=10 2>/dev/null | curl -v --data-binary @- http://localhost:8080/echo
```

The response should echo back all 10 MB plus the diagnostic preamble,
streaming incrementally without timeout or buffer-overflow errors.

### Comparison with send_recv_all

Run the same large POST against `send_recv_all` for contrast — you should
see it either truncate the response or time out, demonstrating the
limitation that `send_recv_stream` solves.

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
