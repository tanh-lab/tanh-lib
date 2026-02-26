// Copyright 2014 Emilie Gillet.
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
// Chorus.

#pragma once


#include <tanh/dsp/utils/DspMath.h>

#include <tanh/dsp/resonator/rings/fx/FxEngine.h>
#include <tanh/dsp/resonator/rings/DspFunctions.h>

namespace thl::dsp::resonator::rings {

class Chorus {
 public:
  Chorus() { }
  ~Chorus() { }

  void init(uint16_t* buffer, float sample_rate = kDefaultSampleRate) {
    WarmDspFunctions();
    m_sine_table = SineTable();
    m_engine.init(buffer);
    m_rate_ratio = sample_rate / kDefaultSampleRate;
    m_phase_1 = 0;
    m_phase_2 = 0;
  }

  void process(float* left, float* right, size_t size) {
    typedef E::Reserve<4095> Memory;
    E::DelayLine<Memory, 0> line;
    E::Context c;

    const float r = m_rate_ratio;

    while (size--) {
      m_engine.start(&c);
      float dry_amount = 1.0f - m_amount * 0.5f;

      // Update LFO (scale phase increments inversely with sample rate).
      m_phase_1 += 4.17e-06f / r;
      if (m_phase_1 >= 1.0f) {
        m_phase_1 -= 1.0f;
      }
      m_phase_2 += 5.417e-06f / r;
      if (m_phase_2 >= 1.0f) {
        m_phase_2 -= 1.0f;
      }
      float sin_1 = thl::dsp::utils::interpolate(m_sine_table, m_phase_1, 4096.0f);
      float cos_1 = thl::dsp::utils::interpolate(m_sine_table, m_phase_1 + 0.25f, 4096.0f);
      float sin_2 = thl::dsp::utils::interpolate(m_sine_table, m_phase_2, 4096.0f);
      float cos_2 = thl::dsp::utils::interpolate(m_sine_table, m_phase_2 + 0.25f, 4096.0f);

      float wet;

      // Sum L & R channel to send to chorus line.
      c.read(*left, 0.5f);
      c.read(*right, 0.5f);
      c.write(line, 0.0f);

      c.interpolate(line, sin_1 * m_depth + 1200.0f * r, 0.5f);
      c.interpolate(line, sin_2 * m_depth + 800.0f * r, 0.5f);
      c.write(wet, 0.0f);
      *left = wet * m_amount + *left * dry_amount;

      c.interpolate(line, cos_1 * m_depth + 800.0f * r + cos_2 * 0, 0.5f);
      c.interpolate(line, cos_2 * m_depth + 1200.0f * r, 0.5f);
      c.write(wet, 0.0f);
      *right = wet * m_amount + *right * dry_amount;
      left++;
      right++;
    }
  }

  inline void set_amount(float amount) {
    m_amount = amount;
  }

  inline void set_depth(float depth) {
    m_depth = depth * 384.0f * m_rate_ratio;
  }

 private:
  typedef FxEngine<4096, FORMAT_16_BIT> E;
  E m_engine;

  float m_amount;
  float m_depth;
  float m_rate_ratio = 1.0f;
  const float* m_sine_table = nullptr;

  float m_phase_1;
  float m_phase_2;

  Chorus(const Chorus&) = delete;
  Chorus& operator=(const Chorus&) = delete;
};

}  // namespace thl::dsp::resonator::rings
