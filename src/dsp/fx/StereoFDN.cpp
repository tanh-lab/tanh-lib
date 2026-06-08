#include <tanh/dsp/fx/StereoFDN.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace thl::dsp::fx {

namespace {

constexpr double k_default_max_delay_seconds = 2.0;
constexpr size_t k_default_delay_samples = 2400;

size_t ms_to_samples(float delay_ms, double sample_rate) {
    const float clamped_ms = std::max(delay_ms, 0.0f);
    const double samples = static_cast<double>(clamped_ms) * sample_rate / 1000.0;
    return static_cast<size_t>(std::lround(samples));
}

std::pair<float, float> input_pan_to_gains(float input_pan) {
    const float pan = std::clamp(input_pan, -1.0f, 1.0f);
    return {std::clamp(1.0f - pan, 0.0f, 1.0f),
            std::clamp(1.0f + pan, 0.0f, 1.0f)};
}

}  // namespace

StereoFDN::StereoFDN(size_t delay_lines_per_channel) {
    m_scalar_smoothers.resize(k_num_scalar_lanes);
    m_scalar_smoothers.set_ramp_samples(m_linear_smoothing_samples);
    m_scalar_smoothers.set_current_and_target(k_feedback_lane, m_feedback);
    m_scalar_smoothers.set_current_and_target(k_cross_feedback_lane, m_cross_feedback);
    m_scalar_smoothers.set_current_and_target(k_input_pan_lane, m_input_pan);
    m_scalar_smoothers.set_current_and_target(k_wet_lane, m_wet);
    m_scalar_smoothers.set_current_and_target(k_dry_lane, m_dry);
    set_delay_lines_per_channel(delay_lines_per_channel);
}

StereoFDN::~StereoFDN() = default;

void StereoFDN::prepare(const double& sample_rate,
                        const size_t& /*samples_per_block*/,
                        const size_t& /*num_channels*/) {
    m_sample_rate = sample_rate > 0.0 ? sample_rate : 48000.0;
    const auto max_delay =
        static_cast<size_t>(std::lround(m_sample_rate * k_default_max_delay_seconds));
    prepare_delay_lines(std::max<size_t>(max_delay, 1));
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
        const float delayed_l = average_delayed_output(k_left);
        const float delayed_r = average_delayed_output(k_right);
        const float feedback = m_scalar_smoothers.next(k_feedback_lane);
        const float cross_feedback = m_scalar_smoothers.next(k_cross_feedback_lane);
        const float remaining = std::max(0.0f, 1.0f - cross_feedback * cross_feedback);
        const float stereo_main = std::sqrt(remaining);
        const float input_pan = m_scalar_smoothers.next(k_input_pan_lane);
        const auto [input_l_gain, input_r_gain] = input_pan_to_gains(input_pan);
        const float wet = m_scalar_smoothers.next(k_wet_lane);
        const float dry = m_scalar_smoothers.next(k_dry_lane);
        const float delay_in_l = in_l * input_l_gain;
        const float delay_in_r = in_r * input_r_gain;

        const float feedback_l =
            feedback * (stereo_main * delayed_l + cross_feedback * delayed_r);
        const float feedback_r =
            feedback * (-cross_feedback * delayed_l + stereo_main * delayed_r);

        write_channel_delay_inputs(k_left, delay_in_l + feedback_l);
        write_channel_delay_inputs(k_right, delay_in_r + feedback_r);

        left[sample] = dry * delay_in_l + wet * delayed_l;
        right[sample] = dry * delay_in_r + wet * delayed_r;
    }

    m_has_processed = true;
}

void StereoFDN::reset() {
    for (auto& delay_line : m_delay_lines) { delay_line->reset(); }
    std::fill(m_delayed_outputs.begin(), m_delayed_outputs.end(), 0.0f);
    std::fill(m_crossfade_positions.begin(), m_crossfade_positions.end(), 0);
    std::fill(m_crossfade_lengths.begin(), m_crossfade_lengths.end(), 0);
    m_previous_delay_samples = m_delay_samples;
    m_delay_smoothers.snap_to_targets();
    m_scalar_smoothers.snap_to_targets();
    m_has_processed = false;
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
    m_delay_smoothers.resize(total_lines, static_cast<float>(k_default_delay_samples));
    m_delay_smoothers.set_ramp_samples(m_linear_smoothing_samples);
    clamp_delay_samples();
    reset();
}

void StereoFDN::set_delay_samples(size_t left_delay_samples, size_t right_delay_samples) {
    for (size_t line = 0; line < m_delay_lines_per_channel; ++line) {
        set_line_delay_sample(line_index(k_left, line), left_delay_samples);
        set_line_delay_sample(line_index(k_right, line), right_delay_samples);
    }
}

void StereoFDN::set_delay_sample(size_t channel, size_t line, size_t delay_samples) {
    if (channel >= k_num_audio_channels || line >= m_delay_lines_per_channel) { return; }

    set_line_delay_sample(line_index(channel, line), delay_samples);
}

void StereoFDN::set_delay_ms(float left_delay_ms, float right_delay_ms) {
    set_delay_samples(ms_to_samples(left_delay_ms, m_sample_rate),
                      ms_to_samples(right_delay_ms, m_sample_rate));
}

void StereoFDN::set_feedback(float feedback) {
    m_feedback = std::clamp(feedback, 0.0f, 0.99f);
    set_smoothed_scalar_target(k_feedback_lane, m_feedback);
}

void StereoFDN::set_crossfade_samples(size_t crossfade_samples) {
    m_crossfade_samples = crossfade_samples;
    if (m_crossfade_samples == 0) {
        std::fill(m_crossfade_positions.begin(), m_crossfade_positions.end(), 0);
        std::fill(m_crossfade_lengths.begin(), m_crossfade_lengths.end(), 0);
        m_previous_delay_samples = m_delay_samples;
    }
}

void StereoFDN::set_linear_smoothing_samples(size_t smoothing_samples) {
    m_linear_smoothing_samples = smoothing_samples;
    m_delay_smoothers.set_ramp_samples(m_linear_smoothing_samples);
    m_scalar_smoothers.set_ramp_samples(m_linear_smoothing_samples);
}

void StereoFDN::set_cross_feedback(float cross_feedback) {
    m_cross_feedback = std::clamp(cross_feedback, 0.0f, 1.0f);
    set_smoothed_scalar_target(k_cross_feedback_lane, m_cross_feedback);
}

void StereoFDN::set_input_pan(float input_pan) {
    m_input_pan = std::clamp(input_pan, -1.0f, 1.0f);
    set_smoothed_scalar_target(k_input_pan_lane, m_input_pan);
}

void StereoFDN::set_wet(float wet) {
    m_wet = std::clamp(wet, 0.0f, 1.0f);
    set_smoothed_scalar_target(k_wet_lane, m_wet);
}

void StereoFDN::set_dry(float dry) {
    m_dry = std::clamp(dry, 0.0f, 1.0f);
    set_smoothed_scalar_target(k_dry_lane, m_dry);
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
        m_delay_smoothers.set_current_and_target(line,
                                                 static_cast<float>(m_delay_samples[line]));
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

    if (m_prepared && m_has_processed && m_crossfade_samples > 0) {
        m_previous_delay_samples[index] = previous_target;
        m_crossfade_positions[index] = 0;
        m_crossfade_lengths[index] = m_crossfade_samples;
    } else {
        m_previous_delay_samples[index] = clamped_delay;
        m_crossfade_positions[index] = 0;
        m_crossfade_lengths[index] = 0;
    }
}

void StereoFDN::set_smoothed_delay_target(size_t index, size_t delay_samples) {
    const float delay = static_cast<float>(delay_samples);
    if (m_prepared && m_has_processed) {
        m_delay_smoothers.set_target(index, delay);
    } else {
        m_delay_smoothers.set_current_and_target(index, delay);
    }
}

void StereoFDN::set_smoothed_scalar_target(size_t lane, float value) {
    if (m_prepared && m_has_processed) {
        m_scalar_smoothers.set_target(lane, value);
    } else {
        m_scalar_smoothers.set_current_and_target(lane, value);
    }
}

void StereoFDN::update_smoothed_parameter_targets(uint32_t modulation_offset) {
    set_smoothed_scalar_target(
        k_feedback_lane,
        std::clamp(get_parameter<float>(Feedback, modulation_offset), 0.0f, 0.99f));
    set_smoothed_scalar_target(
        k_cross_feedback_lane,
        std::clamp(get_parameter<float>(CrossFeedback, modulation_offset), 0.0f, 1.0f));
    set_smoothed_scalar_target(
        k_input_pan_lane,
        std::clamp(get_parameter<float>(InputPan, modulation_offset), -1.0f, 1.0f));
    set_smoothed_scalar_target(k_wet_lane,
                               std::clamp(get_parameter<float>(Wet, modulation_offset), 0.0f, 1.0f));
    set_smoothed_scalar_target(k_dry_lane,
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

void StereoFDN::write_channel_delay_inputs(size_t channel, float delay_input) {
    for (size_t line = 0; line < m_delay_lines_per_channel; ++line) {
        write_delay(line_index(channel, line), delay_input);
    }
}

float StereoFDN::average_delayed_output(size_t channel) const {
    float sum = 0.0f;
    for (size_t line = 0; line < m_delay_lines_per_channel; ++line) {
        sum += m_delayed_outputs[line_index(channel, line)];
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
        case Feedback:
            return m_feedback;
        case CrossFeedback:
            return m_cross_feedback;
        case InputPan:
            return m_input_pan;
        case Wet:
            return m_wet;
        case Dry:
            return m_dry;
        case NumParameters:
            break;
    }
    return 0.0f;
}

}  // namespace thl::dsp::fx
