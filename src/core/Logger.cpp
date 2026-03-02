#include <tanh/core/Logger.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#if defined(__ANDROID__)
#include <android/log.h>
#elif defined(__APPLE__)
#include <os/log.h>
#endif

namespace thl::Logger {

namespace {

#ifndef THL_LOG_COMPILED_MAX_LEVEL
#define THL_LOG_COMPILED_MAX_LEVEL 4
#endif

std::uint32_t clamp_level(std::uint32_t level) {
    switch (level) {
        case static_cast<std::uint32_t>(LogLevel::Error):
        case static_cast<std::uint32_t>(LogLevel::Warning):
        case static_cast<std::uint32_t>(LogLevel::Info):
        case static_cast<std::uint32_t>(LogLevel::Debug): return level;
        default: return static_cast<std::uint32_t>(LogLevel::Info);
    }
}

bool should_log_compiled(std::uint32_t level) {
    return clamp_level(level) <= static_cast<std::uint32_t>(THL_LOG_COMPILED_MAX_LEVEL);
}

FILE* stream_for_level(std::uint32_t level) {
    switch (clamp_level(level)) {
        case static_cast<std::uint32_t>(LogLevel::Error):
        case static_cast<std::uint32_t>(LogLevel::Warning):
            return stderr;
        default:
            return stdout;
    }
}

std::atomic<bool>& shutdown_started_flag() {
    static std::atomic<bool> flag {false};
    return flag;
}

void mark_logging_shutdown() {
    shutdown_started_flag().store(true, std::memory_order_relaxed);
}

bool logging_shutdown_started() {
    return shutdown_started_flag().load(std::memory_order_relaxed);
}

void ensure_shutdown_hook_installed() {
    static const bool registered = []() {
        shutdown_started_flag();
        std::atexit(mark_logging_shutdown);
        return true;
    }();
    (void)registered;
}


const char* level_name(std::uint32_t level) {
    switch (clamp_level(level)) {
        case static_cast<std::uint32_t>(LogLevel::Error): return "error";
        case static_cast<std::uint32_t>(LogLevel::Warning): return "warn";
        case static_cast<std::uint32_t>(LogLevel::Info): return "info";
        case static_cast<std::uint32_t>(LogLevel::Debug): return "debug";
        default: return "info";
    }
}

void write_to_stderr_fallback(std::uint32_t level,
                              const char* source,
                              const char* group,
                              const char* message) noexcept {
    std::fprintf(stderr,
                 "[%s][%s][%s] %s\n",
                 level_name(level),
                 source ? source : "native",
                 group ? group : "default",
                 message ? message : "");
}

std::string escape_logfmt_value(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

bool needs_logfmt_quotes(const std::string& value) {
    if (value.empty()) return true;
    for (const char c : value) {
        if (c == ' ' || c == '=' || c == '"' || c == '\t') return true;
    }
    return false;
}

void append_logfmt_field(std::ostringstream& out,
                         const char* key,
                         const std::string& rawValue) {
    const std::string value = escape_logfmt_value(rawValue);
    out << key << '=';
    if (!needs_logfmt_quotes(value)) {
        out << value;
        return;
    }

    out << '"';
    out << value;
    out << '"';
}

std::string format_iso8601_utc_ms(std::int64_t timestamp_ms) {
    std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    int millis = static_cast<int>(timestamp_ms % 1000);
    if (millis < 0) {
        millis += 1000;
        --seconds;
    }

    std::tm tm_value {};
#if defined(_WIN32)
    gmtime_s(&tm_value, &seconds);
#else
    gmtime_r(&seconds, &tm_value);
#endif

    std::ostringstream out;
    out << std::put_time(&tm_value, "%Y-%m-%dT%H:%M:%S")
        << '.'
        << std::setw(3) << std::setfill('0') << millis
        << 'Z';
    return out.str();
}

void write_to_default_sink(const LogRecord& record) {
    try {
        const std::string line = format_logfmt(record);
        FILE* out = stream_for_level(record.level);
        std::fprintf(out, "%s\n", line.c_str());
        std::fflush(out);
    } catch (...) {
        write_to_stderr_fallback(
            record.level,
            record.source.empty() ? "native" : record.source.c_str(),
            record.group.empty() ? "default" : record.group.c_str(),
            record.message.c_str());
    }
}

// ---------------------------------------------------------------------------
// Platform sink
// ---------------------------------------------------------------------------

#if defined(__APPLE__)
os_log_t platform_log_handle() {
    static os_log_t handle = os_log_create("thl", "logger");
    return handle;
}
#endif

bool emit_platform(const LogRecord& record) {
    const char* group =
        record.group.empty() ? "default" : record.group.c_str();
    const char* message = record.message.c_str();

#if defined(__ANDROID__)
    int android_level = ANDROID_LOG_INFO;
    switch (clamp_level(record.level)) {
        case static_cast<std::uint32_t>(LogLevel::Error):
            android_level = ANDROID_LOG_ERROR; break;
        case static_cast<std::uint32_t>(LogLevel::Warning):
            android_level = ANDROID_LOG_WARN; break;
        case static_cast<std::uint32_t>(LogLevel::Info):
            android_level = ANDROID_LOG_INFO; break;
        case static_cast<std::uint32_t>(LogLevel::Debug):
            android_level = ANDROID_LOG_DEBUG; break;
        default: android_level = ANDROID_LOG_INFO; break;
    }
    __android_log_print(android_level, "thl", "[%s] %s", group, message);
    return true;

#elif defined(__APPLE__)
    os_log_type_t type = OS_LOG_TYPE_INFO;
    switch (clamp_level(record.level)) {
        case static_cast<std::uint32_t>(LogLevel::Error):
            type = OS_LOG_TYPE_ERROR; break;
        case static_cast<std::uint32_t>(LogLevel::Warning):
            type = OS_LOG_TYPE_DEFAULT; break;
        case static_cast<std::uint32_t>(LogLevel::Info):
            type = OS_LOG_TYPE_INFO; break;
        case static_cast<std::uint32_t>(LogLevel::Debug):
            type = OS_LOG_TYPE_DEBUG; break;
        default: type = OS_LOG_TYPE_INFO; break;
    }
    os_log_with_type(platform_log_handle(),
                     type,
                     "[%{public}s] %{public}s",
                     group,
                     message);
    return true;

#else
    write_to_default_sink(record);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct LoggerState {
    std::atomic<std::uint64_t> next_seq {1};

    // Protects config booleans + callback.
    std::mutex config_mutex;
    bool platform_enabled = true;
    bool file_enabled = false;
    bool callback_enabled = true;
    Callback callback;

    // Protects file_path + file_stream.  Lock ordering: config_mutex
    // before file_mutex.
    std::mutex file_mutex;
    std::string file_path;
    std::ofstream file_stream;
};

LoggerState& state() {
    ensure_shutdown_hook_installed();
    static LoggerState instance;
    return instance;
}

// ---------------------------------------------------------------------------
// File sink
// ---------------------------------------------------------------------------

bool emit_file(const LogRecord& record) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.file_mutex);

    if (s.file_path.empty()) { return false; }

    if (!s.file_stream.is_open()) {
        s.file_stream.open(
            s.file_path, std::ios::out | std::ios::app);
        if (!s.file_stream.is_open()) { return false; }
    }

    s.file_stream << format_logfmt(record) << '\n';
    s.file_stream.flush();
    return true;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

int& callback_dispatch_depth() {
    static thread_local int depth = 0;
    return depth;
}

class CallbackDispatchScope {
public:
    CallbackDispatchScope() { ++callback_dispatch_depth(); }
    ~CallbackDispatchScope() { --callback_dispatch_depth(); }
};

LogRecord make_record(std::uint32_t level,
                      const char* source,
                      const char* group,
                      const char* message) {
    LogRecord record;
    record.seq =
        state().next_seq.fetch_add(1, std::memory_order_relaxed);
    const auto wall_now = std::chrono::system_clock::now();
    const auto mono_now = std::chrono::steady_clock::now();
    record.timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            wall_now.time_since_epoch())
            .count();
    record.monotonic_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            mono_now.time_since_epoch())
            .count();
    record.level = clamp_level(level);
    record.source = source ? source : "native";
    record.group = group ? group : "default";
    record.message = message ? message : "";
    return record;
}

void dispatch_record(const LogRecord& record) {
    if (logging_shutdown_started()) {
        write_to_stderr_fallback(record.level,
                                 record.source.c_str(),
                                 record.group.c_str(),
                                 record.message.c_str());
        return;
    }

    // Re-entrant logging from inside the callback is redirected to the
    // default fallback sink to avoid recursive callback dispatch and
    // duplicated multi-sink emission.
    if (callback_dispatch_depth() > 0) {
        write_to_default_sink(record);
        return;
    }

    // Snapshot config booleans + callback under config_mutex.
    bool platform_on = false;
    bool file_on = false;
    bool callback_on = false;
    Callback callback_copy;
    {
        std::lock_guard<std::mutex> lock(state().config_mutex);
        platform_on = state().platform_enabled;
        file_on = state().file_enabled;
        callback_on = state().callback_enabled;
        callback_copy = state().callback;
    }

    bool any_sink_ran = false;

    // 1. Platform sink (lock-free, fire-and-forget).
    if (platform_on) {
        if (emit_platform(record)) { any_sink_ran = true; }
    }

    // 2. File sink (acquires file_mutex internally).
    if (file_on) {
        if (emit_file(record)) { any_sink_ran = true; }
    }

    // 3. Callback sink (gated by callback_enabled + re-entrancy guard).
    if (callback_copy && callback_on) {
        try {
            CallbackDispatchScope scope;
            callback_copy(record);
            any_sink_ran = true;
        } catch (...) {
            write_to_stderr_fallback(
                record.level,
                record.source.c_str(),
                record.group.c_str(),
                record.message.c_str());
            any_sink_ran = true;
        }
    }

    // 4. Last-resort fallback if every sink was disabled or failed.
    if (!any_sink_ran) {
        write_to_default_sink(record);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API -- config
// ---------------------------------------------------------------------------

void set_config(const LoggerConfig& config) {
    auto& s = state();

    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        s.platform_enabled = config.platform_enabled;
        s.file_enabled = config.file_enabled;
        s.callback_enabled = config.callback_enabled;
    }

    {
        std::lock_guard<std::mutex> lock(s.file_mutex);
        const bool path_changed = (s.file_path != config.file_path);
        if (path_changed || !config.file_enabled) {
            if (s.file_stream.is_open()) { s.file_stream.close(); }
        }
        s.file_path = config.file_path;
    }
}

LoggerConfig get_config() {
    auto& s = state();
    LoggerConfig config;
    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        config.platform_enabled = s.platform_enabled;
        config.file_enabled = s.file_enabled;
        config.callback_enabled = s.callback_enabled;
    }
    {
        std::lock_guard<std::mutex> lock(s.file_mutex);
        config.file_path = s.file_path;
    }
    return config;
}

// ---------------------------------------------------------------------------
// Public API -- callback
// ---------------------------------------------------------------------------

void set_callback(Callback cb) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.config_mutex);
    s.callback = std::move(cb);
}

void clear_callback() {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.config_mutex);
    s.callback = nullptr;
}

// ---------------------------------------------------------------------------
// Public API -- formatting
// ---------------------------------------------------------------------------

std::string format_logfmt(const LogRecord& record) {
    std::ostringstream out;
    append_logfmt_field(
        out, "time", format_iso8601_utc_ms(record.timestamp_ms));
    out << ' ';
    out << "level=" << level_name(record.level)
        << " seq=" << record.seq
        << " ts_ms=" << record.timestamp_ms
        << " mono_ns=" << record.monotonic_ns
        << ' ';
    append_logfmt_field(
        out, "source",
        record.source.empty() ? "native" : record.source);
    out << ' ';
    append_logfmt_field(
        out, "group",
        record.group.empty() ? "default" : record.group);
    out << ' ';
    append_logfmt_field(out, "message", record.message);
    return out.str();
}

// ---------------------------------------------------------------------------
// Public API -- logging
// ---------------------------------------------------------------------------

void log(LogLevel level, const char* group, const char* message) {
    log_with_source(level, "native", group, message);
}

void log_with_source(LogLevel level,
                     const char* source,
                     const char* group,
                     const char* message) {
    const auto numeric_level = static_cast<std::uint32_t>(level);
    if (!should_log_compiled(numeric_level)) { return; }

    ensure_shutdown_hook_installed();
    if (logging_shutdown_started()) {
        write_to_stderr_fallback(numeric_level, source, group, message);
        return;
    }

    try {
        dispatch_record(
            make_record(numeric_level, source, group, message));
    } catch (...) {
        write_to_stderr_fallback(
            numeric_level, source, group, message);
    }
}

void logf(LogLevel level, const char* group, const char* fmt, ...) {
    if (!should_log_compiled(static_cast<std::uint32_t>(level))) {
        return;
    }

    if (!fmt) {
        log(level, group, "");
        return;
    }

    std::array<char, 1024> stack_buffer {};
    va_list args;
    va_start(args, fmt);
    const int written =
        std::vsnprintf(
            stack_buffer.data(), stack_buffer.size(), fmt, args);
    va_end(args);

    if (written < 0) {
        log(level, group, "logf formatting failed");
        return;
    }

    log(level, group, stack_buffer.data());
}

void error(const char* group, const char* message) {
    log(LogLevel::Error, group, message);
}

void warning(const char* group, const char* message) {
    log(LogLevel::Warning, group, message);
}

void info(const char* group, const char* message) {
    log(LogLevel::Info, group, message);
}

void debug(const char* group, const char* message) {
    log(LogLevel::Debug, group, message);
}

}  // namespace thl::Logger
