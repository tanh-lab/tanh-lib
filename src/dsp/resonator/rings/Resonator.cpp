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
// Resonator.

#include <tanh/dsp/resonator/rings/Resonator.h>

#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/CosineOscillator.h>
#include <tanh/dsp/utils/ParameterInterpolator.h>

#include <tanh/dsp/resonator/rings/DspFunctions.h>

namespace thl::dsp::resonator::rings {

using namespace std;
using namespace thl::dsp::utils;
using ::thl::dsp::utils::ParameterInterpolator;

void Resonator::init(float sample_rate) {
  m_sample_rate = sample_rate;

  for (int32_t i = 0; i < kMaxModes; ++i) {
    m_f[i].init();
  }

  set_frequency(220.0f / m_sample_rate);
  set_structure(0.25f);
  set_brightness(0.5f);
  set_damping(0.3f);
  set_position(0.999f);
  set_resolution(kMaxModes);
}

int32_t Resonator::compute_filters() {
  float stiffness = Stiffness(m_structure);
  float harmonic = m_frequency;
  float stretch_factor = 1.0f;
  float q = 500.0f * FourDecades(m_damping);
  float brightness_attenuation = 1.0f - m_structure;
  // Reduces the range of brightness when structure is very low, to prevent
  // clipping.
  brightness_attenuation *= brightness_attenuation;
  brightness_attenuation *= brightness_attenuation;
  brightness_attenuation *= brightness_attenuation;
  float brightness = m_brightness * (1.0f - 0.2f * brightness_attenuation);
  float q_loss = brightness * (2.0f - brightness) * 0.85f + 0.15f;
  float q_loss_damping_rate = m_structure * (2.0f - m_structure) * 0.1f;
  int32_t num_modes = 0;
  for (int32_t i = 0; i < min(kMaxModes, m_resolution); ++i) {
    float partial_frequency = harmonic * stretch_factor;
    if (partial_frequency >= 0.49f) {
      partial_frequency = 0.49f;
    } else {
      num_modes = i + 1;
    }
    m_f[i].set_f_q<thl::dsp::utils::FrequencyApproximation::Fast>(
        partial_frequency,
        1.0f + partial_frequency * q);
    stretch_factor += stiffness;
    if (stiffness < 0.0f) {
      // Make sure that the partials do not fold back into negative frequencies.
      stiffness *= 0.93f;
    } else {
      // This helps adding a few extra partials in the highest frequencies.
      stiffness *= 0.98f;
    }
    // This prevents the highest partials from decaying too fast.
    q_loss += q_loss_damping_rate * (1.0f - q_loss);
    harmonic += m_frequency;
    q *= q_loss;
  }

  return num_modes;
}

void Resonator::process(const float* in, float* out, float* aux, size_t size) {
  if (m_dirty) {
    m_num_modes = compute_filters();
    m_dirty = false;
  }
  int32_t num_modes = m_num_modes;

  ParameterInterpolator position(m_previous_position, m_position, size);
  while (size--) {
    CosineOscillator amplitudes;
    amplitudes.init<thl::dsp::utils::CosineOscillatorMode::Approximate>(position.next());

    float input = *in++ * 0.125f;
    float odd = 0.0f;
    float even = 0.0f;
    amplitudes.start();
    for (int32_t i = 0; i < num_modes;) {
      odd += amplitudes.next() * m_f[i++].process<thl::dsp::utils::FilterMode::BandPass>(input);
      even += amplitudes.next() * m_f[i++].process<thl::dsp::utils::FilterMode::BandPass>(input);
    }
    *out++ = odd;
    *aux++ = even;
  }
}

}  // namespace thl::dsp::resonator::rings
