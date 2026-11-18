#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/fx/StereoWidener.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

#include "TestSignals.h"

using thl::core::Buffer;
using thl::core::BufferView;
using thl::dsp::fx::StereoWidenerImpl;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr size_t k_block = 64;

class TestWidener : public StereoWidenerImpl {
public:
    explicit TestWidener(size_t num_bands = 2)
        : StereoWidenerImpl(num_bands, 4), m_widths(num_bands, 1.0f) {}

    std::vector<float> m_widths;
    std::vector<float> m_crossovers;  // empty: set_frequency() decides
    // The first crossover reads' offsets; fixed, as process() runs under RTSan.
    std::array<uint32_t, 64> m_offsets{};
    size_t m_num_offsets = 0;

protected:
    float get_band_parameter_float(BandParameter, size_t band, uint32_t) override {
        return m_widths[band];
    }
    float get_crossover_parameter_float(size_t crossover, uint32_t modulation_offset) override {
        if (crossover == 0 && m_num_offsets < m_offsets.size()) {
            m_offsets[m_num_offsets++] = modulation_offset;
        }
        return crossover < m_crossovers.size()
                   ? m_crossovers[crossover]
                   : StereoWidenerImpl::get_crossover_parameter_float(crossover, modulation_offset);
    }
};

/// A sine only in the side signal (L = s, R = -s) at @p frequency.
Buffer<float> side_sine(double frequency, size_t num_samples) {
    Buffer<float> signal(2, num_samples);
    for (size_t n = 0; n < num_samples; ++n) {
        const auto s = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * frequency *
                                                         static_cast<double>(n) / k_sample_rate));
        signal.get_write_pointer(0)[n] = s;
        signal.get_write_pointer(1)[n] = -s;
    }
    return signal;
}

void run(TestWidener& widener, Buffer<float>& signal, Buffer<float>* dry = nullptr) {
    dsp_test::process_in_blocks(widener, signal, k_block, dry);
}

}  // namespace

TEST(StereoWidener, TheMonoSumDoesNotDependOnTheWidths) {
    // L + R is the mid signal through the crossovers' all-passes, whatever the widths.
    const Buffer<float> input = dsp_test::noise(2, 8192, 0.5f, 1);
    TestWidener neutral(3);
    neutral.prepare(k_sample_rate, k_block, 2);
    Buffer<float> reference = input;
    run(neutral, reference);
    for (const auto& widths : {std::vector<float>{0.0f, 0.0f, 0.0f},
                               std::vector<float>{0.0f, 1.5f, 2.0f},
                               std::vector<float>{2.0f, 0.3f, 1.0f}}) {
        TestWidener widener(3);
        widener.m_widths = widths;
        widener.prepare(k_sample_rate, k_block, 2);
        Buffer<float> out = input;
        run(widener, out);
        for (size_t i = 0; i < out.get_num_samples(); ++i) {
            const float mono = out.get_read_pointer(0)[i] + out.get_read_pointer(1)[i];
            const float want = reference.get_read_pointer(0)[i] + reference.get_read_pointer(1)[i];
            ASSERT_NEAR(mono, want, 1e-5f) << i;
        }
    }
}

TEST(StereoWidener, WidthZeroIsMono) {
    TestWidener widener(2);
    widener.m_widths = {0.0f, 0.0f};
    widener.prepare(k_sample_rate, k_block, 2);
    Buffer<float> out = dsp_test::noise(2, 4096, 0.5f, 2);
    run(widener, out);
    for (size_t i = 0; i < out.get_num_samples(); ++i) {
        ASSERT_EQ(out.get_read_pointer(0)[i], out.get_read_pointer(1)[i]) << i;
    }
}

TEST(StereoWidener, MonoBassRemovesTheSideBelowTheCrossover) {
    // Band 0 at width 0, the crossover at 150 Hz: the side signal's bass goes, its highs stay.
    const auto side_level = [](double frequency) {
        TestWidener widener(2);
        widener.m_widths = {0.0f, 1.0f};
        widener.m_crossovers = {150.0f};
        widener.prepare(k_sample_rate, k_block, 2);
        Buffer<float> out = side_sine(frequency, 48000);
        run(widener, out);
        Buffer<float> side(1, out.get_num_samples());
        for (size_t i = 0; i < out.get_num_samples(); ++i) {
            side.get_write_pointer(0)[i] =
                0.5f * (out.get_read_pointer(0)[i] - out.get_read_pointer(1)[i]);
        }
        return 20.0 * std::log10(dsp_test::settled_rms(side, 0) / (0.5 / std::numbers::sqrt2));
    };
    EXPECT_LT(side_level(30.0), -40.0);  // 2.3 octaves below: LR4 at 24 dB/octave and more
    EXPECT_NEAR(side_level(3000.0), 0.0, 0.05);
}

TEST(StereoWidener, WidthScalesTheSideOfItsBand) {
    for (const float width : {0.5f, 1.0f, 2.0f}) {
        TestWidener widener(2);
        widener.m_widths = {1.0f, width};
        widener.m_crossovers = {150.0f};
        widener.prepare(k_sample_rate, k_block, 2);
        Buffer<float> out = side_sine(3000.0, 48000);
        run(widener, out);
        // Only side in: left is +side, so its level is the side's.
        EXPECT_NEAR(20.0 * std::log10(dsp_test::settled_rms(out, 0) / (0.5 / std::numbers::sqrt2)),
                    20.0 * std::log10(width),
                    0.05)
            << width;
    }
}

TEST(StereoWidener, NeutralIsTheInputThroughFlatAllPassesAndTheDrySignal) {
    TestWidener widener(3);
    widener.prepare(k_sample_rate, k_block, 2);
    Buffer<float> out = dsp_test::noise(2, 8192, 0.5f, 3);
    Buffer<float> dry(2, out.get_num_samples());
    run(widener, out, &dry);
    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t i = 0; i < out.get_num_samples(); ++i) {
            ASSERT_NEAR(out.get_read_pointer(ch)[i], dry.get_read_pointer(ch)[i], 1e-6f) << i;
        }
    }
    // Flat: a sine at any frequency keeps its level.
    for (const double frequency : {50.0, 500.0, 5000.0}) {
        TestWidener flat(3);
        flat.prepare(k_sample_rate, k_block, 2);
        Buffer<float> sine = side_sine(frequency, 48000);
        run(flat, sine);
        EXPECT_NEAR(20.0 * std::log10(dsp_test::settled_rms(sine, 0) / (0.5 / std::numbers::sqrt2)),
                    0.0,
                    0.01)
            << frequency;
    }
}

TEST(StereoWidener, AWidthChangeGlides) {
    TestWidener widener(2);
    widener.prepare(k_sample_rate, k_block, 2);
    Buffer<float> warm = side_sine(3000.0, 4800);
    run(widener, warm);
    widener.m_widths = {2.0f, 2.0f};
    Buffer<float> out = side_sine(3000.0, k_block);
    // Continue the sine where the warm-up ended.
    for (size_t i = 0; i < k_block; ++i) {
        const auto s =
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 3000.0 *
                                              static_cast<double>(4800 + i) / k_sample_rate));
        out.get_write_pointer(0)[i] = s;
        out.get_write_pointer(1)[i] = -s;
    }
    run(widener, out);
    // 20 ms glide: after one block of 64 samples the side has risen by about 64 / 960 of the way.
    double peak = 0.0;
    for (size_t i = 0; i < k_block; ++i) {
        peak = std::max(peak, std::abs(static_cast<double>(out.get_read_pointer(0)[i])));
    }
    EXPECT_LT(peak, 0.5 * 1.1);
}

TEST(StereoWidener, AnythingButStereoPassesUnchanged) {
    TestWidener widener(2);
    widener.m_widths = {0.0f, 2.0f};
    widener.prepare(k_sample_rate, k_block, 1);
    std::vector<float> mono(k_block);
    for (size_t i = 0; i < mono.size(); ++i) { mono[i] = static_cast<float>(i) * 0.01f; }
    const auto input = mono;
    widener.process(BufferView(mono.data(), mono.size()));
    EXPECT_EQ(mono, input);
}

TEST(StereoWidener, CrossoversComeFromTheirParametersAtTheBlockPosition) {
    TestWidener widener(3);
    widener.m_crossovers = {2000.0f, 300.0f};  // crossing: sorted
    widener.prepare(k_sample_rate, k_block, 2);
    EXPECT_FLOAT_EQ(widener.get_frequency(0), 300.0f);
    EXPECT_FLOAT_EQ(widener.get_frequency(1), 2000.0f);
    widener.m_num_offsets = 0;
    Buffer<float> block(2, k_block);
    const std::array<uint32_t, 2> change_points{16, 40};
    widener.process_modulated(BufferView(block), change_points);
    ASSERT_EQ(widener.m_num_offsets, 3u);
    EXPECT_EQ(widener.m_offsets[0], 0u);
    EXPECT_EQ(widener.m_offsets[1], 16u);
    EXPECT_EQ(widener.m_offsets[2], 40u);
}

TEST(StereoWidener, ResetClearsTheCrossovers) {
    TestWidener widener(2);
    widener.prepare(k_sample_rate, k_block, 2);
    Buffer<float> noise = dsp_test::noise(2, 4096, 0.5f, 4);
    run(widener, noise);
    widener.reset();
    Buffer<float> silence(2, 512);
    run(widener, silence);
    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t i = 0; i < silence.get_num_samples(); ++i) {
            ASSERT_EQ(silence.get_read_pointer(ch)[i], 0.0f) << i;
        }
    }
}
