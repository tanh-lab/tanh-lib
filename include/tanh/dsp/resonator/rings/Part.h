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

#include <tanh/dsp/utils/CosineOscillator.h>
#include <tanh/dsp/utils/DelayLine.h>

#include <tanh/dsp/resonator/rings/Dsp.h>
#include <tanh/dsp/resonator/rings/FmVoice.h>
#include <tanh/dsp/resonator/rings/fx/Reverb.h>
#include <tanh/dsp/resonator/rings/Limiter.h>
#include <tanh/dsp/resonator/rings/NoteFilter.h>
#include <tanh/dsp/resonator/rings/Patch.h>
#include <tanh/dsp/resonator/rings/PerformanceState.h>
#include <tanh/dsp/resonator/rings/Plucker.h>
#include <tanh/dsp/resonator/rings/Resonator.h>
#include <tanh/dsp/resonator/rings/String.h>

namespace thl::dsp::resonator::rings {

enum ResonatorModel {
  RESONATOR_MODEL_MODAL,
  RESONATOR_MODEL_SYMPATHETIC_STRING,
  RESONATOR_MODEL_STRING,

  // Bonus!
  RESONATOR_MODEL_FM_VOICE,
  RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED,
  RESONATOR_MODEL_STRING_AND_REVERB,
  RESONATOR_MODEL_LAST
};

const int32_t kMaxPolyphony = 4;
const int32_t kNumStrings = kMaxPolyphony * 2;

struct PreparedVoiceParams {
  float frequency;
  float filter_cutoff;
  float filter_q;
};

class Part {
 public:
  Part() { }
  ~Part() { }

  void init(uint16_t* reverb_buffer, float sample_rate = kDefaultSampleRate);

  void process(
      const PerformanceState& performance_state,
      const Patch& patch,
      const float* in,
      float* out,
      float* aux,
      size_t size);

  inline bool bypass() const { return m_bypass; }
  inline void set_bypass(bool bypass) { m_bypass = bypass; }

  inline int32_t polyphony() const { return m_polyphony; }
  inline void set_polyphony(int32_t polyphony) {
    int32_t old_polyphony = m_polyphony;
    m_polyphony = std::min(polyphony, kMaxPolyphony);
    for (int32_t i = old_polyphony; i < m_polyphony; ++i) {
      m_note[i] = m_note[0] + i * 0.05f;
    }
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
  void prepare_voice_params(
      const PerformanceState& performance_state,
      const Patch& patch);
  void render_modal_voice(
      int32_t voice,
      const PerformanceState& performance_state,
      const Patch& patch,
      float frequency,
      float filter_cutoff,
      size_t size);
  void render_string_voice(
      int32_t voice,
      const PerformanceState& performance_state,
      const Patch& patch,
      float frequency,
      float filter_cutoff,
      size_t size);
  void render_fm_voice(
      int32_t voice,
      const PerformanceState& performance_state,
      const Patch& patch,
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

  void compute_sympathetic_strings_notes(
      float tonic,
      float note,
      float parameter,
      float* destination,
      size_t num_strings);

  float m_sample_rate = kDefaultSampleRate;
  float m_a3 = 440.0f / kDefaultSampleRate;

  bool m_bypass;
  bool m_dirty;

  ResonatorModel m_model;

  int32_t m_num_voices;
  int32_t m_active_voice;
  uint32_t m_step_counter;
  int32_t m_polyphony;

  Resonator m_resonator[kMaxPolyphony];
  String m_string[kNumStrings];
  thl::dsp::utils::CosineOscillator m_lfo[kNumStrings];
  FMVoice m_fm_voice[kMaxPolyphony];

  thl::dsp::utils::Svf m_excitation_filter[kMaxPolyphony];
  thl::dsp::utils::DCBlocker m_dc_blocker[kMaxPolyphony];
  Plucker m_plucker[kMaxPolyphony];

  float m_note[kMaxPolyphony];
  PreparedVoiceParams m_prepared[kMaxPolyphony];
  NoteFilter m_note_filter;

  float m_resonator_input[kMaxBlockSize];
  float m_sympathetic_resonator_input[kMaxBlockSize];
  float m_noise_burst_buffer[kMaxBlockSize];

  float m_out_buffer[kMaxBlockSize];
  float m_aux_buffer[kMaxBlockSize];

  Reverb m_reverb;
  Limiter m_limiter;

  static float m_model_gains[RESONATOR_MODEL_LAST];

  Part(const Part&) = delete;
  Part& operator=(const Part&) = delete;
};

}  // namespace thl::dsp::resonator::rings
