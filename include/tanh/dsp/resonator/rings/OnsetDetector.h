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
// Onset detector.

#pragma once


#include <algorithm>

#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/Svf.h>

namespace thl::dsp::resonator::rings {

using namespace std;
using namespace thl::dsp::utils;

class ZScorer {
 public:
  ZScorer() { }
  ~ZScorer() { }

  void init(float cutoff) {
    m_coefficient = cutoff;
    m_mean = 0.0f;
    m_variance = 0.00f;
  }

  inline float normalize(float sample) {
    return update(sample) / thl::dsp::utils::sqrt(m_variance);
  }

  inline bool test(float sample, float threshold) {
    float value = update(sample);
    return value > thl::dsp::utils::sqrt(m_variance) * threshold;
  }

  inline bool test(float sample, float threshold, float absolute_threshold) {
    float value = update(sample);
    return value > thl::dsp::utils::sqrt(m_variance) * threshold && value > absolute_threshold;
  }

 private:
  inline float update(float sample) {
    float centered = sample - m_mean;
    m_mean += m_coefficient * centered;
    m_variance += m_coefficient * (centered * centered - m_variance);
    return centered;
  }

  float m_coefficient;
  float m_mean;
  float m_variance;

  ZScorer(const ZScorer&) = delete;
  ZScorer& operator=(const ZScorer&) = delete;
};

class Compressor {
 public:
  Compressor() { }
  ~Compressor() { }

  void init(float attack, float decay, float max_gain) {
    m_attack = attack;
    m_decay = decay;
    m_level = 0.0f;
    m_skew = 1.0f / max_gain;
  }

  void process(const float* in, float* out, size_t size) {
    float level = m_level;
    while (size--) {
      SLOPE(level, fabs(*in), m_attack, m_decay);
      *out++ = *in++ / (m_skew + level);
    }
    m_level = level;
  }

 private:
  float m_attack;
  float m_decay;
  float m_level;
  float m_skew;

  Compressor(const Compressor&) = delete;
  Compressor& operator=(const Compressor&) = delete;
};

class OnsetDetector {
 public:
  OnsetDetector() { }
  ~OnsetDetector() { }

  void init(
      float low,
      float low_mid,
      float mid_high,
      float decimated_sr,
      float ioi_time) {
    float ioi_f = 1.0f / (ioi_time * decimated_sr);
    m_compressor.init(ioi_f * 10.0f, ioi_f * 0.05f, 40.0f);

    m_low_mid_filter.init();
    m_mid_high_filter.init();
    m_low_mid_filter.set_f_q<thl::dsp::utils::FrequencyApproximation::Dirty>(low_mid, 0.5f);
    m_mid_high_filter.set_f_q<thl::dsp::utils::FrequencyApproximation::Dirty>(mid_high, 0.5f);

    m_attack[0] = low_mid;
    m_decay[0] = low * 0.25f;

    m_attack[1] = low_mid;
    m_decay[1] = low * 0.25f;

    m_attack[2] = low_mid;
    m_decay[2] = low * 0.25f;

    fill(&m_envelope[0], &m_envelope[3], 0.0f);
    fill(&m_energy[0], &m_energy[3], 0.0f);

    m_z_df.init(ioi_f * 0.05f);

    m_inhibit_time = static_cast<int32_t>(ioi_time * decimated_sr);
    m_inhibit_decay = 1.0f / (ioi_time * decimated_sr);

    m_inhibit_threshold = 0.0f;
    m_inhibit_counter = 0;
    m_onset_df = 0.0f;
  }

  bool process(const float* samples, size_t size) {
    // Automatic gain control.
    m_compressor.process(samples, m_bands[0], size);

    // Quick and dirty filter bank - split the signal in three bands.
    m_mid_high_filter.split(m_bands[0], m_bands[1], m_bands[2], size);
    m_low_mid_filter.split(m_bands[1], m_bands[0], m_bands[1], size);

    // Compute low-pass energy and onset detection function
    // (derivative of energy) in each band.
    float onset_df = 0.0f;
    float total_energy = 0.0f;
    for (int32_t i = 0; i < 3; ++i) {
      float* s = m_bands[i];
      float energy = 0.0f;
      float envelope = m_envelope[i];
      size_t increment = 4 >> i;
      for (size_t j = 0; j < size; j += increment) {
        SLOPE(envelope, s[j] * s[j], m_attack[i], m_decay[i]);
        energy += envelope;
      }
      energy = thl::dsp::utils::sqrt(energy) * float(increment);
      m_envelope[i] = envelope;

      float derivative = energy - m_energy[i];
      onset_df += derivative + fabs(derivative);
      m_energy[i] = energy;
      total_energy += energy;
    }

    m_onset_df += 0.05f * (onset_df - m_onset_df);
    bool outlier_in_df = m_z_df.test(m_onset_df, 1.0f, 0.01f);
    bool exceeds_energy_threshold = total_energy >= m_inhibit_threshold;
    bool not_inhibited = !m_inhibit_counter;
    bool has_onset = outlier_in_df && exceeds_energy_threshold && not_inhibited;

    if (has_onset) {
      m_inhibit_threshold = total_energy * 1.5f;
      m_inhibit_counter = m_inhibit_time;
    } else {
      m_inhibit_threshold -= m_inhibit_decay * m_inhibit_threshold;
      if (m_inhibit_counter) {
        --m_inhibit_counter;
      }
    }
    return has_onset;
  }

 private:
  Compressor m_compressor;
  NaiveSvf m_low_mid_filter;
  NaiveSvf m_mid_high_filter;

  float m_attack[3];
  float m_decay[3];
  float m_energy[3];
  float m_envelope[3];
  float m_onset_df;

  float m_bands[3][32];

  ZScorer m_z_df;

  float m_inhibit_threshold;
  float m_inhibit_decay;
  int32_t m_inhibit_time;
  int32_t m_inhibit_counter;

  OnsetDetector(const OnsetDetector&) = delete;
  OnsetDetector& operator=(const OnsetDetector&) = delete;
};

}  // namespace thl::dsp::resonator::rings
