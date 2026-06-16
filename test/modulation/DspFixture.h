// Shared helpers for DSP processor click/regression tests. Each per-processor
// test file defines its own wrapper (subclass of the processor + SmartHandle
// wiring) and its own LFO setup, then uses these utilities to drive the
// processor through the modulation matrix and assert on the output.
//
// Universal properties asserted by the standard tests:
//   1. Silence in → silence out under static (non-default) parameter values
//      [for effects only; sources/synths have their own variant].
//   2. No per-sample clicks under knob-rate parameter modulation, silent input.
//   3. No per-sample clicks under knob-rate parameter modulation, signal input.
//   4. Finite output (no NaN/Inf) — folded into the above via expect_all_finite.

#pragma once

#include <gtest/gtest.h>
#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/modulation/ModulationMatrix.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "TestHelpers.h"

namespace dsp_fixture {

inline float max_abs(const std::vector<float>& v) {
    float m = 0.0f;
    for (float s : v) { m = std::max(m, std::abs(s)); }
    return m;
}

inline float max_sample_step(const std::vector<float>& v) {
    float m = 0.0f;
    for (size_t i = 1; i < v.size(); ++i) { m = std::max(m, std::abs(v[i] - v[i - 1])); }
    return m;
}

inline void expect_all_finite(const std::vector<float>& v) {
    for (float s : v) { ASSERT_TRUE(std::isfinite(s)) << "non-finite sample"; }
}

// Mono driver. Runs N blocks of input through a processor while advancing
// the modulation matrix once per block. Returns the (mutated) output flat.
template <typename Processor>
std::vector<float> run_blocks_mono(thl::modulation::ModulationMatrix& matrix,
                                   Processor& proc,
                                   const std::vector<float>& input) {
    std::vector<float> output;
    output.reserve(input.size());
    const size_t num_blocks = input.size() / k_block_size;
    std::vector<float> block(k_block_size);
    for (size_t b = 0; b < num_blocks; ++b) {
        std::copy(input.begin() + static_cast<ptrdiff_t>(b * k_block_size),
                  input.begin() + static_cast<ptrdiff_t>((b + 1) * k_block_size),
                  block.begin());
        matrix.process(k_block_size);
        thl::dsp::audio::AudioBufferView view(block.data(), k_block_size);
        proc.process_modulated(view);
        output.insert(output.end(), block.begin(), block.end());
    }
    return output;
}

// Stereo driver. Runs N blocks of mono `input` duplicated onto two channels
// through a 2-channel processor. Returns each channel's output flat.
template <typename Processor>
std::pair<std::vector<float>, std::vector<float>> run_blocks_stereo(
    thl::modulation::ModulationMatrix& matrix,
    Processor& proc,
    const std::vector<float>& input_mono) {
    std::vector<float> out_l, out_r;
    out_l.reserve(input_mono.size());
    out_r.reserve(input_mono.size());

    const size_t num_blocks = input_mono.size() / k_block_size;
    std::vector<float> l(k_block_size), r(k_block_size);
    std::array<float*, 2> channels{l.data(), r.data()};

    for (size_t b = 0; b < num_blocks; ++b) {
        for (size_t i = 0; i < k_block_size; ++i) {
            const float sample = input_mono[b * k_block_size + i];
            l[i] = sample;
            r[i] = sample;
        }
        matrix.process(k_block_size);
        thl::dsp::audio::AudioBufferView view(channels.data(), 2, k_block_size);
        proc.process_modulated(view);
        out_l.insert(out_l.end(), l.begin(), l.end());
        out_r.insert(out_r.end(), r.begin(), r.end());
    }
    return {std::move(out_l), std::move(out_r)};
}

// 220 Hz sine, 0.5 amplitude — the standard signal input for click tests.
inline std::vector<float> make_test_sine(size_t num_blocks,
                                         float frequency_hz = 220.0f,
                                         float amplitude = 0.5f) {
    constexpr float k_two_pi = 6.28318530718f;
    std::vector<float> v(num_blocks * k_block_size);
    for (size_t i = 0; i < v.size(); ++i) {
        v[i] = amplitude * std::sin(k_two_pi * frequency_hz * static_cast<float>(i) /
                                    static_cast<float>(k_sample_rate));
    }
    return v;
}

// "Knob-like" LFO frequencies: slow enough that the 5 ms smoothing window
// fully resolves between transitions, but each parameter steps independently.
// These are intentionally co-prime-ish so the four+ LFOs don't align.
inline constexpr float k_knob_lfo_freqs[] = {5.0f,
                                             7.0f,
                                             11.0f,
                                             13.0f,
                                             17.0f,
                                             19.0f,
                                             23.0f,
                                             29.0f,
                                             31.0f,
                                             37.0f,
                                             41.0f,
                                             43.0f,
                                             47.0f,
                                             53.0f,
                                             59.0f};

}  // namespace dsp_fixture
