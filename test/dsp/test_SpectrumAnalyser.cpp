#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/analysis/SpectrumAnalyser.h>
#include <tanh/dsp/probe/SampleProbe.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <span>
#include <vector>

#include "TestSignals.h"

using thl::core::Buffer;
using thl::core::BufferView;
using thl::dsp::analysis::SpectrumAnalyser;
using thl::dsp::probe::SampleProbe;

namespace {

constexpr double k_sample_rate = 48000.0;

SpectrumAnalyser::Settings flat() {
    SpectrumAnalyser::Settings settings;
    settings.m_tilt_db_per_octave = 0.0f;
    return settings;
}

void feed(SpectrumAnalyser& analyser, const Buffer<float>& signal) {
    std::vector<const float*> channels;
    channels.reserve(signal.get_num_channels());
    for (size_t ch = 0; ch < signal.get_num_channels(); ++ch) {
        channels.push_back(signal.get_read_pointer(ch));
    }
    analyser.push(channels, signal.get_num_samples());
}

/// The curve's value at the point nearest @p frequency.
float at(const SpectrumAnalyser& analyser, float frequency) {
    const auto f = analyser.frequencies();
    const auto nearest = std::ranges::min_element(f, [&](float a, float b) {
        return std::abs(std::log(a / frequency)) < std::abs(std::log(b / frequency));
    });
    return analyser.curve_db()[static_cast<size_t>(std::distance(f.begin(), nearest))];
}

/// A sine exactly at a bin of the 4096-point FFT near @p frequency.
double bin_frequency(double frequency) {
    const double bin_hz = k_sample_rate / 4096.0;
    return std::round(frequency / bin_hz) * bin_hz;
}

}  // namespace

TEST(SpectrumAnalyser, AFullScaleSineReadsZeroDecibelsAtItsFrequency) {
    SpectrumAnalyser analyser;
    ASSERT_TRUE(analyser.prepare(k_sample_rate, 2, flat()));
    const double f = bin_frequency(1000.0);
    feed(analyser, dsp_test::sine(2, 16384, f, 1.0f, k_sample_rate));
    ASSERT_GT(analyser.num_frames(), 0u);
    EXPECT_NEAR(at(analyser, static_cast<float>(f)), 0.0f, 0.1f);
    EXPECT_LT(at(analyser, 4000.0f), -60.0f);  // two octaves away: nothing but the window
    const auto curve = analyser.curve_db();
    const float highest = *std::ranges::max_element(curve);
    EXPECT_NEAR(highest, 0.0f, 0.1f);
}

TEST(SpectrumAnalyser, AnAmplitudeReadsItsLevelAndChannelsAverageInPower) {
    SpectrumAnalyser analyser;
    ASSERT_TRUE(analyser.prepare(k_sample_rate, 2, flat()));
    const double f = bin_frequency(500.0);
    Buffer<float> signal = dsp_test::sine(2, 16384, f, 0.5f, k_sample_rate);  // -6.02 dB
    std::fill_n(signal.get_write_pointer(1), 16384, 0.0f);  // right silent: half the power
    feed(analyser, signal);
    EXPECT_NEAR(at(analyser, static_cast<float>(f)), -6.02f - 3.01f, 0.1f);
}

TEST(SpectrumAnalyser, TiltAddsItsSlopePerOctaveAroundOneKilohertz) {
    // White noise is flat; smoothed over a third of an octave its curve is flat too, so what
    // remains is the tilt.
    SpectrumAnalyser::Settings settings = flat();
    settings.m_smoothing_octaves = 1.0f / 3.0f;
    settings.m_tilt_db_per_octave = 4.5f;
    settings.m_release_db_per_second = 0.0f;  // the curve is the maximum: average the frames
    SpectrumAnalyser analyser;
    ASSERT_TRUE(analyser.prepare(k_sample_rate, 1, settings));
    feed(analyser, dsp_test::noise(1, 4096, 0.5f, 1));
    // One frame: compare octaves well apart, the noise's own spread is a few dB.
    const float at_250 = at(analyser, 250.0f);
    const float at_16k = at(analyser, 16000.0f);
    EXPECT_NEAR((at_16k - at_250) / 6.0f, 4.5f, 1.0f);
    // Pivot: tilt 0 gives the same value at 1 kHz.
    SpectrumAnalyser untilted;
    settings.m_tilt_db_per_octave = 0.0f;
    ASSERT_TRUE(untilted.prepare(k_sample_rate, 1, settings));
    feed(untilted, dsp_test::noise(1, 4096, 0.5f, 1));
    EXPECT_NEAR(at(analyser, 1000.0f), at(untilted, 1000.0f), 0.5f);
}

TEST(SpectrumAnalyser, ItRisesAtOnceAndFallsAtTheReleaseRate) {
    SpectrumAnalyser::Settings settings = flat();
    settings.m_release_db_per_second = 20.0f;
    SpectrumAnalyser analyser;
    ASSERT_TRUE(analyser.prepare(k_sample_rate, 1, settings));
    const double f = bin_frequency(1000.0);
    feed(analyser, dsp_test::sine(1, 8192, f, 1.0f, k_sample_rate));
    ASSERT_NEAR(at(analyser, static_cast<float>(f)), 0.0f, 0.1f);
    const size_t frames = analyser.num_frames();
    // Half a second of silence: 20 dB/s, so 10 dB down, give or take a frame.
    feed(analyser, dsp_test::constant(1, 24000, 0.0f));
    const float seconds = static_cast<float>(analyser.num_frames() - frames) * 1024.0f /
                          static_cast<float>(k_sample_rate);
    EXPECT_NEAR(at(analyser, static_cast<float>(f)), -20.0f * seconds, 0.5f);
}

TEST(SpectrumAnalyser, FreezeHoldsTheMaximum) {
    SpectrumAnalyser analyser;
    ASSERT_TRUE(analyser.prepare(k_sample_rate, 1, flat()));
    analyser.set_freeze(true);
    const double f = bin_frequency(2000.0);
    feed(analyser, dsp_test::sine(1, 8192, f, 1.0f, k_sample_rate));
    feed(analyser, dsp_test::constant(1, 96000, 0.0f));  // two seconds of silence
    EXPECT_NEAR(at(analyser, static_cast<float>(f)), 0.0f, 0.1f);
    analyser.set_freeze(false);
    feed(analyser, dsp_test::constant(1, 48000, 0.0f));
    EXPECT_LT(at(analyser, static_cast<float>(f)), -20.0f);  // falls again
}

TEST(SpectrumAnalyser, SmoothingSpreadsASineOverItsBand) {
    SpectrumAnalyser::Settings settings = flat();
    settings.m_smoothing_octaves = 1.0f;
    SpectrumAnalyser smoothed;
    ASSERT_TRUE(smoothed.prepare(k_sample_rate, 1, settings));
    const double f = bin_frequency(4000.0);
    feed(smoothed, dsp_test::sine(1, 16384, f, 1.0f, k_sample_rate));
    // An octave at 4 kHz holds about 0.7 · 4000 / 11.7 ≈ 240 bins: the sine's power over them.
    EXPECT_LT(at(smoothed, static_cast<float>(f)), -15.0f);
    EXPECT_GT(at(smoothed, static_cast<float>(f)), -30.0f);
}

TEST(SpectrumAnalyser, BelowTheFloorItReadsTheFloor) {
    SpectrumAnalyser::Settings settings = flat();
    settings.m_floor_db = -90.0f;
    SpectrumAnalyser analyser;
    ASSERT_TRUE(analyser.prepare(k_sample_rate, 1, settings));
    feed(analyser, dsp_test::constant(1, 8192, 0.0f));
    for (const float value : analyser.curve_db()) { ASSERT_EQ(value, -90.0f); }
    analyser.set_floor(-60.0f);
    for (const float value : analyser.curve_db()) { ASSERT_EQ(value, -60.0f); }
}

TEST(SpectrumAnalyser, ReadFromAProbeStartsTheFramesOverAfterADiscontinuity) {
    SampleProbe probe;
    probe.prepare(k_sample_rate, 512, 1);
    SpectrumAnalyser analyser;
    ASSERT_TRUE(analyser.prepare(k_sample_rate, 1, flat()));
    Buffer<float> block = dsp_test::sine(1, 512, bin_frequency(1000.0), 1.0f, k_sample_rate);
    for (int i = 0; i < 7; ++i) { probe.process(BufferView(block), 0); }  // 3584: no frame yet
    analyser.read_from(probe);
    EXPECT_EQ(analyser.num_frames(), 0u);
    probe.reset();  // a discontinuity: what came before must not make a frame with what follows
    for (int i = 0; i < 7; ++i) { probe.process(BufferView(block), 0); }
    analyser.read_from(probe);
    EXPECT_EQ(analyser.num_frames(), 0u);  // still 3584 since the gap
    probe.process(BufferView(block), 0);
    analyser.read_from(probe);
    EXPECT_EQ(analyser.num_frames(), 1u);
}

TEST(SpectrumAnalyser, ResolutionSetsTheFftSize) {
    SpectrumAnalyser analyser;
    ASSERT_TRUE(analyser.prepare(k_sample_rate, 1, flat()));
    EXPECT_EQ(analyser.fft_size(), 4096u);
    EXPECT_EQ(analyser.hop_size(), 1024u);
    ASSERT_TRUE(analyser.set_resolution(SpectrumAnalyser::Resolution::Low));
    EXPECT_EQ(analyser.fft_size(), 1024u);
    ASSERT_TRUE(analyser.set_resolution(SpectrumAnalyser::Resolution::Maximum));
    EXPECT_EQ(analyser.fft_size(), 8192u);
    EXPECT_EQ(analyser.curve_db().size(), 256u);
}

TEST(SpectrumAnalyser, PrepareRejectsSettingsItCannotUse) {
    SpectrumAnalyser analyser;
    SpectrumAnalyser::Settings settings = flat();
    settings.m_num_points = 1;
    EXPECT_FALSE(analyser.prepare(k_sample_rate, 1, settings));
    settings = flat();
    settings.m_min_frequency = 20000.0f;
    settings.m_max_frequency = 20.0f;
    EXPECT_FALSE(analyser.prepare(k_sample_rate, 1, settings));
    EXPECT_FALSE(analyser.prepare(k_sample_rate, 0, flat()));
    EXPECT_TRUE(analyser.curve_db().empty());
}
