#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tanh/core/Logger.h"
#include "tanh/utils/RealtimeSanitizer.h"

using thl::Logger::k_flag_contract_violation;
using thl::Logger::k_flag_realtime;
using thl::Logger::LogLevel;
using thl::Logger::LogRecord;
namespace rt = thl::Logger::rt;

namespace {

/// Captures every record that reaches the callback sink. All sinks except
/// the callback are turned off so tests don't spam the platform log.
class LoggerFixture : public ::testing::Test {
protected:
    void SetUp() override {
        thl::Logger::LoggerConfig config;
        config.m_platform_enabled = false;
        config.m_console_enabled = false;
        config.m_file_enabled = false;
        config.m_callback_enabled = true;
        config.m_rt_enabled = true;
        config.m_rt_drain_interval_ms = 1;
        thl::Logger::set_config(config);
        thl::Logger::set_level(LogLevel::Debug);
        thl::Logger::set_callback([this](const LogRecord& record) {
            std::scoped_lock const lock(m_mutex);
            m_records.push_back(record);
        });
        // Discard anything queued by earlier tests without racing the drain
        // thread: stop it, flush, clear, restart.
        rt::stop();
        clear();
        rt::start();
    }

    void TearDown() override {
        // The drain thread invokes a *copy* of the callback outside the
        // config mutex, so clear_callback() alone cannot stop a delivery that
        // is already in flight — it would land in m_records while the fixture
        // is being destroyed. Join the drain thread first; stop() flushes the
        // leftovers synchronously while the fixture is still alive.
        rt::stop();
        thl::Logger::clear_callback();
        thl::Logger::set_level(LogLevel::Debug);
        thl::Logger::LoggerConfig config;
        config.m_platform_enabled = false;
        config.m_file_enabled = false;
        thl::Logger::set_config(config);
    }

    std::vector<LogRecord> records() {
        std::scoped_lock const lock(m_mutex);
        return m_records;
    }

    void clear() {
        std::scoped_lock const lock(m_mutex);
        m_records.clear();
    }

    // Wait until at least `count` records arrived or the timeout passes.
    bool wait_for_records(std::size_t count,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (records().size() >= count) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return records().size() >= count;
    }

    std::vector<LogRecord> rt_records(const std::string& group = "") {
        auto all = records();
        std::vector<LogRecord> out;
        std::copy_if(all.begin(), all.end(), std::back_inserter(out), [&](const LogRecord& r) {
            return r.m_source == "rt" && (group.empty() || r.m_group == group);
        });
        return out;
    }

private:
    std::mutex m_mutex;
    std::vector<LogRecord> m_records;
};

/// A flag bit outside the reserved k_flag_* values, as a consumer would define.
constexpr std::uint32_t k_consumer_bit = 1U << 8;

/// Sum of LogRecord::m_dropped_before over @p records: the number of drops
/// they report, counted the way a consumer counts them.
std::uint64_t reported_drops(const std::vector<LogRecord>& records) {
    std::uint64_t total = 0;
    for (const auto& r : records) { total += r.m_dropped_before; }
    return total;
}

/// The way a wrapper with its own `...` reaches Queue::vlogf with flags.
rt::Status vlogf_with_flags(rt::Queue& queue,
                            LogLevel level,
                            std::uint32_t flags,
                            const char* group,
                            const char* fmt,
                            ...) {
    va_list args;
    va_start(args, fmt);
    const rt::Status status = queue.vlogf(level, flags, group, fmt, args);
    va_end(args);
    return status;
}

}  // namespace

// ---------------------------------------------------------------------------
// Runtime level filter
// ---------------------------------------------------------------------------

TEST_F(LoggerFixture, RuntimeLevelFiltersSynchronousPath) {
    thl::Logger::set_level(LogLevel::Warning);
    EXPECT_EQ(thl::Logger::get_level(), LogLevel::Warning);
    EXPECT_TRUE(thl::Logger::is_enabled(LogLevel::Error));
    EXPECT_TRUE(thl::Logger::is_enabled(LogLevel::Warning));
    EXPECT_FALSE(thl::Logger::is_enabled(LogLevel::Info));
    EXPECT_FALSE(thl::Logger::is_enabled(LogLevel::Debug));

    thl::Logger::info("test", "dropped");
    thl::Logger::debug("test", "dropped");
    thl::Logger::warning("test", "kept-1");
    thl::Logger::error("test", "kept-2");

    const auto got = records();
    ASSERT_EQ(got.size(), 2U);
    EXPECT_EQ(got[0].m_message, "kept-1");
    EXPECT_EQ(got[1].m_message, "kept-2");
}

TEST_F(LoggerFixture, RuntimeLevelFiltersRealtimePath) {
    thl::Logger::set_level(LogLevel::Error);
    EXPECT_EQ(rt::logf(LogLevel::Warning, "test", "dropped %d", 1), rt::Status::Filtered);
    EXPECT_EQ(rt::logf(LogLevel::Error, "test", "kept %d", 2), rt::Status::Ok);
    ASSERT_TRUE(wait_for_records(1));
    const auto got = rt_records();
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].m_message, "kept 2");
}

// ---------------------------------------------------------------------------
// Real-time path: delivery
// ---------------------------------------------------------------------------

TEST_F(LoggerFixture, RtRecordIsDeliveredThroughCallbackSink) {
    EXPECT_TRUE(rt::is_running());
    const auto status =
        rt::logf(LogLevel::Warning, "audio", "xrun after %d frames at %.1f kHz", 64, 48.0);
    EXPECT_EQ(status, rt::Status::Ok);

    ASSERT_TRUE(wait_for_records(1));
    const auto got = rt_records();
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].m_level, static_cast<std::uint32_t>(LogLevel::Warning));
    EXPECT_EQ(got[0].m_source, "rt");
    EXPECT_EQ(got[0].m_group, "audio");
    EXPECT_EQ(got[0].m_message, "xrun after 64 frames at 48.0 kHz");
    EXPECT_GT(got[0].m_seq, 0U);
    EXPECT_GT(got[0].m_timestamp_ms, 0);
    EXPECT_GT(got[0].m_monotonic_ns, 0U);
}

TEST_F(LoggerFixture, RtLogPreformatted) {
    EXPECT_EQ(rt::log(LogLevel::Info, "audio", "100% literal, no formatting"), rt::Status::Ok);
    ASSERT_TRUE(wait_for_records(1));
    EXPECT_EQ(rt_records()[0].m_message, "100% literal, no formatting");
}

TEST_F(LoggerFixture, RtNullArgumentsAreTolerated) {
    EXPECT_EQ(rt::log(LogLevel::Info, nullptr, nullptr), rt::Status::Ok);
    ASSERT_TRUE(wait_for_records(1));
    const auto got = rt_records();
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].m_group, "");
    EXPECT_EQ(got[0].m_message, "");
}

TEST_F(LoggerFixture, RtPreservesOrderAndSharesSequenceWithSyncPath) {
    thl::Logger::info("sync", "a");
    rt::log(LogLevel::Info, "rt", "b");
    rt::log(LogLevel::Info, "rt", "c");
    ASSERT_TRUE(wait_for_records(3));
    thl::Logger::info("sync", "d");

    const auto got = records();
    ASSERT_EQ(got.size(), 4U);
    for (std::size_t i = 1; i < got.size(); ++i) { EXPECT_GT(got[i].m_seq, got[i - 1].m_seq); }
    EXPECT_EQ(got[1].m_message, "b");
    EXPECT_EQ(got[2].m_message, "c");
}

TEST_F(LoggerFixture, RtTruncatesLongMessagesAndGroups) {
    const std::string long_msg(rt::k_message_capacity * 2, 'm');
    const std::string long_group(rt::k_group_capacity * 2, 'g');
    EXPECT_EQ(rt::logf(LogLevel::Info, long_group.c_str(), "%s", long_msg.c_str()),
              rt::Status::Truncated);
    EXPECT_EQ(rt::log(LogLevel::Info, "g", long_msg.c_str()), rt::Status::Truncated);

    ASSERT_TRUE(wait_for_records(2));
    const auto got = rt_records();
    ASSERT_EQ(got.size(), 2U);
    EXPECT_EQ(got[0].m_message.size(), rt::k_message_capacity - 1);
    EXPECT_EQ(got[0].m_group.size(), rt::k_group_capacity - 1);
    EXPECT_EQ(got[1].m_message.size(), rt::k_message_capacity - 1);
}

// ---------------------------------------------------------------------------
// Real-time path: consumer lifecycle
// ---------------------------------------------------------------------------

TEST_F(LoggerFixture, NoConsumerWhenStoppedAndNotManual) {
    rt::stop();
    EXPECT_FALSE(rt::is_running());
    EXPECT_EQ(rt::logf(LogLevel::Error, "test", "lost"), rt::Status::NoConsumer);
    EXPECT_EQ(rt::drain(), 0U);
    EXPECT_TRUE(rt_records().empty());

    rt::start();
    EXPECT_TRUE(rt::is_running());
    EXPECT_EQ(rt::logf(LogLevel::Error, "test", "found"), rt::Status::Ok);
    ASSERT_TRUE(wait_for_records(1));
    EXPECT_EQ(rt_records()[0].m_message, "found");
}

TEST_F(LoggerFixture, ManualDrainWithoutThread) {
    rt::stop();
    rt::enable_manual_drain(true);
    EXPECT_FALSE(rt::is_running());

    EXPECT_EQ(rt::logf(LogLevel::Info, "test", "queued %d", 1), rt::Status::Ok);
    EXPECT_EQ(rt::logf(LogLevel::Info, "test", "queued %d", 2), rt::Status::Ok);
    EXPECT_TRUE(rt_records().empty()) << "nothing may be delivered before drain()";

    EXPECT_EQ(rt::drain(), 2U);
    const auto got = rt_records();
    ASSERT_EQ(got.size(), 2U);
    EXPECT_EQ(got[0].m_message, "queued 1");
    EXPECT_EQ(got[1].m_message, "queued 2");
    EXPECT_EQ(rt::drain(), 0U);

    rt::enable_manual_drain(false);
    EXPECT_EQ(rt::logf(LogLevel::Info, "test", "lost"), rt::Status::NoConsumer);
    rt::start();
}

TEST_F(LoggerFixture, StopFlushesPendingRecords) {
    rt::stop();
    rt::enable_manual_drain(true);
    rt::log(LogLevel::Info, "test", "pending");
    rt::start();
    rt::stop();  // must deliver what the thread didn't get to
    EXPECT_EQ(rt_records().size(), 1U);
    rt::enable_manual_drain(false);
    rt::start();
}

TEST_F(LoggerFixture, ConfigCanDisableAndReenableDrainThread) {
    auto config = thl::Logger::get_config();
    EXPECT_TRUE(config.m_rt_enabled);
    EXPECT_EQ(config.m_rt_drain_interval_ms, 1U);

    config.m_rt_enabled = false;
    thl::Logger::set_config(config);
    EXPECT_FALSE(rt::is_running());
    EXPECT_EQ(rt::logf(LogLevel::Error, "test", "lost"), rt::Status::NoConsumer);

    // Non-RT logging must not resurrect the thread while it's disabled.
    thl::Logger::info("test", "sync");
    EXPECT_FALSE(rt::is_running());

    config.m_rt_enabled = true;
    config.m_rt_drain_interval_ms = 0;  // clamped to 1
    thl::Logger::set_config(config);
    EXPECT_TRUE(rt::is_running());
    EXPECT_EQ(thl::Logger::get_config().m_rt_drain_interval_ms, 1U);
}

// ---------------------------------------------------------------------------
// Real-time path: overflow
// ---------------------------------------------------------------------------

TEST_F(LoggerFixture, QueueFullDropsAndReportsCountOnTheFirstRecord) {
    rt::stop();
    rt::enable_manual_drain(true);

    std::size_t accepted = 0;
    std::size_t dropped = 0;
    for (std::size_t i = 0; i < rt::k_queue_capacity + 10; ++i) {
        const auto status = rt::logf(LogLevel::Info, "test", "%zu", i);
        if (status == rt::Status::Ok) {
            ++accepted;
        } else if (status == rt::Status::QueueFull) {
            ++dropped;
        }
    }
    EXPECT_EQ(accepted, rt::k_queue_capacity);
    EXPECT_EQ(dropped, 10U);
    EXPECT_EQ(rt::dropped_count(), 10U);

    EXPECT_EQ(rt::drain(), rt::k_queue_capacity);
    EXPECT_EQ(rt::dropped_count(), 0U);

    const auto got = records();
    // capacity RT records and nothing else: the pass had a record to carry the
    // count, so no synthetic warning is dispatched.
    ASSERT_EQ(got.size(), rt::k_queue_capacity);
    EXPECT_EQ(got[0].m_message, "0");
    EXPECT_EQ(got[0].m_dropped_before, 10U);
    EXPECT_EQ(got[0].m_flags & k_flag_realtime, k_flag_realtime);
    EXPECT_EQ(got.back().m_message, std::to_string(rt::k_queue_capacity - 1));
    for (std::size_t i = 1; i < got.size(); ++i) {
        EXPECT_EQ(got[i].m_dropped_before, 0U) << "record " << i;
    }
    for (const auto& r : got) { EXPECT_NE(r.m_group, "thl.logger") << r.m_message; }
    EXPECT_EQ(reported_drops(got), 10U) << "every drop reported exactly once";

    // The ring is reusable after a full/empty cycle, and the count taken by
    // the previous pass is not reported again.
    EXPECT_EQ(rt::logf(LogLevel::Info, "test", "again"), rt::Status::Ok);
    EXPECT_EQ(rt::drain(), 1U);
    const auto after = records();
    ASSERT_EQ(after.size(), rt::k_queue_capacity + 1);
    EXPECT_EQ(after.back().m_message, "again");
    EXPECT_EQ(after.back().m_dropped_before, 0U);
    EXPECT_EQ(reported_drops(after), 10U);

    rt::enable_manual_drain(false);
    rt::start();
}

TEST_F(LoggerFixture, DrainWithNothingToDeliverReportsDropsOnOneSyntheticWarning) {
    rt::stop();  // only this thread dispatches below
    rt::Queue queue(2);
    ASSERT_EQ(queue.capacity(), 2U);

    // A pass takes the drop counter before it pops, so drops that happen while
    // the pass runs are invisible to it. Produce them from inside the callback
    // sink, i.e. from within queue.drain(): the pass then delivers every record
    // (none carries a count) and the *next* pass finds the count but no record.
    std::vector<LogRecord> got;
    bool injected = false;
    thl::Logger::set_callback([&](const LogRecord& record) {
        got.push_back(record);
        if (injected) { return; }
        injected = true;
        // The record being delivered freed one slot: fill it, then overflow.
        EXPECT_EQ(queue.log(LogLevel::Info, "owned", "in-flight"), rt::Status::Ok);
        EXPECT_EQ(queue.log(LogLevel::Info, "owned", "lost-1"), rt::Status::QueueFull);
        EXPECT_EQ(queue.log(LogLevel::Info, "owned", "lost-2"), rt::Status::QueueFull);
    });

    EXPECT_EQ(queue.log(LogLevel::Info, "owned", "a"), rt::Status::Ok);
    EXPECT_EQ(queue.log(LogLevel::Info, "owned", "b"), rt::Status::Ok);
    EXPECT_EQ(queue.drain(), 3U);
    ASSERT_EQ(got.size(), 3U);
    EXPECT_EQ(got[0].m_message, "a");
    EXPECT_EQ(got[1].m_message, "b");
    EXPECT_EQ(got[2].m_message, "in-flight");
    EXPECT_EQ(reported_drops(got), 0U) << "the drops happened after the pass took the counter";
    EXPECT_EQ(queue.dropped_count(), 2U);

    // Nothing queued: the count goes out on one synthetic warning.
    EXPECT_EQ(queue.drain(), 0U) << "the synthetic record is not a delivered record";
    ASSERT_EQ(got.size(), 4U);
    const auto& report = got.back();
    EXPECT_EQ(report.m_level, static_cast<std::uint32_t>(LogLevel::Warning));
    EXPECT_EQ(report.m_source, "rt");
    EXPECT_EQ(report.m_group, "thl.logger");
    EXPECT_EQ(report.m_message, "real-time log queue overflowed");
    EXPECT_EQ(report.m_dropped_before, 2U);
    EXPECT_EQ(report.m_flags, k_flag_realtime);
    EXPECT_EQ(queue.dropped_count(), 0U);

    // Reported once: a further pass is silent.
    EXPECT_EQ(queue.drain(), 0U);
    EXPECT_EQ(got.size(), 4U);
    EXPECT_EQ(reported_drops(got), 2U);

    thl::Logger::clear_callback();
    rt::start();
}

// ---------------------------------------------------------------------------
// Real-time path: concurrency
// ---------------------------------------------------------------------------

TEST_F(LoggerFixture, ManyProducersNoLossNoDuplicates) {
    constexpr int k_threads = 4;
    constexpr int k_per_thread = 2000;

    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> retries{0};  // every QueueFull is one counted drop
    std::vector<std::thread> producers;
    producers.reserve(k_threads);
    for (int t = 0; t < k_threads; ++t) {
        producers.emplace_back([&, t]() {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < k_per_thread; ++i) {
                // Spin (never block) if the drain thread is behind.
                while (rt::logf(LogLevel::Info, "p", "%d:%d", t, i) == rt::Status::QueueFull) {
                    retries.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                }
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& p : producers) { p.join(); }

    ASSERT_TRUE(wait_for_records(k_threads * k_per_thread, std::chrono::seconds(10)));
    // Join the drain thread: a pass may still be delivering the record that
    // carries the last drop count.
    rt::stop();
    // Producers that saw QueueFull retried, so those count as drops; the
    // drain reports each of them exactly once, on whatever record it had.
    EXPECT_EQ(reported_drops(rt_records()), retries.load(std::memory_order_relaxed));
    const auto got = rt_records("p");
    ASSERT_EQ(got.size(), static_cast<std::size_t>(k_threads * k_per_thread));

    // Every (thread, index) pair exactly once, and per-thread order preserved.
    std::array<int, k_threads> next{};
    for (const auto& r : got) {
        const auto colon = r.m_message.find(':');
        ASSERT_NE(colon, std::string::npos);
        const int t = std::stoi(r.m_message.substr(0, colon));
        const int i = std::stoi(r.m_message.substr(colon + 1));
        ASSERT_GE(t, 0);
        ASSERT_LT(t, k_threads);
        EXPECT_EQ(i, next[static_cast<std::size_t>(t)]) << "thread " << t;
        ++next[static_cast<std::size_t>(t)];
    }
    for (int t = 0; t < k_threads; ++t) {
        EXPECT_EQ(next[static_cast<std::size_t>(t)], k_per_thread);
    }
    rt::start();
}

TEST_F(LoggerFixture, StartStopWhileProducing) {
    std::atomic<bool> stop{false};
    std::thread producer([&]() {
        int i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            rt::logf(LogLevel::Debug, "churn", "%d", i++);
        }
    });
    for (int i = 0; i < 20; ++i) {
        rt::stop();
        rt::start();
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    EXPECT_TRUE(rt::is_running());
    // No crash, no deadlock; records delivered are well-formed. The producer
    // outruns a stopped consumer, so the drain legitimately reports the
    // resulting drops — on the first "churn" record of a pass, or on its own
    // "thl.logger" record when a pass had nothing else to deliver. Everything
    // else must be the producer's payload.
    for (const auto& r : rt_records()) {
        EXPECT_EQ(r.m_flags & k_flag_realtime, k_flag_realtime);
        if (r.m_group == "thl.logger") {
            EXPECT_GT(r.m_dropped_before, 0U) << r.m_message;
            EXPECT_EQ(r.m_message, "real-time log queue overflowed");
            continue;
        }
        EXPECT_EQ(r.m_group, "churn");
    }
}

// ---------------------------------------------------------------------------
// Real-time safety (enforced under -fsanitize=realtime; otherwise a plain call)
// ---------------------------------------------------------------------------

namespace {

void audio_callback_that_logs(int block) TANH_NONBLOCKING_FUNCTION {
    (void)rt::logf(LogLevel::Warning,
                   "audio",
                   "block %d: %zu missing, gain %.2f",
                   block,
                   static_cast<size_t>(480),
                   0.5);
    (void)rt::log(LogLevel::Info, "audio", "literal");
    (void)rt::logf(LogLevel::Error, k_flag_contract_violation, "audio", "flagged %d", block);
    (void)rt::log(LogLevel::Info, k_flag_contract_violation | k_consumer_bit, "audio", "flagged");
    (void)thl::Logger::is_enabled(LogLevel::Debug);
    (void)rt::dropped_count();
    (void)rt::is_running();
}

}  // namespace

TEST_F(LoggerFixture, RtLogIsNonblocking) {
    for (int i = 0; i < 100; ++i) { audio_callback_that_logs(i); }
    ASSERT_TRUE(wait_for_records(400));
    EXPECT_EQ(rt_records().size(), 400U);
}

TEST_F(LoggerFixture, RtLogIsNonblockingEvenWhenQueueFull) {
    rt::stop();
    rt::enable_manual_drain(true);
    for (std::size_t i = 0; i < rt::k_queue_capacity + 5; ++i) { audio_callback_that_logs(0); }
    EXPECT_GT(rt::dropped_count(), 0U);
    rt::drain();
    rt::enable_manual_drain(false);
    rt::start();
}

// ---------------------------------------------------------------------------
// Convenience macros
// ---------------------------------------------------------------------------

TEST_F(LoggerFixture, MacrosForwardToSyncAndRtPaths) {
    THL_LOG_WARNING("macro", "sync %d", 1);
    THL_LOG_RT_ERROR("macro", "rt %s", "two");
    ASSERT_TRUE(wait_for_records(2));
    const auto got = records();
    ASSERT_EQ(got.size(), 2U);
    EXPECT_EQ(got[0].m_message, "sync 1");
    EXPECT_EQ(got[0].m_group, "macro");
    EXPECT_EQ(got[0].m_level, static_cast<std::uint32_t>(LogLevel::Warning));
    EXPECT_EQ(got[0].m_source, "native");
    EXPECT_EQ(got[1].m_message, "rt two");
    EXPECT_EQ(got[1].m_level, static_cast<std::uint32_t>(LogLevel::Error));
    EXPECT_EQ(got[1].m_source, "rt");
}

TEST_F(LoggerFixture, MacrosRespectRuntimeLevel) {
    thl::Logger::set_level(LogLevel::Error);
    THL_LOG_INFO("macro", "filtered");
    THL_LOG_RT_WARNING("macro", "filtered too");
    THL_LOG_ERROR("macro", "kept");
    ASSERT_TRUE(wait_for_records(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto got = records();
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].m_message, "kept");
}

TEST_F(LoggerFixture, SyncMacroDoesNotEvaluateArgumentsWhenFiltered) {
    thl::Logger::set_level(LogLevel::Error);
    int evaluations = 0;
    THL_LOG_INFO("macro", "%d", ++evaluations);
    THL_LOG_ERROR("macro", "%d", ++evaluations);
    ASSERT_TRUE(wait_for_records(1));
    EXPECT_EQ(evaluations, 1);
    EXPECT_EQ(records()[0].m_message, "1");
}

// ---------------------------------------------------------------------------
// Owned queues and drain threads
// ---------------------------------------------------------------------------

TEST_F(LoggerFixture, OwnedQueueDeliversOnDrain) {
    rt::Queue queue(100);
    EXPECT_EQ(queue.capacity(), 128U);  // rounded up
    EXPECT_TRUE(queue.is_accepting());
    EXPECT_EQ(queue.logf(LogLevel::Info, "owned", "value %d", 7), rt::Status::Ok);
    EXPECT_EQ(queue.log(LogLevel::Warning, "owned", "plain"), rt::Status::Ok);
    EXPECT_TRUE(rt_records().empty());  // nothing until someone drains
    EXPECT_EQ(queue.drain(), 2U);
    const auto got = rt_records("owned");
    ASSERT_EQ(got.size(), 2U);
    EXPECT_EQ(got[0].m_message, "value 7");
    EXPECT_EQ(got[1].m_message, "plain");
    EXPECT_EQ(got[1].m_level, static_cast<std::uint32_t>(LogLevel::Warning));
}

TEST_F(LoggerFixture, OwnedQueueCountsDropsAndReportsThemOnce) {
    rt::Queue queue(4);
    for (int i = 0; i < 6; ++i) { queue.logf(LogLevel::Info, "owned", "%d", i); }
    EXPECT_EQ(queue.dropped_count(), 2U);
    EXPECT_EQ(queue.drain(), 4U);
    EXPECT_EQ(queue.dropped_count(), 0U);
    auto got = rt_records("owned");
    ASSERT_EQ(got.size(), 4U);
    EXPECT_EQ(got[0].m_message, "0");
    EXPECT_EQ(got[0].m_dropped_before, 2U) << "the first record of the pass carries the count";
    for (std::size_t i = 1; i < got.size(); ++i) { EXPECT_EQ(got[i].m_dropped_before, 0U); }
    EXPECT_TRUE(rt_records("thl.logger").empty()) << "no synthetic warning beside a carrier";

    // The next pass starts from zero.
    queue.logf(LogLevel::Info, "owned", "later");
    EXPECT_EQ(queue.drain(), 1U);
    got = rt_records("owned");
    ASSERT_EQ(got.size(), 5U);
    EXPECT_EQ(got.back().m_dropped_before, 0U);
    EXPECT_EQ(reported_drops(records()), 2U);
}

TEST_F(LoggerFixture, OwnedQueueNotAcceptingReturnsNoConsumer) {
    rt::Queue queue(8);
    queue.set_accepting(false);
    EXPECT_EQ(queue.logf(LogLevel::Error, "owned", "lost"), rt::Status::NoConsumer);
    EXPECT_EQ(queue.drain(), 0U);
    queue.set_accepting(true);
    EXPECT_EQ(queue.logf(LogLevel::Error, "owned", "kept"), rt::Status::Ok);
    EXPECT_EQ(queue.drain(), 1U);
}

TEST_F(LoggerFixture, OwnedQueueIsIndependentOfTheDefaultOne) {
    rt::stop();  // default consumer gone ...
    rt::Queue queue(8);
    EXPECT_EQ(queue.logf(LogLevel::Error, "owned", "still fine"), rt::Status::Ok);  // ... ours is
                                                                                    // not
    EXPECT_EQ(rt::logf(LogLevel::Error, "default", "lost"), rt::Status::NoConsumer);
    EXPECT_EQ(queue.drain(), 1U);
    rt::start();
}

TEST_F(LoggerFixture, DrainThreadDeliversAndStopsOnDestruction) {
    rt::Queue queue(16);
    {
        rt::DrainThread::Options options;
        options.m_interval_ms = 1;
        rt::DrainThread drain(queue, options);
        EXPECT_TRUE(drain.is_running());
        EXPECT_FALSE(drain.is_current_thread());
        queue.logf(LogLevel::Info, "owned", "from thread");
        ASSERT_TRUE(wait_for_records(1));
        // Queued right before destruction: the destructor's final drain delivers it.
        queue.logf(LogLevel::Info, "owned", "last one");
    }
    const auto got = rt_records("owned");
    ASSERT_EQ(got.size(), 2U);
    EXPECT_EQ(got[1].m_message, "last one");
}

TEST_F(LoggerFixture, SequenceNumbersInterleaveAcrossQueues) {
    rt::Queue a(8);
    rt::Queue b(8);
    a.logf(LogLevel::Info, "seq", "a1");
    b.logf(LogLevel::Info, "seq", "b1");
    thl::Logger::info("seq", "sync");
    a.logf(LogLevel::Info, "seq", "a2");
    b.drain();
    a.drain();
    auto got = records();
    std::sort(got.begin(), got.end(), [](const LogRecord& x, const LogRecord& y) {
        return x.m_seq < y.m_seq;
    });
    std::vector<std::string> messages;
    for (const auto& r : got) {
        if (r.m_group == "seq") { messages.push_back(r.m_message); }
    }
    EXPECT_EQ(messages, (std::vector<std::string>{"a1", "b1", "sync", "a2"}));
}

TEST_F(LoggerFixture, DefaultDrainThreadIsNeverStartedImplicitly) {
    rt::stop();
    EXPECT_FALSE(rt::is_running());
    thl::Logger::info("test", "a synchronous record");
    thl::Logger::set_callback([](const LogRecord&) {});
    EXPECT_FALSE(rt::is_running());
    EXPECT_EQ(rt::logf(LogLevel::Error, "test", "lost"), rt::Status::NoConsumer);
    rt::start();
    EXPECT_TRUE(rt::is_running());
}

TEST_F(LoggerFixture, StopLeavesRtEnabledConfigAlone) {
    EXPECT_TRUE(thl::Logger::get_config().m_rt_enabled);
    rt::stop();
    EXPECT_TRUE(thl::Logger::get_config().m_rt_enabled);
    rt::start();
}

// ---------------------------------------------------------------------------
// Record flags
// ---------------------------------------------------------------------------

TEST(LoggerRecord, FlagsAndDropCountDefaultToZero) {
    static_assert(k_flag_realtime == 1U);
    static_assert(k_flag_contract_violation == 2U);
    static_assert((k_flag_realtime & k_flag_contract_violation) == 0U);
    const LogRecord record{};
    EXPECT_EQ(record.m_flags, 0U);
    EXPECT_EQ(record.m_dropped_before, 0U);
}

TEST_F(LoggerFixture, SyncFlagsReachTheCallbackSink) {
    thl::Logger::log_with_source(LogLevel::Warning,
                                 k_flag_contract_violation,
                                 "api",
                                 "flags",
                                 "misuse");
    thl::Logger::logf(LogLevel::Error,
                      k_flag_contract_violation | k_consumer_bit,
                      "flags",
                      "value %d",
                      7);
    thl::Logger::logf(LogLevel::Info, k_consumer_bit, "flags", nullptr);
    thl::Logger::log_with_source(LogLevel::Info, "api", "flags", "plain");
    thl::Logger::logf(LogLevel::Info, "flags", "plain %d", 1);
    thl::Logger::info("flags", "shorthand");
    THL_LOG_INFO("flags", "macro");

    const auto got = records();
    ASSERT_EQ(got.size(), 7U);
    EXPECT_EQ(got[0].m_flags, k_flag_contract_violation);
    EXPECT_EQ(got[0].m_source, "api");
    EXPECT_EQ(got[0].m_message, "misuse");
    EXPECT_EQ(got[0].m_level, static_cast<std::uint32_t>(LogLevel::Warning));
    EXPECT_EQ(got[1].m_flags, k_flag_contract_violation | k_consumer_bit);
    EXPECT_EQ(got[1].m_source, "native");
    EXPECT_EQ(got[1].m_message, "value 7");
    EXPECT_EQ(got[2].m_flags, k_consumer_bit);
    EXPECT_EQ(got[2].m_message, "");
    for (std::size_t i = 3; i < got.size(); ++i) {
        EXPECT_EQ(got[i].m_flags, 0U) << "existing overloads pass no flags: " << got[i].m_message;
    }
    for (const auto& r : got) {
        EXPECT_EQ(r.m_flags & k_flag_realtime, 0U) << "never set on the synchronous path";
        EXPECT_EQ(r.m_dropped_before, 0U);
    }
}

TEST_F(LoggerFixture, RtFlagsReachTheCallbackSinkWithRealtimeSet) {
    rt::stop();
    rt::enable_manual_drain(true);

    EXPECT_EQ(rt::logf(LogLevel::Warning, k_flag_contract_violation, "flags", "misuse %d", 1),
              rt::Status::Ok);
    EXPECT_EQ(rt::log(LogLevel::Error, k_consumer_bit, "flags", "consumer bit"), rt::Status::Ok);
    EXPECT_EQ(rt::logf(LogLevel::Info, "flags", "plain %d", 2), rt::Status::Ok);
    EXPECT_EQ(rt::log(LogLevel::Info, "flags", "plain"), rt::Status::Ok);
    THL_LOG_RT_INFO("flags", "macro");

    EXPECT_EQ(rt::drain(), 5U);
    const auto got = rt_records("flags");
    ASSERT_EQ(got.size(), 5U);
    EXPECT_EQ(got[0].m_flags, k_flag_contract_violation | k_flag_realtime);
    EXPECT_EQ(got[0].m_message, "misuse 1");
    EXPECT_EQ(got[1].m_flags, k_consumer_bit | k_flag_realtime);
    EXPECT_EQ(got[1].m_message, "consumer bit");
    for (std::size_t i = 2; i < got.size(); ++i) {
        EXPECT_EQ(got[i].m_flags, k_flag_realtime) << "drain marks what it dispatches";
    }
    for (const auto& r : got) {
        EXPECT_EQ(r.m_source, "rt");
        EXPECT_EQ(r.m_dropped_before, 0U);
    }

    rt::enable_manual_drain(false);
    rt::start();
}

TEST_F(LoggerFixture, OwnedQueueCarriesFlagsThroughEveryOverload) {
    rt::Queue queue(8);
    EXPECT_EQ(queue.logf(LogLevel::Info, k_flag_contract_violation, "owned", "%d", 1),
              rt::Status::Ok);
    EXPECT_EQ(queue.log(LogLevel::Info, k_consumer_bit, "owned", "two"), rt::Status::Ok);
    EXPECT_EQ(vlogf_with_flags(queue,
                               LogLevel::Info,
                               k_flag_contract_violation | k_consumer_bit,
                               "owned",
                               "%s",
                               "three"),
              rt::Status::Ok);
    EXPECT_EQ(queue.logf(LogLevel::Info, "owned", "%d", 4), rt::Status::Ok);
    EXPECT_EQ(queue.log(LogLevel::Info, "owned", "five"), rt::Status::Ok);
    EXPECT_EQ(queue.drain(), 5U);

    const auto got = rt_records("owned");
    ASSERT_EQ(got.size(), 5U);
    EXPECT_EQ(got[0].m_flags, k_flag_contract_violation | k_flag_realtime);
    EXPECT_EQ(got[0].m_message, "1");
    EXPECT_EQ(got[1].m_flags, k_consumer_bit | k_flag_realtime);
    EXPECT_EQ(got[1].m_message, "two");
    EXPECT_EQ(got[2].m_flags, k_flag_contract_violation | k_consumer_bit | k_flag_realtime);
    EXPECT_EQ(got[2].m_message, "three");
    EXPECT_EQ(got[3].m_flags, k_flag_realtime);
    EXPECT_EQ(got[4].m_flags, k_flag_realtime);
}

TEST_F(LoggerFixture, FlagsSurviveTruncationAndTheLevelFilter) {
    rt::Queue queue(8);
    const std::string long_msg(rt::k_message_capacity * 2, 'm');
    EXPECT_EQ(queue.log(LogLevel::Info, k_flag_contract_violation, "owned", long_msg.c_str()),
              rt::Status::Truncated);
    thl::Logger::set_level(LogLevel::Error);
    EXPECT_EQ(queue.log(LogLevel::Info, k_flag_contract_violation, "owned", "filtered"),
              rt::Status::Filtered);
    thl::Logger::logf(LogLevel::Info, k_flag_contract_violation, "owned", "filtered");
    EXPECT_EQ(queue.drain(), 1U);
    const auto got = records();
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].m_flags, k_flag_contract_violation | k_flag_realtime);
    EXPECT_EQ(got[0].m_message.size(), rt::k_message_capacity - 1);
}

// ---------------------------------------------------------------------------
// Drop count rendering
// ---------------------------------------------------------------------------

TEST(LoggerFormat, DropCountIsRenderedOnlyWhenARecordCarriesOne) {
    LogRecord record;
    record.m_level = static_cast<std::uint32_t>(LogLevel::Warning);
    record.m_source = "rt";
    record.m_group = "audio";
    record.m_message = "xrun";

    EXPECT_EQ(thl::Logger::format_plain(record), "[warn][rt][audio] xrun");
    std::string logfmt = thl::Logger::format_logfmt(record);
    EXPECT_EQ(logfmt.find("dropped_before"), std::string::npos) << logfmt;
    EXPECT_EQ(logfmt.substr(logfmt.size() - 12), "message=xrun") << logfmt;

    record.m_dropped_before = 3;
    EXPECT_EQ(thl::Logger::format_plain(record),
              "[warn][rt][audio] xrun [3 real-time log message(s) dropped before this record]");
    logfmt = thl::Logger::format_logfmt(record);
    EXPECT_EQ(logfmt.substr(logfmt.size() - 29), "message=xrun dropped_before=3") << logfmt;

    // Flags are not rendered by the text formatters.
    record.m_flags = k_flag_realtime | k_flag_contract_violation;
    EXPECT_EQ(thl::Logger::format_plain(record),
              "[warn][rt][audio] xrun [3 real-time log message(s) dropped before this record]");
    EXPECT_EQ(thl::Logger::format_logfmt(record), logfmt);
}

// ---------------------------------------------------------------------------
// Platform sink identity
// ---------------------------------------------------------------------------

TEST_F(LoggerFixture, PlatformIdentityDefaultsAndRoundTripsThroughConfig) {
    const thl::Logger::LoggerConfig defaults;
    EXPECT_EQ(defaults.m_platform_tag, "thl");
    EXPECT_EQ(defaults.m_platform_subsystem, "thl");
    EXPECT_EQ(defaults.m_platform_category, "logger");

    auto config = thl::Logger::get_config();
    EXPECT_EQ(config.m_platform_tag, "thl") << "the fixture's config left the defaults in place";
    EXPECT_EQ(config.m_platform_subsystem, "thl");
    EXPECT_EQ(config.m_platform_category, "logger");

    config.m_platform_tag = "anira";
    config.m_platform_subsystem = "org.anira";
    config.m_platform_category = "inference";
    thl::Logger::set_config(config);
    auto back = thl::Logger::get_config();
    EXPECT_EQ(back.m_platform_tag, "anira");
    EXPECT_EQ(back.m_platform_subsystem, "org.anira");
    EXPECT_EQ(back.m_platform_category, "inference");
    EXPECT_TRUE(back.m_rt_enabled) << "the rest of the config is untouched";
    EXPECT_EQ(back.m_rt_drain_interval_ms, 1U);

    // An empty string selects the default.
    config.m_platform_tag.clear();
    config.m_platform_subsystem.clear();
    config.m_platform_category.clear();
    thl::Logger::set_config(config);
    back = thl::Logger::get_config();
    EXPECT_EQ(back.m_platform_tag, "thl");
    EXPECT_EQ(back.m_platform_subsystem, "thl");
    EXPECT_EQ(back.m_platform_category, "logger");
}

TEST_F(LoggerFixture, PlatformSinkFilesRecordsUnderTheConfiguredIdentity) {
    // What the platform sink does with the identity is only observable on the
    // device (logcat tag, os_log subsystem/category); here the sink runs with
    // a custom identity, then with a changed one — on Apple platforms the
    // second record creates a new os_log_t — and every record must still
    // reach the callback sink. On Linux without journald and on Windows the
    // platform sink is plain stdout/stderr and ignores the identity.
    auto config = thl::Logger::get_config();
    config.m_platform_enabled = true;
    config.m_platform_tag = "tanh-test";
    config.m_platform_subsystem = "lab.tanh.test";
    config.m_platform_category = "identity";
    thl::Logger::set_config(config);
    thl::Logger::info("identity", "filed under lab.tanh.test/identity (tag tanh-test)");

    config.m_platform_category = "identity-changed";
    thl::Logger::set_config(config);
    thl::Logger::info("identity", "filed under lab.tanh.test/identity-changed");

    config.m_platform_enabled = false;
    thl::Logger::set_config(config);
    thl::Logger::info("identity", "platform sink off again");

    const auto got = records();
    ASSERT_EQ(got.size(), 3U);
    EXPECT_EQ(got[0].m_group, "identity");
    EXPECT_EQ(got[2].m_message, "platform sink off again");
    EXPECT_EQ(thl::Logger::get_config().m_platform_category, "identity-changed");
}
