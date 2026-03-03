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

#include <tanh/dsp/filter/Svf.h>

#include <tanh/dsp/resonator/rings/Dsp.h>
#include <tanh/dsp/resonator/rings/fx/Chorus.h>
#include <tanh/dsp/resonator/rings/fx/Ensemble.h>
#include <tanh/dsp/resonator/rings/fx/Reverb.h>
#include <tanh/dsp/utils/SoftLimiter.h>
#include <tanh/dsp/analysis/NoteFilter.h>
#include <tanh/dsp/resonator/rings/Patch.h>
#include <tanh/dsp/resonator/rings/PerformanceState.h>
#include <tanh/dsp/utils/StringSynthEnvelope.h>
#include <tanh/dsp/synth/StringSynthVoice.h>

namespace thl::dsp::resonator::rings {

const int32_t kMaxStringSynthPolyphony = 4;
const int32_t kStringSynthVoices = 12;
const int32_t kMaxChordSize = 8;
const int32_t kNumHarmonics = 3;
const int32_t kNumFormants = 3;

enum FxType { FX_FORMANT, FX_CHORUS, FX_REVERB, FX_FORMANT_2, FX_ENSEMBLE, FX_REVERB_2, FX_LAST };

// Per-polyphony-group state: the base pitch, chord selection, AD envelope,
// and structure snapshot captured at note-on time.
struct VoiceGroup {
    float tonic;
    thl::dsp::utils::StringSynthEnvelope envelope;
    int32_t chord;
    float structure;
};

// Additive string synthesiser with chord voicing and effects routing.
//
// Unlike the physical-model String class (KS delay-line feedback), this is a
// purely additive engine: banks of StringSynthVoice oscillators render sine +
// cosine harmonic pairs whose amplitudes are set by an organ-stop-style
// "registration" table (11 presets, interpolated by the brightness parameter).
//
// Signal flow:
//   1. Note allocation -- incoming notes are assigned to polyphonic groups
//      (1-4) via round-robin.  A NoteFilter smooths pitch transitions and
//      handles strum/gate logic.
//   2. Chord expansion -- each group's tonic is expanded into up to
//      kMaxChordSize notes using a chord interval table indexed by chord
//      number and polyphony count.
//   3. Registration -- the brightness parameter interpolates between harmonic
//      amplitude presets (from pure fundamental through various overtone
//      combinations).
//   4. Envelopes -- each group has an AD envelope controlled by the damping
//      parameter (maps to attack/decay times).  Near-maximum damping
//      crossfades into a drone mode.
//   5. Effects -- the position parameter feeds one of six selectable
//      post-effects: formant filter (3-band SVF band-pass), chorus, ensemble,
//      or reverb (two variants each for formant and reverb).
//   6. Output -- a limiter normalises the stereo pair; aux is inverted to
//      prevent cancellation when summed externally.
class StringSynthPart {
public:
    StringSynthPart() {}
    ~StringSynthPart() {}

    void prepare(uint16_t* reverb_buffer, float sample_rate = kDefaultSampleRate);

    void process(const PerformanceState& performance_state,
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
        if (m_active_group >= m_polyphony) { m_active_group = 0; }
    }

    inline void set_fx(FxType fx_type) {
        if ((fx_type % 3) != (m_fx_type % 3)) { m_clear_fx = true; }
        m_fx_type = fx_type;
    }

private:
    // Run AD envelopes for each polyphonic group.  `shape` controls attack/
    // decay times and drone crossfade; results are written to `values`.
    void process_envelopes(float shape, uint8_t* flags, float* values);

    // Interpolate between organ-stop registration presets and scale by `gain`.
    // Writes kNumHarmonics * 2 amplitudes (sine + cosine pairs).
    void compute_registration(float gain, float registration, float* amplitudes);

    // Three-band formant filter (vowel selection + frequency shift).  Mixes
    // out + aux into a mono buffer, runs three SVF band-passes, and writes
    // the panned result back to out/aux.
    void process_formant_filter(float vowel,
                                float shift,
                                float resonance,
                                float* out,
                                float* aux,
                                size_t size);

    thl::dsp::synth::StringSynthVoice<kNumHarmonics> m_voice[kStringSynthVoices];
    VoiceGroup m_group[kMaxStringSynthPolyphony];

    thl::dsp::filter::Svf m_formant_filter[kNumFormants];
    Ensemble m_ensemble;
    Reverb m_reverb;
    Chorus m_chorus;
    thl::dsp::utils::SoftLimiter m_limiter;

    float m_sample_rate = kDefaultSampleRate;
    float m_a3 = 440.0f / kDefaultSampleRate;

    int32_t m_num_voices = 0;
    int32_t m_active_group = 0;
    uint32_t m_step_counter = 0;
    int32_t m_polyphony = 1;
    int32_t m_acquisition_delay = 0;

    FxType m_fx_type = FX_ENSEMBLE;

    thl::dsp::analysis::NoteFilter m_note_filter;

    float m_filter_in_buffer[kMaxBlockSize];
    float m_filter_out_buffer[kMaxBlockSize];

    bool m_clear_fx = false;

    StringSynthPart(const StringSynthPart&) = delete;
    StringSynthPart& operator=(const StringSynthPart&) = delete;
};

}  // namespace thl::dsp::resonator::rings
