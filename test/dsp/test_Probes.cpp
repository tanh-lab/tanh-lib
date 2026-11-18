#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/probe/LevelProbe.h>
#include <tanh/dsp/probe/SampleProbe.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <thread>

#include "TestSignals.h"

using thl::core::Buffer;
using thl::core::BufferView;
using thl::dsp::probe::LevelProbe;
using thl::dsp::probe::SampleProbe;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr size_t k_block = 64;

/// Feeds @p signal to @p probe block by block, as a graph would.
template <typename Probe>
void feed(Probe& probe, const Buffer<float>& signal, uint32_t latency = 0) {
    Buffer<float> copy = signal;  // probes only read; the copy keeps the signal const
    for (size_t start = 0; start < copy.get_num_samples(); start += k_block) {
        const size_t length = std::min(k_block, copy.get_num_samples() - start);
        probe.process(BufferView(copy).sub_block(start, length), latency);
    }
}

}  // namespace

TEST(LevelProbe, PeakAndRmsOfASine) {
    LevelProbe probe;
    probe.prepare(k_sample_rate, k_block, 2);
    // 3 s: ten time constants of the 300 ms window, settled.
    feed(probe, dsp_test::sine(2, 144000, 1000.0, 0.5f, k_sample_rate));
    EXPECT_NEAR(probe.peak(0), 0.5f, 1e-3f);
    EXPECT_NEAR(probe.rms(1), 0.5f / std::numbers::sqrt2_v<float>, 2e-3f);
    EXPECT_EQ(probe.peak(5), 0.0f);  // no such channel
}

TEST(LevelProbe, PeakHoldsUntilTaken) {
    LevelProbe probe;
    probe.prepare(k_sample_rate, k_block, 1);
    Buffer<float> spike = dsp_test::constant(1, k_block, 0.1f);
    spike.get_write_pointer(0)[10] = -0.9f;
    feed(probe, spike);
    feed(probe, dsp_test::constant(1, 4 * k_block, 0.1f));  // quieter blocks after it
    EXPECT_NEAR(probe.peak(0), 0.9f, 1e-6f);
    EXPECT_NEAR(probe.take_peak(0), 0.9f, 1e-6f);
    EXPECT_EQ(probe.peak(0), 0.0f);  // taken: the next starts from 0
    feed(probe, dsp_test::constant(1, k_block, 0.1f));
    EXPECT_NEAR(probe.peak(0), 0.1f, 1e-6f);
}

TEST(LevelProbe, RmsWindowIsATimeConstant) {
    LevelProbe probe(10.0f);
    probe.prepare(k_sample_rate, k_block, 1);
    feed(probe, dsp_test::constant(1, 480, 1.0f));  // one window: 1 - 1/e of the mean square
    EXPECT_NEAR(probe.rms(0), std::sqrt(1.0f - std::exp(-1.0f)), 2e-3f);
    probe.reset();
    EXPECT_EQ(probe.rms(0), 0.0f);
    EXPECT_EQ(probe.peak(0), 0.0f);
}

TEST(LevelProbe, NonFiniteSamplesReadAsSilence) {
    LevelProbe probe;
    probe.prepare(k_sample_rate, k_block, 1);
    Buffer<float> block = dsp_test::constant(1, k_block, 0.25f);
    block.get_write_pointer(0)[3] = std::numeric_limits<float>::infinity();
    block.get_write_pointer(0)[4] = std::numeric_limits<float>::quiet_NaN();
    feed(probe, block);
    EXPECT_EQ(probe.peak(0), 0.25f);
    EXPECT_TRUE(std::isfinite(probe.rms(0)));
}

TEST(LevelProbe, AChannelTheBlockLacksReadsAsSilence) {
    LevelProbe probe(10.0f);
    probe.prepare(k_sample_rate, k_block, 2);
    feed(probe, dsp_test::constant(2, 4800, 0.5f));
    probe.take_peak(1);
    feed(probe, dsp_test::constant(1, 4800, 0.5f));  // mono now: channel 1 is silent
    EXPECT_EQ(probe.peak(1), 0.0f);
    // 100 ms of silence through a 10 ms window: the mean square down by e^-10.
    EXPECT_NEAR(probe.rms(1), 0.5f * std::exp(-5.0f), 1e-5f);
    EXPECT_NEAR(probe.rms(0), 0.5f, 1e-3f);
    EXPECT_EQ(probe.peak(LevelProbe::k_max_channels), 0.0f);
}

TEST(LevelProbe, AfterBeingInactiveItStartsFromZero) {
    LevelProbe probe(10.0f);
    probe.prepare(k_sample_rate, k_block, 1);
    feed(probe, dsp_test::constant(1, 4800, 0.8f));
    probe.set_active(false);
    feed(probe, dsp_test::constant(1, 4800, 0.0f));
    EXPECT_NEAR(probe.rms(0), 0.8f, 1e-3f);  // inactive: the last values stay
    probe.set_active(true);
    feed(probe, dsp_test::constant(1, k_block, 0.0f));  // silence after: no old level decaying
    EXPECT_EQ(probe.rms(0), 0.0f);
    EXPECT_EQ(probe.peak(0), 0.0f);
}

TEST(Probe, AnInactiveProbeSeesNothingAndLatencyIsKept) {
    LevelProbe probe;
    probe.prepare(k_sample_rate, k_block, 1);
    probe.set_active(false);
    feed(probe, dsp_test::constant(1, k_block, 0.7f), 12);
    EXPECT_EQ(probe.peak(0), 0.0f);
    EXPECT_EQ(probe.latency_samples(), 0u);
    probe.set_active(true);
    feed(probe, dsp_test::constant(1, k_block, 0.7f), 12);
    EXPECT_NEAR(probe.peak(0), 0.7f, 1e-6f);
    EXPECT_EQ(probe.latency_samples(), 12u);
}

TEST(SampleProbe, TheReaderGetsEveryBlockInOrder) {
    SampleProbe probe;
    probe.prepare(k_sample_rate, k_block, 2);
    EXPECT_GE(probe.capacity(), static_cast<size_t>(0.25 * k_sample_rate));
    const Buffer<float> signal = dsp_test::noise(2, 1000, 1.0f, 3);
    feed(probe, signal);
    ASSERT_EQ(probe.available(), 1000u);
    std::array<float, 1000> a{};
    std::array<float, 1000> b{};
    const std::array<float*, 2> out{a.data(), b.data()};
    const SampleProbe::ReadResult result = probe.read(out, 1000);
    ASSERT_EQ(result.m_read, 1000u);
    EXPECT_FALSE(result.m_discontinuity);  // the first read after prepare() is no gap
    for (size_t i = 0; i < 1000; ++i) {
        ASSERT_EQ(a[i], signal.get_read_pointer(0)[i]) << i;
        ASSERT_EQ(b[i], signal.get_read_pointer(1)[i]) << i;
    }
    EXPECT_EQ(probe.dropped(), 0u);
}

namespace {

/// Feeds @p count samples counting up from @p first (one channel).
void feed_counting(SampleProbe& probe, float first, size_t count) {
    Buffer<float> signal(1, count);
    for (size_t i = 0; i < count; ++i) {
        signal.get_write_pointer(0)[i] = first + static_cast<float>(i);
    }
    feed(probe, signal);
}

}  // namespace

TEST(SampleProbe, AReaderThatFallsBehindStartsOverAfterTheGap) {
    SampleProbe probe(0.001);  // 48 samples, but at least two blocks: 128
    probe.prepare(k_sample_rate, k_block, 1);
    EXPECT_EQ(probe.capacity(), 2 * k_block);
    feed_counting(probe, 0.0f, 300);  // 172 dropped
    EXPECT_EQ(probe.dropped(), 300 - (2 * k_block));
    feed_counting(probe, 1000.0f, 0);
    std::array<float, 256> a{};
    const std::array<float*, 1> out{a.data()};
    // Everything before the gap is skipped: nothing is joined across it.
    SampleProbe::ReadResult result = probe.read(out, 256);
    EXPECT_TRUE(result.m_discontinuity);
    EXPECT_EQ(result.m_read, 0u);
    feed_counting(probe, 1000.0f, 20);
    result = probe.read(out, 256);
    EXPECT_FALSE(result.m_discontinuity);
    ASSERT_EQ(result.m_read, 20u);
    EXPECT_EQ(a[0], 1000.0f);
}

TEST(SampleProbe, ResetStartsTheReaderOver) {
    SampleProbe probe;
    probe.prepare(k_sample_rate, k_block, 1);
    std::array<float, 256> a{};
    const std::array<float*, 1> out{a.data()};
    feed_counting(probe, 0.0f, 100);
    ASSERT_EQ(probe.read(out, 40).m_read, 40u);  // read part of it
    probe.reset();
    feed_counting(probe, 1000.0f, 50);
    const SampleProbe::ReadResult result = probe.read(out, 256);
    EXPECT_TRUE(result.m_discontinuity);
    ASSERT_EQ(result.m_read, 50u);  // the 60 unread from before the reset are skipped
    EXPECT_EQ(a[0], 1000.0f);
    EXPECT_FALSE(probe.read(out, 256).m_discontinuity);  // reported once
}

TEST(SampleProbe, AfterBeingInactiveTheReaderStartsOver) {
    SampleProbe probe;
    probe.prepare(k_sample_rate, k_block, 1);
    std::array<float, 256> a{};
    const std::array<float*, 1> out{a.data()};
    feed_counting(probe, 0.0f, 64);
    probe.set_active(false);
    feed_counting(probe, 500.0f, 64);  // not seen
    EXPECT_EQ(probe.available(), 64u);
    probe.set_active(true);
    feed_counting(probe, 1000.0f, 64);
    const SampleProbe::ReadResult result = probe.read(out, 256);
    EXPECT_TRUE(result.m_discontinuity);
    ASSERT_EQ(result.m_read, 64u);
    EXPECT_EQ(a[0], 1000.0f);
    EXPECT_EQ(probe.sample_rate(), k_sample_rate);
}

TEST(SampleProbe, ProcessLeavesTheBlockUnchanged) {
    SampleProbe probe;
    probe.prepare(k_sample_rate, k_block, 1);
    Buffer<float> block = dsp_test::noise(1, k_block, 1.0f, 4);
    const Buffer<float> before = dsp_test::noise(1, k_block, 1.0f, 4);
    probe.process(BufferView(block), 0);
    for (size_t i = 0; i < k_block; ++i) {
        ASSERT_EQ(block.get_read_pointer(0)[i], before.get_read_pointer(0)[i]);
    }
}

TEST(LevelProbe, NoPeakIsLostBetweenTheAudioThreadAndAReader) {
    // The audio thread feeds blocks with one spike each while a reader takes peaks: the highest
    // spike must turn up in some take, and no take can exceed it.
    LevelProbe probe;
    probe.prepare(k_sample_rate, k_block, 1);
    std::atomic<bool> done{false};
    float highest_fed = 0.0f;
    std::thread audio([&] {
        Buffer<float> block = dsp_test::constant(1, k_block, 0.0f);
        for (int k = 0; k < 200000; ++k) {
            const float spike = static_cast<float>((k * 7919) % 10007) / 10007.0f;
            block.get_write_pointer(0)[k % k_block] = spike;
            probe.process(BufferView(block), 0);
            block.get_write_pointer(0)[k % k_block] = 0.0f;
            highest_fed = std::max(highest_fed, spike);
        }
        done.store(true, std::memory_order_release);
    });
    float highest_taken = 0.0f;
    while (!done.load(std::memory_order_acquire)) {
        highest_taken = std::max(highest_taken, probe.take_peak(0));
    }
    audio.join();
    highest_taken = std::max(highest_taken, probe.take_peak(0));
    EXPECT_EQ(highest_taken, highest_fed);
}

TEST(SampleProbe, AReaderBesideTheAudioThreadNeverJoinsAcrossAGap) {
    // The audio thread feeds counting samples; every 50th block it resets and starts the count
    // over, and the ring (two blocks) drops whenever the reader is slow. The reader checks that
    // every sample continues the one before, unless read() reported a discontinuity.
    SampleProbe probe(0.0);  // the minimum: two blocks
    probe.prepare(k_sample_rate, k_block, 1);
    std::atomic<bool> done{false};
    std::thread audio([&] {
        Buffer<float> block(1, k_block);
        float next = 0.0f;
        for (int k = 0; k < 20000; ++k) {
            if (k % 50 == 49) {
                probe.reset();
                next = 0.0f;  // a new segment: not consecutive with the last (counts stay
                              // below 2^24, where floats still hold every integer)
            }
            for (size_t i = 0; i < k_block; ++i) { block.get_write_pointer(0)[i] = next++; }
            probe.process(BufferView(block), 0);
            if (k % 8 == 0) { std::this_thread::yield(); }
        }
        done.store(true, std::memory_order_release);
    });
    std::array<float, 48> a{};
    const std::array<float*, 1> out{a.data()};
    double previous = -1.0;
    size_t joined_across_a_gap = 0;
    size_t discontinuities = 0;
    size_t read = 0;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        const SampleProbe::ReadResult result = probe.read(out, a.size());
        if (result.m_discontinuity) {
            ++discontinuities;
            previous = -1.0;
        }
        for (size_t i = 0; i < result.m_read; ++i) {
            if (previous >= 0.0 && a[i] != previous + 1.0) { ++joined_across_a_gap; }
            previous = a[i];
        }
        read += result.m_read;
        if (finished && probe.available() == 0) { break; }
    }
    audio.join();
    EXPECT_EQ(joined_across_a_gap, 0u);
    EXPECT_GT(discontinuities, 100u);  // the resets (399) and drops were seen
    EXPECT_GT(read, 0u);
}
