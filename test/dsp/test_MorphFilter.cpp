#include <gtest/gtest.h>
#include <tanh/dsp/filter/MorphFilter.h>

#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

using thl::dsp::filter::MorphFilter;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr float k_pi = 3.14159265358979f;
constexpr float k_two_pi = 2.0f * k_pi;

float norm(float hz) {
    return hz / static_cast<float>(k_sample_rate);
}

// Bilinear-warped frequency ratio: the TPT prototype matches the analogue
// response at the prewarped frequencies, so expectations use tan ratios.
float warped_ratio(float hz, float cutoff_hz) {
    return std::tan(k_pi * norm(hz)) / std::tan(k_pi * norm(cutoff_hz));
}

// Analytic magnitude of `stages` cascaded Butterworth SVF low-pass stages.
float expected_lp_db(size_t stages, float ratio) {
    return static_cast<float>(stages) * -10.0f * std::log10(1.0f + std::pow(ratio, 4.0f));
}

float expected_one_pole_lp_db(float ratio) {
    return -10.0f * std::log10(1.0f + ratio * ratio);
}

// Double-precision phase accumulator: float sin(w * n) has enough phase
// jitter at large n to put a ~-77 dB noise floor on the input, masking the
// steep taps' stopband.
class SineGen {
public:
    explicit SineGen(float hz) : m_inc(2.0 * 3.14159265358979323846 * hz / k_sample_rate) {}

    float next() {
        const auto value = static_cast<float>(std::sin(m_phase));
        m_phase += m_inc;
        if (m_phase > 2.0 * 3.14159265358979323846) { m_phase -= 2.0 * 3.14159265358979323846; }
        return value;
    }

private:
    double m_phase = 0.0;
    double m_inc;
};

// Steady-state gain: drive with a unit sine, discard the warmup, compare RMS.
float measure_gain_db(MorphFilter& filter,
                      float hz,
                      size_t warmup = 24000,
                      size_t measure = 48000) {
    SineGen gen(hz);
    double sum_in = 0.0;
    double sum_out = 0.0;
    for (size_t n = 0; n < warmup + measure; ++n) {
        const float in = gen.next();
        const float out = filter.process(in);
        if (n >= warmup) {
            sum_in += static_cast<double>(in) * in;
            sum_out += static_cast<double>(out) * out;
        }
    }
    return 20.0f * std::log10(static_cast<float>(std::sqrt(sum_out / sum_in)));
}

MorphFilter make_filter(float cutoff_hz, float q, float mode, float slope) {
    MorphFilter filter;
    filter.set_frequency(norm(cutoff_hz));
    filter.set_q(q);
    filter.set_mode(mode);
    filter.set_slope(slope);
    filter.prepare(k_sample_rate);
    return filter;
}

float measure_tap_gain_db(size_t tap, float cutoff_hz, float mode, float hz) {
    auto filter =
        make_filter(cutoff_hz, MorphFilter::k_butterworth_q, mode, MorphFilter::slope_for_tap(tap));
    return measure_gain_db(filter, hz);
}

// Largest sample-to-sample step; a click shows up as a step far above the
// signal's own per-sample slew.
float max_first_difference(const std::vector<float>& samples, size_t begin, size_t end) {
    float max_diff = 0.0f;
    for (size_t n = begin + 1; n < end; ++n) {
        max_diff = std::max(max_diff, std::abs(samples[n] - samples[n - 1]));
    }
    return max_diff;
}

struct ClickProbe {
    float m_steady_diff;
    float m_transition_diff;
};

// Run a sine through the filter, apply `change` mid-stream, and report the
// worst per-sample step before vs after the change. The steady length is
// deliberately not a whole number of cycles for integer-Hz inputs, so the
// change never lands exactly on a zero crossing; the transition window starts
// one sample early to include the boundary step itself.
template <typename ChangeFn>
ClickProbe probe_click(MorphFilter& filter, SineGen& gen, ChangeFn change) {
    constexpr size_t k_warmup = 24000;
    constexpr size_t k_steady = 24611;
    constexpr size_t k_transition = 24000;

    std::vector<float> out(k_warmup + k_steady + k_transition);
    for (size_t n = 0; n < k_warmup + k_steady; ++n) { out[n] = filter.process(gen.next()); }
    change(filter);
    for (size_t n = k_warmup + k_steady; n < out.size(); ++n) {
        out[n] = filter.process(gen.next());
    }

    return {.m_steady_diff = max_first_difference(out, k_warmup, k_warmup + k_steady),
            .m_transition_diff = max_first_difference(out, k_warmup + k_steady - 1, out.size())};
}

}  // namespace

// --------------------------------------------------------------------------
// Frequency response — every tap against its analytic magnitude
// --------------------------------------------------------------------------

TEST(MorphFilter, LowpassTapResponses) {
    constexpr float k_cutoff = 1000.0f;

    // 6 dB/oct one-pole tap.
    EXPECT_NEAR(measure_tap_gain_db(0, k_cutoff, 0.0f, 4000.0f),
                expected_one_pole_lp_db(warped_ratio(4000.0f, k_cutoff)),
                1.0f);

    // SVF taps: 12, 24, 48, 96 dB/oct at 2x cutoff.
    for (const size_t tap : {size_t{1}, size_t{2}, size_t{4}, size_t{8}}) {
        EXPECT_NEAR(measure_tap_gain_db(tap, k_cutoff, 0.0f, 2000.0f),
                    expected_lp_db(tap, warped_ratio(2000.0f, k_cutoff)),
                    1.5f)
            << "tap " << tap;
    }

    // Steeper check for the shallow taps where the level is still measurable.
    EXPECT_NEAR(measure_tap_gain_db(1, k_cutoff, 0.0f, 4000.0f),
                expected_lp_db(1, warped_ratio(4000.0f, k_cutoff)),
                1.0f);
    EXPECT_NEAR(measure_tap_gain_db(2, k_cutoff, 0.0f, 4000.0f),
                expected_lp_db(2, warped_ratio(4000.0f, k_cutoff)),
                1.5f);

    // Passband stays flat even for the 96 dB cascade.
    EXPECT_NEAR(measure_tap_gain_db(8, k_cutoff, 0.0f, 100.0f), 0.0f, 0.5f);
}

TEST(MorphFilter, HighpassTapResponses) {
    constexpr float k_cutoff = 2000.0f;

    // High-pass magnitudes mirror the low-pass ones at the inverse ratio.
    EXPECT_NEAR(measure_tap_gain_db(0, k_cutoff, 1.0f, 500.0f),
                expected_one_pole_lp_db(1.0f / warped_ratio(500.0f, k_cutoff)),
                1.0f);
    for (const size_t tap : {size_t{1}, size_t{2}, size_t{4}}) {
        EXPECT_NEAR(measure_tap_gain_db(tap, k_cutoff, 1.0f, 1000.0f),
                    expected_lp_db(tap, 1.0f / warped_ratio(1000.0f, k_cutoff)),
                    1.5f)
            << "tap " << tap;
    }
    EXPECT_NEAR(measure_tap_gain_db(8, k_cutoff, 1.0f, 16000.0f), 0.0f, 0.5f);
}

// A 2nd-order SVF's gain at cutoff equals Q; every further Butterworth stage
// adds its -3 dB there.
TEST(MorphFilter, ResonancePeakEqualsQ) {
    auto filter = make_filter(1000.0f, 8.0f, 0.0f, MorphFilter::slope_for_tap(1));
    EXPECT_NEAR(measure_gain_db(filter, 1000.0f), 18.1f, 1.0f);

    filter = make_filter(1000.0f, 8.0f, 0.0f, MorphFilter::slope_for_tap(2));
    EXPECT_NEAR(measure_gain_db(filter, 1000.0f), 15.1f, 1.5f);

    filter = make_filter(1000.0f, 8.0f, 1.0f, MorphFilter::slope_for_tap(1));
    EXPECT_NEAR(measure_gain_db(filter, 1000.0f), 18.1f, 1.0f);
}

// --------------------------------------------------------------------------
// Click-freeness: after a parameter jump the output's per-sample step must
// stay comparable to the steady-state slew — a hard switch would step by the
// full signal amplitude in one sample.
// --------------------------------------------------------------------------

TEST(MorphFilter, ModeToggleIsClickFree) {
    auto filter =
        make_filter(1000.0f, MorphFilter::k_butterworth_q, 0.0f, MorphFilter::slope_for_tap(2));
    SineGen gen(200.0f);
    const auto probe = probe_click(filter, gen, [](MorphFilter& f) { f.set_mode(1.0f); });
    EXPECT_LT(probe.m_transition_diff, probe.m_steady_diff * 1.5f + 1.0e-3f);
}

// Full sweep 6 -> 96 dB/oct crosses every tap boundary (and activates every
// stage from its reset state) in one 5 ms ramp.
TEST(MorphFilter, FullSlopeSweepIsClickFree) {
    auto filter = make_filter(1000.0f, MorphFilter::k_butterworth_q, 0.0f, 0.0f);
    SineGen gen(250.0f);
    const auto probe = probe_click(filter, gen, [](MorphFilter& f) { f.set_slope(1.0f); });
    EXPECT_LT(probe.m_transition_diff, probe.m_steady_diff * 1.5f + 1.0e-3f);
}

TEST(MorphFilter, SlopeSweepDownIsClickFree) {
    auto filter = make_filter(1000.0f, MorphFilter::k_butterworth_q, 0.0f, 1.0f);
    SineGen gen(250.0f);
    const auto probe = probe_click(filter, gen, [](MorphFilter& f) { f.set_slope(0.0f); });
    EXPECT_LT(probe.m_transition_diff, probe.m_steady_diff * 1.5f + 1.0e-3f);
}

// A resonant slope sweep passes taps whose gain at the input frequency spans
// one-pole (-3 dB) to Q (=4): bound by that swell, not the 1.5x margin.
TEST(MorphFilter, ResonantSlopeSweepIsBounded) {
    auto filter = make_filter(1000.0f, 4.0f, 0.0f, 0.0f);
    SineGen gen(1000.0f);
    const auto probe = probe_click(filter, gen, [](MorphFilter& f) { f.set_slope(1.0f); });
    EXPECT_LT(probe.m_transition_diff, probe.m_steady_diff * 10.0f);
}

// A stage that dropped out of the mix re-enters state-reset: park on the 96 dB
// tap, drop to 12 dB, then step back up to 24 dB.
TEST(MorphFilter, StageReactivationIsClickFree) {
    auto filter =
        make_filter(1000.0f, MorphFilter::k_butterworth_q, 0.0f, MorphFilter::slope_for_tap(8));
    SineGen gen(250.0f);
    for (size_t n = 0; n < 24000; ++n) { filter.process(gen.next()); }
    filter.set_slope(MorphFilter::slope_for_tap(1));
    for (size_t n = 0; n < 24000; ++n) { filter.process(gen.next()); }

    const auto probe = probe_click(filter, gen, [](MorphFilter& f) {
        f.set_slope(MorphFilter::slope_for_tap(2));
    });
    EXPECT_LT(probe.m_transition_diff, probe.m_steady_diff * 1.5f + 1.0e-3f);
}

// Retuning re-rings the stored state at the new cutoff for a few ms (as any
// filter does, analogue included) at ~2.6x the steady slew here; the 4x bound
// admits that while still catching a hard coefficient step (~38x).
TEST(MorphFilter, FrequencyJumpHasNoHardStep) {
    auto filter =
        make_filter(500.0f, MorphFilter::k_butterworth_q, 0.0f, MorphFilter::slope_for_tap(3));
    SineGen gen(200.0f);
    const auto probe =
        probe_click(filter, gen, [](MorphFilter& f) { f.set_frequency(norm(8000.0f)); });
    EXPECT_LT(probe.m_transition_diff, probe.m_steady_diff * 4.0f);
}

// The resonance swell itself raises the slew (gain 0.5 -> 10): bound by the
// amplitude ratio, not the 1.5x margin.
TEST(MorphFilter, QJumpIsBounded) {
    auto filter = make_filter(1000.0f, 0.5f, 0.0f, MorphFilter::slope_for_tap(1));
    SineGen gen(1000.0f);
    const auto probe = probe_click(filter, gen, [](MorphFilter& f) { f.set_q(10.0f); });
    EXPECT_LT(probe.m_transition_diff, probe.m_steady_diff * 25.0f);
}

// A hard switch with (near) no smoothing must trip the same probe, proving it
// detects clicks at all.
TEST(MorphFilter, ProbeDetectsUnsmoothedSwitch) {
    MorphFilter filter;
    filter.set_frequency(norm(1000.0f));
    filter.set_q(MorphFilter::k_butterworth_q);
    filter.set_mode(0.0f);
    filter.set_slope(MorphFilter::slope_for_tap(1));
    filter.prepare(k_sample_rate, 1.0 / k_sample_rate);  // 1-sample "smoothing"
    // 170 Hz puts the low-pass output mid-swing at the probe's switch sample
    // (at 200 Hz its phase lands near a zero crossing, hiding the step).
    SineGen gen(170.0f);
    const auto probe = probe_click(filter, gen, [](MorphFilter& f) { f.set_mode(1.0f); });
    EXPECT_GT(probe.m_transition_diff, probe.m_steady_diff * 1.5f + 1.0e-3f);
}

// --------------------------------------------------------------------------
// Stability under abuse
// --------------------------------------------------------------------------

TEST(MorphFilter, StaysBoundedUnderRandomModulation) {
    auto filter = make_filter(1000.0f, 1.0f, 0.0f, 0.0f);

    std::minstd_rand rng(0x5eed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    constexpr size_t k_total = 3 * 48000;
    constexpr size_t k_hold = 64;
    for (size_t n = 0; n < k_total; ++n) {
        if (n % k_hold == 0) {
            filter.set_frequency(1.0e-4f + unit(rng) * 0.45f);
            filter.set_q(0.1f + unit(rng) * 9.9f);
            filter.set_mode(unit(rng));
            filter.set_slope(unit(rng));
        }
        const float out = filter.process(noise(rng));
        ASSERT_TRUE(std::isfinite(out));
        ASSERT_LT(std::abs(out), 1.0e3f);
    }
}

TEST(MorphFilter, SilenceInSilenceOut) {
    auto filter = make_filter(1000.0f, 8.0f, 0.5f, 0.5f);
    for (size_t n = 0; n < 4800; ++n) { EXPECT_EQ(filter.process(0.0f), 0.0f); }
}
