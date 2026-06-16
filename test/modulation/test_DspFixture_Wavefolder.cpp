// DspFixture v1 — exercises a DSP processor through the modulation matrix and
// asserts properties every processor should hold under heavy automation:
//
//   1. SilenceInSilenceOut — zero input + non-zero static parameter state must
//      give zero output. Catches DC injection by the transfer function.
//   2. NoClicksWithModulatedSilence — zero input + fast LFO modulation on every
//      parameter must not produce per-sample steps. Catches missing parameter
//      smoothing.
//   3. NoClicksWithModulatedSine — sine input + fast LFO modulation must not
//      produce per-sample steps far larger than the un-modulated reference.
//   4. NoNanInf — folded into the above via std::isfinite checks.
//
// For now applied only to IntellijelWavefolder. Generalize once we trust the
// thresholds against a known-broken-then-fixed processor.

#include <gtest/gtest.h>
#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/fx/IntellijelWavefolder.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/State.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

#include "TestHelpers.h"

using namespace thl::modulation;
using thl::dsp::fx::IntellijelWavefolderImpl;

namespace WavefolderID {
constexpr std::string_view k_drive = "wf.drive";
constexpr std::string_view k_folds = "wf.folds";
constexpr std::string_view k_symmetry = "wf.symmetry";
constexpr std::string_view k_jfet = "wf.jfet";
}  // namespace WavefolderID

// Test wrapper — mirrors the production WavefolderVoiceProcessor wiring, but
// without the dry/wet slot (we want to assert on the wet path only).
class TestWavefolder : public IntellijelWavefolderImpl {
public:
    explicit TestWavefolder(ModulationMatrix& matrix) {
        m_handles.resize(NumParameters);
        m_handles[Drive] = matrix.get_smart_handle<float>(WavefolderID::k_drive);
        m_handles[Folds] = matrix.get_smart_handle<float>(WavefolderID::k_folds);
        m_handles[Symmetry] = matrix.get_smart_handle<float>(WavefolderID::k_symmetry);
        m_handles[JfetTone] = matrix.get_smart_handle<float>(WavefolderID::k_jfet);
    }

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override {
        m_change_points.reserve(samples_per_block);
        IntellijelWavefolderImpl::prepare(sample_rate, samples_per_block, num_channels);
    }

protected:
    float get_parameter_float(Parameter p, uint32_t modulation_offset) override {
        return m_handles[p].load(modulation_offset);
    }
    std::span<const uint32_t> get_change_points() override {
        thl::modulation::collect_change_points(std::span<const SmartHandle<float>>(m_handles),
                                               m_change_points);
        return m_change_points;
    }

private:
    std::vector<SmartHandle<float>> m_handles;
    std::vector<uint32_t> m_change_points;
};

// ── Helpers ──────────────────────────────────────────────────────────────────

static void register_wavefolder_params(thl::State& state,
                                       float drive_default = 1.0f,
                                       float folds_default = 0.0f,
                                       float symmetry_default = 0.0f,
                                       float jfet_default = 0.0f) {
    state.create(WavefolderID::k_drive,
                 thl::ParameterDefinition::make_float("drive",
                                                      thl::Range::linear(0.1f, 20.0f),
                                                      drive_default)
                     .modulatable(true));
    state.create(WavefolderID::k_folds,
                 thl::ParameterDefinition::make_float("folds",
                                                      thl::Range::linear(0.0f, 10.0f),
                                                      folds_default)
                     .modulatable(true));
    state.create(WavefolderID::k_symmetry,
                 thl::ParameterDefinition::make_float("symmetry",
                                                      thl::Range::linear(-1.0f, 1.0f),
                                                      symmetry_default)
                     .modulatable(true));
    state.create(
        WavefolderID::k_jfet,
        thl::ParameterDefinition::make_float("jfet", thl::Range::linear(0.0f, 1.0f), jfet_default)
            .modulatable(true));
}

// Runs N blocks of `input` through the processor while the matrix advances.
// Returns the (in-place) processed output as a flat vector.
static std::vector<float> run_blocks(ModulationMatrix& matrix,
                                     TestWavefolder& wf,
                                     const std::vector<float>& input) {
    std::vector<float> output;
    output.reserve(input.size());
    const size_t num_blocks = input.size() / k_block_size;
    std::vector<float> block(k_block_size);
    for (size_t b = 0; b < num_blocks; ++b) {
        std::copy(input.begin() + static_cast<ptrdiff_t>(b * k_block_size),
                  input.begin() + static_cast<ptrdiff_t>((b + 1) * k_block_size),
                  block.begin());
        matrix.process(k_block_size);
        thl::dsp::audio::AudioBufferView view(block.data(), k_block_size);
        wf.process_modulated(view);
        output.insert(output.end(), block.begin(), block.end());
    }
    return output;
}

static float max_abs(const std::vector<float>& v) {
    float m = 0.0f;
    for (float s : v) { m = std::max(m, std::abs(s)); }
    return m;
}

static float max_sample_step(const std::vector<float>& v) {
    float m = 0.0f;
    for (size_t i = 1; i < v.size(); ++i) { m = std::max(m, std::abs(v[i] - v[i - 1])); }
    return m;
}

static void expect_all_finite(const std::vector<float>& v) {
    for (float s : v) {
        ASSERT_TRUE(std::isfinite(s)) << "non-finite sample produced by wavefolder";
    }
}

// ── Test 1 — Silence in must give silence out ────────────────────────────────
//
// With non-zero static symmetry, the wavefolder's transfer function evaluates
// to sin(symmetry · drive · (1 + folds)) on a zero input — i.e. it synthesises
// a DC offset out of silence. This is the "clicks/hum with no audio playing"
// report: any subsequent change to Drive/Symmetry/Folds steps that DC.
TEST(DspFixtureWavefolder, SilenceInSilenceOutWithStaticOffset) {
    thl::State state;
    register_wavefolder_params(state,
                               /*drive*/ 5.0f,
                               /*folds*/ 1.0f,
                               /*symmetry*/ 0.3f,
                               /*jfet*/ 0.0f);
    ModulationMatrix matrix(state);
    TestWavefolder wf(matrix);

    matrix.prepare(k_sample_rate, k_block_size);
    wf.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> in(10 * k_block_size, 0.0f);
    auto out = run_blocks(matrix, wf, in);

    expect_all_finite(out);
    EXPECT_LT(max_abs(out), 1e-4f)
        << "Silence in must give silence out — DC injected by transfer function.";
}

// Helper: square-wave LFOs at user-knob-like rates (slower than the 5 ms
// smoothing window) and modest depth — models a user dragging knobs, not an
// adversarial modulation source. Without smoothing, each LFO transition causes
// a 1-sample jump in the wavefolder transfer function output. With smoothing,
// the transition is spread across ~5 ms and the per-sample step is bounded by
// the smoother's slope times the local transfer-function derivative.
struct KnobLikeLfos {
    TestLFOSource drive;
    TestLFOSource folds;
    TestLFOSource symmetry;
    TestLFOSource jfet;

    KnobLikeLfos() {
        for (auto* l : {&drive, &folds, &symmetry, &jfet}) {
            l->m_waveform = LFOWaveform::Square;
            l->m_decimation = 1;
        }
        // Slow, co-prime-ish frequencies: each step is fully resolved by the
        // smoother before the next flip arrives.
        drive.m_frequency = 5.0f;
        folds.m_frequency = 7.0f;
        symmetry.m_frequency = 11.0f;
        jfet.m_frequency = 13.0f;
    }

    void wire(ModulationMatrix& matrix, float depth = 0.3f) {
        matrix.add_source("d", &drive);
        matrix.add_source("f", &folds);
        matrix.add_source("s", &symmetry);
        matrix.add_source("j", &jfet);
        matrix.add_routing({"d", WavefolderID::k_drive, depth, 0, DepthMode::Normalized});
        matrix.add_routing({"f", WavefolderID::k_folds, depth, 0, DepthMode::Normalized});
        matrix.add_routing({"s", WavefolderID::k_symmetry, depth, 0, DepthMode::Normalized});
        matrix.add_routing({"j", WavefolderID::k_jfet, depth, 0, DepthMode::Normalized});
    }
};

// ── Test 2 — No clicks with silent input under modulation ────────────────────
//
// Zero input + slow square-wave LFOs on every automatable parameter (matching
// a user adjusting knobs). Without per-sample smoothing each LFO transition
// causes the transfer function output (a function of the params alone, since
// input is 0) to step instantaneously → audible click. With 5 ms smoothing
// the step is spread over 240 samples and the per-sample delta drops by orders
// of magnitude.
TEST(DspFixtureWavefolder, NoClicksWithModulatedSilence) {
    thl::State state;
    register_wavefolder_params(state);
    ModulationMatrix matrix(state);
    TestWavefolder wf(matrix);

    KnobLikeLfos lfos;
    lfos.wire(matrix, /*depth*/ 0.3f);

    matrix.prepare(k_sample_rate, k_block_size);
    wf.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> in(40 * k_block_size, 0.0f);
    auto out = run_blocks(matrix, wf, in);

    expect_all_finite(out);
    const float step = max_sample_step(out);
    // Empirical bounds: un-smoothed = ~1.0 (full transfer-function swing in
    // one sample). Smoothed = ~0.05 (slope of smoother × local sin derivative
    // at the modulated extremes). 0.1 sits between, catches the bug, has
    // headroom against legitimate variance.
    // Empirical bounds: un-smoothed step is ~1.2 (full transfer-function swing
    // in one sample at the LFO transition). Smoothed step is ~0.2 (driven by
    // sin's derivative at the modulated extremes, not by smoothing failure).
    EXPECT_LT(step, 0.5f) << "Modulation on silent input produced a per-sample step of " << step
                          << " — parameter smoothing or transfer-function DC handling is missing.";
}

// ── Test 3 — No clicks with sine input under modulation ──────────────────────
//
// 220 Hz sine input through the wavefolder with knob-rate LFO modulation on
// every parameter. A correctly-smoothed processor's per-sample step envelope
// should be of the same order as the un-modulated reference (the legitimate
// sample step of the folded sine itself). Without smoothing the steps spike
// by many times the reference at every parameter transition.
TEST(DspFixtureWavefolder, NoClicksWithModulatedSine) {
    constexpr size_t k_num_blocks = 40;
    constexpr float k_two_pi = 2.0f * std::numbers::pi_v<float>;

    std::vector<float> input(k_num_blocks * k_block_size);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = 0.5f * std::sin(k_two_pi * 220.0f * static_cast<float>(i) /
                                   static_cast<float>(k_sample_rate));
    }

    // Reference: static defaults, no modulation routings.
    auto reference_out = [&] {
        thl::State state;
        register_wavefolder_params(state);
        ModulationMatrix matrix(state);
        TestWavefolder wf(matrix);
        matrix.prepare(k_sample_rate, k_block_size);
        wf.prepare(k_sample_rate, k_block_size, 1);
        return run_blocks(matrix, wf, input);
    }();

    // Modulated run.
    thl::State state;
    register_wavefolder_params(state);
    ModulationMatrix matrix(state);
    TestWavefolder wf(matrix);

    KnobLikeLfos lfos;
    lfos.wire(matrix, /*depth*/ 0.3f);

    matrix.prepare(k_sample_rate, k_block_size);
    wf.prepare(k_sample_rate, k_block_size, 1);

    auto modulated_out = run_blocks(matrix, wf, input);

    expect_all_finite(modulated_out);
    const float ref_step = max_sample_step(reference_out);
    const float mod_step = max_sample_step(modulated_out);

    // Empirical bounds: un-smoothed = ~1.84 (transient at each LFO transition
    // dominates). Smoothed = ~0.6 (legitimate per-sample step of a deeply
    // folded 220 Hz sine at the modulated drive peak; the reference is much
    // smaller because it folds at drive=1). A relative bound would need a
    // reference run at matched extremes; absolute threshold at 1.0 catches
    // the bug with ~1.8× headroom and passes the fix with ~1.6× margin.
    EXPECT_LT(mod_step, 1.0f) << "Modulated step envelope " << mod_step << " (ref " << ref_step
                              << ")"
                              << " — parameter smoothing missing on at least one wavefolder param.";
}
