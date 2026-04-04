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

#include <tanh/dsp/rings-resonator/RingsStringSynthPart.h>

#include <tanh/dsp/rings-resonator/RingsDsp.h>

#include <array>

namespace thl::dsp::synth {

using namespace std;
using namespace thl::dsp::utils;

void RingsStringSynthPart::prepare(uint16_t* reverb_buffer, float sample_rate) {
    m_sample_rate = sample_rate;
    m_a3 = 440.0f / m_sample_rate;
    m_active_group = 0;
    m_acquisition_delay = 0;

    m_polyphony = 1;
    m_fx_type = FxType::Ensemble;

    for (auto& voice : m_voice) { voice.prepare(); }

    for (auto& group : m_group) {
        group.m_tonic = 0.0f;
        group.m_envelope.prepare();
    }

    for (auto& filter : m_formant_filter) { filter.reset(); }

    m_limiter.prepare();

    m_reverb.prepare(reverb_buffer, m_sample_rate);
    m_chorus.prepare(reverb_buffer, m_sample_rate);
    m_ensemble.prepare(reverb_buffer, m_sample_rate);

    m_note_filter.prepare(m_sample_rate / thl::dsp::resonator::k_max_block_size,
                          0.001f,   // Lag time with a sharp edge on the V/Oct
                                    // input or trigger.
                          0.005f,   // Lag time after the trigger has been
                                    // received.
                          0.050f,   // Time to transition from reactive to
                                    // filtered.
                          0.004f);  // Prevent a sharp edge to partly leak on
                                    // the previous voice.
}

constexpr size_t k_num_harmonic_pairs = static_cast<size_t>(k_num_harmonics) * 2;

const int32_t k_registration_table_size = 11;
const std::array<std::array<float, k_num_harmonic_pairs>, k_registration_table_size>
    k_registrations = {{
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 0.1f, 0.0f, 0.0f, 1.0f, 0.0f},
        {1.0f, 0.5f, 1.0f, 0.0f, 1.0f, 0.0f},
        {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f},
        {0.0f, 0.5f, 1.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    }};

void RingsStringSynthPart::compute_registration(float gain, float registration, float* amplitudes) {
    registration *= (k_registration_table_size - 1.001f);
    auto [registration_integral, registration_fractional] = split_integral_fractional(registration);
    float total = 0.0f;
    for (size_t i = 0; i < k_num_harmonic_pairs; ++i) {
        float a = k_registrations[registration_integral][i];
        float b = k_registrations[registration_integral + 1][i];
        amplitudes[i] = a + (b - a) * registration_fractional;
        total += amplitudes[i];
    }
    for (size_t i = 0; i < k_num_harmonic_pairs; ++i) {
        amplitudes[i] = gain * amplitudes[i] / total;
    }
}

#ifdef BRYAN_CHORDS

// Chord table by Bryan Noll:
// - more compact, leaving room for a bass
// - more frequent note changes between adjacent chords.
// - dropped fifth.
const std::array<std::array<std::array<float, k_max_chord_size>, thl::dsp::resonator::k_num_chords>,
                 k_max_string_synth_polyphony>
    k_chords = {{
        {{
            {-12.0f, -0.01f, 0.0f, 0.01f, 0.02f, 11.99f, 12.0f, 24.0f},  // OCT
            {-12.0f, -5.01f, -5.0f, 0.0f, 7.0f, 12.0f, 19.0f, 24.0f},    // 5
            {-12.0f, -5.0f, 0.0f, 5.0f, 7.0f, 12.0f, 17.0f, 24.0f},      // sus4
            {-12.0f, -5.0f, 0.0f, 0.01f, 3.0f, 12.0f, 19.0f, 24.0f},     // m
            {-12.0f, -5.01f, -5.0f, 0.0f, 3.0f, 10.0f, 19.0f, 24.0f},    // m7
            {-12.0f, -5.0f, 0.0f, 3.0f, 10.0f, 14.0f, 19.0f, 24.0f},     // m9
            {-12.0f, -5.01f, -5.0f, 0.0f, 3.0f, 10.0f, 17.0f, 24.0f},    // m11
            {-12.0f, -5.0f, 0.0f, 2.0f, 9.0f, 16.0f, 19.0f, 24.0f},      // 69
            {-12.0f, -5.0f, 0.0f, 4.0f, 11.0f, 14.0f, 19.0f, 24.0f},     // M9
            {-12.0f, -5.0f, 0.0f, 4.0f, 7.0f, 11.0f, 19.0f, 24.0f},      // M7
            {-12.0f, -5.0f, 0.0f, 4.0f, 7.0f, 12.0f, 19.0f, 24.0f},      // M
        }},
        {{
            {-12.0f, -0.01f, 0.0f, 0.01f, 12.0f, 12.01f},  // OCT
            {-12.0f, -5.01f, -5.0f, 0.0f, 7.0f, 12.0f},    // 5
            {-12.0f, -5.0f, 0.0f, 5.0f, 7.0f, 12.0f},      // sus4
            {-12.0f, -5.0f, 0.0f, 0.01f, 3.0f, 12.0f},     // m
            {-12.0f, -5.01f, -5.0f, 0.0f, 3.0f, 10.0f},    // m7
            {-12.0f, -5.0f, 0.0f, 3.0f, 10.0f, 14.0f},     // m9
            {-12.0f, -5.0f, 0.0f, 3.0f, 10.0f, 17.0f},     // m11
            {-12.0f, -5.0f, 0.0f, 2.0f, 9.0f, 16.0f},      // 69
            {-12.0f, -5.0f, 0.0f, 4.0f, 11.0f, 14.0f},     // M9
            {-12.0f, -5.0f, 0.0f, 4.0f, 7.0f, 11.0f},      // M7
            {-12.0f, -5.0f, 0.0f, 4.0f, 7.0f, 12.0f},      // M
        }},
        {{
            {-12.0f, 0.0f, 0.01f, 12.0f},   // OCT
            {-12.0f, 6.99f, 7.0f, 12.0f},   // 5
            {-12.0f, 5.0f, 7.0f, 12.0f},    // sus4
            {-12.0f, 3.0f, 11.99f, 12.0f},  // m
            {-12.0f, 3.0f, 9.99f, 10.0f},   // m7
            {-12.0f, 3.0f, 10.0f, 14.0f},   // m9
            {-12.0f, 3.0f, 10.0f, 17.0f},   // m11
            {-12.0f, 2.0f, 9.0f, 16.0f},    // 69
            {-12.0f, 4.0f, 11.0f, 14.0f},   // M9
            {-12.0f, 4.0f, 7.0f, 11.0f},    // M7
            {-12.0f, 4.0f, 7.0f, 12.0f},    // M
        }},
        {{
            {0.0f, 0.01f, 12.0f},  // OCT
            {0.0f, 7.0f, 12.0f},   // 5
            {5.0f, 7.0f, 12.0f},   // sus4
            {0.0f, 3.0f, 12.0f},   // m
            {0.0f, 3.0f, 10.0f},   // m7
            {3.0f, 10.0f, 14.0f},  // m9
            {3.0f, 10.0f, 17.0f},  // m11
            {2.0f, 9.0f, 16.0f},   // 69
            {4.0f, 11.0f, 14.0f},  // M9
            {4.0f, 7.0f, 11.0f},   // M7
            {4.0f, 7.0f, 12.0f},   // M
        }},
    }};

#else

// Original chord table:
// - wider, occupies more room in the spectrum.
// - minimum number of note changes between adjacent chords.
// - consistant with the chord table used for the sympathetic strings model.
const std::array<std::array<std::array<float, k_max_chord_size>, thl::dsp::resonator::k_num_chords>,
                 k_max_string_synth_polyphony>
    chords = {{
        {{
            {-24.0f, -12.0f, 0.0f, 0.01f, 0.02f, 11.99f, 12.0f, 24.0f},
            {-24.0f, -12.0f, 0.0f, 3.0f, 7.0f, 10.0f, 19.0f, 24.0f},
            {-24.0f, -12.0f, 0.0f, 3.0f, 7.0f, 12.0f, 19.0f, 24.0f},
            {-24.0f, -12.0f, 0.0f, 3.0f, 7.0f, 14.0f, 19.0f, 24.0f},
            {-24.0f, -12.0f, 0.0f, 3.0f, 7.0f, 17.0f, 19.0f, 24.0f},
            {-24.0f, -12.0f, 0.0f, 6.99f, 7.0f, 18.99f, 19.0f, 24.0f},
            {-24.0f, -12.0f, 0.0f, 4.0f, 7.0f, 17.0f, 19.0f, 24.0f},
            {-24.0f, -12.0f, 0.0f, 4.0f, 7.0f, 14.0f, 19.0f, 24.0f},
            {-24.0f, -12.0f, 0.0f, 4.0f, 7.0f, 12.0f, 19.0f, 24.0f},
            {-24.0f, -12.0f, 0.0f, 4.0f, 7.0f, 11.0f, 19.0f, 24.0f},
            {-24.0f, -12.0f, 0.0f, 5.0f, 7.0f, 12.0f, 17.0f, 24.0f},
        }},
        {{
            {-24.0f, -12.0f, 0.0f, 0.01f, 12.0f, 12.01f},
            {-24.0f, -12.0f, 0.0f, 3.00f, 7.0f, 10.0f},
            {-24.0f, -12.0f, 0.0f, 3.00f, 7.0f, 12.0f},
            {-24.0f, -12.0f, 0.0f, 3.00f, 7.0f, 14.0f},
            {-24.0f, -12.0f, 0.0f, 3.00f, 7.0f, 17.0f},
            {-24.0f, -12.0f, 0.0f, 6.99f, 12.0f, 19.0f},
            {-24.0f, -12.0f, 0.0f, 4.00f, 7.0f, 17.0f},
            {-24.0f, -12.0f, 0.0f, 4.00f, 7.0f, 14.0f},
            {-24.0f, -12.0f, 0.0f, 4.00f, 7.0f, 12.0f},
            {-24.0f, -12.0f, 0.0f, 4.00f, 7.0f, 11.0f},
            {-24.0f, -12.0f, 0.0f, 5.00f, 7.0f, 12.0f},
        }},
        {{
            {-12.0f, 0.0f, 0.01f, 12.0f},
            {-12.0f, 3.0f, 7.0f, 10.0f},
            {-12.0f, 3.0f, 7.0f, 12.0f},
            {-12.0f, 3.0f, 7.0f, 14.0f},
            {-12.0f, 3.0f, 7.0f, 17.0f},
            {-12.0f, 7.0f, 12.0f, 19.0f},
            {-12.0f, 4.0f, 7.0f, 17.0f},
            {-12.0f, 4.0f, 7.0f, 14.0f},
            {-12.0f, 4.0f, 7.0f, 12.0f},
            {-12.0f, 4.0f, 7.0f, 11.0f},
            {-12.0f, 5.0f, 7.0f, 12.0f},
        }},
        {{
            {0.0f, 0.01f, 12.0f},
            {0.0f, 3.0f, 10.0f},
            {0.0f, 3.0f, 7.0f},
            {0.0f, 3.0f, 14.0f},
            {0.0f, 3.0f, 17.0f},
            {0.0f, 7.0f, 19.0f},
            {0.0f, 4.0f, 17.0f},
            {0.0f, 4.0f, 14.0f},
            {0.0f, 4.0f, 7.0f},
            {0.0f, 4.0f, 11.0f},
            {0.0f, 5.0f, 7.0f},
        }},
    }};

#endif  // BRYAN_CHORDS

void RingsStringSynthPart::process_envelopes(float shape, uint8_t* flags, float* values) {
    float decay = shape;
    float attack = 0.0f;
    if (shape < 0.5f) {
        attack = 0.0f;
    } else {
        attack = (shape - 0.5f) * 2.0f;
    }

    // Convert the arbitrary values to actual units.
    float period = m_sample_rate / thl::dsp::resonator::k_max_block_size;
    float attack_time = semitones_to_ratio(attack * 96.0f) * 0.005f * period;
    // float decay_time = semitones_to_ratio(decay * 96.0f) * 0.125f * period;
    float decay_time = semitones_to_ratio(decay * 84.0f) * 0.180f * period;
    float attack_rate = 1.0f / attack_time;
    float decay_rate = 1.0f / decay_time;

    for (int32_t i = 0; i < m_polyphony; ++i) {
        float drone = shape < 0.98f ? 0.0f : (shape - 0.98f) * 55.0f;
        if (drone >= 1.0f) { drone = 1.0f; }

        m_group[i].m_envelope.set_ad(attack_rate, decay_rate);
        float value = m_group[i].m_envelope.process(flags[i]);
        values[i] = value + (1.0f - value) * drone;
    }
}

const int32_t k_formant_table_size = 5;
const std::array<std::array<float, k_num_formants>, k_formant_table_size> k_formants = {{
    {700, 1100, 2400},
    {500, 1300, 1700},
    {400, 2000, 2500},
    {600, 800, 2400},
    {300, 900, 2200},
}};

void RingsStringSynthPart::process_formant_filter(float vowel,
                                                  float shift,
                                                  float resonance,
                                                  thl::dsp::audio::AudioBufferView out,
                                                  thl::dsp::audio::AudioBufferView aux) {
    float* out_ptr = out.get_write_pointer(0);
    float* aux_ptr = aux.get_write_pointer(0);
    size_t size = out.get_num_frames();

    for (size_t i = 0; i < size; ++i) { m_filter_in_buffer[i] = out_ptr[i] + aux_ptr[i]; }
    fill(&out_ptr[0], &out_ptr[size], 0.0f);
    fill(&aux_ptr[0], &aux_ptr[size], 0.0f);

    vowel *= (k_formant_table_size - 1.001f);
    auto [vowel_integral, vowel_fractional] = split_integral_fractional(vowel);

    for (int32_t i = 0; i < k_num_formants; ++i) {
        float a = k_formants[vowel_integral][i];
        float b = k_formants[vowel_integral + 1][i];
        float f = a + (b - a) * vowel_fractional;
        f *= shift;
        m_formant_filter[i].set_f_q<Approximation::Dirty>(f / m_sample_rate, resonance);
        thl::dsp::audio::ConstAudioBufferView filter_in(m_filter_in_buffer, size);
        thl::dsp::audio::AudioBufferView filter_out(m_filter_out_buffer, size);
        m_formant_filter[i].process<thl::dsp::filter::FilterMode::BandPass>(filter_in, filter_out);
        const float pan = static_cast<float>(i) * 0.3f + 0.2f;
        for (size_t j = 0; j < size; ++j) {
            out_ptr[j] += m_filter_out_buffer[j] * pan * 0.5f;
            aux_ptr[j] += m_filter_out_buffer[j] * (1.0f - pan) * 0.5f;
        }
    }
}

struct ChordNote {
    float m_note;
    float m_amplitude;
};

void RingsStringSynthPart::process(
    const thl::dsp::resonator::RingsPerformanceState& performance_state,
    const thl::dsp::resonator::RingsPatch& patch,
    const thl::dsp::audio::ConstAudioBufferView& in,
    thl::dsp::audio::AudioBufferView out,
    thl::dsp::audio::AudioBufferView aux) {
    const float* in_ptr = in.get_read_pointer(0);
    float* out_ptr = out.get_write_pointer(0);
    float* aux_ptr = aux.get_write_pointer(0);
    size_t size = in.get_num_frames();

    // Assign note to a voice.
    std::array<uint8_t, k_max_string_synth_polyphony> envelope_flags{};

    fill(envelope_flags.begin(), envelope_flags.begin() + m_polyphony, 0);
    m_note_filter.process(performance_state.m_note, performance_state.m_strum);
    if (performance_state.m_strum) {
        m_group[m_active_group].m_tonic = m_note_filter.stable_note();
        envelope_flags[m_active_group] = FallingEdge;
        m_active_group = (m_active_group + 1) % m_polyphony;
        envelope_flags[m_active_group] = RisingEdge;
        m_acquisition_delay = 3;
    }
    if (m_acquisition_delay) {
        --m_acquisition_delay;
    } else {
        m_group[m_active_group].m_tonic = m_note_filter.note();
        m_group[m_active_group].m_chord = performance_state.m_chord;
        m_group[m_active_group].m_structure = patch.m_structure;
        envelope_flags[m_active_group] |= Gate;
    }

    // Process envelopes.
    std::array<float, k_max_string_synth_polyphony> envelope_values{};
    process_envelopes(patch.m_damping, envelope_flags.data(), envelope_values.data());

    copy(&in_ptr[0], &in_ptr[size], &aux_ptr[0]);
    copy(&in_ptr[0], &in_ptr[size], &out_ptr[0]);
    int32_t chord_size = min(k_string_synth_voices / m_polyphony, k_max_chord_size);
    for (int32_t group = 0; group < m_polyphony; ++group) {
        std::array<ChordNote, k_max_chord_size> notes{};
        std::array<float, k_num_harmonic_pairs> harmonics{};

        compute_registration(envelope_values[group] * 0.25f, patch.m_brightness, harmonics.data());

        // Note enough polyphony for smooth transition between chords.
        for (int32_t i = 0; i < chord_size; ++i) {
            float n = k_chords[m_polyphony - 1][m_group[group].m_chord][i];
            notes[i].m_note = n;
            notes[i].m_amplitude = n >= 0.0f && n <= 17.0f ? 1.0f : 0.7f;
        }

        for (int32_t chord_note = 0; chord_note < chord_size; ++chord_note) {
            float note = 0.0f;
            note += m_group[group].m_tonic;
            note += performance_state.m_tonic;
            note += performance_state.m_fm;
            note += notes[chord_note].m_note;

            std::array<float, k_num_harmonic_pairs> amplitudes{};
            for (size_t i = 0; i < k_num_harmonic_pairs; ++i) {
                amplitudes[i] = notes[chord_note].m_amplitude * harmonics[i];
            }

            // Fold truncated harmonics.
            size_t num_harmonics =
                m_polyphony >= 2 && chord_note < 2 ? k_num_harmonics - 1 : k_num_harmonics;
            for (auto i = static_cast<int32_t>(num_harmonics); i < k_num_harmonics; ++i) {
                amplitudes[2 * (num_harmonics - 1)] += amplitudes[static_cast<size_t>(i) * 2];
                amplitudes[2 * (num_harmonics - 1) + 1] +=
                    amplitudes[static_cast<size_t>(i) * 2 + 1];
            }

            float frequency = semitones_to_ratio(note - 69.0f) * m_a3;
            m_voice[group * chord_size + chord_note].render(
                frequency,
                amplitudes.data(),
                num_harmonics,
                (group + chord_note) & 1 ? out_ptr : aux_ptr,
                size);
        }
    }

    if (m_clear_fx) {
        m_reverb.clear();
        m_clear_fx = false;
    }

    std::array<float*, 2> stereo_ptrs = {out_ptr, aux_ptr};
    thl::dsp::audio::AudioBufferView stereo_view(stereo_ptrs.data(), 2, size);

    switch (m_fx_type) {
        case FxType::Formant:
        case FxType::Formant2:
            process_formant_filter(patch.m_position,
                                   m_fx_type == FxType::Formant ? 1.0f : 1.1f,
                                   m_fx_type == FxType::Formant ? 25.0f : 10.0f,
                                   out,
                                   aux);
            break;

        case FxType::Chorus:
            m_chorus.set_amount(patch.m_position);
            m_chorus.set_depth(0.15f + 0.5f * patch.m_position);
            m_chorus.process(stereo_view);
            break;

        case FxType::Ensemble:
            m_ensemble.set_amount(patch.m_position * (2.0f - patch.m_position));
            m_ensemble.set_depth(0.2f + 0.8f * patch.m_position * patch.m_position);
            m_ensemble.process(stereo_view);
            break;

        case FxType::Reverb:
        case FxType::Reverb2:
            m_reverb.set_amount(patch.m_position * 0.5f);
            m_reverb.set_diffusion(0.625f);
            m_reverb.set_time(m_fx_type == FxType::Reverb ? (0.5f + 0.49f * patch.m_position)
                                                          : (0.3f + 0.6f * patch.m_position));
            m_reverb.set_input_gain(0.2f);
            m_reverb.set_lp(m_fx_type == FxType::Reverb ? 0.3f : 0.6f);
            m_reverb.process(stereo_view);
            break;

        default: break;
    }

    // Prevent main signal cancellation when EVEN gets summed with ODD through
    // normalization.
    for (size_t i = 0; i < size; ++i) { aux_ptr[i] = -aux_ptr[i]; }
    m_limiter.process(stereo_view, 1.0f);
}

}  // namespace thl::dsp::synth
