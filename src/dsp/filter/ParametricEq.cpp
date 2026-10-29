#include <tanh/dsp/filter/ParametricEq.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace thl::dsp::filter {

namespace {

constexpr float k_min_frequency = 10.0f;
constexpr float k_max_frequency_ratio = 0.49f;  // of the sample rate
constexpr float k_min_q = 0.025f;
constexpr float k_max_q = 40.0f;
constexpr float k_max_gain_db = 48.0f;

bool is_flat(const EqBand& band) {
    if (!band.m_enabled) { return true; }
    const bool has_gain = band.m_type == EqBandType::Peak || band.m_type == EqBandType::LowShelf ||
                          band.m_type == EqBandType::HighShelf;
    return has_gain && band.m_gain_db == 0.0f;
}

}  // namespace

ParametricEq::Coefficients ParametricEq::coefficients_of(const EqBand& band, double sample_rate) {
    const double nyquist_limit = k_max_frequency_ratio * sample_rate;
    const double frequency = std::clamp(static_cast<double>(band.m_frequency),
                                        static_cast<double>(k_min_frequency),
                                        nyquist_limit);
    const double q = std::clamp(static_cast<double>(band.m_q),
                                static_cast<double>(k_min_q),
                                static_cast<double>(k_max_q));
    const double gain_db = std::clamp(static_cast<double>(band.m_gain_db),
                                      static_cast<double>(-k_max_gain_db),
                                      static_cast<double>(k_max_gain_db));
    const double g = std::tan(std::numbers::pi * frequency / sample_rate);
    const double a = std::pow(10.0, gain_db / 40.0);  // the square root of the linear gain
    const double k = 1.0 / q;

    // Cytomic, "Linear Trapezoidal Integrated State Variable Filter" (Simper 2013): each type
    // as a mix of the input, the band and the low tap.
    double cg = g;
    double ck = k;
    double m0 = 1.0;
    double m1 = 0.0;
    double m2 = 0.0;
    switch (band.m_type) {
        case EqBandType::Peak:
            ck = 1.0 / (q * a);
            m1 = ck * ((a * a) - 1.0);
            break;
        case EqBandType::Notch: m1 = -k; break;
        case EqBandType::LowShelf:
            cg = g / std::sqrt(a);
            m1 = k * (a - 1.0);
            m2 = (a * a) - 1.0;
            break;
        case EqBandType::HighShelf:
            cg = g * std::sqrt(a);
            m0 = a * a;
            m1 = k * (1.0 - a) * a;
            m2 = 1.0 - (a * a);
            break;
        case EqBandType::LowCut:
            m1 = -k;
            m2 = -1.0;
            break;
        case EqBandType::HighCut:
            m0 = 0.0;
            m2 = 1.0;
            break;
    }
    return {.m_g = static_cast<float>(cg),
            .m_k = static_cast<float>(ck),
            .m_m0 = static_cast<float>(m0),
            .m_m1 = static_cast<float>(m1),
            .m_m2 = static_cast<float>(m2)};
}

void ParametricEq::prepare(double sample_rate, size_t num_channels) {
    m_sample_rate = sample_rate > 0.0 ? sample_rate : 48000.0;
    m_num_channels = num_channels;
    m_ramp_samples =
        std::max<uint32_t>(1,
                           static_cast<uint32_t>(std::lround(k_ramp_ms * 0.001 * m_sample_rate)));
    m_state.assign(k_max_bands * num_channels * 2, 0.0f);
    for (auto& b : m_bands) { b = Band{.m_settings = {.m_enabled = false}}; }
}

void ParametricEq::set_band(size_t index, const EqBand& band) TANH_NONBLOCKING_FUNCTION {
    if (index >= k_max_bands) { return; }
    Band& b = m_bands[index];
    if (b.m_settings == band) { return; }
    b.m_settings = band;
    b.m_flat = is_flat(band);
    if (b.m_flat) {
        // The identity, keeping the filter's tuning so only the mix moves.
        b.m_target = {.m_g = b.m_current.m_g, .m_k = b.m_current.m_k, .m_m0 = 1.0f};
    } else {
        b.m_target = coefficients_of(band, m_sample_rate);
        if (b.m_current.m_g == 0.0f) {  // from flat with no tuning yet: tune at once
            b.m_current.m_g = b.m_target.m_g;
            b.m_current.m_k = b.m_target.m_k;
        }
    }
    const auto n = static_cast<float>(m_ramp_samples);
    b.m_step = {.m_g = (b.m_target.m_g - b.m_current.m_g) / n,
                .m_k = (b.m_target.m_k - b.m_current.m_k) / n,
                .m_m0 = (b.m_target.m_m0 - b.m_current.m_m0) / n,
                .m_m1 = (b.m_target.m_m1 - b.m_current.m_m1) / n,
                .m_m2 = (b.m_target.m_m2 - b.m_current.m_m2) / n};
    b.m_ramp_remaining = m_ramp_samples;
}

void ParametricEq::reset() TANH_NONBLOCKING_FUNCTION {
    std::ranges::fill(m_state, 0.0f);
    for (auto& b : m_bands) {
        b.m_current = b.m_target;
        b.m_ramp_remaining = 0;
    }
}

void ParametricEq::process(float* const* channels,
                           size_t num_channels,
                           size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    const size_t n = std::min(num_channels, m_num_channels);
    for (size_t i = 0; i < k_max_bands; ++i) {
        const Band& b = m_bands[i];
        if (b.m_flat && b.m_ramp_remaining == 0) { continue; }
        process_band(i, channels, n, num_samples);
        if (b.m_flat && b.m_ramp_remaining == 0) {  // just arrived at flat: start clean later
            std::fill_n(m_state.begin() + static_cast<std::ptrdiff_t>(i * m_num_channels * 2),
                        m_num_channels * 2,
                        0.0f);
        }
    }
}

void ParametricEq::process_band(size_t index,
                                float* const* channels,
                                size_t num_channels,
                                size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    Band& b = m_bands[index];
    float* state = m_state.data() + (index * m_num_channels * 2);
    size_t s = 0;

    // The ramp: coefficients per sample, shared by the channels.
    for (; s < num_samples && b.m_ramp_remaining > 0; ++s) {
        Coefficients& c = b.m_current;
        if (--b.m_ramp_remaining == 0) {
            c = b.m_target;
        } else {
            c.m_g += b.m_step.m_g;
            c.m_k += b.m_step.m_k;
            c.m_m0 += b.m_step.m_m0;
            c.m_m1 += b.m_step.m_m1;
            c.m_m2 += b.m_step.m_m2;
        }
        const float a1 = 1.0f / (1.0f + (c.m_g * (c.m_g + c.m_k)));
        const float a2 = c.m_g * a1;
        const float a3 = c.m_g * a2;
        for (size_t ch = 0; ch < num_channels; ++ch) {
            float& ic1 = state[ch * 2];
            float& ic2 = state[(ch * 2) + 1];
            const float v0 = channels[ch][s];
            const float v3 = v0 - ic2;
            const float v1 = (a1 * ic1) + (a2 * v3);
            const float v2 = ic2 + (a2 * ic1) + (a3 * v3);
            ic1 = (2.0f * v1) - ic1;
            ic2 = (2.0f * v2) - ic2;
            channels[ch][s] = (c.m_m0 * v0) + (c.m_m1 * v1) + (c.m_m2 * v2);
        }
    }
    if (s == num_samples || b.m_flat) { return; }

    // Settled: fixed coefficients, one channel at a time.
    const Coefficients& c = b.m_current;
    const float a1 = 1.0f / (1.0f + (c.m_g * (c.m_g + c.m_k)));
    const float a2 = c.m_g * a1;
    const float a3 = c.m_g * a2;
    for (size_t ch = 0; ch < num_channels; ++ch) {
        float ic1 = state[ch * 2];
        float ic2 = state[(ch * 2) + 1];
        float* x = channels[ch];
        for (size_t i = s; i < num_samples; ++i) {
            const float v0 = x[i];
            const float v3 = v0 - ic2;
            const float v1 = (a1 * ic1) + (a2 * v3);
            const float v2 = ic2 + (a2 * ic1) + (a3 * v3);
            ic1 = (2.0f * v1) - ic1;
            ic2 = (2.0f * v2) - ic2;
            x[i] = (c.m_m0 * v0) + (c.m_m1 * v1) + (c.m_m2 * v2);
        }
        state[ch * 2] = ic1;
        state[(ch * 2) + 1] = ic2;
    }
}

float ParametricEq::magnitude_db(const EqBand& band, double frequency, double sample_rate) {
    if (is_flat(band) || !(sample_rate > 0.0)) { return 0.0f; }
    const Coefficients c = coefficients_of(band, sample_rate);
    // The bilinear transform maps the frequency onto the prototype's s = j tan(pi f / fs) / g;
    // the taps are band = s / D and low = 1 / D, D = s^2 + k s + 1.
    const double f = std::clamp(frequency, 0.0, 0.5 * sample_rate);
    const std::complex<double> s(0.0,
                                 std::tan(std::numbers::pi * std::min(f / sample_rate, 0.4999)) /
                                     static_cast<double>(c.m_g));
    const std::complex<double> d = (s * s) + (static_cast<double>(c.m_k) * s) + 1.0;
    const std::complex<double> h = static_cast<double>(c.m_m0) +
                                   ((static_cast<double>(c.m_m1) * s) / d) +
                                   (static_cast<double>(c.m_m2) / d);
    return static_cast<float>(20.0 * std::log10(std::max(std::abs(h), 1e-12)));
}

float ParametricEq::response_db(std::span<const EqBand> bands,
                                double frequency,
                                double sample_rate) {
    float total = 0.0f;
    for (const auto& band : bands) { total += magnitude_db(band, frequency, sample_rate); }
    return total;
}

}  // namespace thl::dsp::filter
