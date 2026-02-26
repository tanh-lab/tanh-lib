#include <tanh/dsp/resonator/rings/DspFunctions.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <numbers>
#include <vector>

namespace rings = thl::dsp::resonator::rings;

namespace {

inline float interpolate_ref(const std::array<float, 257>& table, float index, float size) {
    const float scaled = index * size;
    const int32_t i = static_cast<int32_t>(scaled);
    const float frac = scaled - static_cast<float>(i);
    const float a = table[static_cast<size_t>(i)];
    const float b = table[static_cast<size_t>(i + 1)];
    return a + (b - a) * frac;
}

std::array<float, 257> make_ref_4_decades() {
    std::array<float, 257> table {};
    for (size_t i = 0; i < table.size(); ++i) {
        const float x = static_cast<float>(i) / 256.0f;
        table[i] = std::pow(10.0f, 4.0f * x);
    }
    return table;
}

std::array<float, 257> make_ref_svf_shift() {
    std::array<float, 257> table {};
    for (size_t i = 0; i < table.size(); ++i) {
        const float semitones = static_cast<float>(i);
        const float ratio = std::exp2(semitones / 12.0f);
        table[i] = 2.0f * std::atan(1.0f / ratio) / (2.0f * rings::kPi);
    }
    return table;
}

std::array<float, 257> make_ref_stiffness() {
    std::array<float, 257> table {};
    for (size_t i = 0; i < table.size(); ++i) {
        float g = static_cast<float>(i) / 256.0f;
        if (g < 0.25f) {
            g = 0.25f - g;
            table[i] = -g * 0.25f;
        } else if (g < 0.3f) {
            table[i] = 0.0f;
        } else if (g < 0.9f) {
            g -= 0.3f;
            g /= 0.6f;
            table[i] = 0.01f * std::pow(10.0f, g * 2.005f) - 0.01f;
        } else {
            g -= 0.9f;
            g /= 0.1f;
            g *= g;
            table[i] = 1.5f - std::cos(g * rings::kPi) / 2.0f;
        }
    }
    table[255] = 2.0f;
    table[256] = 2.0f;
    return table;
}

std::array<float, 5121> make_ref_sine_table() {
    std::array<float, 5121> table {};
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    for (size_t i = 0; i < table.size(); ++i) {
        table[i] = static_cast<float>(std::sin(
            kTwoPi * static_cast<double>(i) / 4096.0));
    }
    return table;
}

std::array<float, 129> make_ref_fm_quantizer() {
    const double kRatios[] = {
        0.5,
        0.5 * std::exp2(16.0 / 1200.0),
        std::sqrt(2.0) / 2.0,
        std::numbers::pi / 4.0,
        1.0,
        1.0 * std::exp2(16.0 / 1200.0),
        std::sqrt(2.0),
        std::numbers::pi / 2.0,
        7.0 / 4.0,
        2.0,
        2.0 * std::exp2(16.0 / 1200.0),
        9.0 / 4.0,
        11.0 / 4.0,
        2.0 * std::sqrt(2.0),
        3.0,
        std::numbers::pi,
        std::sqrt(3.0) * 2.0,
        4.0,
        std::sqrt(2.0) * 3.0,
        std::numbers::pi * 3.0 / 2.0,
        5.0,
        std::sqrt(2.0) * 4.0,
        8.0,
    };

    std::vector<double> scale;
    for (double ratio : kRatios) {
        const double semitones = 12.0 * std::log2(ratio);
        scale.push_back(semitones);
        scale.push_back(semitones);
        scale.push_back(semitones);
    }

    size_t target_size = 1;
    while (target_size < scale.size()) {
        target_size <<= 1;
    }
    while (scale.size() < target_size) {
        size_t gap = 0;
        double max_gap = -1.0;
        for (size_t i = 0; i + 1 < scale.size(); ++i) {
            const double diff = scale[i + 1] - scale[i];
            if (diff > max_gap) {
                max_gap = diff;
                gap = i;
            }
        }
        scale.insert(scale.begin() + static_cast<ptrdiff_t>(gap + 1),
                     (scale[gap] + scale[gap + 1]) * 0.5);
    }
    scale.push_back(scale.back());

    std::array<float, 129> table {};
    for (size_t i = 0; i < table.size(); ++i) {
        table[i] = static_cast<float>(scale[i]);
    }
    return table;
}

}  // namespace

TEST(RingsDspFunctions, WarmDspFunctionsPrewarmsTables) {
    rings::WarmDspFunctions();
    ASSERT_NE(rings::SineTable(), nullptr);
    ASSERT_NE(rings::FmFrequencyQuantizerTable(), nullptr);
}

TEST(RingsDspFunctions, FourDecadesMatchesOriginalLutTable) {
    const auto ref = make_ref_4_decades();
    for (size_t i = 0; i < ref.size(); ++i) {
        const float x = static_cast<float>(i) / 256.0f;
        EXPECT_NEAR(rings::FourDecades(x), ref[i], 1e-6f) << "index " << i;
    }
}

TEST(RingsDspFunctions, SvfShiftMatchesOriginalLutTable) {
    const auto ref = make_ref_svf_shift();
    for (size_t i = 0; i < ref.size(); ++i) {
        const float semitones = static_cast<float>(i);
        EXPECT_NEAR(rings::SvfShift(semitones), ref[i], 1e-6f) << "index " << i;
    }
}

TEST(RingsDspFunctions, StiffnessMatchesOriginalLutExceptClampedTail) {
    const auto ref = make_ref_stiffness();

    // The original LUT clamps the last two entries to exactly 2.0; the analytic
    // replacement preserves the underlying formula and differs slightly near 1.0.
    for (size_t i = 0; i <= 254; ++i) {
        const float x = static_cast<float>(i) / 256.0f;
        EXPECT_NEAR(rings::Stiffness(x), ref[i], 1e-6f) << "index " << i;
    }
    EXPECT_NEAR(rings::Stiffness(255.0f / 256.0f), ref[255], 2e-2f);
    EXPECT_NEAR(rings::Stiffness(1.0f), ref[256], 1e-6f);
}

TEST(RingsDspFunctions, AnalyticFunctionsTrackInterpolatedLuts) {
    const auto stiffness = make_ref_stiffness();
    const auto four_decades = make_ref_4_decades();
    const auto svf_shift = make_ref_svf_shift();

    float max_stiffness_diff = 0.0f;
    float max_four_decades_rel_err = 0.0f;
    for (int i = 0; i <= 4096; ++i) {
        const float x = static_cast<float>(i) / 4096.0f;
        const float four_ref = interpolate_ref(four_decades, x, 256.0f);
        const float four_exact = rings::FourDecades(x);
        max_stiffness_diff = std::max(
            max_stiffness_diff,
            std::abs(rings::Stiffness(x) - interpolate_ref(stiffness, x, 256.0f)));
        max_four_decades_rel_err = std::max(
            max_four_decades_rel_err,
            std::abs(four_exact - four_ref) / four_exact);
    }

    float max_svf_shift_diff = 0.0f;
    for (int i = 0; i <= 256; ++i) {
        const float semitones = static_cast<float>(i);
        max_svf_shift_diff = std::max(
            max_svf_shift_diff,
            std::abs(rings::SvfShift(semitones) - interpolate_ref(svf_shift, semitones, 1.0f)));
    }

    // The clamped tail creates the only notable mismatch; observed max is ~1.46e-2.
    EXPECT_LT(max_stiffness_diff, 2e-2f);
    EXPECT_LT(max_four_decades_rel_err, 4e-4f);
    EXPECT_LT(max_svf_shift_diff, 1e-6f);
}

TEST(RingsDspFunctions, SineTableMatchesReferenceDefinition) {
    rings::WarmDspFunctions();
    const float* actual = rings::SineTable();
    const auto ref = make_ref_sine_table();
    for (size_t i = 0; i < ref.size(); ++i) {
        EXPECT_NEAR(actual[i], ref[i], 1e-7f) << "index " << i;
    }
}

TEST(RingsDspFunctions, FmQuantizerTableMatchesReferenceDefinition) {
    rings::WarmDspFunctions();
    const float* actual = rings::FmFrequencyQuantizerTable();
    const auto ref = make_ref_fm_quantizer();
    for (size_t i = 0; i < ref.size(); ++i) {
        EXPECT_NEAR(actual[i], ref[i], 1e-6f) << "index " << i;
    }
}

TEST(RingsDspFunctions, FmQuantizerTableIsMonotonicAndHasSentinel) {
    rings::WarmDspFunctions();
    const float* t = rings::FmFrequencyQuantizerTable();
    for (int i = 0; i < 128; ++i) {
        EXPECT_LE(t[i], t[i + 1]) << "Non-monotonic at index " << i;
    }
    EXPECT_FLOAT_EQ(t[128], t[127]);
}
