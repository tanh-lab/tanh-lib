// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "tanh/core/Exports.h"
#include "tanh/core/RtFormat.h"
#include "tanh/core/threading/Thread.h"
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

/// @name Record flags
/// Bits of LogRecord::m_flags. They travel unchanged from the emitting site to
/// the sinks; the console, file and platform sinks ignore them, the callback
/// sink sees them on the record. Bits not listed here are free for consumers.
/// @{

/// The record came through a real-time queue: rt::Queue::drain() sets it on
/// every record it dispatches (its source is "rt" as well).
inline constexpr std::uint32_t k_flag_realtime = 1U;
/// The emitting site reports a misuse of its API by the caller. Never set by
/// the library on its own; passed by the caller of a flag-taking overload.
inline constexpr std::uint32_t k_flag_contract_violation = 2U;

/// @}

/// A single log entry produced by the logger.
///
/// Used in-process only (no ABI promise across library versions). New members
/// are appended at the end, with a default, so that existing brace
/// initialisers and positional reads keep their meaning.
struct LogRecord {
    std::uint64_t m_seq = 0;           ///< Monotonic sequence number.
    std::int64_t m_timestamp_ms = 0;   ///< Wall-clock UTC epoch (ms).
    std::uint64_t m_monotonic_ns = 0;  ///< Steady-clock epoch (ns).
    std::uint32_t m_level = static_cast<std::uint32_t>(LogLevel::Info);  ///< Severity level.
    std::string m_group;                                                 ///< Logical group tag.
    std::string m_message;      ///< Formatted message body.
    std::string m_source;       ///< Origin identifier (e.g. "native").
    std::uint32_t m_flags = 0;  ///< Bit set of k_flag_* values (see "Record flags").

    /// Real-time records lost to a full queue that are reported on this
    /// record: the count a rt::Queue::drain() pass took from its drop counter
    /// rides on the first record the pass dispatches (see Queue::drain()). Zero
    /// on every other record. Summing it over all records received counts
    /// every drop exactly once.
    std::uint64_t m_dropped_before = 0;
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

    /// Start the process-wide real-time drain thread (see thl::Logger::rt)
    /// when this config is applied, and stop it when applied with false. The
    /// thread is never started implicitly: without set_config() or
    /// rt::start(), rt::logf() reports NoConsumer unless the host pumps
    /// rt::drain() itself (rt::enable_manual_drain()).
    bool m_rt_enabled = true;

    /// Interval at which the drain thread flushes real-time records into the
    /// sinks. Bounds both delivery latency and the burst the queue must hold
    /// between two drains (queue capacity / interval = sustainable rate).
    std::uint32_t m_rt_drain_interval_ms = 10;

    /// @name Platform sink identity
    /// What the platform sink files records under, so a consumer that embeds
    /// the logger can be found by its own name: `adb logcat -s <tag>` on
    /// Android, `log stream --predicate 'subsystem == "<subsystem>"'` or a
    /// Console.app filter on Apple platforms. Set them before the first record:
    /// the sink reads the identity when it files each record, so records
    /// logged earlier go out under the defaults. A later set_config() applies
    /// to every record dispatched after it returns (a dispatch already in
    /// flight may still use the previous identity); on Apple platforms each
    /// change creates a new os_log_t, which the system never releases, so the
    /// identity is configuration, not something to switch per record. An
    /// empty string selects the default. Ignored by the plain stdout/stderr
    /// platform sink (Linux without journald, Windows, Emscripten).
    /// @{
    std::string m_platform_tag = "thl";        ///< Android logcat tag; journald SYSLOG_IDENTIFIER.
    std::string m_platform_subsystem = "thl";  ///< os_log subsystem (macOS, iOS).
    std::string m_platform_category = "logger";  ///< os_log category (macOS, iOS).
    /// @}
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
/// @c [level][source][group] message, followed by
/// @c [N real-time log message(s) dropped before this record] when the record
/// carries a drop count (LogRecord::m_dropped_before).
TANH_API std::string format_plain(const LogRecord& record);

/// Format a log record as a
/// [logfmt](https://brandur.org/logfmt)-style string. A record that carries a
/// drop count gets a trailing @c dropped_before=N field.
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

/// log_with_source() with record flags (see "Record flags"); @p flags is
/// stored in LogRecord::m_flags as given.
TANH_API void log_with_source(LogLevel level,
                              std::uint32_t flags,
                              const char* source,
                              const char* group,
                              const char* message);

/// printf-style logging at the given @p level.
TANH_API void logf(LogLevel level, const char* group, const char* fmt, ...);

/// logf() with record flags (see "Record flags"); @p flags is stored in
/// LogRecord::m_flags as given.
TANH_API void logf(LogLevel level, std::uint32_t flags, const char* group, const char* fmt, ...);

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
/// A rt::Queue is a bounded lock-free queue of fixed-size records: `logf()` /
/// `log()` may be called from any number of real-time threads concurrently and
/// never allocate, lock, or make system calls — the message is formatted on the
/// caller's stack and pushed into a pre-allocated slot. Somebody must consume
/// the queue: a rt::DrainThread (a low-priority thl::core::Thread that pops it
/// every few milliseconds), or the owner calling `drain()` from its own loop.
/// Records reach the regular sinks with `source = "rt"`.
///
/// Two ways to use it:
/// - **Own a queue.** A library or host constructs a rt::Queue (capacity of its
///   choosing) and, if it wants a thread, a rt::DrainThread over it; both live
///   exactly as long as their owner decides, which is what a plugin that may be
///   unloaded at any time needs. Nothing is shared with anyone else.
/// - **The process-wide default.** The free functions below operate on one
///   default queue and one optional drain thread, started by rt::start() or
///   set_config(m_rt_enabled = true) and stopped by rt::stop() /
///   set_config(m_rt_enabled = false). Never started implicitly.
///
/// Guarantees and limits:
/// - If the queue is full the message is dropped and counted; the next drain
///   reports the count on the first record it delivers
///   (LogRecord::m_dropped_before), or on one synthetic warning when it has
///   no record to deliver. Every record a drain dispatches carries
///   k_flag_realtime.
/// - Messages longer than `k_message_capacity - 1` are truncated.
/// - Sequence numbers are shared by every queue and the synchronous path, so
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

/// @brief A bounded lock-free queue of log records with real-time safe producers.
class TANH_API Queue {
public:
    /// @param capacity Number of records; rounded up to a power of two (min 2).
    ///        The slots are allocated here and never again.
    explicit Queue(std::size_t capacity = k_queue_capacity);
    ~Queue();
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    Queue(Queue&&) = delete;
    Queue& operator=(Queue&&) = delete;

    /// printf-style real-time logging. Formatting uses thl::core::rt_vsnprintf,
    /// a locale-free subset of printf (see RtFormat.h for the supported
    /// conversions; the compiler checks the format string where it can).
    // NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
    Status logf(LogLevel level,
                const char* group,
                const char* fmt,
                ...) noexcept TANH_NONBLOCKING_FUNCTION THL_PRINTF_FORMAT(4, 5);

    /// logf() with record flags (see "Record flags"): @p flags is copied into
    /// the record unchanged and reaches the sinks in LogRecord::m_flags.
    // NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
    Status logf(LogLevel level,
                std::uint32_t flags,
                const char* group,
                const char* fmt,
                ...) noexcept TANH_NONBLOCKING_FUNCTION THL_PRINTF_FORMAT(5, 6);

    /// va_list variant of logf().
    Status vlogf(LogLevel level,
                 const char* group,
                 const char* fmt,
                 va_list args) noexcept TANH_NONBLOCKING_FUNCTION;

    /// va_list variant of logf() with record flags.
    Status vlogf(LogLevel level,
                 std::uint32_t flags,
                 const char* group,
                 const char* fmt,
                 va_list args) noexcept TANH_NONBLOCKING_FUNCTION;

    /// Real-time logging of a preformatted message.
    Status log(LogLevel level,
               const char* group,
               const char* message) noexcept TANH_NONBLOCKING_FUNCTION;

    /// log() with record flags.
    Status log(LogLevel level,
               std::uint32_t flags,
               const char* group,
               const char* message) noexcept TANH_NONBLOCKING_FUNCTION;

    /// Forward every queued record to the sinks on the calling thread and
    /// report drops. Returns the number of records delivered. Not real-time safe.
    ///
    /// Every record dispatched here carries k_flag_realtime (in addition to
    /// its own flags) and source "rt". Drops are reported exactly once: the
    /// pass takes the drop counter first and puts it into
    /// LogRecord::m_dropped_before of the first record it dispatches; every
    /// other record of the pass carries 0. Only when the pass has no record
    /// to deliver does it dispatch one synthetic Warning record (group
    /// "thl.logger", message "real-time log queue overflowed") that carries
    /// the count instead. Drops that happen while the pass runs are reported
    /// by the next pass. The count says how many records were lost before
    /// this one was delivered, not where in the stream the gap lies.
    std::size_t drain();

    /// While false, logf()/log() return NoConsumer without formatting. A queue
    /// starts accepting; owners that have no consumer yet may switch it off.
    void set_accepting(bool accepting) noexcept;
    [[nodiscard]] bool is_accepting() const noexcept TANH_NONBLOCKING_FUNCTION;

    /// Records dropped because the queue was full, since the last drain().
    [[nodiscard]] std::uint64_t dropped_count() const noexcept TANH_NONBLOCKING_FUNCTION;

    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// @brief A thread that drains a Queue periodically.
///
/// Runs at thl::core::ThreadPriority::Low by default: below UI work, but not
/// starved like a background class would be, so delivery latency stays bounded
/// by the interval. The destructor stops the thread, joins it and drains what
/// arrived after its last pass; it must not run on the drain thread itself
/// (i.e. from inside a sink callback).
class TANH_API DrainThread {
public:
    struct Options {
        std::uint32_t m_interval_ms = 10;
        thl::core::ThreadPriority m_priority = thl::core::ThreadPriority::Low;
        const char* m_name = "thl-log-drain";
    };

    /// Starts the thread. `queue` must outlive this object.
    explicit DrainThread(Queue& queue);
    DrainThread(Queue& queue, const Options& options);
    ~DrainThread() noexcept;
    DrainThread(const DrainThread&) = delete;
    DrainThread& operator=(const DrainThread&) = delete;
    DrainThread(DrainThread&&) = delete;
    DrainThread& operator=(DrainThread&&) = delete;

    [[nodiscard]] bool is_running() const noexcept TANH_NONBLOCKING_FUNCTION;

    /// True when called from the drain thread itself.
    [[nodiscard]] bool is_current_thread() const noexcept;

    /// Change the drain interval of the running thread.
    void set_interval_ms(std::uint32_t interval_ms) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// @name Process-wide default queue
/// The free functions operate on one default Queue (capacity
/// THL_LOG_RT_QUEUE_CAPACITY) and, between start() and stop(), one DrainThread.
/// @{

/// printf-style real-time logging into the default queue (see Queue::logf).
// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
TANH_API Status logf(LogLevel level,
                     const char* group,
                     const char* fmt,
                     ...) noexcept TANH_NONBLOCKING_FUNCTION THL_PRINTF_FORMAT(3, 4);

/// logf() into the default queue with record flags (see "Record flags").
// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
TANH_API Status logf(LogLevel level,
                     std::uint32_t flags,
                     const char* group,
                     const char* fmt,
                     ...) noexcept TANH_NONBLOCKING_FUNCTION THL_PRINTF_FORMAT(4, 5);

/// Real-time logging of a preformatted message into the default queue.
TANH_API Status log(LogLevel level,
                    const char* group,
                    const char* message) noexcept TANH_NONBLOCKING_FUNCTION;

/// log() into the default queue with record flags.
TANH_API Status log(LogLevel level,
                    std::uint32_t flags,
                    const char* group,
                    const char* message) noexcept TANH_NONBLOCKING_FUNCTION;

/// Start the default drain thread if it is not running. Not real-time safe.
/// A no-op once the logger's static state has been torn down (process exit,
/// library unload), so it may be called from a client's own static destructor.
TANH_API void start();

/// Stop and join the default drain thread, flushing what is queued. Not
/// real-time safe. Leaves LoggerConfig::m_rt_enabled untouched. After this,
/// logf() returns NoConsumer until start() is called again or the host pumps
/// drain() itself. A no-op once the logger's static state has been torn down
/// (its destructor stopped the thread already), so it may be called from a
/// client's own static destructor or unload hook.
TANH_API void stop();

/// True while the default drain thread is running.
TANH_API bool is_running() noexcept TANH_NONBLOCKING_FUNCTION;

/// Drain the default queue on the calling thread (see Queue::drain). Hosts
/// that want to own every thread call stop() and pump this from their own
/// message loop; the queue accepts records whenever either the drain thread
/// runs or `enable_manual_drain()` was set.
TANH_API std::size_t drain();

/// Declare that the host will call drain() itself. Makes logf() enqueue even
/// while the drain thread is stopped. Pass false to revert.
TANH_API void enable_manual_drain(bool enabled);

/// Number of messages dropped from the default queue because it was full,
/// since the counter was last reported by drain(). Safe from any thread.
TANH_API std::uint64_t dropped_count() noexcept TANH_NONBLOCKING_FUNCTION;

/// @}

}  // namespace rt

}  // namespace thl::Logger

/// @name Convenience macros
/// @brief printf-style shorthands over thl::Logger::logf (synchronous) and
///        thl::Logger::rt::logf (real-time safe): `THL_LOG_ERROR(group, fmt, ...)`,
///        `THL_LOG_RT_WARNING(group, fmt, ...)`. Define THL_LOGGING_DISABLED to
///        compile every use out (arguments are not evaluated).
/// @{
#ifdef THL_LOGGING_DISABLED
#define THL_LOG_IMPL(level, group, ...) static_cast<void>(0)
#define THL_LOG_RT_IMPL(level, group, ...) static_cast<void>(0)
#else
// Gated on is_enabled() so a filtered call neither evaluates its arguments nor formats.
#define THL_LOG_IMPL(level, group, ...)                                              \
    do {                                                                             \
        if (::thl::Logger::is_enabled(::thl::Logger::LogLevel::level)) {             \
            ::thl::Logger::logf(::thl::Logger::LogLevel::level, group, __VA_ARGS__); \
        }                                                                            \
    } while (false)
#define THL_LOG_RT_IMPL(level, group, ...) \
    static_cast<void>(::thl::Logger::rt::logf(::thl::Logger::LogLevel::level, group, __VA_ARGS__))
#endif

#define THL_LOG_DEBUG(group, ...) THL_LOG_IMPL(Debug, group, __VA_ARGS__)
#define THL_LOG_INFO(group, ...) THL_LOG_IMPL(Info, group, __VA_ARGS__)
#define THL_LOG_WARNING(group, ...) THL_LOG_IMPL(Warning, group, __VA_ARGS__)
#define THL_LOG_ERROR(group, ...) THL_LOG_IMPL(Error, group, __VA_ARGS__)

#define THL_LOG_RT_DEBUG(group, ...) THL_LOG_RT_IMPL(Debug, group, __VA_ARGS__)
#define THL_LOG_RT_INFO(group, ...) THL_LOG_RT_IMPL(Info, group, __VA_ARGS__)
#define THL_LOG_RT_WARNING(group, ...) THL_LOG_RT_IMPL(Warning, group, __VA_ARGS__)
#define THL_LOG_RT_ERROR(group, ...) THL_LOG_RT_IMPL(Error, group, __VA_ARGS__)
/// @}
