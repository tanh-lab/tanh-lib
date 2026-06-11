#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/utils/DynamicDelayLine.h>
#include <tanh/dsp/utils/LinearSmootherBank.h>

namespace thl::dsp::fx {

/**
 * Minimal stereo feedback delay network.
 *
 * This first version defaults to one delay line for the left channel and one
 * delay line for the right channel, but the storage is parameterized by an
 * equal number of delay lines per side:
 *
 *   s[n] = delayed x[n]
 *   x[n] = u[n] + feedback * A * s[n]
 *   y[n] = dry * u[n] + wet * s[n]
 *
 * where A is currently a 2x2 stereo rotation over the per-channel line mix.
 * Later versions can expand A into the full per-line block matrix.
 */
class TANH_API StereoFDN : public thl::dsp::BaseProcessor {
public:
    enum class MatrixKind {
        Householder = 0,
        Circulant,
        Triangular,
        Hadamard,
        Anderson,
        Diagonal,
        RandomOrthogonal,
    };

    static constexpr size_t k_left = 0;
    static constexpr size_t k_right = 1;
    static constexpr size_t k_num_audio_channels = 2;
    static constexpr size_t k_num_channels = k_num_audio_channels;

    explicit StereoFDN(size_t delay_lines_per_channel = 1);
    ~StereoFDN() override;

    StereoFDN(const StereoFDN&) = delete;
    StereoFDN& operator=(const StereoFDN&) = delete;
    StereoFDN(StereoFDN&&) noexcept;
    StereoFDN& operator=(StereoFDN&&) noexcept;

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;
    void process(thl::dsp::audio::AudioBufferView buffer, uint32_t modulation_offset = 0) override;

    void reset();

    // Reallocates delay storage and clears state. Configure outside audio processing.
    void set_delay_lines_per_channel(size_t delay_lines_per_channel);
    void set_delay_samples(size_t left_delay_samples, size_t right_delay_samples);
    void set_delay_sample(size_t channel, size_t line, size_t delay_samples);
    void set_delay_ms(float left_delay_ms, float right_delay_ms);
    void set_crossfade_samples(size_t crossfade_samples);
    void set_linear_smoothing_samples(size_t smoothing_samples);
    void set_base_time_ms(float base_time_ms);
    void set_delay_spread(float delay_spread);
    void set_feedback(float feedback);
    void set_damping(float damping);
    void set_cross_feedback(float cross_feedback);
    void set_input_pan(float input_pan);
    void set_matrix_kind(MatrixKind matrix_kind);
    void set_wet(float wet);
    void set_dry(float dry);

    size_t delay_lines_per_channel() const { return m_delay_lines_per_channel; }
    size_t total_delay_lines() const { return m_delay_lines.size(); }
    size_t left_delay_samples() const { return delay_samples(k_left, 0); }
    size_t right_delay_samples() const { return delay_samples(k_right, 0); }
    size_t delay_samples(size_t channel, size_t line) const;
    size_t crossfade_samples() const { return m_crossfade_samples; }
    size_t linear_smoothing_samples() const { return m_linear_smoothing_samples; }
    float base_time_ms() const { return m_base_time_ms; }
    float delay_spread() const { return m_delay_spread; }
    float feedback() const { return m_feedback; }
    float damping() const { return m_damping; }
    float cross_feedback() const { return m_cross_feedback; }
    float input_pan() const { return m_input_pan; }
    MatrixKind matrix_kind() const { return m_matrix_kind; }
    float wet() const { return m_wet; }
    float dry() const { return m_dry; }

protected:
    enum Parameter {
        BaseTimeMs = 0,
        DelaySpread,
        Feedback,
        Damping,
        CrossFeedback,
        InputPan,
        MatrixKindParam,
        Wet,
        Dry,
        NumParameters
    };

    template <typename T>
    T get_parameter(Parameter p, uint32_t modulation_offset = 0);

    virtual float get_parameter_float(Parameter p, uint32_t modulation_offset = 0);
    virtual int get_parameter_int(Parameter p, uint32_t modulation_offset = 0);

private:
    void prepare_delay_lines(size_t max_delay_samples);
    void clear_delay_state();
    void clamp_delay_samples();
    size_t clamp_delay_sample(size_t delay_samples) const;
    void set_line_delay_sample(size_t index, size_t delay_samples);
    void apply_derived_delay_layout();
    void set_smoothed_delay_target(size_t index, size_t delay_samples);
    size_t delay_target_ramp_samples(size_t index, float delay_samples) const;
    void set_smoothed_scalar_target(size_t lane, float value);
    void update_smoothed_parameter_targets(uint32_t modulation_offset);
    void read_delay_outputs();
    void update_damped_outputs(float damping);
    void update_line_mix_matrix();
    float read_crossfaded_delay(size_t line);
    float read_delay(size_t channel, size_t delay_samples) const;
    float read_delay(size_t channel, float delay_samples) const;
    void write_delay(size_t channel, float sample);
    void write_channel_delay_inputs(size_t channel, const float* delay_inputs);
    float average_delayed_output(size_t channel) const;
    float average_damped_output(size_t channel) const;
    size_t line_index(size_t channel, size_t line) const;

    enum ScalarSmootherLane : size_t {
        FeedbackLane = 0,
        DampingLane,
        CrossFeedbackLane,
        InputPanLane,
        WetLane,
        DryLane,
        NumScalarLanes
    };

    size_t m_delay_lines_per_channel = 1;
    double m_sample_rate = 48000.0;
    size_t m_max_delay_samples = 48000;
    size_t m_crossfade_samples = 64;
    size_t m_linear_smoothing_samples = 64;
    bool m_prepared = false;
    bool m_has_processed = false;
    bool m_use_derived_delay_layout = true;

    std::vector<std::unique_ptr<thl::dsp::utils::DynamicDelayLine>> m_delay_lines;
    std::vector<size_t> m_delay_samples;
    std::vector<size_t> m_previous_delay_samples;
    std::vector<size_t> m_crossfade_positions;
    std::vector<size_t> m_crossfade_lengths;
    std::vector<float> m_delayed_outputs;
    std::vector<float> m_damped_outputs;
    std::vector<float> m_damping_states;
    std::vector<float> m_line_mix_matrix;
    std::vector<float> m_left_delay_inputs;
    std::vector<float> m_right_delay_inputs;
    thl::dsp::utils::LinearSmootherBank m_delay_smoothers;
    thl::dsp::utils::LinearSmootherBank m_scalar_smoothers;

    float m_base_time_ms = 500.0f;
    float m_delay_spread = 0.0f;
    float m_feedback = 0.65f;
    float m_damping = 0.0f;
    float m_cross_feedback = 0.0f;
    float m_input_pan = 0.0f;
    MatrixKind m_matrix_kind = MatrixKind::Diagonal;
    float m_wet = 1.0f;
    float m_dry = 0.0f;
};

template <>
inline float StereoFDN::get_parameter<float>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_float(p, modulation_offset);
}

}  // namespace thl::dsp::fx
