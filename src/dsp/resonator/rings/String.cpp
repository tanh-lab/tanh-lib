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

#include <tanh/dsp/resonator/rings/String.h>

#include <cmath>

#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/ParameterInterpolator.h>
#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/Random.h>

#include <tanh/dsp/resonator/rings/DspFunctions.h>

namespace thl::dsp::resonator::rings {

using namespace std;
using namespace thl::dsp::utils;
using ::thl::dsp::utils::ParameterInterpolator;

void String::init(bool enable_dispersion, float sample_rate) {
  WarmDspFunctions();

  m_sample_rate = sample_rate;
  m_enable_dispersion = enable_dispersion;

  m_string.init();
  m_stretch.init();
  m_fir_damping_filter.init();
  m_iir_damping_filter.init();

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

  m_dc_blocker.init(1.0f - 20.0f / m_sample_rate);
}

void String::prepare_coefficients(
    float delay, float src_ratio, size_t size) {
  float lf_damping = m_damping * (2.0f - m_damping);
  float rt60 = 0.07f * semitones_to_ratio(lf_damping * 96.0f) * m_sample_rate;
  float rt60_base_2_12 = max(-120.0f * delay / src_ratio / rt60, -127.0f);
  float damping_coefficient = semitones_to_ratio(rt60_base_2_12);
  float brightness = m_brightness * m_brightness;
  m_noise_filter = semitones_to_ratio((m_brightness - 1.0f) * 48.0f);
  float damping_cutoff = min(
      24.0f + m_damping * m_damping * 48.0f + m_brightness * m_brightness * 24.0f,
      84.0f);
  float damping_f = min(m_frequency * semitones_to_ratio(damping_cutoff), 0.499f);

  if (m_damping >= 0.95f) {
    float to_infinite = 20.0f * (m_damping - 0.95f);
    damping_coefficient += to_infinite * (1.0f - damping_coefficient);
    brightness += to_infinite * (1.0f - brightness);
    damping_f += to_infinite * (0.4999f - damping_f);
    damping_cutoff += to_infinite * (128.0f - damping_cutoff);
  }

  m_fir_damping_filter.configure(damping_coefficient, brightness, size);
  m_iir_damping_filter.set_f_q<thl::dsp::utils::FrequencyApproximation::Accurate>(damping_f, 0.5f);
  m_damping_compensation_target = 1.0f - SvfShift(damping_cutoff);
}

template<bool enable_dispersion>
void String::process_internal(
    const float* in,
    float* out,
    float* aux,
    size_t size) {
  float delay = 1.0f / m_frequency;
  CONSTRAIN(delay, 4.0f, kDelayLineSize - 4.0f);

  float src_ratio = delay * m_frequency;
  if (src_ratio >= 0.9999f) {
    m_src_phase = 1.0f;
    src_ratio = 1.0f;
  }

  float clamped_position = 0.5f - 0.98f * fabs(m_position - 0.5f);

  ParameterInterpolator delay_modulation(
      m_delay, delay, size);
  ParameterInterpolator position_modulation(
      m_clamped_position, clamped_position, size);
  ParameterInterpolator dispersion_modulation(
      m_previous_dispersion, m_dispersion, size);

  prepare_coefficients(delay, src_ratio, size);
  float noise_filter = m_noise_filter;
  ParameterInterpolator damping_compensation_modulation(
      m_previous_damping_compensation,
      m_damping_compensation_target,
      size);

  while (size--) {
    m_src_phase += src_ratio;
    if (m_src_phase > 1.0f) {
      m_src_phase -= 1.0f;

      float delay = delay_modulation.next();
      float comb_delay = delay * position_modulation.next();

#ifndef MIC_W
      delay *= damping_compensation_modulation.next();  // IIR delay.
#endif  // MIC_W
      delay -= 1.0f; // FIR delay.

      float s = 0.0f;

      if (enable_dispersion) {
        float noise = 2.0f * Random::get_float() - 1.0f;
        noise *= 1.0f / (0.2f + noise_filter);
        m_dispersion_noise += noise_filter * (noise - m_dispersion_noise);

        float dispersion = dispersion_modulation.next();
        float stretch_point = dispersion <= 0.0f
            ? 0.0f
            : dispersion * (2.0f - dispersion) * 0.475f;
        float noise_amount = dispersion > 0.75f
            ? 4.0f * (dispersion - 0.75f)
            : 0.0f;
        float bridge_curving = dispersion < 0.0f
            ? -dispersion
            : 0.0f;

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
        m_dc_blocker.process(&s_ac, 1);
        s += ac_blocking_amount * (s_ac - s);

        float value = fabs(s) - 0.025f;
        float sign = s > 0.0f ? 1.0f : -1.5f;
        m_curved_bridge = (fabs(value) + value) * sign;
      } else {
        s = m_string.read_hermite(delay);
      }

      s += *in;  // When f0 < 11.7 Hz, causes ugly bitcrushing on the input!
      s = m_fir_damping_filter.process(s);
#ifndef MIC_W
      s = m_iir_damping_filter.process<thl::dsp::utils::FilterMode::LowPass>(s);
#endif  // MIC_W
      m_string.write(s);

      m_out_sample[1] = m_out_sample[0];
      m_aux_sample[1] = m_aux_sample[0];

      m_out_sample[0] = s;
      m_aux_sample[0] = m_string.read(comb_delay);
    }
    *out++ += crossfade(m_out_sample[1], m_out_sample[0], m_src_phase);
    *aux++ += crossfade(m_aux_sample[1], m_aux_sample[0], m_src_phase);
    in++;
  }
}

void String::process(const float* in, float* out, float* aux, size_t size) {
  if (m_enable_dispersion) {
    process_internal<true>(in, out, aux, size);
  } else {
    process_internal<false>(in, out, aux, size);
  }
}

}  // namespace thl::dsp::resonator::rings
