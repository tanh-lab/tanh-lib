#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace thl::Logger {

enum class LogLevel : std::uint32_t {
    Error = 1,
    Warning = 2,
    Info = 3,
    Debug = 4,
};

struct LogRecord {
    std::uint64_t seq = 0;
    std::int64_t timestampMs = 0;
    std::uint64_t monotonicNs = 0;
    std::uint32_t level = static_cast<std::uint32_t>(LogLevel::Info);
    std::string group;
    std::string message;
    std::string source;
};

using Callback = std::function<void(const LogRecord&)>;

// Registers a synchronous log callback.
//
// Important constraints:
// - Logging is not real-time safe. Do not call Logger from the audio thread.
//   logf() performs formatting and logging may take locks and block in sinks.
// - The callback runs on the caller thread. Slow sink work will block the
//   thread that emitted the log.
// - Callback implementations should avoid logging back into thl::Logger
//   (re-entrant logging). Nested callback logging is guarded and may be
//   redirected to the default fallback sink.
// - If the callback references plugin/host-owned objects, call clear_callback()
//   before tearing those objects down (for example in plugin unload/teardown).
void set_callback(Callback cb);
void clear_callback();

std::string format_logfmt(const LogRecord& record);

void log(LogLevel level, const char* group, const char* message);
void log_with_source(LogLevel level,
                     const char* source,
                     const char* group,
                     const char* message);
void logf(LogLevel level, const char* group, const char* fmt, ...);

void error(const char* group, const char* message);
void warning(const char* group, const char* message);
void info(const char* group, const char* message);
void debug(const char* group, const char* message);

}  // namespace thl::Logger
