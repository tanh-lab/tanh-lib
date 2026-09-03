// SPDX-License-Identifier: Apache-2.0
#include <tanh/core/Logger.h>
#include <tanh/core/RtFormat.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/core/threading/Thread.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <ios>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(THL_PLATFORM_ANDROID)
#include <android/log.h>
#elif defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
#include <os/log.h>
#include <sys/sysctl.h>
#include <unistd.h>
#elif defined(THL_PLATFORM_LINUX) && defined(THL_WITH_JOURNALD)
#include <sys/syslog.h>
#include <systemd/sd-journal.h>
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

// ---------------------------------------------------------------------------
// Process-lifetime state touched by the real-time path.
//
// Everything rt::logf() reads or writes lives here as constant-initialised
// namespace-scope objects: they are never destroyed, so a real-time thread
// that logs during static teardown finds the consumer gone (g_rt_running ==
// false) instead of a dangling LoggerState.
// ---------------------------------------------------------------------------

std::atomic<std::uint32_t> g_runtime_level{static_cast<std::uint32_t>(LogLevel::Debug)};
std::atomic<std::uint64_t> g_next_seq{1};
// Default-queue consumer state, constant-initialised so the real-time producers
// can read it before the default queue exists (it is constructed on the first
// non-real-time call: start(), enable_manual_drain(), drain()).
std::atomic<bool> g_rt_running{false};
std::atomic<bool> g_rt_manual_drain{false};

bool passes_runtime_level(std::uint32_t level) TANH_NONBLOCKING_FUNCTION {
    return clamp_level(level) <= g_runtime_level.load(std::memory_order_relaxed);
}

struct RtRecord {
    std::uint64_t m_seq = 0;
    std::int64_t m_timestamp_ms = 0;
    std::uint64_t m_monotonic_ns = 0;
    std::uint32_t m_level = 0;
    std::uint32_t m_flags = 0;  // LogRecord::m_flags as passed by the producer
    std::array<char, rt::k_group_capacity> m_group{};
    std::array<char, rt::k_message_capacity> m_message{};
};

/// Bounded copy of a C string into a fixed array (always NUL-terminated).
template <std::size_t N>
void copy_bounded(std::array<char, N>& dst, const char* src) noexcept TANH_NONBLOCKING_FUNCTION {
    std::size_t i = 0;
    if (src != nullptr) {
        for (; i + 1 < N && src[i] != '\0'; ++i) { dst[i] = src[i]; }
    }
    dst[i] = '\0';
}

FILE* stream_for_level(std::uint32_t level) {
    switch (clamp_level(level)) {
        case static_cast<std::uint32_t>(LogLevel::Error):
        case static_cast<std::uint32_t>(LogLevel::Warning): return stderr;
        default: return stdout;
    }
}

std::atomic<bool>& shutdown_started_flag() {
    static std::atomic<bool> flag{false};
    return flag;
}

void mark_logging_shutdown() {
    shutdown_started_flag().store(true, std::memory_order_relaxed);
}

bool logging_shutdown_started() {
    return shutdown_started_flag().load(std::memory_order_relaxed);
}

void ensure_shutdown_hook_installed() {
    static const bool k_registered = []() {
        shutdown_started_flag();
        std::atexit(mark_logging_shutdown);
        return true;
    }();
    (void)k_registered;
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

std::string escape_logfmt_value(const std::string& value) {
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
    if (value.empty()) { return true; }
    for (const char c : value) {
        if (c == ' ' || c == '=' || c == '"' || c == '\t') { return true; }
    }
    return false;
}

void append_logfmt_field(std::ostringstream& out, const char* key, const std::string& raw_value) {
    const std::string value = escape_logfmt_value(raw_value);
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
    auto seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    int millis = static_cast<int>(timestamp_ms % 1000);
    if (millis < 0) {
        millis += 1000;
        --seconds;
    }

    std::tm tm_value{};
#if defined(THL_PLATFORM_WINDOWS)
    gmtime_s(&tm_value, &seconds);
#else
    gmtime_r(&seconds, &tm_value);
#endif

    std::ostringstream out;
    out << std::put_time(&tm_value, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
        << millis << 'Z';
    return out.str();
}

/// Suffix the human-readable sinks append to a record that carries a drop
/// count (LogRecord::m_dropped_before, set by rt::Queue::drain()).
std::string drop_note(const LogRecord& record) {
    return " [" + std::to_string(record.m_dropped_before) +
           " real-time log message(s) dropped before this record]";
}

void write_to_default_sink(const LogRecord& record) {
    try {
        const std::string line = format_plain(record);
        FILE* out = stream_for_level(record.m_level);
        std::fprintf(out, "%s\n", line.c_str());
        std::fflush(out);
    } catch (...) {
        write_to_stderr_fallback(record.m_level,
                                 record.m_source.empty() ? "native" : record.m_source.c_str(),
                                 record.m_group.empty() ? "default" : record.m_group.c_str(),
                                 record.m_message.c_str());
    }
}

// ---------------------------------------------------------------------------
// Platform sink
// ---------------------------------------------------------------------------

/// What the platform sink files a record under (LoggerConfig::m_platform_*).
/// dispatch_record() snapshots it under the config mutex together with the
/// sink switches, so a set_config() racing a dispatch cannot tear it.
struct PlatformIdentity {
#if defined(THL_PLATFORM_ANDROID) || (defined(THL_PLATFORM_LINUX) && defined(THL_WITH_JOURNALD))
    std::string m_tag;  // logcat tag / SYSLOG_IDENTIFIER
#elif defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
    os_log_t m_log = nullptr;  // os_log_create(subsystem, category)
#endif
};

#if defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
bool is_debugger_attached() {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info{};
    size_t size = sizeof(info);
    sysctl(mib, 4, &info, &size, nullptr, 0);
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}
#endif

// Platforms with a native sink; everything else uses the plain default sink.
#if defined(THL_PLATFORM_ANDROID) ||                                                              \
    (defined(THL_PLATFORM_LINUX) && defined(THL_WITH_JOURNALD)) || defined(THL_PLATFORM_MACOS) || \
    defined(THL_PLATFORM_IOS)
#define THL_HAS_NATIVE_LOG_SINK 1
#endif

bool emit_platform(const LogRecord& record, [[maybe_unused]] const PlatformIdentity& identity) {
#if defined(THL_HAS_NATIVE_LOG_SINK)
    const char* source = record.m_source.empty() ? "native" : record.m_source.c_str();
    const char* group = record.m_group.empty() ? "default" : record.m_group.c_str();
    const char* message = record.m_message.c_str();
    std::string annotated;
    if (record.m_dropped_before != 0) {
        annotated = record.m_message + drop_note(record);
        message = annotated.c_str();
    }
#endif

#if defined(THL_PLATFORM_ANDROID)
    int android_level = ANDROID_LOG_INFO;
    switch (clamp_level(record.m_level)) {
        case static_cast<std::uint32_t>(LogLevel::Error): android_level = ANDROID_LOG_ERROR; break;
        case static_cast<std::uint32_t>(LogLevel::Warning): android_level = ANDROID_LOG_WARN; break;
        case static_cast<std::uint32_t>(LogLevel::Info): android_level = ANDROID_LOG_INFO; break;
        case static_cast<std::uint32_t>(LogLevel::Debug): android_level = ANDROID_LOG_DEBUG; break;
        default: android_level = ANDROID_LOG_INFO; break;
    }
    __android_log_print(android_level,
                        identity.m_tag.c_str(),
                        "[%s][%s] %s",
                        source,
                        group,
                        message);
    return true;

#elif defined(THL_PLATFORM_LINUX) && defined(THL_WITH_JOURNALD)
    int priority = LOG_INFO;
    switch (clamp_level(record.m_level)) {
        case static_cast<std::uint32_t>(LogLevel::Error): priority = LOG_ERR; break;
        case static_cast<std::uint32_t>(LogLevel::Warning): priority = LOG_WARNING; break;
        case static_cast<std::uint32_t>(LogLevel::Info): priority = LOG_INFO; break;
        case static_cast<std::uint32_t>(LogLevel::Debug): priority = LOG_DEBUG; break;
        default: priority = LOG_INFO; break;
    }
    sd_journal_send("MESSAGE=[%s][%s] %s",
                    source,
                    group,
                    message,
                    "PRIORITY=%i",
                    priority,
                    "SYSLOG_IDENTIFIER=%s",
                    identity.m_tag.c_str(),
                    "THL_SOURCE=%s",
                    source,
                    "THL_GROUP=%s",
                    group,
                    NULL);

    // In the choc webkit on linux the console logs are automatically forwarded to the stdout/stderr
    // of the process
    return true;

#elif defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
    os_log_type_t type = OS_LOG_TYPE_INFO;
    switch (clamp_level(record.m_level)) {
        case static_cast<std::uint32_t>(LogLevel::Error): type = OS_LOG_TYPE_ERROR; break;
        case static_cast<std::uint32_t>(LogLevel::Warning): type = OS_LOG_TYPE_DEFAULT; break;
        case static_cast<std::uint32_t>(LogLevel::Info): type = OS_LOG_TYPE_INFO; break;
        case static_cast<std::uint32_t>(LogLevel::Debug): type = OS_LOG_TYPE_DEBUG; break;
        default: type = OS_LOG_TYPE_INFO; break;
    }
#if defined(THL_PLATFORM_MACOS)
    if (!is_debugger_attached()) {
        os_log_with_type(identity.m_log,
                         type,
                         "[%{public}s][%{public}s] %{public}s",
                         source,
                         group,
                         message);
    }
    write_to_default_sink(record);
#else
    os_log_with_type(identity.m_log,
                     type,
                     "[%{public}s][%{public}s] %{public}s",
                     source,
                     group,
                     message);
#endif
    return true;
#else
    // Linux without the journald opt-in, Emscripten, Windows and any other
    // platform: plain stdout/stderr, no platform library involved.
    write_to_default_sink(record);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void stop_drain_thread();

struct LoggerState {
    ~LoggerState() noexcept {
        // The atexit hook is registered before this object is constructed and
        // therefore runs *after* it is destroyed; flag the shutdown here so a
        // log call from a later static destructor takes the stderr fallback
        // instead of touching this object (or spawning a drain thread on it).
        mark_logging_shutdown();
        try {
            stop_drain_thread();
        } catch (...) {}  // NOLINT(bugprone-empty-catch) nothing sensible left to do at teardown
    }

    // Protects config booleans + callback + early buffers.
    std::mutex m_config_mutex;
    bool m_platform_enabled = true;
    bool m_console_enabled = false;
    bool m_file_enabled = true;
    bool m_callback_enabled = true;
    Callback m_callback;

    std::size_t m_early_buffer_capacity = 64;
    std::vector<LogRecord> m_early_callback_buffer;
    std::vector<LogRecord> m_early_file_buffer;

    // Platform sink identity (LoggerConfig::m_platform_*), protected by
    // m_config_mutex. Never empty: set_config() substitutes the defaults.
    std::string m_platform_tag = "thl";
    std::string m_platform_subsystem = "thl";
    std::string m_platform_category = "logger";
#if defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
    // The os_log_t for the identity above: created on first use by
    // platform_identity_locked(), reset to nullptr by set_config() when the
    // subsystem or category change so the next record creates a new one.
    os_log_t m_platform_log = nullptr;
#endif

    // Protects file_path + file_stream.  Lock ordering: config_mutex
    // before file_mutex.
    std::mutex m_file_mutex;
    std::string m_file_path;
    std::ofstream m_file_stream;

    // Default real-time drain thread. Protected by m_rt_mutex; g_rt_running is
    // the lock-free view producers read.
    std::mutex m_rt_mutex;
    std::unique_ptr<rt::DrainThread> m_rt_drain;
    std::atomic<bool> m_rt_enabled{true};
    std::atomic<std::uint32_t> m_rt_drain_interval_ms{10};
};

LoggerState& state() {
    ensure_shutdown_hook_installed();
    static LoggerState instance;
    return instance;
}

/// The identity the platform sink files records under, for the current
/// config. Requires m_config_mutex. On Apple platforms this creates the
/// os_log_t on first use and again after set_config() changed the subsystem
/// or category; os_log_t objects are never released, so a handle snapshotted
/// by a dispatch that is still in flight stays valid across the change.
PlatformIdentity platform_identity_locked([[maybe_unused]] LoggerState& s) {
    PlatformIdentity identity;
#if defined(THL_PLATFORM_ANDROID) || (defined(THL_PLATFORM_LINUX) && defined(THL_WITH_JOURNALD))
    identity.m_tag = s.m_platform_tag;
#elif defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
    if (s.m_platform_log == nullptr) {
        s.m_platform_log =
            os_log_create(s.m_platform_subsystem.c_str(), s.m_platform_category.c_str());
    }
    identity.m_log = s.m_platform_log;
#endif
    return identity;
}

// ---------------------------------------------------------------------------
// File sink
// ---------------------------------------------------------------------------

bool emit_file(const LogRecord& record) {
    auto& s = state();
    std::scoped_lock const lock(s.m_file_mutex);

    if (s.m_file_path.empty()) { return false; }

    if (!s.m_file_stream.is_open()) {
        s.m_file_stream.open(s.m_file_path, std::ios::out | std::ios::trunc);
        if (!s.m_file_stream.is_open()) { return false; }
    }

    s.m_file_stream << format_logfmt(record) << '\n';
    s.m_file_stream.flush();
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
                      std::uint32_t flags,
                      const char* source,
                      const char* group,
                      const char* message) {
    LogRecord record;
    record.m_seq = g_next_seq.fetch_add(1, std::memory_order_relaxed);
    const auto wall_now = std::chrono::system_clock::now();
    const auto mono_now = std::chrono::steady_clock::now();
    record.m_timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(wall_now.time_since_epoch()).count();
    record.m_monotonic_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(mono_now.time_since_epoch()).count();
    record.m_level = clamp_level(level);
    record.m_source = source ? source : "native";
    record.m_group = group ? group : "default";
    record.m_message = message ? message : "";
    record.m_flags = flags;
    return record;
}

void dispatch_record(const LogRecord& record) {
    if (logging_shutdown_started()) {
        write_to_stderr_fallback(record.m_level,
                                 record.m_source.c_str(),
                                 record.m_group.c_str(),
                                 record.m_message.c_str());
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
    bool console_on = false;
    bool file_on = false;
    bool callback_on = false;
    std::size_t early_cap = 0;
    Callback callback_copy;
    PlatformIdentity platform_identity;
    {
        std::scoped_lock const lock(state().m_config_mutex);
        platform_on = state().m_platform_enabled;
        console_on = state().m_console_enabled;
        file_on = state().m_file_enabled;
        callback_on = state().m_callback_enabled;
        early_cap = state().m_early_buffer_capacity;
        callback_copy = state().m_callback;
        if (platform_on) { platform_identity = platform_identity_locked(state()); }
    }

    bool any_sink_ran = false;

    // 1. Platform sink (lock-free, fire-and-forget).
    if (platform_on) {
        if (emit_platform(record, platform_identity)) { any_sink_ran = true; }
    }

    // 2. Console sink (explicit stdout/stderr output).
    if (console_on) {
        write_to_default_sink(record);
        any_sink_ran = true;
    }

    // 3. File sink (acquires file_mutex internally).
    if (file_on) {
        if (emit_file(record)) {
            any_sink_ran = true;
        } else if (early_cap > 0) {
            // File enabled but path not yet configured — buffer for later.
            std::scoped_lock const lock(state().m_config_mutex);
            if (state().m_early_file_buffer.size() < state().m_early_buffer_capacity) {
                state().m_early_file_buffer.push_back(record);
                any_sink_ran = true;
            }
        }
    }

    // 4. Callback sink (gated by callback_enabled + re-entrancy guard).
    if (callback_copy && callback_on) {
        try {
            CallbackDispatchScope const scope;
            callback_copy(record);
            any_sink_ran = true;
        } catch (...) {
            write_to_stderr_fallback(record.m_level,
                                     record.m_source.c_str(),
                                     record.m_group.c_str(),
                                     record.m_message.c_str());
            any_sink_ran = true;
        }
    } else if (callback_on && early_cap > 0) {
        std::scoped_lock const lock(state().m_config_mutex);
        if (!state().m_callback &&
            state().m_early_callback_buffer.size() < state().m_early_buffer_capacity) {
            state().m_early_callback_buffer.push_back(record);
            any_sink_ran = true;
        }
    }

    // 5. Last-resort fallback if every sink was disabled or failed.
    if (!any_sink_ran) { write_to_default_sink(record); }
}

// ---------------------------------------------------------------------------
// Real-time drain
// ---------------------------------------------------------------------------

LogRecord to_log_record(const RtRecord& rt_record) {
    LogRecord record;
    record.m_seq = rt_record.m_seq;
    record.m_timestamp_ms = rt_record.m_timestamp_ms;
    record.m_monotonic_ns = rt_record.m_monotonic_ns;
    record.m_level = rt_record.m_level;
    record.m_source = "rt";
    record.m_group = rt_record.m_group.data();
    record.m_message = rt_record.m_message.data();
    record.m_flags = rt_record.m_flags | k_flag_realtime;  // drain marks what it dispatches
    return record;
}

/// The process-wide default queue. Constructed on first use from a non-real-time
/// call; producers never reach it before g_rt_running / g_rt_manual_drain is set,
/// which only those calls do.
rt::Queue& default_queue() {
    // Never destroyed: LoggerState's destructor (which may run after this
    // object would have been torn down, being constructed earlier) still drains
    // it, and a real-time producer during static teardown must find a valid
    // object. Same lifetime the old constinit global had.
    static auto* const k_instance = new rt::Queue(rt::k_queue_capacity);
    return *k_instance;
}

void update_default_accepting() {
    default_queue().set_accepting(g_rt_running.load(std::memory_order_relaxed) ||
                                  g_rt_manual_drain.load(std::memory_order_relaxed));
}

/// `only_if_enabled`: set_config() starts the thread only when the config says so.
void start_drain_thread(bool only_if_enabled) {
    auto& s = state();
    std::scoped_lock const lock(s.m_rt_mutex);
    if (s.m_rt_drain) { return; }
    if (logging_shutdown_started()) { return; }
    if (only_if_enabled && !s.m_rt_enabled.load(std::memory_order_relaxed)) { return; }
    try {
        rt::DrainThread::Options options;
        options.m_interval_ms = s.m_rt_drain_interval_ms.load(std::memory_order_relaxed);
        s.m_rt_drain = std::make_unique<rt::DrainThread>(default_queue(), options);
    } catch (...) {
        write_to_stderr_fallback(static_cast<std::uint32_t>(LogLevel::Error),
                                 "native",
                                 "thl.logger",
                                 "failed to start real-time log drain thread");
        return;
    }
    g_rt_running.store(true, std::memory_order_release);
    update_default_accepting();
}

void stop_drain_thread() {
    auto& s = state();
    std::unique_ptr<rt::DrainThread> to_stop;
    {
        std::scoped_lock const lock(s.m_rt_mutex);
        to_stop.swap(s.m_rt_drain);
        g_rt_running.store(false, std::memory_order_release);
        update_default_accepting();
    }
    if (!to_stop) { return; }
    if (to_stop->is_current_thread()) {
        // stop() called from inside a sink callback on the drain thread: it cannot
        // join itself. Let it run out; the flag is already down.
        to_stop.release();  // NOLINT(bugprone-unused-return-value) intentional leak
        return;
    }
    to_stop.reset();  // joins and delivers what arrived after the thread's last pass
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API -- runtime level
// ---------------------------------------------------------------------------

void set_level(LogLevel level) {
    g_runtime_level.store(clamp_level(static_cast<std::uint32_t>(level)),
                          std::memory_order_relaxed);
}

LogLevel get_level() TANH_NONBLOCKING_FUNCTION {
    return static_cast<LogLevel>(g_runtime_level.load(std::memory_order_relaxed));
}

bool is_enabled(LogLevel level) TANH_NONBLOCKING_FUNCTION {
    const auto numeric = static_cast<std::uint32_t>(level);
    return should_log_compiled(numeric) && passes_runtime_level(numeric);
}

// ---------------------------------------------------------------------------
// Public API -- real-time path
// ---------------------------------------------------------------------------

namespace rt {

// ---------------------------------------------------------------------------
// Queue
// ---------------------------------------------------------------------------

struct Queue::Impl {
    explicit Impl(std::size_t capacity) : m_queue(capacity) {}

    thl::core::DynamicLockFreeQueue<RtRecord> m_queue;
    std::atomic<std::uint64_t> m_dropped{0};
    std::atomic<bool> m_accepting{true};
};

Queue::Queue(std::size_t capacity) : m_impl(std::make_unique<Impl>(capacity)) {}

Queue::~Queue() = default;

Status Queue::vlogf(LogLevel level,
                    std::uint32_t flags,
                    const char* group,
                    const char* fmt,
                    va_list args) noexcept TANH_NONBLOCKING_FUNCTION {
    const auto numeric_level = static_cast<std::uint32_t>(level);
    if (!should_log_compiled(numeric_level) || !passes_runtime_level(numeric_level)) {
        return Status::Filtered;
    }
    if (!m_impl->m_accepting.load(std::memory_order_relaxed)) { return Status::NoConsumer; }

    RtRecord record;
    record.m_seq = g_next_seq.fetch_add(1, std::memory_order_relaxed);
    record.m_timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    record.m_monotonic_ns =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    record.m_level = clamp_level(numeric_level);
    record.m_flags = flags;
    copy_bounded(record.m_group, group);

    bool truncated = false;
    if (fmt == nullptr) {
        record.m_message[0] = '\0';
    } else {
        const int n =
            thl::core::rt_vsnprintf(record.m_message.data(), k_message_capacity, fmt, args);
        truncated = n < 0 || std::cmp_greater_equal(n, k_message_capacity);
    }

    if (!m_impl->m_queue.try_push(record)) {
        m_impl->m_dropped.fetch_add(1, std::memory_order_relaxed);
        return Status::QueueFull;
    }
    return truncated ? Status::Truncated : Status::Ok;
}

Status Queue::vlogf(LogLevel level,
                    const char* group,
                    const char* fmt,
                    va_list args) noexcept TANH_NONBLOCKING_FUNCTION {
    return vlogf(level, 0U, group, fmt, args);
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
Status Queue::logf(LogLevel level,
                   std::uint32_t flags,
                   const char* group,
                   const char* fmt,
                   ...) noexcept TANH_NONBLOCKING_FUNCTION {
    va_list args;
    va_start(args, fmt);
    const Status status = vlogf(level, flags, group, fmt, args);
    va_end(args);
    return status;
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
Status Queue::logf(LogLevel level,
                   const char* group,
                   const char* fmt,
                   ...) noexcept TANH_NONBLOCKING_FUNCTION {
    va_list args;
    va_start(args, fmt);
    const Status status = vlogf(level, 0U, group, fmt, args);
    va_end(args);
    return status;
}

Status Queue::log(LogLevel level,
                  std::uint32_t flags,
                  const char* group,
                  const char* message) noexcept TANH_NONBLOCKING_FUNCTION {
    // "%s" through the RT formatter keeps one code path; the message is bounded
    // by the record either way.
    return logf(level, flags, group, "%s", message == nullptr ? "" : message);
}

Status Queue::log(LogLevel level,
                  const char* group,
                  const char* message) noexcept TANH_NONBLOCKING_FUNCTION {
    return log(level, 0U, group, message);
}

std::size_t Queue::drain() {
    // Taken before the pop loop so the count can ride on this pass's first
    // record; drops that happen while the pass runs are reported by the next.
    std::uint64_t dropped = m_impl->m_dropped.exchange(0, std::memory_order_relaxed);

    std::size_t delivered = 0;
    RtRecord rt_record;
    while (m_impl->m_queue.try_pop(rt_record)) {
        LogRecord record = to_log_record(rt_record);
        record.m_dropped_before = dropped;  // non-zero on the first record only
        dropped = 0;
        dispatch_record(record);
        ++delivered;
    }

    if (dropped > 0) {
        // No record to carry the count: report it on one synthetic warning.
        LogRecord record = make_record(static_cast<std::uint32_t>(LogLevel::Warning),
                                       k_flag_realtime,
                                       "rt",
                                       "thl.logger",
                                       "real-time log queue overflowed");
        record.m_dropped_before = dropped;
        dispatch_record(record);
    }
    return delivered;
}

void Queue::set_accepting(bool accepting) noexcept {
    m_impl->m_accepting.store(accepting, std::memory_order_release);
}

bool Queue::is_accepting() const noexcept TANH_NONBLOCKING_FUNCTION {
    return m_impl->m_accepting.load(std::memory_order_relaxed);
}

std::uint64_t Queue::dropped_count() const noexcept TANH_NONBLOCKING_FUNCTION {
    return m_impl->m_dropped.load(std::memory_order_relaxed);
}

std::size_t Queue::capacity() const noexcept {
    return m_impl->m_queue.capacity();
}

// ---------------------------------------------------------------------------
// DrainThread
// ---------------------------------------------------------------------------

struct DrainThread::Impl {
    explicit Impl(Queue& queue) : m_queue(queue) {}

    Queue& m_queue;
    thl::core::Thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<std::uint32_t> m_interval_ms{10};
    std::atomic<std::thread::id> m_thread_id{};
};

DrainThread::DrainThread(Queue& queue) : DrainThread(queue, Options{}) {}

DrainThread::DrainThread(Queue& queue, const Options& options)
    : m_impl(std::make_unique<Impl>(queue)) {
    m_impl->m_interval_ms.store(options.m_interval_ms == 0 ? 1U : options.m_interval_ms,
                                std::memory_order_relaxed);
    thl::core::ThreadOptions thread_options;
    thread_options.m_priority = options.m_priority;
    thread_options.m_name = options.m_name;
    Impl* impl = m_impl.get();
    const bool started =
        m_impl->m_thread.start(thread_options, [impl](const thl::core::Thread& self) {
            impl->m_thread_id.store(std::this_thread::get_id(), std::memory_order_relaxed);
            std::unique_lock lock(impl->m_mutex);
            while (!self.should_stop()) {
                lock.unlock();
                impl->m_queue.drain();
                lock.lock();
                const auto interval =
                    std::chrono::milliseconds(impl->m_interval_ms.load(std::memory_order_relaxed));
                impl->m_cv.wait_for(lock, interval, [&self] { return self.should_stop(); });
            }
        });
    if (!started) { throw std::runtime_error("thl::Logger::rt::DrainThread: thread refused"); }
}

DrainThread::~DrainThread() noexcept {
    m_impl->m_thread.request_stop();
    {
        std::scoped_lock const lock(m_impl->m_mutex);
        m_impl->m_cv.notify_all();
    }
    if (is_current_thread()) {
        m_impl->m_thread.detach();
        return;
    }
    m_impl->m_thread.join();
    try {
        m_impl->m_queue.drain();  // whatever arrived after the thread's last pass
    } catch (...) {}              // NOLINT(bugprone-empty-catch) sinks already fell back to stderr
}

bool DrainThread::is_running() const noexcept TANH_NONBLOCKING_FUNCTION {
    return m_impl->m_thread.is_running();
}

bool DrainThread::is_current_thread() const noexcept {
    return m_impl->m_thread_id.load(std::memory_order_relaxed) == std::this_thread::get_id();
}

void DrainThread::set_interval_ms(std::uint32_t interval_ms) noexcept {
    m_impl->m_interval_ms.store(interval_ms == 0 ? 1U : interval_ms, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Process-wide default queue
// ---------------------------------------------------------------------------

namespace {

/// True while somebody consumes the default queue. Checked before touching
/// default_queue(): its construction is not real-time safe and happens on the
/// non-real-time call that raised one of these flags.
bool default_queue_has_consumer() noexcept TANH_NONBLOCKING_FUNCTION {
    return g_rt_running.load(std::memory_order_relaxed) ||
           g_rt_manual_drain.load(std::memory_order_relaxed);
}

}  // namespace

// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
Status logf(LogLevel level,
            const char* group,
            const char* fmt,
            ...) noexcept TANH_NONBLOCKING_FUNCTION {
    if (!default_queue_has_consumer()) { return Status::NoConsumer; }
    va_list args;
    va_start(args, fmt);
    const Status status = default_queue().vlogf(level, 0U, group, fmt, args);
    va_end(args);
    return status;
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
Status logf(LogLevel level, std::uint32_t flags, const char* group, const char* fmt, ...) noexcept
    TANH_NONBLOCKING_FUNCTION {
    if (!default_queue_has_consumer()) { return Status::NoConsumer; }
    va_list args;
    va_start(args, fmt);
    const Status status = default_queue().vlogf(level, flags, group, fmt, args);
    va_end(args);
    return status;
}

Status log(LogLevel level,
           const char* group,
           const char* message) noexcept TANH_NONBLOCKING_FUNCTION {
    if (!default_queue_has_consumer()) { return Status::NoConsumer; }
    return default_queue().log(level, 0U, group, message);
}

Status log(LogLevel level,
           std::uint32_t flags,
           const char* group,
           const char* message) noexcept TANH_NONBLOCKING_FUNCTION {
    if (!default_queue_has_consumer()) { return Status::NoConsumer; }
    return default_queue().log(level, flags, group, message);
}

void start() {
    if (logging_shutdown_started()) { return; }
    start_drain_thread(false);
}

void stop() {
    if (logging_shutdown_started()) { return; }
    stop_drain_thread();
}

bool is_running() noexcept TANH_NONBLOCKING_FUNCTION {
    return g_rt_running.load(std::memory_order_relaxed);
}

std::size_t drain() {
    if (logging_shutdown_started()) { return 0; }
    return default_queue().drain();
}

void enable_manual_drain(bool enabled) {
    if (logging_shutdown_started()) { return; }
    auto& s = state();
    std::scoped_lock const lock(s.m_rt_mutex);
    g_rt_manual_drain.store(enabled, std::memory_order_release);
    update_default_accepting();
}

std::uint64_t dropped_count() noexcept TANH_NONBLOCKING_FUNCTION {
    if (!default_queue_has_consumer()) { return 0; }
    return default_queue().dropped_count();
}

}  // namespace rt

// ---------------------------------------------------------------------------
// Public API -- config
// ---------------------------------------------------------------------------

void set_config(const LoggerConfig& config) {
    auto& s = state();

    std::vector<LogRecord> file_buffered;
    {
        std::scoped_lock const lock(s.m_config_mutex);
        s.m_platform_enabled = config.m_platform_enabled;
        s.m_console_enabled = config.m_console_enabled;
        s.m_file_enabled = config.m_file_enabled;
        s.m_callback_enabled = config.m_callback_enabled;
        s.m_early_buffer_capacity = config.m_early_buffer_capacity;
        s.m_rt_enabled.store(config.m_rt_enabled, std::memory_order_relaxed);
        s.m_rt_drain_interval_ms.store(
            config.m_rt_drain_interval_ms == 0 ? 1U : config.m_rt_drain_interval_ms,
            std::memory_order_relaxed);

        // Platform sink identity; an empty string selects the default.
        const std::string subsystem =
            config.m_platform_subsystem.empty() ? "thl" : config.m_platform_subsystem;
        const std::string category =
            config.m_platform_category.empty() ? "logger" : config.m_platform_category;
#if defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
        if (subsystem != s.m_platform_subsystem || category != s.m_platform_category) {
            s.m_platform_log = nullptr;  // re-created for the new identity on the next record
        }
#endif
        s.m_platform_tag = config.m_platform_tag.empty() ? "thl" : config.m_platform_tag;
        s.m_platform_subsystem = subsystem;
        s.m_platform_category = category;

        // Drain the file early buffer when a path becomes available.
        if (config.m_file_enabled && !config.m_file_path.empty()) {
            file_buffered.swap(s.m_early_file_buffer);
        }
    }

    {
        std::scoped_lock const lock(s.m_file_mutex);
        const bool path_changed = (s.m_file_path != config.m_file_path);
        if (path_changed || !config.m_file_enabled) {
            if (s.m_file_stream.is_open()) { s.m_file_stream.close(); }
        }
        s.m_file_path = config.m_file_path;
    }

    // Replay buffered records into the file sink now that the path is set.
    for (const auto& record : file_buffered) { emit_file(record); }

    {
        std::scoped_lock const lock(s.m_rt_mutex);
        if (s.m_rt_drain) {
            s.m_rt_drain->set_interval_ms(s.m_rt_drain_interval_ms.load(std::memory_order_relaxed));
        }
    }
    if (config.m_rt_enabled) {
        start_drain_thread(true);
    } else {
        stop_drain_thread();
    }
}

LoggerConfig get_config() {
    auto& s = state();
    LoggerConfig config;
    {
        std::scoped_lock const lock(s.m_config_mutex);
        config.m_platform_enabled = s.m_platform_enabled;
        config.m_console_enabled = s.m_console_enabled;
        config.m_file_enabled = s.m_file_enabled;
        config.m_callback_enabled = s.m_callback_enabled;
        config.m_early_buffer_capacity = s.m_early_buffer_capacity;
        config.m_rt_enabled = s.m_rt_enabled.load(std::memory_order_relaxed);
        config.m_rt_drain_interval_ms = s.m_rt_drain_interval_ms.load(std::memory_order_relaxed);
        config.m_platform_tag = s.m_platform_tag;
        config.m_platform_subsystem = s.m_platform_subsystem;
        config.m_platform_category = s.m_platform_category;
    }
    {
        std::scoped_lock const lock(s.m_file_mutex);
        config.m_file_path = s.m_file_path;
    }
    return config;
}

// ---------------------------------------------------------------------------
// Public API -- callback
// ---------------------------------------------------------------------------

void set_callback(const Callback& cb) {
    std::vector<LogRecord> buffered;
    {
        auto& s = state();
        std::scoped_lock const lock(s.m_config_mutex);
        s.m_callback = cb;
        buffered.swap(s.m_early_callback_buffer);
    }

    if (cb && !buffered.empty()) {
        CallbackDispatchScope const scope;
        for (const auto& record : buffered) {
            try {
                cb(record);
            } catch (...) {}  // NOLINT(bugprone-empty-catch) don't let one callback crash the
                              // logger
        }
    }
}

void clear_callback() {
    auto& s = state();
    std::scoped_lock const lock(s.m_config_mutex);
    s.m_callback = nullptr;
}

// ---------------------------------------------------------------------------
// Public API -- formatting
// ---------------------------------------------------------------------------

std::string format_plain(const LogRecord& record) {
    std::ostringstream out;
    out << '[' << level_name(record.m_level) << "]["
        << (record.m_source.empty() ? "native" : record.m_source) << "]["
        << (record.m_group.empty() ? "default" : record.m_group) << "] " << record.m_message;
    if (record.m_dropped_before != 0) { out << drop_note(record); }
    return out.str();
}

std::string format_logfmt(const LogRecord& record) {
    std::ostringstream out;
    append_logfmt_field(out, "time", format_iso8601_utc_ms(record.m_timestamp_ms));
    out << ' ';
    out << "level=" << level_name(record.m_level) << " seq=" << record.m_seq
        << " ts_ms=" << record.m_timestamp_ms << " mono_ns=" << record.m_monotonic_ns << ' ';
    append_logfmt_field(out, "source", record.m_source.empty() ? "native" : record.m_source);
    out << ' ';
    append_logfmt_field(out, "group", record.m_group.empty() ? "default" : record.m_group);
    out << ' ';
    append_logfmt_field(out, "message", record.m_message);
    if (record.m_dropped_before != 0) { out << " dropped_before=" << record.m_dropped_before; }
    return out.str();
}

// ---------------------------------------------------------------------------
// Public API -- logging
// ---------------------------------------------------------------------------

void log(LogLevel level, const char* group, const char* message) {
    log_with_source(level, "native", group, message);
}

void log_with_source(LogLevel level, const char* source, const char* group, const char* message) {
    log_with_source(level, 0U, source, group, message);
}

void log_with_source(LogLevel level,
                     std::uint32_t flags,
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
    if (!passes_runtime_level(numeric_level)) { return; }

    try {
        dispatch_record(make_record(numeric_level, flags, source, group, message));
    } catch (...) { write_to_stderr_fallback(numeric_level, source, group, message); }
}

namespace {

void vlogf_sync(LogLevel level,
                std::uint32_t flags,
                const char* group,
                const char* fmt,
                va_list args) {
    if (!fmt) {
        log_with_source(level, flags, "native", group, "");
        return;
    }

    std::array<char, 1024> stack_buffer{};
    const int written = std::vsnprintf(stack_buffer.data(), stack_buffer.size(), fmt, args);
    if (written < 0) {
        log_with_source(level, flags, "native", group, "logf formatting failed");
        return;
    }

    log_with_source(level, flags, "native", group, stack_buffer.data());
}

}  // namespace

// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
void logf(LogLevel level, const char* group, const char* fmt, ...) {
    if (!should_log_compiled(static_cast<std::uint32_t>(level))) { return; }
    va_list args;
    va_start(args, fmt);
    vlogf_sync(level, 0U, group, fmt, args);
    va_end(args);
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
void logf(LogLevel level, std::uint32_t flags, const char* group, const char* fmt, ...) {
    if (!should_log_compiled(static_cast<std::uint32_t>(level))) { return; }
    va_list args;
    va_start(args, fmt);
    vlogf_sync(level, flags, group, fmt, args);
    va_end(args);
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
