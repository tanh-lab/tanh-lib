#include <gtest/gtest.h>
#include <tanh/dsp/transport/HostTimeFilter.h>

#include <cmath>
#include <cstdint>
#include <random>

using thl::dsp::transport::HostTimeFilter;

namespace {
constexpr double k_rate = 48000.0;
constexpr double k_us_per_sample = 1e6 / k_rate;
constexpr uint32_t k_block = 256;
}  // namespace

TEST(HostTimeFilter, ExactClockMapsExactly) {
    HostTimeFilter filter;
    filter.prepare(k_rate);
    for (int b = 0; b < 100; ++b) {
        const double sample_time = b * static_cast<double>(k_block);
        const auto now =
            static_cast<int64_t>(1000000 + std::llround(sample_time * k_us_per_sample));
        EXPECT_NEAR(static_cast<double>(filter.add(sample_time, now)),
                    static_cast<double>(now),
                    1.0);
    }
}

// Reading the clock inside the callback jitters by up to a millisecond; the
// regression keeps the mapped time within a fraction of that.
TEST(HostTimeFilter, SmoothsCallbackJitter) {
    HostTimeFilter filter;
    filter.prepare(k_rate);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jitter(0.0, 1000.0);
    double worst = 0.0;
    for (int b = 0; b < 2000; ++b) {
        const double sample_time = b * static_cast<double>(k_block);
        const double ideal = 5000000.0 + (sample_time * k_us_per_sample);
        const auto mapped =
            static_cast<double>(filter.add(sample_time, std::llround(ideal + jitter(rng))));
        if (b >= HostTimeFilter::k_points) { worst = std::max(worst, std::abs(mapped - ideal)); }
    }
    EXPECT_LT(worst, 600.0);  // the jitter's mean (500 µs) plus a small error
}
