#pragma once

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace lswasm_log {

// Sentinel file: logging is only active if this file exists at startup.
inline constexpr const char *DOLOG_PATH = "/tmp/lshttpd/lswasm.dolog";
// Log file path.
inline constexpr const char *LOG_PATH = "/tmp/lswasm.log";

// Global state — header-only, guarded by inline variables (C++17).
// NOTE: Not thread-safe.  If multi-threading is added, protect with a mutex.
inline bool g_logging_enabled = false;
inline std::ofstream g_log_file;

/**
 * Initialize the logging subsystem.
 * Call once at program start.  Logging is enabled when the sentinel file
 * exists OR when \p debug is true (e.g. via the --debug CLI switch).
 * When creating the log file for the first time the permissions are set
 * to 0666 so that any process can append to it.
 */
inline void log_init(bool debug = false) {
  struct stat st;
  bool enable = debug;
  if (!enable && stat(DOLOG_PATH, &st) == 0) {
    enable = true;
  }
  if (enable) {
    // If the log file does not yet exist, create it with mode 0666.
    // Temporarily clear the umask so the requested permissions are applied
    // exactly (the default umask would mask out group/other write bits).
    if (stat(LOG_PATH, &st) != 0) {
      mode_t old_umask = ::umask(0);
      int fd = ::open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
      ::umask(old_umask);
      if (fd >= 0) {
        ::close(fd);
      }
    }
    g_log_file.open(LOG_PATH, std::ios::app);
    if (g_log_file.is_open()) {
      g_logging_enabled = true;
    }
  }
}

/**
 * Return a timestamp string in the form "YYYY-MM-DD HH:MM:SS.MMMMMM".
 */
inline std::string timestamp() {
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()) %
            std::chrono::seconds(1);
  std::tm tm_buf{};
  ::localtime_r(&time_t_now, &tm_buf);
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
      << '.' << std::setfill('0') << std::setw(6) << us.count();
  return oss.str();
}

/**
 * Write an informational message to the log file (if enabled).
 * Each line is prefixed with a timestamp.
 * Does NOT write to stdout/stderr.
 */
inline void log_info(const std::string &msg) {
  if (g_logging_enabled && g_log_file.is_open()) {
    g_log_file << timestamp() << " " << msg << std::endl;
  }
}

/**
 * Write an error message to stderr AND to the log file (if enabled).
 * Each line is prefixed with a timestamp.
 */
inline void log_error(const std::string &msg) {
  auto ts = timestamp();
  std::cerr << ts << " " << msg << std::endl;
  if (g_logging_enabled && g_log_file.is_open()) {
    g_log_file << ts << " " << msg << std::endl;
  }
}

} // namespace lswasm_log

// Convenience macros for stream-style logging.
// Usage:  LOG_INFO("Loading module: " << name << " (" << size << " bytes)");
//         LOG_ERROR("Failed to open: " << path);
#define LOG_INFO(expr) \
  do { \
    std::ostringstream _lswasm_oss; \
    _lswasm_oss << expr; \
    lswasm_log::log_info(_lswasm_oss.str()); \
  } while (0)

#define LOG_ERROR(expr) \
  do { \
    std::ostringstream _lswasm_oss; \
    _lswasm_oss << expr; \
    lswasm_log::log_error(_lswasm_oss.str()); \
  } while (0)
