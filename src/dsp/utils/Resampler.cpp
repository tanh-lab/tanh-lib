// SPDX-License-Identifier: Apache-2.0
#include <tanh/core/AbiNamespace.h>
#include <tanh/dsp/utils/Resampler.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::utils {

namespace {

constexpr double k_zero_crossings = 24.0;
constexpr double k_cutoff = 0.475;  // x the lower rate: 95 % of its Nyquist band passes
constexpr std::size_t k_phases = 256;

double blackman(double x) {  // x in [-1, 1]
    const double t = (x + 1.0) / 2.0;
    return 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * t) +
           0.08 * std::cos(4.0 * std::numbers::pi * t);
}

double sinc(double x) {
    return x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
}

}  // namespace

void Resampler::prepare(double input_rate, double output_rate, std::size_t max_block) {
    if (input_rate <= 0.0 || output_rate <= 0.0) {
        throw std::invalid_argument("Resampler: sample rates must be positive");
    }
    m_passthrough = std::abs(input_rate - output_rate) < 1e-9;
    m_step = input_rate / output_rate;
    m_position = 0.0;
    if (m_passthrough) {
        m_history.clear();
        m_table.clear();
        m_history_size = 0;
        return;
    }

    // Cut-off in cycles per input sample; the kernel widens as it narrows.
    const double cutoff = k_cutoff * std::min(input_rate, output_rate) / input_rate;
    m_half_width = k_zero_crossings / (2.0 * cutoff);
    m_taps = static_cast<std::size_t>(std::ceil(2.0 * m_half_width)) + 1;
    m_phases = k_phases;

    // Row p holds the kernel sampled at (tap - offset) - p / phases, tap = 0 .. taps-1.
    const double offset = std::floor(m_half_width);
    m_table.assign((m_phases + 1) * m_taps, 0.0f);
    for (std::size_t p = 0; p <= m_phases; ++p) {
        const double fraction = static_cast<double>(p) / static_cast<double>(m_phases);
        for (std::size_t tap = 0; tap < m_taps; ++tap) {
            const double t = static_cast<double>(tap) - offset - fraction;
            if (std::abs(t) >= m_half_width) { continue; }
            m_table[p * m_taps + tap] = static_cast<float>(2.0 * cutoff * sinc(2.0 * cutoff * t) *
                                                           blackman(t / m_half_width));
        }
    }

    m_history.assign(max_block + m_taps + 4, 0.0f);
    reset();
}

std::size_t Resampler::max_output(std::size_t input) const noexcept {
    return m_passthrough
               ? input
               : static_cast<std::size_t>(std::ceil(static_cast<double>(input) / m_step)) + 2;
}

void Resampler::reset() noexcept {
    std::ranges::fill(m_history, 0.0f);
    // Start with half a kernel of silence so the first output sample is centred on input 0.
    m_history_size = m_passthrough ? 0 : static_cast<std::size_t>(std::floor(m_half_width));
    m_position = static_cast<double>(m_history_size);
}

float Resampler::sample_at(double position) const noexcept {
    const auto base = static_cast<std::size_t>(position);
    const double fraction = (position - static_cast<double>(base)) * static_cast<double>(m_phases);
    const auto phase = static_cast<std::size_t>(fraction);
    const auto blend = static_cast<float>(fraction - static_cast<double>(phase));
    const float* row_a = m_table.data() + phase * m_taps;
    const float* row_b = row_a + m_taps;
    const float* in = m_history.data() + base - static_cast<std::size_t>(std::floor(m_half_width));
    float a = 0.0f;
    float b = 0.0f;
    for (std::size_t tap = 0; tap < m_taps; ++tap) {
        a += row_a[tap] * in[tap];
        b += row_b[tap] * in[tap];
    }
    return a + (b - a) * blend;
}

std::size_t Resampler::process(std::span<const float> in,
                               std::span<float> out) noexcept TANH_NONBLOCKING_FUNCTION {
    if (m_passthrough) {
        const auto count = std::min(in.size(), out.size());
        std::copy_n(in.begin(), count, out.begin());
        return count;
    }
    const auto room = m_history.size() - m_history_size;
    const auto accepted = std::min(room, in.size());
    std::copy_n(in.begin(),
                accepted,
                m_history.begin() + static_cast<std::ptrdiff_t>(m_history_size));
    m_history_size += accepted;

    // An output sample needs the kernel's right half: taps - 1 - floor(half) samples ahead.
    const double lookahead = static_cast<double>(m_taps - 1) - std::floor(m_half_width);
    std::size_t written = 0;
    while (written < out.size() &&
           m_position + lookahead + 1.0 <= static_cast<double>(m_history_size)) {
        out[written++] = sample_at(m_position);
        m_position += m_step;
    }

    // Keep what the next outputs still need: from floor(position) - floor(half).
    const auto keep_from =
        static_cast<std::size_t>(std::max(0.0, std::floor(m_position) - std::floor(m_half_width)));
    if (keep_from > 0) {
        std::copy(m_history.begin() + static_cast<std::ptrdiff_t>(keep_from),
                  m_history.begin() + static_cast<std::ptrdiff_t>(m_history_size),
                  m_history.begin());
        m_history_size -= keep_from;
        m_position -= static_cast<double>(keep_from);
    }
    return written;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::utils
