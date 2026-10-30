#include <tanh/core/BufferView.h>
#include <tanh/dsp/filter/EqDesign.h>
#include <tanh/dsp/filter/ParametricEq.h>
#include <tanh/dsp/filter/SvfCoefficients.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace thl::dsp::filter {

namespace {

// What the filter is made of: the band in range, and the fields its type does not use (the gain
// of a cut, everything of a disabled band) at their defaults, so changing them is no change.
EqBand used_settings(const EqBand& band, double sample_rate) TANH_NONBLOCKING_FUNCTION {
    EqBand b = sanitised(band, sample_rate);
    if (!b.m_enabled) { return EqBand{.m_enabled = false}; }
    if (b.m_type != EqBandType::Peak && b.m_type != EqBandType::LowShelf &&
        b.m_type != EqBandType::HighShelf) {
        b.m_gain_db = 0.0f;
    }
    return b;
}

}  // namespace

void ParametricEq::prepare(double sample_rate, size_t num_channels) {
    m_sample_rate = sample_rate > 0.0 ? sample_rate : 48000.0;
    m_num_channels = num_channels;
    m_state.assign(k_max_bands * num_channels, SvfState{});
    const auto ramp =
        std::max<uint32_t>(1,
                           static_cast<uint32_t>(std::lround(k_ramp_ms * 0.001 * m_sample_rate)));
    for (auto& b : m_bands) {
        b.m_ramp.set_ramp_samples(ramp);
        retarget(b, false);
    }
}

void ParametricEq::retarget(Band& band, bool ramp) TANH_NONBLOCKING_FUNCTION {
    SvfCoefficients target = design_band(band.m_settings, m_design, m_sample_rate);
    band.m_flat = target.is_identity();
    const SvfCoefficients& current = band.m_ramp.current();
    if (band.m_flat) {
        // The identity, keeping the filter's tuning so only the mix moves.
        target.m_g = current.m_g;
        target.m_k = current.m_k;
    }
    if (!ramp || (band.m_flat && current.is_identity() && !band.m_ramp.ramping())) {
        band.m_ramp.set_current(target);  // a flat band that stays flat: nothing to ramp
        return;
    }
    if (current.is_identity() && !band.m_flat) {
        // From a silent band: tune at once, then only the mix ramps.
        SvfCoefficients tuned = SvfCoefficients::identity();
        tuned.m_g = target.m_g;
        tuned.m_k = target.m_k;
        band.m_ramp.set_current(tuned);
    }
    band.m_ramp.set_target(target);
}

void ParametricEq::set_band(size_t index, const EqBand& band) TANH_NONBLOCKING_FUNCTION {
    if (index >= k_max_bands) { return; }
    Band& b = m_bands[index];
    // Compared as used: a setting out of range (or NaN) the same way twice, or a field the band
    // does not use, is no change.
    const bool same =
        used_settings(b.m_settings, m_sample_rate) == used_settings(band, m_sample_rate);
    b.m_settings = band;
    if (!same) { retarget(b, true); }
}

void ParametricEq::set_design(EqDesign design) TANH_NONBLOCKING_FUNCTION {
    if (design == m_design) { return; }
    m_design = design;
    for (auto& b : m_bands) { retarget(b, true); }
}

void ParametricEq::reset() TANH_NONBLOCKING_FUNCTION {
    for (auto& s : m_state) { s.reset(); }
    for (auto& b : m_bands) { b.m_ramp.finish(); }
}

void ParametricEq::process(float* const* channels,
                           size_t num_channels,
                           size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    const size_t n = std::min(num_channels, m_num_channels);
    for (size_t i = 0; i < k_max_bands; ++i) {
        const Band& b = m_bands[i];
        if (b.m_flat && !b.m_ramp.ramping()) { continue; }
        process_band(i, channels, n, num_samples);
        if (b.m_flat && !b.m_ramp.ramping()) {  // just arrived at flat: start clean later
            for (size_t ch = 0; ch < m_num_channels; ++ch) {
                m_state[(i * m_num_channels) + ch].reset();
            }
        }
    }
}

void ParametricEq::process_band(size_t index,
                                float* const* channels,
                                size_t num_channels,
                                size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    Band& b = m_bands[index];
    SvfState* state = m_state.data() + (index * m_num_channels);
    size_t s = 0;

    // The ramp: coefficients per sample, shared by the channels.
    for (; s < num_samples && b.m_ramp.ramping(); ++s) {
        b.m_ramp.advance();
        const SvfTick tick(b.m_ramp.current());
        for (size_t ch = 0; ch < num_channels; ++ch) {
            channels[ch][s] = tick.process(state[ch], channels[ch][s]);
        }
    }
    if (s == num_samples || b.m_flat) { return; }

    // Settled: fixed coefficients, one channel at a time.
    const SvfTick tick(b.m_ramp.current());
    for (size_t ch = 0; ch < num_channels; ++ch) {
        SvfState st = state[ch];
        float* x = channels[ch];
        for (size_t i = s; i < num_samples; ++i) { x[i] = tick.process(st, x[i]); }
        state[ch] = st;
    }
}

float ParametricEq::magnitude_db(const EqBand& band,
                                 EqDesign design,
                                 double frequency,
                                 double sample_rate) {
    if (!(sample_rate > 0.0)) { return 0.0f; }
    return static_cast<float>(
        design_band(band, design, sample_rate).magnitude_db(frequency, sample_rate));
}

float ParametricEq::response_db(std::span<const EqBand> bands,
                                EqDesign design,
                                double frequency,
                                double sample_rate) {
    float total = 0.0f;
    for (const auto& band : bands) { total += magnitude_db(band, design, frequency, sample_rate); }
    return total;
}

void ParametricEq::response_curve(std::span<const EqBand> bands,
                                  EqDesign design,
                                  double sample_rate,
                                  std::span<const float> frequencies,
                                  std::span<float> out_db) {
    const size_t n = std::min(frequencies.size(), out_db.size());
    std::fill_n(out_db.begin(), n, 0.0f);
    if (!(sample_rate > 0.0)) { return; }
    for (const auto& band : bands) {
        const SvfCoefficients c = design_band(band, design, sample_rate);
        if (c.is_identity()) { continue; }
        for (size_t i = 0; i < n; ++i) {
            out_db[i] += static_cast<float>(c.magnitude_db(frequencies[i], sample_rate));
        }
    }
}

void ParametricEq::log_frequencies(std::span<float> out, float lowest, float highest) {
    if (out.empty()) { return; }
    if (out.size() == 1) {
        out[0] = lowest;
        return;
    }
    const double lo = std::log(static_cast<double>(lowest));
    const double step =
        (std::log(static_cast<double>(highest)) - lo) / static_cast<double>(out.size() - 1);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(std::exp(lo + (step * static_cast<double>(i))));
    }
    out.back() = highest;
}

// --- ParametricEqImpl ---------------------------------------------------------------------------

ParametricEqImpl::ParametricEqImpl(size_t num_bands)
    : m_num_bands(std::clamp<size_t>(num_bands, 1, ParametricEq::k_max_bands)) {}

void ParametricEqImpl::prepare(const double& sample_rate,
                               const size_t& /*samples_per_block*/,
                               const size_t& num_channels) {
    m_eq.prepare(sample_rate, num_channels);
    read_parameters(0);
    m_eq.reset();  // the first block starts at the parameters, not ramping to them
}

void ParametricEqImpl::read_parameters(uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    const auto index = [](float value, int last) TANH_NONBLOCKING_FUNCTION {
        return std::isfinite(value) ? std::clamp(static_cast<int>(std::lround(value)), 0, last) : 0;
    };
    m_eq.set_design(static_cast<EqDesign>(index(get_parameter_float(Design, modulation_offset),
                                                static_cast<int>(EqDesign::Matched))));
    for (size_t b = 0; b < m_num_bands; ++b) {
        const auto read = [&](BandParameter p) TANH_NONBLOCKING_FUNCTION {
            return get_band_parameter_float(p, b, modulation_offset);
        };
        m_eq.set_band(b,
                      {.m_type = static_cast<EqBandType>(
                           index(read(Type), static_cast<int>(EqBandType::HighCut))),
                       .m_frequency = read(Frequency),
                       .m_gain_db = read(Gain),
                       .m_q = read(Q),
                       .m_enabled = read(Enabled) > 0.5f});
    }
}

void ParametricEqImpl::process(thl::core::BufferView buffer,
                               uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    constexpr size_t k_max_channels = 16;
    const size_t num_channels = std::min(buffer.get_num_channels(), k_max_channels);
    std::array<float*, k_max_channels> channels{};
    for (size_t ch = 0; ch < num_channels; ++ch) { channels[ch] = buffer.get_write_pointer(ch); }
    read_parameters(modulation_offset);
    m_eq.process(channels.data(), num_channels, buffer.get_num_samples());
}

}  // namespace thl::dsp::filter
