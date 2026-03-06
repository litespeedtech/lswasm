# Changelog

All notable changes to lswasm are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-03-06

### Added
- Epoll-based HTTP proxy server with proxy-wasm filter support.
- Four WASM runtime backends: Wasmtime (default), V8, WasmEdge, WAMR.
- Unix domain socket listener (default: `/tmp/lswasm.sock`).
- TCP listener mode via `--port`.
- Configurable UDS file permissions via `--sock-perm`.
- WASM module loading via `--module`.
- WASM environment variable injection via `--env KEY=VALUE` (repeatable).
- Diagnostic response body via `--body-pacifier`.
- Debug logging via `--debug` (writes to `/tmp/lshttpd/lswasm.log`).
- `--version` and `--help` CLI flags.
- Graceful shutdown on SIGINT/SIGTERM with `sigaction()`.
- SHA-256 ETag generation for response caching.
- Sample proxy-wasm filter (`samples/sample_filter.cpp`).
