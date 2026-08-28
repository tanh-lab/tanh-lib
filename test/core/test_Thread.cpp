// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "tanh/core/threading/Thread.h"

using thl::core::Thread;
using thl::core::ThreadOptions;
using thl::core::ThreadPriority;

namespace {

bool wait_until(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() > deadline) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

TEST(Thread, RunsBodyUntilStopRequested) {
    std::atomic<bool> entered{false};
    std::atomic<bool> left{false};
    Thread thread;
    EXPECT_FALSE(thread.is_running());
    ASSERT_TRUE(thread.start({}, [&](const Thread& self) {
        entered = true;
        while (!self.should_stop()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        left = true;
    }));
    EXPECT_TRUE(thread.is_running());
    EXPECT_TRUE(thread.joinable());
    ASSERT_TRUE(wait_until(entered));
    EXPECT_FALSE(left);
    thread.request_stop();
    thread.join();
    EXPECT_TRUE(left);
    EXPECT_FALSE(thread.is_running());
    EXPECT_FALSE(thread.joinable());
}

TEST(Thread, DestructorStopsAndJoins) {
    std::atomic<bool> left{false};
    {
        Thread thread;
        thread.start({}, [&](const Thread& self) {
            while (!self.should_stop()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            left = true;
        });
    }
    EXPECT_TRUE(left);
}

TEST(Thread, StartTwiceIsRefused) {
    Thread thread;
    ASSERT_TRUE(thread.start({}, [](const Thread& self) {
        while (!self.should_stop()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }));
    EXPECT_FALSE(thread.start({}, [](const Thread&) {}));
}

TEST(Thread, IsRunningClearsWhenBodyReturns) {
    std::atomic<bool> done{false};
    Thread thread;
    thread.start({}, [&](const Thread&) { done = true; });
    ASSERT_TRUE(wait_until(done));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (thread.is_running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(thread.is_running());
    EXPECT_TRUE(thread.joinable());  // still needs a join; the destructor does it
}

// A priority is a request: whatever the platform grants, the thread must run.
TEST(Thread, EveryPriorityRunsTheBody) {
    for (const auto priority : {ThreadPriority::Background,
                                ThreadPriority::Low,
                                ThreadPriority::Normal,
                                ThreadPriority::High,
                                ThreadPriority::RealTime}) {
        std::atomic<bool> ran{false};
        Thread thread;
        ThreadOptions options;
        options.m_priority = priority;
        options.m_name = "thl-test";
        ASSERT_TRUE(thread.start(options, [&](const Thread&) { ran = true; }));
        EXPECT_TRUE(wait_until(ran));
    }
}

TEST(Thread, CurrentThreadHelpersDoNotThrow) {
    Thread::set_current_name("thl-test-main");
    // Low and Normal are grantable without privileges everywhere we support.
    Thread::set_current_priority(ThreadPriority::Low);
    Thread::set_current_priority(ThreadPriority::Normal);
    EXPECT_FALSE(Thread::set_current_name(nullptr));
}
