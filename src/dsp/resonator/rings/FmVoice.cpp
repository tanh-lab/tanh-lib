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
// FM Voice.

#include <tanh/dsp/resonator/rings/FmVoice.h>

#include <cmath>

#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/ParameterInterpolator.h>
#include <tanh/dsp/utils/DspMath.h>

#include <tanh/dsp/resonator/rings/DspFunctions.h>

namespace thl::dsp::resonator::rings {

using namespace thl::dsp::utils;
using ::thl::dsp::utils::ParameterInterpolator;

void FMVoice::init(float sample_rate) {
  m_sample_rate = sample_rate;

  WarmDspFunctions();
  m_sine_table = SineTable();
  m_fm_frequency_quantizer_table = FmFrequencyQuantizerTable();

  set_frequency(220.0f / m_sample_rate);
  set_ratio(0.5f);
  set_brightness(0.5f);
  set_damping(0.5f);
  set_position(0.5f);
  set_feedback_amount(0.0f);

  m_previous_carrier_frequency = m_carrier_frequency;
  m_previous_modulator_frequency = m_carrier_frequency;
  m_previous_brightness = m_brightness;
  m_previous_damping = m_damping;
  m_previous_feedback_amount = m_feedback_amount;

  m_amplitude_envelope = 0.0f;
  m_brightness_envelope = 0.0f;

  m_carrier_phase = 0;
  m_modulator_phase = 0;
  m_gain = 0.0f;
  m_fm_amount = 0.0f;
  m_previous_sample = 0.0f;

  m_envelope_amount = 0.0f;
  m_amplitude_decay = 0.0f;
  m_brightness_decay = 0.0f;
  m_modulator_frequency = 0.0f;
  m_feedback = 0.0f;

  m_follower.init(
      8.0f / m_sample_rate,
      160.0f / m_sample_rate,
      1600.0f / m_sample_rate);
}

void FMVoice::prepare_coefficients() {
  m_envelope_amount = m_damping < 0.9f ? 1.0f : (1.0f - m_damping) * 10.0f;
  float amplitude_rt60 =
      0.1f * semitones_to_ratio(m_damping * 96.0f) * m_sample_rate;
  m_amplitude_decay = 1.0f - powf(0.001f, 1.0f / amplitude_rt60);

  float brightness_rt60 =
      0.1f * semitones_to_ratio(m_damping * 84.0f) * m_sample_rate;
  m_brightness_decay = 1.0f - powf(0.001f, 1.0f / brightness_rt60);

  float ratio = interpolate(m_fm_frequency_quantizer_table, m_ratio, 128.0f);
  m_modulator_frequency = m_carrier_frequency * semitones_to_ratio(ratio);
  if (m_modulator_frequency > 0.5f) {
    m_modulator_frequency = 0.5f;
  }

  m_feedback = (m_feedback_amount - 0.5f) * 2.0f;
}

void FMVoice::process(const float* in, float* out, float* aux, size_t size) {
  prepare_coefficients();

  ParameterInterpolator carrier_increment(
      m_previous_carrier_frequency, m_carrier_frequency, size);
  ParameterInterpolator modulator_increment(
      m_previous_modulator_frequency, m_modulator_frequency, size);
  ParameterInterpolator brightness(
      m_previous_brightness, m_brightness, size);
  ParameterInterpolator feedback_amount(
      m_previous_feedback_amount, m_feedback, size);

  uint32_t carrier_phase = m_carrier_phase;
  uint32_t modulator_phase = m_modulator_phase;
  float previous_sample = m_previous_sample;

  while (size--) {
    // Envelope follower and internal envelope.
    float amplitude_envelope, brightness_envelope;
    m_follower.process(
        *in++,
        &amplitude_envelope,
        &brightness_envelope);

    brightness_envelope *= 2.0f * amplitude_envelope * (2.0f - amplitude_envelope);

    SLOPE(m_amplitude_envelope, amplitude_envelope, 0.05f, m_amplitude_decay);
    SLOPE(m_brightness_envelope, brightness_envelope, 0.01f, m_brightness_decay);

    // Compute envelopes.
    float brightness_value = brightness.next();
    brightness_value *= brightness_value;
    float fm_amount_min = brightness_value < 0.5f
        ? 0.0f
        : brightness_value * 2.0f - 1.0f;
    float fm_amount_max = brightness_value < 0.5f
        ? 2.0f * brightness_value
        : 1.0f;
    float fm_envelope = 0.5f + m_envelope_amount * (m_brightness_envelope - 0.5f);
    float fm_amount = (fm_amount_min + fm_amount_max * fm_envelope) * 2.0f;
    SLEW(m_fm_amount, fm_amount, 0.005f + fm_amount_max * 0.015f);

    // FM synthesis in itself
    float phase_feedback = m_feedback < 0.0f ? 0.5f * m_feedback * m_feedback : 0.0f;
    modulator_phase += static_cast<uint32_t>(4294967296.0f * \
      modulator_increment.next() * (1.0f + previous_sample * phase_feedback));
    carrier_phase += static_cast<uint32_t>(4294967296.0f * \
        carrier_increment.next());

    float feedback = feedback_amount.next();
    float modulator_fb = feedback > 0.0f ? 0.25f * feedback * feedback : 0.0f;
    float modulator = sine_fm(modulator_phase, modulator_fb * previous_sample);
    float carrier = sine_fm(carrier_phase, m_fm_amount * modulator);
    ONE_POLE(previous_sample, carrier, 0.1f);

    // Compute amplitude envelope.
    float gain = 1.0f + m_envelope_amount * (m_amplitude_envelope - 1.0f);
    ONE_POLE(m_gain, gain, 0.005f + 0.045f * m_fm_amount);

    *out++ = (carrier + 0.5f * modulator) * m_gain;
    *aux++ = 0.5f * modulator * m_gain;
  }
  m_carrier_phase = carrier_phase;
  m_modulator_phase = modulator_phase;
  m_previous_sample = previous_sample;
}

}  // namespace thl::dsp::resonator::rings
