#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tanh/core/Logger.h"
#include "tanh/utils/RealtimeSanitizer.h"

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

TEST_F(LoggerFixture, QueueFullDropsAndReportsCount) {
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
    // capacity RT records + one synthetic "dropped" warning.
    ASSERT_EQ(got.size(), rt::k_queue_capacity + 1);
    EXPECT_EQ(got[0].m_message, "0");
    EXPECT_EQ(got[rt::k_queue_capacity - 1].m_message, std::to_string(rt::k_queue_capacity - 1));
    const auto& report = got.back();
    EXPECT_EQ(report.m_level, static_cast<std::uint32_t>(LogLevel::Warning));
    EXPECT_EQ(report.m_group, "thl.logger");
    EXPECT_NE(report.m_message.find("10 real-time log message(s) dropped"), std::string::npos)
        << report.m_message;

    // The ring is reusable after a full/empty cycle.
    EXPECT_EQ(rt::logf(LogLevel::Info, "test", "again"), rt::Status::Ok);
    EXPECT_EQ(rt::drain(), 1U);

    rt::enable_manual_drain(false);
    rt::start();
}

// ---------------------------------------------------------------------------
// Real-time path: concurrency
// ---------------------------------------------------------------------------

TEST_F(LoggerFixture, ManyProducersNoLossNoDuplicates) {
    constexpr int k_threads = 4;
    constexpr int k_per_thread = 2000;

    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    producers.reserve(k_threads);
    for (int t = 0; t < k_threads; ++t) {
        producers.emplace_back([&, t]() {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < k_per_thread; ++i) {
                // Spin (never block) if the drain thread is behind.
                while (rt::logf(LogLevel::Info, "p", "%d:%d", t, i) == rt::Status::QueueFull) {
                    std::this_thread::yield();
                }
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& p : producers) { p.join(); }

    ASSERT_TRUE(wait_for_records(k_threads * k_per_thread, std::chrono::seconds(10)));
    rt::drain();
    // Producers that saw QueueFull retried, so those count as drops and the
    // drain reports them; only look at the payload records here.
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
    // No crash, no deadlock; records delivered are well-formed.
    for (const auto& r : rt_records()) { EXPECT_EQ(r.m_group, "churn"); }
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
    (void)thl::Logger::is_enabled(LogLevel::Debug);
    (void)rt::dropped_count();
    (void)rt::is_running();
}

}  // namespace

TEST_F(LoggerFixture, RtLogIsNonblocking) {
    for (int i = 0; i < 100; ++i) { audio_callback_that_logs(i); }
    ASSERT_TRUE(wait_for_records(200));
    EXPECT_EQ(rt_records().size(), 200U);
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

TEST_F(LoggerFixture, OwnedQueueCountsDropsAndReportsThem) {
    rt::Queue queue(4);
    for (int i = 0; i < 6; ++i) { queue.logf(LogLevel::Info, "owned", "%d", i); }
    EXPECT_EQ(queue.dropped_count(), 2U);
    EXPECT_EQ(queue.drain(), 4U);
    EXPECT_EQ(queue.dropped_count(), 0U);
    const auto all = records();
    ASSERT_FALSE(all.empty());
    EXPECT_NE(all.back().m_message.find("2 real-time log message(s) dropped"), std::string::npos);
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
