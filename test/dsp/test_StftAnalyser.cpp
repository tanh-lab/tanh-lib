#include <gtest/gtest.h>
#include <tanh/dsp/fft/StftAnalyser.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

using thl::dsp::fft::StftAnalyser;

namespace {

/// Keeps every frame's magnitudes, per channel, in storage allocated up front: process_spectrum()
/// is a real-time context (RTSan).
class Recorder : public StftAnalyser {
public:
    static constexpr size_t k_max_frames = 64;

    bool start(size_t fft_size,
               size_t hop_size,
               size_t num_channels,
               Window window = Window::Hann) {
        if (!prepare(fft_size, hop_size, num_channels, window)) { return false; }
        m_store.assign(
            num_channels,
            std::vector<std::vector<float>>(k_max_frames, std::vector<float>(num_bins())));
        m_count.assign(num_channels, 0);
        return true;
    }

    /// The frames of channel @p channel so far.
    [[nodiscard]] std::span<const std::vector<float>> frames(size_t channel) const {
        return {m_store[channel].data(), m_count[channel]};
    }

protected:
    void process_spectrum(size_t channel, std::span<const std::complex<float>> bins) override {
        if (m_count[channel] == k_max_frames) { return; }
        std::vector<float>& magnitudes = m_store[channel][m_count[channel]++];
        for (size_t k = 0; k < bins.size(); ++k) { magnitudes[k] = std::abs(bins[k]); }
    }

private:
    std::vector<std::vector<std::vector<float>>> m_store;  // [channel][frame][bin]
    std::vector<size_t> m_count;
};

std::vector<float> sine(size_t num_samples, double cycles_per_sample, float amplitude) {
    std::vector<float> x(num_samples);
    for (size_t i = 0; i < num_samples; ++i) {
        x[i] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * cycles_per_sample *
                                                       static_cast<double>(i)));
    }
    return x;
}

void push_mono(StftAnalyser& analyser, const std::vector<float>& x, size_t chunk) {
    for (size_t start = 0; start < x.size(); start += chunk) {
        const std::array<const float*, 1> channels{x.data() + start};
        analyser.push(channels, std::min(chunk, x.size() - start));
    }
}

}  // namespace

TEST(StftAnalyser, ASineReadsItsAmplitudeAtItsBin) {
    for (const auto window : {StftAnalyser::Window::Hann, StftAnalyser::Window::BlackmanHarris}) {
        Recorder analyser;
        ASSERT_TRUE(analyser.start(1024, 256, 1, window));
        push_mono(analyser, sine(4096, 64.0 / 1024.0, 0.5f), 4096);  // bin 64
        ASSERT_FALSE(analyser.frames(0).empty());
        const auto& last = analyser.frames(0).back();
        EXPECT_NEAR(last[64], 0.5f, 1e-3f);
        // Far from it: nothing but the window's sidelobes.
        EXPECT_LT(*std::max_element(last.begin() + 200, last.end()), 1e-3f);
    }
}

TEST(StftAnalyser, FramesComeEveryHopOnceTheFirstIsFull) {
    Recorder analyser;
    ASSERT_TRUE(analyser.start(1024, 256, 1));
    push_mono(analyser, std::vector<float>(1023, 0.1f), 1023);
    EXPECT_TRUE(analyser.frames(0).empty());
    push_mono(analyser, std::vector<float>(1, 0.1f), 1);
    EXPECT_EQ(analyser.frames(0).size(), 1u);
    push_mono(analyser, std::vector<float>(3 * 256 + 100, 0.1f), 64);
    EXPECT_EQ(analyser.frames(0).size(), 4u);  // 1 + 3 hops
}

TEST(StftAnalyser, ChunkSizesDoNotChangeTheFrames) {
    const std::vector<float> x = sine(5000, 0.0371, 0.3f);
    Recorder whole;
    Recorder pieces;
    ASSERT_TRUE(whole.start(512, 100, 1));
    ASSERT_TRUE(pieces.start(512, 100, 1));
    push_mono(whole, x, x.size());
    push_mono(pieces, x, 37);
    ASSERT_EQ(whole.frames(0).size(), pieces.frames(0).size());
    EXPECT_EQ(whole.frames(0).size(), (5000 - 512) / 100 + 1);
    // The same samples in the same frames (to rounding: an FFT may round differently with
    // another buffer's alignment, as Accelerate does).
    for (size_t f = 0; f < whole.frames(0).size(); ++f) {
        for (size_t k = 0; k < whole.num_bins(); ++k) {
            ASSERT_NEAR(whole.frames(0)[f][k], pieces.frames(0)[f][k], 1e-6f) << f << " " << k;
        }
    }
}

TEST(StftAnalyser, ChannelsAreAnalysedApartAndAMissingOneIsSilence) {
    Recorder analyser;
    ASSERT_TRUE(analyser.start(256, 256, 3));
    const std::vector<float> left = sine(256, 10.0 / 256.0, 1.0f);
    const std::vector<float> right = sine(256, 40.0 / 256.0, 0.25f);
    const std::array<const float*, 2> channels{left.data(), right.data()};  // no third
    analyser.push(channels, 256);
    EXPECT_NEAR(analyser.frames(0)[0][10], 1.0f, 1e-3f);
    EXPECT_NEAR(analyser.frames(1)[0][40], 0.25f, 1e-3f);
    EXPECT_LT(analyser.frames(1)[0][10], 1e-3f);
    EXPECT_EQ(*std::max_element(analyser.frames(2)[0].begin(), analyser.frames(2)[0].end()), 0.0f);
}

TEST(StftAnalyser, ResetDropsWhatWasCollected) {
    Recorder analyser;
    ASSERT_TRUE(analyser.start(512, 128, 1));
    push_mono(analyser, std::vector<float>(600, 1.0f), 600);  // one frame, 88 towards the next
    ASSERT_EQ(analyser.frames(0).size(), 1u);
    analyser.reset();
    push_mono(analyser, std::vector<float>(511, 0.0f), 511);
    EXPECT_EQ(analyser.frames(0).size(), 1u);  // a full fft_size() is needed again
    push_mono(analyser, std::vector<float>(1, 0.0f), 1);
    ASSERT_EQ(analyser.frames(0).size(), 2u);
    // Only silence in it: nothing from before the reset.
    EXPECT_EQ(*std::max_element(analyser.frames(0)[1].begin(), analyser.frames(0)[1].end()), 0.0f);
}

TEST(StftAnalyser, BlackmanHarrisLeaksLessThanHann) {
    // A sine halfway between bins: near it, outside the main lobes, there is only the window's
    // sidelobes. Hann's fall from -31 dB at 18 dB per octave, Blackman-Harris's stay under
    // -92 dB; far away both are tiny, so measure 6 to 40 bins from the sine.
    const std::vector<float> x = sine(2048, 100.5 / 2048.0, 1.0f);
    const auto far_leak = [&](StftAnalyser::Window window) {
        Recorder analyser;
        EXPECT_TRUE(analyser.start(2048, 2048, 1, window));
        push_mono(analyser, x, x.size());
        const auto& bins = analyser.frames(0)[0];
        return *std::max_element(bins.begin() + 106, bins.begin() + 140);
    };
    const float hann = far_leak(StftAnalyser::Window::Hann);
    const float blackman_harris = far_leak(StftAnalyser::Window::BlackmanHarris);
    EXPECT_LT(20.0f * std::log10(blackman_harris), -90.0f);
    EXPECT_GT(20.0f * std::log10(hann), -90.0f);
}

TEST(StftAnalyser, PrepareRejectsWhatItCannotDo) {
    Recorder analyser;
    EXPECT_FALSE(analyser.prepare(1000, 100, 1));  // not a power of two
    EXPECT_FALSE(analyser.prepare(1024, 0, 1));
    EXPECT_FALSE(analyser.prepare(1024, 2048, 1));
    EXPECT_EQ(analyser.fft_size(), 0u);
    const std::array<const float*, 1> none{nullptr};
    analyser.push(none, 10);  // unprepared: nothing happens, process_spectrum() not called
}

TEST(StftAnalyser, AFailedPrepareLeavesItUnprepared) {
    // A prepared analyser that fails a second prepare() must not keep its old FFT with no hop:
    // push() would never get past the first frame.
    Recorder analyser;
    ASSERT_TRUE(analyser.start(1024, 256, 1));
    EXPECT_FALSE(analyser.prepare(1024, 0, 1));
    EXPECT_EQ(analyser.fft_size(), 0u);
    EXPECT_EQ(analyser.hop_size(), 0u);
    EXPECT_FALSE(analyser.prepare(1024, 2048, 1));
    EXPECT_EQ(analyser.fft_size(), 0u);
}

TEST(StftAnalyser, DcAndNyquistReadTheirAmplitudeToo) {
    // They have no mirror image: the sine's factor 2 must not apply to them.
    constexpr size_t k_n = 256;
    Recorder analyser;
    ASSERT_TRUE(analyser.start(k_n, k_n, 2));
    std::vector<float> dc(k_n, 0.5f);
    std::vector<float> nyquist(k_n);
    for (size_t i = 0; i < k_n; ++i) { nyquist[i] = (i % 2 == 0) ? 0.25f : -0.25f; }
    const std::array<const float*, 2> channels{dc.data(), nyquist.data()};
    analyser.push(channels, k_n);
    ASSERT_EQ(analyser.frames(0).size(), 1u);
    EXPECT_NEAR(analyser.frames(0)[0][0], 0.5f, 1e-4f);
    EXPECT_NEAR(analyser.frames(1)[0][k_n / 2], 0.25f, 1e-4f);
}
