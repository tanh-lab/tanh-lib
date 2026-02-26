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

#include <tanh/dsp/resonator/rings/Part.h>

#include <tanh/dsp/utils/DspMath.h>

#include <tanh/dsp/resonator/rings/DspFunctions.h>

namespace thl::dsp::resonator::rings {

using namespace std;
using namespace thl::dsp::utils;

void Part::init(uint16_t* reverb_buffer, float sample_rate) {
  WarmDspFunctions();

  m_sample_rate = sample_rate;
  m_a3 = 440.0f / m_sample_rate;
  m_active_voice = 0;

  fill(&m_note[0], &m_note[kMaxPolyphony], 0.0f);

  m_bypass = false;
  m_polyphony = 1;
  m_model = RESONATOR_MODEL_MODAL;
  m_dirty = true;

  for (int32_t i = 0; i < kMaxPolyphony; ++i) {
    m_excitation_filter[i].init();
    m_plucker[i].init();
    m_dc_blocker[i].init(1.0f - 10.0f / m_sample_rate);
  }

  m_reverb.init(reverb_buffer, m_sample_rate);
  m_limiter.init();

  m_note_filter.init(
      m_sample_rate / kMaxBlockSize,
      0.001f,  // Lag time with a sharp edge on the V/Oct input or trigger.
      0.010f,  // Lag time after the trigger has been received.
      0.050f,  // Time to transition from reactive to filtered.
      0.004f); // Prevent a sharp edge to partly leak on the previous voice.
}

void Part::configure_resonators() {
  if (!m_dirty) {
    return;
  }

  switch (m_model) {
    case RESONATOR_MODEL_MODAL:
      {
        int32_t resolution = 64 / m_polyphony - 4;
        for (int32_t i = 0; i < m_polyphony; ++i) {
          m_resonator[i].init(m_sample_rate);
          m_resonator[i].set_resolution(resolution);
        }
      }
      break;

    case RESONATOR_MODEL_SYMPATHETIC_STRING:
    case RESONATOR_MODEL_STRING:
    case RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED:
    case RESONATOR_MODEL_STRING_AND_REVERB:
      {
        float lfo_frequencies[kNumStrings] = {
          0.5f, 0.4f, 0.35f, 0.23f, 0.211f, 0.2f, 0.171f
        };
        for (int32_t i = 0; i < kNumStrings; ++i) {
          bool has_dispersion = m_model == RESONATOR_MODEL_STRING || \
              m_model == RESONATOR_MODEL_STRING_AND_REVERB;
          m_string[i].init(has_dispersion, m_sample_rate);

          float f_lfo = float(kMaxBlockSize) / m_sample_rate;
          f_lfo *= lfo_frequencies[i];
          m_lfo[i].init<thl::dsp::utils::CosineOscillatorMode::Approximate>(f_lfo);
        }
        for (int32_t i = 0; i < m_polyphony; ++i) {
          m_plucker[i].init();
        }
      }
      break;

    case RESONATOR_MODEL_FM_VOICE:
      {
        for (int32_t i = 0; i < m_polyphony; ++i) {
          m_fm_voice[i].init(m_sample_rate);
        }
      }
      break;

    default:
      break;
  }

  if (m_active_voice >= m_polyphony) {
    m_active_voice = 0;
  }
  m_dirty = false;
}

#ifdef BRYAN_CHORDS

// Chord table by Bryan Noll:
float chords[kMaxPolyphony][11][8] = {
  {
    { -12.0f, -0.01f, 0.0f,  0.01f, 0.02f, 11.98f, 11.99f, 12.0f }, // OCT
    { -12.0f, -5.0f,  0.0f,  6.99f, 7.0f,  11.99f, 12.0f,  19.0f }, // 5
    { -12.0f, -5.0f,  0.0f,  5.0f,  7.0f,  11.99f, 12.0f,  17.0f }, // sus4
    { -12.0f, -5.0f,  0.0f,  3.0f,  7.0f,   3.01f, 12.0f,  19.0f }, // m
    { -12.0f, -5.0f,  0.0f,  3.0f,  7.0f,   3.01f, 10.0f,  19.0f }, // m7
    { -12.0f, -5.0f,  0.0f,  3.0f, 14.0f,   3.01f, 10.0f,  19.0f }, // m9
    { -12.0f, -5.0f,  0.0f,  3.0f,  7.0f,   3.01f, 10.0f,  17.0f }, // m11
    { -12.0f, -5.0f,  0.0f,  2.0f,  7.0f,   9.0f,  16.0f,  19.0f }, // 69
    { -12.0f, -5.0f,  0.0f,  4.0f,  7.0f,  11.0f,  14.0f,  19.0f }, // M9
    { -12.0f, -5.0f,  0.0f,  4.0f,  7.0f,  11.0f,  10.99f, 19.0f }, // M7
    { -12.0f, -5.0f,  0.0f,  4.0f,  7.0f,  11.99f, 12.0f,  19.0f } // M
  },
  {
    { -12.0f, 0.0f,  0.01f, 12.0f }, // OCT
    { -12.0f, 6.99f, 7.0f,  12.0f }, // 5
    { -12.0f, 5.0f,  7.0f,  12.0f }, // sus4
    { -12.0f, 3.0f, 11.99f, 12.0f }, // m
    { -12.0f, 3.0f, 10.0f,  12.0f }, // m7
    { -12.0f, 3.0f, 10.0f,  14.0f }, // m9
    { -12.0f, 3.0f, 10.0f,  17.0f }, // m11
    { -12.0f, 2.0f,  9.0f,  16.0f }, // 69
    { -12.0f, 4.0f, 11.0f,  14.0f }, // M9
    { -12.0f, 4.0f,  7.0f,  11.0f }, // M7
    { -12.0f, 4.0f,  7.0f,  12.0f }, // M
  },
  {
    { 0.0f, -12.0f },
    { 0.0f, 2.0f },
    { 0.0f, 3.0f },
    { 0.0f, 4.0f },
    { 0.0f, 5.0f },
    { 0.0f, 7.0f },
    { 0.0f, 9.0f },
    { 0.0f, 10.0f },
    { 0.0f, 11.0f },
    { 0.0f, 12.0f },
    { -12.0f, 12.0f }
  },
  {
    { 0.0f, -12.0f },
    { 0.0f, 2.0f },
    { 0.0f, 3.0f },
    { 0.0f, 4.0f },
    { 0.0f, 5.0f },
    { 0.0f, 7.0f },
    { 0.0f, 9.0f },
    { 0.0f, 10.0f },
    { 0.0f, 11.0f },
    { 0.0f, 12.0f },
    { -12.0f, 12.0f }
  }
};

#else

// Original chord table
float chords[kMaxPolyphony][11][8] = {
  {
    { -12.0f, 0.0f, 0.01f, 0.02f, 0.03f, 11.98f, 11.99f, 12.0f },
    { -12.0f, 0.0f, 3.0f,  3.01f, 7.0f,  9.99f,  10.0f,  19.0f },
    { -12.0f, 0.0f, 3.0f,  3.01f, 7.0f,  11.99f, 12.0f,  19.0f },
    { -12.0f, 0.0f, 3.0f,  3.01f, 7.0f,  13.99f, 14.0f,  19.0f },
    { -12.0f, 0.0f, 3.0f,  3.01f, 7.0f,  16.99f, 17.0f,  19.0f },
    { -12.0f, 0.0f, 6.98f, 6.99f, 7.0f,  12.00f, 18.99f, 19.0f },
    { -12.0f, 0.0f, 3.99f, 4.0f,  7.0f,  16.99f, 17.0f,  19.0f },
    { -12.0f, 0.0f, 3.99f, 4.0f,  7.0f,  13.99f, 14.0f,  19.0f },
    { -12.0f, 0.0f, 3.99f, 4.0f,  7.0f,  11.99f, 12.0f,  19.0f },
    { -12.0f, 0.0f, 3.99f, 4.0f,  7.0f,  10.99f, 11.0f,  19.0f },
    { -12.0f, 0.0f, 4.99f, 5.0f,  7.0f,  11.99f, 12.0f,  17.0f }
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
    { -12.0f, 5.0f, 7.0f,  12.0f },
  },
  {
    { 0.0f, -12.0f },
    { 0.0f, 0.01f },
    { 0.0f, 2.0f },
    { 0.0f, 3.0f },
    { 0.0f, 4.0f },
    { 0.0f, 5.0f },
    { 0.0f, 7.0f },
    { 0.0f, 10.0f },
    { 0.0f, 11.0f },
    { 0.0f, 12.0f },
    { -12.0f, 12.0f }
  },
  {
    { 0.0f, -12.0f },
    { 0.0f, 0.01f },
    { 0.0f, 2.0f },
    { 0.0f, 3.0f },
    { 0.0f, 4.0f },
    { 0.0f, 5.0f },
    { 0.0f, 7.0f },
    { 0.0f, 10.0f },
    { 0.0f, 11.0f },
    { 0.0f, 12.0f },
    { -12.0f, 12.0f }
  }
};

#endif  // BRYAN_CHORDS

void Part::compute_sympathetic_strings_notes(
    float tonic,
    float note,
    float parameter,
    float* destination,
    size_t num_strings) {
  float notes[9] = {
      tonic,
      note - 12.0f,
      note - 7.01955f,
      note,
      note + 7.01955f,
      note + 12.0f,
      note + 19.01955f,
      note + 24.0f,
      note + 24.0f };
  const float detunings[4] = {
      0.013f,
      0.011f,
      0.007f,
      0.017f
  };

  if (parameter >= 2.0f) {
    // Quantized chords
    int32_t chord_index = parameter - 2.0f;
    const float* chord = chords[m_polyphony - 1][chord_index];
    for (size_t i = 0; i < num_strings; ++i) {
      destination[i] = chord[i] + note;
    }
    return;
  }

  size_t num_detuned_strings = (num_strings - 1) >> 1;
  size_t first_detuned_string = num_strings - num_detuned_strings;

  for (size_t i = 0; i < first_detuned_string; ++i) {
    float note = 3.0f;
    if (i != 0) {
      note = parameter * 7.0f;
      parameter += (1.0f - parameter) * 0.2f;
    }

    MAKE_INTEGRAL_FRACTIONAL(note);
    note_fractional = squash(note_fractional);

    float a = notes[note_integral];
    float b = notes[note_integral + 1];

    note = a + (b - a) * note_fractional;
    destination[i] = note;
    if (i + first_detuned_string < num_strings) {
      destination[i + first_detuned_string] = destination[i] + detunings[i & 3];
    }
  }
}

void Part::render_modal_voice(
    int32_t voice,
    const PerformanceState& performance_state,
    const Patch& patch,
    float frequency,
    float filter_cutoff,
    size_t size) {
  // Internal exciter is a pulse, pre-filter.
  if (performance_state.internal_exciter &&
      voice == m_active_voice &&
      performance_state.strum) {
    m_resonator_input[0] += 0.25f * semitones_to_ratio(
        filter_cutoff * filter_cutoff * 24.0f) / filter_cutoff;
  }

  // Process through filter.
  m_excitation_filter[voice].process<thl::dsp::utils::FilterMode::LowPass>(
      m_resonator_input, m_resonator_input, size);

  Resonator& r = m_resonator[voice];
  r.set_frequency(frequency);
  r.set_structure(patch.structure);
  r.set_brightness(patch.brightness * patch.brightness);
  r.set_position(patch.position);
  r.set_damping(patch.damping);
  r.process(m_resonator_input, m_out_buffer, m_aux_buffer, size);
}

void Part::render_fm_voice(
    int32_t voice,
    const PerformanceState& performance_state,
    const Patch& patch,
    float frequency,
    float filter_cutoff,
    size_t size) {
  FMVoice& v = m_fm_voice[voice];
  if (performance_state.internal_exciter &&
      voice == m_active_voice &&
      performance_state.strum) {
    v.trigger_internal_envelope();
  }

  v.set_frequency(frequency);
  v.set_ratio(patch.structure);
  v.set_brightness(patch.brightness);
  v.set_feedback_amount(patch.position);
  v.set_position(/*patch.position*/ 0.0f);
  v.set_damping(patch.damping);
  v.process(m_resonator_input, m_out_buffer, m_aux_buffer, size);
}

void Part::render_string_voice(
    int32_t voice,
    const PerformanceState& performance_state,
    const Patch& patch,
    float frequency,
    float filter_cutoff,
    size_t size) {
  // Compute number of strings and frequency.
  int32_t num_strings = 1;
  float frequencies[kNumStrings];

  if (m_model == RESONATOR_MODEL_SYMPATHETIC_STRING ||
      m_model == RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED) {
    num_strings = 2 * kMaxPolyphony / m_polyphony;
    float parameter = m_model == RESONATOR_MODEL_SYMPATHETIC_STRING
        ? patch.structure
        : 2.0f + performance_state.chord;
    compute_sympathetic_strings_notes(
        performance_state.tonic + performance_state.fm,
        performance_state.tonic + m_note[voice] + performance_state.fm,
        parameter,
        frequencies,
        num_strings);
    for (int32_t i = 0; i < num_strings; ++i) {
      frequencies[i] = semitones_to_ratio(frequencies[i] - 69.0f) * m_a3;
    }
  } else {
    frequencies[0] = frequency;
  }

  if (voice == m_active_voice) {
    const float gain = 1.0f / thl::dsp::utils::sqrt(static_cast<float>(num_strings) * 2.0f);
    for (size_t i = 0; i < size; ++i) {
      m_resonator_input[i] *= gain;
    }
  }

  // Process external input.
  m_excitation_filter[voice].process<thl::dsp::utils::FilterMode::LowPass>(
      m_resonator_input, m_resonator_input, size);

  // Add noise burst.
  if (performance_state.internal_exciter) {
    if (voice == m_active_voice && performance_state.strum) {
      m_plucker[voice].trigger(frequency, filter_cutoff * 8.0f, patch.position);
    }
    m_plucker[voice].process(m_noise_burst_buffer, size);
    for (size_t i = 0; i < size; ++i) {
      m_resonator_input[i] += m_noise_burst_buffer[i];
    }
  }
  m_dc_blocker[voice].process(m_resonator_input, size);

  fill(&m_out_buffer[0], &m_out_buffer[size], 0.0f);
  fill(&m_aux_buffer[0], &m_aux_buffer[size], 0.0f);

  float structure = patch.structure;
  float dispersion = structure < 0.24f
      ? (structure - 0.24f) * 4.166f
      : (structure > 0.26f ? (structure - 0.26f) * 1.35135f : 0.0f);

  for (int32_t string = 0; string < num_strings; ++string) {
    int32_t i = voice + string * m_polyphony;
    String& s = m_string[i];
    float lfo_value = m_lfo[i].next();

    float brightness = patch.brightness;
    float damping = patch.damping;
    float position = patch.position;
    float glide = 1.0f;
    float string_index = static_cast<float>(string) / static_cast<float>(num_strings);
    const float* input = m_resonator_input;

    if (m_model == RESONATOR_MODEL_STRING_AND_REVERB) {
      damping *= (2.0f - damping);
    }

    // When the internal exciter is used, string 0 is the main
    // source, the other strings are vibrating by sympathetic resonance.
    // When the internal exciter is not used, all strings are vibrating
    // by sympathetic resonance.
    if (string > 0 && performance_state.internal_exciter) {
      brightness *= (2.0f - brightness);
      brightness *= (2.0f - brightness);
      damping = 0.7f + patch.damping * 0.27f;
      float amount = (0.5f - fabs(0.5f - patch.position)) * 0.9f;
      position = patch.position + lfo_value * amount;
      glide = semitones_to_ratio((brightness - 1.0f) * 36.0f);
      input = m_sympathetic_resonator_input;
    }

    s.set_dispersion(dispersion);
    s.set_frequency(frequencies[string], glide);
    s.set_brightness(brightness);
    s.set_position(position);
    s.set_damping(damping + string_index * (0.95f - damping));
    s.process(input, m_out_buffer, m_aux_buffer, size);

    if (string == 0) {
      // Was 0.1f, Ben Wilson -> 0.2f
      float gain = 0.2f / static_cast<float>(num_strings);
      for (size_t i = 0; i < size; ++i) {
        float sum = m_out_buffer[i] - m_aux_buffer[i];
        m_sympathetic_resonator_input[i] = gain * sum;
      }
    }
  }
}

const int32_t kPingPattern[] = {
  1, 0, 2, 1, 0, 2, 1, 0
};

void Part::prepare_voice_params(
    const PerformanceState& performance_state,
    const Patch& patch) {
  float cutoff = patch.brightness * (2.0f - patch.brightness);
  float filter_q = performance_state.internal_exciter ? 1.5f : 0.8f;

  for (int32_t voice = 0; voice < m_polyphony; ++voice) {
    float note =
        m_note[voice] + performance_state.tonic + performance_state.fm;
    float frequency = semitones_to_ratio(note - 69.0f) * m_a3;
    float filter_cutoff_range = performance_state.internal_exciter
        ? frequency * semitones_to_ratio((cutoff - 0.5f) * 96.0f)
        : 0.4f * semitones_to_ratio((cutoff - 1.0f) * 108.0f);
    float filter_cutoff = min(voice == m_active_voice
        ? filter_cutoff_range
        : (10.0f / m_sample_rate), 0.499f);

    m_prepared[voice].frequency = frequency;
    m_prepared[voice].filter_cutoff = filter_cutoff;
    m_prepared[voice].filter_q = filter_q;
  }
}

void Part::process(
    const PerformanceState& performance_state,
    const Patch& patch,
    const float* in,
    float* out,
    float* aux,
    size_t size) {

  // Copy inputs to outputs when bypass mode is enabled.
  if (m_bypass) {
    copy(&in[0], &in[size], &out[0]);
    copy(&in[0], &in[size], &aux[0]);
    return;
  }

  configure_resonators();

  m_note_filter.process(
      performance_state.note,
      performance_state.strum);

  if (performance_state.strum) {
    m_note[m_active_voice] = m_note_filter.stable_note();
    if (m_polyphony > 1 && m_polyphony & 1) {
      m_active_voice = kPingPattern[m_step_counter % 8];
      m_step_counter = (m_step_counter + 1) % 8;
    } else {
      m_active_voice = (m_active_voice + 1) % m_polyphony;
    }
  }

  m_note[m_active_voice] = m_note_filter.note();

  prepare_voice_params(performance_state, patch);

  fill(&out[0], &out[size], 0.0f);
  fill(&aux[0], &aux[size], 0.0f);
  for (int32_t voice = 0; voice < m_polyphony; ++voice) {
    float frequency = m_prepared[voice].frequency;
    float filter_cutoff = m_prepared[voice].filter_cutoff;
    float filter_q = m_prepared[voice].filter_q;

    // Process input with excitation filter. Inactive voices receive silence.
    m_excitation_filter[voice].set_f_q<thl::dsp::utils::FrequencyApproximation::Dirty>(filter_cutoff, filter_q);
    if (voice == m_active_voice) {
      copy(&in[0], &in[size], &m_resonator_input[0]);
    } else {
      fill(&m_resonator_input[0], &m_resonator_input[size], 0.0f);
    }

    if (m_model == RESONATOR_MODEL_MODAL) {
      render_modal_voice(
          voice, performance_state, patch, frequency, filter_cutoff, size);
    } else if (m_model == RESONATOR_MODEL_FM_VOICE) {
      render_fm_voice(
          voice, performance_state, patch, frequency, filter_cutoff, size);
    } else {
      render_string_voice(
          voice, performance_state, patch, frequency, filter_cutoff, size);
    }

    if (m_polyphony == 1) {
      // Send the two sets of harmonics / pickups to individual outputs.
      for (size_t i = 0; i < size; ++i) {
        out[i] += m_out_buffer[i];
        aux[i] += m_aux_buffer[i];
      }
    } else {
      // Dispatch odd/even voices to individual outputs.
      float* destination = voice & 1 ? aux : out;
      for (size_t i = 0; i < size; ++i) {
        destination[i] += m_out_buffer[i] - m_aux_buffer[i];
      }
    }
  }

  if (m_model == RESONATOR_MODEL_STRING_AND_REVERB) {
    for (size_t i = 0; i < size; ++i) {
      float l = out[i];
      float r = aux[i];
      out[i] = l * patch.position + (1.0f - patch.position) * r;
      aux[i] = r * patch.position + (1.0f - patch.position) * l;
    }
    m_reverb.set_amount(0.1f + patch.damping * 0.5f);
    m_reverb.set_diffusion(0.625f);
    m_reverb.set_time(0.35f + 0.63f * patch.damping);
    m_reverb.set_input_gain(0.2f);
    m_reverb.set_lp(0.3f + patch.brightness * 0.6f);
    m_reverb.process(out, aux, size);
    for (size_t i = 0; i < size; ++i) {
      aux[i] = -aux[i];
    }
  }

  // Apply limiter to string output.
  m_limiter.process(out, aux, size, m_model_gains[m_model]);
}

/* static */
float Part::m_model_gains[] = {
  1.4f,  // RESONATOR_MODEL_MODAL
  1.0f,  // RESONATOR_MODEL_SYMPATHETIC_STRING
  1.4f,  // RESONATOR_MODEL_STRING
  0.7f,  // RESONATOR_MODEL_FM_VOICE,
  1.0f,  // RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED
  1.4f,  // RESONATOR_MODEL_STRING_AND_REVERB
};

}  // namespace thl::dsp::resonator::rings
