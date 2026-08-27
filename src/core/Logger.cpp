// SPDX-License-Identifier: Apache-2.0
#include <tanh/core/Logger.h>
#include <tanh/core/RtFormat.h>
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
#include <mutex>
#include <sstream>
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
#elif defined(THL_PLATFORM_LINUX)
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
std::atomic<bool> g_rt_running{false};
std::atomic<bool> g_rt_manual_drain{false};
std::atomic<std::uint64_t> g_rt_dropped{0};

bool passes_runtime_level(std::uint32_t level) TANH_NONBLOCKING_FUNCTION {
    return clamp_level(level) <= g_runtime_level.load(std::memory_order_relaxed);
}

struct RtRecord {
    std::uint64_t m_seq = 0;
    std::int64_t m_timestamp_ms = 0;
    std::uint64_t m_monotonic_ns = 0;
    std::uint32_t m_level = 0;
    std::array<char, rt::k_group_capacity> m_group{};
    std::array<char, rt::k_message_capacity> m_message{};
};

/// Bounded multi-producer / multi-consumer ring (Dmitry Vyukov's sequence
/// scheme). Producers never wait: a full ring fails the push. Constant-
/// initialised; the cell sequence numbers are set up once by init().
class RtQueue {
public:
    constexpr RtQueue() = default;

    void init() {
        for (std::size_t i = 0; i < rt::k_queue_capacity; ++i) {
            m_cells[i].m_seq.store(i, std::memory_order_relaxed);
        }
        m_enqueue_pos.store(0, std::memory_order_relaxed);
        m_dequeue_pos.store(0, std::memory_order_relaxed);
    }

    bool try_push(const RtRecord& record) noexcept TANH_NONBLOCKING_FUNCTION {
        std::size_t pos = m_enqueue_pos.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        for (;;) {
            cell = &m_cells[pos & k_mask];
            const std::size_t seq = cell->m_seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (m_enqueue_pos.compare_exchange_weak(pos,
                                                        pos + 1,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // full
            } else {
                pos = m_enqueue_pos.load(std::memory_order_relaxed);
            }
        }
        cell->m_record = record;
        cell->m_seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(RtRecord& out) noexcept {
        std::size_t pos = m_dequeue_pos.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        for (;;) {
            cell = &m_cells[pos & k_mask];
            const std::size_t seq = cell->m_seq.load(std::memory_order_acquire);
            const auto diff =
                static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                if (m_dequeue_pos.compare_exchange_weak(pos,
                                                        pos + 1,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // empty
            } else {
                pos = m_dequeue_pos.load(std::memory_order_relaxed);
            }
        }
        out = cell->m_record;
        cell->m_seq.store(pos + k_mask + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t k_mask = rt::k_queue_capacity - 1;

    struct Cell {
        std::atomic<std::size_t> m_seq{0};
        RtRecord m_record;
    };

    alignas(64) std::atomic<std::size_t> m_enqueue_pos{0};
    alignas(64) std::atomic<std::size_t> m_dequeue_pos{0};
    std::array<Cell, rt::k_queue_capacity> m_cells{};
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) intentional: RT path
RtQueue g_rt_queue;

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

#if defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
os_log_t platform_log_handle() {
    static os_log_t handle = os_log_create("thl", "logger");
    return handle;
}

bool is_debugger_attached() {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info{};
    size_t size = sizeof(info);
    sysctl(mib, 4, &info, &size, nullptr, 0);
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}
#endif

bool emit_platform(const LogRecord& record) {
    const char* source = record.m_source.empty() ? "native" : record.m_source.c_str();
    const char* group = record.m_group.empty() ? "default" : record.m_group.c_str();
    const char* message = record.m_message.c_str();

#if defined(THL_PLATFORM_ANDROID)
    int android_level = ANDROID_LOG_INFO;
    switch (clamp_level(record.m_level)) {
        case static_cast<std::uint32_t>(LogLevel::Error): android_level = ANDROID_LOG_ERROR; break;
        case static_cast<std::uint32_t>(LogLevel::Warning): android_level = ANDROID_LOG_WARN; break;
        case static_cast<std::uint32_t>(LogLevel::Info): android_level = ANDROID_LOG_INFO; break;
        case static_cast<std::uint32_t>(LogLevel::Debug): android_level = ANDROID_LOG_DEBUG; break;
        default: android_level = ANDROID_LOG_INFO; break;
    }
    __android_log_print(android_level, "thl", "[%s][%s] %s", source, group, message);
    return true;

#elif defined(THL_PLATFORM_LINUX)
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
                    "thl",
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
        os_log_with_type(platform_log_handle(),
                         type,
                         "[%{public}s][%{public}s] %{public}s",
                         source,
                         group,
                         message);
    }
    write_to_default_sink(record);
#else
    os_log_with_type(platform_log_handle(),
                     type,
                     "[%{public}s][%{public}s] %{public}s",
                     source,
                     group,
                     message);
#endif
    return true;
#else
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

    // Protects file_path + file_stream.  Lock ordering: config_mutex
    // before file_mutex.
    std::mutex m_file_mutex;
    std::string m_file_path;
    std::ofstream m_file_stream;

    // Real-time drain thread. Protected by m_rt_mutex; g_rt_running is the
    // lock-free view producers read.
    std::mutex m_rt_mutex;
    std::condition_variable m_rt_cv;
    std::thread m_rt_thread;
    bool m_rt_enabled = true;
    std::atomic<std::uint32_t> m_rt_drain_interval_ms{10};
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
    {
        std::scoped_lock const lock(state().m_config_mutex);
        platform_on = state().m_platform_enabled;
        console_on = state().m_console_enabled;
        file_on = state().m_file_enabled;
        callback_on = state().m_callback_enabled;
        early_cap = state().m_early_buffer_capacity;
        callback_copy = state().m_callback;
    }

    bool any_sink_ran = false;

    // 1. Platform sink (lock-free, fire-and-forget).
    if (platform_on) {
        if (emit_platform(record)) { any_sink_ran = true; }
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
    return record;
}

std::size_t drain_rt_queue() {
    std::size_t delivered = 0;
    RtRecord rt_record;
    while (g_rt_queue.try_pop(rt_record)) {
        dispatch_record(to_log_record(rt_record));
        ++delivered;
    }

    const std::uint64_t dropped = g_rt_dropped.exchange(0, std::memory_order_relaxed);
    if (dropped > 0) {
        std::array<char, 96> msg{};
        std::snprintf(msg.data(),
                      msg.size(),
                      "%llu real-time log message(s) dropped (queue full)",
                      static_cast<unsigned long long>(dropped));
        dispatch_record(make_record(static_cast<std::uint32_t>(LogLevel::Warning),
                                    "rt",
                                    "thl.logger",
                                    msg.data()));
    }
    return delivered;
}

void drain_thread_main() {
    auto& s = state();
    std::unique_lock lock(s.m_rt_mutex);
    while (g_rt_running.load(std::memory_order_relaxed)) {
        lock.unlock();
        drain_rt_queue();
        lock.lock();
        const auto interval =
            std::chrono::milliseconds(s.m_rt_drain_interval_ms.load(std::memory_order_relaxed));
        s.m_rt_cv.wait_for(lock, interval, [] {
            return !g_rt_running.load(std::memory_order_relaxed);
        });
    }
}

void start_drain_thread() {
    auto& s = state();
    std::scoped_lock const lock(s.m_rt_mutex);
    if (s.m_rt_thread.joinable()) { return; }
    if (logging_shutdown_started()) { return; }
    if (!g_rt_manual_drain.load(std::memory_order_relaxed)) { g_rt_queue.init(); }
    g_rt_running.store(true, std::memory_order_release);
    try {
        s.m_rt_thread = std::thread(drain_thread_main);
    } catch (...) {
        g_rt_running.store(false, std::memory_order_release);
        write_to_stderr_fallback(static_cast<std::uint32_t>(LogLevel::Error),
                                 "native",
                                 "thl.logger",
                                 "failed to start real-time log drain thread");
    }
}

void stop_drain_thread() {
    auto& s = state();
    std::thread to_join;
    {
        std::scoped_lock const lock(s.m_rt_mutex);
        g_rt_running.store(false, std::memory_order_release);
        s.m_rt_cv.notify_all();
        to_join.swap(s.m_rt_thread);
    }
    if (to_join.joinable()) {
        if (to_join.get_id() == std::this_thread::get_id()) {
            to_join.detach();  // stop() called from inside a sink callback
        } else {
            to_join.join();
        }
    }
    // Deliver whatever arrived after the thread's last pass.
    drain_rt_queue();
}

void ensure_drain_thread_if_enabled() {
    if (g_rt_running.load(std::memory_order_acquire)) { return; }
    auto& s = state();
    bool enabled = false;
    {
        std::scoped_lock const lock(s.m_config_mutex);
        enabled = s.m_rt_enabled;
    }
    if (enabled) { start_drain_thread(); }
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

namespace {

Status enqueue(std::uint32_t level,
               const char* group,
               const char* fmt,
               va_list args,
               bool preformatted) noexcept TANH_NONBLOCKING_FUNCTION {
    if (!should_log_compiled(level) || !passes_runtime_level(level)) { return Status::Filtered; }
    if (!g_rt_running.load(std::memory_order_relaxed) &&
        !g_rt_manual_drain.load(std::memory_order_relaxed)) {
        return Status::NoConsumer;
    }

    RtRecord record;
    record.m_seq = g_next_seq.fetch_add(1, std::memory_order_relaxed);
    record.m_timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    record.m_monotonic_ns =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    record.m_level = clamp_level(level);
    copy_bounded(record.m_group, group);

    bool truncated = false;
    if (preformatted) {
        copy_bounded(record.m_message, fmt);
        truncated = fmt != nullptr && std::strlen(fmt) >= k_message_capacity;
    } else {
        const int n =
            thl::core::rt_vsnprintf(record.m_message.data(), k_message_capacity, fmt, args);
        truncated = n < 0 || std::cmp_greater_equal(n, k_message_capacity);
    }

    if (!g_rt_queue.try_push(record)) {
        g_rt_dropped.fetch_add(1, std::memory_order_relaxed);
        return Status::QueueFull;
    }
    return truncated ? Status::Truncated : Status::Ok;
}

}  // namespace

// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
Status logf(LogLevel level,
            const char* group,
            const char* fmt,
            ...) noexcept TANH_NONBLOCKING_FUNCTION {
    va_list args;
    va_start(args, fmt);
    const Status status = enqueue(static_cast<std::uint32_t>(level), group, fmt, args, false);
    va_end(args);
    return status;
}

Status log(LogLevel level,
           const char* group,
           const char* message) noexcept TANH_NONBLOCKING_FUNCTION {
    va_list unused{};
    return enqueue(static_cast<std::uint32_t>(level), group, message, unused, true);
}

void start() {
    start_drain_thread();
}

void stop() {
    stop_drain_thread();
}

bool is_running() noexcept TANH_NONBLOCKING_FUNCTION {
    return g_rt_running.load(std::memory_order_relaxed);
}

std::size_t drain() {
    return drain_rt_queue();
}

void enable_manual_drain(bool enabled) {
    auto& s = state();
    std::scoped_lock const lock(s.m_rt_mutex);
    if (enabled && !g_rt_manual_drain.load(std::memory_order_relaxed) &&
        !g_rt_running.load(std::memory_order_relaxed)) {
        g_rt_queue.init();
    }
    g_rt_manual_drain.store(enabled, std::memory_order_release);
}

std::uint64_t dropped_count() noexcept TANH_NONBLOCKING_FUNCTION {
    return g_rt_dropped.load(std::memory_order_relaxed);
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
        s.m_rt_enabled = config.m_rt_enabled;
        s.m_rt_drain_interval_ms.store(
            config.m_rt_drain_interval_ms == 0 ? 1U : config.m_rt_drain_interval_ms,
            std::memory_order_relaxed);

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

    if (config.m_rt_enabled) {
        start_drain_thread();
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
        config.m_rt_enabled = s.m_rt_enabled;
        config.m_rt_drain_interval_ms = s.m_rt_drain_interval_ms.load(std::memory_order_relaxed);
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
    ensure_drain_thread_if_enabled();
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
    return out.str();
}

// ---------------------------------------------------------------------------
// Public API -- logging
// ---------------------------------------------------------------------------

void log(LogLevel level, const char* group, const char* message) {
    log_with_source(level, "native", group, message);
}

void log_with_source(LogLevel level, const char* source, const char* group, const char* message) {
    const auto numeric_level = static_cast<std::uint32_t>(level);
    if (!should_log_compiled(numeric_level)) { return; }

    ensure_shutdown_hook_installed();
    if (logging_shutdown_started()) {
        write_to_stderr_fallback(numeric_level, source, group, message);
        return;
    }
    if (!passes_runtime_level(numeric_level)) { return; }
    ensure_drain_thread_if_enabled();

    try {
        dispatch_record(make_record(numeric_level, source, group, message));
    } catch (...) { write_to_stderr_fallback(numeric_level, source, group, message); }
}

void logf(LogLevel level, const char* group, const char* fmt, ...) {
    if (!should_log_compiled(static_cast<std::uint32_t>(level))) { return; }

    if (!fmt) {
        log(level, group, "");
        return;
    }

    std::array<char, 1024> stack_buffer{};
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(stack_buffer.data(), stack_buffer.size(), fmt, args);
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
