#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/utils/Mixer.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::dsp::utils {

namespace {

// The p-norm of @p values (none negative), given 1/p; 1/p = 0: the largest value. The values are
// divided by the largest first, so a large p cannot overflow.
float norm(std::span<const float> values, float inverse_p) TANH_NONBLOCKING_FUNCTION {
    if (values.empty()) { return 0.0f; }
    const float largest = *std::ranges::max_element(values);
    if (inverse_p == 0.0f || largest == 0.0f) { return largest; }
    float sum = 0.0f;
    if (inverse_p == 1.0f) {
        for (const float value : values) { sum += value; }
        return sum;
    }
    const float p = 1.0f / inverse_p;
    for (const float value : values) { sum += std::pow(value / largest, p); }
    return largest * std::pow(sum, inverse_p);
}

}  // namespace

void Mixer::prepare(double sample_rate, size_t num_inputs, double ramp_seconds) {
    m_target_weights.resize(num_inputs, 1.0f);
    m_weights.resize(num_inputs);
    m_gains.resize(num_inputs);
    for (SmoothedValue& weight : m_weights) { weight.reset(sample_rate, ramp_seconds); }
    m_inverse_p.reset(sample_rate, ramp_seconds);
    m_mix_amount.reset(sample_rate, ramp_seconds);
    reset();
}

void Mixer::set_mode(Mode mode) TANH_NONBLOCKING_FUNCTION {
    m_mode = mode;
    m_mix_amount.set_target_value(mode == Mode::Mix ? 1.0f : 0.0f);
}

void Mixer::set_p(float p) TANH_NONBLOCKING_FUNCTION {
    if (std::isnan(p)) { return; }
    m_p = std::max(p, k_amplitude);
    m_inverse_p.set_target_value(1.0f / m_p);
}

void Mixer::set_weight(size_t input, float weight) TANH_NONBLOCKING_FUNCTION {
    assert(input < num_inputs() && "Mixer: no such input");
    if (!std::isfinite(weight)) { return; }
    m_target_weights[input] = std::max(weight, 0.0f);
    m_weights[input].set_target_value(m_target_weights[input]);
}

void Mixer::set_weights(std::span<const float> weights) TANH_NONBLOCKING_FUNCTION {
    assert(weights.size() <= num_inputs() && "Mixer: more weights than inputs");
    for (size_t input = 0; input < weights.size(); ++input) { set_weight(input, weights[input]); }
}

void Mixer::reset() TANH_NONBLOCKING_FUNCTION {
    for (size_t input = 0; input < num_inputs(); ++input) {
        m_weights[input].set_current_and_target_value(m_target_weights[input]);
    }
    m_inverse_p.set_current_and_target_value(1.0f / m_p);
    m_mix_amount.set_current_and_target_value(m_mode == Mode::Mix ? 1.0f : 0.0f);
}

bool Mixer::passes_only(size_t input) const TANH_NONBLOCKING_FUNCTION {
    if (input >= num_inputs() || is_gliding()) { return false; }
    for (size_t other = 0; other < num_inputs(); ++other) {
        if (other != input && m_target_weights[other] != 0.0f) { return false; }
    }
    const float weight = m_target_weights[input];
    return m_mode == Mode::Sum ? weight == 1.0f : weight > 0.0f;
}

void Mixer::process(std::span<const thl::core::ConstBufferView> inputs,
                    thl::core::BufferView output) TANH_NONBLOCKING_FUNCTION {
    assert(inputs.size() == num_inputs() && "Mixer: needs one input per weight");
    assert(std::ranges::all_of(inputs,
                               [&](const thl::core::ConstBufferView& input) {
                                   return input.get_num_channels() == output.get_num_channels() &&
                                          input.get_num_samples() == output.get_num_samples();
                               }) &&
           "Mixer: inputs and output have different dimensions");

    // Reads every input at sample i before writing it, so the output may be one of the inputs.
    const auto mix_sample = [&](size_t channel, size_t i) {
        float sum = 0.0f;
        for (size_t input = 0; input < inputs.size(); ++input) {
            sum += m_gains[input] * inputs[input].get_read_pointer(channel)[i];
        }
        output.get_write_pointer(channel)[i] = sum;
    };

    const size_t num_channels = output.get_num_channels();
    const size_t num_samples = output.get_num_samples();
    if (!is_gliding()) {
        update_gains();
        for (size_t channel = 0; channel < num_channels; ++channel) {
            for (size_t i = 0; i < num_samples; ++i) { mix_sample(channel, i); }
        }
        return;
    }
    for (size_t i = 0; i < num_samples; ++i) {
        update_gains();
        for (size_t channel = 0; channel < num_channels; ++channel) { mix_sample(channel, i); }
    }
}

bool Mixer::is_gliding() const TANH_NONBLOCKING_FUNCTION {
    return m_inverse_p.is_smoothing() || m_mix_amount.is_smoothing() ||
           std::ranges::any_of(m_weights,
                               [](const SmoothedValue& weight) { return weight.is_smoothing(); });
}

void Mixer::update_gains() TANH_NONBLOCKING_FUNCTION {
    for (size_t input = 0; input < num_inputs(); ++input) {
        m_gains[input] = m_weights[input].get_smoothed_value();
    }
    const float inverse_p = m_inverse_p.get_smoothed_value();
    const float mix_amount = m_mix_amount.get_smoothed_value();
    if (mix_amount == 0.0f) { return; }

    // w / norm, not w * (1 / norm): a single input then gets exactly 1.
    const float weights_norm = norm(m_gains, inverse_p);
    for (float& gain : m_gains) {
        const float normalised = weights_norm > 0.0f ? gain / weights_norm : 0.0f;
        gain = ((1.0f - mix_amount) * gain) + (mix_amount * normalised);
    }
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::utils
