#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/dynamics/LookaheadLimiter.h>
#include <tanh/dsp/dynamics/StereoLink.h>
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
#include <numbers>

namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics {

namespace {

constexpr double k_kaiser_beta = 5.0;

/// The modified Bessel function I0 by its series (std::cyl_bessel_i is missing from libc++).
[[nodiscard]] double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k) {
        const double factor = x / (2.0 * k);
        term *= factor * factor;
        sum += term;
        if (term < sum * 1e-16) { break; }
    }
    return sum;
}

}  // namespace

float LookaheadLimiterImpl::value(Parameter parameter,
                                  uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    return utils::sanitised(get_parameter_float(parameter, modulation_offset),
                            k_parameter_ranges[parameter]);
}

void LookaheadLimiterImpl::prepare(const double& sample_rate,
                                   const size_t& samples_per_block,
                                   const size_t& num_channels) {
    m_sample_rate = sample_rate;
    m_num_channels = std::min(num_channels, k_max_channels);
    m_max_lookahead =
        std::max<size_t>(1,
                         static_cast<size_t>(std::ceil(k_max_lookahead_ms * 0.001 * sample_rate)));

    // The 4x interpolator: a Kaiser-windowed sinc of 4 · 24 + 1 taps, centred on tap 48, split
    // into its phases (phase p, tap j: h[4 j + p]). Phase 0 is the sample k_true_peak_delay back,
    // exactly.
    // Beta 5 against 6.5 and 8: the narrower transition holds the ceiling closer to Nyquist.
    constexpr size_t k_taps = (k_true_peak_factor * (k_true_peak_taps_per_phase - 1)) + 1;
    constexpr double k_centre = static_cast<double>(k_taps - 1) / 2.0;
    const double window_norm = bessel_i0(k_kaiser_beta);
    for (auto& phase : m_phases) { phase.fill(0.0f); }
    for (size_t m = 0; m < k_taps; ++m) {
        const double t = (static_cast<double>(m) - k_centre) / k_true_peak_factor;
        const double sinc =
            t == 0.0 ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
        const double r = (static_cast<double>(m) - k_centre) / k_centre;
        const double window = bessel_i0(k_kaiser_beta * std::sqrt(1.0 - (r * r))) / window_norm;
        m_phases[m % k_true_peak_factor][m / k_true_peak_factor] =
            static_cast<float>(sinc * window);
    }

    m_stages.assign(m_num_channels, {});
    for (auto& stage : m_stages) {
        stage.m_min_values.assign(m_max_lookahead + 1, 1.0f);
        stage.m_min_times.assign(m_max_lookahead + 1, 0);
        stage.m_average.assign(m_max_lookahead, 1.0f);
    }
    m_interpolators.assign(m_num_channels, {});
    m_peaks.assign(m_num_channels, 0.0f);
    m_gains.resize(m_num_channels, samples_per_block);

    const auto max_latency = static_cast<uint32_t>(m_max_lookahead - 1 + k_true_peak_delay);
    set_max_latency_samples(max_latency);
    m_delay.set_max_delay_samples(max_latency);
    m_delay.prepare(sample_rate, samples_per_block, num_channels);
    m_input_gain.reset(sample_rate, k_ramp_seconds);
    m_release_ms = -1.0f;
    m_lookahead = 0;
    read_parameters(0);
    m_input_gain.set_current_and_target_value(m_input_gain_target);
    reset();
}

void LookaheadLimiterImpl::restart(GainStage& stage, float gain) TANH_NONBLOCKING_FUNCTION {
    stage.m_min_head = 0;
    stage.m_min_size = 0;
    std::fill_n(stage.m_average.begin(), m_lookahead, gain);
    stage.m_average_position = 0;
    stage.m_sum = static_cast<double>(gain) * static_cast<double>(m_lookahead);
    stage.m_released_db = utils::gain_to_db(gain);
}

void LookaheadLimiterImpl::reset() TANH_NONBLOCKING_FUNCTION {
    for (auto& stage : m_stages) { restart(stage, 1.0f); }
    for (auto& interpolator : m_interpolators) { interpolator = {}; }
    m_delay.reset();
    m_input_gain.set_current_and_target_value(m_input_gain_target);
    m_meter_gain_db.store(0.0f, std::memory_order_relaxed);
    m_meter_input_db.store(utils::k_min_db, std::memory_order_relaxed);
    m_meter_output_db.store(utils::k_min_db, std::memory_order_relaxed);
}

void LookaheadLimiterImpl::read_parameters(uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    m_ceiling = utils::db_to_gain(value(Ceiling, modulation_offset));
    m_input_gain_target = utils::db_to_gain(value(InputGain, modulation_offset));
    m_input_gain.set_target_value(m_input_gain_target);
    const float release_ms = value(Release, modulation_offset);
    if (release_ms != m_release_ms) {
        m_release_ms = release_ms;
        m_release_coefficient = utils::one_pole_pole(release_ms, m_sample_rate);
    }

    // Switched on, the interpolators start from silence, not from what they held before.
    const bool true_peak = value(TruePeak, modulation_offset) > 0.5f;
    if (true_peak && !m_true_peak) {
        for (auto& interpolator : m_interpolators) { interpolator = {}; }
    }
    m_true_peak = true_peak;

    // Linked, only the first stage runs: a link change hands the gain over. To linked: the
    // deepest gain leads.
    const bool linked = utils::to_enum(get_parameter_float(Link, modulation_offset),
                                       StereoLink::DualMono,
                                       StereoLink::Linked) != StereoLink::DualMono;
    if (!m_stages.empty() && linked != m_linked) {
        if (linked) {
            copy_stage(m_stages.front(),
                       *std::ranges::max_element(m_stages, {}, [](const GainStage& g) {
                           return -g.m_released_db;
                       }));
        } else {
            for (size_t ch = 1; ch < m_stages.size(); ++ch) {
                copy_stage(m_stages[ch], m_stages.front());
            }
        }
    }
    m_linked = linked;

    const size_t lookahead = std::clamp<size_t>(
        static_cast<size_t>(
            std::lround(utils::ms_to_samples(value(Lookahead, modulation_offset), m_sample_rate))),
        1,
        m_max_lookahead);
    if (lookahead != m_lookahead) {
        // The averages restart over the new length at the gains they had reached.
        m_lookahead = lookahead;
        for (auto& stage : m_stages) { restart(stage, utils::db_to_gain(stage.m_released_db)); }
    }
    const auto latency =
        static_cast<uint32_t>(m_lookahead - 1 + (m_true_peak ? k_true_peak_delay : 0));
    m_delay.set_delay_samples(latency);
    set_latency_samples(latency);
}

void LookaheadLimiterImpl::copy_stage(GainStage& to,
                                      const GainStage& from) TANH_NONBLOCKING_FUNCTION {
    if (&to == &from) { return; }
    // Element-wise into the prepared rings: no allocation.
    std::ranges::copy(from.m_min_values, to.m_min_values.begin());
    std::ranges::copy(from.m_min_times, to.m_min_times.begin());
    std::ranges::copy(from.m_average, to.m_average.begin());
    to.m_min_head = from.m_min_head;
    to.m_min_size = from.m_min_size;
    to.m_average_position = from.m_average_position;
    to.m_sum = from.m_sum;
    to.m_released_db = from.m_released_db;
}

float LookaheadLimiterImpl::advance(GainStage& stage, float required) TANH_NONBLOCKING_FUNCTION {
    // The minimum over the last L: a queue of rising values, the oldest first.
    const size_t capacity = stage.m_min_values.size();
    while (stage.m_min_size > 0) {
        const size_t back = (stage.m_min_head + stage.m_min_size - 1) % capacity;
        if (stage.m_min_values[back] < required) { break; }
        --stage.m_min_size;
    }
    const size_t slot = (stage.m_min_head + stage.m_min_size) % capacity;
    stage.m_min_values[slot] = required;
    stage.m_min_times[slot] = m_time;
    ++stage.m_min_size;
    while (stage.m_min_times[stage.m_min_head] + m_lookahead <= m_time) {
        stage.m_min_head = (stage.m_min_head + 1) % capacity;
        --stage.m_min_size;
    }
    const float minimum = stage.m_min_values[stage.m_min_head];
    const float minimum_db = minimum < 1.0f ? utils::gain_to_db(minimum) : 0.0f;

    // Down at once, back with the release, in dB.
    if (minimum_db <= stage.m_released_db) {
        stage.m_released_db = minimum_db;
    } else {
        stage.m_released_db =
            minimum_db + (m_release_coefficient * (stage.m_released_db - minimum_db));
    }
    // Both shortcuts skip the log and the exp while nothing is limited.
    const float released =
        stage.m_released_db < 0.0f ? utils::db_to_gain(stage.m_released_db) : 1.0f;

    // The average over the last L: a linear ramp down to every minimum.
    float& oldest = stage.m_average[stage.m_average_position];
    stage.m_sum += static_cast<double>(released) - static_cast<double>(oldest);
    oldest = released;
    stage.m_average_position = (stage.m_average_position + 1) % m_lookahead;
    return std::min(1.0f, static_cast<float>(stage.m_sum / static_cast<double>(m_lookahead)));
}

float LookaheadLimiterImpl::key_peak(Interpolator& interpolator,
                                     float x) const TANH_NONBLOCKING_FUNCTION {
    if (!m_true_peak) { return std::abs(x); }
    // The newest sample at m_position, written twice (k_true_peak_taps_per_phase apart), so
    // tap j, the sample j back, is m_history[m_position + j] without wrapping.
    interpolator.m_position =
        (interpolator.m_position + k_true_peak_taps_per_phase - 1) % k_true_peak_taps_per_phase;
    interpolator.m_history[interpolator.m_position] = x;
    interpolator.m_history[interpolator.m_position + k_true_peak_taps_per_phase] = x;
    const float* history = interpolator.m_history.data() + interpolator.m_position;
    // Between the aligned sample (k_true_peak_delay back) and the one after it.
    float interval = 0.0f;
    for (size_t p = 1; p < k_true_peak_factor; ++p) {
        float value = 0.0f;
        for (size_t j = 0; j < k_true_peak_taps_per_phase; ++j) {
            value += m_phases[p][j] * history[j];
        }
        interval = std::max(interval, std::abs(value));
    }
    // The aligned sample and the peak after it (the one before is its predecessor's: the gain
    // ramps over the lookahead, so neighbours' gains hardly differ).
    return std::max(std::abs(history[k_true_peak_delay]), interval);
}

void LookaheadLimiterImpl::process(thl::core::BufferView buffer,
                                   uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    read_parameters(modulation_offset);
    const size_t n = buffer.get_num_samples();
    const size_t channels = std::min(buffer.get_num_channels(), m_num_channels);
    assert(n <= m_gains.get_num_samples());

    // Input gain, then the gain each sample needs, from the undelayed input.
    const auto required = [this](float peak) { return peak > m_ceiling ? m_ceiling / peak : 1.0f; };
    float deepest = 1.0f;
    float input_peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float drive = m_input_gain.get_smoothed_value();
        float loudest = 0.0f;
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            float& x = buffer.get_write_pointer(ch)[i];
            x *= drive;
            if (ch < channels) {
                input_peak = std::max(input_peak, std::abs(x));
                m_peaks[ch] = key_peak(m_interpolators[ch], x);
                loudest = std::max(loudest, m_peaks[ch]);
            }
        }
        if (m_linked) {
            const float gain = channels > 0 ? advance(m_stages[0], required(loudest)) : 1.0f;
            for (size_t ch = 0; ch < channels; ++ch) { m_gains.get_write_pointer(ch)[i] = gain; }
            deepest = std::min(deepest, gain);
        } else {
            for (size_t ch = 0; ch < channels; ++ch) {
                const float gain = advance(m_stages[ch], required(m_peaks[ch]));
                m_gains.get_write_pointer(ch)[i] = gain;
                deepest = std::min(deepest, gain);
            }
        }
        ++m_time;
    }

    // The delayed audio times its gain, then the safety clip.
    m_delay.process(buffer);
    float output_peak = 0.0f;
    for (size_t ch = 0; ch < channels; ++ch) {
        float* samples = buffer.get_write_pointer(ch);
        const float* gains = m_gains.get_read_pointer(ch);
        for (size_t i = 0; i < n; ++i) {
            samples[i] = std::clamp(samples[i] * gains[i], -m_ceiling, m_ceiling);
            output_peak = std::max(output_peak, std::abs(samples[i]));
        }
    }
    m_meter_gain_db.store(utils::gain_to_db(deepest), std::memory_order_relaxed);
    m_meter_input_db.store(utils::gain_to_db(input_peak), std::memory_order_relaxed);
    m_meter_output_db.store(utils::gain_to_db(output_peak), std::memory_order_relaxed);
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics
