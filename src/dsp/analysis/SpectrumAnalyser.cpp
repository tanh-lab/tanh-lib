#include <tanh/core/AbiNamespace.h>
#include <tanh/dsp/analysis/SpectrumAnalyser.h>
#include <tanh/dsp/fft/StftAnalyser.h>
#include <tanh/dsp/probe/SampleProbe.h>
#include <tanh/dsp/utils/Decibels.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::analysis {

namespace {
constexpr size_t k_read_chunk = 1024;  // read_from(): samples per channel and read
constexpr float k_tilt_pivot_hz = 1000.0f;
constexpr float k_min_power_db = -300.0f;  // below any floor, before the tilt
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

void SpectrumAnalyser::clear_all() {
    clear();
    m_points.clear();
    m_curve_db.clear();
    m_frequencies.clear();
    m_power.clear();
    m_scratch.clear();
    m_scratch_pointers.clear();
    m_num_frames = 0;
}

bool SpectrumAnalyser::prepare(double sample_rate, size_t num_channels, const Settings& settings) {
    m_num_frames = 0;
    Settings used = settings;
    // Above Nyquist there is no bin to show.
    if (sample_rate > 0.0) {
        used.m_max_frequency =
            std::min(used.m_max_frequency, static_cast<float>(0.5 * sample_rate));
    }
    const bool usable = sample_rate > 0.0 && num_channels > 0 && used.m_num_points >= 2 &&
                        used.m_min_frequency > 0.0f &&
                        used.m_max_frequency > used.m_min_frequency &&
                        used.m_smoothing_octaves >= 0.0f && used.m_release_db_per_second >= 0.0f;
    const size_t fft_size = fft_size_of(used.m_resolution);
    // Four frames a frame length: 75 % overlap, enough for a Hann or Blackman-Harris window.
    if (!usable || !StftAnalyser::prepare(fft_size, fft_size / 4, num_channels, used.m_window)) {
        clear_all();  // the frames stop too: nothing of an earlier prepare() goes on
        return false;
    }
    m_sample_rate = sample_rate;
    m_settings = used;
    m_frame_seconds = static_cast<float>(static_cast<double>(hop_size()) / sample_rate);

    const size_t n = used.m_num_points;
    m_frequencies.resize(n);
    const double ratio = static_cast<double>(used.m_max_frequency) / used.m_min_frequency;
    for (size_t i = 0; i < n; ++i) {
        m_frequencies[i] = static_cast<float>(
            used.m_min_frequency *
            std::pow(ratio, static_cast<double>(i) / static_cast<double>(n - 1)));
    }
    m_points.assign(n, {});
    m_power.assign(num_bins(), 0.0f);
    m_curve_db.assign(n, used.m_floor_db);
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
    if (num_channels() == 0) { return false; }  // unprepared: nothing to re-prepare
    Settings settings = m_settings;
    settings.m_resolution = resolution;
    return prepare(m_sample_rate, num_channels(), settings);
}

void SpectrumAnalyser::set_tilt(float db_per_octave) {
    // The held curve moves by the change at once (frozen, it would never catch up otherwise).
    const float change = db_per_octave - m_settings.m_tilt_db_per_octave;
    m_settings.m_tilt_db_per_octave = db_per_octave;
    for (size_t i = 0; i < m_curve_db.size(); ++i) {
        if (m_curve_db[i] <= m_settings.m_floor_db) { continue; }  // below the range: stays there
        const float shift = change * std::log2(m_frequencies[i] / k_tilt_pivot_hz);
        m_curve_db[i] = std::max(m_curve_db[i] + shift, m_settings.m_floor_db);
    }
    update_points();
}

void SpectrumAnalyser::set_smoothing(float octaves) {
    m_settings.m_smoothing_octaves = std::max(octaves, 0.0f);
    std::ranges::fill(m_curve_db, m_settings.m_floor_db);  // max and mean do not convert
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
        // Halfway to the neighbours at least, so a band narrower than the points' spacing leaves
        // no bin to no point.
        const double below = i > 0 ? m_frequencies[i - 1] : f * f / m_frequencies[1];
        const double above = i + 1 < n ? m_frequencies[i + 1] : f * f / m_frequencies[n - 2];
        double low = std::sqrt(below * f);
        double high = std::sqrt(f * above);
        if (smoothing > 0.0) {
            low = std::min(low, f * std::exp2(-0.5 * smoothing));
            high = std::max(high, f * std::exp2(0.5 * smoothing));
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

void SpectrumAnalyser::process_spectrum(size_t channel, std::span<const std::complex<float>> bins)
    TANH_NONBLOCKING_FUNCTION {
    if (channel == 0) { std::ranges::fill(m_power, 0.0f); }
    for (size_t k = 0; k < bins.size(); ++k) { m_power[k] += std::norm(bins[k]); }
    if (channel + 1 == num_channels()) {
        make_curve();
        ++m_num_frames;
    }
}

void SpectrumAnalyser::make_curve() TANH_NONBLOCKING_FUNCTION {
    const auto channels = static_cast<float>(num_channels());
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
        const float level = std::max(utils::power_to_db(power, k_min_power_db) + point.m_tilt_db,
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
    // Another channel count would read low (a channel missing) or drop channels: read nothing.
    const bool fits = probe.num_channels() == num_channels();
    assert(fits && "SpectrumAnalyser::read_from(): the probe's channel count differs");
    if (!fits) { return; }
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
