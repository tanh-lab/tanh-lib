#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "tanh/core/threading/LockFreeQueue.h"
#include "tanh/utils/RealtimeSanitizer.h"

using thl::core::DynamicLockFreeQueue;
using thl::core::LockFreeQueue;

namespace {

struct Item {
    int m_producer = 0;
    int m_index = 0;
};

}  // namespace

TEST(LockFreeQueue, StartsEmpty) {
    LockFreeQueue<int, 4> q;
    EXPECT_EQ(q.capacity(), 4U);
    EXPECT_TRUE(q.empty_approx());
    EXPECT_EQ(q.size_approx(), 0U);
    int v = 7;
    EXPECT_FALSE(q.try_pop(v));
    EXPECT_EQ(v, 7) << "failed pop must not touch out";
}

TEST(LockFreeQueue, FifoOrder) {
    LockFreeQueue<int, 8> q;
    for (int i = 0; i < 5; ++i) { EXPECT_TRUE(q.try_push(i)); }
    EXPECT_EQ(q.size_approx(), 5U);
    for (int i = 0; i < 5; ++i) {
        int v = -1;
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_TRUE(q.empty_approx());
}

TEST(LockFreeQueue, FullRejectsPush) {
    LockFreeQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) { EXPECT_TRUE(q.try_push(i)); }
    EXPECT_FALSE(q.try_push(99));
    EXPECT_EQ(q.size_approx(), 4U);
    int v = 0;
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 0);
    EXPECT_TRUE(q.try_push(99));
    for (int expected : {1, 2, 3, 99}) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, expected);
    }
}

TEST(LockFreeQueue, WrapsAroundManyTimes) {
    LockFreeQueue<std::uint32_t, 4> q;
    std::uint32_t next_push = 0;
    std::uint32_t next_pop = 0;
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < 3; ++i) { ASSERT_TRUE(q.try_push(next_push++)); }
        for (int i = 0; i < 3; ++i) {
            std::uint32_t v = 0;
            ASSERT_TRUE(q.try_pop(v));
            ASSERT_EQ(v, next_pop++);
        }
    }
}

TEST(LockFreeQueue, PushOverwriteDiscardsOldest) {
    LockFreeQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) { EXPECT_EQ(q.push_overwrite(i), 0U); }
    EXPECT_EQ(q.push_overwrite(4), 1U);
    EXPECT_EQ(q.push_overwrite(5), 1U);
    EXPECT_EQ(q.size_approx(), 4U);
    for (int expected : {2, 3, 4, 5}) {
        int v = -1;
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, expected);
    }
}

TEST(LockFreeQueue, ClearEmptiesQueue) {
    LockFreeQueue<int, 8> q;
    for (int i = 0; i < 6; ++i) { q.try_push(i); }
    q.clear();
    EXPECT_TRUE(q.empty_approx());
    EXPECT_TRUE(q.try_push(1));
    int v = 0;
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 1);
}

TEST(LockFreeQueue, StructPayload) {
    LockFreeQueue<Item, 2> q;
    EXPECT_TRUE(q.try_push(Item{.m_producer = 3, .m_index = 9}));
    Item out;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.m_producer, 3);
    EXPECT_EQ(out.m_index, 9);
}

TEST(LockFreeQueue, TypeRequirements) {
    static_assert(std::is_trivially_destructible_v<LockFreeQueue<int, 4>>);
    static_assert(std::is_nothrow_default_constructible_v<LockFreeQueue<int, 4>>);
    static_assert(!std::is_copy_constructible_v<LockFreeQueue<int, 4>>);
    static_assert(!std::is_move_constructible_v<LockFreeQueue<int, 4>>);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

TEST(LockFreeQueue, ManyProducersOneConsumer) {
    constexpr int k_producers = 4;
    constexpr int k_per_producer = 20000;
    auto q = std::make_unique<LockFreeQueue<Item, 256>>();

    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    producers.reserve(k_producers);
    for (int p = 0; p < k_producers; ++p) {
        producers.emplace_back([&, p]() {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < k_per_producer; ++i) {
                while (!q->try_push(Item{.m_producer = p, .m_index = i})) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::array<int, k_producers> next{};
    std::size_t received = 0;
    std::thread consumer([&]() {
        while (received < static_cast<std::size_t>(k_producers) * k_per_producer) {
            Item item;
            if (!q->try_pop(item)) {
                std::this_thread::yield();
                continue;
            }
            // Per-producer order must hold; nothing may be lost or duplicated.
            EXPECT_EQ(item.m_index, next[static_cast<std::size_t>(item.m_producer)]);
            ++next[static_cast<std::size_t>(item.m_producer)];
            ++received;
        }
    });

    go.store(true, std::memory_order_release);
    for (auto& p : producers) { p.join(); }
    consumer.join();

    for (int p = 0; p < k_producers; ++p) {
        EXPECT_EQ(next[static_cast<std::size_t>(p)], k_per_producer);
    }
    EXPECT_TRUE(q->empty_approx());
}

TEST(LockFreeQueue, ManyProducersManyConsumers) {
    constexpr int k_producers = 3;
    constexpr int k_consumers = 3;
    constexpr int k_per_producer = 20000;
    constexpr std::size_t k_total = static_cast<std::size_t>(k_producers) * k_per_producer;
    auto q = std::make_unique<LockFreeQueue<Item, 64>>();

    std::atomic<bool> go{false};
    std::atomic<std::size_t> received{0};
    // seen[p][i] counts deliveries of each item; every entry must end at 1.
    std::vector<std::atomic<int>> seen(k_total);

    std::vector<std::thread> threads;
    for (int p = 0; p < k_producers; ++p) {
        threads.emplace_back([&, p]() {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < k_per_producer; ++i) {
                while (!q->try_push(Item{.m_producer = p, .m_index = i})) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (int c = 0; c < k_consumers; ++c) {
        threads.emplace_back([&]() {
            while (received.load(std::memory_order_relaxed) < k_total) {
                Item item;
                if (!q->try_pop(item)) {
                    std::this_thread::yield();
                    continue;
                }
                const auto slot = static_cast<std::size_t>(item.m_producer) * k_per_producer +
                                  static_cast<std::size_t>(item.m_index);
                seen[slot].fetch_add(1, std::memory_order_relaxed);
                received.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : threads) { t.join(); }

    for (std::size_t i = 0; i < k_total; ++i) { ASSERT_EQ(seen[i].load(), 1) << "item " << i; }
    EXPECT_TRUE(q->empty_approx());
}

TEST(LockFreeQueue, OverwriteUnderContentionNeverBlocks) {
    auto q = std::make_unique<LockFreeQueue<int, 16>>();
    std::atomic<bool> stop{false};
    std::thread consumer([&]() {
        int v = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            if (!q->try_pop(v)) { std::this_thread::yield(); }
        }
    });
    std::vector<std::thread> producers;
    for (int p = 0; p < 3; ++p) {
        producers.emplace_back([&]() {
            for (int i = 0; i < 50000; ++i) { (void)q->push_overwrite(i); }
        });
    }
    for (auto& p : producers) { p.join(); }
    stop.store(true, std::memory_order_relaxed);
    consumer.join();
    EXPECT_LE(q->size_approx(), 16U);
}

// ---------------------------------------------------------------------------
// Real-time safety (enforced under -fsanitize=realtime)
// ---------------------------------------------------------------------------

namespace {

// A namespace-scope queue must be usable from any static initialiser, so
// the constructor has to be constexpr: constinit enforces that.
constinit LockFreeQueue<Item, 64> g_rt_queue;

void rt_producer(int i) TANH_NONBLOCKING_FUNCTION {
    (void)g_rt_queue.try_push(Item{.m_producer = 0, .m_index = i});
    (void)g_rt_queue.push_overwrite(Item{.m_producer = 1, .m_index = i});
    (void)g_rt_queue.size_approx();
}

void rt_consumer() TANH_NONBLOCKING_FUNCTION {
    Item item;
    while (g_rt_queue.try_pop(item)) {}
}

}  // namespace

TEST(LockFreeQueue, OperationsAreNonblocking) {
    for (int i = 0; i < 200; ++i) { rt_producer(i); }
    rt_consumer();
    EXPECT_TRUE(g_rt_queue.empty_approx());
}

TEST(DynamicLockFreeQueue, RoundsCapacityUpToPowerOfTwo) {
    EXPECT_EQ(DynamicLockFreeQueue<int>(1).capacity(), 2U);
    EXPECT_EQ(DynamicLockFreeQueue<int>(5).capacity(), 8U);
    EXPECT_EQ(DynamicLockFreeQueue<int>(512).capacity(), 512U);
}

TEST(DynamicLockFreeQueue, FifoAndFull) {
    DynamicLockFreeQueue<int> q(4);
    for (int i = 0; i < 4; ++i) { EXPECT_TRUE(q.try_push(i)); }
    EXPECT_FALSE(q.try_push(99));
    for (int i = 0; i < 4; ++i) {
        int v = -1;
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    int v = 7;
    EXPECT_FALSE(q.try_pop(v));
    EXPECT_EQ(v, 7);
}

TEST(DynamicLockFreeQueue, WrapsAroundManyTimes) {
    DynamicLockFreeQueue<int> q(4);
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < 3; ++i) { ASSERT_TRUE(q.try_push(round * 3 + i)); }
        for (int i = 0; i < 3; ++i) {
            int v = -1;
            ASSERT_TRUE(q.try_pop(v));
            EXPECT_EQ(v, round * 3 + i);
        }
    }
}

TEST(DynamicLockFreeQueue, ManyProducersOneConsumerNoLoss) {
    DynamicLockFreeQueue<Item> q(64);
    constexpr int k_producers = 4;
    constexpr int k_per_producer = 5000;
    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    for (int p = 0; p < k_producers; ++p) {
        producers.emplace_back([&, p] {
            while (!go.load()) {}
            for (int i = 0; i < k_per_producer; ++i) {
                Item item{p, i};
                while (!q.try_push(item)) { std::this_thread::yield(); }
            }
        });
    }
    std::array<int, k_producers> next{};
    int received = 0;
    go = true;
    while (received < k_producers * k_per_producer) {
        Item item;
        if (q.try_pop(item)) {
            EXPECT_EQ(item.m_index, next[static_cast<std::size_t>(item.m_producer)]++);
            ++received;
        }
    }
    for (auto& t : producers) { t.join(); }
    EXPECT_TRUE(q.empty_approx());
}
