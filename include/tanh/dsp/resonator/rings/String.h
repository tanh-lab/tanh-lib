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

#include <tanh/dsp/utils/DelayLine.h>
#include <tanh/dsp/utils/Svf.h>

#include <tanh/dsp/resonator/rings/Dsp.h>

namespace thl::dsp::resonator::rings {

const size_t kDelayLineSize = 4096;

class DampingFilter {
 public:
  DampingFilter() { }
  ~DampingFilter() { }

  void init() {
    m_x = 0.0f;
    m_x_prev = 0.0f;
    m_brightness = 0.0f;
    m_brightness_increment = 0.0f;
    m_damping = 0.0f;
    m_damping_increment = 0.0f;
  }

  inline void configure(float damping, float brightness, size_t size) {
    if (!size) {
      m_damping = damping;
      m_brightness = brightness;
      m_damping_increment = 0.0f;
      m_brightness_increment = 0.0f;
    } else {
      float step = 1.0f / static_cast<float>(size);
      m_damping_increment = (damping - m_damping) * step;
      m_brightness_increment = (brightness - m_brightness) * step;
    }
  }

  inline float process(float x) {
    float h0 = (1.0f + m_brightness) * 0.5f;
    float h1 = (1.0f - m_brightness) * 0.25f;
    float y = m_damping * (h0 * m_x + h1 * (x + m_x_prev));
    m_x_prev = m_x;
    m_x = x;
    m_brightness += m_brightness_increment;
    m_damping += m_damping_increment;
    return y;
  }
 private:
  float m_x;
  float m_x_prev;
  float m_brightness;
  float m_brightness_increment;
  float m_damping;
  float m_damping_increment;

  DampingFilter(const DampingFilter&) = delete;
  DampingFilter& operator=(const DampingFilter&) = delete;
};

typedef thl::dsp::utils::DelayLine<float, kDelayLineSize> StringDelayLine;
typedef thl::dsp::utils::DelayLine<float, kDelayLineSize / 2> StiffnessDelayLine;

class String {
 public:
  String() { }
  ~String() { }

  void init(bool enable_dispersion, float sample_rate = kDefaultSampleRate);
  void process(const float* in, float* out, float* aux, size_t size);

  inline void set_frequency(float frequency) {
    m_frequency = frequency;
  }

  inline void set_frequency(float frequency, float coefficient) {
    m_frequency += coefficient * (frequency - m_frequency);
  }

  inline void set_dispersion(float dispersion) {
    m_dispersion = dispersion;
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

  inline StringDelayLine* mutable_string() { return &m_string; }

 private:
  void prepare_coefficients(float delay, float src_ratio, size_t size);

  template<bool enable_dispersion>
  void process_internal(const float* in, float* out, float* aux, size_t size);

  float m_sample_rate = kDefaultSampleRate;
  float m_frequency;
  float m_dispersion;
  float m_brightness;
  float m_damping;
  float m_position;

  float m_delay;
  float m_clamped_position;
  float m_previous_dispersion;
  float m_previous_damping_compensation;

  bool m_enable_dispersion;
  bool m_enable_iir_damping;
  float m_dispersion_noise;

  // Very crappy linear interpolation upsampler used for low pitches that
  // do not fit the delay line. Rarely used.
  float m_src_phase;
  float m_out_sample[2];
  float m_aux_sample[2];

  float m_curved_bridge;
  float m_noise_filter;
  float m_damping_compensation_target;

  StringDelayLine m_string;
  StiffnessDelayLine m_stretch;

  DampingFilter m_fir_damping_filter;
  thl::dsp::utils::Svf m_iir_damping_filter;
  thl::dsp::utils::DCBlocker m_dc_blocker;

  String(const String&) = delete;
  String& operator=(const String&) = delete;
};

}  // namespace thl::dsp::resonator::rings
