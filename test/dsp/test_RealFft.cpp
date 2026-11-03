#include <gtest/gtest.h>
#include <tanh/dsp/fft/RealFft.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <random>
#include <utility>
#include <vector>

using thl::dsp::fft::RealFft;

namespace {

/// The DFT by its definition, in double: the reference every backend must match.
std::vector<std::complex<double>> reference_dft(const std::vector<float>& x) {
    const size_t n = x.size();
    std::vector<std::complex<double>> bins((n / 2) + 1);
    for (size_t k = 0; k < bins.size(); ++k) {
        std::complex<double> sum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double angle =
                -2.0 * std::numbers::pi * static_cast<double>(k * i % n) / static_cast<double>(n);
            sum += static_cast<double>(x[i]) * std::polar(1.0, angle);
        }
        bins[k] = sum;
    }
    return bins;
}

std::vector<float> noise(size_t n, unsigned seed) {
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);
    std::vector<float> x(n);
    for (float& sample : x) { sample = distribution(generator); }
    return x;
}

}  // namespace

TEST(RealFft, MatchesTheDftByDefinition) {
    for (const size_t n : {32u, 64u, 256u, 1024u, 4096u}) {
        RealFft fft;
        ASSERT_TRUE(fft.prepare(n)) << n;
        ASSERT_EQ(fft.num_bins(), (n / 2) + 1);
        const std::vector<float> x = noise(n, static_cast<unsigned>(n));
        std::vector<std::complex<float>> bins(fft.num_bins());
        fft.forward(x, bins);
        const auto reference = reference_dft(x);
        // float rounding grows with log2(N) and the bins' size, about sqrt(N) for noise
        const double tolerance = 1e-5 * static_cast<double>(n);
        for (size_t k = 0; k < bins.size(); ++k) {
            ASSERT_NEAR(bins[k].real(), reference[k].real(), tolerance)
                << RealFft::backend_name() << " " << n << " " << k;
            ASSERT_NEAR(bins[k].imag(), reference[k].imag(), tolerance)
                << RealFft::backend_name() << " " << n << " " << k;
        }
        EXPECT_EQ(bins[0].imag(), 0.0f);
        EXPECT_EQ(bins[n / 2].imag(), 0.0f);
    }
}

TEST(RealFft, ASineLandsInItsBinAtHalfTheSizeTimesItsAmplitude) {
    constexpr size_t k_n = 1024;
    constexpr size_t k_bin = 37;
    RealFft fft;
    ASSERT_TRUE(fft.prepare(k_n));
    std::vector<float> x(k_n);
    for (size_t i = 0; i < k_n; ++i) {
        x[i] = 0.5f * static_cast<float>(
                          std::cos(2.0 * std::numbers::pi * k_bin * static_cast<double>(i) / k_n));
    }
    std::vector<std::complex<float>> bins(fft.num_bins());
    fft.forward(x, bins);
    EXPECT_NEAR(std::abs(bins[k_bin]), 0.5f * k_n / 2, 1e-2f);
    for (size_t k = 0; k < bins.size(); ++k) {
        if (k != k_bin) { ASSERT_LT(std::abs(bins[k]), 1e-2f) << k; }
    }
}

TEST(RealFft, InverseOfForwardIsTheSignal) {
    for (const size_t n : {32u, 512u, 8192u}) {
        RealFft fft;
        ASSERT_TRUE(fft.prepare(n));
        const std::vector<float> x = noise(n, 7);
        std::vector<std::complex<float>> bins(fft.num_bins());
        std::vector<float> back(n);
        fft.forward(x, bins);
        fft.inverse(bins, back);
        for (size_t i = 0; i < n; ++i) { ASSERT_NEAR(back[i], x[i], 1e-5f) << n << " " << i; }
    }
}

TEST(RealFft, ParsevalHolds) {
    constexpr size_t k_n = 2048;
    RealFft fft;
    ASSERT_TRUE(fft.prepare(k_n));
    const std::vector<float> x = noise(k_n, 9);
    std::vector<std::complex<float>> bins(fft.num_bins());
    fft.forward(x, bins);
    double time_energy = 0.0;
    for (const float v : x) { time_energy += static_cast<double>(v) * v; }
    // Every bin but DC and Nyquist stands for itself and its mirror image.
    double bin_energy =
        std::norm(std::complex<double>(bins[0])) + std::norm(std::complex<double>(bins[k_n / 2]));
    for (size_t k = 1; k < k_n / 2; ++k) {
        bin_energy += 2.0 * std::norm(std::complex<double>(bins[k]));
    }
    EXPECT_NEAR(bin_energy / static_cast<double>(k_n), time_energy, 1e-4 * time_energy);
}

TEST(RealFft, PrepareTakesPowersOfTwoInRangeOnly) {
    RealFft fft;
    EXPECT_FALSE(fft.prepare(16));
    EXPECT_FALSE(fft.prepare(1000));
    EXPECT_FALSE(fft.prepare(RealFft::k_max_size * 2));
    EXPECT_EQ(fft.size(), 0u);
    EXPECT_EQ(fft.num_bins(), 0u);
    EXPECT_TRUE(fft.prepare(RealFft::k_min_size));
    EXPECT_TRUE(fft.prepare(RealFft::k_max_size));
    EXPECT_EQ(fft.size(), RealFft::k_max_size);
}

TEST(RealFft, NamesItsBackend) {
    const std::string name = RealFft::backend_name();
    EXPECT_TRUE(name == "pffft" || name == "accelerate") << name;
}

TEST(RealFft, AMovedFromTransformIsUnprepared) {
    RealFft a;
    ASSERT_TRUE(a.prepare(1024));
    RealFft b = std::move(a);
    EXPECT_EQ(b.size(), 1024u);
    EXPECT_EQ(a.size(), 0u);  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move): what
                              // is left behind is the test
    EXPECT_EQ(a.num_bins(), 0u);
    RealFft c;
    ASSERT_TRUE(c.prepare(64));
    c = std::move(b);
    EXPECT_EQ(c.size(), 1024u);
    EXPECT_EQ(b.size(), 0u);  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    // The one moved to still transforms.
    std::vector<float> x(1024, 0.25f);
    std::vector<std::complex<float>> bins(c.num_bins());
    c.forward(x, bins);
    EXPECT_NEAR(bins[0].real(), 256.0f, 1e-3f);
}
