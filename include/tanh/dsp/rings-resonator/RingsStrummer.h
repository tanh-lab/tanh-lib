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
// Strumming logic.

#pragma once

#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/analysis/OnsetDetector.h>
#include <tanh/dsp/rings-resonator/RingsPerformanceState.h>

#include <cmath>

namespace thl::dsp::resonator {

class RingsStrummer {
public:
    RingsStrummer() = default;
    ~RingsStrummer() = default;

    void prepare(float ioi, float decimated_sr, float sample_rate) {
        m_onset_detector.prepare(8.0f / sample_rate,
                                 160.0f / sample_rate,
                                 1600.0f / sample_rate,
                                 decimated_sr,
                                 ioi);
        m_inhibit_timer = static_cast<int32_t>(ioi * decimated_sr);
        m_inhibit_counter = 0;
        m_previous_note = 69.0f;
    }

    void process(const thl::dsp::audio::ConstAudioBufferView& in,
                 RingsPerformanceState* performance_state) {
        const bool has_onset = in.get_num_frames() > 0 && m_onset_detector.process(in);
        const bool note_changed = std::fabs(performance_state->m_note - m_previous_note) > 0.4f;

        int32_t inhibit_timer = m_inhibit_timer;
        if (performance_state->m_internal_strum) {
            const bool has_external_note_cv = !performance_state->m_internal_note;
            const bool has_external_exciter = !performance_state->m_internal_exciter;
            if (has_external_note_cv) {
                performance_state->m_strum = note_changed;
            } else if (has_external_exciter) {
                performance_state->m_strum = has_onset;
                // Use longer inhibit time for onset detector.
                inhibit_timer *= 4;
            } else {
                // Nothing is connected. Should the module play itself in this
                // case?
                performance_state->m_strum = false;
            }
        }

        if (m_inhibit_counter) {
            --m_inhibit_counter;
            performance_state->m_strum = false;
        } else {
            if (performance_state->m_strum) { m_inhibit_counter = inhibit_timer; }
        }
        m_previous_note = performance_state->m_note;
    }

    RingsStrummer(const RingsStrummer&) = delete;
    RingsStrummer& operator=(const RingsStrummer&) = delete;

private:
    float m_previous_note = 69.0f;
    int32_t m_inhibit_counter = 0;
    int32_t m_inhibit_timer = 0;

    thl::dsp::analysis::OnsetDetector m_onset_detector;
};

}  // namespace thl::dsp::resonator
