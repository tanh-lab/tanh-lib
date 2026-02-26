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
// AD envelope for the string synth.

#pragma once


namespace thl::dsp::resonator::rings {

enum EnvelopeShape {
  ENVELOPE_SHAPE_LINEAR,
  ENVELOPE_SHAPE_QUARTIC
};

enum EnvelopeFlags {
  ENVELOPE_FLAG_RISING_EDGE = 1,
  ENVELOPE_FLAG_FALLING_EDGE = 2,
  ENVELOPE_FLAG_GATE = 4
};

class StringSynthEnvelope {
 public:
  StringSynthEnvelope() { }
  ~StringSynthEnvelope() { }

  void init() {
    set_ad(0.1f, 0.001f);
    m_segment = m_num_segments;
    m_phase = 0.0f;
    m_start_value = 0.0f;
    m_value = 0.0f;
  }

  inline float process(uint8_t flags) {
    if (flags & ENVELOPE_FLAG_RISING_EDGE) {
      m_start_value = m_segment == m_num_segments ? m_level[0] : m_value;
      m_segment = 0;
      m_phase = 0.0f;
    } else if (flags & ENVELOPE_FLAG_FALLING_EDGE && m_sustain_point) {
      m_start_value = m_value;
      m_segment = m_sustain_point;
      m_phase = 0.0f;
    } else if (m_phase >= 1.0f) {
      m_start_value = m_level[m_segment + 1];
      ++m_segment;
      m_phase = 0.0f;
    }

    bool done = m_segment == m_num_segments;
    bool sustained = m_sustain_point && m_segment == m_sustain_point &&
        flags & ENVELOPE_FLAG_GATE;

    float phase_increment = 0.0f;
    if (!sustained && !done) {
      phase_increment = m_rate[m_segment];
    }
    float t = m_phase;
    if (m_shape[m_segment] == ENVELOPE_SHAPE_QUARTIC) {
      t = 1.0f - t;
      t *= t;
      t *= t;
      t = 1.0f - t;
    }

    m_phase += phase_increment;
    m_value = m_start_value + (m_level[m_segment + 1] - m_start_value) * t;
    return m_value;
  }

  inline void set_ad(float attack, float decay) {
    m_num_segments = 2;
    m_sustain_point = 0;

    m_level[0] = 0.0f;
    m_level[1] = 1.0f;
    m_level[2] = 0.0f;

    m_rate[0] = attack;
    m_rate[1] = decay;

    m_shape[0] = ENVELOPE_SHAPE_LINEAR;
    m_shape[1] = ENVELOPE_SHAPE_QUARTIC;
  }

  inline void set_ar(float attack, float decay) {
    m_num_segments = 2;
    m_sustain_point = 1;

    m_level[0] = 0.0f;
    m_level[1] = 1.0f;
    m_level[2] = 0.0f;

    m_rate[0] = attack;
    m_rate[1] = decay;

    m_shape[0] = ENVELOPE_SHAPE_LINEAR;
    m_shape[1] = ENVELOPE_SHAPE_LINEAR;
  }

 private:
  float m_level[4];
  float m_rate[4];
  EnvelopeShape m_shape[4];

  int16_t m_segment;
  float m_start_value;
  float m_value;
  float m_phase;

  uint16_t m_num_segments;
  uint16_t m_sustain_point;

  StringSynthEnvelope(const StringSynthEnvelope&) = delete;
  StringSynthEnvelope& operator=(const StringSynthEnvelope&) = delete;
};

}  // namespace thl::dsp::resonator::rings
