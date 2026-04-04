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
// Group of voices.

#pragma once

#include <algorithm>
#include <array>

#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/utils/CosineOscillator.h>
#include <tanh/dsp/filter/DCBlocker.h>
#include <tanh/dsp/utils/DelayLine.h>

#include <tanh/dsp/rings-resonator/RingsDsp.h>
#include <tanh/dsp/rings-resonator/RingsFmVoice.h>
#include <tanh/dsp/rings-resonator/fx/RingsReverb.h>
#include <tanh/dsp/utils/SoftLimiter.h>
#include <tanh/dsp/analysis/NoteFilter.h>
#include <tanh/dsp/rings-resonator/RingsPatch.h>
#include <tanh/dsp/rings-resonator/RingsPerformanceState.h>
#include <tanh/dsp/rings-resonator/RingsPlucker.h>
#include <tanh/dsp/rings-resonator/RingsModalResonator.h>
#include <tanh/dsp/rings-resonator/RingsString.h>

namespace thl::dsp::synth {

using thl::dsp::resonator::FmVoice;
using thl::dsp::resonator::Last;
using thl::dsp::resonator::Modal;
using thl::dsp::resonator::ResonatorModel;
using thl::dsp::resonator::String;
using thl::dsp::resonator::StringAndReverb;
using thl::dsp::resonator::SympatheticString;
using thl::dsp::resonator::SympatheticStringQuantized;

const int32_t k_max_polyphony = 4;
const int32_t k_num_strings = k_max_polyphony * 2;

struct PreparedVoiceParams {
    float m_frequency;
    float m_filter_cutoff;
    float m_filter_q;
};

class RingsVoiceManager {
public:
    RingsVoiceManager() {}
    ~RingsVoiceManager() {}

    void prepare(uint16_t* reverb_buffer,
                 float sample_rate = thl::dsp::resonator::k_default_sample_rate);

    void process(const thl::dsp::resonator::RingsPerformanceState& performance_state,
                 const thl::dsp::resonator::RingsPatch& patch,
                 const thl::dsp::audio::ConstAudioBufferView& in,
                 thl::dsp::audio::AudioBufferView out,
                 thl::dsp::audio::AudioBufferView aux);

    inline bool bypass() const { return m_bypass; }
    inline void set_bypass(bool bypass) { m_bypass = bypass; }

    inline int32_t polyphony() const { return m_polyphony; }
    inline void set_polyphony(int32_t polyphony) {
        int32_t old_polyphony = m_polyphony;
        m_polyphony = std::min(polyphony, k_max_polyphony);
        for (int32_t i = old_polyphony; i < m_polyphony; ++i) { m_note[i] = m_note[0] + i * 0.05f; }
        m_dirty = true;
    }

    inline ResonatorModel model() const { return m_model; }
    inline void set_model(ResonatorModel model) {
        if (model != m_model) {
            m_model = model;
            m_dirty = true;
        }
    }

private:
    void configure_resonators();
    void prepare_voice_params(const thl::dsp::resonator::RingsPerformanceState& performance_state,
                              const thl::dsp::resonator::RingsPatch& patch);
    void render_modal_voice(int32_t voice,
                            const thl::dsp::resonator::RingsPerformanceState& performance_state,
                            const thl::dsp::resonator::RingsPatch& patch,
                            float frequency,
                            float filter_cutoff,
                            size_t size);
    void render_string_voice(int32_t voice,
                             const thl::dsp::resonator::RingsPerformanceState& performance_state,
                             const thl::dsp::resonator::RingsPatch& patch,
                             float frequency,
                             float filter_cutoff,
                             size_t size);
    void render_fm_voice(int32_t voice,
                         const thl::dsp::resonator::RingsPerformanceState& performance_state,
                         const thl::dsp::resonator::RingsPatch& patch,
                         float frequency,
                         float filter_cutoff,
                         size_t size);

    inline float squash(float x) const {
        if (x < 0.5f) {
            x *= 2.0f;
            x *= x;
            x *= x;
            x *= x;
            x *= x;
            x *= 0.5f;
        } else {
            x = 2.0f - 2.0f * x;
            x *= x;
            x *= x;
            x *= x;
            x *= x;
            x = 1.0f - 0.5f * x;
        }
        return x;
    }

    void compute_sympathetic_strings_notes(float tonic,
                                           float note,
                                           float parameter,
                                           float* destination,
                                           size_t num_strings);

    float m_sample_rate = thl::dsp::resonator::k_default_sample_rate;
    float m_a3 = 440.0f / thl::dsp::resonator::k_default_sample_rate;

    bool m_bypass = false;
    bool m_dirty = true;

    ResonatorModel m_model = Modal;

    int32_t m_num_voices = 0;
    int32_t m_active_voice = 0;
    uint32_t m_step_counter = 0;
    int32_t m_polyphony = 1;

    thl::dsp::resonator::RingsModalResonator m_resonator[k_max_polyphony];
    thl::dsp::resonator::RingsString m_string[k_num_strings];
    thl::dsp::utils::CosineOscillator m_lfo[k_num_strings];
    RingsFmVoice m_fm_voice[k_max_polyphony];

    thl::dsp::filter::Svf m_excitation_filter[k_max_polyphony];
    thl::dsp::filter::DCBlocker m_dc_blocker[k_max_polyphony];
    thl::dsp::synth::Plucker m_plucker[k_max_polyphony];

    float m_note[k_max_polyphony];
    PreparedVoiceParams m_prepared[k_max_polyphony];
    thl::dsp::analysis::NoteFilter m_note_filter;

    float m_resonator_input[thl::dsp::resonator::k_max_block_size];
    float m_sympathetic_resonator_input[thl::dsp::resonator::k_max_block_size];
    float m_noise_burst_buffer[thl::dsp::resonator::k_max_block_size];

    float m_out_buffer[thl::dsp::resonator::k_max_block_size];
    float m_aux_buffer[thl::dsp::resonator::k_max_block_size];

    thl::dsp::fx::RingsReverb m_reverb;
    thl::dsp::utils::SoftLimiter m_limiter;

    static std::array<float, Last> m_model_gains;

    RingsVoiceManager(const RingsVoiceManager&) = delete;
    RingsVoiceManager& operator=(const RingsVoiceManager&) = delete;
};

}  // namespace thl::dsp::synth
