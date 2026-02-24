#include <tanh/core/Logger.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace thl::Logger {

namespace {

#ifndef THL_LOG_COMPILED_MAX_LEVEL
#define THL_LOG_COMPILED_MAX_LEVEL 4
#endif

constexpr size_t kQueueCapacity = 2048;

spdlog::level::level_enum to_spdlog_level(std::uint32_t level) {
    switch (level) {
        case static_cast<std::uint32_t>(LogLevel::Error):
            return spdlog::level::err;
        case static_cast<std::uint32_t>(LogLevel::Warning):
            return spdlog::level::warn;
        case static_cast<std::uint32_t>(LogLevel::Info):
            return spdlog::level::info;
        case static_cast<std::uint32_t>(LogLevel::Debug):
            return spdlog::level::debug;
        default:
            return spdlog::level::info;
    }
}

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

std::shared_ptr<spdlog::logger> create_fallback_logger() {
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("tanh_logger", sink);
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
    logger->set_level(spdlog::level::debug);
    logger->flush_on(spdlog::level::trace);
    return logger;
}

std::shared_ptr<spdlog::logger> fallback_logger() {
    static auto logger = create_fallback_logger();
    return logger;
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

std::string format_iso8601_utc_ms(std::int64_t timestampMs) {
    std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000);
    int millis = static_cast<int>(timestampMs % 1000);
    if (millis < 0) {
        millis += 1000;
        --seconds;
    }

    std::tm tmValue {};
#if defined(_WIN32)
    gmtime_s(&tmValue, &seconds);
#else
    gmtime_r(&seconds, &tmValue);
#endif

    std::ostringstream out;
    out << std::put_time(&tmValue, "%Y-%m-%dT%H:%M:%S")
        << '.'
        << std::setw(3) << std::setfill('0') << millis
        << 'Z';
    return out.str();
}

void write_to_default_sink(const LogRecord& record) {
    const char* safeSource =
        record.source.empty() ? "native" : record.source.c_str();
    const char* safeGroup = record.group.empty() ? "default" : record.group.c_str();
    const char* safeMessage = record.message.c_str();
    fallback_logger()->log(to_spdlog_level(record.level),
                           "[{}][{}] {}",
                           safeSource,
                           safeGroup,
                           safeMessage);
}

class AsyncLoggerCore {
public:
    AsyncLoggerCore() : m_worker([this]() { run(); }) {}

    ~AsyncLoggerCore() {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_stop = true;
        }
        m_queueCv.notify_one();
        if (m_worker.joinable()) { m_worker.join(); }
    }

    void setCallback(Callback cb) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callback = std::move(cb);
    }

    void clearCallback() {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callback = nullptr;
    }

    void enqueue(LogRecord record) {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_queueSize == kQueueCapacity) {
                m_ring[m_head] = std::move(record);
                m_head = (m_head + 1) % kQueueCapacity;
                ++m_dropCount;
            } else {
                const size_t tail = (m_head + m_queueSize) % kQueueCapacity;
                m_ring[tail] = std::move(record);
                ++m_queueSize;
            }
        }
        m_queueCv.notify_one();
    }

    LogRecord makeRecord(std::uint32_t level,
                         const char* source,
                         const char* group,
                         const char* message) {
        LogRecord record;
        record.seq = m_nextSeq.fetch_add(1, std::memory_order_relaxed);
        const auto wallNow = std::chrono::system_clock::now();
        const auto monoNow = std::chrono::steady_clock::now();
        record.timestampMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                wallNow.time_since_epoch())
                .count();
        record.monotonicNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                monoNow.time_since_epoch())
                .count();
        record.level = clamp_level(level);
        record.source = source ? source : "native";
        record.group = group ? group : "default";
        record.message = message ? message : "";
        return record;
    }

private:
    void run() {
        while (true) {
            std::optional<LogRecord> record;
            std::uint64_t dropped = 0;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCv.wait(lock, [this]() { return m_stop || m_queueSize > 0; });
                if (m_stop && m_queueSize == 0) { break; }

                dropped = m_dropCount;
                m_dropCount = 0;

                if (m_queueSize > 0) {
                    record = std::move(m_ring[m_head]);
                    m_ring[m_head].reset();
                    m_head = (m_head + 1) % kQueueCapacity;
                    --m_queueSize;
                }
            }

            if (dropped > 0) {
                const std::string droppedMessage =
                    "Dropped " + std::to_string(dropped) + " log message(s)";
                auto droppedRecord = makeRecord(
                    static_cast<std::uint32_t>(LogLevel::Warning),
                    "native",
                    "logger",
                    droppedMessage.c_str());
                dispatch(droppedRecord);
            }

            if (record.has_value()) { dispatch(*record); }
        }
    }

    void dispatch(const LogRecord& record) {
        Callback callbackCopy;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            callbackCopy = m_callback;
        }

        if (callbackCopy) {
            callbackCopy(record);
            return;
        }

        write_to_default_sink(record);
    }

    std::array<std::optional<LogRecord>, kQueueCapacity> m_ring {};
    size_t m_head = 0;
    size_t m_queueSize = 0;
    std::uint64_t m_dropCount = 0;
    bool m_stop = false;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::thread m_worker;
    std::atomic<std::uint64_t> m_nextSeq {1};

    std::mutex m_callbackMutex;
    Callback m_callback;
};

AsyncLoggerCore& core() {
    static AsyncLoggerCore instance;
    return instance;
}

}  // namespace

void set_callback(Callback cb) {
    core().setCallback(std::move(cb));
}

void clear_callback() {
    core().clearCallback();
}

std::string format_logfmt(const LogRecord& record) {
    std::ostringstream out;
    append_logfmt_field(out, "time", format_iso8601_utc_ms(record.timestampMs));
    out << ' ';
    out << "level=" << level_name(record.level)
        << " seq=" << record.seq
        << " ts_ms=" << record.timestampMs
        << " mono_ns=" << record.monotonicNs
        << ' ';
    append_logfmt_field(out, "source", record.source.empty() ? "native" : record.source);
    out << ' ';
    append_logfmt_field(out, "group", record.group.empty() ? "default" : record.group);
    out << ' ';
    append_logfmt_field(out, "message", record.message);
    return out.str();
}

void log(LogLevel level, const char* group, const char* message) {
    log_with_source(level, "native", group, message);
}

void log_with_source(LogLevel level,
                     const char* source,
                     const char* group,
                     const char* message) {
    const auto numericLevel = static_cast<std::uint32_t>(level);
    if (!should_log_compiled(numericLevel)) { return; }

    core().enqueue(core().makeRecord(numericLevel, source, group, message));
}

void logf(LogLevel level, const char* group, const char* fmt, ...) {
    if (!should_log_compiled(static_cast<std::uint32_t>(level))) { return; }

    if (!fmt) {
        log(level, group, "");
        return;
    }

    std::array<char, 1024> stackBuffer {};
    va_list args;
    va_start(args, fmt);
    const int written =
        std::vsnprintf(stackBuffer.data(), stackBuffer.size(), fmt, args);
    va_end(args);

    if (written < 0) {
        log(level, group, "logf formatting failed");
        return;
    }

    log(level, group, stackBuffer.data());
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
