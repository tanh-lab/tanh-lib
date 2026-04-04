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
// Polyblep oscillator used for string synth synthesis.

#pragma once

#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/ParameterInterpolator.h>
#include <tanh/dsp/utils/DspMath.h>

namespace thl::dsp::synth {

using namespace thl::dsp::utils;
using ::thl::dsp::utils::ParameterInterpolator;

// Waveshape selection for the PolyBLEP oscillator.  Each shape applies a
// different post-processing stage to the raw band-limited square:
//   BRIGHT_SQUARE -- high-pass filtered square (emphasises edges)
//   SQUARE        -- plain band-limited square (no post-filter)
//   DARK_SQUARE   -- low-pass filtered square (leaky integrator)
//   TRIANGLE      -- integrated square (leaky integrator with tighter coeff)
enum OscillatorShape {
    BrightSquare,
    Square,
    DarkSquare,
    Triangle,
};

// Band-limited oscillator using the PolyBLEP (Polynomial Band-Limited stEP)
// anti-aliasing technique.
//
// A naive phase-ramp oscillator produces hard discontinuities at transition
// points, which alias badly.  PolyBLEP corrects this by subtracting a
// polynomial residual (quadratic here) from the two samples straddling each
// discontinuity, analytically cancelling the first-order aliasing.
//
// The oscillator generates two parallel signals per sample:
//   - a shaped waveform (square / triangle / bright / dark, selected by the
//     OscillatorShape template parameter), and
//   - a raw sawtooth.
// Both are mixed together with independently interpolated gains and
// accumulated into the output buffer, so multiple instances can be summed
// for the string synth's chord voicing.
//
// Frequencies above f_s * 0.17 (~8 kHz at 48 kHz) are progressively
// attenuated, and the oscillator is silenced entirely above f_s * 0.25
// (~12 kHz) to avoid audible aliasing at the top of the range.
class StringSynthOscillator {
public:
    StringSynthOscillator() {}
    ~StringSynthOscillator() {}

    inline void prepare() {
        m_phase = 0.0f;
        m_phase_increment = 0.01f;
        m_filter_state = 0.0f;
        m_high = false;

        m_next_sample = 0.0f;
        m_next_sample_saw = 0.0f;

        m_gain = 0.0f;
        m_gain_saw = 0.0f;
    }

    // Render `size` samples, accumulating into `out`.  `target_increment` is
    // the normalised frequency (f / f_s).  The shaped waveform is scaled by
    // `target_gain` and the raw sawtooth by `target_gain_saw`; both are
    // smoothly interpolated from the previous block's values.  When
    // `interpolate_pitch` is true, the phase increment is also interpolated
    // per sample for glide effects.
    template <OscillatorShape shape, bool interpolate_pitch>
    inline void render(float target_increment,
                       float target_gain,
                       float target_gain_saw,
                       float* out,
                       size_t size) {
        // Cut harmonics above 12kHz, and low-pass harmonics above 8kHz to clear
        // highs
        if (target_increment >= 0.17f) {
            target_gain *= 1.0f - (target_increment - 0.17f) * 12.5f;
            if (target_increment >= 0.25f) { return; }
        }
        float phase = m_phase;
        ParameterInterpolator phase_increment(m_phase_increment, target_increment, size);
        ParameterInterpolator gain(m_gain, target_gain, size);
        ParameterInterpolator gain_saw(m_gain_saw, target_gain_saw, size);

        float next_sample = m_next_sample;
        float next_sample_saw = m_next_sample_saw;
        float filter_state = m_filter_state;
        bool high = m_high;

        while (size--) {
            float this_sample = next_sample;
            float this_sample_saw = next_sample_saw;
            next_sample = 0.0f;
            next_sample_saw = 0.0f;

            float increment = interpolate_pitch ? phase_increment.next() : target_increment;
            phase += increment;

            float sample = 0.0f;
            const float pw = 0.5f;

            if (!high && phase >= pw) {
                float t = (phase - pw) / increment;
                this_sample += this_blep_sample(t);
                next_sample += next_blep_sample(t);
                high = true;
            }
            if (phase >= 1.0f) {
                phase -= 1.0f;
                float t = phase / increment;
                float a = this_blep_sample(t);
                float b = next_blep_sample(t);
                this_sample -= a;
                next_sample -= b;
                this_sample_saw -= a;
                next_sample_saw -= b;
                high = false;
            }

            next_sample += phase < pw ? 0.0f : 1.0f;
            next_sample_saw += phase;

            if (shape == Triangle) {
                const float integrator_coefficient = increment * 0.125f;
                this_sample = 64.0f * (this_sample - 0.5f);
                filter_state += integrator_coefficient * (this_sample - filter_state);
                sample = filter_state;
            } else if (shape == DarkSquare) {
                const float integrator_coefficient = increment * 2.0f;
                this_sample = 4.0f * (this_sample - 0.5f);
                filter_state += integrator_coefficient * (this_sample - filter_state);
                sample = filter_state;
            } else if (shape == BrightSquare) {
                const float integrator_coefficient = increment * 2.0f;
                this_sample = 2.0f * this_sample - 1.0f;
                filter_state += integrator_coefficient * (this_sample - filter_state);
                sample = (this_sample - filter_state) * 0.5f;
            } else {
                this_sample = 2.0f * this_sample - 1.0f;
                sample = this_sample;
            }
            this_sample_saw = 2.0f * this_sample_saw - 1.0f;

            *out++ += sample * gain.next() + this_sample_saw * gain_saw.next();
        }
        m_high = high;
        m_phase = phase;
        m_next_sample = next_sample;
        m_next_sample_saw = next_sample_saw;
        m_filter_state = filter_state;
    }

private:
    // Quadratic PolyBLEP residuals.  `t` is the fractional distance past the
    // discontinuity (0..1).  this_blep_sample corrects the current sample;
    // next_blep_sample corrects the following sample.
    static inline float this_blep_sample(float t) { return 0.5f * t * t; }
    static inline float next_blep_sample(float t) {
        t = 1.0f - t;
        return -0.5f * t * t;
    }

    bool m_high = false;
    float m_phase = 0.0f;
    float m_phase_increment = 0.01f;
    float m_next_sample = 0.0f;
    float m_next_sample_saw = 0.0f;
    float m_filter_state = 0.0f;
    float m_gain = 0.0f;
    float m_gain_saw = 0.0f;

    StringSynthOscillator(const StringSynthOscillator&) = delete;
    StringSynthOscillator& operator=(const StringSynthOscillator&) = delete;
};

}  // namespace thl::dsp::synth
