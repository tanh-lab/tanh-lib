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
// String synth part.

#include <tanh/dsp/resonator/rings/StringSynthPart.h>

#include <tanh/dsp/resonator/rings/Dsp.h>

namespace thl::dsp::resonator::rings {

using namespace std;
using namespace thl::dsp::utils;

void StringSynthPart::init(uint16_t* reverb_buffer, float sample_rate) {
  m_sample_rate = sample_rate;
  m_a3 = 440.0f / m_sample_rate;
  m_active_group = 0;
  m_acquisition_delay = 0;

  m_polyphony = 1;
  m_fx_type = FX_ENSEMBLE;

  for (int32_t i = 0; i < kStringSynthVoices; ++i) {
    m_voice[i].init();
  }

  for (int32_t i = 0; i < kMaxStringSynthPolyphony; ++i) {
    m_group[i].tonic = 0.0f;
    m_group[i].envelope.init();
  }

  for (int32_t i = 0; i < kNumFormants; ++i) {
    m_formant_filter[i].init();
  }

  m_limiter.init();

  m_reverb.init(reverb_buffer, m_sample_rate);
  m_chorus.init(reverb_buffer, m_sample_rate);
  m_ensemble.init(reverb_buffer, m_sample_rate);

  m_note_filter.init(
      m_sample_rate / kMaxBlockSize,
      0.001f,  // Lag time with a sharp edge on the V/Oct input or trigger.
      0.005f,  // Lag time after the trigger has been received.
      0.050f,  // Time to transition from reactive to filtered.
      0.004f); // Prevent a sharp edge to partly leak on the previous voice.
}

const int32_t kRegistrationTableSize = 11;
const float registrations[kRegistrationTableSize][kNumHarmonics * 2] = {
  { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
  { 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f },
  { 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f },
  { 1.0f, 0.1f, 0.0f, 0.0f, 1.0f, 0.0f },
  { 1.0f, 0.5f, 1.0f, 0.0f, 1.0f, 0.0f },
  { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
  { 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f },
  { 0.0f, 0.5f, 1.0f, 0.0f, 1.0f, 0.0f },
  { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f },
  { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f },
  { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f },
};

void StringSynthPart::compute_registration(
    float gain,
    float registration,
    float* amplitudes) {
  registration *= (kRegistrationTableSize - 1.001f);
  MAKE_INTEGRAL_FRACTIONAL(registration);
  float total = 0.0f;
  for (int32_t i = 0; i < kNumHarmonics * 2; ++i) {
    float a = registrations[registration_integral][i];
    float b = registrations[registration_integral + 1][i];
    amplitudes[i] = a + (b - a) * registration_fractional;
    total += amplitudes[i];
  }
  for (int32_t i = 0; i < kNumHarmonics * 2; ++i) {
    amplitudes[i] = gain * amplitudes[i] / total;
  }
}

#ifdef BRYAN_CHORDS

// Chord table by Bryan Noll:
// - more compact, leaving room for a bass
// - more frequent note changes between adjacent chords.
// - dropped fifth.
const float chords[kMaxStringSynthPolyphony][kNumChords][kMaxChordSize] = {
  {
    { -12.0f, -0.01f,  0.0f,  0.01f,  0.02f, 11.99f, 12.0f, 24.0f }, // OCT
    { -12.0f, -5.01f, -5.0f,  0.0f,   7.0f,  12.0f,  19.0f, 24.0f }, // 5
    { -12.0f, -5.0f,   0.0f,  5.0f,   7.0f,  12.0f,  17.0f, 24.0f }, // sus4
    { -12.0f, -5.0f,   0.0f,  0.01f,  3.0f,  12.0f,  19.0f, 24.0f }, // m
    { -12.0f, -5.01f, -5.0f,  0.0f,   3.0f,  10.0f,  19.0f, 24.0f }, // m7
    { -12.0f, -5.0f,   0.0f,  3.0f,  10.0f,  14.0f,  19.0f, 24.0f }, // m9
    { -12.0f, -5.01f, -5.0f,  0.0f,   3.0f,  10.0f,  17.0f, 24.0f }, // m11
    { -12.0f, -5.0f,   0.0f,  2.0f,   9.0f,  16.0f,  19.0f, 24.0f }, // 69
    { -12.0f, -5.0f,   0.0f,  4.0f,  11.0f,  14.0f,  19.0f, 24.0f }, // M9
    { -12.0f, -5.0f,   0.0f,  4.0f,   7.0f,  11.0f,  19.0f, 24.0f }, // M7
    { -12.0f, -5.0f,   0.0f,  4.0f,   7.0f,  12.0f,  19.0f, 24.0f }, // M
  },
  {
    { -12.0f, -0.01f,  0.0f,  0.01f, 12.0f,  12.01f }, // OCT
    { -12.0f, -5.01f, -5.0f,  0.0f,   7.0f,  12.0f  }, // 5
    { -12.0f, -5.0f,   0.0f,  5.0f,   7.0f,  12.0f  }, // sus4
    { -12.0f, -5.0f,   0.0f,  0.01f,  3.0f,  12.0f  }, // m
    { -12.0f, -5.01f, -5.0f,  0.0f,   3.0f,  10.0f  }, // m7
    { -12.0f, -5.0f,   0.0f,  3.0f,  10.0f,  14.0f  }, // m9
    { -12.0f, -5.0f,   0.0f,  3.0f,  10.0f,  17.0f  }, // m11
    { -12.0f, -5.0f,   0.0f,  2.0f,   9.0f,  16.0f  }, // 69
    { -12.0f, -5.0f,   0.0f,  4.0f,  11.0f,  14.0f  }, // M9
    { -12.0f, -5.0f,   0.0f,  4.0f,   7.0f,  11.0f  }, // M7
    { -12.0f, -5.0f,   0.0f,  4.0f,   7.0f,  12.0f  }, // M
  },
  {
    { -12.0f, 0.0f,  0.01f, 12.0f }, // OCT
    { -12.0f, 6.99f, 7.0f,  12.0f }, // 5
    { -12.0f, 5.0f,  7.0f,  12.0f }, // sus4
    { -12.0f, 3.0f, 11.99f, 12.0f }, // m
    { -12.0f, 3.0f,  9.99f, 10.0f }, // m7
    { -12.0f, 3.0f, 10.0f,  14.0f }, // m9
    { -12.0f, 3.0f, 10.0f,  17.0f }, // m11
    { -12.0f, 2.0f,  9.0f,  16.0f }, // 69
    { -12.0f, 4.0f, 11.0f,  14.0f }, // M9
    { -12.0f, 4.0f,  7.0f,  11.0f }, // M7
    { -12.0f, 4.0f,  7.0f,  12.0f }, // M
  },
  {
    { 0.0f,  0.01f, 12.0f }, // OCT
    { 0.0f,  7.0f,  12.0f }, // 5
    { 5.0f,  7.0f,  12.0f }, // sus4
    { 0.0f,  3.0f,  12.0f }, // m
    { 0.0f,  3.0f,  10.0f }, // m7
    { 3.0f, 10.0f,  14.0f }, // m9
    { 3.0f, 10.0f,  17.0f }, // m11
    { 2.0f,  9.0f,  16.0f }, // 69
    { 4.0f, 11.0f,  14.0f }, // M9
    { 4.0f,  7.0f,  11.0f }, // M7
    { 4.0f,  7.0f,  12.0f }, // M
  }
};

#else

// Original chord table:
// - wider, occupies more room in the spectrum.
// - minimum number of note changes between adjacent chords.
// - consistant with the chord table used for the sympathetic strings model.
const float chords[kMaxStringSynthPolyphony][kNumChords][kMaxChordSize] = {
  {
    { -24.0f, -12.0f, 0.0f, 0.01f, 0.02f, 11.99f, 12.0f, 24.0f },
    { -24.0f, -12.0f, 0.0f, 3.0f,  7.0f,  10.0f,  19.0f, 24.0f },
    { -24.0f, -12.0f, 0.0f, 3.0f,  7.0f,  12.0f,  19.0f, 24.0f },
    { -24.0f, -12.0f, 0.0f, 3.0f,  7.0f,  14.0f,  19.0f, 24.0f },
    { -24.0f, -12.0f, 0.0f, 3.0f,  7.0f,  17.0f,  19.0f, 24.0f },
    { -24.0f, -12.0f, 0.0f, 6.99f, 7.0f,  18.99f, 19.0f, 24.0f },
    { -24.0f, -12.0f, 0.0f, 4.0f,  7.0f,  17.0f,  19.0f, 24.0f },
    { -24.0f, -12.0f, 0.0f, 4.0f,  7.0f,  14.0f,  19.0f, 24.0f },
    { -24.0f, -12.0f, 0.0f, 4.0f,  7.0f,  12.0f,  19.0f, 24.0f },
    { -24.0f, -12.0f, 0.0f, 4.0f,  7.0f,  11.0f,  19.0f, 24.0f },
    { -24.0f, -12.0f, 0.0f, 5.0f,  7.0f,  12.0f,  17.0f, 24.0f },
  },
  {
    { -24.0f, -12.0f, 0.0f, 0.01f, 12.0f, 12.01f },
    { -24.0f, -12.0f, 0.0f, 3.00f, 7.0f,  10.0f },
    { -24.0f, -12.0f, 0.0f, 3.00f, 7.0f,  12.0f },
    { -24.0f, -12.0f, 0.0f, 3.00f, 7.0f,  14.0f },
    { -24.0f, -12.0f, 0.0f, 3.00f, 7.0f,  17.0f },
    { -24.0f, -12.0f, 0.0f, 6.99f, 12.0f, 19.0f },
    { -24.0f, -12.0f, 0.0f, 4.00f, 7.0f,  17.0f },
    { -24.0f, -12.0f, 0.0f, 4.00f, 7.0f,  14.0f },
    { -24.0f, -12.0f, 0.0f, 4.00f, 7.0f,  12.0f },
    { -24.0f, -12.0f, 0.0f, 4.00f, 7.0f,  11.0f },
    { -24.0f, -12.0f, 0.0f, 5.00f, 7.0f, 12.0f },
  },
  {
    { -12.0f, 0.0f, 0.01f, 12.0f },
    { -12.0f, 3.0f, 7.0f,  10.0f },
    { -12.0f, 3.0f, 7.0f,  12.0f },
    { -12.0f, 3.0f, 7.0f,  14.0f },
    { -12.0f, 3.0f, 7.0f,  17.0f },
    { -12.0f, 7.0f, 12.0f, 19.0f },
    { -12.0f, 4.0f, 7.0f,  17.0f },
    { -12.0f, 4.0f, 7.0f,  14.0f },
    { -12.0f, 4.0f, 7.0f,  12.0f },
    { -12.0f, 4.0f, 7.0f,  11.0f },
    { -12.0f, 5.0f, 7.0f, 12.0f },
  },
  {
    { 0.0f, 0.01f, 12.0f },
    { 0.0f, 3.0f,  10.0f },
    { 0.0f, 3.0f,  7.0f },
    { 0.0f, 3.0f,  14.0f },
    { 0.0f, 3.0f,  17.0f },
    { 0.0f, 7.0f,  19.0f },
    { 0.0f, 4.0f,  17.0f },
    { 0.0f, 4.0f,  14.0f },
    { 0.0f, 4.0f,  7.0f },
    { 0.0f, 4.0f,  11.0f },
    { 0.0f, 5.0f,  7.0f },
  }
};

#endif  // BRYAN_CHORDS

void StringSynthPart::process_envelopes(
    float shape,
    uint8_t* flags,
    float* values) {
  float decay = shape;
  float attack = 0.0f;
  if (shape < 0.5f) {
    attack = 0.0f;
  } else {
    attack = (shape - 0.5f) * 2.0f;
  }

  // Convert the arbitrary values to actual units.
  float period = m_sample_rate / kMaxBlockSize;
  float attack_time = semitones_to_ratio(attack * 96.0f) * 0.005f * period;
  // float decay_time = semitones_to_ratio(decay * 96.0f) * 0.125f * period;
  float decay_time = semitones_to_ratio(decay * 84.0f) * 0.180f * period;
  float attack_rate = 1.0f / attack_time;
  float decay_rate = 1.0f / decay_time;

  for (int32_t i = 0; i < m_polyphony; ++i) {
    float drone = shape < 0.98f ? 0.0f : (shape - 0.98f) * 55.0f;
    if (drone >= 1.0f) drone = 1.0f;

    m_group[i].envelope.set_ad(attack_rate, decay_rate);
    float value = m_group[i].envelope.process(flags[i]);
    values[i] = value + (1.0f - value) * drone;
  }
}

const int32_t kFormantTableSize = 5;
const float formants[kFormantTableSize][kNumFormants] = {
  { 700, 1100, 2400 },
  { 500, 1300, 1700 },
  { 400, 2000, 2500 },
  { 600, 800, 2400 },
  { 300, 900, 2200 },
};

void StringSynthPart::process_formant_filter(
    float vowel,
    float shift,
    float resonance,
    float* out,
    float* aux,
    size_t size) {
  for (size_t i = 0; i < size; ++i) {
    m_filter_in_buffer[i] = out[i] + aux[i];
  }
  fill(&out[0], &out[size], 0.0f);
  fill(&aux[0], &aux[size], 0.0f);

  vowel *= (kFormantTableSize - 1.001f);
  MAKE_INTEGRAL_FRACTIONAL(vowel);

  for (int32_t i = 0; i < kNumFormants; ++i) {
    float a = formants[vowel_integral][i];
    float b = formants[vowel_integral + 1][i];
    float f = a + (b - a) * vowel_fractional;
    f *= shift;
    m_formant_filter[i].set_f_q<thl::dsp::utils::FrequencyApproximation::Dirty>(f / m_sample_rate, resonance);
    m_formant_filter[i].process<thl::dsp::utils::FilterMode::BandPass>(
        m_filter_in_buffer,
        m_filter_out_buffer,
        size);
    const float pan = i * 0.3f + 0.2f;
    for (size_t j = 0; j < size; ++j) {
      out[j] += m_filter_out_buffer[j] * pan * 0.5f;
      aux[j] += m_filter_out_buffer[j] * (1.0f - pan) * 0.5f;
    }
  }
}

struct ChordNote {
  float note;
  float amplitude;
};

void StringSynthPart::process(
    const PerformanceState& performance_state,
    const Patch& patch,
    const float* in,
    float* out,
    float* aux,
    size_t size) {
  // Assign note to a voice.
  uint8_t envelope_flags[kMaxStringSynthPolyphony];

  fill(&envelope_flags[0], &envelope_flags[m_polyphony], 0);
  m_note_filter.process(performance_state.note, performance_state.strum);
  if (performance_state.strum) {
    m_group[m_active_group].tonic = m_note_filter.stable_note();
    envelope_flags[m_active_group] = ENVELOPE_FLAG_FALLING_EDGE;
    m_active_group = (m_active_group + 1) % m_polyphony;
    envelope_flags[m_active_group] = ENVELOPE_FLAG_RISING_EDGE;
    m_acquisition_delay = 3;
  }
  if (m_acquisition_delay) {
    --m_acquisition_delay;
  } else {
    m_group[m_active_group].tonic = m_note_filter.note();
    m_group[m_active_group].chord = performance_state.chord;
    m_group[m_active_group].structure = patch.structure;
    envelope_flags[m_active_group] |= ENVELOPE_FLAG_GATE;
  }

  // Process envelopes.
  float envelope_values[kMaxStringSynthPolyphony];
  process_envelopes(patch.damping, envelope_flags, envelope_values);

  copy(&in[0], &in[size], &aux[0]);
  copy(&in[0], &in[size], &out[0]);
  int32_t chord_size = min(kStringSynthVoices / m_polyphony, kMaxChordSize);
  for (int32_t group = 0; group < m_polyphony; ++group) {
    ChordNote notes[kMaxChordSize];
    float harmonics[kNumHarmonics * 2];

    compute_registration(
        envelope_values[group] * 0.25f,
        patch.brightness,
        harmonics);

    // Note enough polyphony for smooth transition between chords.
    for (int32_t i = 0; i < chord_size; ++i) {
      float n = chords[m_polyphony - 1][m_group[group].chord][i];
      notes[i].note = n;
      notes[i].amplitude = n >= 0.0f && n <= 17.0f ? 1.0f : 0.7f;
    }

    for (int32_t chord_note = 0; chord_note < chord_size; ++chord_note) {
      float note = 0.0f;
      note += m_group[group].tonic;
      note += performance_state.tonic;
      note += performance_state.fm;
      note += notes[chord_note].note;

      float amplitudes[kNumHarmonics * 2];
      for (int32_t i = 0; i < kNumHarmonics * 2; ++i) {
        amplitudes[i] = notes[chord_note].amplitude * harmonics[i];
      }

      // Fold truncated harmonics.
      size_t num_harmonics = m_polyphony >= 2 && chord_note < 2
          ? kNumHarmonics - 1
          : kNumHarmonics;
      for (int32_t i = num_harmonics; i < kNumHarmonics; ++i) {
        amplitudes[2 * (num_harmonics - 1)] += amplitudes[2 * i];
        amplitudes[2 * (num_harmonics - 1) + 1] += amplitudes[2 * i + 1];
      }

      float frequency = semitones_to_ratio(note - 69.0f) * m_a3;
      m_voice[group * chord_size + chord_note].render(
          frequency,
          amplitudes,
          num_harmonics,
          (group + chord_note) & 1 ? out : aux,
          size);
    }
  }

  if (m_clear_fx) {
    m_reverb.clear();
    m_clear_fx = false;
  }

  switch (m_fx_type) {
    case FX_FORMANT:
    case FX_FORMANT_2:
      process_formant_filter(
          patch.position,
          m_fx_type == FX_FORMANT ? 1.0f : 1.1f,
          m_fx_type == FX_FORMANT ? 25.0f : 10.0f,
          out,
          aux,
          size);
      break;

    case FX_CHORUS:
      m_chorus.set_amount(patch.position);
      m_chorus.set_depth(0.15f + 0.5f * patch.position);
      m_chorus.process(out, aux, size);
      break;

    case FX_ENSEMBLE:
      m_ensemble.set_amount(patch.position * (2.0f - patch.position));
      m_ensemble.set_depth(0.2f + 0.8f * patch.position * patch.position);
      m_ensemble.process(out, aux, size);
      break;

    case FX_REVERB:
    case FX_REVERB_2:
      m_reverb.set_amount(patch.position * 0.5f);
      m_reverb.set_diffusion(0.625f);
      m_reverb.set_time(m_fx_type == FX_REVERB
        ? (0.5f + 0.49f * patch.position)
        : (0.3f + 0.6f * patch.position));
      m_reverb.set_input_gain(0.2f);
      m_reverb.set_lp(m_fx_type == FX_REVERB ? 0.3f : 0.6f);
      m_reverb.process(out, aux, size);
      break;

    default:
      break;
  }

  // Prevent main signal cancellation when EVEN gets summed with ODD through
  // normalization.
  for (size_t i = 0; i < size; ++i) {
    aux[i] = -aux[i];
  }
  m_limiter.process(out, aux, size, 1.0f);
}

}  // namespace thl::dsp::resonator::rings
