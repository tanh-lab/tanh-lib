#include <gtest/gtest.h>
#include <tanh/core/threading/SpscRingBuffer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <thread>

using thl::core::SpscRingBuffer;

namespace {

/// Writes @p count samples: channel 0 counts up from @p first, channel 1 is its negative.
size_t write_counting(SpscRingBuffer<float>& ring, float first, size_t count) {
    std::array<float, 64> ch0{};
    std::array<float, 64> ch1{};
    if (count > ch0.size()) {  // a test asking for more is wrong: fail it, do not overrun
        ADD_FAILURE() << "write_counting(): at most " << ch0.size() << " samples";
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        ch0[i] = first + static_cast<float>(i);
        ch1[i] = -ch0[i];
    }
    const std::array<const float*, 2> channels{ch0.data(), ch1.data()};
    return ring.write(channels, count);
}

}  // namespace

TEST(SpscRingBuffer, PreparedEmptyWithAPowerOfTwoCapacity) {
    SpscRingBuffer<float> ring;
    ring.prepare(2, 100);
    EXPECT_EQ(ring.capacity(), 128u);
    EXPECT_EQ(ring.num_channels(), 2u);
    EXPECT_EQ(ring.available(), 0u);
    EXPECT_EQ(ring.free_space(), 128u);
}

TEST(SpscRingBuffer, ReadsWhatWasWrittenAcrossTheWrap) {
    SpscRingBuffer<float> ring;
    ring.prepare(2, 16);
    float next = 0.0f;
    float expected = 0.0f;
    for (int round = 0; round < 10; ++round) {  // 10 · 11 samples through 16: wraps often
        ASSERT_EQ(write_counting(ring, next, 11), 11u);
        next += 11.0f;
        std::array<float, 11> a{};
        std::array<float, 11> b{};
        const std::array<float*, 2> out{a.data(), b.data()};
        ASSERT_EQ(ring.read(out, 11), 11u);
        for (size_t i = 0; i < 11; ++i) {
            ASSERT_EQ(a[i], expected) << round << " " << i;
            ASSERT_EQ(b[i], -expected);
            expected += 1.0f;
        }
    }
    EXPECT_EQ(ring.dropped(), 0u);
}

TEST(SpscRingBuffer, AFullRingDropsTheNewestAndCountsThem) {
    SpscRingBuffer<float> ring;
    ring.prepare(2, 16);
    EXPECT_EQ(write_counting(ring, 0.0f, 10), 10u);
    EXPECT_EQ(write_counting(ring, 10.0f, 10), 6u);  // 4 dropped
    EXPECT_EQ(ring.dropped(), 4u);
    EXPECT_EQ(ring.free_space(), 0u);
    std::array<float, 16> a{};
    const std::array<float*, 1> out{a.data()};  // channel 1 not read
    ASSERT_EQ(ring.read(out, 16), 16u);
    for (size_t i = 0; i < 16; ++i) { EXPECT_EQ(a[i], static_cast<float>(i)); }  // the oldest kept
}

TEST(SpscRingBuffer, MissingChannelsReadAsZeroExtraOnesAreIgnored) {
    SpscRingBuffer<float> ring;
    ring.prepare(2, 8);
    const std::array<float, 4> one{1.0f, 2.0f, 3.0f, 4.0f};
    const std::array<const float*, 1> in{one.data()};
    ASSERT_EQ(ring.write(in, 4), 4u);
    std::array<float, 4> a{};
    std::array<float, 4> b{9.0f, 9.0f, 9.0f, 9.0f};
    std::array<float, 4> c{9.0f, 9.0f, 9.0f, 9.0f};
    const std::array<float*, 3> out{a.data(), b.data(), c.data()};
    ASSERT_EQ(ring.read(out, 4), 4u);
    EXPECT_EQ(a, one);
    EXPECT_EQ(b, (std::array<float, 4>{}));
    EXPECT_EQ(c, (std::array<float, 4>{9.0f, 9.0f, 9.0f, 9.0f}));  // no third channel
}

TEST(SpscRingBuffer, ReadLatestSkipsToTheNewest) {
    SpscRingBuffer<float> ring;
    ring.prepare(2, 64);
    write_counting(ring, 0.0f, 40);
    std::array<float, 8> a{};
    const std::array<float*, 1> out{a.data()};
    ASSERT_EQ(ring.read_latest(out, 8), 8u);
    for (size_t i = 0; i < 8; ++i) { EXPECT_EQ(a[i], 32.0f + static_cast<float>(i)); }
    EXPECT_EQ(ring.available(), 0u);
    write_counting(ring, 40.0f, 3);  // fewer than asked: all of them
    EXPECT_EQ(ring.read_latest(out, 8), 3u);
    EXPECT_EQ(a[0], 40.0f);
    EXPECT_EQ(ring.skip(5), 0u);
}

TEST(SpscRingBuffer, ANullSourceWritesSilenceANullDestinationIsSkipped) {
    SpscRingBuffer<float> ring;
    ring.prepare(2, 8);
    const std::array<float, 4> left{1.0f, 2.0f, 3.0f, 4.0f};
    const std::array<const float*, 2> in{left.data(), nullptr};
    ASSERT_EQ(ring.write(in, 4), 4u);
    std::array<float, 4> b{9.0f, 9.0f, 9.0f, 9.0f};
    const std::array<float*, 2> out{nullptr, b.data()};  // channel 0 not wanted
    ASSERT_EQ(ring.read(out, 4), 4u);
    EXPECT_EQ(b, (std::array<float, 4>{}));
    EXPECT_EQ(ring.available(), 0u);  // consumed for every channel
}

TEST(SpscRingBuffer, PrepareAgainEmptiesTheRingAndTheDropCount) {
    SpscRingBuffer<float> ring;
    ring.prepare(2, 16);
    write_counting(ring, 0.0f, 20);  // 4 dropped
    ASSERT_EQ(ring.dropped(), 4u);
    ring.prepare(2, 16);
    EXPECT_EQ(ring.dropped(), 0u);
    EXPECT_EQ(ring.available(), 0u);
    EXPECT_EQ(ring.free_space(), 16u);
    write_counting(ring, 100.0f, 3);
    std::array<float, 3> a{};
    const std::array<float*, 1> out{a.data()};
    ASSERT_EQ(ring.read(out, 3), 3u);
    EXPECT_EQ(a[0], 100.0f);
}

TEST(SpscRingBuffer, ReadLatestAcrossTheWrap) {
    SpscRingBuffer<float> ring;
    ring.prepare(1, 16);
    std::array<float, 16> drain{};
    const std::array<float*, 1> drain_out{drain.data()};
    write_counting(ring, 0.0f, 12);
    ring.read(drain_out, 12);  // positions now 12: the next 10 wrap
    write_counting(ring, 12.0f, 10);
    std::array<float, 6> a{};
    const std::array<float*, 1> out{a.data()};
    ASSERT_EQ(ring.read_latest(out, 6), 6u);
    for (size_t i = 0; i < 6; ++i) { EXPECT_EQ(a[i], 16.0f + static_cast<float>(i)); }
    EXPECT_EQ(ring.available(), 0u);
}

TEST(SpscRingBuffer, OneWriterOneReaderKeepOrderAndCountEveryDrop) {
    // The writer offers 2'000'000 counting samples in blocks of 1 to 64; the reader takes 1 to 48
    // at a time. The writer waits for room for most blocks, so both run side by side through
    // most of the samples (what ThreadSanitizer needs to see), and writes every 8th block
    // without waiting, so some may be dropped (how many depends on scheduling). Every sample read
    // follows the one before it, except where the writer dropped: the gaps add up to dropped().
    // Errors are counted, not asserted, so a failure still joins the writer.
    constexpr size_t k_total = 2'000'000;
    SpscRingBuffer<float> ring;
    ring.prepare(2, 256);
    std::atomic<bool> done{false};
    std::thread writer([&] {
        size_t offered = 0;
        size_t block = 1;
        size_t blocks = 0;
        while (offered < k_total) {
            const size_t count = std::min(block, k_total - offered);
            if (++blocks % 8 != 0) {
                while (ring.free_space() < count) { std::this_thread::yield(); }
            }
            (void)write_counting(ring, static_cast<float>(offered), count);
            offered += count;
            block = (block * 7 + 3) % 64 + 1;
        }
        done.store(true, std::memory_order_release);
    });
    std::array<float, 48> a{};
    std::array<float, 48> b{};
    const std::array<float*, 2> out{a.data(), b.data()};
    double previous = -1.0;
    size_t gaps = 0;
    size_t out_of_order = 0;
    size_t read = 0;
    size_t want = 1;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        const size_t got = ring.read(out, want);
        for (size_t i = 0; i < got; ++i) {
            if (a[i] <= previous || b[i] != -a[i]) {
                ++out_of_order;
                continue;
            }
            gaps += static_cast<size_t>(static_cast<double>(a[i]) - previous - 1.0);
            previous = a[i];
        }
        read += got;
        if (got == 0) { std::this_thread::yield(); }
        want = (want * 5 + 1) % 48 + 1;
        if (finished && ring.available() == 0) { break; }
    }
    writer.join();
    gaps += static_cast<size_t>(static_cast<double>(k_total) - 1.0 - previous);  // dropped at the
                                                                                 // end
    EXPECT_EQ(out_of_order, 0u);
    EXPECT_EQ(gaps, ring.dropped());
    EXPECT_EQ(read + ring.dropped(), k_total);
    EXPECT_GT(read, k_total / 2);  // they really ran side by side
}
