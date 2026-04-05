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
// Onset detector.

#pragma once

#include <algorithm>
#include <array>

#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/filter/Svf.h>
#include <tanh/dsp/audio/AudioBufferView.h>

namespace thl::dsp::analysis {

using namespace std;
using namespace thl::dsp::utils;
using namespace thl::dsp::filter;

class ZScorer {
public:
    ZScorer() = default;
    ~ZScorer() = default;

    void prepare(float cutoff) {
        m_coefficient = cutoff;
        m_mean = 0.0f;
        m_variance = 0.0f;
    }

    inline float normalize(float sample) {
        return update(sample) / thl::dsp::utils::sqrt(m_variance);
    }

    inline bool test(float sample, float threshold) {
        const float value = update(sample);
        return value > thl::dsp::utils::sqrt(m_variance) * threshold;
    }

    inline bool test(float sample, float threshold, float absolute_threshold) {
        const float value = update(sample);
        return value > thl::dsp::utils::sqrt(m_variance) * threshold && value > absolute_threshold;
    }

    ZScorer(const ZScorer&) = delete;
    ZScorer& operator=(const ZScorer&) = delete;

private:
    inline float update(float sample) {
        const float centered = sample - m_mean;
        m_mean += m_coefficient * centered;
        m_variance += m_coefficient * (centered * centered - m_variance);
        return centered;
    }

    float m_coefficient = 0.0f;
    float m_mean = 0.0f;
    float m_variance = 0.0f;
};

class Compressor {
public:
    Compressor() = default;
    ~Compressor() = default;

    void prepare(float attack, float decay, float max_gain) {
        m_attack = attack;
        m_decay = decay;
        m_level = 0.0f;
        m_skew = 1.0f / max_gain;
    }

    void process(const float* in, float* out, size_t size) {
        float level = m_level;
        while (size--) {
            thl::dsp::utils::slope<float>(level, fabs(*in), m_attack, m_decay);
            *out++ = *in++ / (m_skew + level);
        }
        m_level = level;
    }

    Compressor(const Compressor&) = delete;
    Compressor& operator=(const Compressor&) = delete;

private:
    float m_attack = 0.0f;
    float m_decay = 0.0f;
    float m_level = 0.0f;
    float m_skew = 0.0f;
};

class OnsetDetector {
public:
    OnsetDetector() = default;
    ~OnsetDetector() = default;

    void prepare(float low, float low_mid, float mid_high, float decimated_sr, float ioi_time) {
        const float ioi_f = 1.0f / (ioi_time * decimated_sr);
        m_compressor.prepare(ioi_f * 10.0f, ioi_f * 0.05f, 40.0f);

        m_low_mid_filter.reset();
        m_mid_high_filter.reset();
        m_low_mid_filter.set_f_q<Approximation::Dirty>(low_mid, 0.5f);
        m_mid_high_filter.set_f_q<Approximation::Dirty>(mid_high, 0.5f);

        m_attack[0] = low_mid;
        m_decay[0] = low * 0.25f;

        m_attack[1] = low_mid;
        m_decay[1] = low * 0.25f;

        m_attack[2] = low_mid;
        m_decay[2] = low * 0.25f;

        m_envelope.fill(0.0f);
        m_energy.fill(0.0f);

        m_z_df.prepare(ioi_f * 0.05f);

        m_inhibit_time = static_cast<int32_t>(ioi_time * decimated_sr);
        m_inhibit_decay = 1.0f / (ioi_time * decimated_sr);

        m_inhibit_threshold = 0.0f;
        m_inhibit_counter = 0;
        m_onset_df = 0.0f;
    }

    bool process(const thl::dsp::audio::ConstAudioBufferView& samples) {
        const float* samples_ptr = samples.get_read_pointer(0);
        const size_t size = samples.get_num_frames();
        // Automatic gain control.
        m_compressor.process(samples_ptr, m_bands[0].data(), size);

        // Quick and dirty filter bank - split the signal in three bands.
        m_mid_high_filter.split(thl::dsp::audio::ConstAudioBufferView(m_bands[0].data(), size),
                                thl::dsp::audio::AudioBufferView(m_bands[1].data(), size),
                                thl::dsp::audio::AudioBufferView(m_bands[2].data(), size));
        m_low_mid_filter.split(thl::dsp::audio::ConstAudioBufferView(m_bands[1].data(), size),
                               thl::dsp::audio::AudioBufferView(m_bands[0].data(), size),
                               thl::dsp::audio::AudioBufferView(m_bands[1].data(), size));

        // Compute low-pass energy and onset detection function
        // (derivative of energy) in each band.
        float onset_df = 0.0f;
        float total_energy = 0.0f;
        for (int32_t i = 0; i < 3; ++i) {
            const float* const s = m_bands[i].data();
            float energy = 0.0f;
            float envelope = m_envelope[i];
            const size_t increment = 4 >> i;
            for (size_t j = 0; j < size; j += increment) {
                thl::dsp::utils::slope<float>(envelope, s[j] * s[j], m_attack[i], m_decay[i]);
                energy += envelope;
            }
            energy = thl::dsp::utils::sqrt(energy) * float(increment);
            m_envelope[i] = envelope;

            const float derivative = energy - m_energy[i];
            onset_df += derivative + fabs(derivative);
            m_energy[i] = energy;
            total_energy += energy;
        }

        m_onset_df += 0.05f * (onset_df - m_onset_df);
        const bool outlier_in_df = m_z_df.test(m_onset_df, 1.0f, 0.01f);
        const bool exceeds_energy_threshold = total_energy >= m_inhibit_threshold;
        const bool not_inhibited = !m_inhibit_counter;
        const bool has_onset = outlier_in_df && exceeds_energy_threshold && not_inhibited;

        if (has_onset) {
            m_inhibit_threshold = total_energy * 1.5f;
            m_inhibit_counter = m_inhibit_time;
        } else {
            m_inhibit_threshold -= m_inhibit_decay * m_inhibit_threshold;
            if (m_inhibit_counter) { --m_inhibit_counter; }
        }
        return has_onset;
    }

    OnsetDetector(const OnsetDetector&) = delete;
    OnsetDetector& operator=(const OnsetDetector&) = delete;

private:
    Compressor m_compressor;
    NaiveSvf m_low_mid_filter;
    NaiveSvf m_mid_high_filter;

    std::array<float, 3> m_attack = {};
    std::array<float, 3> m_decay = {};
    std::array<float, 3> m_energy = {};
    std::array<float, 3> m_envelope = {};
    float m_onset_df = 0.0f;

    std::array<std::array<float, 32>, 3> m_bands = {};

    ZScorer m_z_df;

    float m_inhibit_threshold = 0.0f;
    float m_inhibit_decay = 0.0f;
    int32_t m_inhibit_time = 0;
    int32_t m_inhibit_counter = 0;
};

}  // namespace thl::dsp::analysis
