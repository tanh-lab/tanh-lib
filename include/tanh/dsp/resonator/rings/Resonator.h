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

#pragma once


#include <algorithm>

#include <tanh/dsp/resonator/rings/Dsp.h>
#include <tanh/dsp/utils/Svf.h>
#include <tanh/dsp/utils/DelayLine.h>

namespace thl::dsp::resonator::rings {

const int32_t kMaxModes = 64;

class Resonator {
 public:
  Resonator() { }
  ~Resonator() { }

  void init(float sample_rate = kDefaultSampleRate);
  void process(
      const float* in,
      float* out,
      float* aux,
      size_t size);

  inline void set_frequency(float frequency) {
    m_frequency = frequency;
    m_dirty = true;
  }

  inline void set_structure(float structure) {
    m_structure = structure;
    m_dirty = true;
  }

  inline void set_brightness(float brightness) {
    m_brightness = brightness;
    m_dirty = true;
  }

  inline void set_damping(float damping) {
    m_damping = damping;
    m_dirty = true;
  }

  inline void set_position(float position) {
    m_position = position;
  }

  inline void set_resolution(int32_t resolution) {
    resolution -= resolution & 1; // Must be even!
    m_resolution = std::min(resolution, kMaxModes);
    m_dirty = true;
  }

 private:
  int32_t compute_filters();
  float m_sample_rate = kDefaultSampleRate;
  float m_frequency;
  float m_structure;
  float m_brightness;
  float m_position;
  float m_previous_position;
  float m_damping;

  int32_t m_resolution;
  int32_t m_num_modes = 0;
  bool m_dirty = true;

  thl::dsp::utils::Svf m_f[kMaxModes];

  Resonator(const Resonator&) = delete;
  Resonator& operator=(const Resonator&) = delete;
};

}  // namespace thl::dsp::resonator::rings
