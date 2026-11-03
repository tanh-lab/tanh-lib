#include <tanh/core/AbiNamespace.h>
#include <tanh/dsp/analysis/SpectrumAnalyser.h>
#include <tanh/dsp/fft/StftAnalyser.h>
#include <tanh/dsp/probe/SampleProbe.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::analysis {

namespace {
constexpr size_t k_read_chunk = 1024;  // read_from(): samples per channel and read
constexpr float k_tilt_pivot_hz = 1000.0f;
constexpr float k_min_power = 1e-30f;  // -300 dB: below any floor
}  // namespace

size_t SpectrumAnalyser::fft_size_of(Resolution resolution) {
    switch (resolution) {
        case Resolution::Low: return 1024;
        case Resolution::Medium: return 2048;
        case Resolution::High: return 4096;
        case Resolution::Maximum: return 8192;
    }
    return 4096;
}

bool SpectrumAnalyser::prepare(double sample_rate, size_t num_channels, const Settings& settings) {
    m_num_frames = 0;
    const bool usable =
        sample_rate > 0.0 && num_channels > 0 && settings.m_num_points >= 2 &&
        settings.m_min_frequency > 0.0f && settings.m_max_frequency > settings.m_min_frequency &&
        settings.m_smoothing_octaves >= 0.0f && settings.m_release_db_per_second >= 0.0f;
    const size_t fft_size = fft_size_of(settings.m_resolution);
    // Four frames a frame length: 75 % overlap, enough for a Hann or Blackman-Harris window.
    if (!usable ||
        !StftAnalyser::prepare(fft_size, fft_size / 4, num_channels, settings.m_window)) {
        m_points.clear();
        m_curve_db.clear();
        m_frequencies.clear();
        return false;
    }
    m_sample_rate = sample_rate;
    m_num_channels = num_channels;
    m_settings = settings;
    m_frame_seconds = static_cast<float>(static_cast<double>(hop_size()) / sample_rate);

    const size_t n = settings.m_num_points;
    m_frequencies.resize(n);
    const double ratio = static_cast<double>(settings.m_max_frequency) / settings.m_min_frequency;
    for (size_t i = 0; i < n; ++i) {
        m_frequencies[i] = static_cast<float>(
            settings.m_min_frequency *
            std::pow(ratio, static_cast<double>(i) / static_cast<double>(n - 1)));
    }
    m_points.assign(n, {});
    m_power.assign(num_bins(), 0.0f);
    m_curve_db.assign(n, settings.m_floor_db);
    m_scratch.assign(num_channels * k_read_chunk, 0.0f);
    m_scratch_pointers.resize(num_channels);
    for (size_t ch = 0; ch < num_channels; ++ch) {
        m_scratch_pointers[ch] = m_scratch.data() + (ch * k_read_chunk);
    }
    update_points();
    return true;
}

bool SpectrumAnalyser::prepare(double sample_rate, size_t num_channels) {
    return prepare(sample_rate, num_channels, Settings{});
}

bool SpectrumAnalyser::set_resolution(Resolution resolution) {
    Settings settings = m_settings;
    settings.m_resolution = resolution;
    return prepare(m_sample_rate, m_num_channels, settings);
}

void SpectrumAnalyser::set_tilt(float db_per_octave) {
    m_settings.m_tilt_db_per_octave = db_per_octave;
    update_points();
}

void SpectrumAnalyser::set_smoothing(float octaves) {
    m_settings.m_smoothing_octaves = std::max(octaves, 0.0f);
    update_points();
}

void SpectrumAnalyser::set_release(float db_per_second) {
    m_settings.m_release_db_per_second = std::max(db_per_second, 0.0f);
}

void SpectrumAnalyser::set_floor(float floor_db) {
    m_settings.m_floor_db = floor_db;
    for (float& value : m_curve_db) { value = std::max(value, floor_db); }
}

void SpectrumAnalyser::update_points() {
    if (m_points.empty()) { return; }
    const size_t n = m_points.size();
    const double bin_hz = m_sample_rate / static_cast<double>(fft_size());
    const size_t last_bin = num_bins() - 1;
    const double smoothing = m_settings.m_smoothing_octaves;
    for (size_t i = 0; i < n; ++i) {
        const double f = m_frequencies[i];
        // The band of the point: its smoothing, or halfway (on the log axis) to its neighbours.
        double low = 0.0;
        double high = 0.0;
        if (smoothing > 0.0) {
            low = f * std::exp2(-0.5 * smoothing);
            high = f * std::exp2(0.5 * smoothing);
        } else {
            const double below = i > 0 ? m_frequencies[i - 1] : f * f / m_frequencies[1];
            const double above = i + 1 < n ? m_frequencies[i + 1] : f * f / m_frequencies[n - 2];
            low = std::sqrt(below * f);
            high = std::sqrt(f * above);
        }
        Point& point = m_points[i];
        point.m_tilt_db =
            static_cast<float>(m_settings.m_tilt_db_per_octave * std::log2(f / k_tilt_pivot_hz));
        const auto first = static_cast<size_t>(std::ceil(low / bin_hz));
        const auto last = static_cast<size_t>(std::floor(high / bin_hz));
        if (first <= last && first <= last_bin) {
            point.m_interpolate = false;
            point.m_first = first;
            point.m_last = std::min(last, last_bin);
        } else {
            // No bin in the band: between the two around the point.
            const double position = std::min(f / bin_hz, static_cast<double>(last_bin));
            point.m_interpolate = true;
            point.m_first = static_cast<size_t>(std::floor(position));
            point.m_last = std::min(point.m_first + 1, last_bin);
            point.m_upper_weight = static_cast<float>(position - std::floor(position));
        }
    }
}

void SpectrumAnalyser::process_spectrum(size_t channel, std::span<const std::complex<float>> bins) {
    if (channel == 0) { std::ranges::fill(m_power, 0.0f); }
    for (size_t k = 0; k < bins.size(); ++k) { m_power[k] += std::norm(bins[k]); }
    if (channel + 1 == m_num_channels) {
        make_curve();
        ++m_num_frames;
    }
}

void SpectrumAnalyser::make_curve() {
    const auto channels = static_cast<float>(m_num_channels);
    const bool mean = m_settings.m_smoothing_octaves > 0.0f;
    const float fall = m_settings.m_release_db_per_second * m_frame_seconds;
    for (size_t i = 0; i < m_points.size(); ++i) {
        const Point& point = m_points[i];
        float power = 0.0f;
        if (point.m_interpolate) {
            power = m_power[point.m_first] +
                    (point.m_upper_weight * (m_power[point.m_last] - m_power[point.m_first]));
        } else if (mean) {
            for (size_t k = point.m_first; k <= point.m_last; ++k) { power += m_power[k]; }
            power /= static_cast<float>(point.m_last - point.m_first + 1);
        } else {
            for (size_t k = point.m_first; k <= point.m_last; ++k) {
                power = std::max(power, m_power[k]);
            }
        }
        power /= channels;
        const float level =
            std::max((10.0f * std::log10(std::max(power, k_min_power))) + point.m_tilt_db,
                     m_settings.m_floor_db);
        float& shown = m_curve_db[i];
        if (level >= shown) {
            shown = level;  // rises at once
        } else if (!m_settings.m_freeze) {
            shown = std::max(level, shown - fall);
        }
    }
}

void SpectrumAnalyser::read_from(thl::dsp::probe::SampleProbe& probe) {
    if (m_scratch_pointers.empty()) { return; }
    const std::span<float* const> destinations(m_scratch_pointers);
    const std::span<const float* const> sources(
        static_cast<const float* const*>(m_scratch_pointers.data()),
        m_scratch_pointers.size());
    for (;;) {
        const auto result = probe.read(destinations, k_read_chunk);
        if (result.m_discontinuity) { reset(); }  // no frame spans the gap
        if (result.m_read == 0) { break; }
        push(sources, result.m_read);
    }
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::analysis
