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
// Limiter.

#pragma once

#include <algorithm>

#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/utils/DspMath.h>

namespace thl::dsp::utils {

class SoftLimiter {
public:
    SoftLimiter() {}
    ~SoftLimiter() {}

    void prepare() { m_peak = 0.5f; }

    void process(thl::dsp::audio::AudioBufferView stereo, float pre_gain) {
        float* l = stereo.get_write_pointer(0);
        float* r = stereo.get_write_pointer(1);
        size_t size = stereo.get_num_frames();
        while (size--) {
            float l_pre = *l * pre_gain;
            float r_pre = *r * pre_gain;

            float l_peak = fabs(l_pre);
            float r_peak = fabs(r_pre);
            float s_peak = fabs(r_pre - l_pre);

            float peak = std::max(std::max(l_peak, r_peak), s_peak);
            thl::dsp::utils::slope<float>(m_peak, peak, 0.05f, 0.00002f);

            // Clamp to 8Vpp, clipping softly towards 10Vpp
            float gain = (m_peak <= 1.0f ? 1.0f : 1.0f / m_peak);
            *l++ = soft_limit(l_pre * gain * 0.8f);
            *r++ = soft_limit(r_pre * gain * 0.8f);
        }
    }

private:
    float m_peak = 0.5f;

    SoftLimiter(const SoftLimiter&) = delete;
    SoftLimiter& operator=(const SoftLimiter&) = delete;
};

}  // namespace thl::dsp::utils
