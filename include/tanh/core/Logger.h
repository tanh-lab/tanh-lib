#pragma once

#include <cstdint>
#include <functional>
#include <string>

/// @namespace thl::Logger
/// @brief Unified logging facade with compile-time filtering, platform sinks,
///        file output, and optional callback forwarding.
///
/// All logging is synchronous: sinks run on the caller's thread.  The logger
/// is not real-time safe -- do not call from the audio thread.
namespace thl::Logger {

/// Severity levels, ordered from most to least severe.
enum class LogLevel : std::uint32_t {
    Error = 1,
    Warning = 2,
    Info = 3,
    Debug = 4,
};

/// A single log entry produced by the logger.
struct LogRecord {
    std::uint64_t seq = 0;              ///< Monotonic sequence number.
    std::int64_t timestamp_ms = 0;      ///< Wall-clock UTC epoch (ms).
    std::uint64_t monotonic_ns = 0;     ///< Steady-clock epoch (ns).
    std::uint32_t level =
        static_cast<std::uint32_t>(LogLevel::Info);  ///< Severity level.
    std::string group;                  ///< Logical group tag.
    std::string message;                ///< Formatted message body.
    std::string source;                 ///< Origin identifier (e.g. "native").
};

/// Signature for a user-provided log sink.
using Callback = std::function<void(const LogRecord&)>;

/// Global sink configuration.
///
/// Controls which sinks are active.  Apply with set_config(); read back
/// with get_config().  Defaults: platform on, file off, callback on.
struct LoggerConfig {
    bool platform_enabled = true;   ///< Platform sink (os_log / android_log / stdout+stderr).
    bool file_enabled = false;      ///< Logfmt file sink.
    bool callback_enabled = true;   ///< Gate for the registered callback.
    std::string file_path;          ///< Output path for the file sink (empty = no writes).
};

/// @brief Apply a new sink configuration.
///
/// Thread-safe.  Manages file stream lifecycle: closes the stream when
/// @c file_path changes or @c file_enabled becomes false; keeps it open
/// when the path is unchanged and the file sink stays enabled.
void set_config(const LoggerConfig& config);

/// Return a snapshot of the current configuration.
LoggerConfig get_config();

/// @brief Register a synchronous log callback.
///
/// @param cb  Callable invoked for every log record that passes the
///            compile-time level filter and the @c callback_enabled gate.
///
/// @note The callback runs on the caller's thread.  Slow work in the
///       callback will block the logging thread.
/// @note Re-entrant logging from inside the callback is guarded and
///       redirected to the default fallback sink.
/// @note If the callback captures plugin/host-owned objects, call
///       clear_callback() before those objects are torn down.
void set_callback(Callback cb);

/// Remove a previously registered callback.
void clear_callback();

/// Format a log record as a
/// [logfmt](https://brandur.org/logfmt)-style string.
std::string format_logfmt(const LogRecord& record);

/// @name Core logging functions
/// @{

/// Log a message at the given @p level.
void log(LogLevel level, const char* group, const char* message);

/// Log a message with an explicit @p source tag.
void log_with_source(LogLevel level,
                     const char* source,
                     const char* group,
                     const char* message);

/// printf-style logging at the given @p level.
void logf(LogLevel level, const char* group, const char* fmt, ...);

/// @}

/// @name Convenience shorthands
/// @{
void error(const char* group, const char* message);
void warning(const char* group, const char* message);
void info(const char* group, const char* message);
void debug(const char* group, const char* message);
/// @}

}  // namespace thl::Logger
