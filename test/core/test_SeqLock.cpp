#include <gtest/gtest.h>
#include <tanh/core/threading/SeqLock.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

using thl::SeqLock;

TEST(SeqLock, ReadOutsideAWriteIsValid) {
    SeqLock lock;
    const auto token = lock.read_begin();
    ASSERT_TRUE(token.has_value());
    EXPECT_TRUE(lock.read_valid(*token));
}

TEST(SeqLock, ReadDuringAWriteIsRefused) {
    SeqLock lock;
    lock.write_begin();
    EXPECT_FALSE(lock.read_begin().has_value());
    lock.write_end();
    EXPECT_TRUE(lock.read_begin().has_value());
}

TEST(SeqLock, ReadOverlappingAWriteIsInvalid) {
    SeqLock lock;
    const auto token = lock.read_begin();
    ASSERT_TRUE(token.has_value());
    lock.write_begin();
    lock.write_end();
    EXPECT_FALSE(lock.read_valid(*token));
}

// A writer keeps every value of a batch equal; a reader that accepts a read never
// sees two different values.
TEST(SeqLock, AcceptedReadsAreNeverTorn) {
    constexpr size_t k_values = 8;
    SeqLock lock;
    std::array<std::atomic<uint32_t>, k_values> values{};
    std::atomic<bool> stop{false};

    std::thread writer([&] {
        for (uint32_t batch = 1; !stop.load(std::memory_order_relaxed); ++batch) {
            lock.write_begin();
            for (auto& v : values) { v.store(batch, std::memory_order_relaxed); }
            lock.write_end();
        }
    });

    uint64_t accepted = 0;
    uint64_t torn = 0;
    for (int i = 0; i < 200000; ++i) {
        const auto token = lock.read_begin();
        if (!token) { continue; }
        std::array<uint32_t, k_values> read{};
        for (size_t k = 0; k < k_values; ++k) {
            read[k] = values[k].load(std::memory_order_relaxed);
        }
        if (!lock.read_valid(*token)) { continue; }
        ++accepted;
        for (size_t k = 1; k < k_values; ++k) {
            if (read[k] != read[0]) { ++torn; }
        }
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    EXPECT_EQ(torn, 0u);
    EXPECT_GT(accepted, 0u);
}
