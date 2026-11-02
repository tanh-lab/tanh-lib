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
    ASSERT_EQ(probe.read(out, 1000), 1000u);
    for (size_t i = 0; i < 1000; ++i) {
        ASSERT_EQ(a[i], signal.get_read_pointer(0)[i]) << i;
        ASSERT_EQ(b[i], signal.get_read_pointer(1)[i]) << i;
    }
    EXPECT_EQ(probe.dropped(), 0u);
}

TEST(SampleProbe, AReaderThatFallsBehindIsToldWhatItLost) {
    SampleProbe probe(0.001);  // 48 samples, but at least two blocks: 128
    probe.prepare(k_sample_rate, k_block, 1);
    EXPECT_EQ(probe.capacity(), 2 * k_block);
    feed(probe, dsp_test::constant(1, 300, 0.5f));
    EXPECT_EQ(probe.available(), 2 * k_block);
    EXPECT_EQ(probe.dropped(), 300 - (2 * k_block));
    EXPECT_EQ(probe.skip(1000), 2 * k_block);
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
