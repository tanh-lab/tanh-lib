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
// Envelope / centroid follower for FM voice.

#pragma once


#include <algorithm>

#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/Svf.h>

namespace thl::dsp::resonator::rings {

using namespace thl::dsp::utils;

class Follower {
 public:
  Follower() { }
  ~Follower() { }

  void init(float low, float low_mid, float mid_high) {
    m_low_mid_filter.init();
    m_mid_high_filter.init();

    m_low_mid_filter.set_f_q<thl::dsp::utils::FrequencyApproximation::Dirty>(low_mid, 0.5f);
    m_mid_high_filter.set_f_q<thl::dsp::utils::FrequencyApproximation::Dirty>(mid_high, 0.5f);
    m_attack[0] = low_mid;
    m_decay[0] = sqrt(low_mid * low);

    m_attack[1] = sqrt(low_mid * mid_high);
    m_decay[1] = low_mid;

    m_attack[2] = sqrt(mid_high * 0.5f);
    m_decay[2] = sqrt(mid_high * low_mid);

    std::fill(&m_detector[0], &m_detector[3], 0.0f);

    m_centroid = 0.0f;
  }

  void process(
      float sample,
      float* envelope,
      float* centroid) {
    float bands[3] = { 0.0f, 0.0f, 0.0f };

    bands[2] = m_mid_high_filter.process<thl::dsp::utils::FilterMode::HighPass>(sample);
    bands[1] = m_low_mid_filter.process<thl::dsp::utils::FilterMode::HighPass>(
        m_mid_high_filter.lp());
    bands[0] = m_low_mid_filter.lp();

    float weighted = 0.0f;
    float total = 0.0f;
    float frequency = 0.0f;
    for (int32_t i = 0; i < 3; ++i) {
      SLOPE(m_detector[i], fabs(bands[i]), m_attack[i], m_decay[i]);
      weighted += m_detector[i] * frequency;
      total += m_detector[i];
      frequency += 0.5f;
    }

    float error = weighted / (total + 0.001f) - m_centroid;
    float coefficient = error > 0.0f ? 0.05f : 0.001f;
    m_centroid += error * coefficient;

    *envelope = total;
    *centroid = m_centroid;
  }

 private:
  NaiveSvf m_low_mid_filter;
  NaiveSvf m_mid_high_filter;

  float m_attack[3];
  float m_decay[3];
  float m_detector[3];

  float m_centroid;

  Follower(const Follower&) = delete;
  Follower& operator=(const Follower&) = delete;
};

}  // namespace thl::dsp::resonator::rings
