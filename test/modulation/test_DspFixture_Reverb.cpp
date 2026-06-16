// DspFixture applied to ConstellationReverbImpl.
//
// Reverb-specific adaptations:
//   - The reverb is stereo (requires 2 channels). Tests use run_blocks_stereo.
//   - Internal Brownian noise sources modulate delay-line and pitch-shifter
//     positions. With silent input these stay benign (zero × shift = zero), so
//     silence-in-silence-out still applies.
//   - Two params (Size, FreqShift) already have internal one-pole smoothing.
//     The remaining params are cached once per process() call. Sub-block param
//     transitions therefore step at sub-block boundaries; the tank's filter
//     network smears most steps, but level-mixing params (Shimmer, FreqShift
//     wet mix, Decay) can still produce audible discontinuities.

#include <gtest/gtest.h>
#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/fx/ConstellationReverb.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/State.h>

#include <span>
#include <string_view>
#include <vector>

#include "DspFixture.h"
#include "TestHelpers.h"

using namespace thl::modulation;
using thl::dsp::fx::ConstellationReverbImpl;
using namespace dsp_fixture;

namespace {

// Parameter paths — order matches the enum in ConstellationReverbImpl.
constexpr std::string_view k_decay = "rev.decay";
constexpr std::string_view k_size = "rev.size";
constexpr std::string_view k_freeze = "rev.freeze";
constexpr std::string_view k_predelay = "rev.predelay_ms";
constexpr std::string_view k_damping = "rev.damping";
constexpr std::string_view k_input_hp = "rev.input_hp_hz";
constexpr std::string_view k_shimmer = "rev.shimmer";
constexpr std::string_view k_shimmer_semis = "rev.shimmer_semis";
constexpr std::string_view k_shimmer_detune = "rev.shimmer_detune";
constexpr std::string_view k_shimmer_mod = "rev.shimmer_mod";
constexpr std::string_view k_fshift = "rev.fshift";
constexpr std::string_view k_fshift_hz = "rev.fshift_hz";
constexpr std::string_view k_fshift_detune = "rev.fshift_detune";
constexpr std::string_view k_fshift_mod = "rev.fshift_mod";
constexpr std::string_view k_channel_mode = "rev.channel_mode";

class TestReverb : public ConstellationReverbImpl {
public:
    explicit TestReverb(ModulationMatrix& matrix) {
        m_float_handles.resize(NumParameters);
        m_bool_handles.resize(NumParameters);
        m_int_handles.resize(NumParameters);

        m_float_handles[Decay] = matrix.get_smart_handle<float>(k_decay);
        m_float_handles[Size] = matrix.get_smart_handle<float>(k_size);
        m_bool_handles[Freeze] = matrix.get_smart_handle<bool>(k_freeze);
        m_float_handles[PredelayMs] = matrix.get_smart_handle<float>(k_predelay);
        m_float_handles[Damping] = matrix.get_smart_handle<float>(k_damping);
        m_float_handles[InputHpHz] = matrix.get_smart_handle<float>(k_input_hp);
        m_float_handles[Shimmer] = matrix.get_smart_handle<float>(k_shimmer);
        m_float_handles[ShimmerSemitones] = matrix.get_smart_handle<float>(k_shimmer_semis);
        m_float_handles[ShimmerDetune] = matrix.get_smart_handle<float>(k_shimmer_detune);
        m_float_handles[ShimmerModDepth] = matrix.get_smart_handle<float>(k_shimmer_mod);
        m_float_handles[FreqShift] = matrix.get_smart_handle<float>(k_fshift);
        m_float_handles[FreqShiftHz] = matrix.get_smart_handle<float>(k_fshift_hz);
        m_float_handles[FreqShiftDetune] = matrix.get_smart_handle<float>(k_fshift_detune);
        m_float_handles[FreqShiftModDepth] = matrix.get_smart_handle<float>(k_fshift_mod);
        m_int_handles[ChannelModeParam] = matrix.get_smart_handle<int>(k_channel_mode);

        // Aggregate change-point handles (float-only — the cached bool/int
        // params don't drive sub-block splitting in this test).
        m_change_point_handles.reserve(NumParameters);
        for (auto& h : m_float_handles) { m_change_point_handles.push_back(h); }
    }

    void prepare(const double& sr, const size_t& spb, const size_t& nc) override {
        m_change_points.reserve(spb);
        ConstellationReverbImpl::prepare(sr, spb, nc);
    }

protected:
    float get_parameter_float(Parameter p, uint32_t modulation_offset) override {
        return m_float_handles[p].load(modulation_offset);
    }
    bool get_parameter_bool(Parameter p, uint32_t modulation_offset) override {
        return m_bool_handles[p].load(modulation_offset);
    }
    int get_parameter_int(Parameter p, uint32_t modulation_offset) override {
        return m_int_handles[p].load(modulation_offset);
    }
    std::span<const uint32_t> get_change_points() override {
        thl::modulation::collect_change_points(
            std::span<const SmartHandle<float>>(m_change_point_handles),
            m_change_points);
        return m_change_points;
    }

private:
    std::vector<SmartHandle<float>> m_float_handles;
    std::vector<SmartHandle<bool>> m_bool_handles;
    std::vector<SmartHandle<int>> m_int_handles;
    std::vector<SmartHandle<float>> m_change_point_handles;
    std::vector<uint32_t> m_change_points;
};

void register_reverb_params(thl::State& state) {
    using thl::ParameterDefinition;
    using thl::Range;
    state.create(k_decay,
                 ParameterDefinition::make_float("decay", Range::linear(0.0f, 1.0f), 0.5f)
                     .modulatable(true));
    state.create(
        k_size,
        ParameterDefinition::make_float("size", Range::linear(0.1f, 4.0f), 1.0f).modulatable(true));
    state.create(k_freeze, ParameterDefinition::make_bool("freeze", false).modulatable(true));
    state.create(k_predelay,
                 ParameterDefinition::make_float("predelay", Range::linear(0.0f, 200.0f), 10.0f)
                     .modulatable(true));
    state.create(k_damping,
                 ParameterDefinition::make_float("damping", Range::linear(0.0f, 1.0f), 0.3f)
                     .modulatable(true));
    state.create(k_input_hp,
                 ParameterDefinition::make_float("input_hp", Range::linear(20.0f, 2000.0f), 100.0f)
                     .modulatable(true));
    state.create(k_shimmer,
                 ParameterDefinition::make_float("shimmer", Range::linear(0.0f, 1.0f), 0.0f)
                     .modulatable(true));
    state.create(
        k_shimmer_semis,
        ParameterDefinition::make_float("shimmer_semis", Range::linear(-24.0f, 24.0f), 12.0f)
            .modulatable(true));
    state.create(k_shimmer_detune,
                 ParameterDefinition::make_float("shimmer_detune", Range::linear(-1.0f, 1.0f), 0.0f)
                     .modulatable(true));
    state.create(k_shimmer_mod,
                 ParameterDefinition::make_float("shimmer_mod", Range::linear(0.0f, 100.0f), 0.0f)
                     .modulatable(true));
    state.create(k_fshift,
                 ParameterDefinition::make_float("fshift", Range::linear(0.0f, 1.0f), 0.0f)
                     .modulatable(true));
    state.create(k_fshift_hz,
                 ParameterDefinition::make_float("fshift_hz", Range::linear(0.0f, 500.0f), 100.0f)
                     .modulatable(true));
    state.create(k_fshift_detune,
                 ParameterDefinition::make_float("fshift_detune", Range::linear(-1.0f, 1.0f), 0.0f)
                     .modulatable(true));
    state.create(k_fshift_mod,
                 ParameterDefinition::make_float("fshift_mod", Range::linear(0.0f, 100.0f), 20.0f)
                     .modulatable(true));
    state.create(
        k_channel_mode,
        ParameterDefinition::make_int("channel_mode", Range::discrete(0, 1), 0).modulatable(true));
}

// Modulate the level-mixing and tonal-shaping params at knob rate. Skip
// PredelayMs (would cause real audible jumps in delay readout — that's a
// design choice, not a click bug) and the per-shifter detune/mod params (no
// direct output coupling at default Shimmer=0 / FreqShift=0).
struct KnobLfos {
    TestLFOSource decay, damping, shimmer, fshift, input_hp, size;

    KnobLfos() {
        for (auto* l : {&decay, &damping, &shimmer, &fshift, &input_hp, &size}) {
            l->m_waveform = LFOWaveform::Square;
            l->m_decimation = 1;
        }
        decay.m_frequency = k_knob_lfo_freqs[0];     // 5 Hz
        damping.m_frequency = k_knob_lfo_freqs[1];   // 7 Hz
        shimmer.m_frequency = k_knob_lfo_freqs[2];   // 11 Hz
        fshift.m_frequency = k_knob_lfo_freqs[3];    // 13 Hz
        input_hp.m_frequency = k_knob_lfo_freqs[4];  // 17 Hz
        size.m_frequency = k_knob_lfo_freqs[5];      // 19 Hz
    }

    void wire(ModulationMatrix& matrix, float depth = 0.3f) {
        matrix.add_source("dec", &decay);
        matrix.add_source("dmp", &damping);
        matrix.add_source("shm", &shimmer);
        matrix.add_source("fsh", &fshift);
        matrix.add_source("ihp", &input_hp);
        matrix.add_source("siz", &size);
        matrix.add_routing({"dec", k_decay, depth, 0, DepthMode::Normalized});
        matrix.add_routing({"dmp", k_damping, depth, 0, DepthMode::Normalized});
        matrix.add_routing({"shm", k_shimmer, depth, 0, DepthMode::Normalized});
        matrix.add_routing({"fsh", k_fshift, depth, 0, DepthMode::Normalized});
        matrix.add_routing({"ihp", k_input_hp, depth, 0, DepthMode::Normalized});
        matrix.add_routing({"siz", k_size, depth, 0, DepthMode::Normalized});
    }
};

}  // namespace

// ── Test 1 — Silence in → silence out ────────────────────────────────────────
//
// Internal Brownian noise modulates delay-line read positions, but with a
// zero input there's no audio in the tank for the modulators to act on.
// Output should converge to ≈0 quickly.
TEST(DspFixtureReverb, SilenceInSilenceOut) {
    thl::State state;
    register_reverb_params(state);
    ModulationMatrix matrix(state);
    TestReverb rv(matrix);

    matrix.prepare(k_sample_rate, k_block_size);
    rv.prepare(k_sample_rate, k_block_size, 2);

    std::vector<float> in(20 * k_block_size, 0.0f);
    auto [out_l, out_r] = run_blocks_stereo(matrix, rv, in);

    expect_all_finite(out_l);
    expect_all_finite(out_r);
    EXPECT_LT(max_abs(out_l), 1e-4f);
    EXPECT_LT(max_abs(out_r), 1e-4f);
}

// ── Test 2 — No output with modulated silence ────────────────────────────────
TEST(DspFixtureReverb, NoOutputWithModulatedSilence) {
    thl::State state;
    register_reverb_params(state);
    ModulationMatrix matrix(state);
    TestReverb rv(matrix);

    KnobLfos lfos;
    lfos.wire(matrix);

    matrix.prepare(k_sample_rate, k_block_size);
    rv.prepare(k_sample_rate, k_block_size, 2);

    std::vector<float> in(20 * k_block_size, 0.0f);
    auto [out_l, out_r] = run_blocks_stereo(matrix, rv, in);

    expect_all_finite(out_l);
    expect_all_finite(out_r);
    EXPECT_LT(max_abs(out_l), 1e-4f);
    EXPECT_LT(max_abs(out_r), 1e-4f);
}

// ── Test 3 — No clicks with modulated sine ───────────────────────────────────
//
// A 220 Hz sine fed into the reverb while Decay, Damping, Shimmer, FreqShift,
// InputHp and Size are modulated at knob rate. Most reverb params are cached
// per process() call, so without smoothing each sub-block transition could
// step the output. The tank's filter network smears most steps, but level-
// mixing params (Shimmer / FreqShift wet mix) and feedback (Decay) directly
// scale the output and would click without smoothing.
//
// Skips the first 2 blocks to let the reverb tail build up; otherwise the
// reference per-sample step is dominated by the impulse-response onset which
// the modulated run also has.
TEST(DspFixtureReverb, NoClicksWithModulatedSine) {
    constexpr size_t k_num_blocks = 40;
    constexpr size_t k_skip_blocks = 2;

    auto input = make_test_sine(k_num_blocks, 220.0f, 0.5f);

    // Reference: static defaults.
    auto reference = [&] {
        thl::State state;
        register_reverb_params(state);
        ModulationMatrix matrix(state);
        TestReverb rv(matrix);
        matrix.prepare(k_sample_rate, k_block_size);
        rv.prepare(k_sample_rate, k_block_size, 2);
        return run_blocks_stereo(matrix, rv, input);
    }();

    // Modulated run.
    thl::State state;
    register_reverb_params(state);
    ModulationMatrix matrix(state);
    TestReverb rv(matrix);
    KnobLfos lfos;
    lfos.wire(matrix);
    matrix.prepare(k_sample_rate, k_block_size);
    rv.prepare(k_sample_rate, k_block_size, 2);
    auto [out_l, out_r] = run_blocks_stereo(matrix, rv, input);

    expect_all_finite(out_l);
    expect_all_finite(out_r);

    auto tail = [&](const std::vector<float>& v) {
        return std::vector<float>(v.begin() + static_cast<ptrdiff_t>(k_skip_blocks * k_block_size),
                                  v.end());
    };

    const float ref_step_l = max_sample_step(tail(reference.first));
    const float ref_step_r = max_sample_step(tail(reference.second));
    const float mod_step_l = max_sample_step(tail(out_l));
    const float mod_step_r = max_sample_step(tail(out_r));

    // Per-block param caching means each sub-block sees stepped params. The
    // tank smears most steps; allow 5× the reference per-sample envelope
    // plus an absolute floor for params that scale the output directly.
    EXPECT_LT(mod_step_l, ref_step_l * 5.0f + 0.05f)
        << "L: mod=" << mod_step_l << " ref=" << ref_step_l;
    EXPECT_LT(mod_step_r, ref_step_r * 5.0f + 0.05f)
        << "R: mod=" << mod_step_r << " ref=" << ref_step_r;
}
