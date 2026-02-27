#include <stddef.h>
#include <stdint.h>

// Minimal Proxy-Wasm ABI types/constants used by this sample.
typedef uint32_t WasmResult;
typedef uint32_t LogLevel;
typedef uint32_t FilterHeadersStatus;
typedef uint32_t FilterDataStatus;
typedef uint32_t FilterTrailersStatus;
typedef uint32_t FilterMetadataStatus;

// Proxy-Wasm result/status enums (subset).
enum {
  WasmResult_Ok = 0,
};

enum {
  LogLevel_Trace = 0,
  LogLevel_Debug = 1,
  LogLevel_Info = 2,
  LogLevel_Warn = 3,
  LogLevel_Error = 4,
  LogLevel_Critical = 5,
};

enum {
  FilterHeadersStatus_Continue = 0,
  FilterHeadersStatus_StopIteration = 1,
};

enum {
  FilterDataStatus_Continue = 0,
};

enum {
  FilterTrailersStatus_Continue = 0,
};

// Host function imports used by this sample.
__attribute__((import_module("env"), import_name("proxy_log")))
extern WasmResult proxy_log(LogLevel level, const char *message, size_t message_size);

__attribute__((import_module("env"), import_name("proxy_send_local_response")))
extern WasmResult proxy_send_local_response(uint32_t response_code,
                                             const char *response_code_details_ptr,
                                             size_t response_code_details_size,
                                             const char *body_ptr, size_t body_size,
                                             const char *additional_headers_ptr,
                                             size_t additional_headers_size,
                                             uint32_t grpc_status);

// WASI environ imports for reading environment variables.
__attribute__((import_module("wasi_snapshot_preview1"), import_name("environ_sizes_get")))
extern uint32_t __wasi_environ_sizes_get(uint32_t *environ_count, uint32_t *environ_buf_size);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("environ_get")))
extern uint32_t __wasi_environ_get(uint32_t **environ, char *environ_buf);

// Keep exported symbols visible in the wasm module.
#define WASM_EXPORT __attribute__((used)) __attribute__((visibility("default")))

// Simple bump allocator required by proxy-wasm-cpp-host for memory allocation.
// Uses __heap_base provided by the wasm-ld linker as the start of the heap.
extern unsigned char __heap_base;
static unsigned char *heap_ptr = &__heap_base;

WASM_EXPORT void *malloc(size_t size) {
  // Align to 8 bytes.
  size = (size + 7u) & ~7u;
  unsigned char *ptr = heap_ptr;
  heap_ptr += size;
  return (void *)ptr;
}

WASM_EXPORT void free(void *ptr) {
  // Bump allocator: free is a no-op.
  (void)ptr;
}

// Required ABI version marker for proxy-wasm host compatibility.
WASM_EXPORT void proxy_abi_version_0_2_1(void) {}

static void log_msg(const char *msg, size_t len) {
  (void)proxy_log(LogLevel_Info, msg, len);
}

// Simple unsigned integer to decimal string (no libc available).
// Returns pointer into buf where the number starts.
static char *uint_to_str(uint32_t val, char *buf, size_t buf_size) {
  char *p = buf + buf_size - 1;
  *p = '\0';
  if (val == 0) {
    --p;
    *p = '0';
    return p;
  }
  while (val > 0 && p > buf) {
    --p;
    *p = '0' + (char)(val % 10);
    val /= 10;
  }
  return p;
}

// Simple strlen (no libc).
static size_t my_strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

// Buffers for WASI environ data (static since we have no malloc).
#define ENV_MAX_PTRS 256
#define ENV_BUF_SIZE 16384
static uint32_t *env_ptrs[ENV_MAX_PTRS];
static char env_buf[ENV_BUF_SIZE];

// Helper: append a string to the body buffer, return new offset.
static size_t body_append(char *body, size_t offset, size_t max,
                          const char *src, size_t src_len) {
  for (size_t i = 0; i < src_len && offset < max; i++, offset++)
    body[offset] = src[i];
  return offset;
}

// Response body buffer for environment variable output.
#define BODY_BUF_SIZE 32768
static char body_buf[BODY_BUF_SIZE];

// Build an HTTP response body containing all environment variables.
// Returns the body length written into body_buf.
static size_t build_environ_body(void) {
  size_t off = 0;
  uint32_t env_count = 0;
  uint32_t env_buf_size_val = 0;

  // Header line.
  {
    static const char hdr[] = "=== Environment Variables ===\n\n";
    off = body_append(body_buf, off, BODY_BUF_SIZE, hdr, sizeof(hdr) - 1);
  }

  if (__wasi_environ_sizes_get(&env_count, &env_buf_size_val) != 0) {
    static const char err[] = "Error: failed to retrieve environment variable sizes\n";
    off = body_append(body_buf, off, BODY_BUF_SIZE, err, sizeof(err) - 1);
    return off;
  }

  // Count line.
  {
    static const char prefix[] = "Environment variable count: ";
    off = body_append(body_buf, off, BODY_BUF_SIZE, prefix, sizeof(prefix) - 1);
    char num_buf[16];
    char *num = uint_to_str(env_count, num_buf, sizeof(num_buf));
    off = body_append(body_buf, off, BODY_BUF_SIZE, num, my_strlen(num));
    off = body_append(body_buf, off, BODY_BUF_SIZE, "\n\n", 2);
  }

  if (env_count == 0) {
    static const char msg[] = "(no environment variables set)\n";
    off = body_append(body_buf, off, BODY_BUF_SIZE, msg, sizeof(msg) - 1);
    return off;
  }

  // Clamp to our static buffer limits.
  if (env_count > ENV_MAX_PTRS) env_count = ENV_MAX_PTRS;
  if (env_buf_size_val > ENV_BUF_SIZE) env_buf_size_val = ENV_BUF_SIZE;

  if (__wasi_environ_get(env_ptrs, env_buf) != 0) {
    static const char err[] = "Error: failed to retrieve environment variables\n";
    off = body_append(body_buf, off, BODY_BUF_SIZE, err, sizeof(err) - 1);
    return off;
  }

  // List each environment variable.
  for (uint32_t i = 0; i < env_count && off < BODY_BUF_SIZE; i++) {
    const char *entry = (const char *)env_ptrs[i];
    if (entry) {
      size_t entry_len = my_strlen(entry);
      off = body_append(body_buf, off, BODY_BUF_SIZE, "  ", 2);
      off = body_append(body_buf, off, BODY_BUF_SIZE, entry, entry_len);
      off = body_append(body_buf, off, BODY_BUF_SIZE, "\n", 1);
    }
  }

  return off;
}

// Lifecycle callbacks.
WASM_EXPORT uint32_t proxy_on_vm_start(uint32_t root_context_id, uint32_t vm_configuration_size) {
  (void)root_context_id;
  (void)vm_configuration_size;
  static const char msg[] = "sample_filter: proxy_on_vm_start";
  log_msg(msg, sizeof(msg) - 1);

  return 1; // true
}

WASM_EXPORT uint32_t proxy_validate_configuration(uint32_t root_context_id,
                                                  uint32_t configuration_size) {
  (void)root_context_id;
  (void)configuration_size;
  return 1; // true
}

WASM_EXPORT uint32_t proxy_on_configure(uint32_t root_context_id, uint32_t configuration_size) {
  (void)root_context_id;
  (void)configuration_size;
  static const char msg[] = "sample_filter: proxy_on_configure";
  log_msg(msg, sizeof(msg) - 1);
  return 1; // true
}

WASM_EXPORT void proxy_on_tick(uint32_t root_context_id) {
  (void)root_context_id;
}

WASM_EXPORT void proxy_on_context_create(uint32_t context_id, uint32_t parent_context_id) {
  (void)context_id;
  (void)parent_context_id;
  static const char msg[] = "sample_filter: proxy_on_context_create";
  log_msg(msg, sizeof(msg) - 1);
}

// HTTP callbacks.
WASM_EXPORT FilterHeadersStatus proxy_on_request_headers(uint32_t context_id,
                                                         uint32_t headers,
                                                         uint32_t end_of_stream) {
  (void)context_id;
  (void)headers;
  (void)end_of_stream;
  static const char msg[] = "sample_filter: proxy_on_request_headers";
  log_msg(msg, sizeof(msg) - 1);

  // Build environment variable listing and send as HTTP response body.
  size_t body_len = build_environ_body();
  static const char status_msg[] = "OK";
  // No additional headers; grpc_status -1 means not a gRPC call.
  proxy_send_local_response(200, status_msg, sizeof(status_msg) - 1,
                            body_buf, body_len,
                            (const char *)0, 0,
                            (uint32_t)-1);

  return FilterHeadersStatus_StopIteration;
}

WASM_EXPORT FilterMetadataStatus proxy_on_request_metadata(uint32_t context_id, uint32_t elements) {
  (void)context_id;
  (void)elements;
  return 0;
}

WASM_EXPORT FilterDataStatus proxy_on_request_body(uint32_t context_id,
                                                   uint32_t body_buffer_length,
                                                   uint32_t end_of_stream) {
  (void)context_id;
  (void)body_buffer_length;
  (void)end_of_stream;
  static const char msg[] = "sample_filter: proxy_on_request_body";
  log_msg(msg, sizeof(msg) - 1);
  return FilterDataStatus_Continue;
}

WASM_EXPORT FilterTrailersStatus proxy_on_request_trailers(uint32_t context_id, uint32_t trailers) {
  (void)context_id;
  (void)trailers;
  return FilterTrailersStatus_Continue;
}

WASM_EXPORT FilterHeadersStatus proxy_on_response_headers(uint32_t context_id,
                                                          uint32_t headers,
                                                          uint32_t end_of_stream) {
  (void)context_id;
  (void)headers;
  (void)end_of_stream;
  static const char msg[] = "sample_filter: proxy_on_response_headers";
  log_msg(msg, sizeof(msg) - 1);
  return FilterHeadersStatus_Continue;
}

WASM_EXPORT FilterMetadataStatus proxy_on_response_metadata(uint32_t context_id, uint32_t elements) {
  (void)context_id;
  (void)elements;
  return 0;
}

WASM_EXPORT FilterDataStatus proxy_on_response_body(uint32_t context_id,
                                                    uint32_t body_buffer_length,
                                                    uint32_t end_of_stream) {
  (void)context_id;
  (void)body_buffer_length;
  (void)end_of_stream;
  static const char msg[] = "sample_filter: proxy_on_response_body";
  log_msg(msg, sizeof(msg) - 1);
  return FilterDataStatus_Continue;
}

WASM_EXPORT FilterTrailersStatus proxy_on_response_trailers(uint32_t context_id, uint32_t trailers) {
  (void)context_id;
  (void)trailers;
  return FilterTrailersStatus_Continue;
}

WASM_EXPORT uint32_t proxy_on_done(uint32_t context_id) {
  (void)context_id;
  static const char msg[] = "sample_filter: proxy_on_done";
  log_msg(msg, sizeof(msg) - 1);
  return 1; // true
}

WASM_EXPORT void proxy_on_log(uint32_t context_id) {
  (void)context_id;
}

WASM_EXPORT void proxy_on_delete(uint32_t context_id) {
  (void)context_id;
}

// Stream and connection callbacks (implemented as pass-through for completeness).
WASM_EXPORT uint32_t proxy_on_new_connection(uint32_t context_id) {
  (void)context_id;
  return 0;
}

WASM_EXPORT uint32_t proxy_on_downstream_data(uint32_t context_id,
                                              uint32_t data_length,
                                              uint32_t end_of_stream) {
  (void)context_id;
  (void)data_length;
  (void)end_of_stream;
  return 0;
}

WASM_EXPORT uint32_t proxy_on_upstream_data(uint32_t context_id,
                                            uint32_t data_length,
                                            uint32_t end_of_stream) {
  (void)context_id;
  (void)data_length;
  (void)end_of_stream;
  return 0;
}

WASM_EXPORT void proxy_on_downstream_connection_close(uint32_t context_id, uint32_t close_type) {
  (void)context_id;
  (void)close_type;
}

WASM_EXPORT void proxy_on_upstream_connection_close(uint32_t context_id, uint32_t close_type) {
  (void)context_id;
  (void)close_type;
}
