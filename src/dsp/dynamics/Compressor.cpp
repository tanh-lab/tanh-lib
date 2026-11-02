#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/dynamics/Compressor.h>
#include <tanh/dsp/dynamics/GainComputer.h>
#include <tanh/dsp/dynamics/GainSmoother.h>
#include <tanh/dsp/dynamics/LevelDetector.h>
#include <tanh/dsp/dynamics/StereoLink.h>
#include <tanh/dsp/filter/EqDesign.h>
#include <tanh/dsp/filter/SvfCoefficients.h>
#include <tanh/dsp/utils/Decibels.h>
#include <tanh/dsp/utils/ParameterValue.h>
#include <tanh/dsp/utils/TimeConstant.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics {

namespace {
constexpr float k_min_high_pass_hz = 10.0f;  // below: the sidechain high-pass is off
}  // namespace

float CompressorImpl::value(Parameter parameter,
                            uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    return utils::sanitised(get_parameter_float(parameter, modulation_offset),
                            k_parameter_ranges[parameter]);
}

bool CompressorImpl::switch_on(Parameter parameter,
                               uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    return value(parameter, modulation_offset) > 0.5f;
}

template <typename Enum>
Enum CompressorImpl::choice(Parameter parameter,
                            uint32_t modulation_offset,
                            Enum last) TANH_NONBLOCKING_FUNCTION {
    return utils::to_enum(get_parameter_float(parameter, modulation_offset),
                          last,
                          static_cast<Enum>(k_parameter_ranges[parameter].m_default));
}

void CompressorImpl::read_parameters(uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    Settings& s = m_settings;
    s.m_curve.m_threshold_db = value(Threshold, modulation_offset);
    s.m_curve.m_ratio = value(Ratio, modulation_offset);
    s.m_curve.m_knee_db = value(Knee, modulation_offset);
    s.m_curve.m_range_db = value(Range, modulation_offset);
    s.m_curve.m_mode = choice(Mode, modulation_offset, DynamicsMode::UpwardExpansion);
    s.m_attack_ms = value(Attack, modulation_offset);
    s.m_hold_ms = value(Hold, modulation_offset);
    s.m_release_ms = value(Release, modulation_offset);
    s.m_auto_release = switch_on(AutoRelease, modulation_offset);
    s.m_lookahead_ms = value(Lookahead, modulation_offset);
    s.m_detector = choice(Detector, modulation_offset, DetectorMode::Rms);
    s.m_rms_window_ms = value(RmsWindow, modulation_offset);
    s.m_link = choice(Link, modulation_offset, StereoLink::DualMono);
    s.m_high_pass_hz = value(SidechainHighPass, modulation_offset);  // design_band() caps it
    s.m_makeup_db = value(Makeup, modulation_offset);
    s.m_auto_makeup = switch_on(AutoMakeup, modulation_offset);
    s.m_mix = value(Mix, modulation_offset);
}

float CompressorImpl::total_makeup_db() const TANH_NONBLOCKING_FUNCTION {
    float makeup = m_settings.m_makeup_db;
    if (m_settings.m_auto_makeup) {
        makeup += std::clamp(-0.5f * m_settings.m_curve.gain_db(0.0f),
                             -k_max_auto_makeup_db,
                             k_max_auto_makeup_db);
    }
    return makeup;
}

void CompressorImpl::apply_settings() TANH_NONBLOCKING_FUNCTION {
    const Settings& s = m_settings;
    for (auto& detector : m_detectors) {
        detector.set_mode(s.m_detector);
        detector.set_rms_window_ms(s.m_rms_window_ms);
    }
    for (auto& smoother : m_smoothers) {
        smoother.set_times(s.m_attack_ms, s.m_release_ms, s.m_hold_ms, s.m_auto_release);
    }
    // Linked, only the first smoother runs: a link change hands the gain over, so no channel
    // jumps to a gain it had before. To linked: the gain farthest from 0 dB leads.
    const bool linked = s.m_link != StereoLink::DualMono;
    if (!m_smoothers.empty() && linked != m_linked) {
        if (linked) {
            const auto farthest =
                std::ranges::max_element(m_smoothers, {}, [](const GainSmoother& g) {
                    return std::abs(g.gain_db());
                });
            m_smoothers.front() = *farthest;
        } else {
            std::fill(m_smoothers.begin() + 1, m_smoothers.end(), m_smoothers.front());
        }
    }
    m_linked = linked;
    if (s.m_high_pass_hz != m_applied_high_pass_hz) {
        m_applied_high_pass_hz = s.m_high_pass_hz;
        m_high_pass_on = s.m_high_pass_hz >= k_min_high_pass_hz;
        if (m_high_pass_on) {
            const thl::dsp::filter::EqBand band{.m_type = thl::dsp::filter::EqBandType::LowCut,
                                                .m_frequency = s.m_high_pass_hz};
            m_high_pass = thl::dsp::filter::design_band(band,
                                                        thl::dsp::filter::EqDesign::Bilinear,
                                                        m_sample_rate);
        }
    }
    const auto lookahead = static_cast<uint32_t>(
        std::lround(std::min(utils::ms_to_samples(s.m_lookahead_ms, m_sample_rate),
                             static_cast<double>(max_latency_samples()))));
    m_lookahead.set_delay_samples(lookahead);
    set_latency_samples(lookahead);
    m_makeup.set_target_value(total_makeup_db());
    m_mix.set_target_value(s.m_mix);
}

void CompressorImpl::prepare(const double& sample_rate,
                             const size_t& samples_per_block,
                             const size_t& num_channels) {
    m_sample_rate = sample_rate;
    m_num_channels = std::min(num_channels, k_max_channels);
    m_high_pass_state.assign(m_num_channels, {});
    m_detectors.assign(m_num_channels, {});
    m_smoothers.assign(m_num_channels, {});
    for (auto& detector : m_detectors) { detector.prepare(sample_rate); }
    for (auto& smoother : m_smoothers) { smoother.prepare(sample_rate); }
    m_powers.assign(m_num_channels, 0.0f);
    m_gains.resize(m_num_channels, samples_per_block);

    const auto max_lookahead =
        static_cast<uint32_t>(std::ceil(utils::ms_to_samples(k_max_lookahead_ms, sample_rate)));
    set_max_latency_samples(max_lookahead);
    m_lookahead.set_max_delay_samples(max_lookahead);
    m_lookahead.prepare(sample_rate, samples_per_block, num_channels);

    m_makeup.reset(sample_rate, k_ramp_seconds);
    m_mix.reset(sample_rate, k_ramp_seconds);
    m_applied_high_pass_hz = -1.0f;  // none designed yet
    read_parameters(0);
    apply_settings();
    reset();
}

void CompressorImpl::reset() TANH_NONBLOCKING_FUNCTION {
    for (auto& state : m_high_pass_state) { state.reset(); }
    for (auto& detector : m_detectors) { detector.reset(); }
    for (auto& smoother : m_smoothers) { smoother.reset(); }
    m_lookahead.reset();
    m_makeup.set_current_and_target_value(total_makeup_db());
    m_mix.set_current_and_target_value(m_settings.m_mix);
    m_meter_gain_db.store(0.0f, std::memory_order_relaxed);
    m_meter_input_db.store(utils::k_min_db, std::memory_order_relaxed);
    m_meter_output_db.store(utils::k_min_db, std::memory_order_relaxed);
}

void CompressorImpl::process(thl::core::BufferView buffer,
                             uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    run(buffer, buffer, modulation_offset);
}

void CompressorImpl::process_with_sidechain(const thl::core::BufferView& buffer,
                                            const thl::core::BufferView& sidechain,
                                            uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    if (sidechain.get_num_channels() == 0) {
        run(buffer, buffer, modulation_offset);
        return;
    }
    assert(sidechain.get_num_samples() == buffer.get_num_samples());
    run(buffer, sidechain, modulation_offset);
}

void CompressorImpl::run(thl::core::BufferView buffer,
                         const thl::core::BufferView& key,
                         uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    const size_t n = buffer.get_num_samples();
    const size_t channels = std::min(buffer.get_num_channels(), m_num_channels);
    assert(n <= m_gains.get_num_samples());
    read_parameters(modulation_offset);
    apply_settings();

    // The gain per channel and sample, in dB, from the undelayed key.
    const GainComputer& curve = m_settings.m_curve;
    const thl::dsp::filter::SvfTick high_pass(m_high_pass);
    const size_t key_channels = key.get_num_channels();
    float input_peak = 0.0f;
    float gain_extreme = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        for (size_t ch = 0; ch < channels; ++ch) {
            float x = key.get_read_pointer(std::min(ch, key_channels - 1))[i];
            input_peak = std::max(input_peak, std::abs(x));
            if (m_high_pass_on) { x = high_pass.process(m_high_pass_state[ch], x); }
            m_powers[ch] = m_detectors[ch].process(x);
        }
        if (m_linked) {
            const float power =
                link_powers(m_settings.m_link, std::span<const float>(m_powers.data(), channels));
            const float gain = m_smoothers[0].process(curve.gain_db(utils::power_to_db(power)));
            for (size_t ch = 0; ch < channels; ++ch) { m_gains.get_write_pointer(ch)[i] = gain; }
            if (std::abs(gain) > std::abs(gain_extreme)) { gain_extreme = gain; }
        } else {
            for (size_t ch = 0; ch < channels; ++ch) {
                const float gain =
                    m_smoothers[ch].process(curve.gain_db(utils::power_to_db(m_powers[ch])));
                m_gains.get_write_pointer(ch)[i] = gain;
                if (std::abs(gain) > std::abs(gain_extreme)) { gain_extreme = gain; }
            }
        }
    }

    // The audio, delayed by the lookahead, times the gain.
    m_lookahead.process(buffer);
    float output_peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float makeup_db = m_makeup.get_smoothed_value();
        const float mix = m_mix.get_smoothed_value();
        for (size_t ch = 0; ch < channels; ++ch) {
            float* samples = buffer.get_write_pointer(ch);
            const float gain = utils::db_to_gain(m_gains.get_read_pointer(ch)[i] + makeup_db);
            samples[i] *= (1.0f - mix) + (mix * gain);
            output_peak = std::max(output_peak, std::abs(samples[i]));
        }
    }

    m_meter_gain_db.store(gain_extreme, std::memory_order_relaxed);
    m_meter_input_db.store(utils::gain_to_db(input_peak), std::memory_order_relaxed);
    m_meter_output_db.store(utils::gain_to_db(output_peak), std::memory_order_relaxed);
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics
