// Copyright 2015 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Comb filter / KS string.

#pragma once

#include <algorithm>
#include <array>

#include <tanh/core/Exports.h>
#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/filter/DCBlocker.h>
#include <tanh/dsp/utils/Allpass.h>
#include <tanh/dsp/utils/DelayLine.h>
#include <tanh/dsp/filter/Svf.h>

#include <tanh/dsp/rings-resonator/RingsDsp.h>

namespace thl::dsp::resonator {

const size_t k_delay_line_size = 4096;

// Three-tap FIR filter used in the KS feedback loop to model frequency-
// dependent energy loss. The two coefficients, brightness and damping
// are linearly interpolated over each processing block to avoid zipper noise.
//
// Transfer function (z-domain):
//   H(z) = damping * [ h0 + h1 * z^-1 + h1 * z^-2 ]
// where h0 = (1 + brightness) / 2,  h1 = (1 - brightness) / 4.
//
// At full brightness the filter is a pure gain (all-pass shape); as
// brightness decreases the high-frequency content is increasingly attenuated,
// producing a warmer, darker decay.
class RingsDampingFilter {
public:
    RingsDampingFilter() = default;
    ~RingsDampingFilter() = default;

    void prepare() {
        m_x = 0.0f;
        m_x_prev = 0.0f;
        m_brightness = 0.0f;
        m_brightness_increment = 0.0f;
        m_damping = 0.0f;
        m_damping_increment = 0.0f;
    }

    void reset() {
        m_x = 0.0f;
        m_x_prev = 0.0f;
    }

    RingsDampingFilter(const RingsDampingFilter&) = delete;
    RingsDampingFilter& operator=(const RingsDampingFilter&) = delete;

    inline void configure(float damping, float brightness, size_t size) {
        if (!size) {
            m_damping = damping;
            m_brightness = brightness;
            m_damping_increment = 0.0f;
            m_brightness_increment = 0.0f;
        } else {
            const float step = 1.0f / static_cast<float>(size);
            m_damping_increment = (damping - m_damping) * step;
            m_brightness_increment = (brightness - m_brightness) * step;
        }
    }

    inline float process(float x) {
        const float h0 = (1.0f + m_brightness) * 0.5f;
        const float h1 = (1.0f - m_brightness) * 0.25f;
        const float y = m_damping * (h0 * m_x + h1 * (x + m_x_prev));
        m_x_prev = m_x;
        m_x = x;
        m_brightness += m_brightness_increment;
        m_damping += m_damping_increment;
        return y;
    }

private:
    float m_x = 0.0f;
    float m_x_prev = 0.0f;
    float m_brightness = 0.0f;
    float m_brightness_increment = 0.0f;
    float m_damping = 0.0f;
    float m_damping_increment = 0.0f;
};

using StringDelayLine = thl::dsp::utils::DelayLine<float, k_delay_line_size>;
using StiffnessAllpass = thl::dsp::utils::Allpass<float, k_delay_line_size / 2>;

// Extended Karplus-Strong string model.
//
// Core topology: a delay line (m_string) with a damping filter chain in the
// feedback path the classic KS comb-filter structure. Several extensions
// bring the model closer to physical string behaviour:
//
//   FIR + IIR damping
//     The feedback signal passes through a three-tap FIR averaging filter
//     (DampingFilter) followed by a TPT SVF in low-pass mode. The FIR sets
//     the broad decay envelope while the IIR gives finer spectral shaping,
//     especially at high pitches where the FIR alone lacks resolution.
//
//   Dispersion (allpass stiffness)
//     An optional allpass delay section (m_stretch) shifts upper partials
//     away from exact harmonics, modelling the inharmonicity of stiff
//     strings (piano wire, metallic bars). Controlled by the dispersion
//     parameter; disabled entirely for sympathetic-string modes.
//
//   DC blocking
//     A first-order DC blocker in the feedback loop prevents the recirculating
//     signal from accumulating a DC offset.
//
//   Curved-bridge nonlinearity
//     A soft nonlinear wrap applied to the delay output simulates the
//     buzzing / rattling caused by a curved bridge termination (e.g. sitar,
//     tanpura). Activated by negative dispersion values.
//
//   Sample rate halving
//     When the required delay exceeds the buffer length (very low pitches)
//     the model transparently switches to half-rate processing, effectively
//     doubling the available delay range. Linear crossfade between successive
//     output samples hides the decimation.
//
//   Hermite interpolation
//     Fractional-sample delay reads use four-point Hermite interpolation for
//     clean pitch accuracy across the full frequency range.
class TANH_API RingsString {
public:
    RingsString() = default;
    ~RingsString() = default;

    // Initialise the string model.  `enable_dispersion` gates the allpass
    // stiffness path (and curved-bridge nonlinearity), when false, the
    // compiler eliminates that code entirely.
    void prepare(bool enable_dispersion, float sample_rate = k_default_sample_rate);

    // Process a block of `size` samples.  Excitation is read from `in`;
    // the primary string output is accumulated into `out` and a comb-
    // filtered pickup signal (position-dependent tap) into `aux`.
    void process(const thl::dsp::audio::ConstAudioBufferView& in,
                 const thl::dsp::audio::AudioBufferView& out,
                 const thl::dsp::audio::AudioBufferView& aux);

    inline void set_frequency(float frequency) { m_frequency = frequency; }

    inline void set_frequency(float frequency, float coefficient) {
        m_frequency += coefficient * (frequency - m_frequency);
    }

    inline void set_dispersion(float dispersion) { m_dispersion = dispersion; }

    inline void set_brightness(float brightness) { m_brightness = brightness; }

    inline void set_damping(float damping) { m_damping = damping; }

    inline void set_position(float position) { m_position = position; }

    inline StringDelayLine* mutable_string() { return &m_string; }

    RingsString(const RingsString&) = delete;
    RingsString& operator=(const RingsString&) = delete;

private:
    // Compute per-block filter coefficients from the current parameter
    // snapshot.  `delay` is the fractional delay in samples, `src_ratio`
    // is the SRC ratio (1.0 at native rate, <1.0 at half rate), and `size`
    // is the block length for parameter interpolation.
    void prepare_coefficients(float delay, float src_ratio, size_t size);

    // Core per-block loop.  The template parameter gates the dispersion
    // path so the compiler can eliminate allpass / curved-bridge logic
    // when it is not needed.
    template <bool enable_dispersion>
    void process_internal(const thl::dsp::audio::ConstAudioBufferView& in,
                          thl::dsp::audio::AudioBufferView out,
                          thl::dsp::audio::AudioBufferView aux);

    float m_sample_rate = k_default_sample_rate;
    float m_frequency = 0.0f;
    float m_dispersion = 0.0f;
    float m_brightness = 0.0f;
    float m_damping = 0.0f;
    float m_position = 0.0f;

    float m_delay = 0.0f;
    float m_clamped_position = 0.0f;
    float m_previous_dispersion = 0.0f;
    float m_previous_damping_compensation = 0.0f;

    bool m_enable_dispersion = false;
    bool m_enable_iir_damping = false;
    float m_dispersion_noise = 0.0f;

    float m_src_phase = 0.0f;
    std::array<float, 2> m_out_sample = {};
    std::array<float, 2> m_aux_sample = {};

    float m_curved_bridge = 0.0f;
    float m_noise_filter = 0.0f;
    float m_damping_compensation_target = 0.0f;

    StringDelayLine m_string;
    StiffnessAllpass m_stretch;

    RingsDampingFilter m_fir_damping_filter;
    thl::dsp::filter::Svf m_iir_damping_filter;
    thl::dsp::filter::DCBlocker m_dc_blocker;
};

}  // namespace thl::dsp::resonator
