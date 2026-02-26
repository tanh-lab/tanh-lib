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
// Low pass filter for getting stable pitch data.

#pragma once

#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/DelayLine.h>

namespace thl::dsp::resonator::rings {

class NoteFilter {
 public:
  enum {
    N = 4  // Median filter order
  };
  NoteFilter() { }
  ~NoteFilter() { }

  void init(
      float sample_rate,
      float time_constant_fast_edge,
      float time_constant_steady_part,
      float edge_recovery_time,
      float edge_avoidance_delay) {
    m_fast_coefficient = 1.0f / (time_constant_fast_edge * sample_rate);
    m_slow_coefficient = 1.0f / (time_constant_steady_part * sample_rate);
    m_lag_coefficient = 1.0f / (edge_recovery_time * sample_rate);

    m_delayed_stable_note.init();
    m_delayed_stable_note.set_delay(
        std::min(size_t(15), size_t(edge_avoidance_delay * sample_rate)));

    m_stable_note = m_note = 69.0f;
    m_coefficient = m_fast_coefficient;
    m_stable_coefficient = m_slow_coefficient;
    std::fill(&m_previous_values[0], &m_previous_values[N], m_note);
  }

  inline float process(float note, bool strum) {
    // If there is a sharp change, follow it instantly.
    if (fabs(note - m_note) > 0.4f || strum) {
      m_stable_note = m_note = note;
      m_coefficient = m_fast_coefficient;
      m_stable_coefficient = m_slow_coefficient;
      std::fill(&m_previous_values[0], &m_previous_values[N], note);
    } else {
      // Median filtering of the raw ADC value.
      float sorted_values[N];
      std::rotate(
          &m_previous_values[0],
          &m_previous_values[1],
          &m_previous_values[N]);
      m_previous_values[N - 1] = note;
      std::copy(&m_previous_values[0], &m_previous_values[N], &sorted_values[0]);
      std::sort(&sorted_values[0], &sorted_values[N]);
      float median = 0.5f * (sorted_values[(N - 1) / 2] + sorted_values[N / 2]);

      // Adaptive lag processor.
      m_note += m_coefficient * (median - m_note);
      m_stable_note += m_stable_coefficient * (m_note - m_stable_note);

      m_coefficient += m_lag_coefficient * (m_slow_coefficient - m_coefficient);
      m_stable_coefficient += m_lag_coefficient * \
          (m_lag_coefficient - m_stable_coefficient);

      m_delayed_stable_note.write(m_stable_note);
    }
    return m_note;
  }

  inline float note() const {
    return m_note;
  }

  inline float stable_note() const {
    return m_delayed_stable_note.read();
  }

 private:
  float m_previous_values[N];
  float m_note;
  float m_stable_note;
  thl::dsp::utils::DelayLine<float, 16> m_delayed_stable_note;

  float m_coefficient;
  float m_stable_coefficient;

  float m_fast_coefficient;
  float m_slow_coefficient;
  float m_lag_coefficient;
};

}  // namespace thl::dsp::resonator::rings
