// DspFixture applied to LimiterImpl.
//
// Notes on what's testable here:
//   - Test 1 (silence in → silence out) is trivially true for any input gain
//     stage (out = in × gain; in = 0 ⇒ out = 0). Included as a baseline
//     property check.
//   - Test 2 (clicks under modulated silence) is also trivial — gain × 0 = 0.
//     Kept for parity with the universal fixture suite.
//   - Test 3 is the real test: modulate Threshold (and Attack/Release) while
//     a sine input is being limited. Without per-sample smoothing of the
//     threshold, sub-block transitions cause the gain envelope to step,
//     audible as clicks.

#include <gtest/gtest.h>

#include "DspFixture.h"
#include "TestHelpers.h"

#include <span>
#include <string_view>
#include <vector>

#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/utils/Limiter.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/State.h>

using namespace thl::modulation;
using thl::dsp::utils::LimiterImpl;
using namespace dsp_fixture;

namespace {

constexpr std::string_view k_threshold = "lim.threshold";
constexpr std::string_view k_attack = "lim.attack";
constexpr std::string_view k_release = "lim.release";

class TestLimiter : public LimiterImpl {
public:
    explicit TestLimiter(ModulationMatrix& matrix) {
        m_handles.resize(NumParameters);
        m_handles[Threshold] = matrix.get_smart_handle<float>(k_threshold);
        m_handles[Attack] = matrix.get_smart_handle<float>(k_attack);
        m_handles[Release] = matrix.get_smart_handle<float>(k_release);
    }

    void prepare(const double& sr, const size_t& spb, const size_t& nc) override {
        m_change_points.reserve(spb);
        LimiterImpl::prepare(sr, spb, nc);
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

// Register Limiter parameters with sensible ranges. Defaults are chosen so the
// test signal (sine @ -6 dBFS) sits within the threshold range — i.e. the
// limiter is actually doing work, so threshold modulation matters.
void register_limiter_params(thl::State& state,
                             float threshold_db_default = -10.0f,
                             float attack_ms_default = 5.0f,
                             float release_ms_default = 50.0f) {
    state.create(k_threshold,
                 thl::ParameterDefinition::make_float("threshold",
                                                      thl::Range::linear(-30.0f, 0.0f),
                                                      threshold_db_default)
                     .modulatable(true));
    state.create(k_attack,
                 thl::ParameterDefinition::make_float("attack",
                                                      thl::Range::linear(0.1f, 50.0f),
                                                      attack_ms_default)
                     .modulatable(true));
    state.create(k_release,
                 thl::ParameterDefinition::make_float("release",
                                                      thl::Range::linear(1.0f, 500.0f),
                                                      release_ms_default)
                     .modulatable(true));
}

struct KnobLfos {
    TestLFOSource threshold, attack, release;

    KnobLfos() {
        for (auto* l : {&threshold, &attack, &release}) {
            l->m_waveform = LFOWaveform::Square;
            l->m_decimation = 1;
        }
        threshold.m_frequency = k_knob_lfo_freqs[0];  // 5 Hz
        attack.m_frequency = k_knob_lfo_freqs[1];     // 7 Hz
        release.m_frequency = k_knob_lfo_freqs[2];    // 11 Hz
    }

    void wire(ModulationMatrix& matrix, float depth = 0.3f) {
        matrix.add_source("thr", &threshold);
        matrix.add_source("atk", &attack);
        matrix.add_source("rel", &release);
        matrix.add_routing({"thr", k_threshold, depth, 0, DepthMode::Normalized});
        matrix.add_routing({"atk", k_attack, depth, 0, DepthMode::Normalized});
        matrix.add_routing({"rel", k_release, depth, 0, DepthMode::Normalized});
    }
};

}  // namespace

// ── Test 1 — Silence in → silence out ────────────────────────────────────────
TEST(DspFixtureLimiter, SilenceInSilenceOut) {
    thl::State state;
    register_limiter_params(state);
    ModulationMatrix matrix(state);
    TestLimiter lim(matrix);

    matrix.prepare(k_sample_rate, k_block_size);
    lim.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> in(10 * k_block_size, 0.0f);
    auto out = run_blocks_mono(matrix, lim, in);

    expect_all_finite(out);
    EXPECT_LT(max_abs(out), 1e-6f) << "Limiter must pass silence unchanged.";
}

// ── Test 2 — No clicks with modulated silence ────────────────────────────────
TEST(DspFixtureLimiter, NoClicksWithModulatedSilence) {
    thl::State state;
    register_limiter_params(state);
    ModulationMatrix matrix(state);
    TestLimiter lim(matrix);

    KnobLfos lfos;
    lfos.wire(matrix);

    matrix.prepare(k_sample_rate, k_block_size);
    lim.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> in(20 * k_block_size, 0.0f);
    auto out = run_blocks_mono(matrix, lim, in);

    expect_all_finite(out);
    EXPECT_LT(max_sample_step(out), 1e-6f)
        << "Modulating limiter params with silent input must not produce output.";
}

// ── Test 3 — No clicks with modulated signal ─────────────────────────────────
//
// 220 Hz sine at -6 dBFS feeding the limiter while Threshold/Attack/Release
// are modulated. The signal is loud enough that the limiter is in gain
// reduction over much of each cycle; threshold modulation directly changes
// the target_gain, so without smoothing each sub-block transition causes a
// step in the applied gain → click in the output.
TEST(DspFixtureLimiter, NoClicksWithModulatedSine) {
    constexpr size_t k_num_blocks = 40;

    auto input = make_test_sine(k_num_blocks, 220.0f, 0.5f);

    // Reference: static params, no modulation routings.
    auto reference_out = [&] {
        thl::State state;
        register_limiter_params(state);
        ModulationMatrix matrix(state);
        TestLimiter lim(matrix);
        matrix.prepare(k_sample_rate, k_block_size);
        lim.prepare(k_sample_rate, k_block_size, 1);
        return run_blocks_mono(matrix, lim, input);
    }();

    // Modulated run.
    thl::State state;
    register_limiter_params(state);
    ModulationMatrix matrix(state);
    TestLimiter lim(matrix);

    KnobLfos lfos;
    lfos.wire(matrix);

    matrix.prepare(k_sample_rate, k_block_size);
    lim.prepare(k_sample_rate, k_block_size, 1);

    auto modulated_out = run_blocks_mono(matrix, lim, input);

    expect_all_finite(modulated_out);
    const float ref_step = max_sample_step(reference_out);
    const float mod_step = max_sample_step(modulated_out);

    // The limiter envelope smoothing (attack/release coeffs) already filters
    // out most threshold-step transients in practice. We expect mod_step to be
    // close to ref_step. Allow generous headroom; a real bug would show 10×+.
    EXPECT_LT(mod_step, ref_step * 3.0f + 0.02f)
        << "Modulated step envelope " << mod_step << " ≫ reference " << ref_step
        << " — threshold modulation is bypassing the gain envelope smoother.";
}
