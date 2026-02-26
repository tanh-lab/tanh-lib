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

#pragma once


#include <algorithm>

#include <tanh/dsp/utils/Svf.h>

#include <tanh/dsp/resonator/rings/Dsp.h>
#include <tanh/dsp/resonator/rings/Follower.h>

#include <tanh/dsp/resonator/rings/DspFunctions.h>

namespace thl::dsp::resonator::rings {

using namespace thl::dsp::utils;

class FMVoice {
 public:
  FMVoice() { }
  ~FMVoice() { }

  void init(float sample_rate = kDefaultSampleRate);
  void process(
      const float* in,
      float* out,
      float* aux,
      size_t size);

  inline void set_frequency(float frequency) {
    m_carrier_frequency = frequency;
  }

  inline void set_ratio(float ratio) {
    m_ratio = ratio;
  }

  inline void set_brightness(float brightness) {
    m_brightness = brightness;
  }

  inline void set_damping(float damping) {
    m_damping = damping;
  }

  inline void set_position(float position) {
    m_position = position;
  }

  inline void set_feedback_amount(float feedback_amount) {
    m_feedback_amount = feedback_amount;
  }

  inline void trigger_internal_envelope() {
    m_amplitude_envelope = 1.0f;
    m_brightness_envelope = 1.0f;
  }

  inline float sine_fm(uint32_t phase, float fm) const {
    phase += (static_cast<uint32_t>((fm + 4.0f) * 536870912.0f)) << 3;
    uint32_t integral = phase >> 20;
    float fractional = static_cast<float>(phase << 12) / 4294967296.0f;
    float a = m_sine_table[integral];
    float b = m_sine_table[integral + 1];
    return a + (b - a) * fractional;
  }

 private:
  void prepare_coefficients();

  float m_sample_rate = kDefaultSampleRate;
  float m_carrier_frequency;
  float m_ratio;
  float m_brightness;
  float m_damping;
  float m_position;
  float m_feedback_amount;

  float m_previous_carrier_frequency;
  float m_previous_modulator_frequency;
  float m_previous_brightness;
  float m_previous_damping;
  float m_previous_feedback_amount;

  float m_amplitude_envelope;
  float m_brightness_envelope;
  float m_gain;
  float m_fm_amount;
  uint32_t m_carrier_phase;
  uint32_t m_modulator_phase;
  float m_previous_sample;

  float m_envelope_amount;
  float m_amplitude_decay;
  float m_brightness_decay;
  float m_modulator_frequency;
  float m_feedback;
  const float* m_sine_table = nullptr;
  const float* m_fm_frequency_quantizer_table = nullptr;

  Follower m_follower;

  FMVoice(const FMVoice&) = delete;
  FMVoice& operator=(const FMVoice&) = delete;
};

}  // namespace thl::dsp::resonator::rings
