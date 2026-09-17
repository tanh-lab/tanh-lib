#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/DspTypes.h>
#include <tanh/dsp/filter/Svf.h>
#include <tanh/dsp/slicing/SliceMap.h>
#include <tanh/dsp/slicing/TransientSlicer.h>
#include <tanh/dsp/utils/Mixdown.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace thl::dsp::slicing {

namespace {

constexpr size_t k_num_bands = 3;

size_t seconds_to_frames(double seconds, double sample_rate) {
    return static_cast<size_t>(std::max(1.0, std::round(seconds * sample_rate)));
}

/// Short-time energy e[n] = sum of squares over (n - win, n] for n in
/// [begin, end), clamped at frame 0. Returned vector index 0 == frame `begin`.
std::vector<double> short_time_energy(const std::vector<float>& mono,
                                      size_t begin,
                                      size_t end,
                                      size_t win) {
    std::vector<double> energy(end > begin ? end - begin : 0, 0.0);
    if (energy.empty()) { return energy; }
    double sum = 0.0;
    // Prime the window with the frames before `begin`.
    const size_t prime_start = begin >= win ? begin - win + 1 : 0;
    for (size_t k = prime_start; k < begin; ++k) {
        sum += static_cast<double>(mono[k]) * static_cast<double>(mono[k]);
    }
    for (size_t n = begin; n < end; ++n) {
        sum += static_cast<double>(mono[n]) * static_cast<double>(mono[n]);
        if (n >= win) {
            sum -= static_cast<double>(mono[n - win]) * static_cast<double>(mono[n - win]);
        }
        energy[n - begin] = std::max(sum, 0.0);
    }
    return energy;
}

/// Nearest frame to `frame` within +-`radius` (and inside [lo, hi]) where the
/// mono signal is zero or changes sign. Returns `frame` when none is found.
size_t snap_to_zero_crossing(const std::vector<float>& mono,
                             size_t frame,
                             size_t radius,
                             size_t lo,
                             size_t hi) {
    const size_t num_frames = mono.size();
    if (num_frames < 2) { return frame; }
    lo = std::max<size_t>(lo, 1);
    hi = std::min<size_t>(hi, num_frames - 1);
    if (lo > hi) { return frame; }

    auto is_crossing = [&](size_t n) {
        return mono[n] == 0.0f ||
               (static_cast<double>(mono[n - 1]) * static_cast<double>(mono[n]) <= 0.0);
    };

    for (size_t d = 0; d <= radius; ++d) {
        if (frame >= d && frame - d >= lo && frame - d <= hi && is_crossing(frame - d)) {
            return frame - d;
        }
        if (frame + d >= lo && frame + d <= hi && is_crossing(frame + d)) { return frame + d; }
    }
    return frame;
}

/// Refine a coarse peak (start frame of the strongest-flux hop) against the
/// audio: find the sharpest energy rise in and just after that hop, move back
/// up to the move-back time to the lowest-energy frame before it, then snap
/// to a zero crossing. `lo` is the earliest frame the boundary may take.
size_t refine_against_audio(const std::vector<float>& mono,
                            size_t coarse_frame,
                            size_t hop,
                            double sample_rate,
                            size_t lo,
                            size_t hi,
                            const TransientSlicer::Settings& settings) {
    const size_t num_frames = mono.size();
    const size_t win = seconds_to_frames(settings.m_energy_window_seconds, sample_rate);
    const size_t back = seconds_to_frames(settings.m_move_back_seconds, sample_rate);
    const size_t snap = seconds_to_frames(settings.m_zero_cross_seconds, sample_rate);

    // Region: enough before the hop for the move-back, the hop itself and one
    // fine window after it so a rise late in the hop is fully seen.
    const size_t region_lo =
        std::max(lo, coarse_frame > back + win ? coarse_frame - back - win : 0);
    const size_t region_hi = std::min(num_frames, coarse_frame + hop + win);
    if (region_hi <= region_lo) { return std::clamp(coarse_frame, lo, hi); }

    const std::vector<double> energy = short_time_energy(mono, region_lo, region_hi, win);
    auto energy_at = [&](size_t n) { return energy[n - region_lo]; };

    // Sharpest rise: e[n] - e[n - win], n inside the peak hop (plus one window).
    size_t peak = coarse_frame;
    double best_rise = -1.0;
    const size_t rise_lo = std::max(region_lo, coarse_frame);
    for (size_t n = rise_lo; n < region_hi; ++n) {
        const double previous = n >= region_lo + win ? energy_at(n - win) : 0.0;
        const double rise = energy_at(n) - previous;
        if (rise > best_rise) {
            best_rise = rise;
            peak = n;
        }
    }

    // Lowest energy in the `back` frames before the peak; ties keep the
    // latest frame (closest to the onset).
    size_t boundary = peak;
    double lowest = energy_at(peak);
    const size_t search_lo = std::max(region_lo, peak > back ? peak - back : 0);
    for (size_t n = peak; n > search_lo; --n) {
        const double e = energy_at(n - 1);
        if (e < lowest) {
            lowest = e;
            boundary = n - 1;
        }
    }

    boundary = std::clamp(boundary, lo, hi);
    return snap_to_zero_crossing(mono, boundary, snap, lo, hi);
}

}  // namespace

TransientAnalysis TransientSlicer::analyse(const core::BufferF& buffer, double sample_rate) const {
    TransientAnalysis analysis;
    const size_t hop_frames = std::max<size_t>(1, m_settings.m_hop_frames);
    const size_t window_hops = std::max<size_t>(1, m_settings.m_window_hops);
    analysis.m_hop_frames = hop_frames;
    analysis.m_num_frames = buffer.get_num_samples();
    analysis.m_sample_rate = sample_rate;
    if (buffer.empty() || sample_rate <= 0.0) { return analysis; }

    const std::vector<float> mono = utils::mixdown(buffer);
    const size_t num_frames = mono.size();
    const size_t num_hops = (num_frames + hop_frames - 1) / hop_frames;

    // Same "quick and dirty" split OnsetDetector uses: one SVF at the
    // mid/high crossover, one at the low/mid crossover, Q = 0.5.
    filter::NaiveSvf low_mid_filter;
    filter::NaiveSvf mid_high_filter;
    low_mid_filter.reset();
    mid_high_filter.reset();
    low_mid_filter.set_f_q<Approximation::Exact>(
        m_settings.m_low_hz / static_cast<float>(sample_rate),
        0.5f);
    mid_high_filter.set_f_q<Approximation::Exact>(
        m_settings.m_high_hz / static_cast<float>(sample_rate),
        0.5f);

    // Per-hop sum of squares per band, streamed hop by hop.
    std::array<std::vector<double>, k_num_bands> hop_energy;
    for (auto& band : hop_energy) { band.assign(num_hops, 0.0); }

    std::array<std::vector<float>, k_num_bands> bands;
    for (auto& band : bands) { band.assign(hop_frames, 0.0f); }
    std::vector<float> low_mid(hop_frames, 0.0f);
    for (size_t h = 0; h < num_hops; ++h) {
        const size_t start = h * hop_frames;
        const size_t size = std::min(hop_frames, num_frames - start);
        const core::ConstBufferView in(mono.data() + start, size);
        mid_high_filter.split(in,
                              core::BufferView(low_mid.data(), size),
                              core::BufferView(bands[2].data(), size));
        low_mid_filter.split(core::ConstBufferView(low_mid.data(), size),
                             core::BufferView(bands[0].data(), size),
                             core::BufferView(bands[1].data(), size));
        for (size_t b = 0; b < k_num_bands; ++b) {
            double sum = 0.0;
            for (size_t i = 0; i < size; ++i) {
                sum += static_cast<double>(bands[b][i]) * static_cast<double>(bands[b][i]);
            }
            hop_energy[b][h] = sum;
        }
    }

    // Trailing-window RMS -> log compression -> positive first difference,
    // summed over the bands.
    std::vector<double> flux(num_hops, 0.0);
    std::array<double, k_num_bands> previous_log{};
    for (size_t h = 0; h < num_hops; ++h) {
        const size_t first_hop = h + 1 >= window_hops ? h + 1 - window_hops : 0;
        const size_t window_frames =
            std::min((h + 1) * hop_frames, num_frames) - first_hop * hop_frames;
        double sum_flux = 0.0;
        for (size_t b = 0; b < k_num_bands; ++b) {
            double sum = 0.0;
            for (size_t k = first_hop; k <= h; ++k) { sum += hop_energy[b][k]; }
            const double rms =
                window_frames > 0 ? std::sqrt(sum / static_cast<double>(window_frames)) : 0.0;
            const double compressed = std::log1p(m_settings.m_log_gain * rms);
            sum_flux += std::max(0.0, compressed - previous_log[b]);
            previous_log[b] = compressed;
        }
        flux[h] = sum_flux;
    }

    // Subtract a local mean (+- local mean hops), half-wave rectify.
    std::vector<double> prefix(num_hops + 1, 0.0);
    for (size_t h = 0; h < num_hops; ++h) { prefix[h + 1] = prefix[h] + flux[h]; }
    analysis.m_strength.assign(num_hops, 0.0f);
    for (size_t h = 0; h < num_hops; ++h) {
        const size_t lo = h >= m_settings.m_local_mean_hops ? h - m_settings.m_local_mean_hops : 0;
        const size_t hi = std::min(num_hops - 1, h + m_settings.m_local_mean_hops);
        const double mean = (prefix[hi + 1] - prefix[lo]) / static_cast<double>(hi - lo + 1);
        analysis.m_strength[h] = static_cast<float>(std::max(0.0, flux[h] - mean));
    }
    return analysis;
}

SliceMap TransientSlicer::pick(const TransientAnalysis& analysis,
                               size_t count,
                               const core::BufferF* buffer) const {
    const size_t num_frames = analysis.m_num_frames;
    if (num_frames == 0) { return {}; }
    // Every slice needs at least one frame.
    const size_t num_slices = std::min(std::clamp<size_t>(count, 1, k_max_slices), num_frames);
    const auto grid = [&] { return SliceMap::grid(num_slices, num_frames); };
    if (analysis.empty() || analysis.m_sample_rate <= 0.0) { return grid(); }

    const size_t hop = analysis.m_hop_frames == 0 ? std::max<size_t>(1, m_settings.m_hop_frames)
                                                  : analysis.m_hop_frames;
    const double sample_rate = analysis.m_sample_rate;
    const size_t min_frames = seconds_to_frames(m_settings.m_min_slice_seconds, sample_rate);
    if (num_frames < num_slices * min_frames) { return grid(); }

    const size_t num_hops = analysis.m_strength.size();
    const auto& strength = analysis.m_strength;

    // Local maxima of the strength curve within +-min_dist_hops.
    const size_t min_dist_hops = std::max<size_t>(1, (min_frames + hop - 1) / hop);
    std::vector<size_t> candidates;
    for (size_t h = 0; h < num_hops; ++h) {
        const float s = strength[h];
        if (s <= 0.0f) { continue; }
        const size_t lo = h >= min_dist_hops ? h - min_dist_hops : 0;
        const size_t hi = std::min(num_hops - 1, h + min_dist_hops);
        bool is_max = true;
        for (size_t k = lo; k <= hi && is_max; ++k) {
            if (k < h && strength[k] >= s) { is_max = false; }
            if (k > h && strength[k] > s) { is_max = false; }
        }
        if (is_max) { candidates.push_back(h); }
    }
    std::ranges::stable_sort(candidates,
                             [&](size_t a, size_t b) { return strength[a] > strength[b]; });

    // Greedy: strongest first, respecting the minimum distance to accepted
    // peaks and to both ends. One hop of slack covers the audio refinement.
    const size_t needed = num_slices - 1;
    const size_t greedy_dist = min_frames + hop;
    std::vector<size_t> peaks;  // coarse frames (start of the peak hop)
    for (const size_t h : candidates) {
        if (peaks.size() >= needed) { break; }
        const size_t frame = h * hop;
        if (frame < greedy_dist || frame + greedy_dist > num_frames) { continue; }
        const bool clear = std::ranges::all_of(peaks, [&](size_t p) {
            return p > frame ? p - frame >= greedy_dist : frame - p >= greedy_dist;
        });
        if (clear) { peaks.push_back(frame); }
    }
    if (peaks.empty()) { return grid(); }
    std::ranges::sort(peaks);

    // Refine each peak: move back to the lowest energy before the attack and
    // snap to a zero crossing when the audio is available.
    std::vector<float> mono;
    if (buffer != nullptr && buffer->get_num_samples() == num_frames && !buffer->empty()) {
        mono = utils::mixdown(*buffer);
    }
    const size_t snap = seconds_to_frames(m_settings.m_zero_cross_seconds, sample_rate);

    std::vector<size_t> boundaries;
    boundaries.reserve(num_slices + 1);
    boundaries.push_back(0);
    for (const size_t coarse : peaks) {
        const size_t lo = boundaries.back() + min_frames;
        const size_t hi = num_frames - min_frames;
        if (lo > hi) { break; }
        const size_t refined =
            mono.empty() ? std::clamp(coarse, lo, hi)
                         : refine_against_audio(mono, coarse, hop, sample_rate, lo, hi, m_settings);
        boundaries.push_back(refined);
    }
    boundaries.push_back(num_frames);

    // Fewer peaks than needed: hand the missing boundaries to the gaps whose
    // resulting sub-slices stay longest, then split those gaps evenly.
    size_t missing = num_slices + 1 - boundaries.size();
    if (missing > 0) {
        std::vector<size_t> splits(boundaries.size() - 1, 0);
        const size_t min_sub = min_frames + 2 * snap;
        while (missing > 0) {
            size_t best_gap = 0;
            double best_len = 0.0;
            for (size_t g = 0; g < splits.size(); ++g) {
                const double len = static_cast<double>(boundaries[g + 1] - boundaries[g]) /
                                   static_cast<double>(splits[g] + 2);
                if (len > best_len) {
                    best_len = len;
                    best_gap = g;
                }
            }
            if (best_len < static_cast<double>(min_sub)) { break; }
            ++splits[best_gap];
            --missing;
        }

        std::vector<size_t> filled;
        filled.reserve(num_slices + 1);
        for (size_t g = 0; g < splits.size(); ++g) {
            const size_t start = boundaries[g];
            const size_t end = boundaries[g + 1];
            filled.push_back(start);
            const size_t parts = splits[g] + 1;
            for (size_t k = 1; k < parts; ++k) {
                size_t frame = start + (end - start) * k / parts;
                if (!mono.empty()) {
                    frame = snap_to_zero_crossing(mono,
                                                  frame,
                                                  snap,
                                                  start + min_frames,
                                                  end - min_frames);
                }
                filled.push_back(frame);
            }
        }
        filled.push_back(num_frames);
        boundaries = std::move(filled);
    }
    if (boundaries.size() != num_slices + 1) { return grid(); }

    // Safety: strictly ascending with at least min_frames per slice.
    for (size_t i = 1; i + 1 < boundaries.size(); ++i) {
        boundaries[i] = std::max(boundaries[i], boundaries[i - 1] + min_frames);
    }
    for (size_t i = boundaries.size() - 2; i > 0; --i) {
        boundaries[i] = std::min(boundaries[i], boundaries[i + 1] - min_frames);
    }

    boundaries.front() = 0;
    boundaries.back() = num_frames;
    return SliceMap::from_frames(boundaries, num_frames);
}

}  // namespace thl::dsp::slicing
