#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/fx/StereoFDN.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

namespace thl::dsp::fx {

namespace {

constexpr double k_default_max_delay_seconds = 2.0;
constexpr size_t k_default_delay_samples = 2400;
constexpr float k_min_base_time_ms = 1.0f;
constexpr float k_min_damping_cutoff_hz = 800.0f;
constexpr float k_max_damping_cutoff_hz = 20000.0f;
constexpr float k_max_delay_slew_samples_per_sample = 0.5f;

constexpr std::array<float, 5> k_left_delay_base_ratios = {0.67f, 0.89f, 1.13f, 1.41f, 1.73f};
constexpr std::array<float, 5> k_right_delay_base_ratios = {0.71f, 0.97f, 1.19f, 1.53f, 1.81f};
constexpr std::array<float, 5> k_householder_vector = {1.0f, -0.7f, 0.5f, -0.3f, 0.2f};
constexpr std::array<float, 16> k_anderson_core = {
    0.5f,
    0.5f,
    0.5f,
    -0.5f,
    -0.5f,
    0.5f,
    0.5f,
    0.5f,
    0.5f,
    -0.5f,
    0.5f,
    0.5f,
    0.5f,
    0.5f,
    -0.5f,
    0.5f,
};

size_t ms_to_samples(float delay_ms, double sample_rate) {
    const float clamped_ms = std::max(delay_ms, 0.0f);
    const double samples = static_cast<double>(clamped_ms) * sample_rate / 1000.0;
    return static_cast<size_t>(std::lround(samples));
}

float base_delay_ratio(size_t channel, size_t line) {
    const auto& ratios =
        channel == StereoFDN::k_left ? k_left_delay_base_ratios : k_right_delay_base_ratios;
    return ratios[line % ratios.size()];
}

size_t largest_power_of_two_at_most(size_t size) {
    size_t power = 1;
    while (power * 2 <= size) { power *= 2; }
    return power;
}

float hadamard_value(size_t row, size_t col, size_t size) {
    size_t bits = row & col;
    bool odd = false;
    while (bits != 0) {
        odd = !odd;
        bits &= bits - 1;
    }
    return (odd ? -1.0f : 1.0f) / std::sqrt(static_cast<float>(size));
}

float householder_value(size_t index, size_t size) {
    if (size == k_householder_vector.size()) { return k_householder_vector[index]; }
    return index % 2 == 0 ? 1.0f : -0.7f;
}

float damping_to_lowpass_coeff(float damping, double sample_rate) {
    const float d = std::clamp(damping, 0.0f, 1.0f);
    if (d <= 0.0f) { return 1.0f; }

    const float cutoff = std::exp(std::log(k_max_damping_cutoff_hz) * (1.0f - d) +
                                  std::log(k_min_damping_cutoff_hz) * d);
    const float clamped_cutoff = std::clamp(cutoff, 0.0f, static_cast<float>(sample_rate * 0.45));
    return 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * clamped_cutoff /
                           static_cast<float>(sample_rate));
}

void normalize_by_power_iteration(std::vector<float>& matrix, size_t size) {
    if (size == 0) { return; }

    std::vector<float> v(size, 1.0f / std::sqrt(static_cast<float>(size)));
    std::vector<float> w(size, 0.0f);
    std::vector<float> z(size, 0.0f);

    for (size_t iter = 0; iter < 20; ++iter) {
        std::ranges::fill(w, 0.0f);
        std::ranges::fill(z, 0.0f);

        for (size_t row = 0; row < size; ++row) {
            for (size_t col = 0; col < size; ++col) { w[row] += matrix[row * size + col] * v[col]; }
        }
        for (size_t col = 0; col < size; ++col) {
            for (size_t row = 0; row < size; ++row) { z[col] += matrix[row * size + col] * w[row]; }
        }

        float norm = 0.0f;
        for (const float value : z) { norm += value * value; }
        norm = std::sqrt(norm);
        if (norm <= 0.0f) { return; }
        for (size_t i = 0; i < size; ++i) { v[i] = z[i] / norm; }
    }

    std::ranges::fill(w, 0.0f);
    for (size_t row = 0; row < size; ++row) {
        for (size_t col = 0; col < size; ++col) { w[row] += matrix[row * size + col] * v[col]; }
    }

    float spectral_norm = 0.0f;
    for (const float value : w) { spectral_norm += value * value; }
    spectral_norm = std::sqrt(spectral_norm);
    if (spectral_norm > 0.0f) {
        for (float& value : matrix) { value /= spectral_norm; }
    }
}

void make_random_orthogonal(std::vector<float>& matrix, size_t size) {
    std::vector<float> q(size * size, 0.0f);
    std::vector<float> column(size, 0.0f);

    for (size_t col = 0; col < size; ++col) {
        for (size_t row = 0; row < size; ++row) {
            column[row] = std::sin(static_cast<float>(row + 1) * 12.9898f +
                                   static_cast<float>(col + 1) * 78.233f);
        }

        for (size_t prev = 0; prev < col; ++prev) {
            float projection = 0.0f;
            for (size_t row = 0; row < size; ++row) {
                projection += column[row] * q[row * size + prev];
            }
            for (size_t row = 0; row < size; ++row) {
                column[row] -= projection * q[row * size + prev];
            }
        }

        float norm = 0.0f;
        for (const float value : column) { norm += value * value; }
        norm = std::sqrt(norm);
        if (norm <= 1.0e-6f) {
            for (size_t row = 0; row < size; ++row) { column[row] = row == col ? 1.0f : 0.0f; }
            norm = 1.0f;
        }
        for (size_t row = 0; row < size; ++row) { q[row * size + col] = column[row] / norm; }
    }

    matrix = std::move(q);
}

std::pair<float, float> input_pan_to_gains(float input_pan) {
    const float pan = std::clamp(input_pan, -1.0f, 1.0f);
    return {std::clamp(1.0f - pan, 0.0f, 1.0f), std::clamp(1.0f + pan, 0.0f, 1.0f)};
}

}  // namespace

StereoFDN::StereoFDN(size_t delay_lines_per_channel) {
    m_scalar_smoothers.resize(NumScalarLanes);
    m_scalar_smoothers.set_ramp_samples(m_linear_smoothing_samples);
    m_scalar_smoothers.set_current_and_target(FeedbackLane, m_feedback);
    m_scalar_smoothers.set_current_and_target(DampingLane, m_damping);
    m_scalar_smoothers.set_current_and_target(CrossFeedbackLane, m_cross_feedback);
    m_scalar_smoothers.set_current_and_target(InputPanLane, m_input_pan);
    m_scalar_smoothers.set_current_and_target(WetLane, m_wet);
    m_scalar_smoothers.set_current_and_target(DryLane, m_dry);
    set_delay_lines_per_channel(delay_lines_per_channel);
}

StereoFDN::~StereoFDN() = default;

StereoFDN::StereoFDN(StereoFDN&&) noexcept = default;

StereoFDN& StereoFDN::operator=(StereoFDN&&) noexcept = default;

void StereoFDN::prepare(const double& sample_rate,
                        const size_t& /*samples_per_block*/,
                        const size_t& /*num_channels*/) {
    m_sample_rate = sample_rate > 0.0 ? sample_rate : 48000.0;
    const auto max_delay =
        static_cast<size_t>(std::lround(m_sample_rate * k_default_max_delay_seconds));
    prepare_delay_lines(std::max<size_t>(max_delay, 1));
    if (m_use_derived_delay_layout) { apply_derived_delay_layout(); }
    reset();
}

void StereoFDN::process(thl::dsp::audio::AudioBufferView buffer, uint32_t modulation_offset) {
    if (!m_prepared || buffer.get_num_channels() < k_num_audio_channels ||
        buffer.get_num_frames() == 0) {
        return;
    }

    update_smoothed_parameter_targets(modulation_offset);

    float* left = buffer.get_write_pointer(k_left);
    float* right = buffer.get_write_pointer(k_right);
    const size_t num_samples = buffer.get_num_frames();

    for (size_t sample = 0; sample < num_samples; ++sample) {
        const float in_l = left[sample];
        const float in_r = right[sample];

        read_delay_outputs();
        const float damping = m_scalar_smoothers.next(DampingLane);
        update_damped_outputs(damping);
        const float delayed_l = average_delayed_output(k_left);
        const float delayed_r = average_delayed_output(k_right);
        const float feedback = m_scalar_smoothers.next(FeedbackLane);
        const float cross_feedback = m_scalar_smoothers.next(CrossFeedbackLane);
        const float remaining = std::max(0.0f, 1.0f - cross_feedback * cross_feedback);
        const float stereo_main = std::sqrt(remaining);
        const float input_pan = m_scalar_smoothers.next(InputPanLane);
        const auto [input_l_gain, input_r_gain] = input_pan_to_gains(input_pan);
        const float wet = m_scalar_smoothers.next(WetLane);
        const float dry = m_scalar_smoothers.next(DryLane);
        const float delay_in_l = in_l * input_l_gain;
        const float delay_in_r = in_r * input_r_gain;

        for (size_t row = 0; row < m_delay_lines_per_channel; ++row) {
            float mixed_l = 0.0f;
            float mixed_r = 0.0f;
            for (size_t col = 0; col < m_delay_lines_per_channel; ++col) {
                const float matrix = m_line_mix_matrix[row * m_delay_lines_per_channel + col];
                mixed_l += matrix * m_damped_outputs[line_index(k_left, col)];
                mixed_r += matrix * m_damped_outputs[line_index(k_right, col)];
            }

            m_left_delay_inputs[row] =
                delay_in_l + feedback * (stereo_main * mixed_l + cross_feedback * mixed_r);
            m_right_delay_inputs[row] =
                delay_in_r + feedback * (-cross_feedback * mixed_l + stereo_main * mixed_r);
        }

        write_channel_delay_inputs(k_left, m_left_delay_inputs.data());
        write_channel_delay_inputs(k_right, m_right_delay_inputs.data());

        left[sample] = dry * delay_in_l + wet * delayed_l;
        right[sample] = dry * delay_in_r + wet * delayed_r;
    }

    m_has_processed = true;
}

void StereoFDN::reset() {
    clear_delay_state();
    m_delay_smoothers.snap_to_targets();
    m_scalar_smoothers.snap_to_targets();
    m_has_processed = false;
}

void StereoFDN::clear_delay_state() {
    for (auto& delay_line : m_delay_lines) { delay_line->reset(); }
    std::ranges::fill(m_delayed_outputs, 0.0f);
    std::ranges::fill(m_damped_outputs, 0.0f);
    std::ranges::fill(m_damping_states, 0.0f);
    std::ranges::fill(m_crossfade_positions, 0);
    std::ranges::fill(m_crossfade_lengths, 0);
    m_previous_delay_samples = m_delay_samples;
}

void StereoFDN::set_delay_lines_per_channel(size_t delay_lines_per_channel) {
    m_delay_lines_per_channel = std::max<size_t>(delay_lines_per_channel, 1);
    const size_t total_lines = k_num_audio_channels * m_delay_lines_per_channel;

    m_delay_lines.clear();
    m_delay_lines.reserve(total_lines);
    for (size_t i = 0; i < total_lines; ++i) {
        auto delay_line = std::make_unique<thl::dsp::utils::DynamicDelayLine>();
        if (m_prepared) { delay_line->prepare(m_max_delay_samples + 1); }
        m_delay_lines.push_back(std::move(delay_line));
    }

    m_delay_samples.assign(total_lines, k_default_delay_samples);
    m_previous_delay_samples = m_delay_samples;
    m_crossfade_positions.assign(total_lines, 0);
    m_crossfade_lengths.assign(total_lines, 0);
    m_delayed_outputs.assign(total_lines, 0.0f);
    m_damped_outputs.assign(total_lines, 0.0f);
    m_damping_states.assign(total_lines, 0.0f);
    m_line_mix_matrix.assign(m_delay_lines_per_channel * m_delay_lines_per_channel, 0.0f);
    m_left_delay_inputs.assign(m_delay_lines_per_channel, 0.0f);
    m_right_delay_inputs.assign(m_delay_lines_per_channel, 0.0f);
    m_delay_smoothers.resize(total_lines, static_cast<float>(k_default_delay_samples));
    m_delay_smoothers.set_ramp_samples(m_linear_smoothing_samples);
    update_line_mix_matrix();
    if (m_use_derived_delay_layout) {
        apply_derived_delay_layout();
    } else {
        clamp_delay_samples();
    }
    reset();
}

void StereoFDN::set_delay_samples(size_t left_delay_samples, size_t right_delay_samples) {
    m_use_derived_delay_layout = false;
    for (size_t line = 0; line < m_delay_lines_per_channel; ++line) {
        set_line_delay_sample(line_index(k_left, line), left_delay_samples);
        set_line_delay_sample(line_index(k_right, line), right_delay_samples);
    }
}

void StereoFDN::set_delay_sample(size_t channel, size_t line, size_t delay_samples) {
    if (channel >= k_num_audio_channels || line >= m_delay_lines_per_channel) { return; }

    m_use_derived_delay_layout = false;
    set_line_delay_sample(line_index(channel, line), delay_samples);
}

void StereoFDN::set_delay_ms(float left_delay_ms, float right_delay_ms) {
    set_delay_samples(ms_to_samples(left_delay_ms, m_sample_rate),
                      ms_to_samples(right_delay_ms, m_sample_rate));
}

void StereoFDN::set_base_time_ms(float base_time_ms) {
    const float clamped = std::max(base_time_ms, k_min_base_time_ms);
    if (clamped == m_base_time_ms) { return; }

    m_base_time_ms = clamped;
    m_use_derived_delay_layout = true;
    apply_derived_delay_layout();
}

void StereoFDN::set_delay_spread(float delay_spread) {
    const float clamped = std::max(delay_spread, 0.0f);
    if (clamped == m_delay_spread) { return; }

    m_delay_spread = clamped;
    m_use_derived_delay_layout = true;
    apply_derived_delay_layout();
}

void StereoFDN::set_feedback(float feedback) {
    m_feedback = std::clamp(feedback, 0.0f, 0.99f);
    set_smoothed_scalar_target(FeedbackLane, m_feedback);
}

void StereoFDN::set_crossfade_samples(size_t crossfade_samples) {
    m_crossfade_samples = crossfade_samples;
    if (m_crossfade_samples == 0) {
        std::ranges::fill(m_crossfade_positions, 0);
        std::ranges::fill(m_crossfade_lengths, 0);
        m_previous_delay_samples = m_delay_samples;
    }
}

void StereoFDN::set_linear_smoothing_samples(size_t smoothing_samples) {
    m_linear_smoothing_samples = smoothing_samples;
    m_delay_smoothers.set_ramp_samples(m_linear_smoothing_samples);
    m_scalar_smoothers.set_ramp_samples(m_linear_smoothing_samples);
}

void StereoFDN::set_damping(float damping) {
    m_damping = std::clamp(damping, 0.0f, 1.0f);
    set_smoothed_scalar_target(DampingLane, m_damping);
}

void StereoFDN::set_cross_feedback(float cross_feedback) {
    m_cross_feedback = std::clamp(cross_feedback, 0.0f, 1.0f);
    set_smoothed_scalar_target(CrossFeedbackLane, m_cross_feedback);
}

void StereoFDN::set_input_pan(float input_pan) {
    m_input_pan = std::clamp(input_pan, -1.0f, 1.0f);
    set_smoothed_scalar_target(InputPanLane, m_input_pan);
}

void StereoFDN::set_matrix_kind(MatrixKind matrix_kind) {
    if (matrix_kind == m_matrix_kind) { return; }

    if (m_prepared && m_has_processed) { clear_delay_state(); }
    m_matrix_kind = matrix_kind;
    update_line_mix_matrix();
}

void StereoFDN::set_wet(float wet) {
    m_wet = std::clamp(wet, 0.0f, 1.0f);
    set_smoothed_scalar_target(WetLane, m_wet);
}

void StereoFDN::set_dry(float dry) {
    m_dry = std::clamp(dry, 0.0f, 1.0f);
    set_smoothed_scalar_target(DryLane, m_dry);
}

void StereoFDN::prepare_delay_lines(size_t max_delay_samples) {
    m_max_delay_samples = std::max<size_t>(max_delay_samples, 1);
    for (auto& delay_line : m_delay_lines) { delay_line->prepare(m_max_delay_samples + 1); }
    m_prepared = true;
    clamp_delay_samples();
}

void StereoFDN::clamp_delay_samples() {
    for (size_t line = 0; line < m_delay_samples.size(); ++line) {
        m_delay_samples[line] = clamp_delay_sample(m_delay_samples[line]);
        m_previous_delay_samples[line] = clamp_delay_sample(m_previous_delay_samples[line]);
        m_delay_lines[line]->set_delay(m_delay_samples[line]);
        m_delay_smoothers.set_current_and_target(line, static_cast<float>(m_delay_samples[line]));
    }
}

size_t StereoFDN::clamp_delay_sample(size_t delay_samples) const {
    return std::clamp<size_t>(delay_samples, 1, m_max_delay_samples);
}

void StereoFDN::set_line_delay_sample(size_t index, size_t delay_samples) {
    if (index >= m_delay_samples.size()) { return; }

    const size_t clamped_delay = clamp_delay_sample(delay_samples);
    const size_t previous_target = m_delay_samples[index];
    if (clamped_delay == previous_target) {
        m_delay_lines[index]->set_delay(clamped_delay);
        return;
    }

    m_delay_samples[index] = clamped_delay;
    m_delay_lines[index]->set_delay(clamped_delay);
    set_smoothed_delay_target(index, clamped_delay);

    const bool use_crossfade =
        m_prepared && m_has_processed && m_crossfade_samples > 0 && m_linear_smoothing_samples == 0;
    if (use_crossfade) {
        m_previous_delay_samples[index] = previous_target;
        m_crossfade_positions[index] = 0;
        m_crossfade_lengths[index] = m_crossfade_samples;
    } else {
        m_previous_delay_samples[index] = clamped_delay;
        m_crossfade_positions[index] = 0;
        m_crossfade_lengths[index] = 0;
    }
}

void StereoFDN::apply_derived_delay_layout() {
    if (m_delay_lines_per_channel == 0) { return; }

    const size_t total_lines = k_num_audio_channels * m_delay_lines_per_channel;
    float log_sum = 0.0f;
    for (size_t channel = 0; channel < k_num_audio_channels; ++channel) {
        for (size_t line = 0; line < m_delay_lines_per_channel; ++line) {
            log_sum += std::log(base_delay_ratio(channel, line));
        }
    }

    const float geometric_mean = std::exp(log_sum / static_cast<float>(total_lines));
    for (size_t channel = 0; channel < k_num_audio_channels; ++channel) {
        for (size_t line = 0; line < m_delay_lines_per_channel; ++line) {
            const float centered = base_delay_ratio(channel, line) / geometric_mean;
            const float ratio = std::pow(centered, m_delay_spread);
            const float delay_ms = m_base_time_ms * ratio;
            set_line_delay_sample(line_index(channel, line),
                                  ms_to_samples(delay_ms, m_sample_rate));
        }
    }
}

void StereoFDN::set_smoothed_delay_target(size_t index, size_t delay_samples) {
    const auto delay = static_cast<float>(delay_samples);
    if (m_prepared && m_has_processed) {
        m_delay_smoothers.set_target(index, delay, delay_target_ramp_samples(index, delay));
    } else {
        m_delay_smoothers.set_current_and_target(index, delay);
    }
}

size_t StereoFDN::delay_target_ramp_samples(size_t index, float delay_samples) const {
    if (m_linear_smoothing_samples == 0 || index >= m_delay_smoothers.lane_count()) {
        return m_linear_smoothing_samples;
    }

    const float distance = std::abs(delay_samples - m_delay_smoothers.current(index));
    const auto slew_limited_samples =
        static_cast<size_t>(std::ceil(distance / k_max_delay_slew_samples_per_sample));
    return std::max(m_linear_smoothing_samples, slew_limited_samples);
}

void StereoFDN::set_smoothed_scalar_target(size_t lane, float value) {
    if (m_prepared && m_has_processed) {
        m_scalar_smoothers.set_target(lane, value);
    } else {
        m_scalar_smoothers.set_current_and_target(lane, value);
    }
}

void StereoFDN::update_smoothed_parameter_targets(uint32_t modulation_offset) {
    set_base_time_ms(
        std::max(get_parameter<float>(BaseTimeMs, modulation_offset), k_min_base_time_ms));
    set_delay_spread(std::max(get_parameter<float>(DelaySpread, modulation_offset), 0.0f));
    set_smoothed_scalar_target(
        FeedbackLane,
        std::clamp(get_parameter<float>(Feedback, modulation_offset), 0.0f, 0.99f));
    set_smoothed_scalar_target(
        DampingLane,
        std::clamp(get_parameter<float>(Damping, modulation_offset), 0.0f, 1.0f));
    set_smoothed_scalar_target(
        CrossFeedbackLane,
        std::clamp(get_parameter<float>(CrossFeedback, modulation_offset), 0.0f, 1.0f));
    set_smoothed_scalar_target(
        InputPanLane,
        std::clamp(get_parameter<float>(InputPan, modulation_offset), -1.0f, 1.0f));
    const int matrix_kind = std::clamp(get_parameter_int(MatrixKindParam, modulation_offset),
                                       static_cast<int>(MatrixKind::Householder),
                                       static_cast<int>(MatrixKind::RandomOrthogonal));
    set_matrix_kind(static_cast<MatrixKind>(matrix_kind));
    set_smoothed_scalar_target(
        WetLane,
        std::clamp(get_parameter<float>(Wet, modulation_offset), 0.0f, 1.0f));
    set_smoothed_scalar_target(
        DryLane,
        std::clamp(get_parameter<float>(Dry, modulation_offset), 0.0f, 1.0f));
}

void StereoFDN::read_delay_outputs() {
    for (size_t line = 0; line < m_delay_lines_per_channel; ++line) {
        const size_t left = line_index(k_left, line);
        const size_t right = line_index(k_right, line);
        m_delayed_outputs[left] = read_crossfaded_delay(left);
        m_delayed_outputs[right] = read_crossfaded_delay(right);
    }
}

void StereoFDN::update_damped_outputs(float damping) {
    if (damping <= 0.0f) {
        m_damped_outputs = m_delayed_outputs;
        m_damping_states = m_delayed_outputs;
        return;
    }

    const float coeff = damping_to_lowpass_coeff(damping, m_sample_rate);
    for (size_t line = 0; line < m_delayed_outputs.size(); ++line) {
        m_damping_states[line] += coeff * (m_delayed_outputs[line] - m_damping_states[line]);
        m_damped_outputs[line] = m_damping_states[line];
    }
}

void StereoFDN::update_line_mix_matrix() {
    const size_t size = m_delay_lines_per_channel;
    m_line_mix_matrix.assign(size * size, 0.0f);
    if (size == 0) { return; }
    if (size == 1) {
        m_line_mix_matrix[0] = 1.0f;
        return;
    }

    switch (m_matrix_kind) {
        case MatrixKind::Householder: {
            float norm = 0.0f;
            for (size_t i = 0; i < size; ++i) {
                const float value = householder_value(i, size);
                norm += value * value;
            }
            if (norm <= 0.0f) { break; }
            for (size_t row = 0; row < size; ++row) {
                const float row_value = householder_value(row, size);
                for (size_t col = 0; col < size; ++col) {
                    const float col_value = householder_value(col, size);
                    m_line_mix_matrix[row * size + col] =
                        (row == col ? 1.0f : 0.0f) - 2.0f * row_value * col_value / norm;
                }
            }
            break;
        }
        case MatrixKind::Circulant:
            for (size_t row = 0; row < size; ++row) {
                m_line_mix_matrix[row * size + ((row + size - 1) % size)] = 1.0f;
            }
            break;
        case MatrixKind::Triangular:
            for (size_t row = 0; row < size; ++row) {
                for (size_t col = 0; col <= row; ++col) {
                    const float sign = (row + col) % 2 == 0 ? 1.0f : -1.0f;
                    m_line_mix_matrix[row * size + col] =
                        sign / (1.0f + static_cast<float>(row - col));
                }
            }
            normalize_by_power_iteration(m_line_mix_matrix, size);
            break;
        case MatrixKind::Hadamard: {
            size_t offset = 0;
            size_t remaining = size;
            while (remaining > 0) {
                const size_t block_size =
                    remaining >= 2 ? largest_power_of_two_at_most(remaining) : 1;
                for (size_t row = 0; row < block_size; ++row) {
                    for (size_t col = 0; col < block_size; ++col) {
                        m_line_mix_matrix[(offset + row) * size + offset + col] =
                            hadamard_value(row, col, block_size);
                    }
                }
                offset += block_size;
                remaining -= block_size;
            }
            break;
        }
        case MatrixKind::Anderson: {
            size_t offset = 0;
            size_t remaining = size;
            while (remaining >= 4) {
                for (size_t row = 0; row < 4; ++row) {
                    for (size_t col = 0; col < 4; ++col) {
                        m_line_mix_matrix[(offset + row) * size + offset + col] =
                            k_anderson_core[row * 4 + col];
                    }
                }
                offset += 4;
                remaining -= 4;
            }
            for (size_t i = 0; i < remaining; ++i) {
                m_line_mix_matrix[(offset + i) * size + offset + i] = 1.0f;
            }
            break;
        }
        case MatrixKind::Diagonal:
            for (size_t i = 0; i < size; ++i) { m_line_mix_matrix[i * size + i] = 1.0f; }
            break;
        case MatrixKind::RandomOrthogonal: make_random_orthogonal(m_line_mix_matrix, size); break;
    }
}

float StereoFDN::read_crossfaded_delay(size_t line) {
    const float target_delay = m_delay_smoothers.next(line);
    const float target = read_delay(line, target_delay);
    const size_t length = m_crossfade_lengths[line];
    const size_t position = m_crossfade_positions[line];

    if (length == 0 || position >= length) {
        m_previous_delay_samples[line] = m_delay_samples[line];
        m_crossfade_positions[line] = 0;
        m_crossfade_lengths[line] = 0;
        return target;
    }

    const float previous = read_delay(line, m_previous_delay_samples[line]);
    const float t = static_cast<float>(position) / static_cast<float>(length);
    const float output = previous + (target - previous) * t;

    const size_t next_position = position + 1;
    if (next_position >= length) {
        m_previous_delay_samples[line] = m_delay_samples[line];
        m_crossfade_positions[line] = 0;
        m_crossfade_lengths[line] = 0;
    } else {
        m_crossfade_positions[line] = next_position;
    }

    return output;
}

float StereoFDN::read_delay(size_t channel, size_t delay_samples) const {
    return m_delay_lines[channel]->read(delay_samples);
}

float StereoFDN::read_delay(size_t channel, float delay_samples) const {
    return m_delay_lines[channel]->read(delay_samples);
}

void StereoFDN::write_delay(size_t channel, float sample) {
    m_delay_lines[channel]->write(sample);
}

void StereoFDN::write_channel_delay_inputs(size_t channel, const float* delay_inputs) {
    for (size_t line = 0; line < m_delay_lines_per_channel; ++line) {
        write_delay(line_index(channel, line), delay_inputs[line]);
    }
}

float StereoFDN::average_delayed_output(size_t channel) const {
    float sum = 0.0f;
    for (size_t line = 0; line < m_delay_lines_per_channel; ++line) {
        sum += m_delayed_outputs[line_index(channel, line)];
    }
    return sum / static_cast<float>(m_delay_lines_per_channel);
}

float StereoFDN::average_damped_output(size_t channel) const {
    float sum = 0.0f;
    for (size_t line = 0; line < m_delay_lines_per_channel; ++line) {
        sum += m_damped_outputs[line_index(channel, line)];
    }
    return sum / static_cast<float>(m_delay_lines_per_channel);
}

size_t StereoFDN::line_index(size_t channel, size_t line) const {
    return channel * m_delay_lines_per_channel + line;
}

size_t StereoFDN::delay_samples(size_t channel, size_t line) const {
    if (channel >= k_num_audio_channels || line >= m_delay_lines_per_channel) { return 0; }
    return m_delay_samples[line_index(channel, line)];
}

float StereoFDN::get_parameter_float(Parameter p, uint32_t /*modulation_offset*/) {
    switch (p) {
        case BaseTimeMs: return m_base_time_ms;
        case DelaySpread: return m_delay_spread;
        case Feedback: return m_feedback;
        case Damping: return m_damping;
        case CrossFeedback: return m_cross_feedback;
        case InputPan: return m_input_pan;
        case Wet: return m_wet;
        case Dry: return m_dry;
        case MatrixKindParam:
        case NumParameters: break;
    }
    return 0.0f;
}

int StereoFDN::get_parameter_int(Parameter p, uint32_t /*modulation_offset*/) {
    if (p == MatrixKindParam) { return static_cast<int>(m_matrix_kind); }
    return 0;
}

}  // namespace thl::dsp::fx
