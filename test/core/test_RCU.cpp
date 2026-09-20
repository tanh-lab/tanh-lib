#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "tanh/core/threading/RCU.h"

using namespace thl;

TEST(RCU, BasicFunctionality) {
    RCU<std::map<std::string, int>> rcu_map;

    // Add some initial data
    rcu_map.update([](auto& map) {
        map["key1"] = 100;
        map["key2"] = 200;
    });

    // Test reading
    auto value = rcu_map.read([](const auto& map) {
        auto it = map.find("key1");
        return (it != map.end()) ? it->second : -1;
    });
    EXPECT_EQ(value, 100);
}

TEST(RCU, VectorFunctionality) {
    RCU<std::vector<int>> rcu_vec;

    // Add some initial data
    rcu_vec.update([](auto& vec) {
        vec.push_back(1);
        vec.push_back(2);
        vec.push_back(3);
    });

    // Test reading
    auto value = rcu_vec.read([](const auto& vec) { return (vec.size() == 3) ? vec[0] : -1; });
    EXPECT_EQ(value, 1);
}

TEST(RCU, MapFunctionality) {
    RCU<std::map<std::string, int>> rcu_map;

    // Add some initial data
    rcu_map.update([](auto& map) {
        map["key1"] = 100;
        map["key2"] = 200;
    });

    // Test lock-free reading
    auto value = rcu_map.read([](const auto& map) {
        auto it = map.find("key1");
        return (it != map.end()) ? it->second : -1;
    });

    EXPECT_EQ(value, 100);

    // Test updating while reading from multiple threads
    std::atomic<bool> running{true};
    std::atomic<int> read_count{0};
    std::atomic<int> errors{0};

    // Start reader threads
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&]() {
            while (running.load()) {
                auto val = rcu_map.read([](const auto& map) {
                    auto it = map.find("key1");
                    return (it != map.end()) ? it->second : -1;
                });

                if (val < 100) { errors.fetch_add(1); }
                read_count.fetch_add(1);

                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });
    }

    // Start writer thread
    std::thread writer([&]() {
        for (int i = 0; i < 100; ++i) {
            rcu_map.update([i](auto& map) {
                map["key1"] = 100 + i;
                map["new_key_" + std::to_string(i)] = i;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Let it run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    running.store(false);

    // Join all threads
    writer.join();
    for (auto& reader : readers) { reader.join(); }

    // Check for errors
    EXPECT_EQ(errors.load(), 0);

    // Check final state
    auto final_size = rcu_map.read([](const auto& map) { return map.size(); });
    EXPECT_EQ(final_size, 102);  // 100 updates + 2 initial keys
}

TEST(RCU, VectorFunctionalityBig) {
    RCU<std::vector<int>> rcu_vec;

    // Add some initial data
    rcu_vec.update([](auto& vec) {
        vec.push_back(1);
        vec.push_back(2);
        vec.push_back(3);
    });

    std::atomic<bool> running{true};
    std::atomic<int> notifications{0};

    // Simulate notification threads (like audio callbacks)
    std::vector<std::thread> notifiers;
    notifiers.reserve(3);
    for (int i = 0; i < 3; ++i) {
        notifiers.emplace_back([&]() {
            while (running.load()) {
                // Simulate notifying all listeners (lock-free read)
                rcu_vec.read([&](const auto& listeners) {
                    for (auto listener_id : listeners) {
                        // Simulate calling listener
                        notifications.fetch_add(1);
                    }
                });
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }

    // Simulate adding/removing listeners from UI thread
    std::thread ui_thread([&]() {
        for (int i = 0; i < 50; ++i) {
            // Add listener
            rcu_vec.update([i](auto& vec) { vec.push_back(100 + i); });

            std::this_thread::sleep_for(std::chrono::milliseconds(2));

            // Remove a listener
            if (i % 5 == 0) {
                rcu_vec.update([](auto& vec) {
                    if (!vec.empty()) { vec.pop_back(); }
                });
            }
        }
    });

    // Let it run
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    running.store(false);

    // Join threads
    ui_thread.join();
    for (auto& notifier : notifiers) { notifier.join(); }

    // Check notifications count
    EXPECT_GT(notifications.load(), 0);
    auto final_vec_size = rcu_vec.read([](const auto& vec) { return vec.size(); });
    EXPECT_EQ(final_vec_size, 43);  // Initial 3 + 50 adds + 10 removes
}

namespace {

// Every version gets a unique sequence number; its destructor marks a side
// table that is never freed. A reader can therefore tell that the version it
// holds has been destroyed without touching freed memory to find out, and
// without depending on a crash or on the allocator reusing the block.
constexpr uint64_t k_version_table_size = uint64_t{1} << 22;

struct VersionTable {
    std::unique_ptr<std::atomic<uint8_t>[]> m_destroyed =
        std::make_unique<std::atomic<uint8_t>[]>(k_version_table_size);
    std::atomic<uint64_t> m_next_seq{1};
};

VersionTable& version_table() {
    static VersionTable table;
    return table;
}

struct TrackedVersion {
    uint64_t m_seq = version_table().m_next_seq.fetch_add(1);

    TrackedVersion() = default;
    TrackedVersion(const TrackedVersion&) {}
    TrackedVersion& operator=(const TrackedVersion&) = delete;
    ~TrackedVersion() {
        if (m_seq < k_version_table_size) { version_table().m_destroyed[m_seq].store(1); }
    }
};

}  // namespace

// Regression: a version must never be reclaimed while a reader is still inside
// the read scope that handed it out. The reader publishes its generation and
// then loads the data pointer; the writer publishes the pointer and then reads
// the generations. With release / acquire orderings neither store was
// guaranteed visible to the other side's load, so the writer could read
// generation 0, free the version, and leave the reader holding freed memory
// (audio-thread SIGSEGV on a freed ProcessingConfig under
// ConcurrentRebuild.RepeatedAddRemoveSingleRouting). The window is a few
// nanoseconds wide and only opened on some x86-64 hosts, so a pass here is
// necessary rather than sufficient.
TEST(RCU, ReadScopeNeverOutlivesItsVersion) {
    RCU<TrackedVersion> rcu;
    auto& table = version_table();

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> violations{0};
    std::atomic<uint64_t> reads{0};

    std::vector<std::thread> readers;
    readers.reserve(2);
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&]() {
            rcu.register_reader_thread();
            uint64_t local_reads = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                {
                    auto scope = rcu.read_scope();
                    const uint64_t seq = scope.data().m_seq;
                    // Stay inside the section for a moment so a premature
                    // reclamation has time to land.
                    for (volatile int i = 0; i < 100; i = i + 1) {}
                    if (seq >= k_version_table_size || table.m_destroyed[seq].load() != 0) {
                        violations.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                ++local_reads;
            }
            reads.fetch_add(local_reads, std::memory_order_relaxed);
        });
    }

    // replace() alone exercises the non-blocking reclamation tier
    // (cleanup_safe_versions), which is where the premature delete happened.
    std::thread writer([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            if (table.m_next_seq.load(std::memory_order_relaxed) + 16 >= k_version_table_size) {
                break;
            }
            rcu.replace([](TrackedVersion&) {});
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    for (auto& reader : readers) { reader.join(); }

    EXPECT_EQ(violations.load(), 0U);
    EXPECT_GT(reads.load(), 100U);
}
