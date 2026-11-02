#include <gtest/gtest.h>
#include <tanh/core/threading/SampleHistory.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <thread>

using thl::core::SampleHistory;

namespace {

using History = SampleHistory<float>;

/// Writes @p count samples: channel 0 counts up from @p first, channel 1 is its negative.
void write_counting(History& history, size_t first, size_t count) {
    std::array<float, 256> ch0{};
    std::array<float, 256> ch1{};
    for (size_t i = 0; i < count; ++i) {
        ch0[i] = static_cast<float>(first + i);
        ch1[i] = -ch0[i];
    }
    const std::array<const float*, 2> channels{ch0.data(), ch1.data()};
    history.write(channels, count);
}

}  // namespace

TEST(SampleHistory, PreparedEmptyWithAPowerOfTwoCapacity) {
    History history;
    history.prepare(2, 100);
    EXPECT_EQ(history.capacity(), 256u);  // twice the longest read, a power of two
    EXPECT_EQ(history.written(), 0u);
    std::array<float, 8> a{};
    const std::array<float*, 1> out{a.data()};
    EXPECT_EQ(history.read_latest(out, 8), 0u);
}

TEST(SampleHistory, ReadLatestGivesTheNewestWindowAcrossTheWrap) {
    History history;
    history.prepare(2, 8);  // capacity 16
    std::array<float, 16> a{};
    std::array<float, 16> b{};
    const std::array<float*, 2> out{a.data(), b.data()};
    size_t next = 0;
    for (int round = 0; round < 20; ++round) {  // 20 · 7 samples through 16: wraps often
        write_counting(history, next, 7);
        next += 7;
        const size_t want = std::min<size_t>(10, next);
        ASSERT_EQ(history.read_latest(out, 10), want);
        for (size_t i = 0; i < want; ++i) {
            ASSERT_EQ(a[i], static_cast<float>(next - want + i)) << round;
            ASSERT_EQ(b[i], -a[i]);
        }
    }
    // Asking for more than the capacity gives the capacity; reading does not consume.
    std::array<float, 32> c{};
    const std::array<float*, 1> wide{c.data()};
    EXPECT_EQ(history.read_latest(wide, 32), 16u);
    EXPECT_EQ(history.read_latest(wide, 32), 16u);
    EXPECT_EQ(c[15], static_cast<float>(next - 1));
}

TEST(SampleHistory, ABlockLongerThanTheCapacityKeepsItsEnd) {
    History history;
    history.prepare(2, 8);  // capacity 16
    write_counting(history, 0, 40);
    EXPECT_EQ(history.written(), 40u);
    std::array<float, 16> a{};
    const std::array<float*, 1> out{a.data()};
    ASSERT_EQ(history.read_latest(out, 16), 16u);
    for (size_t i = 0; i < 16; ++i) { EXPECT_EQ(a[i], static_cast<float>(24 + i)); }
}

TEST(SampleHistory, MissingChannelsReadAsZeroExtraOnesAreIgnored) {
    History history;
    history.prepare(2, 8);
    const std::array<float, 4> one{1.0f, 2.0f, 3.0f, 4.0f};
    const std::array<const float*, 1> in{one.data()};
    history.write(in, 4);
    std::array<float, 4> a{};
    std::array<float, 4> b{9.0f, 9.0f, 9.0f, 9.0f};
    std::array<float, 4> c{9.0f, 9.0f, 9.0f, 9.0f};
    const std::array<float*, 3> out{a.data(), b.data(), c.data()};
    ASSERT_EQ(history.read_latest(out, 4), 4u);
    EXPECT_EQ(a, one);
    EXPECT_EQ(b, (std::array<float, 4>{}));
    EXPECT_EQ(c, (std::array<float, 4>{9.0f, 9.0f, 9.0f, 9.0f}));  // no third channel
}

TEST(SampleHistory, ReadSinceFollowsACursorAndCountsWhatItLost) {
    History history;
    history.prepare(2, 8);  // capacity 16
    History::Cursor cursor = history.cursor_now();
    std::array<float, 16> a{};
    const std::array<float*, 1> out{a.data()};
    write_counting(history, 0, 10);
    History::ReadResult result = history.read_since(cursor, out, 16);
    EXPECT_EQ(result.m_read, 10u);
    EXPECT_EQ(result.m_lost, 0u);
    EXPECT_EQ(a[9], 9.0f);
    result = history.read_since(cursor, out, 16);  // nothing new
    EXPECT_EQ(result.m_read, 0u);
    // 30 more: the cursor fell 14 behind the 16 still there.
    write_counting(history, 10, 30);
    result = history.read_since(cursor, out, 8);
    EXPECT_EQ(result.m_lost, 14u);
    EXPECT_EQ(result.m_read, 8u);
    EXPECT_EQ(a[0], 24.0f);
    result = history.read_since(cursor, out, 16);
    EXPECT_EQ(result.m_read, 8u);
    EXPECT_EQ(a[7], 39.0f);
    // A cursor taken now skips everything before.
    History::Cursor late = history.cursor_now();
    write_counting(history, 40, 3);
    result = history.read_since(late, out, 16);
    EXPECT_EQ(result.m_read, 3u);
    EXPECT_EQ(a[0], 40.0f);
}

TEST(SampleHistory, ReadersSideBySideWithTheWriterSeeOnlyWholeWindows) {
    // The writer writes 2'000'000 counting samples through a small history (64) in blocks of 1 to
    // 64, overwriting all the time. Two readers run beside it:
    // - one copies windows of 48 newest: every window it returns must be consecutive values
    //   (channel 1 their negatives): never a mix of old and overwritten samples;
    // - one follows a cursor: every sample it reads follows the one before, and what it read plus
    //   what it was told it lost is everything.
    constexpr size_t k_total = 2'000'000;
    History history;
    history.prepare(2, 32);  // capacity 64: small on purpose, so overwrites meet reads
    std::atomic<bool> done{false};
    std::thread writer([&] {
        size_t written = 0;
        size_t block = 1;
        while (written < k_total) {
            const size_t count = std::min(block, k_total - written);
            write_counting(history, written, count);
            written += count;
            block = (block * 7 + 3) % 64 + 1;
            if (written % 1024 < count) { std::this_thread::yield(); }  // let the readers in
        }
        done.store(true, std::memory_order_release);
    });

    // Errors are counted, not asserted, so a failure still joins both threads.
    std::atomic<size_t> torn_windows{0};
    std::atomic<size_t> windows_while_writing{0};
    std::thread window_reader([&] {
        std::array<float, 48> a{};
        std::array<float, 48> b{};
        const std::array<float*, 2> out{a.data(), b.data()};
        while (!done.load(std::memory_order_acquire)) {
            const size_t got = history.read_latest(out, 48);
            bool whole = true;
            for (size_t i = 0; i < got; ++i) {
                whole = whole && b[i] == -a[i] && (i == 0 || a[i] == a[i - 1] + 1.0f);
            }
            if (!whole) { torn_windows.fetch_add(1, std::memory_order_relaxed); }
            if (got > 0) { windows_while_writing.fetch_add(1, std::memory_order_relaxed); }
        }
    });

    History::Cursor cursor;
    std::array<float, 32> a{};
    std::array<float, 32> b{};
    const std::array<float*, 2> out{a.data(), b.data()};
    double previous = -1.0;
    size_t read = 0;
    size_t lost = 0;
    size_t out_of_order = 0;
    size_t reads_while_writing = 0;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        const History::ReadResult result = history.read_since(cursor, out, 32);
        for (size_t i = 0; i < result.m_read; ++i) {
            if (b[i] != -a[i] || a[i] <= previous) { ++out_of_order; }
            previous = a[i];
        }
        read += result.m_read;
        lost += result.m_lost;
        if (!finished && result.m_read > 0) { ++reads_while_writing; }
        if (finished && cursor.m_position == history.written()) { break; }
    }
    writer.join();
    window_reader.join();
    EXPECT_EQ(torn_windows.load(), 0u);
    EXPECT_EQ(out_of_order, 0u);
    EXPECT_EQ(read + lost, k_total);
    EXPECT_GT(read, 0u);
    // They really ran beside the writer.
    EXPECT_GT(windows_while_writing.load(), 100u);
    EXPECT_GT(reads_while_writing, 100u);
}
