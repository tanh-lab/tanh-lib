#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <utility>
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

    /// True if the version was destroyed, or is not one this table can tell about.
    [[nodiscard]] bool destroyed(uint64_t seq) const {
        return seq >= k_version_table_size || m_destroyed[seq].load() != 0;
    }
};

VersionTable& version_table() {
    static VersionTable table;
    return table;
}

// The table, emptied for the calling test. The tests share one table; without
// this a run of several of them in one process (--gtest_repeat) uses it up,
// after which the writers stop and every version reads as destroyed. Call it
// before the test creates its first version: none of an earlier test is alive.
VersionTable& fresh_version_table() {
    auto& table = version_table();
    const uint64_t used = std::min(table.m_next_seq.load(), k_version_table_size);
    for (uint64_t seq = 0; seq < used; ++seq) {
        table.m_destroyed[seq].store(0, std::memory_order_relaxed);
    }
    table.m_next_seq.store(1);
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
    auto& table = fresh_version_table();
    RCU<TrackedVersion> rcu;

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
                    if (table.destroyed(seq)) {
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

// A reader slot from add_reader() is not tied to a thread: a host may call the
// render callback from a different thread every block. Reading through the slot
// from a fresh thread per batch must stay protected (no version reclaimed while
// in a section) and must not register any thread (no per-thread node).
TEST(RCU, OwnedReaderProtectsAcrossChangingThreads) {
    auto& table = fresh_version_table();
    RCU<TrackedVersion> rcu;
    const auto reader = rcu.add_reader();
    const unsigned int readers_before = rcu.get_reader_count();

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> violations{0};
    std::atomic<uint64_t> reads{0};

    std::thread writer([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            if (table.m_next_seq.load(std::memory_order_relaxed) + 16 >= k_version_table_size) {
                break;
            }
            rcu.replace([](TrackedVersion&) {});
        }
    });

    // Sequential "render threads": never two at the same time, like a host.
    for (int batch = 0; batch < 50; ++batch) {
        std::thread render([&]() {
            for (int i = 0; i < 2000; ++i) {
                auto scope = rcu.read_scope(reader);
                const uint64_t seq = scope.data().m_seq;
                for (volatile int spin = 0; spin < 100; spin = spin + 1) {}
                if (table.destroyed(seq)) { violations.fetch_add(1, std::memory_order_relaxed); }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
        render.join();
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    EXPECT_EQ(violations.load(), 0U);
    EXPECT_EQ(reads.load(), 50U * 2000U);
    EXPECT_EQ(rcu.get_reader_count(), readers_before);  // no thread registered itself
}

// Read sections nest: closing an inner scope keeps the outer one's version
// alive, on a reader slot and on a registered thread.
TEST(RCU, NestedReadScopesKeepTheOuterVersion) {
    auto& table = fresh_version_table();
    RCU<TrackedVersion> rcu;
    const auto reader = rcu.add_reader();
    {
        auto outer = rcu.read_scope(reader);
        const uint64_t seq = outer.data().m_seq;
        { auto inner = rcu.read_scope(reader); }
        for (int i = 0; i < 4; ++i) {
            rcu.replace([](TrackedVersion&) {});
        }
        EXPECT_FALSE(table.destroyed(seq)) << "slot";
    }
    rcu.register_reader_thread();
    {
        auto outer = rcu.read_scope();
        const uint64_t seq = outer.data().m_seq;
        { auto inner = rcu.read_scope(); }
        for (int i = 0; i < 4; ++i) {
            rcu.replace([](TrackedVersion&) {});
        }
        EXPECT_FALSE(table.destroyed(seq)) << "thread";
    }
}

namespace {

/// A thread that runs one step each time it is told to, then waits for the next.
class StepThread {
public:
    explicit StepThread(std::vector<std::function<void()>> steps)
        : m_steps(std::move(steps)), m_thread([this] { run(); }) {}
    ~StepThread() {
        while (m_done.load() < m_steps.size()) { step(); }
        m_thread.join();
    }
    StepThread(const StepThread&) = delete;
    StepThread& operator=(const StepThread&) = delete;
    StepThread(StepThread&&) = delete;
    StepThread& operator=(StepThread&&) = delete;

    /// Runs the next step on the thread and waits until it finished.
    void step() {
        const size_t target = m_done.load() + 1;
        m_go.store(target);
        while (m_done.load() < target) { std::this_thread::yield(); }
    }

private:
    void run() {
        for (size_t i = 0; i < m_steps.size(); ++i) {
            while (m_go.load() <= i) { std::this_thread::yield(); }
            m_steps[i]();
            m_done.store(i + 1);
        }
    }

    std::vector<std::function<void()>> m_steps;
    std::atomic<size_t> m_go{0};
    std::atomic<size_t> m_done{0};
    std::thread m_thread;
};

}  // namespace

// An RCU destroyed while a thread that read it still runs: the thread frees its
// node when it exits, after the RCU (under ASan a double free or a write into a
// freed node would fail here).
TEST(RCU, ThreadOutlivingTheRcuFreesItsNode) {
    auto rcu = std::make_unique<RCU<int>>(7);
    int seen = 0;
    {
        StepThread reader({[&] { seen = rcu->read([](const int& v) { return v; }); }, [] {}});
        reader.step();  // registered and read
        rcu.reset();    // destroyed while the reader thread lives
        reader.step();
    }  // the thread exits now
    EXPECT_EQ(seen, 7);
}

// A thread that read an RCU destroyed since, then reads a new one at the same
// address: it registers in the new one instead of reusing the old node, which the
// new writer would never see.
TEST(RCU, NewRcuAtADestroyedOnesAddressRegistersAnew) {
    alignas(RCU<int>) std::byte storage[sizeof(RCU<int>)];
    auto* first = new (storage) RCU<int>(1);
    unsigned int readers = 0;
    RCU<int>* second = nullptr;
    {
        // read(), not read_scope(): this read registers the thread (it allocates
        // and locks), which RealtimeSanitizer refuses inside read_scope(), a
        // TANH_NONBLOCKING_FUNCTION.
        StepThread reader({[&] { (void)first->read([](const int& v) { return v; }); },
                           [&] {
                               (void)second->read([](const int& v) { return v; });
                               readers = second->get_reader_count();
                           }});
        reader.step();
        first->~RCU();
        second = new (storage) RCU<int>(2);
        ASSERT_EQ(static_cast<void*>(second), static_cast<void*>(first));
        reader.step();
    }
    EXPECT_EQ(readers, 1u);
    second->~RCU();
}

// A thread reading one short-lived RCU after another, each destroyed on another
// thread, keeps what a destroyed RCU left in its registry only until it registers
// with the next one.
TEST(RCU, ThreadRegistryDropsDestroyedRcus) {
    constexpr int k_rcus = 100;
    // Every RCU at its own address, so none reuses a destroyed one's entry.
    struct alignas(RCU<int>) Storage {
        std::byte m_bytes[sizeof(RCU<int>)];
    };
    std::vector<Storage> storage(k_rcus);
    RCU<int>* rcu = nullptr;
    size_t max_entries = 0;
    std::vector<std::function<void()>> steps(k_rcus, [&] {
        (void)rcu->read([](const int& v) { return v; });
        max_entries = std::max(max_entries, detail::rcu_thread_state().m_nodes.size());
    });
    {
        StepThread reader(std::move(steps));
        for (int i = 0; i < k_rcus; ++i) {
            rcu = new (storage[static_cast<size_t>(i)].m_bytes) RCU<int>(i);
            reader.step();
            rcu->~RCU();  // on this thread: the reader keeps the RCU's entry
        }
    }
    EXPECT_EQ(max_entries, 1u);
}

// An RCU that outlives its thread's reader registry: a thread_local constructed
// before the thread's first read is destroyed after the registry, and so is an
// RCU with static storage duration that the main thread read. Its destructor
// must not look into the registry. (libc++ crashes on a lookup in the destroyed
// map; libstdc++ happens to survive it, so only a libc++ build shows a
// regression here.)
TEST(RCU, DestroyedAfterItsThreadsRegistry) {
    int seen = 0;
    std::thread reader([&seen] {
        struct Owner {
            RCU<int> m_rcu{7};
        };
        static thread_local Owner owner;  // constructed first, so destroyed last
        seen = owner.m_rcu.read([](const int& v) { return v; });  // constructs the registry
    });
    reader.join();
    EXPECT_EQ(seen, 7);
}

// Reader threads exit while another thread destroys the RCU they read. Whichever
// of thread and RCU is second frees each node, once: ASan and LSan report a
// double free, a use after free or a leak here, TSan an access that the
// hand-over does not order.
TEST(RCU, ThreadExitRacingDestructionFreesEachNodeOnce) {
    constexpr int k_rounds = 200;
    constexpr int k_readers = 4;
    for (int round = 0; round < k_rounds; ++round) {
        auto rcu = std::make_unique<RCU<int>>(round);
        std::atomic<int> registered{0};
        std::atomic<bool> leave{false};
        std::vector<std::thread> readers;
        readers.reserve(k_readers);
        for (int r = 0; r < k_readers; ++r) {
            readers.emplace_back([&] {
                (void)rcu->read([](const int& v) { return v; });
                registered.fetch_add(1);
                while (!leave.load()) { std::this_thread::yield(); }
            });  // the thread exits: its registry hands the node over or frees it
        }
        while (registered.load() < k_readers) { std::this_thread::yield(); }
        leave.store(true);
        rcu.reset();  // while the readers exit
        for (auto& reader : readers) { reader.join(); }
    }
}

// Reader threads come and go while a writer publishes: the writer unlinks and
// frees the nodes of exited threads (cleanup_dead_nodes) while others exit.
// Afterwards only the slot is left to count.
TEST(RCU, ExitedReaderThreadsAreReclaimedByWriters) {
    RCU<int> rcu(0);
    const auto slot = rcu.add_reader();
    std::atomic<bool> stop{false};
    std::atomic<int> went_backwards{0};

    std::thread writer([&] {
        int next = 0;
        while (!stop.load()) {
            rcu.update([&next](int& v) { v = ++next; });
        }
    });

    for (int batch = 0; batch < 100; ++batch) {
        std::vector<std::thread> readers;
        readers.reserve(4);
        for (int r = 0; r < 4; ++r) {
            readers.emplace_back([&] {
                int last = -1;
                for (int i = 0; i < 20; ++i) {
                    const int v = rcu.read([](const int& x) { return x; });
                    if (v < last) { went_backwards.fetch_add(1); }
                    last = v;
                }
            });
        }
        for (auto& reader : readers) { reader.join(); }
    }
    stop.store(true);
    writer.join();
    rcu.update([](int&) {});  // a publication after the last exit unlinks the rest

    EXPECT_EQ(went_backwards.load(), 0);
    EXPECT_EQ(rcu.get_reader_count(), 1u);
    EXPECT_TRUE(static_cast<bool>(slot));
}

// A thread inside a read scope of one RCU registers with others: the first of
// those registrations sweeps what a destroyed RCU left in its registry, the
// later ones rehash it. The open scope keeps its node and its version.
TEST(RCU, RegisteringInsideAnotherRcusScopeKeepsItsVersion) {
    constexpr size_t k_others = 64;
    auto& table = fresh_version_table();
    RCU<TrackedVersion> scoped;
    auto stale = std::make_unique<RCU<int>>(1);
    std::vector<std::unique_ptr<RCU<int>>> others;
    others.reserve(k_others);
    for (size_t i = 0; i < k_others; ++i) {
        others.push_back(std::make_unique<RCU<int>>(static_cast<int>(i)));
    }
    bool kept = false;
    size_t entries = 0;
    {
        StepThread reader({[&] {
                               scoped.register_reader_thread();
                               (void)stale->read([](const int& v) { return v; });
                           },
                           [&] {
                               auto scope = scoped.read_scope();
                               const uint64_t seq = scope.data().m_seq;
                               for (auto& other : others) {
                                   (void)other->read([](const int& v) { return v; });
                               }
                               for (int i = 0; i < 4; ++i) {
                                   scoped.replace([](TrackedVersion&) {});
                               }
                               kept = !table.destroyed(seq);
                               entries = detail::rcu_thread_state().m_nodes.size();
                           }});
        reader.step();
        stale.reset();  // leaves its node in the reader's registry
        reader.step();
    }
    EXPECT_TRUE(kept);
    EXPECT_EQ(entries, 1 + k_others);  // the stale entry went, inside the scope
}

// A slot that is idle or was never used holds no version back, and
// synchronize() does not wait for it.
TEST(RCU, IdleSlotsDoNotHoldBackTheWriter) {
    auto& table = fresh_version_table();
    RCU<TrackedVersion> rcu;
    const auto used = rcu.add_reader();
    const auto unused = rcu.add_reader();
    uint64_t seq = 0;
    {
        auto scope = rcu.read_scope(used);
        seq = scope.data().m_seq;
    }
    rcu.replace([](TrackedVersion&) {});
    EXPECT_TRUE(table.destroyed(seq));  // reclaimed by the publication that retired it
    rcu.synchronize();                  // returns
    EXPECT_TRUE(static_cast<bool>(unused));
}

// An open section on a slot holds synchronize() until it is closed, as one on a
// registered thread does.
TEST(RCU, OpenSlotSectionHoldsBackSynchronize) {
    RCU<int> rcu(1);
    const auto slot = rcu.add_reader();
    std::atomic<bool> synchronized{false};
    std::thread writer;
    {
        auto scope = rcu.read_scope(slot);
        rcu.update([](int& v) { v = 2; });  // the scope now holds a retired version
        writer = std::thread([&] {
            rcu.synchronize();
            synchronized.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_FALSE(synchronized.load());
        EXPECT_EQ(scope.data(), 1);
    }
    writer.join();
    EXPECT_TRUE(synchronized.load());
}

namespace {

// A render callback that reads through a reader slot. Under RealtimeSanitizer an
// allocation, a lock or a blocking call anywhere below aborts the test.
int render_through_slot(const RCU<int>& rcu, RCU<int>::Reader reader) TANH_NONBLOCKING_FUNCTION {
    int sum = 0;
    for (int i = 0; i < 100; ++i) {
        const auto outer = rcu.read_scope(reader);
        const auto inner = rcu.read_scope(reader);  // nested, as in a callee of the callback
        sum += outer.data() + inner.data();
    }
    return sum;
}

}  // namespace

// The slot path is real-time safe on a thread that never registered with the
// RCU (a host's render thread): it registers nothing, so it reaches no
// allocation, no lock and no thread-local registry.
TEST(RCU, SlotScopeIsRealtimeSafeOnAnUnregisteredThread) {
    RCU<int> rcu(3);
    const auto reader = rcu.add_reader();
    const unsigned int readers_before = rcu.get_reader_count();
    int sum = 0;
    std::thread render_thread([&] { sum = render_through_slot(rcu, reader); });
    render_thread.join();
    EXPECT_EQ(sum, 100 * (3 + 3));
    EXPECT_EQ(rcu.get_reader_count(), readers_before);  // the thread did not register
}

// A small pool of reader slots shared by more threads than slots, the way a
// reader that is not bound to a thread uses them: a thread claims a free slot
// (an atomic flag, acquire on the claim, release on the return), reads through
// it and returns it after its scope has closed. Each slot has one user at a
// time and the claim orders one user after the other, which is the contract of
// add_reader(): every read stays protected.
TEST(RCU, SlotPoolWithOneUserAtATimeProtects) {
    constexpr size_t k_slots = 2;
    constexpr int k_users = 6;
    auto& table = fresh_version_table();
    RCU<TrackedVersion> rcu;
    struct PooledSlot {
        RCU<TrackedVersion>::Reader m_reader;
        std::atomic<bool> m_claimed{false};
    };
    std::array<PooledSlot, k_slots> pool;
    for (auto& slot : pool) { slot.m_reader = rcu.add_reader(); }

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> violations{0};
    std::atomic<uint64_t> reads{0};

    std::thread writer([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            if (table.m_next_seq.load(std::memory_order_relaxed) + 16 >= k_version_table_size) {
                break;
            }
            rcu.replace([](TrackedVersion&) {});
        }
    });

    std::vector<std::thread> users;
    users.reserve(k_users);
    for (int u = 0; u < k_users; ++u) {
        users.emplace_back([&]() {
            uint64_t local_reads = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (auto& slot : pool) {
                    if (slot.m_claimed.exchange(true, std::memory_order_acquire)) { continue; }
                    {
                        auto scope = rcu.read_scope(slot.m_reader);
                        const uint64_t seq = scope.data().m_seq;
                        for (volatile int spin = 0; spin < 20; spin = spin + 1) {}
                        if (table.destroyed(seq)) {
                            violations.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    slot.m_claimed.store(false, std::memory_order_release);  // after the scope
                    ++local_reads;
                }
            }
            reads.fetch_add(local_reads, std::memory_order_relaxed);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    for (auto& user : users) { user.join(); }

    EXPECT_EQ(violations.load(), 0U);
    EXPECT_GT(reads.load(), 100U);
    EXPECT_EQ(rcu.get_reader_count(), k_slots);  // no thread registered itself
}

// get_reader_count() counts the reader slots and the registered threads that
// are still alive.
TEST(RCU, ReaderCountCountsSlotsAndLiveThreads) {
    RCU<int> rcu(0);
    EXPECT_EQ(rcu.get_reader_count(), 0u);
    const auto slot = rcu.add_reader();
    EXPECT_EQ(rcu.get_reader_count(), 1u);
    rcu.register_reader_thread();
    rcu.register_reader_thread();  // a no-op the second time
    EXPECT_EQ(rcu.get_reader_count(), 2u);
    unsigned int with_other = 0;
    std::thread other([&] {
        (void)rcu.read([](const int& v) { return v; });
        with_other = rcu.get_reader_count();
    });
    other.join();
    EXPECT_EQ(with_other, 3u);
    EXPECT_EQ(rcu.get_reader_count(), 2u);  // an exited thread no longer counts
    EXPECT_TRUE(static_cast<bool>(slot));
}
