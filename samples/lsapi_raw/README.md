# lsapi_raw — Raw LSAPI prefork benchmark baseline

A native C++ LSAPI application that provides a **performance baseline**
for benchmarking lswasm, isolating LSAPI transport overhead from WASM
runtime overhead.  It uses the **LSAPI prefork model** (fork-based
worker processes) — the same concurrency model used by PHP-LSAPI and
other traditional LSAPI applications.

## Modes of operation

### A. Size-based generation (query string present)

If the request URL contains a numeric query string (e.g. `?1048576`),
`lsapi_raw` generates that many bytes of pseudo-random printable ASCII
data and streams it back.  This mirrors the `send_stream_size` WASM
filter, making it straightforward to compare native LSAPI throughput
against the WASM-hosted equivalent.

### B. Echo mode (no query string)

Without a query string, `lsapi_raw` replicates the same streaming echo
behaviour as the `send_recv_stream` WASM filter: dumps environment
variables and request headers, then echoes the request body back
chunk-by-chunk.

## How it works

| Phase                | Action                                                    |
|----------------------|-----------------------------------------------------------|
| Startup              | Initialises LSAPI; reads `LSAPI_CHILDREN` from environment (or `--children` flag) to enable the prefork worker pool. |
| Prefork accept       | The parent process manages a pool of forked child workers.  Each child accepts one connection at a time via `LSAPI_Prefork_Accept_r()`. |
| Dispatch             | Checks the query string via `LSAPI_GetQueryString()`.  If it parses as a positive integer → size mode; otherwise → echo mode. |
| **Size mode**        | Sends `200 OK` with `Content-Length`, generates printable data in 32 KB chunks using a fast xorshift64* PRNG, reusing the buffer. |
| **Echo mode** headers | Sends `200 OK` with `Content-Type: text/plain`.         |
| Echo: env dump       | Iterates LSAPI environment + special env vars via `LSAPI_ForeachEnv()` / `LSAPI_ForeachSpecialEnv()`. |
| Echo: header dump    | Iterates original request headers via `LSAPI_ForeachOrgHeader()`. |
| Echo: body (streamed)| Reads body in 16 KB chunks with `LSAPI_ReadReqBody()` and writes each chunk immediately with `LSAPI_Write()`. |

## Prerequisites

* A C++17 compiler (gcc, clang)
* CMake ≥ 3.16
* LiteSpeed / OpenLiteSpeed (for the inherited-socket deployment model), or
  use `--bind` for standalone testing.

## Building

From the lswasm project root:

```bash
# Configure
cmake -S samples/lsapi_raw -B samples/lsapi_raw/build

# Build
cmake --build samples/lsapi_raw/build
```

This produces `samples/lsapi_raw/build/lsapi_raw`.

## Running

### Inherited socket (LiteSpeed-managed)

Configure `lsapi_raw` as an LSAPI external application in LiteSpeed /
OpenLiteSpeed — the same way you would configure lswasm in LSAPI mode.
Set the `LSAPI_CHILDREN` environment variable (typically via the
**Environment** field in the External App configuration) to control the
number of forked worker processes:

```
LSAPI_CHILDREN=20
```

### Standalone (explicit bind)

```bash
# TCP listener with 20 prefork workers
./samples/lsapi_raw/build/lsapi_raw --bind 127.0.0.1:8000 --children 20

# Unix domain socket with 20 prefork workers
./samples/lsapi_raw/build/lsapi_raw --bind /tmp/lsapi_raw.sock --children 20

# Using environment variable instead of --children flag
LSAPI_CHILDREN=20 ./samples/lsapi_raw/build/lsapi_raw --bind 127.0.0.1:8000
```

## Testing

### Size-based generation

```bash
# Generate 1 MB of random printable data
curl -v http://localhost:8000/test?1048576

# Generate 100 bytes
curl -v http://localhost:8000/test?100
```

Expected: the response body is exactly the requested number of bytes of
printable ASCII text with `Content-Length` header set.

### Echo mode (no body)

```bash
curl -v http://localhost:8000/
```

Expected output:
```
=== Environment Variables ===
  ...
=== Request Headers ===
  ...
=== Request Body ===
(no body)
```

### Echo mode (streaming body)

```bash
# Generate 10 MB of data and POST it
dd if=/dev/urandom bs=1M count=10 2>/dev/null | \
  curl -v --data-binary @- http://localhost:8000/echo
```

The response should echo back all 10 MB plus the diagnostic preamble,
streaming incrementally.

## Benchmarking

### Size-based: lsapi_raw vs. send_stream_size

Compare native LSAPI throughput against the `send_stream_size` WASM filter
to quantify the per-request cost of the WASM runtime for large responses:

```bash
# Benchmark lsapi_raw (native baseline) — 1 MB responses
ab -n 10000 -c 20 http://localhost:8000/test?1048576

# Benchmark lswasm + send_stream_size.wasm — 1 MB responses
ab -n 10000 -c 20 http://localhost:8080/test?1048576
```

### Echo mode: lsapi_raw vs. send_recv_stream

```bash
# Benchmark lsapi_raw (prefork baseline)
ab -n 10000 -c 20 http://localhost:8000/

# Benchmark lswasm + send_recv_stream.wasm (same workload)
ab -n 10000 -c 20 http://localhost:8080/
```

The difference in throughput and latency reflects the WASM runtime overhead.
Both applications should use equivalent concurrency settings (e.g. 20 workers
in lsapi_raw vs. `--workers 20` in lswasm).
