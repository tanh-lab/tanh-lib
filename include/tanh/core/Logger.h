// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "tanh/core/Exports.h"
#include "tanh/utils/RealtimeSanitizer.h"

/// @namespace thl::Logger
/// @brief Unified logging facade with compile-time filtering, platform sinks,
///        file output, and optional callback forwarding.
///
/// The functions in this namespace are synchronous: sinks run on the
/// caller's thread. They are **not** real-time safe -- do not call them from
/// the audio thread. Real-time code uses the lock-free path in
/// thl::Logger::rt instead (see below); its messages are delivered through
/// the same sinks by a background drain thread.
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

    /// Start the real-time drain thread (see thl::Logger::rt) when this
    /// config is applied. When false, rt::logf() reports NoConsumer until
    /// rt::start() is called explicitly or the host pumps rt::drain().
    bool m_rt_enabled = true;

    /// Interval at which the drain thread flushes real-time records into the
    /// sinks. Bounds both delivery latency and the burst the queue must hold
    /// between two drains (queue capacity / interval = sustainable rate).
    std::uint32_t m_rt_drain_interval_ms = 10;
};

/// @name Runtime level filter
/// @{

/// Set the process-wide minimum severity. Records less severe than
/// @p level are dropped by both the synchronous and the real-time path.
/// Default: Debug (everything the compile-time filter lets through).
/// Thread-safe; takes effect for subsequent calls.
TANH_API void set_level(LogLevel level);

/// Current runtime minimum severity.
TANH_API LogLevel get_level() TANH_NONBLOCKING_FUNCTION;

/// True if a message at @p level would pass both the compile-time and the
/// runtime filter. Cheap; safe to call from real-time code.
TANH_API bool is_enabled(LogLevel level) TANH_NONBLOCKING_FUNCTION;

/// @}

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
TANH_API void set_callback(const Callback& cb);

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

/// @namespace thl::Logger::rt
/// @brief Real-time safe logging.
///
/// `logf()` / `log()` may be called from any number of real-time threads
/// concurrently. They never allocate, lock, or make system calls: the message
/// is formatted into a fixed-size record on the caller's stack and pushed
/// into a bounded lock-free queue. A background drain thread pops the queue
/// every `LoggerConfig::m_rt_drain_interval_ms` and forwards each record to
/// the regular sinks with `source = "rt"`, so real-time messages show up in
/// the platform log, the file sink and the callback like any other record,
/// a few milliseconds late.
///
/// Guarantees and limits:
/// - If the queue is full the message is dropped and counted; the drain
///   thread emits a single "N messages dropped" warning afterwards.
/// - Messages longer than `k_message_capacity - 1` are truncated.
/// - If no consumer is running (drain thread stopped and nobody calls
///   drain()), `logf()` returns NoConsumer immediately without formatting.
///   The drain thread starts on the first non-real-time logger call
///   (set_config(), set_callback(), log(), ...) or via start().
/// - Sequence numbers are shared with the synchronous path, so RT and non-RT
///   records interleave in causal order in the file sink.
namespace rt {

/// Result of a real-time log call.
enum class Status : std::uint8_t {
    Ok = 0,          ///< Enqueued.
    Truncated = 1,   ///< Enqueued, but the message was cut to fit.
    QueueFull = 2,   ///< Dropped: no free slot. Counted and reported later.
    NoConsumer = 3,  ///< Dropped: drain thread not running.
    Filtered = 4,    ///< Dropped by the compile-time or runtime level filter.
};

#ifndef THL_LOG_RT_MESSAGE_CAPACITY
#define THL_LOG_RT_MESSAGE_CAPACITY 256
#endif
#ifndef THL_LOG_RT_GROUP_CAPACITY
#define THL_LOG_RT_GROUP_CAPACITY 32
#endif
#ifndef THL_LOG_RT_QUEUE_CAPACITY
#define THL_LOG_RT_QUEUE_CAPACITY 512
#endif

/// Bytes reserved per message, including the terminating NUL.
inline constexpr std::size_t k_message_capacity = THL_LOG_RT_MESSAGE_CAPACITY;
/// Bytes reserved per group tag, including the terminating NUL.
inline constexpr std::size_t k_group_capacity = THL_LOG_RT_GROUP_CAPACITY;
/// Number of records the queue can hold. Must be a power of two.
inline constexpr std::size_t k_queue_capacity = THL_LOG_RT_QUEUE_CAPACITY;
static_assert((k_queue_capacity & (k_queue_capacity - 1)) == 0,
              "THL_LOG_RT_QUEUE_CAPACITY must be a power of two");

/// printf-style real-time logging. Formatting uses thl::core::rt_vsnprintf,
/// a locale-free subset of printf (see RtFormat.h).
// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
TANH_API Status logf(LogLevel level,
                     const char* group,
                     const char* fmt,
                     ...) noexcept TANH_NONBLOCKING_FUNCTION;

/// Real-time logging of a preformatted message.
TANH_API Status log(LogLevel level,
                    const char* group,
                    const char* message) noexcept TANH_NONBLOCKING_FUNCTION;

/// Start the drain thread if it is not running. Not real-time safe.
TANH_API void start();

/// Stop and join the drain thread, flushing what is queued. Not real-time
/// safe. After this, logf() returns NoConsumer until start() is called
/// again or the host pumps drain() itself.
TANH_API void stop();

/// True while the drain thread is running.
TANH_API bool is_running() noexcept TANH_NONBLOCKING_FUNCTION;

/// Forward all queued records to the sinks on the calling thread. Returns
/// the number of records delivered. Hosts that want to own every thread can
/// call stop() and pump this from their own message loop; the queue accepts
/// records whenever either the drain thread runs or `enable_manual_drain()`
/// was set.
TANH_API std::size_t drain();

/// Declare that the host will call drain() itself. Makes logf() enqueue even
/// while the drain thread is stopped. Pass false to revert.
TANH_API void enable_manual_drain(bool enabled);

/// Number of messages dropped because the queue was full since the counter
/// was last reported by drain(). Diagnostic; safe from any thread.
TANH_API std::uint64_t dropped_count() noexcept TANH_NONBLOCKING_FUNCTION;

}  // namespace rt

}  // namespace thl::Logger
