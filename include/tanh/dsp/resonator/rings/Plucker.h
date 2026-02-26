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
// Noise burst generator for Karplus-Strong synthesis.

#pragma once


#include <algorithm>

#include <tanh/dsp/utils/Svf.h>
#include <tanh/dsp/utils/DelayLine.h>
#include <tanh/dsp/utils/Random.h>

namespace thl::dsp::resonator::rings {

class Plucker {
 public:
  Plucker() { }
  ~Plucker() { }

  void init() {
    m_svf.init();
    m_comb_filter.init();
    m_remaining_samples = 0;
    m_comb_filter_period = 0.0f;
  }

  void trigger(float frequency, float cutoff, float position) {
    float ratio = position * 0.9f + 0.05f;
    float comb_period = 1.0f / frequency * ratio;
    m_remaining_samples = static_cast<size_t>(comb_period);
    while (comb_period >= 255.0f) {
      comb_period *= 0.5f;
    }
    m_comb_filter_period = comb_period;
    m_comb_filter_gain = (1.0f - position) * 0.8f;
    m_svf.set_f_q<thl::dsp::utils::FrequencyApproximation::Dirty>(std::min(cutoff, 0.499f), 1.0f);
  }

  void process(float* out, size_t size) {
    const float comb_gain = m_comb_filter_gain;
    const float comb_delay = m_comb_filter_period;
    for (size_t i = 0; i < size; ++i) {
      float in = 0.0f;
      if (m_remaining_samples) {
        in = 2.0f * Random::get_float() - 1.0f;
        --m_remaining_samples;
      }
      out[i] = in + comb_gain * m_comb_filter.read(comb_delay);
      m_comb_filter.write(out[i]);
    }
    m_svf.process<thl::dsp::utils::FilterMode::LowPass>(out, out, size);
  }

 private:
  thl::dsp::utils::Svf m_svf;
  thl::dsp::utils::DelayLine<float, 256> m_comb_filter;
  size_t m_remaining_samples;
  float m_comb_filter_period;
  float m_comb_filter_gain;

  Plucker(const Plucker&) = delete;
  Plucker& operator=(const Plucker&) = delete;
};

}  // namespace thl::dsp::resonator::rings
