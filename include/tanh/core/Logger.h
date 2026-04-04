#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "tanh/core/Exports.h"

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
    std::uint64_t m_seq = 0;           ///< Monotonic sequence number.
    std::int64_t m_timestamp_ms = 0;   ///< Wall-clock UTC epoch (ms).
    std::uint64_t m_monotonic_ns = 0;  ///< Steady-clock epoch (ns).
    std::uint32_t m_level = static_cast<std::uint32_t>(LogLevel::Info);  ///< Severity level.
    std::string m_group;                                                 ///< Logical group tag.
    std::string m_message;  ///< Formatted message body.
    std::string m_source;   ///< Origin identifier (e.g. "native").
};

/// Signature for a user-provided log sink.
using Callback = std::function<void(const LogRecord&)>;

/// Global sink configuration.
///
/// Controls which sinks are active.  Apply with set_config(); read back
/// with get_config().  Defaults: platform on, file off, callback on.
struct LoggerConfig {
    bool m_platform_enabled = true;  ///< Platform sink (os_log / android_log / stdout+stderr).
    bool m_console_enabled = false;  ///< Explicit stdout/stderr sink (errors+warnings to stderr,
                                     ///< rest to stdout).
    bool m_file_enabled = true;      ///< Logfmt file sink.
    bool m_callback_enabled = true;  ///< Gate for the registered callback.
    std::string m_file_path;         ///< Output path for the file sink (empty = no writes).

    /// Maximum number of records to buffer while no callback is registered.
    /// When a callback is set via set_callback(), buffered records are
    /// replayed synchronously.  Set to 0 to disable buffering.
    std::size_t m_early_buffer_capacity = 64;
};

/// @brief Apply a new sink configuration.
///
/// Thread-safe.  Manages file stream lifecycle: closes the stream when
/// @c file_path changes or @c file_enabled becomes false; keeps it open
/// when the path is unchanged and the file sink stays enabled.
TANH_API void set_config(const LoggerConfig& config);

/// Return a snapshot of the current configuration.
TANH_API LoggerConfig get_config();

/// @brief Register a synchronous log callback.
///
/// @param cb  Callable invoked for every log record that passes the
///            compile-time level filter and the @c callback_enabled gate.
///
/// Any records buffered while no callback was registered are replayed
/// synchronously (oldest first) before the function returns.
///
/// @note The callback runs on the caller's thread.  Slow work in the
///       callback will block the logging thread.
/// @note Re-entrant logging from inside the callback is guarded and
///       redirected to the default fallback sink.
/// @note If the callback captures plugin/host-owned objects, call
///       clear_callback() before those objects are torn down.
TANH_API void set_callback(Callback cb);

/// Remove a previously registered callback.
TANH_API void clear_callback();

/// Format a log record as a plain human-readable string:
/// @c [level][source][group] message
TANH_API std::string format_plain(const LogRecord& record);

/// Format a log record as a
/// [logfmt](https://brandur.org/logfmt)-style string.
TANH_API std::string format_logfmt(const LogRecord& record);

/// @name Core logging functions
/// @{

/// Log a message at the given @p level.
TANH_API void log(LogLevel level, const char* group, const char* message);

/// Log a message with an explicit @p source tag.
TANH_API void log_with_source(LogLevel level,
                              const char* source,
                              const char* group,
                              const char* message);

/// printf-style logging at the given @p level.
TANH_API void logf(LogLevel level, const char* group, const char* fmt, ...);

/// @}

/// @name Convenience shorthands
/// @{
TANH_API void error(const char* group, const char* message);
TANH_API void warning(const char* group, const char* message);
TANH_API void info(const char* group, const char* message);
TANH_API void debug(const char* group, const char* message);
/// @}

}  // namespace thl::Logger
