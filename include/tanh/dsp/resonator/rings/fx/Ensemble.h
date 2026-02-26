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
// Ensemble FX.

#pragma once


#include <tanh/dsp/utils/DspMath.h>

#include <tanh/dsp/resonator/rings/fx/FxEngine.h>
#include <tanh/dsp/resonator/rings/DspFunctions.h>

namespace thl::dsp::resonator::rings {

class Ensemble {
 public:
  Ensemble() { }
  ~Ensemble() { }

  void init(uint16_t* buffer, float sample_rate = kDefaultSampleRate) {
    WarmDspFunctions();
    m_sine_table = SineTable();
    m_engine.init(buffer);
    m_rate_ratio = sample_rate / kDefaultSampleRate;
    m_phase_1 = 0;
    m_phase_2 = 0;
  }

  void process(float* left, float* right, size_t size) {
    typedef E::Reserve<4095, E::Reserve<4095> > Memory;
    E::DelayLine<Memory, 0> line_l;
    E::DelayLine<Memory, 1> line_r;
    E::Context c;

    const float r = m_rate_ratio;

    while (size--) {
      m_engine.start(&c);
      float dry_amount = 1.0f - m_amount * 0.5f;

      // Update LFO (scale phase increments inversely with sample rate).
      m_phase_1 += 1.57e-05f / r;
      if (m_phase_1 >= 1.0f) {
        m_phase_1 -= 1.0f;
      }
      m_phase_2 += 1.37e-04f / r;
      if (m_phase_2 >= 1.0f) {
        m_phase_2 -= 1.0f;
      }
      int32_t phi_1 = (m_phase_1 * 4096.0f);
      float slow_0 = m_sine_table[phi_1 & 4095];
      float slow_120 = m_sine_table[(phi_1 + 1365) & 4095];
      float slow_240 = m_sine_table[(phi_1 + 2730) & 4095];
      int32_t phi_2 = (m_phase_2 * 4096.0f);
      float fast_0 = m_sine_table[phi_2 & 4095];
      float fast_120 = m_sine_table[(phi_2 + 1365) & 4095];
      float fast_240 = m_sine_table[(phi_2 + 2730) & 4095];

      float a = m_depth * 1.0f;
      float b = m_depth * 0.1f;

      float mod_1 = slow_0 * a + fast_0 * b;
      float mod_2 = slow_120 * a + fast_120 * b;
      float mod_3 = slow_240 * a + fast_240 * b;

      float wet = 0.0f;

      // Sum L & R channel to send to chorus line.
      c.read(*left, 1.0f);
      c.write(line_l, 0.0f);
      c.read(*right, 1.0f);
      c.write(line_r, 0.0f);

      c.interpolate(line_l, mod_1 + 1024.0f * r, 0.33f);
      c.interpolate(line_l, mod_2 + 1024.0f * r, 0.33f);
      c.interpolate(line_r, mod_3 + 1024.0f * r, 0.33f);
      c.write(wet, 0.0f);
      *left = wet * m_amount + *left * dry_amount;

      c.interpolate(line_r, mod_1 + 1024.0f * r, 0.33f);
      c.interpolate(line_r, mod_2 + 1024.0f * r, 0.33f);
      c.interpolate(line_l, mod_3 + 1024.0f * r, 0.33f);
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
    m_depth = depth * 128.0f * m_rate_ratio;
  }

 private:
  typedef FxEngine<8192, FORMAT_16_BIT> E;
  E m_engine;

  float m_amount;
  float m_depth;
  float m_rate_ratio = 1.0f;
  const float* m_sine_table = nullptr;

  float m_phase_1;
  float m_phase_2;

  Ensemble(const Ensemble&) = delete;
  Ensemble& operator=(const Ensemble&) = delete;
};

}  // namespace thl::dsp::resonator::rings
