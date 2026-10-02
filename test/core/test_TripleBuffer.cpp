#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "tanh/core/threading/TripleBuffer.h"
#include "tanh/utils/RealtimeSanitizer.h"

using thl::TripleBuffer;

namespace {

/// Every field derives from one counter, so a torn copy is detectable.
struct Frame {
    uint64_t m_counter = 0;
    std::array<uint64_t, 64> m_values{};
    double m_half = 0.0;

    void fill(uint64_t k) {
        m_counter = k;
        for (size_t i = 0; i < m_values.size(); ++i) { m_values[i] = (k * 31) + i; }
        m_half = static_cast<double>(k) * 0.5;
    }
    [[nodiscard]] bool consistent() const {
        for (size_t i = 0; i < m_values.size(); ++i) {
            if (m_values[i] != (m_counter * 31) + i) { return false; }
        }
        return m_half == static_cast<double>(m_counter) * 0.5;
    }
};

}  // namespace

TEST(TripleBuffer, NothingNewBeforeTheFirstWrite) {
    TripleBuffer<int> tb(7);
    int out = -1;
    EXPECT_FALSE(tb.has_new());
    EXPECT_FALSE(tb.read(out));
    EXPECT_EQ(out, -1);  // untouched
    EXPECT_EQ(tb.latest(), 7);
}

TEST(TripleBuffer, ReadReturnsTheLatestValueOnce) {
    TripleBuffer<int> tb;
    tb.write(1);
    tb.write(2);
    tb.write(3);
    EXPECT_TRUE(tb.has_new());
    int out = 0;
    EXPECT_TRUE(tb.read(out));
    EXPECT_EQ(out, 3);  // intermediate values are overwritten
    EXPECT_FALSE(tb.read(out));
    EXPECT_EQ(tb.latest(), 3);

    tb.write(4);
    EXPECT_TRUE(tb.read(out));
    EXPECT_EQ(out, 4);
}

TEST(TripleBuffer, WriteBufferAndPublishInPlace) {
    TripleBuffer<Frame> tb;
    for (uint64_t k = 1; k <= 10; ++k) {
        tb.write_buffer().fill(k);
        tb.publish();
        if (k % 3 == 0) {
            ASSERT_TRUE(tb.update());
            EXPECT_EQ(tb.latest().m_counter, k);
            EXPECT_TRUE(tb.latest().consistent());
        }
    }
    Frame f;
    ASSERT_TRUE(tb.read(f));
    EXPECT_EQ(f.m_counter, 10u);
}

// One writer, one reader, no waiting on either side: every frame the reader
// gets is complete and the sequence never goes backwards. Run under TSan.
TEST(TripleBuffer, ConcurrentFramesAreConsistentAndMonotonic) {
    auto tb = std::make_unique<TripleBuffer<Frame>>();
    constexpr uint64_t k_writes = 200000;
    std::atomic<bool> done{false};

    std::thread writer([&] {
        for (uint64_t k = 1; k <= k_writes; ++k) {
            tb->write_buffer().fill(k);
            tb->publish();
        }
        done.store(true, std::memory_order_release);
    });

    uint64_t last = 0;
    uint64_t reads = 0;
    int torn = 0;
    int backwards = 0;
    Frame f;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        if (tb->read(f)) {
            ++reads;
            if (!f.consistent()) { ++torn; }
            if (f.m_counter <= last) { ++backwards; }
            last = f.m_counter;
        }
        if (finished && !tb->has_new()) { break; }
    }
    writer.join();
    EXPECT_EQ(torn, 0);
    EXPECT_EQ(backwards, 0);
    EXPECT_EQ(last, k_writes);  // the final value always arrives
    EXPECT_GT(reads, 0u);
}

namespace {

uint64_t rt_round(TripleBuffer<Frame>& tb, uint64_t k) TANH_NONBLOCKING_FUNCTION {
    tb.write_buffer().m_counter = k;
    tb.publish();
    Frame f;
    return tb.read(f) ? f.m_counter : 0;
}

}  // namespace

// Under -DTANH_WITH_RTSAN=ON neither side allocates, locks or blocks.
TEST(TripleBuffer, BothSidesAreRealtimeSafe) {
    auto tb = std::make_unique<TripleBuffer<Frame>>();
    uint64_t sum = 0;
    for (uint64_t k = 1; k <= 100; ++k) { sum += rt_round(*tb, k); }
    EXPECT_EQ(sum, 5050u);
}
