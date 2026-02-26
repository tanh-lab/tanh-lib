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
// Part for the string synth easter egg.

#pragma once


#include <tanh/dsp/utils/Svf.h>

#include <tanh/dsp/resonator/rings/Dsp.h>
#include <tanh/dsp/resonator/rings/fx/Chorus.h>
#include <tanh/dsp/resonator/rings/fx/Ensemble.h>
#include <tanh/dsp/resonator/rings/fx/Reverb.h>
#include <tanh/dsp/resonator/rings/Limiter.h>
#include <tanh/dsp/resonator/rings/NoteFilter.h>
#include <tanh/dsp/resonator/rings/Patch.h>
#include <tanh/dsp/resonator/rings/PerformanceState.h>
#include <tanh/dsp/resonator/rings/StringSynthEnvelope.h>
#include <tanh/dsp/resonator/rings/StringSynthVoice.h>

namespace thl::dsp::resonator::rings {

const int32_t kMaxStringSynthPolyphony = 4;
const int32_t kStringSynthVoices = 12;
const int32_t kMaxChordSize = 8;
const int32_t kNumHarmonics = 3;
const int32_t kNumFormants = 3;

enum FxType {
  FX_FORMANT,
  FX_CHORUS,
  FX_REVERB,
  FX_FORMANT_2,
  FX_ENSEMBLE,
  FX_REVERB_2,
  FX_LAST
};

struct VoiceGroup {
  float tonic;
  StringSynthEnvelope envelope;
  int32_t chord;
  float structure;
};

class StringSynthPart {
 public:
  StringSynthPart() { }
  ~StringSynthPart() { }

  void init(uint16_t* reverb_buffer, float sample_rate = kDefaultSampleRate);

  void process(
      const PerformanceState& performance_state,
      const Patch& patch,
      const float* in,
      float* out,
      float* aux,
      size_t size);

  inline void set_polyphony(int32_t polyphony) {
    int32_t old_polyphony = m_polyphony;
    m_polyphony = std::min(polyphony, kMaxStringSynthPolyphony);
    for (int32_t i = old_polyphony; i < m_polyphony; ++i) {
      m_group[i].tonic = m_group[0].tonic + i * 0.01f;
    }
    if (m_active_group >= m_polyphony) {
      m_active_group = 0;
    }
  }

  inline void set_fx(FxType fx_type) {
    if ((fx_type % 3) != (m_fx_type % 3)) {
      m_clear_fx = true;
    }
    m_fx_type = fx_type;
  }

 private:
  void process_envelopes(float shape, uint8_t* flags, float* values);
  void compute_registration(float gain, float registration, float* amplitudes);

  void process_formant_filter(float vowel, float shift, float resonance,
                              float* out, float* aux, size_t size);

  StringSynthVoice<kNumHarmonics> m_voice[kStringSynthVoices];
  VoiceGroup m_group[kMaxStringSynthPolyphony];

  thl::dsp::utils::Svf m_formant_filter[kNumFormants];
  Ensemble m_ensemble;
  Reverb m_reverb;
  Chorus m_chorus;
  Limiter m_limiter;

  float m_sample_rate = kDefaultSampleRate;
  float m_a3 = 440.0f / kDefaultSampleRate;

  int32_t m_num_voices;
  int32_t m_active_group;
  uint32_t m_step_counter;
  int32_t m_polyphony;
  int32_t m_acquisition_delay;

  FxType m_fx_type;

  NoteFilter m_note_filter;

  float m_filter_in_buffer[kMaxBlockSize];
  float m_filter_out_buffer[kMaxBlockSize];

  bool m_clear_fx;

  StringSynthPart(const StringSynthPart&) = delete;
  StringSynthPart& operator=(const StringSynthPart&) = delete;
};

}  // namespace thl::dsp::resonator::rings
