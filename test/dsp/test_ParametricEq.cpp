#include <gtest/gtest.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/filter/ParametricEq.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

using thl::dsp::filter::EqBand;
using thl::dsp::filter::EqBandType;
using thl::dsp::filter::EqDesign;
using thl::dsp::filter::ParametricEq;

namespace {

constexpr double k_sample_rate = 48000.0;

// Double-precision phase: float sin(w * n) jitters enough at large n to blur deep cuts.
class SineGen {
public:
    SineGen(double hz, double sample_rate) : m_inc(2.0 * std::numbers::pi * hz / sample_rate) {}

    float next() {
        const auto value = static_cast<float>(std::sin(m_phase));
        m_phase = std::fmod(m_phase + m_inc, 2.0 * std::numbers::pi);
        return value;
    }

private:
    double m_phase = 0.0;
    double m_inc;
};

// Steady-state gain of one channel: a unit sine, the warmup discarded, RMS out over in.
float measure_gain_db(ParametricEq& eq, double hz, double sample_rate = k_sample_rate) {
    constexpr size_t k_block = 256;
    const auto warmup = static_cast<size_t>(sample_rate / 2);
    const auto measure = static_cast<size_t>(sample_rate);
    SineGen gen(hz, sample_rate);
    std::array<float, k_block> in{};
    std::array<float, k_block> out{};
    double sum_in = 0.0;
    double sum_out = 0.0;
    for (size_t done = 0; done < warmup + measure; done += k_block) {
        for (size_t i = 0; i < k_block; ++i) { in[i] = out[i] = gen.next(); }
        float* channels[] = {out.data()};
        eq.process(channels, 1, k_block);
        if (done >= warmup) {
            for (size_t i = 0; i < k_block; ++i) {
                sum_in += static_cast<double>(in[i]) * in[i];
                sum_out += static_cast<double>(out[i]) * out[i];
            }
        }
    }
    return static_cast<float>(10.0 * std::log10(sum_out / sum_in));
}

ParametricEq make_eq(const EqBand& band,
                     EqDesign design = EqDesign::Matched,
                     double sample_rate = k_sample_rate) {
    ParametricEq eq;
    eq.prepare(sample_rate, 2);
    eq.set_design(design);
    eq.set_band(0, band);
    eq.reset();  // settled at once
    return eq;
}

// Largest sample-to-sample step of a block run.
float max_step(const std::vector<float>& x) {
    float m = 0.0f;
    for (size_t n = 1; n < x.size(); ++n) { m = std::max(m, std::abs(x[n] - x[n - 1])); }
    return m;
}

// A 220 Hz sine through @p eq in blocks of 64, calling @p change(block) before each block.
template <typename Change>
std::vector<float> run_sine(ParametricEq& eq, size_t num_blocks, Change change) {
    constexpr size_t k_block = 64;
    SineGen gen(220.0, k_sample_rate);
    std::vector<float> out(num_blocks * k_block);
    for (size_t b = 0; b < num_blocks; ++b) {
        change(b);
        float* block = out.data() + (b * k_block);
        for (size_t i = 0; i < k_block; ++i) { block[i] = 0.5f * gen.next(); }
        float* channels[] = {block};
        eq.process(channels, 1, k_block);
    }
    return out;
}

}  // namespace

// --- the response ---------------------------------------------------------------------------

TEST(ParametricEq, MeasuredResponseMatchesMagnitudeDb) {
    const std::array<EqBand, 7> bands{{
        {.m_type = EqBandType::Peak, .m_frequency = 1000.0f, .m_gain_db = 9.0f, .m_q = 1.4f},
        {.m_type = EqBandType::Peak, .m_frequency = 12000.0f, .m_gain_db = -12.0f, .m_q = 4.0f},
        {.m_type = EqBandType::Notch, .m_frequency = 2000.0f, .m_q = 2.0f},
        {.m_type = EqBandType::LowShelf, .m_frequency = 200.0f, .m_gain_db = 6.0f},
        {.m_type = EqBandType::HighShelf, .m_frequency = 9000.0f, .m_gain_db = -8.0f},
        {.m_type = EqBandType::LowCut, .m_frequency = 300.0f, .m_q = 2.0f},
        {.m_type = EqBandType::HighCut, .m_frequency = 15000.0f},
    }};
    const std::array<double, 9>
        probes{40.0, 110.0, 290.0, 640.0, 1210.0, 2480.0, 4950.0, 9100.0, 17300.0};
    for (const EqDesign design : {EqDesign::Bilinear, EqDesign::Matched}) {
        for (const auto& band : bands) {
            auto eq = make_eq(band, design);
            for (const double hz : probes) {
                eq.reset();
                EXPECT_NEAR(measure_gain_db(eq, hz),
                            ParametricEq::magnitude_db(band, design, hz, k_sample_rate),
                            0.05f)
                    << static_cast<int>(design) << " " << static_cast<int>(band.m_type) << " at "
                    << hz << " Hz";
            }
        }
    }
}

TEST(ParametricEq, BilinearMagnitudeHasTheShapeOfEachType) {
    const auto db = [](EqBand band, double hz) {
        return ParametricEq::magnitude_db(band, EqDesign::Bilinear, hz, k_sample_rate);
    };
    const EqBand peak{.m_type = EqBandType::Peak, .m_frequency = 1000.0f, .m_gain_db = 6.0f};
    EXPECT_NEAR(db(peak, 1000.0), 6.0f, 1e-3f);
    EXPECT_NEAR(db(peak, 20.0), 0.0f, 0.01f);
    const EqBand low_shelf{.m_type = EqBandType::LowShelf,
                           .m_frequency = 300.0f,
                           .m_gain_db = -9.0f};
    EXPECT_NEAR(db(low_shelf, 1.0), -9.0f, 0.01f);
    EXPECT_NEAR(db(low_shelf, 300.0), -4.5f, 1e-3f);  // half the gain at the frequency
    EXPECT_NEAR(db(low_shelf, 20000.0), 0.0f, 0.01f);
    const EqBand high_shelf{.m_type = EqBandType::HighShelf,
                            .m_frequency = 3000.0f,
                            .m_gain_db = 12.0f};
    EXPECT_NEAR(db(high_shelf, 20.0), 0.0f, 0.01f);
    EXPECT_NEAR(db(high_shelf, 3000.0), 6.0f, 1e-3f);
    EXPECT_NEAR(db(high_shelf, 24000.0), 12.0f, 0.01f);
    const EqBand low_cut{.m_type = EqBandType::LowCut, .m_frequency = 100.0f};
    EXPECT_NEAR(db(low_cut, 100.0), -3.01f, 0.01f);  // Butterworth at Q 0.707
    EXPECT_NEAR(db(low_cut, 25.0), -24.1f, 0.2f);    // 12 dB per octave below
    const EqBand high_cut{.m_type = EqBandType::HighCut, .m_frequency = 1000.0f};
    EXPECT_NEAR(db(high_cut, 1000.0), -3.01f, 0.01f);
    EXPECT_LT(db(high_cut, 24000.0), -100.0f);  // the bilinear zero at Nyquist
    const EqBand notch{.m_type = EqBandType::Notch, .m_frequency = 1000.0f, .m_q = 1.0f};
    EXPECT_LT(db(notch, 1000.0), -100.0f);
    EXPECT_NEAR(db(notch, 20.0), 0.0f, 0.01f);
}

TEST(ParametricEq, ResponseIsTheSumOfTheBands) {
    const std::array<EqBand, 2> bands{{
        {.m_type = EqBandType::Peak, .m_frequency = 500.0f, .m_gain_db = 4.0f},
        {.m_type = EqBandType::HighShelf, .m_frequency = 2000.0f, .m_gain_db = -3.0f},
    }};
    for (const double hz : {50.0, 500.0, 5000.0}) {
        EXPECT_FLOAT_EQ(
            ParametricEq::response_db(bands, EqDesign::Matched, hz, k_sample_rate),
            ParametricEq::magnitude_db(bands[0], EqDesign::Matched, hz, k_sample_rate) +
                ParametricEq::magnitude_db(bands[1], EqDesign::Matched, hz, k_sample_rate));
    }
}

TEST(ParametricEq, BandsRunInSeries) {
    ParametricEq eq;
    eq.prepare(k_sample_rate, 1);
    const EqBand a{.m_type = EqBandType::Peak, .m_frequency = 800.0f, .m_gain_db = 5.0f};
    const EqBand b{.m_type = EqBandType::LowShelf, .m_frequency = 400.0f, .m_gain_db = 3.0f};
    eq.set_band(2, a);
    eq.set_band(7, b);
    eq.reset();
    const std::array<EqBand, 2> both{a, b};
    EXPECT_NEAR(measure_gain_db(eq, 600.0),
                ParametricEq::response_db(both, EqDesign::Matched, 600.0, k_sample_rate),
                0.05f);
}

// --- flat and off ---------------------------------------------------------------------------

TEST(ParametricEq, FlatAndDisabledBandsLeaveTheSignalUntouched) {
    ParametricEq eq;
    eq.prepare(k_sample_rate, 2);
    eq.set_band(0, {.m_type = EqBandType::Peak, .m_gain_db = 0.0f});
    eq.set_band(1, {.m_type = EqBandType::LowCut, .m_frequency = 500.0f, .m_enabled = false});
    eq.reset();
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> left(512);
    std::vector<float> right(512);
    for (auto& x : left) { x = dist(rng); }
    for (auto& x : right) { x = dist(rng); }
    const auto left_in = left;
    const auto right_in = right;
    float* channels[] = {left.data(), right.data()};
    eq.process(channels, 2, 512);
    EXPECT_EQ(left, left_in);
    EXPECT_EQ(right, right_in);
}

TEST(ParametricEq, ADisabledBandFadesOutAndThenCostsNothing) {
    ParametricEq eq;
    eq.prepare(k_sample_rate, 1);
    EqBand band{.m_type = EqBandType::Peak, .m_frequency = 220.0f, .m_gain_db = 12.0f};
    eq.set_band(0, band);
    eq.reset();
    band.m_enabled = false;
    eq.set_band(0, band);
    // After the ramp (10 ms) the band is gone: the output is the input.
    std::vector<float> x(4096, 0.25f);
    float* channels[] = {x.data()};
    eq.process(channels, 1, x.size());
    for (size_t n = 1024; n < x.size(); ++n) { ASSERT_EQ(x[n], 0.25f) << n; }
}

// --- changes do not click -------------------------------------------------------------------

TEST(ParametricEq, ChangesRampWithoutClicks) {
    // A sine's own largest step at +12 dB: a click is a step far above it.
    ParametricEq reference = make_eq(
        {.m_type = EqBandType::Peak, .m_frequency = 220.0f, .m_gain_db = 12.0f, .m_q = 0.7f});
    const float slew = max_step(run_sine(reference, 200, [](size_t) {}));

    ParametricEq eq;
    eq.prepare(k_sample_rate, 1);
    const std::array<EqBand, 6> changes{{
        {.m_type = EqBandType::Peak, .m_frequency = 220.0f, .m_gain_db = 12.0f, .m_q = 0.7f},
        {.m_type = EqBandType::Peak, .m_frequency = 220.0f, .m_gain_db = -12.0f, .m_q = 0.7f},
        {.m_type = EqBandType::LowCut, .m_frequency = 2000.0f},
        {.m_type = EqBandType::HighShelf, .m_frequency = 100.0f, .m_gain_db = 12.0f},
        {.m_type = EqBandType::Notch, .m_frequency = 220.0f, .m_enabled = false},
        {.m_type = EqBandType::Peak, .m_frequency = 220.0f, .m_gain_db = 12.0f, .m_q = 0.7f},
    }};
    const auto out = run_sine(eq, 600, [&](size_t b) {
        if (b % 100 == 0) { eq.set_band(0, changes[b / 100]); }
    });
    EXPECT_LT(max_step(out), 1.5f * slew);
}

TEST(ParametricEq, SweepsAreSmooth) {
    ParametricEq reference = make_eq(
        {.m_type = EqBandType::Peak, .m_frequency = 220.0f, .m_gain_db = 12.0f, .m_q = 2.0f});
    const float slew = max_step(run_sine(reference, 200, [](size_t) {}));

    ParametricEq eq;
    eq.prepare(k_sample_rate, 1);
    const auto out = run_sine(eq, 750, [&](size_t b) {
        // Frequency across the sine every block, gain up and down.
        const double t = static_cast<double>(b) / 750.0;
        eq.set_band(0,
                    {.m_type = EqBandType::Peak,
                     .m_frequency = static_cast<float>(50.0 * std::pow(200.0, t)),
                     .m_gain_db = static_cast<float>(12.0 * std::sin(6.0 * t)),
                     .m_q = 2.0f});
    });
    EXPECT_LT(max_step(out), 1.5f * slew);
}

// --- robustness -----------------------------------------------------------------------------

TEST(ParametricEq, RingsOutAfterExtremesAtAnySampleRate) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::uniform_int_distribution<int> type(0, 5);
    for (const double sample_rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        ParametricEq eq;
        eq.prepare(sample_rate, 2);
        std::vector<float> left(128);
        std::vector<float> right(128);
        for (size_t block = 0; block < 4000; ++block) {
            if (block % 3 == 0) {
                for (size_t i = 0; i < ParametricEq::k_max_bands; ++i) {
                    eq.set_band(i,
                                {.m_type = static_cast<EqBandType>(type(rng)),
                                 .m_frequency = 5.0f * std::pow(5000.0f, unit(rng)),  // past both
                                                                                      // ends
                                 .m_gain_db = -60.0f + (120.0f * unit(rng)),
                                 .m_q = 0.01f * std::pow(5000.0f, unit(rng)),
                                 .m_enabled = unit(rng) > 0.2f});
                }
            }
            for (size_t n = 0; n < left.size(); ++n) {
                left[n] = (unit(rng) * 2.0f) - 1.0f;
                right[n] = (unit(rng) * 2.0f) - 1.0f;
            }
            float* channels[] = {left.data(), right.data()};
            eq.process(channels, 2, left.size());
            for (size_t n = 0; n < left.size(); ++n) {
                ASSERT_TRUE(std::isfinite(left[n]) && std::isfinite(right[n])) << sample_rate;
            }
        }
        // Stable filters ring out: every band still running, silence in, silence out.
        for (size_t i = 0; i < ParametricEq::k_max_bands; ++i) {
            eq.set_band(i,
                        {.m_type = EqBandType::Peak,
                         .m_frequency = 100.0f * static_cast<float>(i + 1),
                         .m_gain_db = 6.0f});
        }
        float tail = 0.0f;
        for (size_t block = 0; block < static_cast<size_t>(sample_rate) / 128; ++block) {
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            float* channels[] = {left.data(), right.data()};
            eq.process(channels, 2, left.size());
            tail = std::max(std::abs(left.back()), std::abs(right.back()));
        }
        EXPECT_LT(tail, 1.0e-6f) << sample_rate;
    }
}

TEST(ParametricEq, ChannelsAreIndependent) {
    ParametricEq eq;
    eq.prepare(k_sample_rate, 2);
    eq.set_band(0, {.m_type = EqBandType::Peak, .m_frequency = 300.0f, .m_gain_db = 10.0f});
    std::vector<float> left(2048);
    std::vector<float> right(2048, 0.0f);
    SineGen gen(300.0, k_sample_rate);
    for (auto& x : left) { x = gen.next(); }
    float* channels[] = {left.data(), right.data()};
    eq.process(channels, 2, left.size());
    for (const float x : right) { ASSERT_EQ(x, 0.0f); }
}

TEST(ParametricEq, BandReadsBackWhatWasSet) {
    ParametricEq eq;
    eq.prepare(k_sample_rate, 1);
    const EqBand band{.m_type = EqBandType::HighShelf, .m_frequency = 7000.0f, .m_gain_db = 2.5f};
    eq.set_band(3, band);
    EXPECT_EQ(eq.band(3), band);
    EXPECT_FALSE(eq.band(0).m_enabled);            // prepare() leaves every band off
    eq.set_band(ParametricEq::k_max_bands, band);  // out of range: ignored
}

TEST(ParametricEq, ResponseCurveIsTheResponseAtEachFrequency) {
    const std::array<EqBand, 3> bands{{
        {.m_type = EqBandType::LowCut, .m_frequency = 40.0f},
        {.m_type = EqBandType::Peak, .m_frequency = 2500.0f, .m_gain_db = -4.0f, .m_q = 3.0f},
        {.m_type = EqBandType::HighShelf, .m_frequency = 10000.0f, .m_gain_db = 3.0f},
    }};
    std::array<float, 200> frequencies{};
    ParametricEq::log_frequencies(frequencies, 20.0f, 20000.0f);
    EXPECT_FLOAT_EQ(frequencies.front(), 20.0f);
    EXPECT_FLOAT_EQ(frequencies.back(), 20000.0f);
    EXPECT_NEAR(frequencies[100] / frequencies[99], frequencies[1] / frequencies[0], 1e-4f);
    std::array<float, 200> curve{};
    for (const EqDesign design : {EqDesign::Bilinear, EqDesign::Matched}) {
        ParametricEq::response_curve(bands, design, k_sample_rate, frequencies, curve);
        for (size_t i = 0; i < curve.size(); ++i) {
            EXPECT_NEAR(curve[i],
                        ParametricEq::response_db(bands, design, frequencies[i], k_sample_rate),
                        1e-4f)
                << frequencies[i];
        }
    }
}

TEST(ParametricEq, ChangingTheDesignRampsWithoutClicks) {
    const EqBand band{.m_type = EqBandType::Peak,
                      .m_frequency = 220.0f,
                      .m_gain_db = 12.0f,
                      .m_q = 0.7f};
    ParametricEq reference = make_eq(band);
    const float slew = max_step(run_sine(reference, 200, [](size_t) {}));
    ParametricEq eq = make_eq(band);
    const auto out = run_sine(eq, 400, [&](size_t b) {
        if (b % 100 == 50) {
            eq.set_design(eq.design() == EqDesign::Matched ? EqDesign::Bilinear
                                                           : EqDesign::Matched);
        }
    });
    EXPECT_LT(max_step(out), 1.5f * slew);
}

TEST(ParametricEq, PrepareKeepsTheBands) {
    ParametricEq eq;
    eq.prepare(44100.0, 1);
    const EqBand band{.m_type = EqBandType::Peak, .m_frequency = 1000.0f, .m_gain_db = 6.0f};
    eq.set_band(0, band);
    eq.prepare(96000.0, 2);  // a host re-preparing
    EXPECT_EQ(eq.band(0), band);
    EXPECT_NEAR(measure_gain_db(eq, 1000.0, 96000.0), 6.0f, 0.05f);  // redesigned, no ramp
}

TEST(ParametricEq, NonFiniteSettingsDoNotBreakIt) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    ParametricEq eq;
    eq.prepare(k_sample_rate, 1);
    eq.set_band(0, {.m_type = EqBandType::Peak, .m_frequency = nan, .m_gain_db = 6.0f});
    eq.set_band(1, {.m_type = EqBandType::LowShelf, .m_gain_db = inf, .m_q = nan});
    eq.set_band(2, {.m_type = static_cast<EqBandType>(200), .m_gain_db = 3.0f});
    std::vector<float> x(4096, 0.1f);
    for (int block = 0; block < 4; ++block) {
        eq.set_band(0, {.m_type = EqBandType::Peak, .m_frequency = nan, .m_gain_db = 6.0f});
        float* channels[] = {x.data()};
        eq.process(channels, 1, x.size());
        for (const float v : x) { ASSERT_TRUE(std::isfinite(v)); }
        std::fill(x.begin(), x.end(), 0.1f);
    }
}

// --- ParametricEqImpl -----------------------------------------------------------------------

namespace {

class TestEqProcessor : public thl::dsp::filter::ParametricEqImpl {
public:
    explicit TestEqProcessor(size_t num_bands) : ParametricEqImpl(num_bands) {}

    float m_design = 1.0f;
    std::array<std::array<float, NumBandParameters>, ParametricEq::k_max_bands> m_bands{};

    void set(size_t band, EqBandType type, float f, float gain, float q, bool on = true) {
        m_bands[band] = {static_cast<float>(type), f, gain, q, on ? 1.0f : 0.0f};
    }

private:
    float get_parameter_float(Parameter, uint32_t) override { return m_design; }
    float get_band_parameter_float(BandParameter p, size_t band, uint32_t) override {
        return m_bands[band][static_cast<size_t>(p)];
    }
};

float processor_gain_db(TestEqProcessor& eq, double hz) {
    SineGen gen(hz, k_sample_rate);
    std::vector<float> left(48000);
    std::vector<float> right(48000);
    for (size_t n = 0; n < left.size(); ++n) { left[n] = right[n] = gen.next(); }
    const auto in = left;
    float* channels[] = {left.data(), right.data()};
    eq.process(thl::core::BufferView(channels, 2, left.size()));
    double sum_in = 0.0;
    double sum_out = 0.0;
    for (size_t n = 24000; n < left.size(); ++n) {
        sum_in += static_cast<double>(in[n]) * in[n];
        sum_out += static_cast<double>(left[n]) * left[n];
        EXPECT_EQ(left[n], right[n]);
    }
    return static_cast<float>(10.0 * std::log10(sum_out / sum_in));
}

}  // namespace

TEST(ParametricEqImpl, ReadsItsParametersAndFilters) {
    TestEqProcessor eq(2);
    eq.set(0, EqBandType::Peak, 1000.0f, 6.0f, 1.0f);
    eq.set(1, EqBandType::HighShelf, 8000.0f, -4.0f, 0.7f);
    eq.prepare(k_sample_rate, 512, 2);
    EXPECT_EQ(eq.equaliser().design(), EqDesign::Matched);
    const std::array<EqBand, 2> bands{eq.equaliser().band(0), eq.equaliser().band(1)};
    EXPECT_NEAR(processor_gain_db(eq, 1000.0),
                ParametricEq::response_db(bands, EqDesign::Matched, 1000.0, k_sample_rate),
                0.05f);

    eq.m_design = 0.0f;
    eq.set(0, EqBandType::LowCut, 2000.0f, 0.0f, 0.7f);
    (void)processor_gain_db(eq, 300.0);  // ramps to the new settings
    const std::array<EqBand, 2> now{eq.equaliser().band(0), eq.equaliser().band(1)};
    EXPECT_EQ(eq.equaliser().design(), EqDesign::Bilinear);
    EXPECT_NEAR(processor_gain_db(eq, 300.0),
                ParametricEq::response_db(now, EqDesign::Bilinear, 300.0, k_sample_rate),
                0.05f);
}

TEST(ParametricEqImpl, RoundsAndClampsItsChoices) {
    TestEqProcessor eq(ParametricEq::k_max_bands + 3);  // clamped to the maximum
    EXPECT_EQ(eq.num_bands(), ParametricEq::k_max_bands);
    eq.m_design = 7.0f;
    eq.m_bands[0] = {4.4f, 500.0f, 0.0f, 0.7f, 0.9f};  // LowCut, on
    eq.prepare(k_sample_rate, 256, 1);
    EXPECT_EQ(eq.equaliser().design(), EqDesign::Matched);
    EXPECT_EQ(eq.equaliser().band(0).m_type, EqBandType::LowCut);
    EXPECT_TRUE(eq.equaliser().band(0).m_enabled);
    EXPECT_FALSE(eq.equaliser().band(1).m_enabled);
}
