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

#include <tanh/dsp/rings-resonator/RingsString.h>

#include <cmath>

#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/ParameterInterpolator.h>
#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/Random.h>

#include <tanh/dsp/rings-resonator/RingsDspFunctions.h>

namespace thl::dsp::resonator {

using namespace std;
using namespace thl::dsp::utils;
using ::thl::dsp::utils::ParameterInterpolator;

void RingsString::prepare(bool enable_dispersion, float sample_rate) {
    WarmDspFunctions();

    m_sample_rate = sample_rate;
    m_enable_dispersion = enable_dispersion;

    m_string.prepare();
    m_stretch.prepare();
    m_fir_damping_filter.prepare();
    m_iir_damping_filter.reset();

    set_frequency(220.0f / m_sample_rate);
    set_dispersion(0.25f);
    set_brightness(0.5f);
    set_damping(0.3f);
    set_position(0.8f);

    m_delay = 1.0f / m_frequency;
    m_clamped_position = 0.0f;
    m_previous_dispersion = 0.0f;
    m_dispersion_noise = 0.0f;
    m_curved_bridge = 0.0f;
    m_previous_damping_compensation = 0.0f;
    m_noise_filter = 0.0f;
    m_damping_compensation_target = 0.0f;

    m_out_sample[0] = m_out_sample[1] = 0.0f;
    m_aux_sample[0] = m_aux_sample[1] = 0.0f;

    m_dc_blocker.prepare(1.0f - 20.0f / m_sample_rate);
}

void RingsString::prepare_coefficients(float delay, float src_ratio, size_t size) {
    // RT60-based decay: convert the damping knob (0..1) into a per-sample
    // energy loss coefficient so that the string rings for a musically useful
    // duration. The mapping is: damping -> RT60 in samples -> per-sample gain.
    float lf_damping = m_damping * (2.0f - m_damping);
    float rt60 = 0.07f * semitones_to_ratio(lf_damping * 96.0f) * m_sample_rate;
    float rt60_base_2_12 = max(-120.0f * delay / src_ratio / rt60, -127.0f);
    float damping_coefficient = semitones_to_ratio(rt60_base_2_12);
    float brightness = m_brightness * m_brightness;
    m_noise_filter = semitones_to_ratio((m_brightness - 1.0f) * 48.0f);
    float damping_cutoff =
        min(24.0f + m_damping * m_damping * 48.0f + m_brightness * m_brightness * 24.0f, 84.0f);
    float damping_f = min(m_frequency * semitones_to_ratio(damping_cutoff), 0.499f);

    // Crossfade towards infinite sustain when damping > 0.95.  All loss
    // parameters are interpolated towards unity / Nyquist so the string
    // rings indefinitely at damping = 1.0.
    if (m_damping >= 0.95f) {
        float to_infinite = 20.0f * (m_damping - 0.95f);
        damping_coefficient += to_infinite * (1.0f - damping_coefficient);
        brightness += to_infinite * (1.0f - brightness);
        damping_f += to_infinite * (0.4999f - damping_f);
        damping_cutoff += to_infinite * (128.0f - damping_cutoff);
    }

    m_fir_damping_filter.configure(damping_coefficient, brightness, size);
    m_iir_damping_filter.set_f_q<Approximation::Accurate>(damping_f,
                                                                                     0.5f);
    m_damping_compensation_target = 1.0f - SvfShift(damping_cutoff);
}

template <bool enable_dispersion>
void RingsString::process_internal(thl::dsp::audio::ConstAudioBufferView in,
                                   thl::dsp::audio::AudioBufferView out,
                                   thl::dsp::audio::AudioBufferView aux) {
    const float* in_ptr = in.get_read_pointer(0);
    float* out_ptr = out.get_write_pointer(0);
    float* aux_ptr = aux.get_write_pointer(0);
    size_t size = in.get_num_frames();

    float delay = 1.0f / m_frequency;
    thl::dsp::utils::constrain<float>(delay, 4.0f, kDelayLineSize - 4.0f);

    // Sample-rate conversion ratio.  When the required delay fits in the
    // buffer, src_ratio = 1 and we run at full rate.  For very low pitches
    // the delay would exceed the buffer, so src_ratio < 1 and the inner
    // loop runs at half rate (the outer loop still advances at full rate,
    // crossfading between successive output samples).
    float src_ratio = delay * m_frequency;
    if (src_ratio >= 0.9999f) {
        m_src_phase = 1.0f;
        src_ratio = 1.0f;
    }

    float clamped_position = 0.5f - 0.98f * fabs(m_position - 0.5f);

    ParameterInterpolator delay_modulation(m_delay, delay, size);
    ParameterInterpolator position_modulation(m_clamped_position, clamped_position, size);
    ParameterInterpolator dispersion_modulation(m_previous_dispersion, m_dispersion, size);

    prepare_coefficients(delay, src_ratio, size);
    float noise_filter = m_noise_filter;
    ParameterInterpolator damping_compensation_modulation(m_previous_damping_compensation,
                                                          m_damping_compensation_target,
                                                          size);

    // Main sample loop.  The outer loop runs at the host sample rate; the
    // inner body (guarded by src_phase) executes at most once per output
    // sample at full rate, or once every two samples at half rate.
    while (size--) {
        m_src_phase += src_ratio;
        if (m_src_phase > 1.0f) {
            m_src_phase -= 1.0f;

            float delay = delay_modulation.next();
            float comb_delay = delay * position_modulation.next();

#ifndef MIC_W
            delay *= damping_compensation_modulation.next();  // IIR delay.
#endif                                                        // MIC_W
            delay -= 1.0f;                                    // FIR delay.

            float s = 0.0f;

            // -- Dispersion path: allpass stiffness, noise modulation, curved
            // bridge
            if (enable_dispersion) {
                float noise = 2.0f * Random::get_float() - 1.0f;
                noise *= 1.0f / (0.2f + noise_filter);
                m_dispersion_noise += noise_filter * (noise - m_dispersion_noise);

                float dispersion = dispersion_modulation.next();
                float stretch_point =
                    dispersion <= 0.0f ? 0.0f : dispersion * (2.0f - dispersion) * 0.475f;
                float noise_amount = dispersion > 0.75f ? 4.0f * (dispersion - 0.75f) : 0.0f;
                float bridge_curving = dispersion < 0.0f ? -dispersion : 0.0f;

                noise_amount = noise_amount * noise_amount * 0.025f;
                float ac_blocking_amount = bridge_curving;

                bridge_curving = bridge_curving * bridge_curving * 0.01f;
                float ap_gain = -0.618f * dispersion / (0.15f + fabs(dispersion));

                float delay_fm = 1.0f;
                delay_fm += m_dispersion_noise * noise_amount;
                delay_fm -= m_curved_bridge * bridge_curving;
                delay *= delay_fm;

                float ap_delay = delay * stretch_point;
                float main_delay = delay - ap_delay;
                if (ap_delay >= 4.0f && main_delay >= 4.0f) {
                    s = m_string.read_hermite(main_delay);
                    s = m_stretch.allpass(s, ap_delay, ap_gain);
                } else {
                    s = m_string.read_hermite(delay);
                }
                float s_ac = s;
                thl::dsp::audio::AudioBufferView dc_view(&s_ac, 1);
                m_dc_blocker.process(dc_view);
                s += ac_blocking_amount * (s_ac - s);

                float value = fabs(s) - 0.025f;
                float sign = s > 0.0f ? 1.0f : -1.5f;
                m_curved_bridge = (fabs(value) + value) * sign;
            } else {
                s = m_string.read_hermite(delay);
            }

            // Inject excitation, then run the feedback damping chain:
            //   FIR averaging -> IIR low-pass (TPT SVF) -> write back into
            //   delay.
            s += *in_ptr;
            s = m_fir_damping_filter.process(s);
#ifndef MIC_W
            s = m_iir_damping_filter.process<thl::dsp::filter::FilterMode::LowPass>(s);
#endif  // MIC_W
            m_string.write(s);

            // Store two most recent output/aux samples for crossfade
            // interpolation when running at half rate.
            m_out_sample[1] = m_out_sample[0];
            m_aux_sample[1] = m_aux_sample[0];

            m_out_sample[0] = s;
            m_aux_sample[0] = m_string.read(comb_delay);
        }
        // Linear crossfade between the two most recent inner-loop outputs,
        // weighted by the fractional SRC phase.  At full rate (src_ratio = 1)
        // this always picks m_out_sample[0]; at half rate it blends successive
        // samples to hide the decimation.
        *out_ptr++ += crossfade(m_out_sample[1], m_out_sample[0], m_src_phase);
        *aux_ptr++ += crossfade(m_aux_sample[1], m_aux_sample[0], m_src_phase);
        in_ptr++;
    }
}

void RingsString::process(thl::dsp::audio::ConstAudioBufferView in,
                          thl::dsp::audio::AudioBufferView out,
                          thl::dsp::audio::AudioBufferView aux) {
    if (m_enable_dispersion) {
        process_internal<true>(in, out, aux);
    } else {
        process_internal<false>(in, out, aux);
    }
}

}  // namespace thl::dsp::resonator
